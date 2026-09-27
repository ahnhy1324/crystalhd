// SPDX-License-Identifier: LGPL-2.1-or-later

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

/* Keep this test in the implementation's translation unit so it exercises
 * the exact parameter-set builders used by the VA-API driver. */
#define CRYSTALHD_H264_TEST_BUILD
#include "../filters/vaapi/crystalhd_drv_video.cpp"

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

		assert(BuildPps(picture, slice, expected.profile, FlatMatrix(), &pps));
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

	try {
		ScalingMatrices(picture, slice);
	} catch (const std::exception &error) {
		CheckScaling(false, error.what());
	}
	printf("H.264 scaling matrices: %u checks, %u failures\n",
	       scaling_checks, scaling_failures);
	return scaling_failures != 0;
}
