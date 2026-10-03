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
static void check(bool value, const char *message)
{
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
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

int main(int argc, char **argv)
{
    if (argc > 2 || (argc == 2 && std::strcmp(argv[1], "--framing"))) return 2;
    Framing();
    WmvBFrameMetadata();
    AnnexBScannerBounds();
    // --framing omits the separate SPS detector regression.
    if (argc == 1) DetectorBounds();
    if (failures) { std::fprintf(stderr, "%u input checks failed\n", failures); return 1; }
    std::puts("PASS: actual AVC1/WMV3 input timestamps, framing, SPS detection and bounded Annex-B scanning");
}
