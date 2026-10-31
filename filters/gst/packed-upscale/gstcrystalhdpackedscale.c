/* SPDX-License-Identifier: MIT
 * Project-owned narrow packed resize filter. Source-only scaffold pending root
 * compilation, CPU oracle/lifecycle checks and actual pool/media qualification.
 */
#include <gst/video/gstvideofilter.h>
#include "packed-scale-engine.h"

typedef struct {
  GstVideoFilter parent;
  GMutex engine_lock;
  CrystalHdPackedScaleEngine *engine;
  gboolean unsupported_meta;
  gboolean simd_active;
  guint64 frames;
} GstCrystalHdPackedScale;
typedef struct { GstVideoFilterClass parent; } GstCrystalHdPackedScaleClass;

G_DEFINE_TYPE(GstCrystalHdPackedScale, gst_crystalhd_packed_scale,
    GST_TYPE_VIDEO_FILTER)

#define PACKED_SCALE(obj) ((GstCrystalHdPackedScale *)(obj))
enum { PROP_0, PROP_METHOD, PROP_THREADS, PROP_SIMD_ACTIVE, PROP_FRAMES };
#define INPUT_CAPS "video/x-raw,format=YUY2,width=1280,height=720," \
    "pixel-aspect-ratio=1/1,framerate=50/1,interlace-mode=progressive"
#define OUTPUT_CAPS "video/x-raw,format=UYVY,width=1920,height=1080," \
    "pixel-aspect-ratio=1/1,framerate=50/1,interlace-mode=progressive"

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE("sink",
    GST_PAD_SINK, GST_PAD_ALWAYS, GST_STATIC_CAPS(INPUT_CAPS));
static GstStaticPadTemplate source_template = GST_STATIC_PAD_TEMPLATE("src",
    GST_PAD_SRC, GST_PAD_ALWAYS, GST_STATIC_CAPS(OUTPUT_CAPS));

static GstCaps *packed_transform_caps(GstBaseTransform *base,
    GstPadDirection direction, GstCaps *caps, GstCaps *filter)
{
  (void)base;
  gboolean forward = direction == GST_PAD_SINK;
  GstCaps *from = gst_static_pad_template_get_caps(
      forward ? &sink_template : &source_template);
  GstCaps *to = gst_static_pad_template_get_caps(
      forward ? &source_template : &sink_template);
  GstCaps *result = gst_caps_intersect_full(caps, from,
      GST_CAPS_INTERSECT_FIRST);
  gst_caps_unref(from);
  result = gst_caps_make_writable(result);
  for (guint i = 0; i < gst_caps_get_size(result); i++) {
    GstStructure *s = gst_caps_get_structure(result, i);
    gst_structure_set(s, "format", G_TYPE_STRING, forward ? "UYVY" : "YUY2",
        "width", G_TYPE_INT, forward ? 1920 : 1280,
        "height", G_TYPE_INT, forward ? 1080 : 720, NULL);
  }
  GstCaps *bounded = gst_caps_intersect_full(result, to,
      GST_CAPS_INTERSECT_FIRST);
  gst_caps_unref(result);
  gst_caps_unref(to);
  if (filter) {
    result = gst_caps_intersect_full(filter, bounded, GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref(bounded);
    return result;
  }
  return bounded;
}

static gboolean packed_set_info(GstVideoFilter *video, GstCaps *input_caps,
    GstVideoInfo *input, GstCaps *output_caps, GstVideoInfo *output)
{
  (void)input_caps;
  (void)output_caps;
  GstCrystalHdPackedScale *s = PACKED_SCALE(video);
  GError *error = NULL;
  CrystalHdPackedScaleEngine *replacement =
      crystalhd_packed_scale_engine_new(input, output, &error);
  if (!replacement) {
    GST_ERROR_OBJECT(s, "Unsupported packed resize or setup failed: %s",
        error ? error->message : "unknown error");
    g_clear_error(&error);
    return FALSE;
  }
  g_mutex_lock(&s->engine_lock);
  crystalhd_packed_scale_engine_free(s->engine);
  s->engine = replacement;
  s->simd_active = crystalhd_packed_scale_engine_simd_active(replacement);
  s->unsupported_meta = FALSE;
  g_mutex_unlock(&s->engine_lock);
  return TRUE;
}

static gboolean packed_copy_metadata(GstBaseTransform *base,
    GstBuffer *input, GstBuffer *output)
{
  PACKED_SCALE(base)->unsupported_meta = FALSE;
  GstBaseTransformClass *parent =
      GST_BASE_TRANSFORM_CLASS(gst_crystalhd_packed_scale_parent_class);
  return !parent->copy_metadata || parent->copy_metadata(base, input, output);
}

static gboolean packed_transform_meta(GstBaseTransform *base,
    GstBuffer *output, GstMeta *meta, GstBuffer *input)
{
  (void)output;
  (void)input;
  /* The base skips memory and memory-reference metadata before this callback,
   * leaving the output pool's own GstVideoMeta intact. Arbitrary spatial meta
   * transformations have not been qualified for this narrow implementation. */
  if (gst_meta_api_type_has_tag(meta->info->api,
          g_quark_from_static_string(GST_META_TAG_VIDEO_SIZE_STR))) {
    PACKED_SCALE(base)->unsupported_meta = TRUE;
    return FALSE;
  }
  /* BaseTransform invokes the meta's normal COPY transform for true results.
   * Nonspatial metadata retains original timestamps/semantics. */
  return TRUE;
}

static gboolean packed_decide_allocation(GstBaseTransform *base, GstQuery *query)
{
  GstBaseTransformClass *parent =
      GST_BASE_TRANSFORM_CLASS(gst_crystalhd_packed_scale_parent_class);
  if (parent->decide_allocation && !parent->decide_allocation(base, query))
    return FALSE;
  /* Preserve the downstream pool selection, byte size and min/max limits. */
  if (!gst_query_find_allocation_meta(query, GST_VIDEO_META_API_TYPE, NULL))
    gst_query_add_allocation_meta(query, GST_VIDEO_META_API_TYPE, NULL);
  return TRUE;
}

static gboolean packed_propose_allocation(GstBaseTransform *base,
    GstQuery *decide_query, GstQuery *query)
{
  GstBaseTransformClass *parent =
      GST_BASE_TRANSFORM_CLASS(gst_crystalhd_packed_scale_parent_class);
  if (parent->propose_allocation &&
      !parent->propose_allocation(base, decide_query, query))
    return FALSE;
  if (!gst_query_find_allocation_meta(query, GST_VIDEO_META_API_TYPE, NULL))
    gst_query_add_allocation_meta(query, GST_VIDEO_META_API_TYPE, NULL);
  return TRUE;
}

static GstFlowReturn packed_transform_frame(GstVideoFilter *video,
    GstVideoFrame *input, GstVideoFrame *output)
{
  GstCrystalHdPackedScale *s = PACKED_SCALE(video);
  if (s->unsupported_meta || GST_VIDEO_FRAME_IS_INTERLACED(input) ||
      (GST_VIDEO_FRAME_FLAGS(input) & (GST_VIDEO_FRAME_FLAG_ONEFIELD |
          GST_VIDEO_FRAME_FLAG_MULTIPLE_VIEW))) {
    GST_ELEMENT_ERROR(s, STREAM, FORMAT,
        ("Packed resize received unqualified spatial metadata or frame layout"),
        (NULL));
    return GST_FLOW_ERROR;
  }
  GError *error = NULL;
  g_mutex_lock(&s->engine_lock);
  if (!s->engine) {
    g_mutex_unlock(&s->engine_lock);
    return GST_FLOW_NOT_NEGOTIATED;
  }
  gboolean ok = crystalhd_packed_scale_engine_run(s->engine, input, output,
      &error);
  if (ok && s->frames < G_MAXUINT64) s->frames++;
  g_mutex_unlock(&s->engine_lock);
  if (!ok) {
    GST_ELEMENT_ERROR(s, STREAM, FAILED, ("Packed resize failed"),
        ("%s", error ? error->message : "unknown error"));
    g_clear_error(&error);
    return GST_FLOW_ERROR;
  }
  return GST_FLOW_OK;
}

static gboolean packed_stop(GstBaseTransform *base)
{
  GstCrystalHdPackedScale *s = PACKED_SCALE(base);
  g_mutex_lock(&s->engine_lock);
  crystalhd_packed_scale_engine_free(s->engine);
  s->engine = NULL;
  s->unsupported_meta = FALSE;
  g_mutex_unlock(&s->engine_lock);
  return TRUE;
}

static void packed_finalize(GObject *object)
{
  GstCrystalHdPackedScale *s = PACKED_SCALE(object);
  packed_stop(GST_BASE_TRANSFORM(object));
  g_mutex_clear(&s->engine_lock);
  G_OBJECT_CLASS(gst_crystalhd_packed_scale_parent_class)->finalize(object);
}

static void packed_get_property(GObject *object, guint property,
    GValue *value, GParamSpec *spec)
{
  GstCrystalHdPackedScale *s = PACKED_SCALE(object);
  g_mutex_lock(&s->engine_lock);
  switch (property) {
    case PROP_METHOD: g_value_set_int(value, 4); break;
    case PROP_THREADS: g_value_set_uint(value, 2); break;
    case PROP_SIMD_ACTIVE: g_value_set_boolean(value, s->simd_active); break;
    case PROP_FRAMES: g_value_set_uint64(value, s->frames); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property, spec); break;
  }
  g_mutex_unlock(&s->engine_lock);
}

static void gst_crystalhd_packed_scale_class_init(GstCrystalHdPackedScaleClass *klass)
{
  GstElementClass *element = GST_ELEMENT_CLASS(klass);
  GstBaseTransformClass *base = GST_BASE_TRANSFORM_CLASS(klass);
  GstVideoFilterClass *video = GST_VIDEO_FILTER_CLASS(klass);
  G_OBJECT_CLASS(klass)->finalize = packed_finalize;
  G_OBJECT_CLASS(klass)->get_property = packed_get_property;
  g_object_class_install_property(G_OBJECT_CLASS(klass), PROP_METHOD,
      g_param_spec_int("method", "Method", "Fixed method4 bilinear automatic taps",
          4, 4, 4, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property(G_OBJECT_CLASS(klass), PROP_THREADS,
      g_param_spec_uint("n-threads", "Threads", "Fixed main plus one compute worker",
          2, 2, 2, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property(G_OBJECT_CLASS(klass), PROP_SIMD_ACTIVE,
      g_param_spec_boolean("simd-active", "SIMD active",
          "SIMD path selected by the current or most recent negotiated plan",
          FALSE, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property(G_OBJECT_CLASS(klass), PROP_FRAMES,
      g_param_spec_uint64("frames", "Frames",
          "Successful transforms over this instance lifetime; not output submissions",
          0, G_MAXUINT64, 0, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
  gst_element_class_add_static_pad_template(element, &sink_template);
  gst_element_class_add_static_pad_template(element, &source_template);
  gst_element_class_set_static_metadata(element, "CrystalHD narrow packed resize",
      "Filter/Converter/Video/Scaler",
      "Explicit experimental 720p YUY2 to1080p UYVY method4 packed resize",
      "CrystalHD contributors");
  base->transform_caps = packed_transform_caps;
  base->decide_allocation = packed_decide_allocation;
  base->propose_allocation = packed_propose_allocation;
  base->copy_metadata = packed_copy_metadata;
  base->transform_meta = packed_transform_meta;
  base->stop = packed_stop;
  base->passthrough_on_same_caps = FALSE;
  video->set_info = packed_set_info;
  video->transform_frame = packed_transform_frame;
}

static void gst_crystalhd_packed_scale_init(GstCrystalHdPackedScale *s)
{
  g_mutex_init(&s->engine_lock);
  gst_base_transform_set_in_place(GST_BASE_TRANSFORM(s), FALSE);
  gst_base_transform_set_passthrough(GST_BASE_TRANSFORM(s), FALSE);
  /* Normal stock videoconvert/videoscale enable BaseTransform QoS. Keep that
   * mechanism and unchanged event forwarding; never retimestamp buffers. */
  gst_base_transform_set_qos_enabled(GST_BASE_TRANSFORM(s), TRUE);
}

static gboolean plugin_init(GstPlugin *plugin)
{
  return gst_element_register(plugin, "crystalhdpackedscale", GST_RANK_NONE,
      gst_crystalhd_packed_scale_get_type());
}
#ifndef PACKAGE
#define PACKAGE "crystalhd"
#endif
GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, crystalhdpackedscale,
    "Optional narrow packed CrystalHD resize", plugin_init, "1.3.0-dev.1",
    "MIT/X11", "CrystalHD", "https://github.com/ahnhy1324/crystalhd")
