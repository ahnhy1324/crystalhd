// SPDX-License-Identifier: GPL-2.0-or-later
// Explicit diagnostic build only. Native sync-file observation is independent
// of the EGL display lock held by a potentially blocking SwapBuffers call.
#include "field-view-gpu.h"
#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <poll.h>
#include <sys/eventfd.h>
#include <dlfcn.h>
static EGLBoolean observed_swap(EGLDisplay,EGLSurface);
static EGLBoolean observed_terminate(EGLDisplay);
#define eglSwapBuffers observed_swap
#define eglTerminate observed_terminate
#include "../filters/gst/field-output/gstcrystalhdfield.cpp"
#undef eglSwapBuffers
#undef eglTerminate

namespace {
struct SwapSample {
    int fd=-1;
    unsigned sequence=0;
    guint64 created=0;
    std::atomic<guint64> entered{0},returned{0},ready{0};
    bool valid=true,swap_ok=false;
    ~SwapSample(){if(fd>=0)close(fd);}
};
struct SwapObserver {
    EGLDisplay display;
    PFNEGLCREATESYNCKHRPROC create;
    PFNEGLDESTROYSYNCKHRPROC destroy;
    PFNEGLDUPNATIVEFENCEFDANDROIDPROC duplicate;
    std::mutex mutex;
    std::deque<std::shared_ptr<SwapSample>> incoming;
    std::vector<std::shared_ptr<SwapSample>> samples;
    std::atomic<bool> stopping{false};
    int wake=-1;
    std::thread thread;
    explicit SwapObserver(EGLDisplay d):display(d) {
        const char *ext=eglQueryString(d,EGL_EXTENSIONS);
        need(ext&&strstr(ext,"EGL_ANDROID_native_fence_sync"),"native fence FD diagnostic requires advertised support");
        create=reinterpret_cast<PFNEGLCREATESYNCKHRPROC>(eglGetProcAddress("eglCreateSyncKHR"));
        destroy=reinterpret_cast<PFNEGLDESTROYSYNCKHRPROC>(eglGetProcAddress("eglDestroySyncKHR"));
        duplicate=reinterpret_cast<PFNEGLDUPNATIVEFENCEFDANDROIDPROC>(eglGetProcAddress("eglDupNativeFenceFDANDROID"));
        need(create&&destroy&&duplicate,"native fence diagnostic functions");
        wake=eventfd(0,EFD_CLOEXEC|EFD_NONBLOCK);need(wake>=0,"fence observer wake");
        try{thread=std::thread(&SwapObserver::run,this);}catch(...){close(wake);wake=-1;throw;}
    }
    void notify(){uint64_t one=1;ssize_t ignored=write(wake,&one,sizeof(one));(void)ignored;}
    std::shared_ptr<SwapSample> watch() {
        auto sample=std::make_shared<SwapSample>();sample->created=gst_util_get_timestamp();
        // This fence follows the field draw. It does not authorize BO reuse:
        // the original consumer's retirement barrier remains responsible.
        EGLSyncKHR sync=create(display,EGL_SYNC_NATIVE_FENCE_ANDROID,nullptr);
        need(sync!=EGL_NO_SYNC_KHR,"native fence creation");glFlush();
        sample->fd=duplicate(display,sync);
        const bool destroyed=destroy(display,sync);
        need(sample->fd>=0&&destroyed,"owned native fence FD export/destroy");
        need(fcntl(sample->fd,F_SETFD,FD_CLOEXEC)==0,"native fence close-on-exec");
        pollfd ready={sample->fd,POLLIN,0};
        const int polled=poll(&ready,1,0);
        if(polled>0&&(ready.revents&POLLIN)&&!(ready.revents&(POLLERR|POLLHUP|POLLNVAL))) {
            sample->ready=gst_util_get_timestamp();close(sample->fd);sample->fd=-1;
        }else need(polled>=0&&!(ready.revents&(POLLERR|POLLHUP|POLLNVAL)),"native fence initial poll");
        {
            std::lock_guard<std::mutex> lock(mutex);
            need(samples.size()<4096,"bounded 4096-swap diagnostic");
            sample->sequence=samples.size()+1;samples.push_back(sample);
            if(sample->fd>=0)incoming.push_back(sample);
        }
        notify();return sample;
    }
    void run() noexcept {
        std::vector<std::shared_ptr<SwapSample>> pending;
        for(;;) {
            {std::lock_guard<std::mutex> lock(mutex);
             while(!incoming.empty()){pending.push_back(incoming.front());incoming.pop_front();}}
            std::vector<pollfd> fds;fds.push_back({wake,POLLIN,0});
            for(const auto &sample:pending)fds.push_back({sample->fd,POLLIN,0});
            int result;
            do result=poll(fds.data(),fds.size(),stopping?0:100);while(result<0&&errno==EINTR);
            const auto observed=gst_util_get_timestamp();
            if(fds[0].revents&POLLIN){uint64_t value;while(read(wake,&value,sizeof(value))>0){}}
            for(size_t i=pending.size();i>0;--i) {
                auto &sample=pending[i-1];const auto events=fds[i].revents;
                if(result<0||events&(POLLERR|POLLHUP|POLLNVAL)||stopping||events&POLLIN) {
                    if(result>=0&&(events&POLLIN)&&!(events&(POLLERR|POLLHUP|POLLNVAL)))sample->ready=observed;
                    else sample->valid=false;
                    close(sample->fd);sample->fd=-1;pending.erase(pending.begin()+i-1);
                }
            }
            if(stopping)break;
        }
    }
    ~SwapObserver() {
        stopping=true;notify();if(thread.joinable())thread.join();
        // Cleanup follows the original source retirement/glFinish. Poll any
        // final arrivals once, then close every diagnostic-owned descriptor.
        for(auto &sample:incoming) {
            pollfd fd={sample->fd,POLLIN,0};
            if(poll(&fd,1,0)>0&&(fd.revents&POLLIN)&&!(fd.revents&(POLLERR|POLLHUP|POLLNVAL)))sample->ready=gst_util_get_timestamp();
            else sample->valid=false;
            close(sample->fd);sample->fd=-1;
        }
        for(const auto &sample:samples) {
            const auto enter=sample->entered.load(),end=sample->returned.load(),ready=sample->ready.load();
            dprintf(STDOUT_FILENO,"{\"type\":\"native-fence-swap\",\"swap\":%u,\"valid\":%s,\"swap_ok\":%s,"
                "\"entered_ns\":%" G_GUINT64_FORMAT ",\"setup_ns\":%" G_GUINT64_FORMAT ",\"swap_ns\":%" G_GUINT64_FORMAT ","
                "\"gpu_seen_from_enter_ns\":%" G_GINT64_FORMAT ",\"post_gpu_lower_bound_ns\":%" G_GUINT64_FORMAT ","
                "\"ready_before_swap\":%s}\n",sample->sequence,sample->valid&&ready&&enter&&end?"true":"false",
                sample->swap_ok?"true":"false",enter,enter>=sample->created?enter-sample->created:0,end>=enter?end-enter:0,
                ready&&enter?GST_CLOCK_DIFF(enter,ready):0,ready&&end>ready?end-ready:0,
                ready&&enter&&ready<=enter?"true":"false");
        }
        close(wake);
        size_t live=0,invalid=0;
        for(const auto &sample:samples) {
            live+=sample->fd>=0;
            invalid+=!sample->valid||!sample->ready.load()||!sample->entered.load()||!sample->returned.load();
        }
        dprintf(STDOUT_FILENO,"{\"type\":\"native-fence-summary\",\"swaps\":%zu,\"invalid_samples\":%zu,\"owned_fds_after_close\":%zu}\n",
               samples.size(),invalid,live);
    }
};
std::mutex observers_mutex;
std::map<EGLDisplay,std::shared_ptr<SwapObserver>> observers;
}
static EGLBoolean observed_swap(EGLDisplay display,EGLSurface surface) {
    std::shared_ptr<SwapObserver> observer;
    {std::lock_guard<std::mutex> lock(observers_mutex);
     auto &entry=observers[display];if(!entry)entry=std::make_shared<SwapObserver>(display);observer=entry;}
    using Marker=void(*)(unsigned);
    static auto marker=reinterpret_cast<Marker>(dlsym(RTLD_DEFAULT,"crystalhd_host_wait_mark"));
    auto sample=observer->watch();if(marker)marker(sample->sequence);
    sample->entered=gst_util_get_timestamp();
    const EGLBoolean result=eglSwapBuffers(display,surface);
    sample->returned=gst_util_get_timestamp();if(marker)marker(0);
    sample->swap_ok=result;return result;
}
static EGLBoolean observed_terminate(EGLDisplay display) {
    std::shared_ptr<SwapObserver> observer;
    {std::lock_guard<std::mutex> lock(observers_mutex);
     auto found=observers.find(display);if(found!=observers.end()){observer=found->second;observers.erase(found);}}
    observer.reset();return eglTerminate(display);
}
