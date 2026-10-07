// SPDX-License-Identifier: LGPL-2.1-or-later
#include "libcrystalhd_raw_frame.h"
#include "libcrystalhd_if.h"

#include <stddef.h>
#include <stdlib.h>

struct BC_RAW_FRAME_BUILDER {
    uint32_t magic;
    uint32_t committed_frames;
    uint32_t committed_reference_frames;
    uint32_t slots;
    uint32_t pending_slot;
    uint32_t pending_bytes;
    bool pending;
    bool aborted;
    bool pending_reference;
    bool last_non_reference;
    uint8_t au[BC_RAW_FRAME_AU_CAPACITY];
    uint8_t rbsp[BC_RAW_FRAME_AU_CAPACITY];
};

namespace {
const uint32_t kMagic = 0x52415746u;
const uint32_t kCompose = 2u;
const uint32_t kFrameLimit = 0x40000000u;

struct Bits {
    uint8_t *data;
    uint32_t capacity;
    uint32_t size;
    unsigned used;
    bool good;

    explicit Bits(uint8_t *buffer)
        : data(buffer), capacity(BC_RAW_FRAME_AU_CAPACITY), size(0),
          used(0), good(true) {}

    void u(uint32_t value, unsigned width)
    {
        if (width > 32 || (width < 32 && (value >> width) != 0)) {
            good = false;
            return;
        }
        for (unsigned bit = width; good && bit != 0; --bit) {
            if (used == 0) {
                if (size == capacity) {
                    good = false;
                    return;
                }
                data[size++] = 0;
            }
            data[size - 1] |= static_cast<uint8_t>(
                ((value >> (bit - 1)) & 1u) << (7 - used));
            used = (used + 1) & 7u;
        }
    }

    void ue(uint32_t value)
    {
        if (value == UINT32_MAX) {
            good = false;
            return;
        }
        const uint32_t code = value + 1;
        unsigned width = 0;
        for (uint32_t n = code; n != 0; n >>= 1)
            ++width;
        u(0, width - 1);
        u(code, width);
    }

    void se(int32_t value)
    {
        const uint64_t code = value > 0 ? 2 * static_cast<uint64_t>(value) - 1 :
            2 * static_cast<uint64_t>(-static_cast<int64_t>(value));
        if (code >= UINT32_MAX) {
            good = false;
            return;
        }
        ue(static_cast<uint32_t>(code));
    }

    void align() { u(0, (8 - used) & 7u); }
    void byte(uint8_t value)
    {
        if (!good || used != 0 || size == capacity) {
            good = false;
            return;
        }
        data[size++] = value;
    }
    void finish() { u(1, 1); align(); }
};

struct AnnexB {
    BC_RAW_FRAME_BUILDER *builder;
    uint32_t size;
    bool good;
    explicit AnnexB(BC_RAW_FRAME_BUILDER *b) : builder(b), size(0), good(true) {}
    void byte(uint8_t value)
    {
        if (!good || size == BC_RAW_FRAME_AU_CAPACITY) {
            good = false;
            return;
        }
        builder->au[size++] = value;
    }
    void nal(uint8_t header, const Bits &bits)
    {
        if (!bits.good || bits.used != 0) {
            good = false;
            return;
        }
        byte(0); byte(0); byte(0); byte(1); byte(header);
        unsigned zeros = 0;
        for (uint32_t i = 0; good && i < bits.size; ++i) {
            const uint8_t value = bits.data[i];
            if (zeros == 2 && value <= 3) {
                byte(3);
                zeros = 0;
            }
            byte(value);
            zeros = value == 0 ? zeros + 1 : 0;
        }
    }
};

bool valid(const BC_RAW_FRAME_BUILDER *builder)
{
    return builder != NULL && builder->magic == kMagic;
}

BC_STATUS prepare_status(BC_RAW_FRAME_BUILDER *builder,
                         const uint8_t **au, uint32_t *bytes)
{
    if (!valid(builder) || au == NULL || bytes == NULL)
        return BC_STS_INV_ARG;
    if (builder->aborted)
        return BC_STS_IO_USER_ABORT;
    if (builder->pending)
        return BC_STS_BUSY;
    if (builder->committed_frames >= kFrameLimit)
        return BC_STS_ERR_USAGE;
    return BC_STS_SUCCESS;
}

bool valid_planes(const BC_RAW_FRAME_BUILDER *builder,
                  const BC_RAW_FRAME_PLANES *frame)
{
    if (frame == NULL)
        return false;
    const uintptr_t own_first = reinterpret_cast<uintptr_t>(builder);
    const uintptr_t own_last = own_first + sizeof(*builder) - 1;
    for (unsigned plane = 0; plane < 3; ++plane) {
        const uint32_t width = plane == 0 ? BC_RAW_FRAME_WIDTH : BC_RAW_FRAME_WIDTH / 2;
        const uint32_t rows = plane == 0 ? BC_RAW_FRAME_HEIGHT : BC_RAW_FRAME_HEIGHT / 2;
        if (frame->planes[plane] == NULL || frame->strides[plane] < width)
            return false;
        const uint64_t span = static_cast<uint64_t>(rows - 1) * frame->strides[plane] + width;
        if (span > frame->plane_bytes[plane] || span > UINTPTR_MAX)
            return false;
        const uintptr_t first = reinterpret_cast<uintptr_t>(frame->planes[plane]);
        if (first > UINTPTR_MAX - static_cast<uintptr_t>(span - 1))
            return false;
        const uintptr_t last = first + static_cast<uintptr_t>(span - 1);
        if (first <= own_last && last >= own_first)
            return false;
    }
    return true;
}

void aud(AnnexB &output, bool intra)
{
    Bits bits(output.builder->rbsp);
    bits.u(intra ? 0 : 1, 3);
    bits.finish();
    output.nal(0x09, bits);
}

void sps(AnnexB &output)
{
    Bits b(output.builder->rbsp);
    b.u(66, 8); b.u(0x80, 8); b.u(30, 8);
    b.ue(0); b.ue(0); b.ue(2); b.ue(3);
    b.u(0, 1);
    b.ue(BC_RAW_FRAME_WIDTH / 16 - 1);
    b.ue(BC_RAW_FRAME_HEIGHT / 16 - 1);
    b.u(1, 1); b.u(1, 1); b.u(0, 1); b.u(1, 1);
    b.u(1, 1); b.u(1, 8); b.u(0, 1);
    b.u(1, 1); b.u(5, 3); b.u(0, 1); b.u(0, 1); b.u(0, 1);
    b.u(1, 1); b.u(1, 32); b.u(2 * BC_RAW_FRAME_FPS, 32); b.u(1, 1);
    b.u(0, 1); b.u(0, 1); b.u(0, 1); b.u(0, 1);
    b.finish();
    output.nal(0x67, b);
}

void pps(AnnexB &output)
{
    Bits b(output.builder->rbsp);
    b.ue(0); b.ue(0); b.u(0, 1); b.u(0, 1);
    b.ue(0); b.ue(0); b.ue(0); b.u(0, 1); b.u(0, 2);
    b.se(0); b.se(0); b.se(0);
    b.u(1, 1); b.u(0, 1); b.u(0, 1);
    b.finish();
    output.nal(0x68, b);
}

void upload(AnnexB &output, uint32_t slot, const BC_RAW_FRAME_PLANES &frame)
{
    BC_RAW_FRAME_BUILDER *builder = output.builder;
    const bool first = builder->committed_frames == 0;
    Bits b(builder->rbsp);
    b.ue(0); b.ue(2); b.ue(0);
    b.u(builder->committed_reference_frames & 15u, 4);
    if (first) {
        b.ue(0); b.u(0, 1); b.u(1, 1);
    } else {
        b.u(1, 1);
        if (builder->slots == 1) {
            b.ue(4); b.ue(2);
        } else {
            b.ue(2); b.ue(slot);
        }
        b.ue(6); b.ue(slot); b.ue(0);
    }
    b.se(0); b.ue(1);
    for (unsigned my = 0; my < BC_RAW_FRAME_HEIGHT / 16; ++my) {
        for (unsigned mx = 0; mx < BC_RAW_FRAME_WIDTH / 16; ++mx) {
            b.ue(25);
            b.align();
            for (unsigned plane = 0; plane < 3; ++plane) {
                const unsigned side = plane == 0 ? 16 : 8;
                for (unsigned row = 0; row < side; ++row) {
                    const size_t offset = static_cast<size_t>(my * side + row) *
                        frame.strides[plane] + mx * side;
                    const uint8_t *pixels = frame.planes[plane] + offset;
                    for (unsigned column = 0; column < side; ++column)
                        b.byte(pixels[column]);
                }
            }
        }
    }
    b.finish();
    output.nal(first ? 0x65 : 0x21, b);
}

void compose(AnnexB &output, const uint8_t mask[BC_RAW_FRAME_MASK_BYTES], bool reference)
{
    Bits b(output.builder->rbsp);
    b.ue(0); b.ue(0); b.ue(0);
    b.u(output.builder->committed_reference_frames & 15u, 4);
    b.u(1, 1); b.ue(1);
    b.u(1, 1); b.ue(2); b.ue(0); b.ue(2); b.ue(1); b.ue(3);
    if (reference)
        b.u(0, 1);
    b.se(0); b.ue(1);
    for (unsigned mb = 0; mb < BC_RAW_FRAME_MASK_BYTES; ++mb) {
        b.ue(0); b.ue(0);
        b.u(1u - mask[mb], 1); // te(v) for two active L0 references.
        b.se(0); b.se(0); b.ue(0);
    }
    b.finish();
    output.nal(reference ? 0x21 : 0x01, b);
}

void translate(AnnexB &output, uint32_t slot,
               int32_t mv_x_qpel, int32_t mv_y_qpel, bool reference)
{
    Bits b(output.builder->rbsp);
    b.ue(0); b.ue(0); b.ue(0);
    b.u(output.builder->committed_reference_frames & 15u, 4);
    b.u(1, 1); b.ue(0); // One active L0 reference; no per-MB ref_idx.
    b.u(1, 1); b.ue(2); b.ue(slot); b.ue(3);
    if (reference)
        b.u(0, 1);
    b.se(0); b.ue(1);
    for (unsigned mb = 0; mb < BC_RAW_FRAME_MASK_BYTES; ++mb) {
        b.ue(0); b.ue(0);
        // First MB predicts zero. Same-reference spatial prediction carries
        // this uniform vector through every remaining 16x16 macroblock.
        b.se(mb == 0 ? mv_x_qpel : 0);
        b.se(mb == 0 ? mv_y_qpel : 0);
        b.ue(0);
    }
    b.finish();
    output.nal(reference ? 0x21 : 0x01, b);
}

BC_STATUS publish(AnnexB &output, uint32_t slot,
                  const uint8_t **au, uint32_t *bytes, bool reference = true)
{
    if (!output.good || output.size == 0)
        return BC_STS_INSUFF_RES;
    BC_RAW_FRAME_BUILDER *builder = output.builder;
    builder->pending_slot = slot;
    builder->pending_bytes = output.size;
    builder->pending_reference = reference;
    builder->pending = true;
    *au = builder->au;
    *bytes = output.size;
    return BC_STS_SUCCESS;
}
} // namespace

extern "C" {
DRVIFLIB_API BC_STATUS DtsRawFrameCreate(BC_RAW_FRAME_BUILDER **out)
{
    if (out == NULL || *out != NULL)
        return BC_STS_INV_ARG;
    BC_RAW_FRAME_BUILDER *builder = static_cast<BC_RAW_FRAME_BUILDER *>(
        calloc(1, sizeof(BC_RAW_FRAME_BUILDER)));
    if (builder == NULL)
        return BC_STS_INSUFF_RES;
    builder->magic = kMagic;
    builder->pending_slot = kCompose;
    *out = builder;
    return BC_STS_SUCCESS;
}

DRVIFLIB_API BC_STATUS DtsRawFrameDestroy(BC_RAW_FRAME_BUILDER **builder)
{
    if (builder == NULL)
        return BC_STS_INV_ARG;
    if (*builder == NULL)
        return BC_STS_SUCCESS;
    if (!valid(*builder))
        return BC_STS_INV_ARG;
    (*builder)->magic = 0;
    free(*builder);
    *builder = NULL;
    return BC_STS_SUCCESS;
}

DRVIFLIB_API BC_STATUS DtsRawFramePrepareUpload(BC_RAW_FRAME_BUILDER *builder,
                                 uint32_t slot, const BC_RAW_FRAME_PLANES *frame,
                                 const uint8_t **au, uint32_t *bytes)
{
    const BC_STATUS status = prepare_status(builder, au, bytes);
    if (status != BC_STS_SUCCESS)
        return status;
    if (slot > 1 || !valid_planes(builder, frame))
        return BC_STS_INV_ARG;
    if ((builder->slots == 0 && slot != 0) ||
        (builder->slots == 1 && slot != 1) ||
        (builder->slots != 0 && builder->slots != 1 && builder->slots != 3))
        return BC_STS_ERR_USAGE;
    AnnexB output(builder);
    aud(output, true);
    if (builder->committed_frames == 0) {
        sps(output);
        pps(output);
    }
    upload(output, slot, *frame);
    return publish(output, slot, au, bytes);
}

static BC_STATUS prepare_compose(BC_RAW_FRAME_BUILDER *builder,
                                const uint8_t *mask, uint32_t mask_bytes,
                                const uint8_t **au, uint32_t *bytes, bool reference)
{
    const BC_STATUS status = prepare_status(builder, au, bytes);
    if (status != BC_STS_SUCCESS)
        return status;
    if (mask == NULL || mask_bytes != BC_RAW_FRAME_MASK_BYTES)
        return BC_STS_INV_ARG;
    if (builder->slots != 3 || (!reference && builder->last_non_reference))
        return BC_STS_ERR_USAGE;
    uint8_t choices[BC_RAW_FRAME_MASK_BYTES];
    for (unsigned mb = 0; mb < BC_RAW_FRAME_MASK_BYTES; ++mb) {
        if (mask[mb] > 1)
            return BC_STS_INV_ARG;
        choices[mb] = mask[mb];
    }
    AnnexB output(builder);
    aud(output, false);
    compose(output, choices, reference);
    return publish(output, kCompose, au, bytes, reference);
}

DRVIFLIB_API BC_STATUS DtsRawFramePrepareCompose(BC_RAW_FRAME_BUILDER *builder,
                                  const uint8_t *mask, uint32_t mask_bytes,
                                  const uint8_t **au, uint32_t *bytes)
{
    return prepare_compose(builder, mask, mask_bytes, au, bytes, true);
}

DRVIFLIB_API BC_STATUS DtsRawFramePrepareComposeNonReference(BC_RAW_FRAME_BUILDER *builder,
                                  const uint8_t *mask, uint32_t mask_bytes,
                                  const uint8_t **au, uint32_t *bytes)
{
    return prepare_compose(builder, mask, mask_bytes, au, bytes, false);
}

static BC_STATUS prepare_translate(BC_RAW_FRAME_BUILDER *builder, uint32_t slot,
                                  int32_t mv_x_qpel, int32_t mv_y_qpel,
                                  const uint8_t **au, uint32_t *bytes, bool reference)
{
    const BC_STATUS status = prepare_status(builder, au, bytes);
    if (status != BC_STS_SUCCESS)
        return status;
    if (slot > 1 || mv_x_qpel < -3 || mv_x_qpel > 3 ||
        mv_y_qpel < -3 || mv_y_qpel > 3)
        return BC_STS_INV_ARG;
    if (builder->slots != 3 || (!reference && builder->last_non_reference))
        return BC_STS_ERR_USAGE;
    AnnexB output(builder);
    aud(output, false);
    translate(output, slot, mv_x_qpel, mv_y_qpel, reference);
    return publish(output, kCompose, au, bytes, reference);
}

DRVIFLIB_API BC_STATUS DtsRawFramePrepareTranslate(BC_RAW_FRAME_BUILDER *builder,
                                    uint32_t slot, int32_t mv_x_qpel, int32_t mv_y_qpel,
                                    const uint8_t **au, uint32_t *bytes)
{
    return prepare_translate(builder, slot, mv_x_qpel, mv_y_qpel, au, bytes, true);
}

DRVIFLIB_API BC_STATUS DtsRawFramePrepareTranslateNonReference(BC_RAW_FRAME_BUILDER *builder,
                                    uint32_t slot, int32_t mv_x_qpel, int32_t mv_y_qpel,
                                    const uint8_t **au, uint32_t *bytes)
{
    return prepare_translate(builder, slot, mv_x_qpel, mv_y_qpel, au, bytes, false);
}

DRVIFLIB_API BC_STATUS DtsRawFrameFinish(BC_RAW_FRAME_BUILDER *builder, BC_STATUS input_status)
{
    if (!valid(builder))
        return BC_STS_INV_ARG;
    if (builder->aborted)
        return BC_STS_IO_USER_ABORT;
    if (!builder->pending)
        return BC_STS_ERR_USAGE;
    if (input_status != BC_STS_SUCCESS) {
        builder->aborted = true;
        builder->pending = false;
        builder->pending_bytes = 0;
        builder->pending_reference = false;
        return input_status;
    }
    if (builder->pending_slot < 2)
        builder->slots |= 1u << builder->pending_slot;
    ++builder->committed_frames;
    if (builder->pending_reference)
        ++builder->committed_reference_frames;
    builder->last_non_reference = !builder->pending_reference;
    builder->pending = false;
    builder->pending_bytes = 0;
    builder->pending_slot = kCompose;
    builder->pending_reference = false;
    return BC_STS_SUCCESS;
}

DRVIFLIB_API BC_STATUS DtsRawFrameSubmit(BC_RAW_FRAME_BUILDER *builder, HANDLE device,
                          uint64_t timestamp)
{
    if (!valid(builder))
        return BC_STS_INV_ARG;
    if (builder->aborted)
        return BC_STS_IO_USER_ABORT;
    if (!builder->pending)
        return BC_STS_ERR_USAGE;
    const BC_STATUS status = DtsProcInput(device, builder->au,
                                        builder->pending_bytes, timestamp, FALSE);
    return DtsRawFrameFinish(builder, status);
}

DRVIFLIB_API BC_STATUS DtsRawFrameDiscard(BC_RAW_FRAME_BUILDER *builder)
{
    if (!valid(builder))
        return BC_STS_INV_ARG;
    if (builder->aborted)
        return BC_STS_IO_USER_ABORT;
    if (!builder->pending)
        return BC_STS_ERR_USAGE;
    builder->pending = false;
    builder->pending_bytes = 0;
    builder->pending_slot = kCompose;
    builder->pending_reference = false;
    return BC_STS_SUCCESS;
}

DRVIFLIB_API BC_STATUS DtsRawFrameAbort(BC_RAW_FRAME_BUILDER *builder)
{
    if (!valid(builder))
        return BC_STS_INV_ARG;
    builder->aborted = true;
    builder->pending = false;
    builder->pending_bytes = 0;
    builder->pending_slot = kCompose;
    builder->pending_reference = false;
    return BC_STS_SUCCESS;
}
} // extern "C"
