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

// Literal 8-bit H.264 interpolation, independent of the AU serializer. Source
// coordinates are x+qx/4,y+qy/4; positive vectors move visible content left/up.
// Every tap is clamped before filtering; signed diagonal intermediates are not
// rounded or clipped between passes. C++ division is corrected to floor.
static int FloorDiv(int n, int denominator)
{
    const int quotient = n / denominator;
    return quotient - (n % denominator < 0);
}
static int Clip8(int n) { return std::max(0, std::min(255, n)); }
static int Mean(int a, int b) { return (a+b+1)/2; }
struct MotionSampler {
    const Bytes &source;
    explicit MotionSampler(const Bytes &bytes) : source(bytes) {
        Require(source.size() == frame_bytes, "complete source for MC");
    }
    int At(int x, int y) const {
        return source[std::max(0, std::min(95, y))*256 + std::max(0, std::min(255, x))];
    }
    int HorizontalRaw(int x, int y) const {
        return At(x-2,y)-5*At(x-1,y)+20*At(x,y)+20*At(x+1,y)-5*At(x+2,y)+At(x+3,y);
    }
    int Horizontal(int x, int y) const { return Clip8(FloorDiv(HorizontalRaw(x,y)+16,32)); }
    int Vertical(int x, int y) const {
        return Clip8(FloorDiv(At(x,y-2)-5*At(x,y-1)+20*At(x,y)+20*At(x,y+1)-5*At(x,y+2)+At(x,y+3)+16,32));
    }
    int Diagonal(int x, int y) const {
        const int total = HorizontalRaw(x,y-2)-5*HorizontalRaw(x,y-1)+20*HorizontalRaw(x,y)+
            20*HorizontalRaw(x,y+1)-5*HorizontalRaw(x,y+2)+HorizontalRaw(x,y+3);
        return Clip8(FloorDiv(total+512,1024));
    }
    uint8_t Luma(int x, int y, int qx, int qy) const {
        const int ix = FloorDiv(4*x+qx,4), iy = FloorDiv(4*y+qy,4);
        const int phase = (4*y+qy-4*iy)*4+(4*x+qx-4*ix);
        int value = 0;
        switch (phase) {
        case 0: value=At(ix,iy); break;
        case 1: value=Mean(At(ix,iy),Horizontal(ix,iy)); break;
        case 2: value=Horizontal(ix,iy); break;
        case 3: value=Mean(Horizontal(ix,iy),At(ix+1,iy)); break;
        case 4: value=Mean(At(ix,iy),Vertical(ix,iy)); break;
        case 5: value=Mean(Horizontal(ix,iy),Vertical(ix,iy)); break;
        case 6: value=Mean(Horizontal(ix,iy),Diagonal(ix,iy)); break;
        case 7: value=Mean(Horizontal(ix,iy),Vertical(ix+1,iy)); break;
        case 8: value=Vertical(ix,iy); break;
        case 9: value=Mean(Vertical(ix,iy),Diagonal(ix,iy)); break;
        case 10: value=Diagonal(ix,iy); break;
        case 11: value=Mean(Diagonal(ix,iy),Vertical(ix+1,iy)); break;
        case 12: value=Mean(Vertical(ix,iy),At(ix,iy+1)); break;
        case 13: value=Mean(Vertical(ix,iy),Horizontal(ix,iy+1)); break;
        case 14: value=Mean(Diagonal(ix,iy),Horizontal(ix,iy+1)); break;
        case 15: value=Mean(Vertical(ix+1,iy),Horizontal(ix,iy+1)); break;
        }
        return static_cast<uint8_t>(value);
    }
    int ChromaAt(size_t base, int x, int y) const {
        return source[base+std::max(0,std::min(47,y))*128+std::max(0,std::min(127,x))];
    }
    uint8_t Chroma(size_t base, int x, int y, int qx, int qy) const {
        const int ix=FloorDiv(8*x+qx,8), iy=FloorDiv(8*y+qy,8);
        const int fx=8*x+qx-8*ix, fy=8*y+qy-8*iy;
        const int total=(8-fx)*(8-fy)*ChromaAt(base,ix,iy)+fx*(8-fy)*ChromaAt(base,ix+1,iy)+
            (8-fx)*fy*ChromaAt(base,ix,iy+1)+fx*fy*ChromaAt(base,ix+1,iy+1);
        return static_cast<uint8_t>((total+32)/64);
    }
};
static Bytes Motion(const Bytes &source, int qx, int qy)
{
    MotionSampler m(source);
    Bytes result(frame_bytes);
    for (unsigned y=0;y<height;++y)
        for (unsigned x=0;x<width;++x) result[y*width+x]=m.Luma(x,y,qx,qy);
    for (size_t base : {y_bytes, y_bytes+uv_bytes})
        for (unsigned y=0;y<uv_height;++y)
            for (unsigned x=0;x<uv_width;++x) result[base+y*uv_width+x]=m.Chroma(base,x,y,qx,qy);
    return result;
}

struct Vector { int x, y; };
struct Neighbour { int ref, x, y; bool available; };
static Vector Predict(const Neighbour &a, const Neighbour &b, const Neighbour &topright,
                      const Neighbour &topleft, unsigned target, unsigned *case_index = nullptr)
{
    const Neighbour c = topright.available ? topright : topleft;
    const Neighbour values[] = {a,b,c};
    unsigned matches=0, match=0;
    for (unsigned i=0;i<3;++i)
        if (values[i].available && values[i].ref==static_cast<int>(target)) { ++matches; match=i; }
    if (matches==1) {
        if (case_index) *case_index=1;
        return Vector{values[match].x,values[match].y};
    }
    if (!b.available && !c.available && a.available) {
        if (case_index) *case_index=3;
        return Vector{a.x,a.y};
    }
    int xs[3],ys[3];
    for (unsigned i=0;i<3;++i) { xs[i]=values[i].available ? values[i].x : 0; ys[i]=values[i].available ? values[i].y : 0; }
    std::sort(xs,xs+3); std::sort(ys,ys+3);
    if (case_index) *case_index=matches ? 2 : 0;
    return Vector{xs[1],ys[1]};
}
static Neighbour CacheAt(const std::vector<Neighbour> &cache,int x,int y)
{
    return x>=0 && x<64 && y>=0 && y<24 ? cache[y*64+x] : Neighbour{-2,0,0,false};
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
    unsigned non_reference_frames = 0, poc = 0;
    bool loaded[2] = {false, false};
    Bytes slots[2];
    size_t frame_bit=0,active_count_bit=0,slot_bit=0;
    std::vector<size_t> motion_marks;
    Bytes Read(const Bytes &au, bool upload, unsigned slot, const uint8_t *mask,
               bool translation = false, int qx = 0, int qy = 0, bool non_reference = false) {
        const std::vector<Nalu> ns = Nalus(au);
        Require(ns.size() == (frame ? 2U : 4U), "exact AU NAL count");
        Require(ns[0].header == 9 && ns[0].rbsp == Bytes{uint8_t(upload ? 0x10 : 0x30)}, "matching AUD");
        size_t vcl = 1;
        if (!frame) {
            Require(upload && slot == 0 && ns[1].header == 0x67 && ns[2].header == 0x68, "initial slot0 IDR headers");
            Sps(ns[1].rbsp); Pps(ns[2].rbsp); vcl = 3;
        }
        Require(!non_reference || (!upload && frame >= 2), "NR picture follows both source uploads");
        Require(ns[vcl].header == (non_reference ? 0x01U : frame ? 0x21U : 0x65U),
                "exact reference/non-reference VCL header/IDR only first");
        Reader r(ns[vcl].rbsp);
        Require(r.UE() == 0 && r.UE() == (upload ? 2U : 0U) && r.UE() == 0, "one whole-picture slice");
        frame_bit=r.bit; motion_marks.clear();
        Require(r.U(4) == (frame-non_reference_frames) % 16, "reference-only frame_num including replacement");
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
            Require(r.U(1) == 1,"explicit active-count override");
            active_count_bit=r.bit;
            Require(r.UE() == (translation ? 0U : 1U) && r.U(1) == 1,
                    "operation-specific active L0 count/explicit reordering");
            Require(r.UE() == 2,"long-term reorder operation");
            slot_bit=r.bit;
            Require(r.UE() == (translation ? slot : 0U), "first selected LT picture");
            if (!translation) Require(r.UE() == 2 && r.UE() == 1, "compose second reference LT1");
            Require(r.UE() == 3, "explicit L0 reorder end");
            if (!non_reference) Require(r.U(1) == 0, "reference P sliding marking");
            Require(r.SE() == 0 && r.UE() == 1, "zero QP/deblock disabled, NR marking absent");
            std::vector<Neighbour> cache(64*24, Neighbour{-2,0,0,false});
            unsigned cases[4]={0,0,0,0}, fallbacks=0;
            for (unsigned mb = 0; mb < 96; ++mb) {
                Require(r.UE() == 0 && r.UE() == 0, "no skips, P_L0_16x16");
                // Selected LTslot1 is still active L0 index0 with one reference.
                const unsigned ref = translation ? 0 : 1-r.U(1);
                Require(translation || (mask && ref == mask[mb]), "caller tile reference mask preserved");
                const int bx=4*(mb%16),by=4*(mb/16);
                const Neighbour a=CacheAt(cache,bx-1,by),b=CacheAt(cache,bx,by-1);
                const Neighbour c=CacheAt(cache,bx+4,by-1),d=CacheAt(cache,bx-1,by-1);
                unsigned kind=0;
                const Vector prediction=Predict(a,b,c,d,ref,&kind);
                ++cases[kind]; fallbacks+=!c.available;
                motion_marks.push_back(r.bit); const int dx=r.SE();
                motion_marks.push_back(r.bit); const int dy=r.SE();
                const Vector inferred={prediction.x+dx,prediction.y+dy};
                Require(inferred.x==(translation ? qx : 0) && inferred.y==(translation ? qy : 0),
                        "all96 independently predicted absolute motion vectors");
                Require(r.UE() == 0, "no residual");
                for (int y=by;y<by+4;++y)
                    for (int x=bx;x<bx+4;++x) {
                        Require(!cache[y*64+x].available,"each cache location assigned once");
                        cache[y*64+x]=Neighbour{static_cast<int>(ref),inferred.x,inferred.y,true};
                    }
            }
            if (translation) {
                Require(cases[0]==1 && cases[1]==15 && cases[2]==80 && cases[3]==0 && fallbacks==21,
                        "complete uniform-vector predictor cases including every row start");
                decoded=Motion(slots[slot],qx,qy);
            } else decoded = Assemble(slots[0], slots[1], mask);
            if (!non_reference) short_count = 1; // Sliding window retains both long sources.
        }
        Require(unsigned(loaded[0])+unsigned(loaded[1])+short_count <= 3, "finite reference capacity");
        poc = 2*(frame-non_reference_frames) - unsigned(non_reference);
        r.End(); ++frame;
        non_reference_frames += unsigned(non_reference);
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
static BC_STATUS Translate(Builder &b, unsigned slot, int qx, int qy,
                           const uint8_t **au, uint32_t *bytes)
{
    return DtsRawFramePrepareTranslate(b.value,slot,qx,qy,au,bytes);
}
static BC_STATUS ComposeNonReference(Builder &b, const uint8_t *mask, uint32_t count,
                                     const uint8_t **au, uint32_t *bytes)
{
    return DtsRawFramePrepareComposeNonReference(b.value,mask,count,au,bytes);
}
static BC_STATUS TranslateNonReference(Builder &b, unsigned slot, int qx, int qy,
                                       const uint8_t **au, uint32_t *bytes)
{
    return DtsRawFramePrepareTranslateNonReference(b.value,slot,qx,qy,au,bytes);
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

static Bytes FlipVclBit(const Bytes &au,size_t bit)
{
    std::vector<Nalu> ns=Nalus(au);
    Require(bit<ns.back().rbsp.size()*8,"mutation selects existing VCL bit");
    ns.back().rbsp[bit/8]^=static_cast<uint8_t>(1U<<(7-bit%8));
    Bytes result;
    for (const Nalu &n : ns) {
        const uint8_t prefix[]={0,0,0,1,static_cast<uint8_t>(n.header)};
        result.insert(result.end(),prefix,prefix+5);
        unsigned zeros=0;
        for (uint8_t byte : n.rbsp) {
            if (zeros==2 && byte<=3) { result.push_back(3); zeros=0; }
            result.push_back(byte); zeros=byte==0 ? zeros+1 : 0;
        }
    }
    return result;
}
static void RejectTranslation(const Parser &before,const Bytes &au,unsigned slot,int qx,int qy)
{
    bool rejected=false;
    try { Parser copy=before; copy.Read(au,false,slot,nullptr,true,qx,qy); }
    catch (const char *) { rejected=true; }
    Check(rejected,"translation syntax/MV/source adversary rejected by complete independent parser");
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

static void PredictorTests()
{
    const Neighbour absent={-2,0,0,false};
    for (unsigned target=0;target<2;++target) {
        const int other=1-static_cast<int>(target);
        const Neighbour a={static_cast<int>(target),7,-9,true};
        const Neighbour b={other,31,41,true},c={other,-15,23,true};
        const Neighbour d={static_cast<int>(target),-11,19,true};
        Vector p=Predict(a,b,c,d,target);
        Check(p.x==7 && p.y==-9,"single matching reference chooses nonzero left vector");
        p=Predict(b,a,c,d,target);
        Check(p.x==7 && p.y==-9,"single matching reference chooses nonzero top vector");
        p=Predict(b,c,absent,d,target);
        Check(p.x==-11 && p.y==19,"unavailable top-right falls back to nonzero top-left");
        p=Predict(a,absent,absent,absent,target);
        Check(p.x==7 && p.y==-9,"top unavailable uses nonzero available left");
        p=Predict(b,absent,absent,absent,target);
        Check(p.x==31 && p.y==41,"no matching reference still uses left if both top neighbours unavailable");
        const Neighbour all[]={{other,29,-7,true},{other,-13,41,true},{other,5,9,true}};
        p=Predict(all[0],all[1],all[2],absent,target);
        Check(p.x==5 && p.y==9,"zero matches use component-wise nonzero median");
        p=Predict(Neighbour{static_cast<int>(target),29,-7,true},
                  Neighbour{static_cast<int>(target),-13,41,true},all[2],absent,target);
        Check(p.x==5 && p.y==9,"two matches use all three vectors' component median");
        p=Predict(Neighbour{static_cast<int>(target),29,-7,true},
                  Neighbour{static_cast<int>(target),-13,41,true},
                  Neighbour{static_cast<int>(target),5,9,true},absent,target);
        Check(p.x==5 && p.y==9,"three matches use component-wise nonzero median");
    }
    Check(FloorDiv(-1,4)==-1 && -1-4*FloorDiv(-1,4)==3 &&
          FloorDiv(-3,4)==-1 && -3-4*FloorDiv(-3,4)==1 &&
          FloorDiv(-1,8)==-1 && -1-8*FloorDiv(-1,8)==7 &&
          FloorDiv(-3,8)==-1 && -3-8*FloorDiv(-3,8)==5,
          "negative qpel/chroma8pel floor and fractional coordinates");
    Check(FloorDiv(-33,32)==-2 && FloorDiv(-1025,1024)==-2 &&
          Clip8(-1)==0 && Clip8(256)==255 && Mean(100,101)==101,
          "signed filter rounding, full-range clipping and quarter ties-up");
}

static void InterpolationTests()
{
    Bytes step(frame_bytes,0);
    for (unsigned y=0;y<height;++y)
        for (unsigned x=10;x<width;++x) step[y*width+x]=255;
    for (size_t base : {y_bytes,y_bytes+uv_bytes})
        for (unsigned y=0;y<uv_height;++y)
            for (unsigned x=10;x<uv_width;++x) step[base+y*uv_width+x]=255;
    MotionSampler m(step);
    Check(m.Luma(9,20,2,0)==128 && m.Luma(9,20,1,0)==64 && m.Luma(9,20,3,0)==192,
          "literal horizontal six-tap half sample and both quarter neighbours");
    Check(m.Luma(8,20,2,0)==0 && m.Luma(10,20,2,0)==255,
          "six-tap ringing clips at0/255 not limited source range");
    Check(m.Chroma(y_bytes,9,20,2,0)==64 && m.Chroma(y_bytes,10,20,-2,0)==191 &&
          m.Chroma(y_bytes,10,20,-3,0)==159 && m.Chroma(y_bytes,10,20,-1,0)==223,
          "literal eighth-pel chroma and negative floor phases");
    Bytes impulse(frame_bytes,0); impulse[9*width+9]=255;
    MotionSampler diagonal(impulse);
    Check(diagonal.Diagonal(10,10)==6,
          "diagonal retains negative intermediate (clipped-between-passes would incorrectly0)");
    Bytes tie(frame_bytes,0);
    for (unsigned y=0;y<height;++y)
        for (unsigned x=10;x<width;++x) tie[y*width+x]=1;
    for (size_t base : {y_bytes,y_bytes+uv_bytes})
        for (unsigned y=0;y<uv_height;++y)
            for (unsigned x=10;x<uv_width;++x) tie[base+y*uv_width+x]=4;
    MotionSampler ties(tie);
    Check(ties.Luma(9,20,1,0)==1 && ties.Chroma(y_bytes,9,20,1,0)==1,
          "quarter mean+1 and chroma+32 discriminate missing rounding offsets");
    Bytes edge(frame_bytes,0);
    for (unsigned y=0;y<height;++y) edge[y*width]=255;
    MotionSampler boundary(edge);
    Check(boundary.Horizontal(-1,20)==255 && boundary.Horizontal(0,20)==128 &&
          boundary.Luma(0,20,-1,0)==255,
          "each source tap is clamped; clamping base before six-tap would differ");
    for (uint8_t value : {uint8_t(0),uint8_t(255)}) {
        const Bytes constant(frame_bytes,value); MotionSampler flat(constant);
        for (int qy=-3;qy<=3;++qy)
            for (int qx=-3;qx<=3;++qx)
                Check(flat.Luma(0,0,qx,qy)==value && flat.Luma(255,95,qx,qy)==value &&
                      flat.Luma(16,16,qx,qy)==value && flat.Chroma(y_bytes,0,0,qx,qy)==value &&
                      flat.Chroma(y_bytes+uv_bytes,127,47,qx,qy)==value,
                      "all49 poses preserve full-range DC and every edge");
    }
    const Bytes a=Source(0),b=Source(1),c=Source(2);
    Check(Motion(a,1,3)!=Motion(a,-1,-3) && Motion(a,3,1)!=Motion(b,3,1) &&
          Motion(a,-3,3)!=Motion(c,-3,3),"wrong sign or stale/cross-slot source changes complete pixels");
    Check(Motion(Motion(a,-3,-3),-3,-2)!=Motion(a,-3,-2),
          "previous-P cumulative reference differs from immutable long-term source");
}

static void TranslationValidation()
{
    Builder b; Parser parser; Planes a(Source(0)),second(Source(1));
    const uint8_t *out=reinterpret_cast<const uint8_t *>(uintptr_t(0x4321));
    uint32_t count=0xdeadbeefU;
    ErrorOutputs(DtsRawFramePrepareTranslate(nullptr,0,0,0,&out,&count),BC_STS_INV_ARG,out,count,
                 "NULL translation builder preserves outputs");
    Check(Translate(b,0,0,0,nullptr,&count)==BC_STS_INV_ARG && count==0xdeadbeefU &&
          Translate(b,0,0,0,&out,nullptr)==BC_STS_INV_ARG &&
          out==reinterpret_cast<const uint8_t *>(uintptr_t(0x4321)),
          "NULL translation outputs preserve the other output");
    ErrorOutputs(Translate(b,0,0,0,&out,&count),BC_STS_ERR_USAGE,out,count,"translation before sources refused");
    const uint8_t *au=nullptr; uint32_t bytes=0;
    Check(Upload(b,0,a,&au,&bytes)==BC_STS_SUCCESS &&
          parser.Read(Bytes(au,au+bytes),true,0,nullptr)==Source(0) &&
          Accept(b,au,bytes)==BC_STS_SUCCESS,"first translation prerequisite uploads source A");
    ErrorOutputs(Translate(b,0,0,0,&out,&count),BC_STS_ERR_USAGE,out,count,
                 "translation requires both committed slots even when selecting LT0");
    Check(Upload(b,1,second,&au,&bytes)==BC_STS_SUCCESS &&
          parser.Read(Bytes(au,au+bytes),true,1,nullptr)==Source(1) &&
          Accept(b,au,bytes)==BC_STS_SUCCESS,"second translation prerequisite uploads source B");
    for (unsigned slot : {2U,UINT32_MAX})
        ErrorOutputs(Translate(b,slot,0,0,&out,&count),BC_STS_INV_ARG,out,count,"invalid translation source slot");
    for (int bad : {INT_MIN,-4,4,INT_MAX}) {
        ErrorOutputs(Translate(b,0,bad,0,&out,&count),BC_STS_INV_ARG,out,count,"invalid signed horizontal MV");
        ErrorOutputs(Translate(b,1,0,bad,&out,&count),BC_STS_INV_ARG,out,count,"invalid signed vertical MV");
    }
    Check(Translate(b,1,-3,3,&au,&bytes)==BC_STS_SUCCESS,"prepare boundary vector against native LT1");
    const uint8_t *stable=au; const uint32_t stable_count=bytes;
    const Bytes saved(au,au+bytes);
    const Parser before_translation=parser;
    uint8_t mask[96]={};
    ErrorOutputs(Translate(b,0,3,-3,&out,&count),BC_STS_BUSY,out,count,"pending translation blocks translation");
    ErrorOutputs(Compose(b,mask,96,&out,&count),BC_STS_BUSY,out,count,"pending translation blocks compose");
    ErrorOutputs(Upload(b,0,a,&out,&count),BC_STS_BUSY,out,count,"pending translation blocks upload");
    Check(au==stable && bytes==stable_count && Bytes(au,au+bytes)==saved,"failed prepares preserve pending translation bytes");
    Check(parser.Read(saved,false,1,nullptr,true,-3,3)==Motion(Source(1),-3,3),
          "LT1 becomes active L0 index0 and all96 nonzero vectors independently inferred");
    for (size_t bit : {parser.frame_bit,parser.active_count_bit,parser.slot_bit,
                       parser.motion_marks[0],parser.motion_marks[1],parser.motion_marks[32],
                       parser.motion_marks[33],parser.motion_marks[190],parser.motion_marks[191]})
        RejectTranslation(before_translation,FlipVclBit(saved,bit),1,-3,3);
    RejectTranslation(before_translation,saved,0,-3,3);
    RejectTranslation(before_translation,saved,1,3,-3);
    RejectTranslation(before_translation,saved,1,-12,12);
    Bytes truncated=saved; truncated.pop_back();
    RejectTranslation(before_translation,truncated,1,-3,3);
    Check(DtsRawFrameDiscard(b.value)==BC_STS_SUCCESS,"untransmitted translation can be discarded");
    --parser.frame;
    Check(Translate(b,0,0,0,&au,&bytes)==BC_STS_SUCCESS &&
          parser.Read(Bytes(au,au+bytes),false,0,nullptr,true,0,0)==Source(0),
          "discarded translation preserves frame number; zero motion equals uploaded full raster");
    Check(Accept(b,au,bytes,BC_STS_SUCCESS,UINT64_MAX)==BC_STS_SUCCESS,"translation Submit forwards actual pointer and full token");
    Check(Compose(b,mask,96,&au,&bytes)==BC_STS_SUCCESS &&
          parser.Read(Bytes(au,au+bytes),false,0,mask)==Source(0) &&
          Accept(b,au,bytes)==BC_STS_SUCCESS,"translation leaves both LT slots intact for composition");
    for (BC_STATUS status : {BC_STS_BUSY,BC_STS_IO_ERROR,BC_STS_IO_USER_ABORT,
                            BC_STS_INV_ARG,BC_STS_TIMEOUT,BC_STS_INSUFF_RES}) {
        Builder failing; Planes zero(Bytes(frame_bytes,0)),full(Bytes(frame_bytes,255));
        Check(Upload(failing,0,zero,&au,&bytes)==BC_STS_SUCCESS && Accept(failing,au,bytes)==BC_STS_SUCCESS &&
              Upload(failing,1,full,&au,&bytes)==BC_STS_SUCCESS && Accept(failing,au,bytes)==BC_STS_SUCCESS,
              "fresh both-source builder for translation input failure");
        Check(Translate(failing,1,3,-3,&au,&bytes)==BC_STS_SUCCESS &&
              Accept(failing,au,bytes,status)==status,"translation Submit reports every actual nonSUCCESS unchanged");
        ErrorOutputs(Translate(failing,0,0,0,&out,&count),BC_STS_IO_USER_ABORT,out,count,
                     "failed or partial translation poisons future preparation");
        const unsigned calls=submit.calls;
        Check(DtsRawFrameSubmit(failing.value,reinterpret_cast<HANDLE>(uintptr_t(0x12340)),1)==BC_STS_IO_USER_ABORT &&
              submit.calls==calls && DtsRawFrameDiscard(failing.value)==BC_STS_IO_USER_ABORT,
              "failed translation cannot retry or recover through Discard");
    }
    Check(DtsRawFrameAbort(b.value)==BC_STS_SUCCESS,"explicit Abort after successful translation");
    ErrorOutputs(Translate(b,0,0,0,&out,&count),BC_STS_IO_USER_ABORT,out,count,"Abort poisons translations");
}

static void BothSources(Builder &b, Parser *parser = nullptr)
{
    for (unsigned slot=0;slot<2;++slot) {
        Planes planes(Source(slot)); const uint8_t *au=nullptr; uint32_t bytes=0;
        Check(Upload(b,slot,planes,&au,&bytes)==BC_STS_SUCCESS,"NR prerequisite source upload");
        if (parser) Check(parser->Read(Bytes(au,au+bytes),true,slot,nullptr)==Source(slot),
                          "independent NR prerequisite source/ref syntax");
        Check(Accept(b,au,bytes)==BC_STS_SUCCESS,"NR prerequisites commit only actual input SUCCESS");
    }
}

static BC_STATUS NonReference(Builder &b, bool translation, const uint8_t *mask,
                              const uint8_t **au, uint32_t *bytes)
{
    return translation ? TranslateNonReference(b,1,-3,3,au,bytes) :
                         ComposeNonReference(b,mask,96,au,bytes);
}

static void NonReferenceValidation()
{
    const uint8_t *out=reinterpret_cast<const uint8_t *>(uintptr_t(0x4321));
    uint32_t count=0xdeadbeefU; uint8_t mask[96]={};
    ErrorOutputs(DtsRawFramePrepareComposeNonReference(nullptr,mask,96,&out,&count),
                 BC_STS_INV_ARG,out,count,"NULL NR compose builder preserves sentinels");
    ErrorOutputs(DtsRawFramePrepareTranslateNonReference(nullptr,0,0,0,&out,&count),
                 BC_STS_INV_ARG,out,count,"NULL NR translation builder preserves sentinels");
    for (bool translation : {false,true}) {
        Builder b; Parser parser;
        Check(NonReference(b,translation,mask,nullptr,&count)==BC_STS_INV_ARG && count==0xdeadbeefU &&
              NonReference(b,translation,mask,&out,nullptr)==BC_STS_INV_ARG &&
              out==reinterpret_cast<const uint8_t *>(uintptr_t(0x4321)),"NR NULL outputs preserve other output");
        ErrorOutputs(NonReference(b,translation,mask,&out,&count),BC_STS_ERR_USAGE,out,count,
                     "NR cannot precede initial reference IDR");
        Planes first(Source(0)); const uint8_t *au=nullptr; uint32_t bytes=0;
        Check(Upload(b,0,first,&au,&bytes)==BC_STS_SUCCESS,"prepare initial reference for NR prerequisites");
        ErrorOutputs(NonReference(b,translation,mask,&out,&count),BC_STS_BUSY,out,count,
                     "pending reference blocks NR without state changes");
        Check(parser.Read(Bytes(au,au+bytes),true,0,nullptr)==Source(0) && Accept(b,au,bytes)==BC_STS_SUCCESS,
              "NR first reference prerequisite commits");
        ErrorOutputs(NonReference(b,translation,mask,&out,&count),BC_STS_ERR_USAGE,out,count,
                     "NR requires both committed long-term sources");
        Planes second(Source(1));
        Check(Upload(b,1,second,&au,&bytes)==BC_STS_SUCCESS &&
              parser.Read(Bytes(au,au+bytes),true,1,nullptr)==Source(1) && Accept(b,au,bytes)==BC_STS_SUCCESS,
              "NR second reference prerequisite commits");
        if (translation) {
            for (unsigned slot : {2U,UINT32_MAX})
                ErrorOutputs(TranslateNonReference(b,slot,0,0,&out,&count),BC_STS_INV_ARG,out,count,
                             "NR translation rejects invalid source slot");
            for (int bad : {INT_MIN,-4,4,INT_MAX}) {
                ErrorOutputs(TranslateNonReference(b,0,bad,0,&out,&count),BC_STS_INV_ARG,out,count,
                             "NR translation rejects invalid horizontal MV");
                ErrorOutputs(TranslateNonReference(b,1,0,bad,&out,&count),BC_STS_INV_ARG,out,count,
                             "NR translation rejects invalid vertical MV");
            }
        } else {
            for (uint32_t size : {0U,95U,97U,UINT32_MAX})
                ErrorOutputs(ComposeNonReference(b,mask,size,&out,&count),BC_STS_INV_ARG,out,count,
                             "NR compose requires exactly96 mask entries");
            ErrorOutputs(ComposeNonReference(b,nullptr,96,&out,&count),BC_STS_INV_ARG,out,count,"NR NULL mask");
            for (unsigned index : {0U,47U,95U}) {
                for (uint8_t bad : {uint8_t(2),uint8_t(255)}) {
                    mask[index]=bad;
                    ErrorOutputs(ComposeNonReference(b,mask,96,&out,&count),BC_STS_INV_ARG,out,count,
                                 "NR rejects bad reference indices before publication");
                }
                mask[index]=0;
            }
        }
        Check(NonReference(b,translation,mask,&au,&bytes)==BC_STS_SUCCESS,"prepare first bounded NR P picture");
        const Bytes saved(au,au+bytes); const uint8_t *stable=au; const uint32_t stable_size=bytes;
        ErrorOutputs(ComposeNonReference(b,mask,96,&out,&count),BC_STS_BUSY,out,count,"pending NR blocks NR compose");
        ErrorOutputs(TranslateNonReference(b,0,0,0,&out,&count),BC_STS_BUSY,out,count,"pending NR blocks NR translate");
        ErrorOutputs(Compose(b,mask,96,&out,&count),BC_STS_BUSY,out,count,"pending NR blocks reference compose");
        ErrorOutputs(Translate(b,0,0,0,&out,&count),BC_STS_BUSY,out,count,"pending NR blocks reference translate");
        ErrorOutputs(Upload(b,0,first,&out,&count),BC_STS_BUSY,out,count,"pending NR blocks upload");
        if (!translation) std::fill(mask,mask+96,1);
        Check(au==stable && bytes==stable_size && Bytes(au,au+bytes)==saved,
              "pending NR owns copied mask and survives rejected prepares");
        std::fill(mask,mask+96,0);
        const Parser before=parser;
        const Bytes expected=translation ? Motion(Source(1),-3,3) : Source(0);
        Check(parser.Read(saved,false,1,mask,translation,-3,3,true)==expected && parser.poc==3,
              "NR exact header/absent marking/frame_num2/POC3 and full literal pixels");
        for (bool header_mutation : {false,true}) {
            Bytes bad=header_mutation ? saved : FlipVclBit(saved,parser.frame_bit);
            if (header_mutation) bad[10]=0x21;
            bool refused=false;
            try { Parser independent=before; independent.Read(bad,false,1,mask,translation,-3,3,true); }
            catch (const char *) { refused=true; }
            Check(refused,"independent NR parser rejects reference-bit/frame-number adversaries");
        }
        Check(DtsRawFrameDiscard(b.value)==BC_STS_SUCCESS,"discard pending NR before any transmission");
        parser=before;
        Check(NonReference(b,translation,mask,&au,&bytes)==BC_STS_SUCCESS && Bytes(au,au+bytes)==saved,
              "discard NR preserves AU/reference counts and permits identical reprepare");
        Check(parser.Read(Bytes(au,au+bytes),false,1,mask,translation,-3,3,true)==expected &&
              Accept(b,au,bytes,BC_STS_SUCCESS,translation ? UINT64_MAX : uint64_t(0))==BC_STS_SUCCESS,
              "NR actual Submit forwards exact AU/full token and commits once");
        ErrorOutputs(ComposeNonReference(b,mask,96,&out,&count),BC_STS_ERR_USAGE,out,count,"second committed NR compose refused");
        ErrorOutputs(TranslateNonReference(b,0,0,0,&out,&count),BC_STS_ERR_USAGE,out,count,"cross-operation consecutive NR refused");
        Check(Translate(b,0,1,-1,&au,&bytes)==BC_STS_SUCCESS && DtsRawFrameDiscard(b.value)==BC_STS_SUCCESS,
              "a prepared/discarded reference cannot reset committed NR guard");
        ErrorOutputs(NonReference(b,translation,mask,&out,&count),BC_STS_ERR_USAGE,out,count,"discarded reference leaves NR guard intact");
        Check(Translate(b,0,1,-1,&au,&bytes)==BC_STS_SUCCESS &&
              parser.Read(Bytes(au,au+bytes),false,0,nullptr,true,1,-1)==Motion(Source(0),1,-1) &&
              parser.poc==4 && Accept(b,au,bytes)==BC_STS_SUCCESS,
              "reference after NR reuses frame_num2/POC4 and resets guard only on SUCCESS");
        Check(NonReference(b,translation,mask,&au,&bytes)==BC_STS_SUCCESS &&
              DtsRawFrameAbort(b.value)==BC_STS_SUCCESS && DtsRawFrameAbort(b.value)==BC_STS_SUCCESS,
              "Abort pending NR is idempotent and never commits either count");
        ErrorOutputs(NonReference(b,translation,mask,&out,&count),BC_STS_IO_USER_ABORT,out,count,"Abort poisons NR prepares");
        ErrorOutputs(Compose(b,mask,96,&out,&count),BC_STS_IO_USER_ABORT,out,count,"Abort NR also poisons old reference API");
    }
    for (bool translation : {false,true}) {
        for (BC_STATUS status : {BC_STS_BUSY,BC_STS_IO_ERROR,BC_STS_IO_USER_ABORT,
                                BC_STS_INV_ARG,BC_STS_TIMEOUT,BC_STS_INSUFF_RES}) {
            Builder b; BothSources(b); const uint8_t *au=nullptr; uint32_t bytes=0;
            Check(NonReference(b,translation,mask,&au,&bytes)==BC_STS_SUCCESS && Accept(b,au,bytes,status)==status,
                  "NR Submit returns every actual nonSUCCESS unchanged");
            ErrorOutputs(ComposeNonReference(b,mask,96,&out,&count),BC_STS_IO_USER_ABORT,out,count,"NR failure poisons compose");
            ErrorOutputs(TranslateNonReference(b,0,0,0,&out,&count),BC_STS_IO_USER_ABORT,out,count,"NR failure poisons translate");
            ErrorOutputs(DtsRawFramePrepareUpload(b.value,0,nullptr,&out,&count),BC_STS_IO_USER_ABORT,out,count,
                         "NR failure blocks reference upload");
            const unsigned calls=submit.calls;
            Check(DtsRawFrameSubmit(b.value,nullptr,1)==BC_STS_IO_USER_ABORT && submit.calls==calls &&
                  DtsRawFrameDiscard(b.value)==BC_STS_IO_USER_ABORT,"NR failure cannot retry or recover through Discard");
        }
        Builder manual; BothSources(manual); const uint8_t *au=nullptr; uint32_t bytes=0;
        Check(NonReference(manual,translation,mask,&au,&bytes)==BC_STS_SUCCESS &&
              DtsRawFrameFinish(manual.value,BC_STS_IO_ERROR)==BC_STS_IO_ERROR &&
              DtsRawFrameDiscard(manual.value)==BC_STS_IO_USER_ABORT,"manual failed NR Finish also poisons state");
    }
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

static Stream Translations()
{
    Builder b; Parser parser; Stream stream;
    const Bytes sources[]={Source(0),Source(1),Source(2)};
    unsigned covered[3][49]={},compositions[2]={},translated=0;
    for (unsigned f=0;f<180;++f) {
        const bool upload=f==0 || f==1 || f==90;
        unsigned slot=f==1 ? 1 : 0,source_index=f==0 ? 0 : (f==1 ? 1 : 2);
        unsigned pose=24,phase=0;
        bool composition=false;
        if (!upload) {
            const unsigned j=f<90 ? f-2 : f-3;
            if (j<147) {
                const unsigned block=j/49;
                slot=block==1 ? 1 : 0; source_index=block;
                pose=j%49; ++covered[block][pose];
            } else {
                const unsigned r=j-147;
                composition=(r%2)==0;
                phase=(r/2)%2; slot=1; source_index=1; pose=(11+17*r)%49;
                if (composition) ++compositions[phase];
            }
        }
        const int qx=static_cast<int>(pose%7)-3,qy=static_cast<int>(pose/7)-3;
        uint8_t mask[96];
        for (unsigned mb=0;mb<96;++mb) mask[mb]=static_cast<uint8_t>((mb%16+mb/16+phase)&1);
        const uint8_t *au=nullptr; uint32_t count=0;
        if (upload) {
            Planes planes(sources[source_index],9+f%3);
            Check(Upload(b,slot,planes,&au,&count)==BC_STS_SUCCESS,"translation stream uploads exact caller source");
        } else if (composition)
            Check(Compose(b,mask,96,&au,&count)==BC_STS_SUCCESS,"translation stream retains both checker phases");
        else {
            Check(Translate(b,slot,qx,qy,&au,&count)==BC_STS_SUCCESS,"public API accepts every selected signed-grid vector");
            ++translated;
        }
        Require(au && count && count<=BC_RAW_FRAME_AU_CAPACITY,"complete bounded translation AU");
        const Bytes encoded(au,au+count);
        const Bytes expected=upload ? sources[source_index] : (composition ?
            Assemble(sources[2],sources[1],mask) : Motion(sources[source_index],qx,qy));
        Check(parser.Read(encoded,upload,slot,mask,!upload && !composition,qx,qy)==expected,
              "independent ref/parser/cache and normative full planar oracle agree");
        if (!upload && !composition && pose==24)
            Check(expected==sources[source_index],"zero-pose full pixels equal the corresponding upload");
        Check(Accept(b,au,count,BC_STS_SUCCESS,uint64_t(f+1)*100000)==BC_STS_SUCCESS,
              "translation stream uses actual-status Submit with ordered caller tokens");
        Append(&stream.coded,encoded); Append(&stream.planar,expected);
        stream.maximum_au=std::max(stream.maximum_au,count);
    }
    bool complete=true;
    for (unsigned source=0;source<3;++source)
        for (unsigned pose=0;pose<49;++pose) complete=complete && covered[source][pose]==1;
    Check(complete && translated==162 && compositions[0]==8 && compositions[1]==7,
          "all49 poses each on A/B/C plus15 B repeats and both checker phases");
    Check(parser.frame==180 && (parser.frame-1)%16==3 && 2*(parser.frame-1)==358 &&
          stream.planar.size()==6635520,"all180 frames/global11 wraps/final POC358/full extent");
    std::vector<Bytes> distinct;
    for (unsigned f=0;f<180;++f) {
        const Bytes view(stream.planar.begin()+f*frame_bytes,stream.planar.begin()+(f+1)*frame_bytes);
        if (std::find(distinct.begin(),distinct.end(),view)==distinct.end()) distinct.push_back(view);
    }
    Check(distinct.size()==149,"exact147 translated source views plus two mixed checker views");
    std::printf("translate: frames=180 I=0,1,90 key=0 P=177 translations=162 compose=15 views=%zu bytes=%zu maxAU=%u planar=%zu\n",
                distinct.size(),stream.coded.size(),stream.maximum_au,stream.planar.size());
    return stream;
}

static Stream MixedReferences()
{
    Builder b; Parser parser; Stream stream;
    const Bytes sources[]={Source(0),Source(1),Source(2)};
    unsigned nr_compose=0,nr_translate=0,previous_poc=0;
    for (unsigned f=0;f<64;++f) {
        const bool upload=f==0 || f==1 || f==32;
        const bool non_reference=!upload && f%2==0;
        const bool translation=!upload && f%4!=2 && f%4!=3;
        const unsigned slot=upload ? (f==1 ? 1 : 0) : (f/4)%2;
        const unsigned source_index=upload ? (f==0 ? 0 : f==1 ? 1 : 2) :
            (slot==1 ? 1 : f<32 ? 0 : 2);
        const int qx=static_cast<int>(f%7)-3,qy=static_cast<int>((3*f)%7)-3;
        uint8_t mask[96];
        for (unsigned mb=0;mb<96;++mb) mask[mb]=static_cast<uint8_t>((mb%16+mb/16+(f/4)%2)&1);
        const uint8_t *au=nullptr; uint32_t bytes=0;
        BC_STATUS status;
        if (upload) {
            Planes planes(sources[source_index],11);
            status=Upload(b,slot,planes,&au,&bytes);
        } else if (translation) {
            status=non_reference ? TranslateNonReference(b,slot,qx,qy,&au,&bytes) :
                                   Translate(b,slot,qx,qy,&au,&bytes);
            nr_translate+=unsigned(non_reference);
        } else {
            status=non_reference ? ComposeNonReference(b,mask,96,&au,&bytes) : Compose(b,mask,96,&au,&bytes);
            nr_compose+=unsigned(non_reference);
        }
        Check(status==BC_STS_SUCCESS,"mixed reference/NR stream public Prepare succeeds");
        Require(au && bytes && bytes<=BC_RAW_FRAME_AU_CAPACITY,"mixed AU is complete and bounded");
        const Bytes encoded(au,au+bytes);
        const Bytes expected=upload ? sources[source_index] : translation ? Motion(sources[source_index],qx,qy) :
            Assemble(sources[f<32 ? 0 : 2],sources[1],mask);
        Check(parser.Read(encoded,upload,slot,mask,translation,qx,qy,non_reference)==expected,
              "mixed independent header/marking/MV/LTR reader matches every literal source pixel");
        if (f) Check(parser.poc>previous_poc,"mixed POC2 strictly increases across NR and reference wraps");
        previous_poc=parser.poc;
        Check(Accept(b,au,bytes,BC_STS_SUCCESS,uint64_t(f)*10000000/30)==BC_STS_SUCCESS,
              "mixed Submit commits exact pending mode and monotonic caller token");
        Append(&stream.coded,encoded); Append(&stream.planar,expected);
        stream.maximum_au=std::max(stream.maximum_au,bytes);
    }
    Check(parser.frame==64 && parser.non_reference_frames==30 && nr_compose==16 && nr_translate==14 &&
          parser.frame-parser.non_reference_frames==34 && parser.poc==66 && stream.planar.size()==2359296,
          "mixed64 outputs/30NR/34references/two reference wraps/finalPOC66/full oracle extent");
    std::printf("mixed: frames=64 I=0,1,32 key=0 references=34 NR=30 composeNR=%u translateNR=%u bytes=%zu maxAU=%u planar=%zu\n",
                nr_compose,nr_translate,stream.coded.size(),stream.maximum_au,stream.planar.size());
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
        Validation(); SubmitFailures(); PredictorTests(); InterpolationTests(); TranslationValidation(); NonReferenceValidation();
        const Stream checker = Checker(false), mirror = Checker(true), extreme = Extreme();
        const Stream translate = Translations();
        const Stream mixed = MixedReferences();
        if (argc == 3) {
            Emit(argv[2], "checker", checker); Emit(argv[2], "mirror", mirror); Emit(argv[2], "extreme", extreme);
            Emit(argv[2], "translate", translate);
            Emit(argv[2], "mixed", mixed);
        }
    } catch (const char *message) { Check(false, message); }
    if (failures) { std::fprintf(stderr, "%u/%u raw-frame checks failed\n", failures, checks); return 1; }
    std::printf("PASS: %u public raw-frame API, transactional Submit, syntax/source/tile and finite-output checks\n", checks);
    return 0;
}
