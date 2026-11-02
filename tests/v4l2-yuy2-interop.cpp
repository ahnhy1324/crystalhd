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
#include "yuy2-gpu.h"
using yuy2_gpu::GPU;
using yuy2_gpu::Picture;

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
