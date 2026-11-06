/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "crystalhd_h264_range_binding.h"
#include "fixtures/issue538/h264-poc1-multislice-prefixes-20261106.h"

static unsigned char au[4096], rbsp[128], prefix[192];
static unsigned int bytes, bit, checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr,"POC1 line %d: %s\n",__LINE__,#x); return 1; } } while (0)
static void add(const unsigned char *nal, unsigned int size)
{
    assert(bytes + size + 3 < sizeof(au));
    au[bytes++] = 0; au[bytes++] = 0; au[bytes++] = 1;
    memcpy(au + bytes, nal, size); bytes += size;
}
static unsigned int hex_digit(char c)
{
    assert((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
    return c <= '9' ? (unsigned int)(c - '0') : (unsigned int)(c - 'a' + 10);
}
static void add_hex(const char *text)
{
    unsigned char nal[128];
    const unsigned int size = (unsigned int)strlen(text) / 2;
    assert(strlen(text) == size * 2 && size <= sizeof(nal));
    for (unsigned int i = 0; i < size; i++)
        nal[i] = (unsigned char)((hex_digit(text[i * 2]) << 4) | hex_digit(text[i * 2 + 1]));
    add(nal, size);
}
static void config(unsigned int mode, bool full)
{
    bytes = 0;
    add_hex(poc1_fixtures[mode].sps[full]);
    add_hex(poc1_fixtures[mode].pps);
}
static void real_slices(unsigned int mode, unsigned int count)
{
    assert(count == 2 || count == 4);
    for (unsigned int i = 0; i < count; i++)
        add(poc1_fixtures[mode].vcl[i * (4 / count)], 8);
}
/* These independent header-only controls isolate signed keys and limits.
 * They are never treated as complete entropy-coded pictures or hardware input.
 */
static void put(uint64_t value, unsigned int count)
{
    assert(count <= 64 && bit + count < sizeof(rbsp) * 8);
    while (count--) {
        rbsp[bit / 8] |= (unsigned char)(((value >> count) & 1) << (7 - bit % 8));
        bit++;
    }
}
static void ue(uint64_t value)
{
    const uint64_t code = value + 1;
    unsigned int width = 0;
    assert(code);
    for (uint64_t n = code; n >>= 1;) width++;
    put(0, width); put(code, width + 1);
}
static void se(int64_t value)
{
    ue(value > 0 ? (uint64_t)value * 2 - 1 : (uint64_t)(-value) * 2);
}
static unsigned int synthetic(unsigned int first, int64_t delta0, int64_t delta1,
                              bool write0, bool write1)
{
    unsigned int used = 1, zeros = 0;
    memset(rbsp, 0, sizeof(rbsp)); bit = 0;
    ue(first); ue(2); ue(0); put(2, 4);
    if (write0) se(delta0);
    if (write1) se(delta1);
    put(1, 1); prefix[0] = 0x21;
    for (unsigned int i = 0; i < (bit + 7) / 8; i++) {
        if (zeros == 2 && rbsp[i] <= 3) { prefix[used++] = 3; zeros = 0; }
        prefix[used++] = rbsp[i]; zeros = rbsp[i] ? 0 : zeros + 1;
    }
    return used;
}
static void synthetic_pair(int64_t delta0, int64_t delta1, bool write0, bool write1)
{
    unsigned int size;
    bytes = 0;
    size = synthetic(0, delta0, delta1, write0, write1); add(prefix, size);
    size = synthetic(48, delta0, delta1, write0, write1); add(prefix, size);
}

static int actual_goldens(void)
{
    for (unsigned int mode = 0; mode < 3; mode++) {
        for (unsigned int full = 0; full < 2; full++) {
            for (unsigned int count = 2; count <= 4; count += 2) {
                struct chd_h264_range_state state = {0};
                struct chd_h264_range_snapshot old, next;
                struct chd_h264_range_picture a, b;
                config(mode, full); real_slices(mode, count);
                chd_h264_observe_range(&state, au, bytes, &old);
                /* Literal old guard must fail here on independently decoded input. */
                CHECK(old.associated && old.range_present && old.full_range == (bool)full && old.sps_generation == 1);
                CHECK(state.sps[0].value.poc_type == 1 && state.pps[0].picture_qualified &&
                      state.pps[0].bottom_field_poc_present == (mode != 1));
                CHECK(state.sps[0].value.delta_pic_order_always_zero == (mode == 0));
                CHECK(chd_h264_range_picture_prefix(&state, poc1_fixtures[mode].vcl[0], 8, &a));
                CHECK(chd_h264_range_picture_prefix(&state, poc1_fixtures[mode].vcl[2], 8, &b));
                CHECK(a.first_mb == 0 && b.first_mb == 48 && a.frame_num == 2 && b.frame_num == 2 &&
                      chd_h264_range_same_picture(&a, &b));
                CHECK(a.delta_pic_order_cnt[0] == (mode ? -1 : 0) &&
                      a.delta_pic_order_cnt[1] == (mode == 2 ? 1 : 0));
                config(mode, !full); real_slices(mode, count);
                chd_h264_observe_range(&state, au, bytes, &next);
                CHECK(next.associated && next.full_range != (bool)full && next.sps_generation == 2 &&
                      old.full_range == (bool)full && old.sps_generation == 1);
            }
        }
    }
    return 0;
}
static int actual_delta_mismatches(void)
{
    const unsigned char *wrong[] = {poc1_top_delta0_mismatch,
                                   poc1_both_delta0_mismatch, poc1_both_delta1_mismatch};
    for (unsigned int i = 0; i < 3; i++) {
        const unsigned int mode = i ? 2 : 1;
        struct chd_h264_range_state state = {0};
        struct chd_h264_range_snapshot snapshot;
        struct chd_h264_range_picture a, b;
        config(mode, true);
        add(poc1_fixtures[mode].vcl[0], 8); add(wrong[i], 8);
        chd_h264_observe_range(&state, au, bytes, &snapshot);
        CHECK(!snapshot.associated && state.pps[0].picture_qualified);
        CHECK(chd_h264_range_picture_prefix(&state, poc1_fixtures[mode].vcl[0], 8, &a));
        CHECK(chd_h264_range_picture_prefix(&state, wrong[i], 8, &b));
        CHECK(a.frame_num == b.frame_num && a.pps_id == b.pps_id &&
              a.idr == b.idr && a.reference == b.reference &&
              !chd_h264_range_same_picture(&a, &b));
    }
    return 0;
}
static int conditional_and_limits(void)
{
    struct chd_h264_range_state state = {0};
    struct chd_h264_range_snapshot snapshot;
    struct chd_h264_range_picture a, saved;
    const int64_t values[] = {0, INT32_MAX, -INT64_C(2147483647)};
    unsigned int size;
    for (unsigned int mode = 1; mode <= 2; mode++) {
        config(mode, true);
        chd_h264_observe_range(&state, au, bytes, &snapshot);
        for (unsigned int i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
            synthetic_pair(values[i], values[2 - i], true, mode == 2);
            chd_h264_observe_range(&state, au, bytes, &snapshot);
            CHECK(snapshot.associated && snapshot.full_range);
            size = synthetic(48, values[i], values[2 - i], true, mode == 2);
            CHECK(chd_h264_range_picture_prefix(&state, prefix, size, &a));
            CHECK(a.delta_pic_order_cnt[0] == values[i] &&
                  a.delta_pic_order_cnt[1] == (mode == 2 ? values[2 - i] : 0));
            /* Change each present key independently while retaining frame_num. */
            for (unsigned int key = 0; key < mode; key++) {
                bytes = 0;
                size = synthetic(0, values[i], values[2 - i], true, mode == 2); add(prefix, size);
                size = synthetic(48, key ? values[i] : 17,
                                 key ? 17 : values[2 - i], true, mode == 2); add(prefix, size);
                chd_h264_observe_range(&state, au, bytes, &snapshot);
                CHECK(!snapshot.associated);
            }
        }
    }
    /* Always-zero suppresses BOTH deltas despite PPS bottom=1. */
    config(0, true); chd_h264_observe_range(&state, au, bytes, &snapshot);
    bytes = 0;
    size = synthetic(0, 19, 7, true, true); add(prefix, size);
    size = synthetic(48, -19, -7, true, true); add(prefix, size);
    chd_h264_observe_range(&state, au, bytes, &snapshot);
    CHECK(snapshot.associated && snapshot.full_range);
    /* With PPS bottom=0 only delta0 is a key; later SE-looking bits are payload. */
    config(1, true); chd_h264_observe_range(&state, au, bytes, &snapshot);
    bytes = 0;
    size = synthetic(0, 19, 7, true, true); add(prefix, size);
    size = synthetic(48, 19, -7, true, true); add(prefix, size);
    chd_h264_observe_range(&state, au, bytes, &snapshot);
    CHECK(snapshot.associated && snapshot.full_range);
    config(2, true); chd_h264_observe_range(&state, au, bytes, &snapshot);
    /* Signed values outside [INT32_MIN+1,INT32_MAX] require forbidden >31-zero UEs. */
    for (unsigned int key = 0; key < 2; key++) {
        for (unsigned int negative = 0; negative < 2; negative++) {
            const int64_t invalid = negative ? -INT64_C(2147483648) : INT64_C(2147483648);
            size = synthetic(48, key ? 0 : invalid, key ? invalid : 0, true, true);
            memset(&saved, 0, sizeof(saved)); saved.frame_num = 1234; a = saved;
            CHECK(!chd_h264_range_picture_prefix(&state, prefix, size, &a) &&
                  !memcmp(&a, &saved, sizeof(a)));
        }
    }
    size = synthetic(48, INT32_MAX, -INT64_C(2147483647), true, true);
    CHECK(size > 9);
    for (unsigned int n = 0; n <= 8; n++) {
        memset(&saved, 0, sizeof(saved)); saved.frame_num = 1234; a = saved;
        CHECK(!chd_h264_range_picture_prefix(&state, prefix, n, &a) &&
              !memcmp(&a, &saved, sizeof(a)));
    }
    CHECK(chd_h264_range_picture_prefix(&state, prefix, size, &a));
    return 0;
}
int main(void)
{
    CHECK(!actual_goldens());
    CHECK(!actual_delta_mismatches());
    CHECK(!conditional_and_limits());
    printf("H264 progressive POC1 multi-slice association: %u checks PASS\n", checks);
    return 0;
}
