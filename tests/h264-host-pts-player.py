#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Bounded #533 research player, separate from installed Player/Kodi/VA.

Actual card pictures arrive over an owned-copy pipe after RX release. A
live input/token mapper supplies saved source PTS; private frames stay hidden.
The same MP4's AAC uses an unmodified GStreamer audio branch/common clock.
This is a single180-picture progressive fixture test, not a supported player.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import queue
import selectors
import signal
import struct
import subprocess
import threading
import time

import gi
gi.require_version('Gst', '1.0')
gi.require_version('GstVideo', '1.0')
from gi.repository import GLib, Gst, GstVideo

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('hostmap', ROOT/'tests/h264-host-pts-map.py')
H = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(H)
BYTES = 640*360*2
HEADER = struct.Struct('<8sIQIIII')


def validate_video_probe(probe, original):
    stream = probe['streams'][0]
    if stream.get('codec_name') != 'h264' or (stream.get('width'), stream.get('height')) != (640, 360) or stream.get('time_base') != original['source_time_base']:
        raise H.Unsupported('media video codec/geometry/timebase differs from qualified input')
    packets = probe['packets']
    if len(packets) != 180 or len(original['original_aus']) != 180 or not all(
            p['pts'] == h['pts'] and p['dts'] == h['dts'] and p.get('duration', 0) > 0
            for p, h in zip(packets, original['original_aus'])):
        raise H.Unsupported('media video timestamp/duration descriptors differ')
    return packets


def validate_media(media, original, manifest=None):
    actual = hashlib.sha256(media.read_bytes()).hexdigest()
    expected = original['source_fixture_sha256']
    if manifest is not None:
        record = json.loads(manifest.read_text())
        # A separately qualified, video-identical A/V remux is permitted only
        # through its explicit bound descriptor; the player never creates it.
        if not record.get('video_packets_identical') or not record.get('audio_prefix_packets_identical') or record.get('qualified_source_fixture_sha256') != expected or record.get('source_time_base') != original['source_time_base'] or record.get('video_packet_count') != 180:
            raise H.Unsupported('A/V manifest does not bind this qualified video source')
        expected = record.get('media_sha256')
    if actual != expected:
        raise H.Unsupported('selected media identity differs from qualified source/manifest')
    probe = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'v:0',
        '-show_packets', '-show_streams', '-show_entries',
        'stream=codec_name,width,height,time_base:packet=pts,dts,duration', '-of', 'json',
        str(media)], capture_output=True, text=True, check=True, timeout=10)
    return validate_video_probe(json.loads(probe.stdout), original)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('inputs', 'oracle', 'media', 'results'):
        parser.add_argument(name, type=Path)
    parser.add_argument('--probe', required=True, type=Path)
    parser.add_argument('--library', required=True, type=Path)
    parser.add_argument('--headless', action='store_true')
    parser.add_argument('--media-manifest', type=Path,
        help='Explicit descriptor for a separately verified video-identical A/V remux')
    args = parser.parse_args()
    args.results.mkdir(mode=0o700, exist_ok=False)
    (args.results/'native-cycle-reference.bin').symlink_to(args.oracle.resolve())
    original = json.loads((args.inputs/'offline.json').read_text())
    packets = validate_media(args.media, original, args.media_manifest)
    time_base = tuple(map(int, original['source_time_base'].split('/')))
    provided, durations = [], {}
    def provider(ordinal):
        if ordinal != len(provided)+1 or ordinal > 180:
            raise H.Unsupported('source descriptor requested before accepted input')
        provided.append(ordinal)
        h = original['original_aus'][ordinal-1]
        durations[ordinal] = packets[ordinal-1]['duration']
        return H.Source(ordinal, ordinal*100000, h['pts'], h['dts'], h['poc_lsb'],
            h['frame_num'], h['reference'], h['idr'], time_base, h.get('gop_id', 0), 1)
    mapper = H.TraceMapper(provider, generation='clock-test', capacity=4096)
    env = dict(os.environ, CRYSTALHD_LEGACY_LIB=str(args.library.resolve()),
        LD_LIBRARY_PATH=str(args.library.resolve().parent), CRYSTALHD_CONFIG_DISABLE='1')
    session = subprocess.check_output(['systemctl', '--user', 'show-environment'], text=True)
    for item in session.splitlines():
        key, _, value = item.partition('=')
        if key in ('DISPLAY', 'XAUTHORITY', 'XDG_RUNTIME_DIR', 'DBUS_SESSION_BUS_ADDRESS'):
            os.environ.setdefault(key, value); env.setdefault(key, value)
    Gst.init(None)
    sink_name = 'fakesink' if args.headless else 'xvimagesink'
    # Load GstVideo before constructing an overlay implementer: importing it
    # afterwards leaves a methodless cached GI interface on this runtime.
    # This display exposes UYVY but not native YUY2; packing conversion does
    # not change source dimensions, timestamps or decoded-picture ownership.
    packing = '' if args.headless else '! videoconvert n-threads=1 ! video/x-raw,format=UYVY '
    pipeline = Gst.parse_launch(
        'appsrc name=video format=time block=true max-buffers=8 max-bytes=3686400 '
        '! queue max-size-buffers=8 max-size-bytes=0 max-size-time=0 '
        +packing+'! '+sink_name+' name=display sync=true qos=true '
        'filesrc name=audio_file ! qtdemux name=demux '
        'queue name=audio_queue ! aacparse ! avdec_aac ! audioconvert '
        '! audioresample ! pulsesink name=audio sync=true')
    video = pipeline.get_by_name('video'); sink = pipeline.get_by_name('display')
    audio = pipeline.get_by_name('audio'); audio.set_property('mute', args.headless)
    pipeline.get_by_name('audio_file').set_property('location', str(args.media.resolve()))
    caps = Gst.Caps.from_string('video/x-raw,format=YUY2,width=640,height=360,framerate='+original['source_frame_rate'])
    video.set_property('caps', caps)
    def audio_pad(demux, pad):
        caps = pad.get_current_caps() or pad.query_caps(None)
        if caps and caps.get_size() == 1 and caps.get_structure(0).get_name() == 'audio/mpeg':
            target = pipeline.get_by_name('audio_queue').get_static_pad('sink')
            if not target.is_linked():
                if pad.link(target) != Gst.PadLinkReturn.OK:
                    fail('AAC dynamic pad link failed')
    pipeline.get_by_name('demux').connect('pad-added', audio_pad)
    window = None
    if not args.headless:
        gi.require_version('Gtk', '3.0'); gi.require_version('GdkX11', '3.0')
        from gi.repository import GdkX11, Gtk
        window = Gtk.Window(title='CrystalHD PTS playback test')
        window.set_default_size(800, 450)
        drawing = Gtk.DrawingArea(); window.add(drawing); window.show_all()
        while Gtk.events_pending():
            Gtk.main_iteration_do(False)
        xid = GdkX11.X11Window.get_xid(drawing.get_window())
        GstVideo.VideoOverlay.set_window_handle(sink, xid)
    stop = threading.Event(); frames = queue.Queue(maxsize=8)
    loop = GLib.MainLoop(); errors, events, projections, handoffs = [], [], [], []
    result = {'scope': 'Actual native copied pictures + live source-PTS mapping + AAC/common-clock research test. No installed/frontend change, raw token rewrite, calibrated physical A/V or E2E claim.',
        'headless': args.headless, 'audio_muted_per_stream': args.headless,
        'source_time_base': list(time_base), 'video_frames_pushed': 0,
        'private_pictures_hidden': 0, 'natural_eos': False}
    def fail(message):
        errors.append(str(message)); GLib.idle_add(loop.quit)
    if window:
        window.connect('destroy', lambda unused: loop.quit())
        def key(unused, event):
            from gi.repository import Gdk
            if event.keyval in (Gdk.KEY_Escape, Gdk.KEY_q):
                loop.quit()
            elif event.keyval == Gdk.KEY_F11:
                if window.get_window().get_state() & Gdk.WindowState.FULLSCREEN:
                    window.unfullscreen()
                else:
                    window.fullscreen()
        window.connect('key-press-event', key)
    if args.headless:
        sink.set_property('signal-handoffs', True)
        def handoff(element, buffer, pad):
            clock = pipeline.get_clock()
            now = clock.get_time()-pipeline.get_base_time() if clock else None
            handoffs.append({'pts_ns': buffer.pts, 'running_time_ns': now})
        sink.connect('handoff', handoff)
    def message(bus, msg):
        if msg.type == Gst.MessageType.ERROR:
            error, detail = msg.parse_error(); fail(str(error)+' '+str(detail))
        elif msg.type == Gst.MessageType.EOS:
            result['natural_eos'] = True; loop.quit()
    bus = pipeline.get_bus(); bus.add_signal_watch(); bus.connect('message', message)
    read_fd, write_fd = os.pipe()
    proc = subprocess.Popen([str(args.probe), str(args.inputs), str(args.results),
        'cycles-marker-export', '2', str(write_fd)], env=env, pass_fds=(write_fd,),
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0, start_new_session=True)
    os.close(write_fd)
    (args.results/'active.private.json').write_text(json.dumps({'native_pid': proc.pid})+'\n')
    def put(item):
        while not stop.is_set():
            try:
                frames.put(item, timeout=.1); return
            except queue.Full:
                pass
        raise RuntimeError('consumer stopped')
    def read_native():
        selector = selectors.DefaultSelector()
        selector.register(proc.stdout, selectors.EVENT_READ, 'json')
        selector.register(read_fd, selectors.EVENT_READ, 'pixels')
        buffers = {'json': b'', 'pixels': b''}; indexed = {}; expected_index = 0
        try:
            with (args.results/'native.private.log').open('xb') as log:
                def text_line(data):
                    log.write(data+b'\n'); log.flush()
                    if not data.startswith(b'{'):
                        return
                    event = json.loads(data); events.append(event)
                    if event['type'] == 'cycle-input':
                        kind = 'early-prefix' if event['kind'] == 'reserved-original-prefix' else event['kind']
                        mapper.consume(dict(event, type='input-call', kind=kind, status=0))
                    elif event['type'] == 'cycle-picture':
                        index = len(projections)
                        mapped = mapper.consume({'type': 'pts-attribution', 'actual_token': event['token'], 'flags': event['flags']}, index)
                        if mapper.failed or mapped is None:
                            raise RuntimeError('live metadata mapping rejected: '+str(mapper.failed))
                        mapped['raw_picture_flags'] = event['flags']
                        projections.append(mapped); indexed[index] = mapped
                while selector.get_map() and not stop.is_set():
                    for key, unused in selector.select(.1):
                        data = os.read(key.fd, 65536)
                        if not data:
                            selector.unregister(key.fileobj); continue
                        kind = key.data; buffers[kind] += data
                        if kind == 'json':
                            while b'\n' in buffers[kind]:
                                line, buffers[kind] = buffers[kind].split(b'\n', 1); text_line(line)
                    while len(buffers['pixels']) >= HEADER.size:
                        magic, size, token, flags, index, width, height = HEADER.unpack_from(buffers['pixels'])
                        if magic != b'CHDPIC01' or size != BYTES or (width, height) != (640, 360) or index != expected_index:
                            raise RuntimeError('invalid owned-frame pipe record')
                        if len(buffers['pixels']) < HEADER.size+size or index not in indexed:
                            break
                        data = buffers['pixels'][HEADER.size:HEADER.size+size]
                        buffers['pixels'] = buffers['pixels'][HEADER.size+size:]
                        mapped = indexed.pop(index)
                        if mapped['raw_native_token'] != token or mapped['raw_picture_flags'] != flags:
                            raise RuntimeError('metadata/pixel native-owner mismatch')
                        put((mapped, data)); expected_index += 1
                if not stop.is_set():
                    if buffers['json']:
                        text_line(buffers['json']); buffers['json'] = b''
                    if buffers['pixels'] or indexed:
                        raise RuntimeError('incomplete copied-frame/metadata stream')
                    if proc.wait(timeout=3) != 0:
                        raise RuntimeError('native qualification probe failed')
                    put(None)
        except BaseException as error:
            if not stop.is_set():
                fail(error)
        finally:
            selector.close(); os.close(read_fd)
    def feed():
        try:
            while not stop.is_set():
                try:
                    item = frames.get(timeout=.1)
                except queue.Empty:
                    continue
                if item is None:
                    video.emit('end-of-stream'); return
                mapped, pixels = item
                if mapped['kind'] == 'private':
                    result['private_pictures_hidden'] += 1; continue
                if mapped['effective_pts'] is None or mapped['effective_pts'] < 0:
                    raise RuntimeError('this display prototype needs a nonnegative source PTS')
                buffer = Gst.Buffer.new_wrapped(pixels)
                num, den = mapped['time_base']
                buffer.pts = mapped['effective_pts']*Gst.SECOND*num//den
                buffer.duration = durations[mapped['au_id']]*Gst.SECOND*num//den
                if video.emit('push-buffer', buffer) != Gst.FlowReturn.OK:
                    if stop.is_set():
                        return
                    raise RuntimeError('video appsrc rejected a copied frame')
                result['video_frames_pushed'] += 1
        except BaseException as error:
            if not stop.is_set():
                fail(error)
    reader = threading.Thread(target=read_native, daemon=True)
    feeder = threading.Thread(target=feed, daemon=True)
    reader.start(); feeder.start()
    GLib.timeout_add_seconds(25, lambda: (fail('bounded player watchdog'), False)[1])
    try:
        if pipeline.set_state(Gst.State.PLAYING) == Gst.StateChangeReturn.FAILURE:
            raise RuntimeError('pipeline could not play')
        loop.run()
    finally:
        # Snapshot counters before NULL resets BaseSink statistics.
        for name, element in [('video', sink), ('audio', audio)]:
            stats = element.get_property('stats')
            result[name+'_sink_stats'] = {key: stats.get_value(key) for key in ('rendered', 'dropped')} if stats else None
        clock = pipeline.get_clock(); provided_clock = audio.provide_clock()
        result['clock_name'] = clock.get_name() if clock else None
        result['provided_audio_clock_name'] = provided_clock.get_name() if provided_clock else None
        result['uses_audio_sink_clock'] = bool(clock and provided_clock and clock == provided_clock)
        stop.set(); pipeline.set_state(Gst.State.NULL); pipeline.get_state(3*Gst.SECOND)
        reader.join(timeout=3); feeder.join(timeout=3)
        if proc.poll() is None:
            os.kill(proc.pid, signal.SIGTERM); proc.wait(timeout=10)
        bus.remove_signal_watch()
        if window:
            window.destroy()
        result.update(native_exit=proc.returncode, errors=errors,
            native_result=next((e for e in events if e['type'] == 'cycle-result'), None),
            mapping=mapper.snapshot(), observed_handoffs=handoffs,
            threads_retired=not reader.is_alive() and not feeder.is_alive())
        (args.results/'result.private.json').write_text(json.dumps(result, indent=2)+'\n')
    good = not errors and result['natural_eos'] and result['video_frames_pushed'] == 180 and result['private_pictures_hidden'] == 10 and result['native_exit'] == 0 and result['threads_retired'] and result['uses_audio_sink_clock'] and result['video_sink_stats']['rendered'] == 180 and result['video_sink_stats']['dropped'] == 0 and result['audio_sink_stats']['rendered'] > 0
    print(json.dumps({k: v for k, v in result.items() if k not in ('mapping', 'observed_handoffs')}, indent=2))
    return 0 if good else 1


if __name__ == '__main__':
    raise SystemExit(main())
