# Concrete frontend acceptance targets

This maps remaining [#445](https://github.com/ahnhy1324/crystalhd/issues/445),
[#429](https://github.com/ahnhy1324/crystalhd/issues/429),
[#436](https://github.com/ahnhy1324/crystalhd/issues/436),
[#477](https://github.com/ahnhy1324/crystalhd/issues/477),
[#538](https://github.com/ahnhy1324/crystalhd/issues/538) and
[#400](https://github.com/ahnhy1324/crystalhd/issues/400) to existing software
harnesses and the actual target each claim needs. It is preparation, not a new
execution receipt. Use [the capability index](STATUS-2026-10-31.md) for the
current merged/ready and PASS/FAIL/NOT RUN boundary.

The connected physical target is Core 2 Duo T8100 / BCM70015 / Intel 965GM.
Historical reporter Atom/i386, native ELF32 on an x86_64 kernel, QEMU no-SSE
and headless graphical sessions retain separate identities. No connected Atom
or BCM70012 is available to qualify a new current-source hardware result.
The maintainer selected current equipment only for this continuation; no
additional physical target is assumed available.

## Device-free preparation already available

Run these on the selected exact source before any new frontend run:

```sh
make kodi-compat-check
make ffmpeg-compat-check
make native-picture-check
make -C filters/vaapi check
```

`kodi-compat-check` compiles hash-pinned licensed Kodi excerpts and real lifecycle
models, with broken publication/release/reset controls. It does not launch Kodi
or establish that a patched client is installed. `ffmpeg-compat-check` models
the retained stream-loop boundary; it does not turn the real 296/300 FAIL into
PASS. `native-picture-check` uses inert references/models; complete old capture
replay still requires the external hashed image/reference files. The VA suite
executes own driver helpers with protocol/device scaffolding, not hardware
decoding or physical presentation.

Useful focused VA controls after the ordinary VA check has built its binaries:

```sh
LD_LIBRARY_PATH="$PWD/linux_lib/libcrystalhd" \
  filters/vaapi/vaapi-vpp-test --bob-fhd-return
LD_LIBRARY_PATH="$PWD/linux_lib/libcrystalhd" \
  filters/vaapi/vaapi-vpp-test --centered-readonly-pending
LD_LIBRARY_PATH="$PWD/linux_lib/libcrystalhd" \
  filters/vaapi/vaapi-vpp-test --vpp-chaining
```

These preserve direct/staged Bob pixels, identity/lifetime, cold queued centered
output and completed-source provenance. They are CPU controls. Exact selected
source/build IDs and flags still belong in each eventual hardware receipt.

## Ordinary Kodi, VA and CPU Bob

The [selected original-stock Bob receipt](tests/fixtures/issue445/stock-bob-payload-20261031.md)
now verifies all active Y/U/V bytes of8 selected VPP outputs from4 originals,
both physical parities across TFF/BFF natural EOF/reopen. Actual requested
canvases/crops are1280x736; only the1280x720 active prefix is compared. Every
runtime source was a completed full frame; compact first-field processing is
covered by CPU controls only. This narrows the remaining pixel gap without
claiming all-field, source/outputPTS, GPU retirement or displayed cadence.

| Remaining claim | Existing fixture/harness to reuse | Evidence still required |
| --- | --- | --- |
| #445 interrupted seek/pause/Stop source identity and per-object return | `tests/fixtures/issue445/publish-order-controls-20261029.json`, publication/lifecycle excerpts under that directory, and the original-stock progressive control in `tests/fixtures/issue510/player-window-trace-20261030.json` | Current original Kodi/FFmpeg executable and mapped own VA/DIL identities; selected decoded source/frame/field PTS; actual full NV12 crop/chroma; object acquisition/borrow/return across cancellation/flush/reset/reopen. Empty global counters and media-clock settlement alone are insufficient. |
| #436 selected TFF/BFF Bob and BWDIF semantics | Existing #319 TFF/BFF and #414 750-picture FHD fixtures; `tests/fixtures/issue436/native-single-field.json`, `card-scaled-bob.json`; preserved FFmpeg field references | Off/Auto/CPU fallback distinguished, exact input count versus two progressive outputs per complete picture, parity/cadence/chroma and independently captured field-rate reference. Keep default stream-loop 296/300 and original Kodi duplicate-pair PTS FAILs. |
| #429 reporter/native-i386 current stack | Reporter RC3/RC4/RC5 fixture identities and original results in #429 | Physical native32 Atom run with exact current source, original stock Kodi selection, post-seek pixels/PTS, interlaced Bob A/V and hour-long measured A/V drift. Primary-host/headless software cannot complete this row. |
| #477 centered crop and cadence | `tests/fixtures/issue477/l0s-kodi-20261024.json`, original 902/9,001-picture progressive, TFF/BFF and true60p no-B/B-reference cases | Extend full NV12 ROI/borders/field identity to Off/CPU fallback and 1080i29.97, natural EOS/seek/reopen and current sources. For any performance claim, matched source/policy/link state, useful ROI and declared detail reduction; software render selection and elapsed time remain separate from scanout. |
| Full physical presentation/A/V | Existing source barcodes/field-order references; selected dialogue content | Source-bound physical display capture and audio measurement with a declared tolerance and reference clock, repeatability and target panel refresh. A RenderCapture barcode establishes content, not display time. Neither queued renders nor OSD A/V estimates replace this. |

The public synchronous/pipelined VA readback/export probe already supplies a
useful own-stack checkpoint without a GUI. Select a progressive fixture with
an independently established complete count and the actual GPU render node:

```sh
make -C filters/vaapi seek-test
LIBVA_DRIVER_NAME=crystalhd LIBVA_DRIVERS_PATH="$PWD/filters/vaapi" \
LD_LIBRARY_PATH="$PWD/linux_lib/libcrystalhd" \
timeout --signal=TERM 210s filters/vaapi/vaapi-seek-test \
  /absolute/path/to/verified-progressive.mp4 /dev/dri/renderD128 \
  --export-prime --retain-old-frames
```

This compares complete-file and sought/retained pixels/PTS on real linear
DMA-BUFs, without accepting software fallback. It proves backend export/retained
ownership, not Kodi's per-object return, graphics rendering, physical A/V or
the interlaced Bob path. [Probe contracts and frame-count rules](filters/vaapi/README.md#seek-flush-and-retained-frames)
must be preserved. The broader `tests/vaapi-hardware-stress.sh` also builds and
can load/unload a source module; it is not a read-only preflight and must stay
under the operator's hardware/session boundary.

There are retained private operator harnesses, with fixed old sources and
dependencies rather than public generic Kodi test commands:

| Path | Safe preparation / exact limitation |
| --- | --- |
| `/home/mache/.cache/crystalhd-kodi-controls-20261029/run_v2.py` | `python3 -B .../run_v2.py --name new-plan --plan` prints the inert controls/reopen plan without imports/launch/device writes. Actual execution defaults to a private Kodi21.3 candidate, old own source, fixed fixture and other private controllers; inspect and repin all of them before treating it as a current-stack test. |
| `/home/mache/.cache/crystalhd-kodi-controls-20261029/offline_check_v2.py` | Synthetic asynchronous-control and natural-drain reader controls, never hardware evidence. It writes temporary fixture files under its own cache directory. |
| `/home/mache/.cache/crystalhd-window-runtime-20261030/native-observer/test-replay.py` | Synthetic native EXPBUF allocation/stream/lease/return reader controls, including incorrect index/tracking and copy-window overlap. Never a new capture. |
| `/home/mache/.cache/crystalhd-window-runtime-20261030/native-observer/analyze.py` | Existing offline reader accepts `--raw`, `--stderr`, `--abi`, `--pid`, optional `--seed`, `--require-closed`, `--output`; exact ABI and original owned PID come from the retained receipt, not guesses. It cannot supply missing GPU/source-PTS/physical-display evidence. |

No blind replay of a historical live controller qualifies a new source. The
original-stock-Kodi progressive receipt uses original published Kodi22RC1/App
FFmpeg with own VA/DIL `6db1598`, while the publication-order controls use a
different private Kodi21.3 candidate and `663c3a7`. Preserve that distinction.

## Native Kodi experiment, excluding heap work

Reusing the READY / UNMERGED #579→#581→#590 stack is preferable to reproducing
its code. #579 records source colour tuples/sampled RGB error0 in four frozen
original-client cases; #581 executes the actual format handler with mutations;
#590 binds one held EXPBUF active image to exact YUYV bytes, source barcode/PTS
and later QBUF/fd close. Earlier colour FAILs, collector errors and EPIPE
**NOT OBSERVED** remain retained. None is a complete crop/display/return census.

The public inert source helper already generates the 180-frame barcode fixture:

```sh
python3 -B tests/native-picture-fixture.py --width 1280 --generate-yuv420p /new/source.yuv
python3 -B tests/native-picture-fixture.py --width 1280 --decode CAPTURE.bgra
python3 -B tests/native-picture-oracle.py SOURCE.yuv CAPTURE.bgra \
  --width 1280 --height 720 --frames 180 --step 32 --minimum-samples 100 \
  --minimum-source-y 128 --matrix bt709 \
  --reference-sha256 REFERENCE_SHA --capture-sha256 CAPTURE_SHA
```

The generation command refuses an existing output; `-` instead of the new path
can feed a separately recorded software encoder. The oracle requires independently
established raw reference provenance and the declared limited-range SDR matrix;
do not infer the expected matrix from whichever candidate happens to match.
Full-frame comparisons, crop and source-bound GPU returns need additional
captured data beyond the sampled oracle. See [fixture semantics](tests/fixtures/issue538/README.md).

| Outstanding native row | Actual prerequisite |
| --- | --- |
| K2b colour/crop/pixels | Native stack at the selected exact own source; unmodified original Kodi/private matched FFmpeg; retained software raw reference; source-bound GPU/readback and all intended active/cropped pixels/chroma. Broader full-range/mixed-client/colour-only notification claims need their own cases. |
| K3b presentation/control | Frozen displayed picture while paused, exact unique displayed-frame and A/V clock behavior across seek/resume, source PTS and per-object returns. Existing render accepts/seek settlement are insufficient. |
| K4 nonempty STOP→START | Retain current `-EOPNOTSUPP` and #533 G2 FAIL/G3–G5 NOT RUN. Destructive reset recovery and input-only pause do not prove nondestructive same-DPB restart. No dummy/progress insertion without a separate demonstrated requirement. |
| K5b other targets/endurance | Physical native-i686/Atom, BCM70012, other GPU, broader codecs/interlace, controlled long-GOP/backpressure/endurance and physical A/V/E2E/power. These targets are absent or unmeasured in the current connected-host scope. |

**#538 stays OPEN. Only its shutdown heap-abort subtask is ignored/wontfix.**
No command or acceptance row here resumes investigation/reproduction of that
excluded subtask. Other source/pixel/lifetime/control work keeps its own scope.

## Settings translations and accessibility / #400

The optional editor uses gettext domain `crystalhd-settings` and looks under
`data_directory()/locale`. Catalogue extraction/checking, deterministic MO
compilation/installation, locale fallback and accessible-name/mnemonic relations
are delivered through #600. Reviewed Korean follows through #601: all150
supported messages are active,0 fuzzy, after independent Codex source-context
review and10 corrections. No human approval is claimed.

Catalogue/model20 and existing GTK28 checks pass. The source-tree GTK run uses
English fallback because that source path has no compiled MO. A separate staged
installed-Korean run loads the actual MO and checks150 active strings,0 unknown
Pango glyphs, Korean accessible label, true/false/unset/absent canonical save
states, mnemonic relation/focus and normal worker retirement. Its own-window
screenshot was visually reviewed for clipping/readability. Local retained
artifacts are `crystalhd-all-work-20261031/installed-korean-gtk.json` and `.png`.
Real screen-reader use, other languages/desktops and native-i386 GUI acceptance
remain separate.

Existing checks use private INI files and can run on the current host:

```sh
make configuration-check
xvfb-run -a make settings-ui-check
```

`settings-ui-check` runs real GTK controls and a private D-Bus tray test. It
requires GTK3/PyGObject, a usable display/Xvfb and `dbus-run-session`.
`tests/crystalhd-settings.test.py` is the stdlib editor/storage model and staged
install checker. Existing GTK coverage includes safe saves, responsive
installation inspection, draft preservation, editable Player names/label
relations and localized error descriptions. Own Entry accessibility now also
provides queried persistent INVALID_ENTRY, transition-only state events,
profile-scope restoration and destruction retirement. The direct-PID/private-bus
AT-SPI observer includes a fresh client connected after invalid input, alongside
the retained old-source negative control. Spoken Orca is a separate observation.
The [persistent-state receipt](tests/fixtures/issue400/validation-invalid-state-20261031.json)
records source English and installed Korean actual cross-process checks, fresh
observer PID/name/state and normal retirement. The older description-only
receipt remains accurate historical evidence. Current GTK31 checks include a
held accessible after destruction; no invalid state survives retirement.

The optional #611 [Kodi frontend](addons/script.crystalhd.settings/README.md)
uses native dialogs and shared CLI metadata/writes. Device-free dialog tests
cover Cancel/Back, save/reset, profile/unset, stale inputs and failures; they are
not real Kodi/D-pad or new-process native-reader acceptance. Record those actual
target runs and the ZIP installation separately with exact package/source pins.
The [actual stock Kodi21.3 UI receipt](tests/fixtures/issue611/kodi-settings-ui-20261031.json)
qualifies native ZIP installation, real directional/Select/Back RPC, preset save,
Back/declined-save nonwrites, confirmed global reset, diagnostic save and
file-only installation information. All9 installed ZIP members match, no
independent settings.xml is written, and Kodi exits0 normally. Its actual
English UI does not qualify spoken accessibility, other Kodi builds, or a new
process's native settings consumption/playback.

| Actual target | Required acceptance observation |
| --- | --- |
| Reviewed translated locale | Every visible message/catalogue placeholder and mnemonic checked by a competent reviewer; real GTK window, native-size/short-display layout, save/reset/error/installation/tray flows and English fallback. Catalogue compilation alone is not translation acceptance. |
| AT-SPI + Orca on real desktop session | Navigate every page and editable control by keyboard; correct accessible name, role/state and label relation, focus order/restoration, readable status/errors/draft/save outcomes, scrolling/short display and tray-host loss/recovery. Xvfb widget properties do not establish speech output. |
| Additional supported desktop/panel | Real launcher and icon at 16/22/24/32/64/128 px, light/dark/high-contrast behavior, single-instance focus/close and tray host absent/lost/recovered; record GNOME/KDE/LXQt/session/Wayland/X11 versions actually tested. Do not claim all desktops from the selected LXQt record. |
| Native-i386 GUI | Real ELF32 Python/GI/GTK desktop session on the identified target; same save/reset/profile/installation/tray/keyboard/screen-reader cases with unchanged configuration precedence. Headless Python3.6/i386 or QEMU scalar tests cannot qualify it. No connected native32 desktop target is currently identified. |
| Additional application adapter | Demonstrate that the unchanged named application actually consumes the requested values at its owning lifetime, with explicit override precedence and unsupported/restart wording. A saved key or file declaration alone is insufficient. Native/Player/explicit Chromium scope is already bounded; a universal adapter is not implied. |

Keep actual-target rows **NOT RUN** until those sessions and receipts exist.
Software preparation can complete independently; it must not label screen-reader,
translated-language, other-desktop or native32 GUI acceptance as PASS.
