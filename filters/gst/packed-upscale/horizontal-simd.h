/* Original project-owned, UNBUILT/UNTESTED CPU-only draft.
 * Exact packed YUY2 -> UYVY Q6 two-tap horizontal interpolation.
 * No device or GStreamer object access. Public-coefficient table is supplied
 * by the caller after its own format/colorimetry/admission checks.
 */
#ifndef CRYSTALHD_HORIZONTAL_SIMD_DRAFT_H
#define CRYSTALHD_HORIZONTAL_SIMD_DRAFT_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

enum { CH_H_IN_BYTES = 2560, CH_H_OUT_BYTES = 3840,
       CH_H_GROUP_BYTES = 48, CH_H_GROUP_COUNT = 80 };

typedef struct {
  uint16_t source0, source1;
  uint8_t weight0, weight1;
} ChHorizontalTap2;

typedef struct {
  uint16_t reference_base;
  uint8_t shuffle0[16], shuffle1[16];
  uint8_t weights_lo[16], weights_hi[16];
} ChHorizontalPattern;

typedef struct {
  /* Caller owns this immutable table for the plan's whole lifetime. */
  const ChHorizontalTap2 *table;
  ChHorizontalPattern pattern[3];
  int periodic_valid;
  int use_ssse3;
} ChHorizontalPlan;

#if !defined(CRYSTALHD_LEGACY_CPU) && \
    (defined(__i386__) || defined(__x86_64__)) && \
    (defined(__GNUC__) || defined(__clang__))
#define CH_H_HAVE_SSSE3_DISPATCH 1
#include <tmmintrin.h>
#else
#define CH_H_HAVE_SSSE3_DISPATCH 0
#endif

static inline uint8_t
ch_horizontal_scalar_sample(const uint8_t *source, const ChHorizontalTap2 *tap)
{
  /* Constructor proved nonnegative Q6 weights sum64 and source bounds.
   * Accumulator is in [32,16352], including rounding; no saturation needed.
   */
  unsigned value = (unsigned)source[tap->source0] * tap->weight0 +
      (unsigned)source[tap->source1] * tap->weight1 + 32U;
  return (uint8_t)(value >> 6);
}

static inline void
ch_horizontal_scalar_range(const ChHorizontalPlan *p, const uint8_t *source,
    uint8_t *dest, unsigned first, unsigned end)
{
  for (unsigned i = first; i < end; ++i)
    dest[i] = ch_horizontal_scalar_sample(source, &p->table[i]);
}

/* Returns0 only for an invalid mathematical table. A valid but nonperiodic
 * table remains a supported scalar plan. No floating-point reconstruction,
 * implicit coefficient adjustment, or layout inference occurs here.
 */
static inline int
ch_horizontal_plan_init(ChHorizontalPlan *p, const ChHorizontalTap2 *table,
    unsigned input_bytes, unsigned output_bytes, int enable_simd)
{
  if (!p || !table || input_bytes != CH_H_IN_BYTES ||
      output_bytes != CH_H_OUT_BYTES)
    return 0;
  memset(p, 0, sizeof(*p));
  p->table = table;
  for (unsigned i = 0; i < CH_H_OUT_BYTES; ++i) {
    const ChHorizontalTap2 *t = &table[i];
    if (t->source0 >= input_bytes || t->source1 >= input_bytes ||
        t->weight0 > 64 || t->weight1 > 64 ||
        (unsigned)t->weight0 + t->weight1 != 64)
      return 0;
  }

  /* Each interior group emits48 bytes from a32-byte source advance. Build
   * the three16-output-byte shuffle patterns from group1, then PROVE all
   * groups1..78 use exactly those offsets and weights. Group0 and79 are
   * scalar edges, so neither SIMD loads nor reads assume row padding.
   */
  p->periodic_valid = 1;
  for (unsigned k = 0; k < 3; ++k) {
    ChHorizontalPattern *pat = &p->pattern[k];
    const unsigned out = CH_H_GROUP_BYTES + 16 * k;
    unsigned minimum = CH_H_IN_BYTES, maximum = 0;
    for (unsigned j = 0; j < 16; ++j) {
      const ChHorizontalTap2 *t = &table[out + j];
      if (t->source0 < minimum) minimum = t->source0;
      if (t->source1 < minimum) minimum = t->source1;
      if (t->source0 > maximum) maximum = t->source0;
      if (t->source1 > maximum) maximum = t->source1;
    }
    if (maximum - minimum >= 16 || minimum + 16 > CH_H_IN_BYTES) {
      p->periodic_valid = 0;
      break;
    }
    pat->reference_base = (uint16_t)minimum;
    for (unsigned j = 0; j < 16; ++j) {
      const ChHorizontalTap2 *t = &table[out + j];
      pat->shuffle0[j] = (uint8_t)(t->source0 - minimum);
      pat->shuffle1[j] = (uint8_t)(t->source1 - minimum);
      uint8_t *weights = j < 8 ? pat->weights_lo : pat->weights_hi;
      const unsigned lane = j & 7U;
      weights[2 * lane] = t->weight0;
      weights[2 * lane + 1] = t->weight1;
    }
    for (unsigned g = 1; g + 1 < CH_H_GROUP_COUNT; ++g) {
      const unsigned delta = (g - 1) * 32;
      const unsigned base = minimum + delta;
      if (base + 16 > CH_H_IN_BYTES) {
        p->periodic_valid = 0;
        break;
      }
      for (unsigned j = 0; j < 16; ++j) {
        const ChHorizontalTap2 *t = &table[g * CH_H_GROUP_BYTES + k * 16 + j];
        const ChHorizontalTap2 *ref = &table[out + j];
        if ((unsigned)t->source0 != (unsigned)ref->source0 + delta ||
            (unsigned)t->source1 != (unsigned)ref->source1 + delta ||
            t->weight0 != ref->weight0 || t->weight1 != ref->weight1) {
          p->periodic_valid = 0;
          break;
        }
      }
      if (!p->periodic_valid) break;
    }
    if (!p->periodic_valid) break;
  }
#if CH_H_HAVE_SSSE3_DISPATCH
  /* This ordinary setup branch is the only call into the SSSE3 function.
   * No target-wide -mssse3 flag is permitted. Legacy builds omit intrinsics
   * entirely, honoring their strict i686 instruction baseline.
   */
  if (enable_simd && p->periodic_valid) {
    __builtin_cpu_init();
    p->use_ssse3 = __builtin_cpu_supports("ssse3") != 0;
  }
#else
  (void)enable_simd;
#endif
  return 1;
}

#if CH_H_HAVE_SSSE3_DISPATCH
__attribute__((target("ssse3"), noinline))
static void
ch_horizontal_ssse3(const ChHorizontalPlan *p, const uint8_t *source,
    uint8_t *dest)
{
  const __m128i round = _mm_set1_epi16(32);
  /* All first/last-group samples and stores are scalar, no row overread. */
  ch_horizontal_scalar_range(p, source, dest, 0, CH_H_GROUP_BYTES);
  for (unsigned g = 1; g + 1 < CH_H_GROUP_COUNT; ++g) {
    const unsigned delta = (g - 1) * 32;
    for (unsigned k = 0; k < 3; ++k) {
      const ChHorizontalPattern *pat = &p->pattern[k];
      const __m128i packed = _mm_loadu_si128(
          (const __m128i *)(const void *)(source + pat->reference_base + delta));
      const __m128i s0 = _mm_shuffle_epi8(packed, _mm_loadu_si128(
          (const __m128i *)(const void *)pat->shuffle0));
      const __m128i s1 = _mm_shuffle_epi8(packed, _mm_loadu_si128(
          (const __m128i *)(const void *)pat->shuffle1));
      /* maddubs multiplies unsigned bytes by signed-byte weights then adds
       * adjacent pairs. Weights0..64 fit signed bytes; pair sums<=16320,
       * proving its signed-word saturation is inactive for every input.
       */
      __m128i lo = _mm_maddubs_epi16(_mm_unpacklo_epi8(s0, s1),
          _mm_loadu_si128((const __m128i *)(const void *)pat->weights_lo));
      __m128i hi = _mm_maddubs_epi16(_mm_unpackhi_epi8(s0, s1),
          _mm_loadu_si128((const __m128i *)(const void *)pat->weights_hi));
      lo = _mm_srli_epi16(_mm_add_epi16(lo, round), 6);
      hi = _mm_srli_epi16(_mm_add_epi16(hi, round), 6);
      _mm_storeu_si128((__m128i *)(void *)(dest + g * CH_H_GROUP_BYTES + 16 * k),
          _mm_packus_epi16(lo, hi));
    }
  }
  ch_horizontal_scalar_range(p, source, dest,
      (CH_H_GROUP_COUNT - 1) * CH_H_GROUP_BYTES, CH_H_OUT_BYTES);
}
#endif

static inline void
ch_horizontal_run(const ChHorizontalPlan *p, const uint8_t *source, uint8_t *dest)
{
  /* Nonoverlapping caller-provided active rows are required. Input stride
   * and output stride are handled by the caller; this reads exactly within
   *2560 active input bytes and writes exactly3840 output bytes.
   */
#if CH_H_HAVE_SSSE3_DISPATCH
  if (p->use_ssse3) {
    ch_horizontal_ssse3(p, source, dest);
    return;
  }
#endif
  ch_horizontal_scalar_range(p, source, dest, 0, CH_H_OUT_BYTES);
}

#endif
