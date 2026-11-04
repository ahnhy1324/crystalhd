// SPDX-License-Identifier: LGPL-2.1-or-later
// Explicit OWN private diagnostic artifact; ordinary packages exclude it.
#define CRYSTALHD_VAAPI_TEST_CENTERED_PAYLOAD 1
#define CRYSTALHD_VAAPI_TEST_MAINTAINED_COST 1
#define CRYSTALHD_VAAPI_TEST_REPLAY_DISPOSITION 1
#include "vaapi-centered-readback-observer.h"
#define CRYSTALHD_VAAPI_TEST_CENTERED_READBACK(surface, image, bytes) \
  crystalhd_test_centered_readback::Capture( \
      driver->trace.session(), TraceFields(nullptr, &(surface)), (image), (bytes))
#include "../filters/vaapi/crystalhd_drv_video.cpp"
