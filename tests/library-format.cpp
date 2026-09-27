// SPDX-License-Identifier: LGPL-2.1-or-later
// Actual BC_POUT_FLAGS_MODE conversion, without device access. Unlike the raw
// copy API, MODE StrideSz/StrideSzUV are extra BYTES per destination row.
#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <vector>
#include "7411d.h"
#include "libcrystalhd_int_if.h"
#include "libcrystalhd_priv.h"

extern bc_dil_glob_s *bc_dil_glob_ptr;
static unsigned checks, failures, cases;
static char description[192];
static const uint8_t untouched = 0xa5;
static const BC_OUTPUT_FORMAT modes[] = {
    OUTPUT_MODE420_NV12, OUTPUT_MODE422_YUY2, OUTPUT_MODE422_UYVY
};

static void Check(bool condition, const char *message)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL [%s]: %s\n", description, message);
    }
}

static size_t Extent(unsigned pitch, unsigned bytes, unsigned rows)
{
    return static_cast<size_t>(rows - 1) * pitch + bytes;
}

static unsigned Dwords(size_t bytes) { return static_cast<unsigned>((bytes + 3) / 4); }
static uint8_t Y(unsigned row, unsigned x) { return static_cast<uint8_t>(17 + row * 29 + x * 7); }
static uint8_t U(unsigned row, unsigned pair) { return static_cast<uint8_t>(31 + row * 13 + pair * 17); }
static uint8_t V(unsigned row, unsigned pair) { return static_cast<uint8_t>(143 + row * 19 + pair * 11); }

struct Conversion {
    DTS_LIB_CONTEXT context = {};
    BC_DTS_PROC_OUT input = {}, output = {};
    BC_OUTPUT_FORMAT source_mode, destination_mode;
    unsigned width, source_rows, copied_width, copied_rows;
    unsigned source_pitch, destination_pitch, destination_uv_pitch;
    size_t prefix;
    std::vector<uint8_t> source_y, source_uv, destination_y, destination_uv;
    std::vector<uint8_t> original_y, original_uv;

    Conversion(BC_OUTPUT_FORMAT source, BC_OUTPUT_FORMAT destination,
               unsigned w = 16, unsigned h = 4, unsigned hardware_padding = 0,
               unsigned y_padding = 0, unsigned uv_padding = 0, bool field = false,
               unsigned crop_width = 0, unsigned crop_height = 0,
               unsigned alignment = 0, bool baseline = false)
        : source_mode(source), destination_mode(destination), width(w),
          source_rows(field ? h / 2 : h),
          copied_width(crop_width ? crop_width : w),
          copied_rows(field ? (crop_height ? crop_height : h) / 2 : (crop_height ? crop_height : h)),
          source_pitch((w + hardware_padding) * (source == OUTPUT_MODE420_NV12 ? 1 : 2)),
          destination_pitch(copied_width * (destination == OUTPUT_MODE420_NV12 ? 1 : 2) + y_padding),
          destination_uv_pitch(copied_width + uv_padding), prefix(16 + alignment)
    {
        ++cases;
        std::snprintf(description, sizeof(description),
                      "%u->%u %ux%u pitch+%u dst+%u/%u field=%u crop=%ux%u align=%u",
                      source, destination, w, h, hardware_padding, y_padding, uv_padding,
                      field, crop_width, crop_height, alignment);
        context.b422Mode = source;
        context.VidParams.Progressive = !field;
        context.HWOutPicWidth = w + hardware_padding;
        input.PicInfo.width = w;
        input.PicInfo.height = h;
        input.PicInfo.flags = field ? VDEC_FLAG_INTERLACED_SRC | VDEC_FLAG_TOPFIELD : 0;
        input.PoutFlags = BC_POUT_FLAGS_PIB_VALID;
        input.YBuffDoneSz = Dwords(Extent(source_pitch, w * (source == OUTPUT_MODE420_NV12 ? 1 : 2), source_rows));
        input.UVBuffDoneSz = source == OUTPUT_MODE420_NV12
            ? Dwords(Extent(w + hardware_padding, w, (source_rows + 1) / 2)) : 0;
        output.b422Mode = destination;
        output.PoutFlags = BC_POUT_FLAGS_MODE;
        if (y_padding) output.PoutFlags |= BC_POUT_FLAGS_STRIDE;
        if (uv_padding != y_padding) output.PoutFlags |= BC_POUT_FLAGS_STRIDE_UV;
        output.StrideSz = y_padding;
        output.StrideSzUV = uv_padding;
        if (crop_width || crop_height) output.PoutFlags |= BC_POUT_FLAGS_SIZE;
        output.PicInfo.width = copied_width;
        output.PicInfo.height = crop_height ? crop_height : h;
        output.YbuffSz = Dwords(Extent(destination_pitch,
                                      copied_width * (destination == OUTPUT_MODE420_NV12 ? 1 : 2), copied_rows));
        output.UVbuffSz = destination == OUTPUT_MODE420_NV12
            ? Dwords(Extent(destination_uv_pitch, copied_width, (copied_rows + 1) / 2)) : 0;
        // Normal sources end exactly at the DWORD-rounded declared capacity.
        // Baseline-only backing is deliberately oversized: old unchecked
        // fields/crops can run without making the before proof memory-unsafe.
        source_y.assign(prefix + (baseline ? 65536 : input.YBuffDoneSz * 4), 0xd3);
        source_uv.assign(prefix + (baseline ? 65536 : input.UVBuffDoneSz * 4), 0xc7);
        destination_y.assign(prefix + (baseline ? 65536 : output.YbuffSz * 4) + 32, untouched);
        destination_uv.assign(prefix + (baseline ? 65536 : output.UVbuffSz * 4) + 32, untouched);
        input.Ybuff = source_y.data() + prefix;
        input.UVbuff = source == OUTPUT_MODE420_NV12 ? source_uv.data() + prefix : nullptr;
        output.Ybuff = destination_y.data() + prefix;
        output.UVbuff = destination == OUTPUT_MODE420_NV12 || baseline
            ? destination_uv.data() + prefix : nullptr;
        for (unsigned row = 0; row < source_rows; ++row) {
            for (unsigned x = 0; x < w; ++x) {
                if (source == OUTPUT_MODE420_NV12) input.Ybuff[row * source_pitch + x] = Y(row, x);
                else {
                    const unsigned y_byte = source == OUTPUT_MODE422_YUY2 ? 0 : 1;
                    input.Ybuff[row * source_pitch + 2 * x + y_byte] = Y(row, x);
                    input.Ybuff[row * source_pitch + 2 * x + (1 - y_byte)] =
                        x & 1 ? V(row, x / 2) : U(row, x / 2);
                }
            }
        }
        if (source == OUTPUT_MODE420_NV12) {
            for (unsigned row = 0; row < (source_rows + 1) / 2; ++row)
                for (unsigned x = 0; x < w; x += 2) {
                    input.UVbuff[row * (w + hardware_padding) + x] = U(row, x / 2);
                    input.UVbuff[row * (w + hardware_padding) + x + 1] = V(row, x / 2);
                }
        }
        original_y = source_y;
        original_uv = source_uv;
    }

    BC_STATUS Run() { return DtsCopyFormat(&context, &output, &input); }

    uint8_t Chroma(unsigned row, unsigned pair, bool v) const
    {
        if (source_mode != OUTPUT_MODE420_NV12) return v ? V(row, pair) : U(row, pair);
        const unsigned current = row / 2;
        const unsigned next = std::min(current + 1, (copied_rows - 1) / 2);
        const unsigned a = v ? V(current, pair) : U(current, pair);
        const unsigned b = v ? V(next, pair) : U(next, pair);
        return static_cast<uint8_t>((row & 1) ? (a + b + 1) / 2 : a);
    }

    void Expect()
    {
        std::vector<uint8_t> expected_y(destination_y.size(), untouched);
        std::vector<uint8_t> expected_uv(destination_uv.size(), untouched);
        for (unsigned row = 0; row < copied_rows; ++row) {
            for (unsigned x = 0; x < copied_width; ++x) {
                const size_t offset = prefix + static_cast<size_t>(row) * destination_pitch;
                if (destination_mode == OUTPUT_MODE420_NV12) expected_y[offset + x] = Y(row, x);
                else {
                    const unsigned y_byte = destination_mode == OUTPUT_MODE422_YUY2 ? 0 : 1;
                    expected_y[offset + x * 2 + y_byte] = Y(row, x);
                    expected_y[offset + x * 2 + (1 - y_byte)] = Chroma(row, x / 2, x & 1);
                }
            }
        }
        if (destination_mode == OUTPUT_MODE420_NV12) {
            for (unsigned row = 0; row < (copied_rows + 1) / 2; ++row)
                for (unsigned x = 0; x < copied_width; x += 2) {
                    const unsigned source_row = source_mode == OUTPUT_MODE420_NV12 ? row : row * 2;
                    const size_t offset = prefix + static_cast<size_t>(row) * destination_uv_pitch + x;
                    expected_uv[offset] = U(source_row, x / 2);
                    expected_uv[offset + 1] = V(source_row, x / 2);
                }
        }
        Check(destination_y == expected_y, "exact luma/packed pixels, byte padding and canaries");
        Check(destination_uv == expected_uv, "exact UV pixels, byte padding and canaries");
        Check(source_y == original_y && source_uv == original_uv, "source planes remain unchanged");
    }

    void Success()
    {
        Check(Run() == BC_STS_SUCCESS, "valid conversion succeeds");
        Expect();
        Check(output.YBuffDoneSz == input.YBuffDoneSz && output.UVBuffDoneSz == input.UVBuffDoneSz,
              "completed-size metadata stays the source DWORD counts, not converted output size");
    }

    void Reject(BC_STATUS status)
    {
        Check(Run() == status, "invalid layout returns the expected failure");
        Check(std::all_of(destination_y.begin(), destination_y.end(), [](uint8_t b) { return b == untouched; }) &&
              std::all_of(destination_uv.begin(), destination_uv.end(), [](uint8_t b) { return b == untouched; }),
              "all source/destination validation completes before any pixel write");
        Check(source_y == original_y && source_uv == original_uv, "rejection does not mutate source planes");
    }
};

static void Matrix(bool baseline)
{
    for (BC_OUTPUT_FORMAT source : modes)
        for (BC_OUTPUT_FORMAT destination : modes)
            for (unsigned width : {2U, 6U, 8U, 14U, 16U, 18U, 32U}) {
                // The old NV12->UYVY loop never advances its row counter;
                // even a wide, well-backed image eventually overruns.
                if (baseline && source == OUTPUT_MODE420_NV12 && destination == OUTPUT_MODE422_UYVY) continue;
                if (baseline && width != 16 && width != 32) continue;
                for (unsigned alignment : {0U, 1U, 7U, 15U}) {
                    if (baseline && alignment != 1) continue;
                    for (unsigned hardware_padding : {0U, 4U}) {
                        Conversion plain(source, destination, width, 6, hardware_padding,
                                         0, 0, false, 0, 0, alignment, baseline);
                        plain.Success();
                        Conversion padded(source, destination, width, 6, hardware_padding,
                                          baseline ? 16 : 3, baseline ? 16 : 7,
                                          false, 0, 0, alignment, baseline);
                        padded.Success();
                    }
                }
            }
}

static void CropsAndFields(bool baseline)
{
    for (BC_OUTPUT_FORMAT source : modes)
        for (BC_OUTPUT_FORMAT destination : modes) {
            if (baseline && source == OUTPUT_MODE420_NV12 && destination == OUTPUT_MODE422_UYVY) continue;
            for (bool size : {false, true}) {
                Conversion field(source, destination, 32, 8, 0, 16, 16, true,
                                 size ? 16 : 0, size ? 4 : 0, 1, baseline);
                field.Success();
                Conversion crop(source, destination, 32, 6, 0, 16, 16, false,
                                16, size ? 4 : 6, 1, baseline);
                crop.Success();
            }
            if (!baseline) {
                for (unsigned height : {1U, 3U, 5U}) {
                    Conversion odd(source, destination, 6, height, 4, 3, 5);
                    odd.Success();
                }
                Conversion uv_only(source, destination, 18, 6, 4, 0, 7);
                uv_only.Success();
            }
        }
}

static void Capacity(bool baseline)
{
    for (BC_OUTPUT_FORMAT source : modes)
        for (BC_OUTPUT_FORMAT destination : modes) {
            if (baseline && source == OUTPUT_MODE420_NV12 && destination == OUTPUT_MODE422_UYVY) continue;
            for (unsigned fault = 0; fault < 4; ++fault) {
                if ((fault == 1 && source != OUTPUT_MODE420_NV12) ||
                    (fault == 3 && destination != OUTPUT_MODE420_NV12)) continue;
                Conversion copy(source, destination, 16, 4, 0, 0, 0, false, 0, 0, 1, baseline);
                if (fault == 0) --copy.input.YBuffDoneSz;
                if (fault == 1) --copy.input.UVBuffDoneSz;
                if (fault == 2) --copy.output.YbuffSz;
                if (fault == 3) --copy.output.UVbuffSz;
                copy.Reject(BC_STS_IO_XFR_ERROR);
            }
        }
}

static void Invalid()
{
    for (BC_OUTPUT_FORMAT source : modes)
        for (BC_OUTPUT_FORMAT destination : modes) {
            for (unsigned fault = 0; fault < 4; ++fault) {
                if ((fault == 1 && source != OUTPUT_MODE420_NV12) ||
                    (fault == 3 && destination != OUTPUT_MODE420_NV12)) continue;
                Conversion copy(source, destination);
                if (fault == 0) copy.input.Ybuff = nullptr;
                if (fault == 1) copy.input.UVbuff = nullptr;
                if (fault == 2) copy.output.Ybuff = nullptr;
                if (fault == 3) copy.output.UVbuff = nullptr;
                copy.Reject(BC_STS_INV_ARG);
            }
            for (unsigned fault = 0; fault < 2; ++fault) {
                Conversion copy(source, destination);
                if (fault == 0) copy.context.b422Mode = OUTPUT_MODE_INVALID;
                else copy.output.b422Mode = OUTPUT_MODE_INVALID;
                copy.Reject(BC_STS_INV_ARG);
            }
            for (unsigned fault = 0; fault < 6; ++fault) {
                Conversion copy(source, destination);
                if (fault == 0) copy.input.PicInfo.width = 0;
                if (fault == 1) copy.input.PicInfo.width = 15;
                if (fault == 2) copy.input.PicInfo.height = 0;
                if (fault == 3) copy.context.HWOutPicWidth = 0;
                if (fault == 4) copy.context.HWOutPicWidth = 14;
                if (fault == 5) copy.context.HWOutPicWidth = UINT_MAX;
                copy.Reject(BC_STS_IO_XFR_ERROR);
            }
            for (unsigned fault = 0; fault < 7; ++fault) {
                Conversion copy(source, destination, 16, 4, 0, 0, 0, false, 16, 4);
                if (fault == 0) copy.output.PicInfo.width = 0;
                if (fault == 1) copy.output.PicInfo.width = 15;
                if (fault == 2) copy.output.PicInfo.width = 18;
                if (fault == 3) copy.output.PicInfo.height = 0;
                if (fault == 4) copy.output.PicInfo.height = 6;
                if (fault == 5) copy.output.PicInfo.width = UINT_MAX - 1;
                if (fault == 6) copy.output.PicInfo.height = UINT_MAX;
                copy.Reject(BC_STS_INV_ARG);
            }
            for (unsigned fault = 0; fault < 2; ++fault) {
                Conversion copy(source, destination);
                copy.output.PoutFlags |= fault ? BC_POUT_FLAGS_STRIDE_UV : BC_POUT_FLAGS_STRIDE;
                if (fault) copy.output.StrideSzUV = UINT_MAX;
                else copy.output.StrideSz = UINT_MAX;
                if (fault && destination != OUTPUT_MODE420_NV12) copy.Success();
                else copy.Reject(BC_STS_IO_XFR_ERROR);
            }
            {
                Conversion copy(source, destination, 16, 8, 0, 0, 0, true);
                copy.input.PicInfo.height = 7;
                copy.Reject(BC_STS_IO_XFR_ERROR);
            }
            {
                Conversion copy(source, destination, 16, 8, 0, 0, 0, true, 16, 4);
                copy.output.PicInfo.height = 3;
                copy.Reject(BC_STS_INV_ARG);
            }
        }
    Conversion copy(OUTPUT_MODE420_NV12, OUTPUT_MODE422_YUY2);
    Check(DtsCopyFormat(nullptr, &copy.output, &copy.input) == BC_STS_INV_ARG, "null context rejected");
    Check(DtsCopyFormat(&copy.context, nullptr, &copy.input) == BC_STS_INV_ARG, "null output rejected");
    Check(DtsCopyFormat(&copy.context, &copy.output, nullptr) == BC_STS_INV_ARG, "null input rejected");
}

static void OutputStatistics()
{
    // Use the real DIL counter storage locally, without shared-memory/device
    // initialization. DtsProcOutput calls this same stats function after copy.
    bc_dil_glob_s globals = {};
    bc_dil_glob_ptr = &globals;
    for (BC_OUTPUT_FORMAT source : modes)
        for (BC_OUTPUT_FORMAT destination : modes)
            for (unsigned fault = 0; fault < 3; ++fault) {
                if (fault == 2 && source != OUTPUT_MODE420_NV12) continue;
                Conversion copy(source, destination);
                copy.context.CapState = 2;
                if (fault == 1) copy.input.YBuffDoneSz = 0;
                if (fault == 2) copy.input.UVBuffDoneSz = 0;
                if (fault) copy.Reject(BC_STS_IO_XFR_ERROR);
                else copy.Success();
                const BC_DTS_PROC_OUT before = copy.output;
                globals.stats.opFrameCaptured = 5;
                globals.stats.opFrameDropped = 7;
                DtsUpdateOutStats(&copy.context, &copy.output);
                Check(globals.stats.opFrameCaptured == (fault ? 5U : 6U) &&
                          globals.stats.opFrameDropped == (fault ? 8U : 7U),
                      "stats classify retained transfer counts using the hardware source format");
                Check(copy.output.b422Mode == before.b422Mode &&
                          copy.output.YBuffDoneSz == before.YBuffDoneSz &&
                          copy.output.UVBuffDoneSz == before.UVBuffDoneSz,
                      "stats do not change requested mode or source transfer metadata");
            }
    bc_dil_glob_ptr = nullptr;
}

int main(int argc, char **argv)
{
    const bool baseline = argc == 2 && std::strcmp(argv[1], "--baseline-safe") == 0;
    if (argc != 1 && !baseline) return 2;
    // The legacy SIMD loops subtract 7/15 from unsigned small widths and
    // dereference required planes without validation. Never run those unsafe
    // negative cases against the old implementation just to demonstrate FAIL.
    Matrix(baseline);
    CropsAndFields(baseline);
    Capacity(baseline);
    if (!baseline) {
        Invalid();
        OutputStatistics();
    }
    std::printf("Library format: %u cases, %u checks, %u failures%s\n", cases, checks, failures,
                baseline ? " (bounded baseline cases only)" : "");
    return failures ? 1 : 0;
}
