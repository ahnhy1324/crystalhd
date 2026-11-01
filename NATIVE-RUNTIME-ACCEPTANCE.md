# Compilerless native runtime: retained preparation and #607 follow-up

The compatible-host private installation scope of
[#539](https://github.com/ahnhy1324/crystalhd/issues/539) is complete: it can
activate the measured native Kodi route without compiling FFmpeg at activation.
[#607](https://github.com/ahnhy1324/crystalhd/issues/607) owns the remaining
clean-target and public-distribution deliverables and remains **OPEN /
PUBLIC_NOT_READY**. Independent clean-target acceptance and complete first-user
burden retain their separate unqualified scopes. The current-host native cases
below have their own package, module and media boundaries.

The historical private `1.3.0-dev.1` source/UI checkpoint completed deterministic
packaging, fresh Git-free build and compatible-host private-staging install/remove/reinstall at
source `c48fe3a12d4f0443dc444df27c0b6c35be2b3ced`; see the
[qualified checkpoint](RELEASING.md#private-v13-candidate-preparation) and
[exact receipt](tests/fixtures/issue510/v13-private-admission-qualification-20261101.json).
Its optional stock-Kodi settings ZIP uses the existing VA route. This milestone
does not bundle or qualify the seven-DSO native FFmpeg research runtime below,
or qualify the public native-runtime/clean-target deliverables in #607.

The files-only preflight below checks the existing #576 artifacts. The separate
metadata/software observers execute trusted libraries in a device-free namespace;
the private slot tool manages files and a selection JSON. None builds/modifies
FFmpeg or Kodi, changes the stable stack or publishes binaries. Each recorded
result below retains its exact source, target and execution scope.

## Existing package selection

The retained statically integrated Flathub stable package is already classified
by the own embedded-metadata probe. Its signed app commit is `9d26e518`, Kodi
21.3, ELF SHA256
`6754d32f8b6b6501b247a7fc817c2b6eb151c27ca5380abd4f2115163ed0a1d6`.
The [exact historical receipt](tests/fixtures/issue539/flathub-stable-embedded-hwconfig-20261028.json)
records a present `h264_v4l2m2m` decoder with zero hardware configurations,
strict exit1, and no Kodi executable initializers, main, media or device run.
Dependency initializers may run. The earlier DSO-only exit2 remains a different
unsupported-inspection result, not a decoder-negative measurement.

The source-bound embedded probe, preload helper and DSO probe are available in
`tools/kodi-route-probe/`. Their [guide](tools/kodi-route-probe/README.md) separates
trusted mapped-target metadata execution from files-only preflight. Tests use
synthetic executables/DSOs; no new real-package playback follows from them:

```sh
make kodi-package-check
make runtime-preflight-check runtime-slots-check
```

The preflight report explains recorded ABI/GLIBC/configure-CPU requirements
and the pinned provenance's distribution blockers. Missing fields remain
unknown and positive declarations remain unverified; its status stays
`PASS_PINNED_TARGET_FILES_ONLY` / `PUBLIC_NOT_READY`. These diagnostics do not
recompute symbol resolution, infer a minimum CPU or approve redistribution.

## Minimum compatibility plan

1. Identify the selected executable and its integration before interpreting
   metadata. Use direct FFmpeg SONAMEs for a DSO-linked Kodi, or the reviewed
   main-image exports and exact ELF identity for an embedded build. Distinguish
   an inspection error, a completed zero-config negative and a metadata
   candidate; decoder names and device nodes alone do not establish selection.
2. Apply the target Kodi's rule. The embedded helper pins the original Kodi21
   and Kodi22 source policies separately: both require DRM PRIME, while the
   HW_DEVICE_CTX device-type handling differs. The exact source commits/paths
   are recorded in the probe and receipt.
3. Require real delivery and lifetime semantics after selection. The unchanged
   published [FFmpeg implementation at12cc1f42](https://github.com/jc-kynesim/rpi-ffmpeg/tree/12cc1f42f06ff0e85633c2602c96786fbd280b70)
   supplies hardware-config/get-format, EXPBUF/AVDRM descriptors and
   FD/context/consumer-buffer lifetime handling. An advertisement-only change
   cannot supply those contracts. No external source/binary patch is part of
   this plan, and a minimal upstream patch remains unproved.
4. Keep the matching seven-DSO closure and target providers together. Exact
   SONAMEs, ABI, versioned symbols, CPU options and provider bytes matter;
   replacing only libavcodec is not a qualified compatibility route. Stage
   only a pinned package before selecting its private prefix and keep the old
   slot for file-level rollback.
5. Separate file/provider validation and device-free metadata/software CLI
   checks from each new package's actual Kodi selection, source-qualified
   no-drift A/V logs, control/EOS/reopen and retirement acceptance. A new
   package does not inherit the older private-prefix native playback result.
   Independent clean-target and public source/notices/maintenance decisions
   remain with #607.

## Private slot selection and cost records

`scripts/native-runtime-slots.py` stages an explicitly pinned uncompressed TAR,
checks its files/providers, retains the previous slot and replaces one selection
JSON under an operator-owned private store. `status` returns the selected prefix;
it does not change a Kodi launcher, loader search path, application profile or
already-running process. Use immutable inputs and offline consumers. This is a
private files-only lifecycle tool, not a public native-runtime installer.

The store must be new or empty for its first `install`. Subsequent `update`
uses the same pins/options for the new archive. Expected pins must come from
the reviewed producer/receipt, not be silently taken from a failing target:

```sh
python3 -B scripts/native-runtime-slots.py --store "$slot_store" install \
  --archive "$runtime_archive" --archive-sha256 "$archive_pin" \
  --rootfs "$platform_files" --rootfs-identity "$platform_identity" \
  --platform-relative . --files-sha256 "$files_pin" \
  --dependencies-sha256 "$dependencies_pin" --provenance-sha256 "$provenance_pin"
python3 -B scripts/native-runtime-slots.py --store "$slot_store" status
python3 -B scripts/native-runtime-slots.py --store "$slot_store" rollback
python3 -B scripts/native-runtime-slots.py --store "$slot_store" remove
```

Bad candidates leave the previous selection intact. Rollback rechecks the old
package and target providers. Removal checks the complete owned payload tree
independently of provider availability, preserves unrelated store-top user files,
and tracks/retries pending owned deletion after caught cleanup failure. Unknown
or changed payload files are refused. Initialization and publication failure
controls cover retry, interrupts and lock/store identity replacement. Atomic
selection is not a power-loss, cross-filesystem or live-consumer update guarantee.

Reports include total wall time, phase timing, logical/allocated store snapshots
and own-Python-process RSS high-water. The enclosing serialized-operation timing
overlaps child phases and must not be summed with them. Disk is an observed
store-local maximum, and RSS is not unique physical RAM or whole process-tree
peak. First-user network acquisition, full Kodi/Platform/GL setup and historical
build RAM remain unmeasured. Cached local trial timings are not cold-install
or generic-machine performance claims.

The [current-host receipt](tests/fixtures/issue607/package-acceptance-20261101.json)
records the old private-prefix package and a separately inventoried neutral
package assembled from retained, unmodified `/usr`-prefix binaries. Nine ELF
payload hashes differ. It covers rejected incomplete update, valid update,
selected-prefix metadata, previous-slot rollback, repeated removal and user
preferences preservation. The neutral package also passes independent software
H.264/NV12/PTS checks for50 frames. That receipt covers files/software only;
the later native activation results below are separate. Independently booted
clean-OS acceptance and public distribution remain unqualified.

## Native host prerequisites

`scripts/native-runtime-host-preflight.py` reads bounded sysfs identity fields
and device filesystem metadata. It never opens a device, issues an ioctl or
changes a module. A candidate needs a real character `videoN` node with the
sysfs major/minor and a matching CrystalHD PCI driver/module association.
Module version strings and node presence do not qualify QUERYCAP, M2M,
STREAMING, EXPBUF, ABI, decoding or Kodi playback.

The old native activation receipt used temporary module `3.10.14`, srcversion
`E3D1535AB4C470BC21D6E32`. Check those explicit expectations with:

```sh
python3 -B scripts/native-runtime-host-preflight.py \
  --expected-module-version 3.10.14 \
  --expected-module-srcversion E3D1535AB4C470BC21D6E32
make runtime-host-preflight-check
```

Exit2 means blocked prerequisites; exit0 means only
`PREREQUISITES_PRESENT_UNVERIFIED`. Neither exit code supplies native Kodi/ABI/
A-V/hardware acceptance; actual trials are recorded separately below.
`PUBLIC_NOT_READY` remains in force. Alternate roots are declared offline/test
inputs, not authentication of a live target.

The stable baseline module `3.10.13/F76E163690F147B91495DDD` exposes
`/dev/crystalhd` but no native video node. The stable V4L2 frontend registers
parent lifetime only; the separate native research implementation registers
the decoding video node. The unchanged FFmpeg implementation requires the
video-node file operations and V4L2 ioctls. A setting or filesystem node cannot
supply missing kernel behavior. This baseline observation describes that loaded
configuration; it does not establish that native verification is impossible.

An ordinary guarded idle transition to the pinned temporary native `3.10.14`
module is allowed under [BRINGUP's idle reload procedure](BRINGUP.md#reload-an-idle-device).
Close players and diagnostics, verify no device owners and reference count 0,
save the exact loaded/installed module identities and options, and use normal
unload/load operations. The temporary module supplies the observed `/dev/video0`
native decoding node. After each bounded trial, close clients normally and
restore the exact stable module/options and baseline device state, checking
the restoration separately. Temporary activation does not require replacing
installed module, firmware, Kodi or FFmpeg files. Stop if normal unloading is
busy or fails; forced unload and killing a stuck owner for recovery remain
prohibited. Missing-node baseline observations do not prohibit this guarded
activation and restoration.

## Current-host neutral-package native observations

The retained neutral archive
`dc3d88bdeac5dc45a4f52800e1c653dcb35fcd73c14c158460974083f872c020`
has now been used in native Kodi trials on T8100/BCM70015/Intel965 with the
temporary native module and subsequent stable restoration. The observed
renderer-return census is:

| Pinned media case | Source frames per play | Plays | Renderer acceptances |
| --- | --- | --- | --- |
| 1280x720 short EOS/reopen | 180 | 2 | 360/360 |
| 1920x1080 short EOS/reopen | 180 | 2 | 360/360 |
| 1280x720 long pause/reopen, original observation | 900 | 2 | 1798/1800 (900 + 898) |
| Same long case, linked read-only diagnostic | 900 | 2 | 1799/1800 (899 + 900) |
| Same diagnostic with verified resume progress | 900 | 2 | 1798/1800 (899 + 899) |

All five cases match source-derived caller and returned PTS exactly for all
6120 decoded frames. Full rendering still fails in each long-case pair.
The read-only diagnostic links three newly observed missing submissions to
`WaitForBuffer=-1`, `OutputPicture=AGAIN`, then a fresh successful `GetPicture`
replacing the pending native picture before `AddVideoPicture`. It identifies
that consumer call path without assigning an exclusive external defect.

The comparison harness also waited at least200ms and required actual renderer
and media-clock progress after every resume; all six progress gates passed,
but the 899+899 census still fails. Missing resume verification was corrected
in the own harness; the hypothesis that spacing alone repaired rendering was
rejected. The original failures and raw traces remain preserved.

All five cases selected the AAC clock. Configured source-qualified drift/stall
checks found no matches. Audio/video renderer timeouts are retained separately
from that log-scoped A/V result and do not resolve the frame deficit. Saved live mappings identify all six
actually mapped FFmpeg library families as exact neutral-package files, and all seven selected runtime
providers match the pinned Platform files. The seven-DSO package file/metadata
closure remains a separate check from the libraries mapped by a Kodi session.
These observations do not complete #607, D3/D5, independent clean-OS or public
distribution acceptance. The [portable receipt](tests/fixtures/issue607/neutral-native-activation-20261101.json)
binds package/module inputs, individual axes, diagnostic controls and exact
stable/dependency restoration for all three sessions (maximum60°C, kernel checks0).

## Offline source/caller/returned PTS oracle

`tools/kodi-route-probe/native-pts-oracle.py` computes expected values from
separately pinned source packet ticks/time_base. Its model is pinned to Kodi
`28ea2eac1eb7af8fdcbd2672d933ce87f594be79`, following the published
[demux conversion](https://github.com/xbmc/xbmc/blob/28ea2eac1eb7af8fdcbd2672d933ce87f594be79/xbmc/cores/VideoPlayer/DVDDemuxers/DVDDemuxFFmpeg.cpp#L986),
[DRM PRIME caller conversion](https://github.com/xbmc/xbmc/blob/28ea2eac1eb7af8fdcbd2672d933ce87f594be79/xbmc/cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodecDRMPRIME.cpp#L406)
and [timing constants](https://github.com/xbmc/xbmc/blob/28ea2eac1eb7af8fdcbd2672d933ce87f594be79/xbmc/cores/VideoPlayer/Interface/TimingConstants.h#L12).
It preserves binary64 operation order, then truncates to the caller's int64:

```text
seconds = double(packet_pts) * time_base.num / time_base.den
demux_pts = seconds * 1000000.0
caller_pts = int64(demux_pts / 1000000.0 * 1000000.0)
```

The supported domain is one H.264 video stream with nonnegative packet ticks
and zero-origin format/stream clocks. Comparison tolerance is zero. Expected
values come from source metadata and this pinned model; returned observations
cannot supply their own expected values. Exact rational conversion is reported
only as a diagnostic because it can differ from the actual binary64 caller.

The offline result checks complete accepted PTS in packet order and returned
PTS in presentation order for every cycle. Its separate `renderer_census`
reports full, deficit, excess or unknown counts. A PTS match can coexist with a
failed native report or renderer deficit; it does not promote either result.
With `--require-renderer-census`, an otherwise matching PTS result exits3 for
renderer deficit, excess or unknown; a complete census exits0. Input/PTS failure
remains exit2. This strict count gate still supplies no pixel/scanout or whole
runtime qualification. Actual short reports exit0 and all three failed long
case pairs exit3. All 21 CPU controls pass, including these cases, without opening a device or executing media. Use
`make -C tools/kodi-route-probe check-native-pts` for those controls.
This oracle qualifies PTS comparison only, not rendered pixels, scanout, A/V,
hardware, independent OS or distribution readiness.

## Private source and build-materials companion

`scripts/native-runtime-materials.py` exports exact regular Git blobs and
executable modes from an explicitly pinned clean commit. It combines these
with pinned package metadata/notices and individually selected recipe,
configuration, toolchain or acquisition files. It reads the selected inputs;
it does not build external code, execute a recipe or change the checkout.
The binary archive is SHA-pinned separately and is not embedded.

The reviewed materials spec has schema1 and a `materials` array. Each entry
contains an absolute input `path`, an `archive_path` under `materials/`, its
`sha256` and a `role`. Recipe and configuration roles are required. Keep pins
from the reviewed producer/input inventory rather than deriving replacements
from a failed target. The output must be a new directory outside the source
and binary package roots:

```sh
python3 -B scripts/native-runtime-materials.py \
  --source-checkout "$source_checkout" --source-revision "$source_commit" \
  --package-root "$package_stage" --package-archive "$runtime_archive" \
  --expected-archive-sha256 "$archive_pin" \
  --expected-files-sha256 "$files_pin" \
  --expected-dependencies-sha256 "$dependencies_pin" \
  --expected-provenance-sha256 "$provenance_pin" \
  --materials-spec "$reviewed_spec" \
  --expected-materials-spec-sha256 "$spec_pin" --output-dir "$new_output"
make runtime-materials-check
```

Outputs are `source-materials.tar` (uncompressed USTAR),
`materials-manifest.json` and `SHA256SUMS`. Local input paths and mtimes do not
define archive member identity. Historical configuration/log bytes may still
contain their original paths. No `.git`, untracked/ignored files or runtime
binary archive is exported. The retained own recipe expects an exact Git
checkout; the exported source snapshot has no Git metadata and is not a
drop-in checkout for that helper.

This is reproducible packaging of selected source/material bytes. It does not
establish reproducible compilation, attestation that the old binaries came
from those inputs, dependency-source completeness, redistribution review,
maintenance ownership or runtime acceptance. Frozen package provenance is
copied unchanged, including its original source-archive declaration. A separate
companion does not silently rewrite that historical declaration.
`PUBLIC_NOT_READY` remains in force.

The [source/material execution receipt](tests/fixtures/issue607/source-materials-20261101.json)
records14 host-observer controls and25 source/material controls. Its earlier
default and explicitly pinned observations recorded the stable baseline's
missing native video node, before the guarded temporary activation described
above. Two private exports contain all10,480 source blobs /91,065,090 source
bytes; all three output files are byte-identical. The uncompressed companion
archive is100,249,600 bytes. Independent Git-blob/mode and complete TAR/manifest
checks cover every source/member. This is source/material packaging evidence,
not new native playback or reproducible-compilation qualification.

## Reuse the implemented package and runtime checks

The current repository also carries the three metadata/software observer
workers under `tools/kodi-route-probe/`; the packager itself remains the pinned
#576 tool described below. The observer now distinguishes a visible declared
neutral `/usr` prefix from a leaked private build prefix. All seven FFmpeg
libraries must still map from the exact relocated package. A visible `/usr` is
reported as visible, not hidden, and does not approve neutral/reproducible builds.

The reviewed #576 source is `ec946ac6cc5a78ce552911510cf89a651cf19e5a`, merged
only into `research/issue539-runtime-verified-20261029` at `c65af5e`. Its
[package guide](https://github.com/ahnhy1324/crystalhd/blob/ec946ac6cc5a78ce552911510cf89a651cf19e5a/tools/kodi-route-probe/README.md#private-runtime-packaging-and-dependency-audit)
and four helpers already implement:

| Helper in that research tree | What it proves when executed successfully |
| --- | --- |
| `runtime-package.py pack` / `verify` | Explicit seven-DSO/two-CLI allowlist, relative SONAME links, source/notice inventory, deterministic packaging, exact pinned-provider ELF version/strong-symbol closure; no host library search. |
| `runtime-package-namespace.py` + `runtime-package-metadata.py` | Device-free bubblewrap namespace, inspected build tools absent, actual library metadata calls, exact seven mappings, original prefix absent. Existing kernel remains shared. |
| `runtime-package-namespace.py` + `runtime-package-cli.py` | Actual relocated CLI startup and software H.264/NV12 decode against frozen independent bytes and clock/metadata. Requires all four retained inputs. |

The [package receipt](https://github.com/ahnhy1324/crystalhd/blob/ec946ac6cc5a78ce552911510cf89a651cf19e5a/tests/fixtures/issue539/runtime-package-20261029.json)
records two byte-identical **8,028,160-byte** USTAR archives, nine payload ELFs,
seven external providers and **1,941 strong-symbol requirements**. The
[actual CLI receipt](https://github.com/ahnhy1324/crystalhd/blob/ec946ac6cc5a78ce552911510cf89a651cf19e5a/tests/fixtures/issue539/runtime-cli-20261029.json)
records two relocated prefixes, each with 50 exact frames / 69,120,000 NV12
bytes, complete frame hashes and original PTS 133200–309600 at 1/90000.
The first collector FAIL, corrected run and missing-libavcodec failures remain
separate. Package-byte reproducibility is not reproducible FFmpeg compilation.

## Offline preflight and provenance pins

The [2026-10-31 execution receipt](tests/fixtures/issue539/clean-target-preflight-20261031.json)
records17 device-free control methods and an actual pinned-files inspection of
the retained package/Platform:24 members,9 payload ELFs,7,931,560 payload bytes
and4,205,312 provider bytes. `make runtime-preflight-check` also runs with the
ordinary full check target. This is files-only evidence on the existing host;
independently clean OS/Kodi/GPU/hardware/A/V acceptance remains NOT RUN.

`scripts/native-runtime-preflight.py` accepts an extracted private package and
a separate immutable rootfs or Platform tree. It checks reviewed SHA-256 pins
before trusting the package manifests, verifies every member/hash/mode/link,
matches every recorded ELF payload/provider to its exact bytes, checks actual
ELF class/machine headers and rejects known compiler/build-tool names including
versioned/cross-compiler variants at the inspected target paths. It does not
recompute symbols or load a library. Exact matching bytes reuse the pinned
#576 static inventory; the rootfs identity string is a declaration, not an
authentication claim. The package/rootfs must stay immutable during inspection.

The pinned final local stage has these metadata hashes. Dependency/provenance
bytes match the committed #576 fixtures; these pins are not a package signature.
Never derive replacement pins silently from a target which failed validation.

| File | SHA-256 |
| --- | --- |
| `metadata/files.json` | `8f5fa705cdcdae7c5e6a99263cc3178024d99ff96760b8c4b687170357206a7c` |
| `metadata/dependencies.json` | `bca5916dfc36b91315343b5987caf5cd71bd0c4fe837eae203bc2666d75742c5` |
| `metadata/provenance.json` | `6266b9d0a8bfe6624c236d871f9a16e00feb7c6dc19cd1160e69099d6bd51213` |
| Retained private USTAR archive | `832dbfc64a1e0fef3e9db2061bde28f267a22a293159552416dff3b3ae3d8053` |

For an extracted full rootfs, providers default to its `usr/` subtree:

```sh
python3 -B scripts/native-runtime-preflight.py \
  --package /absolute/path/to/fresh-private-stage \
  --rootfs /absolute/path/to/offline-rootfs \
  --rootfs-identity 'exact image/package identity and acquisition receipt' \
  --files-sha256 8f5fa705cdcdae7c5e6a99263cc3178024d99ff96760b8c4b687170357206a7c \
  --dependencies-sha256 bca5916dfc36b91315343b5987caf5cd71bd0c4fe837eae203bc2666d75742c5 \
  --provenance-sha256 6266b9d0a8bfe6624c236d871f9a16e00feb7c6dc19cd1160e69099d6bd51213 \
  > /new/operator-evidence/preflight.json
```

For the already retained Platform `files/` tree, select that directory as
`--rootfs` and add `--platform-relative .`. The expected runtime identity is
`org.freedesktop.Platform/x86_64/26.08`, retained OSTree identifier
`e7f7b0fb20662fe92e4eac65d58a57019cefae45a39559c4b50440a88acc0491`.
`PASS_PINNED_TARGET_FILES_ONLY` explicitly retains clean-OS/Kodi/GPU/media/A/V
as `NOT_RUN`, and `PUBLIC_NOT_READY`. A changed provider fails this exact-target
check even if it might be ABI-compatible. A genuinely different target needs
its own reviewed `runtime-package.py pack` dependency inventory, pins and
runtime execution; do not lower the check to a SONAME-only comparison.

Development controls, requiring only Python and inert synthetic ELF headers:

```sh
python3 -B tests/native-runtime-preflight.test.py
```

They cover mismatched pins, missing/changed providers, spoofed ABI metadata,
mode changes, extra/duplicate/traversal paths, escaping/cyclic links, compiler
variants, FIFO refusal and explicit unqualified runtime/public status. They
are file-inspection controls, not loader/ABI acceptance. No test execution is
claimed merely by providing the harness.

## Concrete existing operator paths

These are local retained artifacts on this development machine, not public
downloads or a supported installation layout:

| Purpose | Existing path |
| --- | --- |
| #576 source helpers | `/home/mache/.cache/crystalhd-kodi-runtime-package-20261029/tools/kodi-route-probe/` |
| Final package stage | `/home/mache/.cache/crystalhd-runtime-package-proof-20261029/package-c/stage` |
| Retained private archive | `/home/mache/.cache/crystalhd-runtime-package-proof-20261029/package-c/runtime-private.tar` |
| Already extracted relocation cases | `/home/mache/.cache/crystalhd-runtime-package-proof-20261029/fresh extracted 1` and `fresh extracted 2` |
| Compatible Platform tree | `/home/mache/.cache/crystalhd-player-accounting-20261025-6o89Y6yE/issue531/flatpak-user/runtime/org.freedesktop.Platform/x86_64/26.08/e7f7b0fb20662fe92e4eac65d58a57019cefae45a39559c4b50440a88acc0491/files` |
| Frozen software input | `/home/mache/.cache/crystalhd-single-field-20261015.boremY/h264-tff-1280x720-2s.ts` — SHA-256 `fbfe72e9246453d3900887d4a1444fb801fb388d7fa4ee8f58990305271001a6` |
| Frozen independent NV12 reference | `/home/mache/.cache/crystalhd-single-field-20261015.boremY/tff720-software-reference.nv12` — SHA-256 `82dcc068cff510d6086e15df0ff243c7d18e607d3244c4218289f2127029db91` |
| Original worker oracle | `/home/mache/.cache/crystalhd-runtime-package-proof-20261029/cli-reference/reference.json` — SHA-256 `8b2a1097fed435b8e05c21a573c5536ed2f7642444fb89d65a4165b95ce424b8` |
| Independent decoded clock/metadata | `/home/mache/.cache/crystalhd-runtime-package-proof-20261029/cli-reference/independent-ffprobe8.json` — SHA-256 `9d35e0ec6ca17b4678a5a38493ad7637afc26d34fa3bc45623c3ceb5c9823479` |

Inspect/fingerprint selected inputs before execution. In the #576 source tree,
the existing metadata command is:

```sh
python3 -B tools/kodi-route-probe/runtime-package.py verify \
  --package "$package_stage" --platform-root "$platform_files"
python3 -B tools/kodi-route-probe/runtime-package-namespace.py \
  --package "$package_stage" --platform-root "$platform_files" \
  --mount-prefix '/opt/clean target candidate'
```

`package_stage` and `platform_files` denote separately selected absolute paths
from the table. Bubblewrap, namespace permission and Python inside the target
are prerequisites. Docker/Podman are not needed. The current host has
`bwrap`, `unshare`, `chroot`, `debootstrap` and `qemu-i386` available, as
reported by the operator; availability is not a new namespace/target PASS.
These namespaces use the existing host kernel. A fresh debootstrap/chroot
would establish a separately acquired userspace only, not an independently
booted clean OS, native-i386/Atom or another GPU.

For software media validation, append all of:

```sh
  --software-media "$frozen_input_ts" \
  --software-oracle "$retained_reference_json" \
  --software-clock-reference "$independent_ffprobe8_json" \
  --software-nv12-reference "$retained_reference_nv12" \
  --output-dir /new/operator-evidence/software-cli
```

The public receipt's reference projection is not automatically interchangeable
with the original worker input. Use the exact retained input hashes/shape
listed in the actual CLI receipt. Missing retained raw inputs are a **NOT RUN**
prerequisite, not permission to regenerate a convenient golden from the tested
FFmpeg binary.

## Remaining work and constraints

| Deliverable | Next concrete evidence | Current disposition |
| --- | --- | --- |
| D0 official package selection | Reuse #545's `ffmpeg-hwconfig-probe.py` for a directly linked Kodi ELF and its exact FFmpeg closure; require `--require-kodi-drm-prime`. The embedded probe applies reviewed Kodi21/22 policies to exact main-image identities. | Ubuntu/Debian sid tested metadata negatives are retained. The source-bound Flathub stable21.3 embedded result is a completed zero-config metadata negative; its old DSO-probe exit2 remains an unsupported-inspection result. Ubuntu has no supplied Kodi i386 package in the recorded inventory. |
| D3 complete first-user burden | On the selected fresh target, record exact commands, network bytes, installed/temporary/peak disk, peak RSS, elapsed/user CPU, prerequisites, update and rollback work; separate observer tools from user dependencies. | Cached current-host install/update/rollback/remove costs and process RSS are recorded separately above. Prior configure/build/stage17.88 minutes excludes fetch/tool setup; peak build RAM is unknown. Full acquisition and Kodi/Platform/GL/driver/source setup remain unmeasured. |
| D4 independent clean-target acceptance | Run the pinned-manifest/ELF/provider helper against a genuinely independent clean target if one becomes available. Record any narrower clean-userspace proof under its actual scope. | Existing compatible-host, compilerless-namespace, software-CLI and FILES_ONLY results do not establish a clean OS or another native Kodi target. Current equipment remains T8100/BCM70015/Intel965 x86_64; no Atom/BCM70012 is connected. |
| D4 each proposed new package | Pin original Kodi/App/Platform/GL/own-driver identities; qualify activation without a compiler, decoder selection, source-qualified no-drift A/V logs, pixels/PTS, EOS/reopen, pause/seek/Stop, ABI/provider behavior, removal/fallback/re-extraction and resource boundaries. | Guarded temporary native activation and exact stable restoration now exercise the retained neutral archive. Both short EOS/reopen cases have complete renderer counts; three long pause/reopen pairs each return1800/1800 source frames but have1798/1799/1798 renderer acceptances. Linked diagnostics identify pending-picture replacement before renderer submission; verified resume progress alone does not repair it. AAC-clock drift/stall checks are clear; paused output timeouts remain separate diagnostics. Remaining acceptance is not inherited from those scoped results; calibrated physical lip-sync is not an additional synchronization gate. |
| D5 redistribution/maintenance | Neutral build paths and reproducible build recipe, corresponding-source/component/dependency notices, exact CPU/ABI support, update/security owner and explicit distribution decision. | Private source/build-materials companion packaging is complete. Core2/x86_64 and recorded `atan2f@GLIBC_2.43` remain; reproducible compilation, historical binary/source attestation, dependency-source/notices sufficiency and maintenance ownership remain unverified/unassigned. **PUBLIC_NOT_READY**. |
| Minimum credible compatibility plan | Document selection metadata, real EXPBUF/AVDRM descriptors and FD/context/consumer lifetime requirements from existing published implementations. | Advertisement-only own-driver/device changes cannot create missing FFmpeg selection metadata. The minimum compatibility plan above documents these published selection/delivery/lifetime requirements. External Kodi/FFmpeg edits are outside scope; an upstreamable minimum patch remains unproved and is not required to record the package gap. |

The private runtime is a minimal configured subset, not a drop-in FFmpeg
distribution. Replacing only `libavcodec` does not establish the matched ABI,
codecs, demuxers or graphics environment. Nonempty same-DPB STOP→START remains
unsupported (`-EOPNOTSUPP`); destructive seek recovery does not repair that
contract. No release/support/default or public-package promotion is made here.
#538's shutdown heap-abort subtask remains ignored/wontfix and is not included
in these preparation or acceptance commands.
