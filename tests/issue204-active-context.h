/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef CRYSTALHD_TESTS_ISSUE204_ACTIVE_CONTEXT_H
#define CRYSTALHD_TESTS_ISSUE204_ACTIVE_CONTEXT_H

#include <cstddef>
#include <cstdint>
#include <cstring>

/*
 * Device-independent preservation model for the native issue #204
 * observation.  A caller must obtain Plan from a separately bracketed,
 * same-owner PpbContextObserver::Graph() pair.  This helper is not connected
 * to a hardware CLI: the historical native trials used a private literal-
 * address probe, while this model records the invariants needed by a future
 * dynamic probe.
 *
 * A successful model run joins one sequential, non-atomic held outer-ARC
 * snapshot only; it is not a global halt.  It does not establish a source
 * generation, a physical lease, producer identity, a last-consumer completion
 * event, or the cause of a later release.  The observed raw PTS word remains
 * opaque and is not promoted to a host token or frame identity.
 */
namespace issue204_active_context {

static const uint32_t kActiveContext = 0x000d3a00U;
static const uint32_t kRoute = 0x00851010U;
static const uint32_t kStatus = 0x00855000U;
static const uint32_t kIdentity = 0x00855010U;
static const uint32_t kDebug = 0x00855014U;
static const uint32_t kOuterTableAlias = 0x00859378U;
static const uint32_t kOuterRecordAlias = 0x008590e8U;
static const uint32_t kOuterDeliveryAlias = 0x008592d0U;
static const uint32_t kOuterReturnAlias = 0x008592d4U;
static const uint32_t kHalt = 0x02000000U;
static const uint32_t kPendingLoad = 0x80000000U;
static const uint32_t kOtherDebug =
    0x40000000U | 0x20000000U | 0x00800000U | 0x00000800U;
static const uint32_t kDeliveryOffset = 0x00015678U;
static const uint32_t kReturnOffset = 0x00015778U;
static const uint32_t kRecordOffset = 0x00015878U;
static const uint32_t kRecordBytes = 228U;
static const uint32_t kRecordStride = 0x000000e4U;
static const uint32_t kRecordCount = 34U;
static const uint32_t kPoolBytes = 0x000177ccU;
static const uint32_t kSourceReadBytes = 49152U;
static const uint32_t kSourceValidBytes = 36864U;
static const uint32_t kSourceWords = kSourceReadBytes / 4U;

enum class Failure {
    None,
    Argument,
    Owner,
    Budget,
    Access,
    Guard,
    Identity,
    Halt,
    Graph,
    Alias,
    Ledger,
    Record,
    Source,
    Resume
};

struct Plan {
    uint32_t active_context = 0;
    uint32_t context = 0;
    uint32_t holder = 0;
    uint32_t submitted = 0;
    uint32_t submitted_bytes = 0;
    uint32_t video_base = 0;
    uint32_t video_bytes = 0;
};

struct Io {
    void *opaque = nullptr;
    int (*valid)(void *) = nullptr;
    int (*read_register)(void *, uint32_t, uint32_t *) = nullptr;
    int (*write_register)(void *, uint32_t, uint32_t) = nullptr;
    int (*read_memory)(void *, uint32_t, uint32_t *, uint32_t) = nullptr;
    int (*pause)(void *) = nullptr;
};

struct Result {
    Failure failure = Failure::None;
    int api_status = 0;
    unsigned register_reads = 0;
    unsigned register_writes = 0;
    unsigned memory_reads = 0;
    unsigned halt_polls = 0;
    unsigned resume_polls = 0;
    bool route_select_attempted = false;
    bool halt_attempted = false;
    bool resume_attempted = false;
    bool route_restore_attempted = false;
    bool route_selected = false;
    bool halt_verified = false;
    bool resume_verified = false;
    bool route_restored = false;
    // Any failure after a route-select attempt deliberately performs no more
    // custom I/O: an API error does not prove the write was unapplied.  A
    // future hardware caller must use ordinary module/device recovery.
    bool module_recovery_required = false;
    uint32_t halted_status = 0;
    uint32_t allocation = 0;
    uint32_t delivery_ring = 0;
    uint32_t return_ring = 0;
    uint32_t record = 0;
    uint32_t record_slot = kRecordCount;
    uint32_t delivery_read = 0;
    uint32_t delivery_write = 0;
    uint32_t return_read = 0;
    uint32_t return_write = 0;
    uint32_t bank0 = 0;
    uint32_t chroma = 0;
    uint32_t raw_pts = 0;
    uint32_t source_valid_bytes = 0;
};

static inline bool CheckedAdd(uint32_t base, uint32_t amount, uint32_t *sum)
{
    if (!sum || static_cast<uint64_t>(base) + amount > 0xffffffffULL) return false;
    *sum = base + amount;
    return true;
}

static inline bool RangeWithin(uint32_t base, uint32_t bytes,
                               uint32_t outer_base, uint32_t outer_bytes)
{
    const uint64_t end = static_cast<uint64_t>(base) + bytes;
    const uint64_t outer_end = static_cast<uint64_t>(outer_base) + outer_bytes;
    return bytes && outer_bytes && base >= outer_base && end <= outer_end &&
        outer_end <= 0x04000000ULL;
}

static inline bool HeapObject(uint32_t pointer, uint32_t bytes)
{
    return pointer >= 0x000d53dcU && !(pointer & 3U) && bytes &&
        static_cast<uint64_t>(pointer) + bytes <= 0x00116000ULL;
}

static inline bool ValidatePlan(const Plan &plan, uint32_t *allocation = nullptr,
                                uint32_t *delivery = nullptr,
                                uint32_t *returned = nullptr,
                                uint32_t *record = nullptr)
{
    uint32_t d = 0, delivery_local = 0, return_local = 0, record_local = 0;
    const uint32_t skip = (0U - plan.submitted) & 3U;
    const uint64_t holder_end = static_cast<uint64_t>(plan.holder) + 0x0000a84cU;
    const uint64_t context_end = static_cast<uint64_t>(plan.context) + 0x00000378U;
    const bool objects_disjoint = holder_end <= plan.context || context_end <= plan.holder;
    if (plan.active_context != kActiveContext ||
        !HeapObject(plan.holder, 0x0000a84cU) ||
        !HeapObject(plan.context, 0x00000378U) ||
        !objects_disjoint || plan.video_base < 0x00116068U ||
        (plan.video_base & 4095U) ||
        (plan.video_bytes & 3U) ||
        static_cast<uint64_t>(plan.video_base) + plan.video_bytes > 0x03ffc000ULL ||
        !plan.submitted || !plan.submitted_bytes ||
        !RangeWithin(plan.submitted, plan.submitted_bytes,
                     plan.video_base, plan.video_bytes) ||
        !CheckedAdd(plan.submitted, skip, &d) ||
        !RangeWithin(d, kPoolBytes, plan.submitted, plan.submitted_bytes) ||
        !RangeWithin(d, kPoolBytes, plan.video_base, plan.video_bytes) ||
        !CheckedAdd(d, kDeliveryOffset, &delivery_local) ||
        !CheckedAdd(d, kReturnOffset, &return_local) ||
        !CheckedAdd(d, kRecordOffset, &record_local) ||
        !RangeWithin(record_local, kRecordBytes, d, kPoolBytes)) return false;
    if (allocation) *allocation = d;
    if (delivery) *delivery = delivery_local;
    if (returned) *returned = return_local;
    if (record) *record = record_local;
    return true;
}

static inline uint32_t LedgerAddress(unsigned index)
{
    if (index < 32U) return 0x00859800U + index * 4U;
    if (index < 49U) return 0x00859100U + (index - 32U) * 4U;
    return index < 87U ? 0x008591a8U + (index - 49U) * 4U : 0U;
}

/* Numeric DWORD lanes are the normalized MSB-first values returned by MEM_RD.
 * Padding in the 49,152-byte stride is intentionally not certified. */
static inline int KnownSourceWord(unsigned index, uint32_t *word)
{
    if (!word || index >= kSourceWords) return -1;
    uint32_t value = 0;
    for (unsigned lane = 0; lane < 4U; ++lane) {
        const unsigned offset = index * 4U + lane;
        unsigned sample = 0;
        if (offset < 24576U) {
            const unsigned stripe = offset / 6144U;
            const unsigned row = (offset % 6144U) / 64U;
            const unsigned x = stripe * 64U + offset % 64U;
            sample = 16U + (17U * x + 29U * row + (x ^ row)) % 220U;
        } else {
            const unsigned relative = offset - 24576U;
            const unsigned stripe = relative / 6144U;
            const unsigned row = (relative % 6144U) / 64U;
            const unsigned x = stripe * 32U + (relative % 64U) / 2U;
            if (row >= 48U) return 0;
            sample = relative % 2U ?
                16U + (47U * x + 13U * row + ((3U * x) ^ (5U * row))) % 225U :
                16U + (19U * x + 31U * row + (x ^ row)) % 225U;
        }
        value = (value << 8U) | sample;
    }
    *word = value;
    return 1;
}

static inline bool Reject(Result *result, Failure failure, int status = 0)
{
    if (result && result->failure == Failure::None) {
        result->failure = failure;
        result->api_status = status;
        result->module_recovery_required =
            result->route_select_attempted && !result->route_restored;
    }
    return false;
}

static inline bool Alive(const Io &io, Result *result)
{
    return io.valid && io.valid(io.opaque) ? true : Reject(result, Failure::Owner);
}

static inline bool RegisterRead(const Io &io, Result *result,
                                uint32_t address, uint32_t *word)
{
    if (!result || result->failure != Failure::None || !word ||
        !io.read_register || !Alive(io, result)) return false;
    if (result->register_reads >= 4096U) return Reject(result, Failure::Budget);
    ++result->register_reads;
    const int status = io.read_register(io.opaque, address, word);
    if (status || !Alive(io, result)) return Reject(result, Failure::Access, status);
    return true;
}

static inline bool RegisterWrite(const Io &io, Result *result,
                                 uint32_t address, uint32_t word)
{
    if (!result || result->failure != Failure::None ||
        !io.write_register || !Alive(io, result)) return false;
    const bool allowed = (address == kRoute && word <= 1U) ||
        (address == kDebug && word == 2U) ||
        (address == kStatus && !(word & kHalt));
    if (!allowed || result->register_writes >= 4U)
        return Reject(result, Failure::Budget);
    ++result->register_writes;
    const int status = io.write_register(io.opaque, address, word);
    if (status || !Alive(io, result)) return Reject(result, Failure::Access, status);
    return true;
}

static inline bool MemoryRead(const Io &io, Result *result, uint32_t address,
                              uint32_t *words, uint32_t bytes)
{
    if (!result || result->failure != Failure::None || !words || !bytes ||
        (address & 3U) || (bytes & 3U) || !io.read_memory ||
        !Alive(io, result)) return false;
    if (result->memory_reads >= 32U ||
        static_cast<uint64_t>(address) + bytes > 0x04000000ULL)
        return Reject(result, Failure::Budget);
    ++result->memory_reads;
    const int status = io.read_memory(io.opaque, address, words, bytes);
    if (status || !Alive(io, result)) return Reject(result, Failure::Access, status);
    return true;
}

static inline bool Guard(const Io &io, Result *result, uint32_t route)
{
    uint32_t word = 0;
    return RegisterRead(io, result, 0x00540000U, &word) && word == 0x50U &&
        RegisterRead(io, result, 0x004000d4U, &word) && word == 0U &&
        RegisterRead(io, result, kRoute, &word) && word == route ? true :
        Reject(result, Failure::Guard);
}

static inline bool Datum(const Io &io, Result *result, uint32_t address,
                         uint32_t *word)
{
    return Guard(io, result, 1U) && RegisterRead(io, result, address, word) &&
        Guard(io, result, 1U);
}

static inline bool Held(const Io &io, Result *result)
{
    uint32_t identity = 0, status = 0, debug = 0;
    return result && result->halt_verified &&
        Datum(io, result, kIdentity, &identity) && identity == 0x7b010108U &&
        Datum(io, result, kStatus, &status) && status == result->halted_status &&
        (status & kHalt) && Datum(io, result, kDebug, &debug) &&
        !(debug & (kOtherDebug | kPendingLoad)) ? true :
        Reject(result, Failure::Halt);
}

static inline bool HeldMemory(const Io &io, Result *result, uint32_t address,
                              uint32_t *words, uint32_t bytes)
{
    return Held(io, result) && MemoryRead(io, result, address, words, bytes) &&
        Held(io, result);
}

static inline bool HeldDatum(const Io &io, Result *result, uint32_t address,
                             uint32_t *word)
{
    return Held(io, result) && Datum(io, result, address, word) &&
        Held(io, result);
}

struct Edges {
    uint32_t root_context = 0;
    uint32_t holder = 0;
    uint32_t submitted = 0;
    uint32_t submitted_bytes = 0;
    uint32_t context = 0;
    uint32_t terminal = 0;
    uint32_t delivery = 0;
    uint32_t returned = 0;
};

static inline bool ReadEdges(const Io &io, Result *result, const Plan &plan,
                             Edges *edges)
{
    uint32_t pair[2] = {};
    if (!edges || !HeldMemory(io, result, plan.active_context + 8U,
                             &edges->root_context, 4U) ||
        !HeldMemory(io, result, plan.active_context + 0x20U,
                             &edges->holder, 4U) ||
        !HeldMemory(io, result, plan.holder + 8U, pair, 8U)) return false;
    edges->submitted = pair[0];
    edges->submitted_bytes = pair[1];
    if (!HeldMemory(io, result, plan.holder + 0x64U, &edges->context, 4U) ||
        !HeldMemory(io, result, plan.holder + 0x224U, &edges->terminal, 4U) ||
        !HeldMemory(io, result, plan.holder + 0x250U, pair, 8U)) return false;
    edges->delivery = pair[0];
    edges->returned = pair[1];
    return true;
}

static inline bool EdgesMatch(const Edges &edges, const Plan &plan,
                              uint32_t delivery, uint32_t returned)
{
    return edges.root_context == plan.context && edges.holder == plan.holder &&
        edges.submitted == plan.submitted &&
        edges.submitted_bytes == plan.submitted_bytes &&
        edges.context == plan.context && edges.terminal == 0x00116004U &&
        edges.delivery == delivery && edges.returned == returned;
}

static inline bool EdgesEqual(const Edges &a, const Edges &b)
{
    return a.root_context == b.root_context && a.holder == b.holder &&
        a.submitted == b.submitted && a.submitted_bytes == b.submitted_bytes &&
        a.context == b.context && a.terminal == b.terminal &&
        a.delivery == b.delivery && a.returned == b.returned;
}

struct Ledger {
    uint32_t flags = 0;
    uint32_t bank0 = 0;
    uint32_t bitmap = 0;
    uint32_t stride = 0;
    uint32_t geometry = 0;
    uint32_t bank_span = 0;
    uint32_t bank_count = 0;
};

static inline bool ReadLedger(const Io &io, Result *result, Ledger *ledger)
{
    static const unsigned indices[] = {32U, 49U, 50U, 51U, 52U, 85U, 86U};
    if (!ledger) return Reject(result, Failure::Argument);
    uint32_t *fields[] = {&ledger->flags, &ledger->bank0, &ledger->bitmap,
        &ledger->stride, &ledger->geometry, &ledger->bank_span,
        &ledger->bank_count};
    for (unsigned field = 0; field < 7U; ++field)
        if (!HeldDatum(io, result, LedgerAddress(indices[field]), fields[field]))
            return false;
    return true;
}

static inline bool LedgerMatches(const Ledger &ledger)
{
    return (ledger.flags & 0xffffU) == 0xc800U &&
        ledger.bank0 && !(ledger.bank0 & 4095U) &&
        (ledger.bitmap & 1U) && ledger.stride == 0x0000c000U &&
        ledger.geometry == 0x08030100U && ledger.bank_span == 0x00618000U &&
        (ledger.bank_count & 255U) == 6U;
}

static inline bool LedgerEqual(const Ledger &a, const Ledger &b)
{
    return a.flags == b.flags && a.bank0 == b.bank0 &&
        a.bitmap == b.bitmap && a.stride == b.stride &&
        a.geometry == b.geometry && a.bank_span == b.bank_span &&
        a.bank_count == b.bank_count;
}

static inline bool ReadAliases(const Io &io, Result *result, uint32_t *aliases)
{
    static const uint32_t addresses[] = {kOuterTableAlias, kOuterRecordAlias,
        kOuterDeliveryAlias, kOuterReturnAlias};
    if (!aliases) return Reject(result, Failure::Argument);
    for (unsigned alias = 0; alias < 4U; ++alias)
        if (!HeldDatum(io, result, addresses[alias], aliases + alias)) return false;
    return true;
}

static inline bool AliasesEqual(const uint32_t *a, const uint32_t *b)
{
    return a && b && a[0] == b[0] && a[1] == b[1] &&
        a[2] == b[2] && a[3] == b[3];
}

static inline bool RecordMatches(const uint32_t *record, const Ledger &ledger,
                                 const Plan &plan, Result *result)
{
    uint32_t chroma = 0;
    if (!record || !CheckedAdd(ledger.bank0, 24576U, &chroma) ||
        !RangeWithin(ledger.bank0, kSourceReadBytes,
                     plan.video_base, plan.video_bytes) ||
        (record[0] & 0x100U) || record[1] != ledger.bank0 ||
        record[2] != chroma || record[3] != 256U || record[4] != 96U ||
        record[13] != 5U)
        return Reject(result, Failure::Record);
    result->bank0 = ledger.bank0;
    result->chroma = chroma;
    result->raw_pts = record[13];
    return true;
}

static inline bool ReadQueues(const Io &io, Result *result, uint32_t delivery,
                              uint32_t returned, uint32_t *queues)
{
    return queues && HeldMemory(io, result, delivery, queues, 8U) &&
        HeldMemory(io, result, returned, queues + 2, 8U);
}

static inline bool QueuesMatch(const uint32_t *queues)
{
    return queues && queues[0] == 62U && queues[1] == 62U &&
        queues[2] == 62U && queues[3] == 62U;
}

static inline bool SourceMatches(const uint32_t *words, Result *result)
{
    if (!words) return Reject(result, Failure::Argument);
    for (unsigned index = 0; index < kSourceWords; ++index) {
        uint32_t expected = 0;
        const int known = KnownSourceWord(index, &expected);
        if (known < 0 || (known && words[index] != expected))
            return Reject(result, Failure::Source);
    }
    result->source_valid_bytes = kSourceValidBytes;
    return true;
}

static inline bool Run(const Plan &plan, const Io &io, uint32_t *source_words,
                       size_t source_word_count, Result *result)
{
    uint32_t allocation = 0, delivery = 0, returned = 0, slot0_record = 0;
    if (!result) return false;
    *result = Result{};
    if (!source_words || source_word_count != kSourceWords ||
        !io.opaque || !io.valid || !io.read_register || !io.write_register ||
        !io.read_memory || !io.pause ||
        !ValidatePlan(plan, &allocation, &delivery, &returned, &slot0_record) ||
        !Alive(io, result)) return Reject(result, Failure::Argument);
    result->allocation = allocation;
    result->delivery_ring = delivery;
    result->return_ring = returned;
    result->record = 0;
    result->record_slot = kRecordCount;

    uint32_t identity = 0, status = 0, debug = 0;
    if (!Guard(io, result, 0U)) return false;
    result->route_select_attempted = true;
    if (!RegisterWrite(io, result, kRoute, 1U) || !Guard(io, result, 1U))
        return false;
    result->route_selected = true;
    if (!Datum(io, result, kStatus, &status) || (status & kHalt) ||
        !Datum(io, result, kIdentity, &identity) || identity != 0x7b010108U ||
        !Datum(io, result, kDebug, &debug) ||
        (debug & (kOtherDebug | kPendingLoad)))
        return Reject(result, Failure::Identity);
    result->halt_attempted = true;
    if (!RegisterWrite(io, result, kDebug, 2U)) return false;
    for (unsigned poll = 0; poll < 8U; ++poll) {
        if (!Datum(io, result, kStatus, &status) ||
            !Datum(io, result, kDebug, &debug)) return false;
        ++result->halt_polls;
        if (debug & (kOtherDebug | kPendingLoad))
            return Reject(result, Failure::Halt);
        if (status & kHalt) {
            result->halted_status = status;
            result->halt_verified = true;
            break;
        }
        if (io.pause(io.opaque) || !Alive(io, result))
            return Reject(result, Failure::Access);
    }
    if (!result->halt_verified) return Reject(result, Failure::Halt);

    Edges before, after;
    if (!ReadEdges(io, result, plan, &before) ||
        !EdgesMatch(before, plan, delivery, returned))
        return Reject(result, Failure::Graph);
    uint32_t aliases[4] = {}, aliases_after[4] = {};
    if (!ReadAliases(io, result, aliases)) return false;
    if (aliases[0] != allocation || aliases[2] != delivery || aliases[3] != returned)
        return Reject(result, Failure::Alias);
    const uint32_t record_delta = aliases[1] - slot0_record;
    if (aliases[1] < slot0_record || record_delta % kRecordStride ||
        record_delta / kRecordStride >= kRecordCount)
        return Reject(result, Failure::Alias);
    const uint32_t record_slot = record_delta / kRecordStride;
    uint32_t record_address = 0;
    if (!CheckedAdd(slot0_record, record_slot * kRecordStride, &record_address) ||
        record_address != aliases[1] ||
        !RangeWithin(record_address, kRecordBytes, allocation, kPoolBytes))
        return Reject(result, Failure::Alias);
    result->record = record_address;
    result->record_slot = record_slot;

    Ledger ledger_before, ledger_after;
    uint32_t queues_before[4] = {}, queues_after[4] = {};
    uint32_t record_before[kRecordBytes / 4U] = {};
    uint32_t record_after[kRecordBytes / 4U] = {};
    if (!ReadLedger(io, result, &ledger_before) ||
        !LedgerMatches(ledger_before)) return Reject(result, Failure::Ledger);
    if (!ReadQueues(io, result, delivery, returned, queues_before) ||
        !QueuesMatch(queues_before)) return Reject(result, Failure::Record);
    result->delivery_read = queues_before[0];
    result->delivery_write = queues_before[1];
    result->return_read = queues_before[2];
    result->return_write = queues_before[3];
    if (!HeldMemory(io, result, record_address, record_before, kRecordBytes) ||
        !RecordMatches(record_before, ledger_before, plan, result) ||
        !HeldMemory(io, result, ledger_before.bank0, source_words,
                    kSourceReadBytes) ||
        !SourceMatches(source_words, result) ||
        !ReadEdges(io, result, plan, &after) ||
        !EdgesMatch(after, plan, delivery, returned) ||
        !EdgesEqual(before, after) ||
        !ReadAliases(io, result, aliases_after) ||
        !AliasesEqual(aliases, aliases_after))
        return Reject(result, Failure::Graph);
    if (!ReadLedger(io, result, &ledger_after) ||
        !LedgerEqual(ledger_before, ledger_after))
        return Reject(result, Failure::Ledger);
    if (!ReadQueues(io, result, delivery, returned, queues_after) ||
        !QueuesMatch(queues_after) ||
        queues_before[0] != queues_after[0] || queues_before[1] != queues_after[1] ||
        queues_before[2] != queues_after[2] || queues_before[3] != queues_after[3])
        return Reject(result, Failure::Record);
    if (!HeldMemory(io, result, record_address, record_after, kRecordBytes) ||
        std::memcmp(record_before, record_after, sizeof(record_before)))
        return Reject(result, Failure::Record);

    if (!Held(io, result) || !Datum(io, result, kIdentity, &identity) ||
        identity != 0x7b010108U || !Datum(io, result, kDebug, &debug) ||
        (debug & (kOtherDebug | kPendingLoad)) ||
        !Datum(io, result, kStatus, &status) || status != result->halted_status ||
        !(status & kHalt)) return Reject(result, Failure::Resume);
    result->resume_attempted = true;
    if (!RegisterWrite(io, result, kStatus, status & ~kHalt)) return false;
    for (unsigned poll = 0; poll < 8U; ++poll) {
        if (!Datum(io, result, kStatus, &status)) return false;
        ++result->resume_polls;
        if (!(status & kHalt)) { result->resume_verified = true; break; }
        if (io.pause(io.opaque) || !Alive(io, result))
            return Reject(result, Failure::Access);
    }
    if (!result->resume_verified) return Reject(result, Failure::Resume);
    result->route_restore_attempted = true;
    if (!RegisterWrite(io, result, kRoute, 0U) || !Guard(io, result, 0U))
        return Reject(result, Failure::Resume);
    result->route_restored = true;
    return result->failure == Failure::None && result->register_writes == 4U &&
        result->source_valid_bytes == kSourceValidBytes;
}

}  // namespace issue204_active_context

#endif
