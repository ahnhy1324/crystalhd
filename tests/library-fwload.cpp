// SPDX-License-Identifier: LGPL-2.1-or-later
/* Hardware-free firmware file validation and ownership checks against the
 * production loaders. All file and download operations are deterministic.
 */
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "7411d.h"
#include "crystalhd_ioctl_limits.h"
#include "libcrystalhd_fwload_if.h"
#include "libcrystalhd_int_if.h"

enum Loader {
	LINK_LOADER,
	FLEA_LOADER,
};

static unsigned checks;
static unsigned failures;
static unsigned open_calls;
static unsigned seek_calls;
static unsigned tell_calls;
static unsigned allocation_calls;
static unsigned read_calls;
static unsigned download_calls;
static unsigned free_calls;
static unsigned close_calls;
static unsigned perror_calls;
static size_t allocation_size;
static size_t read_request;
static void *live_allocation;
static bool track_call;
static bool open_succeeds;
static int end_seek_result;
static int rewind_result;
static long file_size;
static bool allocation_fails;
static size_t read_result;
static BC_STATUS download_status;
static Loader active_loader;
static int handle_token;
static char firmware_name[] = "firmware.bin";
static FILE *const file_token =
	reinterpret_cast<FILE *>(static_cast<uintptr_t>(1));

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

extern "C" FILE *__wrap_fopen(const char *path, const char *mode)
{
	Check(track_call, "file open occurs only inside a loader call");
	++open_calls;
	Check(path && std::strcmp(path, firmware_name) == 0,
	      "loader opens the requested firmware path");
	Check(mode && std::strcmp(mode, "rb") == 0,
	      "loader opens firmware in binary read mode");
	return open_succeeds ? file_token : nullptr;
}

extern "C" int __wrap_fseek(FILE *file, long offset, int origin)
{
	Check(track_call && file == file_token,
	      "seek uses the opened firmware stream");
	++seek_calls;
	Check(offset == 0, "firmware seeks use a zero offset");
	if (seek_calls == 1U) {
		Check(origin == SEEK_END, "first firmware seek finds the end");
		return end_seek_result;
	}
	Check(seek_calls == 2U && origin == SEEK_SET,
	      "second firmware seek rewinds to the start");
	return rewind_result;
}

extern "C" long __wrap_ftell(FILE *file)
{
	Check(track_call && file == file_token,
	      "size query uses the opened firmware stream");
	Check(seek_calls == 1U, "size query follows the end seek");
	++tell_calls;
	return file_size;
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
	      "a loader owns at most one firmware allocation");
	live_allocation = __real_malloc(size);
	return live_allocation;
}

static size_t MockRead(void *buffer, size_t size, size_t count, FILE *file)
{
	Check(track_call && file == file_token,
	      "read uses the opened firmware stream");
	Check(buffer == live_allocation,
	      "read targets the live firmware allocation");
	Check(size == 1U, "firmware read is byte-addressed");
	++read_calls;
	read_request = count;
	const size_t returned = read_result == static_cast<size_t>(-1) ?
		count : read_result;
	Check(returned <= count, "mocked read never exceeds its request");
	if (returned <= count)
		std::memset(buffer, 0x5a, returned);
	return returned;
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

extern "C" void __wrap_free(void *pointer)
{
	if (track_call && pointer) {
		Check(pointer == live_allocation,
		      "loader frees only its live firmware allocation");
		if (pointer == live_allocation) {
			++free_calls;
			live_allocation = nullptr;
		}
	}
	__real_free(pointer);
}

extern "C" int __wrap_fclose(FILE *file)
{
	Check(track_call && file == file_token,
	      "close uses the opened firmware stream");
	++close_calls;
	return 0;
}

extern "C" void __wrap_perror(const char *message)
{
	++perror_calls;
	const char *expected = active_loader == LINK_LOADER ? "LINK FW" : "FLEA FW";
	Check(message && std::strcmp(message, expected) == 0,
	      "open failure retains the chip-specific diagnostic");
}

extern "C" BC_STATUS DtsPushFwBinToLink(HANDLE handle, uint32_t *buffer,
					 uint32_t bytes)
{
	++download_calls;
	Check(handle == &handle_token, "download preserves the caller handle");
	Check(buffer == live_allocation,
	      "download uses the live firmware allocation");
	Check(file_size > 0 && bytes == static_cast<uint32_t>(file_size),
	      "download receives the complete validated file size");
	const uint8_t *payload = reinterpret_cast<const uint8_t *>(buffer);
	bool complete = true;
	for (uint32_t index = 0; index < bytes; ++index)
		complete = complete && payload[index] == 0x5a;
	Check(complete, "download receives every byte read from the file");
	return download_status;
}

static BC_STATUS Call(Loader loader, HANDLE handle, char *path,
			      uint32_t *bytes)
{
	if (loader == LINK_LOADER)
		return fwbinPushToLINK(handle, path, bytes);
	return fwbinPushToFLEA(handle, path, bytes);
}

static void Begin(Loader loader)
{
	Check(live_allocation == nullptr,
	      "the previous loader call left no allocation");
	open_calls = seek_calls = tell_calls = allocation_calls = 0;
	read_calls = download_calls = free_calls = close_calls = perror_calls = 0;
	allocation_size = read_request = 0;
	open_succeeds = true;
	end_seek_result = rewind_result = 0;
	file_size = 16;
	allocation_fails = false;
	read_result = static_cast<size_t>(-1);
	download_status = BC_STS_SUCCESS;
	active_loader = loader;
	track_call = true;
}

static void End()
{
	track_call = false;
	if (live_allocation) {
		/* Bound a broken cleanup regression so later cases still execute. */
		__real_free(live_allocation);
		live_allocation = nullptr;
	}
}

static void CheckNoWork(const char *message)
{
	Check(open_calls == 0 && seek_calls == 0 && tell_calls == 0 &&
	      allocation_calls == 0 && read_calls == 0 && download_calls == 0 &&
	      free_calls == 0 && close_calls == 0,
	      message);
}

static void CheckInvalidArguments(Loader loader)
{
	uint32_t bytes = 0xfeedfaceU;
	Begin(loader);
	Check(Call(loader, nullptr, firmware_name, &bytes) == BC_STS_INV_ARG,
	      "null device handle is rejected");
	Check(bytes == 0, "null-handle failure clears the byte count");
	CheckNoWork("null-handle failure performs no file or download work");
	End();

	bytes = 0xfeedfaceU;
	Begin(loader);
	Check(Call(loader, &handle_token, nullptr, &bytes) == BC_STS_INV_ARG,
	      "null firmware path is rejected");
	Check(bytes == 0, "null-path failure clears the byte count");
	CheckNoWork("null-path failure performs no file or download work");
	End();

	Begin(loader);
	Check(Call(loader, &handle_token, firmware_name, nullptr) == BC_STS_INV_ARG,
	      "null byte-count output is rejected");
	CheckNoWork("null-output failure performs no file or download work");
	End();
}

static void CheckOpenFailure(Loader loader)
{
	uint32_t bytes = 0xfeedfaceU;
	Begin(loader);
	open_succeeds = false;
	Check(Call(loader, &handle_token, firmware_name, &bytes) == BC_STS_ERROR,
	      "file-open failure preserves the legacy error status");
	Check(bytes == 0, "file-open failure clears the byte count");
	Check(open_calls == 1 && perror_calls == 1 && close_calls == 0,
	      "file-open failure reports once and owns no stream");
	Check(seek_calls == 0 && allocation_calls == 0 &&
	      read_calls == 0 && download_calls == 0,
	      "file-open failure performs no later work");
	End();
}

static void CheckSeekFailure(Loader loader, bool rewind)
{
	uint32_t bytes = 0xfeedfaceU;
	Begin(loader);
	if (rewind)
		rewind_result = -1;
	else
		end_seek_result = -1;
	Check(Call(loader, &handle_token, firmware_name, &bytes) == BC_STS_IO_ERROR,
	      "seek failure returns an I/O error");
	Check(bytes == 0, "seek failure clears the byte count");
	Check(open_calls == 1 && close_calls == 1,
	      "seek failure closes the opened stream");
	Check(seek_calls == (rewind ? 2U : 1U) &&
	      tell_calls == (rewind ? 1U : 0U),
	      "seek failure stops at the failing file operation");
	Check(allocation_calls == 0 && read_calls == 0 && download_calls == 0,
	      "seek failure occurs before allocation and download");
	End();
}

static void CheckTellFailure(Loader loader)
{
	uint32_t bytes = 0xfeedfaceU;
	Begin(loader);
	file_size = -1;
	allocation_fails = true;
	Check(Call(loader, &handle_token, firmware_name, &bytes) == BC_STS_IO_ERROR,
	      "negative file position returns an I/O error");
	Check(bytes == 0, "file-position failure clears the byte count");
	Check(open_calls == 1 && seek_calls == 1 && tell_calls == 1 &&
	      close_calls == 1, "file-position failure closes the stream");
	Check(allocation_calls == 0 && read_calls == 0 && download_calls == 0,
	      "negative file position is rejected before narrowing or allocation");
	End();
}

static void CheckInvalidSize(Loader loader, long size)
{
	uint32_t bytes = 0xfeedfaceU;
	Begin(loader);
	file_size = size;
	allocation_fails = true;
	Check(Call(loader, &handle_token, firmware_name, &bytes) == BC_STS_INV_ARG,
	      "invalid firmware size returns an argument error");
	Check(bytes == 0, "invalid firmware size clears the byte count");
	Check(open_calls == 1 && seek_calls == 1 && tell_calls == 1 &&
	      close_calls == 1, "invalid firmware size closes the stream");
	Check(allocation_calls == 0 && read_calls == 0 && download_calls == 0,
	      "invalid firmware size is rejected before allocation and download");
	End();
}

static void CheckAllocationFailure(Loader loader)
{
	uint32_t bytes = 0xfeedfaceU;
	Begin(loader);
	allocation_fails = true;
	Check(Call(loader, &handle_token, firmware_name, &bytes) == BC_STS_INSUFF_RES,
	      "allocation failure returns insufficient resources");
	Check(bytes == 0, "allocation failure clears the byte count");
	Check(open_calls == 1 && seek_calls == 2 && tell_calls == 1 &&
	      allocation_calls == 1 && allocation_size == 16U,
	      "allocation failure follows validated size and rewind");
	Check(free_calls == 0 && close_calls == 1,
	      "failed allocation still closes the stream");
	Check(read_calls == 0 && download_calls == 0,
	      "allocation failure performs no read or download");
	End();
}

static void CheckShortRead(Loader loader, size_t returned)
{
	uint32_t bytes = 0xfeedfaceU;
	Begin(loader);
	read_result = returned;
	Check(Call(loader, &handle_token, firmware_name, &bytes) == BC_STS_IO_ERROR,
	      "incomplete firmware read returns an I/O error");
	Check(bytes == 0, "incomplete firmware read clears the byte count");
	Check(allocation_calls == 1 && read_calls == 1 && read_request == 16U,
	      "incomplete read uses the validated allocation and size");
	Check(download_calls == 0,
	      "incomplete firmware is never submitted for download");
	Check(free_calls == 1 && close_calls == 1 && live_allocation == nullptr,
	      "incomplete read releases both allocation and stream");
	End();
}

static void CheckCompleteRead(Loader loader, long size, BC_STATUS downstream)
{
	uint32_t bytes = 0xfeedfaceU;
	Begin(loader);
	file_size = size;
	download_status = downstream;
	Check(Call(loader, &handle_token, firmware_name, &bytes) == downstream,
	      "complete firmware read preserves the downstream status");
	Check(bytes == static_cast<uint32_t>(size),
	      "complete firmware read reports the submitted byte count");
	Check(open_calls == 1 && seek_calls == 2 && tell_calls == 1,
	      "complete read validates and rewinds the stream");
	Check(allocation_calls == 1 && allocation_size == static_cast<size_t>(size) &&
	      read_calls == 1 && read_request == static_cast<size_t>(size),
	      "complete read allocates and requests the exact validated size");
	Check(download_calls == 1,
	      "complete read performs exactly one download");
	Check(free_calls == 1 && close_calls == 1 && live_allocation == nullptr,
	      "complete read releases both allocation and stream");
	End();
}

int main()
{
	const Loader loaders[] = { LINK_LOADER, FLEA_LOADER };
	for (Loader loader : loaders) {
		CheckInvalidArguments(loader);
		CheckOpenFailure(loader);
		CheckSeekFailure(loader, false);
		CheckTellFailure(loader);
		CheckInvalidSize(loader, 0);
		CheckInvalidSize(loader, 1);
		CheckInvalidSize(loader, 3);
		CheckInvalidSize(loader, 5);
		CheckInvalidSize(loader,
			static_cast<long>(CRYSTALHD_MAX_FIRMWARE_SIZE) + 4L);
		CheckInvalidSize(loader, LONG_MAX);
#if LONG_MAX > 0x7fffffffL
		CheckInvalidSize(loader, 0x100000004L);
#endif
		CheckSeekFailure(loader, true);
		CheckAllocationFailure(loader);
		CheckShortRead(loader, 0);
		CheckShortRead(loader, 12);
		CheckCompleteRead(loader, 4, BC_STS_SUCCESS);
		CheckCompleteRead(loader, 16, BC_STS_TIMEOUT);
		CheckCompleteRead(loader, CRYSTALHD_MAX_FIRMWARE_SIZE,
				  BC_STS_SUCCESS);
	}

	std::printf("Library firmware loader: %u checks, %u failures\n",
		    checks, failures);
	return failures ? 1 : 0;
}
