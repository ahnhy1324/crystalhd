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

static void test_caps_borders_default(void)
{
  if (!x11_enabled()) return;
  LatestOutput output; latest_output_open(&output);
  GstElement *sink = g_object_new(gst_crystalhd_xv_get_type(), NULL);
  gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(sink), output.window);
  g_assert_true(sink_start(GST_BASE_SINK(sink))); XvContext *c = sink_context(XV_SINK(sink));
  g_mutex_lock(&c->lock); c->redraw_suspended++; g_assert_false(c->latest_frame); g_mutex_unlock(&c->lock);
  guint8 previous[3], current[3];
  for (guint step = 0; step < 2; step++) {
    GstVideoInfo info = info_for(step ? 640 : 320, step ? 360 : 240);
    GstCaps *caps = gst_video_info_to_caps(&info);
    g_assert_true(sink_set_caps(GST_BASE_SINK(sink), caps)); gst_caps_unref(caps);
    GstBuffer *buffer = latest_source(NULL, &info, 7 + step);
    g_assert_cmpint(submit_buffer(c, &info, buffer, NULL), ==, GST_FLOW_OK);
    g_mutex_lock(&c->lock); g_assert_true(drain_locked(c, TRUE));
    latest_rgb(c, step ? current : previous);
    if (step) window_pixel_locked(c, c->width / 2, 1, FALSE);
    g_mutex_unlock(&c->lock); gst_buffer_unref(buffer);
  }
  g_assert_cmpint(memcmp(previous, current, 3), !=, 0);
  sink_stop(GST_BASE_SINK(sink)); context_unref(c); gst_object_unref(sink); clean_contexts(); latest_output_close(&output);
  g_test_message("Default FIFO caps4:3->16:9: new source pixels and black bars replace all old covered regions");
}
