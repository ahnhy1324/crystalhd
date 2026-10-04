// SPDX-License-Identifier: LGPL-2.1-or-later
// Optional direct-library drain probe, not a pixel-quality benchmark.
// Use an external timeout as well: a userspace deadline cannot bound a stuck
// kernel ioctl or device close. --preflight never opens the CrystalHD device.
#include <bc_dts_types.h>
#include <libcrystalhd_if.h>
#include "crystalhd_ioctl_limits.h"
#include "flea/70015/magnum/basemodules/chp/70015/rdb/a0/bchp_mfd.h"
#include "../filters/gst/gst-plugin-1.0/gstcrystalhd-input.h"
extern "C" {
#include <libavformat/avformat.h>
}
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <linux/capability.h>
#include <set>
#include <string>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>
#include <vector>
#include "phase1-progress.h"

// Optional probe-only ABI from libcrystalhd_int_if.h. Avoid pulling its
// unrelated register-map dependencies into this direct-library test.
extern "C" BC_STATUS DtsDevRegisterRead(HANDLE handle, uint32_t offset, uint32_t *value);

struct ChromaConfiguration {
    uint32_t lac = 0, sampling = 0;
};

// Two named configuration reads only; no FIFO/status, pointer or write access.
// Separate reads do not certify atomicity, fetch errors or a source-surface lease.
static BC_STATUS ReadChromaConfiguration(HANDLE handle, ChromaConfiguration *config,
    BC_STATUS (*read_register)(HANDLE, uint32_t, uint32_t *) = DtsDevRegisterRead)
{
    const BC_STATUS status = read_register(handle, BCHP_MFD_LAC_CNTL, &config->lac);
    if (status != BC_STS_SUCCESS) return status;
    return read_register(handle, BCHP_MFD_CHROMA_SAMPLING_CNTL, &config->sampling);
}

static bool CanReadChromaConfiguration()
{
    __user_cap_header_struct header = {_LINUX_CAPABILITY_VERSION_3, 0};
    __user_cap_data_struct capabilities[2] = {};
    return syscall(SYS_capget, &header, capabilities) == 0 &&
        (capabilities[CAP_SYS_RAWIO / 32].effective & (1U << (CAP_SYS_RAWIO % 32)));
}

struct ChromaReadFixture {
    unsigned calls = 0;
    bool addresses_valid = true;
    BC_STATUS status[2] = {BC_STS_SUCCESS, BC_STS_SUCCESS};
    uint32_t values[2] = {};
    static BC_STATUS Read(HANDLE handle, uint32_t address, uint32_t *value) {
        auto *fixture = static_cast<ChromaReadFixture *>(handle);
        const uint32_t addresses[] = {BCHP_MFD_LAC_CNTL, BCHP_MFD_CHROMA_SAMPLING_CNTL};
        const unsigned index = fixture->calls++;
        if (index >= 2) {
            fixture->addresses_valid = false;
            return BC_STS_ERROR;
        }
        fixture->addresses_valid &= address == addresses[index];
        *value = fixture->values[index];
        return fixture->status[index];
    }
};

static volatile std::sig_atomic_t interrupted;
static void Interrupt(int) { interrupted = 1; }
static const unsigned kMaximumPackets = 10000;
static const unsigned kMaximumIterations = 1000;
static const uint64_t kTokenStep = 100000;
static const unsigned long kRssGrowthLimitKiB = 32UL * 1024UL;
static const uint64_t kMaximumCaptureBytes = 256ULL * 1024 * 1024;

struct Deadline {
    gint64 end;
    explicit Deadline(unsigned seconds) : end(g_get_monotonic_time() + seconds * G_USEC_PER_SEC) {}
    bool expired() const { return interrupted || g_get_monotonic_time() >= end; }
    static int Check(void *opaque) { return static_cast<Deadline *>(opaque)->expired(); }
};

struct Packet {
    std::vector<uint8_t> data;
    size_t size;
    gsize reservation;
};
struct Input {
    AVCodecID codec = AV_CODEC_ID_NONE;
    BC_MEDIA_SUBTYPE subtype = BC_MSUBTYPE_INVALID;
    unsigned width = 0, height = 0;
    std::vector<uint8_t> metadata;
    std::vector<Packet> packets;
};

static bool Number(const char *text, unsigned maximum, unsigned *value)
{
    if (!text || !*text || *text == '-' || *text == '+') return false;
    char *end = nullptr;
    errno = 0;
    const unsigned long parsed = std::strtoul(text, &end, 10);
    if (errno || !end || *end || parsed == 0 || parsed > maximum) return false;
    *value = static_cast<unsigned>(parsed);
    return true;
}

enum class Mode { SelfTest, Preflight, Hardware };

struct Options {
    Mode mode = Mode::SelfTest;
    const char *path = nullptr;
    unsigned expected = 0;
    unsigned seconds = 30;
    unsigned iterations = 1;
    bool scaler_test = false;
    unsigned scale_width = 0;
    bool mpeg1_via_mpeg2 = false;
    bool h263_via_divx = false;
    bool open_only = false;
    const char *capture_path = nullptr;
    bool observe_chroma = false;
    BC_OUTPUT_FORMAT output_format = OUTPUT_MODE422_YUY2;
};

static uint32_t ProbeDeviceMode(const Options &options)
{
    uint32_t mode = DTS_PLAYBACK_MODE | DTS_LOAD_FILE_PLAY_FW | DTS_SKIP_TX_CHK_CPB |
        DTS_PLAYBACK_DROP_RPT_MODE | DTS_DFLT_RESOLUTION(vdecRESOLUTION_1080p23_976);
    // Native-width controls must avoid the single-thread 1280-pixel preset.
    if (!options.scaler_test || options.scale_width)
        mode |= DTS_SINGLE_THREADED_MODE;
    return mode;
}

static bool ParseArguments(std::vector<const char *> arguments, Options *options)
{
    if (arguments.size() >= 3 &&
        (!std::strcmp(arguments[arguments.size() - 2], "--capture-yuy2") ||
         !std::strcmp(arguments[arguments.size() - 2], "--capture-uyvy"))) {
        const char *path = arguments.back();
        if (!*path || !std::strcmp(path, "-")) return false;
        options->output_format = !std::strcmp(arguments[arguments.size() - 2], "--capture-uyvy")
            ? OUTPUT_MODE422_UYVY : OUTPUT_MODE422_YUY2;
        options->capture_path = path;
        arguments.resize(arguments.size() - 2);
    }
    if (arguments.size() >= 2 && !std::strcmp(arguments.back(), "--observe-chroma")) {
        options->observe_chroma = true;
        arguments.pop_back();
    }
    if (arguments.size() >= 2 && !std::strcmp(arguments.back(), "--open-only")) {
        options->open_only = true;
        arguments.pop_back();
    }
    if (arguments.size() >= 2 && !std::strcmp(arguments.back(), "--mpeg1-via-mpeg2")) {
        options->mpeg1_via_mpeg2 = true;
        arguments.pop_back();
    } else if (arguments.size() >= 2 && !std::strcmp(arguments.back(), "--h263-via-divx")) {
        options->h263_via_divx = true;
        arguments.pop_back();
    }
    if ((options->mpeg1_via_mpeg2 && options->h263_via_divx) ||
        (options->open_only && !options->h263_via_divx)) return false;
    if (arguments.size() >= 4 &&
        !std::strcmp(arguments[arguments.size() - 2], "--scaler-test")) {
        const char *width = arguments.back();
        if (std::strcmp(width, "0")) {
            for (const char *digit = width; *digit; ++digit)
                if (*digit < '0' || *digit > '9') return false;
            if (!Number(width, 1918, &options->scale_width) ||
                options->scale_width < 128 || (options->scale_width & 1))
                return false;
        }
        options->scaler_test = true;
        arguments.resize(arguments.size() - 2);
    }
    if (arguments.size() == 2 && !std::strcmp(arguments[1], "--self-test")) {
        if (options->capture_path || options->observe_chroma || options->scaler_test || options->mpeg1_via_mpeg2 ||
            options->h263_via_divx || options->open_only) return false;
        options->mode = Mode::SelfTest;
        return true;
    }
    if (arguments.size() < 4 || arguments.size() > 6) return false;
    const bool preflight = !std::strcmp(arguments[1], "--preflight");
    const bool hardware = !std::strcmp(arguments[1], "--hardware");
    if ((!preflight && !hardware) || (preflight && arguments.size() > 5) ||
        (options->open_only && (!hardware || options->scaler_test)) ||
        !Number(arguments[3], kMaximumPackets, &options->expected) ||
        (arguments.size() >= 5 && !Number(arguments[4], 300, &options->seconds)) ||
        (arguments.size() == 6 &&
         !Number(arguments[5], kMaximumIterations, &options->iterations)))
        return false;
    options->mode = preflight ? Mode::Preflight : Mode::Hardware;
    options->path = arguments[2];
    if (options->observe_chroma &&
        (!hardware || !options->capture_path || !options->scaler_test || options->scale_width))
        return false;
    return !options->capture_path ||
        (hardware && options->scaler_test && !options->open_only && options->iterations == 1);
}

// Test-only MPEG-1 admission through the existing algorithm-1 channel. Do not
// use BC_MSUBTYPE_MPEG1VIDEO: the format setter has no MPEG-1 algorithm branch.
static BC_MEDIA_SUBTYPE InputSubtype(AVCodecID codec, const char *demuxer,
                                    bool mpeg1_via_mpeg2,
                                    bool h263_via_divx = false, int extradata_size = 0)
{
    if (!demuxer || (mpeg1_via_mpeg2 && h263_via_divx)) return BC_MSUBTYPE_INVALID;
    // Test-only baseline H.263 bytes through the existing algorithm-6 route.
    // No container, H.263+ protocol, or synthesized MPEG-4 metadata is admitted.
    if (h263_via_divx)
        return codec == AV_CODEC_ID_H263 && !std::strcmp(demuxer, "h263") &&
               extradata_size == 0 ? BC_MSUBTYPE_DIVX : BC_MSUBTYPE_INVALID;
    if (mpeg1_via_mpeg2)
        return codec == AV_CODEC_ID_MPEG1VIDEO && !std::strcmp(demuxer, "mpegvideo")
            ? BC_MSUBTYPE_MPEG2VIDEO : BC_MSUBTYPE_INVALID;
    if (codec == AV_CODEC_ID_H264 && !std::strcmp(demuxer, "h264"))
        return BC_MSUBTYPE_H264;
    if (codec == AV_CODEC_ID_MPEG2VIDEO && !std::strcmp(demuxer, "mpegvideo"))
        return BC_MSUBTYPE_MPEG2VIDEO;
    if (codec == AV_CODEC_ID_VC1 && !std::strcmp(demuxer, "vc1"))
        return BC_MSUBTYPE_VC1;
    if (codec == AV_CODEC_ID_WMV3 && !std::strcmp(demuxer, "asf"))
        return BC_MSUBTYPE_WMV3;
    return BC_MSUBTYPE_INVALID;
}

static unsigned H263Bits(const uint8_t *data, unsigned offset, unsigned count)
{
    unsigned value = 0;
    for (unsigned bit = offset; bit < offset + count; ++bit)
        value = (value << 1) | ((data[bit / 8] >> (7 - bit % 8)) & 1);
    return value;
}

// Only the first 50 bits are inspected, after checking the seven-byte prefix.
// This checks the curated baseline picture header, not decoder conformance.
static bool BaselineH263Picture(const uint8_t *data, size_t size,
                                unsigned width, unsigned height)
{
    static const unsigned dimensions[][2] = {
        {128, 96}, {176, 144}, {352, 288}, {704, 576},
    };
    if (!data || size < 7 || H263Bits(data, 0, 22) != 0x20 ||
        H263Bits(data, 30, 1) != 1 || H263Bits(data, 31, 4) != 0 ||
        H263Bits(data, 39, 4) != 0 || H263Bits(data, 43, 5) == 0 ||
        H263Bits(data, 48, 2) != 0)
        return false;
    const unsigned source_format = H263Bits(data, 35, 3);
    return source_format >= 1 && source_format <= 4 &&
           width == dimensions[source_format - 1][0] &&
           height == dimensions[source_format - 1][1];
}

// Conditional full-frame firmware expectation, not a general scaling oracle.
// Width zero selects an unscaled control; reject upscaling and ambiguous edges.
static bool ScalerGeometry(unsigned source_width, unsigned source_height,
                           unsigned scale_width, unsigned *width, unsigned *height)
{
    if (!source_width || source_width > 1920 || !source_height ||
        source_height > 1088 || (source_width & 1) || (source_height & 1) ||
        (scale_width && (scale_width < 128 || scale_width > 1918 ||
                         (scale_width & 1) || scale_width > source_width)))
        return false;
    *width = scale_width ? scale_width : source_width;
    *height = source_height * *width / source_width;
    *height += *height & 1;
    return *height != 0;
}

static const char *PackedName(BC_OUTPUT_FORMAT format)
{
    if (format == OUTPUT_MODE422_YUY2) return "YUY2";
    if (format == OUTPUT_MODE422_UYVY) return "UYVY";
    return nullptr;
}

static bool HashActivePixels(GChecksum *checksum, const BC_DTS_PROC_OUT &output,
                             unsigned width, unsigned height,
                             BC_OUTPUT_FORMAT format = OUTPUT_MODE422_YUY2)
{
    const uint64_t bytes = static_cast<uint64_t>(width) * height * 2;
    if (!checksum || !width || width > 1920 || (width & 1) || !height ||
        height > 1088 || !output.Ybuff || !PackedName(format) || output.b422Mode != format ||
        output.PicInfo.width != width || output.PicInfo.height != height ||
        static_cast<uint64_t>(output.YBuffDoneSz) * 4 < bytes)
        return false;
    // BCM70015 packed 4:2:2 has width*2 stride; do not relabel or convert bytes.
    // Only active rows are hashed, while the successful NoCopy lease is owned.
    g_checksum_update(checksum, output.Ybuff, static_cast<gssize>(bytes));
    return true;
}

static bool CaptureBudget(unsigned width, unsigned height, unsigned expected,
                          uint64_t *frame_bytes, uint64_t *total_bytes)
{
    if (!frame_bytes || !total_bytes || !width || width > 1920 || (width & 1) || !height || height > 1088 ||
        (height & 1) || !expected || expected > kMaximumPackets)
        return false;
    *frame_bytes = static_cast<uint64_t>(width) * height * 2;
    *total_bytes = *frame_bytes * expected;
    return *total_bytes <= kMaximumCaptureBytes;
}

struct CapturedFrame {
    std::vector<uint8_t> pixels;
    BC_OUTPUT_FORMAT output_format = OUTPUT_MODE422_YUY2;
    uint64_t token = 0;
    uint32_t picture_number = 0, width = 0, height = 0, flags = 0;
    uint32_t chroma_format = 0, output_flags = 0, aspect_ratio = 0, colour_primaries = 0;
};

// This copies only a validated host output lease. No pointer survives release.
// The reported format is not an independent hardware byte-order oracle.
static bool CopyCapturedPixels(const BC_DTS_PROC_OUT &output, unsigned width,
                               unsigned height, uint64_t limit, CapturedFrame *frame,
                               BC_OUTPUT_FORMAT format = OUTPUT_MODE422_YUY2)
{
    uint64_t bytes = 0, total = 0;
    if (!frame || !CaptureBudget(width, height, 1, &bytes, &total) || bytes > limit ||
        !output.Ybuff || !PackedName(format) || output.b422Mode != format ||
        !(output.PoutFlags & BC_POUT_FLAGS_PIB_VALID) ||
        (output.PoutFlags & BC_POUT_FLAGS_ENCRYPTED) ||
        (output.PicInfo.flags & (VDEC_FLAG_INTERLACED_SRC | VDEC_FLAG_EOS)) ||
        output.PicInfo.width != width || output.PicInfo.height != height ||
        static_cast<uint64_t>(output.YBuffDoneSz) * 4 < bytes)
        return false;
    try {
        frame->pixels.assign(output.Ybuff, output.Ybuff + static_cast<size_t>(bytes));
    } catch (...) {
        return false;
    }
    frame->token = output.PicInfo.timeStamp;
    frame->output_format = format;
    frame->picture_number = output.PicInfo.picture_number;
    frame->width = width;
    frame->height = height;
    frame->flags = output.PicInfo.flags;
    frame->chroma_format = output.PicInfo.chroma_format;
    frame->output_flags = output.PoutFlags;
    frame->aspect_ratio = output.PicInfo.aspect_ratio;
    frame->colour_primaries = output.PicInfo.colour_primaries;
    return true;
}

struct PixelCapture {
    FILE *file = nullptr;
    const char *path = nullptr;
    unsigned expected = 0, frames = 0;
    uint64_t frame_bytes = 0, total_bytes = 0, written = 0;
    BC_OUTPUT_FORMAT output_format = OUTPUT_MODE422_YUY2;
    bool report;
    explicit PixelCapture(bool report_values = true) : report(report_values) {}
    PixelCapture(const PixelCapture &) = delete;
    PixelCapture &operator=(const PixelCapture &) = delete;
    ~PixelCapture() { if (file) std::fclose(file); }
    bool Open(const char *destination, unsigned width, unsigned height, unsigned count,
              BC_OUTPUT_FORMAT format = OUTPUT_MODE422_YUY2) {
        if (file || path || !PackedName(format)) return false;
        output_format = format;
        if (!destination) return true;
        if (!CaptureBudget(width, height, count, &frame_bytes, &total_bytes)) {
            if (report) std::fprintf(stderr, "%s capture exceeds geometry/frame/256-MiB bounds\n", PackedName(output_format));
            return false;
        }
        const int fd = open(destination, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0) {
            if (report) std::fprintf(stderr, "Cannot exclusively create %s capture '%s': %s\n",
                         PackedName(output_format), destination, std::strerror(errno));
            return false;
        }
        path = destination;
        expected = count;
        file = fdopen(fd, "wb");
        if (!file) {
            close(fd);
            if (report) std::fprintf(stderr, "Failed %s capture retained at '%s' (empty)\n", PackedName(output_format), path);
            return false;
        }
        return true;
    }
    bool Write(const CapturedFrame &frame) {
        if (!file || frames >= expected || frame.output_format != output_format || frame.pixels.size() != frame_bytes ||
            written > total_bytes || frame_bytes > total_bytes - written)
            return false;
        const size_t bytes = std::fwrite(frame.pixels.data(), 1, frame.pixels.size(), file);
        written += bytes;
        if (bytes != frame.pixels.size()) return false;
        if (report) std::printf("Captured packed output: requested-format=%s frame-index=%u token=%llu picture-number=%u "
                    "geometry=%ux%u flags=%x chroma-format=%x output-flags=%x "
                    "aspect-ratio=%u colour-primaries=%u\n", PackedName(output_format), frames,
                    static_cast<unsigned long long>(frame.token), frame.picture_number,
                    frame.width, frame.height, frame.flags, frame.chroma_format,
                    frame.output_flags, frame.aspect_ratio, frame.colour_primaries);
        ++frames;
        return true;
    }
    bool Finish(bool completed) {
        if (!path) return true;
        bool closed = false;
        if (file) {
            closed = std::fclose(file) == 0;
            file = nullptr;
        }
        const bool ok = completed && closed && frames == expected && written == total_bytes;
        if (report) std::printf("Packed capture: requested-format=%s frames=%u/%u bytes=%llu/%llu transport-result=%s\n",
                    PackedName(output_format), frames, expected, static_cast<unsigned long long>(written),
                    static_cast<unsigned long long>(total_bytes), ok ? "PASS" : "FAIL");
        if (!ok && report)
            std::fprintf(stderr, "Failed/partial %s capture retained at '%s'; do not use as evidence\n", PackedName(output_format), path);
        return ok;
    }
};

struct Resources {
    unsigned long rss_kib = 0;
    unsigned fds = 0;
    unsigned threads = 0;
};

static bool StatusValue(const char *line, const char *name, unsigned long *value)
{
    const size_t length = std::strlen(name);
    if (std::strncmp(line, name, length) || line[length] != ':') return false;
    const char *number = line + length + 1;
    while (*number == ' ' || *number == '\t') ++number;
    if (!*number || *number == '-' || *number == '+') return false;
    char *end = nullptr;
    errno = 0;
    const unsigned long parsed = std::strtoul(number, &end, 10);
    if (errno || end == number) return false;
    *value = parsed;
    return true;
}

static bool SampleResources(Resources *resources)
{
    FILE *status = std::fopen("/proc/self/status", "r");
    if (!status) return false;
    bool have_rss = false, have_threads = false;
    char line[256];
    while (std::fgets(line, sizeof(line), status)) {
        unsigned long value = 0;
        if (StatusValue(line, "VmRSS", &value)) {
            resources->rss_kib = value;
            have_rss = true;
        } else if (StatusValue(line, "Threads", &value) && value <= UINT_MAX) {
            resources->threads = static_cast<unsigned>(value);
            have_threads = true;
        }
    }
    const bool status_read_ok = !std::ferror(status);
    const bool status_ok = std::fclose(status) == 0 && status_read_ok;

    DIR *directory = opendir("/proc/self/fd");
    if (!directory) return false;
    const int scan_fd = dirfd(directory);
    unsigned fds = 0;
    errno = 0;
    while (const struct dirent *entry = readdir(directory)) {
        if (entry->d_name[0] == '.') continue;
        char *end = nullptr;
        const long fd = std::strtol(entry->d_name, &end, 10);
        if (!end || *end || fd < 0 || fd == scan_fd) continue;
        ++fds;
    }
    const bool directory_read_ok = errno == 0;
    const bool directory_ok = closedir(directory) == 0 && directory_read_ok;
    resources->fds = fds;
    return status_ok && directory_ok && have_rss && have_threads;
}

static void ReportResources(unsigned iteration, unsigned iterations,
                            const Resources &resources)
{
    std::printf("Resources: iteration=%u/%u rss-kib=%lu fds=%u threads=%u\n",
                iteration, iterations, resources.rss_kib, resources.fds,
                resources.threads);
    std::fflush(stdout);
}

static uint64_t Token(unsigned generation, size_t packet)
{
    // Each generation owns 10,001 timestamp slots. The input cap is 10,000,
    // so an output left behind by an earlier session cannot match this one.
    return (static_cast<uint64_t>(generation) * (kMaximumPackets + 1) +
            packet + 1) * kTokenStep;
}

static bool BoundedRssGrowth(const std::vector<unsigned long> &samples,
                             unsigned long limit, unsigned long *early,
                             unsigned long *late)
{
    if (samples.empty()) return false;
    size_t window = samples.size() / 10;
    if (window < 3) window = 3;
    size_t warmup = samples.size() / 10;
    if (warmup + window > samples.size()) {
        warmup = 0;
        window = samples.size();
    }
    unsigned long long early_sum = 0, late_sum = 0;
    for (size_t index = warmup; index < warmup + window; ++index)
        early_sum += samples[index];
    for (size_t index = samples.size() - window; index < samples.size(); ++index)
        late_sum += samples[index];
    *early = static_cast<unsigned long>(early_sum / window);
    *late = static_cast<unsigned long>(late_sum / window);
    return samples.size() < 6 || *late <= *early + limit;
}

static bool FreshBeforeFlush(bool observed_eos)
{
    return !observed_eos;
}

static bool SelfTest()
{
    bool ok = true;
    unsigned checks = 0;
    const auto check = [&](bool condition, const char *description) {
        ++checks;
        if (!condition) {
            std::fprintf(stderr, "Self-test failed: %s\n", description);
            ok = false;
        }
    };

    unsigned number = 0;
    check(Number("1", 1, &number) && number == 1, "minimum number");
    check(Number("1000", 1000, &number) && number == 1000, "maximum number");
    check(!Number("0", 1000, &number) && !Number("1001", 1000, &number) &&
          !Number("-1", 1000, &number) && !Number("1x", 1000, &number),
          "invalid numbers");

    Options options;
    const uint32_t legacy_mode = ProbeDeviceMode(options);
    check((legacy_mode & DTS_SINGLE_THREADED_MODE) != 0,
          "ordinary probe retains its single-thread mode");
    Options native_mode;
    native_mode.scaler_test = true;
    check(ProbeDeviceMode(native_mode) == (legacy_mode & ~DTS_SINGLE_THREADED_MODE),
          "native-width control disables only the scaling preset mode");
    native_mode.scale_width = 320;
    check(ProbeDeviceMode(native_mode) == legacy_mode,
          "explicit scaled control retains mode and width override");
    check(ParseArguments({"probe", "--hardware", "fixture", "12"}, &options) &&
          options.mode == Mode::Hardware && options.expected == 12 &&
          options.seconds == 30 && options.iterations == 1 &&
          options.output_format == OUTPUT_MODE422_YUY2 && !options.observe_chroma,
          "legacy hardware arguments");
    options = Options{};
    check(ParseArguments({"probe", "--hardware", "fixture", "12", "9", "1000"},
                         &options) && options.seconds == 9 && options.iterations == 1000,
          "churn arguments");
    options = Options{};
    check(!ParseArguments({"probe", "--hardware", "fixture", "12", "9", "1001"},
                          &options) &&
          !ParseArguments({"probe", "--preflight", "fixture", "12", "9", "2"},
                          &options),
          "iteration argument bounds");

    for (const char *width : {"0", "128", "320", "640", "1918"}) {
        options = Options{};
        check(ParseArguments({"probe", "--hardware", "fixture", "12", "9", "2",
                              "--scaler-test", width}, &options) &&
              options.scaler_test && options.iterations == 2,
              "opt-in scaler arguments");
    }
    for (const char *width : {"", "127", "319", "1919", "1920", "1921", "-1",
                             "+128", " 128", "128x", "4294967296"}) {
        options = Options{};
        check(!ParseArguments({"probe", "--hardware", "fixture", "12",
                               "--scaler-test", width}, &options),
              "invalid scaler width rejected before hardware");
    }
    options = Options{};
    check(ParseArguments({"probe", "--preflight", "fixture", "12", "--scaler-test", "320"},
                         &options) && options.mode == Mode::Preflight,
          "device-free scaler preflight");
    options = Options{};
    check(!ParseArguments({"probe", "--self-test", "--scaler-test", "320"}, &options),
          "self-test does not accept hardware options");
    for (const char *capture : {"--capture-yuy2", "--capture-uyvy"}) {
        for (const char *width : {"0", "320", "640"}) {
            options = Options{};
            check(ParseArguments({"probe", "--hardware", "fixture", "12", "9", "1",
                                  "--scaler-test", width, capture, "owned.raw"}, &options) &&
                  options.mode == Mode::Hardware && options.scaler_test &&
                  options.iterations == 1 && options.capture_path &&
                  options.output_format == (!std::strcmp(capture, "--capture-uyvy")
                      ? OUTPUT_MODE422_UYVY : OUTPUT_MODE422_YUY2) &&
                  !std::strcmp(options.capture_path, "owned.raw"),
                  "single-run scaler capture is an explicit trailing option");
        }
    }
    options = Options{};
    check(ParseArguments({"probe", "--hardware", "fixture", "12", "--scaler-test", "320",
                          "--capture-yuy2", "path with spaces.raw"}, &options),
          "capture path is one argument without shell interpretation");
    for (const std::vector<const char *> &invalid : {
             std::vector<const char *>{"probe", "--self-test", "--capture-yuy2", "owned.raw"},
             {"probe", "--preflight", "fixture", "12", "--scaler-test", "320", "--capture-yuy2", "owned.raw"},
             {"probe", "--hardware", "fixture", "12", "--capture-yuy2", "owned.raw"},
             {"probe", "--hardware", "fixture", "12", "9", "2", "--scaler-test", "320", "--capture-yuy2", "owned.raw"},
             {"probe", "--hardware", "fixture", "12", "--h263-via-divx", "--open-only", "--capture-yuy2", "owned.raw"},
             {"probe", "--hardware", "fixture", "12", "--scaler-test", "320", "--h263-via-divx", "--open-only", "--capture-yuy2", "owned.raw"},
             {"probe", "--hardware", "fixture", "12", "--scaler-test", "320", "--capture-yuy2", ""},
             {"probe", "--hardware", "fixture", "12", "--scaler-test", "320", "--capture-yuy2", "-"},
             {"probe", "--hardware", "fixture", "12", "--scaler-test", "320", "--capture-yuy2"},
             {"probe", "--hardware", "fixture", "12", "--capture-yuy2", "owned.raw", "--scaler-test", "320"},
             {"probe", "--hardware", "fixture", "12", "--scaler-test", "320", "--capture-yuy2", "a.raw", "--capture-yuy2", "b.raw"}}) {
        for (const char *capture : {"--capture-yuy2", "--capture-uyvy"}) {
            std::vector<const char *> variant = invalid;
            for (const char *&argument : variant)
                if (!std::strcmp(argument, "--capture-yuy2")) argument = capture;
            options = Options{};
            check(!ParseArguments(variant, &options), "invalid capture mode/count/path/placement rejected");
        }
    }
    options = Options{};
    check(!ParseArguments({"probe", "--hardware", "fixture", "12", "--scaler-test", "320",
                          "--capture-yuy2", "a.raw", "--capture-uyvy", "b.raw"}, &options),
          "mixed capture formats are rejected rather than silently overriding a request");
    for (const char *capture : {"--capture-yuy2", "--capture-uyvy"}) {
        options = Options{};
        check(ParseArguments({"probe", "--hardware", "fixture", "30", "--scaler-test", "0",
                              "--observe-chroma", capture, "owned.raw"}, &options) &&
              options.observe_chroma && options.capture_path && options.scale_width == 0,
              "chroma observation requires an explicit native capture");
        options = Options{};
        check(ParseArguments({"probe", "--hardware", "fixture", "30", "--scaler-test", "0",
                              "--mpeg1-via-mpeg2", "--observe-chroma", capture, "owned.raw"}, &options) &&
              options.observe_chroma && options.mpeg1_via_mpeg2,
              "chroma observation preserves the MPEG-1 research gate");
    }
    for (const std::vector<const char *> &invalid : {
             std::vector<const char *>{"probe", "--self-test", "--observe-chroma"},
             {"probe", "--hardware", "fixture", "30", "--observe-chroma"},
             {"probe", "--hardware", "fixture", "30", "--scaler-test", "0", "--observe-chroma"},
             {"probe", "--preflight", "fixture", "30", "--scaler-test", "0", "--observe-chroma", "--capture-yuy2", "owned.raw"},
             {"probe", "--hardware", "fixture", "30", "--scaler-test", "320", "--observe-chroma", "--capture-yuy2", "owned.raw"},
             {"probe", "--hardware", "fixture", "30", "9", "2", "--scaler-test", "0", "--observe-chroma", "--capture-yuy2", "owned.raw"},
             {"probe", "--hardware", "fixture", "30", "--scaler-test", "0", "--observe-chroma", "--observe-chroma", "--capture-yuy2", "owned.raw"},
             {"probe", "--hardware", "fixture", "30", "--scaler-test", "0", "--observe-chroma", "--mpeg1-via-mpeg2", "--capture-yuy2", "owned.raw"},
             {"probe", "--hardware", "fixture", "30", "--scaler-test", "0", "--capture-yuy2", "owned.raw", "--observe-chroma"}}) {
        options = Options{};
        check(!ParseArguments(invalid, &options), "invalid chroma observation scope or placement");
    }
    for (uint32_t raw : {0U, 0xffffffffU, 0x10U, 0x8U, 1U}) {
        ChromaReadFixture fixture;
        fixture.values[0] = raw;
        fixture.values[1] = ~raw;
        ChromaConfiguration config;
        check(ReadChromaConfiguration(&fixture, &config, ChromaReadFixture::Read) == BC_STS_SUCCESS &&
              fixture.calls == 2 && fixture.addresses_valid && config.lac == raw &&
              config.sampling == ~raw, "two fixed reads preserve every raw bit pattern");
    }
    for (int raw = -1; raw <= BC_STS_PWR_MGMT; ++raw) {
        if (raw == BC_STS_SUCCESS) continue;
        for (unsigned failed_read : {0U, 1U}) {
            ChromaReadFixture fixture;
            fixture.status[failed_read] = static_cast<BC_STATUS>(raw);
            ChromaConfiguration config;
            check(ReadChromaConfiguration(&fixture, &config, ChromaReadFixture::Read) == raw &&
                  fixture.calls == failed_read + 1 && fixture.addresses_valid,
                  "chroma read failure stops without retries or another register");
        }
    }
    options = Options{};
    check(ParseArguments({"probe", "--preflight", "fixture", "30",
                          "--mpeg1-via-mpeg2"}, &options) &&
          options.mode == Mode::Preflight && options.mpeg1_via_mpeg2,
          "device-free MPEG-1 research preflight");
    options = Options{};
    check(ParseArguments({"probe", "--hardware", "fixture", "30", "10", "2",
                          "--scaler-test", "0", "--mpeg1-via-mpeg2"}, &options) &&
          options.mode == Mode::Hardware && options.mpeg1_via_mpeg2 &&
          options.scaler_test && options.scale_width == 0 && options.iterations == 2,
          "explicit MPEG-1 research and pixel-hash arguments");
    for (const std::vector<const char *> &invalid : {
             std::vector<const char *>{"probe", "--self-test", "--mpeg1-via-mpeg2"},
             {"probe", "--hardware", "fixture", "30", "--mpeg1-via-mpeg2", "--mpeg1-via-mpeg2"},
             {"probe", "--hardware", "fixture", "30", "--mpeg1-via-mpeg2", "--scaler-test", "0"}}) {
        options = Options{};
        check(!ParseArguments(invalid, &options), "invalid research option placement");
    }
    check(InputSubtype(AV_CODEC_ID_MPEG1VIDEO, "mpegvideo", true) == BC_MSUBTYPE_MPEG2VIDEO &&
          InputSubtype(AV_CODEC_ID_MPEG1VIDEO, "mpegvideo", false) == BC_MSUBTYPE_INVALID &&
          InputSubtype(AV_CODEC_ID_MPEG1VIDEO, "mpeg", true) == BC_MSUBTYPE_INVALID &&
          InputSubtype(AV_CODEC_ID_MPEG1VIDEO, nullptr, true) == BC_MSUBTYPE_INVALID,
          "MPEG-1 probe is explicit and elementary-stream-only");
    for (const std::pair<AVCodecID, const char *> &native : {
             std::pair<AVCodecID, const char *>{AV_CODEC_ID_H264, "h264"},
             {AV_CODEC_ID_MPEG2VIDEO, "mpegvideo"}, {AV_CODEC_ID_VC1, "vc1"},
             {AV_CODEC_ID_WMV3, "asf"}})
        check(InputSubtype(native.first, native.second, false) != BC_MSUBTYPE_INVALID &&
              InputSubtype(native.first, native.second, true) == BC_MSUBTYPE_INVALID,
              "native admission unchanged and excluded from MPEG-1 probe");
    check(InputSubtype(AV_CODEC_ID_MPEG2VIDEO, "mpegvideo", false) == BC_MSUBTYPE_MPEG2VIDEO &&
          InputSubtype(AV_CODEC_ID_H264, "mpegvideo", false) == BC_MSUBTYPE_INVALID &&
          InputSubtype(AV_CODEC_ID_HEVC, "hevc", true) == BC_MSUBTYPE_INVALID &&
          InputSubtype(AV_CODEC_ID_HEVC, "hevc", false) == BC_MSUBTYPE_INVALID,
          "research option does not admit another decoder protocol");
    for (const std::vector<const char *> &valid : {
             std::vector<const char *>{"probe", "--preflight", "fixture", "30", "--h263-via-divx"},
             {"probe", "--hardware", "fixture", "30", "--h263-via-divx"},
             {"probe", "--hardware", "fixture", "30", "9", "2", "--h263-via-divx"},
             {"probe", "--hardware", "fixture", "30", "--scaler-test", "0", "--h263-via-divx"}}) {
        options = Options{};
        check(ParseArguments(valid, &options) && options.h263_via_divx &&
              !options.mpeg1_via_mpeg2 && !options.open_only,
              "explicit H.263 research parser path");
    }
    for (const std::vector<const char *> &valid : {
             std::vector<const char *>{"probe", "--hardware", "fixture", "30", "--h263-via-divx", "--open-only"},
             {"probe", "--hardware", "fixture", "30", "9", "2", "--h263-via-divx", "--open-only"}}) {
        options = Options{};
        check(ParseArguments(valid, &options) && options.mode == Mode::Hardware &&
              options.h263_via_divx && options.open_only && !options.scaler_test,
              "H.263 OPEN-only hardware parser path");
    }
    for (const std::vector<const char *> &invalid : {
             std::vector<const char *>{"probe", "--self-test", "--h263-via-divx"},
             {"probe", "--self-test", "--open-only"},
             {"probe", "--preflight", "fixture", "30", "--h263-via-divx", "--open-only"},
             {"probe", "--hardware", "fixture", "30", "--open-only"},
             {"probe", "--hardware", "fixture", "30", "--mpeg1-via-mpeg2", "--open-only"},
             {"probe", "--hardware", "fixture", "30", "--h263-via-divx", "--mpeg1-via-mpeg2"},
             {"probe", "--hardware", "fixture", "30", "--mpeg1-via-mpeg2", "--h263-via-divx"},
             {"probe", "--hardware", "fixture", "30", "--h263-via-divx", "--h263-via-divx"},
             {"probe", "--hardware", "fixture", "30", "--open-only", "--h263-via-divx"},
             {"probe", "--hardware", "fixture", "30", "--h263-via-divx", "--open-only", "--open-only"},
             {"probe", "--hardware", "fixture", "30", "--h263-via-divx", "--scaler-test", "0"},
             {"probe", "--hardware", "fixture", "30", "--scaler-test", "0", "--h263-via-divx", "--open-only"},
             {"probe", "--hardware", "fixture", "30", "--scaler-test", "128", "--h263-via-divx", "--open-only"}}) {
        options = Options{};
        check(!ParseArguments(invalid, &options), "H.263 option conflict or misplaced OPEN-only");
    }
    check(InputSubtype(AV_CODEC_ID_H263, "h263", false, true, 0) == BC_MSUBTYPE_DIVX &&
          InputSubtype(AV_CODEC_ID_H263, "h263", false) == BC_MSUBTYPE_INVALID &&
          InputSubtype(AV_CODEC_ID_H263, "h263", true, true) == BC_MSUBTYPE_INVALID,
          "H.263 DIVX admission is explicit and excludes MPEG-1 flag");
    for (const char *demuxer : {"avi", "mov,mp4,m4a,3gp,3g2,mj2", "mpeg", "h263p", ""})
        check(InputSubtype(AV_CODEC_ID_H263, demuxer, false, true) == BC_MSUBTYPE_INVALID,
              "H.263 containers and alternate demuxers rejected");
    check(InputSubtype(AV_CODEC_ID_H263, nullptr, false, true) == BC_MSUBTYPE_INVALID &&
          InputSubtype(AV_CODEC_ID_H263, "h263", false, true, 1) == BC_MSUBTYPE_INVALID &&
          InputSubtype(AV_CODEC_ID_H263, "h263", false, true, -1) == BC_MSUBTYPE_INVALID,
          "H.263 requires raw demuxer and empty extradata");
    for (AVCodecID codec : {AV_CODEC_ID_H263P, AV_CODEC_ID_H263I, AV_CODEC_ID_MPEG4,
                           AV_CODEC_ID_H264, AV_CODEC_ID_MPEG1VIDEO,
                           AV_CODEC_ID_MPEG2VIDEO, AV_CODEC_ID_VC1,
                           AV_CODEC_ID_WMV3, AV_CODEC_ID_HEVC, AV_CODEC_ID_NONE})
        check(InputSubtype(codec, "h263", false, true) == BC_MSUBTYPE_INVALID,
              "H.263 research flag rejects every other tested codec");

    // Synthetic headers for admission tests only; no coded picture is created.
    const auto put_bits = [](std::vector<uint8_t> *data, unsigned offset,
                             unsigned count, unsigned value) {
        for (unsigned bit = 0; bit < count; ++bit) {
            const unsigned position = offset + bit;
            const uint8_t mask = static_cast<uint8_t>(1U << (7 - position % 8));
            (*data)[position / 8] = static_cast<uint8_t>(((*data)[position / 8] & ~mask) |
                (((value >> (count - bit - 1)) & 1U) ? mask : 0));
        }
    };
    const auto h263_header = [&put_bits](unsigned source_format, bool predicted, unsigned quantizer) {
        std::vector<uint8_t> data(7, 0);
        put_bits(&data, 0, 22, 0x20);
        put_bits(&data, 22, 8, 255);
        put_bits(&data, 30, 1, 1);
        put_bits(&data, 35, 3, source_format);
        put_bits(&data, 38, 1, predicted);
        put_bits(&data, 43, 5, quantizer);
        return data;
    };
    static const unsigned h263_dimensions[][2] = {
        {128, 96}, {176, 144}, {352, 288}, {704, 576},
    };
    for (unsigned source_format = 1; source_format <= 4; ++source_format) {
        for (bool predicted : {false, true}) {
            const std::vector<uint8_t> data = h263_header(source_format, predicted, predicted ? 31 : 1);
            check(BaselineH263Picture(data.data(), data.size(),
                                      h263_dimensions[source_format - 1][0],
                                      h263_dimensions[source_format - 1][1]),
                  "standard baseline H.263 I/P picture header");
        }
        const std::vector<uint8_t> data = h263_header(source_format, false, 2);
        check(!BaselineH263Picture(data.data(), data.size(),
                                   h263_dimensions[source_format - 1][0] + 2,
                                   h263_dimensions[source_format - 1][1]) &&
              !BaselineH263Picture(data.data(), data.size(),
                                   h263_dimensions[source_format - 1][0],
                                   h263_dimensions[source_format - 1][1] + 2),
              "H.263 source-format geometry must match demux geometry");
    }
    const std::vector<uint8_t> header = h263_header(2, false, 2);
    check(!BaselineH263Picture(nullptr, 7, 176, 144), "null H.263 header rejected");
    for (size_t size = 0; size < 7; ++size) {
        const std::vector<uint8_t> truncated(header.begin(), header.begin() + size);
        check(!BaselineH263Picture(truncated.data(), truncated.size(), 176, 144),
              "truncated H.263 header rejected before bit access");
    }
    for (unsigned bit : {0U, 21U, 31U, 32U, 33U, 34U, 39U, 40U, 41U, 42U, 48U, 49U}) {
        std::vector<uint8_t> changed = header;
        changed[bit / 8] ^= static_cast<uint8_t>(1U << (7 - bit % 8));
        check(!BaselineH263Picture(changed.data(), changed.size(), 176, 144),
              "wrong PSC, id, or extended H.263 feature bit rejected");
    }
    std::vector<uint8_t> changed = header;
    put_bits(&changed, 30, 1, 0);
    check(!BaselineH263Picture(changed.data(), changed.size(), 176, 144),
          "H.263 marker bit required");
    changed = header;
    put_bits(&changed, 43, 5, 0);
    check(!BaselineH263Picture(changed.data(), changed.size(), 176, 144),
          "H.263 quantizer must be nonzero");
    for (unsigned source_format : {0U, 5U, 6U, 7U}) {
        changed = h263_header(source_format, false, 2);
        check(!BaselineH263Picture(changed.data(), changed.size(), 176, 144),
              "unsupported H.263 source format rejected");
    }
    changed = header;
    changed.insert(changed.end(), {0, 0, 1, 0xb6});
    const std::vector<uint8_t> original = changed;
    check(BaselineH263Picture(changed.data(), changed.size(), 176, 144) && changed == original,
          "H.263 header validation preserves every packet byte");
    unsigned width = 0, height = 0;
    check(ScalerGeometry(640, 360, 0, &width, &height) && width == 640 && height == 360 &&
          ScalerGeometry(640, 360, 640, &width, &height) && width == 640 && height == 360 &&
          ScalerGeometry(640, 360, 320, &width, &height) && width == 320 && height == 180 &&
          ScalerGeometry(640, 362, 320, &width, &height) && height == 182,
          "unscaled identity downscale and odd-height rounding");
    check(!ScalerGeometry(640, 360, 1280, &width, &height) &&
          !ScalerGeometry(0, 360, 320, &width, &height) &&
          !ScalerGeometry(640, 361, 320, &width, &height),
          "ambiguous scaler geometry rejected");
    uint8_t pixels[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    BC_DTS_PROC_OUT picture = {};
    picture.Ybuff = pixels; picture.YBuffDoneSz = 3; picture.b422Mode = TRUE;
    picture.PicInfo.width = 2; picture.PicInfo.height = 2;
    GChecksum *checksum = g_checksum_new(G_CHECKSUM_SHA256);
    GChecksum *reference = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(reference, pixels, 8);
    check(HashActivePixels(checksum, picture, 2, 2) &&
          !std::strcmp(g_checksum_get_string(checksum), g_checksum_get_string(reference)),
          "digest covers active rows but not trailing bytes");
    picture.YBuffDoneSz = 1;
    check(!HashActivePixels(checksum, picture, 2, 2), "short lease rejected before read");
    picture.YBuffDoneSz = 3; picture.b422Mode = FALSE;
    check(!HashActivePixels(checksum, picture, 2, 2), "wrong output format rejected");
    picture.b422Mode = TRUE; picture.Ybuff = nullptr;
    check(!HashActivePixels(checksum, picture, 2, 2), "missing pixel lease rejected");
    g_checksum_free(checksum); g_checksum_free(reference);

    uint64_t frame_bytes = 0, total_bytes = 0;
    check(CaptureBudget(640, 360, 180, &frame_bytes, &total_bytes) &&
          frame_bytes == 460800 && total_bytes == 82944000,
          "capture byte budget follows exact active geometry and expected frames");
    check(CaptureBudget(128, 2, kMaximumPackets, &frame_bytes, &total_bytes) &&
          total_bytes == 5120000,
          "maximum frame count remains bounded");
    for (const std::vector<unsigned> &invalid : {
             std::vector<unsigned>{0, 2, 1}, {2, 0, 1}, {3, 2, 1}, {2, 3, 1},
             {1922, 2, 1}, {2, 1090, 1}, {2, 2, 0}, {2, 2, kMaximumPackets + 1},
             {1920, 1088, 65}, {UINT_MAX, UINT_MAX, UINT_MAX}})
        check(!CaptureBudget(invalid[0], invalid[1], invalid[2], &frame_bytes, &total_bytes),
              "capture rejects malformed geometry, frame overflow and disk budget excess");
    picture = BC_DTS_PROC_OUT{};
    picture.Ybuff = pixels; picture.YBuffDoneSz = 3;
    picture.b422Mode = OUTPUT_MODE422_YUY2;
    picture.PoutFlags = BC_POUT_FLAGS_PIB_VALID;
    picture.PicInfo.width = 2; picture.PicInfo.height = 2;
    picture.PicInfo.timeStamp = 12300000; picture.PicInfo.picture_number = 7;
    picture.PicInfo.chroma_format = 0x422;
    picture.PicInfo.aspect_ratio = 1; picture.PicInfo.colour_primaries = 5;
    CapturedFrame frame;
    check(CopyCapturedPixels(picture, 2, 2, 8, &frame) && frame.pixels.size() == 8 &&
          frame.pixels == std::vector<uint8_t>(pixels, pixels + 8) &&
          frame.token == 12300000 && frame.picture_number == 7 &&
          frame.width == 2 && frame.height == 2 && frame.flags == 0 &&
          frame.chroma_format == 0x422 && frame.output_flags == BC_POUT_FLAGS_PIB_VALID &&
          frame.aspect_ratio == 1 && frame.colour_primaries == 5 &&
          frame.output_format == OUTPUT_MODE422_YUY2,
          "capture owns exactly active pixels and frozen value-only metadata");
    pixels[0] = 99; picture.PicInfo.timeStamp = 0; picture.Ybuff = nullptr;
    check(frame.pixels[0] == 0 && frame.token == 12300000,
          "owned capture survives source mutation, pointer retirement and metadata reuse");
    pixels[0] = 0; picture.Ybuff = pixels;
    check(!CopyCapturedPixels(picture, 2, 2, 7, &frame) &&
          !CopyCapturedPixels(picture, 2, 2, 8, nullptr) &&
          !CopyCapturedPixels(picture, 4, 2, 16, &frame),
          "capture validates destination budget and exact source geometry before copying");
    for (unsigned fault = 0; fault < 7; ++fault) {
        BC_DTS_PROC_OUT invalid = picture;
        if (fault == 0) invalid.Ybuff = nullptr;
        if (fault == 1) invalid.YBuffDoneSz = 1;
        if (fault == 2) invalid.b422Mode = OUTPUT_MODE422_UYVY;
        if (fault == 3) invalid.PoutFlags = 0;
        if (fault == 4) invalid.PoutFlags |= BC_POUT_FLAGS_ENCRYPTED;
        if (fault == 5) invalid.PicInfo.flags |= VDEC_FLAG_INTERLACED_SRC;
        if (fault == 6) invalid.PicInfo.flags |= VDEC_FLAG_EOS;
        check(!CopyCapturedPixels(invalid, 2, 2, 8, &frame),
              "capture rejects missing, short, wrong-format, invalid, encrypted, interlaced or EOS data");
    }
    check(!CaptureBudget(2, 2, 1, nullptr, &total_bytes) &&
          !CaptureBudget(2, 2, 1, &frame_bytes, nullptr),
          "capture budget rejects missing result storage");
    CapturedFrame uyvy;
    picture.b422Mode = OUTPUT_MODE422_UYVY;
    check(CopyCapturedPixels(picture, 2, 2, 8, &uyvy, OUTPUT_MODE422_UYVY) &&
          uyvy.pixels == frame.pixels && uyvy.output_format == OUTPUT_MODE422_UYVY,
          "explicit UYVY capture preserves bytes and records its format without conversion");
    check(!CopyCapturedPixels(picture, 2, 2, 8, &uyvy) &&
          !CopyCapturedPixels(picture, 2, 2, 8, &uyvy, OUTPUT_MODE420),
          "UYVY cannot be captured as YUY2 or planar output");
    checksum = g_checksum_new(G_CHECKSUM_SHA256);
    check(HashActivePixels(checksum, picture, 2, 2, OUTPUT_MODE422_UYVY) &&
          !HashActivePixels(checksum, picture, 2, 2) &&
          !HashActivePixels(checksum, picture, 2, 2, OUTPUT_MODE420),
          "pixel hash requires the exact selected packed format");
    g_checksum_free(checksum);
    check(PackedName(OUTPUT_MODE422_YUY2) && PackedName(OUTPUT_MODE422_UYVY) &&
          !PackedName(OUTPUT_MODE420), "only the two packed output formats are admitted");

    // These tests own this private directory and remove only their exact files.
    char capture_directory[] = "/tmp/crystalhd-yuy2-selftest.XXXXXX";
    char *created_directory = mkdtemp(capture_directory);
    check(created_directory != nullptr, "capture filesystem test directory created");
    if (created_directory) {
        const std::string base = std::string(created_directory) + "/";
        const std::string complete_path = base + "complete.raw";
        const std::string uyvy_path = base + "uyvy.raw";
        const std::string short_path = base + "short.raw";
        const std::string oversized_path = base + "oversized.raw";
        const std::string failed_path = base + "failed.raw";
        const std::string destructor_path = base + "destructor.raw";
        const std::string budget_path = base + "budget.raw";
        const std::string alias_path = base + "alias.raw";
        const std::string dangling_path = base + "dangling.raw";
        const std::string missing_path = base + "missing.raw";
        const auto closed_fd = [](int fd) {
            errno = 0;
            return fd >= 0 && fcntl(fd, F_GETFD) == -1 && errno == EBADF;
        };
        const auto exact_file = [&frame](const std::string &path, unsigned count) {
            FILE *read = std::fopen(path.c_str(), "rb");
            if (!read) return false;
            bool valid = true;
            uint8_t data[8];
            for (unsigned index = 0; index < count; ++index)
                valid = std::fread(data, 1, sizeof(data), read) == sizeof(data) &&
                        !std::memcmp(data, frame.pixels.data(), sizeof(data)) && valid;
            valid = std::fgetc(read) == EOF && !std::ferror(read) && valid;
            return std::fclose(read) == 0 && valid;
        };
        PixelCapture complete(false);
        const bool opened = complete.Open(complete_path.c_str(), 2, 2, 2);
        check(opened && complete.file, "exclusive new capture opened");
        if (opened) {
            const int fd = fileno(complete.file);
            struct stat info{};
            check(fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && !(info.st_mode & 0077) &&
                  (fcntl(fd, F_GETFD) & FD_CLOEXEC), "capture is private regular close-on-exec file");
            check(!complete.Open(budget_path.c_str(), 2, 2, 1),
                  "open capture cannot replace its owned descriptor");
            check(complete.Write(frame) && complete.Write(frame) && !complete.Write(frame),
                  "capture enforces exact frame count without appending an excess frame");
            check(complete.Finish(true) && !complete.file && closed_fd(fd),
                  "successful capture closes descriptor before postflight");
            check(exact_file(complete_path, 2), "raw capture contains only exact active frames");
            PixelCapture existing(false);
            check(!existing.Open(complete_path.c_str(), 2, 2, 1) && !existing.file &&
                  exact_file(complete_path, 2), "existing capture is never truncated or overwritten");
            check(symlink("complete.raw", alias_path.c_str()) == 0,
                  "capture symlink refusal fixture created");
            PixelCapture alias(false);
            check(!alias.Open(alias_path.c_str(), 2, 2, 1) && !alias.file &&
                  exact_file(complete_path, 2), "capture refuses symlink without changing its target");
        }
        PixelCapture packed(false);
        const bool packed_opened = packed.Open(uyvy_path.c_str(), 2, 2, 1, OUTPUT_MODE422_UYVY);
        check(packed_opened && packed.output_format == OUTPUT_MODE422_UYVY,
              "UYVY file is explicitly bound to the selected format");
        if (packed_opened) {
            check(!packed.Write(frame) && packed.frames == 0 && packed.written == 0,
                  "mismatched YUY2 frame writes no bytes to UYVY capture");
            check(packed.Write(uyvy) && packed.Finish(true) && exact_file(uyvy_path, 1),
                  "UYVY capture writes its exact owned bytes and closes successfully");
        }
        PixelCapture planar(false);
        struct stat planar_info{};
        check(!planar.Open(budget_path.c_str(), 2, 2, 1, OUTPUT_MODE420) &&
              lstat(budget_path.c_str(), &planar_info) == -1 && errno == ENOENT,
              "unsupported capture format is refused before creating a file");
        check(symlink("missing.raw", dangling_path.c_str()) == 0,
              "dangling capture symlink refusal fixture created");
        PixelCapture dangling(false);
        struct stat info{};
        check(!dangling.Open(dangling_path.c_str(), 2, 2, 1) && !dangling.file &&
              lstat(missing_path.c_str(), &info) == -1 && errno == ENOENT,
              "capture refuses dangling symlink without creating its target");
        PixelCapture budget(false);
        check(!budget.Open(budget_path.c_str(), 1920, 1088, 65) && !budget.file &&
              lstat(budget_path.c_str(), &info) == -1 && errno == ENOENT,
              "over-budget capture refuses before creating any output");
        PixelCapture short_capture(false);
        const bool short_opened = short_capture.Open(short_path.c_str(), 2, 2, 2);
        check(short_opened, "short-count capture fixture opened");
        if (short_opened) {
            const int fd = fileno(short_capture.file);
            check(short_capture.Write(frame) && !short_capture.Finish(true) &&
                  closed_fd(fd) && exact_file(short_path, 1),
                  "short-count capture fails, closes and retains only its actual frame");
        }
        PixelCapture oversized(false);
        const bool oversized_opened = oversized.Open(oversized_path.c_str(), 2, 2, 1);
        check(oversized_opened, "oversized-frame capture fixture opened");
        if (oversized_opened) {
            const int fd = fileno(oversized.file);
            CapturedFrame extra = frame;
            extra.pixels.push_back(99);
            check(!oversized.Write(extra) && !oversized.Finish(false) && closed_fd(fd) &&
                  exact_file(oversized_path, 0), "oversized frame writes nothing and capture closes failed");
        }
        PixelCapture failed(false);
        const bool failed_opened = failed.Open(failed_path.c_str(), 2, 2, 1);
        check(failed_opened, "failed-run capture fixture opened");
        if (failed_opened) {
            const int fd = fileno(failed.file);
            check(failed.Write(frame) && !failed.Finish(false) && closed_fd(fd) &&
                  exact_file(failed_path, 1), "full capture remains failed when its decoder run failed");
        }
        int destructor_fd = -1;
        {
            PixelCapture abandoned(false);
            const bool abandoned_opened = abandoned.Open(destructor_path.c_str(), 2, 2, 1);
            check(abandoned_opened, "abandoned capture fixture opened");
            if (abandoned_opened) destructor_fd = fileno(abandoned.file);
        }
        check(closed_fd(destructor_fd), "capture destructor closes an unfinished descriptor");
        for (const std::string &path : {complete_path, uyvy_path, short_path, oversized_path, failed_path,
                                       destructor_path, alias_path, dangling_path})
            check(unlink(path.c_str()) == 0 || errno == ENOENT, "owned capture fixture removed");
        check(rmdir(created_directory) == 0, "owned capture filesystem test directory removed");
    }

    check(Token(0, 0) == 100000 && Token(0, 9999) < Token(1, 0) &&
          Token(998, 9999) < Token(999, 0), "generation token ranges");
    unsigned long value = 0;
    check(StatusValue("VmRSS:\t 1234 kB\n", "VmRSS", &value) && value == 1234 &&
          StatusValue("Threads:\t7\n", "Threads", &value) && value == 7 &&
          !StatusValue("VmSize:\t8 kB\n", "VmRSS", &value),
          "status parser");
    Resources resources;
    check(SampleResources(&resources) && resources.rss_kib > 0 &&
          resources.fds > 0 && resources.threads > 0, "live resource sample");

    unsigned long early = 0, late = 0;
    check(BoundedRssGrowth({1000, 1000, 1000, 1000, 1000, 1000},
                           kRssGrowthLimitKiB, &early, &late) && early == 1000 &&
          late == 1000, "stable RSS series");
    check(BoundedRssGrowth({1000, 40000, 42000, 41000, 42000, 41500,
                            42000, 41500, 42000, 42000},
                           kRssGrowthLimitKiB, &early, &late),
          "warmup then plateau RSS series");
    check(!BoundedRssGrowth({1000, 50000, 49000, 90000, 89000, 130000,
                             129000, 170000, 169000, 210000},
                            kRssGrowthLimitKiB, &early, &late),
          "sawtooth growing RSS series");
    check(!BoundedRssGrowth({1000, 41000, 81000, 121000, 161000, 201000},
                            kRssGrowthLimitKiB, &early, &late),
          "monotonic growing RSS series");
    check(FreshBeforeFlush(false) && !FreshBeforeFlush(true),
          "pre-flush EOS must belong to the current drain");

    std::printf("Library drain hardware-free self-test: %u checks %s\n",
                checks, ok ? "passed" : "failed");
    return ok;
}

static bool StartCode(const uint8_t *data, size_t size)
{
    return size >= 4 && data[0] == 0 && data[1] == 0 &&
           (data[2] == 1 || (data[2] == 0 && data[3] == 1));
}

static bool Load(const char *path, unsigned expected, Deadline *deadline, Input *input,
                 bool mpeg1_via_mpeg2, bool h263_via_divx = false)
{
    struct stat file;
    if (stat(path, &file) || !S_ISREG(file.st_mode) || file.st_size <= 0 ||
        file.st_size > 64 * 1024 * 1024) {
        std::fprintf(stderr, "Require a local regular fixture of at most 64 MiB\n");
        return false;
    }
    AVFormatContext *format = avformat_alloc_context();
    if (!format) return false;
    format->interrupt_callback = AVIOInterruptCB{Deadline::Check, deadline};
    if (avformat_open_input(&format, path, nullptr, nullptr) < 0) {
        avformat_close_input(&format);
        return false;
    }
    bool ok = avformat_find_stream_info(format, nullptr) >= 0;
    const int index = ok ? av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0) : -1;
    ok = index >= 0;
    if (ok) {
        const AVCodecParameters *parameters = format->streams[index]->codecpar;
        const char *demuxer = format->iformat->name;
        input->codec = parameters->codec_id;
        input->subtype = InputSubtype(input->codec, demuxer, mpeg1_via_mpeg2,
                                     h263_via_divx, parameters->extradata_size);
        ok = input->subtype != BC_MSUBTYPE_INVALID && parameters->width > 0 &&
             parameters->width <= 1920 && parameters->height > 0 && parameters->height <= 1088 &&
             (parameters->field_order == AV_FIELD_UNKNOWN || parameters->field_order == AV_FIELD_PROGRESSIVE);
        input->width = parameters->width;
        input->height = parameters->height;
        if (ok && input->subtype == BC_MSUBTYPE_WMV3) {
            // Same normalization as gstcrystalhd-codecs.h: four STRUCT_C bytes.
            ok = parameters->extradata &&
                 (parameters->extradata_size == 4 || parameters->extradata_size == 5);
            if (ok) input->metadata.assign(parameters->extradata, parameters->extradata + 4);
        }
    }
    AVPacket *packet = av_packet_alloc();
    ok = ok && packet;
    size_t total = 0;
    int result = 0;
    while (ok && !deadline->expired() && (result = av_read_frame(format, packet)) >= 0) {
        if (packet->stream_index == index) {
            Packet next;
            next.size = packet->size > 0 ? static_cast<size_t>(packet->size) : 0;
            ok = packet->data && !(packet->flags & AV_PKT_FLAG_CORRUPT) &&
                 gst_crystalhd_input_reservation(input->subtype, next.size,
                     input->metadata.size(), &next.reservation) &&
                 (h263_via_divx ? BaselineH263Picture(packet->data, next.size,
                                                     input->width, input->height) :
                  (input->subtype == BC_MSUBTYPE_WMV3 || StartCode(packet->data, next.size))) &&
                 input->packets.size() < expected && total + next.size <= 64 * 1024 * 1024;
            if (ok) {
                // The library may inspect a full startcode even for short ASF input.
                next.data.resize(next.size + AV_INPUT_BUFFER_PADDING_SIZE, 0);
                std::memcpy(next.data.data(), packet->data, next.size);
                total += next.size;
                input->packets.push_back(std::move(next));
            }
        }
        av_packet_unref(packet);
    }
    ok = ok && !deadline->expired() && result == AVERROR_EOF && input->packets.size() == expected;
    av_packet_free(&packet);
    avformat_close_input(&format);
    if (!ok) std::fprintf(stderr, "Unsupported/corrupt framing, deadline, or packet count (got %zu, expected %u)\n",
                          input->packets.size(), expected);
    return ok;
}

struct Device {
    HANDLE handle = nullptr;
    bool opened = false, started = false;
    bool Close() {
        bool ok = true;
        const auto record = [&](const char *operation, BC_STATUS status) {
            if (status != BC_STS_SUCCESS) {
                std::fprintf(stderr, "%s failed: %d\n", operation, status);
                ok = false;
            }
        };
        if (started) record("DtsStopDecoder", DtsStopDecoder(handle));
        started = false;
        if (opened) record("DtsCloseDecoder", DtsCloseDecoder(handle));
        opened = false;
        if (handle) record("DtsDeviceClose", DtsDeviceClose(handle));
        handle = nullptr;
        return ok;
    }
    ~Device() { if (handle) Close(); }
};

static bool Status(const char *operation, BC_STATUS status)
{
    if (status == BC_STS_SUCCESS) return true;
    std::fprintf(stderr, "%s failed: %d\n", operation, status);
    return false;
}

// Named packing-control observation only; never follow device pointers.
static bool PackingState(HANDLE handle, const char *stage)
{
    uint32_t control = 0;
    if (!Status("DtsDevRegisterRead(packing)",
                DtsDevRegisterRead(handle, CRYSTALHD_FLEA_COLOR_REGISTER, &control)))
        return false;
    std::printf("Packing state: stage=%s register=0x%08x raw=0x%08x yuy2-bit=%u\n",
                stage, CRYSTALHD_FLEA_COLOR_REGISTER, control,
                (control & CRYSTALHD_FLEA_COLOR_YUY2) != 0);
    std::fflush(stdout);
    return true;
}

static bool ChromaState(HANDLE handle, const char *stage)
{
    ChromaConfiguration config;
    if (!Status("DtsDevRegisterRead(chroma configuration)", ReadChromaConfiguration(handle, &config)))
        return false;
    std::printf("Chroma state: stage=%s lac-register=0x%08x lac-raw=0x%08x "
                "sampling-register=0x%08x sampling-raw=0x%08x reposition=%u "
                "vert-position=%u interpolation=%u\n",
                stage, BCHP_MFD_LAC_CNTL, config.lac, BCHP_MFD_CHROMA_SAMPLING_CNTL,
                config.sampling,
                (config.sampling & BCHP_MFD_CHROMA_SAMPLING_CNTL_CHROMA_REPOSITION_ENABLE_MASK) >>
                    BCHP_MFD_CHROMA_SAMPLING_CNTL_CHROMA_REPOSITION_ENABLE_SHIFT,
                (config.lac & BCHP_MFD_LAC_CNTL_CHROMA_VERT_POSITION_MASK) >>
                    BCHP_MFD_LAC_CNTL_CHROMA_VERT_POSITION_SHIFT,
                (config.lac & BCHP_MFD_LAC_CNTL_CHROMA_INTERPOLATION_MASK) >>
                    BCHP_MFD_LAC_CNTL_CHROMA_INTERPOLATION_SHIFT);
    std::fflush(stdout);
    return true;
}

struct OutputLease {
    HANDLE handle;
    bool active = true;
    explicit OutputLease(HANDLE value) : handle(value) {}
    OutputLease(const OutputLease &) = delete;
    OutputLease &operator=(const OutputLease &) = delete;
    ~OutputLease() { if (active) Release(); }
    bool Release() {
        if (!active) return false;
        active = false;
        return Status("DtsReleaseOutputBuffs", DtsReleaseOutputBuffs(handle, nullptr, FALSE));
    }
};

struct Audit {
    unsigned frames = 0;
    bool marker = false, eos = false, packing_format_observed = false;
    uint32_t ready = 0;
    std::set<uint64_t> pending;
    unsigned iteration = 0;
    Phase1Progress *progress = nullptr;
    unsigned output_width = 0, output_height = 0;
    BC_OUTPUT_FORMAT output_format = OUTPUT_MODE422_YUY2;
    bool observe_chroma = false;
    GChecksum *pixels = nullptr;
    PixelCapture *capture = nullptr;
    ~Audit() { if (pixels) g_checksum_free(pixels); }
};

static bool Receive(Device *device, const Input &input, Audit *audit)
{
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        BC_DTS_STATUS driver = {};
        if (!Status("DtsGetDriverStatus", DtsGetDriverStatus(device->handle, &driver))) return false;
        audit->ready = driver.ReadyListCount;
        if (!audit->ready) break;
        BC_DTS_PROC_OUT output = {};
        const BC_STATUS result = DtsProcOutputNoCopy(device->handle, 0, &output);
        const bool marker = result == BC_STS_SUCCESS &&
            (output.PicInfo.flags & VDEC_FLAG_EOS) != 0;
        if (result == BC_STS_SUCCESS) {
            OutputLease lease(device->handle);
            CapturedFrame captured;
            audit->marker |= marker;
            bool valid = true;
            if (!marker) {
                const unsigned width = output.PicInfo.width, height = output.PicInfo.height;
                valid = (output.PoutFlags & BC_POUT_FLAGS_PIB_VALID) && output.Ybuff &&
                    !(output.PicInfo.flags & VDEC_FLAG_INTERLACED_SRC) &&
                    width == audit->output_width &&
                    (height == audit->output_height ||
                     (!audit->pixels && input.height == 1080 && height == 1088)) &&
                    static_cast<uint64_t>(output.YBuffDoneSz) * 4 >= static_cast<uint64_t>(width) * height * 2 &&
                    audit->pending.erase(output.PicInfo.timeStamp) == 1;
                if (valid && audit->pixels)
                    valid = HashActivePixels(audit->pixels, output, width, height, audit->output_format);
                if (valid && audit->capture)
                    valid = CopyCapturedPixels(output, audit->output_width, audit->output_height,
                                               audit->capture->frame_bytes, &captured, audit->output_format);
                if (valid) ++audit->frames;
            }
            // Every successful NoCopy fetch owns a lease, even invalid output.
            const bool released = lease.Release();
            // File I/O uses only the owned copy, after the lease was released.
            if (!marker && valid && released && audit->capture &&
                !audit->capture->Write(captured)) {
                std::fprintf(stderr, "%s capture write/budget failure\n", PackedName(audit->output_format));
                valid = false;
            }
            if (!marker && valid && released && audit->capture && audit->frames == 1)
                valid = PackingState(device->handle, "first-output");
            if (!marker && valid && released && audit->observe_chroma && audit->frames == 1)
                valid = ChromaState(device->handle, "first-output-after-release");
            if (!marker && valid && released && audit->progress)
                phase1_progress_write(audit->progress,
                    "probe=library-drain iteration=%u frame-index=%u token=%llu\n",
                    audit->iteration, audit->frames - 1,
                    static_cast<unsigned long long>(output.PicInfo.timeStamp));
            if (!valid) {
                std::fprintf(stderr, "Invalid progressive picture geometry/data/token: %llu\n",
                             static_cast<unsigned long long>(output.PicInfo.timeStamp));
                if (audit->pixels)
                    std::fprintf(stderr, "Scaler picture: got=%ux%u expected=%ux%u "
                                 "flags=%x words=%u packed422=%u\n",
                                 output.PicInfo.width, output.PicInfo.height,
                                 audit->output_width, audit->output_height,
                                 output.PicInfo.flags, output.YBuffDoneSz, output.b422Mode);
            }
            if (!valid || !released) return false;
        } else if (result == BC_STS_FMT_CHANGE && audit->capture &&
                   !audit->packing_format_observed) {
            audit->packing_format_observed = true;
            if (!PackingState(device->handle, "first-format-change")) return false;
            if (audit->observe_chroma && !ChromaState(device->handle, "first-format-change")) return false;
        } else if (result != BC_STS_FMT_CHANGE && result != BC_STS_NO_DATA &&
                   result != BC_STS_BUSY && result != BC_STS_TIMEOUT) {
            return Status("DtsProcOutputNoCopy", result);
        } else if (result != BC_STS_FMT_CHANGE && !marker) {
            break;
        }
    }
    uint8_t eos = 0;
    if (!Status("DtsIsEndOfStream", DtsIsEndOfStream(device->handle, &eos))) return false;
    audit->eos = eos != 0;
    return true;
}

static bool WaitInput(Device *device, const Input &input, Audit *audit,
                      gsize reservation, const Deadline &deadline)
{
    while (!deadline.expired()) {
        if (!Receive(device, input, audit)) return false;
        if (DtsTxFreeSize(device->handle) >= reservation) return true;
        g_usleep(1000);
    }
    std::fprintf(stderr, "Input admission deadline expired\n");
    return false;
}

static bool Run(Input &input, unsigned expected, unsigned seconds,
                unsigned iteration, unsigned iterations,
                Phase1Progress *progress, const Options &options)
{
    Deadline deadline(seconds);
    Device device;
    Audit audit;
    audit.iteration = iteration;
    audit.progress = progress;
    audit.output_width = input.width;
    audit.output_height = input.height;
    audit.output_format = options.output_format;
    audit.observe_chroma = options.observe_chroma;
    if (options.scaler_test) {
        if (!ScalerGeometry(input.width, input.height, options.scale_width,
                            &audit.output_width, &audit.output_height)) return false;
        audit.pixels = g_checksum_new(G_CHECKSUM_SHA256);
        if (!audit.pixels) return false;
    }
    PixelCapture capture;
    if (!capture.Open(options.capture_path, audit.output_width, audit.output_height, expected,
                      options.output_format))
        return false;
    audit.capture = options.capture_path ? &capture : nullptr;
    const uint32_t mode = ProbeDeviceMode(options);
    bool ok = Status("DtsDeviceOpen", DtsDeviceOpen(&device.handle, mode));
    BC_INFO_CRYSTAL version = {};
    if (ok) ok = Status("DtsCrystalHDVersion", DtsCrystalHDVersion(device.handle, &version));
    if (ok && version.device != 1) {
        std::fprintf(stderr, "This hardware probe is restricted to BCM70015\n");
        ok = false;
    }
    BC_INPUT_FORMAT format = {};
    // Match the production GStreamer input format; no AVC1/RCV conversion.
    format.Progressive = TRUE;
    format.OptFlags = 0x80000000U | vdecFrameRate59_94 | 0x40U;
    format.mSubtype = input.subtype;
    format.width = input.width;
    format.height = input.height;
    format.startCodeSz = input.subtype == BC_MSUBTYPE_H264 ? 4 : 0;
    format.pMetaData = input.metadata.empty() ? nullptr : input.metadata.data();
    format.metaDataSz = input.metadata.size();
    if (ok) ok = Status("DtsSetInputFormat", DtsSetInputFormat(device.handle, &format));
    if (ok && options.scale_width) {
        // The format setter may select its historical single-thread width.
        // Override that cache before OPEN; this is not a live channel update.
        BC_SCALING_PARAMS scaling = {};
        scaling.sWidth = options.scale_width;
        ok = Status("DtsSetScaleParams", DtsSetScaleParams(device.handle, &scaling));
    }
    if (ok) device.opened = ok = Status("DtsOpenDecoder", DtsOpenDecoder(device.handle, BC_STREAM_TYPE_ES));
    if (options.open_only) {
        const bool opened = device.opened;
        const bool closed = device.Close();
        ok = ok && opened && closed && !deadline.expired();
        std::printf("H.263 research OPEN-only: iteration=%u/%u opened=%s "
                    "cleanup=%s result=%s\n", iteration, iterations,
                    opened ? "yes" : "no", closed ? "PASS" : "FAIL", ok ? "PASS" : "FAIL");
        std::fflush(stdout);
        return ok;
    }
    if (ok) ok = Status("DtsSetColorSpace", DtsSetColorSpace(device.handle, options.output_format));
    if (ok && options.capture_path) ok = PackingState(device.handle, "selected-before-start");
    if (ok) device.started = ok = Status("DtsStartDecoder", DtsStartDecoder(device.handle));
    if (ok && options.capture_path) ok = PackingState(device.handle, "started");
    if (ok) ok = Status("DtsStartCapture", DtsStartCapture(device.handle));
    if (ok && options.capture_path) ok = PackingState(device.handle, "capture-before-input");
    size_t packet_index = 0;
    for (Packet &packet : input.packets) {
        if (!ok) break;
        const uint64_t token = Token(iteration - 1, packet_index);
        ok = WaitInput(&device, input, &audit, packet.reservation, deadline) &&
             Status("DtsProcInput", DtsProcInput(device.handle, packet.data.data(),
                    packet.size, token, FALSE));
        if (ok) audit.pending.insert(token);
        ++packet_index;
        if (ok && options.capture_path && packet_index == 1)
            ok = PackingState(device.handle, "first-input-accepted");
    }
    if (ok && options.capture_path) ok = PackingState(device.handle, "all-input-accepted");
    if (ok) {
        ok = WaitInput(&device, input, &audit, GST_CRYSTALHD_EOS_RESERVATION,
                       deadline);
        if (ok && !FreshBeforeFlush(audit.eos)) {
            std::fprintf(stderr,
                         "DtsIsEndOfStream was already set before this drain\n");
            ok = false;
        }
        if (ok)
            ok = Status("DtsFlushInput(0)", DtsFlushInput(device.handle, 0));
        if (ok && options.capture_path) ok = PackingState(device.handle, "flush-input-returned");
    }
    // A complete frame count is deliberately NOT the termination condition.
    while (ok && !deadline.expired()) {
        ok = Receive(&device, input, &audit);
        // EOS reaching the decoder is not itself a delivery barrier: a driver
        // status poll can observe it before every accepted picture is fetched.
        if (!ok || (audit.eos && audit.ready == 0 && audit.pending.empty())) break;
        g_usleep(1000);
    }
    // Native paths derive EOS from the firmware timing marker, which need not
    // appear as a successful NoCopy lease. DIVX also has a library idle-fence
    // fallback: its EOS state alone does not prove firmware-marker consumption.
    // In either case require complete frame delivery and token retirement.
    ok = ok && !deadline.expired() && audit.eos && audit.ready == 0 &&
         audit.frames == expected && audit.pending.empty();
    const bool closed = device.Close();
    ok = ok && closed;
    const bool captured = capture.Finish(ok);
    ok = ok && captured;
    std::printf("Library drain: iteration=%u/%u frames=%u/%u pending=%zu "
        "%s-EOS=%s output-marker=%s ready=%u cleanup=%s result=%s\n",
        iteration, iterations, audit.frames, expected, audit.pending.size(),
        options.h263_via_divx ? "library" : "firmware",
        audit.eos ? "yes" : "no", audit.marker ? "yes" : "no", audit.ready,
        closed ? "PASS" : "FAIL", ok ? "PASS" : "FAIL");
    std::fflush(stdout);
    if (audit.pixels) {
        std::printf("Scaler test: iteration=%u/%u requested-width=%u expected-output=%ux%u "
                    "%s-sha256=%s result=%s\n", iteration, iterations,
                    options.scale_width, audit.output_width, audit.output_height,
                    options.output_format == OUTPUT_MODE422_UYVY ? "requested-uyvy" : "yuy2",
                    g_checksum_get_string(audit.pixels), ok ? "PASS" : "FAIL");
        std::fflush(stdout);
    }
    return ok;
}

int main(int argc, char **argv)
{
    const std::vector<const char *> arguments(argv, argv + argc);
    Options options;
    if (!ParseArguments(arguments, &options)) {
        std::fprintf(stderr, "usage: %s --self-test | --preflight LOCAL_VIDEO "
            "EXPECTED_FRAMES [TIMEOUT_SECONDS] | --hardware LOCAL_VIDEO "
            "EXPECTED_FRAMES [TIMEOUT_SECONDS [ITERATIONS]] "
            "[--scaler-test WIDTH_OR_0] [--mpeg1-via-mpeg2 | --h263-via-divx] "
            "[--open-only] [--observe-chroma] "
            "[--capture-yuy2 NEW_PATH | --capture-uyvy NEW_PATH]\n", argv[0]);
        return 2;
    }
    if (options.mode == Mode::SelfTest) return SelfTest() ? 0 : 1;
    if (options.observe_chroma && !CanReadChromaConfiguration()) {
        std::fprintf(stderr, "--observe-chroma requires CAP_SYS_RAWIO; no device was opened\n");
        return 2;
    }
    Phase1Progress progress{};
    if (!phase1_progress_open(&progress)) {
        std::fprintf(stderr, "Could not open Phase 1 progress record\n");
        return 2;
    }
    phase1_progress_write(&progress, "probe=library-drain state=loading-fixture\n");
    std::signal(SIGINT, Interrupt);
    std::signal(SIGTERM, Interrupt);
    Deadline deadline(options.seconds);
    Input input;
    if (!Load(options.path, options.expected, &deadline, &input,
              options.mpeg1_via_mpeg2, options.h263_via_divx)) {
        phase1_progress_close(&progress);
        return 2;
    }
    std::printf("Preflight: %ux%u subtype=%u packets=%zu metadata=%zu\n",
                input.width, input.height, input.subtype, input.packets.size(), input.metadata.size());
    if (options.mpeg1_via_mpeg2)
        std::printf("MPEG-1 research: input-codec=%s configured-algorithm=1 "
                    "route=MPEG2VIDEO selector5-not-used\n",
                    avcodec_get_name(input.codec));
    if (options.h263_via_divx)
        std::printf("H.263 research: input-codec=%s configured-algorithm=6 "
                    "route=DIVX/PES metadata=empty picture-header=baseline "
                    "packet-bytes=unchanged open-only=%s\n",
                    avcodec_get_name(input.codec), options.open_only ? "yes" : "no");
    if (options.scaler_test) {
        unsigned width = 0, height = 0;
        if (!ScalerGeometry(input.width, input.height, options.scale_width, &width, &height)) {
            std::fprintf(stderr, "Require even progressive geometry and no upscaling\n");
            phase1_progress_close(&progress);
            return 2;
        }
        std::printf("Scaler preflight: requested-width=%u expected=%ux%u\n",
                    options.scale_width, width, height);
    }
    if (options.mode == Mode::Preflight) {
        phase1_progress_close(&progress);
        return 0;
    }

    Resources baseline;
    if (!SampleResources(&baseline)) {
        std::fprintf(stderr, "Unable to sample process resources\n");
        phase1_progress_close(&progress);
        return 2;
    }
    ReportResources(0, options.iterations, baseline);
    Resources current = baseline;
    std::vector<unsigned long> rss_samples{baseline.rss_kib};
    unsigned long peak_rss = baseline.rss_kib;
    unsigned completed = 0;
    bool ok = true;
    for (unsigned iteration = 1; iteration <= options.iterations; ++iteration) {
        phase1_progress_write(&progress,
            "probe=library-drain iteration=%u/%u state=starting last-complete=%u\n",
            iteration, options.iterations, completed);
        ok = Run(input, options.expected, options.seconds, iteration,
                 options.iterations, &progress, options);
        if (!SampleResources(&current)) {
            std::fprintf(stderr, "Unable to sample resources after iteration %u\n",
                         iteration);
            ok = false;
        } else {
            rss_samples.push_back(current.rss_kib);
            if (current.rss_kib > peak_rss) peak_rss = current.rss_kib;
            if (iteration == 1 || iteration % 10 == 0 ||
                iteration == options.iterations || !ok)
                ReportResources(iteration, options.iterations, current);
            if (current.fds != baseline.fds || current.threads != baseline.threads) {
                std::fprintf(stderr, "Resource drift after iteration %u: "
                    "fds=%u/%u threads=%u/%u\n", iteration, current.fds,
                    baseline.fds, current.threads, baseline.threads);
                ok = false;
            }
        }
        if (!ok) break;
        completed = iteration;
    }
    unsigned long early_rss = 0, late_rss = 0;
    if (ok && !BoundedRssGrowth(rss_samples, kRssGrowthLimitKiB,
                                &early_rss, &late_rss)) {
        std::fprintf(stderr, "Sustained RSS growth exceeded %lu KiB: "
                     "early-window=%lu late-window=%lu samples=%zu\n",
                     kRssGrowthLimitKiB, early_rss, late_rss,
                     rss_samples.size());
        ok = false;
    }
    const long long rss_delta = static_cast<long long>(current.rss_kib) -
                                static_cast<long long>(baseline.rss_kib);
    std::printf("Library churn: completed=%u/%u rss-kib=%lu/%lu peak=%lu "
        "delta=%+lld fds=%u/%u threads=%u/%u result=%s\n", completed,
        options.iterations, current.rss_kib, baseline.rss_kib, peak_rss,
        rss_delta, current.fds, baseline.fds, current.threads,
        baseline.threads, ok && completed == options.iterations ? "PASS" : "FAIL");
    phase1_progress_close(&progress);
    return ok && completed == options.iterations ? 0 : 1;
}
