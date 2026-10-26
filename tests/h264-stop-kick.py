#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Offline #533 G0: a private progressive I picture in the frozen 17+3 case.

Never opens a device. Original SPS/PPS/NAL bytes remain unchanged. The new
CAVLC PPS has its own unused ID; every private picture is non-reference.
Software acceptance is not evidence of firmware scheduling or safe STOP.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    'reference_bits', str(ROOT/'tests/h264-reference-lifecycle.py'))
BASE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BASE)


class Reader(BASE.Reader):
    def se(self):
        code = self.ue()
        return (code + 1)//2 if code & 1 else -code//2


def reader(nal):
    assert nal and not nal[0] & 0x80
    return Reader(nal[1:].replace(b'\0\0\3', b'\0\0'))


def dump_bytes(text):
    data = bytearray()
    for line in text.splitlines():
        if ':' in line:
            part = line.split(':', 1)[1].strip().split('  ', 1)[0]
            data.extend(bytes.fromhex(part))
    return bytes(data)


def scaling_list(r, count):
    last = next_scale = 8
    for _ in range(count):
        if next_scale:
            next_scale = (last + r.se() + 256) % 256
        if next_scale:
            last = next_scale


def hrd(r):
    count = r.ue() + 1
    assert count <= 32
    r.u(4); r.u(4)
    for _ in range(count):
        r.ue(); r.ue(); r.u(1)
    for _ in range(4):
        r.u(5)


def sps(nal):
    r = reader(nal)
    profile = r.u(8); r.u(8); level = r.u(8); identifier = r.ue()
    chroma, luma_depth, chroma_depth = 1, 8, 8
    if profile in (100,110,122,244,44,83,86,118,128,138,139,134,135):
        chroma = r.ue()
        assert chroma != 3, 'separate colour planes outside this fixture'
        luma_depth = r.ue()+8; chroma_depth = r.ue()+8; r.u(1)
        if r.u(1):
            for index in range(8):
                if r.u(1): scaling_list(r, 16 if index < 6 else 64)
    frame_bits = r.ue()+4; poc_type = r.ue()
    assert poc_type == 0, 'this candidate requires explicit POC0'
    poc_bits = r.ue()+4; refs = r.ue(); gaps = r.u(1)
    width_mbs = r.ue()+1; map_rows = r.ue()+1; frame_only = r.u(1)
    mbaff = r.u(1) if not frame_only else 0
    r.u(1)
    crop = [r.ue() for _ in range(4)] if r.u(1) else [0]*4
    reorder = buffering = None
    if r.u(1):
        if r.u(1):
            if r.u(8) == 255: r.u(16); r.u(16)
        if r.u(1): r.u(1)
        if r.u(1):
            r.u(3); r.u(1)
            if r.u(1): r.u(8); r.u(8); r.u(8)
        if r.u(1): r.ue(); r.ue()
        if r.u(1): r.u(32); r.u(32); r.u(1)
        nal_hrd = r.u(1)
        if nal_hrd: hrd(r)
        vcl_hrd = r.u(1)
        if vcl_hrd: hrd(r)
        if nal_hrd or vcl_hrd: r.u(1)
        r.u(1)
        if r.u(1):
            r.u(1)
            for _ in range(4): r.ue()
            reorder = r.ue(); buffering = r.ue()
    assert chroma == 1 and luma_depth == chroma_depth == 8
    assert frame_only and not mbaff and 4 <= frame_bits <= 16 and 4 <= poc_bits <= 16
    return dict(id=identifier,profile=profile,level=level,chroma=chroma,
                frame_bits=frame_bits,poc_bits=poc_bits,refs=refs,gaps=gaps,
                frame_only=frame_only,mbaff=mbaff,width_mbs=width_mbs,map_rows=map_rows,
                crop=crop,max_num_reorder_frames=reorder,max_dec_frame_buffering=buffering)


def pps(nal):
    r = reader(nal)
    identifier = r.ue(); sequence = r.ue(); entropy = r.u(1); bottom = r.u(1)
    assert r.ue() == 0, 'slice groups outside the selected fixture'
    refs = [r.ue()+1, r.ue()+1]
    weighted = r.u(1); bipred = r.u(2)
    r.se(); r.se(); r.se(); deblock = r.u(1); r.u(1); redundant = r.u(1)
    return dict(id=identifier,sps=sequence,entropy=entropy,bottom=bottom,
                refs=refs,weighted=weighted,bipred=bipred,deblock=deblock,redundant=redundant)


def slice_header(nal, sequences, pictures):
    r = reader(nal); first = r.ue(); kind = r.ue()%5; identifier = r.ue()
    p = pictures[identifier]; s = sequences[p['sps']]
    frame = r.u(s['frame_bits']); idr = nal[0]&31 == 5; reference = (nal[0]>>5)&3
    if idr: r.ue()
    poc = r.u(s['poc_bits'])
    if p['bottom']: assert r.se() == 0, 'nonzero bottom POC outside selected fixture'
    if p['redundant']: assert r.ue() == 0
    if kind == 1: r.u(1)
    refs = list(p['refs'])
    if kind in (0,1,3) and r.u(1):
        refs[0] = r.ue()+1
        if kind == 1: refs[1] = r.ue()+1
    if kind not in (2,4):
        for _ in range(2 if kind == 1 else 1):
            if r.u(1):
                for _step in range(64):
                    modification = r.ue()
                    if modification == 3: break
                    assert modification in (0,1,2)
                    r.ue()
                else: raise AssertionError('unbounded reference-list modification')
    if (p['weighted'] and kind in (0,3)) or (p['bipred']==1 and kind==1):
        r.ue(); r.ue()
        for direction in range(2 if kind==1 else 1):
            assert refs[direction] <= 32
            for _ in range(refs[direction]):
                if r.u(1): r.se(); r.se()
                if r.u(1):
                    for _c in range(2): r.se(); r.se()
    mmco = []
    if reference:
        if idr: r.u(1); r.u(1)
        elif r.u(1):
            for _ in range(64):
                operation = r.ue()
                if operation == 0: break
                assert operation <= 6
                mmco.append(operation)
                if operation in (1,3): r.ue()
                if operation == 2: r.ue()
                if operation in (3,6): r.ue()
                if operation == 4: r.ue()
            else: raise AssertionError('unbounded MMCO')
    assert 5 not in mmco, 'MMCO5 requires a different POC/frame_num derivation'
    return dict(first_mb=first,kind=kind,pps=identifier,frame_num=frame,
                poc_lsb=poc,reference=reference,idr=idr,mmco=mmco)


def private_picture(s, frame_num, poc_lsb):
    # PPS254 preserves SPS and PPS0. It enables CAVLC I16x16/DC independently
    # of the original PPS's CABAC, reference lists and transform settings.
    b = BASE.Bits(); b.ue(254); b.ue(s['id']); b.u(0,1); b.u(0,1)
    b.ue(0); b.ue(0); b.ue(0); b.u(0,1); b.u(0,2)
    b.se(0); b.se(0); b.se(0); b.u(1,1); b.u(0,1); b.u(0,1)
    parameter = BASE.annexb(0x68,b.finish())
    b = BASE.Bits(); b.ue(0); b.ue(2); b.ue(254)
    b.u(frame_num,s['frame_bits']); b.u(poc_lsb,s['poc_bits'])
    # nal_ref_idc=0: there is no reference marking/MMCO. All MBs use DC128
    # and zero residual, with no MBAFF/field flags in this progressive SPS.
    b.se(0); b.ue(1)
    for _ in range(s['width_mbs']*s['map_rows']):
        b.ue(3); b.ue(0); b.se(0); b.u(1,1)
    return BASE.annexb(9,b'\x10')+parameter+BASE.annexb(1,b.finish())


def decode(path, frame_bytes):
    command = ['ffmpeg','-nostdin','-hide_banner','-loglevel','error','-xerror',
               '-err_detect','explode','-threads','1','-i',str(path),'-map','0:v:0',
               '-fps_mode','passthrough','-pix_fmt','yuv420p','-f','rawvideo','-']
    run = subprocess.run(command,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=30)
    assert run.returncode == 0, run.stderr.decode(errors='replace')
    assert not run.stderr and len(run.stdout)%frame_bytes == 0
    return [run.stdout[i:i+frame_bytes] for i in range(0,len(run.stdout),frame_bytes)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('fixture',type=Path)
    parser.add_argument('output',type=Path)
    args = parser.parse_args()
    row = next(line.split('\t') for line in (ROOT/'tests/phase1-oracle.tsv').read_text().splitlines()
               if line.startswith('round\th264-high-ipb\t'))
    assert hashlib.sha256(args.fixture.read_bytes()).hexdigest() == row[4], 'not the frozen #114 fixture'
    result = subprocess.run(['ffprobe','-v','error','-select_streams','v:0',
        '-read_intervals','%+#20','-show_streams','-show_packets','-show_data',
        '-show_entries','stream=width,height,extradata:packet=pts,dts,duration,flags,data,size',
        '-of','json',str(args.fixture)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=15)
    assert result.returncode == 0 and not result.stderr
    probe = json.loads(result.stdout); stream = probe['streams'][0]
    extra = dump_bytes(stream['extradata']); assert extra[0] == 1
    length_size = (extra[4]&3)+1; offset = 6; sequences = []; parameters = []
    for _ in range(extra[5]&31):
        size = int.from_bytes(extra[offset:offset+2],'big'); offset += 2
        sequences.append(extra[offset:offset+size]); offset += size
    count = extra[offset]; offset += 1
    for _ in range(count):
        size = int.from_bytes(extra[offset:offset+2],'big'); offset += 2
        parameters.append(extra[offset:offset+size]); offset += size
    ss = {s['id']:s for s in map(sps,sequences)}
    pp = {p['id']:p for p in map(pps,parameters)}
    assert len(ss)==len(pp)==1 and 254 not in pp
    s = next(iter(ss.values())); width=stream['width']; height=stream['height']
    assert width==640 and height==360 and s['frame_only'] and s['crop']==[0,0,0,4]
    units = []; headers = []
    for index,packet in enumerate(probe['packets']):
        data = dump_bytes(packet['data']); assert len(data)==int(packet['size'])
        nals = []; cursor = 0
        while cursor < len(data):
            size = int.from_bytes(data[cursor:cursor+length_size],'big'); cursor += length_size
            assert 0 < size <= len(data)-cursor
            nals.append(data[cursor:cursor+size]); cursor += size
        slices = [slice_header(n,ss,pp) for n in nals if n[0]&31 in (1,5)]
        assert slices and slices[0]['first_mb']==0
        assert all({k:v for k,v in h.items() if k!='first_mb'} ==
                   {k:v for k,v in slices[0].items() if k!='first_mb'} for h in slices)
        h = dict(slices[0],index=index,pts=packet['pts'],dts=packet['dts'])
        headers.append(h); units.append(b''.join(b'\0\0\0\1'+n for n in nals))
    assert len(units)==20 and headers[0]['idr'] and not headers[17]['idr']
    previous_msb = previous_lsb = 0; maximum = 1 << s['poc_bits']
    for h in headers:
        current = h['poc_lsb']
        if h['idr']: previous_msb = previous_lsb = 0
        msb = previous_msb
        if current < previous_lsb and previous_lsb-current >= maximum//2: msb += maximum
        elif current > previous_lsb and current-previous_lsb > maximum//2: msb -= maximum
        h['poc'] = msb+current
        if h['reference']: previous_msb,previous_lsb = msb,current
    prefix = headers[:17]
    last_reference = next(h for h in reversed(prefix) if h['reference'])
    frame_num = (last_reference['frame_num']+1) % (1 << s['frame_bits'])
    # Place private pictures after the last decoded reference, not blindly
    # after the highest pending POC. This preserves room for the original
    # later B picture under the unchanged two-frame reorder restriction.
    first_poc = last_reference['poc']+1
    assert first_poc&1 and all(h['poc']%2==0 for h in headers)
    assert first_poc+4-last_reference['poc'] < maximum//2
    args.output.mkdir(mode=0o700,parents=False,exist_ok=False)
    initial = b''.join(b'\0\0\0\1'+n for n in sequences+parameters)
    original = initial+b''.join(units)
    original_path=args.output/'original20.h264';original_path.write_bytes(original)
    container=bytearray(b'CHDKICK1'+struct.pack('<I',20))
    for index,unit in enumerate(units):
        payload=(initial if index==0 else b'')+unit
        container.extend(struct.pack('<IQq',len(payload),(index+1)*100000,headers[index]['pts']))
        container.extend(payload)
    (args.output/'original20.packets').write_bytes(container)
    frame_bytes=width*height*3//2; baseline=decode(original_path,frame_bytes)
    assert len(baseline)==20 and len(set(baseline))==20
    neutral=bytes([128])*frame_bytes;assert neutral not in baseline
    report={'scope':'OFFLINE G0 only; not native scheduling/STOP/START proof',
            'fixture':row[3],'fixture_sha256':row[4],'sps':s,'pps':list(pp.values()),
            'original_aus':headers,'prefix_aus':17,'continuation_aus':3,
            'private_frame_num':frame_num,'private_first_poc':first_poc,
            'production_auxiliary':'ineligible: existing builder requires frame-coded MBAFF',
            'private_candidate':'test-only non-reference progressive I16x16/DC, unused PPS254',
            'software_original_frames':len(baseline),'cases':[]}
    original_pocs=[h['poc'] for h in headers]
    def reorder_bound(pocs):
        return max(sum(prior>current for prior in pocs[:index])
                   for index,current in enumerate(pocs))
    report['original_reorder_bound']=reorder_bound(original_pocs)
    report['prefix_and_continuation_order_equals_uninterrupted']=(
        sorted(original_pocs[:17])+sorted(original_pocs[17:])==sorted(original_pocs))
    report['order_boundary_note']='Original prefix already owns POC34; the following non-IDR AU has POC32. Per-drain order differs from uninterrupted20. Do not silently waive the issue global-order criterion.'
    for count in (1,2,3):
        private=b''.join(private_picture(s,frame_num,(first_poc+2*i)%maximum) for i in range(count))
        (args.output/('kick%d.h264'%count)).write_bytes(private)
        path=args.output/('with-kick%d.h264'%count)
        path.write_bytes(initial+b''.join(units[:17])+private+b''.join(units[17:]))
        decoded=decode(path,frame_bytes); actual_private=sum(f==neutral for f in decoded)
        real=[f for f in decoded if f!=neutral]
        item={'private_pictures':count,'private_bytes':len(private),
              'software_frames':len(decoded),'software_private_outputs':actual_private,
              'real_pixels_and_order_unchanged':real==baseline,
              'all_outputs':len(decoded)==20+count and actual_private==count}
        pocs=original_pocs[:17]+[first_poc+2*i for i in range(count)]+original_pocs[17:]
        item['required_reorder_bound']=reorder_bound(pocs)
        item['within_original_reorder_limit']=(s['max_num_reorder_frames'] is not None and
            item['required_reorder_bound']<=s['max_num_reorder_frames'])
        item['missing_original_indices']=[i for i,frame in enumerate(baseline) if frame not in real]
        item['hardware_eligible']=(item['all_outputs'] and item['real_pixels_and_order_unchanged'] and
                                   item['within_original_reorder_limit'])
        report['cases'].append(item)
        (args.output/'g0.json').write_text(json.dumps(report,indent=2)+'\n')
    aud=b'\0\0\0\1\x09\xf0\0\0\0\1\x0c'+bytes([255])*158+b'\x80'
    assert len(aud)==170
    (args.output/'prefix.h264').write_bytes(aud)
    assert report['original_reorder_bound']<=s['max_num_reorder_frames']
    assert report['cases'][0]['hardware_eligible']
    assert not report['cases'][2]['hardware_eligible']
    report['eligible_private_counts']=[item['private_pictures'] for item in report['cases'] if item['hardware_eligible']]
    report['result']='G0 candidates evaluated; only listed counts are native-eligible. Native G2/G3 NOT RUN. Global output-order boundary is independently unresolved.'
    (args.output/'g0.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({k:v for k,v in report.items() if k!='original_aus'},indent=2))


if __name__=='__main__':
    main()
