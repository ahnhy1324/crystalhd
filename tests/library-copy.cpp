// SPDX-License-Identifier: LGPL-2.1-or-later
// Hardware-free tests linked to the actual raw YUY2 copy implementation.
// Buffer lengths and completed transfer lengths use the legacy DWORD units;
// StrideSz is extra destination pixels per copied line, not the full pitch.
#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <vector>
#include "7411d.h"
#include "libcrystalhd_int_if.h"
#include "libcrystalhd_priv.h"

static unsigned failures, checks;
static void Check(bool condition, const char *message)
{
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

struct Copy {
    DTS_LIB_CONTEXT context = {};
    BC_DTS_PROC_OUT input = {}, output = {};
    // Extra backing keeps deliberately understated-length regressions safe
    // even on the old implementation; every unwritten byte is checked below.
    std::vector<uint8_t> source = std::vector<uint8_t>(512, 0xd3);
    std::vector<uint8_t> destination = std::vector<uint8_t>(512, 0xa5);

    Copy(unsigned pitch = 8, bool field = false) {
        context.b422Mode = OUTPUT_MODE422_YUY2;
        context.VidParams.Progressive = !field;
        context.HWOutPicWidth = pitch;
        input.PicInfo.width = 8;
        input.PicInfo.height = 6;
        input.PicInfo.flags = field ? VDEC_FLAG_INTERLACED_SRC | VDEC_FLAG_TOPFIELD : 0;
        input.PoutFlags = BC_POUT_FLAGS_PIB_VALID;
        input.Ybuff = source.data();
        input.YBuffDoneSz = pitch * (field ? 3 : 6) / 2;
        output.Ybuff = destination.data();
        output.YbuffSz = 8 * 6 / 2;
        Fill(pitch, field ? 3 : 6);
    }
    void Fill(unsigned pitch, unsigned rows, uint8_t base = 0x10) {
        for (unsigned y = 0; y < rows; ++y)
            for (unsigned x = 0; x < 16; ++x)
                source[y * pitch * 2 + x] = static_cast<uint8_t>(base + y * 17 + x);
    }
    BC_STATUS Run() { return DtsCopyRawDataToOutBuff(&context, &output, &input); }
    void Expect(unsigned bytes, unsigned rows, unsigned source_pitch,
                unsigned destination_pitch, unsigned offset = 0) {
        std::vector<uint8_t> expected(destination.size(), 0xa5);
        for (unsigned y = 0; y < rows; ++y)
            std::memcpy(expected.data() + offset + y * destination_pitch,
                        source.data() + y * source_pitch, bytes);
        Check(destination == expected, "copy preserves exact row identity and all padding/canaries");
    }
    void Reject(BC_STATUS status) {
        Check(Run() == status, "invalid layout returns the expected failure");
        Check(std::all_of(destination.begin(), destination.end(),
                          [](uint8_t byte) { return byte == 0xa5; }),
              "invalid layout is rejected before any destination write");
    }
};

static void Rows()
{
    for (unsigned pitch : {8U, 12U}) {
        Copy copy(pitch);
        Check(copy.Run() == BC_STS_SUCCESS, "no-SIZE progressive copy succeeds");
        copy.Expect(16, 6, pitch * 2, 16);
        Check(copy.output.YBuffDoneSz == copy.input.YBuffDoneSz,
              "legacy completed-transfer metadata remains in DWORD units");
    }
    Copy cropped(12);
    cropped.output.PoutFlags = BC_POUT_FLAGS_SIZE | BC_POUT_FLAGS_STRIDE;
    cropped.output.PicInfo.width = 6;
    cropped.output.PicInfo.height = 4;
    cropped.output.StrideSz = 4;
    cropped.output.YbuffSz = 18; // (3 * 20 + 12) / 4: no unused last-row padding.
    Check(cropped.Run() == BC_STS_SUCCESS, "SIZE crops visible rows with independent destination padding");
    cropped.Expect(12, 4, 24, 20);

    for (bool size : {false, true}) {
        for (unsigned bottom : {0U, 1U}) {
            Copy copy(8, true);
            copy.input.PicInfo.flags = VDEC_FLAG_INTERLACED_SRC |
                (bottom ? VDEC_FLAG_BOTTOMFIELD : VDEC_FLAG_TOPFIELD);
            copy.output.PoutFlags = BC_POUT_FLAGS_STRIDE | (size ? BC_POUT_FLAGS_SIZE : 0);
            copy.output.PicInfo.width = 8;
            copy.output.PicInfo.height = 6;
            copy.output.StrideSz = 8; // Weave: skip the other field's row.
            copy.output.Ybuff += bottom * 16;
            copy.output.YbuffSz -= bottom * 4;
            Check(copy.Run() == BC_STS_SUCCESS, "field copy succeeds with and without SIZE");
            copy.Expect(16, 3, 16, 32, bottom * 16);
        }
    }
}

static void FieldPairs()
{
    for (unsigned first : {0U, 1U}) {
        Copy copy(8, true);
        copy.output.PoutFlags = BC_POUT_FLAGS_STRIDE;
        copy.output.StrideSz = 8;
        std::vector<uint8_t> expected(copy.destination.size(), 0xa5);
        for (unsigned part = 0; part < 2; ++part) {
            unsigned bottom = first ^ part;
            copy.Fill(8, 3, static_cast<uint8_t>(part ? 0x70 : 0x10));
            copy.output.Ybuff = copy.destination.data() + bottom * 16;
            copy.output.YbuffSz = 24 - bottom * 4;
            copy.input.PicInfo.flags = VDEC_FLAG_INTERLACED_SRC |
                (bottom ? VDEC_FLAG_BOTTOMFIELD : VDEC_FLAG_TOPFIELD);
            for (unsigned y = 0; y < 3; ++y)
                std::memcpy(expected.data() + (y * 2 + bottom) * 16,
                            copy.source.data() + y * 16, 16);
            Check(copy.Run() == BC_STS_SUCCESS, "both field delivery orders copy successfully");
        }
        Check(copy.destination == expected, "complementary fields weave without overwriting the other field");
    }
}

static void Capacity()
{
    {
        Copy copy(12);
        copy.output.PoutFlags = BC_POUT_FLAGS_STRIDE;
        copy.output.StrideSz = 4;
        copy.output.YbuffSz = 34;
        copy.input.YBuffDoneSz = 34;
        Check(copy.Run() == BC_STS_SUCCESS, "exact last touched source/destination byte is sufficient");
        copy.Expect(16, 6, 24, 24);
    }
    for (unsigned fault = 0; fault < 3; ++fault) {
        Copy copy(12);
        copy.output.PoutFlags = BC_POUT_FLAGS_STRIDE;
        copy.output.StrideSz = 4;
        copy.output.YbuffSz = fault == 0 ? 33 : 34;
        copy.input.YBuffDoneSz = fault == 1 ? 33 : 34;
        if (fault == 2) copy.input.YBuffDoneSz = 0;
        copy.Reject(BC_STS_IO_XFR_ERROR);
    }
}

static void Bounds()
{
    Capacity();
    for (unsigned fault = 0; fault < 8; ++fault) {
        Copy copy;
        copy.output.PoutFlags = BC_POUT_FLAGS_SIZE;
        copy.output.PicInfo.width = 8;
        copy.output.PicInfo.height = 6;
        if (fault == 0) copy.output.PicInfo.width = 0;
        if (fault == 1) copy.output.PicInfo.height = 0;
        if (fault == 2) copy.output.PicInfo.width = 10;
        if (fault == 3) copy.output.PicInfo.height = 8;
        if (fault == 4) copy.output.PicInfo.width = 7;
        if (fault == 5) copy.output.PicInfo.width = UINT_MAX;
        if (fault == 6) copy.output.PicInfo.height = UINT_MAX;
        if (fault == 7) copy.output.PicInfo.width = UINT_MAX - 1;
        copy.Reject(BC_STS_INV_ARG);
    }
    for (unsigned fault = 0; fault < 6; ++fault) {
        Copy copy;
        if (fault == 0) copy.input.PicInfo.width = 0;
        if (fault == 1) copy.input.PicInfo.height = 0;
        if (fault == 2) copy.context.HWOutPicWidth = 0;
        if (fault == 3) copy.context.HWOutPicWidth = 6;
        if (fault == 4) copy.context.HWOutPicWidth = UINT_MAX;
        if (fault == 5) copy.input.PicInfo.width = 7;
        copy.Reject(BC_STS_IO_XFR_ERROR);
    }
    {
        Copy copy(8, true);
        copy.input.PicInfo.height = 5;
        copy.Reject(BC_STS_IO_XFR_ERROR);
    }
    {
        Copy copy;
        copy.output.PoutFlags = BC_POUT_FLAGS_STRIDE;
        copy.output.StrideSz = UINT_MAX;
        copy.Reject(BC_STS_IO_XFR_ERROR);
    }
    {
        Copy copy;
        copy.output.YbuffSz = 0;
        copy.Reject(BC_STS_IO_XFR_ERROR);
    }
    {
        Copy copy;
        Check(DtsChkYUVSizes(nullptr, &copy.output, &copy.input) == BC_STS_INV_ARG,
              "null context cannot be dereferenced by the shared precheck");
        Check(DtsCopyRawDataToOutBuff(&copy.context, nullptr, &copy.input) == BC_STS_INV_ARG,
              "null output is rejected");
        Check(DtsCopyRawDataToOutBuff(&copy.context, &copy.output, nullptr) == BC_STS_INV_ARG,
              "null input is rejected");
    }
}

int main(int argc, char **argv)
{
    if (argc > 2 || (argc == 2 && std::strcmp(argv[1], "--rows") &&
                     std::strcmp(argv[1], "--capacity"))) return 2;
    if (argc == 2 && !std::strcmp(argv[1], "--capacity")) Capacity();
    else {
        Rows();
        FieldPairs();
    }
    // --rows is the bounded old-code reproducer: do not execute unchecked
    // huge dimensions/pitches against a baseline with no capacity validation.
    if (argc == 1) Bounds();
    if (failures) {
        std::fprintf(stderr, "%u raw-copy checks failed\n", failures);
        return 1;
    }
    std::printf("PASS: %u production raw YUY2 row, crop, field and buffer-bound checks\n", checks);
    return 0;
}
