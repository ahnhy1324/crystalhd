// SPDX-License-Identifier: LGPL-2.1-or-later
// Optional hardware probe: libavformat supplies complete WMV3/VC-1 packets,
// avoiding dependence on a particular GStreamer ASF/VC-1 parser version.
// Feeding and EOS share a 25-second deadline and a bounded appsrc queue.
// Still use `timeout -k 5s 35s`: userspace cannot bound a stuck driver close.
#include <gst/app/gstappsrc.h>
#include <gst/video/video.h>
extern "C" {
#include <libavformat/avformat.h>
}
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

static constexpr guint64 kQueueBytes = 4 * 1024 * 1024;
static constexpr int kMaxPacketBytes = 16 * 1024 * 1024;
static constexpr gint64 kTimeoutUs = 25 * G_USEC_PER_SEC;

struct Deadline {
  gint64 expires = g_get_monotonic_time() + kTimeoutUs;
};

static int DeadlineExpired(void *data) {
  return g_get_monotonic_time() >= static_cast<Deadline *>(data)->expires;
}

static GstClockTime Remaining(const Deadline &deadline) {
  const gint64 remaining = deadline.expires - g_get_monotonic_time();
  return remaining > 0 ? static_cast<GstClockTime>(remaining) * GST_USECOND : 0;
}

struct Audit {
  unsigned int frames = 0;
  int width = 0;
  int height = 0;
  bool invalid = false;
  GstClockTime previous_pts = GST_CLOCK_TIME_NONE;
  GChecksum *checksum = g_checksum_new(G_CHECKSUM_SHA256);
};

static void CountFrame(GstElement *, GstBuffer *buffer, GstPad *pad, gpointer data) {
  auto *audit = static_cast<Audit *>(data);
  GstCaps *caps = gst_pad_get_current_caps(pad);
  GstVideoInfo info;
  GstVideoFrame frame;
  const GstClockTime pts = GST_BUFFER_PTS(buffer);
  if (!caps || !gst_video_info_from_caps(&info, caps) ||
      GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_YUY2 ||
      GST_VIDEO_INFO_WIDTH(&info) != audit->width ||
      GST_VIDEO_INFO_HEIGHT(&info) != audit->height ||
      (GST_CLOCK_TIME_IS_VALID(pts) && GST_CLOCK_TIME_IS_VALID(audit->previous_pts) &&
       pts < audit->previous_pts) ||
      !gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ)) {
    audit->invalid = true;
  } else {
    if (GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0) < audit->width * 2) {
      audit->invalid = true;
    } else {
      for (int row = 0; row < audit->height; ++row)
        g_checksum_update(audit->checksum,
            static_cast<const guchar *>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0)) +
                row * GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0), audit->width * 2);
    }
    gst_video_frame_unmap(&frame);
  }
  if (caps)
    gst_caps_unref(caps);
  if (GST_CLOCK_TIME_IS_VALID(pts))
    audit->previous_pts = pts;
  ++audit->frames;
}

static GstClockTime ClockTime(int64_t value, AVRational time_base) {
  if (value == AV_NOPTS_VALUE || value < 0)
    return GST_CLOCK_TIME_NONE;
  const int64_t scaled = av_rescale_q(value, time_base,
      AVRational{1, static_cast<int>(GST_SECOND)});
  return scaled < 0 ? GST_CLOCK_TIME_NONE : static_cast<GstClockTime>(scaled);
}

static GstMessage *TerminalMessage(GstBus *bus, GstClockTime timeout = 0) {
  return gst_bus_timed_pop_filtered(bus, timeout,
      static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
}

static bool WaitForQueue(GstElement *source, GstBus *bus, Deadline *deadline,
                         GstMessage **message) {
  /* One producer, nonblocking push: at most kQueueBytes plus one bounded
   * packet can be queued. Poll the bus/deadline while applying backpressure
   * instead of blocking inside push_buffer after a downstream failure.
   */
  do {
    *message = TerminalMessage(bus);
    if (*message || DeadlineExpired(deadline))
      return false;
    if (gst_app_src_get_current_level_bytes(GST_APP_SRC(source)) < kQueueBytes)
      return true;
    g_usleep(5000);
  } while (true);
}

static int SelfTest() {
  GError *error = nullptr;
  GstElement *pipeline = gst_parse_launch(
      "appsrc name=source format=time is-live=false ! "
      "fakesink name=sink sync=false enable-last-sample=false signal-handoffs=true", &error);
  if (error || !pipeline) {
    g_clear_error(&error);
    if (pipeline)
      gst_object_unref(pipeline);
    return 1;
  }
  GstElement *source = gst_bin_get_by_name(GST_BIN(pipeline), "source");
  GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
  GstCaps *caps = gst_caps_from_string(
      "video/x-raw,format=YUY2,width=32,height=24,framerate=25/1");
  gst_app_src_set_caps(GST_APP_SRC(source), caps);
  gst_caps_unref(caps);
  Audit audit;
  audit.width = 32;
  audit.height = 24;
  g_signal_connect(sink, "handoff", G_CALLBACK(CountFrame), &audit);
  GstBus *bus = gst_element_get_bus(pipeline);
  Deadline deadline;
  GstMessage *message = nullptr;
  bool ok = gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE;
  for (unsigned int i = 0; ok && i < 12; ++i) {
    ok = WaitForQueue(source, bus, &deadline, &message);
    if (!ok)
      break;
    GstBuffer *buffer = gst_buffer_new_allocate(nullptr, 32 * 24 * 2, nullptr);
    gst_buffer_memset(buffer, 0, i, 32 * 24 * 2);
    GST_BUFFER_PTS(buffer) = i * GST_SECOND / 25;
    GST_BUFFER_DURATION(buffer) = GST_SECOND / 25;
    ok = gst_app_src_push_buffer(GST_APP_SRC(source), buffer) == GST_FLOW_OK;
  }
  if (ok)
    ok = gst_app_src_end_of_stream(GST_APP_SRC(source)) == GST_FLOW_OK;
  if (!message)
    message = TerminalMessage(bus, ok ? Remaining(deadline) : 0);
  const bool eos = message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS;
  if (eos) {
    /* EOS serializes after all handoffs; exercise rejection without hardware. */
    GstPad *pad = gst_element_get_static_pad(sink, "sink");
    GstBuffer *buffer = gst_buffer_new_allocate(nullptr, 32 * 24 * 2, nullptr);
    Audit bad;
    bad.width = 34;
    bad.height = 24;
    CountFrame(nullptr, buffer, pad, &bad);
    ok = ok && bad.invalid;
    bad.invalid = false;
    bad.width = 32;
    bad.previous_pts = GST_SECOND;
    GST_BUFFER_PTS(buffer) = 0;
    CountFrame(nullptr, buffer, pad, &bad);
    ok = ok && bad.invalid;
    gst_buffer_unref(buffer);
    gst_object_unref(pad);
    g_checksum_free(bad.checksum);
  }
  gst_element_set_state(pipeline, GST_STATE_NULL);
  ok = ok && eos && !audit.invalid && audit.frames == 12;
  Deadline expired;
  expired.expires = g_get_monotonic_time() - 1;
  ok = ok && DeadlineExpired(&expired) && Remaining(expired) == 0 &&
      ClockTime(AV_NOPTS_VALUE, AVRational{1, 25}) == GST_CLOCK_TIME_NONE &&
      ClockTime(1, AVRational{1, 25}) == GST_SECOND / 25;
  std::printf("Codec probe hardware-free audit self-test: %s\n", ok ? "passed" : "failed");
  if (message)
    gst_message_unref(message);
  g_checksum_free(audit.checksum);
  gst_object_unref(source);
  gst_object_unref(sink);
  gst_object_unref(bus);
  gst_object_unref(pipeline);
  return ok ? 0 : 1;
}

int main(int argc, char **argv) {
  gst_init(&argc, &argv);
  if (argc == 2 && std::strcmp(argv[1], "--self-test") == 0)
    return SelfTest();
  char *end = nullptr;
  errno = 0;
  const unsigned long expected = argc == 3 ? std::strtoul(argv[2], &end, 10) : 0;
  if (argc != 3 || !expected || errno || !end || *end ||
      expected > std::numeric_limits<unsigned int>::max()) {
    std::fprintf(stderr, "usage: %s VIDEO EXPECTED_FRAMES | --self-test\n", argv[0]);
    return 2;
  }
  Deadline deadline;
  AVFormatContext *format = avformat_alloc_context();
  if (!format)
    return 1;
  format->interrupt_callback = AVIOInterruptCB{DeadlineExpired, &deadline};
  if (avformat_open_input(&format, argv[1], nullptr, nullptr) < 0)
    return 1;
  if (avformat_find_stream_info(format, nullptr) < 0) {
    avformat_close_input(&format);
    return 1;
  }
  const int index = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (index < 0) {
    avformat_close_input(&format);
    return 1;
  }
  AVStream *stream = format->streams[index];
  const AVCodecParameters *parameters = stream->codecpar;
  const bool vc1 = parameters->codec_id == AV_CODEC_ID_VC1;
  if ((!vc1 && parameters->codec_id != AV_CODEC_ID_WMV3) ||
      parameters->extradata_size <= 0 || parameters->extradata_size > kMaxPacketBytes ||
      !parameters->extradata || parameters->width <= 0 || parameters->width > 1920 ||
      parameters->height <= 0 || parameters->height > 1088) {
    std::fprintf(stderr, "Probe requires WMV3/VC-1 with codec metadata and dimensions\n");
    avformat_close_input(&format);
    return 2;
  }
  GError *error = nullptr;
  GstElement *pipeline = gst_parse_launch(
      "appsrc name=source format=time is-live=false ! crystalhddec ! "
      "fakesink name=sink sync=false enable-last-sample=false signal-handoffs=true", &error);
  if (error || !pipeline) {
    std::fprintf(stderr, "%s\n", error ? error->message : "No pipeline");
    g_clear_error(&error);
    if (pipeline)
      gst_object_unref(pipeline);
    avformat_close_input(&format);
    return 1;
  }
  GstElement *source = gst_bin_get_by_name(GST_BIN(pipeline), "source");
  GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
  g_object_set(source, "max-bytes", kQueueBytes, "block", FALSE, nullptr);
  GstCaps *caps = gst_caps_new_simple("video/x-wmv",
      "wmvversion", G_TYPE_INT, 3, "format", G_TYPE_STRING, vc1 ? "WVC1" : "WMV3",
      "stream-format", G_TYPE_STRING, "asf", "header-format", G_TYPE_STRING, "asf",
      "width", G_TYPE_INT, parameters->width, "height", G_TYPE_INT, parameters->height,
      nullptr);
  AVRational rate = av_guess_frame_rate(format, stream, nullptr);
  if (rate.num > 0 && rate.den > 0)
    gst_caps_set_simple(caps, "framerate", GST_TYPE_FRACTION, rate.num, rate.den, nullptr);
  GstBuffer *metadata = gst_buffer_new_allocate(nullptr, parameters->extradata_size, nullptr);
  gst_buffer_fill(metadata, 0, parameters->extradata, parameters->extradata_size);
  gst_caps_set_simple(caps, "codec_data", GST_TYPE_BUFFER, metadata, nullptr);
  gst_buffer_unref(metadata);
  gst_app_src_set_caps(GST_APP_SRC(source), caps);
  gst_caps_unref(caps);
  Audit audit;
  audit.width = parameters->width;
  audit.height = parameters->height;
  g_signal_connect(sink, "handoff", G_CALLBACK(CountFrame), &audit);
  GstBus *bus = gst_element_get_bus(pipeline);
  bool ok = gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE;
  AVPacket *packet = av_packet_alloc();
  int read_result = 0;
  unsigned int packets = 0;
  GstMessage *message = nullptr;
  if (!packet)
    ok = false;
  while (ok && !DeadlineExpired(&deadline) &&
         (read_result = av_read_frame(format, packet)) >= 0) {
    if (packet->stream_index == index) {
      if (packet->size <= 0 || packet->size > kMaxPacketBytes || !packet->data) {
        std::fprintf(stderr, "Invalid/oversized compressed packet: %d bytes\n", packet->size);
        ok = false;
        break;
      }
      if (!WaitForQueue(source, bus, &deadline, &message)) {
        ok = false;
        break;
      }
      GstBuffer *input = gst_buffer_new_allocate(nullptr, packet->size, nullptr);
      if (!input) {
        ok = false;
        break;
      }
      gst_buffer_fill(input, 0, packet->data, packet->size);
      GST_BUFFER_PTS(input) = ClockTime(packet->pts, stream->time_base);
      GST_BUFFER_DTS(input) = ClockTime(packet->dts, stream->time_base);
      GST_BUFFER_DURATION(input) = packet->duration > 0
          ? ClockTime(packet->duration, stream->time_base) : GST_CLOCK_TIME_NONE;
      if (!(packet->flags & AV_PKT_FLAG_KEY))
        GST_BUFFER_FLAG_SET(input, GST_BUFFER_FLAG_DELTA_UNIT);
      ok = gst_app_src_push_buffer(GST_APP_SRC(source), input) == GST_FLOW_OK;
      if (ok)
        ++packets;
    }
    av_packet_unref(packet);
  }
  if (ok && (read_result != AVERROR_EOF || DeadlineExpired(&deadline)))
    ok = false;
  av_packet_free(&packet);
  if (ok)
    ok = gst_app_src_end_of_stream(GST_APP_SRC(source)) == GST_FLOW_OK;
  if (!message)
    message = TerminalMessage(bus, ok ? Remaining(deadline) : 0);
  bool eos = message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS;
  if (message && !eos) {
    gchar *debug = nullptr;
    gst_message_parse_error(message, &error, &debug);
    std::fprintf(stderr, "%s: %s\n%s\n", GST_OBJECT_NAME(message->src),
        error->message, debug ? debug : "");
    g_clear_error(&error);
    g_free(debug);
  } else if (!message) {
    std::fprintf(stderr, "Probe failed or exceeded its 25-second feed/EOS deadline\n");
  }
  gst_element_set_state(pipeline, GST_STATE_NULL);
  std::printf("%s: %u packets; %u/%lu YUY2 frames; EOS=%s; SHA256=%s\n",
      vc1 ? "VC-1" : "WMV3", packets, audit.frames, expected, eos ? "yes" : "no",
      g_checksum_get_string(audit.checksum));
  if (audit.invalid)
    std::fprintf(stderr, "Invalid dimensions/YUY2 buffers or regressing output timestamps\n");
  ok = ok && eos && !audit.invalid && audit.frames == expected;
  if (message)
    gst_message_unref(message);
  g_checksum_free(audit.checksum);
  gst_object_unref(source);
  gst_object_unref(sink);
  gst_object_unref(bus);
  gst_object_unref(pipeline);
  avformat_close_input(&format);
  return ok ? 0 : 1;
}
