/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef CRYSTALHD_LEGACY_H
#define CRYSTALHD_LEGACY_H

/* Forced into every LEGACY_CPU translation unit, including test programs. */
#if !defined(__i386__) || __SIZEOF_POINTER__ != 4
#error "LEGACY_CPU requires the 32-bit i686 ABI"
#endif
#if defined(__SSE__) || defined(__SSE2__) || defined(__MMX__) || \
    defined(__AVX__) || defined(__AVX2__) || defined(__BMI__) || \
    defined(__BMI2__) || defined(__POPCNT__) || defined(__LZCNT__) || \
    defined(__AES__) || defined(__PCLMUL__) || defined(__RDRND__) || \
    defined(__RDSEED__) || defined(__ADX__)
#error "LEGACY_CPU must not enable instructions beyond the i686 baseline"
#endif
#define CRYSTALHD_LEGACY_CPU 1

#endif
