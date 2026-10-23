# CrystalHD configuration schema v1

This document specifies the configuration contract implemented by the
`crystalhd-config` inspector and explicit user-config storage commands. The
driver, libraries, plugins and launchers do not read these files; saving or
inspecting settings does not apply them to applications. The current production
controls and their exact behavior remain documented in [CONFIGURATION.md](CONFIGURATION.md).

The v1 goals are deliberately narrow:

- preserve every existing environment variable, module parameter, command
  line option, GStreamer property and public ABI;
- make an effective value and its origin explainable without guessing an
  application from its process name;
- reject malformed or unsafe configuration as one transaction rather than
  applying a convenient subset; and
- keep privileged module configuration outside an ordinary userspace
  playback profile.

The installed implementation is `scripts/crystalhd-config.py`.  The schema
and CLI tests import or execute that production implementation; test code is
never installed as the parser.

## Inspect configuration

The five inspection commands are:

```sh
crystalhd-config validate
crystalhd-config show-effective --format text
crystalhd-config explain vaapi.h264.mode --format json
crystalhd-config list-presets --format text
crystalhd-config bug-report > crystalhd-config-report.json
```

Options follow the command. `--system-config FILE` and `--user-config FILE`
select explicit absolute paths; `--no-system-config` and `--no-user-config`
suppress the corresponding layer. `--preset NAME`, `--profile NAME`, and
`--ignore-environment` control selection and setting-specific process
environment inspection. Experimental presets additionally require their
repeatable, command-line-only `--acknowledge-risk TOKEN`; neither a config
file nor an environment variable can supply it. The path-locating
`XDG_CONFIG_HOME` or `HOME` must
still be absolute unless the user layer is explicitly suppressed. Text and
JSON formats are available for `show-effective`, `explain` and `list-presets`.
`bug-report` is JSON-only.

Exit status `0` means success, `2` is command-line syntax, an unknown canonical
key or an unknown/duplicated acknowledgement token, and `3` is an unsafe,
unreadable or invalid configuration, selector, missing/inapplicable required
acknowledgement, or cross-setting transaction. Write commands additionally use
`4` when replacement committed without confirmed durability, or its outcome
cannot be determined; this is not a claim that the previous bytes survived.

Inspection commands are diagnostic only: selecting a preset changes the reported
target transaction, not a running application. The utility cannot apply settings,
launch another program, reload a module, source `chromium.conf`, or access
hardware. Inspection's last observable layer is the current process environment. A
future application's CLI options and GObject properties can be higher-priority
inputs, but this read-only process cannot observe them and reports that limit.
Reported states describe only configuration and established native-adapter
semantics; they do not mean an application applied a value. Hardware-dependent
requests such as BCM70015 scaling, i915 WC, a DRM node, decoder availability
or link topology are reported as conditional/not inspected where applicable.

`bug-report` exports the same configuration/native-adapter model and origin
classes for support use. It additionally redacts a selected profile's name,
the name suffix in profile origins, non-compiled paths, raw environment
values, and non-compiled Chromium password-store values. Fixed canonical key,
preset and environment-variable names remain visible. The report does not
inspect or certify the installed library, module, plugin, device or hardware;
pair it with separately reviewed `crystalhd-check` output when installation
identity matters. Inspect any report before publishing it.

## Save and reset user defaults

These commands write only the managed user file. **Saved does not mean active:**
stock Kodi and the other production components still use their existing controls.
Runtime integration is a separate stage of
[#400](https://github.com/ahnhy1324/crystalhd/issues/400#issuecomment-6357124035).

```sh
crystalhd-config save vaapi.debug=false
crystalhd-config save 'chromium.password_store=""'
crystalhd-config save --remove vaapi.debug
crystalhd-config reset
```

`save KEY=VALUE` patches the base `[settings]` overlay using the strict value
grammar below. Unspecified settings and profile sections remain intact.
`--remove KEY` removes a base override so lower layers can supply it;
`KEY=unset` explicitly blocks a lower configured value. Neither is equivalent to
`false`, `0`, or `""`. `--preset NAME` and `--profile NAME` save metadata selectors;
they do not edit a named profile's settings. `--remove preset` or
`--remove profile` clears that selector. Experimental presets require their
explicit risk acknowledgements on each write; acknowledgement tokens and current
setting-specific environment values are never copied into the file.

The default destination is the user path below; `--user-config FILE` selects an
absolute user destination. `--system-config FILE` or `--no-system-config` only
controls the read-only system layer used for validation. Neither the compiled
system path nor a selected system file is a writable destination. `reset`
replaces the user overlay with a valid schema-only document, restoring inheritance
from lower layers. It can recover a malformed INI file after the same size,
ownership and file-type checks.

Writes use an atomic `0600` replacement and reject unsafe ownership, symlinks,
special files and hard-linked targets. The containing configuration directory
must be user-owned and not group/other writable; newly created directories use
`0700`. For unprivileged users, existing caller-owned intermediate directories
may retain their usual permissions, including `0775`. Root-owned ancestors
retain the stricter checks. The shared core rejects stale snapshots
and serializes cooperating local CLI/tray writers with a directory lock; it does
not promise cross-host NFS locking or compare-and-swap against unrelated editors.
A failure before replacement leaves the existing bytes unchanged. Exit `4` means
replacement occurred without confirmed durability, or an I/O failure prevented
determining its outcome; inspect the saved state before retrying.
Canonical serialization preserves managed
values and sections, while replacing comments and formatting.

## Files and strict INI subset

The default files are:

1. `/etc/crystalhd/config.ini` for system defaults;
2. `$XDG_CONFIG_HOME/crystalhd/config.ini` for user defaults, falling back to
   `$HOME/.config/crystalhd/config.ini` when `XDG_CONFIG_HOME` is unset.

A missing file contributes no layer.  Merely installing CrystalHD must not
create a file or change the current defaults.

Each present file is UTF-8 without a byte-order mark and at most 64 KiB.
LF and CRLF line endings are accepted.  Blank lines and whole-line comments
whose first non-space character is `#` are accepted.  Inline comments,
semicolon comments, interpolation, line continuation and key-name folding are
not supported.

The only section forms are:

```ini
[crystalhd]
schema = 1
preset = "default"
profile = "moonlight"

[settings]
vaapi.h264.mode = "bounded"
gstreamer.crystalhddec.scale_width = 0

[profile.moonlight]
vaapi.h264.mode = "live"
vaapi.h264.low_latency = true
```

`[crystalhd]` and `schema = 1` are mandatory.  `preset` and `profile` are
optional selectors.  `[settings]` is the file's base overlay.  A
`[profile.NAME]` section is inert until that exact, case-sensitive `NAME` is
selected.  Names match `[A-Za-z0-9][A-Za-z0-9._-]{0,63}`.  Duplicate sections
or keys are errors.

Whitespace surrounding `=` is ignored.  Values are one of:

- the exact booleans `true` and `false`;
- an unsigned decimal integer written as `0` or without a leading zero;
- a double-quoted JSON string; or
- the exact token `unset`.

Omitting a key inherits the next lower layer.  `unset` instead blocks a lower
configured value and restores that control's native/unset behavior.  It is
therefore different from omission, `false`, integer `0` and the empty string
`""`.  A schema adapter must preserve those distinctions when translating to
an existing public interface.

Unknown sections, metadata keys, setting keys, value types and schema versions
are errors.  An error identifies the file role and line and rejects the
complete managed configuration; no profile, preset or partial file is
accepted.  Any future managed launcher must fail before opening a decoder.
Existing programs which do not use the manager retain their current behavior.

## Canonical keys and existing targets

The canonical type is stricter than the raw target where the current target
accepts ambiguous strings.  Detailed owner, device, lifecycle and safety
information remains in the production inventory.

| Canonical key | Type and accepted values | Existing target and translation |
| --- | --- | --- |
| `libcrystalhd.rx_pool_count` <!-- schema-key:libcrystalhd.rx_pool_count --><!-- schema-map:environment:CRYSTALHD_EXPERIMENTAL_RX_POOL_COUNT=libcrystalhd.rx_pool_count --> | Integer enum: `8`, `16` | `CRYSTALHD_EXPERIMENTAL_RX_POOL_COUNT`; decimal string |
| `vaapi.h264.mode` <!-- schema-key:vaapi.h264.mode --><!-- schema-map:environment:CRYSTALHD_VAAPI_LIVE_H264=vaapi.h264.mode --> | String enum: `"bounded"`, `"strict"`, `"live"` | `CRYSTALHD_VAAPI_LIVE_H264`; respectively absent, a non-`1` value, or `1` |
| `vaapi.h264.low_latency` <!-- schema-key:vaapi.h264.low_latency --><!-- schema-map:environment:CRYSTALHD_VAAPI_LOW_LATENCY_H264=vaapi.h264.low_latency --> | Boolean: explicit prefeed request | `CRYSTALHD_VAAPI_LOW_LATENCY_H264`; true is `1`, false is absent, allowing automatic per-AU selection; an explicit non-`1` raw value suppresses automatic prefeed |
| `vaapi.h264.ready_remap` <!-- schema-key:vaapi.h264.ready_remap --><!-- schema-map:environment:CRYSTALHD_VAAPI_EXPERIMENTAL_READY_REMAP=vaapi.h264.ready_remap --> | Boolean | `CRYSTALHD_VAAPI_EXPERIMENTAL_READY_REMAP`; true is `1`, false is absent |
| `vaapi.h264.ready_policy` <!-- schema-key:vaapi.h264.ready_policy --><!-- schema-map:environment:CRYSTALHD_VAAPI_EXPERIMENTAL_READY_POLICY=vaapi.h264.ready_policy --> | String enum: `"previous"`, `"latest"` | `CRYSTALHD_VAAPI_EXPERIMENTAL_READY_POLICY` |
| `vaapi.h264.ready_delay_us` <!-- schema-key:vaapi.h264.ready_delay_us --><!-- schema-map:environment:CRYSTALHD_VAAPI_EXPERIMENTAL_READY_DELAY_US=vaapi.h264.ready_delay_us --> | Integer enum: `0`, `16667`, `33333`, `50000` | `CRYSTALHD_VAAPI_EXPERIMENTAL_READY_DELAY_US`; decimal string |
| `vaapi.i915_wc` <!-- schema-key:vaapi.i915_wc --><!-- schema-map:environment:CRYSTALHD_VAAPI_EXPERIMENTAL_I915_WC=vaapi.i915_wc --> | Boolean | `CRYSTALHD_VAAPI_EXPERIMENTAL_I915_WC`; true is `1`, false is absent |
| `vaapi.debug` <!-- schema-key:vaapi.debug --><!-- schema-map:environment:CRYSTALHD_VAAPI_DEBUG=vaapi.debug --> | Boolean | `CRYSTALHD_VAAPI_DEBUG`; true is present, false is absent. Never encode false as `0`, because any presence currently enables debug |
| `vaapi.trace` <!-- schema-key:vaapi.trace --><!-- schema-map:environment:CRYSTALHD_VAAPI_TRACE=vaapi.trace --> | Boolean | `CRYSTALHD_VAAPI_TRACE`; true is `1`, false is absent |
| `chromium.executable` <!-- schema-key:chromium.executable --><!-- schema-map:environment:CRYSTALHD_CHROMIUM=chromium.executable --> | Nonempty path string | `CRYSTALHD_CHROMIUM`; `unset` retains launcher autodetection |
| `chromium.drm_device` <!-- schema-key:chromium.drm_device --><!-- schema-map:environment:CRYSTALHD_DRM_DEVICE=chromium.drm_device --> | Nonempty path string | `CRYSTALHD_DRM_DEVICE`; `unset` retains `/dev/dri/renderD128` |
| `chromium.password_store` <!-- schema-key:chromium.password_store --><!-- schema-map:environment:CRYSTALHD_CHROMIUM_PASSWORD_STORE=chromium.password_store --> | String, including `""` | `CRYSTALHD_CHROMIUM_PASSWORD_STORE`; absent selects `basic`, while empty omits the browser flag |
| `chromium.profile_dir` <!-- schema-key:chromium.profile_dir --><!-- schema-map:environment:CRYSTALHD_CHROMIUM_PROFILE_DIR=chromium.profile_dir --> | Nonempty path string | `CRYSTALHD_CHROMIUM_PROFILE_DIR`; `unset` uses the browser default |
| `chromium.native_gl` <!-- schema-key:chromium.native_gl --><!-- schema-map:environment:CRYSTALHD_CHROMIUM_NATIVE_GL=chromium.native_gl --> | Boolean | `CRYSTALHD_CHROMIUM_NATIVE_GL`; true is `1`, false is absent |
| `chromium.experimental_hw_decode` <!-- schema-key:chromium.experimental_hw_decode --><!-- schema-map:environment:CRYSTALHD_CHROMIUM_EXPERIMENTAL_HW_DECODE=chromium.experimental_hw_decode --> | Boolean | `CRYSTALHD_CHROMIUM_EXPERIMENTAL_HW_DECODE`; true is `1`, false is absent |
| `chromium.disable_gpu_sandbox` <!-- schema-key:chromium.disable_gpu_sandbox --><!-- schema-map:environment:CRYSTALHD_CHROMIUM_DISABLE_GPU_SANDBOX=chromium.disable_gpu_sandbox --> | Boolean | `CRYSTALHD_CHROMIUM_DISABLE_GPU_SANDBOX`; explicit acknowledgement, true is `1` |
| `chromium.h264_policy_ack` <!-- schema-key:chromium.h264_policy_ack --><!-- schema-map:environment:CRYSTALHD_CHROMIUM_H264_POLICY=chromium.h264_policy_ack --> | Boolean | `CRYSTALHD_CHROMIUM_H264_POLICY`; acknowledgement only, true is `1` |
| `player.python` <!-- schema-key:player.python --><!-- schema-map:environment:CRYSTALHD_PLAYER_PYTHON=player.python --> | Nonempty path string | `CRYSTALHD_PLAYER_PYTHON`; `unset` retains `/usr/bin/python3` |
| `gstreamer.crystalhddec.scale_width` <!-- schema-key:gstreamer.crystalhddec.scale_width --><!-- schema-map:gst-property:scale-width=gstreamer.crystalhddec.scale_width --><!-- schema-map:player-flag:--scale-width=gstreamer.crystalhddec.scale_width --> | `0`, or an even integer from `128` through `1918` | `crystalhddec scale-width`; `crystalhd-play --scale-width` is the explicit player override |
| `player.decoder` <!-- schema-key:player.decoder --><!-- schema-map:player-flag:--hardware=player.decoder --><!-- schema-map:player-flag:--software=player.decoder --> | String enum: `"hardware"`, `"software"` | Mutually exclusive `crystalhd-play` flags; native value is hardware |
| `player.software_output_size` <!-- schema-key:player.software_output_size --><!-- schema-map:player-flag:--software-output-size=player.software_output_size --> | String `"WIDTHxHEIGHT"`; positive even dimensions at most `4096`, total at most `4096x2160` | `crystalhd-play --software-output-size` |

These current surfaces deliberately do not become ordinary merged keys:

| Surface | Boundary |
| --- | --- |
| `force_l0s_off` <!-- schema-boundary:module:force_l0s_off=staged-module --> | Inspect or stage separately; never claim an immediate per-application change |
| `session_l0s_off` <!-- schema-boundary:module:session_l0s_off=staged-module --> | Same separate module-load lifetime and topology checks |
| `CRYSTALHD_CHROMIUM_CONFIG` <!-- schema-boundary:environment:CRYSTALHD_CHROMIUM_CONFIG=legacy-bootstrap --> | Selects the existing trusted shell file; it cannot recursively select the strict schema |
| `crystalhd-play --subtitles` <!-- schema-boundary:player-flag:--subtitles=invocation-only --> | Per-media invocation input, never a saved preset/profile value |
| `crystalhd-play --weave` <!-- schema-boundary:player-flag:--weave=invocation-only --> | Per-invocation interlaced passthrough selection, never a saved preset/profile value |

External loader selectors such as `LIBVA_DRIVER_NAME`, `VAAPI_FORCE_INDIRECT`,
`GST_PLUGIN_PATH` and `LD_LIBRARY_PATH` remain owned by their respective
runtimes and are not schema keys.

## Reviewed preset manifest

The versioned catalog is installed as
`$DATADIR/crystalhd/config-presets-v1.json`. Source checkouts and source
archives use the same tracked sidecar without requiring `.git`. The tool reads
one compiled relative location with the same bounded, non-following regular-file
checks as configuration input; it never searches the current directory,
`HOME`, an environment override or a host installation as a fallback.

`crystalhd-config list-presets` reports the manifest without selecting or
applying an entry. The v1 entries are deliberately narrow:

| Name | Status / safety | Exact preset settings | Selection boundary |
| --- | --- | --- | --- |
| `default` | SELECTABLE / STANDARD | Empty compatibility layer | No acknowledgement |
| `quality` | SELECTABLE / STANDARD | `gstreamer.crystalhddec.scale_width = 0` | Native-width output; no quality-improvement claim |
| `performance` | UNAVAILABLE / EXPERIMENTAL | None | There is no universal media-safe scaling target; `1280` is not made global |
| `low-memory` | SELECTABLE / EXPERIMENTAL | `libcrystalhd.rx_pool_count = 8` | Requires `--acknowledge-risk low-memory-8-buffer`; retained measurement is BCM70015-only |
| `streaming` | SELECTABLE / EXPERIMENTAL | H.264 `live`, low latency on, READY remap off | Requires `--acknowledge-risk bcm70015-live-h264-drain`; BCM70015 H.264 VA, later input or explicit drain, and external loader/presentation setup remain requirements |
| `experimental` | UNAVAILABLE / EXPERIMENTAL | None | Mixed READY-remap, i915, Chromium-sandbox, diagnostic and link-power risks are not bundled |
| `custom` | VIRTUAL / DERIVED | None | Cannot be selected; reports are the selected real preset plus per-key override origins |

The Streaming entry does not configure Moonlight, V-sync, pacing, loader
paths, PCIe L0s, i915 WC, physical A/V synchronization or presentation. The
Quality entry is an explicit native-output setting, not a benchmark result.
Unavailable entries remain visible so a familiar marketing name cannot
silently degrade into a different collection of options.

## Selection and merge order

Preset and profile selection is a separate pass from value merging.  For each
selector, an explicit manager command wins over the user file, which
wins over the system file.  With no preset selection, `default` means an empty
compatibility preset.  With no profile selection, no named profile is active.
An unknown selected preset or profile is an error.

Only `SELECTABLE` manifest entries participate in merging. Experimental
selection requires the entry's exact command-only acknowledgement on every
invocation; it never becomes implicit through a file, environment variable or
another preset.

The requested target state uses this exact low-to-high precedence:

1. the component's compiled/native fallback;
2. the selected preset manifest;
3. system `[settings]`;
4. user `[settings]`;
5. the selected system `[profile.NAME]`;
6. the selected user `[profile.NAME]`;
7. an existing setting-specific process environment variable; and
8. an explicit application command-line option or GObject property.

The system file supplies defaults, not an enforcement policy.  More specific
user, environment and application choices therefore win.

There are two deliberately separate resolution steps.  First, the preset,
system, user, selected profiles and explicit canonical application settings
form one strict managed transaction.  It is type-checked and cross-checked
without using raw process-environment strings to repair a contradiction.
Second, existing setting-specific environment variables are adapted at their
native public-interface boundary, before the higher-priority application
override.  They are not strict-INI input and are never normalized, rewritten
or imported into a managed file.

A resolution report distinguishes:

- the target request and origin after precedence;
- the exact raw environment request and whether its native adapter accepted
  it, subject to the diagnostic redaction rules below;
- the effective value plus active, inactive or fallback reason; and
- a native application rejection, when the existing program refuses the
  combination rather than running with a fallback.

For example, a valid managed `live` request with low latency and READY remap
enabled remains a valid transaction. If raw `CRYSTALHD_VAAPI_LIVE_H264=0`
then wins, the target request is `strict` and its raw environment origin
remains visible. Explicit prefeed stays requested, but READY remap becomes
inactive. The raw override is not a strict-schema error.

Profile selection is always explicit.  The resolver must not inspect
`argv[0]`, executable basenames, process names, desktop-file names, media paths
or undocumented application internals.  A profile cannot select another
profile or preset, which prevents recursive/cyclic selection.

The resolved userspace settings are snapshotted before the relevant
driver/library/plugin session is created.  They are not reread per frame.

## Cross-setting validation

Explicit H.264 prefeed is independent of live mode. A true
`vaapi.h264.low_latency` is reported as conditional on the uninspected BCM70015
H.264 prefeed path, not as proof of runtime activation. False means
`explicit-prefeed:not-requested`, not that automatic prefeed is disabled:
the inspector does not inspect per-AU progressive/MBAFF eligibility. An absent
native variable permits automatic prefeed; any explicit non-`1` raw value
suppresses it. Native behavior is in the
[VA environment inventory](CONFIGURATION.md#va-api-environment).

The current inspector rejects the following combinations in canonical preset,
system, user, selected-profile and application layers:

- `vaapi.h264.ready_remap = true` unless mode is `live` and low latency is
  true;
- `chromium.experimental_hw_decode = true` unless
  `chromium.disable_gpu_sandbox = true` explicitly acknowledges the current
  launcher requirement; and
- a nonzero `gstreamer.crystalhddec.scale_width` unless the player decoder is
  hardware; and
- a non-unset `player.software_output_size` unless the decoder is hardware and
  `gstreamer.crystalhddec.scale_width` is nonzero.

READY policy/delay values may be stored while remapping is false, but must be
reported as inactive.  Device/backend conditions which cannot be known from a
file—BCM70015 scaling, coded width, link topology and DRM/i915 availability—are
reported as conditional or unsupported during effective-setting inspection;
they are never treated as silently successful.

Raw target inspection follows that strict transaction. A non-`1` raw live-mode
value selects strict replay without deactivating explicit prefeed. READY remap
still requires live mode and explicit prefeed. An unsupported raw READY policy
or delay disables remapping while preserving the
exact rejected value and environment origin in diagnostics.  An invalid raw
RX-pool value falls back to 16 with a fallback receipt.  The Chromium launcher
does not silently deactivate raw experimental hardware mode without the raw
sandbox acknowledgement: it rejects that launch, so the report records a
native application rejection and no successful launched state.  None of these
raw outcomes weakens the transactional errors for the same contradiction in a
managed file, preset, profile or canonical application setting.

## File integrity and privileges

The manager opens a config file through its validated containing-directory
descriptor without following the final symlink, then validates the already-open
file with `fstat` before parsing it.

- A system file and its containing CrystalHD directory are root-owned regular
  objects with no group/other write bits.
- A user file is a regular file owned by the real/effective user, with no
  group/other write bits.  Its containing CrystalHD directory has the same
  ownership and write restriction.
- A user file is not read when real and effective user or group IDs differ.
- A writer creates user files as mode `0600` and uses a same-directory atomic
  replacement only after the replacement validates.

Readability alone is not sufficient.  An unsafe owner, type, symlink or mode
is a hard configuration error, not a reason to consume the file partially.
Configuration contains no password-storage secret. Diagnostics redact raw
environment values and non-compiled user paths which are not required to
explain a result.

Kernel module parameters retain their current module-load lifetime.  A future
tool may display their effective sysfs state or prepare a separately confirmed
`modprobe.d` change, but it must not auto-unload the module, modify global ASPM,
or represent a saved value as already applied.

## Legacy Chromium boundary

`crystalhd-chromium` currently sources the readable file selected by
`CRYSTALHD_CHROMIUM_CONFIG` as shell code, using the launcher's current
privileges.  Assignments in that file override inherited launcher variables.
Direct launcher use preserves that compatibility behavior.

The strict v1 manager must never parse, source or automatically migrate this
shell file.  A future **explicit managed invocation** requires an additive
launcher mode which does not source the legacy file.  That mode is not
implemented today; until it exists, the manager must not claim that it can
launch Chromium with a fully determined effective configuration.  No process
name matching or implicit bypass is permitted.

## Versioning and migration

Version 1 accepts only `schema = 1`.  Missing, older and newer versions are not
guessed.  A future migration command must:

1. parse and validate the source under its declared version;
2. show dropped, renamed and changed values before writing;
3. write a separate same-directory candidate and keep a recoverable backup;
4. validate the candidate under the destination version; and
5. replace the original atomically only after explicit confirmation.

There is no automatic import from shell startup files, `chromium.conf`, the
current process environment or `/etc/modprobe.d`.  Existing interfaces remain
valid and continue to take precedence according to their documented lifetime.
