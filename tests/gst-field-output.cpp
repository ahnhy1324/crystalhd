// SPDX-License-Identifier: GPL-2.0-or-later
// Explicit X11/GPU test. Synthetic input; never counts as card or A/V evidence.
// Include the experimental implementation to check its exact window path.
#include "../filters/gst/field-output/gstcrystalhdfield.cpp"
#include <gst/app/gstappsrc.h>
#include <chrono>

static std::vector<uint8_t> pattern(unsigned w,unsigned h,unsigned generation) {
    std::vector<uint8_t> source(size_t(w)*h*2);
    for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;x+=2) {
        const size_t q=(size_t(y)*w+x)*2;
        source[q]=16+(x*3+y*7+generation*29)%220;
        source[q+1]=16+(x+y*11+generation*31)%225;
        source[q+2]=16+(x*5+y*9+generation*23)%220;
        source[q+3]=16+(x*7+y*3+generation*37)%225;
    }
    return source;
}
static void pixels() {
    crystalhd_field::Window window(0);
    yuy2_gpu::GPU gpu("/dev/dri/renderD128",window.display,window.context);
    unsigned comparisons=0;
    for(auto dimensions:{std::array<unsigned,2>{1280,720},{1920,1080},{720,576}}) {
        const unsigned sw=dimensions[0],sh=dimensions[1];
        GbmAllocation storage(gpu,sw,sh);Renderer renderer(sw,sh,2,2);
        for(unsigned generation=0;generation<2;++generation) {
            const auto source=pattern(sw,sh,generation);storage.write(source.data(),sw*2,source.size());
            for(bool sd:{false,true})for(unsigned parity=0;parity<3;++parity) {
                const bool interlaced=parity<2;
                Image image(gpu,storage,interlaced,interlaced?parity:0,
                    sd?EGL_ITU_REC601_EXT:EGL_ITU_REC709_EXT,false);
                need(image.texture,"onscreen field import");
                renderer.draw_to(image,interlaced,parity<2?parity:0,0,0,0,window.width,window.height,true,!interlaced,false);
                std::vector<uint8_t> actual(size_t(window.width)*window.height*4);
                glReadPixels(0,0,window.width,window.height,GL_RGBA,GL_UNSIGNED_BYTE,actual.data());
                need(glGetError()==GL_NO_ERROR,"window RGB readback");
                unsigned maximum=0;size_t over=0;
                for(unsigned y=0;y<window.height;++y)for(unsigned x=0;x<window.width;++x) {
                    const unsigned sx=std::min<unsigned>(uint64_t(2*x+1)*sw/(2*window.width),sw-1);
                    unsigned sy=std::min<unsigned>(uint64_t(2*y+1)*sh/(2*window.height),sh-1);
                    if(interlaced) {
                        const int64_t numerator=int64_t(2*y+1)*sh+int64_t(1-2*int(parity))*window.height;
                        sy=2*unsigned(std::clamp<int64_t>(numerator<0?-1:numerator/(4*window.height),0,sh/2-1))+parity;
                    }
                    const size_t q=(size_t(sy)*sw+(sx/2)*2)*2;
                    const double yy=(source[q+(sx&1)*2]-16)*255.0/219;
                    const double u=(source[q+1]-128)*255.0/224,v=(source[q+3]-128)*255.0/224;
                    const double expected[]={yy+(sd?1.402:1.5748)*v,
                        yy-(sd?0.344136:0.187324)*u-(sd?0.714136:0.468124)*v,
                        yy+(sd?1.772:1.8556)*u};
                    const size_t p=(size_t(window.height-1-y)*window.width+x)*4;
                    for(unsigned c=0;c<3;++c) {
                        const unsigned error=std::abs(int(actual[p+c])-std::clamp(int(std::lround(expected[c])),0,255));
                        maximum=std::max(maximum,error);over+=error>2;
                    }
                }
                printf("{\"type\":\"window-pixels\",\"source\":[%u,%u],\"destination\":[%u,%u],\"generation\":%u,\"matrix\":%u,\"field\":%u,\"max_error\":%u,\"channels_over2\":%zu}\n",
                    sw,sh,window.width,window.height,generation,sd?601:709,parity,maximum,over);
                need(!over,"window orientation/color/field oracle fixed maximum2");++comparisons;
            }
        }
    }
    printf("{\"type\":\"window-pixel-summary\",\"comparisons\":%u,\"pass\":true}\n",comparisons);
}
static void retirement_pixels() {
    crystalhd_field::Window window(0);
    yuy2_gpu::GPU gpu("/dev/dri/renderD128",window.display,window.context);
    GbmAllocation storage(gpu,1280,720);
    Renderer top(1280,720,1920,1080),bottom(1280,720,1920,1080);
    for(unsigned generation=0;generation<4;++generation) {
        const auto source=pattern(1280,720,generation);
        storage.write(source.data(),1280*2,source.size());
        {
            Image upper(gpu,storage,true,0,EGL_ITU_REC709_EXT,false);
            Image lower(gpu,storage,true,1,EGL_ITU_REC709_EXT,false);
            need(upper.texture&&lower.texture,"queued field imports");
            // Queue both uses without a per-field finish or readback. Keep
            // their views and source generation alive through completion.
            top.draw_to(upper,true,0,top.picture.fbo,0,0,1920,1080,false,false,false);
            bottom.draw_to(lower,true,1,bottom.picture.fbo,0,0,1920,1080,false,false,false);
            glFinish();
            need(glGetError()==GL_NO_ERROR,"two-field retirement completion");
        }
        // Replace the producer only after retirement, then read both old
        // outputs. Their independent full RGB oracles must still match the
        // old generation, not the newly written or opposite field.
        const auto replacement=pattern(1280,720,generation+9);
        storage.write(replacement.data(),1280*2,replacement.size());
        glBindFramebuffer(GL_FRAMEBUFFER,top.picture.fbo);
        verify_scaled(top,source,0,"queued-top-retired-before-write");
        glBindFramebuffer(GL_FRAMEBUFFER,bottom.picture.fbo);
        verify_scaled(bottom,source,1,"queued-bottom-retired-before-write");
    }
    printf("{\"type\":\"retirement-pixels\",\"generations\":4,\"comparisons\":8,\"pass\":true}\n");
}
static void slot_retirement_pixels(bool fallback) {
    crystalhd_field::Window window(0);
    yuy2_gpu::GPU gpu("/dev/dri/renderD128",window.display,window.context);
    FieldSyncAPI sync(gpu.display);
    if(fallback)sync.supported=false;
    crystalhd_field::Counters stats;
    GstVideoInfo info{};gst_video_info_set_format(&info,GST_VIDEO_FORMAT_YUY2,1280,720);
    info.colorimetry.matrix=GST_VIDEO_COLOR_MATRIX_BT709;
    Renderer top0(1280,720,1920,1080),bottom0(1280,720,1920,1080);
    Renderer top1(1280,720,1920,1080),bottom1(1280,720,1920,1080);
    unsigned comparisons=0;
    for(unsigned round=0;round<2;++round) {
        std::unique_ptr<crystalhd_field::SourceSlot> slots[2];
        for(auto &slot:slots)slot=std::make_unique<crystalhd_field::SourceSlot>(gpu,1280,720,stats,sync);
        auto upload=[&](unsigned index,unsigned generation) {
            const auto source=pattern(1280,720,generation);
            auto *buffer=gst_buffer_new_allocate(nullptr,source.size(),nullptr);
            need(buffer,"slot test source");gst_buffer_fill(buffer,0,source.data(),source.size());
            slots[index]->prepare(gpu,info,buffer,generation,true);gst_buffer_unref(buffer);
            need(GST_MINI_OBJECT_REFCOUNT_VALUE(slots[index]->retained)==1,"slot owns the exact source reference");
            return source;
        };
        const auto first=upload(0,round*3);
        top0.draw_to(*slots[0]->views[0],true,0,top0.picture.fbo,0,0,1920,1080,false,false,false);
        bottom0.draw_to(*slots[0]->views[1],true,1,bottom0.picture.fbo,0,0,1920,1080,false,false,false);
        slots[0]->checkpoint();
        const auto second=upload(1,round*3+1);
        top1.draw_to(*slots[1]->views[0],true,0,top1.picture.fbo,0,0,1920,1080,false,false,false);
        bottom1.draw_to(*slots[1]->views[1],true,1,bottom1.picture.fbo,0,0,1920,1080,false,false,false);
        slots[1]->checkpoint();
        // Reuse the first BO while the other generation is still retained.
        // No readback or context-wide finish preceded this fenced reuse.
        upload(0,round*3+2);
        need(stats.retained==2&&stats.images==4,"independent live source slots");
        for(auto item:{std::make_pair(&top0,0U),std::make_pair(&bottom0,1U),
                       std::make_pair(&top1,0U),std::make_pair(&bottom1,1U)}) {
            glBindFramebuffer(GL_FRAMEBUFFER,item.first->picture.fbo);
            verify_scaled(*item.first,item.first==&top0||item.first==&bottom0?first:second,
                          item.second,"independent-slot-fenced-reuse");++comparisons;
        }
        for(auto &slot:slots){slot->retire();slot.reset();}
        need(!stats.retained&&!stats.images&&!stats.allocations&&!stats.fences_live,"all slots retired");
        need(stats.fences_created==stats.fences_destroyed,"all slot fences destroyed");
    }
    printf("{\"type\":\"slot-retirement-pixels\",\"fence_supported\":%s,\"forced_fallback\":%s,\"comparisons\":%u,\"resources_after_close\":0,\"pass\":true}\n",sync.supported?"true":"false",fallback?"true":"false",comparisons);
}
static guint64 counter(GstElement *sink,const char *key) {
    GstStructure *stats=nullptr;g_object_get(sink,"field-stats",&stats,nullptr);
    guint64 value=0;need(stats&&gst_structure_get_uint64(stats,key,&value),"counter");
    gst_structure_free(stats);return value;
}
static void check_error(GstMessage *message) {
    if(message&&GST_MESSAGE_TYPE(message)==GST_MESSAGE_ERROR) {
        GError *error=nullptr;gchar *debug=nullptr;gst_message_parse_error(message,&error,&debug);
        std::string text=error?error->message:"pipeline error";
        g_clear_error(&error);g_free(debug);gst_message_unref(message);throw std::runtime_error(text);
    }
}
static void pipeline(bool cancel) {
    auto *pipeline=gst_pipeline_new(nullptr);
    auto *input=gst_element_factory_make("appsrc",nullptr);
    auto *sink=gst_element_factory_make("crystalhdfieldsink",nullptr);
    need(pipeline&&input&&sink,"synthetic pipeline factories");
    auto *caps=gst_caps_from_string("video/x-raw,format=YUY2,width=640,height=360,framerate=25/1,interlace-mode=interleaved,colorimetry=bt709");
    g_object_set(input,"caps",caps,"format",GST_FORMAT_TIME,nullptr);gst_caps_unref(caps);
    gst_bin_add_many(GST_BIN(pipeline),input,sink,nullptr);need(gst_element_link(input,sink),"synthetic link");
    auto *bus=gst_element_get_bus(pipeline);
    const unsigned runs=cancel?1:3;
    for(unsigned run=0;run<runs;++run) {
        const guint64 before=counter(sink,"fields");
        need(gst_element_set_state(pipeline,GST_STATE_PLAYING)!=GST_STATE_CHANGE_FAILURE,"synthetic PLAYING");
        for(unsigned i=0;i<(cancel?1U:12U);++i) {
            const auto source=pattern(640,360,i+run*12);
            auto *buffer=gst_buffer_new_allocate(nullptr,source.size(),nullptr);
            gst_buffer_fill(buffer,0,source.data(),source.size());
            GST_BUFFER_PTS(buffer)=cancel?0:i*40*GST_MSECOND;
            GST_BUFFER_DURATION(buffer)=cancel?4*GST_SECOND:40*GST_MSECOND;
            GST_BUFFER_FLAG_SET(buffer,GST_VIDEO_BUFFER_FLAG_INTERLACED);
            if(run!=1)GST_BUFFER_FLAG_SET(buffer,GST_VIDEO_BUFFER_FLAG_TFF);
            need(gst_app_src_push_buffer(GST_APP_SRC(input),buffer)==GST_FLOW_OK,"synthetic push");
        }
        need(gst_app_src_end_of_stream(GST_APP_SRC(input))==GST_FLOW_OK,"synthetic EOS submit");
        if(cancel) {
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
            while(counter(sink,"fields")==before&&std::chrono::steady_clock::now()<deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            need(counter(sink,"fields")==before+1,"first field before cancellation");
        }else {
            auto *message=gst_bus_timed_pop_filtered(bus,5*GST_SECOND,GstMessageType(GST_MESSAGE_EOS|GST_MESSAGE_ERROR));
            need(message,"bounded synthetic EOS");check_error(message);gst_message_unref(message);
            need(counter(sink,"fields")==before+24,"all fields and no duplicate timed preroll");
        }
        const auto begin=std::chrono::steady_clock::now();
        need(gst_element_set_state(pipeline,GST_STATE_NULL)!=GST_STATE_CHANGE_FAILURE,"synthetic NULL");
        const double stop_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
        need(stop_ms<1000,"bounded cancellation/close");
        for(const char *key:{"allocations","images","retained-buffers","fences-live","failures"})need(!counter(sink,key),"clean worker retirement");
        need(counter(sink,"fences-created")==counter(sink,"fences-destroyed"),"all worker fences destroyed");
        printf("{\"type\":\"pipeline\",\"synthetic\":true,\"cancel\":%s,\"run\":%u,\"fields\":%llu,\"prerolls\":%llu,\"cancelled\":%llu,\"stop_ms\":%.3f,\"resources_after_null\":0,\"pass\":true}\n",
            cancel?"true":"false",run,static_cast<unsigned long long>(counter(sink,"fields")-before),
            static_cast<unsigned long long>(counter(sink,"prerolls")),static_cast<unsigned long long>(counter(sink,"cancelled")),stop_ms);
    }
    gst_object_unref(bus);gst_object_unref(pipeline);
}
static void card(const char *path,bool queued) {
    GError *error=nullptr;
    std::string chain="filesrc name=input ! h264parse ! crystalhddec name=decoder scale-width=1280 ! ";
    if(queued)chain+="queue max-size-buffers=2 max-size-bytes=0 max-size-time=0 ! ";
    chain+="crystalhdfieldsink name=output";
    auto *pipeline=gst_parse_launch(chain.c_str(),&error);
    need(!error&&pipeline,"card pipeline parse");
    auto *input=gst_bin_get_by_name(GST_BIN(pipeline),"input");
    auto *sink=gst_bin_get_by_name(GST_BIN(pipeline),"output");
    g_object_set(input,"location",path,nullptr);gst_object_unref(input);
    auto *bus=gst_element_get_bus(pipeline);
    for(unsigned run=0;run<2;++run) {
        const auto before=counter(sink,"fields");
        need(gst_element_set_state(pipeline,GST_STATE_PLAYING)!=GST_STATE_CHANGE_FAILURE,"card PLAYING");
        auto *message=gst_bus_timed_pop_filtered(bus,15*GST_SECOND,GstMessageType(GST_MESSAGE_ERROR|GST_MESSAGE_EOS));
        need(message,"bounded card EOS");check_error(message);gst_message_unref(message);
        GstStructure *stats=nullptr;g_object_get(sink,"field-stats",&stats,nullptr);
        auto *text=gst_structure_to_string(stats);
        printf("card run%u: %s\n",run,text);g_free(text);gst_structure_free(stats);
        need(counter(sink,"fields")==before+100,"50 native pictures/100 unique field uses");
        need(gst_element_set_state(pipeline,GST_STATE_NULL)!=GST_STATE_CHANGE_FAILURE,"card NULL");
        for(const char *key:{"allocations","images","retained-buffers","failures"})need(!counter(sink,key),"clean native retirement");
        printf("{\"type\":\"card-window\",\"queued\":%s,\"run\":%u,\"fields\":100,\"resources_after_null\":0,\"pass\":true}\n",queued?"true":"false",run);
    }
    gst_object_unref(sink);gst_object_unref(bus);gst_object_unref(pipeline);
}
int main(int argc,char **argv) {
    try {
        gst_init(&argc,&argv);
        need(temperature()<75000,"start temperature below75C");
        need(gst_element_register(nullptr,"crystalhdfieldsink",GST_RANK_NONE,gst_crystalhd_field_get_type()),"register experimental sink");
        if(argc==3&&(std::string(argv[1])=="--card"||std::string(argv[1])=="--card-direct"))card(argv[2],std::string(argv[1])=="--card");
        else {need(argc==1,"usage: gst-field-output [--card|--card-direct 50-picture.h264]");pixels();retirement_pixels();slot_retirement_pixels(false);slot_retirement_pixels(true);pipeline(false);pipeline(true);}
        return 0;
    }catch(const std::exception &error){fprintf(stderr,"FAIL: %s\n",error.what());return 1;}
}
