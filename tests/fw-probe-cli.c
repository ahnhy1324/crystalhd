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
static unsigned checks, opens, stats, infos, runs, state_runs, controller_runs, image_runs, packet_runs, heap_runs, clock_runs, uart_runs, closes;
static int open_error, stat_error, info_error, run_error, close_error;
static bool character, output_error, flush_error;
static struct crystalhd_fw_research_info metadata;
static struct crystalhd_fw_research_request submitted;
static struct crystalhd_fw_research_state_request state_submitted;
static char output[32768], errors[8192];
static unsigned mutation;
static unsigned fault_at, fault_kind;
static unsigned response_pattern;
static unsigned state_mutation, sample_fault_at, sample_fault_kind;
static bool state_opaque;
static unsigned controller_mutation, root_fault_at, root_fault_kind;
static uint32_t root_values[2];
static unsigned image_fault_at, image_fault_kind, image_mutation, image_mutation_stage, image_word;
static uint32_t image_roots[2], image_words[4], image_bad_root;
static unsigned packet_fault_at, packet_fault_kind, packet_mutation, packet_mutation_stage, packet_word;
static uint32_t packet_roots[2], packet_image_words[4], packet_words[3], packet_bad_root;
static unsigned heap_fault_at, heap_fault_kind, heap_mutation, heap_stage, heap_word;
static uint32_t heap_roots[2], heap_bases[2], heap_extent, heap_owned, heap_header[5], heap_slots[2];
static uint32_t heap_bad_root, heap_bad_value;
static unsigned clock_fault_at, clock_fault_kind, clock_mutation, clock_stage, clock_word;
static uint32_t clock_values[2][3];
static unsigned uart_fault_at, uart_fault_kind, uart_mutation, uart_stage, uart_word;
static uint32_t uart_values[2][3];
static const uint32_t commands[] = {eCMD_C011_INIT, eCMD_C011_GET_VERSION,
    eCMD_C011_DEC_CHAN_OPEN, eCMD_C011_DEC_CHAN_STATUS, eCMD_C011_DEC_CHAN_CLOSE};
static const uint32_t raw_commands[] = {eCMD_C011_DEC_CHAN_SCALING_FILTERS,
    eCMD_C011_DEC_CHAN_PIC_CAPTURE, eCMD_C011_DEC_CHAN_SET_CSC,
    eCMD_C011_DEC_CHAN_SET_FGT, eCMD_C011_DEC_CHAN_CUSTOM_VIDOUT,
    eCMD_C011_DEC_CHAN_FILL_PIC_BUF};
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
    reply->command = submitted.selector >= 6 && index == 2 ? raw_commands[submitted.selector - 6] :
        submitted.selector >= 3 && submitted.selector <= 5 && index == 3 ? eCMD_C011_DEC_CHAN_CLOSE : commands[index];
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
        submitted.selector == CRYSTALHD_FW_RESEARCH_H264_CONTROL ? 5 : submitted.selector <= 5 ? 4 : 3;
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
    case 59: case 60: case 61: /* Pinned UnknownCommand echoes only command. */
        CHECK(submitted.selector >= 6);
        result->status = mutation == 60 ? 0 : -EPROTO;
        memset(result->replies[2].response, 0, sizeof(result->replies[2].response));
        result->replies[2].response[0] = result->replies[2].command;
        result->replies[2].header_matches = 0;
        if (mutation == 61) { result->retained = 1; result->cleanup_status = BC_STS_IO_ERROR; }
        break;
    case 62: result->command_count = 4; result_reply(result, 3, BC_STS_SUCCESS); break;
    case 63: result->replies[2].command++; result->replies[2].response[0]++; break;
    case 64: result->replies[2].sequence = result->replies[2].response[1] = 4; break;
    case 65: result->status = -EPROTO; result->replies[2].header_matches = 0; break;
    case 66: result->replies[2].response[2] = 1; break;
    case 67: result->replies[2].response[3] = 7; break;
    case 68: result->replies[2].command = result->replies[2].response[0] = eCMD_C011_DEC_CHAN_OPEN; break;
    case 69: result->replies[1].command = result->replies[1].response[0] = eCMD_C011_INIT; break;
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

static bool successful_reply(const struct crystalhd_fw_research_reply *reply)
{
    return reply->transport_status == BC_STS_SUCCESS && reply->raw_response_valid &&
        reply->header_matches && !reply->response[2];
}

static void make_state_result(struct crystalhd_fw_research_state_result *result)
{
    struct crystalhd_fw_research_state_sample *samples[] = {
        &result->calibration, &result->after_init, &result->after_open,
    };
    unsigned i;
    memset(result, 0, sizeof(*result)); result->request = state_submitted;
    make_result(&result->control);
    if (result->control.command_count >= 2 && successful_reply(&result->control.replies[0]) &&
        successful_reply(&result->control.replies[1])) {
        for (i = 0; i < 2; i++) samples[i]->attempted = samples[i]->read_complete = 1;
        result->calibration.words[0] = 0xd3a00;
        result->after_init.words[0] = 1; result->after_init.words[1] = 0xd3a00;
        if (state_opaque) {
            result->after_init.words[2] = 0xffffff00;
            result->after_init.words[3] = 0xff000000;
        }
        if (result->control.command_count >= 3 && successful_reply(&result->control.replies[2]) &&
            !result->control.replies[2].response[3]) {
            result->after_open.attempted = result->after_open.read_complete = 1;
            result->after_open.words[0] = 1; result->after_open.words[1] = 0xd3a00;
            result->after_open.words[2] = state_opaque ? 0xffffff01 : 1;
            result->after_open.words[3] = state_opaque ? 0xff000200 : 0x200;
        }
    }
    if (sample_fault_at) {
        struct crystalhd_fw_research_state_sample *sample = samples[sample_fault_at - 1];
        unsigned count = sample_fault_at == 3 ? 3 : 2;
        CHECK(sample_fault_at <= 3 && sample_fault_kind >= 1 && sample_fault_kind <= 9);
        result->control.command_count = count;
        memset(result->control.replies + count, 0,
               (CRYSTALHD_FW_RESEARCH_MAX_COMMANDS - count) * sizeof(result->control.replies[0]));
        for (i = sample_fault_at; i < 3; i++) memset(samples[i], 0, sizeof(*samples[i]));
        sample->status = sample_fault_kind == 2 ? -ETIMEDOUT :
            sample_fault_kind >= 3 && sample_fault_kind <= 6 ? -EPROTO :
            sample_fault_kind == 7 || sample_fault_kind == 9 ? -ENODEV :
            sample_fault_kind == 8 ? -4095 : -EIO;
        result->control.status = sample->status;
        if (sample_fault_kind <= 2 || sample_fault_kind >= 7) {
            sample->read_complete = 0; memset(sample->words, 0, sizeof(sample->words));
            if (sample_fault_kind == 9) sample->attempted = 0;
        } else if (sample_fault_kind == 3) sample->words[0] ^= 1;
        else if (sample_fault_kind == 4) sample->words[1] ^= 1;
        else if (sample_fault_kind == 5) sample->words[2] ^= 1;
        else sample->words[3] ^= 1;
    }
    switch (state_mutation) {
    case 0: break;
    case 1: result->request.version++; break;
    case 2: result->request.size--; break;
    case 3: result->request.flags = 1; break;
    case 4: result->request.reserved = 1; break;
    case 5: result->calibration.attempted = 2; break;
    case 6: result->calibration.read_complete = 2; break;
    case 7: result->calibration.reserved = 1; break;
    case 8: result->after_init.reserved = 1; break;
    case 9: result->after_open.reserved = 1; break;
    case 10: result->calibration.status = 1; break;
    case 11: result->calibration.status = -4096; break;
    case 12: result->calibration.status = -EIO; break;
    case 13: result->calibration.read_complete = 0; break;
    case 14: result->calibration.attempted = 0; break;
    case 15: memset(&result->calibration, 0, sizeof(result->calibration)); break;
    case 16: memset(&result->after_init, 0, sizeof(result->after_init)); break;
    case 17: memset(&result->after_open, 0, sizeof(result->after_open)); break;
    case 18: result->calibration.words[0]++; break;
    case 19: result->calibration.words[1] = 1; break;
    case 20: result->calibration.words[2] = 1; break;
    case 21: result->calibration.words[3] = 1; break;
    case 22: result->after_init.words[0] = 0; break;
    case 23: result->after_init.words[1]++; break;
    case 24: result->after_init.words[2] = 1; break;
    case 25: result->after_init.words[3] = 1; break;
    case 26: result->after_open.words[0] = 0; break;
    case 27: result->after_open.words[1]++; break;
    case 28: result->after_open.words[2] = 0; break;
    case 29: result->after_open.words[3] = 0; break;
    case 30: result->control.status = 0; break;
    case 31: result->control.command_count++; result_reply(&result->control, result->control.command_count - 1, BC_STS_SUCCESS); break;
    case 32: result->calibration.status = -EPROTO; break;
    case 33: result->control.request.selector = CRYSTALHD_FW_RESEARCH_VERSION_ONLY; break;
    case 34: result->control.request.size = sizeof(*result); break;
    case 35: result->after_init.attempted = 2; break;
    case 36: result->after_open.read_complete = 2; break;
    case 37: result->control.status = -EIO; result->control.command_count = 2;
        result->after_init.status = -EIO; result->after_init.read_complete = 0;
        memset(result->after_init.words, 0, sizeof(result->after_init.words)); break;
    case 38: result->calibration.words[0] = 1; break;
    case 39: result->control.status = -EIO; break;
    case 40: result->control.status = -EPROTO; break;
    default: CHECK(false);
    }
}

static void make_controller_result(struct crystalhd_fw_research_controller_result *result)
{
    struct crystalhd_fw_research_controller_sample *roots[] = {&result->after_init, &result->after_open};
    memset(result, 0, sizeof(*result));
    make_state_result(&result->state);
    /* Real kernel storage is zeroed before command_count can stop early. */
    if (result->state.control.command_count <= CRYSTALHD_FW_RESEARCH_MAX_COMMANDS)
        memset(result->state.control.replies + result->state.control.command_count, 0,
               (CRYSTALHD_FW_RESEARCH_MAX_COMMANDS - result->state.control.command_count) *
               sizeof(result->state.control.replies[0]));
    if (mutation == 35) result->state.control.cleanup_status = BC_STS_CMD_CANCELLED;
    if (result->state.after_init.attempted && result->state.after_init.read_complete && !result->state.after_init.status) {
        roots[0]->attempted = roots[0]->read_complete = 1; roots[0]->root = root_values[0];
        if (result->state.after_open.attempted && result->state.after_open.read_complete && !result->state.after_open.status) {
            roots[1]->attempted = roots[1]->read_complete = 1; roots[1]->root = root_values[1];
        }
    }
    if (root_fault_at) {
        struct crystalhd_fw_research_controller_sample *sample = roots[root_fault_at - 1];
        unsigned count = root_fault_at == 1 ? 2 : 3;
        CHECK(root_fault_at <= 2 && root_fault_kind >= 1 && root_fault_kind <= 5);
        sample->status = root_fault_kind == 2 ? -ENODEV : root_fault_kind == 3 ? -ETIMEDOUT :
            root_fault_kind == 4 ? -4095 : root_fault_kind == 5 ? -EPROTO : -EIO;
        sample->attempted = root_fault_kind != 2;
        sample->read_complete = sample->root = 0;
        result->state.control.status = sample->status;
        result->state.control.command_count = count;
        memset(result->state.control.replies + count, 0,
               (CRYSTALHD_FW_RESEARCH_MAX_COMMANDS - count) * sizeof(result->state.control.replies[0]));
        if (root_fault_at == 1) {
            memset(&result->state.after_open, 0, sizeof(result->state.after_open));
            memset(roots[1], 0, sizeof(*roots[1]));
        }
    }
    switch (controller_mutation) {
    case 0: break;
    case 1: roots[0]->attempted = 2; break;
    case 2: roots[0]->read_complete = 2; break;
    case 3: roots[0]->status = 1; break;
    case 4: roots[0]->status = -4096; break;
    case 5: roots[0]->status = -EIO; break;
    case 6: roots[0]->attempted = 0; break;
    case 7: roots[0]->read_complete = roots[0]->root = 0; break;
    case 8: memset(roots[0], 0, sizeof(*roots[0])); break;
    case 9: roots[0]->root = 0xdeadbeef; break;
    case 10: roots[0]->read_complete = 1; break;
    case 11: result->state.control.status = -EIO; break;
    case 12: result->state.control.command_count++; result_reply(&result->state.control, 2, BC_STS_SUCCESS); break;
    case 13: roots[0]->status = 0; break;
    case 14: roots[1]->attempted = roots[1]->read_complete = 1; roots[1]->root = 0xd5384; break;
    case 15: roots[1]->attempted = 2; break;
    case 16: roots[1]->read_complete = 2; break;
    case 17: roots[1]->status = 1; break;
    case 18: roots[1]->status = -4096; break;
    case 19: roots[1]->status = -EIO; break;
    case 20: roots[1]->attempted = 0; break;
    case 21: roots[1]->read_complete = roots[1]->root = 0; break;
    case 22: memset(roots[1], 0, sizeof(*roots[1])); break;
    case 23: roots[1]->root = 0xdeadbeef; break;
    case 24: roots[1]->read_complete = 1; break;
    case 25: result->state.control.status = -EIO; break;
    case 26: result->state.control.command_count++; result_reply(&result->state.control, 3, BC_STS_SUCCESS); break;
    case 27: roots[1]->status = 0; break;
    case 28: roots[0]->root = 1; break;
    case 29: result->state.control.replies[4].response[63] = 1; break;
    case 30: result->state.request.size = sizeof(result->state); break;
    case 31: result->state.control.cleanup_status = BC_STS_IO_ERROR; break;
    case 32: result->state.control.download_status = BC_STS_IO_ERROR; break;
    case 33: result->state.control.cleanup_attempted = 0; result->state.control.cleanup_status = 0; break;
    case 34: result->state.control.cleanup_attempted = 1; break;
    case 35: roots[0]->attempted = roots[0]->read_complete = 1; roots[0]->root = 0xd5384; break;
    default: CHECK(false);
    }
}

static void make_image_result(struct crystalhd_fw_research_image_result *result)
{
    struct crystalhd_fw_research_image_sample *samples[] = {&result->after_init, &result->after_open};
    const struct crystalhd_fw_research_controller_sample *roots[] = {
        &result->controller.after_init, &result->controller.after_open,
    };
    struct crystalhd_fw_research_result *control = &result->controller.state.control;
    struct crystalhd_fw_research_image_sample *changed;
    unsigned i;
    memset(result, 0, sizeof(*result));
    make_controller_result(&result->controller);
    for (i = 0; i < 2; i++) {
        if (roots[i]->attempted && roots[i]->read_complete && !roots[i]->status) {
            samples[i]->attempted = samples[i]->read_complete = 1;
            samples[i]->root_before = samples[i]->root_after = image_roots[i];
            memcpy(samples[i]->words, image_words, sizeof(image_words));
        }
    }
    if (image_fault_at) {
        unsigned count = image_fault_at + 1;
        struct crystalhd_fw_research_image_sample *sample = samples[image_fault_at - 1];
        CHECK(image_fault_at <= 2 && image_fault_kind >= 1 && image_fault_kind <= 6);
        memset(sample, 0, sizeof(*sample));
        sample->attempted = image_fault_kind != 2;
        sample->status = image_fault_kind == 2 ? -ENODEV : image_fault_kind == 3 ? -ERANGE :
            image_fault_kind == 4 ? -ESTALE : image_fault_kind == 5 ? -ETIMEDOUT : image_fault_kind == 6 ? -4095 : -EIO;
        control->status = sample->status; control->command_count = count;
        memset(control->replies + count, 0,
               (CRYSTALHD_FW_RESEARCH_MAX_COMMANDS - count) * sizeof(control->replies[0]));
        if (image_fault_at == 1) {
            memset(&result->controller.state.after_open, 0, sizeof(result->controller.state.after_open));
            memset(&result->controller.after_open, 0, sizeof(result->controller.after_open));
            memset(samples[1], 0, sizeof(*samples[1]));
        }
    }
    CHECK(image_mutation_stage < 2);
    changed = samples[image_mutation_stage];
    switch (image_mutation) {
    case 0: break;
    case 1: changed->attempted = 2; break;
    case 2: changed->read_complete = 2; break;
    case 3: changed->reserved = 1; break;
    case 4: changed->status = 1; break;
    case 5: changed->status = -4096; break;
    case 6: changed->attempted = 0; break;
    case 7: changed->read_complete = 0; changed->root_before = changed->root_after = 0;
        memset(changed->words, 0, sizeof(changed->words)); break;
    case 8: changed->status = -EIO; break;
    case 9: changed->root_after ^= 4; break;
    case 10: changed->root_before = changed->root_after = 0; break;
    case 11: changed->root_before = changed->root_after = image_bad_root; break;
    case 12: memset(changed, 0, sizeof(*changed)); break;
    case 13: changed->root_before = 1; break;
    case 14: changed->root_after = 1; break;
    case 15: CHECK(image_word < 4); changed->words[image_word] = 1; break;
    case 16: changed->read_complete = 1; break;
    case 17: changed->attempted = 1; changed->status = 0; break;
    case 18: control->status = -EIO; break;
    case 19: control->command_count++; result_reply(control, control->command_count - 1, BC_STS_SUCCESS); break;
    case 20: changed->attempted = changed->read_complete = 1;
        changed->root_before = changed->root_after = image_roots[image_mutation_stage]; break;
    case 21: result->controller.state.request.size = sizeof(result->controller); break;
    case 22: control->replies[4].response[63] = 1; break;
    default: CHECK(false);
    }
}

static void make_packet_result(struct crystalhd_fw_research_packet_result *result)
{
    struct crystalhd_fw_research_packet_sample *samples[] = {&result->after_init, &result->after_open};
    const struct crystalhd_fw_research_image_sample *images[] = {&result->image.after_init, &result->image.after_open};
    struct crystalhd_fw_research_result *control = &result->image.controller.state.control;
    struct crystalhd_fw_research_packet_sample *changed;
    unsigned i;
    memset(result, 0, sizeof(*result));
    make_image_result(&result->image);
    for (i = 0; i < 2; i++) {
        if (images[i]->attempted && images[i]->read_complete && !images[i]->status) {
            samples[i]->attempted = samples[i]->read_complete = 1;
            samples[i]->root_before = samples[i]->root_after = packet_roots[i];
            memcpy(samples[i]->image_words, packet_image_words, sizeof(packet_image_words));
            memcpy(samples[i]->packet_words, packet_words, sizeof(packet_words));
        }
    }
    if (packet_fault_at) {
        unsigned count = packet_fault_at + 1;
        struct crystalhd_fw_research_packet_sample *sample = samples[packet_fault_at - 1];
        CHECK(packet_fault_at <= 2 && packet_fault_kind >= 1 && packet_fault_kind <= 6);
        memset(sample, 0, sizeof(*sample));
        sample->attempted = packet_fault_kind != 2;
        sample->status = packet_fault_kind == 2 ? -ENODEV : packet_fault_kind == 3 ? -ERANGE :
            packet_fault_kind == 4 ? -ESTALE : packet_fault_kind == 5 ? -ETIMEDOUT : packet_fault_kind == 6 ? -4095 : -EIO;
        control->status = sample->status; control->command_count = count;
        memset(control->replies + count, 0,
               (CRYSTALHD_FW_RESEARCH_MAX_COMMANDS - count) * sizeof(control->replies[0]));
        if (packet_fault_at == 1) {
            memset(&result->image.controller.state.after_open, 0, sizeof(result->image.controller.state.after_open));
            memset(&result->image.controller.after_open, 0, sizeof(result->image.controller.after_open));
            memset(&result->image.after_open, 0, sizeof(result->image.after_open));
            memset(samples[1], 0, sizeof(*samples[1]));
        }
    }
    CHECK(packet_mutation_stage < 2);
    changed = samples[packet_mutation_stage];
    switch (packet_mutation) {
    case 0: break;
    case 1: changed->attempted = 2; break;
    case 2: changed->read_complete = 2; break;
    case 3: changed->reserved = 1; break;
    case 4: changed->status = 1; break;
    case 5: changed->status = -4096; break;
    case 6: changed->attempted = 0; break;
    case 7: memset(changed, 0, sizeof(*changed)); break;
    case 8: changed->status = -EIO; break;
    case 9: changed->root_after ^= 4; break;
    case 10: changed->root_before = changed->root_after = packet_bad_root; break;
    case 11: changed->root_before = 1; break;
    case 12: changed->root_after = 1; break;
    case 13: CHECK(packet_word < 7);
        if (packet_word < 4) changed->image_words[packet_word] = 1;
        else changed->packet_words[packet_word - 4] = 1;
        break;
    case 14: changed->read_complete = 1; break;
    case 15: changed->attempted = 1; changed->status = 0; break;
    case 16: control->status = -EIO; break;
    case 17: control->command_count++; result_reply(control, control->command_count - 1, BC_STS_SUCCESS); break;
    case 18: changed->attempted = changed->read_complete = 1;
        changed->root_before = changed->root_after = packet_roots[packet_mutation_stage]; break;
    case 19: result->image.controller.state.request.size = sizeof(result->image); break;
    case 20: control->replies[4].response[63] = 1; break;
    default: CHECK(false);
    }
}

static void fill_heap_sample(struct crystalhd_fw_research_heap_packet_sample *sample, unsigned stage)
{
    uint32_t target = (uint32_t)((uint64_t)heap_bases[stage] + 0x70000U);
    unsigned i;
    memset(sample, 0, sizeof(*sample));
    sample->attempted = sample->read_complete = 1;
    sample->root_before = sample->root_after = heap_roots[stage];
    sample->image_before[0] = sample->image_before[1] = heap_bases[stage];
    sample->image_before[2] = heap_extent; sample->image_before[3] = heap_owned;
    memcpy(sample->image_after, sample->image_before, sizeof(sample->image_before));
    for (i = 0; i < 3; i++) sample->packet_before[i] = sample->packet_after[i] = target;
    sample->packet_address = target;
    memcpy(sample->header_words, heap_header, sizeof(heap_header));
    memcpy(sample->slots_before, heap_slots, sizeof(heap_slots));
    memcpy(sample->slots_after, heap_slots, sizeof(heap_slots));
}

static void make_heap_result(struct crystalhd_fw_research_heap_packet_result *result)
{
    struct crystalhd_fw_research_heap_packet_sample *samples[] = {&result->after_init, &result->after_open};
    const struct crystalhd_fw_research_image_sample *images[] = {&result->image.after_init, &result->image.after_open};
    struct crystalhd_fw_research_result *control = &result->image.controller.state.control;
    struct crystalhd_fw_research_heap_packet_sample *changed;
    unsigned i;
    memset(result, 0, sizeof(*result));
    make_image_result(&result->image);
    for (i = 0; i < 2; i++)
        if (images[i]->attempted && images[i]->read_complete && !images[i]->status)
            fill_heap_sample(samples[i], i);
    if (heap_fault_at) {
        unsigned count = heap_fault_at + 1;
        struct crystalhd_fw_research_heap_packet_sample *sample = samples[heap_fault_at - 1];
        CHECK(heap_fault_at <= 2 && heap_fault_kind >= 1 && heap_fault_kind <= 6);
        memset(sample, 0, sizeof(*sample));
        sample->attempted = heap_fault_kind != 2;
        sample->status = heap_fault_kind == 2 ? -ENODEV : heap_fault_kind == 3 ? -ERANGE :
            heap_fault_kind == 4 ? -ESTALE : heap_fault_kind == 5 ? -ETIMEDOUT : heap_fault_kind == 6 ? -4095 : -EIO;
        control->status = sample->status; control->command_count = count;
        memset(control->replies + count, 0,
               (CRYSTALHD_FW_RESEARCH_MAX_COMMANDS - count) * sizeof(control->replies[0]));
        if (heap_fault_at == 1) {
            memset(&result->image.controller.state.after_open, 0, sizeof(result->image.controller.state.after_open));
            memset(&result->image.controller.after_open, 0, sizeof(result->image.controller.after_open));
            memset(&result->image.after_open, 0, sizeof(result->image.after_open));
            memset(samples[1], 0, sizeof(*samples[1]));
        }
    }
    CHECK(heap_stage < 2);
    changed = samples[heap_stage];
    switch (heap_mutation) {
    case 0: break;
    case 1: changed->attempted = 2; break;
    case 2: changed->read_complete = 2; break;
    case 3: changed->reserved = 1; break;
    case 4: changed->status = 1; break;
    case 5: changed->status = -4096; break;
    case 6: changed->attempted = 0; break;
    case 7: memset(changed, 0, sizeof(*changed)); break;
    case 8: changed->status = -EIO; break;
    case 9: changed->root_after ^= 4; break;
    case 10: changed->root_before = changed->root_after = heap_bad_root; break;
    case 11: {
        uint32_t one = 1;
        CHECK(heap_word < 26);
        memcpy((unsigned char *)changed + 16 + heap_word * sizeof(one), &one, sizeof(one));
        break;
    }
    case 12: changed->read_complete = 1; break;
    case 13: changed->attempted = 1; changed->status = 0; break;
    case 14: control->status = -EIO; break;
    case 15: control->command_count++; result_reply(control, control->command_count - 1, BC_STS_SUCCESS); break;
    case 16: fill_heap_sample(changed, heap_stage); break;
    case 17: result->image.controller.state.request.size = sizeof(result->image); break;
    case 18: control->replies[4].response[63] = 1; break;
    case 20: changed->image_before[0] ^= 4; changed->image_after[0] = changed->image_before[0]; break;
    case 21:
        heap_bases[heap_stage] = heap_bad_value;
        fill_heap_sample(changed, heap_stage); break;
    case 22: changed->image_before[2] = changed->image_after[2] = heap_bad_value; break;
    case 23: changed->image_before[3] = changed->image_after[3] = heap_bad_value; break;
    case 24: CHECK(heap_word < 3); changed->packet_before[heap_word] ^= 4;
        changed->packet_after[heap_word] = changed->packet_before[heap_word]; break;
    case 25: changed->packet_address ^= 4; break;
    case 26: CHECK(heap_word < 4); changed->image_after[heap_word] ^= 4; break;
    case 27: CHECK(heap_word < 3); changed->packet_after[heap_word] ^= 4; break;
    case 28: CHECK(heap_word < 2); changed->slots_after[heap_word] ^= 4; break;
    case 30: memset(changed, 0, sizeof(*changed)); changed->attempted = 1; break;
    default: CHECK(false);
    }
}

static void make_clock_result(struct crystalhd_fw_research_clock_result *result)
{
    struct crystalhd_fw_research_clock_sample *samples[] = {&result->after_init, &result->after_open};
    struct crystalhd_fw_research_state_sample *prerequisites[] = {&result->state.after_init, &result->state.after_open};
    struct crystalhd_fw_research_clock_sample *changed;
    unsigned i, count;
    memset(result, 0, sizeof(*result)); make_state_result(&result->state);
    CHECK(clock_stage < 2 && clock_word < 3);
    if (result->state.control.command_count <= CRYSTALHD_FW_RESEARCH_MAX_COMMANDS)
        memset(result->state.control.replies + result->state.control.command_count, 0,
               (CRYSTALHD_FW_RESEARCH_MAX_COMMANDS - result->state.control.command_count) *
               sizeof(result->state.control.replies[0]));
    if (mutation == 35) result->state.control.cleanup_status = BC_STS_CMD_CANCELLED;
    for (i = 0; i < 2; i++) {
        if (!prerequisites[i]->attempted || !prerequisites[i]->read_complete || prerequisites[i]->status) break;
        samples[i]->attempted = samples[i]->read_complete = 1;
        samples[i]->reset_ctrl = clock_values[i][0];
        samples[i]->perst_clock_ctrl = clock_values[i][1];
        samples[i]->clk_pm_ctrl = clock_values[i][2];
    }
    if (clock_fault_at) {
        CHECK(clock_fault_at <= 2 && clock_fault_kind >= 1 && clock_fault_kind <= 6);
        changed = samples[clock_fault_at - 1]; count = clock_fault_at + 1;
        memset(changed, 0, sizeof(*changed));
        changed->attempted = clock_fault_kind != 2 && clock_fault_kind < 5;
        changed->status = clock_fault_kind == 2 ? -ENODEV : clock_fault_kind == 3 ? -ETIMEDOUT :
            clock_fault_kind == 4 ? -4095 : clock_fault_kind == 5 ? -EAGAIN :
            clock_fault_kind == 6 ? -512 : -EIO; /* Kernel ERESTARTSYS. */
        result->state.control.status = changed->status; result->state.control.command_count = count;
        memset(result->state.control.replies + count, 0,
               (CRYSTALHD_FW_RESEARCH_MAX_COMMANDS - count) * sizeof(result->state.control.replies[0]));
        if (clock_fault_at == 1) {
            memset(&result->state.after_open, 0, sizeof(result->state.after_open));
            memset(samples[1], 0, sizeof(*samples[1]));
        }
    }
    changed = samples[clock_stage];
    switch (clock_mutation) {
    case 0: break;
    case 1: changed->attempted = 2; break;
    case 2: changed->read_complete = 2; break;
    case 3: changed->status = 1; break;
    case 4: changed->status = -4096; break;
    case 5: changed->reserved = 1; break;
    case 6: changed->attempted = 0; break;
    case 7: changed->read_complete = 0; break;
    case 8: memset(changed, 0, sizeof(*changed)); break;
    case 9: changed->status = -EIO; break;
    case 10: changed->status = 0; break;
    case 11:
        if (!clock_word) changed->reset_ctrl = 1;
        else if (clock_word == 1) changed->perst_clock_ctrl = 1;
        else changed->clk_pm_ctrl = 1;
        break;
    case 12: changed->read_complete = 1; break;
    case 13: result->state.control.replies[4].response[63] = 1; break;
    case 14: result->state.control.status = 0; break;
    case 15: result->state.control.command_count++; break;
    case 16: result->state.control.cleanup_attempted = 0; break;
    case 17: result->state.control.download_attempted = 0; break;
    case 18: result->state.control.download_status = BC_STS_IO_ERROR; break;
    case 19: result->state.control.request.size = sizeof(*result); break;
    case 20: result->state.control.replies[0].response[0]++; break;
    case 21: result->state.control.replies[1].response[1]++; break;
    case 22: result->state.control.replies[2].response[3] = 1; break;
    case 23: result->state.control.command_count = 0; break;
    case 24: memset(&result->after_init, 0, sizeof(result->after_init) + sizeof(result->after_open)); break;
    default: CHECK(false);
    }
}

static void make_uart_result(struct crystalhd_fw_research_uart_result *result)
{
    struct crystalhd_fw_research_uart_sample *samples[] = {&result->after_init, &result->after_open};
    struct crystalhd_fw_research_state_sample *prerequisites[] = {&result->state.after_init, &result->state.after_open};
    struct crystalhd_fw_research_uart_sample *changed;
    unsigned i, count;
    memset(result, 0, sizeof(*result)); make_state_result(&result->state);
    CHECK(uart_stage < 2 && uart_word < 3);
    if (result->state.control.command_count <= CRYSTALHD_FW_RESEARCH_MAX_COMMANDS)
        memset(result->state.control.replies + result->state.control.command_count, 0,
               (CRYSTALHD_FW_RESEARCH_MAX_COMMANDS - result->state.control.command_count) *
               sizeof(result->state.control.replies[0]));
    if (mutation == 35) result->state.control.cleanup_status = BC_STS_CMD_CANCELLED;
    for (i = 0; i < 2; i++) {
        if (!prerequisites[i]->attempted || !prerequisites[i]->read_complete || prerequisites[i]->status) break;
        samples[i]->attempted = samples[i]->read_complete = 1;
        samples[i]->arm_uart_ctl = uart_values[i][0];
        samples[i]->pin_mux_ctrl_0 = uart_values[i][1];
        samples[i]->uart_router_sel = uart_values[i][2];
    }
    if (uart_fault_at) {
        CHECK(uart_fault_at <= 2 && uart_fault_kind >= 1 && uart_fault_kind <= 6);
        changed = samples[uart_fault_at - 1]; count = uart_fault_at + 1;
        memset(changed, 0, sizeof(*changed));
        changed->attempted = uart_fault_kind != 2 && uart_fault_kind < 5;
        changed->status = uart_fault_kind == 2 ? -ENODEV : uart_fault_kind == 3 ? -ETIMEDOUT :
            uart_fault_kind == 4 ? -4095 : uart_fault_kind == 5 ? -EAGAIN :
            uart_fault_kind == 6 ? -512 : -EIO; /* Kernel ERESTARTSYS. */
        result->state.control.status = changed->status; result->state.control.command_count = count;
        memset(result->state.control.replies + count, 0,
               (CRYSTALHD_FW_RESEARCH_MAX_COMMANDS - count) * sizeof(result->state.control.replies[0]));
        if (uart_fault_at == 1) {
            memset(&result->state.after_open, 0, sizeof(result->state.after_open));
            memset(samples[1], 0, sizeof(*samples[1]));
        }
    }
    changed = samples[uart_stage];
    switch (uart_mutation) {
    case 0: break;
    case 1: changed->attempted = 2; break;
    case 2: changed->read_complete = 2; break;
    case 3: changed->status = 1; break;
    case 4: changed->status = -4096; break;
    case 5: changed->reserved = 1; break;
    case 6: changed->attempted = 0; break;
    case 7: changed->read_complete = 0; break;
    case 8: memset(changed, 0, sizeof(*changed)); break;
    case 9: changed->status = -EIO; break;
    case 10: changed->status = 0; break;
    case 11:
        if (!uart_word) changed->arm_uart_ctl = 1;
        else if (uart_word == 1) changed->pin_mux_ctrl_0 = 1;
        else changed->uart_router_sel = 1;
        break;
    case 12: changed->read_complete = 1; break;
    case 13: result->state.control.replies[4].response[63] = 1; break;
    case 14: result->state.control.status = 0; break;
    case 15: result->state.control.command_count++; break;
    case 16: result->state.control.cleanup_attempted = 0; break;
    case 17: result->state.control.download_attempted = 0; break;
    case 18: result->state.control.download_status = BC_STS_IO_ERROR; break;
    case 19: result->state.control.request.size = sizeof(*result); break;
    case 20: result->state.control.replies[0].response[0]++; break;
    case 21: result->state.control.replies[1].response[1]++; break;
    case 22: result->state.control.replies[2].response[3] = 1; break;
    case 23: result->state.control.command_count = 0; break;
    case 24: memset(&result->after_init, 0, sizeof(result->after_init) + sizeof(result->after_open)); break;
    case 25: result->state.control.replies[3].command = result->state.control.replies[3].response[0] = eCMD_C011_DEC_CHAN_OPEN; break;
    case 26: result->state.control.replies[4].command = result->state.control.replies[4].response[0] = eCMD_C011_DEC_CHAN_STATUS; break;
    default: CHECK(false);
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
    if (command == CRYSTALHD_FW_RESEARCH_RUN_STATE) {
        struct crystalhd_fw_research_state_result *result = argument;
        const unsigned char *bytes = argument;
        unsigned i;
        CHECK(infos == 1 && !runs++ && !state_runs++);
        state_submitted = result->request;
        CHECK(state_submitted.version == 1 && state_submitted.size == sizeof(*result));
        CHECK(!state_submitted.flags && !state_submitted.reserved);
        for (i = sizeof(result->request); i < sizeof(*result); i++) CHECK(!bytes[i]);
        memset(&submitted, 0, sizeof(submitted));
        submitted.version = 1; submitted.size = sizeof(result->control);
        submitted.selector = CRYSTALHD_FW_RESEARCH_H264_CONTROL;
        if (run_error) { errno = run_error; return -1; }
        make_state_result(result); return 0;
    }
    if (command == CRYSTALHD_FW_RESEARCH_RUN_CONTROLLER) {
        struct crystalhd_fw_research_controller_result *result = argument;
        const unsigned char *bytes = argument;
        unsigned i;
        CHECK(infos == 1 && !runs++ && !controller_runs++ && !state_runs);
        state_submitted = result->state.request;
        CHECK(state_submitted.version == 1 && state_submitted.size == sizeof(*result));
        CHECK(!state_submitted.flags && !state_submitted.reserved);
        for (i = sizeof(result->state.request); i < sizeof(*result); i++) CHECK(!bytes[i]);
        memset(&submitted, 0, sizeof(submitted));
        submitted.version = 1; submitted.size = sizeof(result->state.control);
        submitted.selector = CRYSTALHD_FW_RESEARCH_H264_CONTROL;
        if (run_error) { errno = run_error; return -1; }
        make_controller_result(result); return 0;
    }
    if (command == CRYSTALHD_FW_RESEARCH_RUN_IMAGE) {
        struct crystalhd_fw_research_image_result *result = argument;
        const unsigned char *bytes = argument;
        unsigned i;
        CHECK(infos == 1 && !runs++ && !image_runs++ && !state_runs && !controller_runs);
        state_submitted = result->controller.state.request;
        CHECK(state_submitted.version == 1 && state_submitted.size == sizeof(*result));
        CHECK(!state_submitted.flags && !state_submitted.reserved);
        for (i = sizeof(result->controller.state.request); i < sizeof(*result); i++) CHECK(!bytes[i]);
        memset(&submitted, 0, sizeof(submitted));
        submitted.version = 1; submitted.size = sizeof(result->controller.state.control);
        submitted.selector = CRYSTALHD_FW_RESEARCH_H264_CONTROL;
        if (run_error) { errno = run_error; return -1; }
        make_image_result(result); return 0;
    }
    if (command == CRYSTALHD_FW_RESEARCH_RUN_PACKET) {
        struct crystalhd_fw_research_packet_result *result = argument;
        const unsigned char *bytes = argument;
        unsigned i;
        CHECK(infos == 1 && !runs++ && !packet_runs++ && !state_runs && !controller_runs && !image_runs);
        state_submitted = result->image.controller.state.request;
        CHECK(state_submitted.version == 1 && state_submitted.size == sizeof(*result));
        CHECK(!state_submitted.flags && !state_submitted.reserved);
        for (i = sizeof(result->image.controller.state.request); i < sizeof(*result); i++) CHECK(!bytes[i]);
        memset(&submitted, 0, sizeof(submitted));
        submitted.version = 1; submitted.size = sizeof(result->image.controller.state.control);
        submitted.selector = CRYSTALHD_FW_RESEARCH_H264_CONTROL;
        if (run_error) { errno = run_error; return -1; }
        make_packet_result(result); return 0;
    }
    if (command == CRYSTALHD_FW_RESEARCH_RUN_HEAP_PACKET) {
        struct crystalhd_fw_research_heap_packet_result *result = argument;
        const unsigned char *bytes = argument;
        unsigned i;
        CHECK(infos == 1 && !runs++ && !heap_runs++ && !state_runs && !controller_runs && !image_runs && !packet_runs);
        state_submitted = result->image.controller.state.request;
        CHECK(state_submitted.version == 1 && state_submitted.size == sizeof(*result));
        CHECK(!state_submitted.flags && !state_submitted.reserved);
        for (i = sizeof(result->image.controller.state.request); i < sizeof(*result); i++) CHECK(!bytes[i]);
        memset(&submitted, 0, sizeof(submitted));
        submitted.version = 1; submitted.size = sizeof(result->image.controller.state.control);
        submitted.selector = CRYSTALHD_FW_RESEARCH_H264_CONTROL;
        if (run_error) { errno = run_error; return -1; }
        make_heap_result(result); return 0;
    }
    if (command == CRYSTALHD_FW_RESEARCH_RUN_CLOCK) {
        struct crystalhd_fw_research_clock_result *result = argument;
        const unsigned char *bytes = argument;
        unsigned i;
        CHECK(infos == 1 && !runs++ && !clock_runs++ && !state_runs && !controller_runs && !image_runs && !packet_runs && !heap_runs);
        state_submitted = result->state.request;
        CHECK(state_submitted.version == 1 && state_submitted.size == sizeof(*result));
        CHECK(!state_submitted.flags && !state_submitted.reserved);
        for (i = sizeof(result->state.request); i < sizeof(*result); i++) CHECK(!bytes[i]);
        memset(&submitted, 0, sizeof(submitted));
        submitted.version = 1; submitted.size = sizeof(result->state.control);
        submitted.selector = CRYSTALHD_FW_RESEARCH_H264_CONTROL;
        if (run_error) { errno = run_error; return -1; }
        make_clock_result(result); return 0;
    }
    if (command == CRYSTALHD_FW_RESEARCH_RUN_UART) {
        struct crystalhd_fw_research_uart_result *result = argument;
        const unsigned char *bytes = argument;
        unsigned i;
        CHECK(command == 0xc6785299UL);
        CHECK(infos == 1 && !runs++ && !uart_runs++ && !state_runs && !controller_runs && !image_runs && !packet_runs && !heap_runs && !clock_runs);
        state_submitted = result->state.request;
        CHECK(state_submitted.version == 1 && state_submitted.size == sizeof(*result));
        CHECK(!state_submitted.flags && !state_submitted.reserved);
        for (i = sizeof(result->state.request); i < sizeof(*result); i++) CHECK(!bytes[i]);
        memset(&submitted, 0, sizeof(submitted));
        submitted.version = 1; submitted.size = sizeof(result->state.control);
        submitted.selector = CRYSTALHD_FW_RESEARCH_H264_CONTROL;
        if (run_error) { errno = run_error; return -1; }
        make_uart_result(result); return 0;
    }
    CHECK(command == CRYSTALHD_FW_RESEARCH_RUN && infos == 1 && !runs++);
    submitted = ((struct crystalhd_fw_research_result *)argument)->request;
    CHECK(submitted.version == 1 && submitted.size == sizeof(struct crystalhd_fw_research_result));
    CHECK(submitted.selector >= 1 && submitted.selector <= 11);
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
    opens = stats = infos = runs = state_runs = controller_runs = image_runs = packet_runs = heap_runs = clock_runs = uart_runs = closes = 0;
    open_error = stat_error = info_error = run_error = close_error = 0;
    character = true; output_error = flush_error = false;
    mutation = fault_at = fault_kind = response_pattern = 0;
    state_mutation = sample_fault_at = sample_fault_kind = 0; state_opaque = false;
    controller_mutation = root_fault_at = root_fault_kind = 0;
    root_values[0] = root_values[1] = 0xd5384;
    image_fault_at = image_fault_kind = image_mutation = image_mutation_stage = image_word = 0;
    image_roots[0] = image_roots[1] = 0xd53dc;
    memset(image_words, 0, sizeof(image_words)); image_bad_root = 0;
    packet_fault_at = packet_fault_kind = packet_mutation = packet_mutation_stage = packet_word = 0;
    packet_roots[0] = packet_roots[1] = 0xd53e0;
    memset(packet_image_words, 0, sizeof(packet_image_words));
    memset(packet_words, 0, sizeof(packet_words)); packet_bad_root = 0;
    heap_fault_at = heap_fault_kind = heap_mutation = heap_stage = heap_word = 0;
    heap_roots[0] = heap_roots[1] = 0xd53e0;
    heap_bases[0] = heap_bases[1] = 0x117000;
    heap_extent = 0x100000; heap_owned = 1; heap_bad_root = heap_bad_value = 0;
    memset(heap_header, 0, sizeof(heap_header)); memset(heap_slots, 0, sizeof(heap_slots));
    clock_fault_at = clock_fault_kind = clock_mutation = clock_stage = clock_word = 0;
    memset(clock_values, 0, sizeof(clock_values));
    uart_fault_at = uart_fault_kind = uart_mutation = uart_stage = uart_word = 0;
    memset(uart_values, 0, sizeof(uart_values));
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
static char *state_args[] = {"probe", "--fixed-state", "--acknowledge-card-reset", "--expected-generation", "42", NULL};
static char *controller_args[] = {"probe", "--controller-root", "--acknowledge-card-reset", "--expected-generation", "42", NULL};
static char *image_args[] = {"probe", "--controller-image", "--acknowledge-card-reset", "--expected-generation", "42", NULL};
static char *packet_args[] = {"probe", "--controller-packet", "--acknowledge-card-reset", "--expected-generation", "42", NULL};
static char *heap_args[] = {"probe", "--heap-packet", "--acknowledge-card-reset", "--expected-generation", "42", NULL};
static char *clock_args[] = {"probe", "--clock-state", "--acknowledge-card-reset", "--expected-generation", "42", NULL};
static char *uart_args[] = {"probe", "--uart-state", "--acknowledge-card-reset", "--expected-generation", "42", NULL};
static char *h261_args[] = {"probe", "--h261-control", "--acknowledge-card-reset", "--expected-generation", "42", NULL};
static char *h263_args[] = {"probe", "--h263-control", "--acknowledge-card-reset", "--expected-generation", "42", NULL};
static char *mpeg1_args[] = {"probe", "--mpeg1-control", "--acknowledge-card-reset", "--expected-generation", "42", NULL};
static char **named_args[] = {h261_args, h263_args, mpeg1_args};
static char **all_live_args[] = {version_args, h264_args, h261_args, h263_args, mpeg1_args};
static char *raw_args[][7] = {
    {"probe", "--scaling-filters-command", "--acknowledge-card-reset", "--expected-generation", "42", NULL},
    {"probe", "--pic-capture-command", "--acknowledge-card-reset", "--expected-generation", "42", NULL},
    {"probe", "--csc-command", "--acknowledge-card-reset", "--expected-generation", "42", NULL},
    {"probe", "--fgt-command", "--acknowledge-card-reset", "--expected-generation", "42", NULL},
    {"probe", "--custom-vidout-command", "--acknowledge-card-reset", "--expected-generation", "42", NULL},
    {"probe", "--fill-pic-buf-command", "--acknowledge-card-reset", "--expected-generation", "42", NULL},
};

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
    CHECK(strstr(output, "\"generation\":\"42\"") && strstr(output, "\"research_selector_mask\":2047"));
    CHECK(strstr(output, CRYSTALHD_FW_RESEARCH_FIRMWARE_SHA256));
    CHECK(!strstr(output, "decoded_response"));
    for (i = 0; i < 8; i++) {
        reset();
        if (i == 0) metadata.version++;
        if (i == 1) metadata.size--;
        if (i == 2) metadata.generation = 0;
        if (i == 3) metadata.selector_mask |= 2048;
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

static void test_raw_commands(void)
{
    unsigned action, phase, kind, pattern, malformed;
    for (action = 0; action < sizeof(raw_args) / sizeof(raw_args[0]); action++) {
        char *missing_ack[] = {"probe", raw_args[action][1], "--expected-generation", "42", NULL};
        char *missing_generation[] = {"probe", raw_args[action][1], "--acknowledge-card-reset", NULL};
        char *duplicate[] = {"probe", raw_args[action][1], raw_args[action][1], "--acknowledge-card-reset",
            "--expected-generation", "42", NULL};
        char *mixed[] = {"probe", raw_args[action][1], raw_args[(action + 1) % 6][1],
            "--acknowledge-card-reset", "--expected-generation", "42", NULL};
        char *legacy_mixed[] = {"probe", raw_args[action][1], "--h264-control",
            "--acknowledge-card-reset", "--expected-generation", "42", NULL};
        char *command[] = {"probe", raw_args[action][1], "--acknowledge-card-reset",
            "--expected-generation", "42", "--command", "0x73763180", NULL};
        char *address[] = {"probe", raw_args[action][1], "--acknowledge-card-reset",
            "--expected-generation", "42", "--address", "0x70000", NULL};
        char *algorithm[] = {"probe", raw_args[action][1], "--acknowledge-card-reset",
            "--expected-generation", "42", "--algorithm=8", NULL};
        char *selector[] = {"probe", raw_args[action][1], "--acknowledge-card-reset",
            "--expected-generation", "42", "--selector", "6", NULL};
        char **invalid[] = {missing_ack, missing_generation, duplicate, mixed, legacy_mixed,
            command, address, algorithm, selector};
        for (malformed = 0; malformed < sizeof(invalid) / sizeof(invalid[0]); malformed++) {
            reset(); CHECK(invoke(invalid[malformed]) == 1 && !opens && !output[0]);
        }
        for (pattern = 0; pattern <= 2; pattern++) {
            reset(); response_pattern = pattern;
            CHECK(!invoke(raw_args[action]) && submitted.selector == action + 6 && runs == 1 && closes == 1);
            CHECK(strstr(output, "\"command_count\":3") && !strstr(output, "\"sequence\":4"));
            check_decoding(3, true);
        }
        reset(); metadata.selector_mask = 31;
        CHECK(invoke(raw_args[action]) == 1 && infos == 1 && !runs && !output[0] && strstr(errors, "unavailable"));
        reset(); metadata.selector_mask = 1U << (action + 5);
        CHECK(!invoke(raw_args[action]) && runs == 1);
        reset(); metadata.selector_mask &= ~(1U << (action + 5));
        CHECK(invoke(raw_args[action]) == 1 && !runs && strstr(errors, "unavailable"));
        reset(); metadata.generation++;
        CHECK(invoke(raw_args[action]) == 1 && !runs && !output[0] && strstr(errors, "generation changed"));
        for (phase = 1; phase <= 3; phase++) {
            for (kind = 1; kind <= 7; kind++) {
                if (kind == 6) continue;
                reset(); response_pattern = 2; fault_at = phase; fault_kind = kind;
                CHECK(invoke(raw_args[action]) == 1 && runs == 1 && closes == 1 && output[0]);
                CHECK(!strstr(errors, "Invalid research result"));
                check_decoding(phase, phase > 2);
            }
        }
        for (malformed = 59; malformed <= 69; malformed++) {
            reset(); mutation = malformed; response_pattern = 2;
            if (malformed == 67) CHECK(!invoke(raw_args[action]) && output[0]);
            else CHECK(invoke(raw_args[action]) == 1 && runs == 1 && closes == 1);
            if (malformed == 59 || malformed == 61 || malformed == 67) {
                CHECK(output[0] && !strstr(errors, "Invalid research result"));
                check_decoding(3, true);
                if (malformed != 67) {
                    CHECK(strstr(output, "\"header_matches\":false") && strstr(output, "\"status\":-71"));
                    CHECK(strstr(output, "\"transport_status\":0"));
                }
                if (malformed == 61) CHECK(strstr(errors, "remain retained"));
            } else CHECK(!output[0] && strstr(errors, "Invalid research result"));
        }
        for (malformed = 51; malformed <= 52; malformed++) {
            reset(); response_pattern = 2; mutation = malformed;
            CHECK(invoke(raw_args[action]) == 1 && output[0]); check_decoding(3, true);
        }
        reset(); mutation = 35; response_pattern = 2;
        CHECK(invoke(raw_args[action]) == 1 && output[0] && strstr(errors, "remain retained"));
        check_decoding(3, true);
        reset(); close_error = EINTR;
        CHECK(invoke(raw_args[action]) == 1 && output[0] && strstr(errors, "close research device"));
    }
    for (action = 0; action < sizeof(all_live_args) / sizeof(all_live_args[0]); action++) {
        reset(); metadata.selector_mask = 31;
        CHECK(!invoke(all_live_args[action]) && runs == 1 && submitted.selector == action + 1);
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

static void test_fixed_state(void)
{
    static const unsigned read_failures[] = {1, 2, 7, 8, 9};
    char *invalid[][10] = {
        {"probe", "--fixed-state", NULL},
        {"probe", "--fixed-state", "--acknowledge-card-reset", NULL},
        {"probe", "--fixed-state", "--expected-generation", "42", NULL},
        {"probe", "--fixed-state", "--fixed-state", "--acknowledge-card-reset", "--expected-generation", "42", NULL},
        {"probe", "--fixed-state", "--info", "--acknowledge-card-reset", "--expected-generation", "42", NULL},
        {"probe", "--fixed-state", "--h264-control", "--acknowledge-card-reset", "--expected-generation", "42", NULL},
        {"probe", "--fixed-state", "--csc-command", "--acknowledge-card-reset", "--expected-generation", "42", NULL},
        {"probe", "--fixed-state", "--acknowledge-card-reset", "--expected-generation", "42", "--address", "0x6fc", NULL},
        {"probe", "--fixed-state", "--acknowledge-card-reset", "--expected-generation", "42", "--selector", "2", NULL},
    };
    unsigned i, stage, kind, phase;
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        reset(); CHECK(invoke(invalid[i]) == 1 && !opens && !output[0]);
    }
    reset(); CHECK(!invoke(state_args) && state_runs == 1 && runs == 1);
    CHECK(strstr(output, "\"fixed_state\":true") && strstr(output, "\"command_count\":5"));
    CHECK(strstr(output, "\"calibration\":{\"attempted\":true,\"status\":0,\"read_complete\":true,\"raw_words\":[866816,0,0,0]}"));
    CHECK(strstr(output, "\"after_init\":{\"attempted\":true,\"status\":0,\"read_complete\":true,\"raw_words\":[1,866816,0,0]}"));
    CHECK(strstr(output, "\"after_open\":{\"attempted\":true,\"status\":0,\"read_complete\":true,\"raw_words\":[1,866816,1,512]}"));
    check_decoding(5, true);
    reset(); state_opaque = true; response_pattern = 2;
    CHECK(!invoke(state_args) && state_runs == 1 && strstr(output, "4294967041,4278190592"));
    check_decoding(5, true);
    reset(); metadata.selector_mask = 1;
    CHECK(invoke(state_args) == 1 && !runs && !state_runs && strstr(errors, "unavailable"));
    reset(); metadata.selector_mask = 2; CHECK(!invoke(state_args) && state_runs == 1);
    reset(); metadata.generation++;
    CHECK(invoke(state_args) == 1 && !runs && !output[0] && strstr(errors, "generation changed"));
    reset(); metadata.firmware_sha256[0] ^= 1;
    CHECK(invoke(state_args) == 1 && !runs && !output[0] && strstr(errors, "Invalid research metadata"));
    reset(); run_error = ENOTTY;
    CHECK(invoke(state_args) == 1 && state_runs == 1 && runs == 1 && !output[0] && strstr(errors, "no retry"));
    reset(); run_error = EINTR;
    CHECK(invoke(state_args) == 1 && state_runs == 1 && runs == 1 && !output[0] && strstr(errors, "no retry"));
    for (stage = 1; stage <= 3; stage++) {
        for (i = 0; i < sizeof(read_failures) / sizeof(read_failures[0]); i++) {
            reset(); sample_fault_at = stage; sample_fault_kind = read_failures[i];
            CHECK(invoke(state_args) == 1 && state_runs == 1 && output[0]);
            CHECK(!strstr(errors, "Invalid fixed-state result"));
            CHECK(strstr(output, "\"read_complete\":false,\"raw_words\":[0,0,0,0]"));
            if (read_failures[i] == 9)
                CHECK(strstr(output, "\"attempted\":false,\"status\":-19,\"read_complete\":false"));
            check_decoding(stage == 3 ? 3 : 2, true);
        }
        for (kind = 3; kind <= 6; kind++) {
            reset(); sample_fault_at = stage; sample_fault_kind = kind;
            CHECK(invoke(state_args) == 1 && state_runs == 1);
            if (stage == 1 && kind != 3) CHECK(!output[0] && strstr(errors, "Invalid fixed-state result"));
            else {
                CHECK(output[0] && !strstr(errors, "Invalid fixed-state result"));
                CHECK(strstr(output, "\"status\":-71,\"read_complete\":true,\"raw_words\":"));
                check_decoding(stage == 3 ? 3 : 2, true);
            }
        }
    }
    for (phase = 1; phase <= 5; phase++) {
        for (kind = 1; kind <= 7; kind++) {
            if (kind == 6) continue;
            reset(); fault_at = phase; fault_kind = kind;
            CHECK(invoke(state_args) == 1 && state_runs == 1 && output[0]);
            CHECK(!strstr(errors, "Invalid fixed-state result"));
            check_decoding(phase, phase > 2);
        }
    }
    reset(); fault_at = 3; fault_kind = 6;
    CHECK(invoke(state_args) == 1 && output[0] && !strstr(errors, "Invalid fixed-state result"));
    CHECK(strstr(output, "\"after_open\":{\"attempted\":false,\"status\":0,\"read_complete\":false,\"raw_words\":[0,0,0,0]}"));
    for (i = 1; i <= 40; i++) {
        reset(); state_mutation = i;
        if (i == 30 || i == 31) { sample_fault_at = 1; sample_fault_kind = 3; }
        if (i == 38) { sample_fault_at = 1; sample_fault_kind = 1; }
        if (i == 39) { sample_fault_at = 1; sample_fault_kind = 3; }
        if (i == 40) { sample_fault_at = 2; sample_fault_kind = 1; }
        CHECK(invoke(state_args) == 1 && state_runs == 1 && !output[0]);
        CHECK(strstr(errors, "Invalid fixed-state result"));
    }
    for (i = 1; i <= 29; i++) {
        reset(); mutation = i;
        CHECK(invoke(state_args) == 1 && !output[0] && strstr(errors, "Invalid fixed-state result"));
    }
    for (i = 33; i <= 35; i++) {
        reset(); mutation = i;
        CHECK(invoke(state_args) == 1 && output[0] && !strstr(errors, "Invalid fixed-state result"));
    }
    reset(); mutation = 41; CHECK(invoke(state_args) == 1 && output[0]);
    CHECK(strstr(output, "\"firmware_hash_valid\":false,\"observed_firmware_sha256\":null"));
    reset(); mutation = 43; CHECK(invoke(state_args) == 1 && output[0]);
    for (i = 51; i <= 52; i++) {
        reset(); mutation = i; CHECK(invoke(state_args) == 1 && output[0]);
        check_decoding(5, true);
    }
    reset(); close_error = EINTR;
    CHECK(invoke(state_args) == 1 && output[0] && strstr(errors, "close research device"));
    reset(); output_error = true; CHECK(invoke(state_args) == 1 && state_runs == 1);
    reset(); flush_error = true; CHECK(invoke(state_args) == 1 && state_runs == 1);
}

static void state_json_examples(void)
{
    static const unsigned read_failures[] = {1, 2, 7, 8, 9};
    static const unsigned control_failures[] = {33, 34, 35, 41, 43, 51, 52, 36};
    unsigned pattern, stage, kind, phase, i;
    for (pattern = 0; pattern <= 2; pattern++) {
        reset(); response_pattern = pattern; CHECK(!invoke(state_args)); fputs(output, stdout);
    }
    reset(); state_opaque = true; response_pattern = 2;
    CHECK(!invoke(state_args)); fputs(output, stdout);
    for (stage = 1; stage <= 3; stage++) {
        for (i = 0; i < sizeof(read_failures) / sizeof(read_failures[0]); i++) {
            reset(); sample_fault_at = stage; sample_fault_kind = read_failures[i];
            CHECK(invoke(state_args) == 1 && output[0]); fputs(output, stdout);
        }
        for (kind = 3; kind <= 6; kind++) {
            if (stage == 1 && kind != 3) continue;
            reset(); sample_fault_at = stage; sample_fault_kind = kind;
            CHECK(invoke(state_args) == 1 && output[0]); fputs(output, stdout);
        }
    }
    for (phase = 1; phase <= 5; phase++) {
        for (kind = 1; kind <= 7; kind++) {
            if (kind == 6) continue;
            reset(); fault_at = phase; fault_kind = kind;
            CHECK(invoke(state_args) == 1 && output[0]); fputs(output, stdout);
        }
    }
    reset(); fault_at = 3; fault_kind = 6; CHECK(invoke(state_args) == 1); fputs(output, stdout);
    for (i = 0; i < sizeof(control_failures) / sizeof(control_failures[0]); i++) {
        reset(); mutation = control_failures[i]; CHECK(invoke(state_args) == 1 && output[0]); fputs(output, stdout);
    }
    reset(); close_error = EINTR; CHECK(invoke(state_args) == 1 && output[0]); fputs(output, stdout);
}

static const uint32_t controller_roots[] = {
    0, 1, 0xd5380, 0xd5384, 0xd5385, 0x115c88, 0x115c89,
    0x115c8c, 0x115ffc, 0x116000, 0x80000000, 0xfffffc88, UINT32_MAX,
};

static void test_controller_root(void)
{
    static const unsigned read_failures[] = {1, 2, 7, 8, 9};
    char *invalid[][11] = {
        {"probe", "--controller-root", NULL},
        {"probe", "--controller-root", "--acknowledge-card-reset", NULL},
        {"probe", "--controller-root", "--expected-generation", "42", NULL},
        {"probe", "--controller-root", "--controller-root", "--acknowledge-card-reset", "--expected-generation", "42", NULL},
        {"probe", "--controller-root", "--info", "--acknowledge-card-reset", "--expected-generation", "42", NULL},
        {"probe", "--controller-root", "--fixed-state", "--acknowledge-card-reset", "--expected-generation", "42", NULL},
        {"probe", "--controller-root", "--h264-control", "--acknowledge-card-reset", "--expected-generation", "42", NULL},
        {"probe", "--controller-root", "--csc-command", "--acknowledge-card-reset", "--expected-generation", "42", NULL},
        {"probe", "--controller-root", "--acknowledge-card-reset", "--expected-generation", "42", "--address", "0xd3a08", NULL},
        {"probe", "--controller-root", "--acknowledge-card-reset", "--expected-generation", "42", "--selector", "2", NULL},
        {"probe", "--controller-root", "--acknowledge-card-reset", "--expected-generation", "0", NULL},
        {"probe", "--controller-root", "--acknowledge-card-reset", "--expected-generation", "42", "--acknowledge-card-reset", NULL},
        {"probe", "--controller-root", "--acknowledge-card-reset", "--expected-generation", "42", "--root", "0", NULL},
    };
    unsigned i, stage, kind, phase;
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        reset(); CHECK(invoke(invalid[i]) == 1 && !opens && !output[0]);
    }
    for (i = 0; i < sizeof(all_live_args) / sizeof(all_live_args[0]); i++) {
        char *conflict[] = {"probe", "--controller-root", all_live_args[i][1],
                           "--acknowledge-card-reset", "--expected-generation", "42", NULL};
        reset(); CHECK(invoke(conflict) == 1 && !opens && !output[0]);
    }
    for (i = 0; i < sizeof(raw_args) / sizeof(raw_args[0]); i++) {
        char *conflict[] = {"probe", "--controller-root", raw_args[i][1],
                           "--acknowledge-card-reset", "--expected-generation", "42", NULL};
        reset(); CHECK(invoke(conflict) == 1 && !opens && !output[0]);
    }
    reset(); CHECK(!invoke(controller_args) && controller_runs == 1 && !state_runs && runs == 1);
    CHECK(strstr(output, "\"controller_root\":true") && strstr(output, "\"root_values_equal\":true"));
    CHECK(strstr(output, "\"raw_root\":873348,\"candidate_alignment_4\":true,\"candidate_full_span_in_window\":true"));
    CHECK(strstr(output, "\"returned_pointer_followed\":false") && strstr(output, "\"equality_excludes_aba\":false"));
    CHECK(strstr(output, "\"ownership_established\":false") && strstr(output, "\"lease_established\":false"));
    check_decoding(5, true);
    for (i = 0; i < sizeof(controller_roots) / sizeof(controller_roots[0]); i++) {
        reset(); root_values[0] = root_values[1] = controller_roots[i];
        CHECK(!invoke(controller_args) && controller_runs == 1 && output[0]);
        CHECK(!strstr(errors, "Invalid controller-root result"));
        CHECK(strstr(output, "\"root_values_equal\":true"));
    }
    reset(); root_values[1] = UINT32_MAX;
    CHECK(!invoke(controller_args) && strstr(output, "\"root_values_equal\":false"));
    reset(); metadata.selector_mask = 1;
    CHECK(invoke(controller_args) == 1 && !runs && !controller_runs && strstr(errors, "unavailable"));
    reset(); metadata.selector_mask = 2; CHECK(!invoke(controller_args) && controller_runs == 1);
    reset(); metadata.generation++;
    CHECK(invoke(controller_args) == 1 && !runs && !output[0] && strstr(errors, "generation changed"));
    reset(); metadata.reserved[0] = 1;
    CHECK(invoke(controller_args) == 1 && !runs && !output[0] && strstr(errors, "Invalid research metadata"));
    reset(); metadata.selector_mask |= 1U << 31;
    CHECK(invoke(controller_args) == 1 && !runs && !output[0] && strstr(errors, "Invalid research metadata"));
    reset(); metadata.firmware_sha256[0] ^= 1;
    CHECK(invoke(controller_args) == 1 && !runs && !output[0] && strstr(errors, "Invalid research metadata"));
    reset(); open_error = EACCES;
    CHECK(invoke(controller_args) == 1 && opens == 1 && !runs && !output[0]);
    reset(); character = false;
    CHECK(invoke(controller_args) == 1 && opens == 1 && !runs && !output[0]);
    reset(); info_error = ENOTTY;
    CHECK(invoke(controller_args) == 1 && infos == 1 && !runs && !output[0]);
    {
        char *reordered[] = {"probe", "--expected-generation", "42", "--acknowledge-card-reset", "--controller-root", NULL};
        reset(); CHECK(!invoke(reordered) && controller_runs == 1 && !state_runs);
    }
    for (i = 0; i < 2; i++) {
        reset(); run_error = i ? EINTR : ENOTTY;
        CHECK(invoke(controller_args) == 1 && controller_runs == 1 && runs == 1 && !state_runs && !output[0]);
        CHECK(strstr(errors, "no retry"));
    }
    for (stage = 1; stage <= 2; stage++) {
        for (kind = 1; kind <= 5; kind++) {
            reset(); root_fault_at = stage; root_fault_kind = kind;
            CHECK(invoke(controller_args) == 1 && controller_runs == 1 && output[0]);
            CHECK(!strstr(errors, "Invalid controller-root result"));
            CHECK(strstr(output, "\"raw_root\":null,\"candidate_alignment_4\":null,\"candidate_full_span_in_window\":null"));
            CHECK(strstr(output, "\"root_values_equal\":null"));
            if (kind == 2) CHECK(strstr(output, "\"attempted\":false,\"status\":-19,\"read_complete\":false"));
            check_decoding(stage == 1 ? 2 : 3, true);
        }
    }
    for (stage = 1; stage <= 3; stage++) {
        for (i = 0; i < sizeof(read_failures) / sizeof(read_failures[0]); i++) {
            reset(); sample_fault_at = stage; sample_fault_kind = read_failures[i];
            CHECK(invoke(controller_args) == 1 && controller_runs == 1 && output[0]);
            CHECK(!strstr(errors, "Invalid controller-root result"));
        }
        reset(); sample_fault_at = stage; sample_fault_kind = 3;
        CHECK(invoke(controller_args) == 1 && output[0] && !strstr(errors, "Invalid controller-root result"));
    }
    for (phase = 1; phase <= 5; phase++) {
        for (kind = 1; kind <= 7; kind++) {
            if (kind == 6 && phase != 3) continue;
            reset(); fault_at = phase; fault_kind = kind;
            CHECK(invoke(controller_args) == 1 && output[0] && !strstr(errors, "Invalid controller-root result"));
        }
    }
    for (i = 1; i <= 35; i++) {
        reset(); controller_mutation = i;
        if ((i >= 9 && i <= 13) || i == 14) { root_fault_at = 1; root_fault_kind = 2; }
        if (i == 11) root_fault_kind = 3;
        if (i >= 23 && i <= 27) { root_fault_at = 2; root_fault_kind = 2; }
        if (i == 25) root_fault_kind = 3;
        if (i == 28) { sample_fault_at = 1; sample_fault_kind = 1; }
        if (i == 29) { sample_fault_at = 1; sample_fault_kind = 1; }
        if (i == 31) mutation = 35;
        if (i == 34) mutation = 41;
        if (i == 35) { sample_fault_at = 2; sample_fault_kind = 3; }
        CHECK(invoke(controller_args) == 1 && controller_runs == 1 && !output[0]);
        CHECK(strstr(errors, "Invalid controller-root result"));
    }
    /* A forged successful root cannot override a failed old fixed sample. */
    for (stage = 2; stage <= 3; stage++) {
        for (kind = 1; kind <= 3; kind++) {
            reset(); sample_fault_at = stage; sample_fault_kind = kind;
            controller_mutation = stage == 2 ? 35 : 14;
            CHECK(invoke(controller_args) == 1 && !output[0] && strstr(errors, "Invalid controller-root result"));
        }
    }
    for (i = 1; i <= 40; i++) {
        reset(); state_mutation = i;
        if (i == 30 || i == 31) { sample_fault_at = 1; sample_fault_kind = 3; }
        if (i == 38) { sample_fault_at = 1; sample_fault_kind = 1; }
        if (i == 39) { sample_fault_at = 1; sample_fault_kind = 3; }
        if (i == 40) { sample_fault_at = 2; sample_fault_kind = 1; }
        CHECK(invoke(controller_args) == 1 && !output[0] && strstr(errors, "Invalid controller-root result"));
    }
    for (i = 1; i <= 29; i++) {
        reset(); mutation = i;
        CHECK(invoke(controller_args) == 1 && !output[0] && strstr(errors, "Invalid controller-root result"));
    }
    for (i = 33; i <= 35; i++) {
        reset(); mutation = i;
        CHECK(invoke(controller_args) == 1 && output[0] && !strstr(errors, "Invalid controller-root result"));
    }
    for (i = 41; i <= 43; i += 2) {
        reset(); mutation = i; CHECK(invoke(controller_args) == 1 && output[0]);
    }
    for (i = 51; i <= 52; i++) {
        reset(); mutation = i; CHECK(invoke(controller_args) == 1 && output[0]);
    }
    reset(); close_error = EINTR;
    CHECK(invoke(controller_args) == 1 && output[0] && strstr(errors, "close research device"));
    reset(); output_error = true; CHECK(invoke(controller_args) == 1 && controller_runs == 1);
    reset(); flush_error = true; CHECK(invoke(controller_args) == 1 && controller_runs == 1);
}

static void controller_json_examples(void)
{
    static const unsigned control_failures[] = {33, 34, 35, 41, 43, 51, 52, 36};
    unsigned i, stage, kind, phase;
    for (i = 0; i < sizeof(controller_roots) / sizeof(controller_roots[0]); i++) {
        reset(); root_values[0] = root_values[1] = controller_roots[i];
        CHECK(!invoke(controller_args)); fputs(output, stdout);
    }
    reset(); root_values[1] = UINT32_MAX; CHECK(!invoke(controller_args)); fputs(output, stdout);
    for (stage = 1; stage <= 2; stage++) {
        for (kind = 1; kind <= 5; kind++) {
            reset(); root_fault_at = stage; root_fault_kind = kind;
            CHECK(invoke(controller_args) == 1 && output[0]); fputs(output, stdout);
        }
    }
    for (stage = 1; stage <= 3; stage++) {
        for (kind = 1; kind <= 3; kind++) {
            reset(); sample_fault_at = stage; sample_fault_kind = kind;
            CHECK(invoke(controller_args) == 1 && output[0]); fputs(output, stdout);
        }
    }
    for (phase = 1; phase <= 5; phase++) {
        for (kind = 1; kind <= 7; kind++) {
            if (kind == 6 && phase != 3) continue;
            reset(); fault_at = phase; fault_kind = kind;
            CHECK(invoke(controller_args) == 1 && output[0]); fputs(output, stdout);
        }
    }
    for (i = 0; i < sizeof(control_failures) / sizeof(control_failures[0]); i++) {
        reset(); mutation = control_failures[i]; CHECK(invoke(controller_args) == 1 && output[0]); fputs(output, stdout);
    }
    reset(); close_error = EINTR; CHECK(invoke(controller_args) == 1 && output[0]); fputs(output, stdout);
}

static const uint32_t image_valid_roots[] = {
    0xd53dc, 0xd53e0, 0x115c88, 0xdfe44, 0xdfe54, 0xefe44,
    0xefe54, 0xffe44, 0xffe54, 0x10fe44, 0x10fe54,
};
static const uint32_t image_invalid_roots[] = {
    0, 1, 0xd5384, 0xd53d8, 0xd53dd, 0x115c8c, 0x116000, 0xfffffc88, UINT32_MAX,
    0xdfe48, 0xdfe4c, 0xdfe50, 0xefe48, 0xefe4c, 0xefe50,
    0xffe48, 0xffe4c, 0xffe50, 0x10fe48, 0x10fe4c, 0x10fe50,
};
static const uint32_t image_word_patterns[][4] = {
    {0, 0, 0, 0}, {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX},
    {0x80000000, 0, UINT32_MAX, 0x101}, {1, 2, 3, 2}, {0, 0, 0, 0xffffff00},
};

static void test_controller_image(void)
{
    char *invalid[][10] = {
        {"probe", "--controller-image", NULL},
        {"probe", "--controller-image", "--acknowledge-card-reset", NULL},
        {"probe", "--controller-image", "--expected-generation", "42", NULL},
        {"probe", "--controller-image", "--controller-image", "--acknowledge-card-reset", "--expected-generation", "42", NULL},
        {"probe", "--controller-image", "--info", "--acknowledge-card-reset", "--expected-generation", "42", NULL},
        {"probe", "--controller-image", "--fixed-state", "--acknowledge-card-reset", "--expected-generation", "42", NULL},
        {"probe", "--controller-image", "--controller-root", "--acknowledge-card-reset", "--expected-generation", "42", NULL},
        {"probe", "--controller-image", "--acknowledge-card-reset", "--expected-generation", "42", "--address", "0", NULL},
        {"probe", "--controller-image", "--acknowledge-card-reset", "--expected-generation", "42", "--image-width", "1", NULL},
    };
    unsigned i, stage, kind, word;
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        reset(); CHECK(invoke(invalid[i]) == 1 && !opens && !output[0]);
    }
    for (i = 0; i < 11; i++) {
        char *conflict[] = {"probe", "--controller-image", i < 5 ? all_live_args[i][1] : raw_args[i - 5][1],
                           "--acknowledge-card-reset", "--expected-generation", "42", NULL};
        reset(); CHECK(invoke(conflict) == 1 && !opens && !output[0]);
    }
    reset(); CHECK(!invoke(image_args) && image_runs == 1 && runs == 1 && !state_runs && !controller_runs);
    CHECK(strstr(output, "\"controller_image\":true") && strstr(output, "\"raw_words\":[0,0,0,0],\"owned_declaration_low8\":0"));
    CHECK(strstr(output, "\"controller_root_used_for_fixed_tuple\":true") && strstr(output, "\"tuple_values_followed\":false"));
    CHECK(strstr(output, "\"bracket_equality_excludes_aba\":false"));
    /* The old root may be anywhere; only the image sample's own bracket is
     * an admission prerequisite. Low declaration bytes never gate success. */
    for (i = 0; i < 256; i++) {
        reset(); root_values[0] = 0; root_values[1] = UINT32_MAX;
        image_words[3] = 0xabcd1200U | i;
        CHECK(!invoke(image_args) && image_runs == 1 && output[0]);
    }
    for (i = 0; i < sizeof(image_valid_roots) / sizeof(image_valid_roots[0]); i++) {
        reset(); image_roots[0] = image_roots[1] = image_valid_roots[i];
        CHECK(!invoke(image_args));
    }
    for (i = 0; i < sizeof(image_word_patterns) / sizeof(image_word_patterns[0]); i++) {
        reset(); memcpy(image_words, image_word_patterns[i], sizeof(image_words)); CHECK(!invoke(image_args));
    }
    for (stage = 0; stage < 2; stage++) {
        for (kind = 1; kind <= 6; kind++) {
            reset(); image_fault_at = stage + 1; image_fault_kind = kind;
            CHECK(invoke(image_args) == 1 && output[0] && !strstr(errors, "Invalid controller-image result"));
            CHECK(strstr(output, "\"root_before\":null,\"root_after\":null,\"raw_words\":null"));
        }
        for (i = 1; i <= 19; i++) {
            if (i == 11) continue;
            reset(); image_mutation_stage = stage; image_mutation = i;
            if (i >= 13) { image_fault_at = stage + 1; image_fault_kind = 4; }
            CHECK(invoke(image_args) == 1 && !output[0] && strstr(errors, "Invalid controller-image result"));
        }
        for (i = 0; i < sizeof(image_invalid_roots) / sizeof(image_invalid_roots[0]); i++) {
            reset(); image_mutation_stage = stage; image_mutation = 11; image_bad_root = image_invalid_roots[i];
            CHECK(invoke(image_args) == 1 && !output[0] && strstr(errors, "Invalid controller-image result"));
        }
        for (word = 0; word < 4; word++) {
            reset(); image_mutation_stage = stage; image_mutation = 15; image_word = word;
            image_fault_at = stage + 1; image_fault_kind = 2;
            CHECK(invoke(image_args) == 1 && !output[0]);
        }
        reset(); root_fault_at = stage + 1; root_fault_kind = 2;
        image_mutation_stage = stage; image_mutation = 20;
        CHECK(invoke(image_args) == 1 && !output[0]);  /* Forged image after failed prior root. */
    }
    reset(); image_fault_at = 1; image_fault_kind = 4; image_mutation_stage = 1; image_mutation = 3;
    CHECK(invoke(image_args) == 1 && !output[0]);  /* Reserved padding also zero when skipped. */
    reset(); image_fault_at = 1; image_fault_kind = 4; image_mutation_stage = 1; image_mutation = 20;
    CHECK(invoke(image_args) == 1 && !output[0]);  /* No OPEN tuple after an INIT tuple failure. */
    for (i = 21; i <= 22; i++) {
        reset(); image_mutation = i; image_fault_at = 1; image_fault_kind = 4;
        CHECK(invoke(image_args) == 1 && !output[0]);
    }
    for (i = 2; i <= 4; i++) {
        reset(); state_mutation = i; CHECK(invoke(image_args) == 1 && !output[0]);
    }
    reset(); mutation = 2; CHECK(invoke(image_args) == 1 && !output[0]);
    reset(); state_mutation = 7; CHECK(invoke(image_args) == 1 && !output[0]);
    for (i = 0; i < 2; i++) {
        reset(); run_error = i ? EINTR : ENOTTY;
        CHECK(invoke(image_args) == 1 && image_runs == 1 && !state_runs && !controller_runs && !output[0]);
        CHECK(strstr(errors, "no retry"));
    }
    reset(); metadata.selector_mask = 1; CHECK(invoke(image_args) == 1 && !runs);
    reset(); metadata.generation++; CHECK(invoke(image_args) == 1 && !runs);
    reset(); close_error = EINTR; CHECK(invoke(image_args) == 1 && output[0]);
    reset(); output_error = true; CHECK(invoke(image_args) == 1 && image_runs == 1);
    reset(); flush_error = true; CHECK(invoke(image_args) == 1 && image_runs == 1);
}

static void image_json_examples(void)
{
    unsigned i, stage, phase;
    for (i = 0; i < sizeof(image_valid_roots) / sizeof(image_valid_roots[0]); i++) {
        reset(); image_roots[0] = image_roots[1] = image_valid_roots[i];
        CHECK(!invoke(image_args)); fputs(output, stdout);
    }
    reset(); root_values[0] = root_values[1] = UINT32_MAX; CHECK(!invoke(image_args)); fputs(output, stdout);
    for (i = 0; i < sizeof(image_word_patterns) / sizeof(image_word_patterns[0]); i++) {
        reset(); memcpy(image_words, image_word_patterns[i], sizeof(image_words)); CHECK(!invoke(image_args)); fputs(output, stdout);
    }
    reset(); image_roots[1] = 0xd53e0; CHECK(!invoke(image_args)); fputs(output, stdout);
    for (stage = 1; stage <= 2; stage++) {
        for (i = 1; i <= 6; i++) {
            reset(); image_fault_at = stage; image_fault_kind = i;
            CHECK(invoke(image_args) == 1 && output[0]); fputs(output, stdout);
        }
        reset(); root_fault_at = stage; root_fault_kind = 2;
        CHECK(invoke(image_args) == 1 && output[0]); fputs(output, stdout);
    }
    for (stage = 1; stage <= 3; stage++) {
        reset(); sample_fault_at = stage; sample_fault_kind = 1;
        CHECK(invoke(image_args) == 1 && output[0]); fputs(output, stdout);
    }
    for (phase = 1; phase <= 5; phase++) {
        reset(); fault_at = phase; fault_kind = 2;
        CHECK(invoke(image_args) == 1 && output[0]); fputs(output, stdout);
    }
    reset(); mutation = 51; CHECK(invoke(image_args) == 1 && output[0]); fputs(output, stdout);
    reset(); close_error = EINTR; CHECK(invoke(image_args) == 1 && output[0]); fputs(output, stdout);
}

static const uint32_t packet_alias_neighbors[] = {
    0xdff64, 0xdff6c, 0xeff64, 0xeff6c, 0xfff64, 0xfff6c, 0x10ff64, 0x10ff6c,
};
static const uint32_t packet_alias_crossings[] = {0xdff68, 0xeff68, 0xfff68, 0x10ff68};
static const uint32_t packet_word_patterns[][3] = {
    {0, 0, 0}, {UINT32_MAX, UINT32_MAX, UINT32_MAX},
    {0x80000000, 1, 0}, {1, 2, 3}, {0xfffffff0, 0x70000, 0xffffffff},
};

static void test_controller_packet(void)
{
    char *invalid[][10] = {
        {"probe", "--controller-packet", NULL},
        {"probe", "--controller-packet", "--acknowledge-card-reset", NULL},
        {"probe", "--controller-packet", "--expected-generation", "42", NULL},
        {"probe", "--controller-packet", "--controller-packet", "--acknowledge-card-reset", "--expected-generation", "42", NULL},
        {"probe", "--controller-packet", "--acknowledge-card-reset", "--expected-generation", "42", "--address", "0", NULL},
        {"probe", "--controller-packet", "--acknowledge-card-reset", "--expected-generation", "42", "--packet-width", "1", NULL},
    };
    char **other_actions[] = {info_args, state_args, controller_args, image_args};
    unsigned i, stage, kind, word;
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        reset(); CHECK(invoke(invalid[i]) == 1 && !opens && !output[0]);
    }
    for (i = 0; i < 15; i++) {
        char *conflict[] = {"probe", "--controller-packet", i < 4 ? other_actions[i][1] :
                           i < 9 ? all_live_args[i - 4][1] : raw_args[i - 9][1],
                           "--acknowledge-card-reset", "--expected-generation", "42", NULL};
        reset(); CHECK(invoke(conflict) == 1 && !opens && !output[0]);
    }
    reset(); CHECK(!invoke(packet_args) && packet_runs == 1 && !state_runs && !controller_runs && !image_runs);
    CHECK(strstr(output, "\"controller_packet\":true") && strstr(output, "\"image_words\":[0,0,0,0],\"packet_words\":[0,0,0]"));
    CHECK(strstr(output, "\"controller_root_used_for_fixed_fields\":true") && strstr(output, "\"returned_values_followed\":false"));
    CHECK(strstr(output, "\"dma_suitability_established\":false") && strstr(output, "\"bracket_equality_excludes_aba\":false"));
    /* Earlier root/image observations need not equal this independent bracket
     * or its values. Even the declaration byte is never a semantic gate. */
    for (i = 0; i < 256; i++) {
        reset(); root_values[0] = 0; root_values[1] = UINT32_MAX;
        image_words[3] = 0xff; packet_image_words[3] = 0xabcd1200U | i;
        packet_words[0] = UINT32_MAX; packet_words[1] = 0; packet_words[2] = i;
        CHECK(!invoke(packet_args) && packet_runs == 1 && output[0]);
    }
    for (i = 0; i < sizeof(image_valid_roots) / sizeof(image_valid_roots[0]); i++) {
        reset(); packet_roots[0] = packet_roots[1] = image_valid_roots[i]; CHECK(!invoke(packet_args));
    }
    for (i = 0; i < sizeof(packet_alias_neighbors) / sizeof(packet_alias_neighbors[0]); i++) {
        reset(); packet_roots[0] = packet_roots[1] = packet_alias_neighbors[i]; CHECK(!invoke(packet_args));
    }
    for (i = 0; i < sizeof(packet_word_patterns) / sizeof(packet_word_patterns[0]); i++) {
        reset(); memcpy(packet_image_words, image_word_patterns[i], sizeof(packet_image_words));
        memcpy(packet_words, packet_word_patterns[i], sizeof(packet_words)); CHECK(!invoke(packet_args));
    }
    for (stage = 0; stage < 2; stage++) {
        for (kind = 1; kind <= 6; kind++) {
            reset(); packet_fault_at = stage + 1; packet_fault_kind = kind;
            CHECK(invoke(packet_args) == 1 && output[0] && !strstr(errors, "Invalid controller-packet result"));
            CHECK(strstr(output, "\"root_before\":null,\"root_after\":null,\"image_words\":null,\"packet_words\":null"));
        }
        for (i = 1; i <= 17; i++) {
            if (i == 10) continue;
            reset(); packet_mutation_stage = stage; packet_mutation = i;
            if (i >= 11) { packet_fault_at = stage + 1; packet_fault_kind = 4; }
            CHECK(invoke(packet_args) == 1 && !output[0] && strstr(errors, "Invalid controller-packet result"));
        }
        for (i = 0; i < sizeof(image_invalid_roots) / sizeof(image_invalid_roots[0]) +
                          sizeof(packet_alias_crossings) / sizeof(packet_alias_crossings[0]); i++) {
            reset(); packet_mutation_stage = stage; packet_mutation = 10;
            packet_bad_root = i < sizeof(image_invalid_roots) / sizeof(image_invalid_roots[0]) ?
                image_invalid_roots[i] : packet_alias_crossings[i - sizeof(image_invalid_roots) / sizeof(image_invalid_roots[0])];
            CHECK(invoke(packet_args) == 1 && !output[0] && strstr(errors, "Invalid controller-packet result"));
        }
        for (word = 0; word < 7; word++) {
            reset(); packet_mutation_stage = stage; packet_mutation = 13; packet_word = word;
            packet_fault_at = stage + 1; packet_fault_kind = 2;
            CHECK(invoke(packet_args) == 1 && !output[0]);
        }
        reset(); image_fault_at = stage + 1; image_fault_kind = 2;
        packet_mutation_stage = stage; packet_mutation = 18;
        CHECK(invoke(packet_args) == 1 && !output[0]);  /* Forged packet after prior image failure. */
        reset(); root_fault_at = stage + 1; root_fault_kind = 2;
        packet_mutation_stage = stage; packet_mutation = 18;
        CHECK(invoke(packet_args) == 1 && !output[0]);
        reset(); sample_fault_at = stage + 2; sample_fault_kind = 1;
        packet_mutation_stage = stage; packet_mutation = 18;
        CHECK(invoke(packet_args) == 1 && !output[0]);
    }
    reset(); packet_fault_at = 1; packet_fault_kind = 4; packet_mutation_stage = 1; packet_mutation = 3;
    CHECK(invoke(packet_args) == 1 && !output[0]);
    reset(); packet_fault_at = 1; packet_fault_kind = 4; packet_mutation_stage = 1; packet_mutation = 18;
    CHECK(invoke(packet_args) == 1 && !output[0]);
    for (i = 19; i <= 20; i++) {
        reset(); packet_mutation = i; packet_fault_at = 1; packet_fault_kind = 4;
        CHECK(invoke(packet_args) == 1 && !output[0]);
    }
    for (i = 2; i <= 4; i++) {
        reset(); state_mutation = i; CHECK(invoke(packet_args) == 1 && !output[0]);
    }
    reset(); mutation = 2; CHECK(invoke(packet_args) == 1 && !output[0]);
    reset(); state_mutation = 7; CHECK(invoke(packet_args) == 1 && !output[0]);
    reset(); image_mutation = 3; CHECK(invoke(packet_args) == 1 && !output[0]);
    for (i = 0; i < 2; i++) {
        reset(); run_error = i ? EINTR : ENOTTY;
        CHECK(invoke(packet_args) == 1 && packet_runs == 1 && !state_runs && !controller_runs && !image_runs && !output[0]);
        CHECK(strstr(errors, "no retry"));
    }
    reset(); metadata.selector_mask = 1; CHECK(invoke(packet_args) == 1 && !runs);
    reset(); metadata.generation++; CHECK(invoke(packet_args) == 1 && !runs);
    reset(); close_error = EINTR; CHECK(invoke(packet_args) == 1 && output[0]);
    reset(); output_error = true; CHECK(invoke(packet_args) == 1 && packet_runs == 1);
    reset(); flush_error = true; CHECK(invoke(packet_args) == 1 && packet_runs == 1);
}

static void packet_json_examples(void)
{
    unsigned i, stage, phase;
    for (i = 0; i < sizeof(image_valid_roots) / sizeof(image_valid_roots[0]); i++) {
        reset(); packet_roots[0] = packet_roots[1] = image_valid_roots[i];
        CHECK(!invoke(packet_args)); fputs(output, stdout);
    }
    for (i = 0; i < sizeof(packet_alias_neighbors) / sizeof(packet_alias_neighbors[0]); i++) {
        reset(); packet_roots[0] = packet_roots[1] = packet_alias_neighbors[i];
        CHECK(!invoke(packet_args)); fputs(output, stdout);
    }
    for (i = 0; i < sizeof(packet_word_patterns) / sizeof(packet_word_patterns[0]); i++) {
        reset(); memcpy(packet_image_words, image_word_patterns[i], sizeof(packet_image_words));
        memcpy(packet_words, packet_word_patterns[i], sizeof(packet_words));
        CHECK(!invoke(packet_args)); fputs(output, stdout);
    }
    reset(); root_values[0] = 0; root_values[1] = UINT32_MAX;
    packet_roots[1] = 0xd53e4; image_words[0] = 1; packet_image_words[0] = 2;
    CHECK(!invoke(packet_args)); fputs(output, stdout);
    for (stage = 1; stage <= 2; stage++) {
        for (i = 1; i <= 6; i++) {
            reset(); packet_fault_at = stage; packet_fault_kind = i;
            CHECK(invoke(packet_args) == 1 && output[0]); fputs(output, stdout);
        }
        reset(); image_fault_at = stage; image_fault_kind = 2;
        CHECK(invoke(packet_args) == 1 && output[0]); fputs(output, stdout);
        reset(); root_fault_at = stage; root_fault_kind = 2;
        CHECK(invoke(packet_args) == 1 && output[0]); fputs(output, stdout);
    }
    for (stage = 1; stage <= 3; stage++) {
        reset(); sample_fault_at = stage; sample_fault_kind = 1;
        CHECK(invoke(packet_args) == 1 && output[0]); fputs(output, stdout);
    }
    for (phase = 1; phase <= 5; phase++) {
        reset(); fault_at = phase; fault_kind = 2;
        CHECK(invoke(packet_args) == 1 && output[0]); fputs(output, stdout);
    }
    reset(); mutation = 51; CHECK(invoke(packet_args) == 1 && output[0]); fputs(output, stdout);
    reset(); close_error = EINTR; CHECK(invoke(packet_args) == 1 && output[0]); fputs(output, stdout);
}

static const uint32_t heap_slot_neighbors[] = {
    0xdfda8, 0xdfdb0, 0xefda8, 0xefdb0, 0xffda8, 0xffdb0, 0x10fda8, 0x10fdb0,
};
static const uint32_t heap_slot_crossings[] = {0xdfdac, 0xefdac, 0xffdac, 0x10fdac};
static const uint32_t heap_valid_bases[] = {0x117000, 0x118000, 0x120000, 0x3efc000};
static const uint32_t heap_invalid_bases[] = {
    0, 1, 0x116000, 0x116fff, 0x117001, 0x117ffc, 0x3efc001, 0x3efd000,
    0x3ffc000, 0x4000000, 0xfff00000, 0xfffffff0, UINT32_MAX,
};
static const uint32_t heap_header_patterns[][5] = {
    {0, 0, 0, 0, 0}, {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX},
    {eCMD_C011_INIT, 0, 0x12345678, 0xfffffff0, 1},
    {eCMD_C011_DEC_CHAN_OPEN, 7, 0x80000000, 0, UINT32_MAX},
    {1, 0xffffffff, 0x70000, 0x100000, 0x117000},
};
static const uint32_t heap_owned_patterns[] = {1, 0xffffff01, 0x80000001, 0x12345601};

static void test_heap_packet(void)
{
    char *invalid[][10] = {
        {"probe", "--heap-packet", NULL},
        {"probe", "--heap-packet", "--acknowledge-card-reset", NULL},
        {"probe", "--heap-packet", "--expected-generation", "42", NULL},
        {"probe", "--heap-packet", "--heap-packet", "--acknowledge-card-reset", "--expected-generation", "42", NULL},
        {"probe", "--heap-packet", "--acknowledge-card-reset", "--expected-generation", "42", "--address", "0", NULL},
        {"probe", "--heap-packet", "--acknowledge-card-reset", "--expected-generation", "42", "--packet-width", "5", NULL},
        {"probe", "--heap-packet", "--acknowledge-card-reset", "--expected-generation", "42", "--slots", NULL},
    };
    char **other_actions[] = {info_args, state_args, controller_args, image_args, packet_args};
    const uint32_t invalid_extents[] = {0, 1, 0xffffc, 0x100004, UINT32_MAX};
    unsigned i, stage, word, kind, phase;
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        reset(); CHECK(invoke(invalid[i]) == 1 && !opens && !output[0]);
    }
    for (i = 0; i < 16; i++) {
        char *conflict[] = {"probe", "--heap-packet", i < 5 ? other_actions[i][1] :
                           i < 10 ? all_live_args[i - 5][1] : raw_args[i - 10][1],
                           "--acknowledge-card-reset", "--expected-generation", "42", NULL};
        reset(); CHECK(invoke(conflict) == 1 && !opens && !output[0]);
    }
    reset(); CHECK(!invoke(heap_args) && heap_runs == 1 && !state_runs && !controller_runs && !image_runs && !packet_runs);
    CHECK(strstr(output, "\"heap_packet\":true") && strstr(output, "\"header_words\":[0,0,0,0,0]"));
    CHECK(strstr(output, "\"image_base_used_for_computed_admitted_fixed_span\":true") &&
          strstr(output, "\"packet_declarations_used_as_equality_gates\":true"));
    CHECK(strstr(output, "\"header_values_followed\":false") && strstr(output, "\"stored_reply_values_followed\":false"));
    CHECK(!strstr(output, "\"returned_values_followed\""));
    CHECK(strstr(output, "\"freshness_established\":false") && strstr(output, "\"queue_validity_established\":false") &&
          strstr(output, "\"independent_fetch_errors_certified\":false"));
    for (i = 0; i < sizeof(image_valid_roots) / sizeof(image_valid_roots[0]); i++) {
        reset(); heap_roots[0] = heap_roots[1] = image_valid_roots[i]; CHECK(!invoke(heap_args));
    }
    for (i = 0; i < sizeof(packet_alias_neighbors) / sizeof(packet_alias_neighbors[0]); i++) {
        reset(); heap_roots[0] = heap_roots[1] = packet_alias_neighbors[i]; CHECK(!invoke(heap_args));
    }
    for (i = 0; i < sizeof(heap_slot_neighbors) / sizeof(heap_slot_neighbors[0]); i++) {
        reset(); heap_roots[0] = heap_roots[1] = heap_slot_neighbors[i]; CHECK(!invoke(heap_args));
    }
    for (i = 0; i < sizeof(heap_valid_bases) / sizeof(heap_valid_bases[0]); i++) {
        reset(); heap_bases[0] = heap_bases[1] = heap_valid_bases[i]; CHECK(!invoke(heap_args));
    }
    for (i = 0; i < sizeof(heap_header_patterns) / sizeof(heap_header_patterns[0]); i++) {
        reset(); memcpy(heap_header, heap_header_patterns[i], sizeof(heap_header));
        heap_slots[0] = heap_header[4]; heap_slots[1] = heap_header[3];
        CHECK(!invoke(heap_args));
    }
    /* Embedded observations do not admit, or constrain, the fresh heap stage. */
    reset(); root_values[0] = 0; root_values[1] = UINT32_MAX;
    image_words[0] = UINT32_MAX; image_words[1] = 1; image_words[2] = 0; image_words[3] = 0xff;
    heap_roots[1] = 0xd53e4; heap_bases[1] = 0x3efc000;
    CHECK(!invoke(heap_args));
    for (i = 0; i < sizeof(heap_owned_patterns) / sizeof(heap_owned_patterns[0]); i++) {
        reset(); heap_owned = heap_owned_patterns[i]; CHECK(!invoke(heap_args));
    }
    for (stage = 0; stage < 2; stage++) {
        for (kind = 1; kind <= 6; kind++) {
            reset(); heap_fault_at = stage + 1; heap_fault_kind = kind;
            CHECK(invoke(heap_args) == 1 && output[0] && !strstr(errors, "Invalid heap-packet result"));
            CHECK(strstr(output, "\"packet_address\":null,\"header_words\":null,\"slots_before\":null,\"slots_after\":null"));
        }
        for (i = 1; i <= 9; i++) {
            reset(); heap_stage = stage; heap_mutation = i;
            CHECK(invoke(heap_args) == 1 && !output[0] && strstr(errors, "Invalid heap-packet result"));
        }
        for (i = 0; i < sizeof(image_invalid_roots) / sizeof(image_invalid_roots[0]) +
                          sizeof(packet_alias_crossings) / sizeof(packet_alias_crossings[0]) +
                          sizeof(heap_slot_crossings) / sizeof(heap_slot_crossings[0]); i++) {
            unsigned image_count = sizeof(image_invalid_roots) / sizeof(image_invalid_roots[0]);
            unsigned alias_count = sizeof(packet_alias_crossings) / sizeof(packet_alias_crossings[0]);
            reset(); heap_stage = stage; heap_mutation = 10;
            heap_bad_root = i < image_count ? image_invalid_roots[i] :
                i < image_count + alias_count ? packet_alias_crossings[i - image_count] :
                heap_slot_crossings[i - image_count - alias_count];
            CHECK(invoke(heap_args) == 1 && !output[0]);
        }
        for (kind = 1; kind <= 6; kind++)
            for (word = 0; word < 26; word++) {
                reset(); heap_fault_at = stage + 1; heap_fault_kind = kind;
                heap_mutation = 11; heap_stage = stage; heap_word = word;
                CHECK(invoke(heap_args) == 1 && !output[0]);
            }
        for (i = 12; i <= 15; i++) {
            reset(); heap_fault_at = stage + 1; heap_fault_kind = 4; heap_mutation = i; heap_stage = stage;
            CHECK(invoke(heap_args) == 1 && !output[0]);
        }
        for (i = 17; i <= 18; i++) {
            reset(); heap_fault_at = stage + 1; heap_fault_kind = 4; heap_mutation = i; heap_stage = stage;
            CHECK(invoke(heap_args) == 1 && !output[0]);
        }
        reset(); heap_stage = stage; heap_mutation = 20; CHECK(invoke(heap_args) == 1 && !output[0]);
        for (i = 0; i < sizeof(heap_invalid_bases) / sizeof(heap_invalid_bases[0]); i++) {
            reset(); heap_stage = stage; heap_mutation = 21; heap_bad_value = heap_invalid_bases[i];
            CHECK(invoke(heap_args) == 1 && !output[0]);
        }
        for (i = 0; i < sizeof(invalid_extents) / sizeof(invalid_extents[0]); i++) {
            reset(); heap_stage = stage; heap_mutation = 22; heap_bad_value = invalid_extents[i];
            CHECK(invoke(heap_args) == 1 && !output[0]);
        }
        for (i = 0; i < 256; i++) {
            reset(); heap_stage = stage; heap_mutation = 23; heap_bad_value = 0xabcd1200U | i;
            CHECK(invoke(heap_args) == (i != 1)); CHECK((output[0] != 0) == (i == 1));
        }
        for (word = 0; word < 3; word++) {
            reset(); heap_stage = stage; heap_mutation = 24; heap_word = word;
            CHECK(invoke(heap_args) == 1 && !output[0]);
        }
        reset(); heap_stage = stage; heap_mutation = 25; CHECK(invoke(heap_args) == 1 && !output[0]);
        for (i = 26; i <= 28; i++)
            for (word = 0; word < (i == 26 ? 4U : i == 27 ? 3U : 2U); word++) {
                reset(); heap_stage = stage; heap_mutation = i; heap_word = word;
                CHECK(invoke(heap_args) == 1 && !output[0]);
            }
        reset(); heap_stage = stage; heap_mutation = 30; CHECK(invoke(heap_args) == 1 && !output[0]);
        for (i = 0; i < 3; i++) {
            reset(); heap_stage = stage; heap_mutation = 16;
            if (!i) { image_fault_at = stage + 1; image_fault_kind = 2; }
            else if (i == 1) { root_fault_at = stage + 1; root_fault_kind = 2; }
            else { sample_fault_at = stage + 2; sample_fault_kind = 1; }
            CHECK(invoke(heap_args) == 1 && !output[0]);
        }
        reset(); heap_fault_at = stage + 1; heap_fault_kind = 4; mutation = 52;
        CHECK(invoke(heap_args) == 1 && output[0] && !strstr(errors, "Invalid heap-packet result"));
        CHECK(strstr(output, "\"status\":-116")); /* Original ESTALE wins over failed cleanup. */
    }
    reset(); heap_fault_at = 1; heap_fault_kind = 4; heap_stage = 1; heap_mutation = 16;
    CHECK(invoke(heap_args) == 1 && !output[0]);
    for (i = 0; i < 2; i++) {
        reset(); run_error = i ? EINTR : ENOTTY;
        CHECK(invoke(heap_args) == 1 && heap_runs == 1 && !state_runs && !controller_runs && !image_runs && !packet_runs && !output[0]);
        CHECK(strstr(errors, "no retry"));
    }
    for (phase = 1; phase <= 5; phase++) {
        reset(); fault_at = phase; fault_kind = 2;
        CHECK(invoke(heap_args) == 1 && output[0] && !strstr(errors, "Invalid heap-packet result"));
    }
    reset(); mutation = 2; CHECK(invoke(heap_args) == 1 && !output[0]);
    reset(); state_mutation = 7; CHECK(invoke(heap_args) == 1 && !output[0]);
    reset(); image_mutation = 3; CHECK(invoke(heap_args) == 1 && !output[0]);
    reset(); metadata.selector_mask = 1; CHECK(invoke(heap_args) == 1 && !runs);
    reset(); metadata.generation++; CHECK(invoke(heap_args) == 1 && !runs);
    reset(); open_error = ENOENT; CHECK(invoke(heap_args) == 1 && !runs);
    reset(); stat_error = EIO; CHECK(invoke(heap_args) == 1 && !runs);
    reset(); info_error = ENOTTY; CHECK(invoke(heap_args) == 1 && !runs);
    reset(); character = false; CHECK(invoke(heap_args) == 1 && !runs);
    reset(); close_error = EINTR; CHECK(invoke(heap_args) == 1 && output[0]);
    reset(); output_error = true; CHECK(invoke(heap_args) == 1 && heap_runs == 1);
    reset(); flush_error = true; CHECK(invoke(heap_args) == 1 && heap_runs == 1);
}

static void test_clock_state(void)
{
    char *invalid[][10] = {
        {"probe", "--clock-state", NULL},
        {"probe", "--clock-state", "--acknowledge-card-reset", NULL},
        {"probe", "--clock-state", "--expected-generation", "42", NULL},
        {"probe", "--clock-state", "--clock-state", "--acknowledge-card-reset", "--expected-generation", "42", NULL},
        {"probe", "--clock-state", "--acknowledge-card-reset", "--expected-generation", "42", "--address", "0", NULL},
        {"probe", "--clock-state", "--acknowledge-card-reset", "--expected-generation", "42", "--register", "0x70004", NULL},
        {"probe", "--clock-state=1", "--acknowledge-card-reset", "--expected-generation", "42", NULL},
    };
    char **other_actions[] = {info_args, state_args, controller_args, image_args, packet_args, heap_args};
    static const unsigned forged_control[] = {1, 2, 3, 4, 14, 15, 16, 17, 18, 19, 20,
        37, 38, 39, 40, 44, 45, 47, 54, 56, 57};
    unsigned i, stage, kind, phase, word;
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        reset(); CHECK(invoke(invalid[i]) == 1 && !opens && !output[0]);
    }
    for (i = 0; i < 17; i++) {
        char *conflict[] = {"probe", "--clock-state", i < 6 ? other_actions[i][1] :
            i < 11 ? all_live_args[i - 6][1] : raw_args[i - 11][1],
            "--acknowledge-card-reset", "--expected-generation", "42", NULL};
        reset(); CHECK(invoke(conflict) == 1 && !opens && !output[0]);
    }
    for (i = 0; i < 3; i++) {
        reset();
        for (stage = 0; stage < 2; stage++) for (word = 0; word < 3; word++)
            clock_values[stage][word] = i == 1 ? UINT32_MAX : i == 2 ? 0x12345678U + stage * 3 + word : 0;
        CHECK(!invoke(clock_args) && clock_runs == 1 && !state_runs && !controller_runs && !image_runs && !packet_runs && !heap_runs);
        CHECK(strstr(output, "\"clock_state\":true") && strstr(output, "\"register_addresses\":[5251584,5251740,458756]"));
        CHECK(strstr(output, "\"sample_clock_reset_writes\":false") && strstr(output, "\"raw_values_only\":true"));
        CHECK(strstr(output, "\"atomic_coherence_established\":false") && strstr(output, "\"independent_fetch_errors_certified\":false"));
        CHECK(strstr(output, "\"reset_ctrl\":0") || strstr(output, "\"reset_ctrl\":4294967295") || strstr(output, "\"reset_ctrl\":305419896"));
    }
    for (stage = 0; stage < 2; stage++) {
        for (i = 1; i <= 9; i++) {
            reset(); clock_stage = stage; clock_mutation = i;
            CHECK(invoke(clock_args) == 1 && !output[0]);
        }
        for (kind = 1; kind <= 6; kind++) {
            reset(); clock_fault_at = stage + 1; clock_fault_kind = kind;
            CHECK(invoke(clock_args) == 1 && output[0] && !strstr(errors, "Invalid clock-state result"));
            CHECK(strstr(output, "\"reset_ctrl\":null,\"perst_clock_ctrl\":null,\"clk_pm_ctrl\":null"));
        }
        for (i = 10; i <= 15; i++) {
            unsigned words = i == 11 ? 3 : 1;
            for (word = 0; word < words; word++) {
                reset(); clock_stage = stage; clock_fault_at = stage + 1; clock_fault_kind = 1;
                clock_mutation = i; clock_word = word;
                CHECK(invoke(clock_args) == 1 && !output[0]);
            }
        }
        reset(); clock_stage = stage; sample_fault_at = stage + 2; sample_fault_kind = 1; clock_mutation = 12;
        CHECK(invoke(clock_args) == 1 && !output[0]);
        reset(); clock_fault_at = 1; clock_fault_kind = 1; clock_stage = 1; clock_mutation = 12;
        CHECK(invoke(clock_args) == 1 && !output[0]);
    }
    for (i = 16; i <= 24; i++) {
        reset(); clock_mutation = i; CHECK(invoke(clock_args) == 1 && !output[0]);
    }
    for (i = 0; i < sizeof(forged_control) / sizeof(forged_control[0]); i++) {
        reset(); mutation = forged_control[i]; CHECK(invoke(clock_args) == 1 && !output[0]);
    }
    for (i = 1; i <= 40; i++) {
        if (i == 39 || i == 40) continue; /* Valid failed-control evidence. */
        reset(); state_mutation = i;
        if (i == 30 || i == 31) { sample_fault_at = 2; sample_fault_kind = 1; }
        CHECK(invoke(clock_args) == 1 && !output[0]);
    }
    for (phase = 1; phase <= 5; phase++) for (kind = 1; kind <= 7; kind++) {
        if (kind == 6 && phase != 3) continue;
        reset(); fault_at = phase; fault_kind = kind;
        CHECK(invoke(clock_args) == 1 && output[0] && !strstr(errors, "Invalid clock-state result"));
    }
    for (phase = 1; phase <= 3; phase++) {
        reset(); sample_fault_at = phase; sample_fault_kind = 9;
        CHECK(invoke(clock_args) == 1 && output[0]);
    }
    for (i = 0; i < 12; i++) {
        reset();
        if (i == 0) metadata.selector_mask = 1;
        if (i == 1) metadata.generation++;
        if (i == 2) open_error = ENOENT;
        if (i == 3) stat_error = EIO;
        if (i == 4) info_error = ENOTTY;
        if (i == 5) character = false;
        if (i == 6) run_error = ENOTTY;
        if (i == 7) run_error = EINTR;
        if (i == 8) close_error = EINTR;
        if (i == 9) output_error = true;
        if (i == 10) flush_error = true;
        if (i == 11) metadata.reserved[0] = 1;
        CHECK(invoke(clock_args) == 1);
        if (i < 6 || i == 11) CHECK(!runs && !output[0]);
        if (i == 6 || i == 7) CHECK(clock_runs == 1 && !output[0] && strstr(errors, "no retry"));
        if (i >= 8 && i <= 10) CHECK(clock_runs == 1 && output[0]);
    }
}

static void test_uart_state(void)
{
    char *invalid[][10] = {
        {"probe", "--uart-state", NULL},
        {"probe", "--uart-state", "--acknowledge-card-reset", NULL},
        {"probe", "--uart-state", "--expected-generation", "42", NULL},
        {"probe", "--uart-state", "--uart-state", "--acknowledge-card-reset", "--expected-generation", "42", NULL},
        {"probe", "--uart-state", "--acknowledge-card-reset", "--expected-generation", "42", "--address", "0", NULL},
        {"probe", "--uart-state", "--acknowledge-card-reset", "--expected-generation", "42", "--register", "0xf3004", NULL},
        {"probe", "--uart-state", "--acknowledge-card-reset", "--expected-generation", "42", "--write", "0", NULL},
        {"probe", "--uart-state", "--acknowledge-card-reset", "--expected-generation", "42", "--start", NULL},
        {"probe", "--uart-state=1", "--acknowledge-card-reset", "--expected-generation", "42", NULL},
        {"probe", "--uart-state", "--acknowledge-card-reset", "--expected-generation", "0", NULL},
        {"probe", "--uart-state", "--acknowledge-card-reset", "--expected-generation", "0x2a", NULL},
        {"probe", "--uart-state", "--acknowledge-card-reset", "--expected-generation", "42", "--acknowledge-card-reset", NULL},
        {"probe", "--uart-state", "--acknowledge-card-reset", "--expected-generation", "42", "--expected-generation", "42", NULL},
    };
    char **other_actions[] = {info_args, state_args, controller_args, image_args, packet_args, heap_args, clock_args};
    static const unsigned forged_control[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
        14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29,
        37, 38, 39, 40, 42, 44, 45, 47, 50, 54, 55, 56, 57, 58};
    static const unsigned stopped_control[] = {30, 31, 32, 33, 34, 35, 41, 43, 51, 52};
    unsigned i, stage, kind, phase, word, bit;
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        reset(); CHECK(invoke(invalid[i]) == 1 && !opens && !output[0]);
    }
    for (i = 0; i < 18; i++) {
        char *conflict[] = {"probe", "--uart-state", i < 7 ? other_actions[i][1] :
            i < 12 ? all_live_args[i - 7][1] : raw_args[i - 12][1],
            "--acknowledge-card-reset", "--expected-generation", "42", NULL};
        reset(); CHECK(invoke(conflict) == 1 && !opens && !output[0]);
    }
    for (i = 0; i < 3; i++) {
        reset();
        for (stage = 0; stage < 2; stage++) for (word = 0; word < 3; word++)
            uart_values[stage][word] = i == 1 ? UINT32_MAX : i == 2 ? 0x12345678U + stage * 3 + word : 0;
        CHECK(!invoke(uart_args) && uart_runs == 1 && !state_runs && !controller_runs && !image_runs && !packet_runs && !heap_runs && !clock_runs);
        CHECK(strstr(output, "\"uart_state\":true") && strstr(output, "\"register_addresses\":[995332,4210944,4211228]"));
        CHECK(strstr(output, "\"sample_status_fifo_reads\":false") && strstr(output, "\"sample_uart_writes\":false"));
        CHECK(strstr(output, "\"raw_values_only\":true") && strstr(output, "\"board_pads_proven\":false"));
        CHECK(strstr(output, "\"voltage_proven\":false") && strstr(output, "\"measured_baud_proven\":false"));
        CHECK(strstr(output, "\"console_availability_proven\":false"));
        CHECK(strstr(output, "\"arm_uart_ctl\":0") || strstr(output, "\"arm_uart_ctl\":4294967295") || strstr(output, "\"arm_uart_ctl\":305419896"));
        CHECK(!strstr(output, "\"clock_state\":") && !strstr(output, "\"clock_samples\":"));
    }
    /* No UART bit, route or divider encoding is an admission criterion. */
    for (stage = 0; stage < 2; stage++) for (word = 0; word < 3; word++) for (bit = 0; bit < 32; bit++) {
        reset(); uart_values[stage][word] = UINT32_C(1) << bit;
        CHECK(!invoke(uart_args) && uart_runs == 1);
    }
    for (stage = 0; stage < 2; stage++) {
        for (i = 1; i <= 9; i++) {
            reset(); uart_stage = stage; uart_mutation = i;
            CHECK(invoke(uart_args) == 1 && !output[0]);
        }
        for (kind = 1; kind <= 6; kind++) {
            reset(); uart_fault_at = stage + 1; uart_fault_kind = kind;
            CHECK(invoke(uart_args) == 1 && output[0] && !strstr(errors, "Invalid uart-state result"));
            CHECK(strstr(output, "\"arm_uart_ctl\":null,\"pin_mux_ctrl_0\":null,\"uart_router_sel\":null"));
        }
        for (i = 10; i <= 15; i++) {
            unsigned words = i == 11 ? 3 : 1;
            for (word = 0; word < words; word++) {
                reset(); uart_stage = stage; uart_fault_at = stage + 1; uart_fault_kind = 1;
                uart_mutation = i; uart_word = word;
                CHECK(invoke(uart_args) == 1 && !output[0]);
            }
        }
        for (word = 0; word < 3; word++) {
            reset(); uart_stage = stage; uart_fault_at = stage + 1; uart_fault_kind = 2;
            uart_mutation = 11; uart_word = word;
            CHECK(invoke(uart_args) == 1 && !output[0]);
            reset(); uart_stage = stage; fault_at = 1; fault_kind = 2;
            uart_mutation = 11; uart_word = word;
            CHECK(invoke(uart_args) == 1 && !output[0]);
        }
        reset(); uart_stage = stage; sample_fault_at = stage + 2; sample_fault_kind = 1; uart_mutation = 12;
        CHECK(invoke(uart_args) == 1 && !output[0]);
        reset(); uart_fault_at = 1; uart_fault_kind = 1; uart_stage = 1; uart_mutation = 12;
        CHECK(invoke(uart_args) == 1 && !output[0]);
    }
    for (i = 16; i <= 26; i++) {
        reset(); uart_mutation = i; CHECK(invoke(uart_args) == 1 && !output[0]);
    }
    for (i = 0; i < sizeof(forged_control) / sizeof(forged_control[0]); i++) {
        reset(); mutation = forged_control[i]; CHECK(invoke(uart_args) == 1 && !output[0]);
    }
    for (i = 1; i <= 40; i++) {
        if (i == 39 || i == 40) continue;
        reset(); state_mutation = i;
        if (i == 30 || i == 31) { sample_fault_at = 2; sample_fault_kind = 1; }
        CHECK(invoke(uart_args) == 1 && !output[0]);
    }
    for (phase = 1; phase <= 5; phase++) for (kind = 1; kind <= 7; kind++) {
        if (kind == 6 && phase != 3) continue;
        reset(); fault_at = phase; fault_kind = kind;
        CHECK(invoke(uart_args) == 1 && output[0] && !strstr(errors, "Invalid uart-state result"));
    }
    for (phase = 1; phase <= 3; phase++) for (kind = 1; kind <= 9; kind++) {
        if (phase == 1 && kind >= 4 && kind <= 6) continue;
        reset(); sample_fault_at = phase; sample_fault_kind = kind;
        CHECK(invoke(uart_args) == 1 && output[0] && !strstr(errors, "Invalid uart-state result"));
    }
    for (i = 0; i < sizeof(stopped_control) / sizeof(stopped_control[0]); i++) {
        reset(); mutation = stopped_control[i];
        CHECK(invoke(uart_args) == 1 && output[0] && !strstr(errors, "Invalid uart-state result"));
    }
    for (i = 0; i < 18; i++) {
        reset();
        if (i == 0) metadata.selector_mask = 1;
        if (i == 1) metadata.generation++;
        if (i == 2) open_error = ENOENT;
        if (i == 3) stat_error = EIO;
        if (i == 4) info_error = ENOTTY;
        if (i == 5) character = false;
        if (i == 6) run_error = ENOTTY;
        if (i == 7) run_error = EINTR;
        if (i == 8) close_error = EINTR;
        if (i == 9) output_error = true;
        if (i == 10) flush_error = true;
        if (i == 11) metadata.reserved[0] = 1;
        if (i == 12) metadata.version++;
        if (i == 13) metadata.size--;
        if (i == 14) metadata.generation = 0;
        if (i == 15) metadata.selector_mask = 1U << 31;
        if (i == 16) metadata.firmware_sha256[0] ^= 1;
        if (i == 17) metadata.reserved[2] = 1;
        CHECK(invoke(uart_args) == 1);
        if (i < 6 || i >= 11) CHECK(!runs && !output[0]);
        if (i == 6 || i == 7) CHECK(uart_runs == 1 && !output[0] && strstr(errors, "no retry"));
        if (i >= 8 && i <= 10) CHECK(uart_runs == 1 && output[0]);
    }
}

static void uart_json_examples(void)
{
    unsigned value, stage, kind, phase, word;
    for (value = 0; value < 3; value++) {
        reset();
        for (stage = 0; stage < 2; stage++) for (word = 0; word < 3; word++)
            uart_values[stage][word] = value == 1 ? UINT32_MAX : value == 2 ? 0x12345678U + stage * 3 + word : 0;
        CHECK(!invoke(uart_args)); fputs(output, stdout);
    }
    for (stage = 1; stage <= 2; stage++) for (kind = 1; kind <= 6; kind++) {
        reset(); uart_fault_at = stage; uart_fault_kind = kind;
        CHECK(invoke(uart_args) == 1 && output[0]); fputs(output, stdout);
    }
    for (stage = 1; stage <= 3; stage++) for (kind = 1; kind <= 9; kind += 8) {
        reset(); sample_fault_at = stage; sample_fault_kind = kind;
        CHECK(invoke(uart_args) == 1 && output[0]); fputs(output, stdout);
    }
    for (phase = 1; phase <= 5; phase++) {
        reset(); fault_at = phase; fault_kind = 2;
        CHECK(invoke(uart_args) == 1 && output[0]); fputs(output, stdout);
    }
    for (value = 51; value <= 52; value++) {
        reset(); mutation = value; CHECK(invoke(uart_args) == 1 && output[0]); fputs(output, stdout);
    }
    for (stage = 1; stage <= 2; stage++) {
        reset(); uart_fault_at = stage; uart_fault_kind = 1; mutation = 52;
        CHECK(invoke(uart_args) == 1 && output[0]); fputs(output, stdout);
    }
    reset(); close_error = EINTR; CHECK(invoke(uart_args) == 1 && output[0]); fputs(output, stdout);
}

static void clock_json_examples(void)
{
    unsigned value, stage, kind, phase, word;
    for (value = 0; value < 3; value++) {
        reset();
        for (stage = 0; stage < 2; stage++) for (word = 0; word < 3; word++)
            clock_values[stage][word] = value == 1 ? UINT32_MAX : value == 2 ? 0x12345678U + stage * 3 + word : 0;
        CHECK(!invoke(clock_args)); fputs(output, stdout);
    }
    for (stage = 1; stage <= 2; stage++) for (kind = 1; kind <= 6; kind++) {
        reset(); clock_fault_at = stage; clock_fault_kind = kind;
        CHECK(invoke(clock_args) == 1 && output[0]); fputs(output, stdout);
    }
    for (stage = 1; stage <= 3; stage++) for (kind = 1; kind <= 9; kind += 8) {
        reset(); sample_fault_at = stage; sample_fault_kind = kind;
        CHECK(invoke(clock_args) == 1 && output[0]); fputs(output, stdout);
    }
    for (phase = 1; phase <= 5; phase++) {
        reset(); fault_at = phase; fault_kind = 2;
        CHECK(invoke(clock_args) == 1 && output[0]); fputs(output, stdout);
    }
    for (value = 51; value <= 52; value++) {
        reset(); mutation = value; CHECK(invoke(clock_args) == 1 && output[0]); fputs(output, stdout);
    }
    for (stage = 1; stage <= 2; stage++) {
        reset(); clock_fault_at = stage; clock_fault_kind = 1; mutation = 52;
        CHECK(invoke(clock_args) == 1 && output[0]); fputs(output, stdout);
    }
    reset(); close_error = EINTR; CHECK(invoke(clock_args) == 1 && output[0]); fputs(output, stdout);
}

static void heap_packet_json_examples(void)
{
    unsigned i, stage, phase;
    for (i = 0; i < sizeof(image_valid_roots) / sizeof(image_valid_roots[0]); i++) {
        reset(); heap_roots[0] = heap_roots[1] = image_valid_roots[i]; CHECK(!invoke(heap_args)); fputs(output, stdout);
    }
    for (i = 0; i < sizeof(heap_slot_neighbors) / sizeof(heap_slot_neighbors[0]); i++) {
        reset(); heap_roots[0] = heap_roots[1] = heap_slot_neighbors[i]; CHECK(!invoke(heap_args)); fputs(output, stdout);
    }
    for (i = 0; i < sizeof(heap_valid_bases) / sizeof(heap_valid_bases[0]); i++) {
        reset(); heap_bases[0] = heap_bases[1] = heap_valid_bases[i]; CHECK(!invoke(heap_args)); fputs(output, stdout);
    }
    for (i = 0; i < sizeof(heap_header_patterns) / sizeof(heap_header_patterns[0]); i++) {
        reset(); memcpy(heap_header, heap_header_patterns[i], sizeof(heap_header));
        heap_slots[0] = heap_header[4]; heap_slots[1] = heap_header[3]; CHECK(!invoke(heap_args)); fputs(output, stdout);
    }
    for (i = 0; i < sizeof(heap_owned_patterns) / sizeof(heap_owned_patterns[0]); i++) {
        reset(); heap_owned = heap_owned_patterns[i]; CHECK(!invoke(heap_args)); fputs(output, stdout);
    }
    reset(); root_values[0] = 0; root_values[1] = UINT32_MAX; image_words[0] = UINT32_MAX;
    heap_roots[1] = 0xd53e4; heap_bases[1] = 0x3efc000; CHECK(!invoke(heap_args)); fputs(output, stdout);
    for (stage = 1; stage <= 2; stage++) {
        for (i = 1; i <= 6; i++) {
            reset(); heap_fault_at = stage; heap_fault_kind = i;
            CHECK(invoke(heap_args) == 1 && output[0]); fputs(output, stdout);
        }
        reset(); image_fault_at = stage; image_fault_kind = 2; CHECK(invoke(heap_args) == 1 && output[0]); fputs(output, stdout);
        reset(); root_fault_at = stage; root_fault_kind = 2; CHECK(invoke(heap_args) == 1 && output[0]); fputs(output, stdout);
    }
    for (stage = 1; stage <= 3; stage++) {
        reset(); sample_fault_at = stage; sample_fault_kind = 1; CHECK(invoke(heap_args) == 1 && output[0]); fputs(output, stdout);
    }
    for (phase = 1; phase <= 5; phase++) {
        reset(); fault_at = phase; fault_kind = 2; CHECK(invoke(heap_args) == 1 && output[0]); fputs(output, stdout);
    }
    for (i = 51; i <= 52; i++) {
        reset(); mutation = i; CHECK(invoke(heap_args) == 1 && output[0]); fputs(output, stdout);
    }
    reset(); close_error = EINTR; CHECK(invoke(heap_args) == 1 && output[0]); fputs(output, stdout);
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--uart-json-examples")) {
        uart_json_examples(); return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--clock-json-examples")) {
        clock_json_examples(); return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--heap-packet-json-examples")) {
        heap_packet_json_examples(); return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--packet-json-examples")) {
        packet_json_examples(); return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--image-json-examples")) {
        image_json_examples(); return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--controller-json-examples")) {
        controller_json_examples(); return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--state-json-examples")) {
        state_json_examples(); return 0;
    }
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
        for (action = 0; action < sizeof(raw_args) / sizeof(raw_args[0]); action++) {
            for (pattern = 0; pattern <= 2; pattern++) {
                reset(); response_pattern = pattern;
                CHECK(!invoke(raw_args[action])); fputs(output, stdout);
            }
            for (phase = 1; phase <= 3; phase++) {
                for (kind = 1; kind <= 7; kind++) {
                    if (kind == 6) continue;
                    reset(); response_pattern = 2; fault_at = phase; fault_kind = kind;
                    CHECK(invoke(raw_args[action]) == 1); fputs(output, stdout);
                }
            }
            for (failure = 51; failure <= 52; failure++) {
                reset(); response_pattern = 2; mutation = failure;
                CHECK(invoke(raw_args[action]) == 1); fputs(output, stdout);
            }
            for (failure = 59; failure <= 61; failure++) {
                if (failure == 60) continue;
                reset(); response_pattern = 2; mutation = failure;
                CHECK(invoke(raw_args[action]) == 1); fputs(output, stdout);
            }
            reset(); response_pattern = 2; mutation = 67;
            CHECK(!invoke(raw_args[action])); fputs(output, stdout);
        }
        return 0;
    }
    CHECK(argc == 1);
    test_arguments(); test_metadata(); test_errors(); test_results(); test_named_controls(); test_decoding(); test_raw_commands(); test_fixed_state(); test_controller_root(); test_controller_image(); test_controller_packet(); test_heap_packet(); test_clock_state(); test_uart_state();
    printf("Firmware probe CLI: %u checks passed\n", checks);
    return 0;
}
