#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Optional GTK settings editor. The model remains stdlib-only/Python 3.6."""
import argparse
import gettext
import importlib.machinery
import importlib.util
import json
import os
from pathlib import Path
import queue
import re
import stat
import sys
import threading

DATA_RELATIVE = "@SETTINGS_DATA_RELATIVE@"


def data_directory():
    relative = "../share" if DATA_RELATIVE.startswith("@") else DATA_RELATIVE
    return Path(os.path.abspath(os.path.join(str(Path(__file__).resolve().parent), relative)))


_ = gettext.translation("crystalhd-settings", localedir=str(data_directory() / "locale"), fallback=True).gettext
# Presentation capabilities, not a second schema/validator. Activations which
# require an acknowledged launcher are deliberately not ordinary GUI toggles.
CONTROLS = (
    ("vaapi.h264.mode", _("_H.264 input policy"), ('"bounded"', '"strict"')),
    ("vaapi.h264.low_latency", _("E_xplicit prefeed"), ("false",)),
    ("vaapi.h264.ready_remap", _("READY re_mapping"), ("false",)),
    ("vaapi.i915_wc", _("i915 _write combining"), ("false",)),
    ("libcrystalhd.rx_pool_count", _("Receive _buffers"), ("16",)),
    ("vaapi.debug", _("VA _debug logging"), ("false", "true")),
    ("vaapi.trace", _("VA timing _trace"), ("false", "true")),
)
PLAYER_KEYS = ("player.decoder", "gstreamer.crystalhddec.scale_width", "player.software_output_size")


class _CoreLoader(importlib.machinery.SourceFileLoader):
    # The installed CLI is an adjacent executable, not an importable package.
    # Reading it must not create __pycache__ in a source checkout or bin dir.
    def set_data(self, path, data, **kwargs):
        pass


def load_core():
    script = Path(__file__).resolve()
    installed = script.with_name("crystalhd-config")
    path = installed if installed.exists() else script.with_name("crystalhd-config.py")
    source, own = path.lstat(), script.stat()
    if (not stat.S_ISREG(source.st_mode) or source.st_mode & 0o022
            or source.st_uid != own.st_uid):
        raise RuntimeError("configuration core has unsafe ownership or permissions")
    loader = _CoreLoader("crystalhd_settings_core", str(path))
    spec = importlib.util.spec_from_loader(loader.name, loader)
    core = importlib.util.module_from_spec(spec)
    loader.exec_module(core)
    required = ("load_user_document_for_edit", "save_user_document", "patch_user_settings",
                "delete_user_profile", "NATIVE_CONFIG_DISABLE_ENVIRONMENT",
                "inspect_installation", "installation_text")
    if not all(hasattr(core, name) for name in required):
        raise RuntimeError("a matching crystalhd-config core is required")
    return core




class Editor(object):
    """A draft plus the existing core's optimistic write snapshot; no GUI I/O."""
    def __init__(self, core, user_path, system_path, environment):
        self.core, self.path, self.system_path = core, user_path, system_path
        self.environment = dict(environment)
        self.reload()

    def reload(self):
        self.snapshot = self.draft = self.catalog = self.system = None
        self.load_error = None
        self.write_blocked = False
        self.saved_canonical = None
        self.edited_profiles = set()
        try:
            self.catalog = self.core.load_preset_catalog()
            self.system = self.core.load_document(self.system_path, "system configuration", system=True)
            # Retain the safe identity/raw bytes even if INI parsing fails, so
            # an explicitly confirmed Reset still uses stale-write protection.
            self.snapshot = self.core.load_user_document_for_edit(self.path, reset=True)
            raw = self.snapshot.original_bytes
            self.draft = self.core.empty_document() if raw is None else self.core.parse_document(raw)
            self.saved_canonical = self.core.serialize_document(self.draft)
        except self.core.SchemaError as error:
            self.load_error = str(error)

    @property
    def dirty(self):
        return self.draft is not None and self.core.serialize_document(self.draft) != self.saved_canonical

    def set_literal(self, key, source, profile=None):
        if key not in self.core.RULES:
            raise self.core.SchemaError("unknown canonical setting")
        if self.draft is None or self.write_blocked:
            raise self.core.SchemaError("reload a valid configuration before editing")
        self.draft = self.core.patch_user_settings(
            self.draft, [] if source is None else [key + "=" + source],
            [key] if source is None else [], profile)
        if profile is not None:
            self.edited_profiles.add(profile)

    def create_profile(self, name):
        if self.draft is None or self.write_blocked:
            raise self.core.SchemaError("reload before editing profiles")
        if name in self.draft.profiles or name in self.system.profiles:
            raise self.core.SchemaError("profile already exists; select its editing layer")
        self.draft = self.core.patch_user_settings(self.draft, profile=name)
        self.edited_profiles.add(name)

    def delete_profile(self, name):
        if self.draft is None or self.write_blocked:
            raise self.core.SchemaError("reload before editing profiles")
        self.draft = self.core.delete_user_profile(self.draft, name, self.system)
        if name in self.system.profiles:
            self.edited_profiles.add(name)
        else:
            self.edited_profiles.discard(name)

    def set_selector(self, key, value):
        if key not in ("preset", "profile") or self.draft is None or self.write_blocked:
            raise self.core.SchemaError("invalid selector edit; reload")
        if value is None:
            self.draft.metadata.pop(key, None)
        else:
            # The existing serializer/parser validates selector names.
            candidate = self.core.parse_document(self.core.serialize_document(self.draft))
            candidate.metadata[key] = value
            self.draft = self.core.parse_document(self.core.serialize_document(candidate))

    def preview(self, observe_environment=True, profile=None):
        if self.draft is None:
            raise self.core.SchemaError(self.load_error or "configuration is not loaded")
        candidate = self.core.parse_document(self.core.serialize_document(self.draft))
        return self.core.resolve(self.system, candidate, self.catalog.selectable,
                                 explicit_profile=profile,
                                 raw_environment=self.environment if observe_environment else {})

    def required_acknowledgements(self):
        selected = self.preview(False).preset
        return self.catalog.by_name[selected].required_acknowledgements

    def report(self, profile=None):
        return self.core.make_report(self.preview(profile=profile), True, bug_report=True)

    def native_requires_launch(self, profile=None):
        managed = self.preview(False, profile)
        values = managed.requested
        risky = (self.catalog.by_name[managed.preset].required_acknowledgements or
                 values["libcrystalhd.rx_pool_count"] == 8 or
                 values["vaapi.h264.mode"] == "live" or
                 any(values[k] for k in ("vaapi.h264.low_latency", "vaapi.h264.ready_remap", "vaapi.i915_wc")))
        return bool(risky)

    def native_warning(self, profile=None):
        if self.native_requires_launch(profile):
            return _("Saved experiments need an explicitly acknowledged launch. Ordinary applications may ignore these saved requests.")
        return _("Updated CrystalHD components read supported defaults when an app starts. See Installation for file declarations; running apps are not inspected.")

    def _commit(self, candidate, acknowledgements=(), validate_profiles=()):
        if self.snapshot is None or self.write_blocked:
            raise self.core.SchemaError("reload before another write")
        serialized = self.core.serialize_document(candidate)
        result = self.core.save_user_document(self.path, candidate, self.snapshot,
                                             system_path=self.system_path,
                                             acknowledgements=acknowledgements,
                                             validate_profiles=sorted(validate_profiles))
        if result.committed is not True:
            self.write_blocked = True
            return _("Save outcome is uncertain. Inspect the file and Reload before another write.")
        try:
            fresh = self.core.load_user_document_for_edit(self.path)
            if fresh.original_bytes != serialized:
                raise self.core.SchemaError("configuration changed after save")
            self.system = self.core.load_document(self.system_path, "system configuration", system=True)
            self.snapshot, self.draft = fresh, fresh.document
            self.saved_canonical = serialized
            self.edited_profiles.clear()
            self.load_error = None
        except self.core.SchemaError:
            self.write_blocked = True
            return _("Saved, but readback changed or failed. Reload before another write.")
        if not result.durable:
            self.write_blocked = True
            return _("Saved, but durability is unconfirmed. Inspect the file and Reload before another write.")
        return _("Defaults saved. Restart applications to read them; active state is unconfirmed.")

    def save(self, acknowledgements=(), profile=None):
        self.preview(False)
        if profile is not None:
            self.preview(False, profile)
        return self._commit(self.draft, acknowledgements, self.edited_profiles)

    def reset(self):
        return self._commit(self.core.empty_document())


def load_gtk():
    import gi
    gi.require_version("Gtk", "3.0")
    gi.require_version("Gdk", "3.0")
    from gi.repository import Gdk, Gio, GLib, Gtk
    return Gtk, Gdk, Gio, GLib


def tray_host_present(Gio, GLib):
    try:
        bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
        value = bus.call_sync("org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher",
                              "org.freedesktop.DBus.Properties", "Get",
                              GLib.Variant("(ss)", ("org.kde.StatusNotifierWatcher", "IsStatusNotifierHostRegistered")),
                              GLib.VariantType.new("(v)"), Gio.DBusCallFlags.NO_AUTO_START, 500, None)
        return value.unpack()[0] is True
    except GLib.Error:
        return False


def load_indicator():
    import gi
    try:
        gi.require_version("AyatanaAppIndicator3", "0.1")
        from gi.repository import AyatanaAppIndicator3
        return AyatanaAppIndicator3
    except (ImportError, ValueError):
        return None


class SettingsWindow(object):
    def __init__(self, editor, gtk_modules, no_tray=False, start_in_tray=False, quit_callback=None):
        self.editor = editor
        self.Gtk, self.Gdk, self.Gio, self.GLib = gtk_modules
        Gtk = self.Gtk
        self.updating = False
        self.indicator = None
        self.tray_connected = False
        self.indicator_connected = False
        self.tray_bus = None
        self.tray_subscriptions = []
        self.quitting = False
        self.readers_unknown = False
        self._inspection_generation = 0
        self._inspection_worker = None
        self._inspection_results = queue.Queue(maxsize=1)
        self._inspection_source = 0
        self._inspection_closed = False
        self.quit_callback = quit_callback
        self.started = False
        self.window_only = no_tray
        self.startup_hide_pending = start_in_tray
        self.pending_player = {}
        self.notice = ""
        self.editing_profile = None
        self.window = Gtk.Window(title=_("CrystalHD Settings"))
        self.window.set_default_size(720, 560)
        self.window.set_border_width(14)
        self.window.connect("delete-event", self.close_window)
        self.window.connect("key-press-event", self.key_press)
        self.window.connect("destroy", self.cancel_installation)
        # PNG works even on minimal GTK/Qt hosts without an SVG loader.
        icon = data_directory() / "icons/hicolor/32x32/apps/crystalhd-settings.png"
        window_icon = data_directory() / "icons/hicolor/64x64/apps/crystalhd-settings.png"
        self.window.set_icon_name("preferences-system")
        if window_icon.is_file():
            try:
                self.window.set_icon_from_file(str(window_icon))
            except self.GLib.Error:
                pass
        outer = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=10)
        self.window.add(outer)
        title = Gtk.Label(label=_("Defaults for next playback"), xalign=0)
        title.get_style_context().add_class("title")
        outer.pack_start(title, False, False, 0)
        self.status = Gtk.Label(xalign=0)
        self.status.set_line_wrap(True)
        self.status.set_selectable(True)
        outer.pack_start(self.status, False, False, 0)
        self.profiles_section = Gtk.Expander(label=_("Profiles and presets"))
        scope_grid = Gtk.Grid(column_spacing=16, row_spacing=10, margin=12)
        self.profiles_section.add(scope_grid)
        outer.pack_start(self.profiles_section, False, False, 0)
        self.notebook = Gtk.Notebook()
        outer.pack_start(self.notebook, True, True, 0)
        grid = Gtk.Grid(column_spacing=16, row_spacing=10, margin=12)
        player_grid = Gtk.Grid(column_spacing=16, row_spacing=10, margin=12)
        advanced = Gtk.Grid(column_spacing=16, row_spacing=10, margin=12)
        # Profiles add controls; keep Save/Close reachable on short displays.
        for page, label in ((player_grid, _("Player")), (grid, _("Decoder defaults")), (advanced, _("Advanced"))):
            scroll = Gtk.ScrolledWindow()
            scroll.set_policy(Gtk.PolicyType.NEVER, Gtk.PolicyType.AUTOMATIC)
            scroll.add(page)
            self.notebook.append_page(scroll, Gtk.Label(label=label))
        self.preset = self.choice(scope_grid, 0, _("_Preset"))
        self.preset.set_tooltip_text(_("Presets provide defaults, not overrides. Player-only presets affect crystalhd-play, not Kodi."))
        self.profile = self.choice(scope_grid, 1, _("Profile used by de_fault"))
        self.profile.set_tooltip_text(_("Select the profile for future application starts. Selecting what to edit below does not change this choice."))
        self.layer = self.choice(scope_grid, 2, _("Settings to _edit"))
        self.layer.set_tooltip_text(_("Edit user defaults or a named profile. Changing this view keeps all unsaved edits and does not select the default profile."))
        profile_buttons = Gtk.Box(spacing=8)
        self.new_profile_button = self.button(profile_buttons, _("_New profile…"), self.new_profile)
        self.delete_profile_button = self.button(profile_buttons, _("Remove _user profile…"), self.delete_profile)
        scope_grid.attach(profile_buttons, 1, 3, 2, 1)
        self.controls, self.badges = {}, {}
        basic_row, advanced_row = 0, 0
        for key, label, _values in CONTROLS:
            is_advanced = key in ("vaapi.h264.low_latency", "vaapi.h264.ready_remap", "vaapi.i915_wc")
            target, row = (advanced, advanced_row) if is_advanced else (grid, basic_row)
            self.controls[key] = self.choice(target, row, label)
            if is_advanced: advanced_row += 1
            else: basic_row += 1
            self.controls[key].set_tooltip_text(key)
            badge = Gtk.Label(xalign=0)
            badge.set_max_width_chars(26)
            badge.set_line_wrap(True)
            badge.get_accessible().set_name(_("Model value and origin: ") + key)
            target.attach(badge, 2, row, 1, 1)
            self.badges[key] = badge
        self.guidance = Gtk.Label(xalign=0)
        self.guidance.set_line_wrap(True)
        self.guidance.set_max_width_chars(70)
        grid.attach(self.guidance, 0, basic_row, 3, 1)
        self.player_controls, self.player_entries, self.player_errors = {}, {}, {}
        self.player_controls["player.decoder"] = self.choice(player_grid, 0, _("_Video decoder"))
        for key, row, label, placeholder in (
                ("gstreamer.crystalhddec.scale_width", 1, _("Card output _width"), _("0 or an even width from 128 to 1918")),
                ("player.software_output_size", 4, _("Software _output size"), _("WIDTHxHEIGHT, for example 1920x1080"))):
            combo = self.choice(player_grid, row, label)
            self.player_controls[key] = combo
            entry = Gtk.Entry()
            entry.set_placeholder_text(placeholder)
            entry.get_accessible().set_name(label.replace("_", "") + _(" value"))
            player_grid.attach(entry, 1, row+1, 2, 1)
            self.player_entries[key] = entry
            error = Gtk.Label(xalign=0)
            error.set_line_wrap(True);error.set_max_width_chars(70)
            player_grid.attach(error, 0, row+2, 3, 1)
            self.player_errors[key] = error
            combo.connect("changed", self.player_choice_changed, key)
            entry.connect("changed", self.player_text_changed, key)
        self.player_controls["player.decoder"].connect("changed", self.player_choice_changed, "player.decoder")
        for key, row in (("player.decoder",0),("gstreamer.crystalhddec.scale_width",1),("player.software_output_size",4)):
            badge = Gtk.Label(xalign=0)
            badge.set_line_wrap(True);badge.set_max_width_chars(25)
            badge.get_accessible().set_name(_("Preview and source: ") + key)
            player_grid.attach(badge, 2, row, 1, 1)
            self.badges[key] = badge
        player_note = Gtk.Label(label=_("Only updated crystalhd-play reads these defaults; see Installation for its reader declaration. Other players keep their own controls. Scaling needs hardware decoding; software resizing also needs a nonzero card width. Interlaced H.264 widths must be multiples of 16. Saving takes effect on the next playback."), xalign=0)
        player_note.set_line_wrap(True);player_note.set_max_width_chars(70)
        player_grid.attach(player_note, 0, 7, 3, 1)
        advanced_note = Gtk.Label(label=_("These controls can clear saved experiments. Activation requires an explicit acknowledged launch; it is not enabled by this editor. Off for explicit prefeed still allows eligible automatic prefeed."), xalign=0)
        advanced_note.set_line_wrap(True)
        advanced_note.set_max_width_chars(70)
        advanced.attach(advanced_note, 0, advanced_row, 3, 1)
        self.values = Gtk.ListStore(str, str, str, str, str)
        self.tree = Gtk.TreeView(model=self.values)
        self.tree.get_accessible().set_name(_("Configuration model and origins"))
        for index, label in enumerate((_("Setting"), _("Value (model)"), _("Origin"), _("Use / timing"), _("Native model state"))):
            column = Gtk.TreeViewColumn(label, Gtk.CellRendererText(), text=index)
            column.set_resizable(True)
            self.tree.append_column(column)
        scroll = Gtk.ScrolledWindow()
        scroll.add(self.tree)
        self.notebook.append_page(scroll, Gtk.Label(label=_("Details")))
        installation = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=10, margin=12)
        self.installation_status = Gtk.Label(xalign=0)
        self.installation_status.set_line_wrap(True)
        self.installation_status.set_max_width_chars(70)
        self.installation_status.set_selectable(True)
        installation.pack_start(self.installation_status, False, False, 0)
        self.installation_refresh = self.button(installation, _("Refresh installation status"), self.refresh_installation)
        self.installation_refresh.set_tooltip_text(_("Read component file declarations. Does not save settings, load a decoder or restart applications."))
        scroll = Gtk.ScrolledWindow()
        scroll.set_policy(Gtk.PolicyType.NEVER, Gtk.PolicyType.AUTOMATIC)
        scroll.add(installation)
        self.notebook.append_page(scroll, Gtk.Label(label=_("Installation")))
        location = Gtk.Label(label=_("User file: ") + str(editor.path), xalign=0)
        location.set_ellipsize(2)
        location.set_tooltip_text(str(editor.path))
        outer.pack_start(location, False, False, 0)
        buttons = Gtk.ButtonBox(orientation=Gtk.Orientation.HORIZONTAL)
        buttons.set_layout(Gtk.ButtonBoxStyle.END)
        outer.pack_start(buttons, False, False, 0)
        self.reload_button = self.button(buttons, _("_Reload file"), self.reload)
        self.reload_button.set_tooltip_text(_("Reread saved files. Ask before discarding any unsaved changes."))
        self.reset_button = self.button(buttons, _("_Clear all user settings…"), self.reset)
        self.reset_button.set_tooltip_text(_("Delete all saved user overrides and profiles after confirmation. System settings remain."))
        self.save_button = self.button(buttons, _("_Save changes"), self.save)
        self.save_button.set_tooltip_text(_("Save every edited profile and user default. Running applications are unchanged."))
        self.close_button = self.button(buttons, _("_Close"), self.close_window)
        self.close_button.set_no_show_all(True)
        self.quit_button = self.button(buttons, _("_Quit"), self.quit)
        self.quit_button.set_tooltip_text(_("Exit the settings app and remove its tray icon. Ask before discarding unsaved changes."))
        self.preset.connect("changed", self.selector_changed, "preset")
        self.profile.connect("changed", self.selector_changed, "profile")
        self.layer.connect("changed", self.layer_changed)
        for key, combo in self.controls.items():
            combo.connect("changed", self.setting_changed, key)
        if not no_tray:
            self.setup_tray(icon)
        self.sync_form()
        self.refresh_installation()

    def refresh_installation(self, *_args):
        if self._inspection_closed or self.quitting:
            return
        self._inspection_generation += 1
        self.installation_status.set_text(_("Inspecting installed components…"))
        # One reader at a time. Repeated refreshes invalidate the old result
        # and coalesce into one inspection of the latest on-disk state.
        if self._inspection_worker is None:
            self.start_installation()
        if self._inspection_worker is not None and not self._inspection_source:
            self._inspection_source = self.GLib.timeout_add(50, self.poll_installation)

    def start_installation(self):
        core, results = self.editor.core, self._inspection_results
        generation = self._inspection_generation

        def inspect():
            # No editor/draft/GTK access from this thread. The queue, unlike
            # a UI callback, can safely outlive a destroyed window.
            try:
                report = core.inspect_installation()
                result = (core.installation_text(report),
                          any(item["state"] != "declared-v1" for item in report["readers"]))
            except Exception:
                # Never reflect file contents or exception paths into status.
                result = None
            results.put_nowait((generation, result))

        self._inspection_worker = threading.Thread(target=inspect, name="crystalhd-inspection", daemon=True)
        try:
            self._inspection_worker.start()
        except (OSError, RuntimeError):
            self._inspection_worker = None
            self.installation_status.set_text(_("Installation inspection failed. Refresh to retry."))

    def poll_installation(self):
        if self._inspection_closed or self.quitting:
            self._inspection_source = 0
            return False
        try:
            generation, result = self._inspection_results.get_nowait()
        except queue.Empty:
            return True
        self._inspection_worker = None
        if generation != self._inspection_generation:
            self.start_installation()
            if self._inspection_worker is not None:
                return True
        elif result is None:
            self.installation_status.set_text(_("Installation inspection failed. Refresh to retry."))
        else:
            text, self.readers_unknown = result
            self.installation_status.set_text(text)
            self.update_preview()
        self._inspection_source = 0
        return False

    def cancel_installation(self, *_args):
        self._inspection_closed = True
        self._inspection_generation += 1
        if self._inspection_source:
            self.GLib.source_remove(self._inspection_source)
            self._inspection_source = 0
        # A slow filesystem read cannot be interrupted safely. Do not join it
        # on the UI thread or keep the app alive; late results are discarded.

    def choice(self, grid, row, text):
        label = self.Gtk.Label.new_with_mnemonic(text)
        label.set_xalign(0)
        model = self.Gtk.ListStore(str, str, bool)
        combo = self.Gtk.ComboBox.new_with_model(model)
        cell = self.Gtk.CellRendererText()
        combo.pack_start(cell, True)
        combo.add_attribute(cell, "text", 1)
        combo.add_attribute(cell, "sensitive", 2)
        combo.set_hexpand(True)
        combo.get_accessible().set_name(text.replace("_", ""))
        label.set_mnemonic_widget(combo)
        grid.attach(label, 0, row, 1, 1)
        grid.attach(combo, 1, row, 1, 1)
        return combo

    def button(self, box, label, callback):
        button = self.Gtk.Button.new_with_mnemonic(label)
        button.connect("clicked", callback)
        box.add(button)
        return button

    def set_choices(self, combo, choices, selected):
        model = combo.get_model()
        model.clear()
        active = None
        for token, label, enabled in choices:
            position = model.append((token, label, enabled))
            if token == selected:
                active = position
        if active is None:
            active = model.append((selected, selected + _(" (saved; unavailable here)"), False))
        combo.set_active_iter(active)

    def token(self, combo):
        position = combo.get_active_iter()
        return combo.get_model()[position][0] if position is not None else ""

    def has_unsaved_changes(self):
        return self.editor.dirty or bool(self.pending_player)

    def edited_values(self):
        doc = self.editor.draft
        if doc is None:
            return {}
        return doc.settings if self.editing_profile is None else doc.profiles.get(self.editing_profile, {})

    def player_error_text(self, key):
        if key == "gstreamer.crystalhddec.scale_width":
            return _("Enter 0 for native width, or an even number from 128 to 1918.")
        return _("Enter WIDTHxHEIGHT: positive even dimensions, each at most 4096, total at most 4096×2160.")

    def store_player_text(self, key):
        raw = self.player_entries[key].get_text()
        pending_key = (self.editing_profile, key)
        try:
            value = raw.strip()
            if key == "gstreamer.crystalhddec.scale_width":
                if re.fullmatch(r"[0-9]{1,4}", value) is None:
                    raise self.editor.core.SchemaError("invalid width")
                source = str(int(value, 10))
            else:
                source = json.dumps(value.lower())
            self.editor.set_literal(key, source, self.editing_profile)
            self.pending_player.pop(pending_key, None)
        except self.editor.core.SchemaError:
            self.pending_player[pending_key] = raw

    def player_choice_changed(self, combo, key):
        if self.updating:
            return
        token = self.token(combo)
        try:
            if token == "custom":
                self.player_entries[key].set_sensitive(not self.editor.write_blocked)
                self.store_player_text(key)
                self.player_entries[key].grab_focus()
            else:
                self.pending_player.pop((self.editing_profile, key), None)
                if token or key in self.edited_values():
                    self.editor.set_literal(key, token or None, self.editing_profile)
            self.notice = ""
        except self.editor.core.SchemaError as error:
            self.notice = str(error)
        self.sync_form()

    def player_text_changed(self, entry, key):
        if self.updating or self.token(self.player_controls[key]) != "custom":
            return
        self.store_player_text(key)
        self.notice = ""
        self.update_preview()

    def sync_form(self):
        self.updating = True
        try:
            doc = self.editor.draft
            presets = [("", _("Inherit preset"), True)]
            if self.editor.catalog:
                for entry in self.editor.catalog.entries:
                    native_owned = entry.settings is not None and all(key in self.controls for key in entry.settings)
                    editable_owned = entry.settings is not None and all(key in self.controls or key in PLAYER_KEYS for key in entry.settings)
                    enabled = entry.status == "SELECTABLE" and not entry.required_acknowledgements and editable_owned
                    status = (_("standard") if native_owned else _("Player only")) if enabled else (_("acknowledged CLI launch only") if entry.required_acknowledgements else (_("unavailable here") if entry.status == "SELECTABLE" else entry.status.lower()))
                    presets.append((entry.name, entry.record["display_name"] + " — " + status, enabled))
            self.set_choices(self.preset, presets, doc.metadata.get("preset", "") if doc else "")
            names = set(self.editor.system.profiles if self.editor.system else ())
            names.update(doc.profiles if doc else ())
            profiles = [("", _("Use system profile selection"), True)] + [(name, name, True) for name in sorted(names)]
            self.set_choices(self.profile, profiles, doc.metadata.get("profile", "") if doc else "")
            if self.editing_profile not in names:
                self.editing_profile = None
            layers = [("", _("User defaults (before profile overrides)"), True)]
            layers += [(name, name + (_(" (user overrides)") if doc and name in doc.profiles else _(" (system; edit adds user overrides)")), True)
                       for name in sorted(names)]
            self.set_choices(self.layer, layers, self.editing_profile or "")
            labels = {"false": _("Off"), "true": _("On"),
                      '"bounded"': _("Automatic / bounded"), '"strict"': _("Strict finite"), "16": _("16 buffers (standard)")}
            inherited_label = _("Follow preset/system defaults") if self.editing_profile is None else _("Follow defaults/system profile")
            for key, _label, allowed in CONTROLS:
                choices = [("", inherited_label, True), ("unset", _("Use built-in default"), True)]
                choices += [(v, labels[v], True) for v in allowed]
                target = (doc.settings if self.editing_profile is None else doc.profiles.get(self.editing_profile, {})) if doc else {}
                value = target.get(key)
                token = "" if value is None else ("unset" if value is self.editor.core.UNSET else self.editor.core._format_value(value))
                self.set_choices(self.controls[key], choices, token)
            editable = doc is not None and not self.editor.write_blocked
            target = self.edited_values()
            for key in PLAYER_KEYS:
                value = target.get(key)
                choices = [("", inherited_label, True), ("unset", _("Use built-in default"), True)]
                if key == "player.decoder":
                    choices += [('"hardware"', _("CrystalHD hardware (required)"), True),
                                ('"software"', _("Software decoding (CPU)"), True)]
                    token = "" if value is None else ("unset" if value is self.editor.core.UNSET else self.editor.core._format_value(value))
                else:
                    choices += [("custom", _("Set a value"), True)]
                    pending = (self.editing_profile, key)
                    token = ("custom" if pending in self.pending_player else
                             ("" if value is None else ("unset" if value is self.editor.core.UNSET else "custom")))
                    raw = self.pending_player[pending] if pending in self.pending_player else (str(value) if token == "custom" else "")
                    self.player_entries[key].set_text(raw)
                    self.player_entries[key].set_sensitive(editable and token == "custom")
                    self.player_errors[key].set_text(self.player_error_text(key) if pending in self.pending_player else "")
                self.set_choices(self.player_controls[key], choices, token)
                self.player_controls[key].set_sensitive(editable)
                help_text = {
                    "player.decoder": _("Hardware requires actual CrystalHD output and never silently falls back. Software uses the CPU. Built-in default: hardware."),
                    "gstreamer.crystalhddec.scale_width": _("Card width controls pixels transferred over PCIe, not display size. Smaller output loses detail. Built-in default: 0, native width."),
                    "player.software_output_size": _("Optional CPU resize after card scaling; it cannot recover lost detail. Built-in default: no software resize."),
                }
                self.player_controls[key].set_tooltip_text(help_text[key] + " " + _("Follow defaults removes this override and uses other saved settings. Built-in default ignores those settings. Explicit CLI overrides still win."))
            for combo in [self.preset, self.profile, self.layer] + list(self.controls.values()):
                combo.set_sensitive(editable)
            self.new_profile_button.set_sensitive(editable)
            self.delete_profile_button.set_sensitive(bool(editable and self.editing_profile in doc.profiles))
        finally:
            self.updating = False
        self.update_preview()

    def update_preview(self):
        self.values.clear()
        valid = False
        partial_model = native_rejected = False
        preview_error = None
        try:
            report = self.editor.report(self.editing_profile)
            partial_model = bool(report["native_only_environment"])
            native_rejected = bool(report["native_rejections"])
            guidance = self.editor.native_warning(self.editing_profile)
            if self.editing_profile:
                guidance = _("Previewing editing profile, not the application's default selection. ") + guidance
            if report["native_file_reading"] == "bypassed-by-environment":
                guidance += " " + _("Native file reading is bypassed in this settings process's environment.")
            self.guidance.set_text(guidance + " " + _("Explicit prefeed Off still permits eligible automatic prefeed."))
            launch_only = self.editor.native_requires_launch(self.editing_profile)
            for item in report["settings"]:
                key = item["key"]
                use = (_("Next start in updated VA/DIL; not confirmed active") if key in self.controls else
                       (_("Next start in updated crystalhd-play; not confirmed active") if key in
                        ("player.decoder", "gstreamer.crystalhddec.scale_width", "player.software_output_size") else
                        _("Saved only; automatic consumer unavailable")))
                if key in self.controls and launch_only:
                    use = _("Launch-only; native transaction may be ignored")
                if key in ("player.decoder", "gstreamer.crystalhddec.scale_width", "player.software_output_size") and self.editor.required_acknowledgements():
                    use = _("Experimental preset; ordinary crystalhd-play ignores this managed overlay")
                if item["origin"].startswith("environment:"):
                    use = _("Environment override in this settings process; application state unconfirmed")
                value = self.editor.core._format_value(item["effective"])
                self.values.append((key, value, item["origin"], use, item["state"]))
                if key in self.badges:
                    origin = item["origin"].split(":", 1)[0]
                    origin_label = {"compiled":_("Built-in default"),"user":_("User defaults"),
                                    "system":_("System settings"),"user-profile":_("User profile"),
                                    "system-profile":_("System profile"),"preset":_("Preset"),
                                    "environment":_("Environment override")}.get(origin,origin)
                    self.badges[key].set_text(_("Preview: ") + value + "\n" + _("From: ") + origin_label)
                    self.badges[key].set_tooltip_text(item["origin"] + "\n" + use)
            valid = True
        except self.editor.core.SchemaError as error:
            preview_error = str(error)
            self.guidance.set_text(str(error))
            for badge in self.badges.values():
                badge.set_text(_("Model unavailable"))
        for key, label in self.player_errors.items():
            label.set_text(self.player_error_text(key) if (self.editing_profile, key) in self.pending_player else "")
        status = self.notice or (_("Unsaved changes") if self.has_unsaved_changes() else _("Settings loaded; running applications have not been inspected"))
        if self.readers_unknown:
            status += "\n" + _("Some inspected components do not advertise settings support; see Installation.")
        if self.editing_profile:
            status += "\n" + _("Editing profile: ") + self.editing_profile
        else:
            status += "\n" + _("Editing user defaults")
        if self.editor.draft is not None:
            default_profile = self.editor.draft.metadata.get("profile", self.editor.system.metadata.get("profile"))
            status += " — " + _("Default profile: ") + (default_profile or _("None"))
        if self.pending_player:
            status += "\n" + _("Finish invalid Player values before saving, including values in other profiles.")
        if preview_error:
            status += "\n" + _("Settings conflict: ") + preview_error
        if self.editor.load_error:
            status = _("Configuration could not be loaded: ") + self.editor.load_error
        if partial_model:
            status += "\n" + _("Model is partial: native-only environment settings are not validated here.")
        if native_rejected:
            status += "\n" + _("Some requests are rejected by the native model; see the model state column.")
        if not self.tray_connected:
            status += "\n" + (_("Window-only mode; closing exits.") if self.window_only else _("Tray unavailable; closing this window exits."))
        self.status.set_text(status)
        self.close_button.set_label(_("Hide to tray") if self.tray_connected else _("_Close"))
        self.close_button.set_visible(self.tray_connected)
        self.close_button.set_tooltip_text(_("Keep settings running; reopen from the tray icon.") if self.tray_connected else _("No tray is available; closing exits the settings app."))
        self.save_button.set_sensitive(valid and self.editor.dirty and not self.editor.write_blocked and not self.pending_player)
        self.reset_button.set_sensitive(self.editor.snapshot is not None and not self.editor.write_blocked)
        self.delete_profile_button.set_sensitive(bool(self.editor.draft is not None and not self.editor.write_blocked and self.editing_profile in self.editor.draft.profiles))

    def setting_changed(self, combo, key):
        if self.updating:
            return
        try:
            token = self.token(combo)
            # A disabled saved row can remain selected, but never be activated.
            row = combo.get_active_iter()
            if row is not None and not combo.get_model()[row][2]:
                return
            self.editor.set_literal(key, token or None, self.editing_profile)
            self.notice = ""
        except self.editor.core.SchemaError as error:
            self.notice = str(error)
        self.sync_form()

    def layer_changed(self, combo):
        if self.updating:
            return
        self.editing_profile = self.token(combo) or None
        self.notice = ""
        self.sync_form()

    def prompt_profile_name(self):
        dialog = self.Gtk.Dialog(title=_("New profile"), transient_for=self.window, modal=True)
        dialog.add_buttons(_("Cancel"), self.Gtk.ResponseType.CANCEL, _("Create"), self.Gtk.ResponseType.OK)
        entry = self.Gtk.Entry()
        entry.set_placeholder_text(_("1–64 ASCII letters/digits/._-; start with letter/digit"))
        entry.get_accessible().set_name(_("Profile name"))
        entry.set_activates_default(True)
        dialog.set_default_response(self.Gtk.ResponseType.OK)
        dialog.get_content_area().add(entry)
        dialog.show_all()
        entry.grab_focus()
        response = dialog.run()
        name = entry.get_text()
        dialog.destroy()
        return name if response == self.Gtk.ResponseType.OK else None

    def new_profile(self, *_args):
        name = self.prompt_profile_name()
        if name is None:
            return
        try:
            self.editor.create_profile(name)
            self.editing_profile = name
            self.notice = _("Profile added to draft. Select it as Default profile if desired, then Save.")
        except self.editor.core.SchemaError as error:
            self.notice = str(error)
        self.sync_form()

    def delete_profile(self, *_args):
        if not self.confirm(_("Remove this user profile from the draft?"),
                _("System definitions remain. A user default selecting a removed user-only profile becomes inherited. Save is required.")):
            return
        try:
            self.editor.delete_profile(self.editing_profile)
            self.pending_player = {key:value for key,value in self.pending_player.items()
                                   if key[0] != self.editing_profile}
            self.notice = _("User profile removed from draft; Save to persist.")
        except self.editor.core.SchemaError as error:
            self.notice = str(error)
        self.sync_form()

    def selector_changed(self, combo, key):
        if self.updating:
            return
        row = combo.get_active_iter()
        if row is not None and not combo.get_model()[row][2]:
            return
        try:
            self.editor.set_selector(key, self.token(combo) or None)
            self.notice = ""
        except self.editor.core.SchemaError as error:
            self.notice = str(error)
        self.update_preview()

    def confirm(self, title, text):
        dialog = self.Gtk.MessageDialog(transient_for=self.window, modal=True,
                                       message_type=self.Gtk.MessageType.WARNING,
                                       buttons=self.Gtk.ButtonsType.OK_CANCEL, text=title)
        dialog.format_secondary_text(text)
        response = dialog.run()
        dialog.destroy()
        return response == self.Gtk.ResponseType.OK

    def save(self, *_args):
        if self.pending_player:
            self.notice = _("Correct the highlighted Player values before saving.")
            self.update_preview()
            return
        try:
            required = self.editor.required_acknowledgements()
            if required and not self.confirm(_("Save launch-only experimental defaults?"),
                    _("This does not enable experiments in ordinary Kodi. An acknowledged launch remains required. Risks: ") + ", ".join(required)):
                return
            self.notice = self.editor.save(required, profile=self.editing_profile)
        except self.editor.core.SchemaError as error:
            self.notice = str(error) + " " + _("Keep your edits; Reload to review changes before retrying.")
        self.sync_form()

    def reload(self, *_args):
        if self.has_unsaved_changes() and not self.confirm(_("Discard unsaved changes and reload?"), _("Reload reads the current user and system files.")):
            return
        self.pending_player.clear()
        self.editor.reload()
        self.notice = ""
        self.sync_form()

    def reset(self, *_args):
        if not self.confirm(_("Clear all saved user settings?"), _("This deletes every user override and profile, including unfinished edits. System settings and process overrides remain.")):
            return
        try:
            self.notice = self.editor.reset()
            if not self.editor.write_blocked:
                self.pending_player.clear()
        except self.editor.core.SchemaError as error:
            self.notice = str(error)
        self.sync_form()

    def close_window(self, *_args):
        if self.tray_connected:
            self.window.hide()
            return True
        return self.quit()

    def quit(self, *_args):
        if self.has_unsaved_changes() and not self.confirm(_("Discard unsaved changes and quit?"), _("Saved defaults remain after this utility exits.")):
            return True
        self.quitting = True
        if self.tray_bus is not None:
            for subscription in self.tray_subscriptions:
                self.tray_bus.signal_unsubscribe(subscription)
            self.tray_subscriptions = []
        if self.indicator:
            self.indicator.set_status(self.indicator_module.IndicatorStatus.PASSIVE)
        self.window.destroy()
        if self.quit_callback is not None:
            self.quit_callback()
        elif self.Gtk.main_level():
            self.Gtk.main_quit()
        return True

    def start(self):
        self.started = True
        self.window.show_all()
        self.hide_on_startup_if_connected()

    def hide_on_startup_if_connected(self):
        if self.started and self.startup_hide_pending and self.tray_connected:
            self.startup_hide_pending = False
            self.window.hide()

    def show_settings(self, *_args):
        self.startup_hide_pending = False
        self.window.present()

    def key_press(self, _widget, event):
        control = event.state & self.Gdk.ModifierType.CONTROL_MASK
        if control and event.keyval in (self.Gdk.KEY_s, self.Gdk.KEY_S):
            if self.save_button.get_sensitive(): self.save()
            return True
        if control and event.keyval in (self.Gdk.KEY_r, self.Gdk.KEY_R):
            self.reload(); return True
        if control and event.keyval in (self.Gdk.KEY_q, self.Gdk.KEY_Q):
            return self.quit()
        if event.keyval == self.Gdk.KEY_Escape:
            return self.close_window()
        return False

    def setup_tray(self, icon):
        if not tray_host_present(self.Gio, self.GLib):
            return
        module = load_indicator()
        if module is None:
            return
        try:
            self.tray_bus = self.Gio.bus_get_sync(self.Gio.BusType.SESSION, None)
        except self.GLib.Error:
            return
        self.indicator_module = module
        icon_name = "crystalhd-settings" if icon.is_file() else "preferences-system"
        self.indicator = module.Indicator.new("crystalhd-settings-" + str(os.getpid()),
                                            icon_name, module.IndicatorCategory.APPLICATION_STATUS)
        self.indicator.set_icon_full(icon_name, _("CrystalHD Settings"))
        if icon.is_file():
            self.indicator.set_icon_theme_path(str(icon.parent))
        self.indicator.set_title(_("CrystalHD Settings"))
        menu = self.Gtk.Menu()
        for title, callback in ((_("Open settings…"), self.show_settings), (_("Quit"), self.quit)):
            item = self.Gtk.MenuItem.new_with_label(title)
            item.connect("activate", callback)
            menu.append(item)
        menu.show_all()
        self.indicator.set_menu(menu)
        self.indicator.connect("connection-changed", self.tray_connection_changed)
        # A watcher may outlive its panel. Watch host presence as well as the
        # indicator connection so a hidden editor cannot become stranded.
        for member in ("StatusNotifierHostRegistered", "StatusNotifierHostUnregistered"):
            subscription = self.tray_bus.signal_subscribe(
                "org.kde.StatusNotifierWatcher", "org.kde.StatusNotifierWatcher",
                member, "/StatusNotifierWatcher", None, self.Gio.DBusSignalFlags.NONE,
                self.tray_host_changed)
            self.tray_subscriptions.append(subscription)
        self.indicator.set_status(module.IndicatorStatus.ACTIVE)

    def tray_host_changed(self, *_args):
        self.tray_connection_changed(self.indicator, self.indicator_connected)

    def tray_connection_changed(self, _indicator, connected):
        if self.quitting:
            return
        self.indicator_connected = bool(connected)
        self.tray_connected = self.indicator_connected and tray_host_present(self.Gio, self.GLib)
        if not self.tray_connected:
            self.window.present()
        else:
            self.hide_on_startup_if_connected()
        self.update_preview()


def main(argv=None):
    parser = argparse.ArgumentParser(description=_("Edit saved CrystalHD defaults; active applications are not reconfigured."))
    tray = parser.add_mutually_exclusive_group()
    tray.add_argument("--no-tray", action="store_true", help=_("use a separate window only"))
    tray.add_argument("--tray", action="store_true", help=_("start in the tray when available; otherwise show the window"))
    system = parser.add_mutually_exclusive_group()
    system.add_argument("--system-config", metavar="FILE")
    system.add_argument("--no-system-config", action="store_true")
    parser.add_argument("--user-config", metavar="FILE")
    args = parser.parse_args(argv)
    try:
        core = load_core()
        Gtk, Gdk, Gio, GLib = load_gtk()
        if not Gtk.init_check([])[0]:
            raise RuntimeError("no graphical display; use crystalhd-config")
        isolated = args.no_tray or args.user_config is not None or args.system_config is not None or args.no_system_config
        def make_application(non_unique):
            flags = Gio.ApplicationFlags.NON_UNIQUE if non_unique else Gio.ApplicationFlags.FLAGS_NONE
            return Gtk.Application(application_id="io.github.ahnhy1324.CrystalHD.Settings", flags=flags)
        application = make_application(isolated)
        try:
            application.register(None)
        except GLib.Error:
            application = make_application(True)
            application.register(None)
        windows, outcome = [], [0]
        def activate(app):
            if windows:
                windows[0].show_settings()
                return
            try:
                path = args.user_config or core.default_user_config(os.environ)
                if path is None:
                    raise RuntimeError("no absolute user configuration path; use --user-config")
                editor = Editor(core, path, None if args.no_system_config else (args.system_config or core.SYSTEM_CONFIG), os.environ)
                window = SettingsWindow(editor, (Gtk,Gdk,Gio,GLib), args.no_tray, args.tray, app.quit)
                window.window.set_application(app)
                windows.append(window)
                window.start()
            except (ImportError, ValueError, RuntimeError, OSError) as error:
                print("crystalhd-settings: " + str(error), file=sys.stderr)
                outcome[0] = 3
                app.quit()
        application.connect("activate", activate)
        return max(application.run(["crystalhd-settings"]), outcome[0])
    except (ImportError, ValueError, RuntimeError, OSError) as error:
        print("crystalhd-settings: " + str(error), file=sys.stderr)
        return 3
    return 0


if __name__ == "__main__":
    sys.exit(main())
