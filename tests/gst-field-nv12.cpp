// SPDX-License-Identifier: GPL-2.0-or-later
// Explicit owned-window GPU integration. Expectations come from frozen source
// arrays/PTS, not returned pixels. No decoder or A/V/scanout acceptance.
#include <gst/video/video.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
static __eglMustCastToProperFunctionPointerType observe_proc(const char *);
static void observe_field(GstBuffer *,const GstVideoInfo &,unsigned,bool,unsigned,unsigned,EGLImageKHR);
#define eglGetProcAddress observe_proc
#define CRYSTALHD_FIELD_OBSERVER observe_field
#include "../filters/gst/field-output/gstcrystalhdfield.cpp"
#undef CRYSTALHD_FIELD_OBSERVER
#undef eglGetProcAddress
#include <gst/app/gstappsrc.h>
#include <map>
#include <chrono>

struct Source {
    GstVideoFormat format;
    bool interlaced,tff,mpeg2,sd;
    unsigned generation;
    std::vector<uint8_t> y,uv,packed;
    std::vector<unsigned> order;
    uint64_t points=0;
    unsigned observations=0;
};
static std::mutex observation_lock;
static std::map<GstClockTime,Source> sources;
static std::map<EGLImageKHR,std::map<EGLint,EGLint>> imports;
static PFNEGLCREATEIMAGEKHRPROC real_create=nullptr;
static std::atomic<unsigned> freed_memories{0};
static bool no_readback=false;
static constexpr unsigned width=640,height=360;

static EGLImageKHR observe_create(EGLDisplay d,EGLContext c,EGLenum target,EGLClientBuffer buffer,const EGLint *attrs) {
    auto image=real_create(d,c,target,buffer,attrs);
    if(image!=EGL_NO_IMAGE_KHR) {
        std::map<EGLint,EGLint> values;
        for(unsigned i=0;attrs&&i<32&&attrs[i]!=EGL_NONE;i+=2)values[attrs[i]]=attrs[i+1];
        std::lock_guard<std::mutex> lock(observation_lock);imports[image]=std::move(values);
    }
    return image;
}
static __eglMustCastToProperFunctionPointerType observe_proc(const char *name) {
    auto proc=eglGetProcAddress(name);
    if(!strcmp(name,"eglCreateImageKHR")) {
        real_create=reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(proc);
        return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(observe_create);
    }
    return proc;
}
static void observe_field(GstBuffer *buffer,const GstVideoInfo &info,unsigned parity,bool interlaced,
                          unsigned dw,unsigned dh,EGLImageKHR image) {
    std::lock_guard<std::mutex> lock(observation_lock);
    auto found=sources.find(GST_BUFFER_PTS(buffer));need(found!=sources.end(),"source-bound observer PTS");
    auto &source=found->second;
    need(source.format==GST_VIDEO_INFO_FORMAT(&info)&&source.interlaced==interlaced,"frozen source format/fields");
    need(dw==width&&dh==height,"owned fixed viewport");
    auto &attrs=imports.at(image);
    const unsigned factor=interlaced?2:1,base_pitch=attrs.at(EGL_DMA_BUF_PLANE0_PITCH_EXT)/factor;
    need(unsigned(attrs.at(EGL_DMA_BUF_PLANE0_OFFSET_EXT))==(interlaced?parity*base_pitch:0),"actual Y parity offset");
    if(source.format==GST_VIDEO_FORMAT_NV12) {
        need(attrs.at(EGL_LINUX_DRM_FOURCC_EXT)==DRM_FORMAT_NV12,"actual NV12 import");
        need(attrs.at(EGL_DMA_BUF_PLANE1_PITCH_EXT)==attrs.at(EGL_DMA_BUF_PLANE0_PITCH_EXT),"actual UV field pitch");
        need(unsigned(attrs.at(EGL_DMA_BUF_PLANE1_OFFSET_EXT))==base_pitch*(height+(interlaced?parity:0)),"actual UV parity offset");
        need(attrs.at(EGL_YUV_CHROMA_HORIZONTAL_SITING_HINT_EXT)==
            (source.mpeg2?EGL_YUV_CHROMA_SITING_0_EXT:EGL_YUV_CHROMA_SITING_0_5_EXT),"actual source-bound chroma hint");
    }
    ++source.observations;
    if(source.order.empty()||source.order.back()!=parity)source.order.push_back(parity);
    if(no_readback)return;
    std::vector<uint8_t> pixels(size_t(dw)*dh*4);glReadPixels(0,0,dw,dh,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
    need(glGetError()==GL_NO_ERROR,"worker RGB observer");
    uint64_t points=0;unsigned maximum=0;size_t over=0;
    for(unsigned y=0;y<dh;y++)for(unsigned x=0;x<dw;x++) {
        const unsigned sx=std::min<unsigned>(uint64_t(2*x+1)*width/(2*dw),width-1);
        const int64_t n=int64_t(2*y+1)*height+int64_t(1-2*int(parity))*dh;
        const unsigned fy=unsigned(std::clamp<int64_t>(n<0?-1:n/(4*dh),0,height/2-1));
        const unsigned sy=interlaced?2*fy+parity:std::min<unsigned>(uint64_t(2*y+1)*height/(2*dh),height-1);
        unsigned yy,u,v;
        if(source.format==GST_VIDEO_FORMAT_NV12) {
            const unsigned row=interlaced?2*(fy/2)+parity:sy/2,pair=sx/2;
            const size_t q=size_t(row)*width+2*pair;u=source.uv[q];v=source.uv[q+1];yy=source.y[size_t(sy)*width+sx];
            bool flat=true;
            // Prequalified 3x3 chroma neighbourhood in the source's own
            // field lattice. Nonflat chroma interpolation edges are separate.
            for(int dy=-1;dy<=1;dy++)for(int dx=-1;dx<=1;dx++) {
                const int rows=interlaced?height/4:height/2;
                unsigned r=std::clamp(int(interlaced?row/2:row)+dy,0,rows-1);
                if(interlaced)r=2*r+parity;
                unsigned p=std::clamp(int(pair)+dx,0,int(width/2-1));
                const size_t at=size_t(r)*width+2*p;
                if(source.uv[at]!=u||source.uv[at+1]!=v)flat=false;
            }
            if(!flat)continue;
        }else {
            const size_t q=(size_t(sy)*width+(sx/2)*2)*2;
            yy=source.packed[q+(sx&1)*2];u=source.packed[q+1];v=source.packed[q+3];
        }
        const double l=(int(yy)-16)*255.0/219,cb=(int(u)-128)*255.0/224,cr=(int(v)-128)*255.0/224;
        const double expected[]={l+(source.sd?1.402:1.5748)*cr,
            l-(source.sd?.344136:.187324)*cb-(source.sd?.714136:.468124)*cr,
            l+(source.sd?1.772:1.8556)*cb};
        const size_t at=(size_t(dh-1-y)*dw+x)*4;
        for(unsigned c=0;c<3;c++) {
            unsigned error=std::abs(int(pixels[at+c])-std::clamp(int(std::lround(expected[c])),0,255));
            maximum=std::max(maximum,error);over+=error>2;
        }
        need(pixels[at+3]==255,"opaque eligible source pixel");++points;
    }
    printf("{\"type\":\"nv12-worker-pixels\",\"source_pts\":%llu,\"format\":\"%s\",\"parity\":%u,\"points\":%llu,\"max_error\":%u,\"over2\":%zu}\n",
        static_cast<unsigned long long>(GST_BUFFER_PTS(buffer)),gst_video_format_to_string(source.format),parity,
        static_cast<unsigned long long>(points),maximum,over);
    need(points>10000&&!over,"source-only spatial RGB oracle tolerance2");
    source.points+=points;
}

static guint64 stat(GstElement *sink,const char *key) {
    GstStructure *value=nullptr;g_object_get(sink,"field-stats",&value,nullptr);
    guint64 result=0;need(value&&gst_structure_get_uint64(value,key,&result),"field counter");
    gst_structure_free(value);return result;
}
static GstMemory *wrapped(const std::shared_ptr<std::vector<uint8_t>> &bytes,size_t start,size_t size) {
    auto *owned=new std::shared_ptr<std::vector<uint8_t>>(bytes);
    return gst_memory_new_wrapped(GST_MEMORY_FLAG_READONLY,bytes->data()+start,size,0,size,owned,
        [](gpointer p){delete static_cast<std::shared_ptr<std::vector<uint8_t>> *>(p);++freed_memories;});
}
static Source pattern(unsigned id) {
    Source s{};s.format=id%7==0||id%7==4?GST_VIDEO_FORMAT_YUY2:GST_VIDEO_FORMAT_NV12;
    s.interlaced=id%7!=6;s.tff=(id%7!=2)^(id>=7);s.mpeg2=id%7==2||id%7==5;s.sd=id%7==1||id%7==4||id%7==5;s.generation=id;
    s.y.resize(width*height);s.uv.resize(width*height/2);s.packed.resize(width*height*2);
    for(unsigned y=0;y<height;y++)for(unsigned x=0;x<width;x++)
        s.y[size_t(y)*width+x]=32+(x*3+(y/2)*7+id*19+(y&1)*29)%180;
    for(unsigned row=0;row<height/2;row++)for(unsigned pair=0;pair<width/2;pair++) {
        unsigned p=s.interlaced?row&1:0,r=s.interlaced?row/2:row;
        s.uv[size_t(row)*width+2*pair]=48+((pair/16+r/8+id+p*2)%7)*24;
        s.uv[size_t(row)*width+2*pair+1]=64+((pair/16*3+r/8*2+id*2+p*3)%6)*24;
    }
    for(unsigned y=0;y<height;y++)for(unsigned x=0;x<width;x+=2) {
        const size_t q=(size_t(y)*width+x)*2;
        s.packed[q]=s.y[size_t(y)*width+x];s.packed[q+2]=s.y[size_t(y)*width+x+1];
        s.packed[q+1]=(y&1)?176:80;s.packed[q+3]=(y&1)?80:176;
    }
    return s;
}
static GstBuffer *payload(const Source &source,unsigned id,unsigned &memories) {
    auto *buffer=gst_buffer_new();gsize offsets[GST_VIDEO_MAX_PLANES]{};gint strides[GST_VIDEO_MAX_PLANES]{};
    if(source.format==GST_VIDEO_FORMAT_YUY2) {
        auto bytes=std::make_shared<std::vector<uint8_t>>(source.packed);
        gst_buffer_append_memory(buffer,wrapped(bytes,0,bytes->size()));++memories;
        strides[0]=width*2;
    }else {
        const bool adjacent=id%2==0;
        const unsigned ys=width+32,us=width+64,yp=adjacent?0:17,up=adjacent?0:29;
        const size_t ybytes=yp+size_t(ys)*height,ubytes=up+size_t(us)*(height/2);
        auto bytes=std::make_shared<std::vector<uint8_t>>(ybytes+ubytes,0xa5);
        for(unsigned y=0;y<height;y++)memcpy(bytes->data()+yp+size_t(y)*ys,source.y.data()+size_t(y)*width,width);
        for(unsigned y=0;y<height/2;y++)memcpy(bytes->data()+ybytes+up+size_t(y)*us,source.uv.data()+size_t(y)*width,width);
        gst_buffer_append_memory(buffer,wrapped(bytes,0,ybytes));gst_buffer_append_memory(buffer,wrapped(bytes,ybytes,ubytes));memories+=2;
        offsets[0]=yp;offsets[1]=ybytes+up;strides[0]=ys;strides[1]=us;
    }
    need(gst_buffer_add_video_meta_full(buffer,GST_VIDEO_FRAME_FLAG_NONE,source.format,width,height,
        source.format==GST_VIDEO_FORMAT_NV12?2:1,offsets,strides),"source video meta");
    GST_BUFFER_PTS(buffer)=guint64(id)*100*GST_MSECOND;GST_BUFFER_DURATION(buffer)=40*GST_MSECOND;
    if(source.interlaced)GST_BUFFER_FLAG_SET(buffer,GST_VIDEO_BUFFER_FLAG_INTERLACED);
    if(source.tff)GST_BUFFER_FLAG_SET(buffer,GST_VIDEO_BUFFER_FLAG_TFF);
    return buffer;
}
int main(int argc,char **argv) {
 try {
    if(argc==2&&!strcmp(argv[1],"--no-readback")){no_readback=true;argc=1;}
    need(argc==1,"usage: field-nv12-test [--no-readback]");
    need(temperature()<75000,"thermal admission");gst_init(&argc,&argv);need(plugin_init(nullptr),"own field registration");
    int screen_number=0;auto *connection=xcb_connect(nullptr,&screen_number);need(!xcb_connection_has_error(connection),"owned parent connection");
    auto screens=xcb_setup_roots_iterator(xcb_get_setup(connection));while(screen_number--&&screens.rem)xcb_screen_next(&screens);
    need(screens.rem,"owned parent screen");auto window=xcb_generate_id(connection);
    uint32_t values[]={0,1};xcb_create_window(connection,XCB_COPY_FROM_PARENT,window,screens.data->root,0,0,width,height,0,
        XCB_WINDOW_CLASS_INPUT_OUTPUT,screens.data->root_visual,XCB_CW_BACK_PIXEL|XCB_CW_OVERRIDE_REDIRECT,values);
    xcb_map_window(connection,window);xcb_flush(connection);
    for(unsigned id=0;id<14;id++)sources.emplace(guint64(id)*100*GST_MSECOND,pattern(id));
    auto *pipeline=gst_pipeline_new(nullptr),*input=gst_element_factory_make("appsrc",nullptr),*sink=gst_element_factory_make("crystalhdfieldsink",nullptr);
    need(pipeline&&input&&sink,"NV12 factories");gst_bin_add_many(GST_BIN(pipeline),input,sink,nullptr);need(gst_element_link(input,sink),"NV12 link");
    const auto cleanup=[](GstElement *value){gst_element_set_state(value,GST_STATE_NULL);gst_object_unref(value);};
    std::unique_ptr<GstElement,decltype(cleanup)> owned(pipeline,cleanup);
    g_object_set(input,"format",GST_FORMAT_TIME,nullptr);g_object_set(sink,"sync",FALSE,nullptr);
    gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(sink),window);auto *bus=gst_element_get_bus(pipeline);unsigned memories=0;
    for(unsigned run=0;run<2;run++) {
      need(gst_element_set_state(pipeline,GST_STATE_PLAYING)!=GST_STATE_CHANGE_FAILURE,"NV12 PLAYING");
      guint64 expected=stat(sink,"fields");
      for(unsigned n=0;n<7;n++) {
        const unsigned id=run*7+n;const auto &source=sources.at(guint64(id)*100*GST_MSECOND);
        auto *caps=gst_caps_new_simple("video/x-raw","format",G_TYPE_STRING,gst_video_format_to_string(source.format),
            "width",G_TYPE_INT,width,"height",G_TYPE_INT,height,"framerate",GST_TYPE_FRACTION,25,1,
            "interlace-mode",G_TYPE_STRING,source.interlaced?"interleaved":"progressive",
            "colorimetry",G_TYPE_STRING,source.sd?"bt601":"bt709",nullptr);
        if(source.format==GST_VIDEO_FORMAT_NV12)gst_caps_set_simple(caps,"chroma-site",G_TYPE_STRING,source.mpeg2?"mpeg2":"jpeg",nullptr);
        gst_app_src_set_caps(GST_APP_SRC(input),caps);gst_caps_unref(caps);
        need(gst_app_src_push_buffer(GST_APP_SRC(input),payload(source,id,memories))==GST_FLOW_OK,"NV12 source submit");
        expected+=source.interlaced?2:1;
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(stat(sink,"fields")<expected&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(2));
        need(stat(sink,"fields")==expected&&stat(sink,"failures")==0,"NV12 complete source fields");
      }
      need(gst_app_src_end_of_stream(GST_APP_SRC(input))==GST_FLOW_OK,"NV12 final EOS");
      auto *message=gst_bus_timed_pop_filtered(bus,5*GST_SECOND,GstMessageType(GST_MESSAGE_EOS|GST_MESSAGE_ERROR));
      need(message&&GST_MESSAGE_TYPE(message)==GST_MESSAGE_EOS,"NV12 natural EOS");gst_message_unref(message);
      need(gst_element_set_state(pipeline,GST_STATE_NULL)!=GST_STATE_CHANGE_FAILURE,"NV12 NULL");
      need(!stat(sink,"images")&&!stat(sink,"allocations")&&!stat(sink,"retained-buffers"),"NV12 resource retirement");
    }
    gst_object_unref(bus);owned.reset();xcb_destroy_window(connection,window);xcb_disconnect(connection);
    need(freed_memories==memories,"all source memories returned");
    for(const auto &entry:sources) {
        const auto &s=entry.second;need(s.observations&&
            (no_readback||s.points>10000),"all source pictures observed");
        const std::vector<unsigned> expected=s.interlaced?std::vector<unsigned>{s.tff?0U:1U,s.tff?1U:0U}:std::vector<unsigned>{0};
        need(s.order==expected,"source-bound TFF/BFF/progressive order");
    }
    printf("PASS NV12 worker:14 source pictures, source-buffer PTS/parity, format/site changes,2 EOS/reopens, %u memories returned, readback=%s\n",memories,no_readback?"NOT_RUN":"sampled-RGB");
 }catch(const std::exception &e){fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
