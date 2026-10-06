// SPDX-License-Identifier: LGPL-2.1-or-later
// Actual DtsProcInput/PES/parser/ring regression; no decoder/device is opened.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <unistd.h>
#include "7411d.h"
#include "libcrystalhd_if.h"
#include "libcrystalhd_int_if.h"
#include "libcrystalhd_priv.h"
#include "libcrystalhd_fwcmds.h"

extern bc_dil_glob_s *bc_dil_glob_ptr;
static unsigned failures;
static std::vector<std::vector<uint8_t> > packets;
// Explicit CPU-only fault injection. Off by default for all legacy framing
// cases; no ioctl/worker/device is reachable. Attempts are not acceptance.
static struct {
    DTS_LIB_CONTEXT *context = nullptr;
    bool consume_after_push = false;
    bool allow_metadata_retry = false;
    unsigned waits = 0, metadata_retries = 0, cancel_after_push = 0;
    BC_STATUS first_marker_status = BC_STS_SUCCESS;
    std::vector<uint32_t> attempts;
    std::vector<std::vector<uint8_t> > accepted;
    std::vector<uint8_t> consumed;
} transport;
static void check(bool value, const char *message)
{
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}
extern "C" BC_STATUS __real_txBufPush(pTXBUFFER, uint8_t *, uint32_t);
static void consume_ring(pTXBUFFER ring)
{
    const uint32_t size = ring->busySize;
    if (!size) return;
    std::vector<uint8_t> output(size);
    if (txBufPop(ring, output.data(), size) != BC_STS_SUCCESS) std::abort();
    transport.consumed.insert(transport.consumed.end(), output.begin(), output.end());
}
extern "C" BC_STATUS __wrap_txBufPush(pTXBUFFER ring, uint8_t *bytes, uint32_t size)
{
    if (transport.context) {
        if (ring != &transport.context->circBuf) std::abort();
        transport.attempts.push_back(size);
        if (transport.attempts.size() == 1 && transport.first_marker_status != BC_STS_SUCCESS)
            return transport.first_marker_status; // Rejected marker: NO ring push.
        const BC_STATUS status = __real_txBufPush(ring, bytes, size);
        if (status == BC_STS_SUCCESS) {
            transport.accepted.emplace_back(bytes, bytes + size);
            if (transport.consume_after_push) consume_ring(ring);
            if (transport.cancel_after_push == transport.accepted.size())
                transport.context->State = BC_DEC_STATE_STOP;
        }
        return status;
    }
    packets.emplace_back(bytes, bytes + size);
    return __real_txBufPush(ring, bytes, size);
}
extern "C" int __wrap_ioctl(int, unsigned long, ...) { std::abort(); }
extern "C" int __wrap_usleep(useconds_t duration)
{
    if (!transport.context) std::abort();
    if (duration == 2000 && transport.allow_metadata_retry) {
        ++transport.metadata_retries; return 0;
    }
    if (duration != 5000) std::abort();
    ++transport.waits;
    transport.context->State = BC_DEC_STATE_STOP;
    // A formerly impossible >ring enqueue exits via the real admission check,
    // not a timeout, actual sleep or fabricated transport success.
    return 0;
}
// This internal implementation helper has C++ linkage (not declared in the
// public C header); exercise its actual production symbol directly.
BC_STATUS DtsAlignSendData(HANDLE, uint8_t *, uint32_t, uint64_t, BOOL);
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

using Bytes = std::vector<uint8_t>;
// Synthetic NAL payloads exercise framing only, not decoder conformance.
static const Bytes sps = {0x67, 0x64, 0x00, 0x1f};
static const Bytes pps = {0x68, 0xee, 0x3c};
static const Bytes idr = {0x65, 0x88, 0x84, 0x11, 0x22, 0x33, 0x44, 0x55};
static void append(Bytes &bytes, const Bytes &nal, unsigned length_size)
{
    if (!length_size) bytes.insert(bytes.end(), {0, 0, 0, 1});
    else for (unsigned i = length_size; i; --i)
        bytes.push_back(static_cast<uint8_t>(nal.size() >> ((i - 1) * 8)));
    bytes.insert(bytes.end(), nal.begin(), nal.end());
}
static Bytes access_unit(unsigned length_size, bool headers)
{
    Bytes bytes;
    if (headers) { append(bytes, sps, length_size); append(bytes, pps, length_size); }
    append(bytes, idr, length_size);
    return bytes;
}

struct Fixture {
    bc_dil_glob_s globals = {};
    DTS_LIB_CONTEXT context = {};
    Fixture(unsigned length_size, const Bytes *wmv_metadata = nullptr)
    {
        bc_dil_glob_ptr = &globals;
        context.Sig = LIB_CTX_SIG;
        context.ProcessID = getpid();
        context.DevId = BC_PCI_DEVID_FLEA;
        context.State = BC_DEC_STATE_START;
        context.alignBuf = static_cast<uint8_t *>(std::malloc(ALIGN_BUF_SIZE));
        if (!context.alignBuf || txBufInit(&context.circBuf, CIRC_TX_BUF_SIZE) != BC_STS_SUCCESS)
            std::abort();
        Bytes metadata;
        if (wmv_metadata) metadata = *wmv_metadata;
        else { append(metadata, sps, 0); append(metadata, pps, 0); }
        BC_INPUT_FORMAT format = {};
        format.mSubtype = wmv_metadata ? BC_MSUBTYPE_WMV3 : BC_MSUBTYPE_AVC1;
        format.width = 640; format.height = 360; format.Progressive = true;
        format.startCodeSz = length_size;
        format.OptFlags = 0x80000001; // Legacy client flags, without the single-thread flag.
        format.pMetaData = metadata.data(); format.metaDataSz = metadata.size();
        check(DtsSetInputFormat(&context, &format) == BC_STS_SUCCESS, "configure actual input converter");
        check(context.PESConvParams.m_bAddSpsPps && context.PESConvParams.m_bIsAdd_SCode_CodeIn &&
              !context.SingleThreadedAppMode && context.VidParams.StreamType == BC_STREAM_TYPE_PES,
              "fixture exercises pending metadata before AVC1 conversion in PES mode");
        packets.clear();
    }
    ~Fixture()
    {
        DtsReleasePESConverter(&context);
        std::free(context.VidParams.pMetaData);
        std::free(context.alignBuf);
        txBufFree(&context.circBuf);
        bc_dil_glob_ptr = nullptr;
    }
    BC_STATUS send(Bytes input)
    {
        const uint32_t size = input.size();
        input.resize(input.size() + 16, 0); // Defensive padding for the legacy converter.
        return DtsProcInput(&context, input.data(), size, 666671, false);
    }
};

static uint64_t pts(const Bytes &packet)
{
    if (packet.size() < 14 || !(packet[7] & 0x80)) return UINT64_MAX;
    const uint8_t *p = packet.data() + 9;
    return (static_cast<uint64_t>((p[0] >> 1) & 7) << 30) |
        (static_cast<uint64_t>(p[1]) << 22) | (static_cast<uint64_t>(p[2] >> 1) << 15) |
        (static_cast<uint64_t>(p[3]) << 7) | (p[4] >> 1);
}
static Bytes payload(const Bytes &packet)
{
    if (packet.size() < 9 || static_cast<size_t>(9 + packet[8]) > packet.size()) return {};
    return Bytes(packet.begin() + 9 + packet[8], packet.end());
}

static void Framing()
{
    for (unsigned length : {1U, 2U, 4U}) {
        Fixture f(length);
        const Bytes input = access_unit(length, true);
        check(DtsCheckSpsPps(&f.context, const_cast<uint8_t *>(input.data()), input.size()),
              "length-prefixed in-band SPS is recognized before conversion");
        check(f.send(input) == BC_STS_SUCCESS, "AVC1 access unit is accepted");
        check(packets.size() == 1, "in-band AVC1 parameter sets suppress redundant untimestamped metadata PES");
        if (!packets.empty()) {
            check(pts(packets.front()) == 66, "first actual PES retains caller's scaled positive timestamp");
            check(payload(packets.front()) == access_unit(0, true), "one PES carries exact converted AU, without duplicate headers");
        }
        check(!f.context.PESConvParams.m_bAddSpsPps, "first-input metadata handling completes normally");
    }
    {
        Fixture f(4);
        check(f.send(access_unit(4, false)) == BC_STS_SUCCESS, "AVC1 without in-band SPS remains accepted");
        check(packets.size() == 2, "missing in-band SPS retains separate stored metadata injection");
        if (packets.size() == 2) {
            Bytes metadata; append(metadata, sps, 0); append(metadata, pps, 0);
            check(pts(packets[0]) == UINT64_MAX && payload(packets[0]) == metadata,
                  "separate metadata retains original untimestamped bytes");
            check(pts(packets[1]) == 66 && payload(packets[1]) == access_unit(0, false),
                  "picture retains timestamp and exact payload after necessary metadata");
        }
    }
    {
        Fixture f(4);
        check(f.send(access_unit(0, true)) == BC_STS_SUCCESS, "already Annex-B AVC1 compatibility remains accepted");
        check(packets.size() == 1 && pts(packets.front()) == 66 &&
              payload(packets.front()) == access_unit(0, true), "Annex-B input keeps one timestamped AU");
        check(!f.context.PESConvParams.m_bIsAdd_SCode_CodeIn, "existing Annex-B converter autodetection remains intact");
    }
}

static void DetectorBounds()
{
    Fixture f(4);
    for (const Bytes &input : {Bytes{}, Bytes{0}, Bytes{0, 0}, Bytes{0, 0, 0},
            Bytes{0, 0, 0, 0}, Bytes{0xff, 0xff, 0xff, 0xff, 0x67},
            Bytes{0, 0, 0, 4, 0x67, 0x11, 0x22}, Bytes{0, 0, 0, 1},
            Bytes{0, 0, 1}, access_unit(4, false), access_unit(0, false)}) {
        // Exact-sized vectors: sanitizers detect any detector overread.
        check(!DtsCheckSpsPps(&f.context, const_cast<uint8_t *>(input.data()), input.size()),
              "missing/truncated/oversized SPS framing does not claim headers are present");
    }
    check(!DtsCheckSpsPps(&f.context, nullptr, 17), "null input is not dereferenced");
    Bytes input = access_unit(4, true);
    const Bytes before = input;
    const PES_CONVERT_PARAMS state = f.context.PESConvParams;
    check(DtsCheckSpsPps(&f.context, input.data(), input.size()), "valid exact-sized AVC1 header buffer is recognized");
    check(input == before && !std::memcmp(&state, &f.context.PESConvParams, sizeof state),
          "SPS detection does not mutate bytes or conversion state");
    f.context.PESConvParams.m_lStartCodeDataSize = 3;
    check(!DtsCheckSpsPps(&f.context, input.data(), input.size()), "continuation bytes are not assumed to start a new length-prefixed NAL");
    f.context.PESConvParams.m_lStartCodeDataSize = 0;
    // The low byte of this valid 4-byte NAL length looks like an SPS type
    // after a three-byte Annex-B prefix; it is actually an ordinary IDR.
    Bytes long_idr(263, 0x55); long_idr[0] = 0x65;
    Bytes length_collision; append(length_collision, long_idr, 4);
    check(!DtsCheckSpsPps(&f.context, length_collision.data(), length_collision.size()),
          "0x00000107 NAL length is not mistaken for an Annex-B SPS marker");
    Bytes preceded; append(preceded, Bytes{0x09, 0xf0}, 4); append(preceded, sps, 4);
    check(DtsCheckSpsPps(&f.context, preceded.data(), preceded.size()),
          "SPS after another bounded length-prefixed NAL is found");
    f.context.PESConvParams.m_bIsAdd_SCode_CodeIn = false;
    Bytes annex_three = {0, 0, 1, 0x67, 0x64, 0, 0x1f};
    check(DtsCheckSpsPps(&f.context, annex_three.data(), annex_three.size()),
          "already-selected Annex-B mode retains three-byte start codes");
    check(!DtsCheckSpsPps(&f.context, input.data(), input.size()),
          "Annex-B mode does not reinterpret a length-prefixed input");
    f.context.PESConvParams.m_bIsAdd_SCode_CodeIn = true;
    for (unsigned length : {0U, 5U, UINT32_MAX}) {
        f.context.VidParams.StartCodeSz = length;
        check(!DtsCheckSpsPps(&f.context, input.data(), input.size()), "invalid configured length width is bounded");
    }
}

static void AnnexBScannerBounds()
{
    Fixture f(4);
    f.context.VidParams.MediaSubType = BC_MSUBTYPE_H264;
    f.context.VidParams.StreamType = BC_STREAM_TYPE_ES;
    f.context.PESConvParams.m_bAddSpsPps = false;
    f.context.PESConvParams.m_bIsAdd_SCode_CodeIn = false;
    DTS_INPUT_MDATA metadata[32] = {};
    f.context.MdataPoolPtr = metadata;
    f.context.MDPendHead = f.context.MDPendTail = DTS_MDATA_PEND_LINK((&f.context));
    for (DTS_INPUT_MDATA &entry : metadata) {
        entry.flink = f.context.MDFreeHead;
        f.context.MDFreeHead = &entry;
    }
    auto scan = [&](const Bytes &input, bool first, NALU_t &nalu) {
        f.context.PESConvParams.m_bIsFirstByteStreamNALU = first;
        uint8_t *exact = new uint8_t[input.size()];
        if (!input.empty()) std::memcpy(exact, input.data(), input.size());
        const int consumed = DtsGetNaluType(&f.context, exact, input.size(), &nalu, false);
        check(input.empty() || !std::memcmp(exact, input.data(), input.size()),
              "NAL scanning leaves exact-sized caller bytes unchanged");
        delete[] exact;
        return consumed;
    };
    auto send = [&](const Bytes &input) {
        packets.clear();
        f.context.PESConvParams.m_bIsFirstByteStreamNALU = true;
        uint8_t *exact = new uint8_t[input.size()];
        std::memcpy(exact, input.data(), input.size());
        const BC_STATUS status = DtsProcInput(&f.context, exact, input.size(), 666671, false);
        check(!std::memcmp(exact, input.data(), input.size()), "ES transport preserves caller bytes");
        delete[] exact;
        return status;
    };
    auto timestamp_packet = [&](const Bytes &packet) {
        const uint32_t sequence = f.context.InMdataTag & DTS_MDATA_MAX_TAG;
        const Bytes expected = {0, 0, 1, 0xbd, 7, 0x40,
            static_cast<uint8_t>(sequence), static_cast<uint8_t>(sequence >> 8), 0x0a, 0, 0, 0};
        return packet == expected && f.context.MDPendTail != DTS_MDATA_PEND_LINK((&f.context)) &&
            f.context.MDPendTail->appTimeStamp == 66;
    };
    for (const Bytes &input : {Bytes{}, Bytes{0}, Bytes{0, 0}, Bytes{0, 0, 0},
            Bytes{0, 0, 0, 0}, Bytes{1}, Bytes{0, 1, 0x65}, Bytes{0, 0, 1},
            Bytes{0, 0, 0, 1}, Bytes{0, 0, 1, 0, 0, 1, 0x65},
            Bytes{0, 0, 1, 0, 0, 0, 1, 0x65}}) {
        NALU_t nalu = {};
        check(scan(input, true, nalu) < 0, "empty/truncated/adjacent prefixes have no NAL header");
        check(f.context.PESConvParams.m_bIsFirstByteStreamNALU,
              "a rejected NAL does not consume first-byte-stream state");
    }
    NALU_t nalu = {};
    check(DtsGetNaluType(&f.context, nullptr, 1, &nalu, false) < 0 &&
          DtsGetNaluType(&f.context, nullptr, 1, &nalu, true) < 0,
          "both NAL scan modes reject null input");
    uint8_t header = 0x65;
    check(DtsGetNaluType(&f.context, &header, 0, &nalu, true) < 0 &&
          DtsGetNaluType(&f.context, &header, UINT32_MAX, &nalu, false) < 0 &&
          DtsGetNaluType(nullptr, &header, 1, &nalu, true) < 0 &&
          DtsGetNaluType(&f.context, &header, 1, nullptr, true) < 0,
          "NAL scan rejects empty, oversized and invalid arguments before reading");
    check(DtsGetNaluType(&f.context, &header, 1, &nalu, true) == 1 &&
          nalu.Len == 1 && nalu.NalUnitType == NALU_TYPE_IDR,
          "sync-marker-free mode retains its one-byte NAL behavior");
    const Bytes leading = {0, 0, 0, 0, 1, 0x65};
    check(scan(leading, true, nalu) == 6 && nalu.StartcodePrefixLen == 4 && nalu.Len == 1,
          "first NAL permits leading_zero_8bits without changing payload length");
    check(scan(leading, false, nalu) < 0, "subsequent NAL retains its leading-zero restriction");
    const Bytes mixed = {0, 0, 1, 9, 0xf0, 0, 0, 0, 0, 0, 1, 0x65, 0x88};
    check(scan(mixed, true, nalu) == 7 && nalu.StartcodePrefixLen == 3 && nalu.Len == 2 &&
          nalu.NalUnitType == NALU_TYPE_AUD, "mixed prefixes exclude trailing zeros from NAL length");
    const Bytes remainder(mixed.begin() + 7, mixed.end());
    check(scan(remainder, false, nalu) == 6 && nalu.StartcodePrefixLen == 4 && nalu.Len == 2 &&
          nalu.NalUnitType == NALU_TYPE_IDR, "next scan begins at the complete four-byte prefix");

    // Extension payloads below test framing only, not MVC bitstream conformance.
    for (uint8_t type : {uint8_t(9), uint8_t(14), uint8_t(15), uint8_t(20), uint8_t(31)}) {
        const Bytes unknown = {0, 0, 1, type, 0xf0};
        uint32_t offset = 123;
        int nal_type = 123;
        uint8_t *exact = new uint8_t[unknown.size()];
        std::memcpy(exact, unknown.data(), unknown.size());
        f.context.PESConvParams.m_bIsFirstByteStreamNALU = true;
        check(DtsParseAVC(&f.context, exact, unknown.size(), &offset, false, &nal_type) == BC_STS_ERROR &&
              nal_type == -1, "terminal unrecognized NAL has no timestamp boundary");
        delete[] exact;
        const uint32_t previous_tag = f.context.InMdataTag;
        check(send(unknown) == BC_STS_SUCCESS && packets.size() == 1 && packets[0] == unknown &&
              f.context.InMdataTag == previous_tag,
              "terminal unrecognized NAL preserves exact ES bytes without timestamp metadata");
        Bytes picture = unknown;
        append(picture, idr, 0);
        check(send(picture) == BC_STS_SUCCESS && packets.size() == 2 && timestamp_packet(packets[0]) &&
              packets[1] == picture,
              "unknown-to-IDR input keeps the whole ES payload after its timestamp metadata");
        for (const Bytes &parameter : {sps, pps, Bytes{0x06, 0x80}}) {
            Bytes combined = unknown;
            append(combined, parameter, 0);
            const size_t boundary = combined.size();
            append(combined, idr, 0);
            const Bytes prefix(combined.begin(), combined.begin() + boundary);
            const Bytes tail(combined.begin() + boundary, combined.end());
            check(send(combined) == BC_STS_SUCCESS && packets.size() == 3 &&
                  packets[0] == prefix && timestamp_packet(packets[1]) && packets[2] == tail,
                  "parameter-set/SEI cumulative split preserves prefix and timestamped remainder");
        }
    }
    for (uint8_t type : {uint8_t(1), uint8_t(5)}) {
        Bytes picture = {0, 0, 1, 9, 0xf0, 0, 0, 1, type, 0x88};
        uint32_t offset = 0;
        f.context.PESConvParams.m_bIsFirstByteStreamNALU = true;
        check(DtsFindIDR(&f.context, picture.data(), picture.size(), &offset) == BC_STS_SUCCESS &&
              offset == picture.size(), "IDR search preserves legacy slice and IDR acceptance");
    }
    // All short combinations exercise start-code look-behind and terminal bounds.
    const uint8_t alphabet[] = {0, 1, 9, 0x65};
    size_t combinations = 1;
    for (size_t size = 0; size <= 8; ++size, combinations *= 4) {
        for (size_t value = 0; value < combinations; ++value) {
            Bytes input(size);
            size_t digits = value;
            for (uint8_t &byte : input) { byte = alphabet[digits & 3]; digits >>= 2; }
            const int consumed = scan(input, true, nalu);
            check(consumed < 0 || (static_cast<size_t>(consumed) <= size && nalu.Len > 0 &&
                  nalu.Len + nalu.StartcodePrefixLen <= static_cast<uint32_t>(consumed)),
                  "short NAL scan returns only an in-range, nonempty extent");
        }
    }
}

static void WmvBFrameMetadata()
{
    // Canonical four-byte STRUCT_C. MAXBFRAMES is a three-bit count,
    // not a boolean whose only true representation is seven. The synthetic
    // picture bodies exercise framing only, not decoder conformance.
    for (unsigned maximum = 0; maximum < 8; ++maximum) {
        for (unsigned range = 0; range < 2; ++range) {
            for (unsigned interpolation = 0; interpolation < 2; ++interpolation) {
                const Bytes metadata = {0x4b, 0xf1, 0x0a,
                    static_cast<uint8_t>((range << 7) | (maximum << 4) |
                                         (interpolation << 1) | 1)};
                Fixture f(4, &metadata);
                check(f.context.PESConvParams.m_bMaxbFrames == (maximum != 0),
                      "every nonzero MAXBFRAMES count enables B-picture syntax");
                const unsigned first_bit = 7 - (2 + range + interpolation);
                const Bytes intra = {static_cast<uint8_t>(maximum ? 1U << (first_bit - 1) : 0),
                                      0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb};
                const Bytes predicted = {static_cast<uint8_t>(1U << first_bit),
                                          0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb};
                const Bytes bidirectional = {0, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb};
                const Bytes sequence = {0, 0, 1, 0x0f, 2, 0x80, 1, 0x68,
                                        metadata[0], metadata[1], metadata[2], metadata[3]};
                auto submit = [&](const Bytes &picture, bool is_intra) {
                    packets.clear();
                    check(f.send(picture) == BC_STS_SUCCESS, "WMV3 input is accepted");
                    check(packets.size() == (is_intra ? 2U : 1U),
                          "only actual WMV3 I pictures inject sequence metadata");
                    if (is_intra && packets.size() == 2)
                        check(payload(packets.front()) == sequence,
                              "I picture retains exact STRUCT_C and visible dimensions");
                    Bytes expected = {0, 0, 1, 0x0d};
                    expected.insert(expected.end(), picture.begin(), picture.end());
                    check(!packets.empty() && pts(packets.back()) == 66 &&
                          payload(packets.back()) == expected,
                          "WMV3 picture bytes and caller timestamp remain unchanged");
                };
                submit(intra, true);
                submit(predicted, false);
                if (maximum) submit(bidirectional, false);
                submit(intra, true);
            }
        }
    }
}

struct TransportFixture : Fixture {
    DTS_INPUT_MDATA metadata[8] = {};
    explicit TransportFixture(bool link_pes = false) : Fixture(4) {
        context.DevId = link_pes ? BC_PCI_DEVID_LINK : BC_PCI_DEVID_FLEA;
        context.VidParams.MediaSubType = BC_MSUBTYPE_H264;
        context.VidParams.StreamType = link_pes ? BC_STREAM_TYPE_PES : BC_STREAM_TYPE_ES;
        context.PESConvParams.m_bAddSpsPps = false;
        context.PESConvParams.m_bIsAdd_SCode_CodeIn = false;
        context.MdataPoolPtr = metadata;
        context.MDPendHead = context.MDPendTail = DTS_MDATA_PEND_LINK((&context));
        for (DTS_INPUT_MDATA &entry : metadata) {
            entry.flink = context.MDFreeHead; context.MDFreeHead = &entry;
        }
        transport.context = &context;
        transport.consume_after_push = transport.allow_metadata_retry = false;
        transport.waits = transport.metadata_retries = transport.cancel_after_push = 0;
        transport.first_marker_status = BC_STS_SUCCESS;
        transport.attempts.clear(); transport.accepted.clear(); transport.consumed.clear();
    }
    ~TransportFixture() {
        transport.context = nullptr;
        DtsClrPendMdataList(&context);
    }
    unsigned free_metadata() const {
        const DTS_INPUT_MDATA *at = context.MDFreeHead;
        unsigned count = 0;
        while (at && count < 9) { ++count; at = at->flink; }
        return count;
    }
    bool no_pending_metadata() {
        return context.MDPendHead == DTS_MDATA_PEND_LINK((&context)) &&
               context.MDPendTail == DTS_MDATA_PEND_LINK((&context));
    }
};

static Bytes transport_bytes(uint32_t size)
{
    Bytes bytes(size);
    for (uint32_t i = 0; i < size; ++i) bytes[i] = static_cast<uint8_t>((i*37U+(i>>8)*13U+91U)%251U);
    return bytes;
}
static Bytes accepted_bytes()
{
    Bytes all;
    for (const Bytes &part : transport.accepted) all.insert(all.end(), part.begin(), part.end());
    return all;
}
static std::vector<uint32_t> expected_chunks(const uint8_t *data, uint32_t size)
{
    std::vector<uint32_t> chunks;
    while (size) {
        uint32_t chunk = size < ALIGN_BUF_SIZE ? size : ALIGN_BUF_SIZE;
        const unsigned odd = reinterpret_cast<uintptr_t>(data) % 4;
        // Preserve the historical unaligned first-copy footprint; the new
        // invariant is that subsequent aligned chunks are also bounded.
        if (odd && size > ALIGN_BUF_SIZE) chunk = ALIGN_BUF_SIZE-odd;
        chunks.push_back(chunk); data += chunk; size -= chunk;
    }
    return chunks;
}
static void BoundedEsChunks()
{
    for (uint32_t size : {ALIGN_BUF_SIZE-1U, uint32_t(ALIGN_BUF_SIZE), ALIGN_BUF_SIZE+1U,
                          ALIGN_BUF_SIZE+3U, CIRC_TX_BUF_SIZE-1U, uint32_t(CIRC_TX_BUF_SIZE),
                          CIRC_TX_BUF_SIZE+1U, 2U*CIRC_TX_BUF_SIZE-1U,
                          2U*CIRC_TX_BUF_SIZE, 2U*CIRC_TX_BUF_SIZE+17U}) {
        for (unsigned offset = 0; offset < 4; ++offset) {
            TransportFixture f;
            transport.consume_after_push = true;
            // Keep this fault-injection fixture explicitly finite, including
            // under 32-bit fortified memcpy range analysis.
            if (size > 3U*CIRC_TX_BUF_SIZE) std::abort();
            const Bytes expected = transport_bytes(size);
            Bytes storage(size+8, 0xd7);
            uint8_t *aligned = storage.data() + ((4-reinterpret_cast<uintptr_t>(storage.data())%4)%4);
            uint8_t *data = aligned+offset;
            std::memcpy(data, expected.data(), size);
            const Bytes original = storage;
            const BC_STATUS status = DtsAlignSendData(&f.context, data, size, 0, false);
            check(status == BC_STS_SUCCESS, "large aligned/unaligned ES accepts complete input");
            check(transport.waits == 0, "bounded ES never waits for an impossible enqueue");
            check(transport.attempts == expected_chunks(data,size), "exact ordered ES chunks bounded at512KiB");
            check(accepted_bytes() == expected && transport.consumed == expected,
                  "actual real ring pushes/pops preserve every ES byte in order");
            check(storage == original, "ES chunk alignment never changes caller samples or canaries");
            check(f.context.txBytesEnqueued == size && f.context.circBuf.busySize == 0 &&
                  f.context.circBuf.freeSize == CIRC_TX_BUF_SIZE, "whole acceptance counters and empty ring agree");
            std::printf("ES chunks: bytes=%u alignment=%u status=%u waits=%u attempts=%zu accepted=%zu\n",
                        size,offset,status,transport.waits,transport.attempts.size(),accepted_bytes().size());
        }
    }
}

static void EsRingPressureAndCancellation()
{
    for (uint32_t existing : {ALIGN_BUF_SIZE-5U,CIRC_TX_BUF_SIZE-16U,uint32_t(CIRC_TX_BUF_SIZE)}) {
        TransportFixture f;
        const Bytes prefix = transport_bytes(existing),expected = transport_bytes(CIRC_TX_BUF_SIZE+19U);
        check(__real_txBufPush(&f.context.circBuf,const_cast<uint8_t *>(prefix.data()),existing)==BC_STS_SUCCESS,
              "real ring prefill establishes near/full capacity case");
        transport.consume_after_push = true;
        Bytes input = expected;
        const BC_STATUS status = DtsAlignSendData(&f.context,input.data(),input.size(),0,false);
        if (existing > ALIGN_BUF_SIZE) {
            check(status==BC_STS_IO_USER_ABORT && transport.waits==1 && transport.attempts.empty() &&
                  transport.accepted.empty() && f.context.txBytesEnqueued==0 &&
                  f.context.circBuf.busySize==existing,"near-full/full ring first wait cancels without accepting a prefix");
            continue;
        }
        Bytes complete = prefix; complete.insert(complete.end(),expected.begin(),expected.end());
        check(status == BC_STS_SUCCESS && transport.waits == 0,
              "near-capacity ring makes bounded progress without an impossible wait");
        check(accepted_bytes() == expected && transport.consumed == complete &&
              f.context.txBytesEnqueued == expected.size(), "ring wrap preserves prefilling and complete new byte ordering");
    }
    {
        TransportFixture f; Bytes existing(CIRC_TX_BUF_SIZE,0x35),input(ALIGN_BUF_SIZE+1U,0x67);
        check(__real_txBufPush(&f.context.circBuf,existing.data(),existing.size())==BC_STS_SUCCESS,"full real ring before cancellation");
        check(DtsAlignSendData(&f.context,input.data(),input.size(),0,false)==BC_STS_IO_USER_ABORT &&
              transport.waits==1 && transport.attempts.empty() && transport.accepted.empty() &&
              f.context.txBytesEnqueued==0 && f.context.circBuf.busySize==existing.size(),
              "first5mswait cancellation rejects all unaccepted bytes without timeout/hang");
    }
    {
        TransportFixture f; Bytes input = transport_bytes(2U*CIRC_TX_BUF_SIZE+17U);
        transport.consume_after_push = true; transport.cancel_after_push = 1;
        check(DtsAlignSendData(&f.context,input.data(),input.size(),0,false)==BC_STS_IO_USER_ABORT,
              "stop after one accepted chunk never reports whole input success");
        check(transport.waits==0 && transport.accepted.size()==1 &&
              transport.accepted[0]==Bytes(input.begin(),input.begin()+ALIGN_BUF_SIZE) &&
              f.context.txBytesEnqueued==ALIGN_BUF_SIZE,"canceled prefix is exactly one bounded accepted chunk");
        const size_t attempts = transport.attempts.size();
        check(DtsAlignSendData(&f.context,input.data(),input.size(),0,false)==BC_STS_IO_USER_ABORT &&
              transport.attempts.size()==attempts,"already stopped input makes no further push");
    }
    for (uint32_t size : {CIRC_TX_BUF_SIZE+1U,UINT32_MAX}) {
        TransportFixture f; uint8_t byte=0x41;
        check(DtsSendData(&f.context,&byte,size,0,false)==BC_STS_INSUFF_RES && transport.waits==0 &&
              transport.attempts.empty() && transport.accepted.empty() && f.context.txBytesEnqueued==0,
              "direct greater-than-ring enqueue rejects INSUFF_RES before waiting or reading input");
    }
    for (bool quiescing : {false,true}) {
        TransportFixture f; uint8_t byte=0x41;
        if (quiescing) f.context.txQuiescing=true; else f.context.State=BC_DEC_STATE_STOP;
        check(DtsSendData(&f.context,&byte,UINT32_MAX,0,false)==BC_STS_IO_USER_ABORT &&
              transport.waits==0 && transport.attempts.empty(),"closed admission takes precedence over oversized capacity rejection");
    }
}

static void SpesAdmissionFailures()
{
    for (bool link_pes : {false,true}) {
        for (BC_STATUS injected : {BC_STS_BUSY,BC_STS_INSUFF_RES,BC_STS_IO_ERROR,
                                   BC_STS_IO_USER_ABORT,BC_STS_INV_ARG,BC_STS_TIMEOUT}) {
            TransportFixture f(link_pes);
            transport.first_marker_status=injected;
            Bytes input=transport_bytes(32);
            const BC_STATUS actual=DtsAlignSendData(&f.context,input.data(),input.size(),66,false);
            check(actual==injected,"first rejected SPES marker status is not overwritten by ES/LinkPES payload");
            check(transport.attempts.size()==1 && transport.attempts[0]==sizeof(BC_SEQ_HDR_FORMAT) &&
                  transport.accepted.empty() && f.context.txBytesEnqueued==0 &&
                  f.context.circBuf.busySize==0,"failed marker has exactly one unaccepted attempt and no payload bytes");
            check(f.free_metadata()==8 && f.no_pending_metadata() && transport.waits==0 &&
                  transport.metadata_retries==0,"failed marker restores free metadata without pending ownership");
            std::printf("SPES failure: link-pes=%u injected=%u actual=%u attempts=%zu accepted=%zu\n",
                        link_pes,injected,actual,transport.attempts.size(),accepted_bytes().size());
        }
        {
            TransportFixture f(link_pes);
            f.context.MDFreeHead=nullptr; // Valid pool, genuinely no free/pending entry.
            transport.allow_metadata_retry=true;
            Bytes input=transport_bytes(32);
            const BC_STATUS actual=DtsAlignSendData(&f.context,input.data(),input.size(),66,false);
            check(actual==BC_STS_BUSY && transport.metadata_retries==20 && transport.waits==0 &&
                  transport.attempts.empty() && transport.accepted.empty() && f.context.txBytesEnqueued==0,
                  "actual empty metadata pool exhausts20retry attempts and refuses all payload");
            check(!f.context.MDFreeHead && f.no_pending_metadata(),"empty metadata pool does not invent/leak an owner");
            std::printf("SPES empty pool: link-pes=%u actual=%u retries=%u push-attempts=%zu\n",
                        link_pes,actual,transport.metadata_retries,transport.attempts.size());
        }
        {
            TransportFixture f(link_pes); Bytes input=transport_bytes(32);
            check(DtsAlignSendData(&f.context,input.data(),input.size(),66,false)==BC_STS_SUCCESS &&
                  transport.attempts.size()==2 && transport.accepted.size()==2,
                  "normal positive timestamp accepts marker followed by one payload packet");
            if (transport.accepted.size()==2) {
                const uint32_t sequence=f.context.InMdataTag&DTS_MDATA_MAX_TAG;
                const Bytes marker={0,0,1,0xbd,7,0x40,static_cast<uint8_t>(sequence),
                    static_cast<uint8_t>(sequence>>8),0x0a,0,0,0};
                check(transport.accepted[0]==marker &&
                      (link_pes ? payload(transport.accepted[1]) : transport.accepted[1])==input,
                      "normal marker bytes and ordered ES/LinkPES payload remain exact");
                check(f.free_metadata()==7 && !f.no_pending_metadata() &&
                      f.context.MDPendHead==f.context.MDPendTail && f.context.MDPendHead->appTimeStamp==66,
                      "accepted marker holds exactly one correctly timestamped metadata owner");
                check(DtsClrPendMdataList(&f.context)==BC_STS_SUCCESS && f.free_metadata()==8 &&
                      f.no_pending_metadata(),"normal pending marker returns safely to free pool");
            }
        }
    }
}

static void TransportAdmission()
{
    BoundedEsChunks(); EsRingPressureAndCancellation(); SpesAdmissionFailures();
}

int main(int argc, char **argv)
{
    if (argc > 2 || (argc == 2 && std::strcmp(argv[1], "--framing") &&
                     std::strcmp(argv[1], "--transport-only"))) return 2;
    if (argc == 2 && !std::strcmp(argv[1], "--transport-only")) {
        TransportAdmission();
        if (failures) { std::fprintf(stderr,"%u transport checks failed\n",failures); return 1; }
        std::puts("PASS: CPU fault injection, bounded real ES ring progress and exact SPES failure admission");
        return 0;
    }
    Framing();
    WmvBFrameMetadata();
    AnnexBScannerBounds();
    // --framing omits the separate SPS detector regression.
    if (argc == 1) { DetectorBounds(); TransportAdmission(); }
    if (failures) { std::fprintf(stderr, "%u input checks failed\n", failures); return 1; }
    std::puts("PASS: actual AVC1/WMV3 input timestamps, framing, SPS detection and bounded Annex-B scanning");
}
