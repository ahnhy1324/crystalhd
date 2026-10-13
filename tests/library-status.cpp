/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Hardware-free status request/response tests. Link the real public API,
 * internal status marshalling, context and pooled ioctl implementation;
 * intercept only the final GET_DRV_STAT ioctl. No device is opened.
 */
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <pthread.h>
#include "7411d.h"
#include "libcrystalhd_if.h"
#include "libcrystalhd_int_if.h"
#include "libcrystalhd_priv.h"

extern bc_dil_glob_s *bc_dil_glob_ptr;
static unsigned failures, checks, calls, rx_peeks;
static BC_DTS_STATS request, response;
static BC_STATUS ioctl_status;
static DTS_LIB_CONTEXT *status_context;
static bool advance_epoch_during_ioctl;
static unsigned pause_calls;

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
    if (fd != 99 || code != BCM_IOC_GET_DRV_STAT) {
        std::fputs("unexpected ioctl in hardware-free status test\n", stderr);
        std::abort();
    }
    va_list args;
    va_start(args, code);
    BC_IOCTL_DATA *data = va_arg(args, BC_IOCTL_DATA *);
    va_end(args);
    ++calls;
    request = data->u.drvStat;
    /* The production driver's status handler suppresses RX peeking only
     * when request bit30 is present, independently of the returned counts.
     */
    if (!(request.DrvcpbEmptySize & (1U << 30))) ++rx_peeks;
    data->u.drvStat = response;
    data->RetSts = ioctl_status;
    if (advance_epoch_during_ioctl) {
        /* Model an old status ioctl completing after another thread has
         * already reset and recovered the decoder into the next epoch. */
        DtsLock(status_context);
        status_context->recoveryGeneration++;
        status_context->recoveryPending = false;
        status_context->recoveryStatus = BC_STS_SUCCESS;
        DtsUnLock(status_context);
    }
    return 0;
}

BC_STATUS DtsFWPauseVideo(HANDLE device, uint32_t)
{
    if (device != status_context)
        std::abort();
    ++pause_calls;
    return BC_STS_SUCCESS;
}

struct Fixture {
    bc_dil_glob_s globals = {};
    DTS_LIB_CONTEXT context = {};
    BC_IOCTL_DATA pooled = {};

    Fixture() {
        bc_dil_glob_ptr = &globals;
        context.Sig = LIB_CTX_SIG;
        context.DevHandle = 99;
        context.DevId = BC_PCI_DEVID_FLEA;
        context.HWOutPicWidth = 1920;
        context.VidParams.VideoAlgo = BC_VID_ALGO_H264;
        context.circBuf.freeSize = 123456;
        context.pIoDataFreeHd = &pooled;
        status_context = &context;
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&context.thLock, &attr);
        pthread_mutexattr_destroy(&attr);
        globals.stats.opFrameCaptured = 17;
        globals.stats.ipSampleCnt = 23;
        globals.stats.ipTotalSize = 456789;
        request = {};
        response = {};
        response.drvRLL = 2;
        response.drvFLL = 14;
        response.DrvcpbEmptySize = 8192;
        response.DrvNextMDataPLD = 27;
        response.picNumFlags = 19;
        calls = rx_peeks = 0;
        ioctl_status = BC_STS_SUCCESS;
        advance_epoch_during_ioctl = false;
        pause_calls = 0;
    }
    ~Fixture() {
        Check(context.pIoDataFreeHd == &pooled && pooled.next == nullptr,
              "status call returns the single owned ioctl object to its pool");
        pthread_mutex_destroy(&context.thLock);
        status_context = nullptr;
        bc_dil_glob_ptr = nullptr;
    }
};

static void InternalRequests()
{
    const uint32_t controls[] = {
        0, 1U << 29, 1U << 30, (1U << 29) | (1U << 30),
        1U << 31, 0x1fffffffU, 0xffffffffU
    };
    for (bool single : {false, true}) {
        for (uint32_t control : controls) {
            Fixture fixture;
            fixture.context.SingleThreadedAppMode = single;
            BC_DTS_STATS status = {};
            status.DrvcpbEmptySize = control;
            status.DrvNextMDataPLD = 0x80000780U;
            Check(DtsGetDrvStat(&fixture.context, &status) == BC_STS_SUCCESS,
                  "internal status succeeds through the actual ioctl path");
            Check(calls == 1 && request.DrvcpbEmptySize == (control & 0x40000000U),
                  "forward only TX-only bit30; leave legacy VC1 bit29 behavior unchanged");
            Check(request.DrvNextMDataPLD == (single ? 0x80000780U : 0),
                  "preserve existing single-threaded metadata-request semantics");
            Check(status.DrvcpbEmptySize == 8192 && status.drvRLL == 2 &&
                      status.drvFLL == 14 && status.opFrameCaptured == 17 &&
                      status.ipSampleCnt == 23 && status.ipTotalSize == 456789,
                  "driver response replaces request flags while retaining DIL counters");
        }
    }
}

static void PublicRequests()
{
    const uint32_t controls[] = { 0, 1U << 30, 3U << 30 };
    for (bool vc1 : {false, true}) {
        for (uint32_t control : controls) {
            Fixture fixture;
            const bool tx_only = (control & (1U << 30)) != 0;
            const bool real_hw_size = (control & (1U << 31)) != 0;
            fixture.context.SingleThreadedAppMode = true;
            if (vc1) fixture.context.VidParams.VideoAlgo = BC_VID_ALGO_VC1MP;
            response.eosDetected = 1;
            BC_DTS_STATUS status = {};
            status.cpbEmptySize = control;
            Check(DtsGetDriverStatus(&fixture.context, &status) == BC_STS_SUCCESS,
                  "public status succeeds through real internal marshalling");
            const uint32_t expected = tx_only ? 1U << 30 : 0;
            Check(request.DrvcpbEmptySize == expected,
                  "public API forwards TX-only bit30 without changing VC1 selection or bit31");
            Check(rx_peeks == (tx_only ? 0U : 1U),
                  "TX-only request suppresses RX peek; ordinary status still peeks");
            Check(status.cpbEmptySize == (real_hw_size ? 8192U : 123456U),
                  "bit31 retains its public-only hardware-versus-software free-size meaning");
            Check(status.ReadyListCount == 2 && status.FreeListCount == 14 &&
                      status.FramesCaptured == 17 && status.InputCount == 23 &&
                      status.InputTotalSize == 456789,
                  "TX-only still returns existing queue counts and library counters");
            Check(status.NextTimeStamp == (tx_only ? 0U : 540000U),
                  "ordinary metadata scaling and TX-only timestamp suppression are unchanged");
            uint8_t eos = 0;
            Check(DtsIsEndOfStream(&fixture.context, &eos) == BC_STS_SUCCESS && eos,
                  "TX-only status preserves the driver EOS observation");
        }
    }
}

static void FailuresAndReuse()
{
    Fixture fixture;
    fixture.context.SingleThreadedAppMode = true;
    BC_DTS_STATS status = {};
    status.DrvcpbEmptySize = 0x60000000U;
    status.DrvNextMDataPLD = 0x80000780U;
    ioctl_status = BC_STS_IO_ERROR;
    Check(DtsGetDrvStat(&fixture.context, &status) == BC_STS_IO_ERROR,
          "internal ioctl failure is propagated");
    Check(status.DrvcpbEmptySize == 0x60000000U &&
              status.DrvNextMDataPLD == 0x80000780U,
          "failed request does not publish a successful response");
    ioctl_status = BC_STS_SUCCESS;
    fixture.context.SingleThreadedAppMode = false;
    status = {};
    Check(DtsGetDrvStat(&fixture.context, &status) == BC_STS_SUCCESS &&
              request.DrvcpbEmptySize == 0 && request.DrvNextMDataPLD == 0,
          "reused pooled ioctl cannot retain prior request flags or metadata");
    BC_DTS_STATUS public_status = {};
    public_status.cpbEmptySize = 3U << 30;
    ioctl_status = BC_STS_IO_ERROR;
    Check(DtsGetDriverStatus(&fixture.context, &public_status) == BC_STS_IO_ERROR &&
              public_status.cpbEmptySize == (3U << 30),
          "public API propagates failure without overwriting caller output");
    const unsigned old_calls = calls;
    Check(DtsGetDrvStat(&fixture.context, nullptr) == BC_STS_ERROR &&
              DtsGetDriverStatus(&fixture.context, nullptr) == BC_STS_INV_ARG &&
              calls == old_calls,
          "invalid status pointers never issue an ioctl");
}

static void RecoveryEpochValidation()
{
    {
        Fixture fixture;
        fixture.context.SingleThreadedAppMode = true;
        response.drvRLL = 12;
        response.DrvNextMDataPLD = 27;
        response.picNumFlags = 0x55;
        response.eosDetected = 1;
        response.pwr_state_change = BC_HW_SUSPEND;
        advance_epoch_during_ioctl = true;
        BC_DTS_STATUS status = {};
        Check(DtsGetDriverStatus(&fixture.context, &status) == BC_STS_SUCCESS,
              "a delayed pre-epoch status ioctl still returns its base counters");
        Check(fixture.context.recoveryGeneration == 1 &&
                  !fixture.context.recoveryPending,
              "an old SUSPEND sample cannot create a second epoch after recovery");
        Check(!fixture.context.bEOS && fixture.context.DrvStatusEOSCnt == 0,
              "a delayed pre-epoch status suffix cannot republish stale EOS");
        Check(status.ReadyListCount == 0 && status.NextTimeStamp == 0 &&
                  status.picNumFlags == 0,
              "a delayed pre-epoch sample suppresses its stale ready/timestamp tuple");
        uint8_t eos = 1;
        Check(DtsIsEndOfStream(&fixture.context, &eos) == BC_STS_SUCCESS && !eos,
              "stale status EOS does not survive into the recovered epoch");
    }
    {
        Fixture fixture;
        fixture.context.DevId = BC_PCI_DEVID_LINK;
        fixture.context.SingleThreadedAppMode = true;
        response.drvRLL = 12;
        response.pwr_state_change = BC_HW_SUSPEND;
        advance_epoch_during_ioctl = true;
        BC_DTS_STATUS status = {};
        Check(DtsGetDriverStatus(&fixture.context, &status) == BC_STS_SUCCESS &&
                  pause_calls == 0 && !fixture.context.hw_paused,
              "stale LINK ready depth cannot auto-pause the recovered channel");
        Check(status.ReadyListCount == 0 && status.NextTimeStamp == 0 &&
                  status.picNumFlags == 0,
              "LINK flow-control revalidation clears the complete stale tuple");
    }
    {
        Fixture fixture;
        fixture.context.SingleThreadedAppMode = true;
        response.drvRLL = 12;
        response.eosDetected = 1;
        response.pwr_state_change = BC_HW_SUSPEND;
        BC_DTS_STATUS status = {};
        Check(DtsGetDriverStatus(&fixture.context, &status) == BC_STS_SUCCESS &&
                  fixture.context.recoveryGeneration == 1 &&
                  fixture.context.recoveryPending,
              "a current SUSPEND status atomically latches the first recovery epoch");
        Check(status.ReadyListCount == 0 && status.NextTimeStamp == 0 &&
                  status.picNumFlags == 0 && !fixture.context.bEOS,
              "the PM-latching status call cannot publish its pre-reset output suffix");
    }
    {
        Fixture fixture;
        fixture.context.recoveryGeneration = 4;
        fixture.context.recoveryPending = true;
        response.drvRLL = 12;
        response.pwr_state_change = BC_HW_SUSPEND;
        BC_DTS_STATUS status = {};
        Check(DtsGetDriverStatus(&fixture.context, &status) == BC_STS_SUCCESS &&
                  fixture.context.recoveryGeneration == 4 &&
                  fixture.context.recoveryPending,
              "a query begun during recovery cannot enqueue a nested PM epoch");
        Check(status.ReadyListCount == 0 && status.NextTimeStamp == 0 &&
                  status.picNumFlags == 0,
              "a query begun during recovery exposes no stale ready-facing state");
    }
}

int main()
{
    InternalRequests();
    PublicRequests();
    FailuresAndReuse();
    RecoveryEpochValidation();
    std::printf("Library status: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
