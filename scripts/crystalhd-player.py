#!/usr/bin/python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Small local-file controller; GStreamer owns decoding, audio and the window."""

import argparse
import os
from pathlib import Path
import signal
import sys
import termios
import time
import tty


def local_file(value):
    try:
        path = Path(value).resolve(strict=True)
        if not path.is_file():
            raise ValueError("not a regular file")
        with path.open("rb"):
            pass
        return path
    except (OSError, ValueError) as error:
        raise argparse.ArgumentTypeError(f"unreadable local file: {value}: {error}") from error


def arguments(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--hardware", dest="mode", action="store_const", const="hardware",
                       help="require CrystalHD video output (default)")
    modes.add_argument("--software", dest="mode", action="store_const", const="software",
                       help="disable hardware video decoders; no automatic fallback")
    parser.set_defaults(mode="hardware")
    parser.add_argument("--subtitles", type=local_file, metavar="FILE")
    parser.add_argument("file", type=local_file, metavar="FILE")
    return parser.parse_args(argv)


def video_decoder(klass):
    parts = klass.split("/")
    return "Decoder" in parts and "Video" in parts


def decoder_allowed(mode, name, klass):
    return name == "crystalhddec" if mode == "hardware" else "Hardware" not in klass.split("/")


class PlaybackHealth:
    """Main-loop-only evidence: an instantiated decoder is not a decoded frame."""
    START_TIMEOUT = 20.0
    STALL_TIMEOUT = 15.0

    def __init__(self, mode, now):
        self.mode = mode
        self.anchor = now
        self.frames = 0
        self.active = None
        self.active_frames = 0
        self.paused = False
        self.error = None

    def fail(self, message):
        if self.error is None:
            self.error = message

    def frame(self, token, name, klass, raw, size, now):
        if self.error is not None:
            return False
        if not raw or size <= 0:
            return False
        if not video_decoder(klass) or not decoder_allowed(self.mode, name, klass):
            self.fail(f"unexpected video decoder output: {name} ({klass})")
            return False
        changed = self.active != token
        if changed:
            self.active = token
            self.active_frames = 0
        self.frames += 1
        self.active_frames += 1
        self.anchor = now
        return changed

    def removed(self, token, now):
        if self.active == token:
            self.active = None
            self.active_frames = 0
            self.anchor = now

    def pause(self, paused, now):
        self.paused = paused
        self.anchor = now

    def seeking(self, now):
        self.anchor = now

    def tick(self, now):
        limit = self.STALL_TIMEOUT if self.active_frames else self.START_TIMEOUT
        if not self.paused and now - self.anchor > limit:
            self.fail("video output stalled" if self.active_frames else "no decoded video output")
        return self.error

    def finish(self):
        if not self.active_frames:
            self.fail("no output from an active video decoder")
        return 1 if self.error else 0


class ControlKeys:
    def __init__(self):
        self.pending = b""

    def feed(self, data):
        self.pending += data
        actions = []
        keys = {b" ": "pause", b"j": "back", b"l": "forward", b"q": "quit",
                b"1": "rate-half", b"2": "rate-normal", b"3": "rate-double",
                b"\x1b[D": "back", b"\x1b[C": "forward"}
        while self.pending:
            matched = next((key for key in keys if self.pending.startswith(key)), None)
            if matched:
                actions.append(keys[matched])
                self.pending = self.pending[len(matched):]
            elif any(key.startswith(self.pending) for key in keys):
                break
            else:
                self.pending = self.pending[1:]
        return actions


def configure_decoders(Gst, mode):
    found = False
    # Rank changes affect only this process, not installed plugins or other players.
    for factory in Gst.Registry.get().get_feature_list(Gst.ElementFactory):
        name = factory.get_name()
        klass = factory.get_metadata("klass") or ""
        if not video_decoder(klass):
            continue
        if name == "crystalhddec":
            found = True
        if not decoder_allowed(mode, name, klass):
            factory.set_rank(Gst.Rank.NONE)
        elif name == "crystalhddec":
            factory.set_rank(int(Gst.Rank.PRIMARY) + 100)
    if mode == "hardware" and not found:
        raise RuntimeError("crystalhddec is unavailable; build/install the GStreamer plugin")


class Player:
    def __init__(self, args, Gst, GLib, sinks=None):
        self.args, self.Gst, self.GLib = args, Gst, GLib
        self.loop = GLib.MainLoop()
        self.health = PlaybackHealth(args.mode, time.monotonic())
        self.pipeline = None
        self.done = False
        self.closed = False
        self.exit_code = 0
        self.rate = 1.0
        self.decoders = {}
        self.next_decoder = 0
        self.keys = ControlKeys()
        self.terminal_state = None
        self.sources = []
        configure_decoders(Gst, args.mode)
        self.pipeline = Gst.ElementFactory.make("playbin", "crystalhd-player")
        if self.pipeline is None:
            raise RuntimeError("playbin is unavailable; install gstreamer1.0-plugins-base")
        self.pipeline.set_property("uri", args.file.as_uri())
        if args.subtitles:
            self.pipeline.set_property("suburi", args.subtitles.as_uri())
        if args.mode == "software":
            # GstPlayFlags::FORCE_SW_DECODERS, present since GStreamer 1.18.
            self.pipeline.set_property("flags", int(self.pipeline.get_property("flags")) | (1 << 12))
        for prop, sink in (sinks or {}).items():
            self.pipeline.set_property(prop, sink)
        self.pipeline.connect("element-setup", self.element_setup)
        self.pipeline.connect("deep-element-removed", self.element_removed)
        self.bus = self.pipeline.get_bus()
        self.bus.add_signal_watch()
        self.bus.connect("message", self.message)

    def fail(self, message):
        self.health.fail(message)
        self.stop(1)

    def element_setup(self, _pipeline, element):
        factory = element.get_factory()
        if factory is None:
            return
        name, klass = factory.get_name(), factory.get_metadata("klass") or ""
        if not video_decoder(klass):
            return
        self.next_decoder += 1
        token = self.next_decoder
        self.decoders[element] = token
        pad = element.get_static_pad("src")
        if pad is not None:
            pad.add_probe(self.Gst.PadProbeType.BUFFER, self.decoder_output, token, name, klass)

    def decoder_output(self, pad, info, token, name, klass):
        buffer = info.get_buffer()
        caps = pad.get_current_caps()
        raw = caps is not None and caps.get_size() == 1 and caps.get_structure(0).get_name() == "video/x-raw"
        if buffer is not None:
            fields = {"token": token, "name": name, "klass": klass, "raw": raw,
                      "size": buffer.get_size(), "observed": time.monotonic()}
            self.post_observation("crystalhd-player-frame", fields)
        return self.Gst.PadProbeReturn.OK

    def post_observation(self, name, fields):
        # Use the bus, not lower-priority idle callbacks: the last frame's
        # evidence must be processed before a following EOS message.
        structure = self.Gst.Structure.new_empty(name)
        for key, value in fields.items():
            structure.set_value(key, value)
        self.bus.post(self.Gst.Message.new_application(self.pipeline, structure))

    def decoded_frame(self, token, name, klass, raw, size, now):
        if not self.done and token in self.decoders.values():
            if self.health.frame(token, name, klass, raw, size, now):
                print(f"Decoder: {name} ({self.args.mode}; video output confirmed)", flush=True)
            if self.health.error:
                self.stop(1)
        return False

    def element_removed(self, _pipeline, _bin, element):
        token = self.decoders.get(element)
        if token is not None:
            self.post_observation("crystalhd-player-removed", {"token": token})

    def decoder_removed(self, token):
        for element, candidate in list(self.decoders.items()):
            if candidate == token:
                del self.decoders[element]
        self.health.removed(token, time.monotonic())

    def message(self, _bus, message):
        Gst = self.Gst
        if self.done:
            return
        if message.type == Gst.MessageType.APPLICATION:
            fields = message.get_structure()
            if fields.get_name() == "crystalhd-player-frame":
                self.decoded_frame(*(fields.get_value(key) for key in
                                     ("token", "name", "klass", "raw", "size", "observed")))
            elif fields.get_name() == "crystalhd-player-removed":
                self.decoder_removed(fields.get_value("token"))
        elif message.type == Gst.MessageType.ERROR:
            error, _debug = message.parse_error()
            self.fail(f"{message.src.get_name()}: {error.message}")
        elif message.type == Gst.MessageType.EOS:
            self.stop(self.health.finish())
        elif message.type == Gst.MessageType.CLOCK_LOST and not self.health.paused:
            self.pipeline.set_state(Gst.State.PAUSED)
            self.pipeline.set_state(Gst.State.PLAYING)
        elif message.type == Gst.MessageType.LATENCY:
            self.pipeline.recalculate_latency()

    def seek(self, offset=0.0, rate=None):
        Gst = self.Gst
        ok, position = self.pipeline.query_position(Gst.Format.TIME)
        if not ok:
            print("Cannot seek: playback position is unavailable.", file=sys.stderr)
            return
        wanted = self.rate if rate is None else rate
        position = max(0, position + int(offset * Gst.SECOND))
        ok, duration = self.pipeline.query_duration(Gst.Format.TIME)
        if ok and duration > 0:
            position = min(position, max(0, duration - 1))
        accepted = self.pipeline.seek(wanted, Gst.Format.TIME,
                                      Gst.SeekFlags.FLUSH | Gst.SeekFlags.ACCURATE,
                                      Gst.SeekType.SET, position, Gst.SeekType.NONE, -1)
        if not accepted:
            print("Seek/rate change was rejected; playback settings are unchanged.", file=sys.stderr)
            return
        self.rate = wanted
        self.health.seeking(time.monotonic())
        print(f"Requested position {position / Gst.SECOND:.1f}s, rate {wanted:g}x", flush=True)

    def control(self, action):
        if action == "quit":
            self.stop(self.health.finish())
        elif action == "pause":
            paused = not self.health.paused
            state = self.Gst.State.PAUSED if paused else self.Gst.State.PLAYING
            if self.pipeline.set_state(state) == self.Gst.StateChangeReturn.FAILURE:
                self.fail("pause/resume state change failed")
            else:
                self.health.pause(paused, time.monotonic())
                print("Paused" if paused else "Playing", flush=True)
        elif action in ("back", "forward"):
            self.seek(-10.0 if action == "back" else 10.0)
        elif action in ("rate-half", "rate-normal", "rate-double"):
            self.seek(rate={"rate-half": 0.5, "rate-normal": 1.0, "rate-double": 2.0}[action])

    def terminal_input(self, fd, condition):
        if condition & self.GLib.IO_IN:
            try:
                data = os.read(fd, 64)
            except OSError as error:
                self.fail(f"terminal input failed: {error}")
                return False
            for action in self.keys.feed(data):
                self.control(action)
                if self.done:
                    break
        # Keep the watch alive until cleanup; an EOF stops polling stdin only.
        return not bool(condition & (self.GLib.IO_HUP | self.GLib.IO_ERR))

    def tick(self):
        if not self.done and self.health.tick(time.monotonic()):
            self.stop(1)
        return True

    def stop(self, code=0):
        self.exit_code = max(self.exit_code, code)
        self.done = True
        self.loop.quit()

    def interrupted(self, number):
        self.stop(128 + number)
        return True

    def close(self):
        if self.closed:
            return
        self.closed = True
        # Restore the terminal even if a device's subsequent close is slow.
        if self.terminal_state:
            fd, previous = self.terminal_state
            self.terminal_state = None
            try:
                termios.tcsetattr(fd, termios.TCSANOW, previous)
            except (termios.error, OSError) as error:
                print(f"crystalhd-play: terminal restoration failed: {error}", file=sys.stderr)
                self.exit_code = max(self.exit_code, 1)
        for source in self.sources:
            if self.GLib.MainContext.default().find_source_by_id(source):
                self.GLib.source_remove(source)
        self.bus.remove_signal_watch()
        self.pipeline.set_state(self.Gst.State.NULL)

    def run(self):
        try:
            if sys.stdin.isatty():
                fd = sys.stdin.fileno()
                self.terminal_state = (fd, termios.tcgetattr(fd))
                tty.setcbreak(fd)
                self.sources.append(self.GLib.io_add_watch(
                    fd, self.GLib.IO_IN | self.GLib.IO_HUP | self.GLib.IO_ERR,
                    self.terminal_input))
                print("Controls: Space pause/resume; Left/Right or j/l seek 10s; "
                      "1/2/3 = 0.5x/1x/2x; q quit.", flush=True)
            else:
                print("Terminal controls unavailable (stdin is not a terminal).", flush=True)
            for number in (signal.SIGINT, signal.SIGTERM):
                self.sources.append(self.GLib.unix_signal_add(
                    self.GLib.PRIORITY_DEFAULT, number, self.interrupted, number))
            self.sources.append(self.GLib.timeout_add(250, self.tick))
            if self.pipeline.set_state(self.Gst.State.PLAYING) == self.Gst.StateChangeReturn.FAILURE:
                self.fail("could not start playback")
            if not self.done:
                self.loop.run()
        finally:
            self.close()
        if self.health.error:
            print(f"crystalhd-play: {self.health.error}", file=sys.stderr)
            if self.args.mode == "hardware":
                print("No automatic fallback. Close other hardware players, or retry with --software.",
                      file=sys.stderr)
        print(f"Decoded video frames: {self.health.frames}", flush=True)
        return self.exit_code


def main(argv=None):
    args = arguments(argv)
    try:
        import gi
        gi.require_version("Gst", "1.0")
        from gi.repository import GLib, Gst
    except (ImportError, ValueError) as error:
        print(f"crystalhd-play: GStreamer Python bindings unavailable: {error}\n"
              "Install python3-gi and gir1.2-gstreamer-1.0.", file=sys.stderr)
        return 1
    try:
        Gst.init(None)
        return Player(args, Gst, GLib).run()
    except (RuntimeError, OSError, ValueError, GLib.Error) as error:
        print(f"crystalhd-play: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
