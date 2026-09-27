/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Hardware-free video-clock DLL reset through the production register path.
 * The mock exposes unrelated status bits so only the DLL-lock bit may decide
 * when the unlock poll finishes. No device is opened.
 */
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <pthread.h>
#include <unistd.h>
#include "7411d.h"
#include "bc_decoder_regs.h"
#include "libcrystalhd_int_if.h"
#include "libcrystalhd_priv.h"

extern bc_dil_glob_s *bc_dil_glob_ptr;
static unsigned checks, failures, calls, status_reads, sleeps;
static uint32_t global_control = 0xa0U;
static bool reset_asserted;

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
	if (fd != 99 || (code != BCM_IOC_FPGA_RD && code != BCM_IOC_FPGA_WR)) {
		std::fputs("unexpected ioctl in hardware-free clock test\n", stderr);
		std::abort();
	}

	va_list args;
	va_start(args, code);
	BC_IOCTL_DATA *data = va_arg(args, BC_IOCTL_DATA *);
	va_end(args);
	++calls;
	data->RetSts = BC_STS_SUCCESS;

	if (code == BCM_IOC_FPGA_WR) {
		Check(data->u.regAcc.Offset == MISC2_GLOBAL_CTRL,
		      "write only the global control register");
		global_control = data->u.regAcc.Value;
		reset_asserted = (global_control & 0x08U) != 0;
		return 0;
	}

	Check(data->u.regAcc.Value == 0,
	      "pooled register read has no stale value");
	if (data->u.regAcc.Offset == MISC2_GLOBAL_CTRL) {
		data->u.regAcc.Value = global_control;
	} else if (data->u.regAcc.Offset == MISC2_INTERNAL_STATUS) {
		++status_reads;
		/* Bit 2 is the DLL lock indicator. Keep an unrelated bit set in
		 * both states so the poll must mask bit 2 rather than test the
		 * complete register value.
		 */
		data->u.regAcc.Value = reset_asserted ? 0x80U : 0x84U;
	} else {
		std::fputs("unexpected register in hardware-free clock test\n", stderr);
		std::abort();
	}
	return 0;
}

extern "C" int __wrap_usleep(useconds_t microseconds)
{
	Check(microseconds == 100000,
	      "clock reset uses only the 100 ms settling interval");
	++sleeps;
	return 0;
}

int main()
{
	bc_dil_glob_s globals = {};
	DTS_LIB_CONTEXT context = {};
	BC_IOCTL_DATA pooled = {};
	bc_dil_glob_ptr = &globals;
	context.Sig = LIB_CTX_SIG;
	context.DevHandle = 99;
	context.DevId = BC_PCI_DEVID_LINK;
	context.pIoDataFreeHd = &pooled;
	pthread_mutexattr_t attr;
	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&context.thLock, &attr);
	pthread_mutexattr_destroy(&attr);

	Check(DtsRstVidClkDLL(&context) == BC_STS_SUCCESS,
	      "DLL reset succeeds after the lock bit clears and returns");
	Check(calls == 6 && status_reads == 2,
	      "unlock and lock states each require exactly one status read");
	Check(sleeps == 1,
	      "an already-unlocked DLL does not consume the 100-poll timeout");
	Check(global_control == 0xa0U && !reset_asserted,
	      "DLL reset is deasserted without changing unrelated control bits");
	Check(context.pIoDataFreeHd == &pooled && pooled.next == nullptr,
	      "all register transactions return the pooled ioctl object");

	pthread_mutex_destroy(&context.thLock);
	bc_dil_glob_ptr = nullptr;
	std::printf("Library clock: %u checks, %u failures\n", checks, failures);
	return failures ? 1 : 0;
}
