/* Original CrystalHD prototype. Caller owns/quiesces engine lifetime. */
#ifndef CRYSTALHD_PACKED_SCALE_ENGINE_H
#define CRYSTALHD_PACKED_SCALE_ENGINE_H
#include <gst/video/video.h>
G_BEGIN_DECLS
typedef struct _CrystalHdPackedScaleEngine CrystalHdPackedScaleEngine;
/* Gst1.28.2 only: progressive50Hz, singleview1280x720YUY2/PAR1 to
 *1920x1080UYVY/PAR1, matching known color/chroma, exactly two taps. */
CrystalHdPackedScaleEngine *crystalhd_packed_scale_engine_new(
    const GstVideoInfo *input, const GstVideoInfo *output, GError **error);
/* Synchronous main+one worker, actual positive mapped strides/offsets.
 * Input readable/output writable, nonoverlapping mapped spans required.
 * Reads/writes active bytes only, no padding dependency, no metadata changes
 * or GstBuffer refs retained after return. Concurrent runs serialize.
 * Caller MUST keep engine alive: serialize free/reconfiguration against all
 * run entries/waiters using the wrapper engine-lifetime lock. */
gboolean crystalhd_packed_scale_engine_run(CrystalHdPackedScaleEngine *engine,
    const GstVideoFrame *input, GstVideoFrame *output, GError **error);
gboolean crystalhd_packed_scale_engine_simd_active(const CrystalHdPackedScaleEngine *engine);
/* After caller quiescence, joins worker before scalers/table/cache release. */
void crystalhd_packed_scale_engine_free(CrystalHdPackedScaleEngine *engine);
G_END_DECLS
#endif
