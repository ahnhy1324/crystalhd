// SPDX-License-Identifier: LGPL-2.1-or-later
#ifndef CRYSTALHD_VPP_BOB_H
#define CRYSTALHD_VPP_BOB_H

#include <cstdint>
#include <limits>

namespace crystalhd_vpp_bob {

// Physical row parity, not temporal field order. TFF/BFF scheduling belongs to
// the caller and must not change which source rows a selected field contains.
enum class FieldParity : unsigned int { kTop = 0, kBottom = 1 };

// Full-frame NV12 needs even Y and UV plane heights. This does not establish
// buffer bounds, field order, chroma siting or support for a cropped region.
inline bool Nv12FrameHeightsValid(uint64_t source_height,
                                 uint64_t target_height) {
  return source_height != 0 && target_height != 0 &&
         ((source_height | target_height) & 3U) == 0;
}

// Origin-aligned nearest-neighbor sampling within one physical field. Apply
// independently to Y (H rows) and UV (H/2 rows), after validating full-frame
// NV12 geometry above; deriving a UV row from the selected Y row mixes fields.
// At equal heights this duplicates each selected field row twice. No pointer
// arithmetic, allocation, pixel writes, interpolation or cropping is done.
// Unrepresentable intermediate products are rejected rather than wrapped.
// On any failure, *source_row is unchanged. A null output is rejected.
inline bool SourcePlaneRow(uint64_t source_rows, uint64_t target_rows,
                           uint64_t target_row, FieldParity selected,
                           uint64_t *source_row) {
  const unsigned int parity = static_cast<unsigned int>(selected);
  if (source_row == nullptr || source_rows == 0 || target_rows == 0 ||
      ((source_rows | target_rows) & 1U) != 0 || target_row >= target_rows ||
      parity > 1U)
    return false;
  const uint64_t field_rows = source_rows / 2;
  if (target_row > std::numeric_limits<uint64_t>::max() / field_rows)
    return false;
  const uint64_t field_row = target_row * field_rows / target_rows;
  // target_row < target_rows implies field_row < source_rows/2, so both
  // doubling and adding either valid parity remain below source_rows.
  *source_row = field_row * 2 + parity;
  return true;
}

}  // namespace crystalhd_vpp_bob

#endif
