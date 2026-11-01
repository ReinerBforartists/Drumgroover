// Lane vectors for the TR-909 hand clap lane batches: NL independent runs of the same circuit in lock step, one AVX2 register per four lanes.
// The primitives (value and mask types, arithmetic, selection, exp, tanh) are carried over from the 808 hand clap lane batch (tools/_sweep/hc_cpp/engine/808HcLaneBatch.h, measured there:
// 4 lanes 3.3x, 8 lanes 3.9x over scalar runs one after the other, 16 lanes no further gain). Available only in a build with AVX2 code generation (__AVX2__, MSVC /arch:AVX2):
// 256-bit code mixed with legacy-SSE code costs transition penalties on some CPUs (measured 5x slower than scalar there), so callers use the scalar path otherwise.
#pragma once
#if defined(__AVX2__)
#define DGG_LANES_AVAILABLE 1
#include <immintrin.h>
#include <cmath>
namespace dgg { namespace lanes {

constexpr int NL = 8;   // lanes: two AVX2 registers of doubles (4 lanes: 3.3x, 8 lanes: 3.9x over scalar runs one after the other; 16 lanes: no further gain)



// ---- lane value and mask: NL doubles as NL/4 AVX2 registers (unions with a plain array for the per-lane code that is not vectorised).
// Every primitive is a few intrinsics on registers and is forced inline.
#ifdef _MSC_VER
#define HC_FI __forceinline
#else
#define HC_FI inline __attribute__((always_inline))
#endif
static_assert(NL % 4 == 0, "the lane count must be a multiple of 4");
constexpr int KR = NL / 4;
struct LV { union { __m256d r[KR]; double v[NL]; }; };
struct LM { union { __m256d r[KR]; double m[NL]; }; };   // all-ones / all-zeros per lane
#define LANE_LOOP for (int l = 0; l < NL; ++l)
#define LR for (int k = 0; k < KR; ++k)
static HC_FI LV bc(double x) { LV r; const __m256d t = _mm256_set1_pd(x); LR r.r[k] = t; return r; }
static HC_FI LV operator+(LV a, LV b) { LV r; LR r.r[k] = _mm256_add_pd(a.r[k], b.r[k]); return r; }
static HC_FI LV operator-(LV a, LV b) { LV r; LR r.r[k] = _mm256_sub_pd(a.r[k], b.r[k]); return r; }
static HC_FI LV operator*(LV a, LV b) { LV r; LR r.r[k] = _mm256_mul_pd(a.r[k], b.r[k]); return r; }
static HC_FI LV operator/(LV a, LV b) { LV r; LR r.r[k] = _mm256_div_pd(a.r[k], b.r[k]); return r; }
static HC_FI LV operator-(LV a) { LV r; const __m256d z = _mm256_setzero_pd(); LR r.r[k] = _mm256_sub_pd(z, a.r[k]); return r; }
static HC_FI LV operator+(LV a, double b) { LV r; const __m256d t = _mm256_set1_pd(b); LR r.r[k] = _mm256_add_pd(a.r[k], t); return r; }
static HC_FI LV operator+(double a, LV b) { return b + a; }
static HC_FI LV operator-(LV a, double b) { LV r; const __m256d t = _mm256_set1_pd(b); LR r.r[k] = _mm256_sub_pd(a.r[k], t); return r; }
static HC_FI LV operator-(double a, LV b) { LV r; const __m256d t = _mm256_set1_pd(a); LR r.r[k] = _mm256_sub_pd(t, b.r[k]); return r; }
static HC_FI LV operator*(LV a, double b) { LV r; const __m256d t = _mm256_set1_pd(b); LR r.r[k] = _mm256_mul_pd(a.r[k], t); return r; }
static HC_FI LV operator*(double a, LV b) { return b * a; }
static HC_FI LV operator/(LV a, double b) { return a * (1.0 / b); }   // divisions by constants: one reciprocal, vector multiplications
static HC_FI LV operator/(double a, LV b) { LV r; const __m256d t = _mm256_set1_pd(a); LR r.r[k] = _mm256_div_pd(t, b.r[k]); return r; }
static HC_FI LM lt(LV a, double b) { LM r; const __m256d t = _mm256_set1_pd(b); LR r.r[k] = _mm256_cmp_pd(a.r[k], t, _CMP_LT_OQ); return r; }
static HC_FI LM ge(LV a, double b) { LM r; const __m256d t = _mm256_set1_pd(b); LR r.r[k] = _mm256_cmp_pd(a.r[k], t, _CMP_GE_OQ); return r; }
static HC_FI LM gt(LV a, double b) { LM r; const __m256d t = _mm256_set1_pd(b); LR r.r[k] = _mm256_cmp_pd(a.r[k], t, _CMP_GT_OQ); return r; }
static HC_FI LM maskAll() { LM r; const __m256d z = _mm256_setzero_pd(); LR r.r[k] = _mm256_cmp_pd(z, z, _CMP_EQ_OQ); return r; }
static HC_FI LV sel(LM m, LV a, LV b) { LV r; LR r.r[k] = _mm256_blendv_pd(b.r[k], a.r[k], m.r[k]); return r; }
static HC_FI LV vabs(LV a) { LV r; const __m256d sg = _mm256_set1_pd(-0.0); LR r.r[k] = _mm256_andnot_pd(sg, a.r[k]); return r; }
static HC_FI LV vmin(LV a, LV b) { LV r; LR r.r[k] = _mm256_min_pd(a.r[k], b.r[k]); return r; }
static HC_FI LV vmax(LV a, LV b) { LV r; LR r.r[k] = _mm256_max_pd(a.r[k], b.r[k]); return r; }
static HC_FI bool anyLane(const LM& m) { int t = 0; LR t |= _mm256_movemask_pd(m.r[k]); return t != 0; }
static HC_FI bool allLane(const LM& m) { int t = 15; LR t &= _mm256_movemask_pd(m.r[k]); return t == 15; }
static HC_FI LV vsqrt(LV a) { LV r; LR r.r[k] = _mm256_sqrt_pd(a.r[k]); return r; }

// exp(x), x clamped to [-708, 709]: x = k ln2 + r with |r| <= 0.347 (two-part ln2), degree-13 Taylor polynomial, 2^k built in the exponent bits.
// Same construction as the scalar engine's FastExp (relative error ~1e-15).
static HC_FI LV vexp(LV x)
{
  LV res;
  const __m256d lo = _mm256_set1_pd(-708.0), hi = _mm256_set1_pd(709.0), l2e = _mm256_set1_pd(1.4426950408889634);
  const __m256d ln2a = _mm256_set1_pd(6.93147180369123816490e-01), ln2b = _mm256_set1_pd(1.90821492927058770002e-10);
  const __m256i bias = _mm256_set1_epi64x(1023);
  LR
  {
    const __m256d xv = _mm256_min_pd(_mm256_max_pd(x.r[k], lo), hi);
    const __m256d kd = _mm256_round_pd(_mm256_mul_pd(xv, l2e), _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
    const __m256d r = _mm256_sub_pd(_mm256_sub_pd(xv, _mm256_mul_pd(kd, ln2a)), _mm256_mul_pd(kd, ln2b));
    __m256d p = _mm256_set1_pd(1.0 / 6227020800.0);
    p = _mm256_add_pd(_mm256_mul_pd(p, r), _mm256_set1_pd(1.0 / 479001600.0));
    p = _mm256_add_pd(_mm256_mul_pd(p, r), _mm256_set1_pd(1.0 / 39916800.0));
    p = _mm256_add_pd(_mm256_mul_pd(p, r), _mm256_set1_pd(1.0 / 3628800.0));
    p = _mm256_add_pd(_mm256_mul_pd(p, r), _mm256_set1_pd(1.0 / 362880.0));
    p = _mm256_add_pd(_mm256_mul_pd(p, r), _mm256_set1_pd(1.0 / 40320.0));
    p = _mm256_add_pd(_mm256_mul_pd(p, r), _mm256_set1_pd(1.0 / 5040.0));
    p = _mm256_add_pd(_mm256_mul_pd(p, r), _mm256_set1_pd(1.0 / 720.0));
    p = _mm256_add_pd(_mm256_mul_pd(p, r), _mm256_set1_pd(1.0 / 120.0));
    p = _mm256_add_pd(_mm256_mul_pd(p, r), _mm256_set1_pd(1.0 / 24.0));
    p = _mm256_add_pd(_mm256_mul_pd(p, r), _mm256_set1_pd(1.0 / 6.0));
    p = _mm256_add_pd(_mm256_mul_pd(p, r), _mm256_set1_pd(0.5));
    p = _mm256_add_pd(_mm256_mul_pd(p, r), _mm256_set1_pd(1.0));
    p = _mm256_add_pd(_mm256_mul_pd(p, r), _mm256_set1_pd(1.0));
    const __m256i k64 = _mm256_slli_epi64(_mm256_add_epi64(_mm256_cvtepi32_epi64(_mm256_cvtpd_epi32(kd)), bias), 52);
    res.r[k] = _mm256_mul_pd(p, _mm256_castsi256_pd(k64));
  }
  return res;
}
static HC_FI LV vtanh(LV x)
{
  const LV e = vexp(vabs(x) * 2.0);   // tanh(|x|) = (e^(2|x|) - 1) / (e^(2|x|) + 1); 2|x| beyond 709 gives exactly 1
  const LV t = (e - 1.0) / (e + 1.0);
  return sel(lt(x, 0.0), -t, t);
}


}}  // namespace dgg::lanes
#else
#define DGG_LANES_AVAILABLE 0
#endif
