#!/usr/bin/python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Player policy and real software-only GStreamer API regressions; no card."""
import contextlib
import importlib.util
import io
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import threading
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
            StateChangeReturn=SimpleNamespace(FAILURE=0, SUCCESS=1, ASYNC=2, NO_PREROLL=3),
            MessageType=SimpleNamespace(APPLICATION=1, ERROR=2, EOS=3,
                ASYNC_DONE=4, CLOCK_LOST=5, LATENCY=6, STATE_CHANGED=7, BUFFERING=8,
                WARNING=9, QOS=10))
        control.args = SimpleNamespace(file=Path("/unused.ogg"))
        control.pipeline = mock.Mock()
        control.pipeline.query_position.return_value = (True, 12000)
        control.pipeline.query_duration.return_value = (True, 60000)
        control.pipeline.seek.return_value = True
        control.pipeline.set_state.return_value = 1
        control.pipeline.get_state.return_value = (1, 2, 0)
        control.rate = 1.0
        control.buffering = False
        control.is_live = False
        control.seek_target = None
        control.queued_seek = None
        control.health = player.PlaybackHealth("software", 0)
        control.stop = mock.Mock()
        control.done = False
        return control

    def test_bus_subscribes_only_to_messages_the_player_handles(self):
        bus = mock.Mock()
        callback = mock.Mock()
        player.connect_bus_messages(bus, callback)
        self.assertEqual(
            bus.connect.call_args_list,
            [mock.call(f"message::{detail}", callback)
             for detail in player.HANDLED_BUS_MESSAGE_DETAILS])
        self.assertNotIn(mock.call("message", callback), bus.connect.call_args_list)
        self.assertIn(mock.call("message::warning", callback), bus.connect.call_args_list)

    def test_warning_is_visible_private_and_nonfatal_for_local_and_remote_input(self):
        remote = "https://user:password@example.test/v?token=secret"
        redirect = "HTTPS://other:private@cdn.test/v?key=hidden'QUERY_TAIL"
        for source in (Path("/unused.ogg"), remote):
            with self.subTest(source_type=type(source).__name__):
                control = self.controller()
                control.args.file = source
                control.exit_code = 0
                control.health.frame(1, "theoradec", SOFTWARE, True, 512, 1)
                before = vars(control.health).copy()
                warning = mock.Mock(type=control.Gst.MessageType.WARNING)
                warning.parse_warning.return_value = (SimpleNamespace(message=(
                    "Skipping undecodable MPEG-2 input frame 414: missing coded rows "
                    + remote + " redirected " + redirect)),
                    "/private/build/source.c:123: secret-debug")
                output = io.StringIO()
                with contextlib.redirect_stderr(output):
                    control.message(None, warning)
                text = output.getvalue()
                self.assertTrue(text.startswith("crystalhd-play: warning: "))
                self.assertIn("input frame 414: missing coded rows", text)
                self.assertIn("<remote URL>", text)
                for secret in ("password", "secret", "private", "hidden", "QUERY_TAIL",
                               "example.test", "cdn.test", "https", "source.c"):
                    self.assertNotIn(secret, text)
                self.assertEqual(vars(control.health), before)
                self.assertEqual((control.exit_code, control.done), (0, False))
                self.assertEqual(control.health.finish(), 0)
                control.stop.assert_not_called()

    def test_all_subscribed_bus_messages_keep_their_existing_actions(self):
        def message(kind, src=None):
            result = mock.Mock()
            result.type = kind
            result.src = src or mock.Mock()
            return result

        control = self.controller()
        control.decoded_frame = mock.Mock()
        control.decoder_removed = mock.Mock()
        frame = mock.Mock()
        frame.get_name.return_value = "crystalhd-player-frame"
        frame.get_value.side_effect = {
            "token": 1, "name": "theoradec", "klass": SOFTWARE,
            "raw": True, "size": 512, "observed": 2.0,
        }.get
        application = message(control.Gst.MessageType.APPLICATION)
        application.get_structure.return_value = frame
        control.message(None, application)
        control.decoded_frame.assert_called_once_with(
            1, "theoradec", SOFTWARE, True, 512, 2.0)

        removed = mock.Mock()
        removed.get_name.return_value = "crystalhd-player-removed"
        removed.get_value.return_value = 1
        application.get_structure.return_value = removed
        control.message(None, application)
        control.decoder_removed.assert_called_once_with(1)

        error = message(control.Gst.MessageType.ERROR)
        error.src.get_name.return_value = "broken-source"
        error.parse_error.return_value = (SimpleNamespace(message="decode failed"), None)
        control.message(None, error)
        self.assertEqual(control.health.error, "broken-source: decode failed")
        control.stop.assert_called_with(1)

        control = self.controller()
        control.health.finish = mock.Mock(return_value=7)
        control.message(None, message(control.Gst.MessageType.EOS))
        control.stop.assert_called_once_with(7)

        control = self.controller()
        control.seek_completed = mock.Mock()
        control.message(None, message(control.Gst.MessageType.ASYNC_DONE))
        control.seek_completed.assert_called_once_with()
        control.seek_completed.reset_mock()
        control.message(None, message(control.Gst.MessageType.STATE_CHANGED, control.pipeline))
        control.seek_completed.assert_called_once_with()

        control = self.controller()
        control.message(None, message(control.Gst.MessageType.CLOCK_LOST))
        self.assertEqual(control.pipeline.set_state.call_args_list,
                         [mock.call(control.Gst.State.PAUSED),
                          mock.call(control.Gst.State.PLAYING)])

        control = self.controller()
        control.message(None, message(control.Gst.MessageType.LATENCY))
        control.pipeline.recalculate_latency.assert_called_once_with()

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

    def test_buffering_pauses_only_nonlive_and_keeps_stall_bounded(self):
        control = self.controller()
        control.health.frame(1, "theoradec", SOFTWARE, True, 512, 1)
        message = SimpleNamespace(type=control.Gst.MessageType.BUFFERING,
                                  parse_buffering=lambda: 20)
        control.message(None, message)
        control.message(None, message)
        control.pipeline.set_state.assert_called_once_with(control.Gst.State.PAUSED)
        self.assertFalse(control.health.paused)
        self.assertEqual(control.health.anchor, 1)
        self.assertIn("stalled", control.health.tick(17))
        control.buffering_changed(100)
        self.assertEqual(control.pipeline.set_state.call_args, mock.call(control.Gst.State.PLAYING))

    def test_buffering_does_not_resume_a_human_pause(self):
        control = self.controller()
        with contextlib.redirect_stdout(io.StringIO()):
            control.control("pause")
            control.pipeline.set_state.reset_mock()
            control.buffering_changed(10)
            control.buffering_changed(100)
        self.assertTrue(control.health.paused)
        control.pipeline.set_state.assert_not_called()

    def test_human_resume_while_buffering_waits_for_completion(self):
        control = self.controller()
        with contextlib.redirect_stdout(io.StringIO()):
            control.buffering_changed(10)
            control.control("pause")
            control.control("pause")
        self.assertFalse(control.health.paused)
        self.assertEqual(control.pipeline.set_state.call_args, mock.call(control.Gst.State.PAUSED))
        control.buffering_changed(100)
        self.assertEqual(control.pipeline.set_state.call_args, mock.call(control.Gst.State.PLAYING))

    def test_live_sources_ignore_buffering_and_no_preroll_is_respected(self):
        control = self.controller()
        control.is_live = True
        control.buffering_changed(10)
        control.pipeline.set_state.assert_not_called()
        control = self.controller()
        control.pipeline.set_state.side_effect = [control.Gst.StateChangeReturn.NO_PREROLL,
                                                 control.Gst.StateChangeReturn.SUCCESS]
        control.buffering_changed(10)
        self.assertTrue(control.is_live)
        self.assertFalse(control.buffering)
        self.assertEqual(control.pipeline.set_state.call_args_list,
                         [mock.call(control.Gst.State.PAUSED), mock.call(control.Gst.State.PLAYING)])

    def test_clock_recovery_cannot_resume_buffering(self):
        control = self.controller()
        control.buffering = True
        control.message(None, SimpleNamespace(type=control.Gst.MessageType.CLOCK_LOST))
        control.pipeline.set_state.assert_not_called()

    def test_buffering_state_change_failure_is_not_ignored(self):
        control = self.controller()
        control.pipeline.set_state.return_value = control.Gst.StateChangeReturn.FAILURE
        control.buffering_changed(10)
        self.assertEqual(control.health.error, "buffering state change failed")
        control.stop.assert_called_once_with(1)

    def test_nonseekable_http_does_not_change_rate_or_health(self):
        for supported, seekable in ((False, False), (True, False), (True, True)):
            with self.subTest(supported=supported, seekable=seekable):
                control = self.controller()
                control.args.file = "https://example.test/live.ogg"
                query = mock.Mock()
                query.parse_seeking.return_value = (0, seekable, 0, 60000)
                control.Gst.Query = SimpleNamespace(new_seeking=lambda _format: query)
                control.pipeline.query.return_value = supported
                with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                    control.control("rate-double")
                if supported and seekable:
                    self.assertEqual(control.rate, 2.0)
                    control.pipeline.seek.assert_called_once()
                else:
                    self.assertEqual(control.rate, 1.0)
                    self.assertEqual(control.health.anchor, 0)
                    self.assertIsNone(control.seek_target)
                    control.pipeline.seek.assert_not_called()

    def test_remote_error_redacts_original_and_redirect_urls(self):
        control = self.controller()
        control.args.file = "https://user:password@example.test/v?token=secret"
        message = mock.Mock(type=control.Gst.MessageType.ERROR)
        message.src.get_name.return_value = "source"
        message.parse_error.return_value = (SimpleNamespace(message=(
            "Not Found: " + control.args.file + " redirected https://other:private@cdn.test/?key=hidden")), None)
        control.message(None, message)
        self.assertIn("Not Found", control.health.error)
        for secret in ("password", "secret", "private", "hidden", "example.test", "cdn.test"):
            self.assertNotIn(secret, control.health.error)

    def test_remote_error_redacts_apostrophe_query_suffixes(self):
        source = "https://origin.test/v?signature=original'ORIGINAL_TAIL"
        redirect = "https://cdn.test/v?signature=redirect'REDIRECT_TAIL&token=REDIRECT_TOKEN"
        for url in (source, redirect):
            self.assertEqual(player.playback_input(url), url)
        error = player.input_error(source, "Failed: " + source + " redirected " + redirect)
        self.assertEqual(error, "Failed: <remote URL> redirected <remote URL>")

    def test_remote_error_redacts_urls_inside_quoted_diagnostics(self):
        source = "https://origin.test/video"
        redirect = "https://user:password@cdn.test/v?signature=prefix'PRIVATE_TAIL"
        for quote in ("'", '"'):
            with self.subTest(quote=quote):
                error = player.input_error(
                    source, "Fetch failed for " + quote + redirect + quote + "; retry disabled")
                self.assertIn("Fetch failed for", error)
                self.assertIn("<remote URL>", error)
                self.assertIn("retry disabled", error)
                for secret in ("password", "cdn.test", "signature", "PRIVATE_TAIL"):
                    self.assertNotIn(secret, error)

    def test_local_error_preserves_path_but_redacts_remote_url(self):
        source = Path("/tmp/video's name.mp4")
        message = "Could not open " + str(source) + "; see https://example.test/help?reason='missing'"
        self.assertEqual(player.input_error(source, message),
                         "Could not open " + str(source) + "; see <remote URL>")


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


class DeinterlaceQosTests(unittest.TestCase):
    def fixture(self):
        Gst = SimpleNamespace(version=lambda: (1, 28, 2, 0),
            PadProbeType=SimpleNamespace(EVENT_UPSTREAM=1),
            PadProbeReturn=SimpleNamespace(OK=0, DROP=1), EventType=SimpleNamespace(QOS=1))
        element = mock.Mock()
        factory = element.get_factory.return_value
        factory.get_name.return_value = "deinterlace"
        factory.get_plugin.return_value.get_version.return_value = "1.28.2"
        properties = dict(mode=0, method=4, fields=0, locking=0, tff=0)
        element.get_property.side_effect = properties.__getitem__
        element.get_properties.side_effect = lambda *names: tuple(properties[name] for name in names)
        pads = {name: mock.Mock() for name in ("sink", "src")}
        pads["src"].add_probe.return_value = 17
        element.get_static_pad.side_effect = pads.__getitem__
        return Gst, element, properties, pads

    def test_version_and_properties_must_match(self):
        Gst, element, properties, _pads = self.fixture()
        self.assertTrue(player.DeinterlaceQosWorkaround.supported(Gst, element))
        for key in properties:
            with self.subTest(property=key):
                original = properties[key]
                properties[key] = original + 1
                self.assertIsNone(player.DeinterlaceQosWorkaround.install(Gst, element, mock.Mock()))
                properties[key] = original
        for version in ((1, 28, 1, 0), (1, 28, 3, 0), (1, 30, 0, 0)):
            Gst.version = lambda: version
            self.assertIsNone(player.DeinterlaceQosWorkaround.install(Gst, element, mock.Mock()))
        Gst.version = lambda: (1, 28, 2, 0)
        element.get_factory.return_value.get_plugin.return_value.get_version.return_value = "1.28.3"
        self.assertIsNone(player.DeinterlaceQosWorkaround.install(Gst, element, mock.Mock()))

    def test_qos_reads_one_fresh_property_batch_and_latches_each_change(self):
        names = ("mode", "method", "fields", "locking", "tff")
        for name in names:
            with self.subTest(property=name):
                Gst, element, properties, pads = self.fixture()
                helper = player.DeinterlaceQosWorkaround.install(Gst, element, mock.Mock())
                self.addCleanup(helper.close)
                for pad in pads.values():
                    pad.get_current_caps.return_value.get_size.return_value = 1
                helper.caps_supported = mock.Mock(return_value=True)
                info = mock.Mock()
                info.get_event.return_value.type = Gst.EventType.QOS
                element.get_properties.reset_mock()
                for _index in range(2):
                    self.assertEqual(helper.event(None, info), Gst.PadProbeReturn.DROP)
                self.assertEqual(element.get_properties.call_args_list, [mock.call(*names)] * 2)
                element.get_property.assert_not_called()
                self.assertEqual(pads["sink"].push_event.call_count, 2)

                original = properties[name]
                properties[name] = original + 1
                element.get_properties.reset_mock()
                self.assertEqual(helper.event(None, info), Gst.PadProbeReturn.OK)
                element.get_properties.assert_called_once_with(*names)
                self.assertFalse(helper.active)
                properties[name] = original
                self.assertEqual(helper.event(None, info), Gst.PadProbeReturn.OK)
                self.assertEqual(element.get_properties.call_count, 1)
                self.assertEqual(pads["sink"].push_event.call_count, 2)

    def test_forward_once_failure_and_probe_cleanup(self):
        for forwarded in (True, False):
            with self.subTest(forwarded=forwarded):
                Gst, element, _properties, pads = self.fixture()
                errors = mock.Mock()
                workaround = player.DeinterlaceQosWorkaround.install(Gst, element, errors)
                workaround.eligible = mock.Mock(return_value=True)
                info = mock.Mock()
                event = info.get_event.return_value
                event.type = Gst.EventType.QOS
                pads["sink"].push_event.return_value = forwarded
                self.assertEqual(workaround.event(None, info), Gst.PadProbeReturn.DROP)
                event.copy.assert_called_once_with()
                pads["sink"].push_event.assert_called_once_with(event.copy.return_value)
                self.assertEqual(errors.call_count, int(not forwarded))
                workaround.eligible.return_value = False
                self.assertEqual(workaround.event(None, info), Gst.PadProbeReturn.OK)
                self.assertFalse(workaround.active)
                event.type = 99
                workaround.eligible.return_value = True
                self.assertEqual(workaround.event(None, info), Gst.PadProbeReturn.OK)
                self.assertEqual(pads["sink"].push_event.call_count, 1)
                workaround.close()
                workaround.close()
                pads["src"].remove_probe.assert_called_once_with(17)
                self.assertTrue(workaround.closed)
                self.assertIsNone(workaround.on_error)
                self.assertIsNone(workaround.caps_policy)

    def test_ineligible_qos_latches_stock_policy(self):
        Gst, element, _properties, pads = self.fixture()
        workaround = player.DeinterlaceQosWorkaround.install(Gst, element, mock.Mock())
        info = mock.Mock()
        info.get_event.return_value.type = Gst.EventType.QOS
        pads["sink"].get_current_caps.return_value = None
        pads["src"].get_current_caps.return_value = None
        self.assertEqual(workaround.event(None, info), Gst.PadProbeReturn.OK)
        self.assertFalse(workaround.active)
        pads["sink"].get_current_caps.reset_mock()
        self.assertFalse(workaround.eligible())
        pads["sink"].get_current_caps.assert_not_called()
        pads["sink"].push_event.assert_not_called()
        workaround.close()

    def test_player_installs_only_on_card_scaled_hardware_path_and_removes_probe(self):
        for changes, install in (({}, True), ({"mode": "software"}, False),
                                 ({"scale_width": 0}, False),
                                 ({"software_output_size": None}, True), ({"weave": True}, False)):
            with self.subTest(changes=changes):
                control = player.Player.__new__(player.Player)
                control.args = SimpleNamespace(**(dict(mode="hardware", scale_width=1280,
                    software_output_size=(1920, 1080), weave=False) | changes))
                control.Gst = object()
                control.bus = object()
                control.deinterlace_qos, control.decoders = {}, {}
                element = mock.Mock()
                element.get_factory.return_value = Factory("deinterlace", "Filter/Effect/Video")
                helper = mock.Mock()
                with mock.patch.object(player.NativeDeinterlaceQosWorkaround, "install", return_value=None) as native, \
                        mock.patch.object(player.DeinterlaceQosWorkaround, "install", return_value=helper) as make:
                    control.element_setup(None, element)
                    control.element_setup(None, element)
                self.assertEqual(make.call_count, int(install))
                self.assertEqual(native.call_count, int(install))
                control.element_removed(None, None, element)
                self.assertEqual(helper.close.call_count, int(install))
                self.assertEqual(control.deinterlace_qos, {})


class NativeDeinterlaceQosTests(unittest.TestCase):
    def test_missing_or_unsupported_helper_keeps_python_fallback(self):
        Gst, element, bus = mock.Mock(), mock.Mock(), mock.Mock()
        Gst.ElementFactory.make.return_value = None
        self.assertIsNone(player.NativeDeinterlaceQosWorkaround.install(Gst, element, bus))
        controller = mock.Mock()
        controller.emit.return_value = 0
        Gst.ElementFactory.make.return_value = controller
        self.assertIsNone(player.NativeDeinterlaceQosWorkaround.install(Gst, element, bus))
        self.assertEqual(controller.emit.call_args_list,
                         [mock.call("attach", element, bus), mock.call("detach")])

    def test_native_properties_and_idempotent_close(self):
        Gst, element, bus, controller = (mock.Mock() for _ in range(4))
        Gst.ElementFactory.make.return_value = controller
        controller.emit.return_value = 1
        controller.get_property.return_value = True
        helper = player.NativeDeinterlaceQosWorkaround.install(Gst, element, bus)
        self.assertTrue(helper.active)
        self.assertTrue(helper.eligible())
        self.assertEqual(helper.probe, 1)
        helper.close()
        helper.close()
        self.assertTrue(helper.closed)
        self.assertFalse(helper.active)
        self.assertFalse(helper.eligible())
        self.assertEqual(helper.probe, 0)
        self.assertEqual(controller.emit.call_args_list,
                         [mock.call("attach", element, bus), mock.call("detach")])

    def test_failed_attach_is_not_silently_replaced_by_another_probe(self):
        for status in (-1, -2):
            with self.subTest(status=status):
                Gst, element, bus, controller = (mock.Mock() for _ in range(4))
                Gst.ElementFactory.make.return_value = controller
                controller.emit.return_value = status
                with self.assertRaisesRegex(RuntimeError, "invalid or already owned"):
                    player.NativeDeinterlaceQosWorkaround.install(Gst, element, bus)
                self.assertEqual(controller.emit.call_args_list[-1], mock.call("detach"))

    def test_player_prefers_native_and_reports_attachment_failure(self):
        for failed in (False, True):
            with self.subTest(failed=failed):
                control = player.Player.__new__(player.Player)
                control.args = SimpleNamespace(mode="hardware", scale_width=1280,
                    software_output_size=(1920, 1080), weave=False)
                control.Gst, control.bus = object(), object()
                control.deinterlace_qos, control.decoders = {}, {}
                control.fail = mock.Mock()
                element, helper = mock.Mock(), mock.Mock()
                element.get_factory.return_value = Factory("deinterlace", "Filter/Effect/Video")
                with mock.patch.object(player.NativeDeinterlaceQosWorkaround, "install",
                        side_effect=RuntimeError("already owned") if failed else None,
                        return_value=helper) as native, \
                        mock.patch.object(player.DeinterlaceQosWorkaround, "install") as fallback:
                    control.element_setup(None, element)
                    if not failed:
                        control.element_setup(None, element)
                    native.assert_called_once_with(control.Gst, element, control.bus)
                    fallback.assert_not_called()
                    self.assertEqual(control.fail.call_count, int(failed))
                control.element_removed(None, None, element)
                self.assertEqual(helper.close.call_count, int(not failed))
                self.assertEqual(control.deinterlace_qos, {})


class CleanupTests(unittest.TestCase):
    def test_teardown_failure_is_nonzero_and_cleanup_remains_idempotent(self):
        for failed, previous_code in ((False, 0), (True, 0), (True, 143)):
            with self.subTest(failed=failed, previous_code=previous_code):
                control = player.Player.__new__(player.Player)
                control.closed = False
                control.terminal_state = None
                control.sources = []
                helper = mock.Mock()
                control.deinterlace_qos = {object(): helper}
                control.exit_code = previous_code
                control.Gst = SimpleNamespace(State=SimpleNamespace(NULL=0),
                    StateChangeReturn=SimpleNamespace(FAILURE=-1))
                control.bus = mock.Mock()
                control.pipeline = mock.Mock()
                teardown_order = []
                control.pipeline.set_state.side_effect = lambda _state: (
                    teardown_order.append("null") or (-1 if failed else 1))
                helper.close.side_effect = lambda: teardown_order.append("probe")
                output = io.StringIO()
                with contextlib.redirect_stderr(output):
                    control.close()
                    control.close()
                self.assertEqual(control.exit_code, max(previous_code, int(failed)))
                control.pipeline.set_state.assert_called_once_with(0)
                control.bus.remove_signal_watch.assert_called_once_with()
                self.assertEqual("teardown failed" in output.getvalue(), failed)
                helper.close.assert_called_once_with()
                self.assertEqual(control.deinterlace_qos, {})
                self.assertEqual(teardown_order, ["null", "probe"])


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
            self.assertFalse(defaults.weave)
            self.assertEqual(defaults.scale_width, 0)
            self.assertIsNone(defaults.software_output_size)

    def test_weave_is_explicit_and_works_with_either_decoder_mode(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "interlaced video.ts"
            path.touch()
            for mode in ("hardware", "software"):
                with self.subTest(mode=mode):
                    args = player.arguments([f"--{mode}", "--weave", str(path)])
                    self.assertEqual(args.mode, mode)
                    self.assertTrue(args.weave)
            self.assertEqual(player.arguments(["--weave", str(path)]).mode, "hardware")

    def test_explicit_hardware_scale_and_software_output_size(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "video.mp4"
            path.touch()
            args = player.arguments([
                "--scale-width", "1280", "--software-output-size", "1920X1080", str(path)])
            self.assertEqual(args.scale_width, 1280)
            self.assertEqual(args.software_output_size, (1920, 1080))
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
                ["--software-output-size", "1920x1080", str(path)],
                ["--scale-width", "1280", "--software-output-size", "1921x1080", str(path)],
                ["--scale-width", "1280", "--software-output-size", "4096x4096", str(path)],
                ["--scale-width", "1280", "--software-output-size", "4098x1080", str(path)],
                ["--scale-width", "1280", "--software-output-size", "1920-by-1080", str(path)],
                ["--software", "--scale-width", "1280", str(path)],
            )
            for argv in invalid:
                with self.subTest(argv=argv), contextlib.redirect_stderr(io.StringIO()):
                    with self.assertRaises(SystemExit) as error:
                        player.arguments(argv)
                    self.assertEqual(error.exception.code, 2)

    def test_http_urls_are_preserved_without_touching_local_subtitles(self):
        urls = ("http://example.test/video.mp4", "https://example.test:8443/a%2fb?b=2&a=1&sig=A%2Bz",
                "HTTP://[::1]:8000/video.ogg", "https://user:password@example.test/a?token=secret",
                "https://example.test/v?signature=" + "a" * 8192)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "captions.srt"
            path.touch()
            for url in urls:
                with self.subTest(url=url):
                    args = player.arguments(["--subtitles", str(path), url])
                    self.assertEqual(args.file, url)
                    self.assertEqual(args.subtitles, path)
            colon = Path(directory) / "video:one.ogg"
            colon.touch()
            self.assertEqual(player.arguments([str(colon)]).file, colon)
            previous = Path.cwd()
            try:
                os.chdir(directory)
                self.assertEqual(player.arguments(["video:one.ogg"]).file, colon)
            finally:
                os.chdir(previous)

    def test_malformed_or_unsupported_urls_are_rejected_without_echoing_secrets(self):
        urls = ("ftp://user:secret@example.test/v", "file:///tmp/video.mp4",
                "https:///secret", "http:secret", "https://user:secret@/v",
                "https://example.test:secret/v", "https://example.test:65536/v",
                "https://example.test:0/secret", "https://[::1/secret",
                "https://example.test:/secret", "https://[::1]secret/video",
                "https://user@secret@example.test/v",
                "https://example.test/v?token=secret\n", "http://exam ple.test/secret",
                "https://example.test/secret%xx", "https://example.test\\secret",
                'https://example.test/"secret"', "https://example.test/secret\x00")
        for url in urls:
            output = io.StringIO()
            with self.subTest(url=url), contextlib.redirect_stderr(output):
                with self.assertRaises(SystemExit) as error:
                    player.arguments([url])
                self.assertEqual(error.exception.code, 2)
                self.assertNotIn("secret", output.getvalue())

    def test_cli_rejects_directories_missing_files_and_conflicting_modes(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "video.mp4"
            path.touch()
            for args in ([directory],
                         [str(path) + ".missing"],
                         ["--hardware", "--software", str(path)],
                         ["--subtitles", str(path) + ".missing", str(path)],
                         ["--subtitles", "https://example.test/subtitles.srt", str(path)]):
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


class ManagedDefaultsTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='crystalhd-player-defaults-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.path = self.root/'xdg/crystalhd/config.ini'
        self.media = self.root/'video.ogg'
        self.media.touch()
        self.environment = {'HOME': str(self.root), 'XDG_CONFIG_HOME': str(self.root/'xdg')}
        self.core = player.load_config_core()
        self.real_loader = player.load_config_core
        self.core.SYSTEM_CONFIG = None
        patch = mock.patch.object(player, 'load_config_core', return_value=self.core)
        patch.start();self.addCleanup(patch.stop)

    def write(self, text, mode=0o600):
        self.path.parent.mkdir(parents=True, mode=0o700, exist_ok=True)
        self.path.write_text('[crystalhd]\nschema=1\n'+text, encoding='utf-8')
        self.path.chmod(mode)

    def args(self, *flags):
        return player.arguments(list(flags)+[str(self.media)], environment=self.environment)

    def test_no_config_is_original_cli_behavior_without_writes_or_report(self):
        args = self.args()
        self.assertEqual((args.mode, args.scale_width, args.software_output_size), ('hardware', 0, None))
        self.assertIsNone(args.settings_report)
        self.assertFalse(self.path.parent.exists())

    def test_selected_profile_cli_precedence_native_resets_and_redacted_origins(self):
        self.write('profile="private-profile"\n[settings]\n'
                   'gstreamer.crystalhddec.scale_width=640\n[profile.private-profile]\n'
                   'gstreamer.crystalhddec.scale_width=1280\nplayer.software_output_size="1920x1080"\n')
        original = self.path.read_bytes()
        args = self.args()
        self.assertEqual((args.mode, args.scale_width, args.software_output_size), ('hardware', 1280, (1920,1080)))
        self.assertIn('user-profile:<redacted>', args.settings_report)
        self.assertNotIn('private-profile', args.settings_report)
        args = self.args('--scale-width', '960')
        self.assertEqual((args.scale_width, args.software_output_size), (960, (1920,1080)))
        args = self.args('--software-output-size', 'native')
        self.assertEqual((args.scale_width, args.software_output_size), (1280, None))
        args = self.args('--software', '--scale-width', '0', '--software-output-size', 'native')
        self.assertEqual((args.mode, args.scale_width, args.software_output_size), ('software', 0, None))
        self.assertEqual(self.path.read_bytes(), original)

    def test_incoherent_explicit_override_errors_instead_of_dropping_saved_values(self):
        self.write('[settings]\ngstreamer.crystalhddec.scale_width=1280\n'
                   'player.software_output_size="1920x1080"\n')
        original = self.path.read_bytes()
        for flags in (('--software',), ('--scale-width','0')):
            error = io.StringIO()
            with contextlib.redirect_stderr(error), self.assertRaises(SystemExit) as failure:
                self.args(*flags)
            self.assertEqual(failure.exception.code, 2)
            self.assertIn('--scale-width 0 --software-output-size native', error.getvalue())
            self.assertNotIn(str(self.path), error.getvalue())
        self.assertEqual(self.path.read_bytes(), original)

    def test_unset_in_profile_blocks_lower_values(self):
        self.write('profile="desk"\n[settings]\nplayer.decoder="software"\n'
                   '[profile.desk]\nplayer.decoder=unset\n')
        self.assertEqual(self.args().mode, 'hardware')
        self.assertEqual(self.args('--software').mode, 'software')

    def test_bad_unsafe_and_experimental_configuration_falls_back_as_one_overlay(self):
        for text, mode in [('[settings]\nplayer.decoder="software"\nbad=true\n',0o600),
                           ('[settings]\nplayer.decoder="software"\n',0o666),
                           ('preset="streaming"\n[settings]\nplayer.decoder="software"\n',0o600)]:
            self.write(text, mode)
            original = self.path.read_bytes()
            error = io.StringIO()
            with contextlib.redirect_stderr(error):
                args = self.args()
                explicit = self.args('--software')
            self.assertEqual(args.mode, 'hardware')
            self.assertEqual(explicit.mode, 'software')
            self.assertIn('managed defaults rejected', error.getvalue())
            self.assertNotIn(str(self.path), error.getvalue())
            self.assertEqual(self.path.read_bytes(), original)

    def test_bypass_differing_ids_and_missing_core_do_not_read_files(self):
        self.write('[settings]\nplayer.decoder="software"\n')
        with mock.patch.object(player, 'load_config_core') as loader:
            self.environment['CRYSTALHD_CONFIG_DISABLE']='1'
            self.assertEqual(self.args().mode, 'hardware')
            loader.assert_not_called()
            self.environment['CRYSTALHD_CONFIG_DISABLE']='0'
            with mock.patch.object(os, 'geteuid', return_value=os.getuid()+1), \
                    contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(self.args().mode, 'hardware')
            loader.assert_not_called()
            loader.side_effect = OSError('private-path')
            error = io.StringIO()
            with contextlib.redirect_stderr(error):
                self.assertEqual(self.args('--software').mode, 'software')
            self.assertNotIn('private-path', error.getvalue())

    def test_core_loader_rejects_writable_and_symlink_sources_before_execution(self):
        script = self.root/'player.py';script.touch()
        core = self.root/'crystalhd-config.py'
        core.write_text("raise AssertionError('must not execute')\n",encoding='utf-8')
        core.chmod(0o666)
        with mock.patch.object(player,'__file__',str(script)):
            with self.assertRaises(RuntimeError): self.real_loader()
            core.unlink();core.symlink_to(ROOT/'scripts/crystalhd-config.py')
            with self.assertRaises(RuntimeError): self.real_loader()

    def test_staged_separate_unicode_paths_bind_wrapper_controller_and_core(self):
        stage = self.root/'stage'
        bindir = "/opt/CrystalHD bin's Ω😀"
        datadir = '/opt/CrystalHD data 🧩/share'
        flags = ['DESTDIR='+str(stage), 'BINDIR='+bindir, 'DATADIR='+datadir]
        subprocess.run(['make','install-config']+flags, cwd=str(ROOT), check=True,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        subprocess.run(['sh','-c','umask 077; exec "$@"','stage-player',
                        'make','-C','filters/gst/gst-plugin-1.0','install-player']+flags,
                       cwd=str(ROOT), check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        wrapper = stage/bindir.lstrip('/')/'crystalhd-play'
        controller = stage/datadir.lstrip('/')/'crystalhd/player.py'
        self.assertEqual(wrapper.stat().st_mode & 0o777,0o755)
        self.assertEqual(controller.stat().st_mode & 0o777,0o644)
        sentinel=self.root/'sentinel';sentinel.write_text('untouched',encoding='utf-8')
        wrapper.unlink();wrapper.symlink_to(sentinel)
        subprocess.run(['make','-C','filters/gst/gst-plugin-1.0','install-player']+flags,
                       cwd=str(ROOT),check=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        self.assertFalse(wrapper.is_symlink())
        self.assertEqual(sentinel.read_text(encoding='utf-8'),'untouched')
        self.write('[settings]\nplayer.decoder="software"\n')
        program = ("import importlib.util,sys; s=importlib.util.spec_from_file_location('installed_player',sys.argv[1]); "
                   "p=importlib.util.module_from_spec(s); s.loader.exec_module(p); c=p.load_config_core(); "
                   "c.SYSTEM_CONFIG=None; p.load_config_core=lambda:c; "
                   "a=p.arguments([sys.argv[2]]); assert a.mode=='software'; assert a.settings_report")
        result = subprocess.run([sys.executable,'-B','-c',program,str(controller),str(self.media)],
                                env=dict(os.environ,**self.environment), stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, universal_newlines=True)
        self.assertEqual(result.returncode,0,result.stderr)
        result = subprocess.run(['sh',str(wrapper),'--help'],
                                env=dict(os.environ,CRYSTALHD_PLAYER_PYTHON='/bin/echo'),
                                stdout=subprocess.PIPE,stderr=subprocess.PIPE,universal_newlines=True)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn(str(controller),str(Path(result.stdout.strip().rsplit(' --help',1)[0]).resolve()))
        self.assertFalse(list(stage.rglob('__pycache__')))


class GStreamerApiTests(unittest.TestCase):
    def test_video_qos_drop_is_visible_once_without_changing_playback_policy(self):
        control = self.make_player()
        Gst = self.Gst
        source = Gst.ElementFactory.make("videoconvert")
        if source is None:
            self.skipTest("QoS fixture needs videoconvert")
        original_qos = source.get_property("qos")
        control.health.frame(1, "theoradec", SOFTWARE, True, 512, 1)
        before = vars(control.health).copy()
        messages = []
        player.connect_bus_messages(control.bus, lambda _bus, message: messages.append(message))
        message = Gst.Message.new_qos(source, False, 0, 0, 0, 20000000)
        message.set_qos_stats(Gst.Format.BUFFERS, 20, 2)
        self.assertTrue(control.bus.post(message))
        context = self.GLib.MainContext.default()
        error = io.StringIO()
        with contextlib.redirect_stderr(error):
            while context.pending():
                context.iteration(False)
            control.message(control.bus, message.copy())
        self.assertTrue(any(m.type == Gst.MessageType.QOS for m in messages))
        self.assertEqual(error.getvalue().count("video buffers were dropped"), 1)
        self.assertIn("not a displayed-frame count", error.getvalue())
        self.assertEqual(vars(control.health), before)
        self.assertFalse(control.done)
        self.assertEqual(source.get_property("qos"), original_qos)
        control.qos_warned = False
        control.args.software_output_size = (1920, 1080)
        error = io.StringIO()
        with contextlib.redirect_stderr(error):
            control.message(control.bus, message.copy())
        self.assertIn("omit --software-output-size", error.getvalue())

    def test_qos_without_a_known_video_buffer_drop_is_quiet(self):
        control = self.make_player()
        Gst = self.Gst
        video = Gst.ElementFactory.make("videoconvert")
        audio = Gst.ElementFactory.make("audioconvert")
        if video is None or audio is None:
            self.skipTest("QoS fixture needs audio/video converters")
        error = io.StringIO()
        with contextlib.redirect_stderr(error):
            for source, fmt, dropped in ((video, Gst.Format.BUFFERS, 0),
                                         (video, Gst.Format.BUFFERS, Gst.CLOCK_TIME_NONE),
                                         (video, Gst.Format.TIME, 2),
                                         (audio, Gst.Format.BUFFERS, 2)):
                message = Gst.Message.new_qos(source, False, 0, 0, 0, 20000000)
                message.set_qos_stats(fmt, 20, dropped)
                control.message(control.bus, message)
        self.assertEqual(error.getvalue(), "")

    def test_saved_software_decoder_reaches_real_frames_and_eos(self):
        self.http_fixture()
        core = player.load_config_core();core.SYSTEM_CONFIG=None
        home = Path(self.temp.name)/'managed'
        config = home/'.config/crystalhd/config.ini'
        config.parent.mkdir(parents=True,mode=0o700)
        config.write_text('[crystalhd]\nschema=1\n[settings]\nplayer.decoder="software"\n',encoding='utf-8')
        config.chmod(0o600)
        with mock.patch.object(player,'load_config_core',return_value=core):
            args = player.arguments([str(Path(self.temp.name)/'http-video.ogg')], environment={'HOME':str(home)})
        result = player.Player(args,self.Gst,self.GLib,{
            'video-sink':self.Gst.ElementFactory.make('fakesink'),
            'audio-sink':self.Gst.ElementFactory.make('fakesink')})
        self.addCleanup(result.close)
        code, _output, error = self.run_player(result)
        self.assertEqual(code,0,error)
        self.assertEqual(result.health.frames,12)
        self.assertEqual(args.mode,'software')

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

    def make_player(self, mode="software", weave=False, source=None):
        args = SimpleNamespace(mode=mode, weave=weave,
                               file=source or Path(self.temp.name) / "unused.ogg",
                               subtitles=Path(self.temp.name) / "sub titles.srt",
                               scale_width=0, software_output_size=None)
        result = player.Player(args, self.Gst, self.GLib, {
            "video-sink": self.Gst.ElementFactory.make("fakesink"),
            "audio-sink": self.Gst.ElementFactory.make("fakesink")})
        self.addCleanup(result.close)
        return result

    def http_fixture(self, *, indexed=False):
        muxer = "matroskamux" if indexed else "oggmux"
        for name in ("videotestsrc", "theoraenc", muxer, "filesink", "theoradec", "souphttpsrc"):
            if self.Gst.ElementFactory.find(name) is None:
                self.skipTest(f"optional HTTP software fixture needs {name}")
        path = Path(self.temp.name) / ("http-video.mkv" if indexed else "http-video.ogg")
        if not path.exists():
            pipeline = self.Gst.parse_launch(
                "videotestsrc num-buffers=12 ! "
                "video/x-raw,width=32,height=32,framerate=12/1 ! theoraenc ! " +
                muxer + " ! filesink name=out")
            pipeline.get_by_name("out").set_property("location", str(path))
            try:
                self.assertNotEqual(pipeline.set_state(self.Gst.State.PLAYING),
                                    self.Gst.StateChangeReturn.FAILURE)
                message = pipeline.get_bus().timed_pop_filtered(
                    5 * self.Gst.SECOND, self.Gst.MessageType.EOS | self.Gst.MessageType.ERROR)
                self.assertIsNotNone(message)
                self.assertEqual(message.type, self.Gst.MessageType.EOS)
            finally:
                pipeline.set_state(self.Gst.State.NULL)
        return path.read_bytes()

    @contextlib.contextmanager
    def http_server(self, data, *, missing=False, stalled=False, ranges=False):
        release = threading.Event()

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, _format, *_args):
                pass

            def do_GET(self):
                if missing:
                    self.send_error(404, "Not Found")
                    return
                start, end = 0, len(data) - 1
                requested = self.headers.get("Range") if ranges else None
                if requested:
                    match = re.fullmatch(r"bytes=([0-9]+)-([0-9]*)", requested)
                    if match is None or int(match.group(1)) >= len(data):
                        self.send_error(416, "Range Not Satisfiable")
                        return
                    start = int(match.group(1))
                    if match.group(2):
                        end = min(end, int(match.group(2)))
                self.send_response(206 if requested else 200)
                self.send_header("Content-Type", "application/octet-stream")
                if ranges:
                    self.send_header("Accept-Ranges", "bytes")
                    self.send_header("Content-Length", str(end - start + 1))
                    if requested:
                        self.send_header("Content-Range", f"bytes {start}-{end}/{len(data)}")
                # Without length/ranges, exercise a finite nonseekable source.
                self.end_headers()
                if stalled:
                    release.wait(2)
                    return
                try:
                    self.wfile.write(data[start:end + 1])
                except (BrokenPipeError, ConnectionResetError):
                    pass

        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever,
                                  kwargs={"poll_interval": 0.05}, daemon=True)
        thread.start()
        try:
            yield f"http://127.0.0.1:{server.server_port}/video.ogg?token=private-test-token"
        finally:
            release.set()
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)

    def run_player(self, result):
        output, error = io.StringIO(), io.StringIO()
        with mock.patch("sys.stdin", new=io.StringIO()), \
                contextlib.redirect_stdout(output), contextlib.redirect_stderr(error):
            code = result.run()
        return code, output.getvalue(), error.getvalue()

    def test_http_loopback_software_frames_eos_and_nonseekable_rate(self):
        data = self.http_fixture()
        with self.http_server(data) as url:
            result = self.make_player(source=url)
            result.pipeline.get_property("video-sink").set_property("sync", True)
            attempted = []

            def try_rate():
                if not result.health.frames:
                    return True
                state, _current, _pending = result.pipeline.get_state(0)
                if state == self.Gst.StateChangeReturn.ASYNC:
                    return True
                attempted.append(True)
                result.seek(rate=2.0)
                return False

            result.sources.append(self.GLib.timeout_add(50, try_rate))
            code, _output, error = self.run_player(result)
        self.assertEqual(code, 0)
        self.assertEqual(result.health.frames, 12)
        self.assertTrue(attempted)
        self.assertEqual(result.rate, 1.0)
        self.assertIn("not currently seekable", error)
        self.assertNotIn("private-test-token", error)

    def test_http_range_source_accepts_time_seek_and_reaches_eos(self):
        data = self.http_fixture(indexed=True)
        with self.http_server(data, ranges=True) as url:
            result = self.make_player(source=url)
            sink = result.pipeline.get_property("video-sink")
            sink.set_property("sync", True)
            accepted = []
            segments = []

            def observe_segment(_pad, info):
                event = info.get_event()
                if event.type == self.Gst.EventType.SEGMENT:
                    segment = event.parse_segment()
                    segments.append((segment.rate, segment.start))
                return self.Gst.PadProbeReturn.OK

            sink.get_static_pad("sink").add_probe(
                self.Gst.PadProbeType.EVENT_DOWNSTREAM, observe_segment)

            def try_seek():
                if not result.health.frames:
                    return True
                state, _current, _pending = result.pipeline.get_state(0)
                if state == self.Gst.StateChangeReturn.ASYNC:
                    return True
                ok, _position = result.pipeline.query_position(self.Gst.Format.TIME)
                if not ok:
                    return True
                result.submit_seek(self.Gst.SECOND // 2, 2.0)
                accepted.append(result.seek_target is not None)
                return False

            result.sources.append(self.GLib.timeout_add(50, try_seek))
            code, _output, error = self.run_player(result)
        self.assertEqual(code, 0, error)
        self.assertEqual(accepted, [True], error)
        self.assertEqual(result.rate, 2.0)
        # Confirm the requested rate/position reached the sink, not just that
        # the controller updated its own bookkeeping after seek returned true.
        self.assertIn((2.0, self.Gst.SECOND // 2), segments)
        self.assertIsNone(result.seek_target)
        self.assertGreater(result.health.frames, 0)

    def test_http_404_fails_without_frames_or_query_disclosure(self):
        data = self.http_fixture()
        with self.http_server(data, missing=True) as url:
            result = self.make_player(source=url)
            code, output, error = self.run_player(result)
        self.assertEqual(code, 1)
        self.assertEqual(result.health.frames, 0)
        self.assertIsNotNone(result.health.error)
        self.assertNotIn("private-test-token", output + error)
        self.assertNotIn(url, output + error)

    def test_http_unfinished_buffering_keeps_startup_timeout(self):
        data = self.http_fixture()
        with self.http_server(data, stalled=True) as url:
            result = self.make_player(source=url)
            with mock.patch.object(player.PlaybackHealth, "START_TIMEOUT", 0.2):
                code, _output, error = self.run_player(result)
        self.assertEqual(code, 1)
        self.assertEqual(result.health.frames, 0)
        self.assertIn("no decoded video output", error)
        self.assertNotIn("private-test-token", error)

    def run_pipeline(self, description):
        result = self.make_player()
        result.bus.remove_signal_watch()
        result.pipeline = self.Gst.parse_launch(description)
        result.bus = result.pipeline.get_bus()
        result.bus.add_signal_watch()
        player.connect_bus_messages(result.bus, result.message)
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

    def test_weave_changes_only_deinterlacing_and_preserves_decoder_policy(self):
        reference = self.Gst.ElementFactory.make("playbin")
        original_flags = int(reference.get_property("flags"))
        for mode in ("hardware", "software"):
            for weave in (False, True):
                with self.subTest(mode=mode, weave=weave):
                    factories = [Factory("crystalhddec", HARDWARE),
                                 Factory("avdec_h264", SOFTWARE),
                                 Factory("vaapih264dec", HARDWARE),
                                 Factory("faad", "Codec/Decoder/Audio")]
                    registry = SimpleNamespace(get_feature_list=lambda _type: factories)
                    output = io.StringIO()
                    # Real playbin/property API, mock factories: never enable or open a card.
                    with mock.patch.object(self.Gst.Registry, "get", return_value=registry), \
                            contextlib.redirect_stdout(output):
                        result = self.make_player(mode, weave)
                    expected_flags = original_flags | ((1 << 12) if mode == "software" else 0)
                    if weave:
                        expected_flags &= ~(1 << 9)
                    self.assertEqual(int(result.pipeline.get_property("flags")), expected_flags)
                    self.assertEqual([factory.rank for factory in factories],
                                     [356, 0, 0, 256] if mode == "hardware"
                                     else [0, 256, 0, 256])
                    self.assertTrue(result.pipeline.get_property("suburi").endswith("sub%20titles.srt"))
                    self.assertIsNone(result.pipeline.get_property("video-filter"))
                    if weave:
                        decoder = "CrystalHD hardware" if mode == "hardware" else "software"
                        self.assertIn(f"deinterlacing off; {decoder} decoding required", output.getvalue())
                    else:
                        self.assertEqual(output.getvalue(), "")
                    wrong_name, wrong_klass = ("avdec_h264", SOFTWARE) if mode == "hardware" \
                        else ("crystalhddec", HARDWARE)
                    self.assertFalse(result.health.frame(1, wrong_name, wrong_klass, True, 512, 1))
                    self.assertEqual(result.health.finish(), 1)

    def test_extended_bus_message_is_not_delivered_to_player_callback(self):
        pipeline = self.Gst.Pipeline.new("extended-message-test")
        bus = pipeline.get_bus()
        bus.add_signal_watch()
        self.addCleanup(bus.remove_signal_watch)
        handled = []
        player.connect_bus_messages(bus, lambda _bus, _message: handled.append(True))

        # PROPERTY_NOTIFY is in the GST_MESSAGE_EXTENDED (bit 31) family that
        # PyGObject cannot expose through message.type as a C long on i386.
        self.assertTrue(bus.post(
            self.Gst.Message.new_property_notify(pipeline, "name", None)))
        context = self.GLib.MainContext.default()
        while context.pending():
            context.iteration(False)
        self.assertEqual(handled, [])

        # Prove that the signal watch was dispatched rather than merely idle.
        structure = self.Gst.Structure.new_empty("test-application-message")
        self.assertTrue(bus.post(self.Gst.Message.new_application(pipeline, structure)))
        while context.pending():
            context.iteration(False)
        self.assertEqual(handled, [True])

    def test_real_warning_bus_is_visible_nonfatal_and_excludes_extended_types(self):
        control = self.make_player()
        control.stop = mock.Mock()
        before = vars(control.health).copy()
        exit_code = control.exit_code
        warning = self.GLib.Error.new_literal(self.Gst.StreamError.quark(),
            "Skipping undecodable MPEG-2 input frame 414: missing coded rows",
            int(self.Gst.StreamError.DECODE))
        self.assertTrue(control.bus.post(self.Gst.Message.new_property_notify(
            control.pipeline, "name", None)))
        self.assertTrue(control.bus.post(self.Gst.Message.new_warning(
            control.pipeline, warning, "/private/build/source.c:123: private debug")))
        output = io.StringIO()
        context = self.GLib.MainContext.default()
        with contextlib.redirect_stderr(output):
            while context.pending():
                context.iteration(False)
        lines = [line for line in output.getvalue().splitlines()
                 if line.startswith("crystalhd-play:")]
        self.assertEqual(lines, [
            "crystalhd-play: warning: Skipping undecodable MPEG-2 input frame 414: missing coded rows"])
        self.assertNotIn("/private/build/source.c", output.getvalue())
        self.assertNotIn("private debug", output.getvalue())
        self.assertEqual(vars(control.health), before)
        self.assertEqual(control.exit_code, exit_code)
        self.assertFalse(control.done)
        control.stop.assert_not_called()

    def scale_caps_player(self, width, mode, h264=False):
        result = self.make_player()
        result.args.scale_width = width
        sink = self.Gst.Pad.new("sink", self.Gst.PadDirection.SINK)
        sink.set_event_function_full(lambda _pad, _parent, _event: True)
        self.assertTrue(sink.set_active(True))
        self.addCleanup(sink.set_active, False)
        # Mock only the decoder factory/property boundary. Actual caps events
        # cross its real sink pad; no decoder implementation or card is used.
        decoder = mock.Mock()
        decoder.get_factory.return_value = Factory("crystalhddec", HARDWARE)
        decoder.find_property.return_value = object()
        decoder.get_static_pad.side_effect = lambda name: sink if name == "sink" else None
        result.element_setup(None, decoder)
        caps = self.Gst.Caps.from_string(
            ("video/x-h264,stream-format=byte-stream,alignment=au,parsed=true,"
             "width=1920,height=1080,framerate=25/1") if h264 else
            ("video/mpeg,mpegversion=2,systemstream=false,parsed=true,"
             "width=1920,height=1088,framerate=25/1"))
        if mode is not None:
            caps.set_value("interlace-mode", mode)
        self.assertTrue(sink.send_event(self.Gst.Event.new_caps(caps)))
        return result

    def test_interlaced_scale_rejection_survives_audio_only_eos(self):
        result = self.scale_caps_player(1440, "mixed")
        # playbin can discard a failed video branch while audio reaches EOS.
        result.post_observation("crystalhd-player-removed", {"token": 1})
        self.assertTrue(result.bus.post(self.Gst.Message.new_eos(result.pipeline)))
        with contextlib.redirect_stderr(io.StringIO()):
            while True:
                message = result.bus.timed_pop_filtered(0,
                    self.Gst.MessageType.APPLICATION | self.Gst.MessageType.EOS)
                if message is None:
                    break
                result.message(result.bus, message)
        self.assertEqual(result.exit_code, 1)
        self.assertIn("interlaced --scale-width requires BCM70015 H.264", result.health.error)
        self.assertIn("mixed", result.health.error)
        self.assertIn("omit --scale-width", result.health.error)
        self.assertEqual(result.health.finish(), 1)
        self.assertNotIn("no output", result.health.error)

    def test_scale_caps_probe_keeps_progressive_and_native_input(self):
        for width, mode in ((1440, "progressive"), (0, "mixed"),
                            (1440, None)):
            with self.subTest(width=width, mode=mode):
                result = self.scale_caps_player(width, mode)
                self.assertIsNone(result.bus.timed_pop_filtered(
                    0, self.Gst.MessageType.APPLICATION))
                self.assertIsNone(result.health.error)

    def test_scale_caps_probe_allows_h264_field_scaling(self):
        for width, mode in ((1280, "interleaved"), (1280, "mixed"),
                            (960, "interleaved")):
            with self.subTest(width=width, mode=mode):
                result = self.scale_caps_player(width, mode, h264=True)
                self.assertIsNone(result.bus.timed_pop_filtered(
                    0, self.Gst.MessageType.APPLICATION))
                self.assertIsNone(result.health.error)

    def test_scale_caps_probe_rejects_unqualified_h264_field_layout(self):
        for width, mode in ((1280, "alternate"), (130, "interleaved")):
            with self.subTest(width=width, mode=mode):
                result = self.scale_caps_player(width, mode, h264=True)
                message = result.bus.timed_pop_filtered(
                    0, self.Gst.MessageType.APPLICATION)
                self.assertIsNotNone(message)
                result.message(result.bus, message)
                self.assertEqual(result.exit_code, 1)
                self.assertIn("multiple-of-16 width", result.health.error)

    def test_explicit_upscale_filter_has_expected_caps_and_quality_settings(self):
        for name in ("videoscale", "capsfilter"):
            if self.Gst.ElementFactory.find(name) is None:
                self.skipTest(f"optional upscale fixture needs {name}")
        result = player.make_software_output_filter(self.Gst, (1920, 1080))
        scale = result.get_by_name("crystalhd-upscale")
        capsfilter = result.get_by_name("crystalhd-upscale-caps")
        self.assertEqual(scale.get_property("method"), 4)
        if scale.find_property("add-borders") is not None:
            self.assertTrue(scale.get_property("add-borders"))
        if scale.find_property("n-threads") is not None:
            self.assertEqual(scale.get_property("n-threads"), 2)
        caps = capsfilter.get_property("caps")
        structure = caps.get_structure(0)
        self.assertEqual(structure.get_name(), "video/x-raw")
        self.assertEqual(structure.get_string("format"), "YUY2")
        self.assertEqual(structure.get_value("width"), 1920)
        self.assertEqual(structure.get_value("height"), 1080)
        self.assertEqual(structure.get_fraction("pixel-aspect-ratio"),
                         (True, 1, 1))
        self.assertIsNotNone(result.get_static_pad("sink"))
        self.assertIsNotNone(result.get_static_pad("src"))
        self.assertIsNone(result.get_by_name("crystalhd-early-pack"))

    def packing_fixture(self):
        for name in ("videoscale", "videoconvert", "capsfilter", "playsink"):
            if self.Gst.ElementFactory.find(name) is None:
                self.skipTest(f"optional packing fixture needs {name}")
        Gst = self.Gst
        pipeline = Gst.Pipeline.new(None)
        playsink = Gst.ElementFactory.make("playsink")
        chain = Gst.Bin.new(None)
        video_filter = player.make_software_output_filter(
            Gst, (1920, 1080), prepare_native_packing=True)
        pipeline.add(playsink)
        playsink.add(chain)
        chain.add(video_filter)
        self.addCleanup(pipeline.set_state, Gst.State.NULL)
        # Real bin ownership/caps, with a display-free stand-in for the READY sink.
        view = mock.Mock(wraps=video_filter)
        helper = player.XvOutputPacking(Gst, view)
        sink = mock.Mock()
        sink.get_factory.return_value = Factory("xvimagesink", "Sink/Video")
        sink.has_as_ancestor.side_effect = lambda ancestor: ancestor is chain
        sink.get_state.return_value = (Gst.StateChangeReturn.SUCCESS, Gst.State.READY,
                                       Gst.State.VOID_PENDING)
        sink.get_static_pad.return_value.query_caps.return_value = Gst.Caps.from_string(
            "video/x-raw,format=UYVY,width=1920,height=1080,pixel-aspect-ratio=1/1")
        return SimpleNamespace(Gst=Gst, pipeline=pipeline, playsink=playsink, chain=chain,
                               video_filter=video_filter, view=view, helper=helper, sink=sink)

    def test_native_packing_selects_once_without_changing_upstream_yuy2(self):
        fixture = self.packing_fixture()
        Gst, helper, sink = fixture.Gst, fixture.helper, fixture.sink
        self.assertTrue(helper.select(sink, fixture.pipeline))
        self.assertTrue(helper.selected)
        self.assertEqual(helper.output.get_property("caps").get_structure(0).get_string("format"), "UYVY")
        inlet = fixture.video_filter.get_by_name("crystalhd-early-yuy2")
        self.assertEqual(inlet.get_property("caps").get_structure(0).get_string("format"), "YUY2")
        self.assertTrue(fixture.video_filter.get_static_pad("sink").query_caps(None).is_subset(
            Gst.Caps.from_string("video/x-raw,format=YUY2")))
        self.assertEqual(fixture.video_filter.get_by_name("crystalhd-early-pack").get_property("n-threads"), 1)
        sink.get_state.reset_mock()
        sink.get_static_pad.reset_mock()
        self.assertFalse(helper.select(sink, fixture.pipeline))
        sink.get_state.assert_not_called()
        sink.get_static_pad.assert_not_called()

    def test_native_packing_requires_real_exclusive_native_caps(self):
        for text in (None, "ANY", "EMPTY", "video/x-raw,format=YUY2",
                     "video/x-raw,format={YUY2,UYVY}", "video/x-raw,format=NV12",
                     "video/x-raw,format=UYVY,width=640,height=360",
                     "video/x-raw(memory:DMABuf),format=UYVY"):
            with self.subTest(caps=text):
                fixture = self.packing_fixture()
                fixture.sink.get_static_pad.return_value.query_caps.return_value = (
                    None if text is None else fixture.Gst.Caps.from_string(text))
                self.assertFalse(fixture.helper.select(fixture.sink, fixture.pipeline))
                self.assertFalse(fixture.helper.selected)
                self.assertEqual(fixture.helper.output.get_property("caps").get_structure(0).get_string("format"), "YUY2")

    def test_native_packing_rejects_trial_late_and_uncertain_sinks(self):
        for condition in ("factory", "unparented", "other-pipeline", "no-chain",
                          "sink-null", "sink-pending", "sink-failed", "filter-ready",
                          "filter-pending", "negotiated", "no-pad", "query-failure"):
            with self.subTest(condition=condition):
                fixture = self.packing_fixture()
                Gst, helper, sink = fixture.Gst, fixture.helper, fixture.sink
                pipeline = fixture.pipeline
                if condition == "factory":
                    sink.get_factory.return_value = Factory("fakesink", "Sink/Video")
                elif condition == "unparented":
                    sink.has_as_ancestor.side_effect = lambda _ancestor: False
                elif condition == "other-pipeline":
                    pipeline = Gst.Pipeline.new(None)
                elif condition == "no-chain":
                    fixture.view.get_parent.return_value = None
                elif condition.startswith("sink-"):
                    sink.get_state.return_value = {
                        "sink-null": (Gst.StateChangeReturn.SUCCESS, Gst.State.NULL, Gst.State.VOID_PENDING),
                        "sink-pending": (Gst.StateChangeReturn.ASYNC, Gst.State.READY, Gst.State.PAUSED),
                        "sink-failed": (Gst.StateChangeReturn.FAILURE, Gst.State.READY, Gst.State.VOID_PENDING),
                    }[condition]
                elif condition.startswith("filter-"):
                    fixture.view.get_state.return_value = (
                        Gst.StateChangeReturn.SUCCESS,
                        Gst.State.READY if condition == "filter-ready" else Gst.State.NULL,
                        Gst.State.VOID_PENDING if condition == "filter-ready" else Gst.State.READY)
                elif condition == "negotiated":
                    fixture.view.get_static_pad.return_value = mock.Mock()
                    fixture.view.get_static_pad.return_value.get_current_caps.return_value = Gst.Caps.from_string("video/x-raw")
                elif condition == "no-pad":
                    sink.get_static_pad.return_value = None
                elif condition == "query-failure":
                    sink.get_static_pad.return_value.query_caps.side_effect = RuntimeError("unavailable")
                self.assertFalse(helper.select(sink, pipeline))
                self.assertFalse(helper.selected)
                self.assertEqual(helper.output.get_property("caps").get_structure(0).get_string("format"), "YUY2")

    def test_player_prepares_packing_only_for_default_hardware_upscale(self):
        for changes, custom, expected in (({}, False, True), ({"mode": "software"}, False, False),
                ({"weave": True}, False, False), ({"scale_width": 0}, False, False),
                ({"software_output_size": None}, False, False),
                ({"software_output_size": (1280, 720)}, False, False),
                ({"software_output_size": (640, 360)}, False, False), ({}, True, False)):
            with self.subTest(changes=changes, custom=custom):
                args = SimpleNamespace(**(dict(mode="hardware", weave=False, scale_width=1280,
                    software_output_size=(1920, 1080), file=Path(self.temp.name)/"unused.mkv",
                    subtitles=None) | changes))
                sinks = {"video-sink": self.Gst.ElementFactory.make("fakesink")} if custom else None
                with mock.patch.object(player, "configure_decoders"):
                    result = player.Player(args, self.Gst, self.GLib, sinks)
                try:
                    self.assertEqual(result.output_packing is not None, expected)
                    if result.video_filter is not None:
                        self.assertEqual(result.video_filter.get_by_name("crystalhd-early-pack") is not None, expected)
                    if expected:
                        element = mock.Mock()
                        element.get_factory.return_value = Factory("xvimagesink", "Sink/Video")
                        with mock.patch.object(result.output_packing, "select") as select:
                            result.element_setup(result.pipeline, element)
                            select.assert_called_once_with(element, result.pipeline)
                            result.closed = True
                            result.element_setup(result.pipeline, element)
                            self.assertEqual(select.call_count, 1)
                            result.closed = False
                finally:
                    result.close()

    def test_prepared_packing_preserves_resized_pixels_and_metadata(self):
        for name in ("videotestsrc", "videoscale", "videoconvert", "capsfilter", "appsink"):
            if self.Gst.ElementFactory.find(name) is None:
                self.skipTest(f"optional packing pixels need {name}")
        Gst = self.Gst
        results = []
        for early in (False, True):
            pipeline = Gst.parse_launch(
                "videotestsrc num-buffers=3 pattern=smpte name=input ! "
                "video/x-raw,format=YUY2,width=32,height=18,framerate=50/1,"
                "pixel-aspect-ratio=1/1,colorimetry=bt709 ! identity name=entry")
            upscale = player.make_software_output_filter(Gst, (48, 32), prepare_native_packing=early)
            if early:
                capsfilter = upscale.get_by_name("crystalhd-upscale-caps")
                caps = capsfilter.get_property("caps").copy()
                caps.set_value("format", "UYVY")
                capsfilter.set_property("caps", caps)
            tail = Gst.parse_bin_from_description(
                "videoconvert ! video/x-raw,format=UYVY ! appsink name=output sync=false", True)
            pipeline.add(upscale)
            pipeline.add(tail)
            self.assertTrue(pipeline.get_by_name("entry").link(upscale))
            self.assertTrue(upscale.link(tail))
            sink = tail.get_by_name("output")
            rows = []
            try:
                self.assertNotEqual(pipeline.set_state(Gst.State.PLAYING), Gst.StateChangeReturn.FAILURE)
                for _index in range(3):
                    sample = sink.emit("try-pull-sample", 5*Gst.SECOND)
                    self.assertIsNotNone(sample)
                    buffer = sample.get_buffer()
                    rows.append((sample.get_caps().to_string(), buffer.pts, buffer.dts,
                        buffer.duration, buffer.offset, buffer.offset_end, int(buffer.get_flags()),
                        buffer.extract_dup(0, buffer.get_size())))
                message = pipeline.get_bus().timed_pop_filtered(5*Gst.SECOND,
                    Gst.MessageType.EOS | Gst.MessageType.ERROR)
                self.assertIsNotNone(message)
                self.assertEqual(message.type, Gst.MessageType.EOS)
            finally:
                pipeline.set_state(Gst.State.NULL)
            results.append(rows)
        self.assertEqual(results[0], results[1])

    def test_explicit_upscale_filter_processes_raw_video(self):
        for name in ("videotestsrc", "videoscale", "capsfilter", "fakesink"):
            if self.Gst.ElementFactory.find(name) is None:
                self.skipTest(f"optional upscale fixture needs {name}")
        pipeline = self.Gst.Pipeline.new("upscale-test")
        source = self.Gst.ElementFactory.make("videotestsrc")
        source_caps = self.Gst.ElementFactory.make("capsfilter")
        source_caps.set_property(
            "caps", self.Gst.Caps.from_string(
                "video/x-raw,width=320,height=240,pixel-aspect-ratio=1/1"))
        upscale = player.make_software_output_filter(self.Gst, (640, 360))
        sink = self.Gst.ElementFactory.make("fakesink")
        source.set_property("num-buffers", 3)
        pipeline.add(source)
        pipeline.add(source_caps)
        pipeline.add(upscale)
        pipeline.add(sink)
        self.assertTrue(source.link(source_caps))
        self.assertTrue(source_caps.link(upscale))
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
        self.assertEqual(caps.get_fraction("pixel-aspect-ratio"), (True, 1, 1))

    def test_native_deinterlace_qos_action_signal_binding_and_close(self):
        Gst = self.Gst
        if Gst.ElementFactory.find("crystalhddiqos") is None:
            self.skipTest("optional native controller requires the built own plugin")
        element = Gst.ElementFactory.make("deinterlace")
        if element is None or not player.DeinterlaceQosWorkaround.supported(Gst, element):
            self.skipTest("exact GStreamer 1.28.2 deinterlace admission")
        bus = Gst.Bus.new()
        helper = player.NativeDeinterlaceQosWorkaround.install(Gst, element, bus)
        self.assertIsNotNone(helper)
        self.addCleanup(helper.close)
        self.assertTrue(helper.active)
        self.assertTrue(helper.probe)
        self.assertFalse(helper.eligible())  # No negotiated caps yet.
        with self.assertRaisesRegex(RuntimeError, "already owned"):
            player.NativeDeinterlaceQosWorkaround.install(Gst, element, bus)
        helper.close()
        helper.close()
        self.assertFalse(helper.probe)
        self.assertFalse(helper.active)
        self.assertFalse(helper.eligible())
        replacement = player.NativeDeinterlaceQosWorkaround.install(Gst, element, bus)
        self.assertIsNotNone(replacement)
        replacement.close()

    def test_deinterlace_qos_caps_are_narrow_and_rechecked(self):
        Gst = self.Gst
        element = Gst.ElementFactory.make("deinterlace")
        if element is None or not player.DeinterlaceQosWorkaround.supported(Gst, element):
            self.skipTest("exact GStreamer 1.28.2 deinterlace caps policy")
        # Exercise actual Gst.Caps parsing without starting another pipeline.
        helper = player.DeinterlaceQosWorkaround.__new__(player.DeinterlaceQosWorkaround)
        helper.Gst, helper.element = Gst, element
        helper.closed, helper.active = False, True
        helper.caps_policy = None
        helper.sink, helper.src = mock.Mock(), mock.Mock()
        source = "video/x-raw,format=YUY2,width=16,height=8,interlace-mode=mixed,framerate=25/1"
        output = source.replace("mixed", "progressive").replace("25/1", "50/1")
        for changed_source, changed_output, expected in (
                (source, output, True), (source.replace("mixed", "interleaved"), output, True),
                (source.replace("mixed", "progressive"), output, False),
                (source.replace("mixed", "alternate"), output, False),
                (source.replace("YUY2", "NV12"), output, False),
                (source, output.replace("YUY2", "NV12"), False),
                (source.replace("25/1", "30/1"), output, False),
                (source, output.replace("50/1", "25/1"), False),
                (source, output.replace("width=16", "width=32"), False),
                (source, output.replace("height=8", "height=16"), False)):
            with self.subTest(source=changed_source, output=changed_output):
                helper.sink.get_current_caps.return_value = Gst.Caps.from_string(changed_source)
                helper.src.get_current_caps.return_value = Gst.Caps.from_string(changed_output)
                self.assertEqual(helper.eligible(), expected)
        helper.sink.get_current_caps.return_value = None
        self.assertFalse(helper.eligible())

    def test_deinterlace_qos_cache_rechecks_caps_and_mutable_properties(self):
        Gst = self.Gst
        policy_gst, element, properties, pads = DeinterlaceQosTests().fixture()
        helper = player.DeinterlaceQosWorkaround.install(policy_gst, element, mock.Mock())
        self.addCleanup(helper.close)
        source = Gst.Caps.from_string(
            "video/x-raw,format=YUY2,width=16,height=8,interlace-mode=mixed,framerate=25/1")
        output = Gst.Caps.from_string(
            "video/x-raw,format=YUY2,width=16,height=8,interlace-mode=progressive,framerate=50/1")
        pads["sink"].get_current_caps.return_value = source
        pads["src"].get_current_caps.return_value = output
        helper.caps_supported = mock.Mock(wraps=helper.caps_supported)
        element.get_factory.reset_mock()
        self.assertTrue(helper.eligible())
        self.assertEqual(helper.caps_supported.call_count, 1)
        # Equal replacement caps use the result, not object or string identity.
        pads["sink"].get_current_caps.return_value = source.copy()
        pads["src"].get_current_caps.return_value = output.copy()
        self.assertTrue(helper.eligible())
        self.assertEqual(helper.caps_supported.call_count, 1)
        for pad in pads.values():
            self.assertEqual(pad.get_current_caps.call_count, 2)
        element.get_factory.assert_not_called()

        changed = source.copy()
        changed.set_value("colorimetry", "bt709")
        pads["sink"].get_current_caps.return_value = changed
        self.assertTrue(helper.eligible())
        self.assertEqual(helper.caps_supported.call_count, 2)
        self.assertIsNone(source.get_structure(0).get_string("colorimetry"))
        wrong_output = output.copy()
        wrong_output.set_value("format", "NV12")
        pads["src"].get_current_caps.return_value = wrong_output
        self.assertFalse(helper.eligible())
        self.assertEqual(helper.caps_supported.call_count, 3)
        pads["src"].get_current_caps.return_value = output
        self.assertTrue(helper.eligible())
        self.assertEqual(helper.caps_supported.call_count, 4)
        pads["sink"].get_current_caps.return_value = None
        self.assertFalse(helper.eligible())
        pads["sink"].get_current_caps.return_value = changed
        self.assertTrue(helper.eligible())
        self.assertEqual(helper.caps_supported.call_count, 4)
        # Set equality alone must not admit duplicate structures: the
        # original policy requires exactly one structure on each pad.
        duplicate = output.copy()
        duplicate.append(output.copy())
        self.assertTrue(duplicate.is_equal(output))
        pads["src"].get_current_caps.return_value = duplicate
        self.assertFalse(helper.eligible())
        pads["src"].get_current_caps.return_value = output
        self.assertTrue(helper.eligible())

        # A cache hit never bypasses a changed runtime property or revives
        # an element after the first QoS event handled by stock policy.
        for name, original in tuple(properties.items()):
            with self.subTest(property=name):
                properties[name] = original + 1
                self.assertFalse(helper.eligible())
                properties[name] = original
                self.assertTrue(helper.eligible())
        info = mock.Mock()
        info.get_event.return_value.type = policy_gst.EventType.QOS
        properties["method"] = 0
        self.assertEqual(helper.event(None, info), policy_gst.PadProbeReturn.OK)
        properties["method"] = 4
        self.assertFalse(helper.eligible())
        pads["sink"].push_event.assert_not_called()
        helper.close()
        self.assertIsNone(helper.caps_policy)

    def test_deinterlace_qos_reads_changes_while_notifications_are_frozen(self):
        Gst = self.Gst
        element = Gst.ElementFactory.make("deinterlace")
        if element is None or not player.DeinterlaceQosWorkaround.supported(Gst, element):
            self.skipTest("exact GStreamer 1.28.2 deinterlace property policy")
        source = Gst.Caps.from_string(
            "video/x-raw,format=YUY2,width=16,height=8,interlace-mode=mixed,framerate=25/1")
        output = Gst.Caps.from_string(
            "video/x-raw,format=YUY2,width=16,height=8,interlace-mode=progressive,framerate=50/1")
        notifications = []
        handler = element.connect("notify", lambda _element, spec: notifications.append(spec.name))
        self.addCleanup(element.disconnect, handler)
        for name in ("mode", "method", "fields", "locking", "tff"):
            with self.subTest(property=name):
                helper = player.DeinterlaceQosWorkaround.__new__(player.DeinterlaceQosWorkaround)
                helper.Gst, helper.element = Gst, element
                helper.closed, helper.active, helper.caps_policy = False, True, None
                helper.sink, helper.src = mock.Mock(), mock.Mock()
                helper.sink.get_current_caps.return_value = source
                helper.src.get_current_caps.return_value = output
                self.assertTrue(helper.eligible())
                original = int(element.get_property(name))
                notifications.clear()
                element.freeze_notify()
                try:
                    element.set_property(name, original + 1)
                    self.assertEqual(notifications, [])
                    self.assertFalse(helper.eligible())
                    info = mock.Mock()
                    info.get_event.return_value.type = Gst.EventType.QOS
                    self.assertEqual(helper.event(None, info), Gst.PadProbeReturn.OK)
                    element.set_property(name, original)
                    self.assertTrue(helper.properties_supported(element))
                    self.assertFalse(helper.active)
                    self.assertFalse(helper.eligible())
                    helper.sink.push_event.assert_not_called()
                    self.assertEqual(notifications, [])
                finally:
                    element.set_property(name, original)
                    element.thaw_notify()
                self.assertIn(name, notifications)

    def test_deinterlace_qos_second_field_pts_tff_and_bff(self):
        Gst = self.Gst
        if tuple(Gst.version())[:3] != (1, 28, 2):
            self.skipTest("exact GStreamer 1.28.2 compatibility regression")
        for name in ("appsrc", "deinterlace", "fakesink"):
            if Gst.ElementFactory.find(name) is None:
                self.skipTest(f"optional deinterlace fixture needs {name}")
        import gi
        gi.require_version("GstVideo", "1.0")
        from gi.repository import GstVideo

        def event_identity(event):
            return (event.get_seqnum(), event.get_running_time_offset(), tuple(event.parse_qos()))

        def run_case(mode, top_first):
            pipeline = Gst.parse_launch(
                'appsrc name=input format=time is-live=false caps="video/x-raw,format=YUY2,'
                'width=16,height=8,interlace-mode=mixed,framerate=25/1,colorimetry=bt709" ! '
                'deinterlace name=di method=linear fields=all locking=none ! '
                'fakesink sync=false async=false qos=false')
            source, di = pipeline.get_by_name("input"), pipeline.get_by_name("di")
            outputs, upstream, injected, errors = [], [], [], []
            helper = None
            if mode == "workaround":
                helper = player.DeinterlaceQosWorkaround.install(Gst, di, errors.append)
                self.assertIsNotNone(helper)

            def observe_upstream(_pad, info):
                event = info.get_event()
                if event.type == Gst.EventType.QOS:
                    upstream.append(event_identity(event))
                return Gst.PadProbeReturn.OK

            def observe_output(_pad, info):
                buffer = info.get_buffer()
                outputs.append((int(buffer.pts), int(buffer.duration), int(buffer.get_flags()),
                                buffer.extract_dup(0, buffer.get_size())))
                if mode != "control" and buffer.pts == 40000000:
                    # The previous 20 ms field is 1 ms late: earliest=20+2+20=42 ms.
                    # The current 40 ms field is already out; its 60 ms sibling
                    # is still timely, but stock compares the original 40 ms PTS.
                    event = Gst.Event.new_qos(Gst.QOSType.UNDERFLOW, 1.0, 1000000, 20000000)
                    event.set_seqnum(0x2345)
                    injected.append(event_identity(event))
                    if not di.get_static_pad("src").send_event(event):
                        errors.append("synthetic QoS was not forwarded")
                return Gst.PadProbeReturn.OK

            source.get_static_pad("src").add_probe(Gst.PadProbeType.EVENT_UPSTREAM, observe_upstream)
            di.get_static_pad("src").add_probe(Gst.PadProbeType.BUFFER, observe_output)
            try:
                self.assertNotEqual(pipeline.set_state(Gst.State.PLAYING), Gst.StateChangeReturn.FAILURE)
                for index in range(3):
                    data = bytes((index * 37 + row * 19 + column * 3) % 256
                                 for row in range(8) for column in range(32))
                    buffer = Gst.Buffer.new_allocate(None, len(data), None)
                    buffer.fill(0, data)
                    buffer.pts, buffer.duration = index * 40000000, 40000000
                    flags = int(GstVideo.VideoBufferFlags.INTERLACED)
                    if top_first:
                        flags |= int(GstVideo.VideoBufferFlags.TFF)
                    buffer.set_flags(Gst.BufferFlags(flags))
                    self.assertEqual(source.emit("push-buffer", buffer), Gst.FlowReturn.OK)
                self.assertEqual(source.emit("end-of-stream"), Gst.FlowReturn.OK)
                message = pipeline.get_bus().timed_pop_filtered(
                    5 * Gst.SECOND, Gst.MessageType.EOS | Gst.MessageType.ERROR)
                self.assertIsNotNone(message)
                self.assertEqual(message.type, Gst.MessageType.EOS)
                self.assertFalse(errors)
                self.assertEqual(upstream, injected)
                if helper:
                    self.assertTrue(helper.eligible())
                    di.set_property("method", 1)
                    self.assertFalse(helper.eligible())
                return outputs
            finally:
                pipeline.set_state(Gst.State.NULL)
                if helper:
                    helper.close()

        for top_first in (True, False):
            with self.subTest(top_first=top_first):
                control = run_case("control", top_first)
                self.assertEqual([row[0] for row in control], list(range(0, 120000000, 20000000)))
                self.assertEqual(run_case("stock", top_first),
                                 [row for row in control if row[0] != 60000000])
                self.assertEqual(run_case("workaround", top_first), control)

    def test_actual_software_decoder_buffers_precede_eos(self):
        for name in ("videotestsrc", "theoraenc", "theoradec", "fakesink"):
            if self.Gst.ElementFactory.find(name) is None:
                self.skipTest(f"optional software API fixture needs {name}")
        code, result = self.run_pipeline(
            "videotestsrc num-buffers=3 ! video/x-raw,width=32,height=32 ! "
            "theoraenc ! theoradec name=tested-decoder ! fakesink sync=false")
        self.assertEqual(code, 0)
        self.assertEqual(result.health.frames, 3)

    def test_live_software_source_is_detected_without_preroll(self):
        for name in ("videotestsrc", "theoraenc", "theoradec", "fakesink"):
            if self.Gst.ElementFactory.find(name) is None:
                self.skipTest(f"optional live software API fixture needs {name}")
        code, result = self.run_pipeline(
            "videotestsrc is-live=true num-buffers=3 ! video/x-raw,width=32,height=32 ! "
            "theoraenc ! theoradec name=tested-decoder ! fakesink sync=false")
        self.assertEqual(code, 0)
        self.assertTrue(result.is_live)
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
