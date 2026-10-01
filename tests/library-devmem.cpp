// SPDX-License-Identifier: LGPL-2.1-or-later
/* Hardware-free coverage for the production device-memory and firmware
 * transfer marshalling paths. No CrystalHD device is opened.
 */
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "7411d.h"
#include "crystalhd_ioctl_limits.h"
#include "libcrystalhd_int_if.h"
#include "libcrystalhd_priv.h"

enum Operation {
	READ_MEMORY,
	WRITE_MEMORY,
	PUSH_FIRMWARE,
};

enum IoctlResult {
	IOCTL_SUCCESS,
	IOCTL_SYSCALL_ERROR,
	IOCTL_STATUS_ERROR,
};

static unsigned checks;
static unsigned failures;
static unsigned allocation_calls;
static unsigned free_calls;
static unsigned ioctl_calls;
static size_t allocation_size;
static void *live_allocation;
static bool track_allocations;
static bool fail_allocation;
static Operation expected_operation;
static IoctlResult ioctl_result;
static uint32_t expected_bytes;
static uint32_t expected_offset;
static const uint8_t *expected_payload;

static void Check(bool condition, const char *message)
{
	++checks;
	if (!condition) {
		std::fprintf(stderr, "FAIL: %s\n", message);
		++failures;
	}
}

extern "C" void *__real_malloc(size_t);
extern "C" void __real_free(void *);

extern "C" void *__wrap_malloc(size_t size)
{
	if (!track_allocations)
		return __real_malloc(size);

	++allocation_calls;
	allocation_size = size;
	if (fail_allocation)
		return nullptr;

	Check(live_allocation == nullptr,
	      "a transfer owns at most one heap allocation");
	live_allocation = __real_malloc(size);
	return live_allocation;
}

extern "C" void __wrap_free(void *pointer)
{
	if (track_allocations && pointer == live_allocation) {
		++free_calls;
		live_allocation = nullptr;
	}
	__real_free(pointer);
}

static unsigned long ExpectedCommand()
{
	switch (expected_operation) {
	case READ_MEMORY:
		return BCM_IOC_MEM_RD;
	case WRITE_MEMORY:
		return BCM_IOC_MEM_WR;
	case PUSH_FIRMWARE:
		return BCM_IOC_FW_DOWNLOAD;
	}
	std::abort();
}

extern "C" int __wrap_ioctl(int fd, unsigned long command, ...)
{
	va_list arguments;
	va_start(arguments, command);
	BC_IOCTL_DATA *data = va_arg(arguments, BC_IOCTL_DATA *);
	va_end(arguments);

	++ioctl_calls;
	Check(fd == 99, "transfer uses the fixture device handle");
	Check(command == ExpectedCommand(), "transfer emits the expected ioctl");
	Check(data != nullptr, "transfer supplies an ioctl envelope");
	if (!data)
		return -1;

	Check(data->IoctlDataSz == sizeof(*data),
	      "transfer reports the native ioctl envelope size");
	Check(data->u.devMem.StartOff == expected_offset,
	      "transfer preserves the requested device offset");
	Check(data->u.devMem.NumDwords == expected_bytes / 4U,
	      "transfer reports the checked DWORD count");

	uint8_t *payload = reinterpret_cast<uint8_t *>(data) + sizeof(*data);
	if (expected_operation != READ_MEMORY)
		Check(std::memcmp(payload, expected_payload, expected_bytes) == 0,
		      "write transfer preserves every caller byte");

	if (ioctl_result == IOCTL_SYSCALL_ERROR) {
		errno = EIO;
		return -1;
	}
	if (ioctl_result == IOCTL_STATUS_ERROR) {
		data->RetSts = BC_STS_IO_ERROR;
		return 0;
	}

	data->RetSts = BC_STS_SUCCESS;
	if (expected_operation == READ_MEMORY)
		std::memset(payload, 0x5a, expected_bytes);
	return 0;
}

static BC_STATUS Call(Operation operation, DTS_LIB_CONTEXT *context,
			      uint32_t *buffer, uint32_t bytes)
{
	switch (operation) {
	case READ_MEMORY:
		return DtsDevMemRd(context, buffer, bytes, expected_offset);
	case WRITE_MEMORY:
		return DtsDevMemWr(context, buffer, bytes, expected_offset);
	case PUSH_FIRMWARE:
		return DtsPushFwBinToLink(context, buffer, bytes);
	}
	std::abort();
}

static void BeginCall(Operation operation, uint32_t bytes, uint32_t offset,
		      const uint8_t *payload)
{
	Check(live_allocation == nullptr,
	      "the previous transfer left no live allocation");
	allocation_calls = 0;
	free_calls = 0;
	ioctl_calls = 0;
	allocation_size = 0;
	fail_allocation = false;
	expected_operation = operation;
	ioctl_result = IOCTL_SUCCESS;
	expected_bytes = bytes;
	expected_offset = operation == PUSH_FIRMWARE ? 0 : offset;
	expected_payload = payload;
	track_allocations = true;
}

static void EndCall()
{
	track_allocations = false;
	if (live_allocation) {
		/* Keep a broken leak regression bounded so later cases remain useful. */
		__real_free(live_allocation);
		live_allocation = nullptr;
	}
}

static void CheckRejectedSizes(DTS_LIB_CONTEXT *context, Operation operation)
{
	const uint32_t allocation_wrap =
		UINT32_MAX - static_cast<uint32_t>(sizeof(BC_IOCTL_DATA)) + 1U;
	const uint32_t invalid_sizes[] = {
		0U,
		1U,
		2U,
		3U,
		5U,
		CRYSTALHD_MAX_IOCTL_TRANSFER + 4U,
		allocation_wrap - 4U,
		allocation_wrap,
		allocation_wrap + 4U,
		UINT32_MAX & ~3U,
	};
	Check((allocation_wrap & 3U) == 0,
	      "the native ioctl allocation wrap threshold is DWORD aligned");

	for (uint32_t bytes : invalid_sizes) {
		BeginCall(operation, bytes, 0x12345678U, nullptr);
		/* If an unfixed implementation attempts allocation, fail it before
		 * it can touch this deliberately invalid caller address.
		 */
		fail_allocation = true;
		BC_STATUS status = Call(operation, context,
			reinterpret_cast<uint32_t *>(static_cast<uintptr_t>(1)), bytes);
		track_allocations = false;
		Check(status == BC_STS_ERROR,
		      "invalid transfer size returns the legacy error status");
		Check(allocation_calls == 0,
		      "invalid transfer size is rejected before allocation");
		Check(ioctl_calls == 0,
		      "invalid transfer size is rejected before ioctl");
		EndCall();
	}
}

static void CheckSuccess(DTS_LIB_CONTEXT *context, Operation operation,
			 uint32_t bytes)
{
	std::vector<uint32_t> storage(bytes / 4U + 2U, 0xa5a5a5a5U);
	uint32_t *buffer = storage.data() + 1;
	for (uint32_t index = 0; index < bytes / 4U; ++index)
		buffer[index] = 0x01010101U * (index & 0xffU);
	const std::vector<uint32_t> before = storage;

	BeginCall(operation, bytes, 0x00c0ffecU,
		  reinterpret_cast<const uint8_t *>(buffer));
	BC_STATUS status = Call(operation, context, buffer, bytes);
	Check(status == BC_STS_SUCCESS, "valid transfer succeeds");
	Check(allocation_calls == 1,
	      "valid transfer performs exactly one allocation");
	Check(allocation_size == sizeof(BC_IOCTL_DATA) + bytes,
	      "valid transfer allocates the exact envelope and payload size");
	Check(ioctl_calls == 1, "valid transfer performs exactly one ioctl");
	Check(free_calls == 1 && live_allocation == nullptr,
	      "valid transfer releases its allocation exactly once");
	Check(storage.front() == before.front() &&
	      storage.back() == before.back(),
	      "valid transfer preserves caller canaries");
	if (operation == READ_MEMORY) {
		const uint8_t *returned = reinterpret_cast<const uint8_t *>(buffer);
		bool complete = true;
		for (uint32_t index = 0; index < bytes; ++index)
			complete = complete && returned[index] == 0x5a;
		Check(complete, "successful read copies every returned byte");
	} else {
		Check(storage == before,
		      "successful write leaves the caller buffer unchanged");
	}
	EndCall();
}

static void CheckAllocationFailure(DTS_LIB_CONTEXT *context,
				   Operation operation)
{
	std::vector<uint32_t> storage(6U, 0x12345678U);
	const std::vector<uint32_t> before = storage;
	BeginCall(operation, 16U, 0x1000U,
		  reinterpret_cast<const uint8_t *>(storage.data() + 1));
	fail_allocation = true;
	BC_STATUS status = Call(operation, context, storage.data() + 1, 16U);
	Check(status == BC_STS_ERROR, "allocation failure returns an error");
	Check(allocation_calls == 1 && free_calls == 0,
	      "failed allocation is attempted once and never freed");
	Check(ioctl_calls == 0, "allocation failure performs no ioctl");
	Check(storage == before,
	      "allocation failure leaves the caller buffer unchanged");
	EndCall();
}

static void CheckIoctlFailure(DTS_LIB_CONTEXT *context, Operation operation,
			      IoctlResult result)
{
	std::vector<uint32_t> storage(6U, 0x87654321U);
	const std::vector<uint32_t> before = storage;
	BeginCall(operation, 16U, 0x2000U,
		  reinterpret_cast<const uint8_t *>(storage.data() + 1));
	ioctl_result = result;
	BC_STATUS status = Call(operation, context, storage.data() + 1, 16U);
	Check(status == BC_STS_ERROR, "ioctl failure returns an error");
	Check(allocation_calls == 1 && ioctl_calls == 1,
	      "ioctl failure follows one bounded allocation and request");
	Check(free_calls == 1 && live_allocation == nullptr,
	      "ioctl failure releases its allocation exactly once");
	Check(storage == before,
	      "ioctl failure leaves the caller buffer unchanged");
	EndCall();
}

int main()
{
	DTS_LIB_CONTEXT context = {};
	context.Sig = LIB_CTX_SIG;
	context.DevHandle = 99;
	context.DevId = BC_PCI_DEVID_FLEA;

	const Operation operations[] = {
		READ_MEMORY,
		WRITE_MEMORY,
		PUSH_FIRMWARE,
	};
	for (Operation operation : operations) {
		CheckRejectedSizes(&context, operation);
		CheckSuccess(&context, operation, 4U);
		CheckSuccess(&context, operation, 16U);
		CheckSuccess(&context, operation,
			     CRYSTALHD_MAX_IOCTL_TRANSFER);
		CheckAllocationFailure(&context, operation);
		CheckIoctlFailure(&context, operation, IOCTL_SYSCALL_ERROR);
		CheckIoctlFailure(&context, operation, IOCTL_STATUS_ERROR);
	}

	std::printf("Library device memory: %u checks, %u failures\n",
		    checks, failures);
	return failures ? 1 : 0;
}
