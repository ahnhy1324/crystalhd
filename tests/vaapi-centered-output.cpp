// SPDX-License-Identifier: LGPL-2.1-or-later
// Standalone C++11 policy checks: no graphics, decoder, or SIMD dependencies.
#include "../filters/vaapi/crystalhd-centered-output.h"

#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>

namespace centered = crystalhd_centered_output;

namespace {

void Require(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

bool IsDisabled(const centered::Config &config) {
  return !config.enabled && !config.full_range && config.capture_width == 0 &&
      config.capture_height == 0 && config.visible_width == 0 &&
      config.visible_height == 0;
}

bool IsDisabled(const centered::Geometry &geometry) {
  return !geometry.enabled && !geometry.full_range && geometry.canvas_width == 0 &&
      geometry.canvas_height == 0 && geometry.visible_width == 0 &&
      geometry.visible_height == 0 && geometry.capture_width == 0 &&
      geometry.capture_height == 0 && geometry.x == 0 && geometry.y == 0;
}

centered::Config Parse(const char *text) {
  centered::Config config;
  Require(centered::ParseConfig(text, &config), "valid configuration parses");
  return config;
}

void Parsing() {
  centered::Config config = Parse("1280x720@1920x1080:full");
  Require(config.enabled && config.full_range && config.capture_width == 1280 &&
              config.capture_height == 720 && config.visible_width == 1920 &&
              config.visible_height == 1080,
          "full range and all explicit dimensions survive parsing");
  Require(centered::ParseConfig(nullptr, &config) && IsDisabled(config),
          "absent configuration disables and resets policy");
  config = Parse("1280x720@1920x1080:full");
  Require(centered::ParseConfig("0", &config) && IsDisabled(config),
          "exact zero disables and resets policy");
  Require(!centered::ParseConfig(nullptr, nullptr) &&
              !centered::ParseConfig("0", nullptr) &&
              !centered::ParseConfig("1280x720@1920x1080:limited", nullptr),
          "a missing output is always rejected");
  config = Parse("001280x00720@001920x01080:limited");
  Require(config.enabled && !config.full_range && config.capture_width == 1280 &&
              config.capture_height == 720 && config.visible_width == 1920 &&
              config.visible_height == 1080,
          "leading zeros remain decimal and limited range is explicit");

  const char *const malformed[] = {
      "", "00", "1", "false", " 0", "0 ", "0\n", "\t0", "0:full",
      "1280x720", "1280x720@1920x1080", "1280x720@1920x1080:",
      "1280x720@1920x1080:Limited", "1280x720@1920x1080:FULL",
      "1280x720@1920x1080:limited ", "1280x720@1920x1080:full\n",
      "1280x720@1920x1080:full:limited", "1280x720@1920x1080:fullx",
      "1280X720@1920x1080:limited", "1280x720@1920X1080:limited",
      "1280x720x1920x1080:limited", "1280x720@1920x1080@limited",
      "0x720@1920x1080:limited", "1280x0@1920x1080:limited",
      "1280x720@0x1080:limited", "1280x720@1920x0:limited",
      "+1280x720@1920x1080:limited", "-1280x720@1920x1080:limited",
      "1280x+720@1920x1080:limited", "1280x-720@1920x1080:limited",
      "1280x720@+1920x1080:limited", "1280x720@-1920x1080:limited",
      "1280x720@1920x+1080:limited", "1280x720@1920x-1080:limited",
      " 1280x720@1920x1080:limited", "1280 x720@1920x1080:limited",
      "1280x 720@1920x1080:limited", "1280x720 @1920x1080:limited",
      "1280x720@ 1920x1080:limited", "1280x720@1920 x1080:limited",
      "1280x720@1920x 1080:limited", "1280x720@1920x1080 :limited",
      "1280x720@1920x1080: limited", "x720@1920x1080:limited",
      "1280x@1920x1080:limited", "1280x720@x1080:limited",
      "1280x720@1920x:limited", "0x500x720@1920x1080:limited"
  };
  for (const char *text : malformed) {
    config = Parse("1280x720@1920x1080:full");
    Require(!centered::ParseConfig(text, &config) && IsDisabled(config),
            "malformed configuration is rejected and clears old policy");
  }

  const std::string maximum = std::to_string(std::numeric_limits<unsigned>::max());
  const std::string overflow = maximum + "0";
  const std::string overflow_fields[] = {
      overflow + "x720@1920x1080:limited",
      "1280x" + overflow + "@1920x1080:limited",
      "1280x720@" + overflow + "x1080:limited",
      "1280x720@1920x" + overflow + ":limited",
      "999999999999999999999999999999999999999x720@1920x1080:full"
  };
  for (const std::string &text : overflow_fields) {
    config = Parse("1280x720@1920x1080:full");
    Require(!centered::ParseConfig(text.c_str(), &config) && IsDisabled(config),
            "every overflowing numeric field is rejected without stale policy");
  }
  config = Parse((maximum + "x" + maximum + "@" + maximum + "x" + maximum +
                  ":full").c_str());
  Require(config.capture_width == std::numeric_limits<unsigned>::max() &&
              config.capture_height == std::numeric_limits<unsigned>::max() &&
              config.visible_width == std::numeric_limits<unsigned>::max() &&
              config.visible_height == std::numeric_limits<unsigned>::max(),
          "parser accepts the exact unsigned boundary before geometry validation");
}

void Geometry() {
  centered::Geometry geometry;
  const centered::Config explicit_1080 = Parse("1280x720@1920x1080:limited");
  Require(centered::BuildGeometry(explicit_1080, 1920, 1088, 4, &geometry),
          "explicit 1080 policy accepts valid bottom crop from coded 1088");
  Require(geometry.enabled && !geometry.full_range && geometry.canvas_width == 1920 &&
              geometry.canvas_height == 1088 && geometry.visible_width == 1920 &&
              geometry.visible_height == 1080 && geometry.capture_width == 1280 &&
              geometry.capture_height == 720 && geometry.x == 320 && geometry.y == 180,
          "coded canvas stays 1088 while ROI centers within explicit visible 1080");
  Require(centered::BuildGeometry(explicit_1080, 1920, 1088, 2, &geometry),
          "progressive crop units also accept explicit 1080 policy");
  Require(!centered::BuildGeometry(Parse("1280x720@1920x1088:limited"),
                                  1920, 1088, 4, &geometry) && IsDisabled(geometry),
          "720 capture cannot silently infer a visible 1080 crop from coded 1088");
  Require(centered::BuildGeometry(Parse("960x544@1920x1088:full"),
                                 1920, 1088, 4, &geometry) &&
              geometry.full_range && geometry.canvas_width == 1920 &&
              geometry.canvas_height == 1088 && geometry.visible_height == 1088 &&
              geometry.x == 480 && geometry.y == 272,
          "coded-only 960x544 policy keeps exact aspect and explicit full range");

  Require(centered::BuildGeometry(Parse("128x4@512x16:limited"),
                                 512, 16, 2, &geometry) && geometry.x == 192 &&
              geometry.y == 4 && geometry.y % 4 == 0,
          "vertical placement rounds center down to a four-line chroma phase");
  Require(centered::BuildGeometry(Parse("128x8@256x16:full"),
                                 256, 16, 4, &geometry) && geometry.x == 64 &&
              geometry.y == 4 && geometry.capture_width == 128 &&
              geometry.capture_height == 8,
          "minimum legal capture width and a small scalar-only geometry work");
  Require(centered::BuildGeometry(Parse("128x4@576x18:limited"),
                                 576, 32, 2, &geometry) && geometry.y == 4,
          "fourteen-line bottom crop is legal for a two-line crop unit");
  Require(!centered::BuildGeometry(Parse("128x4@576x18:limited"),
                                  576, 32, 4, &geometry),
          "fourteen-line bottom crop is not legal for a four-line crop unit");

  struct Invalid {
    const char *policy;
    unsigned width;
    unsigned height;
    unsigned crop_unit;
  };
  const Invalid invalid[] = {
      {"1280x720@1920x1080:limited", 0, 1088, 4},
      {"1280x720@1920x1080:limited", 1920, 0, 4},
      {"1280x720@1920x1080:limited", 1936, 1088, 4},
      {"1280x720@1920x1080:limited", 1920, 1104, 4},
      {"1280x720@1920x1080:limited", 1918, 1088, 4},
      {"1280x720@1920x1080:limited", 1920, 1080, 4},
      {"1280x720@1920x1080:limited", 1920, 1088, 0},
      {"1280x720@1920x1080:limited", 1920, 1088, 1},
      {"1280x720@1920x1080:limited", 1920, 1088, 3},
      {"1280x720@1920x1080:limited", 1920, 1088, 8},
      {"1280x720@1904x1080:limited", 1920, 1088, 4},
      {"1280x720@1920x1096:limited", 1920, 1088, 4},
      {"1280x720@1920x1072:limited", 1920, 1088, 4},
      {"1280x720@1920x1079:limited", 1920, 1088, 2},
      {"1280x720@1920x1082:limited", 1920, 1088, 4},
      {"112x4@448x16:limited", 448, 16, 4},
      {"128x16@128x16:limited", 128, 16, 4},
      {"256x16@128x16:limited", 128, 16, 4},
      {"136x8@272x16:limited", 272, 16, 4},
      {"128x2@1024x16:limited", 1024, 16, 4},
      {"128x20@256x16:limited", 256, 16, 4},
      {"128x6@512x24:limited", 512, 32, 4},
      {"128x4@256x16:limited", 256, 16, 4}
  };
  for (const Invalid &input : invalid) {
    Require(centered::BuildGeometry(explicit_1080, 1920, 1088, 4, &geometry),
            "prime an enabled geometry before rejection");
    Require(!centered::BuildGeometry(Parse(input.policy), input.width, input.height,
                                    input.crop_unit, &geometry) && IsDisabled(geometry),
            "invalid geometry is rejected and resets previously enabled output");
  }
  centered::Config manual = explicit_1080;
  manual.visible_height = 0;
  Require(!centered::BuildGeometry(manual, 1920, 1088, 4, &geometry),
          "manually constructed zero visible height is rejected");
  manual = explicit_1080;
  manual.capture_width = std::numeric_limits<unsigned>::max();
  manual.capture_height = std::numeric_limits<unsigned>::max();
  Require(!centered::BuildGeometry(manual, 1920, 1088, 4, &geometry),
          "manually constructed extreme capture dimensions cannot wrap ROI math");
  Require(!centered::BuildGeometry(explicit_1080, 1920, 1088, 4, nullptr),
          "missing geometry output is rejected");
  manual = centered::Config();
  manual.full_range = true;
  manual.capture_width = std::numeric_limits<unsigned>::max();
  Require(centered::BuildGeometry(manual, 0, 0, 0, &geometry) && IsDisabled(geometry),
          "disabled policy skips picture validation and clears all geometry");
  Require(!centered::BuildGeometry(manual, 0, 0, 0, nullptr),
          "disabled policy still requires an output");
}

}  // namespace

int main() {
  Parsing();
  Geometry();
  std::puts("vaapi-centered-output: PASS");
  return 0;
}
