/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Default build: Gate A, one synthetic packed frame, no hardware access.
 * Separate native target: explicit crystalhddec -> YUY2 -> Kodi-owned buffer. */
#include <kodi/addon-instance/Inputstream.h>
#include <kodi/addon-instance/VideoCodec.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <sys/stat.h>
#include "packed.h"
#ifdef CRYSTALHD_KODI_NATIVE
#include "native.h"
#ifdef CRYSTALHD_KODI_PLAYBACK
#include "playback.h"
#endif
#include "input-progress.h"
#include <gst/video/video.h>
#ifdef CRYSTALHD_KODI_RGB_TEST
#include "../../tests/kodi-rgb-observer.h"
#endif
#endif
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
}

namespace {
constexpr const char* marker = "CrystalHD-GateA";
constexpr size_t max_packet = crystalhd_kodi::packet_limit;

#ifdef CRYSTALHD_KODI_NATIVE
bool source_color(const std::string& path, const AVCodecParameters& params,
                   crystalhd_kodi::ColorHints& color) {
  color.matrix = params.color_space; color.primaries = params.color_primaries;
  color.transfer = params.color_trc; color.range = params.color_range;
  /* Inspect one AU in a separate demux context. The actual input's first
   * video packet and negative-PTS AAC priming packet must remain untouched.
   * Only h264parse runs: there is no software or hardware video decoder. */
  AVFormatContext* probe = nullptr;
  if (avformat_open_input(&probe, path.c_str(), av_find_input_format("mov"), nullptr) < 0) return false;
  const int stream = av_find_best_stream(probe, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  AVPacket* packet = av_packet_alloc(); bool ok = false; size_t bytes = 0;
  for (unsigned n = 0; packet && stream >= 0 && n < 64 && av_read_frame(probe, packet) >= 0; ++n) {
    if (packet->size <= 0 || static_cast<size_t>(packet->size) > max_packet - bytes) break;
    bytes += packet->size;
    if (packet->stream_index == stream) {
      ok = crystalhd_kodi::InspectAvcColor(params.extradata, params.extradata_size,
                                          packet->data, packet->size, color);
      break;
    }
    av_packet_unref(packet);
  }
  av_packet_free(&packet); avformat_close_input(&probe);
  return ok;
}

bool copy_color(const crystalhd_kodi::ColorHints& c, kodi::addon::InputstreamInfo& info) {
  /* Both public APIs use ISO/IEC 23001-8 values. Admit only the prototype's
   * SDR set, never silently replace an HDR/unsupported descriptor with 709.
   * Static assertions keep this distinct from private pixel-enum coercion. */
  static_assert(INPUTSTREAM_COLORPRIMARY_BT709 == 1 && INPUTSTREAM_COLORPRIMARY_BT470BG == 5 &&
                INPUTSTREAM_COLORPRIMARY_SMPTE170M == 6 && INPUTSTREAM_COLORPRIMARY_UNSPECIFIED == 2);
  static_assert(INPUTSTREAM_COLORTRC_BT709 == 1 && INPUTSTREAM_COLORTRC_UNSPECIFIED == 2);
  auto member = [](unsigned value, std::initializer_list<unsigned> list) {
    return std::find(list.begin(), list.end(), value) != list.end();
  };
  if (!member(c.matrix, {1,2,5,6,7}) || !member(c.primaries, {1,2,5,6,7}) ||
      !member(c.transfer, {1,2,4,5,6,7,13}) || c.range > 1) return false;
  info.SetColorSpace(static_cast<INPUTSTREAM_COLORSPACE>(c.matrix));
  info.SetColorPrimaries(static_cast<INPUTSTREAM_COLORPRIMARIES>(c.primaries));
  info.SetColorTransferCharacteristic(static_cast<INPUTSTREAM_COLORTRC>(c.transfer));
  info.SetColorRange(static_cast<INPUTSTREAM_COLORRANGE>(c.range));
  return true;
}
#endif

class ProbeCodec : public kodi::addon::CInstanceVideoCodec {
  unsigned width = 0, height = 0;
  bool pending = false, emitted = false;
  int64_t pts = 0;
public:
  explicit ProbeCodec(const kodi::addon::IInstanceInfo& info) : CInstanceVideoCodec(info) {
    kodi::Log(ADDON_LOG_INFO, "%s VideoCodec instance created; NO hardware decode", marker);
  }
  bool Open(const kodi::addon::VideoCodecInitdata& data) override {
    width = data.GetWidth(); height = data.GetHeight();
    if (data.GetCodecType() != VIDEOCODEC_H264 || !width || (width & 1) ||
        !height || width > 1920 || height > 1088) return false;
    const auto formats = data.GetVideoFormats();
    bool advertised = std::find(formats.begin(), formats.end(), VIDEOCODEC_FORMAT_YUYV422) != formats.end();
    /* RC1's host still advertises only YV12. This is an explicitly labelled
     * format-admission experiment: GetFrameBuffer/GetPicture have packed-format
     * API mappings. Do not call this negotiated or production support. */
    kodi::Log(ADDON_LOG_INFO, "%s Open H264 %ux%u YUY2 advertised=%d; explicit synthetic-format probe",
              marker, width, height, advertised);
    return true;
  }
  const char* GetName() override { return "CrystalHD Gate A: synthetic YUY2, NO hardware decode"; }
  bool AddData(const DEMUX_PACKET& packet) override {
    if (!emitted && !pending && packet.iSize > 0) {
      if (!std::isfinite(packet.pts) || packet.pts < 0 ||
          packet.pts > static_cast<double>(std::numeric_limits<int64_t>::max() / 2)) return false;
      pts = static_cast<int64_t>(packet.pts); pending = true;
    }
    return true;
  }
  VIDEOCODEC_RETVAL GetPicture(VIDEOCODEC_PICTURE& picture) override {
    if (!pending) return (picture.flags & VIDEOCODEC_PICTURE_FLAG_DRAIN) ? VC_EOF : VC_BUFFER;
    picture.videoFormat = VIDEOCODEC_FORMAT_YUYV422;
    picture.width = width; picture.height = height;
    picture.stride[0] = width * 2;
    picture.decodedDataSize = static_cast<size_t>(width) * height * 2;
    if (!GetFrameBuffer(picture)) return VC_ERROR;
    const size_t offset = picture.planeOffsets[0], pitch = picture.stride[0];
    const size_t size = picture.decodedDataSize;
    if (!picture.videoBufferHandle || !picture.decodedData ||
        !crystalhd_kodi::packed_layout(width, height, offset, pitch, size)) {
      if (picture.videoBufferHandle) ReleaseFrameBuffer(picture.videoBufferHandle);
      picture.videoBufferHandle = nullptr;
      return VC_ERROR;
    }
    for (unsigned y = 0; y < height; ++y) {
      uint8_t* row = picture.decodedData + offset + y * pitch;
      for (unsigned x = 0; x < width; x += 2) {
        row[x * 2] = 16 + (200 * x / width);
        row[x * 2 + 1] = y < height / 2 ? 64 : 192;
        row[x * 2 + 2] = 16 + (200 * (x + 1) / width);
        row[x * 2 + 3] = y < height / 2 ? 192 : 64;
      }
    }
    picture.pts = pts; pending = false; emitted = true;
    kodi::Log(ADDON_LOG_INFO, "%s VC_PICTURE synthetic YUY2 %ux%u stride=%u bytes=%zu; Kodi-owned buffer",
              marker, width, height, picture.stride[0], picture.decodedDataSize);
    return VC_PICTURE;
  }
  void Reset() override { pending = false; emitted = false; }
};

#ifdef CRYSTALHD_KODI_NATIVE
#ifdef CRYSTALHD_KODI_PLAYBACK
using PlaybackBackend = crystalhd_kodi::PlaybackDecoder;
#else
using PlaybackBackend = crystalhd_kodi::NativeDecoder;
#endif
class NativeCodec : public kodi::addon::CInstanceVideoCodec {
  PlaybackBackend decoder;
  std::shared_ptr<crystalhd_kodi::InputProgress> progress;
  uint64_t input_epoch = 0;
  unsigned width = 0, height = 0, fps_n, fps_d;
  std::vector<uint8_t> extra;
  bool active = false, fatal = false, eof_logged = false;
  unsigned accepted = 0, pictures = 0, generation = 0;
  int64_t last_pts = -1;
#ifdef CRYSTALHD_KODI_TRACE
  GChecksum* checksum = nullptr;
#endif
  bool Start() {
    accepted = pictures = 0; last_pts = -1; eof_logged = false; fatal = false;
    input_epoch = progress->epoch.load();
#ifdef CRYSTALHD_KODI_TRACE
    if (checksum) g_checksum_free(checksum);
    checksum = g_checksum_new(G_CHECKSUM_SHA256);
#endif
    active = decoder.Open(width, height, fps_n, fps_d, extra.data(), extra.size());
    if (!active) kodi::Log(ADDON_LOG_ERROR, "CrystalHD-Native open failed: %s; no internal software fallback",
                           decoder.Error().c_str());
    else {
      gchar* version = gst_version_string();
      kodi::Log(ADDON_LOG_INFO, "CrystalHD-Native session=%u H264 %ux%u crystalhddec/YUY2; %s; two host copies",
                ++generation, width, height, version);
      g_free(version);
#ifdef CRYSTALHD_KODI_PLAYBACK
      kodi::Log(ADDON_LOG_INFO, "CrystalHD-Playback open generation=%llu; private input-idle facade, no dummy or V4L2 STOP",
                static_cast<unsigned long long>(decoder.Counts().generation));
#endif
    }
    return active;
  }
  VIDEOCODEC_RETVAL Fail(const char* why) {
    fatal = true;
    kodi::Log(ADDON_LOG_ERROR, "CrystalHD-Native failed: %s", why);
    return VC_ERROR;
  }
public:
  NativeCodec(const kodi::addon::IInstanceInfo& info, unsigned n, unsigned d,
              std::shared_ptr<crystalhd_kodi::InputProgress> state = std::make_shared<crystalhd_kodi::InputProgress>())
    : CInstanceVideoCodec(info), progress(std::move(state)), fps_n(n), fps_d(d) {}
  ~NativeCodec() override {
    decoder.Close();
#ifdef CRYSTALHD_KODI_TRACE
    if (checksum) g_checksum_free(checksum);
#endif
  }
  bool Open(const kodi::addon::VideoCodecInitdata& data) override {
    decoder.Close(); active = false;
    width = data.GetWidth(); height = data.GetHeight();
    if (data.GetCodecType() != VIDEOCODEC_H264 || !data.GetExtraData() ||
        !data.GetExtraDataSize() || data.GetExtraDataSize() > max_packet) return false;
    extra.assign(data.GetExtraData(), data.GetExtraData() + data.GetExtraDataSize());
    const auto formats = data.GetVideoFormats();
    kodi::Log(ADDON_LOG_INFO, "CrystalHD-Native YUY2 advertised=%d; explicit nonadvertised packed-format experiment",
              std::find(formats.begin(), formats.end(), VIDEOCODEC_FORMAT_YUYV422) != formats.end());
    return Start();
  }
  const char* GetName() override { return "CrystalHD native YUY2 (experimental add-on)"; }
  bool AddData(const DEMUX_PACKET& packet) override {
    if (fatal) return true; // retrying a fatal packet is useless; GetPicture reports it
    if (!active && !Start()) { fatal = true; return true; }
    if (packet.iSize <= 0) { fatal = true; return true; }
#ifdef CRYSTALHD_KODI_PLAYBACK
    decoder.SetPaused(progress->paused.load());
#endif
    using Submit = PlaybackBackend::Submit;
    const Submit result = decoder.Push(packet.pData, packet.iSize, packet.pts, packet.dts, packet.duration);
    if (result == Submit::Retry) return false;
    if (result == Submit::Error) fatal = true;
    else ++accepted;
    return true;
  }
  VIDEOCODEC_RETVAL GetPicture(VIDEOCODEC_PICTURE& picture) override {
    const bool drain = (picture.flags & VIDEOCODEC_PICTURE_FLAG_DRAIN) != 0;
    if (fatal && drain) {
      /* This host drains until VC_EOF, including Reset/destruction after an
       * error. Retire the failed session before acknowledging terminal drain.
       * This is cancellation, NEVER a natural/media-completeness EOS pass. */
      decoder.Close(); active = false;
      kodi::Log(ADDON_LOG_WARNING, "CrystalHD-Native CANCELLED_ERROR_DRAIN accepted=%u pictures=%u; not natural EOS",
                accepted, pictures);
      return VC_EOF;
    }
    if (fatal) return Fail(decoder.Error().empty() ? "invalid native packet" : decoder.Error().c_str());
    if (!active) return drain ? VC_EOF : VC_BUFFER;
    if (progress->closed.load() || progress->epoch.load() != input_epoch) {
      if (!drain) return VC_BUFFER;
      decoder.Close(); active = false;
      kodi::Log(ADDON_LOG_INFO, "CrystalHD-Native CANCELLED_CONTROL_DRAIN; not natural EOS");
      return VC_EOF;
    }
    if (progress->failed.load()) return Fail("demux input failed before EOF");
#ifdef CRYSTALHD_KODI_PLAYBACK
    decoder.SetPaused(progress->paused.load());
#endif
    using Result = PlaybackBackend::Result;
    Result result;
    const bool final_input = progress->Final(input_epoch, accepted);
    GstSample* sample = decoder.Next(final_input, result);
    if (!sample) {
      if (result == Result::Error) return Fail(decoder.Error().c_str());
      if (result == Result::Eof) {
        if (!eof_logged) {
          kodi::Log(ADDON_LOG_INFO, "CrystalHD-Native EOF session=%u accepted=%u pictures=%u last_pts_us=%lld",
                    generation, accepted, pictures, static_cast<long long>(last_pts));
#ifdef CRYSTALHD_KODI_TRACE
          kodi::Log(ADDON_LOG_INFO, "CrystalHD-Native TRACE YUY2-active-row-v1 sha256=%s", g_checksum_get_string(checksum));
#endif
          eof_logged = true;
#ifdef CRYSTALHD_KODI_PLAYBACK
          const auto& c = decoder.Counts();
          kodi::Log(ADDON_LOG_INFO, "CrystalHD-Playback EOF generation=%llu queued=%llu returned=%llu pauses=%llu resumes=%llu terminal_eos=%llu; queue counts, not scanout",
                    static_cast<unsigned long long>(c.generation), static_cast<unsigned long long>(c.queued),
                    static_cast<unsigned long long>(c.returned), static_cast<unsigned long long>(c.pauses),
                    static_cast<unsigned long long>(c.resumes), static_cast<unsigned long long>(c.terminal_eos));
#endif
        }
        return VC_EOF;
      }
      /* VC_EOF here ends only a temporary squeeze phase. Keep the decoder
       * open and accept later packets; do not flush the firmware DPB or log
       * media EOS when the parent has not delivered complete finite input. */
      if (drain && !final_input) return VC_EOF;
      return result == Result::NeedInput ? VC_BUFFER : VC_NONE;
    }
    GstVideoInfo info{}; GstMapInfo map{};
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    int64_t pts;
    if (!gst_video_info_from_caps(&info, gst_sample_get_caps(sample)) ||
        GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_YUY2 ||
        GST_VIDEO_INFO_WIDTH(&info) != static_cast<int>(width) ||
        GST_VIDEO_INFO_HEIGHT(&info) != static_cast<int>(height) ||
        GST_VIDEO_INFO_IS_INTERLACED(&info) ||
        GST_BUFFER_FLAG_IS_SET(buffer, GST_VIDEO_BUFFER_FLAG_INTERLACED) ||
        !crystalhd_kodi::from_clock(GST_BUFFER_PTS(buffer), pts) || pts <= last_pts ||
        !gst_buffer_map(buffer, &map, GST_MAP_READ)) {
      gst_sample_unref(sample); return Fail("unexpected native layout, field mode or PTS");
    }
    const GstVideoMeta* meta = gst_buffer_get_video_meta(buffer);
    const gint stride = meta ? meta->stride[0] : info.stride[0];
    const size_t offset = meta ? meta->offset[0] : info.offset[0];
    if ((meta && (meta->format != GST_VIDEO_FORMAT_YUY2 || meta->width != width ||
                  meta->height != height || meta->n_planes != 1)) || stride <= 0 ||
        !crystalhd_kodi::packed_layout(width, height, offset, stride, map.size)) {
      gst_buffer_unmap(buffer, &map); gst_sample_unref(sample); return Fail("unsafe native source rows");
    }
    picture.videoFormat = VIDEOCODEC_FORMAT_YUYV422; picture.width = width; picture.height = height;
    picture.stride[0] = width * 2;
    picture.decodedDataSize = static_cast<size_t>(width) * height * 2;
    if (!GetFrameBuffer(picture) || !picture.videoBufferHandle || !picture.decodedData ||
        !crystalhd_kodi::packed_layout(width, height, picture.planeOffsets[0],
                                     picture.stride[0], picture.decodedDataSize)) {
      if (picture.videoBufferHandle) ReleaseFrameBuffer(picture.videoBufferHandle);
      picture.videoBufferHandle = nullptr;
      gst_buffer_unmap(buffer, &map); gst_sample_unref(sample); return Fail("unsafe Kodi destination rows");
    }
    for (unsigned y = 0; y < height; ++y) {
      auto* target = picture.decodedData + picture.planeOffsets[0] + static_cast<size_t>(y) * picture.stride[0];
      std::memcpy(target, map.data + offset + static_cast<size_t>(y) * stride, width * 2);
#ifdef CRYSTALHD_KODI_TRACE
      g_checksum_update(checksum, target, width * 2);
#endif
    }
    gst_buffer_unmap(buffer, &map); gst_sample_unref(sample);
    /* No Gst sample or hardware borrow escapes this call. Only Kodi's own
     * backing is transferred to its renderer on VC_PICTURE. */
    picture.pts = pts; last_pts = pts;
#ifdef CRYSTALHD_KODI_RGB_TEST
    observe_rgb_source(picture, pictures);
#endif
    ++pictures;
#ifdef CRYSTALHD_KODI_TRACE
    kodi::Log(ADDON_LOG_DEBUG, "CrystalHD-Native frame=%u pts_us=%lld stride=%u", pictures,
              static_cast<long long>(pts), picture.stride[0]);
#endif
    return VC_PICTURE;
  }
  void Reset() override {
    kodi::Log(ADDON_LOG_INFO, "CrystalHD-Native Reset session=%u accepted=%u pictures=%u eof=%d",
              generation, accepted, pictures, eof_logged);
    decoder.Close(); active = false; fatal = false;
    /* A seek starts a fresh decoder AND parser lazily on the next packet.
     * Reusing H264Parse across NULL changes its timestamp-inference policy. */
  }
#ifdef CRYSTALHD_KODI_TEST
  bool OpenRawTest() {
    width = 4; height = 2; input_epoch = progress->epoch.load();
    return active = decoder.OpenRawTest(width, height);
  }
#endif
};
#endif

class ProbeStream : public kodi::addon::CInstanceInputStream {
  AVFormatContext* format = nullptr;
  int video = -1;
#ifdef CRYSTALHD_KODI_NATIVE
  int audio = -1;
  int64_t media_end = 0;
  crystalhd_kodi::ColorHints color;
  std::shared_ptr<crystalhd_kodi::InputProgress> progress = std::make_shared<crystalhd_kodi::InputProgress>();
  unsigned video_packets = 0, audio_packets = 0;
#endif
public:
  explicit ProbeStream(const kodi::addon::IInstanceInfo& info) : CInstanceInputStream(info) {}
  ~ProbeStream() override { Close(); }
  ADDON_STATUS CreateInstance(const kodi::addon::IInstanceInfo& info,
                              KODI_ADDON_INSTANCE_HDL& handle) override {
    if (!info.IsType(ADDON_INSTANCE_VIDEOCODEC)) return ADDON_STATUS_NOT_IMPLEMENTED;
#ifdef CRYSTALHD_KODI_NATIVE
    if (!format || video < 0) return ADDON_STATUS_UNKNOWN;
    const AVRational fps = av_guess_frame_rate(format, format->streams[video], nullptr);
    if (fps.num <= 0 || fps.den <= 0) return ADDON_STATUS_UNKNOWN;
    handle = new NativeCodec(info, fps.num, fps.den, progress); return ADDON_STATUS_OK;
#else
    handle = new ProbeCodec(info); return ADDON_STATUS_OK;
#endif
  }
  bool Open(const kodi::addon::InputstreamProperty& props) override {
    Close(); const std::string path = props.GetURL(); struct stat st{};
#ifdef CRYSTALHD_KODI_NATIVE
    progress = std::make_shared<crystalhd_kodi::InputProgress>();
#endif
    if (path.empty() || path.front() != '/' || stat(path.c_str(), &st) ||
        !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > 256 * 1024 * 1024) return false;
    /* MP4 supplies the required codec parameters in its header. Do not run
     * find_stream_info: its probing can invoke a software video decoder. */
    if (avformat_open_input(&format, path.c_str(), av_find_input_format("mov"), nullptr) < 0) {
      Close(); return false;
    }
    video = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video < 0 || format->streams[video]->codecpar->codec_id != AV_CODEC_ID_H264) {
      Close(); return false;
    }
    const auto* p = format->streams[video]->codecpar;
    if (p->width <= 0 || (p->width & 1) || p->width > 1920 ||
        p->height <= 0 || p->height > 1088 || p->extradata_size < 0 ||
        static_cast<size_t>(p->extradata_size) > max_packet) { Close(); return false; }
#ifdef CRYSTALHD_KODI_NATIVE
    kodi::addon::InputstreamInfo color_check;
    if (!source_color(path, *p, color) || !copy_color(color, color_check) ||
        (p->field_order != AV_FIELD_UNKNOWN && p->field_order != AV_FIELD_PROGRESSIVE)) {
      kodi::Log(ADDON_LOG_ERROR, "CrystalHD-Native unsupported/malformed source color or field metadata");
      Close(); return false;
    }
    kodi::Log(ADDON_LOG_INFO, "CrystalHD-Native source color matrix=%u primaries=%u transfer=%u range=%u (ISO; unknown preserved)",
              color.matrix, color.primaries, color.transfer, color.range);
    audio = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audio >= 0) {
      const auto* a = format->streams[audio]->codecpar;
      /* One ordinary AAC track goes to Kodi's own audio decoder. Never send
       * audio into CrystalHD or silently discard an unsupported soundtrack. */
      if (a->codec_id != AV_CODEC_ID_AAC || a->sample_rate <= 0 || a->sample_rate > 192000 ||
          a->ch_layout.nb_channels < 1 || a->ch_layout.nb_channels > 2 ||
          !a->extradata || a->extradata_size <= 0 || static_cast<size_t>(a->extradata_size) > max_packet) {
        Close(); return false;
      }
    }
    /* mov's track headers have duration before find_stream_info, but the
     * AVFormatContext aggregate duration can still be AV_NOPTS_VALUE. Derive
     * the finite seek range from selected track headers without decoding. */
    for (int id : {video, audio}) {
      if (id < 0) continue;
      const AVStream* s = format->streams[id];
      if (s->duration <= 0 || s->duration == AV_NOPTS_VALUE) continue;
      const int64_t duration = av_rescale_q(s->duration, s->time_base, AVRational{1, AV_TIME_BASE});
      const int64_t start = s->start_time == AV_NOPTS_VALUE ? 0 :
          av_rescale_q(s->start_time, s->time_base, AVRational{1, AV_TIME_BASE});
      if (duration <= 0 || duration > 86400000000LL || start < 0 || start > 86400000000LL - duration) {
        Close(); return false;
      }
      media_end = std::max(media_end, start + duration);
    }
    if (!media_end) { Close(); return false; }
    kodi::Log(ADDON_LOG_INFO, "CrystalHD-Native InputStream selected; local H264, cleartext");
#else
    kodi::Log(ADDON_LOG_INFO, "%s InputStream opened local H264; companion decode feature, no crypto", marker);
#endif
    return true;
  }
  void Close() override {
#ifdef CRYSTALHD_KODI_NATIVE
    if (format) kodi::Log(ADDON_LOG_INFO, "CrystalHD-Native Demux close video_packets=%u audio_packets=%u",
                          video_packets, audio_packets);
    audio = -1; media_end = 0; video_packets = audio_packets = 0;
    progress->closed.store(true);
#endif
    if (format) avformat_close_input(&format);
    video = -1;
  }
  void GetCapabilities(kodi::addon::InputstreamCapabilities& caps) override {
#ifdef CRYSTALHD_KODI_NATIVE
    caps.SetMask(INPUTSTREAM_SUPPORTS_IDEMUX | INPUTSTREAM_SUPPORTS_SEEK |
                 INPUTSTREAM_SUPPORTS_PAUSE | INPUTSTREAM_SUPPORTS_ITIME);
#else
    caps.SetMask(INPUTSTREAM_SUPPORTS_IDEMUX);
#endif
  }
  bool IsRealTimeStream() override { return false; }
  bool GetStreamIds(std::vector<unsigned int>& ids) override {
    ids = {0};
#ifdef CRYSTALHD_KODI_NATIVE
    if (audio >= 0) ids.push_back(1);
#endif
    return format != nullptr;
  }
  bool GetStream(int id, kodi::addon::InputstreamInfo& info) override {
#ifdef CRYSTALHD_KODI_NATIVE
    if (format && id == 1 && audio >= 0) {
      const auto* a = format->streams[audio]->codecpar;
      info.SetStreamType(INPUTSTREAM_TYPE_AUDIO); info.SetFeatures(0);
      info.SetCodecName("aac"); info.SetPhysicalIndex(1);
      info.SetChannels(a->ch_layout.nb_channels); info.SetSampleRate(a->sample_rate);
      info.SetBitRate(a->bit_rate > 0 && a->bit_rate <= 64000000 ? a->bit_rate : 0);
      info.SetExtraData(a->extradata, a->extradata_size);
      return true;
    }
#endif
    if (!format || id != 0) return false;
    const auto* p = format->streams[video]->codecpar;
    const AVRational fps = av_guess_frame_rate(format, format->streams[video], nullptr);
    if (fps.num <= 0 || fps.den <= 0) return false;
    info.SetStreamType(INPUTSTREAM_TYPE_VIDEO); info.SetFeatures(INPUTSTREAM_FEATURE_DECODE);
    info.SetCodecName("h264");
#ifdef CRYSTALHD_KODI_NATIVE
    info.SetCodecInternalName("CrystalHD native YUY2 experiment");
    if (!copy_color(color, info)) return false;
#else
    info.SetCodecInternalName("CrystalHD route probe, NO hardware decode");
#endif
    info.SetPhysicalIndex(0); info.SetWidth(p->width); info.SetHeight(p->height);
    info.SetFpsRate(fps.num); info.SetFpsScale(fps.den);
    info.SetAspect(static_cast<float>(p->width) / p->height);
    if (p->extradata && p->extradata_size > 0) info.SetExtraData(p->extradata, p->extradata_size);
    return true;
  }
  bool OpenStream(int id) override {
#ifdef CRYSTALHD_KODI_NATIVE
    if (format && id == 1 && audio >= 0) return true;
#endif
    return format && id == 0;
  }
#ifdef CRYSTALHD_KODI_NATIVE
  bool GetTimes(kodi::addon::InputstreamTimes& times) override {
    if (!format || !media_end) return false;
    times.SetStartTime(0); times.SetPtsStart(0); times.SetPtsBegin(0);
    times.SetPtsEnd(static_cast<double>(media_end));
    return true;
  }
  int GetTotalTime() override {
    return format ? static_cast<int>(media_end / 1000) : 0;
  }
#ifdef CRYSTALHD_KODI_PLAYBACK
  void DemuxSetSpeed(int speed) override {
    progress->paused.store(speed == 0);
    kodi::Log(ADDON_LOG_INFO, "CrystalHD-Playback input_speed=%d epoch=%llu; no EOS/reset",
              speed, static_cast<unsigned long long>(progress->epoch.load()));
  }
#endif
  bool DemuxSeekTime(double ms, bool backwards, double& startpts) override {
    if (!format || !std::isfinite(ms) || ms < 0 || ms > 86400000.0) return false;
    const int64_t timestamp = av_rescale_q(static_cast<int64_t>(ms * 1000), AVRational{1, AV_TIME_BASE},
                                          format->streams[video]->time_base);
    if (av_seek_frame(format, video, timestamp, backwards ? AVSEEK_FLAG_BACKWARD : 0) < 0) return false;
    progress->Seek();
    startpts = ms * 1000;
    return true;
  }
#endif
  DEMUX_PACKET* DemuxRead() override {
    if (!format) return nullptr;
    AVPacket* input = av_packet_alloc(); if (!input) return nullptr;
    int status;
    while ((status = av_read_frame(format, input)) >= 0) {
      bool selected = input->stream_index == video;
#ifdef CRYSTALHD_KODI_NATIVE
      selected |= audio >= 0 && input->stream_index == audio;
#endif
      if (!selected) { av_packet_unref(input); continue; }
      if (input->size <= 0 || static_cast<size_t>(input->size) > max_packet) break;
      DEMUX_PACKET* packet = AllocateDemuxPacket(input->size);
      if (!packet) break;
      std::memcpy(packet->pData, input->data, input->size);
      /* Kodi's allocator reserves storage/padding but leaves iSize zero. */
      packet->iSize = input->size; packet->iStreamId = input->stream_index == video ? 0 : 1;
      packet->recoveryPoint = (input->flags & AV_PKT_FLAG_KEY) != 0;
      const AVRational from = format->streams[input->stream_index]->time_base, to{1, STREAM_TIME_BASE};
      /* Kodi's public STREAM_NOPTS_VALUE bit pattern is negative infinity.
       * Never fabricate a valid -1us DTS for a missing container timestamp. */
      packet->pts = input->pts == AV_NOPTS_VALUE ? -std::numeric_limits<double>::infinity() : static_cast<double>(av_rescale_q(input->pts, from, to));
      packet->dts = input->dts == AV_NOPTS_VALUE ? -std::numeric_limits<double>::infinity() : static_cast<double>(av_rescale_q(input->dts, from, to));
      packet->duration = av_rescale_q(input->duration, from, to);
#ifdef CRYSTALHD_KODI_NATIVE
      if (input->stream_index == video) { ++video_packets; progress->packets.fetch_add(1); }
      else ++audio_packets;
#endif
      av_packet_free(&input); return packet;
    }
    av_packet_free(&input);
#ifdef CRYSTALHD_KODI_NATIVE
    if (status == AVERROR_EOF) progress->eof.store(true);
    else progress->failed.store(true);
#endif
    return nullptr;
  }
};

class ProbeAddon : public kodi::addon::CAddonBase {
public:
  ADDON_STATUS CreateInstance(const kodi::addon::IInstanceInfo& info,
                              KODI_ADDON_INSTANCE_HDL& handle) override {
    if (!info.IsType(ADDON_INSTANCE_INPUTSTREAM)) return ADDON_STATUS_NOT_IMPLEMENTED;
    handle = new ProbeStream(info); return ADDON_STATUS_OK;
  }
};
}
ADDONCREATOR(ProbeAddon)
