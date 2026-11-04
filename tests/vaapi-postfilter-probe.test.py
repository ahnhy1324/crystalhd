#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""OWN forwarding/ownership controls, not FFmpeg ABI or stock qualification.

Tiny synthetic structs/providers below are OUR test doubles. The real observer
must separately compile with unchanged original filter12/util61 headers. These
controls never execute an external decoder, filter, media client or device.
"""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
COMMON = r'''
#ifndef OWN_POSTFILTER_COMMON
#define OWN_POSTFILTER_COMMON
#include <stdint.h>
#include <stddef.h>
#define AV_VERSION_INT(a,b,c) (((a)<<16)|((b)<<8)|(c))
#define LIBAVFILTER_VERSION_INT AV_VERSION_INT(12,1,102)
#define LIBAVUTIL_VERSION_INT AV_VERSION_INT(61,1,102)
#define AV_PIX_FMT_NV12 23
#define AVERROR_EOF (-541478725)
typedef struct AVRational { int num, den; } AVRational;
typedef struct AVFilterGraph { int live; } AVFilterGraph;
typedef struct AVFilterContext { AVFilterGraph *graph; } AVFilterContext;
typedef struct AVFilter { const char *name; } AVFilter;
typedef struct AVFilterInOut { int marker; } AVFilterInOut;
typedef struct AVFrame {
 unsigned char *data[8]; int linesize[8]; int format, width, height;
 int64_t pts, best_effort_timestamp, duration; AVRational time_base;
 int flags; AVRational sample_aspect_ratio;
 int color_range,color_primaries,color_trc,colorspace,chroma_location;
 size_t crop_top,crop_bottom,crop_left,crop_right;
} AVFrame;
unsigned avfilter_version(void); unsigned avutil_version(void);
int avfilter_graph_create_filter(AVFilterContext **,const AVFilter *,const char *,const char *,void *,AVFilterGraph *);
int avfilter_graph_parse_ptr(AVFilterGraph *,const char *,AVFilterInOut **,AVFilterInOut **,void *);
int av_buffersrc_add_frame(AVFilterContext *,AVFrame *);
int av_buffersrc_add_frame_flags(AVFilterContext *,AVFrame *,int);
int av_buffersrc_close(AVFilterContext *,int64_t,unsigned);
int av_buffersink_get_frame(AVFilterContext *,AVFrame *);
int av_buffersink_get_frame_flags(AVFilterContext *,AVFrame *,int);
AVRational av_buffersink_get_time_base(const AVFilterContext *);
AVRational av_buffersink_get_frame_rate(const AVFilterContext *);
void avfilter_graph_free(AVFilterGraph **);
void own_mode(int); int own_failures(void); int own_entered(void); void own_fill(AVFrame *,int);
#endif
'''
FILTER = r'''
#include "common.h"
#include <errno.h>
#include <string.h>
#include <stdatomic.h>
#include <unistd.h>
static int mode, failures;
static atomic_int entered;
int own_entered(void) { return atomic_load(&entered); }
static AVFilterContext source;
static unsigned char y[24], uv[12];
void own_mode(int n) { mode=n; }
int own_failures(void) { return failures; }
unsigned avfilter_version(void) { return LIBAVFILTER_VERSION_INT + BAD_VERSION; }
static void entry(void) { if(errno!=77) ++failures; }
void own_fill(AVFrame *f,int value) {
 memset(f,0,sizeof(*f)); memset(y,value,sizeof(y)); memset(uv,value+80,sizeof(uv));
 f->data[0]=y;f->data[1]=uv;f->linesize[0]=6;f->linesize[1]=6;
 f->format=AV_PIX_FMT_NV12;f->width=4;f->height=4;
 f->pts=123456;f->best_effort_timestamp=123457;f->duration=33367;
 f->time_base=(AVRational){0,1};f->flags=3;f->sample_aspect_ratio=(AVRational){1,1};
 f->color_range=1;f->color_primaries=2;f->color_trc=3;f->colorspace=4;f->chroma_location=5;
}
int avfilter_graph_create_filter(AVFilterContext **c,const AVFilter *f,const char *n,const char *a,void *o,AVFilterGraph *g) {
 (void)f;(void)n;(void)a;(void)o;entry();source.graph=g;g->live=1;*c=&source;errno=88;return 0;
}
int avfilter_graph_parse_ptr(AVFilterGraph *g,const char *f,AVFilterInOut **i,AVFilterInOut **o,void *l) {
 (void)g;(void)f;(void)i;(void)o;(void)l;entry();errno=88;return mode==8?-22:0;
}
int av_buffersrc_add_frame_flags(AVFilterContext *c,AVFrame *f,int flags) {
 (void)c;entry();
 if(mode==2) {errno=88;return -11;}
 if(mode==9) {errno=88;return 0;} /* foreign invalid pointers are never read */
 if(f && !(flags&8)) { memset(f,0,sizeof(*f));memset(y,0,sizeof(y));memset(uv,0,sizeof(uv)); }
 errno=88;return 0;
}
int av_buffersrc_add_frame(AVFilterContext *c,AVFrame *f) {
 /* Deliberately call the other public wrapper: must still forward each once. */
 return av_buffersrc_add_frame_flags(c,f,0);
}
int av_buffersrc_close(AVFilterContext *c,int64_t pts,unsigned flags) {
 (void)c;entry();if(pts!=999 || flags!=7) ++failures;errno=88;return 0;
}
int av_buffersink_get_frame_flags(AVFilterContext *c,AVFrame *f,int flags) {
 (void)c;entry();
 if(mode==10) { atomic_store(&entered,1);sleep(2); }
 if(mode==3) {errno=88;return -11;}
 if(mode==4) {errno=88;return AVERROR_EOF;}
 if(mode==9) {errno=88;return 1;}
 if(flags!=0 && flags!=5) ++failures;
 own_fill(f,30);errno=88;return 1; /* any nonnegative success must be captured */
}
int av_buffersink_get_frame(AVFilterContext *c,AVFrame *f) { return av_buffersink_get_frame_flags(c,f,0); }
AVRational av_buffersink_get_time_base(const AVFilterContext *c) { (void)c;errno=99;return (AVRational){1,2}; }
AVRational av_buffersink_get_frame_rate(const AVFilterContext *c) { (void)c;errno=99;return (AVRational){60000,1001}; }
void avfilter_graph_free(AVFilterGraph **g) { entry();if(g && *g)(*g)->live=0;if(g)*g=0;errno=88; }
'''
UTIL = r'''
#include "common.h"
unsigned avutil_version(void) { return LIBAVUTIL_VERSION_INT + BAD_VERSION; }
'''
APP = r'''
#include "common.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <pthread.h>
static int failures;
static void checked(int result,int expected) { if(result!=expected || errno!=88) ++failures; }
static void add(AVFilterContext *c,const char *mode,int flags) {
 AVFrame f;own_fill(&f,10); AVFrame before=f;
 if(!strcmp(mode,"invalid-geometry")) f.width=1922;
 if(!strcmp(mode,"odd")) f.height=3;
 if(!strcmp(mode,"negative-stride")) f.linesize[0]=-6;
 if(!strcmp(mode,"software")) f.format=0;
 if(!strcmp(mode,"unknown")) {f.pts=INT64_MIN;f.best_effort_timestamp=INT64_MIN;f.duration=0;}
 if(!strcmp(mode,"crop")) {f.crop_top=2;f.crop_bottom=2;f.crop_left=4;f.crop_right=6;}
 int desired=!strcmp(mode,"source-error")?-11:0;
 errno=77;
 AVFrame *arg=!strcmp(mode,"foreign-invalid")?(AVFrame *)(uintptr_t)1:&f;
 int r=flags?av_buffersrc_add_frame_flags(c,arg,flags):av_buffersrc_add_frame(c,arg);
 checked(r,desired);
 if(!strcmp(mode,"source-error") && memcmp(&f,&before,sizeof(f))) ++failures;
 if(flags==8 && memcmp(&f,&before,sizeof(f))) ++failures;
 if(!strcmp(mode,"normal") && f.data[0]) ++failures;
}
static void *pending_pull(void *context) {
 AVFrame f;own_fill(&f,1);errno=77;
 (void)av_buffersink_get_frame(context,&f);return 0;
}
int main(int argc,char **argv) {
 const char *mode=argc>1?argv[1]:"normal";
 if(!avfilter_version() || !avutil_version()) return 30;
 if(!strcmp(mode,"source-error"))own_mode(2);
 if(!strcmp(mode,"foreign-invalid"))own_mode(9);
 AVFilter filter={"buffer"};AVFilterGraph storage={0};AVFilterContext *source=0;pid_t child=0;
 int cycles=!strcmp(mode,"reuse")?2:1;
 for(int cycle=0;cycle<cycles;cycle++) {
  AVFilterGraph *g=&storage;errno=77;
  checked(avfilter_graph_create_filter(&source,&filter,"in","video_size=4x4:pix_fmt=23:time_base=1/1:pixel_aspect=1/1",0,g),0);
  if(!strcmp(mode,"truncated")) {
   char text[1100];memset(text,'a',sizeof(text)-1);text[sizeof(text)-1]=0;
   errno=77;checked(avfilter_graph_parse_ptr(g,text,0,0,0),0);
  } else {errno=77;checked(avfilter_graph_parse_ptr(g,"bwdif=1:-1",0,0,0),0);}
  if(!strcmp(mode,"existing")) {
   char path[4096];snprintf(path,sizeof(path),"%s-%ld.jsonl",getenv("CRYSTALHD_TEST_POSTFILTER_PREFIX"),(long)getpid());
   FILE *out=fopen(path,"w");if(!out)return 31;fputs("original\n",out);fclose(out);
  }
  if(!strcmp(mode,"snapshot-existing")) {
   char path[4096];snprintf(path,sizeof(path),"%s-%ld-s00.nv12",getenv("CRYSTALHD_TEST_POSTFILTER_PREFIX"),(long)getpid());
   FILE *out=fopen(path,"w");if(!out)return 35;fputs("original snapshot\n",out);fclose(out);
  }
  if(!strcmp(mode,"inflight")) {
   own_mode(10);pthread_t thread;
   if(pthread_create(&thread,0,pending_pull,source))return 36;
   for(int wait=0;!own_entered() && wait<1000;wait++)usleep(1000);
   if(!own_entered())return 37;
   printf("{\"failures\":%d,\"pid\":%ld,\"child_pid\":0}\n",failures+own_failures(),(long)getpid());
   return 0; /* normal exit while OUR controlled original pull is pending */
  }
  int n=!strcmp(mode,"overflow")?45:!strcmp(mode,"snapshot-overflow")?5:!strcmp(mode,"selection")?5:1;
  for(int i=0;i<n;i++) {
   add(source,mode,!strcmp(mode,"flags")?8:0);
   AVFrame f;own_fill(&f,1); AVFrame before=f;
   if(!strcmp(mode,"sink-eagain"))own_mode(3);
   if(!strcmp(mode,"sink-eof"))own_mode(4);
   errno=77;
   int wanted=!strcmp(mode,"sink-eagain")?-11:!strcmp(mode,"sink-eof")?AVERROR_EOF:1;
   AVFrame *arg=!strcmp(mode,"foreign-invalid")?(AVFrame *)(uintptr_t)1:&f;
   checked(!strcmp(mode,"flags")?av_buffersink_get_frame_flags(source,arg,5):av_buffersink_get_frame(source,arg),wanted);
   if(wanted<0 && memcmp(&f,&before,sizeof(f))) ++failures;
  }
  if(!strcmp(mode,"null-close")) {
   errno=77;checked(av_buffersrc_add_frame(source,0),0);
   errno=77;checked(av_buffersrc_close(source,999,7),0);
  }
  if(!strcmp(mode,"fork-copy")) {
   child=fork();if(child<0)return 32;
   if(!child) {add(source,"normal",0);exit(0);}
   int status;if(waitpid(child,&status,0)!=child || !WIFEXITED(status) || WEXITSTATUS(status))return 33;
  }
  if(strcmp(mode,"open")) {errno=77;avfilter_graph_free(&g);if(g || errno!=88) ++failures;}
 }
 printf("{\"failures\":%d,\"pid\":%ld,\"child_pid\":%ld}\n",failures+own_failures(),(long)getpid(),(long)child);
 return failures+own_failures()?34:0;
}
'''
SYMBOLS = ('avfilter_graph_create_filter; avfilter_graph_parse_ptr; '
           'av_buffersrc_add_frame; av_buffersrc_add_frame_flags; av_buffersrc_close; '
           'av_buffersink_get_frame; av_buffersink_get_frame_flags; avfilter_graph_free; '
           'av_buffersink_get_time_base; av_buffersink_get_frame_rate; avfilter_version; '
           'own_mode; own_failures; own_entered; own_fill;')


class PostfilterControls(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix='own-postfilter-controls-')
        cls.root = Path(cls.temporary.name)
        include = cls.root/'include'; include.mkdir()
        (include/'common.h').write_text(COMMON)
        for name in ('libavfilter/avfilter.h', 'libavfilter/version.h',
                     'libavfilter/buffersrc.h', 'libavfilter/buffersink.h',
                     'libavutil/frame.h', 'libavutil/pixfmt.h', 'libavutil/version.h'):
            path = include/name; path.parent.mkdir(exist_ok=True)
            path.write_text('#include "common.h"\n')
        for name, source in (('filter', FILTER), ('util', UTIL), ('app', APP)):
            (cls.root/(name+'.c')).write_text(source)
        (cls.root/'filter.map').write_text('LIBAVFILTER_12 { global: '+SYMBOLS+' local: *; };\n')
        (cls.root/'util.map').write_text('LIBAVUTIL_61 { global: avutil_version; local: *; };\n')
        for name, count, snaps in (('probe', 64, 16), ('small', 32, 16), ('few-images', 64, 2)):
            cls.command(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2', '-fPIC', '-shared',
                         '-DPOSTFILTER_CAPACITY='+str(count), '-DPOSTFILTER_SNAPSHOTS='+str(snaps),
                         '-I'+str(include), str(HERE/'vaapi-postfilter-probe.c'),
                         '-Wl,--version-script='+str(HERE/'vaapi-postfilter-probe.map'),
                         '-pthread', '-ldl', '-o', str(cls.root/(name+'.so'))])
        for variant, badfilter, badutil in (('good', 0, 0), ('badfilter', 1, 0), ('badutil', 0, 1)):
            directory = cls.root/variant; directory.mkdir()
            for name, bad in (('filter', badfilter), ('util', badutil)):
                cls.command(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fPIC', '-shared',
                             '-DBAD_VERSION='+str(bad), '-I'+str(include), str(cls.root/(name+'.c')),
                             '-Wl,--version-script='+str(cls.root/(name+'.map')),
                             '-o', str(directory/('lib'+name+'.so'))])
            cls.command(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I'+str(include),
                         '-D_DEFAULT_SOURCE', str(cls.root/'app.c'), '-L'+str(directory), '-Wl,-rpath,'+str(directory),
                         '-lfilter', '-lutil', '-pthread', '-o', str(directory/'app')])
        # Canonical provider identity must accept a loader's symlink spelling.
        (cls.root/'linked').symlink_to(cls.root/'good', target_is_directory=True)
        cls.command(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I'+str(include),
                     '-D_DEFAULT_SOURCE', str(cls.root/'app.c'), '-L'+str(cls.root/'linked'),
                     '-Wl,-rpath,'+str(cls.root/'linked'), '-lfilter', '-lutil', '-pthread',
                     '-o', str(cls.root/'symlink-app')])
        # Unversioned providers forward but never qualify as original LIBAVFILTER_12.
        directory = cls.root/'unversioned'; directory.mkdir()
        cls.command(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fPIC', '-shared',
                     '-DBAD_VERSION=0', '-I'+str(include), str(cls.root/'filter.c'),
                     '-o', str(directory/'libfilter.so')])
        (directory/'libutil.so').symlink_to(cls.root/'good/libutil.so')
        cls.command(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I'+str(include),
                     '-D_DEFAULT_SOURCE', str(cls.root/'app.c'), '-L'+str(directory), '-Wl,-rpath,'+str(directory),
                     '-lfilter', '-lutil', '-pthread', '-o', str(directory/'app')])

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    @staticmethod
    def command(command, env=None):
        result = subprocess.run(command, env=env, text=True, capture_output=True)
        if result.returncode:
            raise AssertionError(f'{command!r}\nexit {result.returncode}\n{result.stdout}\n{result.stderr}')
        return result

    def run_probe(self, mode='normal', variant='good', library='probe', marker='v1', extra=None,
                  symlink=False):
        directory = Path(tempfile.mkdtemp(prefix='case-', dir=self.root))
        prefix = directory/'observed'
        env = {key: value for key, value in os.environ.items()
               if not key.startswith('CRYSTALHD_TEST_POSTFILTER') and key != 'LD_PRELOAD'}
        env.update(LD_PRELOAD=str(self.root/(library+'.so')),
                   CRYSTALHD_TEST_POSTFILTER_PREFIX=str(prefix),
                   CRYSTALHD_TEST_POSTFILTER_FILTER_PROVIDER=str(self.root/variant/'libfilter.so'),
                   CRYSTALHD_TEST_POSTFILTER_UTIL_PROVIDER=str(self.root/variant/'libutil.so'))
        if marker is not None:
            env['CRYSTALHD_TEST_POSTFILTER'] = marker
        if extra:
            env.update(extra)
        binary = self.root/'symlink-app' if symlink else self.root/variant/'app'
        result = self.command([str(binary), mode], env)
        application = json.loads(result.stdout)
        self.assertEqual(application['failures'], 0)
        path = Path(str(prefix)+'-'+str(application['pid'])+'.jsonl')
        records = [json.loads(line) for line in path.read_text().splitlines()] if path.exists() and mode != 'existing' else []
        return directory, path, records, application

    def assert_complete(self, records):
        self.assertTrue(records[0]['provider_ok'])
        self.assertTrue(records[-1]['complete'], records[-1])
        self.assertEqual(records[-1]['events'], len(records)-2)
        self.assertEqual([r['seq'] for r in records[1:-1]], list(range(len(records)-2)))
        for event in records[1:-1]:
            self.assertEqual(event['errno_before'], 77)
            self.assertEqual(event['errno_after'], 88)
            self.assertGreaterEqual(event['end_ns'], event['begin_ns'])

    def test_before_consumption_after_success_and_nested_forward_once(self):
        directory, _, records, _ = self.run_probe()
        self.assert_complete(records)
        events = records[1:-1]
        self.assertEqual([e['api'] for e in events], ['avfilter_graph_create_filter',
            'avfilter_graph_parse_ptr', 'av_buffersrc_add_frame',
            'av_buffersink_get_frame', 'avfilter_graph_free'])
        source, sink = events[2:4]
        self.assertEqual(source['ret'], 0)
        self.assertEqual(sink['ret'], 1)  # >=0, not just zero
        self.assertEqual(source['frame']['pts'], 123456)
        self.assertEqual(source['frame']['time_base'], [0, 1])
        self.assertEqual(sink['sink_time_base'], [1, 2])
        self.assertEqual(sink['sink_frame_rate'], [60000, 1001])
        for event, value in ((source, 10), (sink, 30)):
            frame = event['frame']; snapshot = frame['snapshot']
            self.assertEqual(frame['linesize'], [6, 6])
            self.assertEqual(snapshot['bytes'], 24)
            self.assertEqual((directory/snapshot['file']).read_bytes(), bytes([value])*16+bytes([value+80])*8)
            self.assertEqual((directory/snapshot['file']).stat().st_mode & 0o777, 0o600)
        self.assertEqual(records[-1]['snapshots'], 2)

    def test_source_error_still_snapshots_attempt_before_unchanged_error(self):
        _, _, records, _ = self.run_probe('source-error')
        self.assert_complete(records)
        event = records[3]
        self.assertEqual(event['ret'], -11)
        self.assertEqual(event['input_index'], 1)
        self.assertIsNotNone(event['frame']['snapshot'])

    def test_sink_eagain_and_eof_never_dereference_or_invent_output(self):
        for mode, wanted in (('sink-eagain', -11), ('sink-eof', -541478725)):
            with self.subTest(mode=mode):
                _, _, records, _ = self.run_probe(mode)
                self.assert_complete(records)
                event = records[4]
                self.assertEqual(event['ret'], wanted)
                self.assertIsNone(event['frame'])
                self.assertEqual(event['output_index'], 0)
                self.assertFalse(any(e['input_null'] for e in records[1:-1]))

    def test_null_and_explicit_close_are_only_original_observed_calls(self):
        _, _, records, _ = self.run_probe('null-close')
        self.assert_complete(records)
        null = [e for e in records[1:-1] if e['input_null']]
        self.assertEqual(len(null), 1)
        self.assertEqual(null[0]['api'], 'av_buffersrc_add_frame')
        self.assertIsNone(null[0]['frame'])
        close = next(e for e in records[1:-1] if e['api'] == 'av_buffersrc_close')
        self.assertEqual((close['close_pts'], close['flags']), (999, 7))

    def test_explicit_flags_preserve_keep_ref_and_exact_pull(self):
        _, _, records, _ = self.run_probe('flags')
        self.assert_complete(records)
        self.assertEqual(records[3]['api'], 'av_buffersrc_add_frame_flags')
        self.assertEqual(records[3]['flags'], 8)
        self.assertEqual(records[4]['api'], 'av_buffersink_get_frame_flags')
        self.assertEqual(records[4]['flags'], 5)

    def test_graph_address_reuse_has_new_lifetime_and_selection_reset(self):
        _, _, records, _ = self.run_probe('reuse')
        self.assert_complete(records)
        created = [e for e in records[1:-1] if e['api'] == 'avfilter_graph_create_filter']
        self.assertEqual([e['graph_id'] for e in created], [1, 2])
        self.assertEqual(created[0]['graph'], created[1]['graph'])
        inputs = [e for e in records[1:-1] if e['input_index']]
        self.assertEqual([e['input_index'] for e in inputs], [1, 1])
        self.assertEqual(records[-1]['snapshots'], 4)

    def test_exact_first_three_inputs_four_outputs_active_bytes(self):
        _, _, records, _ = self.run_probe('selection')
        self.assert_complete(records)
        inputs = [e for e in records[1:-1] if e['input_index']]
        outputs = [e for e in records[1:-1] if e['output_index']]
        self.assertEqual([e['input_index'] for e in inputs if e['frame']['snapshot']], [1, 2, 3])
        self.assertEqual([e['output_index'] for e in outputs if e['frame']['snapshot']], [1, 2, 3, 4])
        self.assertEqual(records[-1]['snapshots'], 7)

    def test_event_overflow_is_explicit_incomplete(self):
        _, _, records, _ = self.run_probe('overflow', library='small')
        footer = records[-1]
        self.assertFalse(footer['complete'])
        self.assertEqual(footer['events'], 32)
        self.assertEqual(footer['attempted_events'], 93)
        self.assertEqual(footer['dropped'], 61)

    def test_snapshot_overflow_is_explicit_incomplete(self):
        _, _, records, _ = self.run_probe('snapshot-overflow', library='few-images')
        self.assertFalse(records[-1]['complete'])
        self.assertEqual(records[-1]['snapshots'], 7)
        self.assertEqual(records[-1]['snapshot_failures'], 5)

    def test_selected_invalid_geometry_is_incomplete_not_a_truncated_pixel_claim(self):
        for mode in ('invalid-geometry', 'odd', 'negative-stride'):
            with self.subTest(mode=mode):
                _, _, records, _ = self.run_probe(mode)
                self.assertFalse(records[-1]['complete'])
                self.assertEqual(records[-1]['snapshot_failures'], 1)
                self.assertIsNone(records[3]['frame']['snapshot'])

    def test_raw_unknown_clock_and_crop_remain_raw(self):
        _, _, records, _ = self.run_probe('unknown')
        self.assert_complete(records)
        self.assertEqual(records[3]['frame']['pts'], -(1 << 63))
        self.assertEqual(records[3]['frame']['time_base'], [0, 1])
        _, _, records, _ = self.run_probe('crop')
        self.assert_complete(records)
        self.assertEqual(records[3]['frame']['crop'], [2, 2, 4, 6])

    def test_non_nv12_scalars_are_retained_without_nv12_snapshot(self):
        _, _, records, _ = self.run_probe('software')
        self.assert_complete(records)
        self.assertEqual(records[3]['frame']['format'], 0)
        self.assertIsNone(records[3]['frame']['snapshot'])

    def test_bad_provider_versions_never_read_foreign_frame(self):
        for variant in ('badfilter', 'badutil', 'unversioned'):
            with self.subTest(variant=variant):
                _, _, records, _ = self.run_probe('foreign-invalid', variant=variant)
                self.assertFalse(records[0]['provider_ok'])
                self.assertFalse(records[-1]['complete'])
                self.assertEqual(records[-1]['events'], 0)

    def test_wrong_provider_path_disables_all_foreign_object_reads(self):
        _, _, records, _ = self.run_probe('foreign-invalid', extra={
            'CRYSTALHD_TEST_POSTFILTER_FILTER_PROVIDER': str(self.root/'badfilter/libfilter.so')})
        self.assertFalse(records[0]['provider_ok'])
        self.assertEqual(records[-1]['events'], 0)

    def test_provider_canonical_symlink_is_accepted(self):
        _, _, records, _ = self.run_probe(symlink=True)
        self.assert_complete(records)
        self.assertEqual(records[0]['filter_provider'], str(self.root/'good/libfilter.so'))

    def test_marker_absent_unknown_or_relative_prefix_only_forward(self):
        for marker in (None, 'other'):
            _, path, records, _ = self.run_probe(marker=marker)
            self.assertFalse(path.exists())
            self.assertEqual(records, [])
        directory, path, records, _ = self.run_probe(extra={'CRYSTALHD_TEST_POSTFILTER_PREFIX': 'relative'})
        self.assertFalse(path.exists())
        self.assertEqual(list(directory.iterdir()), [])
        self.assertEqual(records, [])

    def test_open_graph_is_incomplete(self):
        _, _, records, _ = self.run_probe('open')
        self.assertFalse(records[-1]['complete'])
        self.assertEqual(records[-1]['open_graphs'], 1)

    def test_truncated_filter_expression_is_explicit_incomplete(self):
        _, _, records, _ = self.run_probe('truncated')
        self.assertFalse(records[-1]['complete'])
        self.assertEqual(records[-1]['string_truncations'], 1)

    def test_preexec_fork_child_forwards_but_never_publishes_inherited_ledger(self):
        directory, _, records, application = self.run_probe('fork-copy')
        self.assertFalse(records[-1]['complete'])
        self.assertTrue(records[-1]['fork_seen'])
        child = application['child_pid']
        self.assertFalse((directory/f'observed-{child}.jsonl').exists())
        self.assertEqual(records[-1]['events'], 5)

    def test_inflight_call_at_normal_exit_is_explicitly_incomplete(self):
        _, _, records, _ = self.run_probe('inflight')
        self.assertFalse(records[-1]['complete'])
        self.assertEqual(records[-1]['inflight'], 1)
        self.assertEqual(records[-1]['unpublished'], 1)
        self.assertEqual(records[-1]['attempted_events'], 3)
        self.assertEqual(records[-1]['events'], 2)

    def test_existing_selected_snapshot_is_not_overwritten_and_report_is_incomplete(self):
        directory, _, records, application = self.run_probe('snapshot-existing')
        self.assertFalse(records[-1]['complete'])
        self.assertEqual(records[-1]['snapshot_failures'], 1)
        self.assertEqual((directory/f"observed-{application['pid']}-s00.nv12").read_text(),
                         'original snapshot\n')

    def test_preexisting_pid_report_is_never_overwritten(self):
        _, path, records, _ = self.run_probe('existing')
        self.assertEqual(path.read_text(), 'original\n')
        self.assertEqual(records, [])


if __name__ == '__main__':
    unittest.main()
