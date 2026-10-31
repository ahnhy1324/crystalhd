// SPDX-License-Identifier: LGPL-2.1-or-later
// CPU-only controls execute production conversion and diagnostic hooks.
// The established Bob harness aborts unexpected DIL/device calls.
#define CRYSTALHD_VAAPI_TEST_VPP_PAYLOAD 1
#define main CrystalhdExistingBobMain
#define __wrap_ioctl CrystalhdExistingBobIoctl
#include "vaapi-bob-vpp.cpp"
#undef __wrap_ioctl
#undef main

namespace {
bool fail_payload_target_end = false;
void FillPayloadSource(Surface *source) {
  for (unsigned plane = 0; plane != 2; ++plane) {
    const unsigned rows = plane == 0 ? source->height : source->height / 2;
    for (unsigned row = 0; row != rows; ++row)
      for (unsigned column = 0; column != source->width; ++column)
        source->planes[plane][size_t{row} * source->pitch[plane] + column] =
            static_cast<uint8_t>(row * 29 + column * 7 + plane * 83);
  }
}
void CheckPayload(bool compact, unsigned parity, bool current, bool fail_end) {
  using namespace crystalhd_test_vpp_payload;
  Surface source, target, field;
  Require(source.AllocateCpuNv12(kWidth, kHeight) &&
          target.AllocateCpuNv12(kWidth, kHeight), "payload CPU allocation");
  source.id = 1; target.id = 2;
  source.ready = true;
  source.expected_timestamp = source.frame_timestamp = 100;
  source.decode_identity = 101;
  FillPayloadSource(&source);
  crystalhd_vaapi_trace::Fields binding;
  binding.context = 3; binding.generation = 4; binding.token = 100;
  binding.decode_identity = 101; binding.submission_ordinal = 5;
  binding.surface = 2; binding.owner = 2; binding.operation = 6;
  std::unique_ptr<State> state(new State(7));
  Require(state->directory >= 0, "private output directory admitted");
  VppDeinterlace bob;
  bob.bob = true;
  bob.field = static_cast<crystalhd_vpp_bob::FieldParity>(parity);
  Surface *input = &source;
  if (compact) {
    Require(field.AllocateCpuNv12(kWidth, kHeight / 2), "compact CPU allocation");
    field.ready = true;
    field.expected_timestamp = field.frame_timestamp = 100;
    field.payload_observer_stamp.first = {true, parity == 1, parity == 1,
                                         4, 101, 100, 9, 10, 11};
    for (unsigned plane = 0; plane != 2; ++plane) {
      const unsigned rows = plane == 0 ? field.height : field.height / 2;
      for (unsigned row = 0; row != rows; ++row)
        memcpy(field.planes[plane] + size_t{row} * field.pitch[plane],
               source.planes[plane] + size_t{row * 2 + parity} * source.pitch[plane], kWidth);
    }
    input = &field;
  }
  source.object_fds.push_back(777); target.object_fds.push_back(888);
  if (compact) field.object_fds.push_back(777);
  fail_payload_target_end = fail_end;
  Scaler scaler;
  std::vector<uint8_t> staging;
  VAStatus status;
  {
    Scope scope(state.get(), binding, source, source, target, Full(source),
                Full(target), bob, uint64_t{6});
    status = ProcessVpp(&scaler.value, &staging, input, &target,
                        Full(source), Full(target), false, bob, compact);
  }
  fail_payload_target_end = false;
  source.object_fds.clear(); target.object_fds.clear(); field.object_fds.clear();
  Slot &slot = state->slots[parity];
  Require(slot.selected && slot.copy_ok &&
          slot.source_bytes == size_t{kWidth} * input->height * 3 / 2 &&
          slot.target_bytes == kPlaneBytes, "bounded active-row snapshots captured");
  Require(slot.source.serial == input->payload_observer_stamp.serial &&
          slot.target.serial == target.payload_observer_stamp.serial,
          "actual source/target storage views captured");
  Require(slot.process_success == !fail_end, "failed CPU end cannot mark process success");
  state->Commit(6, status, status == VA_STATUS_SUCCESS, current);
  Require(slot.committed == (current && !fail_end),
          "only successful current completion publishes tentative snapshot");
  const uint64_t first_operation = slot.operation;
  {
    Scope retry(state.get(), binding, source, source, target, Full(source),
                Full(target), bob, uint64_t{8});
    Require(active == nullptr, "selected slot cannot be retried or overwritten");
  }
  Require(slot.operation == first_operation, "original selection preserved");
  const auto old_epoch = source.payload_observer_stamp.epoch;
  Require(source.AllocateCpuNv12(kWidth, kHeight) &&
          source.payload_observer_stamp.epoch == old_epoch + 1,
          "storage epoch advances even for identical replacement geometry");
}
}  // namespace

extern "C" int __wrap_ioctl(int fd, unsigned long request, ...) {
  va_list args;
  va_start(args, request);
  const auto *sync = va_arg(args, const dma_buf_sync *);
  va_end(args);
  if (fail_payload_target_end && fd == 888 && request == DMA_BUF_IOCTL_SYNC &&
      sync->flags == (DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE)) {
    errno = EIO;
    return -1;
  }
  return CrystalhdExistingBobIoctl(fd, request, sync);
}

int main() {
  char directory[] = "/tmp/crystalhd-vpp-payload-cpu-XXXXXX";
  if (mkdtemp(directory) == nullptr ||
      setenv("CRYSTALHD_TEST_VPP_PAYLOAD_DIR", directory, 1) != 0) return 1;
  int result = 0;
  try {
    for (unsigned parity = 0; parity != 2; ++parity) {
      CheckPayload(false, parity, true, false);
      CheckPayload(true, parity, true, false);
    }
    CheckPayload(false, 0, false, false);
    CheckPayload(false, 0, true, true);
    Require(crystalhd_test_vpp_payload::active == nullptr,
            "scope does not retain payload state after conversion");
    std::printf("VA private payload hook CPU controls: %u checks PASS\n", checks);
  } catch (const std::exception &error) {
    std::fprintf(stderr, "VA private payload hook CPU FAIL: %s\n", error.what());
    result = 1;
  }
  unsetenv("CRYSTALHD_TEST_VPP_PAYLOAD_DIR");
  if (rmdir(directory) != 0) result = 1;
  return result;
}
