#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
probe_test_dir=$(mktemp -d "${TMPDIR:-/tmp}/crystalhd-fw-probe-check.XXXXXX")
cleanup()
{
    rm -f "$probe_test_dir/check" "$probe_test_dir/probe-functions.h" \
        "$probe_test_dir/device-functions.h" "$probe_test_dir/module-functions.h" \
        "$probe_test_dir/status-functions.h" "$probe_test_dir/cli.o" "$probe_test_dir/cli-check"
    rmdir "$probe_test_dir"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

probe_linkage_fuse()
{
    probe_symbols=$(nm -u "$1")
    printf '%s\n' "$probe_symbols" | awk -v check_output="$2" '
        { symbol = $NF; sub(/@.*/, "", symbol) }
        symbol ~ /^(__)?(open|open64|openat|openat64|fstat|fstat64|fxstat|fxstat64|xstat|xstat64|ioctl|close)(_|$)/ ||
        symbol == "syscall" ||
        (check_output && symbol ~ /^(printf|__printf_chk|fputs|puts|putchar|putc|ferror|fflush|perror)$/) {
            print "CLI callback/linkage safety fuse: " symbol > "/dev/stderr"; bad = 1
        }
        END { exit bad }
    '
}

# Compile the actual implementation, not a second implementation of the probe.
awk '/^static const u8 crystalhd_fw_research_owner;/ { copying = 1 }
     copying { print }
     END { if (!copying) exit 1 }' \
    "$repo_dir/driver/linux/crystalhd_fw_research.c" > "$probe_test_dir/probe-functions.h"
awk '
    /^int crystalhd_(fw_research_generation|device_enter)\(/ ||
    /^void crystalhd_device_exit\(/ { copying = 1; found++ }
    copying { print }
    copying && /^}/ { copying = 0 }
    END { if (found != 3 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_lnx.c" > "$probe_test_dir/device-functions.h"
awk '
    /^static (int __init chd_dec_module_init|void __exit chd_dec_module_cleanup)\(/ {
        copying = 1; found++
    }
    copying { print }
    copying && /^}/ { copying = 0 }
    END { if (found != 2 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_lnx.c" > "$probe_test_dir/module-functions.h"
awk '
    /^int crystalhd_status_to_errno\(/ { copying = 1; found++ }
    copying { print }
    copying && /^}/ { copying = 0 }
    END { if (found != 1 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_cmds.c" > "$probe_test_dir/status-functions.h"

# The kernel hash implementation is mocked below, but the pin must match the
# shipped blob exactly. Also evaluate Kbuild with the default and opt-in flags.
"${PYTHON3:-python3}" -B - "$repo_dir" <<'PY'
import hashlib
import os
import pathlib
import platform
import re
import shlex
import subprocess
import sys

root = pathlib.Path(sys.argv[1])
make_env = dict(os.environ)
for key in ('MAKEFLAGS', 'MFLAGS', 'MAKEOVERRIDES', 'CRYSTALHD_FW_RESEARCH', 'CONFIG_CRYPTO_HASH'):
    make_env.pop(key, None)
source = (root / 'driver/linux/crystalhd_fw_research.c').read_text()
uapi = (root / 'include/crystalhd_fw_research.h').read_text()
pin = re.search(r'crystalhd_fw_research_sha256\[SHA256_DIGEST_SIZE\] = \{(.*?)\};',
                source, re.S).group(1)
digest = bytes(int(value, 16) for value in re.findall(r'0x([0-9a-fA-F]{2})', pin))
assert digest == hashlib.sha256((root / 'firmware/fwbin/70015/bcm70015fw.bin').read_bytes()).digest()
assert digest.hex() == re.search(r'CRYSTALHD_FW_RESEARCH_FIRMWARE_SHA256\s+\\\s*"([0-9a-f]+)"', uapi).group(1)
legacy = (root / 'include/7411d.h').read_text()
wire_header = (root / 'driver/linux/crystalhd_fw_if.h').read_text()
cli = (root / 'tools/fw-research/flea_fw_probe.c').read_text()
for name, value in (('H261', 2), ('H263', 3), ('MPEG1', 5)):
    wire = re.search(r'\bCRYSTALHD_FW_RESEARCH_ALGORITHM_' + name + r'\s+(\d+)U\b', source)
    declared = re.search(r'\beC011_VIDEO_ALG_' + name + r'\s*=\s*(0x[0-9a-fA-F]+)\b', legacy)
    assert wire and declared and int(wire.group(1)) == int(declared.group(1), 16) == value
raw_commands = (('SCALING_FILTERS', 'SCALING_FILTERS', 6, 0x10b),
                ('PIC_CAPTURE', 'PIC_CAPTURE', 7, 0x11c),
                ('SET_CSC', 'SET_CSC', 8, 0x180),
                ('SET_FGT', 'SET_FGT', 9, 0x182),
                ('CUSTOM_VIDOUT', 'CUSTOM_VIDOUT', 10, 0x1ff),
                ('FILL_PIC_BUF', 'FILL_PIC_BUF', 11, 0x126))
for selector_name, command_name, selector, offset in raw_commands:
    name = 'eCMD_C011_DEC_CHAN_' + command_name
    pattern = r'\b' + name + r'\s*=\s*eCMD_C011_CMD_BASE\s*\+\s*(0x[0-9a-fA-F]+)\b'
    for header in (legacy, wire_header):
        declared = re.search(pattern, header)
        assert declared and int(declared.group(1), 16) == offset
        assert re.search(r'#define\s+eCMD_C011_CMD_BASE\s+\(0x73763000\)', header)
    selector_macro = 'CRYSTALHD_FW_RESEARCH_' + selector_name + '_COMMAND'
    assert re.search(r'\b' + selector_macro + r'\s+' + str(selector) + r'U\b', uapi)
    assert re.search(r'case\s+' + selector_macro + r':\s*return\s+' + name + r'\s*;', source)
    wire = re.search(r'case\s+' + selector_macro + r':\s*return\s+(0x[0-9a-fA-F]+)U\s*;', cli)
    assert wire and int(wire.group(1), 16) == 0x73763000 + offset
print('Firmware probe: six fixed command routes match both source enums and CLI wire values')
for enabled in (None, '0', '1'):
    script = f'include {root}/driver/linux/Kbuild\nprobe-check:\n\t@echo $(crystalhd-objs)\n\t@echo $(ccflags-y)\n'
    args = ['make', '--no-print-directory', '-f', '-', 'probe-check', 'CONFIG_CRYPTO_HASH=y']
    if enabled is not None:
        args.append('CRYSTALHD_FW_RESEARCH=' + enabled)
    output = subprocess.check_output(args, input=script, text=True, env=make_env)
    assert ('crystalhd_fw_research.o' in output) == (enabled == '1')
    assert ('-DCRYSTALHD_ENABLE_FW_RESEARCH' in output) == (enabled == '1')
args = ['make', '--no-print-directory', '-f', '-', 'probe-check', 'CRYSTALHD_FW_RESEARCH=1']
assert subprocess.run(args, input=script, text=True, stdout=subprocess.PIPE,
                      stderr=subprocess.PIPE, env=make_env).returncode != 0
for value in ('y', 'm'):
    output = subprocess.check_output(args + ['CONFIG_CRYPTO_HASH=' + value],
                                     input=script, text=True, env=make_env)
    assert 'crystalhd_fw_research.o' in output
args = ['make', '--no-print-directory', '-f', '-', 'probe-check', 'CRYSTALHD_FW_RESEARCH=2', 'CONFIG_CRYPTO_HASH=y']
assert subprocess.run(args, input=script, text=True, stdout=subprocess.PIPE,
                      stderr=subprocess.PIPE, env=make_env).returncode != 0
print('Firmware probe: pinned blob hash and default-OFF Kbuild verified')

# This syntax-only UAPI test needs kernel userspace headers, not 32-bit libc
# or a runnable 32-bit binary. Check both layouts on x86 hosts.
abi = '''#include <stddef.h>
#include "crystalhd_fw_research.h"
_Static_assert(sizeof(struct crystalhd_fw_research_info) == 64, "info");
_Static_assert(offsetof(struct crystalhd_fw_research_info, generation) == 8, "info generation");
_Static_assert(offsetof(struct crystalhd_fw_research_info, selector_mask) == 16, "info mask");
_Static_assert(CRYSTALHD_FW_RESEARCH_GET_INFO == 0x80405291U, "info ioctl");
_Static_assert(sizeof(struct crystalhd_fw_research_request) == 32, "request");
_Static_assert(sizeof(struct crystalhd_fw_research_reply) == 276, "reply");
_Static_assert(sizeof(struct crystalhd_fw_research_result) == 1488, "result");
_Static_assert(offsetof(struct crystalhd_fw_research_result, generation) == 32, "generation");
_Static_assert(offsetof(struct crystalhd_fw_research_result, replies) == 104, "replies");
_Static_assert(offsetof(struct crystalhd_fw_research_result, firmware_hash_valid) == 68, "hash validity");
_Static_assert(_IOC_SIZE(CRYSTALHD_FW_RESEARCH_RUN) == 1488, "encoding");
_Static_assert(CRYSTALHD_FW_RESEARCH_RUN == 0xc5d05292U, "ioctl");
_Static_assert(CRYSTALHD_FW_RESEARCH_VERSION_ONLY == 1U, "existing version selector");
_Static_assert(CRYSTALHD_FW_RESEARCH_H264_CONTROL == 2U, "existing H264 selector");
_Static_assert(CRYSTALHD_FW_RESEARCH_H261_CONTROL == 3U, "H261 selector");
_Static_assert(CRYSTALHD_FW_RESEARCH_H263_CONTROL == 4U, "H263 selector");
_Static_assert(CRYSTALHD_FW_RESEARCH_MPEG1_CONTROL == 5U, "MPEG1 selector");
_Static_assert(CRYSTALHD_FW_RESEARCH_SCALING_FILTERS_COMMAND == 6U, "scaling selector");
_Static_assert(CRYSTALHD_FW_RESEARCH_PIC_CAPTURE_COMMAND == 7U, "capture selector");
_Static_assert(CRYSTALHD_FW_RESEARCH_SET_CSC_COMMAND == 8U, "CSC selector");
_Static_assert(CRYSTALHD_FW_RESEARCH_SET_FGT_COMMAND == 9U, "FGT selector");
_Static_assert(CRYSTALHD_FW_RESEARCH_CUSTOM_VIDOUT_COMMAND == 10U, "custom selector");
_Static_assert(CRYSTALHD_FW_RESEARCH_FILL_PIC_BUF_COMMAND == 11U, "fill selector");
_Static_assert(CRYSTALHD_FW_RESEARCH_SELECTOR_MASK == 2047U, "eleven fixed selectors");
_Static_assert(CRYSTALHD_FW_RESEARCH_MAX_COMMANDS == 5U, "unchanged reply capacity");
_Static_assert(CRYSTALHD_FW_RESEARCH_RESPONSE_WORDS == 64U, "unchanged raw capacity");
'''
compiler = shlex.split(os.environ.get('CC', 'cc'))
multiarch = subprocess.check_output(compiler + ['-print-multiarch'], text=True).strip()
flags = ['-std=c11', '-fsyntax-only', '-I' + str(root / 'include')]
if multiarch and (pathlib.Path('/usr/include') / multiarch).is_dir():
    flags.append('-I/usr/include/' + multiarch)
widths = ['-m32', '-m64'] if platform.machine() in ('x86_64', 'i386', 'i686') else []
for width in widths or ['']:
    subprocess.run(compiler + flags + ([width] if width else []) + ['-xc', '-'],
                   input=abi, text=True, check=True)
version_layout = '''#include <stddef.h>
#include <stdint.h>
#include "7411d.h"
_Static_assert(eCMD_C011_GET_VERSION == 0x73763004U, "GET_VERSION command");
_Static_assert(offsetof(C011RspGetVersion, status) == 2 * sizeof(uint32_t), "status word");
_Static_assert(offsetof(C011RspGetVersion, streamSwVersion) == 3 * sizeof(uint32_t), "stream word");
_Static_assert(offsetof(C011RspGetVersion, decoderSwVersion) == 4 * sizeof(uint32_t), "decoder word");
_Static_assert(offsetof(C011RspGetVersion, chipHwVersion) == 5 * sizeof(uint32_t), "firmware-reported chip word");
'''
for width in widths or ['']:
    subprocess.run(compiler + flags + ['-Wall', '-Wextra', '-Werror'] +
                   ([width] if width else []) + ['-xc', '-'],
                   input=version_layout, text=True, check=True)
print('Firmware probe: UAPI layout/encoding verified' + (' (32/64-bit)' if widths else ' (native)'))
print('Firmware probe CLI: GET_VERSION words match the legacy response declaration')
PY

for probe_sanitize in no yes; do
    probe_extra=
    if [ "$probe_sanitize" = yes ]; then
        probe_extra='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie'
    fi
    "${CC:-cc}" ${CFLAGS:-} -std=c11 -O1 -g -Wall -Wextra -Werror \
        -Wno-unused-parameter $probe_extra -I"$repo_dir/include" \
        -I"$repo_dir/driver/linux" -I"$probe_test_dir" \
        "$repo_dir/tests/fw-probe.c" -o "$probe_test_dir/check"
    printf 'Firmware probe: sanitizers=%s\n' "$probe_sanitize"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
        UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$probe_test_dir/check"

    # Only syscall/output names and main are substituted. Parsing, protocol
    # validation, control flow and JSON output all execute the actual CLI.
    "${CC:-cc}" ${CFLAGS:-} -std=c11 -O1 -g -Wall -Wextra -Werror \
        $probe_extra -U_FORTIFY_SOURCE -I"$repo_dir/include" -Dmain=crystalhd_probe_main \
        -Dopen=probe_open -Dfstat=probe_fstat -Dioctl=probe_ioctl -Dclose=probe_close \
        -Dprintf=probe_printf -Dfputs=probe_fputs -Dputchar=probe_putchar -Dputc=probe_putc \
        -Dferror=probe_ferror -Dfflush=probe_fflush -Dperror=probe_perror \
        -c "$repo_dir/tools/fw-research/flea_fw_probe.c" -o "$probe_test_dir/cli.o"
    # Reject glibc asm aliases and inline/builtin output bypasses BEFORE any
    # fixture execution. The final linkage audit independently checks that
    # the syscall wraps cannot leave a real device syscall reference behind.
    probe_linkage_fuse "$probe_test_dir/cli.o" 1
    "${CC:-cc}" ${CFLAGS:-} -std=c11 -O1 -g -Wall -Wextra -Werror \
        $probe_extra -I"$repo_dir/include" -I"$repo_dir/driver/linux" "$repo_dir/tests/fw-probe-cli.c" \
        "$probe_test_dir/cli.o" -Wl,--wrap=open,--wrap=open64,--wrap=__open_2,--wrap=__open64_2 \
        -Wl,--wrap=fstat,--wrap=fstat64,--wrap=__fxstat,--wrap=__fxstat64,--wrap=ioctl,--wrap=close \
        -o "$probe_test_dir/cli-check"
    probe_linkage_fuse "$probe_test_dir/cli-check" 0
    printf 'Firmware probe CLI: pre-execution callback/linkage audits passed\n'
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
        UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$probe_test_dir/cli-check"
    probe_linkage_fuse "$probe_test_dir/cli.o" 1
    probe_linkage_fuse "$probe_test_dir/cli-check" 0
    "$probe_test_dir/cli-check" --json-examples | "${PYTHON3:-python3}" -B -c '
import json, sys
def unique_object(pairs):
    result = {}
    for key, value in pairs:
        assert key not in result, "duplicate JSON key"
        result[key] = value
    return result
def invalid_constant(value):
    raise AssertionError("non-JSON constant: " + value)
lines = list(sys.stdin)
examples = [json.loads(line, object_pairs_hook=unique_object, parse_constant=invalid_constant) for line in lines]
assert len(examples) == 308
info, version, h264, failure, nohash, rejectedhash, h261, h263, mpeg1, rejected, readfail = examples[:11]
assert info["generation"] == "42" and info["research_selector_mask"] == 2047
assert lines[0] == "{\"version\":1,\"generation\":\"42\",\"research_selector_mask\":2047,\"expected_firmware_sha256\":\"" + info["expected_firmware_sha256"] + "\"}\n"
assert set(info) == {"version", "generation", "research_selector_mask", "expected_firmware_sha256"}
def u32(value):
    assert type(value) is int and 0 <= value <= 4294967295
def check_result(result):
    assert set(result) == {"version", "selector", "generation", "status", "expected_firmware_sha256",
                           "firmware_hash_valid", "observed_firmware_sha256", "download_attempted",
                           "download_status", "cleanup_attempted", "cleanup_status", "retained",
                           "command_count", "replies"}
    assert type(result["status"]) is int and -4095 <= result["status"] <= 0
    assert result["generation"] == "42" and result["version"] == 1
    assert type(result["selector"]) is int and 1 <= result["selector"] <= 11
    assert result["expected_firmware_sha256"] == info["expected_firmware_sha256"]
    for field in ("firmware_hash_valid", "download_attempted", "cleanup_attempted", "retained"):
        assert type(result[field]) is bool
    for field in ("download_status", "cleanup_status", "command_count"):
        u32(result[field])
    assert isinstance(result["replies"], list) and len(result["replies"]) == result["command_count"]
    schedules = {1: [0x73763001, 0x73763004],
                 2: [0x73763001, 0x73763004, 0x73763100, 0x73763103, 0x73763101]}
    for selector in (3, 4, 5):
        schedules[selector] = [0x73763001, 0x73763004, 0x73763100, 0x73763101]
    for selector, command in enumerate((0x7376310b, 0x7376311c, 0x73763180,
                                         0x73763182, 0x737631ff, 0x73763126), 6):
        schedules[selector] = [0x73763001, 0x73763004, command]
    assert [reply["command"] for reply in result["replies"]] == schedules[result["selector"]][:result["command_count"]]
    assert result["command_count"] <= len(schedules[result["selector"]])
    for index, reply in enumerate(result["replies"]):
        assert set(reply) == {"command", "sequence", "transport_status", "raw_response_valid",
                              "header_matches", "raw_response_words", "decoded_response"}
        for field in ("command", "sequence", "transport_status"):
            u32(reply[field])
        assert reply["sequence"] == index + 1
        assert type(reply["raw_response_valid"]) is bool and type(reply["header_matches"]) is bool
        raw = reply["raw_response_words"]
        if reply["raw_response_valid"]:
            assert isinstance(raw, list) and len(raw) == 64
            for word in raw:
                u32(word)
            assert reply["header_matches"] == (raw[:2] == [reply["command"], reply["sequence"]])
            if reply["transport_status"] == 0:
                assert raw[2] == 0
        else:
            assert raw is None and not reply["header_matches"]
        valid_version = (reply["command"] == 0x73763004 and reply["raw_response_valid"] and
                         reply["header_matches"] and reply["transport_status"] == 0 and raw[2] == 0)
        expected = {"stream_firmware_version": raw[3], "decoder_firmware_version": raw[4],
                    "firmware_reported_chip_hw_version": raw[5]} if valid_version else None
        assert reply["decoded_response"] == expected
        if expected is not None:
            for value in reply["decoded_response"].values():
                u32(value)
        if reply["command"] == 0x73763103:
            assert reply["decoded_response"] is None
for result in examples[1:]:
    check_result(result)
for result, count in ((version, 2), (h264, 5), (h261, 4), (h263, 4), (mpeg1, 4)):
    assert result["generation"] == "42" and result["status"] == 0
    assert result["firmware_hash_valid"] and result["observed_firmware_sha256"] == info["expected_firmware_sha256"]
    assert result["command_count"] == count and len(result["replies"]) == count
    for index, reply in enumerate(result["replies"]):
        assert reply["sequence"] == index + 1
        assert reply["raw_response_valid"] and reply["header_matches"]
        assert len(reply["raw_response_words"]) == 64
        assert reply["raw_response_words"][-1] == 4294967295
for selector, result in ((3, h261), (4, h263), (5, mpeg1)):
    assert result["selector"] == selector
    assert [r["command"] for r in result["replies"]] == [0x73763001, 0x73763004, 0x73763100, 0x73763101]
    assert result["replies"][3]["sequence"] == 4
assert [r["command"] for r in h264["replies"]] == [0x73763001, 0x73763004, 0x73763100, 0x73763103, 0x73763101]
assert h264["replies"][4]["sequence"] == 5
assert failure["status"] < 0 and failure["replies"][0]["raw_response_words"] is None
assert not failure["replies"][0]["raw_response_valid"]
assert not nohash["firmware_hash_valid"] and nohash["observed_firmware_sha256"] is None
assert rejectedhash["firmware_hash_valid"] and rejectedhash["observed_firmware_sha256"] != info["expected_firmware_sha256"]
assert rejected["selector"] == 3 and rejected["status"] < 0 and rejected["command_count"] == 3
assert [r["command"] for r in rejected["replies"]] == [0x73763001, 0x73763004, 0x73763100]
reply = rejected["replies"][2]
assert reply["raw_response_valid"] and reply["header_matches"] and reply["transport_status"] == 11
assert len(reply["raw_response_words"]) == 64 and reply["raw_response_words"][2:4] == [4294967295, 4294967295]
assert readfail["selector"] == 4 and readfail["status"] < 0 and readfail["command_count"] == 3
assert not readfail["replies"][2]["raw_response_valid"] and readfail["replies"][2]["raw_response_words"] is None
position = 11
patterns = [(79360, 305419896, 458773), (0, 0, 0), (4294967295, 2147483648, 3735928559)]
for selector in range(1, 6):
    count = 2 if selector == 1 else 5 if selector == 2 else 4
    for pattern in patterns:
        result = examples[position]; position += 1
        assert result["selector"] == selector and result["status"] == 0 and result["command_count"] == count
        assert list(result["replies"][1]["decoded_response"].values()) == list(pattern)
    for phase in range(1, count + 1):
        for kind in (1, 2, 3, 4, 5, 7):
            result = examples[position]; position += 1
            assert result["selector"] == selector and result["status"] < 0 and result["command_count"] == phase
            reply = result["replies"][-1]
            assert reply["decoded_response"] is None
            if phase > 2:
                assert list(result["replies"][1]["decoded_response"].values()) == list(patterns[2])
            if kind in (1, 2):
                assert not reply["raw_response_valid"] and reply["raw_response_words"] is None
            elif kind in (3, 7):
                assert reply["transport_status"] == 11 and reply["header_matches"]
                assert reply["raw_response_words"][2] == (4294967295 if kind == 3 else 0)
            else:
                assert reply["raw_response_valid"] and not reply["header_matches"]
    for retained in (True, False):
        result = examples[position]; position += 1
        assert result["selector"] == selector and result["status"] < 0 and result["command_count"] == count
        assert result["retained"] is retained
        assert result["cleanup_status"] == (0 if retained else 7)
        assert list(result["replies"][1]["decoded_response"].values()) == list(patterns[2])
assert position == 150
assert examples[150]["status"] < 0 and examples[150]["retained"] and not examples[150]["cleanup_attempted"]
assert examples[151]["status"] == 0 and not examples[151]["retained"] and examples[151]["command_count"] == 5
for result in examples[150:152]:
    assert list(result["replies"][1]["decoded_response"].values()) == list(patterns[2])
position = 152
for selector, command in enumerate((0x7376310b, 0x7376311c, 0x73763180,
                                     0x73763182, 0x737631ff, 0x73763126), 6):
    for pattern in patterns:
        result = examples[position]; position += 1
        assert result["selector"] == selector and result["status"] == 0 and result["command_count"] == 3
        assert list(result["replies"][1]["decoded_response"].values()) == list(pattern)
        assert result["replies"][2]["decoded_response"] is None
    for phase in range(1, 4):
        for kind in (1, 2, 3, 4, 5, 7):
            result = examples[position]; position += 1
            assert result["selector"] == selector and result["status"] < 0 and result["command_count"] == phase
            reply = result["replies"][-1]
            assert reply["decoded_response"] is None
            if phase > 2:
                assert list(result["replies"][1]["decoded_response"].values()) == list(patterns[2])
            if kind in (1, 2):
                assert not reply["raw_response_valid"] and reply["raw_response_words"] is None
            elif kind in (3, 7):
                assert reply["transport_status"] == 11 and reply["header_matches"]
                assert reply["raw_response_words"][2] == (4294967295 if kind == 3 else 0)
            else:
                assert reply["raw_response_valid"] and not reply["header_matches"]
    for retained in (True, False):
        result = examples[position]; position += 1
        assert result["selector"] == selector and result["status"] < 0 and result["command_count"] == 3
        assert result["retained"] is retained and result["cleanup_status"] == (0 if retained else 7)
        assert list(result["replies"][1]["decoded_response"].values()) == list(patterns[2])
    for retained in (False, True):
        result = examples[position]; position += 1
        assert result["selector"] == selector and result["status"] == -71 and result["command_count"] == 3
        reply = result["replies"][2]
        assert reply["transport_status"] == 0 and reply["raw_response_valid"] and not reply["header_matches"]
        assert reply["raw_response_words"] == [command] + [0] * 63 and reply["decoded_response"] is None
        assert result["retained"] is retained and result["cleanup_status"] == (7 if retained else 0)
        assert list(result["replies"][1]["decoded_response"].values()) == list(patterns[2])
    result = examples[position]; position += 1
    assert result["selector"] == selector and result["status"] == 0 and result["command_count"] == 3
    assert result["replies"][2]["raw_response_words"][3] == 7 and result["replies"][2]["decoded_response"] is None
assert position == len(examples)
print("Firmware probe CLI: original 152 plus 156 raw-route strict JSON examples verified")
'
done
