## History

There are various versions of the [Broadcom
CrystalHD](https://en.wikipedia.org/wiki/Broadcom_Crystal_HD) (BCM70012 and
BCM70015) drivers floating around the web.

Here are the ones I've found, roughly in order of most obsolete/broken to newest:

1. Staging driver from [Linux kernel v3.16](https://git.kernel.org/pub/scm/linux/kernel/git/gregkh/staging.git/tree/drivers/staging/crystalhd?h=v3.16)
  — this version was [removed in 2014 from v3.17](https://lkml.iu.edu/hypermail/linux/kernel/1408.0/01475.html) due to
  the fact that it was unmaintained and obsolete; it only supported the
  BCM70012 chip, for example.

  * See the [2013 LKML discussion](https://lkml.org/lkml/2013/10/27/103)
    between Steven Newbury and Greg Kroah-Hartman.

2. The historical Debian packaging tree,
   which appears to be based on a ~2010 version of the code from the
   mainline kernel, and like it only supports the BCM70012 chip.

3. Jarod Wilson's LinuxTV tree, last updated in 2012.

4. [@yeradis](https://github.com/yeradis)'s
   [tree](https://github.com/yeradis/crystalhd), forked from Jarod's tree.

5. [@dbason](https://github.com/dbason)'s
   [tree](https://github.com/dbason/crystalhd), forked from Yeradis's tree
   and updated for the kernels available in 2016.

The dbason tree was the practical modern option when this history was first
written and was reported working with Linux 4.4 and BCM70015. That statement
is historical; it does not describe current kernels or this repository.

## April 2025 update

- Added a GitHub Actions workflow (`.github/workflows/main.yml`) to build the
  driver and userspace components on Ubuntu.
- Fixed a bug in the driver compilation process to ensure compatibility with newer kernels.
- Updated the example program to improve stability and performance.
- Enhanced documentation for building and testing the CrystalHD driver.

CI compilation does not load the PCI device or replace hardware testing.

## August 2026 update

The userspace integrations in this update are experimental. See the status
table and component limitations in [README.md](README.md) before deployment.

- Set Linux 6.1 as the minimum supported kernel and added CI compilation
  against current 6.1, 6.6, 6.12, and 6.18 LTS releases, stable, and mainline.
- Added a GStreamer 1.x decoder and a VA-API H.264 backend for current FFmpeg
  and Chromium-class clients.
- Added DRM PRIME import/export, NV12-to-ARGB video processing, DKMS support,
  and a persistent Chrome setup workflow.
- Hardened the kernel device lifetime for safe rejection of a second playback
  client. CrystalHD still supports only one active hardware session.
- Linked Chromium's exported and reimported DMA-BUF decode/display aliases to
  the same completion state, preventing transient solid-green and black frames;
  added immutable timestamped decode staging and asynchronous VPP so
  CrystalHD's reorder buffer cannot block later input or race reused Chrome
  surfaces, plus post-wait display-generation validation, exact frame
  validation, private full-frame ARGB staging, and SIMD NV12-to-ARGB
  conversion. Surface teardown now cancels queued writes before Chromium can
  recycle their DMA-BUF memory across a seek.
- Hardware-tested H.264 Constrained Baseline, Main, and High decoding on a
  BCM70015 with Ubuntu kernel 6.17.0-41-generic.

See [README.md](README.md) for current build, installation, browser, and test
instructions. This file is only the project history.
