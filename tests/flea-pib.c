/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Actual BCM70015 peek/PIB functions, with only memory/queue primitives mocked.
 * These tests do not load a module, open a device, or emulate firmware decode.
 */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "bc_dts_types.h"
struct _BC_DTS_PROC_OUT;
#include "bc_dts_defs.h"

#define OFFSETOF(_s_, _m_) ((size_t)(unsigned long)&(((_s_ *)0)->_m_))
#define PIC_PIB_DATA_OFFSET_FROM_END 4
#define PIC_WIDTH_OFFSET_FROM_END 8
#define PIB_FORMAT_CHANGE_BIT (UINT32_C(1) << 31)
#define PIB_EOS_DETECTED_BIT (UINT32_C(1) << 30)
#define FLEA_DECODE_ERROR_FLAG 0x800
#define COMP_FLAG_FMT_CHANGE 1
#define COMP_FLAG_PIB_VALID 2
#define COMP_FLAG_DATA_VALID 4
#define dev_err(dev, ...) ((void)(dev))
#define dev_dbg(dev, ...) ((void)(dev))
#define spin_lock_irqsave(lock, flags) ((void)(lock), (flags) = 0)
#define spin_unlock_irqrestore(lock, flags) ((void)(lock), (void)(flags))

struct device { int unused; };
struct pci_dev { struct device dev; };
struct crystalhd_adp { struct pci_dev *pdev; };
struct crystalhd_rx_buffer;
struct crystalhd_rx_buffer_ops {
    void (*sync_for_cpu)(struct crystalhd_adp *, struct crystalhd_rx_buffer *);
    void (*sync_for_device)(struct crystalhd_adp *, struct crystalhd_rx_buffer *);
    BC_STATUS (*read)(struct crystalhd_rx_buffer *, uint32_t, void *, size_t);
    BC_STATUS (*write)(struct crystalhd_rx_buffer *, uint32_t, const void *, size_t);
    void (*release)(struct crystalhd_adp *, struct crystalhd_rx_buffer *);
};
struct scatterlist { uint8_t *base; };
struct crystalhd_rx_buffer {
    struct scatterlist *sgl;
    uint32_t dma_nents, capacity, uv_offset, uv_sg_ix, uv_sg_off;
    BC_OUTPUT_FORMAT output_format;
    const struct crystalhd_rx_buffer_ops *ops;
    void *cookie;
};
struct crystalhd_rx_dma_pkt {
    struct crystalhd_rx_buffer *buffer;
    void *cookie;
    uint32_t y_done_sz;
    uint32_t flags;
    BC_PIC_INFO_BLOCK pib;
};
struct crystalhd_elem { void *data; };
struct crystalhd_dioq { unsigned count, lock; struct crystalhd_elem *head; };
struct crystalhd_hw {
    struct crystalhd_adp *adp;
    struct crystalhd_dioq *rx_rdyq;
    uint32_t PICWidth, PICHeight, DrvEosDetected, DrvCancelEosFlag;
    uint32_t LastPicNo, LastTwoPicNo, PDRatio, PauseThreshold, DefaultPauseThreshold;
    uint64_t TickSpentInPD, TickCntDecodePU;
};

static unsigned checks, groups, failures;
static unsigned reads, writes, syncs, fail_read, fail_write;
static bool partial_write;
static void check(bool condition, const char *message)
{
    ++checks;
    if (!condition) {
        ++failures;
        fprintf(stderr, "FAIL: %s\n", message);
    }
}
static void reset_faults(void)
{
    reads = writes = syncs = fail_read = fail_write = 0;
    partial_write = false;
}
static void sync_for_cpu(struct crystalhd_adp *adp,
                         struct crystalhd_rx_buffer *buffer)
{
    assert(adp && buffer && buffer->sgl && buffer->cookie);
    syncs++;
}
static void sync_for_device(struct crystalhd_adp *adp,
                            struct crystalhd_rx_buffer *buffer)
{
    (void)adp; (void)buffer;
    assert(!"PIB parsing never returns the buffer to DMA");
}
static BC_STATUS buffer_read(struct crystalhd_rx_buffer *buffer, uint32_t offset,
                             void *destination, size_t size)
{
    assert(buffer && buffer->sgl && offset <= buffer->capacity &&
           size <= buffer->capacity - offset);
    if (++reads == fail_read) return BC_STS_IO_ERROR;
    memcpy(destination, buffer->sgl->base + offset, size);
    return BC_STS_SUCCESS;
}
static BC_STATUS buffer_write(struct crystalhd_rx_buffer *buffer, uint32_t offset,
                              const void *source, size_t size)
{
    assert(buffer && buffer->sgl && offset <= buffer->capacity &&
           size <= buffer->capacity - offset);
    if (++writes == fail_write) {
        if (partial_write)
            memcpy(buffer->sgl->base + offset, source, size / 2);
        return BC_STS_IO_ERROR;
    }
    memcpy(buffer->sgl->base + offset, source, size);
    return BC_STS_SUCCESS;
}
static void unexpected_release(struct crystalhd_adp *adp,
                               struct crystalhd_rx_buffer *buffer)
{
    (void)adp; (void)buffer;
    assert(!"PIB parsing never releases its borrowed buffer");
}
static const struct crystalhd_rx_buffer_ops buffer_ops = {
    .sync_for_cpu = sync_for_cpu,
    .sync_for_device = sync_for_device,
    .read = buffer_read,
    .write = buffer_write,
    .release = unexpected_release,
};
static void crystalhd_rx_buffer_sync_for_cpu(struct crystalhd_adp *adp,
                                             struct crystalhd_rx_buffer *buffer)
{
    buffer->ops->sync_for_cpu(adp, buffer);
}
static BC_STATUS crystalhd_rx_buffer_read(struct crystalhd_rx_buffer *buffer,
                                          uint32_t offset, void *destination,
                                          size_t size)
{
    return buffer->ops->read(buffer, offset, destination, size);
}
static BC_STATUS crystalhd_rx_buffer_write(struct crystalhd_rx_buffer *buffer,
                                           uint32_t offset, const void *source,
                                           size_t size)
{
    return buffer->ops->write(buffer, offset, source, size);
}
static uint64_t rdtsc_ordered(void) { return 123; }
static bool flea_get_picture_info(struct crystalhd_hw *, struct crystalhd_rx_dma_pkt *,
                                  uint32_t *, uint64_t *, bool);
bool flea_GetPictureInfo(struct crystalhd_hw *, struct crystalhd_rx_dma_pkt *, uint32_t *, uint64_t *);
#include "flea-pib-functions.h"

struct fixture {
    uint32_t words[2048], original[2048];
    struct pci_dev pci;
    struct crystalhd_adp adp;
    struct scatterlist sgl;
    struct crystalhd_rx_buffer buffer;
    uint32_t cookie;
    struct crystalhd_rx_dma_pkt packet;
    struct crystalhd_elem element;
    struct crystalhd_dioq queue;
    struct crystalhd_hw hw;
    uint32_t first, number, line;
    size_t pib_offset;
};

static BC_PIC_INFO_BLOCK read_pib(const struct fixture *f)
{
    BC_PIC_INFO_BLOCK pib;
    memcpy(&pib, (const uint8_t *)f->words + f->pib_offset, sizeof(pib));
    return pib;
}

static void prepare(struct fixture *f, uint32_t first, unsigned picture,
                    uint32_t width_flags, uint32_t picture_flags, bool packed)
{
    uint32_t y_done_words;

    memset(f->words, 0x5a, sizeof(f->words));
    f->first = first;
    f->number = picture;
    f->line = (width_flags & PIB_EOS_DETECTED_BIT) ? 0 : 32;
    f->pib_offset = f->line * 64 * (packed ? 2 : 1) + 4;
    f->words[0] = first;
    f->buffer.uv_offset = packed ? 0 : sizeof(f->words) / 2;
    f->packet.y_done_sz = (packed ? sizeof(f->words) : f->buffer.uv_offset) / 4;
    y_done_words = f->packet.y_done_sz;
    f->words[y_done_words - 2] = 64 | width_flags;
    f->words[y_done_words - 1] = f->line;
    // Real firmware places a picture-number DWORD before the linear PIB.
    if (f->line) f->words[(f->pib_offset - 4) / 4] = picture;
    BC_PIC_INFO_BLOCK pib = {0};
    pib.width = 64; pib.height = 32; pib.picture_number = picture;
    pib.timeStamp = 123400 + picture; pib.flags = picture_flags;
    pib.ycom = 0xdeadbeef;
    memcpy((uint8_t *)f->words + f->pib_offset, &pib, sizeof(pib));
    f->packet.flags = COMP_FLAG_DATA_VALID;
    f->buffer.output_format = packed ? MODE422_YUY2 : MODE420;
    memcpy(f->original, f->words, sizeof(f->words));
    reset_faults();
}

static void init(struct fixture *f)
{
    memset(f, 0, sizeof(*f));
    f->adp.pdev = &f->pci;
    f->sgl.base = (uint8_t *)f->words;
    f->cookie = 0xc00c1e;
    f->buffer = (struct crystalhd_rx_buffer){
        .sgl = &f->sgl,
        .dma_nents = 1,
        .capacity = sizeof(f->words),
        .output_format = MODE422_YUY2,
        .ops = &buffer_ops,
        .cookie = &f->cookie,
    };
    f->packet.buffer = &f->buffer;
    f->packet.cookie = &f->cookie;
    f->element.data = &f->packet;
    f->queue.count = 1;
    f->queue.head = &f->element;
    f->hw.adp = &f->adp;
    f->hw.rx_rdyq = &f->queue;
    f->hw.DefaultPauseThreshold = 6;
    prepare(f, 0xa0286028, 7, 0, 0, true);
}

static void peek(struct fixture *f)
{
    uint32_t number = 99;
    uint64_t metadata = 99;
    unsigned syncs_before = syncs, writes_before = writes;
    check(crystalhd_flea_peek_next_decoded_frame(&f->hw, &metadata, &number, 64),
          "ready-queue status peek retains its success convention");
    const uint32_t expected_number =
        (f->words[f->packet.y_done_sz - 2] & PIB_EOS_DETECTED_BIT) &&
        f->line == 0 ? UINT32_MAX : f->number;
    check(number == expected_number && metadata == 123400 + f->number,
          "status peek returns unchanged picture/timestamp association");
    check(syncs == syncs_before + 1 && writes == writes_before &&
          memcmp(f->words, f->original, sizeof(f->words)) == 0,
          "status peek never changes any image or embedded metadata bytes");
    check(f->packet.buffer == &f->buffer &&
          f->packet.cookie == &f->cookie && f->buffer.cookie == &f->cookie,
          "successful peek preserves distinct backing and opaque cookie identities");
}

static void fetch(struct fixture *f)
{
    uint32_t expected[2048];
    unsigned syncs_before = syncs;
    memcpy(expected, f->original, sizeof(expected));
    expected[0] = f->line;
    BC_PIC_INFO_BLOCK pib = read_pib(f);
    pib.ycom = f->first;
    memcpy((uint8_t *)expected + f->pib_offset, &pib, sizeof(pib));
    check(flea_GetRptDropParam(&f->hw, &f->packet) == f->number,
          "actual dequeue wrapper reports the original picture number");
    check(memcmp(f->words, expected, sizeof(expected)) == 0,
          "dequeue changes only the first-DWORD marker and saved original pixels");
    pib = read_pib(f);
    check(pib.ycom == f->first,
          "library's exact DWORD restoration source retains both original packed pixels");
    check(syncs == syncs_before + 1 && f->packet.buffer == &f->buffer &&
          f->packet.cookie == &f->cookie,
          "dequeue metadata I/O borrows the exact buffer without changing its cookie");
}

static void repeated_peeks(void)
{
    const unsigned peek_counts[] = {0, 1, 2, 8};
    const uint32_t first_values[] = {0xa0286028, 0x28a02860, 32};
    for (unsigned mode = 0; mode != 2; ++mode)
        for (unsigned pattern = 0; pattern != 3; ++pattern)
            for (unsigned count = 0; count != 4; ++count) {
                struct fixture f;
                init(&f);
                ++groups;
                // Reuse the exact packet/backing as fresh DMA completion would.
                // No new per-packet state needs reset on repost.
                for (unsigned reuse = 0; reuse != 3; ++reuse) {
                    prepare(&f, first_values[pattern] + reuse, 7 + reuse, 0, 0, mode != 0);
                    for (unsigned i = 0; i != peek_counts[count]; ++i) peek(&f);
                    fetch(&f);
                }
            }
}

static void uyvy_metadata_lane_case(void)
{
    struct fixture f;

    init(&f);
    ++groups;
    prepare(&f, 0x28a02860, 19, 0, 0, true);
    f.buffer.output_format = MODE422_UYVY;
    peek(&f);
    fetch(&f);
    check(f.buffer.output_format == MODE422_UYVY &&
          read_pib(&f).picture_number == 19,
          "UYVY uses the same two-byte metadata lane without format collapse");
}

static void special_pictures(void)
{
    struct fixture f;
    init(&f);
    ++groups;
    prepare(&f, 0xa0286028, 7, PIB_FORMAT_CHANGE_BIT, 0, true);
    for (unsigned i = 0; i != 3; ++i) peek(&f);
    check(f.packet.flags == (COMP_FLAG_PIB_VALID | COMP_FLAG_FMT_CHANGE),
          "format status peek still identifies the dummy format packet");
    check(f.packet.pib.width == 64 && f.packet.pib.height == 32 &&
          f.packet.pib.ycom == f.first && f.hw.PICWidth == 64 && f.hw.PICHeight == 32 &&
          f.hw.LastPicNo == 0 && f.hw.PauseThreshold == 6,
          "format metadata and geometry/repeat reset semantics are preserved");

    ++groups;
    prepare(&f, 7, 7, PIB_EOS_DETECTED_BIT, VDEC_FLAG_EOS, true);
    for (unsigned i = 0; i != 3; ++i) peek(&f);
    check(f.hw.DrvEosDetected == 1 && f.packet.flags == COMP_FLAG_DATA_VALID,
          "status still observes genuine EOS without presenting dummy format data");
    uint32_t number = 0;
    uint64_t metadata = 0;
    check(flea_GetPictureInfo(&f.hw, &f.packet, &number, &metadata), "EOS dequeue remains successful");
    check(f.words[0] == UINT32_MAX && read_pib(&f).flags == VDEC_FLAG_EOS && writes == 1,
          "actual EOS dequeue writes its existing sentinel exactly once");
    check(memcmp((uint8_t *)f.words + 4, (uint8_t *)f.original + 4, sizeof(f.words) - 4) == 0,
          "EOS dequeue does not alter the remaining metadata/image bytes");
    check(f.packet.buffer == &f.buffer && f.packet.cookie == &f.cookie,
          "EOS parsing retains the generic buffer and opaque cookie");

    ++groups;
    prepare(&f, 0xa0286028, 7, 0, VDEC_FLAG_EOS, true);
    f.words[f.packet.y_done_sz - 2] |= PIB_EOS_DETECTED_BIT;
    memcpy(f.original, f.words, sizeof(f.original));
    peek(&f);
    check(flea_GetPictureInfo(&f.hw, &f.packet, &number, &metadata) && number == 7,
          "EOS with a nonzero PIB line retains its original picture number, not the word-zero sentinel");
    check(f.words[0] == UINT32_MAX && writes == 1,
          "nonzero-line EOS still prepares exactly the same dequeue sentinel");

    ++groups;
    prepare(&f, 0xa0286028, 8, 0, 0, true);
    peek(&f);
    check(f.hw.DrvEosDetected == 0 && f.hw.DrvCancelEosFlag == 1,
          "later real picture cancels prior EOS state as before");
    fetch(&f);
}

static void failures_and_retries(void)
{
    for (unsigned failure = 1; failure <= 5; ++failure) {
        struct fixture f;
        init(&f);
        ++groups;
        fail_read = failure;
        uint32_t number = 99;
        uint64_t metadata = 99;
        crystalhd_flea_peek_next_decoded_frame(&f.hw, &metadata, &number, 64);
        check(number == 0 && metadata == 0 && writes == 0 &&
              f.packet.buffer == &f.buffer && f.packet.cookie == &f.cookie,
              "failed generic read publishes no picture and preserves both identities");
        check(memcmp(f.words, f.original, sizeof(f.words)) == 0, "failed peek never edits shared bytes");
        reset_faults();
        peek(&f);
        fetch(&f);
    }
    for (unsigned failure = 1; failure <= 2; ++failure)
        for (unsigned partial = 0; partial != 2; ++partial) {
            struct fixture f;
            init(&f);
            ++groups;
            peek(&f);
            reset_faults();
            fail_write = failure;
            partial_write = partial != 0;
            uint32_t number = 99;
            uint64_t metadata = 99;
            check(!flea_GetPictureInfo(&f.hw, &f.packet, &number, &metadata),
                  "failed or partially completed dequeue write is not successful output");
            check(number == 0 && metadata == 0 &&
                  f.packet.buffer == &f.buffer && f.packet.cookie == &f.cookie,
                  "failed dequeue write clears outputs without changing either identity");
            // The actual fetch caller drops PicNumber==0 back to the free queue.
            // A subsequent hardware completion supplies a new image; do not
            // pretend a failed frontend write is atomic or retry mutated pixels.
            prepare(&f, 0x70907050, 8, 0, 0, true);
            peek(&f);
            fetch(&f);
        }
    {
        struct fixture f;
        init(&f);
        ++groups;
        f.packet.buffer = NULL;
        uint32_t number = 99;
        uint64_t metadata = 99;
        check(!flea_GetPictureInfo(&f.hw, &f.packet, &number, &metadata) &&
              number == 0 && metadata == 0 && !reads && !writes && !syncs,
              "missing generic buffer fails without touching storage callbacks");
    }
}

static void set_completion_tail(struct fixture *f, uint32_t y_done_bytes)
{
    assert(y_done_bytes >= 8 && !(y_done_bytes & 3));
    f->packet.y_done_sz = y_done_bytes / 4;
    f->words[f->packet.y_done_sz - 2] = 64;
    f->words[f->packet.y_done_sz - 1] = f->line;
    memcpy(f->original, f->words, sizeof(f->original));
    reset_faults();
}

static void pib_tail_extent_cases(void)
{
    const BC_OUTPUT_FORMAT formats[] = {
        MODE420, MODE422_YUY2, MODE422_UYVY,
    };

    for (unsigned i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i) {
        struct fixture f;
        unsigned stride = formats[i] == MODE420 ? 1 : 2;
        uint32_t pib_end, spill = stride == 2 ? 132 : 4;

        init(&f);
        ++groups;
        prepare(&f, 0xa0286028, 7 + i, 0, 0, stride == 2);
        f.buffer.output_format = formats[i];
        pib_end = f.pib_offset + stride * sizeof(BC_PIC_INFO_BLOCK);
        set_completion_tail(&f, pib_end - spill);
        check((uint32_t)(stride * OFFSETOF(BC_PIC_INFO_BLOCK, other)) <=
              f.packet.y_done_sz * 4 - f.pib_offset,
              "completed data contains every common PIB field");
        peek(&f);
        fetch(&f);
        check(pib_end == f.packet.y_done_sz * 4 + spill,
              "PIB tail may span the completion trailer inside the Y plane");
    }

    for (unsigned i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i) {
        struct fixture f;
        unsigned stride = formats[i] == MODE420 ? 1 : 2;
        uint32_t common_bytes = stride * OFFSETOF(BC_PIC_INFO_BLOCK, other);
        uint32_t number = 99;
        uint64_t metadata = 99;

        init(&f);
        ++groups;
        prepare(&f, 0xa0286028, 7, 0, 0, stride == 2);
        f.buffer.output_format = formats[i];
        set_completion_tail(&f, f.pib_offset + common_bytes - 4);
        check(!flea_GetPictureInfo(&f.hw, &f.packet, &number, &metadata) &&
              number == 0 && metadata == 0 && reads == 2 && !writes,
              "truncated common PIB fields are rejected before buffer I/O");
        check(!memcmp(f.words, f.original, sizeof(f.words)),
              "common-prefix rejection preserves capture storage");
    }

    for (unsigned i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i) {
        struct fixture f;
        unsigned stride = formats[i] == MODE420 ? 1 : 2;
        uint32_t pib_end, spill = stride == 2 ? 132 : 4;
        uint32_t number = 99;
        uint64_t metadata = 99;

        init(&f);
        ++groups;
        prepare(&f, 0xa0286028, 7, 0, 0, stride == 2);
        f.buffer.output_format = formats[i];
        pib_end = f.pib_offset + stride * sizeof(BC_PIC_INFO_BLOCK);
        set_completion_tail(&f, pib_end - spill);
        if (formats[i] == MODE420)
            f.buffer.uv_offset = pib_end - 4;
        else
            f.buffer.capacity = pib_end - 4;
        check(!flea_GetPictureInfo(&f.hw, &f.packet, &number, &metadata) &&
              number == 0 && metadata == 0 && reads == 2 && !writes,
              "PIB crossing the registered Y plane is rejected before buffer I/O");
        check(!memcmp(f.words, f.original, sizeof(f.words)) &&
              f.packet.buffer == &f.buffer && f.packet.cookie == &f.cookie,
              "Y-plane rejection preserves storage, buffer and opaque cookie");
    }
}

static void completed_extent_cases(void)
{
    enum {
        INVALID_FORMAT, SHORT_COMPLETION, OVERSIZE_COMPLETION,
        UV_PAST_CAPACITY, COMPLETION_PAST_Y, ZERO_WIDTH, ROW_PAST_COMPLETION
    };

    for (unsigned variant = INVALID_FORMAT;
         variant <= ROW_PAST_COMPLETION; variant++) {
        struct fixture f;
        uint32_t number = 99;
        uint64_t metadata = 99;

        init(&f);
        ++groups;
        switch (variant) {
        case INVALID_FORMAT:
            f.buffer.output_format = OUTPUT_MODE_INVALID;
            break;
        case SHORT_COMPLETION:
            f.packet.y_done_sz = 1;
            break;
        case OVERSIZE_COMPLETION:
            f.packet.y_done_sz = f.buffer.capacity / 4 + 1;
            break;
        case UV_PAST_CAPACITY:
            f.buffer.uv_offset = f.buffer.capacity + 1;
            break;
        case COMPLETION_PAST_Y:
            prepare(&f, 0xa0286028, 7, 0, 0, false);
            f.packet.y_done_sz = f.buffer.uv_offset / 4 + 1;
            break;
        case ZERO_WIDTH:
            f.words[f.packet.y_done_sz - 2] = 0;
            break;
        default:
            f.words[f.packet.y_done_sz - 1] = 1092;
            break;
        }
        memcpy(f.original, f.words, sizeof(f.original));
        check(!flea_GetPictureInfo(&f.hw, &f.packet, &number, &metadata) &&
              number == 0 && metadata == 0 && !writes,
              "invalid layout or completed-Y extent is rejected without output");
        check(!memcmp(f.words, f.original, sizeof(f.words)) &&
              f.packet.buffer == &f.buffer && f.packet.cookie == &f.cookie,
              "extent rejection preserves storage, buffer and opaque cookie");
    }
}

int main(void)
{
    repeated_peeks();
    uyvy_metadata_lane_case();
    special_pictures();
    failures_and_retries();
    pib_tail_extent_cases();
    completed_extent_cases();
    printf("Flea PIB: %u groups, %u checks, %u failures\n", groups, checks, failures);
    return failures ? 1 : 0;
}
