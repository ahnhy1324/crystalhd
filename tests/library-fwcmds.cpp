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
static unsigned sleep_calls;
static unsigned clear_on_sleep;
static unsigned long last_ioctl_code;
static bool flag_seen_in_ioctl;

extern "C" void DumpInputSampleToFile(uint8_t *, uint32_t) {}

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
	if (!active_context || fd != 99 ||
	    (code != BCM_IOC_FW_CMD && code != BCM_IOC_GET_VERSION)) {
		std::fputs("unexpected ioctl in hardware-free firmware-command test\n",
		           stderr);
		std::abort();
	}

	va_list args;
	va_start(args, code);
	BC_IOCTL_DATA *data = va_arg(args, BC_IOCTL_DATA *);
	va_end(args);
	++calls;
	last_ioctl_code = code;
	flag_seen_in_ioctl = active_context->fw_cmd_issued;
	data->RetSts = ioctl_status;
	if (code != BCM_IOC_FW_CMD) {
		if (syscall_failure) {
			errno = EIO;
			return -1;
		}
		return 0;
	}
	captured_fw = data->u.fwCmd;
	command_log.push_back(captured_fw);
	captured = *reinterpret_cast<C011CmdSelfTest *>(data->u.fwCmd.cmd);
	C011RspSelfTest *response =
		reinterpret_cast<C011RspSelfTest *>(data->u.fwCmd.rsp);
	response->status = response_status;
	if (syscall_failure) {
		errno = EIO;
		return -1;
	}
	return 0;
}

extern "C" int __wrap_usleep(useconds_t usec)
{
	++sleep_calls;
	Check(usec == 100,
	      "firmware contention requests the historical wait interval");
	if (active_context && clear_on_sleep == sleep_calls)
		active_context->fw_cmd_issued = false;
	return 0;
}

static void ResetObservations()
{
	captured = {};
	captured_fw = {};
	command_log.clear();
	ioctl_status = BC_STS_SUCCESS;
	response_status = 0;
	syscall_failure = false;
	sleep_calls = 0;
	clear_on_sleep = 0;
	last_ioctl_code = 0;
	flag_seen_in_ioctl = false;
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
		ResetObservations();
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

static size_t PoolSize(const DTS_LIB_CONTEXT &context)
{
	size_t count = 0;
	for (const BC_IOCTL_DATA *item = context.pIoDataFreeHd;
	     item && count <= BC_IOCTL_DATA_POOL_SIZE; item = item->next)
		++count;
	return count;
}

struct FullPoolFixture {
	bc_dil_glob_s globals = {};
	DTS_LIB_CONTEXT context = {};

	explicit FullPoolFixture(uint32_t device)
	{
		bc_dil_glob_ptr = &globals;
		active_context = &context;
		context.Sig = LIB_CTX_SIG;
		context.DevHandle = 99;
		context.DevId = device;
		context.OpMode = DTS_MONITOR_MODE;
		Check(DtsAllocMemPools(&context) == BC_STS_SUCCESS,
		      "contention fixture allocates the complete command pool");
		Check(PoolSize(context) == BC_IOCTL_DATA_POOL_SIZE,
		      "contention fixture starts with the complete command pool");
		ResetObservations();
	}

	void Prepare(bool pending = true)
	{
		ResetObservations();
		context.fw_cmd_issued = pending;
	}

	~FullPoolFixture()
	{
		context.DevHandle = -1;
		DtsReleaseMemPools(&context);
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

enum CloseFailure {
	CLOSE_POOL_EMPTY,
	CLOSE_DRIVER_STATUS,
	CLOSE_FIRMWARE_RESPONSE,
	CLOSE_SYSCALL,
};

static BC_STATUS ExpectedCloseFailure(CloseFailure failure)
{
	switch (failure) {
	case CLOSE_POOL_EMPTY:
		return BC_STS_INSUFF_RES;
	case CLOSE_DRIVER_STATUS:
		return BC_STS_IO_ERROR;
	case CLOSE_FIRMWARE_RESPONSE:
		return BC_STS_FW_CMD_ERR;
	case CLOSE_SYSCALL:
		return BC_STS_ERROR;
	}
	std::abort();
}

static void DecoderCloseOwnership(uint32_t device, CloseFailure failure)
{
	Fixture fixture(device);
	fixture.context.OpenRsp.channelId = 7;
	fixture.context.OpenRsp.channelStatus = 0xfeedfaceU;
	fixture.context.LastPicNum = 17;
	fixture.context.LastSessNum = 9;
	fixture.context.EOSCnt = 5;
	fixture.context.DrvStatusEOSCnt = 6;
	DtsSetDecStat(true, fixture.context.ProcessID);
	const auto open_response = fixture.context.OpenRsp;

	if (failure == CLOSE_POOL_EMPTY)
		fixture.context.pIoDataFreeHd = nullptr;
	else if (failure == CLOSE_DRIVER_STATUS)
		ioctl_status = BC_STS_IO_ERROR;
	else if (failure == CLOSE_FIRMWARE_RESPONSE)
		response_status = 1;
	else
		syscall_failure = true;

	const unsigned old_calls = calls;
	Check(DtsCloseDecoder(&fixture.context) == ExpectedCloseFailure(failure),
	      "failed decoder close preserves its public status");
	Check(calls == old_calls + (failure == CLOSE_POOL_EMPTY ? 0U : 1U),
	      "failed decoder close issues at most one firmware command");
	Check(fixture.context.State == BC_DEC_STATE_STOP &&
	          std::memcmp(&fixture.context.OpenRsp, &open_response,
	                      sizeof(open_response)) == 0,
	      "failed decoder close retains channel state and response identity");
	Check(fixture.globals.g_bDecOpened &&
	          fixture.globals.g_nProcID == fixture.context.ProcessID,
	      "failed decoder close retains shared decoder ownership");
	Check(fixture.context.LastPicNum == 17 &&
	          fixture.context.LastSessNum == 9 &&
	          fixture.context.EOSCnt == 5 &&
	          fixture.context.DrvStatusEOSCnt == 6,
	      "failed decoder close preserves session metadata");
	if (failure != CLOSE_POOL_EMPTY)
		fixture.PoolReturned();

	fixture.Prepare();
	const unsigned retry_calls = calls;
	const uint32_t retry_sequence = fixture.context.fwcmdseq;
	Check(DtsCloseDecoder(&fixture.context) == BC_STS_SUCCESS,
	      "decoder close can be retried after a transient failure");
	Check(calls == retry_calls + 1,
	      "successful retry issues exactly one firmware close command");
	uint32_t expected[BC_MAX_FW_CMD_BUFF_SZ] = {};
	expected[0] = eCMD_C011_DEC_CHAN_CLOSE;
	expected[1] = retry_sequence + 1;
	expected[2] = 7;
	expected[3] = eC011_PIC_REL_INTERNAL;
	expected[4] = eC011_LASTPIC_DISPLAY_ON;
	CheckFirmwarePayload(expected,
	      "decoder close retry preserves the exact reviewed wire payload");
	const decltype(fixture.context.OpenRsp) empty_response = {};
	Check(fixture.context.State == BC_DEC_STATE_CLOSE &&
	          std::memcmp(&fixture.context.OpenRsp, &empty_response,
	                      sizeof(empty_response)) == 0,
	      "successful decoder close retires channel ownership");
	Check(!fixture.globals.g_bDecOpened && fixture.globals.g_nProcID == 0,
	      "successful decoder close clears shared ownership");
	Check(fixture.context.LastPicNum == static_cast<uint32_t>(-1) &&
	          fixture.context.LastSessNum == static_cast<uint32_t>(-1) &&
	          fixture.context.EOSCnt == 0 &&
	          fixture.context.DrvStatusEOSCnt == 0,
	      "successful decoder close resets session metadata");
	fixture.PoolReturned();
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

static void ContentionEnvelopeOwnership()
{
	{
		FullPoolFixture fixture(BC_PCI_DEVID_LINK);
		for (unsigned attempt = 0; attempt < BC_IOCTL_DATA_POOL_SIZE + 2;
		     ++attempt) {
			fixture.Prepare();
			const unsigned old_calls = calls;
			Check(DtsDrvCmd(&fixture.context, BCM_IOC_FW_CMD, 0, nullptr,
			                FALSE) == BC_STS_ERROR,
			      "persistent LINK contention preserves its timeout status");
			Check(sleep_calls == 30,
			      "persistent LINK contention consumes the exact wait budget");
			Check(calls == old_calls,
			      "persistent LINK contention issues no ioctl");
			Check(fixture.context.fw_cmd_issued,
			      "rejected command does not clear the outstanding owner");
			Check(PoolSize(fixture.context) == BC_IOCTL_DATA_POOL_SIZE,
			      "repeated internal timeouts restore the complete pool");
		}
	}

	FullPoolFixture fixture(BC_PCI_DEVID_LINK);
	fixture.Prepare();
	Check(DtsDrvCmd(&fixture.context, BCM_IOC_FW_CMD, 0, nullptr,
	                TRUE) == BC_STS_ERROR,
	      "internal timeout ignores redundant transferred ownership");
	Check(sleep_calls == 30 &&
	          PoolSize(fixture.context) == BC_IOCTL_DATA_POOL_SIZE,
	      "internal transferred timeout returns its envelope once");

	fixture.Prepare();
	BC_IOCTL_DATA *borrowed = DtsAllocIoctlData(&fixture.context);
	Check(borrowed &&
	          PoolSize(fixture.context) == BC_IOCTL_DATA_POOL_SIZE - 1,
	      "caller can borrow an envelope before contention");
	Check(DtsDrvCmd(&fixture.context, BCM_IOC_FW_CMD, 0, borrowed,
	                FALSE) == BC_STS_ERROR,
	      "caller-owned envelope preserves the contention status");
	Check(sleep_calls == 30 &&
	          PoolSize(fixture.context) == BC_IOCTL_DATA_POOL_SIZE - 1,
	      "caller-owned timeout leaves the envelope borrowed");
	DtsRelIoctlData(&fixture.context, borrowed);
	Check(PoolSize(fixture.context) == BC_IOCTL_DATA_POOL_SIZE,
	      "caller can return its envelope after contention");

	fixture.Prepare();
	borrowed = DtsAllocIoctlData(&fixture.context);
	Check(DtsDrvCmd(&fixture.context, BCM_IOC_FW_CMD, 0, borrowed,
	                TRUE) == BC_STS_ERROR,
	      "transferred envelope preserves the contention status");
	Check(sleep_calls == 30 &&
	          PoolSize(fixture.context) == BC_IOCTL_DATA_POOL_SIZE,
	      "transferred timeout restores the envelope to the pool");

	fixture.Prepare();
	BC_IOCTL_DATA *pool = fixture.context.pIoDataFreeHd;
	fixture.context.pIoDataFreeHd = nullptr;
	const unsigned empty_calls = calls;
	Check(DtsDrvCmd(&fixture.context, BCM_IOC_FW_CMD, 0, nullptr,
	                FALSE) == BC_STS_INSUFF_RES,
	      "empty pool retains precedence over contention");
	Check(sleep_calls == 0 && calls == empty_calls,
	      "empty pool returns before waiting or issuing ioctl");
	Check(fixture.context.fw_cmd_issued,
	      "empty pool cannot disturb the outstanding owner");
	fixture.context.pIoDataFreeHd = pool;
	Check(PoolSize(fixture.context) == BC_IOCTL_DATA_POOL_SIZE,
	      "empty-pool fixture restores its detached list");

	fixture.Prepare();
	const unsigned invalid_calls = calls;
	Check(DtsDrvCmd(nullptr, BCM_IOC_FW_CMD, 0, nullptr, FALSE) ==
	          BC_STS_INV_ARG,
	      "null context retains precedence over firmware contention");
	fixture.context.DevHandle = -1;
	Check(DtsDrvCmd(&fixture.context, BCM_IOC_FW_CMD, 0, nullptr, FALSE) ==
	          BC_STS_INV_ARG,
	      "invalid descriptor retains precedence over firmware contention");
	fixture.context.DevHandle = 99;
	Check(sleep_calls == 0 && calls == invalid_calls &&
	          fixture.context.fw_cmd_issued &&
	          PoolSize(fixture.context) == BC_IOCTL_DATA_POOL_SIZE,
	      "invalid command paths do not wait, issue, or borrow envelopes");
}

static void ContentionWaitBoundaries()
{
	FullPoolFixture fixture(BC_PCI_DEVID_LINK);
	for (unsigned release_at : {1U, 29U}) {
		fixture.Prepare();
		clear_on_sleep = release_at;
		const unsigned old_calls = calls;
		Check(DtsDrvCmd(&fixture.context, BCM_IOC_FW_CMD, 0, nullptr,
		                FALSE) == BC_STS_SUCCESS,
		      "LINK command proceeds when the owner clears in budget");
		Check(sleep_calls == release_at && calls == old_calls + 1,
		      "released contention preserves the exact wait and ioctl count");
		Check(last_ioctl_code == BCM_IOC_FW_CMD && flag_seen_in_ioctl,
		      "released contender owns the flag while issuing firmware ioctl");
		Check(!fixture.context.fw_cmd_issued &&
		          PoolSize(fixture.context) == BC_IOCTL_DATA_POOL_SIZE,
		      "issued contender clears its flag and returns its envelope");
	}

	fixture.Prepare();
	clear_on_sleep = 30;
	const unsigned boundary_calls = calls;
	Check(DtsDrvCmd(&fixture.context, BCM_IOC_FW_CMD, 0, nullptr,
	                FALSE) == BC_STS_ERROR,
	      "owner release during the final sleep retains timeout behavior");
	Check(sleep_calls == 30 && calls == boundary_calls,
	      "final-sleep boundary performs no extra poll or ioctl");
	Check(!fixture.context.fw_cmd_issued &&
	          PoolSize(fixture.context) == BC_IOCTL_DATA_POOL_SIZE,
	      "timeout leaves an externally cleared flag unchanged");
}

static void ContentionCommandBoundaries()
{
	FullPoolFixture fixture(BC_PCI_DEVID_LINK);

	fixture.Prepare(false);
	ioctl_status = BC_STS_BUSY;
	unsigned old_calls = calls;
	Check(DtsDrvCmd(&fixture.context, BCM_IOC_FW_CMD, 0, nullptr,
	                FALSE) == BC_STS_BUSY,
	      "issued firmware command preserves driver status");
	Check(sleep_calls == 0 && calls == old_calls + 1 &&
	          flag_seen_in_ioctl && !fixture.context.fw_cmd_issued,
	      "driver-status path acquires and releases firmware ownership");
	Check(PoolSize(fixture.context) == BC_IOCTL_DATA_POOL_SIZE,
	      "driver-status path returns its internal envelope");

	fixture.Prepare(false);
	ioctl_status = BC_STS_BUSY;
	syscall_failure = true;
	old_calls = calls;
	Check(DtsDrvCmd(&fixture.context, BCM_IOC_FW_CMD, 0, nullptr,
	                FALSE) == BC_STS_ERROR,
	      "firmware ioctl syscall failure preserves the public error");
	Check(calls == old_calls + 1 && flag_seen_in_ioctl &&
	          !fixture.context.fw_cmd_issued &&
	          PoolSize(fixture.context) == BC_IOCTL_DATA_POOL_SIZE,
	      "syscall failure releases flag and internal envelope ownership");

	fixture.context.DevId = BC_PCI_DEVID_FLEA;
	fixture.Prepare();
	old_calls = calls;
	Check(DtsDrvCmd(&fixture.context, BCM_IOC_FW_CMD, 0, nullptr,
	                FALSE) == BC_STS_SUCCESS,
	      "FLEA firmware command bypasses LINK contention");
	Check(sleep_calls == 0 && calls == old_calls + 1 &&
	          fixture.context.fw_cmd_issued &&
	          PoolSize(fixture.context) == BC_IOCTL_DATA_POOL_SIZE,
	      "FLEA bypass preserves unrelated flag and envelope ownership");

	fixture.context.DevId = BC_PCI_DEVID_LINK;
	fixture.Prepare();
	old_calls = calls;
	Check(DtsDrvCmd(&fixture.context, BCM_IOC_GET_VERSION, 0, nullptr,
	                FALSE) == BC_STS_SUCCESS,
	      "non-firmware LINK command bypasses firmware contention");
	Check(sleep_calls == 0 && calls == old_calls + 1 &&
	          last_ioctl_code == BCM_IOC_GET_VERSION &&
	          fixture.context.fw_cmd_issued &&
	          PoolSize(fixture.context) == BC_IOCTL_DATA_POOL_SIZE,
	      "non-firmware bypass preserves flag and envelope ownership");
}

static void FirmwareHelperContention()
{
	Fixture fixture(BC_PCI_DEVID_LINK);
	fixture.context.fw_cmd_issued = true;
	const unsigned old_calls = calls;
	Check(DtsFWHwSelfTest(&fixture.context, eC011_TEST_SHORT_MEMORY) ==
	          BC_STS_ERROR,
	      "public firmware helper preserves contention failure");
	Check(sleep_calls == 30 && calls == old_calls,
	      "public firmware helper waits without issuing ioctl");
	Check(fixture.context.fw_cmd_issued,
	      "public firmware helper cannot clear another command owner");
	fixture.PoolReturned();
}

int main()
{
	ValidTestIds(BC_PCI_DEVID_FLEA);
	ValidTestIds(BC_PCI_DEVID_LINK);
	FailurePropagation();
	DecoderStartPayload();
	for (uint32_t device : {BC_PCI_DEVID_LINK, BC_PCI_DEVID_FLEA})
		for (CloseFailure failure : {CLOSE_POOL_EMPTY, CLOSE_DRIVER_STATUS,
		                             CLOSE_FIRMWARE_RESPONSE, CLOSE_SYSCALL})
			DecoderCloseOwnership(device, failure);
	ZeroRateValidation();
	RateErrorPrecedence();
	PositiveRateCommands();
	ContentionEnvelopeOwnership();
	ContentionWaitBoundaries();
	ContentionCommandBoundaries();
	FirmwareHelperContention();
	std::printf("Library firmware commands: %u checks, %u failures\n",
	            checks, failures);
	return failures ? 1 : 0;
}
