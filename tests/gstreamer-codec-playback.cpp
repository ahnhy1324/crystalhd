// SPDX-License-Identifier: LGPL-2.1-or-later
// Optional hardware probe: libavformat supplies complete codec packets,
// avoiding dependence on a particular demuxer/parser version.
// Feeding and EOS share a 25-second deadline and a bounded appsrc queue.
// Still use `timeout -k 5s 35s`: userspace cannot bound a stuck driver close.
#include <gst/app/gstappsrc.h>
#include <gst/video/video.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>
#include "phase1-progress.h"

static constexpr guint64 kQueueBytes = 4 * 1024 * 1024;
static constexpr int kMaxPacketBytes = 16 * 1024 * 1024;
static constexpr gint64 kTimeoutUs = 25 * G_USEC_PER_SEC;

enum class ExpectedField { kAny, kProgressive, kTopFirst, kBottomFirst };

static guint32 CanonicalFieldFlags(GstBufferFlags flags) {
  guint32 result = 0;
  if (flags & GST_VIDEO_BUFFER_FLAG_INTERLACED)
    result |= 1U << 0;
  if (flags & GST_VIDEO_BUFFER_FLAG_TFF)
    result |= 1U << 1;
  if (flags & GST_VIDEO_BUFFER_FLAG_RFF)
    result |= 1U << 2;
  if (flags & GST_VIDEO_BUFFER_FLAG_ONEFIELD)
    result |= 1U << 3;
  return result;
}

static guint32 CanonicalInterlaceMode(GstVideoInterlaceMode mode) {
  switch (mode) {
    case GST_VIDEO_INTERLACE_MODE_PROGRESSIVE:
      return 0;
    case GST_VIDEO_INTERLACE_MODE_INTERLEAVED:
      return 1;
    case GST_VIDEO_INTERLACE_MODE_MIXED:
      return 2;
    case GST_VIDEO_INTERLACE_MODE_FIELDS:
      return 3;
    case GST_VIDEO_INTERLACE_MODE_ALTERNATE:
      return 4;
    default:
      return G_MAXUINT32;
  }
}

static void ChecksumU32Be(GChecksum *checksum, guint32 value) {
  const guchar bytes[] = {
      static_cast<guchar>(value >> 24), static_cast<guchar>(value >> 16),
      static_cast<guchar>(value >> 8), static_cast<guchar>(value)};
  g_checksum_update(checksum, bytes, sizeof(bytes));
}

static void ChecksumU64Be(GChecksum *checksum, guint64 value) {
  const guchar bytes[] = {
      static_cast<guchar>(value >> 56), static_cast<guchar>(value >> 48),
      static_cast<guchar>(value >> 40), static_cast<guchar>(value >> 32),
      static_cast<guchar>(value >> 24), static_cast<guchar>(value >> 16),
      static_cast<guchar>(value >> 8), static_cast<guchar>(value)};
  g_checksum_update(checksum, bytes, sizeof(bytes));
}

static void ChecksumFrameMetadata(GChecksum *checksum, GstClockTime pts,
                                  GstClockTime duration,
                                  const GstVideoInfo &info,
                                  GstBufferFlags flags) {
  /* Stable oracle record: version, exact nanosecond PTS/duration (NONE is
   * UINT64_MAX), geometry, frame rate, canonical interlace mode and field
   * flags. */
  static constexpr guchar version[] = {'C', 'H', 'M', 'D', 2};
  g_checksum_update(checksum, version, sizeof(version));
  ChecksumU64Be(checksum, pts);
  ChecksumU64Be(checksum, duration);
  ChecksumU32Be(checksum, GST_VIDEO_INFO_WIDTH(&info));
  ChecksumU32Be(checksum, GST_VIDEO_INFO_HEIGHT(&info));
  ChecksumU32Be(checksum, GST_VIDEO_INFO_FPS_N(&info));
  ChecksumU32Be(checksum, GST_VIDEO_INFO_FPS_D(&info));
  ChecksumU32Be(checksum,
                CanonicalInterlaceMode(GST_VIDEO_INFO_INTERLACE_MODE(&info)));
  ChecksumU32Be(checksum, CanonicalFieldFlags(flags));
}

static bool FieldMetadataMatches(ExpectedField expected,
                                 GstVideoInterlaceMode mode,
                                 GstBufferFlags flags) {
  const bool interlaced = (flags & GST_VIDEO_BUFFER_FLAG_INTERLACED) != 0;
  const bool top_first = (flags & GST_VIDEO_BUFFER_FLAG_TFF) != 0;
  const bool complete_picture = (flags & GST_VIDEO_BUFFER_FLAG_ONEFIELD) == 0;
  const bool interlaced_mode = mode == GST_VIDEO_INTERLACE_MODE_INTERLEAVED ||
                               mode == GST_VIDEO_INTERLACE_MODE_MIXED;
  const bool progressive_mode = mode == GST_VIDEO_INTERLACE_MODE_PROGRESSIVE ||
                                mode == GST_VIDEO_INTERLACE_MODE_MIXED;

  if (!complete_picture || (top_first && !interlaced) ||
      (interlaced && !interlaced_mode) || (!interlaced && !progressive_mode))
    return false;
  if (expected == ExpectedField::kAny)
    return true;
  if (expected == ExpectedField::kProgressive)
    return !interlaced;
  return interlaced &&
         top_first == (expected == ExpectedField::kTopFirst);
}

static ExpectedField ExpectedFieldOrder(AVFieldOrder field_order) {
  switch (field_order) {
    case AV_FIELD_PROGRESSIVE:
      return ExpectedField::kProgressive;
    case AV_FIELD_TT:
    case AV_FIELD_BT:
      return ExpectedField::kTopFirst;
    case AV_FIELD_BB:
    case AV_FIELD_TB:
      return ExpectedField::kBottomFirst;
    case AV_FIELD_UNKNOWN:
    default:
      return ExpectedField::kAny;
  }
}

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
  ExpectedField expected_field = ExpectedField::kAny;
  GstClockTime previous_pts = GST_CLOCK_TIME_NONE;
  bool have_last_good = false;
  unsigned int last_good_frame = 0;
  GstClockTime last_good_pts = GST_CLOCK_TIME_NONE;
  std::vector<GstClockTime> pts;
  GChecksum *checksum = g_checksum_new(G_CHECKSUM_SHA256);
  GChecksum *metadata_checksum = g_checksum_new(G_CHECKSUM_SHA256);
  Phase1Progress progress{};
};

static void CountFrame(GstElement *, GstBuffer *buffer, GstPad *pad, gpointer data) {
  auto *audit = static_cast<Audit *>(data);
  GstCaps *caps = gst_pad_get_current_caps(pad);
  GstVideoInfo info;
  GstVideoFrame frame;
  const GstClockTime pts = GST_BUFFER_PTS(buffer);
  const GstBufferFlags flags =
      static_cast<GstBufferFlags>(GST_BUFFER_FLAGS(buffer));
  const bool caps_valid = caps && gst_video_info_from_caps(&info, caps);
  if (caps_valid)
    ChecksumFrameMetadata(audit->metadata_checksum, pts,
                          GST_BUFFER_DURATION(buffer), info, flags);
  bool frame_valid = caps_valid &&
      GST_VIDEO_INFO_FORMAT(&info) == GST_VIDEO_FORMAT_YUY2 &&
      GST_VIDEO_INFO_WIDTH(&info) == audit->width &&
      GST_VIDEO_INFO_HEIGHT(&info) == audit->height &&
      (!GST_CLOCK_TIME_IS_VALID(pts) ||
       !GST_CLOCK_TIME_IS_VALID(audit->previous_pts) || pts >= audit->previous_pts) &&
      FieldMetadataMatches(audit->expected_field,
                           GST_VIDEO_INFO_INTERLACE_MODE(&info), flags);
  bool mapped = false;
  if (frame_valid)
    mapped = gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ);
  if (!frame_valid || !mapped) {
    audit->invalid = true;
  } else {
    if (GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0) < audit->width * 2) {
      audit->invalid = true;
      frame_valid = false;
    } else {
      for (int row = 0; row < audit->height; ++row)
        g_checksum_update(audit->checksum,
            static_cast<const guchar *>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0)) +
                row * GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0), audit->width * 2);
    }
    gst_video_frame_unmap(&frame);
  }
  if (frame_valid && !audit->invalid) {
    audit->have_last_good = true;
    audit->last_good_frame = audit->frames;
    audit->last_good_pts = pts;
    if (GST_CLOCK_TIME_IS_VALID(pts))
      phase1_progress_write(&audit->progress,
          "probe=gstreamer-codec-playback frame-index=%u pts=%" G_GUINT64_FORMAT "\n",
          audit->last_good_frame, static_cast<guint64>(pts));
    else
      phase1_progress_write(&audit->progress,
          "probe=gstreamer-codec-playback frame-index=%u pts=NONE\n",
          audit->last_good_frame);
  }
  if (caps)
    gst_caps_unref(caps);
  if (GST_CLOCK_TIME_IS_VALID(pts))
    audit->previous_pts = pts;
  audit->pts.push_back(pts);
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
  audit.expected_field = ExpectedField::kProgressive;
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
    g_checksum_free(bad.metadata_checksum);
  }
  gst_element_set_state(pipeline, GST_STATE_NULL);
  GstVideoInfo expected_info;
  gst_video_info_init(&expected_info);
  GChecksum *expected_metadata = g_checksum_new(G_CHECKSUM_SHA256);
  GChecksum *different_metadata = g_checksum_new(G_CHECKSUM_SHA256);
  ok = ok && gst_video_info_set_format(&expected_info, GST_VIDEO_FORMAT_YUY2,
                                        32, 24);
  GST_VIDEO_INFO_FPS_N(&expected_info) = 25;
  GST_VIDEO_INFO_FPS_D(&expected_info) = 1;
  for (unsigned int i = 0; i < 12; ++i) {
    ChecksumFrameMetadata(expected_metadata, i * GST_SECOND / 25,
                          GST_SECOND / 25, expected_info,
                          static_cast<GstBufferFlags>(0));
    ChecksumFrameMetadata(different_metadata, i * GST_SECOND / 25,
                          i == 11 ? GST_SECOND / 24 : GST_SECOND / 25,
                          expected_info, static_cast<GstBufferFlags>(0));
  }
  ok = ok && eos && !audit.invalid && audit.frames == 12 &&
      audit.have_last_good && audit.last_good_frame == 11 &&
      audit.last_good_pts == 11 * GST_SECOND / 25 &&
      std::strcmp(g_checksum_get_string(audit.metadata_checksum),
                  g_checksum_get_string(expected_metadata)) == 0 &&
      std::strcmp(g_checksum_get_string(expected_metadata),
                  g_checksum_get_string(different_metadata)) != 0 &&
      CanonicalInterlaceMode(GST_VIDEO_INTERLACE_MODE_PROGRESSIVE) == 0 &&
      CanonicalInterlaceMode(GST_VIDEO_INTERLACE_MODE_INTERLEAVED) == 1 &&
      CanonicalInterlaceMode(GST_VIDEO_INTERLACE_MODE_MIXED) == 2 &&
      FieldMetadataMatches(ExpectedField::kProgressive,
                           GST_VIDEO_INTERLACE_MODE_PROGRESSIVE,
                           static_cast<GstBufferFlags>(0)) &&
      FieldMetadataMatches(ExpectedField::kTopFirst,
                           GST_VIDEO_INTERLACE_MODE_MIXED,
                           static_cast<GstBufferFlags>(
                               GST_VIDEO_BUFFER_FLAG_INTERLACED |
                               GST_VIDEO_BUFFER_FLAG_TFF)) &&
      FieldMetadataMatches(ExpectedField::kTopFirst,
                           GST_VIDEO_INTERLACE_MODE_MIXED,
                           static_cast<GstBufferFlags>(
                               GST_VIDEO_BUFFER_FLAG_INTERLACED |
                               GST_VIDEO_BUFFER_FLAG_TFF |
                               GST_VIDEO_BUFFER_FLAG_RFF)) &&
      FieldMetadataMatches(ExpectedField::kBottomFirst,
                           GST_VIDEO_INTERLACE_MODE_INTERLEAVED,
                           static_cast<GstBufferFlags>(
                               GST_VIDEO_BUFFER_FLAG_INTERLACED)) &&
      !FieldMetadataMatches(ExpectedField::kProgressive,
                            GST_VIDEO_INTERLACE_MODE_MIXED,
                            static_cast<GstBufferFlags>(
                                GST_VIDEO_BUFFER_FLAG_INTERLACED)) &&
      !FieldMetadataMatches(ExpectedField::kAny,
                            GST_VIDEO_INTERLACE_MODE_PROGRESSIVE,
                            static_cast<GstBufferFlags>(
                                GST_VIDEO_BUFFER_FLAG_TFF));
  Deadline expired;
  expired.expires = g_get_monotonic_time() - 1;
  ok = ok && DeadlineExpired(&expired) && Remaining(expired) == 0 &&
      ClockTime(AV_NOPTS_VALUE, AVRational{1, 25}) == GST_CLOCK_TIME_NONE &&
      ClockTime(1, AVRational{1, 25}) == GST_SECOND / 25;
  std::printf("Codec probe hardware-free audit self-test: %s\n", ok ? "passed" : "failed");
  if (message)
    gst_message_unref(message);
  g_checksum_free(expected_metadata);
  g_checksum_free(different_metadata);
  g_checksum_free(audit.checksum);
  g_checksum_free(audit.metadata_checksum);
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
  const bool mpeg4 = parameters->codec_id == AV_CODEC_ID_MPEG4;
  const bool mpeg4_simple = parameters->profile == FF_PROFILE_MPEG4_SIMPLE;
  const bool mpeg4_asp = parameters->profile == FF_PROFILE_MPEG4_ADVANCED_SIMPLE;
  if ((!vc1 && parameters->codec_id != AV_CODEC_ID_WMV3 && !mpeg4) ||
      (mpeg4 && ((!mpeg4_simple && !mpeg4_asp) ||
                 (parameters->level != 3 && parameters->level != 5))) ||
      (mpeg4 && parameters->field_order != AV_FIELD_UNKNOWN &&
       parameters->field_order != AV_FIELD_PROGRESSIVE) ||
      parameters->extradata_size <= 0 || parameters->extradata_size > kMaxPacketBytes ||
      !parameters->extradata || parameters->width <= 0 || parameters->width > 1920 ||
      parameters->height <= 0 || parameters->height > 1088) {
    std::fprintf(stderr, "Probe requires WMV3/VC-1 or MPEG-4 Simple/ASP level 3 or 5 with codec metadata and dimensions\n");
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
  GstCaps *caps;
  if (mpeg4) {
    caps = gst_caps_new_simple("video/mpeg", "mpegversion", G_TYPE_INT, 4,
        "systemstream", G_TYPE_BOOLEAN, FALSE, "parsed", G_TYPE_BOOLEAN, TRUE,
        "profile", G_TYPE_STRING, mpeg4_simple ? "simple" : "advanced-simple",
        "level", G_TYPE_STRING, parameters->level == 3 ? "3" : "5",
        "interlace-mode", G_TYPE_STRING, "progressive",
        "width", G_TYPE_INT, parameters->width, "height", G_TYPE_INT, parameters->height,
        nullptr);
  } else {
    caps = gst_caps_new_simple("video/x-wmv",
        "wmvversion", G_TYPE_INT, 3, "format", G_TYPE_STRING, vc1 ? "WVC1" : "WMV3",
        "stream-format", G_TYPE_STRING, "asf", "header-format", G_TYPE_STRING, "asf",
        "width", G_TYPE_INT, parameters->width, "height", G_TYPE_INT, parameters->height,
        nullptr);
  }
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
  audit.expected_field = mpeg4 ? ExpectedField::kProgressive
                               : ExpectedFieldOrder(parameters->field_order);
  g_signal_connect(sink, "handoff", G_CALLBACK(CountFrame), &audit);
  GstBus *bus = gst_element_get_bus(pipeline);
  bool ok = phase1_progress_open(&audit.progress);
  if (!ok)
    std::fprintf(stderr, "Could not open Phase 1 progress record\n");
  ok = ok && gst_element_set_state(pipeline, GST_STATE_PLAYING) !=
      GST_STATE_CHANGE_FAILURE;
  AVPacket *packet = av_packet_alloc();
  int read_result = 0;
  unsigned int packets = 0;
  std::vector<GstClockTime> expected_pts;
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
      if (mpeg4) {
        if (!GST_CLOCK_TIME_IS_VALID(GST_BUFFER_PTS(input))) {
          std::fprintf(stderr, "MPEG-4 packet has no presentation timestamp\n");
          gst_buffer_unref(input);
          ok = false;
          break;
        }
        expected_pts.push_back(GST_BUFFER_PTS(input));
      }
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
  if (gst_element_set_state(pipeline, GST_STATE_NULL) == GST_STATE_CHANGE_FAILURE) {
    std::fprintf(stderr, "Decoder teardown failed\n");
    ok = false;
  }
  if (mpeg4) {
    std::sort(expected_pts.begin(), expected_pts.end());
    if (audit.pts != expected_pts) {
      std::fprintf(stderr, "MPEG-4 output PTS do not match presentation order\n");
      ok = false;
    }
  }
  const char *codec_name = mpeg4 ? (mpeg4_simple ? "MPEG-4 Simple" : "MPEG-4 ASP")
                                  : (vc1 ? "VC-1" : "WMV3");
  std::printf("%s: %u packets; %u/%lu YUY2 frames; EOS=%s; "
              "MetadataSHA256=%s; SHA256=%s\n",
      codec_name, packets, audit.frames, expected, eos ? "yes" : "no",
      g_checksum_get_string(audit.metadata_checksum),
      g_checksum_get_string(audit.checksum));
  if (!audit.have_last_good) {
    std::printf("LastGoodFrameIndex=none; LastGoodPTS=NONE\n");
  } else if (!GST_CLOCK_TIME_IS_VALID(audit.last_good_pts)) {
    std::printf("LastGoodFrameIndex=%u; LastGoodPTS=NONE\n",
                audit.last_good_frame);
  } else {
    std::printf("LastGoodFrameIndex=%u; LastGoodPTS=%" G_GUINT64_FORMAT "\n",
                audit.last_good_frame, static_cast<guint64>(audit.last_good_pts));
  }
  if (audit.invalid)
    std::fprintf(stderr, "Invalid dimensions/YUY2 buffers, timestamps or field metadata\n");
  ok = ok && eos && !audit.invalid && audit.frames == expected;
  if (message)
    gst_message_unref(message);
  g_checksum_free(audit.checksum);
  g_checksum_free(audit.metadata_checksum);
  phase1_progress_close(&audit.progress);
  gst_object_unref(source);
  gst_object_unref(sink);
  gst_object_unref(bus);
  gst_object_unref(pipeline);
  avformat_close_input(&format);
  return ok ? 0 : 1;
}
