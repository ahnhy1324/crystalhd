# SPDX-License-Identifier: LGPL-2.1-or-later
"""Stock-Kodi entry point; shared managed files remain authoritative."""
import xbmcaddon
import xbmcgui

from resources.lib.controller import Backend, ConfigError, SettingsController


def main():
    addon = xbmcaddon.Addon()
    dialog = xbmcgui.Dialog()

    def translate(identifier, fallback):
        return addon.getLocalizedString(identifier) or fallback

    try:
        SettingsController(Backend(), dialog, translate).run()
    except ConfigError as error:
        dialog.ok(translate(30000, "CrystalHD settings"), str(error))


if __name__ == "__main__":
    main()
