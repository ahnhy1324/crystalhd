# CrystalHD configuration inventory

This is the complete inventory of configuration surfaces read by the current
production driver, library, VA-API backend, launchers, GStreamer element and
`crystalhd-play`. Unset controls retain the existing behavior.

The `crystalhd-config` utility validates and explains the
strict schema defined in [CONFIGURATION-SCHEMA.md](CONFIGURATION-SCHEMA.md).
Updated VA/DIL components read their supported defaults directly when an
application starts. Inspection and saving do not change a running application.
For example:

```sh
crystalhd-config validate
crystalhd-config show-effective
crystalhd-config explain vaapi.h264.mode --format json
crystalhd-config list-presets
crystalhd-config installation
crystalhd-config bug-report > crystalhd-config-report.json
```

`installation` (also `--format json`) and the settings window's **Installation**
page inspect reader declarations in the PATH-selected Player and the inspector
ABI's default linker-cache DIL/VA candidates. They do not execute these files,
load a decoder or read saved configuration. `declared-v1` identifies the supported
keys; absent declarations mean a legacy or unknown build. Refresh after updating
components. In the settings window, inspection runs in the background so editing
and closing remain available. Repeated refreshes keep only the latest request;
closing the window without a tray discards pending results.
Loader overrides and another application's ABI can select different
files: this is file capability, not proof of settings applied in a running app.
The report includes local file paths; review it before sharing. `bug-report`
retains its separate redacted configuration-only format.

The reviewed preset catalog includes selectable Standard and explicitly
acknowledged Experimental entries as well as unavailable/virtual names which
cannot be selected. Use `list-presets` before selecting one; exact values,
requirements and acknowledgement syntax are in
[CONFIGURATION-SCHEMA.md](CONFIGURATION-SCHEMA.md#reviewed-preset-manifest).
Explicit [`save` and `reset` commands](CONFIGURATION-SCHEMA.md#save-and-reset-user-defaults)
can store managed user defaults. A plain `kodi` launch then consumes the
native-owned subset **if it loads our updated VA driver/library**; selecting that
backend and enabling hardware decode in Kodi remain application setup steps.
See [native defaults](CONFIGURATION-SCHEMA.md#native-defaults-in-ordinary-applications)
for the supported subset, file validation and restart boundary. Frontend-only
settings are not automatically applied by VA/DIL. Experimental managed settings
still require fresh acknowledgements through the optional
[`run` command](CONFIGURATION-SCHEMA.md#run-an-explicit-command), which also
provides per-invocation CLI overrides without changing saved defaults. For example:

```sh
# Save a diagnostic default for future application starts.
crystalhd-config save vaapi.debug=true
kodi
# Run once without that diagnostic; the saved default remains true.
crystalhd-config run --override vaapi.debug=false -- kodi
# Or disable the saved diagnostic for subsequent starts.
crystalhd-config save vaapi.debug=false
```

Raw process environment wins per key, including its original encoding; a
supported `run --override` wins over that environment only for its own key.
The manager never reloads a module, sources the legacy
`chromium.conf`, or probes CrystalHD hardware.
The JSON-only `bug-report` form removes user-selected profile identifiers,
non-compiled paths, raw environment values and arbitrary password-store
values. It describes configuration only; use separately reviewed
`crystalhd-check` output for installed component and backend identity.

Safety labels mean:

- **Standard** — ordinary supported selection; normal input and device limits
  still apply.
- **Opt-in** — supported but changes power, memory, CPU use or output quality;
  measure the result and keep a rollback.
- **Experimental** — default-off evaluation path without a general support
  claim.
- **Diagnostic** — troubleshooting output which may perturb timing or expose
  media/application details.
- **Security-sensitive** — weakens an isolation boundary or executes trusted
  user code.

“Public” below identifies an existing user-visible interface. Environment
variables do not add to the `libcrystalhd.so.3` ABI, and none of these controls
changes the legacy ioctl ABI.

## Player defaults

Updated `crystalhd-play` reads the saved decoder mode, card scale width and optional
software output size at startup, using the same system/user/profile rules as
`crystalhd-config`. For example, save a scaled hardware profile once, then play
without repeating its options:

```sh
crystalhd-config save --profile-section local --profile local \
  'player.decoder="hardware"' gstreamer.crystalhddec.scale_width=1280 \
  'player.software_output_size="1920x1080"'
crystalhd-play "/path/to/video.mp4"
# One-shot native-width hardware output; the saved profile is unchanged:
crystalhd-play --scale-width 0 --software-output-size native "/path/to/video.mp4"
# One-shot software decode with those hardware-only defaults explicitly cleared:
crystalhd-play --software --scale-width 0 --software-output-size native "/path/to/video.mp4"
```

Explicit flags win only for their own setting. Conflicting combinations stop
before playback and explain the reset flags. `unset` in a profile restores the
native value instead of inheriting a lower file value. With no configuration,
the original hardware/native-size defaults remain. The startup report describes
requests and redacted origins; actual CrystalHD success still requires decoded
frames. Width/media/device restrictions in [USAGE.md](USAGE.md#optional-bcm70015-output-scaling)
still apply; neither saving nor selecting a profile invents support.

Unsafe/invalid files or an experimental preset without acknowledgement discard
the managed player overlay with a diagnostic; original CLI/native defaults
remain. `CRYSTALHD_CONFIG_DISABLE=1` bypasses managed files. `config run` retains
its existing unsupported-player-key rejection and bypass behavior; invoke the
player directly for these defaults. Saved `player.python` is not executed:
the wrapper's existing explicit environment override still selects Python.
Other applications' settings and arbitrary GStreamer pipelines are unaffected.

## Optional desktop settings

With the updated native reader, saved supported defaults are read on ordinary
application startup. The optional `crystalhd-settings` window edits those defaults;
`config run` is only needed for explicit invocation overrides/acknowledged launches.

On Ubuntu, the window needs `python3-gi` and `gir1.2-gtk-3.0`; the optional
tray additionally uses `gir1.2-ayatanaappindicator3-0.1` and a desktop StatusNotifier
host. The command-line utility and native decoders have no GTK/tray dependency.
Install the desktop frontend separately from the runtime:

```sh
sudo make install-settings
crystalhd-settings
# Start in the tray; show the window if no working tray is available:
crystalhd-settings --tray
# Window-only mode, including desktops without a tray:
crystalhd-settings --no-tray
```

For a source checkout, use `python3 scripts/crystalhd-settings.py` without installing.
A normal command opens settings; the desktop launcher starts in the tray.
**Hide to tray** keeps the app running, **Open settings…** in the tray reopens
the window, and **Quit** removes the icon and exits. Without a working tray,
the window remains visible and only Quit is offered at the bottom. Losing the
tray connection reveals the window. Starting again opens the existing desktop
instance. Explicit file paths and `--no-tray` use a separate editor instead.
No login autostart or system service is installed; saved defaults remain after exit.

The first **Player** page edits the decoder, card output width and software output
size consumed by updated `crystalhd-play`. Set a custom width/size or follow other
saved defaults. **Use built-in default** (`unset`) requests native card width/no
software resize; explicit CLI overrides still win. Smaller card output reduces
transferred pixels and detail; software resizing adds CPU work and cannot recover
that detail. Input and dependency errors are shown before Save. Unfinished text
survives switching profiles and blocks saving until corrected or discarded.

**Decoder defaults** edits safe library/VA values, and **Advanced** can clear
saved experiments. Each preview shows its source without claiming active playback.
Under **Profiles and presets**, **Settings to edit** chooses user defaults or a
named profile. **New profile** adds an empty draft; edit its values, then select it
as **Profile used by default** if ordinary launches should use it.
Switching views retains all draft changes and previews that profile
without changing the default selection. **Remove user profile** requires
confirmation and takes effect only after Save; system definitions remain intact.
Editing a system profile adds user overrides, and removing those overrides
reveals its system values. Removing a user-only default profile also clears its
user selector, returning to inherited selection. Other owners'
settings are inspectable in **Details**. Quality is selectable and labeled
**Player only**; it does not change Kodi output. Unavailable presets and
launch-only experimental activations remain disabled. Existing saved entries
remain intact. Saving an already selected experimental
preset requires fresh confirmation for that write only; launch acknowledgement
is still separate. Native-only experiments also mark the model as partial.
Use the CLI
and the [native contract](CONFIGURATION-SCHEMA.md#native-defaults-in-ordinary-applications)
for their supported invocation paths.

**Save changes** writes the user file through the same atomic writer as the CLI.
**Reload file** asks before discarding a draft. **Clear all user settings…** asks before clearing all user settings
and profiles; system defaults remain. Conflicting edits and uncertain write
outcomes require review/reload, never a blind overwrite. Ctrl+S saves, Ctrl+R reloads,
Ctrl+Q exits, and Escape closes/hides the window. Labels and keyboard mnemonics are
provided; UI messages are prepared for gettext translation (English is supplied).
Reopening a long-running tray window checks the saved user and system files in the
background. An external change preserves every draft but requires an explicit
**Reload file** before Save or Clear, so the window never silently merges or overwrites it.

The displayed model belongs to the settings process, including its environment;
it does not certify a running player's settings or installed reader version.
Restart applications after saving. The utility does not select Kodi's decoder,
change another app's configuration, load a module, or modify PCIe/power policy.
`make uninstall-settings` removes only the window/desktop/icon files, retaining
shared `crystalhd-config` and user defaults; full runtime removal also removes the
optional frontend. `PREFIX`/`BINDIR`/`DATADIR`/`DESTDIR` are supported; custom desktop
locations must be on the desktop's normal PATH/XDG search paths.

For the same profile operations without GTK:

```sh
# Create/edit the profile without changing the default:
crystalhd-config save --profile-section local 'vaapi.h264.mode="bounded"'
crystalhd-config save --profile local
# Remove one override (inherit), or explicitly request its native default:
crystalhd-config save --profile-section local --remove vaapi.debug
crystalhd-config save --profile-section local vaapi.debug=unset
crystalhd-config delete-profile local
```

These commands save defaults only. Existing process environment and application
CLI/property overrides still win; restart applications to read a changed file.

## Native configuration bootstrap

| Control | Owner | Device/backend | Default and accepted values | Dependencies | Read/apply lifecycle | Safety | Public API |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `CRYSTALHD_CONFIG_DISABLE` <!-- config-surface:environment:CRYSTALHD_CONFIG_DISABLE --> | Native VA/DIL reader | All native paths | Unset/other values read managed defaults; exactly `1` bypasses both files | Original environment controls remain authoritative | Snapshot on first configuration use per component; restart after saving | **Standard** — native defaults/environment fallback; no risk acknowledgement | Yes — documented process environment |

## Kernel module parameters

Kernel parameters have their own lifetime. They cannot be changed immediately
by an application profile after the module has loaded.

| Control | Owner | Device/backend | Default and accepted values | Dependencies | Read/apply lifecycle | Safety | Public API |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `force_l0s_off` <!-- config-surface:module:force_l0s_off --> | `crystalhd` kernel module | BCM70015 (`0x1615`) on a direct, dedicated PCIe root-port link | `N`; kernel boolean `0/1`, `N/Y` | Supported topology; `force_l0s_off=1` takes precedence over session mode | Read at module load (`0444` in sysfs); applied for the module lifetime; PCI-core policy can remain after unload | **Opt-in** — link-power/persistence effect | Yes — module parameter |
| `session_l0s_off` <!-- config-surface:module:session_l0s_off --> | `crystalhd` kernel module | Same BCM70015 topology | `N`; kernel boolean `0/1`, `N/Y` | Supported topology; ignored when force mode is enabled | Read at module load (`0444`); disables L0s only while a decoder session owns the card and restores the captured idle state on clean close | **Opt-in** — link power; preferred first L0s test | Yes — module parameter |

Use the idle reload, verification and rollback procedure in
[USAGE.md](USAGE.md#full-hd-pcie-workaround). Neither setting is a generic
ASPM switch, and unsupported topology is rejected.

## libcrystalhd environment

| Control | Owner | Device/backend | Default and accepted values | Dependencies | Read/apply lifecycle | Safety | Public API |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `CRYSTALHD_EXPERIMENTAL_RX_POOL_COUNT` <!-- config-surface:environment:CRYSTALHD_EXPERIMENTAL_RX_POOL_COUNT --> | `libcrystalhd` | Library capture pool on BCM70012/BCM70015; the 8-buffer measurement is BCM70015-only | `16`; exactly `8` or `16`; unset or invalid uses `16` | A decoder configuration which allocates userspace YUV output buffers | Read when that library context creates its output memory pool; set before starting the process/session | **Experimental** — memory/queue-depth trade-off | Yes — documented process environment; not ABI |

## VA-API environment

The H.264 policy controls are read when a VA decode context is created. The
i915 and trace controls are read when the VA driver instance is initialized.

| Control | Owner | Device/backend | Default and accepted values | Dependencies | Read/apply lifecycle | Safety | Public API |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `CRYSTALHD_VAAPI_LIVE_H264` <!-- config-surface:environment:CRYSTALHD_VAAPI_LIVE_H264 --> | CrystalHD VA driver | H.264 VA decode; explicit live behavior is BCM70015-only | Unset: bounded finite-input replay; exactly `1`: long-running live/direct path; every other value selects strict replay | Client must provide later input or explicitly drain retained surfaces at context destruction | Snapshot at `vaCreateContext()` | **Experimental** | Yes — documented process environment |
| `CRYSTALHD_VAAPI_LOW_LATENCY_H264` <!-- config-surface:environment:CRYSTALHD_VAAPI_LOW_LATENCY_H264 --> | CrystalHD VA driver | BCM70015 H.264 VA decode | Unset: automatic prefeed for eligible finite progressive/MBAFF frame AUs; exactly `1`: explicit H.264 prefeed; any other explicit value: off | Prefeed is independent of LIVE; explicit live streaming combines both controls; no A/V-sync guarantee | Snapshot at `vaCreateContext()` | **Experimental** for explicit selection; bounded automatic progressive/MBAFF behavior is part of ordinary finite decode | Yes — documented process environment |
| `CRYSTALHD_VAAPI_EXPERIMENTAL_CENTERED_NV12` <!-- config-surface:environment:CRYSTALHD_VAAPI_EXPERIMENTAL_CENTERED_NV12 --> | CrystalHD VA driver | Development BCM70015 ordinary H.264 VLD, NV12 | Unset or `0`: off; `WxH@VISIBLE_WxVISIBLE_H:limited` or `:full`, for example `1280x720@1920x1080:limited`; invalid/inexact geometry is rejected | Explicit visible crop/range and exact downscale; cannot combine with decode-processing additional outputs, READY remap or BCM70012; [usage and limits](filters/vaapi/README.md#development-experiment-centered-card-output) | Snapshot at decode `vaCreateContext()`; sequence crop admission checked at Submit | **Experimental** — smaller centered picture, full readable canvas; not stable v1.2 support | Yes — native process environment only; not in inspector schema/presets |
| `CRYSTALHD_VAAPI_EXPERIMENTAL_READY_REMAP` <!-- config-surface:environment:CRYSTALHD_VAAPI_EXPERIMENTAL_READY_REMAP --> | CrystalHD VA driver | BCM70015 H.264 VA presentation experiment | Off; exactly `1` requests remapping | Requires live mode and low-latency mode, plus a valid policy and delay | Snapshot at `vaCreateContext()` | **Experimental** — deliberately does not preserve standard VA surface identity | Yes — documented process environment |
| `CRYSTALHD_VAAPI_EXPERIMENTAL_READY_POLICY` <!-- config-surface:environment:CRYSTALHD_VAAPI_EXPERIMENTAL_READY_POLICY --> | CrystalHD VA driver | READY-remap experiment | Unset or `previous`; optional `latest`; any other value disables remapping | READY remap and its prerequisites | Snapshot at `vaCreateContext()` | **Experimental** | Yes — documented process environment |
| `CRYSTALHD_VAAPI_EXPERIMENTAL_READY_DELAY_US` <!-- config-surface:environment:CRYSTALHD_VAAPI_EXPERIMENTAL_READY_DELAY_US --> | CrystalHD VA driver | READY-remap experiment | `0`; exactly `0`, `16667`, `33333` or `50000` microseconds; any other value disables remapping | READY remap and its prerequisites | Snapshot at `vaCreateContext()` | **Experimental** | Yes — documented process environment |
| `CRYSTALHD_VAAPI_EXPERIMENTAL_I915_WC` <!-- config-surface:environment:CRYSTALHD_VAAPI_EXPERIMENTAL_I915_WC --> | CrystalHD VA driver | CPU writes to VA-allocated i915 dumb surfaces; not imported surfaces, VPP or other DRM drivers | Off; exactly `1` requests WC/GTT domains | i915 render node and supported GEM-domain transitions; falls back after an unavailable WC path | Snapshot at VA driver initialization; a runtime domain failure disables the experiment for later writes | **Experimental** — reference-host optimization | Yes — documented process environment |
| `CRYSTALHD_VAAPI_DEBUG` <!-- config-surface:environment:CRYSTALHD_VAAPI_DEBUG --> | CrystalHD VA driver | All VA paths | Unset is off; **any presence, including `0`, is on** | None | Consulted when debug output is emitted | **Diagnostic** — stderr may contain access-unit bytes | Yes — documented process environment |
| `CRYSTALHD_VAAPI_TRACE` <!-- config-surface:environment:CRYSTALHD_VAAPI_TRACE --> | CrystalHD VA driver | All VA paths | Off; exactly `1` emits JSONL | None | Snapshot at VA driver initialization | **Diagnostic** — timing perturbation and application metadata | Yes — documented process environment |

The exact replay, drain and presentation limitations are in the
[VA-API guide](filters/vaapi/README.md#replay-and-end-of-stream). Unset all
experimental variables to return to the ordinary path.

The `crystalhd-config` inspector accepts explicit prefeed independently of
LIVE, but does not inspect per-AU automatic-prefeed eligibility or apply
settings. A false `vaapi.h264.low_latency` means explicit prefeed was not
requested, not that automatic prefeed is disabled: an absent native variable
permits automatic selection, while any explicit non-`1` value suppresses it.
READY remap still requires both live mode and explicit prefeed. See the
[schema rules](CONFIGURATION-SCHEMA.md#cross-setting-validation) and backend
[prefeed rules](filters/vaapi/README.md#replay-and-end-of-stream).

## Launcher and wrapper environment

### `crystalhd-chromium`

These variables configure only the launcher process. Hardware browser decode
remains experimental and is not a daily-use support claim.

| Control | Owner | Device/backend | Default and accepted values | Dependencies | Read/apply lifecycle | Safety | Public API |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `CRYSTALHD_CHROMIUM_CONFIG` <!-- config-surface:environment:CRYSTALHD_CHROMIUM_CONFIG --> | `crystalhd-chromium` | Launcher, both modes | `$XDG_CONFIG_HOME/crystalhd/chromium.conf`, falling back to `$HOME/.config/crystalhd/chromium.conf`; or a readable path | File must be trusted: it is sourced as shell code with the launcher's existing privileges; run the launcher unprivileged and never select untrusted code | Path is selected once, then the file is sourced before the remaining launcher variables are read | **Security-sensitive** — arbitrary trusted shell code | Yes — launcher environment |
| `CRYSTALHD_CHROMIUM` <!-- config-surface:environment:CRYSTALHD_CHROMIUM --> | `crystalhd-chromium` | Launcher, both modes | Autodetects `google-chrome-stable`, `google-chrome`, `chromium`, then `chromium-browser`; or an executable path | Non-Snap browser; Snap confinement cannot access the device | Resolved once at launch | **Standard** | Yes — launcher environment |
| `CRYSTALHD_DRM_DEVICE` <!-- config-surface:environment:CRYSTALHD_DRM_DEVICE --> | `crystalhd-chromium` | Experimental hardware mode | `/dev/dri/renderD128`; any explicit device path is passed to Chromium | Must select the display GPU render node | Read at launch; effective only in hardware mode | **Experimental** | Yes — launcher environment |
| `CRYSTALHD_CHROMIUM_PASSWORD_STORE` <!-- config-surface:environment:CRYSTALHD_CHROMIUM_PASSWORD_STORE --> | `crystalhd-chromium` | Chromium argument, both modes | If unset, `basic`; a nonempty string becomes `--password-store=VALUE`; empty omits the flag | Browser must accept the selected upstream backend | Read at launch | **Standard** — upstream browser setting | Yes — launcher environment |
| `CRYSTALHD_CHROMIUM_PROFILE_DIR` <!-- config-surface:environment:CRYSTALHD_CHROMIUM_PROFILE_DIR --> | `crystalhd-chromium` | Chromium profile, both modes | Unset uses browser default; nonempty directory path selects a profile | Launcher creates the directory; user owns its contents | Read and created at launch | **Opt-in** — persistent browser state | Yes — launcher environment |
| `CRYSTALHD_CHROMIUM_NATIVE_GL` <!-- config-surface:environment:CRYSTALHD_CHROMIUM_NATIVE_GL --> | `crystalhd-chromium` | Browser graphics path | Off: launcher forces software GL; exactly `1` leaves native GL available | Host GPU/driver compatibility | Read at launch | **Experimental** | Yes — launcher environment |
| `CRYSTALHD_CHROMIUM_EXPERIMENTAL_HW_DECODE` <!-- config-surface:environment:CRYSTALHD_CHROMIUM_EXPERIMENTAL_HW_DECODE --> | `crystalhd-chromium` | CrystalHD VA browser path | Off; exactly `1` enables the experimental path; other values keep accelerated video decode disabled | CrystalHD VA driver, suitable DRM node and explicit sandbox acknowledgement | Read at launch | **Experimental** | Yes — launcher environment |
| `CRYSTALHD_CHROMIUM_DISABLE_GPU_SANDBOX` <!-- config-surface:environment:CRYSTALHD_CHROMIUM_DISABLE_GPU_SANDBOX --> | `crystalhd-chromium` | Experimental hardware mode | Off; exactly `1` acknowledges and adds `--disable-gpu-sandbox` | Required when experimental hardware decode is `1` | Read at launch; launcher refuses hardware mode without it | **Security-sensitive** — disables Chromium GPU sandboxing | Yes — launcher environment |
| `CRYSTALHD_CHROMIUM_H264_POLICY` <!-- config-surface:environment:CRYSTALHD_CHROMIUM_H264_POLICY --> | `crystalhd-chromium` | Google Chrome warning | Off; exactly `1` suppresses the policy warning | The user must install/verify any actual H.264 policy separately | Read at launch; changes warning only | **Standard** — acknowledgement, not codec enforcement | Yes — launcher environment |

The config file is sourced after its path is selected. Consequently, an
assignment in that file can override an inherited variable; this is the
current launcher behavior, not the proposed future config precedence model.
Extra launcher arguments are passed to the browser and remain upstream
Chromium options rather than CrystalHD settings.

### `crystalhd-play` wrapper

| Control | Owner | Device/backend | Default and accepted values | Dependencies | Read/apply lifecycle | Safety | Public API |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `CRYSTALHD_PLAYER_PYTHON` <!-- config-surface:environment:CRYSTALHD_PLAYER_PYTHON --> | `crystalhd-play` shell wrapper | Local player launcher | `/usr/bin/python3`; path to an executable compatible Python 3 interpreter | Python GObject/GStreamer bindings | Resolved once before the controller is executed | **Standard** | Yes — launcher environment |

## GStreamer property

| Control | Owner | Device/backend | Default and accepted values | Dependencies | Read/apply lifecycle | Safety | Public API |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `crystalhddec scale-width` <!-- config-surface:gst-property:scale-width --> | GStreamer `crystalhddec` | BCM70015 decode; progressive and selected H.264 MBAFF with complementary card-field output | `0` means native; otherwise an even width `128..1918`, not wider than coded input; interlaced H.264 requires a multiple of 16 | Hardware scaling support; output height/aspect is derived from the source | Writable only in NULL or READY; snapshotted for one decoder run and reapplied after flush/reopen | **Opt-in** — less PCIe traffic and less detail | Yes — GObject element property |

The GObject type is an unsigned integer, but values outside the effective
range above fail validation. BCM70012, unsupported interlaced codecs/layouts
and non-multiple-of-16 widths for H.264 interlaced scaling are
rejected.

The player automatically uses the plugin's `crystalhddiqos` compatibility
controller when available; older plugins retain the Python implementation.
Its `attached`, `active`, `eligible`, `qos-count`, `forward-count` and
`failure-count` properties are read-only internal diagnostics, not settings.
It does not change the supported GStreamer version or QoS admission rules.

## `crystalhd-play` command line

| Control | Owner | Device/backend | Default and accepted values | Dependencies | Read/apply lifecycle | Safety | Public API |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `--hardware` <!-- config-surface:player-flag:--hardware --> | `crystalhd-player.py` | GStreamer local playback | Default mode; flag has no value | Installed/discoverable `crystalhddec`, supported media and an idle card | Decoder ranks are selected for this process before pipeline creation; no software fallback | **Standard** | Yes — CLI |
| `--software` <!-- config-surface:player-flag:--software --> | `crystalhd-player.py` | GStreamer local playback | Off; mutually exclusive with `--hardware` | Available software decoder | Decoder ranks and playbin software-decoder flag are selected before pipeline creation | **Standard** | Yes — CLI |
| `--weave` <!-- config-surface:player-flag:--weave --> | `crystalhd-player.py` | Hardware or software local playback | Off; flag has no value | Available selected decoder and playbin | Clears only playbin's deinterlace flag before playback; fails if it remains enabled | **Opt-in** — interlaced frame pass-through, not deinterlacing or a sync guarantee | Yes — CLI |
| `--scale-width PIXELS` <!-- config-surface:player-flag:--scale-width --> | `crystalhd-player.py` | BCM70015 hardware playback; selected H.264 MBAFF in the bounded 1.2 scope | `0`; otherwise even `128..1918`; interlaced H.264 requires a multiple of 16 | Requires hardware mode; also subject to coded-width/property limits | Parsed at startup and applied to each CrystalHD decoder element | **Opt-in** — hardware downscale/quality trade-off | Yes — CLI |
| `--software-output-size SIZE` <!-- config-surface:player-flag:--software-output-size --> | `crystalhd-player.py` | Host resize after scaled CrystalHD output | Unset; positive even dimensions, each at most `4096`, total at most `4096x2160` pixels; `native` clears a saved resize for this invocation | Resize requires hardware mode, nonzero `--scale-width`, GStreamer `videoscale`/`capsfilter` | Pipeline construction | **Opt-in** — additional CPU use; cannot restore source detail | Yes — CLI |
| `--subtitles FILE` <!-- config-surface:player-flag:--subtitles --> | `crystalhd-player.py` | Local playback | Unset; readable local regular file | GStreamer subtitle support for the file | Parsed before pipeline construction | **Standard** | Yes — CLI |

The required positional media `FILE`, automatically supplied `-h/--help`, and
interactive playback keys are command grammar/actions, not saved settings, so
they are not configuration-manager keys.

## Adjacent external selectors

These affect documented launch procedures but are owned by another runtime;
they must not be presented as CrystalHD config keys or silently persisted.

| Control | Owner | Default / use | Lifecycle and safety | Public status |
| --- | --- | --- | --- | --- |
| `LIBVA_DRIVER_NAME=crystalhd` | libva | Select the installed CrystalHD VA driver | Process-local driver selection; Standard | External public interface |
| `LIBVA_DRIVERS_PATH` | libva | Source-tree testing only; do not add checkout paths for an installed stack | Loader path at process start; Opt-in/debug | External public interface |
| `VAAPI_FORCE_INDIRECT=1` | Moonlight | Select the tested DRM PRIME/EGL route because this VA driver has no `vaPutSurface` | Moonlight process start; required by the documented experimental Moonlight path | External application interface |
| `GST_PLUGIN_PATH`, `GST_PLUGIN_PATH_1_0`, `LD_LIBRARY_PATH` | GStreamer/dynamic loader | Source-tree wrapper plumbing; installed use should rely on installed discovery | Process start; stale checkout paths can select mismatched components | External public interfaces |
| `XDG_CONFIG_HOME`, `HOME` | desktop/shell environment | Locate the default Chromium launcher config when no explicit path is set | Evaluated before that trusted config is sourced | External public interfaces |

Build switches (`LEGACY_CPU`, `CRYSTALHD_FW_RESEARCH`, `CRYSTALHD_V4L2`),
test-only `CRYSTALHD_TEST_*` variables, `CRYSTALHD_CHECK_ROOT`, and controls
under `tests/` or `tools/fw-research/` are intentionally outside this runtime
inventory. The V4L2 and firmware-research builds are not supported production
configuration presets.

## Keeping the inventory in sync

Run the device-free check after adding or removing a production setting:

```sh
make configuration-check
```

It compares this document's hidden surface identifiers with module parameters,
literal library/VA environment reads, CrystalHD-named variables in installed
scripts, production GStreamer properties and explicit `crystalhd-play` options.
Missing and stale rows both fail.

The schema conformance check also classifies all 28 surfaces: 23 userspace
surfaces map to 21 canonical keys, the two module parameters retain a separate
module-load lifetime, `CRYSTALHD_CHROMIUM_CONFIG` remains a legacy bootstrap,
and `--subtitles` and `--weave` remain invocation-only.
