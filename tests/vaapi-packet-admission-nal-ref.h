// SPDX-License-Identifier: GPL-2.0-or-later
/* OWN diagnostic Annex B header census, not a slice decoder or AU validator.
 * No allocations, payload copies, hashes, writes or external product changes.
 * Types/ref_idc are the H.264 NAL header fields; only ordinary slices1/5
 * participate. Detectable framing/escape truncation stays UNKNOWN. Entropy
 * completeness, B-picture classification and decoding validity are unproved. */
#ifndef CRYSTALHD_PACKET_ADMISSION_NAL_REF_H
#define CRYSTALHD_PACKET_ADMISSION_NAL_REF_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CRYSTALHD_NAL_REF_MAX_BYTES (4U * 1024U * 1024U)
#define CRYSTALHD_NAL_REF_MAX_NALS 256U
#define CRYSTALHD_NAL_REF_MAX_EXTRADATA 65536U
#define CRYSTALHD_NAL_REF_MAX_SIDE_DATA 64U
enum crystalhd_nal_ref_class { CRYSTALHD_NAL_UNKNOWN, CRYSTALHD_NAL_ALL_ZERO,
    CRYSTALHD_NAL_ALL_REF, CRYSTALHD_NAL_MIXED };
enum crystalhd_nal_ref_reason { CRYSTALHD_NAL_OK, CRYSTALHD_NAL_NO_DATA,
    CRYSTALHD_NAL_INVALID_SIZE, CRYSTALHD_NAL_BYTE_CAP, CRYSTALHD_NAL_NOT_ANNEX_B,
    CRYSTALHD_NAL_TRUNCATED, CRYSTALHD_NAL_FORBIDDEN_BIT, CRYSTALHD_NAL_UNSUPPORTED_TYPE,
    CRYSTALHD_NAL_NAL_CAP, CRYSTALHD_NAL_NO_VCL, CRYSTALHD_NAL_BAD_ESCAPE,
    CRYSTALHD_NAL_IDR_REF_ZERO, CRYSTALHD_NAL_CONTEXT_AVCC, CRYSTALHD_NAL_CONTEXT_MISSING,
    CRYSTALHD_NAL_CONTEXT_AMBIGUOUS, CRYSTALHD_NAL_NEW_EXTRADATA,
    CRYSTALHD_NAL_CONTEXT_TRANSITION, CRYSTALHD_NAL_SIDE_DATA_INVALID,
    CRYSTALHD_NAL_OPTIONS_UNAVAILABLE, CRYSTALHD_NAL_OPTIONS_QUERY_FAILED,
    CRYSTALHD_NAL_OPTIONS_NONZERO };
struct crystalhd_nal_ref {
    enum crystalhd_nal_ref_class classification;
    enum crystalhd_nal_ref_reason reason;
    bool framing_complete, context_annex_b;
    uint32_t bytes_scanned, nal_count, vcl_count;
    uint32_t vcl_ref_idc_counts[4], nal_type_counts[32];
};
static inline const char *crystalhd_nal_ref_class_name(enum crystalhd_nal_ref_class value)
{
    switch (value) {
    case CRYSTALHD_NAL_ALL_ZERO: return "ALL_VCL_ZERO";
    case CRYSTALHD_NAL_ALL_REF: return "ALL_VCL_REF";
    case CRYSTALHD_NAL_MIXED: return "MIXED";
    default: return "UNKNOWN";
    }
}
static inline const char *crystalhd_nal_ref_reason_name(enum crystalhd_nal_ref_reason value)
{
    switch (value) {
    case CRYSTALHD_NAL_OK: return "NONE";
    case CRYSTALHD_NAL_NO_DATA: return "NO_DATA";
    case CRYSTALHD_NAL_INVALID_SIZE: return "INVALID_SIZE";
    case CRYSTALHD_NAL_BYTE_CAP: return "BYTE_CAP";
    case CRYSTALHD_NAL_NOT_ANNEX_B: return "NOT_ANNEX_B";
    case CRYSTALHD_NAL_TRUNCATED: return "TRUNCATED_NAL";
    case CRYSTALHD_NAL_FORBIDDEN_BIT: return "FORBIDDEN_BIT";
    case CRYSTALHD_NAL_UNSUPPORTED_TYPE: return "UNSUPPORTED_TYPE";
    case CRYSTALHD_NAL_NAL_CAP: return "NAL_CAP";
    case CRYSTALHD_NAL_NO_VCL: return "NO_VCL";
    case CRYSTALHD_NAL_BAD_ESCAPE: return "BAD_ESCAPE";
    case CRYSTALHD_NAL_IDR_REF_ZERO: return "IDR_REF_ZERO";
    case CRYSTALHD_NAL_CONTEXT_AVCC: return "CONTEXT_AVCC";
    case CRYSTALHD_NAL_CONTEXT_MISSING: return "CONTEXT_MISSING";
    case CRYSTALHD_NAL_CONTEXT_AMBIGUOUS: return "CONTEXT_AMBIGUOUS";
    case CRYSTALHD_NAL_NEW_EXTRADATA: return "NEW_EXTRADATA";
    case CRYSTALHD_NAL_CONTEXT_TRANSITION: return "CONTEXT_TRANSITION";
    case CRYSTALHD_NAL_SIDE_DATA_INVALID: return "SIDE_DATA_INVALID";
    case CRYSTALHD_NAL_OPTIONS_UNAVAILABLE: return "OPTIONS_UNAVAILABLE";
    case CRYSTALHD_NAL_OPTIONS_QUERY_FAILED: return "OPTIONS_QUERY_FAILED";
    case CRYSTALHD_NAL_OPTIONS_NONZERO: return "OPTIONS_NONZERO";
    }
    return "INVALID_REASON";
}
static inline uint8_t crystalhd_nal_ref_byte(const uint8_t *data, size_t index,
                                            struct crystalhd_nal_ref *result)
{
    if (result->bytes_scanned < index + 1U) result->bytes_scanned = (uint32_t)(index + 1U);
    return data[index];
}
static inline bool crystalhd_nal_ref_start(const uint8_t *data, size_t bytes, size_t from,
    size_t *prefix, size_t *header, struct crystalhd_nal_ref *result)
{
    size_t zeros = 0;
    for (size_t i = from; i < bytes; ++i) {
        const uint8_t value = crystalhd_nal_ref_byte(data, i, result);
        if (value == 0) { ++zeros; continue; }
        if (value == 1 && zeros >= 2) {
            *prefix = i - (zeros >= 3 ? 3U : 2U);
            *header = i + 1U;
            return true;
        }
        zeros = 0;
    }
    return false;
}
/* Internal framing census; packet bytes alone never establish Annex B mode. */
static inline struct crystalhd_nal_ref crystalhd_nal_ref_annexb(const uint8_t *data, int size)
{
    struct crystalhd_nal_ref result = {0};
    result.reason = CRYSTALHD_NAL_NO_DATA;
    if (size < 0) { result.reason = CRYSTALHD_NAL_INVALID_SIZE; return result; }
    if ((unsigned)size > CRYSTALHD_NAL_REF_MAX_BYTES) {
        result.reason = CRYSTALHD_NAL_BYTE_CAP; return result;
    }
    if (!data || !size) return result;
    const size_t bytes = (size_t)size;
    size_t prefix = 0, header = 0;
    if (!crystalhd_nal_ref_start(data, bytes, 0, &prefix, &header, &result)) {
        result.reason = CRYSTALHD_NAL_NOT_ANNEX_B; return result;
    }
    for (size_t i = 0; i < prefix; ++i)
        if (crystalhd_nal_ref_byte(data, i, &result) != 0) {
            result.reason = CRYSTALHD_NAL_NOT_ANNEX_B; return result;
        }
    for (;;) {
        if (result.nal_count == CRYSTALHD_NAL_REF_MAX_NALS) {
            result.reason = CRYSTALHD_NAL_NAL_CAP; return result;
        }
        size_t next_prefix = 0, next_header = 0;
        const bool next = crystalhd_nal_ref_start(data, bytes, header,
                                                 &next_prefix, &next_header, &result);
        size_t end = next ? next_prefix : bytes;
        while (end > header && crystalhd_nal_ref_byte(data, end - 1U, &result) == 0) --end;
        if (end <= header || end - header < 2) {
            result.reason = CRYSTALHD_NAL_TRUNCATED; return result;
        }
        const uint8_t value = crystalhd_nal_ref_byte(data, header, &result);
        if (value & 0x80) { result.reason = CRYSTALHD_NAL_FORBIDDEN_BIT; return result; }
        const unsigned type = value & 31U, ref = (value >> 5) & 3U;
        ++result.nal_count;
        ++result.nal_type_counts[type];
        if (type == 1 || type == 5) {
            ++result.vcl_count; ++result.vcl_ref_idc_counts[ref];
            if (type == 5 && ref == 0) { result.reason = CRYSTALHD_NAL_IDR_REF_ZERO; return result; }
        } else if (type < 6 || type > 12) {
            result.reason = CRYSTALHD_NAL_UNSUPPORTED_TYPE; return result;
        }
        unsigned zeros = 0;
        for (size_t i = header + 1U; i < end; ++i) {
            const uint8_t byte = crystalhd_nal_ref_byte(data, i, &result);
            if (zeros >= 2) {
                if (byte == 3) {
                    if (i + 1U == end || crystalhd_nal_ref_byte(data, i + 1U, &result) > 3) {
                        result.reason = CRYSTALHD_NAL_BAD_ESCAPE; return result;
                    }
                    zeros = 0; continue;
                }
                if (byte <= 2) { result.reason = CRYSTALHD_NAL_BAD_ESCAPE; return result; }
            }
            zeros = byte == 0 ? zeros + 1U : 0;
        }
        if (!next) break;
        header = next_header;
    }
    result.framing_complete = true;
    if (!result.vcl_count) { result.reason = CRYSTALHD_NAL_NO_VCL; return result; }
    result.reason = CRYSTALHD_NAL_OK;
    result.classification = result.vcl_ref_idc_counts[0] == result.vcl_count
        ? CRYSTALHD_NAL_ALL_ZERO : result.vcl_ref_idc_counts[0] == 0
        ? CRYSTALHD_NAL_ALL_REF : CRYSTALHD_NAL_MIXED;
    return result;
}
static inline enum crystalhd_nal_ref_reason crystalhd_nal_ref_context(const uint8_t *extra, int bytes)
{
    if (bytes < 0 || (unsigned)bytes > CRYSTALHD_NAL_REF_MAX_EXTRADATA)
        return CRYSTALHD_NAL_CONTEXT_AMBIGUOUS;
    if (!extra || !bytes) return CRYSTALHD_NAL_CONTEXT_MISSING;
    if (extra[0] == 1) return CRYSTALHD_NAL_CONTEXT_AVCC;
    const struct crystalhd_nal_ref r=crystalhd_nal_ref_annexb(extra,bytes);
    if (!r.framing_complete || r.reason!=CRYSTALHD_NAL_NO_VCL ||
        !r.nal_type_counts[7] || !r.nal_type_counts[8]) return CRYSTALHD_NAL_CONTEXT_AMBIGUOUS;
    return CRYSTALHD_NAL_OK;
}
static inline struct crystalhd_nal_ref crystalhd_nal_ref_packet(const uint8_t *data, int size,
    const uint8_t *extra, int extra_bytes, enum crystalhd_nal_ref_reason authority)
{
    struct crystalhd_nal_ref result={0};
    result.reason=authority;
    if (authority!=CRYSTALHD_NAL_OK) return result;
    result.reason=crystalhd_nal_ref_context(extra,extra_bytes);
    if (result.reason!=CRYSTALHD_NAL_OK) return result;
    result=crystalhd_nal_ref_annexb(data,size);
    result.context_annex_b=true;
    return result;
}
#endif
