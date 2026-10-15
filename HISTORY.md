# History

## 1.1.0-rc.1 — candidate (unpublished)

This is the planned identity for the v1.1 release candidate tracked in
[#430](https://github.com/ahnhy1324/crystalhd/issues/430). It is not a published
tag or release, and the exact candidate remains subject to Gate A freeze,
review and explicit maintainer RC approval.

- Compatibility reports [#412](https://github.com/ahnhy1324/crystalhd/issues/412)–[#415](https://github.com/ahnhy1324/crystalhd/issues/415)
  from [@sjq42](https://github.com/sjq42) led to normal i686 VA SSE2 selection,
  preservation of required VA build flags with external `CXXFLAGS`, bounded
  large H.264 access units with old-library fallback, selected H.264 MBAFF
  field pairs, and 32-bit player bus-message handling. These do not imply
  general interlaced VA, Kodi, Atom or browser support.
- The read-only configuration inspector, reviewed presets and redacted bug
  report are additive tooling; configuration files do not apply runtime
  settings.
- Driver and library work hardens decoder stop, suspend/resume, Flea power and
  receive ownership. Default-off firmware investigations remain research, not
  new supported hardware functions.
- The candidate keeps the legacy ioctl version and the `libcrystalhd.so.3`
  ABI. Its planned bundle/package mapping is `1.1.0-rc.1` / `3.10.4`; v1.0
  tags and qualification evidence remain immutable historical baselines.

## 1.0.0 — October 13, 2026

First numbered bundle release of this maintained fork, following the immutable
`phase1-baseline-2026-09-29`. [Published release](https://github.com/ahnhy1324/crystalhd/releases/tag/v1.0.0);
qualification and publication are recorded in
[#369](https://github.com/ahnhy1324/crystalhd/issues/369).

- Local playback: accumulated DMA-stop, queued-seek, EOS and reopen fixes;
  broader selected GStreamer/VA-API codec and interlace coverage, with visible
  hardware selection in `crystalhd-play`.
- Receive efficiency: negotiated event-driven output readiness with fallback
  for older drivers/libraries, plus bounded default H.264 transport history.
- Optional features: session-scoped BCM70015 L0s workaround, progressive
  hardware output scaling and explicit software enlargement. Native-size
  output remains the default.
- Streaming: documented Moonlight frame-matching playback and opt-in
  previous-frame mode for higher cadence, with measured timing/CPU tradeoffs.
- Compatibility: retain `libcrystalhd.so.3`, legacy ioctl behavior, x86-64/i686
  builds and old-GStreamer support; separate bundle and component versions.
- Installation: keep public module/runtime paths and module indexes readable
  when the invoking shell uses a restrictive file-creation mask.
- Limits: BCM70015 local playback is the primary path. BCM70012, additional
  hosts and browser hardware coverage remain unvalidated; standalone video
  processing and V4L2 research are not advertised as working frontends.

See [README.md](README.md) for first use and [RELEASING.md](RELEASING.md) for
qualification and publication policy.

## Project lineage

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

- Set Linux 6.1 as the then-current minimum supported kernel and added CI
  compilation against 6.1, 6.6, 6.12, and 6.18 LTS releases, stable, and
  mainline. Linux 5.15 support followed in September 2026.
- Added a GStreamer 1.x decoder and an experimental VA-API backend, including
  the buffer and video-processing operations needed by the documented clients.
- Added DKMS support and optional browser integration tooling.
- Hardened the kernel device lifetime for safe rejection of a second playback
  client. CrystalHD still supports only one active hardware session.
- Hardware-tested H.264 Constrained Baseline, Main, and High decoding on a
  BCM70015 with Ubuntu kernel 6.17.0-41-generic.

## October 2026 library addition

Added an opt-in raw-frame uploader, two-reference tile compositor and bounded
quarter-pixel translator using native H.264, including bounded non-reference
composition and translation. Existing library ABI and default decode paths
are preserved; this does not expose standalone MFD processing.

## October 2026 playback scaling

Added opt-in progressive BCM70015 decoder-output width selection to GStreamer
and `crystalhd-play`, plus an explicit bounded software output-size stage.
Native output remains the default and the legacy library ABI is unchanged.

See [README.md](README.md) for current build, installation, playback, and test
instructions. This file is only the project history.
