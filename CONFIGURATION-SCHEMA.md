# CrystalHD configuration schema v1

This document specifies the configuration contract implemented by the
`crystalhd-config` inspector, user-config storage, optional command launcher and
native VA/DIL readers and the player adapter. Updated native components consume supported defaults on
application startup; saving does not reconfigure an already-running process.
`run` additionally resolves explicit invocation overrides and experimental risk
acknowledgements. Exact native controls are in [CONFIGURATION.md](CONFIGURATION.md).

The v1 goals are deliberately narrow:

- preserve every existing environment variable, module parameter, command
  line option, GStreamer property and public ABI;
- make an effective value and its origin explainable without guessing an
  application from its process name;
- reject malformed or unsafe configuration as one transaction rather than
  applying a convenient subset; and
- keep privileged module configuration outside an ordinary userspace
  playback profile.

The CLI/storage implementation is `scripts/crystalhd-config.py`; the shared
native reader is `include/crystalhd-config.h`. Its compiled schema/preset table
is generated from that CLI and the reviewed preset catalog by
`scripts/generate-native-config.py`. Native builds require no Python interpreter;
`configuration-check` checks generated-data freshness and differential parser/
resolver behavior. Test code is never installed as a parser.

## Native defaults in ordinary applications

The native backend integration makes a plain `kodi` invocation consume saved
defaults when it loads **our updated** VA driver/library. It does not replace
Kodi, select its decoder, change an existing process, or require a widget.
The widget will edit the same file through the existing storage/validation core;
it will not introduce a second preset system or a `config run` requirement.

The native reader has this contract:

- Read `/etc/crystalhd/config.ini`, then the existing absolute XDG/HOME user
  path. Missing files contribute nothing. Use schema v1's grammar, selectors,
  complete-file validation, merge order, `unset`, and cross-setting checks,
  including validation of inactive profiles. Do not guess an application profile
  from a process name. The reviewed preset/schema data must agree with the CLI.
- Snapshot managed defaults once per loaded component on first configuration
  use; restart the application after saving. No file reads per frame, Python
  subprocess, file writes, process-wide environment mutation, or public ABI/ioctl
  change. The VA driver and DIL can load at different times; this is not a
  cross-component atomic reload protocol.
- Preserve the existing trusted-file checks (absolute path, safe immediate
  parent and file owner/mode, no final symlink or special file, bounded 64 KiB
  read). Ignore managed defaults when real/effective IDs differ. Discard an
  invalid managed transaction as a whole, emit one redacted diagnostic per
  component snapshot, and retain original environment/native behavior. Do not
  turn a configuration error into an application crash or a partial overlay.
- Apply only native-owned adapters: RX pool count, H.264 mode/prefeed, READY
  remap off, i915 WC off, VA debug and trace. Other valid keys remain saved for
  their owning frontend; this does not make Chromium/player/GStreamer properties
  automatically active. READY policy/delay and centered output remain native
  environment controls, outside this initial managed adapter.
- Existing raw environment presence wins **per key**, even for empty/invalid
  values. Keep its original interpretation: DEBUG presence including `0` is on;
  prefeed `0` suppresses automatic prefeed whereas canonical false removes only
  the explicit request. `unset` blocks lower file layers, not raw environment.
  Supported explicit application CLI/property overrides remain highest priority.
- Do not automatically activate experimental managed settings. Experimental
  presets still need fresh CLI risk acknowledgements through `config run`;
  managed RX=8, LIVE/prefeed on, READY on or WC on without an overriding raw
  environment discard the native managed transaction. Explicit legacy environment
  opt-ins retain their existing semantics. A safe managed change must also not
  newly activate an inherited READY experiment.
- `config run` remains the optional adapter for explicit paths, per-invocation
  selectors, acknowledgements and CLI overrides. Its child must bypass native
  file consumption using `CRYSTALHD_CONFIG_DISABLE=1`, so an explicit false,
  `unset`, suppressed layer or alternate path cannot be undone by rereading
  defaults. A default invocation with no managed request and no custom selection
  keeps the inherited environment unchanged. The bypass is read only at the
  native snapshot boundary; exactly `1` bypasses both managed file layers.
  It is also a diagnostic escape hatch, not a risk acknowledgement.

Completion requires differential native/CLI parsing and resolution tests,
trusted-file failure tests, per-key raw override and false/unset regression
tests, and strict native builds including scalar i686. Exercise the actual VA
entry path with isolated settings and a stock client before claiming plain
`kodi` integration. Do not install into the stable system for these checks.

## Defaults in crystalhd-play

The player adapter reads `player.decoder`, `gstreamer.crystalhddec.scale_width`
and `player.software_output_size` once before constructing a pipeline. It uses
the existing secure file reader, preset selection, profile precedence and
resolver. Each explicitly supplied player flag overrides only its own key;
an absent flag inherits the saved value. With no files, retain hardware decode,
native output width and no software resize. `--weave` and subtitles remain
invocation-only. Other applications and GStreamer pipelines are unaffected.

`--scale-width 0` explicitly requests native card width;
`--software-output-size native` explicitly removes a saved software resize.
Do not silently erase dependent saved keys when another key is overridden.
An incoherent final request fails before starting a pipeline and explains the
reset flags. For example, a saved scaled/upscaled hardware profile needs
`--software --scale-width 0 --software-output-size native` for a one-shot
software decode. This never rewrites the saved file.

Unsafe/malformed files, unavailable core/catalog, ID mismatch or an experimental
preset without transient acknowledgement discard the managed player overlay,
with a redacted diagnostic, leaving original CLI/native defaults. A conflict
introduced by explicit flags is an error, not a reason to silently drop valid
saved settings. Exactly `CRYSTALHD_CONFIG_DISABLE=1` bypasses managed files, also
honoring `config run`'s existing bypass. Do not execute a saved `player.python`,
rewrite environment, or add a new risk acknowledgement path in the player.

The trusted configuration core has a fixed source/installed location, independent
of PATH. Staged installation must bind the controller and core paths correctly
for separate BINDIR/DATADIR and spaces. The startup report labels requested values
and redacted origin categories; actual hardware success still requires decoded
output. Qualify precedence/reset/conflict/security, actual software pipeline/EOS,
staged paths and bounded native output with existing fixtures; no physical A/V
retake is needed when decode/output algorithms are unchanged.

## Optional settings window and tray

`crystalhd-settings` is an optional GTK3 editor over the existing Python core.
Usage and optional installation are in
[CONFIGURATION.md](CONFIGURATION.md#optional-desktop-settings).
It edits persistent user defaults; ordinary Kodi startup uses the native reader
above. There is no application-launch button or mandatory `config run` workflow.

- Open a settings window and, when a StatusNotifier host and Ayatana indicator
  are available, a tray menu for reopening the window or exiting. Without a host
  or indicator dependency, keep the window usable and exit when it is closed.
  Losing the tray connection must reveal the window, never strand a hidden
  process. No autostart service is installed or required. `--no-tray` selects a
  window-only session.
- Offer reviewed standard presets, explicit selection/editing of profiles and
  safe native-owned setting values. Show unavailable, virtual, experimental and
  frontend-only settings with their actual limitations; do not expose an inert
  activation toggle or turn launch-only experiments into automatic defaults.
  Preserve all other settings and profile sections when saving.
- Distinguish inheritance (remove this override), `unset` (block lower file
  layers), and explicit false/zero. Display configured/model values and origins,
  including environment overrides, without claiming a running app was inspected.
  Existing selected profiles may override a base edit; show that provenance.
- Use the existing edit snapshot and atomic writer for Save and Reset. Reload
  explicitly; never silently discard edits or overwrite a newer writer. Reset
  requires confirmation and clears only the user overlay. An invalid but safely
  readable file can be reset; unsafe files cannot bypass the core checks.
- Report committed-but-unconfirmed or uncertain writes distinctly. Do not
  automatically retry or claim that old bytes survived. Require Reload before
  another write when the outcome/readback is uncertain.
- Label successful writes as saved defaults requiring application restart, with
  installed native-reader availability and active playback state unconfirmed.
  Saving does not change the settings process's environment, restart another app,
  reload a module, or modify system configuration/firmware/PCIe policy.
- Keep GUI imports/dependencies out of the CLI and model. Preserve Python 3.6
  compatibility, keyboard access, textual labels/status, translatable messages
  and an optional desktop launcher. Follow the selected Concept C crystal/slider
  identity; inspect real small-size assets before installing them.

Qualification covers headless model/storage parity, stale and uncertain saves,
real GTK editing and keyboard/close behavior with isolated files, tray-host
fallback/disconnection, and staged launcher/icon install/uninstall. The already
qualified native reader and Kodi proof are reused unless runtime code changes.

### Named-profile editing contract

The editor supports explicit creation, editing and removal of user
profile sections through the same core as the CLI. `save --profile-section NAME`
patches that section (creating an empty section when needed); `--profile NAME`
continues to select the default and never means an editing target. Base settings
remain the target when `--profile-section` is omitted. `--remove KEY` removes
only the targeted setting; selector removals retain their existing meaning.
Validate the edited profile independently of the current default before writing.

`delete-profile NAME` removes only an existing user section. If it was the user
default and has no system definition, also remove that user selector, reverting
to inherited selection. Retain the selector when the system still defines that
profile. Reject a resulting dangling inherited selection through normal writer
validation; never edit system configuration to make deletion succeed.

The window distinguishes **Editing layer** from **Default profile**. Changing
the editing layer previews that profile but does not select it for applications
or discard other draft changes. System profiles can acquire user overrides; their
system definitions remain read-only. New/remove operations affect the draft
until Save; removal requires confirmation. Only the existing safe native controls
are editable, other settings/sections are preserved, and the preview remains a
model rather than a report of an active application. Reuse atomic save, stale
snapshot checks, risk acknowledgements and uncertain-write handling unchanged.

### Player controls, wording and tray startup

The window adds a Player page for the three already consumed player keys. Use
the shared validator/writer and current editing scope. Width and output size
accept custom values, without limiting writable dimensions to a few presets.
Separate inherited values, built-in defaults and explicit input. Keep incomplete
or invalid text while switching pages/profiles; indicate the error and block
Save until it is corrected or explicitly discarded. Reload/Quit must also notice
such unfinished input. A valid multi-key draft may temporarily have conflicting
dependencies; resolve them before Save. No automatic clearing of other keys.

Buttons describe their effects: Save changes, Reload file, Clear all user
settings, Hide to tray and Quit. Clearing user data remains confirmed and never
means clearing system defaults. Tooltips explain inheritance, built-in defaults,
profile selection versus editing, and supported application/restart scope. A
standard Player-only preset must be labeled with its actual owner.

`--tray` starts hidden only after a functioning indicator/host is connected.
Without tray support, show a usable window; host loss reveals it without losing
draft text, and host recovery never hides it again unexpectedly. Ordinary launches
reuse the same desktop instance and reopen its window. Explicit file overrides
and window-only runs remain isolated. The desktop launcher uses tray startup;
no login autostart or system service is added. Validate real panel registration,
hide/reopen, single-instance activation, host-loss fallback and no startup write.

### Installed-reader declarations

`crystalhd-config installation` and the window's Installation page inspect
component files without loading their code or opening a decoder. A fixed v1
declaration identifies the managed-defaults reader in DIL, VA and Player; it is
build metadata, not a new exported ABI or a statement about a running process.
An absent declaration means legacy/unknown, not proof that every custom build
lacks a reader. Reader declarations and their owner-specific keys are fixed by
the protocol; unsupported versions remain unknown.

Inspect the PATH-selected Player wrapper's literal controller location and
ELF-class-compatible DIL/libva cache candidates. Invoke only the fixed system
linker-cache listing tool with `-p`, a bounded timeout and a clean environment;
never execute the inspected Player/library/driver. Follow ordinary library
symlinks, read only regular bounded files and retain missing/read-error states.
Report loader/environment overrides and the settings process's ABI explicitly.
File availability does not prove which libraries another application maps.

Refresh is read-only and does not save defaults or restart applications. After
installation, qualify GUI-written temporary defaults through ordinary installed
Player/native clients, actual hardware output, per-key invocation overrides,
EOS and reopen. Reuse existing codec/pixel qualification when code is unchanged.
Keep module/firmware/PCIe policy unchanged unless a separately demonstrated
compatibility dependency requires work there.

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
file nor an environment variable can supply it. Ordinary native file reading
does not acknowledge experimental presets; use the optional `run` command.
The path-locating `XDG_CONFIG_HOME` or `HOME` must still be absolute unless the
user layer is explicitly suppressed. Text and
JSON formats are available for `show-effective`, `explain` and `list-presets`.
`bug-report` is JSON-only.

Exit status `0` means success, `2` is command-line syntax, an unknown canonical
key or an unknown/duplicated acknowledgement token, and `3` is an unsafe,
unreadable or invalid configuration, selector, missing/inapplicable required
acknowledgement, or cross-setting transaction. Write commands additionally use
`4` when replacement committed without confirmed durability, or its outcome
cannot be determined; this is not a claim that the previous bytes survived.

Inspection commands are diagnostic only: selecting a preset changes the reported
target transaction, not a running application. Inspection does not launch a
program or access hardware. The utility never reloads a module or sources
`chromium.conf`. Inspection's last observable layer is the current process environment. A
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
already-running applications do not change. Updated native VA/DIL components
consume their supported defaults on the next application start, including an
ordinary `kodi` launch. Other owners' settings remain saved-only until adapted.
The optional `run` adapter supplies per-invocation overrides and acknowledged
experiments without editing the file.

```sh
crystalhd-config save vaapi.debug=false
crystalhd-config save 'chromium.password_store=""'
crystalhd-config save --remove vaapi.debug
crystalhd-config reset
```

`save KEY=VALUE` patches the base `[settings]` overlay using the strict value
grammar below. Unspecified settings and profile sections remain intact.
With `--profile-section NAME`, patch that user profile instead, creating it if
needed; an empty invocation creates an empty profile without selecting it.
`--remove KEY` removes an override in the targeted layer so lower layers can supply it;
`KEY=unset` explicitly blocks a lower configured value. Neither is equivalent to
`false`, `0`, or `""`. `--preset NAME` and `--profile NAME` save metadata selectors;
they do not edit a named profile's settings. `--remove preset` or
`--remove profile` clears that selector. Experimental presets require their
explicit risk acknowledgements on each write; acknowledgement tokens and current
setting-specific environment values are never copied into the file.
`delete-profile NAME` removes a user section, retaining any system definition;
the [profile contract](#named-profile-editing-contract) specifies default-selector
handling. It uses the same validation and atomic writer as Save/Reset.

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

## Run an explicit command

```sh
crystalhd-config run -- kodi
crystalhd-config run --override vaapi.debug=false -- kodi
```

The literal `--` separates manager options from the command and its arguments.
`run` prepares a child environment and executes the command without a shell.
When it resolves managed requests or explicit layer/selector options, it sets
`CRYSTALHD_CONFIG_DISABLE=1` in that child to prevent native rereading from
undoing false/unset or a selected alternate path.
It reports **REQUESTED FOR THIS CHILD**, not active or confirmed. An application
which forwards to an already-running instance cannot transfer these settings to
that instance. Nothing is saved, and no background manager is required.
The command's exit status and signals are preserved. Failure to find the command
returns `127`; other execution failures return `126`. The status notice uses
stderr when available and leaves command stdout untouched.

Explicit process environment values retain their native encoding and precedence;
only `--override KEY=VALUE` supersedes the corresponding key for this invocation.
This includes empty and invalid native values: the adapter does not replace them
with the inspector's effective fallback. With no managed requests or overrides,
the child inherits the environment unchanged. Application CLI/GObject inputs
retain their own higher priority.

The adapter supports RX pool count, H.264 mode and explicit prefeed,
VA debug/trace, `player.python`, and bounded Chromium launcher keys for the
executable, DRM path, password store, profile directory and native GL. A
managed Chromium request also sets `CRYSTALHD_CHROMIUM_CONFIG_DISABLE=1`, so
the launcher's legacy shell file cannot override the already resolved strict
layers. READY remap, i915 WC and Chromium native GL may be requested off or
unset, but not activated. A new managed RX8 request requires the transient
`low-memory-8-buffer` acknowledgement; a new managed LIVE/prefeed request requires
`bcm70015-live-h264-drain`. Supply the exact union of these and any selected
preset's required `--acknowledge-risk TOKEN` values. Tokens are never inferred
from the saved file or inherited environment. Canonical prefeed `false` removes
its variable; inherited raw `0` remains an explicit native auto-prefeed opt-out.

Managed READY policy/delay changes, Chromium hardware-decode/sandbox/policy
acknowledgements, and the three player/GStreamer CLI/GObject-only keys have no
adapter in this command and are rejected before execution, even for `unset` or
a native-valued request. A schema-selectable preset
such as Quality can therefore be unavailable to this adapter. It also rejects
changes which would indirectly activate inherited READY remapping. Use
`show-effective` to inspect the configured layers; an unsupported request is not
silently treated as applied.

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
| `chromium.password_store` <!-- schema-key:chromium.password_store --><!-- schema-map:environment:CRYSTALHD_CHROMIUM_PASSWORD_STORE=chromium.password_store --> | String, including `""` | `CRYSTALHD_CHROMIUM_PASSWORD_STORE`; absent or empty preserves the browser/platform default, while a nonempty value selects that upstream backend |
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
| `CRYSTALHD_CONFIG_DISABLE` <!-- schema-boundary:environment:CRYSTALHD_CONFIG_DISABLE=native-config-bootstrap --> | Exactly `1` bypasses native managed-file consumption; not a saved setting or risk acknowledgement. `config run` sets it for its already-resolved child when needed |
| `CRYSTALHD_CHROMIUM_CONFIG` <!-- schema-boundary:environment:CRYSTALHD_CHROMIUM_CONFIG=legacy-bootstrap --> | Selects the existing trusted shell file; it cannot recursively select the strict schema |
| `CRYSTALHD_CHROMIUM_CONFIG_DISABLE` <!-- schema-boundary:environment:CRYSTALHD_CHROMIUM_CONFIG_DISABLE=managed-launch-boundary --> | Exactly `1` prevents the launcher from sourcing the legacy shell file. `config run` sets it only when a strict Chromium launcher key participates; it is not a saved setting or risk acknowledgement |
| `CRYSTALHD_VAAPI_EXPERIMENTAL_CENTERED_NV12` <!-- schema-boundary:environment:CRYSTALHD_VAAPI_EXPERIMENTAL_CENTERED_NV12=experimental-centered-nv12 --> | Native-only centered NV12 experiment; no canonical key, profile value, preset or manager translation. The VA driver alone validates its exact syntax and context/media eligibility |
| `crystalhd-play --subtitles` <!-- schema-boundary:player-flag:--subtitles=invocation-only --> | Per-media invocation input, never a saved preset/profile value |
| `crystalhd-play --weave` <!-- schema-boundary:player-flag:--weave=invocation-only --> | Per-invocation interlaced passthrough selection, never a saved preset/profile value |
| `crystalhd-play --trace-frames` <!-- schema-boundary:player-flag:--trace-frames=invocation-only --> | Explicit new diagnostic output file for one playback; never a saved preset/profile value |
| `crystalhd-play --trace-limit` <!-- schema-boundary:player-flag:--trace-limit=invocation-only --> | One playback's bounded record retention; requires explicit trace output |
| `crystalhd-play --trace-window` <!-- schema-boundary:player-flag:--trace-window=invocation-only --> | One playback's optional rolling capture; requires explicit trace output and is never saved in a preset |
| `crystalhdstagetrace capacity` <!-- schema-boundary:gst-property:capacity=invocation-only --> | Player-owned diagnostic controller capacity, set before attachment; no managed frontend setting |
| `crystalhdstagetrace window` <!-- schema-boundary:gst-property:window=invocation-only --> | Player-owned rolling retention, set before attachment; no managed frontend setting |

External loader selectors such as `LIBVA_DRIVER_NAME`, `VAAPI_FORCE_INDIRECT`,
`GST_PLUGIN_PATH` and `LD_LIBRARY_PATH` remain owned by their respective
runtimes and are not schema keys.

The inspector observes the exact centered-NV12 variable above only as a
native-only boundary. If it is present, including an empty string or `0`, the
report marks its effective-configuration scope as partial and records its
presence with the raw value redacted. It does not parse, validate, normalize,
persist or claim acceptance of that value. Reported canonical effective
settings exclude the experiment's output geometry and VA context rejection
rules; they do not establish an ordinary native output configuration. Strict
canonical validation still runs, and `validate` explicitly states that its
success does not validate this native-only experiment. `--ignore-environment`
omits this observation together with other process-environment observations.

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

Only `SELECTABLE` manifest entries participate in merging. Experimental CLI
selection requires the entry's exact command-line acknowledgement on every
invocation; it never becomes implicit through a file, environment variable or
another preset. The optional editor cannot select experimental activations as
automatic native defaults. For an already selected experimental preset, fresh
interactive confirmation acknowledges only that Save operation; it is never
stored and does not authorize a later application launch. Managed experimental
launches still require fresh `run --acknowledge-risk` tokens.

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
shell file. An explicit managed invocation which resolves a bounded Chromium key
sets `CRYSTALHD_CHROMIUM_CONFIG_DISABLE=1` in that child; the launcher then
skips the legacy file and consumes only the resolved environment. This applies
only to that new process and does not alter an already-running browser. The
hardware-decode, sandbox-disable and H.264-policy acknowledgement keys remain
outside this adapter. No process-name matching or implicit bypass is used.

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
