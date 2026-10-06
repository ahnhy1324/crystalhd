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
#include <vector>
#include <unistd.h>
#include <sys/resource.h>
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
static unsigned failures, checks;
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
/* Held-owner cases keep a successful NoCopy borrow across the real retry
 * loop. Separate deterministic CPU threads exercise active-call windows. */
static bool held_owner_case;
static unsigned unmap_calls, activate_calls, start_calls, interface_calls;
static unsigned pause_calls;
static BC_STATUS mock_pause_status = BC_STS_SUCCESS;
static bool pending_at_unmap, pending_at_interface;
static BC_STATUS mock_unmap_status = BC_STS_SUCCESS;
static bool mock_unmap_syscall_failure;
enum PhaseWindow { WINDOW_NONE, WINDOW_EMPTY_CTX_LOCK, WINDOW_FINALIZER, WINDOW_PUBLISH };
static thread_local PhaseWindow phase_window;
static bool phase_window_reached, resume_phase_window, phase_actor_done;
static bool cancel_releases_publish;
static thread_local bool pause_cancel_owner;
static bool cancel_owner_reached, resume_cancel_owner;
static unsigned unmap_fail_once_at;

static void wait_phase_window()
{
    phase_window = WINDOW_NONE;
    std::unique_lock<std::mutex> lock(gate);
    phase_window_reached = true;
    changed.notify_all();
    if (!changed.wait_for(lock, std::chrono::seconds(2), [] { return resume_phase_window; }))
        std::abort();
}
extern "C" void __real_DtsFinishOutputCall(DTS_LIB_CONTEXT *);
extern "C" void __wrap_DtsFinishOutputCall(DTS_LIB_CONTEXT *context)
{
    if (phase_window == WINDOW_FINALIZER)
        wait_phase_window();
    __real_DtsFinishOutputCall(context);
}
extern "C" BC_STATUS __real_DtsPublishOutput(DTS_LIB_CONTEXT *, BC_DTS_PROC_OUT *, BC_STATUS);
extern "C" BC_STATUS __wrap_DtsPublishOutput(DTS_LIB_CONTEXT *context,
                                            BC_DTS_PROC_OUT *output, BC_STATUS status)
{
    if (phase_window == WINDOW_PUBLISH)
        wait_phase_window();
    return __real_DtsPublishOutput(context, output, status);
}

extern "C" BC_STATUS __wrap_DtsReleaseInterface(DTS_LIB_CONTEXT *context)
{
    if (!held_owner_case || context != observed)
        std::abort();
    ++interface_calls;
    pending_at_interface = DtsIsPend(context);
    /* Never consume/free a live stack fixture. Only record the production
     * DeviceClose's interface-release call boundary. */
    return BC_STS_SUCCESS;
}
struct FetchReply {
    BC_STATUS status;
    BC_DEC_OUT_BUFF output;
};
struct AddReply {
    BC_STATUS status;
    bool syscall_failure;
};
static std::vector<FetchReply> fetch_replies;
static std::vector<AddReply> add_replies;
static std::vector<uint8_t *> reposted_buffers;
static std::vector<uint8_t *> copied_buffers;
static std::vector<uint8_t *> callback_buffers;
static unsigned copy_calls, callback_calls;
static BC_STATUS mock_copy_status = BC_STS_SUCCESS;
static bool allow_copy;
static uint8_t frame_storage[8][32];

extern "C" int __real_pthread_mutex_lock(pthread_mutex_t *mutex);
extern "C" int __wrap_pthread_mutex_lock(pthread_mutex_t *mutex)
{
    if (phase_window == WINDOW_EMPTY_CTX_LOCK && observed &&
        mutex == &observed->thLock && fetch_calls &&
        observed->outputPhase == DTS_OUTPUT_ACTIVE && !observed->ProcOutPending)
        wait_phase_window();
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
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

extern "C" int __wrap_usleep(useconds_t microseconds)
{
    if (microseconds != 100000)
        return 0;
    __sync_add_and_fetch(&cancellation_sleeps, 1);
    if (pause_cancel_owner) {
        pause_cancel_owner = false;
        std::unique_lock<std::mutex> lock(gate);
        cancel_owner_reached = true;
        changed.notify_all();
        if (!changed.wait_for(lock, std::chrono::seconds(2), [] { return resume_cancel_owner; }))
            std::abort();
        return 0;
    }
    if (cancel_releases_publish) {
        std::unique_lock<std::mutex> lock(gate);
        resume_phase_window = true;
        changed.notify_all();
        if (!changed.wait_for(lock, std::chrono::seconds(2), [] { return phase_actor_done; }))
            std::abort();
        return 0;
    }
    if (held_owner_case)
        return 0;
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
    if (held_owner_case && code == BCM_IOC_FLUSH_RX_CAP) {
        va_list args;
        va_start(args, code);
        BC_IOCTL_DATA *data = va_arg(args, BC_IOCTL_DATA *);
        va_end(args);
        ++unmap_calls;
        pending_at_unmap = DtsIsPend(observed);
        check(data->u.FlushRxCap.bDiscardOnly == FALSE,
              "destructive unmap is distinguished from discard-only");
        data->RetSts = unmap_calls == unmap_fail_once_at ? BC_STS_IO_ERROR : mock_unmap_status;
        if (mock_unmap_syscall_failure) {
            errno = EFAULT;
            return -1;
        }
        return 0;
    }
    if (allow_output_io && (code == BCM_IOC_FETCH_RXBUFF || code == BCM_IOC_ADD_RXBUFFS)) {
        va_list args;
        va_start(args, code);
        BC_IOCTL_DATA *data = va_arg(args, BC_IOCTL_DATA *);
        va_end(args);
        if (code == BCM_IOC_FETCH_RXBUFF) {
            const unsigned call = fetch_calls++;
            if (call < fetch_replies.size()) {
                data->u.DecOutData = fetch_replies[call].output;
                data->RetSts = fetch_replies[call].status;
            } else {
                data->RetSts = mock_fetch_status;
            }
            if (cancel_after_fetch) {
                DtsLock(observed);
                observed->CancelWaiting = 1;
                DtsUnLock(observed);
            }
        } else {
            const unsigned call = add_calls++;
            reposted_buffers.push_back(data->u.RxBuffs.YuvBuff);
            if (call < add_replies.size()) {
                data->RetSts = add_replies[call].status;
                if (add_replies[call].syscall_failure) {
                    errno = EFAULT;
                    return -1;
                }
            } else {
                data->RetSts = mock_add_status;
            }
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
BC_STATUS DtsFWPauseVideo(HANDLE, uint32_t)
{ ++pause_calls; return mock_pause_status; }
BC_STATUS DtsFWActivateDecoder(HANDLE)
{ ++activate_calls; return BC_STS_SUCCESS; }
BC_STATUS DtsFWStartVideo(HANDLE, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t)
{ ++start_calls; return BC_STS_SUCCESS; }
BC_STATUS DtsSetProgressive(HANDLE, uint32_t)
{ std::abort(); }
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
static BC_STATUS copy_output(BC_DTS_PROC_OUT *source)
{
    if (!allow_copy)
        std::abort();
    ++copy_calls;
    copied_buffers.push_back(source->Ybuff);
    return mock_copy_status;
}
BC_STATUS DtsCopyRawDataToOutBuff(DTS_LIB_CONTEXT *, BC_DTS_PROC_OUT *, BC_DTS_PROC_OUT *source)
{ return copy_output(source); }
BC_STATUS DtsCopyNV12ToYV12(DTS_LIB_CONTEXT *, BC_DTS_PROC_OUT *, BC_DTS_PROC_OUT *source)
{ return copy_output(source); }
BC_STATUS DtsCopyNV12(DTS_LIB_CONTEXT *, BC_DTS_PROC_OUT *, BC_DTS_PROC_OUT *source)
{ return copy_output(source); }
BC_STATUS DtsCopyFormat(DTS_LIB_CONTEXT *, BC_DTS_PROC_OUT *, BC_DTS_PROC_OUT *source)
{ return copy_output(source); }

static BC_STATUS output_callback(void *, uint32_t, uint32_t, uint32_t, void *opaque)
{
    BC_DTS_PROC_OUT *output = static_cast<BC_DTS_PROC_OUT *>(opaque);

    ++callback_calls;
    callback_buffers.push_back(output->Ybuff);
    return BC_STS_SUCCESS;
}

static void reset_output_mocks()
{
    fetch_replies.clear();
    add_replies.clear();
    reposted_buffers.clear();
    copied_buffers.clear();
    callback_buffers.clear();
    copy_calls = callback_calls = 0;
    mock_copy_status = BC_STS_SUCCESS;
    mock_unmap_status = BC_STS_SUCCESS;
    mock_unmap_syscall_failure = false;
    unmap_fail_once_at = 0;
    cancel_releases_publish = false;
    allow_copy = false;
}

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
        reset_output_mocks();
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
        reset_output_mocks();
        mock_flush_status = mock_stop_status = mock_close_status = BC_STS_SUCCESS;
    }
};

enum AddOutcome {
    ADD_SUCCEEDS,
    ADD_STATUS_FAILURE,
    ADD_SYSCALL_FAILURE,
};

static AddReply make_add_reply(AddOutcome outcome)
{
    if (outcome == ADD_STATUS_FAILURE)
        return AddReply{BC_STS_IO_ERROR, false};
    if (outcome == ADD_SYSCALL_FAILURE)
        return AddReply{BC_STS_SUCCESS, true};
    return AddReply{BC_STS_SUCCESS, false};
}

static BC_STATUS add_result(AddOutcome outcome)
{
    return outcome == ADD_STATUS_FAILURE ? BC_STS_IO_ERROR :
        outcome == ADD_SYSCALL_FAILURE ? BC_STS_ERROR : BC_STS_SUCCESS;
}

static FetchReply make_frame(unsigned index, uint32_t picture_number, int32_t session)
{
    BC_DEC_OUT_BUFF output = {};

    output.OutPutBuffs.YuvBuff = frame_storage[index];
    output.OutPutBuffs.YuvBuffSz = sizeof(frame_storage[index]);
    output.OutPutBuffs.UVbuffOffset = sizeof(frame_storage[index]) / 2;
    output.OutPutBuffs.YBuffDoneSz = sizeof(frame_storage[index]) / 2;
    output.OutPutBuffs.UVBuffDoneSz = sizeof(frame_storage[index]) / 2;
    output.PibInfo.ppb.picture_number = picture_number;
    output.PibInfo.ppb.width = 16;
    output.PibInfo.ppb.height = 16;
    output.PibInfo.ptsStcOffset = session;
    output.Flags = COMP_FLAG_PIB_VALID | COMP_FLAG_DATA_VALID;
    return FetchReply{BC_STS_SUCCESS, output};
}

static FetchReply make_fetch_error(BC_STATUS status)
{
    BC_DEC_OUT_BUFF output = {};

    return FetchReply{status, output};
}

static void prepare_scripted_output(OutputContext &fixture, uint32_t device)
{
    fixture.context.DevId = device;
    fixture.context.FixFlags = 0;
    fixture.context.RegCfg.DbgOptions |= BC_BIT(6);
    fixture.context.VidParams.Progressive = TRUE;
    fixture.context.HWOutPicWidth = 16;
    fixture.context.SingleThreadedAppMode = 0;
    allow_copy = true;
}

static BC_DTS_PROC_OUT make_public_output(OutputContext &fixture, uint8_t drops)
{
    BC_DTS_PROC_OUT output = {};

    output.hnd = &fixture.context;
    output.AppCallBack = output_callback;
    output.DropFrames = drops;
    return output;
}

static bool reposts_match(unsigned count)
{
    if (reposted_buffers.size() != count)
        return false;
    for (unsigned i = 0; i < count; ++i) {
        if (reposted_buffers[i] != frame_storage[i])
            return false;
    }
    return true;
}

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

static void test_repeated_picture_repost(AddOutcome outcome)
{
    OutputContext fixture;
    prepare_scripted_output(fixture, BC_PCI_DEVID_LINK);
    fixture.context.LastPicNum = 10;
    fixture.context.LastSessNum = 4;
    fixture.context.PullDownFlag = 0;
    fetch_replies.push_back(make_frame(0, 10, 4));
    add_replies.push_back(make_add_reply(outcome));
    BC_DTS_PROC_OUT output = make_public_output(fixture, 2);
    const BC_STATUS status = DtsProcOutput(&fixture.context, 0, &output);
    const BC_STATUS expected = outcome == ADD_SUCCEEDS ? BC_STS_NO_DATA : add_result(outcome);

    check(status == expected, "LINK repeat reports the repost result before NO_DATA");
    check(fetch_calls == 1 && add_calls == 1 && reposts_match(1),
          "LINK repeat fetches and reposts its buffer exactly once");
    check(copy_calls == 0 && callback_calls == 0 && output.DropFrames == 2,
          "LINK repeat cannot copy, callback, or consume a requested drop");
    check(fixture.context.ProcOutPending == (outcome == ADD_SUCCEEDS ? 0 : 1),
          "LINK repeat retires pending ownership only after confirmed repost");
    if (outcome != ADD_SUCCEEDS) {
        const unsigned old_fetch_calls = fetch_calls;
        const unsigned old_add_calls = add_calls;
        check(DtsProcOutput(&fixture.context, 0, &output) == BC_STS_BUSY &&
              fetch_calls == old_fetch_calls && add_calls == old_add_calls,
              "failed LINK repeat repost blocks reuse without retrying ownership");
    }
}

static void test_flea_repeat_seed_control()
{
    OutputContext fixture;
    prepare_scripted_output(fixture, BC_PCI_DEVID_FLEA);
    fixture.context.LastPicNum = 10;
    fixture.context.LastSessNum = 4;
    fetch_replies.push_back(make_frame(0, 10, 4));
    add_replies.push_back(make_add_reply(ADD_SUCCEEDS));
    BC_DTS_PROC_OUT output = make_public_output(fixture, 0);

    check(DtsProcOutput(&fixture.context, 0, &output) == BC_STS_SUCCESS,
          "FLEA bypasses LINK repeated-picture filtering");
    check(fetch_calls == 1 && add_calls == 1 && reposts_match(1),
          "FLEA repeat seed retains one fetch and one repost");
    check(copy_calls == 1 && callback_calls == 1 &&
          copied_buffers[0] == frame_storage[0] && callback_buffers[0] == frame_storage[0],
          "FLEA repeat seed delivers the fetched frame once");
    check(fixture.context.ProcOutPending == 0,
          "FLEA delivery retires pending ownership after repost");
}

static void test_successful_drops(uint32_t device, bool single_threaded, uint8_t drops)
{
    OutputContext fixture;
    prepare_scripted_output(fixture, device);
    fixture.context.SingleThreadedAppMode = single_threaded;
    const unsigned frame_count = drops + (single_threaded ? 0U : 1U);
    for (unsigned i = 0; i < frame_count; ++i) {
        fetch_replies.push_back(make_frame(i, 10 + i, 4));
        add_replies.push_back(make_add_reply(ADD_SUCCEEDS));
    }
    BC_DTS_PROC_OUT output = make_public_output(fixture, drops);

    check(DtsProcOutput(&fixture.context, 0, &output) == BC_STS_SUCCESS,
          "successful frame drops preserve public success");
    check(output.DropFrames == 0 && fetch_calls == frame_count &&
          add_calls == frame_count && reposts_match(frame_count),
          "each dropped or delivered buffer has one distinct fetch and repost");
    check(fixture.context.ProcOutPending == 0,
          "successful drops and delivery retire every pending reference");
    if (single_threaded) {
        check(copy_calls == 0 && callback_calls == 0,
              "single-threaded final drop returns without delivering a frame");
        check(output.PicInfo.picture_number == 9U + frame_count,
              "single-threaded final drop preserves the dropped picture number");
    } else {
        check(copy_calls == 1 && callback_calls == 1 &&
              copied_buffers[0] == frame_storage[frame_count - 1] &&
              callback_buffers[0] == frame_storage[frame_count - 1],
              "ordinary mode delivers only a freshly fetched undropped frame");
    }
}

static void test_failed_drop_repost(uint32_t device, bool single_threaded,
                                    unsigned failure_index, AddOutcome outcome)
{
    OutputContext fixture;
    prepare_scripted_output(fixture, device);
    fixture.context.SingleThreadedAppMode = single_threaded;
    const uint8_t drops = 2;
    for (unsigned i = 0; i <= failure_index; ++i) {
        fetch_replies.push_back(make_frame(i, 10 + i, 4));
        add_replies.push_back(make_add_reply(i == failure_index ? outcome : ADD_SUCCEEDS));
    }
    BC_DTS_PROC_OUT output = make_public_output(fixture, drops);

    check(DtsProcOutput(&fixture.context, 0, &output) == add_result(outcome),
          "drop path returns the first repost failure");
    check(fetch_calls == failure_index + 1 && add_calls == failure_index + 1 &&
          reposts_match(failure_index + 1),
          "drop repost failure stops before any extra fetch or duplicate repost");
    check(output.DropFrames == drops - failure_index,
          "failed repost does not consume its requested drop");
    check(copy_calls == 0 && callback_calls == 0 && fixture.context.ProcOutPending == 1,
          "failed drop repost retains ownership without copy or callback");
    const unsigned old_fetch_calls = fetch_calls;
    const unsigned old_add_calls = add_calls;
    check(DtsProcOutput(&fixture.context, 0, &output) == BC_STS_BUSY &&
          fetch_calls == old_fetch_calls && add_calls == old_add_calls,
          "ambiguous failed drop repost blocks a second fetch and repost");
}

static void test_replacement_fetch_failure(uint32_t device, BC_STATUS fetch_status)
{
    OutputContext fixture;
    prepare_scripted_output(fixture, device);
    fetch_replies.push_back(make_frame(0, 10, 4));
    fetch_replies.push_back(make_fetch_error(fetch_status));
    add_replies.push_back(make_add_reply(ADD_SUCCEEDS));
    BC_DTS_PROC_OUT output = make_public_output(fixture, 1);
    const BC_STATUS expected = fetch_status == BC_STS_TIMEOUT ? BC_STS_NO_DATA : fetch_status;

    check(DtsProcOutput(&fixture.context, 0, &output) == expected,
          "replacement fetch preserves existing timeout and error reporting");
    check(output.DropFrames == 0 && fetch_calls == 2 && add_calls == 1 && reposts_match(1),
          "replacement fetch occurs only after the dropped buffer was reposted");
    check(copy_calls == 0 && callback_calls == 0 && fixture.context.ProcOutPending == 0,
          "failed replacement fetch cannot copy the released dropped buffer");
}

static void test_delivery_status_precedence(BC_STATUS copy_status, AddOutcome outcome)
{
    OutputContext fixture;
    prepare_scripted_output(fixture, BC_PCI_DEVID_FLEA);
    fetch_replies.push_back(make_frame(0, 10, 4));
    add_replies.push_back(make_add_reply(outcome));
    mock_copy_status = copy_status;
    BC_DTS_PROC_OUT output = make_public_output(fixture, 0);
    const BC_STATUS expected = copy_status != BC_STS_SUCCESS ? copy_status : add_result(outcome);

    check(DtsProcOutput(&fixture.context, 0, &output) == expected,
          "delivered output preserves copy-error precedence over repost status");
    check(fetch_calls == 1 && add_calls == 1 && reposts_match(1) &&
          copy_calls == 1 && callback_calls == 1,
          "delivered output performs one callback, copy, and repost attempt");
    check(fixture.context.ProcOutPending == (outcome == ADD_SUCCEEDS ? 0 : 1),
          "delivered output retires pending ownership only after confirmed repost");
}

static void test_cleanup_errors(unsigned mode, unsigned errors)
{
    OutputContext fixture;
    DtsSetDecStat(true, fixture.context.ProcessID);
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
    const bool close_confirmed = mode != 0 && mock_close_status == BC_STS_SUCCESS;
    check(fixture.context.State ==
              (close_confirmed ? BC_DEC_STATE_CLOSE : BC_DEC_STATE_STOP),
          "only a confirmed channel close publishes CLOSED state");
    check(fixture.globals.g_bDecOpened == !close_confirmed &&
              fixture.globals.g_nProcID ==
                  (close_confirmed ? 0 : fixture.context.ProcessID),
          "shared decoder ownership follows confirmed channel close");
}


static void test_returned_nocopy_held(unsigned operation, bool release_first)
{
    OutputContext fixture;
    BC_IOCTL_DATA control_data = {};
    prepare_scripted_output(fixture, BC_PCI_DEVID_FLEA);
    fixture.context.pIoDataFreeHd = &control_data;
    fixture.context.OpMode = DTS_PLAYBACK_MODE;
    fixture.context.bMapOutBufDone = true;
    fixture.context.bMapOutBufDirty = true;
    DtsSetDecStat(true, fixture.context.ProcessID);
    DtsSetOPMode(1);
    fetch_replies.push_back(make_frame(0, 7, 1));
    BC_DTS_PROC_OUT output = make_public_output(fixture, 0);
    const BC_STATUS fetched = DtsProcOutputNoCopy(&fixture.context, 0, &output);
    check(fetched == BC_STS_SUCCESS && output.Ybuff == frame_storage[0] &&
          fixture.context.ProcOutPending == 1 && fetch_calls == 1 &&
          add_calls == 0 && fixture.context.outputPhase == DTS_OUTPUT_RETURNED,
          "actual successful NoCopy return transfers one host output owner");
    const uint8_t *const borrowed = output.Ybuff;
    if (release_first) {
        const BC_STATUS released = DtsReleaseOutputBuffs(&fixture.context, NULL, FALSE);
        check(released == BC_STS_SUCCESS && fixture.context.ProcOutPending == 0 &&
              add_calls == 1, "released control retires actual NoCopy owner once");
    }

    held_owner_case = true;
    unmap_calls = activate_calls = start_calls = interface_calls = 0;
    pending_at_unmap = pending_at_interface = pending_at_stop = false;
    const BC_STATUS stopped =
        operation == 0 ? DtsStopDecoder(&fixture.context) :
        operation == 1 ? DtsCloseDecoder(&fixture.context) :
        operation == 2 ? DtsDeviceClose(&fixture.context) :
                         DtsUnmapYUVBuffs(&fixture.context);
    const unsigned stop_polls = cancellation_sleeps;
    const uint32_t after_state = fixture.context.State;
    const bool after_quiescing = fixture.context.txQuiescing;
    const bool after_dirty = fixture.context.bMapOutBufDirty;
    const bool after_done = fixture.context.bMapOutBufDone;
    const bool after_pending = fixture.context.ProcOutPending;
    const unsigned after_stop = stop_calls;
    const unsigned after_close = close_calls;
    const unsigned after_unmap = unmap_calls;
    const unsigned after_interface = interface_calls;
    const bool pending_fw_stop = pending_at_stop;

    BC_STATUS restarted = BC_STS_DEC_NOT_OPEN;
    if (operation == 0 && !release_first) {
        /* Restart uses real admission/state logic. Its FW stubs only record
         * entry and this fixture deliberately disables the progressive ioctl. */
        fixture.context.VidParams.Progressive = FALSE;
        restarted = DtsStartDecoder(&fixture.context);
    }
    const unsigned adds_before_release = add_calls;
    BC_STATUS late_release = BC_STS_SUCCESS;
    if (!release_first) {
        late_release = DtsReleaseOutputBuffs(&fixture.context, NULL, FALSE);
        check(late_release == BC_STS_SUCCESS && fixture.context.ProcOutPending == 0 &&
              add_calls == adds_before_release + 1 &&
              reposted_buffers.back() == frame_storage[0],
              "late caller release reposts exact previously returned host buffer");
    }
    std::printf("held-nocopy operation=%u release_first=%d no_fetch_thread=1 "
                "fetched=%d stop=%d polls=%u pending=%d "
                "fw_stop=%u pending_at_fw_stop=%d fw_close=%u "
                "destructive_unmap=%u pending_at_unmap=%d interface_release=%u "
                "pending_at_interface=%d state=%u tx_quiescing=%d dirty=%d done=%d "
                "restart=%d activate=%u start=%u late_release=%d late_add=%u "
                "borrowed_pointer_unchanged=%d\n",
                operation, release_first, fetched, stopped, stop_polls,
                after_pending, after_stop, pending_fw_stop, after_close,
                after_unmap, pending_at_unmap, after_interface,
                pending_at_interface, after_state, after_quiescing,
                after_dirty, after_done, restarted, activate_calls, start_calls,
                late_release, add_calls - adds_before_release, output.Ybuff == borrowed);
    if (release_first) {
        check(stopped == BC_STS_SUCCESS && stop_polls == 0 &&
              after_stop == 1 && !pending_fw_stop && after_unmap == 1 &&
              !pending_at_unmap && !after_dirty && !after_done,
              "normal released control completes existing stop/unmap without cancellation");
    } else {
        check(after_pending && output.Ybuff == borrowed,
              "held lease survives until explicit caller release, not a fetch thread completion");
        if (operation != 3)
            check(stopped == BC_STS_TIMEOUT && stop_polls == (operation == 2 ? 63U : 21U),
                  "unreleased actual output reaches the bounded production cancellation timeout");
        else
            check(stopped != BC_STS_SUCCESS,
                  "destructive unmap must refuse a still-held output owner");
        check(after_stop == 0 && after_close == 0,
              "held-output refusal must not enter firmware stop/close");
        check(after_unmap == 0 && after_dirty && after_done,
              "held-output refusal must retain the registered capture set");
        check(after_interface == 0,
              "DeviceClose must not consume the interface while a returned owner is live");
        if (operation != 3)
            check(after_quiescing && after_state != BC_DEC_STATE_STOP &&
                  after_state != BC_DEC_STATE_CLOSE,
                  "timeout must not publish reusable STOP/CLOSE admission");
        check(fixture.context.Sig == LIB_CTX_SIG && fixture.context.DevHandle == 99 &&
              fixture.context.ProcessID == getpid() &&
              fixture.globals.g_bDecOpened &&
              fixture.globals.g_nProcID == fixture.context.ProcessID &&
              DtsGetOPMode() == 1,
              "held refusal retains live context metadata and shared decoder/mode ownership");
        if (operation == 0)
            check(restarted == BC_STS_BUSY && activate_calls == 0 && start_calls == 0,
                  "restart must not proceed while the returned NoCopy lease is still live");
    }

    if (!release_first) {
        const BC_STATUS retried =
            operation == 0 ? DtsStopDecoder(&fixture.context) :
            operation == 1 ? DtsCloseDecoder(&fixture.context) :
            operation == 2 ? DtsDeviceClose(&fixture.context) :
                             DtsUnmapYUVBuffs(&fixture.context);
        check(retried == BC_STS_SUCCESS && unmap_calls == 1 &&
              !fixture.context.bMapOutBufDone && !fixture.context.bMapOutBufDirty &&
              !fixture.context.ProcOutPending,
              "explicit caller release enables successful retry and exactly one destructive unmap");
        check(stop_calls == (operation == 3 ? 0U : 1U) &&
              close_calls == (operation == 1 || operation == 2 ? 1U : 0U) &&
              interface_calls == (operation == 2 ? 1U : 0U) &&
              !pending_at_unmap && !pending_at_interface,
              "retry enters stop/close/interface only after actual returned-owner release");
        if (operation == 1 || operation == 2)
            check(fixture.context.State == BC_DEC_STATE_CLOSE &&
                  !fixture.globals.g_bDecOpened && fixture.globals.g_nProcID == 0,
                  "successful close retry retires only this decoder's shared ownership");
        if (operation == 2)
            check(DtsGetOPMode() == 0, "successful DeviceClose retry clears playback mode");
        std::printf("released-retry operation=%u status=%d fw_stop=%u fw_close=%u "
                    "unmap=%u interface=%u pending=%d state=%u mode=%u\n",
                    operation, retried, stop_calls, close_calls, unmap_calls,
                    interface_calls, fixture.context.ProcOutPending,
                    fixture.context.State, DtsGetOPMode());
    }
    held_owner_case = false;
}

static void test_input_only_flush_keeps_owner()
{
    OutputContext fixture;
    BC_IOCTL_DATA control_data = {};
    prepare_scripted_output(fixture, BC_PCI_DEVID_FLEA);
    fixture.context.pIoDataFreeHd = &control_data;
    fixture.context.bMapOutBufDone = fixture.context.bMapOutBufDirty = true;
    fetch_replies.push_back(make_frame(0, 7, 1));
    BC_DTS_PROC_OUT output = make_public_output(fixture, 0);
    check(DtsProcOutputNoCopy(&fixture.context, 0, &output) == BC_STS_SUCCESS &&
          fixture.context.ProcOutPending, "input-only test acquires a real NoCopy owner");
    held_owner_case = true;
    unmap_calls = activate_calls = start_calls = interface_calls = 0;
    const BC_STATUS flushed = DtsFlushInput(&fixture.context, 3);
    const BC_STATUS restarted = DtsStartDecoder(&fixture.context);
    const BC_STATUS unmapped = DtsUnmapYUVBuffs(&fixture.context);
    check(flushed == BC_STS_SUCCESS && fixture.context.State == BC_DEC_STATE_FLUSH &&
          !fixture.context.txQuiescing && fixture.context.ProcOutPending,
          "input-only flush preserves pending output independently of TX quiescence");
    check(restarted == BC_STS_BUSY && unmapped == BC_STS_BUSY &&
          activate_calls == 0 && start_calls == 0 && unmap_calls == 0 &&
          cancellation_sleeps == 0 && fixture.context.bMapOutBufDirty &&
          fixture.context.bMapOutBufDone,
          "held output blocks Start and unmap even after op3 clears the TX barrier");
    check(DtsReleaseOutputBuffs(&fixture.context, NULL, FALSE) == BC_STS_SUCCESS &&
          !fixture.context.ProcOutPending && add_calls == 1,
          "op3 owner releases normally without destructive cleanup");
    check(DtsStopDecoder(&fixture.context) == BC_STS_SUCCESS &&
          stop_calls == 1 && unmap_calls == 1,
          "op3 cleanup succeeds after actual owner release");
    std::printf("input-only-op3 flush=%d held_start=%d held_unmap=%d release_add=%u "
                "retry_stop=%u unmap=%u\n", flushed, restarted, unmapped,
                add_calls, stop_calls, unmap_calls);
    held_owner_case = false;
}

static void test_cancel_wait_admission()
{
    OutputContext fixture;
    held_owner_case = true;
    fixture.context.State = BC_DEC_STATE_STOP;
    fixture.context.CancelWaiting = 1;
    activate_calls = start_calls = unmap_calls = interface_calls = 0;
    check(!fixture.context.ProcOutPending && !fixture.context.bMapOutBufDirty &&
          DtsStartDecoder(&fixture.context) == BC_STS_BUSY &&
          DtsUnmapYUVBuffs(&fixture.context) == BC_STS_BUSY &&
          activate_calls == 0 && start_calls == 0 && unmap_calls == 0,
          "cancellation-in-progress alone blocks Start and unmap, including clean map");
    fixture.context.CancelWaiting = 0;
    fixture.context.VidParams.Progressive = FALSE;
    check(DtsStartDecoder(&fixture.context) == BC_STS_SUCCESS &&
          activate_calls == 1 && start_calls == 1 &&
          DtsUnmapYUVBuffs(&fixture.context) == BC_STS_SUCCESS && unmap_calls == 0,
          "cleared cancellation admits normal Start and a clean no-op unmap");
    held_owner_case = false;
}

static void test_cancel_timeout_first_error()
{
    OutputContext fixture;
    BC_IOCTL_DATA control_data = {};
    prepare_scripted_output(fixture, BC_PCI_DEVID_FLEA);
    fixture.context.pIoDataFreeHd = &control_data;
    fixture.context.bMapOutBufDone = fixture.context.bMapOutBufDirty = true;
    fetch_replies.push_back(make_frame(0, 7, 1));
    BC_DTS_PROC_OUT output = make_public_output(fixture, 0);
    check(DtsProcOutputNoCopy(&fixture.context, 0, &output) == BC_STS_SUCCESS,
          "first-error test acquires returned NoCopy owner before stop");
    fixture.context.DevId = BC_PCI_DEVID_LINK;
    fixture.context.hw_paused = true;
    mock_pause_status = BC_STS_FW_CMD_ERR;
    pause_calls = unmap_calls = activate_calls = start_calls = interface_calls = 0;
    held_owner_case = true;
    const BC_STATUS status = DtsStopDecoder(&fixture.context);
    check(status == BC_STS_FW_CMD_ERR && pause_calls == 1 &&
          cancellation_sleeps == 21 && stop_calls == 0 && unmap_calls == 0 &&
          fixture.context.txQuiescing && fixture.context.ProcOutPending &&
          fixture.context.bMapOutBufDirty && fixture.context.bMapOutBufDone,
          "cancellation failure preserves an earlier firmware error and held ownership");
    mock_pause_status = BC_STS_SUCCESS;
    check(DtsReleaseOutputBuffs(&fixture.context, NULL, FALSE) == BC_STS_SUCCESS &&
          DtsStopDecoder(&fixture.context) == BC_STS_SUCCESS &&
          stop_calls == 1 && unmap_calls == 1,
          "first-error refusal remains retryable after actual owner release");
    std::printf("first-error-stop status=%d polls=%u pause=%u retry_stop=%u unmap=%u\n",
                status, cancellation_sleeps, pause_calls, stop_calls, unmap_calls);
    held_owner_case = false;
}


static void test_failed_internal_repost_teardown(bool packing, bool no_copy,
                                                 AddOutcome outcome, bool device_close)
{
    OutputContext fixture;
    BC_IOCTL_DATA control_data = {};
    prepare_scripted_output(fixture, BC_PCI_DEVID_FLEA);
    fixture.context.pIoDataFreeHd = &control_data;
    fixture.context.OpMode = DTS_PLAYBACK_MODE;
    fixture.context.bMapOutBufDone = fixture.context.bMapOutBufDirty = true;
    DtsSetDecStat(true, fixture.context.ProcessID);
    DtsSetOPMode(1);
    fetch_replies.push_back(make_frame(0, 7, 1));
    add_replies.push_back(make_add_reply(outcome));
    if (packing) {
        fixture.context.softwareUyvy = true;
        fixture.context.b422Mode = OUTPUT_MODE422_YUY2;
        /* Actual fetched metadata has separate UV extent and therefore
         * violates packed-YUY2 admission before any pixel conversion. */
    } else {
        cancel_after_fetch = true;
    }
    BC_DTS_PROC_OUT output = make_public_output(fixture, 0);
    const BC_STATUS produced = no_copy ?
        DtsProcOutputNoCopy(&fixture.context, 0, &output) :
        DtsProcOutput(&fixture.context, 0, &output);
    const BC_STATUS expected = packing ? add_result(outcome) : BC_STS_IO_USER_ABORT;
    check(produced == expected && produced != BC_STS_SUCCESS &&
          fetch_calls == 1 && add_calls == 1 && fixture.context.ProcOutPending == 1,
          "actual failed internal repost returns failure and retains teardown-only pending");
    check(fixture.context.outputPhase == DTS_OUTPUT_RETIRE_ONLY,
          "failed public output finalizes retirement-only after its last context access");
    if (packing)
        check(output.Ybuff == NULL && output.UVbuff == NULL,
              "failed packing cannot hand a successful caller-owned buffer to NoCopy/copy caller");
    const unsigned old_adds = add_calls;
    check(DtsReleaseOutputBuffs(&fixture.context, NULL, FALSE) == BC_STS_BUSY &&
          add_calls == old_adds && fixture.context.ProcOutPending &&
          fixture.context.outputPhase == DTS_OUTPUT_RETIRE_ONLY,
          "failed-output caller cannot surrender/repost an orphan as if it held a returned loan");
    /* Model only the cancellation owner's final flag reset, not lease
     * release or pending decrement. The rejected caller must NOT Release. */
    fixture.context.CancelWaiting = 0;
    held_owner_case = true;
    unmap_calls = activate_calls = start_calls = interface_calls = 0;
    pending_at_unmap = pending_at_interface = pending_at_stop = false;
    const BC_STATUS teardown = device_close ? DtsDeviceClose(&fixture.context) :
                                             DtsStopDecoder(&fixture.context);
    std::printf("orphan-repost packing=%d no_copy=%d outcome=%u device_close=%d "
                "output_status=%d cleanup=%d polls=%u fw_stop=%u fw_close=%u "
                "unmap=%u interface=%u pending=%d state=%u dirty=%d done=%d adds=%u "
                "caller_release=0\n", packing, no_copy, outcome, device_close,
                produced, teardown, cancellation_sleeps, stop_calls, close_calls,
                unmap_calls, interface_calls, fixture.context.ProcOutPending,
                fixture.context.State, fixture.context.bMapOutBufDirty,
                fixture.context.bMapOutBufDone, add_calls);
    check(teardown == BC_STS_SUCCESS && stop_calls == 1 && unmap_calls == 1 &&
          fixture.context.ProcOutPending == 0 && !fixture.context.bMapOutBufDirty &&
          !fixture.context.bMapOutBufDone,
          "failed internal repost must retire through acknowledged destructive cleanup");
    check(fixture.context.outputPhase == DTS_OUTPUT_IDLE,
          "only successful destructive unmap ACK returns residual phase to idle");
    check(add_calls == 1 && interface_calls == (device_close ? 1U : 0U) &&
          close_calls == (device_close ? 1U : 0U),
          "orphan teardown must not require/retry caller Release and must retain normal close path");
    held_owner_case = false;
}


static void initialize_phase_capture(OutputContext &fixture, BC_IOCTL_DATA &control)
{
    prepare_scripted_output(fixture, BC_PCI_DEVID_FLEA);
    fixture.context.pIoDataFreeHd = &control;
    fixture.context.OpMode = DTS_PLAYBACK_MODE;
    fixture.context.bMapOutBufDone = fixture.context.bMapOutBufDirty = true;
    DtsSetDecStat(true, fixture.context.ProcessID);
    DtsSetOPMode(1);
    unmap_calls = activate_calls = start_calls = interface_calls = 0;
    pending_at_unmap = pending_at_interface = pending_at_stop = false;
}

static void test_one_shot_release_failure(AddOutcome outcome)
{
    OutputContext fixture;
    BC_IOCTL_DATA control = {};
    initialize_phase_capture(fixture, control);
    fetch_replies.push_back(make_frame(0, 7, 1));
    BC_DTS_PROC_OUT output = make_public_output(fixture, 0);
    check(DtsProcOutputNoCopy(&fixture.context, 0, &output) == BC_STS_SUCCESS &&
          fixture.context.outputPhase == DTS_OUTPUT_RETURNED,
          "release-failure case obtains actual returned owner");
    check(DtsReleaseOutputBuffs(NULL, NULL, FALSE) == BC_STS_INV_ARG &&
          fixture.context.outputPhase == DTS_OUTPUT_RETURNED && add_calls == 0 &&
          DtsReleaseOutputBuffs(&fixture.context, NULL, TRUE) == BC_STS_SUCCESS &&
          fixture.context.outputPhase == DTS_OUTPUT_RETURNED && add_calls == 0,
          "invalid handle and legacy non-admitted no-op cannot surrender returned owner");
    add_replies.push_back(make_add_reply(outcome));
    const BC_STATUS released = DtsReleaseOutputBuffs(&fixture.context, NULL, FALSE);
    check(released == add_result(outcome) && add_calls == 1 &&
          fixture.context.ProcOutPending && fixture.context.outputPhase == DTS_OUTPUT_RETIRE_ONLY,
          "admitted one-shot release ends borrow and leaves only residual on failed ACK");
    /* No pointer access follows surrender, even though the output struct
     * still contains its old values. This is not a renewed caller borrow. */
    check(DtsReleaseOutputBuffs(&fixture.context, NULL, FALSE) == BC_STS_BUSY &&
          add_calls == 1 && fixture.context.outputPhase == DTS_OUTPUT_RETIRE_ONLY,
          "one-shot Release cannot retry an ambiguous ADD or consume another owner");
    held_owner_case = true;
    const BC_STATUS stopped = DtsStopDecoder(&fixture.context);
    check(stopped == BC_STS_SUCCESS && unmap_calls == 1 && stop_calls == 1 &&
          cancellation_sleeps == 0 && !fixture.context.ProcOutPending &&
          fixture.context.outputPhase == DTS_OUTPUT_IDLE,
          "failed admitted Release recovers only through real destructive unmap ACK");
    std::printf("one-shot-release outcome=%u status=%d cleanup=%d adds=%u unmap=%u phase=%u\n",
                outcome, released, stopped, add_calls, unmap_calls, fixture.context.outputPhase);
    held_owner_case = false;
}

static void test_unmap_failure_retains_orphan(bool syscall_failure, bool allocation_failure)
{
    OutputContext fixture;
    BC_IOCTL_DATA control = {};
    initialize_phase_capture(fixture, control);
    fixture.context.softwareUyvy = true;
    fixture.context.b422Mode = OUTPUT_MODE422_YUY2;
    fetch_replies.push_back(make_frame(0, 7, 1));
    add_replies.push_back(make_add_reply(ADD_STATUS_FAILURE));
    BC_DTS_PROC_OUT output = make_public_output(fixture, 0);
    check(DtsProcOutputNoCopy(&fixture.context, 0, &output) == BC_STS_IO_ERROR &&
          fixture.context.outputPhase == DTS_OUTPUT_RETIRE_ONLY,
          "unmap-failure case reaches actual internal repost orphan");
    mock_unmap_status = BC_STS_IO_ERROR;
    mock_unmap_syscall_failure = syscall_failure;
    if (allocation_failure)
        fixture.context.pIoDataFreeHd = NULL;
    held_owner_case = true;
    const BC_STATUS stopped = DtsStopDecoder(&fixture.context);
    const BC_STATUS expected = allocation_failure ? BC_STS_INSUFF_RES :
                               syscall_failure ? BC_STS_ERROR : BC_STS_IO_ERROR;
    check(stopped == expected && fixture.context.outputPhase == DTS_OUTPUT_RETIRE_ONLY &&
          fixture.context.ProcOutPending && fixture.context.bMapOutBufDirty &&
          fixture.context.txQuiescing && fixture.context.State == BC_DEC_STATE_FLUSH &&
          unmap_calls == (allocation_failure ? 0U : 1U),
          "failed or unavailable unmap ACK retains residual, registration and teardown barrier");
    check(DtsStartDecoder(&fixture.context) == BC_STS_BUSY &&
          DtsReleaseOutputBuffs(&fixture.context, NULL, FALSE) == BC_STS_BUSY &&
          add_calls == 1 && activate_calls == 0 && start_calls == 0,
          "ambiguous cleanup allows neither restart nor illegal caller Release");
    const BC_STATUS closed = DtsDeviceClose(&fixture.context);
    check(closed == BC_STS_BUSY && interface_calls == 0 &&
          fixture.context.outputPhase == DTS_OUTPUT_RETIRE_ONLY &&
          fixture.context.ProcOutPending && fixture.context.bMapOutBufDirty &&
          fixture.context.txQuiescing && fixture.context.State == BC_DEC_STATE_FLUSH &&
          fixture.context.Sig == LIB_CTX_SIG && fixture.context.DevHandle == 99 &&
          fixture.globals.g_bDecOpened && DtsGetOPMode() == 1,
          "DeviceClose cannot consume residual context or shared ownership without unmap ACK");
    mock_unmap_status = BC_STS_SUCCESS;
    mock_unmap_syscall_failure = false;
    if (allocation_failure)
        fixture.context.pIoDataFreeHd = &control;
    const unsigned before_ack = unmap_calls;
    const BC_STATUS recovered = DtsDeviceClose(&fixture.context);
    check(recovered == BC_STS_SUCCESS && unmap_calls == before_ack + 1 &&
          interface_calls == 1 && !pending_at_interface &&
          fixture.context.outputPhase == DTS_OUTPUT_IDLE &&
          !fixture.context.ProcOutPending && !fixture.context.bMapOutBufDirty &&
          fixture.context.State == BC_DEC_STATE_CLOSE && !fixture.globals.g_bDecOpened &&
          DtsGetOPMode() == 0 && add_calls == 1,
          "later actual unmap ACK alone discharges orphan before normal close/interface consumption");
    std::printf("unmap-orphan syscall=%d allocation=%d stop=%d close=%d recovery=%d "
                "unmaps=%u adds=%u phase=%u\n",
                syscall_failure, allocation_failure, stopped, closed, recovered,
                unmap_calls, add_calls, fixture.context.outputPhase);
    held_owner_case = false;
}

static void test_active_context_window(PhaseWindow window, bool no_copy)
{
    OutputContext fixture;
    BC_IOCTL_DATA control = {};
    initialize_phase_capture(fixture, control);
    fixture.context.DevId = BC_PCI_DEVID_LINK;
    fixture.context.bEOSCheck = true;
    mock_fetch_status = window == WINDOW_FINALIZER ? BC_STS_TIMEOUT : BC_STS_IO_ERROR;
    phase_window_reached = resume_phase_window = phase_actor_done = false;
    BC_STATUS produced = BC_STS_ERROR;
    std::thread actor([&] {
        phase_window = window;
        BC_DTS_PROC_OUT output = make_public_output(fixture, 0);
        produced = no_copy ? DtsProcOutputNoCopy(&fixture.context, 0, &output) :
                             DtsProcOutput(&fixture.context, 0, &output);
        std::lock_guard<std::mutex> lock(gate);
        phase_actor_done = true;
        changed.notify_all();
    });
    {
        std::unique_lock<std::mutex> lock(gate);
        if (!changed.wait_for(lock, std::chrono::seconds(2), [] { return phase_window_reached; }))
            std::abort();
    }
    check(fixture.context.outputPhase == DTS_OUTPUT_ACTIVE &&
          !fixture.context.ProcOutPending && fetch_calls == 1,
          "whole call remains ACTIVE after early DecPend0 and through outer last-context finalizer");
    held_owner_case = true;
    const BC_STATUS closed = DtsDeviceClose(&fixture.context);
    check(closed == BC_STS_TIMEOUT && cancellation_sleeps == 63 &&
          stop_calls == 0 && close_calls == 0 && unmap_calls == 0 &&
          interface_calls == 0 && fixture.context.outputPhase == DTS_OUTPUT_ACTIVE &&
          fixture.context.bMapOutBufDirty && fixture.globals.g_bDecOpened &&
          DtsGetOPMode() == 1,
          "DeviceClose cannot free ACTIVE call merely because its pending packet is already zero");
    check(DtsUnmapYUVBuffs(&fixture.context) == BC_STS_BUSY &&
          DtsReleaseOutputBuffs(&fixture.context, NULL, FALSE) == BC_STS_BUSY &&
          unmap_calls == 0 && add_calls == 0 && fixture.context.outputPhase == DTS_OUTPUT_ACTIVE,
          "wrong Release/unmap refusal cannot alter another still-active call lifetime");
    {
        std::lock_guard<std::mutex> lock(gate);
        resume_phase_window = true;
        changed.notify_all();
    }
    actor.join();
    const BC_STATUS expected = mock_fetch_status == BC_STS_TIMEOUT ? BC_STS_NO_DATA : mock_fetch_status;
    check(produced == expected && fixture.context.outputPhase == DTS_OUTPUT_IDLE &&
          !fixture.context.ProcOutPending,
          "failure path retires whole ACTIVE call only after its last context access");
    check(DtsDeviceClose(&fixture.context) == BC_STS_SUCCESS &&
          stop_calls == 1 && close_calls == 1 && unmap_calls == 1 && interface_calls == 1,
          "retry consumes the interface only after entire failed output call returns");
    std::printf("active-window kind=%u no_copy=%d close=%d output=%d polls=%u retry_interface=%u\n",
                window, no_copy, closed, produced, cancellation_sleeps, interface_calls);
    held_owner_case = false;
}

static void test_drop_refetch_exclusion(bool no_copy)
{
    OutputContext fixture;
    BC_IOCTL_DATA control = {};
    initialize_phase_capture(fixture, control);
    fetch_replies.push_back(make_frame(0, 7, 1));
    fetch_replies.push_back(make_frame(1, 8, 1));
    add_replies.push_back(make_add_reply(ADD_SUCCEEDS));
    add_replies.push_back(make_add_reply(ADD_SUCCEEDS));
    phase_window_reached = resume_phase_window = phase_actor_done = false;
    BC_STATUS produced = BC_STS_ERROR;
    BC_DTS_PROC_OUT output = make_public_output(fixture, 1);
    std::thread actor([&] {
        phase_window = WINDOW_EMPTY_CTX_LOCK;
        produced = no_copy ? DtsProcOutputNoCopy(&fixture.context, 0, &output) :
                             DtsProcOutput(&fixture.context, 0, &output);
        std::lock_guard<std::mutex> lock(gate);
        phase_actor_done = true;
        changed.notify_all();
    });
    {
        std::unique_lock<std::mutex> lock(gate);
        if (!changed.wait_for(lock, std::chrono::seconds(2), [] { return phase_window_reached; }))
            std::abort();
    }
    check(fixture.context.outputPhase == DTS_OUTPUT_ACTIVE && !fixture.context.ProcOutPending &&
          fetch_calls == 1 && add_calls == 1,
          "DropFrames gap keeps ACTIVE across acknowledged repost before internal refetch");
    BC_DTS_PROC_OUT competing = make_public_output(fixture, 0);
    check(DtsProcOutputNoCopy(&fixture.context, 0, &competing) == BC_STS_BUSY &&
          DtsProcOutput(&fixture.context, 0, &competing) == BC_STS_BUSY &&
          DtsReleaseOutputBuffs(&fixture.context, NULL, FALSE) == BC_STS_BUSY &&
          fetch_calls == 1 && add_calls == 1 && fixture.context.outputPhase == DTS_OUTPUT_ACTIVE,
          "competing public callers cannot steal the shared buffer between internal refetches");
    {
        std::lock_guard<std::mutex> lock(gate);
        resume_phase_window = true;
        changed.notify_all();
    }
    actor.join();
    check(produced == BC_STS_SUCCESS && fetch_calls == 2 && output.DropFrames == 0 &&
          output.PicInfo.picture_number == 8,
          "the same admitted call internally refetches and delivers only its undropped frame");
    if (no_copy)
        check(fixture.context.outputPhase == DTS_OUTPUT_RETURNED &&
              DtsReleaseOutputBuffs(&fixture.context, NULL, FALSE) == BC_STS_SUCCESS,
              "NoCopy refetch returns exactly one caller borrow that can be surrendered once");
    check(add_calls == 2 && fixture.context.outputPhase == DTS_OUTPUT_IDLE &&
          !fixture.context.ProcOutPending,
          "two internal frames have exactly one acknowledged repost each");
    std::printf("drop-refetch no_copy=%d output=%d fetch=%u add=%u phase=%u\n",
                no_copy, produced, fetch_calls, add_calls, fixture.context.outputPhase);
}

static void test_cancel_wins_publication(AddOutcome outcome)
{
    OutputContext fixture;
    BC_IOCTL_DATA control = {};
    initialize_phase_capture(fixture, control);
    fetch_replies.push_back(make_frame(0, 7, 1));
    add_replies.push_back(make_add_reply(outcome));
    phase_window_reached = resume_phase_window = phase_actor_done = false;
    BC_STATUS produced = BC_STS_ERROR;
    BC_DTS_PROC_OUT output = make_public_output(fixture, 0);
    std::thread actor([&] {
        phase_window = WINDOW_PUBLISH;
        produced = DtsProcOutputNoCopy(&fixture.context, 0, &output);
        std::lock_guard<std::mutex> lock(gate);
        phase_actor_done = true;
        changed.notify_all();
    });
    {
        std::unique_lock<std::mutex> lock(gate);
        if (!changed.wait_for(lock, std::chrono::seconds(2), [] { return phase_window_reached; }))
            std::abort();
    }
    check(fixture.context.outputPhase == DTS_OUTPUT_ACTIVE && fixture.context.ProcOutPending &&
          fetch_calls == 1 && add_calls == 0,
          "successful NoCopy work is not a RETURNED borrow before atomic final publication");
    check(DtsReleaseOutputBuffs(&fixture.context, NULL, FALSE) == BC_STS_BUSY &&
          add_calls == 0 && fixture.context.outputPhase == DTS_OUTPUT_ACTIVE,
          "prepublication Release cannot surrender another still-active output");
    held_owner_case = cancel_releases_publish = true;
    const BC_STATUS stopped = DtsStopDecoder(&fixture.context);
    actor.join();
    cancel_releases_publish = false;
    check(stopped == BC_STS_SUCCESS && produced == BC_STS_IO_USER_ABORT &&
          output.Ybuff == NULL && output.UVbuff == NULL &&
          cancellation_sleeps == 1 && add_calls == 1 && unmap_calls == 1 &&
          fixture.context.outputPhase == DTS_OUTPUT_IDLE && !fixture.context.ProcOutPending,
          "cancellation wins publication, reposts once and retires residual only through cleanup ACK");
    check(DtsReleaseOutputBuffs(&fixture.context, NULL, FALSE) == BC_STS_BUSY && add_calls == 1,
          "canceled late success cannot manufacture a caller-owned/releasable buffer");
    std::printf("cancel-publication outcome=%u output=%d stop=%d polls=%u adds=%u unmap=%u phase=%u\n",
                outcome, produced, stopped, cancellation_sleeps,
                add_calls, unmap_calls, fixture.context.outputPhase);
    held_owner_case = false;
}


static void test_release_active_last_access(BC_STATUS status, bool syscall_failure)
{
    OutputContext fixture;
    BC_IOCTL_DATA control = {};
    initialize_phase_capture(fixture, control);
    fetch_replies.push_back(make_frame(0, 7, 1));
    BC_DTS_PROC_OUT output = make_public_output(fixture, 0);
    check(DtsProcOutputNoCopy(&fixture.context, 0, &output) == BC_STS_SUCCESS &&
          fixture.context.outputPhase == DTS_OUTPUT_RETURNED,
          "release-race starts with a genuine returned NoCopy borrow");
    add_replies.push_back(AddReply{status, syscall_failure});
    phase_window_reached = resume_phase_window = phase_actor_done = false;
    BC_STATUS released = BC_STS_ERROR;
    std::thread actor([&] {
        phase_window = WINDOW_FINALIZER;
        released = DtsReleaseOutputBuffs(&fixture.context, NULL, FALSE);
        std::lock_guard<std::mutex> lock(gate);
        phase_actor_done = true;
        changed.notify_all();
    });
    {
        std::unique_lock<std::mutex> lock(gate);
        if (!changed.wait_for(lock, std::chrono::seconds(2), [] { return phase_window_reached; }))
            std::abort();
    }
    const bool acknowledged = !syscall_failure && status == BC_STS_SUCCESS;
    check(fixture.context.outputPhase == DTS_OUTPUT_ACTIVE &&
          fixture.context.ProcOutPending == !acknowledged && add_calls == 1,
          "admitted one-shot Release retains ACTIVE through last access even after ADD ACK clears pending");
    check(DtsReleaseOutputBuffs(&fixture.context, NULL, FALSE) == BC_STS_BUSY &&
          add_calls == 1 && fixture.context.outputPhase == DTS_OUTPUT_ACTIVE,
          "a rejected competing Release cannot surrender or retire the active owner's call");
    held_owner_case = true;
    const BC_STATUS closed = DtsDeviceClose(&fixture.context);
    check(closed == BC_STS_TIMEOUT && unmap_calls == 0 && interface_calls == 0 &&
          stop_calls == 0 && close_calls == 0 &&
          fixture.context.outputPhase == DTS_OUTPUT_ACTIVE,
          "DeviceClose cannot unmap/free while admitted Release still has context work");
    {
        std::lock_guard<std::mutex> lock(gate);
        resume_phase_window = true;
        changed.notify_all();
    }
    actor.join();
    const BC_STATUS expected = syscall_failure ? BC_STS_ERROR : status;
    check(released == expected && fixture.context.outputPhase ==
              (acknowledged ? DTS_OUTPUT_IDLE : DTS_OUTPUT_RETIRE_ONLY) &&
          fixture.context.ProcOutPending == !acknowledged,
          "Release publishes IDLE or retirement-only only after its last context access");
    check(DtsReleaseOutputBuffs(&fixture.context, NULL, FALSE) == BC_STS_BUSY && add_calls == 1,
          "borrow ends at admitted invocation for every driver status, including BUSY");
    check(DtsDeviceClose(&fixture.context) == BC_STS_SUCCESS && interface_calls == 1 &&
          unmap_calls == 1 && !fixture.context.ProcOutPending &&
          fixture.context.outputPhase == DTS_OUTPUT_IDLE && add_calls == 1,
          "release-race cleanup consumes only after whole-call completion and actual unmap ACK");
    std::printf("release-active status=%d syscall=%d output=%d close=%d polls=%u phase=%u\n",
                status, syscall_failure, released, closed,
                cancellation_sleeps, fixture.context.outputPhase);
    held_owner_case = false;
}

static void test_failed_nocopy_pointer_outputs(bool next_fetch_failure, AddOutcome outcome)
{
    OutputContext fixture;
    BC_IOCTL_DATA control = {};
    initialize_phase_capture(fixture, control);
    fetch_replies.push_back(make_frame(0, 7, 1));
    if (next_fetch_failure) {
        add_replies.push_back(make_add_reply(ADD_SUCCEEDS));
        fetch_replies.push_back(make_fetch_error(BC_STS_IO_ERROR));
    } else {
        add_replies.push_back(make_add_reply(outcome));
    }
    BC_DTS_PROC_OUT output = make_public_output(fixture, 1);
    output.Ybuff = output.UVbuff = frame_storage[7]; // Reused output record, not an admitted borrow.
    const BC_STATUS produced = DtsProcOutputNoCopy(&fixture.context, 0, &output);
    const BC_STATUS expected = next_fetch_failure ? BC_STS_IO_ERROR : add_result(outcome);
    check(produced == expected && produced != BC_STS_SUCCESS &&
          output.Ybuff == NULL && output.UVbuff == NULL &&
          fetch_calls == (next_fetch_failure ? 2U : 1U) && add_calls == 1,
          "failed NoCopy drop/refetch exposes no previously reposted or failed-repost pointer");
    check(fixture.context.outputPhase ==
              (next_fetch_failure ? DTS_OUTPUT_IDLE : DTS_OUTPUT_RETIRE_ONLY) &&
          fixture.context.ProcOutPending == !next_fetch_failure,
          "failed refetch leaves idle, failed repost leaves only library retirement");
    held_owner_case = true;
    check(DtsReleaseOutputBuffs(&fixture.context, NULL, FALSE) == BC_STS_BUSY && add_calls == 1,
          "failed NoCopy caller must not Release or retry an old buffer");
    check(DtsStopDecoder(&fixture.context) == BC_STS_SUCCESS && unmap_calls == 1 &&
          fixture.context.outputPhase == DTS_OUTPUT_IDLE && !fixture.context.ProcOutPending,
          "pointer rejection and residual cleanup remain independently correct");
    held_owner_case = false;
}

static void test_empty_publish_and_public_flush()
{
    OutputContext fixture;
    unmap_calls = 0;
    BC_DTS_PROC_OUT output = make_public_output(fixture, 0);
    output.Ybuff = output.UVbuff = frame_storage[7];
    check(DtsBeginOutputCall(&fixture.context) == BC_STS_SUCCESS,
          "empty-publication test admits a whole active call");
    check(DtsPublishOutput(&fixture.context, &output, BC_STS_SUCCESS) != BC_STS_SUCCESS &&
          output.Ybuff == NULL && output.UVbuff == NULL &&
          fixture.context.outputPhase == DTS_OUTPUT_IDLE,
          "SUCCESS without a pending fetched packet cannot manufacture a returned borrow");
    for (DTS_OUTPUT_PHASE phase : {DTS_OUTPUT_ACTIVE, DTS_OUTPUT_RETURNED, DTS_OUTPUT_RETIRE_ONLY}) {
        fixture.context.outputPhase = phase;
        fixture.context.ProcOutPending = phase != DTS_OUTPUT_ACTIVE;
        fixture.context.bMapOutBufDirty = fixture.context.bMapOutBufDone = false;
        check(DtsFlushRxCapture(&fixture.context, FALSE) == BC_STS_BUSY &&
              fixture.context.outputPhase == phase && unmap_calls == 0,
              "public false-flush clean-map path cannot bypass active/returned/residual lifetime");
    }
    fixture.context.outputPhase = DTS_OUTPUT_IDLE;
    fixture.context.ProcOutPending = 0;
    check(DtsFlushRxCapture(&fixture.context, FALSE) == BC_STS_SUCCESS && unmap_calls == 0,
          "original truly idle clean-map no-op is preserved");
}

static void test_device_close_returned_error_dominance()
{
    OutputContext fixture;
    BC_IOCTL_DATA control = {};
    prepare_scripted_output(fixture, BC_PCI_DEVID_FLEA);
    fixture.context.pIoDataFreeHd = &control;
    fixture.context.OpMode = DTS_PLAYBACK_MODE;
    fixture.context.bMapOutBufDone = fixture.context.bMapOutBufDirty = true;
    DtsSetDecStat(true, fixture.context.ProcessID);
    DtsSetOPMode(1);
    fetch_replies.push_back(make_frame(0, 7, 1));
    BC_DTS_PROC_OUT output = make_public_output(fixture, 0);
    check(DtsProcOutputNoCopy(&fixture.context, 0, &output) == BC_STS_SUCCESS &&
          fixture.context.outputPhase == DTS_OUTPUT_RETURNED,
          "DeviceClose error-dominance test acquires an actual returned borrow");
    fixture.context.DevId = BC_PCI_DEVID_LINK;
    fixture.context.hw_paused = true;
    mock_pause_status = BC_STS_FW_CMD_ERR;
    pause_calls = unmap_calls = interface_calls = 0;
    held_owner_case = true;
    const BC_STATUS closed = DtsDeviceClose(&fixture.context);
    check(closed == BC_STS_TIMEOUT && pause_calls == 1 && cancellation_sleeps == 63 &&
          stop_calls == 0 && close_calls == 0 && unmap_calls == 0 && interface_calls == 0,
          "final returned-owner cancellation timeout dominates an earlier pause error");
    check(fixture.context.Sig == LIB_CTX_SIG && fixture.context.DevHandle == 99 &&
          fixture.context.ProcessID == getpid() && fixture.context.ProcOutPending &&
          fixture.context.outputPhase == DTS_OUTPUT_RETURNED &&
          fixture.context.bMapOutBufDone && fixture.context.bMapOutBufDirty &&
          fixture.globals.g_bDecOpened &&
          fixture.globals.g_nProcID == fixture.context.ProcessID && DtsGetOPMode() == 1 &&
          output.Ybuff == frame_storage[0],
          "output-retention dominance preserves exact context, mapping and shared ownership");
    mock_pause_status = BC_STS_SUCCESS;
    check(DtsReleaseOutputBuffs(&fixture.context, NULL, FALSE) == BC_STS_SUCCESS &&
          add_calls == 1 && DtsDeviceClose(&fixture.context) == BC_STS_SUCCESS &&
          stop_calls == 1 && close_calls == 1 && unmap_calls == 1 && interface_calls == 1 &&
          !fixture.context.ProcOutPending && fixture.context.outputPhase == DTS_OUTPUT_IDLE &&
          !fixture.globals.g_bDecOpened && DtsGetOPMode() == 0,
          "returned owner releases once before a successful DeviceClose retry");
    std::printf("returned-error-dominance close=%d pause=%u polls=%u unmap=%u interface=%u\n",
                closed, pause_calls, cancellation_sleeps, unmap_calls, interface_calls);
    held_owner_case = false;
}

static void test_transient_unmap_ack_on_builtin_retry()
{
    OutputContext fixture;
    BC_IOCTL_DATA control = {};
    initialize_phase_capture(fixture, control);
    fixture.context.softwareUyvy = true;
    fixture.context.b422Mode = OUTPUT_MODE422_YUY2;
    fetch_replies.push_back(make_frame(0, 7, 1));
    add_replies.push_back(make_add_reply(ADD_STATUS_FAILURE));
    BC_DTS_PROC_OUT output = make_public_output(fixture, 0);
    check(DtsProcOutputNoCopy(&fixture.context, 0, &output) == BC_STS_IO_ERROR &&
          fixture.context.outputPhase == DTS_OUTPUT_RETIRE_ONLY,
          "transient retry starts from an actual library-only residual");
    held_owner_case = true;
    unmap_fail_once_at = 1;
    const BC_STATUS closed = DtsDeviceClose(&fixture.context);
    check(closed == BC_STS_IO_ERROR && stop_calls == 2 && unmap_calls == 2 &&
          close_calls == 1 && interface_calls == 1 && !pending_at_interface &&
          fixture.context.outputPhase == DTS_OUTPUT_IDLE && !fixture.context.ProcOutPending &&
          !fixture.context.bMapOutBufDirty && !fixture.globals.g_bDecOpened &&
          DtsGetOPMode() == 0,
          "built-in confirmed ACK retry consumes normally while preserving the first ordinary error");
    std::printf("transient-ACK DeviceClose=%d stop=%u unmap=%u interface=%u phase=%u\n",
                closed, stop_calls, unmap_calls, interface_calls, fixture.context.outputPhase);
    held_owner_case = false;
}

static void test_serialized_cancel_owner(AddOutcome outcome)
{
    OutputContext fixture;
    BC_IOCTL_DATA control = {};
    initialize_phase_capture(fixture, control);
    fetch_replies.push_back(make_frame(0, 7, 1));
    add_replies.push_back(make_add_reply(outcome));
    phase_window_reached = resume_phase_window = phase_actor_done = false;
    cancel_owner_reached = resume_cancel_owner = false;
    BC_STATUS produced = BC_STS_ERROR, canceled = BC_STS_ERROR;
    BC_DTS_PROC_OUT output = make_public_output(fixture, 0);
    std::thread actor([&] {
        phase_window = WINDOW_PUBLISH;
        produced = DtsProcOutputNoCopy(&fixture.context, 0, &output);
        std::lock_guard<std::mutex> lock(gate);
        phase_actor_done = true;
        changed.notify_all();
    });
    {
        std::unique_lock<std::mutex> lock(gate);
        if (!changed.wait_for(lock, std::chrono::seconds(2), [] { return phase_window_reached; }))
            std::abort();
    }
    held_owner_case = true;
    std::thread cancel_owner([&] {
        pause_cancel_owner = true;
        canceled = DtsCancelFetchOutInt(&fixture.context);
    });
    {
        std::unique_lock<std::mutex> lock(gate);
        if (!changed.wait_for(lock, std::chrono::seconds(2), [] { return cancel_owner_reached; }))
            std::abort();
    }
    check(DtsCancelFetchOutInt(&fixture.context) == BC_STS_BUSY &&
          fixture.context.CancelWaiting && fixture.context.outputPhase == DTS_OUTPUT_ACTIVE &&
          add_calls == 0,
          "second cancellation cannot clear another caller's owned flag");
    {
        std::lock_guard<std::mutex> lock(gate);
        resume_phase_window = true;
        changed.notify_all();
    }
    actor.join();
    check(produced == BC_STS_IO_USER_ABORT && output.Ybuff == NULL &&
          output.UVbuff == NULL && fixture.context.CancelWaiting &&
          fixture.context.outputPhase ==
              (outcome == ADD_SUCCEEDS ? DTS_OUTPUT_IDLE : DTS_OUTPUT_RETIRE_ONLY),
          "late publication still sees the first cancel owner's flag after second-call refusal");
    const BC_STATUS closed = DtsDeviceClose(&fixture.context);
    check(closed == BC_STS_BUSY && interface_calls == 0 && unmap_calls == 0 &&
          fixture.context.CancelWaiting && fixture.globals.g_bDecOpened && DtsGetOPMode() == 1,
          "DeviceClose retains context while first cancellation still accesses it, even in IDLE phase");
    {
        std::lock_guard<std::mutex> lock(gate);
        resume_cancel_owner = true;
        changed.notify_all();
    }
    cancel_owner.join();
    check(canceled == BC_STS_SUCCESS && !fixture.context.CancelWaiting &&
          cancellation_sleeps == 1,
          "only serialized cancel owner clears its flag at its final context access");
    check(DtsDeviceClose(&fixture.context) == BC_STS_SUCCESS &&
          interface_calls == 1 && unmap_calls == 1 && add_calls == 1 &&
          fixture.context.outputPhase == DTS_OUTPUT_IDLE && !fixture.context.ProcOutPending,
          "retry consumes only after cancellation owner and any residual have retired");
    std::printf("serialized-cancel outcome=%u output=%d first=%d close_busy=%d polls=%u interface=%u\n",
                outcome, produced, canceled, closed, cancellation_sleeps, interface_calls);
    held_owner_case = false;
}

int main()
{
    const rlimit no_core = {0, 0};
    if (setrlimit(RLIMIT_CORE, &no_core) != 0)
        return 2;
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
    for (AddOutcome outcome : {ADD_SUCCEEDS, ADD_STATUS_FAILURE, ADD_SYSCALL_FAILURE})
        test_repeated_picture_repost(outcome);
    test_flea_repeat_seed_control();
    for (uint32_t device : {static_cast<uint32_t>(BC_PCI_DEVID_LINK),
                            static_cast<uint32_t>(BC_PCI_DEVID_FLEA)}) {
        for (bool single_threaded : {false, true}) {
            for (uint8_t drops : {static_cast<uint8_t>(1), static_cast<uint8_t>(2)})
                test_successful_drops(device, single_threaded, drops);
            for (unsigned failure_index : {0U, 1U}) {
                for (AddOutcome outcome : {ADD_STATUS_FAILURE, ADD_SYSCALL_FAILURE})
                    test_failed_drop_repost(device, single_threaded, failure_index, outcome);
            }
        }
        test_replacement_fetch_failure(device, BC_STS_IO_ERROR);
        test_replacement_fetch_failure(device, BC_STS_TIMEOUT);
    }
    for (BC_STATUS copy_status : {BC_STS_SUCCESS, BC_STS_IO_ERROR}) {
        for (AddOutcome outcome : {ADD_SUCCEEDS, ADD_STATUS_FAILURE, ADD_SYSCALL_FAILURE})
            test_delivery_status_precedence(copy_status, outcome);
    }
    for (unsigned mode : {0U, 1U, 2U, 4U})
        for (unsigned errors = 0; errors < 8; ++errors)
            test_cleanup_errors(mode, errors);
    std::printf("original-flush-regression failures=%u\n", failures);
    test_returned_nocopy_held(0, true);
    for (unsigned operation = 0; operation < 4; ++operation)
        test_returned_nocopy_held(operation, false);
    test_input_only_flush_keeps_owner();
    test_cancel_wait_admission();
    test_cancel_timeout_first_error();
    for (bool packing : {false, true})
        for (bool no_copy : {false, true})
            for (AddOutcome outcome : {ADD_STATUS_FAILURE, ADD_SYSCALL_FAILURE})
                for (bool device_close : {false, true})
                    test_failed_internal_repost_teardown(packing, no_copy, outcome, device_close);
    for (AddOutcome outcome : {ADD_STATUS_FAILURE, ADD_SYSCALL_FAILURE})
        test_one_shot_release_failure(outcome);
    test_unmap_failure_retains_orphan(false, false);
    test_unmap_failure_retains_orphan(true, false);
    test_unmap_failure_retains_orphan(false, true);
    for (bool no_copy : {false, true}) {
        test_active_context_window(WINDOW_EMPTY_CTX_LOCK, no_copy);
        test_active_context_window(WINDOW_FINALIZER, no_copy);
        test_drop_refetch_exclusion(no_copy);
    }
    for (AddOutcome outcome : {ADD_SUCCEEDS, ADD_STATUS_FAILURE, ADD_SYSCALL_FAILURE})
        test_cancel_wins_publication(outcome);
    test_failed_nocopy_pointer_outputs(true, ADD_SUCCEEDS);
    test_failed_nocopy_pointer_outputs(false, ADD_STATUS_FAILURE);
    test_failed_nocopy_pointer_outputs(false, ADD_SYSCALL_FAILURE);
    test_empty_publish_and_public_flush();
    test_device_close_returned_error_dominance();
    test_transient_unmap_ack_on_builtin_retry();
    for (BC_STATUS status : {BC_STS_SUCCESS, BC_STS_IO_ERROR, BC_STS_BUSY})
        test_release_active_last_access(status, false);
    test_release_active_last_access(BC_STS_SUCCESS, true);
    for (AddOutcome outcome : {ADD_SUCCEEDS, ADD_STATUS_FAILURE, ADD_SYSCALL_FAILURE})
        test_serialized_cancel_owner(outcome);
    std::printf("held-nocopy-regression checks=%u failures=%u\n", checks, failures);
    if (failures)
        return 1;
    std::puts("PASS: production output ownership and flush ordering checks");
    return 0;
}
