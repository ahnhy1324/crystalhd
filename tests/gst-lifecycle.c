/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Exercise the real GstVideoDecoder subclass with an in-process fake library.
 * No CrystalHD library is linked, and no hardware device can be opened.
 */
#include <gst/check/gstharness.h>

static gint64 lifecycle_time(void);
static void lifecycle_sleep(gulong usecs);
static GThread *lifecycle_thread_new(const gchar *name, GThreadFunc function,
                                    gpointer data);
static gpointer lifecycle_thread_join(GThread *thread);
static void lifecycle_stream_unlock(GRecMutex *mutex);
#define g_get_monotonic_time lifecycle_time
#define g_usleep lifecycle_sleep
#define g_thread_new lifecycle_thread_new
#define g_thread_join lifecycle_thread_join
#define g_rec_mutex_unlock lifecycle_stream_unlock
#include "../filters/gst/gst-plugin-1.0/gstcrystalhd.c"
#undef g_get_monotonic_time
#undef g_usleep
#undef g_thread_new
#undef g_thread_join
#undef g_rec_mutex_unlock

static gint64 lifecycle_now;
static GstHarness *flush_during_wait;
static GstHarness *seek_during_wait;
static GstHarness *flush_during_quiesce;
static GstCrystalHdDec *iteration_decoder;
static gboolean real_workers;
static guint8 fake_thread;
G_LOCK_DEFINE_STATIC(mock_queue);
static GstBuffer *new_input(GstClockTime pts);
static gboolean change_h264_caps(GstHarness *harness);
static void lifecycle_teardown(GstHarness *harness);
static gboolean lifecycle_flush(GstVideoDecoder *decoder);

typedef struct {
  guint64 timestamp;
  guint8 pixel;
  guint flags;
  guint32 pulldown;
  guint32 picture_number;
  guint width;
  guint height;
  gboolean whole_weave;
  guint weave_fault;
} MockPicture;

typedef enum {
  MOCK_DEVICE_OPEN = 1, MOCK_VERSION, MOCK_FORMAT, MOCK_SCALE,
  MOCK_DECODER_OPEN, MOCK_COLOR, MOCK_START, MOCK_CAPTURE, MOCK_STOP,
  MOCK_DECODER_CLOSE, MOCK_DEVICE_CLOSE
} MockOperation;

static struct {
  GQueue pictures;
  gboolean device_active;
  gboolean opened;
  gboolean started;
  gboolean auto_output;
  gboolean busy_stale;
  gboolean fail_input;
  gboolean fail_status;
  gboolean fail_drain;
  gboolean early_eos;
  gboolean publish_after_eos;
  guint input_calls;
  guint status_calls;
  guint status_no_ready_peek_calls;
  guint status_unflagged_calls;
  guint status_invalid_requests;
  guint flush_calls;
  guint drain_calls;
  guint release_calls;
  guint destroyed_inputs;
  guint free_bytes;
  guint required_bytes;
  guint unsafe_submissions;
  guint open_calls;
  guint successful_opens;
  guint close_calls;
  guint decoder_open_calls;
  guint decoder_close_calls;
  guint start_calls;
  guint stop_calls;
  guint format_calls;
  guint scale_calls;
  guint device_version;
  guint fail_operations;
  guint operation_serial;
  guint format_serial;
  guint scale_serial;
  guint decoder_open_serial;
  BC_INPUT_FORMAT last_format;
  BC_SCALING_PARAMS last_scale;
  guint8 last_metadata[64];
  gboolean release_makes_room;
  gboolean publish_on_wait;
  guint32 free_bytes_on_sleep;
  guint64 recovery_generation;
  BC_STATUS recovery_status;
  gboolean recovery_active;
  guint32 recovery_replay_bytes;
  gboolean recovery_after_input;
  gboolean recovery_after_input_complete;
  BC_STATUS recovery_after_input_status;
  gboolean recovery_replay_armed;
  guint recovery_replay_consumptions;
  BC_STATUS input_once;
  BC_STATUS input_persistent;
  guint32 free_bytes_after_input_once;
  gboolean complete_recovery_on_sleep;
  gboolean recovery_query_advance_timeout;
  guint recovery_query_calls;
  BC_STATUS recovery_query_once;
  BC_STATUS driver_status_once;
  BC_STATUS output_once;
  BC_STATUS release_once;
  BC_STATUS eos_once;
  gboolean driver_status_latches_recovery;
  gboolean output_latches_recovery;
  gboolean release_latches_recovery;
  gboolean eos_latches_recovery;
  BC_STATUS stop_transient_status;
  guint stop_transient_count;
  BC_STATUS close_transient_status;
  guint close_transient_count;
  BC_STATUS device_close_transient_status;
  guint device_close_transient_count;
  gboolean device_close_advance_timeout;
  guint sleep_calls;
  guint sleep_limit;
  guint64 accepted[64];
  guint accepted_count;
  guint8 pixels[16 * 16 * 2];
  guint8 scaled_pixels[128 * 72 * 2];
  guint8 whole_pixels[16 * 16 * 2 + 160];
  guint8 legacy_pixels[720 * 16 * 2];
} mock;

static BC_STATUS
operation_status(MockOperation operation)
{
  return (mock.fail_operations & (1U << operation)) ? BC_STS_ERROR : BC_STS_SUCCESS;
}

static void
mock_latch_recovery(void)
{
  mock.recovery_generation++;
  mock.recovery_active = TRUE;
}

static void
lifecycle_stream_unlock(GRecMutex *mutex)
{
  g_rec_mutex_unlock(mutex);
  if (flush_during_quiesce != NULL) {
    GstHarness *harness = flush_during_quiesce;
    GstCrystalHdDec *self = GST_CRYSTALHD_DEC(harness->element);
    if (mutex == &GST_VIDEO_DECODER(self)->stream_lock && self->output_paused) {
      /* Precisely the reopen helper's unlock AFTER draining, before it
       * re-acquires the core lock and commits destructive device changes. */
      flush_during_quiesce = NULL;
      g_assert_true(gst_harness_push_event(harness, gst_event_new_flush_start()));
    }
  }
}

static gint64
lifecycle_time(void)
{
  return real_workers ? g_get_monotonic_time() : lifecycle_now;
}

static void
lifecycle_sleep(gulong usecs)
{
  if (real_workers) {
    g_usleep(usecs);
    return;
  }
  mock.sleep_calls++;
  if (mock.sleep_limit != 0)
    g_assert_cmpuint(mock.sleep_calls, <=, mock.sleep_limit);
  lifecycle_now += usecs;
  if (mock.free_bytes_on_sleep != 0) {
    mock.free_bytes = mock.free_bytes_on_sleep;
    mock.free_bytes_on_sleep = 0;
  }
  if (mock.complete_recovery_on_sleep && mock.recovery_active) {
    mock.complete_recovery_on_sleep = FALSE;
    mock.recovery_active = FALSE;
  }
  if (flush_during_wait != NULL) {
    GstHarness *harness = flush_during_wait;
    flush_during_wait = NULL;
    g_assert_true(gst_harness_push_event(harness, gst_event_new_flush_start()));
  }
  if (seek_during_wait != NULL) {
    GstHarness *harness = seek_during_wait;
    GstSegment segment;
    seek_during_wait = NULL;
    g_assert_true(gst_harness_push_event(harness, gst_event_new_flush_start()));
    g_assert_true(gst_harness_push_event(harness, gst_event_new_flush_stop(TRUE)));
    gst_segment_init(&segment, GST_FORMAT_TIME);
    g_assert_true(gst_harness_push_event(harness, gst_event_new_segment(&segment)));
    mock.free_bytes = 1024 * 1024;
    mock.auto_output = FALSE;
    g_assert_cmpint(gst_harness_push(harness, new_input(0)), ==, GST_FLOW_OK);
    g_assert_cmpint(gst_harness_push(harness, new_input(40 * GST_MSECOND)),
                    ==, GST_FLOW_OK);
  }
  if (iteration_decoder != NULL)
    gst_crystalhd_output_iteration(iteration_decoder);
}

static GThread *
lifecycle_thread_new(const gchar *name, GThreadFunc function, gpointer data)
{
  if (real_workers)
    return g_thread_new(name, function, data);
  return (GThread *)&fake_thread;
}

static gpointer
lifecycle_thread_join(GThread *thread)
{
  if (thread != (GThread *)&fake_thread)
    return g_thread_join(thread);
  return NULL;
}

static void
queue_picture_geometry(guint64 timestamp, guint8 pixel,
                       guint32 picture_number, guint width, guint height,
                       guint flags)
{
  MockPicture *picture = g_new0(MockPicture, 1);
  picture->timestamp = timestamp;
  picture->pixel = pixel;
  picture->picture_number = picture_number;
  picture->width = width;
  picture->height = height;
  picture->flags = flags;
  G_LOCK(mock_queue);
  g_queue_push_tail(&mock.pictures, picture);
  G_UNLOCK(mock_queue);
}

static void
queue_numbered_picture(guint64 timestamp, guint8 pixel,
                       guint32 picture_number)
{
  queue_picture_geometry(timestamp, pixel, picture_number, 16, 16, 0);
}

static void
queue_picture(guint64 timestamp, guint8 pixel)
{
  queue_numbered_picture(timestamp, pixel, 0);
}

BC_STATUS DtsDeviceOpen(HANDLE *device, uint32_t mode)
{
  (void)mode;
  mock.open_calls++;
  g_assert_false(mock.device_active);
  *device = NULL;
  if (operation_status(MOCK_DEVICE_OPEN) != BC_STS_SUCCESS)
    return BC_STS_ERROR;
  mock.successful_opens++;
  mock.device_active = TRUE;
  *device = &mock;
  return BC_STS_SUCCESS;
}

BC_STATUS DtsDeviceClose(HANDLE device)
{
  BC_STATUS status;
  g_assert_true(device == &mock);
  g_assert_true(mock.device_active);
  mock.close_calls++;
  if (mock.device_close_transient_count != 0) {
    mock.device_close_transient_count--;
    status = mock.device_close_transient_status;
    if (status == BC_STS_BUSY) {
      if (mock.device_close_advance_timeout)
        lifecycle_now += GST_CRYSTALHD_DRAIN_TIMEOUT_US;
      return status;
    }
  } else {
    status = operation_status(MOCK_DEVICE_CLOSE);
  }
  mock.device_active = FALSE;
  G_LOCK(mock_queue);
  g_queue_clear_full(&mock.pictures, g_free);
  G_UNLOCK(mock_queue);
  return status;
}

BC_STATUS DtsCrystalHDVersion(HANDLE device, PBC_INFO_CRYSTAL version)
{
  (void)device;
  version->device = mock.device_version;
  return operation_status(MOCK_VERSION);
}

BC_STATUS DtsSetInputFormat(HANDLE device, BC_INPUT_FORMAT *format)
{
  (void)device;
  mock.format_calls++;
  mock.format_serial = ++mock.operation_serial;
  mock.last_format = *format;
  g_assert_cmpuint(format->metaDataSz, <=, sizeof(mock.last_metadata));
  if (format->metaDataSz != 0)
    memcpy(mock.last_metadata, format->pMetaData, format->metaDataSz);
  mock.last_format.pMetaData = mock.last_metadata;
  return operation_status(MOCK_FORMAT);
}

BC_STATUS DtsSetScaleParams(HANDLE device, BC_SCALING_PARAMS *scale)
{
  (void)device;
  mock.scale_calls++;
  mock.scale_serial = ++mock.operation_serial;
  mock.last_scale = *scale;
  return operation_status(MOCK_SCALE);
}

BC_STATUS DtsOpenDecoder(HANDLE device, uint32_t stream_type)
{
  (void)device;
  (void)stream_type;
  mock.decoder_open_calls++;
  mock.decoder_open_serial = ++mock.operation_serial;
  if (operation_status(MOCK_DECODER_OPEN) != BC_STS_SUCCESS)
    return BC_STS_ERROR;
  mock.opened = TRUE;
  return BC_STS_SUCCESS;
}

BC_STATUS DtsCloseDecoder(HANDLE device)
{
  BC_STATUS status;
  (void)device;
  mock.decoder_close_calls++;
  if (mock.close_transient_count != 0) {
    mock.close_transient_count--;
    return mock.close_transient_status;
  }
  status = operation_status(MOCK_DECODER_CLOSE);
  if (status == BC_STS_SUCCESS)
    mock.opened = FALSE;
  return status;
}

BC_STATUS DtsStartDecoder(HANDLE device)
{
  (void)device;
  mock.start_calls++;
  if (operation_status(MOCK_START) != BC_STS_SUCCESS)
    return BC_STS_ERROR;
  mock.started = TRUE;
  return BC_STS_SUCCESS;
}

BC_STATUS DtsStopDecoder(HANDLE device)
{
  BC_STATUS status;
  (void)device;
  mock.stop_calls++;
  if (mock.stop_transient_count != 0) {
    mock.stop_transient_count--;
    return mock.stop_transient_status;
  }
  status = operation_status(MOCK_STOP);
  if (status == BC_STS_SUCCESS)
    mock.started = FALSE;
  return status;
}

BC_STATUS DtsStartCapture(HANDLE device)
{
  (void)device;
  return operation_status(MOCK_CAPTURE);
}

BC_STATUS DtsSetColorSpace(HANDLE device, BC_OUTPUT_FORMAT format)
{
  (void)device;
  (void)format;
  return operation_status(MOCK_COLOR);
}

BC_STATUS DtsProcInput(HANDLE device, uint8_t *data, uint32_t size,
                      uint64_t timestamp, BOOL encrypted)
{
  (void)device;
  (void)data;
  (void)size;
  (void)encrypted;
  mock.input_calls++;
  if (mock.free_bytes < mock.required_bytes) {
    /* The real DtsSendData blocks here. Fail deterministically instead, so
     * a regression cannot hang the test process or write part of a picture.
     */
    mock.unsafe_submissions++;
    return BC_STS_ERROR;
  }
  g_assert_true(mock.device_active);
  g_assert_true(mock.opened);
  g_assert_true(mock.started);
  if (mock.fail_input)
    return BC_STS_ERROR;
  if (mock.input_once != BC_STS_SUCCESS) {
    BC_STATUS result = mock.input_once;
    mock.input_once = BC_STS_SUCCESS;
    if (mock.free_bytes_after_input_once != 0) {
      mock.free_bytes = mock.free_bytes_after_input_once;
      mock.free_bytes_after_input_once = 0;
    }
    return result;
  }
  if (mock.input_persistent != BC_STS_SUCCESS)
    return mock.input_persistent;
  if (mock.busy_stale) {
    mock.busy_stale = FALSE;
    queue_picture(timestamp + 123, 0xee);
    return BC_STS_BUSY;
  }
  if (mock.recovery_after_input) {
    BC_STATUS result = mock.recovery_after_input_status;
    mock.recovery_after_input = FALSE;
    if (result == BC_STS_SUCCESS) {
      g_assert_cmpuint(mock.accepted_count, <, G_N_ELEMENTS(mock.accepted));
      mock.accepted[mock.accepted_count++] = timestamp;
      if (mock.recovery_replay_armed) {
        mock.recovery_replay_armed = FALSE;
        mock.recovery_replay_consumptions++;
      }
    }
    mock.recovery_generation++;
    mock.recovery_active = !mock.recovery_after_input_complete;
    mock.recovery_after_input_complete = FALSE;
    return result;
  }
  g_assert_cmpuint(mock.accepted_count, <, G_N_ELEMENTS(mock.accepted));
  mock.accepted[mock.accepted_count++] = timestamp;
  if (mock.auto_output)
    queue_picture(timestamp, mock.accepted_count);
  return BC_STS_SUCCESS;
}

BC_STATUS DtsGetDriverStatus(HANDLE device, BC_DTS_STATUS *status)
{
  guint32 request;
  (void)device;
  request = status->cpbEmptySize;
  mock.status_calls++;
  if (request == CRYSTALHD_STATUS_SKIP_READY_HEAD_PEEK)
    mock.status_no_ready_peek_calls++;
  else if (request == 0)
    mock.status_unflagged_calls++;
  else
    mock.status_invalid_requests++;
  if (mock.driver_status_once != BC_STS_SUCCESS) {
    BC_STATUS result = mock.driver_status_once;
    mock.driver_status_once = BC_STS_SUCCESS;
    if (mock.driver_status_latches_recovery) {
      mock.driver_status_latches_recovery = FALSE;
      mock_latch_recovery();
    }
    return result;
  }
  if (mock.fail_status)
    return BC_STS_ERROR;
  memset(status, 0, sizeof(*status));
  G_LOCK(mock_queue);
  status->ReadyListCount = mock.pictures.length;
  G_UNLOCK(mock_queue);
  return BC_STS_SUCCESS;
}

BC_STATUS DtsGetRecoveryStatus(HANDLE device, uint64_t *generation,
                               BC_STATUS *status, uint8_t *in_progress,
                               uint32_t *replay_bytes)
{
  g_assert_true(device == &mock);
  mock.recovery_query_calls++;
  if (mock.recovery_query_once != BC_STS_SUCCESS) {
    BC_STATUS result = mock.recovery_query_once;
    mock.recovery_query_once = BC_STS_SUCCESS;
    return result;
  }
  *generation = mock.recovery_generation;
  *status = mock.recovery_status;
  *in_progress = mock.recovery_active;
  *replay_bytes = mock.recovery_replay_bytes;
  if (mock.recovery_active && mock.recovery_query_advance_timeout) {
    mock.recovery_query_advance_timeout = FALSE;
    lifecycle_now += GST_CRYSTALHD_DRAIN_TIMEOUT_US;
  }
  return BC_STS_SUCCESS;
}

uint32_t DtsTxFreeSize(HANDLE device)
{
  (void)device;
  if (mock.publish_on_wait && mock.free_bytes == 0) {
    mock.publish_on_wait = FALSE;
    queue_picture(mock.accepted[0], 1);
  }
  return mock.free_bytes;
}

BC_STATUS DtsProcOutputNoCopy(HANDLE device, uint32_t timeout,
                             BC_DTS_PROC_OUT *output)
{
  MockPicture *picture;
  guint8 *pixels;
  gsize pixel_size;
  gsize completed_size;
  guint field_type;
  (void)device;
  (void)timeout;
  if (mock.output_once != BC_STS_SUCCESS) {
    BC_STATUS result = mock.output_once;
    mock.output_once = BC_STS_SUCCESS;
    if (mock.output_latches_recovery) {
      mock.output_latches_recovery = FALSE;
      mock_latch_recovery();
    }
    return result;
  }
  G_LOCK(mock_queue);
  picture = g_queue_pop_head(&mock.pictures);
  G_UNLOCK(mock_queue);
  if (picture == NULL)
    return BC_STS_NO_DATA;
  field_type = picture->flags & VDEC_FLAG_BOTTOMFIELD;
  completed_size = (gsize)picture->width * picture->height * 2;
  if (!picture->whole_weave &&
      (picture->flags & VDEC_FLAG_INTERLACED_SRC) != 0 &&
      (field_type == VDEC_FLAG_TOPFIELD ||
       field_type == VDEC_FLAG_BOTTOMFIELD)) {
    guint source_width = picture->width;
    if (mock.device_version == 0)
      source_width = source_width <= 720 ? 720 :
                     (source_width <= 1280 ? 1280 : 1920);
    /* Completion counts pixels actually returned, not backing allocation:
     * a real separate-field buffer contains only half the visible rows. */
    completed_size = (gsize)source_width * (picture->height / 2) * 2;
  }
  pixel_size = completed_size;
  if (picture->whole_weave) {
    g_assert_cmpuint(picture->width, ==, 16);
    g_assert_cmpuint(picture->height, ==, 16);
    pixels = mock.whole_pixels;
    pixel_size = completed_size = sizeof(mock.whole_pixels);
  } else if (pixel_size <= sizeof(mock.pixels)) {
    pixels = mock.pixels;
    pixel_size = sizeof(mock.pixels);
  } else if (pixel_size > sizeof(mock.scaled_pixels)) {
    pixels = mock.legacy_pixels;
    pixel_size = MIN(pixel_size, sizeof(mock.legacy_pixels));
  } else {
    pixels = mock.scaled_pixels;
    pixel_size = MIN(pixel_size, sizeof(mock.scaled_pixels));
  }
  memset(pixels, picture->pixel, pixel_size);
  memset(output, 0, sizeof(*output));
  output->PoutFlags = BC_POUT_FLAGS_PIB_VALID;
  output->PicInfo.width = picture->width;
  output->PicInfo.height = picture->height;
  output->PicInfo.timeStamp = picture->timestamp;
  output->PicInfo.picture_number = picture->picture_number;
  output->PicInfo.flags = picture->flags;
  output->PicInfo.pulldown = picture->pulldown;
  output->Ybuff = pixels;
  output->YBuffDoneSz = MIN(completed_size, pixel_size) / 4;
  if (picture->whole_weave) {
    const gsize payload = 16 * 16 * 2;
    BC_PIC_INFO_BLOCK raw = output->PicInfo;
    guint32 ordinal = raw.picture_number;
    guint32 trailer_width = 16;
    guint32 trailer_line = 16;
    guint row;
    output->b422Mode = OUTPUT_MODE422_YUY2;
    for (row = 0; row < 16; ++row)
      memset(pixels + row * 32, picture->pixel + row, 32);
    raw.flags = VDEC_FLAG_FIELDPAIR | VDEC_FLAG_UNKNOWN_SRC |
                (picture->flags & VDEC_FLAG_BOTTOM_FIRST);
    raw.timeStamp = picture->timestamp / 20000U;
    switch (picture->weave_fault) {
      case 0: break;
      case 1: --output->YBuffDoneSz; break;
      case 2: ++trailer_width; break;
      case 3: trailer_line = 8; break;
      case 4: ++raw.timeStamp; break;
      case 5: raw.timeStamp = 0xffffffffULL; break;
      case 6: ++raw.picture_number; break;
      case 7: ++raw.sess_num; break;
      case 8: ++raw.picture_meta_payload; break;
      case 9: raw.flags = VDEC_FLAG_TOPFIELD; break;
      case 10: raw.flags |= VDEC_FLAG_EOS; break;
      case 11: ++raw.width; break;
      case 12: raw.height -= 4; break;
      case 13: ++ordinal; break;
      case 14: output->PoutFlags |= BC_POUT_FLAGS_ENCRYPTED; break;
      case 15: raw.flags |= 0x800U; break;
      case 16: output->Ybuff = NULL; break;
      case 17: output->b422Mode = OUTPUT_MODE422_UYVY; break;
      case 18: output->YBuffDoneSz = payload / 4; break;
      case 19: output->YBuffDoneSz = G_MAXUINT32; break;
      default: g_assert_not_reached();
    }
    memcpy(pixels + payload, &ordinal, sizeof(ordinal));
    memcpy(pixels + payload + sizeof(ordinal), &raw,
           offsetof(BC_PIC_INFO_BLOCK, other));
    memcpy(pixels + pixel_size - 8, &trailer_width, sizeof(trailer_width));
    memcpy(pixels + pixel_size - 4, &trailer_line, sizeof(trailer_line));
  }
  g_free(picture);
  return BC_STS_SUCCESS;
}

BC_STATUS DtsReleaseOutputBuffs(HANDLE device, void *reserved, BOOL change)
{
  BC_STATUS result = mock.release_once;
  (void)device;
  (void)reserved;
  (void)change;
  mock.release_calls++;
  if (mock.release_makes_room)
    mock.free_bytes = 1024 * 1024;
  memset(mock.pixels, 0xcc, sizeof(mock.pixels));
  memset(mock.scaled_pixels, 0xcc, sizeof(mock.scaled_pixels));
  memset(mock.whole_pixels, 0xcc, sizeof(mock.whole_pixels));
  memset(mock.legacy_pixels, 0xcc, sizeof(mock.legacy_pixels));
  mock.release_once = BC_STS_SUCCESS;
  if (result != BC_STS_SUCCESS && mock.release_latches_recovery) {
    mock.release_latches_recovery = FALSE;
    mock_latch_recovery();
  }
  return result;
}

BC_STATUS DtsFlushInput(HANDLE device, uint32_t operation)
{
  (void)device;
  if (!mock.opened)
    return BC_STS_DEC_NOT_OPEN;
  if (operation == 0) {
    if (mock.free_bytes < mock.required_bytes) {
      mock.unsafe_submissions++;
      return BC_STS_ERROR;
    }
    mock.drain_calls++;
    return mock.fail_drain ? BC_STS_ERROR : BC_STS_SUCCESS;
  }
  g_assert_cmpuint(operation, ==, 4);
  mock.flush_calls++;
  mock.opened = FALSE;
  mock.started = FALSE;
  G_LOCK(mock_queue);
  g_queue_clear_full(&mock.pictures, g_free);
  G_UNLOCK(mock_queue);
  return BC_STS_SUCCESS;
}

BC_STATUS DtsIsEndOfStream(HANDLE device, uint8_t *eos)
{
  gboolean publish;
  (void)device;
  if (mock.eos_once != BC_STS_SUCCESS) {
    BC_STATUS result = mock.eos_once;
    mock.eos_once = BC_STS_SUCCESS;
    if (mock.eos_latches_recovery) {
      mock.eos_latches_recovery = FALSE;
      mock_latch_recovery();
    }
    return result;
  }
  G_LOCK(mock_queue);
  *eos = mock.early_eos || g_queue_is_empty(&mock.pictures);
  publish = mock.publish_after_eos;
  mock.publish_after_eos = FALSE;
  G_UNLOCK(mock_queue);
  if (publish) {
    g_assert_cmpuint(mock.accepted_count, >, 0);
    queue_picture(mock.accepted[0], 1);
  }
  return BC_STS_SUCCESS;
}

static GstHarness *
new_decoder_with_caps_device_scale(const gchar *caps, guint device_version,
                                   guint scale_width)
{
  GstElement *element;
  GstHarness *harness;
  memset(&mock, 0, sizeof(mock));
  iteration_decoder = NULL;
  lifecycle_now = 1;
  flush_during_wait = NULL;
  seek_during_wait = NULL;
  flush_during_quiesce = NULL;
  g_queue_init(&mock.pictures);
  mock.auto_output = TRUE;
  mock.free_bytes = 1024 * 1024;
  mock.device_version = device_version;
  element = g_object_new(GST_TYPE_CRYSTALHD_DEC,
                         "scale-width", scale_width, NULL);
  harness = gst_harness_new_with_element(element, "sink", "src");
  gst_object_unref(element); /* the harness takes its own reference */
  gst_harness_set_src_caps_str(harness, caps);
  iteration_decoder = GST_CRYSTALHD_DEC(harness->element);
  return harness;
}

static GstHarness *
new_decoder_with_caps_and_device(const gchar *caps, guint device_version)
{
  return new_decoder_with_caps_device_scale(caps, device_version, 0);
}

static GstHarness *
new_decoder_with_caps(const gchar *caps)
{
  return new_decoder_with_caps_and_device(caps, 1);
}

static GstHarness *
new_decoder(void)
{
  return new_decoder_with_caps(
      "video/x-h264,stream-format=byte-stream,alignment=au,parsed=true,"
      "width=16,height=16,framerate=25/1");
}

static GstHarness *
new_mpeg2_decoder(void)
{
  return new_decoder_with_caps(
      "video/mpeg,mpegversion=2,systemstream=false,parsed=true,"
      "width=16,height=16,framerate=24000/1001");
}

static void
test_scale_caps_query(void)
{
  GstElement *element = g_object_new(GST_TYPE_CRYSTALHD_DEC, NULL);
  GstElement *filter = gst_element_factory_make("capsfilter", NULL);
  GstPad *sink = gst_element_get_static_pad(element, "sink");
  GstPad *src = gst_element_get_static_pad(element, "src");
  GstPad *peer = gst_element_get_static_pad(filter, "sink");
  GstCaps *fhd = gst_caps_from_string(
      "video/x-h264,stream-format=byte-stream,alignment=au,parsed=true,"
      "width=1920,height=1080,framerate=30/1,pixel-aspect-ratio=1/1");
  GstCaps *hd = gst_caps_from_string(
      "video/x-h264,stream-format=byte-stream,alignment=au,parsed=true,"
      "width=1280,height=720,framerate=30/1,pixel-aspect-ratio=1/1");
  GstCaps *output = gst_caps_from_string(
      "video/x-raw,format=YUY2,width=1280,height=720,"
      "framerate=30/1,pixel-aspect-ratio=1/1");
  GstCaps *caps;
  guint opens = mock.open_calls;
  static const gchar *rejected[] = {
    "video/x-raw,format=NV12,width=1280,height=720",
    "video/x-raw,format=YUY2,width=960,height=540",
    "video/x-raw,format=YUY2,width=1280,height=2000",
    "video/x-raw,format=YUY2,width=1280,height=720,interlace-mode=interleaved"
  };

  /* Only queries in NULL: no caps event, firmware open or fake decode. Link
   * without a caps check so incompatible peers can be queried as well. */
  g_object_set(filter, "caps", output, NULL);
  g_assert_cmpint(gst_pad_link_full(src, peer, GST_PAD_LINK_CHECK_NOTHING),
                  ==, GST_PAD_LINK_OK);
  caps = gst_pad_query_caps(sink, fhd);
  g_assert_true(gst_caps_is_empty(caps));
  gst_caps_unref(caps);
  caps = gst_pad_query_caps(sink, hd);
  g_assert_true(gst_caps_is_equal(caps, hd));
  gst_caps_unref(caps);

  g_object_set(element, "scale-width", 1280U, NULL);
  caps = gst_pad_query_caps(sink, fhd);
  g_assert_true(gst_caps_is_equal(caps, fhd));
  gst_caps_unref(caps);
  caps = gst_pad_query_caps(sink, NULL);
  g_assert_true(gst_caps_can_intersect(caps, fhd));
  gst_caps_unref(caps);
  /* Scaling preserves rate, and never permits a coded width below output. */
  gst_caps_set_simple(hd, "framerate", GST_TYPE_FRACTION, 25, 1, NULL);
  caps = gst_pad_query_caps(sink, hd);
  g_assert_true(gst_caps_is_empty(caps));
  gst_caps_unref(caps);
  gst_caps_set_simple(hd, "width", G_TYPE_INT, 640, "height", G_TYPE_INT, 360,
                       "framerate", GST_TYPE_FRACTION, 30, 1, NULL);
  caps = gst_pad_query_caps(sink, hd);
  g_assert_true(gst_caps_is_empty(caps));
  gst_caps_unref(caps);
  for (guint i = 0; i < G_N_ELEMENTS(rejected); ++i) {
    GstCaps *unsupported = gst_caps_from_string(rejected[i]);
    g_object_set(filter, "caps", unsupported, NULL);
    gst_caps_unref(unsupported);
    caps = gst_pad_query_caps(sink, fhd);
    g_assert_true(gst_caps_is_empty(caps));
    gst_caps_unref(caps);
  }

  /* Do not inherit framerate/color constraints from a downstream alternative
   * that cannot accept the requested scaled width, format or scan mode. */
  {
    GstCaps *alternatives = gst_caps_from_string(
        "video/x-raw,format=YUY2,width=1280,height=720,framerate=30/1;"
        "video/x-raw,format=YUY2,width=960,height=540,framerate=25/1;"
        "video/x-raw,format=NV12,width=1280,height=720,framerate=24/1;"
        "video/x-raw,format=YUY2,width=1280,height=720,framerate=20/1,"
        "interlace-mode=interleaved");
    const gint invalid_rates[] = { 25, 24, 20 };

    g_object_set(filter, "caps", alternatives, NULL);
    gst_caps_unref(alternatives);
    caps = gst_pad_query_caps(sink, fhd);
    g_assert_true(gst_caps_is_equal(caps, fhd));
    gst_caps_unref(caps);
    for (guint i = 0; i < G_N_ELEMENTS(invalid_rates); ++i) {
      gst_caps_set_simple(fhd, "framerate", GST_TYPE_FRACTION,
                           invalid_rates[i], 1, NULL);
      caps = gst_pad_query_caps(sink, fhd);
      g_assert_true(gst_caps_is_empty(caps));
      gst_caps_unref(caps);
    }
    gst_caps_set_simple(fhd, "framerate", GST_TYPE_FRACTION, 30, 1, NULL);
  }

  /* Rounded scaled height changes output PAR; that PAR must not leak back
   * into the square-pixel compressed-input query. */
  gst_caps_set_simple(output, "width", G_TYPE_INT, 130, "height", G_TYPE_INT, 74,
                       "pixel-aspect-ratio", GST_TYPE_FRACTION, 592, 585, NULL);
  g_object_set(filter, "caps", output, NULL);
  g_object_set(element, "scale-width", 130U, NULL);
  caps = gst_pad_query_caps(sink, fhd);
  g_assert_true(gst_caps_is_equal(caps, fhd));
  gst_caps_unref(caps);
  g_object_set(element, "scale-width", 129U, NULL);
  caps = gst_pad_query_caps(sink, fhd);
  g_assert_true(gst_caps_is_empty(caps));
  gst_caps_unref(caps);
  g_object_set(element, "scale-width", 0U, NULL);
  caps = gst_pad_query_caps(sink, fhd);
  g_assert_true(gst_caps_is_empty(caps));
  gst_caps_unref(caps);
  g_assert_cmpuint(mock.open_calls, ==, opens);

  gst_pad_unlink(src, peer);
  gst_caps_unref(output);
  gst_caps_unref(hd);
  gst_caps_unref(fhd);
  gst_object_unref(peer);
  gst_object_unref(src);
  gst_object_unref(sink);
  gst_object_unref(filter);
  gst_object_unref(element);
}

static void
test_scale_property_default(void)
{
  const gchar *caps =
      "video/x-h264,stream-format=byte-stream,alignment=au,parsed=true,"
      "width=1920,height=1080,framerate=25/1";
  GstHarness *harness = new_decoder_with_caps_device_scale(caps, 1, 0);
  GParamSpec *spec = g_object_class_find_property(
      G_OBJECT_GET_CLASS(harness->element), "scale-width");
  guint scale_width = G_MAXUINT;

  g_assert_nonnull(spec);
  g_assert_true((spec->flags & GST_PARAM_MUTABLE_READY) != 0);
  g_assert_cmpuint(G_PARAM_SPEC_UINT(spec)->default_value, ==, 0);
  g_object_get(harness->element, "scale-width", &scale_width, NULL);
  g_assert_cmpuint(scale_width, ==, 0);
  g_assert_cmpuint(mock.format_calls, ==, 1);
  g_assert_cmpuint(mock.scale_calls, ==, 0);
  g_assert_cmpuint(mock.decoder_open_calls, ==, 1);
  g_assert_cmpuint(mock.format_serial, <, mock.decoder_open_serial);
  g_assert_cmpint(GST_STATE(harness->element), >, GST_STATE_READY);
  g_object_set(harness->element, "scale-width", 128U, NULL);
  g_object_get(harness->element, "scale-width", &scale_width, NULL);
  g_assert_cmpuint(scale_width, ==, 0);
  lifecycle_teardown(harness);
}

typedef struct {
  GstElement *element;
  GMutex lock;
  GCond cond;
  gboolean done;
} ScalePropertyProbe;

static gpointer
scale_property_probe_thread(gpointer data)
{
  ScalePropertyProbe *probe = data;
  guint scale_width;

  g_object_get(probe->element, "scale-width", &scale_width, NULL);
  g_mutex_lock(&probe->lock);
  probe->done = TRUE;
  g_cond_signal(&probe->cond);
  g_mutex_unlock(&probe->lock);
  return NULL;
}

static void
test_scale_property_does_not_take_state_lock(void)
{
  GstHarness *harness = new_decoder_with_caps_device_scale(
      "video/x-h264,stream-format=byte-stream,alignment=au,parsed=true,"
      "width=1920,height=1080,framerate=25/1", 1, 128);
  ScalePropertyProbe probe = { .element = harness->element };
  GThread *thread;
  gboolean completed;
  gint64 deadline;

  g_mutex_init(&probe.lock);
  g_cond_init(&probe.cond);
  GST_STATE_LOCK(harness->element);
  thread = g_thread_new("scale-property-probe", scale_property_probe_thread,
                        &probe);
  g_mutex_lock(&probe.lock);
  deadline = g_get_monotonic_time() + G_TIME_SPAN_SECOND;
  while (!probe.done &&
         g_cond_wait_until(&probe.cond, &probe.lock, deadline))
    ;
  completed = probe.done;
  g_mutex_unlock(&probe.lock);
  GST_STATE_UNLOCK(harness->element);
  g_thread_join(thread);

  g_assert_true(completed);
  g_cond_clear(&probe.cond);
  g_mutex_clear(&probe.lock);
  lifecycle_teardown(harness);
}

static void
test_scale_valid_widths_and_order(void)
{
  static const guint widths[] = { 128, 854, 1366, 1918 };
  guint i;

  for (i = 0; i < G_N_ELEMENTS(widths); ++i) {
    const gchar *caps = i == 0
        ? "video/x-h264,stream-format=byte-stream,alignment=au,parsed=true,"
          "width=1920,height=1080,framerate=25/1"
        : "video/x-h264,stream-format=byte-stream,alignment=au,parsed=true,"
          "width=1920,height=1080,framerate=25/1,interlace-mode=progressive";
    GstHarness *harness =
        new_decoder_with_caps_device_scale(caps, 1, widths[i]);
    guint configured = 0;

    g_object_get(harness->element, "scale-width", &configured, NULL);
    g_assert_cmpuint(configured, ==, widths[i]);
    g_assert_cmpuint(mock.format_calls, ==, 1);
    g_assert_cmpuint(mock.scale_calls, ==, 1);
    g_assert_cmpuint(mock.last_scale.sWidth, ==, widths[i]);
    g_assert_cmpuint(mock.last_scale.sHeight, ==, 0);
    g_assert_cmpuint(mock.last_scale.DNR, ==, 0);
    g_assert_cmpuint(mock.format_serial, <, mock.scale_serial);
    g_assert_cmpuint(mock.scale_serial, <, mock.decoder_open_serial);
    lifecycle_teardown(harness);
  }
}

static void
test_scale_invalid_requests(void)
{
  static const guint invalid_widths[] = {
    1, 127, 129, 1919, 1920, G_MAXUINT
  };
  const gchar *fhd_caps =
      "video/x-h264,stream-format=byte-stream,alignment=au,parsed=true,"
      "width=1920,height=1080,framerate=25/1";
  guint i;

  for (i = 0; i < G_N_ELEMENTS(invalid_widths); ++i) {
    GstElement *element = g_object_new(GST_TYPE_CRYSTALHD_DEC,
        "scale-width", invalid_widths[i], NULL);
    GstPad *sink = gst_element_get_static_pad(element, "sink");
    GstCaps *input = gst_caps_from_string(fhd_caps);
    guint opens = mock.open_calls;
    GstCaps *allowed = gst_pad_query_caps(sink, input);

    /* Invalid widths now fail discovery, before a harness can link or send
     * set_format. Keep the no-device-open assertion at that earlier boundary. */
    g_assert_true(gst_caps_is_empty(allowed));
    g_assert_false(gst_pad_query_accept_caps(sink, input));
    g_assert_cmpuint(mock.open_calls, ==, opens);
    gst_caps_unref(allowed);
    gst_caps_unref(input);
    gst_object_unref(sink);
    gst_object_unref(element);
  }

  {
    GstHarness *harness = new_decoder_with_caps_device_scale(
        "video/x-h264,stream-format=byte-stream,alignment=au,parsed=true,"
        "width=640,height=360,framerate=25/1", 1, 854);
    g_assert_cmpuint(mock.open_calls, ==, 0);
    g_assert_cmpuint(mock.scale_calls, ==, 0);
    lifecycle_teardown(harness);
  }
  {
    GstHarness *harness = new_decoder_with_caps_device_scale(
        "video/x-h264,stream-format=byte-stream,alignment=au,parsed=true,"
        "width=1920,height=1080,framerate=25/1,interlace-mode=interleaved",
        1, 960);
    g_assert_cmpuint(mock.open_calls, ==, 0);
    g_assert_cmpuint(mock.scale_calls, ==, 0);
    lifecycle_teardown(harness);
  }
  {
    GstHarness *harness = new_decoder_with_caps_device_scale(
        fhd_caps, 0, 960);
    g_assert_cmpuint(mock.open_calls, >, 0);
    g_assert_cmpuint(mock.close_calls, ==, mock.open_calls);
    g_assert_cmpuint(mock.format_calls, ==, 0);
    g_assert_cmpuint(mock.scale_calls, ==, 0);
    g_assert_cmpuint(mock.decoder_open_calls, ==, 0);
    lifecycle_teardown(harness);
  }
}

static void
test_scale_reapplied_on_reopen(void)
{
  GstHarness *harness = new_decoder_with_caps_device_scale(
      "video/x-h264,stream-format=byte-stream,alignment=au,parsed=true,"
      "width=1920,height=1080,framerate=25/1,pixel-aspect-ratio=4/3",
      1, 960);
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(harness->element);
  GstCaps *caps;

  g_assert_cmpuint(mock.scale_calls, ==, 1);
  g_assert_true(gst_crystalhd_configure_output(self, 960, 540, FALSE));
  g_assert_cmpuint(GST_VIDEO_INFO_WIDTH(&self->output_info), ==, 960);
  g_assert_cmpuint(GST_VIDEO_INFO_HEIGHT(&self->output_info), ==, 540);
  g_assert_cmpuint(GST_VIDEO_INFO_PAR_N(&self->output_info), ==, 4);
  g_assert_cmpuint(GST_VIDEO_INFO_PAR_D(&self->output_info), ==, 3);

  g_assert_true(lifecycle_flush(GST_VIDEO_DECODER(self)));
  g_assert_cmpuint(mock.format_calls, ==, 2);
  g_assert_cmpuint(mock.scale_calls, ==, 2);
  g_assert_cmpuint(mock.decoder_open_calls, ==, 2);
  g_assert_cmpuint(mock.format_serial, <, mock.scale_serial);
  g_assert_cmpuint(mock.scale_serial, <, mock.decoder_open_serial);

  caps = gst_caps_from_string(
      "video/x-h264,stream-format=byte-stream,alignment=au,parsed=true,"
      "width=1280,height=720,framerate=30/1,pixel-aspect-ratio=4/3");
  g_assert_true(gst_harness_push_event(harness, gst_event_new_caps(caps)));
  gst_caps_unref(caps);
  g_assert_cmpuint(mock.format_calls, ==, 3);
  g_assert_cmpuint(mock.scale_calls, ==, 3);
  g_assert_cmpuint(mock.last_scale.sWidth, ==, 960);
  g_assert_cmpuint(mock.decoder_open_calls, ==, 3);
  g_assert_cmpuint(mock.format_serial, <, mock.scale_serial);
  g_assert_cmpuint(mock.scale_serial, <, mock.decoder_open_serial);
  lifecycle_teardown(harness);
}

static void
test_scale_library_failure(void)
{
  GstHarness *harness = new_decoder_with_caps_device_scale(
      "video/x-h264,stream-format=byte-stream,alignment=au,parsed=true,"
      "width=1920,height=1080,framerate=25/1", 1, 960);
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(harness->element);

  mock.fail_operations = 1U << MOCK_SCALE;
  g_assert_false(lifecycle_flush(GST_VIDEO_DECODER(self)));
  g_assert_cmpuint(mock.scale_calls, ==, 2);
  g_assert_cmpuint(mock.decoder_open_calls, ==, 1);
  g_assert_null(self->device);
  g_assert_false(self->decoder_open);
  g_assert_false(self->decoder_started);
  g_assert_false(mock.device_active);
  g_assert_cmpuint(mock.successful_opens, ==, mock.close_calls);
  mock.fail_operations = 0;
  lifecycle_teardown(harness);
}

/* Most regressions deliberately schedule the real worker iteration themselves,
 * keeping failure injection and the synthetic ten-second clock deterministic.
 * Separate autonomous-output tests below use actual GLib threads and clocks.
 */
static GstFlowReturn
lifecycle_push(GstHarness *harness, GstBuffer *buffer)
{
  GstFlowReturn flow = gst_harness_push(harness, buffer);
  if (!real_workers && flow == GST_FLOW_OK) {
    while (gst_crystalhd_output_iteration(GST_CRYSTALHD_DEC(harness->element)))
      ;
    flow = GST_CRYSTALHD_DEC(harness->element)->output_flow;
  }
  return flow;
}

static void
lifecycle_teardown(GstHarness *harness)
{
  gst_harness_teardown(harness);
  iteration_decoder = NULL;
}

static gboolean
lifecycle_flush(GstVideoDecoder *decoder)
{
  gboolean result;
  GST_VIDEO_DECODER_STREAM_LOCK(decoder);
  result = gst_crystalhd_flush(decoder);
  GST_VIDEO_DECODER_STREAM_UNLOCK(decoder);
  return result;
}

static gboolean
lifecycle_set_format(GstVideoDecoder *decoder, GstVideoCodecState *state)
{
  gboolean result;
  GST_VIDEO_DECODER_STREAM_LOCK(decoder);
  result = gst_crystalhd_set_format(decoder, state);
  GST_VIDEO_DECODER_STREAM_UNLOCK(decoder);
  return result;
}

static GstFlowReturn
lifecycle_drain(GstVideoDecoder *decoder)
{
  GstFlowReturn result;
  GST_VIDEO_DECODER_STREAM_LOCK(decoder);
  result = gst_crystalhd_drain(decoder);
  GST_VIDEO_DECODER_STREAM_UNLOCK(decoder);
  return result;
}

static GstFlowReturn
lifecycle_receive_available(GstCrystalHdDec *self)
{
  while (gst_crystalhd_output_iteration(self))
    ;
  return self->output_flow;
}

#define gst_harness_push lifecycle_push
#define gst_harness_teardown lifecycle_teardown
#define gst_crystalhd_flush lifecycle_flush
#define gst_crystalhd_set_format lifecycle_set_format
#define gst_crystalhd_drain lifecycle_drain
#define gst_crystalhd_receive_available lifecycle_receive_available

static GstHarness *
new_scaled_output_decoder(void)
{
  return new_decoder_with_caps_device_scale(
      "video/x-h264,stream-format=byte-stream,alignment=au,parsed=true,"
      "width=1920,height=1080,framerate=25/1,pixel-aspect-ratio=4/3",
      1, 128);
}

static void
test_scale_output_contract(void)
{
  GstHarness *harness = new_scaled_output_decoder();
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(harness->element);
  GstBuffer *output;
  guint8 edge[2];

  mock.auto_output = FALSE;
  g_assert_cmpint(gst_harness_push(harness, new_input(0)), ==, GST_FLOW_OK);
  queue_picture_geometry(mock.accepted[0], 0x5a, 0, 128, 72, 0);
  g_assert_cmpint(gst_crystalhd_receive_available(self), ==, GST_FLOW_OK);
  output = gst_harness_try_pull(harness);
  g_assert_nonnull(output);
  g_assert_cmpuint(gst_buffer_get_size(output), ==, 128 * 72 * 2);
  g_assert_cmpuint(gst_buffer_extract(output, 0, edge, 1), ==, 1);
  g_assert_cmpuint(gst_buffer_extract(output, 128 * 72 * 2 - 1,
                                      edge + 1, 1), ==, 1);
  g_assert_cmpuint(edge[0], ==, 0x5a);
  g_assert_cmpuint(edge[1], ==, 0x5a);
  g_assert_cmpuint(GST_VIDEO_INFO_WIDTH(&self->output_info), ==, 128);
  g_assert_cmpuint(GST_VIDEO_INFO_HEIGHT(&self->output_info), ==, 72);
  g_assert_cmpuint(GST_VIDEO_INFO_PAR_N(&self->output_info), ==, 4);
  g_assert_cmpuint(GST_VIDEO_INFO_PAR_D(&self->output_info), ==, 3);
  g_assert_cmpuint(mock.release_calls, ==, 1);
  gst_buffer_unref(output);
  gst_harness_teardown(harness);

  harness = new_scaled_output_decoder();
  self = GST_CRYSTALHD_DEC(harness->element);
  mock.auto_output = FALSE;
  g_assert_cmpint(gst_harness_push(harness, new_input(0)), ==, GST_FLOW_OK);
  queue_picture_geometry(mock.accepted[0], 0x5b, 0, 126, 72, 0);
  g_assert_cmpint(gst_crystalhd_receive_available(self), ==, GST_FLOW_ERROR);
  g_assert_null(gst_harness_try_pull(harness));
  g_assert_cmpuint(mock.release_calls, ==, 1);
  gst_harness_teardown(harness);

  harness = new_scaled_output_decoder();
  self = GST_CRYSTALHD_DEC(harness->element);
  mock.auto_output = FALSE;
  g_assert_cmpint(gst_harness_push(harness, new_input(0)), ==, GST_FLOW_OK);
  queue_picture_geometry(mock.accepted[0], 0x5c, 0, 128, 72,
                         VDEC_FLAG_INTERLACED_SRC | VDEC_FLAG_TOPFIELD);
  g_assert_cmpint(gst_crystalhd_receive_available(self), ==, GST_FLOW_ERROR);
  g_assert_null(gst_harness_try_pull(harness));
  g_assert_cmpuint(mock.release_calls, ==, 1);
  gst_harness_teardown(harness);

  harness = new_scaled_output_decoder();
  self = GST_CRYSTALHD_DEC(harness->element);
  g_assert_false(gst_crystalhd_configure_output(self, 126, 72, FALSE));
  g_assert_false(self->output_configured);
  gst_harness_teardown(harness);

  harness = new_decoder_with_caps_device_scale(
      "video/x-h264,stream-format=byte-stream,alignment=au,parsed=true,"
      "width=1920,height=1080,framerate=25/1,pixel-aspect-ratio=1/1",
      1, 130);
  self = GST_CRYSTALHD_DEC(harness->element);
  g_assert_true(gst_crystalhd_configure_output(self, 130, 74, FALSE));
  g_assert_cmpuint(GST_VIDEO_INFO_PAR_N(&self->output_info), ==, 592);
  g_assert_cmpuint(GST_VIDEO_INFO_PAR_D(&self->output_info), ==, 585);
  gst_harness_teardown(harness);
}

static void
input_destroyed(gpointer bytes)
{
  mock.destroyed_inputs++;
  g_free(bytes);
}

static GstBuffer *
new_input(GstClockTime pts)
{
  guint8 *bytes = g_malloc0(8);
  GstBuffer *buffer;
  bytes[3] = 1;
  bytes[4] = 0x65;
  buffer = gst_buffer_new_wrapped_full(0, bytes, 8, 0, 8,
                                        bytes, input_destroyed);
  GST_BUFFER_PTS(buffer) = pts;
  GST_BUFFER_DURATION(buffer) = 40 * GST_MSECOND;
  return buffer;
}

static GstBuffer *
new_h264_input(GstClockTime pts, const guint8 *bytes, gsize size)
{
  GstBuffer *buffer = gst_buffer_new_allocate(NULL, size, NULL);
  gst_buffer_fill(buffer, 0, bytes, size);
  GST_BUFFER_PTS(buffer) = pts;
  GST_BUFFER_DURATION(buffer) = 40 * GST_MSECOND;
  return buffer;
}

static GstBuffer *
new_mpeg4_vop(GstClockTime pts, guint8 header0, guint8 header1)
{
  guint8 picture[] = { 0, 0, 1, 0xb6, header0, header1 };
  GstBuffer *buffer = gst_buffer_new_allocate(NULL, sizeof(picture), NULL);
  gst_buffer_fill(buffer, 0, picture, sizeof(picture));
  GST_BUFFER_PTS(buffer) = pts;
  GST_BUFFER_DURATION(buffer) = 40 * GST_MSECOND;
  return buffer;
}

static GstBuffer *
new_mpeg2_prediction_picture(guint type, gboolean gop, gboolean closed,
                              guint8 first, guint8 second)
{
  guint8 bytes[] = { 0, 0, 1, 0xb8, 0, 0, 0, 0,
                    0, 0, 1, 0, 0, 0, 0xff, 0xff, 0,
                    0, 0, 1, 0xb5, 0x8f, 0xff, 0xf3, 0, 0,
                    0, 0, 1, 1, 0, 0 };
  const gsize offset = gop ? 0 : 8;
  GstBuffer *buffer;
  bytes[7] = closed ? 0x40 : 0;
  bytes[13] = type << 3;
  bytes[30] = first;
  bytes[31] = second;
  buffer = gst_buffer_new_allocate(NULL, sizeof(bytes) - offset, NULL);
  gst_buffer_fill(buffer, 0, bytes + offset, sizeof(bytes) - offset);
  GST_BUFFER_DURATION(buffer) = 40 * GST_MSECOND;
  return buffer;
}

static GstBuffer *
new_mpeg2_picture(guint type, gboolean gop, gboolean closed)
{
  return new_mpeg2_prediction_picture(type, gop, closed, 0x0b, 0);
}

static GstHarness *
new_mpeg2_admission_decoder(void)
{
  GstHarness *harness = new_mpeg2_decoder();
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(harness->element);
  self->codec.mpeg2_profile = 4;
  self->codec.mpeg2_height = 16;
  self->codec.mpeg2_progressive_sequence = TRUE;
  return harness;
}

static GstBuffer *
new_mpeg2_sliced_picture(guint type, gboolean complete)
{
  guint8 bytes[] = {
    0, 0, 1, 0xb3, 1, 0, 0x20, 0x33,
    0, 0, 1, 0xb5, 0x14, 0x4a, 0, 1, 0, 0,
    0, 0, 1, 0, 0, 0, 0xff, 0xff, 0,
    0, 0, 1, 0xb5, 0x8f, 0xff, 0xf3, 0, 0,
    0, 0, 1, 1, 0x0b, 0,
    0, 0, 1, 2, 0x0b, 0
  };
  GstBuffer *buffer;
  const gsize size = sizeof(bytes) - (complete ? 0 : 6);
  bytes[23] = type << 3;
  buffer = gst_buffer_new_allocate(NULL, size, NULL);
  gst_buffer_fill(buffer, 0, bytes, size);
  GST_BUFFER_DURATION(buffer) = 40 * GST_MSECOND;
  return buffer;
}

static void
test_mpeg2_open_gop_admission(void)
{
  GstHarness *harness = new_mpeg2_admission_decoder();
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(harness->element);
  GstBus *bus = gst_bus_new();
  guint i;
  gst_element_set_bus(harness->element, bus);
  g_assert_cmpint(gst_harness_push(harness, new_mpeg2_picture(1, TRUE, FALSE)), ==, GST_FLOW_OK);
  for (i = 0; i < 2; i++) {
    GstMessage *message;
    g_assert_cmpint(gst_harness_push(harness, new_mpeg2_picture(3, FALSE, FALSE)), ==, GST_FLOW_OK);
    message = gst_bus_pop_filtered(bus, GST_MESSAGE_WARNING);
    g_assert_nonnull(message);
    gst_message_unref(message);
  }
  g_assert_cmpuint(mock.accepted_count, ==, 1);
  g_assert_cmpuint(self->mpeg2_dropped_inputs, ==, 2);
  g_assert_cmpuint(self->timestamps.length + gst_harness_buffers_in_queue(harness), ==, 1);
  g_assert_cmpint(gst_harness_push(harness, new_mpeg2_picture(2, FALSE, FALSE)), ==, GST_FLOW_OK);
  g_assert_cmpint(gst_harness_push(harness, new_mpeg2_picture(3, FALSE, FALSE)), ==, GST_FLOW_OK);
  g_assert_cmpuint(mock.accepted_count, ==, 3);
  g_assert_cmpuint(self->mpeg2_reference_pictures, ==, 2);
  g_assert_cmpint(gst_crystalhd_drain(GST_VIDEO_DECODER(self)), ==, GST_FLOW_OK);
  g_assert_cmpuint(gst_harness_buffers_in_queue(harness), ==, 3);
  gst_element_set_bus(harness->element, NULL);
  gst_object_unref(bus);
  gst_harness_teardown(harness);
}

static void
test_mpeg2_closed_gop_and_discontinuity(void)
{
  GstHarness *harness = new_mpeg2_admission_decoder();
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(harness->element);
  GstBuffer *input;
  g_assert_cmpint(gst_harness_push(harness, new_mpeg2_picture(1, TRUE, TRUE)), ==, GST_FLOW_OK);
  g_assert_cmpint(gst_harness_push(harness,
      new_mpeg2_prediction_picture(3, FALSE, FALSE, 0x0a, 0x80)), ==, GST_FLOW_OK);
  g_assert_cmpuint(mock.accepted_count, ==, 2);
  g_assert_cmpuint(self->mpeg2_dropped_inputs, ==, 0);
  input = new_mpeg2_picture(3, FALSE, FALSE);
  GST_BUFFER_FLAG_SET(input, GST_BUFFER_FLAG_DISCONT);
  g_assert_cmpint(gst_harness_push(harness, input), ==, GST_FLOW_OK);
  g_assert_cmpuint(mock.accepted_count, ==, 2);
  g_assert_cmpuint(self->mpeg2_reference_pictures, ==, 0);
  g_assert_cmpuint(self->mpeg2_dropped_inputs, ==, 1);
  g_assert_cmpint(gst_crystalhd_drain(GST_VIDEO_DECODER(self)), ==, GST_FLOW_OK);
  g_assert_true(lifecycle_flush(GST_VIDEO_DECODER(self)));
  g_assert_cmpuint(self->mpeg2_reference_pictures, ==, 0);
  self->codec.mpeg2_profile = 4;
  self->codec.mpeg2_height = 16;
  self->codec.mpeg2_progressive_sequence = TRUE;
  g_assert_cmpint(gst_harness_push(harness, new_mpeg2_picture(1, TRUE, FALSE)), ==, GST_FLOW_OK);
  g_assert_cmpint(gst_harness_push(harness, new_mpeg2_picture(3, FALSE, FALSE)), ==, GST_FLOW_OK);
  g_assert_cmpuint(self->mpeg2_dropped_inputs, ==, 1);
  gst_harness_teardown(harness);
}

static void
test_mpeg2_missing_valid_output_still_fails(void)
{
  GstHarness *harness = new_mpeg2_admission_decoder();
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(harness->element);
  mock.auto_output = FALSE;
  g_assert_cmpint(gst_harness_push(harness, new_mpeg2_picture(1, TRUE, FALSE)), ==, GST_FLOW_OK);
  g_assert_cmpint(gst_harness_push(harness, new_mpeg2_picture(3, FALSE, FALSE)), ==, GST_FLOW_OK);
  g_assert_cmpuint(mock.accepted_count, ==, 1);
  g_assert_cmpuint(self->mpeg2_dropped_inputs, ==, 1);
  mock.early_eos = TRUE;
  g_assert_cmpint(gst_crystalhd_drain(GST_VIDEO_DECODER(self)), ==, GST_FLOW_ERROR);
  g_assert_cmpuint(self->timestamps.length, ==, 1);
  gst_harness_teardown(harness);
}

static void
test_mpeg2_damaged_reference_recovery(void)
{
  GstHarness *harness = new_mpeg2_decoder();
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(harness->element);
  guint i;
  const guint types[] = { 1, 2, 3, 2, 2, 3, 1, 2, 3 };
  for (i = 0; i < G_N_ELEMENTS(types); i++)
    g_assert_cmpint(gst_harness_push(harness,
        new_mpeg2_sliced_picture(types[i], i != 3)), ==, GST_FLOW_OK);
  g_assert_cmpuint(mock.accepted_count, ==, 6);
  g_assert_cmpuint(self->mpeg2_dropped_inputs, ==, 3);
  g_assert_cmpuint(self->mpeg2_reference_pictures, ==, 2);
  g_assert_cmpint(gst_crystalhd_drain(GST_VIDEO_DECODER(self)), ==, GST_FLOW_OK);
  g_assert_cmpuint(gst_harness_buffers_in_queue(harness), ==, 6);
  for (i = 0; i < 6; i++) {
    guint8 byte;
    GstBuffer *output = gst_harness_pull(harness);
    g_assert_cmpuint(gst_buffer_extract(output, 0, &byte, 1), ==, 1);
    g_assert_cmpuint(byte, ==, i + 1);
    gst_buffer_unref(output);
  }
  gst_harness_teardown(harness);
}

static void
test_mpeg2_field_reference_proof_unchanged(void)
{
  GstHarness *harness = new_mpeg2_admission_decoder();
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(harness->element);
  GstBuffer *input = new_mpeg2_picture(1, FALSE, FALSE);
  guint8 top_field = 0xf1;
  g_assert_cmpuint(gst_buffer_fill(input, 15, &top_field, 1), ==, 1);
  g_assert_cmpint(gst_harness_push(harness, input), ==, GST_FLOW_OK);
  g_assert_false(self->mpeg2_frame_reference_tracking);
  g_assert_cmpint(gst_harness_push(harness, new_mpeg2_picture(3, FALSE, FALSE)), ==, GST_FLOW_OK);
  g_assert_cmpuint(mock.accepted_count, ==, 2);
  g_assert_cmpuint(self->mpeg2_dropped_inputs, ==, 0);
  g_assert_cmpint(gst_crystalhd_drain(GST_VIDEO_DECODER(self)), ==, GST_FLOW_OK);
  gst_harness_teardown(harness);
}

static void
test_mpeg2_positive_prediction_proof(void)
{
  const struct { guint type; guint8 first, second; } preserved[] = {
    { 2, 0x0a, 0x30 }, /* intra P, no I picture required */
    { 3, 0x0a, 0x30 }, /* intra B, no references required */
    { 3, 0x0f, 0 },    /* extended slice header, inconclusive */
    { 3, 0x09, 0 },    /* non-one address increment, inconclusive */
    { 3, 0x03, 0 },    /* reserved quantiser zero */
    { 3, 0x0a, 0 }     /* invalid/unrecognized type VLC */
  };
  const struct { guint type; guint8 first, second; } missing[] = {
    { 2, 0x0b, 0 },    /* P forward prediction without an anchor */
    { 2, 0x0a, 0x80 }, /* P zero-vector prediction still needs an anchor */
    { 3, 0x0b, 0 },    /* B interpolated prediction needs both anchors */
    { 3, 0x0a, 0x80 }  /* B backward-only still needs its one anchor */
  };
  guint i;
  GstHarness *harness = new_mpeg2_admission_decoder();
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(harness->element);
  /* No GOP header is required. A B using only the backward reference is
   * legal after the first anchor; an open-GOP flag does not forbid it.
   */
  g_assert_cmpint(gst_harness_push(harness, new_mpeg2_picture(1, FALSE, FALSE)), ==, GST_FLOW_OK);
  g_assert_cmpint(gst_harness_push(harness,
      new_mpeg2_prediction_picture(3, FALSE, FALSE, 0x0a, 0x80)), ==, GST_FLOW_OK);
  g_assert_cmpuint(mock.accepted_count, ==, 2);
  g_assert_cmpuint(self->mpeg2_dropped_inputs, ==, 0);
  g_assert_cmpint(gst_harness_push(harness, new_mpeg2_picture(3, FALSE, FALSE)), ==, GST_FLOW_OK);
  g_assert_cmpuint(mock.accepted_count, ==, 2);
  g_assert_cmpuint(self->mpeg2_dropped_inputs, ==, 1);
  gst_harness_teardown(harness);
  for (i = 0; i < G_N_ELEMENTS(preserved); i++) {
    GstBuffer *buffer;
    harness = new_mpeg2_admission_decoder();
    self = GST_CRYSTALHD_DEC(harness->element);
    buffer = new_mpeg2_prediction_picture(preserved[i].type, FALSE, FALSE,
        preserved[i].first, preserved[i].second);
    g_assert_cmpint(gst_harness_push(harness, buffer), ==, GST_FLOW_OK);
    g_assert_cmpuint(mock.accepted_count, ==, 1);
    g_assert_cmpuint(self->mpeg2_dropped_inputs, ==, 0);
    gst_harness_teardown(harness);
  }
  for (i = 0; i < G_N_ELEMENTS(missing); i++) {
    harness = new_mpeg2_admission_decoder();
    self = GST_CRYSTALHD_DEC(harness->element);
    g_assert_cmpint(gst_harness_push(harness,
        new_mpeg2_prediction_picture(missing[i].type, FALSE, FALSE,
            missing[i].first, missing[i].second)), ==, GST_FLOW_OK);
    g_assert_cmpuint(mock.accepted_count, ==, 0);
    g_assert_cmpuint(self->mpeg2_dropped_inputs, ==, 1);
    g_assert_cmpuint(self->timestamps.length, ==, 0);
    gst_harness_teardown(harness);
  }
  /* Only one payload byte cannot prove a full macroblock type. */
  harness = new_mpeg2_admission_decoder();
  self = GST_CRYSTALHD_DEC(harness->element);
  {
    GstBuffer *buffer = new_mpeg2_picture(3, FALSE, FALSE);
    gst_buffer_resize(buffer, 0, gst_buffer_get_size(buffer) - 1);
    g_assert_cmpint(gst_harness_push(harness, buffer), ==, GST_FLOW_OK);
  }
  g_assert_cmpuint(mock.accepted_count, ==, 1);
  g_assert_cmpuint(self->mpeg2_dropped_inputs, ==, 0);
  gst_harness_teardown(harness);
  /* Unknown/scalable profiles never enter the new reference admission path. */
  for (i = 0; i <= 6; i++) {
    if (i == 4 || i == 5)
      continue;
    harness = new_mpeg2_admission_decoder();
    self = GST_CRYSTALHD_DEC(harness->element);
    self->codec.mpeg2_profile = i;
    g_assert_cmpint(gst_harness_push(harness, new_mpeg2_picture(2, FALSE, FALSE)), ==, GST_FLOW_OK);
    g_assert_cmpint(gst_harness_push(harness, new_mpeg2_picture(3, FALSE, FALSE)), ==, GST_FLOW_OK);
    g_assert_cmpuint(mock.accepted_count, ==, 2);
    g_assert_cmpuint(self->mpeg2_dropped_inputs, ==, 0);
    gst_harness_teardown(harness);
  }
}

static void
test_mpeg2_new_sequence_invalidates_profile(void)
{
  GstHarness *harness = new_mpeg2_decoder();
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(harness->element);
  GstBuffer *input, *without_extension;
  g_assert_cmpint(gst_harness_push(harness, new_mpeg2_sliced_picture(1, TRUE)), ==, GST_FLOW_OK);
  g_assert_cmpuint(self->codec.mpeg2_profile, ==, 4);
  input = new_mpeg2_sliced_picture(2, FALSE);
  /* Keep the new sequence and picture, but omit its ten-byte sequence
   * extension. A previous Main/progressive classification is no longer proof.
   */
  without_extension = gst_buffer_append(
      gst_buffer_copy_region(input, GST_BUFFER_COPY_ALL, 0, 8),
      gst_buffer_copy_region(input, GST_BUFFER_COPY_ALL, 18, (gsize)-1));
  gst_buffer_unref(input);
  g_assert_cmpint(gst_harness_push(harness, without_extension), ==, GST_FLOW_OK);
  g_assert_cmpuint(self->codec.mpeg2_profile, ==, 0);
  g_assert_cmpuint(mock.accepted_count, ==, 2);
  g_assert_cmpuint(self->mpeg2_dropped_inputs, ==, 0);
  g_assert_cmpint(gst_crystalhd_drain(GST_VIDEO_DECODER(self)), ==, GST_FLOW_OK);
  gst_harness_teardown(harness);
}

static GstBuffer *
new_mpeg4_input(GstClockTime pts)
{
  return new_mpeg4_vop(pts, 0x10, 0x60);
}

static void
configure_mpeg4_codec(GstCrystalHdDec *self,
                      CrystalHdMpeg4ObjectType object_type)
{
  self->codec.subtype = BC_MSUBTYPE_DIVX;
  self->codec.mpeg4_object_type = object_type;
  self->codec.mpeg4_profile_level =
      object_type == CRYSTALHD_MPEG4_OBJECT_TYPE_SIMPLE ? 0x03 : 0xf3;
  self->codec.mpeg4_time_increment_resolution = 30;
  self->codec.mpeg4_time_increment_bits = 5;
}

static void
check_output(GstBuffer *buffer, GstClockTime pts, guint8 pixel)
{
  guint8 bytes[16 * 16 * 2];
  guint i;
  g_assert_nonnull(buffer);
  g_assert_cmpuint(GST_BUFFER_PTS(buffer), ==, pts);
  g_assert_false(GST_BUFFER_FLAG_IS_SET(buffer, GST_VIDEO_BUFFER_FLAG_INTERLACED));
  g_assert_false(GST_BUFFER_FLAG_IS_SET(buffer, GST_VIDEO_BUFFER_FLAG_TFF));
  g_assert_cmpuint(gst_buffer_extract(buffer, 0, bytes, sizeof(bytes)), ==,
                   sizeof(bytes));
  for (i = 0; i < sizeof(bytes); i++)
    g_assert_cmpuint(bytes[i], ==, pixel);
}

static void
flush_decoder(GstHarness *harness)
{
  g_assert_true(gst_harness_push_event(harness, gst_event_new_flush_start()));
  g_assert_true(gst_harness_push_event(harness, gst_event_new_flush_stop(TRUE)));
}

static void
output_destroyed(gpointer count, GstMiniObject *object)
{
  (void)object;
  (*(guint *)count)++;
}

static void
test_frame_references(void)
{
  GstHarness *harness = new_decoder();
  GstBuffer *held;
  guint destroyed_outputs = 0;
  guint i;
  g_assert_cmpint(gst_harness_push(harness, new_input(0)), ==, GST_FLOW_OK);
  held = gst_harness_try_pull(harness);
  check_output(held, 0, 1);
  g_assert_cmpuint(mock.destroyed_inputs, ==, 1);
  gst_mini_object_weak_ref(GST_MINI_OBJECT(held), output_destroyed,
                           &destroyed_outputs);
  for (i = 1; i < 12; i++) {
    GstBuffer *output;
    g_assert_cmpint(gst_harness_push(harness, new_input(i * 40 * GST_MSECOND)),
                    ==, GST_FLOW_OK);
    output = gst_harness_try_pull(harness);
    check_output(output, i * 40 * GST_MSECOND, i + 1);
    g_assert_cmpuint(GST_MINI_OBJECT_REFCOUNT_VALUE(output), ==, 1);
    gst_buffer_unref(output);
    g_assert_cmpuint(mock.destroyed_inputs, ==, i + 1);
  }
  g_assert_true(gst_harness_push_event(harness, gst_event_new_eos()));
  gst_harness_teardown(harness);
  /* Output is an independent copy, valid after release/flush/device close. */
  check_output(held, 0, 1);
  g_assert_cmpuint(destroyed_outputs, ==, 0);
  gst_buffer_unref(held);
  g_assert_cmpuint(destroyed_outputs, ==, 1);
}

static void
test_busy_stale_output(void)
{
  GstHarness *harness = new_decoder();
  GstBuffer *output;
  mock.busy_stale = TRUE;
  g_assert_cmpint(gst_harness_push(harness, new_input(0)), ==, GST_FLOW_OK);
  output = gst_harness_try_pull(harness);
  check_output(output, 0, 1);
  gst_buffer_unref(output);
  g_assert_cmpuint(mock.input_calls, ==, 2);
  g_assert_cmpuint(mock.release_calls, ==, 2);
  g_assert_cmpuint(gst_harness_buffers_in_queue(harness), ==, 0);
  g_assert_cmpuint(GST_CRYSTALHD_DEC(harness->element)->timestamps.length, ==, 0);
  gst_harness_teardown(harness);
}

static void
test_flush_stale_output(void)
{
  GstHarness *harness = new_decoder();
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(harness->element);
  GstBuffer *output, *held;
  GstSegment segment;
  guint64 old_timestamp, next_timestamp;
  g_assert_cmpint(gst_harness_push(harness, new_input(0)), ==, GST_FLOW_OK);
  held = gst_harness_try_pull(harness);
  check_output(held, 0, 1);
  mock.auto_output = FALSE;
  g_assert_cmpint(gst_harness_push(harness, new_input(40 * GST_MSECOND)),
                  ==, GST_FLOW_OK);
  old_timestamp = mock.accepted[1];
  next_timestamp = self->next_hardware_timestamp;
  flush_decoder(harness);
  g_assert_cmpuint(mock.open_calls, ==, 2);
  g_assert_cmpuint(mock.close_calls, ==, 1);
  g_assert_cmpuint(mock.decoder_close_calls, ==, 1);
  g_assert_cmpuint(mock.stop_calls, ==, 1);
  g_assert_cmpuint(mock.format_calls, ==, 2);
  g_assert_cmpuint(mock.flush_calls, ==, 0);
  g_assert_cmpuint(self->next_hardware_timestamp, ==, next_timestamp);
  g_assert_cmpuint(self->timestamps.length, ==, 0);
  g_assert_false(self->output_configured);
  g_assert_false(self->need_second_field);
  check_output(held, 0, 1);
  gst_segment_init(&segment, GST_FORMAT_TIME);
  segment.start = GST_SECOND;
  segment.position = GST_SECOND;
  g_assert_true(gst_harness_push_event(harness, gst_event_new_segment(&segment)));
  queue_picture(old_timestamp, 0xee);
  mock.auto_output = TRUE;
  g_assert_cmpint(gst_harness_push(harness, new_input(GST_SECOND)), ==, GST_FLOW_OK);
  output = gst_harness_try_pull(harness);
  check_output(output, GST_SECOND, 3);
  gst_buffer_unref(output);
  g_assert_cmpuint(mock.accepted[2], ==, next_timestamp);
  g_assert_cmpuint(mock.accepted[2], >, old_timestamp);
  g_assert_cmpuint(gst_harness_buffers_in_queue(harness), ==, 0);
  g_assert_cmpuint(mock.destroyed_inputs, ==, 3);
  gst_harness_teardown(harness);
  g_assert_cmpuint(mock.close_calls, ==, 2);
  g_assert_cmpuint(mock.successful_opens, ==, mock.close_calls);
  check_output(held, 0, 1);
  gst_buffer_unref(held);
}

static void
test_flush_reopen_errors(void)
{
  const MockOperation failures[] = {
    MOCK_STOP, MOCK_DECODER_CLOSE, MOCK_DEVICE_CLOSE, MOCK_DEVICE_OPEN,
    MOCK_VERSION, MOCK_FORMAT, MOCK_DECODER_OPEN, MOCK_COLOR, MOCK_START,
    MOCK_CAPTURE
  };
  guint i;
  for (i = 0; i < G_N_ELEMENTS(failures); i++) {
    GstHarness *harness = new_decoder();
    GstCrystalHdDec *self = GST_CRYSTALHD_DEC(harness->element);
    gboolean teardown_failure = failures[i] >= MOCK_STOP;
    mock.auto_output = FALSE;
    g_assert_cmpint(gst_harness_push(harness, new_input(0)), ==, GST_FLOW_OK);
    mock.fail_operations = 1U << failures[i];
    g_assert_false(gst_crystalhd_flush(GST_VIDEO_DECODER(self)));
    /* Even a failed stop/close must run every remaining cleanup operation;
     * failed reopening must never leave a half-configured device attached.
     */
    g_assert_null(self->device);
    g_assert_false(self->decoder_open);
    g_assert_false(self->decoder_started);
    g_assert_false(mock.device_active);
    g_assert_cmpuint(self->timestamps.length, ==, 0);
    g_assert_cmpuint(mock.open_calls, ==, teardown_failure ? 1 : 2);
    g_assert_cmpuint(mock.successful_opens, ==, mock.close_calls);
    g_assert_cmpuint(mock.decoder_close_calls, >=, 1);
    g_assert_cmpuint(mock.stop_calls, >=, 1);
    g_assert_cmpuint(mock.flush_calls, ==, 0);
    g_assert_nonnull(self->input_state);
    g_assert_true(gst_caps_is_fixed(self->input_state->caps));
    mock.fail_operations = 0;
    gst_harness_teardown(harness);
    g_assert_cmpuint(mock.destroyed_inputs, ==, 1);
    g_assert_cmpuint(mock.successful_opens, ==, mock.close_calls);
  }
}

static void
test_stop_cleanup_errors(void)
{
  const MockOperation failures[] = { MOCK_STOP, MOCK_DECODER_CLOSE, MOCK_DEVICE_CLOSE };
  guint i;
  for (i = 0; i < G_N_ELEMENTS(failures); i++) {
    GstHarness *harness = new_decoder();
    GstCrystalHdDec *self = GST_CRYSTALHD_DEC(harness->element);
    mock.fail_operations = 1U << failures[i];
    g_assert_cmpint(gst_element_set_state(harness->element, GST_STATE_READY),
                    ==, GST_STATE_CHANGE_FAILURE);
    g_assert_null(self->output_thread);
    g_assert_null(self->device);
    g_assert_null(self->input_state);
    g_assert_cmpuint(mock.stop_calls, ==, 1);
    g_assert_cmpuint(mock.decoder_close_calls, ==, 1);
    g_assert_cmpuint(mock.close_calls, ==, 1);
    mock.fail_operations = 0;
    g_assert_cmpint(gst_element_set_state(harness->element, GST_STATE_NULL),
                    ==, GST_STATE_CHANGE_SUCCESS);
    gst_harness_teardown(harness);
    g_assert_cmpuint(mock.stop_calls, ==, 1);
    g_assert_cmpuint(mock.decoder_close_calls, ==, 1);
    g_assert_cmpuint(mock.close_calls, ==, 1);
  }
}

static void
test_flush_codec_state(void)
{
  const struct {
    const gchar *caps;
    BC_MEDIA_SUBTYPE subtype;
    guint8 metadata[64];
    guint metadata_size;
    guint skip;
    guint expected_size;
    gboolean packetized;
    gboolean frame_layer;
  } cases[] = {
    { "video/x-wmv,wmvversion=3,format=WMV3,stream-format=frame-layer,"
      "header-format=asf", BC_MSUBTYPE_WMV3,
      { 0x41, 0x42, 0x43, 0x44, 0xff }, 5, 0, 4, TRUE, TRUE },
    /* Original avidemux codec_data: preserve STRUCT_C across full reopen. */
    { "video/x-wmv,wmvversion=3,format=WMV3", BC_MSUBTYPE_WMV3,
      { 0x0f, 0xf1, 0x8a, 0x01, 0x40, 0x0f }, 6, 0, 4, TRUE, FALSE },
    { "video/x-wmv,wmvversion=3,format=WVC1,stream-format=asf,"
      "header-format=asf", BC_MSUBTYPE_WVC1,
      { 0xff, 0, 0, 1, 0x0f, 0x41, 0x42, 0x43, 0x44 }, 9, 1, 8, TRUE, FALSE },
    { "video/x-wmv,wmvversion=3,format=WVC1,stream-format=bdu,"
      "header-format=none", BC_MSUBTYPE_VC1,
      { 0 }, 0, 0, 0, FALSE, FALSE },
    { "video/mpeg,mpegversion=4,systemstream=false,parsed=true,"
      "profile=simple,level=(string)3,interlace-mode=progressive", BC_MSUBTYPE_DIVX,
      { 0,0,1,0xb0,3,0,0,1,0xb5,0x89,0x13,0,0,1,0,
        0,0,1,0x20,0,0xc4,0x8d,0x88,0,0xf5,0x14,4,0x2d,0x14,0x63,
        0,0,1,0xb2,'L','a','v','c','6','1','.','1','9','.','1','0','1' },
      47, 0, 47, TRUE, FALSE }
  };
  guint i;
  for (i = 0; i < G_N_ELEMENTS(cases); i++) {
    GstHarness *harness = new_decoder();
    GstCrystalHdDec *self = GST_CRYSTALHD_DEC(harness->element);
    GstVideoCodecState *state = g_new0(GstVideoCodecState, 1);
    GstCaps *expected_caps;
    guint opens;
    const guint width = cases[i].subtype == BC_MSUBTYPE_DIVX ? 640 : 16;
    const guint height = cases[i].subtype == BC_MSUBTYPE_DIVX ? 360 : 16;
    state->ref_count = 1;
    gst_video_info_set_format(&state->info, GST_VIDEO_FORMAT_YUY2,
                              width, height);
    state->caps = gst_caps_from_string(cases[i].caps);
    if (cases[i].metadata_size != 0) {
      GstBuffer *metadata = gst_buffer_new_allocate(NULL, cases[i].metadata_size, NULL);
      gst_buffer_fill(metadata, 0, cases[i].metadata, cases[i].metadata_size);
      gst_caps_set_simple(state->caps, "codec_data", GST_TYPE_BUFFER, metadata, NULL);
      gst_buffer_unref(metadata);
    }
    expected_caps = gst_caps_copy(state->caps);
    g_assert_true(gst_crystalhd_set_format(GST_VIDEO_DECODER(self), state));
    gst_video_codec_state_unref(state);
    /* self is now the sole codec-state owner: set_format(self->input_state)
     * must retain it before releasing the old reference during flush.
     */
    g_assert_cmpint(self->input_state->ref_count, ==, 1);
    self->next_hardware_timestamp = 123450000;
    self->need_second_field = TRUE;
    self->have_simple_picture_number = TRUE;
    self->wmv3_reordered_output = TRUE;
    self->wmv3_timing_valid = FALSE;
    self->wmv3_last_duration = 33 * GST_MSECOND;
    opens = mock.open_calls;
    g_assert_true(gst_crystalhd_flush(GST_VIDEO_DECODER(self)));
    g_assert_cmpuint(mock.open_calls, ==, opens + 1);
    g_assert_true(gst_caps_is_equal(self->input_state->caps, expected_caps));
    g_assert_cmpuint(self->next_hardware_timestamp, ==, 123450000);
    g_assert_false(self->need_second_field);
    g_assert_false(self->have_simple_picture_number);
    g_assert_false(self->wmv3_reordered_output);
    g_assert_true(self->wmv3_timing_valid);
    g_assert_cmpuint(self->wmv3_last_duration, ==, GST_CLOCK_TIME_NONE);
    g_assert_cmpint(self->codec.subtype, ==, cases[i].subtype);
    g_assert_cmpint(self->codec.frame_layer, ==, cases[i].frame_layer);
    g_assert_cmpint(gst_video_decoder_get_packetized(GST_VIDEO_DECODER(self)),
                    ==, cases[i].packetized);
    g_assert_cmpuint(self->input_metadata_size, ==, cases[i].expected_size);
    g_assert_cmpint(mock.last_format.mSubtype, ==, cases[i].subtype);
    g_assert_cmpuint(mock.last_format.width, ==, width);
    g_assert_cmpuint(mock.last_format.height, ==, height);
    g_assert_cmpuint(mock.last_format.metaDataSz, ==, cases[i].expected_size);
    if (cases[i].subtype == BC_MSUBTYPE_DIVX) {
      g_assert_cmpuint(mock.last_format.startCodeSz, ==, 4);
      g_assert_cmpuint(self->codec.mpeg4_time_increment_resolution, ==, 30);
      g_assert_cmpuint(self->codec.mpeg4_time_increment_bits, ==, 5);
    }
    g_assert_cmpmem(mock.last_metadata, cases[i].expected_size,
                    cases[i].metadata + cases[i].skip, cases[i].expected_size);
    g_assert_true(self->input_flushed);
    gst_caps_unref(expected_caps);
    gst_harness_teardown(harness);
    g_assert_cmpuint(mock.successful_opens, ==, mock.close_calls);
  }
}

static void
test_mpeg4_device_gate(void)
{
  static const guint8 metadata[] = {
    0,0,1,0xb0,3,0,0,1,0xb5,0x89,0x13,0,0,1,0,
    0,0,1,0x20,0,0xc4,0x8d,0x88,0,0xf5,0x14,4,0x2d,0x14,0x63,
    0,0,1,0xb2,'L','a','v','c','6','1','.','1','9','.','1','0','1'
  };
  GstHarness *harness = new_decoder();
  GstVideoCodecState *state = g_new0(GstVideoCodecState, 1);
  GstBuffer *codec_data = gst_buffer_new_allocate(NULL, sizeof(metadata), NULL);
  guint formats = mock.format_calls;
  guint opens = mock.open_calls;

  gst_buffer_fill(codec_data, 0, metadata, sizeof(metadata));
  state->ref_count = 1;
  gst_video_info_set_format(&state->info, GST_VIDEO_FORMAT_YUY2, 320, 180);
  state->caps = gst_caps_from_string("video/mpeg,mpegversion=4,"
      "systemstream=false,parsed=true,profile=simple,level=(string)3,"
      "interlace-mode=progressive");
  gst_caps_set_simple(state->caps, "codec_data", GST_TYPE_BUFFER, codec_data,
                      NULL);
  gst_buffer_unref(codec_data);
  g_assert_false(gst_crystalhd_set_format(
      GST_VIDEO_DECODER(harness->element), state));
  g_assert_cmpuint(mock.open_calls, ==, opens);
  g_assert_cmpuint(mock.format_calls, ==, formats);

  gst_video_info_set_format(&state->info, GST_VIDEO_FORMAT_YUY2, 640, 360);
  mock.device_version = 0;
  g_assert_false(gst_crystalhd_set_format(
      GST_VIDEO_DECODER(harness->element), state));
  g_assert_cmpuint(mock.format_calls, ==, formats);
  g_assert_false(mock.device_active);
  gst_video_codec_state_unref(state);
  gst_harness_teardown(harness);
}

static void
test_reordered_duplicate_output(void)
{
  GstHarness *harness = new_decoder();
  GstBuffer *output;
  mock.auto_output = FALSE;
  /* Decode order differs from presentation order, and two input PTS values
   * intentionally coincide. Only the hardware token identifies each input.
   */
  g_assert_cmpint(gst_harness_push(harness, new_input(40 * GST_MSECOND)),
                  ==, GST_FLOW_OK);
  g_assert_cmpint(gst_harness_push(harness, new_input(0)), ==, GST_FLOW_OK);
  g_assert_cmpint(gst_harness_push(harness, new_input(40 * GST_MSECOND)),
                  ==, GST_FLOW_OK);
  queue_picture(mock.accepted[1], 2);
  queue_picture(mock.accepted[1], 0xee); /* duplicate, not the next frame */
  queue_picture(mock.accepted[0], 1);
  queue_picture(mock.accepted[2], 3);
  g_assert_true(gst_harness_push_event(harness, gst_event_new_eos()));
  output = gst_harness_try_pull(harness);
  check_output(output, 0, 2);
  gst_buffer_unref(output);
  output = gst_harness_try_pull(harness);
  check_output(output, 40 * GST_MSECOND, 1);
  gst_buffer_unref(output);
  output = gst_harness_try_pull(harness);
  check_output(output, 40 * GST_MSECOND, 3);
  gst_buffer_unref(output);
  g_assert_cmpuint(gst_harness_buffers_in_queue(harness), ==, 0);
  g_assert_cmpuint(mock.destroyed_inputs, ==, 3);
  g_assert_cmpuint(mock.release_calls, ==, 4);
  gst_harness_teardown(harness);
}

static void
test_wmv3_display_order_pts(void)
{
  GstHarness *harness = new_decoder();
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(harness->element);
  GstBuffer *output;

  self->codec.subtype = BC_MSUBTYPE_WMV3;
  mock.auto_output = FALSE;
  g_assert_cmpint(gst_harness_push(harness, new_input(0)), ==, GST_FLOW_OK);
  g_assert_cmpint(gst_harness_push(harness, new_input(40 * GST_MSECOND)),
                  ==, GST_FLOW_OK);
  g_assert_cmpint(gst_harness_push(harness, new_input(80 * GST_MSECOND)),
                  ==, GST_FLOW_OK);

  /* WMV3 pixels are already returned in display order. The associated input
   * tokens can still arrive in decode order and must not reorder them again. */
  queue_picture(mock.accepted[0], 1);
  queue_picture(mock.accepted[2], 3);
  queue_picture(mock.accepted[1], 2);
  g_assert_true(gst_harness_push_event(harness, gst_event_new_eos()));

  output = gst_harness_try_pull(harness);
  check_output(output, 0, 1);
  gst_buffer_unref(output);
  output = gst_harness_try_pull(harness);
  check_output(output, 40 * GST_MSECOND, 3);
  gst_buffer_unref(output);
  output = gst_harness_try_pull(harness);
  check_output(output, 80 * GST_MSECOND, 2);
  gst_buffer_unref(output);
  g_assert_true(self->wmv3_reordered_output);
  g_assert_cmpuint(gst_harness_buffers_in_queue(harness), ==, 0);
  g_assert_cmpuint(mock.destroyed_inputs, ==, 3);
  gst_harness_teardown(harness);
}

static void
run_wmv3_seek_clip(GstClockTime target, gboolean missing_duration)
{
  static const GstClockTime pts[] = {
    0, 33 * GST_MSECOND, 66 * GST_MSECOND, 100 * GST_MSECOND
  };
  static const guint8 pixels[] = { 1, 3, 2, 4 };
  GstHarness *harness = new_decoder();
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(harness->element);
  GstSegment segment;
  GstBuffer *input;
  GstBuffer *output;
  guint first;
  guint i;

  self->codec.subtype = BC_MSUBTYPE_WMV3;
  mock.auto_output = FALSE;
  gst_segment_init(&segment, GST_FORMAT_TIME);
  segment.start = target;
  segment.position = segment.start;
  g_assert_true(gst_harness_push_event(harness, gst_event_new_segment(&segment)));
  for (i = 0; i < G_N_ELEMENTS(pts); ++i) {
    input = new_input(pts[i]);
    if (missing_duration)
      GST_BUFFER_DURATION(input) = GST_CLOCK_TIME_NONE;
    g_assert_cmpint(gst_harness_push(harness, input), ==, GST_FLOW_OK);
  }

  queue_picture(mock.accepted[0], 1);
  queue_picture(mock.accepted[2], 3);
  queue_picture(mock.accepted[1], 2);
  queue_picture(mock.accepted[3], 4);
  g_assert_true(gst_harness_push_event(harness, gst_event_new_eos()));

  first = target == 33 * GST_MSECOND ? 1 : 3;
  for (i = first; i < G_N_ELEMENTS(pts); ++i) {
    output = gst_harness_try_pull(harness);
    check_output(output, pts[i], pixels[i]);
    gst_buffer_unref(output);
  }
  g_assert_null(gst_harness_try_pull(harness));
  g_assert_true(self->wmv3_reordered_output);
  g_assert_cmpuint(gst_harness_buffers_in_queue(harness), ==, 0);
  g_assert_cmpuint(mock.destroyed_inputs, ==, 4);
  gst_harness_teardown(harness);
}

static void
test_wmv3_seek_clip_uses_pts_cadence(void)
{
  /* A stale 40 ms duration from 25 fps caps must not extend the preceding
   * 30 fps picture onto either an early target before token reordering is
   * observed or a later target after it. Exercise absent and explicit stale
   * per-buffer durations. */
  run_wmv3_seek_clip(33 * GST_MSECOND, TRUE);
  run_wmv3_seek_clip(33 * GST_MSECOND, FALSE);
  run_wmv3_seek_clip(100 * GST_MSECOND, TRUE);
  run_wmv3_seek_clip(100 * GST_MSECOND, FALSE);
}

static void
test_wmv3_variable_pts_duration(void)
{
  static const GstClockTime pts[] = {
    0, 33 * GST_MSECOND, 75 * GST_MSECOND, 100 * GST_MSECOND
  };
  static const GstClockTime durations[] = {
    33 * GST_MSECOND, 42 * GST_MSECOND, 25 * GST_MSECOND,
    25 * GST_MSECOND
  };
  static const guint8 pixels[] = { 1, 3, 2, 4 };
  guint pass;
  guint i;

  /* First infer VFR durations from PTS cadence, then repeat with distinct
   * declared durations to prove each duration follows its PTS tuple across
   * the 0,2,1 hardware-token order. */
  for (pass = 0; pass < 2; ++pass) {
    GstHarness *harness = new_decoder();
    GstCrystalHdDec *self = GST_CRYSTALHD_DEC(harness->element);
    GstBuffer *input;
    GstBuffer *output;

    self->codec.subtype = BC_MSUBTYPE_WMV3;
    mock.auto_output = FALSE;
    for (i = 0; i < G_N_ELEMENTS(pts); ++i) {
      input = new_input(pts[i]);
      GST_BUFFER_DURATION(input) = pass == 0 ? GST_CLOCK_TIME_NONE : durations[i];
      g_assert_cmpint(gst_harness_push(harness, input), ==, GST_FLOW_OK);
    }
    queue_picture(mock.accepted[0], 1);
    queue_picture(mock.accepted[2], 3);
    queue_picture(mock.accepted[1], 2);
    queue_picture(mock.accepted[3], 4);
    g_assert_true(gst_harness_push_event(harness, gst_event_new_eos()));

    for (i = 0; i < G_N_ELEMENTS(pts); ++i) {
      output = gst_harness_try_pull(harness);
      check_output(output, pts[i], pixels[i]);
      g_assert_cmpuint(GST_BUFFER_DURATION(output), ==, durations[i]);
      gst_buffer_unref(output);
    }
    g_assert_null(gst_harness_try_pull(harness));
    g_assert_true(self->wmv3_reordered_output);
    g_assert_cmpuint(self->wmv3_last_duration, ==, 25 * GST_MSECOND);
    g_assert_cmpuint(mock.destroyed_inputs, ==, 4);
    gst_harness_teardown(harness);
  }
}

static void
test_wmv3_millisecond_duration(void)
{
  GstHarness *harness = new_decoder_with_caps(
      "video/x-wmv,wmvversion=3,format=WMV3,stream-format=asf,"
      "header-format=asf,codec_data=(buffer)0ff18a01,"
      "width=16,height=16,framerate=24/1");
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(harness->element);
  guint i;

  /* Robotica_1080-first60.wmv uses floor(i * 1000 / 24) millisecond PTS.
   * The first 41 packets lack duration; the last 19 declare 41 ms. Replay
   * that timing with small mock pictures: #341 must infer the missing
   * 41/42 ms cadence, not the old caps-derived 41,666,666 ns duration. */
  mock.auto_output = FALSE;
  g_assert_cmpint(self->codec.subtype, ==, BC_MSUBTYPE_WMV3);
  for (i = 0; i < 60; ++i) {
    GstBuffer *input = new_input((i * 1000 / 24) * GST_MSECOND);

    GST_BUFFER_DURATION(input) = i < 41
        ? GST_CLOCK_TIME_NONE : 41 * GST_MSECOND;
    g_assert_cmpint(gst_harness_push(harness, input), ==, GST_FLOW_OK);
  }
  for (i = 0; i < 60; ++i)
    queue_picture(mock.accepted[i], i + 1);
  g_assert_true(gst_harness_push_event(harness, gst_event_new_eos()));

  for (i = 0; i < 60; ++i) {
    GstBuffer *output = gst_harness_try_pull(harness);
    GstClockTime pts = (i * 1000 / 24) * GST_MSECOND;
    GstClockTime duration = i < 41
        ? (((i + 1) * 1000 / 24) - (i * 1000 / 24)) * GST_MSECOND
        : 41 * GST_MSECOND;

    check_output(output, pts, i + 1);
    g_assert_cmpuint(GST_BUFFER_DURATION(output), ==, duration);
    g_assert_false(GST_BUFFER_FLAG_IS_SET(output, GST_VIDEO_BUFFER_FLAG_RFF));
    g_assert_false(GST_BUFFER_FLAG_IS_SET(output, GST_VIDEO_BUFFER_FLAG_ONEFIELD));
    gst_buffer_unref(output);
  }
  g_assert_null(gst_harness_try_pull(harness));
  g_assert_cmpuint(GST_VIDEO_INFO_FPS_N(&self->output_info), ==, 24);
  g_assert_cmpuint(GST_VIDEO_INFO_FPS_D(&self->output_info), ==, 1);
  /* In particular, the last picture retains its declared duration even
   * though no successor is available for cadence inference. */
  g_assert_cmpuint(self->wmv3_last_duration, ==, 41 * GST_MSECOND);
  g_assert_cmpuint(self->timestamps.length, ==, 0);
  g_assert_cmpuint(mock.destroyed_inputs, ==, 60);
  g_assert_cmpuint(mock.release_calls, ==, 60);
  gst_harness_teardown(harness);
}

static void
test_wmv3_sparse_pts_disables_timing(void)
{
  GstHarness *harness = new_decoder();
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(harness->element);
  CrystalHdTimestamp *pending;
  GstBuffer *output;

  self->codec.subtype = BC_MSUBTYPE_WMV3;
  mock.auto_output = FALSE;
  g_assert_cmpint(gst_harness_push(harness, new_input(0)), ==, GST_FLOW_OK);
  g_assert_cmpint(gst_harness_push(harness, new_input(GST_CLOCK_TIME_NONE)),
                  ==, GST_FLOW_OK);
  g_assert_cmpint(gst_harness_push(harness, new_input(80 * GST_MSECOND)),
                  ==, GST_FLOW_OK);

  /* A non-head token plus a sparse pending PTS cannot establish complete
   * presentation order. Publish the token-owned timing and keep the repair
   * disabled until flush rather than resuming from divergent base state. */
  queue_picture(mock.accepted[2], 3);
  g_assert_cmpint(gst_crystalhd_receive_available(self), ==, GST_FLOW_OK);
  output = gst_harness_try_pull(harness);
  check_output(output, 80 * GST_MSECOND, 3);
  gst_buffer_unref(output);
  g_assert_false(self->wmv3_timing_valid);
  g_assert_true(self->wmv3_reordered_output);
  g_assert_cmpuint(self->timestamps.length, ==, 2);
  pending = self->timestamps.head->data;
  g_assert_cmpuint(pending->pts, ==, 0);

  flush_decoder(harness);
  g_assert_true(self->wmv3_timing_valid);
  g_assert_false(self->wmv3_reordered_output);
  g_assert_cmpuint(self->wmv3_last_duration, ==, GST_CLOCK_TIME_NONE);
  g_assert_true(g_queue_is_empty(&self->timestamps));
  g_assert_cmpuint(mock.destroyed_inputs, ==, 3);
  gst_harness_teardown(harness);
}

static void
test_input_errors(void)
{
  GstHarness *harness = new_decoder();
  mock.fail_input = TRUE;
  g_assert_cmpint(gst_harness_push(harness, new_input(0)), ==, GST_FLOW_ERROR);
  g_assert_cmpuint(mock.destroyed_inputs, ==, 1);
  g_assert_cmpuint(GST_CRYSTALHD_DEC(harness->element)->timestamps.length, ==, 0);
  gst_harness_teardown(harness);

  harness = new_decoder();
  mock.busy_stale = TRUE;
  mock.fail_status = TRUE;
  g_assert_cmpint(gst_harness_push(harness, new_input(0)), ==, GST_FLOW_ERROR);
  /* An asynchronous status failure can occur after a successful retry has
   * queued the input. Teardown must release that base-owned frame too. */
  gst_harness_teardown(harness);
  g_assert_cmpuint(mock.destroyed_inputs, ==, 1);

  harness = new_decoder();
  mock.fail_status = TRUE; /* accepted input, then receive error */
  g_assert_cmpint(gst_harness_push(harness, new_input(0)), ==, GST_FLOW_ERROR);
  gst_harness_teardown(harness);
  g_assert_cmpuint(mock.destroyed_inputs, ==, 1);
}

static void
test_repeated_empty_flush(void)
{
  GstHarness *harness = new_decoder();
  GstVideoDecoder *decoder = GST_VIDEO_DECODER(harness->element);
  /* One full device reopen, with no input between repeated flush/empty EOS. */
  g_assert_true(gst_crystalhd_flush(decoder));
  g_assert_true(gst_crystalhd_flush(decoder));
  g_assert_cmpint(gst_crystalhd_drain(decoder), ==, GST_FLOW_OK);
  g_assert_cmpuint(mock.open_calls, ==, 2);
  g_assert_cmpuint(mock.close_calls, ==, 1);
  g_assert_cmpuint(mock.flush_calls, ==, 0);
  g_assert_cmpuint(mock.drain_calls, ==, 0);
  /* The reopened session accepts more input, then permits another reopen. */
  g_assert_cmpint(gst_harness_push(harness, new_input(0)), ==, GST_FLOW_OK);
  gst_buffer_unref(gst_harness_try_pull(harness));
  g_assert_true(gst_crystalhd_flush(decoder));
  g_assert_cmpuint(mock.open_calls, ==, 3);
  g_assert_cmpuint(mock.close_calls, ==, 2);
  g_assert_cmpuint(mock.flush_calls, ==, 0);
  g_assert_true(gst_harness_push_event(harness, gst_event_new_eos()));
  g_assert_cmpuint(mock.drain_calls, ==, 0);
  gst_harness_teardown(harness);
  g_assert_cmpuint(mock.close_calls, ==, 3);
}

static void
test_status_probe_no_ready_peek(void)
{
  GstHarness *harness = new_decoder();
  GstBuffer *output;

  g_assert_cmpint(gst_harness_push(harness, new_input(0)), ==, GST_FLOW_OK);
  output = gst_harness_try_pull(harness);
  check_output(output, 0, 1);
  gst_buffer_unref(output);
  g_assert_cmpuint(mock.status_calls, ==, 1);
  g_assert_cmpuint(mock.status_no_ready_peek_calls, ==, 1);
  g_assert_cmpuint(mock.status_unflagged_calls, ==, 0);
  g_assert_cmpuint(mock.status_invalid_requests, ==, 0);

  mock.early_eos = TRUE;
  g_assert_true(gst_harness_push_event(harness, gst_event_new_eos()));
  g_assert_cmpuint(mock.status_calls, ==, 3);
  g_assert_cmpuint(mock.status_no_ready_peek_calls, ==, 2);
  /* The one-shot final statistics report consumes counters, not RLL alone,
   * and deliberately retains the ordinary status request. */
  g_assert_cmpuint(mock.status_unflagged_calls, ==, 1);
  g_assert_cmpuint(mock.status_invalid_requests, ==, 0);
  g_assert_true(GST_CRYSTALHD_DEC(harness->element)->output_eos);
  g_assert_true(GST_CRYSTALHD_DEC(harness->element)->drain_idle);
  gst_harness_teardown(harness);

  harness = new_decoder();
  mock.fail_status = TRUE;
  g_assert_cmpint(gst_harness_push(harness, new_input(0)), ==, GST_FLOW_ERROR);
  g_assert_cmpuint(mock.status_calls, ==, 1);
  g_assert_cmpuint(mock.status_no_ready_peek_calls, ==, 1);
  g_assert_cmpuint(mock.status_unflagged_calls, ==, 0);
  g_assert_cmpuint(mock.status_invalid_requests, ==, 0);
  g_assert_cmpuint(mock.release_calls, ==, 0);
  gst_harness_teardown(harness);

  harness = new_decoder_with_caps_and_device(
      "video/x-h264,stream-format=byte-stream,alignment=au,parsed=true,"
      "width=16,height=16,framerate=25/1", 0);
  g_assert_true(GST_CRYSTALHD_DEC(harness->element)->is_70012);
  mock.auto_output = FALSE;
  g_assert_cmpint(gst_harness_push(harness, new_input(0)), ==, GST_FLOW_OK);
  g_assert_null(gst_harness_try_pull(harness));
  g_assert_cmpuint(mock.status_calls, ==, 1);
  g_assert_cmpuint(mock.status_no_ready_peek_calls, ==, 0);
  g_assert_cmpuint(mock.status_unflagged_calls, ==, 1);
  g_assert_cmpuint(mock.status_invalid_requests, ==, 0);
  gst_harness_teardown(harness);
}

static void
test_input_admission(void)
{
  GstHarness *harness = new_decoder();
  GstBuffer *output;
  mock.auto_output = FALSE;
  g_assert_cmpint(gst_harness_push(harness, new_input(0)), ==, GST_FLOW_OK);
  queue_picture(mock.accepted[0], 1);
  /* The 8-byte H264 input needs 8 + 3*32 +128 bytes under the conservative
   * complete-call bound. A previous output must be released first.
   */
  mock.required_bytes = 232;
  mock.free_bytes = mock.required_bytes - 1;
  mock.release_makes_room = TRUE;
  mock.auto_output = TRUE;
  g_assert_cmpint(gst_harness_push(harness, new_input(40 * GST_MSECOND)),
                  ==, GST_FLOW_OK);
  g_assert_cmpuint(mock.unsafe_submissions, ==, 0);
  g_assert_cmpuint(mock.input_calls, ==, 2);
  output = gst_harness_try_pull(harness);
  check_output(output, 0, 1);
  gst_buffer_unref(output);
  output = gst_harness_try_pull(harness);
  check_output(output, 40 * GST_MSECOND, 2);
  gst_buffer_unref(output);
  gst_harness_teardown(harness);
}

static void
test_mpeg4_simple_missing_timestamp(void)
{
  GstHarness *harness = new_decoder();
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(harness->element);
  GstBuffer *output;
  guint i;

  configure_mpeg4_codec(self, CRYSTALHD_MPEG4_OBJECT_TYPE_SIMPLE);
  mock.auto_output = FALSE;
  for (i = 0; i < 3; ++i)
    g_assert_cmpint(gst_harness_push(
        harness, new_mpeg4_input(i * 40 * GST_MSECOND)), ==, GST_FLOW_OK);
  queue_numbered_picture(mock.accepted[0], 1, 3);
  queue_numbered_picture(0, 2, 4);
  queue_numbered_picture(mock.accepted[2], 3, 5);
  g_assert_true(gst_harness_push_event(harness, gst_event_new_eos()));
  for (i = 0; i < 3; ++i) {
    output = gst_harness_try_pull(harness);
    check_output(output, i * 40 * GST_MSECOND, i + 1);
    gst_buffer_unref(output);
  }
  g_assert_cmpuint(self->timestamps.length, ==, 0);
  g_assert_cmpuint(gst_harness_buffers_in_queue(harness), ==, 0);
  gst_harness_teardown(harness);
}

static void
test_mpeg4_uncoded_vop(void)
{
  const CrystalHdMpeg4ObjectType profiles[] = {
    CRYSTALHD_MPEG4_OBJECT_TYPE_SIMPLE,
    CRYSTALHD_MPEG4_OBJECT_TYPE_ADVANCED_SIMPLE
  };
  guint i;

  for (i = 0; i < G_N_ELEMENTS(profiles); ++i) {
    GstHarness *harness = new_decoder();
    GstCrystalHdDec *self = GST_CRYSTALHD_DEC(harness->element);
    GstBuffer *output;
    guint64 next_timestamp;

    configure_mpeg4_codec(self, profiles[i]);
    mock.auto_output = FALSE;
    self->have_simple_picture_number = TRUE;
    self->simple_picture_number = 7;
    next_timestamp = self->next_hardware_timestamp;

    g_assert_cmpint(gst_harness_push(
        harness, new_mpeg4_vop(0, 0x10, 0x40)), ==, GST_FLOW_OK);
    g_assert_cmpuint(mock.input_calls, ==, 1);
    g_assert_cmpuint(mock.accepted_count, ==, 1);
    g_assert_cmpuint(mock.accepted[0], ==, next_timestamp);
    g_assert_cmpuint(self->next_hardware_timestamp, ==,
                     next_timestamp + CRYSTALHD_TIMESTAMP_STEP);
    g_assert_cmpuint(self->timestamps.length, ==, 0);
    g_assert_false(self->have_simple_picture_number);

    g_assert_cmpint(gst_harness_push(
        harness, new_mpeg4_vop(40 * GST_MSECOND, 0x50, 0x60)), ==,
        GST_FLOW_OK);
    g_assert_cmpuint(mock.input_calls, ==, 2);
    g_assert_cmpuint(mock.accepted_count, ==, 2);
    g_assert_cmpuint(mock.accepted[1], ==,
                     next_timestamp + CRYSTALHD_TIMESTAMP_STEP);
    g_assert_cmpuint(self->next_hardware_timestamp, ==,
                     next_timestamp + 2 * CRYSTALHD_TIMESTAMP_STEP);
    g_assert_cmpuint(self->timestamps.length, ==, 1);

    queue_numbered_picture(mock.accepted[1], 2, 1);
    g_assert_cmpint(gst_crystalhd_receive_available(self), ==, GST_FLOW_OK);
    output = gst_harness_try_pull(harness);
    check_output(output, 40 * GST_MSECOND, 2);
    gst_buffer_unref(output);
    g_assert_cmpuint(self->timestamps.length, ==, 0);
    gst_harness_teardown(harness);
  }
}

static void
test_mpeg4_simple_invalid_ordinal_baseline(void)
{
  const guint32 broken_ordinals[] = { 0, G_MAXUINT32 };
  guint i;

  for (i = 0; i < G_N_ELEMENTS(broken_ordinals); ++i) {
    GstHarness *harness = new_decoder();
    GstCrystalHdDec *self = GST_CRYSTALHD_DEC(harness->element);
    GstBuffer *output;
    guint32 candidate = broken_ordinals[i] == 0 ? 4 : 1;
    guint input;

    configure_mpeg4_codec(self, CRYSTALHD_MPEG4_OBJECT_TYPE_SIMPLE);
    mock.auto_output = FALSE;
    for (input = 0; input < 3; ++input)
      g_assert_cmpint(gst_harness_push(
          harness, new_mpeg4_input(input * 40 * GST_MSECOND)), ==,
          GST_FLOW_OK);

    queue_numbered_picture(mock.accepted[0], 1, 3);
    g_assert_cmpint(gst_crystalhd_receive_available(self), ==, GST_FLOW_OK);
    output = gst_harness_try_pull(harness);
    check_output(output, 0, 1);
    gst_buffer_unref(output);
    g_assert_true(self->have_simple_picture_number);

    queue_numbered_picture(mock.accepted[1], 2, broken_ordinals[i]);
    g_assert_cmpint(gst_crystalhd_receive_available(self), ==, GST_FLOW_OK);
    output = gst_harness_try_pull(harness);
    check_output(output, 40 * GST_MSECOND, 2);
    gst_buffer_unref(output);
    g_assert_false(self->have_simple_picture_number);

    queue_numbered_picture(0, 0xee, candidate);
    g_assert_cmpint(gst_crystalhd_receive_available(self), ==, GST_FLOW_OK);
    g_assert_cmpuint(self->timestamps.length, ==, 1);
    g_assert_cmpuint(gst_harness_buffers_in_queue(harness), ==, 0);

    queue_numbered_picture(mock.accepted[2], 3, candidate);
    g_assert_cmpint(gst_crystalhd_receive_available(self), ==, GST_FLOW_OK);
    output = gst_harness_try_pull(harness);
    check_output(output, 80 * GST_MSECOND, 3);
    gst_buffer_unref(output);
    gst_harness_teardown(harness);
  }
}

static void
test_mpeg4_simple_order_violation(void)
{
  GstHarness *harness = new_decoder();
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(harness->element);

  configure_mpeg4_codec(self, CRYSTALHD_MPEG4_OBJECT_TYPE_SIMPLE);
  mock.auto_output = FALSE;
  g_assert_cmpint(gst_harness_push(harness, new_mpeg4_input(0)),
                  ==, GST_FLOW_OK);
  g_assert_cmpint(gst_harness_push(
      harness, new_mpeg4_input(40 * GST_MSECOND)), ==, GST_FLOW_OK);
  queue_numbered_picture(mock.accepted[1], 2, 4);
  g_assert_cmpint(gst_crystalhd_receive_available(self), ==, GST_FLOW_ERROR);
  g_assert_cmpuint(self->timestamps.length, ==, 2);
  g_assert_cmpuint(gst_harness_buffers_in_queue(harness), ==, 0);
  gst_harness_teardown(harness);
}

static void
test_mpeg4_simple_duplicate_zero_timestamp(void)
{
  GstHarness *harness = new_decoder();
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(harness->element);
  GstBuffer *output;

  configure_mpeg4_codec(self, CRYSTALHD_MPEG4_OBJECT_TYPE_SIMPLE);
  mock.auto_output = FALSE;
  g_assert_cmpint(gst_harness_push(harness, new_mpeg4_input(0)),
                  ==, GST_FLOW_OK);
  g_assert_cmpint(gst_harness_push(
      harness, new_mpeg4_input(40 * GST_MSECOND)), ==, GST_FLOW_OK);
  queue_numbered_picture(mock.accepted[0], 1, 3);
  g_assert_cmpint(gst_crystalhd_receive_available(self), ==, GST_FLOW_OK);
  output = gst_harness_try_pull(harness);
  check_output(output, 0, 1);
  gst_buffer_unref(output);

  /* A repeated ordinal is not the next Simple picture, even if its missing
   * timestamp would otherwise resemble the BCM70015 metadata-loss case. */
  queue_numbered_picture(0, 0xee, 3);
  g_assert_cmpint(gst_crystalhd_receive_available(self), ==, GST_FLOW_OK);
  g_assert_cmpuint(self->timestamps.length, ==, 1);
  g_assert_cmpuint(gst_harness_buffers_in_queue(harness), ==, 0);
  queue_numbered_picture(mock.accepted[1], 2, 4);
  g_assert_cmpint(gst_crystalhd_receive_available(self), ==, GST_FLOW_OK);
  output = gst_harness_try_pull(harness);
  check_output(output, 40 * GST_MSECOND, 2);
  gst_buffer_unref(output);
  gst_harness_teardown(harness);
}

static void
test_mpeg4_unrecoverable_zero_timestamp(void)
{
  GstHarness *harness = new_decoder();
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(harness->element);
  GstBuffer *output;

  configure_mpeg4_codec(self,
                        CRYSTALHD_MPEG4_OBJECT_TYPE_ADVANCED_SIMPLE);
  mock.auto_output = FALSE;
  g_assert_cmpint(gst_harness_push(harness, new_mpeg4_input(0)),
                  ==, GST_FLOW_OK);
  queue_numbered_picture(0, 0xee, 3);
  g_assert_cmpint(gst_crystalhd_receive_available(self), ==, GST_FLOW_OK);
  g_assert_cmpuint(self->timestamps.length, ==, 1);
  g_assert_cmpuint(gst_harness_buffers_in_queue(harness), ==, 0);
  queue_numbered_picture(mock.accepted[0], 1, 3);
  g_assert_cmpint(gst_crystalhd_receive_available(self), ==, GST_FLOW_OK);
  output = gst_harness_try_pull(harness);
  check_output(output, 0, 1);
  gst_buffer_unref(output);
  gst_harness_teardown(harness);
}

static void
test_input_admission_timeout(void)
{
  GstHarness *harness = new_decoder();
  mock.required_bytes = 232;
  mock.free_bytes = 0;
  g_assert_cmpint(gst_harness_push(harness, new_input(0)), ==, GST_FLOW_ERROR);
  g_assert_cmpuint(mock.input_calls, ==, 0);
  g_assert_cmpuint(mock.unsafe_submissions, ==, 0);
  g_assert_cmpint(lifecycle_now, >=, 10 * G_USEC_PER_SEC);
  g_assert_cmpint(lifecycle_now, <=, 10 * G_USEC_PER_SEC + 1);
  g_assert_cmpuint(mock.destroyed_inputs, ==, 1);
  gst_harness_teardown(harness);
}

static void
test_input_admission_flush(void)
{
  GstHarness *harness = new_decoder();
  GstSegment segment;
  GstBuffer *output;
  mock.required_bytes = 232;
  mock.free_bytes = 0;
  flush_during_wait = harness;
  g_assert_cmpint(gst_harness_push(harness, new_input(10 * GST_SECOND)),
                  ==, GST_FLOW_FLUSHING);
  g_assert_cmpuint(mock.input_calls, ==, 0);
  g_assert_cmpuint(mock.unsafe_submissions, ==, 0);
  g_assert_cmpint(lifecycle_now, <, 10 * G_USEC_PER_SEC);
  g_assert_cmpuint(mock.destroyed_inputs, ==, 1);
  /* FLUSH_STOP is serialized: only issue it after the interrupted base
   * chain has returned. New input then belongs to a fresh timeline. */
  g_assert_true(gst_harness_push_event(harness, gst_event_new_flush_stop(TRUE)));
  gst_segment_init(&segment, GST_FORMAT_TIME);
  g_assert_true(gst_harness_push_event(harness, gst_event_new_segment(&segment)));
  mock.free_bytes = 1024 * 1024;
  g_assert_cmpint(gst_harness_push(harness, new_input(0)), ==, GST_FLOW_OK);
  output = gst_harness_try_pull(harness);
  check_output(output, 0, 1);
  gst_buffer_unref(output);
  gst_harness_teardown(harness);
  g_assert_cmpuint(mock.destroyed_inputs, ==, 2);
}

static void
test_input_wait_new_generation(void)
{
  GstHarness *harness = new_decoder();
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(harness->element);
  guint64 generation = self->generation;
  GstVideoCodecFrame *retained;
  GstBuffer *output;
  mock.auto_output = FALSE;
  g_assert_cmpint(gst_harness_push(harness, new_input(10 * GST_SECOND)),
                  ==, GST_FLOW_OK);
  retained = gst_video_decoder_get_frame(GST_VIDEO_DECODER(self), 0);
  g_assert_nonnull(retained);
  /* Exercise the subclass's generation-canceled cleanup with an explicitly
   * owned frame. Do not recursively flush inside a base chain callback:
   * that would bypass the real sink-pad stream serialization/current_frame
   * ownership. The original base callback has already returned here.
   */
  mock.accepted_count = 0;
  mock.required_bytes = 232;
  mock.free_bytes = 0;
  seek_during_wait = harness;
  GST_VIDEO_DECODER_STREAM_LOCK(self);
  g_assert_cmpint(gst_crystalhd_handle_frame(GST_VIDEO_DECODER(self), retained),
                  ==, GST_FLOW_FLUSHING);
  GST_VIDEO_DECODER_STREAM_UNLOCK(self);
  g_assert_cmpuint(self->generation, >, generation);
  g_assert_cmpuint(mock.accepted_count, ==, 2);
  g_assert_cmpuint(mock.destroyed_inputs, ==, 1);
  queue_picture(mock.accepted[0], 1);
  queue_picture(mock.accepted[1], 2);
  g_assert_cmpint(gst_crystalhd_receive_available(self), ==, GST_FLOW_OK);
  output = gst_harness_try_pull(harness);
  check_output(output, 0, 1);
  gst_buffer_unref(output);
  output = gst_harness_try_pull(harness);
  check_output(output, 40 * GST_MSECOND, 2);
  gst_buffer_unref(output);
  gst_harness_teardown(harness);
  g_assert_cmpuint(mock.destroyed_inputs, ==, 3);
}

static void
test_eos_admission(void)
{
  GstHarness *harness = new_decoder();
  GstBuffer *output;
  mock.auto_output = FALSE;
  g_assert_cmpint(gst_harness_push(harness, new_input(0)), ==, GST_FLOW_OK);
  queue_picture(mock.accepted[0], 1);
  mock.required_bytes = 1024;
  mock.free_bytes = 1023;
  mock.release_makes_room = TRUE;
  g_assert_cmpint(gst_crystalhd_drain(GST_VIDEO_DECODER(harness->element)),
                  ==, GST_FLOW_OK);
  g_assert_cmpuint(mock.unsafe_submissions, ==, 0);
  g_assert_cmpuint(mock.drain_calls, ==, 1);
  output = gst_harness_try_pull(harness);
  check_output(output, 0, 1);
  gst_buffer_unref(output);
  gst_harness_teardown(harness);
}

static void
test_oversized_input_admission(void)
{
  GstHarness *harness = new_decoder();
  GstBuffer *input = gst_buffer_new_allocate(NULL, 1024 * 1024, NULL);
  GST_BUFFER_PTS(input) = 0;
  g_assert_cmpint(gst_harness_push(harness, input), ==, GST_FLOW_ERROR);
  g_assert_cmpuint(mock.input_calls, ==, 0);
  g_assert_cmpuint(GST_CRYSTALHD_DEC(harness->element)->timestamps.length, ==, 0);
  gst_harness_teardown(harness);
}

static void
test_input_reservation_boundaries(void)
{
  const BC_MEDIA_SUBTYPE codecs[] = {
    BC_MSUBTYPE_H264, BC_MSUBTYPE_MPEG2VIDEO, BC_MSUBTYPE_VC1,
    BC_MSUBTYPE_WVC1, BC_MSUBTYPE_WMV3, BC_MSUBTYPE_DIVX
  };
  const gsize sizes[] = { 1, 59999, 60000, 60001, 65512, 65513, 131024,
                          512 * 1024, 1024 * 1024 - 1024 };
  gsize reservation;
  gsize maximum = GST_CRYSTALHD_INPUT_CAPACITY;
  guint i, j;

  for (i = 0; i < G_N_ELEMENTS(codecs); i++) {
    g_assert_false(gst_crystalhd_input_reservation(codecs[i], 0, 0, &reservation));
    g_assert_false(gst_crystalhd_input_reservation(codecs[i], G_MAXSIZE, 0,
                                                  &reservation));
    g_assert_false(gst_crystalhd_input_reservation(codecs[i], 1, G_MAXSIZE,
                                                  &reservation));
    g_assert_false(gst_crystalhd_input_reservation(codecs[i], 1024 * 1024, 0,
                                                  &reservation));
    for (j = 0; j < G_N_ELEMENTS(sizes); j++) {
      gsize expanded = sizes[j];
      gsize exact_pes_upper_bound;
      gsize metadata = codecs[i] == BC_MSUBTYPE_WVC1 ? 16 : 0;
      if (codecs[i] == BC_MSUBTYPE_DIVX)
        metadata = 47;
      if (codecs[i] == BC_MSUBTYPE_WVC1)
        expanded += 4 + metadata;
      if (codecs[i] == BC_MSUBTYPE_WMV3)
        expanded += 48 + 32;
      if (codecs[i] == BC_MSUBTYPE_DIVX)
        expanded += metadata;
      /* Independently use the actual maximum PES header and minimum full
       * payload, plus up to3 call boundaries/SPES headers, not helper constants.
       */
      exact_pes_upper_bound = expanded +
          14 * (expanded / 65512 + 3) + 3 * 41;
      g_assert_true(gst_crystalhd_input_reservation(codecs[i], sizes[j], metadata,
                                                   &reservation));
      g_assert_cmpuint(reservation, >=, exact_pes_upper_bound);
      g_assert_cmpuint(reservation, <=, GST_CRYSTALHD_INPUT_CAPACITY);
    }
  }
  /* Largest whole H264 AU is accepted; the next byte is rejected, without
   * allocating either picture. Metadata-heavy WVC1 must account for both calls.
   */
  while (!gst_crystalhd_input_reservation(BC_MSUBTYPE_H264, maximum, 0,
                                          &reservation))
    maximum--;
  g_assert_cmpuint(reservation, ==, GST_CRYSTALHD_INPUT_CAPACITY);
  g_assert_false(gst_crystalhd_input_reservation(BC_MSUBTYPE_H264, maximum + 1,
                                                0, &reservation));
  g_assert_true(gst_crystalhd_input_reservation(BC_MSUBTYPE_WVC1, 400000, 400000,
                                               &reservation));
  g_assert_cmpuint(reservation, >, 800000);
  g_assert_false(gst_crystalhd_input_reservation(BC_MSUBTYPE_WVC1, 600000,
                                                600000, &reservation));
  g_assert_true(gst_crystalhd_input_reservation(BC_MSUBTYPE_DIVX, 400000,
                                               400000, &reservation));
  g_assert_cmpuint(reservation, >, 800000);
  g_assert_false(gst_crystalhd_input_reservation(BC_MSUBTYPE_DIVX, 600000,
                                                600000, &reservation));
  g_assert_false(gst_crystalhd_input_reservation(BC_MSUBTYPE_AVC1, 1, 0,
                                                &reservation));
}

static void
test_metadata_admission(void)
{
  GstHarness *harness = new_decoder();
  GstVideoCodecState *state = g_new0(GstVideoCodecState, 1);
  GstBuffer *metadata = gst_buffer_new_allocate(NULL, 1024 * 1024, NULL);
  const guint8 sequence[] = { 0, 0, 1, 0x0f, 0, 0, 0, 0 };
  guint opened = mock.open_calls;
  gst_buffer_fill(metadata, 0, sequence, sizeof(sequence));
  state->ref_count = 1;
  gst_video_info_init(&state->info);
  state->caps = gst_caps_new_simple("video/x-wmv", "wmvversion", G_TYPE_INT, 3,
      "format", G_TYPE_STRING, "WVC1", "stream-format", G_TYPE_STRING, "asf",
      "header-format", G_TYPE_STRING, "asf", "codec_data", GST_TYPE_BUFFER,
      metadata, NULL);
  gst_buffer_unref(metadata);
  g_assert_false(gst_crystalhd_set_format(GST_VIDEO_DECODER(harness->element), state));
  g_assert_cmpuint(mock.open_calls, ==, opened);
  gst_video_codec_state_unref(state);
  gst_harness_teardown(harness);
}

static void
test_eos_with_ready_output(void)
{
  GstHarness *harness = new_decoder();
  guint i;
  mock.auto_output = FALSE;
  for (i = 0; i < 3; i++)
    g_assert_cmpint(gst_harness_push(harness, new_input(i * 40 * GST_MSECOND)),
                    ==, GST_FLOW_OK);
  for (i = 0; i < 3; i++)
    queue_picture(mock.accepted[i], i + 1);
  /* The real library reports driver eosDetected independently of RLL size. */
  mock.early_eos = TRUE;
  g_assert_cmpint(gst_crystalhd_drain(GST_VIDEO_DECODER(harness->element)),
                  ==, GST_FLOW_OK);
  for (i = 0; i < 3; i++) {
    GstBuffer *output = gst_harness_try_pull(harness);
    check_output(output, i * 40 * GST_MSECOND, i + 1);
    gst_buffer_unref(output);
  }
  g_assert_cmpuint(mock.destroyed_inputs, ==, 3);
  g_assert_cmpuint(mock.release_calls, ==, 3);
  gst_harness_teardown(harness);
}

static void
test_eos_before_late_output(void)
{
  GstHarness *harness = new_decoder();
  GstBuffer *output;
  mock.auto_output = FALSE;
  g_assert_cmpint(gst_harness_push(harness, new_input(0)), ==, GST_FLOW_OK);
  /* Driver EOS status and RLL publication are independent observations.
   * Make the accepted picture visible only after the first EOS poll.
   */
  mock.early_eos = TRUE;
  mock.publish_after_eos = TRUE;
  g_assert_cmpint(gst_crystalhd_drain(GST_VIDEO_DECODER(harness->element)),
                  ==, GST_FLOW_OK);
  output = gst_harness_try_pull(harness);
  check_output(output, 0, 1);
  gst_buffer_unref(output);
  g_assert_cmpuint(mock.destroyed_inputs, ==, 1);
  g_assert_cmpuint(mock.release_calls, ==, 1);
  gst_harness_teardown(harness);
}

static void
test_drain_completion_requires_idle(void)
{
  GstHarness *harness = new_decoder();
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(harness->element);
  g_assert_true(g_queue_is_empty(&self->timestamps));
  self->output_eos = TRUE;
  self->need_second_field = FALSE;
  self->drain_idle = FALSE;
  g_assert_false(gst_crystalhd_drain_complete(self, FALSE));
  self->drain_idle = TRUE;
  g_assert_true(gst_crystalhd_drain_complete(self, FALSE));
  g_assert_false(gst_crystalhd_drain_complete(self, TRUE));
  gst_harness_teardown(harness);
}

static void
test_incomplete_drain(void)
{
  GstHarness *harness = new_decoder();
  mock.auto_output = FALSE;
  g_assert_cmpint(gst_harness_push(harness, new_input(0)), ==, GST_FLOW_OK);
  /* A backend EOS indication cannot turn a missing picture into success. */
  g_assert_cmpint(gst_crystalhd_drain(GST_VIDEO_DECODER(harness->element)),
                  ==, GST_FLOW_ERROR);
  g_assert_cmpuint(GST_CRYSTALHD_DEC(harness->element)->timestamps.length, ==, 1);
  gst_harness_teardown(harness);
  g_assert_cmpuint(mock.destroyed_inputs, ==, 1);
}

static void
queue_field(guint64 timestamp, guint8 pixel, gboolean bottom)
{
  MockPicture *picture;
  queue_picture(timestamp, pixel);
  picture = g_queue_peek_tail(&mock.pictures);
  picture->flags = VDEC_FLAG_INTERLACED_SRC |
                   (bottom ? VDEC_FLAG_BOTTOMFIELD : VDEC_FLAG_TOPFIELD);
}

static void
queue_whole_weave(guint64 timestamp, guint8 pixel, gboolean bottom_first,
                   guint fault)
{
  MockPicture *picture;
  queue_picture_geometry(timestamp, pixel, 71, 16, 16,
      VDEC_FLAG_INTERLACED_SRC | VDEC_FLAG_UNKNOWN_SRC | VDEC_FLAG_TOPFIELD |
      (bottom_first ? VDEC_FLAG_BOTTOM_FIRST : 0));
  picture = g_queue_peek_tail(&mock.pictures);
  picture->whole_weave = TRUE;
  picture->weave_fault = fault;
}

static void
assert_whole_weave_buffer(GstBuffer *output, GstClockTime pts,
                          guint8 pixel, gboolean bottom_first)
{
  guint8 bytes[16 * 16 * 2];
  guint i;
  g_assert_nonnull(output);
  g_assert_cmpuint(gst_buffer_get_size(output), ==, sizeof(bytes));
  g_assert_cmpuint(GST_BUFFER_PTS(output), ==, pts);
  g_assert_cmpuint(GST_BUFFER_DURATION(output), ==, 40 * GST_MSECOND);
  g_assert_true(GST_BUFFER_FLAG_IS_SET(output, GST_VIDEO_BUFFER_FLAG_INTERLACED));
  g_assert_cmpint(GST_BUFFER_FLAG_IS_SET(output, GST_VIDEO_BUFFER_FLAG_TFF),
                  ==, !bottom_first);
  g_assert_false(GST_BUFFER_FLAG_IS_SET(output, GST_VIDEO_BUFFER_FLAG_ONEFIELD));
  g_assert_false(GST_BUFFER_FLAG_IS_SET(output, GST_VIDEO_BUFFER_FLAG_RFF));
  g_assert_cmpuint(gst_buffer_extract(output, 0, bytes, sizeof(bytes)),
                   ==, sizeof(bytes));
  for (i = 0; i < sizeof(bytes); ++i)
    g_assert_cmpuint(bytes[i], ==, pixel + i / 32);
}

static void
test_whole_weave_output(void)
{
  guint bottom_first;
  for (bottom_first = 0; bottom_first < 2; ++bottom_first) {
    GstHarness *harness = new_decoder();
    GstCrystalHdDec *self = GST_CRYSTALHD_DEC(harness->element);
    GstBuffer *output;
    const GstClockTime pts = GST_SECOND + 7 * GST_MSECOND;
    mock.auto_output = FALSE;
    g_assert_cmpint(gst_harness_push(harness, new_input(pts)), ==, GST_FLOW_OK);
    queue_whole_weave(mock.accepted[0], 0x30, bottom_first, 0);
    g_assert_cmpint(gst_crystalhd_receive_available(self), ==, GST_FLOW_OK);
    output = gst_harness_try_pull(harness);
    assert_whole_weave_buffer(output, pts, 0x30, bottom_first);
    g_assert_cmpuint(self->timestamps.length, ==, 0);
    g_assert_false(self->need_second_field);
    g_assert_cmpuint(mock.release_calls, ==, 1);
    g_assert_cmpuint(mock.destroyed_inputs, ==, 1);
    g_assert_true(gst_harness_push_event(harness, gst_event_new_eos()));
    g_assert_cmpuint(gst_harness_buffers_in_queue(harness), ==, 0);
    gst_harness_teardown(harness);
    /* The completed lease was poisoned before delivery; close must not
     * invalidate the application's independent full-height copy either. */
    assert_whole_weave_buffer(output, pts, 0x30, bottom_first);
    gst_buffer_unref(output);
  }
}

static void
test_whole_weave_rejects_malformed(void)
{
  guint fault;
  for (fault = 1; fault <= 19; ++fault) {
    GstHarness *harness = new_decoder();
    GstCrystalHdDec *self = GST_CRYSTALHD_DEC(harness->element);
    GstVideoCodecFrame *frame;
    mock.auto_output = FALSE;
    g_assert_cmpint(gst_harness_push(harness, new_input(0)), ==, GST_FLOW_OK);
    queue_whole_weave(mock.accepted[0], 0x30, FALSE, fault);
    g_assert_cmpint(gst_crystalhd_receive_available(self), ==, GST_FLOW_ERROR);
    g_assert_cmpuint(gst_harness_buffers_in_queue(harness), ==, 0);
    g_assert_cmpuint(self->timestamps.length, ==, 1);
    g_assert_false(self->need_second_field);
    g_assert_cmpuint(mock.release_calls, ==, 1);
    frame = gst_video_decoder_get_frame(GST_VIDEO_DECODER(self), 0);
    g_assert_nonnull(frame);
    g_assert_null(frame->output_buffer);
    gst_video_codec_frame_unref(frame);
    gst_harness_teardown(harness);
    g_assert_cmpuint(mock.destroyed_inputs, ==, 1);
  }
}

static void
test_whole_weave_rejects_pending_half(void)
{
  GstHarness *harness = new_decoder();
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(harness->element);
  mock.auto_output = FALSE;
  g_assert_cmpint(gst_harness_push(harness, new_input(0)), ==, GST_FLOW_OK);
  queue_field(mock.accepted[0], 0x11, FALSE);
  g_assert_cmpint(gst_crystalhd_receive_available(self), ==, GST_FLOW_OK);
  g_assert_true(self->need_second_field);
  g_assert_cmpuint(self->timestamps.length, ==, 1);
  g_assert_cmpuint(gst_harness_buffers_in_queue(harness), ==, 0);
  g_assert_cmpuint(mock.release_calls, ==, 1);
  queue_whole_weave(mock.accepted[0], 0x30, FALSE, 0);
  g_assert_cmpint(gst_crystalhd_receive_available(self), ==, GST_FLOW_ERROR);
  g_assert_cmpuint(gst_harness_buffers_in_queue(harness), ==, 0);
  g_assert_cmpuint(self->timestamps.length, ==, 1);
  g_assert_cmpuint(mock.release_calls, ==, 2);
  gst_harness_teardown(harness);
  g_assert_cmpuint(mock.destroyed_inputs, ==, 1);
}

static void
test_whole_weave_token_lifetime(void)
{
  guint scenario;
  for (scenario = 0; scenario < 3; ++scenario) {
    GstHarness *harness = new_decoder();
    GstCrystalHdDec *self = GST_CRYSTALHD_DEC(harness->element);
    GstBuffer *held = NULL;
    guint64 stale;
    guint64 expected_token;
    guint releases;
    mock.auto_output = FALSE;
    g_assert_cmpint(gst_harness_push(harness, new_input(0)), ==, GST_FLOW_OK);
    stale = mock.accepted[0];
    if (scenario == 0) {
      queue_whole_weave(stale, 0x30, FALSE, 0);
      g_assert_cmpint(gst_crystalhd_receive_available(self), ==, GST_FLOW_OK);
      held = gst_harness_try_pull(harness);
      assert_whole_weave_buffer(held, 0, 0x30, FALSE);
      g_assert_cmpint(gst_harness_push(harness, new_input(40 * GST_MSECOND)),
                      ==, GST_FLOW_OK);
      expected_token = mock.accepted[1];
    } else if (scenario == 1) {
      /* Valid embedded metadata cannot bind an otherwise unsubmitted token. */
      stale += CRYSTALHD_TIMESTAMP_STEP;
      expected_token = mock.accepted[0];
    } else {
      GstSegment segment;
      flush_decoder(harness);
      gst_segment_init(&segment, GST_FORMAT_TIME);
      g_assert_true(gst_harness_push_event(harness, gst_event_new_segment(&segment)));
      g_assert_cmpint(gst_harness_push(harness, new_input(40 * GST_MSECOND)),
                      ==, GST_FLOW_OK);
      expected_token = mock.accepted[1];
      g_assert_cmpuint(expected_token, >, stale);
    }
    releases = mock.release_calls;
    queue_whole_weave(stale, 0x80, TRUE, 0);
    g_assert_cmpint(gst_crystalhd_receive_available(self), ==, GST_FLOW_ERROR);
    g_assert_cmpuint(gst_harness_buffers_in_queue(harness), ==, 0);
    g_assert_false(self->need_second_field);
    g_assert_cmpuint(self->timestamps.length, ==, 1);
    g_assert_cmpuint(((CrystalHdTimestamp *)self->timestamps.head->data)->hardware_timestamp,
                     ==, expected_token);
    g_assert_cmpuint(mock.release_calls, ==, releases + 1);
    if (held != NULL)
      assert_whole_weave_buffer(held, 0, 0x30, FALSE);
    gst_harness_teardown(harness);
    if (held != NULL) {
      assert_whole_weave_buffer(held, 0, 0x30, FALSE);
      gst_buffer_unref(held);
    }
  }
}

static void
test_whole_weave_70012_unchanged(void)
{
  GstHarness *harness = new_decoder_with_caps_and_device(
      "video/x-h264,stream-format=byte-stream,alignment=au,parsed=true,"
      "width=16,height=16,framerate=25/1", 0);
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(harness->element);
  GstBuffer *output;
  guint8 bytes[16 * 16 * 2];
  guint i;
  mock.auto_output = FALSE;
  g_assert_cmpint(gst_harness_push(harness, new_input(0)), ==, GST_FLOW_OK);
  queue_field(mock.accepted[0], 0x11, FALSE);
  queue_field(mock.accepted[0], 0x22, TRUE);
  g_assert_cmpint(gst_crystalhd_receive_available(self), ==, GST_FLOW_OK);
  output = gst_harness_try_pull(harness);
  g_assert_nonnull(output);
  g_assert_true(GST_BUFFER_FLAG_IS_SET(output, GST_VIDEO_BUFFER_FLAG_INTERLACED));
  g_assert_true(GST_BUFFER_FLAG_IS_SET(output, GST_VIDEO_BUFFER_FLAG_TFF));
  g_assert_cmpuint(gst_buffer_extract(output, 0, bytes, sizeof(bytes)),
                   ==, sizeof(bytes));
  for (i = 0; i < sizeof(bytes); ++i)
    g_assert_cmpuint(bytes[i], ==, (i / 32) % 2 ? 0x22 : 0x11);
  g_assert_cmpuint(self->timestamps.length, ==, 0);
  g_assert_false(self->need_second_field);
  g_assert_cmpuint(mock.release_calls, ==, 2);
  gst_buffer_unref(output);
  gst_harness_teardown(harness);

  harness = new_decoder_with_caps_and_device(
      "video/x-h264,stream-format=byte-stream,alignment=au,parsed=true,"
      "width=16,height=16,framerate=25/1", 0);
  self = GST_CRYSTALHD_DEC(harness->element);
  mock.auto_output = FALSE;
  g_assert_cmpint(gst_harness_push(harness, new_input(0)), ==, GST_FLOW_OK);
  queue_whole_weave(mock.accepted[0], 0x30, FALSE, 0);
  g_assert_cmpint(gst_crystalhd_receive_available(self), ==, GST_FLOW_ERROR);
  g_assert_cmpuint(gst_harness_buffers_in_queue(harness), ==, 0);
  g_assert_false(self->need_second_field);
  g_assert_cmpuint(self->timestamps.length, ==, 1);
  g_assert_cmpuint(mock.release_calls, ==, 1);
  gst_harness_teardown(harness);
}

static void
test_field_identity(void)
{
  GstHarness *harness = new_decoder();
  GstBuffer *output;
  guint8 pixels[16 * 16 * 2];
  guint i;
  mock.auto_output = FALSE;
  g_assert_cmpint(gst_harness_push(harness, new_input(0)), ==, GST_FLOW_OK);
  queue_field(mock.accepted[0], 0x11, FALSE);
  queue_field(mock.accepted[0], 0x22, TRUE);
  g_assert_true(gst_harness_push_event(harness, gst_event_new_eos()));
  output = gst_harness_try_pull(harness);
  g_assert_nonnull(output);
  g_assert_cmpuint(GST_BUFFER_PTS(output), ==, 0);
  g_assert_cmpuint(gst_buffer_extract(output, 0, pixels, sizeof(pixels)), ==,
                   sizeof(pixels));
  for (i = 0; i < sizeof(pixels); i++)
    g_assert_cmpuint(pixels[i], ==, (i / 32) % 2 ? 0x22 : 0x11);
  gst_buffer_unref(output);
  g_assert_cmpuint(mock.destroyed_inputs, ==, 1);
  g_assert_cmpuint(mock.release_calls, ==, 2);
  gst_harness_teardown(harness);

  harness = new_decoder();
  mock.auto_output = FALSE;
  g_assert_cmpint(gst_harness_push(harness, new_input(0)), ==, GST_FLOW_OK);
  g_assert_cmpint(gst_harness_push(harness, new_input(40 * GST_MSECOND)),
                  ==, GST_FLOW_OK);
  queue_field(mock.accepted[0], 0x11, FALSE);
  queue_field(mock.accepted[1], 0x22, TRUE);
  g_assert_cmpint(gst_crystalhd_receive_available(GST_CRYSTALHD_DEC(harness->element)),
                  ==, GST_FLOW_ERROR);
  g_assert_cmpuint(gst_harness_buffers_in_queue(harness), ==, 0);
  g_assert_cmpuint(mock.release_calls, ==, 2);
  gst_harness_teardown(harness);
  g_assert_cmpuint(mock.destroyed_inputs, ==, 2);

  harness = new_decoder();
  mock.auto_output = FALSE;
  g_assert_cmpint(gst_harness_push(harness, new_input(0)), ==, GST_FLOW_OK);
  queue_field(mock.accepted[0] + 123, 0xee, FALSE);
  g_assert_cmpint(gst_crystalhd_receive_available(GST_CRYSTALHD_DEC(harness->element)),
                  ==, GST_FLOW_ERROR);
  g_assert_cmpuint(gst_harness_buffers_in_queue(harness), ==, 0);
  g_assert_cmpuint(mock.release_calls, ==, 1);
  gst_harness_teardown(harness);
  g_assert_cmpuint(mock.destroyed_inputs, ==, 1);
}

static void
test_field_pair_policy(void)
{
  guint bottom_first, fault;
  for (bottom_first = 0; bottom_first < 2; ++bottom_first) {
    for (fault = 0; fault < 4; ++fault) {
      GstHarness *harness = new_decoder();
      MockPicture *second;
      GstFlowReturn flow;
      mock.auto_output = FALSE;
      g_assert_cmpint(gst_harness_push(harness, new_input(0)), ==, GST_FLOW_OK);
      queue_field(mock.accepted[0], 0x11, bottom_first);
      queue_field(mock.accepted[0], 0x22, fault == 1 ? bottom_first : !bottom_first);
      if (bottom_first) {
        ((MockPicture *)g_queue_peek_head(&mock.pictures))->flags |= VDEC_FLAG_BOTTOM_FIRST;
        ((MockPicture *)g_queue_peek_tail(&mock.pictures))->flags |= VDEC_FLAG_BOTTOM_FIRST;
      }
      second = g_queue_peek_tail(&mock.pictures);
      /* Smaller geometry still fits the already allocated full-size output.
       * It must not complete a frame with unwritten/mis-strided rows. */
      if (fault == 2) second->width = 8;
      if (fault == 3) second->height = 8;
      flow = gst_crystalhd_receive_available(GST_CRYSTALHD_DEC(harness->element));
      if (fault != 0) {
        g_assert_cmpint(flow, ==, GST_FLOW_ERROR);
        g_assert_cmpuint(gst_harness_buffers_in_queue(harness), ==, 0);
        g_assert_cmpuint(GST_CRYSTALHD_DEC(harness->element)->timestamps.length, ==, 1);
      } else {
        GstBuffer *output = gst_harness_try_pull(harness);
        guint8 pixels[sizeof(mock.pixels)];
        guint i;
        g_assert_cmpint(flow, ==, GST_FLOW_OK);
        g_assert_nonnull(output);
        g_assert_cmpuint(GST_BUFFER_PTS(output), ==, 0);
        g_assert_true(GST_BUFFER_FLAG_IS_SET(output, GST_VIDEO_BUFFER_FLAG_INTERLACED));
        g_assert_cmpint(GST_BUFFER_FLAG_IS_SET(output, GST_VIDEO_BUFFER_FLAG_TFF),
                        ==, !bottom_first);
        g_assert_false(GST_BUFFER_FLAG_IS_SET(output, GST_VIDEO_BUFFER_FLAG_ONEFIELD));
        g_assert_cmpuint(gst_buffer_extract(output, 0, pixels, sizeof(pixels)),
                         ==, sizeof(pixels));
        for (i = 0; i < sizeof(pixels); ++i) {
          gboolean bottom_row = (i / 32) % 2;
          g_assert_cmpuint(pixels[i], ==, bottom_row == (gboolean)bottom_first ? 0x11 : 0x22);
        }
        gst_buffer_unref(output);
      }
      g_assert_cmpuint(mock.release_calls, ==, 2);
      gst_harness_teardown(harness);
      g_assert_cmpuint(mock.destroyed_inputs, ==, 1);
    }
  }
}

static void
test_field_presentation_order(void)
{
  static const guint32 measured_pulldown[] = {
    vdecTopBottom, vdecBottomTop,
    vdecTopBottomTop, vdecBottomTopBottom,
  };
  guint capture_bottom;
  for (capture_bottom = 0; capture_bottom < 2; ++capture_bottom) {
    GstHarness *harness = new_decoder();
    GstCrystalHdDec *self = GST_CRYSTALHD_DEC(harness->element);
    guint phase;
    mock.auto_output = FALSE;
    for (phase = 0; phase < 3; ++phase) {
      const gboolean present_bottom = phase == 1;
      GstBuffer *output;
      GstVideoCodecFrame *frame;
      guint8 pixels[sizeof(mock.pixels)];
      guint byte;
      g_assert_cmpint(gst_harness_push(harness, new_input(phase * 40 * GST_MSECOND)),
                      ==, GST_FLOW_OK);
      frame = gst_video_decoder_get_frame(GST_VIDEO_DECODER(harness->element), phase);
      g_assert_nonnull(frame);
      g_assert_null(frame->output_buffer);
      frame->output_buffer = gst_buffer_new_allocate(NULL, sizeof(mock.pixels), NULL);
      GST_BUFFER_FLAG_SET(frame->output_buffer, GST_VIDEO_BUFFER_FLAG_INTERLACED);
      GST_BUFFER_FLAG_SET(frame->output_buffer, GST_VIDEO_BUFFER_FLAG_TFF);
      GST_BUFFER_FLAG_SET(frame->output_buffer, GST_VIDEO_BUFFER_FLAG_ONEFIELD);
      GST_BUFFER_FLAG_SET(frame->output_buffer, GST_VIDEO_BUFFER_FLAG_RFF);
      gst_video_codec_frame_unref(frame);
      queue_field(mock.accepted[phase], capture_bottom ? 0x22 : 0x11, capture_bottom);
      queue_field(mock.accepted[phase], capture_bottom ? 0x11 : 0x22, !capture_bottom);
      /* Pulldown metadata is meaningful here only as part of the existing
       * separate-field path. Even recognized progressive values must not
       * synthesize RFF or override field-pair presentation order. */
      ((MockPicture *)g_queue_peek_head(&mock.pictures))->pulldown =
          measured_pulldown[(phase * 2) % G_N_ELEMENTS(measured_pulldown)];
      ((MockPicture *)g_queue_peek_tail(&mock.pictures))->pulldown =
          measured_pulldown[(phase * 2 + 1) % G_N_ELEMENTS(measured_pulldown)];
      if (present_bottom) {
        ((MockPicture *)g_queue_peek_head(&mock.pictures))->flags |= VDEC_FLAG_BOTTOM_FIRST;
        ((MockPicture *)g_queue_peek_tail(&mock.pictures))->flags |= VDEC_FLAG_BOTTOM_FIRST;
      }
      /* Same caps, same capture order: the presentation order changes
       * TFF -> BFF -> TFF independently of which field arrives first. */
      g_assert_cmpint(gst_crystalhd_receive_available(GST_CRYSTALHD_DEC(harness->element)),
                      ==, GST_FLOW_OK);
      output = gst_harness_try_pull(harness);
      g_assert_nonnull(output);
      g_assert_cmpuint(GST_BUFFER_PTS(output), ==, phase * 40 * GST_MSECOND);
      g_assert_true(GST_BUFFER_FLAG_IS_SET(output, GST_VIDEO_BUFFER_FLAG_INTERLACED));
      g_assert_cmpint(GST_BUFFER_FLAG_IS_SET(output, GST_VIDEO_BUFFER_FLAG_TFF),
                      ==, !present_bottom);
      g_assert_false(GST_BUFFER_FLAG_IS_SET(output, GST_VIDEO_BUFFER_FLAG_ONEFIELD));
      g_assert_false(GST_BUFFER_FLAG_IS_SET(output, GST_VIDEO_BUFFER_FLAG_RFF));
      g_assert_false(self->progressive_field_sequence);
      g_assert_cmpuint(gst_buffer_extract(output, 0, pixels, sizeof(pixels)),
                       ==, sizeof(pixels));
      for (byte = 0; byte < sizeof(pixels); ++byte)
        g_assert_cmpuint(pixels[byte], ==, (byte / 32) % 2 ? 0x22 : 0x11);
      gst_buffer_unref(output);
    }
    g_assert_cmpuint(mock.successful_opens, ==, 1);
    g_assert_cmpuint(mock.release_calls, ==, 6);
    {
      GstVideoCodecFrame *frame;
      GstBuffer *output;
      g_assert_cmpint(gst_harness_push(harness, new_input(120 * GST_MSECOND)),
                      ==, GST_FLOW_OK);
      frame = gst_video_decoder_get_frame(GST_VIDEO_DECODER(harness->element), 3);
      g_assert_nonnull(frame);
      frame->output_buffer = gst_buffer_new_allocate(NULL, sizeof(mock.pixels), NULL);
      GST_BUFFER_FLAG_SET(frame->output_buffer, GST_VIDEO_BUFFER_FLAG_INTERLACED);
      GST_BUFFER_FLAG_SET(frame->output_buffer, GST_VIDEO_BUFFER_FLAG_TFF);
      GST_BUFFER_FLAG_SET(frame->output_buffer, GST_VIDEO_BUFFER_FLAG_ONEFIELD);
      GST_BUFFER_FLAG_SET(frame->output_buffer, GST_VIDEO_BUFFER_FLAG_RFF);
      gst_video_codec_frame_unref(frame);
      queue_picture(mock.accepted[3], 0x33);
      g_assert_cmpint(gst_crystalhd_receive_available(GST_CRYSTALHD_DEC(harness->element)),
                      ==, GST_FLOW_OK);
      output = gst_harness_try_pull(harness);
      g_assert_nonnull(output);
      check_output(output, 120 * GST_MSECOND, 0x33);
      g_assert_false(GST_BUFFER_FLAG_IS_SET(output, GST_VIDEO_BUFFER_FLAG_INTERLACED));
      g_assert_false(GST_BUFFER_FLAG_IS_SET(output, GST_VIDEO_BUFFER_FLAG_TFF));
      g_assert_false(GST_BUFFER_FLAG_IS_SET(output, GST_VIDEO_BUFFER_FLAG_ONEFIELD));
      g_assert_false(GST_BUFFER_FLAG_IS_SET(output, GST_VIDEO_BUFFER_FLAG_RFF));
      gst_buffer_unref(output);
    }
    gst_harness_teardown(harness);
  }
  {
    GstHarness *harness = new_decoder();
    GstCrystalHdDec *self = GST_CRYSTALHD_DEC(harness->element);
    mock.auto_output = FALSE;
    g_assert_cmpint(gst_harness_push(harness, new_input(0)), ==, GST_FLOW_OK);
    queue_field(mock.accepted[0], 0x11, FALSE);
    queue_field(mock.accepted[0], 0x22, TRUE);
    ((MockPicture *)g_queue_peek_tail(&mock.pictures))->flags |= VDEC_FLAG_BOTTOM_FIRST;
    g_assert_cmpint(gst_crystalhd_receive_available(self), ==, GST_FLOW_ERROR);
    g_assert_cmpuint(gst_harness_buffers_in_queue(harness), ==, 0);
    g_assert_cmpuint(self->timestamps.length, ==, 1);
    g_assert_cmpuint(mock.release_calls, ==, 2);
    gst_harness_teardown(harness);
  }
}

static void
test_output_interlace_mode_changes(void)
{
  GstHarness *harness = new_decoder();
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(harness->element);
  guint phase;
  GST_VIDEO_DECODER_STREAM_LOCK(self);
  for (phase = 0; phase < 3; ++phase) {
    gboolean interlaced = phase == 1;
    GstVideoInterlaceMode mode = interlaced ? GST_VIDEO_INTERLACE_MODE_MIXED :
                                             GST_VIDEO_INTERLACE_MODE_PROGRESSIVE;
    GstCaps *caps;
    GstVideoInfo info;
    g_assert_true(gst_crystalhd_configure_output(self, 16, 16, interlaced));
    g_assert_cmpint(GST_VIDEO_INFO_INTERLACE_MODE(&self->output_info), ==, mode);
    caps = gst_pad_get_current_caps(GST_VIDEO_DECODER_SRC_PAD(self));
    g_assert_nonnull(caps);
    g_assert_true(gst_video_info_from_caps(&info, caps));
    g_assert_cmpint(GST_VIDEO_INFO_INTERLACE_MODE(&info), ==, mode);
    gst_caps_unref(caps);
  }
  GST_VIDEO_DECODER_STREAM_UNLOCK(self);
  gst_harness_teardown(harness);
}

static void
queue_pulldown_picture(guint64 timestamp, guint8 pixel, guint32 pulldown)
{
  MockPicture *picture;
  queue_picture(timestamp, pixel);
  picture = g_queue_peek_tail(&mock.pictures);
  picture->pulldown = pulldown;
}

static void
seed_stale_video_flags(GstVideoDecoder *decoder, guint32 frame_number)
{
  GstVideoCodecFrame *frame = gst_video_decoder_get_frame(decoder, frame_number);
  g_assert_nonnull(frame);
  g_assert_null(frame->output_buffer);
  frame->output_buffer = gst_buffer_new_allocate(NULL, sizeof(mock.pixels), NULL);
  g_assert_nonnull(frame->output_buffer);
  GST_BUFFER_FLAG_SET(frame->output_buffer, GST_VIDEO_BUFFER_FLAG_INTERLACED);
  GST_BUFFER_FLAG_SET(frame->output_buffer, GST_VIDEO_BUFFER_FLAG_TFF);
  GST_BUFFER_FLAG_SET(frame->output_buffer, GST_VIDEO_BUFFER_FLAG_ONEFIELD);
  GST_BUFFER_FLAG_SET(frame->output_buffer, GST_VIDEO_BUFFER_FLAG_RFF);
  gst_video_codec_frame_unref(frame);
}

static void
assert_output_interlace_mode(GstCrystalHdDec *self,
                             GstVideoInterlaceMode expected)
{
  GstCaps *caps = gst_pad_get_current_caps(GST_VIDEO_DECODER_SRC_PAD(self));
  GstVideoInfo info;
  g_assert_nonnull(caps);
  g_assert_true(gst_video_info_from_caps(&info, caps));
  g_assert_cmpint(GST_VIDEO_INFO_INTERLACE_MODE(&info), ==, expected);
  g_assert_cmpint(GST_VIDEO_INFO_INTERLACE_MODE(&self->output_info), ==,
                  expected);
  gst_caps_unref(caps);
}

static void
assert_progressive_flags(GstBuffer *buffer, gboolean top_first,
                         gboolean repeat_first)
{
  g_assert_false(GST_BUFFER_FLAG_IS_SET(buffer,
                                        GST_VIDEO_BUFFER_FLAG_INTERLACED));
  g_assert_cmpint(GST_BUFFER_FLAG_IS_SET(buffer, GST_VIDEO_BUFFER_FLAG_TFF),
                  ==, top_first);
  g_assert_false(GST_BUFFER_FLAG_IS_SET(buffer,
                                        GST_VIDEO_BUFFER_FLAG_ONEFIELD));
  g_assert_cmpint(GST_BUFFER_FLAG_IS_SET(buffer, GST_VIDEO_BUFFER_FLAG_RFF),
                  ==, repeat_first);
}

typedef struct {
  guint buffers;
  guint flagged_buffers;
  guint buffers_before_mixed_caps;
  gboolean saw_progressive_caps;
  gboolean saw_mixed_caps;
  gboolean first_flagged_buffer_after_mixed_caps;
} ProgressiveOutputOrder;

static GstPadProbeReturn
record_progressive_output_order(GstPad *pad, GstPadProbeInfo *info,
                                gpointer user_data)
{
  ProgressiveOutputOrder *order = user_data;
  (void)pad;

  if ((GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) !=
      0) {
    GstEvent *event = GST_PAD_PROBE_INFO_EVENT(info);
    if (GST_EVENT_TYPE(event) == GST_EVENT_CAPS) {
      GstCaps *caps;
      GstVideoInfo video_info;
      gst_event_parse_caps(event, &caps);
      if (gst_video_info_from_caps(&video_info, caps)) {
        GstVideoInterlaceMode mode =
            GST_VIDEO_INFO_INTERLACE_MODE(&video_info);
        if (mode == GST_VIDEO_INTERLACE_MODE_PROGRESSIVE)
          order->saw_progressive_caps = TRUE;
        if (mode == GST_VIDEO_INTERLACE_MODE_MIXED) {
          order->saw_mixed_caps = TRUE;
          order->buffers_before_mixed_caps = order->buffers;
        }
      }
    }
  } else if ((GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) != 0) {
    GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    gboolean flagged =
        GST_BUFFER_FLAG_IS_SET(buffer, GST_VIDEO_BUFFER_FLAG_TFF) ||
        GST_BUFFER_FLAG_IS_SET(buffer, GST_VIDEO_BUFFER_FLAG_RFF);
    if (flagged && order->flagged_buffers++ == 0)
      order->first_flagged_buffer_after_mixed_caps = order->saw_mixed_caps;
    order->buffers++;
  }
  return GST_PAD_PROBE_OK;
}

static void
test_progressive_soft_rff_cadence(void)
{
  static const struct {
    guint32 pulldown;
    gboolean top_first;
    gboolean repeat_first;
  } cadence[] = {
    { vdecTopBottomTop, TRUE, TRUE },
    { vdecBottomTop, FALSE, FALSE },
    { vdecBottomTopBottom, FALSE, TRUE },
    { vdecTopBottom, TRUE, FALSE },
  };
  /* The fake-library boundary supplies the exact progressive MPEG-2
   * PicInfo cadence measured on BCM70015; compressed parsing is not under
   * test here. */
  GstHarness *harness = new_mpeg2_decoder();
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(harness->element);
  ProgressiveOutputOrder order = { 0 };
  gulong output_probe;
  guint phase;

  output_probe = gst_pad_add_probe(
      GST_VIDEO_DECODER_SRC_PAD(self),
      GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM | GST_PAD_PROBE_TYPE_BUFFER,
      record_progressive_output_order, &order, NULL);
  g_assert_cmpuint(output_probe, !=, 0);
  mock.auto_output = FALSE;

  /* Establish ordinary progressive output first. The first measured soft-RFF
   * picture must then publish MIXED caps before its flagged buffer. */
  {
    GstBuffer *output;
    guint64 timestamp;
    g_assert_cmpint(gst_harness_push(harness, new_input(0)), ==, GST_FLOW_OK);
    timestamp = mock.accepted[mock.accepted_count - 1];
    seed_stale_video_flags(GST_VIDEO_DECODER(self), 0);
    queue_pulldown_picture(timestamp, 1, vdecNoPulldownInfo);
    g_assert_cmpint(gst_crystalhd_receive_available(self), ==, GST_FLOW_OK);
    output = gst_harness_try_pull(harness);
    g_assert_nonnull(output);
    g_assert_cmpuint(GST_BUFFER_PTS(output), ==, 0);
    g_assert_cmpuint(GST_BUFFER_DURATION(output), ==, 40 * GST_MSECOND);
    assert_progressive_flags(output, FALSE, FALSE);
    g_assert_false(self->progressive_field_sequence);
    assert_output_interlace_mode(self, GST_VIDEO_INTERLACE_MODE_PROGRESSIVE);
    g_assert_true(order.saw_progressive_caps);
    g_assert_false(order.saw_mixed_caps);
    g_assert_cmpuint(order.buffers, ==, 1);
    g_assert_cmpuint(order.flagged_buffers, ==, 0);
    gst_buffer_unref(output);
  }

  for (phase = 0; phase < G_N_ELEMENTS(cadence); ++phase) {
    GstClockTime pts = (phase + 1) * 40 * GST_MSECOND;
    GstBuffer *output;
    guint64 timestamp;

    g_assert_cmpint(gst_harness_push(harness, new_input(pts)), ==, GST_FLOW_OK);
    timestamp = mock.accepted[mock.accepted_count - 1];
    seed_stale_video_flags(GST_VIDEO_DECODER(self), phase + 1);
    queue_pulldown_picture(timestamp, phase + 2, cadence[phase].pulldown);
    g_assert_cmpint(gst_crystalhd_receive_available(self), ==, GST_FLOW_OK);
    output = gst_harness_try_pull(harness);
    g_assert_nonnull(output);
    g_assert_cmpuint(GST_BUFFER_PTS(output), ==, pts);
    g_assert_cmpuint(GST_BUFFER_DURATION(output), ==, 40 * GST_MSECOND);
    assert_progressive_flags(output, cadence[phase].top_first,
                             cadence[phase].repeat_first);
    g_assert_true(self->progressive_field_sequence);
    assert_output_interlace_mode(self, GST_VIDEO_INTERLACE_MODE_MIXED);
    if (phase == 0) {
      g_assert_true(order.saw_mixed_caps);
      g_assert_cmpuint(order.buffers_before_mixed_caps, ==, 1);
      g_assert_cmpuint(order.flagged_buffers, ==, 1);
      g_assert_true(order.first_flagged_buffer_after_mixed_caps);
    }
    gst_buffer_unref(output);
  }

  /* Missing metadata after the measured 5,4,6,3 cadence has no per-buffer
   * flags, while the negotiated MIXED mode remains stable for the session. */
  {
    GstBuffer *output;
    guint64 timestamp;
    g_assert_cmpint(gst_harness_push(harness, new_input(200 * GST_MSECOND)),
                    ==, GST_FLOW_OK);
    timestamp = mock.accepted[mock.accepted_count - 1];
    seed_stale_video_flags(GST_VIDEO_DECODER(self), 5);
    queue_pulldown_picture(timestamp, 6, vdecNoPulldownInfo);
    g_assert_cmpint(gst_crystalhd_receive_available(self), ==, GST_FLOW_OK);
    output = gst_harness_try_pull(harness);
    g_assert_nonnull(output);
    g_assert_cmpuint(GST_BUFFER_PTS(output), ==, 200 * GST_MSECOND);
    g_assert_cmpuint(GST_BUFFER_DURATION(output), ==, 40 * GST_MSECOND);
    assert_progressive_flags(output, FALSE, FALSE);
    g_assert_true(self->progressive_field_sequence);
    assert_output_interlace_mode(self, GST_VIDEO_INTERLACE_MODE_MIXED);
    gst_buffer_unref(output);
  }

  /* A flushing seek starts a new field-sequence observation period. */
  flush_decoder(harness);
  g_assert_false(self->progressive_field_sequence);
  {
    GstBuffer *output;
    GstSegment segment;
    guint64 timestamp;
    gst_segment_init(&segment, GST_FORMAT_TIME);
    segment.start = GST_SECOND;
    segment.position = GST_SECOND;
    g_assert_true(gst_harness_push_event(harness,
                                         gst_event_new_segment(&segment)));
    g_assert_cmpint(gst_harness_push(harness, new_input(GST_SECOND)), ==,
                    GST_FLOW_OK);
    timestamp = mock.accepted[mock.accepted_count - 1];
    queue_pulldown_picture(timestamp, 7, vdecNoPulldownInfo);
    g_assert_cmpint(gst_crystalhd_receive_available(self), ==, GST_FLOW_OK);
    output = gst_harness_try_pull(harness);
    g_assert_nonnull(output);
    g_assert_cmpuint(GST_BUFFER_PTS(output), ==, GST_SECOND);
    g_assert_cmpuint(GST_BUFFER_DURATION(output), ==, 40 * GST_MSECOND);
    assert_progressive_flags(output, FALSE, FALSE);
    g_assert_false(self->progressive_field_sequence);
    assert_output_interlace_mode(self, GST_VIDEO_INTERLACE_MODE_PROGRESSIVE);
    gst_buffer_unref(output);
  }
  gst_pad_remove_probe(GST_VIDEO_DECODER_SRC_PAD(self), output_probe);
  gst_harness_teardown(harness);
}

static void
test_progressive_pulldown_unsupported(void)
{
  static const guint32 unsupported[] = {
    vdecNoPulldownInfo, vdecTop, vdecBottom, vdecFrame_X2,
    vdecFrame_X3, vdecFrame_X1, vdecFrame_X4, 11,
  };
  GstHarness *harness = new_mpeg2_decoder();
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(harness->element);
  guint phase;

  mock.auto_output = FALSE;
  for (phase = 0; phase < G_N_ELEMENTS(unsupported); ++phase) {
    GstBuffer *output;
    guint64 timestamp;
    GstClockTime pts = phase * 40 * GST_MSECOND;
    g_assert_cmpint(gst_harness_push(harness, new_input(pts)), ==, GST_FLOW_OK);
    timestamp = mock.accepted[mock.accepted_count - 1];
    seed_stale_video_flags(GST_VIDEO_DECODER(self), phase);
    queue_pulldown_picture(timestamp, phase + 1, unsupported[phase]);
    g_assert_cmpint(gst_crystalhd_receive_available(self), ==, GST_FLOW_OK);
    output = gst_harness_try_pull(harness);
    g_assert_nonnull(output);
    g_assert_cmpuint(GST_BUFFER_PTS(output), ==, pts);
    g_assert_cmpuint(GST_BUFFER_DURATION(output), ==, 40 * GST_MSECOND);
    assert_progressive_flags(output, FALSE, FALSE);
    g_assert_false(self->progressive_field_sequence);
    assert_output_interlace_mode(self, GST_VIDEO_INTERLACE_MODE_PROGRESSIVE);
    gst_buffer_unref(output);
  }
  gst_harness_teardown(harness);
}

static void
test_progressive_pulldown_format_reopen(void)
{
  GstHarness *harness = new_mpeg2_decoder();
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(harness->element);
  GstBuffer *output;
  guint64 timestamp;
  guint opens;

  mock.auto_output = FALSE;
  g_assert_cmpint(gst_harness_push(harness, new_input(0)), ==, GST_FLOW_OK);
  timestamp = mock.accepted[mock.accepted_count - 1];
  queue_pulldown_picture(timestamp, 1, vdecTopBottomTop);
  g_assert_cmpint(gst_crystalhd_receive_available(self), ==, GST_FLOW_OK);
  output = gst_harness_try_pull(harness);
  g_assert_nonnull(output);
  assert_progressive_flags(output, TRUE, TRUE);
  gst_buffer_unref(output);
  g_assert_true(self->progressive_field_sequence);
  assert_output_interlace_mode(self, GST_VIDEO_INTERLACE_MODE_MIXED);

  /* A natural caps change closes the old device session and must not carry
   * its observed pulldown mode into the new H.264 session. */
  opens = mock.successful_opens;
  g_assert_true(change_h264_caps(harness));
  g_assert_cmpuint(mock.successful_opens, ==, opens + 1);
  g_assert_false(self->progressive_field_sequence);
  g_assert_false(self->output_configured);

  g_assert_cmpint(gst_harness_push(harness, new_input(40 * GST_MSECOND)), ==,
                  GST_FLOW_OK);
  timestamp = mock.accepted[mock.accepted_count - 1];
  queue_pulldown_picture(timestamp, 2, vdecNoPulldownInfo);
  g_assert_cmpint(gst_crystalhd_receive_available(self), ==, GST_FLOW_OK);
  output = gst_harness_try_pull(harness);
  g_assert_nonnull(output);
  g_assert_cmpuint(GST_BUFFER_PTS(output), ==, 40 * GST_MSECOND);
  assert_progressive_flags(output, FALSE, FALSE);
  g_assert_false(self->progressive_field_sequence);
  assert_output_interlace_mode(self, GST_VIDEO_INTERLACE_MODE_PROGRESSIVE);
  gst_buffer_unref(output);
  gst_harness_teardown(harness);
}

static void
test_interlaced_picture_types(void)
{
  static const guint types[] = { VDEC_FLAG_FRAME, VDEC_FLAG_FIELDPAIR };
  guint i;
  for (i = 0; i < G_N_ELEMENTS(types); ++i) {
    GstHarness *harness = new_decoder();
    GstCrystalHdDec *self = GST_CRYSTALHD_DEC(harness->element);
    GstVideoCodecFrame *frame;
    MockPicture *picture;
    mock.auto_output = FALSE;
    g_assert_cmpint(gst_harness_push(harness, new_input(0)), ==, GST_FLOW_OK);
    queue_picture(mock.accepted[0], 0x55);
    picture = g_queue_peek_tail(&mock.pictures);
    picture->flags = VDEC_FLAG_INTERLACED_SRC | types[i];
    /* A full-frame/FIELDPAIR layout must not be mistaken for either half
     * of a separate-field buffer and then woven with unrelated pixels. */
    g_assert_cmpint(gst_crystalhd_receive_available(self), ==, GST_FLOW_ERROR);
    g_assert_cmpuint(gst_harness_buffers_in_queue(harness), ==, 0);
    g_assert_cmpuint(self->timestamps.length, ==, 1);
    g_assert_false(self->need_second_field);
    g_assert_cmpuint(mock.release_calls, ==, 1);
    frame = gst_video_decoder_get_frame(GST_VIDEO_DECODER(self), 0);
    g_assert_nonnull(frame);
    g_assert_null(frame->output_buffer);
    gst_video_codec_frame_unref(frame);
    gst_harness_teardown(harness);
  }
}

static GstBuffer *
wait_for_output(GstHarness *harness)
{
  const gint64 deadline = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
  GstBuffer *buffer;
  while ((buffer = gst_harness_try_pull(harness)) == NULL &&
         g_get_monotonic_time() < deadline)
    g_usleep(1000);
  g_assert_nonnull(buffer);
  return buffer;
}

static void
test_autonomous_output(void)
{
  GstHarness *harness;
  GstBuffer *held;
  real_workers = TRUE;
  harness = new_decoder();
  mock.auto_output = FALSE;
  g_assert_cmpint(gst_harness_push(harness, new_input(0)), ==, GST_FLOW_OK);
  g_assert_cmpuint(gst_harness_buffers_in_queue(harness), ==, 0);
  /* No next input, drain, or control callback: only the output thread can
   * deliver a picture that the mock firmware makes ready after submission.
   */
  queue_picture(mock.accepted[0], 0x51);
  held = wait_for_output(harness);
  check_output(held, 0, 0x51);
  gst_harness_teardown(harness);
  check_output(held, 0, 0x51);
  gst_buffer_unref(held);
  g_assert_cmpuint(mock.release_calls, ==, 1);
  g_assert_cmpuint(mock.successful_opens, ==, mock.close_calls);
  real_workers = FALSE;
}

static void
test_vc1_recursive_input_wait(void)
{
  static const guint8 pictures[] = {
    0, 0, 1, 0x0d, 0x11, 0x22, 0x33, 0x44,
    0, 0, 1, 0x0d, 0x55, 0x66, 0x77, 0x88
  };
  GstHarness *harness;
  GstBuffer *input, *output;
  real_workers = TRUE;
  harness = new_decoder();
  gst_harness_set_src_caps_str(harness,
      "video/x-vc1,parsed=true,width=16,height=16,framerate=25/1");
  mock.auto_output = FALSE;
  input = gst_buffer_new_allocate(NULL, sizeof(pictures), NULL);
  gst_buffer_fill(input, 0, pictures, sizeof(pictures));
  GST_BUFFER_PTS(input) = 0;
  g_assert_cmpint(gst_harness_push(harness, input), ==, GST_FLOW_OK);
  g_assert_cmpuint(mock.accepted_count, ==, 1);
  mock.required_bytes = 232;
  mock.free_bytes = 0;
  mock.release_makes_room = TRUE;
  mock.publish_on_wait = TRUE;
  mock.auto_output = TRUE;
  input = gst_buffer_new_allocate(NULL, 8, NULL);
  gst_buffer_fill(input, 0, pictures, 8);
  GST_BUFFER_PTS(input) = 80 * GST_MSECOND;
  /* The first picture becomes ready only inside the second picture's
   * capacity check. parse->have_frame holds two recursive core levels;
   * both must be released or the real RX worker cannot make input room.
   */
  g_assert_cmpint(gst_harness_push(harness, input), ==, GST_FLOW_OK);
  g_assert_false(mock.publish_on_wait);
  g_assert_cmpuint(mock.accepted_count, ==, 2);
  output = wait_for_output(harness);
  check_output(output, 0, 1);
  gst_buffer_unref(output);
  output = wait_for_output(harness);
  g_assert_cmpuint(gst_buffer_get_size(output), ==, sizeof(mock.pixels));
  {
    guint8 bytes[sizeof(mock.pixels)];
    guint i;
    g_assert_cmpuint(gst_buffer_extract(output, 0, bytes, sizeof(bytes)), ==, sizeof(bytes));
    for (i = 0; i < sizeof(bytes); ++i)
      g_assert_cmpuint(bytes[i], ==, 2);
  }
  gst_buffer_unref(output);
  gst_harness_teardown(harness);
  g_assert_cmpuint(mock.release_calls, ==, 2);
  g_assert_cmpuint(mock.unsafe_submissions, ==, 0);
  real_workers = FALSE;
}

typedef struct {
  GMutex lock;
  GCond changed;
  GstHarness *harness;
  gboolean entered;
  gboolean release;
  gboolean lease_released;
  gboolean timed_out;
  gboolean flush_stop_seen;
  gboolean late_delivery;
  gboolean operation_entered;
  gboolean operation_done;
  gboolean drain;
  gboolean flush;
  gboolean caps;
  GstFlowReturn operation_flow;
} DeliveryGate;

static GstPadProbeReturn
hold_delivery(GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
  DeliveryGate *gate = data;
  const gint64 deadline = g_get_monotonic_time() + 4 * G_USEC_PER_SEC;
  (void)pad;
  if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) {
    if (GST_EVENT_TYPE(GST_PAD_PROBE_INFO_EVENT(info)) == GST_EVENT_FLUSH_STOP) {
      g_mutex_lock(&gate->lock);
      gate->flush_stop_seen = TRUE;
      g_cond_broadcast(&gate->changed);
      g_mutex_unlock(&gate->lock);
    }
    return GST_PAD_PROBE_OK;
  }
  g_assert_nonnull(GST_PAD_PROBE_INFO_BUFFER(info));
  g_mutex_lock(&gate->lock);
  gate->lease_released = mock.release_calls != 0;
  gate->entered = TRUE;
  g_cond_broadcast(&gate->changed);
  while (!gate->release) {
    if (!g_cond_wait_until(&gate->changed, &gate->lock, deadline)) {
      gate->timed_out = TRUE;
      break;
    }
  }
  gate->late_delivery = gate->flush_stop_seen;
  g_mutex_unlock(&gate->lock);
  return GST_PAD_PROBE_OK;
}

static gpointer
delivery_operation(gpointer data)
{
  DeliveryGate *gate = data;
  GstFlowReturn flow;
  g_mutex_lock(&gate->lock);
  gate->operation_entered = TRUE;
  g_cond_broadcast(&gate->changed);
  g_mutex_unlock(&gate->lock);
  if (gate->flush) {
    gboolean started = gst_harness_push_event(gate->harness, gst_event_new_flush_start());
    gboolean stopped = gst_harness_push_event(gate->harness, gst_event_new_flush_stop(TRUE));
    flow = started && stopped ? GST_FLOW_OK : GST_FLOW_ERROR;
  } else if (gate->caps) {
    flow = change_h264_caps(gate->harness) ? GST_FLOW_OK : GST_FLOW_ERROR;
  } else {
    flow = gate->drain ?
        gst_crystalhd_drain(GST_VIDEO_DECODER(gate->harness->element)) :
        gst_harness_push(gate->harness, new_input(40 * GST_MSECOND));
  }
  g_mutex_lock(&gate->lock);
  gate->operation_flow = flow;
  gate->operation_done = TRUE;
  g_cond_broadcast(&gate->changed);
  g_mutex_unlock(&gate->lock);
  return NULL;
}

static void
test_blocked_delivery(gconstpointer data)
{
  DeliveryGate gate = { 0 };
  GThread *operation;
  GstBuffer *output;
  gboolean completed_while_blocked;
  gint64 deadline;
  gulong probe;
  real_workers = TRUE;
  gate.drain = GPOINTER_TO_INT(data) == 1;
  gate.flush = GPOINTER_TO_INT(data) == 2;
  gate.caps = GPOINTER_TO_INT(data) == 3;
  g_mutex_init(&gate.lock);
  g_cond_init(&gate.changed);
  gate.harness = new_decoder();
  mock.auto_output = FALSE;
  probe = gst_pad_add_probe(gate.harness->sinkpad, GST_PAD_PROBE_TYPE_BUFFER |
                            GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM |
                            GST_PAD_PROBE_TYPE_EVENT_FLUSH,
                            hold_delivery, &gate, NULL);
  g_assert_cmpint(gst_harness_push(gate.harness, new_input(0)), ==, GST_FLOW_OK);
  queue_picture(mock.accepted[0], 0x61);
  g_mutex_lock(&gate.lock);
  deadline = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
  while (!gate.entered && g_cond_wait_until(&gate.changed, &gate.lock, deadline))
    ;
  g_assert_true(gate.entered);
  g_assert_true(gate.lease_released);
  g_mutex_unlock(&gate.lock);

  operation = g_thread_new("lifecycle-operation", delivery_operation, &gate);
  g_mutex_lock(&gate.lock);
  deadline = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
  while (!gate.operation_entered &&
         g_cond_wait_until(&gate.changed, &gate.lock, deadline))
    ;
  g_assert_true(gate.operation_entered);
  deadline = g_get_monotonic_time() +
      (gate.drain || gate.flush || gate.caps ? 100000 : G_USEC_PER_SEC);
  while (!gate.operation_done &&
         g_cond_wait_until(&gate.changed, &gate.lock, deadline))
    ;
  completed_while_blocked = gate.operation_done;
  gate.release = TRUE;
  g_cond_broadcast(&gate.changed);
  g_mutex_unlock(&gate.lock);
  /* Always release the sink before joining, including a failing assertion's
   * cleanup path. A broken outer stream lock cannot hang the regression.
   */
  g_thread_join(operation);
  gst_pad_remove_probe(gate.harness->sinkpad, probe);
  if (gate.flush) {
    GstSegment segment;
    g_assert_true(gate.flush_stop_seen);
    g_assert_false(gate.late_delivery);
    /* Unlike a flushing playback sink, GstHarness retains already-delivered
     * buffers in its observation queue. The event probe proves this old
     * delivery finished before FLUSH_STOP; discard that observed old output.
     */
    output = gst_harness_try_pull(gate.harness);
    if (output != NULL) {
      check_output(output, 0, 0x61);
      gst_buffer_unref(output);
    }
    g_assert_cmpuint(gst_harness_buffers_in_queue(gate.harness), ==, 0);
    gst_segment_init(&segment, GST_FORMAT_TIME);
    segment.start = GST_SECOND;
    segment.position = GST_SECOND;
    g_assert_true(gst_harness_push_event(gate.harness, gst_event_new_segment(&segment)));
    queue_picture(mock.accepted[0], 0xee);
    mock.auto_output = TRUE;
    g_assert_cmpint(gst_harness_push(gate.harness, new_input(GST_SECOND)), ==, GST_FLOW_OK);
    output = wait_for_output(gate.harness);
    check_output(output, GST_SECOND, 2);
    gst_buffer_unref(output);
    g_assert_cmpuint(mock.accepted[1], >, mock.accepted[0]);
    g_assert_cmpuint(gst_harness_buffers_in_queue(gate.harness), ==, 0);
  } else {
    output = wait_for_output(gate.harness);
    check_output(output, 0, 0x61);
    gst_buffer_unref(output);
  }
  gst_harness_teardown(gate.harness);
  g_assert_false(gate.timed_out);
  g_assert_cmpint(gate.operation_flow, ==, GST_FLOW_OK);
  if (gate.drain || gate.flush || gate.caps)
    g_assert_false(completed_while_blocked);
  else
    g_assert_true(completed_while_blocked);
  g_assert_cmpuint(mock.release_calls, ==, gate.flush ? 3 : 1);
  g_assert_cmpuint(mock.destroyed_inputs, ==, gate.drain || gate.caps ? 1 : 2);
  if (gate.caps) {
    g_assert_cmpuint(mock.drain_calls, ==, 1);
    g_assert_cmpuint(mock.successful_opens, ==, 2);
  }
  g_cond_clear(&gate.changed);
  g_mutex_clear(&gate.lock);
  real_workers = FALSE;
}

static gboolean
change_h264_caps(GstHarness *harness)
{
  GstCaps *caps = gst_caps_from_string(
      "video/x-h264,stream-format=byte-stream,alignment=au,parsed=true,"
      "width=16,height=16,framerate=30/1");
  gboolean result = gst_harness_push_event(harness, gst_event_new_caps(caps));
  gst_caps_unref(caps);
  return result;
}

static void
test_natural_caps_tail(void)
{
  GstHarness *harness = new_decoder();
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(harness->element);
  GstBuffer *old[2], *output;
  guint i;
  mock.auto_output = FALSE;
  for (i = 0; i < 2; ++i)
    g_assert_cmpint(gst_harness_push(harness, new_input(i * 40 * GST_MSECOND)),
                    ==, GST_FLOW_OK);
  for (i = 0; i < 2; ++i)
    queue_picture(mock.accepted[i], 0x71 + i);
  g_assert_true(change_h264_caps(harness));
  g_assert_cmpuint(mock.drain_calls, ==, 1);
  g_assert_cmpuint(mock.close_calls, ==, 1);
  g_assert_cmpuint(mock.successful_opens, ==, 2);
  g_assert_cmpuint(self->timestamps.length, ==, 0);
  g_assert_cmpuint(GST_VIDEO_INFO_FPS_N(&self->input_state->info), ==, 30);
  for (i = 0; i < 2; ++i) {
    old[i] = gst_harness_try_pull(harness);
    g_assert_nonnull(old[i]);
    check_output(old[i], i * 40 * GST_MSECOND, 0x71 + i);
  }
  mock.auto_output = TRUE;
  g_assert_cmpint(gst_harness_push(harness, new_input(80 * GST_MSECOND)),
                  ==, GST_FLOW_OK);
  output = gst_harness_pull(harness);
  check_output(output, 80 * GST_MSECOND, 3);
  gst_buffer_unref(output);
  g_assert_cmpuint(mock.accepted[2], >, mock.accepted[1]);
  gst_harness_teardown(harness);
  for (i = 0; i < 2; ++i) {
    check_output(old[i], i * 40 * GST_MSECOND, 0x71 + i);
    gst_buffer_unref(old[i]);
  }
}

static void
test_natural_caps_errors(void)
{
  guint mode;
  for (mode = 0; mode < 4; ++mode) {
    GstHarness *harness = new_decoder();
    GstCrystalHdDec *self = GST_CRYSTALHD_DEC(harness->element);
    GstVideoCodecState *old_state = self->input_state;
    guint64 generation = self->generation;
    mock.auto_output = FALSE;
    g_assert_cmpint(gst_harness_push(harness, new_input(0)), ==, GST_FLOW_OK);
    if (mode == 0)
      mock.fail_drain = TRUE;
    else if (mode == 1)
      mock.early_eos = TRUE;
    else if (mode == 2) {
      mock.free_bytes = 0;
      flush_during_wait = harness;
    } else {
      queue_picture(mock.accepted[0], 0x74);
      flush_during_quiesce = harness;
    }
    /* An aliased input state must remain owned and usable on failure. */
    g_assert_false(gst_crystalhd_set_format(GST_VIDEO_DECODER(self), old_state));
    g_assert_true(self->input_state == old_state);
    g_assert_cmpuint(self->generation, ==, generation);
    g_assert_cmpuint(self->timestamps.length, ==, mode == 3 ? 0 : 1);
    g_assert_cmpuint(mock.close_calls, ==, 0);
    g_assert_cmpuint(mock.successful_opens, ==, 1);
    mock.fail_drain = FALSE;
    mock.free_bytes = 1024 * 1024;
    if (mode >= 2)
      g_assert_true(gst_harness_push_event(harness, gst_event_new_flush_stop(TRUE)));
    gst_harness_teardown(harness);
  }
}

static void
test_natural_caps_raw_boundaries(void)
{
  guint mode;
  for (mode = 0; mode < 4; ++mode) {
    guint8 bytes[] = { 0, 0, 1, 0x0d, 0x11, 0x22, 0x33, 0x44 };
    GstHarness *harness = new_decoder();
    GstCrystalHdDec *self = GST_CRYSTALHD_DEC(harness->element);
    GstBuffer *input, *output;
    GstCaps *caps;
    gst_harness_set_src_caps_str(harness,
        "video/x-vc1,parsed=true,width=16,height=16,framerate=25/1");
    if (mode == 0)
      bytes[3] = 0x0f; /* headers only: no picture may be manufactured */
    input = gst_buffer_new_allocate(NULL, sizeof(bytes), NULL);
    gst_buffer_fill(input, 0, bytes, sizeof(bytes));
    GST_BUFFER_PTS(input) = 0;
    g_assert_cmpint(gst_harness_push(harness, input), ==, GST_FLOW_OK);
    g_assert_cmpuint(mock.accepted_count, ==, 0);
    if (mode == 1) {
      flush_decoder(harness);
      g_assert_cmpuint(mock.drain_calls, ==, 0);
      g_assert_null(self->parse_adapter);
    }
    if (mode == 3) {
      GstVideoCodecState *old = self->input_state;
      guint64 generation = self->generation;
      guint closes = mock.close_calls;
      mock.fail_drain = TRUE;
      g_assert_false(gst_crystalhd_set_format(GST_VIDEO_DECODER(self), old));
      g_assert_true(self->input_state == old);
      g_assert_cmpuint(self->generation, ==, generation);
      g_assert_cmpuint(mock.close_calls, ==, closes);
      g_assert_cmpuint(self->timestamps.length, ==, 1);
      g_assert_cmpuint(mock.accepted_count, ==, 1);
      mock.fail_drain = FALSE;
    }
    caps = gst_caps_from_string(
        "video/x-vc1,parsed=true,width=16,height=16,framerate=30/1");
    g_assert_true(gst_harness_push_event(harness, gst_event_new_caps(caps)));
    gst_caps_unref(caps);
    g_assert_cmpuint(mock.accepted_count, ==, mode >= 2 ? 1 : 0);
    if (mode >= 2) {
      output = gst_harness_try_pull(harness);
      g_assert_nonnull(output);
      check_output(output, 0, 1);
      gst_buffer_unref(output);
    }
    /* A new raw tail must be parsed with the new base current frame and
     * buffer offsets, including after header-only and flush transitions. */
    if (mode == 1) {
      GstSegment segment;
      gst_segment_init(&segment, GST_FORMAT_TIME);
      g_assert_true(gst_harness_push_event(harness, gst_event_new_segment(&segment)));
    }
    bytes[3] = 0x0d;
    input = gst_buffer_new_allocate(NULL, sizeof(bytes), NULL);
    gst_buffer_fill(input, 0, bytes, sizeof(bytes));
    GST_BUFFER_PTS(input) = GST_SECOND;
    g_assert_cmpint(gst_harness_push(harness, input), ==, GST_FLOW_OK);
    g_assert_true(gst_harness_push_event(harness, gst_event_new_eos()));
    output = gst_harness_try_pull(harness);
    g_assert_nonnull(output);
    check_output(output, GST_SECOND, mode >= 2 ? 2 : 1);
    gst_buffer_unref(output);
    g_assert_cmpuint(gst_harness_buffers_in_queue(harness), ==, 0);
    gst_harness_teardown(harness);
  }
}

static void
test_natural_caps_raw_tail(void)
{
  static const guint8 picture[] = { 0, 0, 1, 0x0d, 0x11, 0x22, 0x33, 0x44 };
  GstHarness *harness = new_decoder();
  GstBuffer *input, *output;
  guint i;
  gst_harness_set_src_caps_str(harness,
      "video/x-vc1,parsed=true,width=16,height=16,framerate=25/1");
  mock.auto_output = FALSE;
  for (i = 0; i < 2; ++i) {
    input = gst_buffer_new_allocate(NULL, sizeof(picture), NULL);
    gst_buffer_fill(input, 0, picture, sizeof(picture));
    GST_BUFFER_PTS(input) = i * 40 * GST_MSECOND;
    g_assert_cmpint(gst_harness_push(harness, input), ==, GST_FLOW_OK);
  }
  /* The final old AU has no following start code: it is still in the real
   * base-class parser adapter, not in our hardware timestamp queue. */
  g_assert_cmpuint(mock.accepted_count, ==, 1);
  queue_picture(mock.accepted[0], 0x73);
  mock.auto_output = TRUE;
  g_assert_true(change_h264_caps(harness));
  g_assert_cmpuint(mock.accepted_count, ==, 2);
  g_assert_cmpuint(mock.drain_calls, ==, 1);
  output = gst_harness_try_pull(harness);
  g_assert_nonnull(output);
  check_output(output, 0, 0x73);
  gst_buffer_unref(output);
  output = gst_harness_try_pull(harness);
  g_assert_nonnull(output);
  check_output(output, 40 * GST_MSECOND, 2);
  gst_buffer_unref(output);
  g_assert_cmpint(gst_harness_push(harness, new_input(80 * GST_MSECOND)),
                  ==, GST_FLOW_OK);
  output = gst_harness_pull(harness);
  check_output(output, 80 * GST_MSECOND, 3);
  gst_buffer_unref(output);
  gst_harness_teardown(harness);
}

static void
test_recovery_epoch_reconcile(void)
{
  GstHarness *harness = new_decoder();
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(harness->element);
  GstVideoDecoder *decoder = GST_VIDEO_DECODER(self);
  GstVideoCodecFrame *current, *retained;
  GstFlowReturn flow;
  gboolean active, changed;

  mock.auto_output = FALSE;
  g_assert_cmpint(gst_harness_push(harness, new_input(0)), ==, GST_FLOW_OK);
  g_assert_cmpuint(self->timestamps.length, ==, 1);

  GST_VIDEO_DECODER_STREAM_LOCK(decoder);
  current = gst_video_decoder_get_oldest_frame(decoder);
  GST_VIDEO_DECODER_STREAM_UNLOCK(decoder);
  g_assert_nonnull(current);

  mock.recovery_generation = 1;
  mock.recovery_active = TRUE;
  GST_VIDEO_DECODER_STREAM_LOCK(decoder);
  flow = gst_crystalhd_reconcile_recovery(self, current, &active, &changed);
  GST_VIDEO_DECODER_STREAM_UNLOCK(decoder);
  g_assert_cmpint(flow, ==, GST_FLOW_OK);
  g_assert_true(active);
  g_assert_false(changed);
  g_assert_cmpuint(self->timestamps.length, ==, 1);
  g_assert_cmpuint(self->recovery_generation_seen, ==, 0);

  mock.recovery_active = FALSE;
  GST_VIDEO_DECODER_STREAM_LOCK(decoder);
  flow = gst_crystalhd_reconcile_recovery(self, current, &active, &changed);
  retained = gst_video_decoder_get_oldest_frame(decoder);
  GST_VIDEO_DECODER_STREAM_UNLOCK(decoder);
  g_assert_cmpint(flow, ==, GST_FLOW_OK);
  g_assert_false(active);
  g_assert_true(changed);
  g_assert_cmpuint(self->timestamps.length, ==, 0);
  g_assert_cmpuint(self->recovery_generation_seen, ==, 1);
  g_assert_true(self->recovery_needs_sync);
  g_assert_true(retained == current);
  gst_video_codec_frame_unref(retained);

  GST_VIDEO_DECODER_STREAM_LOCK(decoder);
  g_assert_cmpint(gst_video_decoder_drop_frame(decoder, current), ==,
                  GST_FLOW_OK);
  GST_VIDEO_DECODER_STREAM_UNLOCK(decoder);

  mock.recovery_generation = 2;
  mock.recovery_status = BC_STS_ERROR;
  GST_VIDEO_DECODER_STREAM_LOCK(decoder);
  flow = gst_crystalhd_reconcile_recovery(self, NULL, &active, &changed);
  GST_VIDEO_DECODER_STREAM_UNLOCK(decoder);
  g_assert_cmpint(flow, ==, GST_FLOW_ERROR);
  g_assert_cmpuint(self->recovery_generation_seen, ==, 1);
  mock.recovery_status = BC_STS_SUCCESS;
  gst_harness_teardown(harness);
}

static void
test_recovery_config_reservation_boundaries(void)
{
  const gsize exact_config = GST_CRYSTALHD_INPUT_CAPACITY - 768;
  static const gsize rejected[] = {
    GST_CRYSTALHD_INPUT_CAPACITY - 1,
    GST_CRYSTALHD_INPUT_CAPACITY,
    GST_CRYSTALHD_INPUT_CAPACITY + 1,
    G_MAXSIZE
  };
  gsize reservation;
  guint i;

  /* At this size the 17 full packet groups plus fixed framing are 768
   * bytes, so the combined replay call is exactly the 1 MiB ring. */
  g_assert_cmpuint(exact_config / 60000, ==, 17);
  reservation = 0;
  g_assert_true(gst_crystalhd_add_recovery_config_reservation(
      exact_config, &reservation));
  g_assert_cmpuint(reservation, ==, GST_CRYSTALHD_INPUT_CAPACITY);

  reservation = 1;
  g_assert_false(gst_crystalhd_add_recovery_config_reservation(
      exact_config, &reservation));
  g_assert_cmpuint(reservation, ==, 1);
  reservation = 23;
  g_assert_false(gst_crystalhd_add_recovery_config_reservation(
      exact_config + 1, &reservation));
  g_assert_cmpuint(reservation, ==, 23);

  for (i = 0; i < G_N_ELEMENTS(rejected); ++i) {
    reservation = 37;
    g_assert_false(gst_crystalhd_add_recovery_config_reservation(
        rejected[i], &reservation));
    g_assert_cmpuint(reservation, ==, 37);
  }
}

static void
test_recovery_input_deadline(void)
{
  guint mode;

  for (mode = 0; mode < 2; ++mode) {
    GstHarness *harness = new_decoder();
    GstCrystalHdDec *self = GST_CRYSTALHD_DEC(harness->element);

    mock.auto_output = FALSE;
    mock.sleep_limit = 10001;
    if (mode == 0) {
      mock.recovery_generation = 1;
      mock.recovery_active = TRUE;
    } else {
      mock.input_persistent = BC_STS_BUSY;
    }
    g_assert_cmpint(gst_harness_push(harness, new_input(0)), ==,
                    GST_FLOW_ERROR);
    g_assert_cmpint(lifecycle_now, ==,
                    1 + GST_CRYSTALHD_INPUT_TIMEOUT_US);
    g_assert_cmpuint(mock.sleep_calls, ==, 10000);
    g_assert_cmpuint(mock.input_calls, ==, mode == 0 ? 0 : 10000);
    g_assert_cmpuint(mock.accepted_count, ==, 0);
    g_assert_cmpuint(mock.unsafe_submissions, ==, 0);
    g_assert_cmpuint(self->timestamps.length, ==, 0);
    g_assert_cmpuint(mock.destroyed_inputs, ==, 1);
    mock.recovery_active = FALSE;
    mock.input_persistent = BC_STS_SUCCESS;
    gst_harness_teardown(harness);
  }
}

static void
test_recovery_blocked_delivery_reconcile(void)
{
  DeliveryGate gate = { 0 };
  GstCrystalHdDec *self;
  GThread *operation;
  GstBuffer *output;
  gboolean completed_while_blocked;
  gboolean output_active;
  gint64 deadline;
  gulong probe;

  real_workers = TRUE;
  g_mutex_init(&gate.lock);
  g_cond_init(&gate.changed);
  gate.harness = new_decoder();
  self = GST_CRYSTALHD_DEC(gate.harness->element);
  mock.auto_output = FALSE;
  probe = gst_pad_add_probe(gate.harness->sinkpad,
                            GST_PAD_PROBE_TYPE_BUFFER,
                            hold_delivery, &gate, NULL);
  g_assert_cmpint(gst_harness_push(gate.harness, new_input(0)), ==,
                  GST_FLOW_OK);
  queue_picture(mock.accepted[0], 0x71);

  g_mutex_lock(&gate.lock);
  deadline = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
  while (!gate.entered &&
         g_cond_wait_until(&gate.changed, &gate.lock, deadline))
    ;
  g_assert_true(gate.entered);
  g_assert_true(gate.lease_released);
  g_mutex_unlock(&gate.lock);

  /* copy_output already removed the delivered frame's timestamp before the
   * downstream push blocked. A completed epoch may therefore reconcile the
   * remaining timestamp set while output_active still owns that frame. */
  mock.recovery_generation = 1;
  mock.recovery_replay_bytes = 31;
  mock.recovery_active = FALSE;
  operation = g_thread_new("recovery-next-input", delivery_operation, &gate);
  g_mutex_lock(&gate.lock);
  deadline = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
  while (!gate.operation_done &&
         g_cond_wait_until(&gate.changed, &gate.lock, deadline))
    ;
  completed_while_blocked = gate.operation_done;
  g_mutex_lock(&self->output_lock);
  output_active = self->output_active;
  g_mutex_unlock(&self->output_lock);
  gate.release = TRUE;
  g_cond_broadcast(&gate.changed);
  g_mutex_unlock(&gate.lock);
  g_thread_join(operation);
  gst_pad_remove_probe(gate.harness->sinkpad, probe);

  g_assert_true(completed_while_blocked);
  g_assert_cmpint(gate.operation_flow, ==, GST_FLOW_OK);
  g_assert_true(output_active);
  g_assert_cmpuint(self->recovery_generation_seen, ==, 1);
  g_assert_false(self->recovery_needs_sync);
  g_assert_cmpuint(mock.accepted_count, ==, 2);
  g_assert_cmpuint(self->timestamps.length, ==, 1);
  output = wait_for_output(gate.harness);
  check_output(output, 0, 0x71);
  gst_buffer_unref(output);
  g_assert_null(gst_harness_try_pull(gate.harness));
  g_assert_false(gate.timed_out);
  gst_harness_teardown(gate.harness);
  g_assert_cmpuint(mock.release_calls, ==, 1);
  g_assert_cmpuint(mock.destroyed_inputs, ==, 2);
  g_cond_clear(&gate.changed);
  g_mutex_clear(&gate.lock);
  real_workers = FALSE;
}

static void
test_recovery_h264_gate_and_reservation(void)
{
  static const guint8 valid_headers[] = {
    /* One leading_zero_8bits byte before the first four-byte prefix is
     * valid Annex B and must not make the recovery gate miss the headers. */
    0, 0, 0, 0, 1, 0x67, 0x64, 0x00, 0x1f,
    0, 0, 0, 1, 0x68, 0xee, 0x3c,
    0, 0, 0, 1, 0x65, 0x88, 0x84
  };
  static const guint8 garbage_prefixed_headers[] = {
    0x12, 0, 0, 0, 1, 0x67, 0x64, 0x00, 0x1f,
    0, 0, 0, 1, 0x68, 0xee, 0x3c,
    0, 0, 0, 1, 0x65, 0x88, 0x84
  };
  static const guint8 empty_nal_headers[] = {
    0, 0, 0, 1,
    0, 0, 0, 1, 0x67, 0x64, 0x00, 0x1f,
    0, 0, 0, 1, 0x68, 0xee, 0x3c,
    0, 0, 0, 1, 0x65, 0x88
  };
  GstHarness *harness = new_decoder();
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(harness->element);
  gsize reservation;

  mock.auto_output = FALSE;
  mock.recovery_generation = 1;
  g_assert_cmpint(gst_harness_push(harness, new_input(0)), ==, GST_FLOW_OK);
  g_assert_cmpuint(mock.input_calls, ==, 0);
  g_assert_cmpuint(mock.accepted_count, ==, 0);
  g_assert_true(self->recovery_needs_sync);

  g_assert_false(gst_crystalhd_h264_has_parameter_sets(
      empty_nal_headers, sizeof(empty_nal_headers)));
  g_assert_false(gst_crystalhd_h264_has_parameter_sets(
      garbage_prefixed_headers, sizeof(garbage_prefixed_headers)));
  g_assert_cmpint(gst_harness_push(harness,
      new_h264_input(40 * GST_MSECOND, empty_nal_headers,
                     sizeof(empty_nal_headers))), ==, GST_FLOW_OK);
  g_assert_cmpuint(mock.input_calls, ==, 0);
  g_assert_cmpuint(mock.accepted_count, ==, 0);

  g_assert_true(gst_crystalhd_h264_has_parameter_sets(
      valid_headers, sizeof(valid_headers)));
  g_assert_cmpint(gst_harness_push(harness,
      new_h264_input(80 * GST_MSECOND, valid_headers,
                     sizeof(valid_headers))), ==, GST_FLOW_OK);
  g_assert_cmpuint(mock.input_calls, ==, 1);
  g_assert_cmpuint(mock.accepted_count, ==, 1);
  g_assert_false(self->recovery_needs_sync);
  g_assert_cmpuint(self->timestamps.length, ==, 1);

  g_assert_true(gst_crystalhd_input_reservation(
      BC_MSUBTYPE_H264, 8, self->input_metadata_size, &reservation));
  g_assert_true(gst_crystalhd_add_recovery_config_reservation(
      37, &reservation));
  mock.recovery_generation = 2;
  mock.recovery_replay_bytes = 37;
  mock.required_bytes = reservation;
  mock.free_bytes = reservation;
  mock.input_once = BC_STS_BUSY;
  mock.free_bytes_after_input_once = reservation - 1;
  mock.free_bytes_on_sleep = reservation;
  g_assert_cmpint(gst_harness_push(harness,
      new_input(120 * GST_MSECOND)), ==, GST_FLOW_OK);
  g_assert_cmpuint(mock.input_calls, ==, 3);
  g_assert_cmpuint(mock.accepted_count, ==, 2);
  g_assert_cmpuint(mock.unsafe_submissions, ==, 0);
  g_assert_cmpuint(self->timestamps.length, ==, 1);
  g_assert_false(self->recovery_needs_sync);

  mock.recovery_generation = 3;
  mock.free_bytes = reservation;
  g_assert_cmpint(gst_harness_push(harness,
      new_input(160 * GST_MSECOND)), ==, GST_FLOW_OK);
  g_assert_cmpuint(mock.input_calls, ==, 4);
  g_assert_cmpuint(mock.accepted_count, ==, 3);
  g_assert_cmpuint(mock.unsafe_submissions, ==, 0);
  g_assert_cmpuint(self->recovery_generation_seen, ==, 3);
  g_assert_false(self->recovery_needs_sync);
  gst_harness_teardown(harness);
}

static void
test_recovery_post_input_generation(void)
{
  static const guint8 valid_headers[] = {
    0, 0, 0, 1, 0x67, 0x64, 0x00, 0x1f,
    0, 0, 0, 1, 0x68, 0xee, 0x3c,
    0, 0, 0, 1, 0x65, 0x88, 0x84
  };
  GstHarness *harness = new_decoder();
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(harness->element);

  /* Recovery can complete immediately before/in input admission. Model its
   * one-shot config replay as consumed by the successful call while the new
   * getter contract continues to expose the persistent cache capability.
   * The ambiguous AU is retired exactly once and must never be resubmitted. */
  mock.auto_output = FALSE;
  mock.recovery_replay_bytes = 31;
  mock.recovery_replay_armed = TRUE;
  mock.recovery_after_input = TRUE;
  mock.recovery_after_input_complete = TRUE;
  mock.recovery_after_input_status = BC_STS_SUCCESS;
  g_assert_cmpint(gst_harness_push(harness, new_input(0)), ==, GST_FLOW_OK);
  g_assert_cmpuint(mock.input_calls, ==, 1);
  g_assert_cmpuint(mock.accepted_count, ==, 1);
  g_assert_cmpuint(mock.recovery_replay_consumptions, ==, 1);
  g_assert_false(mock.recovery_replay_armed);
  g_assert_cmpuint(self->timestamps.length, ==, 0);
  g_assert_cmpuint(self->recovery_generation_seen, ==, 1);
  g_assert_true(self->recovery_needs_sync);
  g_assert_cmpuint(self->recovery_h264_config_size, ==, 31);
  g_assert_cmpuint(mock.destroyed_inputs, ==, 1);

  g_assert_cmpint(gst_harness_push(harness, new_input(40 * GST_MSECOND)), ==,
                  GST_FLOW_OK);
  g_assert_cmpuint(mock.input_calls, ==, 2);
  g_assert_cmpuint(mock.accepted_count, ==, 2);
  g_assert_cmpuint(mock.accepted[0], !=, mock.accepted[1]);
  g_assert_cmpuint(self->timestamps.length, ==, 1);
  g_assert_cmpuint(self->recovery_generation_seen, ==, 1);
  g_assert_false(self->recovery_needs_sync);
  gst_harness_teardown(harness);

  /* Without a complete cache, the same ambiguity still drops the accepted
   * AU, but a following headerless IDR must not enter the reconstructed
   * decoder. A proven SPS/PPS AU is required to resume. */
  harness = new_decoder();
  self = GST_CRYSTALHD_DEC(harness->element);
  mock.auto_output = FALSE;
  mock.recovery_after_input = TRUE;
  mock.recovery_after_input_complete = TRUE;
  mock.recovery_after_input_status = BC_STS_SUCCESS;
  g_assert_cmpint(gst_harness_push(harness, new_input(0)), ==, GST_FLOW_OK);
  g_assert_cmpuint(mock.input_calls, ==, 1);
  g_assert_cmpuint(mock.accepted_count, ==, 1);
  g_assert_cmpuint(self->timestamps.length, ==, 0);
  g_assert_true(self->recovery_needs_sync);
  g_assert_cmpint(gst_harness_push(harness, new_input(40 * GST_MSECOND)), ==,
                  GST_FLOW_OK);
  g_assert_cmpuint(mock.input_calls, ==, 1);
  g_assert_cmpuint(mock.accepted_count, ==, 1);
  g_assert_cmpuint(self->timestamps.length, ==, 0);
  g_assert_true(self->recovery_needs_sync);
  g_assert_cmpint(gst_harness_push(harness,
      new_h264_input(80 * GST_MSECOND, valid_headers,
                     sizeof(valid_headers))), ==, GST_FLOW_OK);
  g_assert_cmpuint(mock.input_calls, ==, 2);
  g_assert_cmpuint(mock.accepted_count, ==, 2);
  g_assert_cmpuint(self->timestamps.length, ==, 1);
  g_assert_false(self->recovery_needs_sync);
  gst_harness_teardown(harness);

  /* An in-progress epoch after the call is equally ambiguous, including
   * when the library reports the PM status instead of SUCCESS. */
  harness = new_decoder();
  self = GST_CRYSTALHD_DEC(harness->element);
  mock.auto_output = FALSE;
  mock.recovery_replay_bytes = 31;
  mock.recovery_after_input = TRUE;
  mock.recovery_after_input_status = BC_STS_PWR_MGMT;
  g_assert_cmpint(gst_harness_push(harness, new_input(0)), ==, GST_FLOW_OK);
  g_assert_cmpuint(mock.input_calls, ==, 1);
  g_assert_cmpuint(mock.accepted_count, ==, 0);
  g_assert_cmpuint(self->timestamps.length, ==, 0);
  g_assert_true(self->recovery_needs_sync);
  g_assert_true(mock.recovery_active);
  mock.recovery_active = FALSE;
  gst_harness_teardown(harness);
}

static void
test_recovery_release_failure(void)
{
  static const guint8 valid_headers[] = {
    0, 0, 0, 1, 0x67, 0x64, 0x00, 0x1f,
    0, 0, 0, 1, 0x68, 0xee, 0x3c,
    0, 0, 0, 1, 0x65, 0x88, 0x84
  };
  GstHarness *harness = new_decoder();
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(harness->element);
  GstVideoCodecFrame *pending;
  GstBuffer *output;

  mock.auto_output = FALSE;
  g_assert_cmpint(gst_harness_push(harness, new_input(0)), ==, GST_FLOW_OK);
  queue_picture(mock.accepted[0], 0x51);
  mock.release_once = BC_STS_ERROR;
  g_assert_cmpint(gst_crystalhd_receive_available(self), ==, GST_FLOW_ERROR);
  g_assert_null(gst_harness_try_pull(harness));
  g_assert_cmpuint(mock.release_calls, ==, 1);
  g_assert_cmpuint(self->timestamps.length, ==, 0);
  GST_VIDEO_DECODER_STREAM_LOCK(self);
  pending = gst_video_decoder_get_oldest_frame(GST_VIDEO_DECODER(self));
  GST_VIDEO_DECODER_STREAM_UNLOCK(self);
  g_assert_null(pending);
  gst_harness_teardown(harness);

  harness = new_decoder();
  self = GST_CRYSTALHD_DEC(harness->element);
  mock.auto_output = FALSE;
  g_assert_cmpint(gst_harness_push(harness, new_input(0)), ==, GST_FLOW_OK);
  queue_picture(mock.accepted[0], 0x52);
  mock.release_once = BC_STS_PWR_MGMT;
  mock.release_latches_recovery = TRUE;
  g_assert_false(gst_crystalhd_output_iteration(self));
  g_assert_cmpint(self->output_flow, ==, GST_FLOW_OK);
  g_assert_null(gst_harness_try_pull(harness));
  g_assert_cmpuint(mock.release_calls, ==, 1);
  g_assert_cmpuint(self->timestamps.length, ==, 0);
  g_assert_true(mock.recovery_active);
  mock.recovery_active = FALSE;
  g_assert_false(gst_crystalhd_output_iteration(self));
  g_assert_cmpuint(self->recovery_generation_seen, ==, 1);
  g_assert_true(self->recovery_needs_sync);
  gst_harness_teardown(harness);

  harness = new_decoder();
  self = GST_CRYSTALHD_DEC(harness->element);
  mock.auto_output = FALSE;
  g_assert_cmpint(gst_harness_push(harness, new_input(0)), ==, GST_FLOW_OK);
  queue_picture(mock.accepted[0], 0x53);
  mock.output_once = BC_STS_IO_USER_ABORT;
  mock.output_latches_recovery = TRUE;
  g_assert_false(gst_crystalhd_output_iteration(self));
  g_assert_cmpint(self->output_flow, ==, GST_FLOW_OK);
  g_assert_true(mock.recovery_active);
  g_assert_cmpuint(self->timestamps.length, ==, 1);
  mock.complete_recovery_on_sleep = TRUE;
  lifecycle_sleep(1);
  g_assert_false(mock.recovery_active);
  g_assert_cmpuint(self->recovery_generation_seen, ==, 1);
  g_assert_cmpuint(self->timestamps.length, ==, 0);
  g_assert_true(self->recovery_needs_sync);
  g_assert_null(gst_harness_try_pull(harness));
  g_assert_cmpuint(mock.release_calls, ==, 0);
  mock.auto_output = TRUE;
  g_assert_cmpint(gst_harness_push(harness,
      new_h264_input(40 * GST_MSECOND, valid_headers,
                     sizeof(valid_headers))), ==, GST_FLOW_OK);
  output = gst_harness_try_pull(harness);
  check_output(output, 40 * GST_MSECOND, 2);
  gst_buffer_unref(output);
  g_assert_null(gst_harness_try_pull(harness));
  g_assert_cmpuint(mock.release_calls, ==, 2);
  gst_harness_teardown(harness);
}

static void
test_recovery_abort_bounded(void)
{
  static const BC_STATUS statuses[] = {
    BC_STS_IO_USER_ABORT, BC_STS_PWR_MGMT
  };
  guint i;

  for (i = 0; i < G_N_ELEMENTS(statuses); ++i) {
    GstHarness *harness = new_decoder();
    GstCrystalHdDec *self = GST_CRYSTALHD_DEC(harness->element);

    mock.auto_output = FALSE;
    g_assert_cmpint(gst_harness_push(harness, new_input(0)), ==, GST_FLOW_OK);
    queue_picture(mock.accepted[0], 0x61 + i);
    mock.output_once = statuses[i];
    g_assert_false(gst_crystalhd_output_iteration(self));
    g_assert_cmpint(self->output_flow, ==, GST_FLOW_OK);
    g_assert_cmpint(self->recovery_abort_started_us, >, 0);
    lifecycle_now += GST_CRYSTALHD_DRAIN_TIMEOUT_US;
    mock.output_once = statuses[i];
    g_assert_false(gst_crystalhd_output_iteration(self));
    g_assert_cmpint(self->output_flow, ==, GST_FLOW_ERROR);
    g_assert_cmpuint(self->timestamps.length, ==, 1);
    g_assert_null(gst_harness_try_pull(harness));
    gst_harness_teardown(harness);

    harness = new_decoder();
    self = GST_CRYSTALHD_DEC(harness->element);
    self->draining = TRUE;
    mock.eos_once = statuses[i];
    g_assert_false(gst_crystalhd_output_iteration(self));
    g_assert_cmpint(self->output_flow, ==, GST_FLOW_OK);
    g_assert_cmpint(self->recovery_abort_started_us, >, 0);
    lifecycle_now += GST_CRYSTALHD_DRAIN_TIMEOUT_US;
    mock.eos_once = statuses[i];
    g_assert_false(gst_crystalhd_output_iteration(self));
    g_assert_cmpint(self->output_flow, ==, GST_FLOW_ERROR);
    self->draining = FALSE;
    gst_harness_teardown(harness);
  }
}

static void
test_recovery_drain_eos(void)
{
  GstHarness *harness = new_decoder();
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(harness->element);
  GstBuffer *output;

  mock.auto_output = FALSE;
  g_assert_cmpint(gst_harness_push(harness, new_input(0)), ==, GST_FLOW_OK);
  queue_picture(mock.accepted[0], 0x62);
  mock.eos_once = BC_STS_IO_USER_ABORT;
  mock.eos_latches_recovery = TRUE;
  mock.complete_recovery_on_sleep = TRUE;
  g_assert_cmpint(gst_crystalhd_drain(GST_VIDEO_DECODER(self)), ==,
                  GST_FLOW_OK);
  output = gst_harness_try_pull(harness);
  check_output(output, 0, 0x62);
  gst_buffer_unref(output);
  g_assert_null(gst_harness_try_pull(harness));
  g_assert_cmpuint(mock.recovery_generation, ==, 1);
  g_assert_cmpuint(self->recovery_generation_seen, ==, 1);
  g_assert_false(mock.recovery_active);
  g_assert_true(self->output_eos);
  g_assert_true(self->drain_idle);
  g_assert_cmpuint(self->timestamps.length, ==, 0);
  gst_harness_teardown(harness);
}

static void
test_recovery_close_semantics(void)
{
  GstHarness *harness;
  GstCrystalHdDec *self;
  BC_STATUS status;

  harness = new_decoder();
  self = GST_CRYSTALHD_DEC(harness->element);
  mock.recovery_query_once = BC_STS_ERROR;
  status = gst_crystalhd_close_device(self);
  g_assert_cmpint(status, ==, BC_STS_ERROR);
  g_assert_nonnull(self->device);
  g_assert_true(mock.device_active);
  g_assert_cmpuint(mock.close_calls, ==, 0);
  status = gst_crystalhd_close_device(self);
  g_assert_cmpint(status, ==, BC_STS_SUCCESS);
  g_assert_null(self->device);
  g_assert_cmpuint(mock.close_calls, ==, 1);
  gst_harness_teardown(harness);

  harness = new_decoder();
  self = GST_CRYSTALHD_DEC(harness->element);
  mock.recovery_generation = 1;
  mock.recovery_active = TRUE;
  mock.recovery_query_advance_timeout = TRUE;
  status = gst_crystalhd_close_device(self);
  g_assert_cmpint(status, ==, BC_STS_BUSY);
  g_assert_nonnull(self->device);
  g_assert_true(mock.device_active);
  g_assert_cmpuint(mock.stop_calls, ==, 0);
  g_assert_cmpuint(mock.close_calls, ==, 0);
  mock.recovery_active = FALSE;
  status = gst_crystalhd_close_device(self);
  g_assert_cmpint(status, ==, BC_STS_SUCCESS);
  g_assert_null(self->device);
  g_assert_cmpuint(mock.stop_calls, ==, 1);
  g_assert_cmpuint(mock.close_calls, ==, 1);
  gst_harness_teardown(harness);

  harness = new_decoder();
  self = GST_CRYSTALHD_DEC(harness->element);
  mock.recovery_generation = 1;
  mock.recovery_active = TRUE;
  mock.complete_recovery_on_sleep = TRUE;
  status = gst_crystalhd_close_device(self);
  g_assert_cmpint(status, ==, BC_STS_SUCCESS);
  g_assert_null(self->device);
  g_assert_cmpuint(mock.close_calls, ==, 1);
  g_assert_false(mock.recovery_active);
  g_assert_cmpuint(mock.start_calls, ==, 1);
  gst_harness_teardown(harness);

  harness = new_decoder();
  self = GST_CRYSTALHD_DEC(harness->element);
  mock.device_close_transient_status = BC_STS_BUSY;
  mock.device_close_transient_count = 1;
  status = gst_crystalhd_close_device(self);
  g_assert_cmpint(status, ==, BC_STS_SUCCESS);
  g_assert_null(self->device);
  g_assert_cmpuint(mock.close_calls, ==, 2);
  gst_harness_teardown(harness);

  harness = new_decoder();
  self = GST_CRYSTALHD_DEC(harness->element);
  mock.device_close_transient_status = BC_STS_BUSY;
  mock.device_close_transient_count = 1;
  mock.device_close_advance_timeout = TRUE;
  status = gst_crystalhd_close_device(self);
  g_assert_cmpint(status, ==, BC_STS_BUSY);
  g_assert_nonnull(self->device);
  g_assert_true(mock.device_active);
  g_assert_cmpuint(mock.close_calls, ==, 1);
  mock.device_close_advance_timeout = FALSE;
  status = gst_crystalhd_close_device(self);
  g_assert_cmpint(status, ==, BC_STS_SUCCESS);
  g_assert_null(self->device);
  g_assert_cmpuint(mock.close_calls, ==, 2);
  gst_harness_teardown(harness);

  harness = new_decoder();
  self = GST_CRYSTALHD_DEC(harness->element);
  mock.fail_operations = 1U << MOCK_STOP;
  status = gst_crystalhd_close_device(self);
  g_assert_cmpint(status, ==, BC_STS_ERROR);
  g_assert_null(self->device);
  g_assert_false(mock.device_active);
  g_assert_cmpuint(mock.stop_calls, ==, 1);
  g_assert_cmpuint(mock.decoder_close_calls, ==, 1);
  g_assert_cmpuint(mock.close_calls, ==, 1);
  mock.fail_operations = 0;
  gst_harness_teardown(harness);

  harness = new_decoder();
  self = GST_CRYSTALHD_DEC(harness->element);
  mock.recovery_generation = 1;
  mock.recovery_status = BC_STS_PWR_MGMT;
  status = gst_crystalhd_close_device(self);
  g_assert_cmpint(status, ==, BC_STS_SUCCESS);
  g_assert_null(self->device);
  g_assert_cmpuint(mock.stop_calls, ==, 0);
  g_assert_cmpuint(mock.decoder_close_calls, ==, 0);
  g_assert_cmpuint(mock.close_calls, ==, 1);
  gst_harness_teardown(harness);
}

static void
test_recovery_finalizer_retained_handle(void)
{
  guint mode;

  for (mode = 0; mode < 3; ++mode) {
    GstCrystalHdDec *self;

    memset(&mock, 0, sizeof(mock));
    g_queue_init(&mock.pictures);
    lifecycle_now = 1;
    iteration_decoder = NULL;
    flush_during_wait = NULL;
    seek_during_wait = NULL;
    flush_during_quiesce = NULL;
    mock.device_active = TRUE;
    mock.opened = TRUE;
    mock.started = TRUE;
    if (mode == 0) {
      /* No RESUME ever arrives. Terminal DeviceClose owns cancellation;
       * BUSY is the only non-consuming result and must be retried. */
      mock.recovery_generation = 1;
      mock.recovery_active = TRUE;
      mock.device_close_transient_status = BC_STS_BUSY;
      mock.device_close_transient_count = 2;
    } else if (mode == 1) {
      /* A failed status query retained this handle in stop(). Finalization
       * must not query again: the raw close is now the authoritative owner. */
      mock.recovery_query_once = BC_STS_ERROR;
    } else
      mock.fail_operations = 1U << MOCK_DEVICE_CLOSE;
    self = g_object_new(GST_TYPE_CRYSTALHD_DEC, NULL);
    gst_object_ref_sink(self);
    self->device = &mock;
    self->decoder_open = TRUE;
    self->decoder_started = TRUE;
    g_assert_null(self->output_thread);
    gst_object_unref(self);
    g_assert_false(mock.device_active);
    g_assert_cmpuint(mock.recovery_query_calls, ==, 0);
    g_assert_cmpuint(mock.stop_calls, ==, 0);
    g_assert_cmpuint(mock.decoder_close_calls, ==, 0);
    g_assert_cmpuint(mock.close_calls, ==, mode == 0 ? 3 : 1);
  }
}

int
main(int argc, char **argv)
{
  int result;
  gst_init(&argc, &argv);
  g_test_init(&argc, &argv, NULL);
  GST_DEBUG_CATEGORY_INIT(gst_crystalhd_debug, "crystalhd", 0,
                          "CrystalHD lifecycle test");
  g_test_add_func("/crystalhd/scale/caps-query", test_scale_caps_query);
  g_test_add_func("/crystalhd/scale/default", test_scale_property_default);
  g_test_add_func("/crystalhd/scale/property-lock",
                  test_scale_property_does_not_take_state_lock);
  g_test_add_func("/crystalhd/scale/valid-order",
                  test_scale_valid_widths_and_order);
  g_test_add_func("/crystalhd/scale/invalid", test_scale_invalid_requests);
  g_test_add_func("/crystalhd/scale/reopen", test_scale_reapplied_on_reopen);
  g_test_add_func("/crystalhd/scale/library-failure",
                  test_scale_library_failure);
  g_test_add_func("/crystalhd/scale/output-contract",
                  test_scale_output_contract);
  g_test_add_func("/crystalhd/lifecycle/frame-references", test_frame_references);
  g_test_add_func("/crystalhd/lifecycle/natural-caps-tail", test_natural_caps_tail);
  g_test_add_func("/crystalhd/lifecycle/natural-caps-errors", test_natural_caps_errors);
  g_test_add_func("/crystalhd/lifecycle/natural-caps-raw-tail", test_natural_caps_raw_tail);
  g_test_add_func("/crystalhd/lifecycle/natural-caps-raw-boundaries", test_natural_caps_raw_boundaries);
  g_test_add_func("/crystalhd/recovery/epoch-reconcile",
                  test_recovery_epoch_reconcile);
  g_test_add_func("/crystalhd/recovery/config-reservation-boundaries",
                  test_recovery_config_reservation_boundaries);
  g_test_add_func("/crystalhd/recovery/input-deadline",
                  test_recovery_input_deadline);
  g_test_add_func("/crystalhd/recovery/blocked-delivery-reconcile",
                  test_recovery_blocked_delivery_reconcile);
  g_test_add_func("/crystalhd/recovery/h264-gate-reservation",
                  test_recovery_h264_gate_and_reservation);
  g_test_add_func("/crystalhd/recovery/post-input-generation",
                  test_recovery_post_input_generation);
  g_test_add_func("/crystalhd/recovery/release-failure",
                  test_recovery_release_failure);
  g_test_add_func("/crystalhd/recovery/abort-bounded",
                  test_recovery_abort_bounded);
  g_test_add_func("/crystalhd/recovery/drain-eos",
                  test_recovery_drain_eos);
  g_test_add_func("/crystalhd/recovery/close-semantics",
                  test_recovery_close_semantics);
  g_test_add_func("/crystalhd/recovery/finalizer-retained-handle",
                  test_recovery_finalizer_retained_handle);
  g_test_add_func("/crystalhd/lifecycle/busy-stale-output", test_busy_stale_output);
  g_test_add_func("/crystalhd/lifecycle/flush-stale-output", test_flush_stale_output);
  g_test_add_func("/crystalhd/lifecycle/flush-reopen-errors", test_flush_reopen_errors);
  g_test_add_func("/crystalhd/lifecycle/stop-cleanup-errors", test_stop_cleanup_errors);
  g_test_add_func("/crystalhd/lifecycle/flush-codec-state", test_flush_codec_state);
  g_test_add_func("/crystalhd/lifecycle/mpeg4-device-gate", test_mpeg4_device_gate);
  g_test_add_func("/crystalhd/lifecycle/reordered-duplicate-output", test_reordered_duplicate_output);
  g_test_add_func("/crystalhd/lifecycle/wmv3-display-order-pts",
                  test_wmv3_display_order_pts);
  g_test_add_func("/crystalhd/lifecycle/wmv3-seek-clip-uses-pts-cadence",
                  test_wmv3_seek_clip_uses_pts_cadence);
  g_test_add_func("/crystalhd/lifecycle/wmv3-variable-pts-duration",
                  test_wmv3_variable_pts_duration);
  g_test_add_func("/crystalhd/lifecycle/wmv3-millisecond-duration",
                  test_wmv3_millisecond_duration);
  g_test_add_func("/crystalhd/lifecycle/wmv3-sparse-pts-disables-timing",
                  test_wmv3_sparse_pts_disables_timing);
  g_test_add_func("/crystalhd/lifecycle/input-errors", test_input_errors);
  g_test_add_func("/crystalhd/lifecycle/repeated-empty-flush", test_repeated_empty_flush);
  g_test_add_func("/crystalhd/lifecycle/status-probe-no-ready-peek",
                  test_status_probe_no_ready_peek);
  g_test_add_func("/crystalhd/lifecycle/incomplete-drain", test_incomplete_drain);
  g_test_add_func("/crystalhd/lifecycle/mpeg2-open-gop-admission", test_mpeg2_open_gop_admission);
  g_test_add_func("/crystalhd/lifecycle/mpeg2-closed-gop-and-discontinuity", test_mpeg2_closed_gop_and_discontinuity);
  g_test_add_func("/crystalhd/lifecycle/mpeg2-missing-valid-output", test_mpeg2_missing_valid_output_still_fails);
  g_test_add_func("/crystalhd/lifecycle/mpeg2-damaged-reference-recovery", test_mpeg2_damaged_reference_recovery);
  g_test_add_func("/crystalhd/lifecycle/mpeg2-field-reference-proof", test_mpeg2_field_reference_proof_unchanged);
  g_test_add_func("/crystalhd/lifecycle/mpeg2-new-sequence-profile", test_mpeg2_new_sequence_invalidates_profile);
  g_test_add_func("/crystalhd/lifecycle/mpeg2-positive-prediction-proof", test_mpeg2_positive_prediction_proof);
  g_test_add_func("/crystalhd/lifecycle/eos-with-ready-output", test_eos_with_ready_output);
  g_test_add_func("/crystalhd/lifecycle/eos-before-late-output",
                  test_eos_before_late_output);
  g_test_add_func("/crystalhd/lifecycle/drain-completion-requires-idle",
                  test_drain_completion_requires_idle);
  g_test_add_func("/crystalhd/lifecycle/field-identity", test_field_identity);
  g_test_add_func("/crystalhd/lifecycle/whole-weave-output", test_whole_weave_output);
  g_test_add_func("/crystalhd/lifecycle/whole-weave-malformed", test_whole_weave_rejects_malformed);
  g_test_add_func("/crystalhd/lifecycle/whole-weave-pending-half", test_whole_weave_rejects_pending_half);
  g_test_add_func("/crystalhd/lifecycle/whole-weave-token-lifetime", test_whole_weave_token_lifetime);
  g_test_add_func("/crystalhd/lifecycle/whole-weave-70012", test_whole_weave_70012_unchanged);
  g_test_add_func("/crystalhd/lifecycle/field-pair-policy", test_field_pair_policy);
  g_test_add_func("/crystalhd/lifecycle/field-presentation-order", test_field_presentation_order);
  g_test_add_func("/crystalhd/lifecycle/output-interlace-mode-changes", test_output_interlace_mode_changes);
  g_test_add_func("/crystalhd/lifecycle/progressive-soft-rff-cadence",
                  test_progressive_soft_rff_cadence);
  g_test_add_func("/crystalhd/lifecycle/progressive-pulldown-unsupported",
                  test_progressive_pulldown_unsupported);
  g_test_add_func("/crystalhd/lifecycle/progressive-pulldown-format-reopen",
                  test_progressive_pulldown_format_reopen);
  g_test_add_func("/crystalhd/lifecycle/interlaced-picture-types", test_interlaced_picture_types);
  g_test_add_func("/crystalhd/lifecycle/input-admission", test_input_admission);
  g_test_add_func("/crystalhd/lifecycle/mpeg4-simple-missing-timestamp",
                  test_mpeg4_simple_missing_timestamp);
  g_test_add_func("/crystalhd/lifecycle/mpeg4-uncoded-vop",
                  test_mpeg4_uncoded_vop);
  g_test_add_func("/crystalhd/lifecycle/mpeg4-simple-invalid-ordinal-baseline",
                  test_mpeg4_simple_invalid_ordinal_baseline);
  g_test_add_func("/crystalhd/lifecycle/mpeg4-simple-order-violation",
                  test_mpeg4_simple_order_violation);
  g_test_add_func("/crystalhd/lifecycle/mpeg4-simple-duplicate-zero-timestamp",
                  test_mpeg4_simple_duplicate_zero_timestamp);
  g_test_add_func("/crystalhd/lifecycle/mpeg4-unrecoverable-zero-timestamp",
                  test_mpeg4_unrecoverable_zero_timestamp);
  g_test_add_func("/crystalhd/lifecycle/input-admission-timeout", test_input_admission_timeout);
  g_test_add_func("/crystalhd/lifecycle/input-admission-flush", test_input_admission_flush);
  g_test_add_func("/crystalhd/lifecycle/input-wait-new-generation", test_input_wait_new_generation);
  g_test_add_func("/crystalhd/lifecycle/eos-admission", test_eos_admission);
  g_test_add_func("/crystalhd/lifecycle/oversized-input-admission", test_oversized_input_admission);
  g_test_add_func("/crystalhd/lifecycle/input-reservation-boundaries", test_input_reservation_boundaries);
  g_test_add_func("/crystalhd/lifecycle/metadata-admission", test_metadata_admission);
  g_test_add_func("/crystalhd/lifecycle/autonomous-output", test_autonomous_output);
  g_test_add_func("/crystalhd/lifecycle/vc1-recursive-input-wait", test_vc1_recursive_input_wait);
  g_test_add_data_func("/crystalhd/lifecycle/blocked-delivery-input", GINT_TO_POINTER(FALSE),
                       test_blocked_delivery);
  g_test_add_data_func("/crystalhd/lifecycle/blocked-delivery-drain", GINT_TO_POINTER(TRUE),
                       test_blocked_delivery);
  g_test_add_data_func("/crystalhd/lifecycle/blocked-delivery-flush", GINT_TO_POINTER(2),
                       test_blocked_delivery);
  g_test_add_data_func("/crystalhd/lifecycle/blocked-delivery-caps", GINT_TO_POINTER(3),
                       test_blocked_delivery);
  result = g_test_run();
  gst_deinit();
  return result;
}
