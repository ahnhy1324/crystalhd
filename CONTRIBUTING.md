# Contributing

Bug reports, test results, documentation fixes and patches are welcome.
A CrystalHD card or a complete fix is not required to contribute.

## Reporting bugs

Search [existing issues](https://github.com/ahnhy1324/crystalhd/issues) first.
For a new problem, include what you know:

- Installed release or source commit, card/CPU, OS/kernel, 32- or 64-bit
  userspace, player/backend versions and relevant options.
- Reproduction steps or command, codec/profile/resolution/frame rate,
  expected result and actual result.
- Relevant error messages and reviewed `crystalhd-check` output, if available.
  Mention comparisons with software decoding or an older version if tested.

Prefer a small, redistributable sample or a command that generates one.
Remove credentials, private paths and media data from logs before sharing;
verbose decoder logs may contain compressed video bytes. Do not upload private
media or raw memory dumps. Missing details can be added later.

## Patches and testing

Send one focused pull request, link the relevant issue and explain the problem
and change. Discuss substantial API, firmware or feature changes in an issue
first. Follow the surrounding code style and avoid unrelated cleanup.

For code changes, add a focused regression test where practical and run the
relevant [developer checks](README.md#developer-checks). Record the commands,
tested revision and environment, and report `PASS`, `FAIL` or `NOT RUN`.
State missing dependencies or hardware rather than claiming unperformed tests
passed. Documentation-only changes need link and content checks, not hardware
soaks; full [release qualification](RELEASING.md) is maintainer work.

For playback results, distinguish compilation, emulation and real-card tests.
Name the actual player/backend and options; FFmpeg success does not establish
Kodi or another player's compatibility. Report frame counts, timing and clean
close/reopen when available. A merged fix is not a hardware retest.

## Compatibility and experiments

Preserve `libcrystalhd.so.3`, legacy ioctl layouts, documented buffer ownership
and default playback behavior. Consider both x86-64 and i686/legacy CPU paths;
mark untested devices explicitly. Changes to these contracts need discussion.

Keep new experiments opt-in and separate measured results from hypotheses.
DMA or firmware work must explain completion, failure and cleanup behavior;
see [DMA ownership](DMA.md). Do not hide a failed test with repeated resets or
force-unload a module that still owns active DMA.

## Licensing and attribution

Preserve existing copyright/SPDX notices and follow [LICENSES.md](LICENSES.md).
Identify the source of adapted code and credit the original patch, report or
test contributor in the pull request.
