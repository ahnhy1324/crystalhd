// SPDX-License-Identifier: LGPL-2.1-or-later
#ifndef CRYSTALHD_ACCESS_UNIT_LIMITS_H
#define CRYSTALHD_ACCESS_UNIT_LIMITS_H

#include <cstddef>

namespace crystalhd_access_unit_limits {

// CrystalHD accepts one logical access unit while its userspace transport
// admits at most one 512 KiB fragment per call. Keep the VA replay allocation
// bounded independently of that transport fragment size.
// Eight MiB covers the High@L4.1 H.264 CPB ceiling with binary-size headroom.
// Other codec paths retain their independently qualified limits.
constexpr size_t kH264Bytes = 8U * 1024U * 1024U;
constexpr size_t kReplayCacheBytes = 32U * 1024U * 1024U;
// Match libcrystalhd's public bounded-fragment contract.
constexpr size_t kDtsInputChunkBytes = 512U * 1024U;

static_assert(kH264Bytes <= kReplayCacheBytes,
              "one H.264 AU must fit in the replay cache");
static_assert(kDtsInputChunkBytes < kH264Bytes,
              "large H.264 AUs exercise libcrystalhd chunking");
}  // namespace crystalhd_access_unit_limits

#endif
