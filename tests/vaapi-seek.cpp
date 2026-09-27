// SPDX-License-Identifier: LGPL-2.1-or-later
// Compare actual downloaded pixels after avcodec_flush_buffers()/seek with
// the same decoder's uninterrupted output. Timestamps alone miss stale frames.
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libavutil/imgutils.h>
#include <libavutil/sha.h>
}

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <deque>
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
  size_t lookahead = 0;
  size_t deferred_at_limit = 0;
  std::map<int64_t, Picture> reference;

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
    if (codec->id != AV_CODEC_ID_H264 && codec->id != AV_CODEC_ID_MPEG2VIDEO)
      throw std::runtime_error("this probe requires a seekable H.264 or progressive MPEG-2 container");
    if (codec->id == AV_CODEC_ID_MPEG2VIDEO) {
      const AVCodecParameters *parameters = input->streams[stream]->codecpar;
      if (parameters->profile != FF_PROFILE_MPEG2_SIMPLE &&
          parameters->profile != FF_PROFILE_MPEG2_MAIN)
        throw std::runtime_error("MPEG-2 validation requires Simple or Main profile");
      if (parameters->field_order != AV_FIELD_PROGRESSIVE)
        throw std::runtime_error("MPEG-2 validation requires a declared progressive stream");
    }
    if (input->streams[stream]->nb_frames <= 0)
      throw std::runtime_error("container must declare a reliable frame count; use generated MP4/MOV samples");
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
        if (codec->id == AV_CODEC_ID_MPEG2VIDEO &&
            (frame->flags & AV_FRAME_FLAG_INTERLACED))
          throw std::runtime_error("MPEG-2 validation encountered an interlaced picture");
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
        draining = true;
        Check(avcodec_send_packet(decoder, nullptr), "start drain");
      } else {
        Check(result, "read packet");
        Check(avcodec_send_packet(decoder, packet), "send packet");
      }
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
    Check(av_seek_frame(input, stream, target, AVSEEK_FLAG_BACKWARD), "seek");
    if (!decoder)
      OpenDecoder();
    avcodec_flush_buffers(decoder);
    av_packet_unref(packet);
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
    const int64_t declared = input->streams[stream]->nb_frames;
    if (declared > 0 && count != static_cast<size_t>(declared))
      throw std::runtime_error("reference did not drain the declared frame count");
    std::printf("reference: %zu %s frames drained\n", count,
                software ? "software" : "VA-API");
    if (export_prime) {
      // Never let stale exported pixels become their own reference. The first
      // pass above uses synchronized downloads; this pass exports fresh frames.
      const int64_t first = reference.begin()->first;
      Seek(first);
      const size_t compared = Decode(false, first, count, nullptr, true);
      std::printf("PRIME2 without prior vaSyncSurface: %zu complete-file PTS/SHA-256 digests match\n",
                  compared);
    }
    const size_t indices[] = {count / 2, count / 4, count * 3 / 4, 0};
    size_t pass = 0;
    for (size_t index : indices) {
      auto position = reference.begin();
      std::advance(position, index);
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
          auto next = reference.begin();
          std::advance(next, indices[(pass + 1) % 4]);
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
  if (argc < 2 || argc > 7) {
    std::fprintf(stderr, "usage: %s VIDEO_CONTAINER [DRM_DEVICE|--software] [--lookahead 8] [--retain-old-frames] [--export-prime]\n", argv[0]);
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
      } else if (std::strcmp(argv[argument], "--lookahead") == 0) {
        if (probe.lookahead != 0 || argument + 1 == argc ||
            std::strcmp(argv[++argument], "8") != 0)
          throw std::runtime_error("lookahead must be specified once as --lookahead 8");
        probe.lookahead = 8;
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
    if (probe.retain_old_frames)
      probe.lookahead = 8;
    probe.Open(argv[1], device);
    probe.Run();
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "seek validation failed: %s\n", error.what());
    return 1;
  }
}
