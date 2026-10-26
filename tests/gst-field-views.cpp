// SPDX-License-Identifier: GPL-2.0-or-later
// #530 opt-in legacy-card/field-view discriminator. No display or audio.
#include "field-view-gpu.h"
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <atomic>
#include <dlfcn.h>
#include <map>
#include <mutex>
#include <set>
#include <sys/resource.h>

struct Trace {
    std::atomic<unsigned> inputs{0},outputs{0};
    std::mutex lock;
    std::map<GstClockTime,GstClockTime> ready;
    static GstPadProbeReturn input(GstPad *,GstPadProbeInfo *info,gpointer opaque) {
        auto &self=*static_cast<Trace *>(opaque);
        auto *buffer=GST_PAD_PROBE_INFO_BUFFER(info);
        const unsigned i=self.inputs++;
        const auto pts=GST_BUFFER_PTS(buffer);
        const auto text=GST_CLOCK_TIME_IS_VALID(pts)?std::to_string(pts):"null";
        printf("{\"type\":\"input-pad\",\"index\":%u,\"pts\":%s,\"arrival_ns\":%llu}\n",
               i,text.c_str(),
               static_cast<unsigned long long>(gst_util_get_timestamp()));
        return GST_PAD_PROBE_OK;
    }
    static GstPadProbeReturn output(GstPad *,GstPadProbeInfo *info,gpointer opaque) {
        auto &self=*static_cast<Trace *>(opaque);auto *buffer=GST_PAD_PROBE_INFO_BUFFER(info);
        std::lock_guard<std::mutex> guard(self.lock);
        self.ready[GST_BUFFER_PTS(buffer)]=gst_util_get_timestamp();++self.outputs;
        return GST_PAD_PROBE_OK;
    }
    GstClockTime take(GstClockTime pts) {
        std::lock_guard<std::mutex> guard(lock);const auto found=ready.find(pts);
        need(found!=ready.end(),"source-ready observation");
        const auto value=found->second;ready.erase(found);return value;
    }
};

struct Pipeline {
    Trace trace;
    GstElement *pipeline=nullptr,*sink=nullptr,*decoder=nullptr;
    GstBus *bus=nullptr;
    bool closed=false;
    Pipeline()=default;
    void setup(const char *path,unsigned scale) {
        GError *error=nullptr;
        pipeline=gst_parse_launch(
            "filesrc name=input ! h264parse ! crystalhddec name=decoder ! "
            "appsink name=output caps=\"video/x-raw,format=YUY2\" sync=false "
            "max-buffers=1 drop=false wait-on-eos=false enable-last-sample=false",&error);
        const std::string message=error?error->message:"pipeline";
        const bool parsed=error==nullptr;
        if(error)g_error_free(error);
        need(parsed&&pipeline&&GST_IS_PIPELINE(pipeline),message.c_str());
        auto *input=gst_bin_get_by_name(GST_BIN(pipeline),"input");
        sink=gst_bin_get_by_name(GST_BIN(pipeline),"output");
        decoder=gst_bin_get_by_name(GST_BIN(pipeline),"decoder");
        need(input&&sink&&decoder,"explicit pipeline elements");
        g_object_set(input,"location",path,nullptr);gst_object_unref(input);
        g_object_set(decoder,"scale-width",scale,nullptr);
        auto *factory=gst_element_get_factory(decoder);
        need(std::string(gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory)))=="crystalhddec",
             "explicit CrystalHD decoder; no software fallback");
        for(const char *name:{"sink","src"}) {
            auto *pad=gst_element_get_static_pad(decoder,name);
            need(pad,"decoder observation pad");
            gst_pad_add_probe(pad,GST_PAD_PROBE_TYPE_BUFFER,
                              name[1]=='i'?Trace::input:Trace::output,&trace,nullptr);
            gst_object_unref(pad);
        }
        bus=gst_element_get_bus(pipeline);
        auto *clock=gst_system_clock_obtain();gst_pipeline_use_clock(GST_PIPELINE(pipeline),clock);
        gst_object_unref(clock);
    }
    void error_check() {
        auto *message=gst_bus_pop_filtered(bus,GST_MESSAGE_ERROR);
        if(!message)return;
        GError *error=nullptr;gchar *debug=nullptr;gst_message_parse_error(message,&error,&debug);
        std::string text=error?error->message:"GStreamer error";
        if(error)g_error_free(error);
        g_free(debug);gst_message_unref(message);
        need(false,text.c_str());
    }
    void state(GstState target) {
        need(gst_element_set_state(pipeline,target)!=GST_STATE_CHANGE_FAILURE,"pipeline state request");
        GstState current=GST_STATE_NULL;
        const auto result=gst_element_get_state(pipeline,&current,nullptr,10*GST_SECOND);
        error_check();need(result!=GST_STATE_CHANGE_FAILURE&&current==target,"bounded pipeline state");
    }
    void finish() {
        if(closed)return;
        state(GST_STATE_NULL);closed=true;
    }
    ~Pipeline() {
        if(pipeline&&!closed)gst_element_set_state(pipeline,GST_STATE_NULL);
        if(bus)gst_object_unref(bus);
        if(decoder)gst_object_unref(decoder);
        if(sink)gst_object_unref(sink);
        if(pipeline)gst_object_unref(pipeline);
    }
};

struct Sample {
    GstSample *sample;
    GstVideoFrame frame{};
    bool mapped=false;
    Sample(GstSample *s,const GstVideoInfo &info):sample(s) {
        need(s,"sample");
        mapped=gst_video_frame_map(&frame,&info,gst_sample_get_buffer(s),GST_MAP_READ);
        if(!mapped){gst_sample_unref(sample);sample=nullptr;need(false,"read-only decoded frame mapping");}
    }
    struct Rows { const uint8_t *data; size_t stride,available; };
    Rows rows(unsigned w,unsigned h) const {
        const auto *data=static_cast<const uint8_t *>(GST_VIDEO_FRAME_PLANE_DATA(&frame,0));
        const auto stride=GST_VIDEO_FRAME_PLANE_STRIDE(&frame,0);
        const auto &mapping=frame.map[0];
        const auto address=reinterpret_cast<uintptr_t>(data),origin=reinterpret_cast<uintptr_t>(mapping.data);
        need(stride>=int(w*2)&&address>=origin&&address-origin<=mapping.size&&
             size_t(h-1)*stride+w*2<=mapping.size-(address-origin),"mapped row extent");
        return {data,size_t(stride),mapping.size-(address-origin)};
    }
    ~Sample(){if(mapped)gst_video_frame_unmap(&frame);gst_sample_unref(sample);}
};

// CPU materialization control, same physical-nearest RGB reference. The
// half-source-row phase makes the duplicated full-height rows agree with
// direct field sampling at scaled output coordinates.
struct Upload {
    GLuint texture=0,program=0;
    unsigned w,h;
    float xshift,yshift;
    Upload(unsigned sw,unsigned sh,unsigned dw,unsigned dh):w(sw),h(sh) {
        xshift=nearest_bias(sw,dw,2);
        const int64_t denominator=int64_t(2)*dh;int64_t gap=denominator;
        for(unsigned y=0;y<dh;++y) {
            const int64_t numerator=int64_t(2*y+1)*sh-dh;
            if(numerator>=0)gap=std::min(gap,denominator-numerator%denominator);
        }
        yshift=(-0.5+double(gap)/(2*denominator))/sh;
        const char *vs="attribute vec2 pos;uniform vec2 shift;varying vec2 uv;void main(){uv=(pos+1.0)*0.5+shift;gl_Position=vec4(pos,0.,1.);}";
        const char *fs="precision highp float;varying vec2 uv;uniform sampler2D tex;uniform float width;void main(){vec4 p=texture2D(tex,uv);float y=mod(floor(uv.x*width),2.0)<1.0?p.r:p.b;float c=(y-16.0/255.0)*(255.0/219.0);float u=(p.g-128.0/255.0)*(255.0/224.0);float v=(p.a-128.0/255.0)*(255.0/224.0);gl_FragColor=vec4(c+1.5748*v,c-0.187324*u-0.468124*v,c+1.8556*u,1.0);}";
        program=Renderer::link(vs,fs);glGenTextures(1,&texture);
        glBindTexture(GL_TEXTURE_2D,texture);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,w/2,h,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        need(glGetError()==GL_NO_ERROR,"CPU control allocation");
    }
    void draw(Renderer &renderer,const std::vector<uint8_t> &bob,bool upload_pixels=true) {
        need(bob.size()==size_t(w)*h*2,"upload extent");
        glBindFramebuffer(GL_FRAMEBUFFER,renderer.picture.fbo);
        glViewport(0,0,renderer.picture.w,renderer.picture.h);glUseProgram(program);
        glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,texture);
        if(upload_pixels)glTexSubImage2D(GL_TEXTURE_2D,0,0,0,w/2,h,GL_RGBA,GL_UNSIGNED_BYTE,bob.data());
        glUniform1i(glGetUniformLocation(program,"tex"),0);
        glUniform1f(glGetUniformLocation(program,"width"),w);
        glUniform2f(glGetUniformLocation(program,"shift"),xshift,yshift);
        static const float positions[]={-1,-1,1,-1,-1,1,1,1};
        glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,positions);
        glEnableVertexAttribArray(0);glDrawArrays(GL_TRIANGLE_STRIP,0,4);glFinish();
        need(glGetError()==GL_NO_ERROR,"CPU control GPU completion");
    }
    ~Upload(){if(texture)glDeleteTextures(1,&texture);if(program)glDeleteProgram(program);}
};

static unsigned device_fds() {
    unsigned count=0;
    for(const auto &p:std::filesystem::directory_iterator("/proc/self/fd")) {
        std::error_code error;
        if(std::filesystem::read_symlink(p.path(),error)=="/dev/crystalhd")++count;
    }
    return count;
}

int main(int argc,char **argv) {
    try {
        const double main_start=clock_seconds(CLOCK_MONOTONIC);
        need(argc==7,"usage: gst-field-views ANNEXB_INPUT EXPECTED_PICTURES SCALE_WIDTH OUTPUT_HEIGHT field|cpu verify|clocked");
        const unsigned expected=std::stoul(argv[2]),scale=std::stoul(argv[3]),oh=std::stoul(argv[4]);
        const bool field=std::string(argv[5])=="field",verify=std::string(argv[6])=="verify";
        need((field||std::string(argv[5])=="cpu")&&(verify||std::string(argv[6])=="clocked"),"mode");
        need(expected&&expected<=750&&(scale==0||(scale>=128&&scale<=1920&&!(scale%16)))&&
             (oh==360||oh==720||oh==1080),"bounded geometry/count");
        need(temperature()<75000,"start temperature below75C");
        setvbuf(stdout,nullptr,_IOLBF,0);gst_init(nullptr,nullptr);
        yuy2_gpu::GPU gpu("/dev/dri/renderD128");Pipeline pipeline;pipeline.setup(argv[1],scale);
        pipeline.state(GST_STATE_PAUSED);
        need(device_fds()>0,"real CrystalHD device ownership at preroll");
        auto *feature=gst_element_get_factory(pipeline.decoder);
        auto *plugin=gst_plugin_feature_get_plugin(GST_PLUGIN_FEATURE(feature));
        need(plugin,"actual decoder plugin");
        auto *library=dlopen("libcrystalhd.so.3",RTLD_NOW|RTLD_NOLOAD);
        need(library,"already loaded legacy library");Dl_info identity{};
        need(dladdr(dlsym(library,"DtsProcInput"),&identity)&&identity.dli_fname,"actual library identity");
        printf("{\"type\":\"runtime\",\"decoder\":\"%s\",\"library\":\"%s\",\"device_fds\":%u}\n",
               gst_plugin_get_filename(plugin),identity.dli_fname,device_fds());
        dlclose(library);gst_object_unref(plugin);
        auto *preroll=gst_app_sink_try_pull_preroll(GST_APP_SINK(pipeline.sink),GST_SECOND);
        need(preroll,"bounded preroll");GstVideoInfo info{};
        need(gst_video_info_from_caps(&info,gst_sample_get_caps(preroll)),"decoded caps");
        auto held_preroll=std::make_unique<Sample>(preroll,info);
        const unsigned w=GST_VIDEO_INFO_WIDTH(&info),h=GST_VIDEO_INFO_HEIGHT(&info),ow=oh*16/9;
        need(GST_VIDEO_INFO_FORMAT(&info)==GST_VIDEO_FORMAT_YUY2&&w&&h&&!(w%2)&&!(h%2)&&w<=1920&&h<=1088,"linear YUY2 geometry");
        need(GST_VIDEO_INFO_FPS_N(&info)>0&&GST_VIDEO_INFO_FPS_D(&info)>0,"declared frame rate");
        const GstClockTime nominal=gst_util_uint64_scale(GST_SECOND,GST_VIDEO_INFO_FPS_D(&info),GST_VIDEO_INFO_FPS_N(&info));
        need(nominal>=GST_SECOND/60&&nominal<=GST_SECOND/20,"20..60 pictures/s");
        Renderer renderer(w,h,ow,oh);std::unique_ptr<Allocation> memory;std::unique_ptr<Upload> upload;
        if(field){int heap=open("/dev/dma_heap/system",O_RDWR|O_CLOEXEC);need(heap>=0,"DMA heap");memory=std::make_unique<Allocation>(heap,w,h);close(heap);}
        else upload=std::make_unique<Upload>(w,h,ow,oh);
        auto *digest=g_checksum_new(G_CHECKSUM_SHA256);need(digest,"source census digest");
        auto *signatures=g_checksum_new(G_CHECKSUM_SHA256);need(signatures,"signature census digest");
        std::set<std::string> generations;std::vector<uint8_t> bob,oracle,signature(size_t(h)*12);
        std::array<std::unique_ptr<Image>,2> prepared_views;
        auto *first_buffer=gst_sample_get_buffer(held_preroll->sample);
        need(GST_BUFFER_FLAG_IS_SET(first_buffer,GST_VIDEO_BUFFER_FLAG_INTERLACED)&&
             !GST_BUFFER_FLAG_IS_SET(first_buffer,GST_VIDEO_BUFFER_FLAG_ONEFIELD)&&
             !GST_BUFFER_FLAG_IS_SET(first_buffer,GST_VIDEO_BUFFER_FLAG_RFF),"complete interlaced preroll");
        const unsigned first_parity=GST_BUFFER_FLAG_IS_SET(first_buffer,GST_VIDEO_BUFFER_FLAG_TFF)?0:1;
        const auto first_rows=held_preroll->rows(w,h);
        // Prepare the actual first source while PAUSED. Do not rebase PTS or
        // move the media clock to conceal cold upload/JIT work after PLAYING.
        // This GPU use is preroll, not another unique presented field.
        if(field) {
            memory->write(first_rows.data,first_rows.stride,first_rows.available);
            for(unsigned p=0;p<2;++p){prepared_views[p]=std::make_unique<Image>(gpu,*memory,true,p);need(prepared_views[p]->texture,"preroll field import");}
            renderer.draw(*prepared_views[first_parity],true,first_parity);
        } else {
            cpu_bob(first_rows.data,first_rows.stride,w,h,first_parity,bob);
            upload->draw(renderer,bob);
        }
        unsigned pictures=0,fields=0,late=0;GstClockTime previous=GST_CLOCK_TIME_NONE;
        double producer_cpu=0,producer_wall=0,render_cpu=0,render_wall=0,observer_cpu=0,observer_wall=0,maximum_age=0,maximum_ready_age=0;
        const double cpu_start=clock_seconds(CLOCK_PROCESS_CPUTIME_ID),wall_start=clock_seconds(CLOCK_MONOTONIC);
        pipeline.state(GST_STATE_PLAYING);auto *clock=gst_element_get_clock(pipeline.pipeline);need(clock,"media clock");
        const auto base=gst_element_get_base_time(pipeline.pipeline);
        printf("{\"type\":\"native-start\",\"path\":\"%s\",\"verify\":%s,\"source_width\":%u,\"source_height\":%u,\"output_width\":%u,\"output_height\":%u,\"nominal_duration_ns\":%llu,\"physical_presentation\":false}\n",field?"field":"cpu",verify?"true":"false",w,h,ow,oh,static_cast<unsigned long long>(nominal));
        while(true) {
            pipeline.error_check();auto *next=gst_app_sink_try_pull_sample(GST_APP_SINK(pipeline.sink),GST_SECOND);
            if(!next){pipeline.error_check();if(gst_app_sink_is_eos(GST_APP_SINK(pipeline.sink)))break;need(clock_seconds(CLOCK_MONOTONIC)-wall_start<60,"bounded sample wait");continue;}
            GstVideoInfo current{};
            if(!gst_video_info_from_caps(&current,gst_sample_get_caps(next))||
               !gst_video_info_is_equal(&info,&current)) {
                gst_sample_unref(next);need(false,"constant negotiated source geometry/timing");
            }
            Sample sample(next,info);auto *buffer=gst_sample_get_buffer(next);const auto pts=GST_BUFFER_PTS(buffer);
            need(pictures<expected&&GST_CLOCK_TIME_IS_VALID(pts)&&(pictures==0||pts>previous),"picture count/ordered original PTS");previous=pts;
            need(GST_BUFFER_FLAG_IS_SET(buffer,GST_VIDEO_BUFFER_FLAG_INTERLACED)&&
                 !GST_BUFFER_FLAG_IS_SET(buffer,GST_VIDEO_BUFFER_FLAG_ONEFIELD)&&
                 !GST_BUFFER_FLAG_IS_SET(buffer,GST_VIDEO_BUFFER_FLAG_RFF),"complete interlaced nonrepeat picture");
            const bool tff=GST_BUFFER_FLAG_IS_SET(buffer,GST_VIDEO_BUFFER_FLAG_TFF);
            const auto duration=GST_CLOCK_TIME_IS_VALID(GST_BUFFER_DURATION(buffer))?GST_BUFFER_DURATION(buffer):nominal;
            need(duration==nominal,"constant-rate original duration");
            const auto *segment=gst_sample_get_segment(next);need(segment&&segment->rate==1.0,"ordinary media segment");
            const auto running=gst_segment_to_running_time(segment,GST_FORMAT_TIME,pts);need(GST_CLOCK_TIME_IS_VALID(running),"original segment PTS");
            const auto ready=pipeline.trace.take(pts);
            const auto mapped=sample.rows(w,h);const auto *data=mapped.data;const auto stride=mapped.stride;
            if(pictures==0)need(buffer==first_buffer,"first source identity matches held preroll");
            auto *one=g_checksum_new(G_CHECKSUM_SHA256);need(one,"generation digest");
            const double oc=clock_seconds(CLOCK_THREAD_CPUTIME_ID),owall=clock_seconds(CLOCK_MONOTONIC);
            if(verify)oracle.resize(size_t(w)*h*2);
            for(unsigned row=0;row<h;++row) {
                const auto *line=data+size_t(row)*stride;
                if(verify){g_checksum_update(digest,line,w*2);memcpy(oracle.data()+size_t(row)*w*2,line,w*2);}
                // Timed identity observer: three original YUYV pairs on every
                // active row. This is sparse, NOT a full-picture pixel hash.
                memcpy(signature.data()+size_t(row)*12,line,4);
                memcpy(signature.data()+size_t(row)*12+4,line+(w/4)*4,4);
                memcpy(signature.data()+size_t(row)*12+8,line+(w-2)*2,4);
            }
            g_checksum_update(signatures,signature.data(),signature.size());
            g_checksum_update(one,signature.data(),signature.size());
            generations.insert(g_checksum_get_string(one));g_checksum_free(one);
            observer_cpu+=clock_seconds(CLOCK_THREAD_CPUTIME_ID)-oc;observer_wall+=clock_seconds(CLOCK_MONOTONIC)-owall;
            std::array<std::unique_ptr<Image>,2> views;
            if(field&&pictures==0)views=std::move(prepared_views);
            else if(field) {
                const double pc=clock_seconds(CLOCK_THREAD_CPUTIME_ID),pw=clock_seconds(CLOCK_MONOTONIC);
                memory->write(data,stride,mapped.available);
                for(unsigned p=0;p<2;++p){views[p]=std::make_unique<Image>(gpu,*memory,true,p);need(views[p]->texture,"both real field views");}
                producer_cpu+=clock_seconds(CLOCK_THREAD_CPUTIME_ID)-pc;producer_wall+=clock_seconds(CLOCK_MONOTONIC)-pw;
            }
            for(unsigned use=0;use<2;++use) {
                const unsigned parity=use^(tff?0:1);const auto due=base+running+use*(duration/2);
                if(!verify) {
                    need(due<=gst_clock_get_time(clock)+2*GST_SECOND,"bounded field clock wait");
                    auto id=gst_clock_new_single_shot_id(clock,due);const auto waited=gst_clock_id_wait(id,nullptr);gst_clock_id_unref(id);
                    need(waited==GST_CLOCK_OK||waited==GST_CLOCK_EARLY,"original field deadline");
                }
                const double rc=clock_seconds(CLOCK_THREAD_CPUTIME_ID),rw=clock_seconds(CLOCK_MONOTONIC);
                if(field)renderer.draw(*views[parity],true,parity);
                else {
                    const bool reuse= pictures==0&&use==0;
                    if(!reuse)cpu_bob(data,stride,w,h,parity,bob);
                    upload->draw(renderer,bob,!reuse);
                }
                render_cpu+=clock_seconds(CLOCK_THREAD_CPUTIME_ID)-rc;render_wall+=clock_seconds(CLOCK_MONOTONIC)-rw;++fields;
                const auto completed=gst_clock_get_time(clock);
                const double age=completed>due?double(completed-due)/GST_MSECOND:0;
                if(!verify){maximum_age=std::max(maximum_age,age);late+=completed>due+duration/2;maximum_ready_age=std::max(maximum_ready_age,double(gst_util_get_timestamp()-ready)/GST_MSECOND);}
                printf("{\"type\":\"field\",\"picture\":%u,\"use\":%u,\"parity\":%u,\"pts_ns\":%llu,\"duration_ns\":%llu,\"source_ready_ns\":%llu,\"completed_ns\":%llu,\"clocked\":%s,\"deadline_age_ms\":%.6f}\n",pictures,use,parity,static_cast<unsigned long long>(pts+use*duration/2),static_cast<unsigned long long>(duration/2),static_cast<unsigned long long>(ready),static_cast<unsigned long long>(completed),verify?"false":"true",verify?0:age);
                if(verify)verify_scaled(renderer,oracle,parity,field?"native-field-view":"native-cpu-upload");
            }
            // Both GPU uses finish before either image or the held source
            // sample is retired. Only the next iteration can update the BO.
            if(pictures==0)held_preroll.reset();
            ++pictures;need(temperature()<85000,"temperature below85C");
            need(clock_seconds(CLOCK_MONOTONIC)-wall_start<60,"60s experiment deadline");
        }
        const double cpu=clock_seconds(CLOCK_PROCESS_CPUTIME_ID)-cpu_start,wall=clock_seconds(CLOCK_MONOTONIC)-wall_start;
        gst_object_unref(clock);need(pictures==expected&&fields==2*expected&&pipeline.trace.outputs==expected,"complete real picture/field census");
        pipeline.finish();need(device_fds()==0,"live-process CrystalHD FD retirement");
        rusage usage{};need(getrusage(RUSAGE_SELF,&usage)==0,"peak RSS");
        const std::string source_hash=verify?std::string("\"")+g_checksum_get_string(digest)+"\"":"null";
        printf("{\"type\":\"native-result\",\"path\":\"%s\",\"verify\":%s,\"pictures\":%u,\"input_pad_aus\":%u,\"completed_fields\":%u,\"late_fields\":%u,\"unique_sparse_signatures\":%zu,\"sparse_signature_sha256\":\"%s\",\"source_sha256\":%s,\"observer_thread_cpu_s\":%.9f,\"observer_wall_s\":%.9f,\"producer_thread_cpu_s\":%.9f,\"producer_wall_s\":%.9f,\"render_thread_cpu_s\":%.9f,\"render_wall_s\":%.9f,\"playback_process_cpu_s\":%.9f,\"playback_wall_s\":%.9f,\"process_cpu_s_at_null\":%.9f,\"main_wall_s_to_null\":%.9f,\"peak_rss_kib\":%ld,\"max_deadline_age_ms\":%.6f,\"max_ready_age_ms\":%.6f,\"device_fds_after_null\":0,\"natural_eos\":true,\"physical_presentation\":false}\n",field?"field":"cpu",verify?"true":"false",pictures,pipeline.trace.inputs.load(),fields,late,generations.size(),g_checksum_get_string(signatures),source_hash.c_str(),observer_cpu,observer_wall,producer_cpu,producer_wall,render_cpu,render_wall,cpu,wall,clock_seconds(CLOCK_PROCESS_CPUTIME_ID),clock_seconds(CLOCK_MONOTONIC)-main_start,usage.ru_maxrss,maximum_age,maximum_ready_age);
        g_checksum_free(signatures);g_checksum_free(digest);return 0;
    }catch(const std::exception &error){fprintf(stderr,"FAIL %s\n",error.what());return 1;}
}
