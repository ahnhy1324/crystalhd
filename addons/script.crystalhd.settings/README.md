# CrystalHD settings for Kodi

Optional Linux Program add-on for stock Kodi with Python API 3.0.0 (Kodi 19+;
the qualified integration target is Kodi 21.3). The frozen
`script.crystalhd.settings-0.1.0.zip` is published with
[1.3.0-rc.2](https://github.com/ahnhy1324/crystalhd/releases/tag/v1.3.0-rc.2).
It edits CrystalHD defaults; it is not a decoder add-on.

Use it when you already run Kodi and want to edit shared CrystalHD defaults
with a remote or keyboard. To enable actual video decoding first, follow the
[Kodi setup](../../USAGE.md#kodi).

## Install

1. Install matching 1.3 CrystalHD tools and updated VA/DIL readers using the
   [RC2 settings installation](../../README.md#install-rc2-for-managed-settings). Stable 1.2 readers do
   not consume these managed defaults. `crystalhd-config` must be in Kodi's PATH
   and provide `editor-state` v1.
2. Download `script.crystalhd.settings-0.1.0.zip` from the RC2 release linked above.
3. In Kodi, choose **Add-ons → Install from zip file**, select that ZIP, then
   open **Program add-ons → CrystalHD settings**.

## Change and apply settings

Use arrows/Select/Back. Choose a preset or existing profile, edit decoder
defaults, then explicitly **Save**. Back/Cancel never saves. Re-entering reloads
the managed files; there is no separate settings.xml store.

**Saved means restart Kodi to apply.** A new video/context alone is insufficient
with current per-component snapshots. Live state is unknown; the add-on never
restarts Kodi, opens the decoder, reloads modules, or activates experiments.
Next-context reload and live IPC are separate future work.

Quality is a **Player-only** preset and does not change Kodi output. Advanced
contains diagnostic and experiment-disable requests. Launch-only experiments,
module parameters and Kodi's own decoder selection are outside the ordinary UI.

Follow defaults removes an override; built-in default saves `unset`. Inheriting
the profile means inheriting the system selection. Restore ALL requires explicit
confirmation and clears every CrystalHD user override/profile, including other
applications' settings.

## Troubleshooting

| Symptom | Action |
| --- | --- |
| Missing `crystalhd-config` or unsupported reader | Install the matching tools/readers and make the CLI visible in Kodi's PATH. A sandboxed Kodi may need a different supported installation route. |
| Save succeeds but playback is unchanged | Restart Kodi, check that it loads updated CrystalHD VA/DIL readers and review environment overrides. **Installation** reports file declarations, not live decoder use. |
| Stale file, invalid/conflicting configuration or write failure | Reload and inspect the files; the add-on reports the error without replacing a newer edit. |

An uncertain save is never retried automatically: reload and inspect first.

## Remove or build locally

Uninstall the add-on through Kodi. Shared CrystalHD configuration is preserved.
Reset it explicitly using the UI or `crystalhd-config reset` if needed; installing
or removing the add-on itself never writes configuration. Headless CLI and GTK
settings remain available. The ZIP includes LGPL-2.1-or-later license terms.

For a local build from a configured matching source tree, install the CLI with
`sudo make install-config`, then build a separate ZIP with
`make kodi-settings-package KODI_PACKAGE=/absolute/new.zip`. A current-master
build is a separate artifact from the frozen release ZIP and needs its own
qualification.

## Qualification

The [stock Kodi21.3 UI receipt](../../tests/fixtures/issue611/kodi-settings-ui-20261031.json)
records actual ZIP installation, directional/Select/Back controls, save/reset,
nonwrites and normal exit. Other Kodi builds, UI languages and spoken
accessibility require their own observations.
