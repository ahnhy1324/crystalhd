// SPDX-License-Identifier: LGPL-2.1-or-later
/* Header-only DIL diagnostic routing. No SDK object, DSO or device is used. */
#include "7411d.h"
#include "libcrystalhd_priv.h"

#include <string>
#include <unistd.h>

static unsigned checks, failures;

static void Check(bool condition, const char *message)
{
	++checks;
	if (!condition) {
		fprintf(stderr, "FAIL: %s\n", message);
		++failures;
	}
}

static void Require(bool condition, const char *message)
{
	if (!condition) {
		perror(message);
		exit(2);
	}
}

static std::string ReadAll(FILE *stream)
{
	Require(fseek(stream, 0, SEEK_SET) == 0, "rewind capture");
	std::string result;
	char bytes[256];
	size_t size;
	while ((size = fread(bytes, 1, sizeof(bytes), stream)) != 0)
		result.append(bytes, size);
	Require(!ferror(stream), "read capture");
	return result;
}

int main()
{
	FILE *output = tmpfile();
	FILE *diagnostics = tmpfile();
	FILE *caller_stream = tmpfile();
	Require(output && diagnostics && caller_stream, "temporary stream");
	Require(fflush(stdout) == 0 && fflush(stderr) == 0, "flush before capture");
	const int saved_output = dup(STDOUT_FILENO);
	const int saved_diagnostics = dup(STDERR_FILENO);
	Require(saved_output >= 0 && saved_diagnostics >= 0, "save stdio descriptors");
	Require(dup2(fileno(output), STDOUT_FILENO) >= 0 &&
	        dup2(fileno(diagnostics), STDERR_FILENO) >= 0, "capture stdio");

	const unsigned char binary[] = {0x00, 0xff, 0x80, 0x01, '\n', 'D', 'I', 'L', 0x00};
	unsigned argument_evaluations = 0;
	unsigned level_evaluations = 0;
	Require(fwrite(binary, 1, 5, stdout) == 5, "write binary prefix");
	DebugLog_Trace(LDIL_ERR, "DtsDeviceOpen: Create File Failed\n");
	DebugLog_Trace(LDIL_INFO, "zero-argument info\n");
	Require(printf("application stdout %d\n", 7) > 0, "caller stdout");
	DebugLog_Trace(LDIL_DBG, "debug: mode=%08x count=%u\n", 0x1234U, ++argument_evaluations);
	Require(fwrite(binary + 5, 1, sizeof(binary) - 5, stdout) == sizeof(binary) - 5,
	        "write binary suffix");
	DebugLog_Trace(LDIL_ERR | LDIL_INFO | LDIL_DBG, "action: opening mode %x\n", 0x200U);
	DebugLog_Trace(0, "unclassified log\n");
	// The existing macro ignores its level argument; routing must not start
	// evaluating it or silently introduce a level filter.
	DebugLog_Trace(++level_evaluations, "level expression ignored\n");
	Require(fprintf(caller_stream, "caller FILE stream %u\n", 9U) > 0,
	        "caller-selected FILE stream");
	Require(fflush(stdout) == 0 && fflush(stderr) == 0 && fflush(caller_stream) == 0,
	        "flush captured output");
	Require(dup2(saved_output, STDOUT_FILENO) >= 0 &&
	        dup2(saved_diagnostics, STDERR_FILENO) >= 0, "restore stdio");
	Require(close(saved_output) == 0 && close(saved_diagnostics) == 0, "close saved descriptors");

	std::string expected_output(reinterpret_cast<const char *>(binary), 5);
	expected_output += "application stdout 7\n";
	expected_output.append(reinterpret_cast<const char *>(binary + 5), sizeof(binary) - 5);
	const std::string expected_diagnostics =
	    "DtsDeviceOpen: Create File Failed\n"
	    "zero-argument info\n"
	    "debug: mode=00001234 count=1\n"
	    "action: opening mode 200\n"
	    "unclassified log\n"
	    "level expression ignored\n";
	Check(ReadAll(output) == expected_output,
	      "binary stdout (including NUL/non-ASCII) and caller printf stay byte-exact");
	Check(ReadAll(diagnostics) == expected_diagnostics,
	      "all enabled DIL levels/zero-varargs/failure/action messages use exact stderr");
	Check(ReadAll(caller_stream) == "caller FILE stream 9\n",
	      "explicit caller FILE stream is not redirected");
	Check(argument_evaluations == 1, "format arguments are evaluated exactly once");
	Check(level_evaluations == 0, "ignored log level behavior is unchanged");
	Check(fclose(output) == 0 && fclose(diagnostics) == 0 && fclose(caller_stream) == 0,
	      "all capture streams close normally");
	printf("library logging: %u checks, %u failures\n", checks, failures);
	return failures ? 1 : 0;
}
