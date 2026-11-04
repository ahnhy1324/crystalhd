#!/usr/bin/python3
"""Opt-in stock Kodi Off/centered NV12 observer; normal EOF and owned Quit/TERM.

Plan preparation reads regular files only. The private OWN backend does not
qualify stock compatibility, GPU return, presentation or release quality.
No external product code, module, firmware or scheduling policy is changed.
"""
import argparse
from collections import Counter, defaultdict
from fractions import Fraction
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import resource
import select
import signal
import socket
import subprocess
import sys
import tempfile
import time
import traceback
from xml.sax.saxutils import escape

sys.dont_write_bytecode = True
ROOT = Path('/home/mache/.cache/crystalhd-all-work-20261031')
BASE = ROOT / 'stock-gray-checkpoints.py'
BASE_SHA = 'dfec342196ca33fe27e57b9b74451c5378500bb34acb4dc2f17bb20596e1f260'
READER = Path(__file__).with_name('vaapi-centered-payload-reader.py')
PROTOCOL = Path(__file__).with_name('vaapi-centered-payload-protocol.md')
POSTFILTER_READER = Path(__file__).with_name('vaapi-postfilter-reader.py')
POSTFILTER_IMAGE = Path('filters/vaapi/postfilter-observer/postfilter-probe.so')
POSTFILTER_MOUNT = '/own-observer/postfilter-probe.so'
POSTFILTER_REFERENCE = Path('filters/vaapi/postfilter-observer/postfilter-reference')
POSTFILTER_REFERENCE_WRAPPER = Path(__file__).with_name('vaapi-postfilter-reference.py')
FIXTURE = Path('/home/mache/.cache/crystalhd-single-field-20261015.boremY/kodi-h264-1080i25-30s.mkv')
BWDIF_FIXTURE = Path('/home/mache/.cache/crystalhd-va-centered-20261023.D8g26l/fixtures/2997-uniform/uniform-18.ts')
BWDIF_FIXTURE_SHA = '2de3d510076f4037abcb56f84a7bfbb86caa66ae1bb9dc595c79b6e36b450905'
CPU_COPY_PROVIDER = Path('lib/kodi/system/libsse4-x86_64-linux.so')
CENTERED = '1280x720@1920x1080:limited'
SOURCE_REF = 'https://github.com/xbmc/xbmc/blob/28ea2eac1eb7af8fdcbd2672d933ce87f594be79/'
SCOPE = ('Unchanged Kodi22RC1/App FFmpeg, method0 and actual hardware deinterlace none; '
         'two OWN small/composed/public NV12 checkpoints per normally retired decoder context. '
         'Actual media PTS-to-binding, GPU-reader return, scanout and release qualification NOT_OBSERVED.')
POLL_KEYS = ('module', 'srcversion', 'ref', 'pin_net', 'force', 'session', 'power', 'aer', 'links', 'video_nodes')
BINDING = ('driver_instance', 'context', 'generation', 'token', 'decode_identity', 'submission_ordinal')
BWDIF_SCOPE = ('Original Kodi22RC1/App FFmpeg, method7/actual bwdif, explicit OWN centered and post-filter observers; '
               'same-PID natural EOF/reopen and measured input/output metadata with selected same-input pixel reference. '
               'No stock/release, full-source scaler, physical cadence or GPU-return qualification.')


def scenario(name):
    require(type(name) is str and name in ('off750', 'bwdif2997'), 'unknown frozen centered scenario')
    bwdif = name == 'bwdif2997'
    return {'name': name, 'fixture': str(BWDIF_FIXTURE if bwdif else FIXTURE),
            'pictures': 900 if bwdif else 750, 'rate': '30000/1001' if bwdif else '25',
            'method': 7 if bwdif else 0, 'deintmethod': 'bwdif' if bwdif else 'none',
            'prefer_vaapi_renderer': not bwdif, 'scope': BWDIF_SCOPE if bwdif else SCOPE}


def require(ok, message):
    if not ok:
        raise ValueError(message)


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def save(path, value):
    with os.fdopen(os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), 'w') as stream:
        json.dump(value, stream, indent=2)
        stream.write('\n')


def fault(error):
    return {'type': type(error).__name__, 'message': str(error),
            'traceback': ''.join(traceback.format_exception(type(error), error, error.__traceback__))}


def source_metadata(document, media, scenario_name='off750'):
    """Independent source census only; no ordinal/token or physical-parity join."""
    selected = scenario(scenario_name)
    count, rate = selected['pictures'], Fraction(selected['rate'])
    streams = document.get('streams', [])
    require(len(streams) == 1, 'probe must select one video stream')
    s = streams[0]
    require((s.get('codec_type'), s.get('codec_name'), s.get('width'), s.get('height'), s.get('pix_fmt')) ==
            ('video', 'h264', 1920, 1080, 'yuv420p'), 'source codec/geometry differs')
    require(Fraction(s['r_frame_rate']) == rate, 'source nominal rate differs')
    if 'packets_and_frames' in document:
        both = document['packets_and_frames']
        packets = [row for row in both if row.get('type') == 'packet']
        frames = [row for row in both if row.get('type') == 'frame']
        require(len(both) == len(packets) + len(frames), 'unknown probe object')
    else:
        packets, frames = document.get('packets', []), document.get('frames', [])
    require(len(packets) == len(frames) == count, 'source packet/decoded-picture population differs')
    base = Fraction(s['time_base']); pts = [int(frame['pts']) for frame in frames]
    require(base > 0 and len(set(pts)) == count and pts == sorted(pts), 'source PTS unavailable/ambiguous')
    require(all(Fraction(b - a) * base == 1 / rate for a, b in zip(pts, pts[1:])), 'source PTS grid differs')
    require(sorted(int(packet['pts']) for packet in packets) == pts, 'packet/frame PTS sets differ')
    association = document.get('format', {})
    require(Path(association['filename']).resolve() == media.resolve() and
            int(association['size']) == media.stat().st_size, 'probe filename/extent association differs')
    flags = Counter((f.get('interlaced_frame'), f.get('top_field_first')) for f in frames)
    if scenario_name == 'bwdif2997':
        require(base == Fraction(1, 90000) and pts[0] == 6006 and pts[-1] == 2705703 and
                s.get('coded_width') == 1920 and s.get('coded_height') == 1088 and
                flags == Counter({(1, 1): count}) and all(f.get('repeat_pict') == 0 for f in frames),
                'reviewed short TFF source metadata differs')
    return {'codec': 'h264', 'visible': [1920, 1080], 'packets': count, 'pictures': count,
            'source_time_base': str(base), 'source_PTS_ticks': pts, 'nominal_picture_duration': '1/25',
            'nominal_duration_seconds': str(Fraction(count, 1) / rate) if scenario_name == 'bwdif2997' else 30,
            'stream_field_order': s.get('field_order'),
            'decoded_field_flag_counts': [{'interlaced_frame': k[0], 'top_field_first': k[1], 'count': n}
                                          for k, n in sorted(flags.items(), key=lambda item: str(item[0]))],
            'uniform_physical_parity_claimed': False, 'actual_source_PTS_to_VA_binding': 'NOT_OBSERVED',
            **({'nominal_picture_duration': str(1 / rate), 'coded': [1920, 1088]} if scenario_name == 'bwdif2997' else {})}


def lean_snapshot():
    """Current ownership/power state; no artifact hashes or debug inventory."""
    subprocess.run(['sudo', '-n', 'cat', '/proc/sys/vm/stat_refresh'], check=True,
                   stdout=subprocess.DEVNULL, timeout=3)
    vm = dict(line.split() for line in Path('/proc/vmstat').read_text().splitlines())
    def read(path):
        return Path(path).read_text().strip()
    ports = ('0000:02:00.0', '0000:00:1c.0')
    return {'module': read('/sys/module/crystalhd/version'), 'srcversion': read('/sys/module/crystalhd/srcversion'),
            'ref': int(read('/sys/module/crystalhd/refcnt')),
            'pin_net': int(vm['nr_foll_pin_acquired']) - int(vm['nr_foll_pin_released']),
            'force': read('/sys/module/crystalhd/parameters/force_l0s_off'),
            'session': read('/sys/module/crystalhd/parameters/session_l0s_off'),
            'power': read('/sys/bus/pci/devices/0000:02:00.0/power/control'),
            'aer': {p: {f.name: f.read_text() for f in Path('/sys/bus/pci/devices', p).glob('aer_dev_*')} for p in ports},
            'links': {p: int(subprocess.check_output(['sudo', '-n', 'setpci', '-s', p, 'CAP_EXP+10.w'],
                                                    text=True, timeout=3).strip(), 16) for p in ports},
            'video_nodes': sorted(str(p) for p in Path('/dev').glob('video*'))}


# Original Kodi executes this through its published RunScript API. Ordinary
# '\n' escapes inside this raw string become valid Python string escapes.
SCRIPT = r'''
import json,os,threading,time,traceback,xbmc,xbmcgui
from pathlib import Path
R=Path('/profile');lock=threading.RLock();phase='starting';rpcid=0;CYCLES=__CYCLES__
state={'schema':1,'status':'RUNNING','phase':phase,'events':[],'rpc':[],'plays':[]}
def save(name,value):
    with lock:
        temporary=R/(name+'.tmp')
        with temporary.open('w') as output:json.dump(value,output,indent=2);output.write('\n')
        os.replace(temporary,R/name)
def progress(p,**fields):
    global phase
    phase=p
    with lock:state.update(phase=p,monotonic_ns=time.monotonic_ns(),**fields);save('centered-progress.json',state)
def event(kind,**fields):
    with lock:
        if len(state['events'])>=512:state['overflow']=True;return
        state['events'].append(dict(event=kind,phase=phase,monotonic_ns=time.monotonic_ns(),**fields))
class Monitor(xbmc.Monitor):
    def onNotification(self,sender,method,data):
        if method.startswith('Player.'):
            try:payload=json.loads(data)
            except ValueError:payload={'unparsed':data[:1024]}
            event('notification',sender=sender,method=method,data=payload)
class Player(xbmc.Player):
    def onAVStarted(self):event('OnAVStarted')
    def onPlayBackStarted(self):event('OnStarted')
    def onPlayBackStopped(self):event('OnStopped')
    def onPlayBackEnded(self):event('OnEnded')
    def onPlayBackError(self):event('OnError')
monitor=Monitor();player=Player()
def rpc(method,params=None):
    global rpcid
    rpcid+=1
    if rpcid>512:raise RuntimeError('bounded RPC census exceeded')
    request={'jsonrpc':'2.0','id':rpcid,'method':method,'params':params or {}}
    answer=json.loads(xbmc.executeJSONRPC(json.dumps(request)))
    state['rpc'].append({'request':request,'answer':answer,'monotonic_ns':time.monotonic_ns()})
    if 'error' in answer:raise RuntimeError('JSONRPC error '+json.dumps(answer))
    return answer['result']
def pump():
    if (R/'controller-abort.json').exists():raise RuntimeError('owned controller aborted; release script gates')
    if monitor.abortRequested() or state.get('overflow'):raise RuntimeError('application abort/observer overflow')
    if any(row['event']=='OnError' for row in state['events']):raise RuntimeError('playback error')
    xbmc.sleep(20)
def wait(test,seconds,message):
    end=time.monotonic()+seconds
    while time.monotonic()<end:
        value=test()
        if value:return value
        pump()
    raise TimeoutError(message)
def gate(name,p,**fields):
    progress(p,**fields);wait(lambda:(R/(name+'.ok')).exists(),25,'owned controller gate '+name)
def info():
    return rpc('XBMC.GetInfoLabels',{'labels':['Player.Process(videodecoder)','Player.Process(pixformat)',
        'Player.Process(deintmethod)','Player.Filenameandpath','System.ScreenWidth','System.ScreenHeight']})
try:
    for cycle in range(1,CYCLES+1):
        suffix=str(cycle);source='/fixture.mp4'
        gate('open-'+suffix,'await-open-'+suffix,cycle=cycle,source=source)
        first=len(state['events']);item=xbmcgui.ListItem(path=source);item.setContentLookup(False);player.play(source,item)
        wait(lambda:any(row['event']=='OnAVStarted' for row in state['events'][first:]),12,'fresh AVStarted missing')
        active=rpc('Player.GetActivePlayers')
        if len(active)!=1 or active[0]['type']!='video' or player.getPlayingFile()!=source:raise RuntimeError('source/player identity differs')
        playerid=active[0]['playerid'];rpc('Player.PlayPause',{'playerid':playerid,'play':False})
        wait(lambda:rpc('Player.GetProperties',{'playerid':playerid,'properties':['speed']})['speed']==0,2,'identity pause missing')
        def selected():
            labels=info()
            return labels if labels.get('Player.Process(videodecoder)')=='ff-h264-vaapi' and labels.get('Player.Process(deintmethod)')=='__DEINT__' else None
        labels=wait(selected,4,'actual original-stock VA decoder/deinterlace __DEINT__ absent')
        settings=rpc('Settings.GetSettingValue',{'setting':'videoplayer.prefervaapirender'})
        labels['actual_prefer_vaapi_renderer']=settings['value']
        if '__DEINT__'=='bwdif':
            labels['actual_video_logging']={key:rpc('Settings.GetSettingValue',{'setting':key})['value']
                for key in ('debug.extralogging','debug.setextraloglevel')}
        gate('identity-'+suffix,'await-identity-'+suffix,cycle=cycle,source=source,info=labels,initial_pause_for_identity=True)
        rpc('Player.PlayPause',{'playerid':playerid,'play':True})
        wait(lambda:rpc('Player.GetProperties',{'playerid':playerid,'properties':['speed']})['speed']==1,2,'normal-speed resume missing')
        def ended():
            fresh=state['events'][first:]
            if any(row['event']=='OnStopped' for row in fresh):raise RuntimeError('natural play became requested Stop')
            callback=any(row['event']=='OnEnded' for row in fresh)
            notified=any(row['event']=='notification' and row.get('method')=='Player.OnStop' and row.get('data',{}).get('end') is True for row in fresh)
            return callback and notified and not rpc('Player.GetActivePlayers')
        wait(ended,60,'30-second source did not reach fresh natural EOF/end:true')
        row={'cycle':cycle,'source':source,'info':labels,'fresh_events':state['events'][first:],
             'natural_end':True,'active_players':rpc('Player.GetActivePlayers'),'monotonic_ns':time.monotonic_ns()}
        state['plays'].append(row)
        gate('retired-'+suffix,'await-retirement-'+suffix,cycle=cycle,source=source,active_players=row['active_players'])
    state['status']='PASS';progress('centered-complete');save('centered-result.json',state)
    # The owned controller now sends one normal Quit, then waits15s before TERM.
except BaseException as error:
    state.update(status='FAIL',error={'type':type(error).__name__,'message':str(error),'traceback':traceback.format_exc()})
    try:player.stop()
    except BaseException as cleanup:state['stop_error']=str(cleanup)
    progress('centered-failed');save('centered-result.json',state)
'''


def parse_rows(raw, reader):
    rows = reader.parse_trace(raw); offsets = []; offset = 0
    for line in raw.splitlines(keepends=True):
        if reader.TRACE_SCHEMA.encode() in line:offsets.append(offset)
        offset += len(line)
    require(len(offsets) == len(rows), 'raw trace offset association differs')
    return list(zip(offsets, rows))


def operation_census(rows, interval, pid, expected_originals=750):
    """Pinned MBAFF inputs: exact source census, two dequeue records each."""
    require(type(expected_originals) is int and expected_originals in (750, 900), 'unsupported original population')
    selected = [row for offset, row in rows if interval['begin'] <= offset < interval['end']]
    require(all(row['pid'] == pid for row in selected), 'foreign PID in source interval')
    fields = BINDING + ('surface', 'owner')
    stages, errors, population_errors = {}, [], []
    for stage in ('submit_bind', 'input_sent', 'output_dequeued', 'materialized'):
        entries = [row for row in selected if row['event'] == stage]
        require(all(all(type(row[k]) is int and row[k] > 0 for k in fields + ('seq',))
                    for row in entries), 'source stage has incomplete binding/sequence identity')
        if any(type(row['outcome']) is not int or row['outcome'] != 0 for row in entries):
            errors.append(stage + ': nonzero or invalid outcome')
        grouped = defaultdict(list)
        for row in entries:grouped[tuple(row[k] for k in fields)].append(row)
        stages[stage] = dict(grouped)
    originals = set(stages['submit_bind'])
    if len(originals) != expected_originals:population_errors.append('require exactly' + str(expected_originals) + ' distinct original bindings')
    if len({key[:len(BINDING)] for key in originals}) != len(originals):
        errors.append('source binding duplicated through different public surface/owner')
    for index, field in enumerate(BINDING[3:], 3):
        identities = Counter((key[:3], key[index]) for key in originals)
        if any(count != 1 for count in identities.values()):
            errors.append('original ' + field + ' is reused within its decoder lifetime')
    for stage, multiplicity in (('submit_bind', 1), ('input_sent', 1), ('output_dequeued', 2)):
        if set(stages[stage]) != originals:errors.append(stage + ': original binding set differs')
        if any(len(records) != multiplicity for records in stages[stage].values()):
            errors.append(stage + ': require exactly' + str(multiplicity) + ' records per original')
    if not set(stages['materialized']) <= originals:
        errors.append('materialized: binding was not an original submission')
    if any(len(records) != 1 for records in stages['materialized'].values()):
        errors.append('materialized: repeated public materialization')
    identities = [(row['driver_instance'], row['seq']) for row in selected if row['event'] in stages]
    if len(identities) != len(set(identities)):errors.append('source stages reuse an instance/sequence record')
    for key in originals:
        submit, inputs, outputs = (stages[stage].get(key, []) for stage in ('submit_bind', 'input_sent', 'output_dequeued'))
        if len(submit) != 1 or len(inputs) != 1 or len(outputs) != 2:continue
        ordered_outputs = sorted(row['seq'] for row in outputs)
        if not submit[0]['seq'] < inputs[0]['seq'] < ordered_outputs[0] < ordered_outputs[1]:
            errors.append('binding stage order differs: submit < input < output < output'); break
        materialized = stages['materialized'].get(key, [])
        if materialized and not all(ordered_outputs[1] < row['seq'] for row in materialized):
            errors.append('materialized precedes both output dequeue records'); break
    return {'status': 'PASS' if not errors and not population_errors else 'FAIL',
            'ledger_status': 'PASS' if not errors else 'FAIL',
            'source_population_qualified': not population_errors,
            'errors': population_errors + errors,
            'event_counts': dict(Counter(row['event'] for row in selected)),
            'distinct_binding_stage_counts': {key: len(value) for key, value in stages.items()},
            'binding_multiplicity_histograms': {stage: dict(sorted(Counter(len(v) for v in groups.values()).items()))
                                               for stage, groups in stages.items()},
            'all_stage_binding_sets_equal': all(set(value) == originals for value in stages.values()),
            'source_metadata_pictures': expected_originals, 'observed_submit_bindings': len(originals),
            'expected_output_dequeues_per_original': 2,
            'output_record_scope': 'Exactly two outcome0 dequeue events for this pinned MBAFF input; these precede field validation. Matching later materialized separately records completed public output; no parity or unique-field-content claim.',
            'actual_media_PTS_to_binding': 'NOT_OBSERVED',
            'scope': 'Exact' + str(expected_originals) + ' original submit/input bindings and' + str(2 * expected_originals) +
                     ' output dequeue records; public materialization checked when present. Synthetic tokens are not source PTS or renderer frame counts.'}


def postfilter_files(source, app):
    return [source / POSTFILTER_IMAGE, source / POSTFILTER_REFERENCE, POSTFILTER_READER,
            POSTFILTER_REFERENCE_WRAPPER, source / 'tests/vaapi-postfilter-reference.c',
            source / 'tests/vaapi-postfilter-probe.c', source / 'tests/vaapi-postfilter-probe.map',
            source / 'tests/vaapi-postfilter-protocol.md', app / CPU_COPY_PROVIDER,
            app / 'lib/libavfilter.so.12.1.102']


def mapped_image(pid, mount, original, pins):
    """Verify a selected image at the held owned-identity gate."""
    proc = Path('/proc', str(pid)); found = set()
    for line in (proc / 'maps').read_text().splitlines():
        parts = line.split(None, 5)
        if len(parts) == 6 and parts[5] == mount:found.add((parts[3], int(parts[4])))
    require(len(found) == 1, 'required selected image mapping missing or ambiguous: ' + mount)
    device, inode = next(iter(found)); target = proc / 'root' / mount.lstrip('/'); item = target.stat()
    require(item.st_ino == inode and tuple(int(p, 16) for p in device.split(':')) ==
            (os.major(item.st_dev), os.minor(item.st_dev)) and digest(target) == pins[str(original)],
            'selected mapped image differs: ' + mount)
    return {'path': mount, 'device': device, 'inode': inode, 'sha256': pins[str(original)]}


def reference_namespace(app, platform, gl, payload, executable):
    """CPU-only original-library namespace, without card/GPU/desktop mounts."""
    return ['bwrap', '--unshare-uts', '--clearenv', '--ro-bind', str(platform), '/usr',
            '--ro-bind', str(platform / 'etc'), '/etc', '--ro-bind', str(app), '/app',
            '--ro-bind', str(gl), '/gl', '--proc', '/proc', '--dev', '/dev', '--tmpfs', '/tmp',
            '--symlink', 'usr/bin', '/bin', '--symlink', 'usr/lib', '/lib',
            '--symlink', 'usr/lib64', '/lib64', '--bind', str(payload), '/payload',
            '--ro-bind', str(executable), '/reference', '--chdir', '/payload',
            '--setenv', 'LD_LIBRARY_PATH',
            '/app/lib:/app/lib/samba:/app/lib/samba/private:/usr/lib/x86_64-linux-gnu:/usr/lib:/gl/lib',
            '--']


def audit_postfilter_case(source, app, platform, gl, case, report):
    """Read retired original-client observations, then replay bounded CPU prefixes."""
    pid = report['owned_app']['pid']; payload = case / 'payload'
    report_path = payload / ('postfilter-' + str(pid) + '.jsonl')
    reader = load('original_postfilter_case_reader', POSTFILTER_READER)
    expected = {'filter': '/app/lib/libavfilter.so.12.1.102',
                'util': '/app/lib/libavutil.so.61.1.102'}
    validated = reader.read_report(report_path, expected_pid=pid, expected_providers=expected)
    summary = reader.audit_report(report_path, expected_pid=pid, expected_providers=expected)
    require(summary.get('status') == 'PASS_SCOPED_POSTFILTER_OBSERVATION',
            'post-filter observation is incomplete: ' + json.dumps(summary))
    source_apis = {'av_buffersrc_add_frame', 'av_buffersrc_add_frame_flags'}
    sink_apis = {'av_buffersink_get_frame', 'av_buffersink_get_frame_flags'}
    by_play = {key: {'graphs': [], 'accepted_filter_inputs': 0, 'filter_outputs': 0,
                     'source_null_calls': 0, 'source_close_calls': 0, 'prefix_references': []}
               for key in report['intervals']}
    wrapper = load('original_postfilter_prefix_reference', POSTFILTER_REFERENCE_WRAPPER)
    prefix = reference_namespace(app, platform, gl, payload, source / POSTFILTER_REFERENCE)
    for graph in validated['graphs']:
        events = graph['events']
        inputs = [e for e in events if e['api'] in source_apis and not e['input_null'] and e['ret'] >= 0]
        outputs = [e for e in events if e['api'] in sink_apis and e['ret'] >= 0]
        if not inputs:
            require(not outputs, 'post-filter output has no accepted input lifetime')
            continue
        cycles = [key for key, interval in report['intervals'].items()
                  if all(interval['begin_ns'] <= e['begin_ns'] <= interval['end_ns'] for e in inputs)]
        require(len(cycles) == 1, 'post-filter input lifetime crosses or misses natural plays')
        play = by_play[cycles[0]]
        play['graphs'].append(graph['graph_id'])
        play['accepted_filter_inputs'] += len(inputs); play['filter_outputs'] += len(outputs)
        play['source_null_calls'] += sum(e['api'] in source_apis and e['input_null'] for e in events)
        play['source_close_calls'] += sum(e['api'] == 'av_buffersrc_close' for e in events)
        if len(inputs) < 3 or len(outputs) < 4:continue
        manifest = reader.reference_prefix(validated, graph['graph_id'])
        name = 'reference-input-graph-' + str(graph['graph_id']) + '.json'
        manifest_path = payload / name; save(manifest_path, manifest)
        reference_dir = payload / ('reference-graph-' + str(graph['graph_id']))
        try:
            result = wrapper.run_reference(manifest, '/reference', reference_dir,
                command_prefix=prefix, path_map=((str(payload), '/payload'),))
        except BaseException:
            handoff = reference_dir / 'HANDOFF_REQUIRED.json'
            if handoff.exists():
                report['live_resource_handoff'] = {
                    'status': 'HANDOFF_REQUIRED_REFERENCE_NAMESPACE',
                    'reference_output_directory': str(reference_dir),
                    'detail': json.loads(handoff.read_text()),
                    'required_action': 'Preserve reference process/namespace/output; no forced kill or further media/module action.'}
            raise
        save(case / ('graph-' + str(graph['graph_id']) + '.reference.json'), result)
        require(result.get('status') == 'PASS',
                'same-input post-filter prefix differs: ' + json.dumps(result))
        joined = reader.join_reference(validated, graph['graph_id'], result)
        require(joined.get('status') == 'PASS_SAME_INPUT_PREFIX',
                'reference is not bound to the observed graph prefix: ' + json.dumps(joined))
        play['prefix_references'].append({'graph_id': graph['graph_id'], 'reference': result, 'join': joined})
    require(len(by_play) == 2 and all(p['accepted_filter_inputs'] > 0 and p['filter_outputs'] > 0
            and p['prefix_references'] for p in by_play.values()),
            'each natural play needs actual inputs/outputs and a compared captured prefix')
    for key, play in by_play.items():
        play['driver_source_originals'] = report['source_census'][key]['observed_submit_bindings']
        play['nominal_two_outputs_per_accepted_input'] = 2 * play['accepted_filter_inputs']
        play['output_minus_nominal'] = play['filter_outputs'] - play['nominal_two_outputs_per_accepted_input']
        play['full_output_cadence'] = 'NOT_QUALIFIED_BY_SELECTED_PREFIX_REFERENCE'
    return {'status': 'PASS_SCOPED_POSTFILTER_OBSERVATION', 'report': str(report_path),
            'observer': summary, 'natural_plays': by_play,
            'reference_scope': 'Same captured NV12 inputs/pinned App BWDIF prefix only; not independent codec/scaler correctness or a full-tail golden.',
            'clock_scope': 'Raw stream/frame/graph/sink clocks remain separate; no source-seconds or physical-cadence inference.'}


def main():
    require(__debug__, 'ordinary Python without -O required')
    p = argparse.ArgumentParser(description=__doc__)
    modes = p.add_mutually_exclusive_group(required=True)
    modes.add_argument('--prepare-plan', action='store_true'); modes.add_argument('--execute', action='store_true')
    p.add_argument('--plan', type=Path, required=True); p.add_argument('--case', default='stock-centered-off-01')
    p.add_argument('--case-root', type=Path, default=ROOT)
    p.add_argument('--own-source', type=Path); p.add_argument('--own-revision'); p.add_argument('--source-probe', type=Path)
    p.add_argument('--cycles', type=int, choices=(1, 2))
    p.add_argument('--scenario', choices=('off750', 'bwdif2997'))
    a = p.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9_-]+', a.case):p.error('fresh simple case name required')
    if not a.prepare_plan and any(v is not None for v in (a.own_source, a.own_revision, a.source_probe, a.cycles, a.scenario)):
        p.error('source/revision/probe/cycles/scenario belong only to --prepare-plan')
    case = a.case_root.resolve() / a.case
    config = scenario(a.scenario or 'off750') if a.prepare_plan else None
    if a.prepare_plan:
        if a.own_source is None or not re.fullmatch(r'[0-9a-f]{40}', a.own_revision or '') or a.source_probe is None:
            p.error('prepare needs own source/full revision and independent packet/frame probe JSON')
        require(not a.plan.exists() and not case.exists(), 'fresh plan and case paths required')
    else:
        # Even helper/plan preflight failures get a concrete case/result.json.
        case.mkdir(mode=0o700)
    report = {'schema': 1, 'case': a.case, 'case_directory': str(case), 'scope': SCOPE, 'status': 'FAIL',
              'errors': [], 'controls_status': 'NOT_RUN', 'identity_checks': [], 'retirements': [],
              'intervals': {}, 'source_census': {}, 'payload_audits': [], 'payload_status_receipts': [],
              'thermal_observations': [], 'application_exit': None, 'live_resource_handoff': None,
              'qualification_failures': [],
              'actual_media_PTS_to_binding': 'NOT_OBSERVED', 'gpu_reader_return': 'NOT_OBSERVED',
              'stock_compatibility': 'NOT_QUALIFIED_BY_PRIVATE_OBSERVER'}
    G = P = boundary = None; before = before_lean = None
    kodi = kid = pidfd = weston = runtime = endpoint = udp = None
    files = []; audited_contexts = set(); due = 0; stop = []; previous_handlers = {}
    since = time.strftime('%Y-%m-%d %H:%M:%S UTC', time.gmtime())

    def health():
        nonlocal due
        require(not stop, 'owned controller stop requested; perform normal media shutdown')
        if time.monotonic() < due:return
        temps = [int(f.read_text()) for f in Path('/sys/class/thermal').glob('thermal_zone*/temp')]
        mains = [f for f in Path('/sys/class/power_supply').glob('*')
                 if (f / 'type').exists() and (f / 'type').read_text().strip() == 'Mains']
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

    def same():
        require(kid is not None, 'owned Kodi identity unavailable')
        now = G.identity(kid['pid'], False)
        require(all(now[k] == kid[k] for k in now), 'owned Kodi identity changed')
        return now

    def alive():
        launch = kodi is not None and kodi.poll() is None
        if pidfd is None:return launch
        poll = select.poll(); poll.register(pidfd, select.POLLIN)
        return launch or not poll.poll(0)

    def wait_exit(seconds):
        end = time.monotonic() + seconds
        while alive() and time.monotonic() < end:time.sleep(.05)
        require(not alive(), 'owned media remains alive')

    try:
        require(digest(BASE) == BASE_SHA, 'qualified namespace/shutdown helper changed')
        G = load('centered_owned_controller_helpers', BASE)
        require(digest(G.ROUTE_PATH) == G.ROUTE_SHA, 'pinned stock route changed')
        route = json.loads(G.ROUTE_PATH.read_text()); P = load('centered_private_payload_reader', READER)
        platform = G.OLD / 'flatpak-user/runtime/org.freedesktop.Platform/x86_64/26.08/e7f7b0fb20662fe92e4eac65d58a57019cefae45a39559c4b50440a88acc0491/files'
        gl = G.OLD / 'flatpak-user/runtime/org.freedesktop.Platform.GL.default/x86_64/26.08/25dbbb0cd3a63b2ef8a24ee948ad43998960f3e2bea713263742cc076840aec2/files'
        if a.prepare_plan:
            source = a.own_source.resolve(strict=True)
            require(Path(__file__).resolve() == source / 'tests/vaapi-centered-payload-stock.py', 'runner is outside frozen own checkout')
            require(subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip() == a.own_revision,
                    'own source HEAD differs from requested revision')
            require(not subprocess.check_output(['git', '-C', str(source), 'status', '--porcelain', '--untracked-files=all'], text=True),
                    'freeze needs a committed clean own source tree')
            probe = a.source_probe.resolve(strict=True); metadata = source_metadata(json.loads(probe.read_text()), Path(config['fixture']), config['name'])
            selected = [Path(__file__), BASE, READER, PROTOCOL, G.ROUTE_PATH, G.BOUNDARY,
                        source / 'filters/vaapi/centered-observer/crystalhd_drv_video.so',
                        source / 'tests/vaapi-centered-payload-backend.cpp', source / 'tests/vaapi-centered-payload-observer.h',
                        source / 'filters/vaapi/crystalhd_drv_video.cpp', source / 'filters/vaapi/crystalhd-trace.h',
                        source / 'filters/vaapi/crystalhd-centered-output.h', source / 'linux_lib/libcrystalhd/libcrystalhd.so.3.6',
                        source / 'tests/kernel-log-check.sh', Path(config['fixture']), probe, G.WESTON / 'bin/weston',
                        G.WESTON / 'lib/x86_64-linux-gnu/libweston-14/gl-renderer.so',
                        G.WESTON / 'lib/x86_64-linux-gnu/libweston-14/headless-backend.so',
                        G.WESTON / 'lib/x86_64-linux-gnu/weston/kiosk-shell.so', Path('/usr/bin/kodi-send'),
                        platform / 'lib/x86_64-linux-gnu/libswscale.so.9.5.103', platform / 'lib/x86_64-linux-gnu/libavutil.so.60.26.103']
            if config['name'] == 'bwdif2997':
                require(a.cycles in (None, 2), 'bwdif2997 requires same-process two natural plays')
                selected += postfilter_files(source, Path(route['client']['app_directory']))
            pins = {str(f): digest(f) for f in selected}
            if config['name'] == 'bwdif2997':require(pins[config['fixture']] == BWDIF_FIXTURE_SHA, 'original short input changed')
            for path, expected in route['client']['pins'].items():
                require(digest(path) == expected, 'original client artifact changed: ' + path); pins[path] = expected
            save(a.plan, {'schema': 1, 'status': 'FROZEN_SOURCE_PLAN', 'source_directory': str(source), 'revision': a.own_revision,
                          'case_directory': str(case), 'case': a.case, 'cycles': (2 if config['name'] == 'bwdif2997' else a.cycles or 1),
                          'scenario': config,
                          'platform_directory': str(platform), 'gl_directory': str(gl), 'pins': pins,
                          'source_probe': str(probe), 'fixture': config['fixture'], 'source_metadata': metadata,
                          'explicit_interlace_method': config['method'], 'expected_videodecoder': 'ff-h264-vaapi', 'expected_deintmethod': config['deintmethod'],
                          'prefer_vaapi_renderer': config['prefer_vaapi_renderer'], 'centered_environment': CENTERED, 'slots_per_context': 2,
                          'maximum_decoder_contexts': 2, 'publication': 'selected-decoder-context-retirement',
                          'source_refs': [SOURCE_REF + 'xbmc/cores/VideoSettings.h', SOURCE_REF + 'xbmc/cores/VideoPlayer/DVDCodecs/Video/VAAPI.cpp'],
                          'scope': config['scope'], 'scheduling': 'ordinary inherited scheduling; no priority changes',
                          'initial_pause': 'Identity gate only, then normal-speed natural EOF',
                          'cleanup': 'Application.Quit15s; verified pidfd TERM15s; surviving media preserves runtime/profile/compositor'})
            print('FROZEN_FILES_AND_SOURCE_CENSUS ' + str(a.plan)); return 0

        require(hasattr(os, 'pidfd_open') and hasattr(signal, 'pidfd_send_signal'), 'owned shutdown requires pidfds')
        os.umask(0o077); resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        plan = json.loads(a.plan.read_text())
        config = scenario(plan.get('scenario', {}).get('name', 'off750'))
        require(plan.get('scenario', config) == config, 'frozen scenario differs')
        fixture = Path(config['fixture'])
        require(plan['schema'] == 1 and plan['status'] == 'FROZEN_SOURCE_PLAN' and plan['case'] == a.case and
                plan['case_directory'] == str(case), 'frozen case association differs')
        require(plan['cycles'] in (1, 2) and plan['explicit_interlace_method'] == config['method'] and
                plan['expected_videodecoder'] == 'ff-h264-vaapi' and plan['expected_deintmethod'] == config['deintmethod'] and
                plan['prefer_vaapi_renderer'] is config['prefer_vaapi_renderer'] and plan['centered_environment'] == CENTERED and
                plan['slots_per_context'] == plan['maximum_decoder_contexts'] == 2 and
                plan['publication'] == 'selected-decoder-context-retirement', 'frozen protocol differs')
        for path, expected in plan['pins'].items():require(digest(path) == expected, 'frozen artifact changed: ' + path)
        G.SOURCE = Path(plan['source_directory']).resolve(strict=True); G.REVISION = plan['revision']
        G.VA = G.SOURCE / 'filters/vaapi/centered-observer/crystalhd_drv_video.so'
        G.DIL = G.SOURCE / 'linux_lib/libcrystalhd/libcrystalhd.so.3.6'
        require(Path(__file__).resolve() == G.SOURCE / 'tests/vaapi-centered-payload-stock.py',
                'executed runner outside frozen own checkout')
        required = (Path(__file__), BASE, READER, PROTOCOL, G.ROUTE_PATH, G.BOUNDARY, G.VA, G.DIL,
                    fixture, Path(plan['source_probe']), G.SOURCE / 'filters/vaapi/crystalhd_drv_video.cpp',
                    G.SOURCE / 'tests/vaapi-centered-payload-backend.cpp', G.SOURCE / 'tests/vaapi-centered-payload-observer.h',
                    G.SOURCE / 'tests/kernel-log-check.sh')
        if config['name'] == 'bwdif2997':
            require(plan['cycles'] == 2 and plan['pins'][str(fixture)] == BWDIF_FIXTURE_SHA, 'frozen short two-play case differs')
            required += tuple(postfilter_files(G.SOURCE, Path(route['client']['app_directory'])))
        require(all(str(path) in plan['pins'] for path in required), 'required frozen artifact pin missing')
        require(all(plan['pins'].get(path) == expected for path, expected in route['client']['pins'].items()),
                'original App/FFmpeg provenance pin differs')
        require(subprocess.check_output(['git', '-C', str(G.SOURCE), 'rev-parse', 'HEAD'], text=True).strip() == G.REVISION,
                'frozen own source revision changed')
        require(not subprocess.check_output(['git', '-C', str(G.SOURCE), 'status', '--porcelain', '--untracked-files=all'], text=True),
                'execution requires committed clean own source')
        require(str(platform) == plan['platform_directory'] and str(gl) == plan['gl_directory'] and plan['fixture'] == str(fixture),
                'frozen runtime/source path differs')
        require(source_metadata(json.loads(Path(plan['source_probe']).read_text()), fixture, config['name']) == plan['source_metadata'],
                'independent source census changed')
        report.update(revision=G.REVISION, source_directory=str(G.SOURCE), plan=str(a.plan.resolve()), pins=plan['pins'],
                      source_metadata=plan['source_metadata'], cycles=plan['cycles'], scenario=config, scope=config['scope'], source_refs=plan['source_refs'], scheduling=plan['scheduling'])
        if config['name'] == 'bwdif2997':
            for signum in (signal.SIGINT, signal.SIGTERM):
                previous_handlers[signum] = signal.getsignal(signum)
                signal.signal(signum, lambda received, _frame: stop.append(received))
        boundary = load('centered_initial_boundary', G.BOUNDARY); before = boundary.guard(); report['before'] = before
        require(before['debug_rows'] == 85 and before['debug_sha256'] ==
                '4e2758e4d68361f8783c48dbae8bd38ce06c380155a0f9de294d63d888c7a2f4', 'initial dynamic-debug policy differs')
        before_lean = {k: before[k] for k in POLL_KEYS}
        require(all(int(line.split()[-1]) == 0 for text in before['aer']['0000:02:00.0'].values()
                    for line in text.splitlines() if line.strip()), 'initial endpoint AER nonzero')
        health()
        profile, run, payload = case / 'profile', case / 'run', case / 'payload'
        profile.mkdir(); run.mkdir(); payload.mkdir(mode=0o700); (run / 'deps').mkdir()
        (run / 'controls.py').write_text(SCRIPT.replace('__CYCLES__', str(plan['cycles'])).replace('__DEINT__', config['deintmethod']))
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reserve:
            reserve.bind(('127.0.0.1', 0)); udp = reserve.getsockname()[1]
        settings = {'services.webserver': 'false', 'services.esenabled': 'true', 'services.esport': str(udp),
                    'services.esportrange': '1', 'services.esallinterfaces': 'false', 'addons.unknownsources': 'true',
                    'addons.updatemode': '2', 'screensaver.mode': '', 'weather.addon': '', 'lookandfeel.soundskin': '',
                    'videoplayer.usevaapi': 'true', 'videoplayer.usevaapih264': 'true', 'videoplayer.prefervaapirender': str(config['prefer_vaapi_renderer']).lower(),
                    'videoplayer.useprimedecoder': 'false', 'videoplayer.useprimedecoderforhw': 'false'}
        if config['name'] == 'bwdif2997':
            settings.update({'debug.extralogging': 'true', 'debug.setextraloglevel': '32768'})
        userdata = profile / 'userdata'; userdata.mkdir()
        xml = '<settings version="2">' + ''.join('<setting id="' + escape(k) + '">' + escape(v) + '</setting>'
                                                 for k, v in settings.items())
        xml += '<defaultvideosettings><interlacemethod>' + str(config['method']) + '</interlacemethod><scalingmethod>0</scalingmethod><viewmode>0</viewmode><zoomamount>1</zoomamount><pixelratio>1</pixelratio><verticalshift>0</verticalshift></defaultvideosettings></settings>'
        (userdata / 'guisettings.xml').write_text(xml)
        (userdata / 'advancedsettings.xml').write_text('<advancedsettings><loglevel>1</loglevel></advancedsettings>')
        runtime = Path(tempfile.mkdtemp(prefix='ch-centered-', dir='/run/user/' + str(os.getuid())))
        lib = G.WESTON / 'lib/x86_64-linux-gnu'
        clean_env = {k: v for k, v in os.environ.items() if not k.startswith(('LD_', 'GST_', 'CRYSTALHD_', 'LIBVA_'))}
        compositor_env = dict(clean_env, XDG_RUNTIME_DIR=str(runtime), WESTON_DATA_DIR=str(G.WESTON / 'share/weston'),
                              WESTON_MODULE_MAP=';'.join(n + '=' + str(lib / 'libweston-14' / n)
                                                        for n in ('gl-renderer.so', 'headless-backend.so')),
                              LD_LIBRARY_PATH=str(lib) + ':' + str(lib / 'weston'))
        log = (case / 'weston.stdout').open('x'); files.append(log); sock = 'stock-centered'
        weston = subprocess.Popen([str(G.WESTON / 'bin/weston'), '--backend=headless', '--renderer=gl',
                                   '--shell=' + str(lib / 'weston/kiosk-shell.so'), '--width=1920', '--height=1080',
                                   '--refresh-rate=60000', '--socket=' + sock, '--idle-time=0', '--no-config',
                                   '--log=' + str(case / 'weston.log')], env=compositor_env, stdin=subprocess.DEVNULL,
                                  stdout=log, stderr=subprocess.STDOUT)
        wait(lambda: (runtime / sock).exists() if weston.poll() is None else False, 8, 'owned Weston readiness')
        command = G.namespace(Path(route['client']['app_directory']), platform, gl, case, runtime, sock, fixture)
        at = command.index('--')
        command[at:at] = ['--bind', str(payload), '/payload', '--setenv', 'CRYSTALHD_TEST_CENTERED_PAYLOAD_DIR', '/payload',
                         '--setenv', 'CRYSTALHD_VAAPI_EXPERIMENTAL_CENTERED_NV12', CENTERED]
        if config['name'] == 'bwdif2997':
            command[at:at] = ['--ro-bind', str(G.SOURCE / POSTFILTER_IMAGE), POSTFILTER_MOUNT,
                             '--setenv', 'LD_PRELOAD', POSTFILTER_MOUNT,
                             '--setenv', 'CRYSTALHD_TEST_POSTFILTER', 'v1',
                             '--setenv', 'CRYSTALHD_TEST_POSTFILTER_PREFIX', '/payload/postfilter']
        require('--die-with-parent' not in command, 'media runtime must survive operator handoff')
        save(case / 'command.json', command); log = (case / 'kodi.stdout').open('x'); files.append(log)
        kodi = subprocess.Popen(command, env=clean_env, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT)

        def discover():
            require(kodi.poll() is None, 'owned Kodi launcher exited before readiness')
            for candidate in G.descendants(kodi.pid):
                try:
                    if Path('/proc', str(candidate), 'comm').read_text().strip() == 'kodi.bin':
                        value = G.identity(candidate)
                        if value['uid'] == os.getuid() and value['executable_sha256'] == route['client']['pins'][
                                str(Path(route['client']['app_directory']) / 'lib/kodi/kodi.bin')]:return value
                except FileNotFoundError:pass
        kid = wait(discover, 30, 'original owned Kodi readiness')
        report.update(owned_app=kid, owned_launcher_pid=kodi.pid)
        pidfd = os.pidfd_open(kid['pid'], 0); same()

        def find_endpoint():
            same()
            try:return G.udp_owner(kid['pid'], udp)
            except AssertionError:return None
        endpoint = wait(find_endpoint, 20, 'owned EventServer readiness')
        subprocess.run(['/usr/bin/kodi-send', '--host=127.0.0.1', '--port=' + str(udp),
                        '--action=RunScript(/probe/controls.py)'], check=True, stdout=subprocess.DEVNULL, timeout=5)
        dependencies = {'/usr/lib/x86_64-linux-gnu/libswscale.so.9.5.103': str(platform / 'lib/x86_64-linux-gnu/libswscale.so.9.5.103'),
                        '/usr/lib/x86_64-linux-gnu/libavutil.so.60.26.103': str(platform / 'lib/x86_64-linux-gnu/libavutil.so.60.26.103')}

        def gate(name):save(profile / (name + '.ok'), {'verified': True})

        def retire(cycle):
            samples = []
            def idle():
                same()
                sample = {'monotonic_ns': time.monotonic_ns(), 'boundary': lean_snapshot(), 'owned_decoder_fds': G.owned_fds(kid['pid'])}
                samples.append(sample)
                return sample['boundary'] == before_lean and sample['owned_decoder_fds'] == []
            wait(idle, 10, 'normal decoder retirement cycle ' + str(cycle))
            time.sleep(.25); require(idle(), 'decoder retirement not stable')
            report['retirements'].append({'cycle': cycle, 'samples': samples})

        def audit_retired(cycle):
            require((case / 'kodi.stdout').stat().st_size <= P.MAX_TRACE_BYTES, 'bounded raw trace exceeded')
            raw = (case / 'kodi.stdout').read_bytes(); rows = parse_rows(raw, P)
            interval = report['intervals'][str(cycle)]
            interval.update(end=len(raw), end_ns=time.monotonic_ns()); matches = []
            for path in sorted(payload.glob('centered-payload-*.json')):
                document = json.loads(P.read_regular(path, P.MAX_MANIFEST_BYTES), object_pairs_hook=P.strict_object)
                require(document.get('schema') == P.SCHEMA and document.get('pid') == kid['pid'], 'manifest schema/PID differs')
                publication = document['publication']
                key = (document['driver_instance'], publication['context'], publication['generation'])
                if key not in audited_contexts:matches.append((path, document, key))
            require(len(matches) == 1, 'each finite play requires one new retired two-slot decoder manifest')
            manifest, document, key = matches[0]
            require(len(document['slots']) == 2, 'retired context lacks both selected committed slots')
            for slot in document['slots']:
                binding = slot['binding']
                hits = [offset for offset, row in rows if row['pid'] == kid['pid'] and row['driver_instance'] == key[0]
                        and row['event'] == 'submit_bind' and all(row[name] == binding[name] for name in P.BINDING_KEYS)]
                require(len(hits) == 1 and interval['begin'] <= hits[0] < interval['end'], 'payload outside frozen source interval')
            destroyed = [offset for offset, row in rows if row['pid'] == kid['pid'] and row['driver_instance'] == key[0]
                         and row['event'] == 'context_destroy' and (row['context'], row['generation']) == key[1:]]
            require(len(destroyed) == 1 and interval['begin'] <= destroyed[0] < interval['end'], 'retirement outside source interval')
            trace = case / ('cycle-' + str(cycle) + '.trace-prefix.log')
            with trace.open('xb') as stream:stream.write(raw)
            result = P.audit(manifest, trace); save(case / ('cycle-' + str(cycle) + '.payload-audit.json'), result)
            report['payload_audits'].append({'cycle': cycle, 'manifest': str(manifest), 'trace_prefix': str(trace), 'audit': result})
            require(result.get('status') == 'PASS_SCOPED_OWN_CENTERED_NV12_PAYLOAD',
                    'independent retired-context payload audit failed: ' + json.dumps(result))
            audited_contexts.add(key)
            census = operation_census(rows, interval, kid['pid'], config['pictures']); report['source_census'][str(cycle)] = census
            if config['name'] == 'bwdif2997':
                require(census['ledger_status'] == 'PASS', 'observed native ownership/field ledger failed')
                if not census['source_population_qualified']:
                    report['qualification_failures'].append({'cycle': cycle, 'criterion': 'complete_source_population',
                        'expected_originals': config['pictures'], 'observed_originals': census['observed_submit_bindings'],
                        'status': 'FAIL', 'disposition': 'Retain full-source failure while completing diagnostic observation; no lower acceptance count.'})
            else:require(census['status'] == 'PASS', 'exact source input/output binding census failed')

        handled = set(); deadline = time.monotonic() + 65 + plan['cycles'] * 100
        while time.monotonic() < deadline:
            result_path = profile / 'centered-result.json'
            if result_path.exists():
                report['controls'] = json.loads(result_path.read_text()); report['controls_status'] = report['controls']['status']; break
            health(); same(); progress_path = profile / 'centered-progress.json'
            if not progress_path.exists():time.sleep(.05); continue
            value = json.loads(progress_path.read_text()); phase = value['phase']
            if phase in handled:time.sleep(.05); continue
            if phase.startswith('await-'):
                cycle = value['cycle']; require(1 <= cycle <= plan['cycles'], 'unexpected cycle'); suffix = str(cycle)
                if phase == 'await-open-' + suffix:
                    require(lean_snapshot() == before_lean and not G.owned_fds(kid['pid']), 'decoder not idle before play')
                    report['intervals'][suffix] = {'begin': (case / 'kodi.stdout').stat().st_size, 'begin_ns': time.monotonic_ns()}
                    gate('open-' + suffix)
                elif phase == 'await-identity-' + suffix:
                    labels = value['info']
                    require(labels['Player.Process(videodecoder)'] == 'ff-h264-vaapi' and
                            labels['Player.Process(deintmethod)'] == config['deintmethod'] and value['source'] == '/fixture.mp4', 'actual stock VA deinterlace route differs')
                    fds = G.owned_fds(kid['pid']); require(any(f['target'] == '/dev/crystalhd' for f in fds), 'hardware decoder FD absent')
                    check = {'cycle': cycle, 'info': labels, 'owned_fds': fds, 'same_app': same()}
                    require(type(labels.get('actual_prefer_vaapi_renderer')) is bool and
                            labels['actual_prefer_vaapi_renderer'] is config['prefer_vaapi_renderer'], 'actual renderer preference differs')
                    if not report['identity_checks']:check['mapped_images'] = G.mappings(kid['pid'], plan['pins'], dependencies)
                    if config['name'] == 'bwdif2997':
                        require(labels.get('actual_video_logging') ==
                            {'debug.extralogging': True, 'debug.setextraloglevel': [32768]},
                            'original video-component diagnostics not enabled as frozen')
                        app = Path(route['client']['app_directory'])
                        check['postfilter_images'] = [mapped_image(kid['pid'], mount, original, plan['pins'])
                            for mount, original in ((POSTFILTER_MOUNT, G.SOURCE / POSTFILTER_IMAGE),
                                ('/app/' + str(CPU_COPY_PROVIDER), app / CPU_COPY_PROVIDER),
                                ('/app/lib/libavfilter.so.12.1.102', app / 'lib/libavfilter.so.12.1.102'))]
                    report['identity_checks'].append(check); gate('identity-' + suffix)
                elif phase == 'await-retirement-' + suffix:
                    require(value['active_players'] == [], 'EOF left active player')
                    retire(cycle); audit_retired(cycle); gate('retired-' + suffix)
                else:raise ValueError('unknown application gate ' + phase)
                handled.add(phase)
            elif phase == 'centered-failed':raise RuntimeError('stock script failed; original result retains reason')
            time.sleep(.05)
        else:raise TimeoutError('finite centered protocol deadline')
        require(report['controls_status'] == 'PASS' and len(report['payload_audits']) == plan['cycles'], 'stock controls/retirement incomplete')
    except BaseException as error:
        report['errors'].append({'step': 'prepare' if a.prepare_plan else 'execution', 'error': fault(error)})
        if a.prepare_plan:raise
        if (case / 'profile').is_dir():
            try:
                save(case / 'profile/controller-abort.json', {'schema': 1, 'status': 'CONTROLLER_ABORT',
                    'monotonic_ns': time.monotonic_ns(), 'error': {'type': type(error).__name__, 'message': str(error)[:4096]}})
            except BaseException as abort_error:
                report['errors'].append({'step': 'controller-abort-signal', 'error': fault(abort_error)})
    finally:
        if not a.prepare_plan:
            try:
                live = alive()
                if kodi is not None and G is not None:
                    def normal_quit():
                        require(endpoint is not None, 'owned EventServer endpoint unavailable')
                        same(); require(G.udp_owner(kid['pid'], udp) == endpoint, 'owned EventServer endpoint changed')
                        subprocess.run(['/usr/bin/kodi-send', '--host=127.0.0.1', '--port=' + str(udp), '--action=Quit'],
                                       check=True, stdout=subprocess.DEVNULL, timeout=5)
                    def term():
                        require(pidfd is not None, 'verified media pidfd unavailable')
                        same(); signal.pidfd_send_signal(pidfd, signal.SIGTERM)
                    shutdown = G.normal_media_shutdown(alive, normal_quit, term, wait_exit)
                    report['media_shutdown'] = shutdown
                    report['errors'].extend(shutdown['errors']); live = alive()
                report['application_exit'] = kodi.poll() if kodi is not None else None
                if live:
                    report['live_resource_handoff'] = {'status': 'HANDOFF_REQUIRED_MEDIA_STILL_ALIVE', 'launcher_pid': kodi.pid,
                        'last_verified_kodi_identity': kid, 'compositor_pid': weston.pid if weston else None,
                        'private_runtime': str(runtime), 'profile': str(case / 'profile'),
                        'required_action': 'Preserve runtime/profile/compositor; verify PID/start/executable before ordinary Quit/TERM. No kill/unload/retry.'}
                elif weston is not None:
                    try:
                        if weston.poll() is None:weston.terminate(); weston.wait(timeout=10)
                    except BaseException as error:report['errors'].append({'step': 'Weston-close', 'error': fault(error)})
                    if weston.poll() is None:
                        report['live_resource_handoff'] = {'status': 'HANDOFF_REQUIRED_COMPOSITOR_STILL_ALIVE',
                                                           'compositor_pid': weston.pid, 'private_runtime': str(runtime)}
                if report['live_resource_handoff'] is not None:save(case / 'live-resource-handoff.json', report['live_resource_handoff'])
                for stream in files:stream.close()
                if pidfd is not None:os.close(pidfd)
                if runtime is not None and not live and (weston is None or weston.poll() is not None):
                    import shutil
                    shutil.rmtree(runtime)
                if before_lean is not None and not live:
                    try:
                        end = time.monotonic() + 10; after = lean_snapshot()
                        while after != before_lean and time.monotonic() < end:time.sleep(.25); after = lean_snapshot()
                        report['after'] = after; report['boundary_equal'] = after == before_lean
                        require(report['boundary_equal'], 'terminal lean ownership/power boundary differs')
                    except BaseException as error:report['errors'].append({'step': 'terminal-boundary', 'error': fault(error)})
                if before is not None:
                    try:
                        with (case / 'kernel.log').open('x') as out:
                            report['kernel_exit'] = subprocess.run(['sudo', '-n', 'sh', str(G.SOURCE / 'tests/kernel-log-check.sh'), since],
                                                                  stdout=out, stderr=subprocess.STDOUT, timeout=15).returncode
                    except BaseException as error:report['errors'].append({'step': 'kernel-check', 'error': fault(error)})
                if P is not None and (case / 'kodi.stdout').exists():
                    try:
                        require((case / 'kodi.stdout').stat().st_size <= P.MAX_TRACE_BYTES, 'bounded raw trace exceeded')
                        raw = (case / 'kodi.stdout').read_bytes(); report['raw_stdout_bytes'] = len(raw)
                        for line in raw.splitlines():
                            if line.startswith(b'{"schema":"crystalhd-centered-payload-status-v1"'):
                                row = json.loads(line, object_pairs_hook=P.strict_object)
                                require(kid is not None and row['pid'] == kid['pid'] and row['after_driver_unlock'] is True,
                                        'status receipt PID/unlocked publication differs')
                                report['payload_status_receipts'].append(row)
                        require(len(report['payload_status_receipts']) <= 16, 'retirement status receipt bound exceeded')
                        result_path = case / 'profile/centered-result.json'
                        if result_path.exists():
                            report['controls'] = json.loads(result_path.read_text()); report['controls_status'] = report['controls']['status']
                        if report['controls_status'] == 'PASS':
                            rows = parse_rows(raw, P)
                            require(all(row['pid'] == kid['pid'] for _, row in rows), 'foreign OWN trace PID')
                            for instance in {row['driver_instance'] for _, row in rows}:
                                P.trace_index([row for _, row in rows], kid['pid'], instance)
                            intervals = list(report['intervals'].values())
                            require(len(intervals) == plan['cycles'] and all('end' in v for v in intervals), 'source intervals incomplete')
                            for offset, row in rows:
                                if row['event'] in ('submit_bind', 'input_sent', 'output_dequeued', 'materialized', 'prefeed_sent'):
                                    require(sum(v['begin'] <= offset < v['end'] for v in intervals) == 1,
                                            'source work outside frozen natural-play intervals')
                    except BaseException as error:report['errors'].append({'step': 'final-observation', 'error': fault(error)})
                if (config is not None and config['name'] == 'bwdif2997' and kid is not None and
                        report['application_exit'] == 0 and report['controls_status'] == 'PASS' and
                        not report['errors'] and report['live_resource_handoff'] is None):
                    try:
                        health()
                        report['postfilter_audit'] = audit_postfilter_case(G.SOURCE,
                            Path(route['client']['app_directory']), platform, gl, case, report)
                        health()
                    except BaseException as error:report['errors'].append({'step': 'postfilter-observation/reference', 'error': fault(error)})
            except BaseException as error:
                report['errors'].append({'step': 'cleanup', 'error': fault(error)})
                # An unexpected cleanup error never authorizes runtime removal.
                if kodi is not None or weston is not None:
                    report['live_resource_handoff'] = report['live_resource_handoff'] or {
                        'status': 'HANDOFF_REQUIRED_CLEANUP_INCOMPLETE', 'launcher_pid': kodi.pid if kodi else None,
                        'last_verified_kodi_identity': kid, 'compositor_pid': weston.pid if weston else None,
                        'private_runtime': str(runtime), 'profile': str(case / 'profile')}
            finally:
                report['excluded_heap'] = 'No observer/investigation/reproduction; ordinary exit status only retained.'
                payload_pass = bool(report['payload_audits']) and all(
                    row['audit'].get('status') == 'PASS_SCOPED_OWN_CENTERED_NV12_PAYLOAD' for row in report['payload_audits'])
                census_pass = bool(report['source_census']) and all(v['status'] == 'PASS' for v in report['source_census'].values())
                postfilter_pass = (config is None or config['name'] != 'bwdif2997' or
                    report.get('postfilter_audit', {}).get('status') == 'PASS_SCOPED_POSTFILTER_OBSERVATION')
                complete_observation = (payload_pass and report['controls_status'] == 'PASS' and postfilter_pass and
                    report['application_exit'] == 0 and report.get('boundary_equal') and report.get('kernel_exit') == 0 and
                    not report['errors'] and report['live_resource_handoff'] is None)
                report['status'] = 'PASS' if complete_observation and census_pass else 'FAIL'
                if (config is not None and config['name'] == 'bwdif2997' and complete_observation and
                        report['source_census'] and all(v['ledger_status'] == 'PASS' for v in report['source_census'].values())):
                    report['diagnostic_observation'] = 'PASS_SCOPED_POSTFILTER_OBSERVATION'
                    report['complete_source_population_qualification'] = 'PASS' if census_pass else 'FAIL'
                    if not census_pass:report['status'] = 'OBSERVED_SOURCE_QUALIFICATION_FAIL'
                save(case / 'result.json', report)
    for signum, handler in previous_handlers.items():signal.signal(signum, handler)
    print(a.case + ': ' + report['status'] + '; scoped OWN centered NV12 payload observation')
    return 0 if report['status'] == 'PASS' else 1


if __name__ == '__main__':
    try:raise SystemExit(main())
    except Exception as error:
        print('stock-centered-payload: ' + str(error), file=sys.stderr)
        raise SystemExit(1)
