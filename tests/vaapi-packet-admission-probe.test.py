#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""OWN metadata forwarding controls; synthetic structs do not qualify FFmpeg ABI.

No external FFmpeg implementation, media, card or private client is exercised.
Root separately builds the observer with unchanged original public headers.
"""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
COMMON = r'''
#ifndef OWN_PACKET_COMMON
#define OWN_PACKET_COMMON
#include <stdint.h>
#define AV_VERSION_INT(a,b,c) (((a)<<16)|((b)<<8)|(c))
#define LIBAVFORMAT_VERSION_INT AV_VERSION_INT(63,1,102)
#define LIBAVCODEC_VERSION_INT AV_VERSION_INT(63,1,102)
#define LIBAVUTIL_VERSION_INT AV_VERSION_INT(61,1,102)
#define AV_CODEC_ID_H264 27
#define AVMEDIA_TYPE_VIDEO 0
#define AV_PIX_FMT_VAAPI 44
#define AVDISCARD_NONE (-16)
#define AVDISCARD_DEFAULT 0
#define AVDISCARD_NONREF 8
#define OWN_EOF (-541478725)
typedef struct AVRational {int num,den;} AVRational;
typedef struct AVDictionary {int dummy;} AVDictionary;
typedef struct AVInputFormat {int dummy;} AVInputFormat;
typedef struct AVCodec {int dummy;} AVCodec;
typedef struct AVCodecParameters {int codec_type,codec_id,width,height;} AVCodecParameters;
typedef struct AVStream {int id;AVCodecParameters *codecpar;AVRational time_base;int64_t start_time,duration;} AVStream;
typedef struct AVFormatContext {int64_t start_time;unsigned nb_streams;AVStream **streams;char *url;} AVFormatContext;
typedef struct AVCodecContext {
 int codec_type,codec_id,width,height,pix_fmt,skip_frame,skip_idct,skip_loop_filter;
 AVRational pkt_timebase;int thread_count,thread_type,active_thread_type,has_b_frames;
} AVCodecContext;
typedef struct AVPacket {
 int64_t pts,dts,duration,pos;int size,flags,stream_index;AVRational time_base;
 unsigned char *data;int side_data_elems;
} AVPacket;
typedef struct AVFrame {
 int64_t pts,best_effort_timestamp,duration;AVRational time_base;
 int format,width,height,flags;unsigned char *data[8];
} AVFrame;
unsigned avformat_version(void);unsigned avcodec_version(void);unsigned avutil_version(void);
int avformat_open_input(AVFormatContext **,const char *,const AVInputFormat *,AVDictionary **);
int avformat_find_stream_info(AVFormatContext *,AVDictionary **);
int av_read_frame(AVFormatContext *,AVPacket *);
void avformat_close_input(AVFormatContext **);
int avcodec_open2(AVCodecContext *,const AVCodec *,AVDictionary **);
int avcodec_send_packet(AVCodecContext *,const AVPacket *);
int avcodec_receive_frame(AVCodecContext *,AVFrame *);
void avcodec_flush_buffers(AVCodecContext *);
void avcodec_free_context(AVCodecContext **);
void own_format_mode(int);int own_format_failures(void);int own_format_entered(void);void own_packet(AVPacket *);
void own_codec_mode(int);int own_codec_failures(void);int own_codec_entered(void);
#endif
'''
FORMAT = r'''
#include "common.h"
#include <errno.h>
#include <string.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <unistd.h>
static int mode,failures;static atomic_int read_entered;static AVFormatContext *mapped;
int own_format_entered(void){return atomic_load(&read_entered);}
static AVCodecParameters parameters={AVMEDIA_TYPE_VIDEO,AV_CODEC_ID_H264,1920,1080};
static AVStream stream={.id=257,.codecpar=&parameters,.time_base={1,90000},.start_time=6006,.duration=2702700};
static AVStream second={.id=258,.codecpar=&parameters,.time_base={1,90000},.start_time=6006,.duration=2702700};
static AVStream *streams[]={&stream,&second};
static AVFormatContext formats[4];static unsigned opening;
void own_format_mode(int value){mode=value;atomic_store(&read_entered,0);}
int own_format_failures(void){return failures;}
unsigned avformat_version(void){return LIBAVFORMAT_VERSION_INT+BAD_VERSION;}
static void entry(void){if(errno!=77)++failures;}
void own_packet(AVPacket *p){memset(p,0,sizeof(*p));p->pts=6006;p->dts=0;p->duration=3003;p->pos=188;p->size=24;p->flags=1;p->time_base=(AVRational){1,90000};p->data=(void *)(uintptr_t)1;}
int avformat_open_input(AVFormatContext **f,const char *url,const AVInputFormat *input,AVDictionary **o){
 (void)input;(void)o;entry();if(mode==1){errno=88;return -22;}
 if(mode==9){errno=88;return 0;}
 if(mode>=13 && mode<=15){
  if(!mapped){mapped=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);if(mapped==MAP_FAILED)return -12;}
  else if(mprotect(mapped,4096,PROT_READ|PROT_WRITE))return -12;
  if(!*f)*f=mapped;
 }else if(!*f)*f=&formats[mode==12?(opening++%4):0];
 **f=(AVFormatContext){.start_time=6006,.nb_streams=mode==16?0:1,.streams=streams,.url=(char *)url};errno=88;return 0;
}
int avformat_find_stream_info(AVFormatContext *f,AVDictionary **o){
 (void)o;entry();
 /* Discovery deliberately nests all other public APIs: those calls belong to
  * this outer discovery call, not fabricated playback lifetimes. */
 if(mode==2){
  AVPacket p;AVFrame frame;AVCodecContext c={0},*cp=&c;
  errno=77;if(av_read_frame(f,&p))++failures;
  errno=77;if(avcodec_open2(&c,0,0))++failures;
  errno=77;if(avcodec_send_packet(&c,&p))++failures;
  errno=77;if(avcodec_receive_frame(&c,&frame)!=1)++failures;
  errno=77;avcodec_free_context(&cp);
 }
 errno=88;return 0;
}
int av_read_frame(AVFormatContext *f,AVPacket *p){
 entry();if(mode==13||mode==14){atomic_store(&read_entered,1);usleep(200000);}
 if(mode==3){errno=88;return -11;}if(mode==4){errno=88;return OWN_EOF;}
 if(mode!=9){own_packet(p);if(mode==5)p->stream_index=-1;if(mode==6)p->stream_index=1;if(mode==16){f->nb_streams=2;p->stream_index=1;}}
 errno=88;return 0;
}
void avformat_close_input(AVFormatContext **f){entry();if(f){if(*f && mode>=13 && mode<=15 && mprotect(*f,4096,PROT_NONE))++failures;*f=0;}errno=88;}
'''
CODEC = r'''
#include "common.h"
#include <errno.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>
static int mode,failures;static atomic_int entered;
void own_codec_mode(int value){mode=value;}
int own_codec_failures(void){return failures;}
int own_codec_entered(void){return atomic_load(&entered);}
unsigned avcodec_version(void){return LIBAVCODEC_VERSION_INT+BAD_VERSION;}
static void entry(void){if(errno!=77)++failures;}
int avcodec_open2(AVCodecContext *c,const AVCodec *codec,AVDictionary **o){(void)c;(void)codec;(void)o;entry();errno=88;return mode==1?-22:0;}
int avcodec_send_packet(AVCodecContext *c,const AVPacket *p){(void)c;entry();
 /* OUR observer must read before forwarding, including a rejected packet. */
 if(mode==9){errno=88;return 0;}
 if(mode==2){errno=88;return -11;}
 if(p && p->data && p->size==0){errno=88;return -22;}
 errno=88;return 0;
}
int avcodec_receive_frame(AVCodecContext *c,AVFrame *f){(void)c;entry();
 if(mode==10){atomic_store(&entered,1);usleep(200000);}
 if(mode==3){errno=88;return -11;}if(mode==4){errno=88;return OWN_EOF;}
 if(mode!=9){memset(f,0,sizeof(*f));f->pts=1000001;f->best_effort_timestamp=1000002;f->duration=33367;f->time_base=(AVRational){0,1};f->format=mode==5?0:AV_PIX_FMT_VAAPI;f->width=1920;f->height=1080;f->flags=3;f->data[3]=(void *)(uintptr_t)0xffffffff;}
 errno=88;return 1;
}
void avcodec_flush_buffers(AVCodecContext *c){entry();if(mode!=9)c->skip_frame=AVDISCARD_DEFAULT;errno=88;}
void avcodec_free_context(AVCodecContext **c){entry();if(c)*c=0;errno=88;}
'''
UTIL = r'''
#include "common.h"
unsigned avutil_version(void){return LIBAVUTIL_VERSION_INT+BAD_VERSION;}
'''
APP = r'''
#include "common.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <unistd.h>
static int failures;
static void checked(int r,int wanted){if(r!=wanted || errno!=88)++failures;}
static void codec_init(AVCodecContext *c){*c=(AVCodecContext){.codec_type=AVMEDIA_TYPE_VIDEO,.codec_id=AV_CODEC_ID_H264,.width=1920,.height=1080,.pix_fmt=AV_PIX_FMT_VAAPI,.skip_frame=AVDISCARD_NONREF,.skip_idct=AVDISCARD_NONE,.skip_loop_filter=AVDISCARD_DEFAULT,.pkt_timebase={0,1},.thread_count=2,.thread_type=3,.active_thread_type=1,.has_b_frames=2};}
static void *pending(void *arg){AVFrame frame;errno=77;checked(avcodec_receive_frame(arg,&frame),1);return 0;}
static void *pending_read(void *arg){AVPacket packet;errno=77;checked(av_read_frame(arg,&packet),0);return 0;}
int main(int argc,char **argv){
 const char *mode=argc>1?argv[1]:"normal";
 struct rlimit cores={0,0};if(setrlimit(RLIMIT_CORE,&cores))return 40;
 if(!avformat_version()||!avcodec_version()||!avutil_version())return 30;
 AVFormatContext *format=0;AVCodecContext storage,*codec=&storage;codec_init(codec);AVPacket packet;AVFrame frame;
 int invalid=!strcmp(mode,"foreign-invalid");if(invalid){own_format_mode(9);own_codec_mode(9);}
 if(!strcmp(mode,"format-error"))own_format_mode(1);
 if(!strcmp(mode,"codec-error"))own_codec_mode(1);
 if(!strcmp(mode,"nested"))own_format_mode(2);
 if(!strcmp(mode,"read-eagain"))own_format_mode(3);
 if(!strcmp(mode,"read-eof"))own_format_mode(4);
 if(!strcmp(mode,"read-close")||!strcmp(mode,"read-close-reopen"))own_format_mode(15);
 if(!strcmp(mode,"dynamic-streams"))own_format_mode(16);
 if(!strcmp(mode,"bad-stream-negative"))own_format_mode(5);
 if(!strcmp(mode,"bad-stream-range"))own_format_mode(6);
 if(!strcmp(mode,"send-eagain"))own_codec_mode(2);
 if(!strcmp(mode,"receive-eagain"))own_codec_mode(3);
 if(!strcmp(mode,"receive-eof"))own_codec_mode(4);
 if(!strcmp(mode,"software"))own_codec_mode(5);
 pid_t child=0;int cycles=!strcmp(mode,"reuse")?2:1;
 for(int cycle=0;cycle<cycles;cycle++){
  char long_url[1100];memset(long_url,'x',sizeof(long_url)-1);long_url[sizeof(long_url)-1]=0;
  const char *url=!strcmp(mode,"url-null")?0:!strcmp(mode,"url-long")?long_url:"/fixture.mp4";
  errno=77;checked(avformat_open_input(&format,url,0,0),!strcmp(mode,"format-error")?-22:0);
  errno=77;checked(avcodec_open2(codec,0,0),!strcmp(mode,"codec-error")?-22:0);
  if(!strcmp(mode,"repeat-open")){errno=77;checked(avformat_open_input(&format,url,0,0),0);errno=77;checked(avcodec_open2(codec,0,0),0);}
  if(!strcmp(mode,"format-error")||!strcmp(mode,"codec-error"))goto close;
  if(strcmp(mode,"no-find-info") && strcmp(mode,"dynamic-streams")){errno=77;checked(avformat_find_stream_info(format,0),0);}
  AVPacket *packet_arg=invalid?(AVPacket *)(uintptr_t)1:&packet;
  errno=77;checked(av_read_frame(format,packet_arg),!strcmp(mode,"read-eagain")?-11:!strcmp(mode,"read-eof")?OWN_EOF:0);
  own_packet(&packet);
  if(!strcmp(mode,"packet-unknown")){packet.pts=INT64_MIN;packet.dts=INT64_MIN;packet.time_base=(AVRational){0,1};}
  if(!strcmp(mode,"empty-drain")||!strcmp(mode,"side-data")){packet.size=0;packet.data=0;packet.side_data_elems=!strcmp(mode,"side-data")?1:0;}
  if(!strcmp(mode,"zero-size-data"))packet.size=0;
  int count=!strcmp(mode,"overflow")?45:1;
  for(int i=0;i<count;i++){
   AVPacket before=packet;AVCodecContext before_context=*codec;
   errno=77;checked(avcodec_send_packet(codec,invalid?(AVPacket *)(uintptr_t)1:!strcmp(mode,"null-drain")?0:&packet),!strcmp(mode,"send-eagain")?-11:!strcmp(mode,"zero-size-data")?-22:0);
   if(memcmp(&before,&packet,sizeof(packet))||memcmp(&before_context,codec,sizeof(*codec)))++failures;
   AVFrame before_frame;memset(&frame,0x5a,sizeof(frame));before_frame=frame;
   errno=77;int wanted=!strcmp(mode,"receive-eagain")?-11:!strcmp(mode,"receive-eof")?OWN_EOF:1;
   checked(avcodec_receive_frame(codec,invalid?(AVFrame *)(uintptr_t)1:&frame),wanted);
   if(wanted<0&&memcmp(&before_frame,&frame,sizeof(frame)))++failures;
  }
  if(!strcmp(mode,"flush")){errno=77;avcodec_flush_buffers(codec);if(errno!=88)++failures;errno=77;checked(avcodec_send_packet(codec,&packet),0);}
  if(!strcmp(mode,"concurrent")||!strcmp(mode,"inflight")){
   own_codec_mode(10);pthread_t thread;if(pthread_create(&thread,0,pending,codec))return 31;
   for(int i=0;!own_codec_entered()&&i<1000;i++)usleep(1000);
   if(!own_codec_entered())return 32;
   if(!strcmp(mode,"inflight")){printf("{\"pid\":%ld,\"child_pid\":0,\"failures\":%d}\n",(long)getpid(),failures+own_format_failures()+own_codec_failures());return 0;}
   errno=77;checked(avcodec_send_packet(codec,&packet),0);if(pthread_join(thread,0))return 33;
  }
  if(!strcmp(mode,"read-close")||!strcmp(mode,"read-close-reopen")){
   own_format_mode(!strcmp(mode,"read-close")?13:14);pthread_t thread;
   if(pthread_create(&thread,0,pending_read,format))return 41;
   for(int i=0;!own_format_entered()&&i<1000;i++)usleep(1000);
   if(!own_format_entered())return 42;
   errno=77;avformat_close_input(&format);if(format||errno!=88)++failures;
   if(!strcmp(mode,"read-close-reopen")){errno=77;checked(avformat_open_input(&format,"/replacement.mp4",0,0),0);}
   if(pthread_join(thread,0))return 43;
  }
  if(!strcmp(mode,"unknown")){AVCodecContext other;codec_init(&other);errno=77;checked(avcodec_send_packet(&other,&packet),0);}
  if(!strcmp(mode,"fork-copy")||!strcmp(mode,"fork-exec")){
   child=fork();if(child<0)return 34;
   if(!child){errno=77;checked(avcodec_send_packet(codec,&packet),0);
    if(!strcmp(mode,"fork-exec")){if(!freopen("/dev/null","w",stdout))_exit(35);execl("/proc/self/exe","own-packet-app","normal",(char *)0);_exit(36);}exit(0);}
   int status;if(waitpid(child,&status,0)!=child||!WIFEXITED(status)||WEXITSTATUS(status))return 37;
  }
close:
  if(strcmp(mode,"open")){errno=77;avcodec_free_context(&codec);if(codec||errno!=88)++failures;errno=77;avformat_close_input(&format);if(format||errno!=88)++failures;}
  if(!strcmp(mode,"reuse")){codec=&storage;codec_init(codec);}
 }
 if(!strcmp(mode,"unopened-free")){AVCodecContext other,*p=&other;codec_init(p);errno=77;avcodec_free_context(&p);if(p||errno!=88)++failures;errno=77;avformat_close_input(0);if(errno!=88)++failures;}
 if(!strcmp(mode,"existing")){char path[4096];snprintf(path,sizeof(path),"%s-%ld.jsonl",getenv("CRYSTALHD_TEST_PACKET_ADMISSION_PREFIX"),(long)getpid());FILE *out=fopen(path,"w");if(!out)return 38;fputs("original\n",out);fclose(out);}
 printf("{\"pid\":%ld,\"child_pid\":%ld,\"failures\":%d}\n",(long)getpid(),(long)child,failures+own_format_failures()+own_codec_failures());return failures+own_format_failures()+own_codec_failures()?39:0;
}
'''
FORMAT_SYMBOLS = ('avformat_open_input; avformat_find_stream_info; av_read_frame; avformat_close_input; '
                  'avformat_version; own_format_mode; own_format_failures; own_format_entered; own_packet;')
CODEC_SYMBOLS = ('avcodec_open2; avcodec_send_packet; avcodec_receive_frame; avcodec_flush_buffers; '
                 'avcodec_free_context; avcodec_version; own_codec_mode; own_codec_failures; own_codec_entered;')


class PacketControls(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix='own-packet-admission-controls-')
        cls.root = Path(cls.temporary.name)
        include = cls.root/'include'; include.mkdir()
        (include/'common.h').write_text(COMMON)
        for name in ('libavformat/avformat.h','libavformat/version.h','libavcodec/avcodec.h',
                     'libavcodec/version.h','libavutil/frame.h','libavutil/pixfmt.h','libavutil/version.h'):
            path=include/name;path.parent.mkdir(exist_ok=True);path.write_text('#include "common.h"\n')
        for name, source in (('format',FORMAT),('codec',CODEC),('util',UTIL),('app',APP)):
            (cls.root/(name+'.c')).write_text(source)
        for name, version, symbols in (('format','LIBAVFORMAT_63',FORMAT_SYMBOLS),
                ('codec','LIBAVCODEC_63',CODEC_SYMBOLS),('util','LIBAVUTIL_61','avutil_version;')):
            (cls.root/(name+'.map')).write_text(version+' { global: '+symbols+' local: *; };\n')
        for name, capacity in (('probe',128),('small',32)):
            cls.command(['cc','-std=c11','-Wall','-Wextra','-Werror','-O2','-fPIC','-shared',
                '-DPACKET_ADMISSION_CAPACITY='+str(capacity),'-I'+str(include),
                str(HERE/'vaapi-packet-admission-probe.c'),
                '-Wl,--version-script='+str(HERE/'vaapi-packet-admission-probe.map'),
                '-pthread','-ldl','-o',str(cls.root/(name+'.so'))])
        source=(HERE/'vaapi-packet-admission-probe.c').read_text()
        guarded='formats[e->format_slot].busy==1 && formats[e->format_slot].open && formats[e->format_slot].instance==e->format_instance'
        if source.count(guarded)!=1:
            raise AssertionError('OWN old-gate mutation must target one exact post-read lifetime guard')
        old_source=cls.root/'old-busy-only-probe.c'
        old_source.write_text(source.replace(guarded,'formats[e->format_slot].busy==1'))
        cls.command(['cc','-std=c11','-Wall','-Wextra','-Werror','-O2','-fPIC','-shared',
            '-DPACKET_ADMISSION_CAPACITY=128','-I'+str(include),str(old_source),
            '-Wl,--version-script='+str(HERE/'vaapi-packet-admission-probe.map'),
            '-pthread','-ldl','-o',str(cls.root/'unsafe-old.so')])
        for variant, badformat, badcodec, badutil in (('good',0,0,0),('badformat',1,0,0),
                ('badcodec',0,1,0),('badutil',0,0,1),('unversioned',0,0,0)):
            directory=cls.root/variant;directory.mkdir()
            for name, bad in (('codec',badcodec),('util',badutil),('format',badformat)):
                command=['cc','-std=c11','-Wall','-Wextra','-Werror','-D_DEFAULT_SOURCE',
                    '-fPIC','-shared','-DBAD_VERSION='+str(bad),'-I'+str(include),str(cls.root/(name+'.c'))]
                if not (variant=='unversioned' and name=='format'):
                    command+=['-Wl,--version-script='+str(cls.root/(name+'.map'))]
                if name=='format':command+=['-L'+str(directory),'-Wl,-rpath,'+str(directory),'-lcodec']
                command+=['-o',str(directory/('lib'+name+'.so'))]
                cls.command(command)
            cls.command(['cc','-std=c11','-Wall','-Wextra','-Werror','-D_DEFAULT_SOURCE',
                '-I'+str(include),str(cls.root/'app.c'),'-L'+str(directory),'-Wl,-rpath,'+str(directory),
                '-lformat','-lcodec','-lutil','-pthread','-o',str(directory/'app')])
        (cls.root/'linked').symlink_to(cls.root/'good',target_is_directory=True)
        cls.command(['cc','-std=c11','-Wall','-Wextra','-Werror','-D_DEFAULT_SOURCE',
            '-I'+str(include),str(cls.root/'app.c'),'-L'+str(cls.root/'linked'),
            '-Wl,-rpath,'+str(cls.root/'linked'),'-lformat','-lcodec','-lutil','-pthread',
            '-o',str(cls.root/'symlink-app')])

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    @staticmethod
    def command(command, env=None):
        result=subprocess.run(command,env=env,text=True,capture_output=True,timeout=30)
        if result.returncode:
            raise AssertionError(f'{command!r}\nexit {result.returncode}\n{result.stdout}\n{result.stderr}')
        return result

    def run_probe(self,mode='normal',variant='good',library='probe',marker='v1',extra=None,symlink=False):
        directory=Path(tempfile.mkdtemp(prefix='case-',dir=self.root));prefix=directory/'observed'
        env={k:v for k,v in os.environ.items() if not k.startswith('CRYSTALHD_TEST_PACKET_ADMISSION') and k!='LD_PRELOAD'}
        env.update(LD_PRELOAD=str(self.root/(library+'.so')),CRYSTALHD_TEST_PACKET_ADMISSION_PREFIX=str(prefix))
        for name in ('format','codec','util'):
            env['CRYSTALHD_TEST_PACKET_ADMISSION_'+name.upper()+'_PROVIDER']=str(self.root/variant/('lib'+name+'.so'))
        if marker is not None:env['CRYSTALHD_TEST_PACKET_ADMISSION']=marker
        if extra:env.update(extra)
        binary=self.root/'symlink-app' if symlink else self.root/variant/'app'
        application=json.loads(self.command([str(binary),mode],env).stdout)
        self.assertEqual(application['failures'],0)
        path=Path(str(prefix)+'-'+str(application['pid'])+'.jsonl')
        records=[json.loads(line) for line in path.read_text().splitlines()] if path.exists() and mode!='existing' else []
        return directory,path,records,application

    def complete(self,records):
        self.assertTrue(records[0]['provider_ok'])
        self.assertTrue(records[-1]['complete'],records[-1])
        self.assertEqual(records[0]['fork_policy'],'PARENT_PID_ONLY_CHILD_FORWARD_ONLY')
        self.assertEqual(records[0]['observation_scope'],'OUTERMOST_PUBLIC_CALLS_PARENT_PID')
        self.assertEqual(records[-1]['events'],len(records)-2)
        self.assertEqual([e['seq'] for e in records[1:-1]],list(range(len(records)-2)))
        for e in records[1:-1]:
            self.assertEqual((e['errno_before'],e['errno_after']),(77,88))
            self.assertEqual(e['pid'],records[0]['pid'])
            self.assertGreaterEqual(e['end_ns'],e['begin_ns'])

    def event(self,records,api):
        return next(e for e in records[1:-1] if e['api']==api)

    def test_public_forwarding_preserves_arguments_errno_ownership_and_raw_domains(self):
        _,_,r,_=self.run_probe();self.complete(r)
        self.assertEqual(len(r)-2,8)
        opened=self.event(r,'avformat_open_input')
        self.assertEqual((opened['input_url'],opened['format_url']),('/fixture.mp4','/fixture.mp4'))
        read=self.event(r,'av_read_frame');sent=self.event(r,'avcodec_send_packet');received=self.event(r,'avcodec_receive_frame')
        self.assertEqual(read['packet']['pts'],6006)
        self.assertEqual(read['packet']['time_base'],[1,90000])
        self.assertEqual(read['stream']['id'],257)
        self.assertEqual(sent['codec']['pkt_timebase'],[0,1])
        self.assertEqual(sent['codec']['skip_frame'],r[0]['discard_nonref'])
        self.assertEqual(received['ret'],1)
        self.assertEqual(received['frame']['pts'],1000001)
        self.assertEqual(received['frame']['time_base'],[0,1])
        self.assertEqual(received['frame']['va_surface'],0xffffffff)

    def test_find_info_nested_reads_and_decoder_are_not_playback_events(self):
        _,_,r,_=self.run_probe('nested');self.complete(r)
        self.assertEqual(len(r)-2,8)
        self.assertEqual(sum(e['api']=='avcodec_open2' for e in r[1:-1]),1)
        self.assertEqual(sum(e['api']=='av_read_frame' for e in r[1:-1]),1)
        self.assertFalse(self.event(r,'avformat_find_stream_info')['format']['info_complete'])
        self.assertTrue(self.event(r,'av_read_frame')['format']['info_complete'])

    def test_legitimate_playback_open_read_close_without_find_info_stays_observable(self):
        _,_,r,_=self.run_probe('no-find-info');self.complete(r)
        self.assertEqual(r[-1]['events'],7)
        self.assertFalse(any(e['api']=='avformat_find_stream_info' for e in r[1:-1]))
        read=self.event(r,'av_read_frame')
        self.assertEqual(read['format_instance'],1)
        self.assertFalse(read['format']['info_complete'])
        self.assertEqual(read['stream']['codec_id'],r[0]['h264_codec_id'])
        self.assertEqual(read['packet']['pts'],6006)

    def test_successful_read_refreshes_newly_discovered_stream_metadata(self):
        _,_,r,_=self.run_probe('dynamic-streams');self.complete(r)
        self.assertEqual(self.event(r,'avformat_open_input')['format']['nb_streams'],0)
        read=self.event(r,'av_read_frame')
        self.assertEqual(read['format']['nb_streams'],2)
        self.assertFalse(read['format']['info_complete'])
        self.assertEqual((read['packet']['stream_index'],read['stream']['index'],read['stream']['id']),(1,1,258))

    def test_read_return_after_close_or_address_reuse_never_reads_retired_context(self):
        for mode in ('read-close','read-close-reopen'):
            with self.subTest(mode=mode):
                _,_,r,_=self.run_probe(mode)
                self.assertFalse(r[-1]['complete'])
                self.assertGreater(r[-1]['lifecycle_failures'],0)
                read=[e for e in r[1:-1] if e['api']=='av_read_frame'][-1]
                self.assertEqual(read['ret'],0)
                self.assertTrue(read['concurrent'])
                self.assertIsNone(read['packet'])
                self.assertIsNone(read['stream'])
                if mode=='read-close-reopen':
                    opens=[e for e in r[1:-1] if e['api']=='avformat_open_input']
                    self.assertEqual([e['format_instance'] for e in opens],[1,2])
                    self.assertEqual(read['format_instance'],1)

    def test_old_busy_only_read_gate_faults_on_owned_protected_retired_context(self):
        # All code and the deliberately protected allocation are OWN doubles.
        # The old busy-only gate dereferences retired public metadata; the fixed
        # gate above instead preserves originalret/errno and rejects the ledger.
        with self.assertRaisesRegex(AssertionError,'exit -11'):
            self.run_probe('read-close',library='unsafe-old')

    def test_repeat_successful_opens_keep_the_original_lifetime_and_epoch(self):
        _,_,r,_=self.run_probe('repeat-open');self.complete(r)
        for api,key in (('avformat_open_input','format_instance'),('avcodec_open2','codec_instance')):
            events=[e for e in r[1:-1] if e['api']==api]
            self.assertEqual([e[key] for e in events],[1,1])
            self.assertEqual([e['opened_before'] for e in events],[False,True])
        self.assertEqual(self.event(r,'avcodec_free_context')['codec_epoch'],1)

    def test_address_reuse_after_retirement_allocates_new_lifetime(self):
        _,_,r,_=self.run_probe('reuse');self.complete(r)
        for api,key,ptr in (('avformat_open_input','format_instance','format_context'),('avcodec_open2','codec_instance','codec_context')):
            events=[e for e in r[1:-1] if e['api']==api]
            self.assertEqual([e[key] for e in events],[1,2])
            self.assertEqual(events[0][ptr],events[1][ptr])

    def test_flush_records_previous_skip_state_and_explicit_new_epoch(self):
        _,_,r,_=self.run_probe('flush');self.complete(r)
        event=self.event(r,'avcodec_flush_buffers')
        self.assertEqual((event['codec_epoch'],event['flush_old_epoch'],event['flush_new_epoch']),(1,1,2))
        self.assertEqual(event['codec']['skip_frame'],r[0]['discard_nonref'])
        sends=[e for e in r[1:-1] if e['api']=='avcodec_send_packet']
        self.assertEqual([e['codec_epoch'] for e in sends],[1,2])
        self.assertEqual(sends[-1]['codec']['skip_frame'],r[0]['discard_default'])

    def test_failed_opens_never_invent_successful_lifetimes(self):
        for mode,open_api,free_api,key in (('format-error','avformat_open_input','avformat_close_input','format_instance'),
                ('codec-error','avcodec_open2','avcodec_free_context','codec_instance')):
            with self.subTest(mode=mode):
                _,_,r,_=self.run_probe(mode);self.complete(r)
                self.assertEqual(self.event(r,open_api)[key],0)
                self.assertEqual(self.event(r,open_api)['ret'],-22)
                self.assertTrue(self.event(r,free_api)['unopened_free'])
                self.assertEqual(self.event(r,free_api)[key],0)

    def test_unopened_or_null_free_is_not_a_decoder_lifetime(self):
        _,_,r,_=self.run_probe('unopened-free');self.complete(r)
        frees=[e for e in r[1:-1] if e['unopened_free']]
        self.assertEqual(len(frees),2)
        self.assertTrue(all(e['format_instance']==e['codec_instance']==0 for e in frees))
        self.assertTrue(all(e['codec'] is None and e['format'] is None for e in frees))

    def test_send_eagain_is_attempt_metadata_not_a_hidden_acceptance(self):
        _,_,r,_=self.run_probe('send-eagain');self.complete(r)
        event=self.event(r,'avcodec_send_packet')
        self.assertEqual(event['ret'],-11)
        self.assertEqual(event['packet']['size'],24)
        self.assertEqual(event['codec']['skip_frame'],r[0]['discard_nonref'])

    def test_failed_read_and_receive_never_read_unreturned_packet_or_frame(self):
        for mode,api,result in (('read-eagain','av_read_frame',-11),('read-eof','av_read_frame',-541478725),
                ('receive-eagain','avcodec_receive_frame',-11),('receive-eof','avcodec_receive_frame',-541478725)):
            with self.subTest(mode=mode):
                _,_,r,_=self.run_probe(mode);self.complete(r);event=self.event(r,api)
                self.assertEqual(event['ret'],result)
                self.assertIsNone(event['packet'] if api=='av_read_frame' else event['frame'])

    def test_null_empty_side_data_and_invalid_zero_size_data_are_distinct(self):
        for mode,data_null,side,ret in (('empty-drain',True,0,0),('side-data',True,1,0),('zero-size-data',False,0,-22)):
            with self.subTest(mode=mode):
                _,_,r,_=self.run_probe(mode);self.complete(r);event=self.event(r,'avcodec_send_packet')
                self.assertFalse(event['packet_null']);self.assertEqual(event['ret'],ret)
                self.assertEqual((event['packet']['size'],event['packet']['data_null'],event['packet']['side_data_elems']),(0,data_null,side))
        _,_,r,_=self.run_probe('null-drain');self.complete(r);event=self.event(r,'avcodec_send_packet')
        self.assertTrue(event['packet_null']);self.assertIsNone(event['packet'])

    def test_software_frame_has_no_fabricated_va_surface(self):
        _,_,r,_=self.run_probe('software');self.complete(r)
        frame=self.event(r,'avcodec_receive_frame')['frame']
        self.assertEqual(frame['format'],0);self.assertIsNone(frame['va_surface'])

    def test_nopts_unknown_clock_is_not_a_fitted_source_mapping(self):
        _,_,r,_=self.run_probe('packet-unknown');self.complete(r)
        packet=self.event(r,'avcodec_send_packet')['packet']
        self.assertEqual((packet['pts'],packet['dts']),(-(1<<63),-(1<<63)))
        self.assertEqual(packet['time_base'],[0,1])

    def test_invalid_selected_stream_indices_are_incomplete_not_repaired(self):
        for mode,index in (('bad-stream-negative',-1),('bad-stream-range',1)):
            with self.subTest(mode=mode):
                _,_,r,_=self.run_probe(mode)
                self.assertFalse(r[-1]['complete']);self.assertGreater(r[-1]['lifecycle_failures'],0)
                event=self.event(r,'av_read_frame')
                self.assertEqual(event['packet']['stream_index'],index);self.assertIsNone(event['stream'])

    def test_unknown_data_context_invalidates_without_inventing_a_lifetime(self):
        _,_,r,_=self.run_probe('unknown')
        self.assertFalse(r[-1]['complete']);self.assertGreater(r[-1]['lifecycle_failures'],0)
        event=[e for e in r[1:-1] if e['api']=='avcodec_send_packet'][-1]
        self.assertEqual(event['codec_instance'],0);self.assertIsNone(event['codec'])

    def test_same_context_overlap_is_explicit_and_never_serializes_original_calls(self):
        _,_,r,_=self.run_probe('concurrent')
        self.assertFalse(r[-1]['complete']);self.assertGreater(r[-1]['lifecycle_failures'],0)
        self.assertTrue(any(e['concurrent'] for e in r[1:-1]))

    def test_inflight_shutdown_rejects_unpublished_original_call(self):
        _,_,r,_=self.run_probe('inflight')
        self.assertFalse(r[-1]['complete'])
        self.assertEqual((r[-1]['inflight'],r[-1]['unpublished']),(1,1))

    def test_event_overflow_and_open_lifetimes_remain_failures(self):
        _,_,r,_=self.run_probe('overflow',library='small')
        self.assertFalse(r[-1]['complete']);self.assertEqual(r[-1]['events'],32)
        self.assertEqual(r[-1]['attempted_events'],96);self.assertEqual(r[-1]['dropped'],64)
        _,_,r,_=self.run_probe('open')
        self.assertFalse(r[-1]['complete']);self.assertEqual((r[-1]['open_formats'],r[-1]['open_codecs']),(1,1))

    def test_urls_null_and_truncated_are_explicit(self):
        _,_,r,_=self.run_probe('url-null');self.complete(r)
        event=self.event(r,'avformat_open_input');self.assertIsNone(event['input_url']);self.assertIsNone(event['format_url'])
        _,_,r,_=self.run_probe('url-long')
        self.assertFalse(r[-1]['complete']);self.assertEqual(r[-1]['string_truncations'],2)
        self.assertEqual(len(self.event(r,'avformat_open_input')['input_url']),1023)

    def test_bad_provider_versions_unversioned_and_wrong_path_never_read_invalid_frames(self):
        for variant in ('badformat','badcodec','badutil','unversioned'):
            with self.subTest(variant=variant):
                _,_,r,_=self.run_probe('foreign-invalid',variant=variant)
                self.assertFalse(r[0]['provider_ok']);self.assertFalse(r[-1]['complete']);self.assertEqual(r[-1]['events'],0)
        _,_,r,_=self.run_probe('foreign-invalid',extra={'CRYSTALHD_TEST_PACKET_ADMISSION_UTIL_PROVIDER':str(self.root/'badutil/libutil.so')})
        self.assertFalse(r[0]['provider_ok']);self.assertEqual(r[-1]['events'],0)

    def test_provider_symlink_canonical_identity_is_accepted(self):
        _,_,r,_=self.run_probe(symlink=True);self.complete(r)
        self.assertEqual(r[0]['format_provider'],str(self.root/'good/libformat.so'))

    def test_marker_and_relative_prefix_only_forward(self):
        for marker in (None,'other'):
            _,path,records,_=self.run_probe(marker=marker);self.assertFalse(path.exists());self.assertEqual(records,[])
        directory,path,records,_=self.run_probe(extra={'CRYSTALHD_TEST_PACKET_ADMISSION_PREFIX':'relative'})
        self.assertFalse(path.exists());self.assertFalse(list(directory.iterdir()));self.assertEqual(records,[])

    def test_parent_fork_copy_does_not_publish_child_inherited_ledger(self):
        directory,_,r,application=self.run_probe('fork-copy');self.complete(r)
        self.assertTrue(r[-1]['fork_seen']);self.assertEqual(r[-1]['events'],8)
        self.assertFalse((directory/f"observed-{application['child_pid']}.jsonl").exists())

    def test_exec_child_has_only_its_fresh_pid_lifetimes(self):
        directory,_,parent,application=self.run_probe('fork-exec');self.complete(parent)
        child_pid=application['child_pid']
        child=[json.loads(line) for line in (directory/f'observed-{child_pid}.jsonl').read_text().splitlines()]
        self.complete(child);self.assertFalse(child[-1]['fork_seen']);self.assertTrue(parent[-1]['fork_seen'])
        self.assertEqual(child[-1]['events'],8);self.assertEqual(child[0]['pid'],child_pid)
        self.assertEqual(self.event(child,'avcodec_open2')['codec_instance'],1)
        self.assertEqual(self.event(child,'avformat_open_input')['format_instance'],1)
        self.assertTrue(all(e['pid']!=parent[0]['pid'] for e in child[1:-1]))

    def test_existing_report_is_not_overwritten(self):
        _,path,records,_=self.run_probe('existing')
        self.assertEqual(path.read_text(),'original\n');self.assertEqual(records,[])


if __name__=='__main__':
    unittest.main()
