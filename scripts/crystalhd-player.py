#!/usr/bin/python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Small file/HTTP(S) controller; GStreamer owns decoding, audio and the window."""
# CrystalHD managed defaults v1:player

import argparse
import importlib.machinery
import importlib.util
import os
from pathlib import Path
import re
import signal
import stat
import sys
import termios
import time
import tty
from urllib.parse import urlsplit

MAX_SOFTWARE_OUTPUT_DIMENSION = 4096
MAX_SOFTWARE_OUTPUT_PIXELS = 4096 * 2160
CONFIG_CORE_RELATIVE = "@PLAYER_CONFIG_CORE_RELATIVE@"
PLAYER_SETTINGS = ("player.decoder", "gstreamer.crystalhddec.scale_width",
                   "player.software_output_size")
HANDLED_BUS_MESSAGE_DETAILS = (
    "application",
    "error",
    "warning",
    "eos",
    "async-done",
    "state-changed",
    "clock-lost",
    "latency",
    "buffering",
    "qos",
)


def connect_bus_messages(bus, callback):
    """Subscribe only to message types the controller consumes."""
    for detail in HANDLED_BUS_MESSAGE_DETAILS:
        bus.connect(f"message::{detail}", callback)


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


def playback_input(value):
    # Keep real local filenames (including colons) and their Path API unchanged.
    if not re.match(r"^[A-Za-z][A-Za-z0-9+.-]*:", value):
        return local_file(value)
    try:
        if Path(value).is_file():
            return local_file(value)
    except (OSError, ValueError):
        # A signed URL can be longer than the filesystem's filename limit.
        pass
    try:
        if (any(character.isspace() or ord(character) < 32 or ord(character) == 127
                for character in value) or "\\" in value or
                any(character in '<>"{}|^`' for character in value) or
                re.search(r"%(?![0-9A-Fa-f]{2})", value)):
            raise ValueError("invalid URL characters")
        parsed = urlsplit(value)
        if parsed.scheme.lower() not in ("http", "https"):
            raise ValueError("unsupported scheme")
        authority = parsed.netloc.rsplit("@", 1)[-1]
        bracket_tail = authority[authority.index("]") + 1:] if authority.startswith("[") else ""
        if (not parsed.netloc or not parsed.hostname or parsed.port == 0 or
                parsed.netloc.count("@") > 1 or authority.endswith(":") or
                (bracket_tail and re.fullmatch(r":[0-9]+", bracket_tail) is None)):
            raise ValueError("missing host or invalid port")
    except ValueError as error:
        # argparse must not echo credentials or signed query strings on failure.
        raise argparse.ArgumentTypeError(
            "input must be a readable local file or a valid HTTP(S) URL") from error
    # Do not normalize escaping, query ordering or case in signed URLs.
    return value


def input_error(source, message):
    if isinstance(source, str):
        message = message.replace(source, "<remote URL>")
    # Apostrophes are legal URL subdelimiters. Conservatively consume the
    # full token, including diagnostic quotes, so a redirect's query suffix
    # cannot survive masking just because it contains a quote. Warnings for
    # local input may also contain remote URLs from downstream elements.
    message = re.sub(r"https?://[^\s<>]+", "<remote URL>", message,
                     flags=re.IGNORECASE)
    return message


def hardware_scale_width(value):
    try:
        width = int(value, 10)
    except ValueError as error:
        raise argparse.ArgumentTypeError("must be 0 or an even integer from 128 to 1918") from error
    if width != 0 and (width < 128 or width > 1918 or width % 2):
        raise argparse.ArgumentTypeError("must be 0 or an even integer from 128 to 1918")
    return width


def output_size(value):
    match = re.fullmatch(r"([0-9]+)[xX]([0-9]+)", value)
    if match is None:
        raise argparse.ArgumentTypeError(
            "must use WIDTHxHEIGHT with positive even dimensions up to 4096 and 8.8 MP")
    width, height = (int(part, 10) for part in match.groups())
    if (width <= 0 or height <= 0 or
            width > MAX_SOFTWARE_OUTPUT_DIMENSION or
            height > MAX_SOFTWARE_OUTPUT_DIMENSION or
            width * height > MAX_SOFTWARE_OUTPUT_PIXELS or
            width % 2 or height % 2):
        raise argparse.ArgumentTypeError(
            "must use WIDTHxHEIGHT with positive even dimensions up to 4096 and 8.8 MP")
    return width, height


class _CoreLoader(importlib.machinery.SourceFileLoader):
    def set_data(self, path, data, **kwargs):
        pass  # Reading defaults must not create cache files beside the core.


def load_config_core():
    script = Path(__file__).resolve()
    relative = CONFIG_CORE_RELATIVE
    if relative.startswith("@") and relative.endswith("@"):
        relative = "crystalhd-config.py"
    path = script.parent / relative
    own, source = script.stat(), path.lstat()
    if (not stat.S_ISREG(source.st_mode) or source.st_mode & 0o022
            or source.st_uid != own.st_uid):
        raise RuntimeError("unsafe configuration core")
    loader = _CoreLoader("crystalhd_player_config", str(path))
    spec = importlib.util.spec_from_loader(loader.name, loader)
    core = importlib.util.module_from_spec(spec)
    loader.exec_module(core)
    required = ("SYSTEM_CONFIG", "load_document", "default_user_config", "load_preset_catalog", "_validated_preset",
                "resolve", "SchemaError", "UNSET", "ABSENT", "_format_value", "_report_origin")
    if not all(hasattr(core, name) for name in required):
        raise RuntimeError("a matching configuration core is required")
    return core


def requested_output_size(value):
    return None if value == "native" else output_size(value)


def apply_player_defaults(args, parser, environment):
    explicit = {name: getattr(args, name) for name in
                ("mode", "scale_width", "software_output_size") if hasattr(args, name)}
    # Keep the original CLI-only behavior if managed configuration is bypassed
    # or unavailable. Never mutate the environment seen by native components.
    args.mode = explicit.get("mode", "hardware")
    args.scale_width = explicit.get("scale_width", 0)
    args.software_output_size = explicit.get("software_output_size")
    args.settings_report = None
    if environment.get("CRYSTALHD_CONFIG_DISABLE") == "1":
        return
    try:
        if os.getuid() != os.geteuid() or os.getgid() != os.getegid():
            raise RuntimeError("managed configuration disabled for differing IDs")
        core = load_config_core()
    except (OSError, RuntimeError, ImportError, SyntaxError):
        print("crystalhd-play: managed defaults unavailable; using CLI/native defaults. "
              "Check the matching crystalhd-config installation.", file=sys.stderr)
        return
    try:
        system = core.load_document(core.SYSTEM_CONFIG, "system configuration", system=True)
        user = core.load_document(core.default_user_config(environment), "user configuration")
        catalog = core.load_preset_catalog()
        preset = core._validated_preset(catalog, system, user, None, ())
        base = core.resolve(system, user, catalog.selectable, explicit_preset=preset)
    except core.SchemaError:
        print("crystalhd-play: managed defaults rejected; using CLI/native defaults. "
              "Run crystalhd-config validate for details.", file=sys.stderr)
        return
    managed = any(base.origins[key] != "compiled" for key in PLAYER_SETTINGS)
    application = {}
    if "mode" in explicit:
        application["player.decoder"] = explicit["mode"]
    if "scale_width" in explicit:
        application["gstreamer.crystalhddec.scale_width"] = explicit["scale_width"]
    if "software_output_size" in explicit:
        size = explicit["software_output_size"]
        application["player.software_output_size"] = (
            core.UNSET if size is None else "{0}x{1}".format(*size))
    try:
        result = core.resolve(system, user, catalog.selectable, explicit_preset=preset,
                              application=application)
    except core.SchemaError:
        if not managed:
            return  # Original CLI-only coherence checks follow in arguments().
        parser.error("player flags conflict with saved defaults; to disable saved scaling use "
                     "--scale-width 0 --software-output-size native (also with --software)")
    args.mode = result.effective["player.decoder"]
    args.scale_width = result.effective["gstreamer.crystalhddec.scale_width"]
    size = result.effective["player.software_output_size"]
    args.software_output_size = None if size is core.ABSENT else output_size(size)
    if managed:
        args.settings_report = "Requested player settings: " + "; ".join(
            "{0}={1} [{2}]".format(key, "native" if result.effective[key] is core.ABSENT else
                                   core._format_value(result.effective[key]),
                                   core._report_origin(result.origins[key], True))
            for key in PLAYER_SETTINGS)


def arguments(argv=None, environment=None):
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--hardware", dest="mode", action="store_const", const="hardware",
                       default=argparse.SUPPRESS,
                       help="require CrystalHD video output (default)")
    modes.add_argument("--software", dest="mode", action="store_const", const="software",
                       default=argparse.SUPPRESS,
                       help="disable hardware video decoders; no automatic fallback")
    parser.add_argument("--weave", action="store_true",
                        help="preserve interlaced frames without deinterlacing; works with either decoder mode")
    parser.add_argument("--scale-width", type=hardware_scale_width, default=argparse.SUPPRESS, metavar="PIXELS",
                        help="request an even BCM70015 output width from 128 to 1918 (interlaced H.264: multiple of 16; 0: native)")
    parser.add_argument("--software-output-size", type=requested_output_size,
                        default=argparse.SUPPRESS, metavar="WIDTHxHEIGHT|native",
                        help="resize scaled CrystalHD output; native clears a saved resize")
    parser.add_argument("--subtitles", type=local_file, metavar="FILE")
    parser.add_argument("file", type=playback_input, metavar="FILE_OR_HTTP_URL")
    args = parser.parse_args(argv)
    apply_player_defaults(args, parser, os.environ if environment is None else environment)
    if args.mode != "hardware" and (args.scale_width or args.software_output_size):
        parser.error("--scale-width and --software-output-size require --hardware")
    if args.software_output_size and not args.scale_width:
        parser.error("--software-output-size requires a nonzero --scale-width")
    return args


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


def make_software_output_filter(Gst, size, *, prepare_native_packing=False):
    """Build an explicit raw-video resize bin for playbin's video-filter."""
    width, height = size
    result = Gst.Bin.new("crystalhd-software-upscale")
    scale = Gst.ElementFactory.make("videoscale", "crystalhd-upscale")
    capsfilter = Gst.ElementFactory.make("capsfilter", "crystalhd-upscale-caps")
    if result is None or scale is None or capsfilter is None:
        raise RuntimeError("software output sizing needs GStreamer videoscale and capsfilter")
    # Bilinear multi-tap is the explicit quality/performance compromise used by
    # this opt-in path; native playback remains completely unchanged.
    scale.set_property("method", 4)
    if scale.find_property("add-borders") is not None:
        scale.set_property("add-borders", True)
    if scale.find_property("n-threads") is not None:
        scale.set_property("n-threads", 2)
    capsfilter.set_property(
        "caps", Gst.Caps.from_string(
            f"video/x-raw,format=YUY2,width={width},height={height},"
            "pixel-aspect-ratio=1/1"))
    elements = [scale, capsfilter]
    if prepare_native_packing:
        inlet = Gst.ElementFactory.make("capsfilter", "crystalhd-early-yuy2")
        convert = Gst.ElementFactory.make("videoconvert", "crystalhd-early-pack")
        if inlet is None or convert is None:
            raise RuntimeError("native output packing needs GStreamer videoconvert and capsfilter")
        # Keep stock deinterlacing in YUY2. Packing, if selected, belongs before
        # the resize so the sink thread need not repack a full-size picture.
        inlet.set_property("caps", Gst.Caps.from_string("video/x-raw,format=YUY2"))
        convert.set_property("n-threads", 1)
        elements[:0] = [inlet, convert]
    for element in elements:
        result.add(element)
    if not all(left.link(right) for left, right in zip(elements, elements[1:])):
        raise RuntimeError("could not link the software output sizing elements")
    sink_pad = elements[0].get_static_pad("sink")
    source_pad = capsfilter.get_static_pad("src")
    if sink_pad is None or source_pad is None:
        raise RuntimeError("software output sizing elements have no usable raw-video pads")
    if not result.add_pad(Gst.GhostPad.new("sink", sink_pad)) or not result.add_pad(
            Gst.GhostPad.new("src", source_pad)):
        raise RuntimeError("could not expose the software output sizing pads")
    return result


class XvOutputPacking:
    """Select low-resolution packing only before a selected Xv chain starts."""

    def __init__(self, Gst, video_filter):
        self.Gst, self.video_filter = Gst, video_filter
        self.output = video_filter.get_by_name("crystalhd-upscale-caps")
        self.selected = False

    def select(self, element, pipeline):
        if self.selected:
            return False
        Gst = self.Gst
        try:
            factory = element.get_factory()
            if factory is None or factory.get_name() != "xvimagesink":
                return False
            chain = self.video_filter.get_parent()
            playsink = chain.get_parent() if chain is not None else None
            factory = playsink.get_factory() if playsink is not None else None
            if (factory is None or factory.get_name() != "playsink" or
                    not playsink.has_as_ancestor(pipeline) or
                    not element.has_as_ancestor(chain)):
                return False
            # element-setup runs while playsink adds its already-READY sink
            # chain, before activating our filter. Never renegotiate live data.
            for item, expected in ((self.video_filter, Gst.State.NULL),
                                   (element, Gst.State.READY)):
                status, state, pending = item.get_state(0)
                if (status != Gst.StateChangeReturn.SUCCESS or state != expected or
                        pending != Gst.State.VOID_PENDING):
                    return False
            if any(self.video_filter.get_static_pad(name).get_current_caps() is not None
                   for name in ("sink", "src")):
                return False
            pad = element.get_static_pad("sink")
            available = pad.query_caps(None) if pad is not None else None
            current = self.output.get_property("caps")
            if (available is None or available.is_any() or available.is_empty() or
                    current is None or current.get_size() != 1 or
                    current.get_structure(0).get_string("format") != "YUY2"):
                return False
            native = current.copy()
            native.set_value("format", "UYVY")
            if not available.can_intersect(native) or available.can_intersect(current):
                return False
            # videoscale does not convert formats: this one caps change makes
            # the prepared converter pack UYVY at the smaller input dimensions.
            self.output.set_property("caps", native)
            self.selected = True
            return True
        except Exception:
            # Sink discovery is optional; uncertain queries keep the YUY2 path.
            return False


class DeinterlaceQosWorkaround:
    """Bypass only the qualified deinterlacer's incorrect picture-PTS QoS cache."""

    def __init__(self, Gst, element, on_error):
        self.Gst, self.element, self.on_error = Gst, element, on_error
        self.sink, self.src = (element.get_static_pad(name) for name in ("sink", "src"))
        self.closed = False
        self.active = True
        self.caps_policy = None
        self.probe = self.src.add_probe(Gst.PadProbeType.EVENT_UPSTREAM, self.event)

    @staticmethod
    def properties_supported(element):
        return tuple(int(value) for value in element.get_properties(
            "mode", "method", "fields", "locking", "tff")) == (0, 4, 0, 0, 0)

    @classmethod
    def supported(cls, Gst, element):
        factory = element.get_factory()
        plugin = factory.get_plugin() if factory is not None else None
        return (tuple(Gst.version())[:3] == (1, 28, 2) and
                factory is not None and factory.get_name() == "deinterlace" and
                plugin is not None and plugin.get_version() == "1.28.2" and
                cls.properties_supported(element))

    @classmethod
    def install(cls, Gst, element, on_error):
        if not cls.supported(Gst, element):
            return None
        if any(element.get_static_pad(name) is None for name in ("sink", "src")):
            return None
        return cls(Gst, element, on_error)

    def eligible(self):
        # Core/plugin identity was admitted at install; mutable properties and
        # both current caps still need checking on every QoS event.
        if self.closed or not self.active or not self.properties_supported(self.element):
            return False
        caps = tuple(pad.get_current_caps() for pad in (self.sink, self.src))
        if any(value is None or value.get_size() != 1 for value in caps):
            return False
        cached = self.caps_policy
        if cached is not None and all(value.is_equal(old) for value, old in zip(caps, cached[:2])):
            return cached[2]
        accepted = self.caps_supported(*caps)
        # Retained GstCaps references are read-only; renegotiation uses COW.
        self.caps_policy = (*caps, accepted)
        return accepted

    @staticmethod
    def caps_supported(source_caps, output_caps):
        if any(value.get_size() != 1 for value in (source_caps, output_caps)):
            return False
        source, output = (value.get_structure(0) for value in (source_caps, output_caps))
        # Keep this compatibility workaround within the measured 25i -> 50p
        # packed-YUY2 path. Other rates, layouts and methods keep stock policy.
        return (all(value.get_name() == "video/x-raw" and
                    value.get_string("format") == "YUY2" for value in (source, output)) and
                source.get_string("interlace-mode") in ("mixed", "interleaved") and
                output.get_string("interlace-mode") == "progressive" and
                source.get_fraction("framerate") == (True, 25, 1) and
                output.get_fraction("framerate") == (True, 50, 1) and
                all(source.get_int(name)[0] and source.get_int(name)[1] > 0 and
                    source.get_int(name) == output.get_int(name) for name in ("width", "height")))

    def event(self, _pad, info):
        Gst = self.Gst
        event = info.get_event()
        if event.type != Gst.EventType.QOS:
            return Gst.PadProbeReturn.OK
        if not self.eligible():
            # Once stock has observed QoS, keep its policy for this element's
            # lifetime; later caps changes must not leave a stale local cache.
            self.active = False
            return Gst.PadProbeReturn.OK
        on_error = self.on_error
        # 1.28.2 tests both fields against the input picture's PTS, including
        # the later field whose output PTS still meets the QoS deadline.
        # Skip this stage's entire QoS cache update, not downstream QoS: retain
        # the unchanged event upstream and leave the final sink authoritative.
        try:
            forwarded = self.sink.push_event(event.copy())
        except Exception:
            forwarded = False
        if not forwarded and on_error is not None:
            on_error("could not forward deinterlacer QoS feedback upstream")
        # DROP releases the original event. Never retry after a forwarding
        # failure: it may already have reached an upstream element.
        return Gst.PadProbeReturn.DROP

    def close(self):
        if not self.closed:
            self.closed = True
            self.src.remove_probe(self.probe)
            self.probe = 0
            self.on_error = None
            self.caps_policy = None


class NativeDeinterlaceQosWorkaround:
    """Own-plugin equivalent; older plugins retain the Python implementation."""

    def __init__(self, controller):
        self.controller = controller
        self.closed = False

    @classmethod
    def install(cls, Gst, element, bus):
        controller = Gst.ElementFactory.make("crystalhddiqos")
        if controller is None:
            return None
        try:
            result = controller.emit("attach", element, bus)
        except Exception as error:
            controller.emit("detach")
            raise RuntimeError("could not attach native deinterlacer QoS helper") from error
        if result == 1:
            return cls(controller)
        controller.emit("detach")
        if result == 0:
            return None
        # Do not stack a Python probe on an already-owned or invalid target.
        raise RuntimeError("native deinterlacer QoS target is invalid or already owned")

    @property
    def active(self):
        return not self.closed and self.controller.get_property("active")

    @property
    def probe(self):
        return int(not self.closed and self.controller.get_property("attached"))

    def eligible(self):
        return not self.closed and self.controller.get_property("eligible")

    def close(self):
        if not self.closed:
            self.controller.emit("detach")
            self.closed = True


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
        self.buffering = False
        self.is_live = False
        self.qos_warned = False
        self.decoders = {}
        self.deinterlace_qos = {}
        self.next_decoder = 0
        self.seek_target = None
        self.queued_seek = None
        self.keys = ControlKeys()
        self.terminal_state = None
        self.sources = []
        configure_decoders(Gst, args.mode)
        self.pipeline = Gst.ElementFactory.make("playbin", "crystalhd-player")
        if self.pipeline is None:
            raise RuntimeError("playbin is unavailable; install gstreamer1.0-plugins-base")
        self.pipeline.set_property("uri", args.file.as_uri() if isinstance(args.file, Path)
                                   else args.file)
        if args.subtitles:
            self.pipeline.set_property("suburi", args.subtitles.as_uri())
        if args.mode == "software" or args.weave:
            flags = int(self.pipeline.get_property("flags"))
            # GstPlayFlags::FORCE_SW_DECODERS, present since GStreamer 1.18.
            if args.mode == "software":
                flags |= 1 << 12
            if args.weave:
                # GstPlayFlags::DEINTERLACE; leave audio, subtitles and other flags alone.
                flags &= ~(1 << 9)
            self.pipeline.set_property("flags", flags)
        if args.weave:
            if int(self.pipeline.get_property("flags")) & (1 << 9):
                raise RuntimeError("GStreamer did not disable deinterlacing for --weave")
            decoder = "CrystalHD hardware" if args.mode == "hardware" else "software"
            print(f"Output: Weave (deinterlacing off; {decoder} decoding required)", flush=True)
        self.video_filter = None
        self.output_packing = None
        if args.software_output_size:
            prepare_packing = (args.mode == "hardware" and args.scale_width and
                               args.software_output_size[0] > args.scale_width and
                               not args.weave and "video-sink" not in (sinks or {}))
            self.video_filter = make_software_output_filter(
                Gst, args.software_output_size, prepare_native_packing=prepare_packing)
            if prepare_packing:
                self.output_packing = XvOutputPacking(Gst, self.video_filter)
            self.pipeline.set_property("video-filter", self.video_filter)
        for prop, sink in (sinks or {}).items():
            self.pipeline.set_property(prop, sink)
        self.pipeline.connect("element-setup", self.element_setup)
        self.pipeline.connect("deep-element-removed", self.element_removed)
        self.bus = self.pipeline.get_bus()
        self.bus.add_signal_watch()
        connect_bus_messages(self.bus, self.message)

    def fail(self, message):
        self.health.fail(message)
        self.stop(1)

    def element_setup(self, _pipeline, element):
        factory = element.get_factory()
        if factory is None:
            return
        name, klass = factory.get_name(), factory.get_metadata("klass") or ""
        if name == "xvimagesink" and self.output_packing is not None and not self.closed:
            self.output_packing.select(element, self.pipeline)
        if (name == "deinterlace" and self.args.mode == "hardware" and
                self.args.scale_width and not self.args.weave and
                element not in self.deinterlace_qos):
            try:
                workaround = NativeDeinterlaceQosWorkaround.install(self.Gst, element, self.bus)
            except RuntimeError as error:
                self.fail(str(error))
                return
            if workaround is None:
                workaround = DeinterlaceQosWorkaround.install(
                    self.Gst, element, lambda reason: self.post_observation(
                        "crystalhd-player-output-error", {"message": reason}))
            if workaround is not None:
                self.deinterlace_qos[element] = workaround
        if not video_decoder(klass):
            return
        if name == "crystalhddec" and self.args.scale_width:
            if element.find_property("scale-width") is None:
                self.fail("crystalhddec has no scale-width property; update the GStreamer plugin")
                return
            try:
                element.set_property("scale-width", self.args.scale_width)
            except (TypeError, ValueError) as error:
                self.fail(f"could not set CrystalHD scale width {self.args.scale_width}: {error}")
                return
        self.next_decoder += 1
        token = self.next_decoder
        self.decoders[element] = token
        pad = element.get_static_pad("src")
        if pad is not None:
            pad.add_probe(self.Gst.PadProbeType.BUFFER, self.decoder_output, token, name, klass)
        if name == "crystalhddec" and self.args.scale_width:
            pad = element.get_static_pad("sink")
            if pad is not None:
                pad.add_probe(self.Gst.PadProbeType.EVENT_DOWNSTREAM,
                              self.decoder_input, token)

    def decoder_input(self, _pad, info, token):
        event = info.get_event()
        if self.args.scale_width and event.type == self.Gst.EventType.CAPS:
            caps = event.parse_caps()
            if caps is not None and caps.get_size() == 1:
                structure = caps.get_structure(0)
                mode = structure.get_string("interlace-mode")
                field_scale = (structure.get_name() == "video/x-h264" and
                               mode in ("interleaved", "mixed") and
                               self.args.scale_width % 16 == 0)
                if mode is not None and mode != "progressive" and not field_scale:
                    # The plugin already rejects this combination. playbin
                    # may discard that branch's initialization error and play
                    # audio alone, so retain the reason on our existing bus.
                    self.post_observation("crystalhd-player-error", {
                        "token": token,
                        "message": "interlaced --scale-width requires BCM70015 H.264 "
                                   "field input and a multiple-of-16 width "
                                   f"(interlace-mode={mode}); omit --scale-width "
                                   "for native interlaced playback",
                    })
        return self.Gst.PadProbeReturn.OK

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
        workaround = self.deinterlace_qos.pop(element, None)
        if workaround is not None:
            workaround.close()
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
            elif fields.get_name() == "crystalhd-player-error":
                if fields.get_value("token") in self.decoders.values():
                    self.fail(fields.get_value("message"))
            elif fields.get_name() == "crystalhd-player-output-error":
                self.fail(fields.get_value("message"))
        elif message.type == Gst.MessageType.ERROR:
            error, _debug = message.parse_error()
            self.fail(input_error(self.args.file,
                                  f"{message.src.get_name()}: {error.message}"))
        elif message.type == Gst.MessageType.WARNING:
            warning, _debug = message.parse_warning()
            print("crystalhd-play: warning: " + input_error(self.args.file, warning.message),
                  file=sys.stderr, flush=True)
        elif message.type == Gst.MessageType.EOS:
            self.stop(self.health.finish())
        elif message.type == Gst.MessageType.ASYNC_DONE:
            self.seek_completed()
        elif message.type == Gst.MessageType.STATE_CHANGED and message.src == self.pipeline:
            # ASYNC_DONE can precede the final PAUSED -> PLAYING transition.
            # Recheck readiness there so queued controls cannot remain stuck.
            self.seek_completed()
        elif message.type == Gst.MessageType.CLOCK_LOST and not (
                self.health.paused or self.buffering):
            if self.set_state(Gst.State.PAUSED, "clock recovery pause failed"):
                self.set_state(Gst.State.PLAYING, "clock recovery resume failed")
        elif message.type == Gst.MessageType.LATENCY:
            self.pipeline.recalculate_latency()
        elif message.type == Gst.MessageType.BUFFERING:
            self.buffering_changed(message.parse_buffering())
        elif message.type == Gst.MessageType.QOS:
            self.qos_message(message)

    def qos_message(self, message):
        if self.qos_warned:
            return
        Gst = self.Gst
        fmt, _processed, dropped = message.parse_qos_stats()
        if fmt != Gst.Format.BUFFERS or dropped in (0, Gst.CLOCK_TIME_NONE):
            return
        factory = (message.src.get_factory()
                   if isinstance(message.src, Gst.Element) else None)
        if factory is None or "Video" not in (factory.get_metadata("klass") or ""):
            return
        self.qos_warned = True
        # QoS counters can reset on seeks and state changes. Do not add their
        # snapshots, or turn the decoded-pad count into a presentation count.
        print("crystalhd-play: warning: late video buffers were dropped to keep up "
              "with playback; the decoded-frame counter is not a displayed-frame count.",
              file=sys.stderr, flush=True)
        if self.args.software_output_size:
            print("For lower-cost display fitting, keep --scale-width and omit "
                  "--software-output-size.", file=sys.stderr, flush=True)

    def set_state(self, state, failure):
        result = self.pipeline.set_state(state)
        if result == self.Gst.StateChangeReturn.FAILURE:
            self.fail(failure)
            return False
        if result == self.Gst.StateChangeReturn.NO_PREROLL:
            self.is_live = True
        return True

    def buffering_changed(self, percent):
        if self.is_live:
            return
        buffering = percent < 100
        if self.buffering == buffering:
            return
        self.buffering = buffering
        # Only a human pause suspends the health timeout. Repeated BUFFERING
        # messages must not renew startup/stall grace indefinitely.
        if not self.health.paused:
            state = self.Gst.State.PAUSED if buffering else self.Gst.State.PLAYING
            if not self.set_state(state, "buffering state change failed"):
                return
            if self.is_live and buffering:
                self.buffering = False
                self.set_state(self.Gst.State.PLAYING, "live playback resume failed")

    def seek(self, offset=0.0, rate=None):
        Gst = self.Gst
        if self.seek_target is not None:
            position, current_rate = self.queued_seek or self.seek_target
        else:
            if not self.can_seek():
                return
            ok, position = self.pipeline.query_position(Gst.Format.TIME)
            if not ok:
                print("Cannot seek: playback position is unavailable.", file=sys.stderr)
                return
            current_rate = self.rate
        wanted = current_rate if rate is None else rate
        position = max(0, position + int(offset * Gst.SECOND))
        ok, duration = self.pipeline.query_duration(Gst.Format.TIME)
        if ok and duration > 0:
            position = min(position, max(0, duration - 1))
        # A flushing seek temporarily makes position queries unavailable.
        # Coalesce controls against the requested destination until preroll
        # finishes instead of issuing overlapping flushes or losing keys.
        if self.seek_target is not None:
            self.queued_seek = (position, wanted)
            return
        self.submit_seek(position, wanted)

    def can_seek(self):
        Gst = self.Gst
        if isinstance(self.args.file, str):
            query = Gst.Query.new_seeking(Gst.Format.TIME)
            if not self.pipeline.query(query):
                print("Cannot seek: HTTP seekability is not available yet; "
                      "playback settings are unchanged.", file=sys.stderr)
                return False
            if not query.parse_seeking()[1]:
                print("Cannot seek: this HTTP source is not currently seekable; "
                      "playback settings are unchanged.", file=sys.stderr)
                return False
        return True

    def submit_seek(self, position, wanted):
        Gst = self.Gst
        # Recheck after an earlier flushing seek: HTTP seekability can change.
        if not self.can_seek():
            return
        accepted = self.pipeline.seek(wanted, Gst.Format.TIME,
                                      Gst.SeekFlags.FLUSH | Gst.SeekFlags.ACCURATE,
                                      Gst.SeekType.SET, position, Gst.SeekType.NONE, -1)
        if not accepted:
            print("Seek/rate change was rejected; playback settings are unchanged.", file=sys.stderr)
            return
        self.rate = wanted
        self.seek_target = (position, wanted)
        self.health.seeking(time.monotonic())
        print(f"Requested position {position / Gst.SECOND:.1f}s, rate {wanted:g}x", flush=True)

    def seek_completed(self):
        if self.seek_target is None:
            return
        # An older startup/pause completion can still be queued on the bus.
        # It must not complete a newer seek whose preroll is still pending.
        state, _current, _pending = self.pipeline.get_state(0)
        if state == self.Gst.StateChangeReturn.ASYNC:
            return
        if state == self.Gst.StateChangeReturn.FAILURE:
            self.fail("seek preroll state change failed")
            return
        self.seek_target = None
        queued, self.queued_seek = self.queued_seek, None
        if queued is not None:
            self.submit_seek(*queued)

    def control(self, action):
        if action == "quit":
            self.stop(self.health.finish())
        elif action == "pause":
            paused = not self.health.paused
            state = self.Gst.State.PAUSED if paused or self.buffering else self.Gst.State.PLAYING
            if self.set_state(state, "pause/resume state change failed"):
                self.health.pause(paused, time.monotonic())
                print("Paused" if paused else "Buffering" if self.buffering else "Playing", flush=True)
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
        if self.pipeline.set_state(self.Gst.State.NULL) == self.Gst.StateChangeReturn.FAILURE:
            print("crystalhd-play: decoder teardown failed", file=sys.stderr)
            self.exit_code = max(self.exit_code, 1)
        for workaround in tuple(self.deinterlace_qos.values()):
            workaround.close()
        self.deinterlace_qos.clear()

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
            # NO_PREROLL on PAUSED identifies a live source before deciding
            # whether subsequent BUFFERING messages should pause its clock.
            if self.set_state(self.Gst.State.PAUSED, "could not preroll playback"):
                self.set_state(self.Gst.State.PLAYING, "could not start playback")
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
    if args.settings_report:
        print(args.settings_report, flush=True)
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
        print(f"crystalhd-play: {input_error(args.file, str(error))}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
