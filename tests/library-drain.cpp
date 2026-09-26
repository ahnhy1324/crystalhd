// SPDX-License-Identifier: LGPL-2.1-or-later
// Optional direct-library firmware-EOS probe, not a pixel-quality benchmark.
// Use an external timeout as well: a userspace deadline cannot bound a stuck
// kernel ioctl or device close. --preflight never opens the CrystalHD device.
#include <bc_dts_types.h>
#include <libcrystalhd_if.h>
#include "../filters/gst/gst-plugin-1.0/gstcrystalhd-input.h"
extern "C" {
#include <libavformat/avformat.h>
}
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <sys/stat.h>
#include <utility>
#include <vector>

static volatile std::sig_atomic_t interrupted;
static void Interrupt(int) { interrupted = 1; }

struct Deadline {
    gint64 end;
    explicit Deadline(unsigned seconds) : end(g_get_monotonic_time() + seconds * G_USEC_PER_SEC) {}
    bool expired() const { return interrupted || g_get_monotonic_time() >= end; }
    static int Check(void *opaque) { return static_cast<Deadline *>(opaque)->expired(); }
};

struct Packet {
    std::vector<uint8_t> data;
    size_t size;
    gsize reservation;
};
struct Input {
    BC_MEDIA_SUBTYPE subtype = BC_MSUBTYPE_INVALID;
    unsigned width = 0, height = 0;
    std::vector<uint8_t> metadata;
    std::vector<Packet> packets;
};

static bool Number(const char *text, unsigned maximum, unsigned *value)
{
    if (!text || !*text || *text == '-' || *text == '+') return false;
    char *end = nullptr;
    errno = 0;
    const unsigned long parsed = std::strtoul(text, &end, 10);
    if (errno || !end || *end || parsed == 0 || parsed > maximum) return false;
    *value = static_cast<unsigned>(parsed);
    return true;
}

static bool StartCode(const uint8_t *data, size_t size)
{
    return size >= 4 && data[0] == 0 && data[1] == 0 &&
           (data[2] == 1 || (data[2] == 0 && data[3] == 1));
}

static bool Load(const char *path, unsigned expected, Deadline *deadline, Input *input)
{
    struct stat file;
    if (stat(path, &file) || !S_ISREG(file.st_mode) || file.st_size <= 0 ||
        file.st_size > 64 * 1024 * 1024) {
        std::fprintf(stderr, "Require a local regular fixture of at most 64 MiB\n");
        return false;
    }
    AVFormatContext *format = avformat_alloc_context();
    if (!format) return false;
    format->interrupt_callback = AVIOInterruptCB{Deadline::Check, deadline};
    if (avformat_open_input(&format, path, nullptr, nullptr) < 0) {
        avformat_close_input(&format);
        return false;
    }
    bool ok = avformat_find_stream_info(format, nullptr) >= 0;
    const int index = ok ? av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0) : -1;
    ok = index >= 0;
    if (ok) {
        const AVCodecParameters *parameters = format->streams[index]->codecpar;
        const char *demuxer = format->iformat->name;
        if (parameters->codec_id == AV_CODEC_ID_H264 && !std::strcmp(demuxer, "h264"))
            input->subtype = BC_MSUBTYPE_H264;
        else if (parameters->codec_id == AV_CODEC_ID_MPEG2VIDEO && !std::strcmp(demuxer, "mpegvideo"))
            input->subtype = BC_MSUBTYPE_MPEG2VIDEO;
        else if (parameters->codec_id == AV_CODEC_ID_VC1 && !std::strcmp(demuxer, "vc1"))
            input->subtype = BC_MSUBTYPE_VC1;
        else if (parameters->codec_id == AV_CODEC_ID_WMV3 && !std::strcmp(demuxer, "asf"))
            input->subtype = BC_MSUBTYPE_WMV3;
        ok = input->subtype != BC_MSUBTYPE_INVALID && parameters->width > 0 &&
             parameters->width <= 1920 && parameters->height > 0 && parameters->height <= 1088 &&
             (parameters->field_order == AV_FIELD_UNKNOWN || parameters->field_order == AV_FIELD_PROGRESSIVE);
        input->width = parameters->width;
        input->height = parameters->height;
        if (ok && input->subtype == BC_MSUBTYPE_WMV3) {
            // Same normalization as gstcrystalhd-codecs.h: four STRUCT_C bytes.
            ok = parameters->extradata &&
                 (parameters->extradata_size == 4 || parameters->extradata_size == 5);
            if (ok) input->metadata.assign(parameters->extradata, parameters->extradata + 4);
        }
    }
    AVPacket *packet = av_packet_alloc();
    ok = ok && packet;
    size_t total = 0;
    int result = 0;
    while (ok && !deadline->expired() && (result = av_read_frame(format, packet)) >= 0) {
        if (packet->stream_index == index) {
            Packet next;
            next.size = packet->size > 0 ? static_cast<size_t>(packet->size) : 0;
            ok = packet->data && !(packet->flags & AV_PKT_FLAG_CORRUPT) &&
                 gst_crystalhd_input_reservation(input->subtype, next.size,
                     input->metadata.size(), &next.reservation) &&
                 (input->subtype == BC_MSUBTYPE_WMV3 || StartCode(packet->data, next.size)) &&
                 input->packets.size() < expected && total + next.size <= 64 * 1024 * 1024;
            if (ok) {
                // The library may inspect a full startcode even for short ASF input.
                next.data.resize(next.size + AV_INPUT_BUFFER_PADDING_SIZE, 0);
                std::memcpy(next.data.data(), packet->data, next.size);
                total += next.size;
                input->packets.push_back(std::move(next));
            }
        }
        av_packet_unref(packet);
    }
    ok = ok && !deadline->expired() && result == AVERROR_EOF && input->packets.size() == expected;
    av_packet_free(&packet);
    avformat_close_input(&format);
    if (!ok) std::fprintf(stderr, "Unsupported/corrupt framing, deadline, or packet count (got %zu, expected %u)\n",
                          input->packets.size(), expected);
    return ok;
}

struct Device {
    HANDLE handle = nullptr;
    bool opened = false, started = false;
    bool Close() {
        bool ok = true;
        const auto record = [&](const char *operation, BC_STATUS status) {
            if (status != BC_STS_SUCCESS) {
                std::fprintf(stderr, "%s failed: %d\n", operation, status);
                ok = false;
            }
        };
        if (started) record("DtsStopDecoder", DtsStopDecoder(handle));
        started = false;
        if (opened) record("DtsCloseDecoder", DtsCloseDecoder(handle));
        opened = false;
        if (handle) record("DtsDeviceClose", DtsDeviceClose(handle));
        handle = nullptr;
        return ok;
    }
    ~Device() { if (handle) Close(); }
};

static bool Status(const char *operation, BC_STATUS status)
{
    if (status == BC_STS_SUCCESS) return true;
    std::fprintf(stderr, "%s failed: %d\n", operation, status);
    return false;
}

struct Audit {
    unsigned frames = 0;
    bool marker = false, eos = false;
    uint32_t ready = 0;
    std::set<uint64_t> pending;
};

static bool Receive(Device *device, const Input &input, Audit *audit)
{
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        BC_DTS_STATUS driver = {};
        if (!Status("DtsGetDriverStatus", DtsGetDriverStatus(device->handle, &driver))) return false;
        audit->ready = driver.ReadyListCount;
        if (!audit->ready) break;
        BC_DTS_PROC_OUT output = {};
        const BC_STATUS result = DtsProcOutputNoCopy(device->handle, 0, &output);
        const bool marker = (output.PicInfo.flags & VDEC_FLAG_EOS) != 0;
        audit->marker |= marker;
        if (result == BC_STS_SUCCESS) {
            bool valid = true;
            if (!marker) {
                const unsigned width = output.PicInfo.width, height = output.PicInfo.height;
                valid = (output.PoutFlags & BC_POUT_FLAGS_PIB_VALID) && output.Ybuff &&
                    !(output.PicInfo.flags & VDEC_FLAG_INTERLACED_SRC) && width == input.width &&
                    (height == input.height || (input.height == 1080 && height == 1088)) &&
                    static_cast<uint64_t>(output.YBuffDoneSz) * 4 >= static_cast<uint64_t>(width) * height * 2 &&
                    audit->pending.erase(output.PicInfo.timeStamp) == 1;
                if (valid) ++audit->frames;
            }
            // Every successful NoCopy fetch owns a lease, even invalid output.
            const bool released = Status("DtsReleaseOutputBuffs",
                DtsReleaseOutputBuffs(device->handle, nullptr, FALSE));
            if (!valid) std::fprintf(stderr, "Invalid progressive picture geometry/data/token: %llu\n",
                                    static_cast<unsigned long long>(output.PicInfo.timeStamp));
            if (!valid || !released) return false;
        } else if (result != BC_STS_FMT_CHANGE && result != BC_STS_NO_DATA &&
                   result != BC_STS_BUSY && result != BC_STS_TIMEOUT) {
            return Status("DtsProcOutputNoCopy", result);
        } else if (result != BC_STS_FMT_CHANGE && !marker) {
            break;
        }
    }
    uint8_t eos = 0;
    if (!Status("DtsIsEndOfStream", DtsIsEndOfStream(device->handle, &eos))) return false;
    audit->eos = eos != 0;
    return true;
}

static bool WaitInput(Device *device, const Input &input, Audit *audit,
                      gsize reservation, const Deadline &deadline)
{
    while (!deadline.expired()) {
        if (!Receive(device, input, audit)) return false;
        if (DtsTxFreeSize(device->handle) >= reservation) return true;
        g_usleep(1000);
    }
    std::fprintf(stderr, "Input admission deadline expired\n");
    return false;
}

static bool Run(Input &input, unsigned expected, unsigned seconds)
{
    Deadline deadline(seconds);
    Device device;
    Audit audit;
    const uint32_t mode = DTS_PLAYBACK_MODE | DTS_LOAD_FILE_PLAY_FW | DTS_SKIP_TX_CHK_CPB |
        DTS_PLAYBACK_DROP_RPT_MODE | DTS_SINGLE_THREADED_MODE |
        DTS_DFLT_RESOLUTION(vdecRESOLUTION_1080p23_976);
    bool ok = Status("DtsDeviceOpen", DtsDeviceOpen(&device.handle, mode));
    BC_INFO_CRYSTAL version = {};
    if (ok) ok = Status("DtsCrystalHDVersion", DtsCrystalHDVersion(device.handle, &version));
    if (ok && version.device != 1) {
        std::fprintf(stderr, "This firmware-marker probe is restricted to BCM70015\n");
        ok = false;
    }
    BC_INPUT_FORMAT format = {};
    // Match the production GStreamer input format; no AVC1/RCV conversion.
    format.Progressive = TRUE;
    format.OptFlags = 0x80000000U | vdecFrameRate59_94 | 0x40U;
    format.mSubtype = input.subtype;
    format.width = input.width;
    format.height = input.height;
    format.startCodeSz = input.subtype == BC_MSUBTYPE_H264 ? 4 : 0;
    format.pMetaData = input.metadata.empty() ? nullptr : input.metadata.data();
    format.metaDataSz = input.metadata.size();
    if (ok) ok = Status("DtsSetInputFormat", DtsSetInputFormat(device.handle, &format));
    if (ok) device.opened = ok = Status("DtsOpenDecoder", DtsOpenDecoder(device.handle, BC_STREAM_TYPE_ES));
    if (ok) ok = Status("DtsSetColorSpace", DtsSetColorSpace(device.handle, OUTPUT_MODE422_YUY2));
    if (ok) device.started = ok = Status("DtsStartDecoder", DtsStartDecoder(device.handle));
    if (ok) ok = Status("DtsStartCapture", DtsStartCapture(device.handle));
    uint64_t token = 100000;
    for (Packet &packet : input.packets) {
        if (!ok) break;
        ok = WaitInput(&device, input, &audit, packet.reservation, deadline) &&
             Status("DtsProcInput", DtsProcInput(device.handle, packet.data.data(),
                    packet.size, token, FALSE));
        if (ok) audit.pending.insert(token);
        token += 100000;
    }
    if (ok) ok = WaitInput(&device, input, &audit, GST_CRYSTALHD_EOS_RESERVATION, deadline) &&
                 Status("DtsFlushInput(0)", DtsFlushInput(device.handle, 0));
    // A complete frame count is deliberately NOT the termination condition.
    while (ok && !deadline.expired()) {
        ok = Receive(&device, input, &audit);
        // EOS reaching the decoder is not itself a delivery barrier: a driver
        // status poll can observe it before every accepted picture is fetched.
        if (!ok || (audit.eos && audit.marker && audit.ready == 0 && audit.pending.empty())) break;
        g_usleep(1000);
    }
    // Require the fetched firmware marker too: an accidentally selected old
    // library may synthesize IsEndOfStream from silence alone.
    ok = ok && !deadline.expired() && audit.eos && audit.marker && audit.ready == 0 &&
         audit.frames == expected && audit.pending.empty();
    const bool closed = device.Close();
    ok = ok && closed;
    std::printf("Library drain: frames=%u/%u pending=%zu firmware-EOS=%s output-marker=%s "
        "ready=%u cleanup=%s result=%s\n", audit.frames, expected, audit.pending.size(),
        audit.eos ? "yes" : "no", audit.marker ? "yes" : "no", audit.ready,
        closed ? "PASS" : "FAIL", ok ? "PASS" : "FAIL");
    return ok;
}

int main(int argc, char **argv)
{
    unsigned expected = 0, seconds = 30;
    if ((argc != 4 && argc != 5) ||
        (std::strcmp(argv[1], "--preflight") && std::strcmp(argv[1], "--hardware")) ||
        !Number(argv[3], 10000, &expected) || (argc == 5 && !Number(argv[4], 300, &seconds))) {
        std::fprintf(stderr, "usage: %s --preflight|--hardware LOCAL_VIDEO EXPECTED_FRAMES [TIMEOUT_SECONDS]\n", argv[0]);
        return 2;
    }
    std::signal(SIGINT, Interrupt);
    std::signal(SIGTERM, Interrupt);
    Deadline deadline(seconds);
    Input input;
    if (!Load(argv[2], expected, &deadline, &input)) return 2;
    std::printf("Preflight: %ux%u subtype=%u packets=%zu metadata=%zu\n",
                input.width, input.height, input.subtype, input.packets.size(), input.metadata.size());
    if (!std::strcmp(argv[1], "--preflight")) return 0;
    return Run(input, expected, seconds) ? 0 : 1;
}
