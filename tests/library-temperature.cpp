/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Actual DtsGetCoreTemperature -> pooled register ioctl, without a device.
 * Injected register values test extraction and failure contracts, not sensor
 * calibration, sample freshness or a native temperature observation.
 */
#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include "7411d.h"
#include "libcrystalhd_int_if.h"
#include "libcrystalhd_priv.h"

extern bc_dil_glob_s *bc_dil_glob_ptr;
static unsigned checks, failures, calls;
static DTS_LIB_CONTEXT *observed;
static BC_IOCTL_DATA *expected_envelope;
static uint32_t register_value;
static BC_STATUS read_status;
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
	if (!observed || fd != 99 || code != BCM_IOC_REG_RD)
		std::abort();
	va_list args;
	va_start(args, code);
	BC_IOCTL_DATA *data = va_arg(args, BC_IOCTL_DATA *);
	va_end(args);
	++calls;
	Check(data == expected_envelope && !observed->pIoDataFreeHd,
	      "actual register read borrows the sole pooled envelope");
	Check(data->u.regAcc.Offset == 0x00070044U &&
	      data->u.regAcc.Value == 0,
	      "read only the original BCM70015 temperature status register");
	data->u.regAcc.Value = register_value;
	data->RetSts = read_status;
	if (syscall_failure) { errno = EIO; return -1; }
	return 0;
}

struct Fixture {
	bc_dil_glob_s globals = {};
	DTS_LIB_CONTEXT context = {};
	BC_IOCTL_DATA pooled = {};
	Fixture() {
		bc_dil_glob_ptr = &globals;
		observed = &context;
		expected_envelope = &pooled;
		context.Sig = LIB_CTX_SIG;
		context.DevHandle = 99;
		context.DevId = BC_PCI_DEVID_FLEA;
		context.ProcessID = getpid();
		context.pIoDataFreeHd = &pooled;
		pthread_mutexattr_t attr;
		pthread_mutexattr_init(&attr);
		pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
		pthread_mutex_init(&context.thLock, &attr);
		pthread_mutexattr_destroy(&attr);
		Reset();
	}
	void Reset() {
		calls = 0;
		read_status = BC_STS_SUCCESS;
		syscall_failure = false;
		register_value = 300;
	}
	void Balanced() {
		Check(context.pIoDataFreeHd == &pooled && pooled.next == nullptr,
		      "success and failure return the sole pooled envelope");
	}
	~Fixture() {
		Balanced();
		pthread_mutex_destroy(&context.thLock);
		observed = nullptr;
		expected_envelope = nullptr;
		bc_dil_glob_ptr = nullptr;
	}
};

static void DataField()
{
	Fixture f;
	// Independent literals from the retained nine-bit DATA definition.
	// The last value exercises bit8 so an accidental eight-bit mask fails.
	const struct { uint32_t raw; float temperature; } examples[] = {
		{0, 267.2f}, {1, 266.5f}, {256, 88.0f}, {300, 57.2f}, {511, -90.5f}
	};
	for (const auto &example : examples) {
		f.Reset(); register_value = example.raw;
		float result = -999.0f;
		Check(DtsGetCoreTemperature(&f.context, &result) == BC_STS_SUCCESS &&
		      calls == 1 && std::fabs(result - example.temperature) < 0.0001f,
		      "DATA endpoints preserve the existing conversion with one read");
		f.Balanced();
	}
	for (unsigned bit = 9; bit < 32; ++bit) {
		f.Reset(); register_value = 300U | (1U << bit);
		float result = -999.0f;
		Check(DtsGetCoreTemperature(&f.context, &result) == BC_STS_SUCCESS &&
		      calls == 1 && std::fabs(result - 57.2f) < 0.0001f,
		      "reserved/status bits cannot change the nine-bit DATA conversion");
		f.Balanced();
	}
	f.Reset(); register_value = UINT32_MAX;
	float result = -999.0f;
	Check(DtsGetCoreTemperature(&f.context, &result) == BC_STS_SUCCESS &&
	      calls == 1 && std::fabs(result + 90.5f) < 0.0001f,
	      "all injected non-DATA bits remain excluded; this does not validate a physical sample");
}

static void ReadFailures()
{
	Fixture f;
	for (BC_STATUS status : {BC_STS_BUSY, BC_STS_IO_ERROR, BC_STS_PWR_MGMT,
	                        BC_STS_INV_ARG, BC_STS_CMD_CANCELLED}) {
		f.Reset(); read_status = status; register_value = UINT32_MAX;
		float result = -999.0f;
		Check(DtsGetCoreTemperature(&f.context, &result) == status &&
		      calls == 1 && result == 0,
		      "failed read propagates status and zero output without retry or conversion");
		f.Balanced();
	}
	f.Reset(); syscall_failure = true;
	float result = -999.0f;
	Check(DtsGetCoreTemperature(&f.context, &result) == BC_STS_ERROR &&
	      calls == 1 && result == 0,
	      "syscall failure returns the existing error and zero output");
	f.Balanced();
	f.Reset(); f.context.pIoDataFreeHd = nullptr; result = -999.0f;
	Check(DtsGetCoreTemperature(&f.context, &result) == BC_STS_INSUFF_RES &&
	      calls == 0 && result == 0 && !f.context.pIoDataFreeHd,
	      "pool exhaustion requires no register access");
	f.context.pIoDataFreeHd = &f.pooled;
}

static void HandleAndCompatibility()
{
	Fixture f;
	for (uint32_t id : {uint32_t(BC_PCI_DEVID_LINK), uint32_t(BC_PCI_DEVID_DOZER), 0xffffU}) {
		f.Reset(); f.context.DevId = id;
		float result = -999.0f;
		Check(DtsGetCoreTemperature(&f.context, &result) == BC_STS_SUCCESS &&
		      result == 0 && calls == 0,
		      "non-Flea compatibility remains success with zero and no read");
		f.Balanced();
	}
	f.context.DevId = BC_PCI_DEVID_FLEA;
	for (bool null_handle : {false, true}) {
		f.Reset(); f.context.Sig = 0;
		float result = -999.0f;
		Check(DtsGetCoreTemperature(null_handle ? nullptr : &f.context, &result) ==
		      BC_STS_INV_ARG && result == 0 && calls == 0,
		      "invalid and null handles preserve zero output without access");
	}
	f.context.Sig = LIB_CTX_SIG;
	f.context.ProcessID = getpid() + 1;
	float result = -999.0f;
	Check(DtsGetCoreTemperature(&f.context, &result) == BC_STS_ERROR &&
	      result == 0 && calls == 0,
	      "foreign-process context remains an error without access");
	f.context.ProcessID = getpid();

	// Isolate the old null dereference so the same regression can demonstrate
	// both extraction failures and pointer failure without killing its parent.
	std::fflush(nullptr);
	const pid_t child = fork();
	if (child == 0) {
		const rlimit no_core = {0, 0}; setrlimit(RLIMIT_CORE, &no_core);
		f.context.ProcessID = getpid();
		const bool ok = DtsGetCoreTemperature(&f.context, nullptr) == BC_STS_INV_ARG &&
		                DtsGetCoreTemperature(nullptr, nullptr) == BC_STS_INV_ARG && calls == 0;
		_exit(ok ? 0 : 1);
	}
	int status = 0;
	Check(child > 0 && waitpid(child, &status, 0) == child &&
	      WIFEXITED(status) && WEXITSTATUS(status) == 0,
	      "null output is rejected before dereference, allocation or register access");
}

int main()
{
	DataField(); ReadFailures(); HandleAndCompatibility();
	std::printf("Library temperature: %u checks, %u failures; hardware=NOT_RUN\n", checks, failures);
	return failures ? 1 : 0;
}
