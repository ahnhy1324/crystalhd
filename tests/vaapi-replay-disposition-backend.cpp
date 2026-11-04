// SPDX-License-Identifier: LGPL-2.1-or-later
// Explicit OWN private diagnostic artifact; ordinary packages exclude it.
#define CRYSTALHD_VAAPI_TEST_CENTERED_PAYLOAD 1
#define CRYSTALHD_VAAPI_TEST_MAINTAINED_COST 1
#define CRYSTALHD_VAAPI_TEST_REPLAY_DISPOSITION 1
#include "vaapi-centered-readback-observer.h"
#include "vaapi-centered-cow-image-observer.h"
#define CRYSTALHD_VAAPI_TEST_COW_BORDER_COMPOSE(reused) \
  do { \
    /* Compose's backing proof limits this private counter to canonical, */ \
    /* noncompact decoded-public memfd targets. Other routes stay uncounted. */ \
    if (border_backing) crystalhd_test_centered_cow_image::Composed(reused); \
  } while (false)
#define CRYSTALHD_VAAPI_TEST_COW_DERIVED_IMAGE(surface, image, bytes) \
  crystalhd_test_centered_cow_image::Capture( \
      driver->trace.session(), TraceFields(nullptr, &(surface)), (image), (bytes))
#define CRYSTALHD_VAAPI_TEST_CENTERED_READBACK(surface, image, bytes) \
  crystalhd_test_centered_readback::Capture( \
      driver->trace.session(), TraceFields(nullptr, &(surface)), (image), (bytes), \
      (surface).cpu_primary)
#define CRYSTALHD_VAAPI_TEST_CPU_PUBLIC_READBACK(surface, image, bytes) \
  crystalhd_test_centered_readback::CaptureCpuPrimary( \
      driver->trace.session(), TraceFields(nullptr, &(surface)), (image), (bytes))
#include "../filters/vaapi/crystalhd_drv_video.cpp"
