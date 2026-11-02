#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prepare a fresh full-scale source and software YUYV oracle; no device access."""
from pathlib import Path
import argparse,hashlib,json,subprocess as sp
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument("--output", type=Path, required=True)
r=parser.parse_args().output.resolve()
r.mkdir(parents=True, exist_ok=False)
w,h,n=640,360,30
levels=[0,1,8,15,16,17,31,64,96,128,160,192,219,220,234,235,236,247,254,255]
chroma=[(128,128),(64,128),(192,128),(128,64),(128,192),(64,192),(192,64),(0,255),(255,0),(96,160)]
raw=r/'full-scale-input.yuv'
with raw.open('xb') as out:
 for frame in range(n):
  row=bytes(levels[(x//32+frame)%len(levels)] for x in range(w))
  u,v=chroma[frame%len(chroma)]
  out.write(row*h+bytes([u])*(w*h//4)+bytes([v])*(w*h//4))
cmd=['ffmpeg','-hide_banner','-nostdin','-v','info','-f','rawvideo','-pixel_format','yuv420p','-video_size',f'{w}x{h}','-framerate','30','-color_range','pc','-colorspace','bt709','-color_primaries','bt709','-color_trc','bt709','-i',str(raw),'-an','-c:v','libx264','-profile:v','high','-preset','veryfast','-qp','1','-bf','2','-g','30','-x264-params','slices=1:repeat-headers=1','-pix_fmt','yuv420p','-color_range','pc','-colorspace','bt709','-color_primaries','bt709','-color_trc','bt709',str(r/'full-scale.mp4')]
with (r/'encode.log').open('x') as log: sp.run(cmd,stdout=log,stderr=sp.STDOUT,check=True)
with (r/'decode.log').open('x') as log: sp.run(['ffmpeg','-hide_banner','-nostdin','-i',str(r/'full-scale.mp4'),'-an','-c:v','rawvideo','-pix_fmt','yuvj420p','-f','rawvideo',str(r/'decoded.yuv')],stdout=log,stderr=sp.STDOUT,check=True)
with (r/'trace.log').open('x') as log: sp.run(['ffmpeg','-hide_banner','-nostdin','-i',str(r/'full-scale.mp4'),'-c:v','copy','-bsf:v','trace_headers','-f','null','-'],stdout=log,stderr=sp.STDOUT,check=True)
data=(r/'decoded.yuv').read_bytes();assert len(data)==w*h*3//2*n
reference=bytearray(); ys=[]
for f in range(n):
 picture=data[f*w*h*3//2:(f+1)*w*h*3//2];y=picture[:w*h];u=picture[w*h:w*h*5//4];v=picture[w*h*5//4:]
 # Whole-frame constant chroma removes 420->422 interpolation ambiguity.
 assert len(set(u))==len(set(v))==1,(f,len(set(u)),len(set(v)))
 ys.extend(y)
 packed=bytearray(w*h*2);packed[0::2]=y;packed[1::4]=bytes([u[0]])*(w*h//2);packed[3::4]=bytes([v[0]])*(w*h//2);reference.extend(packed)
assert min(ys)==0 and max(ys)==255 and any(0<y<16 for y in ys) and any(235<y<255 for y in ys)
(r/'reference.yuyv').write_bytes(reference)
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
meta={'scope':'Actual full-scale source, not a VUI-only relabel. Single slice, progressive640x360/30pictures, constant chroma per frame.','width':w,'height':h,'frames':n,'levels':levels,'chroma':chroma,'encode_command':cmd,'source_luma_min':min(ys),'source_luma_max':max(ys),'source_below16':sum(y<16 for y in ys),'source_above235':sum(y>235 for y in ys),'reference_YUYV_sha256':sha(r/'reference.yuyv'),'media_sha256':sha(r/'full-scale.mp4'),'decoded_YUV_sha256':sha(r/'decoded.yuv'),'preparation_script_sha256':sha(Path(__file__)),'trace_sha256':sha(r/'trace.log')}
(r/'fixture.json').write_text(json.dumps(meta,indent=2)+'\n');print(json.dumps(meta,indent=2))
