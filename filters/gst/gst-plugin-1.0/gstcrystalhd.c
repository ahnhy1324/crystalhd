/*
 * GStreamer 1.x decoder for Broadcom Crystal HD BCM70012/BCM70015 devices.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <stdint.h>
#include <string.h>

#include <gst/gst.h>
#include <gst/video/gstvideodecoder.h>
#include <gst/video/video.h>

#include <bc_dts_defs.h>
#include <bc_dts_types.h>
#include <libcrystalhd_if.h>

#define GST_TYPE_CRYSTALHD_DEC (gst_crystalhd_dec_get_type())
#define GST_CRYSTALHD_DEC(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_CRYSTALHD_DEC, GstCrystalHdDec))

#define CRYSTALHD_TIMESTAMP_STEP 100000ULL
#define CRYSTALHD_INPUT_RETRIES 1000U
#define CRYSTALHD_DRAIN_RETRIES 500U

typedef struct {
  guint64 hardware_timestamp;
  guint32 frame_number;
} CrystalHdTimestamp;

typedef struct _GstCrystalHdDec {
  GstVideoDecoder parent;

  HANDLE device;
  gboolean decoder_open;
  gboolean decoder_started;
  gboolean is_70012;
  gboolean output_configured;
  gboolean need_second_field;
  guint32 field_frame_number;
  guint width;
  guint height;
  guint64 next_hardware_timestamp;
  GQueue timestamps;
  GstVideoCodecState *input_state;
  GstVideoInfo output_info;
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
        "video/x-vc1, parsed=(boolean)true; "
        "video/x-wmv, wmvversion=(int)3"));

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE(
    "src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS("video/x-raw, format=(string)YUY2, "
                    "width=(int)[1,1920], height=(int)[1,1088], "
                    "framerate=(fraction)[0/1,MAX]"));

static void
gst_crystalhd_clear_timestamps(GstCrystalHdDec *self)
{
  g_queue_clear_full(&self->timestamps, g_free);
}

static void
gst_crystalhd_close_device(GstCrystalHdDec *self)
{
  if (self->decoder_started) {
    DtsStopDecoder(self->device);
    self->decoder_started = FALSE;
  }

  if (self->decoder_open) {
    DtsCloseDecoder(self->device);
    self->decoder_open = FALSE;
  }

  if (self->device != NULL) {
    DtsDeviceClose(self->device);
    self->device = NULL;
  }

  self->output_configured = FALSE;
  self->need_second_field = FALSE;
  gst_crystalhd_clear_timestamps(self);
}

static BC_MEDIA_SUBTYPE
gst_crystalhd_subtype_from_caps(GstCaps *caps)
{
  const GstStructure *structure = gst_caps_get_structure(caps, 0);
  const gchar *name = gst_structure_get_name(structure);

  if (g_str_equal(name, "video/x-h264"))
    return BC_MSUBTYPE_H264;
  if (g_str_equal(name, "video/mpeg"))
    return BC_MSUBTYPE_MPEG2VIDEO;
  if (g_str_equal(name, "video/x-vc1"))
    return BC_MSUBTYPE_VC1;
  if (g_str_equal(name, "video/x-wmv"))
    return BC_MSUBTYPE_WMV3;

  return BC_MSUBTYPE_INVALID;
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

  link = self->timestamps.head;
  if (link_out != NULL)
    *link_out = link;
  return link != NULL ? link->data : NULL;
}

static gboolean
gst_crystalhd_configure_output(GstCrystalHdDec *self, guint width, guint height,
                               gboolean interlaced)
{
  GstVideoDecoder *decoder = GST_VIDEO_DECODER(self);
  GstVideoCodecState *state;

  if (width == 0 || height == 0 || width > 1920 || height > 1088) {
    GST_ERROR_OBJECT(self, "invalid output dimensions %ux%u", width, height);
    return FALSE;
  }

  if (self->output_configured && self->width == width &&
      self->height == height)
    return TRUE;

  state = gst_video_decoder_set_output_state(
      decoder, GST_VIDEO_FORMAT_YUY2, width, height, self->input_state);
  if (state == NULL)
    return FALSE;

  state->info.interlace_mode = interlaced ? GST_VIDEO_INTERLACE_MODE_MIXED
                                          : GST_VIDEO_INTERLACE_MODE_PROGRESSIVE;
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
gst_crystalhd_copy_output(GstCrystalHdDec *self, BC_DTS_PROC_OUT *output)
{
  GstVideoDecoder *decoder = GST_VIDEO_DECODER(self);
  GstVideoCodecFrame *frame;
  GstVideoFrame video_frame;
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
  GstFlowReturn flow;

  interlaced = (output->PicInfo.flags & VDEC_FLAG_INTERLACED_SRC) != 0;
  bottom_field = (output->PicInfo.flags & VDEC_FLAG_BOTTOMFIELD) != 0;

  if (!gst_crystalhd_configure_output(self, width, height, interlaced))
    return GST_FLOW_NOT_NEGOTIATED;

  entry = gst_crystalhd_find_timestamp(self, output->PicInfo.timeStamp,
                                       &timestamp_link);
  if (self->need_second_field) {
    frame_number = self->field_frame_number;
  } else if (entry != NULL) {
    frame_number = entry->frame_number;
  } else {
    frame = gst_video_decoder_get_oldest_frame(decoder);
    if (frame == NULL) {
      GST_WARNING_OBJECT(self, "decoded picture has no queued input frame");
      return GST_FLOW_OK;
    }
    frame_number = frame->system_frame_number;
    gst_video_codec_frame_unref(frame);
  }

  frame = gst_video_decoder_get_frame(decoder, frame_number);
  if (frame == NULL) {
    GST_WARNING_OBJECT(self, "input frame %u is no longer queued", frame_number);
    return GST_FLOW_OK;
  }

  if (frame->output_buffer == NULL) {
    flow = gst_video_decoder_allocate_output_frame(decoder, frame);
    if (flow != GST_FLOW_OK) {
      gst_video_codec_frame_unref(frame);
      return flow;
    }
  }

  if (!gst_video_frame_map(&video_frame, &self->output_info,
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

  if (output->Ybuff == NULL || output->YBuffDoneSz * 4U < source_stride * rows) {
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
    gst_video_codec_frame_unref(frame);
    return GST_FLOW_OK;
  }

  self->need_second_field = FALSE;
  if (timestamp_link != NULL) {
    g_free(timestamp_link->data);
    g_queue_delete_link(&self->timestamps, timestamp_link);
  }

  GST_LOG_OBJECT(self, "decoded frame %u (%ux%u), picture %u", frame_number,
                 width, height, output->PicInfo.picture_number);
  return gst_video_decoder_finish_frame(decoder, frame);
}

static GstFlowReturn
gst_crystalhd_receive_one(GstCrystalHdDec *self, guint timeout_ms,
                          gboolean *activity)
{
  BC_DTS_PROC_OUT output;
  BC_STATUS status;
  GstFlowReturn flow = GST_FLOW_OK;

  memset(&output, 0, sizeof(output));
  output.PicInfo.width = self->width;
  output.PicInfo.height = self->height;
  *activity = FALSE;

  status = DtsProcOutputNoCopy(self->device, timeout_ms, &output);
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
      flow = gst_crystalhd_copy_output(self, &output);
    else
      GST_WARNING_OBJECT(self, "decoder returned a picture without valid PIB");

    status = DtsReleaseOutputBuffs(self->device, NULL, FALSE);
    if (status != BC_STS_SUCCESS && flow == GST_FLOW_OK) {
      GST_ERROR_OBJECT(self, "failed to release output buffers: %d", status);
      flow = GST_FLOW_ERROR;
    }
    return flow;
  }

  if (status == BC_STS_NO_DATA || status == BC_STS_BUSY ||
      status == BC_STS_TIMEOUT)
    return GST_FLOW_OK;

  GST_ERROR_OBJECT(self, "DtsProcOutputNoCopy failed: %d", status);
  return GST_FLOW_ERROR;
}

static GstFlowReturn
gst_crystalhd_receive_available(GstCrystalHdDec *self)
{
  guint attempts;

  for (attempts = 0; attempts < 64; attempts++) {
    BC_DTS_STATUS decoder_status;
    BC_STATUS status;
    gboolean activity;

    memset(&decoder_status, 0, sizeof(decoder_status));
    status = DtsGetDriverStatus(self->device, &decoder_status);
    if (status != BC_STS_SUCCESS) {
      GST_ERROR_OBJECT(self, "DtsGetDriverStatus failed: %d", status);
      return GST_FLOW_ERROR;
    }
    if (decoder_status.ReadyListCount == 0)
      return GST_FLOW_OK;

    GstFlowReturn flow = gst_crystalhd_receive_one(self, 0, &activity);
    if (flow != GST_FLOW_OK || !activity)
      return flow;
  }

  return GST_FLOW_OK;
}

static gboolean
gst_crystalhd_start(GstVideoDecoder *decoder)
{
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(decoder);

  gst_crystalhd_close_device(self);
  self->next_hardware_timestamp = CRYSTALHD_TIMESTAMP_STEP;
  gst_video_decoder_set_packetized(decoder, TRUE);
  return TRUE;
}

static gboolean
gst_crystalhd_stop(GstVideoDecoder *decoder)
{
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(decoder);

  gst_crystalhd_close_device(self);
  g_clear_pointer(&self->input_state, gst_video_codec_state_unref);
  return TRUE;
}

static gboolean
gst_crystalhd_set_format(GstVideoDecoder *decoder, GstVideoCodecState *state)
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

  gst_crystalhd_close_device(self);
  g_clear_pointer(&self->input_state, gst_video_codec_state_unref);
  self->input_state = gst_video_codec_state_ref(state);

  subtype = gst_crystalhd_subtype_from_caps(state->caps);
  if (subtype == BC_MSUBTYPE_INVALID)
    return FALSE;

  memset(&input_format, 0, sizeof(input_format));
  input_format.FGTEnable = FALSE;
  input_format.Progressive = TRUE;
  input_format.OptFlags = 0x80000000U | vdecFrameRate59_94 | 0x40U;
  input_format.mSubtype = subtype;
  input_format.width = GST_VIDEO_INFO_WIDTH(&state->info);
  input_format.height = GST_VIDEO_INFO_HEIGHT(&state->info);
  if (subtype == BC_MSUBTYPE_H264)
    input_format.startCodeSz = 4;

  codec_data_value = gst_structure_get_value(structure, "codec_data");
  if (codec_data_value != NULL) {
    GstBuffer *codec_data = gst_value_get_buffer(codec_data_value);
    if (codec_data != NULL &&
        gst_buffer_map(codec_data, &codec_data_map, GST_MAP_READ)) {
      input_format.pMetaData = codec_data_map.data;
      input_format.metaDataSz = codec_data_map.size;
      codec_data_mapped = TRUE;
    }
  }

  mode = DTS_PLAYBACK_MODE | DTS_LOAD_FILE_PLAY_FW | DTS_SKIP_TX_CHK_CPB |
         DTS_PLAYBACK_DROP_RPT_MODE | DTS_SINGLE_THREADED_MODE |
         DTS_DFLT_RESOLUTION(vdecRESOLUTION_1080p23_976);
  status = DtsDeviceOpen(&self->device, mode);
  if (status != BC_STS_SUCCESS) {
    GST_ELEMENT_ERROR(self, RESOURCE, OPEN_READ,
                      ("Could not open /dev/crystalhd"),
                      ("DtsDeviceOpen returned %d", status));
    goto fail;
  }

  memset(&version, 0, sizeof(version));
  status = DtsCrystalHDVersion(self->device, &version);
  if (status != BC_STS_SUCCESS)
    goto fail_status;
  self->is_70012 = version.device == 0;

  status = DtsSetInputFormat(self->device, &input_format);
  if (status != BC_STS_SUCCESS)
    goto fail_status;

  status = DtsOpenDecoder(self->device, BC_STREAM_TYPE_ES);
  if (status != BC_STS_SUCCESS)
    goto fail_status;
  self->decoder_open = TRUE;

  status = DtsSetColorSpace(self->device, OUTPUT_MODE422_YUY2);
  if (status != BC_STS_SUCCESS)
    goto fail_status;

  status = DtsStartDecoder(self->device);
  if (status != BC_STS_SUCCESS)
    goto fail_status;
  self->decoder_started = TRUE;

  status = DtsStartCapture(self->device);
  if (status != BC_STS_SUCCESS)
    goto fail_status;

  if (codec_data_mapped)
    gst_buffer_unmap(gst_value_get_buffer(codec_data_value), &codec_data_map);

  GST_INFO_OBJECT(self, "opened BCM7001%u for caps %" GST_PTR_FORMAT,
                  self->is_70012 ? 2 : 5, state->caps);
  return TRUE;

fail_status:
  GST_ELEMENT_ERROR(self, LIBRARY, INIT,
                    ("Could not initialize CrystalHD decoder"),
                    ("libcrystalhd returned %d", status));
fail:
  if (codec_data_mapped)
    gst_buffer_unmap(gst_value_get_buffer(codec_data_value), &codec_data_map);
  gst_crystalhd_close_device(self);
  return FALSE;
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
  guint attempt;

  if (!self->decoder_started || frame->input_buffer == NULL)
    return gst_video_decoder_drop_frame(decoder, frame);

  if (!gst_buffer_map(frame->input_buffer, &map, GST_MAP_READ))
    return gst_video_decoder_drop_frame(decoder, frame);

  entry = g_new0(CrystalHdTimestamp, 1);
  entry->hardware_timestamp = self->next_hardware_timestamp;
  entry->frame_number = frame->system_frame_number;
  self->next_hardware_timestamp += CRYSTALHD_TIMESTAMP_STEP;
  g_queue_push_tail(&self->timestamps, entry);

  for (attempt = 0; attempt < CRYSTALHD_INPUT_RETRIES; attempt++) {
    status = DtsProcInput(self->device, map.data, map.size,
                          entry->hardware_timestamp, 0);
    if (status != BC_STS_BUSY)
      break;

    flow = gst_crystalhd_receive_available(self);
    if (flow != GST_FLOW_OK) {
      gst_buffer_unmap(frame->input_buffer, &map);
      return flow;
    }
    g_usleep(1000);
  }
  gst_buffer_unmap(frame->input_buffer, &map);

  if (status != BC_STS_SUCCESS) {
    GList *tail = self->timestamps.tail;
    if (tail != NULL && tail->data == entry) {
      g_free(entry);
      g_queue_delete_link(&self->timestamps, tail);
    }
    GST_ERROR_OBJECT(self, "DtsProcInput failed: %d", status);
    return gst_video_decoder_drop_frame(decoder, frame);
  }

  return gst_crystalhd_receive_available(self);
}

static gboolean
gst_crystalhd_flush(GstVideoDecoder *decoder)
{
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(decoder);

  if (self->device != NULL)
    DtsFlushInput(self->device, 4);
  gst_crystalhd_clear_timestamps(self);
  self->need_second_field = FALSE;
  return TRUE;
}

static GstFlowReturn
gst_crystalhd_drain(GstVideoDecoder *decoder)
{
  GstCrystalHdDec *self = GST_CRYSTALHD_DEC(decoder);
  guint attempt;

  if (!self->decoder_started)
    return GST_FLOW_OK;

  if (DtsFlushInput(self->device, 0) != BC_STS_SUCCESS)
    return GST_FLOW_ERROR;

  for (attempt = 0; attempt < CRYSTALHD_DRAIN_RETRIES; attempt++) {
    BC_DTS_STATUS decoder_status;
    BC_STATUS status;
    gboolean activity;
    gboolean eos = FALSE;

    memset(&decoder_status, 0, sizeof(decoder_status));
    status = DtsGetDriverStatus(self->device, &decoder_status);
    if (status != BC_STS_SUCCESS)
      return GST_FLOW_ERROR;

    if (decoder_status.ReadyListCount > 0) {
      GstFlowReturn flow = gst_crystalhd_receive_one(self, 0, &activity);
      if (flow != GST_FLOW_OK)
        return flow;
    } else {
      activity = FALSE;
    }

    if (DtsIsEndOfStream(self->device, (guint8 *)&eos) == BC_STS_SUCCESS && eos)
      break;
    if (!activity && self->timestamps.length == 0)
      break;
    if (!activity)
      g_usleep(10000);
  }

  return GST_FLOW_OK;
}

static void
gst_crystalhd_dec_class_init(GstCrystalHdDecClass *klass)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS(klass);
  GstVideoDecoderClass *decoder_class = GST_VIDEO_DECODER_CLASS(klass);

  gst_element_class_set_static_metadata(
      element_class, "CrystalHD hardware decoder", "Codec/Decoder/Video/Hardware",
      "Decodes video with Broadcom BCM70012/BCM70015 hardware",
      "CrystalHD contributors");
  gst_element_class_add_static_pad_template(element_class, &sink_template);
  gst_element_class_add_static_pad_template(element_class, &src_template);

  decoder_class->start = gst_crystalhd_start;
  decoder_class->stop = gst_crystalhd_stop;
  decoder_class->set_format = gst_crystalhd_set_format;
  decoder_class->handle_frame = gst_crystalhd_handle_frame;
  decoder_class->flush = gst_crystalhd_flush;
  decoder_class->finish = gst_crystalhd_drain;
  decoder_class->drain = gst_crystalhd_drain;
}

static void
gst_crystalhd_dec_init(GstCrystalHdDec *self)
{
  g_queue_init(&self->timestamps);
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
