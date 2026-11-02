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
	struct { unsigned int sps_id; bool valid; } pps[256];
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
	unsigned int pos, start, nals = 0, slices = 0, auds = 0, i;
	bool ambiguous = false;
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
			} else { s->pps[p].sps_id = q; s->pps[p].valid = true; }
		} else if (type == 1 || type == 5) {
			slices++;
			if (slices != 1 || !chd_h264_slice_prefix(nal, size, &p)) ambiguous = true;
			else if (s->pps[p].valid && s->sps[s->pps[p].sps_id].valid) {
				q = s->pps[p].sps_id;
				observed.sps_id = q; observed.pps_id = p;
				observed.sps_generation = s->sps[q].generation;
				observed.associated = true;
				observed.range_present = s->sps[q].value.range_present;
				observed.full_range = s->sps[q].value.full_range;
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
	/* One first slice only. Full frame_num/field/POC picture identity and
	 * multi-slice/partition/extension streams are deliberately unqualified. */
	if (!ambiguous && slices == 1) *out = observed;
}
#endif
