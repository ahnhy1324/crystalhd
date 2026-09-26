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
    return 0;
}

/* Only the unexecuted BCM70012 flow-control branch references firmware. */
BC_STATUS DtsFWPauseVideo(HANDLE, uint32_t) { std::abort(); }

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
    }
    ~Fixture() {
        Check(context.pIoDataFreeHd == &pooled && pooled.next == nullptr,
              "status call returns the single owned ioctl object to its pool");
        pthread_mutex_destroy(&context.thLock);
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
    for (bool vc1 : {false, true}) {
        for (bool tx_only : {false, true}) {
            Fixture fixture;
            fixture.context.SingleThreadedAppMode = true;
            if (vc1) fixture.context.VidParams.VideoAlgo = BC_VID_ALGO_VC1MP;
            BC_DTS_STATUS status = {};
            /* This is the exact request used on every real TX-worker poll. */
            status.cpbEmptySize = tx_only ? (3U << 30) : 0;
            Check(DtsGetDriverStatus(&fixture.context, &status) == BC_STS_SUCCESS,
                  "public status succeeds through real internal marshalling");
            const uint32_t expected = tx_only ? 1U << 30 : 0;
            Check(request.DrvcpbEmptySize == expected,
                  "public API forwards TX-only bit30 without changing VC1 selection or bit31");
            Check(rx_peeks == (tx_only ? 0U : 1U),
                  "TX-only request suppresses RX peek; ordinary status still peeks");
            Check(status.cpbEmptySize == (tx_only ? 8192U : 123456U),
                  "bit31 retains its public-only hardware-versus-software free-size meaning");
            Check(status.ReadyListCount == 2 && status.FreeListCount == 14 &&
                      status.FramesCaptured == 17 && status.InputCount == 23 &&
                      status.InputTotalSize == 456789,
                  "TX-only still returns existing queue counts and library counters");
            Check(status.NextTimeStamp == (tx_only ? 0U : 540000U),
                  "ordinary metadata scaling and TX-only timestamp suppression are unchanged");
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

int main()
{
    InternalRequests();
    PublicRequests();
    FailuresAndReuse();
    std::printf("Library status: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
