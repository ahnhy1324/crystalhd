/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "native.h"
#include <gst/video/video.h>

namespace crystalhd_kodi {
void NativeDecoder::Close() {
  if (pipeline) {
    /* NULL synchronously joins our decoder's worker before object disposal.
     * Never time out and free a still-running thread or borrowed RX pointer. */
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
  }
  if (bus) gst_object_unref(bus);
  pipeline = nullptr; input = nullptr; output = nullptr; bus = nullptr;
  draining = false;
}

bool NativeDecoder::CheckError() {
  if (!failed && bus) {
    GstMessage* message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
    if (message) {
      GError* detail = nullptr; gchar* debug = nullptr;
      gst_message_parse_error(message, &detail, &debug);
      error = detail ? detail->message : "GStreamer decoder error";
      g_clear_error(&detail); g_free(debug); gst_message_unref(message);
      failed = true;
    }
  }
  return !failed;
}

bool NativeDecoder::Build(GstCaps* caps, bool raw_test) {
  pipeline = gst_pipeline_new(nullptr);
  if (!pipeline) { error = "pipeline allocation failed"; return false; }
  const char* factories[] = {"appsrc", "h264parse", "capsfilter", "crystalhddec", "appsink"};
  GstElement* elements[5]{};
  for (unsigned i = 0; i < 5; ++i) {
    if (raw_test && (i == 1 || i == 2 || i == 3)) continue;
    elements[i] = gst_element_factory_make(factories[i], nullptr);
    if (!elements[i]) { error = std::string("missing required element: ") + factories[i]; Close(); return false; }
    gst_bin_add(GST_BIN(pipeline), elements[i]);
  }
  input = GST_APP_SRC(elements[0]); output = GST_APP_SINK(elements[4]);
  bus = gst_element_get_bus(pipeline);
  gst_app_src_set_caps(input, caps);
  g_object_set(input, "format", GST_FORMAT_TIME, "is-live", FALSE, "block", FALSE,
               "max-bytes", static_cast<guint64>(packet_limit),
               "max-buffers", static_cast<guint64>(8), nullptr);
  g_object_set(output, "sync", FALSE, "max-buffers", 4U, "drop", FALSE,
               "enable-last-sample", FALSE, nullptr);
  GstCaps* out_caps = gst_caps_from_string("video/x-raw,format=YUY2,interlace-mode=progressive");
  gst_app_sink_set_caps(output, out_caps); gst_caps_unref(out_caps);
  bool linked;
  if (raw_test) linked = gst_element_link(elements[0], elements[4]);
  else {
    GstCaps* au = gst_caps_from_string("video/x-h264,stream-format=byte-stream,alignment=au");
    g_object_set(elements[2], "caps", au, nullptr); gst_caps_unref(au);
    linked = gst_element_link_many(elements[0], elements[1], elements[2], elements[3], elements[4], nullptr);
  }
  if (!linked || gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    error = "required native pipeline failed to link/start"; CheckError(); Close(); return false;
  }
  return CheckError();
}

bool NativeDecoder::Open(unsigned width, unsigned height, unsigned fps_n, unsigned fps_d,
                         const uint8_t* extra, size_t extra_size) {
  Close(); failed = false; error.clear();
  if (!packed_layout(width, height, 0, static_cast<size_t>(width) * 2,
                     static_cast<size_t>(width) * height * 2) ||
      !fps_n || !fps_d || fps_n > 240000 || fps_d > 100000 || fps_n > 240 * fps_d ||
      !extra || extra_size < 7 || extra_size > packet_limit || extra[0] != 1) {
    error = "requires bounded progressive H264/AVC MP4 parameters"; failed = true; return false;
  }
  GError* init_error = nullptr;
  if (!gst_init_check(nullptr, nullptr, &init_error)) {
    error = init_error ? init_error->message : "GStreamer initialization failed";
    g_clear_error(&init_error); failed = true; return false;
  }
  GstBuffer* codec_data = gst_buffer_new_allocate(nullptr, extra_size, nullptr);
  gst_buffer_fill(codec_data, 0, extra, extra_size);
  GstCaps* caps = gst_caps_new_simple("video/x-h264", "stream-format", G_TYPE_STRING, "avc",
      "alignment", G_TYPE_STRING, "au", "width", G_TYPE_INT, static_cast<int>(width),
      "height", G_TYPE_INT, static_cast<int>(height), "framerate", GST_TYPE_FRACTION,
      static_cast<int>(fps_n), static_cast<int>(fps_d), "codec_data", GST_TYPE_BUFFER, codec_data, nullptr);
  gst_buffer_unref(codec_data);
  const bool ok = Build(caps, false); gst_caps_unref(caps); return ok;
}

NativeDecoder::Submit NativeDecoder::Push(const uint8_t* data, size_t size,
                                         double pts, double dts, double duration) {
  if (!input || draining || !CheckError()) return Submit::Error;
  uint64_t p, d;
  if (!data || !size || size > packet_limit || !to_clock(pts, true, p) ||
      !to_clock(dts, false, d) || !std::isfinite(duration) || duration < 0 || duration > 10000000.0) {
    error = "invalid/missing packet payload or timestamps"; failed = true; return Submit::Error;
  }
  /* Kodi may retry a packet only if nothing was consumed. There is exactly one
   * producer. Check the whole call before copying/pushing into the bounded
   * appsrc; do not block Kodi's decode thread waiting for its own output. */
  if (gst_app_src_get_current_level_buffers(input) >= 8 ||
      gst_app_src_get_current_level_bytes(input) > packet_limit - size) return Submit::Retry;
  GstBuffer* buffer = gst_buffer_new_allocate(nullptr, size, nullptr);
  gst_buffer_fill(buffer, 0, data, size);
  GST_BUFFER_PTS(buffer) = p; GST_BUFFER_DTS(buffer) = d;
  GST_BUFFER_DURATION(buffer) = static_cast<guint64>(duration * 1000.0);
  const GstFlowReturn flow = gst_app_src_push_buffer(input, buffer);
  /* push_buffer consumes the GstBuffer even on error; never retry it. */
  if (flow != GST_FLOW_OK) { error = "appsrc rejected native packet"; failed = true; }
  return Submit::Accepted;
}

GstSample* NativeDecoder::Next(bool drain, Result& result) {
  result = Result::Error;
  if (!input || !CheckError()) return nullptr;
  if (drain && !draining) {
    draining = true;
    if (gst_app_src_end_of_stream(input) != GST_FLOW_OK) {
      error = "native drain request failed"; failed = true; return nullptr;
    }
  }
  GstSample* sample = gst_app_sink_try_pull_sample(output, GST_MSECOND);
  if (sample) { result = Result::Picture; return sample; }
  if (!CheckError()) return nullptr;
  /* Only the decoder's real EOS, after the complete sink queue, is EOF. */
  if (draining && gst_app_sink_is_eos(output)) result = Result::Eof;
  else if (draining || gst_app_src_get_current_level_buffers(input) >= 8)
    result = Result::Wait;
  else result = Result::NeedInput;
  return nullptr;
}

#ifdef CRYSTALHD_KODI_TEST
bool NativeDecoder::OpenRawTest(unsigned width, unsigned height) {
  Close(); failed = false; error.clear(); gst_init(nullptr, nullptr);
  GstCaps* caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "YUY2",
      "width", G_TYPE_INT, static_cast<int>(width), "height", G_TYPE_INT, static_cast<int>(height),
      "interlace-mode", G_TYPE_STRING, "progressive", "framerate", GST_TYPE_FRACTION, 30, 1, nullptr);
  const bool ok = Build(caps, true); gst_caps_unref(caps); return ok;
}
#endif
}
