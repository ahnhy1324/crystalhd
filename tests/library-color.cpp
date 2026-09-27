/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Hardware-free color-mode transactions through the actual public API,
 * chip-specific helpers and pooled register ioctl path. No device is opened.
 */
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <pthread.h>
#include "7411d.h"
#include "libcrystalhd_if.h"
#include "libcrystalhd_int_if.h"
#include "libcrystalhd_priv.h"

extern bc_dil_glob_s *bc_dil_glob_ptr;
static unsigned checks, failures, calls, reads, writes;
static DTS_LIB_CONTEXT *observed;
static BC_OUTPUT_FORMAT previous_mode;
static BC_STATUS read_status, write_status;
static unsigned syscall_failure;
static uint32_t register_value, written_value;

static void Check(bool condition, const char *message)
{
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

extern "C" int __wrap_ioctl(int fd, unsigned long code, ...)
{
    if (!observed || fd != 99 ||
        (code != BCM_IOC_REG_RD && code != BCM_IOC_REG_WR &&
         code != BCM_IOC_FPGA_RD && code != BCM_IOC_FPGA_WR)) {
        std::fputs("unexpected ioctl in hardware-free color test\n", stderr);
        std::abort();
    }
    va_list args;
    va_start(args, code);
    BC_IOCTL_DATA *data = va_arg(args, BC_IOCTL_DATA *);
    va_end(args);
    ++calls;
    const bool read = code == BCM_IOC_REG_RD || code == BCM_IOC_FPGA_RD;
    const bool flea = observed->DevId == BC_PCI_DEVID_FLEA;
    Check(code == (flea ? (read ? BCM_IOC_REG_RD : BCM_IOC_REG_WR)
                         : (read ? BCM_IOC_FPGA_RD : BCM_IOC_FPGA_WR)),
          "use the selected chip's register interface");
    Check(data->u.regAcc.Offset == (flea ? 0x00502100U : 0x00000d00U),
          "access only the chip's existing color-control register");
    Check(observed->b422Mode == previous_mode,
          "do not publish requested layout before register write succeeds");
    if (read) {
        ++reads;
        Check(data->u.regAcc.Value == 0, "pooled read request has no stale value");
        data->u.regAcc.Value = register_value;
        data->RetSts = read_status;
    } else {
        ++writes;
        written_value = data->u.regAcc.Value;
        data->RetSts = write_status;
    }
    if (calls == syscall_failure) {
        errno = EIO;
        return -1;
    }
    return 0;
}

struct Fixture {
    bc_dil_glob_s globals = {};
    DTS_LIB_CONTEXT context = {};
    BC_IOCTL_DATA pooled = {};

    explicit Fixture(uint32_t device) {
        bc_dil_glob_ptr = &globals;
        observed = &context;
        context.Sig = LIB_CTX_SIG;
        context.DevHandle = 99;
        context.DevId = device;
        context.b422Mode = OUTPUT_MODE422_UYVY;
        context.pIoDataFreeHd = &pooled;
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&context.thLock, &attr);
        pthread_mutexattr_destroy(&attr);
        Prepare();
    }
    void Prepare() {
        previous_mode = context.b422Mode;
        calls = reads = writes = syscall_failure = 0;
        read_status = write_status = BC_STS_SUCCESS;
        register_value = 0xa5b57c7fU;
        written_value = 0;
    }
    void PoolReturned() {
        Check(context.pIoDataFreeHd == &pooled && pooled.next == nullptr,
              "every success or failure returns the sole ioctl object");
    }
    ~Fixture() {
        PoolReturned();
        pthread_mutex_destroy(&context.thLock);
        observed = nullptr;
        bc_dil_glob_ptr = nullptr;
    }
};

static uint32_t ExpectedRegister(uint32_t device, BC_OUTPUT_FORMAT mode)
{
    if (device == BC_PCI_DEVID_FLEA)
        return (register_value & 0x7cU) |
               (mode == OUTPUT_MODE422_YUY2 ? 2U : 0U);
    return (register_value & ~0x00110000U) |
           (mode == OUTPUT_MODE420 ? 0U : 0x00010000U) |
           (mode == OUTPUT_MODE422_UYVY ? 0x00100000U : 0U);
}

static void SuccessfulModes(uint32_t device)
{
    Fixture fixture(device);
    for (BC_OUTPUT_FORMAT mode : {OUTPUT_MODE422_YUY2, OUTPUT_MODE422_UYVY,
                                  OUTPUT_MODE420, OUTPUT_MODE422_YUY2}) {
        if (device == BC_PCI_DEVID_FLEA && mode == OUTPUT_MODE420) continue;
        fixture.Prepare();
        Check(DtsSetColorSpace(&fixture.context, mode) == BC_STS_SUCCESS,
              "supported color mode succeeds");
        Check(reads == 1 && writes == 1 && calls == 2,
              "successful transaction performs exactly one read and write");
        Check(written_value == ExpectedRegister(device, mode),
              "preserve existing chip-specific register encoding");
        Check(fixture.context.b422Mode == mode,
              "successful register write commits the requested capture layout");
        fixture.PoolReturned();
    }
}

static void RejectedModes(uint32_t device)
{
    Fixture fixture(device);
    for (BC_OUTPUT_FORMAT mode : {OUTPUT_MODE420,
                                  static_cast<BC_OUTPUT_FORMAT>(3),
                                  OUTPUT_MODE_INVALID}) {
        if (device == BC_PCI_DEVID_LINK && mode == OUTPUT_MODE420) continue;
        fixture.Prepare();
        Check(DtsSetColorSpace(&fixture.context, mode) == BC_STS_INV_ARG,
              "unsupported or invalid color mode is rejected");
        Check(calls == 0, "invalid mode never accesses registers");
        Check(fixture.context.b422Mode == previous_mode,
              "rejected mode preserves the prior capture layout");
        fixture.PoolReturned();
        fixture.context.b422Mode = OUTPUT_MODE422_UYVY;
    }
}

static void RegisterFailures(uint32_t device)
{
    for (bool write : {false, true}) {
        for (unsigned failure = 0; failure != 3; ++failure) {
            Fixture fixture(device);
            const BC_STATUS status = failure == 0 ? BC_STS_IO_ERROR : BC_STS_TIMEOUT;
            if (failure == 2) syscall_failure = write ? 2 : 1;
            else if (write) write_status = status;
            else read_status = status;
            const BC_STATUS expected = failure == 2 ? BC_STS_ERROR : status;
            Check(DtsSetColorSpace(&fixture.context, OUTPUT_MODE422_YUY2) == expected,
                  "register RetSts and syscall errors propagate unchanged");
            Check(reads == 1 && writes == (write ? 1U : 0U),
                  "failed register read never causes a write");
            Check(fixture.context.b422Mode == previous_mode,
                  "failed register transaction preserves the prior capture layout");
            fixture.PoolReturned();
            fixture.Prepare();
            Check(DtsSetColorSpace(&fixture.context, OUTPUT_MODE422_YUY2) == BC_STS_SUCCESS &&
                      fixture.context.b422Mode == OUTPUT_MODE422_YUY2 &&
                      written_value == ExpectedRegister(device, OUTPUT_MODE422_YUY2),
                  "a successful retry reuses pooled storage and commits normally");
        }
    }
    Fixture fixture(device);
    fixture.context.pIoDataFreeHd = nullptr;
    Check(DtsSetColorSpace(&fixture.context, OUTPUT_MODE422_YUY2) == BC_STS_INSUFF_RES &&
              fixture.context.b422Mode == previous_mode && calls == 0,
          "ioctl-pool exhaustion preserves layout without register access");
    fixture.context.pIoDataFreeHd = &fixture.pooled;
}

static void InternalCompatibility(uint32_t device)
{
    Fixture fixture(device);
    BC_STATUS (*helper)(HANDLE) = device == BC_PCI_DEVID_FLEA
        ? DtsSetFleaIn422Mode : DtsSetLinkIn422Mode;
    Check(helper(&fixture.context) == BC_STS_SUCCESS &&
              written_value == ExpectedRegister(device, previous_mode),
          "existing internal helper signature still programs the cached mode");
    fixture.Prepare();
    read_status = BC_STS_IO_ERROR;
    Check(helper(&fixture.context) == BC_STS_IO_ERROR && writes == 0,
          "existing helper propagates read failure without a write");
    fixture.Prepare();
    fixture.context.b422Mode = OUTPUT_MODE_INVALID;
    Check(helper(&fixture.context) == BC_STS_INV_ARG && calls == 0,
          "existing helper rejects invalid cached mode before register access");
}

int main()
{
    for (uint32_t device : {BC_PCI_DEVID_FLEA, BC_PCI_DEVID_LINK}) {
        SuccessfulModes(device);
        RejectedModes(device);
        RegisterFailures(device);
        InternalCompatibility(device);
    }
    Fixture fixture(0xffff);
    Check(DtsSetColorSpace(&fixture.context, OUTPUT_MODE422_YUY2) == BC_STS_NOT_IMPL &&
              fixture.context.b422Mode == previous_mode && calls == 0,
          "unknown hardware cannot report successful color programming");
    Check(DtsSetColorSpace(nullptr, OUTPUT_MODE422_YUY2) == BC_STS_INV_ARG && calls == 0,
          "invalid device handle never accesses registers");
    std::printf("Library color: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
