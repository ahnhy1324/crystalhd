// SPDX-License-Identifier: LGPL-2.1-or-later
#ifndef CRYSTALHD_CENTERED_OUTPUT_H
#define CRYSTALHD_CENTERED_OUTPUT_H

#include <stdint.h>

#include <limits>
#include <cstring>

namespace crystalhd_centered_output {

struct Config {
  bool enabled = false;
  bool full_range = false;
  unsigned capture_width = 0;
  unsigned capture_height = 0;
  unsigned visible_width = 0;
  unsigned visible_height = 0;
};

struct Geometry {
  bool enabled = false;
  bool full_range = false;
  unsigned canvas_width = 0;
  unsigned canvas_height = 0;
  unsigned visible_width = 0;
  unsigned visible_height = 0;
  unsigned capture_width = 0;
  unsigned capture_height = 0;
  unsigned x = 0;
  unsigned y = 0;
};

namespace detail {

inline bool ParsePositiveUnsigned(const char **cursor, unsigned *result) {
  const char *text = *cursor;
  if (*text < '0' || *text > '9') return false;
  unsigned value = 0;
  const unsigned maximum = std::numeric_limits<unsigned>::max();
  do {
    const unsigned digit = static_cast<unsigned>(*text - '0');
    if (value > (maximum - digit) / 10U) return false;
    value = value * 10U + digit;
    ++text;
  } while (*text >= '0' && *text <= '9');
  if (value == 0) return false;
  *cursor = text;
  *result = value;
  return true;
}

inline bool Consume(const char **cursor, char delimiter) {
  if (**cursor != delimiter) return false;
  ++*cursor;
  return true;
}

}  // namespace detail

// The visible rectangle is an explicit policy, not a crop inferred from the
// coded dimensions. Parsing only establishes syntax; BuildGeometry validates
// that policy against the actual coded picture and its crop units.
inline bool ParseConfig(const char *text, Config *output) {
  if (output == nullptr) return false;
  *output = Config();
  if (text == nullptr || std::strcmp(text, "0") == 0) return true;

  Config parsed;
  if (!detail::ParsePositiveUnsigned(&text, &parsed.capture_width) ||
      !detail::Consume(&text, 'x') ||
      !detail::ParsePositiveUnsigned(&text, &parsed.capture_height) ||
      !detail::Consume(&text, '@') ||
      !detail::ParsePositiveUnsigned(&text, &parsed.visible_width) ||
      !detail::Consume(&text, 'x') ||
      !detail::ParsePositiveUnsigned(&text, &parsed.visible_height) ||
      !detail::Consume(&text, ':'))
    return false;
  if (std::strcmp(text, "full") == 0) {
    parsed.full_range = true;
  } else if (std::strcmp(text, "limited") != 0) {
    return false;
  }
  parsed.enabled = true;
  *output = parsed;
  return true;
}

inline bool BuildGeometry(const Config &config, unsigned coded_width,
                          unsigned coded_height, unsigned crop_unit_y,
                          Geometry *output) {
  if (output == nullptr) return false;
  *output = Geometry();
  if (!config.enabled) return true;

  if (coded_width == 0 || coded_width > 1920U || coded_width % 16U != 0 ||
      coded_height == 0 || coded_height > 1088U || coded_height % 16U != 0 ||
      (crop_unit_y != 2U && crop_unit_y != 4U) ||
      config.visible_width != coded_width || config.visible_height == 0 ||
      config.visible_height > coded_height ||
      (config.visible_height & 1U) != 0)
    return false;
  const unsigned bottom_crop = coded_height - config.visible_height;
  if (bottom_crop >= 16U || bottom_crop % crop_unit_y != 0 ||
      config.capture_width < 128U ||
      config.capture_width >= config.visible_width ||
      config.capture_width % 16U != 0 || config.capture_height < 4U ||
      config.capture_height > config.visible_height ||
      config.capture_height % 4U != 0 ||
      static_cast<uint64_t>(config.capture_width) * config.visible_height !=
          static_cast<uint64_t>(config.capture_height) * config.visible_width)
    return false;

  const unsigned x = (config.visible_width - config.capture_width) / 2U;
  const unsigned y = ((config.visible_height - config.capture_height) / 2U) & ~3U;
  if ((x & 1U) != 0 || x + config.capture_width > coded_width ||
      y + config.capture_height > coded_height)
    return false;

  Geometry geometry;
  geometry.enabled = true;
  geometry.full_range = config.full_range;
  geometry.canvas_width = coded_width;
  geometry.canvas_height = coded_height;
  geometry.visible_width = config.visible_width;
  geometry.visible_height = config.visible_height;
  geometry.capture_width = config.capture_width;
  geometry.capture_height = config.capture_height;
  geometry.x = x;
  geometry.y = y;
  *output = geometry;
  return true;
}

}  // namespace crystalhd_centered_output

#endif  // CRYSTALHD_CENTERED_OUTPUT_H
