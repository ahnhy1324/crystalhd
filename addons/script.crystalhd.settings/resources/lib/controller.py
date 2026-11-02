# SPDX-License-Identifier: LGPL-2.1-or-later
"""Remote UI adapter. Configuration parsing, validation and writes use CLI."""
import json
import os
import re
import shutil
import stat
import subprocess


KEYS = frozenset(("libcrystalhd.rx_pool_count", "vaapi.h264.mode",
                  "vaapi.h264.low_latency", "vaapi.h264.ready_remap",
                  "vaapi.i915_wc", "vaapi.debug", "vaapi.trace"))
LABELS = {
    "libcrystalhd.rx_pool_count": (30020, "Receive buffers"),
    "vaapi.h264.mode": (30021, "H.264 input policy"),
    "vaapi.h264.low_latency": (30022, "Explicit prefeed request"),
    "vaapi.h264.ready_remap": (30023, "READY remapping request"),
    "vaapi.i915_wc": (30024, "i915 write combining request"),
    "vaapi.debug": (30025, "VA debug logging"),
    "vaapi.trace": (30026, "VA timing trace"),
}


class ConfigError(RuntimeError):
    pass


class SaveUncertain(ConfigError):
    pass


class Backend:
    def __init__(self, executable=None, environment=None, layer_arguments=()):
        self.environment = dict(os.environ if environment is None else environment)
        self.executable = executable or shutil.which("crystalhd-config", path=self.environment.get("PATH"))
        if not self.executable:
            raise ConfigError("crystalhd-config is unavailable. Install matching CrystalHD tools; sandboxed Kodi may not see host tools.")
        self.layers = tuple(layer_arguments)  # Private-prefix tests only; no Kodi preference.
        self._unretired_helpers = []

    def call(self, arguments, json_output=False):
        active = []
        for helper in self._unretired_helpers:
            if helper.poll() is None:
                active.append(helper)
            else:
                for stream in (helper.stdout, helper.stderr):
                    if stream is not None:
                        stream.close()
        self._unretired_helpers = active
        if active:
            raise SaveUncertain("Previous configuration helper is still running; writes remain blocked until it retires.")
        try:
            process = subprocess.Popen([self.executable] + list(arguments), env=self.environment,
                                       stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                       stderr=subprocess.PIPE, universal_newlines=True)
        except OSError as error:
            raise ConfigError("Could not start crystalhd-config: " + str(error))
        try:
            output, errors = process.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            process.terminate()
            try:
                process.communicate(timeout=3)
            except subprocess.TimeoutExpired:
                self._unretired_helpers.append(process)
                raise SaveUncertain("Configuration helper did not retire; inspect it before another write.")
            raise SaveUncertain("Configuration helper timed out. A write may have completed; reload and inspect before editing again.")
        if process.returncode == 4:
            raise SaveUncertain("Save outcome or durability is uncertain. Reload and inspect before another write; no automatic retry was made.")
        if process.returncode:
            raise ConfigError((errors.strip() or "crystalhd-config failed")[:2048])
        if len(output) > 1024 * 1024:
            raise ConfigError("Configuration response exceeds the supported size.")
        if json_output:
            try:
                return json.loads(output)
            except (ValueError, TypeError):
                raise ConfigError("Unsupported configuration response; install matching tools.")
        return output

    def state(self):
        state = self.call(["editor-state"] + list(self.layers), json_output=True)
        try:
            valid = (state["format"] == "crystalhd-editor-state" and type(state["format_version"]) is int
                     and state["format_version"] == 1 and state["configuration_schema"] == 1
                     and state["apply"] == "restart-required" and state["application_state"] == "unknown"
                     and re.fullmatch(r"[0-9a-f]{64}", state["revision"]) is not None
                     and set(state["saved_selectors"]) == {"preset", "profile"}
                     and all(value is None or isinstance(value, str) for value in state["saved_selectors"].values())
                     and type(state["user_overlay_empty"]) is bool
                     and {item["key"] for item in state["settings"]} == KEYS
                     and len(state["settings"]) == len(KEYS)
                     and isinstance(state["profiles"], list) and isinstance(state["presets"], list))
            if not valid:
                raise ValueError()
            for field in ("profiles", "presets"):
                for item in state[field]:
                    if (not isinstance(item["name"], str) or type(item["selectable"]) is not bool or
                            re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]{0,63}', item["name"]) is None):
                        raise ValueError()
                    if field == "presets" and not isinstance(item["display_name"], str):
                        raise ValueError()
            for item in state["settings"]:
                if (not isinstance(item["choices"], list) or not item["choices"] or
                        type(item["advanced"]) is not bool or
                        not (item["saved_literal"] is None or isinstance(item["saved_literal"], str))):
                    raise ValueError()
                for choice in item["choices"]:
                    if not isinstance(choice["literal"], str) or type(choice["value"]) not in (bool, int, str):
                        raise ValueError()
            configuration = state["configuration"]
            if (not isinstance(configuration["preset"], str) or
                    not (configuration["profile"] is None or isinstance(configuration["profile"], str)) or
                    not isinstance(configuration["settings"], list)):
                raise ValueError()
            for item in configuration["settings"]:
                if not isinstance(item["key"], str) or not isinstance(item["origin"], str) or "requested" not in item:
                    raise ValueError()
        except (KeyError, TypeError, ValueError):
            raise ConfigError("Unsupported editor-state version or metadata; install matching CrystalHD tools.")
        return state

    def save(self, state, pending):
        arguments = ["save"] + list(self.layers) + ["--editor-safe", "--expect-revision", state["revision"]]
        assignments = []
        settings = {item["key"]: item for item in state["settings"]}
        for key, value in sorted(pending.items()):
            if key in ("preset", "profile"):
                choices = state["presets" if key == "preset" else "profiles"]
                if value is not None and value not in {item["name"] for item in choices if item["selectable"]}:
                    raise ConfigError("Unsupported selector; reload saved settings.")
                if value is None:
                    arguments += ["--remove", key]
                else:
                    arguments += ["--" + key, value]
            elif key in KEYS:
                allowed = {choice["literal"] for choice in settings[key]["choices"]} | {"unset"}
                if value is None:
                    arguments += ["--remove", key]
                elif value in allowed:
                    assignments.append(key + "=" + value)
                else:
                    raise ConfigError("Unsupported setting choice.")
            else:
                raise ConfigError("Unsupported setting key.")
        self.call(arguments + assignments)
        try:
            fresh = self.state()
            saved = {item["key"]: item["saved_literal"] for item in fresh["settings"]}
            saved.update(fresh["saved_selectors"])
            if any(saved[key] != value for key, value in pending.items()):
                raise ConfigError("Saved readback changed.")
        except ConfigError:
            raise SaveUncertain("Save completed, but readback changed or failed. Reload and inspect before another write.")
        return fresh

    def reset(self, state):
        self.call(["reset"] + list(self.layers) + ["--editor-safe", "--expect-revision", state["revision"]])
        try:
            fresh = self.state()
            if not fresh["user_overlay_empty"]:
                raise ConfigError("Reset readback changed.")
            return fresh
        except ConfigError:
            raise SaveUncertain("Reset completed, but readback failed. Reload and inspect before another write.")

    def installation(self):
        report = self.call(["installation", "--format", "json"], json_output=True)
        try:
            if not isinstance(report["readers"], list) or not report["readers"]:
                raise ValueError()
            for item in report["readers"]:
                if not isinstance(item["component"], str) or not isinstance(item["state"], str):
                    raise ValueError()
        except (KeyError, TypeError, ValueError):
            raise ConfigError("Unsupported installation report; install matching CrystalHD tools.")
        # Presence/permissions only: never open the decoder for settings UI.
        try:
            node = os.stat('/dev/crystalhd')
            visible = stat.S_ISCHR(node.st_mode)
        except OSError:
            visible = False
        report['device_node_accessible'] = visible and os.access('/dev/crystalhd', os.R_OK | os.W_OK)
        return report


class SettingsController:
    def __init__(self, backend, dialog, translate=lambda _identifier, fallback: fallback):
        self.backend, self.dialog, self.tr = backend, dialog, translate
        self.state = None
        self.pending = {}
        self.write_blocked = False

    @property
    def title(self):
        return self.tr(30000, "CrystalHD settings")

    def change(self, key, value):
        current = (self.state["saved_selectors"][key] if key in ("preset", "profile") else
                   next(item["saved_literal"] for item in self.state["settings"] if item["key"] == key))
        if value == current:
            self.pending.pop(key, None)
        else:
            self.pending[key] = value

    def choose_selector(self, key):
        records = self.state["presets" if key == "preset" else "profiles"]
        choices = [(None, self.tr(30010, "Follow system selection"))]
        choices += [(item["name"], item.get("display_name", item["name"])) for item in records if item["selectable"]]
        selected = self.dialog.select(self.tr(30001 if key == "preset" else 30002,
                                              "Preset" if key == "preset" else "Profile"), [label for _value, label in choices])
        if selected >= 0:
            self.change(key, choices[selected][0])

    def choose_settings(self, advanced):
        records = [item for item in self.state["settings"] if bool(item["advanced"]) == advanced]
        labels = [self.tr(*LABELS[item["key"]]) for item in records]
        selected = self.dialog.select(self.tr(30004 if advanced else 30003,
                                              "Advanced / diagnostics" if advanced else "Decoder defaults"), labels)
        if selected < 0:
            return
        item = records[selected]
        choices = [(None, self.tr(30011, "Follow preset/system defaults")),
                   ("unset", self.tr(30012, "Use built-in default"))]
        for choice in item["choices"]:
            value = choice["value"]
            label = ({"bounded": self.tr(30027, "Automatic / bounded"),
                      "strict": self.tr(30028, "Strict finite")}.get(value, str(value)) if type(value) is str else
                     self.tr(30029, "On") if value is True else self.tr(30030, "Off") if value is False else str(value))
            choices.append((choice["literal"], label))
        value_index = self.dialog.select(labels[selected], [label for _value, label in choices])
        if value_index >= 0:
            self.change(item["key"], choices[value_index][0])

    def confirm_discard(self):
        return not self.pending or self.dialog.yesno(self.title, self.tr(30013, "Discard unsaved changes?"))

    def values_text(self):
        lines = [self.tr(30014, "Restart required; live state unknown")]
        configuration = self.state["configuration"]
        lines += [self.tr(30001, "Preset") + ": " + str(configuration["preset"]),
                  self.tr(30002, "Profile") + ": " + str(configuration["profile"] or "—")]
        for item in configuration["settings"]:
            if item["key"] in LABELS:
                lines.append(self.tr(*LABELS[item["key"]]) + ": " + str(item["requested"]) +
                             "  (" + item["origin"] + ")")
        if self.pending:
            lines.append("\n" + self.tr(30031, "Unsaved changes"))
            for key, value in sorted(self.pending.items()):
                label = self.tr(*LABELS[key]) if key in LABELS else self.tr(
                    30001 if key == "preset" else 30002, "Preset" if key == "preset" else "Profile")
                lines.append(label + ": " + (str(value) if value is not None else self.tr(30011, "Follow preset/system defaults")))
        return "\n".join(lines)

    def run(self):
        self.state = self.backend.state()
        while True:
            actions = [("preset", self.tr(30001, "Preset")), ("profile", self.tr(30002, "Profile")),
                       ("defaults", self.tr(30003, "Decoder defaults")),
                       ("advanced", self.tr(30004, "Advanced / diagnostics")),
                       ("values", self.tr(30005, "Saved values and origins")),
                       ("installation", self.tr(30006, "Installation / compatibility"))]
            if self.pending and not self.write_blocked:
                actions.append(("save", self.tr(30007, "Save changes — restart Kodi to apply")))
            actions += [("reset", self.tr(30008, "Restore ALL CrystalHD user defaults…")),
                        ("reload", self.tr(30009, "Reload saved values"))]
            index = self.dialog.select(self.title + " — " + self.tr(30014, "Restart required; live state unknown"),
                                       [label for _action, label in actions])
            if index < 0:
                if self.confirm_discard():
                    return
                continue
            action = actions[index][0]
            try:
                if action in ("preset", "profile"):
                    self.choose_selector(action)
                elif action in ("defaults", "advanced"):
                    self.choose_settings(action == "advanced")
                elif action == "values":
                    self.dialog.textviewer(self.tr(30005, "Saved values and origins"), self.values_text())
                elif action == "installation":
                    report = self.backend.installation()
                    details = [self.tr(30015, "File declarations only. Missing/older readers may ignore saved defaults. This does not inspect the running decoder.")]
                    details.append(self.tr(30032, "Device node visible and accessible; decoding has not been tested.")
                                   if report['device_node_accessible'] else
                                   self.tr(30033, "Device node missing or inaccessible. Defaults can still be saved; usable hardware is not confirmed."))
                    details += [item["component"] + ": " + item["state"] for item in report["readers"]]
                    self.dialog.textviewer(self.tr(30006, "Installation / compatibility"), "\n".join(details))
                elif action == "reload":
                    if self.confirm_discard():
                        self.state = self.backend.state()
                        self.pending.clear()
                        self.write_blocked = False
                elif action in ("save", "reset"):
                    if self.write_blocked:
                        raise ConfigError(self.tr(30016, "Reload and inspect saved values before another write."))
                    message = (self.tr(30017, "Remove ALL CrystalHD user overrides and profiles, including settings for other applications? System defaults remain. Restart Kodi to apply.")
                               if action == "reset" else self.tr(30018, "Save shared defaults? Restart Kodi to apply. Running playback will not be changed."))
                    if self.dialog.yesno(self.title, message):
                        self.state = self.backend.reset(self.state) if action == "reset" else self.backend.save(self.state, self.pending)
                        self.pending.clear()
                        self.dialog.ok(self.title, self.tr(30019, "Saved — restart Kodi to apply. Live decoder state is unknown."))
            except SaveUncertain as error:
                self.write_blocked = True
                self.dialog.ok(self.title, str(error))
            except ConfigError as error:
                self.dialog.ok(self.title, str(error))
