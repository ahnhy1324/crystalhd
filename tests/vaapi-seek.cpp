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
#include <map>
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

struct Probe {
  AVFormatContext *input = nullptr;
  AVCodecContext *decoder = nullptr;
  AVPacket *packet = av_packet_alloc();
  AVFrame *frame = av_frame_alloc();
  AVFrame *download = av_frame_alloc();
  int stream = -1;
  bool software = false;
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
    const AVCodec *codec = nullptr;
    stream = av_find_best_stream(input, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    Check(stream, "find video");
    if (codec->id != AV_CODEC_ID_H264)
      throw std::runtime_error("this probe requires a seekable H.264 container");
    if (input->streams[stream]->nb_frames <= 0)
      throw std::runtime_error("container must declare a reliable frame count; use the generated MP4 samples");
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
      Check(av_hwdevice_ctx_create(&decoder->hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI,
                                   device, nullptr, 0), "create VA-API device");
    }
    Check(avcodec_open2(decoder, codec, nullptr), "open decoder");
  }

  Picture Fingerprint() {
    AVFrame *pixels = frame;
    if (!software) {
      if (frame->format != AV_PIX_FMT_VAAPI)
        throw std::runtime_error("decoder returned a software frame");
      av_frame_unref(download);
      Check(av_hwframe_transfer_data(download, frame, 0), "download VA-API frame");
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
  size_t Decode(bool record, int64_t target, size_t limit) {
    size_t count = 0;
    bool draining = false;
    auto expected = reference.lower_bound(target);
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
          Picture picture = Fingerprint();
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
        }
        av_frame_unref(frame);
        if (limit && count == limit)
          return count;
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
    if (!record && count != limit)
      throw std::runtime_error("seek drained before the expected frame count");
    return count;
  }

  void Run() {
    const size_t count = Decode(true, 0, 0);
    if (count < 60)
      throw std::runtime_error("use an input with at least 60 frames");
    const int64_t declared = input->streams[stream]->nb_frames;
    if (declared > 0 && count != static_cast<size_t>(declared))
      throw std::runtime_error("reference did not drain the declared frame count");
    std::printf("reference: %zu %s frames drained\n", count,
                software ? "software" : "VA-API");
    const size_t indices[] = {count / 2, count / 4, count * 3 / 4, 0};
    for (size_t index : indices) {
      auto position = reference.begin();
      std::advance(position, index);
      const int64_t target = position->first;
      Check(av_seek_frame(input, stream, target, AVSEEK_FLAG_BACKWARD), "seek");
      avcodec_flush_buffers(decoder);
      av_packet_unref(packet);
      const size_t compared = Decode(false, target, 12);
      std::printf("seek PTS %lld: %zu frame SHA-256 digests match\n",
                  static_cast<long long>(target), compared);
    }
  }
};

int main(int argc, char **argv) {
  if (argc < 2 || argc > 3) {
    std::fprintf(stderr, "usage: %s VIDEO.mp4 [DRM_DEVICE|--software]\n", argv[0]);
    return 2;
  }
  try {
    Probe probe;
    probe.software = argc == 3 && std::strcmp(argv[2], "--software") == 0;
    probe.Open(argv[1], argc == 3 && !probe.software ? argv[2] : "/dev/dri/renderD128");
    probe.Run();
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "seek validation failed: %s\n", error.what());
    return 1;
  }
}
