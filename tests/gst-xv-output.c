/* SPDX-License-Identifier: MIT */
/* Actual element/transport code; syscall faults affect only this test process. */
#define _GNU_SOURCE
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/base/gstbaseparse.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <xcb/xcb.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static gint fail_alloc_after = -1, fail_attach, defer_detach;
static gpointer drop_connection;
static gpointer fail_flush_connection;
static gpointer delay_connection, delayed_event;
static gint release_delayed;
static gint drop_type;
static const char *self_path;
static int test_flush(xcb_connection_t *connection)
{
  if (g_atomic_pointer_compare_and_exchange(&fail_flush_connection, connection, NULL)) return 0;
  return xcb_flush(connection);
}
static int test_shmget(key_t key, size_t size, int flags)
{
  gint budget = g_atomic_int_get(&fail_alloc_after);
  if (!budget) { errno = ENOMEM; return -1; }
  if (budget > 0) g_atomic_int_add(&fail_alloc_after, -1);
  return shmget(key, size, flags);
}
static void *test_shmat(int id, const void *address, int flags)
{
  if (g_atomic_int_get(&fail_attach)) { errno = ENOMEM; return (void *)-1; }
  return shmat(id, address, flags);
}
static int test_shmctl(int id, int command, struct shmid_ds *status)
{
  int result = shmctl(id, command, status);
  if (!result && command == IPC_STAT && g_atomic_int_get(&defer_detach))
    status->shm_nattch = MAX(status->shm_nattch, 2);
  return result;
}
static xcb_generic_event_t *test_poll_event(xcb_connection_t *connection)
{
  xcb_generic_event_t *event;
  if (g_atomic_pointer_get(&delay_connection) == connection && g_atomic_int_get(&release_delayed)) {
    event = g_atomic_pointer_get(&delayed_event);
    if (event && g_atomic_pointer_compare_and_exchange(&delayed_event, event, NULL)) return event;
  }
  while ((event = xcb_poll_for_event(connection))) {
    if (g_atomic_pointer_get(&delay_connection) == connection &&
        (event->response_type & 0x7f) == g_atomic_int_get(&drop_type) &&
        !g_atomic_int_get(&release_delayed) &&
        g_atomic_pointer_compare_and_exchange(&delayed_event, NULL, event)) return NULL;
    if (g_atomic_pointer_get(&drop_connection) != connection ||
        (event->response_type & 0x7f) != g_atomic_int_get(&drop_type)) return event;
    free(event);
  }
  return NULL;
}
#define shmget test_shmget
#define shmat test_shmat
#define shmctl test_shmctl
#define xcb_poll_for_event test_poll_event
#define xcb_flush test_flush
#include "../filters/gst/xv-output/gstcrystalhdxv.c"
#undef shmget
#undef shmat
#undef shmctl
#undef xcb_poll_for_event
#undef xcb_flush

static guint own_segments(void)
{
  FILE *file = fopen("/proc/sysvipc/shm", "r"); g_assert_nonnull(file);
  char line[2048]; guint count = 0;
  g_assert_nonnull(fgets(line, sizeof(line), file));
  while (fgets(line, sizeof(line), file)) {
    unsigned long key, id, mode, size, creator;
    if (sscanf(line, "%lu %lu %lo %lu %lu", &key, &id, &mode, &size, &creator) == 5 &&
        creator == (unsigned long)getpid()) count++;
  }
  fclose(file); return count;
}
static void clean_contexts(void)
{
  gint64 end = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;
  while (g_atomic_int_get(&live_contexts) && g_get_monotonic_time() < end) g_usleep(1000);
  g_assert_cmpint(g_atomic_int_get(&live_contexts), ==, 0);
  g_assert_cmpuint(own_segments(), ==, 0);
}
static gboolean x11_enabled(void)
{
  if (g_strcmp0(g_getenv("CRYSTALHD_TEST_X11"), "1")) {
    g_test_skip("Set CRYSTALHD_TEST_X11=1 with an available local Xv display"); return FALSE;
  }
  return TRUE;
}
static GstElement *new_pipeline(guint count, gboolean live)
{
  gchar *description = g_strdup_printf("videotestsrc num-buffers=%u is-live=%s pattern=black ! "
      "video/x-raw,format=UYVY,width=320,height=240,framerate=50/1,colorimetry=bt601 ! "
      "crystalhdxvsink name=output sync=true qos=true show-preroll-frame=false "
      "max-lateness=5000000 processing-deadline=15000000",
      count, live ? "true" : "false");
  GError *error = NULL;
  GstElement *pipeline = gst_parse_launch(description, &error); g_free(description);
  g_assert_no_error(error); g_assert_nonnull(pipeline); return pipeline;
}
static GstStructure *sink_stats(GstElement *sink)
{
  GstStructure *stats = NULL; g_object_get(sink, "xv-stats", &stats, NULL);
  g_assert_nonnull(stats); return stats;
}
static guint64 stat64(GstStructure *stats, const char *key)
{
  guint64 value; g_assert_true(gst_structure_get_uint64(stats, key, &value)); return value;
}
static guint stat32(GstStructure *stats, const char *key)
{
  guint value; g_assert_true(gst_structure_get_uint(stats, key, &value)); return value;
}
typedef struct { gint buffers; GstClockTime pts[512]; } NativeStage;
static GstPadProbeReturn count_native_stage(GstPad *pad, GstPadProbeInfo *probe, gpointer data)
{
  (void)pad; NativeStage *stage = data;
  gint index = g_atomic_int_add(&stage->buffers, 1);
  if (index < 512) stage->pts[index] = GST_BUFFER_PTS(GST_PAD_PROBE_INFO_BUFFER(probe));
  return GST_PAD_PROBE_OK;
}
static void await_message(GstElement *pipeline, GstMessageType expected)
{
  GstBus *bus = gst_element_get_bus(pipeline);
  GstMessage *message = gst_bus_timed_pop_filtered(bus, 8 * GST_SECOND, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);
  g_assert_nonnull(message);
  if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR && expected != GST_MESSAGE_ERROR) {
    GError *error; gchar *debug; gst_message_parse_error(message, &error, &debug);
    g_test_message("Unexpected output error: %s (%s)", error->message, debug ? debug : "");
    g_error_free(error); g_free(debug);
  }
  g_assert_cmpuint(GST_MESSAGE_TYPE(message), ==, expected);
  gst_message_unref(message); gst_object_unref(bus);
}
static void check_eos(GstElement *pipeline, guint count)
{
  g_assert_cmpint(gst_element_set_state(pipeline, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
  await_message(pipeline, GST_MESSAGE_EOS);
  GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "output");
  GstStructure *stats = sink_stats(sink);
  gchar *description = gst_structure_to_string(stats); g_test_message("%s", description); g_free(description);
  if (g_object_class_find_property(G_OBJECT_GET_CLASS(sink), "stats")) {
    GstStructure *base = NULL; g_object_get(sink, "stats", &base, NULL);
    if (base) { description = gst_structure_to_string(base); g_test_message("BaseSink: %s", description);
      g_free(description); gst_structure_free(base); }
  }
  NativeStage *stages = g_object_get_data(G_OBJECT(pipeline), "xv-native-stages");
  if (stages) {
    g_test_message("Native stages: parsed=%d decoded=%d post-process=%d sink-input=%d",
        g_atomic_int_get(&stages[0].buffers), g_atomic_int_get(&stages[1].buffers),
        g_atomic_int_get(&stages[2].buffers), g_atomic_int_get(&stages[3].buffers));
    for (guint s = 1; s < 4; s++) {
      GString *times = g_string_new(NULL);
      for (gint i = 0; i < MIN(stages[s].buffers, 10); i++)
        g_string_append_printf(times, " %" GST_TIME_FORMAT, GST_TIME_ARGS(stages[s].pts[i]));
      g_test_message("Native stage %u first PTS:%s", s, times->str); g_string_free(times, TRUE);
    }
    for (gint i = 0; i < MIN(stages[2].buffers, 512); i++) {
      gboolean found = FALSE;
      for (gint j = 0; j < MIN(stages[3].buffers, 512); j++)
        if (stages[2].pts[i] == stages[3].pts[j]) { found = TRUE; break; }
      if (!found) g_test_message("DI output absent at sink: index=%d PTS=%" GST_TIME_FORMAT,
          i, GST_TIME_ARGS(stages[2].pts[i]));
    }
    GstElement *controller = g_object_get_data(G_OBJECT(pipeline), "xv-native-controller");
    if (controller) {
      gboolean active, eligible; guint64 qos, forwarded;
      g_object_get(controller, "active", &active, "eligible", &eligible,
          "qos-count", &qos, "forward-count", &forwarded, NULL);
      g_test_message("DI controller: active=%d eligible=%d qos=%" G_GUINT64_FORMAT " forwarded=%" G_GUINT64_FORMAT,
          active, eligible, qos, forwarded);
    }
  }
  /* Preserve the failed census, but close owned decoder/X11 resources before
   * an assertion terminates this isolated test process. */
  g_assert_cmpint(gst_element_set_state(pipeline, GST_STATE_NULL), !=, GST_STATE_CHANGE_FAILURE);
  gst_element_get_state(pipeline, NULL, NULL, 3 * GST_SECOND);
  clean_contexts();
  g_assert_cmpuint(stat64(stats, "video-submitted"), ==, count);
  g_assert_cmpuint(stat64(stats, "submitted"), ==, count + stat64(stats, "redraw-submitted"));
  g_assert_cmpuint(stat64(stats, "acknowledged"), ==, stat64(stats, "submitted"));
  g_assert_cmpuint(stat64(stats, "terminal-retired"), ==, 0);
  g_assert_cmpuint(stat32(stats, "pending"), ==, 0);
  g_assert_cmpuint(stat64(stats, "pool-submitted"), ==, stat64(stats, "submitted"));
  g_assert_cmpuint(stat64(stats, "copied-bytes"), ==, 0);
  gst_structure_free(stats); gst_object_unref(sink);
}
static GstElement *direct_sink(void)
{
  GstElement *sink = g_object_new(gst_crystalhd_xv_get_type(), NULL);
  g_assert_true(sink_start(GST_BASE_SINK(sink))); return sink;
}
static GstVideoInfo info_for(guint width, guint height)
{
  GstVideoInfo info; gst_video_info_init(&info);
  g_assert_true(gst_video_info_set_format(&info, GST_VIDEO_FORMAT_UYVY, width, height));
  gst_video_colorimetry_from_string(&info.colorimetry, "bt601");
  info.fps_n = 50; info.fps_d = 1; return info;
}
static GstBuffer *pattern_buffer(const GstVideoInfo *info, guint value)
{
  guint pitch = info->width * 2 + 32, offset = 16;
  gsize size = (gsize)pitch * info->height + offset;
  guint8 *data = g_malloc(size); memset(data, 0xa5, size);
  for (gint y = 0; y < info->height; y++)
    for (gint x = 0; x < info->width * 2; x++) data[offset + y * pitch + x] = (value + y * 3 + x) % 251;
  GstBuffer *buffer = gst_buffer_new_wrapped(data, size);
  gsize offsets[GST_VIDEO_MAX_PLANES] = {offset}; gint strides[GST_VIDEO_MAX_PLANES] = {(gint)pitch};
  g_assert_nonnull(gst_buffer_add_video_meta_full(buffer, GST_VIDEO_FRAME_FLAG_NONE,
      GST_VIDEO_FORMAT_UYVY, info->width, info->height, 1, offsets, strides));
  GST_BUFFER_PTS(buffer) = value * 20 * GST_MSECOND;
  GST_BUFFER_DURATION(buffer) = 20 * GST_MSECOND;
  return buffer;
}
static void test_identity(void)
{
  XvContext c = {.window = 11, .opcode = 42, .completion_type = 65};
  XvMemory m = {.server_owned = TRUE, .segment = 17, .generation = 7};
  XvFlight f = {.memory = &m, .sequence = 65536, .generation = 7};
  xcb_shm_completion_event_t event = {.response_type = 65, .drawable = 11, .shmseg = 17,
      .major_event = 42, .minor_event = XCB_XV_SHM_PUT_IMAGE};
  g_assert_true(matches_locked(&c, &f, &event, 65536));
  g_assert_false(matches_locked(&c, &f, &event, 0));
#define WRONG(field, value) do { xcb_shm_completion_event_t copy = event; copy.field = value; \
    g_assert_false(matches_locked(&c, &f, &copy, 65536)); } while (0)
  WRONG(response_type, 65 | 0x80); WRONG(drawable, 12); WRONG(shmseg, 18);
  WRONG(offset, 1); WRONG(major_event, 43); WRONG(minor_event, 0);
#undef WRONG
  m.generation++; g_assert_false(matches_locked(&c, &f, &event, 65536));
  m.generation--; m.server_owned = FALSE; g_assert_false(matches_locked(&c, &f, &event, 65536));
}
static void test_setup_failure(void)
{
  GError *error = NULL;
  g_assert_null(context_new("remote.invalid:0", 0, NULL, &error));
  g_assert_error(error, GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_OPEN_READ_WRITE); g_clear_error(&error);
  g_assert_null(context_new(":9876", 0, NULL, &error)); g_assert_nonnull(error); g_clear_error(&error);
  clean_contexts();
}
static void test_color_admission(void)
{
  const GstVideoColorRange ranges[] = {GST_VIDEO_COLOR_RANGE_0_255,
      GST_VIDEO_COLOR_RANGE_16_235, GST_VIDEO_COLOR_RANGE_16_235};
  const GstVideoColorMatrix matrices[] = {GST_VIDEO_COLOR_MATRIX_BT601,
      GST_VIDEO_COLOR_MATRIX_BT2020, GST_VIDEO_COLOR_MATRIX_BT709};
  for (guint i = 0; i < G_N_ELEMENTS(ranges); i++) {
    XvContext c = {.wake_fd = -1}; g_mutex_init(&c.lock); g_cond_init(&c.changed);
    GstVideoInfo info = info_for(320, 240);
    info.colorimetry.range = ranges[i]; info.colorimetry.matrix = matrices[i];
    g_mutex_lock(&c.lock);
    g_assert_false(colorspace_locked(&c, &info));
    g_assert_true(c.failed); g_assert_cmpuint(c.image_count, ==, 0);
    g_mutex_unlock(&c.lock); g_mutex_clear(&c.lock); g_cond_clear(&c.changed);
  }
}
static void test_unspecified_color_pixels(void)
{
  /* A sink must not turn an implicit HD matrix into a requested BT.601
   * conversion. Use its real template in an otherwise card/display-free path. */
  GError *error = NULL;
  GstElement *pipeline = gst_parse_launch("appsrc name=input format=time ! videoconvert ! "
      "capsfilter name=limits ! appsink name=result sync=false", &error);
  g_assert_no_error(error);
  GstElement *source = gst_bin_get_by_name(GST_BIN(pipeline), "input");
  GstElement *filter = gst_bin_get_by_name(GST_BIN(pipeline), "limits");
  GstElement *output = gst_bin_get_by_name(GST_BIN(pipeline), "result");
  GstCaps *limits = gst_static_pad_template_get_caps(&sink_template);
  g_object_set(filter, "caps", limits, NULL); gst_caps_unref(limits);
  GstCaps *input = gst_caps_from_string("video/x-raw,format=UYVY,width=1280,height=720,"
      "framerate=50/1,interlace-mode=progressive");
  gst_app_src_set_caps(GST_APP_SRC(source), input);
  GstVideoInfo info; g_assert_true(gst_video_info_from_caps(&info, input)); gst_caps_unref(input);
  GstBuffer *expected = pattern_buffer(&info, 41);
  gst_element_set_state(pipeline, GST_STATE_PLAYING);
  g_assert_cmpint(gst_app_src_push_buffer(GST_APP_SRC(source), gst_buffer_ref(expected)), ==, GST_FLOW_OK);
  gst_app_src_end_of_stream(GST_APP_SRC(source));
  GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(output), 3 * GST_SECOND);
  g_assert_nonnull(sample);
  GstVideoInfo result_info;
  g_assert_true(gst_video_info_from_caps(&result_info, gst_sample_get_caps(sample)));
  GstVideoFrame a, b;
  g_assert_true(gst_video_frame_map(&a, &info, expected, GST_MAP_READ));
  g_assert_true(gst_video_frame_map(&b, &result_info, gst_sample_get_buffer(sample), GST_MAP_READ));
  for (gint y = 0; y < info.height; y++)
    g_assert_cmpmem((guint8 *)GST_VIDEO_FRAME_PLANE_DATA(&a, 0) + y * GST_VIDEO_FRAME_PLANE_STRIDE(&a, 0),
        info.width * 2, (guint8 *)GST_VIDEO_FRAME_PLANE_DATA(&b, 0) + y * GST_VIDEO_FRAME_PLANE_STRIDE(&b, 0),
        info.width * 2);
  gst_video_frame_unmap(&a); gst_video_frame_unmap(&b); gst_buffer_unref(expected); gst_sample_unref(sample);
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(source); gst_object_unref(filter); gst_object_unref(output); gst_object_unref(pipeline);
}
static void test_reopen(void)
{
  if (!x11_enabled()) return;
  GstElement *pipeline = new_pipeline(24, FALSE);
  for (guint i = 0; i < 3; i++) check_eos(pipeline, 24);
  gst_object_unref(pipeline); clean_contexts();
}
static void test_preroll(void)
{
  if (!x11_enabled()) return;
  GstElement *pipeline = new_pipeline(12, FALSE);
  GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "output");
  g_object_set(sink, "show-preroll-frame", TRUE, NULL);
  gst_element_set_state(pipeline, GST_STATE_PLAYING); await_message(pipeline, GST_MESSAGE_EOS);
  GstStructure *stats = sink_stats(sink);
  /* One preroll submission plus twelve clocked frames; not thirteen unique frames. */
  g_assert_cmpuint(stat64(stats, "video-submitted"), ==, 13);
  g_assert_cmpuint(stat64(stats, "submitted"), ==, 13 + stat64(stats, "redraw-submitted"));
  g_assert_cmpuint(stat64(stats, "acknowledged"), ==, stat64(stats, "submitted"));
  g_assert_cmpuint(stat64(stats, "pool-submitted"), ==, stat64(stats, "submitted"));
  gst_structure_free(stats); gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(sink); gst_object_unref(pipeline); clean_contexts();
}
static void test_padded_copy(void)
{
  if (!x11_enabled()) return;
  GstElement *sink = direct_sink(); XvContext *c = sink_context(XV_SINK(sink));
  GstVideoInfo info = info_for(320, 241);
  for (guint index = 0; index < 16; index++) {
    GstBuffer *buffer = pattern_buffer(&info, index);
    g_assert_cmpint(submit_buffer(c, &info, buffer, NULL), ==, GST_FLOW_OK);
    g_mutex_lock(&c->lock); g_assert_true(drain_locked(c));
    XvMemory *m = NULL;
    for (guint i = 0; i < XV_DEPTH; i++)
      if (c->copies[i] && (!m || c->copies[i]->generation > m->generation)) m = c->copies[i];
    g_assert_nonnull(m);
    for (gint y = 0; y < info.height; y++)
      for (gint x = 0; x < info.width * 2; x++)
        g_assert_cmpuint(m->data[m->offset + y * m->pitch + x], ==, (index + y * 3 + x) % 251);
    g_mutex_unlock(&c->lock); gst_buffer_unref(buffer);
  }
  g_assert_true(sink_stop(GST_BASE_SINK(sink))); context_unref(c);
  gst_object_unref(sink); clean_contexts();
}
static void test_paused_expose(void)
{
  if (!x11_enabled()) return;
  GstElement *sink = direct_sink(); XvContext *c = sink_context(XV_SINK(sink));
  GstVideoInfo info = info_for(320, 240);
  GstCaps *caps = gst_video_info_to_caps(&info);
  g_assert_true(sink_set_caps(GST_BASE_SINK(sink), caps)); gst_caps_unref(caps);
  GstBuffer *buffer = pattern_buffer(&info, 7);
  GST_BUFFER_PTS(buffer) = 40 * GST_MSECOND;
  g_assert_cmpint(sink_show_frame(GST_VIDEO_SINK(sink), buffer), ==, GST_FLOW_OK);
  g_mutex_lock(&c->lock); g_assert_true(drain_locked(c));
  guint64 before = c->submitted; g_mutex_unlock(&c->lock);
  gst_video_overlay_expose(GST_VIDEO_OVERLAY(sink));
  gint64 end = g_get_monotonic_time() + G_USEC_PER_SEC;
  guint64 after;
  do {
    g_mutex_lock(&c->lock); after = c->submitted; g_mutex_unlock(&c->lock);
    if (after > before) break;
    g_usleep(1000);
  } while (g_get_monotonic_time() < end);
  g_test_message("Paused expose: requests before=%" G_GUINT64_FORMAT " after=%" G_GUINT64_FORMAT, before, after);
  gst_buffer_unref(buffer); sink_stop(GST_BASE_SINK(sink));
  context_unref(c); gst_object_unref(sink); clean_contexts();
  g_assert_cmpuint(after, >, before);
}
static void await_redraw(XvContext *c, guint64 before, guint width, guint height)
{
  gint64 end = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;
  g_mutex_lock(&c->lock);
  while (!c->failed && (c->redraw_submitted <= before || c->pending ||
      c->width != width || c->height != height))
    if (!g_cond_wait_until(&c->changed, &c->lock, end)) break;
  g_assert_false(c->failed); g_assert_cmpuint(c->redraw_submitted, >, before);
  g_assert_cmpuint(c->pending, ==, 0);
  g_assert_cmpuint(c->width, ==, width); g_assert_cmpuint(c->height, ==, height);
  g_assert_cmpuint(c->submitted, ==, c->acknowledged);
  g_mutex_unlock(&c->lock);
}
static void window_pixel_for_locked(XvContext *c, xcb_window_t window,
                                    guint x, guint y, gboolean white)
{
  xcb_get_image_reply_t *reply = reply_locked(c,
      xcb_get_image(c->connection, XCB_IMAGE_FORMAT_Z_PIXMAP, window,
          x, y, 1, 1, UINT32_MAX).sequence);
  g_assert_nonnull(reply); g_assert_cmpuint(reply->depth, ==, 24);
  g_assert_cmpint(xcb_get_image_data_length(reply), >=, 3);
  const guint8 *rgb = xcb_get_image_data(reply);
  for (guint i = 0; i < 3; i++) {
    if (white) g_assert_cmpuint(rgb[i], >=, 248);
    else g_assert_cmpuint(rgb[i], <=, 2);
  }
  free(reply);
}
static void window_pixel_locked(XvContext *c, guint x, guint y, gboolean white)
{
  window_pixel_for_locked(c, c->window, x, y, white);
}
static void paused_resize(gboolean pooled)
{
  if (!x11_enabled()) return;
  GError *error = NULL;
  GstElement *pipeline = gst_parse_launch(pooled ?
      "videotestsrc num-buffers=1 pattern=white ! "
      "video/x-raw,format=UYVY,width=320,height=240,framerate=50/1,colorimetry=bt601 ! "
      "crystalhdxvsink name=output enable-last-sample=false" :
      "appsrc name=input format=time ! crystalhdxvsink name=output enable-last-sample=false", &error);
  g_assert_no_error(error); GstVideoInfo info = info_for(320, 240);
  if (!pooled) {
    GstElement *source = gst_bin_get_by_name(GST_BIN(pipeline), "input");
    GstCaps *caps = gst_video_info_to_caps(&info); gst_app_src_set_caps(GST_APP_SRC(source), caps); gst_caps_unref(caps);
    GstBuffer *buffer = pattern_buffer(&info, 0); GstVideoFrame frame;
    g_assert_true(gst_video_frame_map(&frame, &info, buffer, GST_MAP_WRITE));
    for (gint y = 0; y < info.height; y++) {
      guint8 *row = (guint8 *)GST_VIDEO_FRAME_PLANE_DATA(&frame, 0) + y * GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
      for (gint x = 0; x < info.width * 2; x += 4) {row[x] = row[x+2] = 128; row[x+1] = row[x+3] = 235;}
    }
    gst_video_frame_unmap(&frame);
    g_assert_cmpint(gst_app_src_push_buffer(GST_APP_SRC(source), buffer), ==, GST_FLOW_OK);
    gst_app_src_end_of_stream(GST_APP_SRC(source)); gst_object_unref(source);
  }
  gst_element_set_state(pipeline, GST_STATE_PLAYING); await_message(pipeline, GST_MESSAGE_EOS);
  g_assert_cmpint(gst_element_set_state(pipeline, GST_STATE_PAUSED), !=, GST_STATE_CHANGE_FAILURE);
  GstState state;
  g_assert_cmpint(gst_element_get_state(pipeline, &state, NULL, 2*GST_SECOND), !=, GST_STATE_CHANGE_FAILURE);
  g_assert_cmpint(state, ==, GST_STATE_PAUSED);
  GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "output");
  g_assert_null(gst_base_sink_get_last_sample(GST_BASE_SINK(sink)));
  XvContext *c = sink_context(XV_SINK(sink)); g_assert_nonnull(c);
  g_mutex_lock(&c->lock); g_assert_true(drain_locked(c));
  g_assert_nonnull(c->last_buffer);
  g_assert_cmpint(buffer_memory(c->last_buffer) != NULL, ==, pooled);
  guint64 video = c->video_submitted, before = c->redraw_submitted;
  GstBuffer *identity = c->last_buffer; GstClockTime pts = GST_BUFFER_PTS(identity);
  guint width = c->width, height = c->height;
  window_pixel_locked(c, width/2, height/2, TRUE);
  xcb_rectangle_t all = {0, 0, width, height};
  xcb_poly_fill_rectangle(c->connection, c->window, c->gc, 1, &all);
  window_pixel_locked(c, width/2, height/2, FALSE);
  g_mutex_unlock(&c->lock);
  gst_video_overlay_expose(GST_VIDEO_OVERLAY(sink)); await_redraw(c, before, width, height);
  g_mutex_lock(&c->lock);
  window_pixel_locked(c, width/2, height/2, TRUE); window_pixel_locked(c, 0, 0, FALSE);
  before = c->redraw_submitted;
  guint32 dimensions[] = {640, 360};
  xcb_configure_window(c->connection, c->window, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, dimensions);
  xcb_flush(c->connection); g_mutex_unlock(&c->lock);
  await_redraw(c, before, 640, 360);
  g_mutex_lock(&c->lock);
  window_pixel_locked(c, 320, 180, TRUE); window_pixel_locked(c, 0, 0, FALSE);
  before = c->redraw_submitted;
  xcb_clear_area(c->connection, TRUE, c->window, 0, 0, 640, 360); xcb_flush(c->connection);
  g_mutex_unlock(&c->lock); await_redraw(c, before, 640, 360);
  g_mutex_lock(&c->lock);
  window_pixel_locked(c, 320, 180, TRUE); window_pixel_locked(c, 0, 0, FALSE);
  g_assert_true(c->last_buffer == identity); g_assert_cmpuint(GST_BUFFER_PTS(c->last_buffer), ==, pts);
  g_assert_cmpuint(c->video_submitted, ==, video);
  before = c->redraw_submitted; g_mutex_unlock(&c->lock);
  gst_element_send_event(pipeline, gst_event_new_flush_start());
  gst_element_send_event(pipeline, gst_event_new_flush_stop(TRUE));
  gst_video_overlay_expose(GST_VIDEO_OVERLAY(sink));
  g_usleep(50000);
  g_mutex_lock(&c->lock);
  g_assert_null(c->last_buffer); g_assert_cmpuint(c->redraw_submitted, ==, before);
  g_test_message("Actual PAUSED %s: video=%" G_GUINT64_FORMAT " redraw=%" G_GUINT64_FORMAT
      ", visible white/black pixels, resize/expose, source identity/PTS preserved, flush suppresses stale redraw",
      pooled ? "negotiated pool" : "copied input", video, before);
  g_mutex_unlock(&c->lock);
  gst_element_set_state(pipeline, GST_STATE_NULL); context_unref(c);
  gst_object_unref(sink); gst_object_unref(pipeline); clean_contexts();
}
static void test_paused_resize_pool(void) { paused_resize(TRUE); }
static void test_paused_resize_copy(void) { paused_resize(FALSE); }
static void test_partial_setup(void)
{
  if (!x11_enabled()) return;
  for (guint attach = 0; attach < 2; attach++) {
    GstElement *sink = direct_sink(); XvContext *c = sink_context(XV_SINK(sink));
    g_mutex_lock(&c->lock);
    c->copies[0] = allocate_locked(c, 320, 240); g_assert_nonnull(c->copies[0]);
    if (attach) g_atomic_int_set(&fail_attach, TRUE);
    else g_atomic_int_set(&fail_alloc_after, 1);
    c->copies[1] = allocate_locked(c, 320, 240);
    c->copies[2] = allocate_locked(c, 320, 240);
    g_assert_null(c->copies[2]);
    g_atomic_int_set(&fail_attach, FALSE); g_atomic_int_set(&fail_alloc_after, -1);
    g_mutex_unlock(&c->lock);
    g_assert_true(sink_stop(GST_BASE_SINK(sink))); context_unref(c); gst_object_unref(sink);
    clean_contexts();
  }
  GstElement *pipeline = new_pipeline(12, FALSE); check_eos(pipeline, 12); gst_object_unref(pipeline);
}
static void test_transport_reopen(void)
{
  if (!x11_enabled()) return;
  GstElement *pipeline = new_pipeline(24, TRUE);
  GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "output");
  g_assert_cmpint(gst_element_set_state(pipeline, GST_STATE_PAUSED), !=, GST_STATE_CHANGE_FAILURE);
  XvContext *c = sink_context(XV_SINK(sink)); g_assert_nonnull(c);
  g_atomic_pointer_set(&drop_connection, c->connection); g_atomic_int_set(&drop_type, c->completion_type);
  g_assert_cmpint(gst_element_set_state(pipeline, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
  await_message(pipeline, GST_MESSAGE_ERROR);
  gst_element_set_state(pipeline, GST_STATE_NULL); g_atomic_pointer_set(&drop_connection, NULL);
  g_mutex_lock(&c->lock);
  g_assert_cmpuint(c->acknowledged, ==, 0); g_assert_cmpuint(c->terminal, >, 0);
  g_assert_cmpuint(c->pending, ==, 0);
  g_mutex_unlock(&c->lock); context_unref(c); clean_contexts();
  check_eos(pipeline, 24);
  gst_element_set_state(pipeline, GST_STATE_PAUSED);
  c = sink_context(XV_SINK(sink)); g_assert_nonnull(c);
  g_mutex_lock(&c->lock);
  g_assert_cmpint(shutdown(xcb_get_file_descriptor(c->connection), SHUT_RDWR), ==, 0);
  g_mutex_unlock(&c->lock);
  gst_element_set_state(pipeline, GST_STATE_PLAYING); await_message(pipeline, GST_MESSAGE_ERROR);
  gst_element_set_state(pipeline, GST_STATE_NULL); context_unref(c); clean_contexts();
  check_eos(pipeline, 24);
  gst_object_unref(sink); gst_object_unref(pipeline); clean_contexts();
}
typedef struct { GstElement *pipeline; gint finished; } StopJob;
static GstBusSyncReply stop_on_output_error(GstBus *bus, GstMessage *message, gpointer data)
{
  (void)bus; StopJob *job = data;
  if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
    GError *error = NULL; gst_message_parse_error(message, &error, NULL);
    gboolean output_error = error->domain == GST_RESOURCE_ERROR && error->code == GST_RESOURCE_ERROR_WRITE;
    g_error_free(error);
    if (output_error) {
      gst_element_set_state(job->pipeline, GST_STATE_NULL);
      g_atomic_int_set(&job->finished, TRUE);
    }
  }
  return GST_BUS_PASS;
}
static void test_reentrant_error_stop(void)
{
  if (!x11_enabled()) return;
  GstElement *pipeline = new_pipeline(250, TRUE);
  GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "output");
  gst_element_set_state(pipeline, GST_STATE_PAUSED);
  XvContext *c = sink_context(XV_SINK(sink)); g_assert_nonnull(c);
  g_atomic_pointer_set(&drop_connection, c->connection); g_atomic_int_set(&drop_type, c->completion_type);
  g_atomic_int_set(&defer_detach, TRUE);
  GstBus *bus = gst_element_get_bus(pipeline); StopJob job = {.pipeline = pipeline};
  gst_bus_set_sync_handler(bus, stop_on_output_error, &job, NULL);
  gst_element_set_state(pipeline, GST_STATE_PLAYING);
  gint64 end = g_get_monotonic_time() + 4 * G_USEC_PER_SEC;
  while (!g_atomic_int_get(&job.finished) && g_get_monotonic_time() < end) g_usleep(1000);
  g_assert_true(g_atomic_int_get(&job.finished));
  g_atomic_pointer_set(&drop_connection, NULL); g_atomic_int_set(&defer_detach, FALSE); wake_context(c);
  gst_bus_set_sync_handler(bus, NULL, NULL, NULL);
  context_unref(c); gst_object_unref(bus); gst_object_unref(sink); gst_object_unref(pipeline);
  clean_contexts();
}
typedef struct { GstElement *sink; GstCaps *caps; gint started, finished; gboolean result; } CapsJob;
static gpointer change_caps(gpointer data)
{
  CapsJob *job = data; g_atomic_int_set(&job->started, TRUE);
  job->result = sink_set_caps(GST_BASE_SINK(job->sink), job->caps);
  g_atomic_int_set(&job->finished, TRUE); return NULL;
}
static GstBufferPool *make_pool(GstElement *sink, const GstVideoInfo *info)
{
  GstCaps *caps = gst_video_info_to_caps(info);
  GstQuery *query = gst_query_new_allocation(caps, TRUE);
  g_assert_true(sink_propose_allocation(GST_BASE_SINK(sink), query));
  g_assert_cmpuint(gst_query_get_n_allocation_pools(query), ==, 1);
  GstBufferPool *pool = NULL;
  gst_query_parse_nth_allocation_pool(query, 0, &pool, NULL, NULL, NULL);
  gst_query_unref(query); gst_caps_unref(caps);
  g_assert_nonnull(pool); g_assert_true(gst_buffer_pool_set_active(pool, TRUE)); return pool;
}
static void test_held_pool_and_caps(void)
{
  if (!x11_enabled()) return;
  GstElement *sink = direct_sink(); XvContext *c = sink_context(XV_SINK(sink));
  GstVideoInfo old = info_for(320, 240), next = info_for(640, 360);
  GstCaps *caps = gst_video_info_to_caps(&old);
  g_assert_true(sink_set_caps(GST_BASE_SINK(sink), caps)); gst_caps_unref(caps);
  GstBufferPool *pool = make_pool(sink, &old);
  GstBuffer *buffers[6];
  for (guint i = 0; i < 6; i++) {
    g_assert_cmpint(gst_buffer_pool_acquire_buffer(pool, &buffers[i], NULL), ==, GST_FLOW_OK);
    XvMemory *m = buffer_memory(buffers[i]); g_assert_nonnull(m); memset(m->data, 17 + i, m->size);
  }
  g_atomic_pointer_set(&delay_connection, c->connection); g_atomic_int_set(&drop_type, c->completion_type);
  g_atomic_int_set(&release_delayed, FALSE);
  g_assert_cmpint(submit_buffer(c, &old, buffers[0], buffer_memory(buffers[0])), ==, GST_FLOW_OK);
  gint64 end = g_get_monotonic_time() + G_USEC_PER_SEC;
  while (!g_atomic_pointer_get(&delayed_event) && g_get_monotonic_time() < end) g_usleep(1000);
  g_assert_nonnull(g_atomic_pointer_get(&delayed_event));
  gst_video_overlay_expose(GST_VIDEO_OVERLAY(sink));
  g_usleep(50000);
  g_mutex_lock(&c->lock);
  g_assert_true(c->last_buffer == buffers[0]); g_assert_true(buffer_memory(buffers[0])->server_owned);
  g_assert_cmpuint(c->submitted, ==, 1); g_assert_cmpuint(c->redraw_submitted, ==, 0);
  g_mutex_unlock(&c->lock);
  CapsJob job = {.sink = sink, .caps = gst_video_info_to_caps(&next)};
  GThread *thread = g_thread_new("xv-caps-test", change_caps, &job);
  while (!g_atomic_int_get(&job.started)) g_usleep(1000);
  g_usleep(50000); g_assert_false(g_atomic_int_get(&job.finished));
  g_mutex_lock(&c->lock);
  g_assert_cmpuint(c->pending, ==, 1); g_assert_cmpuint(c->epoch, ==, 1);
  g_mutex_unlock(&c->lock);
  g_atomic_int_set(&release_delayed, TRUE); wake_context(c);
  g_thread_join(thread); g_assert_true(job.result); gst_caps_unref(job.caps);
  g_atomic_pointer_set(&delay_connection, NULL); g_assert_null(g_atomic_pointer_get(&delayed_event));
  g_mutex_lock(&c->lock);
  g_assert_null(c->last_buffer); g_assert_cmpuint(c->redraw_submitted, ==, 0);
  g_mutex_unlock(&c->lock);
  GstBuffer *empty = NULL;
  GstBufferPoolAcquireParams params = {.flags = GST_BUFFER_POOL_ACQUIRE_FLAG_DONTWAIT};
  g_assert_cmpint(gst_buffer_pool_acquire_buffer(pool, &empty, &params), !=, GST_FLOW_OK);
  g_assert_null(empty); /* ACK alone cannot recycle any of six retained Gst owners. */
  XvMemory *released = buffer_memory(buffers[1]); gst_buffer_unref(buffers[1]); buffers[1] = NULL;
  g_assert_cmpint(gst_buffer_pool_acquire_buffer(pool, &empty, &params), ==, GST_FLOW_OK);
  g_assert_true(buffer_memory(empty) == released); gst_buffer_unref(empty);
  GstBufferPool *new_pool = make_pool(sink, &next); GstBuffer *new_buffer;
  g_assert_cmpint(gst_buffer_pool_acquire_buffer(new_pool, &new_buffer, NULL), ==, GST_FLOW_OK);
  XvMemory *m = buffer_memory(new_buffer), *held = buffer_memory(buffers[0]);
  g_assert_cmpuint(m->segment, !=, held->segment); memset(m->data, 55, m->size);
  g_assert_cmpint(sink_show_frame(GST_VIDEO_SINK(sink), new_buffer), ==, GST_FLOW_OK);
  g_mutex_lock(&c->lock); g_assert_true(drain_locked(c));
  for (guint i = 0; i < held->size; i++) g_assert_cmpuint(held->data[i], ==, 17);
  g_assert_cmpuint(c->epoch, ==, 2); g_mutex_unlock(&c->lock);
  gst_buffer_unref(new_buffer);
  for (guint i = 0; i < 6; i++) if (buffers[i]) gst_buffer_unref(buffers[i]);
  /* The renderer's retained last picture is a real Gst owner as well. */
  sink_stop(GST_BASE_SINK(sink));
  g_assert_true(gst_buffer_pool_set_active(pool, FALSE)); gst_object_unref(pool);
  g_assert_true(gst_buffer_pool_set_active(new_pool, FALSE)); gst_object_unref(new_pool);
  context_unref(c); gst_object_unref(sink); clean_contexts();
}
typedef struct { GstElement *sink; xcb_window_t window; gint finished; } WindowJob;
static gpointer change_window(gpointer data)
{
  WindowJob *job = data;
  gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(job->sink), job->window);
  g_atomic_int_set(&job->finished, TRUE); return NULL;
}
static void window_warning(GstBus *bus)
{
  GstMessage *message = gst_bus_timed_pop_filtered(bus, GST_SECOND,
      GST_MESSAGE_WARNING | GST_MESSAGE_ERROR);
  g_assert_nonnull(message); g_assert_cmpuint(GST_MESSAGE_TYPE(message), ==, GST_MESSAGE_WARNING);
  GError *error = NULL; gchar *debug = NULL;
  gst_message_parse_warning(message, &error, &debug);
  g_assert_error(error, GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_SETTINGS);
  g_error_free(error); g_free(debug); gst_message_unref(message);
}
static void solid_pool_buffer(GstBuffer *buffer, const GstVideoInfo *info,
                              gboolean white, guint64 id)
{
  GstVideoFrame frame;
  g_assert_true(gst_video_frame_map(&frame, info, buffer, GST_MAP_WRITE));
  for (gint y = 0; y < info->height; y++) {
    guint8 *row = (guint8 *)GST_VIDEO_FRAME_PLANE_DATA(&frame, 0) +
        y * GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
    for (gint x = 0; x < info->width * 2; x += 4) {
      row[x] = row[x + 2] = 128; row[x + 1] = row[x + 3] = white ? 235 : 16;
    }
  }
  gst_video_frame_unmap(&frame);
  GST_BUFFER_OFFSET(buffer) = id; GST_BUFFER_OFFSET_END(buffer) = id + 1;
  GST_BUFFER_PTS(buffer) = id * 20 * GST_MSECOND;
  GST_BUFFER_DURATION(buffer) = 20 * GST_MSECOND;
}
static void solid_memory(XvMemory *memory, const GstVideoInfo *info, gboolean white)
{
  for (gint y = 0; y < info->height; y++) {
    const guint8 *row = memory->data + memory->offset + y * memory->pitch;
    for (gint x = 0; x < info->width * 2; x += 4) {
      g_assert_cmpuint(row[x], ==, 128); g_assert_cmpuint(row[x + 2], ==, 128);
      g_assert_cmpuint(row[x + 1], ==, white ? 235 : 16);
      g_assert_cmpuint(row[x + 3], ==, white ? 235 : 16);
    }
  }
}
static void test_live_window_replacement(void)
{
  if (!x11_enabled()) return;
  /* This connection owns only our two windows: it does not grab an Xv port,
   * alter the display, or outlive this finite test. Reuse bounded XCB replies. */
  XvContext owner = {.wake_fd = -1};
  g_mutex_init(&owner.lock); g_cond_init(&owner.changed);
  int screen_number;
  owner.connection = xcb_connect(g_getenv("DISPLAY"), &screen_number);
  g_assert_nonnull(owner.connection); g_assert_cmpint(xcb_connection_has_error(owner.connection), ==, 0);
  xcb_screen_iterator_t screens = xcb_setup_roots_iterator(xcb_get_setup(owner.connection));
  for (int n = 0; n < screen_number && screens.rem; n++) xcb_screen_next(&screens);
  g_assert_cmpint(screens.rem, >, 0); xcb_screen_t *screen = screens.data;
  if (screen->width_in_pixels < 640 || screen->height_in_pixels < 240) {
    g_test_skip("The two-window pixel control needs a 640x240 local X11 screen");
    xcb_disconnect(owner.connection); g_mutex_clear(&owner.lock); g_cond_clear(&owner.changed); return;
  }
  owner.root = screen->root; owner.visual = screen->root_visual; owner.depth = screen->root_depth;
  xcb_window_t windows[2];
  for (guint i = 0; i < 2; i++) {
    windows[i] = xcb_generate_id(owner.connection);
    guint32 values[] = {screen->black_pixel, TRUE};
    xcb_create_window(owner.connection, XCB_COPY_FROM_PARENT, windows[i], screen->root,
        i * 320, 0, 320, 240, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
        XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT, values);
    xcb_map_window(owner.connection, windows[i]);
  }
  g_assert_true(barrier_locked(&owner));
  GstBus *bus = gst_bus_new();
  GstElement *sink = g_object_new(gst_crystalhd_xv_get_type(), NULL);
  gst_element_set_bus(sink, bus);
  gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(sink), windows[0]);
  g_assert_true(sink_start(GST_BASE_SINK(sink)));
  XvContext *c = sink_context(XV_SINK(sink)); g_assert_nonnull(c);
  GstVideoInfo info = info_for(320, 240); GstCaps *caps = gst_video_info_to_caps(&info);
  g_assert_true(sink_set_caps(GST_BASE_SINK(sink), caps)); gst_caps_unref(caps);
  GstBufferPool *pool = make_pool(sink, &info); GstBuffer *first, *second;
  g_assert_cmpint(gst_buffer_pool_acquire_buffer(pool, &first, NULL), ==, GST_FLOW_OK);
  solid_pool_buffer(first, &info, TRUE, 2);
  XvMemory *memory = buffer_memory(first); g_assert_nonnull(memory);
  g_mutex_lock(&c->lock);
  window_pixel_for_locked(c, windows[1], 160, 120, FALSE);
  guint64 epoch = c->epoch; xcb_gcontext_t gc = c->gc;
  g_mutex_unlock(&c->lock);
  g_atomic_pointer_set(&delay_connection, c->connection);
  g_atomic_int_set(&drop_type, c->completion_type); g_atomic_int_set(&release_delayed, FALSE);
  g_assert_cmpint(sink_show_frame(GST_VIDEO_SINK(sink), first), ==, GST_FLOW_OK);
  gint64 end = g_get_monotonic_time() + XV_TIMEOUT_US;
  while (!g_atomic_pointer_get(&delayed_event) && g_get_monotonic_time() < end) g_usleep(1000);
  g_assert_nonnull(g_atomic_pointer_get(&delayed_event));

  /* Bad targets are rejected before the held request or its source is disturbed. */
  gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(sink), 0); window_warning(bus);
  xcb_window_t missing = xcb_generate_id(owner.connection);
  gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(sink), missing); window_warning(bus);
  xcb_unmap_window(owner.connection, windows[1]); g_assert_true(barrier_locked(&owner));
  gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(sink), windows[1]); window_warning(bus);
  xcb_map_window(owner.connection, windows[1]); g_assert_true(barrier_locked(&owner));
  gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(sink), windows[0]);
  g_mutex_lock(&c->lock);
  XvFlight flight = {0};
  for (guint i = 0; i < XV_DEPTH; i++) if (c->flights[i].memory) flight = c->flights[i];
  g_assert_true(flight.memory == memory); g_assert_true(flight.buffer == first);
  xcb_generic_event_t *held = g_atomic_pointer_get(&delayed_event);
  g_assert_true(matches_locked(c, &flight, (const void *)held, held->full_sequence));
  g_assert_true(c->last_buffer == first); g_assert_true(memory->server_owned);
  g_assert_cmpuint(c->window, ==, windows[0]); g_assert_cmpuint(c->pending, ==, 1);
  g_assert_cmpuint(c->submitted, ==, 1); g_assert_cmpuint(c->acknowledged, ==, 0);
  g_assert_false(c->failed); g_mutex_unlock(&c->lock);

  WindowJob job = {.sink = sink, .window = windows[1]};
  GThread *thread = g_thread_new("xv-window-test", change_window, &job);
  end = g_get_monotonic_time() + XV_TIMEOUT_US / 2;
  g_mutex_lock(&c->lock);
  while (!c->window_changing && !c->failed && !g_atomic_int_get(&job.finished))
    if (!g_cond_wait_until(&c->changed, &c->lock, end)) break;
  g_assert_true(c->window_changing); g_assert_false(g_atomic_int_get(&job.finished));
  g_assert_cmpuint(c->window, ==, windows[0]); g_assert_cmpuint(c->pending, ==, 1);
  g_assert_cmpuint(c->epoch, ==, epoch); g_assert_cmpuint(memory->generation, ==, flight.generation);
  for (guint i = 0; i < XV_DEPTH; i++) if (c->flights[i].memory) {
    g_assert_true(c->flights[i].memory == flight.memory); g_assert_true(c->flights[i].buffer == first);
    g_assert_cmpuint(c->flights[i].sequence, ==, flight.sequence);
    g_assert_cmpuint(c->flights[i].generation, ==, flight.generation);
    g_assert_cmpuint(c->flights[i].epoch, ==, flight.epoch);
    g_assert_cmpuint(c->flights[i].pts, ==, 40 * GST_MSECOND);
  }
  solid_memory(memory, &info, TRUE); g_mutex_unlock(&c->lock);
  g_atomic_int_set(&release_delayed, TRUE); wake_context(c);
  g_thread_join(thread); g_assert_true(g_atomic_int_get(&job.finished));
  g_atomic_pointer_set(&delay_connection, NULL); g_assert_null(g_atomic_pointer_get(&delayed_event));
  await_redraw(c, 0, 320, 240);
  g_mutex_lock(&c->lock);
  g_assert_cmpuint(c->window, ==, windows[1]); g_assert_false(c->own_window);
  g_assert_cmpuint(c->gc, ==, gc); g_assert_cmpuint(c->epoch, ==, epoch);
  g_assert_true(c->last_buffer == first); g_assert_false(memory->server_owned);
  g_assert_cmpuint(memory->generation, >, flight.generation);
  g_assert_cmpuint(GST_BUFFER_OFFSET(first), ==, 2); g_assert_cmpuint(GST_BUFFER_OFFSET_END(first), ==, 3);
  g_assert_cmpuint(GST_BUFFER_PTS(first), ==, 40 * GST_MSECOND);
  g_assert_cmpuint(GST_BUFFER_DURATION(first), ==, 20 * GST_MSECOND);
  solid_memory(memory, &info, TRUE);
  window_pixel_locked(c, 160, 120, TRUE);
  guint64 first_generation = memory->generation; g_mutex_unlock(&c->lock);
  /* A server may clear the previous Xv drawable during handoff. Establish
   * an application-owned black old window before the next video request. */
  xcb_clear_area(owner.connection, FALSE, windows[0], 0, 0, 320, 240);
  g_assert_true(barrier_locked(&owner));
  g_mutex_lock(&c->lock); window_pixel_for_locked(c, windows[0], 160, 120, FALSE);
  g_mutex_unlock(&c->lock);

  g_assert_cmpint(gst_buffer_pool_acquire_buffer(pool, &second, NULL), ==, GST_FLOW_OK);
  solid_pool_buffer(second, &info, FALSE, 4);
  XvMemory *next = buffer_memory(second); g_assert_nonnull(next); g_assert_true(next != memory);
  g_assert_cmpint(sink_show_frame(GST_VIDEO_SINK(sink), second), ==, GST_FLOW_OK);
  g_mutex_lock(&c->lock); g_assert_true(drain_locked(c));
  g_assert_cmpuint(memory->generation, ==, first_generation);
  g_assert_cmpuint(next->generation, >, first_generation);
  g_assert_true(c->last_buffer == second); g_assert_cmpuint(GST_BUFFER_PTS(second), ==, 80 * GST_MSECOND);
  solid_memory(memory, &info, TRUE); solid_memory(next, &info, FALSE);
  window_pixel_for_locked(c, windows[0], 160, 120, FALSE);
  window_pixel_locked(c, 160, 120, FALSE);
  g_assert_cmpuint(c->video_submitted, ==, 2); g_assert_cmpuint(c->redraw_submitted, >, 0);
  g_assert_cmpuint(c->submitted, ==, c->acknowledged);
  g_assert_cmpuint(c->submitted, ==, c->pool_submitted); g_assert_cmpuint(c->copied_bytes, ==, 0);
  g_assert_cmpuint(c->terminal, ==, 0); g_assert_cmpuint(c->stale, ==, 0); g_mutex_unlock(&c->lock);

  g_assert_true(sink_stop(GST_BASE_SINK(sink)));
  gst_buffer_unref(first); gst_buffer_unref(second);
  g_assert_true(gst_buffer_pool_set_active(pool, FALSE)); gst_object_unref(pool);
  g_mutex_lock(&c->lock);
  g_assert_cmpuint(c->image_count, ==, 0); g_assert_cmpuint(c->image_bytes, ==, 0);
  g_assert_cmpuint(c->allocated, ==, c->freed); g_assert_cmpuint(c->pending, ==, 0);
  g_assert_cmpuint(c->submitted, ==, c->acknowledged);
  g_assert_cmpuint(c->terminal, ==, 0); g_assert_cmpuint(c->stale, ==, 0);
  g_assert_true(c->closed); g_assert_true(c->detached); g_assert_null(c->connection);
  g_assert_null(c->last_buffer); g_assert_false(c->failed); g_assert_false(c->quarantined);
  g_mutex_unlock(&c->lock); context_unref(c); gst_object_unref(sink); gst_object_unref(bus);
  guint width, height;
  for (guint i = 0; i < 2; i++) {
    g_assert_true(target_geometry_locked(&owner, windows[i], &width, &height));
    xcb_destroy_window(owner.connection, windows[i]);
  }
  g_assert_true(barrier_locked(&owner)); xcb_disconnect(owner.connection);
  g_mutex_clear(&owner.lock); g_cond_clear(&owner.changed); clean_contexts();
  g_test_message("Live two-window replacement: held old SHM ACK, source IDs/pixels/PTS and generations preserved; "
      "new window repainted, old target unchanged, invalid targets rejected, pool/GC retained, resources balanced");
}
static void test_submit_flush_failure(void)
{
  if (!x11_enabled()) return;
  GstElement *sink = direct_sink(); XvContext *c = sink_context(XV_SINK(sink));
  GstVideoInfo info = info_for(320, 240); GstCaps *caps = gst_video_info_to_caps(&info);
  g_assert_true(sink_set_caps(GST_BASE_SINK(sink), caps)); gst_caps_unref(caps);
  GstBufferPool *pool = make_pool(sink, &info); GstBuffer *buffer;
  g_assert_cmpint(gst_buffer_pool_acquire_buffer(pool, &buffer, NULL), ==, GST_FLOW_OK);
  XvMemory *m = buffer_memory(buffer); g_assert_nonnull(m); memset(m->data, 128, m->size);
  g_atomic_pointer_set(&fail_flush_connection, c->connection);
  g_assert_cmpint(sink_show_frame(GST_VIDEO_SINK(sink), buffer), ==, GST_FLOW_ERROR);
  g_assert_null(g_atomic_pointer_get(&fail_flush_connection));
  gst_buffer_unref(buffer); sink_stop(GST_BASE_SINK(sink));
  g_mutex_lock(&c->lock);
  g_assert_cmpuint(c->submitted, ==, 1); g_assert_cmpuint(c->video_submitted, ==, 1);
  g_assert_cmpuint(c->redraw_submitted, ==, 0); g_assert_cmpuint(c->terminal, ==, 1);
  g_assert_cmpuint(c->acknowledged, ==, 0); g_assert_cmpuint(c->pending, ==, 0);
  g_assert_null(c->last_buffer); g_mutex_unlock(&c->lock);
  g_assert_true(gst_buffer_pool_set_active(pool, FALSE)); gst_object_unref(pool);
  context_unref(c); gst_object_unref(sink); clean_contexts();
}
static void test_two_instances(void)
{
  if (!x11_enabled()) return;
  GstElement *a = new_pipeline(24, TRUE), *b = new_pipeline(24, TRUE);
  gst_element_set_state(a, GST_STATE_PAUSED); gst_element_set_state(b, GST_STATE_PAUSED);
  GstElement *as = gst_bin_get_by_name(GST_BIN(a), "output"), *bs = gst_bin_get_by_name(GST_BIN(b), "output");
  XvContext *ac = sink_context(XV_SINK(as)), *bc = sink_context(XV_SINK(bs));
  g_assert_nonnull(ac); g_assert_nonnull(bc);
  g_assert_true(ac->connection != bc->connection); g_assert_cmpuint(ac->port, !=, bc->port);
  gst_element_set_state(a, GST_STATE_PLAYING); gst_element_set_state(b, GST_STATE_PLAYING);
  await_message(a, GST_MESSAGE_EOS); await_message(b, GST_MESSAGE_EOS);
  gst_element_set_state(a, GST_STATE_NULL); gst_element_set_state(b, GST_STATE_NULL);
  g_assert_cmpuint(ac->submitted, ==, ac->acknowledged);
  g_assert_cmpuint(bc->submitted, ==, bc->acknowledged);
  context_unref(ac); context_unref(bc); gst_object_unref(as); gst_object_unref(bs);
  gst_object_unref(a); gst_object_unref(b); clean_contexts();
}
/* Report failures only after retiring the pipeline and our private grabs. */
static gboolean contention_check(gboolean ok, gchar **failure, const char *message)
{
  if (!ok && !*failure) *failure = g_strdup(message);
  return ok;
}
static gint contention_fds(void)
{
  DIR *directory = opendir("/proc/self/fd");
  if (!directory) return -1;
  struct dirent *entry; gint count = 0;
  while ((entry = readdir(directory)))
    if (g_ascii_isdigit(entry->d_name[0]) && atoi(entry->d_name) != dirfd(directory)) count++;
  closedir(directory); return count;
}
static gboolean contention_ungrab(XvContext *holder, xcb_xv_port_t port, gchar **failure)
{
  xcb_void_cookie_t cookie = xcb_xv_ungrab_port_checked(holder->connection, port, XCB_CURRENT_TIME);
  /* The bounded later reply makes this checked void request's status available. */
  if (!contention_check(barrier_locked(holder), failure, "owned-port ungrab barrier failed")) return FALSE;
  xcb_generic_error_t *error = xcb_request_check(holder->connection, cookie);
  gboolean ok = contention_check(error == NULL, failure, "owned-port ungrab was rejected");
  free(error); return ok;
}
static gboolean contention_failed_bus(GstBus *bus, GstElement *sink, gchar **failure)
{
  guint messages = 0, fallback = 0;
  GstMessage *message;
  /* A failed NULL->READY need not perform READY->NULL's automatic flush.
   * Record this attempt's BaseSink fallback before any new PLAYING request. */
  while ((message = gst_bus_pop(bus))) {
    gboolean bounded = ++messages <= 64;
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
      GError *error = NULL; gchar *debug = NULL;
      gst_message_parse_error(message, &error, &debug);
      gboolean expected = GST_MESSAGE_SRC(message) == GST_OBJECT(sink) && error &&
          error->domain == GST_CORE_ERROR && error->code == GST_CORE_ERROR_STATE_CHANGE &&
          debug && strstr(debug, "gst_base_sink_change_state") && strstr(debug, "Failed to start");
      g_test_message("failed-start queued error source=%s domain=%s code=%d message=%s debug=%s",
          GST_OBJECT_NAME(GST_MESSAGE_SRC(message)), error ? g_quark_to_string(error->domain) : "none",
          error ? error->code : 0, error ? error->message : "none", debug ? debug : "none");
      if (expected) fallback++;
      contention_check(expected && fallback <= 1, failure, "unexpected error after failed port admission");
      g_clear_error(&error); g_free(debug);
    }
    gst_message_unref(message);
    if (!contention_check(bounded, failure, "failed-attempt bus exceeded64 messages")) break;
  }
  contention_check(!gst_bus_have_pending(bus), failure, "failed-attempt bus is not empty before reopen");
  g_test_message("failed-start bus: recorded-messages=%u expected-state-errors=%u empty=%s",
      messages, fallback, gst_bus_have_pending(bus) ? "no" : "yes");
  return *failure == NULL;
}
static gboolean contention_eos(GstElement *pipeline, GstElement *sink,
                               xcb_xv_port_t port, gchar **failure)
{
  XvContext *c = NULL; int connection_fd = -1, wake_fd = -1;
  GstBus *bus = gst_element_get_bus(pipeline);
  gboolean started = gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE;
  contention_check(started, failure, "released port did not admit the same pipeline");
  c = sink_context(XV_SINK(sink));
  if (contention_check(c != NULL, failure, "reopened sink has no context")) {
    g_mutex_lock(&c->lock);
    contention_check(c->port == port, failure, "reopen selected a different port (environment changed)");
    connection_fd = xcb_get_file_descriptor(c->connection); wake_fd = c->wake_fd;
    g_mutex_unlock(&c->lock);
  }
  if (started && !*failure) {
    GstMessage *message = gst_bus_timed_pop_filtered(bus, 8 * GST_SECOND, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);
    if (message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
      GError *error = NULL; gchar *debug = NULL;
      gst_message_parse_error(message, &error, &debug);
      g_test_message("reopen bus error source=%s domain=%s code=%d message=%s debug=%s",
          GST_OBJECT_NAME(GST_MESSAGE_SRC(message)), error ? g_quark_to_string(error->domain) : "none",
          error ? error->code : 0, error ? error->message : "none", debug ? debug : "none");
      g_clear_error(&error); g_free(debug);
    }
    contention_check(message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS,
        failure, "finite reopened pipeline did not reach natural EOS");
    if (message) gst_message_unref(message);
  }
  contention_check(gst_element_set_state(pipeline, GST_STATE_NULL) != GST_STATE_CHANGE_FAILURE,
      failure, "reopened pipeline refused NULL");
  GstState state;
  contention_check(gst_element_get_state(pipeline, &state, NULL, 3 * GST_SECOND) == GST_STATE_CHANGE_SUCCESS &&
      state == GST_STATE_NULL, failure, "reopened pipeline did not settle at NULL");
  if (c) {
    gint64 end = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;
    while (g_atomic_int_get(&c->references) != 1 && g_get_monotonic_time() < end) g_usleep(1000);
    contention_check(g_atomic_int_get(&c->references) == 1, failure, "reopened context still has another owner");
    g_mutex_lock(&c->lock);
    contention_check(c->closed && c->detached && !c->connection && !c->last_buffer &&
        !c->failed && !c->quarantined && !c->pending && !c->image_count && !c->image_bytes &&
        c->allocated == 6 && c->freed == c->allocated && c->video_submitted == 12 &&
        c->submitted == c->video_submitted + c->redraw_submitted &&
        c->submitted == c->acknowledged && c->submitted == c->pool_submitted &&
        !c->terminal && !c->copied_bytes, failure, "terminal reopen counters/SHM retirement failed");
    g_test_message("port=%u video=%" G_GUINT64_FORMAT " submit/ACK=%" G_GUINT64_FORMAT
        "/%" G_GUINT64_FORMAT " allocated/freed=%" G_GUINT64_FORMAT "/%" G_GUINT64_FORMAT,
        c->port, c->video_submitted, c->submitted, c->acknowledged, c->allocated, c->freed);
    g_mutex_unlock(&c->lock); context_unref(c);
    /* No descriptor-opening helper runs between retirement and these probes. */
    contention_check(connection_fd >= 0 && fcntl(connection_fd, F_GETFD) == -1 && errno == EBADF,
        failure, "reopened XCB descriptor did not close");
    contention_check(wake_fd >= 0 && fcntl(wake_fd, F_GETFD) == -1 && errno == EBADF,
        failure, "reopened event descriptor did not close");
  }
  gst_object_unref(bus); return *failure == NULL;
}
static void test_port_contention_reopen(void)
{
  if (!x11_enabled()) return;
  gchar *failure = NULL, *skip = NULL;
  GstElement *pipeline = NULL, *sink = NULL; GstBus *bus = NULL;
  XvContext holder = {.wake_fd = -1};
  GArray *owned = g_array_new(FALSE, FALSE, sizeof(xcb_xv_port_t));
  GArray *seen = g_array_new(FALSE, FALSE, sizeof(xcb_xv_port_t));
  xcb_xv_query_adaptors_reply_t *adaptors = NULL;
  guint busy = 0; int holder_fd = -1;
  g_mutex_init(&holder.lock); g_cond_init(&holder.changed); g_mutex_lock(&holder.lock);
#define CONTENTION_REQUIRE(ok, message) do { if (!contention_check((ok), &failure, (message))) goto cleanup; } while (0)
  CONTENTION_REQUIRE(!g_atomic_int_get(&live_contexts) && !own_segments(), "test requires an idle own-context/SHM baseline");
  const char *display = g_getenv("DISPLAY");
  if (!display || !g_regex_match_simple("^(:|unix/:)[0-9]+(\\.[0-9]+)?$", display, 0, 0)) {
    skip = g_strdup("A local Unix DISPLAY is required"); goto cleanup;
  }
  int screen_number;
  holder.connection = xcb_connect(display, &screen_number);
  if (!holder.connection || xcb_connection_has_error(holder.connection)) {
    skip = g_strdup("No authorized live local X11 connection"); goto cleanup;
  }
  holder_fd = xcb_get_file_descriptor(holder.connection);
  xcb_screen_iterator_t screen = xcb_setup_roots_iterator(xcb_get_setup(holder.connection));
  for (int n = 0; n < screen_number && screen.rem; n++) xcb_screen_next(&screen);
  if (!screen.rem) { skip = g_strdup("Selected X11 screen is unavailable"); goto cleanup; }
  for (guint n = 0; n < 2; n++) {
    const char *name = n ? "XVideo" : "MIT-SHM";
    xcb_query_extension_reply_t *extension = reply_locked(&holder,
        xcb_query_extension(holder.connection, strlen(name), name).sequence);
    CONTENTION_REQUIRE(extension != NULL, "extension query failed");
    gboolean present = extension->present; free(extension);
    if (!present) { skip = g_strdup_printf("%s is unavailable", name); goto cleanup; }
  }
  adaptors = reply_locked(&holder, xcb_xv_query_adaptors(holder.connection, screen.data->root).sequence);
  CONTENTION_REQUIRE(adaptors != NULL, "Xv adaptor query failed");
  gint64 end = g_get_monotonic_time() + 8 * G_USEC_PER_SEC;
  xcb_xv_adaptor_info_iterator_t a = xcb_xv_query_adaptors_info_iterator(adaptors);
  for (; a.rem; xcb_xv_adaptor_info_next(&a)) {
    if (!(a.data->type & XCB_XV_TYPE_IMAGE_MASK)) continue;
    for (guint j = 0; j < a.data->num_ports; j++) {
      CONTENTION_REQUIRE(g_get_monotonic_time() < end, "bounded port enumeration exceeded8s");
      xcb_xv_port_t port = a.data->base_id + j; gboolean duplicate = FALSE;
      for (guint k = 0; k < seen->len; k++) if (g_array_index(seen, xcb_xv_port_t, k) == port) duplicate = TRUE;
      if (duplicate) continue;
      CONTENTION_REQUIRE(seen->len < 64, "bounded port enumeration exceeded64 candidates");
      g_array_append_val(seen, port);
      xcb_xv_list_image_formats_reply_t *formats = reply_locked(&holder,
          xcb_xv_list_image_formats(holder.connection, port).sequence);
      CONTENTION_REQUIRE(formats != NULL, "port format query failed");
      gboolean compatible = FALSE;
      xcb_xv_image_format_info_t *f = xcb_xv_list_image_formats_format(formats);
      for (int k = 0; k < xcb_xv_list_image_formats_format_length(formats); k++)
        if (f[k].id == XV_UYVY) compatible = TRUE;
      free(formats); if (!compatible) continue;
      xcb_xv_grab_port_reply_t *grab = reply_locked(&holder,
          xcb_xv_grab_port(holder.connection, port, XCB_CURRENT_TIME).sequence);
      CONTENTION_REQUIRE(grab != NULL, "port grab reply failed");
      guint result = grab->result; free(grab);
      if (result == XCB_GRAB_STATUS_SUCCESS) g_array_append_val(owned, port);
      else if (result == XCB_GRAB_STATUS_ALREADY_GRABBED) busy++;
      else CONTENTION_REQUIRE(FALSE, "unexpected Xv grab status");
    }
  }
  if (!owned->len) { skip = g_strdup("No free compatible UYVY Xv port; other clients' grabs were untouched"); goto cleanup; }
  g_test_message("private holder owns%u free UYVY ports; left%u already-grabbed ports untouched", owned->len, busy);
  GError *parse_error = NULL;
  pipeline = gst_parse_launch("videotestsrc num-buffers=12 is-live=false pattern=black ! "
      "video/x-raw,format=UYVY,width=320,height=240,framerate=50/1,colorimetry=bt601 ! "
      "crystalhdxvsink name=output sync=true qos=true show-preroll-frame=false "
      "max-lateness=5000000 processing-deadline=15000000", &parse_error);
  gboolean parsed = pipeline != NULL && parse_error == NULL;
  g_clear_error(&parse_error); CONTENTION_REQUIRE(parsed, "cannot create the decoder-free finite pipeline");
  sink = gst_bin_get_by_name(GST_BIN(pipeline), "output"); bus = gst_element_get_bus(pipeline);
  CONTENTION_REQUIRE(sink != NULL && bus != NULL, "missing public sink/bus");
  gint before_fds = contention_fds();
  CONTENTION_REQUIRE(before_fds >= 0, "cannot count scoped admission descriptors");
  CONTENTION_REQUIRE(gst_element_set_state(pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE,
      "sink unexpectedly admitted while all observed free ports were held");
  GstMessage *message = gst_bus_timed_pop_filtered(bus, GST_SECOND, GST_MESSAGE_ERROR);
  gboolean expected = message && GST_MESSAGE_SRC(message) == GST_OBJECT(sink);
  if (message) {
    GError *error = NULL; gst_message_parse_error(message, &error, NULL);
    expected = expected && error && error->domain == GST_RESOURCE_ERROR &&
        error->code == GST_RESOURCE_ERROR_OPEN_READ_WRITE && !strcmp(error->message, "No free UYVY XVideo port");
    g_clear_error(&error); gst_message_unref(message);
  }
  CONTENTION_REQUIRE(expected, "missing exact public-sink busy-port resource error");
  CONTENTION_REQUIRE(gst_element_set_state(pipeline, GST_STATE_NULL) != GST_STATE_CHANGE_FAILURE,
      "failed admission refused NULL");
  GstState stopped;
  CONTENTION_REQUIRE(gst_element_get_state(pipeline, &stopped, NULL, 3 * GST_SECOND) == GST_STATE_CHANGE_SUCCESS &&
      stopped == GST_STATE_NULL, "failed admission did not settle at NULL");
  CONTENTION_REQUIRE(contention_failed_bus(bus, sink, &failure), "failed-attempt bus cleanup failed");
  XvContext *unexpected = sink_context(XV_SINK(sink)); gboolean has_context = unexpected != NULL;
  if (unexpected) context_unref(unexpected);
  CONTENTION_REQUIRE(!has_context && !g_atomic_int_get(&live_contexts) && !own_segments() &&
      contention_fds() == before_fds, "failed admission leaked a context/SHM/descriptor");
  xcb_xv_port_t released = g_array_index(owned, xcb_xv_port_t, 0);
  CONTENTION_REQUIRE(contention_ungrab(&holder, released, &failure), "release barrier failed");
  g_array_index(owned, xcb_xv_port_t, 0) = 0;
  for (guint cycle = 0; cycle < 2; cycle++) {
    CONTENTION_REQUIRE(contention_eos(pipeline, sink, released, &failure), "same-pipeline EOS/reopen failed");
    xcb_xv_grab_port_reply_t *grab = reply_locked(&holder,
        xcb_xv_grab_port(holder.connection, released, XCB_CURRENT_TIME).sequence);
    CONTENTION_REQUIRE(grab != NULL, "retired exact-port grab reply failed");
    guint result = grab->result; free(grab);
    if (result == XCB_GRAB_STATUS_SUCCESS) g_array_index(owned, xcb_xv_port_t, 0) = released;
    CONTENTION_REQUIRE(result == XCB_GRAB_STATUS_SUCCESS, "retired exact port was not available again");
    CONTENTION_REQUIRE(contention_ungrab(&holder, released, &failure), "retired exact-port release failed");
    g_array_index(owned, xcb_xv_port_t, 0) = 0;
  }
cleanup:
  if (pipeline) { gst_element_set_state(pipeline, GST_STATE_NULL); gst_element_get_state(pipeline, NULL, NULL, 3 * GST_SECOND); }
  if (bus) gst_object_unref(bus);
  if (sink) gst_object_unref(sink);
  if (pipeline) gst_object_unref(pipeline);
  free(adaptors);
  if (holder.connection) {
    for (guint k = 0; k < owned->len; k++) {
      xcb_xv_port_t port = g_array_index(owned, xcb_xv_port_t, k);
      if (port) contention_ungrab(&holder, port, &failure);
    }
    xcb_disconnect(holder.connection);
  }
  if (holder_fd >= 0) contention_check(fcntl(holder_fd, F_GETFD) == -1 && errno == EBADF,
      &failure, "private holder descriptor did not close");
  g_array_unref(owned); g_array_unref(seen);
  g_mutex_unlock(&holder.lock); g_cond_clear(&holder.changed); g_mutex_clear(&holder.lock);
  contention_check(!g_atomic_int_get(&live_contexts) && !own_segments(), &failure, "final context/SHM baseline not restored");
  if (failure) { g_test_message("port-contention failure: %s", failure); g_test_fail(); }
  else if (skip) g_test_skip(skip);
  g_free(skip); g_free(failure);
#undef CONTENTION_REQUIRE
}
static void test_caps_and_flush(void)
{
  if (!x11_enabled()) return;
  GError *error = NULL;
  GstElement *pipeline = gst_parse_launch("appsrc name=input format=time is-live=true ! "
      "crystalhdxvsink name=output sync=false show-preroll-frame=false", &error);
  g_assert_no_error(error);
  GstElement *source = gst_bin_get_by_name(GST_BIN(pipeline), "input");
  gst_element_set_state(pipeline, GST_STATE_PLAYING);
  for (guint epoch = 0; epoch < 2; epoch++) {
    GstVideoInfo info = info_for(epoch ? 640 : 320, epoch ? 360 : 240);
    GstCaps *caps = gst_video_info_to_caps(&info); gst_app_src_set_caps(GST_APP_SRC(source), caps); gst_caps_unref(caps);
    for (guint i = 0; i < 12; i++)
      g_assert_cmpint(gst_app_src_push_buffer(GST_APP_SRC(source), pattern_buffer(&info, i + epoch * 12)), ==, GST_FLOW_OK);
  }
  gst_app_src_end_of_stream(GST_APP_SRC(source)); await_message(pipeline, GST_MESSAGE_EOS);
  GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "output");
  GstStructure *stats = sink_stats(sink);
  g_assert_cmpuint(stat64(stats, "video-submitted"), ==, 24);
  g_assert_cmpuint(stat64(stats, "submitted"), ==, 24 + stat64(stats, "redraw-submitted"));
  g_assert_cmpuint(stat64(stats, "acknowledged"), ==, stat64(stats, "submitted"));
  g_assert_cmpuint(stat64(stats, "copied-bytes"), >, 0);
  gst_structure_free(stats); gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(sink); gst_object_unref(source); gst_object_unref(pipeline); clean_contexts();
  pipeline = new_pipeline(50, TRUE);
  gst_element_set_state(pipeline, GST_STATE_PLAYING);
  g_usleep(150000);
  g_assert_true(gst_element_seek_simple(pipeline, GST_FORMAT_TIME,
      GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT, 400 * GST_MSECOND));
  await_message(pipeline, GST_MESSAGE_EOS);
  gst_element_set_state(pipeline, GST_STATE_NULL); gst_object_unref(pipeline); clean_contexts();
}
static int hold_segment(void)
{
  int id; char byte = 'r';
  if (read(STDIN_FILENO, &id, sizeof(id)) != sizeof(id)) return 3;
  void *data = shmat(id, NULL, 0);
  if (data == (void *)-1 || write(STDOUT_FILENO, &byte, 1) != 1 ||
      read(STDIN_FILENO, &byte, 1) != 1 || shmdt(data)) return 3;
  return 0;
}
static void test_quarantine(void)
{
  if (!x11_enabled()) return;
  GstElement *sink = direct_sink(); XvContext *c = sink_context(XV_SINK(sink));
  GstVideoInfo info = info_for(320, 240); GstBuffer *buffer = pattern_buffer(&info, 0);
  g_atomic_pointer_set(&drop_connection, c->connection); g_atomic_int_set(&drop_type, c->completion_type);
  g_assert_cmpint(submit_buffer(c, &info, buffer, NULL), ==, GST_FLOW_OK);
  g_mutex_lock(&c->lock); int id = c->copies[0]->shmid; g_mutex_unlock(&c->lock);
  gchar *args[] = {(gchar *)self_path, "--hold-segment", NULL};
  GPid child; gint input, output; GError *error = NULL;
  g_assert_true(g_spawn_async_with_pipes(NULL, args, NULL, G_SPAWN_DO_NOT_REAP_CHILD,
      NULL, NULL, &child, &input, &output, NULL, &error)); g_assert_no_error(error);
  g_assert_cmpint(write(input, &id, sizeof(id)), ==, sizeof(id));
  struct pollfd fd = {output, POLLIN, 0}; char byte;
  g_assert_cmpint(poll(&fd, 1, 1000), ==, 1); g_assert_cmpint(read(output, &byte, 1), ==, 1);
  g_assert_true(sink_stop(GST_BASE_SINK(sink))); g_atomic_pointer_set(&drop_connection, NULL);
  g_mutex_lock(&c->lock);
  g_assert_true(c->quarantined); g_assert_cmpuint(c->pending, ==, 1);
  g_assert_cmpuint(c->acknowledged, ==, 0); g_assert_cmpuint(c->terminal, ==, 0);
  g_mutex_unlock(&c->lock);
  g_assert_true(sink_start(GST_BASE_SINK(sink))); /* Same element, old context still retained. */
  byte = 'd'; g_assert_cmpint(write(input, &byte, 1), ==, 1); close(input); close(output);
  int status; g_assert_cmpint(waitpid(child, &status, 0), ==, child);
  g_assert_true(WIFEXITED(status)); g_assert_cmpint(WEXITSTATUS(status), ==, 0); g_spawn_close_pid(child);
  gint64 end = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
  g_mutex_lock(&c->lock);
  while (c->pending && g_get_monotonic_time() < end) g_cond_wait_until(&c->changed, &c->lock, end);
  g_assert_cmpuint(c->pending, ==, 0); g_assert_cmpuint(c->acknowledged, ==, 0);
  g_assert_cmpuint(c->terminal, ==, 1); g_assert_false(c->quarantined);
  g_mutex_unlock(&c->lock); context_unref(c); gst_buffer_unref(buffer);
  g_assert_true(sink_stop(GST_BASE_SINK(sink))); gst_object_unref(sink); clean_contexts();
}
static guint device_fds(void)
{
  DIR *directory = opendir("/proc/self/fd"); g_assert_nonnull(directory);
  struct dirent *entry; guint count = 0;
  while ((entry = readdir(directory))) {
    char target[256]; ssize_t length = readlinkat(dirfd(directory), entry->d_name, target, sizeof(target) - 1);
    if (length >= 0) { target[length] = '\0'; if (!strcmp(target, "/dev/crystalhd")) count++; }
  }
  closedir(directory); return count;
}
static void link_native_video(GstElement *demux, GstPad *pad, gpointer data)
{
  (void)demux;
  GstCaps *caps = gst_pad_query_caps(pad, NULL);
  gboolean video = caps && gst_caps_get_size(caps) == 1 &&
      gst_structure_has_name(gst_caps_get_structure(caps, 0), "video/x-h264");
  if (caps) gst_caps_unref(caps);
  if (!video) return;
  GstElement *parser = gst_bin_get_by_name(GST_BIN(data), "parser");
  GstPad *input = gst_element_get_static_pad(parser, "sink");
  g_assert_cmpint(gst_pad_link(pad, input), ==, GST_PAD_LINK_OK);
  gst_object_unref(input); gst_object_unref(parser);
}
static void native_error_reopen(gboolean clocked, gboolean preroll, gboolean mp4, gboolean restore_parser)
{
  if (!x11_enabled()) return;
  const char *fixture = g_getenv(mp4 ? "CRYSTALHD_XV_TEST_MP4" : "CRYSTALHD_XV_TEST_FIXTURE");
  if (!fixture) {
    g_test_skip(mp4 ? "Set CRYSTALHD_XV_TEST_MP4 to the qualified 180-picture high.mp4 fixture" :
        "Set CRYSTALHD_XV_TEST_FIXTURE to the qualified 50-picture interlaced H.264 fixture"); return;
  }
  g_assert_true(g_file_test(fixture, G_FILE_TEST_IS_REGULAR));
  guint originals = mp4 ? 180 : 50, outputs = mp4 ? 180 : 100;
  GError *error = NULL;
  GstElement *pipeline = gst_parse_launch(mp4 ?
      "filesrc name=input ! qtdemux name=demux "
      "h264parse name=parser ! crystalhddec name=decoder ! "
      "videoconvert name=convert n-threads=1 ! video/x-raw,format=UYVY ! "
      "crystalhdxvsink name=output qos=true" :
      "filesrc name=input ! h264parse name=parser ! "
      "crystalhddec name=decoder scale-width=1280 ! deinterlace name=di method=linear fields=all mode=auto ! "
      "videoconvert n-threads=1 ! video/x-raw,format=UYVY ! "
      "crystalhdxvsink name=output show-preroll-frame=false qos=true", &error);
  g_assert_no_error(error);
  GstElement *source = gst_bin_get_by_name(GST_BIN(pipeline), "input");
  if (mp4) {
    /* qtdemux recreates dynamic pads on NULL/restart. gst_parse_launch's
     * one-shot delayed link cannot own repeated demuxer lifetimes. */
    GstElement *demux = gst_bin_get_by_name(GST_BIN(pipeline), "demux");
    g_signal_connect(demux, "pad-added", G_CALLBACK(link_native_video), pipeline);
    gst_object_unref(demux);
  }
  GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "output");
  /* Separate an unclocked ownership/EOS census from the strict clocked
   * presentation census. The latter retains its 100-output requirement. */
  g_object_set(sink, "sync", clocked, "show-preroll-frame", preroll, NULL);
  GstElement *deinterlace = mp4 ? NULL : gst_bin_get_by_name(GST_BIN(pipeline), "di");
  GstElement *controller = mp4 ? NULL : gst_element_factory_make("crystalhddiqos", NULL);
  GstBus *bus = gst_element_get_bus(pipeline);
  if (!mp4) {
    g_assert_nonnull(controller);
    gint attached = -1;
    g_signal_emit_by_name(controller, "attach", deinterlace, bus, &attached);
    g_assert_cmpint(attached, ==, 1);
  }
  NativeStage stages[4] = {{0}};
  const char *stage_names[] = {"parser", "decoder", mp4 ? "convert" : "di", "output"};
  for (guint i = 0; i < 4; i++) {
    GstElement *element = gst_bin_get_by_name(GST_BIN(pipeline), stage_names[i]);
    GstPad *pad = gst_element_get_static_pad(element, i == 3 ? "sink" : "src");
    g_assert_cmpuint(gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER,
        count_native_stage, &stages[i], NULL), !=, 0);
    gst_object_unref(pad); gst_object_unref(element);
  }
  g_object_set_data(G_OBJECT(pipeline), "xv-native-stages", stages);
  g_object_set_data(G_OBJECT(pipeline), "xv-native-controller", controller);
  g_object_set(source, "location", fixture, NULL);
  /* Lose only this output connection's completion. The application and
   * decoder must close normally and reuse the same pipeline afterward. */
  g_assert_cmpint(gst_element_set_state(pipeline, GST_STATE_PAUSED), !=, GST_STATE_CHANGE_FAILURE);
  XvContext *c = sink_context(XV_SINK(sink)); g_assert_nonnull(c);
  g_atomic_pointer_set(&drop_connection, c->connection); g_atomic_int_set(&drop_type, c->completion_type);
  gst_element_set_state(pipeline, GST_STATE_PLAYING); await_message(pipeline, GST_MESSAGE_ERROR);
  gst_element_set_state(pipeline, GST_STATE_NULL); g_atomic_pointer_set(&drop_connection, NULL);
  g_mutex_lock(&c->lock);
  g_assert_true(c->failed); g_assert_cmpuint(c->submitted, >, 0);
  g_assert_cmpuint(c->terminal, >, 0);
  g_assert_cmpuint(c->submitted, ==, c->acknowledged + c->terminal);
  g_mutex_unlock(&c->lock);
  g_assert_cmpuint(device_fds(), ==, 0);
  context_unref(c); clean_contexts();
  for (guint cycle = 0; cycle < 3; cycle++) {
    /* One compatibility controller owns this deinterlacer for its lifetime,
     * including NULL/restart. Detach does not permit attaching a second owner. */
    memset(stages, 0, sizeof(stages));
    if (restore_parser) {
      /* Test-local counterfactual: 1.28.2 BaseParse resets these policies at
       * PAUSED->READY, while H264Parse disables them only during construction.
       * Reapply its intended policy while stopped; do not rewrite timestamps,
       * alter QoS or replace the unmodified raw-stream failure controls. */
      GstElement *parser = gst_bin_get_by_name(GST_BIN(pipeline), "parser");
      gst_base_parse_set_pts_interpolation(GST_BASE_PARSE(parser), FALSE);
      gst_base_parse_set_infer_ts(GST_BASE_PARSE(parser), FALSE);
      gst_object_unref(parser);
    }
    check_eos(pipeline, outputs + (preroll ? 1 : 0));
    g_assert_cmpint(stages[0].buffers, ==, originals);
    g_assert_cmpint(stages[1].buffers, ==, originals);
    g_assert_cmpint(stages[2].buffers, ==, outputs);
    g_assert_cmpint(stages[3].buffers, ==, outputs);
    for (guint i = 0; i < outputs; i++) {
      g_assert_cmpuint(stages[2].pts[i], ==, stages[3].pts[i]);
      if (mp4 || restore_parser) {
        g_assert_true(GST_CLOCK_TIME_IS_VALID(stages[3].pts[i]));
        if (i) g_assert_cmpuint(stages[3].pts[i], >, stages[3].pts[i-1]);
      }
    }
    g_assert_cmpuint(device_fds(), ==, 0);
    g_test_message("Native %s same-pipeline reopen %u: %u outputs, EOS, live-process device FD0",
        clocked ? "clocked" : "unclocked", cycle + 1, outputs);
  }
  if (controller) { g_signal_emit_by_name(controller, "detach"); gst_object_unref(controller); }
  if (deinterlace) gst_object_unref(deinterlace);
  gst_object_unref(bus);
  gst_object_unref(source); gst_object_unref(sink); gst_object_unref(pipeline); clean_contexts();
}
static void test_native_error_reopen(void) { native_error_reopen(TRUE, FALSE, FALSE, FALSE); }
static void test_native_error_reopen_lifetime(void) { native_error_reopen(FALSE, FALSE, FALSE, FALSE); }
static void test_native_error_reopen_default(void) { native_error_reopen(TRUE, TRUE, FALSE, FALSE); }
static void test_native_error_reopen_mp4(void) { native_error_reopen(TRUE, TRUE, TRUE, FALSE); }
static void test_native_error_reopen_parser_policy(void) { native_error_reopen(TRUE, TRUE, FALSE, TRUE); }
int main(int argc, char **argv)
{
  if (argc == 2 && !strcmp(argv[1], "--hold-segment")) return hold_segment();
  self_path = g_canonicalize_filename(argv[0], NULL);
  gst_init(&argc, &argv); g_test_init(&argc, &argv, NULL);
  g_assert_true(plugin_init(NULL));
  g_test_add_func("/xv/identity", test_identity);
  g_test_add_func("/xv/setup-failure", test_setup_failure);
  g_test_add_func("/xv/color-admission", test_color_admission);
  g_test_add_func("/xv/unspecified-color-pixels", test_unspecified_color_pixels);
  g_test_add_func("/xv/x11/reopen", test_reopen);
  g_test_add_func("/xv/x11/preroll", test_preroll);
  g_test_add_func("/xv/x11/padded-copy", test_padded_copy);
  g_test_add_func("/xv/x11/paused-expose", test_paused_expose);
  g_test_add_func("/xv/x11/paused-resize-pool", test_paused_resize_pool);
  g_test_add_func("/xv/x11/paused-resize-copy", test_paused_resize_copy);
  g_test_add_func("/xv/x11/partial-setup", test_partial_setup);
  g_test_add_func("/xv/x11/transport-reopen", test_transport_reopen);
  g_test_add_func("/xv/x11/reentrant-error-stop", test_reentrant_error_stop);
  g_test_add_func("/xv/x11/two-instances", test_two_instances);
  g_test_add_func("/xv/x11/port-contention-reopen", test_port_contention_reopen);
  g_test_add_func("/xv/x11/caps-flush", test_caps_and_flush);
  g_test_add_func("/xv/x11/held-pool-caps", test_held_pool_and_caps);
  g_test_add_func("/xv/x11/live-window-replacement", test_live_window_replacement);
  g_test_add_func("/xv/x11/submit-flush-failure", test_submit_flush_failure);
  g_test_add_func("/xv/x11/quarantine", test_quarantine);
  g_test_add_func("/xv/card/error-reopen", test_native_error_reopen);
  g_test_add_func("/xv/card/error-reopen-lifetime", test_native_error_reopen_lifetime);
  g_test_add_func("/xv/card/error-reopen-default", test_native_error_reopen_default);
  g_test_add_func("/xv/card/error-reopen-mp4", test_native_error_reopen_mp4);
  g_test_add_func("/xv/card/error-reopen-parser-policy", test_native_error_reopen_parser_policy);
  int result = g_test_run(); g_free((gpointer)self_path); return result;
}
