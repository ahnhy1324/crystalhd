// SPDX-License-Identifier: LGPL-2.1-or-later
// Actual FLEA DIVX public input/PES/ring paths, without a decoder or firmware.
// VOP prefixes and the 47-byte VOL metadata come from a locally generated
// progressive MPEG-4 Simple level-5 640x360 testsrc2 fixture (FFmpeg mpeg4).
// Picture bodies below are synthetic framing data, NOT decoder conformance.
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <pthread.h>
#include <vector>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "7411d.h"
#include "libcrystalhd_if.h"
#include "libcrystalhd_int_if.h"
#include "libcrystalhd_priv.h"
#include "libcrystalhd_fwcmds.h"

extern bc_dil_glob_s *bc_dil_glob_ptr;
using Bytes = std::vector<uint8_t>;
static unsigned checks, failures, groups;
static std::vector<Bytes> packets;
static void check(bool value, const char *message)
{
    ++checks;
    if (!value) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
extern "C" BC_STATUS __real_txBufPush(pTXBUFFER, uint8_t *, uint32_t);
extern "C" BC_STATUS __wrap_txBufPush(pTXBUFFER ring, uint8_t *bytes, uint32_t size)
{
    packets.emplace_back(bytes, bytes + size);
    return __real_txBufPush(ring, bytes, size);
}
extern "C" int __wrap_ioctl(int, unsigned long, ...) { std::abort(); }
extern "C" int __wrap_usleep(useconds_t) { std::abort(); }
BC_STATUS DtsFWDecFlushChannel(HANDLE, uint32_t) { std::abort(); }
BC_STATUS DtsFWPauseVideo(HANDLE, uint32_t) { std::abort(); }
BC_STATUS DtsFWCloseChannel(HANDLE, uint32_t) { std::abort(); }
BC_STATUS DtsFWStopVideo(HANDLE, uint32_t, bool) { std::abort(); }
BC_STATUS DtsSetCoreClock(HANDLE, uint32_t) { std::abort(); }
BC_STATUS DtsPushFwToFlea(HANDLE, char *) { std::abort(); }
BC_STATUS DtsPushAuthFwToLink(HANDLE, char *) { std::abort(); }
BC_STATUS DtsFWInitialize(HANDLE, uint32_t) { std::abort(); }
BC_STATUS DtsFWActivateDecoder(HANDLE) { std::abort(); }
BC_STATUS DtsFWStartVideo(HANDLE, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { std::abort(); }
BC_STATUS DtsFWOpenChannel(HANDLE, uint32_t, uint32_t) { std::abort(); }
BC_STATUS DtsFWSetVideoInput(HANDLE) { std::abort(); }
BC_STATUS DtsSetProgressive(HANDLE, uint32_t) { std::abort(); }
BC_STATUS DtsSetVideoClock(HANDLE, uint32_t) { std::abort(); }
BC_STATUS DtsSetTSMode(HANDLE, uint32_t) { std::abort(); }
void DumpInputSampleToFile(uint8_t *, uint32_t) { std::abort(); }

static const Bytes metadata = {
    0,0,1,0xb0,5,0,0,1,0xb5,0x89,0x13,0,0,1,0,
    0,0,1,0x20,0,0xc4,0x8d,0x88,0,0xf5,0x14,4,0x2d,0x14,0x63,
    0,0,1,0xb2,'L','a','v','c','6','1','.','1','9','.','1','0','1'
};

struct Fixture {
    bc_dil_glob_s globals = {};
    DTS_LIB_CONTEXT context = {};
    bool configured = false;
    explicit Fixture(bool headers, bool h263 = false)
    {
        bc_dil_glob_ptr = &globals;
        context.Sig = LIB_CTX_SIG;
        context.ProcessID = getpid();
        context.DevId = BC_PCI_DEVID_FLEA;
        context.State = BC_DEC_STATE_START;
        pthread_mutexattr_t attr;
        if (pthread_mutexattr_init(&attr) ||
            pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) ||
            pthread_mutex_init(&context.thLock, &attr)) std::abort();
        pthread_mutexattr_destroy(&attr);
        context.alignBuf = static_cast<uint8_t *>(std::malloc(ALIGN_BUF_SIZE));
        if (!context.alignBuf || txBufInit(&context.circBuf, CIRC_TX_BUF_SIZE) != BC_STS_SUCCESS)
            std::abort();
        Bytes original = headers ? metadata : Bytes{};
        BC_INPUT_FORMAT format = {};
        format.mSubtype = BC_MSUBTYPE_DIVX;
        format.width = h263 ? 128 : 640; format.height = h263 ? 96 : 360;
        format.Progressive = true;
        format.startCodeSz = h263 ? 0 : 4;
        format.OptFlags = 0x80000000U | (h263 ? vdecFrameRate25 : vdecFrameRate59_94) | 0x40U;
        format.pMetaData = original.empty() ? nullptr : original.data();
        format.metaDataSz = original.size();
        configured = DtsSetInputFormat(&context, &format) == BC_STS_SUCCESS;
        check(configured, h263 ? "public DIVX configuration accepts empty fixture metadata" :
                               "public DIVX configuration accepts original MPEG-4 metadata");
        if (configured) {
            check(context.VidParams.VideoAlgo == BC_VID_ALGO_DIVX &&
                  context.VidParams.StreamType == BC_STREAM_TYPE_PES &&
                  !context.SingleThreadedAppMode && context.PESConvParams.m_bSoftRave &&
                  !context.PESConvParams.m_bIsAdd_SCode_CodeIn,
                  "FLEA selects real PES/SoftRave path, not H264 start-code recognition");
            if (headers) {
                check(context.VidParams.pMetaData != original.data() &&
                      context.VidParams.MetaDataSz == metadata.size() &&
                      !std::memcmp(context.VidParams.pMetaData, metadata.data(), metadata.size()),
                      "input format owns exact original metadata");
                std::fill(original.begin(), original.end(), 0xcc);
            }
        }
        packets.clear();
    }
    ~Fixture()
    {
        DtsReleasePESConverter(&context);
        std::free(context.VidParams.pMetaData);
        std::free(context.alignBuf);
        txBufFree(&context.circBuf);
        pthread_mutex_destroy(&context.thLock);
        bc_dil_glob_ptr = nullptr;
    }
    bool send(Bytes input, uint64_t timestamp, bool h263 = false)
    {
        const uint32_t size = input.size();
        input.shrink_to_fit();
        const bool accepted = DtsProcInput(&context, input.data(), size, timestamp, false) == BC_STS_SUCCESS;
        check(accepted, h263 ? "actual DtsProcInput accepts bounded H263 fixture bytes" :
                              "actual DtsProcInput accepts bounded VOP data");
        std::fill(input.begin(), input.end(), 0xdd); // Ring must own its copy.
        return accepted;
    }
    Bytes drain()
    {
        static const uint32_t sizes[] = {1, 3, 13, 127, 4093, 65537, 7, 32768};
        Bytes result;
        unsigned n = 0;
        while (context.circBuf.busySize) {
            const uint32_t size = std::min(context.circBuf.busySize, sizes[n++ % 8]);
            Bytes part(size);
            const BC_STATUS status = txBufPop(&context.circBuf, part.data(), size);
            check(status == BC_STS_SUCCESS, "real ring pops arbitrary PES-splitting chunks");
            if (status != BC_STS_SUCCESS) break;
            result.insert(result.end(), part.begin(), part.end());
        }
        check(context.circBuf.busySize == 0 && DtsTxFreeSize(&context) == CIRC_TX_BUF_SIZE,
              "drained ring has exact empty/free accounting");
        return result;
    }
    void prime_wrap()
    {
        Bytes padding(CIRC_TX_BUF_SIZE - 31, 0xa5);
        check(__real_txBufPush(&context.circBuf, padding.data(), padding.size()) == BC_STS_SUCCESS,
              "prime real ring near wrap without changing its pointers directly");
        check(drain() == padding, "ring priming bytes are exact");
        check(context.circBuf.writePointer == CIRC_TX_BUF_SIZE - 31,
              "metadata/PES body will cross actual ring boundary");
    }
};

struct Pes { bool has_pts = false; uint64_t pts = 0; Bytes payload; };
static bool parse(const Bytes &bytes, std::vector<Pes> *out)
{
    // Independent MPEG-2 PES length/flags/marker parser; never calls the
    // production PTS encoder, start-code recognizer, or converter.
    for (size_t at = 0; at < bytes.size();) {
        if (bytes.size() - at < 9) return false;
        const uint8_t *p = bytes.data() + at;
        const size_t length = (static_cast<unsigned>(p[4]) << 8) | p[5];
        if (p[0] || p[1] || p[2] != 1 || p[3] != 0xe0 || (p[6] & 0xc0) != 0x80 ||
            length < 3 || length + 6 > bytes.size() - at || p[8] > length - 3) return false;
        Pes next;
        next.has_pts = (p[7] & 0xc0) == 0x80;
        if (next.has_pts) {
            if (p[8] < 5 || (p[9] & 0xf1) != 0x21 || !(p[11] & 1) || !(p[13] & 1)) return false;
            next.pts = (static_cast<uint64_t>((p[9] >> 1) & 7) << 30) |
                (static_cast<uint64_t>(p[10]) << 22) |
                (static_cast<uint64_t>(p[11] >> 1) << 15) |
                (static_cast<uint64_t>(p[12]) << 7) | (p[13] >> 1);
        } else if (p[7] & 0xc0) return false;
        next.payload.assign(p + 9 + p[8], p + 6 + length);
        out->push_back(std::move(next));
        at += 6 + length;
    }
    return true;
}
static Bytes queued_bytes()
{
    Bytes result;
    for (const auto &p : packets) result.insert(result.end(), p.begin(), p.end());
    return result;
}

static bool fixture_fits(size_t size)
{
    const size_t max_file = 64U * 1024U * 1024U;
    const size_t payload_per_pes = 0xfff0U - 3U - 5U;
    if (!size || size > max_file) return false;
    // Conservatively reserve a PTS-bearing header for every fragment and the
    // existing whole-call EOS reserve. No TX worker drains this test's ring.
    const size_t fragments = (size + payload_per_pes - 1) / payload_per_pes;
    return size + fragments * 14U + 1024U <= CIRC_TX_BUF_SIZE;
}

struct LocalFile {
    int pin = -1, data = -1;
    ~LocalFile() { if (data >= 0) close(data); if (pin >= 0) close(pin); }
};
static bool read_fixture(const char *path, Bytes *bytes)
{
    LocalFile file;
    // O_PATH does not invoke a device's open callback. Reject nonregular files
    // before reopening the pinned inode for data, including a final symlink.
    file.pin = open(path, O_PATH | O_NOFOLLOW | O_CLOEXEC);
    struct stat pinned = {}, opened = {}, final = {};
    if (file.pin < 0 || fstat(file.pin, &pinned) || !S_ISREG(pinned.st_mode) ||
        pinned.st_size <= 0 || pinned.st_size > 64LL * 1024 * 1024 ||
        !fixture_fits(static_cast<size_t>(pinned.st_size))) return false;
    char proc_path[64];
    const int n = std::snprintf(proc_path, sizeof proc_path, "/proc/self/fd/%d", file.pin);
    if (n < 0 || static_cast<size_t>(n) >= sizeof proc_path) return false;
    file.data = open(proc_path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (file.data < 0 || fstat(file.data, &opened) || !S_ISREG(opened.st_mode) ||
        opened.st_dev != pinned.st_dev || opened.st_ino != pinned.st_ino ||
        opened.st_size != pinned.st_size) return false;
    bytes->resize(static_cast<size_t>(pinned.st_size));
    size_t at = 0;
    while (at < bytes->size()) {
        const ssize_t got = read(file.data, bytes->data() + at, bytes->size() - at);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return false;
        at += static_cast<size_t>(got);
    }
    uint8_t extra;
    ssize_t got;
    do { got = read(file.data, &extra, 1); } while (got < 0 && errno == EINTR);
    if (got != 0 || fstat(file.data, &final) || final.st_dev != pinned.st_dev ||
        final.st_ino != pinned.st_ino || final.st_size != pinned.st_size) return false;
    // Baseline H.263's 22-bit picture start code, not a MPEG-4 VOP prefix.
    return bytes->size() >= 3 && (*bytes)[0] == 0 && (*bytes)[1] == 0 &&
        ((*bytes)[2] & 0xfc) == 0x80;
}

static Bytes divx_eos_wire()
{
    const Bytes timestamped_end = {0,0,1,0xe0,0,16,0x81,0x80,5,0x21,0,1,0,1,
                                   0,0,1,0xb1,0,0,1,0xb1};
    const Bytes plain_end = {0,0,1,0xe0,0,11,0x81,0,0,0,0,1,0xb1,0,0,1,0xb1};
    Bytes marker(184, 0xff);
    const uint8_t header[] = {0,0,1,0xe0,0,178,0x81,1,20,0x80,'B','R','C','M'};
    std::memcpy(marker.data(), header, sizeof header);
    std::memset(marker.data() + 14, 0, 12);
    uint8_t *body = marker.data() + 29;
    std::memset(body, 0, 13); body[4] = 0x0c;
    body[13] = body[14] = 0xff; body[15] = 0; body[16] = 1;
    std::memset(body + 29, 0, 7); body[36] = 0xbc;
    Bytes result = timestamped_end;
    result.insert(result.end(), marker.begin(), marker.end());
    result.insert(result.end(), plain_end.begin(), plain_end.end());
    result.insert(result.end(), plain_end.begin(), plain_end.end());
    return result;
}

static void H263Fixture(const Bytes &original, bool wrap)
{
    ++groups;
    Fixture f(false, true);
    if (!f.configured) return;
    check(BC_VID_ALGO_DIVX == 6 && f.context.VidParams.WidthInPixels == 128 &&
          f.context.VidParams.HeightInPixels == 96 && !f.context.VidParams.pMetaData &&
          !f.context.VidParams.MetaDataSz && !f.context.PESConvParams.m_iSpsPpsLen,
          "fixture uses algorithm 6 and original geometry with no injected metadata");
    if (wrap) f.prime_wrap();
    const uint64_t timestamp = wrap ? 2800000 : 100000;
    if (!f.send(original, timestamp, true)) return;
    const Bytes wire = f.drain();
    check(wire == queued_bytes(), "actual queued fixture bytes survive caller mutation and ring wrap");
    std::vector<Pes> parsed;
    const bool valid = parse(wire, &parsed);
    const size_t first_capacity = 0xfff0U - 3U - 5U, continuation_capacity = 0xfff0U - 3U;
    const size_t expected_packets = original.size() <= first_capacity ? 1 :
        1 + (original.size() - first_capacity + continuation_capacity - 1) / continuation_capacity;
    check(valid && parsed.size() == expected_packets,
          "fixture produces exactly the independently expected PES fragment count");
    if (!valid || parsed.empty()) return;
    Bytes joined;
    for (size_t i = 0; i < parsed.size(); ++i) {
        check(!parsed[i].payload.empty(), "fixture fragments contain data, not invented empty packets");
        check(i ? !parsed[i].has_pts : parsed[i].has_pts && parsed[i].pts == timestamp / 10000,
              "only first fixture PES fragment retains the exact scaled timestamp");
        joined.insert(joined.end(), parsed[i].payload.begin(), parsed[i].payload.end());
    }
    check(joined == original, "all real H263 bytes reconstruct exactly without start-code insertion");
    check(!f.context.PESConvParams.m_bAddSpsPps && f.context.PESConvParams.m_bSoftRave &&
          !f.context.PESConvParams.m_bIsAdd_SCode_CodeIn,
          "ordinary fixture input leaves the configured PES path intact");
    packets.clear();
    const bool eos_ok = DtsFlushInput(&f.context, 0) == BC_STS_SUCCESS;
    check(eos_ok, "actual public drain queues the ordinary DIVX EOS control sequence");
    if (!eos_ok) return;
    const Bytes eos = f.drain();
    parsed.clear();
    check(eos == queued_bytes() && eos == divx_eos_wire() && eos.size() < 1024,
          "host EOS retains the exact four-packet oracle within its reservation");
    check(parse(eos, &parsed) && parsed.size() == 4,
          "independent PES parser reconstructs all four host EOS packets");
    check(f.context.PESConvParams.m_bSoftRave && !f.context.PESConvParams.m_bPESPrivData &&
          !f.context.PESConvParams.m_pPESPrivData && !f.context.PESConvParams.m_bPESExtField &&
          !f.context.PESConvParams.m_pPESExtField && !f.context.PESConvParams.m_bStuffing &&
          f.context.bEOSCheck && !f.context.bEOS && !f.context.eosTxComplete,
          "EOS restores packetization state without claiming device consumption");
}
static Bytes vop(unsigned index, size_t size = 0)
{
    // Original first eight bytes of generated Simple P-VOPs 27 and 28.
    // Preserve their original sizes for this host-framing regression, while
    // deliberately replacing compressed bodies with deterministic test data.
    if (!size) size = index == 27 ? 7163 : index == 28 ? 6907 : 7000 + (index * 137) % 2300;
    Bytes result(size, static_cast<uint8_t>(0x80 | (index & 0x3f)));
    const uint8_t prefix27[] = {0,0,1,0xb6,0x5d,0xf0,0x43,0xe1};
    const uint8_t prefix28[] = {0,0,1,0xb6,0x5e,0x60,0x43,0xe3};
    const uint8_t *prefix = index == 28 ? prefix28 : prefix27;
    std::copy(prefix, prefix + 8, result.begin());
    return result;
}
static void Sequence(bool headers, bool wrap)
{
    ++groups;
    Fixture f(headers);
    if (!f.configured) return; // Do not fabricate state to bypass a real setup failure.
    if (wrap) f.prime_wrap();
    std::vector<Bytes> original;
    for (unsigned i = 0; i < 60; ++i) {
        original.push_back(vop(i));
        if (!f.send(original.back(), static_cast<uint64_t>(i + 1) * 100000)) return;
    }
    const Bytes wire = f.drain();
    check(wire == queued_bytes(), "full real-ring stream matches all actual enqueue bytes after caller mutation");
    std::vector<Pes> parsed;
    check(parse(wire, &parsed), "all 60 public inputs produce structurally valid PES");
    const unsigned first = headers ? 1 : 0;
    check(parsed.size() == 60 + first, "exactly one picture PES per original VOP plus optional initial metadata");
    if (parsed.size() != 60 + first) return;
    if (headers) check(parsed[0].has_pts && parsed[0].pts == 0x1ffffffffULL && parsed[0].payload == metadata,
                       "only initial original metadata gets SoftRave reserved PTS");
    for (unsigned i = 0; i < 60; ++i) {
        check(parsed[i + first].has_pts && parsed[i + first].pts == (i + 1) * 10,
              "every VOP, including 27/28, retains caller scaled PTS");
        check(parsed[i + first].payload == original[i], "every VOP payload survives PES/ring byte-exact");
    }
    check(!f.context.PESConvParams.m_bAddSpsPps && f.context.PESConvParams.m_bSoftRave,
          "ordinary input completes metadata injection without clearing SoftRave");
}
static void ReorderedAndZero()
{
    ++groups;
    Fixture f(false);
    if (!f.configured) return;
    const uint64_t timestamps[] = {100000, 400000, 200000, 300000, 0, 500000};
    for (unsigned i = 0; i < 6; ++i) if (!f.send(vop(i, 128), timestamps[i])) return;
    std::vector<Pes> parsed;
    const Bytes wire = f.drain();
    check(parse(wire, &parsed) && parsed.size() == 6, "reordered/zero inputs retain one PES each");
    if (parsed.size() != 6) return;
    for (unsigned i = 0; i < 6; ++i)
        check(parsed[i].has_pts && parsed[i].pts == timestamps[i] / 10000 && parsed[i].payload == vop(i, 128),
              "opaque reordered identities and ordinary zero PTS are not sorted, invented or omitted");
}
static void Fragmented()
{
    ++groups;
    Fixture f(false);
    if (!f.configured) return;
    f.prime_wrap();
    const Bytes original = vop(27, 2 * 0xfff0 + 97);
    if (!f.send(original, 2800000)) return;
    const Bytes wire = f.drain();
    check(wire == queued_bytes(), "fragmented access unit survives wrapped ring exactly");
    std::vector<Pes> parsed;
    check(parse(wire, &parsed) && parsed.size() == 3, "large original access unit splits into three valid PES packets");
    Bytes joined;
    for (unsigned i = 0; i < parsed.size(); ++i) {
        check(i ? !parsed[i].has_pts : parsed[i].has_pts && parsed[i].pts == 280,
              "only first PES fragment carries the original PTS");
        joined.insert(joined.end(), parsed[i].payload.begin(), parsed[i].payload.end());
    }
    check(joined == original, "fragmentation never drops, duplicates or transforms original VOP bytes");
    packets.clear();
    check(f.send(vop(28, 128), 2900000), "subsequent VOP is accepted after large fragmented input");
    parsed.clear();
    check(parse(f.drain(), &parsed) && parsed.size() == 1 && parsed[0].has_pts && parsed[0].pts == 290,
          "following access unit starts a new nonzero PTS after prior continuation fragments");
}
static void InBandHeaders()
{
    ++groups;
    for (unsigned code = 0; code < 256; ++code) {
        Fixture f(true);
        if (!f.configured) return;
        Bytes input = {0,0,1,static_cast<uint8_t>(code),0x80};
        const Bytes picture = vop(0,128);
        input.insert(input.end(),picture.begin(),picture.end());
        if (!f.send(input,100000)) return;
        std::vector<Pes> parsed;
        check(parse(f.drain(),&parsed),"in-band header decision retains valid PES framing");
        const bool vol = code >= 0x20 && code <= 0x2f;
        check(parsed.size() == (vol ? 1U : 2U),"only a MPEG4 VOL suppresses stored metadata injection");
        if (parsed.size() != (vol ? 1U : 2U)) continue;
        check(parsed.back().has_pts && parsed.back().pts == 10 && parsed.back().payload == input,
              "in-band header and picture bytes keep their original timestamp");
        if (!vol) check(parsed.front().payload == metadata,"missing VOL retains exact stored metadata");
    }
}
static void HeaderBounds()
{
    ++groups;
    const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    auto *mapping = static_cast<uint8_t *>(mmap(nullptr,page*2,PROT_READ|PROT_WRITE,
                                               MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    if (mapping == MAP_FAILED || mprotect(mapping+page,page,PROT_NONE)) std::abort();
    Fixture f(true);
    if (!f.configured) { munmap(mapping,page*2); return; }
    for (BC_MEDIA_SUBTYPE codec : {BC_MSUBTYPE_H264,BC_MSUBTYPE_AVC1,BC_MSUBTYPE_DIVX,BC_MSUBTYPE_DIVX311}) {
        f.context.VidParams.MediaSubType = codec;
        f.context.PESConvParams.m_bIsAdd_SCode_CodeIn = false;
        for (const Bytes &input : {Bytes{},Bytes{0},Bytes{0,0},Bytes{0,0,0},Bytes{0,0,1},
                Bytes{0,0,0,1},Bytes{0,0,1,0xb6},Bytes{0,0,1,0xb6,0,0,0},
                Bytes{0,0,1,0x20},Bytes{0,0,1,0x67},
                Bytes{0,0,0,0,1,0xb6,0,0,1,0x2f}}) {
            uint8_t *data = mapping+page-input.size();
            if (!input.empty()) std::memcpy(data,input.data(),input.size());
            const PES_CONVERT_PARAMS before = f.context.PESConvParams;
            const bool expected = codec == BC_MSUBTYPE_DIVX ?
                (input.size() == 4 && input.back() == 0x20) || input.size() == 10 :
                input.size() == 4 && input.back() == 0x67;
            check(!!DtsCheckSpsPps(&f.context,data,input.size()) == expected,
                  "header detection stays within the exact mapped boundary and codec");
            check(!std::memcmp(&before,&f.context.PESConvParams,sizeof before),
                  "header detection does not consume parser state");
        }
        check(!DtsCheckSpsPps(&f.context,nullptr,17),"null header input is not read");
    }
    munmap(mapping,page*2);
}
int main(int argc, char **argv)
{
    if (argc >= 2 && !std::strcmp(argv[1], "--h263-pes-fixture")) {
        if (argc != 3) return 2;
        Bytes original;
        if (!read_fixture(argv[2], &original)) {
            std::fprintf(stderr, "H263 fixture must be a bounded, nonempty regular baseline ES file\n");
            return 2;
        }
        H263Fixture(original, false);
        H263Fixture(original, true);
        std::printf("H263 fixture transport: %zu bytes, algorithm=6 PES, supplied startCodeSz=0, "
                    "%u groups, %u checks, %u failures (host only; no device or decoder conformance)\n",
                    original.size(), groups, checks, failures);
        return failures ? 1 : 0;
    }
    if (argc > 2 || (argc == 2 && std::strcmp(argv[1], "--pes-only"))) return 2;
    // --pes-only isolates packetization from the separately reproduced
    // historical DIVX metadata code-zero/startcode parsing failure.
    Sequence(false, false);
    Sequence(false, true);
    ReorderedAndZero();
    Fragmented();
    if (argc == 1) {
        Sequence(true, false); Sequence(true, true);
        InBandHeaders(); HeaderBounds();
    }
    std::printf("MPEG4 input: %u groups, %u checks, %u failures (host framing only; no device)\n",
                groups, checks, failures);
    return failures ? 1 : 0;
}
