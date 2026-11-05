// SPDX-License-Identifier: LGPL-2.1-or-later
// Production parser source-unit regression; no device, worker or firmware.
// Keep the original scalar algorithms below independent of the fast search.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>
#include "../linux_lib/libcrystalhd/libcrystalhd_parser.cpp"

using Bytes = std::vector<uint8_t>;
static unsigned failures, cases;
static void check(bool value, const char *message)
{
    if (!value) {
        if (failures < 20) std::fprintf(stderr, "FAIL case %u: %s\n", cases, message);
        ++failures;
    }
}

static unsigned scalar_prefix(const uint8_t *data, uint32_t size)
{
    if (size >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1)
        return 4;
    if (size >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1)
        return 3;
    return 0;
}
static uint32_t scalar_next(const uint8_t *data, uint32_t size, uint32_t from)
{
    if (from >= size) return size;
    while (from < size && !scalar_prefix(data + from, size - from)) ++from;
    return from;
}
static bool scalar_initial(const uint8_t *data, uint32_t size, uint32_t *start)
{
    if (!data || !start) return false;
    uint32_t one = 0;
    while (one < size && data[one] == 0) ++one;
    if (one >= size || data[one] != 1 || one < 2) return false;
    const uint32_t prefix = one >= 3 ? 4 : 3;
    *start = one - (prefix - 1);
    return true;
}
static BC_STATUS scalar_convert(const uint8_t *data, uint32_t size, bool divx,
                               uint8_t *output, uint32_t capacity, uint32_t *out_size)
{
    if (divx) {
        uint32_t position = 0;
        while (position < size && data[position] == 0) ++position;
        if (position >= 2 && position < size && data[position] == 1) {
            ++position;
            for (;;) {
                if (position == size) return BC_STS_INV_ARG;
                ++position;
                while (position < size && !(size - position >= 3 &&
                       data[position] == 0 && data[position + 1] == 0 &&
                       data[position + 2] == 1)) ++position;
                if (position == size) break;
                position += 3;
            }
            if (output) {
                if (capacity < size) return BC_STS_INV_ARG;
                std::memcpy(output, data, size);
            }
            *out_size = size;
            return BC_STS_SUCCESS;
        }
    }
    uint32_t position = 0, written = 0;
    const bool annex = scalar_initial(data, size, &position);
    while (position < size) {
        uint32_t begin, length;
        if (annex) {
            const unsigned prefix = scalar_prefix(data + position, size - position);
            if (!prefix) return BC_STS_INV_ARG;
            begin = position + prefix;
            position = scalar_next(data, size, begin);
            length = position - begin;
        } else {
            if (size - position < 2) return BC_STS_INV_ARG;
            length = (static_cast<uint32_t>(data[position]) << 8) | data[position + 1];
            position += 2;
            begin = position;
            if (length > size - position) return BC_STS_INV_ARG;
            position += length;
        }
        if (!length) return BC_STS_INV_ARG;
        const unsigned type = data[begin] & 0x1f;
        if (!divx && type != 7 && type != 8) continue;
        if (written > UINT32_MAX - 4 || length > UINT32_MAX - 4 - written)
            return BC_STS_INV_ARG;
        if (output) {
            if (written > capacity || capacity - written < 4 ||
                length > capacity - written - 4) return BC_STS_INV_ARG;
            output[written] = output[written + 1] = output[written + 2] = 0;
            output[written + 3] = 1;
            std::memcpy(output + written + 4, data + begin, length);
        }
        written += 4 + length;
    }
    *out_size = written;
    return BC_STS_SUCCESS;
}
static bool scalar_fragment(const uint8_t *data, uint32_t size)
{
    uint32_t position = 0;
    if (!scalar_initial(data, size, &position)) return false;
    for (;;) {
        const unsigned prefix = scalar_prefix(data + position, size - position);
        if (!prefix || size - position <= prefix) return false;
        const uint32_t begin = position + prefix;
        const unsigned type = data[begin] & 0x1f;
        position = scalar_next(data, size, begin + 1);
        if (position != size) continue;
        if (type == NALU_TYPE_SPS || type == NALU_TYPE_PPS) return false;
        return data[size - 1] != 0;
    }
}

static void compare(const uint8_t *data, uint32_t size)
{
    ++cases;
    if (size <= 24) {
        for (uint32_t from = 0; from <= size + 1; ++from)
            check(DtsNextSequenceStartCode(data, size, from) == scalar_next(data, size, from),
                  "NEXT prefix position/three-versus-four precedence");
    } else {
        for (uint32_t from : {0U, 1U, 2U, size / 2, size - 3, size - 2,
                              size - 1, size, size + 1})
            check(DtsNextSequenceStartCode(data, size, from) == scalar_next(data, size, from),
                  "bounded NEXT prefix search on long input");
    }
    check(DtsFragmentConfigBoundarySafe(data, size) == scalar_fragment(data, size),
          "fragment SPS/PPS/trailing-zero cache boundary contract");
    for (bool divx : {false, true}) {
        uint32_t wanted = 0xa5a5a5a5U, actual = wanted;
        const BC_STATUS expected = scalar_convert(data, size, divx, nullptr, 0, &wanted);
        check(DtsConvertSequenceHeaders(data, size, divx, nullptr, 0, &actual) == expected &&
                  actual == wanted, "count-pass status and output-size publication");
        const uint32_t enough = expected == BC_STS_SUCCESS ? wanted : size + 8;
        for (uint32_t capacity : {0U, enough ? enough - 1 : 0U, enough}) {
            Bytes reference(capacity + 16U, 0xa5), output = reference;
            uint32_t reference_size = 0x5a5a5a5aU, output_size = reference_size;
            const BC_STATUS status = scalar_convert(data, size, divx,
                                                    reference.data(), capacity, &reference_size);
            check(DtsConvertSequenceHeaders(data, size, divx, output.data(),
                      capacity, &output_size) == status && output_size == reference_size &&
                      output == reference, "actual converted bytes, capacity errors and untouched guards");
        }
    }
}

class Guarded {
    void *mapping_;
    size_t mapped_;
public:
    uint8_t *data;
    Guarded(const Bytes &input, bool at_end, bool readonly) {
        const long queried = sysconf(_SC_PAGESIZE);
        if (queried <= 0) std::abort();
        const size_t page = static_cast<size_t>(queried);
        const size_t span = std::max(page, ((input.size() + page - 1) / page) * page);
        mapped_ = span + 2 * page;
        mapping_ = mmap(nullptr, mapped_, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapping_ == MAP_FAILED) { std::perror("mmap"); std::exit(2); }
        uint8_t *middle = static_cast<uint8_t *>(mapping_) + page;
        if (mprotect(middle, span, PROT_READ | PROT_WRITE)) std::abort();
        data = at_end ? middle + span - input.size() : middle;
        if (!input.empty()) std::memcpy(data, input.data(), input.size());
        if (readonly && mprotect(middle, span, PROT_READ)) std::abort();
    }
    ~Guarded() { munmap(mapping_, mapped_); }
    Guarded(const Guarded &) = delete;
    Guarded &operator=(const Guarded &) = delete;
};

int main()
{
    const std::vector<Bytes> edges = {
        {}, {0}, {0, 0}, {0, 0, 0}, {0, 0, 1}, {0, 0, 0, 1},
        {0, 0, 1, 0x67}, {0, 0, 0, 1, 0x68}, {0, 0, 0, 0, 0, 1, 0x67, 0},
        {0, 0, 1, 0, 0, 1, 0x68}, {0, 0, 1, 0x67, 0, 0, 0, 1, 0x68, 0x88},
        {0, 0, 1, 0x67, 0, 0, 0, 0, 1, 0x68, 0, 0, 1, 0x65, 0x88},
        {0, 0, 1, 0x67, 0, 0, 1, 0x68, 0, 0, 1, 0x65, 0},
        {0, 0, 0, 1, 0, 0, 0, 1, 0x68}, {0, 2, 0x67, 0x88, 0, 1, 0x68},
        {0, 2, 0x67}, {0, 0}, {0xff, 0xff, 0x67},
    };
    for (const Bytes &input : edges) compare(input.data(), input.size());

    // Exhaust every <=7-byte word of zeros/ones/SPS/PPS header candidates.
    const uint8_t alphabet[] = {0, 1, 0x67, 0x68};
    for (unsigned length = 0; length <= 7; ++length) {
        const uint32_t words = 1U << (2U * length);
        for (uint32_t word = 0; word < words; ++word) {
            Bytes input(length);
            uint32_t digits = word;
            for (uint8_t &byte : input) { byte = alphabet[digits & 3U]; digits >>= 2; }
            compare(input.data(), input.size());
        }
    }
    uint32_t seed = 0x47747001U;
    auto random = [&] { seed = seed * 1664525U + 1013904223U; return seed; };
    for (unsigned iteration = 0; iteration != 4096; ++iteration) {
        Bytes input((random() >> 16) % 513U);
        for (uint8_t &byte : input) {
            const uint32_t value = random() >> 16;
            byte = value % 4U == 0 ? 0 : value % 4U == 1 ? 1 : value & 0xffU;
        }
        if (input.size() >= 4 && iteration % 2 == 0) {
            input[0] = input[1] = 0; input[2] = 1;
            input[3] = iteration % 4 == 0 ? 0x67 : 0x68;
        }
        compare(input.data(), input.size());
    }
    Bytes long_au = {0, 0, 0, 1, 0x67, 0x64, 0, 0x1f,
                     0, 0, 1, 0x68, 0xee, 0x3c, 0, 0, 0, 1, 0x65};
    long_au.resize(55 * 1024, 0x55);
    compare(long_au.data(), long_au.size());
    long_au.insert(long_au.end(), {0, 0, 0, 0, 1, 0x68, 0x99, 0, 0, 1, 0x65, 0x88});
    compare(long_au.data(), long_au.size());

    std::vector<Bytes> guarded = edges;
    guarded.push_back(long_au);
    guarded.push_back(Bytes(2 * 4096 + 3, 0));
    guarded.push_back(Bytes(2 * 4096 + 3, 1));
    for (const Bytes &input : guarded) {
        for (bool at_end : {false, true}) {
            Guarded exact(input, at_end, true);
            compare(exact.data, input.size());
            for (bool divx : {false, true}) {
                uint32_t required = 0;
                if (scalar_convert(exact.data, input.size(), divx, nullptr, 0,
                                   &required) != BC_STS_SUCCESS) continue;
                const Bytes fill(required, 0xa5);
                Guarded reference(fill, true, false), output(fill, true, false);
                uint32_t wanted = 0, actual = 0;
                check(scalar_convert(exact.data, input.size(), divx, reference.data,
                                     required, &wanted) == BC_STS_SUCCESS &&
                      DtsConvertSequenceHeaders(exact.data, input.size(), divx,
                          output.data, required, &actual) == BC_STS_SUCCESS && actual == wanted &&
                      (!required || !std::memcmp(reference.data, output.data, required)),
                      "actual conversion input/output end exactly at inaccessible pages");
            }
        }
    }
    Guarded inaccessible({}, true, true);
    for (uint32_t from : {UINT32_MAX, UINT32_MAX - 1U, UINT32_MAX - 2U})
        check(DtsNextSequenceStartCode(inaccessible.data, UINT32_MAX, from) == UINT32_MAX,
              "uint32 short-tail/from bounds return before address arithmetic or access");
    check(DtsNextSequenceStartCode(nullptr, 0, UINT32_MAX) == 0,
          "from beyond empty input performs no read");
    if (failures) { std::fprintf(stderr, "%u failures in %u parser cases\n", failures, cases); return 1; }
    std::printf("PASS: %u actual parser/scalar framing cases, fragments and guard-page bounds\n", cases);
}
