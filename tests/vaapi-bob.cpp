// SPDX-License-Identifier: LGPL-2.1-or-later
// Field-local scalar row mapping only: no VA, decoder, DRM or hardware calls.
#include "../filters/vaapi/crystalhd-vpp-bob.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
using crystalhd_vpp_bob::FieldParity;
using crystalhd_vpp_bob::Nv12FrameHeightsValid;
using crystalhd_vpp_bob::SourcePlaneRow;
unsigned int checks = 0;

void Require(bool condition, const char *message) {
  ++checks;
  if (!condition)
    throw std::runtime_error(message);
}

void KnownRows() {
  struct Case { uint64_t source, target, row, top; };
  const Case cases[] = {
      {8, 8, 0, 0}, {8, 8, 1, 0}, {8, 8, 2, 2}, {8, 8, 7, 6},
      {8, 4, 0, 0}, {8, 4, 1, 2}, {8, 4, 2, 4}, {8, 4, 3, 6},
      {4, 8, 0, 0}, {4, 8, 3, 0}, {4, 8, 4, 2}, {4, 8, 7, 2},
      {6, 4, 0, 0}, {6, 4, 1, 0}, {6, 4, 2, 2}, {6, 4, 3, 4},
      {720, 720, 359, 358}, {720, 720, 360, 360},
      {720, 720, 719, 718},
      {1080, 1080, 539, 538}, {1080, 1080, 540, 540},
      {1080, 1080, 1079, 1078},
      {1088, 1088, 543, 542}, {1088, 1088, 544, 544},
      {1088, 1088, 1087, 1086},
      {1080, 720, 359, 538}, {1080, 720, 360, 540},
      {1080, 720, 719, 1078},
      {1088, 720, 359, 542}, {1088, 720, 360, 544},
      {1088, 720, 719, 1086},
      {720, 1080, 2, 0}, {720, 1080, 3, 2},
      {720, 1080, 539, 358}, {720, 1080, 540, 360},
      {720, 1080, 1079, 718},
      // Coded 1088 is a complete input, not an implicit eight-line crop.
      {1088, 1080, 1079, 1086}, {1080, 1088, 1087, 1078}};
  for (const Case &test : cases) {
    for (unsigned int parity = 0; parity != 2; ++parity) {
      uint64_t actual = 123;
      Require(SourcePlaneRow(test.source, test.target, test.row,
                             static_cast<FieldParity>(parity), &actual),
              "known row must be admitted");
      Require(actual == test.top + parity, "known row mapping differs");
    }
  }
}

// Independent phase-accumulator oracle: no multiplication/division row formula
// and no call to production mapping. Small test dimensions keep sums bounded.
std::vector<size_t> ReferenceRows(size_t source, size_t target,
                                  unsigned int parity) {
  std::vector<size_t> rows;
  size_t field_row = 0, phase = 0;
  for (size_t row = 0; row < target; ++row) {
    rows.push_back(field_row * 2 + parity);
    phase += source / 2;
    while (phase >= target) {
      phase -= target;
      ++field_row;
    }
  }
  return rows;
}

void CompleteRowMaps() {
  for (size_t source : {2U, 4U, 6U, 360U, 540U, 544U, 720U, 1080U, 1088U}) {
    for (size_t target : {2U, 4U, 6U, 360U, 540U, 544U, 720U, 1080U, 1088U}) {
      for (unsigned int parity = 0; parity != 2; ++parity) {
        const auto reference = ReferenceRows(source, target, parity);
        uint64_t previous = parity;
        for (size_t row = 0; row < target; ++row) {
          uint64_t actual = std::numeric_limits<uint64_t>::max();
          Require(SourcePlaneRow(source, target, row,
                                 static_cast<FieldParity>(parity), &actual),
                  "complete row map must be admitted");
          Require(actual == reference[row] && actual < source &&
                      actual % 2 == parity && actual >= previous,
                  "complete map must preserve selected field and bounds");
          previous = actual;
        }
      }
    }
  }
}

struct Plane {
  static constexpr size_t kGuard = 13;
  size_t rows, width, pitch;
  std::vector<uint8_t> storage;
  Plane(size_t row_count, size_t row_width, uint8_t fill)
      : rows(row_count), width(row_width), pitch(row_width + 5),
        storage(kGuard + pitch * rows + kGuard, fill) {}
  uint8_t *Line(size_t row) { return storage.data() + kGuard + row * pitch; }
  const uint8_t *Line(size_t row) const {
    return storage.data() + kGuard + row * pitch;
  }
};

void PlanePixels(size_t source_rows, size_t target_rows, bool uv,
                  unsigned int parity) {
  Plane source(source_rows, 12, 0xa5), actual(target_rows, 12, 0xcd);
  Plane expected = actual;
  for (size_t row = 0; row < source.rows; ++row) {
    for (size_t x = 0; x < source.width; ++x) {
      // Opposite fields have disjoint color ranges, including U and V. A
      // packed U/V pair is copied together; no luma-derived UV row is used.
      source.Line(row)[x] = static_cast<uint8_t>(uv
          ? ((row & 1U) ? 160 : 32) + (x & 1U) * 32 + (row / 2 + x / 2) % 31
          : ((row & 1U) ? 176 : 16) + (row / 2 + x) % 64);
    }
  }
  const auto unchanged = source.storage;
  const auto reference = ReferenceRows(source_rows, target_rows, parity);
  for (size_t row = 0; row < target_rows; ++row) {
    uint64_t selected = std::numeric_limits<uint64_t>::max();
    Require(SourcePlaneRow(source_rows, target_rows, row,
                           static_cast<FieldParity>(parity), &selected),
            "pixel row must be admitted");
    std::copy_n(source.Line(static_cast<size_t>(selected)), actual.width,
                actual.Line(row));
    std::copy_n(source.Line(reference[row]), expected.width, expected.Line(row));
  }
  Require(actual.storage == expected.storage,
          "field-colored pixels and destination row/outer canaries differ");
  Require(source.storage == unchanged, "source pixels/padding must not change");
}

void Nv12Pixels() {
  for (size_t source : {4U, 720U, 1080U, 1088U}) {
    for (size_t target : {4U, 360U, 720U, 1080U, 1088U}) {
      Require(Nv12FrameHeightsValid(source, target), "full NV12 heights valid");
      for (unsigned int parity = 0; parity != 2; ++parity) {
        PlanePixels(source, target, false, parity);
        PlanePixels(source / 2, target / 2, true, parity);
      }
    }
  }
  // A Y-derived UV coordinate would select the wrong physical chroma field:
  // the first bottom Y row is 1, but UV must independently select row 1 too.
  uint64_t y = 99, uv = 99;
  Require(SourcePlaneRow(720, 720, 0, FieldParity::kBottom, &y) &&
              SourcePlaneRow(360, 360, 0, FieldParity::kBottom, &uv),
          "both bottom planes must map");
  Require(y == 1 && uv == 1 && y / 2 != uv,
          "chroma must remain field-local, not derive its row from Y");
}

void InvalidAndWide() {
  const uint64_t maximum = std::numeric_limits<uint64_t>::max();
  const uint64_t canary = UINT64_C(0xdeadbeefcafef00d);
  struct Case { uint64_t source, target, row; unsigned int parity; };
  const Case invalid[] = {
      {0, 720, 0, 0}, {720, 0, 0, 0}, {1, 720, 0, 0},
      {719, 720, 0, 0}, {720, 719, 0, 0}, {720, 1, 0, 0},
      {720, 720, 720, 0}, {720, 720, maximum, 0},
      {720, 720, 0, 2}, {720, 720, 0, ~0U},
      {maximum, 720, 0, 0}, {720, maximum, 0, 0},
      {maximum - 1, maximum - 1, 3, 0},
      {maximum - 1, maximum - 1, maximum - 2, 1},
      {UINT64_C(0x100000000), UINT64_C(0x200000002),
       UINT64_C(0x200000000), 0}};
  for (const Case &test : invalid) {
    uint64_t actual = canary;
    Require(!SourcePlaneRow(test.source, test.target, test.row,
                            static_cast<FieldParity>(test.parity), &actual),
            "invalid or overflowing mapping must fail");
    Require(actual == canary, "failed mapping must leave output untouched");
  }
  Require(!SourcePlaneRow(720, 720, 0, FieldParity::kTop, nullptr),
          "null mapping output rejected");
  for (const auto &pair : {std::pair<uint64_t, uint64_t>(0, 720), {720, 0},
                           {2, 720}, {720, 2}, {722, 720}, {720, 722},
                           {maximum, 720}, {720, maximum}})
    Require(!Nv12FrameHeightsValid(pair.first, pair.second),
            "incomplete NV12 field geometry rejected");

  // Products beyond uint32 are required even for ordinary uint32 geometry.
  uint64_t actual = canary;
  Require(SourcePlaneRow(UINT64_C(0x100000000), UINT64_C(0x100000000),
                         UINT64_C(0xfffffffe), FieldParity::kBottom, &actual) &&
              actual == UINT64_C(0xffffffff),
          "wide valid product must not truncate to uint32");
  Require(SourcePlaneRow(maximum - 1, maximum - 1, 2,
                         FieldParity::kBottom, &actual) && actual == 3,
          "largest nonoverflowing intermediate product admitted");
  Require(SourcePlaneRow(2, maximum - 1, maximum - 2,
                         FieldParity::kBottom, &actual) && actual == 1,
          "large target rows without overflow admitted");
  Require(SourcePlaneRow(maximum - 1, 2, 1,
                         FieldParity::kBottom, &actual) &&
              actual == (maximum - 1) / 2,
          "large source dimensions without overflow admitted");
}
}  // namespace

int main() {
  try {
    KnownRows();
    CompleteRowMaps();
    Nv12Pixels();
    InvalidAndWide();
    std::printf("VA Bob field-local scalar row mapping: %u checks PASS\n", checks);
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "VA Bob field-local row mapping FAIL: %s\n", error.what());
    return 1;
  }
}
