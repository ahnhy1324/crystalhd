// SPDX-License-Identifier: LGPL-2.1-or-later

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <vector>

#include "../filters/vaapi/crystalhd-h264.h"

using crystalhd_h264::AppendNal;
using crystalhd_h264::BeginAccessUnit;
using crystalhd_h264::BuildPps;
using crystalhd_h264::BuildSps;
using crystalhd_h264::FlatH264IqMatrix;
using crystalhd_h264::ValidateH264ProcessingGeometry;

static unsigned int scaling_checks = 0;
static unsigned int scaling_failures = 0;

static void CheckScaling(bool condition, const char *message)
{
	++scaling_checks;
	if (!condition) {
		if (scaling_failures++ < 12)
			fprintf(stderr, "H.264 scaling regression: %s\n", message);
	}
}

static VAIQMatrixBufferH264 FlatMatrix()
{
	VAIQMatrixBufferH264 matrix = {};
	memset(matrix.ScalingList4x4, 16, sizeof(matrix.ScalingList4x4));
	memset(matrix.ScalingList8x8, 16, sizeof(matrix.ScalingList8x8));
	return matrix;
}

static bool BuildMatrixPps(const VAPictureParameterBufferH264 &picture,
			  const VASliceParameterBufferH264 &slice,
			  VAProfile profile, const VAIQMatrixBufferH264 &matrix,
			  std::vector<uint8_t> *output)
{
	return BuildPps(picture, slice, profile, matrix, output);
}

class PpsReader {
public:
	explicit PpsReader(const std::vector<uint8_t> &nal)
	{
		if (nal.size() < 6 || nal[0] || nal[1] || nal[2] ||
		    nal[3] != 1 || (nal[4] & 31) != 8)
			throw std::runtime_error("invalid Annex-B PPS");
		unsigned int zeros = 0;
		for (size_t i = 5; i < nal.size(); ++i) {
			if (zeros == 2 && nal[i] == 3) {
				if (i + 1 == nal.size() || nal[i + 1] > 3)
					throw std::runtime_error("invalid emulation prevention");
				zeros = 0;
				continue;
			}
			bytes.push_back(nal[i]);
			zeros = nal[i] == 0 ? zeros + 1 : 0;
		}
	}

	unsigned int Bit()
	{
		if (offset / 8 >= bytes.size())
			throw std::runtime_error("truncated PPS");
		const unsigned int value = (bytes[offset / 8] >> (7 - offset % 8)) & 1;
		++offset;
		return value;
	}

	unsigned int UE()
	{
		unsigned int zeros = 0;
		while (!Bit()) {
			if (++zeros > 30)
				throw std::runtime_error("oversized Golomb code");
		}
		unsigned int value = 1;
		while (zeros--)
			value = value * 2 + Bit();
		return value - 1;
	}

	int SE()
	{
		const unsigned int code = UE();
		return (code & 1) ? static_cast<int>((code + 1) / 2)
				  : -static_cast<int>(code / 2);
	}

	void Finish()
	{
		if (!Bit())
			throw std::runtime_error("missing RBSP stop bit");
		while (offset % 8 != 0) {
			if (Bit())
				throw std::runtime_error("unexpected PPS tail");
		}
		if (offset != bytes.size() * 8)
			throw std::runtime_error("extra bytes after PPS trailing bits");
	}

private:
	std::vector<uint8_t> bytes;
	size_t offset = 0;
};

// Independent geometric zigzag, not the production lookup tables. Each
// diagonal alternates direction; VA's destination matrix is row-major.
static void ReadScalingList(PpsReader *reader, uint8_t *matrix, int width)
{
	if (!reader->Bit())
		throw std::runtime_error("nonflat PPS must explicitly encode every active list");
	int last = 8;
	for (int diagonal = 0; diagonal < width * 2 - 1; ++diagonal) {
		const int low = std::max(0, diagonal - width + 1);
		const int high = std::min(diagonal, width - 1);
		for (int step = low; step <= high; ++step) {
			const int row = (diagonal & 1) ? step : high - (step - low);
			const int column = diagonal - row;
			const int delta = reader->SE();
			if (delta < -128 || delta > 127)
				throw std::runtime_error("scaling delta outside [-128,127]");
			const int next = (last + delta + 256) % 256;
			if (next == 0)
				throw std::runtime_error("explicit list unexpectedly invokes a default/repeat sentinel");
			matrix[row * width + column] = static_cast<uint8_t>(next);
			last = next;
		}
	}
}

static VAIQMatrixBufferH264 ReadMatrixPps(const std::vector<uint8_t> &nal,
				       bool transform8, bool matrices,
				       int second_chroma)
{
	PpsReader reader(nal);
	if (reader.UE() || reader.UE())
		throw std::runtime_error("unexpected parameter-set IDs");
	reader.Bit(); reader.Bit();
	if (reader.UE())
		throw std::runtime_error("unexpected slice groups");
	reader.UE(); reader.UE();
	reader.Bit(); reader.Bit(); reader.Bit();
	reader.SE(); reader.SE(); reader.SE();
	reader.Bit(); reader.Bit(); reader.Bit();
	CheckScaling(reader.Bit() == transform8, "transform_8x8 flag roundtrip");
	const bool present = reader.Bit();
	CheckScaling(present == matrices, "PPS scaling-matrix presence");
	VAIQMatrixBufferH264 decoded = FlatMatrix();
	if (present) {
		for (auto &list : decoded.ScalingList4x4)
			ReadScalingList(&reader, list, 4);
		if (transform8) {
			for (auto &list : decoded.ScalingList8x8)
				ReadScalingList(&reader, list, 8);
		}
	}
	CheckScaling(reader.SE() == second_chroma, "second chroma QP follows only active lists");
	reader.Finish();
	return decoded;
}

static void ScalingMatrices(VAPictureParameterBufferH264 picture,
			    const VASliceParameterBufferH264 &slice)
{
	picture.second_chroma_qp_index_offset = -3;
	const VAIQMatrixBufferH264 flat = FlatMatrix();
	const auto default_matrix = FlatH264IqMatrix();
	CheckScaling(memcmp(default_matrix.ScalingList4x4, flat.ScalingList4x4,
			    sizeof(flat.ScalingList4x4)) == 0 &&
		     memcmp(default_matrix.ScalingList8x8, flat.ScalingList8x8,
			    sizeof(flat.ScalingList8x8)) == 0,
		     "missing per-picture IQ starts with flat16 matrices");
	std::vector<uint8_t> flat_pps;
	CheckScaling(BuildMatrixPps(picture, slice, VAProfileH264High, flat, &flat_pps),
		     "flat High matrix accepted");
	for (bool transform8 : {false, true}) {
		picture.pic_fields.bits.transform_8x8_mode_flag = transform8;
		for (unsigned int pattern = 0; pattern < 6; ++pattern) {
			VAIQMatrixBufferH264 matrix = flat;
			for (unsigned int list = 0; list < 6; ++list) {
				for (unsigned int i = 0; i < 16; ++i) {
					if (pattern == 1)
						matrix.ScalingList4x4[list][i] = 1 + (list * 37 + i * 11) % 255;
					else if (pattern == 2)
						matrix.ScalingList4x4[list][i] = (i + list) % 2 ? 255 : 1;
					else if (pattern >= 4)
						matrix.ScalingList4x4[list][i] = (i + list) % 2 ? 129 - (pattern - 4) : 1;
				}
			}
			for (unsigned int list = 0; list < 2; ++list) {
				for (unsigned int i = 0; i < 64; ++i) {
					if (pattern == 1)
						matrix.ScalingList8x8[list][i] = 1 + (list * 89 + i * 17) % 255;
					else if (pattern == 2)
						matrix.ScalingList8x8[list][i] = (i + list) % 2 ? 1 : 255;
					else if (pattern >= 4)
						matrix.ScalingList8x8[list][i] = (i + list) % 2 ? 1 : 129 - (pattern - 4);
				}
			}
			if (pattern == 3)
				matrix.ScalingList4x4[2][7] = 129;
			std::vector<uint8_t> pps;
			CheckScaling(BuildMatrixPps(picture, slice, VAProfileH264High, matrix, &pps),
				     "valid High matrices accepted");
			const auto decoded = ReadMatrixPps(pps, transform8, pattern != 0, -3);
			CheckScaling(memcmp(decoded.ScalingList4x4, matrix.ScalingList4x4,
					    sizeof(matrix.ScalingList4x4)) == 0,
				     "all six distinct 4x4 matrices survive raster/zigzag conversion");
			if (transform8)
				CheckScaling(memcmp(decoded.ScalingList8x8, matrix.ScalingList8x8,
						    sizeof(matrix.ScalingList8x8)) == 0,
					     "both distinct 8x8 matrices survive raster/zigzag conversion");
		}
	}

	picture.pic_fields.bits.transform_8x8_mode_flag = 1;
	for (unsigned int list = 0; list < 2; ++list) {
		VAIQMatrixBufferH264 matrix = flat;
		matrix.ScalingList8x8[list][42] = 237;
		std::vector<uint8_t> pps;
		CheckScaling(BuildMatrixPps(picture, slice, VAProfileH264High, matrix, &pps),
			     "custom 8x8 alone requires matrix syntax");
		const auto decoded = ReadMatrixPps(pps, true, true, -3);
		CheckScaling(memcmp(decoded.ScalingList4x4, flat.ScalingList4x4,
				    sizeof(flat.ScalingList4x4)) == 0 &&
			     memcmp(decoded.ScalingList8x8, matrix.ScalingList8x8,
				    sizeof(matrix.ScalingList8x8)) == 0,
			     "each 8x8 list independently prevents flat inference");
	}
	picture.pic_fields.bits.transform_8x8_mode_flag = 0;
	VAIQMatrixBufferH264 unused = flat;
	memset(unused.ScalingList8x8, 0, sizeof(unused.ScalingList8x8));
	std::vector<uint8_t> unused_pps;
	CheckScaling(BuildMatrixPps(picture, slice, VAProfileH264High, unused, &unused_pps) &&
		     unused_pps == flat_pps, "inactive 8x8 entries are ignored; flat PPS bytes unchanged");
	for (bool transform8 : {false, true}) {
		picture.pic_fields.bits.transform_8x8_mode_flag = transform8;
		for (unsigned int list = 0; list < (transform8 ? 8U : 6U); ++list) {
			VAIQMatrixBufferH264 invalid = flat;
			if (list < 6)
				invalid.ScalingList4x4[list][15] = 0;
			else
				invalid.ScalingList8x8[list - 6][63] = 0;
			std::vector<uint8_t> unchanged = {0xaa, 0x55};
			CheckScaling(!BuildMatrixPps(picture, slice, VAProfileH264High, invalid, &unchanged) &&
				     unchanged == std::vector<uint8_t>({0xaa, 0x55}),
				     "active zero coefficient rejected without partial output");
		}
	}
	picture.pic_fields.bits.transform_8x8_mode_flag = 0;
	for (VAProfile profile : {VAProfileH264Main, VAProfileH264ConstrainedBaseline}) {
		std::vector<uint8_t> pps;
		CheckScaling(BuildMatrixPps(picture, slice, profile, flat, &pps),
			     "existing non-High flat matrices remain supported");
		VAIQMatrixBufferH264 custom = flat;
		custom.ScalingList4x4[0][0] = 17;
		pps.clear();
		CheckScaling(!BuildMatrixPps(picture, slice, profile, custom, &pps) && pps.empty(),
			     "non-High custom matrices fail instead of silently becoming flat");
	}
}

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

class SpsReader {
public:
	explicit SpsReader(const std::vector<uint8_t> &nal)
	{
		if (nal.size() < 6 || nal[0] || nal[1] || nal[2] ||
		    nal[3] != 1 || (nal[4] & 31) != 7)
			throw std::runtime_error("invalid Annex-B SPS");
		unsigned int zeros = 0;
		for (size_t i = 5; i < nal.size(); ++i) {
			if (zeros == 2 && nal[i] == 3) {
				if (i + 1 == nal.size() || nal[i + 1] > 3)
					throw std::runtime_error("invalid emulation prevention");
				zeros = 0;
				continue;
			}
			bytes.push_back(nal[i]);
			zeros = nal[i] == 0 ? zeros + 1 : 0;
		}
	}

	unsigned int Bits(unsigned int count)
	{
		unsigned int value = 0;
		while (count--)
			value = value * 2 + Bit();
		return value;
	}

	unsigned int Bit()
	{
		if (offset / 8 >= bytes.size())
			throw std::runtime_error("truncated SPS");
		const unsigned int value = (bytes[offset / 8] >> (7 - offset % 8)) & 1;
		++offset;
		return value;
	}

	unsigned int UE()
	{
		unsigned int zeros = 0;
		while (!Bit()) {
			if (++zeros > 30)
				throw std::runtime_error("oversized Golomb code");
		}
		unsigned int value = 1;
		while (zeros--)
			value = value * 2 + Bit();
		return value - 1;
	}

private:
	std::vector<uint8_t> bytes;
	size_t offset = 0;
};

static void CheckSpsGeometry(const std::vector<uint8_t> &sps,
			     unsigned int expected_height_in_map_units_minus1,
			     bool frame_mbs_only, bool mbaff,
			     unsigned int expected_crop_bottom = 0)
{
	SpsReader reader(sps);
	const unsigned int profile = reader.Bits(8);
	reader.Bits(8); // constraint flags
	reader.Bits(8); // level_idc
	assert(reader.UE() == 0); // seq_parameter_set_id
	if (profile == 100) {
		assert(reader.UE() == 1); // chroma_format_idc
		assert(reader.UE() == 0); // bit_depth_luma_minus8
		assert(reader.UE() == 0); // bit_depth_chroma_minus8
		assert(reader.Bit() == 0); // qpprime_y_zero_transform_bypass_flag
		assert(reader.Bit() == 0); // seq_scaling_matrix_present_flag
	}
	reader.UE(); // log2_max_frame_num_minus4
	assert(reader.UE() == 0); // pic_order_cnt_type
	reader.UE(); // log2_max_pic_order_cnt_lsb_minus4
	reader.UE(); // max_num_ref_frames
	reader.Bit(); // gaps_in_frame_num_value_allowed_flag
	reader.UE(); // pic_width_in_mbs_minus1
	assert(reader.UE() == expected_height_in_map_units_minus1);
	assert(reader.Bit() == frame_mbs_only);
	if (!frame_mbs_only)
		assert(reader.Bit() == mbaff);
	reader.Bit(); // direct_8x8_inference_flag
	assert(reader.Bit() == (expected_crop_bottom != 0));
	if (expected_crop_bottom != 0) {
		assert(reader.UE() == 0); // frame_crop_left_offset
		assert(reader.UE() == 0); // frame_crop_right_offset
		assert(reader.UE() == 0); // frame_crop_top_offset
		assert(reader.UE() == expected_crop_bottom);
	}
}

int main()
{
	VAPictureParameterBufferH264 picture = {};
	VASliceParameterBufferH264 slice = {};

	picture.picture_width_in_mbs_minus1 = 119;
	picture.picture_height_in_mbs_minus1 = 67;
	picture.num_ref_frames = 4;
	picture.seq_fields.bits.chroma_format_idc = 1;
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
	const std::vector<uint8_t> baseline_sps = {
		0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x40, 0x28,
		0xf2, 0x80, 0xf0, 0x04, 0x4d, 0x80, 0x88, 0x00,
		0x00, 0x03, 0x00, 0x08, 0x00, 0x00, 0x03, 0x01,
		0xe4, 0x78, 0xc1, 0x92, 0xc0,
	};
	const std::vector<uint8_t> main_sps = {
		0x00, 0x00, 0x00, 0x01, 0x67, 0x4d, 0x40, 0x28,
		0xf2, 0x80, 0xf0, 0x04, 0x4d, 0x80, 0x88, 0x00,
		0x00, 0x03, 0x00, 0x08, 0x00, 0x00, 0x03, 0x01,
		0xe4, 0x78, 0xc1, 0x92, 0xc0,
	};
	const std::vector<uint8_t> high_sps = {
		0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x00, 0x28,
		0xac, 0xe5, 0x01, 0xe0, 0x08, 0x9b, 0x01, 0x10,
		0x00, 0x00, 0x03, 0x00, 0x10, 0x00, 0x00, 0x03,
		0x03, 0xc8, 0xf1, 0x83, 0x25, 0x80,
	};
	const std::vector<uint8_t> baseline_main_pps = {
		0x00, 0x00, 0x00, 0x01, 0x68, 0xe9, 0x23, 0xc8,
	};
	const std::vector<uint8_t> high_pps = {
		0x00, 0x00, 0x00, 0x01, 0x68, 0xe9, 0x23, 0xc3,
	};

	for (const ProfileExpectation &expected : profiles) {
		std::vector<uint8_t> sps;
		std::vector<uint8_t> pps;

		assert(BuildSps(picture, expected.profile, &sps));
		CheckAnnexBNal(sps, 7);
		assert(sps[5] == expected.profile_idc);
		CheckSpsGeometry(sps, 67, true, false);
		std::vector<uint8_t> explicit_default, full_visible, cropped;
		assert(BuildSps(picture, expected.profile, &explicit_default, 0, 0));
		assert(BuildSps(picture, expected.profile, &full_visible, 1920, 1088));
		assert(sps == explicit_default && sps == full_visible);
		assert(BuildSps(picture, expected.profile, &cropped, 1920, 1080));
		CheckSpsGeometry(cropped, 67, true, false, 4);
		// Profile/output-format selection does not validate caller-supplied
		// picture syntax. Never rewrite unsupported input as 8-bit 4:2:0.
		for (unsigned int fault = 0; fault < 8; ++fault) {
			auto invalid = picture;
			switch (fault) {
			case 0: invalid.seq_fields.bits.chroma_format_idc = 0; break;
			case 1: invalid.seq_fields.bits.chroma_format_idc = 2; break;
			case 2: invalid.seq_fields.bits.chroma_format_idc = 3; break;
			case 3: invalid.seq_fields.bits.residual_colour_transform_flag = 1; break;
			case 4: invalid.bit_depth_luma_minus8 = 1; break;
			case 5: invalid.bit_depth_chroma_minus8 = 1; break;
			case 6: invalid.bit_depth_luma_minus8 = 255; break;
			case 7: invalid.bit_depth_chroma_minus8 = 255; break;
			}
			const std::vector<uint8_t> prior = {0xaa, 0x55};
			std::vector<uint8_t> rejected_format = prior;
			assert(!BuildSps(invalid, expected.profile, &rejected_format));
			assert(rejected_format == prior);
			assert(!BuildSps(invalid, expected.profile, &rejected_format,
					 1920, 1080));
			assert(rejected_format == prior);
		}

		assert(BuildPps(picture, slice, expected.profile, FlatMatrix(), &pps));
		CheckAnnexBNal(pps, 8);
		const std::vector<uint8_t> &golden_sps =
			expected.profile == VAProfileH264ConstrainedBaseline
				? baseline_sps
				: expected.profile == VAProfileH264Main ? main_sps : high_sps;
		assert(sps == golden_sps);
		assert(pps == (expected.profile == VAProfileH264High
				       ? high_pps : baseline_main_pps));
	}

	// VA's height is the full decoded frame in macroblocks.  The SPS uses
	// half as many map units when frame_mbs_only_flag is clear.
	picture.seq_fields.bits.frame_mbs_only_flag = 0;
	picture.seq_fields.bits.mb_adaptive_frame_field_flag = 1;
	std::vector<uint8_t> interlaced_sps;
	assert(BuildSps(picture, VAProfileH264High, &interlaced_sps));
	CheckSpsGeometry(interlaced_sps, 33, false, true);
	std::vector<uint8_t> interlaced_cropped;
	assert(BuildSps(picture, VAProfileH264High, &interlaced_cropped,
			1920, 1080));
	CheckSpsGeometry(interlaced_cropped, 33, false, true, 2);
	for (const auto &visible : {
			std::pair<unsigned int, unsigned int>{1920, 1082},
			{1920, 1089}, {1918, 1080}, {0, 1080}, {1920, 0}}) {
		std::vector<uint8_t> invalid_crop = {0xaa, 0x55};
		assert(!BuildSps(picture, VAProfileH264High, &invalid_crop,
				 visible.first, visible.second));
		assert(invalid_crop == std::vector<uint8_t>({0xaa, 0x55}));
	}
	picture.picture_height_in_mbs_minus1 = 66;
	std::vector<uint8_t> invalid_interlaced_sps = {0xaa, 0x55};
	assert(!BuildSps(picture, VAProfileH264High, &invalid_interlaced_sps));
	assert(invalid_interlaced_sps == std::vector<uint8_t>({0xaa, 0x55}));
	picture.picture_height_in_mbs_minus1 = 67;
	picture.seq_fields.bits.frame_mbs_only_flag = 1;
	picture.seq_fields.bits.mb_adaptive_frame_field_flag = 0;
	assert(ValidateH264ProcessingGeometry(1920, 1088, 1920, 1080,
					     1280, 720));
	assert(ValidateH264ProcessingGeometry(1920, 1088, 1920, 1080,
					     960, 540));
	assert(ValidateH264ProcessingGeometry(1920, 1088, 1920, 1080,
					     1312, 738));
	assert(!ValidateH264ProcessingGeometry(1920, 1088, 1920, 1088,
					      1280, 724));
	assert(!ValidateH264ProcessingGeometry(1920, 1088, 1918, 1080,
					      1280, 720));
	assert(!ValidateH264ProcessingGeometry(1920, 1088, 1920, 1080,
					      1920, 1080));
	assert(!ValidateH264ProcessingGeometry(1920, 1088, 1920, 1080,
					      1280, 722));
	assert(!ValidateH264ProcessingGeometry(1920, 1088, 1920, 1080,
					      64, 36));
	assert(!ValidateH264ProcessingGeometry(1920, 1088, 1920, 1080,
					      944, 531));
	assert(!ValidateH264ProcessingGeometry(1920, 1090, 1920, 1080,
					      1280, 720));

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

	std::vector<uint8_t> escaped;
	AppendNal(&escaped, 1, 3,
		  {0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04,
		   0x00, 0x00, 0x03});
	const std::vector<uint8_t> golden_escaped = {
		0x00, 0x00, 0x00, 0x01, 0x61,
		0x00, 0x00, 0x03, 0x00, 0x01, 0x02, 0x03, 0x04,
		0x00, 0x00, 0x03, 0x03,
	};
	assert(escaped == golden_escaped);

	try {
		ScalingMatrices(picture, slice);
	} catch (const std::exception &error) {
		CheckScaling(false, error.what());
	}
	printf("H.264 scaling matrices: %u checks, %u failures\n",
	       scaling_checks, scaling_failures);
	return scaling_failures != 0;
}
