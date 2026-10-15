// SPDX-License-Identifier: LGPL-2.1-or-later
// Optional LD_PRELOAD experiment for the stock BCM70015 SET_SINGLE_FIELD
// command. Build with -shared -fPIC -ldl -pthread and the project headers.
// CRYSTALHD_SINGLE_FIELD_PROBE=0|1 selects the pre-input command value. Run
// a single hardware decoder under an external timeout and consume its pixels.
// This probe changes no production default and implements no deinterlacer.
#include <bc_dts_types.h>
#include <libcrystalhd_if.h>

#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <mutex>

// Existing internal library entry point; signature matches libcrystalhd_fwcmds.h.
extern BC_STATUS DtsFWSetSingleField(HANDLE, bool);

namespace {
std::mutex state_mutex;
HANDLE owner;
unsigned outputs;
unsigned formats;
unsigned markers;

int RequestedMode() {
  const char *value = std::getenv("CRYSTALHD_SINGLE_FIELD_PROBE");
  if (!value)
    return -1;
  if (value[0] == '0' && value[1] == '\0')
    return 0;
  if (value[0] == '1' && value[1] == '\0')
    return 1;
  return -2;
}

template <typename Function> Function Next(const char *name) {
  return reinterpret_cast<Function>(dlsym(RTLD_NEXT, name));
}

BC_STATUS Restore(HANDLE device) {
  if (owner != device)
    return BC_STS_SUCCESS;
  // No custom retry follows a rejected/timed-out firmware transaction.
  owner = nullptr;
  const BC_STATUS status = DtsFWSetSingleField(device, false);
  std::fprintf(stderr,
      "single-field-probe restore=0 status=%d outputs=%u formats=%u eos=%u\n",
      status, outputs, formats, markers);
  return status;
}
}  // namespace

extern "C" BC_STATUS DtsStartDecoder(HANDLE device) {
  using Start = BC_STATUS (*)(HANDLE);
  const auto start = Next<Start>("DtsStartDecoder");
  if (!start)
    return BC_STS_ERROR;
  const int mode = RequestedMode();
  if (mode == -1)
    return start(device);
  if (mode < 0)
    return BC_STS_INV_ARG;
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    if (owner)
      return BC_STS_BUSY;
  }
  BC_INFO_CRYSTAL version = {};
  BC_STATUS status = DtsCrystalHDVersion(device, &version);
  if (status != BC_STS_SUCCESS || version.device != 1)
    return status == BC_STS_SUCCESS ? BC_STS_NOT_IMPL : status;
  status = start(device);
  if (status != BC_STS_SUCCESS)
    return status;
  // START may initialize channel state. Submit only after it succeeds and
  // before this caller can begin capture or publish compressed input.
  status = DtsFWSetSingleField(device, mode != 0);
  std::fprintf(stderr, "single-field-probe set=%d status=%d device=%u\n",
               mode, status, version.device);
  if (status == BC_STS_SUCCESS) {
    std::lock_guard<std::mutex> lock(state_mutex);
    owner = device;
    outputs = formats = markers = 0;
  }
  return status;
}

extern "C" BC_STATUS DtsProcOutputNoCopy(HANDLE device, uint32_t timeout,
                                        BC_DTS_PROC_OUT *output) {
  using Receive = BC_STATUS (*)(HANDLE, uint32_t, BC_DTS_PROC_OUT *);
  const auto receive = Next<Receive>("DtsProcOutputNoCopy");
  if (!receive)
    return BC_STS_ERROR;
  const BC_STATUS status = receive(device, timeout, output);
  // The library can return the embedded EOS PIB with NO_DATA. Other early
  // failures leave caller output untouched, so inspect it only on these paths.
  if ((status == BC_STS_SUCCESS || status == BC_STS_FMT_CHANGE ||
       status == BC_STS_NO_DATA) && output &&
      (output->PoutFlags & BC_POUT_FLAGS_PIB_VALID)) {
    std::lock_guard<std::mutex> lock(state_mutex);
    if (owner == device) {
      const bool eos = (output->PicInfo.flags & VDEC_FLAG_EOS) != 0;
      if (eos)
        ++markers;
      else if (status == BC_STS_FMT_CHANGE)
        ++formats;
      else
        ++outputs;
      std::fprintf(stderr,
          "single-field-probe output status=%d count=%u size=%ux%u "
          "flags=0x%x pout=0x%x ts=%llu ordinal=%u session=%u "
          "ybytes=%u uvbytes=%u\n", status, outputs,
          output->PicInfo.width, output->PicInfo.height,
          output->PicInfo.flags, output->PoutFlags,
          static_cast<unsigned long long>(output->PicInfo.timeStamp),
          output->PicInfo.picture_number, output->PicInfo.sess_num,
          output->YBuffDoneSz, output->UVBuffDoneSz);
    }
  }
  return status;
}

extern "C" BC_STATUS DtsStopDecoder(HANDLE device) {
  using Stop = BC_STATUS (*)(HANDLE);
  const auto stop = Next<Stop>("DtsStopDecoder");
  if (!stop)
    return BC_STS_ERROR;
  // STOP first quiesces ordinary TX/RX. Changing field routing beforehand
  // could affect a source picture whose complementary field is pending.
  const BC_STATUS status = stop(device);
  BC_STATUS restored = BC_STS_SUCCESS;
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    if (status == BC_STS_SUCCESS)
      restored = Restore(device);
    else if (owner == device)
      owner = nullptr;
  }
  return status != BC_STS_SUCCESS ? status : restored;
}

extern "C" BC_STATUS DtsCloseDecoder(HANDLE device) {
  using Close = BC_STATUS (*)(HANDLE);
  const auto close = Next<Close>("DtsCloseDecoder");
  if (!close)
    return BC_STS_ERROR;
  bool tracked;
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    tracked = owner == device;
  }
  // Some clients close directly. Use the same STOP/reset ordering before
  // allowing CLOSE to consume the channel; never access a closed handle.
  const BC_STATUS stopped = tracked ? DtsStopDecoder(device) : BC_STS_SUCCESS;
  const BC_STATUS status = close(device);
  return status != BC_STS_SUCCESS ? status : stopped;
}
