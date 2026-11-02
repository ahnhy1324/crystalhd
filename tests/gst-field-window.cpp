// SPDX-License-Identifier: GPL-2.0-or-later
// Explicit current-host X11/GPU regression; no decoder, A/V or scanout claim.
#include "../filters/gst/field-output/gstcrystalhdfield.cpp"
#include <gst/app/gstappsrc.h>
#include <chrono>

struct Parents {
    xcb_connection_t *connection=nullptr;
    xcb_screen_t *screen=nullptr;
    std::vector<xcb_window_t> windows;
    Parents() {
        int index=0;connection=xcb_connect(nullptr,&index);
        need(connection&&!xcb_connection_has_error(connection),"test XCB connection");
        auto screens=xcb_setup_roots_iterator(xcb_get_setup(connection));
        while(index--&&screens.rem)xcb_screen_next(&screens);
        need(screens.rem,"test X screen");screen=screens.data;
    }
    xcb_window_t make(unsigned width,unsigned height,xcb_window_t parent=0) {
        auto id=xcb_generate_id(connection);uint32_t background=0;
        auto *error=xcb_request_check(connection,xcb_create_window_checked(connection,
            XCB_COPY_FROM_PARENT,id,parent?parent:screen->root,0,0,width,height,0,
            XCB_WINDOW_CLASS_INPUT_OUTPUT,screen->root_visual,XCB_CW_BACK_PIXEL,&background));
        if(error){free(error);need(false,"test parent creation");}
        windows.push_back(id);xcb_map_window(connection,id);xcb_flush(connection);return id;
    }
    void destroy(xcb_window_t id) {
        auto *error=xcb_request_check(connection,xcb_destroy_window_checked(connection,id));
        if(error){free(error);need(false,"test parent destruction");}
        windows.erase(std::remove(windows.begin(),windows.end(),id),windows.end());
    }
    xcb_window_t parent(xcb_window_t child) {
        auto *reply=xcb_query_tree_reply(connection,xcb_query_tree(connection,child),nullptr);
        need(reply,"test child tree");auto value=reply->parent;free(reply);return value;
    }
    xcb_window_t child(xcb_window_t parent) {
        auto *reply=xcb_query_tree_reply(connection,xcb_query_tree(connection,parent),nullptr);
        need(reply&&xcb_query_tree_children_length(reply)==1,"one owned video child");
        auto id=xcb_query_tree_children(reply)[0];free(reply);return id;
    }
    void close_message(xcb_window_t recipient,xcb_window_t subject) {
        auto atom=[&](const char *name) {
            auto *reply=xcb_intern_atom_reply(connection,xcb_intern_atom(connection,0,strlen(name),name),nullptr);
            need(reply,"test WM atom");auto id=reply->atom;free(reply);return id;
        };
        xcb_client_message_event_t event{};event.response_type=XCB_CLIENT_MESSAGE;
        event.window=subject;event.type=atom("WM_PROTOCOLS");event.format=32;
        event.data.data32[0]=atom("WM_DELETE_WINDOW");
        xcb_send_event(connection,0,recipient,0,reinterpret_cast<const char *>(&event));xcb_flush(connection);
    }
    void resize(xcb_window_t id,unsigned width,unsigned height) {
        uint32_t size[]={width,height};
        auto *error=xcb_request_check(connection,xcb_configure_window_checked(connection,id,
            XCB_CONFIG_WINDOW_WIDTH|XCB_CONFIG_WINDOW_HEIGHT,size));
        if(error){free(error);need(false,"test resize");}
    }
    std::array<unsigned,2> size(xcb_window_t id) {
        auto *reply=xcb_get_geometry_reply(connection,xcb_get_geometry(connection,id),nullptr);
        need(reply,"test geometry");std::array<unsigned,2> value={reply->width,reply->height};free(reply);return value;
    }
    ~Parents() {
        if(connection){for(auto id:windows)xcb_destroy_window(connection,id);xcb_disconnect(connection);}
    }
};
static std::vector<uint8_t> pattern() {
    std::vector<uint8_t> source(640*360*2);
    for(unsigned y=0;y<360;++y)for(unsigned x=0;x<640;x+=2) {
        auto *p=source.data()+(size_t(y)*640+x)*2;
        p[0]=p[2]=(y&1)?176:64;p[1]=p[3]=128;
    }
    return source;
}
static guint64 counter(GstElement *sink,const char *key) {
    GstStructure *stats=nullptr;g_object_get(sink,"field-stats",&stats,nullptr);
    guint64 value=0;need(stats&&gst_structure_get_uint64(stats,key,&value),"counter");
    gst_structure_free(stats);return value;
}
template<class Predicate> static void await(Predicate predicate,const char *label,unsigned milliseconds=1200) {
    const auto end=std::chrono::steady_clock::now()+std::chrono::milliseconds(milliseconds);
    while(!predicate()) {
        need(std::chrono::steady_clock::now()<end,label);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}
static void pixels() {
    Parents parents;auto a=parents.make(640,360),b=parents.make(320,180),c=parents.make(480,270);
    crystalhd_field::Window window(a);
    yuy2_gpu::GPU gpu("/dev/dri/renderD128",window.display,window.context);
    GbmAllocation storage(gpu,640,360);Renderer renderer(640,360,2,2);
    const auto source=pattern();storage.write(source.data(),1280,source.size());
    Image top(gpu,storage,true,0,EGL_ITU_REC709_EXT,false),bottom(gpu,storage,true,1,EGL_ITU_REC709_EXT,false);
    const auto context=window.context;const auto surface=window.surface;const auto child=window.window;
    const auto upper=top.texture,lower=bottom.texture;
    unsigned comparisons=0;
    for(auto target:{guintptr(a),guintptr(b),guintptr(0),guintptr(c)}) {
        need(window.replace(target),"valid live window reparent");
        need(window.window==child&&window.context==context&&window.surface==surface&&top.texture==upper&&bottom.texture==lower,
            "same child/context/import ownership");
        printf("{\"test\":\"attachment\",\"requested\":%llu,\"actual_parent\":%u,\"embedded\":%s}\n",
            static_cast<unsigned long long>(target),parents.parent(child),window.embedded?"true":"false");
        // A normal standalone window can already be inside the WM's frame.
        need(target?parents.parent(child)==target:!window.embedded,"actual reparent acknowledgement");
        if(target==b)parents.destroy(a);
        if(!target)parents.destroy(b);
        const auto invalid=xcb_generate_id(parents.connection);
        need(!window.replace(invalid)&&!window.replace(child),"reject nonexistent and self parents");
        need(!window.replace(parents.screen->root),"root uses explicit standalone mode");
        if(!target) {
            const auto nested=parents.make(160,90,window.standalone);
            need(!window.replace(nested)&&parents.parent(nested)==window.standalone,
                "reject descendant before deleting owned standalone host");
            parents.destroy(nested);
        }
        if(sizeof(guintptr)>4)need(!window.replace(guintptr(G_MAXUINT32)+guintptr(1)),"reject truncated window handle");
        need(target?parents.parent(child)==target:!window.embedded,"rejected replacement preserves attachment");
        for(unsigned parity=0;parity<2;++parity) {
            glBindFramebuffer(GL_FRAMEBUFFER,0);glViewport(0,0,window.width,window.height);
            renderer.draw_to(parity?bottom:top,true,parity,0,0,0,window.width,window.height,true,false,false);
            std::vector<uint8_t> actual(size_t(window.width)*window.height*4);
            glReadPixels(0,0,window.width,window.height,GL_RGBA,GL_UNSIGNED_BYTE,actual.data());
            need(glGetError()==GL_NO_ERROR,"replacement GPU readback");
            for(size_t i=0;i<actual.size();i+=4)for(unsigned channel=0;channel<3;++channel) {
                if(std::abs(int(actual[i+channel])-(parity?186:56))>2) {
                    EGLint ew=0,eh=0;eglQuerySurface(window.display,window.surface,EGL_WIDTH,&ew);
                    eglQuerySurface(window.display,window.surface,EGL_HEIGHT,&eh);
                    fprintf(stderr,"pixel target=%llu field=%u xy=%zu,%zu rgb=%u,%u,%u size=%u,%u egl=%d,%d\n",
                        static_cast<unsigned long long>(target),parity,(i/4)%window.width,(i/4)/window.width,
                        actual[i],actual[i+1],actual[i+2],window.width,window.height,ew,eh);
                    const auto native=parents.size(child),outer=parents.size(window.parent);
                    fprintf(stderr,"native=%u,%u parent=%u,%u actual_parent=%u\n",
                        native[0],native[1],outer[0],outer[1],parents.parent(child));
                    need(false,"retained field identity after replacement");
                }
            }
            need(eglSwapBuffers(window.display,window.surface),"replacement swap");++comparisons;
        }
    }
    printf("{\"test\":\"window-pixels\",\"comparisons\":%u,\"same_context_and_imports\":true,\"pass\":true}\n",comparisons);
}
static void pipeline() {
    Parents parents;auto a=parents.make(640,360),b=parents.make(320,180);
    auto *pipeline=gst_pipeline_new(nullptr),*input=gst_element_factory_make("appsrc",nullptr),
         *sink=gst_element_factory_make("crystalhdfieldsink",nullptr);
    need(pipeline&&input&&sink,"replacement factories");
    const auto cleanup=[](GstElement *value){gst_element_set_state(value,GST_STATE_NULL);gst_object_unref(value);};
    std::unique_ptr<GstElement,decltype(cleanup)> owned(pipeline,cleanup);
    auto *caps=gst_caps_from_string("video/x-raw,format=YUY2,width=640,height=360,framerate=25/1,interlace-mode=interleaved,colorimetry=bt709");
    g_object_set(input,"caps",caps,"format",GST_FORMAT_TIME,nullptr);gst_caps_unref(caps);
    gst_bin_add_many(GST_BIN(pipeline),input,sink,nullptr);need(gst_element_link(input,sink),"replacement link");
    auto *bus=gst_element_get_bus(pipeline);
    gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(sink),a);
    need(gst_element_set_state(pipeline,GST_STATE_PAUSED)!=GST_STATE_CHANGE_FAILURE,"replacement PAUSED");
    auto source=pattern();auto *buffer=gst_buffer_new_allocate(nullptr,source.size(),nullptr);
    gst_buffer_fill(buffer,0,source.data(),source.size());GST_BUFFER_PTS(buffer)=0;GST_BUFFER_DURATION(buffer)=4*GST_SECOND;
    GST_BUFFER_FLAG_SET(buffer,GST_VIDEO_BUFFER_FLAG_INTERLACED);GST_BUFFER_FLAG_SET(buffer,GST_VIDEO_BUFFER_FLAG_TFF);
    need(gst_app_src_push_buffer(GST_APP_SRC(input),buffer)==GST_FLOW_OK,"replacement preroll input");
    await([&]{return counter(sink,"prerolls")==1;},"replacement preroll",3000);
    auto child=parents.child(a);auto retired=counter(sink,"retirements"),redraws=counter(sink,"redraws");
    gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(sink),b);
    need(parents.parent(child)==b&&counter(sink,"window-handle")==b,"paused reparent applied before return");
    await([&]{return counter(sink,"redraws")>redraws;},"paused retained redraw");
    need(!counter(sink,"fields")&&counter(sink,"retirements")==retired&&counter(sink,"images")==2,"pause preserves source and imports");
    parents.close_message(child,a);
    auto *stale=gst_bus_timed_pop_filtered(bus,150*GST_MSECOND,GST_MESSAGE_APPLICATION);
    need(!stale,"retired parent close must not close the new output");
    parents.close_message(child,b);
    auto *close=gst_bus_timed_pop_filtered(bus,GST_SECOND,GST_MESSAGE_APPLICATION);
    need(close&&gst_structure_has_name(gst_message_get_structure(close),"crystalhd-field-close"),"current close still forwarded");
    gst_message_unref(close);
    auto *field=reinterpret_cast<GstCrystalhdField *>(sink);auto worker=get_worker(field);
    guint64 generation=0;{std::lock_guard<std::mutex> lock(worker->mutex);generation=worker->generation;}
    need(gst_element_set_state(pipeline,GST_STATE_PLAYING)!=GST_STATE_CHANGE_FAILURE,"replacement PLAYING");
    await([&]{std::lock_guard<std::mutex> lock(worker->mutex);return worker->active_wait&&counter(sink,"fields")==1;},"scheduled second field",1000);
    // Queue an A resize while subscribed, leave A, resize it unsubscribed,
    // and return to A. The queued old size must not overwrite its live size.
    parents.resize(b,608,342);
    const auto begin=std::chrono::steady_clock::now();
    gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(sink),a);
    parents.resize(b,704,396);
    gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(sink),b);
    const auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
    parents.destroy(a);
    need(elapsed<500&&parents.parent(child)==b,"window request interrupts long clock wait");
    {std::lock_guard<std::mutex> lock(worker->mutex);need(worker->generation==generation,"same field generation");}
    need(counter(sink,"fields")==1&&!counter(sink,"cancelled"),"replacement keeps field deadline");
    const auto invalid=xcb_generate_id(parents.connection);
    gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(sink),invalid);
    need(counter(sink,"window-handle")==b&&field->state->window==b&&parents.parent(child)==b,"invalid target preserves live and cached parent");
    need(counter(sink,"window-rejections")==1,"invalid target reported");
    need(gst_app_src_end_of_stream(GST_APP_SRC(input))==GST_FLOW_OK,"replacement EOS submit");
    auto *message=gst_bus_timed_pop_filtered(bus,6*GST_SECOND,GstMessageType(GST_MESSAGE_EOS|GST_MESSAGE_ERROR));
    need(message&&GST_MESSAGE_TYPE(message)==GST_MESSAGE_EOS,"replacement EOS");gst_message_unref(message);
    need(counter(sink,"fields")==2&&!counter(sink,"cancelled"),"both original clocked fields complete");
    need(parents.size(child)==std::array<unsigned,2>{704,396},"stale parent configure cannot restore old size");
    // This extra reference must not defer GPU retirement beyond NULL.
    need(gst_element_set_state(pipeline,GST_STATE_NULL)!=GST_STATE_CHANGE_FAILURE,"replacement NULL");
    for(const char *key:{"allocations","images","retained-buffers","window-handle","failures"})need(!counter(sink,key),"replacement stop retires resources");
    worker.reset();
    need(gst_element_set_state(pipeline,GST_STATE_PLAYING)!=GST_STATE_CHANGE_FAILURE,"replacement reopen");
    std::atomic<bool> request_started{false};
    std::thread setter([&]{request_started=true;gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(sink),0);});
    await([&]{return request_started.load();},"concurrent replacement start");
    need(gst_element_set_state(pipeline,GST_STATE_NULL)!=GST_STATE_CHANGE_FAILURE,"concurrent replacement stop");setter.join();
    for(const char *key:{"allocations","images","retained-buffers","window-handle","failures"})need(!counter(sink,key),"concurrent replacement retirement");
    printf("{\"test\":\"window-pipeline\",\"paused\":true,\"clock_wait_replacement_ms\":%.3f,\"fields\":2,\"stale_geometry\":true,\"stale_close_ignored\":true,\"current_close_forwarded\":true,\"rejected_target_preserved\":true,\"stop_with_reference\":true,\"concurrent_stop_smoke\":true,\"pass\":true}\n",elapsed);
    gst_object_unref(bus);
}
int main(int argc,char **argv) {
    try {
        gst_init(&argc,&argv);need(argc==1,"usage: field-window-test");
        need(temperature()<75000,"below75C");
        need(gst_element_register(nullptr,"crystalhdfieldsink",GST_RANK_NONE,gst_crystalhd_field_get_type()),"register experimental sink");
        pixels();pipeline();gst_deinit();return 0;
    }catch(const std::exception &error){fprintf(stderr,"FAIL: %s\n",error.what());return 1;}
}
