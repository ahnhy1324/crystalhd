// SPDX-License-Identifier: LGPL-2.1-or-later
// Actual DtsTxDmaText -> pooled ioctl -> DtsDrvCmd, with an abort-only ioctl
// boundary. Flag transport is not a protected-session or decryption test.
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <unistd.h>
#include "7411d.h"
#include "libcrystalhd_int_if.h"
#include "libcrystalhd_priv.h"

extern bc_dil_glob_s *bc_dil_glob_ptr;
static unsigned checks, failures, ioctl_calls;
static DTS_LIB_CONTEXT *active;
static BC_IOCTL_DATA *expected_envelope;
static uint8_t expected_flags;
static const uint8_t plain_bytes[] = {0, 0, 1, 0x65, 0x42, 0xa5, 0x19, 0x7c};
static uint8_t bytes[sizeof(plain_bytes)];
static BC_STATUS returned_status;
static bool syscall_error;
static const uint32_t returned_offset = 0x123400;

static void Check(bool condition, const char *message)
{
    ++checks;
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}

extern "C" int __wrap_ioctl(int fd, unsigned long command, ...)
{
    if (!active || fd != 99 || command != BCM_IOC_PROC_INPUT) std::abort();
    va_list arguments;
    va_start(arguments, command);
    BC_IOCTL_DATA *io = va_arg(arguments, BC_IOCTL_DATA *);
    va_end(arguments);
    ++ioctl_calls;
    Check(io == expected_envelope && active->pIoDataFreeHd == nullptr,
          "the actual command borrows exactly one pooled envelope");
    Check(io->IoctlDataSz == sizeof(*io) && io->u.ProcInput.pDmaBuff == bytes &&
          io->u.ProcInput.BuffSz == sizeof(bytes) && !io->u.ProcInput.Mapped &&
          io->u.ProcInput.DramOffset == 0 && io->u.ProcInput.Encrypted == expected_flags &&
          !std::memcmp(bytes, plain_bytes, sizeof(bytes)),
          "actual direct input preserves bytes, native ABI and the complete flag byte");
    io->u.ProcInput.DramOffset = returned_offset;
    io->RetSts = returned_status;
    if (syscall_error) { errno = EIO; return -1; }
    return 0;
}

struct Fixture {
    bc_dil_glob_s globals = {};
    DTS_LIB_CONTEXT context = {};
    BC_IOCTL_DATA pooled = {};
    Fixture()
    {
        active = &context; expected_envelope = &pooled; bc_dil_glob_ptr = &globals;
        context.Sig = LIB_CTX_SIG; context.DevHandle = 99;
        context.DevId = BC_PCI_DEVID_FLEA; context.ProcessID = getpid();
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&context.thLock, &attr);
        pthread_mutexattr_destroy(&attr);
        Reset();
    }
    void Reset()
    {
        std::memcpy(bytes, plain_bytes, sizeof(bytes));
        std::memset(&pooled, 0xa5, sizeof(pooled)); pooled.next = nullptr;
        context.pIoDataFreeHd = &pooled;
        ioctl_calls = 0; returned_status = BC_STS_SUCCESS; syscall_error = false;
    }
    void Balanced()
    {
        Check(context.pIoDataFreeHd == &pooled && pooled.next == nullptr,
              "success or failure returns exactly the sole command envelope without a cycle");
    }
    ~Fixture()
    {
        Balanced(); pthread_mutex_destroy(&context.thLock);
        active = nullptr; expected_envelope = nullptr; bc_dil_glob_ptr = nullptr;
    }
};

static void FlagsAndReturns()
{
    Fixture f;
    const uint8_t flags[] = {0, 1, 2, 3, 0x80, 0xff};
    const BC_STATUS statuses[] = {BC_STS_SUCCESS, BC_STS_BUSY, BC_STS_IO_USER_ABORT,
                                  BC_STS_INV_ARG, BC_STS_IO_ERROR, BC_STS_CMD_CANCELLED};
    for (uint32_t algorithm : {uint32_t(BC_VID_ALGO_H264), uint32_t(BC_VID_ALGO_VC1MP)}) {
        for (uint8_t flag : flags) {
            for (BC_STATUS status : statuses) {
                f.Reset(); f.context.VidParams.VideoAlgo = algorithm; returned_status = status;
                expected_flags = flag | (algorithm == BC_VID_ALGO_VC1MP ? 2U : 0U);
                uint32_t offset = 0xdeadbeef;
                Check(DtsTxDmaText(&f.context, bytes, sizeof(bytes), &offset, flag) == status &&
                      ioctl_calls == 1 && offset == returned_offset &&
                      !std::memcmp(bytes, plain_bytes, sizeof(bytes)),
                      "direct input retains exact driver return/offset behavior and VC1-only bit0x02 addition");
                f.Balanced();
            }
        }
    }
    f.Reset(); syscall_error = true; expected_flags = 1;
    f.context.VidParams.VideoAlgo = BC_VID_ALGO_H264;
    uint32_t offset = 0xdeadbeef;
    Check(DtsTxDmaText(&f.context, bytes, sizeof(bytes), &offset, 1) == BC_STS_ERROR &&
          ioctl_calls == 1 && offset == returned_offset,
          "syscall failure keeps the legacy status/offset contract and retires its envelope");
    f.Balanced();
}

static void InvalidArgumentsAndPoolExhaustion()
{
    Fixture f;
    for (uint8_t flag : {uint8_t(0), uint8_t(1), uint8_t(0xff)}) {
        for (unsigned invalid = 0; invalid != 4; ++invalid) {
            f.Reset(); uint32_t offset = 0xdeadbeef;
            Check(DtsTxDmaText(invalid == 0 ? nullptr : &f.context,
                    invalid == 1 ? nullptr : bytes, invalid == 2 ? 0 : sizeof(bytes),
                    invalid == 3 ? nullptr : &offset, flag) == BC_STS_INV_ARG &&
                  ioctl_calls == 0 && offset == 0xdeadbeef,
                  "invalid handle/buffer/length/output combinations reject before ioctl for clear and nonzero flags");
            f.Balanced();
        }
        f.Reset(); f.context.pIoDataFreeHd = nullptr;
        uint32_t offset = 0xdeadbeef;
        Check(DtsTxDmaText(&f.context, bytes, sizeof(bytes), &offset, flag) == BC_STS_INSUFF_RES &&
              !ioctl_calls && offset == 0xdeadbeef && !f.context.pIoDataFreeHd,
              "empty command pool returns promptly without ioctl or fabricated success");
        f.context.pIoDataFreeHd = &f.pooled;
        f.Balanced();
    }
}

int main()
{
    FlagsAndReturns(); InvalidArgumentsAndPoolExhaustion();
    std::printf("Direct input flags: %u checks, %u failures; no device or protected session\n", checks, failures);
    return failures ? 1 : 0;
}
