# Native companion support and distribution policy

This policy covers the optional native V4L2 Kodi companion. Use the
[delivery guide](NATIVE-RUNTIME-DELIVERY.md) to evaluate its frozen RC2 package;
for ordinary playback, start with [everyday usage](USAGE.md).

Distribution scope is the published optional 1.3.0-rc.2 companion for the
measured x86_64 Core2 T8100 / BCM70015 / Intel965 cohort and the exact
Kodi22RC1, Platform26.08 and GL commits in the delivery guide. Native FFmpeg was configured `--cpu=core2`
with runtime CPU detection. No wider minimum CPU or cross-distribution ABI
support is inferred. The runtime's observed `atan2f@GLIBC_2.43` requirement is
distinct from the pinned provider package glibc2.44. Provider libdrm2.4.134,
libva2.24.1 and zlib-ng2.3.3 come from that exact Platform; the delivery does
not bundle or replace their binaries.

The neutral runtime, reproduced twice from unchanged pinned source, and its
complete FFmpeg/dependency/SDK source and notice companions form the published
scoped experimental RC2 delivery. A deterministic packager status is
only an assembly result. Public release/tag and stable promotion follow the
main repository's release authorization; no arbitrary machine, clean-OS,
physical presentation, full FFmpeg or security-certification claim follows.
The inner historical `PUBLIC_NOT_READY` provenance remains intact and is
supplemented by dated outer evidence and this policy.

The CrystalHD maintenance project owns this experimental assembly, its pinned
inputs, own integration code, packaging instructions and issue intake at
https://github.com/ahnhy1324/crystalhd/issues. This assigns a project role; it
does not claim that an individual accepted a new support obligation or SLA.

Upstream FFmpeg, Kodi and Platform maintainers own their code and upstream
security fixes. This project does not maintain a modified external fork or
promise external backports. Reports affecting the pinned assembly enter the
project tracker; assess the relevant upstream fixes, acquire a new unmodified
source/package, rebuild/repackage, and repeat affected metadata/ABI/media and
rollback checks before updating pins. Never overwrite a published artifact or
silently follow a moving Flatpak branch. If a vulnerable dependency cannot be
updated and requalified, withdraw or mark the optional companion unsupported
and direct users to the maintained stable route. No response-time guarantee
or continuous vulnerability-monitoring claim is made.

Current native acceptance includes source-qualified PTS/layout, EOS/reopen,
scoped pause controls and bounded lifecycle evidence, with the recorded
renderer/QoS loss disposition. Other seek/Stop evidence retains its actual
frontend/module attribution; broader native controls are not inferred.
Same-DPB nonempty STOP→START remains unsupported. Independent
booted OS, Atom/i386, BCM70012, broad codec coverage, physical presentation and
general FHD50/60 remain unqualified. The #538 heap-abort follow-up is excluded
for the recorded maintainer reason; #538 itself stays open.

Original copyright/SPDX notices control each file. The FFmpeg source companion
contains its complete original COPYING files and configuration. Dependency
archives retain per-file notices plus complete SDK recipes/patches; they are
not relicensed by this policy. Own GPL2 producer/build tools and LGPL2.1 helper
tools are accompanied by both license texts. The runtime is dynamically linked;
source, recipe and private-prefix selection permit replacement/rebuilding of
the library set without changing a system library or signed Kodi executable.
