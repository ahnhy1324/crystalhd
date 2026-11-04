// SPDX-License-Identifier: GPL-2.0-or-later
/* OWN, software-only same-input diagnostic client. Never link replacement
 * FFmpeg libraries. Compile against unchanged 12cc public headers and run in
 * the original App/Platform namespace. Prefix mode NEVER signals source EOF.
 * NV12 sink configuration follows pinned original source, not an observed
 * option-setting call. Active pixels, not unobserved padding, are replayed. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/version.h>
#include <libavutil/buffer.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libavutil/version.h>
#if LIBAVFILTER_VERSION_INT != AV_VERSION_INT(12, 1, 102) || LIBAVUTIL_VERSION_INT != AV_VERSION_INT(61, 1, 102)
#error "Reference requires unchanged original filter12.1.102/util61.1.102 headers"
#endif
#define MAX_FRAMES 3
#define MAX_STEPS 128
#define MAX_OUTPUTS 16
#define MAX_STORAGE (64U * 1024U * 1024U)
#define BASIS "PINNED_ORIGINAL_SOURCE_PLUS_ACTUAL_NV12_SINK_OUTPUT"
#define DECL(name) static __typeof__(&name) fn_##name
DECL(avfilter_version); DECL(avutil_version); DECL(avfilter_get_by_name);
DECL(avfilter_graph_alloc); DECL(avfilter_graph_free); DECL(avfilter_graph_create_filter);
DECL(avfilter_graph_alloc_filter); DECL(avfilter_init_str); DECL(av_opt_set_array);
DECL(avfilter_inout_alloc); DECL(avfilter_inout_free); DECL(avfilter_graph_parse_ptr);
DECL(avfilter_graph_config); DECL(av_buffersrc_add_frame); DECL(av_buffersrc_add_frame_flags);
DECL(av_buffersrc_close); DECL(av_buffersink_get_frame); DECL(av_buffersink_get_frame_flags);
DECL(av_buffersink_get_time_base); DECL(av_buffersink_get_frame_rate);
DECL(av_frame_alloc); DECL(av_frame_free); DECL(av_buffer_allocz); DECL(av_strdup);
struct input { const char *path; int64_t v[21]; };
struct step { const char *api; int index, flags, expected; };
static struct input inputs[MAX_FRAMES];
static struct step steps[MAX_STEPS];
static unsigned input_count, step_count, output_count;
static AVFilterGraph *graph;
static AVFilterContext *source, *sink;
static int outfd = -1;
static const char *mode = "prefix";
static void fail(const char *why)
{
    fprintf(stderr, "OWN postfilter reference: %s\n", why);
    exit(2);
}
static void check(bool condition, const char *why) { if (!condition) fail(why); }
static int64_t number(const char *text)
{
    char *end = NULL; errno = 0; long long value = strtoll(text, &end, 10);
    check(!errno && *text && end && !*end, "invalid integer argument");
    return (int64_t)value;
}
static int integer(const char *text)
{
    int64_t value = number(text); check(value >= INT_MIN && value <= INT_MAX, "integer overflow");
    return (int)value;
}
static void string(const char *text)
{
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (*p == '"' || *p == '\\') { putchar('\\'); putchar(*p); }
        else if (*p < 32 || *p >= 127) printf("\\u%04x", *p);
        else putchar(*p);
    }
    putchar('"');
}
static void *symbol(void *handle, const char *name, const char *version, const char *provider)
{
    void *result = dlvsym(handle, name, version); Dl_info info; char actual[PATH_MAX];
    check(result && dladdr(result, &info) && info.dli_fname && realpath(info.dli_fname, actual),
          "missing versioned original-provider symbol");
    check(!strcmp(actual, provider), "symbol resolved outside selected original provider");
    return result;
}
#define LOAD(handle, name, version, provider) do { \
    void *address = symbol(handle, #name, version, provider); \
    _Static_assert(sizeof(address) == sizeof(fn_##name), "ELF function pointer size"); \
    memcpy(&fn_##name, &address, sizeof(address)); \
} while (0)
static void providers(const char *filter_path, const char *util_path)
{
    char fp[PATH_MAX], up[PATH_MAX];
    check(filter_path && util_path && filter_path[0] == '/' && util_path[0] == '/' &&
          realpath(filter_path, fp) && realpath(util_path, up), "canonical original providers required");
    void *util = dlopen(up, RTLD_NOW | RTLD_LOCAL);
    if (!util) { fprintf(stderr, "%s\n", dlerror()); fail("original util closure unavailable"); }
    void *filter = dlopen(fp, RTLD_NOW | RTLD_LOCAL);
    if (!filter) { fprintf(stderr, "%s\n", dlerror()); fail("original filter closure unavailable"); }
    LOAD(filter, avfilter_version, "LIBAVFILTER_12", fp);
    LOAD(util, avutil_version, "LIBAVUTIL_61", up);
    check(fn_avfilter_version() == LIBAVFILTER_VERSION_INT && fn_avutil_version() == LIBAVUTIL_VERSION_INT,
          "original provider version mismatch");
    LOAD(filter, avfilter_get_by_name, "LIBAVFILTER_12", fp);
    LOAD(filter, avfilter_graph_alloc, "LIBAVFILTER_12", fp);
    LOAD(filter, avfilter_graph_free, "LIBAVFILTER_12", fp);
    LOAD(filter, avfilter_graph_create_filter, "LIBAVFILTER_12", fp);
    LOAD(filter, avfilter_graph_alloc_filter, "LIBAVFILTER_12", fp);
    LOAD(filter, avfilter_init_str, "LIBAVFILTER_12", fp);
    LOAD(filter, avfilter_inout_alloc, "LIBAVFILTER_12", fp);
    LOAD(filter, avfilter_inout_free, "LIBAVFILTER_12", fp);
    LOAD(filter, avfilter_graph_parse_ptr, "LIBAVFILTER_12", fp);
    LOAD(filter, avfilter_graph_config, "LIBAVFILTER_12", fp);
    LOAD(filter, av_buffersrc_add_frame, "LIBAVFILTER_12", fp);
    LOAD(filter, av_buffersrc_add_frame_flags, "LIBAVFILTER_12", fp);
    LOAD(filter, av_buffersrc_close, "LIBAVFILTER_12", fp);
    LOAD(filter, av_buffersink_get_frame, "LIBAVFILTER_12", fp);
    LOAD(filter, av_buffersink_get_frame_flags, "LIBAVFILTER_12", fp);
    LOAD(filter, av_buffersink_get_time_base, "LIBAVFILTER_12", fp);
    LOAD(filter, av_buffersink_get_frame_rate, "LIBAVFILTER_12", fp);
    LOAD(util, av_opt_set_array, "LIBAVUTIL_61", up);
    LOAD(util, av_frame_alloc, "LIBAVUTIL_61", up);
    LOAD(util, av_frame_free, "LIBAVUTIL_61", up);
    LOAD(util, av_buffer_allocz, "LIBAVUTIL_61", up);
    LOAD(util, av_strdup, "LIBAVUTIL_61", up);
    printf("{\"type\":\"header\",\"schema\":\"crystalhd-postfilter-reference-v1\",\"mode\":"); string(mode);
    printf(",\"filter_provider\":"); string(fp); printf(",\"util_provider\":"); string(up);
    printf(",\"filter_version\":%u,\"util_version\":%u,\"nv12_format\":%d,\"provider_ok\":true,\"bwdif_available\":%s,\"constraint_basis\":\"%s\"}\n",
           fn_avfilter_version(), fn_avutil_version(), AV_PIX_FMT_NV12,
           fn_avfilter_get_by_name("bwdif") ? "true" : "false", BASIS);
    fflush(stdout);
    check(fn_avfilter_get_by_name("bwdif") != NULL, "BWDIF unavailable in original App provider");
}
static void transfer(int fd, uint8_t *data, size_t length, bool writing)
{
    while (length) {
        ssize_t done = writing ? write(fd, data, length) : read(fd, data, length);
        if (done < 0 && errno == EINTR) continue;
        check(done > 0, "short active-plane file read/write");
        data += done; length -= (size_t)done;
    }
}
static AVFrame *read_frame(const struct input *in)
{
    const int64_t *v = in->v;
    int w = (int)v[0], h = (int)v[1], ys = (int)v[2], uvs = (int)v[3];
    check(w > 0 && h > 0 && !(w & 1) && !(h & 1) && w <= 1920 && h <= 1088 &&
          ys >= w && uvs >= w && ys <= 32768 && uvs <= 32768, "unsupported captured NV12 geometry/strides");
    check(v[17] >= 0 && v[17] < h && v[18] >= 0 && v[18] < h-v[17] &&
          v[19] >= 0 && v[19] < w && v[20] >= 0 && v[20] < w-v[19], "captured crop outside active geometry");
    size_t storage = (size_t)ys * h + (size_t)uvs * (h / 2);
    check(storage <= MAX_STORAGE, "captured storage exceeds bound");
    struct stat st; int fd = open(in->path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    check(fd >= 0 && !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size == (off_t)((size_t)w*h*3/2),
          "input is not an exact tight active NV12 snapshot");
    AVFrame *frame = fn_av_frame_alloc(); check(frame != NULL, "frame allocation failed");
    frame->buf[0] = fn_av_buffer_allocz(storage); check(frame->buf[0] != NULL, "frame storage allocation failed");
    frame->format = AV_PIX_FMT_NV12; frame->width = w; frame->height = h;
    frame->data[0] = frame->buf[0]->data; frame->data[1] = frame->data[0] + (size_t)ys*h;
    frame->linesize[0] = ys; frame->linesize[1] = uvs;
    for (int plane = 0; plane < 2; ++plane)
        for (int row = 0; row < (plane ? h/2 : h); ++row)
            transfer(fd, frame->data[plane] + (size_t)row * frame->linesize[plane], w, false);
    check(close(fd) == 0, "input close failed");
    frame->pts = v[4]; frame->best_effort_timestamp = v[5]; frame->duration = v[6];
    frame->time_base = (AVRational){(int)v[7], (int)v[8]}; frame->flags = (int)v[9];
    frame->sample_aspect_ratio = (AVRational){(int)v[10], (int)v[11]};
    frame->color_range = (int)v[12]; frame->color_primaries = (int)v[13]; frame->color_trc = (int)v[14];
    frame->colorspace = (int)v[15]; frame->chroma_location = (int)v[16];
    frame->crop_top = (size_t)v[17]; frame->crop_bottom = (size_t)v[18];
    frame->crop_left = (size_t)v[19]; frame->crop_right = (size_t)v[20];
    return frame;
}
static void frame_json(const AVFrame *f)
{
    printf("{\"pts\":%"PRId64",\"best_effort_timestamp\":%"PRId64",\"duration\":%"PRId64,
           f->pts, f->best_effort_timestamp, f->duration);
    printf(",\"time_base\":[%d,%d],\"format\":%d,\"width\":%d,\"height\":%d,\"linesize\":[%d,%d],\"flags\":%d",
           f->time_base.num, f->time_base.den, f->format, f->width, f->height, f->linesize[0], f->linesize[1], f->flags);
    printf(",\"sample_aspect_ratio\":[%d,%d],\"color_range\":%d,\"color_primaries\":%d,\"color_trc\":%d,\"colorspace\":%d,\"chroma_location\":%d,\"crop\":[%zu,%zu,%zu,%zu]}",
           f->sample_aspect_ratio.num, f->sample_aspect_ratio.den, f->color_range, f->color_primaries,
           f->color_trc, f->colorspace, f->chroma_location, f->crop_top, f->crop_bottom, f->crop_left, f->crop_right);
}
static void output(AVFrame *f, bool injected_eof)
{
    check(++output_count <= MAX_OUTPUTS, "reference output bound exhausted");
    check(f->format == AV_PIX_FMT_NV12 && f->width > 0 && f->width <= 1920 && !(f->width & 1) &&
          f->height > 0 && f->height <= 1088 && !(f->height & 1) && f->data[0] && f->data[1] &&
          f->linesize[0] >= f->width && f->linesize[1] >= f->width, "reference output is not bounded active NV12");
    char name[40]; snprintf(name, sizeof(name), "output-%02u.nv12", output_count);
    int fd = openat(outfd, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    check(fd >= 0, "exclusive reference snapshot creation failed");
    for (int plane = 0; plane < 2; ++plane)
        for (int row = 0; row < (plane ? f->height/2 : f->height); ++row)
            transfer(fd, f->data[plane] + (size_t)row * f->linesize[plane], f->width, true);
    check(close(fd) == 0, "reference snapshot close failed");
    AVRational tb = fn_av_buffersink_get_time_base(sink), rate = fn_av_buffersink_get_frame_rate(sink);
    printf("{\"type\":\"output\",\"output_index\":%u,\"reference_only_eof\":%s,\"file\":", output_count, injected_eof ? "true" : "false");
    string(name); printf(",\"frame\":"); frame_json(f);
    printf(",\"sink_time_base\":[%d,%d],\"sink_frame_rate\":[%d,%d]}\n", tb.num, tb.den, rate.num, rate.den);
}
static void setup(const char *buffer_args, const char *filter_args)
{
    check(buffer_args && filter_args && strlen(buffer_args) < 1024 && strlen(filter_args) < 128,
          "missing or unbounded exact graph arguments");
    check(!strncmp(filter_args, "bwdif=", 6) && !strpbrk(filter_args, ",;[]"),
          "only the captured single BWDIF filter is supported");
    graph = fn_avfilter_graph_alloc(); check(graph != NULL, "graph allocation failed");
    check(fn_avfilter_graph_create_filter(&source, fn_avfilter_get_by_name("buffer"), "src", buffer_args, NULL, graph) >= 0,
          "captured buffer configuration rejected");
    sink = fn_avfilter_graph_alloc_filter(graph, fn_avfilter_get_by_name("buffersink"), "out");
    check(sink != NULL, "sink allocation failed");
    enum AVPixelFormat format = AV_PIX_FMT_NV12;
    check(fn_av_opt_set_array(sink, "pixel_formats", AV_OPT_SEARCH_CHILDREN, 0, 1, AV_OPT_TYPE_PIXEL_FMT, &format) >= 0 &&
          fn_avfilter_init_str(sink, NULL) >= 0, "pinned-source NV12 sink configuration rejected");
    AVFilterInOut *outputs = fn_avfilter_inout_alloc(), *ins = fn_avfilter_inout_alloc();
    check(outputs && ins, "graph endpoint allocation failed");
    outputs->name = fn_av_strdup("in"); outputs->filter_ctx = source; outputs->pad_idx = 0;
    ins->name = fn_av_strdup("out"); ins->filter_ctx = sink; ins->pad_idx = 0;
    check(outputs->name && ins->name, "endpoint names allocation failed");
    int ret = fn_avfilter_graph_parse_ptr(graph, filter_args, &ins, &outputs, NULL);
    fn_avfilter_inout_free(&outputs); fn_avfilter_inout_free(&ins);
    check(ret >= 0 && fn_avfilter_graph_config(graph, NULL) >= 0, "captured BWDIF graph rejected");
}
int main(int argc, char **argv)
{
    const char *filter_path = NULL, *util_path = NULL, *buffer_args = NULL, *filter_args = NULL, *directory = NULL;
    bool query = false;
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (!strcmp(arg, "--query")) { query = true; continue; }
        check(i+1 < argc, "missing option argument");
        if (!strcmp(arg, "--filter-provider")) filter_path = argv[++i];
        else if (!strcmp(arg, "--util-provider")) util_path = argv[++i];
        else if (!strcmp(arg, "--mode")) mode = argv[++i];
        else if (!strcmp(arg, "--buffer-args")) buffer_args = argv[++i];
        else if (!strcmp(arg, "--filter-args")) filter_args = argv[++i];
        else if (!strcmp(arg, "--output-dir")) directory = argv[++i];
        else if (!strcmp(arg, "--frame")) {
            check(input_count < MAX_FRAMES && i+22 < argc, "invalid bounded input specification");
            struct input *in = &inputs[input_count++]; in->path = argv[++i];
            for (unsigned j = 0; j < 21; ++j) in->v[j] = number(argv[++i]);
            for (unsigned j = 0; j < 21; ++j)
                if (j < 4 || (j > 6 && j < 17)) check(in->v[j] >= INT_MIN && in->v[j] <= INT_MAX, "frame integer overflow");
            check(in->path[0] == '/' && in->v[17] >= 0 && in->v[18] >= 0 && in->v[19] >= 0 && in->v[20] >= 0,
                  "input path/crop invalid");
        } else if (!strcmp(arg, "--step")) {
            check(step_count < MAX_STEPS && i+4 < argc, "invalid bounded call schedule");
            struct step *s = &steps[step_count++]; s->api = argv[++i]; s->index = integer(argv[++i]);
            s->flags = integer(argv[++i]); s->expected = integer(argv[++i]);
        } else fail("unknown option");
    }
    check(!strcmp(mode, "prefix") || !strcmp(mode, "flushed-reference"), "unknown reference mode");
    providers(filter_path, util_path);
    if (query) { puts("{\"type\":\"footer\",\"query_only\":true,\"complete\":true}"); return 0; }
    check(input_count == MAX_FRAMES && step_count > 0 && directory && directory[0] == '/', "three captured inputs and exact schedule required");
    outfd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW); check(outfd >= 0, "output directory unavailable");
    setup(buffer_args, filter_args);
    unsigned added = 0; bool used[MAX_FRAMES] = {false};
    for (unsigned i = 0; i < step_count; ++i) {
        struct step *s = &steps[i]; int ret;
        bool add = !strcmp(s->api, "av_buffersrc_add_frame") || !strcmp(s->api, "av_buffersrc_add_frame_flags");
        bool get = !strcmp(s->api, "av_buffersink_get_frame") || !strcmp(s->api, "av_buffersink_get_frame_flags");
        check(add || get, "prefix schedule contains unsupported or EOF operation");
        if (add) {
            check(s->index >= 1 && s->index <= MAX_FRAMES && !used[s->index-1] && s->expected >= 0, "prefix needs three distinct accepted inputs");
            check(strcmp(s->api, "av_buffersrc_add_frame") || s->flags == 0, "plain add cannot set flags");
            AVFrame *frame = read_frame(&inputs[s->index-1]);
            ret = !strcmp(s->api, "av_buffersrc_add_frame") ? fn_av_buffersrc_add_frame(source, frame) : fn_av_buffersrc_add_frame_flags(source, frame, s->flags);
            fn_av_frame_free(&frame); used[s->index-1] = true; added++;
        } else {
            check(strcmp(s->api, "av_buffersink_get_frame") || s->flags == 0, "plain sink pull cannot set flags");
            AVFrame *frame = fn_av_frame_alloc(); check(frame != NULL, "output frame allocation failed");
            ret = !strcmp(s->api, "av_buffersink_get_frame") ? fn_av_buffersink_get_frame(sink, frame) : fn_av_buffersink_get_frame_flags(sink, frame, s->flags);
            if (ret >= 0) { check(s->index == (int)output_count+1, "output schedule index differs"); output(frame, false); }
            fn_av_frame_free(&frame);
        }
        printf("{\"type\":\"call\",\"step\":%u,\"api\":", i); string(s->api);
        printf(",\"ret\":%d,\"expected_ret\":%d,\"reference_only_eof\":false}\n", ret, s->expected);
        check(ret == s->expected, "reference result differs from observed prefix schedule");
    }
    check(added == MAX_FRAMES && output_count == 4, "prefix did not reproduce three inputs and four outputs");
    bool eof = false;
    if (!strcmp(mode, "flushed-reference")) {
        int ret = fn_av_buffersrc_close(source, AV_NOPTS_VALUE, 0);
        printf("{\"type\":\"reference_eof\",\"api\":\"av_buffersrc_close\",\"ret\":%d,\"close_pts\":%"PRId64",\"reference_only_eof\":true}\n", ret, (int64_t)AV_NOPTS_VALUE);
        check(ret >= 0, "explicit reference EOF rejected");
        while (output_count < MAX_OUTPUTS) {
            AVFrame *frame = fn_av_frame_alloc(); check(frame != NULL, "drain allocation failed");
            ret = fn_av_buffersink_get_frame(sink, frame);
            if (ret >= 0) output(frame, true);
            fn_av_frame_free(&frame);
            if (ret == AVERROR_EOF) { eof = true; break; }
            check(ret >= 0, "explicit reference drain ended without EOF");
        }
        check(eof, "reference EOF output bound exhausted");
    }
    fn_avfilter_graph_free(&graph); check(close(outfd) == 0, "output directory close failed");
    printf("{\"type\":\"footer\",\"complete\":true,\"inputs\":%u,\"outputs\":%u,\"source_eof_injected\":%s,\"reference_eof_observed\":%s,\"original_client_eof_claim\":false}\n",
           added, output_count, !strcmp(mode, "flushed-reference") ? "true" : "false", eof ? "true" : "false");
    check(!ferror(stdout) && fflush(stdout) == 0, "reference report write failed");
    return 0;
}
