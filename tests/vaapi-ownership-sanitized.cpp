// SPDX-License-Identifier: LGPL-2.1-or-later
// The unchanged handles regression includes the production VA implementation,
// so one sanitizer compile instruments both the callbacks and their harness.
// Its public vtable retains decoder entry points even though these ownership
// tests never decode. Supply abort-only link guards instead of loading an
// ordinary, uninstrumented libcrystalhd or building any normal output.
#include "vaapi-buffer-handles.cpp"

namespace {
[[noreturn]] void OwnershipUnexpectedDtsCall(const char *name) {
  std::fprintf(stderr, "Unexpected DIL call in VA ownership sanitizer test: %s\n", name);
  std::abort();
}
} // namespace

extern "C" {
BC_STATUS DtsCrystalHDVersion(HANDLE, PBC_INFO_CRYSTAL) {
  OwnershipUnexpectedDtsCall(__func__);
}
BC_STATUS DtsDeviceClose(HANDLE) { OwnershipUnexpectedDtsCall(__func__); }
BC_STATUS DtsOpenDecoder(HANDLE, uint32_t) { OwnershipUnexpectedDtsCall(__func__); }
BC_STATUS DtsCloseDecoder(HANDLE) { OwnershipUnexpectedDtsCall(__func__); }
BC_STATUS DtsSetInputFormat(HANDLE, BC_INPUT_FORMAT *) {
  OwnershipUnexpectedDtsCall(__func__);
}
BC_STATUS DtsSetScaleParams(HANDLE, BC_SCALING_PARAMS *) {
  OwnershipUnexpectedDtsCall(__func__);
}
BC_STATUS DtsSetColorSpace(HANDLE, BC_OUTPUT_FORMAT) {
  OwnershipUnexpectedDtsCall(__func__);
}
BC_STATUS DtsStartDecoder(HANDLE) { OwnershipUnexpectedDtsCall(__func__); }
BC_STATUS DtsStopDecoder(HANDLE) { OwnershipUnexpectedDtsCall(__func__); }
BC_STATUS DtsStartCapture(HANDLE) { OwnershipUnexpectedDtsCall(__func__); }
uint32_t DtsTxFreeSize(HANDLE) { OwnershipUnexpectedDtsCall(__func__); }
BC_STATUS DtsFlushInput(HANDLE, uint32_t) { OwnershipUnexpectedDtsCall(__func__); }
BC_STATUS DtsIsEndOfStream(HANDLE, uint8_t *) {
  OwnershipUnexpectedDtsCall(__func__);
}
BC_STATUS DtsGetDriverStatus(HANDLE, BC_DTS_STATUS *) {
  OwnershipUnexpectedDtsCall(__func__);
}
BC_STATUS DtsProcInput(HANDLE, uint8_t *, uint32_t, uint64_t, BOOL) {
  OwnershipUnexpectedDtsCall(__func__);
}
BC_STATUS DtsProcOutputNoCopy(HANDLE, uint32_t, BC_DTS_PROC_OUT *) {
  OwnershipUnexpectedDtsCall(__func__);
}
BC_STATUS DtsReleaseOutputBuffs(HANDLE, PVOID, BOOL) {
  OwnershipUnexpectedDtsCall(__func__);
}
// Optional decoder interfaces also fail closed if any unselected path uses
// them. DtsDeviceOpen is already an aborting --wrap seam in the real harness.
BC_STATUS DtsDupOutputPollFd(HANDLE, int *) {
  OwnershipUnexpectedDtsCall(__func__);
}
BC_STATUS DtsGetLibraryCapabilities(uint32_t *) {
  OwnershipUnexpectedDtsCall(__func__);
}
BC_STATUS DtsProcInputFragment(HANDLE, uint8_t *, uint32_t, uint64_t, BOOL, uint32_t) {
  OwnershipUnexpectedDtsCall(__func__);
}
}
