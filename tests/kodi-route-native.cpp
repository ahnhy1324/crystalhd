/* SPDX-License-Identifier: GPL-2.0-or-later */
/* No libcrystalhd, no device. Raw appsrc/appsink test hook exists in this
 * executable only, never in a shipped add-on. Native decode is a separate gate. */
#include "../tools/kodi-route-probe/native.h"
#include <array>
#include <limits>
#include <unistd.h>
#include <dirent.h>
using crystalhd_kodi::NativeDecoder;

static void rows() {
  using crystalhd_kodi::packed_layout;
  g_assert_true(packed_layout(640, 360, 0, 1280, 460800));
  g_assert_true(packed_layout(640, 360, 17, 1296, 17 + 359 * 1296 + 1280));
  g_assert_false(packed_layout(640, 360, 17, 1296, 17 + 359 * 1296 + 1279));
  g_assert_false(packed_layout(640, 360, 0, 1279, 460800));
  g_assert_false(packed_layout(641, 360, 0, 1282, 461520));
  g_assert_false(packed_layout(640, 0, 0, 1280, 460800));
  g_assert_false(packed_layout(0, 360, 0, 0, 460800));
  g_assert_false(packed_layout(1922, 1080, 0, 3844, 4151520));
  const size_t huge = std::numeric_limits<size_t>::max();
  g_assert_false(packed_layout(640, 360, huge, 1280, huge));
  g_assert_false(packed_layout(640, 360, 0, huge, huge));
  g_assert_true(packed_layout(2, 1, 5, huge, 9)); // one row need not advance pitch
}

static void timestamps() {
  uint64_t clock = 0; int64_t micros = 0;
  g_assert_true(crystalhd_kodi::to_clock(0, true, clock));
  g_assert_cmpuint(clock, ==, crystalhd_kodi::clock_origin);
  g_assert_true(crystalhd_kodi::from_clock(clock, micros)); g_assert_cmpint(micros, ==, 0);
  g_assert_true(crystalhd_kodi::to_clock(-66666, false, clock));
  g_assert_cmpuint(clock, ==, crystalhd_kodi::clock_origin - 66666000);
  g_assert_false(crystalhd_kodi::to_clock(-1, true, clock));
  g_assert_false(crystalhd_kodi::to_clock(-10000001, false, clock));
  g_assert_false(crystalhd_kodi::to_clock(std::numeric_limits<double>::infinity(), true, clock));
  g_assert_false(crystalhd_kodi::to_clock(std::numeric_limits<double>::quiet_NaN(), true, clock));
  g_assert_false(crystalhd_kodi::to_clock(1e100, true, clock));
  g_assert_false(crystalhd_kodi::from_clock(GST_CLOCK_TIME_NONE, micros));
  g_assert_false(crystalhd_kodi::from_clock(crystalhd_kodi::clock_origin - 1, micros));
  for (int64_t t : {1LL, 33333LL, 5999999LL, 86400000000LL}) {
    g_assert_true(crystalhd_kodi::to_clock(t, true, clock));
    g_assert_true(crystalhd_kodi::from_clock(clock, micros)); g_assert_cmpint(micros, ==, t);
  }
}

static unsigned fd_count() {
  unsigned n = 0; DIR* d = opendir("/proc/self/fd"); g_assert_nonnull(d);
  while (auto* e = readdir(d)) if (e->d_name[0] != '.') ++n;
  closedir(d); return n;
}

static void transport() {
  NativeDecoder decoder;
  /* Warm Gst's global worker/eventfd once; compare same live process later. */
  g_assert_true(decoder.OpenRawTest(4, 2)); decoder.Close();
  const unsigned before = fd_count();
  for (unsigned cycle = 0; cycle < 3; ++cycle) {
    g_assert_true(decoder.OpenRawTest(4, 2));
    unsigned accepted = 0, received = 0, retries = 0;
    std::array<uint8_t, 16> data{};
    const auto end = g_get_monotonic_time() + 5000000;
    /* Deliberately fill the sink and source. The caller has no output loop
     * during this phase, so admission must return Retry without blocking. */
    for (unsigned i = 0; i < 60; ++i) {
      data.fill(accepted);
      auto result = decoder.Push(data.data(), data.size(), accepted * 33333., accepted * 33333. - 66666, 33333);
      g_assert_true(result != NativeDecoder::Submit::Error);
      if (result == NativeDecoder::Submit::Accepted) ++accepted; else ++retries;
      g_assert_cmpuint(decoder.QueuedTest(), <=, 8);
    }
    g_assert_cmpuint(retries, >, 0);
    while (received < 180) {
      g_assert_cmpint(g_get_monotonic_time(), <, end);
      if (accepted < 180) {
        data.fill(accepted);
        const auto result = decoder.Push(data.data(), data.size(), accepted * 33333., accepted * 33333. - 66666, 33333);
        g_assert_true(result != NativeDecoder::Submit::Error);
        if (result == NativeDecoder::Submit::Accepted) ++accepted;
      }
      NativeDecoder::Result result;
      GstSample* sample = decoder.Next(accepted == 180, result);
      g_assert_true(result != NativeDecoder::Result::Error);
      if (!sample) continue;
      GstBuffer* b = gst_sample_get_buffer(sample); GstMapInfo map{};
      g_assert_true(gst_buffer_map(b, &map, GST_MAP_READ));
      g_assert_cmpuint(map.size, ==, data.size());
      for (unsigned i = 0; i < map.size; ++i) g_assert_cmpuint(map.data[i], ==, received);
      int64_t pts = -1; g_assert_true(crystalhd_kodi::from_clock(GST_BUFFER_PTS(b), pts));
      g_assert_cmpint(pts, ==, received * 33333LL);
      gst_buffer_unmap(b, &map); gst_sample_unref(sample); ++received;
    }
    NativeDecoder::Result result;
    do {
      g_assert_cmpint(g_get_monotonic_time(), <, end);
      GstSample* sample = decoder.Next(true, result); g_assert_null(sample);
      g_assert_true(result != NativeDecoder::Result::Error);
    } while (result != NativeDecoder::Result::Eof);
    g_assert_cmpuint(accepted, ==, received);
    decoder.Close(); g_assert_cmpuint(fd_count(), ==, before);
  }
}

static void cancel_full_queue() {
  NativeDecoder decoder; std::array<uint8_t, 16> data{};
  g_assert_true(decoder.OpenRawTest(4, 2));
  for (unsigned i = 0; i < 40; ++i)
    g_assert_true(decoder.Push(data.data(), data.size(), 0, 0, 33333) != NativeDecoder::Submit::Error);
  const auto start = g_get_monotonic_time(); decoder.Close();
  g_assert_cmpint(g_get_monotonic_time() - start, <, 2000000);
  decoder.Close(); // idempotent; no join/unref of a retired worker
}

static void invalid_packet() {
  NativeDecoder decoder; NativeDecoder::Result result;
  g_assert_true(decoder.OpenRawTest(4, 2));
  g_assert_true(decoder.Push(nullptr, 1, 0, 0, 33333) == NativeDecoder::Submit::Error);
  g_assert_null(decoder.Next(false, result)); g_assert_true(result == NativeDecoder::Result::Error);
  decoder.Close();
  g_assert_false(decoder.Open(640, 360, 30, 1, nullptr, 0));
}

int main(int argc, char** argv) {
  gst_init(&argc, &argv); g_test_init(&argc, &argv, nullptr);
  g_test_add_func("/kodi/packed/bounds", rows);
  g_test_add_func("/kodi/packed/timestamps", timestamps);
  g_test_add_func("/kodi/transport/bounded-retry-eos-reopen", transport);
  g_test_add_func("/kodi/transport/cancel-full", cancel_full_queue);
  g_test_add_func("/kodi/transport/invalid-packet", invalid_packet);
  const int result = g_test_run(); gst_deinit(); return result;
}
