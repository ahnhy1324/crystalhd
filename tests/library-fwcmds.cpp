/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Hardware-free firmware self-test marshalling through the production path. */
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include "7411d.h"
#include "libcrystalhd_fwcmds.h"
#include "libcrystalhd_priv.h"

static unsigned checks, failures, calls;
static DTS_LIB_CONTEXT *active_context;
static C011CmdSelfTest captured;
static BC_STATUS ioctl_status;
static uint32_t response_status;
static bool syscall_failure;

static void Check(bool condition, const char *message)
{
	++checks;
	if (!condition) {
		std::fprintf(stderr, "FAIL: %s\n", message);
		++failures;
	}
}

extern "C" int __wrap_ioctl(int fd, unsigned long code, ...)
{
	if (!active_context || fd != 99 || code != BCM_IOC_FW_CMD) {
		std::fputs("unexpected ioctl in hardware-free firmware-command test\n",
		           stderr);
		std::abort();
	}

	va_list args;
	va_start(args, code);
	BC_IOCTL_DATA *data = va_arg(args, BC_IOCTL_DATA *);
	va_end(args);
	++calls;
	captured = *reinterpret_cast<C011CmdSelfTest *>(data->u.fwCmd.cmd);
	C011RspSelfTest *response =
		reinterpret_cast<C011RspSelfTest *>(data->u.fwCmd.rsp);
	response->status = response_status;
	data->RetSts = ioctl_status;
	if (syscall_failure) {
		errno = EIO;
		return -1;
	}
	return 0;
}

struct Fixture {
	DTS_LIB_CONTEXT context = {};
	BC_IOCTL_DATA pooled = {};

	explicit Fixture(uint32_t device)
	{
		active_context = &context;
		context.Sig = LIB_CTX_SIG;
		context.DevHandle = 99;
		context.DevId = device;
		context.HWOutPicWidth = 1920;
		context.HWOutPicHeight = 1080;
		context.fwcmdseq = 40;
		pthread_mutexattr_t attr;
		pthread_mutexattr_init(&attr);
		pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
		pthread_mutex_init(&context.thLock, &attr);
		pthread_mutexattr_destroy(&attr);
		Prepare();
	}

	void Prepare()
	{
		std::memset(&pooled, 0xa5, sizeof(pooled));
		pooled.next = nullptr;
		context.pIoDataFreeHd = &pooled;
		captured = {};
		ioctl_status = BC_STS_SUCCESS;
		response_status = 0;
		syscall_failure = false;
	}

	void PoolReturned()
	{
		Check(context.pIoDataFreeHd == &pooled && pooled.next == nullptr,
		      "firmware command returns the sole ioctl object to its pool");
	}

	~Fixture()
	{
		PoolReturned();
		pthread_mutex_destroy(&context.thLock);
		active_context = nullptr;
	}
};

static bool HasFleaParameters(uint32_t test_id)
{
	return test_id >= (uint32_t)eC011_TEST_LONG_REGISTER &&
	       test_id <= (uint32_t)eC011_TEST_DECODE_LOOPBACK;
}

static void ValidTestIds(uint32_t device)
{
	Fixture fixture(device);
	for (uint32_t test_id = eC011_TEST_SHORT_MEMORY;
	     test_id <= (uint32_t)eC011_TEST_ENCODE_LOOPBACK; ++test_id) {
		fixture.Prepare();
		const uint32_t old_sequence = fixture.context.fwcmdseq;
		const unsigned old_calls = calls;
		Check(DtsFWHwSelfTest(&fixture.context, test_id) == BC_STS_SUCCESS,
		      "valid firmware self-test command succeeds");
		Check(calls == old_calls + 1,
		      "firmware self-test issues exactly one ioctl");
		Check(captured.command == eCMD_C011_SELF_TEST &&
		          captured.sequence == old_sequence + 1 &&
		          captured.testId == (eC011_TEST_ID)test_id,
		      "firmware self-test preserves command, sequence and test ID");
		const bool has_parameters = device == BC_PCI_DEVID_FLEA &&
		                            HasFleaParameters(test_id);
		Check(captured.mode == (has_parameters ? test_id : 0) &&
		          captured.height == (has_parameters ? 1080U : 0U) &&
		          captured.width == (has_parameters ? 1920U : 0U),
		      "only FLEA long-register and decode-loopback tests carry parameters");
		bool reserved_zero = true;
		for (uint32_t value : captured.rsvd) reserved_zero &= value == 0;
		Check(reserved_zero, "firmware self-test leaves reserved words clear");
		fixture.PoolReturned();
	}
}

static void FailurePropagation()
{
	Fixture fixture(BC_PCI_DEVID_FLEA);

	ioctl_status = BC_STS_IO_ERROR;
	Check(DtsFWHwSelfTest(&fixture.context,
	                      eC011_TEST_LONG_REGISTER) == BC_STS_IO_ERROR,
	      "driver firmware-command failure is propagated");
	fixture.PoolReturned();

	fixture.Prepare();
	response_status = 1;
	Check(DtsFWHwSelfTest(&fixture.context,
	                      eC011_TEST_DECODE_LOOPBACK) == BC_STS_FW_CMD_ERR,
	      "firmware response failure is translated to the public status");
	fixture.PoolReturned();

	fixture.Prepare();
	syscall_failure = true;
	Check(DtsFWHwSelfTest(&fixture.context,
	                      eC011_TEST_SHORT_MEMORY) == BC_STS_ERROR,
	      "ioctl syscall failure is propagated");
	fixture.PoolReturned();

	fixture.Prepare();
	Check(DtsFWHwSelfTest(&fixture.context,
	                      eC011_TEST_LONG_REGISTER) == BC_STS_SUCCESS,
	      "pooled command storage is reusable after every failure path");
}

int main()
{
	ValidTestIds(BC_PCI_DEVID_FLEA);
	ValidTestIds(BC_PCI_DEVID_LINK);
	FailurePropagation();
	std::printf("Library firmware commands: %u checks, %u failures\n",
	            checks, failures);
	return failures ? 1 : 0;
}
