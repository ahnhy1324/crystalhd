# Native companion support and distribution policy

This policy covers the optional native V4L2 Kodi companion. Use the
[current RC4 preparation instructions](NATIVE-RUNTIME-DELIVERY.md#rc4-companion-layout-and-use);
use the [published RC3 reference](NATIVE-RUNTIME-DELIVERY.md#published-rc3-reference)
for the available RC3 package;
for ordinary playback, start with [everyday usage](USAGE.md).

RC4 preparation retains the bounded optional scope of published RC2/RC3 and
all integrated core features/defaults. New own-core/module19 and outer-package
qualification remain required; a reused runtime build receipt does not supply
those results. Preparation is not qualification or publication. The
measured x86_64 Core2 T8100 / BCM70015 / Intel965 cohort and the exact
Kodi22RC1, Platform26.08 and GL commits in the delivery guide remain the target.
Native FFmpeg was configured `--cpu=core2`
with runtime CPU detection. No wider minimum CPU or cross-distribution ABI
support is inferred. The runtime's observed `atan2f@GLIBC_2.43` requirement is
distinct from the pinned provider package glibc2.44. Provider libdrm2.4.134,
libva2.24.1 and zlib-ng2.3.3 come from that exact Platform; the delivery does
not bundle or replace their binaries.

The neutral runtime, reproduced twice from unchanged pinned source, and its
complete FFmpeg/dependency/SDK source and notice companions form the frozen
published RC2 delivery, reused unchanged in RC3 and RC4 preparation. A deterministic
packager status is
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

The retained RC2 native acceptance includes source-qualified PTS/layout, EOS/reopen,
scoped pause controls and bounded lifecycle evidence, with the recorded
renderer/QoS loss disposition. Other seek/Stop evidence retains its actual
frontend/module attribution; broader native controls are not inferred. RC3
adds own token-bound source-range transitions and bounded progressive POC0/2
multi-slice association. Its [published qualification](https://github.com/ahnhy1324/crystalhd/releases/download/v1.3.0-rc.3/release-qualification.json)
binds frozen own core `36ef4c681f49` / module18 to the fresh 23-case native
campaign, including pinned original Kodi720p/FHD EOS/reopen and restoration.
Those RC3 results retain their original source/module scope. RC4 must bind
its exact frozen core, measured native module19 and actual integration checks
in `evidence/rc4-native-qualification.json`, with unchanged-path reuse named
separately. File-only slot/preflight success remains separate from media evidence;
it does not qualify a changed native source, kernel, launcher or target.
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
