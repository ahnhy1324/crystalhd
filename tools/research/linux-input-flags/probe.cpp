// SPDX-License-Identifier: LGPL-2.1-or-later
// Isolated #690 clear-input observation. No product source is changed.
static int CrystalhdOriginalDrainMain(int, char **) __attribute__((unused));
#define main CrystalhdOriginalDrainMain
#include "../../../tests/library-drain.cpp"
#undef main
#include "7411d.h"
#include "bc_dts_glob_lnx.h"
#include <atomic>
#include <mutex>

extern "C" BC_STATUS __real_DtsProcInput(HANDLE, uint8_t *, uint32_t, uint64_t, BOOL);
extern "C" BC_STATUS __real_DtsProcOutputNoCopy(HANDLE, uint32_t, BC_DTS_PROC_OUT *);
extern "C" BC_STATUS __real_DtsReleaseOutputBuffs(HANDLE, PVOID, BOOL);

namespace issue690 {
const unsigned width = 640, height = 360, expected = 180, seconds = 60;
const size_t input_bytes = 787883, capture_limit = 32U * 1024U * 1024U;
const char input_sha[] = "767d0fc307052fe995ca0921aaf01b48b4e6477698188e1e7a4e42da690f304e";
static bool active;
static unsigned flag, inputs, outputs, tx_calls, fw_calls, io_calls;
static size_t public_bytes, tx_bytes;
static std::atomic<bool> failed{false};
static std::mutex lock;
static FILE *events, *public_file, *tx_file;
static PixelCapture pixels(false);
static std::string pixel_path;
static CapturedFrame pending;
static bool acquired, pending_pixels;
static HANDLE output_owner;
static Input *admitted;
static auto input_call = __real_DtsProcInput;
static auto output_call = __real_DtsProcOutputNoCopy;
static auto release_call = __real_DtsReleaseOutputBuffs;
static int KernelIoctl(int fd, unsigned long request, void *argument)
{ return static_cast<int>(syscall(SYS_ioctl, fd, request, argument)); }
static auto ioctl_call = KernelIoctl;

static void Event(const char *format, ...)
{
    if (!events) { failed = true; return; }
    va_list args; va_start(args, format);
    if (std::vfprintf(events, format, args) < 0 || std::fputc('\n', events) == EOF ||
        std::fflush(events) != 0) failed = true;
    va_end(args);
}

static bool Save(FILE *file, const uint8_t *data, size_t count, size_t *total)
{
    if (!file || !data || count > capture_limit || *total > capture_limit - count)
        return false;
    const size_t written = std::fwrite(data, 1, count, file);
    *total += written;
    return written == count && std::fflush(file) == 0;
}

static FILE *Exclusive(const std::string &path)
{
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
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
    public_file = Exclusive(path + "/public-input.bin");
    tx_file = Exclusive(path + "/tx-input.bin");
    pixel_path = path + "/output.yuy2";
    return events && public_file && tx_file && pixels.Open(pixel_path.c_str(), width, height, expected);
}

static bool CloseFiles()
{
    bool ok = true;
    for (FILE **file : {&public_file, &tx_file, &events}) {
        if (*file) { if (std::fclose(*file) != 0) ok = false; *file = nullptr; }
    }
    return ok;
}
} // namespace issue690

extern "C" BC_STATUS __wrap_DtsProcInput(HANDLE h, uint8_t *data, uint32_t size,
                                          uint64_t token, BOOL original_flag)
{
    using namespace issue690;
    if (!active) return input_call(h, data, size, token, original_flag);
    unsigned index;
    {
        std::lock_guard<std::mutex> guard(lock);
        index = inputs;
        if (failed || original_flag || !admitted || index >= admitted->packets.size() ||
            size != admitted->packets[index].size || token != Token(0, index) ||
            std::memcmp(data, admitted->packets[index].data.data(), size) ||
            !Save(public_file, data, size, &public_bytes)) {
            failed = true; return BC_STS_ERROR;
        }
        ++inputs;
        Event("INPUT_BEGIN\tindex=%u\ttoken=%llu\tbytes=%u\toffset=%zu\toriginal_flag=%u\tpublic_flag=%u",
              index, static_cast<unsigned long long>(token), size, public_bytes-size,
              unsigned(original_flag), flag);
    }
    const BC_STATUS status = input_call(h, data, size, token, BOOL(flag));
    {
        std::lock_guard<std::mutex> guard(lock);
        Event("INPUT_END\tindex=%u\tstatus=%u", index, unsigned(status));
    }
    return status;
}

// The executable's exported ioctl symbol observes the actual DIL syscall
// envelope. It neither rewrites the envelope nor claims DMA visibility.
extern "C" int ioctl(int fd, unsigned long request, ...) noexcept
{
    va_list args; va_start(args, request); void *argument = va_arg(args, void *); va_end(args);
    using namespace issue690;
    if (!active) return ioctl_call(fd, request, argument);
    const bool ours = _IOC_TYPE(request) == BC_IOC_BASE &&
        _IOC_SIZE(request) == sizeof(BC_IOCTL_DATA);
    BC_IOCTL_DATA *io = ours ? static_cast<BC_IOCTL_DATA *>(argument) : nullptr;
    const bool tx = ours && request == BCM_IOC_PROC_INPUT;
    const bool fw = ours && request == BCM_IOC_FW_CMD;
    unsigned id;
    {
        std::lock_guard<std::mutex> guard(lock);
        id = ++io_calls;
        if (id > 200000U || ((tx || fw) && (!io || io->IoctlDataSz != sizeof(*io)))) {
            failed = true; errno = EINVAL; return -1;
        }
        if (tx) {
            const auto &p = io->u.ProcInput;
            // Stage A's expectation is also an admission guard. Never send
            // an unexpected direct flag1 or mapped-buffer experiment.
            if (failed || p.Encrypted != 0 || p.Mapped || !p.BuffSz ||
                ++tx_calls > 4096U || !Save(tx_file, p.pDmaBuff, p.BuffSz, &tx_bytes)) {
                Event("TX_REFUSED\tid=%u\tflag=%u\tmapped=%u\tbytes=%u", id,
                      unsigned(p.Encrypted), unsigned(p.Mapped), p.BuffSz);
                failed = true; io->RetSts = BC_STS_ERROR; errno = EINVAL; return -1;
            }
            Event("TX_BEGIN\tid=%u\tfd=%d\tbytes=%u\toffset=%zu\tflag=%u\tmapped=%u",
                  id, fd, p.BuffSz, tx_bytes-p.BuffSz, unsigned(p.Encrypted), unsigned(p.Mapped));
        } else if (fw) {
            ++fw_calls;
            Event("FW_BEGIN\tid=%u\tcommand=%u\tsequence=%u\tflags=%u", id,
                  io->u.fwCmd.cmd[0], io->u.fwCmd.cmd[1], io->u.fwCmd.flags);
        }
    }
    const int result = ioctl_call(fd, request, argument);
    const int saved_errno = errno;
    {
        std::lock_guard<std::mutex> guard(lock);
        if (tx) Event("TX_END\tid=%u\tsyscall=%d\terrno=%d\tstatus=%u\tdram_offset=%u", id,
                      result, result < 0 ? saved_errno : 0, unsigned(io->RetSts), io->u.ProcInput.DramOffset);
        if (fw) Event("FW_END\tid=%u\tsyscall=%d\terrno=%d\tstatus=%u\trsp_command=%u\trsp_sequence=%u\trsp_status=%u", id,
                      result, result < 0 ? saved_errno : 0, unsigned(io->RetSts),
                      io->u.fwCmd.rsp[0], io->u.fwCmd.rsp[1], io->u.fwCmd.rsp[2]);
    }
    errno = saved_errno;
    return result;
}

extern "C" BC_STATUS __wrap_DtsProcOutputNoCopy(HANDLE h, uint32_t wait, BC_DTS_PROC_OUT *out)
{
    using namespace issue690;
    const BC_STATUS status = output_call(h, wait, out);
    if (!active || status != BC_STS_SUCCESS) return status;
    std::lock_guard<std::mutex> guard(lock);
    if (acquired) failed = true;
    acquired = true; output_owner = h;
    pending_pixels = false;
    const bool marker = (out->PicInfo.flags & VDEC_FLAG_EOS) != 0;
    if (!marker) {
        pending_pixels = outputs < expected && CopyCapturedPixels(*out, width, height,
                                                    uint64_t(width)*height*2U, &pending);
        if (!pending_pixels) { failed = true; interrupted = 1; }
    }
    Event("OUTPUT\tindex=%u\ttoken=%llu\twidth=%u\theight=%u\tformat=%u\tpitch=%u\tbytes=%zu\ty_done_words=%u\tuv_done_words=%u\tpicture=%u\tsession=%u\tpicture_flags=%u\tpout_flags=%u\teos=%u\tcopy=%u",
          outputs, static_cast<unsigned long long>(out->PicInfo.timeStamp),
          out->PicInfo.width, out->PicInfo.height, unsigned(out->b422Mode),
          pending_pixels ? width*2U : 0U, pending_pixels ? pending.pixels.size() : size_t(0),
          out->YBuffDoneSz, out->UVBuffDoneSz, out->PicInfo.picture_number,
          out->PicInfo.sess_num, out->PicInfo.flags, out->PoutFlags, unsigned(marker), unsigned(pending_pixels));
    return status; // Every successful fetch still belongs to the original drain's lease.
}

extern "C" BC_STATUS __wrap_DtsReleaseOutputBuffs(HANDLE h, PVOID reserved, BOOL change)
{
    using namespace issue690;
    if (active) {
        std::lock_guard<std::mutex> guard(lock);
        if (!acquired || h != output_owner) {
            failed = true;
            Event("RELEASE_REFUSED\towned=%u", unsigned(acquired));
            return BC_STS_ERROR;
        }
    }
    const BC_STATUS status = release_call(h, reserved, change);
    if (!active) return status;
    std::lock_guard<std::mutex> guard(lock);
    const bool owner = acquired && h == output_owner;
    bool wrote = false;
    if (!owner) failed = true;
    if (owner && pending_pixels && status == BC_STS_SUCCESS) {
        wrote = pixels.Write(pending);
        if (wrote) ++outputs; else failed = true;
    }
    Event("RELEASE\tindex=%u\tstatus=%u\towned=%u\twritten=%u", outputs,
          unsigned(status), unsigned(owner), unsigned(wrote));
    if (status != BC_STS_SUCCESS) failed = true;
    acquired = pending_pixels = false; pending.pixels.clear(); output_owner = nullptr;
    return status;
}

#ifndef ISSUE690_UNIT
int main(int argc, char **argv)
{
    using namespace issue690;
    const bool preflight = argc == 3 && !std::strcmp(argv[1], "--preflight");
    const bool hardware = argc == 5 && !std::strcmp(argv[1], "--hardware") &&
        (!std::strcmp(argv[3], "0") || !std::strcmp(argv[3], "1")) && argv[4][0] == '/';
    if (!preflight && !hardware) {
        std::fprintf(stderr, "usage: %s --preflight INPUT | --hardware INPUT 0|1 ABSOLUTE_NEW_DIR\n", argv[0]);
        return 2;
    }
    std::signal(SIGINT, Interrupt); std::signal(SIGTERM, Interrupt);
    Deadline deadline(seconds); Input input;
    size_t bytes = 0; std::string digest;
    if (!Load(argv[2], expected, &deadline, &input, false, false) || input.codec != AV_CODEC_ID_H264 ||
        input.width != width || input.height != height || input.packets.size() != expected ||
        !BoundedSubmittedPacketDigest(input, input_bytes, &bytes, &digest) ||
        bytes != input_bytes || digest != input_sha) {
        std::fprintf(stderr, "FAIL: requires pinned high-annexb.h264 640x360/180 clear input; no device opened\n");
        return 2;
    }
    std::printf("INPUT_ADMISSION bytes=%zu sha256=%s packets=%zu width=%u height=%u native=NOT_RUN\n",
                bytes, digest.c_str(), input.packets.size(), input.width, input.height);
    if (preflight) return 0;
    flag = unsigned(argv[3][0]-'0'); admitted = &input;
    if (!Prepare(argv[4])) { CloseFiles(); std::fprintf(stderr, "FAIL: exclusive output preparation; no device opened\n"); return 2; }
    Options options; options.mode = Mode::Hardware; options.path = argv[2];
    options.expected = expected; options.seconds = seconds; options.iterations = 1;
    Phase1Progress progress{}; Resources before{}, after{};
    bool ok = SampleResources(&before);
    Event("BEGIN\tpublic_flag=%u\tframes=%u\tseconds=%u\twidth=%u\theight=%u\tio_boundary=userspace_kernel\tphysical_dma_observed=0", flag, expected, seconds, width, height);
    active = true;
    if (ok) ok = Run(input, expected, seconds, 1, 1, &progress, options, PacedFixture::None, nullptr);
    active = false;
    const bool resources = SampleResources(&after) && SameHandleRetainedResources(before, after);
    const bool capture = pixels.Finish(ok && !failed && !acquired);
    ok = ok && resources && capture && !failed && !acquired && inputs == expected &&
        outputs == expected && public_bytes == input_bytes && tx_calls && fw_calls;
    Event("END\tresult=%s\tinputs=%u\toutputs=%u\tpublic_bytes=%zu\ttx_calls=%u\ttx_bytes=%zu\tfw_calls=%u\tobserver_failed=%u\towned=%u\tfds_before=%u\tfds_after=%u\tthreads_before=%u\tthreads_after=%u\tdecryption_evidence=NOT_TESTED",
          ok ? "PASS" : "FAIL", inputs, outputs, public_bytes, tx_calls, tx_bytes,
          fw_calls, unsigned(bool(failed)), unsigned(acquired), before.fds, after.fds, before.threads, after.threads);
    const bool saved = CloseFiles();
    const bool complete = ok && saved && !failed;
    std::printf("ISSUE690 result=%s public_flag=%u inputs=%u outputs=%u ioctl_tx_flag=0 decryption=NOT_TESTED\n",
                complete ? "PASS" : "FAIL", flag, inputs, outputs);
    return complete ? 0 : 1;
}
#endif
