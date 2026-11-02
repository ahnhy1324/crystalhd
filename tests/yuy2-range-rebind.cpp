// SPDX-License-Identifier: GPL-2.0-or-later
// Decoder-free EGL sample-range rebind study; no CrystalHD node is opened.
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
static int temperature() {
 int maximum=0;
 for(const auto& e:std::filesystem::directory_iterator("/sys/class/thermal")) {
  if(e.path().filename().string().rfind("thermal_zone",0)!=0) continue;
  int value=0;std::ifstream(e.path()/"temp")>>value;maximum=std::max(maximum,value);
 }
 return maximum;
}
#include "yuy2-gpu.h"
using yuy2_gpu::need;
using yuy2_gpu::GPU;
using yuy2_gpu::Picture;


#include <memory>
struct Memory {
 int fd=-1;size_t size=16384;uint8_t* data=nullptr;
 Memory(int heap,const std::vector<uint8_t>& src) {
  dma_heap_allocation_data a{};a.len=size;a.fd_flags=O_RDWR|O_CLOEXEC;need(ioctl(heap,DMA_HEAP_IOCTL_ALLOC,&a)==0,"alloc");fd=a.fd;
  data=(uint8_t*)mmap(nullptr,size,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);need(data!=MAP_FAILED,"mmap");
  dma_buf_sync sync{};sync.flags=DMA_BUF_SYNC_START|DMA_BUF_SYNC_WRITE;need(ioctl(fd,DMA_BUF_IOCTL_SYNC,&sync)==0,"CPU begin");memcpy(data,src.data(),src.size());
  for(size_t n=0;n<size;n+=64){__asm__ __volatile__("clflush (%0)"::"r"(data+n):"memory");}
  __asm__ __volatile__("mfence":::"memory");
  sync.flags=DMA_BUF_SYNC_END|DMA_BUF_SYNC_WRITE;need(ioctl(fd,DMA_BUF_IOCTL_SYNC,&sync)==0,"CPU end");
 }
 ~Memory(){if(data&&data!=MAP_FAILED)munmap(data,size);if(fd>=0)close(fd);}
};
int main(){try{
 need(temperature()<75000,"thermal");setvbuf(stdout,nullptr,_IOLBF,0);GPU gpu("/dev/dri/renderD128");int heap=open("/dev/dma_heap/system",O_RDWR|O_CLOEXEC);need(heap>=0,"heap");
 std::vector<uint8_t> source(128*64*2);const uint8_t values[]={0,8,16,64,128,219,235,255};for(unsigned y=0;y<64;y++)for(unsigned x=0;x<128;x+=2){size_t p=(y*128+x)*2;source[p]=source[p+2]=values[x/16];source[p+1]=source[p+3]=128;}
 unsigned failures=0;
 for(unsigned same_fd=0;same_fd<2;same_fd++)for(unsigned same_texture=0;same_texture<2;same_texture++)for(unsigned same_picture=0;same_picture<2;same_picture++){
  std::unique_ptr<Memory> memory;std::unique_ptr<Picture> picture;GLuint texture=0;
  for(unsigned phase=0;phase<3;phase++){
   const bool full=phase==1;if(!same_fd||!memory)memory=std::make_unique<Memory>(heap,source);if(!same_picture||!picture)picture=std::make_unique<Picture>(128,64,false,full);picture->full_range=full;
   if(!same_texture&&texture){glDeleteTextures(1,&texture);texture=0;}if(!texture)glGenTextures(1,&texture);
   EGLint a[]={EGL_WIDTH,128,EGL_HEIGHT,64,EGL_LINUX_DRM_FOURCC_EXT,(EGLint)DRM_FORMAT_YUYV,EGL_DMA_BUF_PLANE0_FD_EXT,memory->fd,EGL_DMA_BUF_PLANE0_OFFSET_EXT,0,EGL_DMA_BUF_PLANE0_PITCH_EXT,256,EGL_YUV_COLOR_SPACE_HINT_EXT,EGL_ITU_REC709_EXT,EGL_SAMPLE_RANGE_HINT_EXT,full?EGL_YUV_FULL_RANGE_EXT:EGL_YUV_NARROW_RANGE_EXT,EGL_YUV_CHROMA_HORIZONTAL_SITING_HINT_EXT,EGL_YUV_CHROMA_SITING_0_EXT,EGL_YUV_CHROMA_VERTICAL_SITING_HINT_EXT,EGL_YUV_CHROMA_SITING_0_EXT,EGL_NONE};
   EGLImageKHR image=gpu.create(gpu.display,EGL_NO_CONTEXT,EGL_LINUX_DMA_BUF_EXT,nullptr,a);need(image!=EGL_NO_IMAGE_KHR,"import");glBindTexture(GL_TEXTURE_EXTERNAL_OES,texture);gpu.target(GL_TEXTURE_EXTERNAL_OES,image);glTexParameteri(GL_TEXTURE_EXTERNAL_OES,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_EXTERNAL_OES,GL_TEXTURE_MAG_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_EXTERNAL_OES,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_EXTERNAL_OES,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
   char label[160];snprintf(label,sizeof(label),"fd%u-texture%u-picture%u-phase%u",same_fd,same_texture,same_picture,phase);picture->draw(true,texture);try{picture->verify(source,label);}catch(const std::exception& error){if(std::string(error.what()).find("pixel oracle fixed maximum2")!=0)throw;failures++;}glFinish();need(gpu.destroy(gpu.display,image)==EGL_TRUE,"destroy");
  }
  glDeleteTextures(1,&texture);picture.reset();memory.reset();need(glGetError()==GL_NO_ERROR,"cleanup");
 }
 close(heap);printf("COMPLETE neutral EGL range lifetime controls:24 pictures failed=%u decoder=NOT_OPENED\n",failures);return failures ? 1 : 0;
}catch(const std::exception& e){fprintf(stderr,"FAIL %s\n",e.what());return 1;}}
