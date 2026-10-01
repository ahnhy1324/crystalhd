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
    if (failures)
        return 1;
    std::puts("PASS: production output ownership and flush ordering checks");
    return 0;
}
