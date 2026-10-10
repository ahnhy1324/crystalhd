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
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include "phase1-progress.h"

static constexpr guint64 kQueueBytes = 4 * 1024 * 1024;
static constexpr int kMaxPacketBytes = 16 * 1024 * 1024;
static constexpr gint64 kTimeoutUs = 25 * G_USEC_PER_SEC;
static constexpr unsigned int kSeekFixtureFrames = 90;
static constexpr unsigned int kSeekMinimumFrames = 12;
static constexpr std::array<GstClockTime, 4> kSeekTargets = {
    2200 * GST_MSECOND, 700 * GST_MSECOND, 1500 * GST_MSECOND, 0};

enum class ExpectedField { kAny, kProgressive, kTopFirst, kBottomFirst };

struct FrameRecord {
  GstClockTime pts = GST_CLOCK_TIME_NONE;
  guint32 segment_seqnum = GST_SEQNUM_INVALID;
  GstClockTime segment_start = GST_CLOCK_TIME_NONE;
  std::string yuy2_sha256;
};

struct EpochResult {
  GstClockTime target = GST_CLOCK_TIME_NONE;
  guint32 seek_seqnum = GST_SEQNUM_INVALID;
  std::vector<FrameRecord> output;
};

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
  std::mutex mutex;
  unsigned int frames = 0;
  int width = 0;
  int height = 0;
  bool invalid = false;
  ExpectedField expected_field = ExpectedField::kAny;
  GstClockTime previous_pts = GST_CLOCK_TIME_NONE;
  bool have_last_good = false;
  unsigned int last_good_frame = 0;
  GstClockTime last_good_pts = GST_CLOCK_TIME_NONE;
  bool strict_epoch = false;
  GstClockTime epoch_target = 0;
  guint32 seek_seqnum = GST_SEQNUM_INVALID;
  guint32 current_segment_seqnum = GST_SEQNUM_INVALID;
  GstClockTime current_segment_start = GST_CLOCK_TIME_NONE;
  guint32 pending_seek_seqnum = GST_SEQNUM_INVALID;
  GstClockTime pending_target = GST_CLOCK_TIME_NONE;
  bool transition_complete = false;
  bool transition_failed = false;
  EpochResult completed_epoch;
  std::vector<GstClockTime> pts;
  std::vector<FrameRecord> output;
  GChecksum *checksum = g_checksum_new(G_CHECKSUM_SHA256);
  GChecksum *metadata_checksum = g_checksum_new(G_CHECKSUM_SHA256);
  Phase1Progress progress{};
};

static EpochResult CopyEpoch(const Audit &audit) {
  EpochResult result;
  result.target = audit.epoch_target;
  result.seek_seqnum = audit.seek_seqnum;
  result.output = audit.output;
  return result;
}

static void ResetEpoch(Audit *audit, GstClockTime target, guint32 seek_seqnum) {
  audit->frames = 0;
  audit->invalid = false;
  audit->previous_pts = GST_CLOCK_TIME_NONE;
  audit->have_last_good = false;
  audit->last_good_frame = 0;
  audit->last_good_pts = GST_CLOCK_TIME_NONE;
  audit->pts.clear();
  audit->output.clear();
  audit->epoch_target = target;
  audit->seek_seqnum = seek_seqnum;
  audit->current_segment_seqnum = GST_SEQNUM_INVALID;
  audit->current_segment_start = GST_CLOCK_TIME_NONE;
  g_checksum_reset(audit->checksum);
  g_checksum_reset(audit->metadata_checksum);
}

static void CountFrame(GstElement *, GstBuffer *buffer, GstPad *pad, gpointer data) {
  auto *audit = static_cast<Audit *>(data);
  std::unique_lock<std::mutex> lock(audit->mutex, std::defer_lock);
  if (audit->strict_epoch)
    lock.lock();
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
  const bool epoch_valid = !audit->strict_epoch ||
      (GST_CLOCK_TIME_IS_VALID(pts) &&
       GST_CLOCK_TIME_IS_VALID(audit->current_segment_start) &&
       audit->current_segment_start >= audit->epoch_target &&
       pts >= audit->current_segment_start &&
       (!GST_CLOCK_TIME_IS_VALID(audit->previous_pts) ||
        pts > audit->previous_pts) &&
       audit->current_segment_seqnum != GST_SEQNUM_INVALID);
  if (!epoch_valid) {
    if (!audit->invalid)
      std::fprintf(stderr,
                   "Seek epoch rejected output: pts=%" G_GUINT64_FORMAT
                   " target=%" G_GUINT64_FORMAT " segment=%u"
                   " start=%" G_GUINT64_FORMAT "\n",
                   static_cast<guint64>(pts),
                   static_cast<guint64>(audit->epoch_target),
                   audit->current_segment_seqnum,
                   static_cast<guint64>(audit->current_segment_start));
    frame_valid = false;
  }
  bool mapped = false;
  GChecksum *frame_checksum = nullptr;
  if (frame_valid)
    mapped = gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ);
  if (!frame_valid || !mapped) {
    audit->invalid = true;
  } else {
    if (GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0) < audit->width * 2) {
      audit->invalid = true;
      frame_valid = false;
    } else {
      if (audit->strict_epoch)
        frame_checksum = g_checksum_new(G_CHECKSUM_SHA256);
      for (int row = 0; row < audit->height; ++row) {
        const auto *row_data =
            static_cast<const guchar *>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0)) +
            row * GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
        g_checksum_update(audit->checksum,
                          row_data, audit->width * 2);
        if (frame_checksum)
          g_checksum_update(frame_checksum, row_data, audit->width * 2);
      }
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
    if (audit->strict_epoch)
      audit->output.push_back(FrameRecord{
          pts, audit->current_segment_seqnum,
          audit->current_segment_start,
          g_checksum_get_string(frame_checksum)});
  }
  if (frame_checksum)
    g_checksum_free(frame_checksum);
  if (caps)
    gst_caps_unref(caps);
  if (GST_CLOCK_TIME_IS_VALID(pts))
    audit->previous_pts = pts;
  audit->pts.push_back(pts);
  ++audit->frames;
}

static void RecordSegment(Audit *audit, guint32 seqnum, GstClockTime start) {
  std::lock_guard<std::mutex> lock(audit->mutex);
  if (audit->pending_seek_seqnum != GST_SEQNUM_INVALID) {
    const guint32 pending_seqnum = audit->pending_seek_seqnum;
    const GstClockTime pending_target = audit->pending_target;
    audit->completed_epoch = CopyEpoch(*audit);
    audit->transition_failed = audit->invalid ||
        audit->frames < kSeekMinimumFrames ||
        audit->output.size() != audit->frames || seqnum != pending_seqnum;
    ResetEpoch(audit, pending_target, pending_seqnum);
    audit->pending_seek_seqnum = GST_SEQNUM_INVALID;
    audit->pending_target = GST_CLOCK_TIME_NONE;
    audit->transition_complete = true;
  }
  audit->current_segment_seqnum = seqnum;
  audit->current_segment_start = start;
}

static GstPadProbeReturn ObserveSegment(GstPad *, GstPadProbeInfo *info,
                                        gpointer data) {
  if (!(GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM))
    return GST_PAD_PROBE_OK;
  GstEvent *event = GST_PAD_PROBE_INFO_EVENT(info);
  if (!event || GST_EVENT_TYPE(event) != GST_EVENT_SEGMENT)
    return GST_PAD_PROBE_OK;
  const GstSegment *segment = nullptr;
  gst_event_parse_segment(event, &segment);
  if (!segment || segment->format != GST_FORMAT_TIME)
    return GST_PAD_PROBE_OK;
  RecordSegment(static_cast<Audit *>(data), gst_event_get_seqnum(event),
                segment->start);
  return GST_PAD_PROBE_OK;
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

static bool WaitForFrames(Audit *audit, unsigned int minimum, GstBus *bus,
                          Deadline *deadline, GstMessage **message) {
  do {
    *message = TerminalMessage(bus);
    if (*message || DeadlineExpired(deadline))
      return false;
    {
      std::lock_guard<std::mutex> lock(audit->mutex);
      if (audit->invalid)
        return false;
      if (audit->frames >= minimum)
        return true;
    }
    g_usleep(5000);
  } while (true);
}

static GstEvent *NewSeekEvent(GstClockTime target) {
  return gst_event_new_seek(
      1.0, GST_FORMAT_TIME,
      static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE),
      GST_SEEK_TYPE_SET, static_cast<gint64>(target),
      GST_SEEK_TYPE_NONE, -1);
}

static EpochResult SnapshotEpoch(Audit *audit) {
  std::lock_guard<std::mutex> lock(audit->mutex);
  return CopyEpoch(*audit);
}

static bool StartNextEpoch(GstElement *pipeline, Audit *audit,
                           GstClockTime target, GstBus *bus,
                           Deadline *deadline, GstMessage **message,
                           std::vector<EpochResult> *epochs) {
  GstEvent *event = NewSeekEvent(target);
  const guint32 seqnum = gst_event_get_seqnum(event);
  {
    std::lock_guard<std::mutex> lock(audit->mutex);
    if (audit->pending_seek_seqnum != GST_SEQNUM_INVALID ||
        audit->transition_complete) {
      gst_event_unref(event);
      return false;
    }
    audit->pending_seek_seqnum = seqnum;
    audit->pending_target = target;
    audit->transition_failed = false;
  }
  bool sent = gst_element_send_event(pipeline, event);
  while (sent && !DeadlineExpired(deadline)) {
    *message = TerminalMessage(bus);
    if (*message)
      break;
    {
      std::lock_guard<std::mutex> lock(audit->mutex);
      if (audit->transition_complete) {
        const bool ok = !audit->transition_failed;
        if (ok)
          epochs->push_back(std::move(audit->completed_epoch));
        audit->completed_epoch = EpochResult{};
        audit->transition_complete = false;
        audit->transition_failed = false;
        return ok;
      }
    }
    g_usleep(1000);
  }
  std::lock_guard<std::mutex> lock(audit->mutex);
  audit->pending_seek_seqnum = GST_SEQNUM_INVALID;
  audit->pending_target = GST_CLOCK_TIME_NONE;
  audit->transition_complete = false;
  audit->transition_failed = false;
  audit->completed_epoch = EpochResult{};
  return false;
}

static bool ValidateSeekEpochs(const std::vector<EpochResult> &epochs) {
  if (epochs.size() != kSeekTargets.size() + 1)
    return false;
  const EpochResult &reference = epochs.back();
  if (reference.target != 0 ||
      reference.output.size() != kSeekFixtureFrames)
    return false;
  const GstClockTime reference_start = reference.output.front().pts;
  if (!GST_CLOCK_TIME_IS_VALID(reference_start))
    return false;

  std::map<GstClockTime, std::string> reference_frames;
  for (const FrameRecord &frame : reference.output) {
    if (!GST_CLOCK_TIME_IS_VALID(frame.pts) ||
        !reference_frames.emplace(frame.pts, frame.yuy2_sha256).second)
      return false;
  }

  std::set<guint32> segment_seqnums;
  for (size_t index = 0; index < epochs.size(); ++index) {
    const EpochResult &epoch = epochs[index];
    const GstClockTime expected_target = index == 0
        ? 0 : kSeekTargets[index - 1];
    const guint32 segment_seqnum = epoch.output.empty()
        ? GST_SEQNUM_INVALID : epoch.output.front().segment_seqnum;
    if (epoch.target > G_MAXUINT64 - reference_start)
      return false;
    auto expected_frame = reference_frames.lower_bound(
        reference_start + epoch.target);
    if (epoch.target != expected_target || epoch.output.empty() ||
        !GST_CLOCK_TIME_IS_VALID(epoch.output.front().segment_start) ||
        epoch.output.front().segment_start < epoch.target ||
        epoch.output.front().pts != epoch.output.front().segment_start ||
        expected_frame == reference_frames.end() ||
        epoch.output.front().pts != expected_frame->first ||
        (index + 1 != epochs.size() &&
         epoch.output.size() < kSeekMinimumFrames) ||
        segment_seqnum == GST_SEQNUM_INVALID ||
        !segment_seqnums.insert(segment_seqnum).second)
      return false;
    if (index != 0 &&
        (epoch.seek_seqnum == GST_SEQNUM_INVALID ||
         segment_seqnum != epoch.seek_seqnum))
      return false;
    GstClockTime previous = GST_CLOCK_TIME_NONE;
    for (const FrameRecord &frame : epoch.output) {
      if (!GST_CLOCK_TIME_IS_VALID(frame.pts) || frame.pts < epoch.target ||
          (GST_CLOCK_TIME_IS_VALID(previous) && frame.pts <= previous) ||
          frame.segment_seqnum != segment_seqnum ||
          frame.segment_start != epoch.output.front().segment_start ||
          expected_frame == reference_frames.end() ||
          expected_frame->first != frame.pts ||
          expected_frame->second != frame.yuy2_sha256)
        return false;
      previous = frame.pts;
      ++expected_frame;
    }
  }
  return true;
}

static bool RejectCorruptSeekEpochs(const std::vector<EpochResult> &epochs) {
  if (epochs.size() != kSeekTargets.size() + 1 ||
      epochs[0].output.size() < 3 ||
      epochs[1].output.empty() || epochs[2].output.empty())
    return false;
  auto corrupt = epochs;
  corrupt[0].output[1] = corrupt[0].output[0];
  if (ValidateSeekEpochs(corrupt))
    return false;
  corrupt = epochs;
  corrupt[1].output[0].yuy2_sha256[0] =
      corrupt[1].output[0].yuy2_sha256[0] == '0' ? '1' : '0';
  if (ValidateSeekEpochs(corrupt))
    return false;
  corrupt = epochs;
  const guint32 mismatched_seqnum = gst_util_seqnum_next();
  for (FrameRecord &frame : corrupt[1].output)
    frame.segment_seqnum = mismatched_seqnum;
  if (ValidateSeekEpochs(corrupt))
    return false;
  corrupt = epochs;
  const guint32 repeated_seqnum = corrupt[0].output.front().segment_seqnum;
  for (FrameRecord &frame : corrupt[1].output)
    frame.segment_seqnum = repeated_seqnum;
  if (ValidateSeekEpochs(corrupt))
    return false;
  corrupt = epochs;
  const GstClockTime old_last = corrupt[1].output.back().pts;
  auto next = std::find_if(epochs.back().output.begin(),
                           epochs.back().output.end(),
                           [old_last](const FrameRecord &frame) {
                             return frame.pts > old_last;
                           });
  if (next == epochs.back().output.end())
    return false;
  corrupt[1].output.erase(corrupt[1].output.begin());
  corrupt[1].output.push_back(*next);
  const GstClockTime late_start = corrupt[1].output.front().pts;
  for (FrameRecord &frame : corrupt[1].output) {
    frame.segment_seqnum = corrupt[1].seek_seqnum;
    frame.segment_start = late_start;
  }
  return !ValidateSeekEpochs(corrupt);
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

static bool SeekSchedulerSelfTest() {
  std::vector<EpochResult> epochs;
  for (size_t index = 0; index <= kSeekTargets.size(); ++index) {
    EpochResult epoch;
    epoch.target = index == 0 ? 0 : kSeekTargets[index - 1];
    guint32 segment_seqnum;
    if (index == 0) {
      segment_seqnum = gst_util_seqnum_next();
    } else {
      GstEvent *event = NewSeekEvent(epoch.target);
      gdouble rate;
      GstFormat format;
      GstSeekFlags flags;
      GstSeekType start_type, stop_type;
      gint64 start, stop;
      gst_event_parse_seek(event, &rate, &format, &flags, &start_type, &start,
                           &stop_type, &stop);
      const bool event_valid = rate == 1.0 && format == GST_FORMAT_TIME &&
          flags == (GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE) &&
          start_type == GST_SEEK_TYPE_SET &&
          start == static_cast<gint64>(epoch.target) &&
          stop_type == GST_SEEK_TYPE_NONE && stop == -1;
      epoch.seek_seqnum = gst_event_get_seqnum(event);
      segment_seqnum = epoch.seek_seqnum;
      gst_event_unref(event);
      if (!event_valid)
        return false;
    }
    /* qtdemux moves this fixture's negative initial DTS onto a timeline one
     * frame later. Model that offset so the test distinguishes requested seek
     * targets from the first PTS in each resulting segment. */
    const unsigned int first = 1 + static_cast<unsigned int>(
        gst_util_uint64_scale(epoch.target, 30, GST_SECOND));
    const unsigned int count = index == kSeekTargets.size()
        ? kSeekFixtureFrames : kSeekMinimumFrames;
    for (unsigned int frame = first; frame < first + count; ++frame)
      epoch.output.push_back(FrameRecord{
          gst_util_uint64_scale(frame, GST_SECOND, 30),
          segment_seqnum,
          gst_util_uint64_scale(first, GST_SECOND, 30),
          std::to_string(frame)});
    epochs.push_back(std::move(epoch));
  }
  Audit transition;
  transition.strict_epoch = true;
  transition.epoch_target = epochs[0].target;
  transition.seek_seqnum = epochs[0].seek_seqnum;
  transition.frames = epochs[0].output.size();
  transition.output = epochs[0].output;
  transition.pending_seek_seqnum = epochs[1].seek_seqnum;
  transition.pending_target = epochs[1].target;
  RecordSegment(&transition, epochs[1].seek_seqnum,
                epochs[1].output.front().segment_start);
  const bool transition_valid = transition.transition_complete &&
      !transition.transition_failed &&
      transition.completed_epoch.target == epochs[0].target &&
      transition.completed_epoch.output.size() == epochs[0].output.size() &&
      transition.frames == 0 && transition.output.empty() &&
      transition.epoch_target == epochs[1].target &&
      transition.seek_seqnum == epochs[1].seek_seqnum &&
      transition.current_segment_seqnum == epochs[1].seek_seqnum &&
      transition.current_segment_start ==
          epochs[1].output.front().segment_start &&
      transition.pending_seek_seqnum == GST_SEQNUM_INVALID;
  g_checksum_free(transition.checksum);
  g_checksum_free(transition.metadata_checksum);
  return transition_valid && ValidateSeekEpochs(epochs) &&
      RejectCorruptSeekEpochs(epochs);
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
      ClockTime(1, AVRational{1, 25}) == GST_SECOND / 25 &&
      SeekSchedulerSelfTest();
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
  const bool seek_mode = argc == 4 && std::strcmp(argv[3], "--seek") == 0;
  const unsigned long expected = (argc == 3 || seek_mode)
      ? std::strtoul(argv[2], &end, 10) : 0;
  if ((argc != 3 && !seek_mode) || !expected || errno || !end || *end ||
      expected > std::numeric_limits<unsigned int>::max()) {
    std::fprintf(stderr,
                 "usage: %s VIDEO EXPECTED_FRAMES [--seek] | --self-test\n",
                 argv[0]);
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
  AVRational rate = av_guess_frame_rate(format, stream, nullptr);
  if (seek_mode &&
      (!mpeg4_asp || expected != kSeekFixtureFrames ||
       rate.num != 30 || rate.den != 1)) {
    std::fprintf(stderr,
                 "--seek requires the 90-picture, 30-fps MPEG-4 ASP fixture\n");
    avformat_close_input(&format);
    return 2;
  }
  GError *error = nullptr;
  const char *description = seek_mode
      ? "filesrc name=source ! qtdemux ! mpeg4videoparse ! crystalhddec ! "
        "fakesink name=sink sync=true qos=false max-lateness=-1 "
        "enable-last-sample=false signal-handoffs=true"
      : "appsrc name=source format=time is-live=false ! crystalhddec ! "
        "fakesink name=sink sync=false enable-last-sample=false "
        "signal-handoffs=true";
  GstElement *pipeline = gst_parse_launch(description, &error);
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
  if (seek_mode) {
    g_object_set(source, "location", argv[1], nullptr);
  } else {
    g_object_set(source, "max-bytes", kQueueBytes, "block", FALSE, nullptr);
    GstCaps *caps;
    if (mpeg4) {
      caps = gst_caps_new_simple("video/mpeg", "mpegversion", G_TYPE_INT, 4,
          "systemstream", G_TYPE_BOOLEAN, FALSE, "parsed", G_TYPE_BOOLEAN, TRUE,
          "profile", G_TYPE_STRING, mpeg4_simple ? "simple" : "advanced-simple",
          "level", G_TYPE_STRING, parameters->level == 3 ? "3" : "5",
          "interlace-mode", G_TYPE_STRING, "progressive",
          "width", G_TYPE_INT, parameters->width,
          "height", G_TYPE_INT, parameters->height, nullptr);
    } else {
      caps = gst_caps_new_simple("video/x-wmv",
          "wmvversion", G_TYPE_INT, 3,
          "format", G_TYPE_STRING, vc1 ? "WVC1" : "WMV3",
          "stream-format", G_TYPE_STRING, "asf",
          "header-format", G_TYPE_STRING, "asf",
          "width", G_TYPE_INT, parameters->width,
          "height", G_TYPE_INT, parameters->height, nullptr);
    }
    if (rate.num > 0 && rate.den > 0)
      gst_caps_set_simple(caps, "framerate", GST_TYPE_FRACTION,
                          rate.num, rate.den, nullptr);
    GstBuffer *metadata = gst_buffer_new_allocate(
        nullptr, parameters->extradata_size, nullptr);
    gst_buffer_fill(metadata, 0, parameters->extradata,
                    parameters->extradata_size);
    gst_caps_set_simple(caps, "codec_data", GST_TYPE_BUFFER, metadata, nullptr);
    gst_buffer_unref(metadata);
    gst_app_src_set_caps(GST_APP_SRC(source), caps);
    gst_caps_unref(caps);
  }
  Audit audit;
  audit.width = parameters->width;
  audit.height = parameters->height;
  audit.expected_field = mpeg4 ? ExpectedField::kProgressive
                               : ExpectedFieldOrder(parameters->field_order);
  audit.strict_epoch = seek_mode;
  audit.epoch_target = 0;
  g_signal_connect(sink, "handoff", G_CALLBACK(CountFrame), &audit);
  GstPad *sink_pad = nullptr;
  if (seek_mode) {
    sink_pad = gst_element_get_static_pad(sink, "sink");
    gst_pad_add_probe(sink_pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
                      ObserveSegment, &audit, nullptr);
  }
  GstBus *bus = gst_element_get_bus(pipeline);
  bool ok = phase1_progress_open(&audit.progress);
  if (!ok)
    std::fprintf(stderr, "Could not open Phase 1 progress record\n");
  ok = ok && gst_element_set_state(pipeline, GST_STATE_PLAYING) !=
      GST_STATE_CHANGE_FAILURE;
  unsigned int packets = 0;
  std::vector<EpochResult> seek_epochs;
  std::vector<GstClockTime> expected_pts;
  GstMessage *message = nullptr;
  if (seek_mode) {
    for (GstClockTime target : kSeekTargets) {
      if (ok)
        ok = WaitForFrames(&audit, kSeekMinimumFrames, bus, &deadline,
                           &message);
      if (ok)
        ok = StartNextEpoch(pipeline, &audit, target, bus, &deadline,
                            &message, &seek_epochs);
    }
  } else {
    AVPacket *packet = av_packet_alloc();
    int read_result = 0;
    if (!packet)
      ok = false;
    while (ok && !DeadlineExpired(&deadline) &&
           (read_result = av_read_frame(format, packet)) >= 0) {
      if (packet->stream_index == index) {
        if (packet->size <= 0 || packet->size > kMaxPacketBytes ||
            !packet->data) {
          std::fprintf(stderr,
                       "Invalid/oversized compressed packet: %d bytes\n",
                       packet->size);
          ok = false;
          break;
        }
        if (!WaitForQueue(source, bus, &deadline, &message)) {
          ok = false;
          break;
        }
        GstBuffer *input = gst_buffer_new_allocate(
            nullptr, packet->size, nullptr);
        if (!input) {
          ok = false;
          break;
        }
        gst_buffer_fill(input, 0, packet->data, packet->size);
        GST_BUFFER_PTS(input) = ClockTime(packet->pts, stream->time_base);
        GST_BUFFER_DTS(input) = ClockTime(packet->dts, stream->time_base);
        GST_BUFFER_DURATION(input) = packet->duration > 0
            ? ClockTime(packet->duration, stream->time_base)
            : GST_CLOCK_TIME_NONE;
        if (!(packet->flags & AV_PKT_FLAG_KEY))
          GST_BUFFER_FLAG_SET(input, GST_BUFFER_FLAG_DELTA_UNIT);
        if (mpeg4) {
          if (!GST_CLOCK_TIME_IS_VALID(GST_BUFFER_PTS(input))) {
            std::fprintf(stderr,
                         "MPEG-4 packet has no presentation timestamp\n");
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
  }
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
  if (seek_mode && eos)
    seek_epochs.push_back(SnapshotEpoch(&audit));
  if (gst_element_set_state(pipeline, GST_STATE_NULL) == GST_STATE_CHANGE_FAILURE) {
    std::fprintf(stderr, "Decoder teardown failed\n");
    ok = false;
  }
  if (mpeg4 && !seek_mode) {
    std::sort(expected_pts.begin(), expected_pts.end());
    if (audit.pts != expected_pts) {
      std::fprintf(stderr, "MPEG-4 output PTS do not match presentation order\n");
      ok = false;
    }
  }
  const bool seek_epochs_valid = !seek_mode || ValidateSeekEpochs(seek_epochs);
  if (!seek_epochs_valid) {
    std::fprintf(stderr,
                 "MPEG-4 seek epochs contain stale, duplicate, mismatched or incorrectly segmented output\n");
    ok = false;
  }
  const char *codec_name = mpeg4 ? (mpeg4_simple ? "MPEG-4 Simple" : "MPEG-4 ASP")
                                  : (vc1 ? "VC-1" : "WMV3");
  if (seek_mode) {
    std::printf("%s: demux-owned seek; %u/%lu YUY2 frames; EOS=%s; "
                "MetadataSHA256=%s; SHA256=%s\n",
        codec_name, audit.frames, expected, eos ? "yes" : "no",
        g_checksum_get_string(audit.metadata_checksum),
        g_checksum_get_string(audit.checksum));
  } else {
    std::printf("%s: %u packets; %u/%lu YUY2 frames; EOS=%s; "
                "MetadataSHA256=%s; SHA256=%s\n",
        codec_name, packets, audit.frames, expected, eos ? "yes" : "no",
        g_checksum_get_string(audit.metadata_checksum),
        g_checksum_get_string(audit.checksum));
  }
  if (seek_mode && seek_epochs_valid) {
    std::printf("Seek epochs: initial=%zu, 2.2s=%zu, 0.7s=%zu, "
                "1.5s=%zu, final-0s=%zu frames; "
                "new-segment-seqnum=yes; stale=0; duplicate=0; "
                "YUY2-reference=matched\n",
                seek_epochs[0].output.size(), seek_epochs[1].output.size(),
                seek_epochs[2].output.size(), seek_epochs[3].output.size(),
                seek_epochs[4].output.size());
  }
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
  if (sink_pad)
    gst_object_unref(sink_pad);
  gst_object_unref(bus);
  gst_object_unref(pipeline);
  avformat_close_input(&format);
  return ok ? 0 : 1;
}
