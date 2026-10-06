// SPDX-License-Identifier: LGPL-2.1-or-later
/* Hardware-free file parsing and runtime version marshalling against the
 * production public API. File and driver operations are deterministic mocks.
 */
#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "7411d.h"
#include "libcrystalhd_fwcmds.h"
#include "libcrystalhd_if.h"
#include "libcrystalhd_priv.h"

static const size_t kMaxFileSize = 0x300000U;
static const char kMarker[] = "Media_PC_FW_Rev";
static const char kFirmwarePath[] = "/mock/firmware.bin";
static FILE *const kFileToken =
	reinterpret_cast<FILE *>(static_cast<uintptr_t>(1));

static unsigned checks;
static unsigned failures;
static unsigned open_calls;
static unsigned seek_calls;
static unsigned tell_calls;
static unsigned allocation_calls;
static unsigned read_calls;
static unsigned free_calls;
static unsigned close_calls;
static size_t allocation_size;
static bool track_call;
static bool open_succeeds;
static bool end_seek_fails;
static bool rewind_fails;
static bool allocation_fails;
static bool stream_error;
static size_t stale_marker_offset;
static long reported_size;
static size_t read_limit;
static size_t file_position;
static void *live_allocation;
static std::vector<uint8_t> file_data;
static DTS_LIB_CONTEXT context;
static bool runtime_call;
static bool runtime_allocation_fails;
static bool runtime_borrowed;
static unsigned runtime_allocation_calls;
static unsigned runtime_release_calls;
static unsigned runtime_driver_calls;
static BC_STATUS runtime_driver_status;
static uint32_t runtime_response_status;
static uint32_t runtime_versions[3];
static uint32_t runtime_first_sequence;
static BC_FW_CMD runtime_command;
static struct {
	uint32_t before;
	BC_IOCTL_DATA data;
	uint32_t after;
} runtime_envelope;

static void Check(bool condition, const char *message)
{
	++checks;
	if (!condition) {
		std::fprintf(stderr, "FAIL: %s\n", message);
		++failures;
	}
}

static void PutMarker(std::vector<uint8_t> *data, size_t offset,
		      uint8_t major, uint8_t minor, uint8_t revision)
{
	Check(offset >= 4U && offset + sizeof(kMarker) - 1U <= data->size(),
	      "test marker fits with its preceding version bytes");
	if (offset < 4U || offset + sizeof(kMarker) - 1U > data->size())
		return;
	(*data)[offset - 4U] = major;
	(*data)[offset - 3U] = minor;
	(*data)[offset - 2U] = revision;
	std::memcpy(data->data() + offset, kMarker, sizeof(kMarker) - 1U);
}

static void PutMarker(uint8_t *data, size_t size, size_t offset,
		      uint8_t major, uint8_t minor, uint8_t revision)
{
	if (offset < 4U || offset + sizeof(kMarker) - 1U > size)
		return;
	data[offset - 4U] = major;
	data[offset - 3U] = minor;
	data[offset - 2U] = revision;
	std::memcpy(data + offset, kMarker, sizeof(kMarker) - 1U);
}

extern "C" FILE *__real_fopen(const char *, const char *);
extern "C" int __real_fseek(FILE *, long, int);
extern "C" long __real_ftell(FILE *);
extern "C" size_t __real_fread(void *, size_t, size_t, FILE *);
extern "C" int __real_fclose(FILE *);
extern "C" void *__real_malloc(size_t);
extern "C" void __real_free(void *);

extern "C" FILE *__wrap_fopen(const char *path, const char *mode)
{
	if (!track_call)
		return __real_fopen(path, mode);
	++open_calls;
	Check(path && std::strcmp(path, kFirmwarePath) == 0,
	      "version query opens the requested firmware path");
	Check(mode && std::strcmp(mode, "rb") == 0,
	      "version query opens firmware in binary mode");
	file_position = 0;
	return open_succeeds ? kFileToken : nullptr;
}

extern "C" int __wrap_fseek(FILE *file, long offset, int origin)
{
	if (!track_call)
		return __real_fseek(file, offset, origin);
	Check(file == kFileToken, "seek uses the open firmware stream");
	Check(offset == 0, "firmware size seeks use zero offsets");
	++seek_calls;
	if (origin == SEEK_END) {
		if (end_seek_fails)
			return -1;
		file_position = file_data.size();
		return 0;
	}
	Check(origin == SEEK_SET, "firmware rewind uses SEEK_SET");
	if (rewind_fails)
		return -1;
	file_position = 0;
	return 0;
}

extern "C" long __wrap_ftell(FILE *file)
{
	if (!track_call)
		return __real_ftell(file);
	Check(file == kFileToken, "size query uses the open firmware stream");
	++tell_calls;
	return reported_size;
}

static size_t MockRead(void *buffer, size_t size, size_t count, FILE *file)
{
	Check(track_call && file == kFileToken,
	      "read uses the open firmware stream");
	Check(buffer == live_allocation,
	      "read targets the live firmware allocation");
	Check(size == 1U, "firmware version read is byte-addressed");
	++read_calls;
	if (stream_error)
		return 0;
	const size_t available = file_position < file_data.size() ?
		file_data.size() - file_position : 0U;
	const size_t copied = std::min(count, std::min(read_limit, available));
	if (copied)
		std::memcpy(buffer, file_data.data() + file_position, copied);
	file_position += copied;
	return copied;
}

extern "C" size_t __wrap_fread(void *buffer, size_t size, size_t count,
				       FILE *file)
{
	return MockRead(buffer, size, count, file);
}

extern "C" size_t __wrap___fread_chk(void *buffer, size_t buffer_size,
				     size_t size, size_t count, FILE *file)
{
	Check(size == 0U || count <= buffer_size / size,
	      "fortified read fits the destination allocation");
	return MockRead(buffer, size, count, file);
}

extern "C" int __wrap_fclose(FILE *file)
{
	if (!track_call)
		return __real_fclose(file);
	Check(file == kFileToken, "close uses the open firmware stream");
	++close_calls;
	return 0;
}

extern "C" void *__wrap_malloc(size_t size)
{
	if (!track_call)
		return __real_malloc(size);
	++allocation_calls;
	allocation_size = size;
	if (allocation_fails)
		return nullptr;
	Check(live_allocation == nullptr,
	      "version query owns at most one firmware allocation");
	live_allocation = __real_malloc(size);
	if (live_allocation) {
		std::memset(live_allocation, 0, size);
		if (stale_marker_offset != SIZE_MAX)
			PutMarker(static_cast<uint8_t *>(live_allocation), size,
				  stale_marker_offset, 9U, 9U, 9U);
	}
	return live_allocation;
}

extern "C" void __wrap_free(void *pointer)
{
	if (track_call && pointer) {
		Check(pointer == live_allocation,
		      "version query frees only its live allocation");
		if (pointer == live_allocation) {
			++free_calls;
			live_allocation = nullptr;
		}
	}
	__real_free(pointer);
}

extern "C" DTS_LIB_CONTEXT *DtsGetContext(HANDLE handle)
{
	if (handle != &context || context.Sig != LIB_CTX_SIG)
		return nullptr;
	return &context;
}

extern "C" BC_STATUS DtsGetFirmwareFiles(DTS_LIB_CONTEXT *)
{
	std::abort();
}

extern "C" int __wrap_ioctl(int, unsigned long, ...)
{
	std::fputs("unexpected ioctl in hardware-free version test\n", stderr);
	std::abort();
}

BC_IOCTL_DATA *DtsAllocIoctlData(DTS_LIB_CONTEXT *ctx)
{
	Check(runtime_call && ctx == &context,
	      "runtime allocation uses the validated context");
	++runtime_allocation_calls;
	Check(!runtime_borrowed, "runtime query borrows at most one ioctl object");
	if (runtime_allocation_fails)
		return nullptr;
	std::memset(&runtime_envelope.data, 0, sizeof(runtime_envelope.data));
	runtime_borrowed = true;
	return &runtime_envelope.data;
}

void DtsRelIoctlData(DTS_LIB_CONTEXT *ctx, BC_IOCTL_DATA *data)
{
	Check(runtime_call && ctx == &context && runtime_borrowed &&
		      data == &runtime_envelope.data,
	      "runtime query returns its exact borrowed object once");
	++runtime_release_calls;
	runtime_borrowed = false;
}

BC_STATUS DtsDrvCmd(DTS_LIB_CONTEXT *ctx, DWORD code, BOOL async,
		   BC_IOCTL_DATA *data, BOOL release)
{
	Check(runtime_call && ctx == &context && runtime_borrowed &&
		      data == &runtime_envelope.data,
	      "runtime command uses its live borrowed ioctl object");
	Check(code == BCM_IOC_FW_CMD && async == TRUE && release == FALSE,
	      "runtime query requests the existing firmware-command driver path");
	++runtime_driver_calls;
	runtime_command = data->u.fwCmd;
	bool exact = runtime_command.flags == 0 && runtime_command.add_data == 0;
	for (unsigned word = 0; word < BC_MAX_FW_CMD_BUFF_SZ; ++word) {
		const uint32_t expected = word == 0 ? uint32_t(eCMD_C011_GET_VERSION) :
			word == 1 ? runtime_first_sequence + runtime_driver_calls : 0U;
		exact &= runtime_command.cmd[word] == expected &&
			 runtime_command.rsp[word] == 0;
	}
	Check(exact, "GET_VERSION carries only its command and one sequence increment");
	C011RspGetVersion *response =
		reinterpret_cast<C011RspGetVersion *>(data->u.fwCmd.rsp);
	response->command = eCMD_C011_GET_VERSION;
	response->sequence = runtime_command.cmd[1];
	response->status = runtime_response_status;
	response->streamSwVersion = runtime_versions[0];
	response->decoderSwVersion = runtime_versions[1];
	response->chipHwVersion = runtime_versions[2];
	return runtime_driver_status;
}

static void Reset(size_t size)
{
	track_call = false;
	if (live_allocation) {
		__real_free(live_allocation);
		live_allocation = nullptr;
	}
	file_data.assign(size, 0);
	std::memset(&context, 0, sizeof(context));
	context.Sig = LIB_CTX_SIG;
	std::strcpy(context.DilPath, "/mock/");
	open_calls = seek_calls = tell_calls = allocation_calls = 0;
	read_calls = free_calls = close_calls = 0;
	allocation_size = 0;
	open_succeeds = true;
	end_seek_fails = false;
	rewind_fails = false;
	allocation_fails = false;
	stream_error = false;
	stale_marker_offset = SIZE_MAX;
	reported_size = static_cast<long>(size);
	read_limit = SIZE_MAX;
	file_position = 0;
}

static BC_STATUS Query(uint32_t *stream, uint32_t *decoder)
{
	char name[] = "firmware.bin";
	track_call = true;
	const BC_STATUS status =
		DtsGetFWVersionFromFile(&context, stream, decoder, name);
	track_call = false;
	return status;
}

static void CleanupLeak()
{
	if (live_allocation) {
		__real_free(live_allocation);
		live_allocation = nullptr;
	}
}

static void CheckInvalidArguments()
{
	Reset(128U);
	uint32_t stream = 0xaaaaaaaaU;
	char name[] = "firmware.bin";
	track_call = true;
	Check(DtsGetFWVersionFromFile(nullptr, &stream, nullptr, name) ==
		      BC_STS_INV_ARG,
	      "invalid handle is rejected");
	Check(DtsGetFWVersionFromFile(&context, nullptr, nullptr, name) ==
		      BC_STS_INSUFF_RES,
	      "null stream output retains its legacy status");
	track_call = false;
	Check(open_calls == 0 && allocation_calls == 0,
	      "invalid arguments perform no file or heap work");
}

static void CheckEarlyMarkerAndPublicOutputs()
{
	Reset(0x4100U + sizeof(kMarker));
	PutMarker(&file_data, 0x12cU, 1U, 54U, 0U);
	PutMarker(&file_data, 0x4000U, 9U, 9U, 9U);
	uint32_t stream = 0xaaaaaaaaU;
	uint32_t decoder = 0xbbbbbbbbU;
	Check(Query(&stream, &decoder) == BC_STS_SUCCESS,
	      "bounded parser accepts valid binary firmware metadata");
	Check(stream == 0x013600U,
	      "the first valid marker before the legacy 16 KiB offset wins");
	Check(decoder == 0U,
	      "unavailable decoder file metadata is deterministic");
	Check(allocation_calls == 1 && read_calls == 1 && free_calls == 1 &&
		      close_calls == 1 && live_allocation == nullptr,
	      "successful version query balances file and allocation ownership");
	Check(seek_calls == 2 && tell_calls == 1 &&
		      allocation_size == file_data.size(),
	      "successful query validates length and allocates only the file size");

	Reset(0x4100U + sizeof(kMarker));
	PutMarker(&file_data, 0x12cU, 1U, 54U, 0U);
	PutMarker(&file_data, 0x4000U, 9U, 9U, 9U);
	stream = decoder = 0xccccccccU;
	uint32_t hardware = 0xddddddddU;
	char name[] = "firmware.bin";
	track_call = true;
	const BC_STATUS status = DtsGetFWVersion(&context, &stream, &decoder,
						 &hardware, name, 0);
	track_call = false;
	Check(status == BC_STS_SUCCESS && stream == 0x013600U &&
		      decoder == 0U && hardware == 0U,
	      "public file query initializes every available version output");
	CleanupLeak();
}

static void CheckKnownLayoutsAndLimit()
{
	Reset(0x36600U);
	PutMarker(&file_data, 0x36580U, 60U, 42U, 1U);
	uint32_t stream = 0xaaaaaaaaU;
	uint32_t decoder = 0xbbbbbbbbU;
	Check(Query(&stream, &decoder) == BC_STS_SUCCESS &&
		      stream == 0x3c2a01U,
	      "bounded parser preserves the shipped LINK metadata layout");
	Check(close_calls == 1 && free_calls == 1,
	      "LINK-layout query balances ownership");

	Reset(kMaxFileSize);
	PutMarker(&file_data, kMaxFileSize - (sizeof(kMarker) - 1U),
		  10U, 11U, 12U);
	stream = 0xaaaaaaaaU;
	decoder = 0xbbbbbbbbU;
	Check(Query(&stream, &decoder) == BC_STS_SUCCESS &&
		      stream == 0x0a0b0cU,
	      "exact maximum-size firmware and final marker are accepted");
	Check(allocation_size == kMaxFileSize && close_calls == 1 &&
		      free_calls == 1,
	      "exact limit allocates only the validated file and releases it");
	CleanupLeak();
}

static void CheckFinalMarkerBoundary()
{
	Reset(4U + sizeof(kMarker) - 1U);
	PutMarker(&file_data, 4U, 7U, 8U, 9U);
	uint32_t stream = 0xaaaaaaaaU;
	uint32_t decoder = 0xbbbbbbbbU;
	Check(Query(&stream, &decoder) == BC_STS_SUCCESS &&
		      stream == 0x070809U,
	      "marker at the final legal byte is found across binary NULs");
	Check(close_calls == 1 && free_calls == 1,
	      "minimal successful file is fully released");
	CleanupLeak();
}

static void CheckInvalidFiles()
{
	struct Case {
		const char *name;
		size_t size;
		long length;
		size_t limit;
		bool error;
		bool valid_prefix;
		size_t stale_offset;
	};
	const Case cases[] = {
		{"empty file", 0U, 0L, SIZE_MAX, false, false, SIZE_MAX},
		{"negative length", 0x4100U, -1L, SIZE_MAX, false, false,
		 SIZE_MAX},
		{"oversized length", 0x4100U,
		 static_cast<long>(kMaxFileSize + 1U), SIZE_MAX, false, false,
		 SIZE_MAX},
		{"short read with valid prefix", 0x4100U, 0x4100L, 0x200U,
		 false, true, SIZE_MAX},
		{"short read with stale tail", 0x4100U, 0x4100L, 0x200U,
		 false, false, 0x3000U},
		{"stream error", 0x4100U, 0x4100L, SIZE_MAX, true, false,
		 SIZE_MAX},
		{"missing marker", 0x4100U, 0x4100L, SIZE_MAX, false, false,
		 SIZE_MAX},
	};
	for (const Case &test : cases) {
		Reset(test.size);
		if (test.valid_prefix)
			PutMarker(&file_data, 0x12cU, 1U, 54U, 0U);
		reported_size = test.length;
		read_limit = test.limit;
		stream_error = test.error;
		stale_marker_offset = test.stale_offset;
		uint32_t stream = 0xaaaaaaaaU;
		uint32_t decoder = 0xbbbbbbbbU;
		const BC_STATUS status = Query(&stream, &decoder);
		Check(status != BC_STS_SUCCESS, test.name);
		Check(stream == 0xaaaaaaaaU && decoder == 0xbbbbbbbbU,
		      "failed version query leaves caller outputs unchanged");
		Check(close_calls == 1,
		      "invalid firmware still closes its opened stream");
		Check(live_allocation == nullptr,
		      "invalid firmware leaves no live allocation");
		const bool invalid_length = test.length <
			static_cast<long>(4U + sizeof(kMarker) - 1U) ||
			test.length > static_cast<long>(kMaxFileSize);
		Check(allocation_calls == (invalid_length ? 0U : 1U),
		      "length validation occurs before allocation");
		CleanupLeak();
	}
}

static void CheckMalformedMetadata()
{
	for (unsigned kind = 0; kind < 3U; ++kind) {
		Reset(128U);
		if (kind == 0U)
			std::memcpy(file_data.data() + 2U, kMarker,
				    sizeof(kMarker) - 1U);
		else if (kind == 1U)
			PutMarker(&file_data, 64U, 0U, 0U, 0U);
		else
			std::memcpy(file_data.data() + file_data.size() - 5U,
				    kMarker, 5U);
		uint32_t stream = 0xaaaaaaaaU;
		uint32_t decoder = 0xbbbbbbbbU;
		Check(Query(&stream, &decoder) == BC_STS_ERROR,
		      "truncated or zero version metadata is rejected");
		Check(stream == 0xaaaaaaaaU && decoder == 0xbbbbbbbbU,
		      "malformed metadata cannot publish output");
		Check(close_calls == 1 && free_calls == 1 &&
			      live_allocation == nullptr,
		      "malformed metadata releases all resources");
	}
}

static void CheckFailures()
{
	Reset(0x4100U + sizeof(kMarker));
	open_succeeds = false;
	uint32_t stream = 0xaaaaaaaaU;
	uint32_t decoder = 0xbbbbbbbbU;
	Check(Query(&stream, &decoder) == BC_STS_INSUFF_RES,
	      "open failure retains its legacy status");
	Check(allocation_calls == 0 && close_calls == 0,
	      "failed open acquires no later resources");

	Reset(0x4100U + sizeof(kMarker));
	allocation_fails = true;
	Check(Query(&stream, &decoder) == BC_STS_INSUFF_RES,
	      "allocation failure retains its legacy status");
	Check(close_calls == 1 && live_allocation == nullptr,
	      "allocation failure closes the already-opened file");

	for (unsigned failure = 0; failure < 2U; ++failure) {
		Reset(0x4100U + sizeof(kMarker));
		PutMarker(&file_data, 0x4000U, 4U, 5U, 6U);
		end_seek_fails = failure == 0U;
		rewind_fails = failure == 1U;
		stream = 0xaaaaaaaaU;
		decoder = 0xbbbbbbbbU;
		Check(Query(&stream, &decoder) != BC_STS_SUCCESS,
		      "seek failure is propagated");
		Check(close_calls == 1 && live_allocation == nullptr,
		      "seek failure releases all acquired resources");
		CleanupLeak();
	}
}

struct RuntimeOutputs {
	uint32_t before;
	uint32_t values[3];
	uint32_t after;
};

static RuntimeOutputs RuntimeSentinels()
{
	return {0x1234abcdU, {0xaaaaaaaaU, 0xbbbbbbbbU, 0xccccccccU},
		0x9876fedcU};
}

static void ResetRuntime(uint32_t sequence = 40U)
{
	Reset(0);
	runtime_call = false;
	runtime_allocation_fails = false;
	runtime_borrowed = false;
	runtime_allocation_calls = runtime_release_calls = runtime_driver_calls = 0;
	runtime_driver_status = BC_STS_SUCCESS;
	runtime_response_status = 0;
	runtime_versions[0] = 0x010203U;
	runtime_versions[1] = 0x10203040U;
	runtime_versions[2] = 0x12345678U;
	runtime_first_sequence = context.fwcmdseq = sequence;
	std::memset(&runtime_command, 0xa5, sizeof(runtime_command));
	runtime_envelope.before = 0x76543210U;
	runtime_envelope.after = 0xfedcba98U;
}

static BC_STATUS RuntimeQuery(HANDLE handle, uint32_t *stream,
			      uint32_t *decoder, uint32_t *hardware)
{
	track_call = runtime_call = true;
	const BC_STATUS status = DtsGetFWVersion(handle, stream, decoder, hardware,
					       nullptr, 1);
	track_call = runtime_call = false;
	return status;
}

static BC_STATUS RuntimeQuery(RuntimeOutputs *outputs)
{
	return RuntimeQuery(&context, &outputs->values[0], &outputs->values[1],
			    &outputs->values[2]);
}

static void CheckRuntimeOwnership(unsigned allocations, unsigned commands,
				  unsigned releases, const RuntimeOutputs &outputs)
{
	Check(runtime_allocation_calls == allocations &&
		      runtime_driver_calls == commands && runtime_release_calls == releases &&
		      !runtime_borrowed,
	      "runtime query balances the exact allocation, command and release counts");
	Check(context.fwcmdseq == runtime_first_sequence + commands,
	      "runtime query consumes one sequence only after successful allocation");
	Check(runtime_envelope.before == 0x76543210U &&
		      runtime_envelope.after == 0xfedcba98U &&
		      outputs.before == 0x1234abcdU && outputs.after == 0x9876fedcU,
	      "runtime query preserves envelope and caller-output canaries");
	Check(!open_calls && !seek_calls && !tell_calls && !read_calls &&
		      !close_calls && !allocation_calls && !free_calls && !live_allocation,
	      "runtime flag bypasses all firmware file and heap operations");
}

static void CheckRuntimeSuccess()
{
	ResetRuntime();
	for (unsigned query = 0; query < 2; ++query) {
		RuntimeOutputs outputs = RuntimeSentinels();
		Check(RuntimeQuery(&outputs) == BC_STS_SUCCESS &&
			      !std::memcmp(outputs.values, runtime_versions, sizeof(runtime_versions)),
		      "public runtime query publishes all three exact response values");
		CheckRuntimeOwnership(query + 1, query + 1, query + 1, outputs);
	}
	for (unsigned kind = 0; kind < 2; ++kind) {
		ResetRuntime(UINT32_MAX);
		std::fill(runtime_versions, runtime_versions + 3, kind ? UINT32_MAX : 0U);
		RuntimeOutputs outputs = RuntimeSentinels();
		Check(RuntimeQuery(&outputs) == BC_STS_SUCCESS &&
			      !std::memcmp(outputs.values, runtime_versions, sizeof(runtime_versions)),
		      "runtime marshalling preserves raw zero and full-width values without file parsing");
		CheckRuntimeOwnership(1, 1, 1, outputs);
	}
}

static void CheckRuntimeFailures()
{
	for (int status = BC_STS_ERROR; status <= BC_STS_PWR_MGMT; ++status) {
		if (status == BC_STS_SUCCESS)
			continue;
		ResetRuntime();
		runtime_driver_status = static_cast<BC_STATUS>(status);
		RuntimeOutputs outputs = RuntimeSentinels();
		const RuntimeOutputs before = outputs;
		Check(RuntimeQuery(&outputs) == runtime_driver_status &&
			      !std::memcmp(&outputs, &before, sizeof(outputs)),
		      "every non-success driver status preserves all caller outputs despite response data");
		CheckRuntimeOwnership(1, 1, 1, outputs);
	}
	for (unsigned bit = 0; bit < 32; ++bit) {
		ResetRuntime();
		runtime_response_status = uint32_t(1) << bit;
		RuntimeOutputs outputs = RuntimeSentinels();
		const RuntimeOutputs before = outputs;
		Check(RuntimeQuery(&outputs) == BC_STS_FW_CMD_ERR &&
			      !std::memcmp(&outputs, &before, sizeof(outputs)),
		      "each nonzero firmware status bit rejects poisoned version values without publication");
		CheckRuntimeOwnership(1, 1, 1, outputs);
	}
	ResetRuntime();
	runtime_allocation_fails = true;
	RuntimeOutputs outputs = RuntimeSentinels();
	const RuntimeOutputs before = outputs;
	Check(RuntimeQuery(&outputs) == BC_STS_INSUFF_RES &&
		      !std::memcmp(&outputs, &before, sizeof(outputs)),
	      "empty ioctl pool rejects before sequence, driver command or output publication");
	CheckRuntimeOwnership(1, 0, 0, outputs);
}

static void CheckRuntimeArguments()
{
	for (unsigned missing = 1; missing < 8; ++missing) {
		ResetRuntime();
		RuntimeOutputs outputs = RuntimeSentinels();
		const RuntimeOutputs before = outputs;
		Check(RuntimeQuery(&context, missing & 1 ? nullptr : &outputs.values[0],
				   missing & 2 ? nullptr : &outputs.values[1],
				   missing & 4 ? nullptr : &outputs.values[2]) == BC_STS_INV_ARG &&
			      !std::memcmp(&outputs, &before, sizeof(outputs)),
		      "every null runtime-output combination rejects without changing other outputs");
		CheckRuntimeOwnership(0, 0, 0, outputs);
	}
	for (unsigned invalid = 0; invalid < 3; ++invalid) {
		ResetRuntime();
		DTS_LIB_CONTEXT foreign = {};
		foreign.Sig = LIB_CTX_SIG;
		if (invalid == 2)
			context.Sig = 0;
		HANDLE handle = invalid == 0 ? nullptr : invalid == 1 ? &foreign : &context;
		RuntimeOutputs outputs = RuntimeSentinels();
		const RuntimeOutputs before = outputs;
		Check(RuntimeQuery(handle, &outputs.values[0], &outputs.values[1],
				   &outputs.values[2]) == BC_STS_INV_ARG &&
			      !std::memcmp(&outputs, &before, sizeof(outputs)),
		      "null, foreign and invalid-signature runtime handles reject before driver effects");
		CheckRuntimeOwnership(0, 0, 0, outputs);
	}
}

int main()
{
	CheckInvalidArguments();
	CheckEarlyMarkerAndPublicOutputs();
	CheckKnownLayoutsAndLimit();
	CheckFinalMarkerBoundary();
	CheckInvalidFiles();
	CheckMalformedMetadata();
	CheckFailures();
	CleanupLeak();
	CheckRuntimeSuccess();
	CheckRuntimeFailures();
	CheckRuntimeArguments();
	std::printf("Library firmware version: %u checks, %u failures\n",
		    checks, failures);
	return failures ? 1 : 0;
}
