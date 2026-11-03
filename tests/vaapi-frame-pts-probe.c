// SPDX-License-Identifier: GPL-2.0-or-later
/* OWN isolated diagnostic interposer. This does not qualify stock compatibility.
 * Build with unchanged FFmpeg63.1.102/util61.1.102 headers, never private libs.
 * Portable synthetic-header controls do not establish this runtime ABI. */
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
#include <libavcodec/avcodec.h>
#include <libavcodec/version.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libavutil/version.h>
#if LIBAVCODEC_VERSION_INT != AV_VERSION_INT(63, 1, 102) || LIBAVUTIL_VERSION_INT != AV_VERSION_INT(61, 1, 102)
#error "Frame observer requires original codec63.1.102/util61.1.102 headers"
#endif
#ifndef FRAME_PTS_CAPACITY
#define FRAME_PTS_CAPACITY 8192
#endif
#if FRAME_PTS_CAPACITY < 2 || FRAME_PTS_CAPACITY > 65536
#error "Invalid fixed observer capacity"
#endif
#define CODEC_PROVIDER "/app/lib/libavcodec.so.63.1.102"
#define UTIL_PROVIDER "/app/lib/libavutil.so.61.1.102"
typedef int (*receive_fn)(AVCodecContext *, AVFrame *);
typedef unsigned (*version_fn)(void);
struct snapshot {
    uint64_t begin, end;
    uintptr_t codec, frame, pool;
    int64_t pts, best;
    int frame_num, frame_den, packet_num, packet_den, format;
    uint32_t surface;
    pid_t tid;
    atomic_bool ready;
};
static struct snapshot samples[FRAME_PTS_CAPACITY];
static atomic_uint count, dropped, inflight;
static atomic_bool clock_failure;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static receive_fn original_receive;
static bool active, provider_ok;
static unsigned codec_version, util_version;
static pid_t buffer_pid;
static char output_path[PATH_MAX], codec_path[PATH_MAX], util_path[PATH_MAX];
static bool copy_path(char *out, const char *value)
{
    if (!value || value[0] != '/' || strlen(value) >= PATH_MAX) return false;
    memcpy(out, value, strlen(value) + 1); return true;
}
static bool describe(void *symbol, char *path, Dl_info *info)
{
    memset(info, 0, sizeof(*info));
    return symbol && dladdr(symbol, info) && info->dli_fname &&
           realpath(info->dli_fname, path) != NULL;
}
static bool expected_path(const char *actual, const char *variable, const char *fallback)
{
    const char *wanted = getenv(variable);
    if (!wanted) wanted = fallback;
    char resolved[PATH_MAX];
    return wanted[0] == '/' && realpath(wanted, resolved) && strcmp(actual, resolved) == 0;
}
static void initialize(void)
{
    int saved = errno;
    buffer_pid = getpid();
    void *receive_symbol = dlvsym(RTLD_NEXT, "avcodec_receive_frame", "LIBAVCODEC_63");
    bool versioned = receive_symbol != NULL;
    if (!receive_symbol) receive_symbol = dlsym(RTLD_NEXT, "avcodec_receive_frame");
    original_receive = (receive_fn)receive_symbol;
    const char *marker = getenv("CRYSTALHD_TEST_FRAME_PTS");
    active = marker && strcmp(marker, "v1") == 0 &&
             copy_path(output_path, getenv("CRYSTALHD_TEST_FRAME_PTS_PREFIX"));
    if (active) {
        void *cv_symbol = dlvsym(RTLD_NEXT, "avcodec_version", "LIBAVCODEC_63");
        void *uv_symbol = dlvsym(RTLD_NEXT, "avutil_version", "LIBAVUTIL_61");
        Dl_info receive_info, cv_info, uv_info; char receive_path[PATH_MAX];
        bool paths = describe(receive_symbol, receive_path, &receive_info) &&
                     describe(cv_symbol, codec_path, &cv_info) && describe(uv_symbol, util_path, &uv_info);
        if (paths) {
            codec_version = ((version_fn)cv_symbol)(); util_version = ((version_fn)uv_symbol)();
            provider_ok = versioned && receive_info.dli_fbase == cv_info.dli_fbase &&
                          strcmp(receive_path, codec_path) == 0 &&
                          expected_path(codec_path, "CRYSTALHD_TEST_FRAME_PTS_CODEC_PROVIDER", CODEC_PROVIDER) &&
                          expected_path(util_path, "CRYSTALHD_TEST_FRAME_PTS_UTIL_PROVIDER", UTIL_PROVIDER) &&
                          codec_version == LIBAVCODEC_VERSION_INT && util_version == LIBAVUTIL_VERSION_INT;
        }
    }
    errno = saved;
}
static uint64_t now_ns(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        atomic_store_explicit(&clock_failure, true, memory_order_relaxed); return 0;
    }
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) + (uint64_t)value.tv_nsec;
}
int avcodec_receive_frame(AVCodecContext *codec, AVFrame *frame)
{
    int entry_errno = errno;
    pthread_once(&once, initialize); errno = entry_errno;
    /* No next implementation means no original result exists to forward;
     * such a resolver failure cannot produce a qualified observation. */
    if (!original_receive) return -ENOSYS;
    /* A pre-exec fork inherits records belonging to its parent. Forward calls
     * without observing that inherited state; exec starts a fresh observer. */
    bool observing = active && buffer_pid == getpid();
    if (observing) atomic_fetch_add_explicit(&inflight, 1, memory_order_relaxed);
    errno = entry_errno;
    int result = original_receive(codec, frame); int saved = errno;
    if (observing && provider_ok && result == 0 && frame && codec) {
        uint64_t begin = now_ns(); int format = frame->format;
        if (format == AV_PIX_FMT_VAAPI) {
            unsigned index = atomic_fetch_add_explicit(&count, 1, memory_order_relaxed);
            if (index < FRAME_PTS_CAPACITY) {
                struct snapshot *s = &samples[index];
                s->begin = begin; s->codec = (uintptr_t)codec; s->frame = (uintptr_t)frame;
                s->pool = frame->hw_frames_ctx ? (uintptr_t)frame->hw_frames_ctx->data : 0;
                s->surface = (uint32_t)(uintptr_t)frame->data[3]; s->format = format;
                s->pts = frame->pts; s->best = frame->best_effort_timestamp;
                s->frame_num = frame->time_base.num; s->frame_den = frame->time_base.den;
                s->packet_num = codec->pkt_timebase.num; s->packet_den = codec->pkt_timebase.den;
                s->tid = (pid_t)syscall(SYS_gettid); s->end = now_ns();
                atomic_store_explicit(&s->ready, true, memory_order_release);
            } else atomic_fetch_add_explicit(&dropped, 1, memory_order_relaxed);
        }
    }
    if (observing) atomic_fetch_sub_explicit(&inflight, 1, memory_order_release);
    errno = saved; return result;
}
static bool write_all(int fd, const char *text, size_t size)
{
    while (size) {
        ssize_t n = write(fd, text, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        text += n; size -= (size_t)n;
    }
    return true;
}
static bool json_string(int fd, const char *value)
{
    if (!write_all(fd, "\"", 1)) return false;
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        char escaped[7]; size_t n = 1;
        if (*p == '"' || *p == '\\') { escaped[0] = '\\'; escaped[1] = (char)*p; n = 2; }
        else if (*p < 32 || *p >= 127) { snprintf(escaped, sizeof(escaped), "\\u%04x", *p); n = 6; }
        else escaped[0] = (char)*p;
        if (!write_all(fd, escaped, n)) return false;
    }
    return write_all(fd, "\"", 1);
}
__attribute__((destructor)) static void flush_observations(void)
{
    int saved = errno;
    pthread_once(&once, initialize);
    if (!active || buffer_pid != getpid()) { errno = saved; return; }
    /* PREFIX is absolute. Inherited preload configuration must not
     * let an exec child consume the media process's O_EXCL output file. */
    char report_path[PATH_MAX];
    int path_size = snprintf(report_path, sizeof(report_path), "%s-%ld.jsonl", output_path, (long)getpid());
    if (path_size <= 0 || (size_t)path_size >= sizeof(report_path)) { errno = saved; return; }
    int fd = open(report_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) { errno = saved; return; }
    char line[1400]; unsigned total = atomic_load_explicit(&count, memory_order_acquire);
    unsigned n = total < FRAME_PTS_CAPACITY ? total : FRAME_PTS_CAPACITY;
    unsigned lost = atomic_load_explicit(&dropped, memory_order_acquire);
    bool busy = atomic_load_explicit(&inflight, memory_order_acquire) != 0;
    bool bad_clock = atomic_load_explicit(&clock_failure, memory_order_relaxed);
    bool complete = provider_ok && original_receive && !lost && !busy && !bad_clock;
    int len = snprintf(line, sizeof(line), "{\"type\":\"header\",\"schema\":\"crystalhd-frame-pts-v1\",\"pid\":%ld,\"capacity\":%u,\"provider_ok\":%s,\"codec_version\":%u,\"util_version\":%u,\"vaapi_format\":%d,\"codec_provider\":", (long)getpid(), FRAME_PTS_CAPACITY, provider_ok ? "true" : "false", codec_version, util_version, AV_PIX_FMT_VAAPI);
    bool ok = len > 0 && (size_t)len < sizeof(line) && write_all(fd, line, (size_t)len) &&
              json_string(fd, codec_path) && write_all(fd, ",\"util_provider\":", 17) &&
              json_string(fd, util_path) && write_all(fd, "}\n", 2);
    unsigned written = 0;
    /* OWN runtime retires decoders before normal Quit; thread controls join all
     * workers. A concurrent caller at teardown invalidates this receipt, and
     * this destructor must never be used as a live-process flush operation. */
    for (unsigned i = 0; ok && i < n; ++i) {
        struct snapshot *s = &samples[i];
        if (!atomic_load_explicit(&s->ready, memory_order_acquire)) { complete = false; continue; }
        len = snprintf(line, sizeof(line), "{\"type\":\"frame\",\"seq\":%u,\"pid\":%ld,\"tid\":%ld,\"snapshot_begin_ns\":%" PRIu64 ",\"snapshot_end_ns\":%" PRIu64 ",\"codec\":\"0x%" PRIxPTR "\",\"frame\":\"0x%" PRIxPTR "\",\"pool\":\"0x%" PRIxPTR "\",\"display\":null,\"surface\":%" PRIu32 ",\"format\":%d,\"pts\":%" PRId64 ",\"best_effort_timestamp\":%" PRId64 ",\"frame_time_base\":[%d,%d],\"packet_time_base\":[%d,%d]}\n", i + 1, (long)getpid(), (long)s->tid, s->begin, s->end, s->codec, s->frame, s->pool, s->surface, s->format, s->pts, s->best, s->frame_num, s->frame_den, s->packet_num, s->packet_den);
        ok = len > 0 && (size_t)len < sizeof(line) && write_all(fd, line, (size_t)len);
        if (ok) ++written;
    }
    if (written != n) complete = false;
    len = snprintf(line, sizeof(line), "{\"type\":\"footer\",\"records\":%u,\"dropped\":%u,\"complete\":%s,\"provider_ok\":%s,\"errors\":[%s%s%s%s%s]}\n", written, lost, complete ? "true" : "false", provider_ok ? "true" : "false", !provider_ok ? "\"provider_guard_failed\"" : "", lost ? (!provider_ok ? ",\"buffer_overflow\"" : "\"buffer_overflow\"") : "", busy ? ((!provider_ok || lost) ? ",\"inflight_at_shutdown\"" : "\"inflight_at_shutdown\"") : "", bad_clock ? ((!provider_ok || lost || busy) ? ",\"clock_failed\"" : "\"clock_failed\"") : "", written != n ? ((!provider_ok || lost || busy || bad_clock) ? ",\"unpublished_record\"" : "\"unpublished_record\"") : "");
    if (ok && len > 0 && (size_t)len < sizeof(line)) write_all(fd, line, (size_t)len);
    close(fd); errno = saved;
}
