/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <limits>

namespace crystalhd_kodi {
constexpr size_t packet_limit = 4 * 1024 * 1024;
/* GStreamer clock times are unsigned. Preserve negative MP4 decode timestamps
 * with a reversible origin, not a playback delay or a repaired media PTS. */
constexpr uint64_t clock_origin = 10ULL * 1000 * 1000 * 1000;

inline bool packed_layout(unsigned width, unsigned height, size_t offset,
                          size_t pitch, size_t size) {
  if (!width || (width & 1) || width > 1920 || !height || height > 1088)
    return false;
  const size_t row = static_cast<size_t>(width) * 2;
  return pitch >= row && offset <= size && row <= size - offset &&
         (height == 1 || pitch <= (size - offset - row) / (height - 1));
}

inline bool to_clock(double micros, bool presentation, uint64_t& clock) {
  const double low = presentation ? 0.0 : -10000000.0;
  /* An explicit bounded local-file prototype; do not round an overflowing
   * double into an integer, or reinterpret a missing timestamp as zero. */
  if (!std::isfinite(micros) || micros < low || micros > 86400000000.0)
    return false;
  const int64_t nanos = static_cast<int64_t>(micros * 1000.0);
  clock = static_cast<uint64_t>(nanos + static_cast<int64_t>(clock_origin));
  return true;
}

inline bool from_clock(uint64_t clock, int64_t& micros) {
  if (clock < clock_origin || clock == std::numeric_limits<uint64_t>::max() ||
      clock - clock_origin > 86400000000000ULL) return false;
  micros = static_cast<int64_t>((clock - clock_origin) / 1000);
  return true;
}
}
