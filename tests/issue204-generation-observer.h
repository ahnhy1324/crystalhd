/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef CRYSTALHD_TESTS_ISSUE204_GENERATION_OBSERVER_H
#define CRYSTALHD_TESTS_ISSUE204_GENERATION_OBSERVER_H

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "issue204-active-context.h"

/*
 * A device-independent extension of the bounded issue #204 active-context
 * observer.  The outer-ARC addresses below are the finite selector, cursor,
 * mapping and pool-pointer surface recorded by private research PR #34 in
 * results/producer-frame-selector-2026-10-14/summary.json.  The H+e0/E and
 * H+ec/F fields are recorded by PR #40 in
 * results/picture-field-ledger-2026-10-14/ledger.json.  Only those numeric
 * contracts are used here; no firmware body or private type is reproduced.
 * The joined i is the producer-domain selector transported through the last
 * consumed display-ring slot.  latest_i is captured as bounded context only;
 * pipeline overlap means it is not required to equal that delivered i.
 *
 * The extension runs while the existing observer has verified the outer ARC
 * halted.  ARM continues to run, so equal ARM cache samples bracketing the
 * held outer snapshot are stability evidence, not an atomic cross-core halt,
 * a physical lease or a last-consumer completion proof.
 * receipt_generation/run_id identify the armed host/driver evidence capture;
 * they are not firmware PPB or source-memory generation counters.
 */
namespace issue204_generation_observer {

static const uint32_t kOuterProducerWord = 0x00858eb0U;
static const uint32_t kOuterDisplayTable = 0x00858ef8U;
static const uint32_t kOuterDisplayCursors = 0x00858f38U;
static const uint32_t kOuterAssignmentFramePool = 0x00858fc8U;
static const uint32_t kOuterJToI = 0x00859144U;
static const uint32_t kOuterIToJ = 0x00859166U;
static const uint32_t kOuterDisplayFramePool = 0x008592dcU;

static const uint32_t kFrameCount = 63U;
static const uint32_t kDisplayRingCount = 64U;
static const uint32_t kFrameStride = 284U;
static const uint32_t kFrameMetadataOffset = 56U;
static const uint32_t kFramePoolBytes = kFrameStride * kFrameCount;
static const uint32_t kDeliveredCount = 34U;
static const uint32_t kDeliveredPoolBytes =
    issue204_active_context::kRecordStride * kDeliveredCount;
static const uint32_t kArmCacheOffset = 0xe0U;
static const uint32_t kArmElementBytes = 12U;
static const uint32_t kArmPreparedBytes = 140U;
static const uint32_t kArmCacheBytes = kArmElementBytes + kArmPreparedBytes;

enum class Failure {
    None,
    Argument,
    OuterState,
    Address,
    Record,
    ArmCache,
    Source,
    Receipt
};

struct Result {
    Failure failure = Failure::None;
    bool observed = false;
    bool receipt_verified = false;
    uint64_t receipt_generation = 0;
    uint64_t receipt_run_id = 0;
    uint32_t latest_producer_i = kFrameCount;
    uint32_t configured_count = 0;
    uint32_t display_producer = kDisplayRingCount;
    uint32_t display_consumer = kDisplayRingCount;
    uint32_t display_ring_slot = kDisplayRingCount;
    uint32_t delivered_i = kFrameCount;
    uint32_t delivered_j = kDeliveredCount;
    uint32_t frame_pool = 0;
    uint32_t frame_metadata = 0;
    uint32_t delivered_pool = 0;
    uint32_t delivered_record = 0;
    uint32_t arm_mapped_p = 0;
    uint32_t arm_raw_record = 0;
    uint32_t luma = 0;
    uint32_t chroma = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t raw_firmware_timestamp = 0;
    uint32_t source_bank = kFrameCount;
    uint32_t source_valid_bytes = 0;
};

struct Capture {
    Result *result = nullptr;
    uint32_t *source_words = nullptr;
    size_t source_word_count = 0;
    uint64_t receipt_generation = 0;
    uint64_t receipt_run_id = 0;
};

struct Receipt {
    uint64_t generation = 0;
    uint64_t run_id = 0;
    uint64_t host_token = 0;
    uint64_t raw_firmware_timestamp = 0;
    uint64_t normalized_firmware_timestamp = 0;
    uint32_t host_width = 0;
    uint32_t host_height = 0;
    uint32_t driver_width = 0;
    uint32_t driver_height = 0;
};

struct OuterState {
    uint32_t latest_i = 0;
    uint32_t count = 0;
    uint32_t display_producer = 0;
    uint32_t display_consumer = 0;
    uint32_t display_slot = 0;
    uint32_t display_i = 0;
    uint32_t delivered_j = 0;
    uint32_t reverse_i = 0;
    uint32_t assignment_frame_pool = 0;
    uint32_t display_frame_pool = 0;
};

enum class ReportOutcome {
    Pass,
    Fail,
    NotRun
};

enum class ReportReason {
    None,
    ActiveContextHaltPrecondition,
    ActiveContextPrecondition
};

struct ReportDecision {
    ReportDecision(ReportOutcome selected = ReportOutcome::Fail,
                   ReportReason why = ReportReason::None)
        : outcome(selected), reason(why) {}
    ReportOutcome outcome;
    ReportReason reason;
};

/*
 * Keep report classification separate from formatting.  In particular, a
 * halted outer ARC whose DEBUG.LD bit never clears prevents ObserveHeld
 * from running at all: the untouched Result sentinels are not measurements
 * and Failure::None is not a successful extension result.
 */
static inline ReportDecision ClassifyReport(
    bool active_ok, const issue204_active_context::Result &active,
    const Result &generation)
{
    const bool extension_not_executed =
        !generation.observed && generation.failure == Failure::None &&
        !generation.receipt_generation && !generation.receipt_run_id;
    if (extension_not_executed) {
        return {ReportOutcome::NotRun,
                active.failure == issue204_active_context::Failure::Halt
                    ? ReportReason::ActiveContextHaltPrecondition
                    : ReportReason::ActiveContextPrecondition};
    }
    if (active_ok &&
        active.failure == issue204_active_context::Failure::None &&
        active.resume_verified && active.route_restored &&
        generation.observed && generation.failure == Failure::None)
        return {ReportOutcome::Pass, ReportReason::None};
    return {ReportOutcome::Fail, ReportReason::None};
}

static inline const char *ReportOutcomeName(ReportOutcome outcome)
{
    switch (outcome) {
    case ReportOutcome::Pass: return "PASS";
    case ReportOutcome::Fail: return "FAIL";
    case ReportOutcome::NotRun: return "NOT-RUN";
    }
    return "FAIL";
}

static inline const char *ReportReasonName(ReportReason reason)
{
    switch (reason) {
    case ReportReason::None: return "none";
    case ReportReason::ActiveContextHaltPrecondition:
        return "active-context-halt-precondition";
    case ReportReason::ActiveContextPrecondition:
        return "active-context-precondition";
    }
    return "none";
}

static inline bool Fail(Result *result, Failure failure)
{
    if (result && result->failure == Failure::None) result->failure = failure;
    return false;
}

static inline uint32_t Byte(uint32_t word, uint32_t address)
{
    // The pinned outer ELF is ELF32 little-endian; these are byte lanes in
    // the same core-window DWORD representation used by the word aliases.
    return (word >> ((address & 3U) * 8U)) & 0xffU;
}

static inline bool HeldWord(const issue204_active_context::Io &io,
                            issue204_active_context::Result *active,
                            uint32_t address, uint32_t *word)
{
    return word && !(address & 3U) &&
        issue204_active_context::HeldDatum(io, active, address, word);
}

static inline bool HeldByte(const issue204_active_context::Io &io,
                            issue204_active_context::Result *active,
                            uint32_t address, uint32_t *value)
{
    uint32_t word = 0;
    if (!value || !HeldWord(io, active, address & ~3U, &word)) return false;
    *value = Byte(word, address);
    return true;
}

static inline bool ReadOuter(const issue204_active_context::Io &io,
                             issue204_active_context::Result *active,
                             OuterState *state)
{
    uint32_t producer_word = 0, cursor_word = 0;
    if (!state || !HeldWord(io, active, kOuterProducerWord, &producer_word) ||
        !HeldWord(io, active, kOuterDisplayCursors, &cursor_word)) return false;
    state->latest_i = Byte(producer_word, kOuterProducerWord);
    state->count = Byte(producer_word, kOuterProducerWord + 1U);
    state->display_producer = Byte(cursor_word, kOuterDisplayCursors);
    state->display_consumer = Byte(cursor_word, kOuterDisplayCursors + 1U);
    if (!state->count || state->count > kFrameCount ||
        state->latest_i >= state->count ||
        state->display_producer >= kDisplayRingCount ||
        state->display_consumer >= kDisplayRingCount ||
        state->display_producer != state->display_consumer)
        return false;
    state->display_slot =
        (state->display_consumer + kDisplayRingCount - 1U) &
        (kDisplayRingCount - 1U);
    if (!HeldByte(io, active, kOuterDisplayTable + state->display_slot,
                  &state->display_i) ||
        state->display_i >= state->count ||
        !HeldByte(io, active, kOuterIToJ + state->display_i,
                  &state->delivered_j) ||
        state->delivered_j >= kDeliveredCount ||
        !HeldByte(io, active, kOuterJToI + state->delivered_j,
                  &state->reverse_i) ||
        !HeldWord(io, active, kOuterAssignmentFramePool,
                  &state->assignment_frame_pool) ||
        !HeldWord(io, active, kOuterDisplayFramePool,
                  &state->display_frame_pool)) return false;
    return state->reverse_i == state->display_i &&
        state->assignment_frame_pool == state->display_frame_pool;
}

static inline bool OuterEqual(const OuterState &a, const OuterState &b)
{
    return a.latest_i == b.latest_i && a.count == b.count &&
        a.display_producer == b.display_producer &&
        a.display_consumer == b.display_consumer &&
        a.display_slot == b.display_slot && a.display_i == b.display_i &&
        a.delivered_j == b.delivered_j && a.reverse_i == b.reverse_i &&
        a.assignment_frame_pool == b.assignment_frame_pool &&
        a.display_frame_pool == b.display_frame_pool;
}

static inline bool CheckedRecordAddress(uint32_t base, uint32_t stride,
                                        uint32_t index, uint32_t tail,
                                        uint32_t *address)
{
    if (!address || static_cast<uint64_t>(stride) * index + tail > 0xffffffffULL)
        return false;
    return issue204_active_context::CheckedAdd(
        base, stride * index + tail, address);
}

static inline bool SourceMatches(const uint32_t *words, Result *result)
{
    if (!words || !result) return Fail(result, Failure::Argument);
    for (unsigned index = 0; index < issue204_active_context::kSourceWords;
         ++index) {
        uint32_t expected = 0;
        const int known = issue204_active_context::KnownSourceWord(index,
                                                                   &expected);
        if (known < 0 || (known && words[index] != expected))
            return Fail(result, Failure::Source);
    }
    result->source_valid_bytes = issue204_active_context::kSourceValidBytes;
    return true;
}

static inline bool ObserveHeld(const issue204_active_context::Plan &plan,
                               const issue204_active_context::Io &io,
                               issue204_active_context::Result *active,
                               void *opaque)
{
    Capture *capture = static_cast<Capture *>(opaque);
    if (!capture || !capture->result || !capture->source_words ||
        capture->source_word_count != issue204_active_context::kSourceWords ||
        !capture->receipt_generation || !capture->receipt_run_id)
        return capture && capture->result
            ? Fail(capture->result, Failure::Argument) : false;
    Result *result = capture->result;
    *result = Result{};
    result->receipt_generation = capture->receipt_generation;
    result->receipt_run_id = capture->receipt_run_id;

    uint32_t arm_before[kArmCacheBytes / 4U] = {};
    uint32_t arm_after[kArmCacheBytes / 4U] = {};
    OuterState outer_before, outer_after;
    uint32_t frame_before[issue204_active_context::kRecordBytes / 4U] = {};
    uint32_t frame_after[issue204_active_context::kRecordBytes / 4U] = {};
    uint32_t delivered_before[issue204_active_context::kRecordBytes / 4U] = {};
    uint32_t delivered_after[issue204_active_context::kRecordBytes / 4U] = {};
    uint32_t arm_cache = 0;
    if (!issue204_active_context::CheckedAdd(plan.holder, kArmCacheOffset,
                                             &arm_cache) ||
        !issue204_active_context::HeapObject(plan.holder, 0x0000a84cU) ||
        !issue204_active_context::RangeWithin(
            arm_cache, kArmCacheBytes, plan.holder, 0x0000a84cU))
        return Fail(result, Failure::Address);
    if (!issue204_active_context::HeldMemory(io, active, arm_cache, arm_before,
                                             kArmCacheBytes))
        return Fail(result, Failure::ArmCache);
    if (!ReadOuter(io, active, &outer_before))
        return Fail(result, Failure::OuterState);

    uint32_t frame_metadata = 0, delivered_pool = 0, delivered_record = 0;
    if (!issue204_active_context::CheckedAdd(
            active->allocation, issue204_active_context::kRecordOffset,
            &delivered_pool) ||
        !outer_before.display_frame_pool ||
        !issue204_active_context::RangeWithin(
            outer_before.display_frame_pool,
            kFramePoolBytes,
            plan.video_base, plan.video_bytes) ||
        !issue204_active_context::RangeWithin(
            delivered_pool, kDeliveredPoolBytes,
            active->allocation, issue204_active_context::kPoolBytes) ||
        !CheckedRecordAddress(outer_before.display_frame_pool, kFrameStride,
            outer_before.display_i, kFrameMetadataOffset, &frame_metadata) ||
        !CheckedRecordAddress(delivered_pool,
            issue204_active_context::kRecordStride, outer_before.delivered_j,
            0U, &delivered_record) ||
        !issue204_active_context::RangeWithin(
            frame_metadata, issue204_active_context::kRecordBytes,
            outer_before.display_frame_pool,
            kFramePoolBytes) ||
        !issue204_active_context::RangeWithin(
            delivered_record, issue204_active_context::kRecordBytes,
            delivered_pool, kDeliveredPoolBytes))
        return Fail(result, Failure::Address);

    if (!issue204_active_context::HeldMemory(
            io, active, frame_metadata, frame_before,
            issue204_active_context::kRecordBytes) ||
        !issue204_active_context::HeldMemory(
            io, active, delivered_record, delivered_before,
            issue204_active_context::kRecordBytes))
        return Fail(result, Failure::Record);

    const uint32_t luma = delivered_before[1];
    const uint32_t chroma = delivered_before[2];
    const uint32_t width = delivered_before[3];
    const uint32_t height = delivered_before[4];
    const uint32_t raw_timestamp = delivered_before[13];
    uint32_t expected_chroma = 0;
    if (!luma || !issue204_active_context::CheckedAdd(luma, 24576U,
                                                       &expected_chroma) ||
        chroma != expected_chroma || width != 256U || height != 96U ||
        !raw_timestamp || raw_timestamp == 0x7ffffffeU ||
        raw_timestamp == 0xffffffffU ||
        frame_before[1] != luma || frame_before[2] != chroma ||
        frame_before[3] != width || frame_before[4] != height ||
        frame_before[13] != raw_timestamp || !active->bank_stride ||
        !active->bank_count || luma < active->bank0 ||
        (luma - active->bank0) % active->bank_stride)
        return Fail(result, Failure::Record);
    const uint32_t source_bank =
        (luma - active->bank0) / active->bank_stride;
    if (source_bank >= active->bank_count || source_bank >= 32U ||
        !(active->bank_bitmap & (1U << source_bank)) ||
        !issue204_active_context::RangeWithin(
            luma, issue204_active_context::kSourceReadBytes,
            plan.video_base, plan.video_bytes))
        return Fail(result, Failure::Source);
    if (!issue204_active_context::HeldMemory(
            io, active, luma, capture->source_words,
            issue204_active_context::kSourceReadBytes))
        return Fail(result, Failure::Source);
    if (!SourceMatches(capture->source_words, result))
        return false;

    if (!ReadOuter(io, active, &outer_after) ||
        !OuterEqual(outer_before, outer_after))
        return Fail(result, Failure::OuterState);
    if (!issue204_active_context::HeldMemory(
            io, active, frame_metadata, frame_after,
            issue204_active_context::kRecordBytes) ||
        std::memcmp(frame_before, frame_after, sizeof(frame_before)) ||
        !issue204_active_context::HeldMemory(
            io, active, delivered_record, delivered_after,
            issue204_active_context::kRecordBytes) ||
        std::memcmp(delivered_before, delivered_after,
                    sizeof(delivered_before)))
        return Fail(result, Failure::Record);
    if (!issue204_active_context::HeldMemory(io, active, arm_cache, arm_after,
                                             kArmCacheBytes) ||
        std::memcmp(arm_before, arm_after, sizeof(arm_before)))
        return Fail(result, Failure::ArmCache);

    const uint32_t *prepared = arm_before + kArmElementBytes / 4U;
    if (arm_before[1] != delivered_record || !arm_before[0] ||
        prepared[5] != width || prepared[6] != height ||
        prepared[13] != luma || prepared[14] != chroma)
        return Fail(result, Failure::ArmCache);

    result->latest_producer_i = outer_before.latest_i;
    result->configured_count = outer_before.count;
    result->display_producer = outer_before.display_producer;
    result->display_consumer = outer_before.display_consumer;
    result->display_ring_slot = outer_before.display_slot;
    result->delivered_i = outer_before.display_i;
    result->delivered_j = outer_before.delivered_j;
    result->frame_pool = outer_before.display_frame_pool;
    result->frame_metadata = frame_metadata;
    result->delivered_pool = delivered_pool;
    result->delivered_record = delivered_record;
    result->arm_mapped_p = arm_before[0];
    result->arm_raw_record = arm_before[1];
    result->luma = luma;
    result->chroma = chroma;
    result->width = width;
    result->height = height;
    result->raw_firmware_timestamp = raw_timestamp;
    result->source_bank = source_bank;
    result->observed = true;
    return true;
}

static inline issue204_active_context::HeldExtension Extension(Capture *capture)
{
    issue204_active_context::HeldExtension extension;
    extension.opaque = capture;
    extension.observe = ObserveHeld;
    return extension;
}

static inline bool VerifyReceipt(Result *result, const Receipt &receipt)
{
    if (!result || result->failure != Failure::None || !result->observed ||
        result->receipt_verified ||
        receipt.generation != result->receipt_generation ||
        receipt.run_id != result->receipt_run_id ||
        receipt.raw_firmware_timestamp != result->raw_firmware_timestamp ||
        receipt.normalized_firmware_timestamp !=
            static_cast<uint64_t>(result->raw_firmware_timestamp) * 20000U ||
        receipt.host_token != receipt.normalized_firmware_timestamp ||
        receipt.host_width != result->width ||
        receipt.host_height != result->height ||
        receipt.driver_width != result->width ||
        receipt.driver_height != result->height)
        return Fail(result, Failure::Receipt);
    result->receipt_verified = true;
    return true;
}

}  // namespace issue204_generation_observer

#endif
