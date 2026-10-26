// SPDX-License-Identifier: GPL-2.0-or-later
// Opt-in GPU memory-path experiment, not a decoder or V4L2 conformance test.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>
#include <drm_fourcc.h>
#include <cpuid.h>
#include <linux/dma-heap.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
static void need(bool b,const char* s) { if(!b) throw std::runtime_error(std::string(s)+" errno="+std::to_string(errno)); }
static double seconds(clockid_t c) { timespec t{};need(clock_gettime(c,&t)==0,"clock");return t.tv_sec+t.tv_nsec/1e9; }
static int temperature() {
 int maximum=0;
 for(const auto& e:std::filesystem::directory_iterator("/sys/class/thermal")) {
  if(e.path().filename().string().rfind("thermal_zone",0)!=0) continue;
  int value=0;std::ifstream(e.path()/"temp")>>value;maximum=std::max(maximum,value);
 }
 return maximum;
}
struct GPU {
 int drm=-1;gbm_device* gbm=nullptr;EGLDisplay display=EGL_NO_DISPLAY;EGLContext context=EGL_NO_CONTEXT;
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
  const char* renderer=(const char*)glGetString(GL_RENDERER);need(renderer&&std::strstr(renderer,"Intel"),"real Intel GPU required");
  std::printf("{\"type\":\"gpu\",\"renderer\":\"%s\",\"gles\":\"%s\"}\n",renderer,glGetString(GL_VERSION));
  create=(PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");destroy=(PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");target=(PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");need(create&&destroy&&target,"EGL image functions");
 }
 ~GPU(){if(display!=EGL_NO_DISPLAY){eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);if(context!=EGL_NO_CONTEXT)eglDestroyContext(display,context);eglTerminate(display);}if(gbm)gbm_device_destroy(gbm);if(drm>=0)close(drm);}
};
struct Shared {
 GPU& gpu;int fd=-1;unsigned width,height,pitch;size_t size;uint8_t* data=nullptr;EGLImageKHR image=EGL_NO_IMAGE_KHR;GLuint texture=0;
 Shared(GPU& g,int heap,unsigned w,unsigned h):gpu(g),width(w),height(h),pitch((w*2+63)&~63U),size((size_t(pitch)*h+4095)&~size_t(4095)) {
  dma_heap_allocation_data a{};a.len=size;a.fd_flags=O_RDWR|O_CLOEXEC;need(ioctl(heap,DMA_HEAP_IOCTL_ALLOC,&a)==0,"heap allocation");fd=a.fd;
  data=(uint8_t*)mmap(nullptr,size,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);need(data!=MAP_FAILED,"DMA mmap");
 }
 void import() {
  EGLint attrs[]={EGL_WIDTH,(EGLint)width,EGL_HEIGHT,(EGLint)height,EGL_LINUX_DRM_FOURCC_EXT,(EGLint)DRM_FORMAT_YUYV,EGL_DMA_BUF_PLANE0_FD_EXT,fd,EGL_DMA_BUF_PLANE0_OFFSET_EXT,0,EGL_DMA_BUF_PLANE0_PITCH_EXT,(EGLint)pitch,EGL_YUV_COLOR_SPACE_HINT_EXT,EGL_ITU_REC709_EXT,EGL_SAMPLE_RANGE_HINT_EXT,EGL_YUV_NARROW_RANGE_EXT,EGL_YUV_CHROMA_HORIZONTAL_SITING_HINT_EXT,EGL_YUV_CHROMA_SITING_0_5_EXT,EGL_YUV_CHROMA_VERTICAL_SITING_HINT_EXT,EGL_YUV_CHROMA_SITING_0_5_EXT,EGL_NONE};
  image=gpu.create(gpu.display,EGL_NO_CONTEXT,EGL_LINUX_DMA_BUF_EXT,nullptr,attrs);need(image!=EGL_NO_IMAGE_KHR,"YUYV image import");
  glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_EXTERNAL_OES,texture);gpu.target(GL_TEXTURE_EXTERNAL_OES,image);
  glTexParameteri(GL_TEXTURE_EXTERNAL_OES,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_EXTERNAL_OES,GL_TEXTURE_MAG_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_EXTERNAL_OES,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_EXTERNAL_OES,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);need(glGetError()==GL_NO_ERROR,"external texture");
 }

 void write(const std::vector<uint8_t>& src) {
  glFinish();if(texture){glDeleteTextures(1,&texture);texture=0;}if(image!=EGL_NO_IMAGE_KHR){gpu.destroy(gpu.display,image);image=EGL_NO_IMAGE_KHR;}
  dma_buf_sync s{};s.flags=DMA_BUF_SYNC_START|DMA_BUF_SYNC_WRITE;need(ioctl(fd,DMA_BUF_IOCTL_SYNC,&s)==0,"begin DMA CPU write");
  for(unsigned y=0;y<height;++y)std::memcpy(data+size_t(y)*pitch,src.data()+size_t(y)*width*2,width*2);
  unsigned eax=0,ebx=0,ecx=0,edx=0;need(__get_cpuid(1,&eax,&ebx,&ecx,&edx)&&(edx&(1U<<19))&&(edx&(1U<<26)),"CLFLUSH+MFENCE capability");
  const size_t line=((ebx>>8)&255)*8;need(line&&line<=256,"cache line size");
  for(size_t i=0;i<size;i+=line)__asm__ __volatile__("clflush (%0)"::"r"(data+i):"memory");
  __asm__ __volatile__("mfence":::"memory");
  s.flags=DMA_BUF_SYNC_END|DMA_BUF_SYNC_WRITE;need(ioctl(fd,DMA_BUF_IOCTL_SYNC,&s)==0,"end DMA CPU write");
  import();
 }
 ~Shared(){glFinish();if(texture)glDeleteTextures(1,&texture);if(image!=EGL_NO_IMAGE_KHR)gpu.destroy(gpu.display,image);if(data&&data!=MAP_FAILED)munmap(data,size);if(fd>=0)close(fd);}
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
static std::vector<uint8_t> pattern(unsigned w,unsigned h){std::vector<uint8_t> out(size_t(w)*h*2);for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;x+=2){size_t p=(size_t(y)*w+x)*2;out[p]=32+(x*173/w+y*31/h)%180;out[p+2]=32+((x+1)*173/w+y*31/h)%180;out[p+1]=80+(x/64)%5*16;out[p+3]=80+(y/64)%5*16;}return out;}
int main(int argc,char** argv){
 try{
  const char* node="/dev/dri/renderD128";const char* heap_path="/dev/dma_heap/system";unsigned frames=120;
  for(int i=1;i<argc;++i){need(i+1<argc,"argument value");std::string key=argv[i++];if(key=="--render-node")node=argv[i];else if(key=="--heap")heap_path=argv[i];else if(key=="--frames")frames=std::stoul(argv[i]);else throw std::runtime_error("unknown option");}
  need(frames>=4&&frames<=300,"bounded frame count4..300");need(temperature()<75000,"start temperature");setvbuf(stdout,nullptr,_IOLBF,0);
  GPU gpu(node);int heap=open(heap_path,O_RDWR|O_CLOEXEC);need(heap>=0,"DMA heap open");
  for(auto size:{std::pair<unsigned,unsigned>{640,360},{1280,720},{1920,1080}}){unsigned w=size.first,h=size.second;Picture pic(w,h);Shared shared(gpu,heap,w,h);auto source=pattern(w,h);std::vector<uint8_t> first(source.size()),second(source.size());
   for(const char* mode:{"staging-upload","shared-copy","direct-capture-lower-bound","direct-capture-lower-bound","shared-copy","staging-upload"}){
    bool staging=std::strcmp(mode,"staging-upload")==0,direct=std::strcmp(mode,"direct-capture-lower-bound")==0;
    shared.write(source);
    auto step=[&](unsigned i){if(!direct){source[0]=32+i%180;source[2]=33+i%180;}if(staging){std::memcpy(first.data(),source.data(),source.size());std::memcpy(second.data(),first.data(),first.size());glBindTexture(GL_TEXTURE_2D,pic.packed);glTexSubImage2D(GL_TEXTURE_2D,0,0,0,w/2,h,GL_RGBA,GL_UNSIGNED_BYTE,second.data());pic.draw(false,pic.packed);}else{if(!direct)shared.write(source);pic.draw(true,shared.texture);}};
    for(unsigned i=0;i<4;++i) { step(i); }
    pic.verify(source,mode);
    int hottest=0;double cpu=seconds(CLOCK_PROCESS_CPUTIME_ID),wall=seconds(CLOCK_MONOTONIC);
    for(unsigned i=0;i<frames;++i){if(i%8==0){hottest=std::max(hottest,temperature());need(hottest<85000,"run temperature");}step(i);}
    wall=seconds(CLOCK_MONOTONIC)-wall;cpu=seconds(CLOCK_PROCESS_CPUTIME_ID)-cpu;
    pic.verify(source,mode);std::printf("{\"type\":\"timing\",\"mode\":\"%s\",\"width\":%u,\"height\":%u,\"frames\":%u,\"cpu_s\":%.9f,\"wall_s\":%.9f,\"cpu_ms_per_frame\":%.6f,\"completed_draws_per_s\":%.6f,\"max_temp_mC\":%d}\n",mode,w,h,frames,cpu,wall,cpu*1000/frames,frames/wall,hottest);
   }
  }
  close(heap);return 0;
 }catch(const std::exception& e){std::fprintf(stderr,"FAIL %s EGL=%x GL=%x\n",e.what(),eglGetError(),glGetError());return 1;}
}
