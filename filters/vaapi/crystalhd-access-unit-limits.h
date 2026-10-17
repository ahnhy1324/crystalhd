// SPDX-License-Identifier: LGPL-2.1-or-later
#ifndef CRYSTALHD_ACCESS_UNIT_LIMITS_H
#define CRYSTALHD_ACCESS_UNIT_LIMITS_H

#include <cstddef>

namespace crystalhd_access_unit_limits {

// CrystalHD accepts one logical access unit while its userspace transport
// admits at most one 512 KiB fragment per call. Keep the VA replay allocation
// bounded independently of that transport fragment size.
// Eight MiB is a software assembly/allocation bound, not a decoded-AU claim.
// The reconstructed SPS signals level 3.1/4.0 without HRD; a codec-standard
// CPB ceiling alone does not establish the firmware's complete-AU capacity.
constexpr size_t kH264Bytes = 8U * 1024U * 1024U;
// Conservative VA submission policy, not a physical chip maximum. #442's
// reporter decoded 3,080,192 bytes but lost the remaining session at 3,112,960.
// Primary BCM70015 readback qualified 2 MiB with all 75 luma pictures, including
// the following IDR. Keep larger assembled units away from the hardware until
// separately qualified; retain the independent software bound above.
constexpr size_t kVaH264SubmissionBytes = 2U * 1024U * 1024U;
constexpr size_t kReplayCacheBytes = 32U * 1024U * 1024U;
// Match libcrystalhd's public bounded-fragment contract.
constexpr size_t kDtsInputChunkBytes = 512U * 1024U;

static_assert(kH264Bytes <= kReplayCacheBytes,
              "one H.264 AU must fit in the replay cache");
static_assert(kVaH264SubmissionBytes <= kH264Bytes,
              "VA submission must remain within the software assembly bound");
static_assert(kDtsInputChunkBytes < kVaH264SubmissionBytes,
              "large H.264 AUs exercise libcrystalhd chunking");
}  // namespace crystalhd_access_unit_limits

#endif
