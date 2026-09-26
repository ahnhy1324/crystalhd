/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Actual TX worker, ring, FlushInput and StopDecoder with fake firmware/DMA.
 * The worker is paused after the real pop unlock or inside the DMA call.
 * Cancellation polling releases it deterministically, without scheduler sleeps.
 * If the public API returns without waiting, the test then releases it: old
 * DMA demonstrably follows the successful return. A cancellation flush is
 * allowed, but final flush/stop must follow retirement. No device setup is used.
 */
#include <chrono>
#include <cerrno>
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
BC_STATUS DtsAlignSendData(HANDLE, uint8_t *, uint32_t, uint64_t, BOOL);
static DTS_LIB_CONTEXT *observed;
static std::mutex gate;
static std::condition_variable changed;
static thread_local bool worker_thread, cancelling_thread;
static unsigned failures;
static bool stop_at_packet_length;
static unsigned packetizer_stops;
static bool stop_at_ring_wait;
static unsigned ring_wait_stops;
enum class Point { Popped, Dma };
static struct {
    Point point;
    bool timeout, blocked, release, done, packet_ok, allow_restart;
    unsigned polls, status_calls, dma_calls, flush_calls, stop_calls, close_calls;
    unsigned sequence, dma_retired, last_flush, last_stop;
    unsigned firmware_mode;
    unsigned control_calls, release_calls, join_calls;
    bool stop_while_owned, close_while_owned, release_while_owned, fail_first_flush;
} run;
static const uint8_t packet[] = {0x00, 0x00, 0x01, 0xe0, 0x42, 0x19, 0xa5, 0x7c};

static void check(bool condition, const char *message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

/* This cross-TU helper is called after AlignSendData checked START, but before
 * it enqueues the completed PES packet. Stop on another thread at that exact
 * seam, forwarding the real byte swap and all remaining production framing.
 * The mangled spelling is invariant on both supported 32/64-bit ABIs (ushort).
 */
extern "C" uint16_t __real__Z9WORD_SWAPt(uint16_t);
extern "C" uint16_t __wrap__Z9WORD_SWAPt(uint16_t value)
{
    const uint16_t result = __real__Z9WORD_SWAPt(value);
    if (stop_at_packet_length) {
        stop_at_packet_length = false;
        BC_STATUS status = BC_STS_ERROR;
        std::thread stopper([&] { status = DtsStopDecoder(observed); });
        stopper.join();
        check(status == BC_STS_SUCCESS, "packetizer seam completes a real cross-thread StopDecoder");
        ++packetizer_stops;
    }
    return result;
}

template<class Predicate>
static void await(std::unique_lock<std::mutex> &lock, Predicate predicate)
{
    if (!changed.wait_for(lock, std::chrono::seconds(3), predicate)) {
        std::fputs("TX flush fixture handshake timed out\n", stderr);
        std::abort();
    }
}

static void pause_worker()
{
    std::unique_lock<std::mutex> lock(gate);
    run.blocked = true;
    changed.notify_all();
    await(lock, [] { return run.release; });
}

extern "C" int __real_pthread_mutex_unlock(pthread_mutex_t *mutex);
extern "C" int __wrap_pthread_mutex_unlock(pthread_mutex_t *mutex)
{
    const int result = __real_pthread_mutex_unlock(mutex);
    if (worker_thread && observed && mutex == &observed->circBuf.flushLock &&
        run.point == Point::Popped && !run.blocked)
        pause_worker(); // The production pop really completed before this hook.
    return result;
}

extern "C" int __wrap_usleep(useconds_t microseconds)
{
    if (stop_at_ring_wait && microseconds == 5000) {
        stop_at_ring_wait = false;
        bool context_locked = false;
        BC_STATUS status = BC_STS_ERROR;
        std::thread stopper([&] {
            const int locked = pthread_mutex_trylock(&observed->thLock);
            context_locked = locked == EBUSY;
            if (locked == 0) {
                pthread_mutex_unlock(&observed->thLock);
                status = DtsStopDecoder(observed);
            } else if (!context_locked)
                std::abort();
        });
        stopper.join();
        check(!context_locked, "full-ring SendData waits without holding the context mutex");
        // Recover a deliberately failing implementation without hanging CI:
        // the fixture context mutex is recursive and there is no TX owner.
        if (context_locked)
            status = DtsStopDecoder(observed);
        check(status == BC_STS_SUCCESS, "full-ring wait allows the real StopDecoder to complete");
        ++ring_wait_stops;
    }
    if (cancelling_thread && microseconds == 10000) {
        std::unique_lock<std::mutex> lock(gate);
        if (++run.polls > 1000)
            std::abort();
        if (!run.timeout) {
            run.release = true;
            changed.notify_all();
            await(lock, [] { return run.done; });
        }
    }
    // Retain the production loop counter, not its wall-clock test cost.
    return 0;
}

extern "C" int __wrap_ioctl(int, unsigned long, ...)
{
    std::fputs("unexpected ioctl in hardware-free TX flush test\n", stderr);
    std::abort();
}

extern "C" int __real_pthread_join(pthread_t, void **);
extern "C" int __wrap_pthread_join(pthread_t thread, void **result)
{
    if (!cancelling_thread || !observed || thread != observed->htxThread)
        return __real_pthread_join(thread, result);
    std::unique_lock<std::mutex> lock(gate);
    ++run.join_calls;
    run.release = true;
    changed.notify_all();
    await(lock, [] { return run.done; });
    // The fixture's std::thread still performs the real OS join once the
    // public close returns. This hook validates close ordering without making
    // std::thread double-join a handle that production would have consumed.
    return 0;
}

BC_STATUS DtsGetDrvStat(HANDLE device, BC_DTS_STATS *status)
{
    if (!worker_thread || device != observed)
        std::abort();
    std::memset(status, 0, sizeof(*status));
    status->DrvcpbEmptySize = sizeof(packet);
    if (++run.status_calls == 2) {
        // Only the worker writes this production exit flag; no test data race.
        observed->txThreadExit = true;
        return BC_STS_IO_USER_ABORT;
    }
    if (run.status_calls > 2)
        std::abort();
    return BC_STS_SUCCESS;
}

BC_STATUS DtsTxDmaText(HANDLE device, uint8_t *bytes, uint32_t size,
                      uint32_t *, uint8_t)
{
    if (!worker_thread || device != observed)
        std::abort();
    ++run.dma_calls;
    run.packet_ok = size == sizeof(packet) && !std::memcmp(bytes, packet, size);
    if (run.point == Point::Dma)
        pause_worker();
    {
        std::lock_guard<std::mutex> lock(gate);
        run.dma_retired = ++run.sequence;
    }
    return BC_STS_SUCCESS;
}

static void firmware_boundary(unsigned kind)
{
    std::unique_lock<std::mutex> lock(gate);
    // The reset effect occurs here, BEFORE a delayed old DMA can complete.
    // A later second flush is permitted and must repair that ordering.
    if (kind == 0) {
        ++run.flush_calls;
        run.last_flush = ++run.sequence;
    } else if (kind == 1) {
        ++run.stop_calls;
        run.last_stop = ++run.sequence;
        run.stop_while_owned |= !run.done;
    } else {
        ++run.close_calls;
        ++run.sequence;
        run.close_while_owned |= !run.done;
    }
}

BC_STATUS DtsFWDecFlushChannel(HANDLE device, uint32_t mode)
{
    if (device != observed || mode != run.firmware_mode)
        std::abort();
    firmware_boundary(0);
    return run.fail_first_flush && run.flush_calls == 1 ? BC_STS_IO_ERROR : BC_STS_SUCCESS;
}
BC_STATUS DtsFWStopVideo(HANDLE, uint32_t, bool)
{ firmware_boundary(1); return BC_STS_SUCCESS; }
BC_STATUS DtsFWCloseChannel(HANDLE, uint32_t)
{ firmware_boundary(2); return BC_STS_SUCCESS; }
BC_STATUS DtsFWPauseVideo(HANDLE, uint32_t) { std::abort(); }
void DumpInputSampleToFile(uint8_t *, uint32_t) {}
/* Startup must not reach firmware while quiescing. A separate stopped-ring
 * check deliberately allows the actual StartDecoder to restart the worker.
 */
static BC_STATUS control_call()
{
    ++run.control_calls;
    return run.allow_restart ? BC_STS_SUCCESS : BC_STS_ERROR;
}
BC_STATUS DtsFWActivateDecoder(HANDLE) { return control_call(); }
BC_STATUS DtsFWStartVideo(HANDLE, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t)
{ return control_call(); }
BC_STATUS DtsFWOpenChannel(HANDLE, uint32_t, uint32_t) { return control_call(); }
BC_STATUS DtsFWSetVideoInput(HANDLE) { return control_call(); }
BC_STATUS DtsSetProgressive(HANDLE, uint32_t) { return control_call(); }
BC_STATUS DtsSetVideoClock(HANDLE, uint32_t) { return control_call(); }
BC_STATUS DtsSetTSMode(HANDLE, uint32_t) { return control_call(); }
BC_STATUS DtsPushFwToFlea(HANDLE, char *) { return control_call(); }
BC_STATUS DtsPushAuthFwToLink(HANDLE, char *) { return control_call(); }
BC_STATUS DtsFWInitialize(HANDLE, uint32_t) { return control_call(); }
/* Only unexecuted pixel-copy/suspend-recovery paths need these symbols. */
BC_STATUS DtsCopyRawDataToOutBuff(DTS_LIB_CONTEXT *, BC_DTS_PROC_OUT *, BC_DTS_PROC_OUT *)
{ std::abort(); }
BC_STATUS DtsCopyNV12ToYV12(DTS_LIB_CONTEXT *, BC_DTS_PROC_OUT *, BC_DTS_PROC_OUT *)
{ std::abort(); }
BC_STATUS DtsCopyNV12(DTS_LIB_CONTEXT *, BC_DTS_PROC_OUT *, BC_DTS_PROC_OUT *)
{ std::abort(); }
BC_STATUS DtsCopyFormat(DTS_LIB_CONTEXT *, BC_DTS_PROC_OUT *, BC_DTS_PROC_OUT *)
{ std::abort(); }
extern "C" BC_STATUS __wrap_DtsSetupHardware(HANDLE, BOOL) { std::abort(); }
extern "C" BC_STATUS __wrap_DtsOpenDecoder(HANDLE, uint32_t) { std::abort(); }
extern "C" BC_STATUS __real_DtsStartDecoder(HANDLE);
extern "C" BC_STATUS __wrap_DtsStartDecoder(HANDLE device)
{
    if (worker_thread)
        std::abort();
    return __real_DtsStartDecoder(device);
}
extern "C" BC_STATUS __wrap_DtsStartCapture(HANDLE) { std::abort(); }
extern "C" BC_STATUS __wrap_DtsReleaseInterface(DTS_LIB_CONTEXT *)
{
    // Record a bad public-close destruction attempt without freeing a live
    // worker's stack fixture or invoking any real device/shared-memory cleanup.
    ++run.release_calls;
    run.release_while_owned |= !run.done;
    return BC_STS_SUCCESS;
}
BC_STATUS DtsSetCoreClock(HANDLE, uint32_t) { return control_call(); }

static BC_STATUS cancel(DTS_LIB_CONTEXT *context, bool stop, unsigned operation = 2)
{
    cancelling_thread = true;
    const BC_STATUS result = stop ? DtsStopDecoder(context) : DtsFlushInput(context, operation);
    cancelling_thread = false;
    return result;
}

static void check_blocked_controls(DTS_LIB_CONTEXT *context)
{
    const unsigned controls = run.control_calls;
    uint8_t input[sizeof(packet) + 4] = {};
    std::memcpy(input, packet, sizeof(packet));
    check(DtsProcInput(context, input, sizeof(packet), 100000, false) == BC_STS_BUSY,
          "new input cannot bypass a timed-out TX quiesce");
    check(DtsStartDecoder(context) == BC_STS_BUSY,
          "StartDecoder cannot bypass a timed-out TX quiesce");
    check(DtsResumeDecoder(context) == BC_STS_BUSY,
          "ResumeDecoder cannot bypass a timed-out TX quiesce");
    check(DtsPauseDecoder(context) == BC_STS_BUSY,
          "PauseDecoder cannot bypass a timed-out TX quiesce");
    check(run.control_calls == controls && context->State == BC_DEC_STATE_FLUSH,
          "rejected control calls do not reopen firmware or clear the cancellation state");
}

static void check_timeout_admission(DTS_LIB_CONTEXT *context)
{
    const unsigned stops = run.stop_calls;
    const unsigned closes = run.close_calls;
    check_blocked_controls(context);
    cancelling_thread = true;
    run.polls = 0;
    check(DtsCloseDecoder(context) == BC_STS_TIMEOUT,
          "CloseDecoder retains its live context after TX timeout");
    cancelling_thread = false;
    check(run.release_calls == 0, "decoder-close timeout does not release the interface");
    check(run.stop_calls == stops && run.close_calls == closes &&
          context->State == BC_DEC_STATE_FLUSH,
          "rejected lifecycle calls preserve the pending TX cancellation state");
}

static void test_case(Point point, bool stop, bool timeout, bool queued_tail = false,
                      bool fail_first_flush = false, unsigned operation = 2)
{
    run = {};
    run.point = point;
    run.timeout = timeout;
    run.fail_first_flush = fail_first_flush;
    run.firmware_mode = stop || operation == 4 ? 2 : operation;
    bc_dil_glob_s globals = {};
    bc_dil_glob_ptr = &globals;
    DTS_LIB_CONTEXT context = {};
    observed = &context;
    context.Sig = LIB_CTX_SIG;
    context.State = BC_DEC_STATE_START;
    context.DevId = BC_PCI_DEVID_FLEA;
    context.ProcessID = getpid();
    context.VidParams.StreamType = BC_STREAM_TYPE_PES;
    if (operation == 3) {
        context.alignBuf = static_cast<uint8_t *>(std::malloc(ALIGN_BUF_SIZE));
        if (!context.alignBuf)
            std::abort();
    }
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&context.thLock, &attr);
    pthread_mutexattr_destroy(&attr);
    if (txBufInit(&context.circBuf, CIRC_TX_BUF_SIZE) != BC_STS_SUCCESS ||
        txBufPush(&context.circBuf, const_cast<uint8_t *>(packet), sizeof(packet)) != BC_STS_SUCCESS)
        std::abort();
    if (queued_tail && txBufPush(&context.circBuf, const_cast<uint8_t *>(packet), sizeof(packet)) != BC_STS_SUCCESS)
        std::abort();
    std::thread worker([&] {
        worker_thread = true;
        txThreadProc(&context);
        worker_thread = false;
        std::lock_guard<std::mutex> lock(gate);
        run.done = true;
        changed.notify_all();
    });
    {
        std::unique_lock<std::mutex> lock(gate);
        await(lock, [] { return run.blocked; });
        check(context.circBuf.busySize == (queued_tail ? sizeof(packet) : 0),
              "fixture really removed the old bytes from the production ring");
    }
    const BC_STATUS result = cancel(&context, stop, operation);
    const unsigned first_polls = run.polls;
    const unsigned first_flushes = run.flush_calls;
    const unsigned first_stops = run.stop_calls;
    bool retired_before_return;
    {
        std::lock_guard<std::mutex> lock(gate);
        retired_before_return = run.done;
    }
    if (timeout) {
        check(result == BC_STS_TIMEOUT, "unretired TX makes cancellation time out honestly");
        check(run.polls > 0 && run.polls <= 500, "TX wait uses the finite production retry budget");
        check(context.State == BC_DEC_STATE_FLUSH, "timeout retains closed input/output admission");
        check(run.stop_calls == 0 && run.close_calls == 0,
              "timeout cannot stop/close firmware while old TX remains owned");
        check(context.circBuf.busySize == (queued_tail ? sizeof(packet) : 0),
              "timed-out cancellation retains ring storage until the TX owner retires");
        if (operation == 3)
            check_blocked_controls(&context);
        else
            check_timeout_admission(&context);
        {
            std::lock_guard<std::mutex> lock(gate);
            run.release = true;
            changed.notify_all();
        }
        worker.join();
        run.timeout = false;
        check_blocked_controls(&context);
        check(cancel(&context, stop, operation) == BC_STS_SUCCESS,
              "same public operation can be retried after old TX really retires");
    } else {
        check(retired_before_return, "successful cancellation cannot return with old TX still owned");
        {
            std::lock_guard<std::mutex> lock(gate);
            run.release = true;
            changed.notify_all();
        }
        worker.join();
        check(result == (fail_first_flush ? BC_STS_IO_ERROR : BC_STS_SUCCESS),
              "cancellation preserves the first firmware error through later successful cleanup");
    }
    check((point == Point::Popped && run.dma_calls == 0) ||
          (run.dma_calls == 1 && run.packet_ok),
          "old packet is either discarded before DMA or transferred intact exactly once");
    check(!run.stop_while_owned && !run.close_while_owned,
          "firmware stop/close follow retirement of the old TX owner");
    if (operation == 3)
        check(run.flush_calls == 0 && run.stop_calls == 0 && run.close_calls == 0,
              "Op3 quiesces only host input and never issues firmware flush/stop/close");
    else
        check(stop ? run.last_stop > run.dma_retired : run.last_flush > run.dma_retired,
              "final reset follows old DMA, including bytes popped before cancellation");
    check(context.State == (stop ? BC_DEC_STATE_STOP :
                           operation == 3 ? BC_DEC_STATE_FLUSH : BC_DEC_STATE_CLOSE),
          "successful operation reaches its documented stopped/closed state");
    check(context.circBuf.busySize == 0, "successful stop/flush discards all remaining queued input");
    if (operation == 3) {
        check(DtsResumeDecoder(&context) == BC_STS_SUCCESS && context.State == BC_DEC_STATE_START,
              "Resume can restore START after a successful host-only flush");
        check(DtsFlushInput(&context, 3) == BC_STS_SUCCESS && context.State == BC_DEC_STATE_FLUSH,
              "idle Op3 can be repeated without closing the decoder");
        uint8_t input[sizeof(packet) + 4] = {};
        std::memcpy(input, packet, sizeof(packet));
        check(DtsProcInput(&context, input, sizeof(packet), 100000, false) == BC_STS_SUCCESS &&
              context.State == BC_DEC_STATE_START && context.circBuf.busySize > 0,
              "new input independently restores START and queues data after successful Op3");
        check(run.flush_calls == 0 && run.stop_calls == 0 && run.close_calls == 0 && run.control_calls == 0,
              "Op3 restart remains firmware-free");
    }
    if (stop && queued_tail) {
        const unsigned dma_before_restart = run.dma_calls;
        run.allow_restart = true;
        check(DtsStartDecoder(&context) == BC_STS_SUCCESS,
              "actual StartDecoder permits a restart after successful stop");
        run.status_calls = 0;
        context.txThreadExit = false;
        std::thread restarted([&] {
            worker_thread = true;
            txThreadProc(&context);
            worker_thread = false;
        });
        restarted.join();
        check(run.dma_calls == dma_before_restart,
              "restarted real TX worker cannot transmit any pre-stop queued tail");
    }
    std::printf("%s %s timeout=%d queued_tail=%d initial_error=%d first_status=%d polls=%u first_flushes=%u "
                "first_stops=%u retired_on_return=%d dma_at=%u final_flush_at=%u final_stop_at=%u\n",
                stop ? "stop" : operation == 1 ? "flush1" : operation == 3 ? "flush3" :
                operation == 4 ? "flush4" : "flush2", point == Point::Popped ? "popped" : "dma",
                timeout, queued_tail, fail_first_flush, result, first_polls, first_flushes, first_stops, retired_before_return,
                run.dma_retired, run.last_flush, run.last_stop);
    txBufFree(&context.circBuf);
    std::free(context.alignBuf);
    pthread_mutex_destroy(&context.thLock);
    observed = nullptr;
    bc_dil_glob_ptr = nullptr;
}

static void test_device_close(Point point)
{
    run = {};
    run.point = point;
    run.timeout = true;
    run.firmware_mode = 2;
    bc_dil_glob_s globals = {};
    bc_dil_glob_ptr = &globals;
    DTS_LIB_CONTEXT context = {};
    observed = &context;
    context.Sig = LIB_CTX_SIG;
    context.State = BC_DEC_STATE_START;
    context.DevId = BC_PCI_DEVID_FLEA;
    context.ProcessID = getpid();
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&context.thLock, &attr);
    pthread_mutexattr_destroy(&attr);
    if (txBufInit(&context.circBuf, CIRC_TX_BUF_SIZE) != BC_STS_SUCCESS ||
        txBufPush(&context.circBuf, const_cast<uint8_t *>(packet), sizeof(packet)) != BC_STS_SUCCESS)
        std::abort();
    std::thread worker([&] {
        worker_thread = true;
        txThreadProc(&context);
        worker_thread = false;
        std::lock_guard<std::mutex> lock(gate);
        run.done = true;
        changed.notify_all();
    });
    context.htxThread = worker.native_handle();
    {
        std::unique_lock<std::mutex> lock(gate);
        await(lock, [] { return run.blocked; });
    }
    cancelling_thread = true;
    const BC_STATUS result = DtsDeviceClose(&context);
    cancelling_thread = false;
    {
        std::lock_guard<std::mutex> lock(gate);
        run.release = true; // Also lets the unchanged baseline exit safely.
        changed.notify_all();
    }
    worker.join();
    check(result == BC_STS_TIMEOUT, "consuming DeviceClose preserves its initial TX timeout error");
    check(run.join_calls == 1 && context.htxThread == 0,
          "DeviceClose joins the TX owner before retrying decoder teardown");
    check(run.release_calls == 1 && !run.release_while_owned,
          "consuming DeviceClose releases the interface only after TX retirement");
    check(!run.stop_while_owned && !run.close_while_owned &&
          run.last_stop > run.dma_retired && context.State == BC_DEC_STATE_CLOSE,
          "DeviceClose stops and closes only after its joined worker can no longer submit");
    std::printf("device-close %s status=%d joins=%u release_owned=%d\n",
                point == Point::Popped ? "popped" : "dma", result,
                run.join_calls, run.release_while_owned);
    txBufFree(&context.circBuf);
    pthread_mutex_destroy(&context.thLock);
    observed = nullptr;
    bc_dil_glob_ptr = nullptr;
}

static void test_send_admission()
{
    run = {};
    run.done = true; // This fixture has no admitted TX worker.
    run.firmware_mode = 2;
    bc_dil_glob_s globals = {};
    bc_dil_glob_ptr = &globals;
    DTS_LIB_CONTEXT context = {};
    observed = &context;
    context.Sig = LIB_CTX_SIG;
    context.DevId = BC_PCI_DEVID_FLEA;
    context.ProcessID = getpid();
    context.VidParams.StreamType = BC_STREAM_TYPE_PES;
    context.alignBuf = static_cast<uint8_t *>(std::malloc(ALIGN_BUF_SIZE));
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&context.thLock, &attr);
    pthread_mutexattr_destroy(&attr);
    if (!context.alignBuf || txBufInit(&context.circBuf, CIRC_TX_BUF_SIZE) != BC_STS_SUCCESS)
        std::abort();
    uint8_t bytes[sizeof(packet) + 4] = {};
    std::memcpy(bytes, packet, sizeof(packet));
    for (const auto state : {BC_DEC_STATE_STOP, BC_DEC_STATE_FLUSH, BC_DEC_STATE_CLOSE,
                              BC_DEC_STATE_START, BC_DEC_STATE_PAUSE}) {
        context.State = state;
        context.txQuiescing = state == BC_DEC_STATE_START || state == BC_DEC_STATE_PAUSE;
        const BC_STATUS result = DtsSendData(&context, bytes, sizeof(packet), 0, false);
        check(result == BC_STS_IO_USER_ABORT,
              "SendData rejects stopped/flushing/quiescing input even when ring has capacity");
        check(context.circBuf.busySize == 0 && DtsTxFreeSize(&context) == CIRC_TX_BUF_SIZE,
              "rejected SendData never repopulates the cleared ring");
        txBufFlush(&context.circBuf); // Keep each baseline failure independent.
    }
    context.txQuiescing = false;
    for (const auto state : {BC_DEC_STATE_START, BC_DEC_STATE_PAUSE}) {
        context.State = state;
        check(DtsSendData(&context, bytes, sizeof(packet), 0, false) == BC_STS_SUCCESS,
              "SendData retains successful START/PAUSE enqueue behavior");
        check(context.circBuf.busySize == sizeof(packet) &&
              !std::memcmp(context.circBuf.buffer, bytes, sizeof(packet)),
              "accepted SendData queues exact caller bytes");
        txBufFlush(&context.circBuf);
    }
    context.State = BC_DEC_STATE_START;
    uint8_t *filler = static_cast<uint8_t *>(std::calloc(1, CIRC_TX_BUF_SIZE));
    if (!filler || txBufPush(&context.circBuf, filler, CIRC_TX_BUF_SIZE) != BC_STS_SUCCESS)
        std::abort();
    std::free(filler);
    ring_wait_stops = 0;
    stop_at_ring_wait = true;
    check(DtsSendData(&context, bytes, sizeof(packet), 0, false) == BC_STS_IO_USER_ABORT,
          "a producer waiting for capacity aborts after StopDecoder clears the ring");
    check(ring_wait_stops == 1 && !stop_at_ring_wait && context.circBuf.busySize == 0,
          "full-ring cancellation leaves no late queued input");
    context.State = BC_DEC_STATE_START;
    packetizer_stops = 0;
    stop_at_packet_length = true;
    const BC_STATUS result = DtsAlignSendData(&context, bytes, sizeof(packet), 100000, false);
    check(packetizer_stops == 1 && !stop_at_packet_length && context.State == BC_DEC_STATE_STOP,
          "real packetizer passes its initial state check before the injected stop");
    check(result == BC_STS_IO_USER_ABORT && context.circBuf.busySize == 0,
          "already-packetizing input cannot refill the ring after StopDecoder cleared it");
    std::printf("send-admission packetizer_stop_status=%d queued_after_stop=%u\n",
                result, context.circBuf.busySize);
    txBufFree(&context.circBuf);
    std::free(context.alignBuf);
    pthread_mutex_destroy(&context.thLock);
    observed = nullptr;
    bc_dil_glob_ptr = nullptr;
}

int main()
{
    test_send_admission();
    for (const Point point : {Point::Popped, Point::Dma})
        for (const bool stop : {false, true})
            for (const bool timeout : {false, true})
                test_case(point, stop, timeout);
    for (const Point point : {Point::Popped, Point::Dma})
        for (const bool timeout : {false, true})
            test_case(point, true, timeout, true);
    for (const Point point : {Point::Popped, Point::Dma}) {
        for (const bool stop : {false, true})
            test_case(point, stop, false, false, true);
        test_device_close(point);
        for (const bool timeout : {false, true})
            test_case(point, false, timeout, true, false, 3);
        for (const unsigned operation : {1U, 4U})
            test_case(point, false, false, false, false, operation);
    }
    std::printf("TX flush ordering: %s (%u failed checks)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
