/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "v4l2.h"
#include <gst/video/video.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <glob.h>
#include <unistd.h>
#include <poll.h>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <vector>
#include <algorithm>
#include <stdexcept>
extern "C" {
#include <libavcodec/bsf.h>
#include <libavutil/mem.h>
}

namespace crystalhd_kodi {
namespace {
void need(bool value,const char* text) { if(!value)throw std::runtime_error(std::string(text)+": "+strerror(errno)); }
int call(int fd,unsigned long request,void* arg) { int rc;do{rc=ioctl(fd,request,arg);}while(rc<0&&errno==EINTR);return rc; }
void checked(int fd,unsigned long request,void* arg,const char* text) {need(call(fd,request,arg)==0,text);}
std::string node() {
  glob_t paths{};glob("/sys/class/video4linux/video*/name",0,nullptr,&paths);std::string out;
  for(size_t i=0;i<paths.gl_pathc;++i) {
    std::ifstream file(paths.gl_pathv[i]);std::string name;std::getline(file,name);
    if(name.find("crystalhd")==std::string::npos&&name.find("CrystalHD")==std::string::npos)continue;
    std::string p=paths.gl_pathv[i];p.resize(p.rfind('/'));out="/dev/"+p.substr(p.rfind('/')+1);break;
  }
  globfree(&paths);need(!out.empty(),"actual CrystalHD V4L2 node unavailable");return out;
}
}
struct V4l2Decoder::Session {
  struct Mapping { uint8_t* data;size_t size;bool queued=false; };
  struct Metadata { double pts,dts,duration; };
  std::mutex lock;
  int fd=-1;unsigned width,height,fps_n,fps_d,stride=0;
  bool output_on=false,capture_on=false,closing=false,drain=false,stop=false,last=false,eof_reported=false;
  size_t accepted=0,completed=0,returned=0,borrowed=0;
  std::vector<Mapping> output,capture;
  std::map<int64_t,Metadata> pending;
  AVBSFContext* filter=nullptr;
  GstCaps* caps=nullptr;
  std::string failure;
  Session(unsigned w,unsigned h,unsigned n,unsigned d):width(w),height(h),fps_n(n),fps_d(d) {}
  void allocate(v4l2_buf_type type,unsigned count,std::vector<Mapping>& maps) {
    v4l2_requestbuffers r{};r.type=type;r.memory=V4L2_MEMORY_MMAP;r.count=count;
    checked(fd,VIDIOC_REQBUFS,&r,"V4L2 REQBUFS");need(r.count&&r.count<=32,"V4L2 bounded buffer count");
    maps.reserve(r.count);
    for(unsigned i=0;i<r.count;++i) {
      v4l2_buffer b{};b.type=type;b.memory=V4L2_MEMORY_MMAP;b.index=i;
      checked(fd,VIDIOC_QUERYBUF,&b,"V4L2 QUERYBUF");
      auto* p=static_cast<uint8_t*>(mmap(nullptr,b.length,PROT_READ|PROT_WRITE,MAP_SHARED,fd,b.m.offset));
      need(p!=MAP_FAILED,"V4L2 MMAP");maps.push_back({p,b.length,false});
    }
  }
  void queue_capture(unsigned index) {
    v4l2_buffer b{};b.type=V4L2_BUF_TYPE_VIDEO_CAPTURE;b.memory=V4L2_MEMORY_MMAP;b.index=index;
    checked(fd,VIDIOC_QBUF,&b,"V4L2 capture QBUF");capture.at(index).queued=true;
  }
  void service() {
    if(!failure.empty())throw std::runtime_error(failure);
    v4l2_buffer b{};b.type=V4L2_BUF_TYPE_VIDEO_OUTPUT;b.memory=V4L2_MEMORY_MMAP;
    while(output_on&&call(fd,VIDIOC_DQBUF,&b)==0) {
      need(b.index<output.size()&&output[b.index].queued&&!(b.flags&V4L2_BUF_FLAG_ERROR),"V4L2 OUTPUT completion");
      output[b.index].queued=false;++completed;b={};b.type=V4L2_BUF_TYPE_VIDEO_OUTPUT;b.memory=V4L2_MEMORY_MMAP;
    }
    need(!output_on||errno==EAGAIN||errno==EPIPE,"V4L2 OUTPUT dequeue");
    v4l2_event event{};
    while(call(fd,VIDIOC_DQEVENT,&event)==0) {
      need(event.type==V4L2_EVENT_SOURCE_CHANGE&&!capture_on,"V4L2 unexpected stream change");
      v4l2_format f{};f.type=V4L2_BUF_TYPE_VIDEO_CAPTURE;checked(fd,VIDIOC_G_FMT,&f,"V4L2 capture G_FMT");
      need(f.fmt.pix.pixelformat==V4L2_PIX_FMT_YUYV&&f.fmt.pix.width==width&&f.fmt.pix.height==height&&
        f.fmt.pix.field==V4L2_FIELD_NONE&&f.fmt.pix.bytesperline>=width*2,"V4L2 actual YUYV geometry");
      stride=f.fmt.pix.bytesperline;allocate(V4L2_BUF_TYPE_VIDEO_CAPTURE,12,capture);
      for(unsigned i=0;i<capture.size();++i)queue_capture(i);
      v4l2_buf_type type=V4L2_BUF_TYPE_VIDEO_CAPTURE;checked(fd,VIDIOC_STREAMON,&type,"V4L2 capture STREAMON");capture_on=true;
      g_message("CrystalHD-V4L2 capture=%ux%u stride=%u buffers=%zu format=YUYV",width,height,stride,capture.size());
    }
    need(errno==EAGAIN||errno==ENOENT,"V4L2 source event dequeue");
    if(drain&&!stop&&accepted&&output_on&&capture_on&&completed==accepted) {
      v4l2_decoder_cmd cmd{};cmd.cmd=V4L2_DEC_CMD_STOP;checked(fd,VIDIOC_DECODER_CMD,&cmd,"V4L2 terminal EOF STOP");stop=true;
    }
  }
  void close_streams() {
    std::lock_guard<std::mutex> guard(lock);closing=true;
    if(fd>=0) {
      auto stop_queue=[&](v4l2_buf_type type,bool& active) {
        if(active) {if(call(fd,VIDIOC_STREAMOFF,&type)<0)failure="V4L2 STREAMOFF failed";else active=false;}
      };
      stop_queue(V4L2_BUF_TYPE_VIDEO_OUTPUT,output_on);
      stop_queue(V4L2_BUF_TYPE_VIDEO_CAPTURE,capture_on);
    }
  }
  ~Session() {
    close_streams();
    for(auto* maps:{&capture,&output})for(auto& b:*maps)munmap(b.data,b.size);
    if(fd>=0)::close(fd);
    if(filter)av_bsf_free(&filter);
    if(caps)gst_caps_unref(caps);
  }
};

bool V4l2Decoder::Open(unsigned width,unsigned height,unsigned fps_n,unsigned fps_d,const uint8_t* extra,size_t extra_size) {
  Close();error.clear();
  try {
    need(packed_layout(width,height,0,size_t(width)*2,size_t(width)*height*2)&&fps_n&&fps_d&&fps_n<=240000&&fps_d<=100000&&fps_n<=240*fps_d&&extra&&extra_size>=7&&extra_size<=packet_limit&&extra[0]==1,"V4L2 AVC parameters");
    need(gst_init_check(nullptr,nullptr,nullptr),"GStreamer metadata initialization");
    auto s=std::make_shared<Session>(width,height,fps_n,fps_d);
    const auto* bsf=av_bsf_get_by_name("h264_mp4toannexb");need(bsf&&av_bsf_alloc(bsf,&s->filter)>=0,"V4L2 AnnexB filter");
    auto* p=s->filter->par_in;p->codec_type=AVMEDIA_TYPE_VIDEO;p->codec_id=AV_CODEC_ID_H264;p->width=width;p->height=height;
    p->extradata=static_cast<uint8_t*>(av_mallocz(extra_size+AV_INPUT_BUFFER_PADDING_SIZE));need(p->extradata,"V4L2 AVC extradata");
    memcpy(p->extradata,extra,extra_size);p->extradata_size=extra_size;s->filter->time_base_in={1,1000000};need(av_bsf_init(s->filter)>=0,"V4L2 AnnexB initialization");
    const std::string path=node();s->fd=open(path.c_str(),O_RDWR|O_NONBLOCK|O_CLOEXEC);need(s->fd>=0,"open actual V4L2 decoder");
    v4l2_capability cap{};checked(s->fd,VIDIOC_QUERYCAP,&cap,"V4L2 QUERYCAP");
    unsigned bits=cap.capabilities&V4L2_CAP_DEVICE_CAPS?cap.device_caps:cap.capabilities;
    need((bits&(V4L2_CAP_VIDEO_M2M|V4L2_CAP_STREAMING))==(V4L2_CAP_VIDEO_M2M|V4L2_CAP_STREAMING),"V4L2 M2M streaming required");
    v4l2_event_subscription sub{};sub.type=V4L2_EVENT_SOURCE_CHANGE;checked(s->fd,VIDIOC_SUBSCRIBE_EVENT,&sub,"V4L2 subscribe");
    v4l2_format fmt{};fmt.type=V4L2_BUF_TYPE_VIDEO_OUTPUT;fmt.fmt.pix.width=width;fmt.fmt.pix.height=height;fmt.fmt.pix.pixelformat=V4L2_PIX_FMT_H264;fmt.fmt.pix.field=V4L2_FIELD_NONE;fmt.fmt.pix.sizeimage=packet_limit;
    checked(s->fd,VIDIOC_S_FMT,&fmt,"V4L2 output S_FMT");s->allocate(V4L2_BUF_TYPE_VIDEO_OUTPUT,8,s->output);
    s->caps=gst_caps_new_simple("video/x-raw","format",G_TYPE_STRING,"YUY2","width",G_TYPE_INT,int(width),"height",G_TYPE_INT,int(height),"framerate",GST_TYPE_FRACTION,int(fps_n),int(fps_d),"interlace-mode",G_TYPE_STRING,"progressive",nullptr);
    g_message("CrystalHD-V4L2 open node=%s coded=%ux%u; no internal decoder fallback",path.c_str(),width,height);
    session=std::move(s);return true;
  } catch(const std::exception& e) {error=e.what();return false;}
}
V4l2Decoder::Submit V4l2Decoder::Push(const uint8_t* data,size_t size,double pts,double dts,double duration) {
  try {
    need(bool(session),"V4L2 inactive input");auto& s=*session;std::lock_guard<std::mutex> guard(s.lock);
    need(!s.closing&&!s.drain&&data&&size&&size<=packet_limit&&std::isfinite(pts)&&pts>=0&&pts<=86400000000.0&&std::isfinite(dts)&&dts>=-10000000&&dts<=86400000000.0&&std::isfinite(duration)&&duration>=0&&duration<=10000000,"V4L2 packet/time bounds");
    s.service();unsigned index=0;while(index<s.output.size()&&s.output[index].queued)++index;
    if(index==s.output.size())return Submit::Retry;
    const int64_t stamp=std::llround(pts);need(!s.pending.count(stamp),"V4L2 ambiguous source timestamp");
    AVPacket* packet=av_packet_alloc();need(packet,"V4L2 packet allocation");
    struct Cleanup {AVPacket*& p;~Cleanup(){av_packet_free(&p);}} cleanup{packet};
    need(av_new_packet(packet,size)>=0,"V4L2 packet backing");memcpy(packet->data,data,size);
    packet->pts=stamp;packet->dts=std::llround(dts);packet->duration=std::llround(duration);
    need(av_bsf_send_packet(s.filter,packet)>=0&&av_bsf_receive_packet(s.filter,packet)>=0,"V4L2 AnnexB packet");
    need(packet->size>0&&size_t(packet->size)<=s.output[index].size&&packet->pts==stamp,"V4L2 converted AU extent/PTS");
    memcpy(s.output[index].data,packet->data,packet->size);
    v4l2_buffer b{};b.type=V4L2_BUF_TYPE_VIDEO_OUTPUT;b.memory=V4L2_MEMORY_MMAP;b.index=index;b.bytesused=packet->size;b.timestamp.tv_sec=stamp/1000000;b.timestamp.tv_usec=stamp%1000000;b.flags=V4L2_BUF_FLAG_TIMESTAMP_COPY;
    checked(s.fd,VIDIOC_QBUF,&b,"V4L2 coded QBUF");s.output[index].queued=true;++s.accepted;s.pending.emplace(stamp,Session::Metadata{pts,dts,duration});
    if(!s.output_on) {v4l2_buf_type type=V4L2_BUF_TYPE_VIDEO_OUTPUT;checked(s.fd,VIDIOC_STREAMON,&type,"V4L2 output STREAMON");s.output_on=true;}
    return Submit::Accepted;
  } catch(const std::exception& e) {error=e.what();return Submit::Error;}
}
GstSample* V4l2Decoder::Next(bool final_input,Result& result) {
  result=Result::Error;
  try {
    need(bool(session),"V4L2 inactive output");auto s=session;std::lock_guard<std::mutex> guard(s->lock);
    need(!s->closing,"V4L2 closed output");s->drain|=final_input;s->service();
    if(s->drain&&!s->accepted) {result=Result::Eof;return nullptr;}
    if(s->last) {
      need(s->pending.empty()&&s->completed==s->accepted&&s->returned==s->accepted,"V4L2 incomplete terminal census");
      if(!s->borrowed&&!s->eof_reported) {
        g_message("CrystalHD-V4L2 terminal accepted=%zu completed=%zu pictures=%zu LAST=1 EPIPE=1 borrowed=0",s->accepted,s->completed,s->returned);
        s->eof_reported=true;
      }
      result=s->borrowed?Result::Wait:Result::Eof;return nullptr;
    }
    v4l2_buffer b{};b.type=V4L2_BUF_TYPE_VIDEO_CAPTURE;b.memory=V4L2_MEMORY_MMAP;
    if(s->capture_on&&call(s->fd,VIDIOC_DQBUF,&b)==0) {
      need(b.index<s->capture.size()&&s->capture[b.index].queued&&!(b.flags&V4L2_BUF_FLAG_ERROR),"V4L2 capture ownership/error");s->capture[b.index].queued=false;
      const bool last=b.flags&V4L2_BUF_FLAG_LAST;
      if(last) {
        need(s->stop,"V4L2 premature LAST");v4l2_buffer after{};after.type=V4L2_BUF_TYPE_VIDEO_CAPTURE;after.memory=V4L2_MEMORY_MMAP;
        need(call(s->fd,VIDIOC_DQBUF,&after)<0&&errno==EPIPE,"V4L2 terminal EPIPE");s->last=true;
      }
      if(b.bytesused) {
        const auto& map=s->capture[b.index];const size_t active=size_t(s->height-1)*s->stride+s->width*2;
        need(b.bytesused<=map.size&&b.bytesused>=active&&b.field==V4L2_FIELD_NONE,"V4L2 capture layout");
        const int64_t stamp=int64_t(b.timestamp.tv_sec)*1000000+b.timestamp.tv_usec;auto it=s->pending.find(stamp);need(it!=s->pending.end(),"V4L2 unknown returned source PTS");
        uint64_t pts=0,dts=0;need(to_clock(it->second.pts,true,pts)&&to_clock(it->second.dts,false,dts),"V4L2 returned clock range");
        struct Lease {std::shared_ptr<Session> owner;unsigned index;bool last;};
        auto* lease=new Lease{s,b.index,last};++s->borrowed;
        GstBuffer* buffer=gst_buffer_new_wrapped_full(GST_MEMORY_FLAG_READONLY,map.data,map.size,0,active,lease,[](gpointer p){
          std::unique_ptr<Lease> l(static_cast<Lease*>(p));std::lock_guard<std::mutex> hold(l->owner->lock);
          --l->owner->borrowed;
          if(!l->owner->closing&&!l->last)try{l->owner->queue_capture(l->index);}catch(const std::exception& e){l->owner->failure=e.what();}
        });
        GST_BUFFER_PTS(buffer)=pts;GST_BUFFER_DTS(buffer)=dts;GST_BUFFER_DURATION(buffer)=uint64_t(it->second.duration*1000);
        gsize offsets[GST_VIDEO_MAX_PLANES]{};gint strides[GST_VIDEO_MAX_PLANES]{};strides[0]=s->stride;
        gst_buffer_add_video_meta_full(buffer,GST_VIDEO_FRAME_FLAG_NONE,GST_VIDEO_FORMAT_YUY2,s->width,s->height,1,offsets,strides);
        s->pending.erase(it);++s->returned;result=Result::Picture;
        GstSample* sample=gst_sample_new(buffer,s->caps,nullptr,nullptr);gst_buffer_unref(buffer);return sample;
      }
      need(last,"V4L2 empty nonterminal output");
      need(s->pending.empty()&&s->returned==s->accepted&&s->completed==s->accepted&&!s->borrowed,"V4L2 missing original at EOF");
      g_message("CrystalHD-V4L2 terminal accepted=%zu completed=%zu pictures=%zu LAST=1 EPIPE=1 borrowed=0",s->accepted,s->completed,s->returned);
      s->eof_reported=true;result=Result::Eof;return nullptr;
    }
    need(!s->capture_on||errno==EAGAIN,"V4L2 capture dequeue");
    pollfd p{s->fd,POLLIN|POLLPRI,0};int rc;do{rc=poll(&p,1,1);}while(rc<0&&errno==EINTR);need(rc>=0,"V4L2 output poll");
    result=(s->drain||std::all_of(s->output.begin(),s->output.end(),[](const auto& m){return m.queued;}))?Result::Wait:Result::NeedInput;return nullptr;
  } catch(const std::exception& e) {error=e.what();return nullptr;}
}
void V4l2Decoder::Close() {if(session){session->close_streams();if(!session->failure.empty())error=session->failure;session.reset();}}
}
