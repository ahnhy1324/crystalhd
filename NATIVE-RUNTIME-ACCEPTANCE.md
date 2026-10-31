# Compilerless native runtime: reproducible preparation for #539

[#539](https://github.com/ahnhy1324/crystalhd/issues/539) remains **OPEN /
PUBLIC_NOT_READY**. Private installation can activate the measured native Kodi
route without compiling FFmpeg on the compatible existing host. That scoped
result is complete. Independently clean OS/native Kodi acceptance, complete
first-user burden and public distribution remain separate deliverables.

This guide adds an offline file preflight over the existing #576 artifacts.
It does not build/modify FFmpeg or Kodi, publish an archive, install a runtime,
change the stable stack or execute target binaries. Runtime checks below are
commands for a later operator run, not new PASS receipts.

## Reuse the implemented package and runtime checks

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
| D0 official package selection | Reuse #545's `ffmpeg-hwconfig-probe.py` for a directly linked Kodi ELF and its exact FFmpeg closure; require `--require-kodi-drm-prime`. Static-integrated Kodi needs a separately reviewed method. | Ubuntu/Debian sid tested metadata negatives are retained. Flathub stable DSO-probe exit2 is **UNCLASSIFIED**, not negative/PASS. Ubuntu has no supplied Kodi i386 package in the recorded inventory. |
| D1 smallest external implementation delta | Hardware config/get-format, real EXPBUF/AVDRM descriptors and FD/context/consumer lifetime semantics must all be supplied. | Advertisement-only own-driver/device changes cannot create missing FFmpeg selection metadata. Isolating/modifying a minimum FFmpeg/Kodi patch is outside the no-external-patch constraint. No own-side alternative is proved. |
| D3 complete first-user burden | On the selected fresh target, record exact commands, network bytes, installed/temporary/peak disk, peak RSS, elapsed/user CPU, prerequisites, update and rollback work; separate observer tools from user dependencies. | Prior configure/build/stage 17.88 minutes excludes fetch/tool setup; peak build RAM is unknown. Archive bytes exclude Kodi/Platform/GL/driver/source. Report absent measurements as null/NOT_RUN. |
| D4 clean-target native acceptance | Separately acquired/booted target or explicitly narrower clean-userspace scope; pinned original Kodi/App/Platform/GL/own-driver identities, no compiler at activation, 720p/FHD native selection, source-bound pixels/PTS, pause/seek/Stop/EOF/reopen, removal/fallback/re-extraction and resource boundaries. | Existing same-host proof is complete only in its stated scope. A new binary/target cannot inherit those results. No Atom/BCM70012 is connected. |
| D5 redistribution/maintenance | Neutral build paths and reproducible build recipe, corresponding-source/component/dependency notices, exact CPU/ABI support, update/security owner and explicit distribution decision. | Core2/x86_64 only; existing `atan2f@GLIBC_2.43` requirement; private prefixes embedded, source archive absent, maintenance owner unassigned. **PUBLIC_NOT_READY**. |

The private runtime is a minimal configured subset, not a drop-in FFmpeg
distribution. Replacing only `libavcodec` does not establish the matched ABI,
codecs, demuxers or graphics environment. Nonempty same-DPB STOP→START remains
unsupported (`-EOPNOTSUPP`); destructive seek recovery does not repair that
contract. No release/support/default or public-package promotion is made here.
#538's shutdown heap-abort subtask remains ignored/wontfix and is not included
in these preparation or acceptance commands.
