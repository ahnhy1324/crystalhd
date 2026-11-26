// SPDX-License-Identifier: LGPL-2.1-or-later
// Isolated #689 adapter around the maintained drain and output leases.
static int CrystalhdOriginalDrainMain(int, char **) __attribute__((unused));
#define main CrystalhdOriginalDrainMain
#include "../../../tests/library-drain.cpp"
#undef main
#include <set>

extern "C" BC_STATUS __real_DtsProcInput(HANDLE, uint8_t *, uint32_t, uint64_t, BOOL);
extern "C" BC_STATUS __real_DtsSetColorSpace(HANDLE, BC_OUTPUT_FORMAT);
extern "C" BC_STATUS __real_DtsProcOutputNoCopy(HANDLE, uint32_t, BC_DTS_PROC_OUT *);
extern "C" BC_STATUS __real_DtsReleaseOutputBuffs(HANDLE, PVOID, BOOL);

namespace finite_pm {
const unsigned width = 640, height = 360, expected = 180, seconds = 60;
const size_t input_bytes = 1777511;
const char input_sha[] = "32d8ca6ddf8ad529ae3875ee9b7e904630c633bfef6d74ba40d7b1040d20820b";
static bool active, host_only, failed, acquired, pending_pixels, pause_done;
static unsigned inputs, outputs, released_pictures, setter_calls, pause_calls;
static BC_STATUS paused_status = BC_STS_ERROR, resumed_status = BC_STS_ERROR;
static size_t submitted_bytes;
static HANDLE output_owner;
static FILE *events, *input_file;
static Input *admitted;
static PixelCapture pixels(false);
static std::string pixel_path;
static CapturedFrame pending;
static std::set<uint64_t> input_tokens;
static auto input_call = __real_DtsProcInput;
static auto colour_call = __real_DtsSetColorSpace;
static auto output_call = __real_DtsProcOutputNoCopy;
static auto release_call = __real_DtsReleaseOutputBuffs;
static auto pause_call = DtsFWPauseVideo;

static void Event(const char *format, ...)
{
    if (!events) { failed = true; return; }
    va_list args; va_start(args, format);
    if (std::vfprintf(events, format, args) < 0 || std::fputc('\n', events) == EOF ||
        std::fflush(events) != 0) failed = true;
    va_end(args);
}

static FILE *Exclusive(const std::string &path)
{
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return nullptr;
    FILE *file = fdopen(fd, "wb");
    if (!file) close(fd);
    return file;
}

static bool Prepare(const char *directory)
{
    if (!directory || directory[0] != '/' || mkdir(directory, 0700) != 0) return false;
    const std::string path(directory);
    events = Exclusive(path + "/events.tsv");
    input_file = Exclusive(path + "/input.bin");
    pixel_path = path + "/output.yuy2";
    return events && input_file && pixels.Open(pixel_path.c_str(), width, height, expected);
}

static bool CloseFiles()
{
    bool ok = true;
    for (FILE **file : {&input_file, &events}) {
        if (*file) { if (std::fclose(*file)) ok = false; *file = nullptr; }
    }
    return ok;
}

static bool PauseAndResume(HANDLE owner)
{
    if (failed || acquired || !owner || !pause_call || released_pictures != 1 || pause_done) {
        failed = true;
        return false;
    }
    pause_done = true;
    // Preserve e38a2f8 HostPauseAndResume's command semantics: OFF is
    // attempted once even if ON failed after a possibly committed command.
    const gint64 before = g_get_monotonic_time();
    ++pause_calls;
    const BC_STATUS paused = paused_status = pause_call(owner, 1);
    const gint64 paused_at = g_get_monotonic_time();
    Event("PAUSE\toperation=1\tstatus=%u\tbegin_us=%lld\tend_us=%lld\tafter_released_outputs=%u",
          unsigned(paused), static_cast<long long>(before), static_cast<long long>(paused_at), released_pictures);
    const gint64 resume_before = g_get_monotonic_time();
    ++pause_calls;
    const BC_STATUS resumed = resumed_status = pause_call(owner, 0);
    const gint64 resumed_at = g_get_monotonic_time();
    Event("PAUSE\toperation=0\tstatus=%u\tbegin_us=%lld\tend_us=%lld\tafter_released_outputs=%u",
          unsigned(resumed), static_cast<long long>(resume_before), static_cast<long long>(resumed_at), released_pictures);
    failed = failed || paused != BC_STS_SUCCESS || resumed != BC_STS_SUCCESS;
    return !failed;
}
} // namespace finite_pm

extern "C" BC_STATUS __wrap_DtsSetColorSpace(HANDLE owner, BC_OUTPUT_FORMAT format)
{
    using namespace finite_pm;
    if (!active) return colour_call(owner, format);
    if (!owner || ++setter_calls != 1 || format != OUTPUT_MODE422_YUY2) {
        failed = true;
        return BC_STS_INV_ARG;
    }
    // Host mode keeps the DIL's existing YUY2 cache; no target setter call.
    const BC_STATUS status = host_only ? BC_STS_SUCCESS : colour_call(owner, format);
    Event("COLOR\tmode=%s\trequested=%u\tcalled=%u\tstatus=%u", host_only ? "host" : "reference",
          unsigned(format), unsigned(!host_only), unsigned(status));
    return status;
}

extern "C" BC_STATUS __wrap_DtsProcInput(HANDLE owner, uint8_t *data, uint32_t size,
                                          uint64_t token, BOOL encrypted)
{
    using namespace finite_pm;
    if (!active) return input_call(owner, data, size, token, encrypted);
    const unsigned index = inputs;
    if (failed || encrypted || !admitted || !data || !input_file ||
        index >= admitted->packets.size() || token != Token(0, index) ||
        size != admitted->packets[index].size ||
        std::memcmp(data, admitted->packets[index].data.data(), size) ||
        size > input_bytes || submitted_bytes > input_bytes - size ||
        std::fwrite(data, 1, size, input_file) != size || std::fflush(input_file)) {
        failed = true;
        return BC_STS_ERROR;
    }
    ++inputs;
    Event("INPUT_BEGIN\tindex=%u\ttoken=%llu\tbytes=%u\toffset=%zu\tencrypted=0",
          index, static_cast<unsigned long long>(token), size, submitted_bytes);
    submitted_bytes += size;
    const BC_STATUS status = input_call(owner, data, size, token, encrypted);
    if (status == BC_STS_SUCCESS && !input_tokens.insert(token).second) failed = true;
    Event("INPUT_END\tindex=%u\tstatus=%u", index, unsigned(status));
    return status;
}

extern "C" BC_STATUS __wrap_DtsProcOutputNoCopy(HANDLE owner, uint32_t wait, BC_DTS_PROC_OUT *out)
{
    using namespace finite_pm;
    const BC_STATUS status = output_call(owner, wait, out);
    if (!active || status != BC_STS_SUCCESS) return status;
    if (acquired) failed = true;
    acquired = true; output_owner = owner; pending_pixels = false;
    const bool marker = (out->PicInfo.flags & VDEC_FLAG_EOS) != 0;
    if (!marker) {
        // The fixed ordinary Run also requires exactly one pending input
        // token before release. Preserve that condition before triggering PM.
        pending_pixels = outputs < expected && input_tokens.count(out->PicInfo.timeStamp) == 1 &&
            CopyCapturedPixels(*out, width, height,
                                      uint64_t(width)*height*2U, &pending);
        if (pending_pixels) input_tokens.erase(out->PicInfo.timeStamp);
        if (!pending_pixels) { failed = true; interrupted = 1; }
    }
    Event("OUTPUT\tindex=%u\ttoken=%llu\tpicture=%u\tsession=%u\twidth=%u\theight=%u\tformat=%u\ty_done_words=%u\tuv_done_words=%u\tpicture_flags=%u\tpout_flags=%u\teos=%u\tcopy=%u\tbytes=%zu",
          outputs, static_cast<unsigned long long>(out->PicInfo.timeStamp),
          out->PicInfo.picture_number, out->PicInfo.sess_num, out->PicInfo.width,
          out->PicInfo.height, unsigned(out->b422Mode), out->YBuffDoneSz, out->UVBuffDoneSz,
          out->PicInfo.flags, out->PoutFlags, unsigned(marker), unsigned(pending_pixels),
          pending_pixels ? pending.pixels.size() : size_t(0));
    return status; // The original OutputLease remains the sole release owner.
}

extern "C" BC_STATUS __wrap_DtsReleaseOutputBuffs(HANDLE owner, PVOID reserved, BOOL change)
{
    using namespace finite_pm;
    if (active && (!acquired || owner != output_owner)) {
        failed = true;
        Event("RELEASE_REFUSED\towned=%u", unsigned(acquired));
        return BC_STS_ERROR;
    }
    const BC_STATUS status = release_call(owner, reserved, change);
    if (!active) return status;
    const bool picture = pending_pixels;
    acquired = pending_pixels = false; output_owner = nullptr;
    if (picture && status == BC_STS_SUCCESS) ++released_pictures;
    Event("RELEASE\treleased_pictures=%u\tstatus=%u\tpicture=%u",
          released_pictures, unsigned(status), unsigned(picture));
    if (picture && status == BC_STS_SUCCESS && released_pictures == 1 &&
        !PauseAndResume(owner)) interrupted = 1;
    // The original host-only sequence pauses after release, before writing
    // this already owned copy. Preserve that copy even when pause fails.
    const bool wrote = picture && status == BC_STS_SUCCESS && pixels.Write(pending);
    if (wrote) ++outputs;
    if ((picture && !wrote) || status != BC_STS_SUCCESS) failed = true;
    Event("CAPTURE\toutputs=%u\twritten=%u", outputs, unsigned(wrote));
    pending.pixels.clear();
    return status; // A failed native release is consumed once and never retried.
}

#ifndef FINITE_PM_UNIT
int main(int argc, char **argv)
{
    using namespace finite_pm;
    const bool preflight = argc == 3 && !std::strcmp(argv[1], "--preflight");
    const bool hardware = argc == 5 && !std::strcmp(argv[1], "--hardware") &&
        (!std::strcmp(argv[3], "reference") || !std::strcmp(argv[3], "host")) && argv[4][0] == '/';
    if (!preflight && !hardware) {
        std::fprintf(stderr, "usage: %s --preflight INPUT | --hardware INPUT reference|host ABSOLUTE_NEW_DIR\n", argv[0]);
        return 2;
    }
    std::signal(SIGINT, Interrupt); std::signal(SIGTERM, Interrupt);
    Deadline deadline(seconds); Input input;
    size_t bytes = 0; std::string digest;
    if (!Load(argv[2], expected, &deadline, &input, false, false) ||
        input.codec != AV_CODEC_ID_MPEG2VIDEO || input.width != width || input.height != height ||
        input.packets.size() != expected || !BoundedSubmittedPacketDigest(input, input_bytes, &bytes, &digest) ||
        bytes != input_bytes || digest != input_sha || !pause_call) {
        std::fprintf(stderr, "FAIL: pinned MPEG2 640x360/180 and existing pause API required; no device opened\n");
        return 2;
    }
    std::printf("INPUT_ADMISSION bytes=%zu sha256=%s packets=%zu width=%u height=%u native=NOT_RUN\n",
                bytes, digest.c_str(), input.packets.size(), input.width, input.height);
    if (preflight) return 0;
    host_only = !std::strcmp(argv[3], "host"); admitted = &input;
    if (!Prepare(argv[4])) { CloseFiles(); std::fprintf(stderr, "FAIL: output preparation; no device opened\n"); return 2; }
    Options options; options.mode = Mode::Hardware; options.path = argv[2];
    options.expected = expected; options.seconds = seconds; options.iterations = 1;
    // options.capture_path stays null: our owned copy adds no PackingState reads.
    Phase1Progress progress{}; Resources before{}, after{};
    bool ok = SampleResources(&before);
    Event("BEGIN\tmode=%s\tframes=%u\tseconds=%u\tcustom_register_reads=0", argv[3], expected, seconds);
    active = true;
    if (ok) ok = Run(input, expected, seconds, 1, 1, &progress, options, PacedFixture::None, nullptr);
    active = false;
    const bool resources = SampleResources(&after) && SameHandleRetainedResources(before, after);
    const bool capture = pixels.Finish(ok && !failed && !acquired);
    ok = ok && resources && capture && !failed && !acquired && inputs == expected &&
        outputs == expected && input_tokens.empty() && submitted_bytes == input_bytes &&
        pause_done && pause_calls == 2 && setter_calls == 1;
    Event("END\tresult=%s\tinputs=%u\toutputs=%u\tinput_bytes=%zu\tpause_calls=%u\tsetter_calls=%u\tobserver_failed=%u\towned=%u\tfds_before=%u\tfds_after=%u\tthreads_before=%u\tthreads_after=%u",
          ok ? "PASS" : "FAIL", inputs, outputs, submitted_bytes, pause_calls, setter_calls,
          unsigned(failed), unsigned(acquired), before.fds, after.fds, before.threads, after.threads);
    const bool saved = CloseFiles();
    const bool complete = ok && saved && !failed;
    std::printf("PM_EXERCISE result=%s frames=%u inputs=%u pause=%u resume=%u mode=%s\n",
                complete ? "PASS" : "FAIL", outputs, inputs,
                unsigned(paused_status), unsigned(resumed_status), argv[3]);
    return complete ? 0 : 1;
}
#endif
