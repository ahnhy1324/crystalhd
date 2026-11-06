// SPDX-License-Identifier: GPL-2.0-or-later
// Actual own Window constructor against private owned peers; no GL/device is
// reached unless --x11 explicitly requests a final valid display reopen.
#include <xcb/xcb.h>
#include <atomic>
static void observe_discard(xcb_connection_t *,unsigned);
#define xcb_discard_reply observe_discard
#include "../filters/gst/field-output/gstcrystalhdfield.cpp"
#undef xcb_discard_reply
#include "x11-setup-peer.h"
static std::atomic<uint64_t> discarded_sequences{0};
static void observe_discard(xcb_connection_t *connection,unsigned sequence) {
    if(sequence<64)discarded_sequences.fetch_or(UINT64_C(1)<<sequence);
    xcb_discard_reply(connection,sequence);
}
struct Environment {
    char *display=g_strdup(g_getenv("DISPLAY")),*authority=g_strdup(g_getenv("XAUTHORITY"));
    void restore() {
        if(display)g_setenv("DISPLAY",display,TRUE);else g_unsetenv("DISPLAY");
        if(authority)g_setenv("XAUTHORITY",authority,TRUE);else g_unsetenv("XAUTHORITY");
    }
    ~Environment(){restore();g_free(display);g_free(authority);}
};
static void exchanges() {
    Environment original;SetupPeer peer{};setup_listen(&peer);
    char *path=nullptr;int authority=g_file_open_tmp("crystalhd-field-auth-XXXXXX",&path,nullptr);
    need(authority>=0,"private test authority");close(authority);g_setenv("XAUTHORITY",path,TRUE);
    std::array<unsigned char,4> prior{};
    for(unsigned trial=0;trial<6;++trial) {
        const unsigned method=trial/2;
        FILE *file=fdopen(open(path,O_WRONLY|O_TRUNC|O_CLOEXEC),"wb");need(file,"test authority open");
        if(method) {
            char host[256],number[32],mit[]="MIT-MAGIC-COOKIE-1",xdm[]="XDM-AUTHORIZATION-1";
            need(gethostname(host,sizeof(host)-1)==0,"test hostname");host[sizeof(host)-1]=0;
            g_snprintf(number,sizeof(number),"%u",peer.display);char *type=method==1?mit:xdm;
            Xauth a{};a.family=FamilyLocal;a.address_length=strlen(host);a.address=host;
            a.number_length=strlen(number);a.number=number;a.name_length=strlen(type);a.name=type;
            a.data_length=sizeof(setup_cookie);a.data=(char *)setup_cookie;
            need(XauWriteAuth(file,&a)==1,"write known test authority");
        }
        need(fclose(file)==0,"test authority close");
        peer.method=method;peer.validate=TRUE;peer.partial=trial&1;
        peer.expired=peer.accepted=peer.valid=peer.eof=FALSE;peer.caller_pid=0;
        char *name=g_strdup_printf("%s:%u.2",trial&1?"unix/":"",peer.display);
        g_setenv("DISPLAY",name,TRUE);g_free(name);const guint before=setup_fd_count();
        GThread *server=g_thread_new("owned-field-setup",setup_peer,&peer);
        const auto started=g_get_monotonic_time();bool unexpected=false;std::string failure;
        try{crystalhd_field::Window window(0);unexpected=true;}
        catch(const std::exception &e){failure=e.what();}
        const auto elapsed=g_get_monotonic_time()-started;g_thread_join(server);
        need(!unexpected&&peer.accepted&&peer.valid&&peer.eof&&!peer.expired,"owned field setup timeout exchange");
        need(peer.caller_pid==getpid(),"exact own field caller PID");
        need(failure.rfind("X11 connection setup timed out",0)==0,"specific field setup failure");
        need(elapsed<2*G_USEC_PER_SEC&&setup_fd_count()==before,"field setup deadline/FD retirement");
        if(method==2) {
            guint32 address;memcpy(&address,peer.xdm_address,4);address=ntohl(address);
            need(address<=UINT32_C(0x3fffffff),"field XDM domain disjoint from Xv");
            if(trial==4)std::copy_n(peer.xdm_address,4,prior.begin());
            else need(memcmp(prior.data(),peer.xdm_address,4)!=0,"fresh field XDM nonce");
        }
        printf("field setup method=%u partial=%u unix=%u elapsed_us=%lld fd_delta=0 caller=own\n",method,
            peer.partial,trial&1,static_cast<long long>(elapsed));
    }
    close(peer.listener);unlink(path);g_free(path);
    g_setenv("DISPLAY","tcp/localhost:0",TRUE);bool rejected=false;
    try{crystalhd_field::Window window(0);}
    catch(const std::exception &e){rejected=std::string(e.what()).rfind("A local X11 display is unavailable",0)==0;}
    need(rejected,"explicit nonlocal display rejection");original.restore();
    puts("PASS6 owned field setup exchanges; no GL/device access");
}
struct AtomPeer {
    int fd=-1;unsigned mode=0;
    bool valid=false,eof=false,expired=false;pid_t caller=0;
};
static gpointer atom_peer(gpointer user) {
    auto *p=static_cast<AtomPeer *>(user);const int fd=p->fd;
    auto finish=[&]()->gpointer{close(fd);return nullptr;};
    const auto end=g_get_monotonic_time()+3500*1000;
    ucred credentials{};socklen_t size=sizeof(credentials);
    if(!getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&credentials,&size))p->caller=credentials.pid;
    unsigned char header[12]{};
    if(!setup_read(fd,header,sizeof(header),end)||
       header[0]!=(G_BYTE_ORDER==G_LITTLE_ENDIAN?'l':'B')||
       setup_u16(header+6,G_BYTE_ORDER==G_LITTLE_ENDIAN)||
       setup_u16(header+8,G_BYTE_ORDER==G_LITTLE_ENDIAN))return finish();
    xcb_setup_t setup{};setup.status=1;setup.protocol_major_version=11;
    setup.length=(sizeof(setup)-8)/4;setup.resource_id_base=0x200000;
    setup.resource_id_mask=0x1fffff;setup.maximum_request_length=65535;
    if(send(fd,&setup,sizeof(setup),MSG_NOSIGNAL)!=sizeof(setup))return finish();
    auto request=[&]() {
        xcb_intern_atom_request_t atom{};unsigned char name[256]{};
        if(!setup_read(fd,&atom,sizeof(atom),end)||atom.major_opcode!=XCB_INTERN_ATOM||
           atom.only_if_exists||atom.name_len!=strlen("WM_PROTOCOLS")||
           atom.length!=(sizeof(atom)+atom.name_len+3)/4)return false;
        return setup_read(fd,name,(atom.name_len+3)&~3U,end)&&
            !memcmp(name,"WM_PROTOCOLS",atom.name_len);
    };
    if(!request())return finish();
    p->valid=true;
    xcb_intern_atom_reply_t reply{};reply.response_type=1;reply.sequence=1;reply.atom=0x70ef;
    auto send_reply=[&]() {
        // Generated XCB structs omit trailing wire padding; replies are32 bytes.
        std::array<unsigned char,32> wire{};memcpy(wire.data(),&reply,sizeof(reply));
        return send(fd,wire.data(),wire.size(),MSG_NOSIGNAL)==ssize_t(wire.size());
    };
    if(p->mode==1) {
        if(send(fd,&reply,8,MSG_NOSIGNAL)!=8)return finish();
    } else if(p->mode==2) {
        xcb_generic_error_t error{};error.error_code=XCB_ATOM;error.sequence=1;error.major_code=XCB_INTERN_ATOM;
        if(send(fd,&error,32,MSG_NOSIGNAL)!=32)return finish();
    } else if(p->mode==3) {
        if(!send_reply())return finish();
    } else if(p->mode==4)return finish();
    else if(p->mode==5) {
        // The first reply is late. The client discards it and sends sequence2;
        // a fresh request must consume only its own reply on this connection.
        g_usleep(1100*1000);
        if(!request())return finish();
        if(!send_reply())return finish();
        reply.sequence=2;reply.atom++;
        if(!send_reply())return finish();
    }
    while(g_get_monotonic_time()<end) {
        pollfd event{fd,POLLIN,0};int ready=poll(&event,1,20);
        if(ready<0&&errno==EINTR)continue;
        if(ready>0) {
            unsigned char byte;auto count=recv(fd,&byte,1,0);
            if(count==0){p->eof=true;break;}
            if(count<0)break;
        }
        if(p->mode==6) {
            xcb_generic_event_t event{};event.response_type=XCB_EXPOSE;
            if(send(fd,&event,32,MSG_NOSIGNAL)!=32)break;
        }
    }
    p->expired=!p->eof;return finish();
}
static void atom_exchanges() {
    for(unsigned mode=0;mode<7;++mode) {
        const auto before=setup_fd_count();int sockets[2]{};
        need(!socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,sockets),"owned atom socket pair");
        AtomPeer peer{};peer.fd=sockets[1];peer.mode=mode;
        auto *server=g_thread_new("owned-field-atom",atom_peer,&peer);
        auto *connection=xcb_connect_to_fd(sockets[0],nullptr);
        std::string failure;bool connected=!xcb_connection_has_error(connection);xcb_atom_t value=0,recovered=0;
        const auto start=g_get_monotonic_time();
        try{if(connected)value=crystalhd_field::intern_atom_bounded(connection,"WM_PROTOCOLS");}
        catch(const std::exception &e){failure=e.what();}
        const auto elapsed=g_get_monotonic_time()-start;
        auto failed_with=[&](const char *message){return failure.rfind(message,0)==0;};
        if(mode==5&&failed_with("XCB atom reply timed out")) {
            try{recovered=crystalhd_field::intern_atom_bounded(connection,"WM_PROTOCOLS");}
            catch(const std::exception &e){failure=e.what();}
        }
        xcb_disconnect(connection);g_thread_join(server);
        printf("field atom mode=%u elapsed_us=%lld value=%u fd_delta=%d late_recovered=%u failure=%s caller=own\n",
            mode,static_cast<long long>(elapsed),value,int(setup_fd_count())-int(before),recovered,failure.c_str());
        need(connected&&peer.valid&&peer.caller==getpid()&&!peer.expired,"owned atom handshake/request/PID/retirement");
        need((mode==4||peer.eof)&&setup_fd_count()==before,"atom connection/FD retirement");
        if(mode==3)need(value==0x70ef&&failure.empty(),"atom success value");
        else if(mode==2)need(failed_with("XCB atom reply failed"),"atom protocol error");
        else if(mode==4)need(failed_with("XCB atom connection failed")||failed_with("XCB atom reply failed"),"atom disconnect error");
        else need(failed_with("XCB atom reply timed out")&&elapsed>=800000,"specific atom timeout");
        need(elapsed<2*G_USEC_PER_SEC,"atom reply wait limit");
        if(mode==5)need(recovered==0x70f0,"late discarded atom reply cannot satisfy next request");
    }
    puts("PASS7 owned atom reply exchanges after successful setup; production helper, no EGL/device access");
}
static bool plain_peer_setup(int fd,gint64 end,pid_t &caller) {
    ucred credentials{};socklen_t size=sizeof(credentials);
    if(!getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&credentials,&size))caller=credentials.pid;
    unsigned char header[12]{};
    if(!setup_read(fd,header,sizeof(header),end)||
       header[0]!=(G_BYTE_ORDER==G_LITTLE_ENDIAN?'l':'B')||
       setup_u16(header+6,G_BYTE_ORDER==G_LITTLE_ENDIAN)||
       setup_u16(header+8,G_BYTE_ORDER==G_LITTLE_ENDIAN))return false;
    xcb_setup_t setup{};setup.status=1;setup.protocol_major_version=11;
    setup.length=(sizeof(setup)-8)/4;setup.resource_id_base=0x200000;
    setup.resource_id_mask=0x1fffff;setup.maximum_request_length=65535;
    return send(fd,&setup,sizeof(setup),MSG_NOSIGNAL)==sizeof(setup);
}
enum KeyboardMode {
    keyboard_success,keyboard_empty,keyboard_missing,keyboard_partial,
    keyboard_error,keyboard_disconnect,keyboard_late,keyboard_short,
    keyboard_events
};
struct KeyboardPeer {
    int fd=-1;unsigned mode=0,requests=0;
    bool valid=false,eof=false,expired=false;pid_t caller=0;
};
static gpointer keyboard_peer(gpointer user) {
    auto *p=static_cast<KeyboardPeer *>(user);const int fd=p->fd;
    const auto end=g_get_monotonic_time()+3500*1000;
    auto finish=[&]()->gpointer {
        if(p->mode!=keyboard_disconnect) {
            while(g_get_monotonic_time()<end) {
                pollfd event{fd,POLLIN,0};const auto ready=poll(&event,1,20);
                if(ready<0&&errno==EINTR)continue;
                if(ready>0){unsigned char byte;const auto count=recv(fd,&byte,1,0);
                    if(count==0){p->eof=true;break;}if(count<0)break;}
                if(p->mode==keyboard_events) {
                    xcb_generic_event_t value{};value.response_type=XCB_EXPOSE;
                    if(send(fd,&value,32,MSG_NOSIGNAL)!=32)break;
                }
            }
            p->expired=!p->eof;
        }
        close(fd);return nullptr;
    };
    if(!plain_peer_setup(fd,end,p->caller))return finish();
    auto request=[&](uint8_t code) {
        xcb_get_keyboard_mapping_request_t value{};
        if(!setup_read(fd,&value,sizeof(value),end)||value.major_opcode!=XCB_GET_KEYBOARD_MAPPING||
           value.length!=sizeof(value)/4||value.first_keycode!=code||value.count!=1)return false;
        ++p->requests;return true;
    };
    if(!request(42))return finish();
    p->valid=true;
    auto reply=[&](uint16_t sequence,uint32_t first,bool empty=false,bool short_payload=false) {
        xcb_get_keyboard_mapping_reply_t value{};value.response_type=1;value.sequence=sequence;
        value.keysyms_per_keycode=empty?0:2;value.length=empty||short_payload?0:2;
        std::array<unsigned char,40> wire{};memcpy(wire.data(),&value,sizeof(value));
        const uint32_t symbols[]={first,0xffc9};memcpy(wire.data()+32,symbols,sizeof(symbols));
        const size_t bytes=32+size_t(value.length)*4;
        return send(fd,wire.data(),bytes,MSG_NOSIGNAL)==ssize_t(bytes);
    };
    if(p->mode==keyboard_success||p->mode==keyboard_empty||p->mode==keyboard_short) {
        if(!reply(1,0xffc8,p->mode==keyboard_empty,p->mode==keyboard_short))return finish();
    }else if(p->mode==keyboard_partial) {
        xcb_get_keyboard_mapping_reply_t value{};value.response_type=1;value.sequence=1;
        value.keysyms_per_keycode=2;value.length=2;
        if(send(fd,&value,8,MSG_NOSIGNAL)!=8)return finish();
    }else if(p->mode==keyboard_error) {
        xcb_generic_error_t error{};error.error_code=XCB_VALUE;error.sequence=1;
        error.major_code=XCB_GET_KEYBOARD_MAPPING;
        if(send(fd,&error,32,MSG_NOSIGNAL)!=32)return finish();
    }else if(p->mode==keyboard_disconnect)return finish();
    else if(p->mode==keyboard_late) {
        g_usleep(1100*1000);
        if(!request(43)||!reply(1,0xffc8)||!reply(2,0xffca))return finish();
    }
    return finish();
}
static void keyboard_exchanges() {
    for(unsigned mode=keyboard_success;mode<=keyboard_events;++mode) {
        const auto before=setup_fd_count();int sockets[2]{};
        need(!socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,sockets),"owned keyboard socket pair");
        KeyboardPeer peer{};peer.fd=sockets[1];peer.mode=mode;
        auto *server=g_thread_new("owned-field-keyboard",keyboard_peer,&peer);
        auto *connection=xcb_connect_to_fd(sockets[0],nullptr);
        const bool connected=!xcb_connection_has_error(connection);
        uint32_t value=0,recovered=0;std::string failure;discarded_sequences=0;
        const auto start=g_get_monotonic_time();
        try{if(connected)value=crystalhd_field::keyboard_mapping_bounded(connection,42);}
        catch(const std::exception &e){failure=e.what();}
        const auto elapsed=g_get_monotonic_time()-start;
        auto failed_with=[&](const char *message){return failure.rfind(message,0)==0;};
        if(mode==keyboard_late&&failed_with("XCB keyboard mapping reply timed out")) {
            try{recovered=crystalhd_field::keyboard_mapping_bounded(connection,43);}
            catch(const std::exception &e){failure=e.what();}
        }
        const auto discarded=discarded_sequences.load();
        xcb_disconnect(connection);g_thread_join(server);
        printf("field keyboard mode=%u elapsed_us=%lld value=%u recovered=%u requests=%u discarded=%llu failure=%s\n",
            mode,static_cast<long long>(elapsed),value,recovered,peer.requests,
            static_cast<unsigned long long>(discarded),failure.c_str());
        need(connected&&peer.valid&&peer.caller==getpid()&&!peer.expired,"keyboard peer/request/PID retirement");
        need((mode==keyboard_disconnect||peer.eof)&&setup_fd_count()==before,"keyboard connection/thread/FD retirement");
        need(peer.requests==(mode==keyboard_late?2U:1U),"exact keyboard requests");
        if(mode==keyboard_success)need(value==0xffc8&&failure.empty()&&!discarded,"first keyboard symbol value");
        else if(mode==keyboard_empty)need(!value&&failure.empty()&&!discarded,"valid empty keyboard mapping");
        else if(mode==keyboard_error)need(failed_with("XCB keyboard mapping reply failed")&&!discarded,"keyboard protocol error");
        else if(mode==keyboard_disconnect)need(failed_with("XCB keyboard mapping connection failed")||
            failed_with("XCB keyboard mapping reply failed"),"keyboard disconnect error");
        else if(mode==keyboard_short)need(failed_with("XCB keyboard mapping reply extent")&&!discarded,"short keyboard payload rejected");
        else {
            need(failed_with("XCB keyboard mapping reply timed out")&&elapsed>=800000,"specific keyboard deadline");
            need(discarded==2,"retire exact missing keyboard response");
        }
        need(elapsed<2*G_USEC_PER_SEC,"one keyboard response deadline");
        if(mode==keyboard_late)need(recovered==0xffca,"late keyboard reply cannot satisfy next key request");
    }
    puts("PASS9 owned keyboard response controls; actual worker helper, no EGL/device access");
}
struct GeometryPeer {
    int fd=-1;unsigned mode=0,requests=0;
    bool valid=false,eof=false,expired=false;pid_t caller=0;
};
static constexpr xcb_window_t geometry_parent=0x200101,geometry_child=0x200102;
static gpointer geometry_peer(gpointer user) {
    auto *p=static_cast<GeometryPeer *>(user);const int fd=p->fd;
    const auto end=g_get_monotonic_time()+3500*1000;
    auto finish=[&]()->gpointer {
        if(p->mode!=6) {
            while(g_get_monotonic_time()<end) {
                pollfd event{fd,POLLIN,0};const auto ready=poll(&event,1,20);
                if(ready<0&&errno==EINTR)continue;
                if(ready>0){unsigned char byte;const auto count=recv(fd,&byte,1,0);
                    if(count==0){p->eof=true;break;}if(count<0)break;}
            }
            p->expired=!p->eof;
        }
        close(fd);return nullptr;
    };
    if(!plain_peer_setup(fd,end,p->caller))return finish();
    auto request_pair=[&]() {
        for(auto drawable:{geometry_parent,geometry_child}) {
            xcb_get_geometry_request_t request{};
            if(!setup_read(fd,&request,sizeof(request),end)||request.major_opcode!=XCB_GET_GEOMETRY||
               request.length!=sizeof(request)/4||request.drawable!=drawable)return false;
            ++p->requests;
        }
        return true;
    };
    if(!request_pair())return finish();
    p->valid=true;
    auto reply=[&](uint16_t sequence,unsigned width,unsigned height) {
        xcb_get_geometry_reply_t value{};value.response_type=1;value.sequence=sequence;
        value.depth=24;value.root=0x100;value.width=width;value.height=height;
        std::array<unsigned char,32> wire{};memcpy(wire.data(),&value,sizeof(value));
        return send(fd,wire.data(),wire.size(),MSG_NOSIGNAL)==ssize_t(wire.size());
    };
    auto error=[&](uint16_t sequence) {
        xcb_generic_error_t value{};value.error_code=XCB_WINDOW;value.sequence=sequence;
        value.major_code=XCB_GET_GEOMETRY;return send(fd,&value,32,MSG_NOSIGNAL)==32;
    };
    if(p->mode==0) {if(!reply(1,400,240)||!reply(2,200,120))return finish();}
    else if(p->mode==1) { /* Both valid requests remain unanswered. */ }
    else if(p->mode==2) {if(!reply(1,400,240))return finish();}
    else if(p->mode==3||p->mode==8) {
        const auto delay=p->mode==3?600000:300000;
        g_usleep(delay);if(!reply(1,400,240))return finish();
        g_usleep(delay);if(!reply(2,200,120))return finish();
    } else if(p->mode==4) {
        if(!error(1)||!request_pair())return finish();
        // Sequence2 belongs to the failed pair. It must be discarded even
        // though the first wait failed before the second wait was entered.
        if(!reply(2,200,120)||!reply(3,401,241)||!reply(4,201,121))return finish();
    } else if(p->mode==5) {if(!reply(1,400,240)||!error(2))return finish();}
    else if(p->mode==6)return finish();
    else if(p->mode==7) {
        if(!reply(1,400,240))return finish();
        unsigned char partial[8]={1,24,0,0,0,0,0,0};const uint16_t sequence=2;
        memcpy(partial+2,&sequence,sizeof(sequence));
        if(send(fd,partial,sizeof(partial),MSG_NOSIGNAL)!=sizeof(partial))return finish();
    }
    return finish();
}
static void geometry_exchanges() {
    for(unsigned mode=0;mode<9;++mode) {
        const auto before=setup_fd_count();int sockets[2]{};
        need(!socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,sockets),"owned geometry socket pair");
        GeometryPeer peer{};peer.fd=sockets[1];peer.mode=mode;
        auto *server=g_thread_new("owned-field-geometry",geometry_peer,&peer);
        auto *connection=xcb_connect_to_fd(sockets[0],nullptr);
        const bool connected=!xcb_connection_has_error(connection);
        crystalhd_field::WindowGeometry value{},recovered{};std::string failure;
        discarded_sequences=0;const auto start=g_get_monotonic_time();
        try{if(connected)value=crystalhd_field::current_geometry_bounded(connection,geometry_parent,geometry_child);}
        catch(const std::exception &e){failure=e.what();}
        const auto elapsed=g_get_monotonic_time()-start;
        auto failed_with=[&](const char *message){return failure.rfind(message,0)==0;};
        if(mode==4&&failed_with("XCB parent geometry reply failed")) {
            try{recovered=crystalhd_field::current_geometry_bounded(connection,geometry_parent,geometry_child);}
            catch(const std::exception &e){failure=e.what();}
        }
        const auto discarded=discarded_sequences.load();
        xcb_disconnect(connection);g_thread_join(server);
        printf("field geometry mode=%u elapsed_us=%lld requests=%u discarded=%llu failure=%s\n",
            mode,static_cast<long long>(elapsed),peer.requests,static_cast<unsigned long long>(discarded),failure.c_str());
        need(connected&&peer.valid&&peer.caller==getpid()&&!peer.expired,"owned geometry handshake/request/PID/retirement");
        need((mode==6||peer.eof)&&setup_fd_count()==before,"geometry connection/thread/FD retirement");
        need(peer.requests==(mode==4?4U:2U),"both geometry requests before replies");
        if(mode==0||mode==8) {
            need(failure.empty()&&value.width==400&&value.height==240&&value.child_width==200&&
                value.child_height==120&&discarded==0,"matched geometry values and sequences");
        } else if(mode==1)need(failed_with("XCB parent geometry reply timed out")&&discarded==6,"missing parent cancels both requests");
        else if(mode==4)need(failed_with("XCB parent geometry reply failed")&&discarded==4&&
            recovered.width==401&&recovered.height==241&&recovered.child_width==201&&recovered.child_height==121,
            "parent error discards unwaited child; late child cannot satisfy fresh pair");
        else if(mode==5)need(failed_with("XCB child geometry reply failed"),"child protocol error");
        else if(mode==6)need(failed_with("XCB parent geometry connection failed")||
            failed_with("XCB parent geometry reply failed"),"geometry disconnect error");
        else need(failed_with("XCB child geometry reply timed out")&&discarded==4,"shared child timeout and sequence retirement");
        if(mode==1||mode==2||mode==3||mode==7)need(elapsed>=800000,"geometry timeout exercised");
        need(elapsed<2*G_USEC_PER_SEC,"geometry pair reply deadline");
    }
    puts("PASS9 owned geometry pairs; shared reply budget and pending-sequence retirement, no EGL/device access");
}
struct ResizePeer : GeometryPeer {unsigned extra_bytes=0;};
static gpointer resize_peer(gpointer user) {
    auto *p=static_cast<ResizePeer *>(user);const int fd=p->fd;
    const auto end=g_get_monotonic_time()+3500*1000;
    auto finish=[&]()->gpointer {
        if(p->mode!=4) {
            while(g_get_monotonic_time()<end) {
                pollfd event{fd,POLLIN,0};const auto ready=poll(&event,1,20);
                if(ready<0&&errno==EINTR)continue;
                if(ready>0){unsigned char bytes[64];const auto count=recv(fd,bytes,sizeof(bytes),0);
                    if(count==0){p->eof=true;break;}
                    if(count<0)break;
                    p->extra_bytes+=unsigned(count);}
            }
            p->expired=!p->eof;
        }
        close(fd);return nullptr;
    };
    if(!plain_peer_setup(fd,end,p->caller))return finish();
    for(auto drawable:{geometry_parent,geometry_child}) {
        xcb_get_geometry_request_t request{};
        if(!setup_read(fd,&request,sizeof(request),end)||request.major_opcode!=XCB_GET_GEOMETRY||
           request.length!=sizeof(request)/4||request.drawable!=drawable)return finish();
        ++p->requests;
    }
    auto geometry=[&](uint16_t sequence,unsigned width,unsigned height) {
        xcb_get_geometry_reply_t value{};value.response_type=1;value.sequence=sequence;
        value.depth=24;value.root=0x100;value.width=width;value.height=height;
        std::array<unsigned char,32> wire{};memcpy(wire.data(),&value,sizeof(value));
        return send(fd,wire.data(),wire.size(),MSG_NOSIGNAL)==ssize_t(wire.size());
    };
    if(p->mode==0)g_usleep(300000);
    if(p->mode==5)g_usleep(600000);
    if(!geometry(1,400,240)||!geometry(2,p->mode==1?400:200,p->mode==1?240:120))return finish();
    if(p->mode==1){p->valid=true;return finish();}
    xcb_configure_window_request_t configured{};uint32_t dimensions[2]{};
    if(!setup_read(fd,&configured,sizeof(configured),end)||configured.major_opcode!=XCB_CONFIGURE_WINDOW||
       configured.window!=geometry_child||configured.value_mask!=(XCB_CONFIG_WINDOW_WIDTH|XCB_CONFIG_WINDOW_HEIGHT)||
       configured.length!=(sizeof(configured)+sizeof(dimensions))/4||
       !setup_read(fd,dimensions,sizeof(dimensions),end)||dimensions[0]!=400||dimensions[1]!=240)return finish();
    ++p->requests;
    xcb_get_input_focus_request_t barrier{};
    if(!setup_read(fd,&barrier,sizeof(barrier),end)||barrier.major_opcode!=XCB_GET_INPUT_FOCUS||
       barrier.length!=sizeof(barrier)/4)return finish();
    ++p->requests;p->valid=true;
    if(p->mode==3||p->mode==4)return finish();
    if(p->mode==2) {
        xcb_generic_error_t error{};error.error_code=XCB_WINDOW;error.sequence=3;
        error.resource_id=geometry_child;error.major_code=XCB_CONFIGURE_WINDOW;
        if(send(fd,&error,32,MSG_NOSIGNAL)!=32)return finish();
    }
    if(p->mode==0)g_usleep(300000);
    if(p->mode==5)g_usleep(600000);
    xcb_get_input_focus_reply_t reply{};reply.response_type=1;reply.sequence=4;reply.focus=geometry_parent;
    std::array<unsigned char,32> wire{};memcpy(wire.data(),&reply,sizeof(reply));
    // In the over-budget case the client can already have closed its socket.
    (void)send(fd,wire.data(),wire.size(),MSG_NOSIGNAL);
    return finish();
}
static void resize_exchanges() {
    for(unsigned mode=0;mode<6;++mode) {
        const auto before=setup_fd_count();int sockets[2]{};
        need(!socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,sockets),"owned resize socket pair");
        ResizePeer peer{};peer.fd=sockets[1];peer.mode=mode;
        auto *server=g_thread_new("owned-field-resize",resize_peer,&peer);
        auto *connection=xcb_connect_to_fd(sockets[0],nullptr);
        const bool connected=!xcb_connection_has_error(connection);
        unsigned width=17,height=19;std::string failure;discarded_sequences=0;
        const auto start=g_get_monotonic_time();
        try{if(connected)crystalhd_field::resize_current_bounded(connection,geometry_parent,geometry_child,width,height);}
        catch(const std::exception &e){failure=e.what();}
        const auto elapsed=g_get_monotonic_time()-start;const auto discarded=discarded_sequences.load();
        xcb_disconnect(connection);g_thread_join(server);
        printf("field resize mode=%u elapsed_us=%lld requests=%u extra=%u cache=%ux%u discarded=%llu failure=%s\n",
            mode,static_cast<long long>(elapsed),peer.requests,peer.extra_bytes,width,height,
            static_cast<unsigned long long>(discarded),failure.c_str());
        auto failed_with=[&](const char *message){return failure.rfind(message,0)==0;};
        need(connected&&peer.valid&&peer.caller==getpid()&&!peer.expired,"owned resize handshake/request/PID/retirement");
        need((mode==4||peer.eof)&&setup_fd_count()==before,"resize connection/thread/FD retirement");
        need(peer.requests==(mode==1?2U:4U)&&peer.extra_bytes==0,"exact resize requests; no hidden extra sync");
        if(mode==0||mode==1)need(failure.empty()&&width==400&&height==240&&discarded==0,"resize size commit after success");
        else {
            need(width==17&&height==19,"failed resize preserves both cached dimensions");
            if(mode==2)need(failed_with("current video child resize")&&discarded==0,"checked configure error after barrier");
            else if(mode==4)need((failed_with("XCB resize completion connection failed")||
                failed_with("XCB resize completion reply failed")||failed_with("current video child connection"))&&
                (discarded&8),"resize disconnect retires configure sequence");
            else need(failed_with("XCB resize completion reply timed out")&&discarded==24&&elapsed>=800000,
                "shared resize deadline retires configure and barrier");
        }
        need(elapsed<2*G_USEC_PER_SEC,"resize completion reply budget");
    }
    puts("PASS6 owned resize completion controls; shared geometry/configure budget and atomic cached dimensions");
}
enum HandoffMode : unsigned {
    handoff_success, handoff_missing_own, handoff_missing_target,
    handoff_target_error, handoff_foreign_root, handoff_descendant,
    handoff_ancestry_budget, handoff_reparent_error, handoff_missing_reparent,
    handoff_configure_error, handoff_missing_configure, handoff_missing_old_parent,
    handoff_disconnect, handoff_shared_budget, handoff_old_parent_error,
    handoff_modes
};
struct HandoffPeer : GeometryPeer {unsigned extra_bytes=0;};
static constexpr xcb_window_t handoff_root=0x100,handoff_target=0x200103,handoff_middle=0x200104;
static gpointer handoff_peer(gpointer user) {
    auto *p=static_cast<HandoffPeer *>(user);const int fd=p->fd;
    const auto end=g_get_monotonic_time()+3500*1000;
    auto finish=[&]()->gpointer {
        if(p->mode!=handoff_disconnect) {
            while(g_get_monotonic_time()<end) {
                pollfd event{fd,POLLIN,0};const auto ready=poll(&event,1,20);
                if(ready<0&&errno==EINTR)continue;
                if(ready>0) {
                    unsigned char bytes[64];const auto count=recv(fd,bytes,sizeof(bytes),0);
                    if(count==0){p->eof=true;break;}
                    if(count<0)break;
                    p->extra_bytes+=unsigned(count);
                }
            }
            p->expired=!p->eof;
        }
        close(fd);return nullptr;
    };
    if(!plain_peer_setup(fd,end,p->caller))return finish();
    auto request=[&](auto &value,uint8_t opcode,unsigned payload_bytes=0) {
        if(!setup_read(fd,&value,sizeof(value),end)||value.major_opcode!=opcode||
           value.length!=(sizeof(value)+payload_bytes)/4)return false;
        ++p->requests;return true;
    };
    auto geometry=[&](uint16_t sequence,unsigned width,unsigned height,xcb_window_t root=handoff_root) {
        xcb_get_geometry_reply_t value{};value.response_type=1;value.sequence=sequence;
        value.depth=24;value.root=root;value.width=width;value.height=height;
        std::array<unsigned char,32> wire{};memcpy(wire.data(),&value,sizeof(value));
        return send(fd,wire.data(),wire.size(),MSG_NOSIGNAL)==ssize_t(wire.size());
    };
    auto error=[&](uint16_t sequence,uint8_t opcode,xcb_window_t resource) {
        xcb_generic_error_t value{};value.error_code=XCB_WINDOW;value.sequence=sequence;
        value.major_code=opcode;value.resource_id=resource;
        return send(fd,&value,32,MSG_NOSIGNAL)==32;
    };
    auto focus_request=[&]() {
        xcb_get_input_focus_request_t value{};
        return request(value,XCB_GET_INPUT_FOCUS);
    };
    auto focus_reply=[&]() {
        xcb_get_input_focus_reply_t value{};value.response_type=1;
        value.sequence=uint16_t(p->requests);value.focus=handoff_target;
        std::array<unsigned char,32> wire{};memcpy(wire.data(),&value,sizeof(value));
        return send(fd,wire.data(),wire.size(),MSG_NOSIGNAL)==ssize_t(wire.size());
    };
    xcb_get_geometry_request_t own{};
    if(!request(own,XCB_GET_GEOMETRY)||own.drawable!=geometry_child)return finish();
    if(p->mode==handoff_missing_own){p->valid=true;return finish();}
    if(p->mode==handoff_success)g_usleep(300000);
    if(p->mode==handoff_shared_budget)g_usleep(600000);
    if(!geometry(1,200,120))return finish();
    const bool wrapper=p->mode==handoff_descendant||p->mode==handoff_ancestry_budget;
    if(wrapper) {
        for(auto window:{handoff_target,handoff_middle}) {
            xcb_query_tree_request_t value{};
            if(!request(value,XCB_QUERY_TREE)||value.window!=window)return finish();
            if(p->mode==handoff_ancestry_budget)g_usleep(600000);
            xcb_query_tree_reply_t reply{};reply.response_type=1;
            reply.sequence=uint16_t(p->requests);reply.root=handoff_root;
            reply.parent=p->mode==handoff_descendant?geometry_parent:
                window==handoff_target?handoff_middle:handoff_root;
            if(p->mode==handoff_descendant||window==handoff_middle)p->valid=true;
            if(send(fd,&reply,32,MSG_NOSIGNAL)!=32)return finish();
            if(p->mode==handoff_descendant)return finish();
        }
        // An incorrect per-query deadline can continue through the complete
        // exchange. Its accepted state/count must fail the unchanged oracle.
    }
    xcb_get_geometry_request_t target{};
    if(!request(target,XCB_GET_GEOMETRY)||target.drawable!=handoff_target)return finish();
    if(p->mode==handoff_missing_target){p->valid=true;return finish();}
    if(p->mode==handoff_target_error) {
        p->valid=true;(void)error(uint16_t(p->requests),XCB_GET_GEOMETRY,handoff_target);return finish();
    }
    if(!geometry(uint16_t(p->requests),400,240,p->mode==handoff_foreign_root?0x999:handoff_root))return finish();
    if(p->mode==handoff_foreign_root){p->valid=true;return finish();}
    xcb_reparent_window_request_t reparent{};
    if(!request(reparent,XCB_REPARENT_WINDOW)||reparent.window!=geometry_child||
       reparent.parent!=handoff_target||reparent.x||reparent.y)return finish();
    const auto reparent_sequence=uint16_t(p->requests);
    if(!focus_request())return finish();
    if(p->mode==handoff_missing_reparent||p->mode==handoff_disconnect) {
        p->valid=true;return finish();
    }
    if(p->mode==handoff_reparent_error) {
        p->valid=true;
        if(error(reparent_sequence,XCB_REPARENT_WINDOW,geometry_child))(void)focus_reply();
        return finish();
    }
    if(p->mode==handoff_success)g_usleep(300000);
    if(p->mode==handoff_shared_budget){p->valid=true;g_usleep(600000);}
    if(!focus_reply())return finish();
    xcb_configure_window_request_t configure{};uint32_t size[2]{};
    if(!request(configure,XCB_CONFIGURE_WINDOW,sizeof(size))||configure.window!=geometry_child||
       configure.value_mask!=(XCB_CONFIG_WINDOW_WIDTH|XCB_CONFIG_WINDOW_HEIGHT)||
       !setup_read(fd,size,sizeof(size),end)||size[0]!=400||size[1]!=240)return finish();
    const auto configure_sequence=uint16_t(p->requests);
    if(!focus_request())return finish();
    if(p->mode==handoff_missing_configure){p->valid=true;return finish();}
    if(p->mode==handoff_configure_error) {
        p->valid=true;
        if(error(configure_sequence,XCB_CONFIGURE_WINDOW,geometry_child))(void)focus_reply();
        return finish();
    }
    if(!focus_reply())return finish();
    if(!wrapper) {
        xcb_change_window_attributes_request_t attributes{};uint32_t mask=1;
        if(!request(attributes,XCB_CHANGE_WINDOW_ATTRIBUTES,sizeof(mask))||
           attributes.window!=geometry_parent||attributes.value_mask!=XCB_CW_EVENT_MASK||
           !setup_read(fd,&mask,sizeof(mask),end)||mask)return finish();
        const auto attributes_sequence=uint16_t(p->requests);
        if(!focus_request())return finish();
        if(p->mode==handoff_missing_old_parent){p->valid=true;return finish();}
        if(p->mode==handoff_old_parent_error&&
           !error(attributes_sequence,XCB_CHANGE_WINDOW_ATTRIBUTES,geometry_parent))return finish();
        if(!focus_reply())return finish();
    }
    xcb_change_window_attributes_request_t attributes{};uint32_t mask=0;
    if(!request(attributes,XCB_CHANGE_WINDOW_ATTRIBUTES,sizeof(mask))||
       attributes.window!=handoff_target||attributes.value_mask!=XCB_CW_EVENT_MASK||
       !setup_read(fd,&mask,sizeof(mask),end)||mask!=XCB_EVENT_MASK_STRUCTURE_NOTIFY)return finish();
    xcb_map_window_request_t map{};
    if(!request(map,XCB_MAP_WINDOW)||map.window!=geometry_child)return finish();
    if(wrapper) {
        xcb_destroy_window_request_t destroy{};
        if(!request(destroy,XCB_DESTROY_WINDOW)||destroy.window!=geometry_parent)return finish();
    }
    p->valid=true;return finish();
}
static void handoff_exchanges() {
    for(unsigned mode=0;mode<handoff_modes;++mode) {
        const auto before=setup_fd_count();int sockets[2]{};
        need(!socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,sockets),"owned handoff socket pair");
        HandoffPeer peer{};peer.fd=sockets[1];peer.mode=mode;
        auto *server=g_thread_new("owned-field-handoff",handoff_peer,&peer);
        auto *connection=xcb_connect_to_fd(sockets[0],nullptr);
        const bool connected=!xcb_connection_has_error(connection);
        const bool wrapper=mode==handoff_descendant||mode==handoff_ancestry_budget;
        xcb_window_t parent=geometry_parent,standalone=wrapper?geometry_parent:0;
        bool embedded=!wrapper,accepted=false,created=false;
        unsigned width=17,height=19;std::string failure;discarded_sequences=0;
        const auto start=g_get_monotonic_time();
        try {
            if(connected)accepted=crystalhd_field::replace_window_bounded(connection,geometry_child,handoff_target,
                parent,standalone,embedded,width,height,[&](xcb_window_t,gint64)->xcb_window_t {
                    created=true;throw std::runtime_error("unexpected standalone handoff candidate");
                });
        }catch(const std::exception &e){failure=e.what();}
        const auto elapsed=g_get_monotonic_time()-start;const auto discarded=discarded_sequences.load();
        xcb_disconnect(connection);g_thread_join(server);
        printf("field handoff mode=%u elapsed_us=%lld requests=%u extra=%u accepted=%u parent=%u standalone=%u embedded=%u cache=%ux%u discarded=%llu failure=%s\n",
            mode,static_cast<long long>(elapsed),peer.requests,peer.extra_bytes,unsigned(accepted),parent,standalone,
            unsigned(embedded),width,height,static_cast<unsigned long long>(discarded),failure.c_str());
        need(connected&&peer.valid&&peer.caller==getpid()&&!peer.expired&&!created,
            "owned handoff handshake/request/PID/retirement");
        need((mode==handoff_disconnect||peer.eof)&&setup_fd_count()==before,"handoff connection/thread/FD retirement");
        need(peer.extra_bytes==0,"no hidden handoff sync or mutation after rejection");
        const unsigned requests[]={10,1,2,2,2,2,3,4,4,6,6,8,4,4,10};
        need(peer.requests==requests[mode],"exact handoff phase requests");
        if(mode==handoff_success||mode==handoff_old_parent_error) {
            need(accepted&&failure.empty()&&parent==handoff_target&&!standalone&&embedded&&
                width==400&&height==240&&discarded==0,"handoff commits attachment only after completion");
            if(mode==handoff_success)need(elapsed>=400000,"delayed successful handoff exercised");
        }else {
            need(!accepted&&parent==geometry_parent&&standalone==(wrapper?geometry_parent:0)&&
                embedded==!wrapper&&width==17&&height==19,"failed handoff preserves all local attachment fields");
            if(mode<=handoff_ancestry_budget||mode==handoff_reparent_error)
                need(failure.empty(),"read-only or explicitly rejected handoff is a safe rejection");
            else {
                need(!failure.empty(),"indeterminate mutation or later failure is fatal");
                const char *phase=mode==handoff_configure_error||mode==handoff_missing_configure?"configure":
                    mode==handoff_missing_old_parent?"old parent":"reparent";
                need(failure.find(phase)!=std::string::npos,"handoff fatal error identifies its phase");
            }
        }
        if(mode==handoff_missing_own)need(discarded==2,"missing own geometry retires its sequence");
        if(mode==handoff_missing_target)need(discarded==4,"missing target geometry retires its sequence");
        if(mode==handoff_ancestry_budget)need(discarded==8,"ancestry shares the original deadline");
        if(mode==handoff_missing_reparent||mode==handoff_shared_budget)
            need(discarded==24,"unconfirmed reparent retires checked request and barrier");
        if(mode==handoff_missing_configure)need(discarded==96,"unconfirmed configure retires both sequences");
        if(mode==handoff_missing_old_parent)need(discarded==384,"old parent cleanup retires both sequences");
        if(mode==handoff_disconnect)need(discarded&8,"disconnect retires pending reparent request");
        if(mode==handoff_missing_own||mode==handoff_missing_target||mode==handoff_ancestry_budget||
           mode==handoff_missing_reparent||mode==handoff_missing_configure||
           mode==handoff_missing_old_parent||mode==handoff_shared_budget)
            need(elapsed>=800000,"handoff deadline path exercised");
        need(elapsed<2*G_USEC_PER_SEC,"whole handoff response wait budget");
    }
    puts("PASS15 owned handoff controls; shared response budget, safe preflight rejection and fatal uncertain mutation");
}
enum ConstructionMode : unsigned {
    construction_success,construction_geometry_late,construction_geometry_missing,
    construction_create_late,construction_create_missing,construction_create_error,
    construction_atom_late,construction_standalone_success,construction_reparent_late,
    construction_reparent_missing,construction_reparent_error,construction_modes
};
struct ConstructionPeer : GeometryPeer {unsigned extra_bytes=0;};
static gpointer construction_peer(gpointer user) {
    auto *p=static_cast<ConstructionPeer *>(user);const int fd=p->fd;
    const auto end=g_get_monotonic_time()+3500*1000;
    auto finish=[&]()->gpointer {
        while(g_get_monotonic_time()<end) {
            pollfd event{fd,POLLIN,0};const auto ready=poll(&event,1,20);
            if(ready<0&&errno==EINTR)continue;
            if(ready>0) {
                unsigned char bytes[64];const auto count=recv(fd,bytes,sizeof(bytes),0);
                if(count==0){p->eof=true;break;}
                if(count<0)break;
                p->extra_bytes+=unsigned(count);
            }
        }
        p->expired=!p->eof;close(fd);return nullptr;
    };
    if(!plain_peer_setup(fd,end,p->caller))return finish();
    auto request=[&](auto &value,uint8_t opcode,unsigned payload=0) {
        if(!setup_read(fd,&value,sizeof(value),end)||value.major_opcode!=opcode||
           value.length!=(sizeof(value)+payload)/4)return false;
        ++p->requests;return true;
    };
    auto wire_reply=[&](const auto &value) {
        std::array<unsigned char,32> wire{};memcpy(wire.data(),&value,sizeof(value));
        return send(fd,wire.data(),wire.size(),MSG_NOSIGNAL)==ssize_t(wire.size());
    };
    auto checked_create=[&](xcb_window_t window,xcb_window_t parent,unsigned delay,bool missing,bool error) {
        xcb_create_window_request_t create{};
        if(!request(create,XCB_CREATE_WINDOW)||create.wid!=window||create.parent!=parent||
           create.value_mask||create.width!=400||create.height!=240)return false;
        const auto checked_sequence=uint16_t(p->requests);
        xcb_get_input_focus_request_t focus{};
        if(!request(focus,XCB_GET_INPUT_FOCUS))return false;
        p->valid=true;
        if(missing)return false;
        if(delay)g_usleep(delay);
        if(error) {
            xcb_generic_error_t failure{};failure.error_code=XCB_WINDOW;
            failure.sequence=checked_sequence;failure.major_code=XCB_CREATE_WINDOW;failure.resource_id=parent;
            if(send(fd,&failure,32,MSG_NOSIGNAL)!=32)return false;
        }
        xcb_get_input_focus_reply_t reply{};reply.response_type=1;
        reply.sequence=uint16_t(p->requests);reply.focus=parent;
        return wire_reply(reply);
    };
    const bool standalone=p->mode>=construction_standalone_success;
    if(!standalone) {
        xcb_get_geometry_request_t geometry{};
        if(!request(geometry,XCB_GET_GEOMETRY)||geometry.drawable!=geometry_parent)return finish();
        p->valid=true;
        if(p->mode==construction_geometry_missing)return finish();
        if(p->mode==construction_geometry_late)g_usleep(1200000);
        if(p->mode==construction_create_late)g_usleep(600000);
        if(p->mode==construction_success||p->mode==construction_atom_late)g_usleep(200000);
        xcb_get_geometry_reply_t reply{};reply.response_type=1;reply.sequence=1;
        reply.depth=24;reply.root=handoff_root;reply.width=400;reply.height=240;
        if(!wire_reply(reply))return finish();
    }
    if(!checked_create(geometry_child,geometry_parent,
            p->mode==construction_create_late?600000:
            p->mode==construction_success||p->mode==construction_atom_late||standalone?200000:0,
            p->mode==construction_create_missing,p->mode==construction_create_error))return finish();
    if(p->mode==construction_create_error)return finish();
    xcb_intern_atom_request_t atom{};unsigned char name[16]{};
    if(!request(atom,XCB_INTERN_ATOM,12)||atom.only_if_exists||atom.name_len!=12||
       !setup_read(fd,name,12,end)||memcmp(name,"WM_PROTOCOLS",12))return finish();
    if(p->mode==construction_atom_late)g_usleep(800000);
    else if(p->mode==construction_success)g_usleep(200000);
    xcb_intern_atom_reply_t atom_reply{};atom_reply.response_type=1;
    atom_reply.sequence=uint16_t(p->requests);atom_reply.atom=0x70ef;
    if(!wire_reply(atom_reply))return finish();
    if(standalone) {
        if(!checked_create(handoff_target,handoff_root,
                p->mode==construction_reparent_late?400000:200000,false,false))return finish();
        xcb_reparent_window_request_t reparent{};
        if(!request(reparent,XCB_REPARENT_WINDOW)||reparent.window!=geometry_child||
           reparent.parent!=handoff_target||reparent.x||reparent.y)return finish();
        const auto sequence=uint16_t(p->requests);
        xcb_get_input_focus_request_t focus{};
        if(!request(focus,XCB_GET_INPUT_FOCUS))return finish();
        if(p->mode==construction_reparent_missing)return finish();
        if(p->mode==construction_reparent_late)g_usleep(600000);
        else g_usleep(200000);
        if(p->mode==construction_reparent_error) {
            xcb_generic_error_t error{};error.error_code=XCB_WINDOW;error.sequence=sequence;
            error.major_code=XCB_REPARENT_WINDOW;error.resource_id=handoff_target;
            if(send(fd,&error,32,MSG_NOSIGNAL)!=32)return finish();
        }
        xcb_get_input_focus_reply_t reply{};reply.response_type=1;
        reply.sequence=uint16_t(p->requests);reply.focus=handoff_target;
        if(!wire_reply(reply))return finish();
    }
    return finish();
}
static void construction_exchanges() {
    for(unsigned mode=0;mode<construction_modes;++mode) {
        const auto before=setup_fd_count();int sockets[2]{};
        need(!socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,sockets),"owned constructor socket pair");
        ConstructionPeer peer{};peer.fd=sockets[1];peer.mode=mode;
        auto *server=g_thread_new("owned-field-construction",construction_peer,&peer);
        auto *connection=xcb_connect_to_fd(sockets[0],nullptr);
        const bool connected=!xcb_connection_has_error(connection);bool accepted=false;
        const bool standalone=mode>=construction_standalone_success;
        std::string failure;discarded_sequences=0;const auto start=g_get_monotonic_time();
        try {
            need(connected,"constructor peer connection");
            crystalhd_field::ConstructionReplies replies{connection};
            if(!standalone) {
                auto geometry=replies.geometry(geometry_parent);
                need(bool(geometry)&&geometry->width==400&&geometry->height==240,"constructor geometry values");
            }
            auto create=[&](xcb_window_t window,xcb_window_t parent) {
                auto error=replies.check(xcb_create_window_checked(connection,XCB_COPY_FROM_PARENT,
                    window,parent,0,0,400,240,0,XCB_WINDOW_CLASS_INPUT_OUTPUT,
                    XCB_COPY_FROM_PARENT,0,nullptr),"XCB construction create");
                need(!error,"constructor checked create error");
            };
            create(geometry_child,geometry_parent);
            need(replies.atom("WM_PROTOCOLS")==0x70ef,"constructor atom value");
            if(standalone) {
                create(handoff_target,handoff_root);
                auto error=replies.check(xcb_reparent_window_checked(connection,
                    geometry_child,handoff_target,0,0),"XCB construction reparent");
                need(!error,"constructor checked reparent error");
            }
            accepted=true;
        }catch(const std::exception &e){failure=e.what();}
        const auto elapsed=g_get_monotonic_time()-start;const auto discarded=discarded_sequences.load();
        xcb_disconnect(connection);g_thread_join(server);
        printf("field construction mode=%u accepted=%u elapsed_us=%lld requests=%u extra=%u discarded=%llu failure=%s\n",
            mode,accepted,static_cast<long long>(elapsed),peer.requests,peer.extra_bytes,
            static_cast<unsigned long long>(discarded),failure.c_str());
        need(connected&&peer.valid&&peer.caller==getpid()&&peer.eof&&!peer.expired&&
            setup_fd_count()==before,"constructor peer/PID/thread/FD retirement");
        const bool success=mode==construction_success||mode==construction_standalone_success;
        auto exact_error=[&](const char *label) {
            const auto prefix=std::string(label)+" errno=";
            return failure.rfind(prefix,0)==0&&failure.size()>prefix.size()&&
                std::all_of(failure.begin()+prefix.size(),failure.end(),[](char c){return c>='0'&&c<='9';});
        };
        if(success)need(accepted&&failure.empty()&&!discarded,"constructor replies before shared deadline succeed");
        else {
            need(!accepted,"constructor must reject late or missing replies within its shared response deadline");
            if(mode==construction_create_error)need(exact_error("constructor checked create error"),"constructor create error preserved");
            else if(mode==construction_reparent_error)need(exact_error("constructor checked reparent error"),"constructor reparent error preserved");
            else need(failure.find("timed out")!=std::string::npos&&elapsed>=800000,"constructor response deadline exercised");
        }
        need(elapsed<2*G_USEC_PER_SEC,"constructor shared reply wait budget");
        const unsigned requests=standalone?7:
            mode==construction_geometry_late||mode==construction_geometry_missing?1:
            mode==construction_create_late||mode==construction_create_missing||mode==construction_create_error?3:4;
        need(peer.requests==requests,"exact constructor requests and completion barriers");
        const uint64_t expected_discard=mode==construction_geometry_late||mode==construction_geometry_missing?2:
            mode==construction_create_late||mode==construction_create_missing?12:
            mode==construction_atom_late?16:
            mode==construction_reparent_late||mode==construction_reparent_missing?192:0;
        need(discarded==expected_discard,"constructor retires only its uncompleted response sequences");
        need(peer.extra_bytes==0,"constructor has no unaccounted reply synchronization");
    }
    puts("PASS11 owned constructor response controls; shared geometry/create/atom/reparent budget, no EGL/device access");
}
int main(int argc,char **argv) {
 try {
    setvbuf(stdout,nullptr,_IOLBF,0);const bool construction_only=argc==2&&!strcmp(argv[1],"--construction");
    const bool keyboard_only=argc==2&&!strcmp(argv[1],"--keyboard");
    need(argc==1||construction_only||keyboard_only||(argc==2&&!strcmp(argv[1],"--x11")),"usage: field-setup-test [--x11|--construction|--keyboard]");
    if(keyboard_only){keyboard_exchanges();return 0;}
    if(!construction_only) {
        exchanges();atom_exchanges();keyboard_exchanges();geometry_exchanges();resize_exchanges();handoff_exchanges();
    }
    construction_exchanges();
    if(argc==2&&!construction_only) {
        need(temperature()<75000,"thermal admission");
        {crystalhd_field::Window window(0);need(window.context!=EGL_NO_CONTEXT,"valid display after setup timeouts");}
        puts("PASS valid actual X11/EGL Window after6 setup,7 atom,9 keyboard,9 geometry,6 resize,15 handoff and11 constructor controls in same process");
    }
 }catch(const std::exception &e){fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
