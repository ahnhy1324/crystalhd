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
            StateChangeReturn=SimpleNamespace(FAILURE=0))
        control.pipeline = mock.Mock()
        control.pipeline.query_position.return_value = (True, 12000)
        control.pipeline.query_duration.return_value = (True, 60000)
        control.pipeline.seek.return_value = True
        control.pipeline.set_state.return_value = 1
        control.rate = 1.0
        control.health = player.PlaybackHealth("software", 0)
        control.stop = mock.Mock()
        return control

    def test_seek_and_rate_use_explicit_flushing_time_seek(self):
        control = self.controller()
        with contextlib.redirect_stdout(io.StringIO()):
            control.control("back")
            self.assertEqual(control.pipeline.seek.call_args.args, (1.0, 0, 3, 1, 2000, 0, -1))
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

    def test_pause_resume_and_zero_output_quit(self):
        control = self.controller()
        with contextlib.redirect_stdout(io.StringIO()):
            control.control("pause")
            self.assertTrue(control.health.paused)
            control.control("pause")
            self.assertFalse(control.health.paused)
            control.control("quit")
        control.stop.assert_called_with(1)


class ArgumentTests(unittest.TestCase):
    def test_local_paths_and_subtitles_are_safely_encoded(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "video #?.mp4"
            path.touch()
            args = player.arguments(["--software", "--subtitles", str(path), str(path)])
            self.assertEqual(args.mode, "software")
            self.assertIn("video%20%23%3F.mp4", args.file.as_uri())
            self.assertEqual(args.file, args.subtitles)
            self.assertEqual(player.arguments([str(path)]).mode, "hardware")

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
                               subtitles=Path(self.temp.name) / "sub titles.srt")
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
