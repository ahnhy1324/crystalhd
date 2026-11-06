// SPDX-License-Identifier: GPL-2.0-or-later
// Research-only XCB/EGL consumer of the qualified, immutable GBM field views.
// Deliberately not installed, auto-selected, or connected to the VA ABI.
#include <gst/video/gstvideosink.h>
#include <gst/video/videooverlay.h>
#include <xcb/xcb.h>
#include <xcb/xcbext.h>
#include <sys/eventfd.h>
#include <poll.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include "../../../tests/field-view-gpu.h"
#define XV_TIMEOUT_US G_USEC_PER_SEC
#define CRYSTALHD_X11_NONCE_FIRST UINT32_C(0xc0000000)
#define CRYSTALHD_X11_NONCE_LAST UINT32_C(0xffffffff)
#include "../xv-output/xv-connect.h"
#undef CRYSTALHD_X11_NONCE_LAST
#undef CRYSTALHD_X11_NONCE_FIRST
#undef XV_TIMEOUT_US
GST_DEBUG_CATEGORY_STATIC(field_debug);
#define GST_CAT_DEFAULT field_debug

namespace crystalhd_field {
static size_t mapped_plane_available(const GstVideoFrame &frame,unsigned plane) {
    need(plane<GST_VIDEO_MAX_PLANES,"video plane index");
    const auto origin=reinterpret_cast<uintptr_t>(GST_VIDEO_FRAME_PLANE_DATA(&frame,plane));
    size_t available=0;
    for(const auto &map:frame.map) {
        const auto base=reinterpret_cast<uintptr_t>(map.data);
        if(base&&origin>=base&&origin-base<map.size)
            available=std::max(available,map.size-size_t(origin-base));
    }
    need(available,"mapped video plane origin");return available;
}
struct Counters {
    std::atomic<guint64> pictures{0},fields{0},prerolls{0},redraws{0},swaps{0},
        cancelled{0},clipped{0},failures{0},allocations{0},images{0},retained{0},
        clocked{0},late{0},max_age{0},early{0},max_early{0},dropped{0},
        prepare_ns{0},draw_ns{0},wait_ns{0},swap_ns{0},retire_ns{0},retirements{0},
        worst_field_pts{0},worst_field_index{0},worst_parity{0},worst_swap_sequence{0},worst_prepare_ns{0},
        worst_retire_ns{0},worst_draw_ns{0},worst_wait_ns{0},worst_swap_ns{0},
        worst_monotonic_elapsed_ns{0},worst_clock_elapsed_ns{0},
        window_replacements{0},window_rejections{0},window_handle{0};
    std::atomic<gint64> worst_head_age_ns{0},worst_preswap_age_ns{0};
};
// The opaque video draw overwrites its entire rectangle. Clear only the bars;
// clearing the video rectangle first adds a redundant full-rate GPU write.
// Every window pixel is still written on every frame, including after resize.
static void clear_borders(unsigned width,unsigned height,unsigned x,unsigned y,
                          unsigned w,unsigned h) {
    need(w&&h&&x<=width&&y<=height&&w<=width-x&&h<=height-y,"video rectangle extent");
    glClearColor(0,0,0,1);glEnable(GL_SCISSOR_TEST);
    auto clear=[](unsigned left,unsigned bottom,unsigned cw,unsigned ch) {
        if(cw&&ch){glScissor(left,bottom,cw,ch);glClear(GL_COLOR_BUFFER_BIT);}
    };
    clear(0,0,width,y);clear(0,y+h,width,height-y-h);
    clear(0,y,x,h);clear(x+w,y,width-x-w,h);
    glDisable(GL_SCISSOR_TEST);
}
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
struct WindowCommand {
    guintptr handle=0;
    bool done=false,accepted=false;
};

template<class T> using XcbReply=std::unique_ptr<T,decltype(&free)>;
// Retire pending sequences even when a later request/wait fails. The reply
// deadline starts after flush; congested writes and EGL remain separate.
struct ReplyRequest {
    xcb_connection_t *connection;
    unsigned sequence;
    bool pending=true;
    ReplyRequest(xcb_connection_t *c,unsigned s):connection(c),sequence(s){}
    ReplyRequest(const ReplyRequest &)=delete;
    ReplyRequest &operator=(const ReplyRequest &)=delete;
    ~ReplyRequest(){if(pending)xcb_discard_reply(connection,sequence);}
    template<class T> XcbReply<T> wait(gint64 end,const char *operation) {
        auto require=[&](bool valid,const char *reason) {
            if(!valid){const auto message=std::string(operation)+reason;need(false,message.c_str());}
        };
        for(;;) {
            void *raw=nullptr;xcb_generic_error_t *raw_error=nullptr;
            const bool ready=xcb_poll_for_reply(connection,sequence,&raw,&raw_error);
            XcbReply<T> reply(static_cast<T *>(raw),&free);
            XcbReply<xcb_generic_error_t> error(raw_error,&free);
            if(ready) {
                pending=false;
                require(g_get_monotonic_time()<end," reply timed out");
                require(!error&&reply," reply failed");
                return reply;
            }
            require(!xcb_connection_has_error(connection)," connection failed");
            const auto left=end-g_get_monotonic_time();
            require(left>0," reply timed out");
            pollfd event{xcb_get_file_descriptor(connection),POLLIN,0};
            const auto available=poll(&event,1,int((left+999)/1000));
            if(available<0&&errno==EINTR)continue;
            require(available>=0&&!(event.revents&POLLNVAL)," reply poll failed");
            // Retry XCB first: a reply may already be buffered alongside HUP.
        }
    }
};
static xcb_atom_t intern_atom_bounded(xcb_connection_t *connection,const char *name,gint64 *shared_end=nullptr) {
    ReplyRequest request(connection,xcb_intern_atom(connection,0,strlen(name),name).sequence);
    need(xcb_flush(connection)>0,"XCB atom request flush failed");
    const auto end=g_get_monotonic_time()+G_USEC_PER_SEC;
    if(shared_end&&!*shared_end)*shared_end=end;
    return request.wait<xcb_intern_atom_reply_t>(shared_end?*shared_end:end,"XCB atom")->atom;
}
static uint32_t keyboard_mapping_bounded(xcb_connection_t *connection,uint8_t code) {
    ReplyRequest request(connection,xcb_get_keyboard_mapping(connection,code,1).sequence);
    need(xcb_flush(connection)>0,"XCB keyboard mapping request flush failed");
    const auto end=g_get_monotonic_time()+G_USEC_PER_SEC;
    auto reply=request.wait<xcb_get_keyboard_mapping_reply_t>(end,"XCB keyboard mapping");
    // One requested keycode has keysyms_per_keycode trailing CARD32 values.
    // An empty mapping is valid; a nonempty count cannot exceed its payload.
    need(reply->length>=reply->keysyms_per_keycode,"XCB keyboard mapping reply extent");
    return reply->keysyms_per_keycode?xcb_get_keyboard_mapping_keysyms(reply.get())[0]:0;
}
struct WindowGeometry {unsigned width,height,child_width,child_height;gint64 deadline;};
static WindowGeometry current_geometry_bounded(xcb_connection_t *connection,xcb_window_t parent,xcb_window_t child) {
    ReplyRequest parent_request(connection,xcb_get_geometry(connection,parent).sequence);
    ReplyRequest child_request(connection,xcb_get_geometry(connection,child).sequence);
    need(xcb_flush(connection)>0,"XCB geometry request flush failed");
    const auto end=g_get_monotonic_time()+G_USEC_PER_SEC;
    auto geometry=parent_request.wait<xcb_get_geometry_reply_t>(end,"XCB parent geometry");
    auto video=child_request.wait<xcb_get_geometry_reply_t>(end,"XCB child geometry");
    return {geometry->width,geometry->height,video->width,video->height,end};
}
static void resize_current_bounded(xcb_connection_t *connection,xcb_window_t parent,xcb_window_t child,
                                   unsigned &width,unsigned &height) {
    const auto geometry=current_geometry_bounded(connection,parent,child);
    const auto end=geometry.deadline;
    if(geometry.child_width!=geometry.width||geometry.child_height!=geometry.height) {
        need(g_get_monotonic_time()<end,"XCB resize completion timed out");
        uint32_t size[]={geometry.width,geometry.height};
        const auto configured=xcb_configure_window_checked(connection,child,
            XCB_CONFIG_WINDOW_WIDTH|XCB_CONFIG_WINDOW_HEIGHT,size);
        ReplyRequest configure_request(connection,configured.sequence);
        ReplyRequest barrier(connection,xcb_get_input_focus(connection).sequence);
        need(xcb_flush(connection)>0,"XCB resize completion flush failed");
        auto completed=barrier.wait<xcb_get_input_focus_reply_t>(end,"XCB resize completion");
        need(!xcb_connection_has_error(connection),"current video child connection");
        // Once a subsequent reply has arrived, xcb_request_check() consumes
        // any queued error without another network sync (xcb.h contract).
        XcbReply<xcb_generic_error_t> error(xcb_request_check(connection,configured),&free);
        configure_request.pending=false;
        need(!error,"current video child resize");
        need(g_get_monotonic_time()<end,"XCB resize completion timed out");
    }
    need(!xcb_connection_has_error(connection),"current video child connection");
    width=geometry.width;height=geometry.height;
}

// A later reply proves that a checked request was processed. Error retrieval
// then consumes an already queued result without an implicit network sync.
static XcbReply<xcb_generic_error_t> checked_request_bounded(xcb_connection_t *connection,
        xcb_void_cookie_t cookie,gint64 end,const char *operation,gint64 *shared_end=nullptr) {
    ReplyRequest checked(connection,cookie.sequence);
    ReplyRequest barrier(connection,xcb_get_input_focus(connection).sequence);
    need(xcb_flush(connection)>0,"XCB checked completion flush failed");
    if(shared_end) {
        if(!*shared_end)*shared_end=g_get_monotonic_time()+G_USEC_PER_SEC;
        end=*shared_end;
    }
    auto completed=barrier.wait<xcb_get_input_focus_reply_t>(end,operation);
    need(!xcb_connection_has_error(connection),"XCB checked completion connection failed");
    XcbReply<xcb_generic_error_t> error(xcb_request_check(connection,cookie),&free);
    checked.pending=false;
    need(!xcb_connection_has_error(connection),"XCB checked completion connection failed");
    need(g_get_monotonic_time()<end,(std::string(operation)+" reply timed out").c_str());
    return error;
}
// All constructor response waits share one deadline after the first request
// flush. XID refill, congested writes and EGL execution remain outside this
// response-wait bound. Existing handoff deadlines are passed independently.
struct ConstructionReplies {
    xcb_connection_t *connection;
    gint64 deadline=0;
    void flush() {
        need(xcb_flush(connection)>0,"XCB construction request flush failed");
        if(!deadline)deadline=g_get_monotonic_time()+G_USEC_PER_SEC;
    }
    XcbReply<xcb_get_geometry_reply_t> geometry(xcb_window_t parent) {
        ReplyRequest request(connection,xcb_get_geometry(connection,parent).sequence);
        flush();
        return request.wait<xcb_get_geometry_reply_t>(deadline,"XCB construction geometry");
    }
    XcbReply<xcb_generic_error_t> check(xcb_void_cookie_t cookie,const char *operation) {
        return checked_request_bounded(connection,cookie,deadline,operation,&deadline);
    }
    xcb_atom_t atom(const char *name) {
        return intern_atom_bounded(connection,name,&deadline);
    }
};
static bool inside_bounded(xcb_connection_t *connection,xcb_window_t target,
        xcb_window_t ancestor,xcb_window_t root,gint64 end) {
    // Reject a descendant before destroying its old owned wrapper. All hops
    // share the caller's deadline, including an unreasonably deep tree.
    for(unsigned depth=0;target&&target!=root;++depth) {
        if(target==ancestor||depth==256)return true;
        ReplyRequest request(connection,xcb_query_tree(connection,target).sequence);
        need(xcb_flush(connection)>0,"XCB handoff ancestry flush failed");
        auto tree=request.wait<xcb_query_tree_reply_t>(end,"XCB handoff ancestry");
        if(tree->root!=root)return true;
        target=tree->parent;
    }
    return false;
}
template<class CreateStandalone>
static bool replace_window_bounded(xcb_connection_t *connection,xcb_window_t window,
        guintptr handle,xcb_window_t &parent,xcb_window_t &standalone,bool &embedded,
        unsigned &width,unsigned &height,CreateStandalone create_standalone) {
    if(handle>G_MAXUINT32||handle==window||(standalone&&handle==standalone))return false;
    if(!handle&&!embedded)return true;
    xcb_window_t candidate=0,target=0;
    unsigned next_width=width,next_height=height;
    gint64 end=0;bool create_started=false;
    auto reject=[&] {
        if(candidate){xcb_destroy_window(connection,candidate);xcb_flush(connection);}
        return false;
    };
    try {
        // The window's actual root supports nondefault screens. Starting the
        // response budget here covers the explicit query/completion waits.
        // XID replenishment inside a standalone allocator remains separate.
        ReplyRequest own_request(connection,xcb_get_geometry(connection,window).sequence);
        need(xcb_flush(connection)>0,"XCB handoff own geometry flush failed");
        end=g_get_monotonic_time()+G_USEC_PER_SEC;
        auto own=own_request.wait<xcb_get_geometry_reply_t>(end,"XCB handoff own geometry");
        const auto root=own->root;
        if(handle==root||(handle&&standalone&&
                inside_bounded(connection,xcb_window_t(handle),standalone,root,end)))return false;
        if(g_get_monotonic_time()>=end)return false;
        if(!handle) {
            create_started=true;
            candidate=create_standalone(root,end);
            need(candidate,"XCB handoff standalone creation failed");
        }
        target=handle?xcb_window_t(handle):candidate;
        if(target==parent&&embedded==(handle!=0))return true;
        ReplyRequest target_request(connection,xcb_get_geometry(connection,target).sequence);
        need(xcb_flush(connection)>0,"XCB handoff target geometry flush failed");
        auto geometry=target_request.wait<xcb_get_geometry_reply_t>(end,"XCB handoff target geometry");
        if(geometry->root!=root||!geometry->width||!geometry->height)return reject();
        next_width=handle?geometry->width:width;
        next_height=handle?geometry->height:height;
    } catch(const std::exception &) {
        // No child mutation was sent during ordinary preflight. A live
        // connection can retain its old attachment after a rejected query.
        // Candidate creation or a lost connection instead needs worker cleanup.
        if(create_started||xcb_connection_has_error(connection))throw;
        return false;
    }
    if(g_get_monotonic_time()>=end)return reject();
    auto error=checked_request_bounded(connection,
        xcb_reparent_window_checked(connection,window,target,0,0),end,"XCB handoff reparent");
    if(error)return reject(); // An explicit protocol error did not move the child.
    // Once reparenting was sent, a missing completion cannot safely be called
    // a rejection with the old attachment intact. Let the worker close instead.
    uint32_t size[]={next_width,next_height};
    error=checked_request_bounded(connection,xcb_configure_window_checked(connection,window,
        XCB_CONFIG_WINDOW_WIDTH|XCB_CONFIG_WINDOW_HEIGHT,size),end,"XCB handoff configure");
    need(!error,"XCB handoff configure failed");
    if(embedded) {
        uint32_t mask=0;
        error=checked_request_bounded(connection,xcb_change_window_attributes_checked(connection,
            parent,XCB_CW_EVENT_MASK,&mask),end,"XCB handoff old parent");
        // An explicitly rejected old-parent unsubscribe is harmless: that
        // parent can already have been retired. A missing completion still throws.
    }
    need(!xcb_connection_has_error(connection),"XCB handoff connection failed");
    if(handle) {
        uint32_t mask=XCB_EVENT_MASK_STRUCTURE_NOTIFY;
        xcb_change_window_attributes(connection,target,XCB_CW_EVENT_MASK,&mask);
    }
    xcb_map_window(connection,window);
    if(candidate)xcb_map_window(connection,candidate);
    if(standalone)xcb_destroy_window(connection,standalone);
    need(xcb_flush(connection)>0,"XCB handoff final flush failed");
    need(!xcb_connection_has_error(connection),"XCB handoff connection failed");
    // Commit the public binding only after all required completion checks.
    standalone=candidate;parent=target;embedded=handle!=0;
    width=next_width;height=next_height;
    return true;
}

// This object and all GPU objects are constructed/destroyed on the worker.
struct Window {
    xcb_connection_t *connection=nullptr;
    xcb_window_t window=0,parent=0,standalone=0;
    xcb_colormap_t colormap=0;
    xcb_atom_t protocols=0,delete_window=0,state=0,fullscreen=0;
    EGLDisplay display=EGL_NO_DISPLAY;
    EGLContext context=EGL_NO_CONTEXT;
    EGLSurface surface=EGL_NO_SURFACE;
    unsigned width=640,height=360;
    bool embedded=false;

    Window(guintptr handle) {
      try {
        int screen_index=0;
        const char *failure=nullptr;
        connection=connect_local_display(nullptr,&screen_index,&failure);
        need(connection&&!xcb_connection_has_error(connection),failure);
        auto screens=xcb_setup_roots_iterator(xcb_get_setup(connection));
        for(int i=0;i<screen_index&&screens.rem;++i)xcb_screen_next(&screens);
        need(screens.rem,"XCB screen");auto *screen=screens.data;
        embedded=handle!=0;
        need(handle<=G_MAXUINT32&&(!embedded||handle!=screen->root),"valid embedding window, use0 for standalone");
        parent=embedded?xcb_window_t(handle):screen->root;
        const char *extensions=eglQueryString(EGL_NO_DISPLAY,EGL_EXTENSIONS);
        need(extensions&&strstr(extensions,"EGL_EXT_platform_xcb"),"EGL XCB platform extension");
        auto get=reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
        auto create_surface=reinterpret_cast<PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC>(eglGetProcAddress("eglCreatePlatformWindowSurfaceEXT"));
        need(get&&create_surface,"EGL platform functions");
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
        ConstructionReplies replies{connection};
        if(embedded) {
            auto geometry=replies.geometry(parent);
            need(bool(geometry),"embedding window geometry");width=geometry->width;height=geometry->height;
            uint32_t mask=XCB_EVENT_MASK_STRUCTURE_NOTIFY;
            xcb_change_window_attributes(connection,parent,XCB_CW_EVENT_MASK,&mask);
        }
        colormap=xcb_generate_id(connection);
        xcb_create_colormap(connection,XCB_COLORMAP_ALLOC_NONE,colormap,screen->root,visual);
        window=xcb_generate_id(connection);
        uint32_t values[]={0,XCB_EVENT_MASK_EXPOSURE|XCB_EVENT_MASK_STRUCTURE_NOTIFY|XCB_EVENT_MASK_KEY_PRESS,colormap};
        auto made=xcb_create_window_checked(connection,depth,window,parent,0,0,width,height,0,
            XCB_WINDOW_CLASS_INPUT_OUTPUT,visual,XCB_CW_BACK_PIXEL|XCB_CW_EVENT_MASK|XCB_CW_COLORMAP,values);
        auto error=replies.check(made,"XCB construction create");
        need(!error,"create EGL window");
        const char title[]="CrystalHD GPU field output (experimental)";
        const char klass[]="crystalhd-field\0CrystalHD\0";
        xcb_change_property(connection,XCB_PROP_MODE_REPLACE,window,XCB_ATOM_WM_NAME,XCB_ATOM_STRING,8,sizeof(title)-1,title);
        xcb_change_property(connection,XCB_PROP_MODE_REPLACE,window,XCB_ATOM_WM_CLASS,XCB_ATOM_STRING,8,sizeof(klass)-1,klass);
        protocols=replies.atom("WM_PROTOCOLS");delete_window=replies.atom("WM_DELETE_WINDOW");
        state=replies.atom("_NET_WM_STATE");fullscreen=replies.atom("_NET_WM_STATE_FULLSCREEN");
        xcb_change_property(connection,XCB_PROP_MODE_REPLACE,window,protocols,XCB_ATOM_ATOM,32,1,&delete_window);
        if(!embedded) {
            // Keep the EGL child out of the WM's client set. Otherwise a late
            // WM configure/unmanage can resize it after a live reparent.
            standalone=make_standalone(screen->root,0,&replies);parent=standalone;
            error=replies.check(xcb_reparent_window_checked(connection,window,parent,0,0),"XCB construction reparent");
            need(!error,"standalone video parent");
        }
        xcb_map_window(connection,window);xcb_flush(connection);
        if(standalone){xcb_map_window(connection,standalone);xcb_flush(connection);}
        // The EXT API takes the address of an xcb_window_t, not an integer cast.
        surface=create_surface(display,config,&window,nullptr);
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
            if(standalone)xcb_destroy_window(connection,standalone);
            if(colormap)xcb_free_colormap(connection,colormap);
            xcb_disconnect(connection);connection=nullptr;
        }
    }
    ~Window(){cleanup();}
    xcb_window_t make_standalone(xcb_window_t root,gint64 end=0,ConstructionReplies *construction=nullptr) {
        auto id=xcb_generate_id(connection);
        uint32_t values[]={0,XCB_EVENT_MASK_EXPOSURE|XCB_EVENT_MASK_STRUCTURE_NOTIFY|XCB_EVENT_MASK_KEY_PRESS};
        const auto made=xcb_create_window_checked(connection,XCB_COPY_FROM_PARENT,
            id,root,0,0,width,height,0,XCB_WINDOW_CLASS_INPUT_OUTPUT,XCB_COPY_FROM_PARENT,
            XCB_CW_BACK_PIXEL|XCB_CW_EVENT_MASK,values);
        auto error=end?checked_request_bounded(connection,made,end,"XCB handoff standalone"):
            construction?construction->check(made,"XCB construction standalone"):
                XcbReply<xcb_generic_error_t>(xcb_request_check(connection,made),&free);
        need(!error,"standalone output window");
        const char title[]="CrystalHD GPU field output (experimental)";
        const char klass[]="crystalhd-field\0CrystalHD\0";
        xcb_change_property(connection,XCB_PROP_MODE_REPLACE,id,XCB_ATOM_WM_NAME,XCB_ATOM_STRING,8,sizeof(title)-1,title);
        xcb_change_property(connection,XCB_PROP_MODE_REPLACE,id,XCB_ATOM_WM_CLASS,XCB_ATOM_STRING,8,sizeof(klass)-1,klass);
        xcb_change_property(connection,XCB_PROP_MODE_REPLACE,id,protocols,XCB_ATOM_ATOM,32,1,&delete_window);
        return id;
    }
    void resize() {
        resize_current_bounded(connection,parent,window,width,height);
    }
    bool replace(guintptr handle) {
        return replace_window_bounded(connection,window,handle,parent,standalone,
            embedded,width,height,[&](xcb_window_t root,gint64 end){return make_standalone(root,end);});
    }
    void toggle_fullscreen() {
        if(embedded)return;
        xcb_client_message_event_t event{};event.response_type=XCB_CLIENT_MESSAGE;
        event.window=standalone;event.type=state;event.format=32;
        event.data.data32[0]=2;event.data.data32[1]=fullscreen;event.data.data32[3]=1;
        auto screens=xcb_setup_roots_iterator(xcb_get_setup(connection));
        xcb_send_event(connection,0,screens.data->root,
            XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT|XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
            reinterpret_cast<const char *>(&event));xcb_flush(connection);
    }
    uint32_t keysym(uint8_t code) {
        return keyboard_mapping_bounded(connection,code);
    }
};

struct Worker {
    GstElement *owner;
    Counters &stats;
    std::mutex mutex;
    std::mutex join_mutex;
    std::condition_variable changed;
    std::deque<std::shared_ptr<Command>> queue;
    std::deque<std::shared_ptr<WindowCommand>> windows;
    std::thread thread;
    std::thread::id thread_id;
    int wake=-1;
    bool ready=false,stopping=false,flushing=false,broken=false;
    guint64 generation=1;
    GstClockID active_wait=nullptr;
    guintptr handle;
    std::string failure;
    std::atomic<bool> expose{false};
    Worker(GstElement *element,Counters &c,guintptr window):owner(element),stats(c),handle(window) {
        wake=eventfd(0,EFD_CLOEXEC|EFD_NONBLOCK);need(wake>=0,"worker eventfd");
        try {thread=std::thread(&Worker::run,this);thread_id=thread.get_id();}catch(...){close(wake);throw;}
    }
    void notify(){uint64_t one=1;ssize_t ignored=write(wake,&one,sizeof(one));(void)ignored;}
    void cancel(bool stop=false) {
        {std::lock_guard<std::mutex> lock(mutex);flushing=true;++generation;stopping|=stop;
         if(active_wait)gst_clock_id_unschedule(active_wait);}
        notify();changed.notify_all();
    }
    void shutdown() {
        cancel(true);
        std::lock_guard<std::mutex> lock(join_mutex);
        if(thread.joinable())thread.join();
    }
    ~Worker(){shutdown();close(wake);}
    bool replace_window(guintptr value) {
        // Never wait on the thread that must service this request.
        if(std::this_thread::get_id()==thread_id)return false;
        auto request=std::make_shared<WindowCommand>();request->handle=value;
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait(lock,[&]{return ready||broken||stopping;});
        if(broken||stopping)return false;
        windows.push_back(request);
        if(active_wait)gst_clock_id_unschedule(active_wait);
        notify();
        changed.wait(lock,[&]{return request->done||broken||stopping;});
        return request->done&&request->accepted;
    }
    bool replace_windows(Window &window) {
        bool moved=false;
        for(;;) {
            std::shared_ptr<WindowCommand> request;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if(stopping||windows.empty())break;
                request=windows.front();windows.pop_front();
            }
            const auto before=window.embedded?window.parent:0;
            const bool accepted=window.replace(request->handle);
            if(accepted) {
                stats.window_handle=request->handle;
                if(before!=request->handle){++stats.window_replacements;moved=true;}
            }else ++stats.window_rejections;
            {
                std::lock_guard<std::mutex> lock(mutex);
                request->accepted=accepted;request->done=true;
            }
            changed.notify_all();
        }
        return moved;
    }
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
    static bool target_time(Command &command,GstClockTime running,GstClockTime &target) {
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
    template<class Controls> bool wait(Command &command,GstClockTime running,Controls controls) {
        if(!command.clock)return current(command.generation);
        GstClockTime target=0;
        need(target_time(command,running,target),"valid field clock target");
        // SwapBuffers queues/throttles a vsynced presentation; it is not an
        // instantaneous scanout. Waiting until the media target before calling
        // it adds another refresh wait and can accumulate unbounded A/V lag.
        target=target>command.base+command.render_lead?target-command.render_lead:command.base;
        for(;;) {
            controls();
            GstClockID id=gst_clock_new_single_shot_id(command.clock,target);
            {
                std::lock_guard<std::mutex> lock(mutex);
                if(stopping||flushing||generation!=command.generation){gst_clock_id_unref(id);return false;}
                if(!windows.empty()){gst_clock_id_unref(id);continue;}
                active_wait=id;
            }
            GstClockReturn result=gst_clock_id_wait(id,nullptr);
            {std::lock_guard<std::mutex> lock(mutex);active_wait=nullptr;}
            gst_clock_id_unref(id);
            // Window replacement interrupts only this wait, not the picture
            // generation. Service it and wait again for the original deadline.
            if(result==GST_CLOCK_UNSCHEDULED&&current(command.generation))continue;
            return (result==GST_CLOCK_OK||result==GST_CLOCK_EARLY)&&current(command.generation);
        }
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
        std::unique_ptr<GbmAllocation> storage;
        std::unique_ptr<Renderer> renderer;
        std::unique_ptr<Image> views[2];
        GstBuffer *retained=nullptr;
        GstVideoInfo previous{};
        guint64 held_generation=0;
        unsigned last_parity=0;
        bool last_interlaced=false;
        auto retire=[&] {
            if(!retained&&!views[0]&&!views[1])return;
            const auto begin=gst_util_get_timestamp();
            // Both field submissions and paused redraws still own this exact
            // generation. Complete every GPU use before releasing its source,
            // destroying field views, or allowing the next producer write.
            glFinish();
            views[0].reset();views[1].reset();stats.images=0;
            if(retained){gst_buffer_unref(retained);retained=nullptr;stats.retained=0;++stats.retirements;}
            stats.retire_ns+=gst_util_get_timestamp()-begin;
        };
        // Scope guard also runs on exceptions, while the EGL context is current.
        struct RetainGuard { decltype(retire) &cleanup;Counters &stats;
            ~RetainGuard(){cleanup();stats.allocations=0;stats.window_handle=0;}
        } guard{retire,stats};
        stats.window_handle=handle;
        {std::lock_guard<std::mutex> lock(mutex);ready=true;}changed.notify_all();
        auto draw=[&](unsigned parity,bool interlaced) {
            if(!retained||!window.width||!window.height)return;
            glBindFramebuffer(GL_FRAMEBUFFER,0);glViewport(0,0,window.width,window.height);
            const double aspect=double(previous.width)*previous.par_n/(double(previous.height)*previous.par_d);
            unsigned w=window.width,h=std::max(1U,unsigned(std::lround(w/aspect)));
            if(h>window.height){h=window.height;w=std::max(1U,unsigned(std::lround(h*aspect)));}
            clear_borders(window.width,window.height,(window.width-w)/2,(window.height-h)/2,w,h);
            renderer->draw_to(*views[interlaced?parity:0],interlaced,parity,0,
                (window.width-w)/2,(window.height-h)/2,w,h,true,!interlaced,false);
#ifdef CRYSTALHD_FIELD_OBSERVER
            CRYSTALHD_FIELD_OBSERVER(retained,previous,parity,interlaced,window.width,window.height,
                views[interlaced?parity:0]->image);
#endif
        };
        auto swap=[&] {need(eglSwapBuffers(window.display,window.surface),"window swap");++stats.swaps;};
        for(;;) {
            bool redraw=replace_windows(window)|expose.exchange(false),resized=false;
            while(auto *event=xcb_poll_for_event(window.connection)) {
                const uint8_t type=event->response_type&0x7f;
                if(type==XCB_EXPOSE)redraw=true;
                else if(type==XCB_CONFIGURE_NOTIFY) {
                    auto *configure=reinterpret_cast<xcb_configure_notify_event_t *>(event);
                    // An old child ConfigureNotify may predate reparenting.
                    // Embedded size follows only the current parent.
                    if(configure->window==window.window||configure->window==window.parent) {
                        resized=true;
                    }
                } else if(type==XCB_CLIENT_MESSAGE) {
                    auto *client=reinterpret_cast<xcb_client_message_event_t *>(event);
                    if((client->window==window.window||client->window==window.parent)&&
                       client->type==window.protocols&&client->data.data32[0]==window.delete_window)post("crystalhd-field-close");
                } else if(type==XCB_KEY_PRESS) {
                    auto *key=reinterpret_cast<xcb_key_press_event_t *>(event);
                    if(key->event!=window.window&&key->event!=window.parent){free(event);continue;}
                    auto symbol=window.keysym(key->detail);
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
            // Coalesce notifications and read current geometry. A queued
            // event from an earlier attachment to this same parent is stale.
            if(resized){window.resize();redraw=true;}
            std::shared_ptr<Command> command;
            guint64 now_generation=0;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if(stopping)break;
                now_generation=generation;
                if(!queue.empty()){command=queue.front();queue.pop_front();}
            }
            if(retained&&held_generation!=now_generation)retire();
            if(!command) {
                if(redraw&&retained&&current(held_generation)){draw(last_parity,last_interlaced);swap();++stats.redraws;}
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
                    retire();storage.reset();renderer.reset();
                    storage=std::make_unique<GbmAllocation>(gpu,info.width,info.height,
                        GST_VIDEO_INFO_FORMAT(&info)==GST_VIDEO_FORMAT_NV12?DRM_FORMAT_NV12:DRM_FORMAT_YUYV);stats.allocations=1;
                    // Only the window default framebuffer is used; tiny helper FBO.
                    renderer=std::make_unique<Renderer>(info.width,info.height,2,2);previous=info;
                }
                if(command->buffer!=retained||held_generation!=command->generation) {
                    const auto prepare_begin=gst_util_get_timestamp();
                    const auto retirement_before=stats.retire_ns.load();
                    retire();GstVideoFrame frame{};
                    need(gst_video_frame_map(&frame,&info,command->buffer,GST_MAP_READ),"input video mapping");
                    try {
                        const gint stride=GST_VIDEO_FRAME_PLANE_STRIDE(&frame,0);
                        const auto *data=static_cast<const uint8_t *>(GST_VIDEO_FRAME_PLANE_DATA(&frame,0));
                        if(GST_VIDEO_INFO_FORMAT(&info)==GST_VIDEO_FORMAT_NV12) {
                            const gint uv_stride=GST_VIDEO_FRAME_PLANE_STRIDE(&frame,1);
                            need(stride>=info.width&&uv_stride>=info.width,"positive NV12 input strides");
                            storage->write_nv12(data,stride,mapped_plane_available(frame,0),
                                static_cast<const uint8_t *>(GST_VIDEO_FRAME_PLANE_DATA(&frame,1)),uv_stride,mapped_plane_available(frame,1));
                        }else {
                            need(stride>=info.width*2,"positive YUY2 input stride");
                            storage->write(data,stride,mapped_plane_available(frame,0));
                        }
                    }catch(...){gst_video_frame_unmap(&frame);throw;}
                    gst_video_frame_unmap(&frame);
                    const EGLint matrix=info.colorimetry.matrix==GST_VIDEO_COLOR_MATRIX_BT601?EGL_ITU_REC601_EXT:EGL_ITU_REC709_EXT;
                    const EGLint horizontal=GST_VIDEO_INFO_FORMAT(&info)==GST_VIDEO_FORMAT_NV12&&
                        info.chroma_site==GST_VIDEO_CHROMA_SITE_MPEG2?EGL_YUV_CHROMA_SITING_0_EXT:EGL_YUV_CHROMA_SITING_0_5_EXT;
                    views[0]=std::make_unique<Image>(gpu,*storage,interlaced,0,matrix,false,
                        storage->format,storage->uv_offset,storage->uv_pitch,horizontal);
                    need(views[0]->texture,"first field import");stats.images=1;
                    if(interlaced){views[1]=std::make_unique<Image>(gpu,*storage,true,1,matrix,false,
                        storage->format,storage->uv_offset,storage->uv_pitch,horizontal);need(views[1]->texture,"second field import");stats.images=2;}
                    retained=gst_buffer_ref(command->buffer);stats.retained=1;held_generation=command->generation;
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
                    if((!command->preroll&&!wait(*command,running,[&] {
                            if(replace_windows(window)&&current(command->generation))draw(parity,interlaced);
                        }))||!current(command->generation)) {
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
                                stats.worst_parity=parity;stats.worst_swap_sequence=stats.swaps.load();
                                stats.worst_prepare_ns=preparation_ns;
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
                if(!command->preroll&&result==GST_FLOW_OK)++stats.pictures;
            }
            {std::lock_guard<std::mutex> lock(mutex);command->result=result;command->done=true;}changed.notify_all();
        }
        retire();renderer.reset();storage.reset();stats.allocations=0;
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
    std::mutex window_mutex;
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
        std::lock_guard<std::mutex> replacement(self->state->window_mutex);
        std::lock_guard<std::mutex> lock(self->state->mutex);
        self->state->worker=std::make_shared<Worker>(GST_ELEMENT(self),self->state->counters,self->state->window);
        return TRUE;
    }catch(const std::exception &error){GST_ELEMENT_ERROR(self,RESOURCE,OPEN_READ_WRITE,("%s",error.what()),(nullptr));return FALSE;}
}
static gboolean stop(GstBaseSink *sink) {
    auto *self=reinterpret_cast<GstCrystalhdField *>(sink);std::shared_ptr<Worker> worker;
    {std::lock_guard<std::mutex> lock(self->state->mutex);worker.swap(self->state->worker);}
    if(worker)worker->shutdown();
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
    if(!gst_video_info_from_caps(&info,caps)||
       (GST_VIDEO_INFO_FORMAT(&info)!=GST_VIDEO_FORMAT_YUY2&&GST_VIDEO_INFO_FORMAT(&info)!=GST_VIDEO_FORMAT_NV12)||info.fps_n<=0||info.fps_d<=0||
       info.width<2||info.height<2||(info.width&1)||(info.height&1)||
       info.width>1920||info.height>1088||info.par_n<=0||info.par_d<=0||
       info.colorimetry.range!=GST_VIDEO_COLOR_RANGE_16_235||
       (info.colorimetry.matrix!=GST_VIDEO_COLOR_MATRIX_BT709&&info.colorimetry.matrix!=GST_VIDEO_COLOR_MATRIX_BT601))return FALSE;
    const char *site=gst_structure_get_string(gst_caps_get_structure(caps,0),"chroma-site");
    if(GST_VIDEO_INFO_FORMAT(&info)==GST_VIDEO_FORMAT_NV12&&
       ((!site||(strcmp(site,"jpeg")&&strcmp(site,"mpeg2")))||(info.height%4)||
        (info.interlace_mode!=GST_VIDEO_INTERLACE_MODE_PROGRESSIVE&&info.interlace_mode!=GST_VIDEO_INTERLACE_MODE_INTERLEAVED)||
        (info.chroma_site!=GST_VIDEO_CHROMA_SITE_JPEG&&info.chroma_site!=GST_VIDEO_CHROMA_SITE_MPEG2)))return FALSE;
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
    auto caller=get_worker(self);
    if(caller&&std::this_thread::get_id()==caller->thread_id) {
        GST_WARNING_OBJECT(self,"cannot replace output from its GPU worker");return;
    }
    // Serialize setters without holding State::mutex while the worker acks:
    // stop must remain able to cancel and join it concurrently.
    std::lock_guard<std::mutex> replacement(self->state->window_mutex);
    std::shared_ptr<Worker> worker;
    {
        std::lock_guard<std::mutex> lock(self->state->mutex);
        worker=self->state->worker;
        if(!worker){self->state->window=window;return;}
    }
    if(!worker->replace_window(window)) {
        GST_WARNING_OBJECT(self,"window replacement rejected; current output preserved");return;
    }
    std::lock_guard<std::mutex> lock(self->state->mutex);
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
        "window-replacements",G_TYPE_UINT64,c.window_replacements.load(),
        "window-rejections",G_TYPE_UINT64,c.window_rejections.load(),
        "window-handle",G_TYPE_UINT64,c.window_handle.load(),
        "prepare-ns",G_TYPE_UINT64,c.prepare_ns.load(),"draw-ns",G_TYPE_UINT64,c.draw_ns.load(),
        "wait-ns",G_TYPE_UINT64,c.wait_ns.load(),"swap-ns",G_TYPE_UINT64,c.swap_ns.load(),
        "retire-ns",G_TYPE_UINT64,c.retire_ns.load(),"retirements",G_TYPE_UINT64,c.retirements.load(),
        "worst-field-pts",G_TYPE_UINT64,c.worst_field_pts.load(),
        "worst-field-index",G_TYPE_UINT64,c.worst_field_index.load(),
        "worst-parity",G_TYPE_UINT64,c.worst_parity.load(),
        "worst-swap-sequence",G_TYPE_UINT64,c.worst_swap_sequence.load(),
        "worst-prepare-ns",G_TYPE_UINT64,c.worst_prepare_ns.load(),
        "worst-retire-ns",G_TYPE_UINT64,c.worst_retire_ns.load(),
        "worst-draw-ns",G_TYPE_UINT64,c.worst_draw_ns.load(),
        "worst-wait-ns",G_TYPE_UINT64,c.worst_wait_ns.load(),
        "worst-swap-ns",G_TYPE_UINT64,c.worst_swap_ns.load(),
        "worst-head-age-ns",G_TYPE_INT64,c.worst_head_age_ns.load(),
        "worst-preswap-age-ns",G_TYPE_INT64,c.worst_preswap_age_ns.load(),
        "worst-monotonic-elapsed-ns",G_TYPE_UINT64,c.worst_monotonic_elapsed_ns.load(),
        "worst-clock-elapsed-ns",G_TYPE_UINT64,c.worst_clock_elapsed_ns.load(),
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
        "Owned linear YUY2/NV12 field views with pipeline-clocked XCB/EGL output","CrystalHD contributors");
    static GstStaticPadTemplate pad=GST_STATIC_PAD_TEMPLATE("sink",GST_PAD_SINK,GST_PAD_ALWAYS,
        GST_STATIC_CAPS("video/x-raw, format=(string)YUY2, width=(int)[2,1920], height=(int)[2,1088], "
                        "framerate=(fraction)[1/1,60/1], interlace-mode=(string){progressive,interleaved,mixed}; "
                        "video/x-raw, format=(string)NV12, width=(int)[2,1920], height=(int)[4,1088], "
                        "framerate=(fraction)[1/1,60/1], interlace-mode=(string){progressive,interleaved}, chroma-site=(string){jpeg,mpeg2}"));
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
