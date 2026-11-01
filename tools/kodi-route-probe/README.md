# Kodi FFmpeg metadata inspection

These own diagnostic tools inspect a trusted Kodi package's FFmpeg decoder
metadata for #607. They do not install a decoder, change backend selection, or
qualify the private native FFmpeg runtime for distribution.

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

Public native-runtime distribution remains `PUBLIC_NOT_READY`. Existing VA
fallback, system Kodi/FFmpeg files, stable modules/firmware and #538's excluded
shutdown heap-abort subtask are unchanged.
