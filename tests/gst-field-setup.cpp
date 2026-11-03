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
int main(int argc,char **argv) {
 try {
    setvbuf(stdout,nullptr,_IOLBF,0);need(argc==1||(argc==2&&!strcmp(argv[1],"--x11")),"usage: field-setup-test [--x11]");
    exchanges();
    atom_exchanges();
    geometry_exchanges();
    if(argc==2) {
        need(temperature()<75000,"thermal admission");
        {crystalhd_field::Window window(0);need(window.context!=EGL_NO_CONTEXT,"valid display after setup timeouts");}
        puts("PASS valid actual X11/EGL Window after6 setup,7 atom and9 geometry controls in same process");
    }
 }catch(const std::exception &e){fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
