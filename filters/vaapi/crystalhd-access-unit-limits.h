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
// Unknown transports, BCM70012 and interlaced sequences retain this bound.
constexpr size_t kVaH264SubmissionBytes = 2U * 1024U * 1024U;
// Separately qualified BCM70015 / DIL 3.22.0 progressive frame-coded allowance;
// this is an assembled submission policy, not a general chip maximum.
constexpr size_t kVaFleaProgressiveH264SubmissionBytes = 3014656U;
// Additional conservative policy for that extended allowance, not a measured
// firmware allocation or a general decoder maximum. #468 reproduces loss of
// output below the wire-size cap when escape-stripped input approaches 2 MiB.
// Keep a chosen 4 KiB margin; ordinary <=2 MiB admission remains unchanged.
constexpr size_t kVaFleaProgressiveH264DeescapedBytes =
    kVaH264SubmissionBytes - 4096U;
constexpr size_t kReplayCacheBytes = 32U * 1024U * 1024U;
// Match libcrystalhd's public bounded-fragment contract.
constexpr size_t kDtsInputChunkBytes = 512U * 1024U;
// Retained DIL 3.22 BCM70015 H.264/Annex-B input uses bounded PES packets,
// not one ALIGN_BUF_SIZE copy per AU. A complete legacy call must still fit
// its 1 MiB TX ring, including the conservative packet/header reservation.
constexpr size_t kLegacyH264PesBytes = 768U * 1024U;

static_assert(kH264Bytes <= kReplayCacheBytes,
              "one H.264 AU must fit in the replay cache");
static_assert(kVaH264SubmissionBytes <= kH264Bytes,
              "VA submission must remain within the software assembly bound");
static_assert(kVaH264SubmissionBytes < kVaFleaProgressiveH264SubmissionBytes &&
                  kVaFleaProgressiveH264SubmissionBytes <= kH264Bytes,
              "qualified headroom must remain within the software assembly bound");
static_assert(kDtsInputChunkBytes < kVaH264SubmissionBytes,
              "large H.264 AUs exercise libcrystalhd chunking");
}  // namespace crystalhd_access_unit_limits

#endif
