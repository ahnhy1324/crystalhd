#!/usr/bin/python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Player policy and real software-only GStreamer API regressions; no card."""
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("crystalhd_player", ROOT / "scripts/crystalhd-player.py")
player = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(player)
HARDWARE = "Codec/Decoder/Video/Hardware"
SOFTWARE = "Codec/Decoder/Video"


class HealthTests(unittest.TestCase):
    def test_requires_nonempty_raw_output_not_creation(self):
        health = player.PlaybackHealth("hardware", 0)
        self.assertFalse(health.frame(1, "crystalhddec", HARDWARE, False, 512, 1))
        self.assertFalse(health.frame(1, "crystalhddec", HARDWARE, True, 0, 2))
        self.assertEqual(health.frames, 0)
        self.assertEqual(health.finish(), 1)

    def test_actual_hardware_output_can_complete(self):
        health = player.PlaybackHealth("hardware", 0)
        self.assertTrue(health.frame(1, "crystalhddec", HARDWARE, True, 512, 1))
        self.assertFalse(health.frame(1, "crystalhddec", HARDWARE, True, 512, 2))
        self.assertEqual((health.frames, health.finish()), (2, 0))

    def test_wrong_hardware_decoder_is_sticky_error(self):
        health = player.PlaybackHealth("hardware", 0)
        health.frame(1, "avdec_h264", SOFTWARE, True, 512, 1)
        health.frame(2, "crystalhddec", HARDWARE, True, 512, 2)
        self.assertEqual(health.frames, 0)
        self.assertEqual(health.finish(), 1)
        self.assertIn("avdec_h264", health.error)

    def test_software_rejects_hardware_output(self):
        health = player.PlaybackHealth("software", 0)
        health.frame(1, "crystalhddec", HARDWARE, True, 512, 1)
        self.assertEqual(health.finish(), 1)

    def test_replacement_must_produce_its_own_frames(self):
        health = player.PlaybackHealth("software", 0)
        health.frame(1, "avdec_h264", SOFTWARE, True, 512, 1)
        health.removed(1, 2)
        self.assertEqual(health.active_frames, 0)
        self.assertEqual(health.finish(), 1)

    def test_no_output_and_stall_are_bounded(self):
        health = player.PlaybackHealth("software", 0)
        self.assertIsNone(health.tick(20))
        self.assertIn("no decoded", health.tick(20.1))
        health = player.PlaybackHealth("software", 0)
        health.frame(1, "theoradec", SOFTWARE, True, 512, 1)
        self.assertIn("stalled", health.tick(17))

    def test_human_pause_does_not_time_out(self):
        health = player.PlaybackHealth("software", 0)
        health.frame(1, "theoradec", SOFTWARE, True, 512, 1)
        health.pause(True, 2)
        self.assertIsNone(health.tick(9999))
        health.pause(False, 10000)
        self.assertIsNone(health.tick(10001))
        self.assertIsNotNone(health.tick(10016))

    def test_seek_grace_is_not_new_frame_evidence(self):
        health = player.PlaybackHealth("hardware", 0)
        health.seeking(19)
        self.assertIsNone(health.tick(21))
        self.assertEqual(health.finish(), 1)

    def test_late_error_cannot_be_overwritten(self):
        health = player.PlaybackHealth("software", 0)
        health.frame(1, "theoradec", SOFTWARE, True, 512, 1)
        health.fail("late decode failure")
        health.fail("secondary error")
        self.assertEqual(health.finish(), 1)
        self.assertEqual(health.error, "late decode failure")


class Factory:
    def __init__(self, name, klass):
        self.name, self.klass, self.rank = name, klass, 256

    def get_name(self):
        return self.name

    def get_metadata(self, _key):
        return self.klass

    def set_rank(self, rank):
        self.rank = rank


class PolicyAndControlTests(unittest.TestCase):
    def test_rank_filter_preserves_parsers_and_audio(self):
        factories = [Factory("crystalhddec", HARDWARE), Factory("avdec_h264", SOFTWARE),
                     Factory("h264parse", "Codec/Parser/Video"),
                     Factory("faad", "Codec/Decoder/Audio")]
        Gst = SimpleNamespace(Registry=SimpleNamespace(get=lambda: SimpleNamespace(
            get_feature_list=lambda _type: factories)), ElementFactory=object,
            Rank=SimpleNamespace(NONE=0, PRIMARY=256))
        player.configure_decoders(Gst, "hardware")
        self.assertEqual([f.rank for f in factories], [356, 0, 256, 256])
        player.configure_decoders(Gst, "software")
        self.assertEqual(factories[0].rank, 0)

    def test_missing_hardware_plugin_fails(self):
        Gst = SimpleNamespace(Registry=SimpleNamespace(get=lambda: SimpleNamespace(
            get_feature_list=lambda _type: [])), ElementFactory=object)
        with self.assertRaisesRegex(RuntimeError, "unavailable"):
            player.configure_decoders(Gst, "hardware")

    def test_partial_arrow_sequences_and_commands(self):
        keys = player.ControlKeys()
        self.assertEqual(keys.feed(b"\x1b["), [])
        self.assertEqual(keys.feed(b"Dj l123q"),
                         ["back", "back", "pause", "forward", "rate-half",
                          "rate-normal", "rate-double", "quit"])
        self.assertEqual(keys.feed(b"\x1b[C"), ["forward"])
        self.assertEqual(keys.feed(b"unmapped\n"), [])

    def controller(self):
        control = player.Player.__new__(player.Player)
        control.Gst = SimpleNamespace(SECOND=1000, Format=SimpleNamespace(TIME=0),
            SeekFlags=SimpleNamespace(FLUSH=1, ACCURATE=2),
            SeekType=SimpleNamespace(SET=1, NONE=0),
            State=SimpleNamespace(PAUSED=1, PLAYING=2),
            StateChangeReturn=SimpleNamespace(FAILURE=0, SUCCESS=1, ASYNC=2),
            MessageType=SimpleNamespace(APPLICATION=1, ERROR=2, EOS=3,
                ASYNC_DONE=4, CLOCK_LOST=5, LATENCY=6, STATE_CHANGED=7))
        control.pipeline = mock.Mock()
        control.pipeline.query_position.return_value = (True, 12000)
        control.pipeline.query_duration.return_value = (True, 60000)
        control.pipeline.seek.return_value = True
        control.pipeline.set_state.return_value = 1
        control.pipeline.get_state.return_value = (1, 2, 0)
        control.rate = 1.0
        control.seek_target = None
        control.queued_seek = None
        control.health = player.PlaybackHealth("software", 0)
        control.stop = mock.Mock()
        control.done = False
        return control

    def test_seek_and_rate_use_explicit_flushing_time_seek(self):
        control = self.controller()
        with contextlib.redirect_stdout(io.StringIO()):
            control.control("back")
            self.assertEqual(control.pipeline.seek.call_args.args, (1.0, 0, 3, 1, 2000, 0, -1))
            control.seek_completed()
            control.control("rate-double")
        self.assertEqual(control.rate, 2.0)
        self.assertEqual(control.pipeline.seek.call_args.args[0], 2.0)

    def test_rejected_rate_does_not_claim_success(self):
        control = self.controller()
        control.pipeline.seek.return_value = False
        with contextlib.redirect_stderr(io.StringIO()):
            control.control("rate-half")
        self.assertEqual(control.rate, 1.0)
        self.assertEqual(control.health.anchor, 0)

    def test_controls_during_flush_are_coalesced_without_position_query(self):
        control = self.controller()
        with contextlib.redirect_stdout(io.StringIO()):
            control.control("forward")
            control.pipeline.query_position.return_value = (False, -1)
            control.control("forward")
            control.control("back")
            control.control("rate-half")
            self.assertEqual(control.pipeline.seek.call_count, 1)
            self.assertEqual(control.pipeline.query_position.call_count, 1)
            self.assertEqual(control.queued_seek, (22000, 0.5))
            self.assertEqual(control.rate, 1.0)
            control.seek_completed()
            self.assertEqual(control.pipeline.seek.call_count, 2)
            self.assertEqual(control.pipeline.seek.call_args.args,
                             (0.5, 0, 3, 1, 22000, 0, -1))
            self.assertEqual(control.rate, 0.5)
            control.seek_completed()
            self.assertIsNone(control.seek_target)

    def test_rejected_queued_seek_keeps_accepted_settings(self):
        control = self.controller()
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            control.control("forward")
            control.control("rate-half")
            control.pipeline.seek.return_value = False
            control.seek_completed()
        self.assertEqual(control.rate, 1.0)
        self.assertIsNone(control.seek_target)
        self.assertIsNone(control.queued_seek)

    def test_startup_async_done_does_not_issue_seek(self):
        control = self.controller()
        control.seek_completed()
        control.pipeline.seek.assert_not_called()

    def test_old_async_done_does_not_complete_pending_preroll(self):
        control = self.controller()
        with contextlib.redirect_stdout(io.StringIO()):
            control.control("forward")
            control.control("rate-half")
            control.pipeline.get_state.return_value = (2, 1, 2)
            control.seek_completed()
            self.assertEqual(control.pipeline.seek.call_count, 1)
            self.assertEqual(control.seek_target, (22000, 1.0))
            self.assertEqual(control.queued_seek, (22000, 0.5))
            control.pipeline.get_state.return_value = (1, 2, 0)
            control.seek_completed()
            self.assertEqual(control.pipeline.seek.call_count, 2)
            self.assertEqual(control.rate, 0.5)

    def test_failed_preroll_does_not_submit_queued_control(self):
        control = self.controller()
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            control.control("forward")
            control.control("rate-half")
            control.pipeline.get_state.return_value = (0, 1, 2)
            control.seek_completed()
        self.assertEqual(control.pipeline.seek.call_count, 1)
        self.assertIn("preroll", control.health.error)
        control.stop.assert_called_with(1)

    def test_final_pipeline_state_completes_deferred_seek(self):
        control = self.controller()
        with contextlib.redirect_stdout(io.StringIO()):
            control.control("forward")
            control.control("rate-half")
            control.pipeline.get_state.return_value = (2, 1, 2)
            control.message(None, SimpleNamespace(type=4, src=control.pipeline))
            self.assertEqual(control.pipeline.seek.call_count, 1)
            control.pipeline.get_state.return_value = (1, 2, 0)
            control.message(None, SimpleNamespace(type=7, src=object()))
            self.assertEqual(control.pipeline.seek.call_count, 1)
            control.message(None, SimpleNamespace(type=7, src=control.pipeline))
            self.assertEqual(control.pipeline.seek.call_count, 2)
            self.assertEqual(control.rate, 0.5)
            self.assertIsNone(control.queued_seek)

    def test_pause_resume_and_zero_output_quit(self):
        control = self.controller()
        with contextlib.redirect_stdout(io.StringIO()):
            control.control("pause")
            self.assertTrue(control.health.paused)
            control.control("pause")
            self.assertFalse(control.health.paused)
            control.control("quit")
        control.stop.assert_called_with(1)


class ScaleConfigurationTests(unittest.TestCase):
    def controller(self, width=1280):
        result = player.Player.__new__(player.Player)
        result.args = SimpleNamespace(scale_width=width)
        result.Gst = SimpleNamespace(PadProbeType=SimpleNamespace(BUFFER=1))
        result.next_decoder = 0
        result.decoders = {}
        result.fail = mock.Mock()
        return result

    def decoder(self, name="crystalhddec", klass=HARDWARE, property_available=True):
        result = mock.Mock()
        result.get_factory.return_value = Factory(name, klass)
        result.find_property.return_value = object() if property_available else None
        result.get_static_pad.return_value = None
        return result

    def test_scale_width_is_set_only_on_crystalhd_decoder(self):
        control = self.controller()
        decoder = self.decoder()
        control.element_setup(None, decoder)
        decoder.set_property.assert_called_once_with("scale-width", 1280)
        self.assertEqual(control.decoders, {decoder: 1})
        control.fail.assert_not_called()

        other = self.decoder("avdec_h264", SOFTWARE)
        control.element_setup(None, other)
        other.find_property.assert_not_called()
        other.set_property.assert_not_called()

    def test_old_plugin_fails_instead_of_silently_ignoring_scale(self):
        control = self.controller()
        decoder = self.decoder(property_available=False)
        control.element_setup(None, decoder)
        control.fail.assert_called_once()
        self.assertIn("update", control.fail.call_args.args[0])
        decoder.set_property.assert_not_called()
        self.assertEqual(control.decoders, {})

    def test_native_width_does_not_touch_decoder_property(self):
        control = self.controller(0)
        decoder = self.decoder()
        control.element_setup(None, decoder)
        decoder.find_property.assert_not_called()
        decoder.set_property.assert_not_called()
        self.assertEqual(control.decoders, {decoder: 1})


class CleanupTests(unittest.TestCase):
    def test_teardown_failure_is_nonzero_and_cleanup_remains_idempotent(self):
        for failed, previous_code in ((False, 0), (True, 0), (True, 143)):
            with self.subTest(failed=failed, previous_code=previous_code):
                control = player.Player.__new__(player.Player)
                control.closed = False
                control.terminal_state = None
                control.sources = []
                control.exit_code = previous_code
                control.Gst = SimpleNamespace(State=SimpleNamespace(NULL=0),
                    StateChangeReturn=SimpleNamespace(FAILURE=-1))
                control.bus = mock.Mock()
                control.pipeline = mock.Mock()
                control.pipeline.set_state.return_value = -1 if failed else 1
                output = io.StringIO()
                with contextlib.redirect_stderr(output):
                    control.close()
                    control.close()
                self.assertEqual(control.exit_code, max(previous_code, int(failed)))
                control.pipeline.set_state.assert_called_once_with(0)
                control.bus.remove_signal_watch.assert_called_once_with()
                self.assertEqual("teardown failed" in output.getvalue(), failed)


class ArgumentTests(unittest.TestCase):
    def test_local_paths_and_subtitles_are_safely_encoded(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "video #?.mp4"
            path.touch()
            args = player.arguments(["--software", "--subtitles", str(path), str(path)])
            self.assertEqual(args.mode, "software")
            self.assertIn("video%20%23%3F.mp4", args.file.as_uri())
            self.assertEqual(args.file, args.subtitles)
            defaults = player.arguments([str(path)])
            self.assertEqual(defaults.mode, "hardware")
            self.assertEqual(defaults.scale_width, 0)
            self.assertIsNone(defaults.software_upscale)

    def test_explicit_hardware_scale_and_software_upscale(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "video.mp4"
            path.touch()
            args = player.arguments([
                "--scale-width", "1280", "--software-upscale", "1920X1080", str(path)])
            self.assertEqual(args.scale_width, 1280)
            self.assertEqual(args.software_upscale, (1920, 1080))
            self.assertEqual(player.arguments(["--scale-width", "0", str(path)]).scale_width, 0)

    def test_scale_options_reject_unsafe_or_incoherent_combinations(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "video.mp4"
            path.touch()
            invalid = (
                ["--scale-width", "127", str(path)],
                ["--scale-width", "129", str(path)],
                ["--scale-width", "1920", str(path)],
                ["--scale-width", "wide", str(path)],
                ["--software-upscale", "1920x1080", str(path)],
                ["--scale-width", "1280", "--software-upscale", "1280x1080", str(path)],
                ["--scale-width", "1280", "--software-upscale", "1921x1080", str(path)],
                ["--scale-width", "1280", "--software-upscale", "32768x1080", str(path)],
                ["--scale-width", "1280", "--software-upscale", "1920-by-1080", str(path)],
                ["--software", "--scale-width", "1280", str(path)],
            )
            for argv in invalid:
                with self.subTest(argv=argv), contextlib.redirect_stderr(io.StringIO()):
                    with self.assertRaises(SystemExit) as error:
                        player.arguments(argv)
                    self.assertEqual(error.exception.code, 2)

    def test_cli_rejects_directories_uris_missing_files_and_conflicting_modes(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "video.mp4"
            path.touch()
            for args in ([directory], ["https://example.test/video.mp4"],
                         [str(path) + ".missing"],
                         ["--hardware", "--software", str(path)],
                         ["--subtitles", str(path) + ".missing", str(path)]):
                with self.subTest(args=args), contextlib.redirect_stderr(io.StringIO()):
                    with self.assertRaises(SystemExit) as error:
                        player.arguments(args)
                    self.assertEqual(error.exception.code, 2)

    def test_shell_source_and_installed_controller_mapping(self):
        environment = dict(os.environ, CRYSTALHD_PLAYER_PYTHON="/bin/echo")
        result = subprocess.run(["sh", str(ROOT / "scripts/crystalhd-play"), "--help"],
                                env=environment, text=True, capture_output=True, check=True)
        self.assertIn(str(ROOT / "scripts/crystalhd-player.py"), result.stdout)
        with tempfile.TemporaryDirectory() as directory:
            prefix = Path(directory)
            script = prefix / "bin/crystalhd-play"
            script.parent.mkdir()
            script.write_bytes((ROOT / "scripts/crystalhd-play").read_bytes())
            controller = prefix / "share/crystalhd/player.py"
            controller.parent.mkdir(parents=True)
            controller.touch()
            result = subprocess.run(["sh", str(script), "--software", "file with spaces"],
                                    env=environment, text=True, capture_output=True, check=True)
            self.assertIn("/share/crystalhd/player.py --software file with spaces", result.stdout)


class GStreamerApiTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            import gi
            gi.require_version("Gst", "1.0")
            from gi.repository import GLib, Gst
        except (ImportError, ValueError) as error:
            raise unittest.SkipTest(f"optional GStreamer API tests: {error}")
        cls.temp = tempfile.TemporaryDirectory(prefix="crystalhd-player-api-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.environment = mock.patch.dict(os.environ, {
            "GST_REGISTRY_1_0": str(Path(cls.temp.name) / "registry.bin")})
        cls.environment.start()
        cls.addClassCleanup(cls.environment.stop)
        Gst.init(None)
        cls.Gst, cls.GLib = Gst, GLib
        # Disable every hardware decoder before any test pipeline can start.
        player.configure_decoders(Gst, "software")

    def make_player(self):
        args = SimpleNamespace(mode="software", file=Path(self.temp.name) / "unused.ogg",
                               subtitles=Path(self.temp.name) / "sub titles.srt",
                               scale_width=0, software_upscale=None)
        result = player.Player(args, self.Gst, self.GLib, {
            "video-sink": self.Gst.ElementFactory.make("fakesink"),
            "audio-sink": self.Gst.ElementFactory.make("fakesink")})
        self.addCleanup(result.close)
        return result

    def run_pipeline(self, description):
        result = self.make_player()
        result.bus.remove_signal_watch()
        result.pipeline = self.Gst.parse_launch(description)
        result.bus = result.pipeline.get_bus()
        result.bus.add_signal_watch()
        result.bus.connect("message", result.message)
        decoder = result.pipeline.get_by_name("tested-decoder")
        if decoder is not None:
            result.element_setup(result.pipeline, decoder)
        with mock.patch("sys.stdin", new=io.StringIO()), \
                contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            code = result.run()
        return code, result

    def test_playbin_properties_and_software_flag(self):
        result = self.make_player()
        self.assertTrue(int(result.pipeline.get_property("flags")) & (1 << 12))
        self.assertTrue(result.pipeline.get_property("suburi").endswith("sub%20titles.srt"))
        self.assertIsNotNone(result.pipeline.get_property("video-sink"))
        self.assertIsNone(result.pipeline.get_property("video-filter"))

    def test_explicit_upscale_filter_has_expected_caps_and_quality_settings(self):
        for name in ("videoscale", "capsfilter"):
            if self.Gst.ElementFactory.find(name) is None:
                self.skipTest(f"optional upscale fixture needs {name}")
        result = player.make_software_upscale_filter(self.Gst, (1920, 1080))
        scale = result.get_by_name("crystalhd-upscale")
        capsfilter = result.get_by_name("crystalhd-upscale-caps")
        self.assertEqual(scale.get_property("method"), 4)
        if scale.find_property("n-threads") is not None:
            self.assertEqual(scale.get_property("n-threads"), 2)
        caps = capsfilter.get_property("caps")
        structure = caps.get_structure(0)
        self.assertEqual(structure.get_name(), "video/x-raw")
        self.assertEqual(structure.get_string("format"), "YUY2")
        self.assertEqual(structure.get_value("width"), 1920)
        self.assertEqual(structure.get_value("height"), 1080)
        self.assertIsNotNone(result.get_static_pad("sink"))
        self.assertIsNotNone(result.get_static_pad("src"))

    def test_explicit_upscale_filter_processes_raw_video(self):
        for name in ("videotestsrc", "videoscale", "capsfilter", "fakesink"):
            if self.Gst.ElementFactory.find(name) is None:
                self.skipTest(f"optional upscale fixture needs {name}")
        pipeline = self.Gst.Pipeline.new("upscale-test")
        source = self.Gst.ElementFactory.make("videotestsrc")
        upscale = player.make_software_upscale_filter(self.Gst, (640, 360))
        sink = self.Gst.ElementFactory.make("fakesink")
        source.set_property("num-buffers", 3)
        pipeline.add(source)
        pipeline.add(upscale)
        pipeline.add(sink)
        self.assertTrue(source.link(upscale))
        self.assertTrue(upscale.link(sink))
        self.addCleanup(pipeline.set_state, self.Gst.State.NULL)
        self.assertNotEqual(
            pipeline.set_state(self.Gst.State.PLAYING), self.Gst.StateChangeReturn.FAILURE)
        message = pipeline.get_bus().timed_pop_filtered(
            5 * self.Gst.SECOND, self.Gst.MessageType.EOS | self.Gst.MessageType.ERROR)
        self.assertIsNotNone(message)
        if message.type == self.Gst.MessageType.ERROR:
            error, debug = message.parse_error()
            self.fail(f"upscale pipeline failed: {error.message}: {debug}")
        caps = sink.get_static_pad("sink").get_current_caps().get_structure(0)
        self.assertEqual((caps.get_value("width"), caps.get_value("height")), (640, 360))

    def test_actual_software_decoder_buffers_precede_eos(self):
        for name in ("videotestsrc", "theoraenc", "theoradec", "fakesink"):
            if self.Gst.ElementFactory.find(name) is None:
                self.skipTest(f"optional software API fixture needs {name}")
        code, result = self.run_pipeline(
            "videotestsrc num-buffers=3 ! video/x-raw,width=32,height=32 ! "
            "theoraenc ! theoradec name=tested-decoder ! fakesink sync=false")
        self.assertEqual(code, 0)
        self.assertEqual(result.health.frames, 3)

    def test_zero_video_frames_fail(self):
        code, result = self.run_pipeline("fakesrc num-buffers=0 ! fakesink")
        self.assertEqual(code, 1)
        self.assertIn("no output", result.health.error)

    def test_bus_error_is_nonzero(self):
        code, result = self.run_pipeline("fakesrc num-buffers=1 ! identity error-after=1 ! fakesink")
        self.assertEqual(code, 1)
        self.assertIsNotNone(result.health.error)


if __name__ == "__main__":
    unittest.main()
