## History

There are various versions of the [Broadcom
CrystalHD](https://en.wikipedia.org/wiki/Broadcom_Crystal_HD) (BCM70012 and
BCM70015) drivers floating around the web.

The main public lineages, roughly from oldest to newest, are:

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

## April 2025 maintenance milestone

The fork added Ubuntu CI for the driver and userspace components, updated the
kernel build for newer APIs, and refreshed the example and build documentation.
CI compilation does not load the PCI device or replace hardware testing.

## August 2026 maintenance milestone

The userspace integrations in this update are experimental. See
[README.md](README.md) and the component guides before deployment.

- Set Linux 6.1 as the minimum supported kernel and added CI compilation
  against current 6.1, 6.6, 6.12, and 6.18 LTS releases, stable, and mainline.
- Added a GStreamer 1.x decoder and an experimental VA-API backend, including
  the buffer and video-processing operations needed by the documented clients.
- Added DKMS support and optional browser integration tooling.
- Hardened the kernel device lifetime for safe rejection of a second playback
  client. CrystalHD still supports only one active hardware session.
- Hardware-tested H.264 Constrained Baseline, Main, and High decoding on a
  BCM70015 with Ubuntu kernel 6.17.0-41-generic.

## October 2026 library addition

Added an opt-in raw-frame uploader and two-reference tile compositor using a
native H.264 carrier, without changing the existing library ABI or default
decode paths. This does not expose standalone MFD processing.

See [README.md](README.md) for current build, installation, playback, and test
instructions. This file is only the project history.
