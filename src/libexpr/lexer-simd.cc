#include "lexer-simd.hh"

#if defined(__x86_64__) || defined(__i386__)
#  include <immintrin.h>
#  define NIX_LEXER_X86 1
#elif defined(__aarch64__)
#  include <arm_neon.h>
#  define NIX_LEXER_NEON 1
#endif

namespace nix::lexer::simd {

/* Each backend is a vector type with these static members:
     width          bytes per vector
     load(p)        load `width` bytes, unaligned
     splat(c)       every lane `c`
     eq(v, c)       0xff where the lane is `c`
     inRange(v, lo, hi)
                    0xff where `lo <= lane <= hi`, unsigned
     or_(a, b)
     mask(v)        bit `i` set where lane `i` is 0xff
*/

namespace scalar {

struct V
{
    static constexpr unsigned width = 1;
    unsigned char v;

    static V load(const unsigned char * p)
    {
        return {*p};
    }

    static V splat(unsigned char c)
    {
        return {c};
    }

    static V eq(V a, unsigned char c)
    {
        return {static_cast<unsigned char>(a.v == c ? 0xff : 0)};
    }

    static V inRange(V a, unsigned char lo, unsigned char hi)
    {
        return {static_cast<unsigned char>(a.v >= lo && a.v <= hi ? 0xff : 0)};
    }

    static V or_(V a, V b)
    {
        return {static_cast<unsigned char>(a.v | b.v)};
    }

    static uint64_t mask(V a)
    {
        return a.v & 1;
    }
};

#include "lexer-simd-generic.hh"

void classify(const unsigned char * block, Masks & out)
{
    classifyWith<V>(block, out);
}

} // namespace scalar

#ifdef NIX_LEXER_X86

namespace sse2 {

/* Baseline on x86-64 */
struct V
{
    static constexpr unsigned width = 16;
    __m128i v;

    static V load(const unsigned char * p)
    {
        return {_mm_loadu_si128(reinterpret_cast<const __m128i *>(p))};
    }

    static V splat(unsigned char c)
    {
        return {_mm_set1_epi8(char(c))};
    }

    static V eq(V a, unsigned char c)
    {
        return {_mm_cmpeq_epi8(a.v, _mm_set1_epi8(char(c)))};
    }

    static V inRange(V a, unsigned char lo, unsigned char hi)
    {
        __m128i d = _mm_sub_epi8(a.v, _mm_set1_epi8(char(lo)));
        return {_mm_cmpeq_epi8(_mm_min_epu8(d, _mm_set1_epi8(char(hi - lo))), d)};
    }

    static V or_(V a, V b)
    {
        return {_mm_or_si128(a.v, b.v)};
    }

    static uint64_t mask(V a)
    {
        return uint32_t(_mm_movemask_epi8(a.v));
    }
};

#  include "lexer-simd-generic.hh"

void classify(const unsigned char * block, Masks & out)
{
    classifyWith<V>(block, out);
}

} // namespace sse2

#  if defined(__clang__)
#    pragma clang attribute push(__attribute__((target("avx2"))), apply_to = function)
#  else
#    pragma GCC push_options
#    pragma GCC target("avx2")
#  endif

namespace avx2 {

struct V
{
    static constexpr unsigned width = 32;
    __m256i v;

    static V load(const unsigned char * p)
    {
        return {_mm256_loadu_si256(reinterpret_cast<const __m256i *>(p))};
    }

    static V splat(unsigned char c)
    {
        return {_mm256_set1_epi8(char(c))};
    }

    static V eq(V a, unsigned char c)
    {
        return {_mm256_cmpeq_epi8(a.v, _mm256_set1_epi8(char(c)))};
    }

    static V inRange(V a, unsigned char lo, unsigned char hi)
    {
        __m256i d = _mm256_sub_epi8(a.v, _mm256_set1_epi8(char(lo)));
        return {_mm256_cmpeq_epi8(_mm256_min_epu8(d, _mm256_set1_epi8(char(hi - lo))), d)};
    }

    static V or_(V a, V b)
    {
        return {_mm256_or_si256(a.v, b.v)};
    }

    static uint64_t mask(V a)
    {
        return uint32_t(_mm256_movemask_epi8(a.v));
    }
};

#  include "lexer-simd-generic.hh"

void classify(const unsigned char * block, Masks & out)
{
    classifyWith<V>(block, out);
}

} // namespace avx2

#  if defined(__clang__)
#    pragma clang attribute pop
#  else
#    pragma GCC pop_options
#  endif

#endif

#ifdef NIX_LEXER_NEON

namespace neon {

/* Baseline on AArch64 */
struct V
{
    static constexpr unsigned width = 16;
    uint8x16_t v;

    static V load(const unsigned char * p)
    {
        return {vld1q_u8(p)};
    }

    static V splat(unsigned char c)
    {
        return {vdupq_n_u8(c)};
    }

    static V eq(V a, unsigned char c)
    {
        return {vceqq_u8(a.v, vdupq_n_u8(c))};
    }

    static V inRange(V a, unsigned char lo, unsigned char hi)
    {
        return {vcleq_u8(vsubq_u8(a.v, vdupq_n_u8(lo)), vdupq_n_u8(uint8_t(hi - lo)))};
    }

    static V or_(V a, V b)
    {
        return {vorrq_u8(a.v, b.v)};
    }

    static uint64_t mask(V a)
    {
        /* No movemask on NEON: keep one bit per lane, weighted by its
           position, and add up each half */
        static const uint8_t weights[16] = {1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128};
        uint8x16_t bits = vandq_u8(a.v, vld1q_u8(weights));
        return vaddv_u8(vget_low_u8(bits)) | (uint64_t(vaddv_u8(vget_high_u8(bits))) << 8);
    }
};

#  include "lexer-simd-generic.hh"

void classify(const unsigned char * block, Masks & out)
{
    classifyWith<V>(block, out);
}

} // namespace neon

#endif

Classifier pickClassifier()
{
#ifdef NIX_LEXER_X86
    if (__builtin_cpu_supports("avx2"))
        return avx2::classify;
    return sse2::classify;
#elif defined(NIX_LEXER_NEON)
    return neon::classify;
#else
    return scalar::classify;
#endif
}

std::vector<std::pair<const char *, Classifier>> allClassifiers()
{
    std::vector<std::pair<const char *, Classifier>> all{{"scalar", scalar::classify}};
#ifdef NIX_LEXER_X86
    all.emplace_back("sse2", sse2::classify);
    if (__builtin_cpu_supports("avx2"))
        all.emplace_back("avx2", avx2::classify);
#elif defined(NIX_LEXER_NEON)
    all.emplace_back("neon", neon::classify);
#endif
    return all;
}

} // namespace nix::lexer::simd
