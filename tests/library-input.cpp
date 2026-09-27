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
    // Baseline proof uses --framing: do not exercise the known unsafe old
    // Annex-B parser on malformed/exact-sized buffers before the fix.
    if (argc == 1) DetectorBounds();
    if (failures) { std::fprintf(stderr, "%u input checks failed\n", failures); return 1; }
    std::puts("PASS: actual AVC1/WMV3 input timestamps, metadata, framing and bounded SPS detection checks");
}
