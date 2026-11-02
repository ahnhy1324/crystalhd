/* CrystalHD-owned offline MPEG1 chroma/reference research helper.
 * Build privately: cc -std=c11 -O2 -Wall -Wextra -Werror \
 *   tests/mpeg1-chroma-convert.c -ldl -o NEW_CONVERTER
 * Public APIs only; no SwsContext field access or external source changes.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <link.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>

enum { WIDTH = 352, HEIGHT = 288, MAX_OPTIONS = 16, MAX_PROVIDERS = 24 };
struct Provider { const char *symbol; char path[PATH_MAX]; };
struct Option { const char *name; int64_t requested, after_init, after_scale; };
struct Api {
    __typeof__(&swscale_version) scale_version;
    __typeof__(&swscale_configuration) scale_configuration;
    __typeof__(&sws_alloc_context) alloc;
    __typeof__(&sws_init_context) init;
    __typeof__(&sws_scale) scale;
    __typeof__(&sws_freeContext) release;
    __typeof__(&sws_getCoefficients) coefficients;
    __typeof__(&sws_setColorspaceDetails) set_colorspace;
    __typeof__(&sws_getColorspaceDetails) get_colorspace;
    __typeof__(&avutil_version) util_version;
    __typeof__(&avutil_configuration) util_configuration;
    __typeof__(&av_get_pix_fmt) pix_fmt;
    __typeof__(&av_opt_set_int) set_int;
    __typeof__(&av_opt_get_int) get_int;
    __typeof__(&av_image_alloc) image_alloc;
    __typeof__(&av_freep) freep;
};
static struct Provider providers[MAX_PROVIDERS];
static size_t provider_count;
static char expected_scale[PATH_MAX], expected_util[PATH_MAX];
static char failure[512] = "uninitialized";

static void fail(const char *reason) {
    snprintf(failure, sizeof(failure), "%s", reason);
    fprintf(stderr, "issue529: %s\n", reason);
}
static void json_string(FILE *f, const char *s) {
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; ++p) {
        if (*p == '"' || *p == '\\') { fputc('\\', f); fputc(*p, f); }
        else if (*p < 32) fprintf(f, "\\u%04x", *p);
        else fputc(*p, f);
    }
    fputc('"', f);
}
static bool canonical(const char *path, char out[PATH_MAX]) {
    if (!path || path[0] != '/' || !realpath(path, out)) {
        fail("absolute library path cannot be resolved"); return false;
    }
    return true;
}
static bool provider_matches(void *address, const char *name, const char *expected) {
    Dl_info info;
    char found[PATH_MAX];
    if (!address || !dladdr(address, &info) || !canonical(info.dli_fname, found) ||
        strcmp(found, expected) || provider_count == MAX_PROVIDERS) {
        fail("unexpected or unresolvable dynamic symbol provider"); return false;
    }
    providers[provider_count].symbol = name;
    snprintf(providers[provider_count++].path, PATH_MAX, "%s", found);
    return true;
}
static bool bind_symbol(void *handle, const char *name, const char *expected,
                        void *destination, size_t bytes) {
    dlerror();
    void *address = dlsym(handle, name);
    const char *error = dlerror();
    if (error || bytes != sizeof(address) ||
        !provider_matches(address, name, expected)) {
        fail("required public API symbol/provider missing"); return false;
    }
    memcpy(destination, &address, bytes);
    return true;
}
#define BIND(field, handle, name, expected) \
    do { if (!bind_symbol(handle, name, expected, &api.field, sizeof(api.field))) goto finish; } while (0)

static int check_loaded_provider(struct dl_phdr_info *info, size_t size, void *bad) {
    (void)size;
    const char *path = info->dlpi_name;
    if (!path || (!strstr(path, "libswscale.so") && !strstr(path, "libavutil.so"))) return 0;
    char found[PATH_MAX];
    if (!canonical(path, found) ||
        (strcmp(found, expected_scale) && strcmp(found, expected_util))) *(bool *)bad = true;
    return 0;
}
static bool set_option(struct Api *api, struct SwsContext *ctx,
                       struct Option *opts, size_t *count, const char *name, int64_t value) {
    if (*count >= MAX_OPTIONS || api->set_int(ctx, name, value, 0) < 0) {
        fail("setting a declared AVOption failed"); return false;
    }
    opts[*count] = (struct Option){name, value, INT64_MIN, INT64_MIN};
    ++*count;
    return true;
}
static bool snapshot(struct Api *api, struct SwsContext *ctx,
                     struct Option *opts, size_t count, bool after_scale) {
    for (size_t n = 0; n < count; ++n) {
        int64_t value;
        if (api->get_int(ctx, opts[n].name, 0, &value) < 0) {
            fail("reading a declared AVOption failed"); return false;
        }
        if (after_scale) opts[n].after_scale = value;
        else opts[n].after_init = value;
        /* An automatic position may be resolved internally; record that value.
         * Explicit options and all other options must remain requested values. */
        if (opts[n].requested != -513 && value != opts[n].requested) {
            fail("explicit AVOption changed unexpectedly"); return false;
        }
    }
    return true;
}
static bool colorspace_matches(struct Api *api, struct SwsContext *ctx,
                               const int expected[4], int actual[8]) {
    int *inverse = NULL, *forward = NULL, src_range, dst_range, brightness, contrast, saturation;
    if (api->get_colorspace(ctx, &inverse, &src_range, &forward, &dst_range,
                           &brightness, &contrast, &saturation) < 0 || !inverse || !forward) {
        fail("colorspace details unavailable"); return false;
    }
    for (int n = 0; n < 4; ++n) {
        actual[n] = inverse[n]; actual[n + 4] = forward[n];
        if (inverse[n] != expected[n] || forward[n] != expected[n]) {
            fail("colorspace coefficients differ from declared601"); return false;
        }
    }
    if (src_range || dst_range || brightness || contrast != (1 << 16) || saturation != (1 << 16)) {
        fail("colorspace/range/brightness controls changed"); return false;
    }
    return true;
}
static void active_copy(uint8_t *linear, uint8_t *const data[4], const int stride[4],
                        bool planar, bool to_image) {
    size_t offset = 0;
    const int planes = planar ? 3 : 1;
    for (int p = 0; p < planes; ++p) {
        const int rows = planar && p ? HEIGHT / 2 : HEIGHT;
        const int cols = planar ? (p ? WIDTH / 2 : WIDTH) : WIDTH * 2;
        for (int y = 0; y < rows; ++y) {
            if (to_image) memcpy(data[p] + (size_t)y * stride[p], linear + offset, (size_t)cols);
            else memcpy(linear + offset, data[p] + (size_t)y * stride[p], (size_t)cols);
            offset += (size_t)cols;
        }
    }
}
static bool siting(const char *name, bool planar, int *horizontal, int *vertical) {
    if (!strcmp(name, "auto")) *horizontal = *vertical = -513;
    else if (!strcmp(name, "left")) { *horizontal = 0; *vertical = planar ? 128 : 0; }
    else if (!strcmp(name, "center")) { *horizontal = 128; *vertical = planar ? 128 : 0; }
    else return false;
    return true;
}

static bool version_argument(const char *text, unsigned *result) {
    unsigned parts[3];
    const char *position = text;
    for (unsigned index = 0; index < 3; ++index) {
        if (!position || *position < '0' || *position > '9') return false;
        errno = 0;
        char *end = NULL;
        unsigned long value = strtoul(position, &end, 10);
        if (errno || value > 255 || end == position ||
            (index < 2 ? *end != '.' : *end != '\0')) return false;
        parts[index] = (unsigned)value;
        position = end + (index < 2 ? 1 : 0);
    }
    if (!parts[0]) return false;
    *result = (parts[0] << 16) | (parts[1] << 8) | parts[2];
    return true;
}
static int allowed_flags(const char *text) {
    if (!strcmp(text, "4")) return SWS_BICUBIC;
    if (!strcmp(text, "16")) return SWS_POINT;
    if (!strcmp(text, "2")) return SWS_BILINEAR;
    if (!strcmp(text, "512")) return SWS_LANCZOS;
    return -1;
}
static const char *filter_name(int flags) {
    switch (flags) {
    case SWS_BICUBIC: return "bicubic";
    case SWS_POINT: return "point";
    case SWS_BILINEAR: return "bilinear";
    case SWS_LANCZOS: return "lanczos";
    default: return "invalid";
    }
}
int main(int argc, char **argv) {
    if (argc != 14) {
        fprintf(stderr, "usage: %s RUNTIME ABS_SCALE_DSO ABS_UTIL_DSO EXPECT_SCALE_VERSION EXPECT_UTIL_VERSION FLAGS MODE SRC_SITING DST_SITING 30|90 INPUT NEW_OUTPUT NEW_REPORT\n", argv[0]);
        return 2;
    }
    const char *runtime = argv[1], *scale_path = argv[2], *util_path = argv[3], *mode = argv[7];
    unsigned expected_scale_version, expected_util_version;
    const int conversion_flags = allowed_flags(argv[6]);
    const bool conversion = !strcmp(mode, "i420-yuyv");
    const bool identity420 = !strcmp(mode, "i420-i420");
    const bool identity422 = !strcmp(mode, "yuyv-yuyv");
    const bool source_planar = conversion || identity420, destination_planar = identity420;
    const unsigned frames = !strcmp(argv[10], "30") ? 30 : !strcmp(argv[10], "90") ? 90 : 0;
    int src_h, src_v, dst_h, dst_v;
    if (!*runtime || strlen(runtime) > 80 || conversion_flags < 0 ||
        !version_argument(argv[4], &expected_scale_version) ||
        !version_argument(argv[5], &expected_util_version) ||
        (!conversion && !identity420 && !identity422) || !frames ||
        !siting(argv[8], source_planar, &src_h, &src_v) ||
        !siting(argv[9], destination_planar, &dst_h, &dst_v) ||
        (!conversion && strcmp(argv[8], argv[9]))) {
        fprintf(stderr, "issue529: arguments outside declared bounded protocol\n"); return 2;
    }
    FILE *report = fopen(argv[13], "wx");
    if (!report) { perror("new report"); return 2; }
    int exit_status = 1;
    struct Api api = {0};
    void *util_handle = NULL, *scale_handle = NULL;
    struct SwsContext *context = NULL;
    FILE *input = NULL, *output = NULL;
    uint8_t *src[4] = {0}, *dst[4] = {0}, *linear_in = NULL, *linear_out = NULL;
    int src_stride[4] = {0}, dst_stride[4] = {0};
    struct Option options[MAX_OPTIONS]; size_t option_count = 0;
    unsigned completed = 0, identity_changed_frames = 0, scale_version = 0, util_version = 0;
    int coefficients[4] = {0}, colorspace[8] = {0};
    bool colorspace_verified = false, providers_verified = false;
    const size_t source_bytes = source_planar ? WIDTH * HEIGHT * 3U / 2U : WIDTH * HEIGHT * 2U;
    const size_t destination_bytes = destination_planar ? WIDTH * HEIGHT * 3U / 2U : WIDTH * HEIGHT * 2U;
    const char *scale_configuration = NULL, *util_configuration = NULL;
    const char *preload = getenv("LD_PRELOAD"), *audit = getenv("LD_AUDIT"), *library_path = getenv("LD_LIBRARY_PATH");
    if ((preload && *preload) || (audit && *audit) || (library_path && *library_path)) {
        fail("LD_PRELOAD,LD_AUDIT,LD_LIBRARY_PATH must be absent or empty"); goto finish;
    }
    if (!canonical(scale_path, expected_scale) || !canonical(util_path, expected_util)) goto finish;
    util_handle = dlopen(expected_util, RTLD_NOW | RTLD_LOCAL);
    scale_handle = dlopen(expected_scale, RTLD_NOW | RTLD_LOCAL);
    if (!util_handle || !scale_handle) { fail("exact runtime library load failed"); goto finish; }
    BIND(scale_version, scale_handle, "swscale_version", expected_scale);
    BIND(scale_configuration, scale_handle, "swscale_configuration", expected_scale);
    BIND(alloc, scale_handle, "sws_alloc_context", expected_scale);
    BIND(init, scale_handle, "sws_init_context", expected_scale);
    BIND(scale, scale_handle, "sws_scale", expected_scale);
    BIND(release, scale_handle, "sws_freeContext", expected_scale);
    BIND(coefficients, scale_handle, "sws_getCoefficients", expected_scale);
    BIND(set_colorspace, scale_handle, "sws_setColorspaceDetails", expected_scale);
    BIND(get_colorspace, scale_handle, "sws_getColorspaceDetails", expected_scale);
    BIND(util_version, util_handle, "avutil_version", expected_util);
    BIND(util_configuration, util_handle, "avutil_configuration", expected_util);
    BIND(pix_fmt, util_handle, "av_get_pix_fmt", expected_util);
    BIND(set_int, util_handle, "av_opt_set_int", expected_util);
    BIND(get_int, util_handle, "av_opt_get_int", expected_util);
    BIND(image_alloc, util_handle, "av_image_alloc", expected_util);
    BIND(freep, util_handle, "av_freep", expected_util);
    /* Resolve dependencies through the scale handle as an extra binding check. */
    if (!provider_matches(dlsym(scale_handle, "av_opt_set_int"), "scale-dependency:av_opt_set_int", expected_util) ||
        !provider_matches(dlsym(scale_handle, "avutil_version"), "scale-dependency:avutil_version", expected_util)) goto finish;
    bool unexpected = false;
    dl_iterate_phdr(check_loaded_provider, &unexpected);
    if (unexpected) { fail("an unexpected avutil/swscale object is loaded"); goto finish; }
    scale_version = api.scale_version(); util_version = api.util_version();
    if (scale_version != expected_scale_version || util_version != expected_util_version) {
        fail("loaded runtime full version differs from declared exact provider pair"); goto finish;
    }
    providers_verified = true;
    scale_configuration = api.scale_configuration(); util_configuration = api.util_configuration();
    enum AVPixelFormat source_format = api.pix_fmt(source_planar ? "yuv420p" : "yuyv422");
    enum AVPixelFormat destination_format = api.pix_fmt(destination_planar ? "yuv420p" : "yuyv422");
    if (source_format < 0 || destination_format < 0) { fail("pixel format lookup failed"); goto finish; }
    context = api.alloc();
    if (!context) { fail("context allocation failed"); goto finish; }
#define SET(name, value) do { if (!set_option(&api, context, options, &option_count, name, value)) goto finish; } while (0)
    SET("srcw", WIDTH); SET("srch", HEIGHT); SET("dstw", WIDTH); SET("dsth", HEIGHT);
    SET("src_format", source_format); SET("dst_format", destination_format);
    SET("sws_flags", conversion_flags); SET("src_range", 0); SET("dst_range", 0);
    SET("src_h_chr_pos", src_h); SET("src_v_chr_pos", src_v);
    SET("dst_h_chr_pos", dst_h); SET("dst_v_chr_pos", dst_v); SET("threads", 1);
    if (api.init(context, NULL, NULL) < 0 || !snapshot(&api, context, options, option_count, false)) {
        fail("context init or option verification failed"); goto finish;
    }
    const int *chosen = api.coefficients(SWS_CS_ITU601);
    if (!chosen) { fail("601 coefficient lookup failed"); goto finish; }
    memcpy(coefficients, chosen, sizeof(coefficients));
    if (api.set_colorspace(context, coefficients, 0, coefficients, 0, 0, 1 << 16, 1 << 16) < 0) {
        fail("setting the declared limited601 colorspace failed"); goto finish;
    }
    if (!colorspace_matches(&api, context, coefficients, colorspace)) goto finish;
    colorspace_verified = true;
    if (api.image_alloc(src, src_stride, WIDTH, HEIGHT, source_format, 64) < 0 ||
        api.image_alloc(dst, dst_stride, WIDTH, HEIGHT, destination_format, 64) < 0) {
        fail("aligned image allocation failed"); goto finish;
    }
    linear_in = malloc(source_bytes); linear_out = malloc(destination_bytes);
    input = fopen(argv[11], "rb");
    struct stat input_stat;
    if (!linear_in || !linear_out || !input || fstat(fileno(input), &input_stat) ||
        !S_ISREG(input_stat.st_mode) || input_stat.st_size != (off_t)(source_bytes * frames)) {
        fail("input is not an exact-size regular frozen raw stream"); goto finish;
    }
    int output_fd = open(argv[12], O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (output_fd < 0) { fail("new output cannot be created exclusively"); goto finish; }
    output = fdopen(output_fd, "wb");
    if (!output) { close(output_fd); fail("output stream creation failed"); goto finish; }
    for (unsigned frame = 0; frame < frames; ++frame) {
        if (fread(linear_in, 1, source_bytes, input) != source_bytes) {
            fail("short source frame"); goto finish;
        }
        active_copy(linear_in, src, src_stride, source_planar, true);
        if (api.scale(context, (const uint8_t *const *)src, src_stride, 0, HEIGHT, dst, dst_stride) != HEIGHT) {
            fail("conversion did not return the complete frame height"); goto finish;
        }
        active_copy(linear_out, dst, dst_stride, destination_planar, false);
        if (!conversion && memcmp(linear_in, linear_out, source_bytes)) ++identity_changed_frames;
        if (fwrite(linear_out, 1, destination_bytes, output) != destination_bytes) {
            fail("short output write"); goto finish;
        }
        ++completed;
    }
    if (fgetc(input) != EOF || ferror(input) || fflush(output) ||
        !snapshot(&api, context, options, option_count, true) ||
        !colorspace_matches(&api, context, coefficients, colorspace)) {
        fail("final stream/options/colorspace verification failed"); goto finish;
    }
    if (identity_changed_frames) { fail("same-format identity control is not byte-exact"); goto finish; }
    failure[0] = '\0'; exit_status = 0;
finish:
    if (input && fclose(input)) { fail("input close failed"); exit_status = 1; }
    if (output && fclose(output)) { fail("output close failed; partial file retained"); exit_status = 1; }
    fprintf(report, "{\n\"schema\":\"crystalhd.issue529.swscale-conversion.v2\",\n\"status\":\"%s\",\n\"failure\":", exit_status ? "FAIL_RETAIN_PARTIAL_OUTPUT" : "CONVERSION_COMPLETE"); json_string(report, failure);
    fprintf(report, ",\n\"runtime\":"); json_string(report, runtime);
    fprintf(report, ",\n\"mode\":"); json_string(report, mode);
    fprintf(report, ",\n\"input\":"); json_string(report, argv[11]);
    fprintf(report, ",\n\"output\":"); json_string(report, argv[12]);
    fprintf(report, ",\n\"source_siting\":"); json_string(report, argv[8]);
    fprintf(report, ",\n\"destination_siting\":"); json_string(report, argv[9]);
    fprintf(report, ",\n\"geometry\":[352,288],\"frame_order\":\"preserved raw file order; no timestamps or sorting\",\n\"requested_frames\":%u,\"completed_frames\":%u,\"source_active_bytes_per_frame\":%zu,\"output_active_bytes_per_frame\":%zu,\n\"identity_changed_frames\":%u,\"identity_control\":%s,\n\"providers_verified\":%s,\"scale_version_int\":%u,\"util_version_int\":%u,\n\"scale_configuration\":", frames, completed, source_bytes, destination_bytes, identity_changed_frames, conversion ? "false" : "true", providers_verified ? "true" : "false", scale_version, util_version);
    json_string(report, scale_configuration);
    fprintf(report, ",\n\"util_configuration\":"); json_string(report, util_configuration);
    fprintf(report, ",\n\"historical_conversion_flags\":\"UNKNOWN\",\"declared_flags_integer\":%d,\"filter\":", conversion_flags);
    json_string(report, filter_name(conversion_flags));
    fprintf(report, ",\n\"rounding_options\":\"accurate_rnd/bitexact not requested\",\"color_control\":\"BT601 coefficients, limited-to-limited, brightness0,contrast1,saturation1; no RGB\",\"color_verified\":%s,\n\"colorspace_coefficient_vectors\":[", colorspace_verified ? "true" : "false");
    for (int n = 0; n < 8; ++n) fprintf(report, "%s%d", n ? "," : "", colorspace[n]);
    fprintf(report, "],\n\"source_stride\":[%d,%d,%d],\"output_stride\":[%d,%d,%d],\n\"symbols\":[", src_stride[0], src_stride[1], src_stride[2], dst_stride[0], dst_stride[1], dst_stride[2]);
    for (size_t n = 0; n < provider_count; ++n) {
        if (n) fputc(',', report);
        fprintf(report, "{\"symbol\":"); json_string(report, providers[n].symbol);
        fprintf(report, ",\"dladdr_provider_realpath\":"); json_string(report, providers[n].path); fputc('}', report);
    }
    fprintf(report, "],\n\"options\":[");
    for (size_t n = 0; n < option_count; ++n) {
        if (n) fputc(',', report);
        fprintf(report, "{\"name\":"); json_string(report, options[n].name);
        fprintf(report, ",\"requested\":%" PRId64 ",\"after_init\":%" PRId64 ",\"after_scale\":%" PRId64 "}", options[n].requested, options[n].after_init, options[n].after_scale);
    }
    fprintf(report, "]\n}\n");
    bool report_error = ferror(report);
    if (fclose(report) || report_error) exit_status = 1;
    if (context && api.release) api.release(context);
    if (api.freep) { api.freep(&src[0]); api.freep(&dst[0]); }
    free(linear_in); free(linear_out);
    if (scale_handle) dlclose(scale_handle);
    if (util_handle) dlclose(util_handle);
    return exit_status;
}
