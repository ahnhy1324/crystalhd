/*
 * VA-API backend for Broadcom Crystal HD BCM70012/BCM70015 devices.
 *
 * The H.264 parameter-set reconstruction follows the ChromiumOS libva fake
 * driver's BSD-licensed H.264 decoder delegate. The hardware integration and
 * VA surface implementation are specific to CrystalHD.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <climits>
#include <condition_variable>
#include <cstddef>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#if defined(__SSE2__)
#include <cpuid.h>
#include <emmintrin.h>
#endif
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <gbm.h>
#include <i915_drm.h>
#include <libdrm/drm_fourcc.h>
#include <libdrm/drm_mode.h>
#include <linux/dma-buf.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xf86drm.h>

#include <va/va.h>
#include <va/va_backend.h>
#include <va/va_backend_vpp.h>
#include <va/va_drmcommon.h>
#include <va/va_version.h>
#include <va/va_vpp.h>
extern "C" {
#include <libswscale/swscale.h>
}

#include <bc_dts_defs.h>
#include <bc_dts_types.h>
#include <libcrystalhd_if.h>
#include "../crystalhd-h264-output.h"
#include "crystalhd-access-unit-limits.h"
#include "crystalhd-decode-replay.h"
#include "crystalhd-mpeg4.h"
#include "crystalhd-mpeg2.h"
#include "crystalhd-mpeg2-replay.h"
#include "crystalhd-private-backing.h"
#include "crystalhd-trace.h"
#include "crystalhd-vc1.h"
#include "crystalhd-vc1-replay.h"
#include "crystalhd-wmv3.h"
#include "crystalhd-vpp-bob.h"

#if defined(__GNUC__)
// Keep the plugin loadable with an older libcrystalhd.so.3. The optional
// output-event API is negotiated only when the symbol is present.
extern "C" BC_STATUS DtsDupOutputPollFd(HANDLE, int *) __attribute__((weak));
// Older libraries retain the qualified full-device replay restart. Only an
// explicit lifecycle capability enables retaining their device handle.
extern "C" BC_STATUS DtsGetLibraryCapabilities(uint32_t *) __attribute__((weak));
// Large H.264 AUs use this additive libcrystalhd contract when available;
// ordinary bounded AUs retain the legacy DtsProcInput path.
extern "C" BC_STATUS DtsProcInputFragment(
    HANDLE, uint8_t *, uint32_t, uint64_t, BOOL, uint32_t)
    __attribute__((weak));
#endif

using CrystalHDFragmentInput = BC_STATUS (*)(
    HANDLE, uint8_t *, uint32_t, uint64_t, BOOL, uint32_t);
using CrystalHDLibraryCapabilitiesQuery = BC_STATUS (*)(uint32_t *);
#if defined(CRYSTALHD_VAAPI_TEST_FRAGMENT_INPUT)
static CrystalHDFragmentInput crystalhd_test_fragment_input = nullptr;
#endif
#if defined(CRYSTALHD_VAAPI_TEST_LIBRARY_CAPABILITIES)
static CrystalHDLibraryCapabilitiesQuery crystalhd_test_library_capabilities = nullptr;
#endif

namespace {

#ifndef CRYSTALHD_H264_TEST_BUILD
static bool IsH264Profile(VAProfile profile);
#endif

[[maybe_unused]] static CrystalHDFragmentInput H264FragmentInput() {
#if defined(CRYSTALHD_VAAPI_TEST_FRAGMENT_INPUT)
  return crystalhd_test_fragment_input;
#else
  return DtsProcInputFragment;
#endif
}

[[maybe_unused]] static bool SupportsDrainedH264ChannelReopen() {
#if defined(CRYSTALHD_VAAPI_TEST_LIBRARY_CAPABILITIES)
  const auto query = crystalhd_test_library_capabilities;
#else
  const auto query = DtsGetLibraryCapabilities;
#endif
  uint32_t capabilities = 0;
  return query != nullptr && query(&capabilities) == BC_STS_SUCCESS &&
         (capabilities & DTS_LIB_CAP_FLEA_H264_DRAINED_REOPEN) != 0;
}

constexpr uint64_t kDecodeTimeoutNs = 10ULL * 1000 * 1000 * 1000;
constexpr uint64_t kDecodeBatchGraceNs = 100ULL * 1000 * 1000;
constexpr uint64_t kBoundedH264InactivityGraceNs =
    1ULL * 1000 * 1000 * 1000;
constexpr size_t kMpeg4ReplayCacheBytes = 32U * 1024U * 1024U;
constexpr size_t kMpeg4ReplayPictures = 512;
constexpr size_t kExperimentalPresentationReadyLimit = 3;
constexpr size_t kExperimentalPresentationActiveLimit = 2;
constexpr uint64_t kExperimentalPresentationMaxAgeNs =
    2ULL * 1000 * 1000 * 1000 / 30;
constexpr uint64_t kExperimentalDelay16Ns = 16667ULL * 1000;
constexpr uint64_t kExperimentalDelay33Ns = 33333ULL * 1000;
constexpr uint64_t kExperimentalDelay50Ns = 50000ULL * 1000;
// Legacy libcrystalhd treats this input bit as a request to return queue/TX
// counters without peeking and synchronizing the READY-head DMA buffer.
constexpr uint32_t kStatusSkipReadyHeadPeek = 1U << 30;

[[maybe_unused]] static bool ParseExperimentalReadyDelay(
    const char *value, uint64_t *delay_ns) {
  if (delay_ns == nullptr)
    return false;
  if (value == nullptr || strcmp(value, "0") == 0) {
    *delay_ns = 0;
    return true;
  }
  if (strcmp(value, "16667") == 0) {
    *delay_ns = kExperimentalDelay16Ns;
    return true;
  }
  if (strcmp(value, "33333") == 0) {
    *delay_ns = kExperimentalDelay33Ns;
    return true;
  }
  if (strcmp(value, "50000") == 0) {
    *delay_ns = kExperimentalDelay50Ns;
    return true;
  }
  return false;
}

static bool DecodeBatchGraceExpired(uint64_t elapsed_ns,
                                    bool bounded_h264_inactivity = false) {
  return elapsed_ns >= (bounded_h264_inactivity
                            ? kBoundedH264InactivityGraceNs
                            : kDecodeBatchGraceNs);
}

static VAStatus DecodeWaitStatus(uint64_t timeout_ns, uint64_t elapsed_ns) {
  if (timeout_ns == VA_TIMEOUT_INFINITE)
    return elapsed_ns >= kDecodeTimeoutNs ? VA_STATUS_ERROR_DECODING_ERROR
                                         : VA_STATUS_SUCCESS;
  return elapsed_ns >= timeout_ns ? VA_STATUS_ERROR_TIMEDOUT
                                  : VA_STATUS_SUCCESS;
}

#ifndef CRYSTALHD_H264_TEST_BUILD
constexpr unsigned int kMaxWidth = 1920;
constexpr unsigned int kMaxHeight = 1088;
constexpr uint64_t kTimestampStep = 100000;
constexpr char kSwSyncPath[] = "/sys/kernel/debug/sync/sw_sync";

struct SwSyncCreateFenceData {
  uint32_t value;
  char name[32];
  int32_t fence;
};

#define SW_SYNC_IOC_MAGIC 'W'
#define SW_SYNC_IOC_CREATE_FENCE \
  _IOWR(SW_SYNC_IOC_MAGIC, 0, struct SwSyncCreateFenceData)
#define SW_SYNC_IOC_INC _IOW(SW_SYNC_IOC_MAGIC, 1, uint32_t)

static void Debug(const char *format, ...) {
  if (getenv("CRYSTALHD_VAAPI_DEBUG") == nullptr)
    return;
  va_list arguments;
  va_start(arguments, format);
  fputs("crystalhd-vaapi: ", stderr);
  vfprintf(stderr, format, arguments);
  fputc('\n', stderr);
  va_end(arguments);
}

static void DebugBytes(const char *label, const std::vector<uint8_t> &bytes) {
  if (getenv("CRYSTALHD_VAAPI_DEBUG") == nullptr)
    return;
  fprintf(stderr, "crystalhd-vaapi: %s", label);
  for (size_t i = 0; i < std::min<size_t>(bytes.size(), 64); ++i)
    fprintf(stderr, " %02x", bytes[i]);
  fputc('\n', stderr);
}

static unsigned int Align(unsigned int value, unsigned int alignment) {
  return (value + alignment - 1) / alignment * alignment;
}
#endif

class BitWriter {
 public:
  void PutBit(bool value) {
    if (bit_offset_ == 0)
      bytes_.push_back(0);
    bytes_.back() |= static_cast<uint8_t>(value) << (7 - bit_offset_);
    bit_offset_ = (bit_offset_ + 1) & 7;
  }

  void PutBits(unsigned int count, uint64_t value) {
    for (unsigned int bit = count; bit > 0; --bit)
      PutBit(((value >> (bit - 1)) & 1U) != 0);
  }

  void PutUE(uint32_t value) {
    uint64_t code = static_cast<uint64_t>(value) + 1;
    unsigned int bits = 0;
    for (uint64_t temp = code; temp != 0; temp >>= 1)
      ++bits;
    for (unsigned int i = 1; i < bits; ++i)
      PutBit(false);
    PutBits(bits, code);
  }

  void PutSE(int32_t value) {
    PutUE(value <= 0 ? static_cast<uint32_t>(-2LL * value)
                     : static_cast<uint32_t>(2LL * value - 1));
  }

  std::vector<uint8_t> FinishRbsp() {
    PutBit(true);
    while (bit_offset_ != 0)
      PutBit(false);
    return std::move(bytes_);
  }

 private:
  std::vector<uint8_t> bytes_;
  unsigned int bit_offset_ = 0;
};

static void AppendNal(std::vector<uint8_t> *output, uint8_t type,
                      uint8_t reference_idc, std::vector<uint8_t> rbsp) {
  static constexpr uint8_t start_code[] = {0, 0, 0, 1};
  output->insert(output->end(), std::begin(start_code), std::end(start_code));
  output->push_back(static_cast<uint8_t>((reference_idc << 5) | type));

  unsigned int zero_count = 0;
  for (uint8_t byte : rbsp) {
    if (zero_count >= 2 && byte <= 3) {
      output->push_back(3);
      zero_count = 0;
    }
    output->push_back(byte);
    zero_count = byte == 0 ? zero_count + 1 : 0;
  }
}

static void BeginAccessUnit(std::vector<uint8_t> *output) {
  // H.264 7.4.1.2.3 puts AUD first in its access unit. Keep that boundary in
  // the same logical input as its picture. A prefed timestamp is consumed by
  // submitting this complete AU (including its own AUD) with timestamp zero;
  // a trailing AUD in the previous AU would instead start the next picture and
  // misassociate firmware PTS. A subsequent AU or explicit sealed-batch EOS
  // delimits the last slice.
  // primary_pic_type=7 permits all slice types, followed by rbsp_stop_one_bit.
  AppendNal(output, 9, 0, {0xf0});
}

#ifndef CRYSTALHD_H264_TEST_BUILD
static const std::vector<uint8_t> &H264LowLatencyPrefeed() {
  // A timestamped 170-byte ES payload becomes one 184-byte PES transfer on
  // BCM70015.  Hardware measurements show that an AUD at this boundary can
  // advance the following access unit by one input cadence; filler_data keeps
  // the prefix syntactically complete without predicting any slice bytes.
  static const std::vector<uint8_t> bytes = [] {
    std::vector<uint8_t> result = {0x00, 0x00, 0x00, 0x01, 0x09, 0xf0,
                                   0x00, 0x00, 0x00, 0x01, 0x0c};
    result.insert(result.end(), 170 - result.size() - 1, 0xff);
    result.push_back(0x80);
    return result;
  }();
  return bytes;
}
#endif

static bool ValidateH264ProcessingGeometry(
    unsigned int coded_width, unsigned int coded_height,
    unsigned int visible_width, unsigned int visible_height,
    unsigned int capture_width, unsigned int capture_height) {
  // The card accepts a scale width and derives its height from the source
  // aspect ratio. Admit only an exact ratio here: a rounded storage height
  // cannot be repaired by clipping rows after the card has resampled them.
  // The caller supplies an explicit origin-zero visible rectangle; 1080 must
  // never be inferred from a 1088-line coded picture.
  return coded_width != 0 && coded_width <= 1920 &&
         coded_height != 0 && coded_height <= 1088 &&
         ((coded_width | coded_height) & 15U) == 0 &&
         visible_width == coded_width && visible_height != 0 &&
         visible_height <= coded_height &&
         ((visible_width | visible_height) & 1U) == 0 &&
         capture_width >= 128 && capture_width < visible_width &&
         capture_height != 0 && capture_height <= visible_height &&
         ((capture_width | capture_height) & 1U) == 0 &&
         static_cast<uint64_t>(visible_height) * capture_width ==
             static_cast<uint64_t>(capture_height) * visible_width;
}

static bool BuildSps(const VAPictureParameterBufferH264 &picture,
                     VAProfile profile, std::vector<uint8_t> *output,
                     unsigned int visible_width = 0,
                     unsigned int visible_height = 0) {
  BitWriter bits;
  uint8_t profile_idc;
  unsigned int height_in_map_units;
  unsigned int crop_bottom = 0;

  if (visible_width != 0 || visible_height != 0) {
    const unsigned int coded_width =
        (picture.picture_width_in_mbs_minus1 + 1U) * 16U;
    const unsigned int coded_height =
        (picture.picture_height_in_mbs_minus1 + 1U) * 16U;
    const unsigned int crop_unit_y =
        picture.seq_fields.bits.frame_mbs_only_flag ? 2U : 4U;
    if (coded_width > 1920 || coded_height > 1088 ||
        visible_width != coded_width || visible_height == 0 ||
        visible_height > coded_height ||
        (coded_height - visible_height) % crop_unit_y != 0)
      return false;
    crop_bottom = (coded_height - visible_height) / crop_unit_y;
  }

  switch (profile) {
    case VAProfileH264ConstrainedBaseline:
      profile_idc = 66;
      break;
    case VAProfileH264Main:
      profile_idc = 77;
      break;
    case VAProfileH264High:
      profile_idc = 100;
      break;
    default:
      return false;
  }

  // VA reports the decoded frame height in macroblocks.  H.264 SPS syntax
  // instead counts map units, where an interlaced sequence has two
  // macroblock rows per map unit (7.4.2.1.1).  Passing the VA value through
  // unchanged advertises twice the coded height for MBAFF/field sequences and
  // leaves the firmware waiting for a picture that can never complete.
  height_in_map_units = picture.picture_height_in_mbs_minus1 + 1U;
  if (!picture.seq_fields.bits.frame_mbs_only_flag) {
    if ((height_in_map_units & 1U) != 0)
      return false;
    height_in_map_units /= 2U;
  }

  bits.PutBits(8, profile_idc);
  bits.PutBits(8, (profile == VAProfileH264ConstrainedBaseline ||
                   profile == VAProfileH264Main)
                      ? 0x40
                      : 0);
  const unsigned int macroblocks =
      (picture.picture_width_in_mbs_minus1 + 1) *
      (picture.picture_height_in_mbs_minus1 + 1);
  bits.PutBits(8, macroblocks <= 3600 ? 31 : 40);
  bits.PutUE(0);

  if (profile_idc == 100) {
    bits.PutUE(1);
    bits.PutUE(0);
    bits.PutUE(0);
    bits.PutBit(false);
    bits.PutBit(false);
  }

  bits.PutUE(picture.seq_fields.bits.log2_max_frame_num_minus4);
  bits.PutUE(picture.seq_fields.bits.pic_order_cnt_type);
  if (picture.seq_fields.bits.pic_order_cnt_type == 0) {
    bits.PutUE(picture.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4);
  } else if (picture.seq_fields.bits.pic_order_cnt_type == 1) {
    return false;
  }
  bits.PutUE(picture.num_ref_frames);
  bits.PutBit(picture.seq_fields.bits.gaps_in_frame_num_value_allowed_flag);
  bits.PutUE(picture.picture_width_in_mbs_minus1);
  bits.PutUE(height_in_map_units - 1U);
  bits.PutBit(picture.seq_fields.bits.frame_mbs_only_flag);
  if (!picture.seq_fields.bits.frame_mbs_only_flag)
    bits.PutBit(picture.seq_fields.bits.mb_adaptive_frame_field_flag);
  bits.PutBit(picture.seq_fields.bits.direct_8x8_inference_flag);
  bits.PutBit(crop_bottom != 0);
  if (crop_bottom != 0) {
    bits.PutUE(0);
    bits.PutUE(0);
    bits.PutUE(0);
    bits.PutUE(crop_bottom);
  }
  bits.PutBit(true);
  bits.PutBit(true);
  bits.PutBits(8, 1);
  bits.PutBit(false);
  bits.PutBit(false);
  bits.PutBit(false);
  bits.PutBit(true);
  // The browser extension rejects formats above 30 fps. Describe that exact
  // cadence to CrystalHD instead of the old 25 fps VUI: a mismatched timing
  // signal makes the firmware's output cadence fight Chromium's presentation
  // clock, which is observed as regular frame dropping and short stalls.
  bits.PutBits(32, 1);
  bits.PutBits(32, 60);
  bits.PutBit(true);
  bits.PutBit(false);
  bits.PutBit(false);
  bits.PutBit(false);
  bits.PutBit(true);
  bits.PutBit(true);
  bits.PutUE(0);
  bits.PutUE(0);
  bits.PutUE(11);
  bits.PutUE(11);
  bits.PutUE(0);
  bits.PutUE(picture.num_ref_frames);
  AppendNal(output, 7, 3, bits.FinishRbsp());
  return true;
}

static VAIQMatrixBufferH264 FlatH264IqMatrix() {
  VAIQMatrixBufferH264 matrix = {};
  memset(matrix.ScalingList4x4, 16, sizeof(matrix.ScalingList4x4));
  memset(matrix.ScalingList8x8, 16, sizeof(matrix.ScalingList8x8));
  return matrix;
}

static bool ValidateH264IqMatrix(const VAPictureParameterBufferH264 &picture,
                                 VAProfile profile,
                                 const VAIQMatrixBufferH264 &matrix,
                                 bool *flat = nullptr) {
  bool all_flat = true;
  const auto valid_list = [&all_flat](const uint8_t *list, size_t size) {
    for (size_t i = 0; i < size; ++i) {
      // VA supplies effective coefficients, not H.264's delta/default syntax.
      if (list[i] == 0)
        return false;
      all_flat &= list[i] == 16;
    }
    return true;
  };
  for (const auto &list : matrix.ScalingList4x4) {
    if (!valid_list(list, sizeof(list)))
      return false;
  }
  if (picture.pic_fields.bits.transform_8x8_mode_flag) {
    for (const auto &list : matrix.ScalingList8x8) {
      if (!valid_list(list, sizeof(list)))
        return false;
    }
  }
  if (profile != VAProfileH264High && !all_flat)
    return false;
  if (flat != nullptr)
    *flat = all_flat;
  return true;
}

template <size_t Size>
static void PutH264ScalingList(BitWriter *bits, const uint8_t (&matrix)[Size],
                               const uint8_t (&scan)[Size]) {
  bits->PutBit(true);
  int last = 8;
  for (uint8_t position : scan) {
    const int next = matrix[position];
    int delta = next - last;
    if (delta > 127)
      delta -= 256;
    else if (delta < -128)
      delta += 256;
    bits->PutSE(delta);
    last = next;
  }
}

static bool BuildPps(const VAPictureParameterBufferH264 &picture,
                     const VASliceParameterBufferH264 &slice,
                     VAProfile profile, const VAIQMatrixBufferH264 &matrix,
                     std::vector<uint8_t> *output) {
  bool flat;
  if (!ValidateH264IqMatrix(picture, profile, matrix, &flat))
    return false;
  BitWriter bits;
  bits.PutUE(0);
  bits.PutUE(0);
  bits.PutBit(picture.pic_fields.bits.entropy_coding_mode_flag);
  bits.PutBit(picture.pic_fields.bits.pic_order_present_flag);
  bits.PutUE(0);
  bits.PutUE(slice.num_ref_idx_l0_active_minus1);
  bits.PutUE(slice.num_ref_idx_l1_active_minus1);
  bits.PutBit(picture.pic_fields.bits.weighted_pred_flag);
  bits.PutBits(2, picture.pic_fields.bits.weighted_bipred_idc);
  bits.PutSE(picture.pic_init_qp_minus26);
  bits.PutSE(picture.pic_init_qs_minus26);
  bits.PutSE(picture.chroma_qp_index_offset);
  bits.PutBit(picture.pic_fields.bits.deblocking_filter_control_present_flag);
  bits.PutBit(picture.pic_fields.bits.constrained_intra_pred_flag);
  bits.PutBit(picture.pic_fields.bits.redundant_pic_cnt_present_flag);
  if (profile == VAProfileH264High) {
    bits.PutBit(picture.pic_fields.bits.transform_8x8_mode_flag);
    bits.PutBit(!flat);
    if (!flat) {
      // VA arrays are raster ordered; H.264 scaling_list() is zigzag ordered.
      // Clients already resolve SPS/PPS inheritance and JVT defaults. Emit
      // every active effective list explicitly, including flat ones: omitting
      // an individual list here would invoke a different fallback rule.
      static constexpr uint8_t scan4[16] = {
          0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15};
      static constexpr uint8_t scan8[64] = {
          0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
          12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
          35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
          58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
      for (const auto &list : matrix.ScalingList4x4)
        PutH264ScalingList(&bits, list, scan4);
      if (picture.pic_fields.bits.transform_8x8_mode_flag) {
        for (const auto &list : matrix.ScalingList8x8)
          PutH264ScalingList(&bits, list, scan8);
      }
    }
    bits.PutSE(picture.second_chroma_qp_index_offset);
  }
  AppendNal(output, 8, 3, bits.FinishRbsp());
  return true;
}

#ifdef CRYSTALHD_H264_TEST_BUILD
}  // namespace
#else
struct Config {
  VAProfile profile = VAProfileNone;
  VAEntrypoint entrypoint = VAEntrypointVLD;
  uint32_t rt_format = 0;
  bool decode_processing = false;
};

// Image exports use independent backing, never decoder-reference memory.
// Writable cold-image bytes are committed separately after synchronization.
struct ImageExportBacking {
  gbm_bo *bo = nullptr;
  int fd = -1;
  void *mapping = MAP_FAILED;
  size_t size = 0;

  ~ImageExportBacking() {
    if (mapping != MAP_FAILED)
      munmap(mapping, size);
    if (fd >= 0)
      close(fd);
    if (bo != nullptr)
      gbm_bo_destroy(bo);
  }
};

struct Surface;
struct Buffer {
  VAContextID context = VA_INVALID_ID;
  VABufferType type = VAPictureParameterBufferType;
  unsigned int element_size = 0;
  unsigned int elements = 0;
  std::vector<uint8_t> data;
  unsigned int map_count = 0;
  bool export_busy = false;
  bool data_failed = false;
  std::shared_ptr<ImageExportBacking> exported;
  std::shared_ptr<Surface> derived_source, derived_owner;
  VAImageID derived_image = VA_INVALID_ID;
  bool derived_write_active = false;
  bool derived_was_ready = false;
};

static bool BufferBorrowed(const Buffer &buffer) {
  return buffer.export_busy || buffer.exported != nullptr;
}

static bool SyncDmaBuf(int fd, uint64_t flags) {
  dma_buf_sync sync = {};
  sync.flags = flags;
  int result;
  do {
    result = ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
  } while (result != 0 && (errno == EINTR || errno == EAGAIN));
  return result == 0;
}

static bool ExperimentalI915WcRequested(const char *value) {
  return value != nullptr && strcmp(value, "1") == 0;
}

static bool IsI915DriverName(const char *name, size_t length) {
  constexpr char expected[] = "i915";
  return name != nullptr && length == sizeof(expected) - 1 &&
         memcmp(name, expected, sizeof(expected) - 1) == 0;
}

static bool IsI915DrmFd(int fd) {
  if (fd < 0)
    return false;
  drmVersionPtr version = drmGetVersion(fd);
  if (version == nullptr)
    return false;
  const bool is_i915 = IsI915DriverName(version->name, version->name_len);
  drmFreeVersion(version);
  return is_i915;
}

// CLFLUSH is independent of SSE2. Both it and MFENCE must be available before
// an asynchronous writer installs a fence that DMA_BUF_SYNC END could wait on.
// A legacy CPU build contains neither instruction; synchronous CPU access still
// uses the kernel's DMA_BUF_IOCTL_SYNC protocol without this special path.
[[maybe_unused]] static size_t CpuCacheLineBytes(unsigned int ebx,
                                               unsigned int edx) {
  constexpr unsigned int required = (1U << 19) | (1U << 26);
  const size_t bytes = ((ebx >> 8) & 0xffU) * 8U;
  return (edx & required) == required && bytes != 0 &&
                 (bytes & (bytes - 1)) == 0
             ? bytes
             : 0;
}

struct CpuCacheFlushOps {
  size_t line_bytes = 0;
  void (*flush_line)(const void *) = nullptr;
  void (*barrier)() = nullptr;

  bool Available() const {
    return line_bytes != 0 && (line_bytes & (line_bytes - 1)) == 0 &&
           flush_line != nullptr && barrier != nullptr;
  }
};

static const CpuCacheFlushOps &CpuCacheFlush() {
  static const CpuCacheFlushOps operations = [] {
    CpuCacheFlushOps result;
#if defined(__SSE2__)
    unsigned int eax, ebx, ecx, edx;
    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
      result.line_bytes = CpuCacheLineBytes(ebx, edx);
      if (result.line_bytes != 0) {
        result.flush_line = [](const void *address) { _mm_clflush(address); };
        result.barrier = [] { _mm_mfence(); };
      }
    }
#endif
    return result;
  }();
  return operations;
}

struct BackingIdentity {
  dev_t device = 0;
  ino_t inode = 0;

  bool operator==(const BackingIdentity &other) const {
    return device == other.device && inode == other.inode;
  }
};

struct SurfaceLayout {
  struct Plane {
    BackingIdentity backing;
    uint32_t offset = 0;
    uint32_t pitch = 0;
    uint64_t modifier = DRM_FORMAT_MOD_LINEAR;
    bool operator==(const Plane &other) const {
      return backing == other.backing && offset == other.offset &&
             pitch == other.pitch && modifier == other.modifier;
    }
  };
  bool valid = false;
  uint32_t fourcc = 0;
  unsigned int width = 0;
  unsigned int height = 0;
  unsigned int num_planes = 0;
  std::vector<BackingIdentity> objects;
  std::array<Plane, 2> planes = {};
  bool operator==(const SurfaceLayout &other) const {
    return valid && other.valid && fourcc == other.fourcc &&
           width == other.width && height == other.height &&
           num_planes == other.num_planes && objects == other.objects &&
           planes == other.planes;
  }
};

// Cache discovery metadata, not a DRM file or a backing allocation. Each
// surface still opens/closes its own primary-node file, preserving GEM handle,
// DRM-master and runtime-PM lifetime. Calls are serialized by Driver::mutex.
struct PrimaryDrmNodeCache {
  std::string path;
  struct stat render_identity = {};
  struct stat primary_identity = {};

  static bool SameNode(const struct stat &left, const struct stat &right) {
    return left.st_dev == right.st_dev && left.st_ino == right.st_ino &&
           left.st_rdev == right.st_rdev;
  }

  int Open(int drm_fd) {
    if (drm_fd < 0)
      return -1;
    struct stat render = {};
    const bool have_render_identity = fstat(drm_fd, &render) == 0;
    if (!path.empty() && have_render_identity &&
        SameNode(render, render_identity)) {
      const int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
      struct stat primary = {};
      if (fd >= 0 && fstat(fd, &primary) == 0 &&
          SameNode(primary, primary_identity))
        return fd;
      if (fd >= 0)
        close(fd);
    }
    path.clear();

    drmDevicePtr device = nullptr;
    int fd = -1;
    if (drmGetDevice2(drm_fd, 0, &device) == 0 && device != nullptr &&
        (device->available_nodes & (1U << DRM_NODE_PRIMARY)) != 0 &&
        device->nodes != nullptr && device->nodes[DRM_NODE_PRIMARY] != nullptr) {
      const char *node = device->nodes[DRM_NODE_PRIMARY];
      fd = open(node, O_RDWR | O_CLOEXEC);
      struct stat primary = {};
      if (fd >= 0 && have_render_identity && fstat(fd, &primary) == 0) {
        // Discovery caching is optional: failure to retain the path must not
        // discard an otherwise successful allocation path or leak its fd.
        try {
          path = node;
          render_identity = render;
          primary_identity = primary;
        } catch (const std::bad_alloc &) {
          path.clear();
        }
      }
    }
    if (device != nullptr)
      drmFreeDevice(&device);
    return fd;
  }
};

static bool ImportedPlaneFits(uint64_t bytes, uint64_t offset, uint64_t pitch,
                               uint64_t rows, uint64_t row_bytes) {
  // Subtraction/division keeps the last-row bound valid on i686 too; forming
  // pitch * (rows - 1) in size_t can wrap before comparing it with the fd size.
  return rows != 0 && row_bytes != 0 && pitch >= row_bytes && offset <= bytes &&
         row_bytes <= bytes - offset &&
         rows - 1 <= (bytes - offset - row_bytes) / pitch;
}

// A successful decoder teardown can precede a filter's last use of its VA
// surfaces. Retain only their exact completed, independently backed pictures;
// this lease never retains a decoder or authorizes a different public binding.
struct CompletedDecodePicture {
  std::weak_ptr<Surface> owner;
  std::shared_ptr<Surface> picture;
  VASurfaceID surface = VA_INVALID_SURFACE;
  uint64_t timestamp = 0;
  uint64_t identity = 0;
  uint64_t rejection_history = 0;
  uint64_t context = 0;
  uint64_t generation = 0;
  uint64_t ordinal = 0;
};

struct Surface {
  VASurfaceID id = VA_INVALID_SURFACE;
  unsigned int width = 0;
  unsigned int height = 0;
  unsigned int rt_format = 0;
  uint32_t fourcc = VA_FOURCC_NV12;
  unsigned int pitch[2] = {};
  uint8_t *planes[2] = {};
  bool ready = false;
  bool failed = false;
  bool destroyed = false;
  // Card scaling keeps the original reconstruction in the decoder DPB.
  // This identity has completion, but never exposes fabricated host pixels.
  bool opaque_reference = false;
  VAContextID opaque_context = VA_INVALID_ID;
  bool external_accessed = false;
  std::shared_ptr<Surface> opaque_picture;
  unsigned int vpp_readers = 0;
  unsigned int vpp_writers = 0;
  bool image_write_locked = false;
  unsigned int export_waiters = 0;
  VASurfaceID backing_owner = VA_INVALID_SURFACE;
  uint64_t expected_timestamp = 0;
  uint64_t frame_timestamp = 0;
  uint64_t latest_vpp_sequence = 0;
  uint64_t decode_identity = 0;
  // Begin/Render/End rejection ownership, separate from an accepted picture.
  uint64_t picture_transaction = 0;
  uint64_t rejected_picture_transaction = 0;
  uint64_t trace_decode_identity = 0;
  uint64_t trace_context = 0;
  uint64_t trace_generation = 0;
  uint64_t trace_submission_ordinal = 0;
  // The public VA binding remains identified by expected_timestamp and the
  // trace identity above. Experimental presentation may copy a different,
  // completed private picture into that binding; keep its identity separate
  // rather than relabeling the requested decode as complete.
  bool experimental_presentation = false;
  bool experimental_write_lease = false;
  uint64_t experimental_write_context = 0;
  uint64_t experimental_write_generation = 0;
  uint64_t experimental_write_submission_ordinal = 0;
  bool experimental_incompatible = false;
  bool experimental_ready_queued = false;
  uint64_t experimental_submitted_at_ns = 0;
  uint64_t experimental_ready_at_ns = 0;
  uint64_t experimental_presentation_at_ns = 0;
  uint64_t presentation_payload_token = 0;
  uint64_t presentation_payload_decode_identity = 0;
  uint64_t presentation_payload_submission_ordinal = 0;
  bool trace_read_export_armed = false;
  crystalhd_vaapi_trace::Fields trace_read_export;
  bool experimental_read_export_armed = false;
  crystalhd_vaapi_trace::Fields experimental_read_export;
  SurfaceLayout layout;
  bool direct_decode_eligible = false;
  unsigned int h264_crop_unit_y = 0;

  // Live H.264 pictures may initially use the public target's pixels while
  // keeping independent readiness/identity. Capture or backing reuse promotes
  // them to private storage; their own planes remain unset until promotion.
  // The reverse link is weak to avoid a lifetime cycle.
  std::shared_ptr<Surface> direct_backing;
  std::weak_ptr<Surface> decode_target;
  std::weak_ptr<Surface> decode_picture;
  CompletedDecodePicture completed_decode_picture;
  bool direct_picture = false;

  std::vector<uint8_t> storage;
  std::weak_ptr<CrystalHDPrivateBackingPool> private_backing_pool;
  gbm_bo *bo = nullptr;
  void *map_data[2] = {};
  std::vector<void *> object_maps;
  std::vector<size_t> object_sizes;
  std::vector<int> object_fds;
  int dumb_drm_fd = -1;
  uint32_t dumb_handle = 0;
  bool owned_i915_dumb = false;
  bool experimental_i915_wc = false;

  ~Surface() {
    if (bo != nullptr) {
      for (void *mapping : map_data) {
        if (mapping != nullptr)
          gbm_bo_unmap(bo, mapping);
      }
      gbm_bo_destroy(bo);
    }
    for (size_t i = 0; i < object_maps.size(); ++i) {
      if (object_maps[i] != MAP_FAILED && object_maps[i] != nullptr)
        munmap(object_maps[i], object_sizes[i]);
    }
    for (int fd : object_fds)
      close(fd);
    if (dumb_drm_fd >= 0) {
      drm_mode_destroy_dumb destroy = {};
      destroy.handle = dumb_handle;
      ioctl(dumb_drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
      close(dumb_drm_fd);
    }
    if (bo == nullptr && dumb_drm_fd < 0 && object_fds.empty() &&
        object_maps.empty() && !layout.valid && !direct_picture && !direct_backing) {
      if (auto pool = private_backing_pool.lock())
        pool->Recycle(std::move(storage));
    }
  }

  bool AllocateCpuNv12(unsigned int requested_width,
                        unsigned int requested_height,
                        const std::shared_ptr<CrystalHDPrivateBackingPool> &pool = {}) {
    if (requested_width > UINT_MAX - 63U)
      return false;
    const size_t stride = Align(requested_width, 64);
    const size_t luma_rows = requested_height;
    const size_t chroma_rows = luma_rows / 2 + luma_rows % 2;
    if (luma_rows > SIZE_MAX - chroma_rows ||
        (luma_rows + chroma_rows != 0 &&
         stride > SIZE_MAX / (luma_rows + chroma_rows)))
      return false;
    const size_t bytes = stride * (luma_rows + chroma_rows);
    storage = pool ? pool->Acquire(bytes) : std::vector<uint8_t>(bytes);
    private_backing_pool = pool;
    width = requested_width;
    height = requested_height;
    fourcc = VA_FOURCC_NV12;
    pitch[0] = pitch[1] = static_cast<unsigned int>(stride);
    planes[0] = storage.data();
    planes[1] = bytes == 0 ? nullptr : planes[0] + stride * luma_rows;
    return true;
  }

  bool AllocateInternal(gbm_device *gbm, int drm_fd,
                        unsigned int requested_width,
                        unsigned int requested_height,
                        uint32_t requested_fourcc,
                        bool allow_experimental_i915_wc = false,
                        PrimaryDrmNodeCache *primary_node_cache = nullptr) {
    width = requested_width;
    height = requested_height;
    fourcc = requested_fourcc;
    if (fourcc == VA_FOURCC_ARGB) {
      if (gbm == nullptr)
        return false;
      bo = gbm_bo_create(gbm, width, height, GBM_FORMAT_ARGB8888,
                         GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
      if (bo == nullptr) {
        bo = gbm_bo_create(gbm, width, height, GBM_FORMAT_ARGB8888,
                           GBM_BO_USE_RENDERING);
      }
      if (bo == nullptr)
        return false;
      uint32_t mapped_pitch = 0;
      planes[0] = static_cast<uint8_t *>(gbm_bo_map(
          bo, 0, 0, width, height, GBM_BO_TRANSFER_WRITE, &mapped_pitch,
          &map_data[0]));
      if (planes[0] == MAP_FAILED || planes[0] == nullptr)
        return false;
      pitch[0] = mapped_pitch;
      return true;
    }
    if (fourcc != VA_FOURCC_NV12)
      return false;
    if (gbm != nullptr) {
      bo = gbm_bo_create(gbm, width, height, GBM_FORMAT_NV12,
                         GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
      if (bo == nullptr) {
        bo = gbm_bo_create(gbm, width, height, GBM_FORMAT_NV12,
                           GBM_BO_USE_LINEAR);
      }
      if (bo != nullptr && gbm_bo_get_plane_count(bo) >= 2) {
        uint32_t mapped_pitch = 0;
        planes[0] = static_cast<uint8_t *>(gbm_bo_map(
            bo, 0, 0, width, height, GBM_BO_TRANSFER_WRITE, &mapped_pitch,
            &map_data[0]));
        if (planes[0] != MAP_FAILED && planes[0] != nullptr) {
          pitch[0] = mapped_pitch;
          pitch[1] = gbm_bo_get_stride_for_plane(bo, 1);
          planes[1] = planes[0] + gbm_bo_get_offset(bo, 1);
          return true;
        }
        planes[0] = nullptr;
        if (map_data[0] != nullptr) {
          gbm_bo_unmap(bo, map_data[0]);
          map_data[0] = nullptr;
        }
      }
      if (bo != nullptr) {
        gbm_bo_destroy(bo);
        bo = nullptr;
      }
    }
    if (drm_fd >= 0) {
      PrimaryDrmNodeCache uncached;
      dumb_drm_fd = (primary_node_cache != nullptr ? primary_node_cache
                                                  : &uncached)->Open(drm_fd);
    }
    if (dumb_drm_fd >= 0) {
      drm_mode_create_dumb create = {};
      create.width = Align(width, 64);
      create.height = height + (height + 1) / 2;
      create.bpp = 8;
      if (ioctl(dumb_drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) == 0) {
        drm_mode_map_dumb map = {};
        map.handle = create.handle;
        int prime_fd = -1;
        if (ioctl(dumb_drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &map) == 0 &&
            drmPrimeHandleToFD(dumb_drm_fd, create.handle,
                               DRM_CLOEXEC | DRM_RDWR, &prime_fd) == 0) {
          void *mapping = mmap(nullptr, create.size, PROT_READ | PROT_WRITE,
                               MAP_SHARED, dumb_drm_fd, map.offset);
          if (mapping != MAP_FAILED) {
            dumb_handle = create.handle;
            object_maps.push_back(mapping);
            object_sizes.push_back(create.size);
            object_fds.push_back(prime_fd);
            pitch[0] = create.pitch;
            pitch[1] = create.pitch;
            planes[0] = static_cast<uint8_t *>(mapping);
            planes[1] = planes[0] +
                        static_cast<size_t>(pitch[0]) * height;
            if (allow_experimental_i915_wc) {
              owned_i915_dumb = IsI915DrmFd(dumb_drm_fd);
              experimental_i915_wc = owned_i915_dumb;
              Debug(owned_i915_dumb
                        ? "experimental i915 WC/GTT domains enabled"
                        : "experimental i915 WC/GTT domains unavailable on non-i915 DRM");
            }
            return true;
          }
          close(prime_fd);
        }
        drm_mode_destroy_dumb destroy = {};
        destroy.handle = create.handle;
        ioctl(dumb_drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
      }
      close(dumb_drm_fd);
      dumb_drm_fd = -1;
    }
    return AllocateCpuNv12(width, height);
  }

  bool ImportPrime(gbm_device *gbm, const VADRMPRIMESurfaceDescriptor &desc) {
    const bool is_nv12 = desc.fourcc == VA_FOURCC_NV12;
    const bool is_argb = desc.fourcc == VA_FOURCC_ARGB ||
                         desc.fourcc == VA_FOURCC_BGRA;
    if ((!is_nv12 && !is_argb) || desc.width != width ||
        desc.height != height || desc.num_objects == 0 ||
        desc.num_objects > 4 || desc.num_layers == 0 || desc.num_layers > 4)
      return false;
    fourcc = is_argb ? VA_FOURCC_ARGB : VA_FOURCC_NV12;

    struct PlaneDesc {
      uint32_t object;
      uint32_t offset;
      uint32_t pitch;
    } plane_desc[2] = {};
    unsigned int plane_count = 0;
    for (uint32_t layer = 0; layer < desc.num_layers; ++layer) {
      for (uint32_t plane = 0; plane < desc.layers[layer].num_planes; ++plane) {
        if (plane_count >= 2 || desc.layers[layer].object_index[plane] >=
                                    desc.num_objects)
          return false;
        plane_desc[plane_count++] = {desc.layers[layer].object_index[plane],
                                     desc.layers[layer].offset[plane],
                                     desc.layers[layer].pitch[plane]};
      }
    }
    const unsigned int expected_planes = is_nv12 ? 2 : 1;
    if (plane_count != expected_planes)
      return false;

    bool linear = true;
    for (uint32_t object = 0; object < desc.num_objects; ++object) {
      uint64_t modifier = desc.objects[object].drm_format_modifier;
      if (modifier != DRM_FORMAT_MOD_LINEAR && modifier != DRM_FORMAT_MOD_INVALID)
        linear = false;
    }

    if (!linear && gbm != nullptr) {
      gbm_import_fd_modifier_data import = {};
      import.width = width;
      import.height = height;
      import.format = is_nv12 ? GBM_FORMAT_NV12 : GBM_FORMAT_ARGB8888;
      import.num_fds = desc.num_objects;
      import.modifier = desc.objects[0].drm_format_modifier;
      for (unsigned int plane = 0; plane < expected_planes; ++plane) {
        import.fds[plane] = desc.objects[plane_desc[plane].object].fd;
        import.strides[plane] = plane_desc[plane].pitch;
        import.offsets[plane] = plane_desc[plane].offset;
      }
      bo = gbm_bo_import(gbm, GBM_BO_IMPORT_FD_MODIFIER, &import,
                         GBM_BO_USE_WRITE);
      if (bo != nullptr) {
        uint32_t mapped_pitch = 0;
        planes[0] = static_cast<uint8_t *>(gbm_bo_map(
            bo, 0, 0, width, height, GBM_BO_TRANSFER_WRITE,
            &mapped_pitch, &map_data[0]));
        if (planes[0] == MAP_FAILED || planes[0] == nullptr)
          return false;
        pitch[0] = mapped_pitch;
        if (is_nv12) {
          pitch[1] = gbm_bo_get_stride_for_plane(bo, 1);
          planes[1] = planes[0] + gbm_bo_get_offset(bo, 1);
        }
        return true;
      }
    }

    if (!linear)
      return false;

    for (unsigned int plane = 0; plane < expected_planes; ++plane) {
      const uint64_t rows = plane == 0 ? height : (uint64_t{height} + 1) / 2;
      const uint64_t row_bytes = is_argb ? uint64_t{width} * 4
          : (plane == 0 ? width : (uint64_t{width} + 1) & ~uint64_t{1});
      if (!ImportedPlaneFits(desc.objects[plane_desc[plane].object].size,
                              plane_desc[plane].offset, plane_desc[plane].pitch,
                              rows, row_bytes))
        return false;
    }
    object_maps.resize(desc.num_objects, MAP_FAILED);
    object_sizes.resize(desc.num_objects);
    object_fds.resize(desc.num_objects, -1);
    for (uint32_t object = 0; object < desc.num_objects; ++object) {
      object_fds[object] = dup(desc.objects[object].fd);
      object_sizes[object] = desc.objects[object].size;
      if (object_fds[object] < 0 || object_sizes[object] == 0)
        return false;
      object_maps[object] = mmap(nullptr, object_sizes[object],
                                 PROT_READ | PROT_WRITE, MAP_SHARED,
                                 object_fds[object], 0);
      if (object_maps[object] == MAP_FAILED)
        return false;
    }
    for (unsigned int plane = 0; plane < expected_planes; ++plane) {
      planes[plane] = static_cast<uint8_t *>(
                          object_maps[plane_desc[plane].object]) +
                      plane_desc[plane].offset;
      pitch[plane] = plane_desc[plane].pitch;
    }
    return true;
  }

  bool ImportLegacy(const VASurfaceAttribExternalBuffers &desc) {
    const bool is_nv12 = desc.pixel_format == VA_FOURCC_NV12;
    const bool is_argb = desc.pixel_format == VA_FOURCC_ARGB ||
                         desc.pixel_format == VA_FOURCC_BGRA;
    const unsigned int expected_planes = is_nv12 ? 2 : 1;
    if ((!is_nv12 && !is_argb) || desc.width != width ||
        desc.height != height || desc.num_planes != expected_planes ||
        desc.num_buffers != 1 || desc.buffers == nullptr ||
        desc.buffers[0] > static_cast<uintptr_t>(INT_MAX) ||
        desc.data_size == 0)
      return false;

    for (unsigned int plane = 0; plane < expected_planes; ++plane) {
      const uint64_t rows = plane == 0 ? height : (uint64_t{height} + 1) / 2;
      const uint64_t row_bytes = is_argb ? uint64_t{width} * 4
          : (plane == 0 ? width : (uint64_t{width} + 1) & ~uint64_t{1});
      if (!ImportedPlaneFits(desc.data_size, desc.offsets[plane], desc.pitches[plane],
                              rows, row_bytes))
        return false;
    }
    const int fd = dup(static_cast<int>(desc.buffers[0]));
    if (fd < 0)
      return false;
    object_fds.push_back(fd);
    object_sizes.push_back(desc.data_size);
    object_maps.push_back(
        mmap(nullptr, desc.data_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    if (object_maps[0] == MAP_FAILED)
      return false;

    fourcc = is_argb ? VA_FOURCC_ARGB : VA_FOURCC_NV12;
    for (unsigned int plane = 0; plane < expected_planes; ++plane) {
      pitch[plane] = desc.pitches[plane];
      planes[plane] = static_cast<uint8_t *>(object_maps[0]) +
                      desc.offsets[plane];
    }
    return true;
  }

  bool BeginCpuAccess(uint64_t direction) const {
    for (size_t object = 0; object < object_fds.size(); ++object) {
      if (!SyncDmaBuf(object_fds[object], DMA_BUF_SYNC_START | direction)) {
        // START ownership is per object. Release only the successfully
        // acquired prefix before reporting a failed, all-or-nothing access.
        while (object != 0)
          SyncDmaBuf(object_fds[--object], DMA_BUF_SYNC_END | direction);
        return false;
      }
    }
    return true;
  }

  bool EndCpuAccess(uint64_t direction) const {
    bool success = true;
    for (int fd : object_fds) {
      if (!SyncDmaBuf(fd, DMA_BUF_SYNC_END | direction))
        success = false;
    }
    return success;
  }

  bool BeginCpuRead() const { return BeginCpuAccess(DMA_BUF_SYNC_READ); }
  bool EndCpuRead() const { return EndCpuAccess(DMA_BUF_SYNC_READ); }
  bool BeginCpuWrite() const { return BeginCpuAccess(DMA_BUF_SYNC_WRITE); }
  bool EndCpuWrite() const { return EndCpuAccess(DMA_BUF_SYNC_WRITE); }

  bool SetI915Domain(uint32_t read_domains, uint32_t write_domain) const {
    if (!owned_i915_dumb || dumb_drm_fd < 0 || dumb_handle == 0)
      return false;
    drm_i915_gem_set_domain domain = {};
    domain.handle = dumb_handle;
    domain.read_domains = read_domains;
    domain.write_domain = write_domain;
    int result;
    do {
      result = ioctl(dumb_drm_fd, DRM_IOCTL_I915_GEM_SET_DOMAIN, &domain);
    } while (result != 0 && errno == EINTR);
    return result == 0;
  }

  // The experiment is deliberately limited to the synchronous CrystalHD
  // YUY2-to-NV12 copy. Generic CPU access remains on DMA_BUF_IOCTL_SYNC, so
  // imported surfaces and VPP never inherit this i915-specific policy.
  bool BeginDecodedCpuWrite(bool *used_i915_wc) {
    if (used_i915_wc == nullptr)
      return false;
    *used_i915_wc = false;
    if (experimental_i915_wc) {
      if (SetI915Domain(I915_GEM_DOMAIN_WC, I915_GEM_DOMAIN_WC)) {
        *used_i915_wc = true;
        return true;
      }
      const int error = errno;
      experimental_i915_wc = false;
      Debug("experimental i915 WC domain failed; using DMA-BUF sync: %s",
            strerror(error));
    }
    return BeginCpuWrite();
  }

  bool EndDecodedCpuWrite(bool used_i915_wc) {
    if (!used_i915_wc)
      return EndCpuWrite();
    if (SetI915Domain(I915_GEM_DOMAIN_GTT, 0))
      return true;

    // DMA_BUF_SYNC START must precede CPU access, so a rejected post-copy
    // domain transition cannot safely recover this frame by bracketing the
    // pixels after they were written. Fail this frame and keep future writes
    // on the established DMA-BUF path.
    const int error = errno;
    experimental_i915_wc = false;
    Debug("experimental i915 GTT domain failed; disabling experiment: %s",
          strerror(error));
    return false;
  }

  // The asynchronous path cannot call DMA_BUF_IOCTL_SYNC END while its own
  // unsignaled write fence is installed: i915 waits on that fence. Flush every
  // mapped cache line explicitly before signaling, so a newly released GPU
  // reader can only observe the fully committed image.
  bool FlushCpuWrites(const CpuCacheFlushOps &operations = CpuCacheFlush()) const {
    if (!operations.Available() || object_maps.size() != object_sizes.size())
      return false;
    // Validate every extent before flushing any of them. The inclusive last
    // address and subtraction-based loop also avoid wrapping at UINTPTR_MAX.
    for (size_t object = 0; object < object_maps.size(); ++object) {
      if (object_maps[object] != MAP_FAILED && object_maps[object] != nullptr &&
          object_sizes[object] != 0 &&
          object_sizes[object] - 1 > UINTPTR_MAX -
              reinterpret_cast<uintptr_t>(object_maps[object]))
        return false;
    }
    const uintptr_t cache_line_size = operations.line_bytes;
    for (size_t object = 0; object < object_maps.size(); ++object) {
      if (object_maps[object] == MAP_FAILED ||
          object_maps[object] == nullptr || object_sizes[object] == 0)
        continue;
      const uintptr_t first =
          reinterpret_cast<uintptr_t>(object_maps[object]) &
          ~(cache_line_size - 1);
      const uintptr_t last =
          reinterpret_cast<uintptr_t>(object_maps[object]) +
          object_sizes[object] - 1;
      for (uintptr_t address = first;; address += cache_line_size) {
        operations.flush_line(reinterpret_cast<const void *>(address));
        if (last - address < cache_line_size)
          break;
      }
    }
    operations.barrier();
    return true;
  }
};

// vaCreateContext describes H.264 coded geometry, while a render target may
// expose only the visible rectangle. Accept a smaller target only when the
// omitted rows or columns are valid 4:2:0 crop units in the final 16x16
// macroblock. Frame-coded vertical crop units are two rows; interlaced
// sequences require four. Pixel copies remain bounded by the target's logical
// dimensions.
static bool FitsH264CodedGeometry(const Surface &surface,
                                  unsigned int coded_width,
                                  unsigned int coded_height,
                                  unsigned int crop_unit_y) {
  if (crop_unit_y == 0)
    return false;
  const auto fits_axis = [](unsigned int visible, unsigned int coded,
                            unsigned int crop_unit) {
    return visible >= coded ||
           (visible % crop_unit == 0 && Align(visible, 16) == coded);
  };
  return fits_axis(surface.width, coded_width, 2) &&
         fits_axis(surface.height, coded_height, crop_unit_y);
}

static bool EndFencedWrite(Surface *surface, int timeline,
                          const CpuCacheFlushOps &operations = CpuCacheFlush());

static bool AbortFencedWrite(Surface *surface, int timeline,
                            const CpuCacheFlushOps &operations = CpuCacheFlush()) {
  if (timeline < 0)
    return true;
  const bool flushed = surface == nullptr || surface->FlushCpuWrites(operations);
  // No valid picture was committed. Linux sw_sync marks every unsignaled
  // fence -ENOENT when its sole timeline fd closes. Publish that error before
  // CPU END, which may otherwise wait forever on our own imported fence.
  // Never retry close: even on EINTR the fd may already have been recycled.
  const bool closed = close(timeline) == 0;
  const bool released = surface == nullptr || surface->EndCpuWrite();
  return closed && released && flushed;
}

// Begin CPU access before publishing the fence: vaEndPicture has not returned,
// so Chromium cannot yet enqueue a new read of this target. The imported
// unsignaled write fence then protects the whole asynchronous interval.
static int BeginFencedWrite(Surface *surface,
                            const CpuCacheFlushOps &operations = CpuCacheFlush(),
                            bool *synchronous_fallback = nullptr) {
  if (synchronous_fallback != nullptr)
    *synchronous_fallback = false;
  if (surface == nullptr)
    return -1;
  if (surface->object_fds.empty() || !operations.Available()) {
    if (synchronous_fallback != nullptr)
      *synchronous_fallback = true;
    return -1;
  }
  // Establish local fence capability before taking CPU ownership. If sw_sync
  // is unavailable, the synchronous fallback has not touched this target and
  // need not wait on its external readers until the source is actually ready.
  // The fence remains unpublished until CPU START succeeds below.
  int timeline = open(kSwSyncPath, O_RDWR | O_CLOEXEC);
  SwSyncCreateFenceData create = {};
  create.value = 1;
  create.fence = -1;
  snprintf(create.name, sizeof(create.name), "crystalhd-vpp");
  if (timeline < 0 ||
      ioctl(timeline, SW_SYNC_IOC_CREATE_FENCE, &create) != 0) {
    const bool closed = timeline < 0 || close(timeline) == 0;
    if (synchronous_fallback != nullptr)
      *synchronous_fallback = closed;
    return -1;
  }

  if (!surface->BeginCpuWrite()) {
    // BeginCpuWrite already unwound any acquired object prefix. No fence was
    // imported, so close only these local handles; do not issue another END.
    close(create.fence);
    close(timeline);
    return -1;
  }

  bool imported = true;
  for (int fd : surface->object_fds) {
    dma_buf_import_sync_file import = {};
    import.flags = DMA_BUF_SYNC_WRITE;
    import.fd = create.fence;
    if (ioctl(fd, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &import) != 0) {
      imported = false;
      break;
    }
  }
  close(create.fence);
  if (!imported) {
    // An earlier object may already carry the unsignaled fence. Abort it
    // before END can wait on itself; no conversion succeeded here.
    AbortFencedWrite(surface, timeline, operations);
    return -1;
  }
  return timeline;
}

static bool EndFencedWrite(Surface *surface, int timeline,
                          const CpuCacheFlushOps &operations) {
  if (timeline < 0)
    return true;
  if (surface != nullptr && !surface->FlushCpuWrites(operations)) {
    AbortFencedWrite(surface, timeline, operations);
    return false;
  }
  const uint32_t increment = 1;
  if (ioctl(timeline, SW_SYNC_IOC_INC, &increment) != 0) {
    AbortFencedWrite(surface, timeline, operations);
    return false;
  }
  // Once the data is cache-visible it is safe to release CPU ownership. This
  // may briefly wait for the just-released reader, but cannot deadlock on our
  // fence because it has already been signaled.
  const bool released = surface == nullptr || surface->EndCpuWrite();
  close(timeline);
  return released;
}

struct BackingIdentityHash {
  size_t operator()(const BackingIdentity &identity) const {
    const size_t first = std::hash<uint64_t>{}(identity.device);
    const size_t second = std::hash<uint64_t>{}(identity.inode);
    return first ^ (second + 0x9e3779b9U + (first << 6) + (first >> 2));
  }
};

static bool GetBackingIdentity(int fd, BackingIdentity *identity) {
  struct stat info = {};
  if (fd < 0 || identity == nullptr || fstat(fd, &info) != 0)
    return false;
  identity->device = info.st_dev;
  identity->inode = info.st_ino;
  return true;
}

static bool GetPrimeLayout(const VADRMPRIMESurfaceDescriptor &desc,
                           SurfaceLayout *layout) {
  *layout = {};
  layout->fourcc = desc.fourcc == VA_FOURCC_BGRA ? VA_FOURCC_ARGB : desc.fourcc;
  if ((layout->fourcc != VA_FOURCC_NV12 && layout->fourcc != VA_FOURCC_ARGB) ||
      desc.num_objects == 0 || desc.num_objects > 4 ||
      desc.num_layers == 0 || desc.num_layers > 4)
    return false;
  layout->width = desc.width;
  layout->height = desc.height;
  for (unsigned int object = 0; object < desc.num_objects; ++object) {
    BackingIdentity identity;
    if (!GetBackingIdentity(desc.objects[object].fd, &identity))
      return false;
    layout->objects.push_back(identity);
  }
  std::sort(layout->objects.begin(), layout->objects.end(),
            [](const BackingIdentity &a, const BackingIdentity &b) {
              return a.device < b.device || (a.device == b.device && a.inode < b.inode);
            });
  layout->objects.erase(std::unique(layout->objects.begin(), layout->objects.end()),
                        layout->objects.end());
  const unsigned int expected = layout->fourcc == VA_FOURCC_NV12 ? 2 : 1;
  for (unsigned int layer = 0; layer < desc.num_layers; ++layer) {
    if (desc.layers[layer].num_planes > 4)
      return false;
    for (unsigned int plane = 0; plane < desc.layers[layer].num_planes; ++plane) {
      const unsigned int object = desc.layers[layer].object_index[plane];
      if (layout->num_planes >= expected || object >= desc.num_objects)
        return false;
      auto &view = layout->planes[layout->num_planes++];
      if (!GetBackingIdentity(desc.objects[object].fd, &view.backing))
        return false;
      view.offset = desc.layers[layer].offset[plane];
      view.pitch = desc.layers[layer].pitch[plane];
      view.modifier = desc.objects[object].drm_format_modifier;
      if (view.modifier == DRM_FORMAT_MOD_INVALID)
        view.modifier = DRM_FORMAT_MOD_LINEAR;
    }
  }
  layout->valid = layout->num_planes == expected;
  return layout->valid;
}

static bool GetLegacyLayout(const VASurfaceAttribExternalBuffers &desc,
                            SurfaceLayout *layout) {
  *layout = {};
  layout->fourcc = desc.pixel_format == VA_FOURCC_BGRA ? VA_FOURCC_ARGB
                                                     : desc.pixel_format;
  if ((layout->fourcc != VA_FOURCC_NV12 && layout->fourcc != VA_FOURCC_ARGB) ||
      desc.num_buffers != 1 || desc.buffers == nullptr ||
      desc.buffers[0] > static_cast<uintptr_t>(INT_MAX))
    return false;
  layout->width = desc.width;
  layout->height = desc.height;
  BackingIdentity identity;
  if (!GetBackingIdentity(static_cast<int>(desc.buffers[0]), &identity))
    return false;
  layout->objects.push_back(identity);
  layout->num_planes = layout->fourcc == VA_FOURCC_NV12 ? 2 : 1;
  if (desc.num_planes != layout->num_planes)
    return false;
  for (unsigned int plane = 0; plane < layout->num_planes; ++plane) {
    auto &view = layout->planes[plane];
    if (!GetBackingIdentity(static_cast<int>(desc.buffers[0]), &view.backing))
      return false;
    view.offset = desc.offsets[plane];
    view.pitch = desc.pitches[plane];
  }
  layout->valid = true;
  return true;
}

struct VppDeinterlace {
  bool bob = false;
  crystalhd_vpp_bob::FieldParity field =
      crystalhd_vpp_bob::FieldParity::kTop;
};

struct VppColorRange {
  // UNKNOWN retains the historical limited-range YUV interpretation.
  bool source_full = false;
};

static uint8_t ClampRgb(int value) {
  return static_cast<uint8_t>(std::clamp(value, 0, 255));
}

static void StoreArgb(uint8_t *destination, uint8_t y, uint8_t u, uint8_t v) {
  const int c = std::max(0, static_cast<int>(y) - 16);
  const int d = static_cast<int>(u) - 128;
  const int e = static_cast<int>(v) - 128;
  destination[0] = ClampRgb((298 * c + 516 * d + 128) >> 8);
  destination[1] = ClampRgb((298 * c - 100 * d - 208 * e + 128) >> 8);
  destination[2] = ClampRgb((298 * c + 409 * e + 128) >> 8);
  destination[3] = 255;
}

static bool RectFits(const VARectangle &rectangle, const Surface &surface) {
  return rectangle.x >= 0 && rectangle.y >= 0 && rectangle.width != 0 &&
         rectangle.height != 0 &&
         static_cast<unsigned int>(rectangle.x) + rectangle.width <=
             surface.width &&
         static_cast<unsigned int>(rectangle.y) + rectangle.height <=
             surface.height;
}

static VAStatus ProcessVpp(SwsContext **scaler,
                           std::vector<uint8_t> *argb_staging,
                           Surface *source, Surface *destination,
                           const VARectangle &source_region,
                           const VARectangle &output_region,
                           bool destination_write_started = false,
                           const VppDeinterlace &deinterlace = {},
                           bool source_is_field = false,
                           const VppColorRange &color_range = {}) {
  // Only the operation-private first-field view has compact storage. Keep
  // the original full-frame rectangle for field sampling, but validate its
  // physical rows against the real half-height allocation.
  VARectangle source_storage_region = source_region;
  if (source_is_field) {
    if (!deinterlace.bob || source_region.x != 0 || source_region.y != 0 ||
        (source_region.height & 3U) != 0)
      return VA_STATUS_ERROR_INVALID_PARAMETER;
    source_storage_region.height /= 2;
  }
  if (scaler == nullptr || source == nullptr || destination == nullptr ||
      source->fourcc != VA_FOURCC_NV12 ||
      (destination->fourcc != VA_FOURCC_NV12 &&
       destination->fourcc != VA_FOURCC_ARGB) ||
      (destination->fourcc == VA_FOURCC_ARGB && argb_staging == nullptr) ||
      !RectFits(source_storage_region, *source) ||
      !RectFits(output_region, *destination))
    return VA_STATUS_ERROR_INVALID_PARAMETER;
  if (source->pitch[1] < Align(source->width, 2) ||
      (destination->fourcc == VA_FOURCC_NV12 &&
       ((output_region.x | output_region.y | output_region.width |
         output_region.height) & 1U) != 0))
    return VA_STATUS_ERROR_INVALID_PARAMETER;
  size_t argb_staging_pitch = 0;
  if (destination->fourcc == VA_FOURCC_ARGB) {
    // Both ordinary CSC and Bob pass signed-int strides to swscale. Reject
    // unrepresentable strides and staging sizes before acquiring CPU access.
    const unsigned int stride_factor = deinterlace.bob && !source_is_field ? 2U : 1U;
    const uint64_t row_bytes = uint64_t{destination->width} * 4;
    // Give swscale's vectorized row tail private space even when an output
    // rectangle ends at an unaligned surface edge. Never publish that tail.
    const uint64_t padded_row_bytes = (row_bytes + 63U) / 64U * 64U + 64U;
    if (padded_row_bytes > INT_MAX || source->pitch[0] > INT_MAX / stride_factor ||
        source->pitch[1] > INT_MAX / stride_factor ||
        destination->height > std::numeric_limits<size_t>::max() / padded_row_bytes)
      return VA_STATUS_ERROR_INVALID_PARAMETER;
    argb_staging_pitch = static_cast<size_t>(padded_row_bytes);
  }
  if (deinterlace.bob) {
    // An origin-zero visible extent may exclude coded padding, without
    // changing field/chroma phase. Nonzero crop origins remain unsupported.
    if (source == destination || source_region.x != 0 || source_region.y != 0 ||
        output_region.x != 0 || output_region.y != 0 ||
        ((source_region.width | output_region.width) & 1U) != 0 ||
        !crystalhd_vpp_bob::Nv12FrameHeightsValid(source_region.height,
                                                output_region.height))
      return VA_STATUS_ERROR_INVALID_PARAMETER;
    const auto plane_fits = [](const uint8_t *plane, unsigned int pitch,
                               uint64_t rows, uint64_t row_bytes) {
      const uint64_t limit = std::numeric_limits<size_t>::max();
      return plane != nullptr && rows != 0 && row_bytes != 0 &&
             row_bytes <= limit && pitch >= row_bytes &&
             rows - 1 <= (limit - row_bytes) / pitch;
    };
    uint64_t last_source_row = 0;
    const unsigned int source_rows = source_storage_region.height;
    if (!plane_fits(source->planes[0], source->pitch[0], source_rows,
                    source_region.width) ||
        !plane_fits(source->planes[1], source->pitch[1], source_rows / 2,
                    source_region.width) ||
        !crystalhd_vpp_bob::SourcePlaneRow(
            source_region.height, output_region.height, output_region.height - 1,
            deinterlace.field, &last_source_row) ||
        !crystalhd_vpp_bob::SourcePlaneRow(
            source_region.height / 2, output_region.height / 2,
            output_region.height / 2 - 1, deinterlace.field, &last_source_row))
      return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (destination->fourcc == VA_FOURCC_NV12) {
      if (!plane_fits(destination->planes[0], destination->pitch[0],
                      destination->height, destination->width) ||
          !plane_fits(destination->planes[1], destination->pitch[1],
                      (destination->height + 1U) / 2,
                      Align(destination->width, 2)))
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    } else {
      const uint64_t row_bytes = static_cast<uint64_t>(destination->width) * 4;
      if (!plane_fits(destination->planes[0], destination->pitch[0],
                      destination->height, row_bytes))
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
  }
  if (source->expected_timestamp != 0 &&
      source->frame_timestamp != source->expected_timestamp) {
    Debug("refusing mismatched VPP source expected=%llu produced=%llu",
          static_cast<unsigned long long>(source->expected_timestamp),
          static_cast<unsigned long long>(source->frame_timestamp));
    return VA_STATUS_ERROR_DECODING_ERROR;
  }

  if (!source->BeginCpuRead())
    return VA_STATUS_ERROR_OPERATION_FAILED;
  if (destination->fourcc == VA_FOURCC_ARGB) {
    // libswscale used to write scanlines straight into Chromium's imported
    // DMA-BUF. vaEndPicture is asynchronous, so the compositor could sample
    // that buffer halfway through the conversion and display a diagonal or
    // lightning-shaped mix of the old frame and black. Build a complete frame
    // privately, then touch the shared buffer only for the final row copies.
    const size_t staging_pitch = argb_staging_pitch;
    try {
      argb_staging->resize(staging_pitch * destination->height);
    } catch (const std::bad_alloc &) {
      source->EndCpuRead();
      return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    const bool fills_destination =
        output_region.x == 0 && output_region.y == 0 &&
        output_region.width == destination->width &&
        output_region.height == destination->height;
    const unsigned int selected_parity = deinterlace.bob && !source_is_field
        ? static_cast<unsigned int>(deinterlace.field) : 0;
    const unsigned int source_height = deinterlace.bob
        ? source_region.height / 2 : source_region.height;
    const unsigned int source_pitch_factor = deinterlace.bob && !source_is_field ? 2 : 1;
    *scaler = sws_getCachedContext(
        *scaler, source_region.width, source_height, AV_PIX_FMT_NV12,
        output_region.width, output_region.height, AV_PIX_FMT_BGRA, SWS_POINT,
        nullptr, nullptr, nullptr);
    if (*scaler == nullptr) {
      source->EndCpuRead();
      return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    // Range is active even without VAProcColorStandardExplicit. Set it on
    // every operation: getCachedContext may retain a prior full-range input.
    // Keep the existing default matrix; this is not a gamut/transfer change.
    const int *coefficients = sws_getCoefficients(SWS_CS_DEFAULT);
    if (sws_setColorspaceDetails(*scaler, coefficients,
                                color_range.source_full ? 1 : 0,
                                coefficients, 1, 0, 1 << 16, 1 << 16) < 0) {
      source->EndCpuRead();
      return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    const uint8_t *source_planes[4] = {
        source->planes[0] +
            static_cast<size_t>(source_region.y + selected_parity) *
                source->pitch[0] +
            source_region.x,
        source->planes[1] +
            static_cast<size_t>(source_region.y / 2 + selected_parity) *
                source->pitch[1] +
            (source_region.x & ~1U),
        nullptr, nullptr};
    const int source_pitches[4] = {
        static_cast<int>(source->pitch[0] * source_pitch_factor),
        static_cast<int>(source->pitch[1] * source_pitch_factor), 0, 0};
    uint8_t *destination_planes[4] = {
        argb_staging->data() +
            static_cast<size_t>(output_region.y) * staging_pitch +
            static_cast<size_t>(output_region.x) * 4,
        nullptr, nullptr, nullptr};
    const int destination_pitches[4] = {
        static_cast<int>(staging_pitch), 0, 0, 0};
    const int output_lines = sws_scale(
        *scaler, source_planes, source_pitches, 0, source_height,
        destination_planes, destination_pitches);
    if (output_lines != output_region.height) {
      source->EndCpuRead();
      return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (!fills_destination) {
      // A SIMD row tail can extend into the right border. Initialize borders
      // after conversion, not before it, while preserving every active pixel.
      const unsigned int left = static_cast<unsigned int>(output_region.x);
      const unsigned int right = left + output_region.width;
      const unsigned int top = static_cast<unsigned int>(output_region.y);
      const unsigned int bottom = top + output_region.height;
      for (unsigned int row = 0; row < destination->height; ++row) {
        uint8_t *line = argb_staging->data() +
                        static_cast<size_t>(row) * staging_pitch;
        const auto blacken = [line](unsigned int begin, unsigned int end) {
          for (unsigned int column = begin; column < end; ++column) {
            line[column * 4] = 0;
            line[column * 4 + 1] = 0;
            line[column * 4 + 2] = 0;
            line[column * 4 + 3] = 255;
          }
        };
        if (row < top || row >= bottom) {
          blacken(0, destination->width);
        } else {
          blacken(0, left);
          blacken(right, destination->width);
        }
      }
    }
    if (!destination_write_started && !destination->BeginCpuWrite()) {
      source->EndCpuRead();
      return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    for (unsigned int row = 0; row < destination->height; ++row) {
      memcpy(destination->planes[0] +
                 static_cast<size_t>(row) * destination->pitch[0],
             argb_staging->data() + static_cast<size_t>(row) * staging_pitch,
             static_cast<size_t>(destination->width) * 4);
    }
    if (!destination_write_started && !destination->EndCpuWrite()) {
      source->EndCpuRead();
      return VA_STATUS_ERROR_OPERATION_FAILED;
    }
  } else {
    const uint8_t black_luma = color_range.source_full ? 0 : 16;
    if (!destination_write_started && !destination->BeginCpuWrite()) {
      source->EndCpuRead();
      return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (deinterlace.bob) {
      if (output_region.width != destination->width ||
          output_region.height != destination->height) {
        // Bob permits only origin-zero extents. The visible rectangle is
        // overwritten below, so initialize only its right and bottom borders.
        for (unsigned int plane = 0; plane != 2; ++plane) {
          const unsigned int rows = plane == 0
              ? destination->height : (destination->height + 1U) / 2;
          const unsigned int visible_rows = plane == 0
              ? output_region.height : output_region.height / 2;
          const unsigned int row_bytes = plane == 0
              ? destination->width : Align(destination->width, 2);
          if (row_bytes > output_region.width)
            for (unsigned int row = 0; row < visible_rows; ++row)
              memset(destination->planes[plane] +
                         static_cast<size_t>(row) * destination->pitch[plane] +
                         output_region.width,
                     plane == 0 ? black_luma : 128,
                     row_bytes - output_region.width);
          for (unsigned int row = visible_rows; row < rows; ++row)
            memset(destination->planes[plane] +
                       static_cast<size_t>(row) * destination->pitch[plane],
                   plane == 0 ? black_luma : 128, row_bytes);
        }
      }
      // Each plane has its own physical-field row lattice. In particular,
      // selected Y row / 2 is not the selected interlaced NV12 chroma row.
      for (unsigned int plane = 0; plane != 2; ++plane) {
        const unsigned int source_rows = plane == 0
            ? source_region.height : source_region.height / 2;
        const unsigned int target_rows = plane == 0
            ? output_region.height : output_region.height / 2;
        const bool same_width = source_region.width == output_region.width;
        const unsigned int source_samples = plane == 0
            ? source_region.width : source_region.width / 2;
        const unsigned int target_samples = plane == 0
            ? output_region.width : output_region.width / 2;
        // floor(column * source_samples / target_samples), without a wide
        // division per sample. The remainder stays below target_samples;
        // geometry is uint16_t, so each addition fits unsigned int.
        const unsigned int whole = same_width ? 0
            : source_samples / target_samples;
        const unsigned int remainder = same_width ? 0
            : source_samples % target_samples;
        for (unsigned int row = 0; row < target_rows; ++row) {
          uint64_t source_row = 0;
          if (!crystalhd_vpp_bob::SourcePlaneRow(
                  source_rows, target_rows, row, deinterlace.field,
                  &source_row)) {
            if (!destination_write_started)
              destination->EndCpuWrite();
            source->EndCpuRead();
            return VA_STATUS_ERROR_INVALID_PARAMETER;
          }
          if (source_is_field)
            source_row /= 2;
          const uint8_t *input = source->planes[plane] +
              static_cast<size_t>(source_row) * source->pitch[plane];
          uint8_t *output = destination->planes[plane] +
              static_cast<size_t>(row) * destination->pitch[plane];
          if (same_width) {
            memcpy(output, input, output_region.width);
          } else if (plane == 0) {
            unsigned int source_x = 0, phase = 0;
            for (unsigned int column = 0; column < target_samples; ++column) {
              output[column] = input[source_x];
              source_x += whole;
              phase += remainder;
              if (phase >= target_samples) {
                phase -= target_samples;
                ++source_x;
              }
            }
          } else {
            unsigned int source_x = 0, phase = 0;
            for (unsigned int column = 0; column < target_samples; ++column) {
              output[column * 2] = input[source_x * 2];
              output[column * 2 + 1] = input[source_x * 2 + 1];
              source_x += whole;
              phase += remainder;
              if (phase >= target_samples) {
                phase -= target_samples;
                ++source_x;
              }
            }
          }
        }
      }
    } else {
      for (unsigned int row = 0; row < destination->height; ++row)
        memset(destination->planes[0] +
                   static_cast<size_t>(row) * destination->pitch[0],
               black_luma, destination->width);
      for (unsigned int row = 0; row < (destination->height + 1) / 2; ++row)
        memset(destination->planes[1] +
                   static_cast<size_t>(row) * destination->pitch[1],
               128, Align(destination->width, 2));
      for (unsigned int row = 0; row < output_region.height; ++row) {
        const unsigned int source_y = source_region.y +
            static_cast<uint64_t>(row) * source_region.height /
                output_region.height;
        uint8_t *destination_y =
            destination->planes[0] +
            static_cast<size_t>(output_region.y + row) * destination->pitch[0] +
            output_region.x;
        for (unsigned int column = 0; column < output_region.width; ++column) {
          const unsigned int source_x = source_region.x +
              static_cast<uint64_t>(column) * source_region.width /
                  output_region.width;
          destination_y[column] = source->planes[0][
              static_cast<size_t>(source_y) * source->pitch[0] + source_x];
        }
      }
      for (unsigned int row = 0; row < output_region.height; row += 2) {
        const unsigned int source_y = source_region.y +
            static_cast<uint64_t>(row) * source_region.height /
                output_region.height;
        uint8_t *destination_uv =
            destination->planes[1] +
            static_cast<size_t>((output_region.y + row) / 2) *
                destination->pitch[1] + output_region.x;
        for (unsigned int column = 0; column + 1 < output_region.width;
             column += 2) {
          const unsigned int source_x = source_region.x +
              static_cast<uint64_t>(column) * source_region.width /
                output_region.width;
          const size_t source_offset =
              static_cast<size_t>(source_y / 2) * source->pitch[1] +
              (source_x & ~1U);
          destination_uv[column] = source->planes[1][source_offset];
          destination_uv[column + 1] = source->planes[1][source_offset + 1];
        }
      }
    }
    if (!destination_write_started && !destination->EndCpuWrite()) {
      source->EndCpuRead();
      return VA_STATUS_ERROR_OPERATION_FAILED;
    }
  }
  if (!source->EndCpuRead())
    return VA_STATUS_ERROR_OPERATION_FAILED;
  destination->ready = true;
  destination->failed = false;
  destination->expected_timestamp = source->expected_timestamp;
  destination->frame_timestamp = source->frame_timestamp;
  return VA_STATUS_SUCCESS;
}

struct Image {
  VAImage va = {};
};

static bool CopyYuy2ToSurface(Surface *surface,
                              const BC_DTS_PROC_OUT &output, bool is_70012);

struct OutputPollLease {
  explicit OutputPollLease(int descriptor) : fd(descriptor) {}
  ~OutputPollLease() {
    if (fd >= 0)
      close(fd);
  }
  OutputPollLease(const OutputPollLease &) = delete;
  OutputPollLease &operator=(const OutputPollLease &) = delete;

  int fd = -1;
};

struct H264FirstField {
  bool valid = false;
  bool replay_duplicate = false;
  bool bottom = false;
  bool bottom_first = false;
  bool is_70012 = false;
  uint64_t timestamp = 0;
  uint64_t decode_identity = 0;
  uint64_t generation = 0;
  uint32_t picture_number = 0;
  uint32_t sess_num = 0;
  uint32_t picture_meta_payload = 0;
  unsigned int width = 0;
  unsigned int height = 0;
  unsigned int source_pitch = 0;
  std::shared_ptr<Surface> picture;
  std::vector<uint8_t> pixels;
};

// A non-reference intra picture can advance retained H.264 output without
// sealing live input. Its output never owns a public picture or timestamp.
struct H264Auxiliary {
  struct Syntax {
    VAPictureParameterBufferH264 picture{};
    uint64_t generation = 0;
    HANDLE device = nullptr;
  };
  std::map<uint64_t, Syntax> syntax;
  Syntax candidate{};
  uint64_t candidate_timestamp = 0, generation = 0, timestamp = 0;
  HANDLE device = nullptr;
  bool ever = false, pending = false, needs_eos = false;
  bool eos_only = false, ended = false;
};

struct H264ProcessingGeometry {
  bool enabled = false;
  unsigned int visible_width = 0, visible_height = 0;
  unsigned int capture_width = 0, capture_height = 0;

  bool operator==(const H264ProcessingGeometry &other) const {
    return enabled == other.enabled &&
        visible_width == other.visible_width &&
        visible_height == other.visible_height &&
        capture_width == other.capture_width &&
        capture_height == other.capture_height;
  }
};

static bool H264ProcessingSurfaceEligible(const Surface &surface,
                                         VAContextID context, bool original);

struct DecodeContext {
  VAContextID id = VA_INVALID_ID;
  VAConfigID config = VA_INVALID_ID;
  VAProfile profile = VAProfileH264Main;
  uint32_t rt_format = 0;
  unsigned int width = 0;
  unsigned int height = 0;
  unsigned int stream_width = 0;
  unsigned int stream_height = 0;
  bool video_process = false;
  bool decode_processing = false;
  H264ProcessingGeometry processing_geometry;
  H264ProcessingGeometry processing_request;
  VASurfaceID processing_target = VA_INVALID_SURFACE;
  uint64_t processing_target_transaction = 0;
  uint64_t processing_target_identity = 0;
  bool have_processing_parameters = false;
  VASurfaceID target = VA_INVALID_SURFACE;
  std::weak_ptr<Surface> transaction_target;  // Canonical backing owner for VPP.
  uint64_t target_transaction = 0;
  uint64_t target_identity = 0;
  VASurfaceID vpp_source = VA_INVALID_SURFACE;
  std::shared_ptr<Surface> vpp_frame;
  std::shared_ptr<DecodeContext> vpp_decoder;
  uint64_t vpp_generation = 0;
  VARectangle vpp_source_region = {};
  VARectangle vpp_output_region = {};
  VppDeinterlace vpp_deinterlace;
  VppColorRange vpp_color_range;
  bool have_vpp_parameters = false;
  VAPictureParameterBufferH264 picture = {};
  bool have_picture = false;
  bool h264_picture_invalid = false;
  size_t h264_framed_slice_bytes = 0;
  VAIQMatrixBufferH264 iq_matrix = FlatH264IqMatrix();
  bool iq_matrix_valid = true;
  std::vector<VASliceParameterBufferH264> slices;
  std::vector<std::vector<uint8_t>> slice_data;
  struct Mpeg2Picture {
    VAPictureParameterBufferMPEG2 picture = {};
    VAIQMatrixBufferMPEG2 iq = {};
    bool have_picture = false;
    bool have_iq = false;
    bool invalid = false;
    uint64_t forward = 0;
    uint64_t backward = 0;
    size_t bytes = 0;
    std::vector<std::vector<VASliceParameterBufferMPEG2>> parameters;
    std::vector<std::vector<uint8_t>> data;
  } mpeg2_picture;
  crystalhd_mpeg2::QuantMatrices mpeg2_matrices =
      crystalhd_mpeg2::DefaultQuantMatrices();
  struct Mpeg4Picture {
    VAPictureParameterBufferMPEG4 picture = {};
    VAIQMatrixBufferMPEG4 iq = {};
    bool have_picture = false;
    bool have_iq = false;
    bool invalid = false;
    uint64_t forward = 0;
    uint64_t backward = 0;
    size_t bytes = 0;
    std::vector<VASliceParameterBufferMPEG4> parameters;
    std::vector<std::vector<uint8_t>> data;
  } mpeg4_picture;
  crystalhd_mpeg4::TimingState mpeg4_timing;
  struct Vc1Picture {
    VAPictureParameterBufferVC1 picture = {};
    bool have_picture = false;
    bool have_bitplanes = false;
    bool invalid = false;
    uint64_t forward = 0;
    uint64_t backward = 0;
    size_t bytes = 0;
    std::vector<uint8_t> bitplanes;
    std::vector<std::vector<VASliceParameterBufferVC1>> parameters;
    std::vector<std::vector<uint8_t>> data;
  } vc1_picture;
  std::vector<uint32_t> vc1_configuration;
  std::array<uint8_t, 4> wmv3_metadata = {};

  HANDLE device = nullptr;
  bool device_preopened = false;
  std::shared_ptr<OutputPollLease> output_poll;
  std::weak_ptr<OutputPollLease> armed_output_poll;
  bool decoder_open = false;
  bool decoder_started = false;
  bool hardware_close_pending = false;
  BC_STATUS retained_close_status = BC_STS_SUCCESS;
  bool is_70012 = false;
  bool sent_parameter_sets = false;
  // A reference AU rejected before submission leaves the client's reference
  // chain ahead of the hardware. Preserve older output, but admit no dependent
  // input until an actual IDR has committed a new ordered replay/binding.
  bool h264_waiting_for_idr = false;
  // Bounded history is safe for ordinary finite input. Gap tolerance and
  // direct targets remain explicit live-stream policy; prefeed is independent
  // so a finite client can still drain its last picture through sealed EOS.
  bool live_h264 = false;
  bool live_h264_gap_tolerant = false;
  bool live_h264_fast_path = false;
  bool low_latency_h264 = false;
  bool auto_h264_prefeed = false;
  bool input_backpressured = false;
  bool input_budget_exhausted = false;
  uint64_t partial_input_timestamp = 0;
  size_t partial_input_offset = 0;
  bool experimental_ready_remap = false;
  bool experimental_ready_latest = false;
  uint64_t experimental_ready_delay_ns = 0;
  // Prevent later remap choices from repeating or moving behind a payload
  // already delivered by exact sync/export or an experimental commit. This is
  // not a global ordering promise for independently retained VA surfaces.
  uint64_t experimental_last_delivered_ordinal = 0;
  uint64_t prefed_timestamp = 0;
  // Not yet accepted by DIL: never confers timestamp ownership on output.
  uint64_t pending_prefeed_timestamp = 0;
  uint64_t pending_prefeed_generation = 0;
  bool closing = false;
  bool retired = false;
  bool destroy_prepared = false;
  VAStatus destroy_status = VA_STATUS_SUCCESS;
  uint64_t generation = 1;
  uint64_t next_submission_ordinal = 1;
  uint64_t transport_progress = 0;
  // Aggregate only time actually spent waiting in VA sync calls. Wall-clock
  // time between calls can be an intentional player pause and is not EOF.
  uint64_t sync_inactivity_ns = 0;
  uint64_t next_timestamp = kTimestampStep;
  std::unordered_map<uint64_t, VASurfaceID> pending;
  std::unordered_map<uint64_t, std::shared_ptr<Surface>> decoded_frames;
  // Private until a successful consuming close. Close(BUSY) clears ordinary
  // maps, so these exact candidates must survive its explicit retry.
  std::vector<CompletedDecodePicture> retirement_candidates;
  std::shared_ptr<CrystalHDPrivateBackingPool> private_backing_pool;
  H264FirstField h264_first_field;
  H264Auxiliary auxiliary;
  std::deque<std::shared_ptr<Surface>> experimental_ready;
  std::unordered_map<Surface *, uint64_t> surface_timestamps;
  CrystalHDDecodeReplay replay;
  CrystalHDMpeg2Replay mpeg2_replay;
  CrystalHDMpeg4Replay mpeg4_replay;
  std::deque<CrystalHDMpeg4Replay> mpeg4_draining_epochs;
  uint32_t mpeg4_simple_picture_number = 0;
  bool have_mpeg4_simple_picture_number = false;
  CrystalHDVc1Replay vc1_replay;

  bool IsMpeg2() const {
    return profile == VAProfileMPEG2Simple || profile == VAProfileMPEG2Main;
  }
  bool IsMpeg4() const {
    return profile == VAProfileMPEG4Simple ||
           profile == VAProfileMPEG4AdvancedSimple;
  }
  bool IsVc1() const {
    return profile == VAProfileVC1Simple || profile == VAProfileVC1Main ||
           profile == VAProfileVC1Advanced;
  }
  unsigned int StreamWidth() const {
    return IsMpeg2() || IsMpeg4() || IsVc1() ? stream_width : width;
  }
  unsigned int StreamHeight() const {
    return IsMpeg2() || IsMpeg4() || IsVc1() ? stream_height : height;
  }
  unsigned int CaptureWidth() const {
    return processing_geometry.enabled ? processing_geometry.capture_width
                                       : StreamWidth();
  }
  unsigned int CaptureHeight() const {
    return processing_geometry.enabled ? processing_geometry.capture_height
                                       : StreamHeight();
  }
  template<class Function> auto WithReplay(Function function) {
    if (IsMpeg2()) return function(mpeg2_replay);
    if (IsMpeg4()) return function(Mpeg4TransportReplay());
    if (IsVc1()) return function(vc1_replay);
    return function(replay);
  }
  CrystalHDMpeg4Replay &Mpeg4TransportReplay() {
    return mpeg4_draining_epochs.empty()
               ? mpeg4_replay : mpeg4_draining_epochs.front();
  }
  const CrystalHDMpeg4Replay &Mpeg4TransportReplay() const {
    return mpeg4_draining_epochs.empty()
               ? mpeg4_replay : mpeg4_draining_epochs.front();
  }
  size_t Mpeg4CachedPictures() const {
    size_t result = mpeg4_replay.cached_pictures();
    for (const auto &epoch : mpeg4_draining_epochs)
      result += epoch.cached_pictures();
    return result;
  }
  size_t Mpeg4CachedBytes() const {
    size_t result = mpeg4_replay.cached_bytes();
    for (const auto &epoch : mpeg4_draining_epochs)
      result += epoch.cached_bytes();
    return result;
  }
  void RecordTransportProgress() {
    ++transport_progress;
    sync_inactivity_ns = 0;
  }
  void CancelPendingPrefeed() {
    pending_prefeed_timestamp = pending_prefeed_generation = 0;
  }
  bool ReplaySealed() const {
    if (IsVc1()) return vc1_replay.sealed();
    if (IsMpeg4()) return Mpeg4TransportReplay().sealed();
    return IsMpeg2() ? mpeg2_replay.sealed() : replay.sealed();
  }
  bool ReplayFailed() const {
    if (IsVc1()) return vc1_replay.failed();
    if (IsMpeg4()) return Mpeg4TransportReplay().failed();
    return IsMpeg2() ? mpeg2_replay.failed() : replay.failed();
  }
  const char *ReplayFailure() const {
    if (IsVc1()) return vc1_replay.failure();
    if (IsMpeg4()) return Mpeg4TransportReplay().failure();
    return IsMpeg2() ? mpeg2_replay.failure() : replay.failure();
  }

  ~DecodeContext() { Close(); }

  enum class HardwareClosePurpose { Device, H264Replay };

  BC_STATUS CloseHardware(uint64_t timeout_ns = kDecodeTimeoutNs) {
    return CloseHardwareImpl(timeout_ns, HardwareClosePurpose::Device);
  }

  BC_STATUS CloseHardwareForH264Replay(uint64_t timeout_ns = kDecodeTimeoutNs) {
    return CloseHardwareImpl(timeout_ns, HardwareClosePurpose::H264Replay);
  }

  BC_STATUS CloseHardwareImpl(uint64_t timeout_ns, HardwareClosePurpose purpose) {
    // A successful EOS has retired every hardware output before this private
    // replay restart. Completed pictures own copied pixels, not RX leases.
    // Never infer this contract from a version number or another optional API.
    const bool retain_device = purpose == HardwareClosePurpose::H264Replay &&
        IsH264Profile(profile) && !is_70012 && !live_h264_fast_path &&
        device != nullptr && decoder_open && decoder_started &&
        !closing && !retired && !destroy_prepared && !hardware_close_pending &&
        retained_close_status == BC_STS_SUCCESS && replay.NeedsRestart() &&
        replay.outstanding() == 0 && !h264_first_field.valid &&
        partial_input_timestamp == 0 && partial_input_offset == 0 &&
        prefed_timestamp == 0 && pending_prefeed_timestamp == 0 &&
        SupportsDrainedH264ChannelReopen();
    CancelPendingPrefeed();
    BC_STATUS result = retained_close_status;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::nanoseconds(timeout_ns);
    const auto clear_abort_state = [&] {
      if (auxiliary.ever)
        Debug("H.264 auxiliary close token=%llu pending=%d needs_eos=%d ended=%d",
                     static_cast<unsigned long long>(auxiliary.timestamp),
                     auxiliary.pending, auxiliary.needs_eos, auxiliary.ended);
      auxiliary = H264Auxiliary();
      prefed_timestamp = 0;
      mpeg4_simple_picture_number = 0;
      have_mpeg4_simple_picture_number = false;
      input_backpressured = false;
      input_budget_exhausted = false;
      partial_input_timestamp = 0;
      partial_input_offset = 0;
      sync_inactivity_ns = 0;
      h264_first_field = H264FirstField();
    };
    prefed_timestamp = 0;
    mpeg4_simple_picture_number = 0;
    have_mpeg4_simple_picture_number = false;
    bool lifecycle_busy = false;
    if (decoder_started) {
      BC_STATUS status;
      do {
        status = DtsStopDecoder(device);
        if (status != BC_STS_BUSY)
          break;
        if (std::chrono::steady_clock::now() >= deadline)
          break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      } while (true);
      if (status == BC_STS_SUCCESS)
        decoder_started = false;
      else if (status == BC_STS_BUSY)
        lifecycle_busy = true;
      else if (result == BC_STS_SUCCESS)
        result = status;
    }
    if (decoder_open && !lifecycle_busy) {
      BC_STATUS status;
      do {
        status = DtsCloseDecoder(device);
        if (status != BC_STS_BUSY)
          break;
        if (std::chrono::steady_clock::now() >= deadline)
          break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      } while (true);
      if (status == BC_STS_SUCCESS) {
        decoder_open = false;
        decoder_started = false;
      } else if (status == BC_STS_BUSY) {
        lifecycle_busy = true;
      } else if (result == BC_STS_SUCCESS) {
        result = status;
      }
    }
    if (retain_device && !lifecycle_busy && result == BC_STS_SUCCESS &&
        !decoder_open && !decoder_started) {
      // Both STOP and CLOSE succeeded independently. Recreate the firmware
      // channel/DPB, not the device or its TX worker. Failed/BUSY teardown still
      // falls through to the original full-device cleanup below.
      hardware_close_pending = false;
      retained_close_status = BC_STS_SUCCESS;
      armed_output_poll.reset();
      output_poll.reset();
      clear_abort_state();
      device_preopened = true;
      return BC_STS_SUCCESS;
    }
    if (device != nullptr) {
      BC_STATUS status = BC_STS_SUCCESS;
      do {
        status = DtsDeviceClose(device);
        if (status != BC_STS_BUSY)
          break;
        if (std::chrono::steady_clock::now() >= deadline)
          break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      } while (status == BC_STS_BUSY);
      // For the valid same-process handles owned by this backend, BUSY is the
      // sole non-consuming DeviceClose result. Preserve both the handle and
      // its duplicated output-poll ownership so this exact close can be
      // retried. The timeout bounds only our retry window; one library call
      // can still block while it joins the TX worker.
      if (status == BC_STS_BUSY) {
        hardware_close_pending = true;
        retained_close_status = result;
        clear_abort_state();
        return BC_STS_BUSY;
      }
      if (result == BC_STS_SUCCESS)
        result = status;
      device = nullptr;
      decoder_started = false;
      decoder_open = false;
    }
    hardware_close_pending = false;
    device_preopened = false;
    retained_close_status = BC_STS_SUCCESS;
    armed_output_poll.reset();
    output_poll.reset();
    clear_abort_state();
    return result;
  }

  BC_STATUS Close(uint64_t timeout_ns = kDecodeTimeoutNs) {
    const BC_STATUS status = CloseHardware(timeout_ns);
    pending.clear();
    for (const auto &picture : experimental_ready)
      picture->experimental_ready_queued = false;
    experimental_ready.clear();
    experimental_last_delivered_ordinal = 0;
    decoded_frames.clear();
    // A logical reset starts a new cache lifetime. Older externally retained
    // pictures only have weak links and cannot return bytes to the new epoch.
    private_backing_pool.reset();
    surface_timestamps.clear();
    replay = CrystalHDDecodeReplay(live_h264);
    mpeg2_replay = CrystalHDMpeg2Replay();
    mpeg2_picture = Mpeg2Picture();
    mpeg2_matrices = crystalhd_mpeg2::DefaultQuantMatrices();
    mpeg4_replay = CrystalHDMpeg4Replay();
    mpeg4_draining_epochs.clear();
    mpeg4_picture = Mpeg4Picture();
    mpeg4_timing = crystalhd_mpeg4::TimingState();
    vc1_replay = CrystalHDVc1Replay();
    vc1_picture = Vc1Picture();
    vc1_configuration.clear();
    wmv3_metadata = {};
    stream_width = stream_height = 0;
    processing_geometry = {};
    processing_request = {};
    processing_target = VA_INVALID_SURFACE;
    processing_target_transaction = processing_target_identity = 0;
    have_processing_parameters = false;
    vpp_frame.reset();
    vpp_decoder.reset();
    sent_parameter_sets = false;
    h264_waiting_for_idr = false;
    return status;
  }

  BC_STATUS Reset(uint64_t timeout_ns = kDecodeTimeoutNs) {
    retirement_candidates.clear();
    ++generation;
    return Close(timeout_ns);
  }

};

struct PendingVpp {
  std::shared_ptr<DecodeContext> decoder;
  std::shared_ptr<Surface> source;
  std::shared_ptr<Surface> target;
  std::shared_ptr<Surface> target_owner;
  VARectangle source_region = {};
  VARectangle output_region = {};
  VppDeinterlace deinterlace;
  VppColorRange color_range;
  uint64_t source_timestamp = 0;
  uint64_t sequence = 0;
  uint64_t decoder_generation = 0;
  int write_timeline = -1;
  bool worker_active = false;
};

struct Driver;
static void RunDecodeWorker(Driver *driver);
static void RunVppWorker(Driver *driver);

struct Driver {
  std::mutex mutex;
  std::condition_variable condition;
  uint32_t next_config = 1;
  uint32_t next_surface = 1;
  uint32_t next_context = 1;
  uint32_t next_buffer = 1;
  uint32_t next_image = 1;
  uint64_t next_vpp_sequence = 1;
  uint64_t next_decode_identity = 1;
  uint64_t next_trace_call = 1;
  crystalhd_vaapi_trace::Trace trace;
  std::unordered_map<VAConfigID, Config> configs;
  std::unordered_map<VASurfaceID, std::shared_ptr<Surface>> surfaces;
  std::unordered_map<VAContextID, std::shared_ptr<DecodeContext>> contexts;
  std::unordered_map<VABufferID, Buffer> buffers;
  std::unordered_map<VAImageID, Image> images;
  std::unordered_map<BackingIdentity, VASurfaceID, BackingIdentityHash>
      backing_owners;
  std::deque<PendingVpp> pending_vpp;
  SwsContext *vpp_scaler = nullptr;
  std::vector<uint8_t> vpp_argb_staging;
  bool stopping = false;
  std::thread decode_worker;
  std::thread vpp_worker;
  int decode_control_fd = -1;
  gbm_device *gbm = nullptr;
  int drm_fd = -1;
  PrimaryDrmNodeCache primary_node_cache;
  // Borrowed capability metadata, stable until this VA display terminates.
  std::array<uint32_t, 1> vpp_input_formats = {{VA_FOURCC_NV12}};
  std::array<uint32_t, 2> vpp_output_formats = {{VA_FOURCC_NV12, VA_FOURCC_ARGB}};
  bool experimental_i915_wc = false;

  explicit Driver(int requested_drm_fd,
                  int trace_output_fd = STDERR_FILENO)
      : trace(trace_output_fd), drm_fd(requested_drm_fd),
        experimental_i915_wc(ExperimentalI915WcRequested(
            getenv("CRYSTALHD_VAAPI_EXPERIMENTAL_I915_WC"))) {
    decode_control_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (requested_drm_fd >= 0)
      gbm = gbm_create_device(requested_drm_fd);
    vpp_worker = std::thread(RunVppWorker, this);
  }
  void StartDecodeWorker();
  void StopWorkers() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      stopping = true;
    }
    NotifyDecodeWorker();
    if (decode_worker.joinable())
      decode_worker.join();
    if (vpp_worker.joinable())
      vpp_worker.join();
  }
  ~Driver() {
    StopWorkers();
    if (decode_control_fd >= 0) {
      close(decode_control_fd);
      decode_control_fd = -1;
    }
    for (const PendingVpp &pending : pending_vpp)
      AbortFencedWrite(pending.target.get(), pending.write_timeline);
    if (vpp_scaler != nullptr)
      sws_freeContext(vpp_scaler);
    contexts.clear();
    pending_vpp.clear();
    surfaces.clear();
    // Image exports may own GBM objects too; destroy them before their device.
    images.clear();
    buffers.clear();
    if (gbm != nullptr)
      gbm_device_destroy(gbm);
    trace.Emit(crystalhd_vaapi_trace::Event::SessionDestroy);
  }
  void NotifyDecodeWorker() {
    if (decode_control_fd >= 0) {
      const uint64_t increment = 1;
      ssize_t result;
      do {
        result = write(decode_control_fd, &increment, sizeof(increment));
      } while (result < 0 && errno == EINTR);
    }
    condition.notify_all();
  }
};

static uint64_t TraceId(uint32_t id, uint32_t invalid) {
  return id == invalid ? 0 : id;
}

static uint64_t TraceOwner(const Surface *surface) {
  if (surface == nullptr)
    return 0;
  return surface->backing_owner == VA_INVALID_SURFACE
             ? TraceId(surface->id, VA_INVALID_SURFACE)
             : TraceId(surface->backing_owner, VA_INVALID_SURFACE);
}

static crystalhd_vaapi_trace::Fields TraceFields(
    const DecodeContext *decode, const Surface *surface, uint64_t token = 0) {
  crystalhd_vaapi_trace::Fields fields;
  if (decode != nullptr) {
    fields.context = TraceId(decode->id, VA_INVALID_ID);
    fields.generation = decode->generation;
  }
  if (surface != nullptr) {
    if (decode == nullptr) {
      fields.context = surface->trace_context;
      fields.generation = surface->trace_generation;
    }
    fields.surface = TraceId(surface->id, VA_INVALID_SURFACE);
    fields.token = token != 0 ? token : surface->expected_timestamp;
    fields.decode_identity = surface->trace_decode_identity;
    fields.owner = TraceOwner(surface);
    fields.submission_ordinal = surface->trace_submission_ordinal;
    if (surface->experimental_presentation) {
      fields.payload_token = surface->presentation_payload_token;
      fields.payload_decode_identity =
          surface->presentation_payload_decode_identity;
      fields.payload_submission_ordinal =
          surface->presentation_payload_submission_ordinal;
      if (fields.submission_ordinal >= fields.payload_submission_ordinal)
        fields.submission_drift = static_cast<int64_t>(
            fields.submission_ordinal -
            fields.payload_submission_ordinal);
    }
  } else {
    fields.token = token;
  }
  return fields;
}

static crystalhd_vaapi_trace::Fields TraceDecodeTokenFields(
    Driver *driver, DecodeContext *decode, uint64_t token) {
  // A public VA surface can be rebound while an older AU is still queued or
  // its output is still in flight. The per-token decoded picture is immutable
  // across that reuse, so it is the authoritative trace identity.
  const auto decoded = decode->decoded_frames.find(token);
  if (decoded != decode->decoded_frames.end())
    return TraceFields(decode, decoded->second.get(), token);

  // A pending public binding is only a fallback when its complete ownership
  // tuple still resolves to the exact picture. Never label an old token with
  // whatever newer identity happens to occupy the same VASurfaceID.
  const auto pending = decode->pending.find(token);
  if (pending != decode->pending.end()) {
    const auto found = driver->surfaces.find(pending->second);
    if (found != driver->surfaces.end()) {
      const std::shared_ptr<Surface> picture =
          found->second->decode_picture.lock();
      const auto timestamp =
          decode->surface_timestamps.find(found->second.get());
      if (picture && found->second->expected_timestamp == token &&
          timestamp != decode->surface_timestamps.end() &&
          timestamp->second == token && picture->expected_timestamp == token &&
          picture->decode_identity == found->second->decode_identity &&
          picture->trace_decode_identity ==
              found->second->trace_decode_identity)
        return TraceFields(decode, picture.get(), token);
    }
  }
  return TraceFields(decode, nullptr, token);
}

static crystalhd_vaapi_trace::Fields TraceRequestedOwnerFields(
    const Surface *requested, const Surface *owner) {
  crystalhd_vaapi_trace::Fields fields = TraceFields(nullptr, owner);
  fields.surface = requested == nullptr
                       ? 0 : TraceId(requested->id, VA_INVALID_SURFACE);
  return fields;
}

static bool SameTraceBinding(const crystalhd_vaapi_trace::Fields &left,
                             const crystalhd_vaapi_trace::Fields &right) {
  return left.context == right.context &&
      left.generation == right.generation &&
      left.surface == right.surface && left.token == right.token &&
      left.decode_identity == right.decode_identity &&
      left.owner == right.owner &&
      left.submission_ordinal == right.submission_ordinal;
}

static void ClearExperimentalReadExport(Surface *surface) {
  if (surface == nullptr)
    return;
  surface->experimental_read_export_armed = false;
  surface->experimental_read_export = {};
}

static void ClearDiagnosticTraceReadExport(Surface *surface) {
  if (surface == nullptr)
    return;
  surface->trace_read_export_armed = false;
  surface->trace_read_export = {};
}

static void ClearTraceReadExport(Surface *surface) {
  if (surface == nullptr)
    return;
  ClearDiagnosticTraceReadExport(surface);
  ClearExperimentalReadExport(surface);
}

static void ClearExperimentalPresentation(Surface *surface);

static void ClearTraceReadExportsForContext(Driver *driver,
                                            uint64_t context,
                                            uint64_t generation) {
  for (const auto &entry : driver->surfaces) {
    Surface *surface = entry.second.get();
    const bool traced = surface->trace_read_export_armed &&
        surface->trace_read_export.context == context &&
        surface->trace_read_export.generation == generation;
    const bool experimental = surface->experimental_read_export_armed &&
        surface->experimental_read_export.context == context &&
        surface->experimental_read_export.generation == generation;
    if (traced) {
      surface->trace_read_export_armed = false;
      surface->trace_read_export = {};
    }
    const bool presentation = surface->experimental_presentation &&
        surface->trace_context == context &&
        surface->trace_generation == generation;
    const bool reservation = surface->experimental_write_lease &&
        surface->experimental_write_context == context &&
        surface->experimental_write_generation == generation;
    // Context retirement is not an external GPU-reader completion signal.
    // Keep an exported binding and its presentation frozen until the VA
    // surface itself is destroyed. An unexported presentation and a
    // Begin-to-submit reservation have no external reader and can be canceled.
    if ((presentation && !experimental) || reservation)
      ClearExperimentalPresentation(surface);
  }
}

static void ClearTraceIdentity(Surface *surface) {
  if (surface == nullptr)
    return;
  ClearTraceReadExport(surface);
  surface->trace_context = 0;
  surface->trace_generation = 0;
  surface->trace_submission_ordinal = 0;
  surface->trace_decode_identity = 0;
  surface->experimental_submitted_at_ns = 0;
}

static void EndTraceBinding(Driver *driver, Surface *surface) {
  if (surface == nullptr)
    return;
  if (driver->trace.enabled() && surface->trace_submission_ordinal != 0)
    driver->trace.Emit(crystalhd_vaapi_trace::Event::BindingEnd,
                       TraceFields(nullptr, surface));
  ClearTraceIdentity(surface);
}

static void ClearExperimentalPresentation(Surface *surface) {
  if (surface == nullptr)
    return;
  surface->experimental_presentation = false;
  surface->experimental_write_lease = false;
  surface->experimental_write_context = 0;
  surface->experimental_write_generation = 0;
  surface->experimental_write_submission_ordinal = 0;
  surface->experimental_presentation_at_ns = 0;
  surface->presentation_payload_token = 0;
  surface->presentation_payload_decode_identity = 0;
  surface->presentation_payload_submission_ordinal = 0;
}

static bool ExperimentalWriteReservationPending(const Surface *surface) {
  return surface != nullptr && surface->experimental_write_lease &&
      surface->experimental_write_submission_ordinal != 0 &&
      (surface->experimental_write_context != surface->trace_context ||
       surface->experimental_write_generation != surface->trace_generation ||
       surface->experimental_write_submission_ordinal !=
           surface->trace_submission_ordinal);
}

static void RetireFailedExperimentalState(Surface *surface) {
  if (surface == nullptr)
    return;
  surface->trace_read_export_armed = false;
  surface->trace_read_export = {};
  // A decode failure does not prove that an already exported GPU reader has
  // finished. Preserve that immutable backing and its guard; without an
  // exported reader the experimental state can be canceled immediately.
  if (!surface->experimental_read_export_armed) {
    ClearExperimentalReadExport(surface);
    ClearExperimentalPresentation(surface);
  }
}

static bool ExperimentalPresentationReadable(const Driver *driver,
                                             const Surface *surface) {
  if (driver == nullptr || surface == nullptr ||
      !surface->experimental_presentation || surface->trace_context == 0)
    return false;
  const auto found = driver->contexts.find(
      static_cast<VAContextID>(surface->trace_context));
  return found != driver->contexts.end() &&
      found->second->experimental_ready_remap &&
      !found->second->closing && !found->second->retired &&
      found->second->generation == surface->trace_generation &&
      !surface->destroyed && !surface->failed &&
      surface->expected_timestamp != 0 &&
      surface->presentation_payload_token != 0 &&
      surface->trace_decode_identity != 0 &&
      surface->presentation_payload_decode_identity != 0 &&
      surface->trace_submission_ordinal != 0 &&
      surface->presentation_payload_submission_ordinal <
          surface->trace_submission_ordinal &&
      (found->second->experimental_ready_latest ||
       surface->presentation_payload_submission_ordinal + 1 ==
           surface->trace_submission_ordinal) &&
      surface->experimental_submitted_at_ns != 0 &&
      surface->experimental_presentation_at_ns >=
          surface->experimental_submitted_at_ns &&
      surface->experimental_presentation_at_ns -
              surface->experimental_submitted_at_ns >=
          found->second->experimental_ready_delay_ns;
}

static uint64_t NextTraceCall(Driver *driver) {
  uint64_t call = driver->next_trace_call++;
  if (call == 0)
    call = driver->next_trace_call++;
  return call;
}

static bool ExperimentalPresentationBinding(Driver *driver,
                                            const Surface *surface) {
  if (surface == nullptr || surface->trace_context == 0)
    return false;
  const auto found = driver->contexts.find(
      static_cast<VAContextID>(surface->trace_context));
  return found != driver->contexts.end() &&
      found->second->experimental_ready_remap &&
      found->second->generation == surface->trace_generation;
}

static void ArmTraceReadExport(Driver *driver, Surface *requested,
                               Surface *owner, uint64_t export_operation) {
  if (requested == nullptr || owner == nullptr || requested != owner)
    return;
  const bool trace = driver->trace.enabled();
  const bool experimental = ExperimentalPresentationBinding(driver, owner);
  if (!trace && !experimental)
    return;
  // Refresh each arm only when this export owns that arm.  A trace-only
  // re-export after context retirement must not revoke the independent
  // external-reader guard retained by the experimental lifetime protocol.
  if (trace)
    ClearDiagnosticTraceReadExport(owner);
  if (experimental)
    ClearExperimentalReadExport(owner);
  if (owner->destroyed || owner->failed ||
      owner->backing_owner != VA_INVALID_SURFACE ||
      owner->trace_context == 0 || owner->trace_generation == 0 ||
      owner->trace_submission_ordinal == 0 ||
      owner->trace_decode_identity == 0)
    return;
  const crystalhd_vaapi_trace::Fields binding = TraceFields(nullptr, owner);
  if (trace) {
    owner->trace_read_export = binding;
    owner->trace_read_export.operation = export_operation;
    owner->trace_read_export_armed = true;
  }
  if (experimental) {
    owner->experimental_read_export = binding;
    owner->experimental_read_export.operation = export_operation;
    owner->experimental_read_export_armed = true;
  }
}

struct ReadExportReuseProbe {
  bool experimental_armed = false;
  int64_t outcome = 0;
};

static ReadExportReuseProbe ProbeTraceReadExportReuse(
    Driver *driver, DecodeContext *decode, Surface *requested,
    Surface *owner) {
  ReadExportReuseProbe result;
  if (owner == nullptr)
    return result;
  const bool trace_armed = owner->trace_read_export_armed;
  result.experimental_armed = owner->experimental_read_export_armed;
  if (!trace_armed && !result.experimental_armed)
    return result;

  const crystalhd_vaapi_trace::Fields armed = result.experimental_armed
      ? owner->experimental_read_export : owner->trace_read_export;
  const crystalhd_vaapi_trace::Fields current = TraceFields(nullptr, owner);
  const bool same_binding = requested == owner && !owner->destroyed &&
      !owner->failed && decode != nullptr &&
      armed.surface == TraceId(requested->id, VA_INVALID_SURFACE) &&
      armed.owner == TraceOwner(owner) &&
      armed.context == TraceId(decode->id, VA_INVALID_ID) &&
      armed.context == current.context &&
      armed.generation == decode->generation &&
      armed.generation == current.generation &&
      armed.token == current.token &&
      armed.decode_identity == current.decode_identity &&
      armed.submission_ordinal == current.submission_ordinal;
  // The diagnostic record is one-shot.  The experimental ownership arm is
  // different: retain it across busy/error retries and release it only after
  // every exported backing is observed writable.  Otherwise a second
  // vaBeginPicture could skip the failed probe and overwrite a live reader.
  if (trace_armed) {
    owner->trace_read_export_armed = false;
    owner->trace_read_export = {};
  }
  if (!same_binding) {
    if (result.experimental_armed)
      result.outcome = -ESTALE;
    return result;
  }

  crystalhd_vaapi_trace::Fields fields = armed;
  const auto start = std::chrono::steady_clock::now();
  std::unordered_set<BackingIdentity, BackingIdentityHash> objects;
  const auto record_error = [&](int error) {
    ++fields.probe_error_count;
    if (fields.probe_errno == 0)
      fields.probe_errno = error != 0 ? error : EIO;
  };
  const auto probe_fd = [&](int fd) {
    BackingIdentity identity;
    if (!GetBackingIdentity(fd, &identity)) {
      record_error(errno);
      return;
    }
    if (!objects.insert(identity).second)
      return;
    ++fields.identified_exported_object_count;
    // This diagnostic/safety call runs while the driver mutex is held. Timeout zero
    // does not wait for fence signaling, but poll may sleep while acquiring a
    // contended dma_resv lock; duration_ns captures that perturbation. POLLOUT
    // describes only this client-reuse instant, not reader completion.
    pollfd descriptor = {fd, POLLOUT, 0};
    errno = 0;
    const int result = poll(&descriptor, 1, 0);
    if (result < 0) {
      record_error(errno);
    } else if (result == 0) {
      ++fields.not_pollout_count;
    } else if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
      record_error(EIO);
    } else if ((descriptor.revents & POLLOUT) != 0) {
      ++fields.pollout_count;
    } else {
      ++fields.not_pollout_count;
    }
  };

  if (owner->bo != nullptr) {
    const int exported_plane_count =
        owner->fourcc == VA_FOURCC_ARGB ? 1 :
        owner->fourcc == VA_FOURCC_NV12 ? 2 : 0;
    errno = 0;
    const int available_plane_count = gbm_bo_get_plane_count(owner->bo);
    if (exported_plane_count == 0 ||
        available_plane_count < exported_plane_count) {
      record_error(errno);
    } else {
      // Mirror ExportSurfaceHandle exactly: ARGB exposes plane 0, while NV12
      // exposes planes 0 and 1. Ignore any auxiliary GBM planes not exported
      // to the client whose reuse boundary this record describes.
      for (int plane = 0; plane < exported_plane_count; ++plane) {
        errno = 0;
        const int fd = gbm_bo_get_fd_for_plane(owner->bo, plane);
        if (fd < 0) {
          record_error(errno);
          continue;
        }
        probe_fd(fd);
        close(fd);
      }
    }
  } else {
    if (owner->object_fds.empty())
      record_error(ENODEV);
    else if (!owner->layout.valid)
      probe_fd(owner->object_fds.front());
    else {
      // Mapped imports can export several independent plane objects. Match
      // the registered export view, ignoring unrelated cached descriptors
      // and probing each actual backing only once.
      for (int fd : owner->object_fds) {
        BackingIdentity identity;
        if (GetBackingIdentity(fd, &identity) &&
            std::find(owner->layout.objects.begin(), owner->layout.objects.end(),
                       identity) != owner->layout.objects.end())
          probe_fd(fd);
      }
      if (objects.size() != owner->layout.objects.size())
        record_error(ENODEV);
    }
  }
  fields.duration_ns = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - start).count());
  if (fields.probe_error_count != 0)
    fields.outcome = -fields.probe_errno;
  else if (fields.identified_exported_object_count == 0)
    fields.outcome = -ENODEV;
  else if (fields.not_pollout_count != 0)
    fields.outcome = -EAGAIN;
  else if (fields.pollout_count ==
           fields.identified_exported_object_count)
    fields.outcome = 0;
  else
    fields.outcome = -EIO;
  result.outcome = fields.outcome;
  if (result.experimental_armed && result.outcome == 0)
    ClearExperimentalReadExport(owner);
  if (trace_armed && driver->trace.enabled())
    driver->trace.Emit(
        crystalhd_vaapi_trace::Event::ReadExportReuseImplicitFenceProbe,
        fields);
  return result;
}

static void InheritTraceIdentity(Surface *destination,
                                 const Surface &source) {
  if (destination == nullptr)
    return;
  ClearTraceReadExport(destination);
  destination->trace_context = source.trace_context;
  destination->trace_generation = source.trace_generation;
  destination->trace_submission_ordinal = source.trace_submission_ordinal;
  destination->trace_decode_identity = source.trace_decode_identity;
  destination->experimental_submitted_at_ns =
      source.experimental_submitted_at_ns;
}

static uint64_t NextDecodeIdentity(Driver *driver) {
  uint64_t identity = driver->next_decode_identity++;
  if (identity == 0)
    identity = driver->next_decode_identity++;
  return identity;
}

static void RevokeDecodeOwner(Driver *driver, Surface *surface) {
  if (surface == nullptr)
    return;
  ClearTraceReadExport(surface);
  surface->completed_decode_picture = {};
  surface->decode_picture.reset();
  surface->decode_identity = NextDecodeIdentity(driver);
}

static Surface *BackingOwner(Driver *driver, Surface *surface) {
  if (surface == nullptr || surface->backing_owner == VA_INVALID_SURFACE)
    return surface;
  auto owner = driver->surfaces.find(surface->backing_owner);
  return owner != driver->surfaces.end() ? owner->second.get() : surface;
}

static bool HasLiveBackingOwner(const Driver *driver, const Surface *surface) {
  if (surface == nullptr || surface->destroyed)
    return false;
  if (surface->backing_owner == VA_INVALID_SURFACE)
    return true;
  const auto owner = driver->surfaces.find(surface->backing_owner);
  // A queued operation can retain the removed owner's unsignaled fence.
  // Falling back to an alias's cached readiness would both misreport status
  // and let CPU access wait on that fence while holding the worker's mutex.
  return owner != driver->surfaces.end() && !owner->second->destroyed;
}

static bool RegisterExportedLayout(Driver *driver, Surface *surface,
                                   VASurfaceID id, const SurfaceLayout &layout) {
  Surface *owner = BackingOwner(driver, surface);
  const VASurfaceID owner_id = surface->backing_owner == VA_INVALID_SURFACE
                                   ? id : surface->backing_owner;
  if (owner->layout.valid && !(owner->layout == layout))
    return false;
  for (const BackingIdentity &identity : layout.objects) {
    const auto known = driver->backing_owners.find(identity);
    if (known != driver->backing_owners.end() && known->second != owner_id)
      return false;
  }
  for (const BackingIdentity &identity : layout.objects)
    driver->backing_owners[identity] = owner_id;
  surface->layout = layout;
  owner->layout = layout;
  return true;
}

static void SetSurfaceState(Driver *driver, Surface *surface, bool ready,
                            bool failed) {
  if (surface == nullptr)
    return;
  if (failed)
    RetireFailedExperimentalState(surface);
  surface->ready = ready;
  surface->failed = failed;
  Surface *owner = BackingOwner(driver, surface);
  if (owner != surface) {
    if (failed)
      RetireFailedExperimentalState(owner);
    owner->ready = ready;
    owner->failed = failed;
    if (surface->expected_timestamp != 0 || surface->frame_timestamp != 0) {
      owner->expected_timestamp = surface->expected_timestamp;
      owner->frame_timestamp = surface->frame_timestamp;
    } else {
      surface->expected_timestamp = owner->expected_timestamp;
      surface->frame_timestamp = owner->frame_timestamp;
    }
  }
}

static Driver *GetDriver(VADriverContextP context) {
  return static_cast<Driver *>(context->pDriverData);
}

static bool IsH264Profile(VAProfile profile) {
  return profile == VAProfileH264ConstrainedBaseline ||
         profile == VAProfileH264Main || profile == VAProfileH264High;
}

static bool IsMpeg2Profile(VAProfile profile) {
  return profile == VAProfileMPEG2Simple || profile == VAProfileMPEG2Main;
}

static bool IsMpeg4Profile(VAProfile profile) {
  return profile == VAProfileMPEG4Simple ||
         profile == VAProfileMPEG4AdvancedSimple;
}

static bool IsVc1Profile(VAProfile profile) {
  return profile == VAProfileVC1Simple || profile == VAProfileVC1Main ||
         profile == VAProfileVC1Advanced;
}

static bool IsDecodeProfile(VAProfile profile) {
  return IsH264Profile(profile) || IsMpeg2Profile(profile) ||
         IsMpeg4Profile(profile) || IsVc1Profile(profile);
}

static bool IsSupportedProfile(VAProfile profile) {
  return IsDecodeProfile(profile) || profile == VAProfileNone;
}

static bool IsValidConfig(VAProfile profile, VAEntrypoint entrypoint) {
  return (IsDecodeProfile(profile) && entrypoint == VAEntrypointVLD) ||
         (profile == VAProfileNone && entrypoint == VAEntrypointVideoProc);
}

static bool ShouldAutoPrefeedH264(const DecodeContext &decode) {
  return decode.auto_h264_prefeed && IsH264Profile(decode.profile) &&
         !decode.live_h264_fast_path && decode.device != nullptr &&
         decode.decoder_started && !decode.is_70012 &&
         (decode.picture.seq_fields.bits.frame_mbs_only_flag ||
          decode.picture.seq_fields.bits.mb_adaptive_frame_field_flag) &&
         !decode.picture.pic_fields.bits.field_pic_flag;
}

static VAStatus OpenDevice(DecodeContext *decode) {
  const uint32_t mode = DTS_PLAYBACK_MODE | DTS_LOAD_FILE_PLAY_FW |
                  DTS_SKIP_TX_CHK_CPB | DTS_PLAYBACK_DROP_RPT_MODE |
                  DTS_SINGLE_THREADED_MODE |
                  DTS_DFLT_RESOLUTION(vdecRESOLUTION_1080p23_976);
  HANDLE opened = nullptr;
  const BC_STATUS status = DtsDeviceOpen(&opened, mode);
  Debug("DtsDeviceOpen: %d", status);
  if (status != BC_STS_SUCCESS) {
    // ERR_USAGE can also be propagated from unrelated initialization failure;
    // only the library's unambiguous ownership statuses mean hardware busy.
    if (status == BC_STS_BUSY || status == BC_STS_DEC_EXIST_OPEN)
      return VA_STATUS_ERROR_HW_BUSY;
    return VA_STATUS_ERROR_OPERATION_FAILED;
  }
  if (opened == nullptr)
    return VA_STATUS_ERROR_OPERATION_FAILED;
  decode->device = opened;
  return VA_STATUS_SUCCESS;
}

static VAStatus OpenDecoder(DecodeContext *decode, unsigned int width = 0,
                            unsigned int height = 0,
                            const std::array<uint8_t, 4> *wmv3_metadata = nullptr,
                            uint64_t close_timeout_ns = kDecodeTimeoutNs) {
  BC_INPUT_FORMAT input = {};
  BC_INFO_CRYSTAL version = {};
  const bool reuse_admission = decode->device_preopened &&
      !decode->hardware_close_pending && !decode->decoder_open &&
      !decode->decoder_started;
  decode->device_preopened = false;
  if (decode->device != nullptr && !reuse_admission) {
    const BC_STATUS retained = decode->CloseHardware(close_timeout_ns);
    if (decode->device != nullptr)
      return VA_STATUS_ERROR_HW_BUSY;
    if (retained != BC_STS_SUCCESS)
      return VA_STATUS_ERROR_OPERATION_FAILED;
  }
  if (decode->device == nullptr) {
    const VAStatus opened = OpenDevice(decode);
    if (opened != VA_STATUS_SUCCESS)
      return opened;
  }

  int output_poll_fd = -1;
  if (DtsDupOutputPollFd != nullptr &&
      DtsDupOutputPollFd(decode->device, &output_poll_fd) == BC_STS_SUCCESS)
    decode->output_poll =
        std::make_shared<OutputPollLease>(output_poll_fd);
  else
    decode->output_poll.reset();

  BC_STATUS status = DtsCrystalHDVersion(decode->device, &version);
  if (status != BC_STS_SUCCESS)
    goto fail;
  decode->is_70012 = version.device == 0;
  if (decode->is_70012) {
    decode->low_latency_h264 = false;
    decode->experimental_ready_remap = false;
    decode->experimental_ready_latest = false;
    decode->experimental_ready_delay_ns = 0;
    decode->experimental_last_delivered_ordinal = 0;
  }
  // These finite-batch paths have only been validated on BCM70015.
  if ((decode->IsMpeg2() || decode->IsMpeg4() || decode->IsVc1()) &&
      decode->is_70012)
    goto fail;
  if (decode->processing_geometry.enabled && decode->is_70012)
    goto fail;

  input.FGTEnable = FALSE;
  input.Progressive = TRUE;
  // Do not set mpcOutPutMaxFRate (bit 6). At 720p it forces the firmware's
  // maximum cadence even for a 30 fps stream, creating frames Chromium must
  // discard. The reconstructed SPS VUI and this fallback now both say 30 fps.
  input.OptFlags = 0x80000000U | vdecFrameRate30;
  input.mSubtype = decode->IsMpeg2() ? BC_MSUBTYPE_MPEG2VIDEO : BC_MSUBTYPE_H264;
  if (decode->IsMpeg4())
    input.mSubtype = BC_MSUBTYPE_DIVX;
  if (decode->IsVc1()) {
    input.mSubtype = decode->profile == VAProfileVC1Advanced
                         ? BC_MSUBTYPE_VC1 : BC_MSUBTYPE_WMV3;
    if (input.mSubtype == BC_MSUBTYPE_WMV3) {
      // DtsSetInputFormat takes a private copy. Keep the committed metadata
      // through hardware-only teardown so finite-batch reopen is identical.
      const auto &metadata = wmv3_metadata ? *wmv3_metadata : decode->wmv3_metadata;
      input.pMetaData = const_cast<uint8_t *>(metadata.data());
      input.metaDataSz = metadata.size();
    }
  }
  input.width = width != 0 ? width : decode->StreamWidth();
  input.height = height != 0 ? height : decode->StreamHeight();
  input.startCodeSz = 4;

  status = DtsSetInputFormat(decode->device, &input);
  if (status != BC_STS_SUCCESS)
    goto fail;
  if (decode->processing_geometry.enabled) {
    BC_SCALING_PARAMS scaling = {};
    // Only width is implemented by DtsSetScaleParams. The explicit SPS crop
    // and validated exact aspect ratio determine the capture height.
    scaling.sWidth = decode->processing_geometry.capture_width;
    status = DtsSetScaleParams(decode->device, &scaling);
    if (status != BC_STS_SUCCESS)
      goto fail;
  }
  status = DtsOpenDecoder(decode->device, BC_STREAM_TYPE_ES);
  if (status != BC_STS_SUCCESS)
    goto fail;
  decode->decoder_open = true;
  status = DtsSetColorSpace(decode->device, OUTPUT_MODE422_YUY2);
  if (status != BC_STS_SUCCESS)
    goto fail;
  status = DtsStartDecoder(decode->device);
  if (status != BC_STS_SUCCESS)
    goto fail;
  decode->decoder_started = true;
  status = DtsStartCapture(decode->device);
  if (status != BC_STS_SUCCESS)
    goto fail;
  for (const auto &picture : decode->experimental_ready)
    picture->experimental_ready_queued = false;
  decode->experimental_ready.clear();
  return VA_STATUS_SUCCESS;

fail:
  decode->CloseHardware();
  return VA_STATUS_ERROR_OPERATION_FAILED;
}

static VAStatus OpenDecoderForSubmit(
    DecodeContext *decode, Surface *target, unsigned int width,
    unsigned int height,
    const std::array<uint8_t, 4> *wmv3_metadata = nullptr) {
  const bool experimental_before_open = decode->experimental_ready_remap;
  const VAStatus status =
      OpenDecoder(decode, width, height, wmv3_metadata);
  // Device discovery can turn the experiment off before a later setup step
  // fails.  Drop the BeginPicture reservation on that transition even when
  // OpenDecoder itself fails, or this surface would remain permanently busy
  // after falling back to the ordinary BCM70012 path.
  if (experimental_before_open && !decode->experimental_ready_remap)
    ClearExperimentalPresentation(target);
  return status;
}

static bool CopyYuy2ToSurface(Surface *surface, const BC_DTS_PROC_OUT &output,
                              bool is_70012) {
  const unsigned int width = std::min<unsigned int>(output.PicInfo.width,
                                                     surface->width);
  const unsigned int height = std::min<unsigned int>(output.PicInfo.height,
                                                      surface->height);
  unsigned int source_pitch = output.PicInfo.width * 2;
  if (is_70012) {
    const unsigned int padded_width =
        output.PicInfo.width <= 720
            ? 720
            : (output.PicInfo.width <= 1280 ? 1280 : 1920);
    source_pitch = padded_width * 2;
  }

  bool used_i915_wc = false;
  if (!surface->BeginDecodedCpuWrite(&used_i915_wc))
    return false;
  if (surface->fourcc == VA_FOURCC_ARGB) {
    // The decoded rectangle is coded geometry. Larger client allocations are
    // allowed, but their unused logical region must not retain older pixels.
    for (unsigned int y = 0; y < surface->height; ++y) {
      uint8_t *line = surface->planes[0] + static_cast<size_t>(y) * surface->pitch[0];
      for (unsigned int x = 0; x < surface->width; ++x)
        StoreArgb(line + static_cast<size_t>(x) * 4, 16, 128, 128);
    }
    for (unsigned int y = 0; y < height; ++y) {
      const uint8_t *source = output.Ybuff +
                              static_cast<size_t>(y) * source_pitch;
      uint8_t *destination = surface->planes[0] +
                             static_cast<size_t>(y) * surface->pitch[0];
      for (unsigned int x = 0; x + 1 < width; x += 2) {
        StoreArgb(destination + static_cast<size_t>(x) * 4, source[x * 2],
                  source[x * 2 + 1], source[x * 2 + 3]);
        StoreArgb(destination + static_cast<size_t>(x + 1) * 4,
                  source[x * 2 + 2], source[x * 2 + 1], source[x * 2 + 3]);
      }
    }
    if (!surface->EndDecodedCpuWrite(used_i915_wc))
      return false;
    surface->ready = true;
    surface->frame_timestamp = output.PicInfo.timeStamp;
    return true;
  }
  // Clear only neutral padding; the copied rectangle is fully overwritten.
  if (width < surface->width)
    for (unsigned int y = 0; y < height; ++y)
      memset(surface->planes[0] + static_cast<size_t>(y) * surface->pitch[0] + width,
             16, surface->width - width);
  for (unsigned int y = height; y < surface->height; ++y)
    memset(surface->planes[0] + static_cast<size_t>(y) * surface->pitch[0],
           16, surface->width);
  const unsigned int chroma_rows = (height + 1) / 2;
  const unsigned int chroma_width = Align(surface->width, 2);
  // Conversion writes complete UV pairs only, including for odd widths.
  const unsigned int copied_chroma_width = width & ~1U;
  if (copied_chroma_width < chroma_width)
    for (unsigned int y = 0; y < chroma_rows; ++y)
      memset(surface->planes[1] + static_cast<size_t>(y) * surface->pitch[1] +
                 copied_chroma_width,
             128, chroma_width - copied_chroma_width);
  for (unsigned int y = chroma_rows; y < (surface->height + 1) / 2; ++y)
    memset(surface->planes[1] + static_cast<size_t>(y) * surface->pitch[1],
           128, chroma_width);
  // Consume each pair of packed rows once. SSE2 keeps the scalar conversion's
  // exact rounded chroma average; it does not change colorspace or filtering.
#if defined(__SSE2__)
  const __m128i luma_mask = _mm_set1_epi16(0x00ff);
#endif
  for (unsigned int y = 0; y < height; y += 2) {
    const uint8_t *top = output.Ybuff + static_cast<size_t>(y) * source_pitch;
    const uint8_t *bottom = output.Ybuff +
                            static_cast<size_t>(std::min(y + 1, height - 1)) *
                                source_pitch;
    uint8_t *top_luma = surface->planes[0] +
                       static_cast<size_t>(y) * surface->pitch[0];
    const bool has_bottom = y + 1 < height;
    uint8_t *bottom_luma = has_bottom
        ? top_luma + surface->pitch[0] : top_luma;
    uint8_t *destination = surface->planes[1] +
                           static_cast<size_t>(y / 2) * surface->pitch[1];
    unsigned int x = 0;
#if defined(__SSE2__)
    // Only complete 16-pixel blocks: neither load crosses the logical row,
    // and unaligned loads/stores support imported pitches and small surfaces.
    for (; width - x >= 16; x += 16) {
      const __m128i top0 = _mm_loadu_si128(
          reinterpret_cast<const __m128i *>(top + x * 2));
      const __m128i top1 = _mm_loadu_si128(
          reinterpret_cast<const __m128i *>(top + x * 2 + 16));
      const __m128i bottom0 = _mm_loadu_si128(
          reinterpret_cast<const __m128i *>(bottom + x * 2));
      const __m128i bottom1 = _mm_loadu_si128(
          reinterpret_cast<const __m128i *>(bottom + x * 2 + 16));
      _mm_storeu_si128(reinterpret_cast<__m128i *>(top_luma + x),
          _mm_packus_epi16(_mm_and_si128(top0, luma_mask),
                           _mm_and_si128(top1, luma_mask)));
      if (has_bottom)
        _mm_storeu_si128(reinterpret_cast<__m128i *>(bottom_luma + x),
            _mm_packus_epi16(_mm_and_si128(bottom0, luma_mask),
                             _mm_and_si128(bottom1, luma_mask)));
      _mm_storeu_si128(reinterpret_cast<__m128i *>(destination + x),
          _mm_packus_epi16(_mm_srli_epi16(_mm_avg_epu8(top0, bottom0), 8),
                           _mm_srli_epi16(_mm_avg_epu8(top1, bottom1), 8)));
    }
#endif
    for (; x + 1 < width; x += 2) {
      top_luma[x] = top[x * 2];
      top_luma[x + 1] = top[x * 2 + 2];
      // Without SSE2, avoid a per-pair branch: the last odd row aliases
      // top/bottom and both luma destinations, so repeated stores agree.
#if defined(__SSE2__)
      if (has_bottom)
#endif
      {
        bottom_luma[x] = bottom[x * 2];
        bottom_luma[x + 1] = bottom[x * 2 + 2];
      }
      destination[x] = static_cast<uint8_t>(
          (static_cast<unsigned int>(top[x * 2 + 1]) + bottom[x * 2 + 1] + 1) /
          2);
      destination[x + 1] = static_cast<uint8_t>(
          (static_cast<unsigned int>(top[x * 2 + 3]) + bottom[x * 2 + 3] + 1) /
          2);
    }
    if (x < width) {
      top_luma[x] = top[x * 2];
      if (has_bottom)
        bottom_luma[x] = bottom[x * 2];
    }
  }
  if (!surface->EndDecodedCpuWrite(used_i915_wc))
    return false;
  surface->ready = true;
  surface->frame_timestamp = output.PicInfo.timeStamp;
  return true;
}

static bool CopyH264InterlacedToSurface(
    Surface *surface, const uint8_t *const fields[2], unsigned int source_width,
    unsigned int source_height, size_t field_pitch, uint64_t timestamp,
    unsigned int field_mask = 3, bool compact_field = false) {
  // A single parity is used only by the operation-private Bob view below.
  // Public decode surfaces always use both fields and complete as before.
  if (surface == nullptr || field_mask == 0 || field_mask > 3 ||
      ((field_mask & 1U) && fields[0] == nullptr) ||
      ((field_mask & 2U) && fields[1] == nullptr) ||
      (field_mask != 3 && surface->fourcc != VA_FOURCC_NV12) ||
      (compact_field && (field_mask == 3 || surface->fourcc != VA_FOURCC_NV12)) ||
      source_width == 0 || (source_width & 1U) != 0 || source_height == 0 ||
      source_height % 4U != 0 || field_pitch < source_width * 2U)
    return false;

  const unsigned int width =
      std::min<unsigned int>(source_width, surface->width);
  const unsigned int height =
      std::min<unsigned int>(source_height / (compact_field ? 2U : 1U),
                             surface->height);
  const unsigned int field_rows = source_height / 2U;

  bool used_i915_wc = false;
  if (!surface->BeginDecodedCpuWrite(&used_i915_wc))
    return false;
  if (surface->fourcc == VA_FOURCC_ARGB) {
    for (unsigned int row = 0; row < surface->height; ++row) {
      uint8_t *line = surface->planes[0] +
                      static_cast<size_t>(row) * surface->pitch[0];
      for (unsigned int column = 0; column < surface->width; ++column)
        StoreArgb(line + static_cast<size_t>(column) * 4, 16, 128, 128);
    }
    for (unsigned int parity = 0; parity < 2; ++parity) {
      for (unsigned int source_row = 0; source_row < field_rows; ++source_row) {
        const unsigned int destination_row = source_row * 2U + parity;
        if (destination_row >= height)
          break;
        const uint8_t *source = fields[parity] +
            static_cast<size_t>(source_row) * field_pitch;
        uint8_t *destination = surface->planes[0] +
            static_cast<size_t>(destination_row) * surface->pitch[0];
        for (unsigned int column = 0; column + 1 < width; column += 2) {
          StoreArgb(destination + static_cast<size_t>(column) * 4,
                    source[column * 2], source[column * 2 + 1],
                    source[column * 2 + 3]);
          StoreArgb(destination + static_cast<size_t>(column + 1) * 4,
                    source[column * 2 + 2], source[column * 2 + 1],
                    source[column * 2 + 3]);
        }
      }
    }
    if (!surface->EndDecodedCpuWrite(used_i915_wc))
      return false;
    surface->ready = true;
    surface->frame_timestamp = timestamp;
    return true;
  }

  if (surface->fourcc != VA_FOURCC_NV12 || (width & 1U) != 0) {
    surface->EndDecodedCpuWrite(used_i915_wc);
    return false;
  }
  for (unsigned int row = 0; row < height; ++row) {
    if (width < surface->width)
      memset(surface->planes[0] + static_cast<size_t>(row) * surface->pitch[0] +
                 width,
             16, surface->width - width);
  }
  for (unsigned int row = height; row < surface->height; ++row)
    memset(surface->planes[0] + static_cast<size_t>(row) * surface->pitch[0],
           16, surface->width);
  const unsigned int chroma_rows = (height + 1U) / 2U;
  const unsigned int chroma_width = Align(surface->width, 2);
  for (unsigned int row = 0; row < chroma_rows; ++row) {
    if (width < chroma_width)
      memset(surface->planes[1] + static_cast<size_t>(row) * surface->pitch[1] +
                 width,
             128, chroma_width - width);
  }
  for (unsigned int row = chroma_rows;
       row < (surface->height + 1U) / 2U; ++row)
    memset(surface->planes[1] + static_cast<size_t>(row) * surface->pitch[1],
           128, chroma_width);

#if defined(__SSE2__)
  const __m128i luma_mask = _mm_set1_epi16(0x00ff);
#endif
  // Field views can reference two compact buffers or alternating rows of a
  // whole-picture transfer. Downsample chroma within each field in either
  // case; averaging opposite fields produces temporal comb colors.
  for (unsigned int parity = 0; parity < 2; ++parity) {
    if ((field_mask & (1U << parity)) == 0)
      continue;
#if !defined(__SSE2__)
    // Keep same-field source rows hot without keeping all five pixel
    // pointers live in one scalar loop on register-limited 32-bit CPUs.
    for (unsigned int row_pair = 0;
         row_pair < field_rows &&
             (compact_field ? row_pair : row_pair * 2U + parity) < height;
         row_pair += 2) {
      for (unsigned int source_row = row_pair; source_row < row_pair + 2U;
           ++source_row) {
#else
    for (unsigned int source_row = 0; source_row < field_rows; ++source_row) {
#endif
      const unsigned int destination_row = compact_field
          ? source_row : source_row * 2U + parity;
      if (destination_row >= height)
        break;
      const uint8_t *source = fields[parity] +
          static_cast<size_t>(source_row) * field_pitch;
      uint8_t *destination = surface->planes[0] +
          static_cast<size_t>(destination_row) * surface->pitch[0];
      unsigned int column = 0;
#if defined(__SSE2__)
      for (; width - column >= 16; column += 16) {
        const __m128i packed0 = _mm_loadu_si128(
            reinterpret_cast<const __m128i *>(source + column * 2));
        const __m128i packed1 = _mm_loadu_si128(
            reinterpret_cast<const __m128i *>(source + column * 2 + 16));
        _mm_storeu_si128(reinterpret_cast<__m128i *>(destination + column),
            _mm_packus_epi16(_mm_and_si128(packed0, luma_mask),
                             _mm_and_si128(packed1, luma_mask)));
      }
#endif
      for (; column + 1 < width; column += 2) {
        destination[column] = source[column * 2];
        destination[column + 1] = source[column * 2 + 2];
      }
    }

#if !defined(__SSE2__)
      for (unsigned int source_row = row_pair; source_row + 1 < row_pair + 2U;
           source_row += 2) {
#else
    for (unsigned int source_row = 0; source_row + 1 < field_rows;
         source_row += 2) {
#endif
      const unsigned int destination_row = compact_field
          ? source_row / 2U : source_row + parity;
      if (destination_row >= chroma_rows)
        break;
      const uint8_t *first_row = fields[parity] +
          static_cast<size_t>(source_row) * field_pitch;
      const uint8_t *second_row = first_row + field_pitch;
      uint8_t *destination = surface->planes[1] +
          static_cast<size_t>(destination_row) * surface->pitch[1];
      unsigned int column = 0;
#if defined(__SSE2__)
      for (; width - column >= 16; column += 16) {
        const __m128i first0 = _mm_loadu_si128(
            reinterpret_cast<const __m128i *>(first_row + column * 2));
        const __m128i first1 = _mm_loadu_si128(
            reinterpret_cast<const __m128i *>(first_row + column * 2 + 16));
        const __m128i second0 = _mm_loadu_si128(
            reinterpret_cast<const __m128i *>(second_row + column * 2));
        const __m128i second1 = _mm_loadu_si128(
            reinterpret_cast<const __m128i *>(second_row + column * 2 + 16));
        _mm_storeu_si128(reinterpret_cast<__m128i *>(destination + column),
            _mm_packus_epi16(
                _mm_srli_epi16(_mm_avg_epu8(first0, second0), 8),
                _mm_srli_epi16(_mm_avg_epu8(first1, second1), 8)));
      }
#endif
      for (; column + 1 < width; column += 2) {
        destination[column] = static_cast<uint8_t>(
            (static_cast<unsigned int>(first_row[column * 2 + 1]) +
             second_row[column * 2 + 1] + 1U) /
            2U);
        destination[column + 1] = static_cast<uint8_t>(
            (static_cast<unsigned int>(first_row[column * 2 + 3]) +
             second_row[column * 2 + 3] + 1U) /
            2U);
      }
    }
#if !defined(__SSE2__)
    }
#endif
  }
  if (!surface->EndDecodedCpuWrite(used_i915_wc))
    return false;
  surface->ready = true;
  surface->frame_timestamp = timestamp;
  return true;
}

static bool CopyH264FieldPairToSurface(
    Surface *surface, const H264FirstField &first,
    const BC_DTS_PROC_OUT &second) {
  if (!first.valid || second.Ybuff == nullptr ||
      first.width != second.PicInfo.width ||
      first.height != second.PicInfo.height ||
      first.pixels.size() != static_cast<size_t>(first.source_pitch) *
                                 (first.height / 2U))
    return false;
  const uint8_t *fields[2] = {};
  fields[first.bottom ? 1 : 0] = first.pixels.data();
  fields[first.bottom ? 0 : 1] = second.Ybuff;
  return CopyH264InterlacedToSurface(surface, fields, first.width, first.height,
                                     first.source_pitch, first.timestamp);
}

static bool CopyH264WeaveToSurface(Surface *surface,
                                  const BC_DTS_PROC_OUT &output) {
  if (output.Ybuff == nullptr)
    return false;
  const size_t pitch = static_cast<size_t>(output.PicInfo.width) * 2U;
  const uint8_t *fields[2] = {output.Ybuff, output.Ybuff + pitch};
  return CopyH264InterlacedToSurface(surface, fields, output.PicInfo.width,
                                     output.PicInfo.height, pitch * 2U,
                                     output.PicInfo.timeStamp);
}

static bool CopyNv12Pixels(const Surface &source, Surface *destination) {
  if (destination == nullptr || source.fourcc != VA_FOURCC_NV12 ||
      destination->fourcc != VA_FOURCC_NV12)
    return false;
  const unsigned int width = std::min(source.width, destination->width);
  const unsigned int height = std::min(source.height, destination->height);
  if (!source.BeginCpuRead())
    return false;
  if (!destination->BeginCpuWrite()) {
    source.EndCpuRead();
    return false;
  }
  if (width < destination->width)
    for (unsigned int row = 0; row < height; ++row)
      memset(destination->planes[0] + static_cast<size_t>(row) * destination->pitch[0] + width,
             16, destination->width - width);
  for (unsigned int row = height; row < destination->height; ++row)
    memset(destination->planes[0] + static_cast<size_t>(row) * destination->pitch[0],
           16, destination->width);
  const unsigned int chroma_rows = (height + 1) / 2;
  const unsigned int chroma_width = Align(destination->width, 2);
  // Unlike YUY2 conversion, the NV12 memcpy below writes exactly width bytes.
  if (width < chroma_width)
    for (unsigned int row = 0; row < chroma_rows; ++row)
      memset(destination->planes[1] + static_cast<size_t>(row) * destination->pitch[1] + width,
             128, chroma_width - width);
  for (unsigned int row = chroma_rows; row < (destination->height + 1) / 2; ++row)
    memset(destination->planes[1] + static_cast<size_t>(row) * destination->pitch[1],
           128, chroma_width);
  for (unsigned int row = 0; row < height; ++row) {
    memcpy(destination->planes[0] +
               static_cast<size_t>(row) * destination->pitch[0],
           source.planes[0] + static_cast<size_t>(row) * source.pitch[0],
           width);
  }
  for (unsigned int row = 0; row < (height + 1) / 2; ++row) {
    memcpy(destination->planes[1] +
               static_cast<size_t>(row) * destination->pitch[1],
           source.planes[1] + static_cast<size_t>(row) * source.pitch[1],
           width);
  }
  const bool committed = destination->EndCpuWrite();
  const bool released = source.EndCpuRead();
  if (!committed || !released)
    return false;
  return true;
}

static bool CopyNv12Surface(const Surface &source, Surface *destination) {
  if (!CopyNv12Pixels(source, destination))
    return false;
  destination->ready = true;
  destination->failed = false;
  destination->frame_timestamp = source.frame_timestamp;
  return true;
}

static bool PromoteDecodedPicture(const std::shared_ptr<Surface> &picture) {
  if (!picture || !picture->direct_backing)
    return true;
  const auto backing = picture->direct_backing;
  if (!picture->AllocateInternal(nullptr, -1, picture->width, picture->height,
                                  VA_FOURCC_NV12))
    return false;
  if (picture->ready && !picture->failed &&
      (backing->destroyed || backing->decode_picture.lock() != picture ||
       backing->frame_timestamp != picture->frame_timestamp ||
       !CopyNv12Surface(*backing, picture.get()))) {
    picture->ready = false;
    picture->failed = true;
    picture->direct_backing.reset();
    return false;
  }
  picture->direct_backing.reset();
  return true;
}

static uint64_t MonotonicNowNs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count());
}

static bool ExperimentalWriteLeaseMatches(const DecodeContext &decode,
                                          const Surface &surface) {
  return surface.experimental_write_lease &&
      surface.experimental_write_context ==
          TraceId(decode.id, VA_INVALID_ID) &&
      surface.experimental_write_generation == decode.generation &&
      surface.experimental_write_submission_ordinal != 0 &&
      surface.experimental_write_submission_ordinal ==
          surface.trace_submission_ordinal;
}

static void RetireExperimentalReadyThrough(
    Driver *driver, DecodeContext *decode, uint64_t ordinal,
    const std::shared_ptr<Surface> &consumed) {
  if (driver == nullptr || decode == nullptr || ordinal == 0)
    return;
  for (auto it = decode->experimental_ready.begin();
       it != decode->experimental_ready.end();) {
    const std::shared_ptr<Surface> picture = *it;
    if (!picture || picture->trace_submission_ordinal > ordinal) {
      ++it;
      continue;
    }
    it = decode->experimental_ready.erase(it);
    picture->experimental_ready_queued = false;
    const auto found =
        decode->decoded_frames.find(picture->expected_timestamp);
    if (found != decode->decoded_frames.end() && found->second == picture)
      decode->decoded_frames.erase(found);
    if (picture != consumed) {
      crystalhd_vaapi_trace::Fields fields =
          TraceFields(decode, picture.get());
      fields.payload_token = picture->frame_timestamp;
      fields.payload_decode_identity = picture->trace_decode_identity;
      fields.payload_submission_ordinal =
          picture->trace_submission_ordinal;
      fields.outcome = -ESTALE;
      driver->trace.Emit(crystalhd_vaapi_trace::Event::PresentationDrop,
                         fields);
    }
  }
  // An exact public surface may complete without ever entering the remap
  // inventory (for example, a direct output). Retire its matching private
  // picture as well; the queue walk above is intentionally not its lifetime
  // authority.
  if (consumed) {
    consumed->experimental_ready_queued = false;
    const auto found =
        decode->decoded_frames.find(consumed->expected_timestamp);
    if (found != decode->decoded_frames.end() && found->second == consumed)
      decode->decoded_frames.erase(found);
  }
}

static void QueueExperimentalReady(
    Driver *driver, DecodeContext *decode,
    std::shared_ptr<Surface> picture) {
  if (driver == nullptr || decode == nullptr || !picture ||
      !decode->experimental_ready_remap || picture->destroyed ||
      picture->failed || !picture->ready ||
      picture->frame_timestamp != picture->expected_timestamp ||
      picture->trace_submission_ordinal == 0 ||
      picture->experimental_ready_queued)
    return;
  if (picture->trace_submission_ordinal <=
      decode->experimental_last_delivered_ordinal) {
    const auto found =
        decode->decoded_frames.find(picture->expected_timestamp);
    if (found != decode->decoded_frames.end() && found->second == picture)
      decode->decoded_frames.erase(found);
    crystalhd_vaapi_trace::Fields fields =
        TraceFields(decode, picture.get());
    fields.payload_token = picture->frame_timestamp;
    fields.payload_decode_identity = picture->trace_decode_identity;
    fields.payload_submission_ordinal = picture->trace_submission_ordinal;
    fields.outcome = -ESTALE;
    driver->trace.Emit(crystalhd_vaapi_trace::Event::PresentationDrop,
                       fields);
    return;
  }
  decode->experimental_ready.push_back(picture);
  picture->experimental_ready_at_ns = MonotonicNowNs();
  picture->experimental_ready_queued = true;
  crystalhd_vaapi_trace::Fields fields = TraceFields(decode, picture.get());
  fields.payload_token = picture->frame_timestamp;
  fields.payload_decode_identity = picture->trace_decode_identity;
  fields.payload_submission_ordinal = picture->trace_submission_ordinal;
  driver->trace.Emit(crystalhd_vaapi_trace::Event::PresentationReady, fields);

  while (decode->experimental_ready.size() >
         kExperimentalPresentationReadyLimit) {
    const std::shared_ptr<Surface> dropped =
        decode->experimental_ready.front();
    decode->experimental_ready.pop_front();
    dropped->experimental_ready_queued = false;
    crystalhd_vaapi_trace::Fields dropped_fields =
        TraceFields(decode, dropped.get());
    dropped_fields.payload_token = dropped->frame_timestamp;
    dropped_fields.payload_decode_identity = dropped->trace_decode_identity;
    dropped_fields.payload_submission_ordinal =
        dropped->trace_submission_ordinal;
    dropped_fields.outcome = -ENOBUFS;
    driver->trace.Emit(crystalhd_vaapi_trace::Event::PresentationDrop,
                       dropped_fields);
    const auto decoded =
        decode->decoded_frames.find(dropped->expected_timestamp);
    if (decoded != decode->decoded_frames.end() &&
        decoded->second == dropped)
      decode->decoded_frames.erase(decoded);
  }
}

static std::shared_ptr<Surface> SelectExperimentalReady(
    const DecodeContext &decode, const Surface &requested, uint64_t now_ns) {
  if (!decode.experimental_ready_remap || decode.closing || decode.retired ||
      decode.ReplaySealed() || requested.ready ||
      requested.failed || !ExperimentalWriteLeaseMatches(decode, requested) ||
      requested.trace_generation != decode.generation ||
      requested.trace_submission_ordinal == 0)
    return {};
  if (decode.experimental_ready_delay_ns != 0 &&
      (requested.experimental_submitted_at_ns == 0 ||
       requested.experimental_submitted_at_ns > now_ns ||
       now_ns - requested.experimental_submitted_at_ns <
           decode.experimental_ready_delay_ns))
    return {};
  std::shared_ptr<Surface> selected;
  for (const std::shared_ptr<Surface> &candidate :
       decode.experimental_ready) {
    if (!candidate || !candidate->experimental_ready_queued ||
        candidate->destroyed || candidate->failed || !candidate->ready ||
        candidate->frame_timestamp != candidate->expected_timestamp ||
        candidate->trace_context != requested.trace_context ||
        candidate->trace_generation != requested.trace_generation ||
        candidate->trace_submission_ordinal >=
            requested.trace_submission_ordinal ||
        candidate->trace_submission_ordinal <=
            decode.experimental_last_delivered_ordinal ||
        (!decode.experimental_ready_latest &&
         candidate->trace_submission_ordinal + 1 !=
             requested.trace_submission_ordinal) ||
        candidate->width != requested.width ||
        candidate->height != requested.height ||
        candidate->fourcc != VA_FOURCC_NV12 ||
        requested.fourcc != VA_FOURCC_NV12 ||
        candidate->experimental_ready_at_ns == 0 ||
        candidate->experimental_ready_at_ns > now_ns ||
        now_ns - candidate->experimental_ready_at_ns >
            kExperimentalPresentationMaxAgeNs)
      continue;
    if (!selected || candidate->trace_submission_ordinal >
                         selected->trace_submission_ordinal)
      selected = candidate;
  }
  return selected;
}

static VAStatus TryExperimentalPresentation(
    Driver *driver, const std::shared_ptr<Surface> &requested,
    bool *committed) {
  *committed = false;
  if (!requested || requested->trace_context == 0)
    return VA_STATUS_SUCCESS;
  const auto found = driver->contexts.find(
      static_cast<VAContextID>(requested->trace_context));
  if (found == driver->contexts.end() ||
      !found->second->experimental_ready_remap ||
      found->second->generation != requested->trace_generation)
    return VA_STATUS_SUCCESS;
  DecodeContext *decode = found->second.get();
  const size_t active_presentations = static_cast<size_t>(std::count_if(
      driver->surfaces.begin(), driver->surfaces.end(),
      [&](const auto &entry) {
        const Surface *surface = entry.second.get();
        return surface->backing_owner == VA_INVALID_SURFACE &&
            surface->trace_context == requested->trace_context &&
            surface->trace_generation == requested->trace_generation &&
            surface->experimental_presentation;
      }));
  if (active_presentations >= kExperimentalPresentationActiveLimit)
    return VA_STATUS_SUCCESS;
  const uint64_t selection_ns = MonotonicNowNs();
  const std::shared_ptr<Surface> payload =
      SelectExperimentalReady(*decode, *requested, selection_ns);
  if (!payload)
    return VA_STATUS_SUCCESS;
  if (!CopyNv12Pixels(*payload, requested.get()))
    return VA_STATUS_ERROR_OPERATION_FAILED;
  const uint64_t commit_ns = MonotonicNowNs();

  requested->experimental_presentation = true;
  requested->experimental_write_lease = false;
  requested->experimental_write_context = 0;
  requested->experimental_write_generation = 0;
  requested->experimental_write_submission_ordinal = 0;
  requested->experimental_presentation_at_ns = commit_ns;
  requested->presentation_payload_token = payload->frame_timestamp;
  requested->presentation_payload_decode_identity =
      payload->trace_decode_identity;
  requested->presentation_payload_submission_ordinal =
      payload->trace_submission_ordinal;
  crystalhd_vaapi_trace::Fields fields =
      TraceFields(decode, requested.get(), requested->expected_timestamp);
  if (requested->experimental_submitted_at_ns != 0 &&
      commit_ns >= requested->experimental_submitted_at_ns)
    fields.duration_ns = commit_ns - requested->experimental_submitted_at_ns;
  driver->trace.Emit(crystalhd_vaapi_trace::Event::PresentationCommit,
                     fields);
  decode->experimental_last_delivered_ordinal =
      payload->trace_submission_ordinal;
  RetireExperimentalReadyThrough(driver, decode,
                                 payload->trace_submission_ordinal, payload);
  *committed = true;
  driver->condition.notify_all();
  return VA_STATUS_SUCCESS;
}

static void ConsumeExperimentalExact(
    Driver *driver, const std::shared_ptr<Surface> &surface) {
  if (!surface || surface->experimental_presentation || !surface->ready ||
      surface->frame_timestamp != surface->expected_timestamp ||
      surface->trace_context == 0)
    return;
  const auto found = driver->contexts.find(
      static_cast<VAContextID>(surface->trace_context));
  if (found == driver->contexts.end() ||
      !found->second->experimental_ready_remap ||
      found->second->generation != surface->trace_generation)
    return;
  const auto picture =
      found->second->decoded_frames.find(surface->expected_timestamp);
  std::shared_ptr<Surface> consumed;
  if (picture != found->second->decoded_frames.end() &&
      picture->second->trace_decode_identity == surface->trace_decode_identity)
    consumed = picture->second;
  found->second->experimental_last_delivered_ordinal = std::max(
      found->second->experimental_last_delivered_ordinal,
      surface->trace_submission_ordinal);
  RetireExperimentalReadyThrough(driver, found->second.get(),
                                 surface->trace_submission_ordinal, consumed);
}

static void MarkPendingPicturesFailed(Driver *driver, DecodeContext *decode) {
  for (const auto &pending : decode->pending) {
    auto picture = decode->decoded_frames.find(pending.first);
    if (picture != decode->decoded_frames.end() && !picture->second->ready)
      picture->second->failed = true;
    auto surface = driver->surfaces.find(pending.second);
    if (surface != driver->surfaces.end() &&
        surface->second->expected_timestamp == pending.first &&
        ((picture != decode->decoded_frames.end() &&
          surface->second->decode_identity == picture->second->decode_identity) ||
         (picture == decode->decoded_frames.end() &&
          surface->second->decode_identity == 0 &&
          surface->second->decode_picture.expired())))
      surface->second->failed = true;
    if (surface != driver->surfaces.end() && surface->second->failed)
      RetireFailedExperimentalState(surface->second.get());
  }
}

static VAStatus FailDecode(Driver *driver, DecodeContext *decode,
                            const char *reason) {
  decode->auxiliary.candidate_timestamp = 0;
  decode->auxiliary.syntax.clear();
  decode->auxiliary.pending = decode->auxiliary.needs_eos = false;
  decode->CancelPendingPrefeed();
  decode->h264_first_field = H264FirstField();
  decode->partial_input_timestamp = 0;
  decode->partial_input_offset = 0;
  if (decode->IsMpeg4()) {
    decode->mpeg4_replay.Fail(reason);
    for (auto &epoch : decode->mpeg4_draining_epochs)
      epoch.Fail(reason);
  } else {
    decode->WithReplay([&](auto &replay) { return replay.Fail(reason); });
  }
  Debug("sealed-batch decoding failed: %s", reason);
  MarkPendingPicturesFailed(driver, decode);
  driver->condition.notify_all();
  return VA_STATUS_ERROR_DECODING_ERROR;
}

struct OutputProcessingState {
  std::shared_ptr<Surface> picture;
  std::shared_ptr<Surface> surface;
};

enum class H264InterlacedPayload { Field, Weave, Invalid };

static H264InterlacedPayload ClassifyH264InterlacedPayload(
    const BC_DTS_PROC_OUT &output) {
  switch (crystalhd_h264_interlaced_payload(&output)) {
    case CRYSTALHD_H264_PAYLOAD_FIELD:
      return H264InterlacedPayload::Field;
    case CRYSTALHD_H264_PAYLOAD_WEAVE:
      return H264InterlacedPayload::Weave;
    default:
      return H264InterlacedPayload::Invalid;
  }
}

static VAStatus ValidateH264AuxiliaryOutput(Driver *driver, DecodeContext *decode,
                                          const BC_DTS_PROC_OUT &out) {
  const auto &aux = decode->auxiliary;
  const uint64_t extent = static_cast<uint64_t>(out.PicInfo.width) * 2U *
                          out.PicInfo.height;
  if (!aux.pending || out.PicInfo.timeStamp != aux.timestamp ||
      aux.generation != decode->generation || aux.device != decode->device ||
      decode->is_70012 || !(out.PicInfo.flags & VDEC_FLAG_INTERLACED_SRC) ||
      (out.PoutFlags & BC_POUT_FLAGS_ENCRYPTED) != 0 ||
      ClassifyH264InterlacedPayload(out) != H264InterlacedPayload::Weave ||
      out.PicInfo.width != decode->CaptureWidth() ||
      out.PicInfo.height != decode->CaptureHeight() ||
      (out.PicInfo.height & 3U) != 0 || !out.Ybuff ||
      !(out.PoutFlags & BC_POUT_FLAGS_PIB_VALID) ||
      extent > static_cast<uint64_t>(out.YBuffDoneSz) * 4U)
    return FailDecode(driver, decode, "H.264 auxiliary full-height ownership/extent");
  // Known synthetic frame-coded MBAFF output is one full-height record;
  // metadata still carries INTERLACED_SRC. No public pixels or pair are made.
  return VA_STATUS_SUCCESS;
}

// No hardware calls: tests exercise the same exact-once/immutable-picture
// processing used for both normal decoding and IDR-prefix replay.
static VAStatus ProcessDecodedOutputImpl(
    Driver *driver, DecodeContext *decode, const BC_DTS_PROC_OUT &reported,
    OutputProcessingState *processing) {
  if ((reported.PoutFlags & BC_POUT_FLAGS_ENCRYPTED) != 0)
    return FailDecode(driver, decode, "encrypted output is unsupported");
  BC_DTS_PROC_OUT output = reported;
  H264FirstField paired_first_field;
  bool h264_field_pair = false;
  bool h264_weave = false;
  const bool h264_interlaced = IsH264Profile(decode->profile) &&
      (output.PicInfo.flags & VDEC_FLAG_INTERLACED_SRC) != 0;
  // BCM70012 resolves the application's token through DtsFetchMdata. That
  // lookup removes the entry while returning the first field, so a second
  // field carrying zero can only inherit the already-held first field's
  // token. Keep the exception narrow: every field property available before
  // the ownership lookup must still identify the complementary half.
  if (decode->is_70012 && h264_interlaced &&
      output.PicInfo.timeStamp == 0 && decode->h264_first_field.valid) {
    const H264FirstField &first = decode->h264_first_field;
    const uint32_t field_type =
        output.PicInfo.flags & VDEC_FLAG_BOTTOMFIELD;
    const bool field_type_valid = field_type == VDEC_FLAG_TOPFIELD ||
                                  field_type == VDEC_FLAG_BOTTOMFIELD;
    const bool bottom = field_type == VDEC_FLAG_BOTTOMFIELD;
    const bool bottom_first =
        (output.PicInfo.flags & VDEC_FLAG_BOTTOM_FIRST) != 0;
    const unsigned int padded_width = output.PicInfo.width <= 720
        ? 720 : (output.PicInfo.width <= 1280 ? 1280 : 1920);
    if (first.timestamp != 0 && first.is_70012 &&
        first.generation == decode->generation && field_type_valid &&
        first.bottom != bottom && first.bottom_first == bottom_first &&
        first.picture_number == output.PicInfo.picture_number &&
        first.sess_num == output.PicInfo.sess_num &&
        first.picture_meta_payload == output.PicInfo.picture_meta_payload &&
        first.width == output.PicInfo.width &&
        first.height == output.PicInfo.height &&
        first.source_pitch == padded_width * 2U) {
      output.PicInfo.timeStamp = first.timestamp;
      Debug("recover BCM70012 second H.264 field as token %llu",
            static_cast<unsigned long long>(first.timestamp));
    }
  }
  if (driver->trace.enabled()) {
    driver->trace.Emit(crystalhd_vaapi_trace::Event::OutputDequeued,
                       TraceDecodeTokenFields(driver, decode,
                                              output.PicInfo.timeStamp));
  }
  if (decode->prefed_timestamp != 0 &&
      output.PicInfo.timeStamp == decode->prefed_timestamp)
    return FailDecode(driver, decode,
                      "speculative H.264 prefix produced a picture");
  if (h264_interlaced && !decode->is_70012) {
    const auto layout = ClassifyH264InterlacedPayload(output);
    if (layout == H264InterlacedPayload::Invalid)
      return FailDecode(driver, decode,
                        "invalid H.264 interlaced payload or embedded identity");
    h264_weave = layout == H264InterlacedPayload::Weave;
  }
  if (h264_weave && decode->replay.Inspect(output.PicInfo.timeStamp) ==
                        CrystalHDDecodeReplay::Output::New) {
    const auto picture = decode->decoded_frames.find(output.PicInfo.timeStamp);
    const auto pending = decode->pending.find(output.PicInfo.timeStamp);
    if (picture == decode->decoded_frames.end() ||
        pending == decode->pending.end() ||
        picture->second->expected_timestamp != output.PicInfo.timeStamp ||
        picture->second->decode_identity == 0 ||
        picture->second->trace_generation != decode->generation)
      return FailDecode(driver, decode,
                        "whole H.264 picture has no matching decode owner");
  }
  if (decode->h264_first_field.valid && h264_weave)
    return FailDecode(driver, decode,
                      "whole H.264 picture interrupted a field pair");
  if (decode->h264_first_field.valid && !h264_interlaced)
    return FailDecode(driver, decode,
                      "progressive output interrupted an H.264 field pair");
  if (h264_interlaced && !h264_weave) {
    const uint32_t field_type =
        output.PicInfo.flags & VDEC_FLAG_BOTTOMFIELD;
    const bool field_type_valid = field_type == VDEC_FLAG_TOPFIELD ||
                                  field_type == VDEC_FLAG_BOTTOMFIELD;
    const bool bottom = field_type == VDEC_FLAG_BOTTOMFIELD;
    const bool bottom_first =
        (output.PicInfo.flags & VDEC_FLAG_BOTTOM_FIRST) != 0;
    const unsigned int width = output.PicInfo.width;
    const unsigned int height = output.PicInfo.height;
    const unsigned int padded_width = decode->is_70012
        ? (width <= 720 ? 720 : (width <= 1280 ? 1280 : 1920))
        : width;
    const uint64_t field_bytes = static_cast<uint64_t>(padded_width) *
                                 2U * (height / 2U);
    const auto inspection = decode->replay.Inspect(output.PicInfo.timeStamp);
    const bool replay_duplicate =
        inspection == CrystalHDDecodeReplay::Output::Duplicate &&
        decode->replay.IsOutstanding(output.PicInfo.timeStamp);
    const bool new_picture =
        inspection == CrystalHDDecodeReplay::Output::New;
    if (decode->is_70012 && output.PicInfo.timeStamp == 0 &&
        !decode->h264_first_field.valid)
      return FailDecode(driver, decode,
                        "unbound BCM70012 H.264 field has no timestamp");
    if (!decode->h264_first_field.valid &&
        !new_picture && !replay_duplicate) {
      if (inspection == CrystalHDDecodeReplay::Output::Invalid)
        return FailDecode(driver, decode, decode->ReplayFailure());
      return VA_STATUS_SUCCESS;
    }

    const auto decoded =
        decode->decoded_frames.find(output.PicInfo.timeStamp);
    const auto pending = decode->pending.find(output.PicInfo.timeStamp);
    const unsigned int crop_unit_y =
        decode->picture.seq_fields.bits.frame_mbs_only_flag ? 2U : 4U;
    const bool valid_field =
        (new_picture || replay_duplicate) &&
        (output.PoutFlags & BC_POUT_FLAGS_PIB_VALID) != 0 &&
        output.Ybuff != nullptr && field_type_valid && width != 0 &&
        height != 0 && width == decode->CaptureWidth() &&
        height == decode->CaptureHeight() && ((width | height) & 1U) == 0 &&
        (height & 3U) == 0 &&
        (replay_duplicate ||
         (decoded != decode->decoded_frames.end() &&
          pending != decode->pending.end() &&
          decoded->second->expected_timestamp == output.PicInfo.timeStamp &&
          decoded->second->decode_identity != 0 &&
          decoded->second->trace_generation == decode->generation &&
          FitsH264CodedGeometry(*decoded->second, width, height, crop_unit_y))) &&
        field_bytes <= std::numeric_limits<size_t>::max() &&
        static_cast<uint64_t>(output.YBuffDoneSz) * 4U >= field_bytes;
    if (!valid_field)
      return FailDecode(driver, decode,
                        "invalid H.264 field geometry/data or ownership");

    if (!decode->h264_first_field.valid) {
      std::vector<uint8_t> pixels;
      // Reference replay retires a duplicate only after its validated pair;
      // neither field's pixels may replace an already completed picture.
      if (!replay_duplicate)
        pixels.assign(output.Ybuff,
                      output.Ybuff + static_cast<size_t>(field_bytes));
      H264FirstField &first = decode->h264_first_field;
      first.replay_duplicate = replay_duplicate;
      first.bottom = bottom;
      first.bottom_first = bottom_first;
      first.is_70012 = decode->is_70012;
      first.timestamp = output.PicInfo.timeStamp;
      first.decode_identity =
          replay_duplicate ? 0 : decoded->second->decode_identity;
      first.generation = decode->generation;
      first.picture_number = output.PicInfo.picture_number;
      first.sess_num = output.PicInfo.sess_num;
      first.picture_meta_payload = output.PicInfo.picture_meta_payload;
      first.width = width;
      first.height = height;
      first.source_pitch = padded_width * 2U;
      first.picture = replay_duplicate ? nullptr : decoded->second;
      first.pixels = std::move(pixels);
      first.valid = true;
      return VA_STATUS_SUCCESS;
    }

    const H264FirstField &first = decode->h264_first_field;
    if (first.timestamp != output.PicInfo.timeStamp ||
        first.replay_duplicate != replay_duplicate ||
        (!replay_duplicate &&
         (decoded == decode->decoded_frames.end() ||
          first.decode_identity != decoded->second->decode_identity ||
          first.picture != decoded->second)) ||
        first.generation != decode->generation ||
        first.picture_number != output.PicInfo.picture_number ||
        first.sess_num != output.PicInfo.sess_num ||
        first.picture_meta_payload != output.PicInfo.picture_meta_payload ||
        first.width != width || first.height != height ||
        first.source_pitch != padded_width * 2U ||
        first.is_70012 != decode->is_70012 || first.bottom == bottom ||
        first.bottom_first != bottom_first)
      return FailDecode(driver, decode,
                        "H.264 fields have mismatched identity, parity, order or geometry");
    paired_first_field = std::move(decode->h264_first_field);
    decode->h264_first_field = H264FirstField();
    // Identity has already been checked against decoded_frames. Do not let
    // this temporary pair object turn the map into an apparent second owner:
    // a rebound/dropped public surface must still take the stale-pixel discard
    // path, while real capture/VPP owners remain visible in use_count/readers.
    paired_first_field.picture.reset();
    h264_field_pair = true;
  }
  const bool ordered_mpeg4_simple =
      decode->profile == VAProfileMPEG4Simple &&
      (output.PoutFlags & BC_POUT_FLAGS_PIB_VALID) != 0 &&
      (output.PicInfo.flags & VDEC_FLAG_INTERLACED_SRC) == 0;
  bool expected_mpeg4_simple = false;

  if (ordered_mpeg4_simple) {
    auto &replay = decode->Mpeg4TransportReplay();
    if (output.PicInfo.timeStamp == 0 &&
        output.PicInfo.picture_number != 0 &&
        decode->have_mpeg4_simple_picture_number &&
        decode->mpeg4_simple_picture_number !=
            std::numeric_limits<uint32_t>::max() &&
        output.PicInfo.picture_number ==
            decode->mpeg4_simple_picture_number + 1) {
      const uint64_t oldest = replay.OldestOutstanding();
      if (oldest != 0) {
        output.PicInfo.timeStamp = oldest;
        Debug("recover MPEG-4 Simple ordinal %u as token %llu",
              output.PicInfo.picture_number,
              static_cast<unsigned long long>(oldest));
      }
    }
    expected_mpeg4_simple = replay.IsOutstanding(output.PicInfo.timeStamp);
    if (expected_mpeg4_simple &&
        output.PicInfo.timeStamp != replay.OldestOutstanding())
      return FailDecode(driver, decode,
                        "MPEG-4 Simple output skipped an outstanding picture");
  }

  const auto disposition = decode->WithReplay([&](auto &replay) {
    return replay.Observe(output.PicInfo.timeStamp);
  });
  if (disposition == CrystalHDDecodeReplay::Output::Invalid)
    return FailDecode(driver, decode, decode->ReplayFailure());
  if (ordered_mpeg4_simple && expected_mpeg4_simple &&
      (disposition == CrystalHDDecodeReplay::Output::New ||
       disposition == CrystalHDDecodeReplay::Output::Duplicate)) {
    if (output.PicInfo.picture_number != 0) {
      decode->mpeg4_simple_picture_number = output.PicInfo.picture_number;
      decode->have_mpeg4_simple_picture_number = true;
    } else {
      decode->have_mpeg4_simple_picture_number = false;
    }
  }
  if (disposition != CrystalHDDecodeReplay::Output::New)
    return VA_STATUS_SUCCESS;

  VASurfaceID surface_id = VA_INVALID_SURFACE;
  auto pending = decode->pending.find(output.PicInfo.timeStamp);
  if (pending != decode->pending.end()) {
    surface_id = pending->second;
    decode->pending.erase(pending);
  }

  const unsigned int output_width = output.PicInfo.width;
  const unsigned int output_height = output.PicInfo.height;
  const unsigned int padded_width =
      decode->is_70012
          ? (output_width <= 720
                 ? 720
                 : (output_width <= 1280 ? 1280 : 1920))
          : output_width;
  const uint64_t required_bytes =
      static_cast<uint64_t>(padded_width) * 2 *
      (h264_field_pair ? output_height / 2U : output_height);
  auto decoded = decode->decoded_frames.find(output.PicInfo.timeStamp);
  // The map can be the last owner after a frontend reuses/drops its public
  // surface. Check before locking the public weak link below; captured and
  // queued VPP work retains a strong reference and must still receive pixels.
  const bool map_only_picture = decoded != decode->decoded_frames.end() &&
      decoded->second->vpp_readers == 0 && decoded->second.use_count() == 1;
  auto surface = driver->surfaces.find(surface_id);
  const auto owner_picture = surface != driver->surfaces.end()
      ? surface->second->decode_picture.lock() : nullptr;
  const bool exact_owner = surface != driver->surfaces.end() &&
      ((decoded != decode->decoded_frames.end() &&
        surface->second->decode_identity == decoded->second->decode_identity) ||
       (decoded == decode->decoded_frames.end() &&
        surface->second->decode_identity == 0 && !owner_picture));
  const bool current_picture =
      surface != driver->surfaces.end() &&
      surface->second->expected_timestamp == output.PicInfo.timeStamp &&
      exact_owner &&
      (!owner_picture || owner_picture == decoded->second) &&
      (decoded == decode->decoded_frames.end() ||
       !decoded->second->direct_picture ||
       (decoded->second->decode_target.lock() == surface->second &&
        surface->second->decode_picture.lock() == decoded->second &&
        surface->second->vpp_writers == 0));
  if (processing != nullptr) {
    if (decoded != decode->decoded_frames.end())
      processing->picture = decoded->second;
    if (current_picture)
      processing->surface = surface->second;
  }
  const auto fits = [&](const Surface &target) {
    if (decode->decode_processing)
      return target.width == output_width && target.height == output_height &&
             target.fourcc == VA_FOURCC_NV12;
    if (!IsH264Profile(decode->profile))
      return target.width >= output_width && target.height >= output_height;
    const unsigned int crop_unit_y = target.h264_crop_unit_y != 0
        ? target.h264_crop_unit_y
        : (decode->picture.seq_fields.bits.frame_mbs_only_flag ? 2U : 4U);
    return FitsH264CodedGeometry(target, output_width, output_height,
                                 crop_unit_y);
  };
  const bool valid_frame =
      (output.PoutFlags & BC_POUT_FLAGS_PIB_VALID) != 0 &&
      output.Ybuff != nullptr && output_width != 0 && output_height != 0 &&
      output_width == decode->CaptureWidth() &&
      output_height == decode->CaptureHeight() &&
      ((output_width | output_height) & 1U) == 0 &&
      (decoded == decode->decoded_frames.end() || fits(*decoded->second)) &&
      (!current_picture || fits(*surface->second)) &&
      static_cast<uint64_t>(output.YBuffDoneSz) * 4 >= required_bytes &&
      ((output.PicInfo.flags & VDEC_FLAG_INTERLACED_SRC) == 0 ||
       h264_field_pair || h264_weave);
  bool complete = valid_frame;
  bool wrote_pixels = false;
  bool public_materialized = false;
  const bool discard_pixels = complete && !current_picture &&
      map_only_picture && !decode->experimental_ready_remap;
  if (discard_pixels)
    Debug("discard unreferenced stale pixels timestamp=%llu",
          static_cast<unsigned long long>(output.PicInfo.timeStamp));
  const bool direct = current_picture &&
      decoded != decode->decoded_frames.end() &&
      decoded->second->direct_backing == surface->second;
  const auto copy_output = [&](Surface *target) {
    if (h264_field_pair)
      return CopyH264FieldPairToSurface(target, paired_first_field, output);
    if (h264_weave)
      return CopyH264WeaveToSurface(target, output);
    return CopyYuy2ToSurface(target, output, decode->is_70012);
  };
  if (decoded != decode->decoded_frames.end()) {
    if (complete && !discard_pixels) {
      if (direct) {
        complete = copy_output(surface->second.get());
        wrote_pixels = complete;
        decoded->second->ready = complete;
        decoded->second->frame_timestamp = complete ? output.PicInfo.timeStamp : 0;
      } else {
        complete = PromoteDecodedPicture(decoded->second) &&
            copy_output(decoded->second.get());
        wrote_pixels = complete;
      }
    }
    if (!complete)
      decoded->second->failed = true;
  }
  if (current_picture) {
    if (complete && !direct) {
      if (decode->experimental_ready_remap &&
          surface->second->experimental_presentation) {
        // The public backing is frozen with a previously selected READY
        // payload until the client relinquishes this surface at BeginPicture.
        // The newly completed requested picture remains private.
      } else {
        if (decode->experimental_ready_remap &&
            !ExperimentalWriteLeaseMatches(*decode, *surface->second)) {
          // BeginPicture can reserve this backing for the next submission
          // while an older output is still in flight. Preserve that older
          // output only in its private READY picture; it must not consume the
          // next submission's lease or mutate the public backing.
        } else {
          complete = decoded != decode->decoded_frames.end()
              ? CopyNv12Surface(*decoded->second, surface->second.get())
              : copy_output(surface->second.get());
          wrote_pixels = complete;
          public_materialized = complete;
          if (decode->experimental_ready_remap && complete) {
            surface->second->experimental_write_lease = false;
            surface->second->experimental_write_context = 0;
            surface->second->experimental_write_generation = 0;
            surface->second->experimental_write_submission_ordinal = 0;
          }
        }
      }
    }
    if (!complete) {
      surface->second->failed = true;
      RetireFailedExperimentalState(surface->second.get());
    }
  }
  if (complete && decoded != decode->decoded_frames.end() &&
      decode->experimental_ready_remap)
    QueueExperimentalReady(driver, decode, decoded->second);
  // A public surface can be reused before old hardware output arrives. Once
  // that old output is complete, only captured/queued VPP references need to
  // retain it; otherwise dropped frames would accumulate for the whole stream.
  if (driver->trace.enabled() && complete && wrote_pixels && current_picture &&
      (!decode->experimental_ready_remap || public_materialized)) {
    Surface *materialized = surface->second.get();
    driver->trace.Emit(crystalhd_vaapi_trace::Event::Materialized,
                       TraceFields(decode, materialized,
                                   output.PicInfo.timeStamp));
  }
  if (!current_picture) {
    const auto remaining =
        decode->decoded_frames.find(output.PicInfo.timeStamp);
    if (remaining != decode->decoded_frames.end() &&
        !remaining->second->experimental_ready_queued)
      decode->decoded_frames.erase(remaining);
  }
  driver->condition.notify_all();
  return complete ? VA_STATUS_SUCCESS
                  : FailDecode(driver, decode, "invalid picture geometry/data or failed CPU write");
}

static VAStatus ProcessDecodedOutput(Driver *driver, DecodeContext *decode,
                                     const BC_DTS_PROC_OUT &reported) {
  OutputProcessingState processing;
  try {
    return ProcessDecodedOutputImpl(driver, decode, reported, &processing);
  } catch (const std::bad_alloc &) {
    for (const auto &surface : {processing.picture, processing.surface}) {
      if (!surface)
        continue;
      surface->ready = false;
      surface->failed = true;
      RetireFailedExperimentalState(surface.get());
    }
    FailDecode(driver, decode, "decoded output allocation failed");
    return VA_STATUS_ERROR_ALLOCATION_FAILED;
  }
}

static VAStatus CompleteMpeg4EndOfStream(Driver *driver,
                                         DecodeContext *decode);

static VAStatus ReceiveOne(Driver *driver, DecodeContext *decode,
                           unsigned int timeout_ms, bool *activity) {
  BC_DTS_PROC_OUT output = {};
  output.PicInfo.width = decode->CaptureWidth();
  output.PicInfo.height = decode->CaptureHeight();
  *activity = false;

  BC_STATUS status = DtsProcOutputNoCopy(decode->device, timeout_ms, &output);
  Debug("DtsProcOutputNoCopy: status=%d flags=%#x ts=%llu ordinal=%u session=%u output=%ux%u context=%ux%u picture_flags=%#x y_words=%u", status,
        output.PoutFlags,
        static_cast<unsigned long long>(output.PicInfo.timeStamp),
        output.PicInfo.picture_number, output.PicInfo.sess_num,
        output.PicInfo.width, output.PicInfo.height, decode->width, decode->height,
        output.PicInfo.flags, output.YBuffDoneSz);
  if ((output.PoutFlags & BC_POUT_FLAGS_ENCRYPTED) != 0) {
    if (status == BC_STS_SUCCESS) {
      *activity = true;
      if (DtsReleaseOutputBuffs(decode->device, nullptr, FALSE) != BC_STS_SUCCESS)
        return FailDecode(driver, decode, "encrypted output release failed");
    }
    return FailDecode(driver, decode, "encrypted output is unsupported");
  }
  if ((output.PicInfo.flags & VDEC_FLAG_EOS) != 0) {
    // Only the actual firmware marker ends a sealed batch. Library idle/EOS
    // counters are not evidence that every submitted picture was produced.
    *activity = true;
    if (status == BC_STS_SUCCESS &&
        DtsReleaseOutputBuffs(decode->device, nullptr, FALSE) != BC_STS_SUCCESS)
      return FailDecode(driver, decode, "EOS output release failed");
    if (decode->auxiliary.pending)
      return FailDecode(driver, decode, "EOS before H.264 auxiliary output retired");
    if (decode->h264_first_field.valid)
      return FailDecode(driver, decode,
                        "EOS arrived with an incomplete H.264 field pair");
    // DIVX firmware can expose its EOS marker before the final reordered
    // picture becomes ready. Keep the batch sealed and continue polling; once
    // every accepted timestamp arrives, DtsIsEndOfStream (or a later marker)
    // provides the completion fence. A genuinely missing picture still hits
    // the bounded drain timeout instead of being silently discarded.
    if (decode->IsMpeg4() && !decode->Mpeg4TransportReplay().ended() &&
        decode->Mpeg4TransportReplay().outstanding() != 0) {
      Debug("early MPEG-4 EOS marker with %zu timestamps outstanding",
            decode->Mpeg4TransportReplay().outstanding());
      return VA_STATUS_SUCCESS;
    }
    // The DIVX library fallback can prove EOS without a firmware marker. If a
    // late marker nevertheless follows, it is only a duplicate completion of
    // the already-ended MPEG-4 batch.
    if (decode->IsMpeg4() && decode->Mpeg4TransportReplay().ended())
      return VA_STATUS_SUCCESS;
    if (!decode->auxiliary.eos_only &&
        !decode->WithReplay([](auto &replay) { return replay.EndOfSequence(); }))
      return FailDecode(driver, decode, decode->ReplayFailure());
    if (decode->auxiliary.ever) {
      Debug("H.264 auxiliary actual_eos token=%llu eos_only=%d",
                   static_cast<unsigned long long>(decode->auxiliary.timestamp),
                   decode->auxiliary.eos_only);
      decode->auxiliary.needs_eos = decode->auxiliary.eos_only = false;
      decode->auxiliary.ended = true;
    }
    // DtsFlushInput discards an unbound speculative prefix, but retain its
    // identity until the actual firmware EOS marker proves no picture escaped
    // from it. CloseHardware also clears this state on every abort/restart.
    decode->prefed_timestamp = 0;
    Debug("sealed batch EOS: every submitted timestamp completed");
    decode->CancelPendingPrefeed();
    return VA_STATUS_SUCCESS;
  }
  if (status == BC_STS_FMT_CHANGE) {
    *activity = true;
    return VA_STATUS_SUCCESS;
  }
  if (status == BC_STS_NO_DATA || status == BC_STS_BUSY ||
      status == BC_STS_TIMEOUT)
    return CompleteMpeg4EndOfStream(driver, decode);
  if (status != BC_STS_SUCCESS)
    return FailDecode(driver, decode, "hardware output failed");

  *activity = true;
  const size_t outstanding = decode->WithReplay(
      [](auto &replay) { return replay.outstanding(); });
  const bool had_first_field = decode->h264_first_field.valid;
  const bool auxiliary = decode->auxiliary.ever &&
      output.PicInfo.timeStamp == decode->auxiliary.timestamp;
  const VAStatus result = auxiliary
      ? ValidateH264AuxiliaryOutput(driver, decode, output)
      : ProcessDecodedOutput(driver, decode, output);
  status = DtsReleaseOutputBuffs(decode->device, nullptr, FALSE);
  if (status != BC_STS_SUCCESS)
    return FailDecode(driver, decode, "hardware output release failed");
  if (auxiliary && result == VA_STATUS_SUCCESS) {
    decode->auxiliary.pending = false;
    decode->RecordTransportProgress();
    Debug("H.264 auxiliary output_released token=%llu ordinal=%u session=%u meta=%u flags=%u layout=full_height",
                 static_cast<unsigned long long>(output.PicInfo.timeStamp),
                 output.PicInfo.picture_number, output.PicInfo.sess_num,
                 output.PicInfo.picture_meta_payload, output.PicInfo.flags);
    driver->condition.notify_all();
    return VA_STATUS_SUCCESS;
  }
  // A newly validated first field is real hardware progress even though its
  // picture must remain outstanding until the complementary field arrives.
  // Count it only after successful output release, never for unknown repeats
  // or a failed acquisition/pair. Do not publish picture readiness early.
  if (result == VA_STATUS_SUCCESS &&
      ((!had_first_field && decode->h264_first_field.valid) ||
       decode->WithReplay([](auto &replay) { return replay.outstanding(); }) < outstanding))
    decode->RecordTransportProgress();
  return result;
}

static VAStatus CompleteMpeg4EndOfStream(Driver *driver,
                                         DecodeContext *decode) {
  if (!decode->IsMpeg4())
    return VA_STATUS_SUCCESS;
  auto &replay = decode->Mpeg4TransportReplay();
  if (!replay.sealed() || replay.ended())
    return VA_STATUS_SUCCESS;
  if (replay.outstanding() != 0)
    return VA_STATUS_SUCCESS;
  uint8_t eos = 0;
  if (DtsIsEndOfStream(decode->device, &eos) != BC_STS_SUCCESS)
    return FailDecode(driver, decode, "MPEG-4 EOS state query failed");
  if (eos != 0) {
    if (!replay.EndOfSequence())
      return FailDecode(driver, decode, replay.failure());
    Debug("sealed MPEG-4 batch EOS: library drain completed without marker");
  }
  return VA_STATUS_SUCCESS;
}

static VAStatus ReceiveAvailable(Driver *driver, DecodeContext *decode,
                                 unsigned int max_attempts = 64) {
  if (decode->ReplayFailed())
    return VA_STATUS_ERROR_DECODING_ERROR;
  for (unsigned int attempt = 0; attempt < max_attempts; ++attempt) {
    BC_DTS_STATUS decoder_status = {};
    // BCM70012's peek filters repeated/garbage READY entries before reporting
    // them.  BCM70015 needs only the queue count here, so avoid synchronizing
    // its READY-head DMA buffer until the actual dequeue.
    if (!decode->is_70012)
      decoder_status.cpbEmptySize = kStatusSkipReadyHeadPeek;
    BC_STATUS status = DtsGetDriverStatus(decode->device, &decoder_status);
    if (status != BC_STS_SUCCESS || decoder_status.ReadyListCount != 0)
      Debug("DtsGetDriverStatus: status=%d ready=%u", status,
            decoder_status.ReadyListCount);
    if (status != BC_STS_SUCCESS)
      return decode->h264_first_field.valid
          ? FailDecode(driver, decode,
                       "hardware status failed during an H.264 field pair")
          : VA_STATUS_ERROR_DECODING_ERROR;
    if (decoder_status.ReadyListCount == 0)
      return CompleteMpeg4EndOfStream(driver, decode);
    bool activity = false;
    VAStatus va_status = ReceiveOne(driver, decode, 0, &activity);
    if (va_status != VA_STATUS_SUCCESS || !activity)
      return va_status;
  }
  return VA_STATUS_SUCCESS;
}

static VAStatus AdvanceMpeg4Epoch(Driver *driver, DecodeContext *decode) {
  if (!decode->IsMpeg4() || decode->mpeg4_draining_epochs.empty() ||
      !decode->mpeg4_draining_epochs.front().Drained())
    return VA_STATUS_SUCCESS;
  Debug("advance MPEG-4 discontinuity; %zu queued epochs remain",
        decode->mpeg4_draining_epochs.size());
  if (decode->CloseHardware() != BC_STS_SUCCESS)
    return FailDecode(driver, decode,
                      "hardware close for MPEG-4 epoch failed");
  decode->mpeg4_draining_epochs.pop_front();
  if (OpenDecoder(decode) != VA_STATUS_SUCCESS)
    return FailDecode(driver, decode,
                      "hardware reopen for MPEG-4 epoch failed");
  return VA_STATUS_SUCCESS;
}

static bool KnownFleaH264Transport(DecodeContext *decode) {
  if (!IsH264Profile(decode->profile) || decode->device == nullptr)
    return false;
  // Query the preopened public handle even before OpenDecoder has populated
  // is_70012. Unknown devices/versions cannot select a qualified allowance.
  BC_INFO_CRYSTAL version = {};
  return DtsCrystalHDVersion(decode->device, &version) == BC_STS_SUCCESS &&
      version.device == 1 && version.dilVersion.dilRelease == 3 &&
      version.dilVersion.dilMajor == 22 && version.dilVersion.dilMinor == 0;
}

static bool LegacyH264PesOneShot(DecodeContext *decode, size_t bytes) {
  return bytes <= crystalhd_access_unit_limits::kLegacyH264PesBytes &&
      KnownFleaH264Transport(decode);
}

static size_t H264SubmissionLimit(DecodeContext *decode, size_t bytes) {
  const size_t base = crystalhd_access_unit_limits::kVaH264SubmissionBytes;
  // frame_mbs_only excludes MBAFF/field-capable sequences; field_pic is an
  // additional guard, not a progressive/interlaced classification by itself.
  if (bytes > base && decode->picture.seq_fields.bits.frame_mbs_only_flag &&
      !decode->picture.pic_fields.bits.field_pic_flag &&
      KnownFleaH264Transport(decode))
    return crystalhd_access_unit_limits::kVaFleaProgressiveH264SubmissionBytes;
  return base;
}

static bool H264HeadroomDeescapedFits(const std::vector<uint8_t> &bytes) {
  constexpr size_t budget =
      crystalhd_access_unit_limits::kVaFleaProgressiveH264DeescapedBytes;
  if (bytes.size() <= budget)
    return true;
  size_t total = 0;
  const auto add = [&](size_t count) {
    if (count > budget - total)
      return false;
    total += count;
    return true;
  };
  const auto nal_fits = [&](size_t begin, size_t end) {
    unsigned int zeros = 0;
    for (size_t i = begin; i < end; ++i) {
      // Only unambiguous, nonterminal emulation prevention can reduce the
      // estimate. Count literal/terminal 03 and overlong zero runs as data.
      if (zeros == 2 && bytes[i] == 3 && i + 1 < end && bytes[i + 1] <= 3) {
        zeros = 0;
        continue;
      }
      if (!add(1))
        return false;
      zeros = bytes[i] == 0 ? std::min(zeros + 1, 3U) : 0;
    }
    return true;
  };
  // Count every reconstructed start code and NAL header. Partition first so
  // an escape-looking suffix cannot borrow a byte from the next NAL prefix.
  size_t begin = 0;
  for (size_t i = 0; i < bytes.size();) {
    size_t prefix = 0;
    if (bytes.size() - i >= 3 && bytes[i] == 0 && bytes[i + 1] == 0) {
      if (bytes[i + 2] == 1)
        prefix = 3;
      else if (bytes.size() - i >= 4 && bytes[i + 2] == 0 && bytes[i + 3] == 1)
        prefix = 4;
    }
    if (prefix != 0) {
      if (!nal_fits(begin, i) || !add(prefix))
        return false;
      i += prefix;
      begin = i;
    } else {
      ++i;
    }
  }
  return nal_fits(begin, bytes.size());
}

// Neutral I16x16/DC CAVLC picture. Never emits SPS or
// replaces PPS0; immutable accepted-AU syntax supplies frame_num and POC.
static bool BuildH264NeutralIntra(const VAPictureParameterBufferH264 &p,
                                std::vector<uint8_t> *bytes) {
  const auto &s = p.seq_fields.bits;
  const unsigned frame_bits = s.log2_max_frame_num_minus4 + 4;
  const unsigned poc_bits = s.log2_max_pic_order_cnt_lsb_minus4 + 4;
  const unsigned width = (static_cast<unsigned>(p.picture_width_in_mbs_minus1) + 1) * 16U;
  const unsigned height = (static_cast<unsigned>(p.picture_height_in_mbs_minus1) + 1) * 16U;
  if (width > 1920 || height > 1088 || (height & 31U) != 0 ||
      p.bit_depth_luma_minus8 || p.bit_depth_chroma_minus8 ||
      s.chroma_format_idc != 1 || s.residual_colour_transform_flag ||
      s.frame_mbs_only_flag || !s.mb_adaptive_frame_field_flag ||
      s.pic_order_cnt_type != 0 || p.pic_fields.bits.field_pic_flag ||
      frame_bits > 16 || poc_bits > 16 ||
      p.frame_num >= (1U << frame_bits) || p.CurrPic.TopFieldOrderCnt < 0)
    return false;
  bytes->clear();
  AppendNal(bytes, 9, 0, std::vector<uint8_t>{0x10});
  BitWriter pps;
  pps.PutUE(254); pps.PutUE(0);
  pps.PutBit(false); pps.PutBit(false);
  pps.PutUE(0); pps.PutUE(0); pps.PutUE(0);
  pps.PutBit(false); pps.PutBits(2, 0);
  pps.PutSE(0); pps.PutSE(0); pps.PutSE(0);
  pps.PutBit(true); pps.PutBit(false); pps.PutBit(false);
  AppendNal(bytes, 8, 3, pps.FinishRbsp());
  BitWriter slice;
  slice.PutUE(0); slice.PutUE(2); slice.PutUE(254);
  slice.PutBits(frame_bits, p.frame_num);
  slice.PutBit(false); // frame-coded MBAFF slice
  slice.PutBits(poc_bits, static_cast<uint32_t>(p.CurrPic.TopFieldOrderCnt) &
                             ((1U << poc_bits) - 1));
  slice.PutSE(0); slice.PutUE(1); // default QP, deblocking disabled
  for (unsigned mb = 0; mb < (width / 16U) * (height / 16U); ++mb) {
    if ((mb & 1U) == 0) slice.PutBit(false);
    slice.PutUE(3); slice.PutUE(0); slice.PutSE(0);
    slice.PutBit(true); // luma DC coeff_token TotalCoeff=0, nC=0
  }
  AppendNal(bytes, 1, 0, slice.FinishRbsp());
  return bytes->size() <= 16U * 1024U;
}

static VAStatus RetryPendingH264Prefeed(Driver *driver, DecodeContext *decode,
                                      bool *attempted) {
  if (decode->pending_prefeed_timestamp == 0)
    return VA_STATUS_SUCCESS;
  // A real AU wins before any speculative bytes are accepted. Eligibility
  // was captured at the preceding InputSent, never from mutable new metadata.
  if (!IsH264Profile(decode->profile) || decode->is_70012 ||
      !decode->device || !decode->decoder_started || decode->closing ||
      decode->destroy_prepared || decode->retired || decode->hardware_close_pending ||
      decode->pending_prefeed_generation != decode->generation ||
      decode->pending_prefeed_timestamp != decode->next_timestamp ||
      decode->prefed_timestamp != 0 || decode->partial_input_timestamp != 0 ||
      decode->replay.NextInput() != nullptr || !decode->replay.CanSeal()) {
    decode->CancelPendingPrefeed();
    return VA_STATUS_SUCCESS;
  }
  if (*attempted)
    return VA_STATUS_SUCCESS;
  *attempted = true; // At most one prefix attempt per bounded pump turn.
  const auto &prefeed = H264LowLatencyPrefeed();
  if (DtsTxFreeSize(decode->device) < prefeed.size() + 1024) {
    decode->input_backpressured = true;
    return VA_STATUS_SUCCESS;
  }
  const uint64_t timestamp = decode->pending_prefeed_timestamp;
  const BC_STATUS status = DtsProcInput(
      decode->device, const_cast<uint8_t *>(prefeed.data()),
      static_cast<uint32_t>(prefeed.size()), timestamp, FALSE);
  if (status == BC_STS_BUSY) {
    decode->input_backpressured = true;
    return VA_STATUS_SUCCESS;
  }
  if (status != BC_STS_SUCCESS)
    return FailDecode(driver, decode, "speculative H.264 prefix submission failed");
  decode->prefed_timestamp = timestamp;
  decode->CancelPendingPrefeed();
  decode->RecordTransportProgress();
  if (driver->trace.enabled())
    driver->trace.Emit(crystalhd_vaapi_trace::Event::PrefeedSent,
                       TraceDecodeTokenFields(driver, decode, timestamp));
  Debug("prefeed H.264 timestamp=%llu bytes=%zu",
        static_cast<unsigned long long>(timestamp), prefeed.size());
  return VA_STATUS_SUCCESS;
}

static VAStatus PumpDecodeInput(Driver *driver, DecodeContext *decode,
                                unsigned int max_attempts = 32) {
  decode->input_backpressured = false;
  decode->input_budget_exhausted = false;
  VAStatus advance = AdvanceMpeg4Epoch(driver, decode);
  if (advance != VA_STATUS_SUCCESS)
    return advance;
  bool prefeed_attempted = false;
  const VAStatus retry = RetryPendingH264Prefeed(driver, decode, &prefeed_attempted);
  if (retry != VA_STATUS_SUCCESS)
    return retry;
  return decode->WithReplay([&](auto &replay) -> VAStatus {
    if (replay.failed())
      return VA_STATUS_ERROR_DECODING_ERROR;
    if (replay.NeedsRestart()) {
      if (decode->auxiliary.ever)
        return FailDecode(driver, decode, "H.264 auxiliary forbids decoder replay/restart");
      // EOS terminates reference state. A qualified library may replace a fully
      // stopped/closed H.264 channel on its retained device; all other cases
      // retain full-device reopen. This is never the legacy channel-only FLUSH.
      // Immutable frames, client generation and actual replay input stay owned.
      Debug("restart sealed decoder; replay %zu cached access units",
            replay.cached_pictures());
      const BC_STATUS status = decode->CloseHardwareForH264Replay();
      if (status != BC_STS_SUCCESS)
        return FailDecode(driver, decode, "hardware close for replay failed");
      if (OpenDecoder(decode) != VA_STATUS_SUCCESS || !replay.Restarted())
        return FailDecode(driver, decode, "hardware reopen for replay failed");
    }

    // Large H.264 AUs use libcrystalhd's explicit logical-fragment contract.
    // FIRST alone performs AU parsing/timestamp/recovery bookkeeping; raw
    // continuations preserve a NAL split byte-for-byte. Each call is no larger
    // than one transport transfer, so this mutex is released between calls and
    // output can drain. Replay advances only after LAST was accepted.
    for (unsigned int attempt = 0; attempt < max_attempts; ++attempt) {
      const auto *unit = replay.NextInput();
      if (unit == nullptr)
        break;
      if (decode->auxiliary.ever && decode->auxiliary.ended)
        return FailDecode(driver, decode, "H.264 auxiliary forbids input after EOS");
      const uint64_t input_timestamp = unit->timestamp;
      bool prefeed_after_unit = false;
      if constexpr (std::is_same<std::decay_t<decltype(replay)>,
                                 CrystalHDDecodeReplay>::value)
        prefeed_after_unit = decode->low_latency_h264 || unit->prefeed;
      const bool consume_prefeed =
          decode->prefed_timestamp != 0 &&
          decode->prefed_timestamp == unit->timestamp;
      if (decode->prefed_timestamp != 0 &&
          !consume_prefeed)
        return FailDecode(driver, decode,
                          "speculative H.264 timestamp lost alignment");
      const bool bounded_h264 = IsH264Profile(decode->profile);
      size_t offset = 0;
      if (bounded_h264 && decode->partial_input_timestamp != 0) {
        if (decode->partial_input_timestamp != unit->timestamp ||
            decode->partial_input_offset >= unit->bytes.size())
          return FailDecode(driver, decode,
                            "partial H.264 input lost replay alignment");
        offset = decode->partial_input_offset;
      } else if (decode->partial_input_timestamp != 0 ||
                 decode->partial_input_offset != 0) {
        return FailDecode(driver, decode,
                          "partial input escaped its H.264 access unit");
      }
      const size_t remaining = unit->bytes.size() - offset;
      bool fragmented_h264 = bounded_h264 &&
          (offset != 0 || unit->bytes.size() >
                              crystalhd_access_unit_limits::kDtsInputChunkBytes);
      const CrystalHDFragmentInput fragment_input = H264FragmentInput();
      if (fragmented_h264 && fragment_input == nullptr) {
        // Never reinterpret an unfinished modern AU as a legacy tail. The
        // legacy public call owns one complete AU and one timestamp only.
        if (offset != 0 || !LegacyH264PesOneShot(decode, unit->bytes.size()))
          return FailDecode(driver, decode,
                            "large H.264 input requires fragmented libcrystalhd input");
        fragmented_h264 = false;
      }
      const size_t transfer = fragmented_h264
          ? std::min(remaining,
                     crystalhd_access_unit_limits::kDtsInputChunkBytes)
          : remaining;
      const size_t reserve =
          transfer + (transfer / 60000 + 1) * 32 + 1024;
      /* The additive fragment API performs its own exact, zero-byte
       * reservation. A legacy Flea PES call can contain multiple <=65526-byte
       * packets; reserve the WHOLE call before entering its blocking producer.
       * This VA mutex excludes other input/EOS/lifecycle producers on this
       * handle, and its DIL TX worker can only increase the available space. */
      if (!fragmented_h264 && DtsTxFreeSize(decode->device) < reserve) {
        decode->input_backpressured = true;
        break;
      }
      BC_STATUS status;
      if (fragmented_h264) {
        uint32_t flags = offset == 0 ? DTS_INPUT_FRAGMENT_FIRST : 0;
        if (offset + transfer == unit->bytes.size())
          flags |= DTS_INPUT_FRAGMENT_LAST;
        status = fragment_input(
            decode->device,
            const_cast<uint8_t *>(unit->bytes.data() + offset),
            static_cast<uint32_t>(transfer),
            (offset != 0 || consume_prefeed) ? 0 : unit->timestamp, FALSE,
            flags);
      } else {
        status = DtsProcInput(
            decode->device,
            const_cast<uint8_t *>(unit->bytes.data() + offset),
            static_cast<uint32_t>(transfer),
            consume_prefeed ? 0 : unit->timestamp, FALSE);
      }
      if (status == BC_STS_BUSY) {
        decode->input_backpressured = true;
        break;
      }
      if (status != BC_STS_SUCCESS)
        return FailDecode(driver, decode, "compressed input submission failed");
      decode->RecordTransportProgress();
      if (offset + transfer != unit->bytes.size()) {
        decode->partial_input_timestamp = unit->timestamp;
        decode->partial_input_offset = offset + transfer;
        if (attempt + 1 == max_attempts)
          decode->input_budget_exhausted = true;
        continue;
      }
      decode->partial_input_timestamp = 0;
      decode->partial_input_offset = 0;
      H264Auxiliary::Syntax auxiliary_candidate;
      bool have_auxiliary_candidate = false;
      if constexpr (std::is_same<std::decay_t<decltype(replay)>,
                                 CrystalHDDecodeReplay>::value) {
        // Never use mutable later picture metadata or a stale older AU.
        decode->auxiliary.candidate_timestamp = 0;
        auto syntax = decode->auxiliary.syntax.find(input_timestamp);
        if (syntax != decode->auxiliary.syntax.end()) {
          if (!unit->completed && !decode->is_70012 &&
              decode->live_h264_fast_path && decode->live_h264_gap_tolerant &&
              syntax->second.generation == decode->generation &&
              syntax->second.device == decode->device) {
            auxiliary_candidate = syntax->second;
            have_auxiliary_candidate = true;
          }
          decode->auxiliary.syntax.erase(syntax);
        }
      }
      if (!replay.InputSent())
        return FailDecode(driver, decode, "compressed input submission failed");
      if (have_auxiliary_candidate) {
        decode->auxiliary.candidate = auxiliary_candidate;
        decode->auxiliary.candidate_timestamp = input_timestamp;
        Debug("H.264 auxiliary candidate source=%llu generation=%llu syntax_only=1",
                     static_cast<unsigned long long>(input_timestamp),
                     static_cast<unsigned long long>(decode->generation));
      }
      if (driver->trace.enabled())
        driver->trace.Emit(crystalhd_vaapi_trace::Event::InputSent,
                           TraceDecodeTokenFields(driver, decode,
                                                  input_timestamp));
      if (consume_prefeed)
        decode->prefed_timestamp = 0;
      if (attempt + 1 == max_attempts)
        decode->input_budget_exhausted = true;

      if (prefeed_after_unit && !decode->is_70012 &&
          decode->prefed_timestamp == 0 && replay.NextInput() == nullptr) {
        const uint64_t next_timestamp = decode->next_timestamp;
        if (next_timestamp == 0)
          return FailDecode(driver, decode,
                            "speculative H.264 timestamp wrapped to zero");
        decode->pending_prefeed_timestamp = next_timestamp;
        decode->pending_prefeed_generation = decode->generation;
        const VAStatus prefeed_status =
            RetryPendingH264Prefeed(driver, decode, &prefeed_attempted);
        if (prefeed_status != VA_STATUS_SUCCESS)
          return prefeed_status;
      }
    }
    if (replay.failed())
      return FailDecode(driver, decode, replay.failure());
    return VA_STATUS_SUCCESS;
  });
}

static VAStatus SealDecodeBatch(Driver *driver, DecodeContext *decode) {
  decode->CancelPendingPrefeed();
  // The exact EOS marker path below is implemented for BCM70015. Keep
  // BCM70012 on its existing bounded-wait path until separately validated.
  if (decode->is_70012)
    return VA_STATUS_SUCCESS;
  return decode->WithReplay([&](auto &replay) -> VAStatus {
    if (!replay.CanSeal()) {
      // Auxiliary work is not a real replay unit. Only explicit close
      // may flush it when every real input/output has already completed.
      if (!decode->closing || !decode->auxiliary.needs_eos ||
          decode->auxiliary.eos_only || decode->ReplaySealed() ||
          replay.NextInput() != nullptr || replay.outstanding() != 0)
        return VA_STATUS_SUCCESS;
      if (DtsTxFreeSize(decode->device) < 1024) return VA_STATUS_SUCCESS;
      if (DtsFlushInput(decode->device, 0) != BC_STS_SUCCESS)
        return FailDecode(driver, decode, "H.264 auxiliary explicit EOS failed");
      decode->auxiliary.eos_only = true;
      Debug("H.264 auxiliary flush0 real_outstanding=0 auxiliary_pending=%d explicit_close=1",
                   decode->auxiliary.pending);
      return VA_STATUS_SUCCESS;
    }
    const uint32_t tx_free = DtsTxFreeSize(decode->device);
    if (tx_free < 1024)
      return VA_STATUS_SUCCESS;
    // CanSeal means all queued AUs were sent; cached pictures are history.
    Debug("seal batch on sync demand: %zu hardware timestamps outstanding "
          "context=%u generation=%llu cached=%zu partial_ts=%llu partial_offset=%zu "
          "prefed_ts=%llu backpressured=%u progress=%llu next_ordinal=%llu tx_free=%u",
          replay.outstanding(), static_cast<unsigned int>(decode->id),
          static_cast<unsigned long long>(decode->generation), replay.cached_pictures(),
          static_cast<unsigned long long>(decode->partial_input_timestamp),
          decode->partial_input_offset,
          static_cast<unsigned long long>(decode->prefed_timestamp),
          static_cast<unsigned int>(decode->input_backpressured),
          static_cast<unsigned long long>(decode->transport_progress),
          static_cast<unsigned long long>(decode->next_submission_ordinal), tx_free);
    if (!replay.Seal())
      return FailDecode(driver, decode, "could not seal decoder batch");
    if (DtsFlushInput(decode->device, 0) != BC_STS_SUCCESS)
      return FailDecode(driver, decode, "could not seal decoder batch");
    if (decode->auxiliary.ever)
      Debug("H.264 auxiliary flush0 real_outstanding=%zu auxiliary_pending=%d explicit_close=%d",
                   replay.outstanding(), decode->auxiliary.pending, decode->closing);
    return VA_STATUS_SUCCESS;
  });
}

static bool HasLivePendingPictures(const Driver *driver,
                                   const DecodeContext &decode) {
  for (const auto &pending : decode.pending) {
    auto surface = driver->surfaces.find(pending.second);
    if (surface != driver->surfaces.end() && !surface->second->destroyed &&
        surface->second->expected_timestamp == pending.first)
      return true;
  }
  return false;
}

// A submitted VA surface can outlive its decoder context: FFmpeg may destroy
// the decoder while a separate filter thread still holds undownloaded frames.
// Stop accepting input, but finish their actual pixels before releasing the
// only hardware session. This is context teardown, never an EndPicture wait.
static VAStatus DrainClosingContext(
    Driver *driver, const std::shared_ptr<DecodeContext> &decode,
    std::unique_lock<std::mutex> *driver_lock,
    uint64_t timeout_ns = kDecodeTimeoutNs) {
  if (driver_lock == nullptr || !driver_lock->owns_lock() || !decode->closing)
    return VA_STATUS_ERROR_INVALID_PARAMETER;
  if (decode->hardware_close_pending) {
    // Reset can retire every logical picture before a terminal DeviceClose
    // reports BUSY.  A following context destroy then has ownership to close,
    // but no decode work to fail.
    if (!HasLivePendingPictures(driver, *decode) && !decode->ReplaySealed() &&
        !decode->auxiliary.needs_eos)
      return VA_STATUS_SUCCESS;
    return FailDecode(driver, decode.get(),
                      "context has retained terminal hardware ownership");
  }
  const uint64_t generation = decode->generation;
  const auto start = std::chrono::steady_clock::now();
  while (HasLivePendingPictures(driver, *decode) || decode->ReplaySealed() ||
         decode->auxiliary.needs_eos) {
    if (decode->retired || decode->generation != generation)
      return VA_STATUS_ERROR_INVALID_CONTEXT;
    if (driver->stopping)
      return FailDecode(driver, decode.get(), "context drain canceled");
    const uint64_t elapsed =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count();
    if (elapsed >= timeout_ns)
      return FailDecode(driver, decode.get(), "context drain timed out");
    if (!decode->decoder_started)
      return FailDecode(driver, decode.get(), "context has no running decoder");
    VAStatus status = ReceiveAvailable(driver, decode.get());
    if (status == VA_STATUS_SUCCESS)
      status = PumpDecodeInput(driver, decode.get());
    if (status == VA_STATUS_SUCCESS)
      status = SealDecodeBatch(driver, decode.get());
    if (status != VA_STATUS_SUCCESS)
      return FailDecode(driver, decode.get(), "context drain failed");
    if (!HasLivePendingPictures(driver, *decode) && !decode->ReplaySealed() &&
        !decode->auxiliary.needs_eos)
      break;
    const std::shared_ptr<OutputPollLease> output_poll = decode->output_poll;
    driver_lock->unlock();
    if (output_poll) {
      pollfd descriptor = {output_poll->fd, POLLIN, 0};
      int result;
      do {
        result = poll(&descriptor, 1, 10);
      } while (result < 0 && errno == EINTR);
      if (result < 0 ||
          (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        driver_lock->lock();
        return FailDecode(driver, decode.get(),
                          "decoder output poll terminated during drain");
      }
    } else {
      usleep(1000);
    }
    driver_lock->lock();
  }
  return VA_STATUS_SUCCESS;
}

// Compare only active sequence/entry-point state, never padding or dynamic
// picture-level motion/quantization fields. WMV3 metadata stays fixed for a
// context. Advanced can carry a new configuration at an I picture: every I AU
// owns its complete headers, so replay restores each original transition.
static std::vector<uint32_t> Vc1Configuration(const VAPictureParameterBufferVC1 &p) {
  const auto &range = p.range_mapping_fields.bits;
  const uint32_t sequence = p.sequence_fields.bits.profile == 3
      ? p.sequence_fields.value & 0x305fU : p.sequence_fields.value & 0x3fffU;
  return {sequence, p.entrypoint_fields.value & 15U, p.fast_uvmc_flag,
          p.mv_fields.bits.extended_mv_flag, p.mv_fields.bits.extended_dmv_flag,
          p.pic_quantizer_fields.bits.dquant, p.pic_quantizer_fields.bits.quantizer,
          p.transform_fields.bits.variable_sized_transform_flag,
          p.reference_fields.bits.reference_distance_flag,
          range.luma_flag, range.luma_flag ? range.luma : 0U,
          range.chroma_flag, range.chroma_flag ? range.chroma : 0U};
}

static bool PublicReadRejected(const Surface *owner) {
  return owner->rejected_picture_transaction != 0 &&
      owner->rejected_picture_transaction == owner->picture_transaction;
}

static bool CompletedDecodePictureMatches(
    const Driver *driver, const std::shared_ptr<Surface> &owner,
    const CompletedDecodePicture &completed, bool require_private = true) {
  const auto current = driver->surfaces.find(completed.surface);
  const auto &picture = completed.picture;
  return owner && completed.owner.lock() == owner &&
      current != driver->surfaces.end() && current->second == owner &&
      owner->backing_owner == VA_INVALID_SURFACE && !owner->destroyed &&
      owner->ready && !owner->failed && !PublicReadRejected(owner.get()) &&
      owner->rejected_picture_transaction == completed.rejection_history &&
      owner->vpp_writers == 0 && !owner->image_write_locked &&
      !owner->experimental_presentation && !owner->experimental_write_lease &&
      !owner->experimental_read_export_armed &&
      completed.timestamp != 0 && completed.identity != 0 &&
      owner->expected_timestamp == completed.timestamp &&
      owner->frame_timestamp == completed.timestamp &&
      owner->decode_identity == completed.identity &&
      owner->trace_decode_identity == completed.identity &&
      owner->trace_context == completed.context &&
      owner->trace_generation == completed.generation &&
      owner->trace_submission_ordinal == completed.ordinal &&
      picture && picture.get() != owner.get() &&
      owner->decode_picture.lock() == picture &&
      !picture->destroyed && picture->ready && !picture->failed &&
      picture->expected_timestamp == completed.timestamp &&
      picture->frame_timestamp == completed.timestamp &&
      picture->decode_identity == completed.identity &&
      picture->trace_decode_identity == completed.identity &&
      picture->trace_context == completed.context &&
      picture->trace_generation == completed.generation &&
      picture->trace_submission_ordinal == completed.ordinal &&
      picture->width == owner->width && picture->height == owner->height &&
      picture->fourcc == VA_FOURCC_NV12 &&
      (!picture->direct_backing ||
       (!require_private && picture->direct_backing == owner));
}

static VAStatus PrepareCompletedDecodePictures(Driver *driver,
                                               DecodeContext *decode) {
  // Build the complete candidate set before publishing any new ownership.
  // A partial promotion is harmless, but a failure must grant no read lease.
  decode->retirement_candidates.clear();
  std::vector<CompletedDecodePicture> candidates;
  try {
    candidates.reserve(decode->surface_timestamps.size());
    for (const auto &entry : driver->surfaces) {
      const auto &owner = entry.second;
      const auto token = decode->surface_timestamps.find(owner.get());
      if (token == decode->surface_timestamps.end())
        continue;
      const auto picture = decode->decoded_frames.find(token->second);
      if (picture == decode->decoded_frames.end())
        continue;
      CompletedDecodePicture completed;
      completed.owner = owner;
      completed.picture = picture->second;
      completed.surface = entry.first;
      completed.timestamp = token->second;
      completed.identity = picture->second->decode_identity;
      completed.rejection_history = owner->rejected_picture_transaction;
      completed.context = TraceId(decode->id, VA_INVALID_ID);
      completed.generation = decode->generation;
      completed.ordinal = picture->second->trace_submission_ordinal;
      if (!CompletedDecodePictureMatches(driver, owner, completed, false))
        continue;
      // A public strong lease on a direct-backed picture would form a cycle.
      // Promote while the exact owner and its pixels are still available.
      if (!PromoteDecodedPicture(picture->second) ||
          !CompletedDecodePictureMatches(driver, owner, completed))
        return VA_STATUS_ERROR_OPERATION_FAILED;
      candidates.push_back(std::move(completed));
    }
  } catch (const std::bad_alloc &) {
    return VA_STATUS_ERROR_ALLOCATION_FAILED;
  }
  decode->retirement_candidates = std::move(candidates);
  return VA_STATUS_SUCCESS;
}

static std::shared_ptr<Surface> MarkRejectedPictureTransaction(
    Driver *driver, DecodeContext *decode) {
  const auto target = decode->transaction_target.lock();
  const auto current = driver->surfaces.find(decode->target);
  if (!target || current == driver->surfaces.end() || current->second != target ||
      target->destroyed || target->backing_owner != VA_INVALID_SURFACE ||
      target->picture_transaction != decode->target_transaction ||
      target->decode_identity != decode->target_identity)
    return {};
  // This allocation-free public-read barrier is independent of accepted
  // storage/readers. A late old completion cannot clear it; only a new writer
  // transaction supersedes it. Never consult it for captured private reads.
  target->completed_decode_picture = {};
  target->rejected_picture_transaction = target->picture_transaction;
  return target;
}

static VAStatus RejectH264Target(Driver *driver, DecodeContext *decode,
                                 VAStatus status) {
  const auto target = MarkRejectedPictureTransaction(driver, decode);
  if (!target || target->vpp_readers != 0 || target->vpp_writers != 0 ||
      target->export_waiters != 0 || target->image_write_locked ||
      target->experimental_read_export_armed || target->experimental_presentation)
    return status;
  if (target->experimental_write_lease &&
      (!decode->experimental_ready_remap ||
       target->experimental_write_context != TraceId(decode->id, VA_INVALID_ID) ||
       target->experimental_write_generation != decode->generation ||
       target->experimental_write_submission_ordinal !=
           decode->next_submission_ordinal))
    return status;

  // Reject the NEW public transaction, not previously accepted private output.
  // A pending or captured direct picture needs independent storage before the
  // public owner is revoked. Allocation failure must leave that binding intact.
  try {
    const auto picture = target->decode_picture.lock();
    if (picture && picture->direct_backing) {
      // Unlike ordinary reuse, rejection must not poison the earlier accepted
      // picture if allocation or reading its backing fails. Stage the entire
      // promotion before changing that picture's planes or ownership.
      if (picture->direct_backing != target)
        return VA_STATUS_ERROR_OPERATION_FAILED;
      Surface preserved;
      if (!preserved.AllocateInternal(nullptr, -1, picture->width,
                                       picture->height, VA_FOURCC_NV12) ||
          (picture->ready && !picture->failed &&
           (target->frame_timestamp != picture->frame_timestamp ||
            !CopyNv12Surface(*target, &preserved))))
        return VA_STATUS_ERROR_OPERATION_FAILED;
      picture->storage = std::move(preserved.storage);
      for (unsigned int plane = 0; plane < 2; ++plane) {
        picture->pitch[plane] = preserved.pitch[plane];
        picture->planes[plane] = preserved.planes[plane];
      }
      picture->direct_backing.reset();
    }
  } catch (const std::bad_alloc &) {
    return VA_STATUS_ERROR_ALLOCATION_FAILED;
  }
  EndTraceBinding(driver, target.get());
  RevokeDecodeOwner(driver, target.get());
  SetSurfaceState(driver, target.get(), false, true);
  driver->condition.notify_all();
  return status;
}

static VAStatus RejectH264Picture(Driver *driver, DecodeContext *decode, VAStatus status,
                                  const uint8_t *rejected = nullptr,
                                  size_t rejected_size = 0) {
  const auto reference_vcl = [](const uint8_t *data, size_t size) {
    if (data == nullptr || size == 0)
      return false;
    const unsigned type = data[0] & 0x1fU;
    return type == 5 ||
           (type >= 1 && type <= 4 && (data[0] & 0x60U) != 0);
  };
  if (reference_vcl(rejected, rejected_size) ||
      std::any_of(decode->slice_data.begin(), decode->slice_data.end(),
                  [&](const std::vector<uint8_t> &slice) {
                    return reference_vcl(slice.data(), slice.size());
                  }))
    decode->h264_waiting_for_idr = true;
  return RejectH264Target(driver, decode, status);
}

static bool UnobservedPendingCpuBacking(
    const DecodeContext &decode, const std::shared_ptr<Surface> &picture,
    VASurfaceID target, unsigned int width, unsigned int height) {
  // The map and this submission's local candidate must be the ONLY owners.
  // Captured private/VPP readers retain their full backing until final release.
  if (!picture || picture.use_count() != 2 || !decode.private_backing_pool)
    return false;
  const Surface &old = *picture;
  const auto pending = decode.pending.find(old.expected_timestamp);
  const auto found = decode.decoded_frames.find(old.expected_timestamp);
  if (old.id != target || old.expected_timestamp == 0 ||
      pending == decode.pending.end() || pending->second != target ||
      found == decode.decoded_frames.end() || found->second != picture ||
      old.decode_identity == 0 || old.trace_decode_identity != old.decode_identity ||
      old.trace_context != TraceId(decode.id, VA_INVALID_ID) ||
      old.trace_generation != decode.generation || old.trace_submission_ordinal == 0 ||
      old.destroyed || old.ready || old.failed || old.frame_timestamp != 0 ||
      old.vpp_readers != 0 || old.vpp_writers != 0 || old.image_write_locked ||
      old.export_waiters != 0 || old.backing_owner != VA_INVALID_SURFACE ||
      old.direct_picture || old.direct_backing || !old.decode_target.expired() ||
      !old.decode_picture.expired() || old.completed_decode_picture.picture ||
      old.experimental_presentation || old.experimental_write_lease ||
      old.experimental_ready_queued || old.experimental_read_export_armed ||
      old.trace_read_export_armed || old.layout.valid || old.bo != nullptr ||
      old.dumb_drm_fd >= 0 || old.dumb_handle != 0 || old.owned_i915_dumb ||
      old.experimental_i915_wc || old.map_data[0] || old.map_data[1] ||
      !old.object_maps.empty() || !old.object_sizes.empty() || !old.object_fds.empty() ||
      old.private_backing_pool.lock() != decode.private_backing_pool ||
      old.fourcc != VA_FOURCC_NV12 || old.width != width || old.height != height ||
      width == 0 || height == 0 || width > UINT_MAX - 63U)
    return false;
  const size_t stride = Align(width, 64);
  const size_t chroma_rows = height / 2U + height % 2U;
  if (height > SIZE_MAX - chroma_rows ||
      stride > SIZE_MAX / (height + chroma_rows))
    return false;
  const size_t bytes = stride * (height + chroma_rows);
  return old.pitch[0] == stride && old.pitch[1] == stride &&
      old.storage.size() == bytes &&
      old.planes[0] == old.storage.data() &&
      old.planes[1] == old.storage.data() + stride * height;
}

static VAStatus SubmitPicture(Driver *driver, DecodeContext *decode,
                              VAProfile profile) {
  if (decode->IsMpeg4() && decode->mpeg4_replay.failed())
    return VA_STATUS_ERROR_DECODING_ERROR;
  if (IsH264Profile(profile) &&
      (decode->h264_picture_invalid || !decode->have_picture ||
       decode->slices.empty() || decode->slice_data.empty() ||
       decode->target == VA_INVALID_SURFACE || !decode->iq_matrix_valid ||
       !ValidateH264IqMatrix(decode->picture, profile, decode->iq_matrix)))
    return VA_STATUS_ERROR_INVALID_PARAMETER;
  const bool idr = IsH264Profile(profile) &&
                   !decode->slice_data.front().empty() &&
                   (decode->slice_data.front().front() & 0x1fU) == 5;
  if (IsH264Profile(profile) && decode->h264_waiting_for_idr && !idr) {
    Debug("H.264 input requires an actual IDR after a rejected reference picture");
    return RejectH264Target(driver, decode, VA_STATUS_ERROR_DECODING_ERROR);
  }
  if (decode->experimental_ready_remap &&
      std::any_of(decode->slices.begin(), decode->slices.end(),
                  [](const VASliceParameterBufferH264 &slice) {
                    return slice.slice_type % 5 == 1;
                  }))
    return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;

  const auto original = driver->surfaces.find(decode->target);
  if (original == driver->surfaces.end())
    return VA_STATUS_ERROR_INVALID_SURFACE;
  const std::shared_ptr<Surface> original_surface = original->second;
  const VASurfaceID target_id = decode->decode_processing
      ? decode->processing_target : decode->target;
  auto surface = driver->surfaces.find(target_id);
  if (surface == driver->surfaces.end() ||
      !HasLiveBackingOwner(driver, surface->second.get()) ||
      surface->second->backing_owner != VA_INVALID_SURFACE)
    return VA_STATUS_ERROR_INVALID_SURFACE;
  const std::shared_ptr<Surface> target_surface = surface->second;
  if (decode->decode_processing) {
    if (decode->picture.seq_fields.bits.chroma_format_idc != 1 ||
        decode->picture.seq_fields.bits.residual_colour_transform_flag ||
        decode->picture.bit_depth_luma_minus8 != 0 ||
        decode->picture.bit_depth_chroma_minus8 != 0 ||
        decode->picture.pic_fields.bits.field_pic_flag ||
        (!decode->picture.seq_fields.bits.frame_mbs_only_flag &&
         !decode->picture.seq_fields.bits.mb_adaptive_frame_field_flag))
      return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    const auto &geometry = decode->processing_request;
    if (!IsH264Profile(profile) || !decode->have_processing_parameters ||
        !geometry.enabled || target_id == decode->target ||
        !H264ProcessingSurfaceEligible(*original_surface, decode->id, true) ||
        !H264ProcessingSurfaceEligible(*target_surface, decode->id, false) ||
        decode->transaction_target.lock() != original_surface ||
        original_surface->picture_transaction != decode->target_transaction ||
        original_surface->decode_identity != decode->target_identity ||
        target_surface->picture_transaction != decode->processing_target_transaction ||
        target_surface->decode_identity != decode->processing_target_identity ||
        original_surface->width != decode->width ||
        original_surface->height != decode->height ||
        target_surface->width != geometry.capture_width ||
        target_surface->height != geometry.capture_height ||
        !ValidateH264ProcessingGeometry(
            decode->width, decode->height, geometry.visible_width,
            geometry.visible_height, geometry.capture_width,
            geometry.capture_height) ||
        (decode->processing_geometry.enabled &&
         !(decode->processing_geometry == geometry)))
      return VA_STATUS_ERROR_INVALID_PARAMETER;
  } else if (original_surface->opaque_reference) {
    return VA_STATUS_ERROR_INVALID_SURFACE;
  }
  if (target_surface->vpp_writers != 0 || target_surface->vpp_readers != 0 ||
      target_surface->export_waiters != 0)
    return VA_STATUS_ERROR_HW_BUSY;
  // BeginPicture and EndPicture are separate public calls.  Another context
  // may acquire this backing between them, so an ordinary submission must
  // revalidate that no experimental reservation, presentation, or external
  // reader now owns it before changing trace/decode identity.
  if (!decode->experimental_ready_remap &&
      (ExperimentalPresentationBinding(driver, target_surface.get()) ||
       target_surface->experimental_write_lease ||
       target_surface->experimental_read_export_armed ||
       target_surface->experimental_presentation))
    return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
  if (decode->experimental_ready_remap &&
      (!target_surface->direct_decode_eligible ||
       target_surface->experimental_incompatible ||
       !target_surface->experimental_write_lease ||
       target_surface->experimental_write_context !=
           TraceId(decode->id, VA_INVALID_ID) ||
       target_surface->experimental_write_generation != decode->generation ||
       target_surface->experimental_write_submission_ordinal !=
           decode->next_submission_ordinal))
    return VA_STATUS_ERROR_SURFACE_BUSY;
  std::vector<uint8_t> bitstream;
  bool mpeg4_discontinuity = false;
  const uint64_t timestamp = decode->next_timestamp;
  crystalhd_mpeg2::QuantMatrices next_matrices;
  crystalhd_mpeg4::TimingState next_mpeg4_timing = decode->mpeg4_timing;
  std::vector<uint32_t> next_vc1_configuration;
  std::array<uint8_t, 4> next_wmv3_metadata = {};
  unsigned int stream_width = decode->width;
  unsigned int stream_height = decode->height;
  unsigned int h264_crop_unit_y = 0;
  if (decode->IsMpeg2()) {
    const auto &pending = decode->mpeg2_picture;
    const auto &picture = pending.picture;
    if (pending.invalid || !pending.have_picture || pending.parameters.empty() ||
        pending.parameters.size() != pending.data.size())
      return VA_STATUS_ERROR_INVALID_PARAMETER;
    stream_width = picture.horizontal_size;
    stream_height = picture.vertical_size;
    if (((stream_width + 15U) & ~15U) != ((decode->width + 15U) & ~15U) ||
        ((stream_height + 15U) & ~15U) != ((decode->height + 15U) & ~15U) ||
        (decode->stream_width != 0 &&
         (stream_width != decode->stream_width || stream_height != decode->stream_height)))
      return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (target_surface->width < stream_width ||
        target_surface->height < stream_height)
      return VA_STATUS_ERROR_INVALID_SURFACE;
    if ((picture.picture_coding_type == 2 &&
         pending.forward != decode->mpeg2_replay.newest_anchor()) ||
        (picture.picture_coding_type == 3 &&
         (pending.forward != decode->mpeg2_replay.previous_anchor() ||
          pending.backward != decode->mpeg2_replay.newest_anchor())))
      return VA_STATUS_ERROR_INVALID_PARAMETER;
    std::vector<crystalhd_mpeg2::Slice> slices;
    for (size_t group = 0; group < pending.parameters.size(); ++group)
      for (const auto &parameters : pending.parameters[group])
        slices.push_back({parameters, pending.data[group].data(), pending.data[group].size()});
    const crystalhd_mpeg2::SequenceOptions sequence = {
        profile, profile == VAProfileMPEG2Simple ? 8U : 4U, true, false,
        1, 5, 0, 0, 0x3ffff, 112};
    // VA provides neither original temporal_reference nor frame rate/GOP
    // headers. Firmware picture identity comes from the exact input timestamp;
    // its I/P/B ordering is reference-driven. Keep reconstructed timing stable
    // over EOS replay, without inferring a closed GOP from an I picture.
    const crystalhd_mpeg2::PictureOptions options = {0, picture.picture_coding_type == 1};
    if (!crystalhd_mpeg2::Assemble(picture, pending.have_iq ? &pending.iq : nullptr,
        decode->mpeg2_matrices, slices, sequence, options, &bitstream, &next_matrices)) {
      Debug("MPEG-2 assembly rejected type=%d flags=%#x f_code=%#x size=%ux%u slices=%zu",
            picture.picture_coding_type, picture.picture_coding_extension.value,
            picture.f_code, stream_width, stream_height, slices.size());
      return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
  } else if (decode->IsMpeg4()) {
    const auto &pending = decode->mpeg4_picture;
    const auto &picture = pending.picture;
    const unsigned type = picture.vop_fields.bits.vop_coding_type;
    if (pending.invalid || !pending.have_picture || pending.have_iq ||
        pending.parameters.size() != 1 || pending.data.size() != 1 ||
        type > 2)
      return VA_STATUS_ERROR_INVALID_PARAMETER;
    stream_width = picture.vop_width;
    stream_height = picture.vop_height;
    if (((stream_width + 15U) & ~15U) !=
            ((decode->width + 15U) & ~15U) ||
        ((stream_height + 15U) & ~15U) !=
            ((decode->height + 15U) & ~15U) ||
        (decode->stream_width != 0 &&
         (stream_width != decode->stream_width ||
          stream_height != decode->stream_height)))
      return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (target_surface->width < stream_width ||
        target_surface->height < stream_height)
      return VA_STATUS_ERROR_INVALID_SURFACE;
    if ((profile == VAProfileMPEG4Simple && type == 2) ||
        (type == 1 &&
         pending.forward != decode->mpeg4_replay.newest_anchor()) ||
        (type == 2 &&
         (pending.forward != decode->mpeg4_replay.previous_anchor() ||
          pending.backward != decode->mpeg4_replay.newest_anchor())))
      return VA_STATUS_ERROR_INVALID_PARAMETER;
    // FFmpeg reports a negative TRD after a backward seek, and can report zero
    // on the first Advanced Simple random-access I after a flush. Simple
    // Profile has no B-VOP interval to preserve, and some clients legitimately
    // omit TRD for every anchor, so its zero uses the assembler's monotonic
    // fallback. A real discontinuity starts a new accepting epoch; older
    // epochs drain in transport order without blocking vaEndPicture.
    mpeg4_discontinuity =
        type == 0 && decode->mpeg4_timing.have_newest &&
        (picture.TRD < 0 ||
         (profile == VAProfileMPEG4AdvancedSimple && picture.TRD == 0));
    const crystalhd_mpeg4::Slice slice = {
        pending.parameters.front(), pending.data.front().data(),
        pending.data.front().size()};
    const crystalhd_mpeg4::TimingState empty_timing;
    const auto &assembly_timing =
        mpeg4_discontinuity ? empty_timing : decode->mpeg4_timing;
    if (!crystalhd_mpeg4::Assemble(
            picture, profile, timestamp, pending.forward, pending.backward,
            slice, assembly_timing, &bitstream, &next_mpeg4_timing)) {
      Debug("MPEG-4 assembly rejected profile=%d type=%u flags=%#x/%#x "
            "TRB=%d TRD=%d refs=%llu/%llu size=%ux%u slice=%zu",
            profile, type, picture.vol_fields.value,
            picture.vop_fields.value, picture.TRB, picture.TRD,
            static_cast<unsigned long long>(pending.forward),
            static_cast<unsigned long long>(pending.backward),
            stream_width, stream_height,
            pending.data.front().size());
      return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
  } else if (decode->IsVc1()) {
    const auto &pending = decode->vc1_picture;
    const auto &picture = pending.picture;
    const unsigned type = picture.picture_fields.bits.picture_type;
    if (pending.invalid || !pending.have_picture || pending.parameters.empty() ||
        pending.parameters.size() != pending.data.size() || type > 4)
      return VA_STATUS_ERROR_INVALID_PARAMETER;
    stream_width = picture.coded_width;
    stream_height = picture.coded_height;
    if (((stream_width + 15U) & ~15U) != ((decode->width + 15U) & ~15U) ||
        ((stream_height + 15U) & ~15U) != ((decode->height + 15U) & ~15U) ||
        (decode->stream_width != 0 &&
         (stream_width != decode->stream_width || stream_height != decode->stream_height)))
      return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (target_surface->width < stream_width ||
        target_surface->height < stream_height)
      return VA_STATUS_ERROR_INVALID_SURFACE;
    if (((type == 1 || type == 4) &&
         pending.forward != decode->vc1_replay.newest_anchor()) ||
        (type == 2 &&
         (pending.forward != decode->vc1_replay.previous_anchor() ||
          pending.backward != decode->vc1_replay.newest_anchor())) ||
        (type != 0 && decode->vc1_replay.newest_anchor() == 0))
      return VA_STATUS_ERROR_INVALID_PARAMETER;
    next_vc1_configuration = Vc1Configuration(picture);
    if (!decode->vc1_configuration.empty() &&
        decode->vc1_configuration != next_vc1_configuration &&
        (profile != VAProfileVC1Advanced || type != 0))
      return VA_STATUS_ERROR_INVALID_PARAMETER;
    const uint8_t *planes = pending.bitplanes.empty() ? nullptr : pending.bitplanes.data();
    bool assembled = false;
    if (profile == VAProfileVC1Advanced) {
      std::vector<crystalhd_vc1::Slice> slices;
      for (size_t group = 0; group < pending.parameters.size(); ++group)
        for (const auto &parameters : pending.parameters[group])
          slices.push_back({parameters, pending.data[group].data(), pending.data[group].size()});
      // VA has no original level, rate hints or display timing extension.
      // Fixed level 3 covers this backend's bounded progressive geometry;
      // these hints do not replace the caller's presentation timestamps.
      const crystalhd_vc1::SequenceOptions sequence = {3, 5, 31};
      assembled = crystalhd_vc1::Assemble(picture, planes, pending.bitplanes.size(),
                                         slices, sequence, type == 0, &bitstream);
    } else {
      std::vector<crystalhd_wmv3::Slice> slices;
      for (size_t group = 0; group < pending.parameters.size(); ++group)
        for (const auto &parameters : pending.parameters[group])
          slices.push_back({parameters, pending.data[group].data(), pending.data[group].size()});
      assembled = crystalhd_wmv3::Assemble(picture, profile, planes, pending.bitplanes.size(),
                                          slices, &bitstream, &next_wmv3_metadata);
    }
    if (!assembled) {
      Debug("VC-1 assembly rejected profile=%d type=%u size=%ux%u groups=%zu",
            profile, type, stream_width, stream_height, pending.parameters.size());
      return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
  } else {
    // VA contexts and H.264 picture dimensions are coded sizes. A target may
    // expose the smaller visible rectangle when only the final macroblock is
    // cropped; the uncropped SPS keeps hardware output at coded geometry and
    // the bounded copy clips it to that target. In-context coded-geometry
    // changes remain unsupported.
    if ((decode->picture.picture_width_in_mbs_minus1 + 1U) * 16U != decode->width ||
        (decode->picture.picture_height_in_mbs_minus1 + 1U) * 16U != decode->height)
      return VA_STATUS_ERROR_INVALID_PARAMETER;
    h264_crop_unit_y =
        decode->picture.seq_fields.bits.frame_mbs_only_flag ? 2U : 4U;
    if (!FitsH264CodedGeometry(*original_surface, decode->width,
                               decode->height, h264_crop_unit_y)) {
      Debug("H.264 target outside coded grid id=%u surface=%ux%u "
            "aligned=%ux%u coded=%ux%u crop_unit_y=%u",
            target_id, target_surface->width, target_surface->height,
            Align(target_surface->width, 16),
            Align(target_surface->height, 16),
            decode->width, decode->height, h264_crop_unit_y);
      return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    if (decode->decode_processing &&
        (((decode->height - decode->processing_request.visible_height) %
              h264_crop_unit_y) != 0 ||
         (decode->processing_request.capture_height % h264_crop_unit_y) != 0 ||
         (!decode->picture.seq_fields.bits.frame_mbs_only_flag &&
          decode->processing_request.capture_width % 16U != 0)))
      return VA_STATUS_ERROR_INVALID_PARAMETER;

    const auto build_h264 = [&]() -> VAStatus {
      try {
        std::vector<uint8_t> assembled;
        BeginAccessUnit(&assembled);
        // An ordinary GOP can start with an IDR while earlier output is still
        // pending. Let the bitstream's IDR semantics handle reference pictures;
        // discarding pending output here loses real frames during playback.
        if (!decode->sent_parameter_sets || idr) {
          if (!BuildSps(decode->picture, profile, &assembled,
                        decode->decode_processing
                            ? decode->processing_request.visible_width : 0,
                        decode->decode_processing
                            ? decode->processing_request.visible_height : 0))
            return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
        }
        // VA exposes active reference counts on each slice instead of the
        // original PPS defaults. Redefine PPS id 0 for every access unit.
        if (!BuildPps(decode->picture, decode->slices.front(), profile,
                      decode->iq_matrix, &assembled))
          return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;

        static constexpr uint8_t start_code[] = {0, 0, 0, 1};
        if (assembled.size() > crystalhd_access_unit_limits::kH264Bytes)
          return VA_STATUS_ERROR_INVALID_PARAMETER;
        for (const std::vector<uint8_t> &slice : decode->slice_data) {
          const size_t remaining =
              crystalhd_access_unit_limits::kH264Bytes - assembled.size();
          if (remaining < sizeof(start_code) ||
              slice.size() > remaining - sizeof(start_code))
            return VA_STATUS_ERROR_INVALID_PARAMETER;
          assembled.insert(assembled.end(), std::begin(start_code),
                           std::end(start_code));
          assembled.insert(assembled.end(), slice.begin(), slice.end());
        }
        bitstream.swap(assembled);
        return VA_STATUS_SUCCESS;
      } catch (const std::bad_alloc &) {
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
      }
    };
    const VAStatus h264_status = build_h264();
    if (h264_status != VA_STATUS_SUCCESS)
      return h264_status == VA_STATUS_ERROR_INVALID_PARAMETER
          ? RejectH264Picture(driver, decode, h264_status) : h264_status;
    // The assembly allocation bound is not hardware acceptance evidence.
    // Reject unqualified complete AUs before replay/picture identity changes;
    // sending one can suppress all later output, including the next IDR.
    const size_t submission_limit = H264SubmissionLimit(decode, bitstream.size());
    if (bitstream.size() > submission_limit) {
      Debug("H.264 access unit exceeds VA submission policy: bytes=%zu limit=%zu",
            bitstream.size(), submission_limit);
      return RejectH264Picture(driver, decode, VA_STATUS_ERROR_INVALID_PARAMETER);
    }
    // Only constrain the separately qualified extended envelope. The ordinary
    // baseline is preserved; this is containment, not universal AU acceptance.
    // #468 measures signed-firmware CDB retention loss for a large single NAL.
    // Affected input needs caller-selected software decode, not a longer wait.
    if (bitstream.size() > crystalhd_access_unit_limits::kVaH264SubmissionBytes &&
        !H264HeadroomDeescapedFits(bitstream)) {
      Debug("H.264 extended input exceeds de-escaped admission policy: "
            "assembled_bytes=%zu deescaped_budget=%zu", bitstream.size(),
            crystalhd_access_unit_limits::kVaFleaProgressiveH264DeescapedBytes);
      return RejectH264Picture(driver, decode, VA_STATUS_ERROR_INVALID_PARAMETER);
    }
    if (bitstream.size() > crystalhd_access_unit_limits::kVaH264SubmissionBytes)
      Debug("H.264 access unit within VA submission policy: bytes=%zu limit=%zu",
            bitstream.size(), submission_limit);
    // An older library cannot carry this complete AU. Reject the current
    // transaction before allocating/binding a picture or committing replay;
    // failing in PumpDecodeInput would also poison earlier retained output.
    if (bitstream.size() > crystalhd_access_unit_limits::kDtsInputChunkBytes &&
        H264FragmentInput() == nullptr &&
        !LegacyH264PesOneShot(decode, bitstream.size())) {
      Debug("large H.264 input requires fragmented libcrystalhd input");
      return RejectH264Picture(driver, decode, VA_STATUS_ERROR_INVALID_PARAMETER);
    }
    Debug("submit surface=%u bytes=%zu slices=%zu", decode->target,
          bitstream.size(), decode->slice_data.size());
    if (decode->next_timestamp == kTimestampStep) {
      Debug("picture profile=%d poc_type=%u refs=%u size_mbs=%ux%u entropy=%u",
            profile, decode->picture.seq_fields.bits.pic_order_cnt_type,
            decode->picture.num_ref_frames,
            decode->picture.picture_width_in_mbs_minus1 + 1,
            decode->picture.picture_height_in_mbs_minus1 + 1,
            decode->picture.pic_fields.bits.entropy_coding_mode_flag);
      DebugBytes("access-unit", bitstream);
    }
  }

  const HANDLE retirement_device = decode->device;
  const uint64_t retirement_generation = decode->generation;
  const auto retirement_context = [&] {
    // No reopen/drain callback may intervene between candidate capture and
    // the ordinary binding commit. Other paths retain eager ownership.
    return IsH264Profile(profile) && !decode->decode_processing && !decode->is_70012 &&
        decode->device != nullptr && decode->device == retirement_device &&
        decode->generation == retirement_generation &&
        decode->decoder_open && decode->decoder_started &&
        !decode->hardware_close_pending && !decode->closing && !decode->retired &&
        !decode->destroy_prepared && !decode->live_h264_fast_path &&
        !decode->experimental_ready_remap &&
        decode->picture.seq_fields.bits.frame_mbs_only_flag &&
        !decode->picture.pic_fields.bits.field_pic_flag &&
        !decode->h264_first_field.valid;
  };
  std::shared_ptr<Surface> retirement_candidate;
  uint64_t retirement_timestamp = 0;
  uint64_t retirement_identity = 0;
  if (retirement_context() &&
      decode->transaction_target.lock() == target_surface &&
      decode->target_transaction == target_surface->picture_transaction &&
      !target_surface->ready && !target_surface->failed &&
      !target_surface->image_write_locked &&
      !target_surface->experimental_read_export_armed &&
      !target_surface->trace_read_export_armed) {
    auto previous = target_surface->decode_picture.lock();
    if (UnobservedPendingCpuBacking(*decode, previous, target_id,
                                    target_surface->width, target_surface->height) &&
        target_surface->expected_timestamp == previous->expected_timestamp &&
        target_surface->decode_identity == previous->decode_identity &&
        decode->target_identity == previous->decode_identity) {
      retirement_timestamp = previous->expected_timestamp;
      retirement_identity = previous->decode_identity;
      retirement_candidate = std::move(previous);
    }
  }

  std::shared_ptr<Surface> decoded_frame;
  const bool direct_picture = !decode->decode_processing && decode->live_h264_fast_path &&
                              IsH264Profile(profile) &&
                              !decode->experimental_ready_remap &&
                              target_surface->direct_decode_eligible;
  try {
    decoded_frame = std::make_shared<Surface>();
    decoded_frame->id = target_id;
    if (direct_picture) {
      decoded_frame->width = target_surface->width;
      decoded_frame->height = target_surface->height;
      decoded_frame->direct_picture = true;
      decoded_frame->decode_target = target_surface;
    } else {
      if (!decode->private_backing_pool)
        decode->private_backing_pool =
            std::make_shared<CrystalHDPrivateBackingPool>();
      if (!decoded_frame->AllocateCpuNv12(target_surface->width,
                                         target_surface->height,
                                         decode->private_backing_pool))
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
  } catch (const std::bad_alloc &) {
    return VA_STATUS_ERROR_ALLOCATION_FAILED;
  }
  decoded_frame->h264_crop_unit_y = h264_crop_unit_y;
  if (decode->IsMpeg4() &&
      (decode->Mpeg4CachedPictures() >= kMpeg4ReplayPictures ||
       decode->Mpeg4CachedBytes() > kMpeg4ReplayCacheBytes ||
       bitstream.size() >
           kMpeg4ReplayCacheBytes - decode->Mpeg4CachedBytes()))
    return VA_STATUS_ERROR_HW_BUSY;
  if (!decode->decoder_started || decode->hardware_close_pending) {
    const H264ProcessingGeometry previous_geometry = decode->processing_geometry;
    if (decode->decode_processing)
      decode->processing_geometry = decode->processing_request;
    VAStatus status = OpenDecoderForSubmit(
        decode, target_surface.get(), stream_width, stream_height,
        decode->IsVc1() ? &next_wmv3_metadata : nullptr);
    if (status != VA_STATUS_SUCCESS) {
      decode->processing_geometry = previous_geometry;
      return status;
    }
  }
  if (decode->IsMpeg2()) {
    const auto &pending = decode->mpeg2_picture;
    const auto kind = pending.picture.picture_coding_type == 1
        ? CrystalHDMpeg2Replay::Kind::I : pending.picture.picture_coding_type == 2
        ? CrystalHDMpeg2Replay::Kind::P : CrystalHDMpeg2Replay::Kind::B;
    if (!decode->mpeg2_replay.Append(timestamp, kind, pending.forward,
                                   pending.backward, std::move(bitstream)))
      return FailDecode(driver, decode, decode->mpeg2_replay.failure());
    decode->mpeg2_matrices = next_matrices;
    decode->stream_width = stream_width;
    decode->stream_height = stream_height;
  } else if (decode->IsMpeg4()) {
    const auto &pending = decode->mpeg4_picture;
    const unsigned type = pending.picture.vop_fields.bits.vop_coding_type;
    const auto kind = type == 0 ? CrystalHDMpeg4Replay::Kind::I
                               : type == 1 ? CrystalHDMpeg4Replay::Kind::P
                                           : CrystalHDMpeg4Replay::Kind::B;
    if (mpeg4_discontinuity) {
      CrystalHDMpeg4Replay next_epoch;
      if (!next_epoch.Append(timestamp, kind, pending.forward,
                            pending.backward, std::move(bitstream)))
        return FailDecode(driver, decode, next_epoch.failure());
      Debug("queue MPEG-4 timing epoch profile=%d TRD=%d depth=%zu",
            profile, pending.picture.TRD,
            decode->mpeg4_draining_epochs.size() + 1);
      decode->mpeg4_draining_epochs.push_back(
          std::move(decode->mpeg4_replay));
      decode->mpeg4_replay = std::move(next_epoch);
    } else if (!decode->mpeg4_replay.Append(
                   timestamp, kind, pending.forward, pending.backward,
                   std::move(bitstream))) {
      return FailDecode(driver, decode, decode->mpeg4_replay.failure());
    }
    decode->mpeg4_timing = next_mpeg4_timing;
    decode->stream_width = stream_width;
    decode->stream_height = stream_height;
  } else if (decode->IsVc1()) {
    const auto &pending = decode->vc1_picture;
    static constexpr CrystalHDVc1Replay::Kind kinds[] = {
        CrystalHDVc1Replay::Kind::I, CrystalHDVc1Replay::Kind::P,
        CrystalHDVc1Replay::Kind::B, CrystalHDVc1Replay::Kind::BI,
        CrystalHDVc1Replay::Kind::SkippedP};
    if (!decode->vc1_replay.Append(timestamp, kinds[pending.picture.picture_fields.bits.picture_type],
                                  pending.forward, pending.backward, std::move(bitstream)))
      return FailDecode(driver, decode, decode->vc1_replay.failure());
    decode->vc1_configuration = std::move(next_vc1_configuration);
    decode->wmv3_metadata = next_wmv3_metadata;
    decode->stream_width = stream_width;
    decode->stream_height = stream_height;
  } else if (!decode->replay.Append(timestamp, idr, std::move(bitstream),
                                     ShouldAutoPrefeedH264(*decode))) {
    return FailDecode(driver, decode, decode->replay.failure());
  }
  if (IsH264Profile(profile) && decode->live_h264_fast_path &&
      decode->live_h264_gap_tolerant) {
    // Append already applied the replay cache/picture admission bounds.
    // One syntax record per not-yet-InputSent AU shares that bound.
    if (decode->auxiliary.syntax.size() >= decode->replay.cached_pictures())
      return FailDecode(driver, decode, "syntax snapshot/replay ownership mismatch");
    try {
      H264Auxiliary::Syntax syntax;
      syntax.picture = decode->picture;
      syntax.generation = decode->generation;
      syntax.device = decode->device;
      if (!decode->auxiliary.syntax.emplace(timestamp, syntax).second)
        return FailDecode(driver, decode, "H.264 syntax snapshot duplicate");
    } catch (const std::bad_alloc &) {
      return FailDecode(driver, decode, "H.264 syntax snapshot allocation");
    }
  }
  decode->next_timestamp += kTimestampStep;
  decoded_frame->rt_format = VA_RT_FORMAT_YUV420;
  decoded_frame->expected_timestamp = timestamp;
  auto previous_timestamp =
      decode->surface_timestamps.find(target_surface.get());
  if (previous_timestamp != decode->surface_timestamps.end()) {
    auto previous = decode->decoded_frames.find(previous_timestamp->second);
    if (previous != decode->decoded_frames.end() &&
        (previous->second->ready || previous->second->failed))
      decode->decoded_frames.erase(previous);
  }
  // Completed uncaptured records expired above, requiring no extra copy.
  // Pending output and records retained by another context need their own
  // storage before this backing changes identity, even for equal timestamps.
  if (!PromoteDecodedPicture(target_surface->decode_picture.lock()))
    return FailDecode(driver, decode, "could not preserve reused decode picture");
  EndTraceBinding(driver, target_surface.get());
  const uint64_t submission_ordinal = decode->next_submission_ordinal++;
  decoded_frame->decode_identity = NextDecodeIdentity(driver);
  decoded_frame->trace_decode_identity = decoded_frame->decode_identity;
  decoded_frame->trace_context = TraceId(decode->id, VA_INVALID_ID);
  decoded_frame->trace_generation = decode->generation;
  decoded_frame->trace_submission_ordinal = submission_ordinal;
  target_surface->decode_identity = decoded_frame->decode_identity;
  target_surface->completed_decode_picture = {};
  target_surface->trace_decode_identity = decoded_frame->decode_identity;
  target_surface->trace_context = decoded_frame->trace_context;
  target_surface->trace_generation = decoded_frame->trace_generation;
  target_surface->trace_submission_ordinal = submission_ordinal;
  target_surface->h264_crop_unit_y = h264_crop_unit_y;
  if (direct_picture)
    decoded_frame->direct_backing = target_surface;
  target_surface->decode_picture = decoded_frame;
  decode->decoded_frames[timestamp] = decoded_frame;
  decode->surface_timestamps[target_surface.get()] = timestamp;
  decode->pending[timestamp] = target_id;
  target_surface->ready = false;
  target_surface->failed = false;
  target_surface->expected_timestamp = timestamp;
  target_surface->frame_timestamp = 0;
  if (decode->decode_processing) {
    // The original reconstructed frame remains card-owned. Its VA handle
    // tracks this exact decode, while ONLY the additional output has pixels.
    // Holding the small private picture makes original reuse independent of
    // additional-output reuse, without a second full-resolution readback.
    original_surface->opaque_reference = true;
    original_surface->opaque_context = decode->id;
    original_surface->opaque_picture = decoded_frame;
    original_surface->decode_picture = decoded_frame;
    original_surface->decode_identity = decoded_frame->decode_identity;
    original_surface->trace_decode_identity = decoded_frame->decode_identity;
    original_surface->trace_context = decoded_frame->trace_context;
    original_surface->trace_generation = decoded_frame->trace_generation;
    original_surface->trace_submission_ordinal = submission_ordinal;
    original_surface->expected_timestamp = timestamp;
    original_surface->frame_timestamp = 0;
    original_surface->ready = false;
    original_surface->failed = false;
    original_surface->completed_decode_picture = {};
    target_surface->picture_transaction = NextDecodeIdentity(driver);
  }
  // The experimental delay clock starts when the complete VA picture is
  // accepted, not at hardware READY or at an inferred client PTS.
  target_surface->experimental_submitted_at_ns =
      decode->experimental_ready_remap ? MonotonicNowNs() : 0;

  // New allocation, replay and ALL binding/map changes succeeded first. The
  // superseded map-only CPU pixels can no longer be captured through this
  // public target; ProcessDecodedOutput already discards that exact old case.
  // Keep its pending token/geometry until real hardware output retires it.
  if (retirement_context() && retirement_candidate &&
      retirement_candidate->expected_timestamp == retirement_timestamp &&
      retirement_candidate->decode_identity == retirement_identity &&
      target_surface->decode_picture.lock() == decoded_frame &&
      target_surface->expected_timestamp == timestamp &&
      target_surface->decode_identity == decoded_frame->decode_identity &&
      UnobservedPendingCpuBacking(*decode, retirement_candidate, target_id,
                                  target_surface->width, target_surface->height)) {
    std::vector<uint8_t> unused;
    unused.swap(retirement_candidate->storage);
    retirement_candidate->planes[0] = retirement_candidate->planes[1] = nullptr;
    decode->private_backing_pool->Recycle(std::move(unused));
  }
  // A surviving local reference would defeat the output path's map-only
  // check. Release it before ReceiveAvailable can process any old output.
  retirement_candidate.reset();

  if (driver->trace.enabled())
    driver->trace.Emit(crystalhd_vaapi_trace::Event::SubmitBind,
                       TraceFields(decode, target_surface.get(), timestamp));

  decode->sent_parameter_sets = true;
  if (idr)
    decode->h264_waiting_for_idr = false;
  VAStatus status = VA_STATUS_SUCCESS;
  if (decode->IsMpeg4() && !decode->mpeg4_draining_epochs.empty()) {
    // Establish the old epoch's EOS fence before consuming a marker that may
    // already be ready. Each operation is bounded and nonblocking here.
    status = PumpDecodeInput(driver, decode);
    if (status == VA_STATUS_SUCCESS &&
        !decode->mpeg4_draining_epochs.empty())
      status = SealDecodeBatch(driver, decode);
    if (status == VA_STATUS_SUCCESS)
      status = ReceiveAvailable(driver, decode);
    if (status == VA_STATUS_SUCCESS)
      status = PumpDecodeInput(driver, decode);
    if (status == VA_STATUS_SUCCESS &&
        !decode->mpeg4_draining_epochs.empty())
      status = SealDecodeBatch(driver, decode);
  } else {
    status = ReceiveAvailable(driver, decode);
    if (status == VA_STATUS_SUCCESS)
      status = PumpDecodeInput(driver, decode);
  }
  return status;
}

static VAStatus DecodeSurfaceState(Surface *surface) {
  if (surface->destroyed)
    return VA_STATUS_ERROR_INVALID_SURFACE;
  if (surface->failed)
    return VA_STATUS_ERROR_DECODING_ERROR;
  if (!surface->ready)
    return VA_STATUS_ERROR_HW_BUSY;
  if (surface->expected_timestamp != 0 &&
      surface->frame_timestamp != surface->expected_timestamp) {
    Debug("frame identity mismatch expected=%llu produced=%llu",
          static_cast<unsigned long long>(surface->expected_timestamp),
          static_cast<unsigned long long>(surface->frame_timestamp));
    surface->failed = true;
    RetireFailedExperimentalState(surface);
    return VA_STATUS_ERROR_DECODING_ERROR;
  }
  return VA_STATUS_SUCCESS;
}

// Cancellation and surface identity take precedence even when pixels are
// ready. Decoder retirement alone cannot revoke an exact completed public
// picture that another thread finished while this sync released the mutex.
static VAStatus DecodeWaitState(const DecodeContext &decode, Surface *surface,
                                uint64_t generation, uint64_t timestamp,
                                const std::function<bool()> &canceled) {
  if (canceled && canceled())
    return VA_STATUS_ERROR_OPERATION_FAILED;
  if (surface->destroyed || surface->expected_timestamp != timestamp)
    return VA_STATUS_ERROR_INVALID_SURFACE;
  const VAStatus state = DecodeSurfaceState(surface);
  if (state != VA_STATUS_ERROR_HW_BUSY)
    return state;
  if (decode.retired || decode.generation != generation)
    return VA_STATUS_ERROR_INVALID_CONTEXT;
  return VA_STATUS_ERROR_HW_BUSY;
}

// The compact private result is valid ONLY for this operation's selected first
// temporal parity. It never enters a public surface/map or changes readiness.
// The caller holds Driver::mutex, including the successful RX-release boundary.
static VAStatus PrepareFirstFieldBob(const PendingVpp &operation,
                                    std::shared_ptr<Surface> *view) {
  if (view == nullptr)
    return VA_STATUS_ERROR_INVALID_PARAMETER;
  const auto &source = operation.source;
  const auto &decoder = operation.decoder;
  if (!operation.deinterlace.bob || !source || !decoder || source->ready ||
      source->failed || source->destroyed || source->image_write_locked ||
      source->fourcc != VA_FOURCC_NV12 || decoder->is_70012 ||
      !IsH264Profile(decoder->profile) || decoder->closing || decoder->retired ||
      decoder->hardware_close_pending || decoder->ReplayFailed() ||
      decoder->generation != operation.decoder_generation ||
      source->expected_timestamp != operation.source_timestamp)
    return VA_STATUS_SUCCESS;
  const H264FirstField &first = decoder->h264_first_field;
  const bool bottom = operation.deinterlace.field ==
                      crystalhd_vpp_bob::FieldParity::kBottom;
  const auto picture = decoder->decoded_frames.find(operation.source_timestamp);
  if (!first.valid || first.replay_duplicate || first.is_70012 ||
      first.bottom != bottom || first.bottom_first != bottom ||
      first.timestamp != operation.source_timestamp || first.picture != source ||
      first.decode_identity == 0 || first.decode_identity != source->decode_identity ||
      first.generation != operation.decoder_generation ||
      source->trace_generation != operation.decoder_generation ||
      picture == decoder->decoded_frames.end() || picture->second != source ||
      !decoder->replay.IsOutstanding(operation.source_timestamp))
    return VA_STATUS_SUCCESS;
  if (first.source_pitch == 0 || first.height == 0 ||
      first.height / 2U > first.pixels.size() / first.source_pitch)
    return VA_STATUS_ERROR_DECODING_ERROR;
  try {
    auto prepared = std::make_shared<Surface>();
    if (!prepared->AllocateCpuNv12(source->width,
                                   source->height / 2U + source->height % 2U))
      return VA_STATUS_ERROR_ALLOCATION_FAILED;
    prepared->expected_timestamp = operation.source_timestamp;
    const uint8_t *fields[2] = {};
    fields[bottom ? 1 : 0] = first.pixels.data();
    if (!CopyH264InterlacedToSurface(prepared.get(), fields, first.width,
                                    first.height, first.source_pitch,
                                    operation.source_timestamp,
                                    bottom ? 2U : 1U, true))
      return VA_STATUS_ERROR_OPERATION_FAILED;
    *view = std::move(prepared);
  } catch (const std::bad_alloc &) {
    return VA_STATUS_ERROR_ALLOCATION_FAILED;
  }
  return VA_STATUS_SUCCESS;
}

static VAStatus TryH264Auxiliary(Driver *driver, DecodeContext *decode,
                               const std::shared_ptr<Surface> &surface,
                               const VppDeinterlace *demand, uint64_t idle) {
  auto &aux = decode->auxiliary;
  const auto &first = decode->h264_first_field;
  if (!demand || !demand->bob || idle < kDecodeBatchGraceNs || aux.pending ||
      !decode->live_h264_fast_path || !decode->live_h264_gap_tolerant ||
      !aux.candidate_timestamp || !first.valid || first.replay_duplicate ||
      first.bottom == (demand->field == crystalhd_vpp_bob::FieldParity::kBottom) ||
      first.timestamp != surface->expected_timestamp ||
      aux.candidate_timestamp < first.timestamp ||
      aux.candidate.generation != decode->generation ||
      aux.candidate.device != decode->device ||
      first.generation != decode->generation || !first.picture ||
      decode->is_70012 || !IsH264Profile(decode->profile) ||
      decode->experimental_ready_remap || decode->closing || decode->retired ||
      decode->destroy_prepared || decode->hardware_close_pending ||
      !decode->decoder_open || !decode->decoder_started || !decode->device ||
      decode->ReplayFailed() || decode->ReplaySealed() ||
      decode->replay.NeedsRestart() || !decode->replay.CanSeal() ||
      decode->replay.NextInput() != nullptr || decode->partial_input_timestamp ||
      decode->partial_input_offset || driver->stopping)
    return VA_STATUS_SUCCESS;
  const auto picture = decode->decoded_frames.find(first.timestamp);
  const auto pending = decode->pending.find(first.timestamp);
  if (picture == decode->decoded_frames.end() || pending == decode->pending.end() ||
      picture->second != first.picture || first.decode_identity == 0 ||
      first.decode_identity != picture->second->decode_identity)
    return FailDecode(driver, decode, "H.264 auxiliary lost original ownership");
  const uint64_t token = decode->next_timestamp;
  if (!token || token > std::numeric_limits<uint64_t>::max() - kTimestampStep ||
      decode->pending.count(token) || decode->decoded_frames.count(token) ||
      (decode->prefed_timestamp && decode->prefed_timestamp != token) ||
      (decode->pending_prefeed_timestamp &&
       (decode->pending_prefeed_timestamp != token ||
        decode->pending_prefeed_generation != decode->generation)))
    return FailDecode(driver, decode, "H.264 auxiliary timestamp reservation");
  std::vector<uint8_t> payload;
  try {
    if (!BuildH264NeutralIntra(aux.candidate.picture, &payload))
      return VA_STATUS_SUCCESS;
  } catch (const std::bad_alloc &) {
    return FailDecode(driver, decode, "neutral picture allocation");
  }
  const size_t bytes = payload.size();
  const size_t reservation = bytes + (bytes / 60000 + 1) * 32 + 1024;
  if (DtsTxFreeSize(decode->device) < reservation) {
    decode->input_backpressured = true;
    return VA_STATUS_SUCCESS;
  }
  const bool consume = decode->prefed_timestamp == token;
  Debug("H.264 auxiliary qualified source=%llu token=%llu bytes=%zu generation=%llu consume_prefeed=%d idle_ns=%llu",
               static_cast<unsigned long long>(first.timestamp),
               static_cast<unsigned long long>(token), bytes,
               static_cast<unsigned long long>(decode->generation), consume,
               static_cast<unsigned long long>(idle));
  // One whole-call reservation; any non-success is terminal, never retried.
  const BC_STATUS status = DtsProcInput(decode->device, payload.data(),
      static_cast<uint32_t>(bytes), consume ? 0 : token, FALSE);
  if (status != BC_STS_SUCCESS)
    return FailDecode(driver, decode, "H.264 auxiliary input failed (no retry)");
  aux.timestamp = token; aux.ever = aux.pending = aux.needs_eos = true;
  aux.generation = decode->generation;
  aux.device = decode->device;
  aux.eos_only = aux.ended = false;
  aux.candidate_timestamp = 0;
  decode->next_timestamp += kTimestampStep;
  decode->CancelPendingPrefeed();
  if (consume) decode->prefed_timestamp = 0;
  decode->RecordTransportProgress();
  Debug("H.264 auxiliary input_sent token=%llu transport_timestamp=%llu bytes=%zu public_picture=0 public_ordinal_advanced=0",
               static_cast<unsigned long long>(token),
               static_cast<unsigned long long>(consume ? 0 : token), bytes);
  // Delimit the synthetic slice with the ordinary next-AU prefix. The next
  // original AU consumes this prefix without acquiring an auxiliary identity.
  decode->pending_prefeed_timestamp = decode->next_timestamp;
  decode->pending_prefeed_generation = decode->generation;
  bool postprefix_attempted = false;
  const VAStatus postprefix_status =
      RetryPendingH264Prefeed(driver, decode, &postprefix_attempted);
  Debug("H.264 auxiliary postprefix token=%llu attempted=%d accepted=%d pending=%d status=%d",
               static_cast<unsigned long long>(decode->next_timestamp),
               postprefix_attempted, decode->prefed_timestamp == decode->next_timestamp,
               decode->pending_prefeed_timestamp != 0, postprefix_status);
  if (postprefix_status != VA_STATUS_SUCCESS) return postprefix_status;
  driver->condition.notify_all();
  return VA_STATUS_SUCCESS;
}

static VAStatus SyncDecodeSurface(
    Driver *driver, const std::shared_ptr<Surface> &surface,
    std::unique_lock<std::mutex> *driver_lock, uint64_t timeout_ns,
    const std::function<bool()> &canceled = {},
    const std::function<VAStatus(bool *)> &private_vpp_ready = {},
    const VppDeinterlace *private_demand = nullptr) {
  if (driver_lock == nullptr || !driver_lock->owns_lock())
    return VA_STATUS_ERROR_INVALID_PARAMETER;
  if (canceled && canceled())
    return VA_STATUS_ERROR_OPERATION_FAILED;
  const VAStatus initial_state = DecodeSurfaceState(surface.get());
  if (initial_state != VA_STATUS_ERROR_HW_BUSY)
    return initial_state;
  if (private_vpp_ready) {
    bool ready = false;
    const VAStatus status = private_vpp_ready(&ready);
    if (status != VA_STATUS_SUCCESS || ready)
      return status;
  }
  std::shared_ptr<DecodeContext> decode;
  for (auto &entry : driver->contexts) {
    if (!entry.second->decoder_started ||
        entry.second->hardware_close_pending)
      continue;
    for (const auto &pending : entry.second->pending) {
      auto candidate = driver->surfaces.find(pending.second);
      if (candidate != driver->surfaces.end() &&
          candidate->second == surface &&
          pending.first == surface->expected_timestamp) {
        const auto picture = entry.second->decoded_frames.find(pending.first);
        if ((surface->decode_identity != 0 &&
             (picture == entry.second->decoded_frames.end() ||
              surface->decode_identity != picture->second->decode_identity)) ||
            (!surface->decode_picture.expired() &&
             (picture == entry.second->decoded_frames.end() ||
              surface->decode_picture.lock() != picture->second)))
          continue;
        decode = entry.second;
        break;
      }
    }
    if (!decode) {
      for (const auto &decoded : entry.second->decoded_frames) {
        if (decoded.second == surface) {
          decode = entry.second;
          break;
        }
      }
    }
    if (decode)
      break;
  }
  if (!decode)
    return VA_STATUS_ERROR_INVALID_CONTEXT;
  // A zero-timeout query must not turn into output polling, frame conversion
  // or input submission. The caller asked only whether this exact surface is
  // already complete in the surface state published so far.
  if (timeout_ns == 0)
    return VA_STATUS_ERROR_TIMEDOUT;
  const uint64_t generation = decode->generation;
  const uint64_t expected_timestamp = surface->expected_timestamp;
  const uint64_t expected_context = surface->trace_context;
  const uint64_t expected_trace_generation = surface->trace_generation;
  const uint64_t expected_decode_identity = surface->trace_decode_identity;
  const uint64_t expected_submission_ordinal =
      surface->trace_submission_ordinal;
  const auto same_wait_binding = [&]() {
    return !surface->destroyed &&
        surface->expected_timestamp == expected_timestamp &&
        surface->trace_context == expected_context &&
        surface->trace_generation == expected_trace_generation &&
        surface->trace_decode_identity == expected_decode_identity &&
        surface->trace_submission_ordinal == expected_submission_ordinal;
  };
  const auto presentation_ready = [&]() {
    return same_wait_binding() &&
        ExperimentalPresentationReadable(driver, surface.get());
  };
  const auto try_presentation = [&]() -> VAStatus {
    // This call may have released Driver::mutex while another API user
    // destroyed or rebound the public surface.  A timestamp alone is not an
    // ownership identity: another context can legitimately reuse the same
    // synthetic token.  Never let an obsolete waiter mutate the new binding.
    if (!same_wait_binding())
      return VA_STATUS_ERROR_INVALID_SURFACE;
    if (decode->retired || decode->generation != generation)
      return VA_STATUS_ERROR_INVALID_CONTEXT;
    if (!decode->experimental_ready_remap || presentation_ready())
      return VA_STATUS_SUCCESS;
    bool committed = false;
    const VAStatus status =
        TryExperimentalPresentation(driver, surface, &committed);
    if (status != VA_STATUS_SUCCESS)
      return status;
    return committed && !presentation_ready()
               ? VA_STATUS_ERROR_OPERATION_FAILED : VA_STATUS_SUCCESS;
  };

  Debug("sync surface ready=%d failed=%d timeout=%llu", surface->ready,
        surface->failed, static_cast<unsigned long long>(timeout_ns));

  const auto start = std::chrono::steady_clock::now();
  auto inactivity_start = start;
  uint64_t bounded_inactivity_base = decode->sync_inactivity_ns;
  uint64_t progress = decode->transport_progress;
  while (!surface->ready && !surface->failed && !presentation_ready()) {
    const VAStatus state = DecodeWaitState(*decode, surface.get(), generation,
                                           expected_timestamp, canceled);
    if (state != VA_STATUS_ERROR_HW_BUSY &&
        (state != VA_STATUS_SUCCESS || decode->retired ||
         decode->generation != generation))
      return state;
    if (driver->stopping)
      return VA_STATUS_ERROR_OPERATION_FAILED;
    VAStatus status = ReceiveAvailable(driver, decode.get());
    if (status == VA_STATUS_SUCCESS &&
        (surface->ready || presentation_ready()))
      break;
    // Bob alone may consume a released first field. This is not an EOS or a
    // full-picture completion, and ordinary SyncSurface has no such callback.
    if (status == VA_STATUS_SUCCESS && private_vpp_ready) {
      if (canceled && canceled())
        return VA_STATUS_ERROR_OPERATION_FAILED;
      bool ready = false;
      status = private_vpp_ready(&ready);
      if (status != VA_STATUS_SUCCESS || ready)
        return status;
    }
    // The immediately preceding picture commonly becomes READY while this
    // call is already waiting for the exact requested picture.  Reconsider
    // the bounded candidate after harvesting output; checking only once in
    // SyncSurface2 misses the normal stock-Moonlight timing window.
    if (status == VA_STATUS_SUCCESS)
      status = try_presentation();
    if (status == VA_STATUS_SUCCESS && presentation_ready())
      break;
    if (status == VA_STATUS_SUCCESS)
      status = PumpDecodeInput(driver, decode.get());
    if (status != VA_STATUS_SUCCESS)
      return status;
    if (surface->ready || surface->failed || presentation_ready())
      break;
    const auto now = std::chrono::steady_clock::now();
    if (decode->transport_progress != progress) {
      progress = decode->transport_progress;
      bounded_inactivity_base = 0;
      inactivity_start = now;
      decode->sync_inactivity_ns = 0;
    }
    const uint64_t local_idle =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            now - inactivity_start).count();
    // A fully submitted large AU can take longer than 100 ms to complete.
    // Reuse the bounded history-loss grace; count actual sync waits, not player
    // pauses. Queued/partial AUs and completed cache alone do not qualify.
    const bool bounded_h264_inactivity =
        !decode->is_70012 && IsH264Profile(decode->profile) &&
        ((decode->live_h264 && !decode->live_h264_gap_tolerant &&
          decode->replay.live_stream_started() &&
          !decode->replay.replayable()) ||
         decode->replay.HasOutstandingInputLargerThan(
             crystalhd_access_unit_limits::kDtsInputChunkBytes));
    uint64_t idle = local_idle;
    if (bounded_h264_inactivity) {
      const uint64_t accumulated = bounded_inactivity_base + local_idle;
      decode->sync_inactivity_ns =
          std::max(decode->sync_inactivity_ns, accumulated);
      idle = decode->sync_inactivity_ns;
    } else {
      // All other input retains the historical per-call inactivity rule.
      bounded_inactivity_base = 0;
      decode->sync_inactivity_ns = 0;
    }
    const uint64_t elapsed =
        std::chrono::duration_cast<std::chrono::nanoseconds>(now - start).count();
    const VAStatus wait_status = DecodeWaitStatus(timeout_ns, elapsed);
    if (wait_status == VA_STATUS_ERROR_DECODING_ERROR) {
      Debug("firmware did not complete timestamp=%llu within 10 seconds",
            static_cast<unsigned long long>(surface->expected_timestamp));
      return FailDecode(driver, decode.get(), "sealed-batch sync timed out");
    }
    if (wait_status != VA_STATUS_SUCCESS)
      return wait_status;
    if (private_vpp_ready && private_demand) {
      const uint64_t before = decode->transport_progress;
      status = TryH264Auxiliary(driver, decode.get(), surface, private_demand, idle);
      if (status != VA_STATUS_SUCCESS) return status;
      // A newly accepted real auxiliary input is progress, not an EOF trigger.
      if (before != decode->transport_progress) {
        progress = decode->transport_progress;
        inactivity_start = now;
        bounded_inactivity_base = 0;
        idle = 0;
      }
    }
    // Seal only after transport inactivity, not while a live stream or retained
    // prefix replay still makes real input/output progress. Keep the absolute
    // sync deadline above: unrelated progress cannot hide a missing picture.
    // A second accepted live picture distinguishes streaming from a one-frame
    // decoder probe. An input gap is not EOF, even while history is replayable:
    // sealing would needlessly reopen the device when input resumes. Explicit
    // context drain still seals the final accepted pictures.
    const bool retain_live_references =
        decode->live_h264_gap_tolerant &&
        decode->replay.live_stream_started();
    if (!surface->ready && !decode->ReplaySealed() &&
        !retain_live_references &&
        DecodeBatchGraceExpired(idle, bounded_h264_inactivity)) {
      Debug("sync batch inactivity: elapsed=%llu idle=%llu progress=%llu timestamp=%llu",
            static_cast<unsigned long long>(elapsed),
            static_cast<unsigned long long>(idle),
            static_cast<unsigned long long>(decode->transport_progress),
            static_cast<unsigned long long>(expected_timestamp));
      status = SealDecodeBatch(driver, decode.get());
      if (status != VA_STATUS_SUCCESS)
        return status;
    }
    // Let submission threads queue subsequent input while this finite batch
    // drains, and let destruction cancel this wait without touching new state.
    // With negotiated RX polling, the sole hardware waiter publishes output
    // through this condition; retain the old 1 ms sleep for older stacks.
    if (decode->output_poll && driver->decode_control_fd >= 0) {
      driver->condition.wait_for(*driver_lock, std::chrono::milliseconds(10));
    } else {
      driver_lock->unlock();
      usleep(1000);
      driver_lock->lock();
    }
    if (presentation_ready())
      return VA_STATUS_SUCCESS;
    const VAStatus resumed = DecodeWaitState(*decode, surface.get(), generation,
                                             expected_timestamp, canceled);
    if (resumed != VA_STATUS_ERROR_HW_BUSY &&
        (resumed != VA_STATUS_SUCCESS || decode->retired ||
         decode->generation != generation))
      return resumed;
    if (resumed == VA_STATUS_ERROR_HW_BUSY) {
      const VAStatus presentation = try_presentation();
      if (presentation != VA_STATUS_SUCCESS)
        return presentation;
      if (presentation_ready())
        return VA_STATUS_SUCCESS;
    }
  }
  return presentation_ready() ? VA_STATUS_SUCCESS
                              : DecodeSurfaceState(surface.get());
}

static bool PendingVppCanceled(const PendingVpp &operation) {
  return operation.source->destroyed || operation.target->destroyed ||
         operation.target_owner->destroyed ||
         operation.target_owner->latest_vpp_sequence != operation.sequence ||
         (operation.decoder &&
          (operation.decoder->retired ||
           operation.decoder->generation != operation.decoder_generation));
}

// Release bookkeeping for one queued conversion. The caller holds the driver
// mutex. Cancellation must neither substitute pixels from another picture nor
// signal a successful fence for uncommitted output. An aborted timeline reports
// -ENOENT; VA surface status retains the failure independently of fence waits.
static void ReleasePendingVpp(Driver *driver, uint64_t sequence,
                              VAStatus status, bool committed) {
  auto found = std::find_if(
      driver->pending_vpp.begin(), driver->pending_vpp.end(),
      [&](const PendingVpp &pending) { return pending.sequence == sequence; });
  if (found == driver->pending_vpp.end())
    return;
  const PendingVpp operation = *found;
  // Flush CPU caches before signaling the fence imported into Chromium's
  // DMA-BUF. Every implicit GPU reader remains blocked until this point.
  const bool completed = committed && status == VA_STATUS_SUCCESS;
  const bool released = completed
      ? EndFencedWrite(operation.target.get(), operation.write_timeline)
      : AbortFencedWrite(operation.target.get(), operation.write_timeline);
  if (!released) {
    status = VA_STATUS_ERROR_OPERATION_FAILED;
    committed = false;
  }
  if (operation.source->vpp_readers != 0)
    --operation.source->vpp_readers;
  // Keep the current picture available for another VPP of the same still-live
  // decode surface. SubmitPicture reclaims it when that surface is reused;
  // queued/captured VPP operations retain their own immutable shared_ptr.
  if (operation.target_owner->vpp_writers != 0)
    --operation.target_owner->vpp_writers;
  const bool successful_commit =
      committed && status == VA_STATUS_SUCCESS && released;
  if (successful_commit) {
    InheritTraceIdentity(operation.target.get(), *operation.source);
    if (operation.target_owner != operation.target)
      InheritTraceIdentity(operation.target_owner.get(), *operation.source);
  }
  if (operation.target_owner->latest_vpp_sequence == operation.sequence &&
      operation.target_owner->vpp_writers == 0) {
    SetSurfaceState(driver, operation.target.get(),
                    successful_commit, !successful_commit);
  }
  Debug("%s VPP sequence=%llu source_timestamp=%llu target=%p status=%d "
        "expected_timestamp=%llu decoder_context=%u captured_generation=%llu "
        "current_generation=%llu source_ready=%d source_failed=%d decoder_retired=%d "
        "source_destroyed=%d target_destroyed=%d target_owner_destroyed=%d target_superseded=%d "
        "completed_source_timestamp=%llu",
        committed ? "committed" : "discarded",
        static_cast<unsigned long long>(operation.sequence),
        static_cast<unsigned long long>(successful_commit
                                            ? operation.source_timestamp
                                            : operation.source->frame_timestamp),
        static_cast<void *>(operation.target.get()), status,
        static_cast<unsigned long long>(operation.source_timestamp),
        operation.decoder ? operation.decoder->id : VA_INVALID_ID,
        static_cast<unsigned long long>(operation.decoder_generation),
        static_cast<unsigned long long>(operation.decoder ? operation.decoder->generation : 0),
        operation.source->ready, operation.source->failed,
        operation.decoder && operation.decoder->retired,
        operation.source->destroyed, operation.target->destroyed,
        operation.target_owner->destroyed,
        operation.target_owner->latest_vpp_sequence != operation.sequence,
        static_cast<unsigned long long>(operation.source->frame_timestamp));
  crystalhd_vaapi_trace::Fields fields =
      TraceFields(nullptr, operation.target.get(),
                  successful_commit ? operation.source_timestamp
                                    : operation.source->frame_timestamp);
  fields.operation = operation.sequence;
  fields.outcome = status;
  driver->trace.Emit(successful_commit
                         ? crystalhd_vaapi_trace::Event::VppCommit
                         : crystalhd_vaapi_trace::Event::VppCancel,
                     fields);
  driver->pending_vpp.erase(found);
  driver->condition.notify_all();
}

static bool HasAutonomousDecodeWork(
    const std::shared_ptr<DecodeContext> &decode) {
  return !decode->video_process && !decode->closing && !decode->retired &&
         decode->decoder_started && !decode->hardware_close_pending &&
         !decode->ReplayFailed() &&
         (!decode->pending.empty() || decode->ReplaySealed() ||
          decode->auxiliary.pending || decode->auxiliary.eos_only ||
          decode->pending_prefeed_timestamp != 0);
}

static bool NeedsMpeg4EosProbe(const DecodeContext &decode) {
  if (!decode.IsMpeg4())
    return false;
  const auto &replay = decode.Mpeg4TransportReplay();
  return replay.sealed() && !replay.ended() && replay.outstanding() == 0;
}

static bool NeedsDecodeControlWake(const DecodeContext &decode) {
  if (!decode.output_poll || decode.input_backpressured ||
      decode.input_budget_exhausted || NeedsMpeg4EosProbe(decode))
    return true;
  return decode.armed_output_poll.lock() != decode.output_poll;
}

static void DrainDecodeControlFd(int fd) {
  if (fd < 0)
    return;
  uint64_t value;
  ssize_t result;
  do {
    result = read(fd, &value, sizeof(value));
  } while (result < 0 && errno == EINTR);
}

// Some conforming frontends submit pictures and then wait in their presentation
// path without making another VA call that can harvest decoder output. Keep
// QuerySurfaceStatus passive and make that hardware progress independently.
// All Dts calls remain serialized with EndPicture, SyncSurface and teardown by
// the existing driver mutex. Idle-gap EOS sealing remains demand-driven.
static void RunDecodeWorker(Driver *driver) {
  std::unique_lock<std::mutex> lock(driver->mutex);
  for (;;) {
    driver->condition.wait(lock, [&] {
      if (driver->stopping)
        return true;
      return std::any_of(driver->contexts.begin(), driver->contexts.end(),
                         [](const auto &entry) {
                           return HasAutonomousDecodeWork(entry.second);
                         });
    });
    if (driver->stopping)
      return;

    // Bound each context's turn so a ready backlog cannot hold the global VA
    // mutex long enough to starve frontend or VPP calls. Visit every active
    // context once before the polling delay.
    bool input_backpressured = false;
    bool input_budget_exhausted = false;
    bool needs_mpeg4_eos_probe = false;
    std::vector<std::shared_ptr<DecodeContext>> output_poll_contexts;
    std::vector<std::shared_ptr<OutputPollLease>> output_poll_leases;
    bool needs_timer_fallback = driver->decode_control_fd < 0;
    for (const auto &entry : driver->contexts) {
      const std::shared_ptr<DecodeContext> &decode = entry.second;
      if (!HasAutonomousDecodeWork(decode))
        continue;
      VAStatus status = ReceiveAvailable(driver, decode.get(), 1);
      if (status == VA_STATUS_SUCCESS)
        status = PumpDecodeInput(driver, decode.get(), 4);
      // A queued MPEG-4 timing discontinuity is an explicit finite epoch,
      // not an idle-gap EOS inference. EndPicture attempts this seal too, but
      // TX backpressure can make it temporarily impossible; retry once the
      // worker has submitted the old epoch, including each queued successor.
      if (status == VA_STATUS_SUCCESS && decode->IsMpeg4() &&
          !decode->mpeg4_draining_epochs.empty())
        status = SealDecodeBatch(driver, decode.get());
      if (status != VA_STATUS_SUCCESS && !decode->ReplayFailed())
        FailDecode(driver, decode.get(), "autonomous decoder progress failed");
      if (!HasAutonomousDecodeWork(decode))
        continue;
      input_backpressured = input_backpressured || decode->input_backpressured;
      input_budget_exhausted =
          input_budget_exhausted || decode->input_budget_exhausted;
      needs_mpeg4_eos_probe =
          needs_mpeg4_eos_probe || NeedsMpeg4EosProbe(*decode);
      if (decode->output_poll) {
        output_poll_contexts.push_back(decode);
        output_poll_leases.push_back(decode->output_poll);
      } else {
        needs_timer_fallback = true;
      }
    }

    if (driver->stopping)
      return;
    if (input_budget_exhausted) {
      // A ready control/output descriptor can make poll return immediately,
      // and std::mutex does not promise a waiting frontend the next handoff.
      // Release the mutex for one bounded turn before continuing the backlog.
      driver->condition.wait_for(
          lock, std::chrono::milliseconds(1),
          [&] { return driver->stopping; });
      if (driver->stopping)
        return;
      if (needs_timer_fallback)
        continue;
    }
    if (needs_timer_fallback) {
      // An older kernel/library pair has no negotiated output event. Preserve
      // the established polling behavior exactly for that compatibility path.
      // This wait also releases the global VA mutex between bounded input
      // turns; a ready input backlog must not starve frontend or teardown calls.
      driver->condition.wait_for(lock, std::chrono::milliseconds(1));
      continue;
    }

    std::vector<pollfd> descriptors;
    descriptors.reserve(output_poll_leases.size() + 1);
    descriptors.push_back({driver->decode_control_fd, POLLIN, 0});
    for (const auto &lease : output_poll_leases)
      descriptors.push_back({lease->fd, POLLIN, 0});

    // A latched control event wakes new input and teardown. RX readiness is
    // level-triggered by the kernel, so output that arrives before poll() is
    // not lost. A full input turn already yielded above. TX-ring backpressure
    // needs 2 ms.
    // The MPEG-4 missing-marker fallback changes only library EOS state, so
    // retain a bounded state probe for that exact sealed/zero-output window.
    const int timeout_ms = input_budget_exhausted ? 0 :
                           input_backpressured ? 2 :
                           needs_mpeg4_eos_probe ? 5 : -1;
    for (size_t i = 0; i < output_poll_contexts.size(); ++i)
      output_poll_contexts[i]->armed_output_poll = output_poll_leases[i];
    lock.unlock();
    int result;
    do {
      result = poll(descriptors.data(), descriptors.size(), timeout_ms);
    } while (result < 0 && errno == EINTR);
    if (result > 0 && (descriptors.front().revents & POLLIN) != 0)
      DrainDecodeControlFd(driver->decode_control_fd);
    lock.lock();
    for (size_t i = 0; i < output_poll_contexts.size(); ++i) {
      if (output_poll_contexts[i]->armed_output_poll.lock() ==
          output_poll_leases[i])
        output_poll_contexts[i]->armed_output_poll.reset();
    }
    if (result < 0) {
      for (const auto &decode : output_poll_contexts) {
        if (HasAutonomousDecodeWork(decode))
          FailDecode(driver, decode.get(), "decoder output poll failed");
      }
      continue;
    }
    for (size_t i = 0; i < output_poll_contexts.size(); ++i) {
      const short terminal = POLLERR | POLLHUP | POLLNVAL;
      if ((descriptors[i + 1].revents & terminal) == 0)
        continue;
      const std::shared_ptr<DecodeContext> &decode = output_poll_contexts[i];
      if (decode->output_poll == output_poll_leases[i] &&
          HasAutonomousDecodeWork(decode))
        FailDecode(driver, decode.get(), "decoder output poll terminated");
    }
  }
}

void Driver::StartDecodeWorker() {
  std::lock_guard<std::mutex> lock(mutex);
  if (!decode_worker.joinable())
    decode_worker = std::thread(RunDecodeWorker, this);
}

static void RunVppWorker(Driver *driver) {
  std::unique_lock<std::mutex> lock(driver->mutex);
  for (;;) {
    driver->condition.wait(lock, [&] {
      if (driver->stopping)
        return true;
      return std::any_of(driver->pending_vpp.begin(),
                         driver->pending_vpp.end(),
                         [](const PendingVpp &pending) {
                           return PendingVppCanceled(pending) ||
                                  !pending.worker_active;
                         });
    });
    if (driver->stopping)
      return;

    auto selected = std::find_if(
        driver->pending_vpp.begin(), driver->pending_vpp.end(),
        [](const PendingVpp &pending) {
          return PendingVppCanceled(pending) ||
                 !pending.worker_active;
        });
    if (selected == driver->pending_vpp.end())
      continue;
    if (PendingVppCanceled(*selected)) {
      const uint64_t sequence = selected->sequence;
      ReleasePendingVpp(driver, sequence, VA_STATUS_SUCCESS, false);
      continue;
    }

    selected->worker_active = true;
    const PendingVpp operation = *selected;
    std::shared_ptr<Surface> first_field_view;
    VAStatus status = VA_STATUS_SUCCESS;
    if (operation.source->expected_timestamp !=
        operation.source_timestamp) {
      Debug("VPP source was reused expected=%llu queued=%llu",
            static_cast<unsigned long long>(
                operation.source->expected_timestamp),
            static_cast<unsigned long long>(operation.source_timestamp));
      status = VA_STATUS_ERROR_DECODING_ERROR;
    } else {
      status = SyncDecodeSurface(driver, operation.source, &lock,
                                 VA_TIMEOUT_INFINITE, [&] {
                                   return PendingVppCanceled(operation);
                                 }, [&](bool *ready) {
                                   const VAStatus prepared =
                                       PrepareFirstFieldBob(operation, &first_field_view);
                                   *ready = first_field_view != nullptr;
                                   return prepared;
                                 }, &operation.deinterlace);
    }

    auto completed = std::find_if(
        driver->pending_vpp.begin(), driver->pending_vpp.end(),
        [&](const PendingVpp &pending) {
          return pending.sequence == operation.sequence;
        });
    if (completed == driver->pending_vpp.end())
      continue;
    if (PendingVppCanceled(*completed)) {
      ReleasePendingVpp(driver, operation.sequence, VA_STATUS_SUCCESS, false);
      continue;
    }
    if (status == VA_STATUS_SUCCESS) {
      status = ProcessVpp(&driver->vpp_scaler,
                          &driver->vpp_argb_staging,
                          first_field_view ? first_field_view.get() : operation.source.get(),
                          operation.target.get(),
                          operation.source_region, operation.output_region,
                          operation.write_timeline >= 0,
                          operation.deinterlace, first_field_view != nullptr,
                          operation.color_range);
    }
    ReleasePendingVpp(driver, operation.sequence, status, true);
  }
}

static VAStatus Terminate(VADriverContextP context) {
  Driver *driver = GetDriver(context);
  if (driver == nullptr)
    return VA_STATUS_SUCCESS;
  driver->StopWorkers();

  VAStatus status = VA_STATUS_SUCCESS;
  {
    std::lock_guard<std::mutex> lock(driver->mutex);
    for (const auto &entry : driver->contexts) {
      DecodeContext *decode = entry.second.get();
      decode->closing = true;
      decode->retired = true;
      ClearTraceReadExportsForContext(
          driver, TraceId(decode->id, VA_INVALID_ID), decode->generation);
      BC_STATUS closed;
      // libva unloads a backend after this callback regardless of its return
      // value, so there is no public retry boundary. Once our workers are
      // joined, retain and retry the sole non-consuming close result until the
      // library consumes ownership, matching the GStreamer finalizer.
      do {
        closed = decode->CloseHardware(0);
        if (decode->device != nullptr)
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
      } while (decode->device != nullptr);
      if (closed != BC_STS_SUCCESS && status == VA_STATUS_SUCCESS)
        status = VA_STATUS_ERROR_OPERATION_FAILED;
      decode->Close(0);
    }
  }

  delete driver;
  context->pDriverData = nullptr;
  return status;
}

static VAStatus QueryConfigProfiles(VADriverContextP, VAProfile *profiles,
                                    int *count) {
  if (profiles == nullptr || count == nullptr)
    return VA_STATUS_ERROR_INVALID_PARAMETER;
  const VAProfile supported[] = {
      VAProfileH264ConstrainedBaseline, VAProfileH264Main, VAProfileH264High,
      VAProfileNone, VAProfileMPEG2Simple, VAProfileMPEG2Main, VAProfileVC1Simple,
      VAProfileVC1Main, VAProfileVC1Advanced, VAProfileMPEG4Simple,
      VAProfileMPEG4AdvancedSimple};
  *count = sizeof(supported) / sizeof(supported[0]);
  std::copy(std::begin(supported), std::end(supported), profiles);
  return VA_STATUS_SUCCESS;
}

static VAStatus QueryConfigEntrypoints(VADriverContextP, VAProfile profile,
                                       VAEntrypoint *entrypoints, int *count) {
  if (!IsSupportedProfile(profile)) {
    if (count != nullptr)
      *count = 0;
    return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
  }
  if (entrypoints == nullptr || count == nullptr)
    return VA_STATUS_ERROR_INVALID_PARAMETER;
  entrypoints[0] = profile == VAProfileNone ? VAEntrypointVideoProc
                                           : VAEntrypointVLD;
  *count = 1;
  return VA_STATUS_SUCCESS;
}

static uint32_t SupportedConfigRtFormats(VAProfile profile) {
  return profile == VAProfileNone
             ? VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_RGB32
             : VA_RT_FORMAT_YUV420;
}

static uint32_t ConfigRtFormats(const Config &config) {
  return config.rt_format != 0 ? config.rt_format
                                : SupportedConfigRtFormats(config.profile);
}

static uint32_t SurfaceRtFormat(const Surface &surface) {
  if (surface.fourcc == VA_FOURCC_NV12)
    return VA_RT_FORMAT_YUV420;
  if (surface.fourcc == VA_FOURCC_ARGB)
    return VA_RT_FORMAT_RGB32;
  return 0;
}

static VAStatus GetConfigAttributes(VADriverContextP, VAProfile profile,
                                    VAEntrypoint entrypoint,
                                    VAConfigAttrib *attributes, int count) {
  if (!IsSupportedProfile(profile))
    return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
  if (!IsValidConfig(profile, entrypoint))
    return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
  if (count < 0 || (count != 0 && attributes == nullptr))
    return VA_STATUS_ERROR_INVALID_PARAMETER;
  for (int i = 0; i < count; ++i) {
    switch (attributes[i].type) {
      case VAConfigAttribRTFormat:
        attributes[i].value = SupportedConfigRtFormats(profile);
        break;
      case VAConfigAttribDecSliceMode:
        attributes[i].value = IsDecodeProfile(profile)
                                  ? VA_DEC_SLICE_MODE_NORMAL
                                  : VA_ATTRIB_NOT_SUPPORTED;
        break;
      case VAConfigAttribDecProcessing:
        attributes[i].value = IsH264Profile(profile)
                                  ? VA_DEC_PROCESSING
                                  : VA_ATTRIB_NOT_SUPPORTED;
        break;
      case VAConfigAttribMaxPictureWidth:
        attributes[i].value = profile == VAProfileMPEG2Simple ? 720 : kMaxWidth;
        break;
      case VAConfigAttribMaxPictureHeight:
        attributes[i].value = profile == VAProfileMPEG2Simple ? 576 : kMaxHeight;
        break;
      default:
        attributes[i].value = VA_ATTRIB_NOT_SUPPORTED;
        break;
    }
  }
  return VA_STATUS_SUCCESS;
}

static VAStatus CreateConfig(VADriverContextP context, VAProfile profile,
                             VAEntrypoint entrypoint,
                             VAConfigAttrib *attributes, int count,
                             VAConfigID *config_id) {
  if (!IsSupportedProfile(profile))
    return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
  if (!IsValidConfig(profile, entrypoint))
    return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
  if (config_id == nullptr || count < 0 ||
      (count != 0 && attributes == nullptr))
    return VA_STATUS_ERROR_INVALID_PARAMETER;
  uint32_t rt_format = SupportedConfigRtFormats(profile);
  bool decode_processing = false;
  bool processing_seen = false;
  for (int i = 0; i < count; ++i) {
    switch (attributes[i].type) {
      case VAConfigAttribRTFormat:
        if (attributes[i].value == 0 ||
            (attributes[i].value & ~SupportedConfigRtFormats(profile)) != 0)
          return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
        rt_format = attributes[i].value;
        break;
      case VAConfigAttribDecSliceMode:
        if (!IsDecodeProfile(profile) ||
            attributes[i].value != VA_DEC_SLICE_MODE_NORMAL)
          return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
        break;
      case VAConfigAttribDecProcessing:
        if ((attributes[i].value != VA_DEC_PROCESSING_NONE &&
             (attributes[i].value != VA_DEC_PROCESSING || !IsH264Profile(profile))) ||
            (processing_seen &&
             decode_processing != (attributes[i].value == VA_DEC_PROCESSING)))
          return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
        processing_seen = true;
        decode_processing = attributes[i].value == VA_DEC_PROCESSING;
        break;
      case VAConfigAttribEncryption:
        if (attributes[i].value != 0)
          return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
        break;
#if VA_CHECK_VERSION(1, 11, 0)
      case VAConfigAttribProtectedContentCipherAlgorithm:
      case VAConfigAttribProtectedContentCipherBlockSize:
      case VAConfigAttribProtectedContentCipherMode:
      case VAConfigAttribProtectedContentCipherSampleType:
      case VAConfigAttribProtectedContentUsage:
        return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
#endif
      default:
        break;
    }
  }
  Driver *driver = GetDriver(context);
  std::lock_guard<std::mutex> lock(driver->mutex);
  const VAConfigID created = driver->next_config;
  try {
    Config selected{profile, entrypoint, rt_format};
    selected.decode_processing = decode_processing;
    if (!driver->configs.emplace(created, selected).second)
      return VA_STATUS_ERROR_OPERATION_FAILED;
  } catch (const std::bad_alloc &) {
    return VA_STATUS_ERROR_ALLOCATION_FAILED;
  }
  ++driver->next_config;
  *config_id = created;
  return VA_STATUS_SUCCESS;
}

static VAStatus DestroyConfig(VADriverContextP context, VAConfigID config_id) {
  Driver *driver = GetDriver(context);
  std::lock_guard<std::mutex> lock(driver->mutex);
  return driver->configs.erase(config_id) != 0 ? VA_STATUS_SUCCESS
                                               : VA_STATUS_ERROR_INVALID_CONFIG;
}

static VAStatus QueryConfigAttributes(VADriverContextP context,
                                      VAConfigID config_id, VAProfile *profile,
                                      VAEntrypoint *entrypoint,
                                      VAConfigAttrib *attributes, int *count) {
  if (profile == nullptr || entrypoint == nullptr || count == nullptr)
    return VA_STATUS_ERROR_INVALID_PARAMETER;
  Driver *driver = GetDriver(context);
  std::lock_guard<std::mutex> lock(driver->mutex);
  auto config = driver->configs.find(config_id);
  if (config == driver->configs.end())
    return VA_STATUS_ERROR_INVALID_CONFIG;
  *profile = config->second.profile;
  *entrypoint = config->second.entrypoint;
  if (attributes != nullptr) {
    attributes[0].type = VAConfigAttribRTFormat;
    attributes[0].value = ConfigRtFormats(config->second);
    if (config->second.decode_processing) {
      attributes[1].type = VAConfigAttribDecProcessing;
      attributes[1].value = VA_DEC_PROCESSING;
    }
  }
  *count = config->second.decode_processing ? 2 : 1;
  return VA_STATUS_SUCCESS;
}

static VAStatus CreateSurfaces2(VADriverContextP context, unsigned int format,
                                unsigned int width, unsigned int height,
                                VASurfaceID *surface_ids,
                                unsigned int surface_count,
                                VASurfaceAttrib *attributes,
                                unsigned int attribute_count) {
  if ((format != VA_RT_FORMAT_YUV420 && format != VA_RT_FORMAT_RGB32) ||
      width == 0 || height == 0 ||
      width > kMaxWidth || height > kMaxHeight || surface_ids == nullptr)
    return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
  if (attribute_count != 0 && attributes == nullptr)
    return VA_STATUS_ERROR_INVALID_PARAMETER;

  int memory_type = VA_SURFACE_ATTRIB_MEM_TYPE_VA;
  uint32_t pixel_format = format == VA_RT_FORMAT_RGB32 ? VA_FOURCC_ARGB
                                                       : VA_FOURCC_NV12;
  void *descriptor = nullptr;
  for (unsigned int i = 0; i < attribute_count; ++i) {
    switch (attributes[i].type) {
      case VASurfaceAttribMemoryType:
        if (attributes[i].value.type != VAGenericValueTypeInteger)
          return VA_STATUS_ERROR_INVALID_PARAMETER;
        memory_type = attributes[i].value.value.i;
        break;
      case VASurfaceAttribPixelFormat:
        if (attributes[i].value.type != VAGenericValueTypeInteger)
          return VA_STATUS_ERROR_INVALID_PARAMETER;
        pixel_format = attributes[i].value.value.i;
        break;
      case VASurfaceAttribExternalBufferDescriptor:
        if (attributes[i].value.type != VAGenericValueTypePointer)
          return VA_STATUS_ERROR_INVALID_PARAMETER;
        descriptor = attributes[i].value.value.p;
        break;
      default:
        break;
    }
  }
  if ((format == VA_RT_FORMAT_YUV420 && pixel_format != VA_FOURCC_NV12) ||
      (format == VA_RT_FORMAT_RGB32 && pixel_format != VA_FOURCC_ARGB))
    return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
  // PRIME2 supplies one descriptor per surface. Legacy PRIME instead
  // supplies a single geometry descriptor with one buffers[] handle per
  // surface; make a one-handle view only when calling the Surface importer.
  const auto *legacy_descriptor = memory_type == VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME
      ? static_cast<const VASurfaceAttribExternalBuffers *>(descriptor) : nullptr;
  if (legacy_descriptor != nullptr &&
      (legacy_descriptor->buffers == nullptr ||
       legacy_descriptor->num_buffers < surface_count))
    return VA_STATUS_ERROR_INVALID_PARAMETER;
  Driver *driver = GetDriver(context);
  std::lock_guard<std::mutex> lock(driver->mutex);
  const auto pending_backing = [&](int fd) {
    BackingIdentity candidate;
    if (!GetBackingIdentity(fd, &candidate))
      return false;
    for (const PendingVpp &pending : driver->pending_vpp) {
      for (int object_fd : pending.target->object_fds) {
        BackingIdentity existing;
        if (GetBackingIdentity(object_fd, &existing) && existing == candidate)
          return true;
      }
    }
    return false;
  };
  // Preflight every imported object before ImportPrime/Legacy can map it.
  // The old VA owner may already have been removed from backing_owners, but
  // queued VPP still retains its fds and unsignaled fence. Even GBM mapping
  // can wait on that fence, so detecting it after import is too late.
  for (unsigned int i = 0; i < surface_count; ++i) {
    if (memory_type == VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2 && descriptor != nullptr) {
      const auto &desc = static_cast<const VADRMPRIMESurfaceDescriptor *>(descriptor)[i];
      if (desc.num_objects == 0 || desc.num_objects > 4)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
      for (unsigned int object = 0; object < desc.num_objects; ++object)
        if (pending_backing(desc.objects[object].fd))
          return VA_STATUS_ERROR_SURFACE_BUSY;
    } else if (memory_type == VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME && descriptor != nullptr) {
      if (legacy_descriptor->buffers[i] > static_cast<uintptr_t>(INT_MAX))
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
      if (pending_backing(static_cast<int>(legacy_descriptor->buffers[i])))
        return VA_STATUS_ERROR_SURFACE_BUSY;
    }
  }
  std::vector<VASurfaceID> created;
  std::vector<BackingIdentity> registered;
  std::unordered_set<VASurfaceID> write_exposed_owners;
  const auto rollback = [&](VAStatus status) {
    for (const BackingIdentity &identity : registered)
      driver->backing_owners.erase(identity);
    for (VASurfaceID id : created)
      driver->surfaces.erase(id);
    return status;
  };
  for (unsigned int i = 0; i < surface_count; ++i) {
    auto surface = std::make_shared<Surface>();
    surface->width = width;
    surface->height = height;
    surface->rt_format = format;
    surface->fourcc = pixel_format;
    SurfaceLayout layout;
    VASurfaceAttribExternalBuffers legacy_surface = {};
    if (legacy_descriptor != nullptr) {
      legacy_surface = *legacy_descriptor;
      legacy_surface.num_buffers = 1;
      legacy_surface.buffers = legacy_descriptor->buffers + i;
    }
    if (memory_type == VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2 && descriptor != nullptr) {
      const auto &desc = static_cast<const VADRMPRIMESurfaceDescriptor *>(descriptor)[i];
      if (!GetPrimeLayout(desc, &layout))
        return rollback(VA_STATUS_ERROR_ALLOCATION_FAILED);
    } else if (memory_type == VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME && descriptor != nullptr) {
      if (!GetLegacyLayout(legacy_surface, &layout))
        return rollback(VA_STATUS_ERROR_ALLOCATION_FAILED);
    }
    for (const BackingIdentity &identity : layout.objects) {
      const auto known = driver->backing_owners.find(identity);
      if (known != driver->backing_owners.end()) {
        const auto owner = driver->surfaces.find(known->second);
        if (owner == driver->surfaces.end() || !HasLiveBackingOwner(driver, owner->second.get()))
          return rollback(VA_STATUS_ERROR_INVALID_SURFACE);
        if (owner->second->opaque_reference ||
            ExperimentalPresentationBinding(driver, owner->second.get()) ||
            owner->second->experimental_write_lease ||
            owner->second->experimental_read_export_armed ||
            owner->second->experimental_presentation)
          return rollback(VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
        // Same backing does not necessarily mean the same pixels. Refuse
        // nonidentical views before mapping instead of selecting another
        // surface's immutable decoded picture for different plane geometry.
        if (!(owner->second->layout == layout))
          return rollback(VA_STATUS_ERROR_INVALID_PARAMETER);
        if (surface->backing_owner != VA_INVALID_SURFACE &&
            surface->backing_owner != known->second)
          return rollback(VA_STATUS_ERROR_INVALID_PARAMETER);
        surface->backing_owner = known->second;
        surface->ready = owner->second->ready;
        surface->failed = owner->second->failed;
        surface->expected_timestamp = owner->second->expected_timestamp;
        surface->frame_timestamp = owner->second->frame_timestamp;
        surface->trace_context = owner->second->trace_context;
        surface->trace_generation = owner->second->trace_generation;
        surface->trace_submission_ordinal =
            owner->second->trace_submission_ordinal;
        surface->trace_decode_identity =
            owner->second->trace_decode_identity;
      }
    }
    if (surface->backing_owner != VA_INVALID_SURFACE)
      write_exposed_owners.insert(surface->backing_owner);
    bool ok = false;
    if (memory_type == VA_SURFACE_ATTRIB_MEM_TYPE_VA) {
      ok = surface->AllocateInternal(driver->gbm, driver->drm_fd, width, height,
                                     pixel_format,
                                     driver->experimental_i915_wc,
                                     &driver->primary_node_cache);
    } else if (memory_type == VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2 &&
               descriptor != nullptr) {
      const auto *descriptors =
          static_cast<const VADRMPRIMESurfaceDescriptor *>(descriptor);
      ok = surface->ImportPrime(driver->gbm, descriptors[i]);
    } else if (memory_type == VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME &&
               descriptor != nullptr) {
      ok = surface->ImportLegacy(legacy_surface);
    }
    if (!ok)
      return rollback(VA_STATUS_ERROR_ALLOCATION_FAILED);
    surface->direct_decode_eligible =
        memory_type == VA_SURFACE_ATTRIB_MEM_TYPE_VA;
    surface->layout = layout;
    if (surface->backing_owner == VA_INVALID_SURFACE)
      surface->ready = true;
    VASurfaceID id = driver->next_surface++;
    surface->id = id;
    for (const BackingIdentity &identity : layout.objects) {
      const VASurfaceID owner = surface->backing_owner == VA_INVALID_SURFACE
                                   ? id : surface->backing_owner;
      if (driver->backing_owners.emplace(identity, owner).second)
        registered.push_back(identity);
    }
    driver->surfaces[id] = std::move(surface);
    surface_ids[i] = id;
    created.push_back(id);
  }
  // Importing an exported backing introduces another VA object that can be
  // used as a writer. Keep future decode output private on its canonical
  // owner even if the original export was declared read-only.
  for (VASurfaceID owner_id : write_exposed_owners) {
    const auto owner = driver->surfaces.find(owner_id);
    if (owner != driver->surfaces.end()) {
      if (!PromoteDecodedPicture(owner->second->decode_picture.lock()))
        return rollback(VA_STATUS_ERROR_OPERATION_FAILED);
      ClearTraceReadExport(owner->second.get());
      owner->second->direct_decode_eligible = false;
    }
  }
  return VA_STATUS_SUCCESS;
}

static VAStatus CreateSurfaces(VADriverContextP context, int width, int height,
                               int format, int surface_count,
                               VASurfaceID *surface_ids) {
  return CreateSurfaces2(context, format, width, height, surface_ids,
                         surface_count, nullptr, 0);
}

static bool ObsoleteCompletedH264Surface(const DecodeContext &decode,
                                         const Surface &surface,
                                         uint64_t timestamp) {
  if (!IsH264Profile(decode.profile) || decode.live_h264_fast_path ||
      !surface.ready || surface.failed || PublicReadRejected(&surface) ||
      surface.expected_timestamp != timestamp || surface.frame_timestamp != timestamp ||
      decode.pending.count(timestamp) != 0 ||
      !decode.replay.HasPrunedTimestamp(timestamp))
    return false;
  const auto picture = decode.decoded_frames.find(timestamp);
  return picture != decode.decoded_frames.end() &&
      surface.decode_picture.lock() == picture->second &&
      surface.decode_identity == picture->second->decode_identity &&
      picture->second->ready && !picture->second->failed &&
      picture->second->frame_timestamp == timestamp &&
      !picture->second->direct_backing;
}

static VAStatus DestroySurfacesWithTimeout(VADriverContextP context,
                                           VASurfaceID *surface_ids, int count,
                                           uint64_t close_timeout_ns) {
  if (count < 0 || (count != 0 && surface_ids == nullptr))
    return VA_STATUS_ERROR_INVALID_PARAMETER;
  Driver *driver = GetDriver(context);
  std::lock_guard<std::mutex> lock(driver->mutex);
  if (static_cast<size_t>(count) > driver->surfaces.size())
    return VA_STATUS_ERROR_INVALID_SURFACE;
  // Validate the entire request before poisoning or removing any surface.
  // A late invalid/duplicate ID must not skip the decoder-generation reset
  // after partially destroying the decode pool.
  std::unordered_set<VASurfaceID> requested;
  for (int i = 0; i < count; ++i) {
    if (driver->surfaces.find(surface_ids[i]) == driver->surfaces.end() ||
        !requested.insert(surface_ids[i]).second)
      return VA_STATUS_ERROR_INVALID_SURFACE;
  }
  std::unordered_set<DecodeContext *> reset_decoders;
  // Fresh VPP probe surfaces have no decoder or queued conversion to wake.
  // Keep cancellation notifications for queued VPP and decode waiters,
  // including closing contexts that must not be reset a second time.
  bool notify_workers = !driver->pending_vpp.empty();
  for (int i = 0; i < count; ++i) {
    auto surface = driver->surfaces.find(surface_ids[i]);
    for (const auto &entry : driver->contexts) {
      if (entry.second->video_process)
        continue;
      auto timestamp = entry.second->surface_timestamps.find(surface->second.get());
      if (timestamp != entry.second->surface_timestamps.end()) {
        notify_workers = true;
        if (!entry.second->closing) {
          if (ObsoleteCompletedH264Surface(*entry.second, *surface->second,
                                           timestamp->second)) {
            // Drop only obsolete map owners. Captured private pixels keep
            // their shared ownership; independent pending work is untouched.
            entry.second->decoded_frames.erase(timestamp->second);
            entry.second->surface_timestamps.erase(timestamp);
          } else {
            reset_decoders.insert(entry.second.get());
          }
        }
      }
    }
    // A queued worker owns a shared_ptr after the VA ID is gone. Permanently
    // poison that object before erasing it so it can never write an old frame
    // into a DMA-BUF that Chromium recycles for a post-seek surface.
    if (surface->second->backing_owner == VA_INVALID_SURFACE)
      EndTraceBinding(driver, surface->second.get());
    driver->trace.Emit(crystalhd_vaapi_trace::Event::SurfaceDestroy,
                       TraceFields(nullptr, surface->second.get()));
    surface->second->destroyed = true;
    surface->second->completed_decode_picture = {};
    for (auto owner = driver->backing_owners.begin();
         owner != driver->backing_owners.end();) {
      if (owner->second == surface_ids[i])
        owner = driver->backing_owners.erase(owner);
      else
        ++owner;
    }
    driver->surfaces.erase(surface);
  }
  // Chromium implements a decoder flush by retiring its decode-surface pool
  // while retaining the VA context. CrystalHD otherwise keeps compressed and
  // reordered pre-seek pictures in firmware, which makes those old pictures
  // appear on the new media timeline. Restart each affected hardware decoder
  // and advance its generation so queued VPP work from that epoch is canceled.
  VAStatus reset_status = VA_STATUS_SUCCESS;
  for (DecodeContext *decode : reset_decoders) {
    Debug("reset decoder generation=%llu after decode-surface teardown",
          static_cast<unsigned long long>(decode->generation));
    MarkPendingPicturesFailed(driver, decode);
    ClearTraceReadExportsForContext(
        driver, TraceId(decode->id, VA_INVALID_ID), decode->generation);
    const BC_STATUS closed = decode->Reset(close_timeout_ns);
    if (decode->device != nullptr)
      reset_status = VA_STATUS_ERROR_HW_BUSY;
    else if (closed != BC_STS_SUCCESS && reset_status == VA_STATUS_SUCCESS)
      reset_status = VA_STATUS_ERROR_OPERATION_FAILED;
    driver->trace.Emit(crystalhd_vaapi_trace::Event::ContextReset,
                       TraceFields(decode, nullptr));
  }
  if (notify_workers)
    driver->NotifyDecodeWorker();
  return reset_status;
}

static VAStatus DestroySurfaces(VADriverContextP context,
                                VASurfaceID *surface_ids, int count) {
  return DestroySurfacesWithTimeout(context, surface_ids, count,
                                    kDecodeTimeoutNs);
}

static VAStatus QuerySurfaceAttributes(VADriverContextP context,
                                       VAConfigID config_id,
                                       VASurfaceAttrib *attributes,
                                       unsigned int *count) {
  if (count == nullptr)
    return VA_STATUS_ERROR_INVALID_PARAMETER;
  Driver *driver = GetDriver(context);
  std::lock_guard<std::mutex> lock(driver->mutex);
  auto config = driver->configs.find(config_id);
  if (config == driver->configs.end())
    return VA_STATUS_ERROR_INVALID_CONFIG;

  const uint32_t rt_format = ConfigRtFormats(config->second);
  const unsigned int required = 5 +
      ((rt_format & VA_RT_FORMAT_YUV420) != 0) +
      ((rt_format & VA_RT_FORMAT_RGB32) != 0);
  if (attributes == nullptr) {
    *count = required;
    return VA_STATUS_SUCCESS;
  }
  if (*count < required) {
    *count = required;
    return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
  }
  auto integer_attribute = [](VASurfaceAttribType type, uint32_t flags,
                              int32_t value) {
    VASurfaceAttrib attribute = {};
    attribute.type = type;
    attribute.flags = flags;
    attribute.value.type = VAGenericValueTypeInteger;
    attribute.value.value.i = value;
    return attribute;
  };
  unsigned int index = 0;
  if ((rt_format & VA_RT_FORMAT_YUV420) != 0) {
    attributes[index++] = integer_attribute(
        VASurfaceAttribPixelFormat,
        VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE, VA_FOURCC_NV12);
  }
  if ((rt_format & VA_RT_FORMAT_RGB32) != 0) {
    attributes[index++] = integer_attribute(
        VASurfaceAttribPixelFormat,
        VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE,
        VA_FOURCC_ARGB);
  }
  attributes[index++] = integer_attribute(VASurfaceAttribMinWidth,
                                          VA_SURFACE_ATTRIB_GETTABLE, 16);
  attributes[index++] = integer_attribute(VASurfaceAttribMaxWidth,
                                          VA_SURFACE_ATTRIB_GETTABLE,
                                          config->second.profile == VAProfileMPEG2Simple
                                              ? 720 : kMaxWidth);
  attributes[index++] = integer_attribute(VASurfaceAttribMinHeight,
                                          VA_SURFACE_ATTRIB_GETTABLE, 16);
  attributes[index++] = integer_attribute(VASurfaceAttribMaxHeight,
                                          VA_SURFACE_ATTRIB_GETTABLE,
                                          config->second.profile == VAProfileMPEG2Simple
                                              ? 576 : kMaxHeight);
  attributes[index] = integer_attribute(
      VASurfaceAttribMemoryType,
      VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE,
      config->second.decode_processing ? VA_SURFACE_ATTRIB_MEM_TYPE_VA :
          VA_SURFACE_ATTRIB_MEM_TYPE_VA | VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME |
              VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2);
  *count = required;
  return VA_STATUS_SUCCESS;
}

static bool IsVideoProcessContext(const Driver *driver,
                                  VAContextID context_id) {
  auto context = driver->contexts.find(context_id);
  return context != driver->contexts.end() && context->second->video_process;
}

static bool IsDecodeProcessingContext(const Driver *driver,
                                      VAContextID context_id) {
  const auto context = driver->contexts.find(context_id);
  return context != driver->contexts.end() &&
         context->second->decode_processing && !context->second->video_process;
}

static bool H264ProcessingSurfaceEligible(const Surface &surface,
                                           VAContextID context_id,
                                           bool original) {
  return surface.fourcc == VA_FOURCC_NV12 && !surface.destroyed &&
         surface.backing_owner == VA_INVALID_SURFACE && !surface.layout.valid &&
         !surface.external_accessed && !surface.image_write_locked &&
         surface.vpp_readers == 0 && surface.vpp_writers == 0 &&
         surface.export_waiters == 0 && !surface.experimental_write_lease &&
         !surface.experimental_read_export_armed && !surface.experimental_presentation &&
         (!surface.opaque_reference ||
          (original && surface.opaque_context == context_id));
}

// The caller holds the driver mutex. Copy values, never retain a pointer into
// a client buffer that may be mapped, changed or destroyed after RenderPicture.
static VAStatus ReadVppDeinterlace(const Driver *driver, VAContextID context_id,
                                  const VABufferID *filters,
                                  unsigned int filter_count,
                                  VppDeinterlace *result) {
  if (result == nullptr || filter_count > 1 ||
      (filter_count != 0 && filters == nullptr))
    return VA_STATUS_ERROR_INVALID_PARAMETER;
  VppDeinterlace selected;
  if (filter_count != 0) {
    const auto found = driver->buffers.find(filters[0]);
    if (found == driver->buffers.end())
      return VA_STATUS_ERROR_INVALID_BUFFER;
    const Buffer &buffer = found->second;
    if (buffer.context != context_id ||
        buffer.type != VAProcFilterParameterBufferType || buffer.data_failed ||
        buffer.elements != 1 ||
        buffer.element_size != sizeof(VAProcFilterParameterBufferDeinterlacing) ||
        buffer.data.size() != sizeof(VAProcFilterParameterBufferDeinterlacing))
      return VA_STATUS_ERROR_INVALID_BUFFER;
    if (buffer.map_count != 0 || BufferBorrowed(buffer))
      return VA_STATUS_ERROR_SURFACE_BUSY;
    VAProcFilterParameterBufferDeinterlacing parameters = {};
    memcpy(&parameters, buffer.data.data(), sizeof(parameters));
    if (parameters.type != VAProcFilterDeinterlacing ||
        parameters.algorithm != VAProcDeinterlacingBob)
      return VA_STATUS_ERROR_UNSUPPORTED_FILTER;
    constexpr uint32_t supported_flags = VA_DEINTERLACING_BOTTOM_FIELD_FIRST |
                                         VA_DEINTERLACING_BOTTOM_FIELD;
    // ONE_FIELD describes single-field *input storage*, not one output. The
    // current Bob implementation requires both interleaved source fields.
    if ((parameters.flags & ~supported_flags) != 0)
      return VA_STATUS_ERROR_INVALID_PARAMETER;
    // Reserved padding has no implemented meaning. Some existing clients
    // initialize only type/algorithm/flags; never make output depend on those
    // padding bytes or interpret them as support for an unknown feature.
    selected.bob = true;
    selected.field = (parameters.flags & VA_DEINTERLACING_BOTTOM_FIELD) != 0
        ? crystalhd_vpp_bob::FieldParity::kBottom
        : crystalhd_vpp_bob::FieldParity::kTop;
  }
  *result = selected;
  return VA_STATUS_SUCCESS;
}

// Validate only active semantics, before capturing a picture or acquiring any
// backing lease. Unused pointers and inactive color metadata are not requests.
static bool VppPipelineSemanticsSupported(
    const VAProcPipelineParameterBuffer &p, const Surface &target, bool bob,
    VppColorRange *color_range) {
  if (p.num_forward_references != 0 || p.num_backward_references != 0 ||
      p.num_additional_outputs != 0 ||
      (p.pipeline_flags & ~VA_PROC_PIPELINE_FAST) != 0 ||
      p.mirror_state != VA_MIRROR_NONE || p.blend_state != nullptr ||
      p.surface_color_standard != VAProcColorStandardNone ||
      p.output_color_standard != VAProcColorStandardNone ||
      p.processing_mode != VAProcDefaultMode || p.output_hdr_metadata != nullptr)
    return false;
  if (p.input_surface_flag != VA_FRAME_PICTURE &&
      p.input_surface_flag != VA_TOP_FIELD_FIRST &&
      p.input_surface_flag != VA_BOTTOM_FIELD_FIRST)
    return false;
  if (p.output_surface_flag != VA_FRAME_PICTURE &&
      (bob || p.output_surface_flag != p.input_surface_flag))
    return false;
  // Preserve the existing explicit-Bob reserved-field contract. Do not extend
  // it to ordinary VPP or inspect padding in color/filter structures.
  if (bob && std::any_of(std::begin(p.va_reserved), std::end(p.va_reserved),
                         [](uint32_t value) { return value != 0; }))
    return false;

  const auto whole = [](const VARectangle *region, const Surface &surface) {
    return region == nullptr ||
        (region->x == 0 && region->y == 0 &&
         region->width == surface.width && region->height == surface.height);
  };
  const bool whole_output = whole(p.output_region, target);
  // The existing padding implementation supports neutral black, not arbitrary
  // background colors. A full output has no background pixels to convert.
  if (!whole_output && p.output_background_color != 0 &&
      p.output_background_color != 0xff000000U)
    return false;

  const uint8_t input_range = p.input_color_properties.color_range;
  const uint8_t output_range = p.output_color_properties.color_range;
  if (input_range > VA_SOURCE_RANGE_FULL || output_range > VA_SOURCE_RANGE_FULL)
    return false;
  const bool source_full = input_range == VA_SOURCE_RANGE_FULL;
  if (target.fourcc == VA_FOURCC_ARGB) {
    // RGB output is full-range. Reduced-range RGB is not implemented.
    if (output_range == VA_SOURCE_RANGE_REDUCED)
      return false;
  } else if (output_range != VA_SOURCE_RANGE_UNKNOWN &&
             (output_range == VA_SOURCE_RANGE_FULL) != source_full) {
    // NV12 copy/Bob preserves samples; UNKNOWN output inherits the input.
    // Do not acknowledge an actual YUV range conversion that never happened.
    return false;
  }
  color_range->source_full = source_full;
  // Primaries/transfer/matrix apply only with the explicit color standard
  // rejected above; inactive values and reserved padding remain ignored.
  return true;
}

static VAStatus QueryVideoProcFilters(VADriverContextP context,
                                      VAContextID context_id,
                                      VAProcFilterType *filters,
                                      unsigned int *filter_count) {
  if (filter_count == nullptr)
    return VA_STATUS_ERROR_INVALID_PARAMETER;
  Driver *driver = GetDriver(context);
  std::lock_guard<std::mutex> lock(driver->mutex);
  if (IsDecodeProcessingContext(driver, context_id)) {
    *filter_count = 0;
    return VA_STATUS_SUCCESS;
  }
  if (!IsVideoProcessContext(driver, context_id))
    return VA_STATUS_ERROR_INVALID_CONTEXT;
  const unsigned int capacity = *filter_count;
  *filter_count = 1;
  if (filters == nullptr)
    return VA_STATUS_SUCCESS;
  if (capacity < 1)
    return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
  filters[0] = VAProcFilterDeinterlacing;
  return VA_STATUS_SUCCESS;
}

static VAStatus QueryVideoProcFilterCaps(VADriverContextP context,
                                         VAContextID context_id,
                                         VAProcFilterType filter, void *caps,
                                         unsigned int *capability_count) {
  if (capability_count == nullptr)
    return VA_STATUS_ERROR_INVALID_PARAMETER;
  Driver *driver = GetDriver(context);
  std::lock_guard<std::mutex> lock(driver->mutex);
  if (IsDecodeProcessingContext(driver, context_id)) {
    *capability_count = 0;
    return VA_STATUS_ERROR_UNSUPPORTED_FILTER;
  }
  if (!IsVideoProcessContext(driver, context_id))
    return VA_STATUS_ERROR_INVALID_CONTEXT;
  const unsigned int capacity = *capability_count;
  if (filter != VAProcFilterDeinterlacing) {
    *capability_count = 0;
    return VA_STATUS_ERROR_UNSUPPORTED_FILTER;
  }
  *capability_count = 1;
  if (caps == nullptr)
    return VA_STATUS_SUCCESS;
  if (capacity < 1)
    return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
  VAProcFilterCapDeinterlacing capability = {};
  capability.type = VAProcDeinterlacingBob;
  memcpy(caps, &capability, sizeof(capability));
  return VA_STATUS_SUCCESS;
}

static VAStatus QueryVideoProcPipelineCaps(VADriverContextP context,
                                           VAContextID context_id,
                                           VABufferID *filters,
                                           unsigned int filter_count,
                                           VAProcPipelineCaps *caps) {
  if (caps == nullptr)
    return VA_STATUS_ERROR_INVALID_PARAMETER;
  Driver *driver = GetDriver(context);
  std::lock_guard<std::mutex> lock(driver->mutex);
  if (IsDecodeProcessingContext(driver, context_id)) {
    if (filter_count != 0)
      return VA_STATUS_ERROR_UNSUPPORTED_FILTER;
    memset(caps, 0, sizeof(*caps));
    caps->input_pixel_format = driver->vpp_input_formats.data();
    caps->output_pixel_format = driver->vpp_input_formats.data();
    caps->num_input_pixel_formats = caps->num_output_pixel_formats = 1;
    caps->rotation_flags = 1U << VA_ROTATION_NONE;
    caps->num_additional_outputs = 1;
    caps->min_input_width = 144;
    caps->min_input_height = 16;
    caps->max_input_width = kMaxWidth;
    caps->max_input_height = kMaxHeight;
    caps->min_output_width = 128;
    caps->min_output_height = 2;
    caps->max_output_width = kMaxWidth - 2;
    caps->max_output_height = kMaxHeight;
    return VA_STATUS_SUCCESS;
  }
  if (!IsVideoProcessContext(driver, context_id))
    return VA_STATUS_ERROR_INVALID_CONTEXT;
  VppDeinterlace deinterlace;
  const VAStatus filter_status = ReadVppDeinterlace(
      driver, context_id, filters, filter_count, &deinterlace);
  if (filter_status != VA_STATUS_SUCCESS)
    return filter_status;

  // Ordinary VPP clients use caps as an output record, including uninitialized
  // extension fields. Never dereference incoming pointers: publish borrowed
  // driver-owned format metadata, as with ordinary VPP color-standard lists.
  memset(caps, 0, sizeof(*caps));
  caps->input_pixel_format = driver->vpp_input_formats.data();
  caps->num_input_pixel_formats = driver->vpp_input_formats.size();
  const auto &decode = *driver->contexts.at(context_id);
  const uint32_t rt_format = decode.rt_format != 0
                                ? decode.rt_format
                                : SupportedConfigRtFormats(VAProfileNone);
  caps->output_pixel_format = driver->vpp_output_formats.data() +
      ((rt_format & VA_RT_FORMAT_YUV420) != 0 ? 0 : 1);
  caps->num_output_pixel_formats =
      ((rt_format & VA_RT_FORMAT_YUV420) != 0) +
      ((rt_format & VA_RT_FORMAT_RGB32) != 0);
  caps->rotation_flags = 1U << VA_ROTATION_NONE;
  caps->filter_flags = VA_FILTER_SCALING_DEFAULT |
                       VA_FILTER_INTERPOLATION_NEAREST_NEIGHBOR;
  caps->min_input_width = 16;
  caps->min_input_height = 16;
  caps->max_input_width = kMaxWidth;
  caps->max_input_height = kMaxHeight;
  caps->min_output_width = 16;
  caps->min_output_height = 16;
  caps->max_output_width = kMaxWidth;
  caps->max_output_height = kMaxHeight;
  return VA_STATUS_SUCCESS;
}

static VAStatus CreateContext(VADriverContextP context, VAConfigID config_id,
                              int width, int height, int, VASurfaceID *targets,
                              int target_count, VAContextID *context_id) {
  if (context_id == nullptr || target_count < 0 ||
      (target_count != 0 && targets == nullptr))
    return VA_STATUS_ERROR_INVALID_PARAMETER;
  Driver *driver = GetDriver(context);
  std::lock_guard<std::mutex> lock(driver->mutex);
  auto config = driver->configs.find(config_id);
  if (config == driver->configs.end())
    return VA_STATUS_ERROR_INVALID_CONFIG;
  const bool video_process =
      config->second.entrypoint == VAEntrypointVideoProc;
  if (!video_process &&
      (width <= 0 || height <= 0 ||
       static_cast<unsigned int>(width) > kMaxWidth ||
       static_cast<unsigned int>(height) > kMaxHeight ||
       (config->second.profile == VAProfileMPEG2Simple &&
        (width > 720 || height > 576))))
    return VA_STATUS_ERROR_INVALID_PARAMETER;
  for (int i = 0; i < target_count; ++i) {
    const auto target = driver->surfaces.find(targets[i]);
    if (target == driver->surfaces.end())
      return VA_STATUS_ERROR_INVALID_SURFACE;
    if ((SurfaceRtFormat(*target->second) & ConfigRtFormats(config->second)) == 0)
      return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    if (target->second->opaque_reference ||
        (config->second.decode_processing &&
         !H264ProcessingSurfaceEligible(*target->second, VA_INVALID_ID, true)))
      return VA_STATUS_ERROR_INVALID_SURFACE;
  }
  if (!video_process &&
      std::any_of(driver->contexts.begin(), driver->contexts.end(),
                  [](const auto &entry) {
                    return !entry.second->video_process &&
                           entry.second->device != nullptr;
                  }))
    return VA_STATUS_ERROR_HW_BUSY;
  std::shared_ptr<DecodeContext> decode;
  try {
    decode = std::make_shared<DecodeContext>();
  } catch (const std::bad_alloc &) {
    return VA_STATUS_ERROR_ALLOCATION_FAILED;
  }
  decode->config = config_id;
  decode->profile = config->second.profile;
  decode->rt_format = ConfigRtFormats(config->second);
  decode->width = std::max(width, 0);
  decode->height = std::max(height, 0);
  decode->video_process = video_process;
  decode->decode_processing = config->second.decode_processing;
  const char *live_h264 = getenv("CRYSTALHD_VAAPI_LIVE_H264");
  const bool h264 = !video_process && IsH264Profile(decode->profile);
  const bool live_h264_fast_path =
      live_h264 != nullptr && strcmp(live_h264, "1") == 0;
  decode->live_h264 = h264 && !decode->decode_processing &&
      (live_h264 == nullptr || live_h264_fast_path);
  decode->live_h264_gap_tolerant = h264 && live_h264_fast_path;
  decode->live_h264_fast_path = h264 && live_h264_fast_path;
  const char *low_latency_h264 =
      getenv("CRYSTALHD_VAAPI_LOW_LATENCY_H264");
  decode->low_latency_h264 = h264 &&
      low_latency_h264 != nullptr && strcmp(low_latency_h264, "1") == 0;
  // Complete progressive/MBAFF frame AUs may prefeed without live fast paths.
  // An explicit value (including 0) disables automatic prefeed; AUs retain their
  // own eligibility so later sequence metadata cannot change replay behavior.
  decode->auto_h264_prefeed = h264 && !decode->live_h264_fast_path &&
                               low_latency_h264 == nullptr;
  const char *experimental_ready_remap =
      getenv("CRYSTALHD_VAAPI_EXPERIMENTAL_READY_REMAP");
  // Explicit decode processing currently uses finite replay. Never silently
  // reinterpret a requested live/READY experiment as this separate contract.
  if (decode->decode_processing &&
      (live_h264_fast_path || decode->low_latency_h264 ||
       (experimental_ready_remap != nullptr &&
        strcmp(experimental_ready_remap, "1") == 0)))
    return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
  const char *experimental_ready_delay =
      getenv("CRYSTALHD_VAAPI_EXPERIMENTAL_READY_DELAY_US");
  uint64_t experimental_ready_delay_ns = 0;
  const bool experimental_delay_valid = ParseExperimentalReadyDelay(
      experimental_ready_delay, &experimental_ready_delay_ns);
  const char *experimental_ready_policy =
      getenv("CRYSTALHD_VAAPI_EXPERIMENTAL_READY_POLICY");
  const bool experimental_policy_latest =
      experimental_ready_policy != nullptr &&
      strcmp(experimental_ready_policy, "latest") == 0;
  const bool experimental_policy_valid =
      experimental_ready_policy == nullptr ||
      strcmp(experimental_ready_policy, "previous") == 0 ||
      experimental_policy_latest;
  decode->experimental_ready_remap = decode->live_h264_fast_path &&
      decode->low_latency_h264 &&
      experimental_ready_remap != nullptr &&
      strcmp(experimental_ready_remap, "1") == 0 &&
      experimental_delay_valid && experimental_policy_valid;
  if (decode->experimental_ready_remap) {
    decode->experimental_ready_delay_ns = experimental_ready_delay_ns;
    decode->experimental_ready_latest = experimental_policy_latest;
  }
  if (decode->live_h264)
    decode->replay = CrystalHDDecodeReplay(true);
  const VAContextID created = driver->next_context;
  decode->id = created;
  // Complete fallible container allocation before acquiring a real session.
  // The driver lock keeps this entry private until admission succeeds.
  try {
    driver->contexts.emplace(created, decode);
  } catch (const std::bad_alloc &) {
    return VA_STATUS_ERROR_ALLOCATION_FAILED;
  }
  if (!video_process) {
    const VAStatus opened = OpenDevice(decode.get());
    if (opened != VA_STATUS_SUCCESS) {
      driver->contexts.erase(created);
      return opened;
    }
    // Consume this exact lease on the first submit. Later finite replay and
    // reset retain their existing full-device close/reopen workaround.
    decode->device_preopened = true;
  }
  ++driver->next_context;
  *context_id = created;
  return VA_STATUS_SUCCESS;
}

static VAStatus DestroyContextWithTimeout(VADriverContextP context,
                                          VAContextID context_id,
                                          uint64_t close_timeout_ns) {
  Driver *driver = GetDriver(context);
  std::unique_lock<std::mutex> lock(driver->mutex);
  auto found = driver->contexts.find(context_id);
  if (found == driver->contexts.end())
    return VA_STATUS_ERROR_INVALID_CONTEXT;
  const std::shared_ptr<DecodeContext> decode = found->second;
  if (!decode->destroy_prepared) {
    // A second call while the first call has temporarily released the driver
    // lock for draining is not a close retry yet.
    if (decode->closing)
      return VA_STATUS_ERROR_INVALID_CONTEXT;
    decode->closing = true;
    decode->destroy_status = VA_STATUS_SUCCESS;
    if (!decode->video_process) {
      decode->destroy_status = DrainClosingContext(driver, decode, &lock);
      if (decode->destroy_status == VA_STATUS_SUCCESS)
        decode->destroy_status = PrepareCompletedDecodePictures(driver,
                                                                decode.get());
    }
    decode->retired = true;
    ClearTraceReadExportsForContext(
        driver, TraceId(decode->id, VA_INVALID_ID), decode->generation);
    decode->destroy_prepared = true;
  }
  const BC_STATUS closed = decode->Close(close_timeout_ns);
  // The sole non-consuming result keeps the context in the map. A later
  // vaDestroyContext call resumes only this terminal close; it never drains or
  // emits the destroy trace twice.
  if (decode->device != nullptr)
    return VA_STATUS_ERROR_HW_BUSY;
  if (decode->destroy_status == VA_STATUS_SUCCESS &&
      closed != BC_STS_SUCCESS)
    decode->destroy_status = VA_STATUS_ERROR_OPERATION_FAILED;
  if (decode->destroy_status == VA_STATUS_SUCCESS) {
    for (auto &completed : decode->retirement_candidates) {
      const auto owner = completed.owner.lock();
      // Close may have returned BUSY earlier. Another API user can destroy
      // or replace the public binding before the consuming retry succeeds.
      if (CompletedDecodePictureMatches(driver, owner, completed))
        owner->completed_decode_picture = std::move(completed);
    }
  }
  decode->retirement_candidates.clear();
  const VAStatus status = decode->destroy_status;
  crystalhd_vaapi_trace::Fields fields = TraceFields(decode.get(), nullptr);
  fields.outcome = status;
  driver->trace.Emit(crystalhd_vaapi_trace::Event::ContextDestroy, fields);
  // The drain releases the lock. Another context insertion may rehash the
  // map, so never retain its iterator across that interval.
  driver->contexts.erase(context_id);
  // Capability-only VPP contexts never opened the decoder or queued work.
  // Their disposal must not interrupt an unrelated decoder's RX poll.
  if (!decode->video_process || !driver->pending_vpp.empty())
    driver->NotifyDecodeWorker();
  return status;
}

static VAStatus DestroyContext(VADriverContextP context,
                               VAContextID context_id) {
  return DestroyContextWithTimeout(context, context_id, kDecodeTimeoutNs);
}

static bool IsProtectedBufferType(VABufferType type) {
  switch (type) {
    case VAProtectedSliceDataBufferType:
#if VA_CHECK_VERSION(1, 11, 0)
    case VAProtectedSessionExecuteBufferType:
    case VAEncryptionParameterBufferType:
#endif
      return true;
    default:
      return false;
  }
}

static VAStatus CreateBuffer(VADriverContextP context, VAContextID context_id,
                             VABufferType type, unsigned int element_size,
                             unsigned int elements, void *data,
                             VABufferID *buffer_id) {
  Driver *driver = GetDriver(context);
  std::lock_guard<std::mutex> lock(driver->mutex);
  if (context_id != VA_INVALID_ID) {
    auto context = driver->contexts.find(context_id);
    if (context == driver->contexts.end() || context->second->closing)
      return VA_STATUS_ERROR_INVALID_CONTEXT;
  }
  if (buffer_id == nullptr || element_size == 0 || elements == 0)
    return VA_STATUS_ERROR_INVALID_PARAMETER;
  if (IsProtectedBufferType(type))
    return VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;
  if (elements > std::numeric_limits<size_t>::max() / element_size)
    return VA_STATUS_ERROR_ALLOCATION_FAILED;
  try {
    Buffer buffer;
    buffer.context = context_id;
    buffer.type = type;
    buffer.element_size = element_size;
    buffer.elements = elements;
    const size_t total_bytes = static_cast<size_t>(element_size) * elements;
    if (total_bytes > buffer.data.max_size())
      return VA_STATUS_ERROR_ALLOCATION_FAILED;
    buffer.data.resize(total_bytes);
    if (data != nullptr)
      memcpy(buffer.data.data(), data, buffer.data.size());
    const VABufferID next = driver->next_buffer;
    const auto inserted = driver->buffers.emplace(next, std::move(buffer));
    if (!inserted.second)
      return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
    *buffer_id = next;
    ++driver->next_buffer;
    return VA_STATUS_SUCCESS;
  } catch (const std::bad_alloc &) {
    return VA_STATUS_ERROR_ALLOCATION_FAILED;
  }
}

static VAStatus BufferSetNumElements(VADriverContextP context,
                                     VABufferID buffer_id,
                                     unsigned int elements) {
  Driver *driver = GetDriver(context);
  std::lock_guard<std::mutex> lock(driver->mutex);
  auto buffer = driver->buffers.find(buffer_id);
  if (buffer == driver->buffers.end())
    return VA_STATUS_ERROR_INVALID_BUFFER;
  if (BufferBorrowed(buffer->second) || buffer->second.map_count != 0)
    return VA_STATUS_ERROR_SURFACE_BUSY;
  if (buffer->second.data_failed)
    return VA_STATUS_ERROR_INVALID_BUFFER;
  if (elements > std::numeric_limits<size_t>::max() /
                     buffer->second.element_size)
    return VA_STATUS_ERROR_ALLOCATION_FAILED;
  const size_t total_bytes =
      static_cast<size_t>(buffer->second.element_size) * elements;
  if (total_bytes > buffer->second.data.max_size())
    return VA_STATUS_ERROR_ALLOCATION_FAILED;
  try {
    buffer->second.data.resize(total_bytes);
  } catch (const std::bad_alloc &) {
    return VA_STATUS_ERROR_ALLOCATION_FAILED;
  }
  buffer->second.elements = elements;
  return VA_STATUS_SUCCESS;
}

static VAStatus ImageWriteStatus(Driver *driver, Surface *surface);
static VAStatus PrepareDerivedWrite(Driver *driver, Buffer &buffer, bool refresh);
static VAStatus FinishDerivedWrite(Driver *driver, Buffer &buffer, bool success);

// These libva hint values predate their macros in older supported headers.
constexpr uint32_t kImageMapRead = 1U;
constexpr uint32_t kImageMapWrite = 2U;

static VAStatus MapBufferWithFlags(VADriverContextP context, VABufferID buffer_id,
                                   void **data, uint32_t flags) {
  Driver *driver = GetDriver(context);
  std::lock_guard<std::mutex> lock(driver->mutex);
  auto buffer = driver->buffers.find(buffer_id);
  if (buffer == driver->buffers.end() || data == nullptr)
    return VA_STATUS_ERROR_INVALID_BUFFER;
  if (BufferBorrowed(buffer->second))
    return VA_STATUS_ERROR_SURFACE_BUSY;
  if (buffer->second.data_failed || buffer->second.map_count == UINT_MAX)
    return VA_STATUS_ERROR_INVALID_BUFFER;
  if ((flags & ~(kImageMapRead | kImageMapWrite)) != 0)
    return VA_STATUS_ERROR_INVALID_PARAMETER;
  const bool write = flags == 0 || (flags & kImageMapWrite) != 0;
  if (write && buffer->second.derived_source) {
    if (!buffer->second.derived_write_active && buffer->second.map_count != 0)
      return VA_STATUS_ERROR_SURFACE_BUSY;
    const VAStatus status = PrepareDerivedWrite(driver, buffer->second,
                                                flags == 0 || (flags & kImageMapRead));
    if (status != VA_STATUS_SUCCESS)
      return status;
  } else if ((flags & kImageMapWrite) != 0 &&
             buffer->second.derived_image != VA_INVALID_ID) {
    return VA_STATUS_ERROR_OPERATION_FAILED;
  }
  ++buffer->second.map_count;
  *data = buffer->second.data.data();
  return VA_STATUS_SUCCESS;
}

static VAStatus MapBuffer(VADriverContextP context, VABufferID buffer_id,
                          void **data) {
  return MapBufferWithFlags(context, buffer_id, data, 0);
}

#if VA_CHECK_VERSION(1, 21, 0)
static VAStatus MapBuffer2(VADriverContextP context, VABufferID buffer_id,
                           void **data, uint32_t flags) {
  return MapBufferWithFlags(context, buffer_id, data, flags);
}
#endif

static VAStatus UnmapBuffer(VADriverContextP context, VABufferID buffer_id) {
  Driver *driver = GetDriver(context);
  std::lock_guard<std::mutex> lock(driver->mutex);
  auto buffer = driver->buffers.find(buffer_id);
  if (buffer == driver->buffers.end() || buffer->second.map_count == 0)
    return VA_STATUS_ERROR_INVALID_BUFFER;
  --buffer->second.map_count;
  if (buffer->second.map_count == 0 && buffer->second.derived_write_active)
    return FinishDerivedWrite(driver, buffer->second, true);
  return VA_STATUS_SUCCESS;
}

static VAStatus DestroyBuffer(VADriverContextP context, VABufferID buffer_id) {
  Driver *driver = GetDriver(context);
  std::lock_guard<std::mutex> lock(driver->mutex);
  auto buffer = driver->buffers.find(buffer_id);
  if (buffer == driver->buffers.end())
    return VA_STATUS_ERROR_INVALID_BUFFER;
  if (BufferBorrowed(buffer->second) || buffer->second.map_count != 0)
    return VA_STATUS_ERROR_SURFACE_BUSY;
  driver->buffers.erase(buffer);
  return VA_STATUS_SUCCESS;
}

static VAStatus BufferInfo(VADriverContextP context, VABufferID buffer_id,
                           VABufferType *type, unsigned int *size,
                           unsigned int *elements) {
  Driver *driver = GetDriver(context);
  std::lock_guard<std::mutex> lock(driver->mutex);
  auto buffer = driver->buffers.find(buffer_id);
  if (buffer == driver->buffers.end())
    return VA_STATUS_ERROR_INVALID_BUFFER;
  if (type != nullptr)
    *type = buffer->second.type;
  if (size != nullptr)
    *size = buffer->second.element_size;
  if (elements != nullptr)
    *elements = buffer->second.elements;
  return VA_STATUS_SUCCESS;
}

static VAStatus BeginPicture(VADriverContextP context, VAContextID context_id,
                             VASurfaceID target) {
  Driver *driver = GetDriver(context);
  std::unique_lock<std::mutex> lock(driver->mutex);
  auto decode = driver->contexts.find(context_id);
  if (decode == driver->contexts.end() || decode->second->closing)
    return VA_STATUS_ERROR_INVALID_CONTEXT;
  auto target_surface = driver->surfaces.find(target);
  if (target_surface == driver->surfaces.end() ||
      !HasLiveBackingOwner(driver, target_surface->second.get()))
    return VA_STATUS_ERROR_INVALID_SURFACE;
  const uint32_t rt_format = decode->second->rt_format != 0
      ? decode->second->rt_format
      : (decode->second->video_process
             ? SupportedConfigRtFormats(VAProfileNone)
             : VA_RT_FORMAT_YUV420);
  if ((SurfaceRtFormat(*target_surface->second) & rt_format) == 0)
    return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
  Surface *canonical_target = BackingOwner(driver, target_surface->second.get());
  if (canonical_target->opaque_reference &&
      (!decode->second->decode_processing ||
       canonical_target->opaque_context != context_id))
    return VA_STATUS_ERROR_INVALID_SURFACE;
  if (decode->second->decode_processing &&
      !H264ProcessingSurfaceEligible(*target_surface->second, context_id, true))
    return VA_STATUS_ERROR_INVALID_SURFACE;
  if (decode->second->video_process) {
    const std::shared_ptr<Surface> owner_surface =
        target_surface->second->backing_owner == VA_INVALID_SURFACE
            ? target_surface->second
            : driver->surfaces.at(target_surface->second->backing_owner);
    Surface *target_owner = owner_surface.get();
    if (target_owner->vpp_writers != 0 || target_owner->export_waiters != 0)
      return VA_STATUS_ERROR_HW_BUSY;
    // The experimental decode path deliberately excludes VPP and writable
    // aliases.  A VPP writer cannot take a backing whose prior read-only
    // export is still armed or whose pixels are frozen as an older payload.
    if (ExperimentalPresentationBinding(driver, target_owner) ||
        target_owner->experimental_write_lease ||
        target_owner->experimental_read_export_armed ||
        target_owner->experimental_presentation)
      return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    if (!PromoteDecodedPicture(target_owner->decode_picture.lock()))
      return VA_STATUS_ERROR_OPERATION_FAILED;
    ProbeTraceReadExportReuse(driver, decode->second.get(),
                              target_surface->second.get(), target_owner);
    EndTraceBinding(driver, target_owner);
    if (target_surface->second.get() != target_owner)
      ClearTraceIdentity(target_surface->second.get());
    RevokeDecodeOwner(driver, target_owner);
    target_owner->picture_transaction = NextDecodeIdentity(driver);
    decode->second->target = target;
    decode->second->transaction_target = owner_surface;
    decode->second->target_transaction = target_owner->picture_transaction;
    // Chromium renders into an imported alias of an exported ARGB surface.
    // Track readiness on both aliases until VPP commits the exact frame.
    target_surface->second->expected_timestamp = 0;
    target_surface->second->frame_timestamp = 0;
    target_owner->expected_timestamp = 0;
    target_owner->frame_timestamp = 0;
    SetSurfaceState(driver, target_surface->second.get(), false, false);
    decode->second->vpp_source = VA_INVALID_SURFACE;
    decode->second->vpp_frame.reset();
    decode->second->vpp_decoder.reset();
    decode->second->vpp_deinterlace = {};
    decode->second->vpp_color_range = {};
    decode->second->have_vpp_parameters = false;
    return VA_STATUS_SUCCESS;
  }
  // Replay/timestamp ownership is keyed by the canonical decode target.
  // Imported aliases remain supported for image/VPP access, not VLD targets.
  if (target_surface->second->backing_owner != VA_INVALID_SURFACE)
    return VA_STATUS_ERROR_INVALID_SURFACE;
  if (target_surface->second->vpp_writers != 0 ||
      target_surface->second->vpp_readers != 0 ||
      target_surface->second->export_waiters != 0)
    return VA_STATUS_ERROR_HW_BUSY;
  if (!decode->second->experimental_ready_remap &&
      (ExperimentalPresentationBinding(driver, target_surface->second.get()) ||
       target_surface->second->experimental_write_lease ||
       target_surface->second->experimental_read_export_armed ||
       target_surface->second->experimental_presentation))
    return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
  if (decode->second->experimental_ready_remap &&
      (!target_surface->second->direct_decode_eligible ||
       target_surface->second->experimental_incompatible))
    return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
  const ReadExportReuseProbe reuse = ProbeTraceReadExportReuse(
      driver, decode->second.get(), target_surface->second.get(),
      target_surface->second.get());
  if (reuse.experimental_armed &&
      (!decode->second->experimental_ready_remap || reuse.outcome != 0))
    return reuse.outcome == -EAGAIN ? VA_STATUS_ERROR_HW_BUSY
                                   : VA_STATUS_ERROR_OPERATION_FAILED;
  if (decode->second->experimental_ready_remap) {
    ClearExperimentalPresentation(target_surface->second.get());
    target_surface->second->experimental_write_lease = true;
    target_surface->second->experimental_write_context =
        TraceId(decode->second->id, VA_INVALID_ID);
    target_surface->second->experimental_write_generation =
        decode->second->generation;
    target_surface->second->experimental_write_submission_ordinal =
        decode->second->next_submission_ordinal;
  }
  decode->second->target = target;
  target_surface->second->picture_transaction = NextDecodeIdentity(driver);
  decode->second->transaction_target = target_surface->second;
  decode->second->target_transaction =
      target_surface->second->picture_transaction;
  decode->second->target_identity = target_surface->second->decode_identity;
  decode->second->have_picture = false;
  decode->second->h264_picture_invalid = false;
  decode->second->h264_framed_slice_bytes = 0;
  decode->second->processing_request = {};
  decode->second->processing_target = VA_INVALID_SURFACE;
  decode->second->processing_target_transaction = 0;
  decode->second->processing_target_identity = 0;
  decode->second->have_processing_parameters = false;
  decode->second->mpeg2_picture = DecodeContext::Mpeg2Picture();
  decode->second->mpeg4_picture = DecodeContext::Mpeg4Picture();
  decode->second->vc1_picture = DecodeContext::Vc1Picture();
  // Like i965, an omitted per-picture IQ buffer means flat matrices, not the
  // previous picture's custom matrices or the bitstream's JVT default lists.
  decode->second->iq_matrix = FlatH264IqMatrix();
  decode->second->iq_matrix_valid = true;
  decode->second->slices.clear();
  decode->second->slice_data.clear();
  return VA_STATUS_SUCCESS;
}

static VAStatus RenderMpeg2Picture(Driver *driver, DecodeContext *decode,
                                   VAContextID context_id,
                                   VABufferID *buffer_ids, int count) {
  auto &pending = decode->mpeg2_picture;
  const auto fail = [&](VAStatus status) {
    Debug("MPEG-2 RenderPicture rejected status=%d target=%u type=%d bytes=%zu",
          status, decode->target, pending.picture.picture_coding_type, pending.bytes);
    pending.invalid = true;
    return status;
  };
  if (pending.invalid || decode->target == VA_INVALID_SURFACE || count < 0 ||
      (count != 0 && buffer_ids == nullptr))
    return fail(VA_STATUS_ERROR_INVALID_PARAMETER);
  const auto reference = [&](VASurfaceID id, uint64_t *timestamp) {
    if (id == VA_INVALID_SURFACE || id == decode->target) return false;
    auto surface = driver->surfaces.find(id);
    if (surface == driver->surfaces.end() ||
        !HasLiveBackingOwner(driver, surface->second.get()) ||
        surface->second->backing_owner != VA_INVALID_SURFACE) return false;
    auto token = decode->surface_timestamps.find(surface->second.get());
    if (token == decode->surface_timestamps.end() || token->second == 0 ||
        token->second != surface->second->expected_timestamp) return false;
    const auto *unit = decode->mpeg2_replay.Find(token->second);
    if (unit == nullptr || unit->kind == CrystalHDMpeg2Replay::Kind::B) return false;
    *timestamp = token->second;
    return true;
  };
  for (int index = 0; index < count; ++index) {
    auto found = driver->buffers.find(buffer_ids[index]);
    if (found == driver->buffers.end()) return fail(VA_STATUS_ERROR_INVALID_BUFFER);
    const Buffer &buffer = found->second;
    if (buffer.context != context_id || buffer.data_failed || BufferBorrowed(buffer) ||
        buffer.data.empty() || buffer.elements == 0 ||
        static_cast<uint64_t>(buffer.element_size) * buffer.elements != buffer.data.size() ||
        buffer.data.size() > crystalhd_mpeg2::kMaxAccessUnitBytes - pending.bytes)
      return fail(VA_STATUS_ERROR_INVALID_BUFFER);
    pending.bytes += buffer.data.size();
    switch (buffer.type) {
      case VAPictureParameterBufferType: {
        if (pending.have_picture || buffer.elements != 1 ||
            buffer.element_size != sizeof(pending.picture))
          return fail(VA_STATUS_ERROR_INVALID_BUFFER);
        memcpy(&pending.picture, buffer.data.data(), sizeof(pending.picture));
        const auto &picture = pending.picture;
        if (picture.picture_coding_type < 1 || picture.picture_coding_type > 3 ||
            (picture.picture_coding_type == 1 &&
             (picture.forward_reference_picture != VA_INVALID_SURFACE ||
              picture.backward_reference_picture != VA_INVALID_SURFACE)) ||
            (picture.picture_coding_type != 3 &&
             picture.backward_reference_picture != VA_INVALID_SURFACE))
          return fail(VA_STATUS_ERROR_INVALID_PARAMETER);
        if ((picture.picture_coding_type != 1 &&
             !reference(picture.forward_reference_picture, &pending.forward)) ||
            (picture.picture_coding_type == 3 &&
             !reference(picture.backward_reference_picture, &pending.backward)))
          return fail(VA_STATUS_ERROR_INVALID_SURFACE);
        pending.have_picture = true;
        break;
      }
      case VAIQMatrixBufferType:
        if (pending.have_iq || buffer.elements != 1 ||
            buffer.element_size != sizeof(pending.iq))
          return fail(VA_STATUS_ERROR_INVALID_BUFFER);
        memcpy(&pending.iq, buffer.data.data(), sizeof(pending.iq));
        pending.have_iq = true;
        break;
      case VASliceParameterBufferType: {
        if (buffer.element_size != sizeof(VASliceParameterBufferMPEG2))
          return fail(VA_STATUS_ERROR_INVALID_BUFFER);
        std::vector<VASliceParameterBufferMPEG2> group(buffer.elements);
        memcpy(group.data(), buffer.data.data(), buffer.data.size());
        pending.parameters.push_back(std::move(group));
        break;
      }
      case VASliceDataBufferType:
        pending.data.push_back(buffer.data);
        break;
      default:
        return fail(VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE);
    }
  }
  return VA_STATUS_SUCCESS;
}

static VAStatus RenderMpeg4Picture(Driver *driver, DecodeContext *decode,
                                   VAContextID context_id,
                                   VABufferID *buffer_ids, int count) {
  auto &pending = decode->mpeg4_picture;
  const auto fail = [&](VAStatus status) {
    Debug("MPEG-4 RenderPicture rejected status=%d target=%u type=%u bytes=%zu",
          status, decode->target,
          pending.picture.vop_fields.bits.vop_coding_type, pending.bytes);
    pending.invalid = true;
    return status;
  };
  if (pending.invalid || decode->target == VA_INVALID_SURFACE || count < 0 ||
      (count != 0 && buffer_ids == nullptr))
    return fail(VA_STATUS_ERROR_INVALID_PARAMETER);
  const auto reference = [&](VASurfaceID id, uint64_t *timestamp) {
    if (id == VA_INVALID_SURFACE || id == decode->target) return false;
    auto surface = driver->surfaces.find(id);
    if (surface == driver->surfaces.end() ||
        !HasLiveBackingOwner(driver, surface->second.get()) ||
        surface->second->backing_owner != VA_INVALID_SURFACE) return false;
    auto token = decode->surface_timestamps.find(surface->second.get());
    if (token == decode->surface_timestamps.end() || token->second == 0 ||
        token->second != surface->second->expected_timestamp) return false;
    const auto *unit = decode->mpeg4_replay.Find(token->second);
    if (unit == nullptr || unit->kind == CrystalHDMpeg4Replay::Kind::B)
      return false;
    *timestamp = token->second;
    return true;
  };
  for (int index = 0; index < count; ++index) {
    auto found = driver->buffers.find(buffer_ids[index]);
    if (found == driver->buffers.end())
      return fail(VA_STATUS_ERROR_INVALID_BUFFER);
    const Buffer &buffer = found->second;
    if (buffer.context != context_id || buffer.data_failed ||
        BufferBorrowed(buffer) || buffer.data.empty() || buffer.elements == 0 ||
        static_cast<uint64_t>(buffer.element_size) * buffer.elements !=
            buffer.data.size() ||
        buffer.data.size() >
            crystalhd_mpeg4::kMaxAccessUnitBytes - pending.bytes)
      return fail(VA_STATUS_ERROR_INVALID_BUFFER);
    pending.bytes += buffer.data.size();
    switch (buffer.type) {
      case VAPictureParameterBufferType: {
        if (pending.have_picture || buffer.elements != 1 ||
            buffer.element_size != sizeof(pending.picture))
          return fail(VA_STATUS_ERROR_INVALID_BUFFER);
        memcpy(&pending.picture, buffer.data.data(), sizeof(pending.picture));
        const auto &picture = pending.picture;
        const unsigned type = picture.vop_fields.bits.vop_coding_type;
        if (type > 2 ||
            (type == 0 &&
             (picture.forward_reference_picture != VA_INVALID_SURFACE ||
              picture.backward_reference_picture != VA_INVALID_SURFACE)) ||
            (type == 1 &&
             picture.backward_reference_picture != VA_INVALID_SURFACE))
          return fail(VA_STATUS_ERROR_INVALID_PARAMETER);
        if ((type != 0 &&
             !reference(picture.forward_reference_picture,
                        &pending.forward)) ||
            (type == 2 &&
             !reference(picture.backward_reference_picture,
                        &pending.backward)))
          return fail(VA_STATUS_ERROR_INVALID_SURFACE);
        pending.have_picture = true;
        break;
      }
      case VAIQMatrixBufferType:
        if (pending.have_iq || buffer.elements != 1 ||
            buffer.element_size != sizeof(pending.iq))
          return fail(VA_STATUS_ERROR_INVALID_BUFFER);
        memcpy(&pending.iq, buffer.data.data(), sizeof(pending.iq));
        pending.have_iq = true;
        break;
      case VASliceParameterBufferType:
        if (!pending.parameters.empty() || buffer.elements != 1 ||
            buffer.element_size != sizeof(VASliceParameterBufferMPEG4))
          return fail(VA_STATUS_ERROR_INVALID_BUFFER);
        pending.parameters.resize(1);
        memcpy(pending.parameters.data(), buffer.data.data(),
               sizeof(VASliceParameterBufferMPEG4));
        break;
      case VASliceDataBufferType:
        if (!pending.data.empty())
          return fail(VA_STATUS_ERROR_INVALID_BUFFER);
        pending.data.push_back(buffer.data);
        break;
      default:
        return fail(VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE);
    }
  }
  return VA_STATUS_SUCCESS;
}

static VAStatus RenderVc1Picture(Driver *driver, DecodeContext *decode,
                                 VAContextID context_id,
                                 VABufferID *buffer_ids, int count) {
  auto &pending = decode->vc1_picture;
  const auto fail = [&](VAStatus status) {
    Debug("VC-1 RenderPicture rejected status=%d target=%u type=%u bytes=%zu",
          status, decode->target, pending.picture.picture_fields.bits.picture_type, pending.bytes);
    pending.invalid = true;
    return status;
  };
  if (pending.invalid || decode->target == VA_INVALID_SURFACE || count < 0 ||
      (count != 0 && buffer_ids == nullptr))
    return fail(VA_STATUS_ERROR_INVALID_PARAMETER);
  const auto reference = [&](VASurfaceID id, uint64_t *timestamp) {
    if (id == VA_INVALID_SURFACE || id == decode->target) return false;
    auto surface = driver->surfaces.find(id);
    if (surface == driver->surfaces.end() ||
        !HasLiveBackingOwner(driver, surface->second.get()) ||
        surface->second->backing_owner != VA_INVALID_SURFACE) return false;
    auto token = decode->surface_timestamps.find(surface->second.get());
    if (token == decode->surface_timestamps.end() || token->second == 0 ||
        token->second != surface->second->expected_timestamp) return false;
    const auto *unit = decode->vc1_replay.Find(token->second);
    if (unit == nullptr || unit->kind == CrystalHDVc1Replay::Kind::B ||
        unit->kind == CrystalHDVc1Replay::Kind::BI) return false;
    *timestamp = token->second;
    return true;
  };
  for (int index = 0; index < count; ++index) {
    auto found = driver->buffers.find(buffer_ids[index]);
    if (found == driver->buffers.end()) return fail(VA_STATUS_ERROR_INVALID_BUFFER);
    const Buffer &buffer = found->second;
    if (buffer.context != context_id || buffer.data_failed || BufferBorrowed(buffer) ||
        buffer.data.empty() || buffer.elements == 0 ||
        static_cast<uint64_t>(buffer.element_size) * buffer.elements != buffer.data.size() ||
        buffer.data.size() > crystalhd_vc1::kMaxAccessUnitBytes - pending.bytes)
      return fail(VA_STATUS_ERROR_INVALID_BUFFER);
    pending.bytes += buffer.data.size();
    switch (buffer.type) {
      case VAPictureParameterBufferType: {
        if (pending.have_picture || buffer.elements != 1 ||
            buffer.element_size != sizeof(pending.picture))
          return fail(VA_STATUS_ERROR_INVALID_BUFFER);
        memcpy(&pending.picture, buffer.data.data(), sizeof(pending.picture));
        const auto &picture = pending.picture;
        const unsigned type = picture.picture_fields.bits.picture_type;
        if (type > 4 || picture.inloop_decoded_picture != VA_INVALID_SURFACE ||
            (type == 0 &&
             (picture.forward_reference_picture != VA_INVALID_SURFACE ||
              picture.backward_reference_picture != VA_INVALID_SURFACE)) ||
            ((type == 1 || type == 4) &&
             picture.backward_reference_picture != VA_INVALID_SURFACE))
          return fail(VA_STATUS_ERROR_INVALID_PARAMETER);
        // BI is intra and is not a reference picture. Clients may retain the
        // surrounding B reference IDs; they are not dependencies for this AU.
        // P, B and skipped P must resolve exact live own-context anchor tokens.
        if (((type == 1 || type == 2 || type == 4) &&
             !reference(picture.forward_reference_picture, &pending.forward)) ||
            (type == 2 &&
             !reference(picture.backward_reference_picture, &pending.backward)))
          return fail(VA_STATUS_ERROR_INVALID_SURFACE);
        pending.have_picture = true;
        break;
      }
      case VABitPlaneBufferType:
        if (pending.have_bitplanes)
          return fail(VA_STATUS_ERROR_INVALID_BUFFER);
        pending.bitplanes = buffer.data;
        pending.have_bitplanes = true;
        break;
      case VASliceParameterBufferType: {
        if (buffer.element_size != sizeof(VASliceParameterBufferVC1))
          return fail(VA_STATUS_ERROR_INVALID_BUFFER);
        std::vector<VASliceParameterBufferVC1> parameters(buffer.elements);
        memcpy(parameters.data(), buffer.data.data(), buffer.data.size());
        pending.parameters.push_back(std::move(parameters));
        break;
      }
      case VASliceDataBufferType:
        pending.data.push_back(buffer.data);
        break;
      default:
        return fail(VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE);
    }
  }
  return VA_STATUS_SUCCESS;
}

static VAStatus CaptureDecodeProcessing(Driver *driver, DecodeContext *decode,
                                        const Buffer &buffer) {
  if (!decode->decode_processing)
    return VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;
  if (decode->have_processing_parameters || decode->target == VA_INVALID_SURFACE)
    return VA_STATUS_ERROR_INVALID_PARAMETER;
  if (buffer.context != decode->id || buffer.data_failed || buffer.elements != 1 ||
      buffer.element_size != sizeof(VAProcPipelineParameterBuffer) ||
      buffer.data.size() != sizeof(VAProcPipelineParameterBuffer))
    return VA_STATUS_ERROR_INVALID_BUFFER;
  if (buffer.map_count != 0 || BufferBorrowed(buffer))
    return VA_STATUS_ERROR_SURFACE_BUSY;
  VAProcPipelineParameterBuffer pipeline;
  memcpy(&pipeline, buffer.data.data(), sizeof(pipeline));
  if (pipeline.surface != decode->target || pipeline.surface_region == nullptr ||
      pipeline.num_additional_outputs != 1 || pipeline.additional_outputs == nullptr)
    return VA_STATUS_ERROR_INVALID_PARAMETER;
  const VASurfaceID output_id = pipeline.additional_outputs[0];
  if (output_id == decode->target)
    return VA_STATUS_ERROR_INVALID_SURFACE;
  const auto original = driver->surfaces.find(decode->target);
  const auto output = driver->surfaces.find(output_id);
  if (original == driver->surfaces.end() || output == driver->surfaces.end() ||
      !HasLiveBackingOwner(driver, original->second.get()) ||
      !HasLiveBackingOwner(driver, output->second.get()) ||
      !H264ProcessingSurfaceEligible(*original->second, decode->id, true) ||
      !H264ProcessingSurfaceEligible(*output->second, decode->id, false))
    return VA_STATUS_ERROR_INVALID_SURFACE;
  if (!output->second->ready && output->second->expected_timestamp != 0)
    return VA_STATUS_ERROR_SURFACE_BUSY;
  if (pipeline.num_filters != 0 || pipeline.filter_flags != VA_FILTER_SCALING_DEFAULT ||
      pipeline.rotation_state != VA_ROTATION_NONE)
    return VA_STATUS_ERROR_UNSUPPORTED_FILTER;
  // Reuse the no-op color/metadata admission policy, but additional_outputs
  // belongs to this integrated path, not the separate host VPP context.
  VAProcPipelineParameterBuffer semantics = pipeline;
  semantics.num_additional_outputs = 0;
  VppColorRange color_range;
  if (!VppPipelineSemanticsSupported(semantics, *output->second, false, &color_range) ||
      pipeline.input_surface_flag != pipeline.output_surface_flag)
    return VA_STATUS_ERROR_INVALID_PARAMETER;
  const VARectangle source = *pipeline.surface_region;
  const VARectangle destination = pipeline.output_region != nullptr
      ? *pipeline.output_region
      : VARectangle{0, 0, static_cast<uint16_t>(output->second->width),
                    static_cast<uint16_t>(output->second->height)};
  if (source.x != 0 || source.y != 0 ||
      original->second->width != decode->width ||
      original->second->height != decode->height ||
      destination.x != 0 || destination.y != 0 ||
      destination.width != output->second->width ||
      destination.height != output->second->height ||
      !ValidateH264ProcessingGeometry(decode->width, decode->height,
          source.width, source.height, destination.width, destination.height))
    return VA_STATUS_ERROR_INVALID_PARAMETER;
  H264ProcessingGeometry selected;
  selected.enabled = true;
  selected.visible_width = source.width;
  selected.visible_height = source.height;
  selected.capture_width = destination.width;
  selected.capture_height = destination.height;
  if (decode->processing_geometry.enabled &&
      !(decode->processing_geometry == selected))
    return VA_STATUS_ERROR_INVALID_PARAMETER;
  decode->processing_request = selected;
  decode->processing_target = output_id;
  decode->processing_target_transaction = output->second->picture_transaction;
  decode->processing_target_identity = output->second->decode_identity;
  decode->have_processing_parameters = true;
  return VA_STATUS_SUCCESS;
}

static VAStatus RenderPicture(VADriverContextP context, VAContextID context_id,
                              VABufferID *buffer_ids, int count) {
  Driver *driver = GetDriver(context);
  std::lock_guard<std::mutex> lock(driver->mutex);
  auto decode = driver->contexts.find(context_id);
  if (decode == driver->contexts.end() || decode->second->closing)
    return VA_STATUS_ERROR_INVALID_CONTEXT;
  if (decode->second->video_process) {
    // A failed replacement Render must not leave an earlier filter snapshot
    // available for EndPicture to commit as if the failed request succeeded.
    decode->second->have_vpp_parameters = false;
    decode->second->vpp_deinterlace = {};
    decode->second->vpp_color_range = {};
    // Discard the obsolete, unsubmitted capture too. Submitted work carries
    // its own references; this snapshot has not acquired any backing lease.
    decode->second->vpp_source = VA_INVALID_SURFACE;
    decode->second->vpp_frame.reset();
    decode->second->vpp_decoder.reset();
    decode->second->vpp_generation = 0;
    if (count != 1 || buffer_ids == nullptr)
      return VA_STATUS_ERROR_INVALID_PARAMETER;
    auto buffer = driver->buffers.find(buffer_ids[0]);
    if (buffer == driver->buffers.end() ||
        buffer->second.context != context_id || buffer->second.data_failed ||
        buffer->second.type != VAProcPipelineParameterBufferType ||
        buffer->second.data.size() < sizeof(VAProcPipelineParameterBuffer))
      return VA_STATUS_ERROR_INVALID_BUFFER;
    if (buffer->second.map_count != 0 || BufferBorrowed(buffer->second))
      return VA_STATUS_ERROR_SURFACE_BUSY;
    const auto *parameters =
        reinterpret_cast<const VAProcPipelineParameterBuffer *>(
            buffer->second.data.data());
    auto source = driver->surfaces.find(parameters->surface);
    auto target = driver->surfaces.find(decode->second->target);
    if (source == driver->surfaces.end() || target == driver->surfaces.end() ||
        !HasLiveBackingOwner(driver, source->second.get()) ||
        !HasLiveBackingOwner(driver, target->second.get()))
      return VA_STATUS_ERROR_INVALID_SURFACE;
    if (BackingOwner(driver, source->second.get())->opaque_reference ||
        BackingOwner(driver, target->second.get())->opaque_reference)
      return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    constexpr uint32_t supported_filter_flags =
        VA_FILTER_SCALING_DEFAULT | VA_FILTER_INTERPOLATION_NEAREST_NEIGHBOR;
    VppDeinterlace deinterlace;
    const VAStatus filter_status = ReadVppDeinterlace(
        driver, context_id, parameters->filters, parameters->num_filters,
        &deinterlace);
    if (filter_status != VA_STATUS_SUCCESS)
      return filter_status;
    // An explicit DI filter overrides vaPutSurface's field-selection hints.
    const uint32_t supported_flags = supported_filter_flags |
        (deinterlace.bob ? VA_TOP_FIELD | VA_BOTTOM_FIELD : 0);
    if ((parameters->filter_flags & ~supported_flags) != 0 ||
        parameters->rotation_state != VA_ROTATION_NONE)
      return VA_STATUS_ERROR_UNSUPPORTED_FILTER;
    VppColorRange color_range;
    if (!VppPipelineSemanticsSupported(*parameters, *target->second,
                                       deinterlace.bob, &color_range))
      return VA_STATUS_ERROR_INVALID_PARAMETER;

    // Capture the exact picture when its parameters are submitted, not later
    // in EndPicture: a different thread may reuse the public decode surface
    // between those calls. The private picture remains immutable on reuse.
    std::shared_ptr<Surface> source_owner = source->second;
    if (source_owner->backing_owner != VA_INVALID_SURFACE) {
      auto owner = driver->surfaces.find(source_owner->backing_owner);
      if (owner != driver->surfaces.end())
        source_owner = owner->second;
    }
    if (deinterlace.bob &&
        source_owner.get() == BackingOwner(driver, target->second.get()))
      return VA_STATUS_ERROR_INVALID_SURFACE;
    if (PublicReadRejected(source_owner.get()))
      return VA_STATUS_ERROR_DECODING_ERROR;
    if (ExperimentalPresentationBinding(driver, source_owner.get()) ||
        source_owner->experimental_write_lease ||
        source_owner->experimental_read_export_armed ||
        source_owner->experimental_presentation)
      return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    std::shared_ptr<Surface> frame;
    std::shared_ptr<DecodeContext> source_decoder;
    const uint64_t timestamp = source_owner->expected_timestamp;
    for (const auto &candidate : driver->contexts) {
      if (candidate.second->video_process)
        continue;
      auto source_timestamp =
          candidate.second->surface_timestamps.find(source_owner.get());
      if (source_timestamp == candidate.second->surface_timestamps.end() ||
          source_timestamp->second != timestamp)
        continue;
      auto decoded = candidate.second->decoded_frames.find(timestamp);
      if (decoded != candidate.second->decoded_frames.end()) {
        const auto owner_picture = source_owner->decode_picture.lock();
        if (source_owner->decode_identity != decoded->second->decode_identity ||
            (owner_picture && owner_picture != decoded->second) ||
            (decoded->second->direct_picture && !owner_picture))
          continue;
        source_decoder = candidate.second;
        frame = decoded->second;
        break;
      }
    }
    if (!frame) {
      const auto &completed = source_owner->completed_decode_picture;
      if (CompletedDecodePictureMatches(driver, source_owner, completed))
        frame = completed.picture;
    }
    if (!frame) {
      // A timestamped source without its picture belongs to a retired decode
      // epoch unless a successful consuming close retained that exact binding.
      // Never acknowledge it by painting a different or neutral frame.
      if (timestamp != 0)
        return VA_STATUS_ERROR_DECODING_ERROR;
      if (!source_owner->ready || source_owner->failed)
        return VA_STATUS_ERROR_INVALID_SURFACE;
      frame = source_owner;
    }
    if (!PromoteDecodedPicture(frame))
      return VA_STATUS_ERROR_OPERATION_FAILED;
    decode->second->vpp_source = parameters->surface;
    decode->second->vpp_frame = std::move(frame);
    decode->second->vpp_decoder = source_decoder;
    decode->second->vpp_generation = source_decoder
                                        ? source_decoder->generation
                                        : 0;
    decode->second->vpp_source_region = parameters->surface_region != nullptr
        ? *parameters->surface_region
        : VARectangle{0, 0, static_cast<uint16_t>(source->second->width),
                      static_cast<uint16_t>(source->second->height)};
    decode->second->vpp_output_region = parameters->output_region != nullptr
        ? *parameters->output_region
        : VARectangle{0, 0, static_cast<uint16_t>(target->second->width),
                      static_cast<uint16_t>(target->second->height)};
    decode->second->vpp_deinterlace = deinterlace;
    decode->second->vpp_color_range = color_range;
    decode->second->have_vpp_parameters = true;
    driver->trace.Emit(crystalhd_vaapi_trace::Event::VppCapture,
                       TraceFields(source_decoder.get(), source_owner.get(),
                                   timestamp));
    return VA_STATUS_SUCCESS;
  }
  if (decode->second->IsMpeg2())
    return RenderMpeg2Picture(driver, decode->second.get(), context_id, buffer_ids, count);
  if (decode->second->IsMpeg4())
    return RenderMpeg4Picture(driver, decode->second.get(), context_id,
                              buffer_ids, count);
  if (decode->second->IsVc1())
    return RenderVc1Picture(driver, decode->second.get(), context_id, buffer_ids, count);
  if (count < 0 || (count != 0 && buffer_ids == nullptr)) {
    decode->second->h264_picture_invalid = true;
    return VA_STATUS_ERROR_INVALID_PARAMETER;
  }
  for (int i = 0; i < count; ++i) {
    auto buffer = driver->buffers.find(buffer_ids[i]);
    if (buffer == driver->buffers.end())
      return VA_STATUS_ERROR_INVALID_BUFFER;
    if (IsProtectedBufferType(buffer->second.type)) {
      decode->second->h264_picture_invalid = true;
      return VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;
    }
    switch (buffer->second.type) {
      case VAProcPipelineParameterBufferType: {
        const VAStatus processing =
            CaptureDecodeProcessing(driver, decode->second.get(), buffer->second);
        if (processing != VA_STATUS_SUCCESS) {
          decode->second->h264_picture_invalid = true;
          decode->second->have_processing_parameters = false;
          decode->second->processing_target = VA_INVALID_SURFACE;
          decode->second->processing_request = {};
          return processing;
        }
        break;
      }
      case VAPictureParameterBufferType:
        if (buffer->second.data.size() < sizeof(VAPictureParameterBufferH264))
          return VA_STATUS_ERROR_INVALID_BUFFER;
        memcpy(&decode->second->picture, buffer->second.data.data(),
               sizeof(VAPictureParameterBufferH264));
        decode->second->have_picture = true;
        break;
      case VAIQMatrixBufferType:
        if (buffer->second.data.size() != sizeof(VAIQMatrixBufferH264)) {
          // Even if a caller ignores this error, EndPicture must not reuse a
          // previously submitted matrix. A complete valid replacement can
          // recover; coefficients are checked once picture parameters exist.
          decode->second->iq_matrix = FlatH264IqMatrix();
          decode->second->iq_matrix_valid = false;
          return VA_STATUS_ERROR_INVALID_BUFFER;
        }
        memcpy(&decode->second->iq_matrix, buffer->second.data.data(),
               sizeof(VAIQMatrixBufferH264));
        decode->second->iq_matrix_valid = true;
        break;
      case VASliceParameterBufferType: {
        size_t slices = buffer->second.data.size() /
                        sizeof(VASliceParameterBufferH264);
        const auto *parameters = reinterpret_cast<const VASliceParameterBufferH264 *>(
            buffer->second.data.data());
        try {
          decode->second->slices.insert(decode->second->slices.end(), parameters,
                                        parameters + slices);
        } catch (const std::bad_alloc &) {
          decode->second->h264_picture_invalid = true;
          return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
        break;
      }
      case VASliceDataBufferType: {
        const size_t framed = decode->second->h264_framed_slice_bytes;
        const size_t limit = crystalhd_access_unit_limits::kH264Bytes;
        if (framed > limit || limit - framed < 4 ||
            buffer->second.data.size() > limit - framed - 4) {
          decode->second->h264_picture_invalid = true;
          return RejectH264Picture(driver, decode->second.get(),
                                    VA_STATUS_ERROR_INVALID_BUFFER,
                                    buffer->second.data.data(),
                                    buffer->second.data.size());
        }
        try {
          decode->second->slice_data.push_back(buffer->second.data);
        } catch (const std::bad_alloc &) {
          decode->second->h264_picture_invalid = true;
          return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
        decode->second->h264_framed_slice_bytes +=
            4 + buffer->second.data.size();
        break;
      }
      default:
        break;
    }
  }
  return VA_STATUS_SUCCESS;
}

static VAStatus EndPicture(VADriverContextP context, VAContextID context_id) {
  Driver *driver = GetDriver(context);
  std::unique_lock<std::mutex> lock(driver->mutex);
  auto decode = driver->contexts.find(context_id);
  if (decode == driver->contexts.end() || decode->second->closing)
    return VA_STATUS_ERROR_INVALID_CONTEXT;
  const std::shared_ptr<DecodeContext> decode_context = decode->second;
  if (decode_context->video_process) {
    const VASurfaceID request_target = decode_context->target;
    const auto request_owner = decode_context->transaction_target.lock();
    const uint64_t request_transaction = decode_context->target_transaction;
    const auto same_request = [&] {
      return decode_context->target == request_target &&
          decode_context->transaction_target.lock() == request_owner &&
          decode_context->target_transaction == request_transaction;
    };
    auto finish_vpp = [&](VAStatus status) {
      // A synchronous source wait may release the mutex. A later Begin/Render
      // owns its own target and captured parameters; never consume them here.
      if (!same_request())
        return status;
      if (status != VA_STATUS_SUCCESS) {
        auto destination = driver->surfaces.find(request_target);
        if (destination != driver->surfaces.end()) {
          Surface *owner = BackingOwner(driver, destination->second.get());
          // Failure belongs only to this Begin's backing transaction. A newer
          // ordinary writer needs the same protection as experimental output.
          if (request_owner.get() == owner &&
              owner->picture_transaction == request_transaction &&
              owner->vpp_writers == 0 && owner->export_waiters == 0 &&
              !ExperimentalPresentationBinding(driver, owner) &&
              !owner->experimental_write_lease &&
              !owner->experimental_read_export_armed &&
              !owner->experimental_presentation)
            SetSurfaceState(driver, destination->second.get(), false, true);
        }
      }
      decode_context->target = VA_INVALID_SURFACE;
      decode_context->transaction_target.reset();
      decode_context->target_transaction = 0;
      decode_context->vpp_source = VA_INVALID_SURFACE;
      decode_context->vpp_frame.reset();
      decode_context->vpp_decoder.reset();
      decode_context->vpp_deinterlace = {};
      decode_context->vpp_color_range = {};
      decode_context->have_vpp_parameters = false;
      return status;
    };
    if (!decode_context->have_vpp_parameters)
      return finish_vpp(VA_STATUS_ERROR_INVALID_PARAMETER);
    auto source = driver->surfaces.find(decode_context->vpp_source);
    auto target = driver->surfaces.find(decode_context->target);
    if (source == driver->surfaces.end() || target == driver->surfaces.end() ||
        !HasLiveBackingOwner(driver, source->second.get()) ||
        !HasLiveBackingOwner(driver, target->second.get()))
      return finish_vpp(VA_STATUS_ERROR_INVALID_SURFACE);
    const std::shared_ptr<Surface> target_surface = target->second;
    std::shared_ptr<Surface> target_owner = target_surface;
    if (target_surface->backing_owner != VA_INVALID_SURFACE) {
      auto owner = driver->surfaces.find(target_surface->backing_owner);
      if (owner != driver->surfaces.end())
        target_owner = owner->second;
    }
    if (decode_context->vpp_deinterlace.bob &&
        target_owner.get() == BackingOwner(driver, source->second.get()))
      return finish_vpp(VA_STATUS_ERROR_INVALID_SURFACE);
    const std::shared_ptr<DecodeContext> source_decoder =
        decode_context->vpp_decoder;
    const std::shared_ptr<Surface> decoded_source = decode_context->vpp_frame;
    if (!decoded_source || decoded_source->destroyed ||
        (source_decoder &&
         (source_decoder->retired ||
          source_decoder->generation != decode_context->vpp_generation)))
      return finish_vpp(VA_STATUS_ERROR_DECODING_ERROR);
    // A cold source may have acquired a derived-image writer since Render.
    // Do not queue a reader that would prevent that upload from committing.
    if (decoded_source->image_write_locked)
      return finish_vpp(VA_STATUS_ERROR_HW_BUSY);
    const uint64_t timestamp = decoded_source->expected_timestamp;

    // The output is still owned by its previous operation. Replacing it or
    // signaling its fence with unrelated fallback pixels breaks identity.
    if (target_owner->vpp_writers != 0 || target_owner->export_waiters != 0)
      return finish_vpp(VA_STATUS_ERROR_HW_BUSY);
    // VPP releases the driver mutex between BeginPicture and EndPicture.
    // An experimental decoder may have reserved, committed, or exported this
    // backing in that interval.  Cancel only the stale VPP transaction: the
    // newer decode presentation and its external-reader guard stay intact.
    if (ExperimentalPresentationBinding(driver, target_owner.get()) ||
        target_owner->experimental_write_lease ||
        target_owner->experimental_read_export_armed ||
        target_owner->experimental_presentation)
      return finish_vpp(VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
    // A decode can take this target between VPP BeginPicture and EndPicture.
    // Recheck its picture immediately before acquiring write ownership.
    if (!PromoteDecodedPicture(target_owner->decode_picture.lock()))
      return finish_vpp(VA_STATUS_ERROR_OPERATION_FAILED);
    RevokeDecodeOwner(driver, target_owner.get());

    const uint64_t sequence = driver->next_vpp_sequence++;
    target_owner->latest_vpp_sequence = sequence;

    // If CrystalHD already emitted the immutable source, complete the copy
    // while vaEndPicture still owns the destination. No asynchronous fence is
    // needed in this fast path.
    if (decoded_source->ready || decoded_source->failed) {
      VAStatus status = decoded_source->failed
                            ? VA_STATUS_ERROR_DECODING_ERROR
                            : ProcessVpp(&driver->vpp_scaler,
                                         &driver->vpp_argb_staging,
                                         decoded_source.get(),
                                         target_surface.get(),
                                         decode_context->vpp_source_region,
                                         decode_context->vpp_output_region,
                                         false,
                                         decode_context->vpp_deinterlace, false,
                                         decode_context->vpp_color_range);
      if (status == VA_STATUS_SUCCESS) {
        InheritTraceIdentity(target_surface.get(), *decoded_source);
        if (target_owner != target_surface)
          InheritTraceIdentity(target_owner.get(), *decoded_source);
      }
      SetSurfaceState(driver, target_surface.get(),
                      status == VA_STATUS_SUCCESS,
                      status != VA_STATUS_SUCCESS);
      crystalhd_vaapi_trace::Fields fields =
          TraceFields(nullptr, target_surface.get(), timestamp);
      fields.operation = sequence;
      fields.outcome = status;
      driver->trace.Emit(status == VA_STATUS_SUCCESS
                             ? crystalhd_vaapi_trace::Event::VppCommit
                             : crystalhd_vaapi_trace::Event::VppCancel,
                         fields);
      Debug("completed immediate VPP sequence=%llu source_timestamp=%llu "
            "target=%p status=%d",
            static_cast<unsigned long long>(sequence),
            static_cast<unsigned long long>(timestamp),
            static_cast<void *>(target_surface.get()), status);
      return finish_vpp(status);
    }

    // An internal, never-exported target has no external GPU readers. Its VA
    // consumers synchronize before reading; do not require privileged sw_sync
    // access for that path. Export must not expose it while this write is
    // pending. Imported or previously exported targets still need a fence:
    // Chromium can queue them without synchronizing the processed VA surface.
    const bool private_target = target_surface == target_owner &&
                                !target_owner->layout.valid;
    PendingVpp pending;
    pending.decoder = source_decoder;
    pending.source = decoded_source;
    pending.target = target_surface;
    pending.target_owner = target_owner;
    pending.source_region = decode_context->vpp_source_region;
    pending.output_region = decode_context->vpp_output_region;
    pending.deinterlace = decode_context->vpp_deinterlace;
    pending.color_range = decode_context->vpp_color_range;
    pending.source_timestamp = timestamp;
    pending.sequence = sequence;
    pending.decoder_generation = source_decoder
                                     ? source_decoder->generation
                                     : 0;
    // The worker cannot see this entry until the driver lock is released.
    // Complete the fallible deque allocation before acquiring a CPU/fence
    // lease or publishing reader/writer ownership.
    try {
      driver->pending_vpp.push_back(std::move(pending));
    } catch (const std::bad_alloc &) {
      return finish_vpp(VA_STATUS_ERROR_ALLOCATION_FAILED);
    }
    bool synchronous_fallback = false;
    const int write_timeline = private_target
        ? -1 : BeginFencedWrite(target_surface.get(), CpuCacheFlush(),
                                &synchronous_fallback);
    if (!private_target && write_timeline < 0 && !synchronous_fallback) {
      driver->pending_vpp.pop_back();
      Debug("could not fence asynchronous VPP target=%p",
            static_cast<void *>(target_surface.get()));
      return finish_vpp(VA_STATUS_ERROR_OPERATION_FAILED);
    }
    driver->pending_vpp.back().write_timeline = write_timeline;
    ++decoded_source->vpp_readers;
    ++target_owner->vpp_writers;
    if (!private_target && write_timeline < 0) {
      // The target has external consumers, but no fence could be published.
      // Retain normal ownership while waiting and finish all CPU writes before
      // returning EndPicture; never expose an unfenced asynchronous writer.
      driver->pending_vpp.back().worker_active = true;
      const PendingVpp operation = driver->pending_vpp.back();
      driver->condition.notify_all();
      const auto canceled = [&] {
        const auto processor = driver->contexts.find(context_id);
        return driver->stopping || PendingVppCanceled(operation) ||
            decode_context->closing || decode_context->retired ||
            processor == driver->contexts.end() ||
            processor->second != decode_context || !same_request() ||
            target_owner->picture_transaction != request_transaction ||
            decoded_source->expected_timestamp != operation.source_timestamp ||
            std::none_of(driver->pending_vpp.begin(), driver->pending_vpp.end(),
                         [&](const PendingVpp &queued) {
                           return queued.sequence == operation.sequence;
                         });
      };
      VAStatus status = VA_STATUS_ERROR_OPERATION_FAILED;
      std::shared_ptr<Surface> first_field_view;
      try {
        if (!canceled())
          status = SyncDecodeSurface(driver, operation.source, &lock,
                                      kDecodeTimeoutNs,
                                      [&canceled] { return canceled(); },
                                      [&](bool *ready) {
                                        const VAStatus prepared = PrepareFirstFieldBob(
                                            operation, &first_field_view);
                                        *ready = first_field_view != nullptr;
                                        return prepared;
                                      }, &operation.deinterlace);
      } catch (const std::bad_alloc &) {
        status = VA_STATUS_ERROR_ALLOCATION_FAILED;
      }
      // Success alone does not prove this request still owns its destination:
      // source completion and cancellation can occur in the same unlocked turn.
      if (canceled())
        status = VA_STATUS_ERROR_OPERATION_FAILED;
      if (status == VA_STATUS_SUCCESS)
        status = ProcessVpp(&driver->vpp_scaler, &driver->vpp_argb_staging,
                            first_field_view ? first_field_view.get() : operation.source.get(),
                            operation.target.get(),
                            operation.source_region, operation.output_region,
                            false, operation.deinterlace, first_field_view != nullptr,
                            operation.color_range);
      ReleasePendingVpp(driver, operation.sequence, status,
                        status == VA_STATUS_SUCCESS);
      return finish_vpp(status);
    }
    driver->condition.notify_all();
    Debug("queued VPP sequence=%llu source_timestamp=%llu target=%p timeline=%d",
          static_cast<unsigned long long>(sequence),
          static_cast<unsigned long long>(timestamp),
          static_cast<void *>(target_surface.get()), write_timeline);
    return finish_vpp(VA_STATUS_SUCCESS);
  }
  auto config = driver->configs.find(decode_context->config);
  if (config == driver->configs.end())
    return VA_STATUS_ERROR_INVALID_CONFIG;
  const VAProfile profile = config->second.profile;
  VAStatus status = SubmitPicture(driver, decode_context.get(), profile);
  // Every decode profile consumes End even for malformed/unsupported input or
  // allocation failure. Recoverable Render errors remain replaceable before
  // End; successful queued/backpressured submissions are not rejections.
  if (status != VA_STATUS_SUCCESS)
    MarkRejectedPictureTransaction(driver, decode_context.get());
  if (status != VA_STATUS_SUCCESS)
    Debug("EndPicture rejected status=%d profile=%d target=%u", status,
          profile, decode_context->target);
  decode_context->target = VA_INVALID_SURFACE;
  decode_context->transaction_target.reset();
  decode_context->have_picture = false;
  decode_context->h264_picture_invalid = false;
  decode_context->h264_framed_slice_bytes = 0;
  decode_context->slices.clear();
  decode_context->slice_data.clear();
  decode_context->mpeg2_picture = DecodeContext::Mpeg2Picture();
  decode_context->mpeg4_picture = DecodeContext::Mpeg4Picture();
  decode_context->vc1_picture = DecodeContext::Vc1Picture();
  if (status == VA_STATUS_SUCCESS &&
      NeedsDecodeControlWake(*decode_context))
    driver->NotifyDecodeWorker();
  return status;
}

static VAStatus SyncSurface2(VADriverContextP context, VASurfaceID surface_id,
                             uint64_t timeout_ns) {
  Driver *driver = GetDriver(context);
  std::unique_lock<std::mutex> lock(driver->mutex);
  auto surface = driver->surfaces.find(surface_id);
  if (surface == driver->surfaces.end() ||
      !HasLiveBackingOwner(driver, surface->second.get()))
    return VA_STATUS_ERROR_INVALID_SURFACE;
  // SyncDecodeSurface and the VPP wait loop may release the driver mutex.
  // Retain both objects instead of carrying an unordered_map iterator across
  // that interval; another thread may destroy and erase the VA surface.
  const std::shared_ptr<Surface> requested_surface = surface->second;
  std::shared_ptr<Surface> synchronized_surface = requested_surface;
  if (requested_surface->backing_owner != VA_INVALID_SURFACE) {
    auto owner = driver->surfaces.find(requested_surface->backing_owner);
    if (owner != driver->surfaces.end())
      synchronized_surface = owner->second;
  }
  if (synchronized_surface->image_write_locked)
    return VA_STATUS_ERROR_SURFACE_BUSY;
  const bool trace_enabled = driver->trace.enabled();
  crystalhd_vaapi_trace::Fields trace_fields;
  std::chrono::steady_clock::time_point trace_start;
  if (trace_enabled) {
    trace_fields = TraceRequestedOwnerFields(requested_surface.get(),
                                             synchronized_surface.get());
    trace_fields.operation = NextTraceCall(driver);
    trace_start = std::chrono::steady_clock::now();
    driver->trace.Emit(crystalhd_vaapi_trace::Event::SyncEnter, trace_fields);
  }
  const auto finish = [&](VAStatus status) {
    if (trace_enabled) {
      const crystalhd_vaapi_trace::Fields current =
          TraceRequestedOwnerFields(requested_surface.get(),
                                    synchronized_surface.get());
      if (status == VA_STATUS_SUCCESS &&
          SameTraceBinding(trace_fields, current)) {
        trace_fields.payload_token = current.payload_token;
        trace_fields.payload_decode_identity =
            current.payload_decode_identity;
        trace_fields.payload_submission_ordinal =
            current.payload_submission_ordinal;
        trace_fields.submission_drift = current.submission_drift;
      }
      trace_fields.duration_ns = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - trace_start).count());
      trace_fields.outcome = status;
      driver->trace.Emit(crystalhd_vaapi_trace::Event::SyncExit, trace_fields);
    }
    return status;
  };

  if (PublicReadRejected(synchronized_surface.get()))
    return finish(VA_STATUS_ERROR_DECODING_ERROR);
  if (synchronized_surface->opaque_reference) {
    const auto picture = synchronized_surface->opaque_picture;
    if (!picture || picture->decode_identity != synchronized_surface->decode_identity ||
        picture->expected_timestamp != synchronized_surface->expected_timestamp)
      return finish(VA_STATUS_ERROR_INVALID_SURFACE);
    // Completion of an opaque reference is not permission to map its planes.
    // Wait for the independent scaled picture, canceling if this public
    // reference is destroyed/rebound while the driver mutex is released.
    const auto canceled = [&] {
      return synchronized_surface->destroyed ||
          synchronized_surface->opaque_picture != picture ||
          synchronized_surface->decode_identity != picture->decode_identity ||
          PublicReadRejected(synchronized_surface.get());
    };
    return finish(SyncDecodeSurface(driver, picture, &lock, timeout_ns, canceled));
  }
  if (ExperimentalWriteReservationPending(synchronized_surface.get()))
    return finish(VA_STATUS_ERROR_SURFACE_BUSY);

  auto targets_surface = [&](const PendingVpp &pending) {
    Surface *requested_owner = BackingOwner(driver, synchronized_surface.get());
    Surface *pending_owner = BackingOwner(driver, pending.target.get());
    return pending.target == synchronized_surface ||
           pending_owner == requested_owner;
  };
  const bool had_pending = std::any_of(driver->pending_vpp.begin(),
                                       driver->pending_vpp.end(),
                                       targets_surface);
  if (had_pending) {
    const auto start = std::chrono::steady_clock::now();
    const bool infinite = timeout_ns == VA_TIMEOUT_INFINITE;
    for (;;) {
      if (driver->stopping)
        return finish(VA_STATUS_ERROR_OPERATION_FAILED);

      // Dispose of an older operation before selecting the current owner.
      // No canceled operation is ever allowed to touch the shared DMA-BUF.
      auto canceled = std::find_if(
          driver->pending_vpp.begin(), driver->pending_vpp.end(),
          [&](const PendingVpp &pending) {
            return targets_surface(pending) && PendingVppCanceled(pending);
          });
      if (canceled != driver->pending_vpp.end()) {
        const uint64_t sequence = canceled->sequence;
        ReleasePendingVpp(driver, sequence, VA_STATUS_SUCCESS, false);
        continue;
      }

      auto pending = std::find_if(driver->pending_vpp.begin(),
                                  driver->pending_vpp.end(),
                                  targets_surface);
      if (pending == driver->pending_vpp.end()) {
        return finish(synchronized_surface->ready
                          ? VA_STATUS_SUCCESS
                          : VA_STATUS_ERROR_DECODING_ERROR);
      }

      if (infinite) {
        driver->condition.wait(lock);
        continue;
      }
      const uint64_t elapsed =
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - start)
              .count();
      if (elapsed >= timeout_ns)
        return finish(VA_STATUS_ERROR_TIMEDOUT);
      driver->condition.wait_for(lock,
                                 std::chrono::nanoseconds(timeout_ns - elapsed));
    }
  }
  if (ExperimentalPresentationReadable(driver, synchronized_surface.get()))
    return finish(VA_STATUS_SUCCESS);
  if (timeout_ns != 0 &&
      synchronized_surface->backing_owner == VA_INVALID_SURFACE) {
    bool committed = false;
    const VAStatus presentation =
        TryExperimentalPresentation(driver, synchronized_surface, &committed);
    if (presentation != VA_STATUS_SUCCESS)
      return finish(presentation);
    if (committed)
      return finish(VA_STATUS_SUCCESS);
  }
  const VAStatus status =
      SyncDecodeSurface(driver, synchronized_surface, &lock, timeout_ns);
  if (status == VA_STATUS_SUCCESS)
    ConsumeExperimentalExact(driver, synchronized_surface);
  return finish(status);
}

static VAStatus SyncSurface(VADriverContextP context, VASurfaceID surface_id) {
  return SyncSurface2(context, surface_id, VA_TIMEOUT_INFINITE);
}

// VLC's export path does not call vaSyncSurface before using the descriptor.
// Unlike a GPU decoder, our CPU copy has no implicit fence on a plain decode
// surface. Complete the selected picture before read/default export rather
// than hand out an fd containing the preceding picture. Write-only export is
// deliberately not routed here, so clients can still allocate VPP targets.
static VAStatus SynchronizeExportRead(
    VADriverContextP context, VASurfaceID surface_id,
    std::unique_lock<std::mutex> *lock) {
  Driver *driver = GetDriver(context);
  auto found = driver->surfaces.find(surface_id);
  if (found == driver->surfaces.end() ||
      !HasLiveBackingOwner(driver, found->second.get()))
    return VA_STATUS_ERROR_INVALID_SURFACE;
  const std::shared_ptr<Surface> surface = found->second;
  const std::shared_ptr<Surface> owner =
      surface->backing_owner == VA_INVALID_SURFACE
          ? surface : driver->surfaces.at(surface->backing_owner);
  if (owner->image_write_locked)
    return VA_STATUS_ERROR_SURFACE_BUSY;
  if (owner->failed || PublicReadRejected(owner.get()))
    return VA_STATUS_ERROR_DECODING_ERROR;
  if ((owner->ready || ExperimentalPresentationReadable(driver, owner.get())) &&
      owner->vpp_writers == 0)
    return VA_STATUS_SUCCESS;
  if (owner->export_waiters == UINT_MAX)
    return VA_STATUS_ERROR_SURFACE_BUSY;
  const uint64_t timestamp = owner->expected_timestamp;
  const uint64_t sequence = owner->latest_vpp_sequence;
  // A queued VPP assigns its result timestamp only when the copy finishes.
  // Its already-assigned operation sequence identifies that pending picture.
  const bool pending_vpp = owner->vpp_writers != 0;
  ++owner->export_waiters;
  lock->unlock();
  const VAStatus status = SyncSurface2(context, surface_id, kDecodeTimeoutNs);
  lock->lock();
  --owner->export_waiters;
  // Never retain unordered_map iterators across a synchronization wait.
  found = driver->surfaces.find(surface_id);
  if (found == driver->surfaces.end() || found->second != surface ||
      !HasLiveBackingOwner(driver, surface.get()) ||
      BackingOwner(driver, surface.get()) != owner.get())
    return VA_STATUS_ERROR_INVALID_SURFACE;
  if ((!pending_vpp && owner->expected_timestamp != timestamp) ||
      owner->latest_vpp_sequence != sequence)
    return VA_STATUS_ERROR_OPERATION_FAILED;
  if (status != VA_STATUS_SUCCESS)
    return status;
  if (owner->failed)
    return VA_STATUS_ERROR_DECODING_ERROR;
  return (owner->ready || ExperimentalPresentationReadable(driver, owner.get())) &&
             owner->vpp_writers == 0
             ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_SURFACE_BUSY;
}

static VAStatus QuerySurfaceStatus(VADriverContextP context,
                                   VASurfaceID surface_id,
                                   VASurfaceStatus *status) {
  Driver *driver = GetDriver(context);
  std::lock_guard<std::mutex> lock(driver->mutex);
  auto surface = driver->surfaces.find(surface_id);
  if (surface == driver->surfaces.end() ||
      !HasLiveBackingOwner(driver, surface->second.get()))
    return VA_STATUS_ERROR_INVALID_SURFACE;
  if (status == nullptr)
    return VA_STATUS_ERROR_INVALID_PARAMETER;
  Surface *owner = BackingOwner(driver, surface->second.get());
  if (owner->failed || PublicReadRejected(owner))
    return VA_STATUS_ERROR_DECODING_ERROR;
  if (owner->opaque_reference) {
    const auto &picture = owner->opaque_picture;
    if (!picture || picture->decode_identity != owner->decode_identity ||
        picture->expected_timestamp != owner->expected_timestamp)
      return VA_STATUS_ERROR_INVALID_SURFACE;
    if (picture->failed)
      return VA_STATUS_ERROR_DECODING_ERROR;
    if (!picture->ready) {
      const auto decode = driver->contexts.find(owner->opaque_context);
      if (decode == driver->contexts.end() ||
          decode->second->generation != picture->trace_generation)
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    *status = picture->ready ? VASurfaceReady : VASurfaceRendering;
    return VA_STATUS_SUCCESS;
  }
  if (ExperimentalWriteReservationPending(owner)) {
    *status = VASurfaceRendering;
    return VA_STATUS_SUCCESS;
  }
  *status = owner->ready || ExperimentalPresentationReadable(driver, owner)
                ? VASurfaceReady : VASurfaceRendering;
  return VA_STATUS_SUCCESS;
}

static VAStatus QueryImageFormats(VADriverContextP, VAImageFormat *formats,
                                  int *count) {
  if (formats == nullptr || count == nullptr)
    return VA_STATUS_ERROR_INVALID_PARAMETER;
  formats[0] = {};
  formats[0].fourcc = VA_FOURCC_NV12;
  formats[0].byte_order = VA_LSB_FIRST;
  formats[0].bits_per_pixel = 12;
  *count = 1;
  return VA_STATUS_SUCCESS;
}

static VAStatus CreateImageLocked(Driver *driver, const VAImageFormat &format,
                                  int width, int height, VAImage *va_image) {
  if (format.fourcc != VA_FOURCC_NV12 || width <= 0 || height <= 0 ||
      static_cast<unsigned int>(width) > kMaxWidth ||
      static_cast<unsigned int>(height) > kMaxHeight ||
      va_image == nullptr)
    return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;

  // NV12 stores a complete interleaved UV pair for every two luma columns,
  // and a chroma row for an odd final luma row as well.
  const unsigned int uv_pitch = Align(width, 2);
  const unsigned int uv_offset = static_cast<unsigned int>(width) * height;
  const unsigned int data_size = uv_offset + uv_pitch * ((height + 1) / 2);
  Buffer buffer;
  buffer.context = VA_INVALID_ID;
  buffer.type = VAImageBufferType;
  buffer.element_size = 1;
  buffer.elements = data_size;
  buffer.data.resize(buffer.elements);
  VABufferID buffer_id = driver->next_buffer++;
  driver->buffers[buffer_id] = std::move(buffer);

  Image image;
  image.va.image_id = driver->next_image++;
  image.va.format = format;
  image.va.buf = buffer_id;
  image.va.width = width;
  image.va.height = height;
  image.va.data_size = data_size;
  image.va.num_planes = 2;
  image.va.pitches[0] = width;
  image.va.pitches[1] = uv_pitch;
  image.va.offsets[0] = 0;
  image.va.offsets[1] = uv_offset;
  *va_image = image.va;
  driver->images[image.va.image_id] = image;
  return VA_STATUS_SUCCESS;
}

static VAStatus CreateImage(VADriverContextP context, VAImageFormat *format,
                            int width, int height, VAImage *image) {
  if (format == nullptr)
    return VA_STATUS_ERROR_INVALID_PARAMETER;
  Driver *driver = GetDriver(context);
  std::lock_guard<std::mutex> lock(driver->mutex);
  return CreateImageLocked(driver, *format, width, height, image);
}

static VAStatus DestroyImage(VADriverContextP context, VAImageID image_id) {
  Driver *driver = GetDriver(context);
  std::lock_guard<std::mutex> lock(driver->mutex);
  auto image = driver->images.find(image_id);
  if (image == driver->images.end())
    return VA_STATUS_ERROR_INVALID_IMAGE;
  auto buffer = driver->buffers.find(image->second.va.buf);
  if (buffer != driver->buffers.end() &&
      (BufferBorrowed(buffer->second) || buffer->second.map_count != 0))
    return VA_STATUS_ERROR_SURFACE_BUSY;
  driver->buffers.erase(image->second.va.buf);
  driver->images.erase(image);
  return VA_STATUS_SUCCESS;
}

static bool ImageBufferFits(const Image &image, const Buffer &buffer) {
  if (buffer.data_failed || image.va.format.fourcc != VA_FOURCC_NV12 ||
      image.va.num_planes != 2 ||
      image.va.width == 0 || image.va.height == 0)
    return false;
  for (unsigned int plane = 0; plane < 2; ++plane) {
    const size_t rows = plane == 0 ? image.va.height : (image.va.height + 1) / 2;
    const size_t row_bytes = plane == 0 ? image.va.width : Align(image.va.width, 2);
    const size_t offset = image.va.offsets[plane];
    const size_t pitch = image.va.pitches[plane];
    // Buffers are separately public objects and may have been resized after
    // image creation. Check their actual size without overflowing arithmetic.
    if (pitch < row_bytes || offset > buffer.data.size() ||
        row_bytes > buffer.data.size() - offset ||
        rows - 1 > (buffer.data.size() - offset - row_bytes) / pitch)
      return false;
  }
  return true;
}

static bool ImageRectangleFits(const Surface &surface, const Image &image,
                               unsigned int width, unsigned int height) {
  return width != 0 && height != 0 && width <= surface.width &&
         height <= surface.height && width <= image.va.width &&
         height <= image.va.height;
}

static VAStatus CopySurfaceToImage(Driver *driver, Surface *surface,
                                   Image *image, unsigned int width,
                                   unsigned int height) {
  if (!HasLiveBackingOwner(driver, surface))
    return VA_STATUS_ERROR_INVALID_SURFACE;
  if (BackingOwner(driver, surface)->opaque_reference)
    return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
  if (surface->fourcc != VA_FOURCC_NV12)
    return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
  auto buffer = driver->buffers.find(image->va.buf);
  if (buffer == driver->buffers.end())
    return VA_STATUS_ERROR_INVALID_BUFFER;
  if (BufferBorrowed(buffer->second) || buffer->second.map_count != 0)
    return VA_STATUS_ERROR_SURFACE_BUSY;
  if (!ImageRectangleFits(*surface, *image, width, height))
    return VA_STATUS_ERROR_INVALID_PARAMETER;
  if (!ImageBufferFits(*image, buffer->second))
    return VA_STATUS_ERROR_INVALID_BUFFER;
  Surface *owner = BackingOwner(driver, surface);
  if (owner->failed)
    return VA_STATUS_ERROR_DECODING_ERROR;
  // Never wait under the driver lock on our own queued asynchronous fence.
  if (!owner->ready || owner->vpp_writers != 0)
    return VA_STATUS_ERROR_SURFACE_BUSY;
  if (!surface->BeginCpuRead())
    return VA_STATUS_ERROR_OPERATION_FAILED;
  uint8_t *destination_y = buffer->second.data.data() + image->va.offsets[0];
  uint8_t *destination_uv = buffer->second.data.data() + image->va.offsets[1];
  for (unsigned int y = 0; y < height; ++y) {
    memcpy(destination_y + static_cast<size_t>(y) * image->va.pitches[0],
           surface->planes[0] + static_cast<size_t>(y) * surface->pitch[0],
           width);
  }
  for (unsigned int y = 0; y < (height + 1) / 2; ++y) {
    memcpy(destination_uv + static_cast<size_t>(y) * image->va.pitches[1],
           surface->planes[1] + static_cast<size_t>(y) * surface->pitch[1],
           Align(width, 2));
  }
  return surface->EndCpuRead() ? VA_STATUS_SUCCESS
                               : VA_STATUS_ERROR_OPERATION_FAILED;
}

static VAStatus DeriveImage(VADriverContextP context, VASurfaceID surface_id,
                            VAImage *va_image) {
  if (va_image == nullptr)
    return VA_STATUS_ERROR_INVALID_PARAMETER;
  Driver *driver = GetDriver(context);
  std::unique_lock<std::mutex> lock(driver->mutex);
  const auto initial_surface = driver->surfaces.find(surface_id);
  if (initial_surface == driver->surfaces.end() ||
      !HasLiveBackingOwner(driver, initial_surface->second.get()))
    return VA_STATUS_ERROR_INVALID_SURFACE;
  Surface *initial_owner =
      BackingOwner(driver, initial_surface->second.get());
  if (initial_owner->opaque_reference)
    return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
  if (ExperimentalPresentationBinding(driver, initial_owner) ||
      initial_owner->experimental_write_lease ||
      initial_owner->experimental_read_export_armed ||
      initial_owner->experimental_presentation)
    return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
  const VAStatus synchronized = SynchronizeExportRead(context, surface_id, &lock);
  if (synchronized != VA_STATUS_SUCCESS)
    return synchronized;
  auto surface = driver->surfaces.find(surface_id);
  if (surface == driver->surfaces.end() ||
      !HasLiveBackingOwner(driver, surface->second.get()))
    return VA_STATUS_ERROR_INVALID_SURFACE;
  if (BackingOwner(driver, surface->second.get())->opaque_reference)
    return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
  VAImageFormat format = {};
  format.fourcc = VA_FOURCC_NV12;
  format.byte_order = VA_LSB_FIRST;
  format.bits_per_pixel = 12;
  VAImage snapshot = {};
  VAStatus status = CreateImageLocked(driver, format, surface->second->width,
                                      surface->second->height, &snapshot);
  if (status != VA_STATUS_SUCCESS)
    return status;
  status = CopySurfaceToImage(driver, surface->second.get(),
                              &driver->images.at(snapshot.image_id),
                              surface->second->width, surface->second->height);
  if (status != VA_STATUS_SUCCESS) {
    driver->buffers.erase(snapshot.buf);
    driver->images.erase(snapshot.image_id);
    return status;
  }
  Buffer &derived = driver->buffers.at(snapshot.buf);
  derived.derived_image = snapshot.image_id;
  Surface *owner = BackingOwner(driver, surface->second.get());
  if (owner->expected_timestamp == 0 && owner->frame_timestamp == 0 &&
      ImageWriteStatus(driver, surface->second.get()) == VA_STATUS_SUCCESS) {
    derived.derived_source = surface->second;
    derived.derived_owner = owner == surface->second.get()
        ? surface->second : driver->surfaces.at(surface->second->backing_owner);
  }
  *va_image = snapshot;
  owner->external_accessed = true;
  return VA_STATUS_SUCCESS;
}

static VAStatus GetImage(VADriverContextP context, VASurfaceID surface_id,
                         int x, int y, unsigned int width, unsigned int height,
                         VAImageID image_id) {
  if (x != 0 || y != 0)
    return VA_STATUS_ERROR_INVALID_PARAMETER;
  Driver *driver = GetDriver(context);
  std::lock_guard<std::mutex> lock(driver->mutex);
  auto surface = driver->surfaces.find(surface_id);
  auto image = driver->images.find(image_id);
  if (surface == driver->surfaces.end())
    return VA_STATUS_ERROR_INVALID_SURFACE;
  if (image == driver->images.end())
    return VA_STATUS_ERROR_INVALID_IMAGE;
  Surface *owner = BackingOwner(driver, surface->second.get());
  if (owner->opaque_reference)
    return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
  if (PublicReadRejected(owner))
    return VA_STATUS_ERROR_DECODING_ERROR;
  if (ExperimentalPresentationBinding(driver, owner) ||
      owner->experimental_write_lease ||
      owner->experimental_read_export_armed ||
      owner->experimental_presentation)
    return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
  return CopySurfaceToImage(driver, surface->second.get(), &image->second,
                            width, height);
}

static VAStatus ImageWriteStatus(Driver *driver, Surface *surface) {
  if (!HasLiveBackingOwner(driver, surface))
    return VA_STATUS_ERROR_INVALID_SURFACE;
  Surface *owner = BackingOwner(driver, surface);
  if (owner->opaque_reference)
    return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
  if (ExperimentalPresentationBinding(driver, owner) ||
      owner->experimental_write_lease || owner->experimental_read_export_armed ||
      owner->experimental_presentation)
    return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
  if (owner->vpp_writers != 0 || owner->vpp_readers != 0 ||
      owner->export_waiters != 0 || (!owner->ready && !owner->failed))
    return VA_STATUS_ERROR_SURFACE_BUSY;
  for (const auto &entry : driver->contexts)
    for (const auto &candidate : driver->surfaces)
      if (BackingOwner(driver, candidate.second.get()) == owner &&
          entry.second->surface_timestamps.count(candidate.second.get()) != 0)
        return VA_STATUS_ERROR_SURFACE_BUSY;
  return VA_STATUS_SUCCESS;
}

static VAStatus PutImageLocked(Driver *driver, VASurfaceID surface_id,
                         VAImageID image_id, int source_x, int source_y,
                         unsigned int source_width, unsigned int source_height,
                         int destination_x, int destination_y,
                         unsigned int destination_width,
                         unsigned int destination_height) {
  if (source_x != 0 || source_y != 0 || destination_x != 0 ||
      destination_y != 0 || source_width != destination_width ||
      source_height != destination_height)
    return VA_STATUS_ERROR_UNIMPLEMENTED;
  auto surface = driver->surfaces.find(surface_id);
  auto image = driver->images.find(image_id);
  if (surface == driver->surfaces.end() ||
      !HasLiveBackingOwner(driver, surface->second.get()))
    return VA_STATUS_ERROR_INVALID_SURFACE;
  if (image == driver->images.end())
    return VA_STATUS_ERROR_INVALID_IMAGE;
  Surface *owner = BackingOwner(driver, surface->second.get());
  if (owner->opaque_reference)
    return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
  if (ExperimentalPresentationBinding(driver, owner) ||
      owner->experimental_write_lease ||
      owner->experimental_read_export_armed ||
      owner->experimental_presentation)
    return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
  if (surface->second->fourcc != VA_FOURCC_NV12)
    return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
  auto buffer = driver->buffers.find(image->second.va.buf);
  if (buffer == driver->buffers.end())
    return VA_STATUS_ERROR_INVALID_BUFFER;
  if (BufferBorrowed(buffer->second) || buffer->second.map_count != 0)
    return VA_STATUS_ERROR_SURFACE_BUSY;
  if (!ImageRectangleFits(*surface->second, image->second,
                          source_width, source_height))
    return VA_STATUS_ERROR_INVALID_PARAMETER;
  if (!ImageBufferFits(image->second, buffer->second))
    return VA_STATUS_ERROR_INVALID_BUFFER;
  const VAStatus writable = ImageWriteStatus(driver, surface->second.get());
  if (writable != VA_STATUS_SUCCESS)
    return writable;
  const uint8_t *source_y_plane = buffer->second.data.data() +
                                  image->second.va.offsets[0];
  const uint8_t *source_uv_plane = buffer->second.data.data() +
                                   image->second.va.offsets[1];
  owner->completed_decode_picture = {};
  owner->picture_transaction = NextDecodeIdentity(driver);
  EndTraceBinding(driver, owner);
  if (surface->second.get() != owner)
    ClearTraceIdentity(surface->second.get());
  if (!surface->second->BeginCpuWrite()) {
    SetSurfaceState(driver, surface->second.get(), false, true);
    return VA_STATUS_ERROR_OPERATION_FAILED;
  }
  for (unsigned int row = 0; row < source_height; ++row) {
    memcpy(surface->second->planes[0] +
               static_cast<size_t>(row) * surface->second->pitch[0],
           source_y_plane +
               static_cast<size_t>(row) * image->second.va.pitches[0],
           source_width);
  }
  for (unsigned int row = 0; row < (source_height + 1) / 2; ++row) {
    memcpy(surface->second->planes[1] +
               static_cast<size_t>(row) * surface->second->pitch[1],
           source_uv_plane +
               static_cast<size_t>(row) * image->second.va.pitches[1],
           Align(source_width, 2));
  }
  if (!surface->second->EndCpuWrite()) {
    SetSurfaceState(driver, surface->second.get(), false, true);
    return VA_STATUS_ERROR_OPERATION_FAILED;
  }
  surface->second->expected_timestamp = 0;
  surface->second->frame_timestamp = 0;
  owner->expected_timestamp = 0;
  owner->frame_timestamp = 0;
  SetSurfaceState(driver, surface->second.get(), true, false);
  return VA_STATUS_SUCCESS;
}

static VAStatus PutImage(VADriverContextP context, VASurfaceID surface_id,
                         VAImageID image_id, int source_x, int source_y,
                         unsigned int source_width, unsigned int source_height,
                         int destination_x, int destination_y,
                         unsigned int destination_width,
                         unsigned int destination_height) {
  Driver *driver = GetDriver(context);
  std::lock_guard<std::mutex> lock(driver->mutex);
  return PutImageLocked(driver, surface_id, image_id, source_x, source_y,
                        source_width, source_height, destination_x, destination_y,
                        destination_width, destination_height);
}

static bool DerivedSourceLive(Driver *driver, const Buffer &buffer) {
  const auto source = driver->surfaces.find(buffer.derived_source->id);
  return source != driver->surfaces.end() && source->second == buffer.derived_source &&
      HasLiveBackingOwner(driver, source->second.get()) &&
      BackingOwner(driver, source->second.get()) == buffer.derived_owner.get();
}

static VAStatus PrepareDerivedWrite(Driver *driver, Buffer &buffer, bool refresh) {
  if (buffer.derived_write_active)
    return VA_STATUS_SUCCESS;
  if (!DerivedSourceLive(driver, buffer))
    return VA_STATUS_ERROR_INVALID_SURFACE;
  Surface *owner = buffer.derived_owner.get();
  if (owner->expected_timestamp != 0 || owner->frame_timestamp != 0)
    return VA_STATUS_ERROR_SURFACE_BUSY;
  VAStatus status = ImageWriteStatus(driver, buffer.derived_source.get());
  if (status != VA_STATUS_SUCCESS)
    return status;
  auto image = driver->images.find(buffer.derived_image);
  if (image == driver->images.end() || !ImageBufferFits(image->second, buffer))
    return VA_STATUS_ERROR_INVALID_IMAGE;
  if (refresh) {
    status = CopySurfaceToImage(driver, buffer.derived_source.get(), &image->second,
                                image->second.va.width, image->second.va.height);
    if (status != VA_STATUS_SUCCESS)
      return status;
  }
  ++owner->vpp_writers;
  owner->completed_decode_picture = {};
  owner->picture_transaction = NextDecodeIdentity(driver);
  owner->image_write_locked = true;
  buffer.derived_was_ready = owner->ready;
  owner->ready = false;
  buffer.derived_write_active = true;
  return VA_STATUS_SUCCESS;
}

static VAStatus FinishDerivedWrite(Driver *driver, Buffer &buffer, bool success) {
  if (!buffer.derived_write_active)
    return success ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_OPERATION_FAILED;
  Surface *owner = buffer.derived_owner.get();
  --owner->vpp_writers;
  owner->image_write_locked = false;
  buffer.derived_write_active = false;
  VAStatus status = VA_STATUS_ERROR_INVALID_SURFACE;
  if (DerivedSourceLive(driver, buffer)) {
    owner->ready = true;
    if (success)
      status = PutImageLocked(driver, buffer.derived_source->id, buffer.derived_image,
                              0, 0, buffer.derived_source->width,
                              buffer.derived_source->height, 0, 0,
                              buffer.derived_source->width, buffer.derived_source->height);
    else
      status = VA_STATUS_ERROR_OPERATION_FAILED;
    if (status != VA_STATUS_SUCCESS)
      SetSurfaceState(driver, buffer.derived_source.get(), false, true);
  }
  if (status != VA_STATUS_SUCCESS) {
    // The source VA ID may have disappeared while its backing owner remains
    // alive. Never leave that owner indefinitely pending or expose stale bytes.
    owner->ready = false;
    owner->failed = true;
    buffer.data_failed = true;
  }
  driver->condition.notify_all();
  return status;
}

static void CancelDerivedWrite(Buffer &buffer) {
  if (!buffer.derived_write_active)
    return;
  Surface *owner = buffer.derived_owner.get();
  --owner->vpp_writers;
  owner->image_write_locked = false;
  owner->ready = buffer.derived_was_ready;
  buffer.derived_write_active = false;
}

static std::shared_ptr<ImageExportBacking> AllocateImageExportBacking(
    Driver *driver, size_t bytes) {
  constexpr size_t maximum_image_bytes =
      static_cast<size_t>(kMaxWidth) * kMaxHeight * 3 / 2;
  if (driver->gbm == nullptr || bytes == 0 || bytes > maximum_image_bytes)
    return {};
  std::shared_ptr<ImageExportBacking> backing(new (std::nothrow) ImageExportBacking);
  if (!backing)
    return {};
  // Treat this allocation as a linear byte store, not as NV12 GBM planes.
  // In particular, do not change the VAImage pitches already given to the
  // caller to match GBM's allocation pitch, or write into a GBM map shadow.
  const uint32_t width = static_cast<uint32_t>(std::min<size_t>(bytes, 4096));
  const uint32_t height = static_cast<uint32_t>((bytes + width - 1) / width);
  backing->bo = gbm_bo_create(driver->gbm, width, height, GBM_FORMAT_R8,
                              GBM_BO_USE_LINEAR);
  if (backing->bo == nullptr || gbm_bo_get_plane_count(backing->bo) != 1 ||
      gbm_bo_get_modifier(backing->bo) != DRM_FORMAT_MOD_LINEAR ||
      gbm_bo_get_offset(backing->bo, 0) != 0)
    return {};
  backing->fd = gbm_bo_get_fd(backing->bo);
  if (backing->fd < 0 || fcntl(backing->fd, F_SETFD, FD_CLOEXEC) != 0)
    return {};
  const off_t size = lseek(backing->fd, 0, SEEK_END);
  if (size < 0 || static_cast<uint64_t>(size) < bytes ||
      static_cast<uint64_t>(size) > maximum_image_bytes * 4 ||
      static_cast<uint64_t>(size) > SIZE_MAX)
    return {};
  backing->size = static_cast<size_t>(size);
  backing->mapping = mmap(nullptr, backing->size, PROT_READ | PROT_WRITE,
                           MAP_SHARED, backing->fd, 0);
  if (backing->mapping == MAP_FAILED)
    return {};
  return backing;
}

static VAStatus AcquireBufferHandle(VADriverContextP context,
                                    VABufferID buffer_id, VABufferInfo *info) {
  if (info == nullptr)
    return VA_STATUS_ERROR_INVALID_PARAMETER;
  Driver *driver = GetDriver(context);
  std::lock_guard<std::mutex> lock(driver->mutex);
  auto found = driver->buffers.find(buffer_id);
  if (found == driver->buffers.end())
    return VA_STATUS_ERROR_INVALID_BUFFER;
  Buffer &buffer = found->second;
  if (buffer.type != VAImageBufferType || buffer.context != VA_INVALID_ID)
    return VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;
  if (info->mem_type != 0 &&
      (info->mem_type & VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME) == 0)
    return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
  if (BufferBorrowed(buffer) || buffer.map_count != 0)
    return VA_STATUS_ERROR_SURFACE_BUSY;
  const auto image = std::find_if(driver->images.begin(), driver->images.end(),
                                 [&](const auto &entry) {
                                   return entry.second.va.buf == buffer_id;
                                 });
  if (image == driver->images.end() || !ImageBufferFits(image->second, buffer))
    return VA_STATUS_ERROR_INVALID_BUFFER;
  const std::shared_ptr<ImageExportBacking> backing =
      AllocateImageExportBacking(driver, buffer.data.size());
  if (!backing)
    return VA_STATUS_ERROR_ALLOCATION_FAILED;
  if (buffer.derived_source) {
    const VAStatus status = PrepareDerivedWrite(driver, buffer, true);
    if (status != VA_STATUS_SUCCESS)
      return status;
  }
  if (!SyncDmaBuf(backing->fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE)) {
    CancelDerivedWrite(buffer);
    return VA_STATUS_ERROR_OPERATION_FAILED;
  }
  // Never export uninitialized GBM padding along with the image bytes.
  memset(backing->mapping, 0, backing->size);
  memcpy(backing->mapping, buffer.data.data(), buffer.data.size());
  if (!SyncDmaBuf(backing->fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE)) {
    CancelDerivedWrite(buffer);
    return VA_STATUS_ERROR_OPERATION_FAILED;
  }
  buffer.exported = backing;
  *info = {};
  info->handle = static_cast<uintptr_t>(backing->fd);
  info->type = VAImageBufferType;
  info->mem_type = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME;
  info->mem_size = backing->size;
  return VA_STATUS_SUCCESS;
}

static bool WaitImageExportIdle(int fd) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::nanoseconds(kDecodeTimeoutNs);
  for (;;) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline)
      return false;
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                               deadline - now).count();
    // POLLOUT on a DMA-BUF waits for both its reader and writer fences.
    pollfd descriptor = {fd, POLLOUT, 0};
    const int result = poll(&descriptor, 1, static_cast<int>(remaining + 1));
    if (result > 0)
      return (descriptor.revents & POLLOUT) != 0 &&
             (descriptor.revents & (POLLERR | POLLNVAL | POLLHUP)) == 0;
    if (result == 0 || errno != EINTR)
      return false;
  }
}

static VAStatus ReleaseBufferHandle(VADriverContextP context,
                                    VABufferID buffer_id) {
  Driver *driver = GetDriver(context);
  std::unique_lock<std::mutex> lock(driver->mutex);
  auto found = driver->buffers.find(buffer_id);
  if (found == driver->buffers.end() || !found->second.exported)
    return VA_STATUS_ERROR_INVALID_BUFFER;
  if (found->second.export_busy)
    return VA_STATUS_ERROR_SURFACE_BUSY;
  const std::shared_ptr<ImageExportBacking> backing = found->second.exported;
  std::vector<uint8_t> updated(found->second.data.size());
  found->second.export_busy = true;
  // External graphics work must not hold up the decode worker's global lock.
  // Busy guards keep this buffer intact while other maps may be rehashed.
  lock.unlock();
  bool success = WaitImageExportIdle(backing->fd);
  if (success)
    success = SyncDmaBuf(backing->fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
  if (success) {
    memcpy(updated.data(), backing->mapping, updated.size());
    success = SyncDmaBuf(backing->fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
  }
  lock.lock();
  found = driver->buffers.find(buffer_id);
  if (found == driver->buffers.end() || found->second.exported != backing)
    return VA_STATUS_ERROR_INVALID_BUFFER;
  Buffer &buffer = found->second;
  buffer.exported.reset();
  buffer.export_busy = false;
  if (!success) {
    // Do not later report the pre-export CPU bytes as successful output from
    // an external writer whose completion/cache synchronization failed.
    buffer.data_failed = true;
    FinishDerivedWrite(driver, buffer, false);
    return VA_STATUS_ERROR_OPERATION_FAILED;
  }
  buffer.data.swap(updated);
  return FinishDerivedWrite(driver, buffer, true);
}

static VAStatus ExportSurfaceHandle(VADriverContextP context,
                                    VASurfaceID surface_id, uint32_t memory_type,
                                    uint32_t flags, void *descriptor) {
  // export_exit records this VA call's descriptor/synchronization result. It
  // is not evidence that an external GPU reader later finished using the fd.
  if (memory_type != VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2 ||
      descriptor == nullptr)
    return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
  constexpr uint32_t known_flags = VA_EXPORT_SURFACE_READ_WRITE |
      VA_EXPORT_SURFACE_SEPARATE_LAYERS | VA_EXPORT_SURFACE_COMPOSED_LAYERS;
  if ((flags & ~known_flags) != 0 ||
      ((flags & VA_EXPORT_SURFACE_SEPARATE_LAYERS) != 0 &&
       (flags & VA_EXPORT_SURFACE_COMPOSED_LAYERS) != 0))
    return VA_STATUS_ERROR_INVALID_PARAMETER;
  Driver *driver = GetDriver(context);
  std::unique_lock<std::mutex> lock(driver->mutex);
  const uint32_t access = flags & VA_EXPORT_SURFACE_READ_WRITE;
  const bool explicit_read_only = access == VA_EXPORT_SURFACE_READ_ONLY;
  const bool trace_enabled = driver->trace.enabled();
  crystalhd_vaapi_trace::Fields trace_fields;
  std::chrono::steady_clock::time_point trace_start;
  if (trace_enabled) {
    const auto traced = driver->surfaces.find(surface_id);
    if (traced != driver->surfaces.end() &&
        HasLiveBackingOwner(driver, traced->second.get()))
      trace_fields = TraceRequestedOwnerFields(
          traced->second.get(), BackingOwner(driver, traced->second.get()));
    trace_fields.surface = TraceId(surface_id, VA_INVALID_SURFACE);
    trace_fields.operation = NextTraceCall(driver);
    trace_start = std::chrono::steady_clock::now();
    driver->trace.Emit(crystalhd_vaapi_trace::Event::ExportEnter,
                       trace_fields);
  }
  const auto finish = [&](VAStatus status) {
    if (status == VA_STATUS_SUCCESS) {
      const auto exported = driver->surfaces.find(surface_id);
      if (exported != driver->surfaces.end() &&
          HasLiveBackingOwner(driver, exported->second.get()))
        BackingOwner(driver, exported->second.get())->external_accessed = true;
    }
    if (trace_enabled) {
      const auto current_surface = driver->surfaces.find(surface_id);
      if (current_surface != driver->surfaces.end() &&
          HasLiveBackingOwner(driver, current_surface->second.get())) {
        const crystalhd_vaapi_trace::Fields current =
            TraceRequestedOwnerFields(
                current_surface->second.get(),
                BackingOwner(driver, current_surface->second.get()));
        if (status == VA_STATUS_SUCCESS &&
            SameTraceBinding(trace_fields, current)) {
          trace_fields.payload_token = current.payload_token;
          trace_fields.payload_decode_identity =
              current.payload_decode_identity;
          trace_fields.payload_submission_ordinal =
              current.payload_submission_ordinal;
          trace_fields.submission_drift = current.submission_drift;
        }
      }
      trace_fields.duration_ns = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - trace_start).count());
      trace_fields.outcome = status;
      driver->trace.Emit(crystalhd_vaapi_trace::Event::ExportExit,
                         trace_fields);
    }
    return status;
  };
  const auto initial_surface = driver->surfaces.find(surface_id);
  if (initial_surface != driver->surfaces.end() &&
      HasLiveBackingOwner(driver, initial_surface->second.get())) {
    Surface *initial_owner =
        BackingOwner(driver, initial_surface->second.get());
    if (initial_owner->opaque_reference)
      return finish(VA_STATUS_ERROR_INVALID_SURFACE);
    // WRITE_ONLY export skips the read synchronization below. Do not expose
    // an internal target whose queued write has no external completion fence.
    if (access == VA_EXPORT_SURFACE_WRITE_ONLY &&
        std::any_of(driver->pending_vpp.begin(), driver->pending_vpp.end(),
                    [&](const PendingVpp &pending) {
                      return pending.target_owner.get() == initial_owner &&
                             pending.write_timeline < 0;
                    }))
      return finish(VA_STATUS_ERROR_SURFACE_BUSY);
    if (initial_owner->image_write_locked ||
        ExperimentalWriteReservationPending(initial_owner))
      return finish(VA_STATUS_ERROR_SURFACE_BUSY);
    if (!explicit_read_only &&
        (ExperimentalPresentationBinding(driver, initial_owner) ||
         initial_owner->experimental_read_export_armed ||
         initial_owner->experimental_presentation))
      return finish(VA_STATUS_ERROR_INVALID_PARAMETER);
  }
  if ((flags & VA_EXPORT_SURFACE_READ_WRITE) != VA_EXPORT_SURFACE_WRITE_ONLY) {
    const VAStatus status = SynchronizeExportRead(context, surface_id, &lock);
    if (status != VA_STATUS_SUCCESS)
      return finish(status);
  }
  auto surface = driver->surfaces.find(surface_id);
  if (surface == driver->surfaces.end() ||
      !HasLiveBackingOwner(driver, surface->second.get()))
    return finish(VA_STATUS_ERROR_INVALID_SURFACE);
  if (surface->second->bo == nullptr && surface->second->object_fds.empty())
    return finish(VA_STATUS_ERROR_UNIMPLEMENTED);
  Surface *owner = BackingOwner(driver, surface->second.get());
  // SynchronizeExportRead may release the mutex while waiting and may commit
  // an experimental payload.  Revalidate the access contract afterward.
  if (owner->opaque_reference)
    return finish(VA_STATUS_ERROR_INVALID_SURFACE);
  if (owner->image_write_locked || ExperimentalWriteReservationPending(owner))
    return finish(VA_STATUS_ERROR_SURFACE_BUSY);
  if (!explicit_read_only &&
      (ExperimentalPresentationBinding(driver, owner) ||
       owner->experimental_read_export_armed ||
       owner->experimental_presentation))
    return finish(VA_STATUS_ERROR_INVALID_PARAMETER);
  ConsumeExperimentalExact(driver, surface->second);
  // A read-only consumer retains the VA frame until it is finished reading,
  // so the next decode submission is the reuse boundary just as it is for an
  // unexported surface. Writable or unspecified access has no such immutable
  // contract: preserve the current picture and permanently keep later output
  // private before exposing the backing.
  if (!explicit_read_only) {
    if (!PromoteDecodedPicture(owner->decode_picture.lock()))
      return finish(VA_STATUS_ERROR_OPERATION_FAILED);
    owner->direct_decode_eligible = false;
    if (ExperimentalPresentationBinding(driver, owner)) {
      owner->experimental_incompatible = true;
      ClearExperimentalPresentation(owner);
      ClearExperimentalReadExport(owner);
    }
  }

  auto *prime = static_cast<VADRMPRIMESurfaceDescriptor *>(descriptor);
  memset(prime, 0, sizeof(*prime));
  for (auto &object : prime->objects)
    object.fd = -1;
  prime->fourcc = surface->second->fourcc;
  prime->width = surface->second->width;
  prime->height = surface->second->height;
  if (surface->second->bo == nullptr && surface->second->layout.valid) {
    // A mapped import can have separate Y/UV objects, plane offsets or padded
    // strides. Re-export that exact registered view, not the internal DUMB
    // allocation's assumed Y-at-zero/contiguous-UV layout.
    const SurfaceLayout &imported_layout = surface->second->layout;
    const unsigned expected_planes = prime->fourcc == VA_FOURCC_NV12 ? 2 : 1;
    if ((prime->fourcc != VA_FOURCC_NV12 && prime->fourcc != VA_FOURCC_ARGB) ||
        imported_layout.num_planes != expected_planes ||
        imported_layout.objects.empty() || imported_layout.objects.size() > 4 ||
        surface->second->object_fds.size() != surface->second->object_sizes.size())
      return finish(VA_STATUS_ERROR_OPERATION_FAILED);
    prime->num_objects = imported_layout.objects.size();
    const auto failed_export = [&] {
      for (unsigned object = 0; object < prime->num_objects; ++object) {
        if (prime->objects[object].fd >= 0)
          close(prime->objects[object].fd);
        prime->objects[object].fd = -1;
      }
      prime->num_objects = 0;
      return finish(VA_STATUS_ERROR_OPERATION_FAILED);
    };
    for (unsigned object = 0; object < prime->num_objects; ++object) {
      bool found = false;
      for (size_t stored = 0; stored < surface->second->object_fds.size(); ++stored) {
        BackingIdentity identity;
        if (!GetBackingIdentity(surface->second->object_fds[stored], &identity) ||
            !(identity == imported_layout.objects[object]))
          continue;
        const size_t bytes = surface->second->object_sizes[stored];
        if (bytes == 0 || bytes > UINT32_MAX)
          return failed_export();
        const int fd = fcntl(surface->second->object_fds[stored], F_DUPFD_CLOEXEC, 0);
        if (fd < 0)
          return failed_export();
        prime->objects[object].fd = fd;
        prime->objects[object].size = bytes;
        // Nonlinear imports use GBM above; mapped imports are linear.
        prime->objects[object].drm_format_modifier = DRM_FORMAT_MOD_LINEAR;
        found = true;
        break;
      }
      if (!found)
        return failed_export();
    }
    const bool composed = prime->fourcc == VA_FOURCC_ARGB ||
        (flags & VA_EXPORT_SURFACE_COMPOSED_LAYERS) != 0;
    prime->num_layers = composed ? 1 : expected_planes;
    if (composed) {
      prime->layers[0].drm_format = prime->fourcc == VA_FOURCC_ARGB
          ? DRM_FORMAT_ARGB8888 : DRM_FORMAT_NV12;
      prime->layers[0].num_planes = expected_planes;
    }
    for (unsigned plane = 0; plane < expected_planes; ++plane) {
      const auto &view = imported_layout.planes[plane];
      const auto object = std::find(imported_layout.objects.begin(),
                                    imported_layout.objects.end(), view.backing);
      if (object == imported_layout.objects.end())
        return failed_export();
      const unsigned layer = composed ? 0 : plane;
      const unsigned index = composed ? plane : 0;
      if (!composed) {
        prime->layers[layer].drm_format = plane == 0 ? DRM_FORMAT_R8 : DRM_FORMAT_GR88;
        prime->layers[layer].num_planes = 1;
      }
      prime->layers[layer].object_index[index] = object - imported_layout.objects.begin();
      prime->layers[layer].offset[index] = view.offset;
      prime->layers[layer].pitch[index] = view.pitch;
    }
    SurfaceLayout layout;
    if (!GetPrimeLayout(*prime, &layout) ||
        !RegisterExportedLayout(driver, surface->second.get(), surface_id, layout))
      return failed_export();
    if (surface->second.get() == owner && explicit_read_only)
      ArmTraceReadExport(driver, surface->second.get(), owner, trace_fields.operation);
    return finish(VA_STATUS_SUCCESS);
  }
  if (surface->second->fourcc == VA_FOURCC_ARGB) {
    const int fd = surface->second->bo != nullptr
                       ? gbm_bo_get_fd_for_plane(surface->second->bo, 0)
                       : dup(surface->second->object_fds[0]);
    if (fd < 0)
      return finish(VA_STATUS_ERROR_OPERATION_FAILED);
    prime->num_objects = 1;
    prime->num_layers = 1;
    prime->objects[0].fd = fd;
    prime->objects[0].size = surface->second->bo != nullptr
                                 ? surface->second->pitch[0] *
                                       surface->second->height
                                 : surface->second->object_sizes[0];
    prime->objects[0].drm_format_modifier =
        surface->second->bo != nullptr
            ? gbm_bo_get_modifier(surface->second->bo)
            : DRM_FORMAT_MOD_LINEAR;
    prime->layers[0].drm_format = DRM_FORMAT_ARGB8888;
    prime->layers[0].num_planes = 1;
    prime->layers[0].object_index[0] = 0;
    prime->layers[0].offset[0] = surface->second->bo != nullptr
                                     ? gbm_bo_get_offset(surface->second->bo, 0)
                                     : 0;
    prime->layers[0].pitch[0] = surface->second->pitch[0];
    SurfaceLayout layout;
    if (!GetPrimeLayout(*prime, &layout) ||
        !RegisterExportedLayout(driver, surface->second.get(), surface_id, layout)) {
      close(fd);
      prime->num_objects = 0;
      return finish(VA_STATUS_ERROR_OPERATION_FAILED);
    }
    if (surface->second.get() == owner) {
      if (explicit_read_only)
        ArmTraceReadExport(driver, surface->second.get(), owner,
                           trace_fields.operation);
    }
    return finish(VA_STATUS_SUCCESS);
  }
  if (surface->second->fourcc != VA_FOURCC_NV12)
    return finish(VA_STATUS_ERROR_UNIMPLEMENTED);
  const bool gbm_backed = surface->second->bo != nullptr;
  prime->num_objects = gbm_backed ? 2 : 1;
  for (unsigned int object = 0; object < prime->num_objects; ++object) {
    prime->objects[object].fd =
        gbm_backed ? gbm_bo_get_fd_for_plane(surface->second->bo, object)
                   : dup(surface->second->object_fds[0]);
    if (prime->objects[object].fd < 0) {
      for (unsigned int created = 0; created < object; ++created)
        close(prime->objects[created].fd);
      prime->num_objects = 0;
      return finish(VA_STATUS_ERROR_OPERATION_FAILED);
    }
    prime->objects[object].size =
        gbm_backed
            ? gbm_bo_get_offset(surface->second->bo, object) +
                  surface->second->pitch[object] *
                      (object == 0 ? surface->second->height
                                   : (surface->second->height + 1) / 2)
            : surface->second->object_sizes[0];
    prime->objects[object].drm_format_modifier =
        gbm_backed ? gbm_bo_get_modifier(surface->second->bo)
                   : DRM_FORMAT_MOD_LINEAR;
  }
  if ((flags & VA_EXPORT_SURFACE_COMPOSED_LAYERS) == 0) {
    prime->num_layers = 2;
    prime->layers[0].drm_format = DRM_FORMAT_R8;
    prime->layers[1].drm_format = DRM_FORMAT_GR88;
    for (unsigned int plane = 0; plane < 2; ++plane) {
      prime->layers[plane].num_planes = 1;
      prime->layers[plane].object_index[0] = gbm_backed ? plane : 0;
      prime->layers[plane].offset[0] = gbm_backed
          ? gbm_bo_get_offset(surface->second->bo, plane)
          : (plane == 0 ? 0 : surface->second->pitch[0] *
                                  surface->second->height);
      prime->layers[plane].pitch[0] = surface->second->pitch[plane];
    }
  } else {
    prime->num_layers = 1;
    prime->layers[0].drm_format = DRM_FORMAT_NV12;
    prime->layers[0].num_planes = 2;
    for (unsigned int plane = 0; plane < 2; ++plane) {
      prime->layers[0].object_index[plane] = gbm_backed ? plane : 0;
      prime->layers[0].offset[plane] = gbm_backed
          ? gbm_bo_get_offset(surface->second->bo, plane)
          : (plane == 0 ? 0 : surface->second->pitch[0] *
                                  surface->second->height);
      prime->layers[0].pitch[plane] = surface->second->pitch[plane];
    }
  }
  SurfaceLayout layout;
  if (!GetPrimeLayout(*prime, &layout) ||
      !RegisterExportedLayout(driver, surface->second.get(), surface_id, layout)) {
    for (unsigned int object = 0; object < prime->num_objects; ++object)
      close(prime->objects[object].fd);
    prime->num_objects = 0;
    return finish(VA_STATUS_ERROR_OPERATION_FAILED);
  }
  if (surface->second.get() == owner) {
    if (explicit_read_only)
      ArmTraceReadExport(driver, surface->second.get(), owner,
                         trace_fields.operation);
  }
  return finish(VA_STATUS_SUCCESS);
}

static VAStatus QuerySurfaceError(VADriverContextP, VASurfaceID, VAStatus,
                                  void **error_info) {
  if (error_info != nullptr)
    *error_info = nullptr;
  return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus PutSurface(VADriverContextP, VASurfaceID, void *, short, short,
                           unsigned short, unsigned short, short, short,
                           unsigned short, unsigned short, VARectangle *,
                           unsigned int, unsigned int) {
  return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus SetImagePalette(VADriverContextP, VAImageID, unsigned char *) {
  return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus QuerySubpictureFormats(VADriverContextP, VAImageFormat *,
                                       unsigned int *, unsigned int *count) {
  if (count == nullptr)
    return VA_STATUS_ERROR_INVALID_PARAMETER;
  *count = 0;
  return VA_STATUS_SUCCESS;
}

static VAStatus CreateSubpicture(VADriverContextP, VAImageID,
                                 VASubpictureID *) {
  return VA_STATUS_ERROR_UNIMPLEMENTED;
}
static VAStatus DestroySubpicture(VADriverContextP, VASubpictureID) {
  return VA_STATUS_ERROR_UNIMPLEMENTED;
}
static VAStatus SetSubpictureImage(VADriverContextP, VASubpictureID,
                                   VAImageID) {
  return VA_STATUS_ERROR_UNIMPLEMENTED;
}
static VAStatus SetSubpictureChromakey(VADriverContextP, VASubpictureID,
                                       unsigned int, unsigned int,
                                       unsigned int) {
  return VA_STATUS_ERROR_UNIMPLEMENTED;
}
static VAStatus SetSubpictureGlobalAlpha(VADriverContextP, VASubpictureID,
                                         float) {
  return VA_STATUS_ERROR_UNIMPLEMENTED;
}
static VAStatus AssociateSubpicture(VADriverContextP, VASubpictureID,
                                    VASurfaceID *, int, short, short,
                                    unsigned short, unsigned short, short, short,
                                    unsigned short, unsigned short,
                                    unsigned int) {
  return VA_STATUS_ERROR_UNIMPLEMENTED;
}
static VAStatus DeassociateSubpicture(VADriverContextP, VASubpictureID,
                                      VASurfaceID *, int) {
  return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus QueryDisplayAttributes(VADriverContextP, VADisplayAttribute *,
                                       int *count) {
  if (count == nullptr)
    return VA_STATUS_ERROR_INVALID_PARAMETER;
  *count = 0;
  return VA_STATUS_SUCCESS;
}
static VAStatus GetDisplayAttributes(VADriverContextP, VADisplayAttribute *attributes,
                                     int count) {
  if (count < 0 || (count != 0 && attributes == nullptr))
    return VA_STATUS_ERROR_INVALID_PARAMETER;
  return count == 0 ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
}
static VAStatus SetDisplayAttributes(VADriverContextP, VADisplayAttribute *attributes,
                                     int count) {
  if (count < 0 || (count != 0 && attributes == nullptr))
    return VA_STATUS_ERROR_INVALID_PARAMETER;
  return count == 0 ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
}

}  // namespace

#define CRYSTALHD_EXPORT __attribute__((visibility("default")))

#define CRYSTALHD_INIT_NAME_INNER(major, minor) \
  __vaDriverInit_##major##_##minor
#define CRYSTALHD_INIT_NAME(major, minor) \
  CRYSTALHD_INIT_NAME_INNER(major, minor)

static VAStatus InitializeDriver(VADriverContextP context,
                                 int api_minor_version) {
  if (context == nullptr || context->vtable == nullptr ||
      context->drm_state == nullptr)
    return VA_STATUS_ERROR_INVALID_DISPLAY;

  const int drm_fd = static_cast<drm_state *>(context->drm_state)->fd;
  std::unique_ptr<Driver> driver(new (std::nothrow) Driver(drm_fd));
  if (!driver)
    return VA_STATUS_ERROR_ALLOCATION_FAILED;
  driver->StartDecodeWorker();

  context->version_major = VA_MAJOR_VERSION;
  context->version_minor = api_minor_version;
  context->str_vendor = "Broadcom CrystalHD VA-API driver 0.1";
  context->max_profiles = 11;
  context->max_entrypoints = 1;
  context->max_attributes = 16;
  context->max_image_formats = 1;
  context->max_subpic_formats = 1;
  context->max_display_attributes = 1;
  context->pDriverData = driver.release();

  VADriverVTable *vtable = context->vtable;
  vtable->vaTerminate = Terminate;
  vtable->vaQueryConfigProfiles = QueryConfigProfiles;
  vtable->vaQueryConfigEntrypoints = QueryConfigEntrypoints;
  vtable->vaGetConfigAttributes = GetConfigAttributes;
  vtable->vaCreateConfig = CreateConfig;
  vtable->vaDestroyConfig = DestroyConfig;
  vtable->vaQueryConfigAttributes = QueryConfigAttributes;
  vtable->vaCreateSurfaces = CreateSurfaces;
  vtable->vaDestroySurfaces = DestroySurfaces;
  vtable->vaCreateContext = CreateContext;
  vtable->vaDestroyContext = DestroyContext;
  vtable->vaCreateBuffer = CreateBuffer;
  vtable->vaBufferSetNumElements = BufferSetNumElements;
  vtable->vaMapBuffer = MapBuffer;
  vtable->vaUnmapBuffer = UnmapBuffer;
  vtable->vaDestroyBuffer = DestroyBuffer;
  vtable->vaBeginPicture = BeginPicture;
  vtable->vaRenderPicture = RenderPicture;
  vtable->vaEndPicture = EndPicture;
  vtable->vaSyncSurface = SyncSurface;
  vtable->vaQuerySurfaceStatus = QuerySurfaceStatus;
  vtable->vaQuerySurfaceError = QuerySurfaceError;
  vtable->vaPutSurface = PutSurface;
  vtable->vaQueryImageFormats = QueryImageFormats;
  vtable->vaCreateImage = CreateImage;
  vtable->vaDeriveImage = DeriveImage;
  vtable->vaDestroyImage = DestroyImage;
  vtable->vaAcquireBufferHandle = AcquireBufferHandle;
  vtable->vaReleaseBufferHandle = ReleaseBufferHandle;
  vtable->vaSetImagePalette = SetImagePalette;
  vtable->vaGetImage = GetImage;
  vtable->vaPutImage = PutImage;
  vtable->vaQuerySubpictureFormats = QuerySubpictureFormats;
  vtable->vaCreateSubpicture = CreateSubpicture;
  vtable->vaDestroySubpicture = DestroySubpicture;
  vtable->vaSetSubpictureImage = SetSubpictureImage;
  vtable->vaSetSubpictureChromakey = SetSubpictureChromakey;
  vtable->vaSetSubpictureGlobalAlpha = SetSubpictureGlobalAlpha;
  vtable->vaAssociateSubpicture = AssociateSubpicture;
  vtable->vaDeassociateSubpicture = DeassociateSubpicture;
  vtable->vaQueryDisplayAttributes = QueryDisplayAttributes;
  vtable->vaGetDisplayAttributes = GetDisplayAttributes;
  vtable->vaSetDisplayAttributes = SetDisplayAttributes;
  vtable->vaBufferInfo = BufferInfo;
  vtable->vaCreateSurfaces2 = CreateSurfaces2;
  vtable->vaQuerySurfaceAttributes = QuerySurfaceAttributes;
  vtable->vaExportSurfaceHandle = ExportSurfaceHandle;
  vtable->vaSyncSurface2 = SyncSurface2;
  if (context->vtable_vpp != nullptr) {
    context->vtable_vpp->version = VA_DRIVER_VTABLE_VPP_VERSION;
    context->vtable_vpp->vaQueryVideoProcFilters = QueryVideoProcFilters;
    context->vtable_vpp->vaQueryVideoProcFilterCaps =
        QueryVideoProcFilterCaps;
    context->vtable_vpp->vaQueryVideoProcPipelineCaps =
        QueryVideoProcPipelineCaps;
  }
#if VA_CHECK_VERSION(1, 21, 0)
  if (api_minor_version >= 21)
    vtable->vaMapBuffer2 = MapBuffer2;
#endif
  return VA_STATUS_SUCCESS;
}

extern "C" VAStatus CRYSTALHD_EXPORT
CRYSTALHD_INIT_NAME(VA_MAJOR_VERSION, VA_MINOR_VERSION)(
    VADriverContextP context) {
  return InitializeDriver(context, VA_MINOR_VERSION);
}

#if VA_CHECK_VERSION(1, 21, 0) && VA_MINOR_VERSION != 21
extern "C" VAStatus CRYSTALHD_EXPORT __vaDriverInit_1_21(
    VADriverContextP context) {
  return InitializeDriver(context, 21);
}
#endif

#if VA_CHECK_VERSION(1, 20, 0) && VA_MINOR_VERSION != 20
extern "C" VAStatus CRYSTALHD_EXPORT __vaDriverInit_1_20(
    VADriverContextP context) {
  return InitializeDriver(context, 20);
}
#endif
#endif  // CRYSTALHD_H264_TEST_BUILD
