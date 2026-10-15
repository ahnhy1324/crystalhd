// SPDX-License-Identifier: LGPL-2.1-or-later
// Optional BCM70015 research client: drain raw output without VA's field pairing.
// Build only; no installed library, firmware, module or register changes:
// c++ -std=c++11 -O2 -Wall -Wextra -Werror -D__LINUX_USER__
//   -Iinclude -Ilinux_lib/libcrystalhd tests/fw-single-field-decode.cpp
//   $(pkg-config --cflags --libs libavformat libavcodec libavutil glib-2.0)
//   -Llinux_lib/libcrystalhd -lcrystalhd -o /absolute/new/probe
// Run with an external timeout as well; a kernel ioctl/close can outlast our
// deadline. OUTPUT_PREFIX must be absolute and both output files must be new.
#include <bc_dts_types.h>
#include <libcrystalhd_if.h>
#include "../filters/gst/gst-plugin-1.0/gstcrystalhd-input.h"
extern "C" {
#include <libavformat/avformat.h>
}
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <limits>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

extern BC_STATUS DtsFWSetSingleField(HANDLE, bool);

namespace {
const uint64_t kInputLimit = 64ULL * 1024 * 1024;
const uint64_t kOutputLimit = 256ULL * 1024 * 1024;
volatile std::sig_atomic_t stopped;
void Signal(int) { stopped = 1; }

struct Deadline {
    std::chrono::steady_clock::time_point end =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);
    bool Expired() const {
        return stopped || std::chrono::steady_clock::now() >= end;
    }
    static int Interrupt(void *opaque) {
        return static_cast<Deadline *>(opaque)->Expired();
    }
};

bool Status(const char *name, BC_STATUS status) {
    if (status == BC_STS_SUCCESS) return true;
    std::fprintf(stderr, "%s status=%d\n", name, status);
    return false;
}

bool Number(const char *text, unsigned *value, unsigned limit = 1000) {
    if (!text || !*text) return false;
    unsigned parsed = 0;
    for (const char *p = text; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        const unsigned digit = static_cast<unsigned>(*p - '0');
        if (parsed > limit / 10U ||
            (parsed == limit / 10U && digit > limit % 10U)) return false;
        parsed = parsed * 10U + digit;
    }
    if (parsed == 0) return false;
    *value = parsed;
    return true;
}

uint64_t Token(size_t index) { return (index + 1U) * 100000ULL; }

bool TokenIndex(uint64_t token, unsigned count, size_t *index) {
    if (!token || token % 100000ULL || token / 100000ULL > count)
        return false;
    *index = static_cast<size_t>(token / 100000ULL - 1U);
    return true;
}

struct Packet {
    std::vector<uint8_t> bytes;
    uint32_t size = 0;
    gsize reservation = 0;
};

struct Input {
    unsigned width = 0, height = 0;
    std::vector<Packet> packets;
};

struct Options {
    unsigned scale_width = 0;
    unsigned coded_height = 0;
    bool full_payload = false;
};

struct OutputGeometry {
    unsigned width = 0, height = 0;
    unsigned alternate_height = 0;
    bool full_payload = false;
    unsigned Rows() const { return full_payload ? height : (height + 1U) / 2U; }
    const char *Kind() const { return full_payload ? "full-height" : "field"; }
};

// Admit only the separately selected source-width observation and stock field
// scaler contracts. Scaling changes output geometry, not the raw source-width
// scan decision. The public scaler API ignores sHeight.
bool SelectGeometry(const Input &input, const Options &options,
                    OutputGeometry *output) {
    if (!input.width || !input.height || input.width > 1920 ||
        input.height > 1088 || (input.width & 1U) || (input.height & 3U) ||
        (options.coded_height && !options.full_payload))
        return false;
    output->width = input.width;
    output->height = input.height;
    output->full_payload = options.full_payload;
    output->alternate_height = input.height == 1080 && !options.scale_width
        ? 1088U : 0U;
    if (options.full_payload) {
        // Observe either the visible-height setup or VA's explicit MBAFF
        // coded-height setup. Copy every returned row; compare visible crop
        // separately. Neither setup asserts progressive pixels or Bob.
        output->alternate_height = 0;
        if (options.coded_height) output->height = options.coded_height;
        return input.width == 1280 &&
            (input.height == 720 || input.height == 736) &&
            options.scale_width == 0 &&
            (!options.coded_height || options.coded_height == 736);
    }
    if (options.scale_width) {
        const unsigned width = options.scale_width;
        if (width < 128 || width > 1918 || width > input.width ||
            (width & 15U)) return false;
        unsigned field_height = static_cast<unsigned>(
            static_cast<uint64_t>(input.height / 2U) * width / input.width);
        field_height += field_height & 1U;
        if (!field_height) return false;
        output->width = width;
        output->height = field_height * 2U;
    }
    return true;
}

bool StartCode(const uint8_t *data, size_t size) {
    return data && size >= 4 && data[0] == 0 && data[1] == 0 &&
        (data[2] == 1 || (data[2] == 0 && data[3] == 1));
}

bool Load(const char *path, unsigned expected, Deadline *deadline, Input *input) {
    struct stat info = {};
    if (stat(path, &info) || !S_ISREG(info.st_mode) || info.st_size <= 0 ||
        static_cast<uint64_t>(info.st_size) > kInputLimit) return false;
    AVFormatContext *context = avformat_alloc_context();
    if (!context) return false;
    context->interrupt_callback = AVIOInterruptCB{Deadline::Interrupt, deadline};
    if (avformat_open_input(&context, path, nullptr, nullptr) < 0) {
        avformat_close_input(&context);
        return false;
    }
    bool ok = avformat_find_stream_info(context, nullptr) >= 0;
    const int stream = ok ? av_find_best_stream(context, AVMEDIA_TYPE_VIDEO,
        -1, -1, nullptr, 0) : -1;
    ok = stream >= 0;
    if (ok) {
        const AVCodecParameters *p = context->streams[stream]->codecpar;
        ok = !std::strcmp(context->iformat->name, "mpegts") &&
            p->codec_id == AV_CODEC_ID_H264 && p->width > 0 &&
            p->width <= 1920 && !(p->width & 1) && p->height > 0 &&
            p->height <= 1088 && !(p->height & 3) &&
            (p->field_order == AV_FIELD_TT || p->field_order == AV_FIELD_BB ||
             p->field_order == AV_FIELD_TB || p->field_order == AV_FIELD_BT);
        if (ok) {
            input->width = static_cast<unsigned>(p->width);
            input->height = static_cast<unsigned>(p->height);
        }
    }
    AVPacket *packet = av_packet_alloc();
    ok = ok && packet;
    uint64_t total = 0;
    int result = 0;
    while (ok && !deadline->Expired() &&
           (result = av_read_frame(context, packet)) >= 0) {
        if (packet->stream_index == stream) {
            Packet next;
            ok = packet->size > 0 && packet->data &&
                !(packet->flags & AV_PKT_FLAG_CORRUPT) &&
                input->packets.size() < expected;
            if (ok) {
                next.size = static_cast<uint32_t>(packet->size);
                ok = StartCode(packet->data, next.size) &&
                    gst_crystalhd_input_reservation(BC_MSUBTYPE_H264,
                        next.size, 0, &next.reservation) &&
                    next.size <= kInputLimit - total;
            }
            if (ok) {
                next.bytes.resize(next.size + AV_INPUT_BUFFER_PADDING_SIZE, 0);
                std::memcpy(next.bytes.data(), packet->data, next.size);
                total += next.size;
                input->packets.push_back(std::move(next));
            }
        }
        av_packet_unref(packet);
    }
    ok = ok && !deadline->Expired() && result == AVERROR_EOF &&
        input->packets.size() == expected && !input->packets.empty() &&
        gst_crystalhd_h264_has_parameter_sets(input->packets[0].bytes.data(),
            input->packets[0].size);
    av_packet_free(&packet);
    avformat_close_input(&context);
    return ok;
}

struct File {
    int fd = -1;
    ~File() { if (fd >= 0) close(fd); }
    bool Open(const std::string &path) {
        fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC |
                  O_NOFOLLOW, 0600);
        return fd >= 0;
    }
    bool Write(const void *data, size_t size) {
        const uint8_t *bytes = static_cast<const uint8_t *>(data);
        while (size) {
            const ssize_t wrote = write(fd, bytes, size);
            if (wrote < 0 && errno == EINTR && !stopped) continue;
            if (wrote <= 0) return false;
            bytes += wrote;
            size -= static_cast<size_t>(wrote);
        }
        return true;
    }
    bool Finish() {
        if (fd < 0) return false;
        const bool synced = fsync(fd) == 0;
        const int owned = fd;
        fd = -1;
        return close(owned) == 0 && synced;
    }
};

struct Device {
    HANDLE handle = nullptr;
    bool opened = false, started = false, selected = false;
    bool Close() {
        if (!handle) return true;
        bool ok = true;
        if (started) {
            const BC_STATUS status = DtsStopDecoder(handle);
            ok = Status("DtsStopDecoder", status);
            if (status == BC_STS_SUCCESS) {
                started = false;
                if (selected) {
                    const BC_STATUS reset = DtsFWSetSingleField(handle, false);
                    std::fprintf(stderr, "restore-single-field=0 status=%d\n", reset);
                    ok = Status("DtsFWSetSingleField(restore)", reset) && ok;
                    selected = false;
                }
            }
        }
        if (opened) {
            const BC_STATUS status = DtsCloseDecoder(handle);
            ok = Status("DtsCloseDecoder", status) && ok;
            if (status == BC_STS_SUCCESS) opened = started = false;
        }
        BC_STATUS status = BC_STS_BUSY;
        const auto end = std::chrono::steady_clock::now() +
            std::chrono::seconds(2);
        do {
            status = DtsDeviceClose(handle);
            if (status != BC_STS_BUSY) break;
            usleep(1000);
        } while (std::chrono::steady_clock::now() < end);
        ok = Status("DtsDeviceClose", status) && ok;
        // BUSY alone retains the same-process handle. Never reuse a consumed
        // DeviceClose handle even when its returned terminal status is nonzero.
        if (status != BC_STS_BUSY) handle = nullptr;
        return ok;
    }
    ~Device() { if (handle) (void)Close(); }
};

struct Lease {
    HANDLE handle;
    bool owned = true;
    explicit Lease(HANDLE value) : handle(value) {}
    bool Release() {
        if (!owned) return false;
        const BC_STATUS status = DtsReleaseOutputBuffs(handle, nullptr, FALSE);
        // A failed release is not retried through a different owner.
        owned = false;
        return Status("DtsReleaseOutputBuffs", status);
    }
    ~Lease() { if (owned) (void)Release(); }
};

struct RawMetadata {
    bool valid = false;
    uint32_t flags = 0, picture_number = 0, width = 0, line = 0;
};

// Validate all source extents before pointer arithmetic; no borrowed pointer
// survives Release(). Scaled widths have exact multiple-of-16 row pitches.
bool PayloadGeometry(const OutputGeometry &expected, const BC_DTS_PROC_OUT &out,
                     size_t *pitch, size_t *rows, size_t *active_bytes,
                     RawMetadata *metadata = nullptr) {
    const uint32_t field = out.PicInfo.flags & VDEC_FLAG_BOTTOMFIELD;
    const bool kind = (out.PicInfo.flags & VDEC_FLAG_INTERLACED_SRC) &&
        (field == VDEC_FLAG_TOPFIELD || field == VDEC_FLAG_BOTTOMFIELD ||
         (expected.full_payload && field == VDEC_FLAG_FIELDPAIR));
    if (!(out.PoutFlags & BC_POUT_FLAGS_PIB_VALID) || !out.Ybuff ||
        out.b422Mode != OUTPUT_MODE422_YUY2 ||
        (out.PoutFlags & BC_POUT_FLAGS_ENCRYPTED) ||
        !kind || out.PicInfo.width != expected.width ||
        (out.PicInfo.height != expected.height &&
         !(expected.alternate_height &&
           out.PicInfo.height == expected.alternate_height)))
        return false;
    const uint64_t row_pitch = ((static_cast<uint64_t>(expected.width) + 15U) &
                               ~15ULL) * 2U;
    const uint64_t coded_rows = expected.full_payload ? out.PicInfo.height :
        (static_cast<uint64_t>(out.PicInfo.height) + 1U) / 2U;
    const uint64_t visible_rows = expected.Rows();
    const uint64_t extent = row_pitch * coded_rows;
    const uint64_t bytes = static_cast<uint64_t>(expected.width) * 2U * visible_rows;
    if (!extent || extent > static_cast<uint64_t>(out.YBuffDoneSz) * 4U ||
        extent > std::numeric_limits<size_t>::max() ||
        bytes > std::numeric_limits<size_t>::max()) return false;
    RawMetadata raw;
    if (expected.full_payload) {
        const uint64_t done = static_cast<uint64_t>(out.YBuffDoneSz) * 4U;
        const uint64_t flags_offset = extent + sizeof(uint32_t) +
            offsetof(BC_PIC_INFO_BLOCK, flags);
        // The driver reads width/line from the last two completed DWORDs.
        // The embedded raw ordinal precedes BC_PIC_INFO_BLOCK by one DWORD.
        // Require both raw scalars to finish before these trailers.
        if (done > kOutputLimit || done > std::numeric_limits<size_t>::max() ||
            done < 8U || flags_offset + sizeof(uint32_t) > done - 8U)
            return false;
        std::memcpy(&raw.width, out.Ybuff + static_cast<size_t>(done - 8U),
            sizeof(raw.width));
        std::memcpy(&raw.line, out.Ybuff + static_cast<size_t>(done - 4U),
            sizeof(raw.line));
        if (raw.width != expected.width || raw.line != expected.height)
            return false;
        std::memcpy(&raw.picture_number,
            out.Ybuff + static_cast<size_t>(extent), sizeof(raw.picture_number));
        std::memcpy(&raw.flags, out.Ybuff + static_cast<size_t>(flags_offset),
            sizeof(raw.flags));
        raw.valid = true;
    }
    if (metadata) *metadata = raw;
    *pitch = static_cast<size_t>(row_pitch);
    *rows = static_cast<size_t>(visible_rows);
    *active_bytes = static_cast<size_t>(bytes);
    return true;
}

struct Capture {
    const OutputGeometry &geometry;
    unsigned expected;
    unsigned multiplicity;
    File &pixels;
    File &metadata;
    std::vector<unsigned> seen;
    std::vector<unsigned> parities;
    std::vector<uint8_t> staging;
    uint64_t bytes = 0;
    unsigned outputs = 0, formats = 0;
    bool marker = false, eos = false;
    unsigned ready = 0;

    Capture(const OutputGeometry &output, unsigned count, unsigned per_input,
            File &raw, File &tsv)
        : geometry(output), expected(count), multiplicity(per_input), pixels(raw),
          metadata(tsv), seen(count, 0), parities(count, 0),
          staging(static_cast<size_t>(output.width) * 2U * output.Rows()) {}

    bool Receive(HANDLE handle) {
        for (unsigned attempt = 0; attempt < 32; ++attempt) {
            BC_DTS_STATUS driver = {};
            if (!Status("DtsGetDriverStatus", DtsGetDriverStatus(handle, &driver)))
                return false;
            ready = driver.ReadyListCount;
            if (!ready) break;
            BC_DTS_PROC_OUT out = {};
            out.PicInfo.width = geometry.width;
            out.PicInfo.height = geometry.height;
            const BC_STATUS status = DtsProcOutputNoCopy(handle, 0, &out);
            const bool is_marker = (out.PicInfo.flags & VDEC_FLAG_EOS) != 0;
            marker |= is_marker;
            if (status == BC_STS_FMT_CHANGE) {
                ++formats;
                continue;
            }
            if (status != BC_STS_SUCCESS) {
                if (status == BC_STS_NO_DATA || status == BC_STS_TIMEOUT ||
                    status == BC_STS_BUSY) break;
                return Status("DtsProcOutputNoCopy", status);
            }
            Lease lease(handle);
            if (is_marker) {
                if (!lease.Release()) return false;
                continue;
            }
            size_t pitch = 0, rows = 0, active_bytes = 0, index = 0;
            RawMetadata raw;
            const bool valid = PayloadGeometry(geometry, out, &pitch, &rows,
                &active_bytes, &raw) && TokenIndex(out.PicInfo.timeStamp, expected,
                &index) && outputs < expected * multiplicity &&
                seen[index] < multiplicity &&
                active_bytes <= kOutputLimit - bytes;
            if (valid) {
                const size_t row_bytes = static_cast<size_t>(geometry.width) * 2U;
                for (size_t row = 0; row < rows; ++row)
                    std::memcpy(staging.data() + row * row_bytes,
                        out.Ybuff + row * pitch, row_bytes);
            }
            // The scalar copy remains valid after release; do not log host
            // addresses or perform file I/O while borrowing the DMA buffer.
            const BC_PIC_INFO_BLOCK info = out.PicInfo;
            const uint32_t pout = out.PoutFlags, words = out.YBuffDoneSz;
            if (!lease.Release()) return false;
            if (!valid) {
                std::fprintf(stderr, "invalid-%s token=%llu ordinal=%u geometry=%ux%u flags=%#x pout=%#x words=%u\n",
                    geometry.Kind(),
                    static_cast<unsigned long long>(info.timeStamp),
                    info.picture_number, info.width, info.height, info.flags,
                    pout, words);
                return false;
            }
            char line[512];
            const int length = std::snprintf(line, sizeof(line),
                "%u\t%llu\t%u\t%u\t%u\t%u\t%#x\t%#x\t%u\t%u\t%zu\t%u\t%zu\t%llu\t%zu\t%s\t%u\t%#x\t%u\t%u\t%u\n",
                outputs, static_cast<unsigned long long>(info.timeStamp),
                info.picture_number, info.sess_num, info.width, info.height,
                info.flags, pout, info.chroma_format, words, pitch,
                geometry.full_payload ? info.height : (info.height + 1U) / 2U, rows,
                static_cast<unsigned long long>(bytes), active_bytes,
                geometry.Kind(), raw.valid, raw.flags, raw.picture_number,
                raw.width, raw.line);
            if (length <= 0 || static_cast<size_t>(length) >= sizeof(line) ||
                !pixels.Write(staging.data(), active_bytes) ||
                !metadata.Write(line, static_cast<size_t>(length))) return false;
            ++seen[index];
            parities[index] |= (info.flags & VDEC_FLAG_BOTTOMFIELD) ==
                VDEC_FLAG_BOTTOMFIELD ? 2U : 1U;
            ++outputs;
            bytes += active_bytes;
        }
        uint8_t ended = 0;
        if (!Status("DtsIsEndOfStream", DtsIsEndOfStream(handle, &ended)))
            return false;
        eos = ended != 0;
        return true;
    }

    bool Exact() const {
        return outputs == expected * multiplicity &&
            std::all_of(seen.begin(), seen.end(), [this](unsigned count) {
                return count == multiplicity;
            }) && (multiplicity == 1U ||
                std::all_of(parities.begin(), parities.end(), [](unsigned mask) {
                    return mask == 3U;
                }));
    }
};

bool Run(const Input &input, const Options &options,
         const OutputGeometry &geometry, unsigned expected, bool single,
         File &raw, File &tsv, Deadline *deadline) {
    Device device;
    const uint32_t mode = DTS_PLAYBACK_MODE | DTS_LOAD_FILE_PLAY_FW |
        DTS_SKIP_TX_CHK_CPB | DTS_PLAYBACK_DROP_RPT_MODE |
        DTS_DFLT_RESOLUTION(vdecRESOLUTION_1080p23_976);
    bool ok = Status("DtsDeviceOpen", DtsDeviceOpen(&device.handle, mode));
    BC_INFO_CRYSTAL version = {};
    if (ok) ok = Status("DtsCrystalHDVersion", DtsCrystalHDVersion(device.handle,
        &version)) && version.device == 1;
    BC_INPUT_FORMAT format = {};
    format.Progressive = TRUE;
    format.OptFlags = 0x80000000U | vdecFrameRate30;
    format.mSubtype = BC_MSUBTYPE_H264;
    format.width = input.width;
    format.height = options.coded_height ? options.coded_height : input.height;
    format.startCodeSz = 4;
    if (ok) ok = Status("DtsSetInputFormat", DtsSetInputFormat(device.handle,
        &format));
    if (ok && options.scale_width) {
        BC_SCALING_PARAMS scale = {};
        scale.sWidth = options.scale_width;
        ok = Status("DtsSetScaleParams", DtsSetScaleParams(device.handle, &scale));
    }
    if (ok) device.opened = ok = Status("DtsOpenDecoder",
        DtsOpenDecoder(device.handle, BC_STREAM_TYPE_ES));
    if (ok) ok = Status("DtsSetColorSpace", DtsSetColorSpace(device.handle,
        OUTPUT_MODE422_YUY2));
    if (ok) device.started = ok = Status("DtsStartDecoder",
        DtsStartDecoder(device.handle));
    uint32_t stream_version = 0, decoder_version = 0, hardware = 0;
    if (ok) ok = Status("DtsGetFWVersion", DtsGetFWVersion(device.handle,
        &stream_version, &decoder_version, &hardware, nullptr, 1)) &&
        stream_version == 0x13600U && hardware == 0x70015U;
    std::fprintf(stderr, "firmware stream=%#x decoder=%#x hardware=%#x\n",
        stream_version, decoder_version, hardware);
    if (ok) {
        const BC_STATUS status = DtsFWSetSingleField(device.handle, single);
        std::fprintf(stderr, "set-single-field=%u status=%d\n", single, status);
        device.selected = status == BC_STS_SUCCESS;
        ok = Status("DtsFWSetSingleField", status);
    }
    if (ok) ok = Status("DtsStartCapture", DtsStartCapture(device.handle));
    Capture capture(geometry, expected, geometry.full_payload || single ? 1U : 2U,
        raw, tsv);
    size_t submitted = 0;
    bool flushed = false;
    while (ok && !deadline->Expired()) {
        ok = capture.Receive(device.handle);
        if (!ok) break;
        bool progressed = false;
        if (submitted < input.packets.size()) {
            const Packet &packet = input.packets[submitted];
            if (DtsTxFreeSize(device.handle) >= packet.reservation) {
                const BC_STATUS status = DtsProcInput(device.handle,
                    const_cast<uint8_t *>(packet.bytes.data()), packet.size,
                    Token(submitted), FALSE);
                if (status == BC_STS_SUCCESS) {
                    ++submitted;
                    progressed = true;
                } else if (status != BC_STS_BUSY && status != BC_STS_NO_DATA) {
                    ok = Status("DtsProcInput", status);
                }
            }
        } else if (!flushed &&
                   DtsTxFreeSize(device.handle) >= GST_CRYSTALHD_EOS_RESERVATION) {
            if (capture.eos) {
                std::fprintf(stderr, "stale EOS before this drain\n");
                ok = false;
            } else {
                ok = Status("DtsFlushInput(0)", DtsFlushInput(device.handle, 0));
                flushed = ok;
                progressed = true;
            }
        }
        if (flushed && capture.eos && capture.ready == 0) break;
        if (!progressed) usleep(1000);
    }
    ok = ok && !deadline->Expired() && submitted == expected && flushed &&
        capture.eos && capture.ready == 0 && capture.Exact();
    const bool closed = device.Close();
    ok = closed && ok;
    std::printf("single=%u inputs=%zu expected-inputs=%u payload-kind=%s scale-width=%u geometry=%ux%u outputs=%u expected-outputs=%u eos=%u marker=%u ready=%u formats=%u bytes=%llu cleanup=%s result=%s\n",
        single, submitted, expected, geometry.Kind(), options.scale_width,
        geometry.width, geometry.height, capture.outputs,
        expected * capture.multiplicity, capture.eos, capture.marker,
        capture.ready, capture.formats,
        static_cast<unsigned long long>(capture.bytes),
        closed ? "PASS" : "FAIL", ok ? "PASS" : "FAIL");
    for (size_t i = 0; i < capture.seen.size(); ++i) {
        if (capture.seen[i] != capture.multiplicity)
            std::printf("token=%llu outputs=%u expected=%u\n",
                static_cast<unsigned long long>(Token(i)), capture.seen[i],
                capture.multiplicity);
    }
    return ok;
}

bool SelfTest() {
    unsigned value = 0;
    size_t index = 0;
    Input input;
    input.width = 1920;
    input.height = 1080;
    Options options;
    OutputGeometry geometry;
    BC_DTS_PROC_OUT out = {};
    uint8_t dummy = 0;
    out.Ybuff = &dummy;
    out.b422Mode = 1;
    out.PoutFlags = BC_POUT_FLAGS_PIB_VALID;
    out.PicInfo.width = 1920;
    out.PicInfo.height = 1080;
    out.PicInfo.flags = VDEC_FLAG_INTERLACED_SRC | VDEC_FLAG_TOPFIELD;
    out.YBuffDoneSz = 1920U * 2U * 540U / 4U;
    size_t pitch = 0, rows = 0, bytes = 0;
    bool ok = SelectGeometry(input, options, &geometry) &&
        Number("50", &value) && value == 50 &&
        !Number("0", &value) && !Number("1001", &value) &&
        !Number("-1", &value) && !Number("1x", &value) &&
        TokenIndex(Token(49), 50, &index) && index == 49 &&
        !TokenIndex(0, 50, &index) && !TokenIndex(100001, 50, &index) &&
        !TokenIndex(Token(50), 50, &index) &&
        PayloadGeometry(geometry, out, &pitch, &rows, &bytes) &&
        pitch == 3840 && rows == 540 && bytes == 2073600;
    --out.YBuffDoneSz;
    ok = !PayloadGeometry(geometry, out, &pitch, &rows, &bytes) && ok;
    ++out.YBuffDoneSz;
    out.PicInfo.flags = VDEC_FLAG_INTERLACED_SRC;
    ok = !PayloadGeometry(geometry, out, &pitch, &rows, &bytes) && ok;
    out.PicInfo.flags |= VDEC_FLAG_BOTTOMFIELD;
    out.PoutFlags |= BC_POUT_FLAGS_ENCRYPTED;
    ok = !PayloadGeometry(geometry, out, &pitch, &rows, &bytes) && ok;
    out.PoutFlags &= ~BC_POUT_FLAGS_ENCRYPTED;
    out.PicInfo.height = 1088;
    out.YBuffDoneSz = 1920U * 2U * 544U / 4U;
    ok = PayloadGeometry(geometry, out, &pitch, &rows, &bytes) &&
        pitch == 3840 && rows == 540 && bytes == 2073600 && ok;
    --out.YBuffDoneSz;
    ok = !PayloadGeometry(geometry, out, &pitch, &rows, &bytes) && ok;

    options.scale_width = 960;
    ok = SelectGeometry(input, options, &geometry) &&
        geometry.width == 960 && geometry.height == 540 && ok;
    out.PicInfo.width = 960;
    out.PicInfo.height = 540;
    out.YBuffDoneSz = 960U * 2U * 270U / 4U;
    ok = PayloadGeometry(geometry, out, &pitch, &rows, &bytes) &&
        pitch == 1920 && rows == 270 && bytes == 518400 && ok;
    --out.YBuffDoneSz;
    ok = !PayloadGeometry(geometry, out, &pitch, &rows, &bytes) && ok;
    input.height = 1076;
    ok = SelectGeometry(input, options, &geometry) && geometry.height == 540 && ok;
    options.scale_width = 962;
    ok = !SelectGeometry(input, options, &geometry) && ok;
    options.scale_width = 1920;
    ok = !SelectGeometry(input, options, &geometry) && ok;
    options.scale_width = 0;
    options.full_payload = true;
    ok = !SelectGeometry(input, options, &geometry) && ok;
    input.width = 1280;
    input.height = 720;
    ok = SelectGeometry(input, options, &geometry) && geometry.full_payload && ok;
    out.PicInfo.width = 1280;
    out.PicInfo.height = 720;
    out.PicInfo.flags = VDEC_FLAG_INTERLACED_SRC | VDEC_FLAG_UNKNOWN_SRC |
        VDEC_FLAG_TOPFIELD;
    const size_t payload_bytes = 1280U * 2U * 720U;
    std::vector<uint8_t> full_output(payload_bytes + 160U, 0);
    out.Ybuff = full_output.data();
    out.YBuffDoneSz = static_cast<uint32_t>(full_output.size() / 4U);
    uint32_t word = 1280;
    std::memcpy(full_output.data() + full_output.size() - 8U, &word, sizeof(word));
    word = 720;
    std::memcpy(full_output.data() + full_output.size() - 4U, &word, sizeof(word));
    word = 3;
    std::memcpy(full_output.data() + payload_bytes, &word, sizeof(word));
    word = 0x68;
    std::memcpy(full_output.data() + payload_bytes + sizeof(uint32_t) +
        offsetof(BC_PIC_INFO_BLOCK, flags), &word, sizeof(word));
    RawMetadata raw;
    ok = PayloadGeometry(geometry, out, &pitch, &rows, &bytes, &raw) &&
        pitch == 2560 && rows == 720 && bytes == 1843200 &&
        raw.valid && raw.flags == 0x68 && raw.picture_number == 3 &&
        raw.width == 1280 && raw.line == 720 && ok;
    out.YBuffDoneSz = static_cast<uint32_t>(payload_bytes / 4U) - 1U;
    ok = !PayloadGeometry(geometry, out, &pitch, &rows, &bytes) && ok;
    out.YBuffDoneSz = static_cast<uint32_t>((payload_bytes + 32U) / 4U);
    ok = !PayloadGeometry(geometry, out, &pitch, &rows, &bytes) && ok;
    out.YBuffDoneSz = static_cast<uint32_t>(full_output.size() / 4U);
    word = 1281;
    std::memcpy(full_output.data() + full_output.size() - 8U, &word, sizeof(word));
    ok = !PayloadGeometry(geometry, out, &pitch, &rows, &bytes) && ok;
    word = 1280U | 0x80000000U;
    std::memcpy(full_output.data() + full_output.size() - 8U, &word, sizeof(word));
    ok = !PayloadGeometry(geometry, out, &pitch, &rows, &bytes) && ok;
    word = 1280;
    std::memcpy(full_output.data() + full_output.size() - 8U, &word, sizeof(word));
    word = 360;
    std::memcpy(full_output.data() + full_output.size() - 4U, &word, sizeof(word));
    ok = !PayloadGeometry(geometry, out, &pitch, &rows, &bytes) && ok;
    word = 720;
    std::memcpy(full_output.data() + full_output.size() - 4U, &word, sizeof(word));
    out.PicInfo.flags = 0;
    ok = !PayloadGeometry(geometry, out, &pitch, &rows, &bytes) && ok;
    out.PicInfo.flags = VDEC_FLAG_INTERLACED_SRC | VDEC_FLAG_FIELDPAIR;
    ok = PayloadGeometry(geometry, out, &pitch, &rows, &bytes) && ok;
    out.PicInfo.flags = VDEC_FLAG_INTERLACED_SRC | VDEC_FLAG_BOTTOMFIELD;
    ok = PayloadGeometry(geometry, out, &pitch, &rows, &bytes) && ok;
    out.PicInfo.height = 736;
    ok = !PayloadGeometry(geometry, out, &pitch, &rows, &bytes) && ok;
    options.coded_height = 735;
    ok = !SelectGeometry(input, options, &geometry) && ok;
    options.coded_height = 736;
    ok = SelectGeometry(input, options, &geometry) &&
        geometry.height == 736 && geometry.Rows() == 736 &&
        !PayloadGeometry(geometry, out, &pitch, &rows, &bytes) && ok;
    const size_t padded_bytes = 1280U * 2U * 736U;
    full_output.assign(padded_bytes + 160U, 0);
    out.Ybuff = full_output.data();
    out.YBuffDoneSz = static_cast<uint32_t>(full_output.size() / 4U);
    word = 1280;
    std::memcpy(full_output.data() + full_output.size() - 8U, &word, sizeof(word));
    word = 736;
    std::memcpy(full_output.data() + full_output.size() - 4U, &word, sizeof(word));
    word = 3;
    std::memcpy(full_output.data() + padded_bytes, &word, sizeof(word));
    word = 0x48;
    std::memcpy(full_output.data() + padded_bytes + sizeof(uint32_t) +
        offsetof(BC_PIC_INFO_BLOCK, flags), &word, sizeof(word));
    ok = PayloadGeometry(geometry, out, &pitch, &rows, &bytes, &raw) &&
        rows == 736 && bytes == padded_bytes && raw.line == 736 &&
        raw.flags == 0x48 && ok;
    input.height = 736;
    options.coded_height = 0;
    ok = SelectGeometry(input, options, &geometry) &&
        PayloadGeometry(geometry, out, &pitch, &rows, &bytes) && ok;
    input.height = 732;
    ok = !SelectGeometry(input, options, &geometry) && ok;
    input.height = 720;
    options.coded_height = 736;
    options.full_payload = false;
    ok = !SelectGeometry(input, options, &geometry) && ok;
    options.full_payload = true;
    options.scale_width = 640;
    ok = !SelectGeometry(input, options, &geometry) && ok;
    std::printf("single-field device-free guards: %s\n", ok ? "PASS" : "FAIL");
    return ok;
}
} // namespace

int main(int argc, char **argv) {
    if (argc == 2 && !std::strcmp(argv[1], "--self-test"))
        return SelfTest() ? 0 : 1;
    unsigned expected = 0;
    Options options;
    bool arguments = true;
    for (int i = 5; i < argc && arguments; ++i) {
        if (!std::strcmp(argv[i], "--scale-width") && !options.scale_width &&
            i + 1 < argc) {
            arguments = Number(argv[++i], &options.scale_width, 1918);
        } else if (!std::strcmp(argv[i], "--full-payload") &&
                   !options.full_payload) {
            options.full_payload = true;
        } else if (!std::strcmp(argv[i], "--coded-height") &&
                   !options.coded_height && i + 1 < argc) {
            arguments = Number(argv[++i], &options.coded_height, 736);
        } else arguments = false;
    }
    if (argc < 5 || !arguments || !Number(argv[2], &expected) ||
        (std::strcmp(argv[3], "0") && std::strcmp(argv[3], "1")) ||
        argv[4][0] != '/' || !argv[4][1]) {
        std::fprintf(stderr, "usage: %s INPUT EXPECTED_INPUTS_1_TO_1000 0_OR_1 /absolute/NEW_OUTPUT_PREFIX [--scale-width WIDTH_128_TO_1918_MULTIPLE_16 | --full-payload [--coded-height 736]] | --self-test\n", argv[0]);
        return 2;
    }
    std::signal(SIGINT, Signal);
    std::signal(SIGTERM, Signal);
    Deadline deadline;
    Input input;
    if (!Load(argv[1], expected, &deadline, &input)) {
        std::fprintf(stderr, "Require bounded local interlaced H.264 MPEG-TS Annex-B fixture with exact packet count and initial SPS/PPS\n");
        return 2;
    }
    const bool single = !std::strcmp(argv[3], "1");
    OutputGeometry geometry;
    if (!SelectGeometry(input, options, &geometry)) {
        std::fprintf(stderr, "Require native field geometry, a multiple-of-16 scale width no greater than source width, or unscaled 1280x720/736 --full-payload; no device opened\n");
        return 2;
    }
    const uint64_t budget = static_cast<uint64_t>(geometry.width) * 2U *
        geometry.Rows() * expected * (geometry.full_payload || single ? 1U : 2U);
    if (budget > kOutputLimit) {
        std::fprintf(stderr, "Capture would exceed 256 MiB; no device opened\n");
        return 2;
    }
    File raw, tsv;
    const std::string prefix(argv[4]);
    if (!raw.Open(prefix + ".yuy2") || !tsv.Open(prefix + ".tsv")) {
        std::perror("new output file");
        return 2;
    }
    const char header[] = "output_index\ttoken\tpicture_number\tsession\twidth\theight\tpicture_flags\tpout_flags\tchroma_format\ty_done_words\tsource_pitch\tcoded_output_rows\toutput_rows\tbyte_offset\tbytes\tpayload_kind\traw_metadata_valid\traw_pib_flags\traw_picture_number\ttrailer_width\ttrailer_line\n";
    if (!tsv.Write(header, sizeof(header) - 1U)) return 2;
    try {
        const bool passed = Run(input, options, geometry, expected, single,
            raw, tsv, &deadline);
        const bool finished_raw = raw.Finish();
        const bool finished_tsv = tsv.Finish();
        return passed && finished_raw && finished_tsv ? 0 : 1;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "research client exception: %s\n", error.what());
        return 1;
    }
}
