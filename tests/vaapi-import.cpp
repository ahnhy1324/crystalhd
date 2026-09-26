// SPDX-License-Identifier: LGPL-2.1-or-later

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include <fcntl.h>
#include <libdrm/drm_fourcc.h>
#include <linux/dma-buf.h>
#include <linux/memfd.h>
#include <linux/udmabuf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <va/va_vpp.h>

namespace {

int CreateBuffer(size_t size, bool *is_dma_buf) {
  int memfd = memfd_create("crystalhd-vaapi-test", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  if (memfd < 0 || ftruncate(memfd, static_cast<off_t>(size)) != 0)
    return -1;

  *is_dma_buf = false;
  int udmabuf = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
  if (udmabuf < 0)
    return memfd;

  if (fcntl(memfd, F_ADD_SEALS, F_SEAL_SHRINK) != 0) {
    close(udmabuf);
    return memfd;
  }
  udmabuf_create request = {};
  request.memfd = memfd;
  request.size = size;
  int dma_buf = ioctl(udmabuf, UDMABUF_CREATE, &request);
  close(udmabuf);
  if (dma_buf < 0)
    return memfd;
  close(memfd);
  *is_dma_buf = true;
  return dma_buf;
}

bool Check(VAStatus status, const char *operation) {
  if (status == VA_STATUS_SUCCESS)
    return true;
  fprintf(stderr, "%s failed: %s\n", operation, vaErrorStr(status));
  return false;
}

void CloseDescriptor(VADRMPRIMESurfaceDescriptor *descriptor) {
  const uint32_t capacity = sizeof(descriptor->objects) / sizeof(descriptor->objects[0]);
  for (uint32_t object = 0; object < descriptor->num_objects && object < capacity; ++object) {
    if (descriptor->objects[object].fd >= 0)
      close(descriptor->objects[object].fd);
  }
  descriptor->num_objects = 0;
}

bool ContainsFormat(const uint32_t *formats, uint32_t count, uint32_t format) {
  for (uint32_t index = 0; index < count; ++index) {
    if (formats[index] == format)
      return true;
  }
  return false;
}

bool ColdImageHandle(VADisplay display, VASurfaceID surface) {
  VAImage image = {};
  if (!Check(vaDeriveImage(display, surface, &image),
             "vaDeriveImage(VLC cold surface probe)"))
    return false;
  VABufferInfo info = {};
  info.mem_type = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME;
  const bool acquired = Check(vaAcquireBufferHandle(display, image.buf, &info),
                             "vaAcquireBufferHandle(VLC cold surface probe)");
  bool success = acquired;
  if (acquired)
    success = Check(vaReleaseBufferHandle(display, image.buf),
                    "vaReleaseBufferHandle(VLC cold surface probe)");
  return Check(vaDestroyImage(display, image.image_id),
               "vaDestroyImage(VLC cold surface probe)") && success;
}

// Use the public libva entry points and real DRM-backed storage. The fault
// injection suite covers failed ownership; this verifies successful CPU
// upload/readback through two VA IDs referring to the same DMA-BUF.
bool ImagePattern(VADisplay display, const VAImage &image, uint8_t seed,
                  bool verify) {
  if (image.format.fourcc != VA_FOURCC_NV12 || image.num_planes != 2 ||
      image.width == 0 || image.height == 0)
    return false;
  for (unsigned int plane = 0; plane < 2; ++plane) {
    const size_t rows = plane == 0 ? image.height : (image.height + 1) / 2;
    const size_t columns = plane == 0 ? image.width : (image.width + 1) & ~1U;
    if (image.pitches[plane] < columns ||
        static_cast<size_t>(image.offsets[plane]) +
                (rows - 1) * image.pitches[plane] + columns > image.data_size)
      return false;
  }
  void *mapped = nullptr;
  if (!Check(vaMapBuffer(display, image.buf, &mapped), "vaMapBuffer(image)"))
    return false;
  bool success = mapped != nullptr;
  auto *bytes = static_cast<uint8_t *>(mapped);
  for (unsigned int plane = 0; success && plane < 2; ++plane) {
    const unsigned int rows = plane == 0 ? image.height : (image.height + 1) / 2;
    const unsigned int columns = plane == 0 ? image.width : (image.width + 1) & ~1U;
    for (unsigned int row = 0; success && row < rows; ++row) {
      for (unsigned int column = 0; column < columns; ++column) {
        uint8_t &pixel = bytes[image.offsets[plane] +
                               static_cast<size_t>(row) * image.pitches[plane] +
                               column];
        const uint8_t expected = static_cast<uint8_t>(
            seed + plane * 71 + row * 13 + column * 37);
        if (!verify)
          pixel = expected;
        else if (pixel != expected) {
          fprintf(stderr, "image readback mismatch at plane %u row %u column %u\n",
                  plane, row, column);
          success = false;
          break;
        }
      }
    }
  }
  return Check(vaUnmapBuffer(display, image.buf), "vaUnmapBuffer(image)") && success;
}

// Exercise VLC's public DeriveImage/AcquireBufferHandle path with an actual
// DMA-BUF, including external writes. A derived snapshot must never write back
// into its original decoded surface when the external handle is released.
bool ImageHandleRoundTrip(VADisplay display, VASurfaceID surface,
                          const VAImage &image, uint8_t seed) {
  VABufferInfo info = {};
  info.mem_type = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME;
  if (!Check(vaAcquireBufferHandle(display, image.buf, &info),
             "vaAcquireBufferHandle(NV12 snapshot)"))
    return false;
  const int fd = static_cast<int>(info.handle);
  bool success = info.mem_type == VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME &&
                 info.type == VAImageBufferType &&
                 info.mem_size >= image.data_size && fd >= 0 &&
                 (fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0;
  void *map = MAP_FAILED;
  bool started = false;
  if (success) {
    map = mmap(nullptr, info.mem_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    dma_buf_sync sync = {};
    sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW;
    started = map != MAP_FAILED && ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) == 0;
    success = started;
  }
  if (success) {
    auto *bytes = static_cast<uint8_t *>(map);
    for (unsigned int plane = 0; plane < image.num_planes; ++plane) {
      const unsigned int rows = plane == 0 ? image.height : (image.height + 1) / 2;
      const unsigned int columns = plane == 0 ? image.width : (image.width + 1) & ~1U;
      for (unsigned int row = 0; row < rows; ++row) {
        for (unsigned int column = 0; column < columns; ++column) {
          uint8_t &pixel = bytes[image.offsets[plane] +
              static_cast<size_t>(row) * image.pitches[plane] + column];
          const uint8_t expected = static_cast<uint8_t>(
              seed + plane * 71 + row * 13 + column * 37);
          success = (pixel == expected) && success;
          pixel = static_cast<uint8_t>(expected + 1);
        }
      }
    }
    void *forbidden = nullptr;
    success = vaMapBuffer(display, image.buf, &forbidden) ==
                  VA_STATUS_ERROR_SURFACE_BUSY && success;
    success = vaBufferSetNumElements(display, image.buf, 1) ==
                  VA_STATUS_ERROR_SURFACE_BUSY && success;
    success = vaGetImage(display, surface, 0, 0, image.width, image.height,
                        image.image_id) == VA_STATUS_ERROR_SURFACE_BUSY && success;
    success = vaDestroyBuffer(display, image.buf) ==
                  VA_STATUS_ERROR_SURFACE_BUSY && success;
    success = vaDestroyImage(display, image.image_id) ==
                  VA_STATUS_ERROR_SURFACE_BUSY && success;
  }
  if (started) {
    dma_buf_sync sync = {};
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW;
    success = ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) == 0 && success;
  }
  if (map != MAP_FAILED)
    success = munmap(map, info.mem_size) == 0 && success;
  success = Check(vaReleaseBufferHandle(display, image.buf),
                  "vaReleaseBufferHandle(NV12 snapshot)") && success;
  errno = 0;
  success = fcntl(fd, F_GETFD) == -1 && errno == EBADF && success;
  if (success)
    success = ImagePattern(display, image, static_cast<uint8_t>(seed + 1), true);
  VAImage original = {};
  original.image_id = VA_INVALID_ID;
  if (success)
    success = Check(vaDeriveImage(display, surface, &original),
                    "vaDeriveImage(original after external snapshot write)");
  if (success)
    success = ImagePattern(display, original, seed, true);
  if (original.image_id != VA_INVALID_ID)
    success = Check(vaDestroyImage(display, original.image_id),
                    "vaDestroyImage(original snapshot)") && success;
  if (!success)
    fprintf(stderr, "DMA-BUF snapshot bytes, exclusivity or lifetime check failed\n");
  else
    printf("NV12 DMA-BUF snapshot read/write and lifetime passed (%ux%u)\n",
           image.width, image.height);
  return success;
}

bool SurfaceExportLayers(VADisplay display, VASurfaceID surface,
                         unsigned int width, unsigned int height, uint8_t seed) {
  for (uint32_t flags : {0U, static_cast<uint32_t>(VA_EXPORT_SURFACE_COMPOSED_LAYERS)}) {
    VADRMPRIMESurfaceDescriptor desc = {};
    if (!Check(vaExportSurfaceHandle(display, surface,
                                    VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                    flags, &desc), "vaExportSurfaceHandle(layer layout)"))
      return false;
    const bool composed = flags != 0;
    bool success = desc.fourcc == VA_FOURCC_NV12 && desc.width == width &&
                   desc.height == height && desc.num_objects > 0 &&
                   desc.num_objects <= 2 && desc.num_layers == (composed ? 1U : 2U);
    for (unsigned int plane = 0; success && plane < 2; ++plane) {
      const unsigned int layer = composed ? 0 : plane;
      const unsigned int index = composed ? plane : 0;
      const auto &layout = desc.layers[layer];
      const unsigned int object = layout.object_index[index];
      const unsigned int rows = plane == 0 ? height : (height + 1) / 2;
      const unsigned int columns = plane == 0 ? width : (width + 1) & ~1U;
      const uint32_t format = composed ? DRM_FORMAT_NV12 :
          (plane == 0 ? DRM_FORMAT_R8 : DRM_FORMAT_GR88);
      success = layout.drm_format == format &&
                layout.num_planes == (composed ? 2U : 1U) &&
                object < desc.num_objects && layout.pitch[index] >= columns;
      if (!success)
        break;
      const auto &backing = desc.objects[object];
      const size_t end = static_cast<size_t>(layout.offset[index]) +
          static_cast<size_t>(rows - 1) * layout.pitch[index] + columns;
      success = backing.fd >= 0 && end <= backing.size &&
                backing.drm_format_modifier == DRM_FORMAT_MOD_LINEAR;
      if (!success)
        break;
      void *map = mmap(nullptr, backing.size, PROT_READ, MAP_SHARED, backing.fd, 0);
      dma_buf_sync sync = {};
      sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ;
      const bool started = map != MAP_FAILED &&
          ioctl(backing.fd, DMA_BUF_IOCTL_SYNC, &sync) == 0;
      success = started;
      if (started) {
        const auto *bytes = static_cast<const uint8_t *>(map);
        for (unsigned int row = 0; row < rows; ++row) {
          for (unsigned int column = 0; column < columns; ++column) {
            const uint8_t expected = static_cast<uint8_t>(
                seed + plane * 71 + row * 13 + column * 37);
            success = bytes[layout.offset[index] +
                static_cast<size_t>(row) * layout.pitch[index] + column] == expected && success;
          }
        }
        sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
        success = ioctl(backing.fd, DMA_BUF_IOCTL_SYNC, &sync) == 0 && success;
      }
      if (map != MAP_FAILED)
        success = munmap(map, backing.size) == 0 && success;
    }
    CloseDescriptor(&desc);
    if (!success) {
      fprintf(stderr, "NV12 default/composed export layout or pixels failed\n");
      return false;
    }
  }
  puts("NV12 default separate/composed PRIME2 plane pixels passed");
  return true;
}

bool ImageRoundTrip(VADisplay display, VASurfaceID writer, VASurfaceID reader,
                    unsigned int width, unsigned int height, uint8_t seed) {
  VAImageFormat format = {};
  format.fourcc = VA_FOURCC_NV12;
  format.byte_order = VA_LSB_FIRST;
  format.bits_per_pixel = 12;
  VAImage input = {}, output = {}, derived = {};
  input.image_id = output.image_id = derived.image_id = VA_INVALID_ID;
  bool success = Check(vaCreateImage(display, &format, width, height, &input),
                       "vaCreateImage(upload)");
  if (success)
    success = ImagePattern(display, input, seed, false);
  if (success)
    success = Check(vaPutImage(display, writer, input.image_id, 0, 0, width,
                               height, 0, 0, width, height), "vaPutImage(NV12)");
  if (success)
    success = Check(vaSyncSurface(display, reader), "vaSyncSurface(image alias)");
  if (success)
    success = Check(vaCreateImage(display, &format, width, height, &output),
                     "vaCreateImage(readback)");
  if (success)
    success = Check(vaGetImage(display, reader, 0, 0, width, height,
                               output.image_id), "vaGetImage(NV12 alias)");
  if (success)
    success = ImagePattern(display, output, seed, true);
  if (success)
    success = Check(vaDeriveImage(display, reader, &derived),
                     "vaDeriveImage(NV12 snapshot)");
  if (success)
    success = ImagePattern(display, derived, seed, true);
  if (success)
    success = ImageHandleRoundTrip(display, reader, derived, seed);
  for (VAImage *image : {&derived, &output, &input}) {
    if (image->image_id != VA_INVALID_ID)
      success = Check(vaDestroyImage(display, image->image_id),
                       "vaDestroyImage(roundtrip)") && success;
  }
  return success;
}

}  // namespace

int main(int argc, char **argv) {
  const char *device_path = argc > 1 ? argv[1] : "/dev/dri/renderD128";
  int drm_fd = open(device_path, O_RDWR | O_CLOEXEC);
  if (drm_fd < 0) {
    fprintf(stderr, "open %s failed: %s\n", device_path, strerror(errno));
    return 1;
  }

  VADisplay display = vaGetDisplayDRM(drm_fd);
  int major = 0;
  int minor = 0;
  if (display == nullptr || !Check(vaInitialize(display, &major, &minor),
                                   "vaInitialize")) {
    close(drm_fd);
    return 1;
  }

  constexpr unsigned int width = 128;
  constexpr unsigned int height = 64;
  constexpr size_t buffer_size = width * height * 3 / 2;
  bool is_dma_buf = false;
  int buffer_fd = CreateBuffer(buffer_size, &is_dma_buf);
  if (buffer_fd < 0) {
    fprintf(stderr, "surface buffer allocation failed: %s\n", strerror(errno));
    vaTerminate(display);
    close(drm_fd);
    return 1;
  }

  VADRMPRIMESurfaceDescriptor descriptor = {};
  descriptor.fourcc = DRM_FORMAT_NV12;
  descriptor.width = width;
  descriptor.height = height;
  descriptor.num_objects = 1;
  descriptor.objects[0].fd = buffer_fd;
  descriptor.objects[0].size = buffer_size;
  descriptor.objects[0].drm_format_modifier = DRM_FORMAT_MOD_LINEAR;
  descriptor.num_layers = 1;
  descriptor.layers[0].drm_format = DRM_FORMAT_NV12;
  descriptor.layers[0].num_planes = 2;
  descriptor.layers[0].object_index[0] = 0;
  descriptor.layers[0].object_index[1] = 0;
  descriptor.layers[0].offset[0] = 0;
  descriptor.layers[0].offset[1] = width * height;
  descriptor.layers[0].pitch[0] = width;
  descriptor.layers[0].pitch[1] = width;

  VASurfaceAttrib attributes[2] = {};
  attributes[0].type = VASurfaceAttribMemoryType;
  attributes[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
  attributes[0].value.type = VAGenericValueTypeInteger;
  attributes[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
  attributes[1].type = VASurfaceAttribExternalBufferDescriptor;
  attributes[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
  attributes[1].value.type = VAGenericValueTypePointer;
  attributes[1].value.value.p = &descriptor;

  VASurfaceID surface = VA_INVALID_SURFACE;
  bool success = Check(vaCreateSurfaces(display, VA_RT_FORMAT_YUV420, width,
                                        height, &surface, 1, attributes, 2),
                       "vaCreateSurfaces(DRM PRIME NV12)");
  if (success)
    success = Check(vaDestroySurfaces(display, &surface, 1),
                    "vaDestroySurfaces");

  close(buffer_fd);

  VASurfaceID nv12_surface = VA_INVALID_SURFACE;
  VADRMPRIMESurfaceDescriptor nv12_exported = {};
  bool nv12_export_success = false;
  bool vpp_success = Check(
      vaCreateSurfaces(display, VA_RT_FORMAT_YUV420, width, height,
                       &nv12_surface, 1, nullptr, 0),
      "vaCreateSurfaces(internal NV12)");
  if (vpp_success)
    vpp_success = ColdImageHandle(display, nv12_surface);
  if (vpp_success) {
    nv12_export_success = Check(
        vaExportSurfaceHandle(display, nv12_surface,
                              VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                              VA_EXPORT_SURFACE_READ_WRITE |
                                  VA_EXPORT_SURFACE_SEPARATE_LAYERS,
                              &nv12_exported),
        "vaExportSurfaceHandle(NV12)");
    vpp_success = nv12_export_success;
  }
  if (vpp_success &&
      (nv12_exported.fourcc != VA_FOURCC_NV12 ||
       nv12_exported.num_objects < 1 || nv12_exported.num_objects > 2 ||
       nv12_exported.num_layers != 2 ||
       nv12_exported.layers[0].drm_format != DRM_FORMAT_R8 ||
       nv12_exported.layers[1].drm_format != DRM_FORMAT_GR88 ||
       nv12_exported.layers[0].num_planes != 1 ||
       nv12_exported.layers[1].num_planes != 1)) {
    fprintf(stderr, "exported NV12 descriptor is invalid\n");
    vpp_success = false;
  }
  for (uint32_t object = 0;
       vpp_success && object < nv12_exported.num_objects; ++object) {
    if (nv12_exported.objects[object].fd < 0) {
      fprintf(stderr, "exported NV12 object has no descriptor\n");
      vpp_success = false;
    }
  }
  VASurfaceID nv12_alias = VA_INVALID_SURFACE;
  VASurfaceAttrib nv12_attributes[2] = {};
  nv12_attributes[0].type = VASurfaceAttribMemoryType;
  nv12_attributes[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
  nv12_attributes[0].value.type = VAGenericValueTypeInteger;
  nv12_attributes[0].value.value.i =
      VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
  nv12_attributes[1].type = VASurfaceAttribExternalBufferDescriptor;
  nv12_attributes[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
  nv12_attributes[1].value.type = VAGenericValueTypePointer;
  nv12_attributes[1].value.value.p = &nv12_exported;
  if (vpp_success) {
    vpp_success = Check(
        vaCreateSurfaces(display, VA_RT_FORMAT_YUV420, width, height,
                         &nv12_alias, 1, nv12_attributes, 2),
        "vaCreateSurfaces(reimported NV12)");
  }
  if (nv12_export_success)
    CloseDescriptor(&nv12_exported);
  if (vpp_success)
    vpp_success = ImageRoundTrip(display, nv12_surface, nv12_alias,
                                 width, height, 17);
  if (vpp_success)
    vpp_success = ImageRoundTrip(display, nv12_alias, nv12_surface,
                                 width, height, 29);
  if (vpp_success)
    vpp_success = SurfaceExportLayers(display, nv12_surface, width, height, 29);
  // Tight VAImage pitches differ from DRM allocation strides, especially
  // for odd sizes. The exported byte store must preserve those public offsets.
  VASurfaceID odd_surface = VA_INVALID_SURFACE;
  if (vpp_success)
    vpp_success = Check(vaCreateSurfaces(display, VA_RT_FORMAT_YUV420, 17, 17,
                                       &odd_surface, 1, nullptr, 0),
                        "vaCreateSurfaces(odd snapshot)");
  if (vpp_success)
    vpp_success = ImageRoundTrip(display, odd_surface, odd_surface, 17, 17, 43);
  if (odd_surface != VA_INVALID_SURFACE)
    vpp_success = Check(vaDestroySurfaces(display, &odd_surface, 1),
                        "vaDestroySurfaces(odd snapshot)") && vpp_success;

  VASurfaceID argb_surface = VA_INVALID_SURFACE;
  if (vpp_success) {
    vpp_success = Check(
        vaCreateSurfaces(display, VA_RT_FORMAT_RGB32, width, height,
                         &argb_surface, 1, nullptr, 0),
        "vaCreateSurfaces(ARGB)");
  }

  VADRMPRIMESurfaceDescriptor argb_exported = {};
  bool argb_export_success = false;
  if (vpp_success) {
    argb_export_success = Check(
        vaExportSurfaceHandle(display, argb_surface,
                              VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                              VA_EXPORT_SURFACE_READ_WRITE |
                                  VA_EXPORT_SURFACE_COMPOSED_LAYERS,
                              &argb_exported),
        "vaExportSurfaceHandle(ARGB)");
    vpp_success = argb_export_success;
  }
  if (vpp_success &&
      (argb_exported.fourcc != VA_FOURCC_ARGB ||
       argb_exported.num_objects != 1 || argb_exported.num_layers != 1 ||
       argb_exported.layers[0].drm_format != DRM_FORMAT_ARGB8888 ||
       argb_exported.layers[0].num_planes != 1 ||
       argb_exported.objects[0].fd < 0)) {
    fprintf(stderr, "exported ARGB descriptor is invalid\n");
    vpp_success = false;
  }
  VASurfaceID argb_alias = VA_INVALID_SURFACE;
  VASurfaceAttrib argb_attributes[2] = {};
  argb_attributes[0].type = VASurfaceAttribMemoryType;
  argb_attributes[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
  argb_attributes[0].value.type = VAGenericValueTypeInteger;
  argb_attributes[0].value.value.i =
      VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
  argb_attributes[1].type = VASurfaceAttribExternalBufferDescriptor;
  argb_attributes[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
  argb_attributes[1].value.type = VAGenericValueTypePointer;
  argb_attributes[1].value.value.p = &argb_exported;
  if (vpp_success) {
    vpp_success = Check(
        vaCreateSurfaces(display, VA_RT_FORMAT_RGB32, width, height,
                         &argb_alias, 1, argb_attributes, 2),
        "vaCreateSurfaces(reimported ARGB)");
  }
  if (argb_export_success)
    CloseDescriptor(&argb_exported);

  VAConfigID vpp_config = VA_INVALID_ID;
  VAContextID vpp_context = VA_INVALID_ID;
  VABufferID vpp_buffer = VA_INVALID_ID;
  if (vpp_success) {
    vpp_success = Check(
        vaCreateConfig(display, VAProfileNone, VAEntrypointVideoProc, nullptr,
                       0, &vpp_config),
        "vaCreateConfig(VideoProc)");
  }
  if (vpp_success) {
    vpp_success = Check(
        vaCreateContext(display, vpp_config, 0, 0, VA_PROGRESSIVE, nullptr, 0,
                        &vpp_context),
        "vaCreateContext(VideoProc)");
  }

  uint32_t input_formats[2] = {};
  uint32_t output_formats[2] = {};
  VAProcPipelineCaps caps = {};
  caps.input_pixel_format = input_formats;
  caps.num_input_pixel_formats = 2;
  caps.output_pixel_format = output_formats;
  caps.num_output_pixel_formats = 2;
  if (vpp_success) {
    vpp_success = Check(
        vaQueryVideoProcPipelineCaps(display, vpp_context, nullptr, 0, &caps),
        "vaQueryVideoProcPipelineCaps");
  }
  if (vpp_success &&
      (!ContainsFormat(input_formats, caps.num_input_pixel_formats,
                       VA_FOURCC_NV12) ||
       !ContainsFormat(output_formats, caps.num_output_pixel_formats,
                       VA_FOURCC_ARGB))) {
    fprintf(stderr, "video processor formats are incomplete\n");
    vpp_success = false;
  }

  VARectangle source_region = {0, 0, width, height};
  VARectangle output_region = {0, 0, width, height};
  VAProcPipelineParameterBuffer parameters = {};
  parameters.surface = nv12_alias;
  parameters.surface_region = &source_region;
  parameters.output_region = &output_region;
  if (vpp_success) {
    vpp_success = Check(
        vaCreateBuffer(display, vpp_context,
                       VAProcPipelineParameterBufferType, sizeof(parameters), 1,
                       &parameters, &vpp_buffer),
        "vaCreateBuffer(VideoProc)");
  }
  if (vpp_success)
    vpp_success = Check(vaBeginPicture(display, vpp_context, argb_alias),
                        "vaBeginPicture(VideoProc)");
  if (vpp_success)
    vpp_success = Check(vaRenderPicture(display, vpp_context, &vpp_buffer, 1),
                        "vaRenderPicture(VideoProc)");
  if (vpp_success)
    vpp_success = Check(vaEndPicture(display, vpp_context),
                        "vaEndPicture(VideoProc)");
  bool asynchronous_vpp_completed = false;
  for (unsigned int attempt = 0; vpp_success && attempt < 1000; ++attempt) {
    VASurfaceStatus surface_status = VASurfaceRendering;
    vpp_success = Check(vaQuerySurfaceStatus(display, argb_surface,
                                             &surface_status),
                        "vaQuerySurfaceStatus(async VideoProc)");
    if (!vpp_success)
      break;
    if (surface_status == VASurfaceReady) {
      asynchronous_vpp_completed = true;
      break;
    }
    usleep(1000);
  }
  if (vpp_success && !asynchronous_vpp_completed) {
    fprintf(stderr, "video processor did not complete asynchronously\n");
    vpp_success = false;
  }
  if (vpp_success)
    vpp_success = Check(vaSyncSurface(display, argb_surface),
                        "vaSyncSurface(exported ARGB owner)");

  if (vpp_buffer != VA_INVALID_ID)
    vpp_success = Check(vaDestroyBuffer(display, vpp_buffer),
                        "vaDestroyBuffer(VideoProc)") && vpp_success;
  if (vpp_context != VA_INVALID_ID)
    vpp_success = Check(vaDestroyContext(display, vpp_context),
                        "vaDestroyContext(VideoProc)") && vpp_success;
  if (vpp_config != VA_INVALID_ID)
    vpp_success = Check(vaDestroyConfig(display, vpp_config),
                        "vaDestroyConfig(VideoProc)") && vpp_success;
  if (argb_alias != VA_INVALID_SURFACE)
    vpp_success = Check(vaDestroySurfaces(display, &argb_alias, 1),
                        "vaDestroySurfaces(reimported ARGB)") && vpp_success;
  if (argb_surface != VA_INVALID_SURFACE)
    vpp_success = Check(vaDestroySurfaces(display, &argb_surface, 1),
                        "vaDestroySurfaces(ARGB)") && vpp_success;
  if (nv12_alias != VA_INVALID_SURFACE)
    vpp_success = Check(vaDestroySurfaces(display, &nv12_alias, 1),
                        "vaDestroySurfaces(reimported NV12)") && vpp_success;
  if (nv12_surface != VA_INVALID_SURFACE)
    vpp_success = Check(vaDestroySurfaces(display, &nv12_surface, 1),
                        "vaDestroySurfaces(internal NV12)") && vpp_success;
  success = success && vpp_success;

  success = Check(vaTerminate(display), "vaTerminate") && success;
  close(drm_fd);
  if (!success)
    return 1;
  printf("VA-API NV12 import/export, image readback, and video processing passed (%s, API %d.%d)\n",
         is_dma_buf ? "dma-buf" : "memfd fallback", major, minor);
  return 0;
}
