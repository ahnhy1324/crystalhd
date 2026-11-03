#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Bounded card/Pulse-clock FIFO/latest comparison; no physical-display proof."""
import argparse
import importlib.util
import json
from pathlib import Path
import resource
import time


def structure(value):
    return {value.nth_field_name(i): value.get_value(value.nth_field_name(i))
            for i in range(value.n_fields())}


def temperature():
    values = [int(p.read_text()) for p in Path("/sys/class/thermal").glob("thermal_zone*/temp")]
    if not values:
        raise RuntimeError("thermal observations unavailable")
    return max(values)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("file", type=Path)
    parser.add_argument("--mode", choices=("fifo", "latest"), required=True)
    parser.add_argument("--results", type=Path, required=True)
    parser.add_argument("--expected-plugin", type=Path, required=True)
    parser.add_argument("--expected-pictures", type=int, required=True)
    args = parser.parse_args()
    if args.results.exists() or not args.results.parent.is_dir():
        parser.error("use a new results file in an existing directory")
    if args.expected_pictures <= 0 or not args.file.is_file():
        parser.error("a local fixture and positive expected picture count are required")
    report = dict(mode=args.mode, file=str(args.file.resolve()), eos=False, code=None,
                  audio_buffers=0, sink_buffers=0, clock_samples=0, audio_clock_mismatches=0,
                  same_audio_clock=False, audio_stream_muted=True, warnings=[], errors=[],
                  message_counts={"warnings": 0, "errors": 0},
                  qos_sources={}, sizes=[], pass_scope="card/audio-clock/counts/retirement only",
                  limits=["No source pixel or unique displayed-picture census, physical scanout or endurance claim.",
                          "No queue-age or submit-to-ACK latency measurement; CPU is process-only.",
                          "Zero replacements means replacement behavior was not exercised; no gain is demonstrated."])
    player = window = sink = None
    cpu0 = resource.getrusage(resource.RUSAGE_SELF)
    begin = time.monotonic()
    try:
        import gi
        gi.require_version("Gst", "1.0")
        gi.require_version("GstVideo", "1.0")
        gi.require_version("Gtk", "3.0")
        gi.require_version("GdkX11", "3.0")
        from gi.repository import GLib, Gst, GstVideo, Gtk, GdkX11
        Gst.init(None)
        factory = Gst.ElementFactory.find("crystalhdxvsink")
        if factory is None or Path(factory.get_plugin().get_filename()).resolve() != args.expected_plugin.resolve():
            raise RuntimeError("selected Xv plugin differs from --expected-plugin")
        report["plugin"] = str(args.expected_plugin.resolve())
        root = Path(__file__).resolve().parents[1]
        spec = importlib.util.spec_from_file_location("xv_bench_player", root / "scripts/crystalhd-player.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        argv = ["--hardware", "--video-output", "xv-async", "--scale-width", "1280",
                "--software-output-size", "native"]
        if args.mode == "latest":
            argv.append("--xv-latest-frame")
        options = module.arguments(argv + [str(args.file.resolve())], environment={})
        audio = Gst.ElementFactory.make("pulsesink", "xv-bench-audio")
        if audio is None:
            raise RuntimeError("real PulseSink is required; no fake-audio fallback")
        audio.set_property("mute", True)

        class BenchPlayer(module.Player):
            def close(self):
                if self.closed:
                    return
                try:
                    report["base_sink_before_null"] = structure(sink.get_property("stats"))
                    report["xv_before_null"] = structure(sink.get_property("xv-stats"))
                except Exception as error:
                    report["snapshot_error"] = str(error)
                finally:
                    super().close()

        player = BenchPlayer(options, Gst, GLib, {"audio-sink": audio})
        sink = player.pipeline.get_property("video-sink")
        if sink.get_factory().get_name() != "crystalhdxvsink":
            raise RuntimeError("Player did not select the expected Xv sink")
        window = Gtk.Window(title="CrystalHD Xv " + args.mode + " benchmark")
        area = Gtk.DrawingArea()
        window.add(area)
        window.set_default_size(640, 360)
        window.set_resizable(False)
        window.show_all()
        while Gtk.events_pending():
            Gtk.main_iteration_do(False)
        if [area.get_allocated_width(), area.get_allocated_height()] != [640, 360]:
            raise RuntimeError("benchmark requires a mapped640x360 X11 area")
        handle = GdkX11.X11Window.get_xid(area.get_window())
        GstVideo.VideoOverlay.set_window_handle(sink, handle)
        window.connect("delete-event", lambda *_: (player.control("quit"), True)[1])

        def audio_probe(_pad, _info):
            report["audio_buffers"] += 1
            return Gst.PadProbeReturn.OK

        def video_probe(pad, _info):
            report["sink_buffers"] += 1
            if "sink_caps" not in report:
                caps = pad.get_current_caps()
                report["sink_caps"] = caps.to_string()
                values = caps.get_structure(0)
                report["native_progressive"] = (values.get_value("width") == 1280 and
                    values.get_value("height") == 720 and values.get_string("format") == "UYVY" and
                    values.get_string("interlace-mode") == "progressive")
            return Gst.PadProbeReturn.OK

        audio.get_static_pad("sink").add_probe(Gst.PadProbeType.BUFFER, audio_probe)
        sink.get_static_pad("sink").add_probe(Gst.PadProbeType.BUFFER, video_probe)

        def observe(_bus, message):
            if message.type == Gst.MessageType.EOS:
                report["eos"] = True
            elif message.type in (Gst.MessageType.WARNING, Gst.MessageType.ERROR):
                error, debug = (message.parse_warning() if message.type == Gst.MessageType.WARNING
                                else message.parse_error())
                key = "warnings" if message.type == Gst.MessageType.WARNING else "errors"
                report["message_counts"][key] += 1
                if len(report[key]) < 64:
                    report[key].append(dict(source=message.src.get_name() if message.src else None,
                                            message=str(error), debug=debug))
            elif message.type == Gst.MessageType.QOS:
                fmt, processed, dropped = message.parse_qos_stats()
                report["qos_sources"][message.src.get_name() if message.src else "unknown"] = dict(
                    format=str(fmt), processed=processed, dropped=dropped)

        player.bus.connect("message", observe)

        def tick():
            try:
                report["peak_mC"] = max(report["peak_mC"], temperature())
                if report["peak_mC"] >= 75000:
                    player.fail("benchmark thermal guard75C")
                size = [area.get_allocated_width(), area.get_allocated_height()]
                if size not in report["sizes"]:
                    report["sizes"].append(size)
                if player.pipeline.get_state(0).state == Gst.State.PLAYING:
                    clock, audio_clock = player.pipeline.get_clock(), audio.provide_clock()
                    if clock and audio_clock:
                        report["clock_samples"] += 1
                        report.update(clock=clock.get_name(), audio_clock=audio_clock.get_name(),
                                      same_audio_clock=clock == audio_clock)
                        report["audio_clock_mismatches"] += int(clock != audio_clock)
            except Exception as error:
                report["observation_error"] = str(error)
                player.fail("benchmark observation failed")
            return not player.done

        report["start_mC"] = report["peak_mC"] = temperature()
        if report["start_mC"] >= 75000:
            raise RuntimeError("start temperature must be below75C")
        player.sources.append(GLib.timeout_add(250, tick))
        player.sources.append(GLib.timeout_add_seconds(45, lambda: (
            player.fail("benchmark45s watchdog"), False)[1]))
        cpu0 = resource.getrusage(resource.RUSAGE_SELF)
        begin = time.monotonic()
        report["code"] = player.run()
    except Exception as error:
        report["exception"] = repr(error)
    finally:
        if player is not None:
            try:
                player.close()
                report.update(decoded=player.health.frames, health_error=player.health.error,
                              closed=player.closed, code=player.exit_code if report["code"] is None else report["code"])
                report["pipeline_null"] = player.pipeline.get_state(2*Gst.SECOND).state == Gst.State.NULL
                if sink is not None:
                    report["xv_after_null"] = structure(sink.get_property("xv-stats"))
            except Exception as error:
                report["cleanup_error"] = repr(error)
        cpu1 = resource.getrusage(resource.RUSAGE_SELF)
        report.update(elapsed_s=time.monotonic()-begin,
                      cpu_s=cpu1.ru_utime+cpu1.ru_stime-cpu0.ru_utime-cpu0.ru_stime,
                      max_rss_kib=cpu1.ru_maxrss)
        if window is not None:
            try:
                window.destroy()
            except Exception as error:
                report["cleanup_error"] = repr(error)
        after = report.get("xv_after_null", {})
        clean = (all(after.get(key) == 0 for key in ("pending", "images", "bytes")) and
                 after.get("allocated") == after.get("freed") and after.get("closed") is True and
                 after.get("failed") is False and after.get("quarantined") is False and
                 after.get("latest-pending") is False and after.get("retained-picture") is False and
                 after.get("latest-frame") == (args.mode == "latest") and
                 after.get("video-submitted", 0) > 0 and
                 after.get("submitted") ==
                 after.get("acknowledged", 0) + after.get("terminal-retired", 0))
        report["replacement_observed"] = after.get("latest-replaced", 0) > 0
        report["pass"] = (report["code"] == 0 and report["eos"] and clean and
            report.get("pipeline_null") is True and report.get("decoded") == args.expected_pictures and
            not report.get("health_error") and report["audio_buffers"] > 0 and
            report["same_audio_clock"] and report["audio_clock_mismatches"] == 0 and
            report.get("native_progressive") is True and report["sizes"] == [[640, 360]] and
            report.get("peak_mC", 75000) < 75000 and not report["errors"] and
            not any(key in report for key in ("exception", "cleanup_error", "snapshot_error", "observation_error")))
        with args.results.open("x", encoding="utf-8") as stream:
            json.dump(report, stream, indent=2)
            stream.write("\n")
    print(json.dumps(report), flush=True)
    return 0 if report["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
