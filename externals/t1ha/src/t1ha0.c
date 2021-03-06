/*
 *  Copyright (c) 2016-2017 Positive Technologies, https://www.ptsecurity.com,
 *  Fast Positive Hash.
 *
 *  Portions Copyright (c) 2010-2017 Leonid Yuriev <leo@yuriev.ru>,
 *  The 1Hippeus project (t1h).
 *
 *  This software is provided 'as-is', without any express or implied
 *  warranty. In no event will the authors be held liable for any damages
 *  arising from the use of this software.
 *
 *  Permission is granted to anyone to use this software for any purpose,
 *  including commercial applications, and to alter it and redistribute it
 *  freely, subject to the following restrictions:
 *
 *  1. The origin of this software must not be misrepresented; you must not
 *     claim that you wrote the original software. If you use this software
 *     in a product, an acknowledgement in the product documentation would be
 *     appreciated but is not required.
 *  2. Altered source versions must be plainly marked as such, and must not be
 *     misrepresented as being the original software.
 *  3. This notice may not be removed or altered from any source distribution.
 */

/*
 * t1ha = { Fast Positive Hash, aka "Позитивный Хэш" }
 * by [Positive Technologies](https://www.ptsecurity.ru)
 *
 * Briefly, it is a 64-bit Hash Function:
 *  1. Created for 64-bit little-endian platforms, in predominantly for x86_64,
 *     but without penalties could runs on any 64-bit CPU.
 *  2. In most cases up to 15% faster than City64, xxHash, mum-hash, metro-hash
 *     and all others which are not use specific hardware tricks.
 *  3. Not suitable for cryptography.
 *
 * The Future will Positive. Всё будет хорошо.
 *
 * ACKNOWLEDGEMENT:
 * The t1ha was originally developed by Leonid Yuriev (Леонид Юрьев)
 * for The 1Hippeus project - zerocopy messaging in the spirit of Sparta!
 */

#if defined(_MSC_VER) && _MSC_VER > 1800
#pragma warning(disable : 4464) /* relative include path contains '..' */
#endif

#include "../t1ha.h"
#include "t1ha_bits.h"

static __inline uint32_t tail32_le(const void *v, size_t tail) {
  const uint8_t *p = (const uint8_t *)v;
#ifdef can_read_underside
  /* On some systems (e.g. x86) we can perform a 'oneshot' read, which
   * is little bit faster. Thanks Marcin Żukowski <marcin.zukowski@gmail.com>
   * for the reminder. */
  const unsigned offset = (4 - tail) & 3;
  const unsigned shift = offset << 3;
  if (likely(can_read_underside(p, 4))) {
    p -= offset;
    return fetch32_le(p) >> shift;
  }
  return fetch32_le(p) & ((~UINT32_C(0)) >> shift);
#endif /* 'oneshot' read */

  uint32_t r = 0;
  switch (tail & 3) {
#if UNALIGNED_OK && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  /* For most CPUs this code is better when not needed
   * copying for alignment or byte reordering. */
  case 0:
    return fetch32_le(p);
  case 3:
    r = (uint32_t)p[2] << 16;
  /* fall through */
  case 2:
    return r + fetch16_le(p);
  case 1:
    return p[0];
#else
  /* For most CPUs this code is better than a
   * copying for alignment and/or byte reordering. */
  case 0:
    r += p[3];
    r <<= 8;
  /* fall through */
  case 3:
    r += p[2];
    r <<= 8;
  /* fall through */
  case 2:
    r += p[1];
    r <<= 8;
  /* fall through */
  case 1:
    return r + p[0];
#endif
  }
  unreachable();
}

static __inline uint32_t tail32_be(const void *v, size_t tail) {
  const uint8_t *p = (const uint8_t *)v;
#ifdef can_read_underside
  /* On some systems we can perform a 'oneshot' read, which is little bit
   * faster. Thanks Marcin Żukowski <marcin.zukowski@gmail.com> for the
   * reminder. */
  const unsigned offset = (4 - tail) & 3;
  const unsigned shift = offset << 3;
  if (likely(can_read_underside(p, 4))) {
    p -= offset;
    return fetch32_be(p) & ((~UINT32_C(0)) >> shift);
  }
  return fetch32_be(p) >> shift;
#endif /* 'oneshot' read */

  switch (tail & 3) {
#if UNALIGNED_OK && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  /* For most CPUs this code is better when not needed
   * copying for alignment or byte reordering. */
  case 1:
    return p[0];
  case 2:
    return fetch16_be(p);
  case 3:
    return fetch16_be(p) << 8 | p[2];
  case 0:
    return fetch32_be(p);
#else
  /* For most CPUs this code is better than a
   * copying for alignment and/or byte reordering. */
  case 1:
    return p[0];
  case 2:
    return p[1] | (uint32_t)p[0] << 8;
  case 3:
    return p[2] | (uint32_t)p[1] << 8 | (uint32_t)p[0] << 16;
  case 0:
    return p[3] | (uint32_t)p[2] << 8 | (uint32_t)p[1] << 16 |
           (uint32_t)p[0] << 24;
#endif
  }
  unreachable();
}

/***************************************************************************/

#ifndef rot32
static maybe_unused __inline uint32_t rot32(uint32_t v, unsigned s) {
  return (v >> s) | (v << (32 - s));
}
#endif /* rot32 */

static __inline uint64_t remix32(uint32_t a, uint32_t b) {
  a ^= rot32(b, 13);
  uint64_t l = a | (uint64_t)b << 32;
  l *= p0;
  l ^= l >> 41;
  return l;
}

static __inline void mixup32(uint32_t *a, uint32_t *b, uint32_t v, uint32_t p) {
  uint64_t l = mul_32x32_64(*b + v, p);
  *a ^= (uint32_t)l;
  *b += (uint32_t)(l >> 32);
}

/* 32-bit 'magic' primes */
static const uint32_t q0 = UINT32_C(0x92D78269);
static const uint32_t q1 = UINT32_C(0xCA9B4735);
static const uint32_t q2 = UINT32_C(0xA4ABA1C3);
static const uint32_t q3 = UINT32_C(0xF6499843);
static const uint32_t q4 = UINT32_C(0x86F0FD61);
static const uint32_t q5 = UINT32_C(0xCA2DA6FB);
static const uint32_t q6 = UINT32_C(0xC4BB3575);

uint64_t t1ha0_32le(const void *data, size_t len, uint64_t seed) {
  uint32_t a = rot32((uint32_t)len, s1) + (uint32_t)seed;
  uint32_t b = (uint32_t)len ^ (uint32_t)(seed >> 32);

  const int need_align = (((uintptr_t)data) & 3) != 0 && !UNALIGNED_OK;
  uint32_t align[4];

  if (unlikely(len > 16)) {
    uint32_t c = ~a;
    uint32_t d = rot32(b, 5);
    const void *detent = (const uint8_t *)data + len - 15;
    do {
      const uint32_t *v = (const uint32_t *)data;
      if (unlikely(need_align))
        v = (const uint32_t *)memcpy(&align, v, 16);

      uint32_t w0 = fetch32_le(v + 0);
      uint32_t w1 = fetch32_le(v + 1);
      uint32_t w2 = fetch32_le(v + 2);
      uint32_t w3 = fetch32_le(v + 3);

      uint32_t c02 = w0 ^ rot32(w2 + c, 11);
      uint32_t d13 = w1 + rot32(w3 + d, s1);
      c ^= rot32(b + w1, 7);
      d ^= rot32(a + w0, 3);
      b = q1 * (c02 + w3);
      a = q0 * (d13 ^ w2);

      data = (const uint32_t *)data + 4;
    } while (likely(data < detent));

    c += a;
    d += b;
    a ^= q6 * (rot32(c, 16) + d);
    b ^= q5 * (c + rot32(d, 16));

    len &= 15;
  }

  const uint8_t *v = (const uint8_t *)data;
  if (unlikely(need_align) && len > 4)
    v = (const uint8_t *)memcpy(&align, v, len);

  switch (len) {
  default:
    mixup32(&a, &b, fetch32_le(v), q4);
    v += 4;
  /* fall through */
  case 12:
  case 11:
  case 10:
  case 9:
    mixup32(&b, &a, fetch32_le(v), q3);
    v += 4;
  /* fall through */
  case 8:
  case 7:
  case 6:
  case 5:
    mixup32(&a, &b, fetch32_le(v), q2);
    v += 4;
  /* fall through */
  case 4:
  case 3:
  case 2:
  case 1:
    mixup32(&b, &a, tail32_le(v, len), q1);
  /* fall through */
  case 0:
    return remix32(a, b);
  }
}

uint64_t t1ha0_32be(const void *data, size_t len, uint64_t seed) {
  uint32_t a = rot32((uint32_t)len, s1) + (uint32_t)seed;
  uint32_t b = (uint32_t)len ^ (uint32_t)(seed >> 32);

  const int need_align = (((uintptr_t)data) & 3) != 0 && !UNALIGNED_OK;
  uint32_t align[4];

  if (unlikely(len > 16)) {
    uint32_t c = ~a;
    uint32_t d = rot32(b, 5);
    const void *detent = (const uint8_t *)data + len - 15;
    do {
      const uint32_t *v = (const uint32_t *)data;
      if (unlikely(need_align))
        v = (const uint32_t *)memcpy(&align, v, 16);

      uint32_t w0 = fetch32_be(v + 0);
      uint32_t w1 = fetch32_be(v + 1);
      uint32_t w2 = fetch32_be(v + 2);
      uint32_t w3 = fetch32_be(v + 3);

      uint32_t c02 = w0 ^ rot32(w2 + c, 11);
      uint32_t d13 = w1 + rot32(w3 + d, s1);
      c ^= rot32(b + w1, 7);
      d ^= rot32(a + w0, 3);
      b = q1 * (c02 + w3);
      a = q0 * (d13 ^ w2);

      data = (const uint32_t *)data + 4;
    } while (likely(data < detent));

    c += a;
    d += b;
    a ^= q6 * (rot32(c, 16) + d);
    b ^= q5 * (c + rot32(d, 16));

    len &= 15;
  }

  const uint8_t *v = (const uint8_t *)data;
  if (unlikely(need_align) && len > 4)
    v = (const uint8_t *)memcpy(&align, v, len);

  switch (len) {
  default:
    mixup32(&a, &b, fetch32_be(v), q4);
    v += 4;
  /* fall through */
  case 12:
  case 11:
  case 10:
  case 9:
    mixup32(&b, &a, fetch32_be(v), q3);
    v += 4;
  /* fall through */
  case 8:
  case 7:
  case 6:
  case 5:
    mixup32(&a, &b, fetch32_be(v), q2);
    v += 4;
  /* fall through */
  case 4:
  case 3:
  case 2:
  case 1:
    mixup32(&b, &a, tail32_be(v, len), q1);
  /* fall through */
  case 0:
    return remix32(a, b);
  }
}

/***************************************************************************/

#undef T1HA_ia32aes_AVAILABLE
#if defined(__x86_64__) || (defined(_M_IX86) && _MSC_VER > 1800) ||            \
    defined(_M_X64) || defined(i386) || defined(_X86_) || defined(__i386__) || \
    defined(_X86_64_)

static uint64_t x86_cpu_features(void) {
  uint32_t features = 0;
  uint32_t extended = 0;
#ifdef __GNUC__
  uint32_t eax, ebx, ecx, edx;
  const unsigned cpuid_max = __get_cpuid_max(0, NULL);
  if (cpuid_max >= 1) {
    __cpuid_count(1, 0, eax, ebx, features, edx);
    if (cpuid_max >= 7)
      __cpuid_count(7, 0, eax, extended, ecx, edx);
  }
#elif defined(_MSC_VER)
  int info[4];
  __cpuid(info, 0);
  const unsigned cpuid_max = info[0];
  if (cpuid_max >= 1) {
    __cpuidex(info, 1, 0);
    features = info[2];
    if (cpuid_max >= 7) {
      __cpuidex(info, 7, 0);
      extended = info[1];
    }
  }
#endif
  return features | (uint64_t)extended << 32;
}

#define T1HA_ia32aes_AVAILABLE

uint64_t t1ha0_ia32aes_avx(const void *data, size_t len, uint64_t seed);
uint64_t t1ha0_ia32aes_noavx(const void *data, size_t len, uint64_t seed);

#endif /* __i386__ || __x86_64__ */

/***************************************************************************/

static
#if __GNUC_PREREQ(4, 0) || __has_attribute(used)
    __attribute__((used))
#endif
    uint64_t (*t1ha0_resolve(void))(const void *, size_t, uint64_t) {

#ifdef T1HA_ia32aes_AVAILABLE
  uint64_t features = x86_cpu_features();
  if (features & UINT32_C(0x02000000)) {
    if ((features & UINT32_C(0x1A000000)) == UINT32_C(0x1A000000))
      return ((features >> 32) & 32) ? t1ha0_ia32aes_avx2 : t1ha0_ia32aes_avx;
    return t1ha0_ia32aes_noavx;
  }
#endif /* T1HA_ia32aes_AVAILABLE */

  return (sizeof(size_t) >= 8)
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
             ? t1ha1_be
             : t1ha0_32be;
#else
             ? t1ha1_le
             : t1ha0_32le;
#endif
}

#ifdef __ELF__

#if __GNUC_PREREQ(4, 6) || __has_attribute(ifunc)
uint64_t t1ha0(const void *data, size_t len, uint64_t seed)
    __attribute__((ifunc("t1ha0_resolve")));
#else
__asm("\t.globl\tt1ha0\n\t.type\tt1ha0, "
      "@gnu_indirect_function\n\t.set\tt1ha0,t1ha0_resolve");
#endif /* ifunc */

#elif __GNUC_PREREQ(4, 0) || __has_attribute(constructor)

uint64_t (*t1ha0_funcptr)(const void *, size_t, uint64_t);

static void __attribute__((constructor)) t1ha0_init(void) {
  t1ha0_funcptr = t1ha0_resolve();
}

#else /* ELF */

static uint64_t t1ha0_proxy(const void *data, size_t len, uint64_t seed) {
  t1ha0_funcptr = t1ha0_resolve();
  return t1ha0_funcptr(data, len, seed);
}

uint64_t (*t1ha0_funcptr)(const void *, size_t, uint64_t) = t1ha0_proxy;

#endif
