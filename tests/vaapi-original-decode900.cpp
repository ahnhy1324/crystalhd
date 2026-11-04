// SPDX-License-Identifier: LGPL-2.1-or-later
// OWN diagnostic client; external FFmpeg/VA implementations remain unchanged.
// Public feed/drain and retained-tail ownership follow tests/vaapi-seek.cpp.
// There is no presentation clock, renderer or caller drop feedback. Optional
// BWDIF compares an unflushed prefix with an explicitly reference-only EOF.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/version.h>
#include <libavformat/avformat.h>
#include <libavformat/version.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/version.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
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
#include <map>
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
    LIBAVUTIL_VERSION_INT != AV_VERSION_INT(61, 1, 102) || \
    LIBAVFILTER_VERSION_INT != AV_VERSION_INT(12, 1, 102)
#error "Use unchanged original 12cc codec63/format63/util61/filter12 public headers"
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
constexpr const char* kFilter = "/app/lib/libavfilter.so.12.1.102";
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
DECL(av_frame_copy_props); DECL(av_frame_get_buffer); DECL(av_rescale_q);
DECL(av_strdup); DECL(av_opt_set_array);
DECL(avfilter_version); DECL(avfilter_get_by_name); DECL(avfilter_graph_alloc);
DECL(avfilter_graph_free); DECL(avfilter_graph_create_filter); DECL(avfilter_graph_alloc_filter);
DECL(avfilter_init_str); DECL(avfilter_inout_alloc); DECL(avfilter_inout_free);
DECL(avfilter_graph_parse_ptr); DECL(avfilter_graph_config);
DECL(av_buffersrc_add_frame); DECL(av_buffersrc_close); DECL(av_buffersink_get_frame);
DECL(av_buffersink_get_time_base); DECL(av_buffersink_get_frame_rate);
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
struct Metric { uint64_t calls = 0, wall_ns = 0, main_cpu_ns = 0; };
template<typename Call> auto Measure(Metric& metric, Call call) -> decltype(call()) {
  const uint64_t wall = Clock(CLOCK_MONOTONIC), cpu = Clock(CLOCK_THREAD_CPUTIME_ID);
  const auto result = call();
  const uint64_t end_cpu = Clock(CLOCK_THREAD_CPUTIME_ID), end_wall = Clock(CLOCK_MONOTONIC);
  Require(end_cpu >= cpu && end_wall >= wall, "per-clock call timing regressed");
  ++metric.calls; metric.wall_ns += end_wall - wall; metric.main_cpu_ns += end_cpu - cpu;
  // Independent clocks are retained raw; no CPU<=wall clamp or inference.
  return result;
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
void Providers(Report& report, bool bwdif) {
  void* util = Library(kUtil);
  void* codec = Library(kCodec);
  void* format = Library(kFormat);
  void* va = Library(kVa);
#define UTIL(name) LOAD(util, name, "LIBAVUTIL_61", kUtil)
  UTIL(avutil_version); UTIL(av_frame_alloc); UTIL(av_frame_free);
  UTIL(av_frame_clone); UTIL(av_frame_unref); UTIL(av_hwdevice_ctx_create);
  UTIL(av_hwframe_transfer_data); UTIL(av_image_get_buffer_size); UTIL(av_strerror);
  UTIL(av_frame_copy_props);
  if (bwdif) {
    UTIL(av_frame_get_buffer); UTIL(av_rescale_q); UTIL(av_strdup); UTIL(av_opt_set_array);
  }
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
  if (bwdif) {
    void* filter = Library(kFilter);
#define FILTER(name) LOAD(filter, name, "LIBAVFILTER_12", kFilter)
    FILTER(avfilter_version); FILTER(avfilter_get_by_name); FILTER(avfilter_graph_alloc);
    FILTER(avfilter_graph_free); FILTER(avfilter_graph_create_filter); FILTER(avfilter_graph_alloc_filter);
    FILTER(avfilter_init_str); FILTER(avfilter_inout_alloc); FILTER(avfilter_inout_free);
    FILTER(avfilter_graph_parse_ptr); FILTER(avfilter_graph_config);
    FILTER(av_buffersrc_add_frame); FILTER(av_buffersrc_close); FILTER(av_buffersink_get_frame);
    FILTER(av_buffersink_get_time_base); FILTER(av_buffersink_get_frame_rate);
#undef FILTER
    Require(AV(avfilter_version)() == LIBAVFILTER_VERSION_INT, "original filter version mismatch");
    report.Row("{\"type\":\"filter_provider\",\"path\":%s,\"version\":%u}",
               Quote(kFilter).c_str(), AV(avfilter_version)());
  }
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
  const bool bwdif;
  AVFilterGraph* graph = nullptr;
  AVFilterContext* filter_source = nullptr;
  AVFilterContext* filter_sink = nullptr;
  Frame filter_input, filter_output;
  std::map<int64_t, unsigned> filter_labels;
  unsigned filter_inputs = 0, filter_outputs = 0, unflushed_outputs = 0;
  bool filter_source_eof = false, filter_sink_eof = false;
  Metric send_metric, receive_metric, sync_metric, download_metric;
  Metric filter_add_metric, filter_sink_metric, filter_close_metric;
  explicit Probe(Report& r, bool enable_bwdif) : report(r), bwdif(enable_bwdif) {}
  ~Probe() { Close(); }
  void Close() {
    // Match the explicit retained-tail close ordering even on cancellation.
    if (decoder) AV(avcodec_free_context)(&decoder);
    held.clear(); frame.reset(); download.reset();
    filter_input.reset(); filter_output.reset();
    if (graph) AV(avfilter_graph_free)(&graph);
    filter_source = filter_sink = nullptr;
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
  void OpenFilter(const AVFrame* picture) {
    graph = AV(avfilter_graph_alloc)();
    Require(graph, "allocate original BWDIF graph");
    Require(graph->nb_threads == 0, "unexpected original graph thread default");
    const AVFilter* buffer = AV(avfilter_get_by_name)("buffer");
    const AVFilter* sink = AV(avfilter_get_by_name)("buffersink");
    Require(buffer && sink && AV(avfilter_get_by_name)("bwdif"), "original BWDIF filters absent");
    AVRational sar = picture->sample_aspect_ratio;
    if (sar.num <= 0 || sar.den <= 0) sar = {1, 1};
    char args[160];
    std::snprintf(args, sizeof(args),
                  "video_size=1920x1080:pix_fmt=%d:time_base=1/1:pixel_aspect=%d/%d",
                  AV_PIX_FMT_NV12, sar.num, sar.den);
    Check(AV(avfilter_graph_create_filter)(&filter_source, buffer, "src", args, nullptr, graph),
          "create original-style NV12 source");
    filter_sink = AV(avfilter_graph_alloc_filter)(graph, sink, "out");
    Require(filter_sink, "allocate original-style NV12 sink");
    const AVPixelFormat pixel = AV_PIX_FMT_NV12;
    Check(AV(av_opt_set_array)(filter_sink, "pixel_formats", AV_OPT_SEARCH_CHILDREN,
                              0, 1, AV_OPT_TYPE_PIXEL_FMT, &pixel), "constrain original NV12 sink");
    Check(AV(avfilter_init_str)(filter_sink, nullptr), "initialize original NV12 sink");
    AVFilterInOut* outputs = AV(avfilter_inout_alloc)();
    AVFilterInOut* inputs = AV(avfilter_inout_alloc)();
    if (!outputs || !inputs) {
      AV(avfilter_inout_free)(&outputs); AV(avfilter_inout_free)(&inputs);
      throw std::runtime_error("allocate filter endpoints");
    }
    outputs->name = AV(av_strdup)("in"); outputs->filter_ctx = filter_source;
    inputs->name = AV(av_strdup)("out"); inputs->filter_ctx = filter_sink;
    if (!outputs->name || !inputs->name) {
      AV(avfilter_inout_free)(&outputs); AV(avfilter_inout_free)(&inputs);
      throw std::runtime_error("allocate filter endpoint names");
    }
    const int parsed = AV(avfilter_graph_parse_ptr)(graph, "bwdif=1:-1", &inputs, &outputs, nullptr);
    AV(avfilter_inout_free)(&outputs); AV(avfilter_inout_free)(&inputs);
    Check(parsed, "parse exact original BWDIF graph");
    Check(AV(avfilter_graph_config)(graph, nullptr), "configure original BWDIF graph");
    filter_input.reset(AV(av_frame_alloc)()); filter_output.reset(AV(av_frame_alloc)());
    Require(filter_input && filter_output, "allocate bounded filter frames");
    const AVRational tb = AV(av_buffersink_get_time_base)(filter_sink);
    const AVRational rate = AV(av_buffersink_get_frame_rate)(filter_sink);
    report.Row("{\"type\":\"filter_graph\",\"buffer_args\":%s,\"filter_args\":\"bwdif=1:-1\","
               "\"requested_nb_threads\":0,\"configured_nb_threads\":%d,\"thread_type\":%d,"
               "\"sink_time_base\":[%d,%d],\"sink_frame_rate\":[%d,%d],"
               "\"timestamp_recipe\":\"PUBLIC_STREAM_TIME_BASE_TO_AV_TIME_BASE_NO_KODI_OFFSET\","
               "\"input_frame_time_base\":[1,1000000],\"public_frame_properties_copied\":true,"
               "\"stock_private_metadata_equivalence\":false}",
               Quote(args).c_str(), graph->nb_threads, graph->thread_type,
               tb.num, tb.den, rate.num, rate.den);
  }
  void PullFilter(bool after_close) {
    for (unsigned turn = 0; turn != 16; ++turn) {
      CheckStop(); AV(av_frame_unref)(filter_output.get());
      const int result = Measure(filter_sink_metric, [&] {
        return AV(av_buffersink_get_frame)(filter_sink, filter_output.get());
      });
      if (result == AVERROR(EAGAIN)) {
        Require(!after_close, "BWDIF requested input after explicit reference EOF");
        return;
      }
      if (result == AVERROR_EOF) {
        Require(after_close, "BWDIF sink EOF before explicit reference EOF");
        filter_sink_eof = true; return;
      }
      Check(result, "pull original BWDIF output");
      Require(filter_output->format == AV_PIX_FMT_NV12 && filter_output->width == 1920 &&
              filter_output->height == 1080 && filter_output->data[0] && filter_output->data[1] &&
              filter_output->linesize[0] >= 1920 && filter_output->linesize[1] >= 1920 &&
              !(filter_output->flags & (AV_FRAME_FLAG_CORRUPT | AV_FRAME_FLAG_INTERLACED)) &&
              filter_output->decode_error_flags == 0,
              "unexpected or corrupt BWDIF NV12 output");
      Require(++filter_outputs <= 2 * kExpected, "more than1800 BWDIF reference outputs");
      const auto label = filter_labels.find(filter_output->best_effort_timestamp);
      Require(label != filter_labels.end() && ++label->second <= 2,
              "foreign or repeated BWDIF best-effort label");
      report.Row("{\"type\":\"filter_output\",\"ordinal\":%u,\"after_reference_eof\":%s,"
                 "\"pts\":%lld,\"best_effort_timestamp\":%lld,\"pkt_dts\":%lld,\"duration\":%lld,"
                 "\"time_base\":[%d,%d],\"flags\":%d,\"decode_error_flags\":%d,"
                 "\"sample_aspect_ratio\":[%d,%d],\"colour\":[%d,%d,%d,%d,%d],"
                 "\"crop\":[%zu,%zu,%zu,%zu]}",
                 filter_outputs, after_close ? "true" : "false",
                 static_cast<long long>(filter_output->pts),
                 static_cast<long long>(filter_output->best_effort_timestamp),
                 static_cast<long long>(filter_output->pkt_dts), static_cast<long long>(filter_output->duration),
                 filter_output->time_base.num, filter_output->time_base.den, filter_output->flags,
                 filter_output->decode_error_flags, filter_output->sample_aspect_ratio.num,
                 filter_output->sample_aspect_ratio.den, filter_output->color_range,
                 filter_output->color_primaries, filter_output->color_trc, filter_output->colorspace,
                 filter_output->chroma_location, filter_output->crop_top, filter_output->crop_bottom,
                 filter_output->crop_left, filter_output->crop_right);
      AV(av_frame_unref)(filter_output.get());
    }
    throw std::runtime_error("bounded BWDIF sink turn exhausted");
  }
  void FeedFilter(const AVFrame* picture) {
    if (!graph) OpenFilter(picture);
    AV(av_frame_unref)(filter_input.get());
    Check(AV(av_frame_copy_props)(filter_input.get(), download.get()), "copy public filter input properties");
    filter_input->format = AV_PIX_FMT_NV12;
    filter_input->width = 1920; filter_input->height = 1080;
    const AVRational from = input->streams[stream]->time_base, microseconds{1, AV_TIME_BASE};
    const auto rescale = [&](int64_t value) {
      return value == AV_NOPTS_VALUE ? AV_NOPTS_VALUE : AV(av_rescale_q)(value, from, microseconds);
    };
    filter_input->pts = rescale(picture->pts);
    filter_input->best_effort_timestamp = rescale(picture->best_effort_timestamp);
    filter_input->pkt_dts = rescale(picture->pkt_dts);
    filter_input->duration = AV(av_rescale_q)(picture->duration, from, microseconds);
    filter_input->time_base = microseconds;
    Check(AV(av_frame_get_buffer)(filter_input.get(), 64), "allocate visible NV12 filter input");
    // Copy active visible planes only; no coded1088 padding/pixel equivalence claim.
    for (int plane = 0; plane != 2; ++plane) {
      const int rows = plane == 0 ? 1080 : 540;
      Require(download->linesize[plane] >= 1920 && filter_input->linesize[plane] >= 1920,
              "invalid NV12 copy stride");
      for (int row = 0; row != rows; ++row)
        std::memcpy(filter_input->data[plane] + row * filter_input->linesize[plane],
                    download->data[plane] + row * download->linesize[plane], 1920);
    }
    const int64_t label = filter_input->best_effort_timestamp;
    Require(filter_inputs < kExpected && label != AV_NOPTS_VALUE &&
            filter_labels.emplace(label, 0).second, "missing or duplicate BWDIF input label");
    report.Row("{\"type\":\"filter_input\",\"ordinal\":%u,\"source_raw_label\":%lld,"
               "\"pts\":%lld,\"best_effort_timestamp\":%lld,\"pkt_dts\":%lld,\"duration\":%lld,"
               "\"time_base\":[%d,%d],\"flags\":%d,\"linesize\":[%d,%d],"
               "\"sample_aspect_ratio\":[%d,%d],\"colour\":[%d,%d,%d,%d,%d],"
               "\"crop\":[%zu,%zu,%zu,%zu]}",
               filter_inputs + 1, static_cast<long long>(picture->best_effort_timestamp),
               static_cast<long long>(filter_input->pts), static_cast<long long>(label),
               static_cast<long long>(filter_input->pkt_dts), static_cast<long long>(filter_input->duration),
               filter_input->time_base.num, filter_input->time_base.den, filter_input->flags,
               filter_input->linesize[0], filter_input->linesize[1], filter_input->sample_aspect_ratio.num,
               filter_input->sample_aspect_ratio.den, filter_input->color_range, filter_input->color_primaries,
               filter_input->color_trc, filter_input->colorspace, filter_input->chroma_location,
               filter_input->crop_top, filter_input->crop_bottom, filter_input->crop_left, filter_input->crop_right);
    Check(Measure(filter_add_metric, [&] {
      return AV(av_buffersrc_add_frame)(filter_source, filter_input.get());
    }), "add original BWDIF input");
    ++filter_inputs;
    PullFilter(false);
  }
  void FinishFilter() {
    if (!bwdif) return;
    unflushed_outputs = filter_outputs;
    report.Row("{\"type\":\"bwdif_unflushed_checkpoint\",\"inputs\":%u,\"outputs\":%u,"
               "\"source_eof_injected\":false,\"sink_eof\":%s,\"stock_qualification\":false}",
               filter_inputs, unflushed_outputs, filter_sink_eof ? "true" : "false");
    Require(filter_inputs == kExpected && unflushed_outputs == 2 * kExpected - 2 && !filter_sink_eof,
            "unexpected unflushed BWDIF reference prefix count");
    Check(Measure(filter_close_metric, [&] {
      return AV(av_buffersrc_close)(filter_source, AV_NOPTS_VALUE, 0);
    }), "explicit reference-only BWDIF source EOF");
    filter_source_eof = true;
    PullFilter(true);
    Require(filter_sink_eof && filter_outputs == 2 * kExpected && filter_labels.size() == kExpected,
            "explicit BWDIF reference tail incomplete");
    for (const auto& label : filter_labels)
      Require(label.second == 2, "BWDIF reference did not return both fields per input label");
    report.Row("{\"type\":\"bwdif_reference_flushed_checkpoint\",\"inputs\":%u,\"outputs\":%u,"
               "\"source_eof_injected\":true,\"sink_eof\":true,\"stock_qualification\":false}",
               filter_inputs, filter_outputs);
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
    const VAStatus ready = Measure(sync_metric, [&] {
      return AV(vaSyncSurface2)(device->display, surface, kSyncNs);
    });
    if (ready != VA_STATUS_SUCCESS) {
      report.Row("{\"type\":\"readiness_failure\",\"returned_ordinal\":%u,\"surface\":%u,"
                 "\"status\":%d,\"retained\":%zu,\"decoder_closed\":%s}",
                 completed + 1, surface, ready, held.size(), closed_before_tail ? "true" : "false");
      throw std::runtime_error("bounded retained-frame readiness failed");
    }
    AV(av_frame_unref)(download.get());
    Check(Measure(download_metric, [&] {
      return AV(av_hwframe_transfer_data)(download.get(), picture, 0);
    }), "download ready VA frame");
    Check(AV(av_frame_copy_props)(download.get(), picture), "copy public downloaded frame properties");
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
    if (bwdif) FeedFilter(picture);
    AV(av_frame_unref)(download.get());
    held.pop_front();
  }
  unsigned Receive() {
    unsigned progress = 0;
    for (;;) {
      CheckStop(); DefaultDiscard();
      const int result = Measure(receive_metric, [&] {
        return AV(avcodec_receive_frame)(decoder, frame.get());
      });
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
      const int result = Measure(send_metric, [&] {
        return AV(avcodec_send_packet)(decoder, value);
      });
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
    FinishFilter();
  }
  void MetricRow(const char* name, const Metric& metric) {
    report.Row("{\"type\":\"call_metric\",\"operation\":%s,\"calls\":%llu,\"wall_ns\":%llu,"
               "\"calling_thread_cpu_ns\":%llu,\"scope\":\"INCLUSIVE_CALL_INTERVAL_NOT_PURE_COMPONENT_CPU\"}",
               Quote(name).c_str(), static_cast<unsigned long long>(metric.calls),
               static_cast<unsigned long long>(metric.wall_ns),
               static_cast<unsigned long long>(metric.main_cpu_ns));
  }
  void Counts(const char* status, const std::string& error, uint64_t wall, uint64_t cpu,
              uint64_t process_cpu) {
    MetricRow("send", send_metric); MetricRow("receive", receive_metric);
    MetricRow("va_sync", sync_metric); MetricRow("download", download_metric);
    MetricRow("filter_add", filter_add_metric); MetricRow("filter_sink", filter_sink_metric);
    MetricRow("filter_reference_close", filter_close_metric);
    report.Row("{\"type\":\"result\",\"status\":%s,\"error\":%s,\"pid\":%ld,"
               "\"expected\":%u,\"demux_packets\":%u,\"video_packets\":%u,\"send_attempts\":%u,"
               "\"accepted_packets\":%u,\"returned_vaapi_frames\":%u,\"completed_downloads\":%u,"
               "\"unique_accepted_labels\":%zu,\"unique_returned_labels\":%zu,\"raw_label_sets_equal\":%s,"
               "\"max_retained_frames\":%u,\"demux_eof\":%s,\"drain_attempts\":%u,"
               "\"drain_accepted\":%s,\"decoder_eof\":%s,\"decoder_closed_before_tail\":%s,"
               "\"client_resources_released\":%s,"
               "\"elapsed_wall_ns\":%llu,\"calling_thread_cpu_ns\":%llu,\"process_cpu_ns\":%llu,\"signal\":%d,"
               "\"bwdif_enabled\":%s,\"bwdif_inputs\":%u,\"bwdif_unflushed_outputs\":%u,"
               "\"bwdif_reference_flushed_outputs\":%u,\"bwdif_reference_source_eof_injected\":%s,"
               "\"bwdif_reference_sink_eof\":%s,\"bwdif_unique_input_labels\":%zu,"
               "\"stock_realtime_qualification\":false,\"bwdif_tail_qualification\":false,"
               "\"independent_pixel_golden\":false}",
               Quote(status).c_str(), Quote(error).c_str(), static_cast<long>(getpid()), kExpected,
               reads, video_reads, attempts, accepted, returned, completed,
               accepted_labels.size(), returned_labels.size(),
               accepted_labels == returned_labels ? "true" : "false", max_held,
               demux_eof ? "true" : "false", drain_attempts, drain_accepted ? "true" : "false",
               decoder_eof ? "true" : "false", closed_before_tail ? "true" : "false",
               resources_released ? "true" : "false",
               static_cast<unsigned long long>(wall), static_cast<unsigned long long>(cpu),
               static_cast<unsigned long long>(process_cpu), static_cast<int>(stopped),
               bwdif ? "true" : "false", filter_inputs, unflushed_outputs, filter_outputs,
               filter_source_eof ? "true" : "false", filter_sink_eof ? "true" : "false", filter_labels.size());
  }
};
} // namespace

int main(int argc, char** argv) {
  if ((argc != 4 && argc != 5) || (argc == 5 && std::strcmp(argv[4], "--bwdif") != 0)) {
    std::fprintf(stderr, "usage: %s ORIGINAL_TS VA_DEVICE FRESH_JSONL_REPORT [--bwdif]\n", argv[0]);
    return 2;
  }
  const bool bwdif = argc == 5;
  try {
    Report report(argv[3]);
    struct sigaction action{}; action.sa_handler = Signal; sigemptyset(&action.sa_mask);
    Require(sigaction(SIGTERM, &action, nullptr) == 0 && sigaction(SIGINT, &action, nullptr) == 0,
            "install normal cleanup signal handlers");
    const uint64_t start = Clock(CLOCK_MONOTONIC), cpu = Clock(CLOCK_THREAD_CPUTIME_ID);
    const uint64_t process_cpu = Clock(CLOCK_PROCESS_CPUTIME_ID);
    deadline = start + kDeadlineNs;
    report.Row("{\"type\":\"header\",\"schema\":\"crystalhd-original-decode900-v1\","
               "\"pid\":%ld,\"input\":%s,\"device\":%s,\"expected\":%u,\"lookahead\":%u,"
               "\"deadline_ns\":%llu,\"readiness_timeout_ns\":%llu,\"discard\":\"DEFAULT\","
               "\"bwdif_enabled\":%s,\"bwdif_eof_policy\":\"UNFLUSHED_CHECKPOINT_THEN_REFERENCE_ONLY_EOF\","
               "\"scope\":\"OWN_NO_CLOCK_OR_RENDERER_COUNTERFACTUAL_UNMODIFIED_ORIGINAL_PROVIDERS\"}",
               static_cast<long>(getpid()), Quote(argv[1]).c_str(), Quote(argv[2]).c_str(),
               kExpected, kLookahead, static_cast<unsigned long long>(kDeadlineNs),
               static_cast<unsigned long long>(kSyncNs), bwdif ? "true" : "false");
    Providers(report, bwdif);
    Probe probe(report, bwdif);
    try {
      probe.Open(argv[1], argv[2]);
      probe.Run();
      probe.Close();
      probe.Counts("PASS_DIAGNOSTIC_900_ORIGINALS", "", Clock(CLOCK_MONOTONIC) - start,
                   Clock(CLOCK_THREAD_CPUTIME_ID) - cpu, Clock(CLOCK_PROCESS_CPUTIME_ID) - process_cpu);
      return 0;
    } catch (const std::exception& error) {
      probe.Close();
      probe.Counts(stopped ? "CANCELLED" : "FAIL", error.what(), Clock(CLOCK_MONOTONIC) - start,
                   Clock(CLOCK_THREAD_CPUTIME_ID) - cpu, Clock(CLOCK_PROCESS_CPUTIME_ID) - process_cpu);
      std::fprintf(stderr, "OWN no-clock decode900: %s\n", error.what());
      return stopped ? 128 + static_cast<int>(stopped) : 1;
    }
  } catch (const std::exception& error) {
    std::fprintf(stderr, "OWN no-clock decode900 setup/report: %s\n", error.what());
    return 2;
  }
}
