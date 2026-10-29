// SIMD types and operations used by FullFast2.h on every platform.
//
// SSE2 operations come from <emmintrin.h> on x86-64 (all compilers). The AVX operations are used with MSVC only, chosen at run time by
// FF2DetectAvx(). Everywhere else (ARM, other compilers) the same operations are provided as scalar stand-ins that perform the identical
// additions and multiplications in the identical order, so the results are bitwise equal to the SSE2 path.
#pragma once
#if (defined(_M_X64) || defined(__x86_64__)) && !defined(FF2_NO_SIMD)   // FF2_NO_SIMD: force the scalar stand-ins (tests)
#define FF2_SSE2 1
#else
#define FF2_SSE2 0
#endif
#if defined(_MSC_VER) && defined(_M_X64) && !defined(FF2_NO_SIMD)
#define FF2_AVX 1
#else
#define FF2_AVX 0
#endif

#if FF2_SSE2
#include <emmintrin.h>
#endif
#if FF2_AVX
#include <immintrin.h>
#include <intrin.h>
#endif

#if !FF2_SSE2
struct FF2M128d { double v[2]; };
using __m128d = FF2M128d;
inline __m128d _mm_setzero_pd() { return {{0.0, 0.0}}; }
inline __m128d _mm_set1_pd(double a) { return {{a, a}}; }
inline __m128d _mm_loadu_pd(const double* p) { return {{p[0], p[1]}}; }
inline void _mm_storeu_pd(double* p, __m128d a) { p[0] = a.v[0]; p[1] = a.v[1]; }
inline __m128d _mm_add_pd(__m128d a, __m128d b) { return {{a.v[0] + b.v[0], a.v[1] + b.v[1]}}; }
inline __m128d _mm_sub_pd(__m128d a, __m128d b) { return {{a.v[0] - b.v[0], a.v[1] - b.v[1]}}; }
inline __m128d _mm_mul_pd(__m128d a, __m128d b) { return {{a.v[0] * b.v[0], a.v[1] * b.v[1]}}; }
#endif

#if !FF2_AVX
struct FF2M256d { double v[4]; };
using __m256d = FF2M256d;
inline __m256d _mm256_setzero_pd() { return {{0.0, 0.0, 0.0, 0.0}}; }
inline __m256d _mm256_set1_pd(double a) { return {{a, a, a, a}}; }
inline __m256d _mm256_loadu_pd(const double* p) { return {{p[0], p[1], p[2], p[3]}}; }
inline void _mm256_storeu_pd(double* p, __m256d a) { for (int i = 0; i < 4; ++i) p[i] = a.v[i]; }
inline __m256d _mm256_add_pd(__m256d a, __m256d b) { return {{a.v[0] + b.v[0], a.v[1] + b.v[1], a.v[2] + b.v[2], a.v[3] + b.v[3]}}; }
inline __m256d _mm256_sub_pd(__m256d a, __m256d b) { return {{a.v[0] - b.v[0], a.v[1] - b.v[1], a.v[2] - b.v[2], a.v[3] - b.v[3]}}; }
inline __m256d _mm256_mul_pd(__m256d a, __m256d b) { return {{a.v[0] * b.v[0], a.v[1] * b.v[1], a.v[2] * b.v[2], a.v[3] * b.v[3]}}; }
inline __m128d _mm256_castpd256_pd128(__m256d a) { return _mm_loadu_pd(a.v); }
inline __m128d _mm256_extractf128_pd(__m256d a, int) { return _mm_loadu_pd(a.v + 2); }
inline void _mm256_zeroupper() {}
#endif

// True when the AVX kernels may run (MSVC x64 build, the CPU and the operating system support AVX).
inline bool FF2DetectAvx()
{
#if FF2_AVX
  int r[4]; __cpuid(r, 1); const bool osx = (r[2] & (1 << 27)) != 0, avx = (r[2] & (1 << 28)) != 0; if (!osx || !avx) return false;
  return (_xgetbv(0) & 6) == 6;
#else
  return false;
#endif
}
