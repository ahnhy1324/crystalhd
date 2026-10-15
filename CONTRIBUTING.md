# Contributing

Bug reports, test results, documentation fixes, and patches are welcome. A CrystalHD card is not required to contribute.

## Reporting bugs

Check existing issues first. Include what you can:

- Release tag/commit and the **installed** kernel module, library, and frontend versions.
- Card model (BCM70012/BCM70015), CPU architecture, distribution/kernel, application, and enabled options.
- Reproduction steps, expected and actual behavior, media codec/profile/resolution, and relevant logs.
- For playback failures, frame/timestamp counts, error messages, and close/reopen behavior where available.

Mark results **PASS**, **FAIL**, or **NOT RUN**, and distinguish real hardware from containers or emulation. Redact private paths, credentials, sensitive environment values, and media bytes before posting logs. See [BRINGUP.md](BRINGUP.md) for diagnostics.

## Pull requests

- Keep changes focused and link related issues. Discuss broad ABI, firmware, DMA, or device-lifetime changes before implementation.
- Preserve the documented ioctl and `libcrystalhd.so.3` contracts, default behavior, and single-session device ownership. Experimental features must be explicit opt-ins.
- Add regression tests when practical, report what was actually run, and update affected documentation.
- Keep existing copyright and SPDX notices; see [LICENSES.md](LICENSES.md).

## Validation

Run relevant [developer checks](README.md#developer-checks). Hardware evidence should identify the installed components, device, test input, frame/PTS/pixel results, errors, and clean teardown. If hardware is unavailable, say **NOT RUN**; CI/build success does not prove physical playback.

Maintainer release gates and support claims are tracked in [RELEASING.md](RELEASING.md).
