/* SPDX-License-Identifier: MIT */
/* Actual element/transport code; syscall faults affect only this test process. */
#define _GNU_SOURCE
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <xcb/xcb.h>
#include <errno.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static gint fail_alloc_after = -1, fail_attach, defer_detach;
static gpointer drop_connection;
static gpointer delay_connection, delayed_event;
static gint release_delayed;
static gint drop_type;
static const char *self_path;
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
#include "../filters/gst/xv-output/gstcrystalhdxv.c"
#undef shmget
#undef shmat
#undef shmctl
#undef xcb_poll_for_event

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
    g_test_message("Native stages: parsed=%d decoded=%d deinterlaced=%d sink-input=%d",
        g_atomic_int_get(&stages[0].buffers), g_atomic_int_get(&stages[1].buffers),
        g_atomic_int_get(&stages[2].buffers), g_atomic_int_get(&stages[3].buffers));
    for (gint i = 0; i < MIN(stages[2].buffers, 512); i++) {
      gboolean found = FALSE;
      for (gint j = 0; j < MIN(stages[3].buffers, 512); j++)
        if (stages[2].pts[i] == stages[3].pts[j]) { found = TRUE; break; }
      if (!found) g_test_message("DI output absent at sink: index=%d PTS=%" GST_TIME_FORMAT,
          i, GST_TIME_ARGS(stages[2].pts[i]));
    }
    GstElement *controller = g_object_get_data(G_OBJECT(pipeline), "xv-native-controller");
    gboolean active, eligible; guint64 qos, forwarded;
    g_object_get(controller, "active", &active, "eligible", &eligible,
        "qos-count", &qos, "forward-count", &forwarded, NULL);
    g_test_message("DI controller: active=%d eligible=%d qos=%" G_GUINT64_FORMAT " forwarded=%" G_GUINT64_FORMAT,
        active, eligible, qos, forwarded);
  }
  /* Preserve the failed census, but close owned decoder/X11 resources before
   * an assertion terminates this isolated test process. */
  g_assert_cmpint(gst_element_set_state(pipeline, GST_STATE_NULL), !=, GST_STATE_CHANGE_FAILURE);
  gst_element_get_state(pipeline, NULL, NULL, 3 * GST_SECOND);
  clean_contexts();
  g_assert_cmpuint(stat64(stats, "submitted"), ==, count);
  g_assert_cmpuint(stat64(stats, "acknowledged"), ==, count);
  g_assert_cmpuint(stat64(stats, "terminal-retired"), ==, 0);
  g_assert_cmpuint(stat32(stats, "pending"), ==, 0);
  g_assert_cmpuint(stat64(stats, "pool-submitted"), ==, count);
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
  g_assert_cmpuint(stat64(stats, "submitted"), ==, 13);
  g_assert_cmpuint(stat64(stats, "acknowledged"), ==, 13);
  g_assert_cmpuint(stat64(stats, "pool-submitted"), ==, 13);
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
    XvMemory *m = c->copies[0];
    for (gint y = 0; y < info.height; y++)
      for (gint x = 0; x < info.width * 2; x++)
        g_assert_cmpuint(m->data[m->offset + y * m->pitch + x], ==, (index + y * 3 + x) % 251);
    g_mutex_unlock(&c->lock); gst_buffer_unref(buffer);
  }
  g_assert_true(sink_stop(GST_BASE_SINK(sink))); context_unref(c);
  gst_object_unref(sink); clean_contexts();
}
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
  g_assert_true(gst_buffer_pool_set_active(pool, FALSE)); gst_object_unref(pool);
  g_assert_true(gst_buffer_pool_set_active(new_pool, FALSE)); gst_object_unref(new_pool);
  sink_stop(GST_BASE_SINK(sink)); context_unref(c); gst_object_unref(sink); clean_contexts();
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
  g_assert_cmpuint(stat64(stats, "submitted"), ==, 24);
  g_assert_cmpuint(stat64(stats, "acknowledged"), ==, 24);
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
static void native_error_reopen(gboolean clocked)
{
  if (!x11_enabled()) return;
  const char *fixture = g_getenv("CRYSTALHD_XV_TEST_FIXTURE");
  if (!fixture) { g_test_skip("Set CRYSTALHD_XV_TEST_FIXTURE to the qualified 50-picture H.264 fixture"); return; }
  g_assert_true(g_file_test(fixture, G_FILE_TEST_IS_REGULAR));
  GError *error = NULL;
  GstElement *pipeline = gst_parse_launch("filesrc name=input ! h264parse name=parser ! "
      "crystalhddec name=decoder scale-width=1280 ! deinterlace name=di method=linear fields=all mode=auto ! "
      "videoconvert n-threads=1 ! video/x-raw,format=UYVY ! "
      "crystalhdxvsink name=output show-preroll-frame=false qos=true", &error);
  g_assert_no_error(error);
  GstElement *source = gst_bin_get_by_name(GST_BIN(pipeline), "input");
  GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "output");
  /* Separate an unclocked ownership/EOS census from the strict clocked
   * presentation census. The latter retains its 100-output requirement. */
  g_object_set(sink, "sync", clocked, NULL);
  GstElement *deinterlace = gst_bin_get_by_name(GST_BIN(pipeline), "di");
  GstElement *controller = gst_element_factory_make("crystalhddiqos", NULL);
  GstBus *bus = gst_element_get_bus(pipeline);
  g_assert_nonnull(controller);
  gint attached = -1;
  g_signal_emit_by_name(controller, "attach", deinterlace, bus, &attached);
  g_assert_cmpint(attached, ==, 1);
  NativeStage stages[4] = {{0}};
  const char *stage_names[] = {"parser", "decoder", "di", "output"};
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
  g_assert_cmpuint(device_fds(), ==, 0);
  context_unref(c); clean_contexts();
  for (guint cycle = 0; cycle < 3; cycle++) {
    /* One compatibility controller owns this deinterlacer for its lifetime,
     * including NULL/restart. Detach does not permit attaching a second owner. */
    memset(stages, 0, sizeof(stages));
    check_eos(pipeline, 100);
    g_assert_cmpint(stages[0].buffers, ==, 50);
    g_assert_cmpint(stages[1].buffers, ==, 50);
    g_assert_cmpint(stages[2].buffers, ==, 100);
    g_assert_cmpint(stages[3].buffers, ==, 100);
    for (guint i = 0; i < 100; i++)
      g_assert_cmpuint(stages[2].pts[i], ==, stages[3].pts[i]);
    g_assert_cmpuint(device_fds(), ==, 0);
    g_test_message("Native %s same-pipeline reopen %u: 100 outputs, EOS, live-process device FD0",
        clocked ? "clocked" : "unclocked", cycle + 1);
  }
  g_signal_emit_by_name(controller, "detach");
  gst_object_unref(controller); gst_object_unref(deinterlace); gst_object_unref(bus);
  gst_object_unref(source); gst_object_unref(sink); gst_object_unref(pipeline); clean_contexts();
}
static void test_native_error_reopen(void) { native_error_reopen(TRUE); }
static void test_native_error_reopen_lifetime(void) { native_error_reopen(FALSE); }
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
  g_test_add_func("/xv/x11/partial-setup", test_partial_setup);
  g_test_add_func("/xv/x11/transport-reopen", test_transport_reopen);
  g_test_add_func("/xv/x11/reentrant-error-stop", test_reentrant_error_stop);
  g_test_add_func("/xv/x11/two-instances", test_two_instances);
  g_test_add_func("/xv/x11/caps-flush", test_caps_and_flush);
  g_test_add_func("/xv/x11/held-pool-caps", test_held_pool_and_caps);
  g_test_add_func("/xv/x11/quarantine", test_quarantine);
  g_test_add_func("/xv/card/error-reopen", test_native_error_reopen);
  g_test_add_func("/xv/card/error-reopen-lifetime", test_native_error_reopen_lifetime);
  int result = g_test_run(); g_free((gpointer)self_path); return result;
}
