// SPDX-License-Identifier: LGPL-2.1-or-later
// OWN diagnostic client; external FFmpeg/VA implementations remain unchanged.
// Public feed/drain and retained-tail ownership follow tests/vaapi-seek.cpp.
// There is no presentation clock, renderer, BWDIF graph or caller drop feedback.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/version.h>
#include <libavformat/avformat.h>
#include <libavformat/version.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libavutil/imgutils.h>
#include <libavutil/version.h>
}
#include <va/va.h>

#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <csignal>
#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <time.h>
#include <unistd.h>

#if LIBAVCODEC_VERSION_INT != AV_VERSION_INT(63, 1, 102) || \
    LIBAVFORMAT_VERSION_INT != AV_VERSION_INT(63, 1, 102) || \
    LIBAVUTIL_VERSION_INT != AV_VERSION_INT(61, 1, 102)
#error "Use unchanged original 12cc codec63/format63/util61 public headers"
#endif

namespace {
constexpr unsigned kExpected = 900;
constexpr unsigned kLookahead = 8;
constexpr unsigned kMaxDemuxPackets = 4096;
constexpr uint64_t kDeadlineNs = 150ULL * 1000 * 1000 * 1000;
constexpr uint64_t kSyncNs = 100ULL * 1000 * 1000;
constexpr const char* kFormat = "/app/lib/libavformat.so.63.1.102";
constexpr const char* kCodec = "/app/lib/libavcodec.so.63.1.102";
constexpr const char* kUtil = "/app/lib/libavutil.so.61.1.102";
constexpr const char* kVa = "/usr/lib/x86_64-linux-gnu/libva.so.2";
volatile sig_atomic_t stopped = 0;
uint64_t deadline = 0;

#define DECL(name) static decltype(&name) fn_##name = nullptr
DECL(avformat_version); DECL(avcodec_version); DECL(avutil_version);
DECL(avformat_alloc_context); DECL(avformat_open_input);
DECL(avformat_find_stream_info); DECL(av_find_best_stream);
DECL(av_read_frame); DECL(avformat_close_input);
DECL(avcodec_alloc_context3); DECL(avcodec_parameters_to_context);
DECL(avcodec_open2); DECL(avcodec_send_packet); DECL(avcodec_receive_frame);
DECL(avcodec_free_context); DECL(av_packet_alloc); DECL(av_packet_free);
DECL(av_packet_unref); DECL(av_frame_alloc); DECL(av_frame_free);
DECL(av_frame_clone); DECL(av_frame_unref); DECL(av_hwdevice_ctx_create);
DECL(av_hwframe_transfer_data); DECL(av_image_get_buffer_size); DECL(av_strerror);
DECL(vaSyncSurface2);
#undef DECL
#define AV(name) fn_##name

uint64_t Clock(clockid_t which) {
  timespec value{};
  if (clock_gettime(which, &value) != 0)
    throw std::runtime_error("clock_gettime failed");
  return static_cast<uint64_t>(value.tv_sec) * 1000000000ULL + value.tv_nsec;
}
void Signal(int signal) { stopped = signal; }
int Interrupted(void*) {
  timespec value{};
  if (stopped || clock_gettime(CLOCK_MONOTONIC, &value) != 0)
    return 1;
  const auto now = static_cast<uint64_t>(value.tv_sec) * 1000000000ULL + value.tv_nsec;
  return deadline != 0 && now >= deadline;
}
void CheckStop() {
  if (Interrupted(nullptr))
    throw std::runtime_error(stopped ? "termination requested" : "150-second client deadline");
}
void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
void Check(int result, const char* operation) {
  if (result >= 0) return;
  char error[AV_ERROR_MAX_STRING_SIZE]{};
  if (AV(av_strerror)) AV(av_strerror)(result, error, sizeof(error));
  throw std::runtime_error(std::string(operation) + ": " + error);
}
std::string Quote(const std::string& value) {
  std::string result = "\"";
  for (unsigned char byte : value) {
    if (byte == '"' || byte == '\\') { result += '\\'; result += static_cast<char>(byte); }
    else if (byte < 32 || byte >= 127) {
      char escape[7]; std::snprintf(escape, sizeof(escape), "\\u%04x", byte); result += escape;
    } else result += static_cast<char>(byte);
  }
  return result + '"';
}

struct Report {
  FILE* output = nullptr;
  explicit Report(const char* path) {
    const int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) throw std::runtime_error("report must be a fresh exclusive file");
    output = fdopen(fd, "w");
    if (!output) { close(fd); throw std::runtime_error("open report stream"); }
  }
  ~Report() { if (output) std::fclose(output); }
  __attribute__((format(printf, 2, 3))) void Row(const char* format, ...) {
    va_list args; va_start(args, format);
    const int result = std::vfprintf(output, format, args);
    va_end(args);
    if (result < 0 || std::fputc('\n', output) == EOF || std::fflush(output) != 0)
      throw std::runtime_error("write diagnostic report");
  }
};

void* Library(const char* path) {
  void* handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (!handle) throw std::runtime_error(std::string("original runtime closure: ") + dlerror());
  // Keep provider handles alive until process exit, including all frame cleanup.
  return handle;
}
void* Symbol(void* handle, const char* name, const char* version, const char* provider) {
  void* symbol = version ? dlvsym(handle, name, version) : dlsym(handle, name);
  Dl_info info{}; char actual[PATH_MAX]{}, expected[PATH_MAX]{};
  Require(symbol && dladdr(symbol, &info) && info.dli_fname &&
          realpath(info.dli_fname, actual) && realpath(provider, expected) &&
          std::strcmp(actual, expected) == 0, "symbol outside exact original provider");
  return symbol;
}
#define LOAD(handle, name, version, path) do { \
  void* address = Symbol(handle, #name, version, path); \
  static_assert(sizeof(address) == sizeof(AV(name)), "ELF function pointer size"); \
  std::memcpy(&AV(name), &address, sizeof(address)); \
} while (false)
void Providers(Report& report) {
  void* util = Library(kUtil);
  void* codec = Library(kCodec);
  void* format = Library(kFormat);
  void* va = Library(kVa);
#define UTIL(name) LOAD(util, name, "LIBAVUTIL_61", kUtil)
  UTIL(avutil_version); UTIL(av_frame_alloc); UTIL(av_frame_free);
  UTIL(av_frame_clone); UTIL(av_frame_unref); UTIL(av_hwdevice_ctx_create);
  UTIL(av_hwframe_transfer_data); UTIL(av_image_get_buffer_size); UTIL(av_strerror);
#undef UTIL
#define CODEC(name) LOAD(codec, name, "LIBAVCODEC_63", kCodec)
  CODEC(avcodec_version); CODEC(avcodec_alloc_context3); CODEC(avcodec_parameters_to_context);
  CODEC(avcodec_open2); CODEC(avcodec_send_packet); CODEC(avcodec_receive_frame);
  CODEC(avcodec_free_context); CODEC(av_packet_alloc); CODEC(av_packet_free); CODEC(av_packet_unref);
#undef CODEC
#define FORMAT(name) LOAD(format, name, "LIBAVFORMAT_63", kFormat)
  FORMAT(avformat_version); FORMAT(avformat_alloc_context); FORMAT(avformat_open_input);
  FORMAT(avformat_find_stream_info); FORMAT(av_find_best_stream); FORMAT(av_read_frame);
  FORMAT(avformat_close_input);
#undef FORMAT
  LOAD(va, vaSyncSurface2, nullptr, kVa);
  Require(AV(avformat_version)() == LIBAVFORMAT_VERSION_INT &&
          AV(avcodec_version)() == LIBAVCODEC_VERSION_INT &&
          AV(avutil_version)() == LIBAVUTIL_VERSION_INT, "original runtime version mismatch");
  char va_path[PATH_MAX]{};
  Require(realpath(kVa, va_path), "canonical platform VA provider missing");
  report.Row("{\"type\":\"providers\",\"format\":%s,\"codec\":%s,\"util\":%s,\"va\":%s,"
             "\"format_version\":%u,\"codec_version\":%u,\"util_version\":%u}",
             Quote(kFormat).c_str(), Quote(kCodec).c_str(), Quote(kUtil).c_str(),
             Quote(va_path).c_str(), AV(avformat_version)(), AV(avcodec_version)(), AV(avutil_version)());
}
#undef LOAD

AVPixelFormat ChooseHardware(AVCodecContext*, const AVPixelFormat* formats) {
  for (; *formats != AV_PIX_FMT_NONE; ++formats)
    if (*formats == AV_PIX_FMT_VAAPI) return *formats;
  return AV_PIX_FMT_NONE;
}
struct FreeFrame { void operator()(AVFrame* p) const { if (p) AV(av_frame_free)(&p); } };
using Frame = std::unique_ptr<AVFrame, FreeFrame>;

struct Probe {
  Report& report;
  AVFormatContext* input = nullptr;
  AVCodecContext* decoder = nullptr;
  AVPacket* packet = nullptr;
  Frame frame, download;
  std::deque<Frame> held;
  // Fixed fixture bound: at most900 scalar labels in each set, no frame map.
  std::set<int64_t> accepted_labels, returned_labels;
  int stream = -1;
  unsigned reads = 0, video_reads = 0, attempts = 0, accepted = 0;
  unsigned returned = 0, completed = 0, max_held = 0, drain_attempts = 0;
  bool decoder_eof = false, demux_eof = false, drain_accepted = false;
  bool closed_before_tail = false, resources_released = false;
  explicit Probe(Report& r) : report(r) {}
  ~Probe() { Close(); }
  void Close() {
    // Match the explicit retained-tail close ordering even on cancellation.
    if (decoder) AV(avcodec_free_context)(&decoder);
    held.clear(); frame.reset(); download.reset();
    if (packet) AV(av_packet_free)(&packet);
    if (input) AV(avformat_close_input)(&input);
    resources_released = true;
  }
  void DefaultDiscard() const {
    Require(decoder && decoder->skip_frame == AVDISCARD_DEFAULT &&
            decoder->skip_idct == AVDISCARD_DEFAULT &&
            decoder->skip_loop_filter == AVDISCARD_DEFAULT,
            "decoder discard fields changed from DEFAULT");
  }
  void Open(const char* path, const char* device) {
    input = AV(avformat_alloc_context)();
    Require(input, "allocate demux context");
    input->interrupt_callback = {Interrupted, nullptr};
    Check(AV(avformat_open_input)(&input, path, nullptr, nullptr), "open source");
    CheckStop();
    Check(AV(avformat_find_stream_info)(input, nullptr), "discover source");
    const AVCodec* codec = nullptr;
    stream = AV(av_find_best_stream)(input, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    Check(stream, "find source video");
    const AVCodecParameters* parameters = input->streams[stream]->codecpar;
    Require(codec && codec->id == AV_CODEC_ID_H264 && parameters->width == 1920 &&
            parameters->height == 1080 && input->iformat &&
            std::strcmp(input->iformat->name, "mpegts") == 0,
            "expected original 1920x1080 H264 MPEG-TS fixture");
    decoder = AV(avcodec_alloc_context3)(codec);
    Require(decoder, "allocate original decoder");
    Check(AV(avcodec_parameters_to_context)(decoder, parameters), "copy source parameters");
    decoder->pkt_timebase = input->streams[stream]->time_base;
    decoder->thread_count = 1;
    decoder->get_format = ChooseHardware;
    decoder->extra_hw_frames = static_cast<int>(kLookahead);
    decoder->skip_frame = AVDISCARD_DEFAULT;
    decoder->skip_idct = AVDISCARD_DEFAULT;
    decoder->skip_loop_filter = AVDISCARD_DEFAULT;
    Check(AV(av_hwdevice_ctx_create)(&decoder->hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI,
                                   device, nullptr, 0), "create original VA device");
    Check(AV(avcodec_open2)(decoder, codec, nullptr), "open original H264 decoder");
    DefaultDiscard();
    packet = AV(av_packet_alloc)(); frame.reset(AV(av_frame_alloc)());
    download.reset(AV(av_frame_alloc)());
    Require(packet && frame && download, "allocate bounded client buffers");
    report.Row("{\"type\":\"stream\",\"stream_index\":%d,\"stream_id\":%d,"
               "\"time_base\":[%d,%d],\"start_time\":%lld,\"width\":%d,\"height\":%d,"
               "\"coded_width\":%d,\"coded_height\":%d,\"thread_count\":%d,\"extra_hw_frames\":%d}",
               stream, input->streams[stream]->id, decoder->pkt_timebase.num,
               decoder->pkt_timebase.den, static_cast<long long>(input->streams[stream]->start_time),
               decoder->width, decoder->height, decoder->coded_width, decoder->coded_height,
               decoder->thread_count, decoder->extra_hw_frames);
  }
  void CompleteOldest() {
    CheckStop();
    const AVFrame* picture = held.front().get();
    Require(picture->format == AV_PIX_FMT_VAAPI && picture->hw_frames_ctx,
            "software fallback or missing hardware owner");
    auto* frames = reinterpret_cast<AVHWFramesContext*>(picture->hw_frames_ctx->data);
    Require(frames && frames->device_ctx && frames->device_ctx->type == AV_HWDEVICE_TYPE_VAAPI,
            "retained frame lost VA device owner");
    auto* device = static_cast<AVVAAPIDeviceContext*>(frames->device_ctx->hwctx);
    Require(device && device->display, "retained frame has no VA display");
    const auto surface = static_cast<VASurfaceID>(reinterpret_cast<uintptr_t>(picture->data[3]));
    // Eight later returned pictures have already fed future-reference work.
    // This finite readiness call is not a fake decoder EOF or an ownership bypass.
    const VAStatus ready = AV(vaSyncSurface2)(device->display, surface, kSyncNs);
    if (ready != VA_STATUS_SUCCESS) {
      report.Row("{\"type\":\"readiness_failure\",\"returned_ordinal\":%u,\"surface\":%u,"
                 "\"status\":%d,\"retained\":%zu,\"decoder_closed\":%s}",
                 completed + 1, surface, ready, held.size(), closed_before_tail ? "true" : "false");
      throw std::runtime_error("bounded retained-frame readiness failed");
    }
    AV(av_frame_unref)(download.get());
    Check(AV(av_hwframe_transfer_data)(download.get(), picture, 0), "download ready VA frame");
    Require(download->format == AV_PIX_FMT_NV12 && download->width >= picture->width &&
            download->height >= picture->height && download->data[0] && download->data[1],
            "unexpected downloaded NV12 geometry");
    const int bytes = AV(av_image_get_buffer_size)(AV_PIX_FMT_NV12, download->width, download->height, 1);
    Check(bytes, "download geometry size");
    ++completed;
    report.Row("{\"type\":\"completed_frame\",\"ordinal\":%u,\"pts\":%lld,"
               "\"best_effort_timestamp\":%lld,\"pkt_dts\":%lld,\"time_base\":[%d,%d],"
               "\"format\":%d,\"width\":%d,\"height\":%d,\"flags\":%d,\"decode_error_flags\":%d,"
               "\"crop\":[%zu,%zu,%zu,%zu],\"surface\":%u,\"ready_status\":%d,"
               "\"download_format\":%d,\"download_width\":%d,\"download_height\":%d,"
               "\"download_active_bytes\":%d,\"decoder_closed\":%s}",
               completed, static_cast<long long>(picture->pts),
               static_cast<long long>(picture->best_effort_timestamp), static_cast<long long>(picture->pkt_dts),
               picture->time_base.num, picture->time_base.den, picture->format, picture->width,
               picture->height, picture->flags, picture->decode_error_flags,
               picture->crop_top, picture->crop_bottom,
               picture->crop_left, picture->crop_right, surface, ready, download->format,
               download->width, download->height, bytes, closed_before_tail ? "true" : "false");
    AV(av_frame_unref)(download.get());
    held.pop_front();
  }
  unsigned Receive() {
    unsigned progress = 0;
    for (;;) {
      CheckStop(); DefaultDiscard();
      const int result = AV(avcodec_receive_frame)(decoder, frame.get());
      DefaultDiscard();
      if (result == AVERROR(EAGAIN)) return progress;
      if (result == AVERROR_EOF) { decoder_eof = true; return progress; }
      Check(result, "receive original frame");
      Require(frame->format == AV_PIX_FMT_VAAPI, "software fallback rejected");
      Require(frame->width == 1920 && frame->height == 1080,
              "selected decoded frame geometry changed");
      if ((frame->flags & AV_FRAME_FLAG_CORRUPT) || frame->decode_error_flags != 0) {
        report.Row("{\"type\":\"rejected_frame\",\"ordinal\":%u,\"pts\":%lld,"
                   "\"best_effort_timestamp\":%lld,\"flags\":%d,\"decode_error_flags\":%d}",
                   returned + 1, static_cast<long long>(frame->pts),
                   static_cast<long long>(frame->best_effort_timestamp),
                   frame->flags, frame->decode_error_flags);
        throw std::runtime_error("corrupt or decode-error frame rejected");
      }
      Require(++returned <= kExpected, "more than900 returned source pictures");
      Require(frame->best_effort_timestamp != AV_NOPTS_VALUE &&
              returned_labels.insert(frame->best_effort_timestamp).second,
              "missing or duplicate returned raw picture label");
      Frame copy(AV(av_frame_clone)(frame.get()));
      Require(copy != nullptr, "retain bounded VA frame");
      held.push_back(std::move(copy));
      AV(av_frame_unref)(frame.get());
      if (held.size() > max_held) max_held = static_cast<unsigned>(held.size());
      Require(held.size() <= kLookahead + 1, "bounded hardware lookahead overflow");
      if (held.size() > kLookahead) CompleteOldest();
      ++progress;
    }
  }
  void Send(const AVPacket* value) {
    // A send EAGAIN retains this exact packet; receive and retry it unchanged.
    for (unsigned retry = 0; retry < 16; ++retry) {
      CheckStop(); DefaultDiscard();
      const int result = AV(avcodec_send_packet)(decoder, value);
      DefaultDiscard();
      if (value) ++attempts; else ++drain_attempts;
      report.Row("{\"type\":\"send\",\"packet_ordinal\":%u,\"drain\":%s,\"ret\":%d,"
                 "\"pts\":%lld,\"dts\":%lld,\"duration\":%lld,\"size\":%d,\"flags\":%d,"
                 "\"discard\":[%d,%d,%d]}",
                 value ? video_reads : 0, value ? "false" : "true", result,
                 static_cast<long long>(value ? value->pts : AV_NOPTS_VALUE),
                 static_cast<long long>(value ? value->dts : AV_NOPTS_VALUE),
                 static_cast<long long>(value ? value->duration : 0), value ? value->size : 0,
                 value ? value->flags : 0, decoder->skip_frame, decoder->skip_idct, decoder->skip_loop_filter);
      if (result == AVERROR(EAGAIN)) {
        Require(Receive() != 0 && !decoder_eof, "send/receive EAGAIN made no progress");
        continue;
      }
      Check(result, value ? "send source packet" : "accept explicit decoder drain");
      if (value) {
        Require(value->pts != AV_NOPTS_VALUE && accepted_labels.insert(value->pts).second,
                "missing or duplicate accepted raw packet label");
        ++accepted;
      } else drain_accepted = true;
      return;
    }
    throw std::runtime_error("bounded send retry exhausted");
  }
  void Run() {
    for (;;) {
      CheckStop();
      AV(av_packet_unref)(packet);
      const int result = AV(av_read_frame)(input, packet);
      if (result == AVERROR_EOF) { demux_eof = true; break; }
      Check(result, "read source packet");
      Require(++reads <= kMaxDemuxPackets, "bounded demux packet census exceeded");
      if (packet->stream_index != stream) continue;
      Require(packet->data && packet->size > 0, "unexpected empty selected source packet");
      Require(++video_reads <= kExpected, "more than900 video source packets");
      Send(packet);
      Receive();
      Require(!decoder_eof, "decoder EOF before source EOF");
    }
    AV(av_packet_unref)(packet);
    Send(nullptr);
    Receive();
    Require(decoder_eof, "decoder requested more input after accepted drain");
    // Public decoder EOF does not itself guarantee native pending tail ready.
    // Free the decoder explicitly, while retained AVFrames keep surface/device
    // ownership, matching existing --close-before-tail diagnostic coverage.
    AV(avcodec_free_context)(&decoder);
    closed_before_tail = true;
    while (!held.empty()) CompleteOldest();
    Require(demux_eof && drain_accepted && video_reads == kExpected &&
            accepted == kExpected && returned == kExpected && completed == kExpected,
            "unchanged900 source population criterion failed");
    Require(accepted_labels.size() == kExpected && returned_labels.size() == kExpected &&
            accepted_labels == returned_labels,
            "accepted/returned900 raw label sets differ");
  }
  void Counts(const char* status, const std::string& error, uint64_t wall, uint64_t cpu) {
    report.Row("{\"type\":\"result\",\"status\":%s,\"error\":%s,\"pid\":%ld,"
               "\"expected\":%u,\"demux_packets\":%u,\"video_packets\":%u,\"send_attempts\":%u,"
               "\"accepted_packets\":%u,\"returned_vaapi_frames\":%u,\"completed_downloads\":%u,"
               "\"unique_accepted_labels\":%zu,\"unique_returned_labels\":%zu,\"raw_label_sets_equal\":%s,"
               "\"max_retained_frames\":%u,\"demux_eof\":%s,\"drain_attempts\":%u,"
               "\"drain_accepted\":%s,\"decoder_eof\":%s,\"decoder_closed_before_tail\":%s,"
               "\"client_resources_released\":%s,"
               "\"elapsed_wall_ns\":%llu,\"calling_thread_cpu_ns\":%llu,\"signal\":%d,"
               "\"stock_realtime_qualification\":false,\"bwdif_tail_qualification\":false}",
               Quote(status).c_str(), Quote(error).c_str(), static_cast<long>(getpid()), kExpected,
               reads, video_reads, attempts, accepted, returned, completed,
               accepted_labels.size(), returned_labels.size(),
               accepted_labels == returned_labels ? "true" : "false", max_held,
               demux_eof ? "true" : "false", drain_attempts, drain_accepted ? "true" : "false",
               decoder_eof ? "true" : "false", closed_before_tail ? "true" : "false",
               resources_released ? "true" : "false",
               static_cast<unsigned long long>(wall), static_cast<unsigned long long>(cpu),
               static_cast<int>(stopped));
  }
};
} // namespace

int main(int argc, char** argv) {
  if (argc != 4) {
    std::fprintf(stderr, "usage: %s ORIGINAL_TS VA_DEVICE FRESH_JSONL_REPORT\n", argv[0]);
    return 2;
  }
  try {
    Report report(argv[3]);
    struct sigaction action{}; action.sa_handler = Signal; sigemptyset(&action.sa_mask);
    Require(sigaction(SIGTERM, &action, nullptr) == 0 && sigaction(SIGINT, &action, nullptr) == 0,
            "install normal cleanup signal handlers");
    const uint64_t start = Clock(CLOCK_MONOTONIC), cpu = Clock(CLOCK_THREAD_CPUTIME_ID);
    deadline = start + kDeadlineNs;
    report.Row("{\"type\":\"header\",\"schema\":\"crystalhd-original-decode900-v1\","
               "\"pid\":%ld,\"input\":%s,\"device\":%s,\"expected\":%u,\"lookahead\":%u,"
               "\"deadline_ns\":%llu,\"readiness_timeout_ns\":%llu,\"discard\":\"DEFAULT\","
               "\"scope\":\"OWN_NO_CLOCK_OR_RENDERER_COUNTERFACTUAL_UNMODIFIED_ORIGINAL_PROVIDERS\"}",
               static_cast<long>(getpid()), Quote(argv[1]).c_str(), Quote(argv[2]).c_str(),
               kExpected, kLookahead, static_cast<unsigned long long>(kDeadlineNs),
               static_cast<unsigned long long>(kSyncNs));
    Providers(report);
    Probe probe(report);
    try {
      probe.Open(argv[1], argv[2]);
      probe.Run();
      probe.Close();
      probe.Counts("PASS_DIAGNOSTIC_900_ORIGINALS", "", Clock(CLOCK_MONOTONIC) - start,
                   Clock(CLOCK_THREAD_CPUTIME_ID) - cpu);
      return 0;
    } catch (const std::exception& error) {
      probe.Close();
      probe.Counts(stopped ? "CANCELLED" : "FAIL", error.what(), Clock(CLOCK_MONOTONIC) - start,
                   Clock(CLOCK_THREAD_CPUTIME_ID) - cpu);
      std::fprintf(stderr, "OWN no-clock decode900: %s\n", error.what());
      return stopped ? 128 + static_cast<int>(stopped) : 1;
    }
  } catch (const std::exception& error) {
    std::fprintf(stderr, "OWN no-clock decode900 setup/report: %s\n", error.what());
    return 2;
  }
}
