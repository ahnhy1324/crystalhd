/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Actual BCM70015 peek/PIB functions, with only memory/queue primitives mocked.
 * These tests do not load a module, open a device, or emulate firmware decode.
 */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bc_dts_types.h"
struct _BC_DTS_PROC_OUT;
#include "bc_dts_defs.h"

#define GFP_KERNEL 0
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
struct crystalhd_dio_req {
    struct { uint8_t *xfr_buff; uint32_t y_done_sz; bool b422mode; } uinfo;
    void *pib_va;
};
struct crystalhd_rx_dma_pkt {
    struct crystalhd_dio_req *dio_req;
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

static unsigned checks, groups, failures, live_allocations;
static unsigned reads, writes, allocations, fail_read, fail_write, fail_alloc;
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
    check(live_allocations == 0, "no scratch allocation survives a call");
    reads = writes = allocations = fail_read = fail_write = fail_alloc = 0;
    partial_write = false;
}
static void *kmalloc(size_t size, int flags)
{
    (void)flags;
    if (++allocations == fail_alloc) return NULL;
    void *p = malloc(size);
    assert(p);
    ++live_allocations;
    return p;
}
static void kfree(void *p)
{
    if (p) {
        assert(live_allocations);
        --live_allocations;
        free(p);
    }
}
static unsigned long copy_from_user(void *to, const void *from, size_t size)
{
    if (++reads == fail_read) return size;
    memcpy(to, from, size);
    return 0;
}
static unsigned long copy_to_user(void *to, const void *from, size_t size)
{
    if (++writes == fail_write) {
        if (partial_write) memcpy(to, from, size / 2);
        return partial_write ? size - size / 2 : size;
    }
    memcpy(to, from, size);
    return 0;
}
static void crystalhd_dio_to_cpu(struct crystalhd_adp *adp, struct crystalhd_dio_req *dio)
{
    (void)adp;
    check(dio != NULL, "DMA ownership conversion has an existing buffer");
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
    struct crystalhd_dio_req dio;
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
    memset(f->words, 0x5a, sizeof(f->words));
    f->first = first;
    f->number = picture;
    f->line = (width_flags & PIB_EOS_DETECTED_BIT) ? 0 : 32;
    f->pib_offset = f->line * 64 * (packed ? 2 : 1) + 4;
    f->words[0] = first;
    f->words[2046] = 64 | width_flags;
    f->words[2047] = f->line;
    // Real firmware places a picture-number DWORD before the linear PIB.
    if (f->line) f->words[(f->pib_offset - 4) / 4] = picture;
    BC_PIC_INFO_BLOCK pib = {0};
    pib.width = 64; pib.height = 32; pib.picture_number = picture;
    pib.timeStamp = 123400 + picture; pib.flags = picture_flags;
    pib.ycom = 0xdeadbeef;
    memcpy((uint8_t *)f->words + f->pib_offset, &pib, sizeof(pib));
    f->packet.flags = COMP_FLAG_DATA_VALID;
    f->dio.uinfo.b422mode = packed;
    memcpy(f->original, f->words, sizeof(f->words));
    reset_faults();
}

static void init(struct fixture *f)
{
    memset(f, 0, sizeof(*f));
    f->adp.pdev = &f->pci;
    f->dio.uinfo.xfr_buff = (uint8_t *)f->words;
    f->dio.uinfo.y_done_sz = sizeof(f->words) / 4;
    f->packet.dio_req = &f->dio;
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
    check(crystalhd_flea_peek_next_decoded_frame(&f->hw, &metadata, &number, 64),
          "ready-queue status peek retains its success convention");
    const uint32_t expected_number =
        (f->words[2046] & PIB_EOS_DETECTED_BIT) && f->line == 0 ? UINT32_MAX : f->number;
    check(number == expected_number && metadata == 123400 + f->number,
          "status peek returns unchanged picture/timestamp association");
    check(writes == 0 && memcmp(f->words, f->original, sizeof(f->words)) == 0,
          "status peek never changes any image or embedded metadata bytes");
    check(live_allocations == 0 && f->dio.pib_va == NULL,
          "successful peek releases and clears scratch storage");
}

static void fetch(struct fixture *f)
{
    uint32_t expected[2048];
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
    check(live_allocations == 0 && f->dio.pib_va == NULL,
          "dequeue releases and clears scratch storage");
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
    check(live_allocations == 0 && f.dio.pib_va == NULL, "EOS scratch storage released");

    ++groups;
    prepare(&f, 0xa0286028, 7, 0, VDEC_FLAG_EOS, true);
    f.words[2046] |= PIB_EOS_DETECTED_BIT;
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
    for (unsigned failure = 1; failure <= 2; ++failure) {
        struct fixture f;
        init(&f);
        ++groups;
        peek(&f); // A failed allocation after a prior success must not double-free.
        reset_faults();
        fail_alloc = failure;
        uint32_t number = 99;
        uint64_t metadata = 99;
        crystalhd_flea_peek_next_decoded_frame(&f.hw, &metadata, &number, 64);
        check(number == 0 && metadata == 0 && writes == 0 && live_allocations == 0 && f.dio.pib_va == NULL,
              "either scratch-allocation failure leaves no pixels, outputs or stale allocation");
        check(memcmp(f.words, f.original, sizeof(f.words)) == 0, "allocation failure leaves the picture intact");
        reset_faults();
        peek(&f);
        fetch(&f);
    }
    for (unsigned failure = 1; failure <= 5; ++failure) {
        struct fixture f;
        init(&f);
        ++groups;
        fail_read = failure;
        uint32_t number = 99;
        uint64_t metadata = 99;
        crystalhd_flea_peek_next_decoded_frame(&f.hw, &metadata, &number, 64);
        check(number == 0 && metadata == 0 && writes == 0 && live_allocations == 0 && f.dio.pib_va == NULL,
              "failed peek read publishes no picture and releases all scratch allocations");
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
            check(number == 0 && metadata == 0 && live_allocations == 0 && f.dio.pib_va == NULL,
                  "failed dequeue write clears outputs and releases temporary allocations");
            // The actual fetch caller drops PicNumber==0 back to the free queue.
            // A subsequent hardware completion supplies a new image; do not
            // pretend failed copy_to_user is atomic or retry mutated pixels.
            prepare(&f, 0x70907050, 8, 0, 0, true);
            peek(&f);
            fetch(&f);
        }
    {
        struct fixture f;
        init(&f);
        ++groups;
        f.packet.dio_req = NULL;
        uint32_t number = 99;
        uint64_t metadata = 99;
        check(!flea_GetPictureInfo(&f.hw, &f.packet, &number, &metadata) &&
              number == 0 && metadata == 0 && live_allocations == 0,
              "missing backing fails without dereferencing or freeing nonexistent storage");
    }
}

int main(void)
{
    repeated_peeks();
    special_pictures();
    failures_and_retries();
    printf("Flea PIB: %u groups, %u checks, %u failures\n", groups, checks, failures);
    return failures ? 1 : 0;
}
