// SPDX-License-Identifier: LGPL-2.1-or-later
// Isolated real EndPicture early-return controls using existing CPU DIL mocks.
#define CRYSTALHD_VAAPI_TEST_MAINTAINED_COST 1
#define CRYSTALHD_VAAPI_TEST_ENDPICTURE_BODY_COST 1
#define main CrystalHDExistingReplayMain
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsubobject-linkage"
#endif
#include "vaapi-replay.cpp"
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
#undef main

int main() {
  namespace cost = crystalhd_test_maintained_cost;
  Require(cost::state.enabled, "explicit cost environment before process startup");
  Require(cost::state.attempted == 0, "fresh collector has no earlier operations");
  Driver driver;
  VADriverContext context{};
  context.pDriverData = &driver;
  Require(EndPicture(&context, 99) == VA_STATUS_ERROR_INVALID_CONTEXT,
          "invalid context preserves the actual API result");
  auto decode = std::make_shared<DecodeContext>();
  decode->id = 1;
  decode->generation = 1;
  driver.contexts[1] = decode;
  Require(EndPicture(&context, 1) == VA_STATUS_ERROR_INVALID_CONFIG,
          "missing decode configuration preserves the actual API result");
  decode->closing = true;
  Require(EndPicture(&context, 1) == VA_STATUS_ERROR_INVALID_CONTEXT,
          "closing context preserves the actual API result");
  decode->closing = false;
  decode->video_process = true;
  Require(EndPicture(&context, 1) == VA_STATUS_ERROR_INVALID_PARAMETER,
          "missing VPP parameters preserve the actual API result");
  Require(cost::state.attempted == 4 && cost::state.inflight == 0,
          "each actual EndPicture early return closes one body scope");
  const uint64_t details[] = {0, 1, 0, 2};
  for (unsigned n = 0; n < 4; ++n) {
    const auto &event = cost::state.events[n];
    Require(event.ready.load() && event.stage == cost::Stage::EndPictureBody &&
                event.parent == 0 && event.detail == details[n] &&
                !event.has_result && event.identity.token == 0 &&
                event.identity.decode == 0,
            "body/context-kind is observed without fabricating API status or picture identity");
  }
  Require(cost::state.events[1].phase == cost::Phase::Admission &&
              cost::state.events[3].phase == cost::Phase::Admission &&
              cost::state.events[1].identity.context == 1 &&
              cost::state.events[3].identity.context == 1,
          "validated API context is bound after lock acquisition");
  Require(cost::state.clock_failures == 0 && cost::state.lifecycle_failures == 0,
          "paired clocks and scope retirement remain complete");
  std::puts("EndPicture body: four real early-return controls pass; no hardware/media");
  return 0;
}
