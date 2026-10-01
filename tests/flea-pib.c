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
#define COMP_FLAG_DATA_ENC 8
#define COMP_FLAG_DATA_BOT 16
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
/* The finisher must not inspect the legacy PIB, whose ABI is tested elsewhere. */
struct C011_PIB { uint32_t unused; };
#include "rx-metadata.h"
struct crystalhd_rx_dma_pkt {
    struct crystalhd_rx_buffer *buffer;
    void *cookie;
    uint32_t y_done_sz;
    uint32_t flags;
    BC_PIC_INFO_BLOCK pib;
    struct crystalhd_rx_metadata metadata;
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
static BC_STATUS write_error;
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
    write_error = BC_STS_IO_ERROR;
}
static bool metadata_empty(const struct crystalhd_rx_dma_pkt *packet)
{
    static const struct crystalhd_rx_metadata empty;

    return !memcmp(&packet->metadata, &empty, sizeof(empty));
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
        return write_error;
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
#include "rx-buffer-functions.h"
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
    check(metadata_empty(&f->packet),
          "status peek leaves kernel completion metadata unpublished");
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
    check(f->packet.metadata.valid &&
          f->packet.metadata.firmware_timestamp == pib.timeStamp &&
          f->packet.metadata.picture_number == f->number &&
          f->packet.metadata.picture_flags == pib.flags &&
          f->packet.metadata.picture_width == pib.width &&
          f->packet.metadata.picture_height == pib.height &&
          f->packet.metadata.row_width == 64 &&
          f->packet.metadata.pib_line == f->line &&
          f->packet.metadata.first_pixel_word == f->first &&
          !f->packet.metadata.eos_trailer,
          "accepted picture publishes exact firmware metadata independently of legacy flags");
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
                // The parser must discard metadata from the prior picture.
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
    check(metadata_empty(&f.packet),
          "format-change status retains no accepted-picture snapshot");

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
    check(f.packet.metadata.valid && f.packet.metadata.eos_trailer &&
          f.packet.metadata.picture_flags == VDEC_FLAG_EOS &&
          f.packet.metadata.picture_number == UINT32_MAX,
          "EOS dequeue exposes separate trailer and PIB evidence with its sentinel");

    ++groups;
    prepare(&f, 0xa0286028, 7, 0, VDEC_FLAG_EOS, true);
    f.words[f.packet.y_done_sz - 2] |= PIB_EOS_DETECTED_BIT;
    memcpy(f.original, f.words, sizeof(f.original));
    peek(&f);
    check(flea_GetPictureInfo(&f.hw, &f.packet, &number, &metadata) && number == 7,
          "EOS with a nonzero PIB line retains its original picture number, not the word-zero sentinel");
    check(f.words[0] == UINT32_MAX && writes == 1,
          "nonzero-line EOS still prepares exactly the same dequeue sentinel");
    check(f.packet.metadata.valid && f.packet.metadata.eos_trailer &&
          f.packet.metadata.picture_number == 7,
          "nonzero-line EOS preserves the parsed number in its snapshot");

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
        memset(&f.packet.metadata, 0xa5, sizeof(f.packet.metadata));
        uint32_t number = 99;
        uint64_t metadata = 99;
        crystalhd_flea_peek_next_decoded_frame(&f.hw, &metadata, &number, 64);
        check(number == 0 && metadata == 0 && writes == 0 &&
              f.packet.buffer == &f.buffer && f.packet.cookie == &f.cookie,
              "failed generic read publishes no picture and preserves both identities");
        check(memcmp(f.words, f.original, sizeof(f.words)) == 0, "failed peek never edits shared bytes");
        check(metadata_empty(&f.packet),
              "failed peek discards metadata from a previous packet use");
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
            memset(&f.packet.metadata, 0xa5, sizeof(f.packet.metadata));
            uint32_t number = 99;
            uint64_t metadata = 99;
            check(!flea_GetPictureInfo(&f.hw, &f.packet, &number, &metadata),
                  "failed or partially completed dequeue write is not successful output");
            check(number == 0 && metadata == 0 &&
                  f.packet.buffer == &f.buffer && f.packet.cookie == &f.cookie,
                  "failed dequeue write clears outputs without changing either identity");
            check(metadata_empty(&f.packet),
                  "failed dequeue preparation never publishes stale metadata");
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
        memset(&f.packet.metadata, 0xa5, sizeof(f.packet.metadata));
        uint32_t number = 99;
        uint64_t metadata = 99;
        check(!flea_GetPictureInfo(&f.hw, &f.packet, &number, &metadata) &&
              number == 0 && metadata == 0 && !reads && !writes && !syncs &&
              metadata_empty(&f.packet),
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
        memset(&f.packet.metadata, 0xa5, sizeof(f.packet.metadata));
        check(!flea_GetPictureInfo(&f.hw, &f.packet, &number, &metadata) &&
              number == 0 && metadata == 0 && reads == 2 && !writes &&
              metadata_empty(&f.packet),
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
        memset(&f.packet.metadata, 0xa5, sizeof(f.packet.metadata));
        check(!flea_GetPictureInfo(&f.hw, &f.packet, &number, &metadata) &&
              number == 0 && metadata == 0 && reads == 2 && !writes &&
              metadata_empty(&f.packet),
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
        memset(&f.packet.metadata, 0xa5, sizeof(f.packet.metadata));
        check(!flea_GetPictureInfo(&f.hw, &f.packet, &number, &metadata) &&
              number == 0 && metadata == 0 && !writes &&
              metadata_empty(&f.packet),
              "invalid layout or completed-Y extent is rejected without output");
        check(!memcmp(f.words, f.original, sizeof(f.words)) &&
              f.packet.buffer == &f.buffer && f.packet.cookie == &f.cookie,
              "extent rejection preserves storage, buffer and opaque cookie");
    }
}

static void kernel_snapshot_cases(void)
{
    const BC_OUTPUT_FORMAT formats[] = {
        MODE420, MODE422_YUY2, MODE422_UYVY,
    };
    const uint64_t timestamps[] = {0, UINT64_C(0xabcdef0123456789)};
    const uint32_t picture_flags = VDEC_FLAG_FIELDPAIR |
        VDEC_FLAG_INTERLACED_SRC | VDEC_FLAG_BOTTOM_FIRST |
        VDEC_FLAG_LAST_PICTURE | VDEC_FLAG_PICTURE_META_DATA_PRESENT;

    for (unsigned mode = 0; mode < sizeof(formats) / sizeof(formats[0]); mode++)
        for (unsigned timestamp = 0; timestamp < 2; timestamp++) {
            struct fixture f;
            uint32_t number = 99;
            uint64_t metadata = 99;
            BC_PIC_INFO_BLOCK pib;

            init(&f);
            ++groups;
            prepare(&f, 0xa0286028, 19, 0, picture_flags, mode != 0);
            f.buffer.output_format = formats[mode];
            pib = read_pib(&f);
            pib.timeStamp = timestamps[timestamp];
            memcpy((uint8_t *)f.words + f.pib_offset, &pib, sizeof(pib));
            check(flea_GetPictureInfo(&f.hw, &f.packet, &number, &metadata) &&
                  number == 19 && metadata == timestamps[timestamp] &&
                  f.packet.metadata.valid &&
                  f.packet.metadata.firmware_timestamp == timestamps[timestamp] &&
                  f.packet.metadata.picture_number == 19 &&
                  f.packet.metadata.picture_flags == picture_flags &&
                  !f.packet.metadata.eos_trailer &&
                  f.packet.flags == COMP_FLAG_DATA_VALID,
                  "zero/full-width timestamps and field flags retain exact firmware values");
        }

    for (unsigned trailer = 0; trailer < 2; trailer++)
        for (unsigned eos_flag = 0; eos_flag < 2; eos_flag++) {
            struct fixture f;
            uint32_t number = 99;
            uint64_t metadata = 99;

            init(&f);
            ++groups;
            prepare(&f, 7, 7, trailer ? PIB_EOS_DETECTED_BIT : 0,
                    eos_flag ? VDEC_FLAG_EOS : 0, true);
            check(flea_GetPictureInfo(&f.hw, &f.packet, &number, &metadata) &&
                  f.packet.metadata.valid &&
                  f.packet.metadata.eos_trailer == (trailer != 0) &&
                  f.packet.metadata.picture_flags == (eos_flag ? VDEC_FLAG_EOS : 0),
                  "trailer and PIB EOS indicators remain independent even when they disagree");
            check(f.packet.metadata.picture_width == 64 &&
                  f.packet.metadata.picture_height == 32 &&
                  f.packet.metadata.row_width == 64 &&
                  f.packet.metadata.pib_line == f.line &&
                  f.packet.metadata.first_pixel_word == (trailer ? 0 : f.first),
                  "EOS snapshots retain geometry but never invent saved pixels for trailer EOS");
        }

    for (unsigned rejected = 0; rejected < 3; rejected++) {
        struct fixture f;
        uint32_t number = 99;
        uint64_t metadata = 99;

        init(&f);
        ++groups;
        prepare(&f, 0xa0286028, rejected == 0 ? 0 : 7,
                rejected == 1 ? PIB_FORMAT_CHANGE_BIT : 0,
                rejected == 2 ? FLEA_DECODE_ERROR_FLAG : 0, true);
        memset(&f.packet.metadata, 0xa5, sizeof(f.packet.metadata));
        bool success = flea_GetPictureInfo(&f.hw, &f.packet, &number, &metadata);
        check(success == (rejected != 1) && metadata_empty(&f.packet),
              "zero-number, format-change and decode-error packets publish no picture metadata");
    }

    for (unsigned failure = 1; failure <= 5; failure++) {
        struct fixture f;
        uint32_t number = 99;
        uint64_t metadata = 99;

        init(&f);
        ++groups;
        fetch(&f);
        check(f.packet.metadata.valid, "prior packet use has a populated snapshot");
        prepare(&f, 0xa0286028, 8, 0, 0, true);
        fail_read = failure;
        check(!flea_GetPictureInfo(&f.hw, &f.packet, &number, &metadata) &&
              number == 0 && metadata == 0 && metadata_empty(&f.packet),
              "failed read after successful dequeue clears the prior snapshot");
    }

    for (unsigned failure = 1; failure <= 4; failure++) {
        struct fixture f;
        uint32_t number = 99;
        uint64_t metadata = 99;

        init(&f);
        ++groups;
        prepare(&f, 7, 7, PIB_EOS_DETECTED_BIT, VDEC_FLAG_EOS, true);
        memset(&f.packet.metadata, 0xa5, sizeof(f.packet.metadata));
        fail_read = failure;
        check(!flea_GetPictureInfo(&f.hw, &f.packet, &number, &metadata) &&
              number == 0 && metadata == 0 && metadata_empty(&f.packet),
              "failed EOS read publishes no metadata even after sentinel preparation");
    }
}

static struct crystalhd_rx_completion completion_from(const struct fixture *f)
{
    struct crystalhd_rx_completion result = {
        .buffer = f->packet.buffer,
        .cookie = f->packet.cookie,
        .pib = {.unused = 0xdeadbeef},
        .metadata = f->packet.metadata,
        .capture_epoch = 27,
        .flags = f->packet.flags,
        .y_done_sz = f->packet.y_done_sz,
    };

    return result;
}

static bool image_empty(const struct crystalhd_rx_image *image)
{
    static const struct crystalhd_rx_image empty;

    return !memcmp(image, &empty, sizeof(empty));
}

static void finish_success_cases(void)
{
    const uint32_t first_words[] = {0xa0286028, 32, 0};

    for (unsigned pattern = 0; pattern < 3; pattern++)
        for (unsigned alignment_row = 0; alignment_row < 2; alignment_row++) {
            struct fixture f;
            struct crystalhd_rx_completion result, before;
            struct crystalhd_rx_image image, first_image;
            struct crystalhd_rx_buffer_ops write_only = {.write = buffer_write};
            uint32_t prepared[2048];
            BC_PIC_INFO_BLOCK pib;

            init(&f);
            ++groups;
            prepare(&f, first_words[pattern], 7, 0, VDEC_FLAG_LAST_PICTURE, true);
            pib = read_pib(&f);
            pib.height -= alignment_row;
            pib.timeStamp = 0;
            memcpy((uint8_t *)f.words + f.pib_offset, &pib, sizeof(pib));
            memcpy(f.original, f.words, sizeof(f.original));
            fetch(&f);
            result = completion_from(&f);
            before = result;
            memcpy(prepared, f.words, sizeof(prepared));
            // Prove the completed snapshot is independent of packet reuse,
            // legacy PIB contents, reads and hardware/cache callbacks.
            memset(&f.packet.metadata, 0, sizeof(f.packet.metadata));
            f.buffer.ops = &write_only;
            reset_faults();
            memset(&image, 0xa5, sizeof(image));
            check(crystalhd_rx_finish_yuyv(&result, &image) == BC_STS_SUCCESS,
                  "progressive YUYV finishes from an owned by-value snapshot");
            check(image.width == 64 && image.height == 32 - alignment_row &&
                  image.stride_bytes == 128 &&
                  image.payload_bytes == 128 * (32 - alignment_row) &&
                  image.payload_bytes < result.y_done_sz * 4,
                  "visible payload excludes an optional alignment row, PIB and DMA trailer");
            check(f.words[0] == f.first && writes == 1 && !reads && !syncs &&
                  !memcmp((uint8_t *)f.words + 4, (uint8_t *)prepared + 4,
                          sizeof(prepared) - 4),
                  "finishing restores exactly the saved first word with no parsing or sync");
            check(!memcmp(&result, &before, sizeof(result)) &&
                  result.metadata.valid && result.metadata.firmware_timestamp == 0 &&
                  result.metadata.picture_flags == VDEC_FLAG_LAST_PICTURE,
                  "finishing retains ownership and zero-timestamp/LAST_PICTURE metadata");
            first_image = image;
            memcpy(prepared, f.words, sizeof(prepared));
            check(crystalhd_rx_finish_yuyv(&result, &image) == BC_STS_SUCCESS &&
                  !memcmp(&image, &first_image, sizeof(image)) && writes == 2 &&
                  !reads && !syncs && !memcmp(f.words, prepared, sizeof(prepared)) &&
                  !memcmp(&result, &before, sizeof(result)),
                  "repeated finish is byte-idempotent even when original pixels equal the marker");
        }

    {
        struct fixture f;
        struct crystalhd_rx_completion result;
        struct crystalhd_rx_image image;

        init(&f);
        ++groups;
        set_completion_tail(&f, f.pib_offset + 2 * sizeof(BC_PIC_INFO_BLOCK) - 132);
        fetch(&f);
        result = completion_from(&f);
        reset_faults();
        check(crystalhd_rx_finish_yuyv(&result, &image) == BC_STS_SUCCESS &&
              image.payload_bytes == 4096 && writes == 1 && !reads && !syncs,
              "valid PIB extension beyond completed bytes does not enlarge visible image payload");
    }
}

static void finish_rejection_cases(void)
{
    enum variant {
        NO_BUFFER, NO_COOKIE, WRONG_COOKIE, NO_OPS, NO_WRITE, FORMAT_CHANGE,
        NO_DATA, NO_METADATA, DECODE_ERROR, PLANAR, UYVY, INVALID_MODE,
        UV_OFFSET, UV_DONE, ENCRYPTED, BOTTOM_DATA, FIELDPAIR, TOP_FIELD,
        BOTTOM_FIELD, INTERLACED, UNKNOWN_SOURCE, BOTTOM_FIRST, ZERO_WIDTH,
        ODD_WIDTH, WIDTH_MISMATCH, WIDE_ROW, ZERO_HEIGHT, HUGE_HEIGHT,
        LINE_BEFORE_IMAGE, TOO_MANY_ROWS, HUGE_LINE, SHORT_DONE, HUGE_DONE,
        DONE_PAST_CAPACITY, PIXELS_PAST_DONE, NO_PIB_WORD, SHORT_CAPACITY,
    };
    const struct {
        enum variant variant;
        BC_STATUS status;
        const char *name;
    } cases[] = {
        {NO_BUFFER, BC_STS_INV_ARG, "finish rejects missing buffer"},
        {NO_COOKIE, BC_STS_INV_ARG, "finish rejects missing ownership cookie"},
        {WRONG_COOKIE, BC_STS_INV_ARG, "finish rejects mismatched ownership cookie"},
        {NO_OPS, BC_STS_INV_ARG, "finish rejects missing buffer operations"},
        {NO_WRITE, BC_STS_INV_ARG, "finish rejects missing write operation"},
        {FORMAT_CHANGE, BC_STS_NO_DATA, "finish does not expose a format dummy as pixels"},
        {NO_DATA, BC_STS_IO_ERROR, "finish rejects a non-data completion"},
        {NO_METADATA, BC_STS_NOT_IMPL, "finish cannot reinterpret Link or missing snapshots"},
        {DECODE_ERROR, BC_STS_IO_ERROR, "finish rejects firmware decode errors"},
        {PLANAR, BC_STS_NOT_IMPL, "finish rejects planar output"},
        {UYVY, BC_STS_NOT_IMPL, "finish rejects UYVY without swapping pixels"},
        {INVALID_MODE, BC_STS_NOT_IMPL, "finish rejects unknown output modes"},
        {UV_OFFSET, BC_STS_NOT_IMPL, "finish rejects a split Y/UV buffer"},
        {UV_DONE, BC_STS_NOT_IMPL, "finish rejects unexpected UV completion"},
        {ENCRYPTED, BC_STS_NOT_IMPL, "finish rejects encrypted output"},
        {BOTTOM_DATA, BC_STS_NOT_IMPL, "finish rejects bottom-field completion"},
        {FIELDPAIR, BC_STS_NOT_IMPL, "finish rejects field-pair pictures"},
        {TOP_FIELD, BC_STS_NOT_IMPL, "finish rejects top-field pictures"},
        {BOTTOM_FIELD, BC_STS_NOT_IMPL, "finish rejects bottom-field pictures"},
        {INTERLACED, BC_STS_NOT_IMPL, "finish rejects interlaced sources"},
        {UNKNOWN_SOURCE, BC_STS_NOT_IMPL, "finish rejects unknown source scan type"},
        {BOTTOM_FIRST, BC_STS_NOT_IMPL, "finish rejects contradictory bottom-first flags"},
        {ZERO_WIDTH, BC_STS_INV_ARG, "finish rejects zero width"},
        {ODD_WIDTH, BC_STS_INV_ARG, "finish rejects incomplete YUYV pixel pairs"},
        {WIDTH_MISMATCH, BC_STS_INV_ARG, "finish rejects mismatched PIB and DMA row widths"},
        {WIDE_ROW, BC_STS_INV_ARG, "finish rejects rows outside validated Flea bounds"},
        {ZERO_HEIGHT, BC_STS_INV_ARG, "finish rejects zero height"},
        {HUGE_HEIGHT, BC_STS_INV_ARG, "finish rejects height without arithmetic wrap"},
        {LINE_BEFORE_IMAGE, BC_STS_INV_ARG, "finish rejects metadata overlapping image rows"},
        {TOO_MANY_ROWS, BC_STS_INV_ARG, "finish rejects unsupported multiple padding rows"},
        {HUGE_LINE, BC_STS_INV_ARG, "finish rejects PIB lines outside validated Flea bounds"},
        {SHORT_DONE, BC_STS_INV_ARG, "finish rejects short DMA completions"},
        {HUGE_DONE, BC_STS_INV_ARG, "finish rejects overflowing DWORD-to-byte extents"},
        {DONE_PAST_CAPACITY, BC_STS_INV_ARG, "finish rejects completed data beyond capacity"},
        {PIXELS_PAST_DONE, BC_STS_INV_ARG, "finish rejects pixels beyond completed DMA data"},
        {NO_PIB_WORD, BC_STS_INV_ARG, "finish rejects completion without the saved PIB position"},
        {SHORT_CAPACITY, BC_STS_INV_ARG, "finish rejects backing shorter than the image"},
    };

    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct fixture f;
        struct crystalhd_rx_completion result, before;
        struct crystalhd_rx_image image;
        struct crystalhd_rx_buffer_ops no_write = buffer_ops;
        uint32_t prepared[2048];

        init(&f);
        ++groups;
        fetch(&f);
        result = completion_from(&f);
        switch (cases[i].variant) {
        case NO_BUFFER: result.buffer = NULL; break;
        case NO_COOKIE: result.cookie = NULL; break;
        case WRONG_COOKIE: result.cookie = &f.number; break;
        case NO_OPS: f.buffer.ops = NULL; break;
        case NO_WRITE: no_write.write = NULL; f.buffer.ops = &no_write; break;
        case FORMAT_CHANGE: result.flags = COMP_FLAG_FMT_CHANGE | COMP_FLAG_PIB_VALID; break;
        case NO_DATA: result.flags = 0; break;
        case NO_METADATA: result.metadata.valid = false; break;
        case DECODE_ERROR: result.metadata.picture_flags = FLEA_DECODE_ERROR_FLAG; break;
        case PLANAR: f.buffer.output_format = MODE420; break;
        case UYVY: f.buffer.output_format = MODE422_UYVY; break;
        case INVALID_MODE: f.buffer.output_format = OUTPUT_MODE_INVALID; break;
        case UV_OFFSET: f.buffer.uv_offset = 4096; break;
        case UV_DONE: result.uv_done_sz = 1; break;
        case ENCRYPTED: result.flags |= COMP_FLAG_DATA_ENC; break;
        case BOTTOM_DATA: result.flags |= COMP_FLAG_DATA_BOT; break;
        case FIELDPAIR: result.metadata.picture_flags = VDEC_FLAG_FIELDPAIR; break;
        case TOP_FIELD: result.metadata.picture_flags = VDEC_FLAG_TOPFIELD; break;
        case BOTTOM_FIELD: result.metadata.picture_flags = VDEC_FLAG_BOTTOMFIELD; break;
        case INTERLACED: result.metadata.picture_flags = VDEC_FLAG_INTERLACED_SRC; break;
        case UNKNOWN_SOURCE: result.metadata.picture_flags = VDEC_FLAG_UNKNOWN_SRC; break;
        case BOTTOM_FIRST: result.metadata.picture_flags = VDEC_FLAG_BOTTOM_FIRST; break;
        case ZERO_WIDTH: result.metadata.picture_width = 0; break;
        case ODD_WIDTH: result.metadata.picture_width = result.metadata.row_width = 63; break;
        case WIDTH_MISMATCH: result.metadata.picture_width = 62; break;
        case WIDE_ROW: result.metadata.picture_width = result.metadata.row_width = UINT32_MAX - 1; break;
        case ZERO_HEIGHT: result.metadata.picture_height = 0; break;
        case HUGE_HEIGHT: result.metadata.picture_height = UINT32_MAX; break;
        case LINE_BEFORE_IMAGE: result.metadata.pib_line = 31; break;
        case TOO_MANY_ROWS: result.metadata.pib_line = 34; break;
        case HUGE_LINE: result.metadata.pib_line = UINT32_MAX; break;
        case SHORT_DONE: result.y_done_sz = 1; break;
        case HUGE_DONE: f.buffer.capacity = UINT32_MAX; result.y_done_sz = UINT32_MAX; break;
        case DONE_PAST_CAPACITY: result.y_done_sz = f.buffer.capacity / 4 + 1; break;
        case PIXELS_PAST_DONE: result.y_done_sz = 4096 / 4 - 1; break;
        case NO_PIB_WORD: result.y_done_sz = 4096 / 4; break;
        case SHORT_CAPACITY: f.buffer.capacity = 4092; result.y_done_sz = 4092 / 4; break;
        }
        before = result;
        memcpy(prepared, f.words, sizeof(prepared));
        memset(&image, 0xa5, sizeof(image));
        reset_faults();
        check(crystalhd_rx_finish_yuyv(&result, &image) == cases[i].status, cases[i].name);
        check(image_empty(&image) && !writes && !reads && !syncs &&
              !memcmp(f.words, prepared, sizeof(prepared)) &&
              !memcmp(&result, &before, sizeof(result)),
              "rejected finish clears output without storage I/O or ownership changes");
    }

    {
        struct crystalhd_rx_image image;
        struct crystalhd_rx_completion result = {0};

        ++groups;
        memset(&image, 0xa5, sizeof(image));
        reset_faults();
        check(crystalhd_rx_finish_yuyv(NULL, &image) == BC_STS_INV_ARG && image_empty(&image),
              "missing completion still clears output");
        check(crystalhd_rx_finish_yuyv(&result, NULL) == BC_STS_INV_ARG &&
              !writes && !reads && !syncs, "missing image result performs no I/O");
    }
}

static void finish_nonpicture_cases(void)
{
    for (unsigned variant = 0; variant < 4; variant++) {
        struct fixture f;
        struct crystalhd_rx_completion result, before;
        struct crystalhd_rx_image image;
        uint32_t number, prepared[2048];
        uint64_t timestamp;

        init(&f);
        ++groups;
        prepare(&f, 7, 7,
                variant == 3 ? PIB_FORMAT_CHANGE_BIT :
                variant != 0 ? PIB_EOS_DETECTED_BIT : 0,
                variant != 1 && variant != 3 ? VDEC_FLAG_EOS : 0, true);
        (void)flea_GetPictureInfo(&f.hw, &f.packet, &number, &timestamp);
        result = completion_from(&f);
        before = result;
        memcpy(prepared, f.words, sizeof(prepared));
        memset(&image, 0xa5, sizeof(image));
        reset_faults();
        check(crystalhd_rx_finish_yuyv(&result, &image) == BC_STS_NO_DATA &&
              image_empty(&image) && !writes && !reads && !syncs &&
              !memcmp(&result, &before, sizeof(result)) &&
              !memcmp(f.words, prepared, sizeof(prepared)),
              "actual independent EOS indications and format packets remain nonpicture evidence");
    }

    for (unsigned bottom_field = 0; bottom_field < 2; bottom_field++) {
        struct fixture f;
        struct crystalhd_rx_completion result, before;
        struct crystalhd_rx_image image;
        uint32_t prepared[2048];

        init(&f);
        ++groups;
        prepare(&f, 0xa0286028, 7 | (bottom_field ? UINT32_C(0x40000000) :
                                  UINT32_C(0x80000000)), 0, 0, true);
        fetch(&f);
        result = completion_from(&f);
        before = result;
        memcpy(prepared, f.words, sizeof(prepared));
        memset(&image, 0xa5, sizeof(image));
        reset_faults();
        check(crystalhd_rx_finish_yuyv(&result, &image) == BC_STS_NOT_IMPL &&
              image_empty(&image) && !writes && !reads && !syncs &&
              !memcmp(&result, &before, sizeof(result)) &&
              !memcmp(f.words, prepared, sizeof(prepared)),
              "real parsed encrypted/bottom picture-number sideband bits reject native pixels");
    }
}

static void finish_write_retries(void)
{
    for (unsigned variant = 0; variant < 3; variant++) {
        struct fixture f;
        struct crystalhd_rx_completion result, before;
        struct crystalhd_rx_image image;
        uint32_t prepared[2048];
        BC_STATUS expected = variant == 2 ? BC_STS_NO_ACCESS : BC_STS_IO_ERROR;

        init(&f);
        ++groups;
        fetch(&f);
        result = completion_from(&f);
        before = result;
        memcpy(prepared, f.words, sizeof(prepared));
        reset_faults();
        fail_write = 1;
        partial_write = variant == 1;
        write_error = expected;
        memset(&image, 0xa5, sizeof(image));
        check(crystalhd_rx_finish_yuyv(&result, &image) == expected &&
              image_empty(&image) && writes == 1 && !reads && !syncs &&
              !memcmp(&result, &before, sizeof(result)) &&
              !memcmp((uint8_t *)f.words + 4, (uint8_t *)prepared + 4,
                      sizeof(prepared) - 4),
              "failed writes propagate status, publish no image and retain the owned snapshot");
        check((f.words[0] != prepared[0]) == partial_write,
              "a partial callback failure may already have modified the first word");
        reset_faults();
        check(crystalhd_rx_finish_yuyv(&result, &image) == BC_STS_SUCCESS &&
              f.words[0] == f.first && image.payload_bytes == 4096 &&
              writes == 1 && !reads && !syncs &&
              !memcmp(&result, &before, sizeof(result)),
              "retry restores all saved bytes without re-reading a partly changed marker");
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
    kernel_snapshot_cases();
    finish_success_cases();
    finish_rejection_cases();
    finish_nonpicture_cases();
    finish_write_retries();
    printf("Flea PIB: %u groups, %u checks, %u failures\n", groups, checks, failures);
    return failures ? 1 : 0;
}
