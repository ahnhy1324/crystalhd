/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Player-owned compatibility controller, not a decoder or pipeline filter. */
#include "gstcrystalhd-diqos.h"
#include <string.h>

typedef struct {
  gint references;
  GstElement *target;
  GstPad *sink, *src;
  GstBus *bus;
} CrystalHdDiQosResources;

typedef struct {
  gint references;
  GMutex lock;
  CrystalHdDiQosResources *resources;
  /* Borrowed only by the probe's destroy callback, while its pad is alive. */
  GstPad *marker_pad;
  GstCaps *sink_caps, *src_caps;
  gulong probe;
  gboolean closed, active, caps_accepted;
  guint64 qos, forwarded, failures;
} CrystalHdDiQosState;

typedef struct {
  GstElement parent;
  GMutex lock;
  CrystalHdDiQosState *state;
  gboolean disposing;
} GstCrystalHdDiQos;

typedef struct {
  GstElementClass parent_class;
  gint (*attach)(GstCrystalHdDiQos *, GstElement *, GstBus *);
  void (*detach)(GstCrystalHdDiQos *);
} GstCrystalHdDiQosClass;

enum {
  PROP_0, PROP_ATTACHED, PROP_ACTIVE, PROP_ELIGIBLE,
  PROP_QOS_COUNT, PROP_FORWARD_COUNT, PROP_FAILURE_COUNT
};

G_DEFINE_TYPE(GstCrystalHdDiQos, gst_crystalhd_di_qos, GST_TYPE_ELEMENT)

/* Never hold this lock while adding/removing a probe: its destroy callback
 * runs with the pad object lock held and needs to release this marker. */
static GMutex marker_lock;

static GQuark
marker_key(void)
{
  return g_quark_from_static_string("crystalhd-di-qos-owner");
}

static CrystalHdDiQosState *
state_ref(CrystalHdDiQosState *state)
{
  g_atomic_int_inc(&state->references);
  return state;
}

static CrystalHdDiQosResources *
resources_ref(CrystalHdDiQosResources *resources)
{
  g_atomic_int_inc(&resources->references);
  return resources;
}

static void
resources_unref(CrystalHdDiQosResources *resources)
{
  if (!resources || !g_atomic_int_dec_and_test(&resources->references))
    return;
  gst_object_unref(resources->sink);
  gst_object_unref(resources->src);
  gst_object_unref(resources->bus);
  gst_object_unref(resources->target);
  g_free(resources);
}

static void
state_unref(CrystalHdDiQosState *state)
{
  if (!g_atomic_int_dec_and_test(&state->references))
    return;
  gst_clear_caps(&state->sink_caps);
  gst_clear_caps(&state->src_caps);
  /* Object references are released by detach/callback outside pad locks. */
  g_mutex_clear(&state->lock);
  g_free(state);
}

static CrystalHdDiQosState *
controller_state(GstCrystalHdDiQos *self)
{
  CrystalHdDiQosState *state;
  g_mutex_lock(&self->lock);
  state = self->state ? state_ref(self->state) : NULL;
  g_mutex_unlock(&self->lock);
  return state;
}

static gboolean
properties_supported(GstElement *target)
{
  gint mode, method, fields, locking, tff;
  /* Fresh values on every QoS, including during frozen notify signals. */
  g_object_get(target, "mode", &mode, "method", &method, "fields", &fields,
               "locking", &locking, "tff", &tff, NULL);
  return mode == 0 && method == 4 && fields == 0 && locking == 0 && tff == 0;
}

static gboolean
target_supported(GstElement *target)
{
  guint major, minor, micro, nano;
  GstElementFactory *factory;
  GstPlugin *plugin;
  gboolean accepted;
  gst_version(&major, &minor, &micro, &nano);
  if (major != 1 || minor != 28 || micro != 2)
    return FALSE;
  factory = gst_element_get_factory(target);
  if (!factory || strcmp(gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory)),
                          "deinterlace"))
    return FALSE;
  plugin = gst_plugin_feature_get_plugin(GST_PLUGIN_FEATURE(factory));
  if (!plugin)
    return FALSE;
  accepted = !g_strcmp0(gst_plugin_get_version(plugin), "1.28.2");
  gst_object_unref(plugin);
  return accepted && properties_supported(target);
}

static gboolean
caps_supported(GstCaps *input, GstCaps *output)
{
  const GstStructure *source, *dest;
  const gchar *mode;
  gint sn, sd, dn, dd, sw, sh, dw, dh;
  if (gst_caps_get_size(input) != 1 || gst_caps_get_size(output) != 1)
    return FALSE;
  source = gst_caps_get_structure(input, 0);
  dest = gst_caps_get_structure(output, 0);
  mode = gst_structure_get_string(source, "interlace-mode");
  return gst_structure_has_name(source, "video/x-raw") &&
      gst_structure_has_name(dest, "video/x-raw") &&
      !g_strcmp0(gst_structure_get_string(source, "format"), "YUY2") &&
      !g_strcmp0(gst_structure_get_string(dest, "format"), "YUY2") &&
      (!g_strcmp0(mode, "mixed") || !g_strcmp0(mode, "interleaved")) &&
      !g_strcmp0(gst_structure_get_string(dest, "interlace-mode"), "progressive") &&
      gst_structure_get_fraction(source, "framerate", &sn, &sd) && sn == 25 && sd == 1 &&
      gst_structure_get_fraction(dest, "framerate", &dn, &dd) && dn == 50 && dd == 1 &&
      gst_structure_get_int(source, "width", &sw) && sw > 0 &&
      gst_structure_get_int(dest, "width", &dw) && sw == dw &&
      gst_structure_get_int(source, "height", &sh) && sh > 0 &&
      gst_structure_get_int(dest, "height", &dh) && sh == dh;
}

/* Caller holds state->lock. Only immutable caps policy is cached. */
static gboolean
state_eligible(CrystalHdDiQosState *state)
{
  GstCaps *input, *output;
  gboolean accepted = FALSE;
  if (state->closed || !state->active || !properties_supported(state->resources->target))
    return FALSE;
  input = gst_pad_get_current_caps(state->resources->sink);
  output = gst_pad_get_current_caps(state->resources->src);
  /* Set equality alone can admit duplicate structures; check sizes first. */
  if (input && output && gst_caps_get_size(input) == 1 && gst_caps_get_size(output) == 1) {
    if (state->sink_caps && gst_caps_is_equal(input, state->sink_caps) &&
        gst_caps_is_equal(output, state->src_caps)) {
      accepted = state->caps_accepted;
    } else {
      accepted = caps_supported(input, output);
      gst_caps_replace(&state->sink_caps, input);
      gst_caps_replace(&state->src_caps, output);
      state->caps_accepted = accepted;
    }
  }
  if (input)
    gst_caps_unref(input);
  if (output)
    gst_caps_unref(output);
  return accepted;
}

static GstPadProbeReturn
qos_event(GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
  CrystalHdDiQosState *state = data;
  CrystalHdDiQosResources *resources = NULL;
  GstEvent *event = GST_PAD_PROBE_INFO_EVENT(info);
  gboolean accepted, forwarded;
  (void)pad;
  if (!event || GST_EVENT_TYPE(event) != GST_EVENT_QOS)
    return GST_PAD_PROBE_OK;
  g_mutex_lock(&state->lock);
  if (state->closed) {
    g_mutex_unlock(&state->lock);
    return GST_PAD_PROBE_OK;
  }
  state->qos++;
  accepted = state_eligible(state);
  if (!accepted)
    state->active = FALSE;
  else
    resources = resources_ref(state->resources);
  g_mutex_unlock(&state->lock);
  if (!accepted)
    return GST_PAD_PROBE_OK;

  /* The probe owns a state ref until GStreamer's dispatch releases its hook.
   * Hold no helper/state lock across forwarding: upstream may detach us. */
  forwarded = gst_pad_push_event(resources->sink, gst_event_copy(event));
  g_mutex_lock(&state->lock);
  if (forwarded)
    state->forwarded++;
  else
    state->failures++;
  g_mutex_unlock(&state->lock);
  if (!forwarded) {
    GstStructure *message = gst_structure_new("crystalhd-player-output-error",
        "message", G_TYPE_STRING, "could not forward deinterlacer QoS feedback upstream", NULL);
    gst_bus_post(resources->bus, gst_message_new_application(GST_OBJECT(resources->target), message));
  }
  /* Final DI disposal can acquire its pad locks. Do it before returning to
   * GStreamer's dispatcher, which reacquires the source pad object lock. */
  resources_unref(resources);
  /* A failed push may already have reached upstream. Never retry it or let
   * the original update the stock deinterlacer's incorrect picture-PTS cache. */
  return GST_PAD_PROBE_DROP;
}

static void
probe_destroy(gpointer data)
{
  CrystalHdDiQosState *state = data;
  /* gstpad.c's GHook dispatch retains the hook while calling qos_event;
   * destroy runs after any in-flight callback, including reentrant detach.
   * Keep the marker until then so a replacement cannot overlap forwarding. */
  g_mutex_lock(&marker_lock);
  if (g_object_get_qdata(G_OBJECT(state->marker_pad), marker_key()) == state)
    g_object_set_qdata(G_OBJECT(state->marker_pad), marker_key(), NULL);
  g_mutex_unlock(&marker_lock);
  state_unref(state);
}

static void
state_detach(CrystalHdDiQosState *state)
{
  gulong probe;
  CrystalHdDiQosResources *resources;
  g_mutex_lock(&state->lock);
  state->closed = TRUE;
  probe = state->probe;
  state->probe = 0;
  resources = state->resources;
  state->resources = NULL;
  g_mutex_unlock(&state->lock);
  if (probe)
    gst_pad_remove_probe(resources->src, probe);
  resources_unref(resources);
}

static gint
attach(GstCrystalHdDiQos *self, GstElement *target, GstBus *bus)
{
  CrystalHdDiQosState *state;
  CrystalHdDiQosResources *resources;
  GstPad *sink, *src;
  if (!target || !bus || !GST_IS_ELEMENT(target) || !GST_IS_BUS(bus))
    return -1;
  if (!target_supported(target))
    return 0;
  sink = gst_element_get_static_pad(target, "sink");
  src = gst_element_get_static_pad(target, "src");
  if (!sink || !src) {
    if (sink)
      gst_object_unref(sink);
    if (src)
      gst_object_unref(src);
    return 0;
  }
  g_mutex_lock(&self->lock);
  g_mutex_lock(&marker_lock);
  if (self->state || self->disposing ||
      g_object_get_qdata(G_OBJECT(src), marker_key())) {
    g_mutex_unlock(&marker_lock);
    g_mutex_unlock(&self->lock);
    gst_object_unref(sink);
    gst_object_unref(src);
    return -1;
  }
  state = g_new0(CrystalHdDiQosState, 1);
  state->references = 1;
  g_mutex_init(&state->lock);
  resources = g_new0(CrystalHdDiQosResources, 1);
  resources->references = 1;
  resources->target = gst_object_ref(target);
  resources->bus = gst_object_ref(bus);
  resources->sink = sink;
  resources->src = src;
  state->resources = resources;
  state->marker_pad = src;
  state->active = TRUE;
  g_object_set_qdata(G_OBJECT(src), marker_key(), state);
  g_mutex_unlock(&marker_lock);
  /* Non-IDLE probes do not invoke the callback synchronously on install. */
  state->probe = gst_pad_add_probe(src, GST_PAD_PROBE_TYPE_EVENT_UPSTREAM,
                                  qos_event, state_ref(state), probe_destroy);
  self->state = state;
  g_mutex_unlock(&self->lock);
  return 1;
}

static void
detach(GstCrystalHdDiQos *self)
{
  CrystalHdDiQosState *state = controller_state(self);
  if (state) {
    state_detach(state);
    state_unref(state);
  }
}

static void
get_property(GObject *object, guint property, GValue *value, GParamSpec *spec)
{
  CrystalHdDiQosState *state = controller_state((GstCrystalHdDiQos *)object);
  gboolean flag = FALSE;
  guint64 count = 0;
  if (state) {
    g_mutex_lock(&state->lock);
    switch (property) {
      case PROP_ATTACHED: flag = !state->closed; break;
      case PROP_ACTIVE: flag = !state->closed && state->active; break;
      case PROP_ELIGIBLE: flag = state_eligible(state); break;
      case PROP_QOS_COUNT: count = state->qos; break;
      case PROP_FORWARD_COUNT: count = state->forwarded; break;
      case PROP_FAILURE_COUNT: count = state->failures; break;
      default: break;
    }
    g_mutex_unlock(&state->lock);
    state_unref(state);
  }
  switch (property) {
    case PROP_ATTACHED: case PROP_ACTIVE: case PROP_ELIGIBLE:
      g_value_set_boolean(value, flag); break;
    case PROP_QOS_COUNT: case PROP_FORWARD_COUNT: case PROP_FAILURE_COUNT:
      g_value_set_uint64(value, count); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property, spec); break;
  }
}

static void
dispose(GObject *object)
{
  GstCrystalHdDiQos *self = (GstCrystalHdDiQos *)object;
  CrystalHdDiQosState *state;
  g_mutex_lock(&self->lock);
  self->disposing = TRUE;
  state = self->state;
  self->state = NULL;
  g_mutex_unlock(&self->lock);
  if (state) {
    state_detach(state);
    state_unref(state);
  }
  G_OBJECT_CLASS(gst_crystalhd_di_qos_parent_class)->dispose(object);
}

static void
finalize(GObject *object)
{
  g_mutex_clear(&((GstCrystalHdDiQos *)object)->lock);
  G_OBJECT_CLASS(gst_crystalhd_di_qos_parent_class)->finalize(object);
}

static void
gst_crystalhd_di_qos_class_init(GstCrystalHdDiQosClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GParamFlags flags = G_PARAM_READABLE | G_PARAM_STATIC_STRINGS;
  object_class->get_property = get_property;
  object_class->dispose = dispose;
  object_class->finalize = finalize;
  klass->attach = attach;
  klass->detach = detach;
  gst_element_class_set_static_metadata(GST_ELEMENT_CLASS(klass),
      "CrystalHD player deinterlace QoS controller", "Generic",
      "Scoped GStreamer 1.28.2 second-field QoS compatibility controller",
      "CrystalHD contributors");
  g_signal_new("attach", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
      G_STRUCT_OFFSET(GstCrystalHdDiQosClass, attach), NULL, NULL, NULL,
      G_TYPE_INT, 2, GST_TYPE_ELEMENT, GST_TYPE_BUS);
  g_signal_new("detach", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
      G_STRUCT_OFFSET(GstCrystalHdDiQosClass, detach), NULL, NULL, NULL,
      G_TYPE_NONE, 0);
  g_object_class_install_property(object_class, PROP_ATTACHED,
      g_param_spec_boolean("attached", "Attached", "A probe is attached", FALSE, flags));
  g_object_class_install_property(object_class, PROP_ACTIVE,
      g_param_spec_boolean("active", "Active", "Stock QoS policy has not been latched", FALSE, flags));
  g_object_class_install_property(object_class, PROP_ELIGIBLE,
      g_param_spec_boolean("eligible", "Eligible", "Current properties and caps qualify", FALSE, flags));
  g_object_class_install_property(object_class, PROP_QOS_COUNT,
      g_param_spec_uint64("qos-count", "QoS count", "QoS events inspected", 0, G_MAXUINT64, 0, flags));
  g_object_class_install_property(object_class, PROP_FORWARD_COUNT,
      g_param_spec_uint64("forward-count", "Forward count", "Successful upstream QoS forwards", 0, G_MAXUINT64, 0, flags));
  g_object_class_install_property(object_class, PROP_FAILURE_COUNT,
      g_param_spec_uint64("failure-count", "Failure count", "Failed upstream QoS forwards", 0, G_MAXUINT64, 0, flags));
}

static void
gst_crystalhd_di_qos_init(GstCrystalHdDiQos *self)
{
  g_mutex_init(&self->lock);
}
