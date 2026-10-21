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
#include <vector>
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
static thread_local bool packetizer_stopper;
static bool packetizer_stop_waiting, packetizer_input_release;
static bool claim_recovery_at_packet_length;
static unsigned packetizer_stops;
static std::thread *packetizer_stop_thread;
static BC_STATUS packetizer_stop_status;
static bool stop_at_ring_wait;
static unsigned ring_wait_stops;
static bool latch_recovery_at_pause_poll;
static unsigned pause_polls;
static bool observe_recovery_eos_packetization;
static uint32_t recovery_eos_packetization_state;
static bool claim_control_after_close_check;
static bool pause_input_at_config_cache;
static bool input_at_config_cache;
static bool release_input_at_config_cache;
static bool pause_flush_input_drain;
static bool flush_waiting_for_input;
static bool overlapped_input_done;
static bool observe_recovery_during_join;
static BC_STATUS join_snapshot_getter_status;
static BC_STATUS join_snapshot_recovery_status;
static uint64_t join_snapshot_generation;
static uint8_t join_snapshot_active;
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

enum class RecoveryStage {
    Setup,
    OutputFlush,
    Open,
    RingFlush,
    MetadataFlush,
    FleaColor,
    LinkColor,
    Start,
    Capture,
};

static struct {
    bool active, block, waiting;
    bool pause_owner_contention, contention_waiting, release_contention;
    bool pause_input_drain, input_drain_waiting, release_input_drain;
    bool latch_pm_on_failure;
    unsigned released, status_calls, exit_status_call;
    uint8_t ready_count;
    int fail_stage;
    BC_STATUS failure;
    BC_STATUS open_status;
    uint32_t open_channel_id;
    unsigned open_calls;
    bool publish_open_scale;
    uint32_t open_cached_scale;
    uint32_t open_active_scale_before;
    uint32_t open_active_scale_after;
    uint32_t capture_cached_scale;
    uint32_t capture_active_scale;
    uint32_t capture_buffer_width;
    RecoveryStage current;
    std::vector<RecoveryStage> order;
    std::vector<uint8_t> power_states;
} recovery;

enum class SetupCommand { FleaPush, LinkPush, Initialize };
static struct {
    bool active;
    BC_STATUS push_status, initialize_status;
    std::vector<SetupCommand> calls;
    std::vector<uint32_t> states;
} setup_commands;

static std::thread *late_recovery_owner;
static bool late_owner_ready, late_owner_release;
static BC_STATUS late_owner_status;
static const uint8_t packet[] = {0x00, 0x00, 0x01, 0xe0, 0x42, 0x19, 0xa5, 0x7c};
static const uint32_t active_scale_word = 0x50050001U;
static const uint32_t active_scale_width = 1280U;

static void check(bool condition, const char *message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

template<class Predicate>
static void await(std::unique_lock<std::mutex> &lock, Predicate predicate);

/* This cross-TU helper is called after AlignSendData checked START, but before
 * it enqueues the completed PES packet. Stop on another thread at that exact
 * seam, forwarding the real byte swap and all remaining production framing.
 * The mangled spelling is invariant on both supported 32/64-bit ABIs (ushort).
 */
extern "C" uint16_t __real__Z9WORD_SWAPt(uint16_t);
extern "C" uint16_t __wrap__Z9WORD_SWAPt(uint16_t value)
{
    const uint16_t result = __real__Z9WORD_SWAPt(value);
	if (observe_recovery_eos_packetization && worker_thread && recovery.active) {
		observe_recovery_eos_packetization = false;
		recovery_eos_packetization_state = observed->State;
	}
    if (stop_at_packet_length) {
        stop_at_packet_length = false;
        packetizer_stop_status = BC_STS_ERROR;
        packetizer_stop_waiting = false;
        packetizer_input_release = false;
        packetizer_stop_thread = new std::thread([] {
            packetizer_stopper = true;
            packetizer_stop_status = DtsStopDecoder(observed);
            packetizer_stopper = false;
        });
        std::unique_lock<std::mutex> lock(gate);
        await(lock, [] { return packetizer_stop_waiting; });
        ++packetizer_stops;
    }
    if (claim_recovery_at_packet_length) {
        claim_recovery_at_packet_length = false;
        late_owner_ready = false;
        late_owner_release = false;
        late_owner_status = BC_STS_ERROR;
        late_recovery_owner = new std::thread([] {
            DtsLock(observed);
            observed->recoveryPending = true;
            /* DtsAcquireControl is deliberately recursive here: this mirrors
             * the production RESUME latch and owner claim in one thLock
             * critical section while input is already inside ProcInput. */
            late_owner_status = DtsAcquireControl(observed, DTS_CONTROL_RECOVERY);
            DtsUnLock(observed);
            std::unique_lock<std::mutex> lock(gate);
            late_owner_ready = true;
            changed.notify_all();
            await(lock, [] { return late_owner_release; });
            lock.unlock();
            if (late_owner_status == BC_STS_SUCCESS)
                DtsReleaseControl(observed);
        });
        std::unique_lock<std::mutex> lock(gate);
        await(lock, [] { return late_owner_ready; });
    }
    return result;
}

extern "C" BC_STATUS __real__Z14DtsCacheSpsPpsPvPKhj(
    HANDLE, const uint8_t *, uint32_t);
extern "C" BC_STATUS __wrap__Z14DtsCacheSpsPpsPvPKhj(
    HANDLE device, const uint8_t *bytes, uint32_t size)
{
    const BC_STATUS status =
        __real__Z14DtsCacheSpsPpsPvPKhj(device, bytes, size);
    if (pause_input_at_config_cache) {
        std::unique_lock<std::mutex> lock(gate);
        pause_input_at_config_cache = false;
        input_at_config_cache = true;
        changed.notify_all();
        await(lock, [] { return release_input_at_config_cache; });
    }
    return status;
}

template<class Predicate>
static void await(std::unique_lock<std::mutex> &lock, Predicate predicate)
{
    if (!changed.wait_for(lock, std::chrono::seconds(3), predicate)) {
        std::fputs("TX flush fixture handshake timed out\n", stderr);
        std::abort();
    }
}

static BC_STATUS recovery_stage(RecoveryStage stage)
{
    std::unique_lock<std::mutex> lock(gate);
    recovery.current = stage;
    recovery.order.push_back(stage);
    recovery.waiting = true;
    const unsigned arrival = recovery.order.size();
    changed.notify_all();
    if (recovery.block)
        await(lock, [arrival] { return recovery.released >= arrival; });
    recovery.waiting = false;
    changed.notify_all();
    if (recovery.fail_stage == static_cast<int>(arrival - 1)) {
        if (recovery.latch_pm_on_failure)
            DtsObservePowerState(observed, BC_HW_SUSPEND);
        return recovery.failure;
    }
    return BC_STS_SUCCESS;
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
    if (claim_control_after_close_check && observed && mutex == &observed->thLock) {
        claim_control_after_close_check = false;
        // Model START acquiring control just after DeviceClose's first check.
        DtsLock(observed);
        observed->stopInProgress = true;
        DtsUnLock(observed);
    }
    if (!recovery.active && worker_thread && observed &&
        mutex == &observed->circBuf.flushLock &&
        run.point == Point::Popped && !run.blocked)
        pause_worker(); // The production pop really completed before this hook.
    return result;
}

extern "C" int __wrap_usleep(useconds_t microseconds)
{
	if (pause_flush_input_drain && microseconds == 10000) {
		std::unique_lock<std::mutex> lock(gate);
		pause_flush_input_drain = false;
		flush_waiting_for_input = true;
		changed.notify_all();
		await(lock, [] { return overlapped_input_done; });
		return 0;
	}
	if (packetizer_stopper && microseconds == 10000) {
		std::unique_lock<std::mutex> lock(gate);
		packetizer_stop_waiting = true;
		changed.notify_all();
		await(lock, [] { return packetizer_input_release; });
		return 0;
	}
	if (latch_recovery_at_pause_poll && !worker_thread &&
	    microseconds == 10000) {
		latch_recovery_at_pause_poll = false;
		++pause_polls;
		DtsObservePowerState(observed, BC_HW_SUSPEND);
		return 0;
	}
	if (recovery.active && worker_thread && microseconds == 10000 &&
	    recovery.pause_input_drain) {
		std::unique_lock<std::mutex> lock(gate);
		recovery.pause_input_drain = false;
		recovery.input_drain_waiting = true;
		changed.notify_all();
		await(lock, [] { return recovery.release_input_drain; });
		return 0;
	}
    if (recovery.active && worker_thread && microseconds == 2000 &&
        recovery.pause_owner_contention) {
        std::unique_lock<std::mutex> lock(gate);
        recovery.pause_owner_contention = false;
        recovery.contention_waiting = true;
        changed.notify_all();
        await(lock, [] { return recovery.release_contention; });
        return 0;
    }
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
	if (observe_recovery_during_join && observed) {
		DtsLock(observed);
		const pthread_t published_thread = observed->htxThread;
		DtsUnLock(observed);
		if (thread == published_thread) {
			observe_recovery_during_join = false;
			DtsObservePowerState(observed, BC_HW_SUSPEND);
			uint32_t replay_bytes = 0;
			join_snapshot_getter_status = DtsGetRecoveryStatus(observed,
				&join_snapshot_generation, &join_snapshot_recovery_status,
				&join_snapshot_active, &replay_bytes);
			return __real_pthread_join(thread, result);
		}
	}
    if (!cancelling_thread || !observed || thread != observed->htxThread)
        return __real_pthread_join(thread, result);
    check(observed->stopInProgress,
          "DeviceClose claims the control lifetime before joining its TX worker");
    if (observed->stopInProgress)
        check(DtsStartDecoder(observed) == BC_STS_BUSY &&
              DtsDeviceClose(observed) == BC_STS_BUSY,
              "no restart or competing close can enter the claimed join interval");
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
    if (recovery.active) {
        status->DrvcpbEmptySize = 0;
		status->drvRLL = recovery.ready_count;
		if (!recovery.power_states.empty()) {
			const size_t index = recovery.status_calls++;
			if (index < recovery.power_states.size()) {
				status->pwr_state_change = recovery.power_states[index];
				return BC_STS_SUCCESS;
			}
			DtsLock(observed);
			observed->txThreadExit = true;
			DtsUnLock(observed);
			return BC_STS_IO_USER_ABORT;
		}
        if (++recovery.status_calls < recovery.exit_status_call) {
            status->pwr_state_change = BC_HW_RESUME;
            return BC_STS_SUCCESS;
        }
        DtsLock(observed);
        observed->txThreadExit = true;
        DtsUnLock(observed);
        return BC_STS_IO_USER_ABORT;
    }
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
BC_STATUS DtsFWStopVideo(HANDLE device, uint32_t, bool force)
{
    firmware_boundary(1);
    DTS_LIB_CONTEXT *context = static_cast<DTS_LIB_CONTEXT *>(device);
    DtsLock(context);
    if (!force && context->stopInProgress) {
        context->stopOutcome = DTS_STOP_CONFIRMED;
        context->stopStatus = BC_STS_SUCCESS;
    }
    context->State = BC_DEC_STATE_STOP;
    DtsUnLock(context);
    return BC_STS_SUCCESS;
}
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
BC_STATUS DtsFWOpenChannel(HANDLE device, uint32_t, uint32_t)
{
    const BC_STATUS status = control_call();
    ++recovery.open_calls;
    if (status != BC_STS_SUCCESS || recovery.open_status != BC_STS_SUCCESS)
        return status != BC_STS_SUCCESS ? status : recovery.open_status;
    DTS_LIB_CONTEXT *context = static_cast<DTS_LIB_CONTEXT *>(device);
    recovery.open_cached_scale = context->EnableScaling;
    recovery.open_active_scale_before = context->activeFleaScaleWord;
    if (recovery.publish_open_scale) {
        check(context->DevId == BC_PCI_DEVID_FLEA &&
              context->VidParams.VideoAlgo == BC_VID_ALGO_H264 &&
              context->EnableScaling == active_scale_word,
              "scaled recovery OPEN republishes only the exact cached Flea H.264 request");
        context->activeFleaScaleWord = active_scale_word;
    }
    recovery.open_active_scale_after = context->activeFleaScaleWord;
    context->OpenRsp.channelId = recovery.open_channel_id;
    return BC_STS_SUCCESS;
}
BC_STATUS DtsFWSetVideoInput(HANDLE) { return control_call(); }
BC_STATUS DtsFWSetHostTrickMode(HANDLE, uint32_t) { std::abort(); }
BC_STATUS DtsFWSetSkipPictureMode(HANDLE, uint32_t) { std::abort(); }
BC_STATUS DtsFWSetSlowMotionRate(HANDLE, uint32_t) { std::abort(); }
BC_STATUS DtsFWSetFFRate(HANDLE, uint32_t) { std::abort(); }
BC_STATUS DtsFWFrameAdvance(HANDLE) { std::abort(); }
BC_STATUS DtsFWDrop(HANDLE, uint32_t) { std::abort(); }
BC_STATUS DtsSetProgressive(HANDLE, uint32_t) { return control_call(); }
BC_STATUS DtsSetVideoClock(HANDLE, uint32_t) { return control_call(); }
BC_STATUS DtsSetTSMode(HANDLE, uint32_t) { return control_call(); }
static BC_STATUS setup_command(HANDLE device, SetupCommand command)
{
    if (!setup_commands.active)
        return control_call();
    ++run.control_calls;
    check(device == observed, "setup command retains its exact fixture handle");
    setup_commands.calls.push_back(command);
    setup_commands.states.push_back(DtsGetHwInitSts());
    return command == SetupCommand::Initialize ?
        setup_commands.initialize_status : setup_commands.push_status;
}
BC_STATUS DtsPushFwToFlea(HANDLE device, char *)
{ return setup_command(device, SetupCommand::FleaPush); }
BC_STATUS DtsPushAuthFwToLink(HANDLE device, char *)
{ return setup_command(device, SetupCommand::LinkPush); }
BC_STATUS DtsFWInitialize(HANDLE device, uint32_t)
{ return setup_command(device, SetupCommand::Initialize); }
/* Only unexecuted pixel-copy paths need these symbols. */
BC_STATUS DtsCopyRawDataToOutBuff(DTS_LIB_CONTEXT *, BC_DTS_PROC_OUT *, BC_DTS_PROC_OUT *)
{ std::abort(); }
BC_STATUS DtsCopyNV12ToYV12(DTS_LIB_CONTEXT *, BC_DTS_PROC_OUT *, BC_DTS_PROC_OUT *)
{ std::abort(); }
BC_STATUS DtsCopyNV12(DTS_LIB_CONTEXT *, BC_DTS_PROC_OUT *, BC_DTS_PROC_OUT *)
{ std::abort(); }
BC_STATUS DtsCopyFormat(DTS_LIB_CONTEXT *, BC_DTS_PROC_OUT *, BC_DTS_PROC_OUT *)
{ std::abort(); }
extern "C" BC_STATUS __real_DtsSetupHardware(HANDLE, BOOL);
extern "C" BC_STATUS __wrap_DtsSetupHardware(HANDLE device, BOOL ignore_clock)
{
    if (!worker_thread)
        return __real_DtsSetupHardware(device, ignore_clock);
    if (!recovery.active)
        std::abort();
    const BC_STATUS status = recovery_stage(RecoveryStage::Setup);
    return status == BC_STS_SUCCESS ?
        __real_DtsSetupHardware(device, ignore_clock) : status;
}
extern "C" BC_STATUS __real_DtsOpenDecoder(HANDLE, uint32_t);
extern "C" BC_STATUS __wrap_DtsOpenDecoder(HANDLE device, uint32_t stream_type)
{
    if (!worker_thread)
        return __real_DtsOpenDecoder(device, stream_type);
    if (!recovery.active)
        std::abort();
    const BC_STATUS status = recovery_stage(RecoveryStage::Open);
    return status == BC_STS_SUCCESS ?
        __real_DtsOpenDecoder(device, stream_type) : status;
}
extern "C" BC_STATUS __real_DtsFlushRxCapture(HANDLE, BOOL);
extern "C" BC_STATUS __wrap_DtsFlushRxCapture(HANDLE device, BOOL discard_only)
{
    if (!worker_thread || !recovery.active)
        return __real_DtsFlushRxCapture(device, discard_only);
    check(!discard_only, "recovery uses destructive capture cleanup after reset");
    const BC_STATUS status = recovery_stage(RecoveryStage::OutputFlush);
    if (status == BC_STS_SUCCESS) {
        DTS_LIB_CONTEXT *context = static_cast<DTS_LIB_CONTEXT *>(device);
        DtsLock(context);
        context->bMapOutBufDone = false;
        context->bMapOutBufDirty = false;
        if (context->outputPhase == DTS_OUTPUT_RETIRE_ONLY) {
            context->ProcOutPending = 0;
            context->outputPhase = DTS_OUTPUT_IDLE;
        }
        DtsUnLock(context);
    }
    return status;
}
extern "C" BC_STATUS __real_DtsStartDecoder(HANDLE);
extern "C" BC_STATUS __wrap_DtsStartDecoder(HANDLE device)
{
    if (worker_thread) {
        if (!recovery.active)
            std::abort();
        const BC_STATUS status = recovery_stage(RecoveryStage::Start);
        if (status != BC_STS_SUCCESS)
            return status;
    }
    return __real_DtsStartDecoder(device);
}
extern "C" BC_STATUS __real_DtsStartCapture(HANDLE);
extern "C" BC_STATUS __wrap_DtsStartCapture(HANDLE device)
{
    if (!worker_thread)
        return __real_DtsStartCapture(device);
    if (!recovery.active)
        std::abort();
    const BC_STATUS status = recovery_stage(RecoveryStage::Capture);
    if (status == BC_STS_SUCCESS) {
        DTS_LIB_CONTEXT *context = static_cast<DTS_LIB_CONTEXT *>(device);
        recovery.capture_cached_scale = context->EnableScaling;
        recovery.capture_active_scale = context->activeFleaScaleWord;
        recovery.capture_buffer_width = context->rxBufferWidth;
    }
    return status;
}
extern "C" BC_STATUS __real_DtsRecoveryFlushInputRing(DTS_LIB_CONTEXT *);
extern "C" BC_STATUS __wrap_DtsRecoveryFlushInputRing(DTS_LIB_CONTEXT *context)
{
    if (!worker_thread || !recovery.active)
        return __real_DtsRecoveryFlushInputRing(context);
    const BC_STATUS status = recovery_stage(RecoveryStage::RingFlush);
    return status == BC_STS_SUCCESS ?
        __real_DtsRecoveryFlushInputRing(context) : status;
}
extern "C" BC_STATUS __real_DtsRecoveryRestoreInputState(DTS_LIB_CONTEXT *);
extern "C" BC_STATUS __wrap_DtsRecoveryRestoreInputState(DTS_LIB_CONTEXT *context)
{
    if (!worker_thread || !recovery.active)
        return __real_DtsRecoveryRestoreInputState(context);
    const BC_STATUS status = recovery_stage(RecoveryStage::MetadataFlush);
    return status == BC_STS_SUCCESS ?
        __real_DtsRecoveryRestoreInputState(context) : status;
}
extern "C" BC_STATUS __wrap_DtsSetFleaIn422Mode(HANDLE device)
{
    if (!worker_thread || !recovery.active || device != observed)
        std::abort();
    return recovery_stage(RecoveryStage::FleaColor);
}
extern "C" BC_STATUS __wrap_DtsSetLinkIn422Mode(HANDLE device)
{
    if (!worker_thread || !recovery.active || device != observed)
        std::abort();
    return recovery_stage(RecoveryStage::LinkColor);
}
extern "C" BC_STATUS __wrap_DtsReleaseInterface(DTS_LIB_CONTEXT *)
{
    // Record a bad public-close destruction attempt without freeing a live
    // worker's stack fixture or invoking any real device/shared-memory cleanup.
    ++run.release_calls;
    run.release_while_owned |= !run.done;
    check(observed->controlOwner == DTS_CONTROL_TEARDOWN &&
          observed->controlDepth == 1,
          "terminal handle consumption retains its teardown owner until free");
    BC_STATUS start_status = BC_STS_ERROR;
    BC_STATUS close_status = BC_STS_ERROR;
    std::thread competitor([&] {
        start_status = DtsStartDecoder(observed);
        close_status = DtsDeviceClose(observed);
    });
    competitor.join();
    check(start_status == BC_STS_BUSY && close_status == BC_STS_BUSY,
          "no lifecycle or second close enters the final handle-free window");
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
    if (posix_memalign(reinterpret_cast<void **>(&context.txScratch), 128,
                       CIRC_TX_BUF_SIZE) != 0 ||
        txBufInit(&context.circBuf, CIRC_TX_BUF_SIZE) != BC_STS_SUCCESS ||
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
        // The fixture worker sets this flag only to end its bounded loop.
        // This is a fresh worker test, not a terminal DeviceClose join.
        context.txThreadExit = false;
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
    std::free(context.txScratch);
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
    if (posix_memalign(reinterpret_cast<void **>(&context.txScratch), 128,
                       CIRC_TX_BUF_SIZE) != 0 ||
        txBufInit(&context.circBuf, CIRC_TX_BUF_SIZE) != BC_STS_SUCCESS ||
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
    context.inputFragmentActive = true;
    context.inputFragmentEncrypted = false;
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
          run.last_stop > run.dma_retired && context.State == BC_DEC_STATE_CLOSE &&
          !context.inputFragmentActive && !context.inputFragmentRawPermit &&
          !context.inputFragmentMdata,
          "DeviceClose stops and closes only after its joined worker can no longer submit");
    std::printf("device-close %s status=%d joins=%u release_owned=%d\n",
                point == Point::Popped ? "popped" : "dma", result,
                run.join_calls, run.release_while_owned);
    txBufFree(&context.circBuf);
    std::free(context.txScratch);
    pthread_mutex_destroy(&context.thLock);
    observed = nullptr;
    bc_dil_glob_ptr = nullptr;
}

static void test_flush_rearms_config_after_old_input_retires()
{
    run = {};
    run.done = true;
    recovery = {};
    bc_dil_glob_s globals = {};
    bc_dil_glob_ptr = &globals;
    DTS_LIB_CONTEXT context = {};
    observed = &context;
    context.Sig = LIB_CTX_SIG;
    context.State = BC_DEC_STATE_START;
    context.DevId = BC_PCI_DEVID_FLEA;
    context.ProcessID = getpid();
    context.VidParams.StreamType = BC_STREAM_TYPE_PES;
    context.VidParams.MediaSubType = BC_MSUBTYPE_H264;
    context.PESConvParams.m_bAddSpsPps = false;
    context.alignBuf = static_cast<uint8_t *>(std::malloc(ALIGN_BUF_SIZE));
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&context.thLock, &attr);
    pthread_mutexattr_destroy(&attr);
    if (!context.alignBuf ||
        txBufInit(&context.circBuf, CIRC_TX_BUF_SIZE) != BC_STS_SUCCESS)
        std::abort();

    uint8_t input[] = {0, 0, 0, 1, 0x65, 0x88, 0x84, 0x11};
    BC_STATUS input_status = BC_STS_ERROR;
    BC_STATUS flush_status = BC_STS_ERROR;
    pause_input_at_config_cache = true;
    input_at_config_cache = false;
    release_input_at_config_cache = false;
    pause_flush_input_drain = true;
    flush_waiting_for_input = false;
    overlapped_input_done = false;

    std::thread input_thread([&] {
        input_status = DtsProcInput(&context, input, sizeof(input), 0, false);
        std::lock_guard<std::mutex> lock(gate);
        overlapped_input_done = true;
        changed.notify_all();
    });
    {
        std::unique_lock<std::mutex> lock(gate);
        await(lock, [] { return input_at_config_cache; });
    }
    std::thread flush_thread([&] {
        flush_status = DtsFlushInput(&context, 3);
    });
    {
        std::unique_lock<std::mutex> lock(gate);
        await(lock, [] { return flush_waiting_for_input; });
        DtsLock(&context);
        check(context.inputCalls == 1 && context.txQuiescing &&
              context.controlOwner == DTS_CONTROL_LIFECYCLE &&
              context.circBuf.busySize != 0,
              "destructive Op3 flush waits for the owner of an already accepted input");
        check(!context.PESConvParams.m_bAddSpsPps,
              "flush does not publish the next-session config rearm before old input retires");
        DtsUnLock(&context);
        release_input_at_config_cache = true;
        changed.notify_all();
    }
    input_thread.join();
    flush_thread.join();

    DtsLock(&context);
    check(input_status == BC_STS_SUCCESS &&
          flush_status == BC_STS_SUCCESS && context.inputCalls == 0,
          "accepted old input retires before destructive Op3 flush completes");
    check(context.PESConvParams.m_bAddSpsPps &&
          context.State == BC_DEC_STATE_FLUSH && !context.txQuiescing &&
          context.controlOwner == DTS_CONTROL_NONE,
          "post-drain Op3 rearm survives the retired input and preserves host-flush state");
    DtsUnLock(&context);

    pause_input_at_config_cache = false;
    input_at_config_cache = false;
    release_input_at_config_cache = false;
    pause_flush_input_drain = false;
    flush_waiting_for_input = false;
    overlapped_input_done = false;
    txBufFree(&context.circBuf);
    std::free(context.alignBuf);
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
    context.inputFragmentActive = true;
    context.inputFragmentEncrypted = false;
    check(DtsStopDecoder(&context) == BC_STS_SUCCESS &&
          !context.inputFragmentActive && !context.inputFragmentRawPermit,
          "real StopDecoder aborts an unfinished fragmented input transaction");
    run.allow_restart = true;
    check(DtsStartDecoder(&context) == BC_STS_SUCCESS,
          "fragment teardown fixture restarts through the real lifecycle API");
    run.allow_restart = false;
    context.inputFragmentActive = true;
    context.inputFragmentEncrypted = false;
    check(DtsFlushInput(&context, 3) == BC_STS_SUCCESS &&
          !context.inputFragmentActive && !context.inputFragmentRawPermit &&
          !context.inputFragmentMdata,
          "real destructive FlushInput aborts an unfinished fragmented input transaction");
    run.allow_restart = true;
    check(DtsStartDecoder(&context) == BC_STS_SUCCESS,
          "fragment flush fixture restarts through the real lifecycle API");
    run.allow_restart = false;
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
    run.allow_restart = true;
    check(DtsStartDecoder(&context) == BC_STS_SUCCESS,
          "next packetizer case restarts through the real lifecycle API");
    run.allow_restart = false;
    packetizer_stops = 0;
    stop_at_packet_length = true;
    const BC_STATUS result = DtsProcInput(&context, bytes, sizeof(packet), 100000, false);
    {
        std::lock_guard<std::mutex> lock(gate);
        packetizer_input_release = true;
        changed.notify_all();
    }
    packetizer_stop_thread->join();
    delete packetizer_stop_thread;
    packetizer_stop_thread = nullptr;
    check(packetizer_stop_status == BC_STS_SUCCESS,
          "packetizer seam completes a real cross-thread StopDecoder");
    check(packetizer_stops == 1 && !stop_at_packet_length && context.State == BC_DEC_STATE_STOP,
          "real packetizer passes its initial state check before the injected stop");
    check(result == BC_STS_IO_USER_ABORT && context.circBuf.busySize == 0,
          "already-packetizing input cannot refill the ring after StopDecoder cleared it");
    run.allow_restart = true;
    check(DtsStartDecoder(&context) == BC_STS_SUCCESS,
          "late-recovery input seam restarts through the normal lifecycle path");
    run.allow_restart = false;
    uint8_t recovery_bytes[sizeof(packet) + 4] = {};
    std::memcpy(recovery_bytes, packet, sizeof(packet));
    claim_recovery_at_packet_length = true;
    const BC_STATUS recovery_result = DtsProcInput(&context, recovery_bytes,
        sizeof(packet), 100000, false);
    check(!claim_recovery_at_packet_length && late_owner_status == BC_STS_SUCCESS,
          "input admitted before RESUME observes the atomically claimed recovery owner");
    check(recovery_result == BC_STS_BUSY && context.circBuf.busySize == 0,
          "late recovery returns BUSY, not IO_USER_ABORT, with no partial input");
    DtsLock(&context);
    check(context.inputCalls == 0 && context.controlOwner == DTS_CONTROL_RECOVERY &&
          context.controlDepth == 1 && context.recoveryPending,
          "the admitted input retires before the recovery owner is released");
    DtsUnLock(&context);
    {
        std::lock_guard<std::mutex> lock(gate);
        late_owner_release = true;
        changed.notify_all();
    }
    late_recovery_owner->join();
    delete late_recovery_owner;
    late_recovery_owner = nullptr;
    DtsLock(&context);
    context.recoveryPending = false;
    DtsUnLock(&context);
    std::memset(recovery_bytes, 0, sizeof(recovery_bytes));
    std::memcpy(recovery_bytes, packet, sizeof(packet));
    check(DtsProcInput(&context, recovery_bytes, sizeof(packet), 100000, false) ==
              BC_STS_SUCCESS && context.circBuf.busySize > 0,
          "ordinary input succeeds again after recovery ownership retires");
	txBufFlush(&context.circBuf);
	context.State = BC_DEC_STATE_PAUSE;
	pause_polls = 0;
	latch_recovery_at_pause_poll = true;
	check(DtsAlignSendData(&context, bytes, sizeof(packet), 0, false) == BC_STS_BUSY &&
	      pause_polls == 1 && context.circBuf.busySize == 0 &&
	      context.inputCalls == 0,
	      "paused input observes a PM latch on its first short poll and retires promptly");
	DtsLock(&context);
	context.recoveryPending = false;
	context.recoverySuspendSeen = false;
	DtsUnLock(&context);
    std::printf("send-admission packetizer_stop_status=%d queued_after_stop=%u\n",
                result, context.circBuf.busySize);
    txBufFree(&context.circBuf);
    std::free(context.alignBuf);
    pthread_mutex_destroy(&context.thLock);
    observed = nullptr;
    bc_dil_glob_ptr = nullptr;
}

struct RecoveryFixture {
    bc_dil_glob_s globals;
    DTS_LIB_CONTEXT context;
    DTS_INPUT_MDATA metadata;

    explicit RecoveryFixture(uint32_t device)
    {
        run = {};
        run.done = true;
        run.allow_restart = true;
        recovery = {};
        recovery.active = true;
        recovery.fail_stage = -1;
        recovery.failure = BC_STS_IO_XFR_ERROR;
        recovery.exit_status_call = 4;
		observe_recovery_eos_packetization = false;
		recovery_eos_packetization_state = UINT32_MAX;
        std::memset(&globals, 0, sizeof(globals));
        std::memset(&context, 0, sizeof(context));
        std::memset(&metadata, 0, sizeof(metadata));
        bc_dil_glob_ptr = &globals;
        observed = &context;
        context.Sig = LIB_CTX_SIG;
        context.State = BC_DEC_STATE_START;
        context.DevId = device;
        context.ProcessID = getpid();
        context.VidParams.StreamType = BC_STREAM_TYPE_PES;
        context.VidParams.MediaSubType = BC_MSUBTYPE_H264;
        context.VidParams.VideoAlgo = BC_VID_ALGO_H264;
        context.b422Mode = device == BC_PCI_DEVID_FLEA ?
            OUTPUT_MODE422_YUY2 : OUTPUT_MODE422_UYVY;
        context.MdataPoolPtr = &metadata;
        context.MDFreeHead = &metadata;
        context.MDPendHead = context.MDPendTail = DTS_MDATA_PEND_LINK((&context));
        context.LastPicNum = 91;
        context.LastSessNum = 73;
        context.EOSCnt = 5;
        context.DrvStatusEOSCnt = 7;
        context.bEOS = true;
        context.PESConvParams.m_lStartCodeDataSize = 11;
        context.PESConvParams.m_bAddSpsPps = false;
        context.alignBuf = static_cast<uint8_t *>(std::malloc(ALIGN_BUF_SIZE));
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&context.thLock, &attr);
        pthread_mutexattr_destroy(&attr);
        if (!context.alignBuf ||
            posix_memalign(reinterpret_cast<void **>(&context.txScratch), 128,
                           CIRC_TX_BUF_SIZE) != 0 ||
            txBufInit(&context.circBuf, CIRC_TX_BUF_SIZE) != BC_STS_SUCCESS ||
            txBufPush(&context.circBuf, const_cast<uint8_t *>(packet),
                      sizeof(packet)) != BC_STS_SUCCESS)
            std::abort();
    }

    ~RecoveryFixture()
    {
        txBufFree(&context.circBuf);
        std::free(context.txScratch);
        std::free(context.alignBuf);
        pthread_mutex_destroy(&context.thLock);
        observed = nullptr;
        bc_dil_glob_ptr = nullptr;
        recovery.active = false;
    }
};

struct SetupCommandScope {
    SetupCommandScope()
    {
        setup_commands = {};
        setup_commands.active = true;
        setup_commands.push_status = BC_STS_SUCCESS;
        setup_commands.initialize_status = BC_STS_SUCCESS;
    }
    ~SetupCommandScope() { setup_commands.active = false; }
};

static void test_setup_hardware_initialization_state()
{
    for (const uint32_t device : {BC_PCI_DEVID_LINK, BC_PCI_DEVID_FLEA}) {
        const SetupCommand push = device == BC_PCI_DEVID_LINK ?
            SetupCommand::LinkPush : SetupCommand::FleaPush;
        {
            RecoveryFixture fixture(device);
            SetupCommandScope commands;
            setup_commands.push_status = BC_STS_FW_AUTH_FAILED;
            check(DtsSetupHardware(&fixture.context, false) == BC_STS_FW_AUTH_FAILED &&
                  DtsGetHwInitSts() == BC_DIL_HWINIT_NOT_YET &&
                  setup_commands.calls == std::vector<SetupCommand>{push},
                  "failed firmware push returns the original error and leaves initialization retryable");
            setup_commands.push_status = BC_STS_SUCCESS;
            check(DtsSetupHardware(&fixture.context, false) == BC_STS_SUCCESS &&
                  DtsGetHwInitSts() == BC_DIL_HWINIT_DONE &&
                  setup_commands.calls == std::vector<SetupCommand>{push, push, SetupCommand::Initialize} &&
                  setup_commands.states == std::vector<uint32_t>(3, BC_DIL_HWINIT_IN_PROGRESS),
                  "retry after failed push actually uploads and initializes before publishing DONE");
        }
        {
            RecoveryFixture fixture(device);
            SetupCommandScope commands;
            setup_commands.initialize_status = BC_STS_FW_CMD_ERR;
            check(DtsSetupHardware(&fixture.context, false) == BC_STS_FW_CMD_ERR &&
                  DtsGetHwInitSts() == BC_DIL_HWINIT_NOT_YET &&
                  setup_commands.calls == std::vector<SetupCommand>{push, SetupCommand::Initialize},
                  "failed firmware initialization does not publish DONE or hide its original error");
            setup_commands.initialize_status = BC_STS_SUCCESS;
            check(DtsSetupHardware(&fixture.context, false) == BC_STS_SUCCESS &&
                  DtsGetHwInitSts() == BC_DIL_HWINIT_DONE &&
                  setup_commands.calls == std::vector<SetupCommand>{push, SetupCommand::Initialize,
                                                                    push, SetupCommand::Initialize} &&
                  setup_commands.states == std::vector<uint32_t>(4, BC_DIL_HWINIT_IN_PROGRESS),
                  "retry after initialization failure repeats the complete firmware transaction");
        }
        {
            RecoveryFixture fixture(device);
            SetupCommandScope commands;
            DtsSetHwInitSts(BC_DIL_HWINIT_DONE);
            check(DtsSetupHardware(&fixture.context, false) == BC_STS_SUCCESS &&
                  DtsGetHwInitSts() == BC_DIL_HWINIT_DONE && setup_commands.calls.empty() &&
                  run.control_calls == 0,
                  "completed initialization remains a no-command fast path unless forced");
            check(DtsSetupHardware(&fixture.context, true) == BC_STS_SUCCESS &&
                  DtsGetHwInitSts() == BC_DIL_HWINIT_DONE &&
                  setup_commands.calls == std::vector<SetupCommand>{push, SetupCommand::Initialize} &&
                  setup_commands.states == std::vector<uint32_t>(2, BC_DIL_HWINIT_IN_PROGRESS),
                  "forced setup replaces a completed initialization with a complete successful transaction");
        }
        for (const bool forced : {false, true}) {
            RecoveryFixture fixture(device);
            SetupCommandScope commands;
            DtsSetHwInitSts(BC_DIL_HWINIT_IN_PROGRESS);
            check(DtsSetupHardware(&fixture.context, forced) == BC_STS_BUSY &&
                  DtsGetHwInitSts() == BC_DIL_HWINIT_IN_PROGRESS &&
                  setup_commands.calls.empty() && run.control_calls == 0,
                  "an in-progress initializer remains BUSY without firmware commands or state clearing");
        }
        for (const bool forced : {false, true}) {
            RecoveryFixture fixture(device);
            SetupCommandScope commands;
            DtsSetHwInitSts(UINT32_MAX);
            check(DtsSetupHardware(&fixture.context, forced) == BC_STS_ERROR &&
                  DtsGetHwInitSts() == UINT32_MAX && setup_commands.calls.empty() &&
                  run.control_calls == 0,
                  "unknown initialization state is rejected without firmware commands or state changes");
        }
    }
    {
        RecoveryFixture fixture(UINT32_MAX);
        SetupCommandScope commands;
        check(DtsSetupHardware(&fixture.context, false) == BC_STS_ERROR &&
              DtsGetHwInitSts() == BC_DIL_HWINIT_NOT_YET &&
              setup_commands.calls.empty() && run.control_calls == 0,
              "an unknown hardware type cannot poison initialization state");
        fixture.context.DevId = BC_PCI_DEVID_FLEA;
        check(DtsSetupHardware(&fixture.context, false) == BC_STS_SUCCESS &&
              DtsGetHwInitSts() == BC_DIL_HWINIT_DONE &&
              setup_commands.calls == std::vector<SetupCommand>{SetupCommand::FleaPush,
                                                                SetupCommand::Initialize} &&
              setup_commands.states == std::vector<uint32_t>(2, BC_DIL_HWINIT_IN_PROGRESS),
              "a supported retry after unknown hardware performs actual setup instead of false success");
    }
    for (const uint32_t state : {BC_DIL_HWINIT_IN_PROGRESS, BC_DIL_HWINIT_DONE}) {
        for (const bool forced : {false, true}) {
            RecoveryFixture fixture(UINT32_MAX);
            SetupCommandScope commands;
            DtsSetHwInitSts(state);
            check(DtsSetupHardware(&fixture.context, forced) == BC_STS_ERROR &&
                  DtsGetHwInitSts() == state && setup_commands.calls.empty() &&
                  run.control_calls == 0,
                  "unknown hardware validation leaves another initializer's incoming state untouched");
        }
    }
}

static std::vector<RecoveryStage> expected_recovery_stages(uint32_t device)
{
    return {
        RecoveryStage::Setup,
        RecoveryStage::OutputFlush,
        RecoveryStage::Open,
        RecoveryStage::RingFlush,
        RecoveryStage::MetadataFlush,
        device == BC_PCI_DEVID_FLEA ? RecoveryStage::FleaColor :
                                      RecoveryStage::LinkColor,
        RecoveryStage::Start,
        RecoveryStage::Capture,
    };
}

static void check_recovery_owner_and_admission(DTS_LIB_CONTEXT *context)
{
    const int lock_status = pthread_mutex_trylock(&context->thLock);
    check(lock_status == 0, "recovery stage does not retain thLock across its barrier");
    if (lock_status == 0) {
        check(context->controlOwner == DTS_CONTROL_RECOVERY &&
              context->controlDepth == 1 &&
              !pthread_equal(context->controlThread, pthread_self()),
              "one worker owns the complete recovery transaction");
        pthread_mutex_unlock(&context->thLock);
    }

    const unsigned controls = run.control_calls;
    const uint32_t queued = context->circBuf.busySize;
    uint8_t bytes[sizeof(packet) + 4] = {};
    std::memcpy(bytes, packet, sizeof(packet));
    BC_INPUT_FORMAT format = {};
    BC_SCALING_PARAMS scale = {};
    BC_DTS_PROC_OUT output = {};

    check(DtsProcInput(context, bytes, sizeof(packet), 100000, false) == BC_STS_BUSY,
          "input arriving after the recovery latch returns BUSY");
    check(DtsProcOutputNoCopy(context, 0, &output) == BC_STS_BUSY,
          "output arriving after the recovery latch returns BUSY");
    check(context->circBuf.busySize == queued,
          "recovery-rejected input leaves the ring unchanged");
    check(DtsSetupHardware(context, true) == BC_STS_BUSY &&
          DtsOpenDecoder(context, 0) == BC_STS_BUSY &&
          DtsStartDecoder(context) == BC_STS_BUSY &&
          DtsStopDecoder(context) == BC_STS_BUSY &&
          DtsCloseDecoder(context) == BC_STS_BUSY,
          "open/start/stop/close/setup calls are BUSY throughout recovery");
    check(DtsPauseDecoder(context) == BC_STS_BUSY &&
          DtsResumeDecoder(context) == BC_STS_BUSY &&
          DtsStartCaptureImmidiate(context, 0) == BC_STS_BUSY &&
          DtsStartCapture(context) == BC_STS_BUSY,
          "pause/resume/capture calls are BUSY throughout recovery");
    check(DtsFlushRxCapture(context, true) == BC_STS_BUSY &&
          DtsFlushInput(context, 2) == BC_STS_BUSY &&
          DtsSetVideoParams(context, BC_VID_ALGO_H264, false, false, false, 0) == BC_STS_BUSY &&
          DtsSetInputFormat(context, &format) == BC_STS_BUSY,
          "flush and format lifecycle calls are BUSY throughout recovery");
    check(DtsSetRateChange(context, 10000, 0) == BC_STS_BUSY &&
          DtsSetFFRate(context, 10000) == BC_STS_BUSY &&
          DtsSetSkipPictureMode(context, 0) == BC_STS_BUSY &&
          DtsSetIFrameTrickMode(context) == BC_STS_BUSY &&
          DtsStepDecoder(context) == BC_STS_BUSY &&
          DtsDropPictures(context, 1) == BC_STS_BUSY &&
          DtsSetScaleParams(context, &scale) == BC_STS_BUSY,
          "trick-mode, drop, step and cached scaling controls are BUSY throughout recovery");
    uint64_t generation = 0;
    BC_STATUS recovery_status = BC_STS_ERROR;
    uint8_t in_progress = 0;
    uint32_t replay_bytes = 0;
    check(DtsGetRecoveryStatus(context, &generation, &recovery_status,
                               &in_progress, &replay_bytes) == BC_STS_SUCCESS &&
          generation == 1 && recovery_status == BC_STS_SUCCESS && in_progress,
          "recovery getter reports the bounded in-progress epoch");
    check(run.control_calls == controls,
          "recovery-rejected lifecycle calls issue no firmware command");
}

static void run_successful_recovery(uint32_t device)
{
    RecoveryFixture fixture(device);
    if (device == BC_PCI_DEVID_FLEA)
        fixture.context.activeFleaScaleWord = active_scale_word;
    recovery.block = true;
    const std::vector<RecoveryStage> expected = expected_recovery_stages(device);
    std::thread worker([&] {
        worker_thread = true;
        txThreadProc(&fixture.context);
        worker_thread = false;
    });

    for (size_t i = 0; i < expected.size(); ++i) {
        {
            std::unique_lock<std::mutex> lock(gate);
            await(lock, [i] {
                return recovery.waiting && recovery.order.size() == i + 1;
            });
            check(recovery.current == expected[i],
                  "recovery reaches each restore stage in exact order");
        }
        check_recovery_owner_and_admission(&fixture.context);
        {
            std::lock_guard<std::mutex> lock(gate);
            recovery.released = i + 1;
            changed.notify_all();
        }
    }
    worker.join();

    check(recovery.order == expected,
          "resume issues Setup/Open/flush/metadata/color/Start/Capture once");
    check(fixture.context.controlOwner == DTS_CONTROL_NONE &&
          fixture.context.controlDepth == 0 && !fixture.context.recoveryPending &&
          fixture.context.recoveryResumeSeen &&
          fixture.context.recoveryStatus == BC_STS_SUCCESS &&
          recovery.status_calls == recovery.exit_status_call,
          "successful recovery releases once and does not replay a repeated RESUME level");
    check(fixture.context.State == BC_DEC_STATE_START &&
          fixture.context.circBuf.busySize == 0,
          "successful recovery restarts the decoder with the stale ring empty");
    if (device == BC_PCI_DEVID_FLEA)
        check(fixture.context.activeFleaScaleWord == 0,
              "successful non-scaled recovery leaves no stale Flea scale identity");
    check(fixture.context.LastPicNum == UINT32_MAX &&
          fixture.context.LastSessNum == UINT32_MAX &&
          fixture.context.EOSCnt == 0 && fixture.context.DrvStatusEOSCnt == 0 &&
          !fixture.context.bEOS &&
          fixture.context.PESConvParams.m_lStartCodeDataSize == 0 &&
          fixture.context.PESConvParams.m_bAddSpsPps,
          "recovery restores cached input metadata before restart");
    check(fixture.context.b422Mode ==
              (device == BC_PCI_DEVID_FLEA ? OUTPUT_MODE422_YUY2 : OUTPUT_MODE422_UYVY),
          "recovery preserves the cached Flea/Link color selection");
    check(DtsAcquireControl(&fixture.context, DTS_CONTROL_LIFECYCLE) == BC_STS_SUCCESS,
          "normal lifecycle admission resumes after successful recovery");
    DtsReleaseControl(&fixture.context);
}

static void test_scaled_flea_recovery_republishes_layout_identity()
{
    RecoveryFixture fixture(BC_PCI_DEVID_FLEA);
    /* Model the pool selected by an exact successful 1280-wide OPEN before
     * suspend.  Setup must invalidate only the active-channel identity: the
     * cached request is what the replacement OPEN submits again. */
    fixture.context.EnableScaling = active_scale_word;
    fixture.context.activeFleaScaleWord = active_scale_word;
    fixture.context.rxBufferWidth = active_scale_width;
    recovery.publish_open_scale = true;

    std::thread worker([&] {
        worker_thread = true;
        txThreadProc(&fixture.context);
        worker_thread = false;
    });
    worker.join();

    check(recovery.order == expected_recovery_stages(BC_PCI_DEVID_FLEA) &&
          fixture.context.recoveryStatus == BC_STS_SUCCESS,
          "exact scaled Flea recovery completes the normal replacement transaction");
    check(recovery.open_cached_scale == active_scale_word &&
          recovery.open_active_scale_before == 0 &&
          recovery.open_active_scale_after == active_scale_word,
          "hardware reset retains the cached scale request and successful replacement OPEN republishes its identity");
    check(recovery.capture_cached_scale == active_scale_word &&
          recovery.capture_active_scale == active_scale_word &&
          recovery.capture_buffer_width == active_scale_width &&
          fixture.context.EnableScaling == active_scale_word &&
          fixture.context.activeFleaScaleWord == active_scale_word &&
          fixture.context.rxBufferWidth == active_scale_width,
          "capture starts with the restored exact-scale identity and preserves its established buffer layout");
}

static void test_recovery_aborts_partial_fragment()
{
    RecoveryFixture fixture(BC_PCI_DEVID_FLEA);
    fixture.context.SingleThreadedAppMode = 1;
    fixture.context.PESConvParams.m_bAddSpsPps = false;
    const uint8_t first[] = {
        0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x21, 0xa0,
        0x19, 0x7f, 0x5e, 0x33,
    };
    const uint8_t tail[] = {0x42, 0x17, 0x8c, 0xe1};
    const uint32_t stale_before = fixture.context.circBuf.busySize;
    check(DtsProcInputFragment(&fixture.context,
                               const_cast<uint8_t *>(first), sizeof(first),
                               660000, false, DTS_INPUT_FRAGMENT_FIRST) ==
              BC_STS_SUCCESS &&
          fixture.context.inputFragmentActive &&
          fixture.context.circBuf.busySize > stale_before &&
          fixture.context.inputFragmentMdata == nullptr,
          "Flea suspend fixture stops between a real PES FIRST and LAST");
    const uint32_t partial_bytes = fixture.context.circBuf.busySize;
    const unsigned controls = run.control_calls;
    check(DtsSetupHardware(&fixture.context, true) == BC_STS_BUSY &&
          run.control_calls == controls &&
          fixture.context.inputFragmentActive &&
          fixture.context.circBuf.busySize == partial_bytes,
          "ordinary setup cannot use the recovery-only partial-fragment bypass");

    DtsObservePowerState(&fixture.context, BC_HW_SUSPEND);
    recovery.power_states = {BC_HW_RESUME};
    std::thread worker([&] {
        worker_thread = true;
        txThreadProc(&fixture.context);
        worker_thread = false;
    });
    worker.join();

    check(recovery.order == expected_recovery_stages(BC_PCI_DEVID_FLEA) &&
          fixture.context.recoveryStatus == BC_STS_SUCCESS &&
          fixture.context.State == BC_DEC_STATE_START,
          "same-thread recovery setup crosses the partial-fragment latch once");
    check(!fixture.context.inputFragmentActive &&
          fixture.context.inputFragmentMdata == nullptr &&
          fixture.context.circBuf.busySize == 0 &&
          fixture.context.MDPendHead == DTS_MDATA_PEND_LINK((&fixture.context)) &&
          fixture.context.MDPendTail == DTS_MDATA_PEND_LINK((&fixture.context)),
          "recovery discards every partial PES byte, latch and metadata owner");
    check(DtsProcInputFragment(&fixture.context,
                               const_cast<uint8_t *>(tail), sizeof(tail), 0,
                               false, DTS_INPUT_FRAGMENT_LAST) ==
              BC_STS_INV_ARG &&
          fixture.context.circBuf.busySize == 0,
          "post-resume stale LAST is rejected without restoring partial bytes");
    check(DtsProcInputFragment(&fixture.context,
                               const_cast<uint8_t *>(first), sizeof(first),
                               670000, false,
                               DTS_INPUT_FRAGMENT_FIRST |
                                   DTS_INPUT_FRAGMENT_LAST) ==
              BC_STS_SUCCESS &&
          !fixture.context.inputFragmentActive &&
          fixture.context.circBuf.busySize != 0,
          "a fresh complete AU is admitted after partial-fragment recovery");
}

static void test_recovery_failures()
{
    const std::vector<RecoveryStage> expected =
        expected_recovery_stages(BC_PCI_DEVID_FLEA);
    for (size_t failed = 0; failed < expected.size(); ++failed) {
        RecoveryFixture fixture(BC_PCI_DEVID_FLEA);
        fixture.context.activeFleaScaleWord = active_scale_word;
        recovery.fail_stage = static_cast<int>(failed);
        recovery.failure = BC_STS_IO_XFR_ERROR;
        std::thread worker([&] {
            worker_thread = true;
            txThreadProc(&fixture.context);
            worker_thread = false;
        });
        worker.join();
        const std::vector<RecoveryStage> prefix(expected.begin(), expected.begin() + failed + 1);
        check(recovery.order == prefix,
              "a recovery-stage error preserves the exact failing prefix and stops later commands");
        check(fixture.context.controlOwner == DTS_CONTROL_NONE &&
              fixture.context.controlDepth == 0 && !fixture.context.recoveryPending &&
              fixture.context.recoveryResumeSeen &&
              fixture.context.recoveryStatus == BC_STS_IO_XFR_ERROR,
              "failed recovery releases ownership and records the exact one-shot error");
        check(fixture.context.activeFleaScaleWord ==
                  (failed == 0 ? active_scale_word : 0),
              "recovery preserves scale identity until hardware reset succeeds and clears it before later failures");
        uint8_t input[sizeof(packet) + 4] = {};
        BC_DTS_PROC_OUT output = {};
        check(DtsAcquireControl(&fixture.context, DTS_CONTROL_LIFECYCLE) ==
                  BC_STS_IO_XFR_ERROR &&
              DtsProcInput(&fixture.context, input, sizeof(packet), 0, false) ==
                  BC_STS_IO_XFR_ERROR &&
              DtsProcOutputNoCopy(&fixture.context, 0, &output) ==
                  BC_STS_IO_XFR_ERROR,
              "failed recovery quarantines lifecycle, input and output with the exact error");
        check(run.dma_calls == 0,
              "failed recovery never drains queued bytes into TX DMA");
        check(DtsAcquireControl(&fixture.context, DTS_CONTROL_TEARDOWN) == BC_STS_SUCCESS &&
              fixture.context.controlDepth == 1 &&
              DtsAcquireControl(&fixture.context, DTS_CONTROL_TEARDOWN) == BC_STS_SUCCESS &&
              fixture.context.controlDepth == 2,
              "terminal teardown nesting retains an exact control depth after failure");
        DtsReleaseControl(&fixture.context);
        check(fixture.context.controlOwner == DTS_CONTROL_TEARDOWN &&
              fixture.context.controlDepth == 1,
              "nested teardown release cannot drop its outer owner");
        DtsReleaseControl(&fixture.context);
        const unsigned stop_calls = run.stop_calls;
        const unsigned close_calls = run.close_calls;
        check(DtsDeviceClose(&fixture.context) == BC_STS_SUCCESS &&
              run.release_calls == 1 && run.stop_calls == stop_calls &&
              run.close_calls == close_calls,
              "failed recovery permits one terminal handle retire without firmware STOP/CLOSE");
        if (expected[failed] == RecoveryStage::FleaColor)
            check(recovery.order.size() == failed + 1 &&
                  recovery.order.back() == RecoveryStage::FleaColor,
                  "an exact color-restore failure prevents START and CAPTURE");
    }
}

static void test_recovery_stage_pm_failure_is_one_shot()
{
    RecoveryFixture fixture(BC_PCI_DEVID_FLEA);
    recovery.fail_stage = 0;
    recovery.failure = BC_STS_PWR_MGMT;
    recovery.latch_pm_on_failure = true;
    std::thread worker([&] {
        worker_thread = true;
        txThreadProc(&fixture.context);
        worker_thread = false;
    });
    worker.join();
    uint64_t generation = 0;
    BC_STATUS status = BC_STS_ERROR;
    uint8_t active = 1;
    uint32_t replay_bytes = UINT32_MAX;
    check(recovery.order.size() == 1 &&
          recovery.order.front() == RecoveryStage::Setup &&
          fixture.context.recoveryStatus == BC_STS_PWR_MGMT &&
          !fixture.context.recoveryPending &&
          fixture.context.controlOwner == DTS_CONTROL_NONE,
          "a recovery-stage PWR failure is recorded once without re-latching pending");
    check(DtsGetRecoveryStatus(&fixture.context, &generation, &status,
                               &active, &replay_bytes) == BC_STS_SUCCESS &&
          status == BC_STS_PWR_MGMT && !active,
          "a recovery-stage PM failure publishes an inactive terminal epoch");
    check(run.dma_calls == 0 && recovery.order.size() == 1,
          "a PM-failed recovery issues no later recovery stage or TX command");
    check(DtsDeviceClose(&fixture.context) == BC_STS_SUCCESS &&
          run.release_calls == 1,
          "a PM-failed recovery remains terminally consumable");
}

static void test_link_recovery_open_is_one_shot()
{
	for (const BC_STATUS failure : {BC_STS_PWR_MGMT, BC_STS_IO_XFR_ERROR}) {
		RecoveryFixture fixture(BC_PCI_DEVID_LINK);
		recovery.open_status = failure;
		std::thread worker([&] {
			worker_thread = true;
			txThreadProc(&fixture.context);
			worker_thread = false;
		});
		worker.join();
		check(recovery.order.size() == 3 &&
		      recovery.order.back() == RecoveryStage::Open &&
		      recovery.open_calls == 1 &&
		      fixture.context.recoveryStatus == failure,
		      "Link replacement Open returns its exact first firmware error once");
		check(run.stop_calls == 0 && run.close_calls == 0,
		      "Link recovery Open failure issues no blind STOP/CLOSE repair");
	}
	{
		RecoveryFixture fixture(BC_PCI_DEVID_LINK);
		recovery.open_channel_id = 7;
		std::thread worker([&] {
			worker_thread = true;
			txThreadProc(&fixture.context);
			worker_thread = false;
		});
		worker.join();
		check(recovery.order.size() == 3 && recovery.open_calls == 1 &&
		      fixture.context.recoveryStatus == BC_STS_FW_CMD_ERR &&
		      run.stop_calls == 0 && run.close_calls == 0,
		      "Link recovery quarantines a nonzero replacement channel without retry cleanup");
	}
}

static void test_workerless_pending_epoch_quarantines_for_teardown()
{
    RecoveryFixture fixture(BC_PCI_DEVID_FLEA);
    DtsObservePowerState(&fixture.context, BC_HW_SUSPEND);
    uint64_t generation = 0;
    BC_STATUS status = BC_STS_ERROR;
    uint8_t active = 1;
    uint32_t replay_bytes = UINT32_MAX;
    check(DtsGetRecoveryStatus(&fixture.context, &generation, &status,
                               &active, &replay_bytes) == BC_STS_SUCCESS &&
          generation == 1 && status == BC_STS_PWR_MGMT && !active &&
          !fixture.context.recoveryPending,
          "a pending PM epoch with no TX worker becomes terminal quarantine");
    const unsigned stop_calls = run.stop_calls;
    const unsigned close_calls = run.close_calls;
    check(DtsDeviceClose(&fixture.context) == BC_STS_SUCCESS &&
          run.release_calls == 1 && run.stop_calls == stop_calls &&
          run.close_calls == close_calls,
          "workerless quarantine consumes once without replaying firmware cleanup");
}

static void test_recovery_owner_contention()
{
    RecoveryFixture fixture(BC_PCI_DEVID_FLEA);
    recovery.pause_owner_contention = true;
    check(DtsAcquireControl(&fixture.context, DTS_CONTROL_LIFECYCLE) == BC_STS_SUCCESS,
          "contention fixture owns lifecycle control before RESUME");
    std::thread worker([&] {
        worker_thread = true;
        txThreadProc(&fixture.context);
        worker_thread = false;
    });
    {
        std::unique_lock<std::mutex> lock(gate);
        await(lock, [] { return recovery.contention_waiting; });
    }
    DtsLock(&fixture.context);
    check(fixture.context.recoveryPending && fixture.context.recoveryResumeSeen &&
          fixture.context.recoveryGeneration == 1 && recovery.order.empty() &&
          fixture.context.controlOwner == DTS_CONTROL_LIFECYCLE,
          "owner contention retains the unattempted RESUME without issuing a stage");
    DtsUnLock(&fixture.context);
    DtsReleaseControl(&fixture.context);
    {
        std::lock_guard<std::mutex> lock(gate);
        recovery.release_contention = true;
        changed.notify_all();
    }
    worker.join();
    check(recovery.order == expected_recovery_stages(BC_PCI_DEVID_FLEA) &&
          !fixture.context.recoveryPending &&
          fixture.context.recoveryStatus == BC_STS_SUCCESS &&
          fixture.context.controlOwner == DTS_CONTROL_NONE,
          "owner contention retries once, while a claimed stage failure does not");
}

static void test_recovery_abort_stamp_precedes_input_drain()
{
    RecoveryFixture fixture(BC_PCI_DEVID_FLEA);
    recovery.pause_input_drain = true;
    recovery.power_states = {BC_HW_RESUME};
    check(DtsBeginOutputCall(&fixture.context) == BC_STS_SUCCESS,
          "pre-PM output call is admitted in the old epoch");
    DtsLock(&fixture.context);
    fixture.context.inputCalls = 1;
    DtsUnLock(&fixture.context);
    std::thread worker([&] {
        worker_thread = true;
        txThreadProc(&fixture.context);
        worker_thread = false;
    });
    {
        std::unique_lock<std::mutex> lock(gate);
        await(lock, [] { return recovery.input_drain_waiting; });
    }
    DtsLock(&fixture.context);
    check(fixture.context.controlOwner == DTS_CONTROL_RECOVERY &&
          fixture.context.recoveryGeneration == 1 &&
          fixture.context.recoveryCancelGeneration == 1,
          "recovery stamps the output-abort epoch before waiting for old input");
    DtsUnLock(&fixture.context);
    BC_DTS_PROC_OUT output = {};
    check(DtsPublishOutput(&fixture.context, &output, BC_STS_IO_USER_ABORT) ==
              BC_STS_BUSY && output.Ybuff == NULL && output.UVbuff == NULL,
          "an old no-buffer fetch abort during input drain is retryable BUSY");
    DtsLock(&fixture.context);
    fixture.context.inputCalls = 0;
    DtsUnLock(&fixture.context);
    {
        std::lock_guard<std::mutex> lock(gate);
        recovery.release_input_drain = true;
        changed.notify_all();
    }
    worker.join();
    check(recovery.order == expected_recovery_stages(BC_PCI_DEVID_FLEA),
          "recovery continues exactly once after the old input/output calls retire");
}

static void test_resume_only_epochs_rearm_after_running()
{
    RecoveryFixture fixture(BC_PCI_DEVID_FLEA);
    recovery.power_states = {BC_HW_RESUME, BC_HW_RUNNING, BC_HW_RESUME};
    std::thread worker([&] {
        worker_thread = true;
        txThreadProc(&fixture.context);
        worker_thread = false;
    });
    worker.join();
    std::vector<RecoveryStage> twice = expected_recovery_stages(BC_PCI_DEVID_FLEA);
    const std::vector<RecoveryStage> once = twice;
    twice.insert(twice.end(), once.begin(), once.end());
    check(recovery.order == twice && fixture.context.recoveryGeneration == 2 &&
          fixture.context.recoveryStatus == BC_STS_SUCCESS,
          "RUNNING re-arms a second RESUME-only epoch exactly once");
}

static void test_pending_recovery_does_not_resurrect_close()
{
    RecoveryFixture fixture(BC_PCI_DEVID_FLEA);
    recovery.pause_owner_contention = true;
    check(DtsAcquireControl(&fixture.context, DTS_CONTROL_LIFECYCLE) == BC_STS_SUCCESS,
          "close-wins fixture owns lifecycle control before RESUME");
    std::thread worker([&] {
        worker_thread = true;
        txThreadProc(&fixture.context);
        worker_thread = false;
    });
    {
        std::unique_lock<std::mutex> lock(gate);
        await(lock, [] { return recovery.contention_waiting; });
    }
    DtsLock(&fixture.context);
    fixture.context.State = BC_DEC_STATE_CLOSE;
    DtsUnLock(&fixture.context);
    DtsReleaseControl(&fixture.context);
    {
        std::lock_guard<std::mutex> lock(gate);
        recovery.release_contention = true;
        changed.notify_all();
    }
    worker.join();
    check(recovery.order.empty() && fixture.context.State == BC_DEC_STATE_CLOSE &&
          !fixture.context.recoveryPending &&
          fixture.context.recoveryStatus == BC_STS_PWR_MGMT,
	      "a close completed before recovery ownership is quarantined, never reopened");
	const unsigned stop_calls = run.stop_calls;
	const unsigned close_calls = run.close_calls;
	check(DtsDeviceClose(&fixture.context) == BC_STS_SUCCESS &&
	      run.release_calls == 1 && run.stop_calls == stop_calls &&
	      run.close_calls == close_calls,
	      "a close-won PM epoch permits terminal retirement without firmware replay");
}

static void test_recovery_open_failure_retires_shared_decoder_owner()
{
	RecoveryFixture fixture(BC_PCI_DEVID_FLEA);
	fixture.context.OpMode = DTS_PLAYBACK_MODE;
	fixture.globals.g_bDecOpened = true;
	fixture.globals.g_nProcID = fixture.context.ProcessID;
	recovery.fail_stage = 2; /* replacement Open */
	std::thread worker([&] {
		worker_thread = true;
		txThreadProc(&fixture.context);
		worker_thread = false;
	});
	worker.join();
	check(recovery.order.size() == 3 &&
	      recovery.order.back() == RecoveryStage::Open &&
	      fixture.context.State == BC_DEC_STATE_CLOSE &&
	      fixture.context.recoveryRetireDecoderOwner &&
	      fixture.globals.g_bDecOpened &&
	      fixture.globals.g_nProcID == fixture.context.ProcessID,
	      "replacement Open failure retains the pre-S3 shared owner until teardown");
	check(DtsDeviceClose(&fixture.context) == BC_STS_SUCCESS &&
	      run.release_calls == 1,
	      "replacement Open failure remains terminally consumable");
}

static void test_paused_recovery_resends_eos_before_restoring_pause()
{
	RecoveryFixture fixture(BC_PCI_DEVID_FLEA);
	fixture.context.State = BC_DEC_STATE_PAUSE;
	fixture.context.bEOSCheck = true;
	observe_recovery_eos_packetization = true;
	std::thread worker([&] {
		worker_thread = true;
		txThreadProc(&fixture.context);
		worker_thread = false;
	});
	worker.join();
	check(recovery.order == expected_recovery_stages(BC_PCI_DEVID_FLEA) &&
	      !observe_recovery_eos_packetization &&
	      recovery_eos_packetization_state == BC_DEC_STATE_START,
	      "paused recovery re-enqueues pending EOS while decoder admission is START");
	check(fixture.context.recoveryStatus == BC_STS_SUCCESS &&
	      fixture.context.State == BC_DEC_STATE_PAUSE &&
	      fixture.context.bEOSCheck && fixture.context.txBytesEnqueued != 0,
	      "paused recovery restores PAUSE only after the EOS transaction succeeds");
}

static void test_suspend_latches_admission_before_resume()
{
    RecoveryFixture fixture(BC_PCI_DEVID_FLEA);
    /* The production context has a live TX worker while S3 is pending. */
    fixture.context.htxThread = pthread_self();
    DtsObservePowerState(&fixture.context, BC_HW_SUSPEND);
    uint64_t generation = 0;
    BC_STATUS status = BC_STS_ERROR;
    uint8_t in_progress = 0;
    uint32_t replay_bytes = 0;
    check(DtsGetRecoveryStatus(&fixture.context, &generation, &status,
                               &in_progress, &replay_bytes) == BC_STS_SUCCESS &&
          generation == 1 && status == BC_STS_SUCCESS && in_progress,
          "first SUSPEND observation publishes an in-progress epoch");
    uint8_t input[sizeof(packet) + 4] = {};
    BC_DTS_PROC_OUT output = {};
    check(DtsProcInput(&fixture.context, input, sizeof(packet), 0, false) ==
              BC_STS_BUSY &&
          DtsProcOutputNoCopy(&fixture.context, 0, &output) == BC_STS_BUSY &&
          run.release_calls == 0,
	      "SUSPEND blocks input/output before the matching RESUME");
    recovery.power_states = {BC_HW_RESUME};
    std::thread worker([&] {
        worker_thread = true;
        txThreadProc(&fixture.context);
        worker_thread = false;
    });
    worker.join();
    check(recovery.order == expected_recovery_stages(BC_PCI_DEVID_FLEA) &&
          fixture.context.recoveryGeneration == 1,
          "RESUME consumes the already-latched SUSPEND epoch without incrementing twice");
}

static void test_persistent_suspend_allows_terminal_close()
{
	RecoveryFixture fixture(BC_PCI_DEVID_FLEA);
	fixture.context.htxThread = pthread_self();
	DtsObservePowerState(&fixture.context, BC_HW_SUSPEND);
	const unsigned stop_calls = run.stop_calls;
	const unsigned close_calls = run.close_calls;
	check(DtsDeviceClose(&fixture.context) == BC_STS_SUCCESS &&
	      !fixture.context.recoveryPending &&
	      fixture.context.recoveryStatus == BC_STS_PWR_MGMT &&
	      run.release_calls == 1 && recovery.order.empty(),
	      "terminal close cancels a persistent SUSPEND epoch and consumes once");
	check(run.stop_calls == stop_calls && run.close_calls == close_calls,
	      "persistent-SUSPEND teardown never replays firmware STOP/CLOSE");
}

static void test_pm_latch_blocks_link_auto_pause()
{
    RecoveryFixture fixture(BC_PCI_DEVID_LINK);
    fixture.context.SingleThreadedAppMode = 1;
    recovery.ready_count = 11;
    recovery.power_states = {BC_HW_SUSPEND, BC_HW_RESUME};
    std::thread worker([&] {
        worker_thread = true;
        txThreadProc(&fixture.context);
        worker_thread = false;
    });
    worker.join();
    check(recovery.order == expected_recovery_stages(BC_PCI_DEVID_LINK),
          "PM admission latch suppresses LINK auto-pause firmware mutation");
}

static void test_link_recovery_resets_eos_idle_count()
{
	RecoveryFixture fixture(BC_PCI_DEVID_LINK);
	fixture.context.bEOSCheck = true;
	fixture.context.bEOS = false;
	for (unsigned count = 0; count < BC_EOS_PIC_COUNT - 1; ++count)
		recovery.power_states.push_back(BC_HW_RUNNING);
	recovery.power_states.push_back(BC_HW_SUSPEND);
	recovery.power_states.push_back(BC_HW_RESUME);
	recovery.power_states.push_back(BC_HW_RUNNING);
	std::thread worker([&] {
		worker_thread = true;
		txThreadProc(&fixture.context);
		worker_thread = false;
	});
	worker.join();
	check(recovery.order == expected_recovery_stages(BC_PCI_DEVID_LINK) &&
	      fixture.context.recoveryStatus == BC_STS_SUCCESS,
	      "Link EOS idle-count fixture completes channel reconstruction");
	check(!fixture.context.bEOS && fixture.context.bEOSCheck &&
	      fixture.context.txBytesEnqueued != 0 && run.dma_calls == 0,
	      "first restored Link poll cannot complete EOS before its replayed drain reaches DMA");
}

static void test_recovery_retires_failed_output_release()
{
    {
        RecoveryFixture fixture(BC_PCI_DEVID_FLEA);
        fixture.context.ProcOutPending = 1;
        fixture.context.outputPhase = DTS_OUTPUT_RETIRE_ONLY;
        fixture.context.bMapOutBufDone = true;
        fixture.context.bMapOutBufDirty = true;
        std::thread worker([&] {
            worker_thread = true;
            txThreadProc(&fixture.context);
            worker_thread = false;
        });
        worker.join();
        check(recovery.order == expected_recovery_stages(BC_PCI_DEVID_FLEA) &&
              !fixture.context.ProcOutPending &&
              fixture.context.outputPhase == DTS_OUTPUT_IDLE &&
              !fixture.context.bMapOutBufDone && !fixture.context.bMapOutBufDirty,
              "reset capture cleanup acknowledges and retires a failed output repost");
        check(DtsBeginOutputCall(&fixture.context) == BC_STS_SUCCESS,
              "post-recovery output admission is not stuck behind RETIRE_ONLY");
        DtsFinishOutputCall(&fixture.context);
    }
    {
        RecoveryFixture fixture(BC_PCI_DEVID_FLEA);
        fixture.context.ProcOutPending = 1;
        fixture.context.outputPhase = DTS_OUTPUT_RETIRE_ONLY;
        fixture.context.bMapOutBufDone = true;
        fixture.context.bMapOutBufDirty = true;
        recovery.fail_stage = 1; /* destructive capture cleanup */
        std::thread worker([&] {
            worker_thread = true;
            txThreadProc(&fixture.context);
            worker_thread = false;
        });
        worker.join();
        check(recovery.order.size() == 2 &&
              recovery.order.back() == RecoveryStage::OutputFlush &&
              fixture.context.recoveryStatus == BC_STS_IO_XFR_ERROR,
              "unacknowledged capture cleanup quarantines without Open/START");
        check(DtsDeviceClose(&fixture.context) == BC_STS_SUCCESS &&
              run.release_calls == 1,
              "quarantine can terminally consume a caller-free RETIRE_ONLY handle");
    }
}

static void test_recovery_transaction()
{
    test_recovery_owner_contention();
    test_recovery_abort_stamp_precedes_input_drain();
    test_resume_only_epochs_rearm_after_running();
    test_pending_recovery_does_not_resurrect_close();
	test_recovery_open_failure_retires_shared_decoder_owner();
	test_paused_recovery_resends_eos_before_restoring_pause();
    test_suspend_latches_admission_before_resume();
    test_persistent_suspend_allows_terminal_close();
    test_pm_latch_blocks_link_auto_pause();
    test_link_recovery_resets_eos_idle_count();
    test_recovery_retires_failed_output_release();
    test_recovery_aborts_partial_fragment();
    run_successful_recovery(BC_PCI_DEVID_FLEA);
    run_successful_recovery(BC_PCI_DEVID_LINK);
    test_scaled_flea_recovery_republishes_layout_identity();
    test_recovery_failures();
    test_recovery_stage_pm_failure_is_one_shot();
    test_link_recovery_open_is_one_shot();
    test_workerless_pending_epoch_quarantines_for_teardown();
}

static void test_device_close_control_race()
{
    run = {};
    run.done = true;
    bc_dil_glob_s globals = {};
    bc_dil_glob_ptr = &globals;
    DTS_LIB_CONTEXT context = {};
    observed = &context;
    context.Sig = LIB_CTX_SIG;
    context.ProcessID = getpid();
    context.State = BC_DEC_STATE_START;
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&context.thLock, &attr);
    pthread_mutexattr_destroy(&attr);
    if (txBufInit(&context.circBuf, CIRC_TX_BUF_SIZE) != BC_STS_SUCCESS)
        std::abort();
    claim_control_after_close_check = true;
    check(DtsDeviceClose(&context) == BC_STS_BUSY &&
          !claim_control_after_close_check && context.stopInProgress,
          "DeviceClose recognizes control acquired after its initial check");
    check(!context.txThreadExit && !run.join_calls && !run.release_calls &&
          !run.stop_calls && !run.close_calls,
          "control-only BUSY leaves the other caller's worker and handle untouched");
    context.stopInProgress = false;
    context.txThreadExit = true;
    context.State = BC_DEC_STATE_STOP;
    context.stopOutcome = DTS_STOP_CONFIRMED;
    context.stopStatus = BC_STS_SUCCESS;
    context.stopHostCleanupDone = true;
    check(DtsStartDecoder(&context) == BC_STS_BUSY && !run.control_calls,
          "a terminally joined worker cannot be restarted through a retained handle");
    txBufFree(&context.circBuf);
    pthread_mutex_destroy(&context.thLock);
    observed = nullptr;
    bc_dil_glob_ptr = nullptr;
}

static void *retiring_tx_thread(void *)
{
	return NULL;
}

static void test_join_publishes_worker_retirement()
{
	DTS_LIB_CONTEXT context = {};
	observed = &context;
	context.Sig = LIB_CTX_SIG;
	context.recoveryStatus = BC_STS_SUCCESS;
	pthread_mutexattr_t attr;
	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&context.thLock, &attr);
	pthread_mutexattr_destroy(&attr);
	if (txBufInit(&context.circBuf, CIRC_TX_BUF_SIZE) != BC_STS_SUCCESS ||
		pthread_create(&context.htxThread, NULL, retiring_tx_thread, NULL) != 0)
		std::abort();

	observe_recovery_during_join = true;
	join_snapshot_getter_status = BC_STS_ERROR;
	join_snapshot_recovery_status = BC_STS_ERROR;
	join_snapshot_generation = 0;
	join_snapshot_active = 0;
	DtsJoinTxThread(&context);
	check(!observe_recovery_during_join &&
		join_snapshot_getter_status == BC_STS_SUCCESS &&
		join_snapshot_generation == 1 &&
		join_snapshot_recovery_status == BC_STS_SUCCESS &&
		join_snapshot_active,
		"recovery getter sees a pending epoch while the joined worker is still published");

	uint64_t generation = 0;
	BC_STATUS recovery_status = BC_STS_SUCCESS;
	uint8_t active = 1;
	uint32_t replay_bytes = 0;
	check(DtsGetRecoveryStatus(&context, &generation, &recovery_status,
			&active, &replay_bytes) == BC_STS_SUCCESS &&
		generation == 1 && recovery_status == BC_STS_PWR_MGMT && !active &&
		!context.recoveryPending && context.htxThread == 0,
		"worker retirement is published under the context lock before pending recovery becomes terminal");

	txBufFree(&context.circBuf);
	pthread_mutex_destroy(&context.thLock);
	observed = nullptr;
}

int main()
{
	test_setup_hardware_initialization_state();
	test_join_publishes_worker_retirement();
    test_device_close_control_race();
    test_flush_rearms_config_after_old_input_retires();
    test_send_admission();
    test_recovery_transaction();
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
