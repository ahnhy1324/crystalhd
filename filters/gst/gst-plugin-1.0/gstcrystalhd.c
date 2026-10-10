/*
 * GStreamer 1.x decoder for Broadcom Crystal HD BCM70012/BCM70015 devices.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <stdint.h>
#include <string.h>

#include <gst/gst.h>
#include <gst/base/gstadapter.h>
#include <gst/video/gstvideodecoder.h>
#include <gst/video/video.h>

#include <bc_dts_defs.h>
#include <bc_dts_types.h>
#include <libcrystalhd_if.h>
#include "gstcrystalhd-codecs.h"
#include "gstcrystalhd-input.h"
#include "gstcrystalhd-timing.h"

G_STATIC_ASSERT(GST_CRYSTALHD_MPEG4_MAX_INPUT_SIZE ==
                GST_CRYSTALHD_INPUT_CAPACITY);

#define GST_TYPE_CRYSTALHD_DEC (gst_crystalhd_dec_get_type())
#define GST_CRYSTALHD_DEC(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_CRYSTALHD_DEC, GstCrystalHdDec))

#define CRYSTALHD_TIMESTAMP_STEP 100000ULL

typedef struct {
  guint64 hardware_timestamp;
  guint32 frame_number;
} CrystalHdTimestamp;

typedef struct _GstCrystalHdDec {
  GstVideoDecoder parent;

  HANDLE device;
  gboolean decoder_open;
  gboolean decoder_started;
  gboolean input_flushed;
  gboolean is_70012;
  gboolean output_configured;
  gboolean progressive_field_sequence;
  gboolean need_second_field;
  gboolean field_bottom;
  gboolean field_bottom_first;
  CrystalHdCodec codec;
  gsize input_metadata_size;
  guint32 field_frame_number;
  guint field_width;
  guint field_height;
  guint width;
  guint height;
  guint64 next_hardware_timestamp;
  guint32 simple_picture_number;
  gboolean have_simple_picture_number;
  GQueue timestamps;
  GstVideoCodecState *input_state;
  GstAdapter *parse_adapter;
  gboolean parse_pending;
  GstVideoInfo output_info;

  /* The decoder stream lock protects the device, timestamps and flow state.
   * output_lock only protects worker scheduling and the delivery barrier;
   * never take the stream lock while holding output_lock. */
  GMutex output_lock;
  GCond output_cond;
  GThread *output_thread;
  gboolean output_stop;
  gboolean output_paused;
  gboolean output_active;
  GstFlowReturn output_flow;
  guint64 generation;
  gboolean draining;
  gboolean output_eos;
  gboolean drain_idle;
  gboolean parsing_frame;
  gint64 last_delivery_us;
} GstCrystalHdDec;

typedef struct _GstCrystalHdDecClass {
  GstVideoDecoderClass parent_class;
} GstCrystalHdDecClass;

G_DEFINE_TYPE(GstCrystalHdDec, gst_crystalhd_dec, GST_TYPE_VIDEO_DECODER)

GST_DEBUG_CATEGORY_STATIC(gst_crystalhd_debug);
#define GST_CAT_DEFAULT gst_crystalhd_debug

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE(
    "sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS(
        "video/x-h264, stream-format=(string)byte-stream, "
        "alignment=(string)au, parsed=(boolean)true; "
        "video/mpeg, mpegversion=(int)2, systemstream=(boolean)false, "
        "parsed=(boolean)true; "
        "video/mpeg, mpegversion=(int)4, systemstream=(boolean)false, "
        "parsed=(boolean)true, profile=(string){simple,advanced-simple}, "
        "level=(string){\"3\",\"5\"}, width=(int)[1,1920], "
        "height=(int)[1,1088]; "
        "video/x-vc1, parsed=(boolean)true; "
        "video/x-wmv, wmvversion=(int)3, format=(string)WVC1, "
        "stream-format=(string){bdu,bdu-frame}, header-format=(string)none; "
        "video/x-wmv, wmvversion=(int)3, format=(string)WVC1, "
        "stream-format=(string)asf, header-format=(string)asf; "
        "video/x-wmv, wmvversion=(int)3, format=(string)WMV3, "
        "stream-format=(string){frame-layer,asf}, header-format=(string)asf"));

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE(
    "src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS("video/x-raw, format=(string)YUY2, "
                    "width=(int)[1,1920], height=(int)[1,1088], "
                    "framerate=(fraction)[0/1,MAX]"));

static gboolean
gst_crystalhd_caps_have_mpeg4_metadata(GstCaps *caps,
                                       const CrystalHdCodec *codec)
{
  const GstStructure *structure = gst_caps_get_structure(caps, 0);
  const GValue *value = gst_structure_get_value(structure, "codec_data");
  GstBuffer *buffer;
  GstMapInfo map;
  const guint8 *data;
  gsize size;
  gboolean valid;
  CrystalHdCodec checked_codec = *codec;

  if (value == NULL || !GST_VALUE_HOLDS_BUFFER(value))
    return FALSE;
  buffer = gst_value_get_buffer(value);
  if (buffer == NULL || !gst_buffer_map(buffer, &map, GST_MAP_READ))
    return FALSE;
  data = map.data;
  size = map.size;
  valid = gst_crystalhd_codec_metadata(&checked_codec, &data, &size);
  gst_buffer_unmap(buffer, &map);
  return valid;
}

static gboolean
gst_crystalhd_sink_query(GstVideoDecoder *decoder, GstQuery *query)
{
  GstVideoDecoderClass *parent =
      GST_VIDEO_DECODER_CLASS(gst_crystalhd_dec_parent_class);

  if (GST_QUERY_TYPE(query) == GST_QUERY_ACCEPT_CAPS) {
    GstCaps *caps;
    gst_query_parse_accept_caps(query, &caps);
    if (gst_caps_is_fixed(caps)) {
      CrystalHdCodec codec;

      if (!gst_crystalhd_codec_from_caps(caps, &codec)) {
        gst_query_set_accept_caps_result(query, FALSE);
        return TRUE;
      }
      if (codec.subtype == BC_MSUBTYPE_DIVX &&
          !gst_crystalhd_caps_have_mpeg4_metadata(caps, &codec)) {
        gst_query_set_accept_caps_result(query, FALSE);
        return TRUE;
      }
      if (gst_structure_has_name(gst_caps_get_structure(caps, 0),
                                 "video/x-wmv")) {
        GstCaps *normalized;
        GstStructure *s;
        GstQuery *check;
        gboolean accepted = FALSE;
        gboolean result;

        /* asfdemux omits stream-format/header-format (and older producers
         * also omit format). Those caps still describe ASF packets. Normalize
         * only this query, retaining strict advertised parser framing and the
         * original caps/codec_data for set_format.
         */
        normalized = gst_caps_copy(caps);
        s = gst_caps_get_structure(normalized, 0);
        if (!gst_structure_has_field(s, "format"))
          gst_structure_set(s, "format", G_TYPE_STRING, "WMV3", NULL);
        if (!gst_structure_has_field(s, "stream-format"))
          gst_structure_set(s, "stream-format", G_TYPE_STRING, "asf", NULL);
        if (!gst_structure_has_field(s, "header-format"))
          gst_structure_set(s, "header-format", G_TYPE_STRING,
                            codec.vc1_bdu ? "none" : "asf", NULL);
        check = gst_query_new_accept_caps(normalized);
        result = parent->sink_query(decoder, check);
        if (result)
          gst_query_parse_accept_caps_result(check, &accepted);
        gst_query_set_accept_caps_result(query, accepted);
        gst_query_unref(check);
        gst_caps_unref(normalized);
        return result;
      }
    }
  }
  return parent->sink_query(decoder, query);
}

static const gchar *
gst_crystalhd_status_hint(BC_STATUS status)
{
  switch (status) {
    case BC_STS_BUSY:
    case BC_STS_DEC_EXIST_OPEN:
      return "CrystalHD supports one playback session; close other hardware players";
    case BC_STS_NO_ACCESS:
      return "check /dev/crystalhd permissions and video-group or desktop udev access";
    case BC_STS_FWHEX_NOT_FOUND:
      return "install the matching bcm70012fw.bin or bcm70015fw.bin in /lib/firmware";
    case BC_STS_FW_AUTH_FAILED:
    case BC_STS_BOOTLOADER_FAILED:
    case BC_STS_CERT_VERIFY_ERROR:
    case BC_STS_FW_CMD_ERR:
      return "firmware startup or command failed; check the firmware and kernel log";
    case BC_STS_INV_ARG:
    case BC_STS_NOT_IMPL:
      return "check the input codec/framing and supported output format (BCM70015 uses YUY2)";
    case BC_STS_TIMEOUT:
      return "decoder timed out; check Annex-B input, YUY2 output selection and the kernel log";
    default:
      return "check module binding, /dev/crystalhd access, firmware and other playback clients";
  }
}

static void
gst_crystalhd_clear_timestamps(GstCrystalHdDec *self)
{
  g_queue_clear_full(&self->timestamps, g_free);
  self->have_simple_picture_number = FALSE;
}

static BC_STATUS
gst_crystalhd_close_device(GstCrystalHdDec *self)
{
  BC_STATUS result = BC_STS_SUCCESS;
  BC_STATUS status;

  if (self->decoder_started) {
    status = DtsStopDecoder(self->device);
    if (result == BC_STS_SUCCESS)
      result = status;
    self->decoder_started = FALSE;
  }

  if (self->decoder_open) {
    status = DtsCloseDecoder(self->device);
    if (result == BC_STS_SUCCESS)
      result = status;
    self->decoder_open = FALSE;
  }

  if (self->device != NULL) {
    status = DtsDeviceClose(self->device);
    if (result == BC_STS_SUCCESS)
      result = status;
    self->device = NULL;
  }

  self->output_configured = FALSE;
  self->progressive_field_sequence = FALSE;
  self->input_flushed = FALSE;
  self->input_metadata_size = 0;
  self->need_second_field = FALSE;
  gst_crystalhd_clear_timestamps(self);
  return result;
}

static CrystalHdTimestamp *
gst_crystalhd_find_timestamp(GstCrystalHdDec *self, guint64 timestamp,
                             GList **link_out)
{
  GList *link;

  for (link = self->timestamps.head; link != NULL; link = link->next) {
    CrystalHdTimestamp *entry = link->data;
    if (entry->hardware_timestamp == timestamp) {
      if (link_out != NULL)
        *link_out = link;
      return entry;
    }
  }

  if (link_out != NULL)
    *link_out = NULL;
  return NULL;
}

static gboolean
gst_crystalhd_progressive_field_flags(guint32 pulldown, gboolean *top_first,
                                      gboolean *repeat_first)
{
  switch (pulldown) {
    case vdecTopBottom:
      *top_first = TRUE;
      *repeat_first = FALSE;
      return TRUE;
    case vdecBottomTop:
      *top_first = FALSE;
      *repeat_first = FALSE;
      return TRUE;
    case vdecTopBottomTop:
      *top_first = TRUE;
      *repeat_first = TRUE;
      return TRUE;
    case vdecBottomTopBottom:
      *top_first = FALSE;
      *repeat_first = TRUE;
      return TRUE;
    default:
      /* Single-field and whole-frame repeat values have not been measured
       * against GstVideo buffer semantics. Do not infer flags for them. */
      return FALSE;
  }
}

static gboolean
gst_crystalhd_configure_output(GstCrystalHdDec *self, guint width, guint height,
                               gboolean interlaced)
{
  GstVideoDecoder *decoder = GST_VIDEO_DECODER(self);
  GstVideoCodecState *state;
  GstVideoInterlaceMode mode =
      interlaced || self->progressive_field_sequence
          ? GST_VIDEO_INTERLACE_MODE_MIXED
          : GST_VIDEO_INTERLACE_MODE_PROGRESSIVE;

  if (width == 0 || height == 0 || width > 1920 || height > 1088) {
    GST_ERROR_OBJECT(self, "invalid output dimensions %ux%u", width, height);
    return FALSE;
  }

  if (self->output_configured && self->width == width &&
      self->height == height &&
      GST_VIDEO_INFO_INTERLACE_MODE(&self->output_info) == mode)
    return TRUE;

  state = gst_video_decoder_set_output_state(
      decoder, GST_VIDEO_FORMAT_YUY2, width, height, self->input_state);
  if (state == NULL)
    return FALSE;

  state->info.interlace_mode = mode;
  self->output_info = state->info;
  gst_video_codec_state_unref(state);

  if (!gst_video_decoder_negotiate(decoder))
    return FALSE;

  self->width = width;
  self->height = height;
  self->output_configured = TRUE;
  GST_INFO_OBJECT(self, "negotiated YUY2 output %ux%u", width, height);
  return TRUE;
}

static GstFlowReturn
gst_crystalhd_copy_output(GstCrystalHdDec *self, BC_DTS_PROC_OUT *output,
                          GstVideoCodecFrame **completed)
{
  GstVideoDecoder *decoder = GST_VIDEO_DECODER(self);
  GstVideoCodecFrame *frame;
  GstVideoFrame video_frame;
  GstVideoInfo copy_info;
  CrystalHdTimestamp *entry;
  GList *timestamp_link = NULL;
  guint32 frame_number;
  guint width = output->PicInfo.width;
  guint height = output->PicInfo.height;
  guint source_stride;
  guint destination_stride;
  guint row;
  guint rows;
  guint destination_row = 0;
  gboolean interlaced;
  gboolean bottom_field;
  gboolean bottom_first;
  gboolean progressive_top_first = FALSE;
  gboolean progressive_repeat_first = FALSE;
  gboolean progressive_field_sequence;
  gboolean ordered_mpeg4_simple;

  *completed = NULL;

  interlaced = (output->PicInfo.flags & VDEC_FLAG_INTERLACED_SRC) != 0;
  /* These are values in a two-bit picture-type field, not independent bits:
   * TOPFIELD=0x10 shares a bit with BOTTOMFIELD=0x18; FIELDPAIR is 0x08. */
  bottom_field = (output->PicInfo.flags & VDEC_FLAG_BOTTOMFIELD) ==
                 VDEC_FLAG_BOTTOMFIELD;
  bottom_first = (output->PicInfo.flags & VDEC_FLAG_BOTTOM_FIRST) != 0;
  progressive_field_sequence =
      !interlaced && gst_crystalhd_progressive_field_flags(
                         output->PicInfo.pulldown, &progressive_top_first,
                         &progressive_repeat_first);
  ordered_mpeg4_simple =
      self->codec.subtype == BC_MSUBTYPE_DIVX &&
      self->codec.mpeg4_object_type == CRYSTALHD_MPEG4_OBJECT_TYPE_SIMPLE &&
      !interlaced && !self->need_second_field;
  if (interlaced &&
      (output->PicInfo.flags & VDEC_FLAG_BOTTOMFIELD) != VDEC_FLAG_TOPFIELD &&
      !bottom_field) {
    GST_ELEMENT_ERROR(self, STREAM, DECODE,
                      ("Unsupported CrystalHD interlaced output picture type"),
                      ("Expected separate TOPFIELD/BOTTOMFIELD buffers, flags=0x%08x",
                       output->PicInfo.flags));
    return GST_FLOW_ERROR;
  }

  entry = gst_crystalhd_find_timestamp(self, output->PicInfo.timeStamp,
                                       &timestamp_link);
  /* MPEG-4 Simple Profile has no B-VOP reordering. BCM70015 can return a
   * valid picture with its timestamp metadata cleared; picture zero is not
   * recoverable because the kernel uses it for invalid/repeated output. */
  if (entry == NULL && ordered_mpeg4_simple &&
      output->PicInfo.timeStamp == 0 &&
      output->PicInfo.picture_number != 0 &&
      self->have_simple_picture_number &&
      output->PicInfo.picture_number == self->simple_picture_number + 1 &&
      self->timestamps.head != NULL) {
    timestamp_link = self->timestamps.head;
    entry = timestamp_link->data;
    GST_WARNING_OBJECT(self,
        "recovering MPEG-4 Simple picture %u with missing hardware "
        "timestamp as token=%" G_GUINT64_FORMAT " frame=%u",
        output->PicInfo.picture_number, entry->hardware_timestamp,
        entry->frame_number);
  }
  if (entry == NULL) {
    /* Flush/retry can expose a late or duplicate hardware picture. Never
     * attach its pixels to an unrelated (possibly not yet submitted) frame.
     * Field pairing without a matching token is not a supported contract.
     */
    if (interlaced || self->need_second_field) {
      GST_ELEMENT_ERROR(self, STREAM, DECODE,
                        ("CrystalHD field has no matching input timestamp"),
                        ("Hardware timestamp: %" G_GUINT64_FORMAT,
                         (guint64)output->PicInfo.timeStamp));
      return GST_FLOW_ERROR;
    }
    GST_DEBUG_OBJECT(self, "ignoring unmatched hardware timestamp %"
                     G_GUINT64_FORMAT, (guint64)output->PicInfo.timeStamp);
    return GST_FLOW_OK;
  }
  if (ordered_mpeg4_simple && timestamp_link != self->timestamps.head) {
    CrystalHdTimestamp *head = self->timestamps.head->data;
    GST_ELEMENT_ERROR(self, STREAM, DECODE,
                      ("CrystalHD MPEG-4 Simple output skipped a pending picture"),
                      ("Hardware timestamp: %" G_GUINT64_FORMAT "; "
                       "oldest pending timestamp: %" G_GUINT64_FORMAT "; "
                       "picture: %u",
                       (guint64)output->PicInfo.timeStamp,
                       head->hardware_timestamp, output->PicInfo.picture_number));
    return GST_FLOW_ERROR;
  }
  frame_number = entry->frame_number;
  if (self->need_second_field &&
      (!interlaced || frame_number != self->field_frame_number)) {
    GST_ELEMENT_ERROR(self, STREAM, DECODE,
                      ("CrystalHD fields belong to different input frames"),
                      (NULL));
    return GST_FLOW_ERROR;
  }
  if (self->need_second_field &&
      (bottom_field == self->field_bottom || width != self->field_width ||
       height != self->field_height)) {
    /* Both fields share one output allocation and must fill complementary
     * rows using exactly the same layout. A repeated parity or changed
     * geometry would otherwise finish partially unwritten/corrupt pixels. */
    GST_ELEMENT_ERROR(self, STREAM, DECODE,
                      ("CrystalHD fields have repeated parity or different geometry"),
                      ("First field: bottom=%d %ux%u; next: bottom=%d %ux%u "
                       "flags=0x%08x token=%" G_GUINT64_FORMAT " picture=%u",
                       self->field_bottom, self->field_width, self->field_height,
                       bottom_field, width, height, output->PicInfo.flags,
                       (guint64)output->PicInfo.timeStamp,
                       output->PicInfo.picture_number));
    return GST_FLOW_ERROR;
  }
  if (self->need_second_field && bottom_first != self->field_bottom_first) {
    GST_ELEMENT_ERROR(self, STREAM, DECODE,
                      ("CrystalHD fields have different presentation order"),
                      ("First bottom-first=%d; next bottom-first=%d "
                       "flags=0x%08x token=%" G_GUINT64_FORMAT " picture=%u",
                       self->field_bottom_first, bottom_first, output->PicInfo.flags,
                       (guint64)output->PicInfo.timeStamp,
                       output->PicInfo.picture_number));
    return GST_FLOW_ERROR;
  }

  if (width == 0 || height == 0 || width > 1920 || height > 1088 ||
      (interlaced && (height & 1)) ||
      !gst_video_info_set_format(&copy_info, GST_VIDEO_FORMAT_YUY2,
                                  width, height))
    return GST_FLOW_ERROR;

  frame = gst_video_decoder_get_frame(decoder, frame_number);
  if (frame == NULL) {
    GST_WARNING_OBJECT(self, "input frame %u is no longer queued", frame_number);
    return GST_FLOW_OK;
  }

  if (frame->output_buffer == NULL) {
    /* Plain video/x-raw caps permit owned system memory. Do not wait for a
     * downstream pool while holding a hardware RX lease or the stream lock:
     * that can prevent the demuxer feeding the audio needed for preroll. */
    frame->output_buffer = gst_buffer_new_allocate(NULL, copy_info.size, NULL);
    if (frame->output_buffer == NULL) {
      gst_video_codec_frame_unref(frame);
      return GST_FLOW_ERROR;
    }
  }

  if (!gst_video_frame_map(&video_frame, &copy_info,
                           frame->output_buffer, GST_MAP_WRITE)) {
    gst_video_codec_frame_unref(frame);
    return GST_FLOW_ERROR;
  }

  source_stride = width * 2;
  if (self->is_70012) {
    guint padded_width = width <= 720 ? 720 : (width <= 1280 ? 1280 : 1920);
    source_stride = padded_width * 2;
  }
  destination_stride = GST_VIDEO_FRAME_PLANE_STRIDE(&video_frame, 0);
  rows = interlaced ? height / 2 : height;
  destination_row = interlaced && bottom_field ? 1 : 0;

  if (output->Ybuff == NULL ||
      (guint64)output->YBuffDoneSz * 4 < (guint64)source_stride * rows) {
    GST_ERROR_OBJECT(self, "short CrystalHD output buffer (%u dwords)",
                     output->YBuffDoneSz);
    gst_video_frame_unmap(&video_frame);
    gst_video_codec_frame_unref(frame);
    return GST_FLOW_ERROR;
  }

  for (row = 0; row < rows; row++) {
    guint8 *destination =
        GST_VIDEO_FRAME_PLANE_DATA(&video_frame, 0) +
        destination_row * destination_stride;
    memcpy(destination, output->Ybuff + row * source_stride, width * 2);
    destination_row += interlaced ? 2 : 1;
  }

  gst_video_frame_unmap(&video_frame);

  if (interlaced && !self->need_second_field) {
    self->need_second_field = TRUE;
    self->field_frame_number = frame_number;
    self->field_bottom = bottom_field;
    self->field_bottom_first = bottom_first;
    self->field_width = width;
    self->field_height = height;
    gst_video_codec_frame_unref(frame);
    return GST_FLOW_OK;
  }

  /* Mixed caps alone do not mark an individual buffer as interlaced. The
   * complete weave's presentation order is explicit BOTTOM_FIRST metadata:
   * capture can still return the top field first during a BFF transition.
   * Clear stale flags even when an existing output allocation is reused. */
  GST_BUFFER_FLAG_UNSET(frame->output_buffer, GST_VIDEO_BUFFER_FLAG_INTERLACED);
  GST_BUFFER_FLAG_UNSET(frame->output_buffer, GST_VIDEO_BUFFER_FLAG_TFF);
  GST_BUFFER_FLAG_UNSET(frame->output_buffer, GST_VIDEO_BUFFER_FLAG_ONEFIELD);
  GST_BUFFER_FLAG_UNSET(frame->output_buffer, GST_VIDEO_BUFFER_FLAG_RFF);
  if (interlaced) {
    GST_BUFFER_FLAG_SET(frame->output_buffer, GST_VIDEO_BUFFER_FLAG_INTERLACED);
    if (!self->field_bottom_first)
      GST_BUFFER_FLAG_SET(frame->output_buffer, GST_VIDEO_BUFFER_FLAG_TFF);
  } else if (progressive_field_sequence) {
    /* Progressive soft telecine remains a progressive buffer. MIXED caps
     * allow its per-picture field order/repeat flags, and stay negotiated for
     * the rest of this session so no-info pictures cannot churn caps. */
    self->progressive_field_sequence = TRUE;
    if (progressive_top_first)
      GST_BUFFER_FLAG_SET(frame->output_buffer, GST_VIDEO_BUFFER_FLAG_TFF);
    if (progressive_repeat_first)
      GST_BUFFER_FLAG_SET(frame->output_buffer, GST_VIDEO_BUFFER_FLAG_RFF);
  }
  self->need_second_field = FALSE;
  if (timestamp_link != NULL) {
    g_free(timestamp_link->data);
    g_queue_delete_link(&self->timestamps, timestamp_link);
  }
  if (ordered_mpeg4_simple) {
    if (output->PicInfo.picture_number != 0 &&
        output->PicInfo.picture_number != G_MAXUINT32) {
      self->simple_picture_number = output->PicInfo.picture_number;
      self->have_simple_picture_number = TRUE;
    } else {
      self->have_simple_picture_number = FALSE;
    }
  }

  GST_LOG_OBJECT(self, "decoded frame %u (%ux%u), picture %u", frame_number,
                 width, height, output->PicInfo.picture_number);
  *completed = frame;
  return GST_FLOW_OK;
}

static GstFlowReturn
gst_crystalhd_receive_one(GstCrystalHdDec *self, guint timeout_ms,
                          gboolean *activity, GstVideoCodecFrame **completed)
{
  BC_DTS_PROC_OUT output;
  BC_STATUS status;
  GstFlowReturn flow = GST_FLOW_OK;

  memset(&output, 0, sizeof(output));
  output.PicInfo.width = self->width;
  output.PicInfo.height = self->height;
  *activity = FALSE;
  *completed = NULL;

  status = DtsProcOutputNoCopy(self->device, timeout_ms, &output);
  if (status == BC_STS_SUCCESS || status == BC_STS_FMT_CHANGE)
    GST_LOG_OBJECT(self, "received PIB: status=%d flags=0x%08x output-flags=0x%08x "
        "token=%" G_GUINT64_FORMAT " picture=%u %ux%u Y-dwords=%u",
        status, output.PicInfo.flags, output.PoutFlags,
        (guint64)output.PicInfo.timeStamp, output.PicInfo.picture_number,
        output.PicInfo.width, output.PicInfo.height, output.YBuffDoneSz);
  if (status == BC_STS_FMT_CHANGE) {
    *activity = TRUE;
    if (!gst_crystalhd_configure_output(
            self, output.PicInfo.width, output.PicInfo.height,
            (output.PicInfo.flags & VDEC_FLAG_INTERLACED_SRC) != 0))
      return GST_FLOW_NOT_NEGOTIATED;
    return GST_FLOW_OK;
  }

  if (status == BC_STS_SUCCESS) {
    *activity = TRUE;
    if ((output.PoutFlags & BC_POUT_FLAGS_PIB_VALID) != 0)
      flow = gst_crystalhd_copy_output(self, &output, completed);
    else
      GST_WARNING_OBJECT(self, "decoder returned a picture without valid PIB");

    status = DtsReleaseOutputBuffs(self->device, NULL, FALSE);
    if (status != BC_STS_SUCCESS && flow == GST_FLOW_OK) {
      GST_ERROR_OBJECT(self, "failed to release output buffers: %d", status);
      flow = GST_FLOW_ERROR;
    }
    /* No hardware pointer or lease survives negotiation/downstream calls. */
    if (flow == GST_FLOW_OK && *completed != NULL &&
        !gst_crystalhd_configure_output(
            self, output.PicInfo.width, output.PicInfo.height,
            (output.PicInfo.flags & VDEC_FLAG_INTERLACED_SRC) != 0))
      flow = GST_FLOW_NOT_NEGOTIATED;
    if (flow != GST_FLOW_OK && *completed != NULL) {
      gst_video_codec_frame_unref(*completed);
      *completed = NULL;
    }
    return flow;
  }

  if (status == BC_STS_NO_DATA || status == BC_STS_BUSY ||
      status == BC_STS_TIMEOUT)
    return GST_FLOW_OK;

  GST_ERROR_OBJECT(self, "DtsProcOutputNoCopy failed: %d", status);
  return GST_FLOW_ERROR;
}

static gboolean
gst_crystalhd_is_flushing(GstCrystalHdDec *self)
{
  GstVideoDecoder *decoder = GST_VIDEO_DECODER(self);
  return GST_PAD_IS_FLUSHING(GST_VIDEO_DECODER_SINK_PAD(decoder)) ||
         GST_PAD_IS_FLUSHING(GST_VIDEO_DECODER_SRC_PAD(decoder));
}

static void
gst_crystalhd_wake_output(GstCrystalHdDec *self)
{
  g_mutex_lock(&self->output_lock);
  g_cond_signal(&self->output_cond);
  g_mutex_unlock(&self->output_lock);
}

/* One sole-consumer iteration; callers must NOT own the decoder stream lock.
 * output_active covers the gap before finish_frame obtains its own lock as
 * well as its downstream push. A flushing generation cannot retire until
 * that delivery has actually returned. */
static gboolean
gst_crystalhd_output_iteration(GstCrystalHdDec *self)
{
  GstVideoDecoder *decoder = GST_VIDEO_DECODER(self);
  GstVideoCodecFrame *completed = NULL;
  BC_DTS_STATUS decoder_status;
  BC_STATUS status;
  GstFlowReturn flow = GST_FLOW_OK;
  gboolean activity = FALSE;

  GST_VIDEO_DECODER_STREAM_LOCK(decoder);
  g_mutex_lock(&self->output_lock);
  if (self->output_stop || self->output_paused || self->output_active ||
      !self->decoder_started || self->output_flow != GST_FLOW_OK ||
      gst_crystalhd_is_flushing(self) ||
      (g_queue_is_empty(&self->timestamps) && !self->draining)) {
    g_mutex_unlock(&self->output_lock);
    GST_VIDEO_DECODER_STREAM_UNLOCK(decoder);
    return FALSE;
  }
  self->output_active = TRUE;
  g_mutex_unlock(&self->output_lock);

  memset(&decoder_status, 0, sizeof(decoder_status));
  status = DtsGetDriverStatus(self->device, &decoder_status);
  if (status != BC_STS_SUCCESS) {
    GST_ERROR_OBJECT(self, "DtsGetDriverStatus failed: %d", status);
    flow = GST_FLOW_ERROR;
  } else if (decoder_status.ReadyListCount != 0) {
    self->drain_idle = FALSE;
    flow = gst_crystalhd_receive_one(self, 0, &activity, &completed);
  } else if (self->draining) {
    guint8 eos = FALSE;
    self->drain_idle = TRUE;
    status = DtsIsEndOfStream(self->device, &eos);
    if (status != BC_STS_SUCCESS)
      flow = GST_FLOW_ERROR;
    else
      self->output_eos = eos != 0;
  }
  GST_VIDEO_DECODER_STREAM_UNLOCK(decoder);

  /* finish_frame drops only its own recursive stream-lock level around
   * downstream delivery. Entering with an outer level would block input,
   * including the demuxed audio required to complete video preroll. */
  if (completed != NULL)
    flow = gst_video_decoder_finish_frame(decoder, completed);

  GST_VIDEO_DECODER_STREAM_LOCK(decoder);
  if (completed != NULL && flow == GST_FLOW_OK)
    self->last_delivery_us = g_get_monotonic_time();
  if (flow != GST_FLOW_OK) {
    self->output_flow = flow;
    if (flow != GST_FLOW_FLUSHING && flow != GST_FLOW_EOS)
      GST_ELEMENT_ERROR(self, STREAM, DECODE,
                        ("CrystalHD output delivery failed"),
                        ("Flow: %s", gst_flow_get_name(flow)));
  }
  g_mutex_lock(&self->output_lock);
  self->output_active = FALSE;
  g_cond_broadcast(&self->output_cond);
  g_mutex_unlock(&self->output_lock);
  GST_VIDEO_DECODER_STREAM_UNLOCK(decoder);
  return activity;
}

static gpointer
gst_crystalhd_output_thread(gpointer data)
{
  GstCrystalHdDec *self = data;

  for (;;) {
    g_mutex_lock(&self->output_lock);
    if (!self->output_stop) {
      if (self->output_paused)
        g_cond_wait(&self->output_cond, &self->output_lock);
      else
        g_cond_wait_until(&self->output_cond, &self->output_lock,
                          g_get_monotonic_time() + 5000);
    }
    if (self->output_stop) {
      g_mutex_unlock(&self->output_lock);
      break;
    }
    g_mutex_unlock(&self->output_lock);
    /* Consume ready output without a sleep between pictures, but release
     * the stream lock between iterations so input and flush can progress. */
    while (gst_crystalhd_output_iteration(self))
      ;
  }
  return NULL;
}

/* Called with exactly the vfunc's stream-lock level. In particular, neither
 * stop nor the nonserialized FLUSH_START event may call this without first
 * taking that lock. Flush cannot return before quiescence: the base class
 * ignores its boolean result and resets queued frames unconditionally. */
static gboolean
gst_crystalhd_quiesce_output(GstCrystalHdDec *self, gboolean bounded)
{
  GstVideoDecoder *decoder = GST_VIDEO_DECODER(self);
  gint64 deadline = g_get_monotonic_time() + GST_CRYSTALHD_DRAIN_TIMEOUT_US;
  gboolean active;

  g_mutex_lock(&self->output_lock);
  self->output_paused = TRUE;
  g_mutex_unlock(&self->output_lock);
  GST_VIDEO_DECODER_STREAM_UNLOCK(decoder);
  do {
    g_mutex_lock(&self->output_lock);
    active = self->output_active;
    g_mutex_unlock(&self->output_lock);
    if (!active || (bounded && g_get_monotonic_time() >= deadline))
      break;
    g_usleep(1000);
  } while (TRUE);
  GST_VIDEO_DECODER_STREAM_LOCK(decoder);
  if (active)
    GST_ELEMENT_ERROR(self, STREAM, DECODE,
                      ("Output delivery did not stop before a format change"),
                      ("A flushing transition is required to cancel downstream playback"));
  return !active;
}

static void
gst_crystalhd_resume_output(GstCrystalHdDec *self)
{
  g_mutex_lock(&self->output_lock);
  self->output_paused = FALSE;
  g_cond_signal(&self->output_cond);
  g_mutex_unlock(&self->output_lock);
}

/* Progress waits never consume RX themselves. Revalidate the generation
 * after dropping the stream lock: a flushing seek may have reopened the
 * device and invalidated the old compressed-input/drain request. */
static GstFlowReturn
gst_crystalhd_wait_output(GstCrystalHdDec *self, guint64 generation,
                          gint64 delay_us, guint lock_levels)
{
  GstVideoDecoder *decoder = GST_VIDEO_DECODER(self);
  guint level;
  gst_crystalhd_wake_output(self);
  for (level = 0; level < lock_levels; level++)
    GST_VIDEO_DECODER_STREAM_UNLOCK(decoder);
  g_usleep((gulong)MAX((gint64)1, delay_us));
  for (level = 0; level < lock_levels; level++)
    GST_VIDEO_DECODER_STREAM_LOCK(decoder);
  if (generation != self->generation || gst_crystalhd_is_flushing(self))
    return GST_FLOW_FLUSHING;
  return self->output_flow;
}

/* A human pause is not stalled decoding. Inspect the top-level state so a
 * child already PAUSED while its pipeline still awaits PLAYING/preroll is
 * not mistaken for a deliberate pause. No state change is performed here. */
static gboolean
gst_crystalhd_playback_paused(GstCrystalHdDec *self)
{
  GstObject *object = gst_object_ref(self);
  GstObject *parent;
  GstState state = GST_STATE_NULL;
  GstState pending = GST_STATE_VOID_PENDING;

  while ((parent = gst_object_get_parent(object)) != NULL) {
    gst_object_unref(object);
    object = parent;
  }
  if (GST_IS_ELEMENT(object)) {
    /* State fields are protected by GstObject's lock. Do not take the
     * element state-change lock from inside the decoder stream lock. */
    GST_OBJECT_LOCK(object);
    state = GST_STATE(object);
    pending = GST_STATE_PENDING(object);
    GST_OBJECT_UNLOCK(object);
  }
  gst_object_unref(object);
  return state == GST_STATE_PAUSED && pending == GST_STATE_VOID_PENDING;
}

static GstFlowReturn
gst_crystalhd_wait_input_space(GstCrystalHdDec *self, gsize reservation,
                               gint64 deadline, guint lock_levels)
{
  guint64 generation = self->generation;
  for (;;) {
    gint64 remaining;
    GstFlowReturn flow;

    if (gst_crystalhd_is_flushing(self))
      return GST_FLOW_FLUSHING;
    if (self->output_flow != GST_FLOW_OK)
      return self->output_flow;
    deadline = MAX(deadline,
                    self->last_delivery_us + GST_CRYSTALHD_INPUT_TIMEOUT_US);
    if (gst_crystalhd_playback_paused(self))
      deadline = g_get_monotonic_time() + GST_CRYSTALHD_INPUT_TIMEOUT_US;
    remaining = gst_crystalhd_drain_remaining_us(deadline, g_get_monotonic_time());
    if (remaining == 0) {
      GST_ELEMENT_ERROR(self, STREAM, DECODE,
                        ("Timed out waiting for CrystalHD input capacity"),
                        ("Whole-call reservation: %" G_GSIZE_FORMAT " bytes",
                         reservation));
      return GST_FLOW_ERROR;
    }
    if (DtsTxFreeSize(self->device) >= reservation)
      return GST_FLOW_OK;

    /* The RX worker runs even when demuxed input is waiting for audio
     * preroll. Let it retire hardware output before retrying admission. */
    flow = gst_crystalhd_wait_output(self, generation,
                                    MIN(remaining, (gint64)1000), lock_levels);
    if (flow != GST_FLOW_OK)
      return flow;
  }
}

static gboolean
gst_crystalhd_start(GstVideoDecoder *decoder)
{
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(decoder);

  g_return_val_if_fail(self->output_thread == NULL, FALSE);
  gst_crystalhd_close_device(self);
  self->next_hardware_timestamp = CRYSTALHD_TIMESTAMP_STEP;
  self->output_flow = GST_FLOW_OK;
  self->output_stop = FALSE;
  self->output_paused = TRUE;
  self->draining = FALSE;
  self->output_eos = FALSE;
  self->last_delivery_us = 0;
  self->generation++;
  self->output_thread = g_thread_new("crystalhd-output",
                                     gst_crystalhd_output_thread, self);
  gst_video_decoder_set_packetized(decoder, TRUE);
  return TRUE;
}

static gboolean
gst_crystalhd_stop(GstVideoDecoder *decoder)
{
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(decoder);
  BC_STATUS status;

  /* start/stop are outside the base stream lock. Pad deactivation/flush
   * cancels a downstream push before this join; never close a live lease. */
  g_mutex_lock(&self->output_lock);
  self->output_stop = TRUE;
  self->output_paused = TRUE;
  g_cond_broadcast(&self->output_cond);
  g_mutex_unlock(&self->output_lock);
  if (self->output_thread != NULL) {
    g_thread_join(self->output_thread);
    self->output_thread = NULL;
  }
  GST_VIDEO_DECODER_STREAM_LOCK(decoder);
  self->generation++;
  status = gst_crystalhd_close_device(self);
  g_clear_pointer(&self->input_state, gst_video_codec_state_unref);
  g_clear_object(&self->parse_adapter);
  self->parse_pending = FALSE;
  GST_VIDEO_DECODER_STREAM_UNLOCK(decoder);
  if (status != BC_STS_SUCCESS)
    GST_ELEMENT_ERROR(self, LIBRARY, SHUTDOWN,
                      ("Could not close CrystalHD playback session"),
                      ("Device teardown returned %d", status));
  return status == BC_STS_SUCCESS;
}

static gboolean
gst_crystalhd_reopen_format(GstVideoDecoder *decoder, GstVideoCodecState *state,
                            gboolean discard)
{
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(decoder);
  GstStructure *structure = gst_caps_get_structure(state->caps, 0);
  BC_INPUT_FORMAT input_format;
  BC_INFO_CRYSTAL version;
  BC_MEDIA_SUBTYPE subtype;
  BC_STATUS status;
  guint32 mode;
  const GValue *codec_data_value;
  GstMapInfo codec_data_map;
  gboolean codec_data_mapped = FALSE;
  const gchar *operation = "DtsDeviceOpen";
  GstVideoCodecState *retained_state = gst_video_codec_state_ref(state);
  guint64 generation = self->generation;

  if (!gst_crystalhd_quiesce_output(self, TRUE) ||
      generation != self->generation ||
      (!discard && gst_crystalhd_is_flushing(self))) {
    gst_video_codec_state_unref(retained_state);
    return FALSE;
  }
  self->generation++;
  self->output_flow = GST_FLOW_OK;
  self->draining = FALSE;
  self->output_eos = FALSE;
  self->last_delivery_us = 0;
  g_clear_object(&self->parse_adapter);
  self->parse_pending = FALSE;
  status = gst_crystalhd_close_device(self);
  if (status != BC_STS_SUCCESS) {
    gst_video_codec_state_unref(retained_state);
    GST_ELEMENT_ERROR(self, LIBRARY, SHUTDOWN,
                      ("Could not close previous CrystalHD session"),
                      ("Device teardown returned %d", status));
    return FALSE;
  }
  g_clear_pointer(&self->input_state, gst_video_codec_state_unref);
  self->input_state = retained_state;

  if (!gst_crystalhd_codec_from_caps(state->caps, &self->codec)) {
    GST_ELEMENT_ERROR(self, STREAM, FORMAT,
                      ("Unsupported CrystalHD input codec"),
                      ("Caps: %" GST_PTR_FORMAT, state->caps));
    return FALSE;
  }
  subtype = self->codec.subtype;
  gst_video_decoder_set_packetized(decoder, !self->codec.vc1_bdu);

  memset(&input_format, 0, sizeof(input_format));
  input_format.FGTEnable = FALSE;
  input_format.Progressive = TRUE;
  input_format.OptFlags = 0x80000000U | vdecFrameRate59_94 | 0x40U;
  input_format.mSubtype = subtype;
  input_format.width = GST_VIDEO_INFO_WIDTH(&state->info);
  input_format.height = GST_VIDEO_INFO_HEIGHT(&state->info);
  if (subtype == BC_MSUBTYPE_DIVX) {
    if (input_format.width == 0 || input_format.width > 1920 ||
        input_format.height == 0 || input_format.height > 1088 ||
        (self->codec.mpeg4_width != 0 &&
         self->codec.mpeg4_width != input_format.width) ||
        (self->codec.mpeg4_height != 0 &&
         self->codec.mpeg4_height != input_format.height)) {
      GST_ELEMENT_ERROR(self, STREAM, FORMAT,
                        ("Invalid MPEG-4 coded dimensions"),
                        ("Caps/state dimensions are outside the supported "
                         "range or disagree"));
      goto fail;
    }
    self->codec.mpeg4_width = input_format.width;
    self->codec.mpeg4_height = input_format.height;
  }
  if (subtype == BC_MSUBTYPE_H264 || subtype == BC_MSUBTYPE_DIVX)
    input_format.startCodeSz = 4;

  codec_data_value = gst_structure_get_value(structure, "codec_data");
  if (codec_data_value != NULL && GST_VALUE_HOLDS_BUFFER(codec_data_value)) {
    GstBuffer *codec_data = gst_value_get_buffer(codec_data_value);
    if (codec_data != NULL &&
        gst_buffer_map(codec_data, &codec_data_map, GST_MAP_READ)) {
      codec_data_mapped = TRUE;
      if (codec_data_map.size > G_MAXUINT32) {
        GST_ELEMENT_ERROR(self, STREAM, FORMAT,
                          ("Codec data exceeds the input limit"), (NULL));
        goto fail;
      }
      input_format.pMetaData = codec_data_map.data;
      input_format.metaDataSz = codec_data_map.size;
    }
  }

  {
    const guint8 *metadata = input_format.pMetaData;
    gsize metadata_size = input_format.metaDataSz;
    gsize minimum_reservation;
    if (!gst_crystalhd_codec_metadata(&self->codec, &metadata, &metadata_size) ||
        (subtype == BC_MSUBTYPE_WMV3 &&
         (!input_format.width || !input_format.height))) {
      GST_ELEMENT_ERROR(self, STREAM, FORMAT,
                        ("Missing or invalid codec data or dimensions"),
                        ("Use a parser with a supported stream/header format"));
      goto fail;
    }
    if (!gst_crystalhd_input_reservation(subtype, 1, metadata_size,
                                         &minimum_reservation)) {
      GST_ELEMENT_ERROR(self, STREAM, FORMAT,
                        ("Codec metadata exceeds CrystalHD input ring capacity"),
                        (NULL));
      goto fail;
    }
    self->input_metadata_size = metadata_size;
    input_format.pMetaData = (guint8 *)metadata;
    input_format.metaDataSz = metadata_size;
  }

  mode = DTS_PLAYBACK_MODE | DTS_LOAD_FILE_PLAY_FW | DTS_SKIP_TX_CHK_CPB |
         DTS_PLAYBACK_DROP_RPT_MODE | DTS_SINGLE_THREADED_MODE |
         DTS_DFLT_RESOLUTION(vdecRESOLUTION_1080p23_976);
  status = DtsDeviceOpen(&self->device, mode);
  if (status != BC_STS_SUCCESS) {
    GST_ELEMENT_ERROR(self, RESOURCE, OPEN_READ,
                      ("Could not open /dev/crystalhd: %s",
                       gst_crystalhd_status_hint(status)),
                      ("DtsDeviceOpen returned %d", status));
    goto fail;
  }

  memset(&version, 0, sizeof(version));
  operation = "DtsCrystalHDVersion";
  status = DtsCrystalHDVersion(self->device, &version);
  if (status != BC_STS_SUCCESS)
    goto fail_status;
  self->is_70012 = version.device == 0;
  if (subtype == BC_MSUBTYPE_DIVX && self->is_70012) {
    GST_ELEMENT_ERROR(self, STREAM, FORMAT,
                      ("MPEG-4 Part 2 requires BCM70015"),
                      ("BCM70012 does not expose the validated DIVX decoder"));
    goto fail;
  }

  operation = "DtsSetInputFormat";
  status = DtsSetInputFormat(self->device, &input_format);
  if (status != BC_STS_SUCCESS)
    goto fail_status;

  operation = "DtsOpenDecoder";
  status = DtsOpenDecoder(self->device, BC_STREAM_TYPE_ES);
  if (status != BC_STS_SUCCESS)
    goto fail_status;
  self->decoder_open = TRUE;

  operation = "DtsSetColorSpace(YUY2)";
  status = DtsSetColorSpace(self->device, OUTPUT_MODE422_YUY2);
  if (status != BC_STS_SUCCESS)
    goto fail_status;

  operation = "DtsStartDecoder";
  status = DtsStartDecoder(self->device);
  if (status != BC_STS_SUCCESS)
    goto fail_status;
  self->decoder_started = TRUE;

  operation = "DtsStartCapture";
  status = DtsStartCapture(self->device);
  if (status != BC_STS_SUCCESS)
    goto fail_status;

  if (codec_data_mapped)
    gst_buffer_unmap(gst_value_get_buffer(codec_data_value), &codec_data_map);

  GST_INFO_OBJECT(self, "opened BCM7001%u for caps %" GST_PTR_FORMAT,
                  self->is_70012 ? 2 : 5, state->caps);
  gst_crystalhd_resume_output(self);
  return TRUE;

fail_status:
  GST_ELEMENT_ERROR(self, LIBRARY, INIT,
                    ("Could not initialize CrystalHD decoder: %s",
                     gst_crystalhd_status_hint(status)),
                    ("%s returned %d", operation, status));
fail:
  if (codec_data_mapped)
    gst_buffer_unmap(gst_value_get_buffer(codec_data_value), &codec_data_map);
  gst_crystalhd_close_device(self);
  return FALSE;
}

static GstFlowReturn gst_crystalhd_drain(GstVideoDecoder *decoder);
static GstFlowReturn gst_crystalhd_parse(GstVideoDecoder *decoder,
    GstVideoCodecFrame *frame, GstAdapter *adapter, gboolean at_eos);

static gboolean
gst_crystalhd_set_format(GstVideoDecoder *decoder, GstVideoCodecState *state)
{
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(decoder);
  GstVideoCodecState *retained_state = gst_video_codec_state_ref(state);
  guint64 generation = self->generation;
  gboolean active, result = FALSE;

  /* GstVideoDecoder does not drain before set_format. Keep the old state
   * installed until every accepted old picture has reached downstream,
   * including a copied frame whose token retired before finish_frame returns.
   * Retain state first: the flush/reopen caller may alias input_state. */
  if (self->output_flow != GST_FLOW_OK || gst_crystalhd_is_flushing(self))
    goto done;
  if (self->codec.vc1_bdu && self->parse_adapter != NULL &&
      gst_adapter_available(self->parse_adapter) != 0) {
    GstFlowReturn flow;
    /* parse_available leaves a current frame allocated when our parser
     * requests more data. All complete boundaries were already consumed:
     * the unchanged adapter contains at most one undelimited picture (or
     * trailing headers). Finalize it through the same public parser API
     * used at EOS, while the OLD codec/state and timestamps still apply.
     * Never access the base class's private current_frame or adapters. */
    if (!self->parse_pending || decoder->input_segment.rate < 0.0 ||
        gst_video_decoder_get_pending_frame_size(decoder) != 0) {
      GST_ELEMENT_ERROR(self, STREAM, DECODE,
                        ("Cannot finalize pending VC-1 input at a format change"),
                        ("A successful forward parser boundary is required"));
      goto done;
    }
    flow = gst_crystalhd_parse(decoder, NULL, self->parse_adapter, TRUE);
    if ((flow != GST_FLOW_OK && flow != GST_VIDEO_DECODER_FLOW_NEED_DATA) ||
        generation != self->generation || gst_crystalhd_is_flushing(self))
      goto done;
    if (gst_adapter_available(self->parse_adapter) != 0)
      goto done;
  }
  g_mutex_lock(&self->output_lock);
  active = self->output_active;
  g_mutex_unlock(&self->output_lock);
  if (self->decoder_started &&
      (!g_queue_is_empty(&self->timestamps) || self->need_second_field || active)) {
    if (gst_crystalhd_drain(decoder) != GST_FLOW_OK)
      goto done;
  }
  if (generation != self->generation || gst_crystalhd_is_flushing(self) ||
      self->output_flow != GST_FLOW_OK)
    goto done;
  result = gst_crystalhd_reopen_format(decoder, retained_state, FALSE);
done:
  gst_video_codec_state_unref(retained_state);
  return result;
}

static GstFlowReturn
gst_crystalhd_parse(GstVideoDecoder *decoder, GstVideoCodecFrame *frame,
                    GstAdapter *adapter, gboolean at_eos)
{
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(decoder);
  gsize available = gst_adapter_available(adapter);
  const guint8 *data;
  gsize size;

  (void)frame;
  g_set_object(&self->parse_adapter, adapter);
  self->parse_pending = FALSE;
  if (available == 0)
    return GST_VIDEO_DECODER_FLOW_NEED_DATA;
  if (available > 16 * 1024 * 1024) {
    GST_ELEMENT_ERROR(decoder, STREAM, DECODE,
                      ("VC-1 picture exceeds the input limit"), (NULL));
    return GST_FLOW_ERROR;
  }
  data = gst_adapter_map(adapter, available);
  size = gst_crystalhd_vc1_frame_size(data, available, at_eos);
  gst_adapter_unmap(adapter);
  if (size != 0) {
    GstFlowReturn flow;
    gst_video_decoder_add_to_frame(decoder, size);
    /* have_frame adds one recursive lock around handle_frame. Preserve
     * that known extra level only for waits made by this callback. */
    self->parsing_frame = TRUE;
    flow = gst_video_decoder_have_frame(decoder);
    self->parsing_frame = FALSE;
    return flow;
  }
  if (at_eos)
    gst_adapter_flush(adapter, available); /* trailing headers, no picture */
  else
    self->parse_pending = TRUE;
  return GST_VIDEO_DECODER_FLOW_NEED_DATA;
}

static GstFlowReturn
gst_crystalhd_handle_frame(GstVideoDecoder *decoder,
                           GstVideoCodecFrame *frame)
{
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(decoder);
  CrystalHdTimestamp *entry;
  GstMapInfo map;
  BC_STATUS status = BC_STS_ERROR;
  GstFlowReturn flow;
  gint64 deadline;
  guint64 hardware_timestamp;
  const guint8 *data;
  gsize size;
  gsize reservation;
  guint8 *padded = NULL;
  guint64 generation = self->generation;
  guint lock_levels = self->parsing_frame ? 2 : 1;
  CrystalHdPayloadStatus payload;

  if (!self->decoder_started || frame->input_buffer == NULL)
    return gst_video_decoder_drop_frame(decoder, frame);

  if (!gst_buffer_map(frame->input_buffer, &map, GST_MAP_READ))
    return gst_video_decoder_drop_frame(decoder, frame);

  data = map.data;
  size = map.size;
  payload = gst_crystalhd_codec_payload(&self->codec, &data, &size);
  if (payload == CRYSTALHD_PAYLOAD_INVALID) {
    gst_buffer_unmap(frame->input_buffer, &map);
    GST_ELEMENT_ERROR(self, STREAM, DECODE,
                      ("Invalid compressed picture framing"), (NULL));
    gst_video_decoder_drop_frame(decoder, frame);
    return GST_FLOW_ERROR;
  }
  if (!gst_crystalhd_input_reservation(self->codec.subtype, size,
                                      self->input_metadata_size, &reservation)) {
    gst_buffer_unmap(frame->input_buffer, &map);
    GST_ELEMENT_ERROR(self, STREAM, DECODE,
                      ("Compressed picture exceeds CrystalHD input ring capacity"),
                      ("Picture: %" G_GSIZE_FORMAT " bytes; metadata: %"
                       G_GSIZE_FORMAT " bytes; ring: 1048576 bytes",
                       size, self->input_metadata_size));
    gst_video_decoder_drop_frame(decoder, frame);
    return GST_FLOW_ERROR;
  }
  if (self->codec.subtype == BC_MSUBTYPE_WMV3 ||
      self->codec.subtype == BC_MSUBTYPE_WVC1) {
    /* The library probes a four-byte startcode even for valid sub-word
     * ASF pictures. Supply readable padding without submitting extra data.
     */
    padded = g_malloc0(size + 4);
    memcpy(padded, data, size);
    data = padded;
  }

  hardware_timestamp = self->next_hardware_timestamp;
  self->next_hardware_timestamp += CRYSTALHD_TIMESTAMP_STEP;
  deadline = g_get_monotonic_time() + GST_CRYSTALHD_INPUT_TIMEOUT_US;

  for (;;) {
    flow = gst_crystalhd_wait_input_space(self, reservation, deadline,
                                         lock_levels);
    if (flow != GST_FLOW_OK)
      goto input_flow_error;
    /* Once submission starts, this session is no longer an empty flush. */
    self->input_flushed = FALSE;
    status = DtsProcInput(self->device, (guint8 *)data, size,
                          hardware_timestamp, 0);
    if (status != BC_STS_BUSY)
      break;

    flow = gst_crystalhd_wait_output(self, generation,
        MIN((gint64)1000,
            gst_crystalhd_drain_remaining_us(deadline, g_get_monotonic_time())),
        lock_levels);
    if (flow != GST_FLOW_OK)
      goto input_flow_error;
  }
  g_free(padded);
  gst_buffer_unmap(frame->input_buffer, &map);

  if (status != BC_STS_SUCCESS) {
    GST_ELEMENT_ERROR(self, STREAM, DECODE,
                      ("CrystalHD rejected compressed input: %s",
                       gst_crystalhd_status_hint(status)),
                      ("DtsProcInput returned %d", status));
    gst_video_decoder_drop_frame(decoder, frame);
    return GST_FLOW_ERROR;
  }

  if (payload == CRYSTALHD_PAYLOAD_NOT_CODED) {
    /* An uncoded VOP has no output picture, but its header still advances the
     * decoder's MPEG-4 timing state (including the state used by later B-VOPs).
     * Give it a unique transport token without creating an expected-output
     * mapping.  It also breaks the proof needed to infer a missing Simple
     * timestamp from consecutive output ordinals. */
    self->have_simple_picture_number = FALSE;
    gst_crystalhd_wake_output(self);
    return gst_video_decoder_drop_frame(decoder, frame);
  }

  /* Only accepted input owns a token. Output polling during BUSY must not
   * consume a provisional mapping or finish the current unaccepted frame.
   */
  entry = g_new0(CrystalHdTimestamp, 1);
  entry->hardware_timestamp = hardware_timestamp;
  entry->frame_number = frame->system_frame_number;
  g_queue_push_tail(&self->timestamps, entry);
  GST_LOG_OBJECT(self, "submitted token=%" G_GUINT64_FORMAT " pending=%u",
                 hardware_timestamp, self->timestamps.length);
  /* GstVideoDecoder keeps its own queued reference; get_frame() supplies the
   * reference consumed by finish_frame() when this picture is received.
   */
  gst_video_codec_frame_unref(frame);
  gst_crystalhd_wake_output(self);
  return self->output_flow;

input_flow_error:
  g_free(padded);
  gst_buffer_unmap(frame->input_buffer, &map);
  if (generation != self->generation)
    gst_video_codec_frame_unref(frame);
  else
    gst_video_decoder_drop_frame(decoder, frame);
  return flow;
}

static gboolean
gst_crystalhd_flush(GstVideoDecoder *decoder)
{
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(decoder);

  /* The base resets frames even when this vfunc returns FALSE. Therefore
   * cancellation must reach genuine quiescence, not a timed false return. */
  gst_crystalhd_quiesce_output(self, FALSE);
  self->generation++;
  self->output_flow = GST_FLOW_OK;
  self->draining = FALSE;
  self->output_eos = FALSE;
  self->last_delivery_us = 0;
  self->progressive_field_sequence = FALSE;
  g_clear_object(&self->parse_adapter);
  self->parse_pending = FALSE;
  if (self->device != NULL && !self->input_flushed) {
    /* Decoder-only flush can leave BCM70015's old firmware session unable
     * to output after an in-flight seek. Recreate the device from retained
     * caps/metadata. Keep token numbering monotonic across this boundary.
     * reopen_format retains its argument before releasing input_state.
     * This is intentional discard, not a natural caps change: never drain
     * old pictures after the flushing seek has canceled their delivery.
     */
    if (self->input_state == NULL ||
        !gst_crystalhd_reopen_format(decoder, self->input_state, TRUE))
      return FALSE;
    self->input_flushed = TRUE;
  }
  gst_crystalhd_clear_timestamps(self);
  self->need_second_field = FALSE;
  gst_crystalhd_resume_output(self);
  return TRUE;
}

static gboolean
gst_crystalhd_drain_complete(GstCrystalHdDec *self, gboolean active)
{
  return !active && self->drain_idle && self->output_eos &&
         g_queue_is_empty(&self->timestamps) && !self->need_second_field;
}

static GstFlowReturn
gst_crystalhd_drain(GstVideoDecoder *decoder)
{
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(decoder);
  gint64 started;
  gint64 deadline;
  guint64 generation = self->generation;
  BC_STATUS status;
  GstFlowReturn flow = GST_FLOW_OK;
  gboolean active = FALSE;

  if (!self->decoder_started || self->input_flushed)
    return GST_FLOW_OK;

  started = g_get_monotonic_time();
  deadline = started + GST_CRYSTALHD_DRAIN_TIMEOUT_US;
  flow = gst_crystalhd_wait_input_space(self, GST_CRYSTALHD_EOS_RESERVATION,
                                       deadline, 1);
  if (flow != GST_FLOW_OK)
    return flow;
  status = DtsFlushInput(self->device, 0);
  if (status != BC_STS_SUCCESS) {
    GST_ELEMENT_ERROR(self, STREAM, DECODE,
                      ("Could not drain CrystalHD decoder: %s",
                       gst_crystalhd_status_hint(status)),
                      ("DtsFlushInput returned %d", status));
    return GST_FLOW_ERROR;
  }

  self->draining = TRUE;
  self->output_eos = FALSE;
  self->drain_idle = FALSE;
  gst_crystalhd_wake_output(self);
  for (;;) {
    gint64 remaining;
    if (generation != self->generation || gst_crystalhd_is_flushing(self)) {
      flow = GST_FLOW_FLUSHING;
      goto done;
    }
    if (self->output_flow != GST_FLOW_OK) {
      flow = self->output_flow;
      goto done;
    }
    g_mutex_lock(&self->output_lock);
    active = self->output_active;
    g_mutex_unlock(&self->output_lock);
    /* Tokens retire after copying, not after the clocked downstream push.
     * EOS must wait for both, otherwise the final real picture can vanish. */
    if (gst_crystalhd_drain_complete(self, active))
      break;
    /* Asynchronous input may reach EOS long before clocked playback ends.
     * Bound lack of completed delivery, not the entire remaining movie. */
    deadline = MAX(deadline,
                    self->last_delivery_us + GST_CRYSTALHD_DRAIN_TIMEOUT_US);
    if (gst_crystalhd_playback_paused(self))
      deadline = g_get_monotonic_time() + GST_CRYSTALHD_DRAIN_TIMEOUT_US;
    remaining = gst_crystalhd_drain_remaining_us(deadline, g_get_monotonic_time());
    if (remaining == 0)
      break;
    flow = gst_crystalhd_wait_output(self, generation,
                                    MIN(remaining, (gint64)1000), 1);
    if (flow != GST_FLOW_OK)
      goto done;
  }

  if (!gst_crystalhd_drain_complete(self, active)) {
    GST_ELEMENT_ERROR(self, STREAM, DECODE,
                      ("CrystalHD drain ended before all input pictures were decoded"),
                      ("%u input timestamps remain after %" G_GINT64_FORMAT
                       " ms (delivery active: %d; output idle: %d; "
                       "firmware EOS: %d; "
                       "no-progress limit: 10000 ms)",
                       self->timestamps.length,
                       (g_get_monotonic_time() - started) / 1000, active,
                       self->drain_idle, self->output_eos));
    flow = GST_FLOW_ERROR;
  }
done:
  if (generation == self->generation) {
    self->draining = FALSE;
    if (flow == GST_FLOW_OK) {
      BC_DTS_STATUS final_status;
      memset(&final_status, 0, sizeof(final_status));
      if (DtsGetDriverStatus(self->device, &final_status) == BC_STS_SUCCESS)
        GST_INFO_OBJECT(self,
            "drain complete: captured=%u dropped=%u repeated=%u "
            "pib-misses=%u input-busy=%u",
            final_status.FramesCaptured, final_status.FramesDropped,
            final_status.FramesRepeated, final_status.PIBMissCount,
            final_status.InputBusyCount);
    }
  }
  return flow;
}

static gboolean
gst_crystalhd_sink_event(GstVideoDecoder *decoder, GstEvent *event)
{
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(decoder);
  GstVideoDecoderClass *parent =
      GST_VIDEO_DECODER_CLASS(gst_crystalhd_dec_parent_class);

  if (GST_EVENT_TYPE(event) == GST_EVENT_FLUSH_START) {
    gboolean result;
    g_mutex_lock(&self->output_lock);
    self->output_paused = TRUE;
    g_mutex_unlock(&self->output_lock);
    /* This event is nonserialized: cancel downstream BEFORE waiting on a
     * worker that may be in the sink's preroll/clock wait. */
    result = parent->sink_event(decoder, event);
    GST_VIDEO_DECODER_STREAM_LOCK(decoder);
    gst_crystalhd_quiesce_output(self, FALSE);
    GST_VIDEO_DECODER_STREAM_UNLOCK(decoder);
    return result;
  }
  if (GST_EVENT_TYPE(event) == GST_EVENT_FLUSH_STOP) {
    GST_VIDEO_DECODER_STREAM_LOCK(decoder);
    gst_crystalhd_quiesce_output(self, FALSE);
    GST_VIDEO_DECODER_STREAM_UNLOCK(decoder);
  }
  return parent->sink_event(decoder, event);
}

static void
gst_crystalhd_finalize(GObject *object)
{
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(object);
  g_assert(self->output_thread == NULL);
  g_cond_clear(&self->output_cond);
  g_mutex_clear(&self->output_lock);
  G_OBJECT_CLASS(gst_crystalhd_dec_parent_class)->finalize(object);
}

static void
gst_crystalhd_dec_class_init(GstCrystalHdDecClass *klass)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS(klass);
  GstVideoDecoderClass *decoder_class = GST_VIDEO_DECODER_CLASS(klass);

  G_OBJECT_CLASS(klass)->finalize = gst_crystalhd_finalize;

  gst_element_class_set_static_metadata(
      element_class, "CrystalHD hardware decoder", "Codec/Decoder/Video/Hardware",
      "Decodes video with Broadcom BCM70012/BCM70015 hardware",
      "CrystalHD contributors");
  gst_element_class_add_static_pad_template(element_class, &sink_template);
  gst_element_class_add_static_pad_template(element_class, &src_template);

  decoder_class->start = gst_crystalhd_start;
  decoder_class->stop = gst_crystalhd_stop;
  decoder_class->set_format = gst_crystalhd_set_format;
  decoder_class->parse = gst_crystalhd_parse;
  decoder_class->sink_query = gst_crystalhd_sink_query;
  decoder_class->sink_event = gst_crystalhd_sink_event;
  decoder_class->handle_frame = gst_crystalhd_handle_frame;
  decoder_class->flush = gst_crystalhd_flush;
  decoder_class->finish = gst_crystalhd_drain;
  decoder_class->drain = gst_crystalhd_drain;
}

static void
gst_crystalhd_dec_init(GstCrystalHdDec *self)
{
  g_queue_init(&self->timestamps);
  g_mutex_init(&self->output_lock);
  g_cond_init(&self->output_cond);
  self->output_paused = TRUE;
  gst_video_info_init(&self->output_info);
  gst_video_decoder_set_packetized(GST_VIDEO_DECODER(self), TRUE);
}

static gboolean
plugin_init(GstPlugin *plugin)
{
  GST_DEBUG_CATEGORY_INIT(gst_crystalhd_debug, "crystalhd", 0,
                          "CrystalHD hardware decoder");
  return gst_element_register(plugin, "crystalhddec", GST_RANK_PRIMARY + 1,
                              GST_TYPE_CRYSTALHD_DEC);
}

#ifndef PACKAGE
#define PACKAGE "crystalhd"
#endif

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, crystalhd,
                  "Broadcom CrystalHD hardware video decoder", plugin_init,
                  "3.10.0", "LGPL", "crystalhd",
                  "https://github.com/ahnhy1324/crystalhd")
