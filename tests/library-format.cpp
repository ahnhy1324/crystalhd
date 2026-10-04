// SPDX-License-Identifier: LGPL-2.1-or-later
// Actual BC_POUT_FLAGS_MODE conversion, without device access. Unlike the raw
// copy API, MODE StrideSz/StrideSzUV are extra BYTES per destination row.
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <vector>
#include "7411d.h"
#include "libcrystalhd_if.h"
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

// Real fetched host layout: packed YbuffSz is zero, the registration has a
// separate byte capacity, and completed transfer sizes remain DWORD counts.
// Fixed, unequal literals make byte-order checks independent of the helper.
struct PackedLease {
    DTS_LIB_CONTEXT context = {};
    BC_DTS_PROC_OUT output = {};
    unsigned width, rows, pitch, prefix, allocation;
    std::vector<uint8_t> storage, original;

    PackedLease(unsigned w = 6, unsigned h = 3, unsigned padding = 2,
                bool field = false, unsigned alignment = 0)
        : width(w), rows(field ? h / 2 : h), pitch((w + padding) * 2),
          prefix(32 + alignment), allocation(Dwords(Extent(pitch, w * 2, rows)) * 4)
    {
        ++cases;
        std::snprintf(description, sizeof(description),
                      "leased packing %ux%u pitch+%u field=%u alignment=%u",
                      w, h, padding, field, alignment);
        context.DevId = BC_PCI_DEVID_FLEA;
        context.b422Mode = OUTPUT_MODE422_YUY2;
        context.softwareUyvy = true;
        context.VidParams.Progressive = !field;
        context.HWOutPicWidth = w + padding;
        output.PicInfo.width = w;
        output.PicInfo.height = h;
        output.PicInfo.flags = field ? VDEC_FLAG_INTERLACED_SRC | VDEC_FLAG_TOPFIELD : 0;
        output.PicInfo.timeStamp = 12300000;
        output.PicInfo.picture_number = 7;
        output.PoutFlags = BC_POUT_FLAGS_PIB_VALID;
        output.YBuffDoneSz = allocation / 4;
        output.b422Mode = OUTPUT_MODE_INVALID;
        storage.assign(prefix + allocation + 64, untouched);
        output.Ybuff = storage.data() + prefix;
        static const uint8_t yuy2[] = {10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120};
        for (unsigned y = 0; y < rows; ++y)
            for (unsigned x = 0; x < width * 2; ++x)
                output.Ybuff[y * pitch + x] = yuy2[x % sizeof(yuy2)];
        original = storage;
    }

    BC_STATUS Run() { return DtsPrepareOutputPacking(&context, &output, allocation); }

    void Expect(bool converted, BC_OUTPUT_FORMAT reported)
    {
        std::vector<uint8_t> expected = original;
        static const uint8_t uyvy[] = {20, 10, 40, 30, 60, 50, 80, 70, 100, 90, 120, 110};
        if (converted)
            for (unsigned y = 0; y < rows; ++y)
                for (unsigned x = 0; x < width * 2; ++x)
                    expected[prefix + y * pitch + x] = uyvy[x % sizeof(uyvy)];
        Check(storage == expected,
              "literal active pixels are exact; row padding, PIB/trailer and both canaries untouched");
        Check(output.b422Mode == reported && context.b422Mode == OUTPUT_MODE422_YUY2 &&
                  output.YbuffSz == 0 && output.YBuffDoneSz == allocation / 4,
              "reported format changes without changing source or real packed transfer metadata");
    }

    void Success()
    {
        const BC_DTS_PROC_OUT before = output;
        Check(Run() == BC_STS_SUCCESS, "valid host lease converts in place");
        Expect(true, OUTPUT_MODE422_UYVY);
        BC_DTS_PROC_OUT expected = before;
        expected.b422Mode = OUTPUT_MODE422_UYVY;
        Check(std::memcmp(&output, &expected, sizeof(output)) == 0,
              "conversion changes no pointers, PIB, timestamps, flags, counts or other metadata");
    }

    void Reject(BC_STATUS status)
    {
        const BC_DTS_PROC_OUT before = output;
        Check(Run() == status, "invalid/unsupported host lease returns the specified status");
        Check(storage == original, "all checks finish before any active pixel mutation");
        BC_DTS_PROC_OUT expected = before;
        expected.b422Mode = context.b422Mode;
        Check(std::memcmp(&output, &expected, sizeof(output)) == 0,
              "rejection retains source enum and every other output field");
    }
};

static void LeasedPacking()
{
    for (unsigned width : {2U, 6U, 8U, 14U, 16U, 18U, 32U})
        for (unsigned padding : {0U, 1U, 4U})
            for (unsigned alignment : {0U, 1U, 7U, 15U}) {
                PackedLease progressive(width, 3, padding, false, alignment);
                progressive.Success();
                PackedLease field(width, 6, padding, true, alignment);
                field.Success();
            }
    for (unsigned height : {1U, 1088U}) {
        PackedLease minimum(2, height);
        minimum.Success();
    }
    PackedLease maximum(1920, 1088, 0);
    maximum.Success();
    for (unsigned fault = 0; fault < 18; ++fault) {
        PackedLease bad;
        if (fault == 0) bad.output.PoutFlags = 0;
        if (fault == 1) bad.output.Ybuff = nullptr;
        if (fault == 2) bad.output.PicInfo.width = 0;
        if (fault == 3) bad.output.PicInfo.width = 5;
        if (fault == 4) bad.output.PicInfo.width = 1922;
        if (fault == 5) bad.output.PicInfo.height = 0;
        if (fault == 6) bad.output.PicInfo.height = 1089;
        if (fault == 7) bad.context.HWOutPicWidth = 5;
        if (fault == 8) bad.context.HWOutPicWidth = 1921;
        if (fault == 9) bad.context.VidParams.Progressive = false;
        if (fault == 10) --bad.output.YBuffDoneSz;
        if (fault == 11) --bad.allocation;
        if (fault == 12) bad.allocation = 0;
        if (fault == 13) bad.output.YBuffDoneSz = 0;
        if (fault == 14) bad.output.YBuffDoneSz = UINT_MAX;
        if (fault == 15) bad.context.HWOutPicWidth = UINT_MAX;
        if (fault == 16) bad.output.PicInfo.width = UINT_MAX;
        if (fault == 17) bad.output.PicInfo.height = UINT_MAX;
        bad.Reject(BC_STS_IO_XFR_ERROR);
    }
    for (unsigned fault = 0; fault < 3; ++fault) {
        PackedLease bad;
        if (fault == 0) bad.context.DevId = BC_PCI_DEVID_LINK;
        if (fault == 1) bad.context.b422Mode = OUTPUT_MODE420_NV12;
        if (fault == 2) bad.context.b422Mode = OUTPUT_MODE422_UYVY;
        bad.Reject(BC_STS_INV_ARG);
    }
    PackedLease encrypted;
    encrypted.output.PoutFlags |= BC_POUT_FLAGS_ENCRYPTED;
    encrypted.Reject(BC_STS_NOT_IMPL);
    for (bool format : {false, true}) {
        PackedLease notification;
        if (format) notification.output.PoutFlags = BC_POUT_FLAGS_FMT_CHANGE;
        else notification.output.PicInfo.flags |= VDEC_FLAG_EOS;
        notification.output.Ybuff = nullptr;
        notification.output.PicInfo.width = notification.output.PicInfo.height = 0;
        notification.output.YBuffDoneSz = 0;
        const BC_DTS_PROC_OUT before = notification.output;
        Check(DtsPrepareOutputPacking(&notification.context, &notification.output, 0) == BC_STS_SUCCESS &&
                  notification.storage == notification.original,
              "FMT/EOS notifications expose requested format without touching pixel memory");
        BC_DTS_PROC_OUT expected = before;
        expected.b422Mode = OUTPUT_MODE422_UYVY;
        Check(std::memcmp(&notification.output, &expected, sizeof(expected)) == 0,
              "notification keeps all non-format fields unchanged");
    }
    for (BC_OUTPUT_FORMAT source : modes) {
        PackedLease legacy;
        legacy.context.softwareUyvy = false;
        legacy.context.b422Mode = source;
        legacy.context.DevId = BC_PCI_DEVID_LINK;
        const BC_DTS_PROC_OUT before = legacy.output;
        Check(DtsPrepareOutputPacking(&legacy.context, &legacy.output, 0) == BC_STS_SUCCESS &&
                  legacy.storage == legacy.original,
              "disabled software packing leaves each legacy source's bytes alone");
        BC_DTS_PROC_OUT expected = before;
        expected.b422Mode = source;
        Check(std::memcmp(&legacy.output, &expected, sizeof(expected)) == 0,
              "legacy no-op stamps only its actual source enum");
    }
    PackedLease null;
    Check(DtsPrepareOutputPacking(nullptr, &null.output, null.allocation) == BC_STS_INV_ARG &&
              null.storage == null.original && null.output.b422Mode == OUTPUT_MODE_INVALID,
          "null context rejects without mutation");
    Check(DtsPrepareOutputPacking(&null.context, nullptr, null.allocation) == BC_STS_INV_ARG &&
              null.storage == null.original,
          "null output rejects without mutation");
}

static void PreparedModeConversions()
{
    for (BC_OUTPUT_FORMAT destination : modes)
        for (bool field : {false, true}) {
            Conversion copy(OUTPUT_MODE422_YUY2, destination, 18, 6, 4,
                            3, 7, field, 14, field ? 4 : 5, 1);
            copy.context.DevId = BC_PCI_DEVID_FLEA;
            copy.context.softwareUyvy = true;
            Check(DtsPrepareOutputPacking(&copy.context, &copy.input,
                      copy.input.YBuffDoneSz * 4) == BC_STS_SUCCESS,
                  "prepare the actual shared host source before MODE output conversion");
            copy.source_mode = OUTPUT_MODE422_UYVY;
            copy.original_y = copy.source_y;
            copy.Success();
            Check(copy.context.b422Mode == OUTPUT_MODE422_YUY2 &&
                      copy.input.b422Mode == OUTPUT_MODE422_UYVY,
                  "MODE follows the prepared input enum without changing capture source");
        }
    Conversion invalid(OUTPUT_MODE422_YUY2, OUTPUT_MODE422_YUY2);
    invalid.context.softwareUyvy = true;
    invalid.input.b422Mode = OUTPUT_MODE_INVALID;
    invalid.Reject(BC_STS_INV_ARG);
}

// This symbol interposes only this device-free executable's final syscall.
// No real descriptor is opened: any unrecognised fd/command aborts the test.
static DTS_LIB_CONTEXT *fetch_context;
static BC_DEC_OUT_BUFF fetched_output;
static BC_STATUS fetch_status, repost_status;
static bool fetch_syscall_failure, repost_syscall_failure, cancel_fetch;
static unsigned fetch_calls, repost_calls;
static const std::vector<uint8_t> *fetch_original;
static unsigned fetch_original_prefix, fetch_capacity;

extern "C" int ioctl(int fd, unsigned long code, ...) noexcept
{
    if (fd != 99 || !fetch_context ||
        (code != BCM_IOC_FETCH_RXBUFF && code != BCM_IOC_ADD_RXBUFFS)) {
        std::fputs("unexpected ioctl in device-free leased-packing test\n", stderr);
        std::abort();
    }
    va_list args;
    va_start(args, code);
    BC_IOCTL_DATA *data = va_arg(args, BC_IOCTL_DATA *);
    va_end(args);
    Check(data == fetch_context->pOutData && fetch_context->ProcOutPending == 1,
          "fetch and retirement retain the one admitted shared output owner");
    if (code == BCM_IOC_FETCH_RXBUFF) {
        ++fetch_calls;
        Check(fetched_output.OutPutBuffs.YuvBuffSz <= fetch_capacity,
              "fake transfer stays inside the owned registration fixture");
        if (fetched_output.OutPutBuffs.YuvBuffSz <= fetch_capacity)
            std::memcpy(fetched_output.OutPutBuffs.YuvBuff,
                        fetch_original->data() + fetch_original_prefix,
                        fetched_output.OutPutBuffs.YuvBuffSz);
        data->u.DecOutData = fetched_output;
        data->RetSts = fetch_status;
        if (cancel_fetch) fetch_context->CancelWaiting = true;
        if (fetch_syscall_failure) { errno = EIO; return -1; }
    } else {
        ++repost_calls;
        Check(data->u.RxBuffs.YuvBuff == fetched_output.OutPutBuffs.YuvBuff &&
                  data->u.RxBuffs.YuvBuffSz == fetched_output.OutPutBuffs.YuvBuffSz &&
                  data->u.RxBuffs.b422Mode == OUTPUT_MODE422_YUY2 &&
                  data->u.RxBuffs.YBuffDoneSz == 0 && data->u.RxBuffs.UVBuffDoneSz == 0,
              "retirement reposts the original registration as hardware YUY2, not requested UYVY");
        data->RetSts = repost_status;
        if (repost_syscall_failure) { errno = EIO; return -1; }
    }
    return 0;
}

struct FetchFixture {
    PackedLease lease;
    BC_IOCTL_DATA out_data = {};
    bc_dil_glob_s globals = {};

    FetchFixture()
    {
        lease.context.Sig = LIB_CTX_SIG;
        lease.context.DevHandle = 99;
        lease.context.State = BC_DEC_STATE_START;
        lease.context.pOutData = &out_data;
        lease.context.RegCfg.DbgOptions = BC_BIT(6);
        lease.context.CapState = 2;
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&lease.context.thLock, &attr);
        pthread_mutexattr_destroy(&attr);
        fetch_context = &lease.context;
        bc_dil_glob_ptr = &globals;
        fetch_original = &lease.original;
        fetch_original_prefix = lease.prefix;
        fetch_capacity = lease.allocation;
        fetched_output = BC_DEC_OUT_BUFF{};
        fetched_output.Flags = COMP_FLAG_DATA_VALID | COMP_FLAG_PIB_VALID;
        fetched_output.PibInfo.ppb.width = lease.width;
        fetched_output.PibInfo.ppb.height = lease.output.PicInfo.height;
        fetched_output.OutPutBuffs.YuvBuff = lease.output.Ybuff;
        fetched_output.OutPutBuffs.YuvBuffSz = lease.allocation;
        fetched_output.OutPutBuffs.b422Mode = OUTPUT_MODE422_YUY2;
        fetched_output.OutPutBuffs.YBuffDoneSz = lease.output.YBuffDoneSz;
        fetch_status = repost_status = BC_STS_SUCCESS;
        fetch_syscall_failure = repost_syscall_failure = cancel_fetch = false;
        fetch_calls = repost_calls = 0;
    }
    BC_STATUS Run() { return DtsFetchOutInterruptible(&lease.context, &lease.output, 0); }
    void EmbeddedMarker(bool eos)
    {
        lease.allocation = 512;
        lease.storage.resize(lease.prefix + lease.allocation + 64, untouched);
        lease.output.Ybuff = lease.storage.data() + lease.prefix;
        lease.output.YBuffDoneSz = lease.allocation / 4;
        lease.context.FixFlags = DTS_LOAD_FILE_PLAY_FW;
        lease.context.RegCfg.DbgOptions = 0;
        lease.context.HWOutPicHeight = lease.output.PicInfo.height;
        BC_PIC_INFO_BLOCK picture = {};
        picture.width = lease.width;
        picture.height = lease.output.PicInfo.height;
        picture.picture_number = 7;
        picture.flags = eos ? VDEC_FLAG_EOS : 0;
        picture.timeStamp = 123;
        // Original active first word is 10,20,30,40. The real Flea parser
        // repairs it from ycom before the shared packing helper runs.
        picture.ycom = 0x281e140aU;
        const uint32_t marker = eos ? BC_EOS_DETECTED : lease.rows;
        std::memcpy(lease.output.Ybuff, &marker, sizeof(marker));
        const unsigned pib_offset = eos ? 0 : lease.rows * lease.pitch;
        if (!eos) std::memcpy(lease.output.Ybuff + pib_offset, &picture.picture_number, 4);
        std::memcpy(lease.output.Ybuff + pib_offset + 4, &picture, sizeof(picture));
        lease.original = lease.storage;
        fetched_output.Flags = COMP_FLAG_DATA_VALID;
        fetched_output.OutPutBuffs.YuvBuff = lease.output.Ybuff;
        fetched_output.OutPutBuffs.YuvBuffSz = lease.allocation;
        fetched_output.OutPutBuffs.YBuffDoneSz = lease.output.YBuffDoneSz;
        fetch_capacity = lease.allocation;
    }
    ~FetchFixture()
    {
        fetch_context = nullptr;
        fetch_original = nullptr;
        bc_dil_glob_ptr = nullptr;
        pthread_mutex_destroy(&lease.context.thLock);
    }
};

static void FetchPackingOwnership()
{
    {
        FetchFixture good;
        Check(good.Run() == BC_STS_SUCCESS && fetch_calls == 1 && repost_calls == 0 &&
                  good.lease.context.ProcOutPending == 1,
              "successful fetch returns exactly one unreleased converted host lease");
        good.lease.Expect(true, OUTPUT_MODE422_UYVY);
        const std::vector<uint8_t> converted = good.lease.storage;
        BC_DTS_PROC_OUT competing = {};
        Check(DtsFetchOutInterruptible(&good.lease.context, &competing, 0) == BC_STS_BUSY &&
                  fetch_calls == 1 && repost_calls == 0 && good.lease.storage == converted,
              "second fetch cannot convert twice or reuse a caller-owned lease");
        Check(DtsRelRxBuff(&good.lease.context, &good.out_data.u.RxBuffs, FALSE) == BC_STS_SUCCESS &&
                  repost_calls == 1 && good.lease.context.ProcOutPending == 0,
              "successful release retires the sole pending owner exactly once");
    }
    for (int raw_status = -1; raw_status <= BC_STS_PWR_MGMT; ++raw_status) {
        FetchFixture rejected;
        fetched_output.PibInfo.ppb.width = 5;
        repost_status = static_cast<BC_STATUS>(raw_status);
        const bool retired = raw_status == BC_STS_SUCCESS;
        const BC_STATUS expected = retired ? BC_STS_IO_XFR_ERROR : repost_status;
        Check(rejected.Run() == expected && fetch_calls == 1 && repost_calls == 1 &&
                  rejected.lease.context.ProcOutPending == (retired ? 0 : 1),
              "packing failure reposts once and clears pending only on acknowledged retirement");
        Check(rejected.lease.output.Ybuff == nullptr && rejected.lease.output.UVbuff == nullptr &&
                  rejected.lease.output.b422Mode == OUTPUT_MODE422_YUY2 &&
                  rejected.lease.storage == rejected.lease.original,
              "rejected packing exposes no caller-owned pixels and never mutates their bytes");
        if (!retired) {
            BC_DTS_PROC_OUT next = {};
            Check(DtsFetchOutInterruptible(&rejected.lease.context, &next, 0) == BC_STS_BUSY &&
                      fetch_calls == 1 && repost_calls == 1,
                  "failed retirement preserves the pending owner and blocks a new fetch");
        }
    }
    for (unsigned fault = 0; fault < 4; ++fault) {
        FetchFixture bad;
        if (fault == 0) fetched_output.Flags |= COMP_FLAG_DATA_ENC;
        if (fault == 1) --fetched_output.OutPutBuffs.YuvBuffSz;
        if (fault == 2) fetched_output.OutPutBuffs.UVbuffOffset = 4;
        if (fault == 3) fetched_output.OutPutBuffs.UVBuffDoneSz = 1;
        Check(bad.Run() == (fault == 0 ? BC_STS_NOT_IMPL : BC_STS_IO_XFR_ERROR) &&
                  repost_calls == 1 && bad.lease.context.ProcOutPending == 0 &&
                  bad.lease.output.Ybuff == nullptr && bad.lease.storage == bad.lease.original,
              "encrypted/corrupt fetched source retires without conversion or returned ownership");
    }
    {
        FetchFixture bad;
        fetched_output.PibInfo.ppb.width = 5;
        repost_syscall_failure = true;
        Check(bad.Run() == BC_STS_ERROR && repost_calls == 1 &&
                  bad.lease.context.ProcOutPending == 1 && bad.lease.output.Ybuff == nullptr &&
                  bad.lease.storage == bad.lease.original,
              "ambiguous repost syscall failure retains pending ownership and original bytes");
    }
    for (int raw_status = -1; raw_status <= BC_STS_PWR_MGMT; ++raw_status) {
        if (raw_status == BC_STS_SUCCESS) continue;
        FetchFixture failed;
        fetch_status = static_cast<BC_STATUS>(raw_status);
        Check(failed.Run() == fetch_status && fetch_calls == 1 && repost_calls == 0 &&
                  failed.lease.context.ProcOutPending == 0 &&
                  failed.lease.storage == failed.lease.original,
              "failed driver fetch admits no caller-owned picture and does not attempt conversion/repost");
    }
    {
        FetchFixture failed;
        fetch_syscall_failure = true;
        Check(failed.Run() == BC_STS_ERROR && repost_calls == 0 &&
                  failed.lease.context.ProcOutPending == 0 &&
                  failed.lease.storage == failed.lease.original,
              "failed fetch syscall does not transfer or convert output ownership");
    }
    {
        FetchFixture cancelled;
        cancel_fetch = true;
        Check(cancelled.Run() == BC_STS_IO_USER_ABORT && fetch_calls == 1 && repost_calls == 1 &&
                  cancelled.lease.context.ProcOutPending == 0 &&
                  cancelled.lease.storage == cancelled.lease.original,
              "cancellation is checked before conversion and retires the unmodified source lease");
    }
}

static unsigned callback_calls;
static BC_STATUS ObservePreparedCallback(void *handle, uint32_t width,
                                       uint32_t height, uint32_t stride, void *data)
{
    ++callback_calls;
    const BC_DTS_PROC_OUT *output = static_cast<BC_DTS_PROC_OUT *>(data);
    static const uint8_t uyvy[] = {20, 10, 40, 30, 60, 50, 80, 70, 100, 90, 120, 110};
    Check(handle == fetch_context && width == 8 && height == 3 && stride == 0 &&
              fetch_context->ProcOutPending == 1 && output->b422Mode == OUTPUT_MODE422_UYVY,
          "callback sees prepared requested format while the shared host lease is still owned");
    Check(output->Ybuff && std::memcmp(output->Ybuff, uyvy, sizeof(uyvy)) == 0,
          "callback receives literal UYVY bytes, not a relabelled YUY2 source");
    return BC_STS_SUCCESS;
}

static void PublicPackingConsumers()
{
    {
        FetchFixture no_copy;
        BC_DTS_PROC_OUT output = {};
        Check(DtsProcOutputNoCopy(&no_copy.lease.context, 0, &output) == BC_STS_SUCCESS &&
                  output.Ybuff == no_copy.lease.output.Ybuff &&
                  output.b422Mode == OUTPUT_MODE422_UYVY && fetch_calls == 1 &&
                  repost_calls == 0 && no_copy.lease.context.ProcOutPending == 1,
              "public NoCopy preserves the prepared enum and one live converted lease");
        no_copy.lease.output = output;
        no_copy.lease.Expect(true, OUTPUT_MODE422_UYVY);
        Check(DtsReleaseOutputBuffs(&no_copy.lease.context, nullptr, FALSE) == BC_STS_SUCCESS &&
                  repost_calls == 1 && no_copy.lease.context.ProcOutPending == 0,
              "public NoCopy release retires the original YUY2 registration");
    }
    for (bool mode : {false, true}) {
        for (BC_OUTPUT_FORMAT destination : modes) {
            if (!mode && destination != OUTPUT_MODE422_UYVY) continue;
            FetchFixture copy;
            const bool planar = destination == OUTPUT_MODE420_NV12;
            const unsigned y_bytes = planar ? 18 : 36, uv_bytes = planar ? 12 : 0;
            const unsigned prefix = 19;
            std::vector<uint8_t> y(prefix + Dwords(y_bytes) * 4 + 32, untouched);
            std::vector<uint8_t> uv(prefix + Dwords(uv_bytes) * 4 + 32, untouched);
            BC_DTS_PROC_OUT output = {};
            output.Ybuff = y.data() + prefix;
            output.YbuffSz = Dwords(y_bytes);
            output.UVbuff = planar ? uv.data() + prefix : nullptr;
            output.UVbuffSz = Dwords(uv_bytes);
            output.b422Mode = destination;
            output.PoutFlags = mode ? BC_POUT_FLAGS_MODE : 0;
            callback_calls = 0;
            if (!mode) {
                output.hnd = &copy.lease.context;
                output.AppCallBack = ObservePreparedCallback;
            }
            Check(DtsProcOutput(&copy.lease.context, 0, &output) == BC_STS_SUCCESS &&
                      output.b422Mode == destination && fetch_calls == 1 && repost_calls == 1 &&
                      copy.lease.context.ProcOutPending == 0 &&
                      callback_calls == (mode ? 0U : 1U),
                  "public default/callback and each MODE destination copy then retire one host lease");
            std::vector<uint8_t> expected_y(y.size(), untouched), expected_uv(uv.size(), untouched);
            static const uint8_t yuy2[] = {10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120};
            static const uint8_t uyvy[] = {20, 10, 40, 30, 60, 50, 80, 70, 100, 90, 120, 110};
            static const uint8_t luma[] = {10, 30, 50, 70, 90, 110};
            static const uint8_t chroma[] = {20, 40, 60, 80, 100, 120};
            for (unsigned row = 0; row < 3; ++row) {
                const uint8_t *literal = planar ? luma :
                    destination == OUTPUT_MODE422_UYVY ? uyvy : yuy2;
                const unsigned row_bytes = planar ? 6 : 12;
                std::memcpy(expected_y.data() + prefix + row * row_bytes, literal, row_bytes);
            }
            if (planar)
                for (unsigned row = 0; row < 2; ++row)
                    std::memcpy(expected_uv.data() + prefix + row * 6, chroma, sizeof(chroma));
            Check(y == expected_y && uv == expected_uv,
                  "public copies produce literal target bytes and preserve destination tail/canaries");
            Check(output.YBuffDoneSz == copy.lease.allocation / 4 && output.UVBuffDoneSz == 0 &&
                      copy.lease.context.b422Mode == OUTPUT_MODE422_YUY2,
                  "consumer keeps source transfer DWORD metadata and hardware registration format");
            Check(copy.globals.stats.opFrameCaptured == 1 && copy.globals.stats.opFrameDropped == 0,
                  "converted public copy counts one picture, not one per conversion stage");
        }
    }
    for (bool no_copy : {false, true}) {
        FetchFixture dropped;
        BC_DTS_PROC_OUT output = {};
        std::vector<uint8_t> pixels(36, untouched);
        output.Ybuff = pixels.data();
        output.YbuffSz = 9;
        output.DropFrames = 1;
        const BC_STATUS status = no_copy
            ? DtsProcOutputNoCopy(&dropped.lease.context, 0, &output)
            : DtsProcOutput(&dropped.lease.context, 0, &output);
        Check(status == BC_STS_SUCCESS && fetch_calls == 2 &&
                  repost_calls == (no_copy ? 1U : 2U) && output.DropFrames == 0 &&
                  dropped.lease.context.ProcOutPending == (no_copy ? 1 : 0),
              "drop path retires first picture and prepares only the separately fetched next picture");
        static const uint8_t uyvy[] = {20, 10, 40, 30, 60, 50, 80, 70, 100, 90, 120, 110};
        const uint8_t *picture = no_copy ? output.Ybuff : pixels.data();
        Check(picture && std::memcmp(picture, uyvy, sizeof(uyvy)) == 0,
              "drop refetch delivers a once-converted next picture, not a double byte swap");
        if (no_copy)
            Check(DtsReleaseOutputBuffs(&dropped.lease.context, nullptr, FALSE) == BC_STS_SUCCESS &&
                      repost_calls == 2 && dropped.lease.context.ProcOutPending == 0,
                  "NoCopy drop returns only the next lease for caller release");
    }
    for (bool no_copy : {false, true}) {
        FetchFixture format;
        fetched_output.Flags = COMP_FLAG_FMT_CHANGE | COMP_FLAG_PIB_VALID;
        BC_DTS_PROC_OUT output = {};
        output.b422Mode = OUTPUT_MODE_INVALID;
        const BC_STATUS status = no_copy
            ? DtsProcOutputNoCopy(&format.lease.context, 0, &output)
            : DtsProcOutput(&format.lease.context, 0, &output);
        Check(status == BC_STS_FMT_CHANGE && output.b422Mode == OUTPUT_MODE422_UYVY &&
                  fetch_calls == 1 && repost_calls == 0 &&
                  format.lease.context.ProcOutPending == 0 && format.lease.storage == format.lease.original,
              "public FMT notification reports requested packing without conversion or caller lease");
    }
    {
        FetchFixture format;
        fetched_output.Flags = COMP_FLAG_FMT_CHANGE | COMP_FLAG_PIB_VALID;
        BC_DTS_PROC_OUT output = {};
        output.PoutFlags = BC_POUT_FLAGS_MODE;
        output.b422Mode = OUTPUT_MODE422_YUY2;
        Check(DtsProcOutput(&format.lease.context, 0, &output) == BC_STS_FMT_CHANGE &&
                  output.b422Mode == OUTPUT_MODE422_YUY2 && repost_calls == 0 &&
                  format.lease.context.ProcOutPending == 0,
              "MODE FMT notification preserves the explicit destination request");
    }
    {
        FetchFixture eos;
        fetched_output.PibInfo.ppb.flags = VDEC_FLAG_EOS;
        BC_DTS_PROC_OUT output = {};
        Check(DtsProcOutputNoCopy(&eos.lease.context, 0, &output) == BC_STS_SUCCESS &&
                  output.b422Mode == OUTPUT_MODE422_UYVY &&
                  (output.PicInfo.flags & VDEC_FLAG_EOS) && eos.lease.storage == eos.lease.original &&
                  eos.lease.context.ProcOutPending == 1 && repost_calls == 0,
              "successful metadata EOS is not byte-swapped and retains only its actual NoCopy ownership");
        Check(DtsReleaseOutputBuffs(&eos.lease.context, nullptr, FALSE) == BC_STS_SUCCESS &&
                  eos.lease.context.ProcOutPending == 0 && repost_calls == 1,
              "metadata EOS success still retires its returned lease once");
    }
    for (bool no_copy : {false, true}) {
        FetchFixture cancelled;
        cancel_fetch = true;
        BC_DTS_PROC_OUT output = {};
        const BC_STATUS status = no_copy
            ? DtsProcOutputNoCopy(&cancelled.lease.context, 0, &output)
            : DtsProcOutput(&cancelled.lease.context, 0, &output);
        Check(status == BC_STS_IO_USER_ABORT && fetch_calls == 1 && repost_calls == 1 &&
                  cancelled.lease.context.ProcOutPending == 0 &&
                  cancelled.lease.storage == cancelled.lease.original,
              "both public consumers honor cancellation before any software pixel mutation");
    }
    {
        FetchFixture repaired;
        repaired.EmbeddedMarker(false);
        BC_DTS_PROC_OUT output = {};
        Check(DtsProcOutputNoCopy(&repaired.lease.context, 0, &output) == BC_STS_SUCCESS &&
                  output.b422Mode == OUTPUT_MODE422_UYVY &&
                  output.PicInfo.picture_number == 7 && output.PicInfo.timeStamp == 2460000,
              "real embedded Flea PIB parsing and first-word repair precede packing");
        repaired.lease.output = output;
        repaired.lease.Expect(true, OUTPUT_MODE422_UYVY);
        Check(DtsReleaseOutputBuffs(&repaired.lease.context, nullptr, FALSE) == BC_STS_SUCCESS &&
                  repost_calls == 1 && repaired.lease.context.ProcOutPending == 0,
              "embedded-PIB picture still retires its original registration once");
    }
    for (bool no_copy : {false, true}) {
        FetchFixture eos;
        eos.EmbeddedMarker(true);
        BC_DTS_PROC_OUT output = {};
        const BC_STATUS status = no_copy
            ? DtsProcOutputNoCopy(&eos.lease.context, 0, &output)
            : DtsProcOutput(&eos.lease.context, 0, &output);
        Check(status == BC_STS_NO_DATA && (output.PicInfo.flags & VDEC_FLAG_EOS) &&
                  eos.lease.context.bEOS && eos.lease.context.ProcOutPending == 0 &&
                  fetch_calls == 1 && repost_calls == 0 && eos.lease.storage == eos.lease.original,
              "real embedded EOS is parsed as no-data before conversion, with no returned output lease");
    }
    for (bool no_copy : {false, true}) {
        for (bool retire : {false, true}) {
            FetchFixture rejected;
            fetched_output.PibInfo.ppb.width = 5;
            repost_status = retire ? BC_STS_SUCCESS : BC_STS_IO_ERROR;
            BC_DTS_PROC_OUT output = {};
            output.DropFrames = 1;
            const BC_STATUS status = no_copy
                ? DtsProcOutputNoCopy(&rejected.lease.context, 0, &output)
                : DtsProcOutput(&rejected.lease.context, 0, &output);
            Check(status == (retire ? BC_STS_IO_XFR_ERROR : BC_STS_IO_ERROR) &&
                      output.DropFrames == 1 && fetch_calls == 1 && repost_calls == 1 &&
                      rejected.lease.context.ProcOutPending == (retire ? 0 : 1) &&
                      rejected.lease.storage == rejected.lease.original,
                  "both consumers preserve failed retirement and never drop/release another caller's output");
        }
    }
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
        LeasedPacking();
        PreparedModeConversions();
        FetchPackingOwnership();
        PublicPackingConsumers();
    }
    std::printf("Library format: %u cases, %u checks, %u failures%s\n", cases, checks, failures,
                baseline ? " (bounded baseline cases only)" : "");
    return failures ? 1 : 0;
}
