// SPDX-License-Identifier: GPL-2.0-or-later
// Real-device, single-plane MMAP H.264 probe. No driver-private ioctls.
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <glob.h>
#include <dlfcn.h>
#include <cstdint>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include <algorithm>
#include <memory>
extern "C" {
#include "bc_dts_types.h"
#include "bc_dts_defs.h"
#include "libcrystalhd_if.h"
#include <libavformat/avformat.h>
#include <libavcodec/bsf.h>
#include <libavutil/sha.h>
}

static void require(bool yes, const std::string &what)
{
    if (!yes) throw std::runtime_error(what);
}
static int call(int fd, unsigned long request, void *arg)
{
    int rc;
    do { rc = ioctl(fd, request, arg); } while (rc < 0 && errno == EINTR);
    return rc;
}
static void checked(int fd, unsigned long request, void *arg, const char *what)
{
    if (call(fd, request, arg) < 0)
        throw std::runtime_error(std::string(what) + ": " + strerror(errno));
}
struct Packet { std::vector<unsigned char> data; uint64_t timestamp; int64_t display_order; bool keyframe; };
struct Segment { size_t begin, end; unsigned width, height; std::string path; };
struct Input {
    std::vector<Packet> packets;
    unsigned width, height;
    size_t largest = 0;
    std::vector<Segment> segments;
};
static Input demux(const std::string &path, bool stress)
{
    struct stat info{};
    require(!stat(path.c_str(), &info) && S_ISREG(info.st_mode), "input must be a local regular fixture file");
    AVFormatContext *raw = nullptr;
    require(avformat_open_input(&raw, path.c_str(), nullptr, nullptr) >= 0, "open input failed");
    auto close_format = [](AVFormatContext *p) { avformat_close_input(&p); };
    std::unique_ptr<AVFormatContext, decltype(close_format)> fmt(raw, close_format);
    require(avformat_find_stream_info(raw, nullptr) >= 0, "stream info failed");
    int index = av_find_best_stream(raw, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    require(index >= 0, "no video stream");
    AVStream *stream = raw->streams[index];
    require(stream->codecpar->codec_id == AV_CODEC_ID_H264, "input must be H.264");
    AVBSFContext *filter = nullptr;
    const AVBitStreamFilter *bsf = av_bsf_get_by_name("h264_mp4toannexb");
    require(bsf && av_bsf_alloc(bsf, &filter) >= 0, "allocate Annex-B filter failed");
    auto free_bsf = [](AVBSFContext *p) { av_bsf_free(&p); };
    std::unique_ptr<AVBSFContext, decltype(free_bsf)> filter_owner(filter, free_bsf);
    require(avcodec_parameters_copy(filter->par_in, stream->codecpar) >= 0, "copy codec parameters");
    filter->time_base_in = stream->time_base;
    require(av_bsf_init(filter) >= 0, "initialize Annex-B filter failed");
    auto free_packet = [](AVPacket *p) { av_packet_free(&p); };
    std::unique_ptr<AVPacket, decltype(free_packet)> packet(av_packet_alloc(), free_packet);
    require(bool(packet), "allocate packet failed");
    Input input{{}, unsigned(stream->codecpar->width), unsigned(stream->codecpar->height), 0, {}};
    size_t total = 0;
    auto receive = [&]() {
        for (;;) {
            int rc = av_bsf_receive_packet(filter, packet.get());
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
            require(rc >= 0, "Annex-B filter receive failed");
            if (packet->size) {
                int64_t ts = packet->pts == AV_NOPTS_VALUE ?
                    int64_t(input.packets.size()) * 40000 :
                    av_rescale_q(packet->pts, filter->time_base_out, AVRational{1, 1000000});
                int64_t display_order = ts;
                static const uint64_t pattern[] = {0, 70000, 70000, 1000, 9000, 0};
                if (stress) ts = pattern[input.packets.size() % 6];
                require(ts >= 0, "negative input PTS unsupported; use --timestamps stress");
                total += packet->size;
                require(total <= 256U * 1024U * 1024U, "fixture exceeds 256 MiB bound");
                input.largest = std::max(input.largest, size_t(packet->size));
                input.packets.push_back({std::vector<unsigned char>(packet->data,
                    packet->data + packet->size), uint64_t(ts), display_order, bool(packet->flags & AV_PKT_FLAG_KEY)});
            }
            av_packet_unref(packet.get());
        }
    };
    int rc;
    while ((rc = av_read_frame(raw, packet.get())) >= 0) {
        if (packet->stream_index == index) {
            require(av_bsf_send_packet(filter, packet.get()) >= 0, "Annex-B filter send failed");
            receive();
        }
        av_packet_unref(packet.get());
    }
    require(rc == AVERROR_EOF, "input read failed before EOF");
    require(av_bsf_send_packet(filter, nullptr) >= 0, "flush Annex-B filter failed");
    receive();
    require(!input.packets.empty(), "no access units in fixture");
    input.segments.push_back({0, input.packets.size(), input.width, input.height, path});
    return input;
}

static void append_input(Input &input, Input next)
{
    const size_t begin = input.packets.size();
    const uint64_t timestamp_offset = uint64_t(input.segments.size()) << 32;
    int64_t display_offset = 0;
    for (const auto &p : input.packets) display_offset = std::max(display_offset, p.display_order + 1);
    int64_t first_display = next.packets.front().display_order;
    for (const auto &p : next.packets) first_display = std::min(first_display, p.display_order);
    for (auto &p : next.packets) {
        require(p.timestamp <= UINT64_MAX / 1000 - timestamp_offset, "dynamic fixture timestamp overflow");
        p.timestamp += timestamp_offset;
        p.display_order += display_offset - first_display;
        input.packets.push_back(std::move(p));
    }
    input.largest = std::max(input.largest, next.largest);
    input.segments.push_back({begin, input.packets.size(), next.width, next.height, next.segments.front().path});
}

struct Device {
    int fd;
    explicit Device(const std::string &path) : fd(open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC))
    { require(fd >= 0, "open " + path + ": " + strerror(errno)); }
    ~Device() { close(fd); }
};
struct LegacyLibrary {
    using Open = BC_STATUS (*)(HANDLE *, uint32_t);
    using Close = BC_STATUS (*)(HANDLE);
    struct Unload { void operator()(void *p) const { dlclose(p); } };
    std::unique_ptr<void, Unload> library;
    Open open_device;
    Close close_device;
    LegacyLibrary() : library(dlopen("libcrystalhd.so.3", RTLD_NOW | RTLD_LOCAL)),
        open_device(nullptr), close_device(nullptr)
    {
        if (!library) {
            const char *error = dlerror();
            throw std::runtime_error(std::string("load libcrystalhd.so.3: ") + (error ? error : "unknown loader failure"));
        }
        open_device = reinterpret_cast<Open>(dlsym(library.get(), "DtsDeviceOpen"));
        close_device = reinterpret_cast<Close>(dlsym(library.get(), "DtsDeviceClose"));
        require(open_device && close_device, "installed libcrystalhd is missing public Open/Close symbols");
    }
};
struct LegacySession {
    LegacyLibrary &library;
    HANDLE handle = nullptr;
    explicit LegacySession(LegacyLibrary &lib, uint32_t mode = DTS_PLAYBACK_MODE) : library(lib)
    {
        HANDLE candidate = nullptr;
        BC_STATUS status = library.open_device(&candidate, mode);
        /* Open's failure cleanup can leave candidate pointing to a freed
         * context. Only a successful open grants a Close obligation.
         */
        require(status == BC_STS_SUCCESS, "legacy owner DtsDeviceOpen failed with status " + std::to_string(status));
        require(candidate != nullptr, "legacy successful open returned no handle");
        handle = candidate;
    }
    void close_checked() {
        HANDLE closing = handle; handle = nullptr;
        BC_STATUS status = library.close_device(closing);
        require(status == BC_STS_SUCCESS, "legacy owner DtsDeviceClose failed with status " + std::to_string(status));
    }
    ~LegacySession() { if (handle) library.close_device(handle); }
};
static void check_legacy_blocked(LegacyLibrary &library)
{
    HANDLE candidate = nullptr;
    BC_STATUS status = library.open_device(&candidate, DTS_PLAYBACK_MODE);
    if (status == BC_STS_SUCCESS) {
        library.close_device(candidate);
        throw std::runtime_error("legacy playback open acquired device while native session owned it");
    }
    /* The frozen legacy mode ioctl reports ERR_USAGE for an already-active
     * core, rather than translating ownership contention to BUSY. Accept it
     * only in this intentional negative-open check; the following native
     * decode must still pass every timestamp, frame and hash assertion.
     */
    require(status == BC_STS_BUSY || status == BC_STS_DEC_EXIST_OPEN || status == BC_STS_ERR_USAGE,
        "native-held legacy Open must return BUSY/DEC_EXIST_OPEN/ERR_USAGE, got " + std::to_string(status));
    const char *name = status == BC_STS_BUSY ? "BC_STS_BUSY" :
        status == BC_STS_DEC_EXIST_OPEN ? "BC_STS_DEC_EXIST_OPEN" : "BC_STS_ERR_USAGE";
    printf("legacy_arbitration direction=native_blocks_legacy status=%u status_name=%s\n", unsigned(status), name);
}
struct Queue {
    int fd;
    v4l2_buf_type type;
    bool streaming = false;
    struct Mapping { void *address; size_t length; bool queued = false; };
    std::vector<Mapping> buffers;
    Queue(int f, v4l2_buf_type t) : fd(f), type(t) {}
    ~Queue() {
        if (streaming) call(fd, VIDIOC_STREAMOFF, &type);
        for (auto &b : buffers) munmap(b.address, b.length);
        v4l2_requestbuffers req{};
        req.type = type; req.memory = V4L2_MEMORY_MMAP;
        call(fd, VIDIOC_REQBUFS, &req);
    }
    void allocate(unsigned count) {
        v4l2_requestbuffers req{};
        req.type = type; req.memory = V4L2_MEMORY_MMAP; req.count = count;
        checked(fd, VIDIOC_REQBUFS, &req, "REQBUFS");
        require(req.count > 0, "REQBUFS returned zero buffers");
        for (unsigned i = 0; i < req.count; i++) {
            v4l2_buffer b{};
            b.type = type; b.memory = V4L2_MEMORY_MMAP; b.index = i;
            checked(fd, VIDIOC_QUERYBUF, &b, "QUERYBUF");
            void *p = mmap(nullptr, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, b.m.offset);
            require(p != MAP_FAILED, "MMAP: " + std::string(strerror(errno)));
            buffers.push_back({p, b.length, false});
        }
    }
    void queue(unsigned index, const Packet *packet = nullptr, uint64_t timestamp_offset = 0) {
        auto &mapping = buffers.at(index);
        v4l2_buffer b{};
        b.type = type; b.memory = V4L2_MEMORY_MMAP; b.index = index;
        if (packet) {
            require(packet->data.size() <= mapping.length, "AU exceeds OUTPUT buffer");
            memcpy(mapping.address, packet->data.data(), packet->data.size());
            b.bytesused = packet->data.size();
            require(packet->timestamp <= UINT64_MAX / 1000 - timestamp_offset,
                "timestamp exceeds nanosecond V4L2 representation");
            uint64_t timestamp = packet->timestamp + timestamp_offset;
            b.timestamp.tv_sec = timestamp / 1000000;
            b.timestamp.tv_usec = timestamp % 1000000;
            b.flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
        }
        checked(fd, VIDIOC_QBUF, &b, "QBUF");
        mapping.queued = true;
    }
    bool dequeue(v4l2_buffer &b, bool discard_retired = false) {
        b = {}; b.type = type; b.memory = V4L2_MEMORY_MMAP;
        if (call(fd, VIDIOC_DQBUF, &b) < 0) {
            if (errno == EAGAIN || errno == EPIPE) return false;
            throw std::runtime_error("DQBUF: " + std::string(strerror(errno)));
        }
        require(b.index < buffers.size(), "DQBUF index outside allocation");
        buffers[b.index].queued = false;
        require(discard_retired || !(b.flags & V4L2_BUF_FLAG_ERROR),
            "driver returned ERROR buffer: queue=" + std::to_string(type) +
            " index=" + std::to_string(b.index) +
            " timestamp_us=" + std::to_string(uint64_t(b.timestamp.tv_sec) * 1000000 + b.timestamp.tv_usec) +
            " bytesused=" + std::to_string(b.bytesused));
        return true;
    }
    void start() { checked(fd, VIDIOC_STREAMON, &type, "STREAMON"); streaming = true; }
    void stop() {
        checked(fd, VIDIOC_STREAMOFF, &type, "STREAMOFF");
        streaming = false;
        for (auto &b : buffers) b.queued = false;
    }
    void release() {
        if (streaming) stop();
        for (auto &b : buffers) require(!munmap(b.address, b.length), "unmap CAPTURE buffers");
        buffers.clear();
        v4l2_requestbuffers req{};
        req.type = type; req.memory = V4L2_MEMORY_MMAP;
        checked(fd, VIDIOC_REQBUFS, &req, "release CAPTURE REQBUFS");
    }
};
static std::string discover()
{
    glob_t matches{};
    glob("/sys/class/video4linux/video*/name", 0, nullptr, &matches);
    std::string found;
    for (size_t i = 0; i < matches.gl_pathc; i++) {
        std::ifstream file(matches.gl_pathv[i]); std::string name;
        std::getline(file, name);
        if (name.find("crystalhd") == std::string::npos && name.find("CrystalHD") == std::string::npos)
            continue;
        std::string path(matches.gl_pathv[i]);
        auto end = path.rfind('/'); auto begin = path.rfind('/', end - 1);
        found = "/dev/" + path.substr(begin + 1, end - begin - 1);
        break;
    }
    globfree(&matches);
    require(!found.empty(), "no CrystalHD V4L2 node discovered; supply --device");
    return found;
}

static std::string hash(AVSHA *sha)
{
    unsigned char digest[32]; char result[65];
    av_sha_final(sha, digest);
    for (unsigned i = 0; i < 32; i++) snprintf(result + i * 2, 3, "%02x", digest[i]);
    return result;
}

template <typename T> static T legacy_symbol(LegacyLibrary &library, const char *name)
{
    auto symbol = reinterpret_cast<T>(dlsym(library.library.get(), name));
    require(symbol != nullptr, std::string("missing legacy public symbol ") + name);
    return symbol;
}
static void legacy_status(const char *name, BC_STATUS status)
{
    require(status == BC_STS_SUCCESS, std::string(name) + " status=" + std::to_string(status));
}
static void legacy_contract(const Input &input, size_t prefix, unsigned timeout)
{
    require(prefix && prefix + 3 <= input.packets.size() && input.segments.size() == 1,
        "legacy contract needs a nonempty prefix and three continuation AUs in one segment");
    require(input.packets.front().keyframe && !input.packets[prefix].keyframe,
        "legacy contract must start at an IDR and resume at a non-IDR AU");
    LegacyLibrary library;
#define LEGACY_SYMBOL(name) auto name = legacy_symbol<decltype(&::name)>(library, #name)
    LEGACY_SYMBOL(DtsSetInputFormat); LEGACY_SYMBOL(DtsOpenDecoder);
    LEGACY_SYMBOL(DtsSetColorSpace); LEGACY_SYMBOL(DtsStartDecoder);
    LEGACY_SYMBOL(DtsStartCapture); LEGACY_SYMBOL(DtsStopDecoder);
    LEGACY_SYMBOL(DtsCloseDecoder); LEGACY_SYMBOL(DtsProcInput);
    LEGACY_SYMBOL(DtsTxFreeSize); LEGACY_SYMBOL(DtsProcOutputNoCopy);
    LEGACY_SYMBOL(DtsReleaseOutputBuffs); LEGACY_SYMBOL(DtsGetDriverStatus);
    LEGACY_SYMBOL(DtsIsEndOfStream); LEGACY_SYMBOL(DtsFlushInput);
    LEGACY_SYMBOL(DtsCrystalHDVersion);
#undef LEGACY_SYMBOL
    const uint32_t mode = DTS_PLAYBACK_MODE | DTS_LOAD_FILE_PLAY_FW | DTS_SKIP_TX_CHK_CPB |
        DTS_PLAYBACK_DROP_RPT_MODE | DTS_SINGLE_THREADED_MODE |
        DTS_DFLT_RESOLUTION(vdecRESOLUTION_1080p23_976);
    LegacySession session(library, mode);
    BC_INFO_CRYSTAL version{};
    legacy_status("DtsCrystalHDVersion", DtsCrystalHDVersion(session.handle, &version));
    require(version.device == 1, "legacy marker/packed-output contract requires BCM70015");
    struct DecoderCleanup {
        HANDLE handle;
        decltype(DtsStopDecoder) stop;
        decltype(DtsCloseDecoder) close;
        bool opened = false, started = false;
        bool finish() {
            bool ok = true;
            if (started) { auto rc = stop(handle); started = false;
                if (rc != BC_STS_SUCCESS) { fprintf(stderr, "DtsStopDecoder status=%u\n", unsigned(rc)); ok = false; } }
            if (opened) { auto rc = close(handle); opened = false;
                if (rc != BC_STS_SUCCESS) { fprintf(stderr, "DtsCloseDecoder status=%u\n", unsigned(rc)); ok = false; } }
            return ok;
        }
        ~DecoderCleanup() { finish(); }
    } cleanup{session.handle, DtsStopDecoder, DtsCloseDecoder};
    BC_INPUT_FORMAT format{};
    format.Progressive = TRUE;
    format.OptFlags = 0x80000000U | vdecFrameRate59_94 | 0x40U;
    format.mSubtype = BC_MSUBTYPE_H264;
    format.width = input.width; format.height = input.height; format.startCodeSz = 4;
    legacy_status("DtsSetInputFormat", DtsSetInputFormat(session.handle, &format));
    legacy_status("DtsOpenDecoder", DtsOpenDecoder(session.handle, BC_STREAM_TYPE_ES)); cleanup.opened = true;
    legacy_status("DtsSetColorSpace", DtsSetColorSpace(session.handle, OUTPUT_MODE422_YUY2));
    legacy_status("DtsStartDecoder", DtsStartDecoder(session.handle)); cleanup.started = true;
    legacy_status("DtsStartCapture", DtsStartCapture(session.handle));
    /* Positive, even opaque tokens avoid the frozen legacy zero-PTS sentinel.
     * Both drain ranges use the original demux display order, not FIFO order.
     */
    auto token = [](size_t packet) { return uint64_t(packet + 1) * 100000; };
    for (unsigned epoch = 0; epoch < 2; epoch++) {
        size_t begin = epoch ? prefix : 0, end = epoch ? prefix + 3 : prefix;
        std::vector<size_t> order;
        for (size_t i = begin; i < end; i++) order.push_back(i);
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            return input.packets[a].display_order < input.packets[b].display_order;
        });
        size_t frames = 0, submitted = begin;
        bool drained = false, flushed = false;
        uint32_t ready = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout);
        auto bounded = [&]() {
            require(std::chrono::steady_clock::now() < deadline,
                "legacy contract timed out: epoch=" + std::to_string(epoch) +
                " frames=" + std::to_string(frames) + " expected=" + std::to_string(order.size()) +
                " submitted=" + std::to_string(submitted - begin) + " eos=" + std::to_string(drained));
        };
        auto receive = [&]() {
            for (unsigned attempt = 0; attempt < 32; attempt++) {
                bounded();
                BC_DTS_STATUS driver{};
                legacy_status("DtsGetDriverStatus", DtsGetDriverStatus(session.handle, &driver));
                ready = driver.ReadyListCount;
                if (!ready) break;
                BC_DTS_PROC_OUT output{};
                BC_STATUS rc = DtsProcOutputNoCopy(session.handle, 0, &output);
                if (rc == BC_STS_SUCCESS) {
                    /* Release every successful NoCopy lease even on assertion failure. */
                    struct Lease {
                        HANDLE handle; decltype(DtsReleaseOutputBuffs) release; bool live = true;
                        ~Lease() { if (live) release(handle, nullptr, FALSE); }
                    } lease{session.handle, DtsReleaseOutputBuffs};
                    if (!(output.PicInfo.flags & VDEC_FLAG_EOS)) {
                        require(frames < order.size(), "legacy contract returned extra picture");
                        size_t packet = order[frames];
                        require(output.PicInfo.timeStamp == token(packet),
                            "legacy contract display-order mismatch: epoch=" + std::to_string(epoch) +
                            " frame=" + std::to_string(frames) + " packet=" + std::to_string(packet) +
                            " expected_pts=" + std::to_string(token(packet)) +
                            " actual_pts=" + std::to_string(output.PicInfo.timeStamp));
                        require((output.PoutFlags & BC_POUT_FLAGS_PIB_VALID) && output.Ybuff &&
                            !(output.PicInfo.flags & VDEC_FLAG_INTERLACED_SRC) &&
                            output.PicInfo.width == input.width &&
                            (output.PicInfo.height == input.height || (input.height == 1080 && output.PicInfo.height == 1088)) &&
                            uint64_t(output.YBuffDoneSz) * 4 >= uint64_t(input.width) * input.height * 2,
                            "legacy contract invalid YUYV geometry/payload");
                        AVSHA *sha = av_sha_alloc(); require(sha && !av_sha_init(sha, 256), "legacy SHA allocation");
                        av_sha_update(sha, output.Ybuff, size_t(input.width) * input.height * 2);
                        std::string digest = hash(sha); av_free(sha);
                        printf("legacy_contract epoch=%u frame=%zu packet=%zu pts=%llu flags=%x sha256=%s\n",
                            epoch, frames, packet, static_cast<unsigned long long>(output.PicInfo.timeStamp),
                            output.PicInfo.flags, digest.c_str()); fflush(stdout);
                        frames++;
                    } else require(flushed, "legacy contract EOS before flush");
                    rc = DtsReleaseOutputBuffs(session.handle, nullptr, FALSE); lease.live = false;
                    legacy_status("DtsReleaseOutputBuffs", rc);
                } else if (rc != BC_STS_FMT_CHANGE) {
                    require(rc == BC_STS_NO_DATA || rc == BC_STS_BUSY || rc == BC_STS_TIMEOUT,
                        "DtsProcOutputNoCopy status=" + std::to_string(rc));
                    break;
                }
            }
            uint8_t eos = 0;
            legacy_status("DtsIsEndOfStream", DtsIsEndOfStream(session.handle, &eos));
            drained = eos != 0;
        };
        auto wait_input = [&](size_t reservation) {
            do { bounded(); receive();
                if (DtsTxFreeSize(session.handle) >= reservation) return;
                usleep(1000);
            } while (true);
        };
        for (; submitted < end; submitted++) {
            const auto &data = input.packets[submitted].data;
            wait_input(data.size() + 32 * (data.size() / 60000 + 3) + 128);
            /* Padding is readable by the legacy Annex-B parser, not submitted. */
            auto padded = data; padded.resize(data.size() + AV_INPUT_BUFFER_PADDING_SIZE, 0);
            legacy_status("DtsProcInput", DtsProcInput(session.handle, padded.data(), data.size(), token(submitted), FALSE));
        }
        wait_input(1024);
        require(!drained, "legacy EOS stale before new flush after continuation input");
        legacy_status("DtsFlushInput(0)", DtsFlushInput(session.handle, 0)); flushed = true;
        do { bounded(); receive();
            if (drained && !ready && frames == order.size()) break;
            usleep(1000);
        } while (true);
        printf("legacy_contract epoch=%u frames=%zu expected=%zu firmware_eos=1 ready=0\n", epoch, frames, order.size());
        fflush(stdout);
    }
    require(cleanup.finish(), "legacy decoder cleanup failed");
    session.close_checked();
    puts("PASS legacy_contract epochs=2 continuation_aus=3 no_reset=1 firmware_eos=1");
}
static void check_busy(const std::string &path, const Input &input)
{
    Device contender(path);
    Queue output(contender.fd, V4L2_BUF_TYPE_VIDEO_OUTPUT);
    v4l2_format fmt{};
    fmt.type = output.type;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_H264;
    fmt.fmt.pix.width = input.width; fmt.fmt.pix.height = input.height;
    fmt.fmt.pix.sizeimage = std::max(size_t(2 * 1024 * 1024), input.largest);
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    checked(contender.fd, VIDIOC_S_FMT, &fmt, "contender S_FMT");
    output.allocate(2);
    output.queue(0, &input.packets.front());
    int rc = call(contender.fd, VIDIOC_STREAMON, &output.type);
    int saved_errno = errno;
    if (!rc) output.streaming = true;
    require(rc < 0 && saved_errno == EBUSY,
        "second native session STREAMON must fail EBUSY while first owns device");
    puts("busy_arbitration=EBUSY contender_closed=on_return");
}
static std::unique_ptr<Device> failed_streamon_preflight(const std::string &path)
{
    auto device = std::make_unique<Device>(path);
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    require(call(device->fd, VIDIOC_STREAMON, &type) < 0 && errno == EINVAL,
        "OUTPUT STREAMON without REQBUFS must fail EINVAL");
    /* Keep the failed file open: closing it would hide a leaked new lease. */
    puts("CONTRACT failed_streamon=EINVAL output_reqbufs=0 failed_fd_held=1 primary_decode_follows=1");
    return device;
}
static void check_legacy_owner(const std::string &path, const Input &input, LegacyLibrary &library)
{
    LegacySession owner(library);
    /* Public playback Open may initialize firmware. No decoder/video-start
     * operation is needed to hold the exclusive legacy session lease.
     */
    check_busy(path, input);
    owner.close_checked();
    puts("legacy_arbitration direction=legacy_blocks_native status=EBUSY legacy_closed=1 native_decode_follows=1");
}
static void decoder_command(int fd, unsigned command, const char *what)
{
    v4l2_decoder_cmd cmd{}; cmd.cmd = command;
    checked(fd, VIDIOC_DECODER_CMD, &cmd, what);
}
static void expect_decoder_command_errno(int fd, unsigned command, int expected, const char *what)
{
    v4l2_decoder_cmd cmd{}; cmd.cmd = command;
    int rc = call(fd, VIDIOC_DECODER_CMD, &cmd);
    int saved_errno = errno;
    require(rc < 0 && saved_errno == expected,
        std::string(what) + " must fail with " + strerror(expected) +
        ", got " + (rc < 0 ? strerror(saved_errno) : "success"));
}
static void assert_epipe(int fd)
{
    v4l2_buffer b{}; b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; b.memory = V4L2_MEMORY_MMAP;
    require(call(fd, VIDIOC_DQBUF, &b) < 0 && errno == EPIPE, "contract CAPTURE after LAST must return EPIPE");
}
static void configure_contract_output(int fd, Queue &output, const Input &input, unsigned buffers)
{
    v4l2_format fmt{}; fmt.type = output.type;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_H264; fmt.fmt.pix.field = V4L2_FIELD_NONE;
    fmt.fmt.pix.width = input.width; fmt.fmt.pix.height = input.height;
    fmt.fmt.pix.sizeimage = std::max(size_t(2 * 1024 * 1024), input.largest);
    checked(fd, VIDIOC_S_FMT, &fmt, "contract OUTPUT S_FMT");
    output.allocate(buffers);
}
template <typename Progress>
static void wait_source_change(int fd, unsigned timeout, Progress progress)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout);
    for (;;) {
        v4l2_event event{};
        if (!call(fd, VIDIOC_DQEVENT, &event)) {
            if (event.type == V4L2_EVENT_SOURCE_CHANGE &&
                (event.u.src_change.changes & V4L2_EVENT_SRC_CH_RESOLUTION)) return;
            continue;
        }
        require(errno == ENOENT || errno == EAGAIN, "contract DQEVENT failed");
        require(std::chrono::steady_clock::now() < deadline, "contract SOURCE_CHANGE timed out");
        progress();
        pollfd pfd{fd, POLLPRI, 0};
        int rc = poll(&pfd, 1, 20);
        require(rc >= 0 || errno == EINTR, "contract source poll failed");
    }
}
static void expect_drain_busy(int fd)
{
    for (unsigned command : {V4L2_DEC_CMD_STOP, V4L2_DEC_CMD_START}) {
        v4l2_decoder_cmd cmd{}; cmd.cmd = command;
        require(call(fd, VIDIOC_DECODER_CMD, &cmd) < 0 && errno == EBUSY,
            "STOP and START while drain awaits CAPTURE must return EBUSY");
    }
}
static std::vector<std::string> queued_stop_preflight(const std::string &path, const Input &input, unsigned timeout)
{
    require(input.segments.front().end >= 7, "contract preflight requires at least seven H.264 pictures in its first segment");
    require(input.packets.front().keyframe, "contract fixture must start with a keyframe");
    Device device(path);
    Queue output(device.fd, V4L2_BUF_TYPE_VIDEO_OUTPUT), capture(device.fd, V4L2_BUF_TYPE_VIDEO_CAPTURE);
    v4l2_event_subscription subscription{}; subscription.type = V4L2_EVENT_SOURCE_CHANGE;
    checked(device.fd, VIDIOC_SUBSCRIBE_EVENT, &subscription, "contract subscribe SOURCE_CHANGE");
    configure_contract_output(device.fd, output, input, 16);
    require(output.buffers.size() >= 7, "contract preflight requires seven OUTPUT buffers");
    const unsigned continuation_count = 3;
    const unsigned prefix_slots = output.buffers.size() - continuation_count;
    const size_t prefix_limit = input.segments.front().end - continuation_count;
    std::vector<size_t> slot_packet(output.buffers.size(), SIZE_MAX);
    std::vector<bool> completed_packets(input.packets.size(), false);
    std::vector<uint64_t> timestamps(input.packets.size());
    size_t prefix_count = 0;
    auto queue_packet = [&](unsigned slot, size_t index, bool continuation) {
        Packet packet = input.packets[index];
        packet.timestamp = (continuation ? UINT64_C(1) << 40 : 0) + uint64_t(index + 1) * 1000;
        timestamps[index] = packet.timestamp;
        output.queue(slot, &packet);
        slot_packet[slot] = index;
    };
    auto dequeue_prefix = [&]() {
        v4l2_buffer b{};
        while (output.dequeue(b)) {
            size_t index = slot_packet[b.index];
            require(index < prefix_count && !completed_packets[index], "initial OUTPUT identity mismatch");
            completed_packets[index] = true;
        }
    };
    auto feed_prefix = [&]() {
        for (unsigned slot = 0; slot < prefix_slots && prefix_count < prefix_limit; slot++)
            if (!output.buffers[slot].queued) queue_packet(slot, prefix_count++, false);
    };
    feed_prefix();
    output.start();
    decoder_command(device.fd, V4L2_DEC_CMD_STOP, "STOP without CAPTURE must be a successful no-op");
    /* Discovery has no four-AU guarantee: continue source queueing/dequeueing
     * until firmware announces the format. Keep three slots unoccupied so
     * post-STOP admission can be tested while CAPTURE still has no buffers.
     */
    wait_source_change(device.fd, timeout, [&]() { dequeue_prefix(); feed_prefix(); });
    dequeue_prefix();
    /* Include a boundary keyframe in the snapshot, if necessary, so the
     * continuation demonstrably needs the preserved decoder references.
     */
    if (input.packets[prefix_count].keyframe) {
        require(prefix_count < prefix_limit, "fixture ends before a non-IDR contract continuation");
        const auto slot_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout);
        bool queued = false;
        while (!queued) {
            dequeue_prefix();
            for (unsigned slot = 0; slot < prefix_slots; slot++) {
                if (!output.buffers[slot].queued) {
                    queue_packet(slot, prefix_count++, false); queued = true; break;
                }
            }
            require(std::chrono::steady_clock::now() < slot_deadline, "no slot for final pre-STOP keyframe");
            if (!queued) { pollfd pfd{device.fd, POLLOUT, 0}; poll(&pfd, 1, 10); }
        }
    }
    require(!input.packets[prefix_count].keyframe, "contract continuation must start with a non-keyframe AU");
    std::vector<std::string> reference_hashes(prefix_count);
    printf("contract_discovery pre_stop_aus=%zu output_completed=%zu reserved_post_stop_slots=%u\n",
        prefix_count, size_t(std::count(completed_packets.begin(), completed_packets.end(), true)), continuation_count);
    v4l2_format fmt{}; fmt.type = capture.type;
    checked(device.fd, VIDIOC_G_FMT, &fmt, "contract CAPTURE G_FMT");
    require(fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_YUYV && fmt.fmt.pix.field == V4L2_FIELD_NONE,
        "contract requires progressive YUYV");
    const unsigned width = fmt.fmt.pix.width, height = fmt.fmt.pix.height, stride = fmt.fmt.pix.bytesperline;
    require(width && height && stride >= width * 2, "contract invalid geometry");
    capture.allocate(6);
    /* Start without available CAPTURE buffers. This deterministically holds
     * drain open while checking EBUSY and queuing post-STOP OUTPUT buffers.
     */
    capture.start();
    decoder_command(device.fd, V4L2_DEC_CMD_STOP, "queued snapshot STOP");
    expect_drain_busy(device.fd);
    for (unsigned i = 0; i < continuation_count; i++) queue_packet(prefix_slots + i, prefix_count + i, true);
    for (unsigned i = 0; i < capture.buffers.size(); i++) capture.queue(i);
    auto drain_range = [&](size_t begin, size_t end) {
        std::vector<size_t> expected;
        for (size_t i = begin; i < end; i++) expected.push_back(i);
        std::stable_sort(expected.begin(), expected.end(), [&](size_t a, size_t b) {
            return input.packets[a].display_order < input.packets[b].display_order;
        });
        size_t frames = 0, completed = std::count(completed_packets.begin() + begin, completed_packets.begin() + end, true);
        bool last = false;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout);
        while (!last || completed != end - begin) {
            require(std::chrono::steady_clock::now() < deadline, "queued STOP contract timed out");
            v4l2_buffer b{};
            while (output.dequeue(b)) {
                size_t index = slot_packet[b.index];
                require(index >= begin && index < end && !completed_packets[index],
                    "post-STOP OUTPUT was consumed before START or old OUTPUT was consumed twice");
                completed_packets[index] = true;
                completed++;
            }
            while (!last && capture.dequeue(b)) {
                require(b.bytesused <= capture.buffers[b.index].length, "contract CAPTURE extent overflow");
                if (b.bytesused) {
                    require(frames < expected.size(), "STOP snapshot produced a post-boundary picture");
                    uint64_t timestamp = uint64_t(b.timestamp.tv_sec) * 1000000 + b.timestamp.tv_usec;
                    require(timestamp == timestamps[expected[frames]],
                        "STOP snapshot timestamp/display-order mismatch: epoch=" + std::to_string(begin ? 1 : 0) +
                        " frame=" + std::to_string(frames) + " packet=" + std::to_string(expected[frames]) +
                        " expected_us=" + std::to_string(timestamps[expected[frames]]) +
                        " actual_us=" + std::to_string(timestamp));
                    require(b.bytesused >= uint64_t(height - 1) * stride + width * 2,
                        "contract picture is incomplete");
                    AVSHA *sha = av_sha_alloc(); require(sha && !av_sha_init(sha, 256), "contract SHA allocation");
                    for (unsigned y = 0; y < height; y++)
                        av_sha_update(sha, static_cast<unsigned char *>(capture.buffers[b.index].address) + size_t(y) * stride, width * 2);
                    std::string digest = hash(sha); av_free(sha);
                    reference_hashes[expected[frames]] = digest;
                    printf("contract_epoch=%u frame=%zu timestamp_us=%llu sha256=%s\n", begin ? 1 : 0,
                        frames++, (unsigned long long)timestamp, digest.c_str());
                }
                last = b.flags & V4L2_BUF_FLAG_LAST;
                if (!last) capture.queue(b.index);
            }
            pollfd pfd{device.fd, short(POLLIN | POLLOUT | POLLPRI), 0};
            int rc = poll(&pfd, 1, 10);
            require(rc >= 0 || errno == EINTR, "contract decode poll failed");
        }
        require(frames == end - begin, "STOP snapshot frame count mismatch");
        assert_epipe(device.fd);
    };
    drain_range(0, prefix_count);
    const auto hold_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    do {
        v4l2_buffer b{};
        require(!output.dequeue(b), "post-STOP OUTPUT decoded while waiting for START");
        assert_epipe(device.fd);
        pollfd pfd{device.fd, POLLPRI, 0};
        int rc = poll(&pfd, 1, 10);
        require(rc >= 0 || errno == EINTR, "contract hold poll failed");
    } while (std::chrono::steady_clock::now() < hold_until);
    expect_decoder_command_errno(device.fd, V4L2_DEC_CMD_START, EOPNOTSUPP,
        "START after terminal nonempty drain");
    expect_decoder_command_errno(device.fd, V4L2_DEC_CMD_START, EOPNOTSUPP,
        "repeated START after terminal nonempty drain");
    v4l2_buffer held{};
    require(!output.dequeue(held), "post-STOP OUTPUT completed after rejected START");
    for (unsigned i = 0; i < continuation_count; i++)
        require(output.buffers[prefix_slots + i].queued,
            "post-STOP OUTPUT ownership changed after rejected START");
    assert_epipe(device.fd);
    printf("CONTRACT queued_stop_snapshot=%zu post_stop_accepted=%u held_after_rejected_start=1 "
        "drain_busy=1 start_errno=EOPNOTSUPP repeated_start_errno=EOPNOTSUPP\n",
        prefix_count, continuation_count);
    return reference_hashes;
}
static void empty_stop_preflight(int fd, Queue &output, Queue &capture, const Input &input, unsigned timeout)
{
    configure_contract_output(fd, output, input, 8);
    output.start();
    decoder_command(fd, V4L2_DEC_CMD_STOP, "empty STOP without CAPTURE must be a no-op");
    v4l2_format fmt{}; fmt.type = capture.type;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV; fmt.fmt.pix.field = V4L2_FIELD_NONE;
    fmt.fmt.pix.width = input.width; fmt.fmt.pix.height = input.height;
    checked(fd, VIDIOC_S_FMT, &fmt, "empty-drain CAPTURE S_FMT");
    capture.allocate(12); capture.start();
    decoder_command(fd, V4L2_DEC_CMD_STOP, "explicit empty-input STOP");
    expect_drain_busy(fd);
    for (unsigned i = 0; i < capture.buffers.size(); i++) capture.queue(i);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout);
    for (;;) {
        require(std::chrono::steady_clock::now() < deadline, "empty STOP did not return LAST");
        v4l2_buffer b{};
        if (capture.dequeue(b)) {
            require(!b.bytesused && (b.flags & V4L2_BUF_FLAG_LAST), "empty STOP must produce empty LAST");
            break;
        }
        pollfd pfd{fd, POLLIN, 0}; int rc = poll(&pfd, 1, 10);
        require(rc >= 0 || errno == EINTR, "empty-drain poll failed");
    }
    assert_epipe(fd);
    decoder_command(fd, V4L2_DEC_CMD_START, "START after explicit empty drain");
    puts("CONTRACT empty_stop_last=1 after_last_epipe=1 empty_start=1 full_decode_follows=1");
}
static void decode(int fd, Queue &output, Queue &capture, const Input &input,
    unsigned timeout, std::ofstream &raw, const std::string &expected_hash,
    unsigned seek_at, unsigned idle_before_stop_ms, const std::string &busy_device,
    const std::vector<std::string> &expected_segments, bool after_empty_drain,
    const std::vector<std::string> &contract_hashes, LegacyLibrary *legacy,
    const std::string &late_admission_device)
{
    bool restarting = output.streaming;
    bool recovered_terminal_drain = false;
    v4l2_format fmt{};
    if (restarting && !after_empty_drain) {
        expect_decoder_command_errno(fd, V4L2_DEC_CMD_START, EOPNOTSUPP,
            "START after terminal nonempty drain");
        output.stop();
        v4l2_buffer retired{};
        unsigned retired_done = 0, retired_error = 0;
        while (capture.dequeue(retired, true)) {
            require(!(retired.flags & V4L2_BUF_FLAG_LAST),
                "terminal recovery returned a second LAST buffer");
            require(retired.bytesused <= capture.buffers[retired.index].length,
                "terminal recovery CAPTURE extent invalid");
            if (retired.flags & V4L2_BUF_FLAG_ERROR) retired_error++;
            else retired_done++;
        }
        for (unsigned i = 0; i < capture.buffers.size(); i++)
            if (!capture.buffers[i].queued) capture.queue(i);
        printf("terminal_recovery start_errno=EOPNOTSUPP output_streamoff=1 "
            "retired_done=%u retired_error=%u full_decode_follows=1\n",
            retired_done, retired_error);
        restarting = false;
        recovered_terminal_drain = true;
    }
    if (!restarting && output.buffers.empty()) {
        fmt.type = output.type;
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_H264;
        fmt.fmt.pix.width = input.width; fmt.fmt.pix.height = input.height;
        fmt.fmt.pix.sizeimage = std::max(size_t(2 * 1024 * 1024), input.largest);
        fmt.fmt.pix.field = V4L2_FIELD_NONE;
        checked(fd, VIDIOC_S_FMT, &fmt, "S_FMT OUTPUT");
        require(fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_H264, "H264 OUTPUT format unavailable");
        output.allocate(8);
    } else if (restarting) {
        /* An empty drain is resumable without resetting either queue. */
        require(after_empty_drain, "streaming restart must follow an empty drain");
        for (unsigned i = 0; i < capture.buffers.size(); i++)
            if (!capture.buffers[i].queued) capture.queue(i);
    } else {
        require(recovered_terminal_drain && !output.buffers.empty(),
            "stopped OUTPUT queue lacks a terminal-drain recovery fence");
    }
    size_t next = 0, completed = 0, frames = 0;
    bool stopped = false, last = false, source_change = false;
    bool resolution_pending = false;
    unsigned resolution_boundaries = 0;
    bool seek_done = false;
    bool late_admission_done = late_admission_device.empty();
    std::vector<unsigned> held_capture;
    uint64_t timestamp_offset = 0;
    std::string initial_frame_hash;
    const std::streampos raw_start = raw.is_open() ? raw.tellp() : std::streampos(0);
    auto stop_ready = std::chrono::steady_clock::time_point::max();
    std::map<uint64_t, unsigned> expected_timestamps;
    auto reset_timestamps = [&]() {
        expected_timestamps.clear();
        for (const auto &p : input.packets) expected_timestamps[p.timestamp + timestamp_offset]++;
    };
    reset_timestamps();
    std::vector<const Packet *> display_order;
    for (const auto &p : input.packets) display_order.push_back(&p);
    std::stable_sort(display_order.begin(), display_order.end(), [](const Packet *a, const Packet *b) {
        return a->display_order < b->display_order;
    });
    auto feed = [&]() {
        for (unsigned i = 0; i < output.buffers.size() && next < input.packets.size(); i++)
            if (!output.buffers[i].queued) output.queue(i, &input.packets[next++], timestamp_offset);
    };
    auto free_sha = [](AVSHA *p) { av_free(p); };
    std::unique_ptr<AVSHA, decltype(free_sha)> aggregate(av_sha_alloc(), free_sha);
    require(bool(aggregate) && av_sha_init(aggregate.get(), 256) == 0, "SHA allocation");
    std::unique_ptr<AVSHA, decltype(free_sha)> segment_sha(av_sha_alloc(), free_sha);
    require(bool(segment_sha) && av_sha_init(segment_sha.get(), 256) == 0, "segment SHA allocation");
    size_t segment_index = 0;
    std::map<std::string, std::string> prior_segment_hashes;
    feed();
    if (!restarting) output.start();
    if (legacy) check_legacy_blocked(*legacy);
    if (!busy_device.empty()) check_busy(busy_device, input);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout);
    unsigned width = 0, height = 0, stride = 0;
    auto configure_capture = [&](bool reallocate) {
        if (reallocate) capture.release();
        fmt = {}; fmt.type = capture.type;
        checked(fd, VIDIOC_G_FMT, &fmt, "G_FMT CAPTURE after source change");
        require(fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_YUYV && fmt.fmt.pix.field == V4L2_FIELD_NONE,
            "expected progressive YUYV CAPTURE");
        width = fmt.fmt.pix.width; height = fmt.fmt.pix.height; stride = fmt.fmt.pix.bytesperline;
        require(width && height && stride >= width * 2, "invalid CAPTURE geometry");
        printf("format width=%u height=%u stride=%u sizeimage=%u output_streaming=%u\n",
            width, height, stride, fmt.fmt.pix.sizeimage, output.streaming ? 1 : 0);
        if (!capture.streaming) {
            capture.allocate(12);
            for (unsigned i = 0; i < capture.buffers.size(); i++) capture.queue(i);
            capture.start();
        }
        require(late_admission_done || input.packets.size() >
            capture.buffers.size() + output.buffers.size() + 4,
            "late admission requires more pictures than CAPTURE/OUTPUT buffers plus four");
    };
    if (restarting && !after_empty_drain) configure_capture(false);
    auto receive_events = [&]() {
        v4l2_event event{};
        while (call(fd, VIDIOC_DQEVENT, &event) == 0) {
            if (event.type != V4L2_EVENT_SOURCE_CHANGE) continue;
            require(event.u.src_change.changes & V4L2_EVENT_SRC_CH_RESOLUTION, "unknown source-change event");
            if (!source_change) {
                source_change = true;
                configure_capture(false);
            } else {
                require(!resolution_pending, "second source change before old CAPTURE boundary");
                resolution_pending = true;
                /* G_FMT already describes the new stream, but all pictures
                 * through boundary LAST still use the old capture geometry.
                 */
            }
        }
        require(errno == EAGAIN || errno == ENOENT,
            "DQEVENT failed: " + std::string(strerror(errno)));
    };
    while (!last || completed != input.packets.size()) {
        require(std::chrono::steady_clock::now() < deadline, "decode timed out before LAST and OUTPUT completion");
        pollfd pfd{fd, short(POLLIN | POLLOUT | POLLPRI), 0};
        int polled = poll(&pfd, 1, 20);
        if (polled < 0 && errno == EINTR) continue;
        require(polled >= 0 && !(pfd.revents & (POLLHUP | POLLNVAL)), "poll device failure");
        /* __v4l2_event_dequeue returns ENOENT when fh->available is empty;
         * unlike VIDIOC_DQBUF, this is its normal nonblocking empty result.
         */
        receive_events();
        v4l2_buffer b{};
        while (output.dequeue(b)) completed++;
        feed();
        if (!stopped && completed == input.packets.size() && (!seek_at || seek_done)) {
            if (stop_ready == std::chrono::steady_clock::time_point::max())
                stop_ready = std::chrono::steady_clock::now();
            if (std::chrono::steady_clock::now() - stop_ready >= std::chrono::milliseconds(idle_before_stop_ms)) {
                v4l2_decoder_cmd cmd{}; cmd.cmd = V4L2_DEC_CMD_STOP;
                checked(fd, VIDIOC_DECODER_CMD, &cmd, "DECODER STOP"); stopped = true;
            }
        }
        while (capture.streaming && !last && capture.dequeue(b)) {
            auto &mapping = capture.buffers[b.index];
            require(b.bytesused <= mapping.length, "CAPTURE bytesused exceeds allocation");
            if (b.bytesused) {
                require(segment_index < input.segments.size(), "extra picture after final segment");
                const Segment &segment = input.segments[segment_index];
                require(width == segment.width && height == segment.height,
                    "CAPTURE geometry does not match the expected fixture segment");
                require(b.bytesused >= uint64_t(height - 1) * stride + width * 2,
                    "CAPTURE bytesused smaller than visible picture");
                uint64_t ts = uint64_t(b.timestamp.tv_sec) * 1000000 + b.timestamp.tv_usec;
                auto expected = expected_timestamps.find(ts);
                const std::string timestamp_detail = " frame=" + std::to_string(frames) +
                    " segment=" + std::to_string(segment_index) +
                    " expected_us=" + (frames < display_order.size() ?
                        std::to_string(display_order[frames]->timestamp + timestamp_offset) : "none") +
                    " actual_us=" + std::to_string(ts);
                require(expected != expected_timestamps.end() && expected->second,
                    "CAPTURE timestamp absent from submitted timestamps:" + timestamp_detail);
                require(frames < display_order.size() && ts == display_order[frames]->timestamp + timestamp_offset,
                    "CAPTURE timestamp does not match picture display order:" + timestamp_detail);
                expected->second--;
                std::unique_ptr<AVSHA, decltype(free_sha)> frame_sha(av_sha_alloc(), free_sha);
                require(bool(frame_sha) && av_sha_init(frame_sha.get(), 256) == 0, "frame SHA allocation");
                for (unsigned y = 0; y < height; y++) {
                    const auto *row = static_cast<unsigned char *>(mapping.address) + size_t(y) * stride;
                    av_sha_update(aggregate.get(), row, width * 2);
                    av_sha_update(segment_sha.get(), row, width * 2);
                    av_sha_update(frame_sha.get(), row, width * 2);
                    if (raw.is_open()) raw.write(reinterpret_cast<const char *>(row), width * 2);
                }
                std::string frame_digest = hash(frame_sha.get());
                size_t packet_index = size_t(display_order[frames] - input.packets.data());
                if (packet_index < contract_hashes.size())
                    require(frame_digest == contract_hashes[packet_index],
                        "drained prefix pixels differ from uninterrupted full decode");
                if (!frames) {
                    if (seek_done) require(frame_digest == initial_frame_hash, "seek first frame differs from original first frame");
                    else initial_frame_hash = frame_digest;
                }
                printf("epoch=%u frame=%zu timestamp_us=%llu bytes=%u sha256=%s\n", seek_done ? 1 : 0, frames++,
                    (unsigned long long)ts, width * height * 2, frame_digest.c_str());
                require(!raw.is_open() || bool(raw), "raw output write failed");
                if (frames == segment.end) {
                    std::string segment_digest = hash(segment_sha.get());
                    if (!expected_segments.empty())
                        require(segment_digest == expected_segments[segment_index], "segment YUYV SHA256 mismatch");
                    auto prior = prior_segment_hashes.find(segment.path);
                    if (prior != prior_segment_hashes.end())
                        require(segment_digest == prior->second, "repeated fixture segment hash changed");
                    prior_segment_hashes[segment.path] = segment_digest;
                    printf("segment=%zu frames=%zu geometry=%ux%u yuyv_sha256=%s\n", segment_index,
                        segment.end - segment.begin, segment.width, segment.height, segment_digest.c_str());
                    segment_index++;
                    require(av_sha_init(segment_sha.get(), 256) == 0, "reset segment SHA");
                }
            }
            last = b.flags & V4L2_BUF_FLAG_LAST;
            if (last) receive_events(); /* Event may have arrived after this loop's first peek. */
            if (last && resolution_pending) {
                require(output.streaming, "OUTPUT stopped during dynamic resolution change");
                require((restarting && frames == 0) ||
                    (segment_index && segment_index < input.segments.size() &&
                     frames == input.segments[segment_index - 1].end),
                    "resolution LAST did not follow all old-segment pictures");
                v4l2_buffer boundary{}; boundary.type = capture.type; boundary.memory = V4L2_MEMORY_MMAP;
                require(call(fd, VIDIOC_DQBUF, &boundary) < 0 && errno == EPIPE,
                    "DQBUF after resolution LAST must return EPIPE");
                if (frames) resolution_boundaries++;
                configure_capture(true);
                resolution_pending = false;
                last = false;
                break;
            }
            require(!last || stopped, "premature LAST without DECODER STOP (idle is not EOS)");
            if (!last && !late_admission_done) {
                held_capture.push_back(b.index);
                if (held_capture.size() == capture.buffers.size()) {
                    /* No CAPTURE backing remains queued. Preserve queued AUs
                     * while attempting admission on distinct file handles.
                     */
                    v4l2_buffer out{};
                    while (output.dequeue(out)) completed++;
                    feed();
                    require(!stopped && completed < input.packets.size() &&
                        std::any_of(output.buffers.begin(), output.buffers.end(),
                            [](const Queue::Mapping &m) { return m.queued; }),
                        "late admission must run with pending OUTPUT and CAPTURE backpressure");
                    check_busy(late_admission_device, input);
                    if (legacy) check_legacy_blocked(*legacy);
                    printf("CONTRACT late_admission=1 capture_held=%zu pending_output=1 native_busy=1 legacy_busy=%u\n",
                        held_capture.size(), legacy ? 1 : 0);
                    late_admission_done = true;
                    for (unsigned index : held_capture) capture.queue(index);
                    held_capture.clear();
                }
            } else if (!last) capture.queue(b.index);
            if (seek_at && !seek_done && frames >= seek_at) break;
        }
        if (seek_at && !seek_done && frames >= seek_at) {
            require(!stopped && !last && capture.streaming, "seek must interrupt a live undrained stream");
            output.stop();
            /* Successful OUTPUT STREAMOFF is the retirement fence. Only here
             * may pre-reset DONE and ERROR buffers be discarded explicitly.
             */
            unsigned retired_done = 0, retired_error = 0;
            while (capture.dequeue(b, true)) {
                require(!(b.flags & V4L2_BUF_FLAG_LAST), "old epoch produced LAST without STOP");
                require(b.bytesused <= capture.buffers[b.index].length, "retired CAPTURE extent invalid");
                if (b.flags & V4L2_BUF_FLAG_ERROR) retired_error++;
                else retired_done++;
            }
            for (unsigned i = 0; i < capture.buffers.size(); i++)
                if (!capture.buffers[i].queued) capture.queue(i);
            printf("seek discarded_done=%u discarded_error=%u output_streamoff=1 capture_streaming=1\n",
                retired_done, retired_error);
            seek_done = true;
            timestamp_offset = UINT64_C(1) << 40;
            next = completed = frames = 0;
            source_change = false;
            reset_timestamps();
            require(av_sha_init(aggregate.get(), 256) == 0, "reset seek SHA");
            require(av_sha_init(segment_sha.get(), 256) == 0, "reset seek segment SHA");
            if (raw.is_open()) { raw.seekp(raw_start); require(bool(raw), "reset raw output position"); }
            deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout);
            feed(); output.start();
        }
    }
    require(source_change && frames, "no decoded frames/source change");
    require(late_admission_done, "late admission backpressure check was not reached");
    require(frames == input.packets.size(), "frame count differs from access-unit count");
    require(resolution_boundaries == input.segments.size() - 1,
        "missing dynamic resolution boundary handshakes");
    for (const auto &entry : expected_timestamps) require(!entry.second, "missing CAPTURE timestamps");
    v4l2_buffer after_last{};
    after_last.type = capture.type; after_last.memory = V4L2_MEMORY_MMAP;
    require(call(fd, VIDIOC_DQBUF, &after_last) < 0 && errno == EPIPE,
        "CAPTURE DQBUF after LAST must return EPIPE");
    require(!seek_at || seek_done, "requested seek was never executed");
    std::string digest = hash(aggregate.get());
    require(expected_hash.empty() || digest == expected_hash, "aggregate YUYV SHA256 mismatch");
    printf("PASS frames=%zu output_completed=%zu last=1 after_last_epipe=1 seek=%u dynamic_boundaries=%u idle_before_stop_ms=%u yuyv_sha256=%s\n",
        frames, completed, seek_done ? 1 : 0, resolution_boundaries, idle_before_stop_ms, digest.c_str());
}

int main(int argc, char **argv)
{
    try {
        std::string path, input_path, raw_path, expected_hash;
        std::vector<std::string> next_inputs, expected_segments;
        unsigned timeout = 60, repeat = 1; bool stress = false, demux_only = false, busy = false;
        bool contracts = false, empty_stop = false, failed_streamon = false, late_admission = false;
        bool legacy_arbitration = false;
        unsigned seek_at = 0, idle_before_stop_ms = 250, legacy_prefix = 0;
        for (int i = 1; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--help") {
                puts("usage: v4l2-decode --input fixture.mp4 [--device /dev/videoN] [--timeout 60]\n"
                     "       [--raw frames.yuyv] [--timestamps source|stress] [--repeat N] [--expect-sha256 HEX]\n"
                     "       [--demux-only] (validate fixture without opening any device)\n"
                     "       [--seek-at N --expect-sha256 HEX] [--idle-before-stop-ms 250]\n"
                     "       [--check-busy] (second native session must fail STREAMON with EBUSY)\n"
                     "       [--next-input fixture.mp4 ...] [--expect-segment-sha256 HEX ...]\n"
                     "       [--contract-preflight|--stop-queued] (STOP boundary/empty-drain contracts)\n"
                     "       [--empty-stop] (standalone empty STOP/LAST/EPIPE/START, then full decode)\n"
                     "       [--failed-streamon] (no REQBUFS failure must release its new session lease)\n"
                     "       [--late-admission] (native busy under CAPTURE backpressure; legacy with --legacy-arbitration)\n"
                     "       [--legacy-arbitration|--check-legacy-busy] (installed libcrystalhd, both directions)\n"
                     "       [--legacy-contract N] (legacy-only: drain N AUs, resume next three without reset)\n"
                     "Fixtures must contain exactly one progressive H.264 picture per demuxed AU.\n"
                     "Hashes cover tightly packed visible YUYV rows in display order; --repeat tests rejected START and\n"
                     "OUTPUT STREAMOFF recovery after each terminal nonempty drain.");
                return 0;
            }
            if (arg == "--demux-only") { demux_only = true; continue; }
            if (arg == "--check-busy") { busy = true; continue; }
            if (arg == "--empty-stop") { empty_stop = true; continue; }
            if (arg == "--failed-streamon") { failed_streamon = true; continue; }
            if (arg == "--late-admission") { late_admission = true; continue; }
            if (arg == "--contract-preflight" || arg == "--stop-queued") { contracts = true; continue; }
            if (arg == "--legacy-arbitration" || arg == "--check-legacy-busy") { legacy_arbitration = true; continue; }
            require(i + 1 < argc, "missing argument for " + arg);
            std::string value = argv[++i];
            if (arg == "--device") path = value;
            else if (arg == "--input") input_path = value;
            else if (arg == "--next-input") next_inputs.push_back(value);
            else if (arg == "--expect-segment-sha256") expected_segments.push_back(value);
            else if (arg == "--raw") raw_path = value;
            else if (arg == "--expect-sha256") expected_hash = value;
            else if (arg == "--timeout") timeout = std::stoul(value);
            else if (arg == "--repeat") repeat = std::stoul(value);
            else if (arg == "--seek-at") seek_at = std::stoul(value);
            else if (arg == "--legacy-contract") { legacy_prefix = std::stoul(value); require(legacy_prefix, "legacy prefix must be positive"); }
            else if (arg == "--idle-before-stop-ms") idle_before_stop_ms = std::stoul(value);
            else if (arg == "--timestamps") {
                require(value == "source" || value == "stress", "invalid timestamp mode"); stress = value == "stress";
            } else throw std::runtime_error("unknown option " + arg);
        }
        require(!input_path.empty() && timeout && timeout <= 3600 && repeat && repeat <= 100,
            "provide --input and valid timeout/repeat bounds");
        Input input = demux(input_path, stress);
        require(next_inputs.size() <= 8, "at most eight --next-input segments are supported");
        require(next_inputs.empty() || (!seek_at && repeat == 1),
            "dynamic fixture sequence must use repeat=1 and cannot combine seek");
        for (const auto &next_input : next_inputs) append_input(input, demux(next_input, stress));
        require(expected_segments.empty() || expected_segments.size() == input.segments.size(),
            "provide one --expect-segment-sha256 per fixture segment");
        require(seek_at < input.packets.size() && idle_before_stop_ms <= 5000,
            "seek-at must precede fixture end; idle-before-stop-ms must be <=5000");
        require(!seek_at || !expected_hash.empty(), "--seek-at requires --expect-sha256 for full restarted-epoch validation");
        require(!late_admission || (!seek_at && next_inputs.empty()),
            "--late-admission requires a single fixture without seek");
        printf("input=%s access_units=%zu width=%u height=%u largest_au=%zu timestamp_mode=%s\n",
            input_path.c_str(), input.packets.size(), input.width, input.height,
            input.largest, stress ? "stress" : "source");
        for (size_t i = 0; i < input.segments.size(); i++) {
            const auto &segment = input.segments[i];
            printf("input_segment=%zu frames=%zu geometry=%ux%u\n", i,
                segment.end - segment.begin, segment.width, segment.height);
        }
        if (demux_only) { puts("DEMUX_ONLY: no hardware decode performed"); return 0; }
        if (legacy_prefix) {
            require(next_inputs.empty() && !contracts && !empty_stop && !failed_streamon && !late_admission &&
                !legacy_arbitration && !busy && !seek_at && repeat == 1 &&
                raw_path.empty() && expected_hash.empty() && expected_segments.empty(),
                "--legacy-contract is a separate legacy-only mode; native options/hashes are not applied");
            legacy_contract(input, legacy_prefix, timeout);
            return 0;
        }
        if (path.empty()) path = discover();
        std::unique_ptr<LegacyLibrary> legacy;
        if (legacy_arbitration) {
            legacy = std::make_unique<LegacyLibrary>();
            check_legacy_owner(path, input, *legacy);
        }
        std::vector<std::string> contract_hashes;
        if (contracts) contract_hashes = queued_stop_preflight(path, input, timeout);
        std::unique_ptr<Device> failed_device;
        if (failed_streamon) failed_device = failed_streamon_preflight(path);
        Device device(path);
        v4l2_capability caps{};
        checked(device.fd, VIDIOC_QUERYCAP, &caps, "QUERYCAP");
        unsigned cap = caps.capabilities & V4L2_CAP_DEVICE_CAPS ? caps.device_caps : caps.capabilities;
        require((cap & (V4L2_CAP_VIDEO_M2M | V4L2_CAP_STREAMING)) ==
            (V4L2_CAP_VIDEO_M2M | V4L2_CAP_STREAMING), "single-plane streaming M2M capability required");
        printf("device=%s driver=%s card=%s packets=%zu\n", path.c_str(), caps.driver, caps.card, input.packets.size());
        v4l2_event_subscription subscription{}; subscription.type = V4L2_EVENT_SOURCE_CHANGE;
        checked(device.fd, VIDIOC_SUBSCRIBE_EVENT, &subscription, "subscribe SOURCE_CHANGE");
        std::ofstream raw;
        if (!raw_path.empty()) { raw.open(raw_path, std::ios::binary); require(bool(raw), "open raw output failed"); }
        Queue output(device.fd, V4L2_BUF_TYPE_VIDEO_OUTPUT);
        Queue capture(device.fd, V4L2_BUF_TYPE_VIDEO_CAPTURE);
        if (contracts || empty_stop) empty_stop_preflight(device.fd, output, capture, input, timeout);
        for (unsigned i = 0; i < repeat; i++) {
            printf("iteration=%u\n", i);
            decode(device.fd, output, capture, input, timeout, raw, expected_hash,
                seek_at, idle_before_stop_ms, busy ? path : std::string(), expected_segments,
                (contracts || empty_stop) && !i, contract_hashes, legacy.get(),
                late_admission ? path : std::string());
        }
        return 0;
    } catch (const std::exception &e) {
        fprintf(stderr, "FAIL: %s\n", e.what()); return 1;
    }
}
