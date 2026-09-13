// SPDX-License-Identifier: LGPL-2.1-or-later
// Compare actual downloaded pixels after avcodec_flush_buffers()/seek with
// the same decoder's uninterrupted output. Timestamps alone miss stale frames.
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/sha.h>
}

#include <array>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

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
    if (codec->id != AV_CODEC_ID_H264)
      throw std::runtime_error("this probe requires a seekable H.264 container");
    if (input->streams[stream]->nb_frames <= 0)
      throw std::runtime_error("container must declare a reliable frame count; use the generated MP4 samples");
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
      decoder->get_format = ChooseHardware;
      if (lookahead != 0)
        decoder->extra_hw_frames = static_cast<int>(lookahead);
      Check(av_hwdevice_ctx_create(&decoder->hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI,
                                   device_path.c_str(), nullptr, 0), "create VA-API device");
    }
    Check(avcodec_open2(decoder, codec, nullptr), "open decoder");
  }

  Picture Fingerprint(const AVFrame *picture_frame) {
    const AVFrame *pixels = picture_frame;
    if (!software) {
      if (picture_frame->format != AV_PIX_FMT_VAAPI)
        throw std::runtime_error("decoder returned a software frame");
      av_frame_unref(download);
      Check(av_hwframe_transfer_data(download, picture_frame, 0), "download VA-API frame");
      if (download->format != AV_PIX_FMT_NV12)
        throw std::runtime_error("expected NV12 download");
      pixels = download;
    }
    const auto format = static_cast<AVPixelFormat>(pixels->format);
    int size = av_image_get_buffer_size(format, pixels->width, pixels->height, 1);
    Check(size, "size pixels");
    std::vector<uint8_t> packed(size);
    Check(av_image_copy_to_buffer(packed.data(), size, pixels->data,
                                 pixels->linesize, format, pixels->width,
                                 pixels->height, 1), "pack pixels");
    Picture picture{pixels->width, pixels->height, pixels->format, {}};
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
                std::vector<HeldPicture> *held = nullptr) {
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
      Picture picture = Fingerprint(picture_frame.get());
      if (record) {
        if (!reference.emplace(pts, picture).second)
          throw std::runtime_error("duplicate reference timestamp");
      } else {
        if (expected == reference.end() || expected->first != pts ||
            !(expected->second == picture))
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
        if (limit && count == limit) {
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
      if (limit && count == limit)
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
  if (argc < 2 || argc > 6) {
    std::fprintf(stderr, "usage: %s VIDEO.mp4 [DRM_DEVICE|--software] [--lookahead 8] [--retain-old-frames]\n", argv[0]);
    return 2;
  }
  try {
    Probe probe;
    const char *device = "/dev/dri/renderD128";
    bool mode_selected = false;
    for (int argument = 2; argument < argc; ++argument) {
      if (std::strcmp(argv[argument], "--retain-old-frames") == 0) {
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
