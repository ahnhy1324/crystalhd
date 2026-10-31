# CrystalHD settings for Kodi

Optional Linux Program add-on for stock Kodi with Python API 3.0.0 (Kodi 19+;
the selected integration target is Kodi 21.3). This is a local development
candidate, not a published release or decoder add-on.

Install matching CrystalHD tools first (`make install-config`). The executable
`crystalhd-config` must be visible in Kodi's PATH and provide `editor-state` v1.
Updated VA/DIL readers are required to consume managed defaults. Missing or older
readers, sandboxed Kodi and environment overrides can prevent application;
Installation reports file declarations only, never live decoder success.

Build with `make kodi-settings-package KODI_PACKAGE=/absolute/new.zip`, then use
Kodi **Add-ons → Install from zip file**, select the ZIP, and open **Program
add-ons → CrystalHD settings**. Use arrows/Select/Back. Choose a preset or existing
profile, edit decoder defaults, then explicitly Save. Back/Cancel never saves.
There is no separate settings.xml store. Re-entering reloads managed files.
Quality is a Player-only preset; launch-only experiments are not offered as
ordinary Kodi choices. Advanced contains diagnostic and experiment-disable
requests. Module parameters and Kodi's own decoder selection are outside this UI.

**Saved means restart Kodi to apply.** A new video/context alone is insufficient
with current per-component snapshots. Live state is unknown; the add-on never
restarts Kodi, opens the decoder, reloads modules, or activates experiments.
Next-context reload and live IPC are separate future work.

Follow defaults removes an override; built-in default saves `unset`. Inheriting
the profile means inheriting the system selection. Restore ALL requires explicit
confirmation and clears every CrystalHD user override/profile, including other
applications' settings. A stale file, invalid/conflicting configuration, missing
CLI or write failure is reported without silently replacing a newer edit. An
uncertain save is never retried automatically: reload and inspect first.

Uninstall the add-on through Kodi. Shared CrystalHD configuration is preserved.
Reset it explicitly using the UI or `crystalhd-config reset` if needed; installing
or removing the add-on itself never writes configuration. Headless CLI and GTK
settings remain available. The ZIP includes LGPL-2.1-or-later license terms.
