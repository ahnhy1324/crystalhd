#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Explicit card/audio-clock benchmark; never installation or physical A/V proof."""
import argparse
import importlib.util
import json
from pathlib import Path
import resource
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("file", type=Path)
    parser.add_argument("--results", type=Path, required=True)
    parser.add_argument("--expected-plugin", type=Path, required=True)
    parser.add_argument("--label", required=True)
    parser.add_argument("--expected-pictures", type=int, default=750)
    parser.add_argument("--fullscreen", action="store_true")
    args = parser.parse_args()
    if args.results.exists():
        parser.error("use a new results file")
    if args.expected_pictures <= 0 or not args.file.is_file():
        parser.error("a local fixture and positive expected picture count are required")

    import gi
    gi.require_version("Gst", "1.0")
    gi.require_version("GstVideo", "1.0")
    gi.require_version("Gtk", "3.0")
    gi.require_version("GdkX11", "3.0")
    from gi.repository import GLib, Gst, GstVideo, Gtk, GdkX11
    Gst.init(None)
    factory = Gst.ElementFactory.find("crystalhdfieldsink")
    if factory is None or Path(factory.get_plugin().get_filename()).resolve() != args.expected_plugin.resolve():
        raise RuntimeError("the selected experimental plugin does not match --expected-plugin")

    root = Path(__file__).resolve().parents[1]
    spec = importlib.util.spec_from_file_location("field_bench_player", root / "scripts/crystalhd-player.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    options = module.arguments(["--hardware", "--video-output", "field-gpu",
                                "--scale-width", "1280", "--software-output-size", "native",
                                str(args.file.resolve())], environment={})
    audio = Gst.ElementFactory.make("pulsesink", "field-bench-audio")
    if audio is None:
        raise RuntimeError("PulseSink is required; there is no fake-audio fallback")
    # Real audio samples and its clock still run; mute this stream only.
    audio.set_property("mute", True)
    player = module.Player(options, Gst, GLib, {"audio-sink": audio})
    window = Gtk.Window(title="CrystalHD field benchmark")
    area = Gtk.DrawingArea()
    window.add(area)
    window.set_default_size(640, 360)
    window.set_resizable(False)
    if args.fullscreen:
        window.fullscreen()
    window.show_all()
    while Gtk.events_pending():
        Gtk.main_iteration_do(False)
    native = area.get_window()
    if native is None:
        raise RuntimeError("the X11 benchmark area was not realized")
    handle = GdkX11.X11Window.get_xid(native)
    GstVideo.VideoOverlay.set_window_handle(player.field_sink, handle)
    window.connect("delete-event", lambda *_: (player.control("quit"), True)[1])

    report = dict(label=args.label, fullscreen=args.fullscreen, eos=False,
                  audio_buffers=0, audio_stream_muted=True, clock=None,
                  audio_clock=None, same_audio_clock=False, peak_mC=0, sizes=[])

    def audio_probe(_pad, _info):
        report["audio_buffers"] += 1
        return Gst.PadProbeReturn.OK

    audio.get_static_pad("sink").add_probe(Gst.PadProbeType.BUFFER, audio_probe)
    player.bus.connect("message::eos", lambda *_: report.update(eos=True))

    def tick():
        report["peak_mC"] = max(report["peak_mC"], max(
            int(p.read_text()) for p in Path("/sys/class/thermal").glob("thermal_zone*/temp")))
        if report["peak_mC"] >= 85000:
            player.fail("benchmark thermal guard85C")
        clock, audio_clock = player.pipeline.get_clock(), audio.provide_clock()
        if clock and audio_clock:
            report.update(clock=clock.get_name(), audio_clock=audio_clock.get_name(),
                          same_audio_clock=clock == audio_clock)
        size = [area.get_allocated_width(), area.get_allocated_height()]
        if size not in report["sizes"]:
            report["sizes"].append(size)
        return not player.done

    player.sources.append(GLib.timeout_add(250, tick))
    player.sources.append(GLib.timeout_add_seconds(45, lambda: (
        player.fail("benchmark45s watchdog"), False)[1]))
    initial_temperature = max(int(p.read_text()) for p in Path("/sys/class/thermal").glob("thermal_zone*/temp"))
    if initial_temperature >= 75000:
        player.close()
        window.destroy()
        raise RuntimeError("start temperature must be below75C")
    cpu0 = resource.getrusage(resource.RUSAGE_SELF)
    begin = time.monotonic()
    code = player.run()
    cpu1 = resource.getrusage(resource.RUSAGE_SELF)
    fields = player.field_sink.get_property("field-stats")
    base = player.field_sink.get_property("stats")
    report.update(code=code, decoded=player.health.frames, health_error=player.health.error,
                  elapsed_s=time.monotonic()-begin,
                  cpu_s=cpu1.ru_utime+cpu1.ru_stime-cpu0.ru_utime-cpu0.ru_stime,
                  max_rss_kib=cpu1.ru_maxrss, start_mC=initial_temperature,
                  field_stats={fields.nth_field_name(i): fields.get_value(fields.nth_field_name(i))
                               for i in range(fields.n_fields())},
                  whole_buffer_drops=base.get_value("dropped"))
    window.destroy()
    clean = all(report["field_stats"][key] == 0
                for key in ("failures", "allocations", "images", "retained-buffers"))
    report["pass"] = (code == 0 and report["eos"] and clean and
                      report["decoded"] == args.expected_pictures and
                      report["audio_buffers"] > 0 and report["same_audio_clock"])
    with args.results.open("x", encoding="utf-8") as stream:
        json.dump(report, stream, indent=2)
        stream.write("\n")
    print(json.dumps(report), flush=True)
    return 0 if report["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
