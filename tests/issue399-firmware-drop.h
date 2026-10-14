/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CRYSTALHD_TESTS_ISSUE399_FIRMWARE_DROP_H
#define CRYSTALHD_TESTS_ISSUE399_FIRMWARE_DROP_H

#include <bc_dts_types.h>
#include <bc_dts_defs.h>
#include <glib.h>

#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace issue399_firmware_drop {

/* This is an exact issue #399 experiment, not a generic H.264 parser or a new
 * libcrystalhd contract.  The payload digests pin the two generated fixtures.
 */
static const unsigned kPictures = 900;
static const unsigned kRequestedDrops = 435;
// Every frozen native/scaled/paced receipt control produced two EOS records.
static const unsigned kExpectedEosControls = 2;
static const unsigned kWidth = 1920;
static const unsigned kHeight = 1080;
static const uint32_t kYDoneWords = 1036840;
static const uint32_t kUVDoneWords = 0;
static const uint64_t kDmaDoneBytes =
    static_cast<uint64_t>(kYDoneWords + kUVDoneWords) * 4U;
static const uint64_t kTokenStep = 100000;

enum class Profile { None, NoB, ReferenceB };
enum class PictureKind { P, B, I };
enum class Failure {
    None, Argument, Payload, Packet, Classification, Token, Api,
    Geometry, Dma, DropProgress, DeliveredSet, PendingSet, Eos,
    ReceiptCount
};

inline const char *FailureName(Failure failure)
{
    static const char *const names[] = {
        "none", "argument", "payload-identity", "annexb-packet",
        "picture-classification", "token", "drop-api", "geometry",
        "full-size-dma", "n-drop-sequence", "delivered-token-set",
        "pending-drop-set", "eos-barrier", "receipt-count"
    };
    const size_t index = static_cast<size_t>(failure);
    return index < sizeof(names) / sizeof(names[0]) ? names[index] : "unknown";
}

inline const char *ProfileName(Profile profile)
{
    return profile == Profile::NoB ? "no-b" :
        profile == Profile::ReferenceB ? "reference-b" : "none";
}

inline bool CheckedAdd(size_t value, size_t add, size_t *result)
{
    if (!result || add > SIZE_MAX - value) return false;
    *result = value + add;
    return true;
}

class Digest {
public:
    Digest() : checksum_(g_checksum_new(G_CHECKSUM_SHA256)) {}
    Digest(const Digest &) = delete;
    Digest &operator=(const Digest &) = delete;
    ~Digest() { if (checksum_) g_checksum_free(checksum_); }

    bool valid() const { return checksum_ != nullptr; }

    bool Add(const uint8_t *data, size_t size)
    {
        size_t next = 0;
        if (sealed_ || !checksum_ || (!data && size) ||
            !CheckedAdd(bytes_, size, &next)) return false;
        if (size) g_checksum_update(checksum_, data, size);
        bytes_ = next;
        return true;
    }

    bool Finish(size_t *bytes, std::string *hex)
    {
        if (sealed_ || !checksum_ || !bytes || !hex) return false;
        const char *value = g_checksum_get_string(checksum_);
        if (!value || std::strlen(value) != 64) return false;
        try { *hex = value; } catch (...) { return false; }
        *bytes = bytes_;
        sealed_ = true;
        return true;
    }

private:
    GChecksum *checksum_ = nullptr;
    size_t bytes_ = 0;
    bool sealed_ = false;
};

struct PacketView {
    const uint8_t *data = nullptr;
    size_t size = 0;
    PacketView() = default;
    PacketView(const uint8_t *bytes, size_t length) : data(bytes), size(length) {}
};

struct Picture {
    PictureKind kind = PictureKind::P;
    unsigned nal_ref_idc = 0;
    Picture() = default;
    Picture(PictureKind picture_kind, unsigned reference) :
        kind(picture_kind), nal_ref_idc(reference) {}
};

class BitReader {
public:
    explicit BitReader(const std::vector<uint8_t> &bytes) : bytes_(bytes) {}

    bool Bit(unsigned *value)
    {
        if (!value || position_ / 8U >= bytes_.size()) return false;
        *value = (bytes_[position_ / 8U] >> (7U - position_ % 8U)) & 1U;
        ++position_;
        return true;
    }

    bool UnsignedExpGolomb(uint32_t *value)
    {
        if (!value) return false;
        unsigned leading = 0, bit = 0;
        while (true) {
            if (!Bit(&bit)) return false;
            if (bit) break;
            if (++leading > 31U) return false;
        }
        uint64_t suffix = 0;
        for (unsigned index = 0; index < leading; ++index) {
            if (!Bit(&bit)) return false;
            suffix = (suffix << 1) | bit;
        }
        const uint64_t decoded = ((UINT64_C(1) << leading) - 1U) + suffix;
        if (decoded > UINT32_MAX) return false;
        *value = static_cast<uint32_t>(decoded);
        return true;
    }

private:
    const std::vector<uint8_t> &bytes_;
    size_t position_ = 0;
};

inline bool StartCodeAt(const uint8_t *data, size_t size, size_t offset,
                        size_t *length)
{
    if (!data || !length || offset > size || size - offset < 3U ||
        data[offset] || data[offset + 1]) return false;
    if (data[offset + 2] == 1) { *length = 3; return true; }
    if (size - offset >= 4U && !data[offset + 2] && data[offset + 3] == 1) {
        *length = 4;
        return true;
    }
    return false;
}

inline bool SliceHeader(const uint8_t *data, size_t size, Picture *picture)
{
    if (!data || size < 2U || !picture || (data[0] & 0x80U)) return false;
    const unsigned nal_type = data[0] & 0x1fU;
    if (nal_type != 1U && nal_type != 5U) return false;
    const unsigned ref = (data[0] >> 5U) & 3U;
    std::vector<uint8_t> rbsp;
    try { rbsp.reserve(size - 1U); } catch (...) { return false; }
    unsigned zeros = 0;
    for (size_t index = 1; index < size; ++index) {
        const uint8_t byte = data[index];
        if (zeros >= 2U && byte == 3U) {
            if (index + 1U >= size || data[index + 1U] > 3U) return false;
            zeros = 0;
            continue;
        }
        if (zeros >= 2U && byte <= 2U) return false;
        try { rbsp.push_back(byte); } catch (...) { return false; }
        zeros = byte == 0 ? zeros + 1U : 0U;
    }
    BitReader bits(rbsp);
    uint32_t first_mb = 0, slice_type = 0;
    if (!bits.UnsignedExpGolomb(&first_mb) || first_mb != 0U ||
        !bits.UnsignedExpGolomb(&slice_type) || slice_type > 9U) return false;
    switch (slice_type % 5U) {
    case 0: picture->kind = PictureKind::P; break;
    case 1: picture->kind = PictureKind::B; break;
    case 2: picture->kind = PictureKind::I; break;
    default: return false; // The two pinned fixtures contain no SP/SI slices.
    }
    if ((nal_type == 5U && picture->kind != PictureKind::I) ||
        (nal_type == 5U && !ref)) return false;
    picture->nal_ref_idc = ref;
    return true;
}

inline bool ClassifyPacket(const PacketView &packet, Picture *picture)
{
    if (!packet.data || !packet.size || !picture) return false;
    size_t cursor = 0, vcl = 0;
    Picture found;
    while (cursor < packet.size) {
        size_t start = 0;
        if (!StartCodeAt(packet.data, packet.size, cursor, &start)) return false;
        const size_t header = cursor + start;
        if (header >= packet.size) return false;
        size_t next = packet.size;
        for (size_t scan = header + 1U; scan < packet.size; ++scan) {
            size_t candidate = 0;
            if (StartCodeAt(packet.data, packet.size, scan, &candidate)) {
                next = scan;
                break;
            }
        }
        if (packet.data[header] & 0x80U) return false;
        const unsigned type = packet.data[header] & 0x1fU;
        if (type == 1U || type == 5U) {
            size_t nal_end = next;
            while (nal_end > header + 1U && !packet.data[nal_end - 1U])
                --nal_end; // Annex-B trailing_zero_8bits is outside the NAL.
            Picture current;
            if (!SliceHeader(packet.data + header, nal_end - header, &current) ||
                ++vcl != 1U) return false;
            found = current;
        }
        cursor = next;
    }
    if (vcl != 1U) return false;
    *picture = found;
    return true;
}

struct Plan {
    Profile profile = Profile::None;
    std::set<uint64_t> all_tokens;
    std::set<uint64_t> expected_drop_tokens;
    std::string expected_drop_digest;
    unsigned i = 0, p = 0, reference_b = 0, nonreference_b = 0;
};

inline bool ExpectedDelivered(const Plan &plan, unsigned total,
                              unsigned *delivered)
{
    if (!delivered || plan.all_tokens.size() != total ||
        plan.expected_drop_tokens.size() > total) return false;
    *delivered = total -
        static_cast<unsigned>(plan.expected_drop_tokens.size());
    return true;
}

inline bool TokenDigest(const std::set<uint64_t> &tokens, std::string *digest)
{
    if (!digest) return false;
    Digest checksum;
    for (uint64_t token : tokens) {
        char line[32];
        const int length = std::snprintf(line, sizeof(line), "%llu\n",
            static_cast<unsigned long long>(token));
        if (length <= 0 || static_cast<size_t>(length) >= sizeof(line) ||
            !checksum.Add(reinterpret_cast<const uint8_t *>(line),
                          static_cast<size_t>(length))) return false;
    }
    size_t bytes = 0;
    return checksum.Finish(&bytes, digest);
}

inline bool BuildPlan(const std::vector<PacketView> &packets, Profile profile,
                      Plan *plan)
{
    if (!plan || profile == Profile::None || packets.size() != kPictures) return false;
    Plan next;
    next.profile = profile;
    try {
        for (size_t index = 0; index < packets.size(); ++index) {
            Picture picture;
            if (!ClassifyPacket(packets[index], &picture) ||
                index + 1U > UINT64_MAX / kTokenStep) return false;
            const uint64_t token = static_cast<uint64_t>(index + 1U) * kTokenStep;
            if (!next.all_tokens.insert(token).second) return false;
            if (picture.kind == PictureKind::I) {
                if (picture.nal_ref_idc != 3U) return false;
                ++next.i;
            } else if (picture.kind == PictureKind::P) {
                if (picture.nal_ref_idc != 2U) return false;
                ++next.p;
            } else if (!picture.nal_ref_idc) {
                ++next.nonreference_b;
                next.expected_drop_tokens.insert(token);
            } else {
                if (picture.nal_ref_idc != 1U) return false;
                ++next.reference_b;
            }
        }
    } catch (...) { return false; }
    const bool counts = profile == Profile::NoB
        ? next.i == 15U && next.p == 885U && !next.reference_b &&
          !next.nonreference_b && next.expected_drop_tokens.empty()
        : next.i == 15U && next.p == 225U && next.reference_b == 225U &&
          next.nonreference_b == kRequestedDrops &&
          next.expected_drop_tokens.size() == kRequestedDrops;
    if (!counts || !TokenDigest(next.expected_drop_tokens,
                                &next.expected_drop_digest)) return false;
    static const char empty_digest[] =
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    static const char reference_b_digest[] =
        "dcbd2e38e91f5bdcdda2d680474ef9bd0ed9cc1bcdfe206ea67e11ce5bb5a72b";
    if (next.expected_drop_digest !=
        (profile == Profile::NoB ? empty_digest : reference_b_digest)) return false;
    try { *plan = next; } catch (...) { return false; }
    return true;
}

inline bool ExactPayload(size_t raw_bytes, const std::string &raw_sha,
                         size_t submitted_bytes,
                         const std::string &submitted_sha, Profile *profile)
{
    if (!profile || raw_bytes != submitted_bytes || raw_sha != submitted_sha) return false;
    if (raw_bytes == 22727167U &&
        raw_sha == "ed043bbb681fc9d0feb0412100fc732f867d912b348e7fd1056efeb02aac48b8") {
        *profile = Profile::NoB;
        return true;
    }
    if (raw_bytes == 23586770U &&
        raw_sha == "69bf3f0ae2a9508076ad4fc89e5b2b06444ddfa2a1c29116a0b0766dee31a8ec") {
        *profile = Profile::ReferenceB;
        return true;
    }
    return false;
}

class Oracle {
public:
    explicit Oracle(const Plan &plan) : plan_(plan), previous_drop_(kRequestedDrops) {}

    template <class Drop>
    bool Issue(HANDLE handle, bool started, bool capturing, Drop drop)
    {
        if (failure_ != Failure::None || issued_ || !handle || !started ||
            !capturing) return Fail(Failure::Argument);
        issued_ = true;
        api_status_ = drop(handle, kRequestedDrops);
        return api_status_ == BC_STS_SUCCESS || Fail(Failure::Api);
    }

    bool Observe(uint64_t token, uint32_t n_drop, unsigned width,
                 unsigned height, uint32_t y_done_words,
                 uint32_t uv_done_words,
                 uint8_t b422_mode = OUTPUT_MODE422_YUY2)
    {
        if (failure_ != Failure::None || !issued_) return Fail(Failure::Token);
        if (marker_receipts_) return Fail(Failure::Eos);
        if (!plan_.all_tokens.count(token) ||
            plan_.expected_drop_tokens.count(token) || delivered_.count(token))
            return Fail(Failure::Token);
        if (width != kWidth || height != kHeight ||
            b422_mode != OUTPUT_MODE422_YUY2)
            return Fail(Failure::Geometry);
        if (y_done_words != kYDoneWords || uv_done_words != kUVDoneWords)
            return Fail(Failure::Dma);
        try {
            if (!delivered_.insert(token).second) return Fail(Failure::Token);
        } catch (...) { return Fail(Failure::Token); }
        char line[64];
        const int length = std::snprintf(line, sizeof(line), "%llu\t%u\n",
            static_cast<unsigned long long>(token), n_drop);
        if (length <= 0 || static_cast<size_t>(length) >= sizeof(line) ||
            !drop_digest_.Add(reinterpret_cast<const uint8_t *>(line),
                              static_cast<size_t>(length)))
            return Fail(Failure::DropProgress);
        if (!have_drop_) {
            first_drop_ = n_drop;
            min_drop_ = n_drop;
            max_drop_ = n_drop;
        }
        else if (n_drop < previous_drop_) ++drop_decreases_;
        else if (n_drop > previous_drop_) ++drop_increases_;
        if (n_drop < min_drop_) min_drop_ = n_drop;
        if (n_drop > max_drop_) max_drop_ = n_drop;
        previous_drop_ = n_drop;
        have_drop_ = true;
        return true;
    }

    bool ObserveMarker(uint32_t n_drop, bool pib_valid)
    {
        if (failure_ != Failure::None || !issued_ || marker_receipts_)
            return Fail(Failure::Eos);
        ++marker_receipts_;
        if (!pib_valid) return true;
        marker_drop_ = n_drop;
        marker_drop_observed_ = true;
        return true;
    }

    bool PendingSatisfied(const std::set<uint64_t> &pending) const
    {
        return pending == plan_.expected_drop_tokens;
    }

    bool FinishNative(const std::set<uint64_t> &pending, unsigned frames,
                      bool eos, uint32_t ready)
    {
        if (failure_ != Failure::None) return false;
        if (!issued_) return Fail(Failure::Argument);
        if (!have_drop_) return Fail(Failure::DeliveredSet);
        if (!eos || ready) return Fail(Failure::Eos);
        const size_t expected = plan_.all_tokens.size() -
            plan_.expected_drop_tokens.size();
        if (frames != expected || delivered_.size() != expected)
            return Fail(Failure::DeliveredSet);
        std::set<uint64_t> missing;
        try {
            for (uint64_t token : plan_.all_tokens)
                if (!delivered_.count(token)) missing.insert(token);
        } catch (...) { return Fail(Failure::DeliveredSet); }
        if (missing != plan_.expected_drop_tokens)
            return Fail(Failure::DeliveredSet);
        if (!PendingSatisfied(pending)) return Fail(Failure::PendingSet);
        // n_drop belongs to the acquired picture snapshot copied into this
        // PIB, not to a global post-EOS state.  Outer static evidence shows
        // that it may be incremented on a non-delivery path or replaced from
        // another descriptor on a normal path.  Preserve the exact sequence
        // as telemetry; direction, initial value and terminal value are not
        // native PASS conditions until the two fixtures establish semantics.
        size_t digest_bytes = 0;
        if (!drop_digest_.Finish(&digest_bytes, &drop_digest_hex_) ||
            !digest_bytes) return Fail(Failure::DropProgress);
        native_complete_ = true;
        return true;
    }

    bool FinishReceipts(size_t host, size_t joined, bool dma_audit_ok,
                        size_t driver_records, size_t eos_controls,
                        size_t full_size_dma, uint64_t full_size_dma_bytes,
                        size_t non_output_full_size_dma,
                        size_t partial_size_dma)
    {
        if (failure_ != Failure::None) return false;
        if (!native_complete_) return Fail(Failure::Eos);
        const size_t expected = delivered_.size() + marker_receipts_;
        if (host != expected || joined != expected)
            return Fail(Failure::ReceiptCount);
        if (!dma_audit_ok ||
            driver_records != delivered_.size() + kExpectedEosControls ||
            eos_controls != kExpectedEosControls ||
            full_size_dma != delivered_.size() ||
            non_output_full_size_dma || partial_size_dma ||
            delivered_.size() > UINT64_MAX / kDmaDoneBytes ||
            full_size_dma_bytes != delivered_.size() * kDmaDoneBytes)
            return Fail(Failure::Dma);
        receipts_complete_ = true;
        return true;
    }

    Profile profile() const { return plan_.profile; }
    bool valid() const { return drop_digest_.valid(); }
    size_t delivered() const { return delivered_.size(); }
    size_t expected_drops() const { return plan_.expected_drop_tokens.size(); }
    uint32_t first_drop() const { return first_drop_; }
    uint32_t last_picture_drop() const { return previous_drop_; }
    uint32_t min_drop() const { return min_drop_; }
    uint32_t max_drop() const { return max_drop_; }
    unsigned drop_decreases() const { return drop_decreases_; }
    unsigned drop_increases() const { return drop_increases_; }
    const std::string &drop_digest() const { return drop_digest_hex_; }
    size_t marker_receipts() const { return marker_receipts_; }
    bool marker_drop_observed() const { return marker_drop_observed_; }
    uint32_t marker_drop() const { return marker_drop_; }
    bool issued() const { return issued_; }
    BC_STATUS api_status() const { return api_status_; }
    Failure failure() const { return failure_; }
    bool complete() const { return native_complete_ && receipts_complete_ &&
        failure_ == Failure::None; }

private:
    bool Fail(Failure failure)
    {
        if (failure_ == Failure::None) failure_ = failure;
        return false;
    }

    const Plan &plan_;
    std::set<uint64_t> delivered_;
    Digest drop_digest_;
    std::string drop_digest_hex_;
    uint32_t first_drop_ = 0;
    uint32_t previous_drop_;
    uint32_t min_drop_ = 0;
    uint32_t max_drop_ = 0;
    uint32_t marker_drop_ = 0;
    unsigned drop_decreases_ = 0, drop_increases_ = 0;
    size_t marker_receipts_ = 0;
    bool issued_ = false, have_drop_ = false;
    bool marker_drop_observed_ = false;
    bool native_complete_ = false, receipts_complete_ = false;
    BC_STATUS api_status_ = BC_STS_SUCCESS;
    Failure failure_ = Failure::None;
};

} // namespace issue399_firmware_drop

#endif
