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
for name, value in (('H261', 2), ('H263', 3), ('MPEG1', 5)):
    wire = re.search(r'\bCRYSTALHD_FW_RESEARCH_ALGORITHM_' + name + r'\s+(\d+)U\b', source)
    declared = re.search(r'\beC011_VIDEO_ALG_' + name + r'\s*=\s*(0x[0-9a-fA-F]+)\b', legacy)
    assert wire and declared and int(wire.group(1)) == int(declared.group(1), 16) == value
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
_Static_assert(CRYSTALHD_FW_RESEARCH_SELECTOR_MASK == 31U, "five fixed selectors");
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
print('Firmware probe: UAPI layout/encoding verified' + (' (32/64-bit)' if widths else ' (native)'))
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
info, version, h264, failure, nohash, rejectedhash, h261, h263, mpeg1, rejected, readfail = [json.loads(line) for line in sys.stdin]
assert info["generation"] == "42" and info["research_selector_mask"] == 31
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
print("Firmware probe CLI: JSON metadata, full replies and invalid raw=null verified")
'
done
