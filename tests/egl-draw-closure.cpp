// SPDX-License-Identifier: GPL-2.0-or-later
// Independent owned GLES objects: no CrystalHD/GStreamer/project headers.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <gbm.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <thread>
#include <cstdlib>
static void require(bool value,const char *what){if(!value){fprintf(stderr,"FAIL %s EGL=%x GL=%x\n",what,eglGetError(),glGetError());std::abort();}}
static GLuint shader(GLenum type,const char *text){GLuint s=glCreateShader(type);glShaderSource(s,1,&text,nullptr);glCompileShader(s);GLint ok=0;glGetShaderiv(s,GL_COMPILE_STATUS,&ok);require(ok,"compile");return s;}
static void cycle(bool draw,bool vbo) {
 int fd=open("/dev/dri/renderD128",O_RDWR|O_CLOEXEC);require(fd>=0,"own render node");
 auto *gbm=gbm_create_device(fd);require(gbm,"own GBM device");
 auto get=reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));require(get,"platform entry");
 auto display=get(EGL_PLATFORM_GBM_KHR,gbm,nullptr);EGLint major=0,minor=0;require(eglInitialize(display,&major,&minor),"init");
 require(eglBindAPI(EGL_OPENGL_ES_API),"bind");
 EGLint attrs[]={EGL_SURFACE_TYPE,EGL_DONT_CARE,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_NONE};
 EGLConfig config{};EGLint count=0;require(eglChooseConfig(display,attrs,&config,1,&count)&&count==1,"config");
 EGLint context_attrs[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};auto context=eglCreateContext(display,config,EGL_NO_CONTEXT,context_attrs);require(context!=EGL_NO_CONTEXT,"context");
 require(eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,context),"current");
 printf("renderer=%s draw=%u vbo=%u\n",glGetString(GL_RENDERER),draw,vbo);
 GLuint texture=0,fbo=0;glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_2D,texture);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,16,16,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
 glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
 glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);require(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,"FBO");
 GLuint vs=shader(GL_VERTEX_SHADER,"attribute vec2 pos;void main(){gl_Position=vec4(pos,0.,1.);}"),fs=shader(GL_FRAGMENT_SHADER,"precision mediump float;void main(){gl_FragColor=vec4(.25,.5,.75,1.);}");
 GLuint program=glCreateProgram();glAttachShader(program,vs);glAttachShader(program,fs);glBindAttribLocation(program,0,"pos");glLinkProgram(program);GLint linked=0;glGetProgramiv(program,GL_LINK_STATUS,&linked);require(linked,"link");
 glDeleteShader(vs);glDeleteShader(fs);glUseProgram(program);glViewport(0,0,16,16);
 static const float positions[]={-1,-1,1,-1,-1,1,1,1};GLuint vertices=0;
 if(vbo){glGenBuffers(1,&vertices);glBindBuffer(GL_ARRAY_BUFFER,vertices);glBufferData(GL_ARRAY_BUFFER,sizeof(positions),positions,GL_STATIC_DRAW);}
 glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,vbo?nullptr:positions);glEnableVertexAttribArray(0);
 if(draw){glDrawArrays(GL_TRIANGLE_STRIP,0,4);}
 glFinish();require(glGetError()==GL_NO_ERROR,"draw completion");
 glDisableVertexAttribArray(0);glBindBuffer(GL_ARRAY_BUFFER,0);glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,positions);
 if(vertices){glDeleteBuffers(1,&vertices);require(!glIsBuffer(vertices),"vertex buffer deleted");}
 glUseProgram(0);glDeleteProgram(program);require(!glIsProgram(program)&&!glIsShader(vs)&&!glIsShader(fs),"program/shaders deleted");
 glBindFramebuffer(GL_FRAMEBUFFER,0);glDeleteFramebuffers(1,&fbo);glBindTexture(GL_TEXTURE_2D,0);glDeleteTextures(1,&texture);
 require(!glIsFramebuffer(fbo)&&!glIsTexture(texture)&&glGetError()==GL_NO_ERROR,"FBO/texture deleted");
 require(eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT),"unbind");
 require(eglDestroyContext(display,context),"destroy context");require(eglTerminate(display),"terminate");require(eglReleaseThread(),"release thread");
 gbm_device_destroy(gbm);require(close(fd)==0,"close render node");puts("all own GL/EGL/GBM/FD objects retired; API errors0");
}
int main(int argc,char **argv){setvbuf(stdout,nullptr,_IOLBF,0);bool draw=true,vbo=false;for(int i=1;i<argc;i++){if(!strcmp(argv[i],"--no-draw"))draw=false;else if(!strcmp(argv[i],"--vbo"))vbo=true;else return 2;}for(int run=0;run<2;run++){std::thread worker([&]{cycle(draw,vbo);});worker.join();}return 0;}
