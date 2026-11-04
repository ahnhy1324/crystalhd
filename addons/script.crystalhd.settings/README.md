# CrystalHD settings for Kodi

Optional Linux Program add-on for stock Kodi with Python API 3.0.0 (Kodi 19+;
the qualified integration target is Kodi 21.3). It edits shared CrystalHD
defaults; it is not a decoder add-on.

Use the matching **1.3.0 core/tools and settings ZIP** after the
[official final release entry](https://github.com/ahnhy1324/crystalhd/releases/tag/v1.3.0)
identifies the qualified package. This guide targets final 1.3/module20;
for prepublication evaluation use only an explicitly supplied exact private
core/ZIP binding. Publication and actual qualification come from the release
entry and receipts, not the add-on version label. Use the immutable 1.2 fallback
for ordinary playback if final 1.3 is unavailable; its inspector cannot edit
these managed defaults.

Use it when you already run Kodi and want to edit shared CrystalHD defaults
with a remote or keyboard. To enable actual video decoding first, follow the
[Kodi setup](../../USAGE.md#kodi).

## Install

1. Install the matching final 1.3 CrystalHD core/tools and updated VA/DIL readers
   using [matching settings installation](../../README.md#install-matching-13-tools).
   Stable 1.2 readers do not consume these managed defaults. `crystalhd-config`
   must be in Kodi's PATH and provide `editor-state` v1.
2. After publication, download `script.crystalhd.settings-0.1.0.zip` from that
   same final release entry. During preparation, select the exact ZIP identified
   by the private final-candidate binding; do not substitute a prior candidate ZIP.
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
build is a separate artifact from a frozen release ZIP and needs its own
source/ZIP binding and applicable qualification.

## Qualification and prior packages

The final 1.3 ZIP is a separately identified bundle artifact. Its add-on code,
manifest version `0.1.0` and UI behavior are unchanged from RC4; this README
change alone changes the ZIP bytes. The matching final release manifest records
the new ZIP identity. Reuse of the existing functional UI result does not claim
byte identity with a prior ZIP or a new stock-Kodi UI execution.

Published [RC3](https://github.com/ahnhy1324/crystalhd/releases/tag/v1.3.0-rc.3)
and [RC2](https://github.com/ahnhy1324/crystalhd/releases/tag/v1.3.0-rc.2)
ZIPs remain frozen with their original package/qualification receipts. RC4's
source/module19 and its separately bound ZIP also retain their original scope;
final 1.3/module20 does not overwrite or relabel those artifacts.

The [stock Kodi21.3 UI receipt](../../tests/fixtures/issue611/kodi-settings-ui-20261031.json)
records actual ZIP installation, directional/Select/Back controls, save/reset,
nonwrites and normal exit. Other Kodi builds, UI languages and spoken
accessibility require their own observations.
