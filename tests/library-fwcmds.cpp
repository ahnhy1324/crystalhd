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
static bool stop_lifecycle_script, fail_allocation_once, mailbox_poisoned;
static BC_STATUS stop_status, unmap_status;
static uint32_t stop_response;
static bool stop_syscall_failure;
static unsigned stop_calls, activation_calls, close_calls, unmap_calls;
static uint32_t state_at_unmap;
static bool probe_control_owner;
static BC_STATUS activation_status, start_status;
static uint32_t activation_response, start_response;
static unsigned start_calls;
static bool fail_start_allocation, probe_lifecycle_owner;

extern "C" void DumpInputSampleToFile(uint8_t *, uint32_t) {}
extern "C" BC_STATUS DtsSetProgressive(HANDLE, uint32_t) { std::abort(); }
/* Unexecuted EOS serialization dependencies of the real DtsFlushInput. */
uint16_t WORD_SWAP(uint16_t) { std::abort(); }
void PTS2MakerBit5Bytes(uint8_t *, int64_t) { std::abort(); }
extern "C" BC_IOCTL_DATA *__real_DtsAllocIoctlData(DTS_LIB_CONTEXT *);
extern "C" BC_IOCTL_DATA *__wrap_DtsAllocIoctlData(DTS_LIB_CONTEXT *context)
{
	if (fail_allocation_once) {
		fail_allocation_once = false;
		return nullptr;
	}
	return __real_DtsAllocIoctlData(context);
}

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
	    (code != BCM_IOC_FW_CMD && code != BCM_IOC_GET_VERSION &&
	     !(stop_lifecycle_script && code == BCM_IOC_FLUSH_RX_CAP))) {
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
	if (code == BCM_IOC_FLUSH_RX_CAP) {
		++unmap_calls;
		state_at_unmap = active_context->State;
		data->RetSts = unmap_status;
		return 0;
	}
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
	if (data->u.fwCmd.cmd[0] == eCMD_C011_DEC_CHAN_OPEN) {
		DecRspChannelChannelOpen *open =
			reinterpret_cast<DecRspChannelChannelOpen *>(
				data->u.fwCmd.rsp);
		open->ChannelID = 7;
	}
	bool fail_syscall = syscall_failure;
	if (stop_lifecycle_script) {
		const uint32_t opcode = data->u.fwCmd.cmd[0];
		if (probe_lifecycle_owner) {
			const unsigned entered_calls = calls;
			Check(active_context->stopInProgress &&
			          DtsStopDecoder(active_context) == BC_STS_BUSY &&
			          DtsStartDecoder(active_context) == BC_STS_BUSY &&
			          DtsCloseDecoder(active_context) == BC_STS_BUSY &&
			          active_context->stopInProgress && calls == entered_calls,
			      "public lifecycle owner admits its real helper but rejects competing controls");
		}
		if (opcode == eCMD_C011_DEC_CHAN_STOP_VIDEO) {
			++stop_calls;
			if (probe_control_owner) {
				probe_control_owner = false;
				const unsigned entered_calls = calls;
				Check(active_context->stopInProgress &&
				          DtsStopDecoder(active_context) == BC_STS_BUSY &&
				          DtsStartDecoder(active_context) == BC_STS_BUSY &&
				          DtsCloseDecoder(active_context) == BC_STS_BUSY &&
				          active_context->stopInProgress && calls == entered_calls,
				      "in-flight real STOP retains its lifecycle owner against nested controls");
			}
			data->RetSts = stop_status;
			response->status = stop_response;
			fail_syscall = stop_syscall_failure;
		} else if (opcode == eCMD_C011_DEC_CHAN_ACTIVATE) {
			++activation_calls;
			data->RetSts = activation_status;
			response->status = activation_response;
			if (fail_start_allocation) {
				fail_start_allocation = false;
				fail_allocation_once = true;
			}
		} else if (opcode == eCMD_C011_DEC_CHAN_START_VIDEO) {
			++start_calls;
			data->RetSts = start_status;
			response->status = start_response;
		} else if (opcode == eCMD_C011_DEC_CHAN_CLOSE) {
			++close_calls;
		} else if (opcode == eCMD_C011_DEC_CHAN_PAUSE) {
			data->RetSts = BC_STS_IO_ERROR;
		} else {
			std::fputs("unexpected firmware command in STOP lifecycle test\n", stderr);
			std::abort();
		}
		if (mailbox_poisoned) data->RetSts = BC_STS_BUSY;
		if (opcode == eCMD_C011_DEC_CHAN_STOP_VIDEO && stop_status == BC_STS_TIMEOUT)
			mailbox_poisoned = true;
	}
	if (fail_syscall) {
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
	stop_lifecycle_script = fail_allocation_once = mailbox_poisoned = false;
	stop_status = unmap_status = BC_STS_SUCCESS;
	stop_response = 0;
	stop_syscall_failure = false;
	stop_calls = activation_calls = close_calls = unmap_calls = state_at_unmap = 0;
	probe_control_owner = false;
	// Failure witnesses cannot accidentally complete START; only the explicit
	// positive lifecycle cases below enable its ACTIVATE/START acknowledgements.
	activation_status = BC_STS_IO_ERROR;
	start_status = BC_STS_SUCCESS;
	activation_response = start_response = start_calls = 0;
	fail_start_allocation = probe_lifecycle_owner = false;
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
	fixture.context.activeFleaScaleWord = 0x50050001U;
	Check(DtsFWStopVideo(&fixture.context, 0, false) == BC_STS_SUCCESS,
	      "started decoder channel stops successfully");
	CheckFirmwarePayload(expected,
		"decoder stop preserves the reviewed 64-word wire payload");
	Check(fixture.context.State == BC_DEC_STATE_STOP,
	      "successful low-level stop publishes the library stop state");
	Check(fixture.context.activeFleaScaleWord == 0x50050001U,
	      "STOP preserves the successful OPEN scale identity");
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
	Check(fixture.context.activeFleaScaleWord == 0,
	      "successful low-level close clears the OPEN scale identity");
}

static void FleaOpenScaleContract()
{
	struct OpenCase {
		uint32_t device;
		uint32_t algorithm;
		uint32_t requested;
		uint32_t wire;
		uint32_t active;
	};
	const OpenCase cases[] = {
		{BC_PCI_DEVID_FLEA, BC_VID_ALGO_H264,
		 0x07f07f01U, 0x3c050001U, 0x3c050001U},
		{BC_PCI_DEVID_FLEA, BC_VID_ALGO_H264,
		 0x08008001U, 0x08008001U, 0x08008001U},
		{BC_PCI_DEVID_FLEA, BC_VID_ALGO_H264,
		 0x50050001U, 0x50050001U, 0x50050001U},
		{BC_PCI_DEVID_FLEA, BC_VID_ALGO_H264,
		 0x08108101U, 0x08208201U, 0x08208201U},
		{BC_PCI_DEVID_FLEA, BC_VID_ALGO_H264,
		 0x28028001U, 0x28028001U, 0x28028001U},
		{BC_PCI_DEVID_FLEA, BC_VID_ALGO_H264,
		 0x3c03c001U, 0x3c03c001U, 0x3c03c001U},
		{BC_PCI_DEVID_FLEA, BC_VID_ALGO_H264,
		 0x4ff4ff01U, 0x50050001U, 0x50050001U},
		{BC_PCI_DEVID_FLEA, BC_VID_ALGO_H264,
		 0x50150101U, 0x50250201U, 0x50250201U},
		{BC_PCI_DEVID_FLEA, BC_VID_ALGO_H264,
		 0x77e77e01U, 0x77e77e01U, 0x77e77e01U},
		{BC_PCI_DEVID_FLEA, BC_VID_ALGO_H264,
		 0x77f77f01U, 0x78078001U, 0x78078001U},
		{BC_PCI_DEVID_FLEA, BC_VID_ALGO_H264,
		 0x78078001U, 0x3c050001U, 0x3c050001U},
		{BC_PCI_DEVID_FLEA, BC_VID_ALGO_H264,
		 0x07f08001U, 0x3c008001U, 0x3c008001U},
		{BC_PCI_DEVID_FLEA, BC_VID_ALGO_H264,
		 0x08007f01U, 0x08050001U, 0x08050001U},
		{BC_PCI_DEVID_FLEA, BC_VID_ALGO_H264, 0, 0, 0},
		{BC_PCI_DEVID_FLEA, BC_VID_ALGO_MPEG2,
		 0x50050001U, 0x50050001U, 0},
		{BC_PCI_DEVID_LINK, BC_VID_ALGO_H264,
		 0x50050001U, 0, 0},
	};
	for (const OpenCase &test : cases) {
		Fixture fixture(test.device);
		fixture.context.State = BC_DEC_STATE_CLOSE;
		fixture.context.VidParams.VideoAlgo = test.algorithm;
		fixture.context.EnableScaling = test.requested;
		fixture.context.activeFleaScaleWord = 0xdeadbeefU;
		Check(DtsFWOpenChannel(&fixture.context, eC011_STREAM_TYPE_ES, 0) ==
		          BC_STS_SUCCESS,
		      "firmware channel OPEN succeeds");
		if (test.device == BC_PCI_DEVID_FLEA) {
			const DecCmdChannelChannelOpen *open =
				reinterpret_cast<const DecCmdChannelChannelOpen *>(
					captured_fw.cmd);
			Check(open->reservedWord8 == test.wire &&
			      open->videoAlg == (eC011_VIDEO_ALG)test.algorithm,
			      "FLEA OPEN carries the normalized scale word and codec");
		}
		Check(fixture.context.activeFleaScaleWord == test.active &&
		      fixture.context.State == BC_DEC_STATE_STOP,
		      "only successful FLEA H.264 OPEN publishes its submitted scale word");
		fixture.PoolReturned();
	}

	for (unsigned failure = 0; failure < 4; ++failure) {
		Fixture fixture(BC_PCI_DEVID_FLEA);
		fixture.context.State = BC_DEC_STATE_CLOSE;
		fixture.context.VidParams.VideoAlgo = BC_VID_ALGO_H264;
		fixture.context.EnableScaling = 0x50050001U;
		fixture.context.activeFleaScaleWord = 0xdeadbeefU;
		if (failure == 0)
			fixture.context.pIoDataFreeHd = nullptr;
		else if (failure == 1)
			ioctl_status = BC_STS_IO_ERROR;
		else if (failure == 2)
			response_status = 1;
		else
			syscall_failure = true;
		const BC_STATUS expected[] = {
			BC_STS_INSUFF_RES, BC_STS_IO_ERROR,
			BC_STS_FW_CMD_ERR, BC_STS_ERROR,
		};
		Check(DtsFWOpenChannel(&fixture.context, eC011_STREAM_TYPE_ES, 0) ==
		          expected[failure],
		      "failed OPEN preserves its exact error");
		Check(fixture.context.activeFleaScaleWord == 0 &&
		      fixture.context.State == BC_DEC_STATE_CLOSE,
		      "failed OPEN cannot retain or publish a scale identity");
		if (failure == 0)
			fixture.context.pIoDataFreeHd = &fixture.pooled;
		else
			fixture.PoolReturned();
	}

	Fixture wrong_state(BC_PCI_DEVID_FLEA);
	wrong_state.context.State = BC_DEC_STATE_STOP;
	wrong_state.context.activeFleaScaleWord = 0x50050001U;
	const unsigned old_calls = calls;
	Check(DtsFWOpenChannel(&wrong_state.context, eC011_STREAM_TYPE_ES, 0) ==
	          BC_STS_ERR_USAGE && calls == old_calls &&
	      wrong_state.context.activeFleaScaleWord == 0x50050001U,
	      "wrong-state OPEN leaves the existing channel identity untouched");

	Fixture already_closed(BC_PCI_DEVID_FLEA);
	already_closed.context.State = BC_DEC_STATE_CLOSE;
	already_closed.context.activeFleaScaleWord = 0x50050001U;
	const unsigned close_calls_before = calls;
	Check(DtsFWCloseChannel(&already_closed.context, 0) == BC_STS_SUCCESS &&
	      calls == close_calls_before &&
	      already_closed.context.activeFleaScaleWord == 0,
	      "idempotent CLOSE converges stale scale identity to zero");
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
	fixture.context.activeFleaScaleWord = 0x50050001U;
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
	          !fixture.context.stopInProgress &&
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
	Check(fixture.context.activeFleaScaleWord == 0x50050001U,
	      "failed decoder close preserves the active scale identity");
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
	          !fixture.context.stopInProgress &&
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

enum StopFailure {
	STOP_HEALTHY, STOP_POOL_EMPTY, STOP_DRIVER_STATUS, STOP_FIRMWARE_RESPONSE,
	STOP_TIMEOUT, STOP_SYSCALL, STOP_DRIVER_BUSY
};

struct StopFixture : Fixture {
	explicit StopFixture(uint32_t device) : Fixture(device)
	{
		Check(txBufInit(&context.circBuf, CIRC_TX_BUF_SIZE) == BC_STS_SUCCESS,
		      "STOP lifecycle fixture initializes the real TX ring");
		context.State = BC_DEC_STATE_START;
		context.OpenRsp.channelId = 7;
		context.OpenRsp.channelStatus = 1;
		context.bMapOutBufDirty = context.bMapOutBufDone = true;
		DtsSetDecStat(true, context.ProcessID);
		stop_lifecycle_script = true;
	}
	~StopFixture() { txBufFree(&context.circBuf); }
};

static BC_STATUS StopFault(StopFailure failure)
{
	switch (failure) {
	case STOP_HEALTHY: return BC_STS_SUCCESS;
	case STOP_POOL_EMPTY:
		fail_allocation_once = true;
		return BC_STS_INSUFF_RES;
	case STOP_DRIVER_STATUS: return stop_status = BC_STS_IO_ERROR;
	case STOP_DRIVER_BUSY: return stop_status = BC_STS_BUSY;
	case STOP_FIRMWARE_RESPONSE:
		stop_response = 1;
		return BC_STS_FW_CMD_ERR;
	case STOP_TIMEOUT: return stop_status = BC_STS_TIMEOUT;
	case STOP_SYSCALL:
		stop_syscall_failure = true;
		return BC_STS_ERROR;
	}
	std::abort();
}

static void LowLevelStopOutcomeScope(uint32_t device, StopFailure failure, unsigned mode)
{
	Fixture fixture(device);
	const bool force = mode == 1, coordinated = mode == 2;
	fixture.context.State = force ? BC_DEC_STATE_CLOSE : BC_DEC_STATE_START;
	const uint32_t old_state = fixture.context.State;
	fixture.context.stopOutcome = DTS_STOP_PENDING;
	fixture.context.stopStatus = BC_STS_BUSY;
	fixture.context.stopInProgress = force || coordinated;
	fixture.context.stopHostCleanupDone = true;
	stop_lifecycle_script = true;
	const BC_STATUS expected = StopFault(failure);
	Check(DtsFWStopVideo(&fixture.context, 7, force) == expected &&
	          stop_calls == (failure == STOP_POOL_EMPTY ? 0U : 1U) && !unmap_calls,
	      "low-level STOP retains force entry, status and submission behavior");
	DTS_STOP_OUTCOME outcome = DTS_STOP_PENDING;
	if (coordinated) {
		outcome = failure == STOP_HEALTHY ? DTS_STOP_CONFIRMED :
		    failure == STOP_POOL_EMPTY ? DTS_STOP_NOT_SUBMITTED :
		    failure == STOP_FIRMWARE_RESPONSE ? DTS_STOP_REJECTED : DTS_STOP_UNCERTAIN;
	}
	Check(fixture.context.stopOutcome == outcome &&
	          fixture.context.stopStatus == (coordinated ? expected : BC_STS_BUSY) &&
	          fixture.context.stopInProgress == (force || coordinated) &&
	          fixture.context.stopHostCleanupDone,
	      "only coordinator-owned non-forced STOP publishes outcome and never claims host cleanup");
	Check(fixture.context.State == (failure == STOP_HEALTHY ? BC_DEC_STATE_STOP : old_state),
	      "low-level STOP retains historical success-only state publication");
}

static void DecoderStopFailureOwnership(uint32_t device, StopFailure failure)
{
	StopFixture fixture(device);
	const BC_STATUS expected = StopFault(failure);
	const BC_STATUS first = DtsStopDecoder(&fixture.context);
	Check(first == expected && stop_calls == (failure == STOP_POOL_EMPTY ? 0U : 1U),
	      "public STOP retains real helper allocation/driver/response status");
	Check(unmap_calls == 1 && !fixture.context.bMapOutBufDirty &&
	          !fixture.context.bMapOutBufDone,
	      "one-shot STOP failure does not prevent acknowledged host RX cleanup");
	fixture.PoolReturned();
	if (failure == STOP_HEALTHY) {
		Check(fixture.context.State == BC_DEC_STATE_STOP && !fixture.context.txQuiescing &&
		          state_at_unmap == BC_DEC_STATE_STOP,
		      "real firmware helper commits STOP before successful outer RX cleanup");
		Check(DtsStopDecoder(&fixture.context) == BC_STS_SUCCESS &&
		          stop_calls == 1 && unmap_calls == 1,
		      "fully successful repeated STOP remains an idempotent no-op");
		return;
	}
	const unsigned activations = activation_calls;
	Check(DtsStartDecoder(&fixture.context) != BC_STS_SUCCESS &&
	          activation_calls == activations,
	      "unresolved real firmware STOP blocks START before activation");
	const unsigned before_retry = stop_calls;
	const BC_STATUS repeated = DtsStopDecoder(&fixture.context);
	if (failure == STOP_POOL_EMPTY)
		Check(repeated == BC_STS_SUCCESS && stop_calls == before_retry + 1,
		      "explicit repeated STOP retries a proven one-shot pre-submission allocation failure");
	else
		Check(repeated == expected && stop_calls == before_retry,
		      "repeated STOP preserves rejection/transport failure without replay or false success");
	if (failure == STOP_TIMEOUT) {
		Check(repeated != BC_STS_SUCCESS,
		      "repeated STOP cannot falsely resolve an uncertain timeout");
		Check(stop_calls == before_retry,
		      "uncertain STOP timeout is not automatically retransmitted");
	}
	Check(unmap_calls == 1, "already acknowledged RX cleanup is not repeated");
	const BC_STATUS closed = DtsCloseDecoder(&fixture.context);
	if (failure == STOP_TIMEOUT) {
		Check(closed == expected && fixture.context.State != BC_DEC_STATE_CLOSE &&
		          fixture.globals.g_bDecOpened && stop_calls == before_retry,
		      "timeout quarantine is not bypassed by CLOSE or another STOP");
	} else {
		Check(closed == (failure == STOP_POOL_EMPTY ? BC_STS_SUCCESS : expected) &&
		          close_calls == 1 && fixture.context.State == BC_DEC_STATE_CLOSE &&
		          !fixture.globals.g_bDecOpened,
		      "host-clean unresolved STOP retains a confirmed CLOSE route and original error");
	}
}

static void StopLifecycleOwner(uint32_t device)
{
	StopFixture fixture(device);
	probe_control_owner = true;
	Check(DtsStopDecoder(&fixture.context) == BC_STS_SUCCESS && !probe_control_owner &&
	          stop_calls == 1 && unmap_calls == 1 && !fixture.context.stopInProgress,
	      "real STOP owns one complete command/host cleanup interval then releases admission");
}

static void PublicStartStopCycle(uint32_t device)
{
	StopFixture fixture(device);
	fixture.context.State = BC_DEC_STATE_STOP;
	activation_status = BC_STS_SUCCESS;
	probe_lifecycle_owner = true;
	// Progressive mode is off here, so both firmware helpers are real without
	// entering the unrelated DtsSetProgressive stub. No capture/video is run.
	Check(DtsStartDecoder(&fixture.context) == BC_STS_SUCCESS &&
	          activation_calls == 1 && start_calls == 1 &&
	          fixture.context.State == BC_DEC_STATE_START &&
	          fixture.context.stopOutcome == DTS_STOP_NONE && !fixture.context.stopInProgress,
	      "public START passes ACTIVATE and START acknowledgements without self-blocking");
	Check(DtsStopDecoder(&fixture.context) == BC_STS_SUCCESS && stop_calls == 1 &&
	          unmap_calls == 1 && fixture.context.State == BC_DEC_STATE_STOP &&
	          fixture.context.stopOutcome == DTS_STOP_CONFIRMED &&
	          fixture.context.stopHostCleanupDone && !fixture.context.stopInProgress,
	      "public STOP fully retires the first acknowledged START and releases its owner");
	Check(DtsStartDecoder(&fixture.context) == BC_STS_SUCCESS &&
	          activation_calls == 2 && start_calls == 2 && stop_calls == 1 &&
	          fixture.context.State == BC_DEC_STATE_START &&
	          fixture.context.stopOutcome == DTS_STOP_NONE &&
	          !fixture.context.stopHostCleanupDone && !fixture.context.stopInProgress,
	      "public restart clears only a fully resolved STOP obligation after both helper acknowledgements");
	Check(DtsStartDecoder(&fixture.context) == BC_STS_SUCCESS &&
	          activation_calls == 3 && start_calls == 3 && stop_calls == 2 &&
	          !fixture.context.stopInProgress,
	      "START from an active session performs its ordinary STOP before claiming the next START owner");
	Check(DtsCloseDecoder(&fixture.context) == BC_STS_SUCCESS && stop_calls == 3 &&
	          close_calls == 1 && unmap_calls == 1 &&
	          fixture.context.State == BC_DEC_STATE_CLOSE &&
	          fixture.context.stopOutcome == DTS_STOP_NONE &&
	          !fixture.context.txQuiescing && !fixture.context.stopInProgress &&
	          !fixture.globals.g_bDecOpened,
	      "public CLOSE acknowledges the final STOP/channel close and clears lifecycle ownership");
	const uint32_t expected[] = {eCMD_C011_DEC_CHAN_ACTIVATE, eCMD_C011_DEC_CHAN_START_VIDEO,
	    eCMD_C011_DEC_CHAN_STOP_VIDEO, eCMD_C011_DEC_CHAN_ACTIVATE, eCMD_C011_DEC_CHAN_START_VIDEO,
	    eCMD_C011_DEC_CHAN_STOP_VIDEO, eCMD_C011_DEC_CHAN_ACTIVATE, eCMD_C011_DEC_CHAN_START_VIDEO,
	    eCMD_C011_DEC_CHAN_STOP_VIDEO, eCMD_C011_DEC_CHAN_CLOSE};
	bool exact = command_log.size() == sizeof(expected) / sizeof(expected[0]);
	for (unsigned i = 0; i < command_log.size(); i++)
		exact &= i < sizeof(expected) / sizeof(expected[0]) && command_log[i].cmd[0] == expected[i];
	Check(exact, "public start/stop/restart/close emits only its ordered stock firmware commands");
	const unsigned finished_calls = calls;
	Check(DtsCloseDecoder(&fixture.context) == BC_STS_SUCCESS &&
	          DtsStartDecoder(&fixture.context) == BC_STS_DEC_NOT_OPEN && calls == finished_calls,
	      "completed CLOSE is idempotent and cannot reopen through START");
}

static void PublicStartFailureOwner(uint32_t device, bool video_start, unsigned fault)
{
	StopFixture fixture(device);
	fixture.context.State = BC_DEC_STATE_STOP;
	fixture.context.bMapOutBufDirty = fixture.context.bMapOutBufDone = false;
	activation_status = BC_STS_SUCCESS;
	probe_lifecycle_owner = true;
	BC_STATUS expected = BC_STS_SUCCESS;
	if (fault == 0) {
		fail_allocation_once = !video_start;
		fail_start_allocation = video_start;
		expected = BC_STS_INSUFF_RES;
	} else if (fault == 1) {
		(video_start ? start_status : activation_status) = BC_STS_IO_ERROR;
		expected = BC_STS_IO_ERROR;
	} else {
		(video_start ? start_response : activation_response) = 1;
		expected = BC_STS_FW_CMD_ERR;
	}
	Check(DtsStartDecoder(&fixture.context) == expected &&
	          activation_calls == (video_start || fault ? 1U : 0U) &&
	          start_calls == (video_start && fault ? 1U : 0U) &&
	          fixture.context.State == BC_DEC_STATE_STOP &&
	          fixture.context.stopOutcome == DTS_STOP_NONE && !fixture.context.stopInProgress,
	      "failed ACTIVATE/START preserves its error and releases the public START owner");
	Check(DtsCloseDecoder(&fixture.context) == BC_STS_SUCCESS && close_calls == 1 &&
	          !stop_calls && !fixture.context.stopInProgress &&
	          fixture.context.State == BC_DEC_STATE_CLOSE,
	      "a returned START failure does not leave the control owner blocking channel retirement");
}

static void DecoderCloseFirstStopFailure(uint32_t device, StopFailure failure)
{
	StopFixture fixture(device);
	const BC_STATUS expected = StopFault(failure);
	Check(DtsCloseDecoder(&fixture.context) == expected &&
	          stop_calls == (failure == STOP_POOL_EMPTY ? 0U : 1U) &&
	          unmap_calls == 1 && close_calls == 1,
	      "same-operation CLOSE preserves first STOP failure without an implicit STOP replay");
	const bool close_confirmed = failure != STOP_TIMEOUT;
	Check((fixture.context.State == BC_DEC_STATE_CLOSE) == close_confirmed &&
	          fixture.globals.g_bDecOpened == !close_confirmed &&
	          !fixture.context.bMapOutBufDirty,
	      "CLOSE confirmation and host RX cleanup remain distinct after real STOP failure");
}

static void DecoderStopCleanupRetry(uint32_t device, bool close_retry)
{
	StopFixture fixture(device);
	unmap_status = BC_STS_IO_ERROR;
	Check(DtsStopDecoder(&fixture.context) == BC_STS_IO_ERROR && stop_calls == 1 &&
	          unmap_calls == 1 && state_at_unmap == BC_DEC_STATE_STOP &&
	          fixture.context.bMapOutBufDirty && fixture.context.txQuiescing,
	      "real helper STOP success followed by failed RX unmap retains cleanup ownership");
	Check(DtsStartDecoder(&fixture.context) != BC_STS_SUCCESS && !activation_calls,
	      "incomplete host cleanup blocks activation after confirmed firmware STOP");
	const BC_STATUS repeated = close_retry ? DtsCloseDecoder(&fixture.context) :
	                                       DtsStopDecoder(&fixture.context);
	Check(repeated != BC_STS_SUCCESS && stop_calls == 1 && unmap_calls == 2 &&
	          !close_calls && fixture.context.bMapOutBufDirty &&
	          fixture.globals.g_bDecOpened,
	      "STOP/CLOSE retry repeats only pending RX cleanup, not firmware STOP or premature CLOSE");
	unmap_status = BC_STS_SUCCESS;
	const BC_STATUS recovered = close_retry ? DtsCloseDecoder(&fixture.context) :
	                                        DtsStopDecoder(&fixture.context);
	Check(recovered == BC_STS_SUCCESS && stop_calls == 1 && unmap_calls == 3 &&
	          !fixture.context.bMapOutBufDirty && !fixture.context.txQuiescing &&
	          fixture.context.State == (close_retry ? BC_DEC_STATE_CLOSE : BC_DEC_STATE_STOP) &&
	          close_calls == (close_retry ? 1U : 0U),
	      "acknowledged RX cleanup resolves the pending STOP without a duplicate firmware command");
}

static void FlushDoesNotResolveStop(uint32_t device, unsigned operation)
{
	StopFixture fixture(device);
	stop_response = 1;
	Check(DtsStopDecoder(&fixture.context) == BC_STS_FW_CMD_ERR && unmap_calls == 1,
	      "flush bypass test begins with unresolved STOP and acknowledged RX cleanup");
	const unsigned before_flush = calls;
	(void)DtsFlushInput(&fixture.context, 3);
	Check(calls == before_flush, "flush-op3 cannot replay an unresolved firmware STOP");
	const BC_STATUS status = operation == 0 ? DtsStartDecoder(&fixture.context) :
	    operation == 1 ? DtsPauseDecoder(&fixture.context) : DtsResumeDecoder(&fixture.context);
	Check(status != BC_STS_SUCCESS && calls == before_flush && !activation_calls,
	      "flush-op3 cannot erase unresolved STOP admission for START/PAUSE/RESUME");
}

static void StopProcessOwnership(uint32_t device)
{
	StopFixture fixture(device);
	fixture.globals.g_nProcID = fixture.context.ProcessID + 1;
	const unsigned before = calls;
	Check(DtsStopDecoder(&fixture.context) == BC_STS_ERROR &&
	          DtsStartDecoder(&fixture.context) == BC_STS_ERROR &&
	          DtsCloseDecoder(&fixture.context) == BC_STS_ERROR &&
	          DtsPauseDecoder(&fixture.context) == BC_STS_ERROR &&
	          DtsResumeDecoder(&fixture.context) == BC_STS_ERROR,
	      "STOP lifecycle preserves process-ownership rejection across public controls");
	Check(calls == before && !stop_calls && !unmap_calls && !close_calls &&
	          fixture.context.State == BC_DEC_STATE_START &&
	          !fixture.context.txQuiescing && fixture.context.bMapOutBufDirty,
	      "process-ownership refusal cannot issue commands or retire capture ownership");
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
	FleaOpenScaleContract();
	for (uint32_t device : {BC_PCI_DEVID_LINK, BC_PCI_DEVID_FLEA}) {
		for (StopFailure failure : {STOP_HEALTHY, STOP_POOL_EMPTY, STOP_DRIVER_STATUS,
		                           STOP_FIRMWARE_RESPONSE, STOP_TIMEOUT, STOP_SYSCALL,
		                           STOP_DRIVER_BUSY}) {
			for (unsigned mode = 0; mode < 3; mode++)
				LowLevelStopOutcomeScope(device, failure, mode);
			DecoderStopFailureOwnership(device, failure);
			if (failure != STOP_HEALTHY) DecoderCloseFirstStopFailure(device, failure);
		}
		DecoderStopCleanupRetry(device, false);
		DecoderStopCleanupRetry(device, true);
		for (unsigned operation = 0; operation < 3; operation++)
			FlushDoesNotResolveStop(device, operation);
		StopProcessOwnership(device);
		StopLifecycleOwner(device);
		PublicStartStopCycle(device);
		for (unsigned stage = 0; stage < 2; stage++)
			for (unsigned fault = 0; fault < 3; fault++)
				PublicStartFailureOwner(device, stage != 0, fault);
	}
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
