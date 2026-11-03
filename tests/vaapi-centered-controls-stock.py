#!/usr/bin/python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Explicit stock centered seek/Stop/reopen probe; OWN diagnostics, no release claim."""
import argparse
from collections import Counter, defaultdict
from fractions import Fraction
import importlib.util
import json
import os
from pathlib import Path
import re
import resource
import select
import signal
import socket
import stat
import subprocess
import sys
import tempfile
import time
from xml.sax.saxutils import escape
sys.dont_write_bytecode = True

DIRECTORY = Path(__file__).resolve().parent
STOCK_PATH = DIRECTORY / 'vaapi-centered-payload-stock.py'
def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module); return module
S = load('centered_controls_stock_primitives', STOCK_PATH)
require, save, digest, fault = S.require, S.save, S.digest, S.fault
READER_PATH = DIRECTORY / 'vaapi-centered-controls-reader.py'
PAYLOAD_READER = DIRECTORY / 'vaapi-centered-payload-reader.py'
FRAME_PTS_READER = DIRECTORY / 'vaapi-frame-pts-reader.py'
FRAME_PTS_IMAGE = Path('filters/vaapi/frame-pts-observer/frame-pts-probe.so')
FRAME_PTS_MOUNT = '/own-observer/frame-pts-probe.so'
ROOT = Path('/home/mache/.cache/crystalhd-centered-seek-20261103')
FIXTURE = ROOT / 'frozen-1080p180/neutral-spatial-1080p30-180.mp4'
FIXTURE_SHA = '226c91b7f6fd361af37fcac88dfa23302072f58f7cce0b8aa0c57c3f94f06555'
PROBE = ROOT / 'frozen-1080p180/encoded-metadata.json'
MANIFEST = ROOT / 'qualified-1080p180/manifest.json'
COORDINATES = ROOT / 'coordinate-contract.json'
ORIGIN = ROOT / 'preparation-origin.json'
SCOPE = ('Original Kodi22RC1/App FFmpeg, OWN centered observer, method0/actual none; '
         'source-content checkpoints, forward seek, requested Stop and same-PID natural reopen. '
         'Private diagnostics do not qualify normal-backend compatibility, release, actual AVFrame PTS or GPU return.')
SAMPLE_MODES = ('first-two', 'case2-90-91')
RENDER_ROUTES = ('va', 'cpu-copy')
CPU_COPY_PROVIDER = Path('lib/kodi/system/libsse4-x86_64-linux.so')
CPU_COPY_MOUNT = '/app/' + str(CPU_COPY_PROVIDER)
MAX_ROUTE_LOG_BYTES = 32 * 1024 * 1024
KODI_LOG_LINE = re.compile(rb'\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3} T:([1-9][0-9]*)\s+'
                           rb'(trace|debug|info|warning|error|critical) <([^>\r\n]+)>: (.*)')
CPU_COPY_INIT = b'VAAPI output: Initializing ffmpeg postproc'
CPU_COPY_READY = b'VAAPI::CFFmpegPostproc::Init  - skip deinterlacing'
CPU_COPY_SCOPE = ('Original Kodi22RC1/App FFmpeg, explicit OWN centered observer; requested Off CPU-copy route. '
                  'Actual ffmpeg-postproc selection, selected source payloads and ordinary controls are checked. '
                  'No normal-backend stock compatibility, release, GPU return or post-filter cadence qualification.')
FRAME_PTS_SCOPE = ('Instrumented original Kodi22RC1/App FFmpeg with an explicit OWN receive-frame observer; '
                   'actual returned-frame metadata joined to OWN active VA bindings. '
                   'No stock compatibility, release, GPU return or scanout qualification.')


def validate_render_route(route):
    require(type(route) is str and route in RENDER_ROUTES, 'missing or invalid frozen render route')
    return route


def render_route_option(prepare, supplied):
    if prepare:return validate_render_route('va' if supplied is None else supplied)
    require(supplied is None, '--render-route belongs only to --prepare-plan')
    return None


def render_route_settings(route):
    validate_render_route(route)
    if route == 'va':return {'videoplayer.prefervaapirender': 'true'}
    return {'videoplayer.prefervaapirender': 'false', 'debug.extralogging': 'true',
            'debug.setextraloglevel': '32768'}


def render_route_files(app, route):
    return [] if validate_render_route(route) == 'va' else [app / CPU_COPY_PROVIDER]


def scenario_scope(route, observe_frame_pts):
    validate_frame_pts_mode(observe_frame_pts)
    if validate_render_route(route) == 'cpu-copy':return CPU_COPY_SCOPE
    return FRAME_PTS_SCOPE if observe_frame_pts else SCOPE


def qualify_cpu_copy_settings(settings):
    require(type(settings) is dict and
            type(settings.get('videoplayer.prefervaapirender')) is bool and
            settings['videoplayer.prefervaapirender'] is False and
            type(settings.get('debug.extralogging')) is bool and settings['debug.extralogging'] is True and
            type(settings.get('debug.setextraloglevel')) is list and
            len(settings['debug.setextraloglevel']) == 1 and
            type(settings['debug.setextraloglevel'][0]) is int and settings['debug.setextraloglevel'][0] == 32768,
            'actual CPU-copy/logcomponent settings differ from frozen route')
    return settings


def cpu_copy_mapping(pid, app, pins):
    """Verify the original SIMD copy provider at a held identity checkpoint."""
    expected = pins[str(app / CPU_COPY_PROVIDER)]; proc = Path('/proc', str(pid)); found = set()
    for line in (proc / 'maps').read_text().splitlines():
        parts = line.split(None, 5)
        if len(parts) == 6 and parts[5] == CPU_COPY_MOUNT:found.add((parts[3], int(parts[4])))
    require(len(found) == 1, 'original CPU-copy provider mapping missing or ambiguous')
    device, inode = next(iter(found)); target = proc / 'root' / CPU_COPY_MOUNT.lstrip('/'); st = target.stat()
    require(st.st_ino == inode and (os.major(st.st_dev), os.minor(st.st_dev)) ==
            tuple(int(part, 16) for part in device.split(':')) and digest(target) == expected,
            'mapped original CPU-copy provider differs from frozen artifact')
    return {'role': 'original-cpu-copy-library', 'path': CPU_COPY_MOUNT,
            'device': device, 'inode': inode, 'sha256': expected}


def read_route_log(path):
    """Bound a regular fresh-profile log; keep appends outside a frozen prefix."""
    with os.fdopen(os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW), 'rb') as stream:
        identity = os.fstat(stream.fileno())
        require(stat.S_ISREG(identity.st_mode) and 0 <= identity.st_size <= MAX_ROUTE_LOG_BYTES,
                'bounded regular original route log required')
        raw = stream.read(MAX_ROUTE_LOG_BYTES + 1)
    require(len(raw) <= MAX_ROUTE_LOG_BYTES, 'original route log exceeded bound')
    return raw, {'device': identity.st_dev, 'inode': identity.st_ino}


def route_log_start(path, owned_pid):
    raw, identity = read_route_log(path)
    require(type(owned_pid) is int and owned_pid > 0, 'owned route-log PID must be positive integer')
    first = KODI_LOG_LINE.fullmatch(raw.split(b'\n', 1)[0].removeprefix(b'\xef\xbb\xbf'))
    require(first is not None and int(first[1]) == owned_pid,
            'original route log startup thread differs from owned application PID')
    begin = raw.rfind(b'\n') + 1
    require(begin > 0, 'original route log has no complete startup record')
    return dict(identity, begin=begin, owned_pid=owned_pid)


def audit_cpu_copy_route_log(raw, begin, end, owned_pid):
    """Require completed original Off CPU-copy initialization in one play."""
    require(type(raw) is bytes and len(raw) <= MAX_ROUTE_LOG_BYTES, 'bounded original route-log bytes required')
    require(type(begin) is int and type(end) is int and 0 < begin < end <= len(raw) and
            raw[begin - 1:begin] == raw[end - 1:end] == b'\n', 'route-log window is incomplete or misaligned')
    require(type(owned_pid) is int and owned_pid > 0, 'owned route-log PID must be positive integer')
    first = KODI_LOG_LINE.fullmatch(raw.split(b'\n', 1)[0].removeprefix(b'\xef\xbb\xbf'))
    require(first is not None and int(first[1]) == owned_pid,
            'original route log startup thread differs from owned application PID')
    pending, pairs = defaultdict(list), []
    offset = begin
    for line in raw[begin:end].splitlines(keepends=True):
        match = KODI_LOG_LINE.fullmatch(line[:-1])
        if match:
            tid, level, component, message = int(match[1]), match[2], match[3], match[4]
            reject = (message.startswith((b'VAAPI output: Initializing skip postproc',
                        b'VAAPI output: Initializing vaapi postproc', b'VAAPI output: Postproc preinit failed',
                        b'VAAPI/ffpp error:')) or
                      (message.startswith(b'VAAPI::CFFmpegPostproc::Init') and message != CPU_COPY_READY))
            require(not reject, 'original route log reports another postproc path or CPU-copy failure')
            if message in (CPU_COPY_INIT, CPU_COPY_READY):
                require(level == b'debug' and component == b'video', 'CPU-copy marker lacks original video debug context')
                if message == CPU_COPY_INIT:pending[tid].append(offset)
                else:
                    require(pending[tid], 'CPU-copy completion lacks its same-thread initialization')
                    pairs.append({'tid': tid, 'initializing_offset': pending[tid].pop(0), 'completed_offset': offset})
        offset += len(line)
    require(pairs and not any(pending.values()), 'one complete actual CPU-copy initialization is required in each play')
    return {'status': 'PASS', 'render_route': 'cpu-copy', 'method': 0, 'actual_deintmethod': 'none',
            'owned_pid': owned_pid, 'begin': begin, 'end': end, 'completed_initializations': pairs,
            'scope': 'Original same-process log records explicit ffmpeg postproc and completed Off CPU-copy Init; worker IDs are thread IDs.'}


def audit_cpu_copy_route_file(path, started):
    raw, identity = read_route_log(path)
    require(identity == {key: started[key] for key in ('device', 'inode')}, 'original route log identity changed')
    end = raw.rfind(b'\n') + 1
    return dict(audit_cpu_copy_route_log(raw, started['begin'], end, started['owned_pid']),
                path=str(path), file_identity=identity)


def frame_pts_option(prepare, supplied):
    require(type(supplied) is bool, 'frame PTS option must be boolean')
    require(prepare or not supplied, '--observe-frame-pts belongs only to --prepare-plan')
    return supplied if prepare else None


def validate_frame_pts_mode(mode):
    require(type(mode) is bool, 'missing or invalid frozen frame PTS mode')
    return mode


def frame_pts_environment(source, enabled):
    validate_frame_pts_mode(enabled)
    if not enabled:return []
    return ['--ro-bind', str(source / FRAME_PTS_IMAGE), FRAME_PTS_MOUNT,
            '--setenv', 'LD_PRELOAD', FRAME_PTS_MOUNT,
            '--setenv', 'CRYSTALHD_TEST_FRAME_PTS', 'v1',
            '--setenv', 'CRYSTALHD_TEST_FRAME_PTS_PREFIX', '/payload/frame-pts']


def frame_pts_report_path(payload_directory, owned_pid):
    require(type(owned_pid) is int and owned_pid > 0, 'owned frame PTS PID must be positive integer')
    return payload_directory / ('frame-pts-' + str(owned_pid) + '.jsonl')


def frame_pts_mapping(pid, source, pins):
    """Verify the sole injected observer image once at the held content checkpoint."""
    found = set(); proc = Path('/proc', str(pid)); expected = pins[str(source / FRAME_PTS_IMAGE)]
    for line in (proc / 'maps').read_text().splitlines():
        parts = line.split(None, 5)
        if len(parts) != 6 or parts[5] != FRAME_PTS_MOUNT:continue
        found.add((parts[3], int(parts[4])))
    require(len(found) == 1, 'explicit frame PTS observer image missing or ambiguous')
    device, inode = next(iter(found)); target = proc / 'root' / FRAME_PTS_MOUNT.lstrip('/'); st = target.stat()
    require(st.st_ino == inode and (os.major(st.st_dev), os.minor(st.st_dev)) ==
            tuple(int(part, 16) for part in device.split(':')) and digest(target) == expected,
            'mapped frame PTS observer differs from frozen own artifact')
    return {'path': FRAME_PTS_MOUNT, 'device': device, 'inode': inode, 'sha256': expected}


def audit_frame_pts(reader, report_path, trace_path, payload_records, owned_pid):
    require(type(owned_pid) is int and owned_pid > 0, 'owned frame PTS PID must be positive integer')
    selected = []
    for record in payload_records:
        require(record['audit']['status'] == 'PASS', 'frame PTS needs independently qualified source payloads')
        instance = record['audit']['payload_audit']['driver_instance']
        selected.extend(dict(item, driver_instance=instance, phase=record['phase'], pid=owned_pid)
                        for item in record['audit']['selected_source_bindings'])
    result = reader.audit_frames(report_path, trace_path, selected_bindings=selected)
    require(result.get('status') == 'PASS', 'actual AVFrame PTS binding audit failed: ' + json.dumps(result))
    require(type(result.get('pid')) is int and result['pid'] == owned_pid,
            'frame PTS report PID differs from the verified owned application')
    require(type(result['selected_joins']) is list and len(result['selected_joins']) == len(selected),
            'selected frame PTS population differs')
    natural = {(row['driver_instance'], row['binding']['context'], row['binding']['generation'])
               for row in selected if row['phase'] == 'reopen'}
    require(len(natural) == 1, 'one source-proven natural reopen lifetime required')
    require(type(result['lifetime_counts']) is list, 'frame PTS lifetime census must be an array')
    counts = [row for row in result['lifetime_counts'] if
              (row['driver_instance'], row['context'], row['generation']) in natural]
    require(len(counts) == 1 and all(type(counts[0][key]) is int and counts[0][key] == 180
            for key in ('frame_returns', 'unique_bindings', 'raw_pts_observed')),
            'natural reopen needs180 distinct returned frames with actual raw PTS observations')
    require(type(counts[0]['clock_observed']) is int and 0 <= counts[0]['clock_observed'] <= 180,
            'natural declared-clock census must remain explicit and bounded')
    return result


def validate_sample_mode(mode):
    require(mode in SAMPLE_MODES, 'unknown or missing frozen sample mode')
    return mode


def sample_option(prepare, supplied):
    if prepare:return validate_sample_mode('first-two' if supplied is None else supplied)
    require(supplied is None, '--sample-mode belongs only to --prepare-plan')
    return None


def sample_environment(mode):
    validate_sample_mode(mode)
    return [] if mode == 'first-two' else ['--setenv', 'CRYSTALHD_TEST_CENTERED_PAYLOAD_SELECT', mode]


def validate_selection(document, mode):
    validate_sample_mode(mode)
    if mode == 'case2-90-91':require(document.get('selection') == mode, 'target selection differs from frozen mode')
    else:require('selection' not in document, 'baseline unexpectedly carries a target selection')


def audit_payload_for_mode(reader, manifest, trace, mode):
    validate_sample_mode(mode)
    if mode == 'case2-90-91':return reader.audit_payload(manifest, trace, required_targets=(90, 91))
    return reader.audit_payload(manifest, trace)


def audit_target_phase(reader, records, seek_request_ns, stop_request_ns):
    require(type(seek_request_ns) is int and type(stop_request_ns) is int and
            0 < seek_request_ns < stop_request_ns, 'target seek/Stop request interval differs')
    content = [item for record in records for item in record['audit']['selected_source_bindings']]
    require(len(content) == 2 and Counter(item['source_ordinal'] for item in content) == Counter((90, 91)),
            'interrupted play needs exactly two source-proven target bindings90/91')
    windows = []
    for record in records:
        require(record['audit']['status'] == 'PASS', 'target payload is not independently qualified')
        result = reader.audit_target_window(record['audit'], record['trace_prefix'], seek_request_ns, stop_request_ns)
        windows.append({'manifest': record['manifest'], 'audit': result})
        require(result.get('status') == 'PASS', 'target binding timing audit failed: ' + json.dumps(result))
    return {'status': 'PASS', 'selected_target_bindings': 2, 'source_ordinals': [90, 91],
            'seek_request_ns': seek_request_ns, 'stop_request_ns': stop_request_ns, 'window_audits': windows,
            'actual_media_PTS_to_VA_binding': 'NOT_OBSERVED'}


def qualify_target_reopen(census):
    require(census['status'] == 'PASS' and census['complete_natural_population_required'] is True and
            all(census['binding_stage_counts'].get(stage) == 180 for stage in
                ('submit_bind', 'input_sent', 'output_dequeued')),
            'target natural reopen requires180 original submit/input/output bindings')


def clock_seconds(value):
    return sum(Fraction(value.get(key, 0)) * factor for key, factor in
               (('hours', 3600), ('minutes', 60), ('seconds', 1), ('milliseconds', Fraction(1, 1000))))


def verify_capture(result, metadata, label):
    require(result.get('status') == 'PASS', 'independent source barcode failed: ' + json.dumps(result))
    require((metadata['format'], metadata['width'], metadata['height'], metadata['bytes']) ==
            ('BGRA', 1920, 1080, 1920 * 1080 * 4), 'capture geometry differs')
    require(type(result['source_ordinal']) is int and 0 <= result['source_ordinal'] < 180 and
            result.get('source_case') == 2 and result['source_time_base'] == '1/15360' and
            result['source_pts_ticks'] == result['source_ordinal'] * 512, 'fixture content clock differs')
    before, after = metadata['player_before'], metadata['player_after']
    require(before['speed'] == after['speed'] == 0, 'content capture is not paused')
    first, last = clock_seconds(before['time']), clock_seconds(after['time'])
    require(abs(last - first) <= Fraction(1, 20), 'capture pause clock did not hold')
    content = Fraction(result['source_pts_ticks'], 15360)
    require(abs(content - first) <= Fraction(1, 2) and abs(content - last) <= Fraction(1, 2),
            'barcode source content is not near the actual paused clock')
    if label == 'initial-pause':require(0 <= content < Fraction(5, 2), 'initial content too late for forward3s seek')
    elif label == 'seek-forward':require(Fraction(3) - Fraction(1, 30) <= content <= Fraction(7, 2), 'seek content outside declared target window')
    else:raise ValueError('unknown source checkpoint')
    return dict(result, label=label, paused_clock_seconds=[str(first), str(last)],
                content_clock_difference_seconds=[str(content - first), str(content - last)])


def source_metadata(document):
    streams = document['streams']; require(len(streams) == 1, 'one video stream required')
    v = streams[0]
    for key, expected in (('codec_type', 'video'), ('codec_name', 'h264'), ('width', 1920), ('height', 1080),
                          ('coded_width', 1920), ('coded_height', 1088), ('field_order', 'progressive'),
                          ('pix_fmt', 'yuv420p'), ('color_range', 'tv'), ('r_frame_rate', '30/1'),
                          ('time_base', '1/15360'), ('has_b_frames', 0)):
        require(v.get(key) == expected, 'source declaration differs: ' + key)
    both = document.get('packets_and_frames')
    packets = [r for r in both if r.get('type') == 'packet'] if both is not None else document['packets']
    frames = [r for r in both if r.get('type') == 'frame'] if both is not None else document['frames']
    require(len(packets) == len(frames) == 180, 'source requires180 packets/pictures')
    require(both is None or len(both) == len(packets) + len(frames), 'unknown interleaved probe object')
    ticks = [n * 512 for n in range(180)]
    require([int(r['pts']) for r in packets] == [int(r['dts']) for r in packets] ==
            [int(r['pts']) for r in frames] == ticks, 'independent packet/frame source clock differs')
    require(all(int(r['duration']) == 512 for r in packets), 'packet duration differs')
    require(all(r.get('interlaced_frame') == 0 for r in frames), 'fixture is not progressive')
    require(Path(document['format']['filename']).resolve() == FIXTURE and
            int(document['format']['size']) == 8931575, 'independent probe source association differs')
    return {'pictures': 180, 'packets': 180, 'fps': '30/1', 'coded': [1920, 1088], 'visible': [1920, 1080],
            'field_order': 'progressive', 'range': 'limited', 'source_time_base': '1/15360',
            'source_pts_ticks': ticks, 'actual_media_PTS_to_VA_binding': 'NOT_OBSERVED'}


def application_script(render_route='va'):
    validate_render_route(render_route)
    marker = '\ntry:\n    for cycle in range(1,CYCLES+1):'
    require(S.SCRIPT.count(marker) == 1, 'qualified application helper boundary changed')
    prefix = S.SCRIPT.split(marker)[0].replace('__CYCLES__', '1')
    callback = "    def onPlayBackError(self):event('OnError')"
    require(prefix.count(callback) == 1, 'qualified Player callback boundary changed')
    prefix = prefix.replace(callback, callback + "\n    def onPlayBackSeek(self,time,seekOffset):event('OnSeek',time_ms=time,seek_offset_ms=seekOffset)")
    controls = CONTROLS
    if render_route == 'cpu-copy':
        anchor = 'initial_pause_for_identity=True)'
        require(controls.count(anchor) == 2, 'qualified paused route identity boundary changed')
        controls = CPU_COPY_SETTINGS_SCRIPT + controls.replace(anchor,
            'initial_pause_for_identity=True,render_route_settings=render_route_settings())')
    return prefix.replace('centered-progress.json', 'centered-controls-progress.json') + controls


CPU_COPY_SETTINGS_SCRIPT = r'''
def render_route_settings():
    return {key:rpc('Settings.GetSettingValue',{'setting':key})['value'] for key in
        ('videoplayer.prefervaapirender','debug.extralogging','debug.setextraloglevel')}
'''


CONTROLS = r'''
state.update(scope='Forward3s seek, requested Stop, same-PID natural reopen; source-content proof only.',steps=[])
def seconds(v):return v.get('hours',0)*3600+v.get('minutes',0)*60+v.get('seconds',0)+v.get('milliseconds',0)/1000
def properties(pid):return rpc('Player.GetProperties',{'playerid':pid,'properties':['time','speed']})
def open_fixture():
    first=len(state['events']);item=xbmcgui.ListItem(path='/fixture.mp4');item.setContentLookup(False);player.play('/fixture.mp4',item)
    wait(lambda:any(e['event']=='OnAVStarted' for e in state['events'][first:]),12,'fresh AVStarted missing')
    active=rpc('Player.GetActivePlayers')
    if len(active)!=1 or active[0]['type']!='video' or player.getPlayingFile()!='/fixture.mp4':raise RuntimeError('sole active fixture identity differs')
    return active[0]['playerid'],first
def pause(pid):
    rpc('Player.PlayPause',{'playerid':pid,'play':False})
    wait(lambda:properties(pid)['speed']==0,2,'pause did not settle')
    before=properties(pid);end=time.monotonic()+.2
    while time.monotonic()<end:pump()
    after=properties(pid)
    if abs(seconds(after['time'])-seconds(before['time']))>.05:raise RuntimeError('pause clock did not hold')
    return {'before':before,'after':after}
def capture(label,pid,target=None):
    c=xbmc.RenderCapture();before=properties(pid);c.capture(1920,1080);pixels=c.getImage(1000)
    metadata={'file':label+'.bgra','format':c.getImageFormat(),'width':c.getWidth(),'height':c.getHeight(),
        'bytes':len(pixels),'player_before':before,'player_after':properties(pid),'monotonic_ns':time.monotonic_ns(),'target_seconds':target}
    if (metadata['format'],metadata['width'],metadata['height'],metadata['bytes'])!=('BGRA',1920,1080,1920*1080*4):raise RuntimeError('capture format/extent differs')
    with (R/metadata['file']).open('xb') as output:output.write(pixels)
    state['steps'].append({'action':'capture','label':label,'capture':metadata})
    gate('capture-'+label,'await-capture-'+label,capture=metadata,capture_label=label)
try:
    pid,first=open_fixture()
    wait(lambda:properties(pid)['speed']==1 and seconds(properties(pid)['time'])>=.15,2,'initial normal-speed progress missing')
    held=pause(pid)
    if seconds(held['after']['time'])>=2.5:raise RuntimeError('initial hold too late for forward3s seek')
    state['steps'].append({'action':'initial-pause','hold':held})
    gate('initial-ready','await-initial-ready',source='/fixture.mp4',info=info(),initial_pause_for_identity=True)
    capture('initial-pause',pid)
    start=seconds(properties(pid)['time']);rpc('Player.PlayPause',{'playerid':pid,'play':True})
    wait(lambda:properties(pid)['speed']==1 and seconds(properties(pid)['time'])>=start+.1,2,'resume did not advance')
    if seconds(properties(pid)['time'])>=3:raise RuntimeError('forward3s seek would not be forward')
    first_seek=len(state['events']);begin=time.monotonic_ns()
    answer=rpc('Player.Seek',{'playerid':pid,'value':{'time':{'hours':0,'minutes':0,'seconds':3,'milliseconds':0}}})
    samples=[]
    def settled():
        value=properties(pid);samples.append({'monotonic_ns':time.monotonic_ns(),'properties':value});fresh=state['events'][first_seek:]
        if any(e['event'] in ('OnStopped','OnEnded') for e in fresh):raise RuntimeError('seek stopped/ended playback')
        callback=any(e['event']=='OnSeek' and abs(e['time_ms']-3000)<=1 for e in fresh)
        notified=any(e['event']=='notification' and e.get('method')=='Player.OnSeek' for e in fresh)
        return value if callback and notified and value['speed']==1 and 3-1/30<=seconds(value['time'])<=3.5 else None
    value=wait(settled,3,'fresh seek event and actual clock did not settle');held=pause(pid)
    state['steps'].append({'action':'seek-forward','target_seconds':3,'begin_ns':begin,'answer':answer,
        'samples':samples,'settled':value,'pause':held,'fresh_events':state['events'][first_seek:]})
    capture('seek-forward',pid,3)
    first_stop=len(state['events']);begin=time.monotonic_ns();answer=rpc('Player.Stop',{'playerid':pid})
    def stopped():
        fresh=state['events'][first_stop:]
        if any(e['event']=='OnEnded' for e in fresh):raise RuntimeError('requested Stop became EOF')
        callback=any(e['event']=='OnStopped' for e in fresh)
        notified=any(e['event']=='notification' and e.get('method')=='Player.OnStop' and e.get('data',{}).get('end') is False for e in fresh)
        return callback and notified and not rpc('Player.GetActivePlayers')
    wait(stopped,5,'fresh requested Stop/end:false missing')
    state['steps'].append({'action':'stop','begin_ns':begin,'answer':answer,'fresh_events':state['events'][first_stop:]})
    gate('stop-retired','await-stop-retirement',active_players=rpc('Player.GetActivePlayers'))
    gate('reopen','await-reopen',source='/fixture.mp4')
    pid,first=open_fixture();held=pause(pid)
    gate('reopen-identity','await-reopen-identity',source='/fixture.mp4',info=info(),initial_pause_for_identity=True)
    rpc('Player.PlayPause',{'playerid':pid,'play':True})
    wait(lambda:properties(pid)['speed']==1,2,'normal natural-reopen resume missing')
    def ended():
        fresh=state['events'][first:]
        if any(e['event']=='OnStopped' for e in fresh):raise RuntimeError('natural reopen became requested Stop')
        callback=any(e['event']=='OnEnded' for e in fresh)
        notified=any(e['event']=='notification' and e.get('method')=='Player.OnStop' and e.get('data',{}).get('end') is True for e in fresh)
        return callback and notified and not rpc('Player.GetActivePlayers')
    wait(ended,15,'fresh same-PID natural EOF/end:true missing')
    state['steps'].append({'action':'natural-reopen','fresh_events':state['events'][first:],'eof_ns':time.monotonic_ns()})
    gate('reopen-retired','await-reopen-retirement',active_players=rpc('Player.GetActivePlayers'))
    state['status']='PASS';progress('controls-complete');save('centered-controls-result.json',state)
except BaseException as error:
    state.update(status='FAIL',error={'type':type(error).__name__,'message':str(error),'traceback':traceback.format_exc()})
    try:player.stop()
    except BaseException as cleanup:state['stop_error']=str(cleanup)
    progress('controls-failed');save('centered-controls-result.json',state)
'''


def source_census(rows, interval, pid, complete):
    selected = [r for offset, r in rows if interval['begin'] <= offset < interval['end']]
    require(all(r['pid'] == pid for r in selected), 'foreign trace PID in source interval')
    fields = S.BINDING + ('surface', 'owner'); stages = {}; errors = []
    for stage in ('submit_bind', 'input_sent', 'output_dequeued', 'materialized'):
        grouped = defaultdict(list)
        for row in selected:
            if row['event'] != stage:continue
            require(all(type(row[k]) is int and row[k] > 0 for k in fields + ('seq',)), 'incomplete source binding')
            if type(row['outcome']) is not int or row['outcome'] != 0:errors.append(stage + ': failed outcome')
            grouped[tuple(row[k] for k in fields)].append(row)
        stages[stage] = dict(grouped)
    originals = set(stages['submit_bind'])
    for index in (3, 4, 5):
        if any(n != 1 for n in Counter((key[:3], key[index]) for key in originals).values()):
            errors.append('ambiguous original identity within decoder lifetime')
    for stage, groups in stages.items():
        if not set(groups) <= originals:errors.append(stage + ': binding not submitted in this play')
        if any(len(v) != 1 for v in groups.values()):errors.append(stage + ': repeated picture record')
    if not set(stages['output_dequeued']) <= set(stages['input_sent']):errors.append('output has no matching sent input')
    sequence_ids = [(r['driver_instance'], r['seq']) for r in selected if r['event'] in stages]
    if len(sequence_ids) != len(set(sequence_ids)):errors.append('source stages reuse an instance/sequence record')
    if complete:
        if len(originals) != 180:errors.append('natural reopen needs exactly180 originals')
        for stage in ('input_sent', 'output_dequeued'):
            if set(stages[stage]) != originals:errors.append(stage + ': incomplete natural population')
    # A seek/Stop can cancel originals. Completed observations must still retain
    # their admitted identity/order; a canceled source is not a renderer loss.
    for key in originals:
        previous = 0
        for stage in ('submit_bind', 'input_sent', 'output_dequeued', 'materialized'):
            records = stages[stage].get(key, [])
            if not records:continue
            if records[0]['seq'] <= previous:errors.append('source stages out of order');break
            previous = records[0]['seq']
        if key in stages['materialized'] and key not in stages['output_dequeued']:
            errors.append('materialized has no matching output dequeue')
    return {'status': 'PASS' if not errors else 'FAIL', 'errors': errors, 'complete_natural_population_required': complete,
            'event_counts': dict(Counter(r['event'] for r in selected)),
            'binding_stage_counts': {stage: len(groups) for stage, groups in stages.items()},
            'public_materialization_count': len(stages['materialized']),
            'output_outcome_scope': 'Dequeue events precede codec validation; matching later materialized records completed public output.',
            'scope': 'Pinned progressive source: one dequeue per original; interrupted seek/Stop is partial. Public counts are separate from renderer loss.',
            'actual_media_PTS_to_VA_binding': 'NOT_OBSERVED'}


def qualify_statuses(receipts, audited, pid, sample_mode='first-two'):
    """Never hide a failed selected snapshot behind another generation's PASS."""
    validate_sample_mode(sample_mode)
    seen, eligible, result = set(), set(), []
    # Rejected-candidate counters and an unfilled group are distinct from
    # failed snapshots. Values are the private observer's Reject enum.
    allowed_mask = 2 | 4 | 128 | 512
    for row in receipts:
        validate_selection(row, sample_mode)
        key = (row['driver_instance'], row['context'], row['generation'])
        require(row['pid'] == pid and key not in seen, 'status receipt PID/lifetime duplicated')
        seen.add(key)
        require(row['sealed'] is True and row['after_driver_unlock'] is True and
                row['directory_open'] is True and row['directory_errno'] == 0 and
                row['retirement_status'] == 0, 'selected lifetime did not retire successfully')
        require(type(row['reject_mask']) is int and row['reject_mask'] >= 0 and
                not row['reject_mask'] & ~allowed_mask, 'selected snapshot/reset failed')
        selected = row['selected_slots']
        require(type(selected) is int and 1 <= selected <= 2 and all(
            type(row[name]) is int and row[name] == selected
            for name in ('compose_committed_slots', 'public_committed_slots', 'materialized_slots')),
            'selected snapshots did not all commit and materialize')
        require(bool(row['reject_mask'] & 128) == (selected < 2), 'incomplete status disagrees with fixed slots')
        kind = row.get('publication_kind', 'selected-decoder-context-retirement')
        if kind == 'selected-decoder-generation-reset':
            require(row['close_status'] == 0 and row['device_closed'] is True and
                    row['next_generation'] == row['generation'] + 1, 'private reset close/generation failed')
        else:
            require(kind == 'selected-decoder-context-retirement', 'unknown status publication kind')
        require(type(row['eligible']) is bool and row['eligible'] == (selected == 2), 'status eligibility disagrees with fixed slots')
        if row['eligible']:
            eligible.add(key)
        else:
            require(key not in audited, 'incomplete lifetime received a qualifying audit')
        result.append({'lifetime': list(key), 'selected_slots': selected,
                       'status': 'AUDITED' if row['eligible'] else 'NOT_QUALIFIED_INCOMPLETE'})
    require(eligible == set(audited), 'eligible selected lifetime lacks its matching manifest/audit')
    return result


def frozen_files(source, G, platform, observe_frame_pts=False):
    files = [Path(__file__), STOCK_PATH, READER_PATH, PAYLOAD_READER, S.BASE, G.ROUTE_PATH, G.BOUNDARY,
            source / 'tests/native-picture-fixture.py', source / 'tests/vaapi-centered-payload-protocol.md',
            source / 'tests/vaapi-centered-payload-observer.h', source / 'tests/vaapi-centered-payload-backend.cpp',
            source / 'filters/vaapi/centered-observer/crystalhd_drv_video.so',
            source / 'filters/vaapi/crystalhd_drv_video.cpp', source / 'filters/vaapi/crystalhd-trace.h',
            source / 'filters/vaapi/crystalhd-centered-output.h', source / 'linux_lib/libcrystalhd/libcrystalhd.so.3.6',
            source / 'tests/kernel-log-check.sh', FIXTURE, PROBE, MANIFEST, COORDINATES, ORIGIN,
            G.WESTON / 'bin/weston', G.WESTON / 'lib/x86_64-linux-gnu/libweston-14/gl-renderer.so',
            G.WESTON / 'lib/x86_64-linux-gnu/libweston-14/headless-backend.so',
            G.WESTON / 'lib/x86_64-linux-gnu/weston/kiosk-shell.so', Path('/usr/bin/kodi-send'),
            platform / 'lib/x86_64-linux-gnu/libswscale.so.9.5.103', platform / 'lib/x86_64-linux-gnu/libavutil.so.60.26.103']
    if validate_frame_pts_mode(observe_frame_pts):
        files.extend((FRAME_PTS_READER, source / 'tests/vaapi-frame-pts-probe.c',
                      source / 'tests/vaapi-frame-pts-probe.map', source / FRAME_PTS_IMAGE))
    return files


def clean_source(source, revision):
    require(subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip() == revision,
            'own source revision differs')
    require(not subprocess.check_output(['git', '-C', str(source), 'status', '--porcelain', '--untracked-files=all'], text=True),
            'committed clean own source required')


def main():
    require(__debug__, 'ordinary Python without -O required')
    p = argparse.ArgumentParser(description=__doc__); modes = p.add_mutually_exclusive_group(required=True)
    modes.add_argument('--prepare-plan', action='store_true'); modes.add_argument('--execute', action='store_true')
    p.add_argument('--plan', type=Path, required=True); p.add_argument('--case', default='stock-centered-controls-01')
    p.add_argument('--case-root', type=Path, default=ROOT); p.add_argument('--own-source', type=Path); p.add_argument('--own-revision')
    p.add_argument('--sample-mode', choices=SAMPLE_MODES, help='prepare only; default first-two')
    p.add_argument('--observe-frame-pts', action='store_true', help='prepare only; explicit OWN receive-frame diagnostic preload')
    p.add_argument('--render-route', choices=RENDER_ROUTES, help='prepare only; default va, explicit cpu-copy requires original postproc evidence')
    a = p.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9_-]+', a.case):p.error('fresh simple case name required')
    if a.prepare_plan and (a.own_source is None or not re.fullmatch(r'[0-9a-f]{40}', a.own_revision or '')):p.error('prepare needs own source/full revision')
    if a.execute and (a.own_source is not None or a.own_revision is not None or a.sample_mode is not None):p.error('source/revision/sample-mode belong to preparation')
    sample_mode = sample_option(a.prepare_plan, a.sample_mode)
    observe_frame_pts = frame_pts_option(a.prepare_plan, a.observe_frame_pts)
    render_route = render_route_option(a.prepare_plan, a.render_route)
    case = a.case_root.resolve() / a.case
    if a.prepare_plan:require(not a.plan.exists() and not case.exists(), 'fresh plan and case paths required')
    else:case.mkdir(mode=0o700)
    report = {'schema': 1, 'case': a.case, 'status': 'FAIL', 'scope': SCOPE, 'errors': [], 'identity_checks': [],
              'captures': [], 'payload_audits': [], 'source_census': {}, 'intervals': {}, 'retirements': [],
              'render_route_audits': [],
              'thermal_observations': [], 'controls_status': 'NOT_RUN', 'application_exit': None, 'live_resource_handoff': None}
    G = P = C = before = before_lean = kodi = kid = pidfd = weston = runtime = endpoint = udp = None
    files = []; audited = set(); due = 0; since = time.strftime('%Y-%m-%d %H:%M:%S UTC', time.gmtime())
    def same():
        require(kid is not None, 'owned Kodi identity unavailable'); now = G.identity(kid['pid'], False)
        require(all(now[k] == kid[k] for k in now), 'owned Kodi identity changed'); return now
    def alive():
        launch = kodi is not None and kodi.poll() is None
        if pidfd is None:return launch
        poll = select.poll(); poll.register(pidfd, select.POLLIN); return launch or not poll.poll(0)
    def wait_exit(seconds):
        end = time.monotonic() + seconds
        while alive() and time.monotonic() < end:time.sleep(.05)
        require(not alive(), 'owned media remains alive')
    def health():
        nonlocal due
        if time.monotonic() < due:return
        temps = [int(f.read_text()) for f in Path('/sys/class/thermal').glob('thermal_zone*/temp')]
        mains = [f for f in Path('/sys/class/power_supply').glob('*') if (f / 'type').exists() and (f / 'type').read_text().strip() == 'Mains']
        ac = bool(mains) and any((f / 'online').read_text().strip() == '1' for f in mains)
        report['thermal_observations'].append({'monotonic_ns': time.monotonic_ns(), 'temperatures_mC': temps, 'ac_present': ac})
        require(ac and temps and max(temps) < 75000, 'AC/temperature guard failed'); due = time.monotonic() + 5
    def wait(test, seconds, message):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            health(); value = test()
            if value:return value
            time.sleep(.05)
        raise TimeoutError(message)
    try:
        require(digest(S.BASE) == S.BASE_SHA, 'qualified namespace/shutdown helper changed')
        G = load('centered_controls_owned_runtime', S.BASE)
        require(digest(G.ROUTE_PATH) == G.ROUTE_SHA, 'qualified original App route changed')
        route = json.loads(G.ROUTE_PATH.read_text()); P = load('centered_controls_payload_reader', PAYLOAD_READER)
        C = load('centered_controls_content_reader', READER_PATH)
        app = Path(route['client']['app_directory'])
        platform = G.OLD / 'flatpak-user/runtime/org.freedesktop.Platform/x86_64/26.08/e7f7b0fb20662fe92e4eac65d58a57019cefae45a39559c4b50440a88acc0491/files'
        gl = G.OLD / 'flatpak-user/runtime/org.freedesktop.Platform.GL.default/x86_64/26.08/25dbbb0cd3a63b2ef8a24ee948ad43998960f3e2bea713263742cc076840aec2/files'
        metadata = source_metadata(json.loads(PROBE.read_text())); preparation = json.loads(MANIFEST.read_text())
        require(preparation['status'] == 'PASS_SOFTWARE_FIXTURE_ONLY' and preparation['input_mp4']['path'] == str(FIXTURE) and
                preparation['input_mp4']['sha256'] == FIXTURE_SHA, 'canonical prepared fixture differs')
        if a.prepare_plan:
            source = a.own_source.resolve(strict=True); clean_source(source, a.own_revision)
            require(Path(__file__).resolve() == source / 'tests/vaapi-centered-controls-stock.py', 'runner outside frozen source')
            pins = {str(f): digest(f) for f in frozen_files(source, G, platform, observe_frame_pts) + render_route_files(app, render_route)}
            require(pins[str(FIXTURE)] == FIXTURE_SHA, 'canonical input changed')
            for path, expected in route['client']['pins'].items():
                require(digest(path) == expected, 'original App/FFmpeg changed'); pins[path] = expected
            save(a.plan, {'schema': 1, 'status': 'FROZEN_CENTERED_CONTROLS', 'revision': a.own_revision, 'source_directory': str(source),
                          'case': a.case, 'case_directory': str(case), 'pins': pins, 'source_metadata': metadata,
                          'sample_mode': sample_mode, 'observe_frame_pts': observe_frame_pts, 'render_route': render_route,
                          'platform_directory': str(platform), 'gl_directory': str(gl), 'scope': scenario_scope(render_route, observe_frame_pts),
                          'method': 0, 'actual_deintmethod': 'none', 'centered': S.CENTERED, 'observer_lifetimes': 3,
                          'natural_originals': 180, 'dequeues_per_original': 1, 'seek_seconds': 3,
                          'initial_source_window': ['0', '5/2'], 'seek_source_window': ['89/30', '7/2'],
                          'paused_content_clock_tolerance': '1/2', 'scheduling': 'ordinary inherited scheduling'})
            print('FROZEN_CENTERED_CONTROLS ' + str(a.plan)); return 0
        plan = json.loads(a.plan.read_text()); G.SOURCE = Path(plan['source_directory']).resolve(strict=True); G.REVISION = plan['revision']
        sample_mode = validate_sample_mode(plan.get('sample_mode'))
        observe_frame_pts = validate_frame_pts_mode(plan.get('observe_frame_pts'))
        render_route = validate_render_route(plan.get('render_route'))
        require(plan['status'] == 'FROZEN_CENTERED_CONTROLS' and plan['case'] == a.case and plan['case_directory'] == str(case), 'frozen case differs')
        require(plan['method'] == 0 and plan['actual_deintmethod'] == 'none' and plan['centered'] == S.CENTERED and
                plan['observer_lifetimes'] == 3 and plan['natural_originals'] == 180 and plan['dequeues_per_original'] == 1 and
                plan['seek_seconds'] == 3 and plan['paused_content_clock_tolerance'] == '1/2', 'frozen control contract differs')
        clean_source(G.SOURCE, G.REVISION)
        require(Path(__file__).resolve() == G.SOURCE / 'tests/vaapi-centered-controls-stock.py', 'executed runner outside frozen source')
        require(all(str(f) in plan['pins'] for f in frozen_files(G.SOURCE, G, platform, observe_frame_pts) + render_route_files(app, render_route)), 'required frozen artifact missing')
        require(all(plan['pins'].get(path) == expected for path, expected in route['client']['pins'].items()), 'original App pins differ')
        for path, expected in plan['pins'].items():require(digest(path) == expected, 'frozen artifact changed: ' + path)
        require(plan['source_metadata'] == metadata and plan['pins'][str(FIXTURE)] == FIXTURE_SHA and
                plan['platform_directory'] == str(platform) and plan['gl_directory'] == str(gl), 'frozen fixture/runtime differs')
        require(hasattr(os, 'pidfd_open') and hasattr(signal, 'pidfd_send_signal'), 'owned shutdown requires pidfds')
        os.umask(0o077); resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        G.VA = G.SOURCE / 'filters/vaapi/centered-observer/crystalhd_drv_video.so'; G.DIL = G.SOURCE / 'linux_lib/libcrystalhd/libcrystalhd.so.3.6'
        report.update(plan=str(a.plan), revision=G.REVISION, source_metadata=metadata, pins=plan['pins'], scheduling=plan['scheduling'],
                      sample_mode=sample_mode, observe_frame_pts=observe_frame_pts, render_route=render_route,
                      scope=scenario_scope(render_route, observe_frame_pts))
        before = load('centered_controls_initial_boundary', G.BOUNDARY).guard(); report['before'] = before
        require(before['debug_rows'] == 85 and before['debug_sha256'] ==
                '4e2758e4d68361f8783c48dbae8bd38ce06c380155a0f9de294d63d888c7a2f4', 'initial debug policy differs')
        require(all(int(line.split()[-1]) == 0 for text in before['aer']['0000:02:00.0'].values()
                    for line in text.splitlines() if line.strip()), 'initial endpoint AER nonzero')
        before_lean = {k: before[k] for k in S.POLL_KEYS}; health()
        profile, run, payload = case / 'profile', case / 'run', case / 'payload'
        profile.mkdir(); run.mkdir(); payload.mkdir(mode=0o700); (run / 'deps').mkdir()
        (run / 'controls.py').write_text(application_script(render_route))
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reserve:
            reserve.bind(('127.0.0.1', 0)); udp = reserve.getsockname()[1]
        settings = {'services.webserver': 'false', 'services.esenabled': 'true', 'services.esport': str(udp),
                    'services.esportrange': '1', 'services.esallinterfaces': 'false', 'addons.unknownsources': 'true',
                    'addons.updatemode': '2', 'screensaver.mode': '', 'weather.addon': '', 'lookandfeel.soundskin': '',
                    'videoplayer.usevaapi': 'true', 'videoplayer.usevaapih264': 'true',
                    'videoplayer.useprimedecoder': 'false', 'videoplayer.useprimedecoderforhw': 'false'}
        settings.update(render_route_settings(render_route))
        userdata = profile / 'userdata'; userdata.mkdir()
        xml = '<settings version="2">' + ''.join('<setting id="' + escape(k) + '">' + escape(v) + '</setting>' for k, v in settings.items())
        xml += '<defaultvideosettings><interlacemethod>0</interlacemethod><scalingmethod>0</scalingmethod><viewmode>0</viewmode><zoomamount>1</zoomamount><pixelratio>1</pixelratio><verticalshift>0</verticalshift></defaultvideosettings></settings>'
        (userdata / 'guisettings.xml').write_text(xml)
        (userdata / 'advancedsettings.xml').write_text('<advancedsettings><loglevel>1</loglevel></advancedsettings>')
        runtime = Path(tempfile.mkdtemp(prefix='ch-centered-controls-', dir='/run/user/' + str(os.getuid())))
        lib = G.WESTON / 'lib/x86_64-linux-gnu'; clean_env = {k: v for k, v in os.environ.items() if not k.startswith(('LD_', 'GST_', 'CRYSTALHD_', 'LIBVA_'))}
        env = dict(clean_env, XDG_RUNTIME_DIR=str(runtime), WESTON_DATA_DIR=str(G.WESTON / 'share/weston'),
                   WESTON_MODULE_MAP=';'.join(n + '=' + str(lib / 'libweston-14' / n) for n in ('gl-renderer.so', 'headless-backend.so')),
                   LD_LIBRARY_PATH=str(lib) + ':' + str(lib / 'weston'))
        log = (case / 'weston.stdout').open('x'); files.append(log); sock = 'stock-centered-controls'
        weston = subprocess.Popen([str(G.WESTON / 'bin/weston'), '--backend=headless', '--renderer=gl',
            '--shell=' + str(lib / 'weston/kiosk-shell.so'), '--width=1920', '--height=1080', '--refresh-rate=60000',
            '--socket=' + sock, '--idle-time=0', '--no-config', '--log=' + str(case / 'weston.log')],
            env=env, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT)
        wait(lambda: (runtime / sock).exists() if weston.poll() is None else False, 8, 'owned Weston readiness')
        command = G.namespace(Path(route['client']['app_directory']), platform, gl, case, runtime, sock, FIXTURE)
        at = command.index('--'); command[at:at] = ['--bind', str(payload), '/payload',
            '--setenv', 'CRYSTALHD_TEST_CENTERED_PAYLOAD_DIR', '/payload', '--setenv', 'CRYSTALHD_VAAPI_EXPERIMENTAL_CENTERED_NV12', S.CENTERED] + sample_environment(sample_mode) + frame_pts_environment(G.SOURCE, observe_frame_pts)
        require('--die-with-parent' not in command, 'media runtime must survive operator handoff'); save(case / 'command.json', command)
        log = (case / 'kodi.stdout').open('x'); files.append(log)
        kodi = subprocess.Popen(command, env=clean_env, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT)
        def discover():
            require(kodi.poll() is None, 'owned Kodi launcher exited before readiness')
            for candidate in G.descendants(kodi.pid):
                try:
                    if Path('/proc', str(candidate), 'comm').read_text().strip() == 'kodi.bin':
                        value = G.identity(candidate)
                        if value['uid'] == os.getuid() and value['executable_sha256'] == route['client']['pins'][str(Path(route['client']['app_directory']) / 'lib/kodi/kodi.bin')]:return value
                except FileNotFoundError:pass
        kid = wait(discover, 30, 'original owned Kodi readiness'); report.update(owned_app=kid, owned_launcher_pid=kodi.pid)
        pidfd = os.pidfd_open(kid['pid'], 0); same()
        def find_endpoint():
            same()
            try:return G.udp_owner(kid['pid'], udp)
            except AssertionError:return None
        endpoint = wait(find_endpoint, 20, 'owned EventServer readiness')
        dependencies = {'/usr/lib/x86_64-linux-gnu/libswscale.so.9.5.103': str(platform / 'lib/x86_64-linux-gnu/libswscale.so.9.5.103'),
                        '/usr/lib/x86_64-linux-gnu/libavutil.so.60.26.103': str(platform / 'lib/x86_64-linux-gnu/libavutil.so.60.26.103')}
        require(S.lean_snapshot() == before_lean and not G.owned_fds(kid['pid']), 'decoder not idle before controls')
        report['intervals']['interrupted'] = {'begin': (case / 'kodi.stdout').stat().st_size, 'begin_ns': time.monotonic_ns()}
        if render_route == 'cpu-copy':
            route_log = profile / 'temp/kodi.log'
            report['route_log_intervals'] = {'interrupted': route_log_start(route_log, kid['pid'])}
        subprocess.run(['/usr/bin/kodi-send', '--host=127.0.0.1', '--port=' + str(udp), '--action=RunScript(/probe/controls.py)'],
                       check=True, stdout=subprocess.DEVNULL, timeout=5)
        def gate(name):save(profile / (name + '.ok'), {'verified': True})
        def raw_rows(readiness=False):
            require((case / 'kodi.stdout').stat().st_size <= P.MAX_TRACE_BYTES, 'bounded raw trace exceeded')
            raw = (case / 'kodi.stdout').read_bytes()
            # Readiness alone ignores a concurrently incomplete final write.
            # Frozen acceptance prefixes retain the original bytes below.
            if readiness and not raw.endswith(b'\n'):raw = raw[:raw.rfind(b'\n') + 1]
            return raw, S.parse_rows(raw, P)
        def identity(value):
            labels = value['info']
            require(labels['Player.Process(videodecoder)'] == 'ff-h264-vaapi' and
                    labels['Player.Process(deintmethod)'] == 'none' and value['source'] == '/fixture.mp4', 'actual stock VA Off route differs')
            fds = G.owned_fds(kid['pid']); require(any(f['target'] == '/dev/crystalhd' for f in fds), 'hardware decoder FD absent')
            report['identity_checks'].append({'info': labels, 'owned_fds': fds, 'same_app': same()})
            if render_route == 'cpu-copy':
                report['identity_checks'][-1].update(render_route_settings=qualify_cpu_copy_settings(value.get('render_route_settings')),
                    cpu_copy_provider=cpu_copy_mapping(kid['pid'], app, plan['pins']))
        def retire(name):
            samples = []
            def idle():
                same(); row = {'monotonic_ns': time.monotonic_ns(), 'boundary': S.lean_snapshot(), 'owned_decoder_fds': G.owned_fds(kid['pid'])}
                samples.append(row); return row['boundary'] == before_lean and not row['owned_decoder_fds']
            wait(idle, 10, 'normal retirement ' + name); time.sleep(.25); require(idle(), 'retirement not stable')
            report['retirements'].append({'phase': name, 'samples': samples})
        def audit_retired(name, progress):
            raw, rows = raw_rows(); interval = report['intervals'][name]; interval.update(end=len(raw), end_ns=time.monotonic_ns())
            trace = case / (name + '.trace-prefix.log')
            with trace.open('xb') as out:out.write(raw)
            new = []
            for path in sorted(payload.glob('centered-payload-*.json')):
                doc = json.loads(P.read_regular(path, P.MAX_MANIFEST_BYTES), object_pairs_hook=P.strict_object)
                validate_selection(doc, sample_mode)
                require(doc['pid'] == kid['pid'] and len(doc['slots']) == 2, 'manifest PID/slot count differs')
                b = doc['slots'][0]['binding']; key = (doc['driver_instance'], b['context'], b['generation'])
                if key not in audited:new.append((path, doc, key))
            require((1 <= len(new) <= 2 if name == 'interrupted' else len(new) == 1) and
                    len(audited) + len(new) <= 3, 'unexpected selected decoder lifetime count')
            for path, doc, key in new:
                for slot in doc['slots']:
                    b = slot['binding']
                    submits = [offset for offset, r in rows if r['pid'] == kid['pid'] and r['driver_instance'] == key[0]
                               and r['event'] == 'submit_bind' and all(r[k] == b[k] for k in P.BINDING_KEYS)]
                    require(len(submits) == 1 and interval['begin'] <= submits[0] < interval['end'], 'payload belongs to another play interval')
                result = audit_payload_for_mode(C, path, trace, sample_mode)
                report['payload_audits'].append({'phase': name, 'manifest': str(path), 'trace_prefix': str(trace), 'audit': result})
                save(case / ('payload-audit-' + str(len(report['payload_audits'])) + '.json'), result)
                require(result['status'] == 'PASS', 'source-bound payload audit failed: ' + json.dumps(result)); audited.add(key)
            census = source_census(rows, interval, kid['pid'], name == 'reopen'); report['source_census'][name] = census
            require(census['status'] == 'PASS', 'source binding census failed: ' + json.dumps(census))
            if render_route == 'cpu-copy':
                route_audit = audit_cpu_copy_route_file(route_log, report['route_log_intervals'][name])
                report['render_route_audits'].append({'phase': name, 'audit': route_audit})
                save(case / (name + '-cpu-copy-route.json'), route_audit)
            if name == 'reopen' and sample_mode == 'case2-90-91':qualify_target_reopen(census)
            if name == 'interrupted':
                seeks = [r for r in progress['steps'] if r['action'] == 'seek-forward']; require(len(seeks) == 1, 'seek record missing')
                if sample_mode == 'case2-90-91':
                    stops = [r for r in progress['steps'] if r['action'] == 'stop']; require(len(stops) == 1, 'requested Stop record missing')
                    selected = [r for r in report['payload_audits'] if r['phase'] == name]
                    target = audit_target_phase(C, selected, seeks[0]['begin_ns'], stops[0]['begin_ns'])
                    report['seek_target_window_audit'] = target; save(case / 'seek-target-window-audit.json', target)
                    report['seek_target_full_NV12_samples'] = 2
                    report['seek_target_full_NV12_scope'] = 'SOURCE_PROVEN90/91; submit/materialized/successfulSync after seek and before requested Stop'
                    return
                count = 0
                for record in report['payload_audits']:
                    if record['phase'] != name:continue
                    instance = record['audit']['payload_audit']['driver_instance']
                    for content in record['audit']['selected_source_bindings']:
                        b = content['binding']
                        materialized = [r for _, r in rows if r['driver_instance'] == instance and r['event'] == 'materialized'
                                        and all(r[k] == b[k] for k in P.BINDING_KEYS)]
                        require(len(materialized) == 1, 'selected content materialization differs')
                        if materialized[0]['ts_monotonic_ns'] >= seeks[0]['begin_ns'] and 89 <= content['source_ordinal'] <= 105:count += 1
                report['seek_target_full_NV12_samples'] = count
                report['seek_target_full_NV12_scope'] = 'CAPTURED_SELECTED_BINDINGS' if count else 'NOT_CAPTURED; post-seek BGRA barcode is controls/content proof only'
        handled = set(); deadline = time.monotonic() + 120
        while time.monotonic() < deadline:
            result_path = profile / 'centered-controls-result.json'
            if result_path.exists():
                report['controls'] = json.loads(result_path.read_text()); report['controls_status'] = report['controls']['status']; break
            health(); same(); progress_path = profile / 'centered-controls-progress.json'
            if not progress_path.exists():time.sleep(.05); continue
            value = json.loads(progress_path.read_text()); phase = value['phase']
            if phase in handled:time.sleep(.05); continue
            if phase == 'await-initial-ready':
                identity(value)
                def ready():
                    _, rows = raw_rows(True); groups = defaultdict(set)
                    for offset, r in rows:
                        if offset >= report['intervals']['interrupted']['begin'] and r['pid'] == kid['pid'] and r['event'] == 'materialized' and r['outcome'] == 0:
                            groups[(r['driver_instance'], r['context'], r['generation'])].add(tuple(r[k] for k in S.BINDING))
                    return [{'lifetime': list(k), 'distinct_materialized_bindings': len(v)} for k, v in groups.items() if len(v) >= 2]
                report['initial_ready'] = wait(ready, 2, 'two initial completed bindings missing'); gate('initial-ready')
            elif phase.startswith('await-capture-'):
                label = value['capture_label']; meta = value['capture']
                require(label in ('initial-pause', 'seek-forward') and meta['file'] == label + '.bgra', 'capture path/label differs')
                audit = verify_capture(C.audit_capture(profile / meta['file']), meta, label)
                if label == 'initial-pause':
                    report['identity_checks'][0]['mapped_images'] = G.mappings(kid['pid'], plan['pins'], dependencies)
                    if observe_frame_pts:report['identity_checks'][0]['frame_pts_observer'] = frame_pts_mapping(kid['pid'], G.SOURCE, plan['pins'])
                report['captures'].append({'capture': meta, 'audit': audit}); gate('capture-' + label)
            elif phase == 'await-stop-retirement':
                require(value['active_players'] == [], 'requested Stop left active player'); retire('interrupted'); audit_retired('interrupted', value); gate('stop-retired')
            elif phase == 'await-reopen':
                require(S.lean_snapshot() == before_lean and not G.owned_fds(kid['pid']), 'decoder not idle before same-PID reopen')
                report['intervals']['reopen'] = {'begin': (case / 'kodi.stdout').stat().st_size, 'begin_ns': time.monotonic_ns()}
                if render_route == 'cpu-copy':report['route_log_intervals']['reopen'] = route_log_start(route_log, kid['pid'])
                gate('reopen')
            elif phase == 'await-reopen-identity':identity(value); gate('reopen-identity')
            elif phase == 'await-reopen-retirement':
                require(value['active_players'] == [], 'natural EOF left active player'); retire('reopen'); audit_retired('reopen', value); gate('reopen-retired')
            elif phase == 'controls-failed':raise RuntimeError('stock controls script failed; retained result has reason')
            elif phase == 'controls-complete':pass
            else:raise ValueError('unknown application phase ' + phase)
            handled.add(phase); time.sleep(.05)
        else:raise TimeoutError('bounded centered controls deadline')
        require(report['controls_status'] == 'PASS' and len(report['captures']) == len(report['identity_checks']) == 2 and
                set(report['source_census']) == {'interrupted', 'reopen'}, 'controls/source evidence incomplete')
    except BaseException as error:
        report['errors'].append({'step': 'prepare' if a.prepare_plan else 'execution', 'error': fault(error)})
        if a.prepare_plan:raise
        if (case / 'profile').is_dir():
            try:save(case / 'profile/controller-abort.json', {'schema': 1, 'status': 'CONTROLLER_ABORT', 'error': str(error)[:4096]})
            except BaseException as abort_error:report['errors'].append({'step': 'controller-abort', 'error': fault(abort_error)})
    finally:
        if not a.prepare_plan:
            live = kodi is not None
            try:
                live = alive()
                if kodi is not None and G is not None:
                    def normal_quit():
                        require(endpoint is not None, 'owned EventServer endpoint unavailable'); same()
                        require(G.udp_owner(kid['pid'], udp) == endpoint, 'owned EventServer changed')
                        subprocess.run(['/usr/bin/kodi-send', '--host=127.0.0.1', '--port=' + str(udp), '--action=Quit'],
                                       check=True, stdout=subprocess.DEVNULL, timeout=5)
                    def term():
                        require(pidfd is not None, 'verified media pidfd unavailable'); same(); signal.pidfd_send_signal(pidfd, signal.SIGTERM)
                    shutdown = G.normal_media_shutdown(alive, normal_quit, term, wait_exit)
                    report['media_shutdown'] = shutdown; report['errors'].extend(shutdown['errors']); live = alive()
                report['application_exit'] = kodi.poll() if kodi is not None else None
                if live:
                    report['live_resource_handoff'] = {'status': 'HANDOFF_REQUIRED_MEDIA_STILL_ALIVE', 'launcher_pid': kodi.pid,
                        'last_verified_kodi_identity': kid, 'compositor_pid': weston.pid if weston else None, 'private_runtime': str(runtime),
                        'profile': str(case / 'profile'), 'required_action': 'Preserve runtime/profile/compositor; verified ordinary Quit/TERM only. No kill/unload/retry.'}
                elif weston is not None:
                    try:
                        if weston.poll() is None:weston.terminate(); weston.wait(timeout=10)
                    except BaseException as error:report['errors'].append({'step': 'Weston-close', 'error': fault(error)})
                    if weston.poll() is None:report['live_resource_handoff'] = {'status': 'HANDOFF_REQUIRED_COMPOSITOR_STILL_ALIVE', 'compositor_pid': weston.pid, 'private_runtime': str(runtime)}
                if report['live_resource_handoff'] is not None:save(case / 'live-resource-handoff.json', report['live_resource_handoff'])
                for stream in files:stream.close()
                if pidfd is not None:os.close(pidfd)
                if runtime is not None and not live and (weston is None or weston.poll() is not None):
                    import shutil
                    shutil.rmtree(runtime)
                if before_lean is not None and not live:
                    end = time.monotonic() + 10; after = S.lean_snapshot()
                    while after != before_lean and time.monotonic() < end:time.sleep(.25); after = S.lean_snapshot()
                    report['after'] = after; report['boundary_equal'] = after == before_lean
                    require(report['boundary_equal'], 'terminal lean ownership/power boundary differs')
                if before is not None:
                    with (case / 'kernel.log').open('x') as out:
                        report['kernel_exit'] = subprocess.run(['sudo', '-n', 'sh', str(G.SOURCE / 'tests/kernel-log-check.sh'), since], stdout=out, stderr=subprocess.STDOUT, timeout=15).returncode
                result_path = case / 'profile/centered-controls-result.json'
                if result_path.exists():report['controls'] = json.loads(result_path.read_text()); report['controls_status'] = report['controls']['status']
                if P is not None and (case / 'kodi.stdout').exists():
                    require((case / 'kodi.stdout').stat().st_size <= P.MAX_TRACE_BYTES, 'bounded raw trace exceeded')
                    raw = (case / 'kodi.stdout').read_bytes(); report['raw_stdout_bytes'] = len(raw)
                    report['payload_status_receipts'] = [json.loads(line, object_pairs_hook=P.strict_object) for line in raw.splitlines()
                        if line.startswith(b'{"schema":"crystalhd-centered-payload-status-v1"')]
                    require(len(report['payload_status_receipts']) <= 16 and all(r['pid'] == kid['pid'] and r['after_driver_unlock'] is True
                            for r in report['payload_status_receipts']), 'bounded status receipt identity differs')
                    for receipt in report['payload_status_receipts']:validate_selection(receipt, sample_mode)
                    if report['controls_status'] == 'PASS':
                        report['payload_status_classifications'] = qualify_statuses(report['payload_status_receipts'], audited, kid['pid'], sample_mode)
                        rows = S.parse_rows(raw, P); intervals = list(report['intervals'].values())
                        require(all(r['pid'] == kid['pid'] for _, r in rows), 'foreign trace PID')
                        for instance in {r['driver_instance'] for _, r in rows}:P.trace_index([r for _, r in rows], kid['pid'], instance)
                        require(len(intervals) == 2 and all('end' in v for v in intervals), 'play intervals incomplete')
                        for offset, r in rows:
                            if r['event'] in ('submit_bind', 'input_sent', 'output_dequeued', 'materialized', 'prefeed_sent'):
                                require(sum(v['begin'] <= offset < v['end'] for v in intervals) == 1, 'source work outside frozen play intervals')
                        if observe_frame_pts:
                            require(not live and report.get('boundary_equal'), 'frame PTS report requires completed owned shutdown and restored boundary')
                            frame_reader = load('centered_controls_frame_pts_reader', FRAME_PTS_READER)
                            frame_report = frame_pts_report_path(case / 'payload', kid['pid'])
                            report['frame_pts_report'] = str(frame_report)
                            result = audit_frame_pts(frame_reader, frame_report, case / 'kodi.stdout', report['payload_audits'], kid['pid'])
                            report['frame_pts_audit'] = result; save(case / 'frame-pts-audit.json', result)
            except BaseException as error:
                report['errors'].append({'step': 'cleanup/final-observation', 'error': fault(error)})
                if live or (weston is not None and weston.poll() is None):
                    report['live_resource_handoff'] = report['live_resource_handoff'] or {'status': 'HANDOFF_REQUIRED_CLEANUP_INCOMPLETE',
                        'launcher_pid': kodi.pid if kodi else None, 'last_verified_kodi_identity': kid,
                        'compositor_pid': weston.pid if weston else None, 'private_runtime': str(runtime), 'profile': str(case / 'profile')}
            finally:
                passed = (report['controls_status'] == 'PASS' and len(report['captures']) == 2 and bool(report['payload_audits']) and
                    all(r['audit'].get('status') == 'PASS' for r in report['payload_audits']) and set(report['source_census']) == {'interrupted', 'reopen'} and
                    all(r['status'] == 'PASS' for r in report['source_census'].values()) and report['application_exit'] == 0 and
                    report.get('boundary_equal') and report.get('kernel_exit') == 0 and not report['errors'] and report['live_resource_handoff'] is None)
                if observe_frame_pts:passed = passed and report.get('frame_pts_audit', {}).get('status') == 'PASS'
                if render_route == 'cpu-copy':
                    route_passed = (len(report['render_route_audits']) == 2 and
                        {record['phase'] for record in report['render_route_audits']} == {'interrupted', 'reopen'} and
                        all(record['audit']['status'] == 'PASS' for record in report['render_route_audits']))
                    passed = passed and route_passed
                    report.update(actual_cpu_copy_route='OBSERVED_FROM_ORIGINAL_LOGS' if route_passed else 'NOT_QUALIFIED',
                        cpu_copy_pixel_scope='Full NV12 snapshots are before client CPU copy; downstream proof is selected BGRA source-content checkpoints only.')
                pts_status = 'OBSERVED_BY_OWN_INSTRUMENTATION' if report.get('frame_pts_audit', {}).get('status') == 'PASS' else 'NOT_OBSERVED'
                report.update(status='PASS' if passed else 'FAIL', actual_media_PTS_to_VA_binding=pts_status,
                    actual_media_PTS_clock_comparison=report.get('frame_pts_audit', {}).get('selected_clock_status', 'NOT_OBSERVED'), gpu_reader_return='NOT_OBSERVED',
                    stock_compatibility='NOT_QUALIFIED_BY_PRIVATE_OBSERVER', excluded_heap='No observer/investigation/reproduction; ordinary exit status only retained.')
                save(case / 'result.json', report)
    print(a.case + ': ' + report['status'] + '; scoped centered source controls'); return 0 if report['status'] == 'PASS' else 1


if __name__ == '__main__':
    try:raise SystemExit(main())
    except Exception as error:print('stock-centered-controls: ' + str(error), file=sys.stderr); raise SystemExit(1)
