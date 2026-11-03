#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""OWN synthetic-provider controls; not external FFmpeg ABI qualification.

Structs below are deliberately tiny owned mocks, not copied FFmpeg ABI layouts.
Actual compilation requires unchanged original63/61 headers separately. No
candidate FFmpeg library or real decoder is executed by these controls.
"""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
COMMON = r'''
#ifndef OWN_MOCK_COMMON
#define OWN_MOCK_COMMON
#include <stdint.h>
#define AV_VERSION_INT(a,b,c) (((a)<<16)|((b)<<8)|(c))
#define LIBAVCODEC_VERSION_INT AV_VERSION_INT(63,1,102)
#define LIBAVUTIL_VERSION_INT AV_VERSION_INT(61,1,102)
#define AV_PIX_FMT_VAAPI 44
typedef struct AVRational { int num, den; } AVRational;
typedef struct AVBufferRef { void *data; } AVBufferRef;
typedef struct AVCodecContext { AVRational pkt_timebase; } AVCodecContext;
typedef struct AVFrame {
    unsigned char *data[8]; int format; int64_t pts, best_effort_timestamp;
    AVRational time_base; AVBufferRef *hw_frames_ctx;
} AVFrame;
int avcodec_receive_frame(AVCodecContext *, AVFrame *);
unsigned avcodec_version(void);
unsigned avutil_version(void);
#endif
'''
CODEC = r'''
#include <errno.h>
#include "common.h"
static int result, output_errno;
void fake_configure(int r, int e) { result=r; output_errno=e; }
unsigned avcodec_version(void) { return LIBAVCODEC_VERSION_INT + BAD_VERSION; }
int avcodec_receive_frame(AVCodecContext *c, AVFrame *f) {
    (void)c; (void)f;
    int r = errno == 77 ? result : -999;
    errno=output_errno; return r;
}
'''
UTIL = r'''
#include "common.h"
unsigned avutil_version(void) { return LIBAVUTIL_VERSION_INT + BAD_VERSION; }
'''
UTIL60 = r'''
unsigned avutil_version(void) { return (60U<<16)|1; }
unsigned own_util60_present(void) { return avutil_version(); }
'''
APP = r'''
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "common.h"
void fake_configure(int,int);
unsigned own_util60_present(void);
static const char *mode;
static int desired;
static int call_one(int i) {
    AVCodecContext c = { .pkt_timebase={1,1000000} };
    AVBufferRef pool={.data=(void *)(uintptr_t)0x123456};
    AVFrame f; memset(&f,0,sizeof(f));
    f.format = !strcmp(mode,"software") ? 0 : AV_PIX_FMT_VAAPI;
    f.pts=i*33333; f.best_effort_timestamp=i*33333+1;
    f.time_base=(AVRational){1,1000000};
    f.hw_frames_ctx=&pool; f.data[3]=(unsigned char *)(uintptr_t)(i%8);
    if (!strcmp(mode,"unknown")) { f.pts=INT64_MIN; f.best_effort_timestamp=INT64_MIN; f.time_base=(AVRational){0,1}; c.pkt_timebase=(AVRational){0,0}; }
    if (!strcmp(mode,"nullpool")) f.hw_frames_ctx=NULL;
    AVFrame before=f;
    errno=77;
    AVFrame *arg=!strcmp(mode,"invalid") ? (AVFrame *)(uintptr_t)1 : &f;
    int r=avcodec_receive_frame(&c,arg); int e=errno;
    return r==desired && e==88 && memcmp(&before,&f,sizeof(f))==0 ? 0 : 1;
}
static void *worker(void *number) {
    int start=(int)(uintptr_t)number, failures=0;
    for(int i=0;i<6;i++) failures+=call_one(start+i);
    return (void *)(uintptr_t)failures;
}
int main(int argc,char **argv) {
    mode=argc>1?argv[1]:"normal";
    if (!avutil_version() || !own_util60_present()) return 20;
    desired=!strcmp(mode,"error")?-11:0; fake_configure(desired,88);
    int failures=0; pid_t child=0;
    if (!strcmp(mode,"spawn")) {
        child=fork(); if (child<0) return 23;
        if (!child) { execl("/bin/true","true",(char *)NULL); _exit(24); }
        int status; if(waitpid(child,&status,0)!=child || !WIFEXITED(status) || WEXITSTATUS(status)) return 25;
    }
    if (!strcmp(mode,"existing")) {
        char path[4096];
        int n=snprintf(path,sizeof(path),"%s-%ld.jsonl",getenv("CRYSTALHD_TEST_FRAME_PTS_PREFIX"),(long)getpid());
        if(n<0 || (size_t)n>=sizeof(path)) return 26;
        FILE *out=fopen(path,"w"); if(!out) return 27;
        fputs("original\n",out); fclose(out);
    }
    if (!strcmp(mode,"threads")) {
        pthread_t threads[4];
        for(int i=0;i<4;i++) if(pthread_create(&threads[i],NULL,worker,(void *)(uintptr_t)(i*6))) return 21;
        for(int i=0;i<4;i++){void *r; pthread_join(threads[i],&r); failures+=(int)(uintptr_t)r;}
    } else {
        int n=!strcmp(mode,"overflow")?40:1;
        for(int i=0;i<n;i++)failures+=call_one(i);
    }
    if (!strcmp(mode,"fork-copy")) {
        child=fork(); if(child<0)return 28;
        if(!child) exit(call_one(10));
        int status; if(waitpid(child,&status,0)!=child || !WIFEXITED(status) || WEXITSTATUS(status))return 29;
    }
    printf("{\"failures\":%d,\"pid\":%ld,\"child_pid\":%ld}\n",failures,(long)getpid(),(long)child);
    return failures?22:0;
}
'''


class ProbeControls(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix='own-frame-pts-controls-')
        cls.root = Path(cls.temporary.name)
        include = cls.root/'include'; include.mkdir()
        (include/'common.h').write_text(COMMON)
        for path in ('libavcodec/avcodec.h', 'libavcodec/version.h', 'libavutil/frame.h',
                     'libavutil/pixfmt.h', 'libavutil/version.h'):
            target=include/path; target.parent.mkdir(exist_ok=True); target.write_text('#include "common.h"\n')
        for name, source in (('codec.c',CODEC),('util.c',UTIL),('util60.c',UTIL60),('app.c',APP)):
            (cls.root/name).write_text(source)
        for name, version, symbols in (('codec','LIBAVCODEC_63','avcodec_receive_frame; avcodec_version; fake_configure;'),
                                       ('util','LIBAVUTIL_61','avutil_version;'),
                                       ('util60','LIBAVUTIL_60','avutil_version; own_util60_present;')):
            (cls.root/(name+'.map')).write_text(version+' { global: '+symbols+' local: *; };\n')
        cls.command(['cc','-std=c11','-Wall','-Wextra','-Werror','-O2','-fPIC','-shared',
                     '-DFRAME_PTS_CAPACITY=32','-I'+str(include), str(HERE/'vaapi-frame-pts-probe.c'),
                     '-Wl,--version-script='+str(HERE/'vaapi-frame-pts-probe.map'),'-pthread','-ldl','-o',str(cls.root/'probe.so')])
        for variant,badcodec,badutil in (('good',0,0),('badcodec',1,0),('badutil',0,1)):
            directory=cls.root/variant; directory.mkdir()
            for name,bad in (('codec',badcodec),('util',badutil),('util60',0)):
                cls.command(['cc','-std=c11','-Wall','-Wextra','-Werror','-fPIC','-shared',
                             '-DBAD_VERSION='+str(bad),'-I'+str(include),str(cls.root/(name+'.c')),
                             '-Wl,--version-script='+str(cls.root/(name+'.map')),
                             '-o',str(directory/('lib'+name+'.so'))])
            cls.command(['cc','-std=c11','-Wall','-Wextra','-Werror','-I'+str(include),str(cls.root/'app.c'),
                         '-L'+str(directory),'-Wl,-rpath,'+str(directory),'-lcodec','-lutil','-lutil60','-pthread','-o',str(directory/'app')])
        # Force dladdr to expose a loader symlink and require canonical identity.
        (cls.root/'linked').symlink_to(cls.root/'good', target_is_directory=True)
        cls.command(['cc','-std=c11','-Wall','-Wextra','-Werror','-I'+str(include),str(cls.root/'app.c'),
                     '-L'+str(cls.root/'linked'),'-Wl,-rpath,'+str(cls.root/'linked'),
                     '-lcodec','-lutil','-lutil60','-pthread','-o',str(cls.root/'symlink-app')])

    @classmethod
    def tearDownClass(cls): cls.temporary.cleanup()

    @staticmethod
    def command(args, **kwargs):
        return subprocess.run(args,check=True,text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,**kwargs)

    def run_probe(self, mode='normal', variant='good', marker='v1', override=None, output_name=None, app=None):
        directory=self.root/variant
        output=self.root/(output_name or ('out-'+self.id().rsplit('.',1)[-1]))
        env=os.environ.copy()
        for key in list(env):
            if key.startswith('CRYSTALHD_TEST_FRAME_PTS'): del env[key]
        env.update(LD_PRELOAD=str(self.root/'probe.so'),
                   CRYSTALHD_TEST_FRAME_PTS_PREFIX=str(output),
                   CRYSTALHD_TEST_FRAME_PTS_CODEC_PROVIDER=str(directory/'libcodec.so'),
                   CRYSTALHD_TEST_FRAME_PTS_UTIL_PROVIDER=str(directory/'libutil.so'))
        if marker is not None: env['CRYSTALHD_TEST_FRAME_PTS']=marker
        if override: env.update(override)
        result=self.command([str(app or directory/'app'),mode],env=env)
        identity=json.loads(result.stdout); self.assertEqual(identity['failures'],0)
        self.last_identity=identity
        output=Path(str(output)+'-'+str(identity['pid'])+'.jsonl')
        documents=[json.loads(line) for line in output.read_text().splitlines()] if output.exists() else []
        return documents,output

    def test_success_preserves_original_return_errno_frame_and_records_scalars(self):
        docs,path=self.run_probe()
        header,frame,footer=docs
        self.assertTrue(header['provider_ok']); self.assertEqual(header['codec_version'],4129126)
        self.assertEqual(header['util_version'],3998054); self.assertEqual(header['vaapi_format'],44)
        self.assertEqual(frame['format'],44); self.assertEqual(frame['surface'],0)
        self.assertEqual(frame['pts'],0); self.assertEqual(frame['best_effort_timestamp'],1)
        self.assertEqual(frame['frame_time_base'],[1,1000000]); self.assertEqual(frame['packet_time_base'],[1,1000000])
        self.assertEqual(frame['pool'],'0x123456'); self.assertIsNone(frame['display'])
        self.assertGreater(frame['snapshot_begin_ns'],0)
        self.assertGreaterEqual(frame['snapshot_end_ns'],frame['snapshot_begin_ns'])
        self.assertTrue(footer['complete']); self.assertEqual(footer['records'],1); self.assertEqual(footer['errors'],[])
        self.assertEqual(path.stat().st_mode & 0o777,0o600)

    def test_original_error_return_has_no_frame_observation(self):
        docs,_=self.run_probe('error'); self.assertEqual(len(docs),2); self.assertEqual(docs[-1]['records'],0)
        self.assertTrue(docs[-1]['complete'])

    def test_software_success_is_forwarded_without_va_record(self):
        docs,_=self.run_probe('software'); self.assertEqual(len(docs),2); self.assertEqual(docs[-1]['records'],0)

    def test_nopts_and_unknown_time_bases_are_not_inferred(self):
        docs,_=self.run_probe('unknown'); f=docs[1]
        self.assertEqual(f['pts'],-2**63); self.assertEqual(f['best_effort_timestamp'],-2**63)
        self.assertEqual(f['frame_time_base'],[0,1]); self.assertEqual(f['packet_time_base'],[0,0])

    def test_null_pool_identity_is_descriptive_zero(self):
        docs,_=self.run_probe('nullpool'); self.assertEqual(docs[1]['pool'],'0x0')

    def test_marker_absent_is_transparent_and_emits_nothing(self):
        docs,_=self.run_probe(marker=None); self.assertEqual(docs,[])

    def test_unknown_marker_is_transparent_and_emits_nothing(self):
        docs,_=self.run_probe(marker='v2'); self.assertEqual(docs,[])

    def test_bad_codec_version_never_dereferences_foreign_frame(self):
        docs,_=self.run_probe('invalid',variant='badcodec')
        self.assertFalse(docs[0]['provider_ok']); self.assertFalse(docs[-1]['complete'])
        self.assertEqual(docs[-1]['records'],0); self.assertIn('provider_guard_failed',docs[-1]['errors'])

    def test_bad_util_version_never_dereferences_foreign_frame(self):
        docs,_=self.run_probe('invalid',variant='badutil')
        self.assertFalse(docs[0]['provider_ok']); self.assertFalse(docs[-1]['complete']); self.assertEqual(docs[-1]['records'],0)

    def test_wrong_provider_path_never_dereferences_foreign_frame(self):
        docs,_=self.run_probe('invalid',override={'CRYSTALHD_TEST_FRAME_PTS_CODEC_PROVIDER':'/not/original.so'})
        self.assertFalse(docs[0]['provider_ok']); self.assertEqual(docs[-1]['records'],0)

    def test_symlink_loaded_provider_is_canonicalized(self):
        docs,_=self.run_probe(app=self.root/'symlink-app')
        self.assertTrue(docs[0]['provider_ok']); self.assertTrue(docs[-1]['complete'])
        self.assertEqual(docs[0]['codec_provider'],str(self.root/'good/libcodec.so'))

    def test_concurrent_calls_publish_distinct_complete_records(self):
        docs,_=self.run_probe('threads'); frames=docs[1:-1]
        self.assertEqual(len(frames),24); self.assertEqual([f['seq'] for f in frames],list(range(1,25)))
        self.assertEqual(len({f['tid'] for f in frames}),4); self.assertTrue(docs[-1]['complete'])
        self.assertTrue(all(f['snapshot_begin_ns']<=f['snapshot_end_ns'] for f in frames))

    def test_fixed_buffer_overflow_is_explicit_incomplete(self):
        docs,_=self.run_probe('overflow')
        self.assertEqual(len(docs[1:-1]),32); self.assertEqual(docs[-1]['dropped'],8)
        self.assertFalse(docs[-1]['complete']); self.assertIn('buffer_overflow',docs[-1]['errors'])

    def test_exec_child_cannot_consume_parent_report_path(self):
        docs,path=self.run_probe('spawn')
        self.assertEqual(docs[0]['pid'],self.last_identity['pid'])
        self.assertTrue(docs[-1]['complete']); self.assertEqual(docs[-1]['records'],1)
        child_path=Path(str(path).replace('-'+str(self.last_identity['pid'])+'.jsonl',
                                      '-'+str(self.last_identity['child_pid'])+'.jsonl'))
        child=[json.loads(line) for line in child_path.read_text().splitlines()]
        self.assertEqual(child[0]['pid'],self.last_identity['child_pid'])
        self.assertFalse(child[-1]['complete']); self.assertFalse(child[0]['provider_ok'])
        self.assertEqual(child[-1]['records'],0)

    def test_pre_exec_fork_cannot_relabel_inherited_parent_buffer(self):
        docs,path=self.run_probe('fork-copy')
        self.assertEqual(docs[0]['pid'],self.last_identity['pid'])
        self.assertTrue(docs[-1]['complete']); self.assertEqual(docs[-1]['records'],1)
        child_path=Path(str(path).replace('-'+str(self.last_identity['pid'])+'.jsonl',
                                      '-'+str(self.last_identity['child_pid'])+'.jsonl'))
        self.assertFalse(child_path.exists())

    def test_existing_pid_report_is_not_overwritten(self):
        prefix=self.root/'existing'
        env=os.environ.copy(); env.update(LD_PRELOAD=str(self.root/'probe.so'),CRYSTALHD_TEST_FRAME_PTS='v1',CRYSTALHD_TEST_FRAME_PTS_PREFIX=str(prefix))
        result=self.command([str(self.root/'good/app'),'existing'],env=env)
        identity=json.loads(result.stdout)
        self.assertEqual(identity['failures'],0)
        output=Path(str(prefix)+'-'+str(identity['pid'])+'.jsonl')
        self.assertEqual(output.read_text(),'original\n')

    def test_old_fixed_output_variable_is_ignored(self):
        docs,_=self.run_probe('invalid',override={
            'CRYSTALHD_TEST_FRAME_PTS_PREFIX':'',
            'CRYSTALHD_TEST_FRAME_PTS_OUTPUT':str(self.root/'old-output.jsonl')})
        self.assertEqual(docs,[])
        self.assertFalse((self.root/'old-output.jsonl').exists())

    def test_relative_output_disables_observation_and_foreign_reads(self):
        docs,_=self.run_probe('invalid',override={'CRYSTALHD_TEST_FRAME_PTS_PREFIX':'relative-output.jsonl'})
        self.assertEqual(docs,[])


if __name__ == '__main__': unittest.main()
