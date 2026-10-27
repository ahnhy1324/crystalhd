// SPDX-License-Identifier: GPL-2.0-or-later
// Explicit LD_PRELOAD diagnostic. Forward every call unchanged; collect only
// waits inside the EGLSwapBuffers interval marked by our companion probe.
#include <xcb/xcb.h>
#include <xcb/xcbext.h>
#include <xcb/present.h>
#include <xf86drm.h>
#include <dlfcn.h>
#include <poll.h>
#include <unistd.h>
#include <time.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <vector>

namespace {
thread_local unsigned active_swap;
uint64_t now() {timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);return uint64_t(t.tv_sec)*1000000000+t.tv_nsec;}
template<class T>T original(const char *name) {
    const int saved=errno;
    auto pointer=reinterpret_cast<T>(dlsym(RTLD_NEXT,name));
    if(!pointer){dprintf(STDERR_FILENO,"Missing original host-wait symbol: %s\n",name);_exit(125);}
    errno=saved;return pointer;
}
struct Entry {
    const char *function;
    unsigned swap,extension=0,event_type=0,serial=0,kind=0,mode=0;
    uint64_t elapsed,msc=0;
    unsigned long request;
    bool present=false;
};
struct Log {
    std::mutex mutex;
    std::vector<Entry> entries;
    unsigned dropped=0;
    void add(const Entry &entry) noexcept {
        if(!entry.swap||entry.elapsed<1000000)return;
        std::lock_guard<std::mutex> lock(mutex);
        if(entries.size()>=8192){++dropped;return;}
        try{entries.push_back(entry);}catch(...){++dropped;}
    }
    ~Log() {
        for(const auto &e:entries)
            dprintf(STDOUT_FILENO,"{\"type\":\"host-wait\",\"swap\":%u,\"function\":\"%s\",\"elapsed_ns\":%llu,"
                "\"request\":%lu,\"present\":%s,\"extension\":%u,\"event_type\":%u,\"serial\":%u,"
                "\"msc\":%llu,\"kind\":%u,\"mode\":%u}\n",e.swap,e.function,
                static_cast<unsigned long long>(e.elapsed),e.request,e.present?"true":"false",
                e.extension,e.event_type,e.serial,static_cast<unsigned long long>(e.msc),e.kind,e.mode);
        dprintf(STDOUT_FILENO,"{\"type\":\"host-wait-summary\",\"events\":%zu,\"dropped\":%u}\n",entries.size(),dropped);
    }
} log;
}
extern "C" void crystalhd_host_wait_mark(unsigned swap) {active_swap=swap;}
extern "C" int poll(pollfd *fds,nfds_t count,int timeout) {
    using Function=int(*)(pollfd*,nfds_t,int);static auto real=original<Function>("poll");
    const auto swap=active_swap;const auto start=swap?now():0;
    const int result=real(fds,count,timeout),error=errno;
    if(swap)log.add({"poll",swap,0,0,0,0,0,now()-start,0,0,false});
    errno=error;return result;
}
extern "C" int drmIoctl(int fd,unsigned long request,void *argument) {
    using Function=int(*)(int,unsigned long,void*);static auto real=original<Function>("drmIoctl");
    const auto swap=active_swap;const auto start=swap?now():0;
    const int result=real(fd,request,argument),error=errno;
    if(swap)log.add({"drmIoctl",swap,0,0,0,0,0,now()-start,0,request,false});
    errno=error;return result;
}
extern "C" void *xcb_wait_for_reply(xcb_connection_t *connection,unsigned sequence,xcb_generic_error_t **error) {
    using Function=void*(*)(xcb_connection_t*,unsigned,xcb_generic_error_t**);
    static auto real=original<Function>("xcb_wait_for_reply");
    const auto swap=active_swap;const auto start=swap?now():0;
    void *reply=real(connection,sequence,error);const int saved=errno;
    if(swap)log.add({"xcb_wait_for_reply",swap,0,0,0,0,0,now()-start,0,0,false});
    errno=saved;return reply;
}
extern "C" xcb_generic_event_t *xcb_wait_for_special_event(xcb_connection_t *connection,xcb_special_event_t *event) {
    using Function=xcb_generic_event_t*(*)(xcb_connection_t*,xcb_special_event_t*);
    static auto real=original<Function>("xcb_wait_for_special_event");
    const auto swap=active_swap;const auto start=swap?now():0;
    auto *reply=real(connection,event);const int saved=errno;
    if(swap) {
        Entry entry{"xcb_wait_for_special_event",swap,0,0,0,0,0,now()-start,0,0,false};
        if(entry.elapsed>=1000000&&reply&&(reply->response_type&0x7f)==XCB_GE_GENERIC) {
            const auto *ge=reinterpret_cast<const xcb_ge_generic_event_t *>(reply);
            entry.extension=ge->extension;entry.event_type=ge->event_type;
            const auto *present=xcb_get_extension_data(connection,&xcb_present_id);
            entry.present=present&&present->present&&present->major_opcode==ge->extension;
            if(entry.present&&ge->event_type==XCB_PRESENT_COMPLETE_NOTIFY) {
                const auto *complete=reinterpret_cast<const xcb_present_complete_notify_event_t *>(reply);
                entry.serial=complete->serial;entry.msc=complete->msc;entry.kind=complete->kind;entry.mode=complete->mode;
            }
        }
        log.add(entry);
    }
    errno=saved;return reply;
}
