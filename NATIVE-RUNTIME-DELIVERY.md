# Experimental native runtime delivery for 1.3

Use this optional companion to evaluate the **native V4L2 route in Kodi** with
the matching 1.3 candidate and the exact target below. For ordinary local-file playback,
start with [installation](README.md#quick-start) and [everyday usage](USAGE.md).
The companion provides prebuilt minimal FFmpeg libraries; using them does not
require compiling FFmpeg. To reproduce those libraries, use the
[build recipe](NATIVE-RUNTIME-BUILDING.md).

**RC3 candidate:** `1.3.0-rc.3` combines the complete own native V4L2
producer with prebuilt, unmodified FFmpeg DRM_PRIME libraries. Its separate
core-binding and native-qualification receipts identify the actual candidate
source/module18 and runtime results. Existing runtime/source/build proofs do
not by themselves qualify changed own-core bytes; the release entry supplies
public availability and qualification status.

The published [v1.3.0-rc.2](https://github.com/ahnhy1324/crystalhd/releases/tag/v1.3.0-rc.2)
remains frozen with 13 assets, core `940cf357` and module-DKMS `3.10.17`.
Stable 1.3 promotion remains deferred. The ordinary source bundle supplies the
driver, GStreamer, VA, Player, settings, default-off Centered and experimental
outputs. The companion adds the separately licensed FFmpeg binaries and their
complete source/build materials; no external product patch is applied.

<a id="rc3-candidate-preparation"></a>

## RC3 companion layout and use

The outer archive is
`crystalhd-native-runtime-1.3.0-rc.3.tar.gz`. Its own core must be selected from
`materials/crystalhd-core-1.3.0-rc.3-source.tar.gz`, not RC2's source940 or a
moving master checkout. The native module identity is `3.10.18`.

| Item | RC3 candidate binding |
| --- | --- |
| Own core and native module | `evidence/rc3-core-binding.json` identifies the actual final core archive/revision, module identity and packaged own tools/guides. |
| Native candidate acceptance | `evidence/rc3-native-qualification.json` records the actual changed-source candidate playback, PTS/picture/EOS/reopen/lifetime and restoration results. They are not inherited from module13 observations or RC2. |
| Reused unchanged runtime | `runtime/runtime-private.tar` and its exact provider/metadata pins remain unchanged; `evidence/build-proof.json` retains the two reproduced builds. `evidence/reuse-scope.json` distinguishes this reuse from new core qualification. |
| Original RC2 costs and native evidence | Explicitly named `evidence/rc2-first-user-cost.json`, `rc2-first-user-evidence.tar`, `rc2-native-qualification.json` and `rc2-native-evidence.tar`; their dates, old module identities and original outcomes remain intact. |
| External source and notices | The complete FFmpeg/dependency/SDK materials and offline Git companion keep their original bytes, revisions and licenses. Historical inner `PUBLIC_NOT_READY` metadata is not rewritten. |

Use the matching candidate archive and its qualification receipt:

1. Verify the outer release checksums, extract into a new directory and inspect
   `delivery-manifest.json` plus `evidence/rc3-core-binding.json`.
2. Extract the RC3 own core into a separate private build directory and build
   with `CRYSTALHD_V4L2=1`. Preserve installed stable/RC2 modules and registered
   source; use the existing [idle transition rules](BRINGUP.md#reload-an-idle-device)
   for activation and restoration. The candidate must expose a genuine
   CrystalHD video node, not a manually created substitute.
3. Acquire the exact App/Platform/GL target below. Use the unchanged slot
   archive/metadata arguments in [private file installation](#install-update-and-recover-the-private-files)
   only when the candidate's reuse receipt confirms those pins.
4. Run the host preflight with expected version `3.10.18` and the **RC3** module
   srcversion recorded by its candidate receipt. The literal module17 preflight
   example below is retained RC2 history and is not an RC3 command.
5. Select the active private FFmpeg library directory as documented below.
   Check the candidate's actual native receipt and retire consumers before
   changing/removing a slot; preserve a surviving owner for handoff.

No external Kodi/FFmpeg replacement patch is required. The native route stays
optional; broader nonempty STOP→START, controls, equipment and physical-display
limits remain in [SUPPORT](NATIVE-RUNTIME-SUPPORT.md). Centered is separately
available through the ordinary VA route and does not require this companion.

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
