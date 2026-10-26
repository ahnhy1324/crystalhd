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
#include <gst/video/video.h>
#endif
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
}

namespace {
constexpr const char* marker = "CrystalHD-GateA";
constexpr size_t max_packet = crystalhd_kodi::packet_limit;

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
class NativeCodec : public kodi::addon::CInstanceVideoCodec {
  crystalhd_kodi::NativeDecoder decoder;
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
    }
    return active;
  }
  VIDEOCODEC_RETVAL Fail(const char* why) {
    fatal = true;
    kodi::Log(ADDON_LOG_ERROR, "CrystalHD-Native failed: %s", why);
    return VC_ERROR;
  }
public:
  NativeCodec(const kodi::addon::IInstanceInfo& info, unsigned n, unsigned d)
    : CInstanceVideoCodec(info), fps_n(n), fps_d(d) {}
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
    using Submit = crystalhd_kodi::NativeDecoder::Submit;
    const Submit result = decoder.Push(packet.pData, packet.iSize, packet.pts, packet.dts, packet.duration);
    if (result == Submit::Retry) return false;
    if (result == Submit::Error) fatal = true;
    else ++accepted;
    return true;
  }
  VIDEOCODEC_RETVAL GetPicture(VIDEOCODEC_PICTURE& picture) override {
    if (fatal) return Fail(decoder.Error().empty() ? "invalid native packet" : decoder.Error().c_str());
    if (!active) return VC_BUFFER;
    using Result = crystalhd_kodi::NativeDecoder::Result;
    Result result;
    GstSample* sample = decoder.Next((picture.flags & VIDEOCODEC_PICTURE_FLAG_DRAIN) != 0, result);
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
        }
        return VC_EOF;
      }
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
    picture.pts = pts; last_pts = pts; ++pictures;
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
};
#endif

class ProbeStream : public kodi::addon::CInstanceInputStream {
  AVFormatContext* format = nullptr;
  int video = -1;
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
    handle = new NativeCodec(info, fps.num, fps.den); return ADDON_STATUS_OK;
#else
    handle = new ProbeCodec(info); return ADDON_STATUS_OK;
#endif
  }
  bool Open(const kodi::addon::InputstreamProperty& props) override {
    Close(); const std::string path = props.GetURL(); struct stat st{};
    if (path.empty() || path.front() != '/' || stat(path.c_str(), &st) ||
        !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > 256 * 1024 * 1024) return false;
    /* MP4 supplies the required codec parameters in its header. Do not run
     * find_stream_info: its probing can invoke a software video decoder. */
    if (avformat_open_input(&format, path.c_str(), nullptr, nullptr) < 0) { Close(); return false; }
    video = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video < 0 || format->streams[video]->codecpar->codec_id != AV_CODEC_ID_H264) {
      Close(); return false;
    }
    const auto* p = format->streams[video]->codecpar;
    if (p->width <= 0 || (p->width & 1) || p->width > 1920 ||
        p->height <= 0 || p->height > 1088 || p->extradata_size < 0 ||
        static_cast<size_t>(p->extradata_size) > max_packet) { Close(); return false; }
#ifdef CRYSTALHD_KODI_NATIVE
    if (p->color_range == AVCOL_RANGE_JPEG ||
        (p->field_order != AV_FIELD_UNKNOWN && p->field_order != AV_FIELD_PROGRESSIVE)) {
      Close(); return false;
    }
    kodi::Log(ADDON_LOG_INFO, "CrystalHD-Native InputStream selected; local H264, cleartext");
#else
    kodi::Log(ADDON_LOG_INFO, "%s InputStream opened local H264; companion decode feature, no crypto", marker);
#endif
    return true;
  }
  void Close() override { if (format) avformat_close_input(&format); video = -1; }
  void GetCapabilities(kodi::addon::InputstreamCapabilities& caps) override {
#ifdef CRYSTALHD_KODI_NATIVE
    caps.SetMask(INPUTSTREAM_SUPPORTS_IDEMUX | INPUTSTREAM_SUPPORTS_SEEK | INPUTSTREAM_SUPPORTS_PAUSE);
#else
    caps.SetMask(INPUTSTREAM_SUPPORTS_IDEMUX);
#endif
  }
  bool IsRealTimeStream() override { return false; }
  bool GetStreamIds(std::vector<unsigned int>& ids) override { ids = {0}; return format != nullptr; }
  bool GetStream(int id, kodi::addon::InputstreamInfo& info) override {
    if (!format || id != 0) return false;
    const auto* p = format->streams[video]->codecpar;
    const AVRational fps = av_guess_frame_rate(format, format->streams[video], nullptr);
    if (fps.num <= 0 || fps.den <= 0) return false;
    info.SetStreamType(INPUTSTREAM_TYPE_VIDEO); info.SetFeatures(INPUTSTREAM_FEATURE_DECODE);
    info.SetCodecName("h264");
#ifdef CRYSTALHD_KODI_NATIVE
    info.SetCodecInternalName("CrystalHD native YUY2 experiment");
    if (p->color_range == AVCOL_RANGE_MPEG) info.SetColorRange(INPUTSTREAM_COLORRANGE_LIMITED);
    switch (p->color_space) {
    case AVCOL_SPC_BT709: info.SetColorSpace(INPUTSTREAM_COLORSPACE_BT709); break;
    case AVCOL_SPC_BT470BG: info.SetColorSpace(INPUTSTREAM_COLORSPACE_BT470BG); break;
    case AVCOL_SPC_SMPTE170M: info.SetColorSpace(INPUTSTREAM_COLORSPACE_SMPTE170M); break;
    default: info.SetColorSpace(INPUTSTREAM_COLORSPACE_UNSPECIFIED); break;
    }
#else
    info.SetCodecInternalName("CrystalHD route probe, NO hardware decode");
#endif
    info.SetPhysicalIndex(0); info.SetWidth(p->width); info.SetHeight(p->height);
    info.SetFpsRate(fps.num); info.SetFpsScale(fps.den);
    info.SetAspect(static_cast<float>(p->width) / p->height);
    if (p->extradata && p->extradata_size > 0) info.SetExtraData(p->extradata, p->extradata_size);
    return true;
  }
  bool OpenStream(int id) override { return format && id == 0; }
#ifdef CRYSTALHD_KODI_NATIVE
  bool DemuxSeekTime(double ms, bool backwards, double& startpts) override {
    if (!format || !std::isfinite(ms) || ms < 0 || ms > 86400000.0) return false;
    const int64_t timestamp = av_rescale_q(static_cast<int64_t>(ms * 1000), AVRational{1, AV_TIME_BASE},
                                          format->streams[video]->time_base);
    if (av_seek_frame(format, video, timestamp, backwards ? AVSEEK_FLAG_BACKWARD : 0) < 0) return false;
    startpts = ms * 1000;
    return true;
  }
#endif
  DEMUX_PACKET* DemuxRead() override {
    if (!format) return nullptr;
    AVPacket* input = av_packet_alloc(); if (!input) return nullptr;
    while (av_read_frame(format, input) >= 0) {
      if (input->stream_index != video) { av_packet_unref(input); continue; }
      if (input->size <= 0 || static_cast<size_t>(input->size) > max_packet) break;
      DEMUX_PACKET* packet = AllocateDemuxPacket(input->size);
      if (!packet) break;
      std::memcpy(packet->pData, input->data, input->size);
      /* Kodi's allocator reserves storage/padding but leaves iSize zero. */
      packet->iSize = input->size; packet->iStreamId = 0;
      const AVRational from = format->streams[video]->time_base, to{1, STREAM_TIME_BASE};
      /* Kodi's public STREAM_NOPTS_VALUE bit pattern is negative infinity.
       * Never fabricate a valid -1us DTS for a missing container timestamp. */
      packet->pts = input->pts == AV_NOPTS_VALUE ? -std::numeric_limits<double>::infinity() : static_cast<double>(av_rescale_q(input->pts, from, to));
      packet->dts = input->dts == AV_NOPTS_VALUE ? -std::numeric_limits<double>::infinity() : static_cast<double>(av_rescale_q(input->dts, from, to));
      packet->duration = av_rescale_q(input->duration, from, to);
      av_packet_free(&input); return packet;
    }
    av_packet_free(&input); return nullptr;
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
