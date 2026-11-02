// SPDX-License-Identifier: GPL-2.0-or-later
// Actual own Window constructor against private owned peers; no GL/device is
// reached unless --x11 explicitly requests a final valid display reopen.
#include "../filters/gst/field-output/gstcrystalhdfield.cpp"
#include "x11-setup-peer.h"
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
int main(int argc,char **argv) {
 try {
    setvbuf(stdout,nullptr,_IOLBF,0);need(argc==1||(argc==2&&!strcmp(argv[1],"--x11")),"usage: field-setup-test [--x11]");
    exchanges();
    if(argc==2) {
        need(temperature()<75000,"thermal admission");
        {crystalhd_field::Window window(0);need(window.context!=EGL_NO_CONTEXT,"valid display after setup timeouts");}
        puts("PASS valid actual X11/EGL Window after6 setup timeouts in same process");
    }
 }catch(const std::exception &e){fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
