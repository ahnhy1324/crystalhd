// SPDX-License-Identifier: GPL-2.0-or-later
// Device-free display experiment for #530. No CrystalHD node is opened.
#include "field-view-gpu.h"

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

int main(int argc, char **argv) {
    try {
        unsigned fields = 0;
        bool scaled_only=false;
        for(int i=1;i<argc;++i) {
            if(std::string(argv[i])=="--fields"&&i+1<argc)fields=std::stoul(argv[++i]);
            else if(std::string(argv[i])=="--scaled-only")scaled_only=true;
            else need(false,"usage: egl-field-views [--fields 0..180] [--scaled-only]");
        }
        need(fields <= 180 && !(fields & 1), "bounded even field count");
        need(temperature() < 75000, "start temperature below75C");
        setvbuf(stdout, nullptr, _IOLBF, 0);
        yuy2_gpu::GPU gpu("/dev/dri/renderD128");
        const int heap = open("/dev/dma_heap/system", O_RDWR | O_CLOEXEC);
        need(heap >= 0, "DMA heap open");
        for (const auto dimensions : {std::array<unsigned,4>{640,360,640,360},
                                      {1280,720,1280,720}, {1920,1080,1920,1080},
                                      {1280,720,1920,1080}}) {
            const unsigned w=dimensions[0],h=dimensions[1],ow=dimensions[2],oh=dimensions[3];
            const bool scaled=w!=ow||h!=oh;
            if(scaled_only&&!scaled)continue;
            Renderer renderer(w,h,ow,oh);
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
                    verify_scaled(renderer,sources[generation],parity,"full-image-parity-shader");
                    if (views[generation][parity]->texture) {
                        renderer.draw(*views[generation][parity], true, parity);
                        verify_scaled(renderer,sources[generation],parity,"double-pitch-field-view");
                    }
                }
            }
            printf("{\"type\":\"geometry\",\"width\":%u,\"height\":%u,\"output_width\":%u,\"output_height\":%u,\"field_views\":%s}\n",
                   w,h,ow,oh,import_ok?"true":"false");
            for (unsigned rate : {50U,60U}) for (bool tff : {true,false}) {
                if (!fields) continue;
                for (const char *mode : {"cpu-bob-upload", "field-view", "full-image-parity",
                                         "full-image-parity", "field-view", "cpu-bob-upload"}) {
                    const bool cpu = std::string(mode) == "cpu-bob-upload";
                    const bool field = std::string(mode) == "field-view";
                    if(cpu&&scaled)continue; // No unmatched CPU scaler comparison.
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
                    verify_scaled(renderer,sources[(last/2)&1],(last&1)^(tff?0:1),mode);
                    printf("{\"type\":\"clocked\",\"mode\":\"%s\",\"width\":%u,\"height\":%u,"
                           "\"output_width\":%u,\"output_height\":%u,"
                           "\"rate\":%u,\"tff\":%s,\"submitted\":%u,\"completed\":%u,\"late\":%u,"
                           "\"cpu_s\":%.9f,\"wall_s\":%.9f,\"max_deadline_age_ms\":%.6f,"
                           "\"cpu_bob_bytes\":%zu,\"materialize_cpu_s\":%.9f,\"upload_cpu_s\":%.9f,"
                           "\"draw_wait_wall_s\":%.9f,\"max_temp_mC\":%d,\"physical_presentation\":false}\n",
                           mode,w,h,ow,oh,rate,tff?"true":"false",submitted,completed,late,cpu_time,wall,
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
