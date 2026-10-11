// SPDX-License-Identifier: LGPL-2.1-or-later
/* Hardware-free coverage for negotiated output-readiness descriptors. */
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fcntl.h>

#include "7411d.h"
#include "libcrystalhd_if.h"
#include "libcrystalhd_priv.h"

static unsigned checks;
static unsigned failures;
static unsigned ioctl_calls;
static unsigned fcntl_calls;
static int query_errno;
static BC_STATUS query_status;
static uint32_t query_abi;
static uint32_t query_flags;
static int duplicate_result;
static int duplicate_errno;
static bool duplicate_eintr_once;

static void Check(bool condition, const char *message)
{
	++checks;
	if (!condition) {
		std::fprintf(stderr, "FAIL: %s\n", message);
		++failures;
	}
}

extern "C" int __wrap_ioctl(int fd, unsigned long command, ...)
{
	va_list arguments;
	va_start(arguments, command);
	BC_IOCTL_DATA *data = va_arg(arguments, BC_IOCTL_DATA *);
	va_end(arguments);

	++ioctl_calls;
	Check(fd == 41, "capability query uses the context device descriptor");
	Check(command == BCM_IOC_QUERY_CAPS,
	      "capability query uses the additive QUERY_CAPS command");
	Check(data != nullptr && data->IoctlDataSz == sizeof(*data),
	      "capability query supplies an initialized native envelope");
	if (query_errno) {
		errno = query_errno;
		return -1;
	}
	data->RetSts = query_status;
	data->u.DriverCaps.abi_version = query_abi;
	data->u.DriverCaps.flags = query_flags;
	return 0;
}

extern "C" int __wrap_fcntl(int fd, int command, ...)
{
	va_list arguments;
	va_start(arguments, command);
	const int minimum = va_arg(arguments, int);
	va_end(arguments);

	++fcntl_calls;
	Check(fd == 41, "duplication uses the context device descriptor");
	Check(command == F_DUPFD_CLOEXEC,
	      "duplicate descriptor is atomically close-on-exec");
	Check(minimum == 0, "descriptor duplication accepts the full fd range");
	if (duplicate_eintr_once) {
		duplicate_eintr_once = false;
		errno = EINTR;
		return -1;
	}
	if (duplicate_result < 0)
		errno = duplicate_errno;
	return duplicate_result;
}

struct Fixture {
	DTS_LIB_CONTEXT context;

	Fixture()
	{
		std::memset(&context, 0, sizeof(context));
		context.Sig = LIB_CTX_SIG;
		context.DevHandle = 41;
		ioctl_calls = 0;
		fcntl_calls = 0;
		query_errno = 0;
		query_status = BC_STS_SUCCESS;
		query_abi = BC_DRIVER_CAPS_ABI_VERSION;
		query_flags = BC_DRIVER_CAP_RX_POLL;
		duplicate_result = 73;
		duplicate_errno = 0;
		duplicate_eintr_once = false;
	}
};

static void Success()
{
	Fixture fixture;
	int poll_fd = 99;
	Check(DtsDupOutputPollFd(&fixture.context, &poll_fd) == BC_STS_SUCCESS,
	      "advertised polling returns success");
	Check(poll_fd == 73, "success publishes the owned duplicate");
	Check(ioctl_calls == 1 && fcntl_calls == 1,
	      "success queries once and duplicates once");

	Fixture interrupted;
	duplicate_eintr_once = true;
	poll_fd = 99;
	Check(DtsDupOutputPollFd(&interrupted.context, &poll_fd) == BC_STS_SUCCESS &&
	          poll_fd == 73 && fcntl_calls == 2,
	      "an interrupted descriptor duplication is retried");

	Fixture future;
	query_abi = BC_DRIVER_CAPS_ABI_VERSION + 1;
	query_flags = BC_DRIVER_CAP_RX_POLL | (1U << 31);
	poll_fd = 99;
	Check(DtsDupOutputPollFd(&future.context, &poll_fd) == BC_STS_SUCCESS &&
	          poll_fd == 73,
	      "newer capability versions and unrelated bits remain compatible");
}

static void UnsupportedDriver()
{
	for (int unsupported_case = 0; unsupported_case != 4; ++unsupported_case) {
		Fixture fixture;
		int poll_fd = 99;
		if (unsupported_case == 0)
			query_errno = ENOTTY;
		else if (unsupported_case == 1)
			query_status = BC_STS_NOT_IMPL;
		else if (unsupported_case == 2)
			query_abi = BC_DRIVER_CAPS_ABI_VERSION - 1;
		else
			query_flags = 0;
		Check(DtsDupOutputPollFd(&fixture.context, &poll_fd) == BC_STS_NOT_IMPL,
		      "unsupported drivers use the explicit compatibility fallback");
		Check(poll_fd == -1 && fcntl_calls == 0,
		      "unsupported drivers return no descriptor");
	}
}

static void Failures()
{
	Fixture fixture;
	int poll_fd = 99;
	Check(DtsDupOutputPollFd(nullptr, &poll_fd) == BC_STS_INV_ARG &&
	          poll_fd == -1 && ioctl_calls == 0,
	      "invalid handles fail before querying the driver");
	fixture.context.DevHandle = -1;
	poll_fd = 99;
	Check(DtsDupOutputPollFd(&fixture.context, &poll_fd) == BC_STS_INV_ARG &&
	          poll_fd == -1 && ioctl_calls == 0,
	      "closed context descriptors fail before querying the driver");
	fixture.context.DevHandle = 41;
	Check(DtsDupOutputPollFd(&fixture.context, nullptr) == BC_STS_INV_ARG &&
	          ioctl_calls == 0,
	      "null output pointers fail before querying the driver");

	query_errno = ENODEV;
	poll_fd = 99;
	Check(DtsDupOutputPollFd(&fixture.context, &poll_fd) == BC_STS_IO_ERROR &&
	          poll_fd == -1 && fcntl_calls == 0,
	      "non-negotiation ioctl failures remain I/O errors");

	query_errno = 0;
	query_status = BC_STS_BUSY;
	poll_fd = 99;
	Check(DtsDupOutputPollFd(&fixture.context, &poll_fd) == BC_STS_BUSY &&
	          poll_fd == -1 && fcntl_calls == 0,
	      "driver status failures are preserved");

	query_status = BC_STS_SUCCESS;
	duplicate_result = -1;
	duplicate_errno = EMFILE;
	poll_fd = 99;
	Check(DtsDupOutputPollFd(&fixture.context, &poll_fd) == BC_STS_IO_ERROR &&
	          poll_fd == -1 && fcntl_calls == 1,
	      "duplication failures publish no descriptor");
}

int main()
{
	Success();
	UnsupportedDriver();
	Failures();
	std::printf("Library poll fd: %u checks, %u failures\n", checks, failures);
	return failures ? 1 : 0;
}
