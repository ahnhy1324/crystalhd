// SPDX-License-Identifier: GPL-2.0-or-later
// Reuse our unchanged provider/ABI/captured-prefix oracle, never external code.
#ifndef CRYSTALHD_COUPLED_REFERENCE_SOURCE
#error "Select latest maintained original-provider reference source"
#endif
#define main original_reference_main
#include CRYSTALHD_COUPLED_REFERENCE_SOURCE
#undef main
#include "filter-bridge.h"
DECL(av_frame_unref);
static AVFrame *coupled_out;
static const char *coupled_buffer_args,*coupled_filter_args;
int coupled_original_oracle(int argc,char **argv)
{
    const char *util_path = NULL;
    for(int i=1;i+1<argc;++i) {
        if(!strcmp(argv[i],"--buffer-args")) coupled_buffer_args=argv[i+1];
        if(!strcmp(argv[i],"--filter-args")) coupled_filter_args=argv[i+1];
        if(!strcmp(argv[i],"--util-provider")) util_path=argv[i+1];
    }
    check(coupled_buffer_args && coupled_filter_args && util_path,"coupled graph/provider arguments required");
    int result=original_reference_main(argc,argv);
    check(result==0 && input_count==3 && output_count==4 && !graph,"unchanged captured prefix must retire");
    char canonical[PATH_MAX];check(realpath(util_path,canonical)!=NULL,"original util canonical path");
    void *util=dlopen(canonical,RTLD_NOW|RTLD_LOCAL);check(util!=NULL,"original util retained");
    LOAD(util,av_frame_unref,"LIBAVUTIL_61",canonical);
    coupled_reset();
    return result;
}
void coupled_close(void)
{
    if(coupled_out)fn_av_frame_free(&coupled_out);
    if(graph)fn_avfilter_graph_free(&graph);
}
void coupled_reset(void)
{
    coupled_close();setup(coupled_buffer_args,coupled_filter_args);
    coupled_out=fn_av_frame_alloc();check(coupled_out!=NULL,"coupled output allocation");
    AVRational tb=fn_av_buffersink_get_time_base(sink);
    check(tb.num==1 && tb.den==2,"unchanged graph output timebase");
}
struct CoupledFrame coupled_input(unsigned pattern,int64_t pts,int bottom_first)
{
    check(pattern<3,"one of three bounded pattern metadata templates");
    const int64_t *v=inputs[pattern].v;
    AVFrame *f=fn_av_frame_alloc();check(f!=NULL,"coupled frame allocation");
    f->buf[0]=fn_av_buffer_allocz(1920U*1080U*3U/2U);check(f->buf[0]!=NULL,"coupled owned client allocation");
    f->format=AV_PIX_FMT_NV12;f->width=1920;f->height=1080;
    f->data[0]=f->buf[0]->data;f->data[1]=f->data[0]+1920U*1080U;
    f->linesize[0]=f->linesize[1]=1920;
    f->pts=f->best_effort_timestamp=pts;f->duration=v[6];
    f->time_base=(AVRational){(int)v[7],(int)v[8]};
    f->flags=(int)v[9];
    if(bottom_first)f->flags &= ~AV_FRAME_FLAG_TOP_FIELD_FIRST;
    else f->flags |= AV_FRAME_FLAG_TOP_FIELD_FIRST;
    f->sample_aspect_ratio=(AVRational){(int)v[10],(int)v[11]};
    f->color_range=v[12];f->color_primaries=v[13];f->color_trc=v[14];
    f->colorspace=v[15];f->chroma_location=v[16];
    struct CoupledFrame result={.owner=f,.planes={f->data[0],f->data[1]},
        .pitches={f->linesize[0],f->linesize[1]},.width=f->width,.height=f->height,
        .flags=f->flags,.pts=f->pts,.best=f->best_effort_timestamp};
    return result;
}
int coupled_add(struct CoupledFrame *frame)
{
    AVFrame *f=frame->owner;check(f!=NULL,"owned input required");
    int result=fn_av_buffersrc_add_frame(source,f);
    fn_av_frame_free(&f);memset(frame,0,sizeof(*frame));return result;
}
int coupled_get(struct CoupledFrame *frame)
{
    int result=fn_av_buffersink_get_frame(sink,coupled_out);
    memset(frame,0,sizeof(*frame));
    if(!result) {
        AVFrame *f=coupled_out;
        check(f->format==AV_PIX_FMT_NV12 && f->width==1920 && f->height==1080 &&
              f->linesize[0]>=1920 && f->linesize[1]>=1920,"actual bounded NV12 output");
        check(f->duration==0 && f->time_base.num==0 && f->time_base.den==1 &&
              f->color_range==0 && f->color_primaries==2 && f->color_trc==2 &&
              f->colorspace==2 && f->chroma_location==0 && !f->crop_top &&
              !f->crop_bottom && !f->crop_left && !f->crop_right,"unchanged output metadata");
        *frame=(struct CoupledFrame){.owner=f,.planes={f->data[0],f->data[1]},
            .pitches={f->linesize[0],f->linesize[1]},.width=f->width,.height=f->height,
            .flags=f->flags,.pts=f->pts,.best=f->best_effort_timestamp};
    }
    return result;
}
void coupled_release(void){fn_av_frame_unref(coupled_out);}
