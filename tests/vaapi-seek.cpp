// SPDX-License-Identifier: LGPL-2.1-or-later
// Compare actual downloaded pixels after avcodec_flush_buffers()/seek with
// the same decoder's uninterrupted output. Timestamps alone miss stale frames.
extern "C" {
#include "ffmpeg-profile-compat.h"
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libavutil/imgutils.h>
#include <libavutil/sha.h>
}

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <libdrm/drm_fourcc.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <va/va_drmcommon.h>

static void Check(int result, const char *operation) {
  if (result >= 0)
    return;
  char error[AV_ERROR_MAX_STRING_SIZE];
  av_strerror(result, error, sizeof(error));
  throw std::runtime_error(std::string(operation) + ": " + error);
}

static size_t ParseExpectedFrames(const char *text) {
  if (!text || !*text)
    throw std::runtime_error("--expected-frames requires a positive integer");
  size_t value = 0;
  for (const unsigned char byte : std::string(text)) {
    if (byte < '0' || byte > '9')
      throw std::runtime_error("--expected-frames requires a positive integer");
    const size_t digit = byte - '0';
    if (value > (std::numeric_limits<size_t>::max() - digit) / 10)
      throw std::runtime_error("--expected-frames is out of range");
    value = value * 10 + digit;
  }
  if (value == 0)
    throw std::runtime_error("--expected-frames requires a positive integer");
  return value;
}

static AVPixelFormat ChooseHardware(AVCodecContext *, const AVPixelFormat *formats) {
  for (; *formats != AV_PIX_FMT_NONE; ++formats)
    if (*formats == AV_PIX_FMT_VAAPI)
      return *formats;
  return AV_PIX_FMT_NONE;  // Never count software fallback as a hardware pass.
}

struct Picture {
  int width, height, format;
  std::array<uint8_t, 32> digest;
  bool operator==(const Picture &other) const {
    return width == other.width && height == other.height &&
           format == other.format && digest == other.digest;
  }
};

struct FreeFrame {
  void operator()(AVFrame *frame) const { av_frame_free(&frame); }
};
using OwnedFrame = std::unique_ptr<AVFrame, FreeFrame>;

struct PrimeDescriptor {
  VADRMPRIMESurfaceDescriptor value = {};
  PrimeDescriptor() {
    for (auto &object : value.objects)
      object.fd = -1;
  }
  ~PrimeDescriptor() {
    for (size_t index = 0; index < value.num_objects &&
         index < sizeof(value.objects) / sizeof(value.objects[0]); ++index)
      if (value.objects[index].fd >= 0)
        close(value.objects[index].fd);
  }
};

static bool DmaReadSync(int fd, uint64_t phase) {
  dma_buf_sync sync = {};
  sync.flags = phase | DMA_BUF_SYNC_READ;
  int result;
  do { result = ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync); }
  while (result < 0 && (errno == EINTR || errno == EAGAIN));
  return result == 0;
}

struct PrimeRead {
  int fd = -1;
  size_t size = 0;
  void *data = MAP_FAILED;
  bool started = false;
  ~PrimeRead() {
    if (started)
      DmaReadSync(fd, DMA_BUF_SYNC_END);
    if (data != MAP_FAILED)
      munmap(data, size);
  }
  void Finish() {
    const bool ended = DmaReadSync(fd, DMA_BUF_SYNC_END);
    started = false;
    const bool unmapped = munmap(data, size) == 0;
    data = MAP_FAILED;
    if (!ended || !unmapped)
      throw std::runtime_error("exported DMA-BUF read cleanup failed");
  }
};

// Deliberately no vaSyncSurface or hwdownload before export, matching VLC's
// modern PRIME2 consumer. DMA_BUF_SYNC must operate on a real DMA-BUF; a plain
// memfd is not accepted as evidence of synchronized exported pixels.
static std::vector<uint8_t> ReadPrime(const AVFrame *frame) {
  if (frame->width <= 0 || frame->height <= 0 || !frame->hw_frames_ctx)
    throw std::runtime_error("invalid exported frame geometry/context");
  auto *frames = reinterpret_cast<AVHWFramesContext *>(frame->hw_frames_ctx->data);
  if (!frames->device_ctx || frames->device_ctx->type != AV_HWDEVICE_TYPE_VAAPI)
    throw std::runtime_error("exported frame has no VA-API device");
  auto *device = static_cast<AVVAAPIDeviceContext *>(frames->device_ctx->hwctx);
  if (!device || !device->display)
    throw std::runtime_error("exported frame has no VA display");
  PrimeDescriptor exported;
  auto &desc = exported.value;
  const VAStatus status = vaExportSurfaceHandle(device->display,
      static_cast<VASurfaceID>(reinterpret_cast<uintptr_t>(frame->data[3])),
      VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, 0, &desc);
  if (status != VA_STATUS_SUCCESS)
    throw std::runtime_error(std::string("export PRIME2 frame: ") + vaErrorStr(status));
  if (desc.fourcc != VA_FOURCC_NV12 || desc.num_layers != 2 ||
      desc.num_objects == 0 || desc.num_objects > 4 ||
      desc.width < static_cast<unsigned int>(frame->width) ||
      desc.height < static_cast<unsigned int>(frame->height))
    throw std::runtime_error("expected default separate NV12 export layers");
  const int size = av_image_get_buffer_size(AV_PIX_FMT_NV12,
                                           frame->width, frame->height, 1);
  Check(size, "size exported pixels");
  std::vector<uint8_t> packed(size);
  size_t destination = 0;
  for (unsigned int plane = 0; plane < 2; ++plane) {
    const auto &layer = desc.layers[plane];
    const uint32_t columns = plane == 0 ? frame->width :
        (static_cast<uint32_t>(frame->width) + 1U) & ~1U;
    const uint32_t rows = plane == 0 ? frame->height :
        (static_cast<uint32_t>(frame->height) + 1U) / 2U;
    if (layer.num_planes != 1 || layer.object_index[0] >= desc.num_objects ||
        layer.drm_format != (plane == 0 ? DRM_FORMAT_R8 : DRM_FORMAT_GR88) ||
        layer.pitch[0] < columns)
      throw std::runtime_error("invalid exported NV12 plane layout");
    const auto &object = desc.objects[layer.object_index[0]];
    const uint64_t end = layer.offset[0] +
        static_cast<uint64_t>(rows - 1) * layer.pitch[0] + columns;
    if (object.fd < 0 || object.drm_format_modifier != DRM_FORMAT_MOD_LINEAR ||
        end > object.size || static_cast<uint64_t>(rows) * columns > packed.size() - destination)
      throw std::runtime_error("exported NV12 plane exceeds linear backing");
    PrimeRead map;
    map.fd = object.fd;
    map.size = object.size;
    map.data = mmap(nullptr, map.size, PROT_READ, MAP_SHARED, map.fd, 0);
    if (map.data == MAP_FAILED || !(map.started = DmaReadSync(map.fd, DMA_BUF_SYNC_START)))
      throw std::runtime_error("exported DMA-BUF mapping/read ownership failed");
    for (uint32_t row = 0; row < rows; ++row) {
      memcpy(packed.data() + destination,
             static_cast<const uint8_t *>(map.data) + layer.offset[0] +
                 static_cast<size_t>(row) * layer.pitch[0], columns);
      destination += columns;
    }
    map.Finish();
  }
  if (destination != packed.size())
    throw std::runtime_error("exported NV12 visible pixel size differs");
  return packed;
}

struct HeldPicture {
  OwnedFrame frame;
  int64_t pts;
  Picture expected;
};

struct Probe {
  AVFormatContext *input = nullptr;
  AVCodecContext *decoder = nullptr;
  AVPacket *packet = av_packet_alloc();
  AVFrame *frame = av_frame_alloc();
  AVFrame *download = av_frame_alloc();
  const AVCodec *codec = nullptr;
  std::string device_path;
  int stream = -1;
  bool software = false;
  bool export_prime = false;
  bool retain_old_frames = false;
  bool close_before_tail = false;
  bool live_gap = false;
  bool live_gap_exercised = false;
  size_t lookahead = 0;
  size_t expected_frames = 0;
  size_t deferred_at_limit = 0;
  std::map<int64_t, Picture> reference;
  std::map<int64_t, AVPictureType> reference_types;
  std::map<int64_t, bool> reference_keyframes;
  std::map<int64_t, int64_t> reference_key_dts;

  bool UsesH264TransportDts() const {
    return codec->id == AV_CODEC_ID_H264 &&
           std::strcmp(input->iformat->name, "mpegts") == 0;
  }

  ~Probe() {
    av_frame_free(&download);
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&decoder);
    avformat_close_input(&input);
  }

  void Open(const char *path, const char *device) {
    if (!packet || !frame || !download)
      throw std::runtime_error("allocate probe buffers");
    Check(avformat_open_input(&input, path, nullptr, nullptr), "open input");
    Check(avformat_find_stream_info(input, nullptr), "read stream info");
    stream = av_find_best_stream(input, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    Check(stream, "find video");
    if (live_gap && codec->id != AV_CODEC_ID_H264)
      throw std::runtime_error("--live-gap requires H.264 input");
    if (codec->id != AV_CODEC_ID_H264 && codec->id != AV_CODEC_ID_MPEG2VIDEO &&
        codec->id != AV_CODEC_ID_MPEG4 && codec->id != AV_CODEC_ID_VC1 &&
        codec->id != AV_CODEC_ID_WMV3)
      throw std::runtime_error("this probe requires a seekable H.264, MPEG-2, MPEG-4 Part 2, VC-1 or WMV3 container");
    if (codec->id == AV_CODEC_ID_MPEG2VIDEO) {
      const AVCodecParameters *parameters = input->streams[stream]->codecpar;
      if (parameters->profile != AV_PROFILE_MPEG2_SIMPLE &&
          parameters->profile != AV_PROFILE_MPEG2_MAIN)
        throw std::runtime_error("MPEG-2 validation requires Simple or Main profile");
      if (parameters->field_order != AV_FIELD_PROGRESSIVE)
        throw std::runtime_error("MPEG-2 validation requires a declared progressive stream");
    }
    if (codec->id == AV_CODEC_ID_MPEG4) {
      const AVCodecParameters *parameters = input->streams[stream]->codecpar;
      if (parameters->profile != AV_PROFILE_MPEG4_SIMPLE &&
          parameters->profile != AV_PROFILE_MPEG4_ADVANCED_SIMPLE)
        throw std::runtime_error("MPEG-4 validation requires Simple or Advanced Simple profile");
      if (parameters->level != 3 && parameters->level != 5)
        throw std::runtime_error("MPEG-4 validation requires level 3 or 5");
      if (parameters->field_order != AV_FIELD_PROGRESSIVE &&
          parameters->field_order != AV_FIELD_UNKNOWN)
        throw std::runtime_error("MPEG-4 validation requires progressive pictures");
    }
    if (codec->id == AV_CODEC_ID_VC1 || codec->id == AV_CODEC_ID_WMV3) {
      const AVCodecParameters *parameters = input->streams[stream]->codecpar;
      if (parameters->profile != AV_PROFILE_VC1_SIMPLE &&
          parameters->profile != AV_PROFILE_VC1_MAIN &&
          parameters->profile != AV_PROFILE_VC1_ADVANCED)
        throw std::runtime_error("VC-1 validation requires Simple, Main or Advanced profile");
      if (parameters->field_order != AV_FIELD_PROGRESSIVE &&
          parameters->field_order != AV_FIELD_UNKNOWN)
        throw std::runtime_error("VC-1 validation requires progressive pictures");
    }
    const int64_t declared_frames = input->streams[stream]->nb_frames;
    if (declared_frames > 0) {
      if (static_cast<uint64_t>(declared_frames) >
          std::numeric_limits<size_t>::max())
        throw std::runtime_error("declared frame count is out of range");
      const size_t declared = static_cast<size_t>(declared_frames);
      if (expected_frames != 0 && expected_frames != declared)
        throw std::runtime_error("--expected-frames conflicts with the container's declared frame count");
      expected_frames = declared;
    } else if (expected_frames == 0) {
      throw std::runtime_error(
          "container has no declared frame count; provide an independently verified --expected-frames boundary");
    }
    device_path = device;
    OpenDecoder();
  }

  void OpenDecoder() {
    if (decoder)
      throw std::runtime_error("decoder already open");
    decoder = avcodec_alloc_context3(codec);
    if (!decoder)
      throw std::runtime_error("allocate decoder");
    Check(avcodec_parameters_to_context(decoder, input->streams[stream]->codecpar),
          "copy codec parameters");
    decoder->pkt_timebase = input->streams[stream]->time_base;
    decoder->thread_count = 1;
    decoder->err_recognition = AV_EF_EXPLODE;
    if (!software) {
      // FFmpeg's MPEG-2 VA path cannot run software error concealment; its
      // unconsumed error count otherwise makes AV_EF_EXPLODE reject valid input.
      if (codec->id == AV_CODEC_ID_MPEG2VIDEO)
        decoder->error_concealment = 0;
      decoder->get_format = ChooseHardware;
      if (lookahead != 0)
        decoder->extra_hw_frames = static_cast<int>(lookahead);
      Check(av_hwdevice_ctx_create(&decoder->hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI,
                                   device_path.c_str(), nullptr, 0), "create VA-API device");
    }
    Check(avcodec_open2(decoder, codec, nullptr), "open decoder");
  }

  bool NextPacketIsNonIdr() const {
    if ((packet->flags & AV_PKT_FLAG_KEY) != 0 || !packet->data || packet->size <= 0)
      return false;
    const AVCodecParameters *parameters = input->streams[stream]->codecpar;
    const bool avcc = parameters->extradata && parameters->extradata_size >= 5 &&
                      parameters->extradata[0] == 1;
    bool vcl = false;
    const size_t size = static_cast<size_t>(packet->size);
    if (avcc) {
      const size_t length_size = (parameters->extradata[4] & 3U) + 1U;
      for (size_t at = 0; at < size;) {
        if (size - at < length_size)
          return false;
        size_t length = 0;
        for (size_t byte = 0; byte < length_size; ++byte)
          length = (length << 8U) | packet->data[at++];
        if (length == 0 || length > size - at)
          return false;
        const unsigned int type = packet->data[at] & 31U;
        if (type == 5)
          return false;
        vcl = vcl || type == 1;
        at += length;
      }
    } else {
      for (size_t at = 0; at + 3U < size; ++at) {
        if (packet->data[at] != 0 || packet->data[at + 1] != 0 ||
            packet->data[at + 2] != 1)
          continue;
        const unsigned int type = packet->data[at + 3] & 31U;
        if (type == 5)
          return false;
        vcl = vcl || type == 1;
      }
    }
    return vcl;
  }

  void TryLiveGap(const std::deque<OwnedFrame> &deferred) {
#if VA_CHECK_VERSION(1, 9, 0)
    const AVFrame *owner = deferred.back().get();
    if (owner->format != AV_PIX_FMT_VAAPI || !owner->hw_frames_ctx)
      throw std::runtime_error("live gap requires an owned VA frame pool");
    auto *frames = reinterpret_cast<AVHWFramesContext *>(owner->hw_frames_ctx->data);
    if (!frames->device_ctx || frames->device_ctx->type != AV_HWDEVICE_TYPE_VAAPI)
      throw std::runtime_error("live gap frame pool has no VA device");
    auto *device = static_cast<AVVAAPIDeviceContext *>(frames->device_ctx->hwctx);
    if (!device || !device->display)
      throw std::runtime_error("live gap frame pool has no VA display");
    const auto try_surface = [&](VASurfaceID surface, const char *source,
                                 int64_t pts) {
      VASurfaceStatus state = VASurfaceReady;
      if (vaQuerySurfaceStatus(device->display, surface, &state) != VA_STATUS_SUCCESS)
        throw std::runtime_error("query live-gap surface failed");
      if (state != VASurfaceRendering)
        return false;
      // Unused static-pool slots may also report Rendering. Zero-time sync
      // validates an actual pending decode binding without pumping input.
      const VAStatus bound = vaSyncSurface2(device->display, surface, 0);
      if (bound == VA_STATUS_SUCCESS || bound == VA_STATUS_ERROR_INVALID_CONTEXT)
        return false;
      if (bound != VA_STATUS_ERROR_TIMEDOUT)
        throw std::runtime_error("live-gap surface has no valid pending binding");
      constexpr uint64_t gap_ns = 350ULL * 1000U * 1000U;
      std::fprintf(stderr, "live-gap begin surface=%u owner=%s PTS=%lld next-packet-PTS=%lld next-packet-DTS=%lld wait-ns=%llu\n",
                  surface, source, static_cast<long long>(pts),
                  static_cast<long long>(packet->pts), static_cast<long long>(packet->dts),
                  static_cast<unsigned long long>(gap_ns));
      std::fflush(stderr);
      const auto start = std::chrono::steady_clock::now();
      const VAStatus waited = vaSyncSurface2(device->display, surface, gap_ns);
      const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - start).count();
      if (waited == VA_STATUS_SUCCESS) {
        std::fprintf(stderr, "live-gap end surface=%u elapsed-ns=%lld status=SUCCESS; gap not established\n",
                     surface, static_cast<long long>(elapsed));
        std::fflush(stderr);
        return false;
      }
      if (waited != VA_STATUS_ERROR_TIMEDOUT || elapsed < static_cast<int64_t>(gap_ns) ||
          vaQuerySurfaceStatus(device->display, surface, &state) != VA_STATUS_SUCCESS ||
          state != VASurfaceRendering)
        throw std::runtime_error("live gap did not preserve a pending surface through its deadline");
      std::fprintf(stderr, "live-gap end surface=%u elapsed-ns=%lld status=TIMEDOUT still=Rendering; resume original non-IDR packet\n",
                  surface, static_cast<long long>(elapsed));
      std::fflush(stderr);
      live_gap_exercised = true;
      return true;
    };
    for (auto picture = deferred.rbegin(); picture != deferred.rend(); ++picture) {
      if (try_surface(static_cast<VASurfaceID>(reinterpret_cast<uintptr_t>((*picture)->data[3])),
                      "retained-frame", (*picture)->best_effort_timestamp))
        return;
    }
    // Reordered pending pictures need not have been returned as AVFrames.
    // The documented static pool retains its own live surface owners.
    auto *pool = static_cast<AVVAAPIFramesContext *>(frames->hwctx);
    if (frames->initial_pool_size > 0 && pool && pool->surface_ids && pool->nb_surfaces > 0) {
      for (int index = 0; index < pool->nb_surfaces; ++index)
        if (try_surface(pool->surface_ids[index], "static-pool", AV_NOPTS_VALUE))
          return;
    }
#else
    (void)deferred;
    throw std::runtime_error("--live-gap requires VA-API 1.9 headers or newer");
#endif
  }

  Picture Fingerprint(const AVFrame *picture_frame, bool allow_export = true) {
    const AVFrame *pixels = picture_frame;
    const bool exported = export_prime && allow_export;
    std::vector<uint8_t> packed;
    if (!software) {
      if (picture_frame->format != AV_PIX_FMT_VAAPI)
        throw std::runtime_error("decoder returned a software frame");
      if (exported) {
        packed = ReadPrime(picture_frame);
      } else {
        av_frame_unref(download);
        Check(av_hwframe_transfer_data(download, picture_frame, 0), "download VA-API frame");
        if (download->format != AV_PIX_FMT_NV12)
          throw std::runtime_error("expected NV12 download");
        pixels = download;
      }
    }
    const auto format = exported ? AV_PIX_FMT_NV12 : static_cast<AVPixelFormat>(pixels->format);
    if (!exported) {
      int size = av_image_get_buffer_size(format, pixels->width, pixels->height, 1);
      Check(size, "size pixels");
      packed.resize(size);
      Check(av_image_copy_to_buffer(packed.data(), size, pixels->data,
                                   pixels->linesize, format, pixels->width,
                                   pixels->height, 1), "pack pixels");
    }
    Picture picture{pixels->width, pixels->height, format, {}};
    AVSHA *sha = av_sha_alloc();
    if (!sha)
      throw std::runtime_error("allocate SHA-256");
    av_sha_init(sha, 256);
    av_sha_update(sha, packed.data(), packed.size());
    av_sha_final(sha, picture.digest.data());
    av_free(sha);
    return picture;
  }

  // Limit output on seek passes to leave reordered frames pending before the
  // next flush. The reference pass must reach EOF and drain completely.
  size_t Decode(bool record, int64_t target, size_t limit,
                std::vector<HeldPicture> *held = nullptr,
                bool complete = false) {
    if (held && (record || !held->empty()))
      throw std::runtime_error("retained output requires an empty seek-pass owner");
    size_t count = 0;
    bool draining = false;
    deferred_at_limit = 0;
    std::deque<OwnedFrame> deferred;
    auto expected = reference.lower_bound(target);
    auto consume_oldest = [&] {
      OwnedFrame picture_frame = std::move(deferred.front());
      deferred.pop_front();
      const int64_t pts = picture_frame->best_effort_timestamp;
      Picture picture = Fingerprint(picture_frame.get(), !record);
      if (record) {
        if (!reference.emplace(pts, picture).second)
          throw std::runtime_error("duplicate reference timestamp");
        if (!reference_types.emplace(pts, picture_frame->pict_type).second ||
            !reference_keyframes.emplace(
                pts, (picture_frame->flags & AV_FRAME_FLAG_KEY) != 0).second)
          throw std::runtime_error("duplicate reference picture metadata");
      } else {
        if (expected == reference.end() || expected->first != pts)
          throw std::runtime_error("post-seek timestamp differs: expected " +
              (expected == reference.end() ? std::string("end of reference") :
               "PTS " + std::to_string(expected->first)) +
              ", got PTS " + std::to_string(pts));
        if (!(expected->second == picture))
          throw std::runtime_error("post-seek pixels differ at PTS " +
                                   std::to_string(pts));
        ++expected;
      }
      ++count;
    };
    auto finish_limited_pass = [&] {
      deferred_at_limit = deferred.size();
      if (held) {
        while (!deferred.empty()) {
          const int64_t pts = deferred.front()->best_effort_timestamp;
          if (expected == reference.end() || expected->first != pts)
            throw std::runtime_error("retained frame sequence differs at PTS " +
                                     std::to_string(pts));
          // Capture this frame's original identity and reference pixels before
          // the lifecycle operation. Do not download the held suffix yet.
          held->push_back({std::move(deferred.front()), pts, expected->second});
          deferred.pop_front();
          ++expected;
        }
      }
      return count;
    };
    for (;;) {
      int result = avcodec_receive_frame(decoder, frame);
      if (result == AVERROR_EOF)
        break;
      if (result != AVERROR(EAGAIN)) {
        Check(result, "receive frame");
        if ((codec->id == AV_CODEC_ID_MPEG2VIDEO || codec->id == AV_CODEC_ID_MPEG4 ||
             codec->id == AV_CODEC_ID_VC1 || codec->id == AV_CODEC_ID_WMV3) &&
            (frame->flags & AV_FRAME_FLAG_INTERLACED))
          throw std::runtime_error("progressive validation encountered an interlaced picture");
        int64_t pts = frame->best_effort_timestamp;
        if (pts == AV_NOPTS_VALUE)
          throw std::runtime_error("input has no frame timestamps");
        if (record || pts >= target) {
          OwnedFrame retained(av_frame_clone(frame));
          if (!retained)
            throw std::runtime_error("retain lookahead frame");
          deferred.push_back(std::move(retained));
        }
        av_frame_unref(frame);
        // Feed a bounded number of future pictures before downloading the
        // oldest one. Each clone retains its own surface, PTS and pixel owner;
        // neither identities nor hashes come from the latest decoder frame.
        if (deferred.size() > lookahead)
          consume_oldest();
        if (limit && count == limit && !complete) {
          // Normally release the suffix without downloading it. The optional
          // lifecycle regression instead transfers ownership to the caller.
          return finish_limited_pass();
        }
        continue;
      }
      if (draining)
        throw std::runtime_error("decoder requested input after drain");
      do {
        av_packet_unref(packet);
        result = av_read_frame(input, packet);
      } while (result >= 0 && packet->stream_index != stream);
      if (result == AVERROR_EOF) {
        if (record && live_gap && !live_gap_exercised)
          throw std::runtime_error("no bound pending surface sustained the live gap before input EOF");
        draining = true;
        Check(avcodec_send_packet(decoder, nullptr), "start drain");
      } else {
        Check(result, "read packet");
        if (record && UsesH264TransportDts() &&
            (packet->flags & AV_PKT_FLAG_KEY) != 0) {
          if (packet->pts == AV_NOPTS_VALUE || packet->dts == AV_NOPTS_VALUE)
            throw std::runtime_error("H.264 TS key packet lacks PTS/DTS anchor");
          if (!reference_key_dts.emplace(packet->pts, packet->dts).second)
            throw std::runtime_error("duplicate H.264 TS key packet PTS");
        }
        if (record && live_gap && !live_gap_exercised && count >= 12 &&
            !deferred.empty() && NextPacketIsNonIdr())
          TryLiveGap(deferred);
        Check(avcodec_send_packet(decoder, packet), "send packet");
      }
    }
    if (close_before_tail) {
      if (!draining || !decoder || deferred.size() != lookahead)
        throw std::runtime_error(
            "explicit stream stop requires decoder EOF and the full retained tail");
      // A live VA sync is not an EOF request. Retained AVFrames keep their
      // surfaces/device alive while context teardown explicitly drains the
      // accepted backend tail, before any of those pixels are downloaded.
      avcodec_free_context(&decoder);
      std::printf("explicit stream stop: decoder freed before %zu retained tail downloads\n",
                  deferred.size());
    }
    // Input EOF is not validation EOF: download and count every retained tail
    // picture before checking the container's declared reference frame count.
    while (!deferred.empty()) {
      consume_oldest();
      if (limit && count == limit && !complete)
        return finish_limited_pass();
    }
    if (!record && count != limit)
      throw std::runtime_error("seek drained before the expected frame count");
    return count;
  }

  void Seek(int64_t target) {
    int64_t seek_timestamp = target;
    if (UsesH264TransportDts()) {
      // MPEG-TS binary seeking compares decoding timestamps. A display PTS
      // can land after its IDR when B pictures reorder. Use the preceding
      // reference key packet's DTS, while Decode still compares the original
      // requested PTS and every pixel against the uninterrupted reference.
      auto anchor = reference_key_dts.upper_bound(target);
      if (anchor == reference_key_dts.begin())
        throw std::runtime_error("no H.264 TS key packet anchor at/before target PTS");
      --anchor;
      seek_timestamp = anchor->second;
      std::printf("TS seek target PTS %lld: key packet PTS %lld DTS %lld\n",
                  static_cast<long long>(target),
                  static_cast<long long>(anchor->first),
                  static_cast<long long>(anchor->second));
    }
    Check(av_seek_frame(input, stream, seek_timestamp, AVSEEK_FLAG_BACKWARD), "seek");
    if (!decoder) {
      OpenDecoder();
      if (close_before_tail)
        std::printf("explicit stream reconnect: new decoder context opened\n");
    }
    avcodec_flush_buffers(decoder);
    av_packet_unref(packet);
  }

  std::map<int64_t, Picture>::const_iterator SeekPosition(size_t index) const {
    auto position = reference.cbegin();
    std::advance(position, index);
    if (codec->id != AV_CODEC_ID_MPEG4 ||
        input->streams[stream]->codecpar->profile !=
            AV_PROFILE_MPEG4_ADVANCED_SIMPLE ||
        reference_types.at(position->first) != AV_PICTURE_TYPE_B)
      return position;

    // Display-order B pictures immediately preceding the next random-access I
    // need the older anchor too. A container seek can begin at that I, and the
    // software decoder then omits the same undecodable leading B suffix. Move
    // only this target to the I so the hardware comparison starts at the first
    // frame either decoder can produce from the selected seek point.
    auto next = position;
    while (next != reference.cend() &&
           reference_types.at(next->first) == AV_PICTURE_TYPE_B)
      ++next;
    if (next != reference.cend() && reference_keyframes.at(next->first))
      return next;
    return position;
  }

  void VerifyHeld(const std::vector<HeldPicture> &held, const char *operation) {
    if (held.size() != lookahead)
      throw std::runtime_error("lifecycle pass did not retain the full lookahead");
    for (const auto &picture : held) {
      if (picture.frame->best_effort_timestamp != picture.pts ||
          !(Fingerprint(picture.frame.get()) == picture.expected))
        throw std::runtime_error(std::string("old frame differs after ") +
                                 operation + " at PTS " +
                                 std::to_string(picture.pts));
    }
    std::printf("%s: %zu retained old-frame PTS/SHA-256 digests match\n",
                operation, held.size());
  }

  void Run() {
    std::printf("download lookahead: %zu frames (0 is synchronous)\n", lookahead);
    const size_t count = Decode(true, 0, 0);
    if (count < 60)
      throw std::runtime_error("use an input with at least 60 frames");
    if (retain_old_frames && count < 80)
      throw std::runtime_error("retained-frame regression requires at least 80 frames");
    if (count != expected_frames)
      throw std::runtime_error("reference did not drain the expected frame count");
    std::printf("reference: %zu %s frames drained\n", count,
                software ? "software" : "VA-API");
    if (close_before_tail) {
      auto tail = reference.end();
      std::advance(tail, -static_cast<std::ptrdiff_t>(std::min<size_t>(8, reference.size())));
      for (; tail != reference.end(); ++tail) {
        std::printf("reference-tail PTS=%lld time_base=%d/%d width=%d height=%d SHA256=",
                    static_cast<long long>(tail->first), input->streams[stream]->time_base.num,
                    input->streams[stream]->time_base.den, tail->second.width, tail->second.height);
        for (uint8_t byte : tail->second.digest)
          std::printf("%02x", byte);
        std::putchar('\n');
      }
    }
    if (export_prime || live_gap) {
      // Compare a complete fresh timeline with synchronized reference pixels;
      // PRIME2 changes only how that fresh timeline is read.
      const int64_t first = reference.begin()->first;
      Seek(first);
      const size_t compared = Decode(false, first, count, nullptr, true);
      if (export_prime)
        std::printf("PRIME2 without prior vaSyncSurface: %zu complete-file PTS/SHA-256 digests match\n",
                    compared);
      else
        std::printf("live-gap/no-gap replay: %zu complete-file PTS/SHA-256 digests match\n",
                    compared);
    }
    const size_t indices[] = {count / 2, count / 4, count * 3 / 4, 0};
    size_t pass = 0;
    for (size_t index : indices) {
      auto position = SeekPosition(index);
      const int64_t target = position->first;
      Seek(target);
      std::vector<HeldPicture> held;
      const size_t compared = Decode(false, target, 12,
                                     retain_old_frames ? &held : nullptr);
      std::printf("seek PTS %lld: %zu frame SHA-256 digests match\n",
                  static_cast<long long>(target), compared);
      if (lookahead != 0)
        std::printf("seek left %zu queued pictures undownloaded\n", deferred_at_limit);
      if (retain_old_frames) {
        if (pass % 2 == 0) {
          auto next = SeekPosition(indices[(pass + 1) % 4]);
          Seek(next->first);
          // FFmpeg can defer VA context recreation until post-flush input.
          // Keep all old owners alive while a new timeline actually decodes.
          const size_t new_count = Decode(false, next->first, 1);
          std::printf("post-flush new timeline: %zu frame SHA-256 digest matches\n",
                      new_count);
          VerifyHeld(held, "flush and new input");
        } else {
          // Unlike a flush-only probe, this necessarily destroys the codec
          // context before any held picture is downloaded. Its AVFrame owns
          // the hardware frame/device references needed for that download.
          avcodec_free_context(&decoder);
          VerifyHeld(held, "decoder context teardown");
        }
      }
      ++pass;
    }
  }
};

int main(int argc, char **argv) {
  if (argc < 2 || argc > 11) {
    std::fprintf(stderr, "usage: %s VIDEO_CONTAINER [DRM_DEVICE|--software] [--lookahead 8] [--expected-frames COUNT] [--retain-old-frames] [--export-prime] [--close-before-tail] [--live-gap]\n", argv[0]);
    return 2;
  }
  try {
    Probe probe;
    const char *device = "/dev/dri/renderD128";
    bool mode_selected = false;
    for (int argument = 2; argument < argc; ++argument) {
      if (std::strcmp(argv[argument], "--export-prime") == 0) {
        if (probe.export_prime)
          throw std::runtime_error("--export-prime must be specified once");
        probe.export_prime = true;
      } else if (std::strcmp(argv[argument], "--retain-old-frames") == 0) {
        if (probe.retain_old_frames)
          throw std::runtime_error("--retain-old-frames must be specified once");
        probe.retain_old_frames = true;
      } else if (std::strcmp(argv[argument], "--close-before-tail") == 0) {
        if (probe.close_before_tail)
          throw std::runtime_error("--close-before-tail must be specified once");
        probe.close_before_tail = true;
      } else if (std::strcmp(argv[argument], "--live-gap") == 0) {
        if (probe.live_gap)
          throw std::runtime_error("--live-gap must be specified once");
        probe.live_gap = true;
      } else if (std::strcmp(argv[argument], "--lookahead") == 0) {
        if (probe.lookahead != 0 || argument + 1 == argc ||
            std::strcmp(argv[++argument], "8") != 0)
          throw std::runtime_error("lookahead must be specified once as --lookahead 8");
        probe.lookahead = 8;
      } else if (std::strcmp(argv[argument], "--expected-frames") == 0) {
        if (probe.expected_frames != 0 || argument + 1 == argc)
          throw std::runtime_error(
              "--expected-frames must be specified once with a positive integer");
        probe.expected_frames = ParseExpectedFrames(argv[++argument]);
      } else {
        if (mode_selected)
          throw std::runtime_error("choose only one DRM device or --software");
        mode_selected = true;
        probe.software = std::strcmp(argv[argument], "--software") == 0;
        if (!probe.software) {
          if (argv[argument][0] == '-')
            throw std::runtime_error("unknown probe option");
          device = argv[argument];
        }
      }
    }
    if (probe.export_prime && probe.software)
      throw std::runtime_error("--export-prime requires hardware mode");
    if (probe.close_before_tail && probe.retain_old_frames)
      throw std::runtime_error(
          "--close-before-tail cannot be combined with --retain-old-frames");
    if (probe.close_before_tail && (probe.software || probe.lookahead != 8))
      throw std::runtime_error(
          "--close-before-tail requires hardware mode and --lookahead 8");
    if (probe.retain_old_frames)
      probe.lookahead = 8;
    if (probe.live_gap) {
#if !VA_CHECK_VERSION(1, 9, 0)
      throw std::runtime_error("--live-gap requires VA-API 1.9 headers or newer");
#endif
      if (probe.software || probe.lookahead != 8)
        throw std::runtime_error("--live-gap requires hardware mode and --lookahead 8");
      const char *live = std::getenv("CRYSTALHD_VAAPI_LIVE_H264");
      if (!live || std::strcmp(live, "1") != 0)
        throw std::runtime_error("--live-gap requires CRYSTALHD_VAAPI_LIVE_H264=1");
    }
    probe.Open(argv[1], device);
    probe.Run();
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "seek validation failed: %s\n", error.what());
    return 1;
  }
}
