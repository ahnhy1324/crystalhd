/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The real CLI is linked with mocked calls; no device can be opened. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "bc_dts_types.h"
struct _BC_DTS_PROC_OUT;
#include "bc_dts_defs.h"
#include "crystalhd_fw_if.h"
#include "crystalhd_fw_research.h"

int crystalhd_probe_main(int argc, char **argv);
static unsigned checks, opens, stats, infos, runs, closes;
static int open_error, stat_error, info_error, run_error, close_error;
static bool character, output_error, flush_error;
static struct crystalhd_fw_research_info metadata;
static struct crystalhd_fw_research_request submitted;
static char output[32768], errors[8192];
static unsigned mutation;
static unsigned fault_at, fault_kind;
static unsigned response_pattern;
static const uint32_t commands[] = {eCMD_C011_INIT, eCMD_C011_GET_VERSION,
    eCMD_C011_DEC_CHAN_OPEN, eCMD_C011_DEC_CHAN_STATUS, eCMD_C011_DEC_CHAN_CLOSE};
#define CHECK(value) do { checks++; if (!(value)) { \
    fprintf(stderr, "CLI line %u: %s\n%s", __LINE__, #value, errors); abort(); } } while (0)

static void append(FILE *stream, const char *format, va_list args)
{
    char *buffer = stream == stdout ? output : errors;
    size_t capacity = stream == stdout ? sizeof(output) : sizeof(errors);
    size_t length = strlen(buffer);
    int count;
    CHECK(stream == stdout || stream == stderr);
    if (stream == stdout) CHECK(closes == opens || open_error);
    count = vsnprintf(buffer + length, capacity - length, format, args);
    CHECK(count >= 0 && (size_t)count < capacity - length);
}
int probe_printf(const char *format, ...)
{ va_list args; va_start(args, format); append(stdout, format, args); va_end(args); return 1; }
int probe_fputs(const char *text, FILE *stream)
{
    char *buffer = stream == stdout ? output : errors;
    size_t capacity = stream == stdout ? sizeof(output) : sizeof(errors);
    CHECK(stream == stdout || stream == stderr);
    if (stream == stdout) CHECK(closes == opens || open_error);
    CHECK(strlen(buffer) + strlen(text) < capacity);
    strcat(buffer, text); return 1;
}
int probe_putchar(int value)
{ char text[] = {(char)value, 0}; probe_fputs(text, stdout); return value; }
int probe_putc(int value, FILE *stream)
{ char text[] = {(char)value, 0}; probe_fputs(text, stream); return value; }
int probe_ferror(FILE *stream)
{ CHECK(stream == stdout); return output_error; }
int probe_fflush(FILE *stream)
{ CHECK(stream == stdout && (closes == opens || open_error)); return flush_error ? EOF : 0; }
void probe_perror(const char *text)
{ probe_fputs(text, stderr); probe_fputs(": mock error\n", stderr); }
int probe_open(const char *path, int flags, ...)
{
    CHECK(!strcmp(path, "/dev/crystalhd-fw-research"));
    CHECK(flags == (O_RDWR | O_CLOEXEC | O_NOFOLLOW) && !opens++);
    if (open_error) { errno = open_error; return -1; }
    return 41;
}
int probe_fstat(int fd, struct stat *statbuf)
{
    CHECK(fd == 41 && opens == 1 && !stats++ && !infos && !runs);
    if (stat_error) { errno = stat_error; return -1; }
    memset(statbuf, 0, sizeof(*statbuf));
    statbuf->st_mode = (character ? S_IFCHR : S_IFREG) | 0600;
    return 0;
}
int probe_close(int fd)
{
    CHECK(fd == 41 && opens == 1 && !closes++ && !output[0]);
    if (close_error) { errno = close_error; return -1; }
    return 0;
}
static void digest(uint8_t bytes[32])
{
    const char *hex = CRYSTALHD_FW_RESEARCH_FIRMWARE_SHA256;
    unsigned i, value;
    for (i = 0; i < 32; i++) {
        CHECK(sscanf(hex + 2 * i, "%2x", &value) == 1);
        bytes[i] = (uint8_t)value;
    }
}
static void result_reply(struct crystalhd_fw_research_result *result, unsigned index, uint32_t status)
{
    struct crystalhd_fw_research_reply *reply = &result->replies[index];
    memset(reply, 0, sizeof(*reply));
    reply->command = submitted.selector >= 3 && index == 3 ? eCMD_C011_DEC_CHAN_CLOSE : commands[index];
    reply->sequence = index + 1;
    reply->transport_status = status;
    if (status == BC_STS_SUCCESS || status == BC_STS_FW_CMD_ERR) {
        reply->raw_response_valid = reply->header_matches = 1;
        reply->response[0] = reply->command; reply->response[1] = reply->sequence;
        reply->response[2] = status == BC_STS_FW_CMD_ERR ? 0x1234 : 0;
        if (reply->command == eCMD_C011_GET_VERSION) {
            CHECK(response_pattern <= 2);
            reply->response[3] = response_pattern == 1 ? 0 :
                response_pattern == 2 ? UINT32_MAX : 0x013600;
            reply->response[4] = response_pattern == 1 ? 0 :
                response_pattern == 2 ? 0x80000000 : 0x12345678;
            reply->response[5] = response_pattern == 1 ? 0 :
                response_pattern == 2 ? 0xdeadbeef : 0x00070015;
        }
        if (reply->command == eCMD_C011_DEC_CHAN_STATUS) {
            unsigned word;
            /* Even nonzero ACK payload words must not be called status. */
            for (word = 3; word <= 9; word++) reply->response[word] = UINT32_MAX - word;
        }
        reply->response[63] = UINT32_MAX;
    }
}
static void make_result(struct crystalhd_fw_research_result *result)
{
    unsigned i;
    memset(result, 0, sizeof(*result)); result->request = submitted;
    result->generation = metadata.generation;
    result->download_attempted = result->cleanup_attempted = 1;
    result->firmware_hash_valid = 1;
    result->command_count = submitted.selector == CRYSTALHD_FW_RESEARCH_VERSION_ONLY ? 2 :
        submitted.selector == CRYSTALHD_FW_RESEARCH_H264_CONTROL ? 5 : 4;
    digest(result->firmware_sha256);
    for (i = 0; i < result->command_count; i++) result_reply(result, i, BC_STS_SUCCESS);
    switch (mutation) {
    case 0: break;
    case 1: result->request.version++; break;
    case 2: result->generation++; break;
    case 3: result->status = 1; break;
    case 4: result->status = -4096; break;
    case 5: result->firmware_hash_valid = 2; break;
    case 6: result->download_attempted = 2; break;
    case 7: result->cleanup_attempted = 2; break;
    case 8: result->retained = 2; break;
    case 9: result->command_count++; break;
    case 10: result->download_status = 27; break;
    case 11: result->cleanup_status = 27; break;
    case 12: result->firmware_sha256[0] ^= 1; break;
    case 13: result->download_attempted = 0; break;
    case 14: result->replies[0].command++; break;
    case 15: result->replies[0].sequence++; break;
    case 16: result->replies[0].transport_status = 27; break;
    case 17: result->replies[0].raw_response_valid = 0; break;
    case 18: result->replies[0].header_matches = 2; break;
    case 19: result->replies[0].response[0]++; break;
    case 20: result->replies[0].transport_status = BC_STS_FW_CMD_ERR; break;
    case 21: result->cleanup_attempted = 0; break;
    case 22: result->download_status = BC_STS_IO_ERROR; break;
    case 23: result->cleanup_status = BC_STS_IO_ERROR; break;
    case 24: result->retained = 1; break;
    case 25: result->command_count = 1; break;
    case 26: result->request.reserved[3] = 1; break;
    case 27: result->request.flags = 1; break;
    case 28: result->request.size--; break;
    case 29: result->request.selector = 0; break;
    case 30: /* Real failed transport: bytes must remain invalid/zero. */
        result->status = -ETIMEDOUT; result->command_count = 1;
        result_reply(result, 0, BC_STS_TIMEOUT); break;
    case 31: /* Real completed reply that firmware rejected. */
        result->status = -EIO; result->command_count = 1;
        result_reply(result, 0, BC_STS_FW_CMD_ERR); break;
    case 32: /* Real completed but mismatched command echo. */
        result->status = -EPROTO; result->command_count = 1;
        result->replies[0].response[0]++; result->replies[0].header_matches = 0; break;
    case 33: /* Acquisition failure before download, no claimed success. */
        result->status = -EBUSY; result->command_count = 0;
        result->download_attempted = result->cleanup_attempted = 0;
        break;
    case 34: result->status = -EIO; result->retained = 1; result->cleanup_status = BC_STS_IO_ERROR; break;
    case 35: result->status = -EIO; result->retained = 1; result->cleanup_attempted = 0; break;
    case 36: result->status = -EPROTO; result->command_count = 3; result->replies[2].response[3] = 7; break;
    case 37: result->status = -ETIMEDOUT; result->command_count = 1; result_reply(result, 0, BC_STS_TIMEOUT); result->replies[0].response[63] = 1; break;
    case 38: result->status = -ETIMEDOUT; result->command_count = 1; result_reply(result, 0, BC_STS_TIMEOUT); result->replies[0].header_matches = 1; break;
    case 39: result->replies[0].response[1]++; break;
    case 40: result->replies[0].raw_response_valid = 2; break;
    case 41: /* No completed hash, e.g. request/provider/descriptor failure. */
        result->status = -ENOENT; result->command_count = 0;
        result->download_attempted = result->cleanup_attempted = result->firmware_hash_valid = 0;
        memset(result->firmware_sha256, 0, 32); break;
    case 42: /* An invalid hash flag cannot bless partial provider bytes. */
        result->status = -EIO; result->command_count = 0;
        result->download_attempted = result->cleanup_attempted = result->firmware_hash_valid = 0;
        break;
    case 43: /* Completed but rejected digest remains valid evidence. */
        result->status = -EKEYREJECTED; result->command_count = 0;
        result->download_attempted = result->cleanup_attempted = 0;
        result->firmware_sha256[0] ^= 1; break;
    case 44: result->firmware_hash_valid = 0; break;
    case 45: result->replies[2].response[3] = 7; break;
    case 46: result->status = -EPROTO; result->replies[2].response[3] = 7; break;
    case 47: result->replies[0].response[2] = 1; break;
    case 48: result->command_count = 5; result_reply(result, 4, BC_STS_SUCCESS); break;
    case 49: result->replies[3].command = eCMD_C011_DEC_CHAN_STATUS;
        result->replies[3].response[0] = eCMD_C011_DEC_CHAN_STATUS; break;
    case 50: result->replies[3].sequence = result->replies[3].response[1] = 5; break;
    case 51: result->status = -EIO; result->retained = 1; break;
    case 52: result->status = -EIO; result->cleanup_status = BC_STS_IO_ERROR; break;
    case 53: result->replies[2].response[2] = UINT32_MAX;
        result->replies[2].response[3] = UINT32_MAX; break;
    case 54: result->replies[1].response[2] = 1; break;
    case 55: result->replies[3].response[2] = 1; break;
    case 56: result->replies[1].raw_response_valid = 0; break;
    case 57: result->replies[1].header_matches = 0; break;
    case 58: result->replies[3].header_matches = 0; break;
    default: CHECK(false);
    }
    if (fault_at) {
        struct crystalhd_fw_research_reply *reply;
        unsigned index = fault_at - 1;
        CHECK(fault_at <= result->command_count && fault_kind >= 1 && fault_kind <= 7);
        result->command_count = fault_at;
        result->status = fault_kind == 2 ? -ETIMEDOUT :
            fault_kind >= 4 && fault_kind <= 6 ? -EPROTO : -EIO;
        result_reply(result, index, fault_kind == 1 ? BC_STS_IO_ERROR :
            fault_kind == 2 ? BC_STS_TIMEOUT :
            fault_kind == 3 || fault_kind == 7 ? BC_STS_FW_CMD_ERR : BC_STS_SUCCESS);
        reply = &result->replies[index];
        if (fault_kind == 3) {
            reply->response[2] = UINT32_MAX;
            if (index == 2) reply->response[3] = UINT32_MAX;
        }
        if (fault_kind == 4) { reply->response[0]++; reply->header_matches = 0; }
        if (fault_kind == 5) { reply->response[1]++; reply->header_matches = 0; }
        if (fault_kind == 6) { CHECK(index == 2); reply->response[3] = 7; }
        if (fault_kind == 7) reply->response[2] = 0;
        memset(result->replies + fault_at, 0,
               (CRYSTALHD_FW_RESEARCH_MAX_COMMANDS - fault_at) * sizeof(result->replies[0]));
    }
}
int probe_ioctl(int fd, unsigned long command, ...)
{
    void *argument; va_list args;
    CHECK(fd == 41 && stats == 1 && !closes);
    va_start(args, command); argument = va_arg(args, void *); va_end(args);
    if (command == CRYSTALHD_FW_RESEARCH_GET_INFO) {
        CHECK(!infos++ && !runs);
        if (info_error) { errno = info_error; return -1; }
        memcpy(argument, &metadata, sizeof(metadata)); return 0;
    }
    CHECK(command == CRYSTALHD_FW_RESEARCH_RUN && infos == 1 && !runs++);
    submitted = ((struct crystalhd_fw_research_result *)argument)->request;
    CHECK(submitted.version == 1 && submitted.size == sizeof(struct crystalhd_fw_research_result));
    CHECK(submitted.selector >= 1 && submitted.selector <= 5);
    CHECK(!submitted.flags && !submitted.reserved[0] && !submitted.reserved[1] && !submitted.reserved[2] && !submitted.reserved[3]);
    if (run_error) { errno = run_error; return -1; }
    make_result(argument); return 0;
}
/* glibc fortified headers can retain an asm syscall alias despite macro
 * substitution. These linker wraps are a second no-device safety boundary. */
int __wrap_open(const char *path, int flags, ...)
{ return probe_open(path, flags); }
int __wrap_open64(const char *path, int flags, ...)
{ return probe_open(path, flags); }
int __wrap___open_2(const char *path, int flags)
{ return probe_open(path, flags); }
int __wrap___open64_2(const char *path, int flags)
{ return probe_open(path, flags); }
int __wrap_fstat(int fd, struct stat *statbuf)
{ return probe_fstat(fd, statbuf); }
int __wrap_fstat64(int fd, struct stat64 *statbuf)
{ (void)fd; (void)statbuf; CHECK(false); return -1; }
int __wrap___fxstat(int version, int fd, struct stat *statbuf)
{ (void)version; return probe_fstat(fd, statbuf); }
int __wrap___fxstat64(int version, int fd, struct stat64 *statbuf)
{ (void)version; (void)fd; (void)statbuf; CHECK(false); return -1; }
int __wrap_close(int fd)
{ return probe_close(fd); }
int __wrap_ioctl(int fd, unsigned long command, ...)
{
    void *argument; va_list args;
    va_start(args, command); argument = va_arg(args, void *); va_end(args);
    return probe_ioctl(fd, command, argument);
}

static void reset(void)
{
    opens = stats = infos = runs = closes = 0;
    open_error = stat_error = info_error = run_error = close_error = 0;
    character = true; output_error = flush_error = false;
    mutation = fault_at = fault_kind = response_pattern = 0;
    output[0] = errors[0] = 0;
    memset(&metadata, 0, sizeof(metadata));
    metadata.version = 1; metadata.size = sizeof(metadata); metadata.generation = 42;
    metadata.selector_mask = CRYSTALHD_FW_RESEARCH_SELECTOR_MASK; digest(metadata.firmware_sha256);
}
static int invoke(char **arguments)
{
    int argc = 0, rc;
    while (arguments[argc]) argc++;
    rc = crystalhd_probe_main(argc, arguments);
    CHECK(opens <= 1 && infos <= 1 && runs <= 1 && closes == opens - (open_error && opens));
    return rc;
}
static char *info_args[] = {"probe", "--info", NULL};
static char *version_args[] = {"probe", "--version", "--acknowledge-card-reset", "--expected-generation", "42", NULL};
static char *h264_args[] = {"probe", "--h264-control", "--acknowledge-card-reset", "--expected-generation", "42", NULL};
static char *h261_args[] = {"probe", "--h261-control", "--acknowledge-card-reset", "--expected-generation", "42", NULL};
static char *h263_args[] = {"probe", "--h263-control", "--acknowledge-card-reset", "--expected-generation", "42", NULL};
static char *mpeg1_args[] = {"probe", "--mpeg1-control", "--acknowledge-card-reset", "--expected-generation", "42", NULL};
static char **named_args[] = {h261_args, h263_args, mpeg1_args};
static char **all_live_args[] = {version_args, h264_args, h261_args, h263_args, mpeg1_args};

static unsigned output_count(const char *needle)
{
    const char *cursor = output;
    unsigned count = 0;
    while ((cursor = strstr(cursor, needle))) { count++; cursor += strlen(needle); }
    return count;
}

static void check_decoding(unsigned reply_count, bool version_valid)
{
    static const char *decoded[] = {
        "\"decoded_response\":{\"stream_firmware_version\":79360,\"decoder_firmware_version\":305419896,\"firmware_reported_chip_hw_version\":458773}",
        "\"decoded_response\":{\"stream_firmware_version\":0,\"decoder_firmware_version\":0,\"firmware_reported_chip_hw_version\":0}",
        "\"decoded_response\":{\"stream_firmware_version\":4294967295,\"decoder_firmware_version\":2147483648,\"firmware_reported_chip_hw_version\":3735928559}",
    };
    CHECK(response_pattern <= 2);
    CHECK(output_count("\"decoded_response\":") == reply_count);
    CHECK(output_count("\"decoded_response\":null") == reply_count - (version_valid ? 1 : 0));
    CHECK(output_count("\"decoded_response\":{") == (version_valid ? 1 : 0));
    if (version_valid) CHECK(strstr(output, decoded[response_pattern]));
    CHECK(!strstr(output, "\"channel_status\"") && !strstr(output, "\"cpb_size\"") &&
          !strstr(output, "\"chip_hw_version\"") && !strstr(output, "\"block_size_pib\""));
}

static void test_arguments(void)
{
    static const char *invalid_decimal[] = {"", "0", "-1", "+42", " 42", "42 ", "42x", "0x2a", "18446744073709551616", "1e2", "٤٢"};
    char *invalid[][10] = {
        {"probe", NULL}, {"probe", "--bogus", NULL}, {"probe", "--help", "--info", NULL},
        {"probe", "--info", "--info", NULL}, {"probe", "--info", "--version", NULL},
        {"probe", "--info", "--acknowledge-card-reset", NULL},
        {"probe", "--info", "--expected-generation", "42", NULL},
        {"probe", "--version", NULL}, {"probe", "--h264-control", "--acknowledge-card-reset", NULL},
        {"probe", "--version", "--expected-generation", "42", NULL},
        {"probe", "--acknowledge-card-reset", "--expected-generation", "42", NULL},
        {"probe", "--version", "--acknowledge-card-reset", "--expected-generation", NULL},
        {"probe", "--version", "--acknowledge-card-reset", "--acknowledge-card-reset", "--expected-generation", "42", NULL},
        {"probe", "--version", "--acknowledge-card-reset", "--expected-generation", "42", "--expected-generation", "42", NULL},
        {"probe", "--version", "--h264-control", "--acknowledge-card-reset", "--expected-generation", "42", NULL},
        {"probe", "--version", "--acknowledge-card-reset", "--expected-generation=42", NULL},
    };
    char *help[] = {"probe", "--help", NULL};
    char *decimal[] = {"probe", "--version", "--acknowledge-card-reset", "--expected-generation", NULL, NULL};
    unsigned i;
    reset(); CHECK(!invoke(help) && !opens && strstr(output, "Usage:"));
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        reset(); CHECK(invoke(invalid[i]) == 1 && !opens && !output[0] && strstr(errors, "Usage:"));
    }
    for (i = 0; i < sizeof(invalid_decimal) / sizeof(invalid_decimal[0]); i++) {
        reset(); decimal[4] = (char *)invalid_decimal[i];
        CHECK(invoke(decimal) == 1 && !opens && !output[0]);
    }
    reset(); decimal[4] = "00042"; CHECK(!invoke(decimal) && runs == 1);
    reset(); decimal[4] = "18446744073709551615"; metadata.generation = UINT64_MAX;
    CHECK(!invoke(decimal) && runs == 1 && strstr(output, "\"18446744073709551615\""));
}
static void test_metadata(void)
{
    unsigned i;
    reset(); CHECK(!invoke(info_args) && opens == 1 && infos == 1 && !runs && closes == 1);
    CHECK(strstr(output, "\"generation\":\"42\"") && strstr(output, "\"research_selector_mask\":31"));
    CHECK(strstr(output, CRYSTALHD_FW_RESEARCH_FIRMWARE_SHA256));
    CHECK(!strstr(output, "decoded_response"));
    for (i = 0; i < 8; i++) {
        reset();
        if (i == 0) metadata.version++;
        if (i == 1) metadata.size--;
        if (i == 2) metadata.generation = 0;
        if (i == 3) metadata.selector_mask |= 32;
        if (i == 4) metadata.firmware_sha256[0] ^= 1;
        if (i >= 5) metadata.reserved[i - 5] = 1;
        CHECK(invoke(version_args) == 1 && infos == 1 && !runs && closes == 1 && !output[0]);
        CHECK(strstr(errors, "Invalid research metadata"));
    }
    reset(); metadata.generation++;
    CHECK(invoke(version_args) == 1 && !runs && strstr(errors, "generation changed"));
    reset(); metadata.selector_mask = 0;
    CHECK(!invoke(info_args) && !runs && strstr(output, "\"research_selector_mask\":0"));
    reset(); metadata.selector_mask = 0;
    CHECK(invoke(version_args) == 1 && !runs && strstr(errors, "unavailable"));
    reset(); metadata.selector_mask = 1;
    CHECK(invoke(h264_args) == 1 && !runs && strstr(errors, "unavailable"));
    reset(); metadata.selector_mask = 1; CHECK(!invoke(version_args) && runs == 1);
}
static void test_named_controls(void)
{
    unsigned action, phase, kind, malformed;
    for (action = 0; action < 3; action++) {
        char *missing_ack[] = {"probe", named_args[action][1], "--expected-generation", "42", NULL};
        char *missing_generation[] = {"probe", named_args[action][1], "--acknowledge-card-reset", NULL};
        char *duplicate[] = {"probe", named_args[action][1], named_args[action][1],
            "--acknowledge-card-reset", "--expected-generation", "42", NULL};
        char *mixed[] = {"probe", named_args[action][1], "--version",
            "--acknowledge-card-reset", "--expected-generation", "42", NULL};
        char *arbitrary[] = {"probe", named_args[action][1], "--acknowledge-card-reset",
            "--expected-generation", "42", "--algorithm", "8", NULL};
        char *algorithm_equal[] = {"probe", named_args[action][1], "--acknowledge-card-reset",
            "--expected-generation", "42", "--algorithm=2", NULL};
        char *invalid_selector[] = {"probe", named_args[action][1], "--acknowledge-card-reset",
            "--expected-generation", "42", "--selector", "3", NULL};
        char **invalid[] = {missing_ack, missing_generation, duplicate, mixed,
            arbitrary, algorithm_equal, invalid_selector};
        for (malformed = 0; malformed < sizeof(invalid) / sizeof(invalid[0]); malformed++) {
            reset(); CHECK(invoke(invalid[malformed]) == 1 && !opens && !output[0]);
        }
        reset(); CHECK(!invoke(named_args[action]) && submitted.selector == action + 3 && runs == 1 && closes == 1);
        CHECK(strstr(output, "\"command_count\":4") && strstr(output, "\"sequence\":4"));
        CHECK(submitted.flags == 0 && submitted.reserved[0] == 0 && submitted.reserved[1] == 0 &&
              submitted.reserved[2] == 0 && submitted.reserved[3] == 0);
        reset(); metadata.selector_mask = 3;
        CHECK(invoke(named_args[action]) == 1 && infos == 1 && !runs && closes == 1 && !output[0]);
        CHECK(strstr(errors, "unavailable"));
        reset(); metadata.selector_mask = 1U << (action + 2);
        CHECK(!invoke(named_args[action]) && runs == 1);
        reset(); metadata.generation++;
        CHECK(invoke(named_args[action]) == 1 && infos == 1 && !runs && !output[0]);
        for (phase = 1; phase <= 4; phase++) {
            for (kind = 1; kind <= 5; kind++) {
                reset(); fault_at = phase; fault_kind = kind;
                CHECK(invoke(named_args[action]) == 1 && runs == 1 && closes == 1 && output[0]);
                CHECK(!strstr(errors, "Invalid research result"));
                CHECK(submitted.selector == action + 3);
                if (kind <= 2) CHECK(strstr(output, "\"raw_response_words\":null"));
                if (kind == 3) CHECK(strstr(output, "4294967295") && strstr(output, "\"transport_status\":11"));
                if (kind >= 4) CHECK(strstr(output, "\"header_matches\":false"));
            }
        }
        reset(); fault_at = 3; fault_kind = 3;
        CHECK(invoke(named_args[action]) == 1 && strstr(output, "\"command_count\":3"));
        CHECK(strstr(output, "4294967295,4294967295") && strstr(output, "\"raw_response_valid\":true"));
        reset(); fault_at = 3; fault_kind = 6;
        CHECK(invoke(named_args[action]) == 1 && output[0] && strstr(output, "\"command_count\":3"));
        reset(); mutation = 24; /* Retention cannot turn completed CLOSE into success. */
        CHECK(invoke(named_args[action]) == 1 && !output[0] && strstr(errors, "Invalid research result"));
        for (malformed = 48; malformed <= 53; malformed++) {
            reset(); mutation = malformed;
            CHECK(invoke(named_args[action]) == 1 && runs == 1 && closes == 1);
            if (malformed == 51 || malformed == 52) {
                CHECK(output[0] && !strstr(errors, "Invalid research result"));
                CHECK(strstr(output, "\"command_count\":4"));
                if (malformed == 51) CHECK(strstr(errors, "remain retained"));
            } else CHECK(!output[0] && strstr(errors, "Invalid research result"));
        }
    }
    reset(); metadata.selector_mask = 3; CHECK(!invoke(version_args) && runs == 1 && submitted.selector == 1);
    reset(); metadata.selector_mask = 3; CHECK(!invoke(h264_args) && runs == 1 && submitted.selector == 2);
}
static void test_errors(void)
{
    unsigned i;
    for (i = 0; i < 6; i++) {
        reset();
        if (i == 0) open_error = EINTR;
        if (i == 1) stat_error = EIO;
        if (i == 2) character = false;
        if (i == 3) info_error = EAGAIN;
        if (i == 4) run_error = EINTR;
        if (i == 5) close_error = EINTR;
        CHECK(invoke(version_args) == 1 && opens == 1);
        CHECK(infos == (i >= 3) && runs == (i >= 4));
        if (i == 4) CHECK(strstr(errors, "no retry") && !output[0]);
        if (i == 5) CHECK(strstr(errors, "close research device") && strstr(output, "\"status\":0"));
    }
    reset(); output_error = true; CHECK(invoke(info_args) == 1 && closes == 1);
    reset(); flush_error = true; CHECK(invoke(h264_args) == 1 && closes == 1 && runs == 1);
}
static void test_results(void)
{
    unsigned i;
    reset(); CHECK(!invoke(version_args) && submitted.selector == 1 && runs == 1);
    CHECK(strstr(output, "\"command_count\":2") && strstr(output, "\"raw_response_valid\":true"));
    reset(); CHECK(!invoke(h264_args) && submitted.selector == 2 && runs == 1);
    CHECK(strstr(output, "\"command_count\":5"));
    for (i = 1; i <= 29; i++) {
        reset(); mutation = i;
        CHECK(invoke(version_args) == 1 && runs == 1 && closes == 1 && !output[0]);
        CHECK(strstr(errors, "Invalid research result"));
    }
    for (i = 30; i <= 36; i++) {
        reset(); mutation = i;
        CHECK(invoke(i == 36 ? h264_args : version_args) == 1 && runs == 1 && closes == 1);
        CHECK(output[0] && !strstr(errors, "Invalid research result"));
        if (i == 30) CHECK(strstr(output, "\"raw_response_words\":null"));
        if (i == 31) CHECK(strstr(output, "\"raw_response_valid\":true") && strstr(output, "\"transport_status\":11"));
        if (i == 32) CHECK(strstr(output, "\"header_matches\":false"));
        if (i == 33) CHECK(strstr(output, "\"download_attempted\":false") && strstr(output, "\"command_count\":0"));
        if (i == 34 || i == 35) CHECK(strstr(errors, "remain retained") && strstr(output, "\"retained\":true"));
    }
    for (i = 37; i <= 40; i++) {
        reset(); mutation = i;
        CHECK(invoke(version_args) == 1 && !output[0] && strstr(errors, "Invalid research result"));
    }
    reset(); mutation = 41; CHECK(invoke(version_args) == 1 && output[0]);
    CHECK(strstr(output, "\"firmware_hash_valid\":false,\"observed_firmware_sha256\":null"));
    reset(); mutation = 43; CHECK(invoke(version_args) == 1 && output[0]);
    CHECK(strstr(output, "\"firmware_hash_valid\":true") && !strstr(errors, "Invalid research result"));
    for (i = 42; i <= 47; i++) {
        if (i == 43) continue;
        reset(); mutation = i;
        CHECK(invoke(i == 45 || i == 46 ? h264_args : version_args) == 1);
        CHECK(!output[0] && strstr(errors, "Invalid research result"));
    }
}

static void test_decoding(void)
{
    unsigned action, pattern, phase, kind, malformed;
    for (action = 0; action < sizeof(all_live_args) / sizeof(all_live_args[0]); action++) {
        unsigned count = action == 0 ? 2 : action == 1 ? 5 : 4;
        for (pattern = 0; pattern <= 2; pattern++) {
            reset(); response_pattern = pattern;
            CHECK(!invoke(all_live_args[action]) && runs == 1 && closes == 1);
            check_decoding(count, true);
        }
        for (phase = 1; phase <= count; phase++) {
            for (kind = 1; kind <= 7; kind++) {
                if (kind == 6) continue;
                reset(); response_pattern = 2; fault_at = phase; fault_kind = kind;
                CHECK(invoke(all_live_args[action]) == 1 && runs == 1 && closes == 1 && output[0]);
                CHECK(!strstr(errors, "Invalid research result"));
                check_decoding(phase, phase > 2);
            }
        }
        for (malformed = 51; malformed <= 52; malformed++) {
            reset(); response_pattern = 2; mutation = malformed;
            CHECK(invoke(all_live_args[action]) == 1 && output[0]);
            check_decoding(count, true);
            if (malformed == 51) CHECK(strstr(errors, "remain retained"));
        }
    }
    for (malformed = 54; malformed <= 58; malformed++) {
        reset(); mutation = malformed;
        CHECK(invoke(h264_args) == 1 && !output[0] && strstr(errors, "Invalid research result"));
    }
    reset(); mutation = 35; response_pattern = 2;
    CHECK(invoke(version_args) == 1 && output[0] && strstr(errors, "remain retained"));
    check_decoding(2, true);
    reset(); close_error = EINTR; response_pattern = 2;
    CHECK(invoke(h264_args) == 1 && output[0] && strstr(errors, "close research device"));
    check_decoding(5, true);
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--json-examples")) {
        reset(); CHECK(!invoke(info_args)); fputs(output, stdout);
        reset(); CHECK(!invoke(version_args)); fputs(output, stdout);
        reset(); CHECK(!invoke(h264_args)); fputs(output, stdout);
        reset(); mutation = 30; CHECK(invoke(version_args) == 1); fputs(output, stdout);
        reset(); mutation = 41; CHECK(invoke(version_args) == 1); fputs(output, stdout);
        reset(); mutation = 43; CHECK(invoke(version_args) == 1); fputs(output, stdout);
        reset(); CHECK(!invoke(h261_args)); fputs(output, stdout);
        reset(); CHECK(!invoke(h263_args)); fputs(output, stdout);
        reset(); CHECK(!invoke(mpeg1_args)); fputs(output, stdout);
        reset(); fault_at = 3; fault_kind = 3; CHECK(invoke(h261_args) == 1); fputs(output, stdout);
        reset(); fault_at = 3; fault_kind = 1; CHECK(invoke(h263_args) == 1); fputs(output, stdout);
        unsigned action, pattern, phase, kind, failure;
        for (action = 0; action < sizeof(all_live_args) / sizeof(all_live_args[0]); action++) {
            unsigned count = action == 0 ? 2 : action == 1 ? 5 : 4;
            for (pattern = 0; pattern <= 2; pattern++) {
                reset(); response_pattern = pattern;
                CHECK(!invoke(all_live_args[action])); fputs(output, stdout);
            }
            for (phase = 1; phase <= count; phase++) {
                for (kind = 1; kind <= 7; kind++) {
                    if (kind == 6) continue;
                    reset(); response_pattern = 2; fault_at = phase; fault_kind = kind;
                    CHECK(invoke(all_live_args[action]) == 1); fputs(output, stdout);
                }
            }
            for (failure = 51; failure <= 52; failure++) {
                reset(); response_pattern = 2; mutation = failure;
                CHECK(invoke(all_live_args[action]) == 1); fputs(output, stdout);
            }
        }
        reset(); mutation = 35; response_pattern = 2;
        CHECK(invoke(version_args) == 1); fputs(output, stdout);
        reset(); close_error = EINTR; response_pattern = 2;
        CHECK(invoke(h264_args) == 1); fputs(output, stdout);
        return 0;
    }
    CHECK(argc == 1);
    test_arguments(); test_metadata(); test_errors(); test_results(); test_named_controls(); test_decoding();
    printf("Firmware probe CLI: %u checks passed\n", checks);
    return 0;
}
