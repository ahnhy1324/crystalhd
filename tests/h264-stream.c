/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Source-extracted H.264 stream formatter and synchronous sender. */
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bc_dts_types.h"
struct _BC_DTS_PROC_OUT;
#include "bc_dts_defs.h"

#ifndef ERESTARTSYS
#define ERESTARTSYS 512
#endif
#ifndef EKEYREJECTED
#define EKEYREJECTED 129
#endif

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint64_t dma_addr_t;

struct scatterlist {
    dma_addr_t dma_address;
    unsigned int dma_length;
    bool last;
};

#define sg_dma_address(sg) ((sg)->dma_address)
#define sg_dma_len(sg) ((sg)->dma_length)
static void sg_init_table(struct scatterlist *sg, unsigned int count)
{
    if (count != 1) abort();
    *sg = (struct scatterlist){ .last = true };
}

#include "h264-stream-types.h"

struct device { int unused; };
struct pci_dev { struct device dev; u32 device; };
struct crystalhd_hw { int unused; };
struct crystalhd_stream;
struct crystalhd_adp {
    struct pci_dev *pdev;
    bool present;
    int user_lock;
    int tx_lock;
};
struct crystalhd_cmd {
    u32 state;
    struct crystalhd_adp *adp;
    const void *session_owner;
    enum crystalhd_decoder_phase decoder_phase;
    enum crystalhd_decoder_codec decoder_codec;
    u32 fw_sequence;
    u32 decoder_channel_id;
    struct crystalhd_stream *stream;
    struct crystalhd_hw *hw_ctx;
};

static void *kzalloc(size_t size, int flags);
static void kfree(void *memory);
static void *dma_alloc_coherent(struct device *device, size_t size,
                                dma_addr_t *dma, int flags);
static void dma_free_coherent(struct device *device, size_t size,
                              void *cpu, dma_addr_t dma);
static void DmaWriteBarrier(void);
static void AuditLock(const int *lock);
static BC_STATUS crystalhd_tx_deadline_from_ms(u32 timeout_ms,
                                                unsigned long *deadline);
static BC_STATUS crystalhd_tx_transfer_until(
    struct crystalhd_cmd *ctx, const struct crystalhd_tx_buffer *buffer,
    u8 data_flags, unsigned long deadline);
int crystalhd_status_to_errno(BC_STATUS status);

#define GFP_KERNEL 0
#define READ_ONCE(value) (value)
#define ALIGN(value, alignment) \
    (((value) + (alignment) - 1U) & ~((alignment) - 1U))
#define min(left, right) ((left) < (right) ? (left) : (right))
#define GENMASK_ULL(high, low) \
    ((((~0ULL) - (1ULL << (low)) + 1ULL) & \
      (~0ULL >> (63U - (high)))))
#define lockdep_assert_held(lock) AuditLock(lock)
#define dma_wmb() DmaWriteBarrier()

#include "h264-stream-production.h"

#define MAX_PACKETS 4U

struct captured_packet {
    size_t bytes;
    u8 data[CRYSTALHD_H264_STAGE_BYTES];
    dma_addr_t sg_address;
    unsigned int sg_length;
    dma_addr_t tail_address;
    u32 tail_size;
    unsigned long deadline;
};

static unsigned checks, failures;
static struct pci_dev endpoint;
static struct crystalhd_adp adapter;
static struct crystalhd_hw hardware;
static struct crystalhd_cmd context;
static int owner_token, foreign_owner;
static void *stage_cpu, *stream_heap;
static dma_addr_t stage_dma, next_dma;
static bool fail_stream_alloc, fail_dma_alloc;
static unsigned stream_allocs, stream_frees, dma_allocs, dma_frees;
static unsigned barriers, lock_assertions, user_lock_assertions;
static unsigned tx_lock_assertions, deadline_calls, transfer_calls;
static u32 seen_timeout;
static unsigned long supplied_deadline;
static BC_STATUS transfer_status[MAX_PACKETS];
static struct captured_packet captured[MAX_PACKETS];

static void Check(bool condition, const char *why)
{
    checks++;
    if (!condition) {
        failures++;
        fprintf(stderr, "FAIL: %s\n", why);
    }
}

static void *kzalloc(size_t size, int flags)
{
    void *memory;

    Check(size == sizeof(struct crystalhd_stream) && flags == GFP_KERNEL,
          "stream allocation has the exact private-object size");
    if (fail_stream_alloc)
        return NULL;
    memory = calloc(1, size);
    if (!memory) abort();
    Check(!stream_heap, "only one stream object is live");
    stream_heap = memory;
    stream_allocs++;
    return memory;
}

static void kfree(void *memory)
{
    if (!memory)
        return;
    Check(memory == stream_heap, "stream release frees its exact heap owner");
    stream_heap = NULL;
    stream_frees++;
    free(memory);
}

static void *dma_alloc_coherent(struct device *device, size_t size,
                                dma_addr_t *dma, int flags)
{
    void *memory;

    Check(device == &endpoint.dev && size == CRYSTALHD_H264_STAGE_BYTES &&
          dma && flags == GFP_KERNEL,
          "coherent staging allocation uses the endpoint and exact capacity");
    if (fail_dma_alloc)
        return NULL;
    memory = calloc(1, size);
    if (!memory) abort();
    Check(!stage_cpu, "only one coherent staging buffer is live");
    stage_cpu = memory;
    stage_dma = next_dma;
    *dma = stage_dma;
    dma_allocs++;
    return memory;
}

static void dma_free_coherent(struct device *device, size_t size,
                              void *cpu, dma_addr_t dma)
{
    Check(device == &endpoint.dev && size == CRYSTALHD_H264_STAGE_BYTES &&
          cpu == stage_cpu && dma == stage_dma,
          "coherent staging release accepts the original CPU and DMA owners");
    stage_cpu = NULL;
    dma_frees++;
    free(cpu);
}

static void DmaWriteBarrier(void)
{
    barriers++;
}

static void AuditLock(const int *lock)
{
    Check((lock == &adapter.user_lock || lock == &adapter.tx_lock) &&
          *lock == 1,
          "typed submission retains session lifetime and TX serialization");
    lock_assertions++;
    if (lock == &adapter.user_lock)
        user_lock_assertions++;
    else if (lock == &adapter.tx_lock)
        tx_lock_assertions++;
}

static BC_STATUS crystalhd_tx_deadline_from_ms(u32 timeout_ms,
                                                unsigned long *deadline)
{
    deadline_calls++;
    seen_timeout = timeout_ms;
    if (!deadline || !timeout_ms || timeout_ms > INT_MAX)
        return BC_STS_INV_ARG;
    *deadline = supplied_deadline;
    return BC_STS_SUCCESS;
}

static BC_STATUS crystalhd_tx_transfer_until(
    struct crystalhd_cmd *ctx, const struct crystalhd_tx_buffer *buffer,
    u8 data_flags, unsigned long deadline)
{
    const struct crystalhd_stream *stream = ctx->stream;
    struct captured_packet *packet;
    size_t aligned;
    unsigned index = transfer_calls++;

    Check(index < MAX_PACKETS, "typed submission stays within packet fixture capacity");
    if (index >= MAX_PACKETS)
        return BC_STS_ERROR;
    packet = &captured[index];
    aligned = buffer->bytes & ~3U;
    Check(ctx == &context && stream && buffer && data_flags == 0,
          "typed sender delegates one clear mapped transfer");
    Check(buffer->cookie == stream && buffer->sgl == &stream->sg &&
          buffer->dma_nents == 1,
          "staged transfer retains one stable coherent SG owner");
    Check(buffer->bytes && buffer->bytes <= CRYSTALHD_H264_PES_MAX_WIRE &&
          buffer->tail_size == (buffer->bytes & 3U),
          "staged transfer advertises the exact unpadded wire length");
    Check(sg_dma_address(buffer->sgl) == stage_dma &&
          sg_dma_len(buffer->sgl) == aligned,
          "main SG covers only complete aligned words");
    Check(buffer->tail_addr == stage_dma + aligned &&
          buffer->tail_size == buffer->bytes - aligned,
          "tail descriptor addresses the final coherent word");
    for (size_t n = buffer->bytes; n < ALIGN(buffer->bytes, 4U); n++)
        Check(((const u8 *)stage_cpu)[n] == 0,
              "DMA-only alignment bytes are zero without extending the stream");
    Check(barriers == transfer_calls,
          "CPU packet writes are ordered before every DMA submission");
    packet->bytes = buffer->bytes;
    memcpy(packet->data, stage_cpu, buffer->bytes);
    packet->sg_address = sg_dma_address(buffer->sgl);
    packet->sg_length = sg_dma_len(buffer->sgl);
    packet->tail_address = buffer->tail_addr;
    packet->tail_size = buffer->tail_size;
    packet->deadline = deadline;
    return transfer_status[index];
}

static void Reset(void)
{
    Check(!stage_cpu && !stream_heap,
          "each stream test begins without leaked staging ownership");
    endpoint = (struct pci_dev){ .device = BC_PCI_DEVID_FLEA };
    adapter = (struct crystalhd_adp){
        .pdev = &endpoint, .present = true, .user_lock = 1, .tx_lock = 1,
    };
    context = (struct crystalhd_cmd){
        .state = BC_LINK_INIT,
        .adp = &adapter,
        .session_owner = &owner_token,
        .decoder_phase = CRYSTALHD_DECODER_CHANNEL_STARTED,
        .decoder_codec = CRYSTALHD_DECODER_CODEC_H264,
        .fw_sequence = 7,
        .hw_ctx = &hardware,
    };
    next_dma = 0x100000U;
    fail_stream_alloc = fail_dma_alloc = false;
    stream_allocs = stream_frees = dma_allocs = dma_frees = 0;
    barriers = lock_assertions = user_lock_assertions = 0;
    tx_lock_assertions = deadline_calls = transfer_calls = 0;
    seen_timeout = 0;
    supplied_deadline = 0xabc123UL;
    memset(transfer_status, 0, sizeof(transfer_status));
    memset(captured, 0, sizeof(captured));
}

static void Prepare(void)
{
    Check(crystalhd_stream_prepare(&context) == 0 && context.stream &&
          stream_allocs == 1 && dma_allocs == 1,
          "valid typed session owns one coherent H.264 staging buffer");
}

static void Finish(void)
{
    crystalhd_stream_release(&context);
    Check(!context.stream && !stage_cpu && !stream_heap &&
          stream_frees == stream_allocs && dma_frees == dma_allocs,
          "typed stream release balances heap and coherent ownership");
}

static uint64_t Fnv1a64(uint64_t hash, const u8 *bytes, size_t size)
{
    for (size_t i = 0; i < size; i++) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void ResourceLifetime(void)
{
    Reset();
    Check(crystalhd_stream_prepare(NULL) == -ENODEV,
          "stream preparation rejects a missing command context");
    context.adp = NULL;
    Check(crystalhd_stream_prepare(&context) == -ENODEV,
          "stream preparation rejects a retired adapter");

    Reset(); fail_stream_alloc = true;
    Check(crystalhd_stream_prepare(&context) == -ENOMEM && !context.stream &&
          !stream_allocs && !dma_allocs,
          "private stream allocation failure has no coherent side effect");

    Reset(); fail_dma_alloc = true;
    Check(crystalhd_stream_prepare(&context) == -ENOMEM && !context.stream &&
          stream_allocs == 1 && stream_frees == 1 && !dma_allocs,
          "coherent allocation failure releases the private stream owner");

    Reset(); next_dma = 0;
    Prepare();
    Check(context.stream->dma == 0,
          "DMA address zero remains a valid coherent allocation");
    Check(crystalhd_stream_prepare(&context) == 0 && stream_allocs == 1 &&
          dma_allocs == 1,
          "repeated preparation reuses the session staging object");
    context.adp = NULL;
    Finish();
    crystalhd_stream_release(&context);
    Check(stream_frees == 1 && dma_frees == 1,
          "repeated release is idempotent");
}

static void Formatter(void)
{
    static const u8 source[] = { 0x11, 0x22, 0x33 };
    static const u8 plain_expected[] = {
        0x00, 0x00, 0x01, 0xe0, 0x00, 0x06, 0x81, 0x00, 0x00,
        0x11, 0x22, 0x33,
    };
    static const u8 zero_pts_expected[] = {
        0x00, 0x00, 0x01, 0xe0, 0x00, 0x0b, 0x81, 0x80, 0x05,
        0x21, 0x00, 0x01, 0x00, 0x01, 0x11, 0x22, 0x33,
    };
    u8 output[64], before[64];
    size_t consumed, wire;

    memset(output, 0xa5, sizeof(output));
    consumed = wire = SIZE_MAX;
    Check(crystalhd_h264_format_pes(output, sizeof(output), source,
                                    sizeof(source), false, 0,
                                    &consumed, &wire) == 0 &&
          consumed == sizeof(source) && wire == sizeof(plain_expected) &&
          !memcmp(output, plain_expected, sizeof(plain_expected)),
          "ordinary H.264 input receives the exact legacy PES header");

    memset(output, 0xa5, sizeof(output));
    Check(crystalhd_h264_format_pes(output, sizeof(output), source,
                                    sizeof(source), true, 0,
                                    &consumed, &wire) == 0 &&
          wire == sizeof(zero_pts_expected) &&
          !memcmp(output, zero_pts_expected, sizeof(zero_pts_expected)),
          "explicit PTS validity can encode raw timestamp zero");

    Check(crystalhd_h264_format_pes(output, sizeof(output), source,
                                    sizeof(source), true,
                                    UINT64_C(0x1ffffffff),
                                    &consumed, &wire) == 0 &&
          output[9] == 0x2f && output[10] == 0xff &&
          output[11] == 0xff && output[12] == 0xff &&
          output[13] == 0xff,
          "maximum raw 33-bit PTS uses the exact marker-bit encoding");
    Check(crystalhd_h264_format_pes(output, sizeof(output), source,
                                    sizeof(source), true,
                                    UINT64_C(0x123456789),
                                    &consumed, &wire) == 0 &&
          output[9] == 0x29 && output[10] == 0x8d &&
          output[11] == 0x15 && output[12] == 0xcf &&
          output[13] == 0x13,
          "mixed raw PTS bits preserve every legacy shift group");

    memset(output, 0x5a, sizeof(output));
    memcpy(before, output, sizeof(output));
    consumed = 7; wire = 9;
    Check(crystalhd_h264_format_pes(output, 11, source, sizeof(source),
                                    false, 0, &consumed, &wire) == -ENOSPC &&
          consumed == 7 && wire == 9 &&
          !memcmp(output, before, sizeof(output)),
          "insufficient formatter capacity leaves bytes and outputs unchanged");
    Check(crystalhd_h264_format_pes(NULL, sizeof(output), source,
                                    sizeof(source), false, 0,
                                    &consumed, &wire) == -EINVAL &&
          crystalhd_h264_format_pes(output, sizeof(output), source, 0,
                                    false, 0, &consumed, &wire) == -EINVAL,
          "formatter rejects missing storage and empty input");

    memset(output, 0x3c, sizeof(output));
    memcpy(before, output, sizeof(output));
    wire = 23;
    Check(crystalhd_h264_format_eos(output, sizeof(output), 4, &wire) ==
              -EINVAL && wire == 23 &&
          !memcmp(output, before, sizeof(output)),
          "invalid EOS index leaves destination and length unchanged");
}

static void SenderValidation(void)
{
    static const u8 source[] = { 0, 0, 1, 9 };
    unsigned locks;

    Reset();
    Check(crystalhd_decoder_submit_h264(NULL, &owner_token, source,
                                        sizeof(source), false, 0, 100) ==
              -EINVAL &&
          crystalhd_decoder_submit_h264(&context, NULL, source,
                                        sizeof(source), false, 0, 100) ==
              -EINVAL,
          "typed input rejects missing context and owner");
    Check(crystalhd_decoder_submit_h264(&context, &owner_token, NULL, 1,
                                        false, 0, 100) == -EINVAL &&
          crystalhd_decoder_submit_h264(&context, &owner_token, source, 0,
                                        false, 0, 100) == -EINVAL,
          "typed input rejects absent or empty Annex-B data");
    Check(!deadline_calls && !transfer_calls,
          "invalid input arguments have no deadline or hardware effects");

    locks = lock_assertions;
    Check(crystalhd_decoder_submit_h264(&context, &foreign_owner, source,
                                        sizeof(source), false, 0, 100) ==
              -EBUSY && !deadline_calls && lock_assertions == locks + 2,
          "typed input rejects a foreign session before TX");
    endpoint.device = BC_PCI_DEVID_LINK;
    locks = lock_assertions;
    Check(crystalhd_decoder_submit_h264(&context, &owner_token, source,
                                        sizeof(source), false, 0, 100) ==
              -EOPNOTSUPP && !deadline_calls &&
          lock_assertions == locks + 2,
          "typed H.264 sender rejects BCM70012");
    endpoint.device = BC_PCI_DEVID_FLEA;
    locks = lock_assertions;
    Check(crystalhd_decoder_submit_h264(&context, &owner_token, source,
                                        sizeof(source), false, 0, 100) ==
              -EBUSY && !deadline_calls &&
          lock_assertions == locks + 2,
          "typed input requires prepared stream storage");

    Prepare();
    for (unsigned condition = 0; condition < 7; condition++) {
        adapter.present = true;
        context.session_owner = &owner_token;
        context.state = BC_LINK_INIT;
        context.decoder_phase = CRYSTALHD_DECODER_CHANNEL_STARTED;
        context.decoder_codec = CRYSTALHD_DECODER_CODEC_H264;
        context.fw_sequence = 7;
        context.decoder_channel_id = 0;
        if (condition == 0)
            adapter.present = false;
        else if (condition == 1)
            context.session_owner = NULL;
        else if (condition == 2)
            context.state = BC_LINK_SUSPEND;
        else if (condition == 3)
            context.decoder_phase = CRYSTALHD_DECODER_CHANNEL_CONFIGURED;
        else if (condition == 4)
            context.decoder_codec = CRYSTALHD_DECODER_CODEC_INVALID;
        else if (condition == 5)
            context.fw_sequence = 0;
        else
            context.decoder_channel_id = 1;
        locks = lock_assertions;
        Check(crystalhd_decoder_submit_h264(&context, &owner_token, source,
                                            sizeof(source), false, 0, 100) ==
                  (condition == 0 ? -ENODEV :
                   condition == 1 ? -EINVAL : -EBUSY) &&
              !deadline_calls && !transfer_calls &&
              lock_assertions == locks + 2,
              "prepared typed input rejects every invalid owner/state publication");
    }
    adapter.present = true;
    context.session_owner = &owner_token;
    context.state = BC_LINK_INIT;
    context.decoder_phase = CRYSTALHD_DECODER_CHANNEL_STARTED;
    context.decoder_codec = CRYSTALHD_DECODER_CODEC_H264;
    context.fw_sequence = 7;
    context.decoder_channel_id = 0;
    locks = lock_assertions;
    Check(crystalhd_decoder_submit_h264(&context, &owner_token, source,
                                        sizeof(source), false, 0, 0) ==
              -EINVAL && deadline_calls == 1 && !transfer_calls &&
          lock_assertions == locks + 2,
          "typed input requires one finite whole-submission deadline");
    Check(user_lock_assertions == tx_lock_assertions &&
          lock_assertions == user_lock_assertions + tx_lock_assertions &&
          user_lock_assertions,
          "every typed validation asserts both lifetime and TX locks once");
    Finish();
}

static void OrdinarySender(void)
{
    static const u8 source[] = { 0, 0, 1, 9, 0x10 };
    const size_t payload_bytes = 65512U + 65517U + 3U;
    u8 *large, *copy, *reconstructed;
    size_t rebuilt = 0;

    Reset(); next_dma = 0;
    Prepare();
    Check(crystalhd_decoder_submit_h264(&context, &owner_token, source,
                                        sizeof(source), true, 0, 250) == 0 &&
          deadline_calls == 1 && seen_timeout == 250 && transfer_calls == 1 &&
          captured[0].deadline == supplied_deadline,
          "single PES submission uses one caller budget and accepts DMA zero");
    Check(captured[0].bytes == sizeof(source) + CRYSTALHD_H264_PTS_HEADER &&
          captured[0].data[7] == 0x80 && captured[0].data[9] == 0x21 &&
          !memcmp(captured[0].data + CRYSTALHD_H264_PTS_HEADER,
                  source, sizeof(source)),
          "typed sender preserves Annex-B bytes and explicit zero PTS");
    Finish();

    large = malloc(payload_bytes);
    copy = malloc(payload_bytes);
    reconstructed = malloc(payload_bytes);
    if (!large || !copy || !reconstructed) abort();
    for (size_t i = 0; i < payload_bytes; i++)
        large[i] = (u8)(i * 29U + 7U);
    memcpy(copy, large, payload_bytes);

    Reset(); Prepare();
    Check(crystalhd_decoder_submit_h264(&context, &owner_token, large,
                                        payload_bytes, true,
                                        UINT64_C(0x1ffffffff), 777) == 0 &&
          deadline_calls == 1 && transfer_calls == 3 && barriers == 3,
          "multi-PES access unit shares one deadline across all fragments");
    Check(captured[0].bytes == 65526U && captured[1].bytes == 65526U &&
          captured[2].bytes == 12U && captured[0].data[7] == 0x80 &&
          captured[1].data[7] == 0x00 && captured[2].data[7] == 0x00 &&
          captured[0].data[4] == 0xff && captured[0].data[5] == 0xf0 &&
          captured[1].data[4] == 0xff && captured[1].data[5] == 0xf0,
          "fragmentation uses legacy first and continuation payload limits");
    for (unsigned i = 0; i < 3; i++) {
        size_t header = i ? CRYSTALHD_H264_PES_HEADER :
                            CRYSTALHD_H264_PTS_HEADER;
        size_t payload = captured[i].bytes - header;
        Check(captured[i].deadline == supplied_deadline,
              "every access-unit fragment reuses the original absolute deadline");
        memcpy(reconstructed + rebuilt, captured[i].data + header, payload);
        rebuilt += payload;
    }
    Check(rebuilt == payload_bytes &&
          !memcmp(reconstructed, large, payload_bytes) &&
          !memcmp(large, copy, payload_bytes),
          "fragmented sender reconstructs the source exactly without mutation");
    Finish();

    for (unsigned failed = 0; failed < 3; failed++) {
        unsigned submitted, deadlines;

        Reset(); Prepare();
        transfer_status[failed] = BC_STS_TIMEOUT;
        Check(crystalhd_decoder_submit_h264(&context, &owner_token, large,
                                            payload_bytes, true, 3, 400) ==
                  -ETIMEDOUT && transfer_calls == failed + 1U &&
              deadline_calls == 1,
              "fragment failure stops the access unit without renewing its budget");
        submitted = transfer_calls;
        deadlines = deadline_calls;
        Check(crystalhd_decoder_submit_h264(&context, &owner_token, large, 1,
                                            false, 0, 400) == -EPIPE &&
              transfer_calls == submitted && deadline_calls == deadlines,
              "failed access unit quarantines the staging session from retry");
        Finish();
    }
    free(reconstructed);
    free(copy);
    free(large);
}

static void EosSender(void)
{
    static const u8 eos[] = {
        0x00, 0x00, 0x01, 0xe0, 0x00, 0x0b, 0x81, 0x00, 0x00,
        0x00, 0x00, 0x01, 0x0a, 0x00, 0x00, 0x01, 0x0a,
    };
    uint64_t hash = UINT64_C(14695981039346656037);
    static const u8 source[] = { 0, 0, 1, 9 };

    Reset(); Prepare();
    Check(crystalhd_decoder_submit_h264_eos(&context, &owner_token, 900) == 0 &&
          deadline_calls == 1 && transfer_calls == 4 && barriers == 4,
          "typed EOS submits all four packets under one finite budget");
    Check(captured[0].bytes == 17 && captured[1].bytes == 184 &&
          captured[2].bytes == 17 && captured[3].bytes == 17 &&
          !memcmp(captured[0].data, eos, sizeof(eos)) &&
          !memcmp(captured[2].data, eos, sizeof(eos)) &&
          !memcmp(captured[3].data, eos, sizeof(eos)),
          "typed EOS preserves the exact E-M-E-E packet order");
    Check(captured[1].data[10] == 'B' && captured[1].data[11] == 'R' &&
          captured[1].data[12] == 'C' && captured[1].data[13] == 'M' &&
          captured[1].data[29 + 4] == 0x0c &&
          captured[1].data[29 + 36] == 0xbc,
          "timing marker retains its private tag and fixed body sentinels");
    for (unsigned i = 0; i < 4; i++) {
        Check(captured[i].deadline == supplied_deadline,
              "every EOS packet reuses the original absolute deadline");
        hash = Fnv1a64(hash, captured[i].data, captured[i].bytes);
    }
    Check(hash == UINT64_C(0x17cd22341ff27781),
          "typed EOS bytes match the independent legacy fingerprint");
    Check(crystalhd_decoder_submit_h264_eos(&context, &owner_token, 900) ==
              -EPIPE &&
          crystalhd_decoder_submit_h264(&context, &owner_token, source,
                                        sizeof(source), false, 0, 900) ==
              -EPIPE && transfer_calls == 4 && deadline_calls == 1,
          "submitted EOS blocks duplicate markers and later access units");
    Finish();

    for (unsigned failed = 0; failed < 4; failed++) {
        unsigned submitted, deadlines;

        Reset(); Prepare();
        transfer_status[failed] = BC_STS_IO_ERROR;
        Check(crystalhd_decoder_submit_h264_eos(&context, &owner_token,
                                                900) == -EIO &&
              transfer_calls == failed + 1U && deadline_calls == 1,
              "EOS transport failure suppresses every later marker packet");
        submitted = transfer_calls;
        deadlines = deadline_calls;
        Check(crystalhd_decoder_submit_h264_eos(&context, &owner_token,
                                                900) == -EPIPE &&
              transfer_calls == submitted && deadline_calls == deadlines,
              "partial EOS quarantines its staging session from retry");
        Finish();
    }
}

int main(void)
{
    ResourceLifetime();
    Formatter();
    SenderValidation();
    OrdinarySender();
    EosSender();
    printf("H.264 typed stream: %u checks, %u failures\n", checks, failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
