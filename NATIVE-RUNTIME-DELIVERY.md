# Experimental native runtime delivery for 1.3

Use this optional companion to evaluate the **native V4L2 route in Kodi** with
its matching candidate and exact target. For ordinary local-file playback,
start with [installation](README.md#quick-start) and [everyday usage](USAGE.md).
The companion supplies prebuilt, unmodified minimal FFmpeg DRM_PRIME libraries;
using them does not require compiling FFmpeg. Rebuild instructions are separate
in [BUILDING](NATIVE-RUNTIME-BUILDING.md).

**Matching 1.3 companion:** bundle `1.3.0` /
module-DKMS `3.10.20` retains the complete core, shared settings, default-off
Centered and every integrated opt-in with their current defaults. The
[release scope](RELEASING.md#stable-13-promotion-preparation) separates
actual archive/native/install checks from unchanged-component reuse. Use the
core and companion identified by the [official 1.3 release entry](https://github.com/ahnhy1324/crystalhd/releases/tag/v1.3.0)
and its qualification receipt; those records define availability and results.
A source version or files-only PASS does not establish native playback acceptance.

If the official 1.3 release is not available, use the published
[RC3 reference](#published-rc3-reference) or stable `1.2.0` instead; retain the
previous source/module for rollback. The [frozen RC4 reference](#frozen-rc4-reference)
keeps its own candidate identity. No external product patch is applied, and
selecting this companion does not change other defaults.

## Stable 1.3 companion layout and use

The matching outer archive is `crystalhd-native-runtime-1.3.0.tar.gz`.
Select its own frozen core through `evidence/stable-core-binding.json`; do not
substitute an older core or moving master checkout. Module20's actual native
srcversion belongs to its measured qualification receipt, not a guessed value.

| Item | Stable 1.3 binding and evidence |
| --- | --- |
| Own core and tools | `evidence/stable-core-binding.json` identifies the exact core archive/revision, module20 and packaged own tools/guides. |
| Actual native acceptance | `evidence/stable-native-qualification.json` must bind that source and measured native module to the actual candidate checks and restoration. A preparation-only receipt cannot authorize activation. |
| Unchanged runtime | `runtime/runtime-private.tar`, original provider/metadata pins and two-build `evidence/build-proof.json` remain unchanged. `evidence/reuse-scope.json` names the limits of reuse. |
| Historical results | Explicit RC4, RC3 and RC2 evidence retains its source/module, original failures and dates; it is not relabeled module20. |
| Source and notices | Complete FFmpeg/dependency/SDK materials and offline Git metadata retain their original bytes, revisions and licenses. Inner historical `PUBLIC_NOT_READY` metadata remains intact. |

Install the exact pinned App/Platform/GL deployments using the shared
[acquisition commands and target pins](#acquire-the-exact-target) before selecting
private files. The later Frozen RC2 reference retains module17's commands;
use the matching 1.3 procedure below.

### Verify and select private files

Start in a new extraction of the stable 1.3 companion. Verify the archive against its
release `SHA256SUMS`, then run the included `sha256sum -c SHA256SUMS`. Inspect the
core binding, native qualification and reuse-scope receipts before activation.
The unchanged inner runtime has the same three metadata pins as RC2/RC3.
Choose a new absolute store whose parent exists and the pinned Platform's
`files` directory. These commands copy and inspect private files only:

```sh
delivery_root=$(pwd -P)
slot_store=/absolute/private/stable13-native-slots
platform_files=/absolute/pinned/Platform/files
(
  set -eu
  python3 -B tools/native-runtime-slots.py --store "$slot_store" install \
    --archive "$delivery_root/runtime/runtime-private.tar" \
    --archive-sha256 dc3d88bdeac5dc45a4f52800e1c653dcb35fcd73c14c158460974083f872c020 \
    --rootfs "$platform_files" --platform-relative . \
    --rootfs-identity 'Platform e7f7b0fb20662fe92e4eac65d58a57019cefae45a39559c4b50440a88acc0491' \
    --files-sha256 aee1eba5bc5d54bf54b6499ad3ffdb94ee8bd89015953c800804ce44e889c11d \
    --dependencies-sha256 dfbff250c9cd61dfc83beba99bffab30e2d7c2a3af0e3785d021d964feed6b02 \
    --provenance-sha256 7ffd06ad3b22fbf19ae5c4088f5c3d6418ed8dcaca1365957d4cd369d4ea781f
  python3 -B tools/native-runtime-slots.py --store "$slot_store" status
)
```

Stop if a block fails. Use the returned `active_prefix`; its `lib` directory
is the selected FFmpeg library set. Files-only PASS does not activate native
decoding or qualify playback. Do not change/remove a slot while a consumer
still uses it.

### Build and temporarily activate the frozen native module

The ordinary legacy module does not expose the native video node. Build the
supplied frozen core in a new private directory, separate from `/usr/src`,
registered DKMS source and the installed/cached legacy module20. Do not register
this optional build under the legacy package number. Install the normal
[build prerequisites](README.md#quick-start), including `build-essential` and
headers matching the running kernel (`linux-headers-$(uname -r)` on Ubuntu).
The commands also use Python3, GNU tar, kmod and udev from that environment.

```sh
native_build=/absolute/new/stable13-native-build
native_source=$(python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); t=d["core_archive_top_level"]; assert t.startswith("crystalhd-1.3.0-") and "/" not in t and t not in (".",".."); print(sys.argv[2]+"/"+t)' "$delivery_root/evidence/stable-core-binding.json" "$native_build") &&
(
  set -eu
  core_member=$(python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); assert d["bundle_version"]=="1.3.0" and d["module_version"]=="3.10.20"; print(d["core_archive"]["member"])' "$delivery_root/evidence/stable-core-binding.json")
  mkdir "$native_build"
  tar -xzf "$delivery_root/$core_member" -C "$native_build"
  python3 -c 'import json,pathlib,sys; d=json.load(open(sys.argv[1])); p=pathlib.Path(sys.argv[2]); assert (p/"VERSION").read_text().strip()==d["bundle_version"]; assert (p/"SOURCE_REVISION").read_text().strip()==d["core_source"]' "$delivery_root/evidence/stable-core-binding.json" "$native_source"
  make -C "$native_source" driver CRYSTALHD_V4L2=1 KVER="$(uname -r)"
  test "$(modinfo -F version "$native_source/driver/linux/crystalhd.ko")" = 3.10.20
  modinfo -F srcversion "$native_source/driver/linux/crystalhd.ko"
)
```

An existing build directory must not be reused or extracted over. The two
path variables remain available in the parent shell for activation. A
different kernel/build is a separately identified artifact; the measured native
srcversion below must match before unloading the working module. A legacy
build of the same package has a different srcversion. Module strings alone
do not establish ABI or media acceptance.

Before activation, preserve the installed module's selection and actual options.
Follow the [idle transition rules](BRINGUP.md#reload-an-idle-device): AC,
temperature below75C, no legacy/video owners, refcount and pin balance zero.
This temporary load uses recorded `force_l0s_off=1` / `session_l0s_off=0`;
preserve any distinct prior options for rollback. This block deliberately stops
before unloading if the candidate lacks a passing actual native receipt.

```sh
(
  set -eu
  native_srcversion=$(python3 -c 'import json,sys; r=json.load(open(sys.argv[1])); b=json.load(open(sys.argv[2])); assert r["status"].startswith("PASS") and r["core_source"]==b["core_source"] and r["module_version"]=="3.10.20" and r["restored"] is True; print(r["module_srcversion"])' "$delivery_root/evidence/stable-native-qualification.json" "$delivery_root/evidence/stable-core-binding.json")
  test "$(modinfo -F version "$native_source/driver/linux/crystalhd.ko")" = 3.10.20
  test "$(modinfo -F srcversion "$native_source/driver/linux/crystalhd.ko")" = "$native_srcversion"
  if test -d /sys/module/crystalhd; then sudo modprobe -r crystalhd; fi
  sudo modprobe -a v4l2_mem2mem videobuf2_dma_sg
  sudo insmod "$native_source/driver/linux/crystalhd.ko" \
    force_l0s_off=1 session_l0s_off=0
  sudo udevadm settle --timeout=10
  python3 -B "$delivery_root/tools/native-runtime-host-preflight.py" \
    --expected-module-version 3.10.20 \
    --expected-module-srcversion "$native_srcversion"
)
```

The driver must create a genuine CrystalHD video node with matching PCI/sysfs
identity; never manufacture a node. Host preflight reads metadata and returns
`PREREQUISITES_PRESENT_UNVERIFIED` when those checks pass; it does not open the
device or establish media/ABI/display acceptance. The preserved legacy module
remains selected on disk during temporary `insmod`.

### Native playback scope and recovery

The measured route mounts the selected slot's `lib` at `/native-ffmpeg` with
the pinned App/Platform/GL, firmware and graphics/audio inputs. Its search path
is `/native-ffmpeg:/app/lib:/app/lib/samba:/app/lib/samba/private:/usr/lib/x86_64-linux-gnu:/usr/lib:/gl/lib`.
The stable native receipt must identify actual new-module EOS/reopen/PTS/lifetime
checks separately from reused native-path evidence. The retained observer and
namespace recipe in `evidence/rc2-native-evidence.tar`, under `harness/`, remains
historical qualification scaffolding, not a general desktop launcher. Files-only
selection and host preflight do not run that route or qualify another launcher.

After normal Quit and idle retirement, unload the temporary module and reload
the preserved installed legacy module with its saved options, following
[BRINGUP](BRINGUP.md#reload-an-idle-device). Do not force an unload or kill a
surviving owner to recover; preserve runtime and state for handoff. After all
consumers retire, the file-only removal may be repeated:

```sh
(
  set -eu
  python3 -B "$delivery_root/tools/native-runtime-slots.py" --store "$slot_store" status
  python3 -B "$delivery_root/tools/native-runtime-slots.py" --store "$slot_store" remove
)
```

`rollback` needs a previously installed update. Reusing the same RC3/stable inner
runtime does not create a second slot. The route stays optional, with broader
nonempty STOP→START, controls, equipment and physical-display limits in
[SUPPORT](NATIVE-RUNTIME-SUPPORT.md). Centered uses the ordinary VA route and
does not require this companion.

<a id="rc4-companion-layout-and-use"></a>

## Frozen RC4 reference

The frozen RC4 candidate is `1.3.0-rc.4` / module19, source `1c8b0451c631`.
Its [source-bound operator instructions](https://github.com/ahnhy1324/crystalhd/blob/1c8b0451c631bd93e3f0395dd3e528875b8ecfe9/NATIVE-RUNTIME-DELIVERY.md#rc4-companion-layout-and-use)
use `evidence/rc4-core-binding.json` and `rc4-native-qualification.json`.
Actual qualification and public availability belong to that candidate's
[release entry](https://github.com/ahnhy1324/crystalhd/releases/tag/v1.3.0-rc.4)
and separate receipt; the frozen source's preparation text is not a result.
Do not relabel its module19 observations as stable module20. The
[current 1.3 procedure](#stable-13-companion-layout-and-use) has separate
bindings and actual checks.

<a id="rc3-candidate-preparation"></a>
<a id="rc3-companion-layout-and-use"></a>

## Published RC3 reference

Published [RC3](https://github.com/ahnhy1324/crystalhd/releases/tag/v1.3.0-rc.3)
remains immutable: frozen core `36ef4c681f49`, module18/native srcversion
`FEB8DC48436425D92497DA7`, unchanged FFmpeg and 23-case native qualification.
Its complete [original operator guide](https://github.com/ahnhy1324/crystalhd/blob/v1.3.0-rc.3/NATIVE-RUNTIME-DELIVERY.md#rc3-companion-layout-and-use)
and `evidence/rc3-core-binding.json` / `rc3-native-qualification.json` belong to
that package. For the matching 1.3 package, return to the [current procedure](#stable-13-companion-layout-and-use).
Published RC2's original recipe follows without alteration.

## Frozen RC2 operator reference

The source940/module17 preparation, outer contents, preflight literal and
2026-11-02 measurements below describe the **published RC2**. They remain
reproducible and are not reclassified as RC3 results. Shared target/runtime pins
may be reused only within the RC3 receipt's declared unchanged scope.

## Before you start

The tested target is **x86_64 Core2 T8100 / BCM70015 / Intel965**, with
**Kodi22RC1 and the exact Platform/GL commits** listed under
[Acquire the exact target](#acquire-the-exact-target). You need a working
Flatpak session with its normal system/session DBus and private storage for
the pinned deployments and runtime. See the [support limits](NATIVE-RUNTIME-SUPPORT.md)
before using another host, codec or container; the delivered FFmpeg is a
configured subset whose full configuration is in `materials/source-materials.tar`.

Native-module preparation is separate. Extract the supplied
`materials/crystalhd-core-940-source.tar.gz` into a new private build directory
and build that frozen core source `940cf357` with `CRYSTALHD_V4L2=1`. Keep the
build separate from registered DKMS source trees; do not substitute master or
overwrite an existing package17 registration. The selected module must be
`3.10.17` with a genuine CrystalHD video node and matching PCI/sysfs identity.
Use the [current idle module-transition rules](BRINGUP.md#reload-an-idle-device)
for activation and restoration. The [route-probe observations](tools/kodi-route-probe/README.md#historical-native-activation-observations)
are historical evidence from older temporary modules, not the current recipe.
The ordinary stable module exposes no native video node; creating one by hand
or changing a setting cannot add V4L2 behavior.

Preserve the installed stable module and settings. Use normal idle module
operations with AC, temperature below75C, no owners and ref/pin zero. An owner
that does not retire requires preserving the live state for handoff; follow
[BRINGUP](BRINGUP.md#reload-an-idle-device). The runtime slot commands below
manage private files and do not activate or replace a kernel module.

## Contents and verification

The outer `crystalhd-native-runtime-1.3.0-rc.2.tar.gz` contains:

- `runtime/runtime-private.tar`: the frozen neutral prefix, seven shared-library
  families, ffmpeg/ffprobe, original metadata and notices;
- `materials/crystalhd-core-940-source.tar.gz`: complete unchanged own core
  source940 (separate source acquisition receipt included);
- `materials/source-materials.tar` and `materials/materials-manifest.json`:
  complete pinned FFmpeg source, notices and build configuration;
- `materials/ffmpeg-offline-git-metadata.tar.gz`: independent offline Git
  metadata needed by the unchanged build helper;
- `materials/dependency-materials.tar`: complete libdrm, libva, zlib-ng,
  glibc and freedesktop SDK source/recipe/patch snapshots with original notices;
- `tools/`: the producer/build helper, preflight, slot, host and observation
  tools, with their original per-file licenses;
- `evidence/`: two-build correspondence, fresh target playback, first-user
  costs and bounded command/log records; `licenses/`: GPL2 and LGPL2.1 texts.

Obtain the outer archive SHA-256 from its separate release receipt. Extract
into a new directory, then check every included file before use:

```sh
sha256sum -c SHA256SUMS
```

`delivery-manifest.json` binds the runtime, source companion, build proof and
every included file. Hashes authenticate agreement with the separate receipt,
not the identity of a publisher. Existing inner metadata and NOTICE retain the
historical `PUBLIC_NOT_READY` decision made before these later checks. The
outer evidence and scoped support policy supplement that frozen record; the
packager and files-only tools never grant release or runtime acceptance.

## Acquire the exact target

Use an ordinary working Flatpak session with its normal system/session DBus.
Use an isolated user installation or account when preserving another Kodi
installation. The clean-acquisition receipt records the actual separate empty
store and all commands. Install each ref with `--no-deps --no-related`, then
pin it with `flatpak --user update --commit=COMMIT REF`. `flatpak install`
has no `--commit` option. A current tip may be downloaded before the pinned
historical commit; that cost is included in the receipt.

For example, after adding the normal flathub/flathub-beta remotes to the
selected user installation, the exact Platform acquisition commands are:

```sh
flatpak --user install --noninteractive --no-deps --no-related flathub \
  runtime/org.freedesktop.Platform/x86_64/26.08
flatpak --user update --noninteractive --no-deps --no-related \
  --commit=e7f7b0fb20662fe92e4eac65d58a57019cefae45a39559c4b50440a88acc0491 \
  runtime/org.freedesktop.Platform/x86_64/26.08
```

Repeat for the GL and Kodi refs using their respective remotes/commits below.

| Ref | Commit |
| --- | --- |
| `app/tv.kodi.Kodi/x86_64/beta` (flathub-beta) | `0a381e42781914ae76ed338d3aa11f9403c55c57f5c9c194e1f4fb437cf71630` |
| `runtime/org.freedesktop.Platform/x86_64/26.08` (flathub) | `e7f7b0fb20662fe92e4eac65d58a57019cefae45a39559c4b50440a88acc0491` |
| `runtime/org.freedesktop.Platform.GL.default/x86_64/26.08` (flathub) | `25dbbb0cd3a63b2ef8a24ee948ad43998960f3e2bea713263742cc076840aec2` |

Do not use whichever deployment happens to be current. Set `platform_files`
to the pinned Platform deployment's `files` directory and record that identity.
The actual Kodi ELF SHA-256 is
`2862331a8a047f929d8019987a518527fcb45330082435bec7b7756141cf688f`.
The complete seven-provider inventory is checked by the following tools.

## Install, update and recover the private files

Run from the extracted delivery. Choose a new absolute store whose parent
already exists; keep the target files immutable while checking them.

```sh
slot_store=/absolute/private/native-slots
platform_files=/absolute/pinned/Platform/files
python3 -B tools/native-runtime-slots.py --store "$slot_store" install \
  --archive "$PWD/runtime/runtime-private.tar" \
  --archive-sha256 dc3d88bdeac5dc45a4f52800e1c653dcb35fcd73c14c158460974083f872c020 \
  --rootfs "$platform_files" --platform-relative . \
  --rootfs-identity 'Platform e7f7b0fb20662fe92e4eac65d58a57019cefae45a39559c4b50440a88acc0491' \
  --files-sha256 aee1eba5bc5d54bf54b6499ad3ffdb94ee8bd89015953c800804ce44e889c11d \
  --dependencies-sha256 dfbff250c9cd61dfc83beba99bffab30e2d7c2a3af0e3785d021d964feed6b02 \
  --provenance-sha256 7ffd06ad3b22fbf19ae5c4088f5c3d6418ed8dcaca1365957d4cd369d4ea781f
python3 -B tools/native-runtime-slots.py --store "$slot_store" status
```

Use the returned `active_prefix`; the FFmpeg library directory is its `lib`
subdirectory. A successful files report is not a playback report. The selected
module must expose a genuine CrystalHD video node with matching PCI/sysfs
identity. The ordinary stable module has no native video node. Check the
explicitly selected native module without opening it:

```sh
python3 -B tools/native-runtime-host-preflight.py \
  --expected-module-version 3.10.17 \
  --expected-module-srcversion DF1416C32049D4DADE81EB2
```

The recorded Kodi route mounts only the active slot's `lib` at
`/native-ffmpeg`, alongside the exact App/Platform/GL and required graphics,
firmware, Wayland and PipeWire inputs. Its library search is
`/native-ffmpeg:/app/lib:/app/lib/samba:/app/lib/samba/private:/usr/lib/x86_64-linux-gnu:/usr/lib:/gl/lib`.
It does not need the old `/native-lib` or `/probe/deps` mounts. The bounded
qualification recipe is retained in `evidence/fresh-target-evidence.tar`;
it is an observer, not a general desktop launcher.

For a later reviewed package, `update` accepts the same pinned arguments as
`install`; obtain its own hashes independently. Stop consumers before changing
selection. After at least one update, restore the previous retained slot with:

```sh
python3 -B tools/native-runtime-slots.py --store "$slot_store" rollback
python3 -B tools/native-runtime-slots.py --store "$slot_store" status
python3 -B tools/native-runtime-slots.py --store "$slot_store" remove
```

Rollback requires a previous slot. Removal affects only owned runtime files
and selection, preserves the store/lock, and can be repeated. It does not
change a running process or driver. Retire playback, restore the stable module
normally, and use the existing stable VA/GStreamer route for fallback. An
unretired owner requires preserving state for diagnosis. Use normal Quit, then
identity-verified TERM if needed; retain a surviving process and its runtime
for handoff. Follow [BRINGUP](BRINGUP.md#reload-an-idle-device) rather than a
kill escalation or forced module unload.

## Measured costs and boundaries

Successful fresh Ubuntu26 userspace acquisition/build/staging commands total
1145.99 seconds across preserved attempts. This is a command-time sum, not one
uninterrupted installation. The actual first prerequisite installation took
140.51 seconds; the later 2.98-second cache check is excluded. Retained APT
package bodies total406,673,418 bytes. A separate fresh Git-free core source
archive acquisition took3.142 seconds and8,644,373 HTTP body bytes; all911
included source members match commit940 (including exported source identity).
The original Git transfer and HTTP/TLS overhead are not reconstructed.

The separate empty Flatpak store took194.411 seconds across successful phases;
six transactions reported522,363,521 pull/extra-data body bytes, plus8076 bytes
of descriptor bodies. Preparatory summaries, headers and TLS/IP are excluded.
Its observed store occupied1,419,534,336 allocated bytes. Cold setup observed
2,362,372,096 allocated bytes before the Flatpak stores; transient sampling
cannot establish an exact instantaneous disk peak.

The cold build unit peaked at1,807,728,640 cgroup bytes and the clean Flatpak
unit at1,559,031,808 bytes, including file cache. These are observations, not
minimum RAM requirements. FFmpeg reproduction separately used the existing
host GCC15 environment: two builds took1198.082 and1202.392 seconds at50% of
one CPU, with a combined925,896,704-byte cgroup peak. No third-party sources
were patched. Details, command hashes, earlier failures and counting limits
are in `evidence/first-user-cost.json` and its evidence TAR.

Fresh target playback returned all180 source pictures twice, independently
matched source PTS, reached EOS/reopened and retired. A/V drift/stall logs were
clear; the stable module and36 runtime leaves were restored. Existing accepted
renderer/QoS losses remain nonblocking. Headless Weston GL and PipeWire null
sink results do not establish physical scanout/audio or ordinary desktop
acceptance. The fresh userspace shares the existing host kernel; an independently
booted OS is unavailable with the current non-disruptive equipment constraint
and remains explicitly untested.
