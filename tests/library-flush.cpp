/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Link the actual libcrystalhd_if.cpp/libcrystalhd_priv.cpp with function
 * sections and --wrap=ioctl/usleep/pthread_mutex_lock. Firmware entry points
 * are stubbed and unused pixel-copy paths abort;
 * flush, stop, cancellation, output-release and pthread locks are production
 * functions. No device, shared-memory setup or TX worker is started.
 */
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
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
static bool allow_output_io, admission_probe, resume_at_stop;
static bool admission_waiting, resume_admission, admission_done;
static unsigned fetch_calls, add_calls;
static BC_STATUS mock_fetch_status = BC_STS_NO_DATA;
static BC_STATUS mock_add_status = BC_STS_SUCCESS;
static bool cancel_after_fetch;
static thread_local unsigned delayed_output_lock;
static BC_STATUS mock_flush_status = BC_STS_SUCCESS;
static BC_STATUS mock_stop_status = BC_STS_SUCCESS;
static BC_STATUS mock_close_status = BC_STS_SUCCESS;
static unsigned close_calls;

extern "C" int __real_pthread_mutex_lock(pthread_mutex_t *mutex);
extern "C" int __wrap_pthread_mutex_lock(pthread_mutex_t *mutex)
{
    if (delayed_output_lock && observed && mutex == &observed->thLock &&
        --delayed_output_lock == 0) {
        std::unique_lock<std::mutex> lock(gate);
        admission_waiting = true;
        changed.notify_all();
        if (!changed.wait_for(lock, std::chrono::seconds(2), [] { return resume_admission; }))
            std::abort();
    }
    return __real_pthread_mutex_lock(mutex);
}

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

extern "C" int __wrap_ioctl(int, unsigned long code, ...)
{
    if (allow_output_io && (code == BCM_IOC_FETCH_RXBUFF || code == BCM_IOC_ADD_RXBUFFS)) {
        va_list args;
        va_start(args, code);
        BC_IOCTL_DATA *data = va_arg(args, BC_IOCTL_DATA *);
        va_end(args);
        if (code == BCM_IOC_FETCH_RXBUFF) {
            ++fetch_calls;
            data->RetSts = mock_fetch_status;
            if (cancel_after_fetch) {
                DtsLock(observed);
                observed->CancelWaiting = 1;
                DtsUnLock(observed);
            }
        } else {
            ++add_calls;
            data->RetSts = mock_add_status;
        }
        return 0;
    }
    std::fputs("unexpected ioctl in hardware-free flush test\n", stderr);
    std::abort();
}

BC_STATUS DtsFWDecFlushChannel(HANDLE, uint32_t mode)
{
    firmware_flush_mode = mode;
    return mock_flush_status;
}
BC_STATUS DtsFWPauseVideo(HANDLE, uint32_t) { return BC_STS_SUCCESS; }
BC_STATUS DtsFWCloseChannel(HANDLE, uint32_t)
{
    ++close_calls;
    return mock_close_status;
}
BC_STATUS DtsFWStopVideo(HANDLE, uint32_t, bool)
{
    ++stop_calls;
    pending_at_stop = DtsIsPend(observed);
    if (admission_probe && resume_at_stop) {
        std::unique_lock<std::mutex> lock(gate);
        resume_admission = true;
        changed.notify_all();
        if (!changed.wait_for(lock, std::chrono::seconds(2), [] { return admission_done; }))
            std::abort();
    }
    return mock_stop_status;
}
void DumpInputSampleToFile(uint8_t *, uint32_t) {}
/* Needed only by the unexecuted EOS branch; entering it is a test error. */
uint16_t WORD_SWAP(uint16_t) { std::abort(); }
void PTS2MakerBit5Bytes(uint8_t *, int64_t) { std::abort(); }
BC_STATUS DtsCopyRawDataToOutBuff(DTS_LIB_CONTEXT *, BC_DTS_PROC_OUT *, BC_DTS_PROC_OUT *)
{ std::abort(); }
BC_STATUS DtsCopyNV12ToYV12(DTS_LIB_CONTEXT *, BC_DTS_PROC_OUT *, BC_DTS_PROC_OUT *)
{ std::abort(); }
BC_STATUS DtsCopyNV12(DTS_LIB_CONTEXT *, BC_DTS_PROC_OUT *, BC_DTS_PROC_OUT *)
{ std::abort(); }
BC_STATUS DtsCopyFormat(DTS_LIB_CONTEXT *, BC_DTS_PROC_OUT *, BC_DTS_PROC_OUT *)
{ std::abort(); }

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

struct OutputContext {
    bc_dil_glob_s globals = {};
    DTS_LIB_CONTEXT context = {};
    BC_IOCTL_DATA output_data = {};
    OutputContext()
    {
        observed = &context;
        bc_dil_glob_ptr = &globals;
        context.Sig = LIB_CTX_SIG;
        context.State = BC_DEC_STATE_START;
        context.DevId = BC_PCI_DEVID_FLEA;
        context.DevHandle = 99; // Fake descriptor: only the ioctl wrapper sees it.
        context.ProcessID = getpid();
        context.FixFlags = DTS_LOAD_FILE_PLAY_FW;
        context.pOutData = &output_data;
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&context.thLock, &attr);
        pthread_mutexattr_destroy(&attr);
        if (txBufInit(&context.circBuf, CIRC_TX_BUF_SIZE) != BC_STS_SUCCESS)
            std::abort();
        allow_output_io = true;
        mock_fetch_status = BC_STS_NO_DATA;
        mock_add_status = BC_STS_SUCCESS;
        cancel_after_fetch = false;
        fetch_calls = add_calls = cancellation_sleeps = stop_calls = 0;
        close_calls = 0;
        mock_flush_status = mock_stop_status = mock_close_status = BC_STS_SUCCESS;
    }
    ~OutputContext()
    {
        txBufFree(&context.circBuf);
        pthread_mutex_destroy(&context.thLock);
        observed = nullptr;
        bc_dil_glob_ptr = nullptr;
        allow_output_io = admission_probe = false;
        mock_flush_status = mock_stop_status = mock_close_status = BC_STS_SUCCESS;
    }
};

static void test_late_output(unsigned mode, bool no_copy, bool during_stop)
{
    OutputContext fixture;
    admission_probe = true;
    resume_at_stop = during_stop;
    admission_waiting = resume_admission = admission_done = false;
    BC_STATUS output_status = BC_STS_ERROR;
    std::thread output([&] {
        BC_DTS_PROC_OUT out = {};
        delayed_output_lock = 1;
        output_status = no_copy ? DtsProcOutputNoCopy(&fixture.context, 0, &out) :
                                 DtsProcOutput(&fixture.context, 0, &out);
        std::lock_guard<std::mutex> lock(gate);
        admission_done = true;
        changed.notify_all();
    });
    {
        std::unique_lock<std::mutex> lock(gate);
        if (!changed.wait_for(lock, std::chrono::seconds(2), [] { return admission_waiting; }))
            std::abort();
    }
    const BC_STATUS flushed = mode ? DtsFlushInput(&fixture.context, mode) :
                                     DtsStopDecoder(&fixture.context);
    if (!during_stop) {
        std::lock_guard<std::mutex> lock(gate);
        resume_admission = true;
        changed.notify_all();
    }
    output.join();
    std::printf("late-output mode=%u no_copy=%d during_stop=%d status=%d fetch=%u add=%u pending=%d\n",
                mode, no_copy, during_stop, output_status, fetch_calls, add_calls,
                fixture.context.ProcOutPending);
    check(flushed == BC_STS_SUCCESS, "flush with a not-yet-admitted caller succeeds");
    check(output_status == BC_STS_DEC_NOT_STARTED || output_status == BC_STS_DEC_NOT_OPEN ||
          output_status == BC_STS_IO_USER_ABORT, "late caller observes flush/closed state");
    check(fetch_calls == 0 && add_calls == 0, "late output never reaches driver after flush barrier");
    check(fixture.context.ProcOutPending == 0, "rejected late output owns no pending reference");
}

static void test_busy_output(bool no_copy)
{
    OutputContext fixture;
    fixture.context.ProcOutPending = 1; // A different successful fetch owns this buffer.
    fixture.output_data.u.RxBuffs.YBuffDoneSz = 123;
    const BC_IOCTL_DATA original = fixture.output_data;
    BC_DTS_PROC_OUT out = {};
    out.DropFrames = 1;
    const BC_STATUS status = no_copy ? DtsProcOutputNoCopy(&fixture.context, 0, &out) :
                                     DtsProcOutput(&fixture.context, 0, &out);
    std::printf("busy-output no_copy=%d status=%d fetch=%u add=%u pending=%d\n",
                no_copy, status, fetch_calls, add_calls, fixture.context.ProcOutPending);
    check(status == BC_STS_BUSY, "competing output returns BUSY");
    check(fixture.context.ProcOutPending == 1, "rejected caller cannot retire another output");
    check(fetch_calls == 0 && add_calls == 0, "rejected caller cannot fetch or repost another output");
    check(std::memcmp(&original, &fixture.output_data, sizeof(original)) == 0,
          "rejected caller leaves the acquired driver buffer untouched");
}

static void test_competing_admissions()
{
    OutputContext fixture;
    mock_fetch_status = BC_STS_SUCCESS;
    admission_waiting = resume_admission = admission_done = false;
    BC_STATUS first_status = BC_STS_ERROR;
    std::thread first([&] {
        BC_DTS_PROC_OUT out = {};
        /* Before the fix this is the gap between IsPend and IncPend. With
         * atomic admission the first lock has already reserved pOutData.
         */
        delayed_output_lock = 2;
        first_status = DtsProcOutputNoCopy(&fixture.context, 0, &out);
    });
    {
        std::unique_lock<std::mutex> lock(gate);
        if (!changed.wait_for(lock, std::chrono::seconds(2), [] { return admission_waiting; }))
            std::abort();
    }
    BC_DTS_PROC_OUT out = {};
    const BC_STATUS second_status = DtsProcOutputNoCopy(&fixture.context, 0, &out);
    {
        std::lock_guard<std::mutex> lock(gate);
        resume_admission = true;
        changed.notify_all();
    }
    first.join();
    std::printf("competing-admissions first=%d second=%d fetch=%u pending=%d\n",
                first_status, second_status, fetch_calls, fixture.context.ProcOutPending);
    check((first_status == BC_STS_SUCCESS && second_status == BC_STS_BUSY) ||
          (first_status == BC_STS_BUSY && second_status == BC_STS_SUCCESS),
          "only one competing caller acquires the shared output buffer");
    check(fetch_calls == 1 && fixture.context.ProcOutPending == 1,
          "atomic admission reserves exactly one fetch and pending reference");
    check(DtsReleaseOutputBuffs(&fixture.context, nullptr, FALSE) == BC_STS_SUCCESS &&
          fixture.context.ProcOutPending == 0 && add_calls == 1,
          "successful NoCopy output remains releasable exactly once");
}

static void test_failed_fetch(bool no_copy, BC_STATUS fetch_status)
{
    OutputContext fixture;
    mock_fetch_status = fetch_status;
    BC_DTS_PROC_OUT out = {};
    out.DropFrames = 1;
    const BC_STATUS status = no_copy ? DtsProcOutputNoCopy(&fixture.context, 0, &out) :
                                     DtsProcOutput(&fixture.context, 0, &out);
    check(status == (fetch_status == BC_STS_TIMEOUT ? BC_STS_NO_DATA : fetch_status),
          "failed fetch preserves status/peek-timeout semantics");
    check(fetch_calls == 1 && add_calls == 0 && fixture.context.ProcOutPending == 0,
          "failed fetch retires only its admission without posting a buffer");
    check(out.DropFrames == 1, "failed fetch cannot consume a requested frame drop");
}

static void test_canceled_fetch(bool no_copy, bool repost_fails)
{
    OutputContext fixture;
    mock_fetch_status = BC_STS_SUCCESS;
    mock_add_status = repost_fails ? BC_STS_IO_ERROR : BC_STS_SUCCESS;
    cancel_after_fetch = true;
    BC_DTS_PROC_OUT out = {};
    out.DropFrames = 1;
    const BC_STATUS status = no_copy ? DtsProcOutputNoCopy(&fixture.context, 0, &out) :
                                     DtsProcOutput(&fixture.context, 0, &out);
    check(status == BC_STS_IO_USER_ABORT, "canceled fetch transfers no successful output");
    check(fetch_calls == 1 && add_calls == 1 && out.DropFrames == 1,
          "cancellation returns the buffer once without public-wrapper reuse");
    check(fixture.context.ProcOutPending == (repost_fails ? 1 : 0),
          "failed cancellation repost cannot falsely retire the buffer");
    fixture.context.CancelWaiting = 0; // Model cancellation's final flag reset.
    if (repost_fails) {
        const unsigned old_fetch_calls = fetch_calls;
        check(DtsProcOutputNoCopy(&fixture.context, 0, &out) == BC_STS_BUSY &&
              fetch_calls == old_fetch_calls && add_calls == 1,
              "failed repost keeps new fetches out until lifecycle teardown");
    }
}

static void test_cleanup_errors(unsigned mode, unsigned errors)
{
    OutputContext fixture;
    mock_flush_status = errors & 1 ? BC_STS_FW_CMD_ERR : BC_STS_SUCCESS;
    mock_stop_status = errors & 2 ? BC_STS_IO_ERROR : BC_STS_SUCCESS;
    mock_close_status = errors & 4 ? BC_STS_TIMEOUT : BC_STS_SUCCESS;
    const BC_STATUS expected = mode >= 2 && mock_flush_status != BC_STS_SUCCESS ?
        mock_flush_status : mock_stop_status != BC_STS_SUCCESS ? mock_stop_status :
        mode == 0 ? BC_STS_SUCCESS : mock_close_status;
    const BC_STATUS status = mode == 0 ? DtsStopDecoder(&fixture.context) :
        mode == 1 ? DtsCloseDecoder(&fixture.context) : DtsFlushInput(&fixture.context, mode);
    std::printf("cleanup mode=%u failures=%u status=%d expected=%d\n",
                mode, errors, status, expected);
    check(status == expected, "cleanup reports the first firmware failure, not a later success");
    check(stop_calls == 1 && close_calls == (mode == 0 ? 0U : 1U),
          "failure does not skip the existing stop/close cleanup sequence");
    check(fixture.context.State == (mode == 0 ? BC_DEC_STATE_STOP : BC_DEC_STATE_CLOSE),
          "error reporting preserves the existing lifecycle state policy");
}

int main()
{
    test_flush(2, true);
    test_flush(4, true);
    test_flush(2, false);
    test_flush(4, false);
    for (unsigned mode : {2U, 4U}) {
        test_late_output(mode, false, true);
        test_late_output(mode, true, true);
        test_late_output(mode, false, false);
        test_late_output(mode, true, false);
    }
    test_busy_output(false);
    test_busy_output(true);
    test_late_output(0, false, true);
    test_late_output(0, true, true);
    test_competing_admissions();
    for (BC_STATUS status : {BC_STS_NO_DATA, BC_STS_TIMEOUT, BC_STS_IO_ERROR}) {
        test_failed_fetch(false, status);
        test_failed_fetch(true, status);
    }
    test_canceled_fetch(false, false);
    test_canceled_fetch(true, false);
    test_canceled_fetch(false, true);
    test_canceled_fetch(true, true);
    for (unsigned mode : {0U, 1U, 2U, 4U})
        for (unsigned errors = 0; errors < 8; ++errors)
            test_cleanup_errors(mode, errors);
    if (failures)
        return 1;
    std::puts("PASS: production flush completes pending output before decoder stop");
    return 0;
}
