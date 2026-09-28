// SPDX-License-Identifier: LGPL-2.1-or-later
// Optional direct-library firmware-EOS probe, not a pixel-quality benchmark.
// Use an external timeout as well: a userspace deadline cannot bound a stuck
// kernel ioctl or device close. --preflight never opens the CrystalHD device.
#include <bc_dts_types.h>
#include <libcrystalhd_if.h>
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
#include <set>
#include <sys/stat.h>
#include <utility>
#include <vector>
#include "phase1-progress.h"

static volatile std::sig_atomic_t interrupted;
static void Interrupt(int) { interrupted = 1; }
static const unsigned kMaximumPackets = 10000;
static const unsigned kMaximumIterations = 1000;
static const uint64_t kTokenStep = 100000;
static const unsigned long kRssGrowthLimitKiB = 32UL * 1024UL;

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
};

static bool ParseArguments(const std::vector<const char *> &arguments, Options *options)
{
    if (arguments.size() == 2 && !std::strcmp(arguments[1], "--self-test")) {
        options->mode = Mode::SelfTest;
        return true;
    }
    if (arguments.size() < 4 || arguments.size() > 6) return false;
    const bool preflight = !std::strcmp(arguments[1], "--preflight");
    const bool hardware = !std::strcmp(arguments[1], "--hardware");
    if ((!preflight && !hardware) || (preflight && arguments.size() > 5) ||
        !Number(arguments[3], kMaximumPackets, &options->expected) ||
        (arguments.size() >= 5 && !Number(arguments[4], 300, &options->seconds)) ||
        (arguments.size() == 6 &&
         !Number(arguments[5], kMaximumIterations, &options->iterations)))
        return false;
    options->mode = preflight ? Mode::Preflight : Mode::Hardware;
    options->path = arguments[2];
    return true;
}

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
    check(ParseArguments({"probe", "--hardware", "fixture", "12"}, &options) &&
          options.mode == Mode::Hardware && options.expected == 12 &&
          options.seconds == 30 && options.iterations == 1,
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

static bool Load(const char *path, unsigned expected, Deadline *deadline, Input *input)
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
        if (parameters->codec_id == AV_CODEC_ID_H264 && !std::strcmp(demuxer, "h264"))
            input->subtype = BC_MSUBTYPE_H264;
        else if (parameters->codec_id == AV_CODEC_ID_MPEG2VIDEO && !std::strcmp(demuxer, "mpegvideo"))
            input->subtype = BC_MSUBTYPE_MPEG2VIDEO;
        else if (parameters->codec_id == AV_CODEC_ID_VC1 && !std::strcmp(demuxer, "vc1"))
            input->subtype = BC_MSUBTYPE_VC1;
        else if (parameters->codec_id == AV_CODEC_ID_WMV3 && !std::strcmp(demuxer, "asf"))
            input->subtype = BC_MSUBTYPE_WMV3;
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
                 (input->subtype == BC_MSUBTYPE_WMV3 || StartCode(packet->data, next.size)) &&
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

struct Audit {
    unsigned frames = 0;
    bool marker = false, eos = false;
    uint32_t ready = 0;
    std::set<uint64_t> pending;
    unsigned iteration = 0;
    Phase1Progress *progress = nullptr;
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
            audit->marker |= marker;
            bool valid = true;
            if (!marker) {
                const unsigned width = output.PicInfo.width, height = output.PicInfo.height;
                valid = (output.PoutFlags & BC_POUT_FLAGS_PIB_VALID) && output.Ybuff &&
                    !(output.PicInfo.flags & VDEC_FLAG_INTERLACED_SRC) && width == input.width &&
                    (height == input.height || (input.height == 1080 && height == 1088)) &&
                    static_cast<uint64_t>(output.YBuffDoneSz) * 4 >= static_cast<uint64_t>(width) * height * 2 &&
                    audit->pending.erase(output.PicInfo.timeStamp) == 1;
                if (valid) ++audit->frames;
            }
            // Every successful NoCopy fetch owns a lease, even invalid output.
            const bool released = Status("DtsReleaseOutputBuffs",
                DtsReleaseOutputBuffs(device->handle, nullptr, FALSE));
            if (!marker && valid && released && audit->progress)
                phase1_progress_write(audit->progress,
                    "probe=library-drain iteration=%u frame-index=%u token=%llu\n",
                    audit->iteration, audit->frames - 1,
                    static_cast<unsigned long long>(output.PicInfo.timeStamp));
            if (!valid) std::fprintf(stderr, "Invalid progressive picture geometry/data/token: %llu\n",
                                    static_cast<unsigned long long>(output.PicInfo.timeStamp));
            if (!valid || !released) return false;
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
                Phase1Progress *progress)
{
    Deadline deadline(seconds);
    Device device;
    Audit audit;
    audit.iteration = iteration;
    audit.progress = progress;
    const uint32_t mode = DTS_PLAYBACK_MODE | DTS_LOAD_FILE_PLAY_FW | DTS_SKIP_TX_CHK_CPB |
        DTS_PLAYBACK_DROP_RPT_MODE | DTS_SINGLE_THREADED_MODE |
        DTS_DFLT_RESOLUTION(vdecRESOLUTION_1080p23_976);
    bool ok = Status("DtsDeviceOpen", DtsDeviceOpen(&device.handle, mode));
    BC_INFO_CRYSTAL version = {};
    if (ok) ok = Status("DtsCrystalHDVersion", DtsCrystalHDVersion(device.handle, &version));
    if (ok && version.device != 1) {
        std::fprintf(stderr, "This firmware-marker probe is restricted to BCM70015\n");
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
    if (ok) device.opened = ok = Status("DtsOpenDecoder", DtsOpenDecoder(device.handle, BC_STREAM_TYPE_ES));
    if (ok) ok = Status("DtsSetColorSpace", DtsSetColorSpace(device.handle, OUTPUT_MODE422_YUY2));
    if (ok) device.started = ok = Status("DtsStartDecoder", DtsStartDecoder(device.handle));
    if (ok) ok = Status("DtsStartCapture", DtsStartCapture(device.handle));
    size_t packet_index = 0;
    for (Packet &packet : input.packets) {
        if (!ok) break;
        const uint64_t token = Token(iteration - 1, packet_index);
        ok = WaitInput(&device, input, &audit, packet.reservation, deadline) &&
             Status("DtsProcInput", DtsProcInput(device.handle, packet.data.data(),
                    packet.size, token, FALSE));
        if (ok) audit.pending.insert(token);
        ++packet_index;
    }
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
    }
    // A complete frame count is deliberately NOT the termination condition.
    while (ok && !deadline.expired()) {
        ok = Receive(&device, input, &audit);
        // EOS reaching the decoder is not itself a delivery barrier: a driver
        // status poll can observe it before every accepted picture is fetched.
        if (!ok || (audit.eos && audit.ready == 0 && audit.pending.empty())) break;
        g_usleep(1000);
    }
    // BCM70015 sets DtsIsEndOfStream only after its RX path consumes the real
    // firmware timing marker.  That marker is not necessarily exposed as a
    // successful NoCopy lease, so require the firmware-derived EOS state and
    // complete token retirement rather than manufacturing a client marker.
    ok = ok && !deadline.expired() && audit.eos && audit.ready == 0 &&
         audit.frames == expected && audit.pending.empty();
    const bool closed = device.Close();
    ok = ok && closed;
    std::printf("Library drain: iteration=%u/%u frames=%u/%u pending=%zu "
        "firmware-EOS=%s output-marker=%s ready=%u cleanup=%s result=%s\n",
        iteration, iterations, audit.frames, expected, audit.pending.size(),
        audit.eos ? "yes" : "no", audit.marker ? "yes" : "no", audit.ready,
        closed ? "PASS" : "FAIL", ok ? "PASS" : "FAIL");
    std::fflush(stdout);
    return ok;
}

int main(int argc, char **argv)
{
    const std::vector<const char *> arguments(argv, argv + argc);
    Options options;
    if (!ParseArguments(arguments, &options)) {
        std::fprintf(stderr, "usage: %s --self-test | --preflight LOCAL_VIDEO "
            "EXPECTED_FRAMES [TIMEOUT_SECONDS] | --hardware LOCAL_VIDEO "
            "EXPECTED_FRAMES [TIMEOUT_SECONDS [ITERATIONS]]\n", argv[0]);
        return 2;
    }
    if (options.mode == Mode::SelfTest) return SelfTest() ? 0 : 1;
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
    if (!Load(options.path, options.expected, &deadline, &input)) {
        phase1_progress_close(&progress);
        return 2;
    }
    std::printf("Preflight: %ux%u subtype=%u packets=%zu metadata=%zu\n",
                input.width, input.height, input.subtype, input.packets.size(), input.metadata.size());
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
                 options.iterations, &progress);
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
