// SPDX-License-Identifier: LGPL-2.1-or-later
// Optional BCM70015 research client: drain raw fields without VA's field pairing.
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

bool Number(const char *text, unsigned *value) {
    if (!text || !*text) return false;
    unsigned parsed = 0;
    for (const char *p = text; *p; ++p) {
        if (*p < '0' || *p > '9' || parsed > 1000U) return false;
        parsed = parsed * 10U + static_cast<unsigned>(*p - '0');
    }
    if (parsed == 0 || parsed > 1000) return false;
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

// Validate all source extents before pointer arithmetic; no borrowed pointer
// survives Release(). The tested 1920-pixel fixture has no horizontal padding.
bool FieldGeometry(const Input &input, const BC_DTS_PROC_OUT &out,
                   size_t *pitch, size_t *rows, size_t *active_bytes) {
    const uint32_t field = out.PicInfo.flags & VDEC_FLAG_BOTTOMFIELD;
    if (!(out.PoutFlags & BC_POUT_FLAGS_PIB_VALID) || !out.Ybuff ||
        out.b422Mode != OUTPUT_MODE422_YUY2 ||
        (out.PoutFlags & BC_POUT_FLAGS_ENCRYPTED) ||
        !(out.PicInfo.flags & VDEC_FLAG_INTERLACED_SRC) ||
        (field != VDEC_FLAG_TOPFIELD && field != VDEC_FLAG_BOTTOMFIELD) ||
        out.PicInfo.width != input.width ||
        (out.PicInfo.height != input.height &&
         !(input.height == 1080 && out.PicInfo.height == 1088)))
        return false;
    const uint64_t row_pitch = ((static_cast<uint64_t>(input.width) + 15U) &
                               ~15ULL) * 2U;
    const uint64_t field_rows = (static_cast<uint64_t>(out.PicInfo.height) + 1U) / 2U;
    const uint64_t visible_rows = (static_cast<uint64_t>(input.height) + 1U) / 2U;
    const uint64_t extent = row_pitch * field_rows;
    const uint64_t bytes = static_cast<uint64_t>(input.width) * 2U * visible_rows;
    if (!extent || extent > static_cast<uint64_t>(out.YBuffDoneSz) * 4U ||
        extent > std::numeric_limits<size_t>::max() ||
        bytes > std::numeric_limits<size_t>::max()) return false;
    *pitch = static_cast<size_t>(row_pitch);
    *rows = static_cast<size_t>(visible_rows);
    *active_bytes = static_cast<size_t>(bytes);
    return true;
}

struct Capture {
    const Input &input;
    unsigned expected;
    unsigned multiplicity;
    File &pixels;
    File &metadata;
    std::vector<unsigned> seen;
    std::vector<unsigned> parities;
    std::vector<uint8_t> staging;
    uint64_t bytes = 0;
    unsigned fields = 0, formats = 0;
    bool marker = false, eos = false;
    unsigned ready = 0;

    Capture(const Input &source, unsigned count, unsigned per_input,
            File &raw, File &tsv)
        : input(source), expected(count), multiplicity(per_input), pixels(raw),
          metadata(tsv), seen(count, 0), parities(count, 0),
          staging(static_cast<size_t>(source.width) * 2U *
                  ((source.height + 1U) / 2U)) {}

    bool Receive(HANDLE handle) {
        for (unsigned attempt = 0; attempt < 32; ++attempt) {
            BC_DTS_STATUS driver = {};
            if (!Status("DtsGetDriverStatus", DtsGetDriverStatus(handle, &driver)))
                return false;
            ready = driver.ReadyListCount;
            if (!ready) break;
            BC_DTS_PROC_OUT out = {};
            out.PicInfo.width = input.width;
            out.PicInfo.height = input.height;
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
            const bool valid = FieldGeometry(input, out, &pitch, &rows,
                &active_bytes) && TokenIndex(out.PicInfo.timeStamp, expected,
                &index) && fields < expected * 2U &&
                active_bytes <= kOutputLimit - bytes;
            if (valid) {
                const size_t row_bytes = static_cast<size_t>(input.width) * 2U;
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
                std::fprintf(stderr, "invalid-field token=%llu ordinal=%u geometry=%ux%u flags=%#x pout=%#x words=%u\n",
                    static_cast<unsigned long long>(info.timeStamp),
                    info.picture_number, info.width, info.height, info.flags,
                    pout, words);
                return false;
            }
            char line[512];
            const int length = std::snprintf(line, sizeof(line),
                "%u\t%llu\t%u\t%u\t%u\t%u\t%#x\t%#x\t%u\t%u\t%zu\t%u\t%zu\t%llu\t%zu\n",
                fields, static_cast<unsigned long long>(info.timeStamp),
                info.picture_number, info.sess_num, info.width, info.height,
                info.flags, pout, info.chroma_format, words, pitch,
                (info.height + 1U) / 2U, rows,
                static_cast<unsigned long long>(bytes), active_bytes);
            if (length <= 0 || static_cast<size_t>(length) >= sizeof(line) ||
                !pixels.Write(staging.data(), active_bytes) ||
                !metadata.Write(line, static_cast<size_t>(length))) return false;
            ++seen[index];
            parities[index] |= (info.flags & VDEC_FLAG_BOTTOMFIELD) ==
                VDEC_FLAG_BOTTOMFIELD ? 2U : 1U;
            ++fields;
            bytes += active_bytes;
        }
        uint8_t ended = 0;
        if (!Status("DtsIsEndOfStream", DtsIsEndOfStream(handle, &ended)))
            return false;
        eos = ended != 0;
        return true;
    }

    bool Exact() const {
        return fields == expected * multiplicity &&
            std::all_of(seen.begin(), seen.end(), [this](unsigned count) {
                return count == multiplicity;
            }) && (multiplicity == 1U ||
                std::all_of(parities.begin(), parities.end(), [](unsigned mask) {
                    return mask == 3U;
                }));
    }
};

bool Run(const Input &input, unsigned expected, bool single,
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
    format.height = input.height;
    format.startCodeSz = 4;
    if (ok) ok = Status("DtsSetInputFormat", DtsSetInputFormat(device.handle,
        &format));
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
    Capture capture(input, expected, single ? 1U : 2U, raw, tsv);
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
    std::printf("single=%u inputs=%zu expected-inputs=%u fields=%u expected-fields=%u eos=%u marker=%u ready=%u formats=%u bytes=%llu cleanup=%s result=%s\n",
        single, submitted, expected, capture.fields,
        expected * (single ? 1U : 2U), capture.eos, capture.marker,
        capture.ready, capture.formats,
        static_cast<unsigned long long>(capture.bytes),
        closed ? "PASS" : "FAIL", ok ? "PASS" : "FAIL");
    for (size_t i = 0; i < capture.seen.size(); ++i) {
        if (capture.seen[i] != capture.multiplicity)
            std::printf("token=%llu fields=%u expected=%u\n",
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
    bool ok = Number("50", &value) && value == 50 &&
        !Number("0", &value) && !Number("1001", &value) &&
        !Number("-1", &value) && !Number("1x", &value) &&
        TokenIndex(Token(49), 50, &index) && index == 49 &&
        !TokenIndex(0, 50, &index) && !TokenIndex(100001, 50, &index) &&
        !TokenIndex(Token(50), 50, &index) &&
        FieldGeometry(input, out, &pitch, &rows, &bytes) &&
        pitch == 3840 && rows == 540 && bytes == 2073600;
    --out.YBuffDoneSz;
    ok = !FieldGeometry(input, out, &pitch, &rows, &bytes) && ok;
    ++out.YBuffDoneSz;
    out.PicInfo.flags = VDEC_FLAG_INTERLACED_SRC;
    ok = !FieldGeometry(input, out, &pitch, &rows, &bytes) && ok;
    out.PicInfo.flags |= VDEC_FLAG_BOTTOMFIELD;
    out.PoutFlags |= BC_POUT_FLAGS_ENCRYPTED;
    ok = !FieldGeometry(input, out, &pitch, &rows, &bytes) && ok;
    out.PoutFlags &= ~BC_POUT_FLAGS_ENCRYPTED;
    out.PicInfo.height = 1088;
    out.YBuffDoneSz = 1920U * 2U * 544U / 4U;
    ok = FieldGeometry(input, out, &pitch, &rows, &bytes) &&
        pitch == 3840 && rows == 540 && bytes == 2073600 && ok;
    --out.YBuffDoneSz;
    ok = !FieldGeometry(input, out, &pitch, &rows, &bytes) && ok;
    std::printf("single-field device-free guards: %s\n", ok ? "PASS" : "FAIL");
    return ok;
}
} // namespace

int main(int argc, char **argv) {
    if (argc == 2 && !std::strcmp(argv[1], "--self-test"))
        return SelfTest() ? 0 : 1;
    unsigned expected = 0;
    if (argc != 5 || !Number(argv[2], &expected) ||
        (std::strcmp(argv[3], "0") && std::strcmp(argv[3], "1")) ||
        argv[4][0] != '/' || !argv[4][1]) {
        std::fprintf(stderr, "usage: %s INPUT EXPECTED_INPUTS_1_TO_1000 0_OR_1 /absolute/NEW_OUTPUT_PREFIX | --self-test\n", argv[0]);
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
    const uint64_t budget = static_cast<uint64_t>(input.width) * 2U *
        ((input.height + 1U) / 2U) * expected * (single ? 1U : 2U);
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
    const char header[] = "field_index\ttoken\tpicture_number\tsession\twidth\theight\tpicture_flags\tpout_flags\tchroma_format\ty_done_words\tsource_pitch\tcoded_field_rows\tvisible_field_rows\tbyte_offset\tbytes\n";
    if (!tsv.Write(header, sizeof(header) - 1U)) return 2;
    try {
        const bool passed = Run(input, expected, single, raw, tsv, &deadline);
        const bool finished_raw = raw.Finish();
        const bool finished_tsv = tsv.Finish();
        return passed && finished_raw && finished_tsv ? 0 : 1;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "research client exception: %s\n", error.what());
        return 1;
    }
}
