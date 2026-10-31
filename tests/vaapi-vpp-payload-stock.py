#!/usr/bin/python3
"""Private OWN payload backend, two finite stock VA Bob plays; opt-in only.

Freeze requires independently collected software FFprobe JSON for each input.
No external code is edited; no firmware/module switch or heap observer exists.
The diagnostic backend supplies eight OWN completed NV12 field checkpoints.
Normal Quit/verified pidfd TERM ownership is inherited from the qualified helper.
"""
import argparse
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
from xml.sax.saxutils import escape

sys.dont_write_bytecode=True
ROOT=Path('/home/mache/.cache/crystalhd-all-work-20261031')
BASE_PATH=ROOT/'stock-gray-checkpoints.py'
BASE_SHA='dfec342196ca33fe27e57b9b74451c5378500bb34acb4dc2f17bb20596e1f260'
READER_PATH=ROOT/'stock-bob-eof-observer.py'
PAYLOAD_READER=Path(__file__).with_name('vaapi-vpp-payload-reader.py')
OPERATOR_ORIGIN=ROOT/'stock-bob-eof.py'
OPERATOR_ORIGIN_SHA='74716c7db9d74bd7868438247957ed5f95ffacf06e38358563416baa34a91b75'
OPERATION_READER_SHA='33a82d85aa0a193695446e595b0b63c4996c440ea4260ad5340941066d52c5b6'
INPUT_ROOT=Path('/home/mache/.cache/crystalhd-single-field-20261015.boremY')
INPUTS={'tff':('h264-tff-1280x720-2s.mp4','847249796f3ff572a82421b0573aa3c5aaa4856b2a6901bb874204466b1409f3'),
        'bff':('h264-bff-1280x720-2s.mp4','ea902de5e02fc530f18fc8597bd476e185fa94f84c4a8ff3d4f94a0789afc779')}
SOURCE_REF='https://github.com/xbmc/xbmc/blob/28ea2eac1eb7af8fdcbd2672d933ce87f594be79/'
SCOPE='Unchanged original App/FFmpeg, OWN diagnostic VA Bob backend;50 source bindings/100 VPP operations per TFF/BFF and eight full active NV12 OWN source/target field checkpoints; source/output PTS,GPU lifetime,chronological presentation,scanout NOT_RUN'


def load(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m


SCRIPT=r'''
import json,os,threading,time,traceback,xbmc,xbmcgui
from pathlib import Path
R=Path('/profile');lock=threading.RLock();phase='starting';rpcid=0
state={'schema':1,'status':'RUNNING','phase':phase,'events':[],'rpc':[],'plays':[],
       'scope':'Two finite original-stock explicit VA Bob plays; no field-content/presentation census.'}
def save(name,value):
    with lock:
        temporary=R/(name+'.tmp');temporary.write_text(json.dumps(value,indent=2)+'\n');os.replace(temporary,R/name)
def progress(p,**fields):
    global phase
    phase=p
    with lock:state.update(phase=p,monotonic_ns=time.monotonic_ns(),**fields);save('bob-progress.json',state)
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
    if monitor.abortRequested() or state.get('overflow'):raise RuntimeError('application abort/observer overflow')
    if any(e['event']=='OnError' for e in state['events']):raise RuntimeError('playback error')
    xbmc.sleep(20)
def wait(test,seconds,message):
    end=time.monotonic()+seconds
    while time.monotonic()<end:
        value=test()
        if value:return value
        pump()
    raise TimeoutError(message)
def gate(name,p,**fields):
    progress(p,**fields);wait(lambda:(R/(name+'.ok')).exists(),20,'owned root gate '+name)
def info():
    return rpc('XBMC.GetInfoLabels',{'labels':['Player.Process(videodecoder)','Player.Process(pixformat)',
        'Player.Process(deintmethod)','Player.Filenameandpath','System.ScreenWidth','System.ScreenHeight']})
try:
    for order in ('tff','bff'):
        source='/fixture-'+order+'.mp4'
        gate('open-'+order,'await-open-'+order,source_case=order,source=source)
        first=len(state['events']);item=xbmcgui.ListItem(path=source);item.setContentLookup(False);player.play(source,item)
        wait(lambda:any(e['event']=='OnAVStarted' for e in state['events'][first:]),10,'fresh AVStarted missing')
        active=rpc('Player.GetActivePlayers')
        if len(active)!=1 or active[0]['type']!='video' or player.getPlayingFile()!=source:raise RuntimeError('source/player identity differs')
        pid=active[0]['playerid'];rpc('Player.PlayPause',{'playerid':pid,'play':False})
        wait(lambda:rpc('Player.GetProperties',{'playerid':pid,'properties':['speed']})['speed']==0,2,'initial identity pause missing')
        def selected():
            labels=info()
            return labels if labels.get('Player.Process(videodecoder)')=='ff-h264-vaapi' and labels.get('Player.Process(deintmethod)')=='vaapi-bob' else None
        labels=wait(selected,3,'actual original-stock VA Bob selection absent')
        gate('identity-'+order,'await-identity-'+order,source_case=order,source=source,info=labels,initial_pause_for_identity=True)
        rpc('Player.PlayPause',{'playerid':pid,'play':True})
        wait(lambda:rpc('Player.GetProperties',{'playerid':pid,'properties':['speed']})['speed']==1,2,'normal-speed resume missing')
        def ended():
            fresh=state['events'][first:]
            if any(e['event']=='OnStopped' for e in fresh):raise RuntimeError('finite natural play became requested Stop')
            callback=any(e['event']=='OnEnded' for e in fresh)
            notified=any(e['event']=='notification' and e.get('method')=='Player.OnStop' and e.get('data',{}).get('end') is True for e in fresh)
            return callback and notified and not rpc('Player.GetActivePlayers')
        wait(ended,15,'fresh natural EOF/end:true missing')
        row={'source_case':order,'source':source,'info':labels,'fresh_events':state['events'][first:],
             'natural_end':True,'active_players':rpc('Player.GetActivePlayers'),'monotonic_ns':time.monotonic_ns()}
        state['plays'].append(row)
        gate('retired-'+order,'await-retirement-'+order,source_case=order,source=source,active_players=row['active_players'])
    state['status']='PASS';progress('bob-complete');save('bob-result.json',state)
    state['quit_reply']=rpc('Application.Quit');save('bob-result.json',state)
except BaseException as error:
    state.update(status='FAIL',error={'type':type(error).__name__,'message':str(error),'traceback':traceback.format_exc()})
    try:player.stop()
    except BaseException as cleanup:state['stop_error']=str(cleanup)
    progress('bob-failed');save('bob-result.json',state)
'''


def main():
    if not __debug__:raise RuntimeError('ordinary Python without -O is required')
    p=argparse.ArgumentParser(description=__doc__)
    modes=p.add_mutually_exclusive_group()
    modes.add_argument('--execute',action='store_true');modes.add_argument('--prepare-plan',action='store_true')
    p.add_argument('--plan',type=Path,required=True);p.add_argument('--case',default='stock-bob-payload-03')
    p.add_argument('--own-source',type=Path);p.add_argument('--own-revision');p.add_argument('--tff-probe',type=Path);p.add_argument('--bff-probe',type=Path)
    a=p.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9_-]+',a.case):p.error('fresh simple case name required')
    if not a.prepare_plan and any(value is not None for value in
            (a.own_source,a.own_revision,a.tff_probe,a.bff_probe)):
        p.error('source/revision/probe arguments belong only to --prepare-plan')
    if a.prepare_plan and a.own_source is not None and a.own_revision is not None:
        prepared_source = a.own_source.resolve(strict=True)
        actual_revision = subprocess.check_output(['git','-C',str(prepared_source),'rev-parse','HEAD'],text=True).strip()
        if actual_revision != a.own_revision:
            p.error('own source HEAD differs from requested frozen revision')
        if subprocess.check_output(['git','-C',str(prepared_source),'status','--porcelain','--untracked-files=all'],text=True):
            p.error('freeze requires a committed, clean own source tree')
    # Importing this previously qualified OWN helper defines functions only.
    import hashlib
    def digest(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
    assert digest(BASE_PATH)==BASE_SHA,'qualified own shutdown/namespace helper changed'
    assert digest(OPERATOR_ORIGIN)==OPERATOR_ORIGIN_SHA,'qualified original Bob operator changed'
    assert digest(READER_PATH)==OPERATION_READER_SHA,'qualified operation reader changed'
    G=load('bob_owned_controller_helpers',BASE_PATH);R=load('bob_operation_reader',READER_PATH)
    P=load('bob_payload_reader',PAYLOAD_READER)
    assert G.digest(G.ROUTE_PATH)==G.ROUTE_SHA
    route=json.loads(G.ROUTE_PATH.read_text())
    platform=G.OLD/'flatpak-user/runtime/org.freedesktop.Platform/x86_64/26.08/e7f7b0fb20662fe92e4eac65d58a57019cefae45a39559c4b50440a88acc0491/files'
    gl=G.OLD/'flatpak-user/runtime/org.freedesktop.Platform.GL.default/x86_64/26.08/25dbbb0cd3a63b2ef8a24ee948ad43998960f3e2bea713263742cc076840aec2/files'
    if a.prepare_plan:
        if a.execute or a.own_source is None or not re.fullmatch(r'[0-9a-f]{40}',a.own_revision or '') or a.tff_probe is None or a.bff_probe is None:
            p.error('prepare needs own source/full revision and two independently generated probe JSON inputs')
        if a.plan.exists():p.error('freeze requires a new plan path; existing evidence is retained')
        source=a.own_source.resolve(strict=True);fixtures={}
        for order,probe in (('tff',a.tff_probe),('bff',a.bff_probe)):
            media=INPUT_ROOT/INPUTS[order][0];assert G.digest(media)==INPUTS[order][1]
            meta=R.source_metadata(json.loads(probe.read_text()),media,media.stat().st_size,order)
            fixtures[order]={'path':str(media),'sha256':INPUTS[order][1],'bytes':media.stat().st_size,
                'independent_probe':str(probe.resolve()),'probe_sha256':G.digest(probe),'source_metadata':meta}
        selected=[Path(__file__),BASE_PATH,READER_PATH,PAYLOAD_READER,OPERATOR_ORIGIN,G.ROUTE_PATH,G.BOUNDARY,
            source/'filters/vaapi/payload-observer/crystalhd_drv_video.so',
            source/'tests/vaapi-vpp-payload-backend.cpp',source/'tests/vaapi-vpp-payload-observer.h',
            source/'tests/vaapi-vpp-payload.cpp',source/'tests/vaapi-vpp-payload-reader.test.py',
            source/'linux_lib/libcrystalhd/libcrystalhd.so.3.6',source/'filters/vaapi/crystalhd_drv_video.cpp',
            source/'filters/vaapi/crystalhd-trace.h',source/'tests/kernel-log-check.sh',G.WESTON/'bin/weston',
            G.WESTON/'lib/x86_64-linux-gnu/libweston-14/gl-renderer.so',G.WESTON/'lib/x86_64-linux-gnu/libweston-14/headless-backend.so',
            G.WESTON/'lib/x86_64-linux-gnu/weston/kiosk-shell.so',Path('/usr/bin/kodi-send'),Path('/usr/bin/ffprobe'),
            platform/'lib/x86_64-linux-gnu/libswscale.so.9.5.103',platform/'lib/x86_64-linux-gnu/libavutil.so.60.26.103']
        pins={str(path):G.digest(path) for path in selected}
        for path,expected in route['client']['pins'].items():assert G.digest(path)==expected;pins[path]=expected
        for f in fixtures.values():pins[f['path']]=f['sha256'];pins[f['independent_probe']]=f['probe_sha256']
        G.save(a.plan,{'schema':1,'status':'FROZEN_ROOT_APPROVED','source_directory':str(source),'revision':a.own_revision,
            'cases':[a.case],'platform_directory':str(platform),'gl_directory':str(gl),'fixtures':fixtures,'pins':pins,
            'order':['tff','bff'],'explicit_interlace_method':22,'expected_bindings':50,'expected_VPP_operations':100,
            'source_refs':[SOURCE_REF+'xbmc/cores/VideoSettings.h',SOURCE_REF+'xbmc/settings/MediaSettings.cpp',
                           SOURCE_REF+'xbmc/cores/VideoPlayer/DVDCodecs/Video/VAAPI.cpp'],
            'scope':SCOPE,'initial_pause':'Identity gate only, then normal-speed natural EOF; no cadence/performance claim',
            'payload_observer':{'schema':'crystalhd-vpp-payload-v1','quota':8,
                'source_parents_per_stream':2,'physical_parities_per_parent':2,
                'publication':'selected-decoder-context-retirement',
                'active_payload':[1280,720],'maximum_physical_height':768,'equal_origin_crops':True,
                'normal_driver_replacement':False,'performance_claim':False},
            'missing_observer':'Source/output PTS,GPU lifetime,chronological presentation,scanout NOT_RUN'})
        print('FROZEN_FILE_AND_INDEPENDENT_SOURCE_METADATA '+str(a.plan));return 0
    if not a.execute:print('INERT: --execute with a reviewed frozen plan is required');return 0
    assert hasattr(os,'pidfd_open') and hasattr(signal,'pidfd_send_signal')
    os.umask(0o077);resource.setrlimit(resource.RLIMIT_CORE,(0,0))
    plan=json.loads(a.plan.read_text());assert plan['status']=='FROZEN_ROOT_APPROVED' and a.case in plan['cases']
    assert plan['order']==['tff','bff'] and plan['explicit_interlace_method']==22 and plan['expected_bindings']==50 and plan['expected_VPP_operations']==100
    for path,expected in plan['pins'].items():assert G.digest(path)==expected,('frozen file changed',path)
    assert plan['payload_observer']=={'schema':'crystalhd-vpp-payload-v1','quota':8,
        'source_parents_per_stream':2,'physical_parities_per_parent':2,
        'publication':'selected-decoder-context-retirement',
        'active_payload':[1280,720],'maximum_physical_height':768,'equal_origin_crops':True,
        'normal_driver_replacement':False,'performance_claim':False}
    G.SOURCE=Path(plan['source_directory']);G.REVISION=plan['revision'];G.VA=G.SOURCE/'filters/vaapi/payload-observer/crystalhd_drv_video.so';G.DIL=G.SOURCE/'linux_lib/libcrystalhd/libcrystalhd.so.3.6'
    assert subprocess.check_output(['git','-C',str(G.SOURCE),'rev-parse','HEAD'],text=True).strip()==G.REVISION
    subprocess.run(['git','-C',str(G.SOURCE),'diff','--quiet','HEAD','--','filters/vaapi','linux_lib/libcrystalhd','include'],check=True)
    fixtures=plan['fixtures']
    for order in ('tff','bff'):
        f=fixtures[order];assert f['path']==str(INPUT_ROOT/INPUTS[order][0]) and f['sha256']==INPUTS[order][1]
        assert R.source_metadata(json.loads(Path(f['independent_probe']).read_text()),f['path'],f['bytes'],order)==f['source_metadata']
    assert str(platform)==plan['platform_directory'] and str(gl)==plan['gl_directory']
    case=ROOT/a.case;case.mkdir(mode=0o700)
    report={'schema':1,'case':a.case,'revision':G.REVISION,'scope':SCOPE,'plan_sha256':G.digest(a.plan),
        'pins':plan['pins'],'source_refs':plan['source_refs'],'fixtures':fixtures,'identity_checks':[],'retirements':[],
        'intervals':{},'source_census':{},'thermal_observations':[],'errors':[],'controls_status':'NOT_RUN',
        'application_exit':None,'live_resource_handoff':None,'completed_own_payload':'NOT_RUN',
        'payload_status_receipts':[],
        'source_outputPTS_gpu_lifetime_chronological_presentation_scanout':'NOT_RUN'}
    boundary=G.load('bob_boundary',G.BOUNDARY);before=None;kodi=None;kid=None;pidfd=None;weston=None;runtime=None;endpoint=None;udp=None;files=[]
    since=time.strftime('%Y-%m-%d %H:%M:%S UTC',time.gmtime());due=0
    def health():
        nonlocal due
        if time.monotonic()<due:return
        temps=[int(path.read_text()) for path in Path('/sys/class/thermal').glob('thermal_zone*/temp')]
        mains=[path for path in Path('/sys/class/power_supply').glob('*') if (path/'type').exists() and (path/'type').read_text().strip()=='Mains']
        ac=bool(mains) and any((path/'online').read_text().strip()=='1' for path in mains)
        report['thermal_observations'].append({'monotonic_ns':time.monotonic_ns(),'temperatures_mC':temps,'ac_present':ac})
        assert ac and temps and max(temps)<75000;due=time.monotonic()+5
    def wait(test,seconds,message):
        end=time.monotonic()+seconds
        while time.monotonic()<end:
            health();value=test()
            if value:return value
            time.sleep(.05)
        raise TimeoutError(message)
    def same():
        now=G.identity(kid['pid'],False);assert all(now[k]==kid[k] for k in now),'owned identity changed';return now
    def alive():
        launch=kodi is not None and kodi.poll() is None
        if pidfd is None:return launch
        poll=select.poll();poll.register(pidfd,select.POLLIN);return launch or not poll.poll(0)
    def wait_exit(seconds):
        end=time.monotonic()+seconds
        while alive() and time.monotonic()<end:time.sleep(.05)
        if alive():raise TimeoutError('owned media remains alive')
    def gate(name):G.save(case/'profile'/(name+'.ok'),{'verified':True})
    def retire(order):
        samples=[]
        def idle():
            same();sample={'monotonic_ns':time.monotonic_ns(),'boundary':boundary.snapshot(),'owned_decoder_fds':G.owned_fds(kid['pid'])};samples.append(sample)
            return sample['boundary']==before and sample['owned_decoder_fds']==[]
        wait(idle,10,'resource retirement '+order);time.sleep(.25);assert idle()
        report['retirements'].append({'source_case':order,'samples':samples})
    runtime_deps={'/usr/lib/x86_64-linux-gnu/libswscale.so.9.5.103':str(platform/'lib/x86_64-linux-gnu/libswscale.so.9.5.103'),
                  '/usr/lib/x86_64-linux-gnu/libavutil.so.60.26.103':str(platform/'lib/x86_64-linux-gnu/libavutil.so.60.26.103')}
    try:
        before=boundary.guard();report['before']=before
        assert before['debug_rows']==85 and before['debug_sha256']=='4e2758e4d68361f8783c48dbae8bd38ce06c380155a0f9de294d63d888c7a2f4'
        for text in before['aer']['0000:02:00.0'].values():assert all(int(line.split()[-1])==0 for line in text.splitlines() if line.strip())
        health();profile=case/'profile';profile.mkdir();run=case/'run';run.mkdir();(run/'deps').mkdir();(run/'controls.py').write_text(SCRIPT)
        payload=case/'payload';payload.mkdir(mode=0o700)
        with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as reserve:reserve.bind(('127.0.0.1',0));udp=reserve.getsockname()[1]
        settings={'services.webserver':'false','services.esenabled':'true','services.esport':str(udp),'services.esportrange':'1',
            'services.esallinterfaces':'false','addons.unknownsources':'true','addons.updatemode':'2','screensaver.mode':'','weather.addon':'',
            'lookandfeel.soundskin':'','videoplayer.usevaapi':'true','videoplayer.usevaapih264':'true','videoplayer.useprimedecoder':'false','videoplayer.useprimedecoderforhw':'false'}
        userdata=profile/'userdata';userdata.mkdir()
        xml='<settings version="2">'+''.join('<setting id="'+escape(k)+'">'+escape(v)+'</setting>' for k,v in settings.items())
        xml+='<defaultvideosettings><interlacemethod>22</interlacemethod><scalingmethod>0</scalingmethod><viewmode>0</viewmode><zoomamount>1</zoomamount><pixelratio>1</pixelratio><verticalshift>0</verticalshift></defaultvideosettings></settings>'
        (userdata/'guisettings.xml').write_text(xml);(userdata/'advancedsettings.xml').write_text('<advancedsettings><loglevel>0</loglevel></advancedsettings>')
        runtime=Path(tempfile.mkdtemp(prefix='ch-bob-',dir='/run/user/'+str(os.getuid())))
        lib=G.WESTON/'lib/x86_64-linux-gnu';env={k:v for k,v in os.environ.items() if not k.startswith(('LD_','GST_','CRYSTALHD_','LIBVA_'))}
        env.update(XDG_RUNTIME_DIR=str(runtime),WESTON_DATA_DIR=str(G.WESTON/'share/weston'),
            WESTON_MODULE_MAP=';'.join(n+'='+str(lib/'libweston-14'/n) for n in ('gl-renderer.so','headless-backend.so')),LD_LIBRARY_PATH=str(lib)+':'+str(lib/'weston'))
        log=(case/'weston.stdout').open('x');files.append(log);sock='stock-bob'
        weston=subprocess.Popen([str(G.WESTON/'bin/weston'),'--backend=headless','--renderer=gl','--shell='+str(lib/'weston/kiosk-shell.so'),
            '--width=1280','--height=720','--refresh-rate=60000','--socket='+sock,'--idle-time=0','--no-config','--log='+str(case/'weston.log')],env=env,stdin=subprocess.DEVNULL,stdout=log,stderr=subprocess.STDOUT)
        wait(lambda:(runtime/sock).exists() if weston.poll() is None else False,8,'owned Weston readiness')
        command=G.namespace(Path(route['client']['app_directory']),platform,gl,case,runtime,sock,Path(fixtures['tff']['path']))
        at=command.index('/fixture.mp4');command[at]='/fixture-tff.mp4'
        at=command.index('--');command[at:at]=['--ro-bind',fixtures['bff']['path'],'/fixture-bff.mp4']
        at=command.index('--');command[at:at]=['--bind',str(payload),'/payload',
            '--setenv','CRYSTALHD_TEST_VPP_PAYLOAD_DIR','/payload']
        assert '--die-with-parent' not in command
        G.save(case/'command.json',command);log=(case/'kodi.stdout').open('x');files.append(log)
        env={k:v for k,v in os.environ.items() if not k.startswith(('LD_','GST_','CRYSTALHD_','LIBVA_'))}
        kodi=subprocess.Popen(command,env=env,stdin=subprocess.DEVNULL,stdout=log,stderr=subprocess.STDOUT)
        def discover():
            assert kodi.poll() is None
            for candidate in G.descendants(kodi.pid):
                try:
                    if Path('/proc',str(candidate),'comm').read_text().strip()=='kodi.bin':
                        value=G.identity(candidate)
                        if value['uid']==os.getuid() and value['executable_sha256']==route['client']['pins'][str(Path(route['client']['app_directory'])/'lib/kodi/kodi.bin')]:return value
                except FileNotFoundError:pass
        kid=wait(discover,30,'original owned Kodi readiness');report['owned_app']=kid;report['owned_launcher_pid']=kodi.pid
        pidfd=os.pidfd_open(kid['pid'],0);same()
        def find_endpoint():
            same()
            try:return G.udp_owner(kid['pid'],udp)
            except AssertionError:return None
        endpoint=wait(find_endpoint,20,'owned EventServer readiness')
        subprocess.run(['/usr/bin/kodi-send','--host=127.0.0.1','--port='+str(udp),'--action=RunScript(/probe/controls.py)'],check=True,stdout=subprocess.DEVNULL,timeout=5)
        handled=set();end=time.monotonic()+100
        while time.monotonic()<end:
            if (profile/'bob-result.json').exists():
                report['controls']=json.loads((profile/'bob-result.json').read_text());report['controls_status']=report['controls']['status'];break
            health();same();progress=profile/'bob-progress.json'
            if not progress.exists():time.sleep(.05);continue
            value=json.loads(progress.read_text());phase=value['phase']
            if phase in handled:time.sleep(.05);continue
            if phase.startswith('await-open-'):
                order=phase.removeprefix('await-open-');assert order in ('tff','bff')
                assert boundary.guard()==before and not G.owned_fds(kid['pid'])
                report['intervals'][order]={'begin':(case/'kodi.stdout').stat().st_size,'begin_ns':time.monotonic_ns()}
                gate('open-'+order);handled.add(phase)
            elif phase.startswith('await-identity-'):
                order=phase.removeprefix('await-identity-');labels=value['info']
                assert labels['Player.Process(videodecoder)']=='ff-h264-vaapi' and labels['Player.Process(deintmethod)']=='vaapi-bob'
                assert value['source']=='/fixture-'+order+'.mp4'
                mapped=G.mappings(kid['pid'],plan['pins'],runtime_deps);fds=G.owned_fds(kid['pid'])
                assert any(row['target']=='/dev/crystalhd' for row in fds)
                report['identity_checks'].append({'source_case':order,'info':labels,'mapped_images':mapped,'owned_fds':fds,'same_app':same()})
                gate('identity-'+order);handled.add(phase)
            elif phase.startswith('await-retirement-'):
                order=phase.removeprefix('await-retirement-');assert value['active_players']==[];retire(order)
                report['intervals'][order].update(end=(case/'kodi.stdout').stat().st_size,end_ns=time.monotonic_ns())
                gate('retired-'+order);handled.add(phase)
            elif phase=='bob-failed':raise RuntimeError('finite Bob script failed; retained result has exact reason')
            time.sleep(.05)
        else:raise TimeoutError('finite Bob protocol deadline')
        assert report['controls_status']=='PASS';wait_exit(15)
    except BaseException as error:report['errors'].append({'step':'execution','error':G.fault(error)})
    finally:
        def normal_quit():
            assert endpoint is not None and kid is not None;same();assert G.udp_owner(kid['pid'],udp)==endpoint
            subprocess.run(['/usr/bin/kodi-send','--host=127.0.0.1','--port='+str(udp),'--action=Quit'],check=True,stdout=subprocess.DEVNULL,timeout=5)
        def term():
            assert kid is not None and pidfd is not None;same();signal.pidfd_send_signal(pidfd,signal.SIGTERM)
        shutdown=G.normal_media_shutdown(alive,normal_quit,term,wait_exit);report['errors'].extend(shutdown['errors']);live=alive()
        report['application_exit']=kodi.poll() if kodi else None
        if live:
            report['live_resource_handoff']={'status':'HANDOFF_REQUIRED_MEDIA_STILL_ALIVE','launcher_pid':kodi.pid,
                'last_verified_kodi_identity':kid,'compositor_pid':weston.pid if weston else None,'private_runtime':str(runtime),
                'required_action':'Keep runtime/profile/compositor. Verify PID/start/executable before ordinary Quit/TERM; no kill/reload/retry/cleanup.'}
        elif weston is not None:
            try:
                if weston.poll() is None:weston.terminate();weston.wait(timeout=10)
            except BaseException as error:report['errors'].append({'step':'Weston-close','error':G.fault(error)})
            if weston.poll() is None:report['live_resource_handoff']={'status':'HANDOFF_REQUIRED_COMPOSITOR_STILL_ALIVE','compositor_pid':weston.pid,'private_runtime':str(runtime)}
        if report['live_resource_handoff'] is not None:G.save(case/'live-resource-handoff.json',report['live_resource_handoff'])
        for stream in files:stream.close()
        if pidfd is not None:os.close(pidfd)
        if runtime is not None and not live and (weston is None or weston.poll() is not None):
            import shutil
            shutil.rmtree(runtime)
        if before is not None and not live:
            try:
                end=time.monotonic()+10;after=boundary.snapshot()
                while after!=before and time.monotonic()<end:time.sleep(.2);after=boundary.snapshot()
                report['after']=after;report['boundary_equal']=after==before
                if after==before:assert boundary.guard()==before
            except BaseException as error:report['errors'].append({'step':'terminal-boundary','error':G.fault(error)})
        try:
            with (case/'kernel.log').open('x') as out:report['kernel_exit']=subprocess.run(['sudo','-n','sh',str(G.SOURCE/'tests/kernel-log-check.sh'),since],stdout=out,stderr=subprocess.STDOUT,timeout=15).returncode
        except BaseException as error:report['errors'].append({'step':'kernel-check','error':G.fault(error)})
        try:
            if live:raise RuntimeError('operation log incomplete while owned media lives')
            raw=(case/'kodi.stdout').read_bytes();report['raw_stdout_sha256']=R.sha(raw);report['raw_stdout_bytes']=len(raw)
            for line in raw.splitlines():
                if line.startswith(b'{"schema":"crystalhd-vpp-payload-status-v1"'):
                    receipt=json.loads(line,object_pairs_hook=P.strict_object)
                    assert receipt['pid']==kid['pid'] and receipt['after_driver_unlock'] is True
                    report['payload_status_receipts'].append(receipt)
            assert len(report['payload_status_receipts'])<=16,'bounded diagnostic retirement receipt count exceeded'
            rows=R.parse(raw,kid['pid'])
            if all('end' in report['intervals'].get(order,{}) for order in ('tff','bff')):
                R.guard_intervals(rows,report['intervals'],len(raw))
            for order in ('tff','bff'):
                interval=report['intervals'].get(order,{})
                report['source_census'][order]=R.census(rows,interval['begin'],interval['end']) if 'end' in interval else {'status':'NOT_RUN','reason':'finite source did not retire'}
            manifests=[]
            for path in sorted((case/'payload').glob('vpp-payload-*.json')):
                document=json.loads(P.read_regular(path,256*1024),object_pairs_hook=P.strict_object)
                assert document['schema']=='crystalhd-vpp-payload-v1' and document['pid']==kid['pid']
                if document['slots']:manifests.append((path,document))
            summary=[{key:row[key] for key in ('driver_instance','context','generation',
                'observer_available','directory_open','directory_errno','candidates','rejected',
                'reject_mask','selected_slots','retired_slots','committed_slots','sealed','first_candidate')}
                for row in report['payload_status_receipts']]
            assert len(manifests)==1,('exactly one selected diagnostic VA session required; '
                'caller-side diagnostic receipts='+json.dumps(summary,separators=(',',':')))
            manifest,document=manifests[0];blobs={};frozen_files={str(manifest):G.digest(manifest)}
            for slot in document['slots']:
                for kind in ('source','target'):
                    name=P.filename(slot[kind+'_file']);path=manifest.parent/name
                    blobs[name]=P.read_regular(path,P.WIDTH*P.HEIGHT*3//2)
                    frozen_files[str(path)]=G.digest(path)
                order=('tff','bff')[slot['stream_index']];interval=report['intervals'][order]
                matches=[offset for offset,row in rows if row['driver_instance']==document['driver_instance']
                    and row['event']=='vpp_commit' and row['operation']==slot['operation']]
                assert len(matches)==1 and interval['begin']<=matches[0]<interval['end'],\
                    'payload stream/checkpoint not associated with exact frozen source interval'
            report['completed_own_payload']=P.check_document(document,blobs,P.parse_trace(raw))
            report['payload_manifest']=str(manifest);report['payload_files_sha256']=frozen_files
        except BaseException as error:
            report['operation_observer_status']='UNAVAILABLE_OR_INVALID';report['errors'].append({'step':'operation-census','error':G.fault(error)})
        strict=all(report['source_census'].get(order,{}).get('status')=='PASS' for order in ('tff','bff'))
        payload_pass=isinstance(report['completed_own_payload'],dict) and report['completed_own_payload'].get('status')=='PASS_SCOPED_OWN_COMPLETED_VPP_PAYLOAD'
        report['status']='PASS' if strict and payload_pass and report['controls_status']=='PASS' and report['application_exit']==0 and report.get('boundary_equal') and report.get('kernel_exit')==0 and not report['errors'] and report['live_resource_handoff'] is None else 'FAIL'
        report['excluded_heap']='No observer/investigation/reproduction; ordinary exit status only retained.'
        G.save(case/'result.json',report)
    print(a.case+': '+report['status']+'; bounded completed OWN NV12 field payload scope');return 0 if report['status']=='PASS' else 1


if __name__=='__main__':
    try:raise SystemExit(main())
    except Exception as error:print('stock-bob-payload: '+str(error),file=sys.stderr);raise SystemExit(1)
