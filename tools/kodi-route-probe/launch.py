"""Run inside Kodi with RunScript(launch.py,/absolute/path/high.mp4)."""
import sys
import xbmc
import xbmcgui

if len(sys.argv) != 2 or not sys.argv[1].startswith("/"):
    raise RuntimeError("Gate A requires one absolute local H.264 MP4 path")
item = xbmcgui.ListItem(path=sys.argv[1])
item.setProperty("inputstream", "inputstream.crystalhd.routeprobe")
item.setContentLookup(False)
xbmc.Player().play(sys.argv[1], item)
