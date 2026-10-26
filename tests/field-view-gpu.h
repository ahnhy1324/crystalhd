// SPDX-License-Identifier: GPL-2.0-or-later
// Shared field-view probe mechanics; never installed.
#ifndef CRYSTALHD_TEST_FIELD_VIEW_GPU_H
#define CRYSTALHD_TEST_FIELD_VIEW_GPU_H
#include "yuy2-gpu.h"
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <cpuid.h>
#include <time.h>
#include <array>
#include <filesystem>
#include <fstream>
#include <memory>

using yuy2_gpu::need;

[[maybe_unused]] static double clock_seconds(clockid_t clock) {
    timespec value{};
    need(clock_gettime(clock, &value) == 0, "clock");
    return value.tv_sec + value.tv_nsec / 1e9;
}

[[maybe_unused]] static int temperature() {
    int maximum = 0;
    for (const auto &entry : std::filesystem::directory_iterator("/sys/class/thermal")) {
        if (entry.path().filename().string().rfind("thermal_zone", 0) != 0) continue;
        int value = 0;
        std::ifstream(entry.path() / "temp") >> value;
        maximum = std::max(maximum, value);
    }
    return maximum;
}

struct Allocation {
    int fd = -1;
    uint8_t *data = nullptr;
    unsigned width, height, pitch;
    size_t length;
    bool initialized = false;
    Allocation(int heap, unsigned w, unsigned h)
        : width(w), height(h), pitch((2 * w + 63) & ~63U),
          length((size_t(pitch) * (h + 8) + 4095) & ~size_t(4095)) {
        need(w && !(w & 1) && h && !(h & 1), "even geometry");
        dma_heap_allocation_data request{};
        request.len = length;
        request.fd_flags = O_RDWR | O_CLOEXEC;
        need(ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &request) == 0, "DMA heap allocation");
        fd = request.fd;
        data = static_cast<uint8_t *>(mmap(nullptr, length, PROT_READ | PROT_WRITE,
                                           MAP_SHARED, fd, 0));
        if (data == MAP_FAILED) { data = nullptr; close(fd); fd = -1; need(false, "DMA mapping"); }
    }
    void write(const std::vector<uint8_t> &source) {
        need(source.size() == size_t(width) * height * 2, "source extent");
        write(source.data(),width*2,source.size());
    }
    void write(const uint8_t *source, size_t stride, size_t available) {
        need(source && stride >= size_t(width)*2 &&
             stride <= SIZE_MAX/height && available >= (height-1)*stride+width*2,
             "mapped source extent");
        // No imported image may survive this CPU producer update. The caller
        // retires both field uses and destroys the images first.
        glFinish();
        dma_buf_sync sync{};
        sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE;
        need(ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) == 0, "CPU producer begin");
        if (!initialized) memset(data, 0xa5, length);
        for (unsigned row = 0; row < height; ++row)
            memcpy(data + size_t(row) * pitch,
                   source + size_t(row) * stride, width * 2);
        unsigned a = 0, b = 0, c = 0, d = 0;
        need(__get_cpuid(1, &a, &b, &c, &d) && (d & (1U << 19)) &&
             (d & (1U << 26)), "owned CPU-producer CLFLUSH/MFENCE capability");
        const unsigned line = ((b >> 8) & 255) * 8;
        need(line && line <= 256, "cache line");
        for (size_t offset = 0; offset < length; offset += line)
            __asm__ __volatile__("clflush (%0)" :: "r"(data + offset) : "memory");
        __asm__ __volatile__("mfence" ::: "memory");
        sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
        need(ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) == 0, "CPU producer end");
        initialized = true;
    }
    Allocation(const Allocation &) = delete;
    ~Allocation() {
        if (data) munmap(data, length);
        if (fd >= 0) close(fd);
    }
};

struct Image {
    yuy2_gpu::GPU &gpu;
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    GLuint texture = 0;
    EGLint error = EGL_SUCCESS;
    template<class Source>
    Image(yuy2_gpu::GPU &g, Source &source, bool field, unsigned parity,
          EGLint matrix=EGL_ITU_REC709_EXT, bool trace=true)
        : gpu(g) {
        const unsigned height = field ? source.height / 2 : source.height;
        const unsigned pitch = source.pitch * (field ? 2 : 1);
        const unsigned offset = field ? parity * source.pitch : 0;
        need(parity < 2 && size_t(offset) + size_t(height - 1) * pitch +
             source.width * 2 <= source.length, "import extent");
        EGLint attributes[] = {
            EGL_WIDTH, EGLint(source.width), EGL_HEIGHT, EGLint(height),
            EGL_LINUX_DRM_FOURCC_EXT, EGLint(DRM_FORMAT_YUYV),
            EGL_DMA_BUF_PLANE0_FD_EXT, source.fd,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGLint(offset),
            EGL_DMA_BUF_PLANE0_PITCH_EXT, EGLint(pitch),
            EGL_YUV_COLOR_SPACE_HINT_EXT, matrix,
            EGL_SAMPLE_RANGE_HINT_EXT, EGL_YUV_NARROW_RANGE_EXT,
            EGL_YUV_CHROMA_HORIZONTAL_SITING_HINT_EXT, EGL_YUV_CHROMA_SITING_0_5_EXT,
            EGL_YUV_CHROMA_VERTICAL_SITING_HINT_EXT, EGL_YUV_CHROMA_SITING_0_5_EXT,
            EGL_NONE
        };
        image = gpu.create(gpu.display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
                           nullptr, attributes);
        error = eglGetError();
        if (trace) printf("{\"type\":\"import\",\"field\":%s,\"parity\":%u,\"width\":%u,"
               "\"height\":%u,\"offset\":%u,\"pitch\":%u,\"ok\":%s,\"egl_error\":%u}\n",
               field ? "true" : "false", parity, source.width, height, offset,
               pitch, image != EGL_NO_IMAGE_KHR ? "true" : "false", unsigned(error));
        if (image == EGL_NO_IMAGE_KHR) return;
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture);
        gpu.target(GL_TEXTURE_EXTERNAL_OES, image);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        need(glGetError() == GL_NO_ERROR, "external field texture");
    }
    Image(const Image &) = delete;
    ~Image() {
        glFinish();
        if (texture) glDeleteTextures(1, &texture);
        if (image != EGL_NO_IMAGE_KHR) gpu.destroy(gpu.display, image);
    }
};

// Native producer alternative: GBM owns the map/unmap coherency boundary.
// Storage is a linear array of four-byte pairs; EGL interprets those bytes
// as YUYV. No CPU color conversion is performed. map/unmap may internally
// stage/copy, so its entire cost belongs to the producer measurement.
struct GbmAllocation {
    gbm_bo *bo=nullptr;
    int fd=-1;
    unsigned width,height,pitch;
    size_t length=0;
    bool initialized=false;
    GbmAllocation(yuy2_gpu::GPU &gpu,unsigned w,unsigned h):width(w),height(h),pitch(0) {
      try {
        need(w&&!(w&1)&&h&&!(h&1),"GBM even geometry");
        bo=gbm_bo_create(gpu.gbm,w/2,h+8,GBM_FORMAT_ARGB8888,
                         GBM_BO_USE_LINEAR);
        need(bo,"owned GBM linear pair storage");
        need(gbm_bo_get_modifier(bo)==DRM_FORMAT_MOD_LINEAR,"actual linear GBM modifier");
        pitch=gbm_bo_get_stride(bo);fd=gbm_bo_get_fd(bo);
        need(fd>=0&&pitch>=w*2,"GBM export/pitch");
        const off_t bytes=lseek(fd,0,SEEK_END);
        need(bytes>0&&uint64_t(bytes)>=uint64_t(pitch)*(h+8),"GBM exported extent");
        length=bytes;need(lseek(fd,0,SEEK_SET)==0,"DMA-BUF size query restore");
        printf("{\"type\":\"allocation\",\"kind\":\"gbm-linear-map\",\"view\":\"YUYV\",\"width\":%u,\"height\":%u,\"pitch\":%u,\"bytes\":%zu,\"modifier\":0}\n",w,h,pitch,length);
      } catch(...) {
        if(fd>=0){close(fd);fd=-1;}
        if(bo){gbm_bo_destroy(bo);bo=nullptr;}
        throw;
      }
    }
    void write(const uint8_t *source,size_t stride,size_t available) {
        need(source&&stride>=width*2&&stride<=SIZE_MAX/height&&
             available>=(height-1)*stride+width*2,"GBM mapped source extent");
        // Caller has completed both uses and destroyed the old field images.
        glFinish();uint32_t mapped_pitch=0;void *handle=nullptr;
        auto *data=static_cast<uint8_t *>(gbm_bo_map(bo,0,0,width/2,height+8,
                                      GBM_BO_TRANSFER_WRITE,&mapped_pitch,&handle));
        if(data&&mapped_pitch<width*2){gbm_bo_unmap(bo,handle);need(false,"GBM transfer pitch");}
        need(data,"GBM writable transfer");
        if(!initialized)for(unsigned row=0;row<height+8;++row)
            memset(data+size_t(row)*mapped_pitch,0xa5,width*2);
        for(unsigned row=0;row<height;++row)
            memcpy(data+size_t(row)*mapped_pitch,source+row*stride,width*2);
        gbm_bo_unmap(bo,handle);initialized=true;
    }
    GbmAllocation(const GbmAllocation &)=delete;
    ~GbmAllocation(){if(fd>=0)close(fd);if(bo)gbm_bo_destroy(bo);}
};

// Stay inside the mathematically selected nearest texel even when an
// interpolated coordinate lies on a tie. Compute the available interval at
// this exact geometry; a constant phase is not safe across scale ratios.
static float nearest_bias(unsigned source, unsigned destination, int phase) {
    const int64_t denominator = int64_t(phase == 2 ? 2 : 4) * destination;
    int64_t gap = denominator;
    for (unsigned pixel = 0; pixel < destination; ++pixel) {
        const int64_t numerator = int64_t(2 * pixel + 1) * source +
            (phase == 2 ? 0 : int64_t(1 - 2 * phase) * destination);
        if (numerator < 0) continue; // Clamped leading field line.
        gap = std::min(gap, denominator - numerator % denominator);
    }
    const double texels = double(gap) / (2 * denominator);
    return phase == 2 ? texels / source : (0.5 - phase + 2 * texels) / source;
}

struct Renderer {
    yuy2_gpu::Picture picture;
    const unsigned source_w, source_h;
    const float x_shift;
    const std::array<float,2> y_shifts;
    unsigned viewport_w=0,viewport_h=0;
    float viewport_x_shift=0;
    float viewport_progressive_y_shift=0;
    std::array<float,2> viewport_y_shifts{};
    GLuint program, affine_program;
    static GLuint link(const char *vertex, const char *fragment) {
        GLuint v = yuy2_gpu::shader(GL_VERTEX_SHADER, vertex);
        GLuint f = yuy2_gpu::shader(GL_FRAGMENT_SHADER, fragment);
        GLuint result = glCreateProgram();
        glAttachShader(result, v); glAttachShader(result, f);
        glBindAttribLocation(result, 0, "pos"); glLinkProgram(result);
        GLint linked = 0; glGetProgramiv(result, GL_LINK_STATUS, &linked);
        glDeleteShader(v); glDeleteShader(f);
        need(linked, "field shader link");
        return result;
    }
    Renderer(unsigned w, unsigned h, unsigned output_w, unsigned output_h)
        : picture(output_w, output_h), source_w(w), source_h(h),
          x_shift(nearest_bias(w,output_w,2)),
          y_shifts{nearest_bias(h,output_h,0),nearest_bias(h,output_h,1)} {
        const char *vertex = "attribute vec2 pos;void main(){gl_Position=vec4(pos,0.0,1.0);}";
        // Original field lines remain on their original vertical positions.
        // At an exactly equidistant missing line, choose the following line,
        // clamping at the bottom. Sample texel centres to avoid nearest-filter
        // ties and accidental opposite-field sampling. This is nearest Bob,
        // not the production VA/GStreamer interpolation algorithm.
        const char *fragment =
            "#extension GL_OES_EGL_image_external : require\n"
            "precision highp float;uniform samplerExternalOES tex;"
            "uniform vec2 size;uniform vec2 outputSize;uniform float parity;uniform float xshift;uniform float rowShift;"
            "void main(){float row=clamp(floor(gl_FragCoord.y*(size.y/(2.0*outputSize.y))+rowShift),"
            "0.0,size.y*0.5-1.0);"
            "vec2 uv=vec2(gl_FragCoord.x/outputSize.x+xshift,(2.0*row+parity+0.5)/size.y);"
            "gl_FragColor=vec4(texture2D(tex,uv).rgb,1.0);}";
        program = link(vertex, fragment);
        const char *affine_vertex =
            "attribute vec2 pos;uniform vec2 shift;uniform float flip;varying vec2 uv;"
            "void main(){uv=(pos+1.0)*0.5+shift;gl_Position=vec4(pos.x,pos.y*(1.0-2.0*flip),0.0,1.0);}";
        const char *affine_fragment =
            "#extension GL_OES_EGL_image_external : require\n"
            "precision highp float;varying vec2 uv;uniform samplerExternalOES tex;"
            "void main(){gl_FragColor=vec4(texture2D(tex,uv).rgb,1.0);}";
        affine_program = link(affine_vertex, affine_fragment);
    }
    void draw(const Image &image, bool field_view, unsigned parity) {
        draw_to(image,field_view,parity,picture.fbo,0,0,picture.w,picture.h,false,false);
    }
    void draw_to(const Image &image,bool field_view,unsigned parity,GLuint target,
                 int x,int y,unsigned w,unsigned h,bool flip,bool progressive) {
        need(image.texture && parity < 2, "field draw admission");
        glBindFramebuffer(GL_FRAMEBUFFER,target);
        const GLuint selected = field_view||progressive ? affine_program : program;
        glViewport(x,y,w,h); glUseProgram(selected);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_EXTERNAL_OES, image.texture);
        glUniform1i(glGetUniformLocation(selected, "tex"), 0);
        if (field_view||progressive) {
            // Nearest only. Linear filtering requires its own phase oracle.
            if (w!=viewport_w||h!=viewport_h) {
                viewport_x_shift=w==picture.w?x_shift:nearest_bias(source_w,w,2);
                for (unsigned p=0;p<2;++p)
                    viewport_y_shifts[p]=h==picture.h?y_shifts[p]:nearest_bias(source_h,h,p);
                viewport_progressive_y_shift=nearest_bias(source_h,h,2);
                viewport_w=w;viewport_h=h;
            }
            glUniform2f(glGetUniformLocation(selected, "shift"),
                        viewport_x_shift,
                        progressive?viewport_progressive_y_shift:viewport_y_shifts[parity]);
            glUniform1f(glGetUniformLocation(selected,"flip"),flip?1:0);
        } else {
            glUniform2f(glGetUniformLocation(selected, "size"), source_w, source_h);
            glUniform2f(glGetUniformLocation(selected, "outputSize"), picture.w, picture.h);
            glUniform1f(glGetUniformLocation(selected, "xshift"),x_shift);
            glUniform1f(glGetUniformLocation(selected, "rowShift"),y_shifts[parity]*source_h*0.5f);
            glUniform1f(glGetUniformLocation(selected, "parity"), parity);
        }
        static const float positions[] = {-1,-1, 1,-1, -1,1, 1,1};
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, positions);
        glEnableVertexAttribArray(0); glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glFinish();
        need(glGetError() == GL_NO_ERROR, "field GPU completion");
    }
    ~Renderer() { glDeleteProgram(program); glDeleteProgram(affine_program); }
};

[[maybe_unused]] static void verify_scaled(Renderer &renderer, const std::vector<uint8_t> &source,
                          unsigned parity, const char *path) {
    const unsigned w=renderer.picture.w, h=renderer.picture.h;
    const unsigned sw=renderer.source_w, sh=renderer.source_h;
    std::vector<uint8_t> pixels(size_t(w)*h*4);
    glReadPixels(0,0,w,h,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
    need(glGetError()==GL_NO_ERROR,"scaled RGB readback");
    unsigned maximum=0;size_t over=0,alpha=0;uint64_t sum=0;
    for(unsigned y=0;y<h;++y) for(unsigned x=0;x<w;++x) {
        // Independent integer physical-nearest model; never repack output
        // into YUYV: adjacent destination pixels can use different UV pairs.
        int64_t numerator=int64_t(2*y+1)*sh+int64_t(1-2*int(parity))*h;
        const unsigned fy=unsigned(std::clamp<int64_t>(numerator<0?-1:numerator/(4*h),0,sh/2-1));
        const unsigned sx=std::min<unsigned>(uint64_t(2*x+1)*sw/(2*w),sw-1);
        const size_t q=(size_t(2*fy+parity)*sw+(sx/2)*2)*2;
        const double yy=(source[q+(sx&1)*2]-16)*255.0/219;
        const double u=(source[q+1]-128)*255.0/224,v=(source[q+3]-128)*255.0/224;
        const double expected[]={yy+1.5748*v,yy-0.187324*u-0.468124*v,yy+1.8556*u};
        for(unsigned c=0;c<3;++c) {
            const int value=std::clamp(int(std::lround(expected[c])),0,255);
            const unsigned error=std::abs(int(pixels[(size_t(y)*w+x)*4+c])-value);
            if(error>2&&over<8)fprintf(stderr,"scaled xy=%u,%u source=%u,%u channel=%u got=%u expected=%d\n",x,y,sx,2*fy+parity,c,pixels[(size_t(y)*w+x)*4+c],value);
            maximum=std::max(maximum,error);sum+=error;over+=error>2;
        }
        alpha+=pixels[(size_t(y)*w+x)*4+3]!=255;
    }
    printf("{\"type\":\"pixels\",\"path\":\"%s\",\"source_width\":%u,\"source_height\":%u,\"width\":%u,\"height\":%u,\"max_error\":%u,\"mean_error\":%.9f,\"channels_over2\":%zu,\"nonopaque\":%zu}\n",
           path,sw,sh,w,h,maximum,double(sum)/(size_t(w)*h*3),over,alpha);
    need(!over&&!alpha,"scaled pixel oracle fixed maximum2");
}

inline void cpu_bob(const uint8_t *source, size_t stride, unsigned w, unsigned h,
                    unsigned parity, std::vector<uint8_t> &result) {
    result.resize(size_t(w)*h*2);
    for (unsigned y = 0; y < h; ++y) {
        unsigned input = y;
        if ((input & 1) != parity) ++input;
        if (input >= h) input = h - 2 + parity;
        memcpy(result.data() + size_t(y) * w * 2,
               source + size_t(input) * stride, w * 2);
    }
}

inline void cpu_bob(const std::vector<uint8_t> &source, unsigned w, unsigned h,
                    unsigned parity, std::vector<uint8_t> &result) {
    need(source.size()==size_t(w)*h*2,"CPU Bob source extent");
    cpu_bob(source.data(),w*2,w,h,parity,result);
}

#endif
