// SPDX-License-Identifier: GPL-2.0-or-later
// Shared GLES YUYV probe helpers; not installed or linked into production.
#ifndef CRYSTALHD_TEST_YUY2_GPU_H
#define CRYSTALHD_TEST_YUY2_GPU_H
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
namespace yuy2_gpu {
inline void need(bool b, const char *s) {
    if (!b) throw std::runtime_error(std::string(s) + " errno=" + std::to_string(errno));
}
struct GPU {
 int drm=-1;gbm_device* gbm=nullptr;EGLDisplay display=EGL_NO_DISPLAY;EGLContext context=EGL_NO_CONTEXT;
 bool owns_context=true;
 PFNEGLCREATEIMAGEKHRPROC create=nullptr;PFNEGLDESTROYIMAGEKHRPROC destroy=nullptr;
 PFNGLEGLIMAGETARGETTEXTURE2DOESPROC target=nullptr;
 GPU(const char* node) {
  drm=open(node,O_RDWR|O_CLOEXEC);need(drm>=0,"render node");gbm=gbm_create_device(drm);need(gbm,"GBM device");
  auto get=(PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");need(get,"platform display");
  display=get(EGL_PLATFORM_GBM_KHR,gbm,nullptr);EGLint major=0,minor=0;need(eglInitialize(display,&major,&minor),"EGL init");
  need(eglBindAPI(EGL_OPENGL_ES_API),"bind GLES");
  EGLint attrs[]={EGL_SURFACE_TYPE,EGL_DONT_CARE,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
  EGLConfig cfg{};EGLint n=0;need(eglChooseConfig(display,attrs,&cfg,1,&n)&&n==1,"EGL config");
  EGLint ca[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};context=eglCreateContext(display,cfg,EGL_NO_CONTEXT,ca);need(context!=EGL_NO_CONTEXT,"EGL context");
  need(eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,context),"surfaceless context");
  resolve();
 }
 GPU(const char* node,EGLDisplay borrowed_display,EGLContext borrowed_context):display(borrowed_display),context(borrowed_context),owns_context(false) {
  need(display!=EGL_NO_DISPLAY&&context!=EGL_NO_CONTEXT&&eglGetCurrentDisplay()==display&&eglGetCurrentContext()==context,"current borrowed EGL context");
  try {drm=open(node,O_RDWR|O_CLOEXEC);need(drm>=0,"render node");gbm=gbm_create_device(drm);need(gbm,"GBM device");resolve();}
  catch(...) {if(gbm){gbm_device_destroy(gbm);gbm=nullptr;}if(drm>=0){close(drm);drm=-1;}throw;}
 }
 void resolve() {
  const char* renderer=(const char*)glGetString(GL_RENDERER);need(renderer&&std::strstr(renderer,"Intel"),"real Intel GPU required");
  std::printf("{\"type\":\"gpu\",\"renderer\":\"%s\",\"gles\":\"%s\"}\n",renderer,glGetString(GL_VERSION));
  create=(PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");destroy=(PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");target=(PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");need(create&&destroy&&target,"EGL image functions");
 }
 ~GPU(){if(owns_context&&display!=EGL_NO_DISPLAY){eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);if(context!=EGL_NO_CONTEXT)eglDestroyContext(display,context);eglTerminate(display);}if(gbm)gbm_device_destroy(gbm);if(drm>=0)close(drm);}
};
static GLuint shader(GLenum type,const char* text) {
 GLuint s=glCreateShader(type);glShaderSource(s,1,&text,nullptr);glCompileShader(s);GLint ok=0;glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
 if(!ok){char log[2048]{};glGetShaderInfoLog(s,sizeof(log),nullptr,log);std::fprintf(stderr,"shader: %s\n",log);}need(ok,"shader compile");return s;
}
static GLuint program(bool external) {
 const char* vs="attribute vec2 pos;varying vec2 uv;void main(){uv=(pos+1.0)*0.5;gl_Position=vec4(pos,0.0,1.0);}";
 const char* packed="precision highp float;varying vec2 uv;uniform sampler2D tex;uniform float width;void main(){vec4 p=texture2D(tex,uv);float y=mod(floor(uv.x*width),2.0)<1.0?p.r:p.b;float c=(y-16.0/255.0)*(255.0/219.0);float u=(p.g-128.0/255.0)*(255.0/224.0);float v=(p.a-128.0/255.0)*(255.0/224.0);gl_FragColor=vec4(c+1.5748*v,c-0.187324*u-0.468124*v,c+1.8556*u,1.0);}";
 const char* ext="#extension GL_OES_EGL_image_external : require\nprecision highp float;varying vec2 uv;uniform samplerExternalOES tex;void main(){gl_FragColor=vec4(texture2D(tex,uv).rgb,1.0);}";
 GLuint v=shader(GL_VERTEX_SHADER,vs),f=shader(GL_FRAGMENT_SHADER,external?ext:packed),p=glCreateProgram();glAttachShader(p,v);glAttachShader(p,f);glBindAttribLocation(p,0,"pos");glLinkProgram(p);GLint ok=0;glGetProgramiv(p,GL_LINK_STATUS,&ok);need(ok,"shader link");glDeleteShader(v);glDeleteShader(f);return p;
}
struct Picture {
 unsigned w,h;GLuint fbo=0,output=0,packed=0,external_program=0,packed_program=0;
 Picture(unsigned width,unsigned height):w(width),h(height) {
  external_program=program(true);packed_program=program(false);
  glGenTextures(1,&output);glBindTexture(GL_TEXTURE_2D,output);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
  glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,output,0);need(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,"output FBO");
  glGenTextures(1,&packed);glBindTexture(GL_TEXTURE_2D,packed);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,w/2,h,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);need(glGetError()==GL_NO_ERROR,"picture allocation");
 }
 void draw(bool external,GLuint texture) {
  GLuint p=external?external_program:packed_program;glBindFramebuffer(GL_FRAMEBUFFER,fbo);glViewport(0,0,w,h);glUseProgram(p);glActiveTexture(GL_TEXTURE0);glBindTexture(external?GL_TEXTURE_EXTERNAL_OES:GL_TEXTURE_2D,texture);glUniform1i(glGetUniformLocation(p,"tex"),0);if(!external)glUniform1f(glGetUniformLocation(p,"width"),w);
  static const float positions[]={-1,-1,1,-1,-1,1,1,1};glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,positions);glEnableVertexAttribArray(0);glDrawArrays(GL_TRIANGLE_STRIP,0,4);glFinish();need(glGetError()==GL_NO_ERROR,"draw/finish");
 }
 void verify(const std::vector<uint8_t>& source,const char* path) {
  std::vector<uint8_t> pixels(size_t(w)*h*4);glReadPixels(0,0,w,h,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());need(glGetError()==GL_NO_ERROR,"RGB readback");
  unsigned maximum=0;size_t over=0;unsigned long long sum=0;size_t alpha=0;
  for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){size_t q=(size_t(y)*w+(x/2)*2)*2;double yy=(source[q+(x%2)*2]-16)*255.0/219,u=(source[q+1]-128)*255.0/224,v=(source[q+3]-128)*255.0/224;double expected[]={yy+1.5748*v,yy-0.187324*u-0.468124*v,yy+1.8556*u};for(unsigned c=0;c<3;++c){int value=std::clamp((int)std::lround(expected[c]),0,255);unsigned error=std::abs((int)pixels[(size_t(y)*w+x)*4+c]-value);maximum=std::max(maximum,error);sum+=error;if(error>2 && over<8)std::fprintf(stderr,"pixel path=%s xy=%u,%u channel=%u got=%u expected=%d YUV=%u/%u/%u\n",path,x,y,c,pixels[(size_t(y)*w+x)*4+c],value,source[q+(x%2)*2],source[q+1],source[q+3]);over+=error>2;}alpha+=pixels[(size_t(y)*w+x)*4+3]!=255;}
  std::printf("{\"type\":\"pixels\",\"path\":\"%s\",\"width\":%u,\"height\":%u,\"max_error\":%u,\"mean_error\":%.9f,\"channels_over2\":%zu,\"nonopaque\":%zu}\n",path,w,h,maximum,double(sum)/(size_t(w)*h*3),over,alpha);need(over==0&&alpha==0,"pixel oracle fixed maximum2");
 }
 ~Picture(){glDeleteTextures(1,&packed);glDeleteFramebuffers(1,&fbo);glDeleteTextures(1,&output);glDeleteProgram(external_program);glDeleteProgram(packed_program);}
};

// Borrows the export FD. The caller retains it through glFinish and image
// destruction, and must not requeue the capture buffer while GPU work is live.
struct Imported {
    GPU &gpu;
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    GLuint texture = 0;
    Imported(GPU &g, int fd, unsigned width, unsigned height, unsigned pitch)
        : gpu(g)
    {
        EGLint attrs[] = {
            EGL_WIDTH, EGLint(width), EGL_HEIGHT, EGLint(height),
            EGL_LINUX_DRM_FOURCC_EXT, EGLint(DRM_FORMAT_YUYV),
            EGL_DMA_BUF_PLANE0_FD_EXT, fd,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
            EGL_DMA_BUF_PLANE0_PITCH_EXT, EGLint(pitch),
            EGL_YUV_COLOR_SPACE_HINT_EXT, EGL_ITU_REC709_EXT,
            EGL_SAMPLE_RANGE_HINT_EXT, EGL_YUV_NARROW_RANGE_EXT,
            EGL_YUV_CHROMA_HORIZONTAL_SITING_HINT_EXT, EGL_YUV_CHROMA_SITING_0_5_EXT,
            EGL_YUV_CHROMA_VERTICAL_SITING_HINT_EXT, EGL_YUV_CHROMA_SITING_0_5_EXT,
            EGL_NONE
        };
        image = gpu.create(gpu.display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
                           nullptr, attrs);
        need(image != EGL_NO_IMAGE_KHR, "capture YUYV image import");
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture);
        gpu.target(GL_TEXTURE_EXTERNAL_OES, image);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        need(glGetError() == GL_NO_ERROR, "capture external texture");
    }
    Imported(const Imported &) = delete;
    Imported &operator=(const Imported &) = delete;
    ~Imported() {
        glFinish();
        if (texture) glDeleteTextures(1, &texture);
        if (image != EGL_NO_IMAGE_KHR) gpu.destroy(gpu.display, image);
    }
};
} // namespace yuy2_gpu
#endif
