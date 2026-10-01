// SPDX-License-Identifier: LGPL-2.1-or-later
/* Hardware-free capture-buffer ownership tests.  Link the production public
 * and private library sections and intercept only their final ioctls and
 * teardown syscalls.  The mock deliberately accepts an ADD whose ioctl then
 * fails, matching the ownership ambiguity of a post-command copyout error.
 */
#include <cerrno>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <pthread.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <thread>
#include <unistd.h>

#include "7411d.h"
#include "libcrystalhd_if.h"
#include "libcrystalhd_priv.h"

extern bc_dil_glob_s *bc_dil_glob_ptr;

static unsigned checks;
static unsigned failures;
static const char *case_name = "setup";

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

static bool release_seen;
static bool close_seen;
static unsigned close_calls;
static unsigned sequence_number;
static unsigned release_sequence;
static unsigned close_sequence;
static bool release_clears_ownership;
static pthread_mutex_t *observed_forbidden_lock;
static unsigned forbidden_lock_calls;

static bool release_tracking;
static void *release_outputs[BC_RX_LIST_CNT];
static void *release_pool_array;
static void *release_ioctl_pool[BC_IOCTL_DATA_POOL_SIZE];
static unsigned release_output_frees;
static unsigned release_ioctl_frees;
static bool release_pool_array_freed;
static bool output_freed_too_early;

static unsigned OwnedCount()
{
	unsigned count = 0;
	for (size_t index = 0; index < BC_RX_LIST_CNT; ++index)
		count += driver_owned[index] ? 1U : 0U;
	return count;
}

static int PoolIndex(const void *buffer)
{
	for (size_t index = 0; index < active_pool_count; ++index) {
		if (active_pools[index].buff == buffer)
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
	release_seen = close_seen = false;
	close_calls = 0;
	sequence_number = release_sequence = close_sequence = 0;
	release_clears_ownership = true;
	observed_forbidden_lock = nullptr;
	forbidden_lock_calls = 0;
	if (clear_ownership)
		ClearOwnership();
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
	return 0;
}

extern "C" void __real_free(void *);
extern "C" void __wrap_free(void *pointer)
{
	if (release_tracking && pointer) {
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

	explicit Fixture(uint32_t device)
	{
		std::memset(&globals, 0, sizeof(globals));
		std::memset(&context, 0, sizeof(context));
		std::memset(ioctl_pool, 0, sizeof(ioctl_pool));
		std::memset(pools, 0, sizeof(pools));
		std::memset(buffers, 0, sizeof(buffers));

		bc_dil_glob_ptr = &globals;
		active_context = &context;
		active_pools = pools;
		active_pool_count = BC_RX_LIST_CNT;
		context.Sig = LIB_CTX_SIG;
		context.DevHandle = 99;
		context.DevId = device;
		context.ProcessID = getpid();
		context.State = BC_DEC_STATE_START;
		context.CfgFlags = BC_MPOOL_INCL_YUV_BUFFS | BC_ADDBUFF_MOVE;
		context.MpoolCnt = BC_RX_LIST_CNT;
		context.Mpools = pools;
		for (size_t index = 0; index < BC_RX_LIST_CNT; ++index) {
			pools[index].type = BC_MEM_DEC_YUVBUFF |
			                    BC_MEM_USER_MODE_ALLOC;
			pools[index].sz = sizeof(buffers[index]);
			pools[index].buff = buffers[index];
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
	if (count != BC_RX_LIST_CNT || first + count > event_count)
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

static void CheckAddPayloads(size_t first, size_t count)
{
	const uint32_t y_size = 1920U * 1090U;
	bool valid = first + count <= event_count;
	for (size_t index = first; valid && index < first + count; ++index) {
		valid = events[index].command == BCM_IOC_ADD_RXBUFFS &&
		        events[index].buffer_mode == OUTPUT_MODE420 &&
		        events[index].buffer_size == y_size + y_size / 2 &&
		        events[index].buffer_offset == y_size;
	}
	Check(valid, "ADD payloads describe the complete NV12 output backing");
}

static void CheckStartPayload(const IoctlEvent &event, uint32_t device,
			      bool immediate)
{
	const uint32_t pause = device == BC_PCI_DEVID_FLEA ?
	                       BC_RX_LIST_CNT - 2 : PAUSE_DECODER_THRESHOLD;
	const uint32_t resume = device == BC_PCI_DEVID_FLEA ?
	                        FLEA_RT_PU_THRESHOLD : RESUME_DECODER_THRESHOLD;
	Check(event.command == BCM_IOC_START_RX_CAP,
	      "mapping is followed by exactly one START command");
	Check(event.reserved == (immediate ? ST_CAP_IMMIDIATE : NO_PARAM),
	      "START carries the selected delivery mode");
	Check(event.start_threshold == RX_START_DELIVERY_THRESHOLD &&
	      event.pause_threshold == pause && event.resume_threshold == resume,
	      "START carries device-specific delivery, pause and resume thresholds");
}

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
	case_name = "concurrent starts";
	Fixture fixture(BC_PCI_DEVID_FLEA);
	pthread_barrier_t barrier;
	Check(pthread_barrier_init(&barrier, nullptr, 3) == 0,
	      "concurrent fixture creates its start barrier");
	BC_STATUS ordinary_status = BC_STS_ERROR;
	BC_STATUS immediate_status = BC_STS_ERROR;
	std::thread ordinary([&] {
		pthread_barrier_wait(&barrier);
		ordinary_status = DtsStartCapture(&fixture.context);
	});
	std::thread immediate([&] {
		pthread_barrier_wait(&barrier);
		immediate_status = DtsStartCaptureImmidiate(&fixture.context, 0);
	});
	pthread_barrier_wait(&barrier);
	ordinary.join();
	immediate.join();
	pthread_barrier_destroy(&barrier);

	Check(ordinary_status == BC_STS_SUCCESS &&
	      immediate_status == BC_STS_SUCCESS,
	      "both concurrent start variants succeed");
	Check(CountCommand(BCM_IOC_ADD_RXBUFFS) == BC_RX_LIST_CNT &&
	      CountCommand(BCM_IOC_START_RX_CAP) == 2 &&
	      CountCommand(BCM_IOC_FLUSH_RX_CAP) == 0,
	      "concurrent starts create one registration set and two starts");
	Check(event_count == BC_RX_LIST_CNT + 2 &&
	      AddsUseUniqueConfiguredBuffers(0, BC_RX_LIST_CNT),
	      "concurrent registration contains sixteen unique buffers");
	Check(events[BC_RX_LIST_CNT].command == BCM_IOC_START_RX_CAP &&
	      events[BC_RX_LIST_CNT + 1].command == BCM_IOC_START_RX_CAP &&
	      events[BC_RX_LIST_CNT].reserved !=
	          events[BC_RX_LIST_CNT + 1].reserved &&
	      (events[BC_RX_LIST_CNT].reserved == NO_PARAM ||
	       events[BC_RX_LIST_CNT + 1].reserved == NO_PARAM) &&
	      (events[BC_RX_LIST_CNT].reserved == ST_CAP_IMMIDIATE ||
	       events[BC_RX_LIST_CNT + 1].reserved == ST_CAP_IMMIDIATE),
	      "concurrent calls retain their distinct START modes");
	Check(!duplicate_add && OwnedCount() == BC_RX_LIST_CNT &&
	      fixture.context.bMapOutBufDone && fixture.context.bMapOutBufDirty,
	      "concurrent starts leave one complete driver-owned map");
	Check(PoolDepthIs(0, event_count, BC_IOCTL_DATA_POOL_SIZE - 1) &&
	      PoolSize(fixture.context) == BC_IOCTL_DATA_POOL_SIZE,
	      "concurrent starts preserve the complete ioctl pool");
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

static void TestReleaseOrdering()
{
	struct ReleaseScenario {
		FailureKind cleanup_failure;
		FailureKind handle_failure;
		const char *description;
	};
	const ReleaseScenario scenarios[] = {
		{FAIL_NONE, FAIL_NONE, "confirmed cleanup"},
		{FAIL_NONE, FAIL_STATUS, "RELEASE status failure"},
		{FAIL_NONE, FAIL_SYSCALL, "RELEASE syscall failure"},
		{FAIL_SYSCALL, FAIL_NONE, "failed cleanup"},
	};
	char label[80];
	for (const ReleaseScenario &scenario : scenarios) {
		const bool quarantine = scenario.cleanup_failure != FAIL_NONE;
		std::snprintf(label, sizeof(label), "release %s",
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
		context->State = BC_DEC_STATE_START;
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
		if (quarantine) {
			fail_flush_at = 0;
			flush_failure = scenario.cleanup_failure;
			release_clears_ownership = false;
		}
		release_failure = scenario.handle_failure;
		release_pool_array = context->Mpools;
		release_output_frees = release_ioctl_frees = 0;
		release_pool_array_freed = false;
		output_freed_too_early = false;
		release_tracking = true;

		Check(PoolSize(*context) == BC_IOCTL_DATA_POOL_SIZE,
		      "release starts with the complete ioctl envelope pool");
		Check(DtsReleaseInterface(context) == BC_STS_SUCCESS,
		      "interface release succeeds");
		Check(event_count == 2 &&
		      events[0].command == BCM_IOC_FLUSH_RX_CAP &&
		      events[0].discard_only == FALSE &&
		      events[1].command == BCM_IOC_RELEASE,
		      "teardown attempts destructive unmap before RELEASE");
		Check(events[0].pool_size == BC_IOCTL_DATA_POOL_SIZE - 1 &&
		      events[1].pool_size == BC_IOCTL_DATA_POOL_SIZE,
		      "teardown returns the unmap envelope before RELEASE");
		Check(release_calls == 1 && close_calls == 1 &&
		      release_sequence < close_sequence,
		      "interface closes after the single RELEASE attempt");
		Check(release_ioctl_frees == BC_IOCTL_DATA_POOL_SIZE &&
		      release_pool_array_freed,
		      "teardown frees ioctl and output-pool metadata after close");
		Check(release_output_frees ==
		          (quarantine ? 0U : static_cast<unsigned>(BC_RX_LIST_CNT)),
		      quarantine ?
		      "unconfirmed unmap quarantines every capture backing" :
		      "confirmed unmap frees every capture backing");
		Check(!output_freed_too_early,
		      "RELEASE and close precede every output-pool free");
		if (scenario.handle_failure != FAIL_NONE) {
			Check(release_output_frees == BC_RX_LIST_CNT,
			      "failed RELEASE cannot retain backing after confirmed unmap");
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
	TestDirectMapSuccess();
	TestAddFailureMatrix();
	TestRollbackFailureAndRetry();
	TestRepeatedStartMappingFailures();
	TestSuccessfulStarts();
	TestConcurrentStarts();
	TestStartFailuresAndRetry();
	TestFlushSemantics();
	TestFlushPidMismatch();
	TestReleaseOrdering();
	std::printf("Library capture: %u checks, %u failures\n", checks, failures);
	return failures ? 1 : 0;
}
