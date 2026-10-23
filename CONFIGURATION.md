# CrystalHD configuration inventory

This is the complete inventory of configuration surfaces read by the current
production driver, library, VA-API backend, launchers, GStreamer element and
`crystalhd-play`. Unset controls retain the existing behavior.

The `crystalhd-config` utility validates and explains the
strict schema defined in [CONFIGURATION-SCHEMA.md](CONFIGURATION-SCHEMA.md).
Production components do not read those files, so installing or invoking the
utility does not change their behavior. For example:

```sh
crystalhd-config validate
crystalhd-config show-effective
crystalhd-config explain vaapi.h264.mode --format json
crystalhd-config list-presets
crystalhd-config bug-report > crystalhd-config-report.json
```

The reviewed preset catalog includes selectable Standard and explicitly
acknowledged Experimental entries as well as unavailable/virtual names which
cannot be selected. Use `list-presets` before selecting one; exact values,
requirements and acknowledgement syntax are in
[CONFIGURATION-SCHEMA.md](CONFIGURATION-SCHEMA.md#reviewed-preset-manifest).
Explicit [`save` and `reset` commands](CONFIGURATION-SCHEMA.md#save-and-reset-user-defaults)
can store managed user defaults. Saved settings are not applied to production
applications yet. The utility does not launch an application, reload a module,
source the legacy `chromium.conf`, or access CrystalHD hardware.
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
| `--software-output-size WIDTHxHEIGHT` <!-- config-surface:player-flag:--software-output-size --> | `crystalhd-player.py` | Host resize after scaled CrystalHD output | Unset; positive even dimensions, each at most `4096`, total at most `4096x2160` pixels | Requires hardware mode, nonzero `--scale-width`, GStreamer `videoscale`/`capsfilter` | Pipeline construction | **Opt-in** — additional CPU use; cannot restore source detail | Yes — CLI |
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
