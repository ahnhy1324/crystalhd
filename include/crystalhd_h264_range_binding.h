/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CRYSTALHD_H264_RANGE_BINDING_H
#define CRYSTALHD_H264_RANGE_BINDING_H
#include "crystalhd_h264_sps_range.h"

#define CHD_H264_RANGE_MAX_AU (8U * 1024U * 1024U)
#define CHD_H264_RANGE_MAX_NALS 128U

/* An input-prefix observation, not proof of firmware parameter adoption or
 * decoded pixels. Generations are local to the owning decoder epoch. */
struct chd_h264_range_snapshot {
	unsigned long long sps_generation;
	unsigned int sps_id, pps_id;
	bool associated, range_present, full_range;
};
struct chd_h264_range_state {
	struct {
		struct chd_h264_sps_range value;
		unsigned long long generation;
		bool valid;
	} sps[32];
	struct {
		unsigned int sps_id;
		bool valid, picture_qualified, bottom_field_poc_present;
	} pps[256];
	unsigned long long generation;
	bool exhausted;
};

static inline void chd_h264_range_forget(struct chd_h264_range_state *s)
{
	unsigned int i;
	for (i = 0; i < 32; i++) s->sps[i].valid = false;
	for (i = 0; i < 256; i++) s->pps[i].valid = false;
}
static inline bool chd_h264_pps_prefix(const unsigned char *nal, unsigned int bytes,
	unsigned int *pps, unsigned int *sps)
{
	struct chd_h264_range_bits b = {nal, bytes, 1, 0, 0, 0, false};
	unsigned int p, q;
	if (!nal || !pps || !sps || bytes < 2 || bytes > CHD_H264_SPS_MAX_BYTES || (nal[0] & 0x80) ||
	    !(nal[0] & 0x60) || (nal[0] & 31) != 8 ||
	    !chd_h264_range_ebsp(nal, bytes)) return false;
	p = chd_h264_range_ue(&b); q = chd_h264_range_ue(&b);
	if (b.failed || p > 255 || q > 31) return false;
	*pps = p; *sps = q; return true;
}
static inline bool chd_h264_slice_prefix(const unsigned char *nal, unsigned int bytes,
	unsigned int *pps)
{
	/* Only consume the first three UEs, not a multi-megabyte coded slice. */
	struct chd_h264_range_bits b = {nal, bytes < 128 ? bytes : 128, 1, 0, 0, 0, false};
	unsigned int first, type, p;
	if (!nal || !pps || bytes < 2 || (nal[0] & 0x80) ||
	    ((nal[0] & 31) != 1 && (nal[0] & 31) != 5) ||
	    ((nal[0] & 31) == 5 && !(nal[0] & 0x60))) return false;
	first = chd_h264_range_ue(&b); type = chd_h264_range_ue(&b);
	p = chd_h264_range_ue(&b);
	if (b.failed || first || type > 9 || p > 255) return false;
	*pps = p; return true;
}

/* Qualify a single slice group without redundant pictures. Reading through
 * redundant_pic_cnt_present_flag is necessary: the two ID UEs alone do not
 * distinguish these supported picture headers from FMO/redundant syntax.
 * The caller has already validated the PPS header/EBSP with pps_prefix().
 */
static inline bool chd_h264_pps_picture_prefix(const unsigned char *nal,
	unsigned int bytes, bool *bottom_field_poc_present)
{
	struct chd_h264_range_bits b = {nal, bytes, 1, 0, 0, 0, false};
	int value;
	bool bottom;
	*bottom_field_poc_present = false;
	chd_h264_range_ue(&b); chd_h264_range_ue(&b);
	chd_h264_range_get(&b, 1); /* entropy_coding_mode_flag */
	bottom = chd_h264_range_get(&b, 1) != 0;
	if (chd_h264_range_ue(&b)) return false; /* num_slice_groups_minus1 */
	if (chd_h264_range_ue(&b) > 31 || chd_h264_range_ue(&b) > 31) return false;
	chd_h264_range_get(&b, 1);
	if (chd_h264_range_get(&b, 2) > 2) return false;
	value = chd_h264_range_se(&b);
	if (value < -26 || value > 25) return false;
	value = chd_h264_range_se(&b);
	if (value < -26 || value > 25) return false;
	value = chd_h264_range_se(&b);
	if (value < -12 || value > 12) return false;
	chd_h264_range_get(&b, 1); chd_h264_range_get(&b, 1);
	if (chd_h264_range_get(&b, 1) || b.failed) return false;
	*bottom_field_poc_present = bottom;
	return true;
}

struct chd_h264_range_picture {
	unsigned int first_mb, pps_id, frame_num, idr_pic_id;
	unsigned int pic_order_cnt_lsb;
	int delta_pic_order_cnt_bottom;
	bool idr, reference;
};

/* Bounded multi-slice scope: progressive 8-bit420, POC0/2, ordinary
 * ordered I/P/B slices, one group and no redundant picture. ITU-T H.264
 * 7.4.1.2.4 picture-boundary fields reduce here to PPS, frame_num, IDR/id and
 * reference zero/nonzero, plus POC0 LSB and conditional signed bottom delta.
 * These are equality keys within one AU, not an absolute POC/DPB calculation.
 * POC1 and fields remain UNKNOWN for multi-slice.
 */
static inline bool chd_h264_range_picture_prefix(const struct chd_h264_range_state *s,
	const unsigned char *nal, unsigned int bytes, struct chd_h264_range_picture *out)
{
	struct chd_h264_range_bits b = {nal, bytes < 128 ? bytes : 128, 1, 0, 0, 0, false};
	struct chd_h264_range_picture p = {0};
	const struct chd_h264_sps_range *sps;
	unsigned int type, slice_type;
	if (!nal || bytes < 2 || (nal[0] & 0x80)) return false;
	type = nal[0] & 31;
	if ((type != 1 && type != 5) || (type == 5 && !(nal[0] & 0x60))) return false;
	p.first_mb = chd_h264_range_ue(&b);
	slice_type = chd_h264_range_ue(&b); p.pps_id = chd_h264_range_ue(&b);
	if (b.failed || p.pps_id > 255 || slice_type > 9 || slice_type % 5 > 2 ||
	    !s->pps[p.pps_id].valid || !s->pps[p.pps_id].picture_qualified ||
	    !s->sps[s->pps[p.pps_id].sps_id].valid) return false;
	sps = &s->sps[s->pps[p.pps_id].sps_id].value;
	if (!sps->frame_mbs_only || (sps->poc_type != 0 && sps->poc_type != 2) ||
	    sps->frame_num_bits < 4 || sps->frame_num_bits > 16 ||
	    !sps->pic_size_in_map_units || p.first_mb >= sps->pic_size_in_map_units)
		return false;
	p.frame_num = chd_h264_range_get(&b, sps->frame_num_bits);
	p.idr = type == 5; p.reference = (nal[0] & 0x60) != 0;
	if (p.idr) {
		p.idr_pic_id = chd_h264_range_ue(&b);
		if (p.frame_num || slice_type % 5 != 2 || p.idr_pic_id > 65535) return false;
	}
	if (sps->poc_type == 0) {
		if (sps->poc_lsb_bits < 4 || sps->poc_lsb_bits > 16) return false;
		p.pic_order_cnt_lsb = chd_h264_range_get(&b, sps->poc_lsb_bits);
		if (s->pps[p.pps_id].bottom_field_poc_present)
			p.delta_pic_order_cnt_bottom = chd_h264_range_se(&b);
	}
	if (b.failed) return false;
	*out = p; return true;
}

static inline bool chd_h264_range_same_picture(const struct chd_h264_range_picture *a,
	const struct chd_h264_range_picture *b)
{
	return a->pps_id == b->pps_id && a->frame_num == b->frame_num &&
	       a->idr == b->idr && a->idr_pic_id == b->idr_pic_id &&
	       a->reference == b->reference &&
	       a->pic_order_cnt_lsb == b->pic_order_cnt_lsb &&
	       a->delta_pic_order_cnt_bottom == b->delta_pic_order_cnt_bottom;
}
/* Sequential Annex-B scan; zero padding is framing, not SPS/PPS EBSP. */
static inline unsigned int chd_h264_range_start(const unsigned char *data,
	unsigned int bytes, unsigned int from, unsigned int *start)
{
	unsigned int i, zeros = 0;
	for (i = from; i < bytes; i++) {
		if (!data[i]) { zeros++; continue; }
		if (data[i] == 1 && zeros >= 2) { *start = i - zeros; return i + 1; }
		zeros = 0;
	}
	*start = bytes; return bytes;
}
static inline void chd_h264_observe_range(struct chd_h264_range_state *s,
	const unsigned char *data, unsigned int bytes, struct chd_h264_range_snapshot *out)
{
	struct chd_h264_range_snapshot observed = {0, 0, 0, false, false, false};
	struct chd_h264_range_picture picture = {0};
	unsigned int pos, start, nals = 0, slices = 0, auds = 0, i;
	bool ambiguous = false, picture_qualified = false;
	*out = observed;
	if (!data || !bytes || bytes > CHD_H264_RANGE_MAX_AU || s->exhausted) {
		chd_h264_range_forget(s); return;
	}
	pos = chd_h264_range_start(data, bytes, 0, &start);
	for (i = 0; i < start; i++) if (data[i]) ambiguous = true;
	if (pos == bytes || ambiguous) { chd_h264_range_forget(s); return; }
	while (pos < bytes) {
		unsigned int end, next = chd_h264_range_start(data, bytes, pos, &end);
		unsigned int size, type, p, q;
		bool boundary = end < bytes;
		const unsigned char *nal = data + pos;
		while (end > pos && !data[end - 1]) end--;
		if (++nals > CHD_H264_RANGE_MAX_NALS || end == pos || (nal[0] & 0x80)) {
			chd_h264_range_forget(s); return;
		}
		size = end - pos; type = nal[0] & 31;
		if (type == 7) {
			struct chd_h264_sps_range value;
			if (slices) ambiguous = true;
			if (chd_h264_read_sps_range(nal, size, &value) != CHD_H264_RANGE_OK) {
				chd_h264_range_forget(s); ambiguous = true;
			} else if (s->generation == ~0ULL) {
				s->exhausted = true; chd_h264_range_forget(s); return;
			} else {
				q = value.sps_id; s->sps[q].value = value;
				s->sps[q].generation = ++s->generation; s->sps[q].valid = true;
			}
		} else if (type == 8) {
			if (slices) ambiguous = true;
			if (!chd_h264_pps_prefix(nal, size, &p, &q)) {
				for (i = 0; i < 256; i++) s->pps[i].valid = false;
				ambiguous = true;
			} else {
				s->pps[p].sps_id = q; s->pps[p].valid = true;
				s->pps[p].picture_qualified = chd_h264_pps_picture_prefix(nal, size,
					&s->pps[p].bottom_field_poc_present);
			}
		} else if (type == 1 || type == 5) {
			slices++;
			if (slices > 1) {
				struct chd_h264_range_picture next_picture;
				if (!picture_qualified ||
				    !chd_h264_range_picture_prefix(s, nal, size, &next_picture) ||
				    next_picture.first_mb <= picture.first_mb ||
				    !chd_h264_range_same_picture(&picture, &next_picture)) ambiguous = true;
				else picture = next_picture;
			} else if (!chd_h264_slice_prefix(nal, size, &p)) ambiguous = true;
			else if (s->pps[p].valid && s->sps[s->pps[p].sps_id].valid) {
				q = s->pps[p].sps_id;
				observed.sps_id = q; observed.pps_id = p;
				observed.sps_generation = s->sps[q].generation;
				observed.associated = true;
				observed.range_present = s->sps[q].value.range_present;
				observed.full_range = s->sps[q].value.full_range;
				picture_qualified = chd_h264_range_picture_prefix(s, nal, size, &picture);
			}
		} else if (type == 9) {
			if (++auds > 1 || slices) ambiguous = true;
		} else if (type == 6) {
			if (slices) ambiguous = true;
		} else if (type == 10 || type == 11) {
			chd_h264_range_forget(s);
		} else if (type != 12) {
			chd_h264_range_forget(s); ambiguous = true;
		}
		if (next == bytes) {
			if (boundary) { chd_h264_range_forget(s); return; }
			break;
		}
		pos = next;
	}
	/* Preserve the existing single-first-slice observation. Additional slices
	 * require independently qualified, matching progressive POC2 identities.
	 */
	if (!ambiguous && slices) *out = observed;
}
#endif
