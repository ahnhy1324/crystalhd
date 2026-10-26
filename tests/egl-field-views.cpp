// SPDX-License-Identifier: GPL-2.0-or-later
// Device-free display experiment for #530. No CrystalHD node is opened.
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

static double clock_seconds(clockid_t clock) {
    timespec value{};
    need(clock_gettime(clock, &value) == 0, "clock");
    return value.tv_sec + value.tv_nsec / 1e9;
}

static int temperature() {
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
        // No imported image may survive this CPU producer update. The caller
        // retires both field uses and destroys the images first.
        glFinish();
        dma_buf_sync sync{};
        sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE;
        need(ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) == 0, "CPU producer begin");
        memset(data, 0xa5, length);
        for (unsigned row = 0; row < height; ++row)
            memcpy(data + size_t(row) * pitch,
                   source.data() + size_t(row) * width * 2, width * 2);
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
    Image(yuy2_gpu::GPU &g, Allocation &source, bool field, unsigned parity)
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
            EGL_YUV_COLOR_SPACE_HINT_EXT, EGL_ITU_REC709_EXT,
            EGL_SAMPLE_RANGE_HINT_EXT, EGL_YUV_NARROW_RANGE_EXT,
            EGL_YUV_CHROMA_HORIZONTAL_SITING_HINT_EXT, EGL_YUV_CHROMA_SITING_0_5_EXT,
            EGL_YUV_CHROMA_VERTICAL_SITING_HINT_EXT, EGL_YUV_CHROMA_SITING_0_5_EXT,
            EGL_NONE
        };
        image = gpu.create(gpu.display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
                           nullptr, attributes);
        error = eglGetError();
        printf("{\"type\":\"import\",\"field\":%s,\"parity\":%u,\"width\":%u,"
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

struct Renderer {
    yuy2_gpu::Picture picture;
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
    Renderer(unsigned w, unsigned h) : picture(w, h) {
        const char *vertex = "attribute vec2 pos;void main(){gl_Position=vec4(pos,0.0,1.0);}";
        // Original field lines remain on their original vertical positions.
        // At an exactly equidistant missing line, choose the following line,
        // clamping at the bottom. Sample texel centres to avoid nearest-filter
        // ties and accidental opposite-field sampling. This is nearest Bob,
        // not the production VA/GStreamer interpolation algorithm.
        const char *fragment =
            "#extension GL_OES_EGL_image_external : require\n"
            "precision highp float;uniform samplerExternalOES tex;"
            "uniform vec2 size;uniform float parity;"
            "void main(){float row=clamp(floor((floor(gl_FragCoord.y)-parity+1.0)*0.5),"
            "0.0,size.y*0.5-1.0);"
            "vec2 uv=vec2(gl_FragCoord.x/size.x,(2.0*row+parity+0.5)/size.y);"
            "gl_FragColor=vec4(texture2D(tex,uv).rgb,1.0);}";
        program = link(vertex, fragment);
        const char *affine_vertex =
            "attribute vec2 pos;uniform float shift;varying vec2 uv;"
            "void main(){uv=(pos+1.0)*0.5;uv.y+=shift;gl_Position=vec4(pos,0.0,1.0);}";
        const char *affine_fragment =
            "#extension GL_OES_EGL_image_external : require\n"
            "precision highp float;varying vec2 uv;uniform samplerExternalOES tex;"
            "void main(){gl_FragColor=vec4(texture2D(tex,uv).rgb,1.0);}";
        affine_program = link(affine_vertex, affine_fragment);
    }
    void draw(const Image &image, bool field_view, unsigned parity) {
        need(image.texture && parity < 2, "field draw admission");
        glBindFramebuffer(GL_FRAMEBUFFER, picture.fbo);
        const GLuint selected = field_view ? affine_program : program;
        glViewport(0, 0, picture.w, picture.h); glUseProgram(selected);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_EXTERNAL_OES, image.texture);
        glUniform1i(glGetUniformLocation(selected, "tex"), 0);
        if (field_view) {
            // Nearest-only: the quarter output-pixel bias resolves exact
            // missing-line ties toward the following field row, away from
            // interpolation-rounding boundaries. It does not change the CPU
            // reference row selection. Linear filtering needs another phase.
            glUniform1f(glGetUniformLocation(selected, "shift"),
                         (0.75f - parity) / picture.h);
        } else {
            glUniform2f(glGetUniformLocation(selected, "size"), picture.w, picture.h);
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

static std::vector<uint8_t> pattern(unsigned w, unsigned h, unsigned generation) {
    std::vector<uint8_t> result(size_t(w) * h * 2);
    for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w; x += 2) {
        const size_t offset = (size_t(y) * w + x) * 2;
        result[offset] = 32 + (x * 7 + y * 11 + generation * 37) % 180;
        result[offset + 2] = 32 + ((x + 1) * 7 + y * 11 + generation * 37) % 180;
        result[offset + 1] = (y & 1 ? 160 : 72) + ((x / 16 + generation) % 3) * 8;
        result[offset + 3] = (y & 1 ? 72 : 160) + ((y / 16 + generation) % 3) * 8;
    }
    return result;
}

static void cpu_bob(const std::vector<uint8_t> &source, unsigned w, unsigned h,
                    unsigned parity, std::vector<uint8_t> &result) {
    result.resize(source.size());
    for (unsigned y = 0; y < h; ++y) {
        unsigned input = y;
        if ((input & 1) != parity) ++input;
        if (input >= h) input = h - 2 + parity;
        memcpy(result.data() + size_t(y) * w * 2,
               source.data() + size_t(input) * w * 2, w * 2);
    }
}

int main(int argc, char **argv) {
    try {
        unsigned fields = 0;
        if (argc == 3 && std::string(argv[1]) == "--fields") fields = std::stoul(argv[2]);
        else need(argc == 1, "usage: egl-field-views [--fields 0..180]");
        need(fields <= 180 && !(fields & 1), "bounded even field count");
        need(temperature() < 75000, "start temperature below75C");
        setvbuf(stdout, nullptr, _IOLBF, 0);
        yuy2_gpu::GPU gpu("/dev/dri/renderD128");
        const int heap = open("/dev/dma_heap/system", O_RDWR | O_CLOEXEC);
        need(heap >= 0, "DMA heap open");
        for (const auto dimensions : {std::pair<unsigned,unsigned>{640,360}, {1280,720}, {1920,1080}}) {
            const unsigned w = dimensions.first, h = dimensions.second;
            Renderer renderer(w, h);
            // Two retained, differently patterned source allocations alternate
            // per picture. They model immutable ready pictures; no decoder,
            // source-generation cost or physical presentation is benchmarked.
            std::array<std::unique_ptr<Allocation>,2> memory;
            std::array<std::vector<uint8_t>,2> sources;
            std::array<std::unique_ptr<Image>,2> full;
            std::array<std::array<std::unique_ptr<Image>,2>,2> views;
            bool import_ok = true;
            std::vector<uint8_t> bob;
            for (unsigned generation = 0; generation < 2; ++generation) {
                memory[generation] = std::make_unique<Allocation>(heap, w, h);
                sources[generation] = pattern(w, h, generation);
                memory[generation]->write(sources[generation]);
                full[generation] = std::make_unique<Image>(gpu, *memory[generation], false, 0);
                need(full[generation]->texture, "full-image control import");
                for (unsigned parity = 0; parity < 2; ++parity) {
                    views[generation][parity] = std::make_unique<Image>(gpu, *memory[generation], true, parity);
                    import_ok &= views[generation][parity]->texture != 0;
                    cpu_bob(sources[generation], w, h, parity, bob);
                    renderer.draw(*full[generation], false, parity);
                    renderer.picture.verify(bob, "full-image-parity-shader");
                    if (views[generation][parity]->texture) {
                        renderer.draw(*views[generation][parity], true, parity);
                        renderer.picture.verify(bob, "double-pitch-field-view");
                    }
                }
            }
            printf("{\"type\":\"geometry\",\"width\":%u,\"height\":%u,\"field_views\":%s}\n",
                   w, h, import_ok ? "true" : "false");
            for (unsigned rate : {50U,60U}) for (bool tff : {true,false}) {
                if (!fields) continue;
                for (const char *mode : {"cpu-bob-upload", "field-view", "full-image-parity",
                                         "full-image-parity", "field-view", "cpu-bob-upload"}) {
                    const bool cpu = std::string(mode) == "cpu-bob-upload";
                    const bool field = std::string(mode) == "field-view";
                    if (field && !import_ok) continue;
                    unsigned submitted = 0, completed = 0, late = 0;
                    int hottest = 0;
                    double materialize_cpu = 0, upload_cpu = 0, draw_wait_wall = 0;
                    double max_age = 0, cpu_start = clock_seconds(CLOCK_PROCESS_CPUTIME_ID);
                    const double start = clock_seconds(CLOCK_MONOTONIC);
                    for (unsigned i = 0; i < fields; ++i) {
                        const unsigned generation = (i / 2) & 1;
                        const unsigned parity = (i & 1) ^ (tff ? 0 : 1);
                        const double due = start + double(i) / rate;
                        timespec deadline{time_t(due),long((due - time_t(due))*1e9)};
                        int error;
                        do { error = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, nullptr); }
                        while (error == EINTR);
                        need(error == 0, "field deadline wait");
                        if ((i & 15) == 0) {
                            hottest = std::max(hottest, temperature());
                            need(hottest < 85000, "temperature below85C");
                        }
                        if (cpu) {
                            double phase = clock_seconds(CLOCK_PROCESS_CPUTIME_ID);
                            cpu_bob(sources[generation], w, h, parity, bob);
                            materialize_cpu += clock_seconds(CLOCK_PROCESS_CPUTIME_ID) - phase;
                            phase = clock_seconds(CLOCK_PROCESS_CPUTIME_ID);
                            glBindTexture(GL_TEXTURE_2D, renderer.picture.packed);
                            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w/2, h,
                                            GL_RGBA, GL_UNSIGNED_BYTE, bob.data());
                            upload_cpu += clock_seconds(CLOCK_PROCESS_CPUTIME_ID) - phase;
                            ++submitted;
                            phase = clock_seconds(CLOCK_MONOTONIC);
                            renderer.picture.draw(false, renderer.picture.packed);
                            draw_wait_wall += clock_seconds(CLOCK_MONOTONIC) - phase;
                        } else {
                            ++submitted;
                            const double phase = clock_seconds(CLOCK_MONOTONIC);
                            renderer.draw(field ? *views[generation][parity] : *full[generation],
                                          field, parity);
                            draw_wait_wall += clock_seconds(CLOCK_MONOTONIC) - phase;
                        }
                        ++completed;
                        const double age = clock_seconds(CLOCK_MONOTONIC) - due;
                        max_age = std::max(max_age, age);
                        late += age > 1.0/rate;
                    }
                    const double wall = clock_seconds(CLOCK_MONOTONIC) - start;
                    const double cpu_time = clock_seconds(CLOCK_PROCESS_CPUTIME_ID) - cpu_start;
                    const unsigned last = fields - 1;
                    cpu_bob(sources[(last/2)&1], w, h, (last&1)^(tff?0:1), bob);
                    renderer.picture.verify(bob, mode);
                    printf("{\"type\":\"clocked\",\"mode\":\"%s\",\"width\":%u,\"height\":%u,"
                           "\"rate\":%u,\"tff\":%s,\"submitted\":%u,\"completed\":%u,\"late\":%u,"
                           "\"cpu_s\":%.9f,\"wall_s\":%.9f,\"max_deadline_age_ms\":%.6f,"
                           "\"cpu_bob_bytes\":%zu,\"materialize_cpu_s\":%.9f,\"upload_cpu_s\":%.9f,"
                           "\"draw_wait_wall_s\":%.9f,\"max_temp_mC\":%d,\"physical_presentation\":false}\n",
                           mode,w,h,rate,tff?"true":"false",submitted,completed,late,cpu_time,wall,
                           max_age*1000,cpu?size_t(fields)*w*h*2:0,materialize_cpu,upload_cpu,
                           draw_wait_wall,hottest);
                }
            }
        }
        close(heap);
        return 0;
    } catch (const std::exception &error) {
        fprintf(stderr,"FAIL %s EGL=%x GL=%x\n",error.what(),eglGetError(),glGetError());
        return 1;
    }
}
