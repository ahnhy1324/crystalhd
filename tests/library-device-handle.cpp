// SPDX-License-Identifier: LGPL-2.1-or-later
/* Hardware-free coverage for device descriptor ownership. */
#include <cerrno>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <initializer_list>
#include <pthread.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#include "7411d.h"
#include "libcrystalhd_if.h"
#include "libcrystalhd_priv.h"

extern bc_dil_glob_s *bc_dil_glob_ptr;

static unsigned checks;
static unsigned failures;

static int open_result;
static unsigned open_calls;
static unsigned close_calls;
static int closed_fd;
static unsigned thread_create_calls;
static unsigned thread_join_calls;
static int thread_create_status;
static pthread_t thread_create_id;
static DTS_LIB_CONTEXT *thread_context;
static bool thread_id_published_privately;
static bool thread_buffers_ready_at_create;
static bool thread_buffers_live_at_join;
static bool thread_join_required;
static bool unexpected_thread_join;
static bool allocation_freed_before_join;
static unsigned shared_attach_calls;
static unsigned shared_detach_calls;
static unsigned shared_nattch;
static bool raw_close_expected;
static bool expect_recovery_owner_during_close;
static bool recovery_owner_seen_during_close;
static uint32_t expected_mode;
static uint32_t initial_hw_state;
static unsigned firmware_push_calls;
static unsigned firmware_link_calls;
static unsigned firmware_init_calls;
static BC_STATUS firmware_push_status;
static BC_STATUS firmware_init_status;

struct IoctlRecord {
	int fd;
	unsigned long command;
};

static IoctlRecord ioctl_log[32];
static size_t ioctl_count;
static unsigned long failed_command;
static bool fail_with_status;
static bool fail_with_syscall;
static bool fail_cleanup;
static bool unexpected_ioctl;

static bool track_allocations;
static bool allocation_overflow;
static void *tracked_allocations[64];
static size_t tracked_count;
static size_t tracked_frees;
static size_t failed_malloc_size;
static unsigned failed_malloc_occurrence;
static unsigned matching_malloc_calls;
static bool malloc_failure_fired;
static size_t failed_aligned_size;
static unsigned failed_aligned_occurrence;
static unsigned matching_aligned_calls;
static bool aligned_failure_fired;

enum class LockFailure { None, Attribute, Type, Mutex };
static LockFailure lock_failure;
static bool lock_failure_fired;
static const pthread_mutexattr_t *context_attribute_address;
static pthread_mutexattr_t context_attribute;
static bool context_attribute_live, context_recursive;
static pthread_mutex_t *context_mutex;
static bool context_mutex_live;
static unsigned attribute_init_calls, attribute_type_calls, attribute_destroy_calls;
static unsigned context_mutex_init_calls, context_mutex_destroy_calls;
static unsigned invalid_attribute_use, invalid_mutex_use;

static bc_dil_glob_s shared_state;

static void Check(bool condition, const char *message)
{
	++checks;
	if (!condition) {
		std::fprintf(stderr, "FAIL: %s\n", message);
		++failures;
	}
}

static void TrackAllocation(void *pointer)
{
	if (!track_allocations || !pointer)
		return;
	if (tracked_count == sizeof(tracked_allocations) /
	                         sizeof(tracked_allocations[0])) {
		allocation_overflow = true;
		return;
	}
	tracked_allocations[tracked_count++] = pointer;
}

static void TrackFree(void *pointer)
{
	if (!track_allocations || !pointer)
		return;
	for (size_t index = 0; index < tracked_count; ++index) {
		if (tracked_allocations[index] == pointer) {
			allocation_freed_before_join |= thread_join_required;
			tracked_allocations[index] = nullptr;
			++tracked_frees;
			return;
		}
	}
}

static bool IsLiveAllocation(const void *pointer)
{
	if (!pointer)
		return false;
	for (size_t index = 0; index < tracked_count; ++index)
		if (tracked_allocations[index] == pointer)
			return true;
	return false;
}

extern "C" void *__real_malloc(size_t);
extern "C" void __real_free(void *);
extern "C" int __real_posix_memalign(void **, size_t, size_t);
extern "C" int __real_pthread_mutexattr_init(pthread_mutexattr_t *);
extern "C" int __real_pthread_mutexattr_settype(pthread_mutexattr_t *, int);
extern "C" int __real_pthread_mutexattr_destroy(pthread_mutexattr_t *);
extern "C" int __real_pthread_mutex_init(pthread_mutex_t *, const pthread_mutexattr_t *);
extern "C" int __real_pthread_mutex_destroy(pthread_mutex_t *);
extern "C" int __real_pthread_mutex_lock(pthread_mutex_t *);
extern "C" int __real_pthread_mutex_unlock(pthread_mutex_t *);

/* Keep the attribute in fixture-owned storage so OLD-code failure cases can
 * be inspected and cleaned up even after DtsInitLock's stack frame returned.
 * Invalid mutex operations are counted, never passed to pthreads. */
extern "C" int __wrap_pthread_mutexattr_init(pthread_mutexattr_t *attribute)
{
	if (!track_allocations || context_attribute_address)
		return __real_pthread_mutexattr_init(attribute);
	context_attribute_address = attribute;
	++attribute_init_calls;
	if (lock_failure == LockFailure::Attribute) {
		lock_failure_fired = true;
		return ENOMEM;
	}
	const int status = __real_pthread_mutexattr_init(&context_attribute);
	context_attribute_live = status == 0;
	return status;
}

extern "C" int __wrap_pthread_mutexattr_settype(pthread_mutexattr_t *attribute,
					       int type)
{
	if (attribute != context_attribute_address)
		return __real_pthread_mutexattr_settype(attribute, type);
	++attribute_type_calls;
	if (!context_attribute_live) {
		++invalid_attribute_use;
		return EINVAL;
	}
	Check(type == PTHREAD_MUTEX_RECURSIVE, "context requires a recursive mutex");
	if (lock_failure == LockFailure::Type) {
		lock_failure_fired = true;
		return EINVAL;
	}
	const int status = __real_pthread_mutexattr_settype(&context_attribute, type);
	context_recursive = status == 0 && type == PTHREAD_MUTEX_RECURSIVE;
	return status;
}

extern "C" int __wrap_pthread_mutexattr_destroy(pthread_mutexattr_t *attribute)
{
	if (attribute != context_attribute_address)
		return __real_pthread_mutexattr_destroy(attribute);
	++attribute_destroy_calls;
	if (!context_attribute_live) {
		++invalid_attribute_use;
		return EINVAL;
	}
	const int status = __real_pthread_mutexattr_destroy(&context_attribute);
	if (!status)
		context_attribute_live = false;
	return status;
}

extern "C" int __wrap_pthread_mutex_init(pthread_mutex_t *mutex,
					 const pthread_mutexattr_t *attribute)
{
	if (!attribute || attribute != context_attribute_address)
		return __real_pthread_mutex_init(mutex, attribute);
	context_mutex = mutex;
	++context_mutex_init_calls;
	if (!context_attribute_live || !context_recursive) {
		++invalid_attribute_use;
		return EINVAL;
	}
	if (lock_failure == LockFailure::Mutex) {
		lock_failure_fired = true;
		return EAGAIN;
	}
	const int status = __real_pthread_mutex_init(mutex, &context_attribute);
	context_mutex_live = status == 0;
	return status;
}

extern "C" int __wrap_pthread_mutex_destroy(pthread_mutex_t *mutex)
{
	if (mutex == context_mutex) {
		++context_mutex_destroy_calls;
		if (!context_mutex_live) {
			++invalid_mutex_use;
			return EINVAL;
		}
		const int status = __real_pthread_mutex_destroy(mutex);
		if (!status)
			context_mutex_live = false;
		return status;
	}
	return __real_pthread_mutex_destroy(mutex);
}

extern "C" int __wrap_pthread_mutex_lock(pthread_mutex_t *mutex)
{
	if (mutex == context_mutex && !context_mutex_live) {
		++invalid_mutex_use;
		return EINVAL;
	}
	return __real_pthread_mutex_lock(mutex);
}

extern "C" int __wrap_pthread_mutex_unlock(pthread_mutex_t *mutex)
{
	if (mutex == context_mutex && !context_mutex_live) {
		++invalid_mutex_use;
		return EINVAL;
	}
	return __real_pthread_mutex_unlock(mutex);
}

extern "C" void *__wrap_malloc(size_t size)
{
	if (failed_malloc_occurrence && failed_malloc_size == size &&
	    ++matching_malloc_calls == failed_malloc_occurrence) {
		malloc_failure_fired = true;
		return nullptr;
	}
	void *pointer = __real_malloc(size);
	TrackAllocation(pointer);
	return pointer;
}

extern "C" void __wrap_free(void *pointer)
{
	TrackFree(pointer);
	__real_free(pointer);
}

extern "C" int __wrap_posix_memalign(void **pointer, size_t alignment,
				      size_t size)
{
	if (failed_aligned_occurrence && failed_aligned_size == size &&
	    ++matching_aligned_calls == failed_aligned_occurrence) {
		aligned_failure_fired = true;
		return ENOMEM;
	}
	const int status = __real_posix_memalign(pointer, alignment, size);
	if (status == 0)
		TrackAllocation(*pointer);
	return status;
}

extern "C" int __wrap_open(const char *path, int flags, ...)
{
	++open_calls;
	Check(std::strcmp(path, CRYSTALHD_API_DEV_NAME) == 0,
	      "device open uses the CrystalHD node");
	Check(flags == O_RDWR, "device open requests read/write access");
	if (open_result < 0)
		errno = ENOENT;
	return open_result;
}

extern "C" int __wrap_close(int fd)
{
	++close_calls;
	closed_fd = fd;
	if (expect_recovery_owner_during_close) {
		recovery_owner_seen_during_close = shared_state.g_bDecOpened;
		Check(shared_state.g_bDecOpened,
		      "recovery-owned decoder remains reserved through driver close");
	}
	if (raw_close_expected)
		Check(ioctl_count == 0,
		      "context-allocation failure closes without driver commands");
	else
		Check(ioctl_count > 0 &&
		      ioctl_count <= sizeof(ioctl_log) / sizeof(ioctl_log[0]) &&
		      ioctl_log[ioctl_count - 1].command == BCM_IOC_RELEASE,
		      "device close follows the driver release command");
	return 0;
}

extern "C" int __wrap_ioctl(int fd, unsigned long command, ...)
{
	va_list arguments;
	va_start(arguments, command);
	BC_IOCTL_DATA *data = va_arg(arguments, BC_IOCTL_DATA *);
	va_end(arguments);

	if (ioctl_count < sizeof(ioctl_log) / sizeof(ioctl_log[0]))
		ioctl_log[ioctl_count] = {fd, command};
	else
		unexpected_ioctl = true;
	++ioctl_count;
	Check(data != nullptr, "every ioctl supplies an envelope");
	if (!data)
		return -1;

	data->RetSts = BC_STS_SUCCESS;
	switch (command) {
	case BCM_IOC_GET_HWTYPE:
		data->u.hwType.PciDevId = BC_PCI_DEVID_FLEA;
		data->u.hwType.PciVenId = 0x14e4;
		data->u.hwType.HwRev = 1;
		break;
	case BCM_IOC_GET_VERSION:
		data->u.VerInfo.DriverMajor = 2;
		data->u.VerInfo.DriverMinor = 14;
		data->u.VerInfo.DriverRevision = 0;
		break;
	case BCM_IOC_NOTIFY_MODE:
		Check(data->u.NotifyMode.Mode == expected_mode,
		      "open notifies the selected mode");
		break;
	case BCM_IOC_RELEASE:
	case BCM_IOC_FW_CMD:
		break;
	default:
		unexpected_ioctl = true;
		break;
	}

	if (fail_cleanup && command == BCM_IOC_RELEASE) {
		errno = EIO;
		return -1;
	}
	if (command == failed_command) {
		if (fail_with_syscall) {
			data->RetSts = BC_STS_BUSY;
			errno = EIO;
			return -1;
		}
		if (fail_with_status)
			data->RetSts = BC_STS_BUSY;
	}
	return 0;
}

extern "C" int __wrap_pthread_create(pthread_t *thread,
				       const pthread_attr_t *attributes,
				       void *(*entry)(void *), void *argument)
{
	++thread_create_calls;
	Check(attributes == nullptr && entry == txThreadProc,
	      "worker creation uses its exact entry and default joinable attributes");
	thread_context = static_cast<DTS_LIB_CONTEXT *>(argument);
	thread_buffers_ready_at_create =
		IsLiveAllocation(thread_context->circBuf.buffer) &&
		IsLiveAllocation(thread_context->alignBuf) &&
		IsLiveAllocation(thread_context->txScratch);
	/* POSIX leaves this output unspecified on failure: poison it deliberately. */
	*thread = thread_create_id;
	thread_id_published_privately = thread != &thread_context->htxThread &&
		thread_context->htxThread == pthread_t();
	thread_join_required = thread_create_status == 0 &&
		thread_create_id != pthread_t();
	return thread_create_status;
}

extern "C" int __wrap_pthread_join(pthread_t thread, void **)
{
	++thread_join_calls;
	if (!thread_join_required || thread != thread_create_id) {
		unexpected_thread_join = true;
		return ESRCH;
	}
	thread_buffers_live_at_join = IsLiveAllocation(thread_context) &&
		IsLiveAllocation(thread_context->circBuf.buffer) &&
		IsLiveAllocation(thread_context->alignBuf) &&
		IsLiveAllocation(thread_context->txScratch);
	thread_join_required = false;
	return 0;
}

/* These internal SDK functions have C++ linkage; public DtsDeviceOpen does not. */
extern "C" BC_STATUS __wrap__Z15DtsPushFwToFleaPvPc(HANDLE device, char *path)
{
	++firmware_push_calls;
	const DTS_LIB_CONTEXT *context = DtsGetContext(device);
	Check(expected_mode == DTS_HWINIT_MODE && context &&
	      context->OpMode == DTS_HWINIT_MODE &&
	      context->DevId == BC_PCI_DEVID_FLEA && path == nullptr,
	      "hardware-init uploads firmware for its exact Flea context");
	Check(DtsGetHwInitSts() == BC_DIL_HWINIT_IN_PROGRESS &&
	      firmware_init_calls == 0,
	      "hardware-init claims progress before uploading, never after INIT");
	return firmware_push_status;
}

extern "C" BC_STATUS __wrap__Z19DtsPushAuthFwToLinkPvPc(HANDLE, char *)
{
	++firmware_link_calls;
	Check(false, "Flea hardware-init never selects the Link loader");
	return BC_STS_ERROR;
}

extern "C" BC_STATUS __wrap__Z15DtsFWInitializePvj(HANDLE device, uint32_t reserved)
{
	++firmware_init_calls;
	const DTS_LIB_CONTEXT *context = DtsGetContext(device);
	Check(expected_mode == DTS_HWINIT_MODE && context &&
	      context->OpMode == DTS_HWINIT_MODE && reserved == 0,
	      "hardware-init initializes its exact context");
	Check(DtsGetHwInitSts() == BC_DIL_HWINIT_IN_PROGRESS &&
	      firmware_push_calls == 1 && firmware_push_status == BC_STS_SUCCESS,
	      "firmware INIT follows exactly one successful upload");
	return firmware_init_status;
}

extern "C" int __wrap_shmget(key_t, size_t, int)
{
	return 17;
}

extern "C" void *__wrap_shmat(int, const void *, int)
{
	++shared_attach_calls;
	std::memset(&shared_state, 0, sizeof(shared_state));
	shared_state.gHwInitSts = initial_hw_state;
	return &shared_state;
}

extern "C" int __wrap_shmdt(const void *address)
{
	++shared_detach_calls;
	Check(address == &shared_state,
	      "shared-memory cleanup detaches the test segment");
	return 0;
}

extern "C" int __wrap_shmctl(int, int command, struct shmid_ds *buffer)
{
	if (command == IPC_STAT && buffer) {
		std::memset(buffer, 0, sizeof(*buffer));
		buffer->shm_nattch = shared_nattch;
		return 0;
	}
	if (command == IPC_RMID)
		return 0;
	return -1;
}

static void BeginCase(int descriptor)
{
	open_result = descriptor;
	open_calls = 0;
	close_calls = 0;
	closed_fd = -2;
	thread_create_calls = 0;
	thread_join_calls = 0;
	thread_create_status = 0;
	thread_create_id = pthread_t();
	thread_context = nullptr;
	thread_id_published_privately = false;
	thread_buffers_ready_at_create = false;
	thread_buffers_live_at_join = false;
	thread_join_required = false;
	unexpected_thread_join = false;
	allocation_freed_before_join = false;
	shared_attach_calls = 0;
	shared_detach_calls = 0;
	shared_nattch = 0;
	expected_mode = DTS_MONITOR_MODE;
	initial_hw_state = BC_DIL_HWINIT_NOT_YET;
	firmware_push_calls = firmware_link_calls = firmware_init_calls = 0;
	firmware_push_status = firmware_init_status = BC_STS_SUCCESS;
	raw_close_expected = false;
	ioctl_count = 0;
	failed_command = 0;
	fail_with_status = false;
	fail_with_syscall = false;
	fail_cleanup = false;
	unexpected_ioctl = false;
	allocation_overflow = false;
	tracked_count = 0;
	tracked_frees = 0;
	failed_malloc_size = 0;
	failed_malloc_occurrence = 0;
	matching_malloc_calls = 0;
	malloc_failure_fired = false;
	failed_aligned_size = 0;
	failed_aligned_occurrence = 0;
	matching_aligned_calls = 0;
	aligned_failure_fired = false;
	lock_failure = LockFailure::None;
	lock_failure_fired = false;
	context_attribute_address = nullptr;
	context_attribute_live = context_recursive = false;
	context_mutex = nullptr;
	context_mutex_live = false;
	attribute_init_calls = attribute_type_calls = attribute_destroy_calls = 0;
	context_mutex_init_calls = context_mutex_destroy_calls = 0;
	invalid_attribute_use = invalid_mutex_use = 0;
	std::memset(tracked_allocations, 0, sizeof(tracked_allocations));
	track_allocations = true;
}

static void EndCase(const char *message)
{
	size_t live = 0;
	for (size_t index = 0; index < tracked_count; ++index)
		live += tracked_allocations[index] != nullptr;
	Check(!allocation_overflow, "allocation tracking remains bounded");
	Check(live == 0 && tracked_frees == tracked_count, message);
	Check(!context_attribute_live, "initialized context attributes are destroyed");
	Check(!context_mutex_live, "initialized context mutex is destroyed after cleanup");
	Check(!invalid_attribute_use && !invalid_mutex_use,
	      "failed synchronization objects are never used or destroyed");
	if (context_attribute_live) {
		__real_pthread_mutexattr_destroy(&context_attribute);
		context_attribute_live = false;
	}
	track_allocations = false;
	for (size_t index = 0; index < tracked_count; ++index) {
		if (tracked_allocations[index]) {
			__real_free(tracked_allocations[index]);
			tracked_allocations[index] = nullptr;
		}
	}
}

static void CheckIoctls(int descriptor,
			std::initializer_list<unsigned long> expected,
			const char *message)
{
	bool exact = ioctl_count == expected.size() && !unexpected_ioctl;
	size_t index = 0;
	for (unsigned long command : expected) {
		if (index >= ioctl_count || index >= sizeof(ioctl_log) /
		                                      sizeof(ioctl_log[0])) {
			exact = false;
			break;
		}
		exact &= ioctl_log[index].fd == descriptor;
		exact &= ioctl_log[index].command == command;
		++index;
	}
	Check(exact, message);
}

static size_t PoolSize(const DTS_LIB_CONTEXT &context)
{
	size_t count = 0;
	for (const BC_IOCTL_DATA *item = context.pIoDataFreeHd;
	     item && count <= BC_IOCTL_DATA_POOL_SIZE; item = item->next)
		++count;
	return count;
}

static void CheckCommandOwnership()
{
	BeginCase(0);
	DTS_LIB_CONTEXT context = {};
	context.Sig = LIB_CTX_SIG;
	context.DevHandle = 0;
	context.DevId = BC_PCI_DEVID_FLEA;
	context.OpMode = DTS_MONITOR_MODE;
	Check(DtsAllocMemPools(&context) == BC_STS_SUCCESS,
	      "fd-zero command fixture allocates its pools");
	Check(PoolSize(context) == BC_IOCTL_DATA_POOL_SIZE,
	      "command fixture starts with the complete ioctl pool");

	Check(DtsDrvCmd(&context, BCM_IOC_GET_VERSION, 0, nullptr, FALSE) ==
	          BC_STS_SUCCESS,
	      "fd zero accepts an internally allocated command envelope");
	Check(PoolSize(context) == BC_IOCTL_DATA_POOL_SIZE,
	      "internally allocated command ownership returns to the pool");

	BC_IOCTL_DATA *borrowed = DtsAllocIoctlData(&context);
	Check(borrowed && PoolSize(context) == BC_IOCTL_DATA_POOL_SIZE - 1,
	      "caller can borrow one command envelope");
	Check(DtsDrvCmd(&context, BCM_IOC_GET_VERSION, 0, borrowed, FALSE) ==
	          BC_STS_SUCCESS,
	      "fd zero accepts a caller-owned command envelope");
	Check(PoolSize(context) == BC_IOCTL_DATA_POOL_SIZE - 1,
	      "caller-owned command remains borrowed");
	DtsRelIoctlData(&context, borrowed);

	borrowed = DtsAllocIoctlData(&context);
	Check(DtsDrvCmd(&context, BCM_IOC_GET_VERSION, 0, borrowed, TRUE) ==
	          BC_STS_SUCCESS,
	      "fd zero accepts transferred command ownership");
	Check(PoolSize(context) == BC_IOCTL_DATA_POOL_SIZE,
	      "transferred command returns to the pool");

	BC_IOCTL_DATA *pool = context.pIoDataFreeHd;
	context.pIoDataFreeHd = nullptr;
	const size_t calls_before_empty = ioctl_count;
	Check(DtsDrvCmd(&context, BCM_IOC_GET_VERSION, 0, nullptr, FALSE) ==
	          BC_STS_INSUFF_RES,
	      "empty command pool preserves its resource error");
	Check(ioctl_count == calls_before_empty,
	      "empty command pool issues no ioctl");
	context.pIoDataFreeHd = pool;

	DtsReleaseMemPools(&context);
	CheckIoctls(0,
		{BCM_IOC_GET_VERSION, BCM_IOC_GET_VERSION,
		 BCM_IOC_GET_VERSION},
		"fd-zero commands use descriptor zero exactly");
	EndCase("fd-zero command paths release every allocation");
}

static void CheckInvalidDescriptorCleanup()
{
	BeginCase(-1);
	DTS_LIB_CONTEXT *context =
		reinterpret_cast<DTS_LIB_CONTEXT *>(std::malloc(sizeof(*context)));
	Check(context != nullptr, "negative-descriptor fixture allocates a context");
	if (!context) {
		EndCase("failed fixture allocation leaves no allocation");
		return;
	}
	std::memset(context, 0, sizeof(*context));
	context->Sig = LIB_CTX_SIG;
	context->DevHandle = -1;
	context->OpMode = DTS_MONITOR_MODE;
	Check(DtsAllocMemPools(context) == BC_STS_SUCCESS,
	      "negative-descriptor fixture allocates its pools");
	BC_IOCTL_DATA envelope = {};
	Check(DtsDrvCmd(nullptr, BCM_IOC_GET_VERSION, 0, &envelope, FALSE) ==
	          BC_STS_INV_ARG,
	      "null contexts remain invalid");
	Check(DtsDrvCmd(context, BCM_IOC_GET_VERSION, 0, &envelope, FALSE) ==
	          BC_STS_INV_ARG,
	      "negative descriptors are rejected before ioctl");
	std::memset(&shared_state, 0, sizeof(shared_state));
	bc_dil_glob_ptr = &shared_state;
	Check(DtsReleaseInterface(context) == BC_STS_SUCCESS,
	      "negative-descriptor interface cleanup succeeds");
	Check(ioctl_count == 0 && close_calls == 0,
	      "negative-descriptor cleanup issues no ioctl or close");
	EndCase("negative-descriptor cleanup releases the complete pool");
}

static void CheckOpenSuccess(int descriptor)
{
	BeginCase(descriptor);
	HANDLE device = nullptr;
	const BC_STATUS open_status = DtsDeviceOpen(&device, DTS_MONITOR_MODE);
	Check(open_status == BC_STS_SUCCESS,
	      "monitor-mode device open succeeds");
	Check(device != nullptr, "successful open returns a public handle");
	if (open_status == BC_STS_SUCCESS && device) {
		Check(DtsGetContext(device)->DevHandle == descriptor,
		      "the context preserves the exact descriptor");
		DtsLock(DtsGetContext(device));
		const int nested = pthread_mutex_trylock(&DtsGetContext(device)->thLock);
		Check(nested == 0, "successful context lock supports nested ownership");
		if (!nested)
			pthread_mutex_unlock(&DtsGetContext(device)->thLock);
		DtsUnLock(DtsGetContext(device));
		Check(attribute_init_calls == 1 && attribute_type_calls == 1 &&
		      attribute_destroy_calls == 1 && !context_attribute_live,
		      "successful initialization releases its temporary mutex attributes");
		Check(DtsDeviceClose(device) == BC_STS_SUCCESS,
		      "monitor-mode device close succeeds");
	}
	Check(open_calls == 1 && close_calls == 1 && closed_fd == descriptor,
	      "successful lifetime opens and closes the descriptor once");
	Check(thread_create_calls == 1,
	      "successful initialization reaches the worker setup");
	Check(shared_attach_calls == 1 && shared_detach_calls == 1,
	      "successful lifetime balances shared-memory attachment");
	CheckIoctls(descriptor,
		{BCM_IOC_GET_HWTYPE, BCM_IOC_GET_VERSION, BCM_IOC_NOTIFY_MODE,
		 BCM_IOC_RELEASE},
		"successful lifetime preserves command and teardown ordering");
	EndCase("successful device lifetime releases every allocation");
}

static DTS_LIB_CONTEXT *AllocateTerminalContext(uint32_t mode, pid_t process_id)
{
	DTS_LIB_CONTEXT *context = reinterpret_cast<DTS_LIB_CONTEXT *>(
		std::malloc(sizeof(*context)));
	Check(context != nullptr, "terminal fixture allocates a context");
	if (!context)
		return nullptr;
	std::memset(context, 0, sizeof(*context));
	context->Sig = LIB_CTX_SIG;
	context->DevHandle = mode == DTS_PLAYBACK_MODE ? 0 : -1;
	context->DevId = BC_PCI_DEVID_FLEA;
	context->OpMode = mode;
	context->ProcessID = process_id;
	Check(DtsAllocMemPools(context) == BC_STS_SUCCESS,
	      "terminal fixture allocates its pools");
	return context;
}

static void CheckTerminalDecoderOwnership()
{
	BeginCase(0);
	shared_nattch = 1;
	std::memset(&shared_state, 0, sizeof(shared_state));
	bc_dil_glob_ptr = &shared_state;
	const pid_t owner = 1234;
	DTS_LIB_CONTEXT *context =
		AllocateTerminalContext(DTS_PLAYBACK_MODE, owner);
	if (context) {
		context->State = BC_DEC_STATE_STOP;
		context->OpenRsp.channelId = 7;
		DtsSetDecStat(true, owner);
		failed_command = BCM_IOC_FW_CMD;
		fail_with_status = true;
		Check(DtsDeviceClose(context) == BC_STS_ERROR,
		      "consuming close does not return the retry-only BUSY status");
		Check(!shared_state.g_bDecOpened && shared_state.g_nProcID == 0,
		      "terminal release retires its unreachable decoder ownership");
		Check(!DtsIsDecOpened(owner + 1),
		      "a surviving shared segment admits a later decoder process");
	}
	Check(close_calls == 1 && shared_detach_calls == 1,
	      "terminal close consumes the device and shared attachment once");
	CheckIoctls(0, {BCM_IOC_FW_CMD, BCM_IOC_RELEASE},
	      "terminal failure still releases the driver session");
	EndCase("terminal decoder failure releases every allocation");

	BeginCase(-1);
	shared_nattch = 1;
	std::memset(&shared_state, 0, sizeof(shared_state));
	bc_dil_glob_ptr = &shared_state;
	DtsSetDecStat(true, owner);
	context = AllocateTerminalContext(DTS_MONITOR_MODE, owner + 1);
	if (context)
		Check(DtsReleaseInterface(context) == BC_STS_SUCCESS,
		      "non-owner monitor release succeeds");
	Check(shared_state.g_bDecOpened && shared_state.g_nProcID == owner,
	      "non-owner release preserves another decoder's ownership");
	Check(close_calls == 0 && ioctl_count == 0 && shared_detach_calls == 1,
	      "non-owner control performs only local terminal cleanup");
	EndCase("non-owner terminal release frees every allocation");

	BeginCase(-1);
	shared_nattch = 1;
	std::memset(&shared_state, 0, sizeof(shared_state));
	bc_dil_glob_ptr = &shared_state;
	DtsSetDecStat(true, owner);
	context = AllocateTerminalContext(DTS_PLAYBACK_MODE, owner + 1);
	if (context) {
		context->DevHandle = -1;
		context->State = BC_DEC_STATE_STOP;
		Check(DtsReleaseInterface(context) == BC_STS_SUCCESS,
		      "foreign decoder-context release succeeds");
	}
	Check(shared_state.g_bDecOpened && shared_state.g_nProcID == owner,
	      "foreign decoder context cannot retire the current owner");
	Check(close_calls == 0 && ioctl_count == 0 && shared_detach_calls == 1,
	      "foreign-owner control performs only local terminal cleanup");
	EndCase("foreign decoder-context release frees every allocation");

	BeginCase(0);
	shared_nattch = 1;
	std::memset(&shared_state, 0, sizeof(shared_state));
	bc_dil_glob_ptr = &shared_state;
	DtsSetDecStat(true, owner);
	context = AllocateTerminalContext(DTS_PLAYBACK_MODE, owner);
	if (context) {
		context->State = BC_DEC_STATE_CLOSE;
		context->recoveryRetireDecoderOwner = true;
		expect_recovery_owner_during_close = true;
		Check(DtsReleaseInterface(context) == BC_STS_SUCCESS,
		      "failed-recovery logical CLOSE consumes its interface");
		expect_recovery_owner_during_close = false;
	}
	Check(recovery_owner_seen_during_close &&
	      !shared_state.g_bDecOpened && shared_state.g_nProcID == 0,
	      "failed-recovery ownership retires only after driver close");
	Check(close_calls == 1 && shared_detach_calls == 1,
	      "failed-recovery release closes and detaches exactly once");
	CheckIoctls(0, {BCM_IOC_RELEASE},
	      "failed-recovery release preserves terminal driver ordering");
	EndCase("failed-recovery decoder owner releases every allocation");
}

static void CheckNullOpenOutput()
{
	BeginCase(0);
	Check(DtsDeviceOpen(nullptr, DTS_MONITOR_MODE) == BC_STS_INV_ARG,
	      "null device output is rejected");
	Check(open_calls == 0 && close_calls == 0 && ioctl_count == 0,
	      "null device output performs no device operation");
	Check(shared_attach_calls == 0 && shared_detach_calls == 0,
	      "null device output performs no shared-memory operation");
	Check(thread_create_calls == 0,
	      "null device output creates no worker");
	Check(DtsInitInterface(0, nullptr, DTS_MONITOR_MODE) == BC_STS_INV_ARG,
	      "null internal output is rejected");
	EndCase("null device output leaves no allocation");
}

static void CheckContextAllocationFailure(int descriptor)
{
	BeginCase(descriptor);
	failed_malloc_size = sizeof(DTS_LIB_CONTEXT);
	failed_malloc_occurrence = 1;
	raw_close_expected = true;
	HANDLE device = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(1));
	Check(DtsDeviceOpen(&device, DTS_MONITOR_MODE) == BC_STS_INSUFF_RES,
	      "context allocation failure preserves its resource status");
	Check(malloc_failure_fired,
	      "context allocation failure injection is exercised");
	Check(device == nullptr,
	      "context allocation failure publishes no handle");
	Check(open_calls == 1 && close_calls == 1 && closed_fd == descriptor,
	      "context allocation failure closes the raw descriptor once");
	Check(ioctl_count == 0 && thread_create_calls == 0,
	      "context allocation failure issues no ioctl or worker creation");
	Check(shared_attach_calls == 1 && shared_detach_calls == 1,
	      "context allocation failure balances shared-memory attachment");
	EndCase("context allocation failure leaves no allocation");
}

static void CheckInternalContextAllocationFailure()
{
	BeginCase(0);
	failed_malloc_size = sizeof(DTS_LIB_CONTEXT);
	failed_malloc_occurrence = 1;
	HANDLE context = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(1));
	Check(DtsInitInterface(0, &context, DTS_MONITOR_MODE) ==
	          BC_STS_INSUFF_RES,
	      "internal context allocation preserves its resource status");
	Check(malloc_failure_fired,
	      "internal context allocation failure injection is exercised");
	Check(context == nullptr,
	      "internal context allocation failure clears its output");
	Check(open_calls == 0 && close_calls == 0 && ioctl_count == 0,
	      "internal context allocation leaves descriptor ownership to caller");
	Check(thread_create_calls == 0 && shared_attach_calls == 0 &&
	      shared_detach_calls == 0,
	      "internal context allocation failure has no external side effect");
	EndCase("internal context allocation failure leaves no allocation");
}

static void CheckInitAllocationFailure(size_t size, unsigned occurrence,
			       uint32_t mode,
			       std::initializer_list<unsigned long> commands)
{
	BeginCase(0);
	failed_malloc_size = size;
	failed_malloc_occurrence = occurrence;
	HANDLE device = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(1));
	Check(DtsDeviceOpen(&device, mode) == BC_STS_INSUFF_RES,
	      "partial initialization preserves its resource status");
	Check(malloc_failure_fired,
	      "partial initialization failure injection is exercised");
	Check(device == nullptr,
	      "partial initialization failure publishes no handle");
	Check(open_calls == 1 && close_calls == 1 && closed_fd == 0,
	      "partial initialization closes descriptor zero once");
	Check(thread_create_calls == 0,
	      "pre-worker initialization failure creates no worker");
	Check(shared_attach_calls == 1 && shared_detach_calls == 1,
	      "partial initialization detaches shared memory once");
	CheckIoctls(0, commands,
	      "partial initialization preserves cleanup command ordering");
	Check(context_mutex_init_calls == 1 && context_mutex_destroy_calls == 1 &&
	      attribute_destroy_calls == 1 && !invalid_mutex_use,
	      "later allocation failure cleans up its initialized mutex exactly once");
	EndCase("partial initialization releases every allocation");
}

static void CheckContextLockFailure(LockFailure failure, bool internal)
{
	BeginCase(0);
	lock_failure = failure;
	raw_close_expected = true;
	initial_hw_state = BC_DIL_HWINIT_DONE;
	if (internal) {
		std::memset(&shared_state, 0, sizeof(shared_state));
		shared_state.gHwInitSts = initial_hw_state;
		bc_dil_glob_ptr = &shared_state;
	}
	HANDLE context = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(1));
	const BC_STATUS status = internal ?
		DtsInitInterface(0, &context, DTS_MONITOR_MODE) :
		DtsDeviceOpen(&context, DTS_MONITOR_MODE);
	Check(lock_failure_fired && status == BC_STS_INSUFF_RES,
	      "each context lock setup failure reports insufficient resources");
	Check(context == nullptr,
	      "lock setup failure cannot publish a cleanup-unsafe partial context");
	/* OLD-code RED remains bounded: wrappers reject invalid mutex operations
	 * without executing undefined pthread behavior during fallback cleanup. */
	if (context) {
		raw_close_expected = false;
		DtsReleaseInterface(DtsGetContext(context));
	}
	Check(attribute_init_calls == 1 &&
	      attribute_type_calls == (failure == LockFailure::Attribute ? 0U : 1U) &&
	      context_mutex_init_calls == (failure == LockFailure::Mutex ? 1U : 0U),
	      "lock initialization stops at the first failed operation");
	Check(attribute_destroy_calls == (failure == LockFailure::Attribute ? 0U : 1U) &&
	      !context_attribute_live && context_mutex_destroy_calls == 0 &&
	      !invalid_attribute_use && !invalid_mutex_use,
	      "lock failure destroys only a successfully initialized attribute");
	Check(thread_create_calls == 0 && thread_join_calls == 0 && ioctl_count == 0,
	      "lock failure reaches neither worker setup nor driver commands");
	Check(firmware_push_calls == 0 && firmware_link_calls == 0 &&
	      firmware_init_calls == 0 && shared_state.gHwInitSts == initial_hw_state,
	      "lock failure leaves hardware initialization untouched");
	Check(open_calls == (internal ? 0U : 1U) &&
	      close_calls == (internal ? 0U : 1U) &&
	      (internal || closed_fd == 0) &&
	      shared_attach_calls == (internal ? 0U : 1U) &&
	      shared_detach_calls == (internal ? 0U : 1U),
	      "internal failure retains caller ownership; public failure closes and detaches once");
	Check(tracked_count == 1 && tracked_frees == 1,
	      "lock failure frees its unpublished context before any pool allocation");
	EndCase("context lock failure leaves no allocation");
}

static void CheckWorkerSetupFailure(size_t aligned_size, int create_error,
				    bool internal, unsigned occurrence = 1)
{
	BeginCase(0);
	failed_aligned_size = aligned_size;
	failed_aligned_occurrence = aligned_size ? occurrence : 0;
	thread_create_status = create_error;
	thread_create_id = static_cast<pthread_t>(0x1234);
	if (internal) {
		/* The internal caller already owns the shared-memory attachment. */
		std::memset(&shared_state, 0, sizeof(shared_state));
		bc_dil_glob_ptr = &shared_state;
	}
	HANDLE context = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(1));
	const BC_STATUS status = internal ?
		DtsInitInterface(0, &context, DTS_MONITOR_MODE) :
		DtsDeviceOpen(&context, DTS_MONITOR_MODE);
	Check(status == BC_STS_INSUFF_RES,
	      "worker setup failure reports insufficient resources");
	Check(aligned_size ? aligned_failure_fired : thread_create_calls == 1,
	      "the selected worker setup fault is exercised");
	Check(thread_create_calls == (aligned_size ? 0U : 1U),
	      "all aligned buffers must exist before worker creation");
	if (internal) {
		Check(context != nullptr,
		      "internal setup failure returns its caller-owned partial context");
		Check(open_calls == 0 && close_calls == 0 && ioctl_count == 0 &&
		      shared_attach_calls == 0 && shared_detach_calls == 0,
		      "internal setup failure leaves cleanup to its caller");
		if (context) {
			Check(DtsGetContext(context)->htxThread == pthread_t(),
			      "failed setup never publishes an uncreated worker ID");
			Check(DtsReleaseInterface(DtsGetContext(context)) == BC_STS_SUCCESS,
			      "caller can release the partial context exactly once");
		}
	} else {
		Check(context == nullptr,
		      "public worker setup failure publishes no handle");
		/* Keep the OLD-code RED bounded if it incorrectly publishes success. */
		if (context)
			Check(DtsDeviceClose(context) == BC_STS_SUCCESS,
			      "unexpected successful handle still closes safely in the fixture");
	}
	if (create_error)
		Check(thread_id_published_privately,
		      "pthread_create writes only a private temporary until success");
	Check(thread_join_calls == 0 && !unexpected_thread_join,
	      "failed setup never joins a failed or uncreated worker");
	Check(!thread_join_required && !allocation_freed_before_join,
	      "failed setup has no live worker when backing is freed");
	Check(open_calls == (internal ? 0U : 1U) && close_calls == 1 &&
	      closed_fd == 0 && shared_attach_calls == (internal ? 0U : 1U) &&
	      shared_detach_calls == 1,
	      "setup failure consumes exactly the owned descriptor and attachment");
	CheckIoctls(0, {BCM_IOC_RELEASE},
	      "setup failure performs no downstream hardware initialization");
	EndCase("worker setup failure releases every partial allocation");
}

static void CheckWorkerJoinOrdering()
{
	BeginCase(0);
	thread_create_id = static_cast<pthread_t>(0x5678);
	HANDLE device = nullptr;
	const BC_STATUS status = DtsDeviceOpen(&device, DTS_MONITOR_MODE);
	Check(status == BC_STS_SUCCESS && device,
	      "successful worker setup publishes its device handle");
	Check(thread_create_calls == 1 && thread_id_published_privately &&
	      thread_buffers_ready_at_create,
	      "worker starts only after buffers exist and its ID remains private");
	if (device) {
		Check(DtsGetContext(device)->htxThread == thread_create_id,
		      "successful create publishes the exact joinable worker ID");
		Check(DtsDeviceClose(device) == BC_STS_SUCCESS,
		      "successful joinable worker lifetime closes normally");
	}
	Check(thread_join_calls == 1 && !unexpected_thread_join &&
	      thread_buffers_live_at_join && !thread_join_required &&
	      !allocation_freed_before_join,
	      "cleanup joins the successful worker before freeing its buffers or context");
	Check(open_calls == 1 && close_calls == 1 && closed_fd == 0 &&
	      shared_attach_calls == 1 && shared_detach_calls == 1,
	      "joinable worker lifetime balances descriptor and shared ownership");
	CheckIoctls(0,
		{BCM_IOC_GET_HWTYPE, BCM_IOC_GET_VERSION, BCM_IOC_NOTIFY_MODE,
		 BCM_IOC_RELEASE},
		"joinable worker lifetime preserves normal hardware command ordering");
	EndCase("joinable worker lifetime releases every allocation");
}

static void CheckOpenFailure(unsigned long command, bool syscall_failure,
			     bool cleanup_failure, BC_STATUS expected,
			     std::initializer_list<unsigned long> commands)
{
	BeginCase(0);
	failed_command = command;
	fail_with_status = !syscall_failure;
	fail_with_syscall = syscall_failure;
	fail_cleanup = cleanup_failure;
	HANDLE device = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(1));
	Check(DtsDeviceOpen(&device, DTS_MONITOR_MODE) == expected,
	      "device initialization preserves the triggering failure");
	Check(device == nullptr,
	      "failed device initialization publishes no handle");
	Check(open_calls == 1 && close_calls == 1 && closed_fd == 0,
	      "failed initialization releases descriptor zero exactly once");
	Check(shared_attach_calls == 1 && shared_detach_calls == 1,
	      "failed initialization detaches shared memory exactly once");
	CheckIoctls(0, commands,
	      "failed initialization preserves cleanup command ordering");
	EndCase("failed initialization releases every allocation");
}

static void CheckSystemOpenFailure()
{
	BeginCase(-1);
	HANDLE device = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(1));
	Check(DtsDeviceOpen(&device, DTS_MONITOR_MODE) == BC_STS_ERROR,
	      "system open failure preserves the public error status");
	Check(device == nullptr, "system open failure publishes no handle");
	Check(open_calls == 1 && close_calls == 0,
	      "system open failure never closes an invalid descriptor");
	Check(ioctl_count == 0 && thread_create_calls == 0,
	      "system open failure performs no initialization or ioctl");
	Check(shared_attach_calls == 1 && shared_detach_calls == 1,
	      "system open failure releases its shared-memory attachment");
	EndCase("system open failure leaves no allocation");
}

static void CheckHardwareInitOpen(uint32_t initial_state, BC_STATUS push_status,
				 BC_STATUS init_status, BC_STATUS expected,
				 unsigned pushes, unsigned initializations)
{
	BeginCase(0);
	expected_mode = DTS_HWINIT_MODE;
	initial_hw_state = initial_state;
	firmware_push_status = push_status;
	firmware_init_status = init_status;
	HANDLE device = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(1));
	Check(DtsDeviceOpen(&device, DTS_HWINIT_MODE) == expected,
	      "public hardware-init preserves success or its exact setup error");
	Check(firmware_push_calls == pushes && firmware_init_calls == initializations &&
	      firmware_link_calls == 0,
	      "public hardware-init executes the required upload/INIT exactly once or neither");
	Check((device != nullptr) == (expected == BC_STS_SUCCESS),
	      "only completed or already-DONE hardware-init publishes a handle");
	if (device) {
		Check(shared_state.gHwInitSts == BC_DIL_HWINIT_NOT_YET,
		      "successful hardware-init preserves its existing post-open status reset");
		Check(DtsDeviceClose(device) == BC_STS_SUCCESS,
		      "hardware-init handle closes normally without a decoder channel");
	}
	if (initial_state == BC_DIL_HWINIT_NOT_YET)
		Check(shared_state.gHwInitSts == BC_DIL_HWINIT_NOT_YET,
		      "failed owned setup or normal close leaves no stale initialization progress");
	Check(open_calls == 1 && close_calls == 1 && closed_fd == 0 &&
	      thread_create_calls == 1,
	      "hardware-init balances its exact device/interface lifetime");
	Check(shared_attach_calls == 1 && shared_detach_calls == 1,
	      "hardware-init balances its shared-memory attachment");
	CheckIoctls(0,
		{BCM_IOC_GET_HWTYPE, BCM_IOC_GET_VERSION, BCM_IOC_NOTIFY_MODE,
		 BCM_IOC_RELEASE},
		"mocked setup adds no real firmware I/O and preserves terminal release ordering");
	EndCase("hardware-init success/failure releases every allocation");
}

int main()
{
	CheckCommandOwnership();
	CheckInvalidDescriptorCleanup();
	CheckNullOpenOutput();
	CheckContextAllocationFailure(0);
	CheckContextAllocationFailure(99);
	CheckInternalContextAllocationFailure();
	CheckInitAllocationFailure(sizeof(BC_IOCTL_DATA), 1,
		DTS_MONITOR_MODE, {BCM_IOC_RELEASE});
	CheckInitAllocationFailure(sizeof(BC_IOCTL_DATA), 4,
		DTS_MONITOR_MODE, {BCM_IOC_RELEASE});
	CheckInitAllocationFailure(sizeof(BC_IOCTL_DATA),
		BC_IOCTL_DATA_POOL_SIZE + 1, DTS_MONITOR_MODE,
		{BCM_IOC_RELEASE});
	CheckInitAllocationFailure(
		BC_MAX_SW_VOUT_BUFFS * sizeof(DTS_MPOOL_TYPE), 1,
		DTS_PLAYBACK_MODE, {BCM_IOC_RELEASE});
	for (bool internal : {false, true}) {
		for (LockFailure failure : {LockFailure::Attribute, LockFailure::Type,
		                           LockFailure::Mutex})
			CheckContextLockFailure(failure, internal);
		CheckWorkerSetupFailure(CIRC_TX_BUF_SIZE, 0, internal);
		CheckWorkerSetupFailure(ALIGN_BUF_SIZE, 0, internal);
		CheckWorkerSetupFailure(CIRC_TX_BUF_SIZE, 0, internal, 2);
		CheckWorkerSetupFailure(0, EAGAIN, internal);
		CheckWorkerSetupFailure(0, ENOMEM, internal);
	}
	CheckWorkerJoinOrdering();
	CheckOpenSuccess(0);
	CheckOpenSuccess(99);
	CheckTerminalDecoderOwnership();
	CheckOpenFailure(BCM_IOC_GET_HWTYPE, false, true, BC_STS_BUSY,
		{BCM_IOC_GET_HWTYPE, BCM_IOC_RELEASE});
	CheckOpenFailure(BCM_IOC_GET_VERSION, true, false, BC_STS_ERROR,
		{BCM_IOC_GET_HWTYPE, BCM_IOC_GET_VERSION, BCM_IOC_RELEASE});
	CheckOpenFailure(BCM_IOC_NOTIFY_MODE, false, false, BC_STS_BUSY,
		{BCM_IOC_GET_HWTYPE, BCM_IOC_GET_VERSION, BCM_IOC_NOTIFY_MODE,
		 BCM_IOC_RELEASE});
	CheckSystemOpenFailure();
	CheckHardwareInitOpen(BC_DIL_HWINIT_NOT_YET, BC_STS_SUCCESS,
		BC_STS_SUCCESS, BC_STS_SUCCESS, 1, 1);
	CheckHardwareInitOpen(BC_DIL_HWINIT_NOT_YET, BC_STS_IO_XFR_ERROR,
		BC_STS_SUCCESS, BC_STS_IO_XFR_ERROR, 1, 0);
	CheckHardwareInitOpen(BC_DIL_HWINIT_NOT_YET, BC_STS_SUCCESS,
		BC_STS_FW_CMD_ERR, BC_STS_FW_CMD_ERR, 1, 1);
	CheckHardwareInitOpen(BC_DIL_HWINIT_DONE, BC_STS_SUCCESS,
		BC_STS_SUCCESS, BC_STS_SUCCESS, 0, 0);
	CheckHardwareInitOpen(BC_DIL_HWINIT_IN_PROGRESS, BC_STS_SUCCESS,
		BC_STS_SUCCESS, BC_STS_BUSY, 0, 0);
	std::printf("Library device handles: %u checks, %u failures\n",
	            checks, failures);
	return failures ? 1 : 0;
}
