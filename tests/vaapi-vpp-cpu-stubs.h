// SPDX-License-Identifier: LGPL-2.1-or-later
// Link-only guards for the graphics-free VPP state-machine test. Every
// unexpected graphics/scaler/DIL call aborts; no hardware API returns success.
// Exact declarations come from the headers already included by vaapi-vpp.cpp.
#pragma once

[[noreturn]] static void VppCpuUnexpectedCall(const char *name) {
  fprintf(stderr, "Unexpected graphics/scaler/DIL call in CPU-only VPP test: %s\n", name);
  std::abort();
}

extern "C" {
BC_STATUS DtsCrystalHDVersion(HANDLE, PBC_INFO_CRYSTAL) { VppCpuUnexpectedCall(__func__); }
BC_STATUS DtsDeviceOpen(HANDLE *, uint32_t) { VppCpuUnexpectedCall(__func__); }
BC_STATUS DtsDeviceClose(HANDLE) { VppCpuUnexpectedCall(__func__); }
BC_STATUS DtsOpenDecoder(HANDLE, uint32_t) { VppCpuUnexpectedCall(__func__); }
BC_STATUS DtsCloseDecoder(HANDLE) { VppCpuUnexpectedCall(__func__); }
BC_STATUS DtsSetInputFormat(HANDLE, BC_INPUT_FORMAT *) { VppCpuUnexpectedCall(__func__); }
BC_STATUS DtsSetScaleParams(HANDLE, BC_SCALING_PARAMS *) { VppCpuUnexpectedCall(__func__); }
BC_STATUS DtsSetColorSpace(HANDLE, BC_OUTPUT_FORMAT) { VppCpuUnexpectedCall(__func__); }
BC_STATUS DtsStartDecoder(HANDLE) { VppCpuUnexpectedCall(__func__); }
BC_STATUS DtsStartCapture(HANDLE) { VppCpuUnexpectedCall(__func__); }
uint32_t DtsTxFreeSize(HANDLE) { VppCpuUnexpectedCall(__func__); }
BC_STATUS DtsFlushInput(HANDLE, uint32_t) { VppCpuUnexpectedCall(__func__); }
BC_STATUS DtsIsEndOfStream(HANDLE, uint8_t *) { VppCpuUnexpectedCall(__func__); }
BC_STATUS DtsProcInput(HANDLE, uint8_t *, uint32_t, uint64_t, BOOL) { VppCpuUnexpectedCall(__func__); }
BC_STATUS DtsProcOutputNoCopy(HANDLE, uint32_t, BC_DTS_PROC_OUT *) { VppCpuUnexpectedCall(__func__); }
BC_STATUS DtsReleaseOutputBuffs(HANDLE, PVOID, BOOL) { VppCpuUnexpectedCall(__func__); }

gbm_device *gbm_create_device(int) { VppCpuUnexpectedCall(__func__); }
void gbm_device_destroy(gbm_device *) { VppCpuUnexpectedCall(__func__); }
gbm_bo *gbm_bo_create(gbm_device *, uint32_t, uint32_t, uint32_t, uint32_t) { VppCpuUnexpectedCall(__func__); }
gbm_bo *gbm_bo_import(gbm_device *, uint32_t, void *, uint32_t) { VppCpuUnexpectedCall(__func__); }
int gbm_bo_get_plane_count(gbm_bo *) { VppCpuUnexpectedCall(__func__); }
int gbm_bo_get_fd_for_plane(gbm_bo *, int) { VppCpuUnexpectedCall(__func__); }
void *gbm_bo_map(gbm_bo *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t *, void **) { VppCpuUnexpectedCall(__func__); }
void gbm_bo_unmap(gbm_bo *, void *) { VppCpuUnexpectedCall(__func__); }
void gbm_bo_destroy(gbm_bo *) { VppCpuUnexpectedCall(__func__); }
uint32_t gbm_bo_get_stride_for_plane(gbm_bo *, int) { VppCpuUnexpectedCall(__func__); }
uint32_t gbm_bo_get_offset(gbm_bo *, int) { VppCpuUnexpectedCall(__func__); }
uint64_t gbm_bo_get_modifier(gbm_bo *) { VppCpuUnexpectedCall(__func__); }

drmVersionPtr drmGetVersion(int) { VppCpuUnexpectedCall(__func__); }
void drmFreeVersion(drmVersionPtr) { VppCpuUnexpectedCall(__func__); }
int drmGetDevice2(int, uint32_t, drmDevicePtr *) { VppCpuUnexpectedCall(__func__); }
void drmFreeDevice(drmDevicePtr *) { VppCpuUnexpectedCall(__func__); }
int drmPrimeHandleToFD(int, uint32_t, uint32_t, int *) { VppCpuUnexpectedCall(__func__); }

SwsContext *sws_getCachedContext(SwsContext *, int, int, AVPixelFormat, int, int,
    AVPixelFormat, int, SwsFilter *, SwsFilter *, const double *) { VppCpuUnexpectedCall(__func__); }
int sws_scale(SwsContext *, const uint8_t *const [], const int [], int, int,
              uint8_t *const [], const int []) { VppCpuUnexpectedCall(__func__); }
const int *sws_getCoefficients(int) { VppCpuUnexpectedCall(__func__); }
int sws_setColorspaceDetails(SwsContext *, const int *, int, const int *, int,
                             int, int, int) { VppCpuUnexpectedCall(__func__); }
void sws_freeContext(SwsContext *context) {
  if (context != nullptr) VppCpuUnexpectedCall(__func__);
}
}
