// SPDX-License-Identifier: LGPL-2.1-or-later
// Public raw-frame carrier API regression. This executable never opens a device.
// Source rasters, tile assembly and the Annex-B syntax reader below are independent
// of the production serializer. Packed output is a finite output-chain prediction,
// not a claim about standalone hardware or the H.264 standard's output format.
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include "libcrystalhd_raw_frame.h"

using Bytes = std::vector<uint8_t>;
static unsigned checks, failures;
static void Check(bool value, const char *message)
{
    ++checks;
    if (!value) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
static void Require(bool value, const char *message)
{
    if (!value) throw message;
}

static const unsigned width = 256, height = 96, uv_width = 128, uv_height = 48;
static const size_t y_bytes = 24576, uv_bytes = 6144, frame_bytes = 36864;

static Bytes Source(unsigned k)
{
    Bytes out(frame_bytes);
    // Literal flat-raster construction, not the serializer's macroblock loop.
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x)
            out[y * width + x] = static_cast<uint8_t>(16 +
                (17*x + 29*y + 11*k + (x ^ (y + 7*k))) % 220);
    for (unsigned q = 0; q < uv_height; ++q)
        for (unsigned x = 0; x < uv_width; ++x) {
            out[y_bytes + q * uv_width + x] = static_cast<uint8_t>(16 +
                (19*x + 31*q + 23*k + (x ^ (q + 3*k))) % 225);
            out[y_bytes + uv_bytes + q * uv_width + x] = static_cast<uint8_t>(16 +
                (47*x + 13*q + 19*k + ((3*x) ^ (5*q + k))) % 225);
        }
    return out;
}

static Bytes Assemble(const Bytes &first, const Bytes &second, const uint8_t *mask)
{
    Bytes out(frame_bytes);
    const size_t offsets[] = {0, y_bytes, y_bytes + uv_bytes};
    for (unsigned plane = 0; plane < 3; ++plane) {
        const unsigned pitch = plane ? uv_width : width;
        const unsigned rows = plane ? uv_height : height;
        const unsigned tile = plane ? 8 : 16;
        for (unsigned y = 0; y < rows; ++y)
            for (unsigned x = 0; x < pitch; ++x) {
                const size_t at = offsets[plane] + y*pitch + x;
                const unsigned mb = (y/tile)*16 + x/tile;
                out[at] = (mask[mb] ? second : first)[at];
            }
    }
    return out;
}

static Bytes OutputChainYuy2(const Bytes &planar)
{
    Require(planar.size() % frame_bytes == 0, "complete planar frames for packing");
    Bytes out(planar.size() / frame_bytes * width * height * 2);
    for (size_t f = 0; f < planar.size() / frame_bytes; ++f)
        for (unsigned y = 0; y < height; ++y) {
            const unsigned q = y/2;
            const unsigned neighbour = (y & 1) ? std::min(q+1, uv_height-1) : (q ? q-1 : 0);
            for (unsigned x = 0; x < width; x += 2) {
                const size_t src = f*frame_bytes;
                const size_t dst = (f*width*height + y*width + x)*2;
                out[dst] = planar[src + y*width + x];
                out[dst+2] = planar[src + y*width + x+1];
                for (unsigned p = 0; p < 2; ++p) {
                    const size_t base = src + y_bytes + p*uv_bytes;
                    const unsigned a = planar[base + q*uv_width + x/2];
                    const unsigned b = planar[base + neighbour*uv_width + x/2];
                    out[dst+1+2*p] = static_cast<uint8_t>((3*a+b+2)/4);
                }
            }
        }
    return out;
}

struct Reader {
    const Bytes &data;
    size_t bit;
    explicit Reader(const Bytes &bytes) : data(bytes), bit(0) {}
    uint32_t U(unsigned count) {
        Require(count <= 32 && bit <= data.size()*8 && count <= data.size()*8-bit,
                "bounded bit reader");
        uint32_t value = 0;
        while (count--) { value = (value << 1) | ((data[bit/8] >> (7-bit%8)) & 1); ++bit; }
        return value;
    }
    uint32_t UE() {
        unsigned zeros = 0;
        while (!U(1)) Require(++zeros < 32, "bounded Exp-Golomb");
        return ((uint32_t(1) << zeros)-1) + U(zeros);
    }
    int32_t SE() { const uint32_t n = UE(); return n & 1 ? int32_t((n+1)/2) : -int32_t(n/2); }
    void Align() { while (bit % 8) Require(U(1) == 0, "zero alignment bits"); }
    void End() { Require(U(1) == 1, "RBSP trailing bit"); Align(); Require(bit == data.size()*8, "no ignored RBSP suffix"); }
    void Samples(Bytes *raster, size_t base, unsigned pitch, unsigned x, unsigned y, unsigned tile) {
        Require(bit % 8 == 0 && tile*tile <= data.size()-bit/8, "complete I_PCM samples");
        for (unsigned row = 0; row < tile; ++row) {
            std::memcpy(raster->data()+base+(y+row)*pitch+x, data.data()+bit/8, tile);
            bit += tile*8;
        }
    }
};

struct Nalu { unsigned header; Bytes rbsp; };
static std::vector<Nalu> Nalus(const Bytes &au)
{
    std::vector<Nalu> result;
    size_t at = 0;
    while (at < au.size()) {
        Require(au.size()-at >= 6 && au[at] == 0 && au[at+1] == 0 &&
                au[at+2] == 0 && au[at+3] == 1, "four-byte Annex-B prefix");
        Nalu n = {au[at+4], Bytes{}};
        size_t end = at+5;
        while (end+3 < au.size() && !(au[end] == 0 && au[end+1] == 0 &&
                                    au[end+2] == 0 && au[end+3] == 1)) ++end;
        if (end+3 >= au.size()) end = au.size();
        unsigned zeros = 0;
        for (size_t pos = at+5; pos < end; ++pos) {
            const unsigned v = au[pos];
            if (zeros == 2) {
                Require(v >= 3, "escaped start-code emulation");
                if (v == 3) {
                    Require(pos+1 < end && au[pos+1] <= 3, "valid escape byte");
                    zeros = 0; continue;
                }
            }
            n.rbsp.push_back(static_cast<uint8_t>(v));
            zeros = v == 0 ? zeros+1 : 0;
        }
        result.push_back(n);
        at = end;
    }
    return result;
}

static void Sps(const Bytes &bytes)
{
    Reader r(bytes);
    Require(r.U(8) == 66 && r.U(8) == 128 && r.U(8) == 30, "plain Baseline/level3 profile");
    Require(r.UE() == 0 && r.UE() == 0 && r.UE() == 2 && r.UE() == 3, "SPS ids/frame-num4/POC2/maxrefs3");
    Require(r.U(1) == 0 && r.UE() == 15 && r.UE() == 5, "no gaps, exact coded geometry");
    Require(r.U(1) == 1 && r.U(1) == 1 && r.U(1) == 0 && r.U(1) == 1, "progressive/no crop/VUI");
    Require(r.U(1) == 1 && r.U(8) == 1 && r.U(1) == 0, "square aspect/no overscan");
    Require(r.U(1) == 1 && r.U(3) == 5 && r.U(1) == 0 && r.U(1) == 0 && r.U(1) == 0,
            "fixed limited-range colour metadata");
    Require(r.U(1) == 1 && r.U(32) == 1 && r.U(32) == 60 && r.U(1) == 1, "VUI30fps timing");
    Require(r.U(1) == 0 && r.U(1) == 0 && r.U(1) == 0 && r.U(1) == 0, "no HRD or extra restrictions");
    r.End();
}

static void Pps(const Bytes &bytes)
{
    Reader r(bytes);
    Require(r.UE() == 0 && r.UE() == 0 && r.U(1) == 0 && r.U(1) == 0 &&
            r.UE() == 0 && r.UE() == 0 && r.UE() == 0, "PPS CAVLC/one slice group/ref defaults");
    Require(r.U(1) == 0 && r.U(2) == 0 && r.SE() == 0 && r.SE() == 0 && r.SE() == 0,
            "PPS no weighting/default QP");
    Require(r.U(1) == 1 && r.U(1) == 0 && r.U(1) == 0, "PPS deblock controls/no redundant pictures");
    r.End();
}

struct Parser {
    unsigned frame = 0, max_long_index = 0, short_count = 0;
    bool loaded[2] = {false, false};
    Bytes slots[2];
    Bytes Read(const Bytes &au, bool upload, unsigned slot, const uint8_t *mask) {
        const std::vector<Nalu> ns = Nalus(au);
        Require(ns.size() == (frame ? 2U : 4U), "exact AU NAL count");
        Require(ns[0].header == 9 && ns[0].rbsp == Bytes{uint8_t(upload ? 0x10 : 0x30)}, "matching AUD");
        size_t vcl = 1;
        if (!frame) {
            Require(upload && slot == 0 && ns[1].header == 0x67 && ns[2].header == 0x68, "initial slot0 IDR headers");
            Sps(ns[1].rbsp); Pps(ns[2].rbsp); vcl = 3;
        }
        Require(ns[vcl].header == (frame ? 0x21U : 0x65U), "reference VCL header/IDR only first");
        Reader r(ns[vcl].rbsp);
        Require(r.UE() == 0 && r.UE() == (upload ? 2U : 0U) && r.UE() == 0, "one whole-picture slice");
        Require(r.U(4) == frame % 16, "global contiguous frame_num including replacement");
        Bytes decoded;
        if (upload) {
            if (!frame) {
                Require(r.UE() == 0 && r.U(1) == 0 && r.U(1) == 1, "IDR assigns LT0");
            } else {
                Require(r.U(1) == 1, "adaptive upload marking");
                if (!loaded[slot]) {
                    Require(slot == 1 && r.UE() == 4 && r.UE() == 2, "raise MaxLongTermFrameIdx for second source");
                    max_long_index = 1;
                } else Require(r.UE() == 2 && r.UE() == slot, "explicitly retire replaced slot");
                Require(r.UE() == 6 && r.UE() == slot && r.UE() == 0, "assign replacement LT slot/end marking");
            }
            Require(r.SE() == 0 && r.UE() == 1, "intra QP/deblock disabled");
            decoded.resize(frame_bytes);
            for (unsigned mb = 0; mb < 96; ++mb) {
                Require(r.UE() == 25, "every intra macroblock is PCM"); r.Align();
                r.Samples(&decoded, 0, width, (mb%16)*16, (mb/16)*16, 16);
                r.Samples(&decoded, y_bytes, uv_width, (mb%16)*8, (mb/16)*8, 8);
                r.Samples(&decoded, y_bytes+uv_bytes, uv_width, (mb%16)*8, (mb/16)*8, 8);
            }
            loaded[slot] = true; slots[slot] = decoded;
        } else {
            Require(loaded[0] && loaded[1] && max_long_index == 1, "both retained LTR slots available");
            Require(r.U(1) == 1 && r.UE() == 1 && r.U(1) == 1, "two active L0/explicit reordering");
            Require(r.UE() == 2 && r.UE() == 0 && r.UE() == 2 && r.UE() == 1 && r.UE() == 3,
                    "L0 ordered LT0,LT1/end");
            Require(r.U(1) == 0 && r.SE() == 0 && r.UE() == 1, "P sliding marking/zero QP/deblock disabled");
            for (unsigned mb = 0; mb < 96; ++mb) {
                Require(r.UE() == 0 && r.UE() == 0, "no skips, P_L0_16x16");
                const unsigned ref = 1-r.U(1); // te(v) for two active references.
                Require(mask && ref == mask[mb], "caller tile reference mask preserved");
                Require(r.SE() == 0 && r.SE() == 0 && r.UE() == 0, "zero MVD/no residual");
                // For each ref, every available matching-ref neighbour is zero.
                // Any single-match/median/unavailable fallback therefore infers
                // zero: this does not discriminate nonzero silicon MV branches.
            }
            decoded = Assemble(slots[0], slots[1], mask);
            short_count = 1; // maxrefs3 sliding window retains both long sources.
        }
        Require(unsigned(loaded[0])+unsigned(loaded[1])+short_count <= 3, "finite reference capacity");
        r.End(); ++frame;
        return decoded;
    }
};

static void WriteExclusive(const std::string &path, const Bytes &bytes)
{
    const int fd = open(path.c_str(), O_WRONLY|O_CREAT|O_EXCL, 0600);
    Require(fd >= 0, "exclusive output creation");
    size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t n = write(fd, bytes.data()+done, bytes.size()-done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(fd); throw "complete output write"; }
        done += static_cast<size_t>(n);
    }
    Require(close(fd) == 0, "output close");
}

static bool fail_calloc;
static unsigned calloc_failures;
extern "C" void *__real_calloc(size_t, size_t);
extern "C" void *__wrap_calloc(size_t n, size_t size)
{
    if (fail_calloc) { ++calloc_failures; errno = ENOMEM; return nullptr; }
    return __real_calloc(n, size);
}

static struct {
    bool enabled;
    HANDLE handle;
    const uint8_t *bytes;
    uint32_t size;
    uint64_t timestamp;
    BC_STATUS status;
    unsigned calls;
} submit;

extern "C" BC_STATUS DtsProcInput(HANDLE device, uint8_t *bytes, uint32_t size,
                                  uint64_t timestamp, BOOL encrypted)
{
    // Linking the production API's Submit must hit this exact CPU boundary.
    // No fake device is ever handed to the production input implementation.
    Check(submit.enabled, "Submit calls the expected CPU-only input boundary");
    Check(device == submit.handle && bytes == submit.bytes && size == submit.size &&
          timestamp == submit.timestamp && encrypted == FALSE,
          "Submit forwards exact pending pointer/size/device/token and FALSE");
    ++submit.calls;
    return submit.status;
}

struct Planes {
    Bytes storage[3];
    BC_RAW_FRAME_PLANES view = {};
    explicit Planes(const Bytes &flat, unsigned padding = 0) {
        Require(flat.size() == frame_bytes, "whole caller raster");
        const size_t bases[] = {0, y_bytes, y_bytes+uv_bytes};
        for (unsigned p = 0; p < 3; ++p) {
            const unsigned pitch = p ? uv_width : width, rows = p ? uv_height : height;
            view.strides[p] = pitch + padding + p;
            view.plane_bytes[p] = (rows-1)*view.strides[p]+pitch;
            // Deliberately unaligned user planes, exact declared last row,
            // and readable canaries outside the declared extent.
            storage[p].assign(view.plane_bytes[p]+19, 0xa5);
            view.planes[p] = storage[p].data()+3;
            for (unsigned row = 0; row < rows; ++row)
                std::memcpy(storage[p].data()+3+row*view.strides[p],
                            flat.data()+bases[p]+row*pitch, pitch);
        }
    }
};

struct Builder {
    BC_RAW_FRAME_BUILDER *value = nullptr;
    Builder() { Require(DtsRawFrameCreate(&value) == BC_STS_SUCCESS && value, "create builder"); }
    ~Builder() { if (DtsRawFrameDestroy(&value) != BC_STS_SUCCESS) std::abort(); }
};

static BC_STATUS Upload(Builder &b, unsigned slot, Planes &planes,
                        const uint8_t **au, uint32_t *bytes)
{
    return DtsRawFramePrepareUpload(b.value, slot, &planes.view, au, bytes);
}
static BC_STATUS Compose(Builder &b, const uint8_t *mask, uint32_t count,
                         const uint8_t **au, uint32_t *bytes)
{
    return DtsRawFramePrepareCompose(b.value, mask, count, au, bytes);
}
static BC_STATUS Accept(Builder &b, const uint8_t *au, uint32_t bytes,
                        BC_STATUS status = BC_STS_SUCCESS, uint64_t timestamp = 1234567000,
                        HANDLE device = reinterpret_cast<HANDLE>(uintptr_t(0x12340)))
{
    submit.enabled = true;
    submit.handle = device;
    submit.bytes = au; submit.size = bytes; submit.timestamp = timestamp;
    submit.status = status;
    const unsigned before = submit.calls;
    const BC_STATUS result = DtsRawFrameSubmit(b.value, submit.handle, submit.timestamp);
    Check(submit.calls == before+1, "one Submit invokes input exactly once");
    submit.enabled = false;
    return result;
}

static void ErrorOutputs(BC_STATUS actual, BC_STATUS expected, const uint8_t *const &au, const uint32_t &bytes,
                          const char *message)
{
    Check(actual == expected && au == reinterpret_cast<const uint8_t *>(uintptr_t(0x4321)) &&
          bytes == 0xdeadbeefU, message);
}

static void Validation()
{
    Check(BC_RAW_FRAME_WIDTH == width && BC_RAW_FRAME_HEIGHT == height &&
          BC_RAW_FRAME_FPS == 30 && BC_RAW_FRAME_MASK_BYTES == 96 &&
          BC_RAW_FRAME_AU_CAPACITY == 65536, "public bounded profile constants");
    Check(DtsRawFrameCreate(nullptr) == BC_STS_INV_ARG, "NULL create output");
    BC_RAW_FRAME_BUILDER *bad = reinterpret_cast<BC_RAW_FRAME_BUILDER *>(uintptr_t(0x123));
    Check(DtsRawFrameCreate(&bad) == BC_STS_INV_ARG && uintptr_t(bad) == 0x123,
          "Create cannot overwrite existing caller handle");
    Check(DtsRawFrameDestroy(nullptr) == BC_STS_INV_ARG, "NULL destroy output");
    BC_RAW_FRAME_BUILDER *none = nullptr;
    Check(DtsRawFrameDestroy(&none) == BC_STS_SUCCESS && !none, "destroy NULL is idempotent");
    fail_calloc = true;
    Check(DtsRawFrameCreate(&none) == BC_STS_INSUFF_RES && !none,
          "single context allocation failure preserves NULL handle");
    fail_calloc = false;
    Check(calloc_failures == 1, "deterministic context OOM hook actually fires");

    Builder b;
    BC_RAW_FRAME_BUILDER *other = b.value;
    Check(DtsRawFrameCreate(&other) == BC_STS_INV_ARG && other == b.value,
          "Create preserves a live output handle");
    Planes source(Source(0), 7);
    const Bytes originals[] = {source.storage[0], source.storage[1], source.storage[2]};
    uint8_t mask[96] = {};
    const uint8_t *au = reinterpret_cast<const uint8_t *>(uintptr_t(0x4321));
    uint32_t bytes = 0xdeadbeefU;
    ErrorOutputs(DtsRawFramePrepareUpload(nullptr, 0, &source.view, &au, &bytes),
                 BC_STS_INV_ARG, au, bytes, "NULL builder preserves output sentinels");
    ErrorOutputs(DtsRawFramePrepareCompose(nullptr, mask, 96, &au, &bytes),
                 BC_STS_INV_ARG, au, bytes, "NULL compose builder preserves output sentinels");
    Check(DtsRawFrameFinish(nullptr, BC_STS_SUCCESS) == BC_STS_INV_ARG &&
          DtsRawFrameDiscard(nullptr) == BC_STS_INV_ARG &&
          DtsRawFrameAbort(nullptr) == BC_STS_INV_ARG &&
          DtsRawFrameSubmit(nullptr, reinterpret_cast<HANDLE>(uintptr_t(0x12340)), 1) == BC_STS_INV_ARG,
          "NULL builder rejected by every control/Submit operation");
    // Conventional opaque C ownership: only NULL or an owned live handle may
    // be passed to operations. Forged/stale/nonowned non-NULL tokens are caller
    // UB, not a promised safe-rejection interface. The non-NULL Create output
    // refusal above is safe because Create never dereferences that token.
    ErrorOutputs(DtsRawFramePrepareUpload(b.value, 0, nullptr, &au, &bytes),
                 BC_STS_INV_ARG, au, bytes, "NULL planes rejected");
    Check(DtsRawFramePrepareUpload(b.value, 0, &source.view, nullptr, &bytes) == BC_STS_INV_ARG &&
          bytes == 0xdeadbeefU, "NULL AU output preserves length");
    Check(DtsRawFramePrepareUpload(b.value, 0, &source.view, &au, nullptr) == BC_STS_INV_ARG &&
          au == reinterpret_cast<const uint8_t *>(uintptr_t(0x4321)), "NULL byte output preserves pointer");
    ErrorOutputs(Compose(b, mask, 96, &au, &bytes), BC_STS_ERR_USAGE, au, bytes,
                 "compose before sources refused");
    ErrorOutputs(Upload(b, 1, source, &au, &bytes), BC_STS_ERR_USAGE, au, bytes,
                 "first source must be LT0");
    ErrorOutputs(Upload(b, 2, source, &au, &bytes), BC_STS_INV_ARG, au, bytes,
                 "out-of-range upload slot refused");
    for (unsigned p = 0; p < 3; ++p) {
        const BC_RAW_FRAME_PLANES original = source.view;
        source.view.planes[p] = nullptr;
        ErrorOutputs(Upload(b, 0, source, &au, &bytes), BC_STS_INV_ARG, au, bytes, "NULL sample plane");
        source.view = original; source.view.strides[p] = (p ? uv_width : width)-1;
        ErrorOutputs(Upload(b, 0, source, &au, &bytes), BC_STS_INV_ARG, au, bytes, "stride shorter than active row");
        source.view = original; --source.view.plane_bytes[p];
        ErrorOutputs(Upload(b, 0, source, &au, &bytes), BC_STS_INV_ARG, au, bytes, "last active sample beyond extent");
        source.view = original; source.view.strides[p] = UINT32_MAX; source.view.plane_bytes[p] = UINT32_MAX;
        ErrorOutputs(Upload(b, 0, source, &au, &bytes), BC_STS_INV_ARG, au, bytes, "row extent arithmetic cannot wrap");
        source.view = original; source.view.planes[p] = reinterpret_cast<const uint8_t *>(UINTPTR_MAX-3);
        ErrorOutputs(Upload(b, 0, source, &au, &bytes), BC_STS_INV_ARG, au, bytes, "plane end address cannot wrap");
        source.view = original;
    }
    Check(Upload(b, 0, source, &au, &bytes) == BC_STS_SUCCESS && au && bytes &&
          bytes <= BC_RAW_FRAME_AU_CAPACITY, "padded/unaligned caller source accepted");
    const uint8_t *const stable = au;
    const uint32_t stable_size = bytes;
    const Bytes saved(au, au+bytes);
    const uint8_t *out = reinterpret_cast<const uint8_t *>(uintptr_t(0x4321));
    uint32_t count = 0xdeadbeefU;
    ErrorOutputs(Upload(b, 0, source, &out, &count), BC_STS_BUSY, out, count,
                 "pending upload blocks a second prepare without changing outputs");
    ErrorOutputs(Compose(b, mask, 96, &out, &count), BC_STS_BUSY, out, count,
                 "pending upload blocks compose without changing outputs");
    for (unsigned p = 0; p < 3; ++p)
        Check(source.storage[p] == originals[p], "upload never changes caller sample/padding bytes");
    for (unsigned p = 0; p < 3; ++p) std::fill(source.storage[p].begin(), source.storage[p].end(), 0);
    Check(au == stable && bytes == stable_size && Bytes(au, au+bytes) == saved,
          "pending AU owns immutable copied sample bytes after caller mutation");
    Check(DtsRawFrameDiscard(b.value) == BC_STS_SUCCESS, "discard before any transmission");
    const BC_RAW_FRAME_PLANES caller_view = source.view;
    source.view.planes[0] = stable;
    source.view.strides[0] = width; source.view.plane_bytes[0] = y_bytes;
    ErrorOutputs(Upload(b, 0, source, &out, &count), BC_STS_INV_ARG, out, count,
                 "caller source cannot alias the builder's AU allocation");
    source.view = caller_view;
    Check(DtsRawFrameFinish(b.value, BC_STS_SUCCESS) == BC_STS_ERR_USAGE,
          "cannot finish a discarded/nonpending AU");
    Parser parser;
    Check(Upload(b, 0, source, &au, &bytes) == BC_STS_SUCCESS,
          "discard preserves initial-IDR/frame-num transaction state");
    Check(parser.Read(Bytes(au, au+bytes), true, 0, nullptr) == Bytes(frame_bytes, 0),
          "reprepared source changes samples but remains initial frame0");
    Check(Accept(b, au, bytes) == BC_STS_SUCCESS, "successful actual Submit commits initial LT0");
    out = reinterpret_cast<const uint8_t *>(uintptr_t(0x4321)); count = 0xdeadbeefU;
    ErrorOutputs(Upload(b, 0, source, &out, &count), BC_STS_ERR_USAGE, out, count,
                 "cannot replace LT0 before acquiring LT1");
    Planes second(Source(1));
    Check(Upload(b, 1, second, &au, &bytes) == BC_STS_SUCCESS &&
          parser.Read(Bytes(au, au+bytes), true, 1, nullptr) == Source(1), "second upload acquires LT1 without reset");
    Check(DtsRawFrameFinish(b.value, BC_STS_SUCCESS) == BC_STS_SUCCESS,
          "manual Finish commits caller-reported successful input");
    for (uint32_t size : {0U, 95U, 97U, UINT32_MAX})
        ErrorOutputs(Compose(b, mask, size, &out, &count), BC_STS_INV_ARG, out, count,
                     "exact96-entry mask extent required");
    ErrorOutputs(Compose(b, nullptr, 96, &out, &count), BC_STS_INV_ARG, out, count, "NULL mask rejected");
    Check(Compose(b, mask, 96, nullptr, &count) == BC_STS_INV_ARG && count == 0xdeadbeefU &&
          Compose(b, mask, 96, &out, nullptr) == BC_STS_INV_ARG &&
          out == reinterpret_cast<const uint8_t *>(uintptr_t(0x4321)),
          "NULL compose outputs leave other caller output unchanged");
    for (unsigned index : {0U, 47U, 95U}) {
        for (uint8_t bad_value : {uint8_t(2), uint8_t(255)}) {
            mask[index] = bad_value;
            ErrorOutputs(Compose(b, mask, 96, &out, &count), BC_STS_INV_ARG, out, count,
                         "invalid tile reference index rejected at every boundary");
        }
        mask[index] = 0;
    }
    Check(Compose(b, mask, 96, &au, &bytes) == BC_STS_SUCCESS,
          "all-LT0 mask is legal despite two active references");
    const Bytes pending(au, au+bytes);
    std::fill(mask, mask+96, 1);
    Check(Bytes(au, au+bytes) == pending, "pending mask encoded independently of caller mutation");
    Check(parser.Read(pending, false, 0, std::vector<uint8_t>(96, 0).data()) == Bytes(frame_bytes, 0),
          "all-LT0 mask parses all96 references and zero vectors");
    Check(DtsRawFrameDiscard(b.value) == BC_STS_SUCCESS,
          "discard composition does not consume a frame number");
    Check(Compose(b, mask, 96, &au, &bytes) == BC_STS_SUCCESS,
          "all-LT1 composition after discard remains legal");
    // The reader already inspected a discarded frame2, so restore its logical
    // acceptance index; production state never advanced for that discarded AU.
    --parser.frame;
    Check(parser.Read(Bytes(au, au+bytes), false, 0, mask) == Source(1),
          "reprepared mask remains global frame2 and uses LT1");
    Check(Accept(b, au, bytes) == BC_STS_SUCCESS, "successful compose Submit commits");
    Check(DtsRawFrameAbort(b.value) == BC_STS_SUCCESS, "abort after successful prior transmit poisons channel model");
    ErrorOutputs(Upload(b, 0, second, &out, &count), BC_STS_IO_USER_ABORT, out, count,
                 "poison prevents future uploads");
    ErrorOutputs(Compose(b, mask, 96, &out, &count), BC_STS_IO_USER_ABORT, out, count,
                 "poison prevents future compositions");
    Check(DtsRawFrameDestroy(&b.value) == BC_STS_SUCCESS && !b.value, "destroy consumes and clears handle");
    Check(DtsRawFrameDestroy(&b.value) == BC_STS_SUCCESS && !b.value,
          "consumed cleared handle can be destroyed idempotently");
}

static void SubmitFailures()
{
    for (BC_STATUS status : {BC_STS_BUSY, BC_STS_IO_ERROR, BC_STS_IO_USER_ABORT,
                            BC_STS_INV_ARG, BC_STS_TIMEOUT, BC_STS_INSUFF_RES}) {
        Builder b;
        Planes p(Source(0));
        const uint8_t *au = nullptr; uint32_t bytes = 0;
        Check(Upload(b, 0, p, &au, &bytes) == BC_STS_SUCCESS, "prepare before scripted submit failure");
        Check(Accept(b, au, bytes, status) == status, "Submit returns actual input failure unchanged");
        const uint8_t *out = reinterpret_cast<const uint8_t *>(uintptr_t(0x4321));
        uint32_t count = 0xdeadbeefU;
        ErrorOutputs(Upload(b, 0, p, &out, &count), BC_STS_IO_USER_ABORT, out, count,
                     "every nonsuccess input result permanently poisons builder");
        const unsigned calls = submit.calls;
        Check(DtsRawFrameSubmit(b.value, reinterpret_cast<HANDLE>(uintptr_t(0x12340)), 123) == BC_STS_IO_USER_ABORT &&
              submit.calls == calls, "poison prevents retry of partially accepted input");
    }
    for (BC_STATUS status : {BC_STS_BUSY, BC_STS_IO_ERROR, BC_STS_IO_USER_ABORT}) {
        Builder b; Planes p(Source(0)); const uint8_t *au = nullptr; uint32_t bytes = 0;
        Check(Upload(b, 0, p, &au, &bytes) == BC_STS_SUCCESS, "prepare before manual Finish failure");
        Check(DtsRawFrameFinish(b.value, status) == status, "manual Finish preserves actual nonsuccess result");
        Check(DtsRawFrameDiscard(b.value) == BC_STS_IO_USER_ABORT,
              "Discard cannot recover poisoned potentially transmitted prefix");
    }
    Builder b; Planes p(Source(0)); const uint8_t *au = nullptr; uint32_t bytes = 0;
    Check(DtsRawFrameFinish(b.value, BC_STS_IO_ERROR) == BC_STS_ERR_USAGE,
          "nonpending Finish cannot invent an actual submission or poison state");
    Check(DtsRawFrameSubmit(b.value, reinterpret_cast<HANDLE>(uintptr_t(0x12340)), 1) == BC_STS_ERR_USAGE,
          "Submit requires a pending AU");
    Check(Upload(b, 0, p, &au, &bytes) == BC_STS_SUCCESS, "prepare prior to direct Abort");
    Check(DtsRawFrameAbort(b.value) == BC_STS_SUCCESS && DtsRawFrameAbort(b.value) == BC_STS_SUCCESS,
          "explicit Abort is idempotent");
    for (uint64_t token : {uint64_t(0), UINT64_MAX}) {
        Builder fresh;
        Check(Upload(fresh, 0, p, &au, &bytes) == BC_STS_SUCCESS &&
              Accept(fresh, au, bytes, BC_STS_SUCCESS, token) == BC_STS_SUCCESS,
              "Submit preserves complete caller64-bit token including zero/max");
    }
    Builder invalid_device;
    Check(Upload(invalid_device, 0, p, &au, &bytes) == BC_STS_SUCCESS &&
          Accept(invalid_device, au, bytes, BC_STS_INV_ARG, 1, nullptr) == BC_STS_INV_ARG &&
          DtsRawFrameDiscard(invalid_device.value) == BC_STS_IO_USER_ABORT,
          "actual invalid-device input return also poisons rather than retrying");
}

struct Stream { Bytes coded, planar; unsigned maximum_au = 0; };
static void Append(Bytes *target, const Bytes &part) { target->insert(target->end(), part.begin(), part.end()); }
static Stream Checker(bool mirror)
{
    Builder b; Parser parser; Stream stream;
    const Bytes a = Source(0), original_b = Source(1), c = Source(2);
    for (unsigned f = 0; f < 180; ++f) {
        const bool upload = f == 0 || f == 1 || f == 90;
        const unsigned slot = f == 1 ? 1 : (f == 90 && mirror ? 1 : 0);
        const Bytes &source = f == 0 ? a : (f == 1 ? original_b : c);
        uint8_t mask[96];
        for (unsigned mb = 0; mb < 96; ++mb) mask[mb] = static_cast<uint8_t>((mb%16+mb/16+f%2)&1);
        const uint8_t *au = nullptr; uint32_t count = 0;
        if (upload) {
            Planes p(source, f == 90 ? 13 : 0);
            Check(Upload(b, slot, p, &au, &count) == BC_STS_SUCCESS, "checker source upload through public API");
        } else Check(Compose(b, mask, 96, &au, &count) == BC_STS_SUCCESS, "checker tile composition through public API");
        Require(au && count && count <= 65536, "bounded complete checker AU");
        const Bytes encoded(au, au+count);
        const Bytes expected = upload ? source : Assemble(
            f < 90 || mirror ? a : c, f < 90 || !mirror ? original_b : c, mask);
        Check(parser.Read(encoded, upload, slot, mask) == expected,
              "independent parser/ref lifecycle produces complete literal checker planes");
        Check(Accept(b, au, count) == BC_STS_SUCCESS, "checker actual-status Submit advances one frame");
        Append(&stream.coded, encoded); Append(&stream.planar, expected);
        stream.maximum_au = std::max(stream.maximum_au, count);
    }
    Check(parser.frame == 180 && (parser.frame-1)%16 == 3 && 2*(parser.frame-1) == 358,
          "all180 frames/global11 wraps/final frame_num3/POC358");
    Check(stream.planar.size() == 6635520, "complete180-frame planar byte extent");
    // Exactly seven independently distinct images: pure A/B/C plus two
    // checker phases before and after replacement; no final frames discarded.
    std::vector<Bytes> distinct;
    std::vector<unsigned> counts;
    for (unsigned f = 0; f < 180; ++f) {
        Bytes view(stream.planar.begin()+f*frame_bytes, stream.planar.begin()+(f+1)*frame_bytes);
        const auto found = std::find(distinct.begin(), distinct.end(), view);
        if (found == distinct.end()) { distinct.push_back(view); counts.push_back(1); }
        else ++counts[static_cast<size_t>(found-distinct.begin())];
    }
    std::sort(counts.begin(), counts.end());
    Check(distinct.size() == 7 && counts == std::vector<unsigned>({1, 1, 1, 44, 44, 44, 45}),
          "exact seven complete views and expected repeat counts");
    std::printf("%s: frames=180 I=0,1,90 key=0 P=177 bytes=%zu maxAU=%u planar=%zu\n",
                mirror ? "mirror-slot1" : "checker", stream.coded.size(), stream.maximum_au, stream.planar.size());
    return stream;
}

static Stream Extreme()
{
    Builder b; Parser parser; Stream stream;
    Bytes slots[] = {Bytes(frame_bytes, 0), Bytes(frame_bytes, 255)};
    for (unsigned f = 0; f < 6; ++f) {
        const bool upload = f < 2 || f == 4;
        unsigned slot = f == 1 ? 1 : 0;
        if (f == 4) {
            for (size_t at = 0; at < frame_bytes; ++at)
                slots[0][at] = static_cast<uint8_t>((at % 7 < 3) ? 0 : (at % 7 == 3 ? 3 : 255));
        }
        uint8_t mask[96];
        for (unsigned mb = 0; mb < 96; ++mb)
            mask[mb] = static_cast<uint8_t>((mb*17 + (mb>>2) + f)%2);
        const uint8_t *au = nullptr; uint32_t count = 0;
        if (upload) {
            Planes p(slots[slot], 31);
            Check(Upload(b, slot, p, &au, &count) == BC_STS_SUCCESS, "full-range caller source upload");
        } else Check(Compose(b, mask, 96, &au, &count) == BC_STS_SUCCESS, "arbitrary nonchecker mask accepted");
        Require(au && count && count <= BC_RAW_FRAME_AU_CAPACITY, "extreme escape output remains bounded");
        if (!f) Check(count > 49152, "all-zero source exercises AU above old private48KiB cap");
        const Bytes encoded(au, au+count);
        const Bytes expected = upload ? slots[slot] : Assemble(slots[0], slots[1], mask);
        Check(parser.Read(encoded, upload, slot, mask) == expected,
              "all0/255/escape-boundary samples and arbitrary masks preserved exactly");
        Check(Accept(b, au, count) == BC_STS_SUCCESS, "extreme stream Submit succeeds");
        Append(&stream.coded, encoded); Append(&stream.planar, expected);
        stream.maximum_au = std::max(stream.maximum_au, count);
    }
    std::printf("extreme: frames=6 maxAU=%u full-range samples retained\n", stream.maximum_au);
    return stream;
}

static void Emit(const std::string &directory, const char *name, const Stream &stream)
{
    const std::string base = directory+"/"+name;
    WriteExclusive(base+".h264", stream.coded);
    WriteExclusive(base+".yuv420", stream.planar);
    WriteExclusive(base+".output-chain.yuy2", OutputChainYuy2(stream.planar));
}

int main(int argc, char **argv)
{
    if (argc != 1 && (argc != 3 || std::strcmp(argv[1], "--emit"))) {
        std::fprintf(stderr, "usage: %s [--emit NEW_OUTPUT_DIRECTORY]\n", argv[0]); return 2;
    }
    try {
        if (argc == 3) Require(mkdir(argv[2], 0700) == 0, "refuse existing output directory");
        Validation(); SubmitFailures();
        const Stream checker = Checker(false), mirror = Checker(true), extreme = Extreme();
        if (argc == 3) {
            Emit(argv[2], "checker", checker); Emit(argv[2], "mirror", mirror); Emit(argv[2], "extreme", extreme);
        }
    } catch (const char *message) { Check(false, message); }
    if (failures) { std::fprintf(stderr, "%u/%u raw-frame checks failed\n", failures, checks); return 1; }
    std::printf("PASS: %u public raw-frame API, transactional Submit, syntax/source/tile and finite-output checks\n", checks);
    return 0;
}
