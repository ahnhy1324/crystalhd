// SPDX-License-Identifier: GPL-2.0-or-later
/* OWN metadata-only interposer: no external product changes or stock/release
 * qualification. Build against unchanged original format63/codec63/util61
 * public headers. Tiny owned-provider controls establish only OUR behavior. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <libavformat/avformat.h>
#include <libavformat/version.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/version.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libavutil/version.h>
#if LIBAVFORMAT_VERSION_INT != AV_VERSION_INT(63,1,102) || LIBAVCODEC_VERSION_INT != AV_VERSION_INT(63,1,102) || LIBAVUTIL_VERSION_INT != AV_VERSION_INT(61,1,102)
#error "Packet observer requires unchanged original format63.1.102/codec63.1.102/util61.1.102 headers"
#endif
#ifndef PACKET_ADMISSION_CAPACITY
#define PACKET_ADMISSION_CAPACITY 32768
#endif
#ifndef PACKET_ADMISSION_LIFETIMES
#define PACKET_ADMISSION_LIFETIMES 256
#endif
#if PACKET_ADMISSION_CAPACITY < 2 || PACKET_ADMISSION_CAPACITY > 32768 || PACKET_ADMISSION_LIFETIMES < 2 || PACKET_ADMISSION_LIFETIMES > 256
#error "Invalid fixed metadata observer capacity"
#endif
#define MAX_STREAMS 1024
#define URL_SIZE 1024
#define FORMAT_PROVIDER "/app/lib/libavformat.so.63.1.102"
#define CODEC_PROVIDER "/app/lib/libavcodec.so.63.1.102"
#define UTIL_PROVIDER "/app/lib/libavutil.so.61.1.102"
typedef int (*format_open_fn)(AVFormatContext **, const char *, const AVInputFormat *, AVDictionary **);
typedef int (*info_fn)(AVFormatContext *, AVDictionary **);
typedef int (*read_fn)(AVFormatContext *, AVPacket *);
typedef void (*format_close_fn)(AVFormatContext **);
typedef int (*codec_open_fn)(AVCodecContext *, const AVCodec *, AVDictionary **);
typedef int (*send_fn)(AVCodecContext *, const AVPacket *);
typedef int (*receive_fn)(AVCodecContext *, AVFrame *);
typedef void (*flush_fn)(AVCodecContext *);
typedef void (*codec_free_fn)(AVCodecContext **);
typedef unsigned (*version_fn)(void);
struct lifetime {
    uintptr_t address;
    unsigned instance, epoch, busy;
    bool open, info_complete;
};
struct context_data {
    int type, id, width, height, pix_fmt, skip_frame, skip_idct, skip_loop_filter;
    AVRational pkt_tb;
    int threads, thread_type, active_thread_type, has_b_frames;
};
struct packet_data {
    int64_t pts, dts, duration, pos;
    int size, flags, stream_index, side_data_elems;
    AVRational tb;
    bool data_null;
};
struct stream_data {
    int index, id, type, codec, width, height;
    AVRational tb;
    int64_t start_time, duration;
};
struct frame_data {
    int64_t pts, best, duration;
    AVRational tb;
    int format, width, height, flags;
    uint32_t surface;
    bool vaapi;
};
struct event {
    uint64_t begin, end;
    uintptr_t format_address, codec_address, caller;
    unsigned format_instance, codec_instance, epoch, old_epoch, new_epoch;
    int format_slot, codec_slot;
    const char *api;
    pid_t tid;
    int ret, before_errno, after_errno;
    bool void_ret, concurrent, opened_before, unopened_free, packet_null;
    bool has_format, has_codec, has_stream, has_packet, has_frame;
    bool input_url_present, format_url_present, info_complete;
    int64_t format_start;
    unsigned streams;
    char input_url[URL_SIZE], format_url[URL_SIZE];
    struct context_data codec;
    struct packet_data packet;
    struct stream_data stream;
    struct frame_data frame;
    atomic_bool ready;
};
static struct event events[PACKET_ADMISSION_CAPACITY];
static struct lifetime formats[PACKET_ADMISSION_LIFETIMES], codecs[PACKET_ADMISSION_LIFETIMES];
static unsigned format_slots, codec_slots, next_format_id, next_codec_id;
static atomic_uint attempted, dropped, lifecycle_failures, string_truncations, inflight;
static atomic_bool clock_failure, fork_seen;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static bool active, provider_ok, publishing;
static pid_t owner_pid;
static char prefix[PATH_MAX], format_path[PATH_MAX], codec_path[PATH_MAX], util_path[PATH_MAX];
static unsigned format_version, codec_version, util_version;
static format_open_fn next_format_open;
static info_fn next_info;
static read_fn next_read;
static format_close_fn next_format_close;
static codec_open_fn next_codec_open;
static send_fn next_send;
static receive_fn next_receive;
static flush_fn next_flush;
static codec_free_fn next_codec_free;
static _Thread_local unsigned forwarding;
static bool describe(void *symbol, char *path, Dl_info *info)
{
    memset(info, 0, sizeof(*info));
    return symbol && dladdr(symbol, info) && info->dli_fname && realpath(info->dli_fname, path);
}
static bool expected(const char *actual, const char *variable, const char *fallback)
{
    char canonical[PATH_MAX]; const char *wanted = getenv(variable);
    if (!wanted) wanted = fallback;
    return wanted[0] == '/' && realpath(wanted, canonical) && !strcmp(actual, canonical);
}
static void fork_prepare(void)
{
    if (active && owner_pid == getpid()) atomic_store(&fork_seen, true);
}
static void initialize(void)
{
    int saved = errno; owner_pid = getpid();
    const char *marker = getenv("CRYSTALHD_TEST_PACKET_ADMISSION");
    const char *path = getenv("CRYSTALHD_TEST_PACKET_ADMISSION_PREFIX");
    active = marker && !strcmp(marker,"v1") && path && path[0]=='/' && strlen(path)<sizeof(prefix);
    if (active) memcpy(prefix,path,strlen(path)+1);
    const char *names[] = {"avformat_open_input","avformat_find_stream_info","av_read_frame","avformat_close_input",
        "avcodec_open2","avcodec_send_packet","avcodec_receive_frame","avcodec_flush_buffers","avcodec_free_context"};
    void *symbols[9]; bool versioned = true;
    for (unsigned i=0;i<9;++i) {
        symbols[i]=dlvsym(RTLD_NEXT,names[i],i<4?"LIBAVFORMAT_63":"LIBAVCODEC_63");
        if (!symbols[i]) {versioned=false;symbols[i]=dlsym(RTLD_NEXT,names[i]);}
    }
    next_format_open=(format_open_fn)symbols[0]; next_info=(info_fn)symbols[1]; next_read=(read_fn)symbols[2];
    next_format_close=(format_close_fn)symbols[3];next_codec_open=(codec_open_fn)symbols[4];next_send=(send_fn)symbols[5];
    next_receive=(receive_fn)symbols[6];next_flush=(flush_fn)symbols[7];next_codec_free=(codec_free_fn)symbols[8];
    if (active) {
        void *fv=dlvsym(RTLD_NEXT,"avformat_version","LIBAVFORMAT_63");
        void *cv=dlvsym(RTLD_NEXT,"avcodec_version","LIBAVCODEC_63");
        void *uv=dlvsym(RTLD_NEXT,"avutil_version","LIBAVUTIL_61");
        Dl_info fi,ci,ui;
        bool valid=versioned && describe(fv,format_path,&fi) && describe(cv,codec_path,&ci) && describe(uv,util_path,&ui);
        if (valid) {
            format_version=((version_fn)fv)();codec_version=((version_fn)cv)();util_version=((version_fn)uv)();
            valid=format_version==LIBAVFORMAT_VERSION_INT && codec_version==LIBAVCODEC_VERSION_INT && util_version==LIBAVUTIL_VERSION_INT &&
                expected(format_path,"CRYSTALHD_TEST_PACKET_ADMISSION_FORMAT_PROVIDER",FORMAT_PROVIDER) &&
                expected(codec_path,"CRYSTALHD_TEST_PACKET_ADMISSION_CODEC_PROVIDER",CODEC_PROVIDER) &&
                expected(util_path,"CRYSTALHD_TEST_PACKET_ADMISSION_UTIL_PROVIDER",UTIL_PROVIDER);
            for (unsigned i=0;i<9;++i) {
                char actual[PATH_MAX];Dl_info info;
                if (!describe(symbols[i],actual,&info) || info.dli_fbase!=(i<4?fi.dli_fbase:ci.dli_fbase) ||
                    strcmp(actual,i<4?format_path:codec_path)) valid=false;
            }
        }
        provider_ok=valid;
        if (pthread_atfork(fork_prepare,NULL,NULL)) atomic_fetch_add(&lifecycle_failures,1);
    }
    errno=saved;
}
__attribute__((constructor)) static void start_observer(void)
{
    int saved=errno;pthread_once(&once,initialize);errno=saved;
}
static uint64_t now_ns(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC,&t)) {atomic_store(&clock_failure,true);return 0;}
    return (uint64_t)t.tv_sec*UINT64_C(1000000000)+(uint64_t)t.tv_nsec;
}
static bool observing(void)
{
    if (!active || !provider_ok || owner_pid!=getpid() || forwarding) return false;
    pthread_mutex_lock(&lock);bool watch=!publishing;
    if (watch) atomic_fetch_add(&inflight,1);
    pthread_mutex_unlock(&lock);return watch;
}
static int find_slot(struct lifetime *table,unsigned *count,uintptr_t address,bool create)
{
    if (!address) return -1;
    for (unsigned i=0;i<*count;++i) if (table[i].address==address) return (int)i;
    if (!create) return -1;
    if (*count==PACKET_ADMISSION_LIFETIMES) {atomic_fetch_add(&lifecycle_failures,1);return -1;}
    unsigned index=(*count)++;table[index].address=address;return (int)index;
}
static void codec_data(struct event *e,const AVCodecContext *c)
{
    e->has_codec=true;struct context_data *d=&e->codec;
    d->type=c->codec_type;d->id=c->codec_id;d->width=c->width;d->height=c->height;d->pix_fmt=c->pix_fmt;
    d->skip_frame=c->skip_frame;d->skip_idct=c->skip_idct;d->skip_loop_filter=c->skip_loop_filter;
    d->pkt_tb=c->pkt_timebase;d->threads=c->thread_count;d->thread_type=c->thread_type;
    d->active_thread_type=c->active_thread_type;d->has_b_frames=c->has_b_frames;
}
static void format_data(struct event *e,const AVFormatContext *f,bool info)
{
    e->has_format=true;e->format_start=f->start_time;e->streams=f->nb_streams;e->info_complete=info;
}
/* A tracked successful context is required for data APIs. Open/free are also
 * recorded for un-opened objects without inventing a successful lifetime. */
static struct event *begin_event(const char *api,void *context,bool format,bool opening,bool freeing,void *caller)
{
    unsigned index=atomic_fetch_add(&attempted,1);
    if (index>=PACKET_ADMISSION_CAPACITY) {atomic_fetch_add(&dropped,1);return NULL;}
    struct event *e=&events[index];e->api=api;e->caller=(uintptr_t)caller;e->tid=(pid_t)syscall(SYS_gettid);
    e->begin=now_ns();e->format_slot=e->codec_slot=-1;
    pthread_mutex_lock(&lock);
    struct lifetime *table=format?formats:codecs;unsigned *count=format?&format_slots:&codec_slots;
    int slot=find_slot(table,count,(uintptr_t)context,opening);
    struct lifetime *life=slot>=0?&table[slot]:NULL;
    if (format) {e->format_address=(uintptr_t)context;e->format_slot=slot;} else {e->codec_address=(uintptr_t)context;e->codec_slot=slot;}
    e->opened_before=life && life->open;e->unopened_free=freeing && !e->opened_before;
    if (life) {
        if (life->busy++) {e->concurrent=true;atomic_fetch_add(&lifecycle_failures,1);}
        if (life->open) {
            if (format) e->format_instance=life->instance;
            else {e->codec_instance=life->instance;e->epoch=life->epoch;}
        }
    }
    if (!opening && !freeing && !e->opened_before) atomic_fetch_add(&lifecycle_failures,1);
    if (context && !e->concurrent && (e->opened_before || opening)) {
        if (format && !opening) format_data(e,context,life->info_complete);
        if (!format) codec_data(e,context);
    }
    pthread_mutex_unlock(&lock);return e;
}
static void finish_event(struct event *e,int ret,int before,int after)
{
    if (e) {
        e->ret=ret;e->before_errno=before;e->after_errno=after;e->end=now_ns();
        pthread_mutex_lock(&lock);
        if (e->format_slot>=0) {if (formats[e->format_slot].busy) --formats[e->format_slot].busy;else atomic_fetch_add(&lifecycle_failures,1);}
        if (e->codec_slot>=0) {if (codecs[e->codec_slot].busy) --codecs[e->codec_slot].busy;else atomic_fetch_add(&lifecycle_failures,1);}
        pthread_mutex_unlock(&lock);
        atomic_store_explicit(&e->ready,true,memory_order_release);
    }
    atomic_fetch_sub_explicit(&inflight,1,memory_order_release);
}
static void copy_url(char *to,bool *present,const char *from)
{
    *present=from!=NULL;if (!from) return;
    size_t n=strnlen(from,URL_SIZE);
    if (n==URL_SIZE) {n=URL_SIZE-1;atomic_fetch_add(&string_truncations,1);}
    memcpy(to,from,n);to[n]=0;
}
static void packet_data(struct event *e,const AVPacket *p)
{
    if (!p) return;
    e->has_packet=true;e->packet=(struct packet_data){.pts=p->pts,.dts=p->dts,.duration=p->duration,.pos=p->pos,
        .size=p->size,.flags=p->flags,.stream_index=p->stream_index,.tb=p->time_base,
        .data_null=p->data==NULL,.side_data_elems=p->side_data_elems};
}
static void stream_data(struct event *e,const AVFormatContext *f)
{
    int index=e->packet.stream_index;
    if (index<0 || f->nb_streams>MAX_STREAMS || (unsigned)index>=f->nb_streams || !f->streams || !f->streams[index] || !f->streams[index]->codecpar) {
        atomic_fetch_add(&lifecycle_failures,1);return;
    }
    const AVStream *s=f->streams[index];const AVCodecParameters *p=s->codecpar;e->has_stream=true;
    e->stream=(struct stream_data){.index=index,.id=s->id,.type=p->codec_type,.codec=p->codec_id,
        .width=p->width,.height=p->height,.tb=s->time_base,.start_time=s->start_time,.duration=s->duration};
}
int avformat_open_input(AVFormatContext **context,const char *url,const AVInputFormat *input,AVDictionary **options)
{
    void *caller=__builtin_return_address(0);int entry=errno;pthread_once(&once,initialize);
    if (!next_format_open) {errno=entry;return -ENOSYS;}
    bool watch=observing();struct event *e=watch?begin_event("avformat_open_input",context?*context:NULL,true,true,false,caller):NULL;
    if (e) copy_url(e->input_url,&e->input_url_present,url);
    ++forwarding;errno=entry;int result=next_format_open(context,url,input,options);int saved=errno;--forwarding;
    if (e && result>=0) {
        pthread_mutex_lock(&lock);
        AVFormatContext *f=context?*context:NULL;
        int slot=find_slot(formats,&format_slots,(uintptr_t)f,true);
        if (!f || slot<0) atomic_fetch_add(&lifecycle_failures,1);
        else {
            struct lifetime *life=&formats[slot];
            if (e->format_slot!=slot) {if (e->format_slot>=0) atomic_fetch_add(&lifecycle_failures,1);++life->busy;e->format_slot=slot;}
            if (!life->open) {life->instance=++next_format_id;life->info_complete=false;life->open=true;}
            e->format_address=(uintptr_t)f;e->format_instance=life->instance;
            if (!e->concurrent && life->busy==1) {format_data(e,f,life->info_complete);copy_url(e->format_url,&e->format_url_present,f->url);}
            else {e->concurrent=true;atomic_fetch_add(&lifecycle_failures,1);}
        }
        pthread_mutex_unlock(&lock);
    }
    if (watch) finish_event(e,result,entry,saved);
    errno=saved;return result;
}
int avformat_find_stream_info(AVFormatContext *context,AVDictionary **options)
{
    void *caller=__builtin_return_address(0);int entry=errno;pthread_once(&once,initialize);
    if (!next_info) {errno=entry;return -ENOSYS;}
    bool watch=observing();struct event *e=watch?begin_event("avformat_find_stream_info",context,true,false,false,caller):NULL;
    ++forwarding;errno=entry;int result=next_info(context,options);int saved=errno;--forwarding;
    if (e && result>=0 && e->format_slot>=0) {
        pthread_mutex_lock(&lock);
        struct lifetime *life=&formats[e->format_slot];
        if (life->open && life->instance==e->format_instance && life->busy==1) life->info_complete=true;
        else {e->concurrent=true;atomic_fetch_add(&lifecycle_failures,1);}
        pthread_mutex_unlock(&lock);
    }
    if (watch) finish_event(e,result,entry,saved);
    errno=saved;return result;
}
int av_read_frame(AVFormatContext *context,AVPacket *packet)
{
    void *caller=__builtin_return_address(0);int entry=errno;pthread_once(&once,initialize);
    if (!next_read) {errno=entry;return -ENOSYS;}
    bool watch=observing();struct event *e=watch?begin_event("av_read_frame",context,true,false,false,caller):NULL;
    ++forwarding;errno=entry;int result=next_read(context,packet);int saved=errno;--forwarding;
    if (e && result>=0 && e->opened_before && !e->concurrent) {
        pthread_mutex_lock(&lock);
        if (e->format_slot>=0 && formats[e->format_slot].busy==1 && formats[e->format_slot].open && formats[e->format_slot].instance==e->format_instance) {
            /* An overlapping close may have finished while read was blocked;
             * busy==1 alone cannot prove this context is still alive. */
            format_data(e,context,formats[e->format_slot].info_complete);
            packet_data(e,packet);if (packet) stream_data(e,context);else atomic_fetch_add(&lifecycle_failures,1);
        }
        else {e->concurrent=true;atomic_fetch_add(&lifecycle_failures,1);}
        pthread_mutex_unlock(&lock);
    }
    if (watch) finish_event(e,result,entry,saved);
    errno=saved;return result;
}
void avformat_close_input(AVFormatContext **context)
{
    void *caller=__builtin_return_address(0);int entry=errno;pthread_once(&once,initialize);
    if (!next_format_close) {errno=entry;return;}
    bool watch=observing();struct event *e=watch?begin_event("avformat_close_input",context?*context:NULL,true,false,true,caller):NULL;
    if (e) e->void_ret=true;
    ++forwarding;errno=entry;next_format_close(context);int saved=errno;--forwarding;
    if (e && e->format_slot>=0) {pthread_mutex_lock(&lock);formats[e->format_slot].open=false;pthread_mutex_unlock(&lock);}
    if (watch) finish_event(e,0,entry,saved);
    errno=saved;
}
int avcodec_open2(AVCodecContext *context,const AVCodec *codec,AVDictionary **options)
{
    void *caller=__builtin_return_address(0);int entry=errno;pthread_once(&once,initialize);
    if (!next_codec_open) {errno=entry;return -ENOSYS;}
    bool watch=observing();struct event *e=watch?begin_event("avcodec_open2",context,false,true,false,caller):NULL;
    ++forwarding;errno=entry;int result=next_codec_open(context,codec,options);int saved=errno;--forwarding;
    if (e && result>=0) {
        pthread_mutex_lock(&lock);
        if (e->codec_slot<0) atomic_fetch_add(&lifecycle_failures,1);
        else {
            struct lifetime *life=&codecs[e->codec_slot];
            if (!life->open) {life->instance=++next_codec_id;life->epoch=1;life->open=true;}
            e->codec_instance=life->instance;e->epoch=life->epoch;
        }
        pthread_mutex_unlock(&lock);
    }
    if (watch) finish_event(e,result,entry,saved);
    errno=saved;return result;
}
int avcodec_send_packet(AVCodecContext *context,const AVPacket *packet)
{
    void *caller=__builtin_return_address(0);int entry=errno;pthread_once(&once,initialize);
    if (!next_send) {errno=entry;return -ENOSYS;}
    bool watch=observing();struct event *e=watch?begin_event("avcodec_send_packet",context,false,false,false,caller):NULL;
    if (e) {e->packet_null=packet==NULL;if (e->opened_before && !e->concurrent) packet_data(e,packet);}
    ++forwarding;errno=entry;int result=next_send(context,packet);int saved=errno;--forwarding;
    if (watch) finish_event(e,result,entry,saved);
    errno=saved;return result;
}
int avcodec_receive_frame(AVCodecContext *context,AVFrame *frame)
{
    void *caller=__builtin_return_address(0);int entry=errno;pthread_once(&once,initialize);
    if (!next_receive) {errno=entry;return -ENOSYS;}
    bool watch=observing();struct event *e=watch?begin_event("avcodec_receive_frame",context,false,false,false,caller):NULL;
    ++forwarding;errno=entry;int result=next_receive(context,frame);int saved=errno;--forwarding;
    if (e && result>=0 && e->opened_before && !e->concurrent) {
        if (!frame) atomic_fetch_add(&lifecycle_failures,1);
        else {
            e->has_frame=true;e->frame=(struct frame_data){.pts=frame->pts,.best=frame->best_effort_timestamp,.duration=frame->duration,
                .tb=frame->time_base,.format=frame->format,.width=frame->width,.height=frame->height,.flags=frame->flags,
                .vaapi=frame->format==AV_PIX_FMT_VAAPI};
            if (e->frame.vaapi) e->frame.surface=(uint32_t)(uintptr_t)frame->data[3];
        }
    }
    if (watch) finish_event(e,result,entry,saved);
    errno=saved;return result;
}
void avcodec_flush_buffers(AVCodecContext *context)
{
    void *caller=__builtin_return_address(0);int entry=errno;pthread_once(&once,initialize);
    if (!next_flush) {errno=entry;return;}
    bool watch=observing();struct event *e=watch?begin_event("avcodec_flush_buffers",context,false,false,false,caller):NULL;
    if (e) {e->void_ret=true;e->old_epoch=e->epoch;}
    ++forwarding;errno=entry;next_flush(context);int saved=errno;--forwarding;
    if (e && e->codec_slot>=0 && e->opened_before) {
        pthread_mutex_lock(&lock);
        struct lifetime *life=&codecs[e->codec_slot];
        if (life->open && life->instance==e->codec_instance && life->epoch==e->old_epoch && life->busy==1) e->new_epoch=++life->epoch;
        else {e->concurrent=true;atomic_fetch_add(&lifecycle_failures,1);}
        pthread_mutex_unlock(&lock);
    }
    if (watch) finish_event(e,0,entry,saved);
    errno=saved;
}
void avcodec_free_context(AVCodecContext **context)
{
    void *caller=__builtin_return_address(0);int entry=errno;pthread_once(&once,initialize);
    if (!next_codec_free) {errno=entry;return;}
    bool watch=observing();struct event *e=watch?begin_event("avcodec_free_context",context?*context:NULL,false,false,true,caller):NULL;
    if (e) e->void_ret=true;
    ++forwarding;errno=entry;next_codec_free(context);int saved=errno;--forwarding;
    if (e && e->codec_slot>=0) {pthread_mutex_lock(&lock);codecs[e->codec_slot].open=false;pthread_mutex_unlock(&lock);}
    if (watch) finish_event(e,0,entry,saved);
    errno=saved;
}
static bool write_all(int fd,const void *data,size_t size)
{
    const unsigned char *p=data;
    while (size) {ssize_t n=write(fd,p,size);if (n<0 && errno==EINTR) continue;if (n<=0) return false;p+=n;size-=(size_t)n;}
    return true;
}
static bool string(int fd,const char *text)
{
    if (!write_all(fd,"\"",1)) return false;
    for (const unsigned char *p=(const unsigned char *)text;*p;++p) {
        char escaped[7];size_t n=1;
        if (*p=='"'||*p=='\\') {escaped[0]='\\';escaped[1]=(char)*p;n=2;}
        else if (*p<32||*p>=127) {snprintf(escaped,sizeof(escaped),"\\u%04x",*p);n=6;}
        else escaped[0]=(char)*p;
        if (!write_all(fd,escaped,n)) return false;
    }
    return write_all(fd,"\"",1);
}
static bool print_event(int fd,unsigned seq,const struct event *e)
{
    if (dprintf(fd,"{\"type\":\"event\",\"seq\":%u,\"pid\":%ld,\"tid\":%ld,\"begin_ns\":%"PRIu64",\"end_ns\":%"PRIu64",\"api\":\"%s\",\"ret\":",seq,(long)owner_pid,(long)e->tid,e->begin,e->end,e->api)<0) return false;
    if (e->void_ret?dprintf(fd,"null")<0:dprintf(fd,"%d",e->ret)<0) return false;
    if (dprintf(fd,",\"errno_before\":%d,\"errno_after\":%d,\"format_instance\":%u,\"codec_instance\":%u,\"codec_epoch\":%u,\"format_context\":\"0x%"PRIxPTR"\",\"codec_context\":\"0x%"PRIxPTR"\",\"caller\":\"0x%"PRIxPTR"\",\"concurrent\":%s,\"opened_before\":%s,\"unopened_free\":%s,\"flush_old_epoch\":%u,\"flush_new_epoch\":%u,\"packet_null\":%s,\"input_url\":",e->before_errno,e->after_errno,e->format_instance,e->codec_instance,e->epoch,e->format_address,e->codec_address,e->caller,e->concurrent?"true":"false",e->opened_before?"true":"false",e->unopened_free?"true":"false",e->old_epoch,e->new_epoch,e->packet_null?"true":"false")<0) return false;
    if (e->input_url_present?!string(fd,e->input_url):dprintf(fd,"null")<0) return false;
    if (dprintf(fd,",\"format_url\":")<0 || (e->format_url_present?!string(fd,e->format_url):dprintf(fd,"null")<0)) return false;
    if (dprintf(fd,",\"format\":")<0) return false;
    if (e->has_format) {if (dprintf(fd,"{\"start_time\":%"PRId64",\"nb_streams\":%u,\"info_complete\":%s}",e->format_start,e->streams,e->info_complete?"true":"false")<0) return false;}
    else if (dprintf(fd,"null")<0) return false;
    if (dprintf(fd,",\"codec\":")<0) return false;
    if (e->has_codec) {
        const struct context_data *c=&e->codec;
        if (dprintf(fd,"{\"codec_type\":%d,\"codec_id\":%d,\"width\":%d,\"height\":%d,\"pix_fmt\":%d,\"skip_frame\":%d,\"skip_idct\":%d,\"skip_loop_filter\":%d,\"pkt_timebase\":[%d,%d],\"thread_count\":%d,\"thread_type\":%d,\"active_thread_type\":%d,\"has_b_frames\":%d}",c->type,c->id,c->width,c->height,c->pix_fmt,c->skip_frame,c->skip_idct,c->skip_loop_filter,c->pkt_tb.num,c->pkt_tb.den,c->threads,c->thread_type,c->active_thread_type,c->has_b_frames)<0) return false;
    } else if (dprintf(fd,"null")<0) return false;
    if (dprintf(fd,",\"stream\":")<0) return false;
    if (e->has_stream) {
        const struct stream_data *s=&e->stream;
        if (dprintf(fd,"{\"index\":%d,\"id\":%d,\"codec_type\":%d,\"codec_id\":%d,\"width\":%d,\"height\":%d,\"time_base\":[%d,%d],\"start_time\":%"PRId64",\"duration\":%"PRId64"}",s->index,s->id,s->type,s->codec,s->width,s->height,s->tb.num,s->tb.den,s->start_time,s->duration)<0) return false;
    } else if (dprintf(fd,"null")<0) return false;
    if (dprintf(fd,",\"packet\":")<0) return false;
    if (e->has_packet) {
        const struct packet_data *p=&e->packet;
        if (dprintf(fd,"{\"pts\":%"PRId64",\"dts\":%"PRId64",\"duration\":%"PRId64",\"pos\":%"PRId64",\"size\":%d,\"flags\":%d,\"stream_index\":%d,\"time_base\":[%d,%d],\"data_null\":%s,\"side_data_elems\":%d}",p->pts,p->dts,p->duration,p->pos,p->size,p->flags,p->stream_index,p->tb.num,p->tb.den,p->data_null?"true":"false",p->side_data_elems)<0) return false;
    } else if (dprintf(fd,"null")<0) return false;
    if (dprintf(fd,",\"frame\":")<0) return false;
    if (e->has_frame) {
        const struct frame_data *f=&e->frame;
        if (dprintf(fd,"{\"pts\":%"PRId64",\"best_effort_timestamp\":%"PRId64",\"duration\":%"PRId64",\"time_base\":[%d,%d],\"format\":%d,\"width\":%d,\"height\":%d,\"flags\":%d,\"va_surface\":",f->pts,f->best,f->duration,f->tb.num,f->tb.den,f->format,f->width,f->height,f->flags)<0) return false;
        if (f->vaapi?dprintf(fd,"%"PRIu32,f->surface)<0:dprintf(fd,"null")<0) return false;
        if (dprintf(fd,"}")<0) return false;
    } else if (dprintf(fd,"null")<0) return false;
    return dprintf(fd,"}\n")>=0;
}
__attribute__((destructor)) static void finish_observer(void)
{
    int saved=errno;pthread_once(&once,initialize);
    if (!active || owner_pid!=getpid()) {errno=saved;return;}
    pthread_mutex_lock(&lock);publishing=true;unsigned busy=atomic_load(&inflight),open_formats=0,open_codecs=0;
    if (!busy) {
        for (unsigned i=0;i<format_slots;++i) if (formats[i].open) ++open_formats;
        for (unsigned i=0;i<codec_slots;++i) if (codecs[i].open) ++open_codecs;
    }
    pthread_mutex_unlock(&lock);
    unsigned total=atomic_load_explicit(&attempted,memory_order_acquire),n=total<PACKET_ADMISSION_CAPACITY?total:PACKET_ADMISSION_CAPACITY;
    char path[PATH_MAX];int len=snprintf(path,sizeof(path),"%s-%ld.jsonl",prefix,(long)owner_pid);
    if (len<=0 || (size_t)len>=sizeof(path)) {errno=saved;return;}
    int fd=open(path,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    if (fd<0) {errno=saved;return;}
    bool ok=dprintf(fd,"{\"type\":\"header\",\"schema\":\"crystalhd-packet-admission-v1\",\"pid\":%ld,\"fork_policy\":\"PARENT_PID_ONLY_CHILD_FORWARD_ONLY\",\"observation_scope\":\"OUTERMOST_PUBLIC_CALLS_PARENT_PID\",\"capacity\":%d,\"lifecycle_capacity\":%d,\"max_streams\":%d,\"format_provider\":",(long)owner_pid,PACKET_ADMISSION_CAPACITY,PACKET_ADMISSION_LIFETIMES,MAX_STREAMS)>=0 && string(fd,format_path) && dprintf(fd,",\"codec_provider\":")>=0 && string(fd,codec_path) && dprintf(fd,",\"util_provider\":")>=0 && string(fd,util_path) && dprintf(fd,",\"format_version\":%u,\"codec_version\":%u,\"util_version\":%u,\"provider_ok\":%s,\"h264_codec_id\":%d,\"video_media_type\":%d,\"vaapi_format\":%d,\"discard_none\":%d,\"discard_default\":%d,\"discard_nonref\":%d}\n",format_version,codec_version,util_version,provider_ok?"true":"false",AV_CODEC_ID_H264,AVMEDIA_TYPE_VIDEO,AV_PIX_FMT_VAAPI,AVDISCARD_NONE,AVDISCARD_DEFAULT,AVDISCARD_NONREF)>=0;
    unsigned published=0,unpublished=0;
    for (unsigned i=0;i<n;++i) {
        if (!atomic_load_explicit(&events[i].ready,memory_order_acquire)) {++unpublished;continue;}
        if (ok) {ok=print_event(fd,i,&events[i]);if (ok) ++published;}
    }
    unsigned lost=atomic_load(&dropped),life=atomic_load(&lifecycle_failures),truncated=atomic_load(&string_truncations);
    bool clock=atomic_load(&clock_failure),forked=atomic_load(&fork_seen);
    bool complete=ok && provider_ok && !lost && !life && !truncated && !clock && !busy && !unpublished && !open_formats && !open_codecs;
    if (ok) (void)dprintf(fd,"{\"type\":\"footer\",\"events\":%u,\"attempted_events\":%u,\"dropped\":%u,\"lifecycle_failures\":%u,\"string_truncations\":%u,\"clock_failure\":%s,\"fork_seen\":%s,\"inflight\":%u,\"unpublished\":%u,\"open_formats\":%u,\"open_codecs\":%u,\"complete\":%s}\n",published,total,lost,life,truncated,clock?"true":"false",forked?"true":"false",busy,unpublished,open_formats,open_codecs,complete?"true":"false");
    close(fd);errno=saved;
}
