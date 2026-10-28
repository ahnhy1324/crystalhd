/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../filters/crystalhd-mpeg2-inspect.h"

#include <stdio.h>

static unsigned int checks;
static unsigned int failures;

#define REQUIRE(condition, description) do { \
  checks++; \
  if (!(condition)) { \
    fprintf(stderr, "FAIL: %s\n", description); \
    failures++; \
  } \
} while (0)

static void
test_start_codes(void)
{
  const uint8_t bytes[] = { 0xff, 0, 0, 1, 0xb3 };
  size_t size;

  for (size = 0; size < 4; size++)
    REQUIRE(crystalhd_mpeg2_next_start_code(bytes, size, 0) == size,
            "short input has no complete start code");
  REQUIRE(crystalhd_mpeg2_next_start_code(bytes, sizeof(bytes), 0) == 1,
          "start-code offset is found");
  REQUIRE(crystalhd_mpeg2_next_start_code(bytes, sizeof(bytes), 2) == sizeof(bytes),
          "search origin is respected");
}

static void
test_slice_prediction(void)
{
  static const struct {
    unsigned int type;
    unsigned int code;
    unsigned int length;
    unsigned int prediction;
  } cases[] = {
    { 2, 1, 1, 1 }, { 2, 1, 2, 1 }, { 2, 1, 3, 1 },
    { 2, 3, 5, 0 }, { 2, 2, 5, 1 }, { 2, 1, 5, 1 }, { 2, 1, 6, 0 },
    { 3, 2, 2, 3 }, { 3, 3, 2, 3 },
    { 3, 2, 3, 2 }, { 3, 3, 3, 2 },
    { 3, 2, 4, 1 }, { 3, 3, 4, 1 },
    { 3, 3, 5, 0 }, { 3, 2, 5, 3 },
    { 3, 3, 6, 1 }, { 3, 2, 6, 2 }, { 3, 1, 6, 0 }
  };
  size_t i;

  for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    const unsigned int length = cases[i].length;
    const unsigned int code = cases[i].code;
    uint8_t bytes[] = {
      (uint8_t)(0x0a | (code >> (length - 1))),
      (uint8_t)((code & (((unsigned int)1 << (length - 1)) - 1)) <<
                (9 - length))
    };
    REQUIRE(crystalhd_mpeg2_first_slice_prediction(
                bytes, sizeof(bytes), cases[i].type) == cases[i].prediction,
            "bounded first-macroblock prediction is preserved");
    REQUIRE(crystalhd_mpeg2_first_slice_prediction(bytes, 1, cases[i].type) == 0,
            "truncated first-macroblock prefix is unknown");
  }
}

static void
test_picture_headers(void)
{
  static const uint8_t stream[] = {
    0, 0, 1, 0xb3, 0, 0, 48, 0,
    0, 0, 1, 0xb5, 0x14, 0x08, 0, 0, 0, 0,
    0, 0, 1, 0x00, 0, 2 << 3, 0xff, 0xff, 0,
    0, 0, 1, 0xb5, 0x80, 0, 3, 0, 0,
    0, 0, 1, 0x01, 0x0b, 0,
    0, 0, 1, 0x02, 0x0b, 0,
    0, 0, 1, 0x03, 0x0b, 0
  };
  static const uint8_t truncated_extension[] = {
    0, 0, 1, 0xb5, 0x14, 0x08, 0, 0, 0
  };
  static const uint8_t two_pictures[] = {
    0, 0, 1, 0x00, 0, 1 << 3, 0xff, 0xff,
    0, 0, 1, 0x00, 0, 2 << 3, 0xff, 0xff, 0
  };
  CrystalHdMpeg2Picture picture;
  const uint8_t junk[] = { 0xff, 0xff, 0xff };

  crystalhd_mpeg2_picture_headers(stream, sizeof(stream), &picture);
  REQUIRE(picture.have_sequence && picture.height == 48,
          "sequence height is parsed");
  REQUIRE(picture.have_sequence_extension && picture.profile == 4 &&
              picture.progressive_sequence,
          "sequence extension is parsed");
  REQUIRE(picture.picture_count == 1 && picture.picture_type == 2 &&
              picture.have_minimum_picture_header && picture.picture_structure == 3,
          "complete frame picture headers are parsed");
  REQUIRE((picture.slice_rows[0] & 7) == 7 && picture.slice_prediction == 1,
          "slice rows and forward prediction are retained");

  crystalhd_mpeg2_picture_headers(truncated_extension,
                                  sizeof(truncated_extension), &picture);
  REQUIRE(!picture.have_sequence_extension,
          "truncated sequence extension is not accepted");
  crystalhd_mpeg2_picture_headers(two_pictures, sizeof(two_pictures), &picture);
  REQUIRE(picture.picture_count == 2 && picture.picture_type == 1,
          "only the first of two picture headers supplies frame metadata");
  crystalhd_mpeg2_picture_headers(junk, sizeof(junk), &picture);
  REQUIRE(picture.picture_count == 0 && picture.slice_rows[0] == 0,
          "each inspection clears prior results");
}

int
main(void)
{
  test_start_codes();
  test_slice_prediction();
  test_picture_headers();
  printf("MPEG-2 inspect: %u checks, %u failures\n", checks, failures);
  return failures != 0;
}
