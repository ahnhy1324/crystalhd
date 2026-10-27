/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Actual bounded appsrc/appsink transport; no card or codec plugin loaded. */
#include "../tools/kodi-route-probe/playback.h"
#include <array>
using Decoder = crystalhd_kodi::PlaybackDecoder;

static GstSample* next(Decoder& decoder, bool final, Decoder::Result want) {
  const auto until = g_get_monotonic_time() + 3000000;
  while (true) {
    Decoder::Result result;
    GstSample* sample = decoder.Next(final, result);
    if (result == want) return sample;
    if (sample) gst_sample_unref(sample);
    g_assert_true(result == Decoder::Result::Wait || result == Decoder::Result::NeedInput);
    g_assert_cmpint(g_get_monotonic_time(), <, until);
  }
}
static void pause_lifetime() {
  Decoder decoder; g_assert_true(decoder.OpenRawTest(4, 2));
  std::array<uint8_t, 16> bytes{}; bytes.fill(73);
  g_assert_true(decoder.Push(bytes.data(), bytes.size(), 0, 0, 40000) == Decoder::Submit::Accepted);
  GstSample* held = next(decoder, false, Decoder::Result::Picture);
  g_assert_cmpuint(GST_BUFFER_PTS(gst_sample_get_buffer(held)), ==, crystalhd_kodi::clock_origin);
  g_assert_true(decoder.SetPaused(true));
  g_assert_true(decoder.Push(bytes.data(), bytes.size(), 40000, 40000, 40000) == Decoder::Submit::Retry);
  Decoder::Result result;
  g_assert_null(decoder.Next(true, result)); // even known final EOF deferred during pause
  g_assert_true(result != Decoder::Result::Eof && result != Decoder::Result::Error);
  g_assert_cmpuint(decoder.Counts().queued, ==, 1);
  g_assert_cmpuint(decoder.Counts().terminal_eos, ==, 0);
  g_assert_true(decoder.SetPaused(false)); bytes.fill(129);
  g_assert_true(decoder.Push(bytes.data(), bytes.size(), 40000, 40000, 40000) == Decoder::Submit::Accepted);
  gst_sample_unref(next(decoder, false, Decoder::Result::Picture));
  g_assert_null(next(decoder, true, Decoder::Result::Eof));
  g_assert_true(decoder.Current() == Decoder::State::Ended);
  g_assert_cmpuint(decoder.Counts().queued, ==, 2); g_assert_cmpuint(decoder.Counts().returned, ==, 2);
  g_assert_cmpuint(decoder.Counts().pauses, ==, 1); g_assert_cmpuint(decoder.Counts().resumes, ==, 1);
  g_assert_cmpuint(decoder.Counts().terminal_eos, ==, 1);
  g_assert_null(next(decoder, true, Decoder::Result::Eof));
  g_assert_cmpuint(decoder.Counts().terminal_eos, ==, 1);
  g_assert_true(decoder.Push(bytes.data(), bytes.size(), 80000, 80000, 40000) == Decoder::Submit::Error);
  decoder.Close();
  GstMapInfo map{}; g_assert_true(gst_buffer_map(gst_sample_get_buffer(held), &map, GST_MAP_READ));
  for (size_t i = 0; i < map.size; ++i) g_assert_cmpuint(map.data[i], ==, 73);
  gst_buffer_unmap(gst_sample_get_buffer(held), &map); gst_sample_unref(held);
  g_assert_true(decoder.OpenRawTest(4, 2)); g_assert_cmpuint(decoder.Counts().generation, ==, 2);
  g_assert_cmpuint(decoder.Counts().queued, ==, 0);
  g_assert_true(decoder.Push(bytes.data(), bytes.size(), 0, 0, 40000) == Decoder::Submit::Accepted);
  gst_sample_unref(next(decoder, false, Decoder::Result::Picture));
  g_assert_null(next(decoder, true, Decoder::Result::Eof)); decoder.Close();
}
static void cancel_and_error() {
  Decoder decoder; g_assert_true(decoder.OpenRawTest(4, 2));
  g_assert_true(decoder.SetPaused(true)); decoder.Close();
  Decoder::Result result; g_assert_null(decoder.Next(false, result)); g_assert_true(result == Decoder::Result::Error);
  g_assert_true(decoder.OpenRawTest(4, 2));
  g_assert_true(decoder.Push(nullptr, 0, 0, 0, 40000) == Decoder::Submit::Error);
  g_assert_null(decoder.Next(true, result)); g_assert_true(result == Decoder::Result::Error);
  g_assert_cmpuint(decoder.Counts().terminal_eos, ==, 0); decoder.Close();
}
int main(int argc, char** argv) {
  g_test_init(&argc, &argv, nullptr);
  g_test_add_func("/kodi/playback/pause-resume-source-zero-eof-owned-buffer-reopen", pause_lifetime);
  g_test_add_func("/kodi/playback/cancel-paused-and-error-never-eos", cancel_and_error);
  const int result = g_test_run(); gst_deinit(); return result;
}
