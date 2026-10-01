// SPDX-License-Identifier: GPL-2.0-or-later
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
struct _BC_DTS_PROC_OUT;
#include "bc_dts_defs.h"
typedef uint64_t u64;
typedef uint32_t u32;
typedef uint8_t u8;
#include "decoder-types.h"
_Static_assert(CRYSTALHD_PICTURE_FLAG_EOS == VDEC_FLAG_EOS,
    "native EOS mask must match the shared firmware PIB flag");
_Static_assert(CRYSTALHD_PICTURE_FLAG_DECODE_ERROR == FLEA_DECODE_ERROR_FLAG,
    "native failed-picture mask must match the actual Flea firmware flag");

struct crystalhd_cmd { enum crystalhd_decoder_phase decoder_phase; };
static const char owner;
static unsigned calls, fail_call, bootstrap_calls;
static u64 last_pts;
struct crystalhd_v4l2_output_buffer { unsigned transport_calls; };
static struct crystalhd_v4l2_output_buffer mapped;
static struct crystalhd_v4l2_decoder *early_eos;
static int step(const void *p)
{
    assert(p == &owner);
    return ++calls == fail_call ? -EIO : 0;
}
static int crystalhd_fw_bootstrap_locked(struct crystalhd_cmd *c, const void *p)
{
    int rc = step(p);
    bootstrap_calls++;
    if (!rc) c->decoder_phase = CRYSTALHD_DECODER_BOOTSTRAPPED;
    return rc;
}
static int crystalhd_decoder_channel_open_locked(struct crystalhd_cmd *c,
    const void *p, const struct crystalhd_decoder_config *config)
{
    assert(c->decoder_phase == CRYSTALHD_DECODER_BOOTSTRAPPED);
    assert(config->codec == CRYSTALHD_DECODER_CODEC_H264);
    int rc = step(p);
    if (!rc) c->decoder_phase = CRYSTALHD_DECODER_CHANNEL_CONFIGURED;
    return rc;
}
static int crystalhd_decoder_channel_start_locked(struct crystalhd_cmd *c,
    const void *p)
{
    assert(c->decoder_phase == CRYSTALHD_DECODER_CHANNEL_CONFIGURED);
    int rc = step(p);
    if (!rc) c->decoder_phase = CRYSTALHD_DECODER_CHANNEL_STARTED;
    return rc;
}
static int crystalhd_decoder_channel_stop_locked(struct crystalhd_cmd *c,
    const void *p)
{
    assert(c->decoder_phase == CRYSTALHD_DECODER_CHANNEL_STARTED);
    int rc = step(p);
    if (!rc) c->decoder_phase = CRYSTALHD_DECODER_CHANNEL_CONFIGURED;
    return rc;
}
static int crystalhd_decoder_channel_close_locked(struct crystalhd_cmd *c,
    const void *p)
{
    assert(c->decoder_phase == CRYSTALHD_DECODER_CHANNEL_CONFIGURED);
    int rc = step(p);
    if (!rc) c->decoder_phase = CRYSTALHD_DECODER_BOOTSTRAPPED;
    return rc;
}
static int crystalhd_decoder_submit_h264(struct crystalhd_cmd *c,
    const void *p, const u8 *data, size_t bytes, bool valid, u64 pts, u32 timeout)
{
    assert(c->decoder_phase == CRYSTALHD_DECODER_CHANNEL_STARTED);
    assert(data && bytes && valid && timeout == 100);
    last_pts = pts;
    return step(p);
}
static int crystalhd_decoder_submit_h264_eos(struct crystalhd_cmd *c,
    const void *p, u32 timeout)
{
    assert(c->decoder_phase == CRYSTALHD_DECODER_CHANNEL_STARTED);
    assert(timeout == 100);
    if (early_eos) {
        struct crystalhd_rx_metadata m = {
            .valid = true, .eos_trailer = true,
            .picture_number = UINT32_MAX, .picture_flags = VDEC_FLAG_EOS,
        };
        u64 timestamp = 0;
        assert(early_eos->phase == CHD_V4L2_DRAINING);
        assert(crystalhd_v4l2_decoder_complete(early_eos, &m,
            early_eos->epoch, &timestamp) == 1);
    }
    return step(p);
}
static int crystalhd_v4l2_output_submit(struct crystalhd_cmd *c, const void *p,
    struct crystalhd_v4l2_output_buffer *buffer, u64 pts, u32 timeout)
{
    assert(c->decoder_phase == CRYSTALHD_DECODER_CHANNEL_STARTED);
    assert(buffer == &mapped && timeout == 100);
    buffer->transport_calls++;
    last_pts = pts;
    return step(p);
}
#include "decoder-production.h"

static const u8 au[] = {0, 0, 1, 0x65};
static int submit(struct crystalhd_v4l2_decoder *d, struct crystalhd_cmd *c, u64 ts)
{
    return crystalhd_v4l2_decoder_submit(d, c, &owner, au, sizeof(au), ts, 100);
}
static void restart(struct crystalhd_v4l2_decoder *d, struct crystalhd_cmd *c)
{
    u64 epoch = d->epoch;
    unsigned boots = bootstrap_calls;
    assert(!crystalhd_v4l2_decoder_stop_locked(d, c, &owner));
    assert(d->phase == CHD_V4L2_STOPPED);
    assert(!crystalhd_v4l2_decoder_close_locked(d, c, &owner));
    assert(d->epoch == epoch + 1 && !d->count);
    assert(!crystalhd_v4l2_decoder_open_locked(d, c, &owner));
    assert(bootstrap_calls == boots);
}

static void mapped_checks(void)
{
    struct crystalhd_v4l2_decoder d;
    struct crystalhd_cmd c = {0};
    struct crystalhd_rx_metadata m = {.valid = true};
    const u64 timestamps[] = {0, UINT64_MAX, 0, 1234567890123456789ULL};
    const unsigned order[] = {2, 3, 1, 0};
    u64 timestamp = 12;
    unsigned before;

    crystalhd_v4l2_decoder_init(&d);
    assert(!crystalhd_v4l2_decoder_open_locked(&d, &c, &owner));
    assert(crystalhd_v4l2_decoder_submit_mapped(&d, &c, &owner, NULL, 0, 100) == -EINVAL);
    assert(d.count == 0 && d.next_token == 1);
    for (unsigned i = 0; i < 4; i++) {
        assert(!crystalhd_v4l2_decoder_submit_mapped(&d, &c, &owner,
            &mapped, timestamps[i], 100));
        assert(last_pts == (u64)(i + 1) << 1);
    }
    assert(d.count == 4 && mapped.transport_calls == 4);
    for (unsigned i = 0; i < 4; i++) {
        m.firmware_timestamp = order[i] + 1;
        assert(!crystalhd_v4l2_decoder_complete(&d, &m, d.epoch, &timestamp));
        assert(timestamp == timestamps[order[i]]);
    }
    /* Mixing transports still shares one bounded token namespace. */
    assert(!submit(&d, &c, 7) && last_pts == 10);
    for (unsigned i = 1; i < CRYSTALHD_V4L2_TIMESTAMPS; i++)
        assert(!crystalhd_v4l2_decoder_submit_mapped(&d, &c, &owner, &mapped, i, 100));
    before = mapped.transport_calls;
    assert(crystalhd_v4l2_decoder_submit_mapped(&d, &c, &owner, &mapped, 0, 100) == -EAGAIN);
    assert(mapped.transport_calls == before);
    restart(&d, &c);
    d.next_token = UINT32_MAX - 1;
    assert(!crystalhd_v4l2_decoder_submit_mapped(&d, &c, &owner, &mapped, 77, 100));
    assert(last_pts == 0x1fffffffcULL);
    assert(crystalhd_v4l2_decoder_submit_mapped(&d, &c, &owner, &mapped, 0, 100) == -EOVERFLOW);

    crystalhd_v4l2_decoder_init(&d);
    c.decoder_phase = CRYSTALHD_DECODER_COLD;
    assert(!crystalhd_v4l2_decoder_open_locked(&d, &c, &owner));
    fail_call = calls + 1;
    assert(crystalhd_v4l2_decoder_submit_mapped(&d, &c, &owner, &mapped, 0, 100) == -EIO);
    assert(d.phase == CHD_V4L2_FAILED && d.count == 1 && d.timestamps[0].token == 1);
    assert(d.timestamps[0].timestamp == 0);
    before = mapped.transport_calls;
    assert(crystalhd_v4l2_decoder_submit_mapped(&d, &c, &owner, &mapped, 1, 100) == -EPIPE);
    assert(mapped.transport_calls == before);
    fail_call = 0;
    restart(&d, &c);
    assert(!crystalhd_v4l2_decoder_submit_mapped(&d, &c, &owner, &mapped, 91, 100));
    assert(last_pts == 4 && d.count == 1);
}

static void asynchronous_checks(void)
{
    struct crystalhd_v4l2_decoder d;
    struct crystalhd_cmd c = {0};
    struct crystalhd_rx_metadata m = {.valid = true};
    u64 raw_pts = 77, timestamp = 99;
    int transport_result;

    crystalhd_v4l2_decoder_init(&d);
    assert(crystalhd_v4l2_decoder_reserve(&d, &c, &owner, 0, &raw_pts) == -EPIPE);
    assert(raw_pts == 77 && !d.count);
    assert(!crystalhd_v4l2_decoder_open_locked(&d, &c, &owner));
    assert(crystalhd_v4l2_decoder_reserve(&d, &c, &owner, 0, NULL) == -EINVAL);
    assert(!d.count);
    assert(!crystalhd_v4l2_decoder_reserve(&d, &c, &owner, 0, &raw_pts));
    assert(raw_pts == 2 && d.count == 1);
    transport_result = crystalhd_v4l2_output_submit(&c, &owner, &mapped, raw_pts, 100);
    /* RX can retire this frame before the frontend reacquires its state
     * lock to record the synchronous transport's success.
     */
    m.firmware_timestamp = 1;
    assert(!crystalhd_v4l2_decoder_complete(&d, &m, d.epoch, &timestamp));
    assert(!timestamp && !d.count);
    assert(!crystalhd_v4l2_decoder_submitted(&d, transport_result));
    assert(d.phase == CHD_V4L2_RUNNING);
    assert(!crystalhd_v4l2_decoder_reserve(&d, &c, &owner, UINT64_MAX, &raw_pts));
    assert(raw_pts == 4);
    assert(!crystalhd_v4l2_decoder_begin_drain(&d));
    assert(crystalhd_v4l2_decoder_begin_drain(&d) == -EPIPE);
    assert(crystalhd_v4l2_decoder_reserve(&d, &c, &owner, 4, &raw_pts) == -EPIPE);
    m.firmware_timestamp = 2;
    assert(!crystalhd_v4l2_decoder_complete(&d, &m, d.epoch, &timestamp));
    assert(timestamp == UINT64_MAX);
    early_eos = &d;
    transport_result = crystalhd_decoder_submit_h264_eos(&c, &owner, 100);
    assert(d.phase == CHD_V4L2_DRAINED);
    assert(!crystalhd_v4l2_decoder_submitted(&d, transport_result));
    assert(d.phase == CHD_V4L2_DRAINED);
    restart(&d, &c);
    /* The convenience path also arms drain before invoking transport. */
    assert(!crystalhd_v4l2_decoder_drain(&d, &c, &owner, 100));
    assert(d.phase == CHD_V4L2_DRAINED);
    restart(&d, &c);
    fail_call = calls + 1;
    assert(crystalhd_v4l2_decoder_drain(&d, &c, &owner, 100) == -EIO);
    assert(d.phase == CHD_V4L2_FAILED);
    assert(!crystalhd_v4l2_decoder_drained(&d));
    early_eos = NULL;
    fail_call = 0;
    restart(&d, &c);
    assert(!crystalhd_v4l2_decoder_reserve(&d, &c, &owner, 123, &raw_pts));
    assert(crystalhd_v4l2_decoder_submitted(&d, -ETIMEDOUT) == -ETIMEDOUT);
    assert(d.phase == CHD_V4L2_FAILED && d.count == 1);
}

static void failed_picture_checks(void)
{
    const struct crystalhd_rx_metadata eos = {
        .valid = true, .eos_trailer = true,
        .picture_number = UINT32_MAX, .picture_flags = CRYSTALHD_PICTURE_FLAG_EOS,
    };
    for (unsigned draining = 0; draining < 2; draining++) {
        struct crystalhd_v4l2_decoder d;
        struct crystalhd_cmd c = {0};
        struct crystalhd_rx_metadata m = {
            .valid = true, .picture_flags = CRYSTALHD_PICTURE_FLAG_DECODE_ERROR,
            .firmware_timestamp = 1, .picture_number = 22,
        };
        u64 timestamp = 77;
        crystalhd_v4l2_decoder_init(&d);
        fail_call = 0;
        assert(!crystalhd_v4l2_decoder_open_locked(&d, &c, &owner));
        assert(!submit(&d, &c, UINT64_C(0x1234567890)));
        assert(!submit(&d, &c, 999));
        if (draining) assert(!crystalhd_v4l2_decoder_begin_drain(&d));
        enum crystalhd_v4l2_decoder_phase phase = d.phase;
        if (draining) {
            struct crystalhd_v4l2_decoder saved = d;
            assert(crystalhd_v4l2_decoder_complete(&d, &eos, d.epoch, &timestamp) == -EPROTO);
            assert(!memcmp(&d, &saved, sizeof(d)) && timestamp == 77);
        }
        m.firmware_timestamp = 0;
        assert(crystalhd_v4l2_decoder_complete(&d, &m, d.epoch, &timestamp) == -ESTALE);
        m.firmware_timestamp = 99;
        assert(crystalhd_v4l2_decoder_complete(&d, &m, d.epoch, &timestamp) == -ESTALE);
        m.firmware_timestamp = 1;
        assert(crystalhd_v4l2_decoder_complete(&d, &m, d.epoch + 1, &timestamp) == -ESTALE);
        m.eos_trailer = true;
        assert(crystalhd_v4l2_decoder_complete(&d, &m, d.epoch, &timestamp) == -EPROTO);
        m.eos_trailer = false;
        m.picture_flags |= CRYSTALHD_PICTURE_FLAG_EOS;
        assert(crystalhd_v4l2_decoder_complete(&d, &m, d.epoch, &timestamp) == -EPROTO);
        assert(d.count == 2 && timestamp == 77 && d.phase == phase);
        m.picture_flags = CRYSTALHD_PICTURE_FLAG_DECODE_ERROR;
        assert(crystalhd_v4l2_decoder_complete(&d, &m, d.epoch, &timestamp) == -EILSEQ);
        assert(timestamp == UINT64_C(0x1234567890) && d.count == 1 && d.phase == phase);
        assert(crystalhd_v4l2_decoder_complete(&d, &m, d.epoch, &timestamp) == -ESTALE);
        if (draining) {
            struct crystalhd_v4l2_decoder saved = d;
            assert(crystalhd_v4l2_decoder_complete(&d, &eos, d.epoch, &timestamp) == -EPROTO);
            assert(!memcmp(&d, &saved, sizeof(d)));
            assert(timestamp == UINT64_C(0x1234567890));
        }
        m.firmware_timestamp = 2; m.picture_flags = 0;
        assert(!crystalhd_v4l2_decoder_complete(&d, &m, d.epoch, &timestamp));
        assert(timestamp == 999 && !d.count && d.phase == phase);
        if (!draining) assert(!crystalhd_v4l2_decoder_begin_drain(&d));
        assert(crystalhd_v4l2_decoder_complete(&d, &eos, d.epoch, &timestamp) == 1);
        assert(d.phase == CHD_V4L2_DRAINED && !d.count);
        assert(crystalhd_v4l2_decoder_complete(&d, &eos, d.epoch, &timestamp) == -EPIPE);
    }
}

int main(void)
{
    struct crystalhd_v4l2_decoder d;
    struct crystalhd_cmd c = {0};
    struct crystalhd_rx_metadata m = {.valid = true};
    const u64 stamps[] = {0, UINT64_MAX, 0, 1234567890123456789ULL};
    const unsigned order[] = {3, 0, 2, 1};
    u64 out = 42, old_epoch;
    u32 token;
    unsigned before;

    crystalhd_v4l2_decoder_init(&d);
    assert(!crystalhd_v4l2_decoder_open_locked(&d, &c, &owner));
    assert(bootstrap_calls == 1 && calls == 3);
    m.valid = false;
    assert(crystalhd_v4l2_decoder_complete(&d, &m, d.epoch, &out) == -ENODATA);
    m.valid = true;
    m.firmware_timestamp = 0x100000001ULL;
    assert(crystalhd_v4l2_decoder_complete(&d, &m, d.epoch, &out) == -ESTALE);
    assert(out == 42);
    for (unsigned i = 0; i < 4; i++) {
        assert(!submit(&d, &c, stamps[i]));
        assert(last_pts == ((u64)i + 1) * 2);
    }
    assert(d.count == 4); /* TX completion must not consume mapping. */
    for (unsigned i = 0; i < 4; i++) {
        m.firmware_timestamp = order[i] + 1;
        assert(!crystalhd_v4l2_decoder_complete(&d, &m, d.epoch, &out));
        assert(out == stamps[order[i]]);
        assert(crystalhd_v4l2_decoder_complete(&d, &m, d.epoch, &out) == -ESTALE);
    }
    assert(!d.count);
    for (unsigned i = 0; i < CRYSTALHD_V4L2_TIMESTAMPS; i++)
        assert(!submit(&d, &c, i));
    before = calls;
    assert(submit(&d, &c, 1) == -EAGAIN && calls == before);
    m.firmware_timestamp = 6;
    assert(!crystalhd_v4l2_decoder_complete(&d, &m, d.epoch, &out) && out == 1);
    assert(!submit(&d, &c, 99));
    assert(!crystalhd_v4l2_decoder_drain(&d, &c, &owner, 100));
    assert(!crystalhd_v4l2_decoder_drained(&d));
    assert(submit(&d, &c, 0) == -EPIPE);
    m.firmware_timestamp = 5; /* Delayed picture after EOS TX. */
    assert(!crystalhd_v4l2_decoder_complete(&d, &m, d.epoch, &out) && out == 0);
    m.eos_trailer = true;
    m.picture_number = UINT32_MAX;
    assert(crystalhd_v4l2_decoder_complete(&d, &m, d.epoch, &out) == -ENODATA);
    m.picture_flags = VDEC_FLAG_EOS;
    m.pib_line = 1;
    assert(crystalhd_v4l2_decoder_complete(&d, &m, d.epoch, &out) == -ENODATA);
    m.pib_line = 0;
    assert(crystalhd_v4l2_decoder_complete(&d, &m, d.epoch - 1, &out) == -ESTALE);
    {
        struct crystalhd_v4l2_decoder saved = d;
        assert(crystalhd_v4l2_decoder_complete(&d, &m, d.epoch, &out) == -EPROTO);
        assert(!memcmp(&d, &saved, sizeof(d)) && out == 0);
        /* Retire actual occupied slots; EOS must never erase filler tokens. */
        for (unsigned i = 0; i < CRYSTALHD_V4L2_TIMESTAMPS; i++) {
            struct crystalhd_rx_metadata picture = {.valid = true};
            u64 expected = d.timestamps[i].timestamp;
            picture.firmware_timestamp = d.timestamps[i].token;
            if (!picture.firmware_timestamp) continue;
            assert(!crystalhd_v4l2_decoder_complete(&d, &picture, d.epoch, &out));
            assert(out == expected);
        }
        assert(!d.count);
    }
    assert(crystalhd_v4l2_decoder_complete(&d, &m, d.epoch, &out) == 1);
    assert(crystalhd_v4l2_decoder_drained(&d) && !d.count);
    old_epoch = d.epoch;
    token = d.next_token;
    restart(&d, &c);
    assert(d.next_token == token);
    assert(crystalhd_v4l2_decoder_complete(&d, &m, old_epoch, &out) == -ESTALE);
    assert(crystalhd_v4l2_decoder_complete(&d, &m, d.epoch, &out) == -ENODATA);
    m.eos_trailer = false;
    m.firmware_timestamp = 5;
    assert(crystalhd_v4l2_decoder_complete(&d, &m, d.epoch, &out) == -ESTALE);
    d.next_token = UINT32_MAX - 1;
    assert(!submit(&d, &c, 77) && last_pts == 0x1fffffffcULL);
    assert(submit(&d, &c, 88) == -EOVERFLOW);
    restart(&d, &c);
    assert(submit(&d, &c, 88) == -EOVERFLOW);

    /* Each firmware transition failure prevents subsequent input. */
    for (unsigned n = 1; n <= 3; n++) {
        crystalhd_v4l2_decoder_init(&d);
        c.decoder_phase = CRYSTALHD_DECODER_COLD;
        fail_call = calls + n;
        assert(crystalhd_v4l2_decoder_open_locked(&d, &c, &owner) == -EIO);
        assert(d.phase == CHD_V4L2_FAILED && submit(&d, &c, 0) == -EPIPE);
    }
    fail_call = 0;
    crystalhd_v4l2_decoder_init(&d);
    c.decoder_phase = CRYSTALHD_DECODER_COLD;
    assert(!crystalhd_v4l2_decoder_open_locked(&d, &c, &owner));
    fail_call = calls + 1;
    assert(submit(&d, &c, 3) == -EIO && d.count == 1);
    assert(d.phase == CHD_V4L2_FAILED);
    fail_call = 0;
    restart(&d, &c);
    fail_call = calls + 1;
    assert(crystalhd_v4l2_decoder_drain(&d, &c, &owner, 100) == -EIO);
    assert(!crystalhd_v4l2_decoder_drained(&d));
    fail_call = 0;
    restart(&d, &c);
    old_epoch = d.epoch;
    fail_call = calls + 1;
    assert(crystalhd_v4l2_decoder_stop_locked(&d, &c, &owner) == -EIO);
    assert(d.phase == CHD_V4L2_FAILED && d.epoch == old_epoch);
    fail_call = 0;
    assert(!crystalhd_v4l2_decoder_stop_locked(&d, &c, &owner));
    fail_call = calls + 1;
    assert(crystalhd_v4l2_decoder_close_locked(&d, &c, &owner) == -EIO);
    assert(d.phase == CHD_V4L2_FAILED && d.epoch == old_epoch);
    fail_call = 0;
    assert(!crystalhd_v4l2_decoder_close_locked(&d, &c, &owner));
    assert(d.epoch == old_epoch + 1);
    assert(!crystalhd_v4l2_decoder_open_locked(&d, &c, &owner));
    d.epoch = UINT64_MAX;
    crystalhd_v4l2_decoder_reset(&d);
    assert(d.phase == CHD_V4L2_FAILED);
    mapped_checks();
    asynchronous_checks();
    failed_picture_checks();
    puts("V4L2 decoder state, timestamp correlation and genuine drain checks passed");
    return 0;
}
