/* SPDX-License-Identifier: MIT */
/* Controlled real-Xv ACK delay: no claim of physical scanout or latency gain. */
static void latest_hold(XvContext *c)
{
  g_mutex_lock(&c->lock); c->redraw_suspended++;
  g_mutex_lock(&latest_event_lock); g_assert_true(g_queue_is_empty(&latest_events));
  latest_release_budget = 0; g_atomic_int_set(&drop_type, c->completion_type);
  g_atomic_pointer_set(&latest_event_connection, c->connection);
  g_mutex_unlock(&latest_event_lock); g_mutex_unlock(&c->lock);
}
static guint latest_held(void)
{
  g_mutex_lock(&latest_event_lock); guint count = latest_events.length;
  g_mutex_unlock(&latest_event_lock); return count;
}
static void latest_wait_held(guint count)
{
  gint64 end = g_get_monotonic_time() + XV_TIMEOUT_US;
  while (latest_held() != count && g_get_monotonic_time() < end) g_usleep(1000);
  g_assert_cmpuint(latest_held(), ==, count);
}
static void latest_wait_final_ack(XvContext *c)
{
  gint64 end = g_get_monotonic_time() + XV_TIMEOUT_US; gboolean ready = FALSE;
  while (!ready && g_get_monotonic_time() < end) {
    g_mutex_lock(&c->lock);
    ready = !c->failed && c->latest_submitted == 1 && c->video_submitted == 4 &&
        c->pending == 1 && latest_held() == 1;
    gboolean failed = c->failed; g_mutex_unlock(&c->lock);
    if (failed) break;
    if (!ready) g_usleep(1000);
  }
  g_assert_true(ready);
}
static void latest_release(XvContext *c, guint count)
{
  g_mutex_lock(&latest_event_lock); latest_release_budget += count;
  g_mutex_unlock(&latest_event_lock); wake_context(c);
}
static void latest_unhold(XvContext *c)
{
  g_mutex_lock(&c->lock); g_mutex_lock(&latest_event_lock);
  if (!c->failed && !c->closed) {
    g_assert_cmpuint(c->pending, ==, 0); g_assert_null(c->latest_buffer);
    g_assert_true(g_queue_is_empty(&latest_events));
  }
  g_atomic_pointer_set(&latest_event_connection, NULL);
  g_queue_clear_full(&latest_events, free); latest_release_budget = 0;
  g_mutex_unlock(&latest_event_lock); g_mutex_unlock(&c->lock);
}
static GstBuffer *latest_source(GstBufferPool *pool, const GstVideoInfo *info, guint id)
{
  GstBuffer *buffer = NULL;
  if (pool) g_assert_cmpint(gst_buffer_pool_acquire_buffer(pool, &buffer, NULL), ==, GST_FLOW_OK);
  else buffer = pattern_buffer(info, id);
  GstVideoFrame frame; g_assert_true(gst_video_frame_map(&frame, info, buffer, GST_MAP_WRITE));
  for (gint y = 0; y < info->height; y++) {
    guint8 *row = (guint8 *)GST_VIDEO_FRAME_PLANE_DATA(&frame, 0) + y * GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
    for (gint x = 0; x < info->width * 2; x += 4) {
      row[x] = row[x + 2] = 128; row[x + 1] = row[x + 3] = 16 + 24 * id;
    }
  }
  gst_video_frame_unmap(&frame); GST_BUFFER_OFFSET(buffer) = id; GST_BUFFER_OFFSET_END(buffer) = id + 1;
  GST_BUFFER_PTS(buffer) = id * 20 * GST_MSECOND; GST_BUFFER_DURATION(buffer) = 20 * GST_MSECOND;
  return buffer;
}
static void latest_memory(XvMemory *m, const GstVideoInfo *info, guint id)
{
  guint8 *row = g_malloc(info->width * 2);
  for (gint x = 0; x < info->width * 2; x += 4) {
    row[x] = row[x + 2] = 128; row[x + 1] = row[x + 3] = 16 + 24 * id;
  }
  for (gint y = 0; y < info->height; y++)
    g_assert_cmpmem(m->data + m->offset + y * m->pitch, info->width * 2, row, info->width * 2);
  g_free(row);
}
static void latest_ledger(XvContext *c)
{
  g_assert_cmpuint(c->latest_queued, ==, c->latest_replaced + c->latest_submitted +
      c->latest_cancelled + (c->latest_buffer != NULL));
}
static void latest_rgb(XvContext *c, guint8 rgb[3])
{
  xcb_get_image_reply_t *r = reply_locked(c, xcb_get_image(c->connection, XCB_IMAGE_FORMAT_Z_PIXMAP,
      c->window, c->width / 2, c->height / 2, 1, 1, UINT32_MAX).sequence);
  g_assert_nonnull(r); g_assert_cmpuint(r->depth, ==, 24); g_assert_cmpint(xcb_get_image_data_length(r), >=, 3);
  memcpy(rgb, xcb_get_image_data(r), 3); free(r);
}
typedef struct { XvContext *c; gint finished; gboolean result; } LatestDrain;
static gpointer latest_drain(gpointer data)
{
  LatestDrain *job = data; g_mutex_lock(&job->c->lock);
  job->result = drain_locked(job->c, TRUE); g_mutex_unlock(&job->c->lock);
  g_atomic_int_set(&job->finished, TRUE); return NULL;
}
typedef struct { XvContext owner; xcb_window_t window; } LatestOutput;
static void latest_output_open(LatestOutput *out)
{
  memset(out, 0, sizeof(*out)); out->owner.wake_fd = -1;
  g_mutex_init(&out->owner.lock); g_cond_init(&out->owner.changed);
  int screen_number = 0; const char *failure = NULL;
  out->owner.connection = connect_local_display(NULL, &screen_number, &failure);
  g_assert_nonnull(out->owner.connection); g_assert_cmpint(xcb_connection_has_error(out->owner.connection), ==, 0);
  xcb_screen_iterator_t screens = xcb_setup_roots_iterator(xcb_get_setup(out->owner.connection));
  for (int i = 0; i < screen_number && screens.rem; i++) xcb_screen_next(&screens);
  g_assert_cmpint(screens.rem, >, 0);
  out->window = xcb_generate_id(out->owner.connection); guint32 values[] = {0, TRUE};
  xcb_create_window(out->owner.connection, XCB_COPY_FROM_PARENT, out->window, screens.data->root,
      0, 0, 320, 240, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screens.data->root_visual,
      XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT, values);
  xcb_map_window(out->owner.connection, out->window); g_assert_true(barrier_locked(&out->owner));
}
static void latest_output_close(LatestOutput *out)
{
  xcb_destroy_window(out->owner.connection, out->window); g_assert_true(barrier_locked(&out->owner));
  xcb_disconnect(out->owner.connection); g_mutex_clear(&out->owner.lock); g_cond_clear(&out->owner.changed);
}
static xcb_xv_port_t latest_free_port(LatestOutput *output, XvContext *c, const GstVideoInfo *info)
{
  xcb_xv_query_adaptors_reply_t *r = reply_locked(&output->owner,
      xcb_xv_query_adaptors(output->owner.connection, c->root).sequence);
  g_assert_nonnull(r); xcb_xv_port_t found = 0;
  xcb_xv_adaptor_info_iterator_t a = xcb_xv_query_adaptors_info_iterator(r);
  for (; a.rem && !found; xcb_xv_adaptor_info_next(&a)) {
    if (!(a.data->type & XCB_XV_TYPE_IMAGE_MASK)) continue;
    for (guint i = 0; i < a.data->num_ports && !found; i++) {
      xcb_xv_port_t port = a.data->base_id + i; if (port == c->port) continue;
      g_mutex_lock(&c->lock);
      gboolean compatible = port_advertised_locked(c, port) && port_layouts_locked(c, port, info);
      g_mutex_unlock(&c->lock);
      if (compatible && grab_result(&output->owner, port) == XCB_GRAB_STATUS_SUCCESS) {
        ungrab_test_port(&output->owner, port); found = port;
      }
    }
  }
  free(r); return found;
}
static void latest_coalescing(guint mode, guint control)
{
  if (!x11_enabled()) return;
  LatestOutput output; latest_output_open(&output);
  GstElement *sink = g_object_new(gst_crystalhd_xv_get_type(), "latest-frame", TRUE, NULL);
  gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(sink), output.window);
  g_assert_true(sink_start(GST_BASE_SINK(sink))); XvContext *c = sink_context(XV_SINK(sink));
  gboolean enabled = FALSE; g_object_set(sink, "latest-frame", FALSE, NULL);
  g_object_get(sink, "latest-frame", &enabled, NULL); g_assert_true(enabled);
  GstVideoInfo info = info_for(320, 240); GstCaps *caps = gst_video_info_to_caps(&info);
  g_assert_true(sink_set_caps(GST_BASE_SINK(sink), caps)); gst_caps_unref(caps);
  GstBufferPool *pool = mode ? make_pool(sink, &info) : NULL;
  LatestOutput target; gboolean has_target = FALSE;
  xcb_xv_port_t next_port = control == 2 ? latest_free_port(&output, c, &info) : 0;
  if (control == 2 && !next_port) {
    sink_stop(GST_BASE_SINK(sink));
    if (pool) { gst_buffer_pool_set_active(pool, FALSE); gst_object_unref(pool); }
    context_unref(c); gst_object_unref(sink); clean_contexts(); latest_output_close(&output);
    g_test_skip("A second free compatible local UYVY port is required"); return;
  }
  GstVideoInfo next = info_for(640, 360);
  GstBuffer *first[3], *expected = NULL; XvFlight frozen[3]; guint8 *pixels[3];
  latest_hold(c);
  for (guint id = 0; id < 3; id++) {
    first[id] = latest_source(pool, &info, id);
    g_assert_cmpint(submit_buffer(c, &info, first[id], buffer_memory(first[id])), ==, GST_FLOW_OK);
  }
  latest_wait_held(3); g_mutex_lock(&c->lock);
  for (guint i = 0; i < 3; i++) {
    frozen[i] = c->flights[i]; g_assert_nonnull(frozen[i].memory); latest_memory(frozen[i].memory, &info, i);
    pixels[i] = g_memdup2(frozen[i].memory->data, frozen[i].memory->size);
  }
  g_mutex_unlock(&c->lock);
  for (guint id = 3; id < 8; id++) {
    GstBuffer *buffer = latest_source(mode == 1 ? pool : NULL, &info, id);
    g_assert_cmpint(submit_buffer(c, &info, buffer, buffer_memory(buffer)), ==, GST_FLOW_OK);
    if (id == 7) expected = gst_buffer_ref(buffer);
    gst_buffer_unref(buffer);
  }
  g_mutex_lock(&c->lock); guint64 allocated = c->allocated;
  g_assert_cmpuint(c->pending, ==, 3); g_assert_cmpuint(c->video_submitted, ==, 3);
  g_assert_cmpuint(c->latest_queued, ==, 5); g_assert_cmpuint(c->latest_replaced, ==, 4);
  g_assert_true(c->latest_buffer == expected); g_assert_true(c->last_buffer == first[2]); latest_ledger(c);
  for (guint i = 0; i < 3; i++) {
    XvFlight *f = &c->flights[i]; g_assert_true(f->memory == frozen[i].memory);
    g_assert_true(f->buffer == frozen[i].buffer); g_assert_cmpuint(f->pts, ==, frozen[i].pts);
    g_assert_cmpuint(f->generation, ==, frozen[i].generation); g_assert_cmpuint(f->epoch, ==, frozen[i].epoch);
    g_assert_cmpuint(f->sequence, ==, frozen[i].sequence); g_assert_true(f->memory->server_owned);
    g_assert_cmpmem(f->memory->data, f->memory->size, pixels[i], f->memory->size); g_free(pixels[i]);
  }
  g_mutex_unlock(&c->lock);
  LatestDrain job = {.c = c}; WindowJob window_job = {.sink = sink};
  PortJob port_job = {.sink = sink, .port = next_port};
  CapsJob caps_job = {.sink = sink}; GThread *thread = NULL;
  if (control == 1) {
    latest_output_open(&target); has_target = TRUE; window_job.window = target.window;
    thread = g_thread_new("latest-window", change_window, &window_job);
  } else if (control == 2) thread = g_thread_new("latest-port", change_port, &port_job);
  else if (control == 3) {
    caps_job.caps = gst_video_info_to_caps(&next);
    thread = g_thread_new("latest-caps", change_caps, &caps_job);
  } else thread = g_thread_new("latest-drain", latest_drain, &job);
  gint64 started_end = g_get_monotonic_time() + XV_TIMEOUT_US;
  gboolean started = FALSE;
  while (!started && g_get_monotonic_time() < started_end) {
    g_mutex_lock(&c->lock);
    started = (control == 1 || control == 2) ? c->output_changing : c->redraw_suspended >= 2;
    g_mutex_unlock(&c->lock); if (!started) g_usleep(1000);
  }
  g_assert_true(started); latest_release(c, 3); latest_wait_final_ack(c);
  if (control == 0) g_assert_false(g_atomic_int_get(&job.finished));
  if (control == 3) g_assert_false(g_atomic_int_get(&caps_job.finished));
  g_mutex_lock(&c->lock);
  g_assert_cmpuint(c->pending, ==, 1); g_assert_cmpuint(c->video_submitted, ==, 4);
  g_assert_cmpuint(c->latest_submitted, ==, 1); g_assert_null(c->latest_buffer);
  g_assert_true(c->last_buffer == expected); g_assert_cmpuint(c->allocated, ==, allocated); latest_ledger(c);
  XvFlight *last = NULL;
  for (guint i = 0; i < 3; i++) if (c->flights[i].memory) last = &c->flights[i];
  g_assert_nonnull(last); g_assert_cmpuint(last->pts, ==, 140 * GST_MSECOND);
  g_assert_true(mode == 1 ? last->buffer == expected : last->buffer == NULL);
  latest_memory(last->memory, &info, 7); g_mutex_unlock(&c->lock);
  guint8 actual[3], reference[3]; g_mutex_lock(&c->lock); latest_rgb(c, actual); g_mutex_unlock(&c->lock);
  latest_release(c, 1); g_thread_join(thread);
  if (control == 0) g_assert_true(job.result);
  if (control == 1) { g_assert_true(g_atomic_int_get(&window_job.finished)); g_assert_cmpuint(c->window, ==, target.window); }
  if (control == 2) { g_assert_true(g_atomic_int_get(&port_job.finished)); g_assert_cmpuint(c->port, ==, next_port); }
  if (control == 3) { g_assert_true(caps_job.result); gst_caps_unref(caps_job.caps); }
  g_mutex_lock(&c->lock); g_assert_true(drain_locked(c, TRUE)); g_mutex_unlock(&c->lock);
  latest_unhold(c);
  GstBuffer *new_layout = NULL;
  if (control == 3) {
    g_mutex_lock(&c->lock); g_assert_null(c->last_buffer); g_assert_cmpint(XV_SINK(sink)->info.width, ==, 640);
    g_mutex_unlock(&c->lock); new_layout = latest_source(NULL, &next, 8);
    g_assert_cmpint(submit_buffer(c, &next, new_layout, NULL), ==, GST_FLOW_OK);
  } else {
    /* Deliberate duplicate reference request, separate from the8 input IDs. */
    g_assert_cmpint(submit_buffer(c, &info, expected, buffer_memory(expected)), ==, GST_FLOW_OK);
  }
  g_mutex_lock(&c->lock); g_assert_true(drain_locked(c, TRUE)); latest_rgb(c, reference);
  if (control == 3) {
    g_assert_cmpint(memcmp(actual, reference, 3), !=, 0);
    window_pixel_locked(c, c->width / 2, 1, FALSE); /* New aspect's bar must replace old video. */
    g_assert_true(c->last_buffer == new_layout); g_assert_cmpint(c->last_info.width, ==, 640);
  } else g_assert_cmpmem(actual, 3, reference, 3);
  g_assert_cmpuint(c->video_submitted, ==, 5);
  g_assert_cmpuint(c->acknowledged, ==, c->submitted); latest_ledger(c); c->redraw_suspended--;
  g_mutex_unlock(&c->lock); g_assert_true(sink_stop(GST_BASE_SINK(sink)));
  for (guint i = 0; i < 3; i++) gst_buffer_unref(first[i]);
  gst_buffer_unref(expected); if (new_layout) gst_buffer_unref(new_layout);
  if (pool) { g_assert_true(gst_buffer_pool_set_active(pool, FALSE)); gst_object_unref(pool); }
  context_unref(c); gst_object_unref(sink); clean_contexts(); latest_output_close(&output); if (has_target) latest_output_close(&target);
  g_test_message("latest mode%u control%u:8 input IDs ->3 immediate+1 newest,4 intentional replacements; held flight pixels/PTS unchanged;final layout or explicit RGB reference request", mode, control);
}
static void test_latest_copy(void) { latest_coalescing(0, 0); }
static void test_latest_pool(void) { latest_coalescing(1, 0); }
static void test_latest_mixed(void) { latest_coalescing(2, 0); }
typedef struct { XvContext *c; GstVideoInfo info; gint started, finished; gboolean result; } LatestFifoProducer;
static gpointer latest_fifo_producer(gpointer data)
{
  LatestFifoProducer *job = data; job->result = TRUE; g_atomic_int_set(&job->started, TRUE);
  for (guint id = 3; id < 8; id++) {
    GstBuffer *buffer = latest_source(NULL, &job->info, id);
    GstFlowReturn flow = submit_buffer(job->c, &job->info, buffer, NULL);
    g_mutex_lock(&job->c->lock);
    gboolean ok = flow == GST_FLOW_OK && drain_locked(job->c, TRUE);
    if (ok) {
      XvMemory *newest = NULL;
      for (guint i = 0; i < XV_DEPTH; i++) {
        XvMemory *m = job->c->copies[i];
        if (m && (!newest || m->generation > newest->generation)) newest = m;
      }
      g_assert_nonnull(newest); latest_memory(newest, &job->info, id);
      g_assert_true(job->c->last_buffer == buffer);
      g_assert_cmpuint(GST_BUFFER_PTS(job->c->last_buffer), ==, id * 20 * GST_MSECOND);
    }
    g_mutex_unlock(&job->c->lock); gst_buffer_unref(buffer);
    if (!ok) { job->result = FALSE; break; }
  }
  g_atomic_int_set(&job->finished, TRUE); return NULL;
}
static void test_latest_fifo(void)
{
  if (!x11_enabled()) return;
  LatestOutput output; latest_output_open(&output);
  GstElement *sink = g_object_new(gst_crystalhd_xv_get_type(), NULL);
  gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(sink), output.window);
  g_assert_true(sink_start(GST_BASE_SINK(sink))); XvContext *c = sink_context(XV_SINK(sink));
  GstVideoInfo info = info_for(320, 240); GstCaps *caps = gst_video_info_to_caps(&info);
  g_assert_true(sink_set_caps(GST_BASE_SINK(sink), caps)); gst_caps_unref(caps);
  latest_hold(c); GstBuffer *first[3];
  for (guint id = 0; id < 3; id++) {
    first[id] = latest_source(NULL, &info, id);
    g_assert_cmpint(submit_buffer(c, &info, first[id], NULL), ==, GST_FLOW_OK);
  }
  latest_wait_held(3); LatestFifoProducer job = {.c = c, .info = info};
  GThread *thread = g_thread_new("fifo-producer", latest_fifo_producer, &job);
  while (!g_atomic_int_get(&job.started)) g_usleep(1000);
  g_usleep(20000); g_assert_false(g_atomic_int_get(&job.finished));
  g_mutex_lock(&c->lock); g_assert_false(c->latest_frame);
  g_assert_cmpuint(c->pending, ==, 3); g_assert_cmpuint(c->video_submitted, ==, 3);
  g_assert_null(c->latest_buffer); g_assert_cmpuint(c->latest_queued, ==, 0);
  for (guint i = 0; i < 3; i++) {
    g_assert_nonnull(c->flights[i].memory); latest_memory(c->flights[i].memory, &info, i);
    g_assert_cmpuint(c->flights[i].pts, ==, i * 20 * GST_MSECOND);
  }
  g_mutex_unlock(&c->lock); latest_release(c, 8); g_thread_join(thread); g_assert_true(job.result);
  g_mutex_lock(&c->lock); g_assert_true(drain_locked(c, TRUE));
  g_assert_cmpuint(c->video_submitted, ==, 8); g_assert_cmpuint(c->acknowledged, ==, c->submitted);
  g_assert_cmpuint(c->latest_replaced, ==, 0); latest_ledger(c); g_mutex_unlock(&c->lock);
  latest_unhold(c); sink_stop(GST_BASE_SINK(sink));
  for (guint i = 0; i < 3; i++) gst_buffer_unref(first[i]);
  context_unref(c); gst_object_unref(sink); clean_contexts(); latest_output_close(&output);
  g_test_message("Matched default FIFO: same8 gray source IDs/PTS and3 held ACKs; producer waits, all8 requests/ACKs and source pixels preserved, no latest replacements");
}

static void test_latest_window(void) { latest_coalescing(1, 1); }
static void test_latest_port(void) { latest_coalescing(1, 2); }
static void test_latest_caps(void) { latest_coalescing(1, 3); }

typedef struct {
  LatestOutput output;
  GstElement *pipeline, *source, *sink;
  GstBus *bus;
  XvContext *c;
  GstVideoInfo info;
  guint offered;
  gint returned, seeks;
  guint64 seek_offset;
} LatestPipeline;
static void latest_memory_returned(gpointer data, GstMiniObject *memory)
{
  (void)memory; g_atomic_int_inc((gint *)data);
}
static gboolean latest_seek_data(GstAppSrc *source, guint64 offset, gpointer data)
{
  (void)source; LatestPipeline *p = data; p->seek_offset = offset;
  g_atomic_int_inc(&p->seeks); return TRUE;
}
static void latest_offer(LatestPipeline *p, guint id, GstClockTime pts)
{
  GstBuffer *buffer = latest_source(NULL, &p->info, id); GST_BUFFER_PTS(buffer) = pts;
  gst_mini_object_weak_ref(GST_MINI_OBJECT(gst_buffer_peek_memory(buffer, 0)), latest_memory_returned, &p->returned);
  p->offered++; g_assert_cmpint(gst_app_src_push_buffer(GST_APP_SRC(p->source), buffer), ==, GST_FLOW_OK);
}
static void latest_pipeline_begin(LatestPipeline *p, gboolean seekable)
{
  memset(p, 0, sizeof(*p)); latest_output_open(&p->output);
  GError *error = NULL;
  p->pipeline = gst_parse_launch("appsrc name=input format=time is-live=true ! "
      "crystalhdxvsink name=output latest-frame=true sync=false qos=false "
      "show-preroll-frame=false enable-last-sample=false", &error);
  g_assert_no_error(error); p->source = gst_bin_get_by_name(GST_BIN(p->pipeline), "input");
  p->sink = gst_bin_get_by_name(GST_BIN(p->pipeline), "output"); p->bus = gst_element_get_bus(p->pipeline);
  if (seekable) {
    g_object_set(p->source, "stream-type", GST_APP_STREAM_TYPE_SEEKABLE, NULL);
    g_signal_connect(p->source, "seek-data", G_CALLBACK(latest_seek_data), p);
  }
  p->info = info_for(320, 240); GstCaps *caps = gst_video_info_to_caps(&p->info);
  gst_app_src_set_caps(GST_APP_SRC(p->source), caps); gst_caps_unref(caps);
  gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(p->sink), p->output.window);
  g_assert_cmpint(gst_element_set_state(p->pipeline, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
  p->c = sink_context(XV_SINK(p->sink)); g_assert_nonnull(p->c); latest_hold(p->c);
  for (guint id = 0; id < 8; id++) latest_offer(p, id, id * 20 * GST_MSECOND);
  gint64 end = g_get_monotonic_time() + XV_TIMEOUT_US; gboolean queued = FALSE;
  while (!queued && g_get_monotonic_time() < end) {
    g_mutex_lock(&p->c->lock);
    queued = !p->c->failed && p->c->latest_queued == 5 && p->c->video_submitted == 3 && latest_held() == 3;
    g_mutex_unlock(&p->c->lock); if (!queued) g_usleep(1000);
  }
  g_assert_true(queued); g_mutex_lock(&p->c->lock);
  g_assert_nonnull(p->c->latest_buffer); g_assert_cmpuint(GST_BUFFER_OFFSET(p->c->latest_buffer), ==, 7);
  g_assert_cmpuint(GST_BUFFER_PTS(p->c->latest_buffer), ==, 140 * GST_MSECOND); latest_ledger(p->c);
  g_mutex_unlock(&p->c->lock);
}
static void latest_pipeline_end(LatestPipeline *p)
{
  g_assert_cmpint(gst_element_set_state(p->pipeline, GST_STATE_NULL), !=, GST_STATE_CHANGE_FAILURE);
  latest_unhold(p->c); context_unref(p->c);
  gst_object_unref(p->source); gst_object_unref(p->sink); gst_object_unref(p->bus); gst_object_unref(p->pipeline);
  clean_contexts(); gint64 end = g_get_monotonic_time() + XV_TIMEOUT_US;
  while ((guint)g_atomic_int_get(&p->returned) != p->offered && g_get_monotonic_time() < end) g_usleep(1000);
  g_assert_cmpuint(g_atomic_int_get(&p->returned), ==, p->offered); latest_output_close(&p->output);
  g_test_message("Latest public pipeline: all%u original source memory objects returned", p->offered);
}
static void latest_wait_eos_drain(XvContext *c)
{
  gint64 end = g_get_monotonic_time() + XV_TIMEOUT_US; gboolean waiting = FALSE;
  while (!waiting && g_get_monotonic_time() < end) {
    g_mutex_lock(&c->lock); waiting = !c->failed && c->redraw_suspended >= 2;
    g_mutex_unlock(&c->lock); if (!waiting) g_usleep(1000);
  }
  g_assert_true(waiting);
}
static void test_latest_eos(void)
{
  if (!x11_enabled()) return;
  LatestPipeline p; latest_pipeline_begin(&p, FALSE);
  g_assert_cmpint(gst_app_src_end_of_stream(GST_APP_SRC(p.source)), ==, GST_FLOW_OK); latest_wait_eos_drain(p.c);
  GstMessage *message = gst_bus_timed_pop_filtered(p.bus, 20 * GST_MSECOND, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
  g_assert_null(message);
  /* Each completion is healthy under1s, but old+queued stages exceed1s together. */
  g_usleep(600000); latest_release(p.c, 3); latest_wait_final_ack(p.c);
  message = gst_bus_timed_pop_filtered(p.bus, 20 * GST_MSECOND, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
  g_assert_null(message); g_mutex_lock(&p.c->lock);
  g_assert_cmpuint(GST_BUFFER_OFFSET(p.c->last_buffer), ==, 7);
  g_assert_cmpuint(GST_BUFFER_PTS(p.c->last_buffer), ==, 140 * GST_MSECOND);
  XvFlight *last = NULL;
  for (guint i = 0; i < XV_DEPTH; i++) if (p.c->flights[i].memory) last = &p.c->flights[i];
  g_assert_nonnull(last); latest_memory(last->memory, &p.info, 7); g_mutex_unlock(&p.c->lock);
  g_usleep(600000); latest_release(p.c, 1); await_message(p.pipeline, GST_MESSAGE_EOS);
  g_mutex_lock(&p.c->lock); g_assert_cmpuint(p.c->pending, ==, 0); g_assert_null(p.c->latest_buffer);
  g_assert_cmpuint(p.c->video_submitted, ==, 4); g_assert_cmpuint(p.c->latest_replaced, ==, 4);
  g_assert_cmpuint(p.c->acknowledged, ==, p.c->submitted); latest_ledger(p.c); g_mutex_unlock(&p.c->lock);
  latest_pipeline_end(&p); g_test_message("Actual EOS waits for pending latest and its real matching final ACK");
}
typedef struct { GstElement *pipeline; gint finished; gboolean result; } LatestSeek;
static gpointer latest_do_seek(gpointer data)
{
  LatestSeek *job = data; job->result = gst_element_seek_simple(job->pipeline, GST_FORMAT_TIME,
      GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT, 400 * GST_MSECOND);
  g_atomic_int_set(&job->finished, TRUE); return NULL;
}
static void test_latest_flush_seek(void)
{
  if (!x11_enabled()) return;
  LatestPipeline p; latest_pipeline_begin(&p, TRUE);
  g_assert_cmpint(gst_element_set_state(p.pipeline, GST_STATE_PAUSED), !=, GST_STATE_CHANGE_FAILURE);
  GstState state; g_assert_cmpint(gst_element_get_state(p.pipeline, &state, NULL, 50 * GST_MSECOND), !=, GST_STATE_CHANGE_FAILURE);
  g_assert_cmpint(state, ==, GST_STATE_PAUSED);
  g_mutex_lock(&p.c->lock); g_assert_nonnull(p.c->latest_buffer);
  g_assert_cmpuint(GST_BUFFER_OFFSET(p.c->latest_buffer), ==, 7); g_assert_cmpuint(p.c->latest_cancelled, ==, 0);
  g_mutex_unlock(&p.c->lock);
  LatestSeek job = {.pipeline = p.pipeline}; GThread *thread = g_thread_new("latest-flush-seek", latest_do_seek, &job);
  gint64 end = g_get_monotonic_time() + XV_TIMEOUT_US; gboolean cancelled = FALSE;
  while (!cancelled && g_get_monotonic_time() < end) {
    g_mutex_lock(&p.c->lock); cancelled = !p.c->failed && p.c->latest_cancelled == 1 && !p.c->latest_buffer;
    g_mutex_unlock(&p.c->lock); if (!cancelled) g_usleep(1000);
  }
  g_assert_true(cancelled); g_mutex_lock(&p.c->lock);
  g_assert_cmpuint(p.c->pending, ==, 3); g_assert_cmpuint(p.c->video_submitted, ==, 3);
  for (guint i = 0; i < 3; i++) { g_assert_true(p.c->flights[i].memory->server_owned); latest_memory(p.c->flights[i].memory, &p.info, i); }
  latest_ledger(p.c); g_mutex_unlock(&p.c->lock); latest_release(p.c, 3);
  g_thread_join(thread); g_assert_true(job.result); g_assert_cmpint(g_atomic_int_get(&p.seeks), >, 0);
  g_assert_cmpuint(p.seek_offset, ==, 400 * GST_MSECOND); g_mutex_lock(&p.c->lock);
  end = g_get_monotonic_time() + XV_TIMEOUT_US;
  while (p.c->pending && !p.c->failed && g_get_monotonic_time() < end) g_cond_wait_until(&p.c->changed, &p.c->lock, end);
  g_assert_cmpuint(p.c->pending, ==, 0); g_assert_null(p.c->last_buffer); g_mutex_unlock(&p.c->lock); latest_unhold(p.c);
  g_assert_cmpint(gst_element_set_state(p.pipeline, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
  latest_offer(&p, 8, 400 * GST_MSECOND);
  g_assert_cmpint(gst_app_src_end_of_stream(GST_APP_SRC(p.source)), ==, GST_FLOW_OK); await_message(p.pipeline, GST_MESSAGE_EOS);
  g_mutex_lock(&p.c->lock); g_assert_cmpuint(p.c->video_submitted, ==, 4);
  g_assert_cmpuint(p.c->latest_submitted, ==, 0); g_assert_cmpuint(p.c->latest_cancelled, ==, 1);
  g_assert_cmpuint(GST_BUFFER_OFFSET(p.c->last_buffer), ==, 8);
  g_assert_cmpuint(GST_BUFFER_PTS(p.c->last_buffer), ==, 400 * GST_MSECOND); latest_ledger(p.c);
  g_mutex_unlock(&p.c->lock); latest_pipeline_end(&p);
  g_test_message("Actual pause preserves latest; flushing seek cancels it without reusing3 server-owned slots; original fresh ID8/PTS400ms reaches EOS");
}
static void test_latest_submit_error(void)
{
  if (!x11_enabled()) return;
  LatestPipeline p; latest_pipeline_begin(&p, FALSE);
  g_assert_cmpint(gst_app_src_end_of_stream(GST_APP_SRC(p.source)), ==, GST_FLOW_OK); latest_wait_eos_drain(p.c);
  g_atomic_pointer_set(&fail_flush_connection, p.c->connection); latest_release(p.c, 3);
  await_message(p.pipeline, GST_MESSAGE_ERROR);
  gint64 end = g_get_monotonic_time() + 2 * XV_TIMEOUT_US;
  g_mutex_lock(&p.c->lock);
  while (!p.c->closed && g_get_monotonic_time() < end) g_cond_wait_until(&p.c->changed, &p.c->lock, end);
  g_assert_true(p.c->failed); g_assert_true(p.c->closed); g_assert_false(p.c->quarantined);
  g_assert_cmpstr(p.c->failure, ==, "Xv image submission flush failed");
  g_assert_cmpuint(p.c->pending, ==, 0); g_assert_cmpuint(p.c->terminal, ==, 1);
  g_assert_cmpuint(p.c->acknowledged, ==, 3); g_assert_cmpuint(p.c->latest_submitted, ==, 1);
  g_assert_null(p.c->latest_buffer); latest_ledger(p.c); g_mutex_unlock(&p.c->lock);
  g_assert_null(g_atomic_pointer_get(&fail_flush_connection));
  GstMessage *eos = gst_bus_timed_pop_filtered(p.bus, 20 * GST_MSECOND, GST_MESSAGE_EOS); g_assert_null(eos);
  latest_pipeline_end(&p); g_test_message("Controlled queued submission flush failure: ERROR, no successful EOS, positive detach before terminal retirement");
}
static void test_latest_stop_pool(void)
{
  if (!x11_enabled()) return;
  LatestOutput output; latest_output_open(&output);
  GstElement *sink = g_object_new(gst_crystalhd_xv_get_type(), "latest-frame", TRUE, NULL);
  gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(sink), output.window);
  g_assert_true(sink_start(GST_BASE_SINK(sink))); XvContext *c = sink_context(XV_SINK(sink));
  GstVideoInfo info = info_for(320, 240); GstCaps *caps = gst_video_info_to_caps(&info);
  g_assert_true(sink_set_caps(GST_BASE_SINK(sink), caps)); gst_caps_unref(caps);
  GstBufferPool *pool = make_pool(sink, &info); GstBuffer *buffers[4]; latest_hold(c);
  for (guint i = 0; i < 4; i++) {
    buffers[i] = latest_source(pool, &info, i);
    g_assert_cmpint(submit_buffer(c, &info, buffers[i], buffer_memory(buffers[i])), ==, GST_FLOW_OK);
  }
  latest_wait_held(3); g_assert_true(sink_unlock(GST_BASE_SINK(sink)));
  g_mutex_lock(&c->lock); g_assert_true(c->latest_buffer == buffers[3]); g_assert_cmpuint(c->latest_cancelled, ==, 0); g_mutex_unlock(&c->lock);
  g_assert_true(sink_stop(GST_BASE_SINK(sink))); latest_unhold(c); g_mutex_lock(&c->lock);
  g_assert_true(c->closed); g_assert_false(c->failed); g_assert_false(c->quarantined);
  g_assert_cmpuint(c->pending, ==, 0); g_assert_cmpuint(c->terminal, ==, 3);
  g_assert_cmpuint(c->latest_cancelled, ==, 1); g_assert_cmpuint(c->latest_submitted, ==, 0);
  g_assert_null(c->last_buffer); g_assert_null(c->latest_buffer); latest_ledger(c); g_mutex_unlock(&c->lock);
  for (guint i = 0; i < 4; i++) gst_buffer_unref(buffers[i]);
  g_assert_true(gst_buffer_pool_set_active(pool, FALSE)); gst_object_unref(pool);
  context_unref(c); gst_object_unref(sink); clean_contexts(); latest_output_close(&output);
  g_test_message("Stop after temporary unlock retires3 pooled flights +retained picture +pending latest refs outside lock; all6 pool images released");
}
static void test_latest_missing_ack(void)
{
  if (!x11_enabled()) return;
  LatestPipeline p; latest_pipeline_begin(&p, FALSE);
  g_assert_cmpint(gst_app_src_end_of_stream(GST_APP_SRC(p.source)), ==, GST_FLOW_OK); latest_wait_eos_drain(p.c);
  /* Keep the old ACKs held: the original per-flight deadline must still fail,
   * rather than accepting EOS under the new two-stage aggregate budget. */
  await_message(p.pipeline, GST_MESSAGE_ERROR);
  gint64 end = g_get_monotonic_time() + 2 * XV_TIMEOUT_US; g_mutex_lock(&p.c->lock);
  while (!p.c->closed && g_get_monotonic_time() < end) g_cond_wait_until(&p.c->changed, &p.c->lock, end);
  g_assert_true(p.c->failed); g_assert_true(p.c->closed); g_assert_false(p.c->quarantined);
  g_assert_cmpstr(p.c->failure, ==, "Missing shared image completion");
  g_assert_cmpuint(p.c->pending, ==, 0); g_assert_cmpuint(p.c->terminal, ==, 3);
  g_assert_cmpuint(p.c->acknowledged, ==, 0); g_assert_cmpuint(p.c->latest_submitted, ==, 0);
  g_assert_cmpuint(p.c->latest_cancelled, ==, 1); g_assert_null(p.c->latest_buffer);
  latest_ledger(p.c); g_mutex_unlock(&p.c->lock);
  GstMessage *eos = gst_bus_timed_pop_filtered(p.bus, 20 * GST_MSECOND, GST_MESSAGE_EOS); g_assert_null(eos);
  latest_pipeline_end(&p); g_test_message("Missing old ACKs still fail the original per-flight deadline; unsent latest cancelled, positive detach, no successful EOS");
}
