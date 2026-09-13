/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Exercise the real GstVideoDecoder subclass with an in-process fake library.
 * No CrystalHD library is linked, and no hardware device can be opened.
 */
#include <gst/check/gstharness.h>

static gint64 lifecycle_time(void);
static void lifecycle_sleep(gulong usecs);
#define g_get_monotonic_time lifecycle_time
#define g_usleep lifecycle_sleep
#include "../filters/gst/gst-plugin-1.0/gstcrystalhd.c"
#undef g_get_monotonic_time
#undef g_usleep

static gint64 lifecycle_now;
static GstHarness *flush_during_wait;

typedef struct {
  guint64 timestamp;
  guint8 pixel;
  guint flags;
} MockPicture;

static struct {
  GQueue pictures;
  gboolean opened;
  gboolean started;
  gboolean auto_output;
  gboolean busy_stale;
  gboolean fail_input;
  gboolean fail_status;
  gboolean early_eos;
  guint input_calls;
  guint flush_calls;
  guint drain_calls;
  guint release_calls;
  guint destroyed_inputs;
  guint free_bytes;
  guint required_bytes;
  guint unsafe_submissions;
  guint open_calls;
  gboolean release_makes_room;
  guint64 accepted[16];
  guint accepted_count;
  guint8 pixels[16 * 16 * 2];
} mock;

static gint64
lifecycle_time(void)
{
  return lifecycle_now;
}

static void
lifecycle_sleep(gulong usecs)
{
  lifecycle_now += usecs;
  if (flush_during_wait != NULL) {
    GstHarness *harness = flush_during_wait;
    flush_during_wait = NULL;
    g_assert_true(gst_harness_push_event(harness, gst_event_new_flush_start()));
  }
}

static void
queue_picture(guint64 timestamp, guint8 pixel)
{
  MockPicture *picture = g_new0(MockPicture, 1);
  picture->timestamp = timestamp;
  picture->pixel = pixel;
  g_queue_push_tail(&mock.pictures, picture);
}

BC_STATUS DtsDeviceOpen(HANDLE *device, uint32_t mode)
{
  (void)mode;
  mock.open_calls++;
  *device = &mock;
  return BC_STS_SUCCESS;
}

BC_STATUS DtsDeviceClose(HANDLE device)
{
  g_assert_true(device == &mock);
  g_queue_clear_full(&mock.pictures, g_free);
  return BC_STS_SUCCESS;
}

BC_STATUS DtsCrystalHDVersion(HANDLE device, PBC_INFO_CRYSTAL version)
{
  (void)device;
  version->device = 1;
  return BC_STS_SUCCESS;
}

BC_STATUS DtsSetInputFormat(HANDLE device, BC_INPUT_FORMAT *format)
{
  (void)device;
  (void)format;
  return BC_STS_SUCCESS;
}

BC_STATUS DtsOpenDecoder(HANDLE device, uint32_t stream_type)
{
  (void)device;
  (void)stream_type;
  mock.opened = TRUE;
  return BC_STS_SUCCESS;
}

BC_STATUS DtsCloseDecoder(HANDLE device)
{
  (void)device;
  mock.opened = FALSE;
  return BC_STS_SUCCESS;
}

BC_STATUS DtsStartDecoder(HANDLE device)
{
  (void)device;
  mock.started = TRUE;
  return BC_STS_SUCCESS;
}

BC_STATUS DtsStopDecoder(HANDLE device)
{
  (void)device;
  mock.started = FALSE;
  return BC_STS_SUCCESS;
}

BC_STATUS DtsStartCapture(HANDLE device)
{
  (void)device;
  return BC_STS_SUCCESS;
}

BC_STATUS DtsSetColorSpace(HANDLE device, BC_OUTPUT_FORMAT format)
{
  (void)device;
  (void)format;
  return BC_STS_SUCCESS;
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
  /* DtsProcInput lazily reopens the decoder after DtsFlushInput(4). */
  mock.opened = TRUE;
  mock.started = TRUE;
  if (mock.fail_input)
    return BC_STS_ERROR;
  if (mock.busy_stale) {
    mock.busy_stale = FALSE;
    queue_picture(timestamp + 123, 0xee);
    return BC_STS_BUSY;
  }
  g_assert_cmpuint(mock.accepted_count, <, G_N_ELEMENTS(mock.accepted));
  mock.accepted[mock.accepted_count++] = timestamp;
  if (mock.auto_output)
    queue_picture(timestamp, mock.accepted_count);
  return BC_STS_SUCCESS;
}

BC_STATUS DtsGetDriverStatus(HANDLE device, BC_DTS_STATUS *status)
{
  (void)device;
  if (mock.fail_status)
    return BC_STS_ERROR;
  memset(status, 0, sizeof(*status));
  status->ReadyListCount = mock.pictures.length;
  return BC_STS_SUCCESS;
}

uint32_t DtsTxFreeSize(HANDLE device)
{
  (void)device;
  return mock.free_bytes;
}

BC_STATUS DtsProcOutputNoCopy(HANDLE device, uint32_t timeout,
                             BC_DTS_PROC_OUT *output)
{
  MockPicture *picture = g_queue_pop_head(&mock.pictures);
  (void)device;
  (void)timeout;
  if (picture == NULL)
    return BC_STS_NO_DATA;
  memset(mock.pixels, picture->pixel, sizeof(mock.pixels));
  memset(output, 0, sizeof(*output));
  output->PoutFlags = BC_POUT_FLAGS_PIB_VALID;
  output->PicInfo.width = 16;
  output->PicInfo.height = 16;
  output->PicInfo.timeStamp = picture->timestamp;
  output->PicInfo.flags = picture->flags;
  output->Ybuff = mock.pixels;
  output->YBuffDoneSz = sizeof(mock.pixels) / 4;
  g_free(picture);
  return BC_STS_SUCCESS;
}

BC_STATUS DtsReleaseOutputBuffs(HANDLE device, void *reserved, BOOL change)
{
  (void)device;
  (void)reserved;
  (void)change;
  mock.release_calls++;
  if (mock.release_makes_room)
    mock.free_bytes = 1024 * 1024;
  memset(mock.pixels, 0xcc, sizeof(mock.pixels));
  return BC_STS_SUCCESS;
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
    return BC_STS_SUCCESS;
  }
  g_assert_cmpuint(operation, ==, 4);
  mock.flush_calls++;
  mock.opened = FALSE;
  mock.started = FALSE;
  g_queue_clear_full(&mock.pictures, g_free);
  return BC_STS_SUCCESS;
}

BC_STATUS DtsIsEndOfStream(HANDLE device, uint8_t *eos)
{
  (void)device;
  *eos = mock.early_eos || g_queue_is_empty(&mock.pictures);
  return BC_STS_SUCCESS;
}

static GstHarness *
new_decoder(void)
{
  GstElement *element;
  GstHarness *harness;
  memset(&mock, 0, sizeof(mock));
  lifecycle_now = 1;
  flush_during_wait = NULL;
  g_queue_init(&mock.pictures);
  mock.auto_output = TRUE;
  mock.free_bytes = 1024 * 1024;
  element = g_object_new(GST_TYPE_CRYSTALHD_DEC, NULL);
  harness = gst_harness_new_with_element(element, "sink", "src");
  gst_object_unref(element); /* the harness takes its own reference */
  gst_harness_set_src_caps_str(harness,
      "video/x-h264,stream-format=byte-stream,alignment=au,parsed=true,"
      "width=16,height=16,framerate=25/1");
  return harness;
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

static void
check_output(GstBuffer *buffer, GstClockTime pts, guint8 pixel)
{
  guint8 bytes[16 * 16 * 2];
  guint i;
  g_assert_nonnull(buffer);
  g_assert_cmpuint(GST_BUFFER_PTS(buffer), ==, pts);
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
  GstBuffer *output;
  GstSegment segment;
  guint64 old_timestamp;
  mock.auto_output = FALSE;
  g_assert_cmpint(gst_harness_push(harness, new_input(0)), ==, GST_FLOW_OK);
  old_timestamp = mock.accepted[0];
  flush_decoder(harness);
  gst_segment_init(&segment, GST_FORMAT_TIME);
  segment.start = GST_SECOND;
  segment.position = GST_SECOND;
  g_assert_true(gst_harness_push_event(harness, gst_event_new_segment(&segment)));
  queue_picture(old_timestamp, 0xee);
  mock.auto_output = TRUE;
  g_assert_cmpint(gst_harness_push(harness, new_input(GST_SECOND)), ==, GST_FLOW_OK);
  output = gst_harness_try_pull(harness);
  check_output(output, GST_SECOND, 2);
  gst_buffer_unref(output);
  g_assert_cmpuint(gst_harness_buffers_in_queue(harness), ==, 0);
  g_assert_cmpuint(mock.destroyed_inputs, ==, 2);
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
  g_assert_cmpuint(mock.destroyed_inputs, ==, 1);
  g_assert_cmpuint(GST_CRYSTALHD_DEC(harness->element)->timestamps.length, ==, 0);
  gst_harness_teardown(harness);

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
  /* No input between two real library-close flushes / a seek to empty EOS. */
  g_assert_true(gst_crystalhd_flush(decoder));
  g_assert_true(gst_crystalhd_flush(decoder));
  g_assert_cmpint(gst_crystalhd_drain(decoder), ==, GST_FLOW_OK);
  g_assert_cmpuint(mock.flush_calls, ==, 1);
  g_assert_cmpuint(mock.drain_calls, ==, 0);
  /* Lazy reopen must still accept more input, then permit another flush. */
  g_assert_cmpint(gst_harness_push(harness, new_input(0)), ==, GST_FLOW_OK);
  gst_buffer_unref(gst_harness_try_pull(harness));
  g_assert_true(gst_crystalhd_flush(decoder));
  g_assert_cmpuint(mock.flush_calls, ==, 2);
  g_assert_true(gst_harness_push_event(harness, gst_event_new_eos()));
  g_assert_cmpuint(mock.drain_calls, ==, 0);
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
  mock.required_bytes = 232;
  mock.free_bytes = 0;
  flush_during_wait = harness;
  g_assert_cmpint(gst_harness_push(harness, new_input(0)), ==, GST_FLOW_FLUSHING);
  g_assert_cmpuint(mock.input_calls, ==, 0);
  g_assert_cmpuint(mock.unsafe_submissions, ==, 0);
  g_assert_cmpint(lifecycle_now, <, 10 * G_USEC_PER_SEC);
  g_assert_cmpuint(mock.destroyed_inputs, ==, 1);
  gst_harness_teardown(harness);
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
    BC_MSUBTYPE_WVC1, BC_MSUBTYPE_WMV3
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
      if (codecs[i] == BC_MSUBTYPE_WVC1)
        expanded += 4 + metadata;
      if (codecs[i] == BC_MSUBTYPE_WMV3)
        expanded += 48 + 32;
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
                   (bottom ? VDEC_FLAG_BOTTOMFIELD : 0);
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

int
main(int argc, char **argv)
{
  int result;
  gst_init(&argc, &argv);
  g_test_init(&argc, &argv, NULL);
  GST_DEBUG_CATEGORY_INIT(gst_crystalhd_debug, "crystalhd", 0,
                          "CrystalHD lifecycle test");
  g_test_add_func("/crystalhd/lifecycle/frame-references", test_frame_references);
  g_test_add_func("/crystalhd/lifecycle/busy-stale-output", test_busy_stale_output);
  g_test_add_func("/crystalhd/lifecycle/flush-stale-output", test_flush_stale_output);
  g_test_add_func("/crystalhd/lifecycle/reordered-duplicate-output", test_reordered_duplicate_output);
  g_test_add_func("/crystalhd/lifecycle/input-errors", test_input_errors);
  g_test_add_func("/crystalhd/lifecycle/repeated-empty-flush", test_repeated_empty_flush);
  g_test_add_func("/crystalhd/lifecycle/incomplete-drain", test_incomplete_drain);
  g_test_add_func("/crystalhd/lifecycle/eos-with-ready-output", test_eos_with_ready_output);
  g_test_add_func("/crystalhd/lifecycle/field-identity", test_field_identity);
  g_test_add_func("/crystalhd/lifecycle/input-admission", test_input_admission);
  g_test_add_func("/crystalhd/lifecycle/input-admission-timeout", test_input_admission_timeout);
  g_test_add_func("/crystalhd/lifecycle/input-admission-flush", test_input_admission_flush);
  g_test_add_func("/crystalhd/lifecycle/eos-admission", test_eos_admission);
  g_test_add_func("/crystalhd/lifecycle/oversized-input-admission", test_oversized_input_admission);
  g_test_add_func("/crystalhd/lifecycle/input-reservation-boundaries", test_input_reservation_boundaries);
  g_test_add_func("/crystalhd/lifecycle/metadata-admission", test_metadata_admission);
  result = g_test_run();
  gst_deinit();
  return result;
}
