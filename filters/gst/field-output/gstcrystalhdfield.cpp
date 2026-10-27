// SPDX-License-Identifier: GPL-2.0-or-later
// Research-only XCB/EGL consumer of the qualified, immutable GBM field views.
// Deliberately not installed, auto-selected, or connected to the VA ABI.
#include <gst/video/gstvideosink.h>
#include <gst/video/videooverlay.h>
#include <xcb/xcb.h>
#include <sys/eventfd.h>
#include <poll.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include "../../../tests/field-view-gpu.h"
GST_DEBUG_CATEGORY_STATIC(field_debug);
#define GST_CAT_DEFAULT field_debug

namespace crystalhd_field {
struct Counters {
    std::atomic<guint64> pictures{0},fields{0},prerolls{0},redraws{0},swaps{0},
        cancelled{0},clipped{0},failures{0},allocations{0},images{0},retained{0},
        clocked{0},late{0},max_age{0},early{0},max_early{0},dropped{0},
        prepare_ns{0},draw_ns{0},wait_ns{0},swap_ns{0},retire_ns{0},retirements{0},
        worst_field_pts{0},worst_field_index{0},worst_parity{0},worst_prepare_ns{0},
        worst_retire_ns{0},worst_draw_ns{0},worst_wait_ns{0},worst_swap_ns{0},
        worst_monotonic_elapsed_ns{0},worst_clock_elapsed_ns{0},fence_supported{0},
        fences_created{0},fences_destroyed{0},fences_live{0},retire_ready{0},retire_waits{0};
    std::atomic<gint64> worst_head_age_ns{0},worst_preswap_age_ns{0};
};
// Two independent producer allocations let CPU preparation overlap the other
// picture's queued display work. Only the selected slot's fence authorizes
// reuse; a source is never released on SwapBuffers return alone.
struct SourceSlot {
    Counters &stats;
    FieldSyncAPI &sync;
    GbmAllocation storage;
    std::unique_ptr<Image> views[2];
    std::unique_ptr<FieldCompletion> completion;
    GstBuffer *retained=nullptr;
    guint64 generation=0;
    SourceSlot(yuy2_gpu::GPU &gpu,unsigned w,unsigned h,Counters &s,FieldSyncAPI &api)
        :stats(s),sync(api),storage(gpu,w,h){++stats.allocations;}
    void clear_images() noexcept {
        for(auto &view:views)if(view){view->uses_completed=true;view.reset();--stats.images;}
        if(retained){gst_buffer_unref(retained);retained=nullptr;--stats.retained;++stats.retirements;}
    }
    void release_fence() {
        if(completion) {
            need(completion->release(),"destroy retired source fence");
            completion.reset();--stats.fences_live;++stats.fences_destroyed;
        }
    }
    void retire() {
        if(!retained&&!views[0]&&!views[1])return;
        const auto begin=gst_util_get_timestamp();
        if(completion) {
            if(completion->ready())++stats.retire_ready;
            else {++stats.retire_waits;while(!completion->ready(GST_MSECOND)) {}}
        }else glFinish(); // Missing extension or exception before checkpoint.
        clear_images();release_fence();stats.retire_ns+=gst_util_get_timestamp()-begin;
    }
    void checkpoint() {
        // Replaces the fence after every use, including paused redraws and
        // timed reuse of the preroll buffer. The latest fence covers them all.
        release_fence();
        if(sync.supported) {
            completion=std::make_unique<FieldCompletion>(sync);
            ++stats.fences_created;++stats.fences_live;
        }
    }
    void prepare(yuy2_gpu::GPU &gpu,const GstVideoInfo &info,GstBuffer *buffer,
                 guint64 epoch,bool interlaced) {
        retire();GstVideoFrame frame{};
        need(gst_video_frame_map(&frame,&info,buffer,GST_MAP_READ),"input video mapping");
        try {
            const gint stride=GST_VIDEO_FRAME_PLANE_STRIDE(&frame,0);
            need(stride>=info.width*2,"positive YUY2 input stride");
            const auto *data=static_cast<const uint8_t *>(GST_VIDEO_FRAME_PLANE_DATA(&frame,0));
            const auto *base=frame.map[0].data;
            need(base&&data>=base&&size_t(data-base)<=frame.map[0].size,"mapped video plane origin");
            // This exact BO is fresh or has passed retire(). Other slots may
            // still be in use; no context-wide finish is needed for this write.
            storage.write(data,stride,frame.map[0].size-size_t(data-base),true);
        }catch(...){gst_video_frame_unmap(&frame);throw;}
        gst_video_frame_unmap(&frame);
        const EGLint matrix=info.colorimetry.matrix==GST_VIDEO_COLOR_MATRIX_BT601?EGL_ITU_REC601_EXT:EGL_ITU_REC709_EXT;
        views[0]=std::make_unique<Image>(gpu,storage,interlaced,0,matrix,false);++stats.images;
        need(views[0]->texture,"first field import");
        if(interlaced){views[1]=std::make_unique<Image>(gpu,storage,true,1,matrix,false);++stats.images;need(views[1]->texture,"second field import");}
        retained=gst_buffer_ref(buffer);++stats.retained;generation=epoch;
    }
    ~SourceSlot() {
        // Error unwinding may follow an unchecked/failed fence or a draw that
        // never reached checkpoint(). Complete all work before freeing memory.
        if(retained||views[0]||views[1]){glFinish();clear_images();}
        if(completion){if(!completion->release())++stats.failures;completion.reset();--stats.fences_live;++stats.fences_destroyed;}
        --stats.allocations;
    }
};
struct Command {
    GstBuffer *buffer=nullptr;
    GstClock *clock=nullptr;
    GstVideoInfo info{};
    GstSegment segment{};
    guint64 generation=0;
    GstClockTime base=0,latency=0,render_lead=0;
    gint64 offset=0;
    bool preroll=false,done=false;
    GstFlowReturn result=GST_FLOW_OK;
    ~Command() { if(buffer)gst_buffer_unref(buffer);if(clock)gst_object_unref(clock); }
};

// This object and all GPU objects are constructed/destroyed on the worker.
struct Window {
    xcb_connection_t *connection=nullptr;
    xcb_window_t window=0,parent=0;
    xcb_colormap_t colormap=0;
    xcb_atom_t protocols=0,delete_window=0,state=0,fullscreen=0;
    EGLDisplay display=EGL_NO_DISPLAY;
    EGLContext context=EGL_NO_CONTEXT;
    EGLSurface surface=EGL_NO_SURFACE;
    unsigned width=640,height=360;
    bool embedded=false;

    xcb_atom_t atom(const char *name) {
        auto cookie=xcb_intern_atom(connection,0,strlen(name),name);
        auto *reply=xcb_intern_atom_reply(connection,cookie,nullptr);
        need(reply,"XCB atom");xcb_atom_t value=reply->atom;free(reply);return value;
    }
    Window(guintptr handle) {
      try {
        int screen_index=0;
        connection=xcb_connect(nullptr,&screen_index);
        need(connection&&!xcb_connection_has_error(connection),"XCB display connection");
        auto screens=xcb_setup_roots_iterator(xcb_get_setup(connection));
        for(int i=0;i<screen_index&&screens.rem;++i)xcb_screen_next(&screens);
        need(screens.rem,"XCB screen");auto *screen=screens.data;
        embedded=handle!=0;parent=embedded?xcb_window_t(handle):screen->root;
        const char *extensions=eglQueryString(EGL_NO_DISPLAY,EGL_EXTENSIONS);
        need(extensions&&strstr(extensions,"EGL_EXT_platform_xcb"),"EGL XCB platform extension");
        auto get=reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
        auto create=reinterpret_cast<PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC>(eglGetProcAddress("eglCreatePlatformWindowSurfaceEXT"));
        need(get&&create,"EGL platform functions");
        EGLint attrs[]={EGL_PLATFORM_XCB_SCREEN_EXT,screen_index,EGL_NONE};
        display=get(EGL_PLATFORM_XCB_EXT,connection,attrs);
        EGLint major=0,minor=0;need(eglInitialize(display,&major,&minor),"window EGL initialization");
        need(eglBindAPI(EGL_OPENGL_ES_API),"window GLES API");
        EGLint desired[]={EGL_SURFACE_TYPE,EGL_WINDOW_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,
            EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_NONE};
        EGLConfig config{};EGLint count=0,visual=0;
        need(eglChooseConfig(display,desired,&config,1,&count)&&count==1,"window EGL config");
        need(eglGetConfigAttrib(display,config,EGL_NATIVE_VISUAL_ID,&visual),"window visual");
        uint8_t depth=0;
        for(auto depths=xcb_screen_allowed_depths_iterator(screen);depths.rem;xcb_depth_next(&depths))
            for(auto visuals=xcb_depth_visuals_iterator(depths.data);visuals.rem;xcb_visualtype_next(&visuals))
                if(visuals.data->visual_id==xcb_visualid_t(visual))depth=depths.data->depth;
        need(depth,"XCB EGL visual depth");
        if(embedded) {
            auto *geometry=xcb_get_geometry_reply(connection,xcb_get_geometry(connection,parent),nullptr);
            need(geometry,"embedding window geometry");width=geometry->width;height=geometry->height;free(geometry);
            uint32_t mask=XCB_EVENT_MASK_STRUCTURE_NOTIFY;
            xcb_change_window_attributes(connection,parent,XCB_CW_EVENT_MASK,&mask);
        }
        colormap=xcb_generate_id(connection);
        xcb_create_colormap(connection,XCB_COLORMAP_ALLOC_NONE,colormap,screen->root,visual);
        window=xcb_generate_id(connection);
        uint32_t values[]={0,XCB_EVENT_MASK_EXPOSURE|XCB_EVENT_MASK_STRUCTURE_NOTIFY|XCB_EVENT_MASK_KEY_PRESS,colormap};
        auto made=xcb_create_window_checked(connection,depth,window,parent,0,0,width,height,0,
            XCB_WINDOW_CLASS_INPUT_OUTPUT,visual,XCB_CW_BACK_PIXEL|XCB_CW_EVENT_MASK|XCB_CW_COLORMAP,values);
        auto *error=xcb_request_check(connection,made);
        if(error){free(error);need(false,"create EGL window");}
        const char title[]="CrystalHD GPU field output (experimental)";
        const char klass[]="crystalhd-field\0CrystalHD\0";
        xcb_change_property(connection,XCB_PROP_MODE_REPLACE,window,XCB_ATOM_WM_NAME,XCB_ATOM_STRING,8,sizeof(title)-1,title);
        xcb_change_property(connection,XCB_PROP_MODE_REPLACE,window,XCB_ATOM_WM_CLASS,XCB_ATOM_STRING,8,sizeof(klass)-1,klass);
        protocols=atom("WM_PROTOCOLS");delete_window=atom("WM_DELETE_WINDOW");
        state=atom("_NET_WM_STATE");fullscreen=atom("_NET_WM_STATE_FULLSCREEN");
        xcb_change_property(connection,XCB_PROP_MODE_REPLACE,window,protocols,XCB_ATOM_ATOM,32,1,&delete_window);
        xcb_map_window(connection,window);xcb_flush(connection);
        // The EXT API takes the address of an xcb_window_t, not an integer cast.
        surface=create(display,config,&window,nullptr);
        need(surface!=EGL_NO_SURFACE,"EGL XCB window surface");
        EGLint context_attrs[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
        context=eglCreateContext(display,config,EGL_NO_CONTEXT,context_attrs);
        need(context!=EGL_NO_CONTEXT&&eglMakeCurrent(display,surface,surface,context),"window GLES context");
        need(eglSwapInterval(display,1),"local swap interval");
      } catch(...) { cleanup();throw; }
    }
    void cleanup() {
        if(display!=EGL_NO_DISPLAY) {
            eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
            if(context!=EGL_NO_CONTEXT)eglDestroyContext(display,context);
            if(surface!=EGL_NO_SURFACE)eglDestroySurface(display,surface);
            eglTerminate(display);display=EGL_NO_DISPLAY;
        }
        if(connection) {
            if(window)xcb_destroy_window(connection,window);
            if(colormap)xcb_free_colormap(connection,colormap);
            xcb_disconnect(connection);connection=nullptr;
        }
    }
    ~Window(){cleanup();}
    void toggle_fullscreen() {
        if(embedded)return;
        xcb_client_message_event_t event{};event.response_type=XCB_CLIENT_MESSAGE;
        event.window=window;event.type=state;event.format=32;
        event.data.data32[0]=2;event.data.data32[1]=fullscreen;event.data.data32[3]=1;
        auto screens=xcb_setup_roots_iterator(xcb_get_setup(connection));
        xcb_send_event(connection,0,screens.data->root,
            XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT|XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
            reinterpret_cast<const char *>(&event));xcb_flush(connection);
    }
    uint32_t keysym(uint8_t code) {
        auto *reply=xcb_get_keyboard_mapping_reply(connection,xcb_get_keyboard_mapping(connection,code,1),nullptr);
        if(!reply)return 0;
        uint32_t value=reply->keysyms_per_keycode?xcb_get_keyboard_mapping_keysyms(reply)[0]:0;
        free(reply);return value;
    }
};

struct Worker {
    GstElement *owner;
    Counters &stats;
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::shared_ptr<Command>> queue;
    std::thread thread;
    int wake=-1;
    bool ready=false,stopping=false,flushing=false,broken=false;
    guint64 generation=1;
    GstClockID active_wait=nullptr;
    guintptr handle;
    std::string failure;
    std::atomic<bool> expose{false};
    Worker(GstElement *element,Counters &c,guintptr window):owner(element),stats(c),handle(window) {
        wake=eventfd(0,EFD_CLOEXEC|EFD_NONBLOCK);need(wake>=0,"worker eventfd");
        try {thread=std::thread(&Worker::run,this);}catch(...){close(wake);throw;}
    }
    void notify(){uint64_t one=1;ssize_t ignored=write(wake,&one,sizeof(one));(void)ignored;}
    void cancel(bool stop=false) {
        {std::lock_guard<std::mutex> lock(mutex);flushing=true;++generation;stopping|=stop;
         if(active_wait)gst_clock_id_unschedule(active_wait);}
        notify();changed.notify_all();
    }
    ~Worker(){cancel(true);if(thread.joinable())thread.join();close(wake);}
    bool current(guint64 value) {
        std::lock_guard<std::mutex> lock(mutex);
        return !stopping&&!flushing&&!broken&&generation==value;
    }
    GstFlowReturn submit(std::shared_ptr<Command> command) {
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait(lock,[&]{return ready||broken||stopping;});
        if(broken)return GST_FLOW_ERROR;
        if(stopping||flushing)return GST_FLOW_FLUSHING;
        command->generation=generation;queue.push_back(command);notify();
        changed.wait(lock,[&]{return command->done||flushing||stopping||broken;});
        return command->done?command->result:broken?GST_FLOW_ERROR:GST_FLOW_FLUSHING;
    }
    bool target_time(Command &command,GstClockTime running,GstClockTime &target) {
        if(!GST_CLOCK_TIME_IS_VALID(running)||!GST_CLOCK_TIME_IS_VALID(command.base)||
           !GST_CLOCK_TIME_IS_VALID(command.latency)||command.latency>=GST_CLOCK_TIME_NONE-running)return false;
        // BaseSink applies latency and ts-offset in running time, then its
        // render-delay wakes us early. Keep the nominal media target separate
        // from the earlier vsynced swap submission. Clamp before base-time.
        GstClockTime adjusted=running+command.latency;
        if(command.offset<0) {
            guint64 subtract=guint64(-(command.offset+1))+1;
            adjusted=adjusted>subtract?adjusted-subtract:0;
        } else if(guint64(command.offset)<GST_CLOCK_TIME_NONE-adjusted)adjusted+=command.offset;
        else return false;
        if(command.base>=GST_CLOCK_TIME_NONE-adjusted)return false;
        target=command.base+adjusted;return true;
    }
    bool wait(Command &command,GstClockTime running) {
        if(!command.clock)return current(command.generation);
        GstClockTime target=0;
        need(target_time(command,running,target),"valid field clock target");
        // SwapBuffers queues/throttles a vsynced presentation; it is not an
        // instantaneous scanout. Waiting until the media target before calling
        // it adds another refresh wait and can accumulate unbounded A/V lag.
        target=target>command.base+command.render_lead?target-command.render_lead:command.base;
        GstClockID id=gst_clock_new_single_shot_id(command.clock,target);
        {
            std::lock_guard<std::mutex> lock(mutex);
            if(stopping||flushing||generation!=command.generation){gst_clock_id_unref(id);return false;}
            active_wait=id;
        }
        GstClockReturn result=gst_clock_id_wait(id,nullptr);
        {std::lock_guard<std::mutex> lock(mutex);active_wait=nullptr;}
        gst_clock_id_unref(id);
        return (result==GST_CLOCK_OK||result==GST_CLOCK_EARLY)&&current(command.generation);
    }
    void post(const char *name,const char *action=nullptr) {
        auto *fields=gst_structure_new_empty(name);
        if(action)gst_structure_set(fields,"action",G_TYPE_STRING,action,nullptr);
        // A bus sync handler may stop the pipeline even for an application
        // message. Keep such callbacks away from the GL owner/join target.
        gst_element_call_async(owner,[](GstElement *element,gpointer data) {
            gst_element_post_message(element,gst_message_new_application(GST_OBJECT(element),
                gst_structure_copy(static_cast<GstStructure *>(data))));
        },fields,[](gpointer data){gst_structure_free(static_cast<GstStructure *>(data));});
    }
    void run() noexcept {
      try {
        Window window(handle);
        yuy2_gpu::GPU gpu("/dev/dri/renderD128",window.display,window.context);
        FieldSyncAPI sync(gpu.display);stats.fence_supported=sync.supported;
        std::unique_ptr<SourceSlot> slots[2];
        SourceSlot *active=nullptr;
        unsigned next_slot=0;
        std::unique_ptr<Renderer> renderer;
        GstVideoInfo previous{};
        unsigned last_parity=0;
        bool last_interlaced=false;
        auto retire=[&] {
            for(auto &slot:slots)if(slot)slot->retire();
        };
        {std::lock_guard<std::mutex> lock(mutex);ready=true;}changed.notify_all();
        auto draw=[&](unsigned parity,bool interlaced) {
            if(!active||!active->retained||!window.width||!window.height)return;
            glBindFramebuffer(GL_FRAMEBUFFER,0);glViewport(0,0,window.width,window.height);
            glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
            const double aspect=double(previous.width)*previous.par_n/(double(previous.height)*previous.par_d);
            unsigned w=window.width,h=std::max(1U,unsigned(std::lround(w/aspect)));
            if(h>window.height){h=window.height;w=std::max(1U,unsigned(std::lround(h*aspect)));}
            renderer->draw_to(*active->views[interlaced?parity:0],interlaced,parity,0,
                (window.width-w)/2,(window.height-h)/2,w,h,true,!interlaced,false);
        };
        auto swap=[&] {need(eglSwapBuffers(window.display,window.surface),"window swap");++stats.swaps;};
        for(;;) {
            bool redraw=expose.exchange(false);
            while(auto *event=xcb_poll_for_event(window.connection)) {
                const uint8_t type=event->response_type&0x7f;
                if(type==XCB_EXPOSE)redraw=true;
                else if(type==XCB_CONFIGURE_NOTIFY) {
                    auto *configure=reinterpret_cast<xcb_configure_notify_event_t *>(event);
                    if(configure->window==window.window||configure->window==window.parent) {
                        window.width=configure->width;window.height=configure->height;redraw=true;
                        if(window.embedded&&configure->window==window.parent) {
                            uint32_t size[]={window.width,window.height};
                            xcb_configure_window(window.connection,window.window,XCB_CONFIG_WINDOW_WIDTH|XCB_CONFIG_WINDOW_HEIGHT,size);
                            xcb_flush(window.connection);
                        }
                    }
                } else if(type==XCB_CLIENT_MESSAGE) {
                    auto *client=reinterpret_cast<xcb_client_message_event_t *>(event);
                    if(client->type==window.protocols&&client->data.data32[0]==window.delete_window)post("crystalhd-field-close");
                } else if(type==XCB_KEY_PRESS) {
                    auto symbol=window.keysym(reinterpret_cast<xcb_key_press_event_t *>(event)->detail);
                    if(symbol==0xffc8)window.toggle_fullscreen(); // F11
                    else if(symbol==0x20)post("crystalhd-field-key","pause");
                    else if(symbol==0xff51)post("crystalhd-field-key","back");
                    else if(symbol==0xff53)post("crystalhd-field-key","forward");
                    else if(symbol==0x71||symbol==0xff1b)post("crystalhd-field-close");
                } else if(type==XCB_DESTROY_NOTIFY) {
                    auto *destroy=reinterpret_cast<xcb_destroy_notify_event_t *>(event);
                    if(destroy->window==window.window||destroy->window==window.parent)
                        throw std::runtime_error("output window was destroyed");
                }
                free(event);
            }
            need(!xcb_connection_has_error(window.connection),"live XCB connection");
            std::shared_ptr<Command> command;
            guint64 now_generation=0;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if(stopping)break;
                now_generation=generation;
                if(!queue.empty()){command=queue.front();queue.pop_front();}
            }
            if(active&&active->retained&&active->generation!=now_generation)retire();
            if(!command) {
                if(redraw&&active&&active->retained&&current(active->generation)){
                    draw(last_parity,last_interlaced);swap();active->checkpoint();++stats.redraws;
                }
                pollfd fds[]={{wake,POLLIN,0},{xcb_get_file_descriptor(window.connection),POLLIN,0}};
                poll(fds,2,50);uint64_t value;while(read(wake,&value,sizeof(value))>0){}
                continue;
            }
            GstFlowReturn result=GST_FLOW_OK;
            if(!current(command->generation)){result=GST_FLOW_FLUSHING;++stats.cancelled;}
            else {
                auto &info=command->info;
                guint64 preparation_ns=0,retirement_ns=0;
                const bool interlaced=info.interlace_mode==GST_VIDEO_INTERLACE_MODE_INTERLEAVED||
                    (info.interlace_mode==GST_VIDEO_INTERLACE_MODE_MIXED&&
                     GST_BUFFER_FLAG_IS_SET(command->buffer,GST_VIDEO_BUFFER_FLAG_INTERLACED));
                const unsigned first=interlaced&&!GST_BUFFER_FLAG_IS_SET(command->buffer,GST_VIDEO_BUFFER_FLAG_TFF)?1:0;
                if(GST_BUFFER_FLAG_IS_SET(command->buffer,GST_VIDEO_BUFFER_FLAG_ONEFIELD)||
                   GST_BUFFER_FLAG_IS_SET(command->buffer,GST_VIDEO_BUFFER_FLAG_RFF))
                    throw std::runtime_error("one-field/repeat-field input is not qualified");
                if(!gst_video_info_is_equal(&info,&previous)) {
                    retire();for(auto &slot:slots)slot.reset();active=nullptr;next_slot=0;renderer.reset();
                    // Only the window default framebuffer is used; tiny helper FBO.
                    renderer=std::make_unique<Renderer>(info.width,info.height,2,2);previous=info;
                }
                if(!active||command->buffer!=active->retained||active->generation!=command->generation) {
                    const auto prepare_begin=gst_util_get_timestamp();
                    const auto retirement_before=stats.retire_ns.load();
                    const unsigned index=next_slot;next_slot=(next_slot+1)%(sync.supported?2:1);
                    if(!slots[index])slots[index]=std::make_unique<SourceSlot>(gpu,info.width,info.height,stats,sync);
                    active=slots[index].get();active->prepare(gpu,info,command->buffer,command->generation,interlaced);
                    preparation_ns=gst_util_get_timestamp()-prepare_begin;
                    retirement_ns=stats.retire_ns.load()-retirement_before;
                    stats.prepare_ns+=preparation_ns;
                }
                const unsigned count=interlaced?2:1;
                const GstClockTime duration=GST_BUFFER_DURATION_IS_VALID(command->buffer)?GST_BUFFER_DURATION(command->buffer):
                    gst_util_uint64_scale(GST_SECOND,info.fps_d,info.fps_n);
                for(unsigned field=0;field<(command->preroll?1:count);++field) {
                    const unsigned parity=interlaced?first^field:0;
                    GstClockTime field_offset=gst_util_uint64_scale(duration,field,count);
                    need(field_offset<GST_CLOCK_TIME_NONE-GST_BUFFER_PTS(command->buffer),"field PTS extent");
                    GstClockTime pts=GST_BUFFER_PTS(command->buffer)+field_offset;
                    GstClockTime running=gst_segment_to_running_time(&command->segment,GST_FORMAT_TIME,pts);
                    GST_LOG_OBJECT(owner,"field pts=%" G_GUINT64_FORMAT " running=%" G_GUINT64_FORMAT
                        " base=%" G_GUINT64_FORMAT " latency=%" G_GUINT64_FORMAT " preroll=%d now=%" G_GUINT64_FORMAT,
                        pts,running,command->base,command->latency,command->preroll,
                        command->clock?gst_clock_get_time(command->clock):0);
                    if(!GST_CLOCK_TIME_IS_VALID(running)){++stats.clipped;continue;}
                    if(!current(command->generation)){result=GST_FLOW_FLUSHING;++stats.cancelled;break;}
                    GstClockTime phase_clock=0;
                    auto expired=[&] {
                        if(command->preroll||!command->clock)return false;
                        const auto end_pts=pts+gst_util_uint64_scale(duration,1,count);
                        const auto end_running=gst_segment_to_running_time(&command->segment,GST_FORMAT_TIME,end_pts);
                        GstClockTime end_target=0;
                        phase_clock=gst_clock_get_time(command->clock);
                        return target_time(*command,end_running,end_target)&&phase_clock>=end_target;
                    };
                    if(expired()){++stats.dropped;continue;}
                    const auto head_clock=phase_clock;
                    const auto draw_begin=gst_util_get_timestamp();
                    draw(parity,interlaced);
                    const auto draw_elapsed=gst_util_get_timestamp()-draw_begin;
                    stats.draw_ns+=draw_elapsed;
                    const auto wait_begin=gst_util_get_timestamp();
                    if((!command->preroll&&!wait(*command,running))||!current(command->generation)) {
                        result=GST_FLOW_FLUSHING;++stats.cancelled;break;
                    }
                    const auto wait_elapsed=gst_util_get_timestamp()-wait_begin;
                    stats.wait_ns+=wait_elapsed;
                    if(expired()){++stats.dropped;continue;}
                    const auto preswap_clock=phase_clock;
                    const auto swap_begin=gst_util_get_timestamp();
                    swap();const auto swap_end=gst_util_get_timestamp();
                    const auto swap_elapsed=swap_end-swap_begin;
                    stats.swap_ns+=swap_elapsed;
                    last_parity=parity;last_interlaced=interlaced;
                    if(command->preroll)++stats.prerolls;
                    else {
                        ++stats.fields;
                        if(command->clock) {
                            GstClockTime target=0;need(target_time(*command,running,target),"field timing observation");
                            const auto actual=gst_clock_get_time(command->clock);
                            const guint64 age=actual>target?actual-target:0;
                            const guint64 early=actual<target?target-actual:0;
                            if(early)++stats.early;
                            stats.max_early=std::max(stats.max_early.load(),early);
                            GST_LOG_OBJECT(owner,"swap pts=%" G_GUINT64_FORMAT " target=%" G_GUINT64_FORMAT " age=%" G_GUINT64_FORMAT,pts,target,age);
                            ++stats.clocked;
                            if(age>stats.max_age.load()) {
                                // Capture only a new worst event, without per-field
                                // logging. Audio-clock and monotonic deltas distinguish
                                // submission stalls from clock-reporting jumps.
                                stats.worst_field_pts=pts;stats.worst_field_index=field;
                                stats.worst_parity=parity;stats.worst_prepare_ns=preparation_ns;
                                stats.worst_retire_ns=retirement_ns;stats.worst_draw_ns=draw_elapsed;
                                stats.worst_wait_ns=wait_elapsed;stats.worst_swap_ns=swap_elapsed;
                                stats.worst_head_age_ns=GST_CLOCK_DIFF(target,head_clock);
                                stats.worst_preswap_age_ns=GST_CLOCK_DIFF(target,preswap_clock);
                                stats.worst_clock_elapsed_ns=actual-head_clock;
                                stats.worst_monotonic_elapsed_ns=swap_end-draw_begin;
                                stats.max_age=age;
                            }
                            if(age>duration/(count*command->segment.rate))++stats.late;
                        }
                    }
                }
                active->checkpoint();
                if(!command->preroll&&result==GST_FLOW_OK)++stats.pictures;
            }
            {std::lock_guard<std::mutex> lock(mutex);command->result=result;command->done=true;}changed.notify_all();
        }
        retire();renderer.reset();for(auto &slot:slots)slot.reset();
      } catch(const std::exception &error) {
        ++stats.failures;
        {std::lock_guard<std::mutex> lock(mutex);failure=error.what();broken=true;}changed.notify_all();
        // Never run an application's synchronous ERROR handler on our GL thread:
        // it may set NULL and join this worker. Notify through a GStreamer task.
        gst_element_call_async(owner,[](GstElement *element,gpointer data) {
            GST_ELEMENT_ERROR(element,RESOURCE,FAILED,("%s",static_cast<char *>(data)),(nullptr));
        },g_strdup(error.what()),g_free);
      }
    }
};
struct State {
    std::mutex mutex;
    std::shared_ptr<Worker> worker;
    GstVideoInfo info{};
    guintptr window=0;
    Counters counters;
};
}

using crystalhd_field::State;
using crystalhd_field::Worker;
using crystalhd_field::Command;
typedef struct _GstCrystalhdField {GstVideoSink parent;State *state;} GstCrystalhdField;
typedef struct _GstCrystalhdFieldClass {GstVideoSinkClass parent;} GstCrystalhdFieldClass;
static void overlay_init(GstVideoOverlayInterface *iface);
G_DEFINE_TYPE_WITH_CODE(GstCrystalhdField,gst_crystalhd_field,GST_TYPE_VIDEO_SINK,
    G_IMPLEMENT_INTERFACE(GST_TYPE_VIDEO_OVERLAY,overlay_init))
enum {PROP_0,PROP_STATS};

static std::shared_ptr<Worker> get_worker(GstCrystalhdField *self) {
    std::lock_guard<std::mutex> lock(self->state->mutex);return self->state->worker;
}
static gboolean start(GstBaseSink *sink) {
    auto *self=reinterpret_cast<GstCrystalhdField *>(sink);
    gst_video_overlay_prepare_window_handle(GST_VIDEO_OVERLAY(self));
    try {
        std::lock_guard<std::mutex> lock(self->state->mutex);
        self->state->worker=std::make_shared<Worker>(GST_ELEMENT(self),self->state->counters,self->state->window);
        return TRUE;
    }catch(const std::exception &error){GST_ELEMENT_ERROR(self,RESOURCE,OPEN_READ_WRITE,("%s",error.what()),(nullptr));return FALSE;}
}
static gboolean stop(GstBaseSink *sink) {
    auto *self=reinterpret_cast<GstCrystalhdField *>(sink);std::shared_ptr<Worker> worker;
    {std::lock_guard<std::mutex> lock(self->state->mutex);worker.swap(self->state->worker);}
    if(worker)worker->cancel(true);
    worker.reset();return TRUE;
}
static gboolean unlock(GstBaseSink *sink) {
    auto worker=get_worker(reinterpret_cast<GstCrystalhdField *>(sink));if(worker)worker->cancel();return TRUE;
}
static gboolean unlock_stop(GstBaseSink *sink) {
    auto worker=get_worker(reinterpret_cast<GstCrystalhdField *>(sink));
    if(worker){std::lock_guard<std::mutex> lock(worker->mutex);worker->flushing=false;}return TRUE;
}
static gboolean set_caps(GstBaseSink *sink,GstCaps *caps) {
    auto *self=reinterpret_cast<GstCrystalhdField *>(sink);GstVideoInfo info{};
    if(!gst_video_info_from_caps(&info,caps)||GST_VIDEO_INFO_FORMAT(&info)!=GST_VIDEO_FORMAT_YUY2||info.fps_n<=0||info.fps_d<=0||
       info.width<2||info.height<2||(info.width&1)||(info.height&1)||
       info.width>1920||info.height>1088||info.par_n<=0||info.par_d<=0||
       info.colorimetry.range!=GST_VIDEO_COLOR_RANGE_16_235||
       (info.colorimetry.matrix!=GST_VIDEO_COLOR_MATRIX_BT709&&info.colorimetry.matrix!=GST_VIDEO_COLOR_MATRIX_BT601))return FALSE;
    std::lock_guard<std::mutex> lock(self->state->mutex);self->state->info=info;return TRUE;
}
static GstFlowReturn output(GstBaseSink *sink,GstBuffer *buffer,bool preroll) {
    auto *self=reinterpret_cast<GstCrystalhdField *>(sink);
    if(!GST_BUFFER_PTS_IS_VALID(buffer)||sink->segment.format!=GST_FORMAT_TIME||sink->segment.rate<=0)return GST_FLOW_NOT_NEGOTIATED;
    auto command=std::make_shared<Command>();command->buffer=gst_buffer_ref(buffer);command->preroll=preroll;
    {std::lock_guard<std::mutex> lock(self->state->mutex);command->info=self->state->info;}
    command->segment=sink->segment;
    if(!preroll&&gst_base_sink_get_sync(sink))command->clock=gst_element_get_clock(GST_ELEMENT(sink));
    command->base=gst_element_get_base_time(GST_ELEMENT(sink));
    command->latency=gst_base_sink_get_latency(sink);command->offset=gst_base_sink_get_ts_offset(sink);
    command->render_lead=gst_base_sink_get_render_delay(sink);
    auto worker=get_worker(self);return worker?worker->submit(command):GST_FLOW_FLUSHING;
}
static GstFlowReturn render(GstBaseSink *sink,GstBuffer *buffer){return output(sink,buffer,false);}
static GstFlowReturn preroll(GstBaseSink *sink,GstBuffer *buffer){return output(sink,buffer,true);}
static void get_times(GstBaseSink *sink,GstBuffer *buffer,GstClockTime *start_time,GstClockTime *end_time) {
    *start_time=GST_BUFFER_PTS(buffer);*end_time=GST_CLOCK_TIME_NONE;
    if(!GST_CLOCK_TIME_IS_VALID(*start_time))return;
    GstClockTime duration=GST_BUFFER_DURATION(buffer);
    if(!GST_CLOCK_TIME_IS_VALID(duration)) {
        auto *self=reinterpret_cast<GstCrystalhdField *>(sink);
        std::lock_guard<std::mutex> lock(self->state->mutex);
        const auto &info=self->state->info;
        if(info.fps_n>0&&info.fps_d>0)duration=gst_util_uint64_scale(GST_SECOND,info.fps_d,info.fps_n);
    }
    if(GST_CLOCK_TIME_IS_VALID(duration)&&duration<GST_CLOCK_TIME_NONE-*start_time)*end_time=*start_time+duration;
}
static void set_window(GstVideoOverlay *overlay,guintptr window) {
    auto *self=reinterpret_cast<GstCrystalhdField *>(overlay);
    std::lock_guard<std::mutex> lock(self->state->mutex);
    if(self->state->worker){GST_WARNING_OBJECT(self,"set window before READY; live replacement is unqualified");return;}
    self->state->window=window;
}
static void expose(GstVideoOverlay *overlay) {
    auto worker=get_worker(reinterpret_cast<GstCrystalhdField *>(overlay));
    if(worker){worker->expose=true;worker->notify();}
}
static void overlay_init(GstVideoOverlayInterface *iface){iface->set_window_handle=set_window;iface->expose=expose;}
static void get_property(GObject *object,guint id,GValue *value,GParamSpec *spec) {
    auto *self=reinterpret_cast<GstCrystalhdField *>(object);
    if(id!=PROP_STATS){G_OBJECT_WARN_INVALID_PROPERTY_ID(object,id,spec);return;}
    auto &c=self->state->counters;
    auto *stats=gst_structure_new("crystalhd-field-stats",
        "pictures",G_TYPE_UINT64,c.pictures.load(),"fields",G_TYPE_UINT64,c.fields.load(),
        "prerolls",G_TYPE_UINT64,c.prerolls.load(),"redraws",G_TYPE_UINT64,c.redraws.load(),
        "swaps",G_TYPE_UINT64,c.swaps.load(),"cancelled",G_TYPE_UINT64,c.cancelled.load(),
        "clipped",G_TYPE_UINT64,c.clipped.load(),"failures",G_TYPE_UINT64,c.failures.load(),
        "clocked-fields",G_TYPE_UINT64,c.clocked.load(),"late-fields",G_TYPE_UINT64,c.late.load(),
        "max-age-ns",G_TYPE_UINT64,c.max_age.load(),
        "early-fields",G_TYPE_UINT64,c.early.load(),"max-early-ns",G_TYPE_UINT64,c.max_early.load(),
        "dropped-fields",G_TYPE_UINT64,c.dropped.load(),
        "prepare-ns",G_TYPE_UINT64,c.prepare_ns.load(),"draw-ns",G_TYPE_UINT64,c.draw_ns.load(),
        "wait-ns",G_TYPE_UINT64,c.wait_ns.load(),"swap-ns",G_TYPE_UINT64,c.swap_ns.load(),
        "retire-ns",G_TYPE_UINT64,c.retire_ns.load(),"retirements",G_TYPE_UINT64,c.retirements.load(),
        "worst-field-pts",G_TYPE_UINT64,c.worst_field_pts.load(),
        "worst-field-index",G_TYPE_UINT64,c.worst_field_index.load(),
        "worst-parity",G_TYPE_UINT64,c.worst_parity.load(),
        "worst-prepare-ns",G_TYPE_UINT64,c.worst_prepare_ns.load(),
        "worst-retire-ns",G_TYPE_UINT64,c.worst_retire_ns.load(),
        "worst-draw-ns",G_TYPE_UINT64,c.worst_draw_ns.load(),
        "worst-wait-ns",G_TYPE_UINT64,c.worst_wait_ns.load(),
        "worst-swap-ns",G_TYPE_UINT64,c.worst_swap_ns.load(),
        "worst-head-age-ns",G_TYPE_INT64,c.worst_head_age_ns.load(),
        "worst-preswap-age-ns",G_TYPE_INT64,c.worst_preswap_age_ns.load(),
        "worst-monotonic-elapsed-ns",G_TYPE_UINT64,c.worst_monotonic_elapsed_ns.load(),
        "worst-clock-elapsed-ns",G_TYPE_UINT64,c.worst_clock_elapsed_ns.load(),
        "fence-supported",G_TYPE_UINT64,c.fence_supported.load(),
        "fences-created",G_TYPE_UINT64,c.fences_created.load(),
        "fences-destroyed",G_TYPE_UINT64,c.fences_destroyed.load(),
        "fences-live",G_TYPE_UINT64,c.fences_live.load(),
        "retire-ready",G_TYPE_UINT64,c.retire_ready.load(),
        "retire-waits",G_TYPE_UINT64,c.retire_waits.load(),
        "allocations",G_TYPE_UINT64,c.allocations.load(),"images",G_TYPE_UINT64,c.images.load(),
        "retained-buffers",G_TYPE_UINT64,c.retained.load(),nullptr);
    g_value_take_boxed(value,stats);
}
static void finalize(GObject *object) {
    auto *self=reinterpret_cast<GstCrystalhdField *>(object);stop(GST_BASE_SINK(self));delete self->state;
    G_OBJECT_CLASS(gst_crystalhd_field_parent_class)->finalize(object);
}
static void gst_crystalhd_field_class_init(GstCrystalhdFieldClass *klass) {
    GST_DEBUG_CATEGORY_INIT(field_debug,"crystalhdfield",0,"Experimental GPU field output");
    auto *object=G_OBJECT_CLASS(klass);auto *element=GST_ELEMENT_CLASS(klass);auto *base=GST_BASE_SINK_CLASS(klass);
    object->get_property=get_property;object->finalize=finalize;
    g_object_class_install_property(object,PROP_STATS,g_param_spec_boxed("field-stats","Field statistics",
        "Swap submissions and fenced source retirements, not scanout; live snapshots may span a transition",
        GST_TYPE_STRUCTURE,GParamFlags(G_PARAM_READABLE|G_PARAM_STATIC_STRINGS)));
    gst_element_class_set_static_metadata(element,"Experimental CrystalHD GPU fields","Sink/Video",
        "Owned linear YUY2 field views with pipeline-clocked XCB/EGL output","CrystalHD contributors");
    static GstStaticPadTemplate pad=GST_STATIC_PAD_TEMPLATE("sink",GST_PAD_SINK,GST_PAD_ALWAYS,
        GST_STATIC_CAPS("video/x-raw, format=(string)YUY2, width=(int)[2,1920], height=(int)[2,1088], "
                        "framerate=(fraction)[1/1,60/1], interlace-mode=(string){progressive,interleaved,mixed}"));
    gst_element_class_add_static_pad_template(element,&pad);
    base->start=start;base->stop=stop;base->unlock=unlock;base->unlock_stop=unlock_stop;
    base->set_caps=set_caps;base->preroll=preroll;base->render=render;base->get_times=get_times;
}
static void gst_crystalhd_field_init(GstCrystalhdField *self) {
    self->state=new State;
    gst_base_sink_set_render_delay(GST_BASE_SINK(self),15*GST_MSECOND);
    gst_base_sink_set_max_lateness(GST_BASE_SINK(self),20*GST_MSECOND);
    // One input picture owns two presentation deadlines. Whole-picture QoS
    // feedback can make upstream raw converters discard a still-usable field
    // pair. Keep this sink's max-lateness and per-field expiry, but let each
    // decoded pair reach its actual field-deadline owner before dropping it.
    gst_base_sink_set_qos_enabled(GST_BASE_SINK(self),FALSE);
    gst_base_sink_set_last_sample_enabled(GST_BASE_SINK(self),FALSE);
}
static gboolean plugin_init(GstPlugin *plugin) {
    return gst_element_register(plugin,"crystalhdfieldsink",GST_RANK_NONE,gst_crystalhd_field_get_type());
}
#ifndef PACKAGE
#define PACKAGE "crystalhd-field-research"
#endif
GST_PLUGIN_DEFINE(GST_VERSION_MAJOR,GST_VERSION_MINOR,crystalhdfield,
    "Experimental GPU field output",plugin_init,"0.1","GPL",PACKAGE,"https://github.com/ahnhy1324/crystalhd")
