/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Gate A only: real local-file demux/codec routing, one synthetic packed frame.
 * Deliberately has no libcrystalhd dependency and makes no decode claim. */
#include <kodi/addon-instance/Inputstream.h>
#include <kodi/addon-instance/VideoCodec.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <sys/stat.h>
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
}

namespace {
constexpr const char* marker = "CrystalHD-GateA";
constexpr size_t max_packet = 4 * 1024 * 1024;

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
    const size_t offset = picture.planeOffsets[0], pitch = picture.stride[0], row_bytes = width * 2;
    const size_t size = picture.decodedDataSize;
    if (!picture.videoBufferHandle || !picture.decodedData || pitch < row_bytes ||
        offset > size || row_bytes > size - offset ||
        (height > 1 && pitch > (size - offset - row_bytes) / (height - 1))) {
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

class ProbeStream : public kodi::addon::CInstanceInputStream {
  AVFormatContext* format = nullptr;
  int video = -1;
public:
  explicit ProbeStream(const kodi::addon::IInstanceInfo& info) : CInstanceInputStream(info) {}
  ~ProbeStream() override { Close(); }
  ADDON_STATUS CreateInstance(const kodi::addon::IInstanceInfo& info,
                              KODI_ADDON_INSTANCE_HDL& handle) override {
    if (!info.IsType(ADDON_INSTANCE_VIDEOCODEC)) return ADDON_STATUS_NOT_IMPLEMENTED;
    handle = new ProbeCodec(info); return ADDON_STATUS_OK;
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
    kodi::Log(ADDON_LOG_INFO, "%s InputStream opened local H264; companion decode feature, no crypto", marker);
    return true;
  }
  void Close() override { if (format) avformat_close_input(&format); video = -1; }
  void GetCapabilities(kodi::addon::InputstreamCapabilities& caps) override {
    caps.SetMask(INPUTSTREAM_SUPPORTS_IDEMUX);
  }
  bool GetStreamIds(std::vector<unsigned int>& ids) override { ids = {0}; return format != nullptr; }
  bool GetStream(int id, kodi::addon::InputstreamInfo& info) override {
    if (!format || id != 0) return false;
    const auto* p = format->streams[video]->codecpar;
    const AVRational fps = av_guess_frame_rate(format, format->streams[video], nullptr);
    if (fps.num <= 0 || fps.den <= 0) return false;
    info.SetStreamType(INPUTSTREAM_TYPE_VIDEO); info.SetFeatures(INPUTSTREAM_FEATURE_DECODE);
    info.SetCodecName("h264"); info.SetCodecInternalName("CrystalHD route probe, NO hardware decode");
    info.SetPhysicalIndex(0); info.SetWidth(p->width); info.SetHeight(p->height);
    info.SetFpsRate(fps.num); info.SetFpsScale(fps.den);
    info.SetAspect(static_cast<float>(p->width) / p->height);
    if (p->extradata && p->extradata_size > 0) info.SetExtraData(p->extradata, p->extradata_size);
    return true;
  }
  bool OpenStream(int id) override { return format && id == 0; }
  DEMUX_PACKET* DemuxRead() override {
    if (!format) return nullptr;
    AVPacket* input = av_packet_alloc(); if (!input) return nullptr;
    while (av_read_frame(format, input) >= 0) {
      if (input->stream_index != video) { av_packet_unref(input); continue; }
      if (input->size <= 0 || static_cast<size_t>(input->size) > max_packet) break;
      DEMUX_PACKET* packet = AllocateDemuxPacket(input->size);
      if (!packet) break;
      std::memcpy(packet->pData, input->data, input->size); packet->iStreamId = 0;
      const AVRational from = format->streams[video]->time_base, to{1, STREAM_TIME_BASE};
      packet->pts = input->pts == AV_NOPTS_VALUE ? -1.0 : static_cast<double>(av_rescale_q(input->pts, from, to));
      packet->dts = input->dts == AV_NOPTS_VALUE ? -1.0 : static_cast<double>(av_rescale_q(input->dts, from, to));
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
