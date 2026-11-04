// SPDX-License-Identifier: GPL-2.0-or-later
/* OWN isolated observer, never a product fix or stock/release qualification.
 * Compile against unchanged original filter12.1.102/util61.1.102 headers.
 * Synthetic-provider controls exercise OUR forwarding, not an external ABI. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/version.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libavutil/version.h>
#if LIBAVFILTER_VERSION_INT != AV_VERSION_INT(12, 1, 102) || LIBAVUTIL_VERSION_INT != AV_VERSION_INT(61, 1, 102)
#error "Post-filter observer requires original filter12.1.102/util61.1.102 headers"
#endif
#ifndef POSTFILTER_CAPACITY
#define POSTFILTER_CAPACITY 16384
#endif
#ifndef POSTFILTER_SNAPSHOTS
#define POSTFILTER_SNAPSHOTS 16
#endif
#if POSTFILTER_CAPACITY < 2 || POSTFILTER_CAPACITY > 16384 || POSTFILTER_SNAPSHOTS < 1 || POSTFILTER_SNAPSHOTS > 16
#error "Invalid bounded observer capacity"
#endif
#define GRAPH_CAPACITY 256
#define MAX_WIDTH 1920
#define MAX_HEIGHT 1088
#define SNAPSHOT_BYTES ((size_t)MAX_WIDTH * MAX_HEIGHT * 3 / 2)
#define FILTER_PROVIDER "/app/lib/libavfilter.so.12.1.102"
#define UTIL_PROVIDER "/app/lib/libavutil.so.61.1.102"
#define TEXT_SIZE 1024

typedef int (*create_fn)(AVFilterContext **, const AVFilter *, const char *, const char *, void *, AVFilterGraph *);
typedef int (*parse_fn)(AVFilterGraph *, const char *, AVFilterInOut **, AVFilterInOut **, void *);
typedef int (*frame_fn)(AVFilterContext *, AVFrame *);
typedef int (*flags_fn)(AVFilterContext *, AVFrame *, int);
typedef int (*close_fn)(AVFilterContext *, int64_t, unsigned);
typedef void (*free_fn)(AVFilterGraph **);
typedef unsigned (*version_fn)(void);
typedef AVRational (*rational_fn)(const AVFilterContext *);
struct frame_data {
    int64_t pts, best, duration;
    int format, width, height, linesize[2], flags;
    AVRational time_base, sar;
    int range, primaries, trc, space, chroma;
    size_t crop[4];
    int snapshot;
};
struct event {
    uint64_t begin, end, graph_id;
    uintptr_t graph, context;
    pid_t tid;
    const char *api;
    int ret, before_errno, after_errno, flags;
    bool void_ret, input_null, has_frame;
    int64_t close_pts;
    unsigned input_index, output_index;
    char name[128], args[TEXT_SIZE];
    struct frame_data frame;
    AVRational sink_tb, sink_rate;
    atomic_bool ready;
};
struct graph_state {
    uintptr_t address;
    uint64_t id;
    unsigned inputs, outputs, nv12_inputs, nv12_outputs, busy;
    bool open;
};
struct image {
    unsigned char bytes[SNAPSHOT_BYTES];
    size_t size;
    atomic_bool ready, failed;
};
static struct event events[POSTFILTER_CAPACITY];
static struct graph_state graphs[GRAPH_CAPACITY];
static struct image images[POSTFILTER_SNAPSHOTS];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static atomic_uint attempted, dropped, inflight, snapshot_count, snapshot_failures;
static atomic_uint lifecycle_failures, string_truncations;
static atomic_bool clock_failure, fork_seen;
static unsigned graph_count;
static bool active, provider_ok, publishing;
static pid_t owner_pid;
static char prefix[PATH_MAX], filter_path[PATH_MAX], util_path[PATH_MAX];
static unsigned filter_version, util_version;
static create_fn next_create;
static parse_fn next_parse;
static frame_fn next_add, next_get;
static flags_fn next_add_flags, next_get_flags;
static close_fn next_close;
static free_fn next_free;
static rational_fn next_sink_tb, next_sink_rate;
/* Suppress only nested forwarding of these public wrappers. Different threads
 * still have independent observation state. No original call is retried. */
static _Thread_local unsigned forwarding;

static bool describe(void *symbol, char *path, Dl_info *info)
{
    memset(info, 0, sizeof(*info));
    return symbol && dladdr(symbol, info) && info->dli_fname && realpath(info->dli_fname, path);
}
static bool expected(const char *actual, const char *variable, const char *fallback)
{
    char canonical[PATH_MAX]; const char *path = getenv(variable);
    if (!path) path = fallback;
    return path[0] == '/' && realpath(path, canonical) && !strcmp(actual, canonical);
}
static void fork_prepare(void)
{
    if (active && owner_pid == getpid()) atomic_store_explicit(&fork_seen, true, memory_order_relaxed);
}
static void initialize(void)
{
    int saved = errno;
    owner_pid = getpid();
    const char *marker = getenv("CRYSTALHD_TEST_POSTFILTER");
    const char *path = getenv("CRYSTALHD_TEST_POSTFILTER_PREFIX");
    active = marker && !strcmp(marker, "v1") && path && path[0] == '/' && strlen(path) < sizeof(prefix);
    if (active) memcpy(prefix, path, strlen(path) + 1);
    const char *names[] = {"avfilter_graph_create_filter", "avfilter_graph_parse_ptr",
        "av_buffersrc_add_frame", "av_buffersrc_add_frame_flags", "av_buffersrc_close",
        "av_buffersink_get_frame", "av_buffersink_get_frame_flags", "avfilter_graph_free",
        "av_buffersink_get_time_base", "av_buffersink_get_frame_rate"};
    void *symbols[10]; bool versioned = true;
    for (unsigned i = 0; i < 10; ++i) {
        symbols[i] = dlvsym(RTLD_NEXT, names[i], "LIBAVFILTER_12");
        if (!symbols[i]) { versioned = false; symbols[i] = dlsym(RTLD_NEXT, names[i]); }
    }
    next_create = (create_fn)symbols[0]; next_parse = (parse_fn)symbols[1];
    next_add = (frame_fn)symbols[2]; next_add_flags = (flags_fn)symbols[3];
    next_close = (close_fn)symbols[4]; next_get = (frame_fn)symbols[5];
    next_get_flags = (flags_fn)symbols[6]; next_free = (free_fn)symbols[7];
    next_sink_tb = (rational_fn)symbols[8]; next_sink_rate = (rational_fn)symbols[9];
    if (active) {
        void *fv = dlvsym(RTLD_NEXT, "avfilter_version", "LIBAVFILTER_12");
        void *uv = dlvsym(RTLD_NEXT, "avutil_version", "LIBAVUTIL_61");
        Dl_info fi, ui;
        bool valid = versioned && describe(fv, filter_path, &fi) && describe(uv, util_path, &ui);
        if (valid) {
            filter_version = ((version_fn)fv)(); util_version = ((version_fn)uv)();
            valid = filter_version == LIBAVFILTER_VERSION_INT && util_version == LIBAVUTIL_VERSION_INT &&
                expected(filter_path, "CRYSTALHD_TEST_POSTFILTER_FILTER_PROVIDER", FILTER_PROVIDER) &&
                expected(util_path, "CRYSTALHD_TEST_POSTFILTER_UTIL_PROVIDER", UTIL_PROVIDER);
            for (unsigned i = 0; i < 10; ++i) {
                char actual[PATH_MAX]; Dl_info info;
                if (!describe(symbols[i], actual, &info) || info.dli_fbase != fi.dli_fbase || strcmp(actual, filter_path)) valid = false;
            }
        }
        provider_ok = valid;
        if (pthread_atfork(fork_prepare, NULL, NULL)) atomic_fetch_add(&lifecycle_failures, 1);
    }
    errno = saved;
}
__attribute__((constructor)) static void start_observer(void)
{
    int saved = errno; pthread_once(&once, initialize); errno = saved;
}
static uint64_t now_ns(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) { atomic_store(&clock_failure, true); return 0; }
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}
static bool observing(void)
{
    if (!active || !provider_ok || owner_pid != getpid() || forwarding) return false;
    pthread_mutex_lock(&lock);
    bool watch = !publishing;
    if (watch) atomic_fetch_add(&inflight, 1);
    pthread_mutex_unlock(&lock);
    return watch;
}
/* Lock is never held across an original provider call. Retired address entries
 * remain in this bounded ledger so a later reuse gets a distinct lifetime ID. */
static struct graph_state *find_graph(AVFilterGraph *graph, bool create)
{
    if (!graph) { atomic_fetch_add(&lifecycle_failures, 1); return NULL; }
    for (unsigned i = graph_count; i > 0; --i)
        if (graphs[i - 1].address == (uintptr_t)graph && graphs[i - 1].open) return &graphs[i - 1];
    if (!create || graph_count == GRAPH_CAPACITY) { atomic_fetch_add(&lifecycle_failures, 1); return NULL; }
    struct graph_state *g = &graphs[graph_count++];
    g->address = (uintptr_t)graph; g->id = graph_count; g->open = true;
    return g;
}
static void copy_text(char *destination, size_t size, const char *source)
{
    if (!source) { destination[0] = 0; return; }
    size_t n = strnlen(source, size);
    if (n == size) { atomic_fetch_add(&string_truncations, 1); n = size - 1; }
    memcpy(destination, source, n); destination[n] = 0;
}
static struct event *begin_event(const char *api, AVFilterGraph *graph, AVFilterContext *context, bool allow_new)
{
    unsigned index = atomic_fetch_add(&attempted, 1);
    if (index >= POSTFILTER_CAPACITY) { atomic_fetch_add(&dropped, 1); return NULL; }
    struct event *e = &events[index];
    e->api = api; e->graph = (uintptr_t)graph; e->context = (uintptr_t)context;
    e->tid = (pid_t)syscall(SYS_gettid); e->begin = now_ns(); e->frame.snapshot = -1;
    pthread_mutex_lock(&lock);
    struct graph_state *g = find_graph(graph, allow_new);
    if (g) { e->graph_id = g->id; ++g->busy; }
    pthread_mutex_unlock(&lock);
    return e;
}
static void end_event(struct event *e, int result, int before, int after)
{
    if (e) {
        e->ret = result; e->before_errno = before; e->after_errno = after; e->end = now_ns();
        pthread_mutex_lock(&lock);
        if (e->graph_id) {
            struct graph_state *g = &graphs[e->graph_id - 1];
            if (!g->busy) atomic_fetch_add(&lifecycle_failures, 1); else --g->busy;
        }
        pthread_mutex_unlock(&lock);
        atomic_store_explicit(&e->ready, true, memory_order_release);
    }
    atomic_fetch_sub_explicit(&inflight, 1, memory_order_release);
}
static void capture_frame(struct event *e, const AVFrame *f, bool input)
{
    if (!e || !f) return;
    e->has_frame = true;
    struct frame_data *d = &e->frame;
    d->pts = f->pts; d->best = f->best_effort_timestamp; d->duration = f->duration;
    d->time_base = f->time_base; d->format = f->format; d->width = f->width; d->height = f->height;
    d->linesize[0] = f->linesize[0]; d->linesize[1] = f->linesize[1]; d->flags = f->flags;
    d->sar = f->sample_aspect_ratio; d->range = f->color_range; d->primaries = f->color_primaries;
    d->trc = f->color_trc; d->space = f->colorspace; d->chroma = f->chroma_location;
    d->crop[0] = f->crop_top; d->crop[1] = f->crop_bottom; d->crop[2] = f->crop_left; d->crop[3] = f->crop_right;
    bool selected = false;
    pthread_mutex_lock(&lock);
    if (e->graph_id) {
        struct graph_state *g = &graphs[e->graph_id - 1];
        if (input) e->input_index = ++g->inputs; else e->output_index = ++g->outputs;
        if (f->format == AV_PIX_FMT_NV12) {
            unsigned n = input ? ++g->nv12_inputs : ++g->nv12_outputs;
            selected = n <= (input ? 3U : 4U);
        }
    }
    pthread_mutex_unlock(&lock);
    if (!selected) return;
    unsigned slot = atomic_fetch_add(&snapshot_count, 1);
    if (slot >= POSTFILTER_SNAPSHOTS) { atomic_fetch_add(&snapshot_failures, 1); return; }
    struct image *image = &images[slot];
    if (d->width <= 0 || d->height <= 0 || d->width > MAX_WIDTH || d->height > MAX_HEIGHT ||
        d->width % 2 || d->height % 2 || !f->data[0] || !f->data[1] ||
        d->linesize[0] < d->width || d->linesize[1] < d->width) {
        atomic_store_explicit(&image->failed, true, memory_order_release);
        atomic_fetch_add(&snapshot_failures, 1); return;
    }
    size_t y_size = (size_t)d->width * (size_t)d->height;
    for (int y = 0; y < d->height; ++y)
        memcpy(image->bytes + (size_t)y * (size_t)d->width, f->data[0] + (size_t)y * (size_t)d->linesize[0], (size_t)d->width);
    for (int y = 0; y < d->height / 2; ++y)
        memcpy(image->bytes + y_size + (size_t)y * (size_t)d->width, f->data[1] + (size_t)y * (size_t)d->linesize[1], (size_t)d->width);
    image->size = y_size + y_size / 2; d->snapshot = (int)slot;
    atomic_store_explicit(&image->ready, true, memory_order_release);
}
int avfilter_graph_create_filter(AVFilterContext **context, const AVFilter *filter,
    const char *name, const char *args, void *opaque, AVFilterGraph *graph)
{
    int entry = errno; pthread_once(&once, initialize);
    if (!next_create) { errno = entry; return -ENOSYS; }
    bool watch = observing(); struct event *e = watch ? begin_event("avfilter_graph_create_filter", graph, NULL, true) : NULL;
    if (e) { copy_text(e->name, sizeof(e->name), filter ? filter->name : NULL); copy_text(e->args, sizeof(e->args), args); }
    ++forwarding; errno = entry; int result = next_create(context, filter, name, args, opaque, graph); int saved = errno; --forwarding;
    if (e && result >= 0 && context) e->context = (uintptr_t)*context;
    if (watch) end_event(e, result, entry, saved);
    errno = saved; return result;
}
int avfilter_graph_parse_ptr(AVFilterGraph *graph, const char *filters, AVFilterInOut **inputs, AVFilterInOut **outputs, void *log)
{
    int entry = errno; pthread_once(&once, initialize);
    if (!next_parse) { errno = entry; return -ENOSYS; }
    bool watch = observing(); struct event *e = watch ? begin_event("avfilter_graph_parse_ptr", graph, NULL, true) : NULL;
    if (e) copy_text(e->args, sizeof(e->args), filters);
    ++forwarding; errno = entry; int result = next_parse(graph, filters, inputs, outputs, log); int saved = errno; --forwarding;
    if (watch) end_event(e, result, entry, saved);
    errno = saved; return result;
}
static int add_frame(AVFilterContext *context, AVFrame *frame, int flags, bool explicit_flags)
{
    int entry = errno; pthread_once(&once, initialize);
    if ((explicit_flags && !next_add_flags) || (!explicit_flags && !next_add)) { errno = entry; return -ENOSYS; }
    bool watch = observing(); struct event *e = watch ? begin_event(explicit_flags ? "av_buffersrc_add_frame_flags" : "av_buffersrc_add_frame", context ? context->graph : NULL, context, false) : NULL;
    if (e) { e->input_null = !frame; e->flags = flags; capture_frame(e, frame, true); }
    ++forwarding; errno = entry;
    int result = explicit_flags ? next_add_flags(context, frame, flags) : next_add(context, frame);
    int saved = errno; --forwarding;
    if (watch) end_event(e, result, entry, saved);
    errno = saved; return result;
}
int av_buffersrc_add_frame(AVFilterContext *context, AVFrame *frame) { return add_frame(context, frame, 0, false); }
int av_buffersrc_add_frame_flags(AVFilterContext *context, AVFrame *frame, int flags) { return add_frame(context, frame, flags, true); }
int av_buffersrc_close(AVFilterContext *context, int64_t pts, unsigned flags)
{
    int entry = errno; pthread_once(&once, initialize);
    if (!next_close) { errno = entry; return -ENOSYS; }
    bool watch = observing(); struct event *e = watch ? begin_event("av_buffersrc_close", context ? context->graph : NULL, context, false) : NULL;
    if (e) { e->close_pts = pts; e->flags = (int)flags; }
    ++forwarding; errno = entry; int result = next_close(context, pts, flags); int saved = errno; --forwarding;
    if (watch) end_event(e, result, entry, saved);
    errno = saved; return result;
}
static int get_frame(AVFilterContext *context, AVFrame *frame, int flags, bool explicit_flags)
{
    int entry = errno; pthread_once(&once, initialize);
    if ((explicit_flags && !next_get_flags) || (!explicit_flags && !next_get)) { errno = entry; return -ENOSYS; }
    bool watch = observing(); struct event *e = watch ? begin_event(explicit_flags ? "av_buffersink_get_frame_flags" : "av_buffersink_get_frame", context ? context->graph : NULL, context, false) : NULL;
    if (e) e->flags = flags;
    ++forwarding; errno = entry;
    int result = explicit_flags ? next_get_flags(context, frame, flags) : next_get(context, frame);
    int saved = errno;
    if (e && result >= 0) {
        capture_frame(e, frame, false);
        e->sink_tb = next_sink_tb(context); e->sink_rate = next_sink_rate(context);
    }
    --forwarding;
    if (watch) end_event(e, result, entry, saved);
    errno = saved; return result;
}
int av_buffersink_get_frame(AVFilterContext *context, AVFrame *frame) { return get_frame(context, frame, 0, false); }
int av_buffersink_get_frame_flags(AVFilterContext *context, AVFrame *frame, int flags) { return get_frame(context, frame, flags, true); }
void avfilter_graph_free(AVFilterGraph **graph)
{
    int entry = errno; pthread_once(&once, initialize);
    if (!next_free) { errno = entry; return; }
    bool watch = observing();
    if (watch && (!graph || !*graph)) { atomic_fetch_sub(&inflight, 1); watch = false; }
    struct event *e = watch ? begin_event("avfilter_graph_free", *graph, NULL, false) : NULL;
    if (e) e->void_ret = true;
    ++forwarding; errno = entry; next_free(graph); int saved = errno; --forwarding;
    if (e && e->graph_id) {
        pthread_mutex_lock(&lock);
        struct graph_state *g = &graphs[e->graph_id - 1];
        if (!g->open || g->busy != 1) atomic_fetch_add(&lifecycle_failures, 1);
        g->open = false;
        pthread_mutex_unlock(&lock);
    }
    if (watch) end_event(e, 0, entry, saved);
    errno = saved;
}
static bool write_all(int fd, const void *data, size_t size)
{
    const unsigned char *p = data;
    while (size) {
        ssize_t n = write(fd, p, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n; size -= (size_t)n;
    }
    return true;
}
static bool json_string(int fd, const char *text)
{
    if (!write_all(fd, "\"", 1)) return false;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        char escaped[7]; size_t n = 1;
        if (*p == '"' || *p == '\\') { escaped[0] = '\\'; escaped[1] = (char)*p; n = 2; }
        else if (*p < 32 || *p >= 127) { snprintf(escaped, sizeof(escaped), "\\u%04x", *p); n = 6; }
        else escaped[0] = (char)*p;
        if (!write_all(fd, escaped, n)) return false;
    }
    return write_all(fd, "\"", 1);
}
static bool print_event(int fd, unsigned seq, const struct event *e)
{
    if (dprintf(fd, "{\"type\":\"event\",\"seq\":%u,\"pid\":%ld,\"tid\":%ld,\"begin_ns\":%" PRIu64 ",\"end_ns\":%" PRIu64 ",\"api\":\"%s\",\"graph_id\":%" PRIu64 ",\"graph\":\"0x%" PRIxPTR "\",\"context\":\"0x%" PRIxPTR "\",\"ret\":", seq, (long)owner_pid, (long)e->tid, e->begin, e->end, e->api, e->graph_id, e->graph, e->context) < 0) return false;
    if (e->void_ret ? dprintf(fd, "null") < 0 : dprintf(fd, "%d", e->ret) < 0) return false;
    if (dprintf(fd, ",\"errno_before\":%d,\"errno_after\":%d,\"flags\":%d,\"input_null\":%s,\"close_pts\":%" PRId64 ",\"input_index\":%u,\"output_index\":%u,\"name\":", e->before_errno, e->after_errno, e->flags, e->input_null ? "true" : "false", e->close_pts, e->input_index, e->output_index) < 0 || !json_string(fd, e->name) || dprintf(fd, ",\"args\":") < 0 || !json_string(fd, e->args)) return false;
    if (!e->has_frame) return dprintf(fd, ",\"frame\":null}\n") >= 0;
    const struct frame_data *f = &e->frame;
    if (dprintf(fd, ",\"sink_time_base\":[%d,%d],\"sink_frame_rate\":[%d,%d],\"frame\":{\"pts\":%" PRId64 ",\"best_effort_timestamp\":%" PRId64 ",\"duration\":%" PRId64 ",\"time_base\":[%d,%d],\"format\":%d,\"width\":%d,\"height\":%d,\"linesize\":[%d,%d],\"flags\":%d,\"sample_aspect_ratio\":[%d,%d],\"color_range\":%d,\"color_primaries\":%d,\"color_trc\":%d,\"colorspace\":%d,\"chroma_location\":%d,\"crop\":[%zu,%zu,%zu,%zu],\"snapshot\":", e->sink_tb.num, e->sink_tb.den, e->sink_rate.num, e->sink_rate.den, f->pts, f->best, f->duration, f->time_base.num, f->time_base.den, f->format, f->width, f->height, f->linesize[0], f->linesize[1], f->flags, f->sar.num, f->sar.den, f->range, f->primaries, f->trc, f->space, f->chroma, f->crop[0], f->crop[1], f->crop[2], f->crop[3]) < 0) return false;
    if (f->snapshot < 0) { if (dprintf(fd, "null") < 0) return false; }
    else {
        const char *base = strrchr(prefix, '/'); base = base ? base + 1 : prefix;
        char name[PATH_MAX]; int n = snprintf(name, sizeof(name), "%s-%ld-s%02d.nv12", base, (long)owner_pid, f->snapshot);
        if (n < 0 || (size_t)n >= sizeof(name) || dprintf(fd, "{\"slot\":%d,\"file\":", f->snapshot) < 0 || !json_string(fd, name) || dprintf(fd, ",\"bytes\":%zu}", images[f->snapshot].size) < 0) return false;
    }
    return dprintf(fd, "}}\n") >= 0;
}
__attribute__((destructor)) static void finish_observer(void)
{
    int saved = errno; pthread_once(&once, initialize);
    if (!active || owner_pid != getpid()) { errno = saved; return; }
    /* Close the start gate before sampling inflight. An exiting process with
     * an already active wrapper publishes an explicitly incomplete ledger. */
    pthread_mutex_lock(&lock);
    publishing = true;
    unsigned busy = atomic_load_explicit(&inflight, memory_order_acquire);
    unsigned open_graphs = 0;
    if (!busy) for (unsigned i = 0; i < graph_count; ++i) if (graphs[i].open) ++open_graphs;
    pthread_mutex_unlock(&lock);
    unsigned total = atomic_load_explicit(&attempted, memory_order_acquire);
    unsigned n = total < POSTFILTER_CAPACITY ? total : POSTFILTER_CAPACITY;
    unsigned snaps = atomic_load_explicit(&snapshot_count, memory_order_acquire);
    unsigned ns = snaps < POSTFILTER_SNAPSHOTS ? snaps : POSTFILTER_SNAPSHOTS;
    unsigned unpublished = 0;
    for (unsigned i = 0; i < n; ++i) if (!atomic_load_explicit(&events[i].ready, memory_order_acquire)) ++unpublished;
    for (unsigned i = 0; i < ns; ++i) {
        if (!atomic_load_explicit(&images[i].ready, memory_order_acquire)) {
            /* One failed selected attempt is one failure, not a second failure
             * merely because its rejected payload is correctly unpublished. */
            if (!atomic_exchange_explicit(&images[i].failed, true, memory_order_acq_rel)) atomic_fetch_add(&snapshot_failures, 1);
            continue;
        }
        char path[PATH_MAX]; int m = snprintf(path, sizeof(path), "%s-%ld-s%02u.nv12", prefix, (long)owner_pid, i);
        if (m <= 0 || (size_t)m >= sizeof(path)) { atomic_fetch_add(&snapshot_failures, 1); continue; }
        int out = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (out < 0) { atomic_fetch_add(&snapshot_failures, 1); continue; }
        bool wrote = write_all(out, images[i].bytes, images[i].size);
        if (close(out)) wrote = false;
        if (!wrote) atomic_fetch_add(&snapshot_failures, 1);
    }
    char path[PATH_MAX]; int m = snprintf(path, sizeof(path), "%s-%ld.jsonl", prefix, (long)owner_pid);
    if (m <= 0 || (size_t)m >= sizeof(path)) { errno = saved; return; }
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) { errno = saved; return; }
    bool ok = dprintf(fd, "{\"type\":\"header\",\"schema\":\"crystalhd-postfilter-v1\",\"pid\":%ld,\"capacity\":%d,\"snapshot_capacity\":%d,\"max_width\":%d,\"max_height\":%d,\"nv12_format\":%d,\"sink_constraint_basis\":\"PINNED_ORIGINAL_SOURCE_PLUS_ACTUAL_NV12_SINK_OUTPUT\",\"filter_provider\":", (long)owner_pid, POSTFILTER_CAPACITY, POSTFILTER_SNAPSHOTS, MAX_WIDTH, MAX_HEIGHT, AV_PIX_FMT_NV12) >= 0 && json_string(fd, filter_path) && dprintf(fd, ",\"util_provider\":") >= 0 && json_string(fd, util_path) && dprintf(fd, ",\"filter_version\":%u,\"util_version\":%u,\"provider_ok\":%s}\n", filter_version, util_version, provider_ok ? "true" : "false") >= 0;
    unsigned published = 0;
    for (unsigned i = 0; ok && i < n; ++i) if (atomic_load_explicit(&events[i].ready, memory_order_acquire)) { ok = print_event(fd, i, &events[i]); ++published; }
    unsigned lost = atomic_load(&dropped), failures = atomic_load(&snapshot_failures);
    unsigned lifecycle = atomic_load(&lifecycle_failures), truncated = atomic_load(&string_truncations);
    bool clock = atomic_load(&clock_failure), forked = atomic_load(&fork_seen);
    bool complete = ok && provider_ok && !lost && !failures && !lifecycle && !truncated && !clock && !forked && !busy && !unpublished && !open_graphs;
    if (ok) (void)dprintf(fd, "{\"type\":\"footer\",\"events\":%u,\"attempted_events\":%u,\"dropped\":%u,\"snapshots\":%u,\"snapshot_failures\":%u,\"lifecycle_failures\":%u,\"string_truncations\":%u,\"clock_failure\":%s,\"fork_seen\":%s,\"inflight\":%u,\"unpublished\":%u,\"open_graphs\":%u,\"complete\":%s}\n", published, total, lost, snaps, failures, lifecycle, truncated, clock ? "true" : "false", forked ? "true" : "false", busy, unpublished, open_graphs, complete ? "true" : "false");
    close(fd); errno = saved;
}
