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
import stat
import sys

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
    required = ("load_user_document_for_edit", "save_user_document", "NATIVE_CONFIG_DISABLE_ENVIRONMENT")
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

    def set_literal(self, key, source):
        if key not in self.core.RULES:
            raise self.core.SchemaError("unknown canonical setting")
        if self.draft is None or self.write_blocked:
            raise self.core.SchemaError("reload a valid configuration before editing")
        if source is None:
            self.draft.settings.pop(key, None)
        else:
            self.draft.settings.update(self.core._parse_setting_assignments([key + "=" + source]))

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

    def preview(self, observe_environment=True):
        if self.draft is None:
            raise self.core.SchemaError(self.load_error or "configuration is not loaded")
        candidate = self.core.parse_document(self.core.serialize_document(self.draft))
        return self.core.resolve(self.system, candidate, self.catalog.selectable,
                                 raw_environment=self.environment if observe_environment else {})

    def required_acknowledgements(self):
        selected = self.preview(False).preset
        return self.catalog.by_name[selected].required_acknowledgements

    def report(self):
        return self.core.make_report(self.preview(), True, bug_report=True)

    def native_requires_launch(self):
        managed = self.preview(False)
        values = managed.requested
        risky = (self.catalog.by_name[managed.preset].required_acknowledgements or
                 values["libcrystalhd.rx_pool_count"] == 8 or
                 values["vaapi.h264.mode"] == "live" or
                 any(values[k] for k in ("vaapi.h264.low_latency", "vaapi.h264.ready_remap", "vaapi.i915_wc")))
        return bool(risky)

    def native_warning(self):
        if self.native_requires_launch():
            return _("Saved experiments need an explicit acknowledged launch. Ordinary Kodi may ignore this managed transaction.")
        return _("Updated VA/DIL reads supported defaults on next start. Installed readers and active applications are not inspected.")

    def _commit(self, candidate, acknowledgements=()):
        if self.snapshot is None or self.write_blocked:
            raise self.core.SchemaError("reload before another write")
        serialized = self.core.serialize_document(candidate)
        result = self.core.save_user_document(self.path, candidate, self.snapshot,
                                             system_path=self.system_path,
                                             acknowledgements=acknowledgements)
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
            self.load_error = None
        except self.core.SchemaError:
            self.write_blocked = True
            return _("Saved, but readback changed or failed. Reload before another write.")
        if not result.durable:
            self.write_blocked = True
            return _("Saved, but durability is unconfirmed. Inspect the file and Reload before another write.")
        return _("Defaults saved. Restart applications to read them; active state is unconfirmed.")

    def save(self, acknowledgements=()):
        self.preview(False)
        return self._commit(self.draft, acknowledgements)

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
    def __init__(self, editor, gtk_modules, no_tray=False):
        self.editor = editor
        self.Gtk, self.Gdk, self.Gio, self.GLib = gtk_modules
        Gtk = self.Gtk
        self.updating = False
        self.indicator = None
        self.tray_connected = False
        self.quitting = False
        self.notice = ""
        self.window = Gtk.Window(title=_("CrystalHD Settings"))
        self.window.set_default_size(720, 560)
        self.window.set_border_width(14)
        self.window.connect("delete-event", self.close_window)
        self.window.connect("key-press-event", self.key_press)
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
        title = Gtk.Label(label=_("Saved defaults"), xalign=0)
        title.get_style_context().add_class("title")
        outer.pack_start(title, False, False, 0)
        self.status = Gtk.Label(xalign=0)
        self.status.set_line_wrap(True)
        self.status.set_selectable(True)
        outer.pack_start(self.status, False, False, 0)
        self.notebook = Gtk.Notebook()
        outer.pack_start(self.notebook, True, True, 0)
        grid = Gtk.Grid(column_spacing=16, row_spacing=10, margin=12)
        self.notebook.append_page(grid, Gtk.Label(label=_("Native defaults")))
        self.preset = self.choice(grid, 0, _("_Preset"))
        self.profile = self.choice(grid, 1, _("Default pro_file"))
        self.controls, self.badges = {}, {}
        advanced = Gtk.Grid(column_spacing=16, row_spacing=10, margin=12)
        self.notebook.append_page(advanced, Gtk.Label(label=_("Advanced native")))
        basic_row, advanced_row = 2, 0
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
        self.notebook.append_page(scroll, Gtk.Label(label=_("All settings / origins")))
        location = Gtk.Label(label=_("User file: ") + str(editor.path), xalign=0)
        location.set_ellipsize(2)
        location.set_tooltip_text(str(editor.path))
        outer.pack_start(location, False, False, 0)
        buttons = Gtk.ButtonBox(orientation=Gtk.Orientation.HORIZONTAL)
        buttons.set_layout(Gtk.ButtonBoxStyle.END)
        outer.pack_start(buttons, False, False, 0)
        self.reload_button = self.button(buttons, _("R_eload"), self.reload)
        self.reset_button = self.button(buttons, _("_Reset user defaults"), self.reset)
        self.save_button = self.button(buttons, _("_Save defaults"), self.save)
        self.button(buttons, _("_Close"), self.close_window)
        self.preset.connect("changed", self.selector_changed, "preset")
        self.profile.connect("changed", self.selector_changed, "profile")
        for key, combo in self.controls.items():
            combo.connect("changed", self.setting_changed, key)
        if not no_tray:
            self.setup_tray(icon)
        self.sync_form()

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

    def sync_form(self):
        self.updating = True
        try:
            doc = self.editor.draft
            presets = [("", _("Inherit preset"), True)]
            if self.editor.catalog:
                for entry in self.editor.catalog.entries:
                    native_owned = entry.settings is not None and all(key in self.controls for key in entry.settings)
                    enabled = entry.status == "SELECTABLE" and not entry.required_acknowledgements and native_owned
                    status = _("standard") if enabled else (_("acknowledged CLI launch only") if entry.required_acknowledgements else (_("frontend-only; not applied here") if entry.status == "SELECTABLE" else entry.status.lower()))
                    presets.append((entry.name, entry.record["display_name"] + " — " + status, enabled))
            self.set_choices(self.preset, presets, doc.metadata.get("preset", "") if doc else "")
            names = set(self.editor.system.profiles if self.editor.system else ())
            names.update(doc.profiles if doc else ())
            profiles = [("", _("Inherit profile"), True)] + [(name, name, True) for name in sorted(names)]
            self.set_choices(self.profile, profiles, doc.metadata.get("profile", "") if doc else "")
            labels = {"false": _("Off / not explicitly requested"), "true": _("On (diagnostic)"),
                      '"bounded"': _("Automatic / bounded"), '"strict"': _("Strict finite"), "16": _("16 buffers (standard)")}
            for key, _label, allowed in CONTROLS:
                choices = [("", _("Inherit lower file layers"), True), ("unset", _("Native default (unset)"), True)]
                choices += [(v, labels[v], True) for v in allowed]
                value = doc.settings.get(key) if doc and key in doc.settings else None
                token = "" if value is None else ("unset" if value is self.editor.core.UNSET else self.editor.core._format_value(value))
                self.set_choices(self.controls[key], choices, token)
            editable = doc is not None and not self.editor.write_blocked
            for combo in [self.preset, self.profile] + list(self.controls.values()):
                combo.set_sensitive(editable)
        finally:
            self.updating = False
        self.update_preview()

    def update_preview(self):
        self.values.clear()
        valid = False
        partial_model = native_rejected = False
        try:
            report = self.editor.report()
            partial_model = bool(report["native_only_environment"])
            native_rejected = bool(report["native_rejections"])
            guidance = self.editor.native_warning()
            if report["native_file_reading"] == "bypassed-by-environment":
                guidance += " " + _("Native file reading is bypassed in this settings process's environment.")
            self.guidance.set_text(guidance + " " + _("Explicit prefeed Off still permits eligible automatic prefeed."))
            launch_only = self.editor.native_requires_launch()
            for item in report["settings"]:
                key = item["key"]
                use = (_("Next start in updated VA/DIL; not confirmed active") if key in self.controls else
                       _("Saved only; automatic consumer unavailable"))
                if key in self.controls and launch_only:
                    use = _("Launch-only; native transaction may be ignored")
                if item["origin"].startswith("environment:"):
                    use = _("Environment override in this settings process; application state unconfirmed")
                value = self.editor.core._format_value(item["effective"])
                self.values.append((key, value, item["origin"], use, item["state"]))
                if key in self.badges:
                    origin = item["origin"].split(":", 1)[0]
                    self.badges[key].set_text(_("Model: ") + value + "\n" + _("Origin: ") + origin)
                    self.badges[key].set_tooltip_text(item["origin"] + "\n" + use)
            valid = True
        except self.editor.core.SchemaError as error:
            self.guidance.set_text(str(error))
            for badge in self.badges.values():
                badge.set_text(_("Model unavailable"))
        status = self.notice or (_("Unsaved changes") if self.editor.dirty else _("Defaults loaded; active application state unconfirmed"))
        if self.editor.load_error:
            status = _("Configuration could not be loaded: ") + self.editor.load_error
        if partial_model:
            status += "\n" + _("Model is partial: native-only environment settings are not validated here.")
        if native_rejected:
            status += "\n" + _("Some requests are rejected by the native model; see the model state column.")
        if not self.tray_connected:
            status += "\n" + _("Tray unavailable; closing this window exits.")
        self.status.set_text(status)
        self.save_button.set_sensitive(valid and self.editor.dirty and not self.editor.write_blocked)
        self.reset_button.set_sensitive(self.editor.snapshot is not None and not self.editor.write_blocked)

    def setting_changed(self, combo, key):
        if self.updating:
            return
        try:
            token = self.token(combo)
            # A disabled saved row can remain selected, but never be activated.
            row = combo.get_active_iter()
            if row is not None and not combo.get_model()[row][2]:
                return
            self.editor.set_literal(key, token or None)
            self.notice = ""
        except self.editor.core.SchemaError as error:
            self.notice = str(error)
        self.update_preview()

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
        try:
            required = self.editor.required_acknowledgements()
            if required and not self.confirm(_("Save launch-only experimental defaults?"),
                    _("This does not enable experiments in ordinary Kodi. An acknowledged launch remains required. Risks: ") + ", ".join(required)):
                return
            self.notice = self.editor.save(required)
        except self.editor.core.SchemaError as error:
            self.notice = str(error) + " " + _("Keep your edits; Reload to review changes before retrying.")
        self.sync_form()

    def reload(self, *_args):
        if self.editor.dirty and not self.confirm(_("Discard the editor draft?"), _("Reload reads the current user and system files.")):
            return
        self.editor.reload()
        self.notice = ""
        self.sync_form()

    def reset(self, *_args):
        if not self.confirm(_("Reset all user defaults?"), _("This clears user settings and profile sections. System defaults and process overrides remain.")):
            return
        try:
            self.notice = self.editor.reset()
        except self.editor.core.SchemaError as error:
            self.notice = str(error)
        self.sync_form()

    def close_window(self, *_args):
        if self.tray_connected:
            self.window.hide()
            return True
        return self.quit()

    def quit(self, *_args):
        if self.editor.dirty and not self.confirm(_("Discard the editor draft and exit?"), _("Saved defaults remain after this utility exits.")):
            return True
        self.quitting = True
        if self.indicator:
            self.indicator.set_status(self.indicator_module.IndicatorStatus.PASSIVE)
        self.window.destroy()
        if self.Gtk.main_level():
            self.Gtk.main_quit()
        return True

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
        self.indicator_module = module
        icon_name = "crystalhd-settings" if icon.is_file() else "preferences-system"
        self.indicator = module.Indicator.new("crystalhd-settings-" + str(os.getpid()),
                                            icon_name, module.IndicatorCategory.APPLICATION_STATUS)
        self.indicator.set_icon_full(icon_name, _("CrystalHD Settings"))
        if icon.is_file():
            self.indicator.set_icon_theme_path(str(icon.parent))
        self.indicator.set_title(_("CrystalHD Settings"))
        menu = self.Gtk.Menu()
        for title, callback in ((_("Open settings"), lambda *_: self.window.present()), (_("Exit"), self.quit)):
            item = self.Gtk.MenuItem.new_with_label(title)
            item.connect("activate", callback)
            menu.append(item)
        menu.show_all()
        self.indicator.set_menu(menu)
        self.indicator.connect("connection-changed", self.tray_connection_changed)
        self.indicator.set_status(module.IndicatorStatus.ACTIVE)

    def tray_connection_changed(self, _indicator, connected):
        if self.quitting:
            return
        self.tray_connected = bool(connected) and tray_host_present(self.Gio, self.GLib)
        if not self.tray_connected:
            self.window.present()
        self.update_preview()


def main(argv=None):
    parser = argparse.ArgumentParser(description=_("Edit saved CrystalHD defaults; active applications are not reconfigured."))
    parser.add_argument("--no-tray", action="store_true", help=_("use a window only"))
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
        path = args.user_config or core.default_user_config(os.environ)
        if path is None:
            raise RuntimeError("no absolute user configuration path; use --user-config")
        editor = Editor(core, path, None if args.no_system_config else (args.system_config or core.SYSTEM_CONFIG), os.environ)
        application = SettingsWindow(editor, (Gtk, Gdk, Gio, GLib), args.no_tray)
        application.window.show_all()
        Gtk.main()
    except (ImportError, ValueError, RuntimeError, OSError) as error:
        print("crystalhd-settings: " + str(error), file=sys.stderr)
        return 3
    return 0


if __name__ == "__main__":
    sys.exit(main())
