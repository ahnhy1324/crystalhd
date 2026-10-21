// SPDX-License-Identifier: LGPL-2.1-or-later
/* Hardware-free capture-buffer ownership tests.  Link the production public
 * and private library sections and intercept only their final ioctls and
 * teardown syscalls.  The mock deliberately accepts an ADD whose ioctl then
 * fails, matching the ownership ambiguity of a post-command copyout error.
 */
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <pthread.h>
#include <string>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <thread>
#include <unistd.h>

#include "7411d.h"
#include "libcrystalhd_fwcmds.h"
#include "libcrystalhd_if.h"
#include "libcrystalhd_int_if.h"
#include "libcrystalhd_priv.h"

extern bc_dil_glob_s *bc_dil_glob_ptr;

static unsigned checks;
static unsigned failures;
static const char *case_name = "setup";
static const uint32_t kLegacyYBytes = 1920U * 1090U;
static const uint32_t kLegacyNv12Bytes = kLegacyYBytes + kLegacyYBytes / 2U;
static const uint32_t kLegacyPacked422Bytes = kLegacyYBytes * 2U;
#ifdef CRYSTALHD_EXPERIMENTAL_SCALED_RX
static const uint32_t kScaledWidth = 1280U;
static const uint32_t kScaledPacked422Bytes = kScaledWidth * 1090U * 2U;
static const uint32_t kScaledOpenWord = 0x50050001U;
#endif

static void Check(bool condition, const char *message)
{
	++checks;
	if (!condition) {
		std::fprintf(stderr, "FAIL [%s]: %s\n", case_name, message);
		++failures;
	}
}

static size_t PoolSize(const DTS_LIB_CONTEXT &context)
{
	size_t count = 0;
	for (const BC_IOCTL_DATA *item = context.pIoDataFreeHd;
	     item && count <= BC_IOCTL_DATA_POOL_SIZE; item = item->next)
		++count;
	return count;
}

enum FailureKind {
	FAIL_NONE,
	FAIL_STATUS,
	FAIL_SYSCALL,
};

struct IoctlEvent {
	unsigned long command;
	size_t pool_size;
	unsigned owned_before;
	void *buffer;
	uint32_t buffer_size;
	uint32_t buffer_offset;
	uint32_t buffer_mode;
	uint32_t reserved;
	uint32_t start_threshold;
	uint32_t pause_threshold;
	uint32_t resume_threshold;
	uint32_t discard_only;
};

static const size_t kEventCapacity = 4096;
static IoctlEvent events[kEventCapacity];
static size_t event_count;
static bool event_overflow;
static DTS_LIB_CONTEXT *active_context;
static DTS_MPOOL_TYPE *active_pools;
static size_t active_pool_count;
static bool driver_owned[BC_RX_LIST_CNT];
static bool duplicate_add;
static unsigned add_calls;
static unsigned flush_calls;
static unsigned start_calls;
static unsigned release_calls;
static int fail_add_at;
static int fail_flush_at;
static int fail_start_at;
static FailureKind add_failure;
static FailureKind flush_failure;
static FailureKind start_failure;
static FailureKind release_failure;
static int close_error;

static bool release_seen;
static bool close_seen;
static unsigned close_calls;
static unsigned sequence_number;
static unsigned release_sequence;
static unsigned close_sequence;
static bool release_clears_ownership;
static pthread_mutex_t *observed_forbidden_lock;
static unsigned forbidden_lock_calls;
static std::mutex lifecycle_gate;
static std::condition_variable lifecycle_changed;
static bool delayed_lock_waiting;
static bool release_delayed_lock;
static bool pause_firmware_stop;
static bool firmware_stop_waiting;
static bool release_firmware_stop;
static thread_local unsigned delay_context_lock;
static unsigned firmware_stop_calls;
static unsigned firmware_close_calls;
static BC_STATUS firmware_close_status;

static bool release_tracking;
static void *release_outputs[BC_RX_LIST_CNT];
static void *release_pool_array;
static void *release_ioctl_pool[BC_IOCTL_DATA_POOL_SIZE];
static unsigned release_output_frees;
static unsigned release_ioctl_frees;
static bool release_pool_array_freed;
static bool output_freed_too_early;
static bool release_context_freed;
static unsigned shared_detach_calls;
static size_t fail_resize_size;
static int fail_resize_at;
static unsigned resize_allocations;
static void *resize_live_allocations[BC_RX_LIST_CNT + 4];
static unsigned resize_live_count;
static bool track_resize_allocations;
static void *tracked_old_outputs[BC_RX_LIST_CNT];
static unsigned tracked_old_frees;

static unsigned OwnedCount()
{
	unsigned count = 0;
	for (size_t index = 0; index < BC_RX_LIST_CNT; ++index)
		count += driver_owned[index] ? 1U : 0U;
	return count;
}

static int PoolIndex(const void *buffer)
{
	DTS_MPOOL_TYPE *pools = active_context ? active_context->Mpools : active_pools;
	const size_t count = active_context ? active_context->MpoolCnt : active_pool_count;
	for (size_t index = 0; index < count; ++index) {
		if (pools && pools[index].buff == buffer)
			return static_cast<int>(index);
	}
	return -1;
}

static void ClearOwnership()
{
	std::memset(driver_owned, 0, sizeof(driver_owned));
}

static void ResetMock(bool clear_ownership)
{
	event_count = 0;
	event_overflow = false;
	duplicate_add = false;
	add_calls = flush_calls = start_calls = release_calls = 0;
	fail_add_at = fail_flush_at = fail_start_at = -1;
	add_failure = flush_failure = start_failure = release_failure = FAIL_NONE;
	close_error = 0;
	firmware_close_status = BC_STS_SUCCESS;
	release_seen = close_seen = false;
	close_calls = 0;
	shared_detach_calls = 0;
	sequence_number = release_sequence = close_sequence = 0;
	release_clears_ownership = true;
	observed_forbidden_lock = nullptr;
	forbidden_lock_calls = 0;
	if (clear_ownership)
		ClearOwnership();
	fail_resize_size = 0;
	fail_resize_at = -1;
	resize_allocations = 0;
	resize_live_count = 0;
	track_resize_allocations = false;
	std::memset(resize_live_allocations, 0,
	            sizeof(resize_live_allocations));
	tracked_old_frees = 0;
	std::memset(tracked_old_outputs, 0, sizeof(tracked_old_outputs));
}

static void ResetLifecycleGate()
{
	std::lock_guard<std::mutex> lock(lifecycle_gate);
	delayed_lock_waiting = false;
	release_delayed_lock = false;
	pause_firmware_stop = false;
	firmware_stop_waiting = false;
	release_firmware_stop = false;
	firmware_stop_calls = 0;
	firmware_close_calls = 0;
}

static void WaitForDelayedLock()
{
	std::unique_lock<std::mutex> lock(lifecycle_gate);
	if (!lifecycle_changed.wait_for(lock, std::chrono::seconds(2),
	                                [] { return delayed_lock_waiting; }))
		std::abort();
}

static void ReleaseDelayedLock()
{
	std::lock_guard<std::mutex> lock(lifecycle_gate);
	release_delayed_lock = true;
	lifecycle_changed.notify_all();
}

static void WaitForFirmwareStop()
{
	std::unique_lock<std::mutex> lock(lifecycle_gate);
	if (!lifecycle_changed.wait_for(lock, std::chrono::seconds(2),
	                                [] { return firmware_stop_waiting; }))
		std::abort();
}

static void ReleaseFirmwareStop()
{
	std::lock_guard<std::mutex> lock(lifecycle_gate);
	release_firmware_stop = true;
	lifecycle_changed.notify_all();
}

static IoctlEvent *RecordEvent(unsigned long command)
{
	if (event_count == kEventCapacity) {
		event_overflow = true;
		return nullptr;
	}
	IoctlEvent *event = &events[event_count++];
	std::memset(event, 0, sizeof(*event));
	event->command = command;
	event->pool_size = active_context ? PoolSize(*active_context) : 0;
	event->owned_before = OwnedCount();
	return event;
}

static int ApplyFailure(FailureKind kind, BC_IOCTL_DATA *data,
			BC_STATUS status)
{
	if (kind == FAIL_STATUS) {
		data->RetSts = status;
		return 0;
	}
	if (kind == FAIL_SYSCALL) {
		/* Leave the response looking successful: the command may have run
		 * before the kernel failed to copy its response back to userspace. */
		data->RetSts = BC_STS_SUCCESS;
		errno = EIO;
		return -1;
	}
	return 0;
}

extern "C" int __wrap_ioctl(int fd, unsigned long command, ...)
{
	va_list arguments;
	va_start(arguments, command);
	BC_IOCTL_DATA *data = va_arg(arguments, BC_IOCTL_DATA *);
	va_end(arguments);

	Check(active_context != nullptr, "ioctl has an active fixture");
	Check(fd == 99, "capture ioctl uses the fixture descriptor");
	Check(data != nullptr, "capture ioctl supplies an envelope");
	if (!data)
		return -1;

	IoctlEvent *event = RecordEvent(command);
	data->RetSts = BC_STS_SUCCESS;

	switch (command) {
	case BCM_IOC_ADD_RXBUFFS: {
		const unsigned call = add_calls++;
		const int index = PoolIndex(data->u.RxBuffs.YuvBuff);
		if (event) {
			event->buffer = data->u.RxBuffs.YuvBuff;
			event->buffer_size = data->u.RxBuffs.YuvBuffSz;
			event->buffer_offset = data->u.RxBuffs.UVbuffOffset;
			event->buffer_mode = data->u.RxBuffs.b422Mode;
		}
		Check(index >= 0, "ADD references one configured output buffer");
		if (static_cast<int>(call) == fail_add_at &&
		    add_failure == FAIL_STATUS)
			return ApplyFailure(add_failure, data, BC_STS_BUSY);

		/* A syscall failure is intentionally ownership-ambiguous.  Model the
		 * dangerous side by admitting the current buffer before returning -1. */
		if (index >= 0) {
			if (driver_owned[index])
				duplicate_add = true;
			driver_owned[index] = true;
		}
		if (static_cast<int>(call) == fail_add_at)
			return ApplyFailure(add_failure, data, BC_STS_BUSY);
		return 0;
	}
	case BCM_IOC_FLUSH_RX_CAP: {
		const unsigned call = flush_calls++;
		if (event) {
			event->reserved = data->u.FlushRxCap.Rsrd;
			event->discard_only = data->u.FlushRxCap.bDiscardOnly;
		}
		if (static_cast<int>(call) == fail_flush_at)
			return ApplyFailure(flush_failure, data, BC_STS_IO_ERROR);
		if (!data->u.FlushRxCap.bDiscardOnly)
			ClearOwnership();
		return 0;
	}
	case BCM_IOC_START_RX_CAP: {
		const unsigned call = start_calls++;
		if (event) {
			event->reserved = data->u.RxCap.Rsrd;
			event->start_threshold = data->u.RxCap.StartDeliveryThsh;
			event->pause_threshold = data->u.RxCap.PauseThsh;
			event->resume_threshold = data->u.RxCap.ResumeThsh;
		}
		if (static_cast<int>(call) == fail_start_at)
			return ApplyFailure(start_failure, data, BC_STS_BUSY);
		return 0;
	}
	case BCM_IOC_RELEASE:
		++release_calls;
		release_seen = true;
		release_sequence = ++sequence_number;
		if (release_clears_ownership)
			ClearOwnership();
		return ApplyFailure(release_failure, data, BC_STS_BUSY);
	default:
		Check(false, "capture test received no unexpected ioctl");
		data->RetSts = BC_STS_ERROR;
		return 0;
	}
}

extern "C" int __real_pthread_mutex_lock(pthread_mutex_t *);
extern "C" int __wrap_pthread_mutex_lock(pthread_mutex_t *mutex)
{
	if (mutex == observed_forbidden_lock)
		++forbidden_lock_calls;
	if (delay_context_lock && active_context &&
	    mutex == &active_context->thLock && --delay_context_lock == 0) {
		std::unique_lock<std::mutex> lock(lifecycle_gate);
		delayed_lock_waiting = true;
		lifecycle_changed.notify_all();
		if (!lifecycle_changed.wait_for(lock, std::chrono::seconds(2),
		                                [] { return release_delayed_lock; }))
			std::abort();
	}
	return __real_pthread_mutex_lock(mutex);
}

extern "C" int __wrap_close(int fd)
{
	++close_calls;
	close_seen = true;
	close_sequence = ++sequence_number;
	Check(fd == 99, "teardown closes the fixture descriptor");
	Check(release_seen, "teardown releases the driver handle before close");
	Check(release_output_frees == 0,
	      "teardown closes before freeing output backing");
	if (close_error) {
		errno = close_error;
		return -1;
	}
	return 0;
}

extern "C" void __real_free(void *);
extern "C" void *__real_malloc(size_t);
extern "C" void *__wrap_malloc(size_t size)
{
	void *pointer;
	if (size == fail_resize_size) {
		const unsigned call = resize_allocations++;
		if (static_cast<int>(call) == fail_resize_at)
			return nullptr;
	}
	pointer = __real_malloc(size);
	if (track_resize_allocations && pointer && resize_live_count <
	    sizeof(resize_live_allocations) /
	        sizeof(resize_live_allocations[0]))
		resize_live_allocations[resize_live_count++] = pointer;
	return pointer;
}

extern "C" void __wrap_free(void *pointer)
{
	for (unsigned index = 0; pointer && index < resize_live_count; ++index) {
		if (pointer != resize_live_allocations[index])
			continue;
		resize_live_allocations[index] =
			resize_live_allocations[--resize_live_count];
		resize_live_allocations[resize_live_count] = nullptr;
		break;
	}
	for (size_t index = 0; pointer && index < BC_RX_LIST_CNT; ++index)
		if (pointer == tracked_old_outputs[index])
			++tracked_old_frees;
	if (release_tracking && pointer) {
		if (pointer == active_context) {
			release_context_freed = true;
			Check(active_context->DevHandle == -1,
			      "terminal cleanup consumes the descriptor even after close failure");
		}
		for (size_t index = 0; index < BC_RX_LIST_CNT; ++index) {
			if (pointer == release_outputs[index]) {
				++release_output_frees;
				if (!release_seen || !close_seen)
					output_freed_too_early = true;
			}
		}
		if (pointer == release_pool_array) {
			release_pool_array_freed = true;
			if (!release_seen || !close_seen)
				output_freed_too_early = true;
		}
		for (size_t index = 0; index < BC_IOCTL_DATA_POOL_SIZE; ++index) {
			if (pointer == release_ioctl_pool[index])
				++release_ioctl_frees;
		}
	}
	__real_free(pointer);
}

extern "C" int __wrap_shmdt(const void *)
{
	++shared_detach_calls;
	return 0;
}

extern "C" int __wrap_shmget(key_t, size_t, int)
{
	return 17;
}

extern "C" int __wrap_shmctl(int, int command, struct shmid_ds *buffer)
{
	if (command == IPC_STAT && buffer) {
		std::memset(buffer, 0, sizeof(*buffer));
		buffer->shm_nattch = 1;
	}
	return 0;
}

BC_STATUS DtsFWDecFlushChannel(HANDLE, uint32_t)
{
	return BC_STS_SUCCESS;
}

BC_STATUS DtsFWPauseVideo(HANDLE, uint32_t)
{
	return BC_STS_SUCCESS;
}

BC_STATUS DtsFWStopVideo(HANDLE device, uint32_t, bool force)
{
	++firmware_stop_calls;
	if (pause_firmware_stop) {
		std::unique_lock<std::mutex> lock(lifecycle_gate);
		firmware_stop_waiting = true;
		lifecycle_changed.notify_all();
		if (!lifecycle_changed.wait_for(lock, std::chrono::seconds(2),
		                                [] { return release_firmware_stop; }))
			std::abort();
	}
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
{
	++firmware_close_calls;
	return firmware_close_status;
}

void DumpInputSampleToFile(uint8_t *, uint32_t)
{
}

/* DtsReleaseInterface owns this call, but parser behavior is unrelated to
 * capture-buffer lifetime and no converter is installed in these fixtures. */
BC_STATUS DtsReleasePESConverter(HANDLE)
{
	return BC_STS_SUCCESS;
}

struct Fixture {
	bc_dil_glob_s globals;
	DTS_LIB_CONTEXT context;
	BC_IOCTL_DATA ioctl_pool[BC_IOCTL_DATA_POOL_SIZE];
	DTS_MPOOL_TYPE pools[BC_RX_LIST_CNT];
	uint8_t buffers[BC_RX_LIST_CNT][1];
	bool heap_buffers;

	explicit Fixture(uint32_t device,
	                 uint32_t pool_count = BC_RX_LIST_CNT,
	                 bool replaceable = false)
		: heap_buffers(replaceable)
	{
		if (pool_count > BC_RX_LIST_CNT)
			std::abort();
		std::memset(&globals, 0, sizeof(globals));
		std::memset(&context, 0, sizeof(context));
		std::memset(ioctl_pool, 0, sizeof(ioctl_pool));
		std::memset(pools, 0, sizeof(pools));
		std::memset(buffers, 0, sizeof(buffers));

		bc_dil_glob_ptr = &globals;
		active_context = &context;
		active_pools = pools;
		active_pool_count = pool_count;
		context.Sig = LIB_CTX_SIG;
		context.DevHandle = 99;
		context.DevId = device;
		context.ProcessID = getpid();
		context.State = BC_DEC_STATE_START;
		context.CfgFlags = BC_MPOOL_INCL_YUV_BUFFS | BC_ADDBUFF_MOVE;
		context.MpoolCnt = pool_count;
		context.Mpools = pools;
		context.rxBufferWidth = 1920;
		for (size_t index = 0; index < pool_count; ++index) {
			pools[index].type = BC_MEM_DEC_YUVBUFF |
			                    BC_MEM_USER_MODE_ALLOC;
			/* The mock never DMA-writes these one-byte sentinels.  Keep the
			 * production allocation extent in metadata so buffer-size
			 * admission is exercised without allocating a 64 MiB fixture. */
			pools[index].sz = kLegacyPacked422Bytes;
			pools[index].buff = replaceable ?
				static_cast<uint8_t *>(__real_malloc(1)) : buffers[index];
			if (!pools[index].buff)
				std::abort();
		}
		for (size_t index = 0; index < BC_IOCTL_DATA_POOL_SIZE; ++index) {
			ioctl_pool[index].next = context.pIoDataFreeHd;
			context.pIoDataFreeHd = &ioctl_pool[index];
		}
		pthread_mutexattr_t attributes;
		pthread_mutexattr_init(&attributes);
		pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
		pthread_mutex_init(&context.thLock, &attributes);
		pthread_mutexattr_destroy(&attributes);
		ResetMock(true);
	}

	~Fixture()
	{
		Check(PoolSize(context) == BC_IOCTL_DATA_POOL_SIZE,
		      "fixture regains every ioctl envelope");
		if (heap_buffers)
			for (size_t index = 0; index < context.MpoolCnt; ++index)
				__real_free(context.Mpools[index].buff);
		pthread_mutex_destroy(&context.thLock);
		active_context = nullptr;
		active_pools = nullptr;
		active_pool_count = 0;
		bc_dil_glob_ptr = nullptr;
	}
};

static BC_STATUS StartCapture(Fixture &fixture, bool immediate)
{
	return immediate ? DtsStartCaptureImmidiate(&fixture.context, 0) :
	                   DtsStartCapture(&fixture.context);
}

static BC_STATUS ShutdownDecoder(Fixture &fixture, bool close_decoder)
{
	return close_decoder ? DtsCloseDecoder(&fixture.context) :
	                       DtsStopDecoder(&fixture.context);
}

static unsigned CountCommand(unsigned long command)
{
	unsigned count = 0;
	for (size_t index = 0; index < event_count; ++index)
		count += events[index].command == command ? 1U : 0U;
	return count;
}

static bool AddsUseUniqueConfiguredBuffers(size_t first, size_t count)
{
	bool seen[BC_RX_LIST_CNT] = {};
	const size_t pool_count = active_context ?
		active_context->MpoolCnt : active_pool_count;
	if (count != pool_count || first + count > event_count)
		return false;
	for (size_t offset = 0; offset < count; ++offset) {
		const IoctlEvent &event = events[first + offset];
		const int index = PoolIndex(event.buffer);
		if (event.command != BCM_IOC_ADD_RXBUFFS || index < 0 || seen[index])
			return false;
		seen[index] = true;
	}
	return true;
}

static bool PoolDepthIs(size_t first, size_t count, size_t expected)
{
	if (first + count > event_count)
		return false;
	for (size_t index = first; index < first + count; ++index) {
		if (events[index].pool_size != expected)
			return false;
	}
	return true;
}

static void CheckAddPayloads(size_t first, size_t count,
			     uint32_t output_mode = OUTPUT_MODE420,
			     uint32_t width = 1920U)
{
	const uint32_t y_size = width * 1090U;
	const bool packed = output_mode != OUTPUT_MODE420;
	bool valid = first + count <= event_count;
	for (size_t index = first; valid && index < first + count; ++index) {
		valid = events[index].command == BCM_IOC_ADD_RXBUFFS &&
		        events[index].buffer_mode == output_mode &&
		        events[index].buffer_size ==
		            (packed ? y_size * 2 : y_size + y_size / 2) &&
		        events[index].buffer_offset == (packed ? 0 : y_size);
	}
	Check(valid, packed ?
	      "ADD payloads describe the complete packed 4:2:2 output backing" :
	      "ADD payloads describe the complete NV12 output backing");
}

static void CheckStartPayload(const IoctlEvent &event, uint32_t device,
			      bool immediate,
			      uint32_t pool_count = BC_RX_LIST_CNT)
{
	const uint32_t pause_limit = pool_count > 2 ? pool_count - 2 : 1;
	const uint32_t pause = device == BC_PCI_DEVID_FLEA ?
	                       pause_limit :
	                       (pause_limit < PAUSE_DECODER_THRESHOLD ?
	                        pause_limit : PAUSE_DECODER_THRESHOLD);
	uint32_t resume = device == BC_PCI_DEVID_FLEA ?
	                  FLEA_RT_PU_THRESHOLD : RESUME_DECODER_THRESHOLD;
	if (resume >= pause)
		resume = pause > 1 ? pause / 2 : 1;
	Check(event.command == BCM_IOC_START_RX_CAP,
	      "mapping is followed by exactly one START command");
	Check(event.reserved == (immediate ? ST_CAP_IMMIDIATE : NO_PARAM),
	      "START carries the selected delivery mode");
	Check(event.start_threshold == RX_START_DELIVERY_THRESHOLD &&
	      event.pause_threshold == pause && event.resume_threshold == resume,
	      "START carries device-specific delivery, pause and resume thresholds");
}

static void TestConfiguredPoolAllocation()
{
	static const char environment[] =
		"CRYSTALHD_EXPERIMENTAL_RX_POOL_COUNT";
	const char *previous = std::getenv(environment);
	const bool was_set = previous != nullptr;
	const std::string saved = previous ? previous : "";
	struct PoolCase {
		const char *value;
		uint32_t expected;
		BC_OUTPUT_FORMAT output_mode;
		const char *description;
	};
	const PoolCase cases[] = {
		{nullptr, BC_RX_LIST_CNT, OUTPUT_MODE420,
		 "unset defaults to sixteen"},
		{"8", 8, OUTPUT_MODE422_YUY2,
		 "exact eight selects the reduced pool"},
		{"16", BC_RX_LIST_CNT, OUTPUT_MODE422_UYVY,
		 "exact sixteen selects the default pool"},
		{"8 ", BC_RX_LIST_CNT, OUTPUT_MODE420,
		 "whitespace is rejected"},
		{"7", BC_RX_LIST_CNT, OUTPUT_MODE422_YUY2,
		 "unsupported counts are rejected"},
	};

	for (const PoolCase &test : cases) {
		case_name = test.description;
		const int environment_status = test.value ?
			setenv(environment, test.value, 1) : unsetenv(environment);
		Check(environment_status == 0,
		      "test configures the experimental pool environment");

		DTS_LIB_CONTEXT context = {};
		context.OpMode = DTS_PLAYBACK_MODE;
		context.CfgFlags = BC_MPOOL_INCL_YUV_BUFFS;
		context.b422Mode = test.output_mode;
		const BC_STATUS status = DtsAllocMemPools(&context);
		Check(status == BC_STS_SUCCESS,
		      "pool configuration allocates the library memory pools");
		if (status != BC_STS_SUCCESS)
			continue;
		Check(context.MpoolCnt == test.expected,
		      "pool configuration selects only an accepted count");
		Check(context.rxBufferWidth == 1920,
		      "initial allocation commits the fixed fallback width");
		bool complete = context.Mpools != nullptr;
		for (uint32_t index = 0; complete && index < context.MpoolCnt;
		     ++index) {
			complete = context.Mpools[index].buff != nullptr &&
				(context.Mpools[index].type & BC_MEM_DEC_YUVBUFF) != 0 &&
				context.Mpools[index].sz == kLegacyPacked422Bytes;
		}
		Check(complete,
		      "every selected output-pool entry keeps the fixed fallback backing");
		DtsReleaseMemPools(&context);
	}

	const int restore_status = was_set ?
		setenv(environment, saved.c_str(), 1) : unsetenv(environment);
	case_name = "pool environment restoration";
	Check(restore_status == 0,
	      "pool allocation test restores the inherited environment");

	case_name = "invalid RX layout allocation";
	DTS_LIB_CONTEXT invalid = {};
	invalid.OpMode = DTS_PLAYBACK_MODE;
	invalid.CfgFlags = BC_MPOOL_INCL_YUV_BUFFS;
	invalid.b422Mode = OUTPUT_MODE_INVALID;
	const BC_STATUS invalid_status = DtsAllocMemPools(&invalid);
	Check(invalid_status == BC_STS_INV_ARG,
	      "invalid output mode is rejected before pool allocation");
	Check(invalid.pIoDataFreeHd == nullptr && invalid.pOutData == nullptr &&
	      invalid.Mpools == nullptr && invalid.MdataPoolPtr == nullptr,
	      "invalid layout leaves every allocation owner empty");
	if (invalid_status == BC_STS_INV_ARG)
		pthread_mutex_destroy(&invalid.thLock);
}

static void TestReducedPoolStarts()
{
	const uint32_t devices[] = {BC_PCI_DEVID_LINK, BC_PCI_DEVID_FLEA};
	const BC_OUTPUT_FORMAT output_modes[] = {
		OUTPUT_MODE420,
		OUTPUT_MODE422_YUY2,
	};
	char label[96];

	for (uint32_t device : devices) {
		for (BC_OUTPUT_FORMAT output_mode : output_modes) {
		 for (bool immediate : {false, true}) {
			std::snprintf(label, sizeof(label), "eight-buffer %s %s %s START",
			              device == BC_PCI_DEVID_FLEA ? "FLEA" : "LINK",
			              output_mode == OUTPUT_MODE420 ? "NV12" : "YUY2",
			              immediate ? "immediate" : "ordinary");
			case_name = label;
			Fixture fixture(device, 8);
			fixture.context.b422Mode = output_mode;
			Check(StartCapture(fixture, immediate) == BC_STS_SUCCESS,
			      "reduced-pool capture start succeeds");
			Check(event_count == 9 &&
			      CountCommand(BCM_IOC_ADD_RXBUFFS) == 8 &&
			      CountCommand(BCM_IOC_START_RX_CAP) == 1,
			      "reduced start registers eight buffers then starts once");
			Check(AddsUseUniqueConfiguredBuffers(0, 8) &&
			      !duplicate_add && OwnedCount() == 8,
			      "reduced start gives the driver eight unique addresses");
			Check(fixture.context.bMapOutBufDone &&
			      fixture.context.bMapOutBufDirty,
			      "reduced start retains complete driver ownership");
			CheckAddPayloads(0, 8, output_mode);
			CheckStartPayload(events[8], device, immediate, 8);
		}
		}
	}
}

#ifdef CRYSTALHD_EXPERIMENTAL_SCALED_RX
static void TrackOldOutputs(const Fixture &fixture)
{
	std::memset(tracked_old_outputs, 0, sizeof(tracked_old_outputs));
	tracked_old_frees = 0;
	for (size_t index = 0; index < fixture.context.MpoolCnt; ++index)
		tracked_old_outputs[index] = fixture.context.Mpools[index].buff;
}

static bool PoolHasLayout(const Fixture &fixture, uint32_t bytes,
			  const void *const *not_these = nullptr)
{
	for (size_t index = 0; index < fixture.context.MpoolCnt; ++index) {
		const DTS_MPOOL_TYPE &pool = fixture.context.Mpools[index];
		if (!pool.buff || pool.sz != bytes ||
		    !(pool.type & BC_MEM_DEC_YUVBUFF) ||
		    (not_these && pool.buff == not_these[index]))
			return false;
	}
	return true;
}

static void TestScaledPoolStarts()
{
	struct ScaledCase {
		uint32_t count;
		bool immediate;
		bool software_uyvy;
	};
	const ScaledCase cases[] = {
		{BC_RX_LIST_CNT, false, false},
		{8, true, true},
	};
	char label[96];

	for (const ScaledCase &test : cases) {
		std::snprintf(label, sizeof(label),
		              "scaled %u-buffer %s%s START", test.count,
		              test.immediate ? "immediate" : "ordinary",
		              test.software_uyvy ? " UYVY request" : "");
		case_name = label;
		Fixture fixture(BC_PCI_DEVID_FLEA, test.count, true);
		fixture.context.b422Mode = OUTPUT_MODE422_YUY2;
		fixture.context.softwareUyvy = test.software_uyvy;
		fixture.context.activeFleaScaleWord = kScaledOpenWord;
		void *old[BC_RX_LIST_CNT] = {};
		for (size_t index = 0; index < test.count; ++index)
			old[index] = fixture.context.Mpools[index].buff;
		TrackOldOutputs(fixture);

		Check(StartCapture(fixture, test.immediate) == BC_STS_SUCCESS,
		      "qualified scaled capture starts");
		Check(fixture.context.rxBufferWidth == kScaledWidth &&
		      PoolHasLayout(fixture, kScaledPacked422Bytes, old),
		      "qualified start atomically replaces every fixed buffer");
		Check(fixture.context.Mpools[0].buff[0] == 0 &&
		      fixture.context.Mpools[0].buff[kScaledPacked422Bytes - 1] == 0,
		      "replacement backing is zeroed across its complete extent");
		Check(tracked_old_frees == test.count,
		      "successful replacement frees every retired fixed buffer");
		Check(event_count == test.count + 1 &&
		      CountCommand(BCM_IOC_ADD_RXBUFFS) == test.count &&
		      CountCommand(BCM_IOC_START_RX_CAP) == 1,
		      "qualified start submits the complete scaled set then START");
		Check(AddsUseUniqueConfiguredBuffers(0, test.count),
		      "scaled ADDs use each replacement address exactly once");
		CheckAddPayloads(0, test.count, OUTPUT_MODE422_YUY2,
		                 kScaledWidth);
		CheckStartPayload(events[test.count], BC_PCI_DEVID_FLEA,
		                  test.immediate, test.count);
		Check(DtsUnmapYUVBuffs(&fixture.context) == BC_STS_SUCCESS,
		      "scaled mapping can be retired cleanly");
	}
}

static void TestScaledPoolFallbacks()
{
	struct FallbackCase {
		uint32_t device;
		uint32_t scale_word;
		BC_OUTPUT_FORMAT mode;
		bool addbuff_move;
		const char *name;
	};
	const FallbackCase cases[] = {
		{BC_PCI_DEVID_LINK, kScaledOpenWord, OUTPUT_MODE422_YUY2, true,
		 "LINK scaled-word fallback"},
		{BC_PCI_DEVID_FLEA, 0, OUTPUT_MODE422_YUY2, true,
		 "FLEA unscaled fallback"},
		{BC_PCI_DEVID_FLEA, 0x28028001U, OUTPUT_MODE422_YUY2, true,
		 "FLEA normalized 640 scale fallback"},
		{BC_PCI_DEVID_FLEA, 0x3c03c001U, OUTPUT_MODE422_YUY2, true,
		 "FLEA normalized 960 scale fallback"},
		{BC_PCI_DEVID_FLEA, 0x5004ff01U, OUTPUT_MODE422_YUY2, true,
		 "FLEA nonexact scale fallback"},
		{BC_PCI_DEVID_FLEA, 0x50250201U, OUTPUT_MODE422_YUY2, true,
		 "FLEA normalized 1282 scale fallback"},
		{BC_PCI_DEVID_FLEA, kScaledOpenWord, OUTPUT_MODE420, true,
		 "FLEA planar fallback"},
		{BC_PCI_DEVID_FLEA, kScaledOpenWord, OUTPUT_MODE422_YUY2, false,
		 "FLEA non-move fallback"},
	};

	for (const FallbackCase &test : cases) {
		case_name = test.name;
		Fixture fixture(test.device, 8, true);
		fixture.context.b422Mode = test.mode;
		fixture.context.activeFleaScaleWord = test.scale_word;
		if (!test.addbuff_move)
			fixture.context.CfgFlags &= ~BC_ADDBUFF_MOVE;
		void *old[BC_RX_LIST_CNT] = {};
		for (size_t index = 0; index < fixture.context.MpoolCnt; ++index)
			old[index] = fixture.context.Mpools[index].buff;
		TrackOldOutputs(fixture);

		Check(DtsMapYUVBuffs(&fixture.context) == BC_STS_SUCCESS,
		      "unqualified layout maps with the fixed fallback");
		Check(fixture.context.rxBufferWidth == 1920 &&
		      PoolHasLayout(fixture, kLegacyPacked422Bytes) &&
		      tracked_old_frees == 0,
		      "fallback preserves every fixed allocation");
		bool same = true;
		for (size_t index = 0; index < fixture.context.MpoolCnt; ++index)
			same &= fixture.context.Mpools[index].buff == old[index];
		Check(same, "fallback preserves every fixed address");
		CheckAddPayloads(0, fixture.context.MpoolCnt, test.mode);
		Check(DtsUnmapYUVBuffs(&fixture.context) == BC_STS_SUCCESS,
		      "fallback mapping retires cleanly");
	}
}

static void TestScaledPoolAllocationFailures()
{
	const int failures_at[] = {-2, 0, 7, 15};
	for (int ordinal : failures_at) {
		case_name = ordinal == -2 ? "scaled metadata allocation failure" :
		            "scaled backing allocation failure";
		Fixture fixture(BC_PCI_DEVID_FLEA, BC_RX_LIST_CNT, true);
		fixture.context.b422Mode = OUTPUT_MODE422_YUY2;
		fixture.context.activeFleaScaleWord = kScaledOpenWord;
		void *old[BC_RX_LIST_CNT] = {};
		for (size_t index = 0; index < BC_RX_LIST_CNT; ++index)
			old[index] = fixture.context.Mpools[index].buff;
		TrackOldOutputs(fixture);
		fail_resize_size = ordinal == -2 ?
			BC_RX_LIST_CNT * sizeof(uint8_t *) : kScaledPacked422Bytes;
		fail_resize_at = ordinal == -2 ? 0 : ordinal;
		track_resize_allocations = true;

		const BC_STATUS failed = DtsMapYUVBuffs(&fixture.context);
		track_resize_allocations = false;
		Check(failed == BC_STS_INSUFF_RES,
		      "staging allocation failure is propagated");
		bool unchanged = fixture.context.rxBufferWidth == 1920;
		for (size_t index = 0; index < BC_RX_LIST_CNT; ++index)
			unchanged &= fixture.context.Mpools[index].buff == old[index] &&
			             fixture.context.Mpools[index].sz ==
			                 kLegacyPacked422Bytes;
		Check(unchanged && tracked_old_frees == 0,
		      "failed staging leaves all old backing and metadata intact");
		Check(event_count == 0 && !fixture.context.bMapOutBufDone &&
		      !fixture.context.bMapOutBufDirty,
		      "failed staging sends no ioctl and borrows no DMA ownership");
		Check(resize_live_count == 0,
		      "failed staging releases every temporary allocation");

		ResetMock(true);
		TrackOldOutputs(fixture);
		Check(DtsMapYUVBuffs(&fixture.context) == BC_STS_SUCCESS,
		      "allocation failure is retryable");
		Check(fixture.context.rxBufferWidth == kScaledWidth &&
		      PoolHasLayout(fixture, kScaledPacked422Bytes, old) &&
		      tracked_old_frees == BC_RX_LIST_CNT,
		      "retry commits exactly one complete scaled replacement");
		Check(DtsUnmapYUVBuffs(&fixture.context) == BC_STS_SUCCESS,
		      "retried scaled mapping retires cleanly");
	}
}

static void TestScaledPoolOwnershipAndRestart()
{
	case_name = "mapped layout transition barrier";
	Fixture fixture(BC_PCI_DEVID_FLEA, 8, true);
	fixture.context.b422Mode = OUTPUT_MODE422_YUY2;
	Check(DtsMapYUVBuffs(&fixture.context) == BC_STS_SUCCESS,
	      "fixed pool maps before a requested transition");
	fixture.context.activeFleaScaleWord = kScaledOpenWord;
	ResetMock(false);
	Check(DtsMapYUVBuffs(&fixture.context) == BC_STS_BUSY &&
	      event_count == 0 && fixture.context.rxBufferWidth == 1920,
	      "mapped backing cannot change layout");
	Check(DtsUnmapYUVBuffs(&fixture.context) == BC_STS_SUCCESS,
	      "mapped fixed backing retires before transition");

	ResetMock(true);
	TrackOldOutputs(fixture);
	Check(DtsMapYUVBuffs(&fixture.context) == BC_STS_SUCCESS &&
	      fixture.context.rxBufferWidth == kScaledWidth,
	      "retired fixed backing can transition to scaled layout");
	void *scaled[BC_RX_LIST_CNT] = {};
	for (size_t index = 0; index < fixture.context.MpoolCnt; ++index)
		scaled[index] = fixture.context.Mpools[index].buff;
	ResetMock(false);
	Check(DtsMapYUVBuffs(&fixture.context) == BC_STS_SUCCESS &&
	      event_count == 0,
	      "repeat map of the committed layout does not reallocate or re-add");
	Check(DtsUnmapYUVBuffs(&fixture.context) == BC_STS_SUCCESS,
	      "scaled mapping retires before unscaled reopen");

	fixture.context.activeFleaScaleWord = 0;
	ResetMock(true);
	TrackOldOutputs(fixture);
	Check(DtsMapYUVBuffs(&fixture.context) == BC_STS_SUCCESS &&
	      fixture.context.rxBufferWidth == 1920 &&
	      PoolHasLayout(fixture, kLegacyPacked422Bytes, scaled),
	      "unscaled reopen restores the complete fixed layout");
	Check(tracked_old_frees == fixture.context.MpoolCnt,
	      "unscaled reopen retires each scaled allocation once");
	CheckAddPayloads(0, fixture.context.MpoolCnt, OUTPUT_MODE422_YUY2);
	Check(DtsUnmapYUVBuffs(&fixture.context) == BC_STS_SUCCESS,
	      "restored fixed mapping retires cleanly");

	const char *barriers[] = {
		"pending output call", "cancel wait", "returned output",
		"TX quiesce", "unresolved stop",
	};
	for (unsigned barrier = 0; barrier < 5; ++barrier) {
		case_name = barriers[barrier];
		Fixture leased(BC_PCI_DEVID_FLEA, 8, true);
		leased.context.b422Mode = OUTPUT_MODE422_YUY2;
		leased.context.activeFleaScaleWord = kScaledOpenWord;
		switch (barrier) {
		case 0: leased.context.ProcOutPending = TRUE; break;
		case 1: leased.context.CancelWaiting = TRUE; break;
		case 2: leased.context.outputPhase = DTS_OUTPUT_RETURNED; break;
		case 3: leased.context.txQuiescing = true; break;
		case 4:
			leased.context.stopOutcome = DTS_STOP_PENDING;
			break;
		}
		void *old[BC_RX_LIST_CNT] = {};
		for (size_t index = 0; index < leased.context.MpoolCnt; ++index)
			old[index] = leased.context.Mpools[index].buff;
		TrackOldOutputs(leased);
		fail_resize_size = kScaledPacked422Bytes;
		Check(DtsMapYUVBuffs(&leased.context) == BC_STS_BUSY &&
		      event_count == 0 && resize_allocations == 0 &&
		      leased.context.rxBufferWidth == 1920,
		      "active lifetime blocks replacement before allocation");
		bool unchanged = true;
		for (size_t index = 0; index < leased.context.MpoolCnt; ++index)
			unchanged &= leased.context.Mpools[index].buff == old[index];
		Check(unchanged && tracked_old_frees == 0,
		      "lifetime barrier preserves every fixed owner");
	}
}

static void TestScaledPoolPartialAddRetry()
{
	case_name = "scaled partial ADD rollback and retry";
	Fixture fixture(BC_PCI_DEVID_FLEA, 8, true);
	fixture.context.b422Mode = OUTPUT_MODE422_YUY2;
	fixture.context.activeFleaScaleWord = kScaledOpenWord;
	fail_add_at = 3;
	add_failure = FAIL_STATUS;
	Check(DtsMapYUVBuffs(&fixture.context) == BC_STS_BUSY,
	      "partial scaled ADD failure is propagated");
	Check(fixture.context.rxBufferWidth == kScaledWidth &&
	      PoolHasLayout(fixture, kScaledPacked422Bytes) &&
	      CountCommand(BCM_IOC_FLUSH_RX_CAP) == 1 &&
	      !fixture.context.bMapOutBufDone && !fixture.context.bMapOutBufDirty,
	      "rollback keeps the scaled pool but retires driver ownership");
	void *scaled[BC_RX_LIST_CNT] = {};
	for (size_t index = 0; index < fixture.context.MpoolCnt; ++index)
		scaled[index] = fixture.context.Mpools[index].buff;

	ResetMock(true);
	fail_resize_size = kScaledPacked422Bytes;
	Check(DtsMapYUVBuffs(&fixture.context) == BC_STS_SUCCESS,
	      "clean retry maps the retained scaled pool");
	bool same = resize_allocations == 0;
	for (size_t index = 0; index < fixture.context.MpoolCnt; ++index)
		same &= fixture.context.Mpools[index].buff == scaled[index];
	Check(same, "retry performs no second replacement allocation");
	Check(DtsUnmapYUVBuffs(&fixture.context) == BC_STS_SUCCESS,
	      "retried partial mapping retires cleanly");
}

static void TestScaledPoolValidationBarriers()
{
	case_name = "dirty unmap precedes resize";
	Fixture dirty(BC_PCI_DEVID_FLEA, 8, true);
	dirty.context.b422Mode = OUTPUT_MODE422_YUY2;
	dirty.context.activeFleaScaleWord = kScaledOpenWord;
	dirty.context.bMapOutBufDirty = true;
	fail_flush_at = 0;
	flush_failure = FAIL_SYSCALL;
	fail_resize_size = kScaledPacked422Bytes;
	track_resize_allocations = true;
	void *dirty_old[BC_RX_LIST_CNT] = {};
	for (size_t index = 0; index < dirty.context.MpoolCnt; ++index)
		dirty_old[index] = dirty.context.Mpools[index].buff;
	const BC_STATUS dirty_status = DtsMapYUVBuffs(&dirty.context);
	track_resize_allocations = false;
	Check(dirty_status == BC_STS_ERROR &&
	      event_count == 1 && events[0].command == BCM_IOC_FLUSH_RX_CAP &&
	      resize_allocations == 0 && resize_live_count == 0 &&
	      dirty.context.bMapOutBufDirty,
	      "ambiguous unmap failure stops before replacement allocation");
	bool unchanged = true;
	for (size_t index = 0; index < dirty.context.MpoolCnt; ++index)
		unchanged &= dirty.context.Mpools[index].buff == dirty_old[index];
	Check(unchanged, "ambiguous unmap preserves all quarantined backing");

	case_name = "duplicate pool owner rejection";
	Fixture duplicate(BC_PCI_DEVID_FLEA, 8, true);
	duplicate.context.b422Mode = OUTPUT_MODE422_YUY2;
	duplicate.context.activeFleaScaleWord = kScaledOpenWord;
	uint8_t *second = duplicate.context.Mpools[1].buff;
	duplicate.context.Mpools[1].buff = duplicate.context.Mpools[0].buff;
	fail_resize_size = kScaledPacked422Bytes;
	track_resize_allocations = true;
	const BC_STATUS duplicate_status = DtsMapYUVBuffs(&duplicate.context);
	track_resize_allocations = false;
	Check(duplicate_status == BC_STS_INV_ARG &&
	      event_count == 0 && resize_allocations == 0 &&
	      resize_live_count == 0,
	      "aliased owners are rejected before allocation or ioctl");
	duplicate.context.Mpools[1].buff = second;

	case_name = "foreign pool owner rejection";
	Fixture foreign(BC_PCI_DEVID_FLEA, 8, true);
	foreign.context.b422Mode = OUTPUT_MODE422_YUY2;
	foreign.context.activeFleaScaleWord = kScaledOpenWord;
	foreign.context.Mpools[3].type &= ~BC_MEM_USER_MODE_ALLOC;
	fail_resize_size = kScaledPacked422Bytes;
	track_resize_allocations = true;
	const BC_STATUS foreign_status = DtsMapYUVBuffs(&foreign.context);
	track_resize_allocations = false;
	Check(foreign_status == BC_STS_INV_ARG &&
	      event_count == 0 && resize_allocations == 0 &&
	      resize_live_count == 0,
	      "foreign backing is rejected before allocation or ioctl");
	foreign.context.Mpools[3].type |= BC_MEM_USER_MODE_ALLOC;
}
#else
static void TestScaledPoolBuildGate()
{
	case_name = "scaled RX build gate disabled";
	Fixture fixture(BC_PCI_DEVID_FLEA, 8);
	fixture.context.b422Mode = OUTPUT_MODE422_YUY2;
	fixture.context.activeFleaScaleWord = 0x50050001U;
	Check(DtsMapYUVBuffs(&fixture.context) == BC_STS_SUCCESS,
	      "ordinary build maps the fixed pool despite a scaled OPEN word");
	Check(fixture.context.rxBufferWidth == 1920,
	      "ordinary build keeps the fixed 1920-wide layout");
	CheckAddPayloads(0, fixture.context.MpoolCnt,
	                 OUTPUT_MODE422_YUY2);
	Check(DtsUnmapYUVBuffs(&fixture.context) == BC_STS_SUCCESS,
	      "ordinary build retires the fixed mapping cleanly");
}
#endif

static void TestDirectMapSuccess()
{
	case_name = "direct map success";
	Fixture fixture(BC_PCI_DEVID_LINK);
	Check(DtsMapYUVBuffs(&fixture.context) == BC_STS_SUCCESS,
	      "direct mapping succeeds");
	Check(event_count == BC_RX_LIST_CNT &&
	      CountCommand(BCM_IOC_ADD_RXBUFFS) == BC_RX_LIST_CNT,
	      "direct mapping submits exactly sixteen buffers");
	Check(AddsUseUniqueConfiguredBuffers(0, BC_RX_LIST_CNT),
	      "direct mapping submits sixteen unique addresses");
	Check(!duplicate_add && OwnedCount() == BC_RX_LIST_CNT,
	      "driver model owns each output address once");
	Check(fixture.context.bMapOutBufDone && fixture.context.bMapOutBufDirty,
	      "complete mapping records complete and driver-owned state");
	Check(PoolDepthIs(0, BC_RX_LIST_CNT, BC_IOCTL_DATA_POOL_SIZE - 1),
	      "each ADD borrows and returns only its own ioctl envelope");
	CheckAddPayloads(0, BC_RX_LIST_CNT);
	const size_t old_events = event_count;
	Check(DtsMapYUVBuffs(&fixture.context) == BC_STS_SUCCESS &&
	      event_count == old_events,
	      "completed mapping is idempotent");
}

static void TestAddBufferSizeContract()
{
	struct ModeCase {
		BC_OUTPUT_FORMAT mode;
		uint32_t required;
		uint32_t offset;
		const char *name;
	};
	const ModeCase modes[] = {
		{OUTPUT_MODE420, kLegacyNv12Bytes, kLegacyYBytes, "NV12"},
		{OUTPUT_MODE422_YUY2, kLegacyPacked422Bytes, 0, "YUY2"},
		{OUTPUT_MODE422_UYVY, kLegacyPacked422Bytes, 0, "UYVY"},
	};
	char label[96];

	for (const ModeCase &test : modes) {
		Fixture fixture(BC_PCI_DEVID_LINK);
		fixture.context.b422Mode = test.mode;
		const size_t initial_pool = PoolSize(fixture.context);
		const uint32_t accepted[] = {test.required, test.required + 1U};

		for (uint32_t size : accepted) {
			std::snprintf(label, sizeof(label), "%s ADD size %u accepted",
			              test.name, size);
			case_name = label;
			ResetMock(true);
			Check(DtsAddOutBuff(&fixture.context, fixture.pools[0].buff,
			                    size, fixture.pools[0].type) == BC_STS_SUCCESS,
			      "valid backing is accepted");
			Check(event_count == 1 && add_calls == 1 && flush_calls == 0,
			      "valid backing issues exactly one ADD");
			Check(events[0].command == BCM_IOC_ADD_RXBUFFS &&
			      events[0].buffer == fixture.pools[0].buff &&
			      events[0].buffer_size == test.required &&
			      events[0].buffer_offset == test.offset &&
			      events[0].buffer_mode == test.mode,
			      "ADD advertises the fixed registered extent, not caller excess");
			Check(PoolSize(fixture.context) == initial_pool,
			      "accepted ADD returns its ioctl envelope");
		}

		const uint32_t rejected[] = {0, test.required - 1U};
		for (uint32_t size : rejected) {
			std::snprintf(label, sizeof(label), "%s ADD size %u rejected",
			              test.name, size);
			case_name = label;
			ResetMock(true);
			Check(DtsAddOutBuff(&fixture.context, fixture.pools[0].buff,
			                    size, fixture.pools[0].type) == BC_STS_INV_ARG,
			      "undersized backing is rejected as an invalid contract");
			Check(event_count == 0 && add_calls == 0 && flush_calls == 0 &&
			      OwnedCount() == 0,
			      "rejected backing causes no ioctl or ownership mutation");
			Check(PoolSize(fixture.context) == initial_pool,
			      "rejected ADD does not borrow an ioctl envelope");
		}

		std::snprintf(label, sizeof(label), "%s null ADD rejected", test.name);
		case_name = label;
		ResetMock(true);
		Check(DtsAddOutBuff(&fixture.context,
		                    static_cast<uint8_t *>(nullptr), test.required,
		                    fixture.pools[0].type) == BC_STS_INV_ARG,
		      "null backing is rejected");
		Check(event_count == 0 && add_calls == 0 && OwnedCount() == 0 &&
		      PoolSize(fixture.context) == initial_pool,
		      "null backing causes no ioctl, ownership or envelope change");
	}

	case_name = "invalid output mode ADD rejected";
	Fixture fixture(BC_PCI_DEVID_LINK);
	fixture.context.b422Mode = OUTPUT_MODE_INVALID;
	Check(DtsAddOutBuff(&fixture.context, fixture.pools[0].buff,
	                    fixture.pools[0].sz,
	                    fixture.pools[0].type) == BC_STS_INV_ARG,
	      "invalid output mode is rejected");
	Check(event_count == 0 && add_calls == 0 && flush_calls == 0 &&
	      OwnedCount() == 0 &&
	      PoolSize(fixture.context) == BC_IOCTL_DATA_POOL_SIZE,
	      "invalid mode causes no ioctl, ownership or envelope change");

	case_name = "RX layout uint32 boundary";
	Fixture boundary(BC_PCI_DEVID_FLEA);
	boundary.context.b422Mode = OUTPUT_MODE422_YUY2;
	boundary.context.rxBufferWidth = 1970168U;
	ResetMock(true);
	Check(DtsAddOutBuff(&boundary.context, boundary.pools[0].buff,
	                    UINT32_MAX, boundary.pools[0].type) ==
	          BC_STS_SUCCESS &&
	      event_count == 1 && events[0].buffer_size == 4294966240U,
	      "largest uint32-representable packed layout remains exact");
	for (uint32_t width : {1970169U, UINT32_MAX}) {
		boundary.context.rxBufferWidth = width;
		ResetMock(true);
		Check(DtsAddOutBuff(&boundary.context, boundary.pools[0].buff,
		                    UINT32_MAX, boundary.pools[0].type) ==
		          BC_STS_INV_ARG &&
		      event_count == 0 && add_calls == 0,
		      "overflowing packed layout is rejected before ioctl");
	}
}

static void TestMapPreflightRejectsInvalidPool()
{
	struct ModeCase {
		BC_OUTPUT_FORMAT mode;
		uint32_t required;
		const char *name;
	};
	const ModeCase modes[] = {
		{OUTPUT_MODE420, kLegacyNv12Bytes, "NV12"},
		{OUTPUT_MODE422_YUY2, kLegacyPacked422Bytes, "YUY2"},
		{OUTPUT_MODE422_UYVY, kLegacyPacked422Bytes, "UYVY"},
	};
	const size_t invalid_indices[] = {0, 8};
	char label[112];

	for (const ModeCase &test : modes) {
		for (size_t invalid_index : invalid_indices) {
			std::snprintf(label, sizeof(label),
			              "%s pool preflight undersized index %zu",
			              test.name, invalid_index);
			case_name = label;
			Fixture fixture(BC_PCI_DEVID_LINK);
			fixture.context.b422Mode = test.mode;
			fixture.pools[invalid_index].sz = test.required - 1U;
			Check(DtsMapYUVBuffs(&fixture.context) == BC_STS_INV_ARG,
			      "an undersized pool entry rejects the whole map");
			Check(event_count == 0 && add_calls == 0 && flush_calls == 0,
			      "pool preflight rejects before every driver ioctl");
			Check(!fixture.context.bMapOutBufDone &&
			      !fixture.context.bMapOutBufDirty && OwnedCount() == 0,
			      "pool preflight leaves mapping ownership untouched");
			Check(PoolSize(fixture.context) == BC_IOCTL_DATA_POOL_SIZE,
			      "pool preflight preserves every ioctl envelope");
		}

		std::snprintf(label, sizeof(label),
		              "%s pool preflight null middle entry", test.name);
		case_name = label;
		Fixture fixture(BC_PCI_DEVID_LINK);
		fixture.context.b422Mode = test.mode;
		fixture.pools[8].buff = nullptr;
		Check(DtsMapYUVBuffs(&fixture.context) == BC_STS_INV_ARG,
		      "a null pool entry rejects the whole map");
		Check(event_count == 0 && add_calls == 0 && flush_calls == 0 &&
		      !fixture.context.bMapOutBufDone &&
		      !fixture.context.bMapOutBufDirty && OwnedCount() == 0,
		      "null preflight rejection has no driver or ownership side effect");
		Check(PoolSize(fixture.context) == BC_IOCTL_DATA_POOL_SIZE,
		      "null preflight rejection preserves every ioctl envelope");
	}

	case_name = "invalid output mode pool preflight";
	Fixture fixture(BC_PCI_DEVID_LINK);
	fixture.context.b422Mode = OUTPUT_MODE_INVALID;
	Check(DtsMapYUVBuffs(&fixture.context) == BC_STS_INV_ARG,
	      "invalid output mode rejects the whole map");
	Check(event_count == 0 && add_calls == 0 && flush_calls == 0 &&
	      !fixture.context.bMapOutBufDone &&
	      !fixture.context.bMapOutBufDirty && OwnedCount() == 0 &&
	      PoolSize(fixture.context) == BC_IOCTL_DATA_POOL_SIZE,
	      "invalid mode preflight has no driver or ownership side effect");
}

static void TestAddFailureMatrix()
{
	const int indices[] = {0, 1, 8, 15};
	const FailureKind kinds[] = {FAIL_STATUS, FAIL_SYSCALL};
	char label[80];
	for (FailureKind kind : kinds) {
		for (int failed_index : indices) {
			std::snprintf(label, sizeof(label), "ADD %s failure at %d",
			              kind == FAIL_STATUS ? "status" : "syscall",
			              failed_index);
			case_name = label;
			Fixture fixture(BC_PCI_DEVID_LINK);
			fail_add_at = failed_index;
			add_failure = kind;
			const BC_STATUS status = DtsMapYUVBuffs(&fixture.context);
			Check(status == (kind == FAIL_STATUS ? BC_STS_BUSY : BC_STS_ERROR),
			      "mapping preserves the original ADD failure after rollback");
			Check(add_calls == static_cast<unsigned>(failed_index + 1) &&
			      flush_calls == 1 &&
			      event_count == static_cast<size_t>(failed_index + 2),
			      "every ADD failure triggers one immediate rollback");
			Check(events[event_count - 1].command == BCM_IOC_FLUSH_RX_CAP &&
			      events[event_count - 1].discard_only == FALSE &&
			      events[event_count - 1].reserved == 0,
			      "rollback uses a destructive zero-reserved FLUSH payload");
			const unsigned accepted = static_cast<unsigned>(failed_index) +
			                          (kind == FAIL_SYSCALL ? 1U : 0U);
			Check(events[event_count - 1].owned_before == accepted,
			      "rollback covers the accepted prefix and ambiguous failed ADD");
			Check(!fixture.context.bMapOutBufDone &&
			      !fixture.context.bMapOutBufDirty && OwnedCount() == 0,
			      "successful rollback clears complete and dirty ownership state");
			Check(!duplicate_add &&
			      PoolDepthIs(0, event_count,
			                      BC_IOCTL_DATA_POOL_SIZE - 1),
			      "failed mapping neither duplicates buffers nor leaks envelopes");
		}
	}
}

static void TestRollbackFailureAndRetry()
{
	case_name = "rollback failure and retry";
	Fixture fixture(BC_PCI_DEVID_FLEA);
	fail_add_at = 8;
	add_failure = FAIL_SYSCALL;
	fail_flush_at = 0;
	flush_failure = FAIL_STATUS;
	Check(DtsMapYUVBuffs(&fixture.context) != BC_STS_SUCCESS,
	      "mapping reports a failed rollback");
	Check(!fixture.context.bMapOutBufDone &&
	      fixture.context.bMapOutBufDirty && OwnedCount() == 9,
	      "failed rollback retains partial ambiguous ownership as dirty");
	Check(event_count == 10 &&
	      events[9].command == BCM_IOC_FLUSH_RX_CAP &&
	      events[9].discard_only == FALSE,
	      "partial map attempts destructive rollback once");
	Check(PoolSize(fixture.context) == BC_IOCTL_DATA_POOL_SIZE,
	      "failed rollback returns every ioctl envelope");

	ResetMock(false);
	fail_flush_at = 0;
	flush_failure = FAIL_SYSCALL;
	Check(StartCapture(fixture, false) == BC_STS_ERROR,
	      "retry propagates a failed pre-map cleanup");
	Check(event_count == 1 && events[0].command == BCM_IOC_FLUSH_RX_CAP &&
	      add_calls == 0 && start_calls == 0,
	      "retry cleans dirty ownership before any ADD or START");
	Check(!fixture.context.bMapOutBufDone &&
	      fixture.context.bMapOutBufDirty && OwnedCount() == 9,
	      "ambiguous cleanup failure remains dirty and incomplete");

	ResetMock(false);
	Check(StartCapture(fixture, false) == BC_STS_SUCCESS,
	      "retry succeeds after confirmed dirty cleanup");
	Check(event_count == BC_RX_LIST_CNT + 2 &&
	      events[0].command == BCM_IOC_FLUSH_RX_CAP &&
	      events[0].discard_only == FALSE,
	      "successful retry begins with destructive cleanup");
	Check(AddsUseUniqueConfiguredBuffers(1, BC_RX_LIST_CNT) &&
	      events[BC_RX_LIST_CNT + 1].command == BCM_IOC_START_RX_CAP,
	      "retry registers sixteen unique buffers before one START");
	Check(!duplicate_add && OwnedCount() == BC_RX_LIST_CNT &&
	      fixture.context.bMapOutBufDone && fixture.context.bMapOutBufDirty,
	      "retry replaces ambiguous ownership with one complete mapping");
	Check(PoolDepthIs(0, event_count, BC_IOCTL_DATA_POOL_SIZE - 1),
	      "retry maps before borrowing the outer START envelope");
	CheckStartPayload(events[BC_RX_LIST_CNT + 1], BC_PCI_DEVID_FLEA, false);
}

static void TestRepeatedStartMappingFailures()
{
	char label[80];
	for (bool immediate : {false, true}) {
		std::snprintf(label, sizeof(label), "%s repeated mapping failures",
		              immediate ? "immediate" : "ordinary");
		case_name = label;
		Fixture fixture(BC_PCI_DEVID_LINK);
		for (unsigned attempt = 0; attempt < 12; ++attempt) {
			ResetMock(false);
			fail_add_at = 0;
			add_failure = FAIL_STATUS;
			Check(StartCapture(fixture, immediate) == BC_STS_BUSY,
			      "start preserves a transient mapping failure");
			Check(event_count == 2 &&
			      events[0].command == BCM_IOC_ADD_RXBUFFS &&
			      events[1].command == BCM_IOC_FLUSH_RX_CAP &&
			      start_calls == 0,
			      "failed start maps and rolls back without START");
			Check(PoolDepthIs(0, event_count,
			                      BC_IOCTL_DATA_POOL_SIZE - 1) &&
			      PoolSize(fixture.context) == BC_IOCTL_DATA_POOL_SIZE,
			      "more than eight failures cannot consume the START envelope pool");
			Check(!fixture.context.bMapOutBufDone &&
			      !fixture.context.bMapOutBufDirty && OwnedCount() == 0,
			      "each repeated failure returns to a clean unmapped state");
		}
	}
}

static void TestSuccessfulStarts()
{
	const uint32_t devices[] = {BC_PCI_DEVID_LINK, BC_PCI_DEVID_FLEA};
	char label[80];
	for (uint32_t device : devices) {
		for (bool immediate : {false, true}) {
			std::snprintf(label, sizeof(label), "%s %s START payload",
			              device == BC_PCI_DEVID_FLEA ? "FLEA" : "LINK",
			              immediate ? "immediate" : "ordinary");
			case_name = label;
			Fixture fixture(device);
			Check(StartCapture(fixture, immediate) == BC_STS_SUCCESS,
			      "capture start succeeds");
			Check(event_count == BC_RX_LIST_CNT + 1 &&
			      CountCommand(BCM_IOC_ADD_RXBUFFS) == BC_RX_LIST_CNT &&
			      CountCommand(BCM_IOC_START_RX_CAP) == 1,
			      "start registers sixteen buffers then issues one START");
			Check(AddsUseUniqueConfiguredBuffers(0, BC_RX_LIST_CNT) &&
			      !duplicate_add && OwnedCount() == BC_RX_LIST_CNT,
			      "start gives the driver sixteen unique addresses");
			Check(PoolDepthIs(0, event_count,
			                      BC_IOCTL_DATA_POOL_SIZE - 1),
			      "mapping precedes allocation of the outer START envelope");
			Check(fixture.context.bMapOutBufDone &&
			      fixture.context.bMapOutBufDirty,
			      "successful start retains complete driver ownership");
			CheckAddPayloads(0, BC_RX_LIST_CNT);
			CheckStartPayload(events[BC_RX_LIST_CNT], device, immediate);
		}
	}
}

static void TestConcurrentStarts()
{
	for (const bool winner_immediate : {false, true}) {
		case_name = winner_immediate ?
			"immediate start owns lifecycle" :
			"ordinary start owns lifecycle";
		Fixture fixture(BC_PCI_DEVID_FLEA);
		ResetLifecycleGate();
		BC_STATUS winner_status = BC_STS_ERROR;
		std::thread winner([&] {
			/* Stop after lifecycle admission but before the public body takes
			 * thLock. This proves the competing call observes the lease rather
			 * than depending on scheduler timing at a start barrier. */
			delay_context_lock = 2;
			winner_status = StartCapture(fixture, winner_immediate);
		});
		WaitForDelayedLock();

		const BC_STATUS competing_status =
			StartCapture(fixture, !winner_immediate);
		Check(competing_status == BC_STS_BUSY && event_count == 0,
		      "overlapping capture start is rejected before driver mutation");
		ReleaseDelayedLock();
		winner.join();

		Check(winner_status == BC_STS_SUCCESS,
		      "lifecycle owner completes its capture start");
		Check(CountCommand(BCM_IOC_ADD_RXBUFFS) == BC_RX_LIST_CNT &&
		      CountCommand(BCM_IOC_START_RX_CAP) == 1 &&
		      CountCommand(BCM_IOC_FLUSH_RX_CAP) == 0,
		      "winning start creates one registration set and one START");
		Check(event_count == BC_RX_LIST_CNT + 1 &&
		      AddsUseUniqueConfiguredBuffers(0, BC_RX_LIST_CNT),
		      "winning start registers sixteen unique buffers");

		Check(StartCapture(fixture, !winner_immediate) == BC_STS_SUCCESS,
		      "competing start succeeds when retried after lease retirement");
		Check(event_count == BC_RX_LIST_CNT + 2 &&
		      CountCommand(BCM_IOC_ADD_RXBUFFS) == BC_RX_LIST_CNT &&
		      CountCommand(BCM_IOC_START_RX_CAP) == 2,
		      "retry reuses one registration set and issues the second START");
		Check(events[BC_RX_LIST_CNT].command == BCM_IOC_START_RX_CAP &&
		      events[BC_RX_LIST_CNT + 1].command == BCM_IOC_START_RX_CAP &&
		      events[BC_RX_LIST_CNT].reserved !=
		          events[BC_RX_LIST_CNT + 1].reserved &&
		      (events[BC_RX_LIST_CNT].reserved == NO_PARAM ||
		       events[BC_RX_LIST_CNT + 1].reserved == NO_PARAM) &&
		      (events[BC_RX_LIST_CNT].reserved == ST_CAP_IMMIDIATE ||
		       events[BC_RX_LIST_CNT + 1].reserved == ST_CAP_IMMIDIATE),
		      "owner and retry retain their distinct START modes");
		Check(!duplicate_add && OwnedCount() == BC_RX_LIST_CNT &&
		      fixture.context.bMapOutBufDone && fixture.context.bMapOutBufDirty,
		      "overlap and retry leave one complete driver-owned map");
		Check(PoolDepthIs(0, event_count, BC_IOCTL_DATA_POOL_SIZE - 1) &&
		      PoolSize(fixture.context) == BC_IOCTL_DATA_POOL_SIZE,
		      "overlap and retry preserve the complete ioctl pool");
	}
}

static void TestCaptureLifecycleStates()
{
	const uint32_t devices[] = {BC_PCI_DEVID_LINK, BC_PCI_DEVID_FLEA};
	const uint32_t states[] = {
		BC_DEC_STATE_FLUSH,
		BC_DEC_STATE_STOP,
		BC_DEC_STATE_CLOSE,
	};
	char label[96];
	for (uint32_t device : devices) {
		for (bool immediate : {false, true}) {
			for (uint32_t state : states) {
				std::snprintf(label, sizeof(label), "%s %s rejects state %u",
				              device == BC_PCI_DEVID_FLEA ? "FLEA" : "LINK",
				              immediate ? "immediate" : "ordinary",
				              static_cast<unsigned>(state));
				case_name = label;
				Fixture fixture(device);
				fixture.context.State = state;
				Check(StartCapture(fixture, immediate) ==
				          BC_STS_DEC_NOT_STARTED,
				      "capture rejects a lifecycle with closed admission");
				Check(event_count == 0 && add_calls == 0 && start_calls == 0,
				      "rejected lifecycle performs no driver operation");
				Check(!fixture.context.bMapOutBufDone &&
				      !fixture.context.bMapOutBufDirty && OwnedCount() == 0,
				      "rejected lifecycle cannot acquire output ownership");
				Check(PoolSize(fixture.context) == BC_IOCTL_DATA_POOL_SIZE,
				      "rejected lifecycle preserves every ioctl envelope");
			}
		}
	}
}

static void TestShutdownWinsCaptureRace()
{
	const uint32_t devices[] = {BC_PCI_DEVID_LINK, BC_PCI_DEVID_FLEA};
	char label[112];
	for (uint32_t device : devices) {
		for (bool immediate : {false, true}) {
			for (bool close_decoder : {false, true}) {
				std::snprintf(label, sizeof(label),
				              "%s %s delayed after %s",
				              device == BC_PCI_DEVID_FLEA ? "FLEA" : "LINK",
				              immediate ? "immediate" : "ordinary",
				              close_decoder ? "close" : "stop");
				case_name = label;
				Fixture fixture(device);
				Check(DtsMapYUVBuffs(&fixture.context) == BC_STS_SUCCESS,
				      "race fixture begins with driver-owned output buffers");
				ResetMock(false);
				ResetLifecycleGate();

				BC_STATUS start_status = BC_STS_ERROR;
				std::thread starter([&] {
					delay_context_lock = 1;
					start_status = StartCapture(fixture, immediate);
				});
				WaitForDelayedLock();
				const BC_STATUS shutdown_status =
					ShutdownDecoder(fixture, close_decoder);
				const size_t shutdown_events = event_count;
				ReleaseDelayedLock();
				starter.join();

				Check(shutdown_status == BC_STS_SUCCESS,
				      "shutdown completes while delayed capture is excluded");
				Check(start_status == BC_STS_DEC_NOT_STARTED,
				      "capture revalidates lifecycle after acquiring the mutex");
				Check(shutdown_events == 1 && event_count == shutdown_events &&
				      CountCommand(BCM_IOC_FLUSH_RX_CAP) == 1 &&
				      CountCommand(BCM_IOC_ADD_RXBUFFS) == 0 &&
				      CountCommand(BCM_IOC_START_RX_CAP) == 0,
				      "no capture command follows completed shutdown cleanup");
				Check(events[0].command == BCM_IOC_FLUSH_RX_CAP &&
				      events[0].discard_only == FALSE,
				      "shutdown performs one destructive output flush");
				Check(fixture.context.State ==
				          (close_decoder ? BC_DEC_STATE_CLOSE : BC_DEC_STATE_STOP),
				      "shutdown leaves the requested terminal decoder state");
				Check(!fixture.context.bMapOutBufDone &&
				      !fixture.context.bMapOutBufDirty && OwnedCount() == 0,
				      "shutdown retains no output-buffer ownership");
				Check(firmware_stop_calls == 1 &&
				      firmware_close_calls == (close_decoder ? 1U : 0U),
				      "shutdown crosses each required firmware boundary once");
				Check(PoolSize(fixture.context) == BC_IOCTL_DATA_POOL_SIZE,
				      "shutdown race preserves every ioctl envelope");
			}
		}
	}
}

static void TestFlushAdmissionBarrier()
{
	const uint32_t devices[] = {BC_PCI_DEVID_LINK, BC_PCI_DEVID_FLEA};
	char label[96];
	for (uint32_t device : devices) {
		for (bool immediate : {false, true}) {
			std::snprintf(label, sizeof(label), "%s %s FLUSH barrier",
			              device == BC_PCI_DEVID_FLEA ? "FLEA" : "LINK",
			              immediate ? "immediate" : "ordinary");
			case_name = label;
			Fixture fixture(device);
			Check(DtsMapYUVBuffs(&fixture.context) == BC_STS_SUCCESS,
			      "barrier fixture begins with driver-owned output buffers");
			ResetMock(false);
			ResetLifecycleGate();
			pause_firmware_stop = true;

			BC_STATUS start_status = BC_STS_ERROR;
			BC_STATUS stop_status = BC_STS_ERROR;
			std::thread starter([&] {
				delay_context_lock = 1;
				start_status = StartCapture(fixture, immediate);
			});
			WaitForDelayedLock();
			std::thread stopper([&] {
				stop_status = DtsStopDecoder(&fixture.context);
			});
			WaitForFirmwareStop();
			Check(fixture.context.State == BC_DEC_STATE_FLUSH,
			      "stop publishes FLUSH before firmware and output cleanup");

			ReleaseDelayedLock();
			starter.join();
			Check(start_status == BC_STS_DEC_NOT_STARTED,
			      "FLUSH closes capture admission under the shared mutex");
			Check(event_count == 0 && add_calls == 0 && start_calls == 0,
			      "capture cannot issue commands after the FLUSH barrier");

			ReleaseFirmwareStop();
			stopper.join();
			Check(stop_status == BC_STS_SUCCESS && firmware_stop_calls == 1,
			      "stop completes after the barrier probe");
			Check(event_count == 1 &&
			      events[0].command == BCM_IOC_FLUSH_RX_CAP &&
			      events[0].discard_only == FALSE,
			      "stop destructively retires the original output mapping");
			Check(fixture.context.State == BC_DEC_STATE_STOP &&
			      !fixture.context.bMapOutBufDone &&
			      !fixture.context.bMapOutBufDirty && OwnedCount() == 0,
			      "barrier race finishes stopped with no output ownership");
			Check(PoolSize(fixture.context) == BC_IOCTL_DATA_POOL_SIZE,
			      "barrier race preserves every ioctl envelope");
		}
	}
}

static void TestCaptureWinsShutdownRace()
{
	const uint32_t devices[] = {BC_PCI_DEVID_LINK, BC_PCI_DEVID_FLEA};
	char label[112];
	for (uint32_t device : devices) {
		for (bool immediate : {false, true}) {
			for (bool close_decoder : {false, true}) {
				std::snprintf(label, sizeof(label),
				              "%s %s wins before %s",
				              device == BC_PCI_DEVID_FLEA ? "FLEA" : "LINK",
				              immediate ? "immediate" : "ordinary",
				              close_decoder ? "close" : "stop");
				case_name = label;
				Fixture fixture(device);
				ResetLifecycleGate();

				BC_STATUS shutdown_status = BC_STS_ERROR;
				std::thread shutdown([&] {
					delay_context_lock = 1;
					shutdown_status =
						ShutdownDecoder(fixture, close_decoder);
				});
				WaitForDelayedLock();
				const BC_STATUS start_status =
					StartCapture(fixture, immediate);
				const size_t start_events = event_count;
				Check(start_status == BC_STS_SUCCESS &&
				      start_events == BC_RX_LIST_CNT + 1,
				      "capture completes before shutdown acquires admission");
				Check(AddsUseUniqueConfiguredBuffers(0, BC_RX_LIST_CNT) &&
				      CountCommand(BCM_IOC_START_RX_CAP) == 1 &&
				      OwnedCount() == BC_RX_LIST_CNT,
				      "winning capture registers one complete unique output set");
				CheckStartPayload(events[BC_RX_LIST_CNT], device, immediate);

				ReleaseDelayedLock();
				shutdown.join();
				Check(shutdown_status == BC_STS_SUCCESS,
				      "shutdown succeeds after capture wins admission");
				Check(event_count == start_events + 1 &&
				      CountCommand(BCM_IOC_ADD_RXBUFFS) == BC_RX_LIST_CNT &&
				      CountCommand(BCM_IOC_START_RX_CAP) == 1 &&
				      CountCommand(BCM_IOC_FLUSH_RX_CAP) == 1 &&
				      events[event_count - 1].command == BCM_IOC_FLUSH_RX_CAP,
				      "shutdown follows capture with one destructive flush");
				Check(!duplicate_add && OwnedCount() == 0 &&
				      !fixture.context.bMapOutBufDone &&
				      !fixture.context.bMapOutBufDirty,
				      "shutdown cleans every buffer admitted by winning capture");
				Check(fixture.context.State ==
				          (close_decoder ? BC_DEC_STATE_CLOSE : BC_DEC_STATE_STOP),
				      "capture-win race reaches the requested terminal state");
				Check(firmware_stop_calls == 1 &&
				      firmware_close_calls == (close_decoder ? 1U : 0U),
				      "capture-win shutdown crosses firmware boundaries once");
				Check(PoolSize(fixture.context) == BC_IOCTL_DATA_POOL_SIZE,
				      "capture-win race preserves every ioctl envelope");
			}
		}
	}
}

static void TestStartFailuresAndRetry()
{
	const FailureKind kinds[] = {FAIL_STATUS, FAIL_SYSCALL};
	char label[96];
	for (bool immediate : {false, true}) {
		for (FailureKind kind : kinds) {
			std::snprintf(label, sizeof(label), "%s START %s failure",
			              immediate ? "immediate" : "ordinary",
			              kind == FAIL_STATUS ? "status" : "syscall");
			case_name = label;
			Fixture fixture(BC_PCI_DEVID_LINK);
			fail_start_at = 0;
			start_failure = kind;
			const BC_STATUS expected = kind == FAIL_STATUS ?
			                           BC_STS_BUSY : BC_STS_ERROR;
			Check(StartCapture(fixture, immediate) == expected,
			      "START failure is returned to the caller");
			Check(add_calls == BC_RX_LIST_CNT && start_calls == 1 &&
			      fixture.context.bMapOutBufDone &&
			      fixture.context.bMapOutBufDirty &&
			      OwnedCount() == BC_RX_LIST_CNT,
			      "START failure preserves the complete mapped set");
			Check(PoolSize(fixture.context) == BC_IOCTL_DATA_POOL_SIZE,
			      "failed START returns its outer envelope");

			ResetMock(false);
			Check(StartCapture(fixture, immediate) == BC_STS_SUCCESS,
			      "START can be retried without remapping");
			Check(event_count == 1 && add_calls == 0 && flush_calls == 0 &&
			      start_calls == 1 &&
			      events[0].command == BCM_IOC_START_RX_CAP,
			      "START retry issues only the START command");
			Check(!duplicate_add && OwnedCount() == BC_RX_LIST_CNT,
			      "START retry cannot duplicate driver buffer ownership");
			CheckStartPayload(events[0], BC_PCI_DEVID_LINK, immediate);
		}
	}
}

static void TestFlushSemantics()
{
	const uint32_t devices[] = {BC_PCI_DEVID_LINK, BC_PCI_DEVID_FLEA};
	char label[80];
	for (uint32_t device : devices) {
		std::snprintf(label, sizeof(label), "%s flush semantics",
		              device == BC_PCI_DEVID_FLEA ? "FLEA" : "LINK");
		case_name = label;
		Fixture fixture(device);
		Check(DtsMapYUVBuffs(&fixture.context) == BC_STS_SUCCESS,
		      "flush fixture maps its buffers");

		ResetMock(false);
		Check(DtsFlushRxCapture(&fixture.context, TRUE) == BC_STS_SUCCESS,
		      "discard-only flush succeeds");
		Check(event_count == 1 && events[0].command == BCM_IOC_FLUSH_RX_CAP &&
		      events[0].discard_only == TRUE && events[0].reserved == 0,
		      "discard-only flush sends the exact payload");
		Check(fixture.context.bMapOutBufDone &&
		      fixture.context.bMapOutBufDirty &&
		      OwnedCount() == BC_RX_LIST_CNT,
		      "discard-only flush preserves mapped ownership");

		ResetMock(false);
		fail_flush_at = 0;
		flush_failure = device == BC_PCI_DEVID_FLEA ?
		                FAIL_SYSCALL : FAIL_STATUS;
		const BC_STATUS expected = flush_failure == FAIL_SYSCALL ?
		                           BC_STS_ERROR : BC_STS_IO_ERROR;
		Check(DtsFlushRxCapture(&fixture.context, FALSE) == expected,
		      "destructive flush reports driver and syscall failures");
		Check(event_count == 1 && events[0].command == BCM_IOC_FLUSH_RX_CAP &&
		      events[0].discard_only == FALSE && events[0].reserved == 0,
		      "destructive flush sends the exact payload");
		Check(!fixture.context.bMapOutBufDone &&
		      fixture.context.bMapOutBufDirty &&
		      OwnedCount() == BC_RX_LIST_CNT,
		      "issued destructive failure invalidates Done but retains Dirty");

		ResetMock(false);
		Check(DtsFlushRxCapture(&fixture.context, FALSE) == BC_STS_SUCCESS,
		      "destructive flush can be retried");
		Check(event_count == 1 && events[0].discard_only == FALSE &&
		      !fixture.context.bMapOutBufDone &&
		      !fixture.context.bMapOutBufDirty && OwnedCount() == 0,
		      "confirmed destructive flush clears both ownership flags");
		const size_t old_events = event_count;
		Check(DtsFlushRxCapture(&fixture.context, TRUE) == BC_STS_SUCCESS &&
		      DtsFlushRxCapture(&fixture.context, FALSE) == BC_STS_SUCCESS &&
		      event_count == old_events,
		      "flushes short-circuit when the driver owns no output buffers");
	}
}

static void TestFlushPidMismatch()
{
	case_name = "flush PID mismatch";
	Fixture fixture(BC_PCI_DEVID_LINK);
	/* Model a fork child: both inherited values still name the parent, so
	 * DtsChkPID alone reports a match even though getpid() has changed. */
	fixture.context.ProcessID = getpid() + 1;
	fixture.globals.g_nProcID = fixture.context.ProcessID;
	observed_forbidden_lock = &fixture.context.thLock;

	fixture.context.bMapOutBufDone = false;
	fixture.context.bMapOutBufDirty = false;
	Check(DtsFlushRxCapture(&fixture.context, FALSE) == BC_STS_SUCCESS,
	      "forked child keeps the historical clean no-op success");
	Check(event_count == 0 && forbidden_lock_calls == 0,
	      "clean PID mismatch returns before locking or issuing an ioctl");
	Check(DtsStartCapture(&fixture.context) == BC_STS_ERROR &&
	      DtsStartCaptureImmidiate(&fixture.context, 0) == BC_STS_ERROR,
	      "forked child rejects both capture start variants");
	Check(event_count == 0 && forbidden_lock_calls == 0,
	      "start PID mismatch returns before locking or issuing an ioctl");

	fixture.context.bMapOutBufDone = false;
	fixture.context.bMapOutBufDirty = true;
	Check(DtsFlushRxCapture(&fixture.context, TRUE) == BC_STS_ERROR,
	      "forked child rejects cleanup of dirty parent ownership");
	Check(event_count == 0 && forbidden_lock_calls == 0,
	      "dirty PID mismatch returns before locking or issuing an ioctl");
	observed_forbidden_lock = nullptr;
}

static void TestReleaseOrdering(bool device_close, BC_STATUS decoder_status)
{
	struct ReleaseScenario {
		FailureKind cleanup_failure;
		FailureKind handle_failure;
		int close_errno;
		BC_STATUS expected;
		const char *description;
	};
	const ReleaseScenario scenarios[] = {
		{FAIL_NONE, FAIL_NONE, 0, BC_STS_SUCCESS, "confirmed cleanup"},
		{FAIL_STATUS, FAIL_NONE, 0, BC_STS_IO_ERROR, "unmap status failure"},
		{FAIL_SYSCALL, FAIL_NONE, 0, BC_STS_ERROR, "unmap syscall failure"},
		{FAIL_NONE, FAIL_STATUS, 0, BC_STS_BUSY, "RELEASE status failure"},
		{FAIL_NONE, FAIL_SYSCALL, 0, BC_STS_ERROR, "RELEASE syscall failure"},
		{FAIL_NONE, FAIL_NONE, EIO, BC_STS_ERROR, "close EIO failure"},
		{FAIL_NONE, FAIL_NONE, EINTR, BC_STS_ERROR, "close EINTR failure"},
		{FAIL_STATUS, FAIL_STATUS, 0, BC_STS_IO_ERROR, "unmap before RELEASE failure"},
		{FAIL_STATUS, FAIL_NONE, EIO, BC_STS_IO_ERROR, "unmap before close failure"},
		{FAIL_NONE, FAIL_STATUS, EIO, BC_STS_BUSY, "RELEASE before close failure"},
		{FAIL_STATUS, FAIL_STATUS, EIO, BC_STS_IO_ERROR, "all cleanup failures"},
		{FAIL_SYSCALL, FAIL_STATUS, EINTR, BC_STS_ERROR, "unmap syscall before later failures"},
	};
	char label[112];
	for (const ReleaseScenario &scenario : scenarios) {
		const bool decoder_failed = decoder_status != BC_STS_SUCCESS;
		/* Failed firmware close leaves STOP state, so DeviceClose first retries
		 * capture cleanup before the terminal release retries a failed unmap. */
		const bool quarantine = scenario.cleanup_failure != FAIL_NONE &&
		                        !decoder_failed;
		std::snprintf(label, sizeof(label), "%s%s %s",
		              device_close ? "DeviceClose" : "release",
		              decoder_failed ? " after decoder failure" : "",
		              scenario.description);
		case_name = label;
		bc_dil_glob_s globals = {};
		DTS_LIB_CONTEXT *context = static_cast<DTS_LIB_CONTEXT *>(
			std::malloc(sizeof(*context)));
		Check(context != nullptr, "release fixture allocates its context");
		if (!context)
			continue;
		std::memset(context, 0, sizeof(*context));
		context->Sig = LIB_CTX_SIG;
		context->DevHandle = 99;
		context->DevId = BC_PCI_DEVID_FLEA;
		context->ProcessID = getpid();
		context->OpMode = DTS_PLAYBACK_MODE;
		context->State = device_close ?
			(decoder_failed ? BC_DEC_STATE_STOP : BC_DEC_STATE_CLOSE) :
			BC_DEC_STATE_START;
		context->OpenRsp.channelId = 7;
		context->CfgFlags = BC_MPOOL_INCL_YUV_BUFFS | BC_ADDBUFF_MOVE;
		context->MpoolCnt = BC_RX_LIST_CNT;
		context->bMapOutBufDone = true;
		context->bMapOutBufDirty = true;
		context->Mpools = static_cast<DTS_MPOOL_TYPE *>(
			std::malloc(BC_RX_LIST_CNT * sizeof(*context->Mpools)));
		context->pOutData = static_cast<BC_IOCTL_DATA *>(
			std::malloc(sizeof(*context->pOutData)));
		Check(context->Mpools && context->pOutData,
		      "release fixture allocates output metadata");
		if (!context->Mpools || !context->pOutData) {
			std::free(context->Mpools);
			std::free(context->pOutData);
			std::free(context);
			continue;
		}
		std::memset(context->Mpools, 0,
		            BC_RX_LIST_CNT * sizeof(*context->Mpools));
		std::memset(context->pOutData, 0, sizeof(*context->pOutData));

		pthread_mutexattr_t attributes;
		pthread_mutexattr_init(&attributes);
		pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
		pthread_mutex_init(&context->thLock, &attributes);
		pthread_mutexattr_destroy(&attributes);

		std::memset(release_outputs, 0, sizeof(release_outputs));
		std::memset(release_ioctl_pool, 0, sizeof(release_ioctl_pool));
		for (size_t index = 0; index < BC_RX_LIST_CNT; ++index) {
			context->Mpools[index].type = BC_MEM_DEC_YUVBUFF |
			                               BC_MEM_USER_MODE_ALLOC;
			context->Mpools[index].sz = 1;
			context->Mpools[index].buff =
				static_cast<uint8_t *>(std::malloc(1));
			Check(context->Mpools[index].buff != nullptr,
			      "release fixture allocates output backing");
			release_outputs[index] = context->Mpools[index].buff;
		}
		for (size_t index = 0; index < BC_IOCTL_DATA_POOL_SIZE; ++index) {
			BC_IOCTL_DATA *item = static_cast<BC_IOCTL_DATA *>(
				std::malloc(sizeof(*item)));
			Check(item != nullptr,
			      "release fixture allocates an ioctl envelope");
			if (!item)
				continue;
			std::memset(item, 0, sizeof(*item));
			item->next = context->pIoDataFreeHd;
			context->pIoDataFreeHd = item;
			release_ioctl_pool[index] = item;
		}

		bc_dil_glob_ptr = &globals;
		active_context = context;
		active_pools = context->Mpools;
		active_pool_count = BC_RX_LIST_CNT;
		ResetMock(true);
		for (size_t index = 0; index < BC_RX_LIST_CNT; ++index)
			driver_owned[index] = true;
		if (scenario.cleanup_failure != FAIL_NONE) {
			fail_flush_at = 0;
			flush_failure = scenario.cleanup_failure;
			release_clears_ownership = false;
		}
		release_failure = scenario.handle_failure;
		close_error = scenario.close_errno;
		firmware_close_status = decoder_status;
		firmware_close_calls = 0;
		if (!device_close || decoder_failed)
			DtsSetDecStat(true, context->ProcessID);
		release_pool_array = context->Mpools;
		release_output_frees = release_ioctl_frees = 0;
		release_pool_array_freed = false;
		release_context_freed = false;
		output_freed_too_early = false;
		release_tracking = true;

		Check(PoolSize(*context) == BC_IOCTL_DATA_POOL_SIZE,
		      "release starts with the complete ioctl envelope pool");
		const BC_STATUS status = device_close ? DtsDeviceClose(context) :
		                                      DtsReleaseInterface(context);
		BC_STATUS expected = decoder_failed ? decoder_status : scenario.expected;
		if (device_close && expected == BC_STS_BUSY)
			expected = BC_STS_ERROR;
		Check(status == expected,
		      device_close && scenario.expected == BC_STS_BUSY ?
		      "consuming DeviceClose never returns retry-only BUSY" :
		      "terminal cleanup preserves the first failure status");
		const size_t unmaps = decoder_failed &&
		                      scenario.cleanup_failure != FAIL_NONE ? 2U : 1U;
		Check(event_count == unmaps + 1 &&
		      events[0].command == BCM_IOC_FLUSH_RX_CAP &&
		      events[0].discard_only == FALSE &&
		      events[unmaps].command == BCM_IOC_RELEASE,
		      "teardown attempts destructive unmap before RELEASE");
		Check(events[0].pool_size == BC_IOCTL_DATA_POOL_SIZE - 1 &&
		      events[unmaps].pool_size == BC_IOCTL_DATA_POOL_SIZE,
		      "teardown returns the unmap envelope before RELEASE");
		Check(release_calls == 1 && close_calls == 1 &&
		      release_sequence < close_sequence,
		      "interface closes after the single RELEASE attempt");
		Check(release_ioctl_frees == BC_IOCTL_DATA_POOL_SIZE &&
		      release_pool_array_freed,
		      "teardown frees ioctl and output-pool metadata after close");
		Check(release_context_freed && shared_detach_calls == 1,
		      "terminal cleanup consumes the context and shared attachment once");
		Check(!globals.g_bDecOpened && globals.g_nProcID == 0,
		      "terminal cleanup retires unreachable decoder ownership");
		Check(firmware_close_calls == (decoder_failed ? 1U : 0U),
		      "only the open public decoder requires a firmware close");
		Check(release_output_frees ==
		          (quarantine ? 0U : static_cast<unsigned>(BC_RX_LIST_CNT)),
		      quarantine ?
		      "unconfirmed unmap quarantines every capture backing" :
		      "confirmed unmap frees every capture backing");
		Check(!output_freed_too_early,
		      "RELEASE and close precede every output-pool free");
		if (!quarantine) {
			Check(release_output_frees == BC_RX_LIST_CNT,
			      "later teardown failures cannot retain backing after confirmed unmap");
		}
		Check(OwnedCount() ==
		          (quarantine ? static_cast<unsigned>(BC_RX_LIST_CNT) : 0U),
		      quarantine ?
		      "failed cleanup models ownership surviving RELEASE" :
		      "confirmed cleanup retires all driver ownership");

		release_tracking = false;
		active_context = nullptr;
		active_pools = nullptr;
		active_pool_count = 0;
		bc_dil_glob_ptr = nullptr;
		if (quarantine) {
			/* Production intentionally quarantines these addresses. Reclaim
			 * them only after the context and mock driver are gone so sanitizer
			 * runs do not mistake this test fixture for a suite leak. */
			for (size_t index = 0; index < BC_RX_LIST_CNT; ++index)
				__real_free(release_outputs[index]);
		}
	}
}

int main()
{
	TestConfiguredPoolAllocation();
	TestReducedPoolStarts();
#ifdef CRYSTALHD_EXPERIMENTAL_SCALED_RX
	TestScaledPoolStarts();
	TestScaledPoolFallbacks();
	TestScaledPoolAllocationFailures();
	TestScaledPoolOwnershipAndRestart();
	TestScaledPoolPartialAddRetry();
	TestScaledPoolValidationBarriers();
#else
	TestScaledPoolBuildGate();
#endif
	TestDirectMapSuccess();
	TestAddBufferSizeContract();
	TestMapPreflightRejectsInvalidPool();
	TestAddFailureMatrix();
	TestRollbackFailureAndRetry();
	TestRepeatedStartMappingFailures();
	TestSuccessfulStarts();
	TestConcurrentStarts();
	TestCaptureLifecycleStates();
	TestShutdownWinsCaptureRace();
	TestFlushAdmissionBarrier();
	TestCaptureWinsShutdownRace();
	TestStartFailuresAndRetry();
	TestFlushSemantics();
	TestFlushPidMismatch();
	TestReleaseOrdering(false, BC_STS_SUCCESS);
	TestReleaseOrdering(true, BC_STS_SUCCESS);
	TestReleaseOrdering(true, BC_STS_TIMEOUT);
	std::printf("Library capture: %u checks, %u failures\n", checks, failures);
	return failures ? 1 : 0;
}
