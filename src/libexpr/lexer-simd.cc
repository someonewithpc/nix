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

/* With VBMI one byte permute looks up a whole class byte, one bit per mask,
   for all 64 bytes of a block at once. The table comes from the scalar
   backend, so the two cannot disagree */
namespace avx512 {

enum : uint8_t {
    BSpace = 1 << 0,
    BIdent = 1 << 1,
    BPath = 1 << 2,
    BDigit = 1 << 3,
    BStrStop = 1 << 4,
    BIndStop = 1 << 5,
    BNewline = 1 << 6,
    BStar = 1 << 7,
};

struct alignas(64) Table
{
    uint8_t bytes[128];
};

/* Filled by `supported`, which `pickClassifier` and `allClassifiers` call
   before handing out `classify` */
static Table table;

static bool fillTable()
{
    auto & t = table;
    for (unsigned c = 0; c < 128; c++) {
        unsigned char block[64];
        for (auto & b : block)
            b = (unsigned char) c;
        Masks m;
        scalar::classify(block, m);
        t.bytes[c] = (m.space & 1 ? BSpace : 0) | (m.ident & 1 ? BIdent : 0) | (m.path & 1 ? BPath : 0)
                     | (m.digit & 1 ? BDigit : 0) | (m.strStop & 1 ? BStrStop : 0) | (m.indStop & 1 ? BIndStop : 0)
                     | (m.newline & 1 ? BNewline : 0) | (m.star & 1 ? BStar : 0);
    }
    return true;
}

#  if defined(__clang__)
#    pragma clang attribute push(__attribute__((target("avx512f,avx512bw,avx512vbmi"))), apply_to = function)
#  else
#    pragma GCC push_options
#    pragma GCC target("avx512f,avx512bw,avx512vbmi")
#  endif

void classify(const unsigned char * block, Masks & out)
{
    __m512i x = _mm512_loadu_si512(block);
    __m512i lo = _mm512_load_si512(table.bytes);
    __m512i hi = _mm512_load_si512(table.bytes + 64);
    /* Bytes from 0x80 up are in no class */
    __mmask64 ascii = ~_mm512_movepi8_mask(x);
    __m512i cls = _mm512_maskz_permutex2var_epi8(ascii, lo, x, hi);
    auto bit = [&](uint8_t b) { return uint64_t(_mm512_test_epi8_mask(cls, _mm512_set1_epi8(char(b)))); };
    out.space = bit(BSpace);
    out.ident = bit(BIdent);
    out.path = bit(BPath);
    out.digit = bit(BDigit);
    out.strStop = bit(BStrStop);
    out.indStop = bit(BIndStop);
    out.newline = bit(BNewline);
    out.star = bit(BStar);
}

#  if defined(__clang__)
#    pragma clang attribute pop
#  else
#    pragma GCC pop_options
#  endif

bool supported()
{
    static const bool ok = __builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw")
                           && __builtin_cpu_supports("avx512vbmi") && fillTable();
    return ok;
}

} // namespace avx512

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
    if (avx512::supported())
        return avx512::classify;
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
    if (avx512::supported())
        all.emplace_back("avx512", avx512::classify);
#elif defined(NIX_LEXER_NEON)
    all.emplace_back("neon", neon::classify);
#endif
    return all;
}

} // namespace nix::lexer::simd
