/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Hardware-free firmware self-test marshalling through the production path. */
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <pthread.h>
#include <unistd.h>
#include <vector>
#include "7411d.h"
#include "libcrystalhd_fwcmds.h"
#include "libcrystalhd_if.h"
#include "libcrystalhd_priv.h"

extern bc_dil_glob_s *bc_dil_glob_ptr;
static unsigned checks, failures, calls;
static DTS_LIB_CONTEXT *active_context;
static C011CmdSelfTest captured;
static BC_FW_CMD captured_fw;
static std::vector<BC_FW_CMD> command_log;
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
	captured_fw = data->u.fwCmd;
	command_log.push_back(captured_fw);
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
	bc_dil_glob_s globals = {};
	DTS_LIB_CONTEXT context = {};
	BC_IOCTL_DATA pooled = {};

	explicit Fixture(uint32_t device)
	{
		bc_dil_glob_ptr = &globals;
		active_context = &context;
		context.Sig = LIB_CTX_SIG;
		context.DevHandle = 99;
		context.DevId = device;
		context.ProcessID = getpid();
		globals.g_nProcID = context.ProcessID;
		context.HWOutPicWidth = 1920;
		context.HWOutPicHeight = 1080;
		context.fwcmdseq = 40;
		context.State = BC_DEC_STATE_STOP;
		context.OpenRsp.channelId = 0;
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
		captured_fw = {};
		command_log.clear();
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
		bc_dil_glob_ptr = nullptr;
	}
};

struct ExpectedRateCommand {
	uint32_t command;
	uint32_t value;
};

static void CheckRateCommands(uint32_t first_sequence,
			      std::initializer_list<ExpectedRateCommand> expected,
			      const char *message)
{
	bool exact = command_log.size() == expected.size();
	size_t index = 0;
	for (const ExpectedRateCommand &want : expected) {
		if (index >= command_log.size())
			break;
		const BC_FW_CMD &actual = command_log[index];
		exact &= actual.flags == 0 && actual.add_data == 0;
		for (unsigned word = 0; word < BC_MAX_FW_CMD_BUFF_SZ; ++word) {
			uint32_t value = 0;
			if (word == 0)
				value = want.command;
			else if (word == 1)
				value = first_sequence + (uint32_t)index + 1U;
			else if (word == 2)
				value = active_context->OpenRsp.channelId;
			else if (word == 3)
				value = want.value;
			exact &= actual.cmd[word] == value && actual.rsp[word] == 0;
		}
		++index;
	}
	Check(exact, message);
}

static void CheckFirmwarePayload(const uint32_t expected[BC_MAX_FW_CMD_BUFF_SZ],
				 const char *message)
{
	bool exact = captured_fw.flags == 0 && captured_fw.add_data == 0;

	for (unsigned word = 0; word < BC_MAX_FW_CMD_BUFF_SZ; ++word)
		exact &= captured_fw.cmd[word] == expected[word] &&
		         captured_fw.rsp[word] == 0;
	Check(exact, message);
}

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

static void DecoderStartPayload()
{
	Fixture fixture(BC_PCI_DEVID_FLEA);
	uint32_t expected[BC_MAX_FW_CMD_BUFF_SZ] = {};

	expected[0] = 0x73763102U;
	expected[1] = 41U;
	Check(DtsFWActivateDecoder(&fixture.context) == BC_STS_SUCCESS,
	      "decoder channel activation succeeds");
	CheckFirmwarePayload(expected,
		"decoder activation preserves the reviewed 64-word wire payload");
	fixture.PoolReturned();

	fixture.Prepare();
	std::memset(expected, 0, sizeof(expected));
	expected[0] = 0x7376311aU;
	expected[1] = 42U;
	expected[18] = 1U;
	expected[20] = 1U;
	expected[32] = 1U;
	Check(DtsFWStartVideo(&fixture.context, 0, 0, 0, 1, 0) ==
	          BC_STS_SUCCESS,
	      "progressive H.264 video start succeeds");
	CheckFirmwarePayload(expected,
		"decoder start preserves the reviewed 64-word wire payload");
	Check(fixture.context.State == BC_DEC_STATE_START,
	      "successful low-level start publishes the library run state");
	fixture.PoolReturned();

	fixture.Prepare();
	std::memset(expected, 0, sizeof(expected));
	expected[0] = 0x7376311bU;
	expected[1] = 43U;
	expected[3] = 1U;
	Check(DtsFWStopVideo(&fixture.context, 0, false) == BC_STS_SUCCESS,
	      "started decoder channel stops successfully");
	CheckFirmwarePayload(expected,
		"decoder stop preserves the reviewed 64-word wire payload");
	Check(fixture.context.State == BC_DEC_STATE_STOP,
	      "successful low-level stop publishes the library stop state");
	fixture.PoolReturned();

	fixture.Prepare();
	std::memset(expected, 0, sizeof(expected));
	expected[0] = 0x73763101U;
	expected[1] = 44U;
	expected[3] = 1U;
	fixture.context.OpenRsp.channelStatus = 0xfeedfaceU;
	Check(DtsFWCloseChannel(&fixture.context, 0) == BC_STS_SUCCESS,
	      "stopped decoder channel closes successfully");
	CheckFirmwarePayload(expected,
		"decoder close preserves the reviewed 64-word wire payload");
	Check(fixture.context.OpenRsp.channelStatus == 0,
	      "successful low-level close clears the cached open response");
}

static void ZeroRateValidation()
{
	for (uint32_t device : {BC_PCI_DEVID_LINK, BC_PCI_DEVID_FLEA}) {
		Fixture fixture(device);
		fixture.context.State = BC_DEC_STATE_START;
		fixture.context.OpenRsp.channelId = 7;
		const unsigned old_calls = calls;
		const uint32_t old_sequence = fixture.context.fwcmdseq;
		Check(DtsSetRateChange(&fixture.context, 0, 0) == BC_STS_INV_ARG,
		      "zero general playback rate is rejected");
		Check(DtsSetFFRate(&fixture.context, 0) == BC_STS_INV_ARG,
		      "zero fast-forward rate is rejected");
		Check(calls == old_calls && command_log.empty() &&
		          fixture.context.fwcmdseq == old_sequence,
		      "zero rates issue no command and consume no sequence");
		fixture.PoolReturned();
	}
}

static void RateErrorPrecedence()
{
	const unsigned old_calls = calls;
	Check(DtsSetRateChange(nullptr, 0, 0) == BC_STS_INV_ARG &&
	          DtsSetFFRate(nullptr, 0) == BC_STS_INV_ARG,
	      "invalid handles retain precedence over rate validation");
	Check(calls == old_calls,
	      "invalid rate handles cannot issue firmware commands");

	Fixture fixture(BC_PCI_DEVID_FLEA);
	fixture.context.State = BC_DEC_STATE_CLOSE;
	Check(DtsSetRateChange(&fixture.context, 0, 0) == BC_STS_DEC_NOT_OPEN &&
	          DtsSetFFRate(&fixture.context, 0) == BC_STS_DEC_NOT_OPEN,
	      "closed decoder errors retain precedence over zero rates");
	fixture.context.State = BC_DEC_STATE_STOP;
	Check(DtsSetRateChange(&fixture.context, 0, 0) == BC_STS_DEC_NOT_STARTED &&
	          DtsSetFFRate(&fixture.context, 0) == BC_STS_DEC_NOT_STARTED,
	      "stopped decoder errors retain precedence over zero rates");
	fixture.context.State = BC_DEC_STATE_FLUSH;
	Check(DtsSetRateChange(&fixture.context, 0, 0) == BC_STS_DEC_NOT_STARTED &&
	          DtsSetFFRate(&fixture.context, 0) == BC_STS_DEC_NOT_STARTED,
	      "flushing decoder errors retain precedence over zero rates");
	fixture.context.State = BC_DEC_STATE_START;
	fixture.context.OpenRsp.channelId = 7;
	fixture.globals.g_nProcID = fixture.context.ProcessID + 1;
	Check(DtsSetRateChange(&fixture.context, 0, 0) == BC_STS_ERROR &&
	          DtsSetFFRate(&fixture.context, 0) == BC_STS_ERROR,
	      "process ownership errors retain precedence over zero rates");
	Check(calls == old_calls && command_log.empty() &&
	          fixture.context.fwcmdseq == 40,
	      "all validation failures preserve command state and ownership");
	fixture.PoolReturned();
}

static void PositiveRateCommands()
{
	{
		Fixture fixture(BC_PCI_DEVID_LINK);
		fixture.context.State = BC_DEC_STATE_START;
		fixture.context.OpenRsp.channelId = 7;
		const uint32_t sequence = fixture.context.fwcmdseq;
		Check(DtsSetRateChange(&fixture.context, 10000, 0) ==
		          BC_STS_SUCCESS,
		      "LINK normal rate succeeds");
		CheckRateCommands(sequence,
			{{eCMD_C011_DEC_CHAN_SET_HOST_TRICK_MODE, 0},
			 {eCMD_C011_DEC_CHAN_SET_SKIP_PIC_MODE,
			  eC011_SKIP_PIC_IPB_DECODE},
			 {eCMD_C011_DEC_CHAN_SET_FF_RATE, 1}},
			"LINK normal rate preserves its exact command payloads");
		fixture.PoolReturned();
	}
	{
		Fixture fixture(BC_PCI_DEVID_LINK);
		fixture.context.State = BC_DEC_STATE_START;
		fixture.context.OpenRsp.channelId = 7;
		const uint32_t sequence = fixture.context.fwcmdseq;
		Check(DtsSetRateChange(&fixture.context, 5000, 0) ==
		          BC_STS_SUCCESS,
		      "LINK fast rate succeeds");
		CheckRateCommands(sequence,
			{{eCMD_C011_DEC_CHAN_SET_HOST_TRICK_MODE, 1},
			 {eCMD_C011_DEC_CHAN_SET_SKIP_PIC_MODE,
			  eC011_SKIP_PIC_I_DECODE},
			 {eCMD_C011_DEC_CHAN_SET_FF_RATE, 2}},
			"LINK fast rate preserves its exact command payloads");
		fixture.PoolReturned();
	}
	{
		Fixture fixture(BC_PCI_DEVID_LINK);
		fixture.context.State = BC_DEC_STATE_START;
		fixture.context.OpenRsp.channelId = 7;
		const uint32_t sequence = fixture.context.fwcmdseq;
		Check(DtsSetRateChange(&fixture.context, 20000, 0) ==
		          BC_STS_SUCCESS,
		      "LINK slow rate succeeds");
		CheckRateCommands(sequence,
			{{eCMD_C011_DEC_CHAN_SET_HOST_TRICK_MODE, 1},
			 {eCMD_C011_DEC_CHAN_SET_SKIP_PIC_MODE,
			  eC011_SKIP_PIC_IPB_DECODE},
			 {eCMD_C011_DEC_CHAN_SET_SLOWM_RATE, 2}},
			"LINK slow rate preserves its exact command payloads");
		fixture.PoolReturned();
	}
	for (uint32_t rate : {10000U, 1U}) {
		Fixture fixture(BC_PCI_DEVID_FLEA);
		fixture.context.State = BC_DEC_STATE_START;
		fixture.context.OpenRsp.channelId = 7;
		const uint32_t sequence = fixture.context.fwcmdseq;
		Check(DtsSetRateChange(&fixture.context, rate, 0) ==
		          BC_STS_SUCCESS,
		      "FLEA public rate succeeds");
		CheckRateCommands(sequence,
			{{eCMD_C011_DEC_CHAN_SET_SKIP_PIC_MODE,
			  rate == 10000U ? eC011_SKIP_PIC_IPB_DECODE :
			                   eC011_SKIP_PIC_I_DECODE}},
			"FLEA public rate preserves its device-specific command");
		fixture.PoolReturned();
	}
	for (uint32_t rate : {10000U, 5000U}) {
		Fixture fixture(BC_PCI_DEVID_LINK);
		fixture.context.State = BC_DEC_STATE_START;
		fixture.context.OpenRsp.channelId = 7;
		const uint32_t sequence = fixture.context.fwcmdseq;
		const uint32_t firmware_rate = rate == 10000U ? 1U : 2U;
		Check(DtsSetFFRate(&fixture.context, rate) == BC_STS_SUCCESS,
		      "LINK catch-up rate succeeds");
		CheckRateCommands(sequence,
			{{eCMD_C011_DEC_CHAN_SET_HOST_TRICK_MODE,
			  firmware_rate == 1U ? 0U : 1U},
			 {eCMD_C011_DEC_CHAN_SET_SKIP_PIC_MODE,
			  eC011_SKIP_PIC_IPB_DECODE},
			 {eCMD_C011_DEC_CHAN_SET_FF_RATE, firmware_rate}},
			"LINK catch-up rate preserves its exact command payloads");
		fixture.PoolReturned();
	}
	{
		Fixture fixture(BC_PCI_DEVID_FLEA);
		fixture.context.State = BC_DEC_STATE_START;
		fixture.context.OpenRsp.channelId = 7;
		const uint32_t sequence = fixture.context.fwcmdseq;
		Check(DtsSetFFRate(&fixture.context, 1) == BC_STS_SUCCESS,
		      "FLEA extreme catch-up rate succeeds with its firmware cap");
		CheckRateCommands(sequence,
			{{eCMD_C011_DEC_CHAN_SET_SKIP_PIC_MODE,
			  eC011_SKIP_PIC_IPB_DECODE},
			 {eCMD_C011_DEC_CHAN_SET_FF_RATE,
			  FLEA_MAX_TRICK_MODE_SPEED}},
			"FLEA catch-up rate preserves its cap and command payloads");
		fixture.PoolReturned();
	}
	{
		Fixture fixture(BC_PCI_DEVID_LINK);
		fixture.context.State = BC_DEC_STATE_START;
		fixture.context.OpenRsp.channelId = 7;
		const unsigned old_calls = calls;
		Check(DtsSetFFRate(&fixture.context, 20000) == BC_STS_INV_ARG,
		      "positive slow-motion input remains invalid for catch-up API");
		Check(calls == old_calls && command_log.empty() &&
		          fixture.context.fwcmdseq == 40,
		      "unsupported positive catch-up rate remains side-effect free");
		fixture.PoolReturned();
	}
}

int main()
{
	ValidTestIds(BC_PCI_DEVID_FLEA);
	ValidTestIds(BC_PCI_DEVID_LINK);
	FailurePropagation();
	DecoderStartPayload();
	ZeroRateValidation();
	RateErrorPrecedence();
	PositiveRateCommands();
	std::printf("Library firmware commands: %u checks, %u failures\n",
	            checks, failures);
	return failures ? 1 : 0;
}
