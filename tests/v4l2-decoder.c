// SPDX-License-Identifier: GPL-2.0-or-later
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include "crystalhd_h264_range_binding.h"
#include "fixtures/issue538/h264-range-nals-20261102.h"
#include "fixtures/issue538/h264-multislice-prefixes-20261102.h"
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
struct crystalhd_v4l2_output_buffer { unsigned transport_calls; const u8 *source; size_t bytes; int source_error; };
static unsigned source_calls;
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
static int crystalhd_v4l2_output_source(struct crystalhd_v4l2_output_buffer *buffer,
    const u8 **data, size_t *bytes)
{
    static const u8 unknown[] = {0,0,1,0x65};
    source_calls++;
    if (buffer->source_error) return buffer->source_error;
    *data = buffer->source ? buffer->source : unknown;
    *bytes = buffer->source ? buffer->bytes : sizeof(unknown);
    return 0;
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

static void observation_checks(void)
{
    static const u8 limited[] = CHD_TEST_SPS_LIMITED;
    static const u8 full[] = CHD_TEST_SPS_FULL;
    static const u8 pps[] = CHD_TEST_PPS;
    static const u8 vcl[] = CHD_TEST_VCL;
    struct crystalhd_v4l2_decoder d, before;
    struct crystalhd_cmd c = {0};
    struct crystalhd_rx_metadata m = {.valid=true};
    struct chd_h264_range_snapshot out, saved;
    u8 input[128]; size_t n;
    u64 timestamp, pts = 0, old_epoch;
    unsigned old_sources;
    fail_call=0; crystalhd_v4l2_decoder_init(&d);
    assert(!crystalhd_v4l2_decoder_open_locked(&d,&c,&owner));
#define RANGE_AU(sps) do { \
    n=0; memcpy(input+n,"\0\0\1",3);n+=3;memcpy(input+n,sps,sizeof(sps));n+=sizeof(sps); \
    memcpy(input+n,"\0\0\0\1",4);n+=4;memcpy(input+n,pps,sizeof(pps));n+=sizeof(pps); \
    memcpy(input+n,"\0\0\1",3);n+=3;memcpy(input+n,vcl,sizeof(vcl));n+=sizeof(vcl); \
} while(0)
    RANGE_AU(limited);
    assert(!crystalhd_v4l2_decoder_submit(&d,&c,&owner,input,n,0,100));
    assert(d.timestamps[0].input_range.associated && !d.timestamps[0].input_range.full_range &&
           d.timestamps[0].input_range.sps_generation==1);
    saved=d.timestamps[0].input_range;
    RANGE_AU(full);mapped.source=input;mapped.bytes=n;
    assert(!crystalhd_v4l2_decoder_submit_mapped(&d,&c,&owner,&mapped,0,100));
    assert(d.timestamps[1].input_range.associated && d.timestamps[1].input_range.full_range &&
           d.timestamps[1].input_range.sps_generation==2);
    assert(!memcmp(&saved,&d.timestamps[0].input_range,sizeof(saved)));
    RANGE_AU(limited);
    assert(!crystalhd_v4l2_decoder_submit(&d,&c,&owner,input,n,UINT64_MAX,100));
    assert(d.timestamps[2].input_range.sps_generation==3);
    /* Format provenance peeks an exact admission, including an older SPS
     * generation, without releasing tokens or changing parser state.
     */
    before=d;
    for (unsigned k=0;k<3;k++) {
        assert(!crystalhd_v4l2_decoder_peek_range(&d,d.epoch,k+1,&out));
        assert(out.associated && out.range_present && out.sps_generation==k+1 &&
               out.full_range==(k==1) && !memcmp(&before,&d,sizeof(d)));
    }
    for (unsigned k=0;k<4;k++) {
        static const u64 bad[]={0,4,0xffffffffULL,UINT64_MAX};
        out=saved;
        assert(crystalhd_v4l2_decoder_peek_range(&d,d.epoch,bad[k],&out)==-ESTALE);
        assert(!memcmp(&out,&saved,sizeof(out)) && !memcmp(&before,&d,sizeof(d)));
    }
    assert(crystalhd_v4l2_decoder_peek_range(&d,d.epoch-1,1,&out)==-ESTALE);
    assert(!memcmp(&before,&d,sizeof(d)));
    for(unsigned i=0;i<3;i++) {
        static const unsigned order[]={2,0,1}; unsigned k=order[i];
        m.firmware_timestamp=k+1;
        assert(!crystalhd_v4l2_decoder_complete_observed(&d,&m,d.epoch,&timestamp,&out));
        assert(timestamp==(k==2?UINT64_MAX:0) && out.associated && out.range_present &&
               out.sps_generation==k+1 && out.full_range==(k==1));
        out=saved;
        assert(crystalhd_v4l2_decoder_peek_range(&d,d.epoch,k+1,&out)==-ESTALE);
        assert(!memcmp(&out,&saved,sizeof(out)));
    }
    assert(!d.count);
    /* A full queue must not observe newer headers or even read mapped bytes. */
    for(unsigned i=0;i<CRYSTALHD_V4L2_TIMESTAMPS;i++)
        assert(!crystalhd_v4l2_decoder_reserve_au(&d,&c,&owner,
            (const u8[]){0,0,1,0x65,0x88,0x84},6,i,&pts));
    memcpy(&before,&d,sizeof(d)); RANGE_AU(full);pts=123;
    assert(crystalhd_v4l2_decoder_reserve_au(&d,&c,&owner,input,n,999,&pts)==-EAGAIN);
    assert(pts==123 && !memcmp(&before,&d,sizeof(d)));
    old_sources=source_calls;mapped.source=input;mapped.bytes=n;
    assert(crystalhd_v4l2_decoder_reserve_mapped(&d,&c,&owner,&mapped,999,&pts)==-EAGAIN);
    assert(source_calls==old_sources && !memcmp(&before,&d,sizeof(d)));
    old_epoch=d.epoch;restart(&d,&c);
    assert(!d.input_parameters.generation && !d.input_parameters.pps[0].valid);
    /* State reset cannot revive old parameters or reuse old token/epoch. */
    assert(!crystalhd_v4l2_decoder_reserve_au(&d,&c,&owner,
        (const u8[]){0,0,1,0x65,0x88,0x84},6,77,&pts));
    m.firmware_timestamp=pts>>1;timestamp=55;
    out=(struct chd_h264_range_snapshot){.associated=true,.sps_generation=999};saved=out;
    assert(crystalhd_v4l2_decoder_complete_observed(&d,&m,old_epoch,&timestamp,&out)==-ESTALE);
    assert(timestamp==55 && !memcmp(&out,&saved,sizeof(out)) && d.count==1);
    assert(!crystalhd_v4l2_decoder_complete_observed(&d,&m,d.epoch,&timestamp,&out));
    assert(timestamp==77 && !out.associated && !d.count);
    RANGE_AU(full);mapped.source=input;mapped.bytes=n;mapped.source_error=-EIO;
    memcpy(&before,&d,sizeof(d));pts=543;
    assert(crystalhd_v4l2_decoder_reserve_mapped(&d,&c,&owner,&mapped,3,&pts)==-EIO);
    assert(pts==543 && !memcmp(&before,&d,sizeof(d)));mapped.source_error=0;
    /* Row observation exists before RX can race a still-running TX. */
    assert(!crystalhd_v4l2_decoder_reserve_mapped(&d,&c,&owner,&mapped,14,&pts));
    m.firmware_timestamp=pts>>1;
    assert(!crystalhd_v4l2_decoder_complete_observed(&d,&m,d.epoch,&timestamp,&out));
    assert(timestamp==14 && out.associated && out.full_range && out.sps_generation==1);
    assert(!crystalhd_v4l2_decoder_submitted(&d,0) && d.phase==CHD_V4L2_RUNNING);
    assert(!crystalhd_v4l2_decoder_reserve_au(&d,&c,&owner,input,n,89,&pts));
    m.firmware_timestamp=pts>>1;m.picture_flags=CRYSTALHD_PICTURE_FLAG_DECODE_ERROR;
    assert(crystalhd_v4l2_decoder_complete_observed(&d,&m,d.epoch,&timestamp,&out)==-EILSEQ);
    assert(timestamp==89 && out.full_range && out.sps_generation==2 && !d.count);
    /* Generation exhaustion degrades only metadata; older rows stay immutable. */
    m.picture_flags=0;
    assert(!crystalhd_v4l2_decoder_reserve_au(&d,&c,&owner,input,n,90,&pts));
    u64 token=pts>>1; saved=d.timestamps[0].input_range;
    d.input_parameters.generation=UINT64_MAX;
    assert(!crystalhd_v4l2_decoder_reserve_au(&d,&c,&owner,input,n,91,&pts));
    assert(d.input_parameters.exhausted && !d.timestamps[1].input_range.associated);
    m.firmware_timestamp=token;
    assert(!crystalhd_v4l2_decoder_complete_observed(&d,&m,d.epoch,&timestamp,&out));
    assert(out.associated && out.sps_generation==saved.sps_generation && timestamp==90);
    m.firmware_timestamp=pts>>1;
    assert(!crystalhd_v4l2_decoder_complete_observed(&d,&m,d.epoch,&timestamp,&out));
    assert(!out.associated && timestamp==91);
    /* Any transport error retains the already-reserved input observation. */
    restart(&d,&c); RANGE_AU(limited);fail_call=calls+1;
    assert(crystalhd_v4l2_decoder_submit(&d,&c,&owner,input,n,123,100)==-EIO);
    assert(d.phase==CHD_V4L2_FAILED && d.count==1 && d.timestamps[0].input_range.associated &&
           !d.timestamps[0].input_range.full_range);
    assert(crystalhd_v4l2_decoder_peek_range(&d,d.epoch,d.timestamps[0].token,&out)==-EPIPE);
    out=(struct chd_h264_range_snapshot){.sps_generation=777};saved=out;timestamp=9;
    m.firmware_timestamp=d.timestamps[0].token;
    assert(crystalhd_v4l2_decoder_complete_observed(&d,&m,d.epoch,&timestamp,&out)==-EPIPE);
    assert(timestamp==9 && !memcmp(&out,&saved,sizeof(out)) && d.count==1);
    fail_call=0;restart(&d,&c);
    assert(!d.count && !d.input_parameters.pps[0].valid);
    assert(!crystalhd_v4l2_decoder_begin_drain(&d));
    m.eos_trailer=true;m.picture_number=UINT32_MAX;m.picture_flags=CRYSTALHD_PICTURE_FLAG_EOS;
    assert(crystalhd_v4l2_decoder_complete_observed(&d,&m,d.epoch,&timestamp,&out)==1);
    assert(timestamp==9 && !memcmp(&out,&saved,sizeof(out)));
    mapped=(struct crystalhd_v4l2_output_buffer){0};
    printf("V4L2 immutable input observations: redefinition/reordering/duplicate-PTS/EAGAIN/reset/TX-error/RX-before-return/error/EOS PASS; decoder=%zu bytes\n",sizeof(d));
#undef RANGE_AU
}

static void multislice_observation_checks(void)
{
    static const u8 limited[] = CHD_TEST_MULTI_SPS_LIMITED;
    static const u8 full[] = CHD_TEST_MULTI2_SPS;
    static const u8 pps[] = CHD_TEST_MULTI2_PPS;
    static const u8 first[] = CHD_TEST_MULTI2_VCL0;
    static const u8 next[] = CHD_TEST_MULTI2_VCL1;
    struct crystalhd_v4l2_decoder d;
    struct crystalhd_cmd c = {0};
    struct crystalhd_rx_metadata m = {.valid=true};
    struct chd_h264_range_snapshot out, saved;
    u64 pts[2], timestamp = 42, old_epoch;
    u8 input[128]; size_t n;
    fail_call=0;crystalhd_v4l2_decoder_init(&d);
    assert(!crystalhd_v4l2_decoder_open_locked(&d,&c,&owner));
#define MULTI_ADD(nal) do { memcpy(input+n,"\0\0\1",3);n+=3; \
    memcpy(input+n,nal,sizeof(nal));n+=sizeof(nal); } while (0)
    for (unsigned i=0;i<2;i++) {
        n=0;if(i) { MULTI_ADD(full); } else { MULTI_ADD(limited); }
        MULTI_ADD(pps);MULTI_ADD(first);MULTI_ADD(next);
        mapped.source=input;mapped.bytes=n;
        assert(!crystalhd_v4l2_decoder_reserve_mapped(&d,&c,&owner,&mapped,
            i?UINT64_MAX:0,&pts[i]));
        assert(d.timestamps[i].input_range.associated && d.timestamps[i].input_range.range_present &&
               d.timestamps[i].input_range.full_range==!!i &&
               d.timestamps[i].input_range.sps_generation==i+1);
        if(!i)saved=d.timestamps[0].input_range;
    }
    assert(!memcmp(&saved,&d.timestamps[0].input_range,sizeof(saved)) && d.count==2);
    for(unsigned i=0;i<2;i++) {
        unsigned index=1-i;m.firmware_timestamp=pts[index]>>1;
        assert(!crystalhd_v4l2_decoder_complete_observed(&d,&m,d.epoch,&timestamp,&out));
        assert(timestamp==(index?UINT64_MAX:0) && out.associated && out.full_range==!!index &&
               out.sps_generation==index+1);
    }
    assert(!d.count);old_epoch=d.epoch;restart(&d,&c);
    n=0;MULTI_ADD(first);MULTI_ADD(next);mapped.source=input;mapped.bytes=n;
    assert(!crystalhd_v4l2_decoder_reserve_mapped(&d,&c,&owner,&mapped,31,&pts[0]));
    assert(!d.timestamps[0].input_range.associated && !d.input_parameters.pps[0].valid);
    m.firmware_timestamp=pts[0]>>1;saved=out;timestamp=77;
    assert(crystalhd_v4l2_decoder_complete_observed(&d,&m,old_epoch,&timestamp,&out)==-ESTALE);
    assert(timestamp==77 && !memcmp(&saved,&out,sizeof(out)) && d.count==1);
    assert(!crystalhd_v4l2_decoder_complete_observed(&d,&m,d.epoch,&timestamp,&out));
    assert(timestamp==31 && !out.associated && !d.count);
    mapped=(struct crystalhd_v4l2_output_buffer){0};
    puts("V4L2 multi-slice input observations: immutable redefinition/reordered completion/reset PASS");
#undef MULTI_ADD
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
    observation_checks();
    mapped_checks();
    asynchronous_checks();
    failed_picture_checks();
    multislice_observation_checks();
    puts("V4L2 decoder state, timestamp correlation and genuine drain checks passed");
    return 0;
}
