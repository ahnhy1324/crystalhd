/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Link the actual libcrystalhd_if.cpp/libcrystalhd_priv.cpp with function
 * sections and --wrap=ioctl/usleep. Only firmware entry points are stubbed;
 * flush, stop, cancellation, output-release and pthread locks are production
 * functions. No device, shared-memory setup or TX worker is started.
 */
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <unistd.h>
#include "7411d.h"
#include "libcrystalhd_if.h"
#include "libcrystalhd_int_if.h"
#include "libcrystalhd_priv.h"
#include "libcrystalhd_fwcmds.h"

extern bc_dil_glob_s *bc_dil_glob_ptr;
static DTS_LIB_CONTEXT *observed;
static std::mutex gate;
static std::condition_variable changed;
static bool release_output, lock_checked, completed, output_lock_blocked;
static unsigned cancellation_sleeps, firmware_flush_mode, stop_calls;
static bool pending_at_stop;
static unsigned failures;

static void check(bool condition, const char *message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

extern "C" int __wrap_usleep(useconds_t microseconds)
{
    if (microseconds != 100000)
        return 0;
    ++cancellation_sleeps;
    std::unique_lock<std::mutex> lock(gate);
    release_output = true;
    changed.notify_all();
    /* Start the completion during cancellation's first poll, not merely
     * before flush. This makes the old held-lock interleaving deterministic.
     * Compress each production 100ms wait; retain its actual retry counter.
     */
    if (!changed.wait_for(lock, std::chrono::seconds(1), [] { return lock_checked; }))
        std::abort();
    /* An available mutex lets the completion finish immediately. Wait for
     * that handshake rather than making correctness depend on a 2ms slice
     * of scheduler time on a busy build machine. A genuinely held mutex
     * must run the production retry/timeout path, without blocking the test.
     */
    if (!output_lock_blocked &&
        !changed.wait_for(lock, std::chrono::seconds(1), [] { return completed; }))
        std::abort();
    return 0;
}

extern "C" int __wrap_ioctl(int, unsigned long, ...)
{
    std::fputs("unexpected ioctl in hardware-free flush test\n", stderr);
    std::abort();
}

BC_STATUS DtsFWDecFlushChannel(HANDLE, uint32_t mode)
{
    firmware_flush_mode = mode;
    return BC_STS_SUCCESS;
}
BC_STATUS DtsFWPauseVideo(HANDLE, uint32_t) { return BC_STS_SUCCESS; }
BC_STATUS DtsFWCloseChannel(HANDLE, uint32_t) { return BC_STS_SUCCESS; }
BC_STATUS DtsFWStopVideo(HANDLE, uint32_t, bool)
{
    ++stop_calls;
    pending_at_stop = DtsIsPend(observed);
    return BC_STS_SUCCESS;
}
void DumpInputSampleToFile(uint8_t *, uint32_t) {}
/* Needed only by the unexecuted EOS branch; entering it is a test error. */
uint16_t WORD_SWAP(uint16_t) { std::abort(); }
void PTS2MakerBit5Bytes(uint8_t *, int64_t) { std::abort(); }

static void test_flush(unsigned mode, bool pending)
{
    release_output = lock_checked = completed = output_lock_blocked = false;
    cancellation_sleeps = firmware_flush_mode = stop_calls = 0;
    pending_at_stop = false;
    bc_dil_glob_s globals = {};
    bc_dil_glob_ptr = &globals;
    DTS_LIB_CONTEXT context = {};
    observed = &context;
    context.Sig = LIB_CTX_SIG;
    context.State = BC_DEC_STATE_START;
    context.DevId = BC_PCI_DEVID_FLEA;
    context.ProcessID = getpid();
    context.ProcOutPending = pending ? 1 : 0;
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&context.thLock, &attr);
    pthread_mutexattr_destroy(&attr);
    if (txBufInit(&context.circBuf, CIRC_TX_BUF_SIZE) != BC_STS_SUCCESS)
        std::abort();

    std::thread output;
    if (pending) {
        output = std::thread([&] {
            {
                std::unique_lock<std::mutex> lock(gate);
                changed.wait(lock, [] { return release_output; });
            }
            const int result = pthread_mutex_trylock(&context.thLock);
            output_lock_blocked = result == EBUSY;
            if (result == 0)
                pthread_mutex_unlock(&context.thLock);
            else if (result != EBUSY)
                std::abort();
            {
                std::lock_guard<std::mutex> lock(gate);
                lock_checked = true;
                changed.notify_all();
            }
            BC_DEC_YUV_BUFFS ignored = {};
            /* The real release/decrement path after a fetch. SkipAddBuff
             * avoids posting a replacement receive buffer to hardware.
             */
            if (DtsRelRxBuff(&context, &ignored, TRUE) != BC_STS_SUCCESS)
                std::abort();
            {
                std::lock_guard<std::mutex> lock(gate);
                completed = true;
                changed.notify_all();
            }
        });
    }
    const BC_STATUS status = DtsFlushInput(&context, mode);
    if (output.joinable())
        output.join();
    std::printf("mode=%u pending=%d flush=%d cancellation_polls=%u "
                "completion_lock_blocked=%d pending_at_firmware_stop=%d\n",
                mode, pending, status, cancellation_sleeps,
                output_lock_blocked, pending_at_stop);
    check(status == BC_STS_SUCCESS, "flush succeeds after output is released");
    check(!output_lock_blocked, "cancellation leaves output-completion mutex available");
    check(!pending_at_stop, "output completion precedes firmware stop");
    check(cancellation_sleeps <= 1, "completed output does not hit cancellation timeout");
    check(context.ProcOutPending == 0 && context.CancelWaiting == 0,
          "flush retires output and clears cancellation state");
    check(stop_calls == 1 && firmware_flush_mode == 2,
          "mode2/mode4 preserve the decoder-flush and single-stop sequence");
    check(context.State == BC_DEC_STATE_CLOSE && context.circBuf.busySize == 0,
          "successful flush closes decoder and empties input ring");
    txBufFree(&context.circBuf);
    pthread_mutex_destroy(&context.thLock);
    bc_dil_glob_ptr = nullptr;
    observed = nullptr;
}

int main()
{
    test_flush(2, true);
    test_flush(4, true);
    test_flush(2, false);
    test_flush(4, false);
    if (failures)
        return 1;
    std::puts("PASS: production flush completes pending output before decoder stop");
    return 0;
}
