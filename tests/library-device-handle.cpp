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
static unsigned shared_attach_calls;
static unsigned shared_detach_calls;

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
			tracked_allocations[index] = nullptr;
			++tracked_frees;
			return;
		}
	}
}

extern "C" void *__real_malloc(size_t);
extern "C" void __real_free(void *);
extern "C" int __real_posix_memalign(void **, size_t, size_t);

extern "C" void *__wrap_malloc(size_t size)
{
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
		Check(data->u.NotifyMode.Mode == DTS_MONITOR_MODE,
		      "open notifies the selected monitor mode");
		break;
	case BCM_IOC_FLUSH_RX_CAP:
		Check(data->u.FlushRxCap.bDiscardOnly == TRUE,
		      "cleanup flush discards mapped buffers");
		break;
	case BCM_IOC_RELEASE:
		break;
	default:
		unexpected_ioctl = true;
		break;
	}

	if (fail_cleanup &&
	    (command == BCM_IOC_FLUSH_RX_CAP || command == BCM_IOC_RELEASE)) {
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
				       const pthread_attr_t *,
				       void *(*)(void *), void *)
{
	++thread_create_calls;
	*thread = pthread_t();
	return 0;
}

extern "C" int __wrap_shmget(key_t, size_t, int)
{
	return 17;
}

extern "C" void *__wrap_shmat(int, const void *, int)
{
	++shared_attach_calls;
	std::memset(&shared_state, 0, sizeof(shared_state));
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
	shared_attach_calls = 0;
	shared_detach_calls = 0;
	ioctl_count = 0;
	failed_command = 0;
	fail_with_status = false;
	fail_with_syscall = false;
	fail_cleanup = false;
	unexpected_ioctl = false;
	allocation_overflow = false;
	tracked_count = 0;
	tracked_frees = 0;
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
		 BCM_IOC_GET_VERSION, BCM_IOC_FLUSH_RX_CAP},
		"fd-zero commands and cleanup use descriptor zero exactly");
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
		 BCM_IOC_FLUSH_RX_CAP, BCM_IOC_RELEASE},
		"successful lifetime preserves command and teardown ordering");
	EndCase("successful device lifetime releases every allocation");
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
	HANDLE device = nullptr;
	Check(DtsDeviceOpen(&device, DTS_MONITOR_MODE) == expected,
	      "device initialization preserves the triggering failure");
	Check(open_calls == 1 && close_calls == 1 && closed_fd == 0,
	      "failed initialization releases descriptor zero exactly once");
	CheckIoctls(0, commands,
	      "failed initialization preserves cleanup command ordering");
	EndCase("failed initialization releases every allocation");
}

static void CheckSystemOpenFailure()
{
	BeginCase(-1);
	HANDLE device = nullptr;
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

int main()
{
	CheckCommandOwnership();
	CheckInvalidDescriptorCleanup();
	CheckOpenSuccess(0);
	CheckOpenSuccess(99);
	CheckOpenFailure(BCM_IOC_GET_HWTYPE, false, true, BC_STS_BUSY,
		{BCM_IOC_GET_HWTYPE, BCM_IOC_FLUSH_RX_CAP, BCM_IOC_RELEASE});
	CheckOpenFailure(BCM_IOC_GET_VERSION, true, false, BC_STS_ERROR,
		{BCM_IOC_GET_HWTYPE, BCM_IOC_GET_VERSION,
		 BCM_IOC_FLUSH_RX_CAP, BCM_IOC_RELEASE});
	CheckSystemOpenFailure();
	std::printf("Library device handles: %u checks, %u failures\n",
	            checks, failures);
	return failures ? 1 : 0;
}
