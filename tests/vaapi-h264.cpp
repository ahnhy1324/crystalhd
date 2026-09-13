// SPDX-License-Identifier: LGPL-2.1-or-later

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

/* Keep this test in the implementation's translation unit so it exercises
 * the exact parameter-set builders used by the VA-API driver. */
#define CRYSTALHD_H264_TEST_BUILD
#include "../filters/vaapi/crystalhd_drv_video.cpp"

static void CheckAnnexBNal(const std::vector<uint8_t> &nal, uint8_t type)
{
	assert(nal.size() > 6);
	assert(nal[0] == 0 && nal[1] == 0 && nal[2] == 0 && nal[3] == 1);
	assert((nal[4] & 0x1f) == type);

	for (size_t i = 6; i < nal.size(); ++i) {
		if (nal[i - 2] == 0 && nal[i - 1] == 0)
			assert(nal[i] >= 3);
	}
}

int main()
{
	VAPictureParameterBufferH264 picture = {};
	VASliceParameterBufferH264 slice = {};

	picture.picture_width_in_mbs_minus1 = 119;
	picture.picture_height_in_mbs_minus1 = 67;
	picture.num_ref_frames = 4;
	picture.seq_fields.bits.frame_mbs_only_flag = 1;
	picture.seq_fields.bits.direct_8x8_inference_flag = 1;
	picture.seq_fields.bits.log2_max_frame_num_minus4 = 0;
	picture.seq_fields.bits.pic_order_cnt_type = 0;
	picture.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 = 0;
	picture.pic_fields.bits.entropy_coding_mode_flag = 1;
	picture.pic_fields.bits.deblocking_filter_control_present_flag = 1;
	slice.num_ref_idx_l0_active_minus1 = 3;
	slice.num_ref_idx_l1_active_minus1 = 0;

	struct ProfileExpectation {
		VAProfile profile;
		uint8_t profile_idc;
	};
	const ProfileExpectation profiles[] = {
		{VAProfileH264ConstrainedBaseline, 66},
		{VAProfileH264Main, 77},
		{VAProfileH264High, 100},
	};

	for (const ProfileExpectation &expected : profiles) {
		std::vector<uint8_t> sps;
		std::vector<uint8_t> pps;

		assert(BuildSps(picture, expected.profile, &sps));
		CheckAnnexBNal(sps, 7);
		assert(sps[5] == expected.profile_idc);

		assert(BuildPps(picture, slice, expected.profile, &pps));
		CheckAnnexBNal(pps, 8);
	}

	std::vector<uint8_t> rejected;
	assert(!BuildSps(picture, VAProfileNone, &rejected));
	assert(rejected.empty());
	picture.seq_fields.bits.pic_order_cnt_type = 1;
	assert(!BuildSps(picture, VAProfileH264High, &rejected));
	assert(rejected.empty());

	// The timestamped packet must begin at its own AU, never the next AU.
	std::vector<uint8_t> access_unit;
	BeginAccessUnit(&access_unit);
	const std::vector<uint8_t> slice_bytes = {0, 0, 0, 1, 0x65, 0x88, 0x84};
	access_unit.insert(access_unit.end(), slice_bytes.begin(), slice_bytes.end());
	std::vector<uint8_t> delimiter(access_unit.begin(), access_unit.begin() + 6);
	assert(std::equal(slice_bytes.begin(), slice_bytes.end(), access_unit.begin() + 6));
	assert(delimiter.size() == 6);
	assert(delimiter[0] == 0 && delimiter[1] == 0 &&
	       delimiter[2] == 0 && delimiter[3] == 1);
	assert(delimiter[4] == 9); // nal_ref_idc=0, access_unit_delimiter
	assert((delimiter[5] >> 5) == 7); // I/P/B/SP/SI slices permitted
	assert((delimiter[5] & 0x1f) == 0x10); // valid RBSP termination

	// A stalled infinite sync fails truthfully; finite client timeouts retain
	// their distinct timeout status and are not converted into decode errors.
	assert(!DecodeBatchGraceExpired(kDecodeBatchGraceNs - 1));
	assert(DecodeBatchGraceExpired(kDecodeBatchGraceNs));
	assert(DecodeWaitStatus(VA_TIMEOUT_INFINITE, kDecodeTimeoutNs - 1) ==
	       VA_STATUS_SUCCESS);
	assert(DecodeWaitStatus(VA_TIMEOUT_INFINITE, kDecodeTimeoutNs) ==
	       VA_STATUS_ERROR_DECODING_ERROR);
	assert(DecodeWaitStatus(0, 0) == VA_STATUS_ERROR_TIMEDOUT);
	assert(DecodeWaitStatus(500, 499) == VA_STATUS_SUCCESS);
	assert(DecodeWaitStatus(500, 500) == VA_STATUS_ERROR_TIMEDOUT);
	assert(DecodeWaitStatus(kDecodeTimeoutNs + 1, kDecodeTimeoutNs) ==
	       VA_STATUS_SUCCESS);

	return 0;
}
