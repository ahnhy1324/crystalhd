// SPDX-License-Identifier: LGPL-2.1-or-later
// Real non-MODE NV12/YV12 copies, without firmware or device access.
// Sizes/Done counts are DWORDs; strides are additional destination bytes.
#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <vector>
#include "7411d.h"
#include "libcrystalhd_int_if.h"
#include "libcrystalhd_priv.h"

static unsigned checks, failures, cases;
static bool baseline;
static const uint8_t untouched = 0xa5;
static char description[192];

static void Check(bool condition, const char *message)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL [%s]: %s\n", description, message);
    }
}

static unsigned Dwords(size_t bytes) { return static_cast<unsigned>((bytes + 3) / 4); }
static size_t Extent(unsigned pitch, unsigned bytes, unsigned rows)
{ return static_cast<size_t>(rows - 1) * pitch + bytes; }
static uint8_t Y(unsigned row, unsigned x) { return static_cast<uint8_t>(17 + row * 29 + x * 7); }
static uint8_t U(unsigned row, unsigned x) { return static_cast<uint8_t>(31 + row * 13 + x * 17); }
static uint8_t V(unsigned row, unsigned x) { return static_cast<uint8_t>(143 + row * 19 + x * 11); }

struct Planar {
    DTS_LIB_CONTEXT context = {};
    BC_DTS_PROC_OUT input = {}, output = {};
    bool yv12;
    unsigned width, rows, uv_rows, source_pitch, y_pitch, uv_pitch;
    size_t prefix, u_offset;
    std::vector<uint8_t> source_y, source_uv, destination_y, destination_uv;
    std::vector<uint8_t> original_y, original_uv;

    Planar(bool planar_vu, unsigned w = 8, unsigned h = 4,
           unsigned hardware_padding = 0, unsigned y_padding = 0,
           bool uv_override = false, unsigned uv_padding = 0,
           bool field = false, bool size = false, unsigned crop_w = 0,
           unsigned crop_h = 0, unsigned alignment = 0)
        : yv12(planar_vu), width(crop_w ? crop_w : w),
          rows((crop_h ? crop_h : h) / (field ? 2 : 1)),
          uv_rows((rows + 1) / 2), source_pitch(w + hardware_padding),
          y_pitch(width + y_padding),
          uv_pitch((yv12 ? width / 2 : width) +
                   (uv_override ? uv_padding : (yv12 ? y_padding / 2 : y_padding))),
          prefix(16 + alignment), u_offset(yv12 ? static_cast<size_t>(uv_pitch) * uv_rows : 0)
    {
        ++cases;
        std::snprintf(description, sizeof(description),
                      "%s %ux%u hw+%u pad%u/%u override%u field%u size%u crop%ux%u align%u",
                      yv12 ? "YV12" : "NV12", w, h, hardware_padding, y_padding,
                      uv_padding, uv_override, field, size, crop_w, crop_h, alignment);
        context.b422Mode = OUTPUT_MODE420_NV12;
        context.VidParams.Progressive = !field;
        context.HWOutPicWidth = source_pitch;
        input.PicInfo.width = w;
        input.PicInfo.height = h;
        input.PicInfo.flags = field ? VDEC_FLAG_INTERLACED_SRC | VDEC_FLAG_TOPFIELD : 0;
        input.PoutFlags = BC_POUT_FLAGS_PIB_VALID;
        const unsigned source_rows = h / (field ? 2 : 1);
        input.YBuffDoneSz = Dwords(Extent(source_pitch, w, source_rows));
        input.UVBuffDoneSz = Dwords(Extent(source_pitch, w, (source_rows + 1) / 2));
        output.b422Mode = OUTPUT_MODE420_NV12;
        output.PoutFlags = yv12 ? BC_POUT_FLAGS_YV12 : 0;
        if (size || crop_w || crop_h) output.PoutFlags |= BC_POUT_FLAGS_SIZE;
        if (y_padding) output.PoutFlags |= BC_POUT_FLAGS_STRIDE;
        if (uv_override) output.PoutFlags |= BC_POUT_FLAGS_STRIDE_UV;
        output.StrideSz = y_padding;
        output.StrideSzUV = uv_padding;
        output.PicInfo.width = width;
        output.PicInfo.height = crop_h ? crop_h : h;
        output.YbuffSz = Dwords(Extent(y_pitch, width, rows));
        output.UVbuffSz = Dwords(u_offset + Extent(uv_pitch, yv12 ? width / 2 : width, uv_rows));
        // Baseline proof uses oversized backing, never real underallocation.
        // Final runs end sources exactly at the declared DWORD boundary.
        source_y.assign(prefix + (baseline ? 4096 : input.YBuffDoneSz * 4), 0xd3);
        source_uv.assign(prefix + (baseline ? 4096 : input.UVBuffDoneSz * 4), 0xc7);
        destination_y.assign(prefix + (baseline ? 4096 : output.YbuffSz * 4) + 32, untouched);
        destination_uv.assign(prefix + (baseline ? 4096 : output.UVbuffSz * 4) + 32, untouched);
        input.Ybuff = source_y.data() + prefix;
        input.UVbuff = source_uv.data() + prefix;
        output.Ybuff = destination_y.data() + prefix;
        output.UVbuff = destination_uv.data() + prefix;
        for (unsigned row = 0; row < source_rows; ++row)
            for (unsigned x = 0; x < w; ++x) input.Ybuff[row * source_pitch + x] = Y(row, x);
        for (unsigned row = 0; row < (source_rows + 1) / 2; ++row)
            for (unsigned x = 0; x < w; x += 2) {
                input.UVbuff[row * source_pitch + x] = U(row, x / 2);
                input.UVbuff[row * source_pitch + x + 1] = V(row, x / 2);
            }
        original_y = source_y;
        original_uv = source_uv;
    }

    BC_STATUS Run()
    {
        return yv12 ? DtsCopyNV12ToYV12(&context, &output, &input)
                    : DtsCopyNV12(&context, &output, &input);
    }
    void SourceUnchanged()
    {
        Check(source_y == original_y && source_uv == original_uv, "source pixels and padding remain unchanged");
    }
    void Success()
    {
        Check(Run() == BC_STS_SUCCESS, "valid planar layout succeeds");
        std::vector<uint8_t> expected_y(destination_y.size(), untouched);
        std::vector<uint8_t> expected_uv(destination_uv.size(), untouched);
        for (unsigned row = 0; row < rows; ++row)
            for (unsigned x = 0; x < width; ++x) expected_y[prefix + row * y_pitch + x] = Y(row, x);
        for (unsigned row = 0; row < uv_rows; ++row)
            for (unsigned x = 0; x < width / 2; ++x) {
                if (yv12) {
                    expected_uv[prefix + row * uv_pitch + x] = V(row, x);
                    expected_uv[prefix + u_offset + row * uv_pitch + x] = U(row, x);
                } else {
                    expected_uv[prefix + row * uv_pitch + 2 * x] = U(row, x);
                    expected_uv[prefix + row * uv_pitch + 2 * x + 1] = V(row, x);
                }
            }
        Check(destination_y == expected_y, "exact luma rows, destination padding and canaries");
        Check(destination_uv == expected_uv, "exact chroma rows/plane order, padding and canaries");
        Check(output.YBuffDoneSz == input.YBuffDoneSz && output.UVBuffDoneSz == input.UVBuffDoneSz,
              "completed-transfer DWORD metadata is preserved");
        SourceUnchanged();
    }
    void Reject(BC_STATUS status)
    {
        Check(Run() == status, "invalid layout returns expected status");
        Check(std::all_of(destination_y.begin(), destination_y.end(), [](uint8_t v) { return v == untouched; }) &&
              std::all_of(destination_uv.begin(), destination_uv.end(), [](uint8_t v) { return v == untouched; }),
              "all planes validated before any destination write");
        SourceUnchanged();
    }
};

static void Rows()
{
    for (bool yv12 : {false, true})
        for (unsigned width : {2U, 6U, 8U, 18U})
            for (unsigned height : {1U, 3U, 4U, 6U})
                for (unsigned padding : {0U, 4U})
                    for (unsigned layout = 0; layout < 3; ++layout)
                        for (unsigned alignment : {0U, 1U, 7U, 15U}) {
                            Planar copy(yv12, width, height, padding, layout ? 3 : 0,
                                        layout == 2, 5, false, layout == 1, 0, 0, alignment);
                            copy.Success();
                        }
    for (bool yv12 : {false, true}) {
        for (bool size : {false, true})
            for (unsigned height : {2U, 6U, 8U})
                for (bool bottom : {false, true}) {
                    Planar copy(yv12, 8, height, 4, 8, false, 0, true, size);
                    copy.input.PicInfo.flags = VDEC_FLAG_INTERLACED_SRC |
                        (bottom ? VDEC_FLAG_BOTTOMFIELD : VDEC_FLAG_TOPFIELD);
                    copy.Success();
                }
        Planar crop(yv12, 18, 8, 4, 3, true, 5, false, true, 6, 3);
        crop.Success();
        Planar field_crop(yv12, 18, 8, 4, 3, true, 5, true, true, 6, 6);
        field_crop.Success();
    }
}

static void Capacity()
{
    for (bool yv12 : {false, true}) {
        Planar exact(yv12, 8, 4, 4, 4, true, 4, false, true);
        exact.Success();
        for (unsigned fault = 0; fault < 4; ++fault) {
            Planar copy(yv12, 8, 4, 4, 4, true, 4, false, true);
            if (fault == 0) --copy.output.YbuffSz;
            if (fault == 1) --copy.output.UVbuffSz;
            if (fault == 2) --copy.input.YBuffDoneSz;
            if (fault == 3) --copy.input.UVBuffDoneSz;
            copy.Reject(BC_STS_IO_XFR_ERROR);
        }
    }
}

static void FieldPairs()
{
    for (bool yv12 : {false, true})
        for (bool size : {false, true})
            for (unsigned first : {0U, 1U}) {
                Planar copy(yv12, 8, 8, 4, 8, false, 0, true, size);
                std::vector<uint8_t> expected_y(copy.destination_y.size(), untouched);
                std::vector<uint8_t> expected_uv(copy.destination_uv.size(), untouched);
                for (unsigned part = 0; part < 2; ++part) {
                    const unsigned bottom = first ^ part;
                    const unsigned delta = part * 47;
                    for (unsigned row = 0; row < copy.rows; ++row)
                        for (unsigned x = 0; x < copy.width; ++x) {
                            const uint8_t value = static_cast<uint8_t>(Y(row, x) + delta);
                            copy.input.Ybuff[row * copy.source_pitch + x] = value;
                            expected_y[copy.prefix + (2 * row + bottom) * 8 + x] = value;
                        }
                    for (unsigned row = 0; row < copy.uv_rows; ++row)
                        for (unsigned x = 0; x < copy.width / 2; ++x) {
                            const uint8_t u = static_cast<uint8_t>(U(row, x) + delta);
                            const uint8_t v = static_cast<uint8_t>(V(row, x) + delta);
                            copy.input.UVbuff[row * copy.source_pitch + x * 2] = u;
                            copy.input.UVbuff[row * copy.source_pitch + x * 2 + 1] = v;
                            if (yv12) {
                                expected_uv[copy.prefix + (2 * row + bottom) * 4 + x] = v;
                                expected_uv[copy.prefix + 16 + (2 * row + bottom) * 4 + x] = u;
                            } else {
                                expected_uv[copy.prefix + (2 * row + bottom) * 8 + x * 2] = u;
                                expected_uv[copy.prefix + (2 * row + bottom) * 8 + x * 2 + 1] = v;
                            }
                        }
                    copy.original_y = copy.source_y;
                    copy.original_uv = copy.source_uv;
                    copy.output.Ybuff = copy.destination_y.data() + copy.prefix + bottom * 8;
                    copy.output.YbuffSz = 16 - bottom * 2;
                    copy.output.UVbuff = copy.destination_uv.data() + copy.prefix + bottom * (yv12 ? 4 : 8);
                    copy.output.UVbuffSz = 8 - bottom * (yv12 ? 1 : 2);
                    copy.input.PicInfo.flags = VDEC_FLAG_INTERLACED_SRC |
                        (bottom ? VDEC_FLAG_BOTTOMFIELD : VDEC_FLAG_TOPFIELD);
                    Check(copy.Run() == BC_STS_SUCCESS, "either field order can copy into offset output pointers");
                    copy.SourceUnchanged();
                }
                Check(copy.destination_y == expected_y && copy.destination_uv == expected_uv,
                      "both fields weave without overwriting opposite-field pixels or plane padding");
            }
}

static void Invalid()
{
    for (bool yv12 : {false, true}) {
        for (unsigned fault = 0; fault < 10; ++fault) {
            Planar copy(yv12, 8, 4, 0, 0, false, 0, false, true);
            if (fault == 0) copy.output.PicInfo.width = 0;
            if (fault == 1) copy.output.PicInfo.height = 0;
            if (fault == 2) copy.output.PicInfo.width = 7;
            if (fault == 3) copy.output.PicInfo.width = 10;
            if (fault == 4) copy.output.PicInfo.height = 6;
            if (fault == 5) copy.output.PicInfo.width = UINT_MAX;
            if (fault == 6) copy.output.PicInfo.height = UINT_MAX;
            if (fault == 7) copy.output.PicInfo.width = UINT_MAX - 1;
            if (fault == 8) { copy.context.VidParams.Progressive = false; copy.output.PicInfo.height = 3; }
            if (fault == 9) copy.context.b422Mode = OUTPUT_MODE422_YUY2;
            copy.Reject(BC_STS_INV_ARG);
        }
        for (unsigned fault = 0; fault < 13; ++fault) {
            Planar copy(yv12);
            if (fault == 0) copy.input.PicInfo.width = 0;
            if (fault == 1) copy.input.PicInfo.height = 0;
            if (fault == 2) copy.input.PicInfo.width = 7;
            if (fault == 3) copy.context.HWOutPicWidth = 0;
            if (fault == 4) copy.context.HWOutPicWidth = 6;
            if (fault == 5) copy.context.HWOutPicWidth = UINT_MAX;
            if (fault == 6) { copy.context.VidParams.Progressive = false; copy.input.PicInfo.height = 3; }
            if (fault == 7) copy.input.YBuffDoneSz = 0;
            if (fault == 8) copy.input.UVBuffDoneSz = 0;
            if (fault == 9) copy.output.YbuffSz = 0;
            if (fault == 10) { copy.output.PoutFlags |= BC_POUT_FLAGS_STRIDE; copy.output.StrideSz = UINT_MAX; }
            if (fault == 11) { copy.output.PoutFlags |= BC_POUT_FLAGS_STRIDE_UV; copy.output.StrideSzUV = UINT_MAX; }
            if (fault == 12) copy.output.UVbuffSz = 0;
            copy.Reject(BC_STS_IO_XFR_ERROR);
        }
        for (unsigned plane = 0; plane < 4; ++plane) {
            Planar copy(yv12);
            if (plane == 0) copy.input.Ybuff = nullptr;
            if (plane == 1) copy.input.UVbuff = nullptr;
            if (plane == 2) copy.output.Ybuff = nullptr;
            if (plane == 3) copy.output.UVbuff = nullptr;
            copy.Reject(BC_STS_INV_ARG);
        }
        Planar copy(yv12);
        const auto run = yv12 ? DtsCopyNV12ToYV12 : DtsCopyNV12;
        Check(run(nullptr, &copy.output, &copy.input) == BC_STS_INV_ARG, "null context rejected");
        Check(run(&copy.context, nullptr, &copy.input) == BC_STS_INV_ARG, "null output rejected");
        Check(run(&copy.context, &copy.output, nullptr) == BC_STS_INV_ARG, "null input rejected");
    }
#if SIZE_MAX == UINT32_MAX
    // Each chroma plane fits separately in a 32-bit address space, but the
    // combined V+U extent does not. Declared DWORD capacity must not bypass it.
    Planar combined(true);
    combined.output.PoutFlags |= BC_POUT_FLAGS_STRIDE_UV;
    combined.output.StrideSzUV = 0x60000000U - 4;
    combined.output.UVbuffSz = UINT_MAX;
    combined.Reject(BC_STS_IO_XFR_ERROR);
#endif
}

int main(int argc, char **argv)
{
    if (argc > 2 || (argc == 2 && std::strcmp(argv[1], "--baseline-safe"))) return 2;
    baseline = argc == 2;
    Rows();
    Capacity();
    if (!baseline) {
        FieldPairs();
        Invalid();
    }
    std::printf("Planar copies: %u cases, %u checks, %u failures\n", cases, checks, failures);
    return failures ? 1 : 0;
}
