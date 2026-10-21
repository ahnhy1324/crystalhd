/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* CPU-only real GStreamer DI policy/lifetime tests. No decoder or DIL linkage. */
#include <gst/check/gstharness.h>
#include <gst/video/video.h>
#include <string.h>

static gboolean test_wrong_version;
static void
test_core_version(guint *major, guint *minor, guint *micro, guint *nano)
{
  gst_version(major, minor, micro, nano);
  if (test_wrong_version)
    *micro += 1;
}

/* Exercise unsupported core admission without changing installed libraries. */
#define gst_version test_core_version
#include "../filters/gst/gst-plugin-1.0/gstcrystalhd-diqos.c"
#undef gst_version

#define INPUT_CAPS "video/x-raw,format=YUY2,width=16,height=8," \
  "interlace-mode=mixed,framerate=25/1,pixel-aspect-ratio=1/1"
#define OUTPUT_CAPS "video/x-raw,format=YUY2,width=16,height=8," \
  "interlace-mode=progressive,framerate=50/1,pixel-aspect-ratio=1/1"

typedef struct {
  GstElement *di, *helper;
  GstBus *bus;
  GstPad *sink, *src, *upstream;
  GMutex mutex;
  GCond cond;
  guint events;
  gboolean fail_forward, reentrant, release_owners, block, entered, release;
} TestFixture;

static GstElement *
test_helper_new(void)
{
  return gst_object_ref_sink(g_object_new(GST_TYPE_CRYSTALHD_DI_QOS, NULL));
}

static gint
test_attach(GstElement *helper, GstElement *target, GstBus *bus)
{
  gint result = -99;
  g_signal_emit_by_name(helper, "attach", target, bus, &result);
  return result;
}

static gboolean
test_boolean(GstElement *helper, const gchar *name)
{
  gboolean value = FALSE;
  g_object_get(helper, name, &value, NULL);
  return value;
}

static guint64
test_count(GstElement *helper, const gchar *name)
{
  guint64 value = 0;
  g_object_get(helper, name, &value, NULL);
  return value;
}

static GstEvent *
test_qos(void)
{
  GstEvent *event = gst_event_new_qos(GST_QOS_TYPE_UNDERFLOW, 1.25,
      GST_MSECOND, 20 * GST_MSECOND);
  gst_event_set_seqnum(event, 0x2345);
  gst_event_set_running_time_offset(event, 123456);
  return event;
}

static gboolean
test_upstream_event(GstPad *pad, GstObject *parent, GstEvent *event)
{
  TestFixture *f = g_object_get_data(G_OBJECT(pad), "test-fixture");
  (void) parent;
  if (GST_EVENT_TYPE(event) == GST_EVENT_QOS) {
    GstQOSType type;
    gdouble proportion;
    GstClockTimeDiff diff;
    GstClockTime timestamp;
    GstClockTime expected_timestamp;
    GstEvent *expected = test_qos();
    gst_event_parse_qos(expected, NULL, NULL, NULL, &expected_timestamp);
    gst_event_parse_qos(event, &type, &proportion, &diff, &timestamp);
    g_assert_cmpint(type, ==, GST_QOS_TYPE_UNDERFLOW);
    g_assert_cmpfloat(proportion, ==, 1.25);
    g_assert_cmpint(diff, ==, GST_MSECOND);
    g_assert_cmpuint(timestamp, ==, expected_timestamp);
    g_assert_true(gst_structure_is_equal(gst_event_get_structure(event),
        gst_event_get_structure(expected)));
    g_assert_cmpuint(gst_event_get_seqnum(event), ==, 0x2345);
    g_assert_cmpint(gst_event_get_running_time_offset(event), ==, 123456);
    gst_event_unref(expected);
    g_mutex_lock(&f->mutex);
    f->events++;
    f->entered = TRUE;
    g_cond_broadcast(&f->cond);
    while (f->block && !f->release)
      g_cond_wait(&f->cond, &f->mutex);
    g_mutex_unlock(&f->mutex);
    if (f->reentrant)
      g_signal_emit_by_name(f->helper, "detach");
    if (f->release_owners) {
      gst_object_unref(f->helper);
      f->helper = NULL;
      gst_object_unref(f->di);
      f->di = NULL;
    }
    gst_event_unref(event);
    return !f->fail_forward;
  }
  gst_event_unref(event);
  return TRUE;
}

static void
test_store_caps(GstPad *pad, const gchar *text)
{
  GstCaps *caps = gst_caps_from_string(text);
  GstEvent *event = gst_event_new_caps(caps);
  g_assert_cmpint(gst_pad_store_sticky_event(pad, event), ==, GST_FLOW_OK);
  gst_event_unref(event);
  gst_caps_unref(caps);
}

static void
test_fixture_init(TestFixture *f)
{
  memset(f, 0, sizeof(*f));
  g_mutex_init(&f->mutex);
  g_cond_init(&f->cond);
  f->di = gst_object_ref_sink(gst_element_factory_make("deinterlace", NULL));
  f->helper = test_helper_new();
  f->bus = gst_bus_new();
  f->sink = gst_element_get_static_pad(f->di, "sink");
  f->src = gst_element_get_static_pad(f->di, "src");
  f->upstream = gst_object_ref_sink(gst_pad_new("upstream", GST_PAD_SRC));
  g_object_set_data(G_OBJECT(f->upstream), "test-fixture", f);
  gst_pad_set_event_function(f->upstream, test_upstream_event);
  g_assert_true(gst_pad_set_active(f->upstream, TRUE));
  g_assert_true(gst_pad_set_active(f->sink, TRUE));
  g_assert_true(gst_pad_set_active(f->src, TRUE));
  g_assert_cmpint(gst_pad_link(f->upstream, f->sink), ==, GST_PAD_LINK_OK);
  test_store_caps(f->sink, INPUT_CAPS);
  test_store_caps(f->src, OUTPUT_CAPS);
}

static void
test_fixture_clear(TestFixture *f)
{
  if (f->helper) {
    g_signal_emit_by_name(f->helper, "detach");
    gst_object_unref(f->helper);
  }
  gst_pad_unlink(f->upstream, f->sink);
  gst_pad_set_active(f->upstream, FALSE);
  gst_pad_set_active(f->sink, FALSE);
  gst_pad_set_active(f->src, FALSE);
  gst_object_unref(f->upstream);
  gst_object_unref(f->sink);
  gst_object_unref(f->src);
  if (f->di) gst_object_unref(f->di);
  gst_object_unref(f->bus);
  g_cond_clear(&f->cond);
  g_mutex_clear(&f->mutex);
}

static gboolean
test_admitted_runtime(void)
{
  guint major, minor, micro, nano;
  GstElement *di = gst_element_factory_make("deinterlace", NULL);
  GstElementFactory *factory;
  GstPlugin *plugin;
  gboolean supported;
  gst_version(&major, &minor, &micro, &nano);
  if (!di) {
    g_test_skip("deinterlace plugin unavailable");
    return FALSE;
  }
  factory = gst_element_get_factory(di);
  plugin = gst_plugin_feature_get_plugin(GST_PLUGIN_FEATURE(factory));
  supported = major == 1 && minor == 28 && micro == 2 && plugin &&
      !g_strcmp0(gst_plugin_get_version(plugin), "1.28.2");
  if (plugin)
    gst_object_unref(plugin);
  gst_object_unref(di);
  if (!supported)
    g_test_skip("exact GStreamer core/plugin 1.28.2 required for admitted policy");
  return supported;
}

static void
test_admission(void)
{
  GstElement *helper = test_helper_new();
  GstElement *wrong = gst_element_factory_make("identity", NULL);
  GstBus *bus = gst_bus_new();
  GstElementFactory *factory = gst_element_factory_find("crystalhddiqos");
  g_assert_nonnull(factory);
  g_assert_cmpuint(gst_plugin_feature_get_rank(GST_PLUGIN_FEATURE(factory)), ==,
      GST_RANK_NONE);
  g_assert_null(GST_ELEMENT_PADS(helper));
  g_assert_null(GST_OBJECT_PARENT(helper));
  g_assert_cmpint(test_attach(helper, NULL, bus), ==, -1);
  g_assert_cmpint(test_attach(helper, wrong, NULL), ==, -1);
  g_assert_cmpint(test_attach(helper, wrong, bus), ==, 0);
  if (test_admitted_runtime()) {
    GstElement *di = gst_element_factory_make("deinterlace", NULL);
    test_wrong_version = TRUE;
    g_assert_cmpint(test_attach(helper, di, bus), ==, 0);
    test_wrong_version = FALSE;
    g_object_set(di, "method", 1, NULL);
    g_assert_cmpint(test_attach(helper, di, bus), ==, 0);
    g_object_set(di, "method", 4, NULL);
    g_assert_cmpint(test_attach(helper, di, bus), ==, 1);
    g_signal_emit_by_name(helper, "detach");
    g_signal_emit_by_name(helper, "detach");
    g_assert_false(test_boolean(helper, "attached"));
    g_assert_false(test_boolean(helper, "active"));
    g_assert_false(test_boolean(helper, "eligible"));
    g_assert_cmpint(test_attach(helper, di, bus), ==, -1);
    gst_object_unref(di);
  }
  gst_object_unref(factory);
  gst_object_unref(wrong);
  gst_object_unref(helper);
  gst_object_unref(bus);
}

static void
test_duplicate_and_forward(void)
{
  TestFixture f;
  GstElement *other;
  if (!test_admitted_runtime()) return;
  test_fixture_init(&f);
  other = test_helper_new();
  g_assert_cmpint(test_attach(f.helper, f.di, f.bus), ==, 1);
  g_assert_true(test_boolean(f.helper, "eligible"));
  g_assert_cmpint(test_attach(other, f.di, f.bus), ==, -1);
  g_assert_true(gst_pad_send_event(f.src, test_qos()));
  g_assert_cmpuint(f.events, ==, 1);
  g_assert_cmpuint(test_count(f.helper, "qos-count"), ==, 1);
  g_assert_cmpuint(test_count(f.helper, "forward-count"), ==, 1);
  g_assert_cmpuint(test_count(f.helper, "failure-count"), ==, 0);
  g_signal_emit_by_name(f.helper, "detach");
  g_assert_cmpint(test_attach(other, f.di, f.bus), ==, 1);
  g_assert_true(gst_pad_send_event(f.src, test_qos()));
  g_assert_cmpuint(f.events, ==, 2);
  g_signal_emit_by_name(other, "detach");
  gst_object_unref(other);
  test_fixture_clear(&f);
}

static void
test_mutable_properties(void)
{
  static const gchar *names[] = {"mode", "method", "fields", "locking", "tff"};
  if (!test_admitted_runtime()) return;
  for (guint n = 0; n < G_N_ELEMENTS(names); n++) {
    TestFixture f;
    gint original;
    test_fixture_init(&f);
    g_test_message("fresh property %s", names[n]);
    g_assert_cmpint(test_attach(f.helper, f.di, f.bus), ==, 1);
    g_object_get(f.di, names[n], &original, NULL);
    g_object_freeze_notify(G_OBJECT(f.di));
    /* mode/fields setters defer while output caps exist. Deactivate only the
     * standalone test pad so these changes become current, then restore caps. */
    gst_pad_set_active(f.src, FALSE);
    g_object_set(f.di, names[n], original + 1, NULL);
    gst_pad_set_active(f.src, TRUE);
    test_store_caps(f.src, OUTPUT_CAPS);
    g_assert_false(test_boolean(f.helper, "eligible"));
    g_assert_true(test_boolean(f.helper, "active"));
    gst_pad_set_active(f.src, FALSE);
    g_object_set(f.di, names[n], original, NULL);
    gst_pad_set_active(f.src, TRUE);
    test_store_caps(f.src, OUTPUT_CAPS);
    g_assert_true(test_boolean(f.helper, "eligible"));
    gst_pad_set_active(f.src, FALSE);
    g_object_set(f.di, names[n], original + 1, NULL);
    gst_pad_set_active(f.src, TRUE);
    test_store_caps(f.src, OUTPUT_CAPS);
    g_assert_true(gst_pad_send_event(f.src, test_qos()));
    g_assert_false(test_boolean(f.helper, "active"));
    gst_pad_set_active(f.src, FALSE);
    g_object_set(f.di, names[n], original, NULL);
    gst_pad_set_active(f.src, TRUE);
    test_store_caps(f.src, OUTPUT_CAPS);
    g_object_thaw_notify(G_OBJECT(f.di));
    g_assert_false(test_boolean(f.helper, "eligible"));
    g_assert_true(gst_pad_send_event(f.src, test_qos()));
    g_assert_cmpuint(f.events, ==, 2);
    g_assert_cmpuint(test_count(f.helper, "forward-count"), ==, 0);
    test_fixture_clear(&f);
  }
}

static void
test_caps_latch(void)
{
  static const gchar *bad[] = {
    "video/x-raw,format=UYVY,width=16,height=8,interlace-mode=mixed,framerate=25/1",
    "video/x-raw,format=YUY2,width=32,height=8,interlace-mode=mixed,framerate=25/1",
    "video/x-raw,format=YUY2,width=16,height=8,interlace-mode=mixed,framerate=30/1",
    "video/x-raw,format=YUY2,width=16,height=8,interlace-mode=progressive,framerate=25/1"
  };
  if (!test_admitted_runtime()) return;
  for (guint n = 0; n < G_N_ELEMENTS(bad); n++) {
    TestFixture f;
    test_fixture_init(&f);
    g_assert_cmpint(test_attach(f.helper, f.di, f.bus), ==, 1);
    g_assert_true(test_boolean(f.helper, "eligible"));
    test_store_caps(f.sink, bad[n]);
    g_assert_false(test_boolean(f.helper, "eligible"));
    g_assert_true(gst_pad_send_event(f.src, test_qos()));
    test_store_caps(f.sink, INPUT_CAPS);
    g_assert_false(test_boolean(f.helper, "active"));
    g_assert_false(test_boolean(f.helper, "eligible"));
    g_assert_cmpuint(f.events, ==, 1);
    g_assert_cmpuint(test_count(f.helper, "forward-count"), ==, 0);
    test_fixture_clear(&f);
  }
}

static void
test_forward_failure(void)
{
  TestFixture f;
  GstMessage *message;
  const GstStructure *s;
  if (!test_admitted_runtime()) return;
  test_fixture_init(&f);
  f.fail_forward = TRUE;
  g_assert_cmpint(test_attach(f.helper, f.di, f.bus), ==, 1);
  /* DROP consumes the original even when forwarding failed; never retry. */
  g_assert_true(gst_pad_send_event(f.src, test_qos()));
  g_assert_cmpuint(f.events, ==, 1);
  g_assert_cmpuint(test_count(f.helper, "failure-count"), ==, 1);
  g_assert_cmpuint(test_count(f.helper, "forward-count"), ==, 0);
  message = gst_bus_pop_filtered(f.bus, GST_MESSAGE_APPLICATION);
  g_assert_nonnull(message);
  g_assert_true(GST_MESSAGE_SRC(message) == GST_OBJECT(f.di));
  s = gst_message_get_structure(message);
  g_assert_true(gst_structure_has_name(s, "crystalhd-player-output-error"));
  g_assert_cmpstr(gst_structure_get_string(s, "message"), ==,
      "could not forward deinterlacer QoS feedback upstream");
  gst_message_unref(message);
  g_assert_null(gst_bus_pop_filtered(f.bus, GST_MESSAGE_APPLICATION));
  test_fixture_clear(&f);
}

static void
test_finalized(gpointer data, GObject *where)
{
  (void) where;
  *(gboolean *) data = TRUE;
}

static gpointer
test_event_thread(gpointer data)
{
  TestFixture *f = data;
  return GINT_TO_POINTER(gst_pad_send_event(f->src, test_qos()));
}

static void
test_inflight_dispose(void)
{
  TestFixture f;
  GstElement *other;
  GThread *thread;
  gboolean finalized = FALSE, di_finalized = FALSE, bus_finalized = FALSE;
  gint64 until;
  if (!test_admitted_runtime()) return;
  test_fixture_init(&f);
  f.block = TRUE;
  g_assert_cmpint(test_attach(f.helper, f.di, f.bus), ==, 1);
  g_object_weak_ref(G_OBJECT(f.helper), test_finalized, &finalized);
  g_object_weak_ref(G_OBJECT(f.di), test_finalized, &di_finalized);
  g_object_weak_ref(G_OBJECT(f.bus), test_finalized, &bus_finalized);
  thread = g_thread_new("di-qos-event", test_event_thread, &f);
  until = g_get_monotonic_time() + 2 * G_TIME_SPAN_SECOND;
  g_mutex_lock(&f.mutex);
  while (!f.entered)
    g_assert_true(g_cond_wait_until(&f.cond, &f.mutex, until));
  g_mutex_unlock(&f.mutex);
  g_signal_emit_by_name(f.helper, "detach");
  g_assert_false(test_boolean(f.helper, "attached"));
  gst_object_unref(f.helper);
  f.helper = NULL;
  g_assert_true(finalized); /* Probe/state must not retain the controller. */
  other = test_helper_new();
  g_assert_cmpint(test_attach(other, f.di, f.bus), ==, -1);
  g_mutex_lock(&f.mutex);
  f.release = TRUE;
  g_cond_broadcast(&f.cond);
  g_mutex_unlock(&f.mutex);
  g_assert_true(GPOINTER_TO_INT(g_thread_join(thread)));
  g_assert_cmpuint(f.events, ==, 1);
  g_assert_cmpint(test_attach(other, f.di, f.bus), ==, 1);
  g_signal_emit_by_name(other, "detach");
  gst_object_unref(other);
  test_fixture_clear(&f);
  g_assert_true(di_finalized);
  g_assert_true(bus_finalized);
}

static void
test_reentrant_detach(void)
{
  TestFixture f;
  if (!test_admitted_runtime()) return;
  test_fixture_init(&f);
  f.reentrant = TRUE;
  g_assert_cmpint(test_attach(f.helper, f.di, f.bus), ==, 1);
  g_assert_true(gst_pad_send_event(f.src, test_qos()));
  g_assert_false(test_boolean(f.helper, "attached"));
  g_assert_false(test_boolean(f.helper, "active"));
  g_assert_cmpuint(f.events, ==, 1);
  g_assert_cmpuint(test_count(f.helper, "forward-count"), ==, 1);
  test_fixture_clear(&f);
}

static void
test_reentrant_final_owners(void)
{
  if (!test_admitted_runtime()) return;
  if (g_test_subprocess()) {
    TestFixture f;
    gboolean helper_finalized = FALSE, di_finalized = FALSE;
    test_fixture_init(&f);
    f.release_owners = TRUE;
    g_object_weak_ref(G_OBJECT(f.helper), test_finalized, &helper_finalized);
    g_object_weak_ref(G_OBJECT(f.di), test_finalized, &di_finalized);
    g_assert_cmpint(test_attach(f.helper, f.di, f.bus), ==, 1);
    /* The sender retains the src pad, not its parent. The forwarding callback
     * drops every external helper/target reference before probe destruction. */
    g_assert_true(gst_pad_send_event(f.src, test_qos()));
    g_assert_true(helper_finalized);
    g_assert_true(di_finalized);
    test_fixture_clear(&f);
    return;
  }
  g_test_trap_subprocess(NULL, 2 * G_TIME_SPAN_SECOND, 0);
  g_test_trap_assert_passed();
}

static void
test_playing_bin_removal(void)
{
  TestFixture f;
  GstElement *pipeline;
  GstState state;
  gboolean finalized = FALSE;
  if (!test_admitted_runtime()) return;
  test_fixture_init(&f);
  pipeline = gst_pipeline_new("removal-test");
  g_assert_true(gst_bin_add(GST_BIN(pipeline), gst_object_ref(f.di)));
  g_assert_cmpint(gst_element_set_state(pipeline, GST_STATE_PLAYING), !=,
      GST_STATE_CHANGE_FAILURE);
  g_assert_cmpint(gst_element_get_state(pipeline, &state, NULL, GST_SECOND), !=,
      GST_STATE_CHANGE_FAILURE);
  g_assert_cmpint(state, ==, GST_STATE_PLAYING);
  test_store_caps(f.sink, INPUT_CAPS);
  test_store_caps(f.src, OUTPUT_CAPS);
  g_assert_cmpint(test_attach(f.helper, f.di, f.bus), ==, 1);
  g_assert_true(gst_pad_send_event(f.src, test_qos()));
  g_assert_true(gst_bin_remove(GST_BIN(pipeline), f.di));
  g_assert_null(GST_OBJECT_PARENT(f.di));
  g_assert_cmpint(GST_STATE(f.di), ==, GST_STATE_PLAYING);
  g_object_weak_ref(G_OBJECT(f.helper), test_finalized, &finalized);
  gst_object_unref(f.helper);
  f.helper = NULL;
  g_assert_true(finalized);
  /* Normal owner cleanup still puts the removed element into NULL. */
  gst_element_set_state(f.di, GST_STATE_NULL);
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(pipeline);
  test_fixture_clear(&f);
}

typedef struct { GstElement *di; guint injected; } TestPixelProbe;

static GstPadProbeReturn
test_pixel_qos(GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
  TestPixelProbe *probe = data;
  GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
  (void) pad;
  if (GST_BUFFER_PTS(buffer) == 40 * GST_MSECOND) {
    GstPad *src = gst_element_get_static_pad(probe->di, "src");
    GstEvent *event = gst_event_new_qos(GST_QOS_TYPE_UNDERFLOW, 1.0,
        GST_MSECOND, 20 * GST_MSECOND);
    g_assert_true(gst_pad_send_event(src, event));
    probe->injected++;
    gst_object_unref(src);
  }
  return GST_PAD_PROBE_OK;
}

static GPtrArray *
test_pixels_run(gboolean helper_enabled, gboolean inject, gboolean top_first)
{
  GstHarness *h = gst_harness_new("deinterlace");
  GstElement *helper = test_helper_new();
  GstBus *bus = gst_bus_new();
  GPtrArray *outputs = g_ptr_array_new_with_free_func((GDestroyNotify) gst_buffer_unref);
  TestPixelProbe probe = {h->element, 0};
  GstPad *src = gst_element_get_static_pad(h->element, "src");
  gulong id = 0;
  gst_harness_set_caps_str(h, INPUT_CAPS, OUTPUT_CAPS);
  if (helper_enabled)
    g_assert_cmpint(test_attach(helper, h->element, bus), ==, 1);
  if (inject)
    id = gst_pad_add_probe(src, GST_PAD_PROBE_TYPE_BUFFER, test_pixel_qos, &probe, NULL);
  for (guint n = 0; n < 3; n++) {
    GstBuffer *buffer = gst_buffer_new_allocate(NULL, 256, NULL);
    GstMapInfo map;
    g_assert_true(gst_buffer_map(buffer, &map, GST_MAP_WRITE));
    for (guint row = 0; row < 8; row++)
      for (guint col = 0; col < 32; col++)
        map.data[row * 32 + col] = (guint8) (n * 37 + row * 19 + col * 3);
    gst_buffer_unmap(buffer, &map);
    GST_BUFFER_PTS(buffer) = n * 40 * GST_MSECOND;
    GST_BUFFER_DURATION(buffer) = 40 * GST_MSECOND;
    GST_BUFFER_FLAG_SET(buffer, GST_VIDEO_BUFFER_FLAG_INTERLACED);
    if (top_first) GST_BUFFER_FLAG_SET(buffer, GST_VIDEO_BUFFER_FLAG_TFF);
    g_assert_cmpint(gst_harness_push(h, buffer), ==, GST_FLOW_OK);
  }
  g_assert_true(gst_harness_push_event(h, gst_event_new_eos()));
  while (gst_harness_buffers_in_queue(h))
    g_ptr_array_add(outputs, gst_harness_pull(h));
  if (inject) g_assert_cmpuint(probe.injected, ==, 1);
  if (id) gst_pad_remove_probe(src, id);
  if (helper_enabled) {
    g_assert_true(test_boolean(helper, "eligible"));
    g_assert_cmpuint(test_count(helper, "failure-count"), ==, 0);
  }
  /* Removal while the harness is still PLAYING must not require NULL. */
  g_signal_emit_by_name(helper, "detach");
  gst_object_unref(helper);
  gst_object_unref(src);
  gst_harness_teardown(h);
  gst_object_unref(bus);
  return outputs;
}

static void
test_exact_fields(void)
{
  if (!test_admitted_runtime()) return;
  for (guint tff = 0; tff < 2; tff++) {
    GPtrArray *reference = test_pixels_run(FALSE, FALSE, tff);
    GPtrArray *stock = test_pixels_run(FALSE, TRUE, tff);
    GPtrArray *actual = test_pixels_run(TRUE, TRUE, tff);
    g_assert_cmpuint(reference->len, ==, 6);
    g_assert_cmpuint(stock->len, ==, 5); /* Negative control: stock loses timely sibling. */
    g_assert_cmpuint(actual->len, ==, reference->len);
    for (guint n = 0; n < actual->len; n++) {
      GstBuffer *a = g_ptr_array_index(reference, n), *b = g_ptr_array_index(actual, n);
      GstMapInfo ma, mb;
      g_assert_cmpuint(GST_BUFFER_PTS(a), ==, n * 20 * GST_MSECOND);
      g_assert_cmpuint(GST_BUFFER_PTS(b), ==, GST_BUFFER_PTS(a));
      g_assert_cmpuint(GST_BUFFER_DURATION(b), ==, GST_BUFFER_DURATION(a));
      g_assert_cmpuint(GST_BUFFER_FLAGS(b), ==, GST_BUFFER_FLAGS(a));
      g_assert_true(gst_buffer_map(a, &ma, GST_MAP_READ));
      g_assert_true(gst_buffer_map(b, &mb, GST_MAP_READ));
      g_assert_cmpmem(ma.data, ma.size, mb.data, mb.size);
      gst_buffer_unmap(a, &ma);
      gst_buffer_unmap(b, &mb);
    }
    g_ptr_array_unref(reference);
    g_ptr_array_unref(stock);
    g_ptr_array_unref(actual);
  }
}

int
main(int argc, char **argv)
{
  gst_init(&argc, &argv);
  g_test_init(&argc, &argv, NULL);
  g_assert_true(gst_element_register(NULL, "crystalhddiqos", GST_RANK_NONE,
      GST_TYPE_CRYSTALHD_DI_QOS));
  g_test_add_func("/diqos/admission", test_admission);
  g_test_add_func("/diqos/duplicate-forward", test_duplicate_and_forward);
  g_test_add_func("/diqos/mutable-frozen-properties", test_mutable_properties);
  g_test_add_func("/diqos/caps-latch", test_caps_latch);
  g_test_add_func("/diqos/forward-failure", test_forward_failure);
  g_test_add_func("/diqos/inflight-dispose", test_inflight_dispose);
  g_test_add_func("/diqos/reentrant-detach", test_reentrant_detach);
  g_test_add_func("/diqos/reentrant-final-owners", test_reentrant_final_owners);
  g_test_add_func("/diqos/playing-bin-removal", test_playing_bin_removal);
  g_test_add_func("/diqos/exact-tff-bff-fields", test_exact_fields);
  return g_test_run();
}
