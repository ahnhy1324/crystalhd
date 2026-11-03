# Kodi FFmpeg metadata and native PTS inspection

Use the [native runtime delivery guide](../../NATIVE-RUNTIME-DELIVERY.md) to
install the published companion, or the [ordinary Kodi guide](../../USAGE.md#kodi)
for VA-API playback. This page is for maintainers inspecting package metadata,
source/caller PTS and retained diagnostic evidence.

These own diagnostic tools inspect a trusted Kodi package's FFmpeg decoder
metadata and compare source/caller/returned PTS for #607. They do not install a
decoder, change backend selection, or qualify the private native FFmpeg runtime
for distribution.

For the separately published 1.3.0-rc.2 native companion and its qualification,
use [delivery](../../NATIVE-RUNTIME-DELIVERY.md) and
[acceptance](../../NATIVE-RUNTIME-ACCEPTANCE.md). Fixture verdicts here describe
the individual probes, not the current companion's publication status.

## Device-free checks

```sh
make -C tools/kodi-route-probe check-capability
```

The checks build synthetic DSOs and Kodi-shaped executables. They cover decoder
absence, zero configurations, DRM PRIME policy differences, package identity,
ABI/origin checks, environment isolation and the before-main execution boundary.
They do not start a real Kodi playback session or open a video/CrystalHD device.
The minimal build here contains only the metadata helper and these checks;
research add-ons, FFmpeg builds and runtime packaging are separate work.

## Shared-library and embedded metadata

`ffmpeg-hwconfig-probe.py` handles a Kodi ELF that directly needs one each of
`libavcodec.so.N`, `libavformat.so.N` and `libavutil.so.N`. It loads and verifies
the exact selected DSO set, then calls its metadata APIs. Missing DSOs or an
unsupported integration return an inspection error; they do not establish a
missing decoder.

`ffmpeg-embedded-hwconfig-probe.py` handles reviewed glibc Kodi executables with
main-image FFmpeg exports. It verifies the target identity and uses the own
preload helper to query the embedded metadata. This **executes mapped trusted
package code**: dependency initializers may run before the helper. The helper
exits before Kodi executable initializers and `main`, and calls no media or
device API. This is not a files-only inspector or a sandbox. A matching isolated
runtime and explicit device-free mount boundary are needed for a real package
observation; the optional bwrap prefix is supplied by the operator.

An embedded candidate verdict requires a reviewed selection policy, matching
SHA-256/Build ID, the Kodi major-version cross-check, and a supported FFmpeg ABI.
Unknown or unsupported targets remain unclassified. A decoder publishing zero
hardware configurations is a completed metadata negative, distinct from an
inspection error. Strict mode returns 1 for no candidate and 2 for an incomplete
or unsupported inspection. A metadata candidate is not playback acceptance.

## Retained exact-package observation

The preserved [Flathub stable receipt](../../tests/fixtures/issue539/flathub-stable-embedded-hwconfig-20261028.json)
pins Kodi 21.3, app commit `9d26e518`, its exact ELF/manifest/Platform identities,
and the original probe-source hashes. That package's `h264_v4l2m2m` decoder is
present but publishes zero configurations, so its reviewed Kodi 21 policy finds
no DRM PRIME candidate. This is the retained 2026-10-28 metadata result, not a
new observation of another package, a Kodi main/media run, or clean-OS evidence.

The two probe sources and CPU tests are preserved byte-for-byte from the own
static-inspection lineage; the tests enforce the receipt's Python/C source
binding. The embedded runner retains its bounded process-timeout cleanup,
including SIGKILL if the selected child hangs. Normal CPU cases do not request
that timeout path. No real target rerun is required to reuse the exact retained
observation.

## Private-runtime namespace observers

`runtime-package-namespace.py` mounts a separately selected pinned Platform and
package read-only in a device-free/compilerless namespace. Its metadata worker
verifies the package file manifest and all seven actual FFmpeg library mappings.
The optional CLI worker performs only the bounded software H.264 comparison
against separately pinned media, pixel and timing references. The host kernel
is shared; these are not independent clean-OS, Kodi or native-playback proofs.
No FFmpeg packager, application add-on or external build helper is included here.

The metadata observer permits visible `/usr` only for a sole literal neutral
configure prefix, matching recorded prefix hashes and an explicit
`private_build_prefixes_detected: false` declaration. Other configure prefixes
must remain absent. It reports visible neutral `/usr` truthfully instead of
claiming every original configure prefix is absent; exact package mappings still
reject any library fallback into the Platform. This does not establish a
reproducible neutral build or change any distribution readiness declaration.

`make -C tools/kodi-route-probe check-runtime-metadata` uses synthetic package
files, Python metadata stand-ins and mapping records to exercise the actual
worker guards. It executes no selected ELF, namespace, CLI, compiler or device.
The preserved namespace/CLI observers retain bounded subprocess timeout cleanup;
the CLI also retains forceful cleanup of an unfinished software child. An actual
operator run must keep its own execution and restoration evidence separate.

<a id="guarded-native-activation-and-current-observations"></a>

## Historical native activation observations

The following retained October29–31 runs used a temporary research module.
They explain the original missing-node result and are not instructions to
change the module during current stable-module-only research. Current delivery
selection and accepted scope belong to the companion guides linked above.

The stable `3.10.13` baseline provides `/dev/crystalhd`; it lacks the native
decoding video node required by the unchanged FFmpeg implementation. The pinned
temporary native `3.10.14` module supplies the observed `/dev/video0`. The
baseline missing-node observation does not prohibit an ordinary guarded idle
transition. Follow [BRINGUP](../../BRINGUP.md#reload-an-idle-device): close
clients, verify no device owners and reference count 0, save exact module/options
identities, use normal unload/load, and restore the exact stable baseline after
the bounded trial. Stop on a busy or failed normal unload. Forced unloading
and killing a stuck owner for recovery remain prohibited.

The retained neutral archive has now run native Kodi 720p and 1080p short
EOS/reopen cases, each 180 frames played twice with 360 renderer acceptances.
The 720p long pause/reopen case returns all 1800 source frames across two plays,
but its renderer counts are 900+898; that deficit does not receive a full-render PASS. Linked diagnostic comparisons
return899+900 and899+899 renderer acceptances. The latter verifies renderer and
media-clock progress after each resume, so spacing alone does not repair it.
Read-only call linkage observes buffer-wait failure, output retry and fresh
GetPicture replacement before renderer submission; no external source is edited. All cases use the AAC clock with clear drift/stall checks; the long case's
paused audio-output timeout is retained separately with its video-buffer timeout. Live mappings match the exact neutral package for all six mapped
FFmpeg families and the pinned Platform for all seven selected providers.
The [native acceptance record](../../NATIVE-RUNTIME-ACCEPTANCE.md#current-host-neutral-package-native-observations)
keeps those case boundaries separate from complete #607/D3/D5, independent OS
and public-distribution acceptance.

## Offline source/caller/returned PTS

`native-pts-oracle.py` compares pinned source packet metadata with complete
accepted/returned PTS sequences. It follows the published demux and DRM PRIME
caller conversions at Kodi `28ea2eac1eb7af8fdcbd2672d933ce87f594be79`, preserving
binary64 operation order before int64 truncation. Its domain is one H.264
video stream with nonnegative packet ticks and zero-origin source clocks;
comparison tolerance is zero. Expected values come from source ticks/time_base
and the pinned primary-source model. Exact rational conversion is a diagnostic,
and observed native PTS cannot define the expected values.

The PTS verdict is separate from the native report status and the diagnostic
`renderer_census`. Thus all 900 returned PTS can match while only 898 frames are
accepted by the renderer. Missing renderer counts remain unknown. This offline
tool opens no device and qualifies no rendered pixels, scanout, A/V or runtime
acceptance. Its [model and scope](../../NATIVE-RUNTIME-ACCEPTANCE.md#offline-sourcecallerreturned-pts-oracle)
include primary source references. All 21 CPU controls pass, including renderer
deficit, unknown and excess cases. They run with:

```sh
make -C tools/kodi-route-probe check-native-pts
```

For a retained report, supply both JSON digests independently of the report:

```sh
python3 -B tools/kodi-route-probe/native-pts-oracle.py \
  --source-packets "$source_packets_json" \
  --source-packets-sha256 "$source_packets_sha256" \
  --native-report "$native_report_json" \
  --native-report-sha256 "$native_report_sha256" \
  --media-sha256 "$media_sha256" \
  --kodi-source 28ea2eac1eb7af8fdcbd2672d933ce87f594be79
```

Add `--require-renderer-census` to require a complete renderer return count.
It exits3 on deficit, excess or unknown while preserving the independent PTS
verdict, and exits0 on a complete count. Input/PTS failure exits2. No count gate
qualifies physical presentation or the whole native runtime.

The selected model and supplied digests identify inputs; they do not authenticate
the Kodi binary or the FFprobe execution that produced source metadata.

The private package producer is included alongside the current preflight and
slot lifecycle tools. It copies the exact seven FFmpeg DSOs, their SONAME links,
the two CLIs and source/dependency inventories; it never executes the selected
payload or publishes a package. Run `runtime-package.py --help` for the explicit
`pack`/`verify` inputs. `build-native-ffmpeg.sh` retains the pinned unmodified
source recipe; these tests do not configure or compile third-party code.

The build helper requires GNU `realpath` and `mv`, the pinned clean checkout,
and two fresh, absolute, disjoint output directories. Its logical prefix is
`/usr`; `make install DESTDIR=...` writes into the private build tree. Only a
successful installation publishes the requested private `bin/` and `lib/`
layout. Existing outputs, dangling links, overlapping paths and competing
destinations are rejected or preserved. No installation targets the host `/usr`.
The neutral prefix keeps private output paths out of configuration metadata;
it does not establish reproducible compilation or ABI compatibility.

`make -C tools/kodi-route-probe check-build-helper` runs the 13 existing
fake-tool controls for staging, paths, failures and publication. It does not
configure or compile FFmpeg. The aggregate `check` target includes them.

`make -C tools/kodi-route-probe check-package` tests real generated ELF fixtures,
reproducible packaging and relocation. It also passes producer output through
the current pinned-target preflight and actual private slot install, rejected
update, valid update, rollback and repeated removal. The fixture executables
are never run. These synthetic fixtures retain `PUBLIC_NOT_READY` and do not
establish native playback, source/binary correspondence or complete dependency notices.

`PUBLIC_NOT_READY` is the fixture/inner-producer status, not a blanket verdict
on the separately qualified published companion. These checks alone cannot
promote another runtime or target. Actual temporary activation requires its
own verified stable restoration. #538's excluded shutdown heap-abort subtask
remains outside these checks.
