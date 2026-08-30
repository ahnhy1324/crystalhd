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
#include <climits>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <emmintrin.h>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <gbm.h>
#include <libdrm/drm_fourcc.h>
#include <libdrm/drm_mode.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
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

namespace {

constexpr unsigned int kMaxWidth = 1920;
constexpr unsigned int kMaxHeight = 1088;
constexpr uint64_t kTimestampStep = 100000;
constexpr uint32_t kInputRetries = 1000;
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

static bool BuildSps(const VAPictureParameterBufferH264 &picture,
                     VAProfile profile, std::vector<uint8_t> *output) {
  BitWriter bits;
  uint8_t profile_idc;

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
  bits.PutUE(picture.picture_height_in_mbs_minus1);
  bits.PutBit(picture.seq_fields.bits.frame_mbs_only_flag);
  if (!picture.seq_fields.bits.frame_mbs_only_flag)
    bits.PutBit(picture.seq_fields.bits.mb_adaptive_frame_field_flag);
  bits.PutBit(picture.seq_fields.bits.direct_8x8_inference_flag);
  bits.PutBit(false);
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

static bool BuildPps(const VAPictureParameterBufferH264 &picture,
                     const VASliceParameterBufferH264 &slice,
                     VAProfile profile, std::vector<uint8_t> *output) {
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
    bits.PutBit(false);
    bits.PutSE(picture.second_chroma_qp_index_offset);
  }
  AppendNal(output, 8, 3, bits.FinishRbsp());
  return true;
}

struct Config {
  VAProfile profile = VAProfileNone;
  VAEntrypoint entrypoint = VAEntrypointVLD;
};

struct Buffer {
  VAContextID context = VA_INVALID_ID;
  VABufferType type = VAPictureParameterBufferType;
  unsigned int element_size = 0;
  unsigned int elements = 0;
  std::vector<uint8_t> data;
};

struct Surface {
  unsigned int width = 0;
  unsigned int height = 0;
  unsigned int rt_format = 0;
  uint32_t fourcc = VA_FOURCC_NV12;
  unsigned int pitch[2] = {};
  uint8_t *planes[2] = {};
  bool ready = false;
  bool failed = false;
  bool destroyed = false;
  unsigned int vpp_readers = 0;
  unsigned int vpp_writers = 0;
  VASurfaceID backing_owner = VA_INVALID_SURFACE;
  uint64_t expected_timestamp = 0;
  uint64_t frame_timestamp = 0;
  uint64_t latest_vpp_sequence = 0;

  std::vector<uint8_t> storage;
  gbm_bo *bo = nullptr;
  void *map_data[2] = {};
  std::vector<void *> object_maps;
  std::vector<size_t> object_sizes;
  std::vector<int> object_fds;
  int dumb_drm_fd = -1;
  uint32_t dumb_handle = 0;

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
  }

  bool AllocateInternal(gbm_device *gbm, int drm_fd,
                        unsigned int requested_width,
                        unsigned int requested_height,
                        uint32_t requested_fourcc) {
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
    drmDevicePtr device = nullptr;
    if (drm_fd >= 0 && drmGetDevice2(drm_fd, 0, &device) == 0 &&
        device != nullptr &&
        (device->available_nodes & (1U << DRM_NODE_PRIMARY)) != 0) {
      dumb_drm_fd = open(device->nodes[DRM_NODE_PRIMARY], O_RDWR | O_CLOEXEC);
      drmFreeDevice(&device);
    } else if (device != nullptr) {
      drmFreeDevice(&device);
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
    pitch[0] = Align(width, 64);
    pitch[1] = pitch[0];
    storage.resize(static_cast<size_t>(pitch[0]) * height +
                   static_cast<size_t>(pitch[1]) * ((height + 1) / 2));
    planes[0] = storage.data();
    planes[1] = planes[0] + static_cast<size_t>(pitch[0]) * height;
    return true;
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
      const size_t rows = plane == 0 ? height : (height + 1) / 2;
      const size_t row_bytes = is_argb
          ? static_cast<size_t>(width) * 4
          : (plane == 0 ? width : Align(width, 2));
      const size_t object_size = object_sizes[plane_desc[plane].object];
      if (plane_desc[plane].pitch < row_bytes || rows == 0 ||
          plane_desc[plane].offset > object_size ||
          static_cast<size_t>(plane_desc[plane].pitch) * (rows - 1) +
                  row_bytes >
              object_size - plane_desc[plane].offset)
        return false;
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
      const size_t rows = plane == 0 ? height : (height + 1) / 2;
      const size_t row_bytes = is_argb
          ? static_cast<size_t>(width) * 4
          : (plane == 0 ? width : Align(width, 2));
      if (desc.pitches[plane] < row_bytes || rows == 0 ||
          desc.offsets[plane] > desc.data_size ||
          static_cast<size_t>(desc.pitches[plane]) * (rows - 1) + row_bytes >
              desc.data_size - desc.offsets[plane])
        return false;
      pitch[plane] = desc.pitches[plane];
      planes[plane] = static_cast<uint8_t *>(object_maps[0]) +
                      desc.offsets[plane];
    }
    return true;
  }

  void BeginCpuRead() const {
    for (int fd : object_fds) {
      dma_buf_sync sync = {};
      sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ;
      ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
    }
  }

  void EndCpuRead() const {
    for (int fd : object_fds) {
      dma_buf_sync sync = {};
      sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
      ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
    }
  }

  bool BeginCpuWrite() const {
    bool success = true;
    for (int fd : object_fds) {
      dma_buf_sync sync = {};
      sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE;
      if (ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) != 0)
        success = false;
    }
    return success;
  }

  bool EndCpuWrite() const {
    bool success = true;
    for (int fd : object_fds) {
      dma_buf_sync sync = {};
      sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
      if (ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) != 0)
        success = false;
    }
    return success;
  }

  // The asynchronous path cannot call DMA_BUF_IOCTL_SYNC END while its own
  // unsignaled write fence is installed: i915 waits on that fence. Flush every
  // mapped cache line explicitly before signaling, so a newly released GPU
  // reader can only observe the fully committed image.
  void FlushCpuWrites() const {
    constexpr uintptr_t cache_line_size = 64;
    for (size_t object = 0; object < object_maps.size(); ++object) {
      if (object_maps[object] == MAP_FAILED ||
          object_maps[object] == nullptr || object_sizes[object] == 0)
        continue;
      const uintptr_t first =
          reinterpret_cast<uintptr_t>(object_maps[object]) &
          ~(cache_line_size - 1);
      const uintptr_t last =
          reinterpret_cast<uintptr_t>(object_maps[object]) +
          object_sizes[object];
      for (uintptr_t address = first; address < last;
           address += cache_line_size) {
        _mm_clflush(reinterpret_cast<const void *>(address));
      }
    }
    _mm_mfence();
  }
};

// Begin CPU access before publishing the fence: vaEndPicture has not returned,
// so Chromium cannot yet enqueue a new read of this target. The imported
// unsignaled write fence then protects the whole asynchronous interval.
static int BeginFencedWrite(Surface *surface) {
  if (surface == nullptr || surface->object_fds.empty())
    return -1;
  if (!surface->BeginCpuWrite())
    return -1;

  int timeline = open(kSwSyncPath, O_RDWR | O_CLOEXEC);
  SwSyncCreateFenceData create = {};
  create.value = 1;
  create.fence = -1;
  snprintf(create.name, sizeof(create.name), "crystalhd-vpp");
  if (timeline < 0 ||
      ioctl(timeline, SW_SYNC_IOC_CREATE_FENCE, &create) != 0) {
    surface->EndCpuWrite();
    if (timeline >= 0)
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
    surface->EndCpuWrite();
    const uint32_t increment = 1;
    ioctl(timeline, SW_SYNC_IOC_INC, &increment);
    close(timeline);
    return -1;
  }
  return timeline;
}

static void EndFencedWrite(Surface *surface, int timeline) {
  if (timeline < 0)
    return;
  if (surface != nullptr)
    surface->FlushCpuWrites();
  const uint32_t increment = 1;
  ioctl(timeline, SW_SYNC_IOC_INC, &increment);
  // Once the data is cache-visible it is safe to release CPU ownership. This
  // may briefly wait for the just-released reader, but cannot deadlock on our
  // fence because it has already been signaled.
  if (surface != nullptr)
    surface->EndCpuWrite();
  close(timeline);
}

struct BackingIdentity {
  dev_t device = 0;
  ino_t inode = 0;

  bool operator==(const BackingIdentity &other) const {
    return device == other.device && inode == other.inode;
  }
};

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
                           bool destination_write_started = false) {
  if (scaler == nullptr || source == nullptr || destination == nullptr ||
      source->fourcc != VA_FOURCC_NV12 ||
      (destination->fourcc != VA_FOURCC_NV12 &&
       destination->fourcc != VA_FOURCC_ARGB) ||
      (destination->fourcc == VA_FOURCC_ARGB && argb_staging == nullptr) ||
      !RectFits(source_region, *source) ||
      !RectFits(output_region, *destination))
    return VA_STATUS_ERROR_INVALID_PARAMETER;
  if (source->pitch[1] < Align(source->width, 2) ||
      (destination->fourcc == VA_FOURCC_NV12 &&
       ((output_region.x | output_region.y | output_region.width |
         output_region.height) & 1U) != 0))
    return VA_STATUS_ERROR_INVALID_PARAMETER;
  if (source->expected_timestamp != 0 &&
      source->frame_timestamp != source->expected_timestamp) {
    Debug("refusing mismatched VPP source expected=%llu produced=%llu",
          static_cast<unsigned long long>(source->expected_timestamp),
          static_cast<unsigned long long>(source->frame_timestamp));
    return VA_STATUS_ERROR_DECODING_ERROR;
  }

  source->BeginCpuRead();
  if (destination->fourcc == VA_FOURCC_ARGB) {
    // libswscale used to write scanlines straight into Chromium's imported
    // DMA-BUF. vaEndPicture is asynchronous, so the compositor could sample
    // that buffer halfway through the conversion and display a diagonal or
    // lightning-shaped mix of the old frame and black. Build a complete frame
    // privately, then touch the shared buffer only for the final row copies.
    const size_t staging_pitch = static_cast<size_t>(destination->width) * 4;
    argb_staging->resize(staging_pitch * destination->height);
    const bool fills_destination =
        output_region.x == 0 && output_region.y == 0 &&
        output_region.width == destination->width &&
        output_region.height == destination->height;
    if (!fills_destination) {
      for (unsigned int row = 0; row < destination->height; ++row) {
        uint8_t *line = argb_staging->data() +
                        static_cast<size_t>(row) * staging_pitch;
        for (unsigned int column = 0; column < destination->width; ++column) {
          line[column * 4] = 0;
          line[column * 4 + 1] = 0;
          line[column * 4 + 2] = 0;
          line[column * 4 + 3] = 255;
        }
      }
    }
    *scaler = sws_getCachedContext(
        *scaler, source_region.width, source_region.height, AV_PIX_FMT_NV12,
        output_region.width, output_region.height, AV_PIX_FMT_BGRA, SWS_POINT,
        nullptr, nullptr, nullptr);
    if (*scaler == nullptr) {
      source->EndCpuRead();
      return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    const uint8_t *source_planes[4] = {
        source->planes[0] +
            static_cast<size_t>(source_region.y) * source->pitch[0] +
            source_region.x,
        source->planes[1] +
            static_cast<size_t>(source_region.y / 2) * source->pitch[1] +
            (source_region.x & ~1U),
        nullptr, nullptr};
    const int source_pitches[4] = {static_cast<int>(source->pitch[0]),
                                   static_cast<int>(source->pitch[1]), 0, 0};
    uint8_t *destination_planes[4] = {
        argb_staging->data() +
            static_cast<size_t>(output_region.y) * staging_pitch +
            static_cast<size_t>(output_region.x) * 4,
        nullptr, nullptr, nullptr};
    const int destination_pitches[4] = {
        static_cast<int>(staging_pitch), 0, 0, 0};
    const int output_lines = sws_scale(
        *scaler, source_planes, source_pitches, 0, source_region.height,
        destination_planes, destination_pitches);
    if (output_lines != output_region.height) {
      source->EndCpuRead();
      return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (!destination_write_started)
      destination->BeginCpuWrite();
    for (unsigned int row = 0; row < destination->height; ++row) {
      memcpy(destination->planes[0] +
                 static_cast<size_t>(row) * destination->pitch[0],
             argb_staging->data() + static_cast<size_t>(row) * staging_pitch,
             staging_pitch);
    }
    if (!destination_write_started)
      destination->EndCpuWrite();
  } else {
    if (!destination_write_started)
      destination->BeginCpuWrite();
    for (unsigned int row = 0; row < destination->height; ++row)
      memset(destination->planes[0] +
                 static_cast<size_t>(row) * destination->pitch[0],
             16, destination->width);
    for (unsigned int row = 0; row < (destination->height + 1) / 2; ++row)
      memset(destination->planes[1] +
                 static_cast<size_t>(row) * destination->pitch[1],
             128, destination->width);
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
    if (!destination_write_started)
      destination->EndCpuWrite();
  }
  source->EndCpuRead();
  destination->ready = true;
  destination->failed = false;
  destination->expected_timestamp = source->expected_timestamp;
  destination->frame_timestamp = source->frame_timestamp;
  return VA_STATUS_SUCCESS;
}

struct Image {
  VAImage va = {};
};

static void CopyYuy2ToSurface(Surface *surface,
                              const BC_DTS_PROC_OUT &output, bool is_70012);

struct DecodeContext {
  VAConfigID config = VA_INVALID_ID;
  unsigned int width = 0;
  unsigned int height = 0;
  bool video_process = false;
  VASurfaceID target = VA_INVALID_SURFACE;
  VASurfaceID vpp_source = VA_INVALID_SURFACE;
  VARectangle vpp_source_region = {};
  VARectangle vpp_output_region = {};
  bool have_vpp_parameters = false;
  VAPictureParameterBufferH264 picture = {};
  bool have_picture = false;
  std::vector<VASliceParameterBufferH264> slices;
  std::vector<std::vector<uint8_t>> slice_data;

  HANDLE device = nullptr;
  bool decoder_open = false;
  bool decoder_started = false;
  bool is_70012 = false;
  bool sent_parameter_sets = false;
  bool retired = false;
  uint64_t generation = 1;
  uint64_t next_timestamp = kTimestampStep;
  std::unordered_map<uint64_t, VASurfaceID> pending;
  std::unordered_map<uint64_t, std::shared_ptr<Surface>> decoded_frames;
  std::unordered_map<Surface *, uint64_t> surface_timestamps;

  ~DecodeContext() { Close(); }

  void Close() {
    if (decoder_started) {
      DtsStopDecoder(device);
      decoder_started = false;
    }
    if (decoder_open) {
      DtsCloseDecoder(device);
      decoder_open = false;
    }
    if (device != nullptr) {
      DtsDeviceClose(device);
      device = nullptr;
    }
    pending.clear();
    decoded_frames.clear();
    surface_timestamps.clear();
    sent_parameter_sets = false;
  }

  void Reset() {
    ++generation;
    Close();
  }

  BC_STATUS FlushDiscontinuity() {
    ++generation;
    BC_STATUS status = decoder_started
                           ? DtsFlushInput(device, 4)
                           : BC_STS_SUCCESS;
    pending.clear();
    decoded_frames.clear();
    surface_timestamps.clear();
    sent_parameter_sets = false;
    return status;
  }
};

struct PendingVpp {
  std::shared_ptr<DecodeContext> decoder;
  std::shared_ptr<Surface> source;
  std::shared_ptr<Surface> target;
  std::shared_ptr<Surface> target_owner;
  VARectangle source_region = {};
  VARectangle output_region = {};
  uint64_t source_timestamp = 0;
  uint64_t sequence = 0;
  uint64_t decoder_generation = 0;
  int write_timeline = -1;
  bool worker_active = false;
};

struct Driver;
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
  std::unordered_map<VAConfigID, Config> configs;
  std::unordered_map<VASurfaceID, std::shared_ptr<Surface>> surfaces;
  std::unordered_map<VAContextID, std::shared_ptr<DecodeContext>> contexts;
  std::unordered_set<VAContextID> retired_contexts;
  std::deque<VAContextID> retired_context_order;
  std::unordered_map<VABufferID, Buffer> buffers;
  std::unordered_map<VAImageID, Image> images;
  std::unordered_map<BackingIdentity, VASurfaceID, BackingIdentityHash>
      backing_owners;
  std::deque<PendingVpp> pending_vpp;
  SwsContext *vpp_scaler = nullptr;
  std::vector<uint8_t> vpp_argb_staging;
  std::vector<uint8_t> vpp_fallback;
  unsigned int vpp_fallback_width = 0;
  unsigned int vpp_fallback_height = 0;
  uint64_t vpp_fallback_sequence = 0;
  bool stopping = false;
  std::thread vpp_worker;
  gbm_device *gbm = nullptr;
  int drm_fd = -1;

  explicit Driver(int requested_drm_fd) : drm_fd(requested_drm_fd) {
    if (requested_drm_fd >= 0)
      gbm = gbm_create_device(requested_drm_fd);
    vpp_worker = std::thread(RunVppWorker, this);
  }
  ~Driver() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      stopping = true;
    }
    condition.notify_all();
    if (vpp_worker.joinable())
      vpp_worker.join();
    for (const PendingVpp &pending : pending_vpp)
      EndFencedWrite(pending.target.get(), pending.write_timeline);
    if (vpp_scaler != nullptr)
      sws_freeContext(vpp_scaler);
    contexts.clear();
    pending_vpp.clear();
    surfaces.clear();
    if (gbm != nullptr)
      gbm_device_destroy(gbm);
  }
};

static Surface *BackingOwner(Driver *driver, Surface *surface) {
  if (surface == nullptr || surface->backing_owner == VA_INVALID_SURFACE)
    return surface;
  auto owner = driver->surfaces.find(surface->backing_owner);
  return owner != driver->surfaces.end() ? owner->second.get() : surface;
}

static void SetSurfaceState(Driver *driver, Surface *surface, bool ready,
                            bool failed) {
  if (surface == nullptr)
    return;
  surface->ready = ready;
  surface->failed = failed;
  Surface *owner = BackingOwner(driver, surface);
  if (owner != surface) {
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

static bool IsValidConfig(VAProfile profile, VAEntrypoint entrypoint) {
  return (IsH264Profile(profile) && entrypoint == VAEntrypointVLD) ||
         (profile == VAProfileNone && entrypoint == VAEntrypointVideoProc);
}

static VAStatus OpenDecoder(DecodeContext *decode) {
  BC_INPUT_FORMAT input = {};
  BC_INFO_CRYSTAL version = {};
  uint32_t mode = DTS_PLAYBACK_MODE | DTS_LOAD_FILE_PLAY_FW |
                  DTS_SKIP_TX_CHK_CPB | DTS_PLAYBACK_DROP_RPT_MODE |
                  DTS_SINGLE_THREADED_MODE |
                  DTS_DFLT_RESOLUTION(vdecRESOLUTION_1080p23_976);

  BC_STATUS status = DtsDeviceOpen(&decode->device, mode);
  Debug("DtsDeviceOpen: %d", status);
  if (status != BC_STS_SUCCESS) {
    if (status == BC_STS_BUSY || status == BC_STS_DEC_EXIST_OPEN)
      return VA_STATUS_ERROR_HW_BUSY;
    return VA_STATUS_ERROR_OPERATION_FAILED;
  }

  status = DtsCrystalHDVersion(decode->device, &version);
  if (status != BC_STS_SUCCESS)
    goto fail;
  decode->is_70012 = version.device == 0;

  input.FGTEnable = FALSE;
  input.Progressive = TRUE;
  // Do not set mpcOutPutMaxFRate (bit 6). At 720p it forces the firmware's
  // maximum cadence even for a 30 fps stream, creating frames Chromium must
  // discard. The reconstructed SPS VUI and this fallback now both say 30 fps.
  input.OptFlags = 0x80000000U | vdecFrameRate30;
  input.mSubtype = BC_MSUBTYPE_H264;
  input.width = decode->width;
  input.height = decode->height;
  input.startCodeSz = 4;

  status = DtsSetInputFormat(decode->device, &input);
  if (status != BC_STS_SUCCESS)
    goto fail;
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
  return VA_STATUS_SUCCESS;

fail:
  decode->Close();
  return VA_STATUS_ERROR_OPERATION_FAILED;
}

static void CopyYuy2ToSurface(Surface *surface, const BC_DTS_PROC_OUT &output,
                              bool is_70012) {
  const unsigned int width = std::min<unsigned int>(output.PicInfo.width,
                                                     surface->width);
  const unsigned int height = std::min<unsigned int>(output.PicInfo.height,
                                                      surface->height);
  unsigned int source_pitch = width * 2;
  if (is_70012) {
    const unsigned int padded_width =
        width <= 720 ? 720 : (width <= 1280 ? 1280 : 1920);
    source_pitch = padded_width * 2;
  }

  surface->BeginCpuWrite();
  if (surface->fourcc == VA_FOURCC_ARGB) {
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
    surface->EndCpuWrite();
    surface->ready = true;
    surface->frame_timestamp = output.PicInfo.timeStamp;
    return;
  }
  for (unsigned int y = 0; y < height; ++y) {
    const uint8_t *source = output.Ybuff +
                            static_cast<size_t>(y) * source_pitch;
    uint8_t *destination = surface->planes[0] + static_cast<size_t>(y) *
                                                   surface->pitch[0];
    for (unsigned int x = 0; x < width; ++x)
      destination[x] = source[x * 2];
  }

  for (unsigned int y = 0; y < height; y += 2) {
    const uint8_t *top = output.Ybuff + static_cast<size_t>(y) * source_pitch;
    const uint8_t *bottom = output.Ybuff +
                            static_cast<size_t>(std::min(y + 1, height - 1)) *
                                source_pitch;
    uint8_t *destination = surface->planes[1] +
                           static_cast<size_t>(y / 2) * surface->pitch[1];
    for (unsigned int x = 0; x + 1 < width; x += 2) {
      destination[x] = static_cast<uint8_t>(
          (static_cast<unsigned int>(top[x * 2 + 1]) + bottom[x * 2 + 1] + 1) /
          2);
      destination[x + 1] = static_cast<uint8_t>(
          (static_cast<unsigned int>(top[x * 2 + 3]) + bottom[x * 2 + 3] + 1) /
          2);
    }
  }
  surface->EndCpuWrite();
  surface->ready = true;
  surface->frame_timestamp = output.PicInfo.timeStamp;
}

static void CopyNv12Surface(const Surface &source, Surface *destination) {
  if (destination == nullptr || source.fourcc != VA_FOURCC_NV12 ||
      destination->fourcc != VA_FOURCC_NV12)
    return;
  const unsigned int width = std::min(source.width, destination->width);
  const unsigned int height = std::min(source.height, destination->height);
  source.BeginCpuRead();
  destination->BeginCpuWrite();
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
  destination->EndCpuWrite();
  source.EndCpuRead();
  destination->ready = true;
  destination->failed = false;
  destination->frame_timestamp = source.frame_timestamp;
}

static VAStatus ReceiveOne(Driver *driver, DecodeContext *decode,
                           unsigned int timeout_ms, bool *activity) {
  BC_DTS_PROC_OUT output = {};
  output.PicInfo.width = decode->width;
  output.PicInfo.height = decode->height;
  *activity = false;

  BC_STATUS status = DtsProcOutputNoCopy(decode->device, timeout_ms, &output);
  Debug("DtsProcOutputNoCopy: status=%d flags=%#x ts=%llu", status,
        output.PoutFlags,
        static_cast<unsigned long long>(output.PicInfo.timeStamp));
  if (status == BC_STS_FMT_CHANGE) {
    *activity = true;
    return VA_STATUS_SUCCESS;
  }
  if (status == BC_STS_NO_DATA || status == BC_STS_BUSY ||
      status == BC_STS_TIMEOUT)
    return VA_STATUS_SUCCESS;
  if (status != BC_STS_SUCCESS)
    return VA_STATUS_ERROR_DECODING_ERROR;

  *activity = true;
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
      static_cast<uint64_t>(padded_width) * 2 * output_height;
  const bool valid_frame =
      (output.PoutFlags & BC_POUT_FLAGS_PIB_VALID) != 0 &&
      output.Ybuff != nullptr && output_width != 0 && output_height != 0 &&
      static_cast<uint64_t>(output.YBuffDoneSz) * 4 >= required_bytes &&
      (output.PicInfo.flags & VDEC_FLAG_INTERLACED_SRC) == 0;
  auto decoded = decode->decoded_frames.find(output.PicInfo.timeStamp);
  if (decoded != decode->decoded_frames.end()) {
    if (!valid_frame)
      decoded->second->failed = true;
    else
      CopyYuy2ToSurface(decoded->second.get(), output, decode->is_70012);
  }
  auto surface = driver->surfaces.find(surface_id);
  if (surface != driver->surfaces.end() &&
      surface->second->expected_timestamp == output.PicInfo.timeStamp) {
    if (!valid_frame)
      surface->second->failed = true;
    else if (decoded != decode->decoded_frames.end())
      CopyNv12Surface(*decoded->second, surface->second.get());
    else
      CopyYuy2ToSurface(surface->second.get(), output, decode->is_70012);
  }
  driver->condition.notify_all();

  status = DtsReleaseOutputBuffs(decode->device, nullptr, FALSE);
  return status == BC_STS_SUCCESS ? VA_STATUS_SUCCESS
                                  : VA_STATUS_ERROR_DECODING_ERROR;
}

static VAStatus ReceiveAvailable(Driver *driver, DecodeContext *decode) {
  for (unsigned int attempt = 0; attempt < 64; ++attempt) {
    BC_DTS_STATUS decoder_status = {};
    BC_STATUS status = DtsGetDriverStatus(decode->device, &decoder_status);
    if (status != BC_STS_SUCCESS || decoder_status.ReadyListCount != 0)
      Debug("DtsGetDriverStatus: status=%d ready=%u", status,
            decoder_status.ReadyListCount);
    if (status != BC_STS_SUCCESS)
      return VA_STATUS_ERROR_DECODING_ERROR;
    if (decoder_status.ReadyListCount == 0)
      return VA_STATUS_SUCCESS;
    bool activity = false;
    VAStatus va_status = ReceiveOne(driver, decode, 0, &activity);
    if (va_status != VA_STATUS_SUCCESS || !activity)
      return va_status;
  }
  return VA_STATUS_SUCCESS;
}

static VAStatus SubmitPicture(Driver *driver, DecodeContext *decode,
                              VAProfile profile) {
  if (!decode->have_picture || decode->slices.empty() ||
      decode->slice_data.empty() || decode->target == VA_INVALID_SURFACE)
    return VA_STATUS_ERROR_INVALID_PARAMETER;

  auto surface = driver->surfaces.find(decode->target);
  if (surface == driver->surfaces.end())
    return VA_STATUS_ERROR_INVALID_SURFACE;

  std::vector<uint8_t> bitstream;
  const bool idr = !decode->slice_data.front().empty() &&
                   (decode->slice_data.front().front() & 0x1f) == 5;
  if (idr && decode->decoder_started && !decode->pending.empty()) {
    Debug("reset decoder generation=%llu at discontinuous IDR with %zu "
          "pending pictures",
          static_cast<unsigned long long>(decode->generation),
          decode->pending.size());
    // Complete old VA surfaces as neutral placeholders. Chromium may still
    // synchronize them while it retires the pre-seek pipeline; returning a
    // decode error here makes the player restore the old media time.
    for (const auto &pending : decode->pending) {
      auto old_surface = driver->surfaces.find(pending.second);
      if (old_surface == driver->surfaces.end())
        continue;
      old_surface->second->ready = true;
      old_surface->second->failed = false;
      old_surface->second->frame_timestamp =
          old_surface->second->expected_timestamp;
    }
    const BC_STATUS flush_status = decode->FlushDiscontinuity();
    if (flush_status != BC_STS_SUCCESS) {
      Debug("DtsFlushInput mode 4 failed: %d", flush_status);
      decode->Reset();
    }
    driver->vpp_fallback.clear();
    driver->vpp_fallback_width = 0;
    driver->vpp_fallback_height = 0;
    driver->vpp_fallback_sequence = 0;
    driver->condition.notify_all();
  }
  if (!decode->sent_parameter_sets || idr) {
    if (!BuildSps(decode->picture, profile, &bitstream))
      return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
  }
  // VA-API exposes the active reference counts on each slice rather than the
  // original PPS defaults. Redefine PPS id 0 before every access unit so a
  // slice without num_ref_idx_active_override_flag is parsed consistently.
  if (!BuildPps(decode->picture, decode->slices.front(), profile, &bitstream))
    return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;

  static constexpr uint8_t start_code[] = {0, 0, 0, 1};
  for (const std::vector<uint8_t> &slice : decode->slice_data) {
    bitstream.insert(bitstream.end(), std::begin(start_code), std::end(start_code));
    bitstream.insert(bitstream.end(), slice.begin(), slice.end());
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

  if (!decode->decoder_started) {
    VAStatus status = OpenDecoder(decode);
    if (status != VA_STATUS_SUCCESS)
      return status;
  }

  const uint64_t timestamp = decode->next_timestamp;
  decode->next_timestamp += kTimestampStep;
  auto decoded_frame = std::make_shared<Surface>();
  if (!decoded_frame->AllocateInternal(nullptr, -1, surface->second->width,
                                       surface->second->height,
                                       VA_FOURCC_NV12))
    return VA_STATUS_ERROR_ALLOCATION_FAILED;
  decoded_frame->rt_format = VA_RT_FORMAT_YUV420;
  decoded_frame->expected_timestamp = timestamp;
  decode->decoded_frames[timestamp] = decoded_frame;
  decode->surface_timestamps[surface->second.get()] = timestamp;
  decode->pending[timestamp] = decode->target;
  surface->second->ready = false;
  surface->second->failed = false;
  surface->second->expected_timestamp = timestamp;
  surface->second->frame_timestamp = 0;

  BC_STATUS input_status = BC_STS_BUSY;
  for (unsigned int attempt = 0; attempt < kInputRetries; ++attempt) {
    input_status = DtsProcInput(decode->device, bitstream.data(), bitstream.size(),
                                timestamp, FALSE);
    if (input_status != BC_STS_BUSY)
      break;
    VAStatus status = ReceiveAvailable(driver, decode);
    if (status != VA_STATUS_SUCCESS)
      return status;
    usleep(1000);
  }
  if (input_status != BC_STS_SUCCESS) {
    decode->pending.erase(timestamp);
    decode->decoded_frames.erase(timestamp);
    surface->second->failed = true;
    return VA_STATUS_ERROR_DECODING_ERROR;
  }
  decode->sent_parameter_sets = true;
  return ReceiveAvailable(driver, decode);
}

static VAStatus SyncDecodeSurface(
    Driver *driver, const std::shared_ptr<Surface> &surface,
    std::unique_lock<std::mutex> *driver_lock, uint64_t timeout_ns,
    const std::function<bool()> &canceled = {}) {
  if (driver_lock == nullptr || !driver_lock->owns_lock())
    return VA_STATUS_ERROR_INVALID_PARAMETER;
  if (surface->ready) {
    if (surface->expected_timestamp == 0 ||
        surface->frame_timestamp == surface->expected_timestamp)
      return VA_STATUS_SUCCESS;
    Debug("frame identity mismatch expected=%llu produced=%llu",
          static_cast<unsigned long long>(surface->expected_timestamp),
          static_cast<unsigned long long>(surface->frame_timestamp));
    surface->failed = true;
    return VA_STATUS_ERROR_DECODING_ERROR;
  }
  if (surface->failed)
    return VA_STATUS_ERROR_DECODING_ERROR;

  std::shared_ptr<DecodeContext> decode;
  for (auto &entry : driver->contexts) {
    if (!entry.second->decoder_started)
      continue;
    for (const auto &pending : entry.second->pending) {
      auto candidate = driver->surfaces.find(pending.second);
      if (candidate != driver->surfaces.end() &&
          candidate->second == surface) {
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

  Debug("sync surface ready=%d failed=%d timeout=%llu", surface->ready,
        surface->failed, static_cast<unsigned long long>(timeout_ns));

  const auto start = std::chrono::steady_clock::now();
  const bool infinite = timeout_ns == VA_TIMEOUT_INFINITE;
  while (!surface->ready && !surface->failed) {
    if (driver->stopping)
      return VA_STATUS_ERROR_OPERATION_FAILED;
    if (canceled && canceled())
      return VA_STATUS_ERROR_OPERATION_FAILED;
    VAStatus status = ReceiveAvailable(driver, decode.get());
    if (status != VA_STATUS_SUCCESS)
      return status;
    if (surface->ready || surface->failed)
      break;
    uint64_t elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();
    if (!infinite) {
      if (elapsed >= timeout_ns)
        return VA_STATUS_ERROR_TIMEDOUT;
    }
    // CrystalHD may require later compressed pictures before it emits this
    // exact timestamp. Let the decoder submission thread enter the driver;
    // holding the global lock here would deadlock input behind VPP output.
    driver_lock->unlock();
    usleep(1000);
    driver_lock->lock();
    if (canceled && canceled())
      return VA_STATUS_ERROR_OPERATION_FAILED;
    if (decode->retired)
      return VA_STATUS_ERROR_INVALID_CONTEXT;
  }
  if (!surface->ready)
    return VA_STATUS_ERROR_DECODING_ERROR;
  if (surface->expected_timestamp != 0 &&
      surface->frame_timestamp != surface->expected_timestamp) {
    Debug("frame identity mismatch expected=%llu produced=%llu",
          static_cast<unsigned long long>(surface->expected_timestamp),
          static_cast<unsigned long long>(surface->frame_timestamp));
    surface->failed = true;
    return VA_STATUS_ERROR_DECODING_ERROR;
  }
  return VA_STATUS_SUCCESS;
}

static bool PendingVppCanceled(const PendingVpp &operation) {
  return operation.source->destroyed || operation.target->destroyed ||
         operation.target_owner->destroyed ||
         operation.target_owner->latest_vpp_sequence != operation.sequence ||
         (operation.decoder &&
          (operation.decoder->retired ||
           operation.decoder->generation != operation.decoder_generation));
}

static void RecordVppFallback(Driver *driver, const Surface *target,
                              uint64_t sequence, VAStatus status) {
  if (driver == nullptr || target == nullptr || status != VA_STATUS_SUCCESS ||
      target->fourcc != VA_FOURCC_ARGB ||
      sequence <= driver->vpp_fallback_sequence)
    return;
  const size_t required =
      static_cast<size_t>(target->width) * target->height * 4;
  if (driver->vpp_argb_staging.size() != required)
    return;
  driver->vpp_fallback = driver->vpp_argb_staging;
  driver->vpp_fallback_width = target->width;
  driver->vpp_fallback_height = target->height;
  driver->vpp_fallback_sequence = sequence;
}

static void WriteVppFallback(Driver *driver, Surface *target) {
  if (driver == nullptr || target == nullptr)
    return;
  if (target->fourcc == VA_FOURCC_ARGB) {
    const size_t row_bytes = static_cast<size_t>(target->width) * 4;
    const bool have_frame =
        driver->vpp_fallback_width == target->width &&
        driver->vpp_fallback_height == target->height &&
        driver->vpp_fallback.size() == row_bytes * target->height;
    for (unsigned int row = 0; row < target->height; ++row) {
      uint8_t *destination = target->planes[0] +
          static_cast<size_t>(row) * target->pitch[0];
      if (have_frame) {
        memcpy(destination,
               driver->vpp_fallback.data() +
                   static_cast<size_t>(row) * row_bytes,
               row_bytes);
      } else {
        memset(destination, 0, row_bytes);
        for (unsigned int column = 0; column < target->width; ++column)
          destination[column * 4 + 3] = 255;
      }
    }
    return;
  }
  if (target->fourcc == VA_FOURCC_NV12) {
    for (unsigned int row = 0; row < target->height; ++row)
      memset(target->planes[0] + static_cast<size_t>(row) * target->pitch[0],
             16, target->width);
    for (unsigned int row = 0; row < (target->height + 1) / 2; ++row)
      memset(target->planes[1] + static_cast<size_t>(row) * target->pitch[1],
             128, target->width);
  }
}

// Release bookkeeping for one queued conversion. The caller holds the driver
// mutex. A superseded operation still receives a complete fallback image
// before its fence is signaled; exposing untouched pool memory is never safe.
static void ReleasePendingVpp(Driver *driver, uint64_t sequence,
                              VAStatus status, bool committed) {
  auto found = std::find_if(
      driver->pending_vpp.begin(), driver->pending_vpp.end(),
      [&](const PendingVpp &pending) { return pending.sequence == sequence; });
  if (found == driver->pending_vpp.end())
    return;
  const PendingVpp operation = *found;
  if (!committed && !operation.target->destroyed &&
      !operation.target_owner->destroyed) {
    WriteVppFallback(driver, operation.target.get());
  }
  // Flush CPU caches before signaling the fence imported into Chromium's
  // DMA-BUF. Every implicit GPU reader remains blocked until this point.
  EndFencedWrite(operation.target.get(), operation.write_timeline);
  if (operation.source->vpp_readers != 0)
    --operation.source->vpp_readers;
  if (operation.decoder) {
    auto decoded = operation.decoder->decoded_frames.find(
        operation.source_timestamp);
    if (decoded != operation.decoder->decoded_frames.end() &&
        decoded->second == operation.source &&
        operation.source->vpp_readers == 0) {
      operation.decoder->decoded_frames.erase(decoded);
    }
  }
  if (operation.target_owner->vpp_writers != 0)
    --operation.target_owner->vpp_writers;
  if (committed && operation.target_owner->vpp_writers == 0) {
    SetSurfaceState(driver, operation.target.get(),
                    status == VA_STATUS_SUCCESS,
                    status != VA_STATUS_SUCCESS);
  }
  Debug("%s VPP sequence=%llu source_timestamp=%llu target=%p status=%d",
        committed ? "committed" : "discarded",
        static_cast<unsigned long long>(operation.sequence),
        static_cast<unsigned long long>(operation.source->frame_timestamp),
        static_cast<void *>(operation.target.get()), status);
  driver->pending_vpp.erase(found);
  driver->condition.notify_all();
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
                                 });
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
                          operation.source.get(), operation.target.get(),
                          operation.source_region, operation.output_region,
                          true);
      RecordVppFallback(driver, operation.target.get(), operation.sequence,
                        status);
    }
    ReleasePendingVpp(driver, operation.sequence, status, true);
  }
}

static VAStatus Terminate(VADriverContextP context) {
  delete GetDriver(context);
  context->pDriverData = nullptr;
  return VA_STATUS_SUCCESS;
}

static VAStatus QueryConfigProfiles(VADriverContextP, VAProfile *profiles,
                                    int *count) {
  if (profiles == nullptr || count == nullptr)
    return VA_STATUS_ERROR_INVALID_PARAMETER;
  profiles[0] = VAProfileH264ConstrainedBaseline;
  profiles[1] = VAProfileH264Main;
  profiles[2] = VAProfileH264High;
  profiles[3] = VAProfileNone;
  *count = 4;
  return VA_STATUS_SUCCESS;
}

static VAStatus QueryConfigEntrypoints(VADriverContextP, VAProfile profile,
                                       VAEntrypoint *entrypoints, int *count) {
  if (profile != VAProfileNone && !IsH264Profile(profile))
    return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
  if (entrypoints == nullptr || count == nullptr)
    return VA_STATUS_ERROR_INVALID_PARAMETER;
  entrypoints[0] = profile == VAProfileNone ? VAEntrypointVideoProc
                                           : VAEntrypointVLD;
  *count = 1;
  return VA_STATUS_SUCCESS;
}

static VAStatus GetConfigAttributes(VADriverContextP, VAProfile profile,
                                    VAEntrypoint entrypoint,
                                    VAConfigAttrib *attributes, int count) {
  if (profile != VAProfileNone && !IsH264Profile(profile))
    return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
  if (!IsValidConfig(profile, entrypoint))
    return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
  for (int i = 0; i < count; ++i) {
    switch (attributes[i].type) {
      case VAConfigAttribRTFormat:
        attributes[i].value = IsH264Profile(profile)
                                  ? VA_RT_FORMAT_YUV420
                                  : VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_RGB32;
        break;
      case VAConfigAttribDecSliceMode:
        attributes[i].value = IsH264Profile(profile)
                                  ? VA_DEC_SLICE_MODE_NORMAL
                                  : VA_ATTRIB_NOT_SUPPORTED;
        break;
      case VAConfigAttribMaxPictureWidth:
        attributes[i].value = kMaxWidth;
        break;
      case VAConfigAttribMaxPictureHeight:
        attributes[i].value = kMaxHeight;
        break;
      default:
        attributes[i].value = VA_ATTRIB_NOT_SUPPORTED;
        break;
    }
  }
  return VA_STATUS_SUCCESS;
}

static VAStatus CreateConfig(VADriverContextP context, VAProfile profile,
                             VAEntrypoint entrypoint, VAConfigAttrib *, int,
                             VAConfigID *config_id) {
  if (profile != VAProfileNone && !IsH264Profile(profile))
    return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
  if (!IsValidConfig(profile, entrypoint))
    return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
  Driver *driver = GetDriver(context);
  std::lock_guard<std::mutex> lock(driver->mutex);
  *config_id = driver->next_config++;
  driver->configs[*config_id] = {profile, entrypoint};
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
  Driver *driver = GetDriver(context);
  std::lock_guard<std::mutex> lock(driver->mutex);
  auto config = driver->configs.find(config_id);
  if (config == driver->configs.end())
    return VA_STATUS_ERROR_INVALID_CONFIG;
  *profile = config->second.profile;
  *entrypoint = config->second.entrypoint;
  if (attributes != nullptr) {
    attributes[0].type = VAConfigAttribRTFormat;
    attributes[0].value = IsH264Profile(config->second.profile)
                              ? VA_RT_FORMAT_YUV420
                              : VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_RGB32;
  }
  *count = 1;
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
  Driver *driver = GetDriver(context);
  std::lock_guard<std::mutex> lock(driver->mutex);
  std::vector<VASurfaceID> created;
  for (unsigned int i = 0; i < surface_count; ++i) {
    auto surface = std::make_shared<Surface>();
    surface->width = width;
    surface->height = height;
    surface->rt_format = format;
    surface->fourcc = pixel_format;
    bool ok = false;
    if (memory_type == VA_SURFACE_ATTRIB_MEM_TYPE_VA) {
      ok = surface->AllocateInternal(driver->gbm, driver->drm_fd, width, height,
                                     pixel_format);
    } else if (memory_type == VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2 &&
               descriptor != nullptr) {
      const auto *descriptors =
          static_cast<const VADRMPRIMESurfaceDescriptor *>(descriptor);
      ok = surface->ImportPrime(driver->gbm, descriptors[i]);
    } else if (memory_type == VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME &&
               descriptor != nullptr) {
      const auto *descriptors =
          static_cast<const VASurfaceAttribExternalBuffers *>(descriptor);
      ok = surface->ImportLegacy(descriptors[i]);
    }
    if (!ok) {
      for (VASurfaceID id : created)
        driver->surfaces.erase(id);
      return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    int backing_fd = -1;
    if (memory_type == VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2 &&
        descriptor != nullptr) {
      const auto *descriptors =
          static_cast<const VADRMPRIMESurfaceDescriptor *>(descriptor);
      backing_fd = descriptors[i].objects[0].fd;
    } else if (memory_type == VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME &&
               descriptor != nullptr) {
      const auto *descriptors =
          static_cast<const VASurfaceAttribExternalBuffers *>(descriptor);
      backing_fd = static_cast<int>(descriptors[i].buffers[0]);
    }
    BackingIdentity identity;
    if (GetBackingIdentity(backing_fd, &identity)) {
      auto owner = driver->backing_owners.find(identity);
      if (owner != driver->backing_owners.end()) {
        surface->backing_owner = owner->second;
        auto owner_surface = driver->surfaces.find(owner->second);
        if (owner_surface != driver->surfaces.end()) {
          surface->ready = owner_surface->second->ready;
          surface->failed = owner_surface->second->failed;
          surface->expected_timestamp =
              owner_surface->second->expected_timestamp;
          surface->frame_timestamp = owner_surface->second->frame_timestamp;
        }
      }
    }
    if (surface->backing_owner == VA_INVALID_SURFACE)
      surface->ready = true;
    VASurfaceID id = driver->next_surface++;
    driver->surfaces[id] = std::move(surface);
    surface_ids[i] = id;
    created.push_back(id);
  }
  return VA_STATUS_SUCCESS;
}

static VAStatus CreateSurfaces(VADriverContextP context, int width, int height,
                               int format, int surface_count,
                               VASurfaceID *surface_ids) {
  return CreateSurfaces2(context, format, width, height, surface_ids,
                         surface_count, nullptr, 0);
}

static VAStatus DestroySurfaces(VADriverContextP context,
                                VASurfaceID *surface_ids, int count) {
  Driver *driver = GetDriver(context);
  std::lock_guard<std::mutex> lock(driver->mutex);
  std::unordered_set<DecodeContext *> reset_decoders;
  for (int i = 0; i < count; ++i) {
    auto surface = driver->surfaces.find(surface_ids[i]);
    if (surface == driver->surfaces.end())
      return VA_STATUS_ERROR_INVALID_SURFACE;
    for (const auto &entry : driver->contexts) {
      if (entry.second->video_process)
        continue;
      if (entry.second->surface_timestamps.find(surface->second.get()) !=
          entry.second->surface_timestamps.end()) {
        reset_decoders.insert(entry.second.get());
      }
    }
    // A queued worker owns a shared_ptr after the VA ID is gone. Permanently
    // poison that object before erasing it so it can never write an old frame
    // into a DMA-BUF that Chromium recycles for a post-seek surface.
    surface->second->destroyed = true;
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
  for (DecodeContext *decode : reset_decoders) {
    Debug("reset decoder generation=%llu after decode-surface teardown",
          static_cast<unsigned long long>(decode->generation));
    decode->Reset();
  }
  if (!reset_decoders.empty()) {
    driver->vpp_fallback.clear();
    driver->vpp_fallback_width = 0;
    driver->vpp_fallback_height = 0;
    driver->vpp_fallback_sequence = 0;
  }
  driver->condition.notify_all();
  return VA_STATUS_SUCCESS;
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

  const bool video_process = config->second.entrypoint == VAEntrypointVideoProc;
  const unsigned int required = video_process ? 7 : 6;
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
  attributes[0] = integer_attribute(
      VASurfaceAttribPixelFormat,
      VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE, VA_FOURCC_NV12);
  unsigned int index = 1;
  if (video_process) {
    attributes[index++] = integer_attribute(
        VASurfaceAttribPixelFormat,
        VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE,
        VA_FOURCC_ARGB);
  }
  attributes[index++] = integer_attribute(VASurfaceAttribMinWidth,
                                          VA_SURFACE_ATTRIB_GETTABLE, 16);
  attributes[index++] = integer_attribute(VASurfaceAttribMaxWidth,
                                          VA_SURFACE_ATTRIB_GETTABLE,
                                          kMaxWidth);
  attributes[index++] = integer_attribute(VASurfaceAttribMinHeight,
                                          VA_SURFACE_ATTRIB_GETTABLE, 16);
  attributes[index++] = integer_attribute(VASurfaceAttribMaxHeight,
                                          VA_SURFACE_ATTRIB_GETTABLE,
                                          kMaxHeight);
  attributes[index] = integer_attribute(
      VASurfaceAttribMemoryType,
      VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE,
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

static VAStatus QueryVideoProcFilters(VADriverContextP context,
                                      VAContextID context_id,
                                      VAProcFilterType *,
                                      unsigned int *filter_count) {
  if (filter_count == nullptr)
    return VA_STATUS_ERROR_INVALID_PARAMETER;
  Driver *driver = GetDriver(context);
  std::lock_guard<std::mutex> lock(driver->mutex);
  if (!IsVideoProcessContext(driver, context_id))
    return VA_STATUS_ERROR_INVALID_CONTEXT;
  *filter_count = 0;
  return VA_STATUS_SUCCESS;
}

static VAStatus QueryVideoProcFilterCaps(VADriverContextP context,
                                         VAContextID context_id,
                                         VAProcFilterType, void *,
                                         unsigned int *capability_count) {
  if (capability_count == nullptr)
    return VA_STATUS_ERROR_INVALID_PARAMETER;
  Driver *driver = GetDriver(context);
  std::lock_guard<std::mutex> lock(driver->mutex);
  if (!IsVideoProcessContext(driver, context_id))
    return VA_STATUS_ERROR_INVALID_CONTEXT;
  *capability_count = 0;
  return VA_STATUS_ERROR_UNSUPPORTED_FILTER;
}

static VAStatus QueryVideoProcPipelineCaps(VADriverContextP context,
                                           VAContextID context_id,
                                           VABufferID *,
                                           unsigned int filter_count,
                                           VAProcPipelineCaps *caps) {
  if (caps == nullptr || filter_count != 0)
    return VA_STATUS_ERROR_INVALID_PARAMETER;
  Driver *driver = GetDriver(context);
  std::lock_guard<std::mutex> lock(driver->mutex);
  if (!IsVideoProcessContext(driver, context_id))
    return VA_STATUS_ERROR_INVALID_CONTEXT;

  uint32_t *input_formats = caps->input_pixel_format;
  const uint32_t input_capacity = caps->num_input_pixel_formats;
  uint32_t *output_formats = caps->output_pixel_format;
  const uint32_t output_capacity = caps->num_output_pixel_formats;
  memset(caps, 0, sizeof(*caps));
  caps->input_pixel_format = input_formats;
  caps->output_pixel_format = output_formats;
  caps->num_input_pixel_formats = 1;
  caps->num_output_pixel_formats = 2;
  if ((input_formats != nullptr && input_capacity < 1) ||
      (output_formats != nullptr && output_capacity < 2))
    return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
  if (input_formats != nullptr)
    input_formats[0] = VA_FOURCC_NV12;
  if (output_formats != nullptr) {
    output_formats[0] = VA_FOURCC_NV12;
    output_formats[1] = VA_FOURCC_ARGB;
  }
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
       static_cast<unsigned int>(height) > kMaxHeight))
    return VA_STATUS_ERROR_INVALID_PARAMETER;
  for (int i = 0; i < target_count; ++i) {
    if (driver->surfaces.find(targets[i]) == driver->surfaces.end())
      return VA_STATUS_ERROR_INVALID_SURFACE;
  }
  auto decode = std::make_shared<DecodeContext>();
  decode->config = config_id;
  decode->width = std::max(width, 0);
  decode->height = std::max(height, 0);
  decode->video_process = video_process;
  *context_id = driver->next_context++;
  driver->contexts[*context_id] = std::move(decode);
  return VA_STATUS_SUCCESS;
}

static VAStatus DestroyContext(VADriverContextP context,
                               VAContextID context_id) {
  Driver *driver = GetDriver(context);
  std::lock_guard<std::mutex> lock(driver->mutex);
  auto found = driver->contexts.find(context_id);
  if (found == driver->contexts.end())
    return VA_STATUS_ERROR_INVALID_CONTEXT;
  found->second->retired = true;
  found->second->Close();
  driver->contexts.erase(found);
  // Chrome's asynchronous image processor can race context teardown with one
  // already-dispatched vaEndPicture call. Keep only the retired ID so that
  // completion can be acknowledged without retaining a decoder or device.
  driver->retired_contexts.insert(context_id);
  driver->retired_context_order.push_back(context_id);
  if (driver->retired_context_order.size() > 64) {
    driver->retired_contexts.erase(driver->retired_context_order.front());
    driver->retired_context_order.pop_front();
  }
  driver->condition.notify_all();
  return VA_STATUS_SUCCESS;
}

static VAStatus CreateBuffer(VADriverContextP context, VAContextID context_id,
                             VABufferType type, unsigned int element_size,
                             unsigned int elements, void *data,
                             VABufferID *buffer_id) {
  Driver *driver = GetDriver(context);
  std::lock_guard<std::mutex> lock(driver->mutex);
  if (context_id != VA_INVALID_ID &&
      driver->contexts.find(context_id) == driver->contexts.end())
    return VA_STATUS_ERROR_INVALID_CONTEXT;
  if (buffer_id == nullptr || element_size == 0 || elements == 0)
    return VA_STATUS_ERROR_INVALID_PARAMETER;
  Buffer buffer;
  buffer.context = context_id;
  buffer.type = type;
  buffer.element_size = element_size;
  buffer.elements = elements;
  buffer.data.resize(static_cast<size_t>(element_size) * elements);
  if (data != nullptr)
    memcpy(buffer.data.data(), data, buffer.data.size());
  *buffer_id = driver->next_buffer++;
  driver->buffers[*buffer_id] = std::move(buffer);
  return VA_STATUS_SUCCESS;
}

static VAStatus BufferSetNumElements(VADriverContextP context,
                                     VABufferID buffer_id,
                                     unsigned int elements) {
  Driver *driver = GetDriver(context);
  std::lock_guard<std::mutex> lock(driver->mutex);
  auto buffer = driver->buffers.find(buffer_id);
  if (buffer == driver->buffers.end())
    return VA_STATUS_ERROR_INVALID_BUFFER;
  buffer->second.data.resize(static_cast<size_t>(buffer->second.element_size) *
                             elements);
  buffer->second.elements = elements;
  return VA_STATUS_SUCCESS;
}

static VAStatus MapBuffer(VADriverContextP context, VABufferID buffer_id,
                          void **data) {
  Driver *driver = GetDriver(context);
  std::lock_guard<std::mutex> lock(driver->mutex);
  auto buffer = driver->buffers.find(buffer_id);
  if (buffer == driver->buffers.end() || data == nullptr)
    return VA_STATUS_ERROR_INVALID_BUFFER;
  *data = buffer->second.data.data();
  return VA_STATUS_SUCCESS;
}

#if VA_CHECK_VERSION(1, 21, 0)
static VAStatus MapBuffer2(VADriverContextP context, VABufferID buffer_id,
                           void **data, uint32_t) {
  return MapBuffer(context, buffer_id, data);
}
#endif

static VAStatus UnmapBuffer(VADriverContextP context, VABufferID buffer_id) {
  Driver *driver = GetDriver(context);
  std::lock_guard<std::mutex> lock(driver->mutex);
  return driver->buffers.find(buffer_id) != driver->buffers.end()
             ? VA_STATUS_SUCCESS
             : VA_STATUS_ERROR_INVALID_BUFFER;
}

static VAStatus DestroyBuffer(VADriverContextP context, VABufferID buffer_id) {
  Driver *driver = GetDriver(context);
  std::lock_guard<std::mutex> lock(driver->mutex);
  return driver->buffers.erase(buffer_id) != 0
             ? VA_STATUS_SUCCESS
             : VA_STATUS_ERROR_INVALID_BUFFER;
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
  if (decode == driver->contexts.end())
    return VA_STATUS_ERROR_INVALID_CONTEXT;
  auto target_surface = driver->surfaces.find(target);
  if (target_surface == driver->surfaces.end())
    return VA_STATUS_ERROR_INVALID_SURFACE;
  decode->second->target = target;
  if (decode->second->video_process) {
    // Chromium renders into an imported alias of an exported ARGB surface.
    // Track readiness on both aliases until VPP commits the exact frame.
    target_surface->second->expected_timestamp = 0;
    target_surface->second->frame_timestamp = 0;
    Surface *target_owner = BackingOwner(driver, target_surface->second.get());
    target_owner->expected_timestamp = 0;
    target_owner->frame_timestamp = 0;
    SetSurfaceState(driver, target_surface->second.get(), false, false);
    decode->second->vpp_source = VA_INVALID_SURFACE;
    decode->second->have_vpp_parameters = false;
    return VA_STATUS_SUCCESS;
  }
  decode->second->have_picture = false;
  decode->second->slices.clear();
  decode->second->slice_data.clear();
  return VA_STATUS_SUCCESS;
}

static VAStatus RenderPicture(VADriverContextP context, VAContextID context_id,
                              VABufferID *buffer_ids, int count) {
  Driver *driver = GetDriver(context);
  std::lock_guard<std::mutex> lock(driver->mutex);
  auto decode = driver->contexts.find(context_id);
  if (decode == driver->contexts.end())
    return VA_STATUS_ERROR_INVALID_CONTEXT;
  if (decode->second->video_process) {
    if (count != 1 || buffer_ids == nullptr)
      return VA_STATUS_ERROR_INVALID_PARAMETER;
    auto buffer = driver->buffers.find(buffer_ids[0]);
    if (buffer == driver->buffers.end() ||
        buffer->second.type != VAProcPipelineParameterBufferType ||
        buffer->second.data.size() < sizeof(VAProcPipelineParameterBuffer))
      return VA_STATUS_ERROR_INVALID_BUFFER;
    const auto *parameters =
        reinterpret_cast<const VAProcPipelineParameterBuffer *>(
            buffer->second.data.data());
    auto source = driver->surfaces.find(parameters->surface);
    auto target = driver->surfaces.find(decode->second->target);
    if (source == driver->surfaces.end() || target == driver->surfaces.end())
      return VA_STATUS_ERROR_INVALID_SURFACE;
    constexpr uint32_t supported_filter_flags =
        VA_FILTER_SCALING_DEFAULT | VA_FILTER_INTERPOLATION_NEAREST_NEIGHBOR;
    if (parameters->num_filters != 0 ||
        (parameters->filter_flags & ~supported_filter_flags) != 0 ||
        parameters->rotation_state != VA_ROTATION_NONE)
      return VA_STATUS_ERROR_UNSUPPORTED_FILTER;
    decode->second->vpp_source = parameters->surface;
    decode->second->vpp_source_region = parameters->surface_region != nullptr
        ? *parameters->surface_region
        : VARectangle{0, 0, static_cast<uint16_t>(source->second->width),
                      static_cast<uint16_t>(source->second->height)};
    decode->second->vpp_output_region = parameters->output_region != nullptr
        ? *parameters->output_region
        : VARectangle{0, 0, static_cast<uint16_t>(target->second->width),
                      static_cast<uint16_t>(target->second->height)};
    decode->second->have_vpp_parameters = true;
    return VA_STATUS_SUCCESS;
  }
  for (int i = 0; i < count; ++i) {
    auto buffer = driver->buffers.find(buffer_ids[i]);
    if (buffer == driver->buffers.end())
      return VA_STATUS_ERROR_INVALID_BUFFER;
    switch (buffer->second.type) {
      case VAPictureParameterBufferType:
        if (buffer->second.data.size() < sizeof(VAPictureParameterBufferH264))
          return VA_STATUS_ERROR_INVALID_BUFFER;
        memcpy(&decode->second->picture, buffer->second.data.data(),
               sizeof(VAPictureParameterBufferH264));
        decode->second->have_picture = true;
        break;
      case VASliceParameterBufferType: {
        size_t slices = buffer->second.data.size() /
                        sizeof(VASliceParameterBufferH264);
        const auto *parameters = reinterpret_cast<const VASliceParameterBufferH264 *>(
            buffer->second.data.data());
        decode->second->slices.insert(decode->second->slices.end(), parameters,
                                      parameters + slices);
        break;
      }
      case VASliceDataBufferType:
        decode->second->slice_data.push_back(buffer->second.data);
        break;
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
  if (decode == driver->contexts.end() &&
      driver->retired_contexts.erase(context_id) != 0) {
    Debug("ignoring late end for retired context=%u", context_id);
    return VA_STATUS_SUCCESS;
  }
  if (decode == driver->contexts.end())
    return VA_STATUS_ERROR_INVALID_CONTEXT;
  const std::shared_ptr<DecodeContext> decode_context = decode->second;
  if (decode_context->video_process) {
    auto finish_vpp = [&](VAStatus status) {
      if (status != VA_STATUS_SUCCESS) {
        auto destination = driver->surfaces.find(decode_context->target);
        if (destination != driver->surfaces.end())
          SetSurfaceState(driver, destination->second.get(), false, true);
      }
      decode_context->target = VA_INVALID_SURFACE;
      decode_context->vpp_source = VA_INVALID_SURFACE;
      decode_context->have_vpp_parameters = false;
      return status;
    };
    if (!decode_context->have_vpp_parameters)
      return finish_vpp(VA_STATUS_ERROR_INVALID_PARAMETER);
    auto source = driver->surfaces.find(decode_context->vpp_source);
    auto target = driver->surfaces.find(decode_context->target);
    if (source == driver->surfaces.end() || target == driver->surfaces.end())
      return finish_vpp(VA_STATUS_ERROR_INVALID_SURFACE);
    std::shared_ptr<Surface> synchronized_source = source->second;
    if (source->second->backing_owner != VA_INVALID_SURFACE) {
      auto owner = driver->surfaces.find(source->second->backing_owner);
      if (owner != driver->surfaces.end())
        synchronized_source = owner->second;
    }
    const std::shared_ptr<Surface> target_surface = target->second;
    std::shared_ptr<Surface> target_owner = target_surface;
    if (target_surface->backing_owner != VA_INVALID_SURFACE) {
      auto owner = driver->surfaces.find(target_surface->backing_owner);
      if (owner != driver->surfaces.end())
        target_owner = owner->second;
    }
    const uint64_t timestamp = synchronized_source->expected_timestamp;
    std::shared_ptr<DecodeContext> source_decoder;
    std::shared_ptr<Surface> decoded_source;
    for (const auto &candidate : driver->contexts) {
      if (candidate.second->video_process)
        continue;
      auto source_timestamp =
          candidate.second->surface_timestamps.find(synchronized_source.get());
      if (source_timestamp == candidate.second->surface_timestamps.end() ||
          source_timestamp->second != timestamp)
        continue;
      auto frame = candidate.second->decoded_frames.find(timestamp);
      if (frame != candidate.second->decoded_frames.end()) {
        source_decoder = candidate.second;
        decoded_source = frame->second;
        break;
      }
    }
    if (!source_decoder || !decoded_source) {
      if (timestamp != 0) {
        // A discontinuity reset can retire this decode epoch while Chromium
        // still dispatches its already-planned VPP calls. Complete those
        // calls with a full fallback image; returning success without touching
        // the target exposes whatever stale pool frame it previously held.
        const uint64_t fallback_sequence = driver->next_vpp_sequence++;
        target_owner->latest_vpp_sequence = fallback_sequence;
        for (;;) {
          auto older = std::find_if(
              driver->pending_vpp.begin(), driver->pending_vpp.end(),
              [&](const PendingVpp &pending) {
                return pending.target_owner == target_owner;
              });
          if (older == driver->pending_vpp.end())
            break;
          ReleasePendingVpp(driver, older->sequence, VA_STATUS_SUCCESS,
                            false);
        }
        target_surface->BeginCpuWrite();
        WriteVppFallback(driver, target_surface.get());
        target_surface->EndCpuWrite();
        SetSurfaceState(driver, target_surface.get(), true, false);
        Debug("completed late VPP fallback sequence=%llu timestamp=%llu",
              static_cast<unsigned long long>(fallback_sequence),
              static_cast<unsigned long long>(timestamp));
        return finish_vpp(VA_STATUS_SUCCESS);
      }
      if (!synchronized_source->ready)
        return finish_vpp(VA_STATUS_ERROR_INVALID_SURFACE);
      decoded_source = synchronized_source;
    }

    const uint64_t sequence = driver->next_vpp_sequence++;
    target_owner->latest_vpp_sequence = sequence;

    // Chromium may recycle an output-pool buffer before CrystalHD has emitted
    // the source for its previous use. Supersede that operation, but let
    // ReleasePendingVpp atomically replace the buffer with the latest complete
    // frame (or post-reset black) before signaling its old fence.
    for (;;) {
      auto older = std::find_if(
          driver->pending_vpp.begin(), driver->pending_vpp.end(),
          [&](const PendingVpp &pending) {
            return pending.target_owner == target_owner &&
                   pending.sequence != sequence;
          });
      if (older == driver->pending_vpp.end())
        break;
      ReleasePendingVpp(driver, older->sequence, VA_STATUS_SUCCESS, false);
    }

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
                                         decode_context->vpp_output_region);
      SetSurfaceState(driver, target_surface.get(),
                      status == VA_STATUS_SUCCESS,
                      status != VA_STATUS_SUCCESS);
      RecordVppFallback(driver, target_surface.get(), sequence, status);
      if (source_decoder && decoded_source->vpp_readers == 0) {
        auto decoded = source_decoder->decoded_frames.find(timestamp);
        if (decoded != source_decoder->decoded_frames.end() &&
            decoded->second == decoded_source) {
          source_decoder->decoded_frames.erase(decoded);
        }
      }
      Debug("completed immediate VPP sequence=%llu source_timestamp=%llu "
            "target=%p status=%d",
            static_cast<unsigned long long>(sequence),
            static_cast<unsigned long long>(timestamp),
            static_cast<void *>(target_surface.get()), status);
      return finish_vpp(status);
    }

    // Chromium does not synchronize processed VA surfaces itself. Import an
    // unsignaled write fence into the destination DMA-BUF before returning;
    // the compositor can queue the buffer, but cannot sample it until the
    // worker has copied and flushed the complete frame.
    const int write_timeline = BeginFencedWrite(target_surface.get());
    if (write_timeline < 0) {
      Debug("could not fence asynchronous VPP target=%p",
            static_cast<void *>(target_surface.get()));
      return finish_vpp(VA_STATUS_ERROR_OPERATION_FAILED);
    }
    ++decoded_source->vpp_readers;
    ++target_owner->vpp_writers;
    PendingVpp pending;
    pending.decoder = source_decoder;
    pending.source = decoded_source;
    pending.target = target_surface;
    pending.target_owner = target_owner;
    pending.source_region = decode_context->vpp_source_region;
    pending.output_region = decode_context->vpp_output_region;
    pending.source_timestamp = timestamp;
    pending.sequence = sequence;
    pending.decoder_generation = source_decoder
                                     ? source_decoder->generation
                                     : 0;
    pending.write_timeline = write_timeline;
    driver->pending_vpp.push_back(std::move(pending));
    driver->condition.notify_all();
    Debug("queued fenced VPP sequence=%llu source_timestamp=%llu target=%p",
          static_cast<unsigned long long>(sequence),
          static_cast<unsigned long long>(timestamp),
          static_cast<void *>(target_surface.get()));
    return finish_vpp(VA_STATUS_SUCCESS);
  }
  auto config = driver->configs.find(decode_context->config);
  if (config == driver->configs.end())
    return VA_STATUS_ERROR_INVALID_CONFIG;
  VAStatus status = SubmitPicture(driver, decode_context.get(),
                                  config->second.profile);
  decode_context->target = VA_INVALID_SURFACE;
  decode_context->have_picture = false;
  decode_context->slices.clear();
  decode_context->slice_data.clear();
  return status;
}

static VAStatus SyncSurface2(VADriverContextP context, VASurfaceID surface_id,
                             uint64_t timeout_ns) {
  Driver *driver = GetDriver(context);
  std::unique_lock<std::mutex> lock(driver->mutex);
  auto surface = driver->surfaces.find(surface_id);
  if (surface == driver->surfaces.end())
    return VA_STATUS_ERROR_INVALID_SURFACE;
  std::shared_ptr<Surface> synchronized_surface = surface->second;
  if (surface->second->backing_owner != VA_INVALID_SURFACE) {
    auto owner = driver->surfaces.find(surface->second->backing_owner);
    if (owner != driver->surfaces.end())
      synchronized_surface = owner->second;
  }

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
        return VA_STATUS_ERROR_OPERATION_FAILED;

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
        return synchronized_surface->ready ? VA_STATUS_SUCCESS
                                           : VA_STATUS_ERROR_DECODING_ERROR;
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
        return VA_STATUS_ERROR_TIMEDOUT;
      driver->condition.wait_for(lock,
                                 std::chrono::nanoseconds(timeout_ns - elapsed));
    }
  }
  return SyncDecodeSurface(driver, synchronized_surface, &lock, timeout_ns);
}

static VAStatus SyncSurface(VADriverContextP context, VASurfaceID surface_id) {
  return SyncSurface2(context, surface_id, VA_TIMEOUT_INFINITE);
}

static VAStatus QuerySurfaceStatus(VADriverContextP context,
                                   VASurfaceID surface_id,
                                   VASurfaceStatus *status) {
  Driver *driver = GetDriver(context);
  std::lock_guard<std::mutex> lock(driver->mutex);
  auto surface = driver->surfaces.find(surface_id);
  if (surface == driver->surfaces.end())
    return VA_STATUS_ERROR_INVALID_SURFACE;
  *status = surface->second->ready ? VASurfaceReady : VASurfaceRendering;
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
      va_image == nullptr)
    return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;

  Buffer buffer;
  buffer.context = VA_INVALID_ID;
  buffer.type = VAImageBufferType;
  buffer.element_size = 1;
  buffer.elements = static_cast<unsigned int>(width * height * 3 / 2);
  buffer.data.resize(buffer.elements);
  VABufferID buffer_id = driver->next_buffer++;
  driver->buffers[buffer_id] = std::move(buffer);

  Image image;
  image.va.image_id = driver->next_image++;
  image.va.format = format;
  image.va.buf = buffer_id;
  image.va.width = width;
  image.va.height = height;
  image.va.data_size = width * height * 3 / 2;
  image.va.num_planes = 2;
  image.va.pitches[0] = width;
  image.va.pitches[1] = width;
  image.va.offsets[0] = 0;
  image.va.offsets[1] = width * height;
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
  driver->buffers.erase(image->second.va.buf);
  driver->images.erase(image);
  return VA_STATUS_SUCCESS;
}

static VAStatus CopySurfaceToImage(Driver *driver, Surface *surface,
                                   Image *image, unsigned int width,
                                   unsigned int height) {
  if (surface->fourcc != VA_FOURCC_NV12)
    return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
  auto buffer = driver->buffers.find(image->va.buf);
  if (buffer == driver->buffers.end())
    return VA_STATUS_ERROR_INVALID_BUFFER;
  width = std::min<unsigned int>(
      width, std::min<unsigned int>(surface->width, image->va.width));
  height = std::min<unsigned int>(height,
                                  std::min<unsigned int>(surface->height,
                                                         image->va.height));
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
           width);
  }
  return VA_STATUS_SUCCESS;
}

static VAStatus DeriveImage(VADriverContextP context, VASurfaceID surface_id,
                            VAImage *va_image) {
  Driver *driver = GetDriver(context);
  std::lock_guard<std::mutex> lock(driver->mutex);
  auto surface = driver->surfaces.find(surface_id);
  if (surface == driver->surfaces.end())
    return VA_STATUS_ERROR_INVALID_SURFACE;
  VAImageFormat format = {};
  format.fourcc = VA_FOURCC_NV12;
  format.byte_order = VA_LSB_FIRST;
  format.bits_per_pixel = 12;
  VAStatus status = CreateImageLocked(driver, format, surface->second->width,
                                      surface->second->height, va_image);
  if (status != VA_STATUS_SUCCESS)
    return status;
  return CopySurfaceToImage(driver, surface->second.get(),
                            &driver->images.at(va_image->image_id),
                            surface->second->width, surface->second->height);
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
  return CopySurfaceToImage(driver, surface->second.get(), &image->second,
                            width, height);
}

static VAStatus PutImage(VADriverContextP context, VASurfaceID surface_id,
                         VAImageID image_id, int source_x, int source_y,
                         unsigned int source_width, unsigned int source_height,
                         int destination_x, int destination_y,
                         unsigned int destination_width,
                         unsigned int destination_height) {
  if (source_x != 0 || source_y != 0 || destination_x != 0 ||
      destination_y != 0 || source_width != destination_width ||
      source_height != destination_height)
    return VA_STATUS_ERROR_UNIMPLEMENTED;
  Driver *driver = GetDriver(context);
  std::lock_guard<std::mutex> lock(driver->mutex);
  auto surface = driver->surfaces.find(surface_id);
  auto image = driver->images.find(image_id);
  if (surface == driver->surfaces.end())
    return VA_STATUS_ERROR_INVALID_SURFACE;
  if (image == driver->images.end())
    return VA_STATUS_ERROR_INVALID_IMAGE;
  if (surface->second->fourcc != VA_FOURCC_NV12)
    return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
  auto buffer = driver->buffers.find(image->second.va.buf);
  if (buffer == driver->buffers.end())
    return VA_STATUS_ERROR_INVALID_BUFFER;
  const uint8_t *source_y_plane = buffer->second.data.data() +
                                  image->second.va.offsets[0];
  const uint8_t *source_uv_plane = buffer->second.data.data() +
                                   image->second.va.offsets[1];
  surface->second->BeginCpuWrite();
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
           source_width);
  }
  surface->second->EndCpuWrite();
  surface->second->ready = true;
  return VA_STATUS_SUCCESS;
}

static VAStatus ExportSurfaceHandle(VADriverContextP context,
                                    VASurfaceID surface_id, uint32_t memory_type,
                                    uint32_t flags, void *descriptor) {
  if (memory_type != VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2 ||
      descriptor == nullptr)
    return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
  Driver *driver = GetDriver(context);
  std::lock_guard<std::mutex> lock(driver->mutex);
  auto surface = driver->surfaces.find(surface_id);
  if (surface == driver->surfaces.end())
    return VA_STATUS_ERROR_INVALID_SURFACE;
  if (surface->second->bo == nullptr && surface->second->object_fds.empty())
    return VA_STATUS_ERROR_UNIMPLEMENTED;

  auto *prime = static_cast<VADRMPRIMESurfaceDescriptor *>(descriptor);
  memset(prime, 0, sizeof(*prime));
  for (auto &object : prime->objects)
    object.fd = -1;
  prime->fourcc = surface->second->fourcc;
  prime->width = surface->second->width;
  prime->height = surface->second->height;
  if (surface->second->fourcc == VA_FOURCC_ARGB) {
    const int fd = surface->second->bo != nullptr
                       ? gbm_bo_get_fd_for_plane(surface->second->bo, 0)
                       : dup(surface->second->object_fds[0]);
    if (fd < 0)
      return VA_STATUS_ERROR_OPERATION_FAILED;
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
    BackingIdentity identity;
    if (GetBackingIdentity(fd, &identity)) {
      driver->backing_owners[identity] =
          surface->second->backing_owner != VA_INVALID_SURFACE
              ? surface->second->backing_owner
              : surface_id;
    }
    return VA_STATUS_SUCCESS;
  }
  if (surface->second->fourcc != VA_FOURCC_NV12)
    return VA_STATUS_ERROR_UNIMPLEMENTED;
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
      return VA_STATUS_ERROR_OPERATION_FAILED;
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
  BackingIdentity identity;
  if (GetBackingIdentity(prime->objects[0].fd, &identity)) {
    driver->backing_owners[identity] =
        surface->second->backing_owner != VA_INVALID_SURFACE
            ? surface->second->backing_owner
            : surface_id;
  }
  if ((flags & VA_EXPORT_SURFACE_SEPARATE_LAYERS) != 0) {
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
  return VA_STATUS_SUCCESS;
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
  *count = 0;
  return VA_STATUS_SUCCESS;
}
static VAStatus GetDisplayAttributes(VADriverContextP, VADisplayAttribute *,
                                     int) {
  return VA_STATUS_SUCCESS;
}
static VAStatus SetDisplayAttributes(VADriverContextP, VADisplayAttribute *,
                                     int) {
  return VA_STATUS_SUCCESS;
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

  context->version_major = VA_MAJOR_VERSION;
  context->version_minor = api_minor_version;
  context->str_vendor = "Broadcom CrystalHD VA-API driver 0.1";
  context->max_profiles = 4;
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
