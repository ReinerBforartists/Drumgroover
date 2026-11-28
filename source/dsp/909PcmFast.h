// Reduced fast model of a TR-909 PCM voice analog path (CRASH / RIDE): linear dynamics exact (exponential integrator, piecewise-linear PCM inputs),
// BJT static currents interpolated linearly over the step, Newton on the BJT port voltages (vbe, vbc per device). Constants from gen_crash_model.py.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>
#include <algorithm>
#include "909PcmSteps.h"
#ifdef _MSC_VER
#include <intrin.h>
#endif

#ifdef PCMFAST_PROFILE
#include <chrono>
#define PF_T0 auto pf_t = std::chrono::steady_clock::now()
#define PF_LAP(k) { auto pf_n = std::chrono::steady_clock::now(); prof[k] += std::chrono::duration<double>(pf_n - pf_t).count(); pf_t = pf_n; }
#else
#define PF_T0
#define PF_LAP(k)
#endif

namespace dgg { namespace pcmfast {

constexpr double kDcTol = 1e-9, kDcDamp = 0.2; constexpr int kDcIts = 40;   // DC solve of the idle state: largest port correction at convergence (V; the slowest states need the ports to this accuracy), largest port correction of one iteration (V), iterations
constexpr double kChordMaxCorr = 0.2;   // volts: above this correction the next iteration is a full Newton iteration
// Block-triangular Jacobian (blockRel > 0, hi-hats): at some operating points (a quiet hit after a loud one) the dropped entries matter and the iteration fails step after step. A step that reaches kBlkSwitchIt
// iterations (or fails) is solved again from its start with the dense Jacobian, and so are the following kBlkDenseSteps steps. Not in the first kBlkStartSteps steps of a hit from rest (about 10 ms).
constexpr int kBlkSwitchIt = 30, kBlkDenseSteps = 200; constexpr long kBlkStartSteps = 2000;
// Hi-hats: while the output envelope is below kQuietLevelHat the correction limit is kNewtonTolHatQuiet instead of kNewtonTolHat. With the loose limit alone the quiet tails of hits retriggered on a ringing voice deviate.
constexpr double kQuietLevelHat = 0.05, kNewtonTolHatQuiet = 3e-3;   // volts of the envelope; volts on the port voltages
constexpr double kVtPcm = 0.025864917;   // thermal voltage of the PCM voices: the NGSpice reference at its 27 degC (300.15 K)
constexpr double kVt = 0.025852, kGmin = 1e-12, kNewtonTol = 1e-3, kNewtonTolCym = 3e-3, kNewtonTolHat = 3e-2, kBlockRelHat = 1e-4, kJReuseHat = 100, kDioMemoHat = 1e-9, kTailRel = 0.0, kTailRelCym = 1e-2, kMidRelCym = 0.12, kEnvDecay = 0.975, kAcAlpha = 0.0031, kAcEndLevel = 1e-3, kAcEndRelCym = 1e-3, kAcEndLevelHat = 2e-7; constexpr int kAcEndPeriods = 400;   // volts on the port voltages; the correction is applied before the test, so the error left is of the order of the square

// Gummel-Poon static currents of one BJT in node orientation: ic = s*cc (leaving the collector node into the device), ib = s*cb, and the 2x2 Jacobian wrt (vbe, vbc)
struct BjtOut { double ic, ib, d[2][2]; };
// Approximate evaluation for devices that stay below high injection (cbe/IKF < ~1e-3, checked by the generator) and have either vbc <= 0.1 V (the
// collector junction carries a negligible current: IS*exp(vbc/Vt) < 5e-12 A) or are evaluated with the exact junction term: no division, no sqrt.
// BjtFromExp is the algebra with the exponentials supplied: eE = exp(vbe/(NF Vt)) and eL = exp(vbe/(NE Vt)) (both 0 in deep cutoff), eC = exp(vbc/Vt) (read above vbc = 0.1 V only).
inline BjtOut BjtFromExp(const double* P, double vbe, double vbc, double eE, double eL, double eC)
{
  const double s = P[0], IS = P[1], ISE = P[8], invNFVt = P[9], invNEVt = P[10], invVAF = P[11], invIKF = P[12], invBF = P[13], invBR = P[14];
  const double cbe = IS * (eE - 1.0), gbe = IS * eE * invNFVt;
  double cbc = -IS, gbc = 0.0;
  if (vbc > 0.1) { cbc = IS * (eC - 1.0); gbc = IS * eC * (1.0 / kVtPcm); }
  double cben = kGmin * vbe, gben = kGmin;   // gmin as in ngspice: on the two leakage branches of the base current only
  if (ISE != 0.0) { cben += ISE * (eL - 1.0); gben += ISE * eL * invNEVt; }
  const double iq1 = 1.0 - vbc * invVAF;                         // 1/q1 (Early effect)
  const double q2 = cbe * invIKF, dq2 = gbe * invIKF;
  const double g = 1.0 - q2 + 2.0 * q2 * q2, dg = (-1.0 + 4.0 * q2) * dq2;    // 1/(1 + q2 - q2^2), series in the small q2
  const double iq = iq1 * g, x = cbe - cbc;
  const double cc = x * iq - cbc * invBR - kGmin * vbc, cb = cbe * invBF + cben + cbc * invBR + kGmin * vbc;
  const double dccbe = gbe * iq + x * iq1 * dg, dccbc = -gbc * iq - x * (-invVAF) * 0.0 + x * (-invVAF) * g - gbc * invBR - kGmin;
  const double dcbbe = gbe * invBF + gben, dcbbc = gbc * invBR + kGmin;
  BjtOut o; o.ic = s * cc; o.ib = s * cb; o.d[0][0] = s * dccbe; o.d[0][1] = s * dccbc; o.d[1][0] = s * dcbbe; o.d[1][1] = s * dcbbc;
  return o;
}

inline BjtOut BjtEvalFast(const double* P, double vbe, double vbc)
{
  const double aE = vbe * P[9], aL = vbe * P[10];   // exp(a) < 2.4e-16 for a < -36: the junction is in deep cutoff, the exponential is dropped (change below the rounding of IS)
  const double eE = aE < -36.0 ? 0.0 : std::exp(std::fmin(aE, 80.0));
  const double eC = vbc > 0.1 ? std::exp(std::fmin(vbc * (1.0 / kVtPcm), 80.0)) : 0.0;
  const double eL = P[8] == 0.0 || aL < -36.0 ? 0.0 : std::exp(std::fmin(aL, 80.0));
  return BjtFromExp(P, vbe, vbc, eE, eL, eC);
}

inline BjtOut BjtEval(const double* P, double vbe, double vbc, double vt = kVt)   // vt: kVtPcm for the PCM voices (P[9], P[10] are built with it)
{
  const double s = P[0], IS = P[1], BR = P[3], ISE = P[8], invNFVt = P[9], invNEVt = P[10], invVAF = P[11], invIKF = P[12], invBF = P[13], invBR = P[14];
  const double eE = std::exp(std::fmin(vbe * invNFVt, 80.0)), eC = std::exp(std::fmin(vbc * (1.0 / vt), 80.0));
  const double cbe = IS * (eE - 1.0), gbe = IS * eE * invNFVt;
  const double cbc = IS * (eC - 1.0), gbc = IS * eC * (1.0 / vt);
  double cben = kGmin * vbe, gben = kGmin;
  if (ISE != 0.0) { const double eL = std::exp(std::fmin(vbe * invNEVt, 80.0)); cben += ISE * (eL - 1.0); gben += ISE * eL * invNEVt; }
  const double q1 = 1.0 / (1.0 - vbc * invVAF), dq1 = q1 * q1 * invVAF;
  const double q2 = cbe * invIKF, arg = std::fmax(0.0, 1.0 + 4.0 * q2), sq = std::sqrt(arg), qb = q1 * (1.0 + sq) * 0.5;
  const double dsqbe = arg > 0.0 ? (2.0 * gbe * invIKF) / std::fmax(sq, 1e-12) : 0.0;
  const double dqbe = q1 * 0.5 * dsqbe, dqbc = dq1 * (1.0 + sq) * 0.5;
  const double iq = 1.0 / qb, x = cbe - cbc;
  const double cc = x * iq - cbc * invBR - kGmin * vbc, cb = cbe * invBF + cben + cbc * invBR + kGmin * vbc;
  const double dccbe = gbe * iq - x * iq * iq * dqbe, dccbc = -gbc * iq - x * iq * iq * dqbc - gbc * invBR - kGmin;
  const double dcbbe = gbe * invBF + gben, dcbbc = gbc * invBR + kGmin;
  BjtOut o; o.ic = s * cc; o.ib = s * cb; o.d[0][0] = s * dccbe; o.d[0][1] = s * dccbc; o.d[1][0] = s * dcbbe; o.d[1][1] = s * dcbbc;
  (void)BR;
  return o;
}

// Diode with series resistance (Net909 dioJunction / dioRs): current and conductance at the terminal voltage v; vj is the internal junction voltage (warm start, updated).
// P = {Is, N*Vt, Rs, knee of the breakdown branch}
inline void DioReverse(const double* P, double vd, double& i, double& g)   // between the breakdown knee and -3 N Vt, as NGSpice computes it: -Is (1 + (3 N Vt / (e vd))^3) plus gmin
{
  const double a = 3.0 * P[1] / (2.718281828459045 * vd), a3 = a * a * a; i = -P[0] * (1.0 + a3) + kGmin * vd; g = P[0] * 3.0 * a3 / vd + kGmin;
}
inline void DioJunction(const double* P, double vd, double& i, double& g)
{
  if (vd < -3.0 * P[1] && vd > -P[3]) DioReverse(P, vd, i, g);
  else if (vd > -P[3]) { const double e = std::exp(std::fmin(vd / P[1], 80.0)); i = P[0] * (e - 1.0) + kGmin * vd; g = P[0] * e / P[1] + kGmin; }
  else { const double e = std::exp(std::fmin(-(P[3] + vd) / P[1], 80.0)); i = -P[0] * e + kGmin * vd; g = P[0] * e / P[1] + kGmin; }
}
inline void DioEval(const double* P, double v, double& vj, double& i, double& g)
{
  if (v < -36.0 * P[1] && v > -P[3]) { vj = v; DioReverse(P, v, i, g); return; }   // deep reverse bias (above the breakdown knee): no iteration, the drop over the series resistance is below 1e-8 V
  if (P[2] <= 0.0) { vj = v; DioJunction(P, v, i, g); return; }
  double ii, gg;
  for (int k = 0; k < 60; ++k)
  {
    DioJunction(P, vj, ii, gg);
    double dv = (vj + ii * P[2] - v) / (1.0 + gg * P[2]); const double lim = 6.0 * P[1]; dv = dv > lim ? lim : (dv < -lim ? -lim : dv);
    vj -= dv; if (std::fabs(dv) < 1e-12) break;
  }
  DioJunction(P, vj, ii, gg); i = ii; g = gg / (1.0 + gg * P[2]);
}

// Excess junction charge of one capacitance (state-dependent part beyond the capacitance frozen in the linear model, see gen_crash_model.py CHARGE_BJT):
// dq(v) = (q(v) - q0 - c0 (v - v0)) / unit and its slope dc = (c(v) - c0) / unit. P = {CJ, VJ, M, TF, IS, 1/(NF Vt), v0, c0, q0, beFlag, unit};
// q = depletion charge (SPICE, FC = 0.5) + TF * forward current for the base-emitter junction; unit = coulomb of the channel variable.
inline void QCharge(const double* P, double v, double& dq, double& dc)
{
  const double cj0 = P[0], vj = P[1], m = P[2], fc = 0.5; double qd, cd;
  if (v < fc * vj) { const double t = 1.0 - v / vj, tm = std::pow(t, -m); qd = vj * cj0 / (1.0 - m) * (1.0 - t * tm); cd = cj0 * tm; }
  else
  {
    const double f1 = vj * (1.0 - std::pow(1.0 - fc, 1.0 - m)) / (1.0 - m), f2 = std::pow(1.0 - fc, 1.0 + m), f3 = 1.0 - fc * (1.0 + m);
    qd = cj0 * (f1 + (1.0 / f2) * (f3 * (v - fc * vj) + m / (2.0 * vj) * (v * v - fc * fc * vj * vj))); cd = cj0 / f2 * (f3 + m * v / vj);
  }
  double q = qd, c = cd;
  if (P[9] > 0.5) { const double e = std::exp(std::fmin(v * P[5], 80.0)); q += P[3] * (P[4] * (e - 1.0) + kGmin * v); c += P[3] * (P[4] * e * P[5] + kGmin); }
  dq = (q - P[8] - P[7] * (v - P[6])) / P[10]; dc = (c - P[7]) / P[10];
}

// Cubic Hermite table of the excess-charge channels (HH-K10): per channel dq(v) and its slope dc(v) on a uniform grid; dc is the derivative of the interpolating polynomial. Outside the grid the exact function is used.
template<class M, class = void> struct GateMask { static constexpr uint32_t value = 0; };   // the period in which the mute gate switches runs the full layout
template<class M> struct GateMask<M, std::void_t<decltype(M::kGateBit)>> { static constexpr uint32_t value = 1u << M::kGateBit; };
template <class M> struct QTable
{
  static constexpr double kV0 = -20.0, kV1 = 1.2, kH = 0.005; static constexpr int kN = static_cast<int>((kV1 - kV0) / kH + 0.5) + 1;
  std::vector<double> t;   // per channel and node: dq, dc
  QTable() { constexpr int NQc = QCount<M>::value; t.resize(static_cast<size_t>(NQc > 0 ? NQc : 1) * kN * 2); if constexpr (NQc > 0) for (int j = 0; j < NQc; ++j) for (int i = 0; i < kN; ++i) { double dq, dc; QCharge(&M::kQPar[11 * j], kV0 + i * kH, dq, dc); t[(static_cast<size_t>(j) * kN + i) * 2] = dq; t[(static_cast<size_t>(j) * kN + i) * 2 + 1] = dc; } }
  static const QTable& Get() { static const QTable inst; return inst; }
  void Eval(int j, double v, double& dq, double& dc) const
  {
    double x = (v - kV0) * (1.0 / kH);
    if (x < 0.0 || x >= kN - 1) { QCharge(&M::kQPar[11 * j], v, dq, dc); return; }
    const int i = static_cast<int>(x); const double s = x - i; const double* a = &t[(static_cast<size_t>(j) * kN + i) * 2]; const double q0 = a[0], m0 = a[1] * kH, q1 = a[2], m1 = a[3] * kH;
    const double s2 = s * s, s3 = s2 * s;
    dq = (2 * s3 - 3 * s2 + 1) * q0 + (s3 - 2 * s2 + s) * m0 + (-2 * s3 + 3 * s2) * q1 + (s3 - s2) * m1;
    dc = ((6 * s2 - 6 * s) * q0 + (3 * s2 - 4 * s + 1) * m0 + (-6 * s2 + 6 * s) * q1 + (3 * s2 - 2 * s) * m1) * (1.0 / kH);
  }
};
#ifdef PCMFAST_DOT_AVX2
// The unit that defines PCMFAST_DOT_AVX2 is compiled for the baseline instruction set: the only AVX2 code is that of the intrinsics in Dot, MatVecAdd and ExpBatch, which clear the upper register halves before they return.
#include <immintrin.h>
// exp(x[i]) for N values (a multiple of four), four at a time, equal to std::exp within the rounding of the result; the arguments are limited to [-700, 80] (a NaN gives exp(80), like std::fmin).
// x = n ln 2 + r with |r| <= ln 2 / 2, Taylor polynomial of degree 13 (truncation 4e-18), n added into the exponent field. No table.
template <int N> inline void ExpBatch(const double (&x)[N], double (&e)[N])
{
  static_assert(N % 4 == 0, "ExpBatch works on groups of four");
#define PF_C4(v) _mm256_set1_pd(v)   // a macro: a lambda here gets a symbol outside the namespace of the unit
  const __m256d shift = PF_C4(6755399441055744.0);   // 1.5 * 2^52: the low bits of (x / ln 2 + shift) hold the rounded integer n
  for (int j = 0; j < N; j += 4)
  {
    const __m256d v = _mm256_max_pd(_mm256_min_pd(_mm256_loadu_pd(x + j), PF_C4(80.0)), PF_C4(-700.0));
    const __m256d t = _mm256_fmadd_pd(v, PF_C4(1.4426950408889634), shift), n = _mm256_sub_pd(t, shift);
    const __m256d r = _mm256_fnmadd_pd(n, PF_C4(1.90821492927058770002e-10), _mm256_fnmadd_pd(n, PF_C4(6.93147180369123816490e-01), v));   // ln 2 in two parts
    const __m256d r2 = _mm256_mul_pd(r, r), r4 = _mm256_mul_pd(r2, r2), r8 = _mm256_mul_pd(r4, r4);
    const __m256d p23 = _mm256_fmadd_pd(r, PF_C4(1.0 / 6.0), PF_C4(0.5)), p45 = _mm256_fmadd_pd(r, PF_C4(1.0 / 120.0), PF_C4(1.0 / 24.0)), p67 = _mm256_fmadd_pd(r, PF_C4(1.0 / 5040.0), PF_C4(1.0 / 720.0));
    const __m256d p89 = _mm256_fmadd_pd(r, PF_C4(1.0 / 362880.0), PF_C4(1.0 / 40320.0)), pab = _mm256_fmadd_pd(r, PF_C4(1.0 / 39916800.0), PF_C4(1.0 / 3628800.0)), pcd = _mm256_fmadd_pd(r, PF_C4(1.0 / 6227020800.0), PF_C4(1.0 / 479001600.0));
    const __m256d low = _mm256_fmadd_pd(r4, _mm256_fmadd_pd(r2, p67, p45), _mm256_fmadd_pd(r2, p23, r)), high = _mm256_fmadd_pd(r4, pcd, _mm256_fmadd_pd(r2, pab, p89));
    const __m256d q = _mm256_fmadd_pd(r8, high, low);   // exp(r) - 1
    const __m256d scale = _mm256_castsi256_pd(_mm256_slli_epi64(_mm256_add_epi64(_mm256_castpd_si256(t), _mm256_set1_epi64x(1023)), 52));   // 2^n
    _mm256_storeu_pd(e + j, _mm256_fmadd_pd(scale, q, scale));
  }
#undef PF_C4
  _mm256_zeroupper();
}
inline double Dot(const double* a, const double* b, int n)   // AVX2/FMA dot product, four accumulators over 16 elements per pass
{
  __m256d s0 = _mm256_setzero_pd(), s1 = s0, s2 = s0, s3 = s0; int k = 0;
  for (; k + 16 <= n; k += 16)
  {
    s0 = _mm256_fmadd_pd(_mm256_loadu_pd(a + k), _mm256_loadu_pd(b + k), s0); s1 = _mm256_fmadd_pd(_mm256_loadu_pd(a + k + 4), _mm256_loadu_pd(b + k + 4), s1);
    s2 = _mm256_fmadd_pd(_mm256_loadu_pd(a + k + 8), _mm256_loadu_pd(b + k + 8), s2); s3 = _mm256_fmadd_pd(_mm256_loadu_pd(a + k + 12), _mm256_loadu_pd(b + k + 12), s3);
  }
  for (; k + 4 <= n; k += 4) s0 = _mm256_fmadd_pd(_mm256_loadu_pd(a + k), _mm256_loadu_pd(b + k), s0);
  s0 = _mm256_add_pd(_mm256_add_pd(s0, s1), _mm256_add_pd(s2, s3)); const __m128d h = _mm_add_pd(_mm256_castpd256_pd128(s0), _mm256_extractf128_pd(s0, 1));
  _mm256_zeroupper();
  double r = _mm_cvtsd_f64(_mm_add_sd(h, _mm_unpackhi_pd(h, h)));
  for (; k < n; ++k) r += a[k] * b[k];
  return r;
}
// y += A x for a row-major Rows x Cols matrix: eight (then four, then two) rows at a time share the loads of x and the final horizontal sum; the Cols % 4 rest of a row is read with a masked load.
template <int Rows, int Cols> inline void MatVecAdd(const double* A, const double* x, double* y)
{
  constexpr int C4 = Cols & ~3, CT = Cols - C4;
  const __m256i mask = _mm256_setr_epi64x(CT > 0 ? -1 : 0, CT > 1 ? -1 : 0, CT > 2 ? -1 : 0, 0);
  __m256d xt = _mm256_setzero_pd(); if constexpr (CT != 0) xt = _mm256_maskload_pd(x + C4, mask);
  int i = 0;
  for (; i + 8 <= Rows; i += 8)
  {
    const double* r = A + static_cast<size_t>(i) * Cols;
    __m256d s0 = _mm256_setzero_pd(), s1 = s0, s2 = s0, s3 = s0, s4 = s0, s5 = s0, s6 = s0, s7 = s0;
    for (int k = 0; k < C4; k += 4)
    {
      const __m256d v = _mm256_loadu_pd(x + k);
      s0 = _mm256_fmadd_pd(_mm256_loadu_pd(r + k), v, s0); s1 = _mm256_fmadd_pd(_mm256_loadu_pd(r + Cols + k), v, s1); s2 = _mm256_fmadd_pd(_mm256_loadu_pd(r + 2 * Cols + k), v, s2); s3 = _mm256_fmadd_pd(_mm256_loadu_pd(r + 3 * Cols + k), v, s3);
      s4 = _mm256_fmadd_pd(_mm256_loadu_pd(r + 4 * Cols + k), v, s4); s5 = _mm256_fmadd_pd(_mm256_loadu_pd(r + 5 * Cols + k), v, s5); s6 = _mm256_fmadd_pd(_mm256_loadu_pd(r + 6 * Cols + k), v, s6); s7 = _mm256_fmadd_pd(_mm256_loadu_pd(r + 7 * Cols + k), v, s7);
    }
    if constexpr (CT != 0)
    {
      s0 = _mm256_fmadd_pd(_mm256_maskload_pd(r + C4, mask), xt, s0); s1 = _mm256_fmadd_pd(_mm256_maskload_pd(r + Cols + C4, mask), xt, s1); s2 = _mm256_fmadd_pd(_mm256_maskload_pd(r + 2 * Cols + C4, mask), xt, s2); s3 = _mm256_fmadd_pd(_mm256_maskload_pd(r + 3 * Cols + C4, mask), xt, s3);
      s4 = _mm256_fmadd_pd(_mm256_maskload_pd(r + 4 * Cols + C4, mask), xt, s4); s5 = _mm256_fmadd_pd(_mm256_maskload_pd(r + 5 * Cols + C4, mask), xt, s5); s6 = _mm256_fmadd_pd(_mm256_maskload_pd(r + 6 * Cols + C4, mask), xt, s6); s7 = _mm256_fmadd_pd(_mm256_maskload_pd(r + 7 * Cols + C4, mask), xt, s7);
    }
    const __m256d h01 = _mm256_hadd_pd(s0, s1), h23 = _mm256_hadd_pd(s2, s3), h45 = _mm256_hadd_pd(s4, s5), h67 = _mm256_hadd_pd(s6, s7);
    _mm256_storeu_pd(y + i, _mm256_add_pd(_mm256_loadu_pd(y + i), _mm256_add_pd(_mm256_permute2f128_pd(h01, h23, 0x21), _mm256_blend_pd(h01, h23, 0xC))));
    _mm256_storeu_pd(y + i + 4, _mm256_add_pd(_mm256_loadu_pd(y + i + 4), _mm256_add_pd(_mm256_permute2f128_pd(h45, h67, 0x21), _mm256_blend_pd(h45, h67, 0xC))));
  }
  for (; i + 4 <= Rows; i += 4)
  {
    const double* r = A + static_cast<size_t>(i) * Cols;
    __m256d s0 = _mm256_setzero_pd(), s1 = s0, s2 = s0, s3 = s0;
    for (int k = 0; k < C4; k += 4)
    {
      const __m256d v = _mm256_loadu_pd(x + k);
      s0 = _mm256_fmadd_pd(_mm256_loadu_pd(r + k), v, s0); s1 = _mm256_fmadd_pd(_mm256_loadu_pd(r + Cols + k), v, s1); s2 = _mm256_fmadd_pd(_mm256_loadu_pd(r + 2 * Cols + k), v, s2); s3 = _mm256_fmadd_pd(_mm256_loadu_pd(r + 3 * Cols + k), v, s3);
    }
    if constexpr (CT != 0)
    {
      s0 = _mm256_fmadd_pd(_mm256_maskload_pd(r + C4, mask), xt, s0); s1 = _mm256_fmadd_pd(_mm256_maskload_pd(r + Cols + C4, mask), xt, s1); s2 = _mm256_fmadd_pd(_mm256_maskload_pd(r + 2 * Cols + C4, mask), xt, s2); s3 = _mm256_fmadd_pd(_mm256_maskload_pd(r + 3 * Cols + C4, mask), xt, s3);
    }
    const __m256d h01 = _mm256_hadd_pd(s0, s1), h23 = _mm256_hadd_pd(s2, s3);
    _mm256_storeu_pd(y + i, _mm256_add_pd(_mm256_loadu_pd(y + i), _mm256_add_pd(_mm256_permute2f128_pd(h01, h23, 0x21), _mm256_blend_pd(h01, h23, 0xC))));
  }
  if (i + 2 <= Rows)
  {
    const double* r = A + static_cast<size_t>(i) * Cols;
    __m256d s0 = _mm256_setzero_pd(), s1 = s0;
    for (int k = 0; k < C4; k += 4) { const __m256d v = _mm256_loadu_pd(x + k); s0 = _mm256_fmadd_pd(_mm256_loadu_pd(r + k), v, s0); s1 = _mm256_fmadd_pd(_mm256_loadu_pd(r + Cols + k), v, s1); }
    if constexpr (CT != 0) { s0 = _mm256_fmadd_pd(_mm256_maskload_pd(r + C4, mask), xt, s0); s1 = _mm256_fmadd_pd(_mm256_maskload_pd(r + Cols + C4, mask), xt, s1); }
    const __m256d h = _mm256_hadd_pd(s0, s1);
    _mm_storeu_pd(y + i, _mm_add_pd(_mm_loadu_pd(y + i), _mm_add_pd(_mm256_castpd256_pd128(h), _mm256_extractf128_pd(h, 1))));
    i += 2;
  }
  for (; i < Rows; ++i) y[i] += Dot(A + static_cast<size_t>(i) * Cols, x, Cols);   // a single last row
  _mm256_zeroupper();
}
#elif !defined(PCMFAST_DOT_SCALAR)
#include <emmintrin.h>
inline double Dot(const double* a, const double* b, int n)   // SSE2 dot product (baseline x64, no mixing with VEX code), four 2-wide accumulators over 8 elements per pass
{
  __m128d s0 = _mm_setzero_pd(), s1 = s0, s2 = s0, s3 = s0; int k = 0;
  for (; k + 8 <= n; k += 8)
  {
    s0 = _mm_add_pd(s0, _mm_mul_pd(_mm_loadu_pd(a + k), _mm_loadu_pd(b + k))); s1 = _mm_add_pd(s1, _mm_mul_pd(_mm_loadu_pd(a + k + 2), _mm_loadu_pd(b + k + 2)));
    s2 = _mm_add_pd(s2, _mm_mul_pd(_mm_loadu_pd(a + k + 4), _mm_loadu_pd(b + k + 4))); s3 = _mm_add_pd(s3, _mm_mul_pd(_mm_loadu_pd(a + k + 6), _mm_loadu_pd(b + k + 6)));
  }
  for (; k + 2 <= n; k += 2) s0 = _mm_add_pd(s0, _mm_mul_pd(_mm_loadu_pd(a + k), _mm_loadu_pd(b + k)));
  s0 = _mm_add_pd(_mm_add_pd(s0, s1), _mm_add_pd(s2, s3)); double r = _mm_cvtsd_f64(_mm_add_sd(s0, _mm_unpackhi_pd(s0, s0)));
  for (; k < n; ++k) r += a[k] * b[k];
  return r;
}
#else
inline double Dot(const double* a, const double* b, int n) { double r = 0.0; for (int k = 0; k < n; ++k) r += a[k] * b[k]; return r; }
#endif
// The matrix-vector products of Step() (row-major, constant dimensions). The AVX2 build runs them over blocks of rows (MatVecAdd), the baseline build row by row.
#if defined(_MSC_VER) && !defined(PCMFAST_DOT_AVX2)
#define PCMFAST_ROWFN __forceinline   // baseline: the row loops belong inside Step() (MSVC leaves them out of line otherwise, about 2 % slower)
#else
#define PCMFAST_ROWFN inline
#endif
template <int Rows, int Cols> PCMFAST_ROWFN void AddProduct(const double* base, const double* A, const double* x, double* out)   // out = base + A x
{
#ifdef PCMFAST_DOT_AVX2
  for (int i = 0; i < Rows; ++i) out[i] = base[i];
  MatVecAdd<Rows, Cols>(A, x, out);
#else
  for (int i = 0; i < Rows; ++i) out[i] = base[i] + Dot(A + static_cast<size_t>(i) * Cols, x, Cols);
#endif
}
template <int Rows, int Cols, int Cols2> PCMFAST_ROWFN void AddProduct2(const double* base, const double* A, const double* x, const double* B, const double* w, double* out)   // out = base + A x + B w
{
#ifdef PCMFAST_DOT_AVX2
  for (int i = 0; i < Rows; ++i) out[i] = base[i];
  MatVecAdd<Rows, Cols>(A, x, out); MatVecAdd<Rows, Cols2>(B, w, out);
#else
  for (int i = 0; i < Rows; ++i) { double a = base[i]; a += Dot(A + static_cast<size_t>(i) * Cols, x, Cols); a += Dot(B + static_cast<size_t>(i) * Cols2, w, Cols2); out[i] = a; }
#endif
}
template <int Rows, int Cols> PCMFAST_ROWFN void Residual(const double* p, const double* pk, const double* A, const double* f, double* F)   // F = -(p - pk - A f)
{
#ifdef PCMFAST_DOT_AVX2
  double af[Rows] = {}; MatVecAdd<Rows, Cols>(A, f, af);
  for (int i = 0; i < Rows; ++i) F[i] = -(p[i] - pk[i] - af[i]);
#else
  for (int i = 0; i < Rows; ++i) { const double af = Dot(A + static_cast<size_t>(i) * Cols, f, Cols); F[i] = -(p[i] - pk[i] - af); }
#endif
}
inline int Ctz(uint32_t m)
{
#ifdef _MSC_VER
  unsigned long i; _BitScanForward(&i, m); return (int)i;
#else
  return __builtin_ctz(m);
#endif
}

// LU with partial pivoting kept for reuse: A is overwritten by the multipliers (below the diagonal) and U, piv holds the row exchanges
template <int N> inline bool LuFactor(double (&A)[N][N], int (&piv)[N])
{
  for (int c = 0; c < N; ++c)
  {
    int pr = c; double mx = std::fabs(A[c][c]);
    for (int i = c + 1; i < N; ++i) { const double a = std::fabs(A[i][c]); if (a > mx) { mx = a; pr = i; } }
    if (mx < 1e-300) return false;
    piv[c] = pr;
    if (pr != c) for (int j = 0; j < N; ++j) { const double t = A[c][j]; A[c][j] = A[pr][j]; A[pr][j] = t; }
    const double inv = 1.0 / A[c][c];
    for (int i = c + 1; i < N; ++i) { const double f = A[i][c] * inv; A[i][c] = f; if (f == 0.0) continue; for (int j = c + 1; j < N; ++j) A[i][j] -= f * A[c][j]; }
  }
  return true;
}
template <int N> inline bool LuFactorNoPivot(double (&A)[N][N])
{
  for (int c = 0; c < N; ++c)
  {
    const double d = A[c][c]; if (!(std::fabs(d) > 1e-3)) return false;
    const double inv = 1.0 / d;
    for (int i = c + 1; i < N; ++i) { const double f = A[i][c] * inv; A[i][c] = f; for (int j = c + 1; j < N; ++j) A[i][j] -= f * A[c][j]; }
  }
  return true;
}
template <int N> inline void LuSolve(const double (&A)[N][N], const int (&piv)[N], double (&b)[N])
{
  for (int c = 0; c < N; ++c) if (piv[c] != c) { const double t = b[c]; b[c] = b[piv[c]]; b[piv[c]] = t; }   // the factorization exchanged whole rows: apply all exchanges first
  for (int c = 0; c < N; ++c) for (int i = c + 1; i < N; ++i) b[i] -= A[i][c] * b[c];
  for (int i = N - 1; i >= 0; --i) { double sum = b[i]; for (int j = i + 1; j < N; ++j) sum -= A[i][j] * b[j]; b[i] = sum / A[i][i]; }
}

template <class M> struct Voice
{
  static constexpr int R = M::kR, NU = M::kU, NP = M::kPorts, NF = M::kCur, NB = M::kNumBjt, NBITS = M::kBits, IACC = M::kAccentIdx, ND = DioCount<M>::value, NT = TanhCount<M>::value, NR = ResCount<M>::value, NQ = QCount<M>::value, Q0 = 2 * M::kNumBjt + DioCount<M>::value + TanhCount<M>::value + ResCount<M>::value;   // currents: 2 per BJT, then one per diode, then one per op-amp input stage (deviation from the linear gm)
  double resG[NR > 0 ? NR : 1];   // conductances of the pot resistors (run-time parameters; the nominal values are the header's kResG0)
  void SetResG(int m, double g) { if constexpr (NR > 0) { if (!resInit) { for (int i = 0; i < NR; ++i) resG[i] = M::kResG0[i]; resInit = true; } resG[m] = g; } }
  bool resInit = false;
  mutable double qG[NQ > 0 ? NQ : 1];   // slope of the excess junction charge channels at the last full evaluation
  mutable double tanhG[NT > 0 ? NT : 1];   // slope of the op-amp stage currents at the last full evaluation
  mutable double dp[ND > 0 ? ND : 1][5], dioVj[ND > 0 ? ND : 1], dioG[ND > 0 ? ND : 1];   // diode constants {Is, N Vt, Rs, breakdown knee, critical voltage}, internal junction voltage (warm start), conductance of the last full evaluation
  double coneEff[M::kSteps * R], pd1Eff[NP], accOut = 0.0;   // constant forcing of every step type including the accent input (RefreshAccent)
  double z[R], f0[NF], p[NP], u0[NU], vbcFix[NB], acc = M::kAccNom;   // vbcFix: vbc of the devices whose vbc is not a port (DC value at the accent)
    // acc: accent voltage of the hit (run-time input)
  double bp[NB][16];   // BJT parameters with precomputed reciprocals (BjtEval)
  int period = 0, stepIdx = 0, layout = 0; uint32_t bitsNow = 0, bitsPrev = 0;   // stepIdx counts within the layout of the current period
  double acLow = 0.0, acEnv = 0.0, acPeak = 0.0; long quietPeriods = 0; bool finished = false;   // audio end: 20 Hz high-passed output envelope below the end level for kAcEndPeriods periods after its peak (acEndRel > 0: only after the PCM sequence, level = min(acEndLevel, acEndRel * peak))
  double lastOut = 0.0, lastH = 0.0, outDc = 0.0, env = 0.0, peak = 1e-12;   // last step length, DC output, level envelope and its maximum (layout choice)
  long long periodsByLayout[4] = {0, 0, 0, 0}; double tailRel = kTailRel, newtonTol = kNewtonTol, acEndLevel = kAcEndLevel, acEndRel = 0.0; int noSolve = 0;   // noSolve: timing probe only (no nonlinear solve); tailRel: switch to the cheaper layout below this fraction of the loudest envelope
  double prof[6] = {0, 0, 0, 0, 0, 0};
  // Block-triangular factorization of the Newton Jacobian: the port graph (row i depends on port c where a current with a Zp entry above blockRel*max|Zp| depends on c) is split into
  // strongly connected blocks ordered so that a block depends only on earlier blocks; entries outside the structure are dropped from the Jacobian (the residual stays exact). blockRel = 0: dense LU.
  struct JCache { double J[NP][NP]; double LU[NP * NP]; int pv[NP]; double inv[NP]; double jf[NB][4]; double dioG[ND > 0 ? ND : 1], tanhG[NT > 0 ? NT : 1], qG[NQ > 0 ? NQ : 1]; bool valid = false; int age = 0; };
  JCache jc[M::kSteps];   // Jacobian and block factors per step type (jReuse > 0): reused for up to jReuse periods; invalidated when the reused Jacobian needs more than one extra iteration
  double dioMemoThr = 0.0; mutable double dioMemoV[ND > 0 ? ND : 1], dioMemoI[ND > 0 ? ND : 1], dioMemoG[ND > 0 ? ND : 1]; mutable bool dioMemoOk[ND > 0 ? ND : 1] = {};   // diode evaluation memo (HH-K17): reuse within |dv| <= dioMemoThr volts
  bool qTable = false;   // charge channels from the Hermite table instead of the exact function
  int denseSteps = 0; long long denseSwitches = 0;   // block mode: steps left with the dense Jacobian; number of switches
  int jReuse = 0; long long jReuseUsed = 0; double jReuseTol = 0.1;   // jReuseTol: the correction limit of a step that starts with a reused Jacobian is newtonTol * jReuseTol
  double quietLevel = 0.0, quietTol = 0.0;   // while the output envelope env is below quietLevel volts, quietTol replaces newtonTol (0: never)
  double blockRel = 0.0, blkT = -1.0; bool blkReady = false; int nBlk = 0, bOrd[NP], bStart[NP + 1], bOff[NP + 1], blkPiv[NP], bDepN[NP], bDep[NP][NP]; double blkInv[NP], blkLU[NP * NP];   // bDep: for the port at position r the ports of earlier blocks it depends on; blkInv: reciprocal pivots
  void PrepareBlocks() { if (blockRel > 0.0 && (!blkReady || blkT != st.T)) BuildBlocks(); }   // builds the block structure before the first step; allocates, so call it off the audio thread
  void BuildBlocks()
  {
    std::vector<std::vector<int>> deps(NF);
    for (int k = 0; k < NB; ++k) { deps[2 * k].push_back(M::kPortBe[k]); if (M::kPortBc[k] >= 0) deps[2 * k].push_back(M::kPortBc[k]); deps[2 * k + 1] = deps[2 * k]; }
    if constexpr (ND > 0) for (int m = 0; m < ND; ++m) deps[2 * NB + m].push_back(M::kPortD[m]);
    if constexpr (NT > 0) for (int t = 0; t < NT; ++t) deps[2 * NB + ND + t].push_back(M::kPortT[t]);
    if constexpr (NR > 0) for (int m = 0; m < NR; ++m) deps[2 * NB + ND + NT + m].push_back(M::kPortR[m]);
    if constexpr (NQ > 0) for (int q = 0; q < NQ; ++q) deps[Q0 + q].push_back(M::kQPort[q]);
    std::vector<char> A(NP * NP, 0); double gmax = 0.0;
    for (int j = M::kLayoutFirst[0]; j < M::kLayoutFirst[0] + M::kLayoutN[0]; ++j) for (int i = 0; i < NP; ++i) for (int k = 0; k < NF; ++k) gmax = std::fmax(gmax, std::fabs(st.zp[static_cast<size_t>(j) * NP * NF + i * NF + k]));
    for (int j = M::kLayoutFirst[0]; j < M::kLayoutFirst[0] + M::kLayoutN[0]; ++j) for (int i = 0; i < NP; ++i) for (int k = 0; k < NF; ++k)
      if (std::fabs(st.zp[static_cast<size_t>(j) * NP * NF + i * NF + k]) > blockRel * gmax) for (int c : deps[k]) A[i * NP + c] = 1;
    std::vector<int> idx(NP, -1), low(NP, 0), comp(NP, -1), stk; std::vector<char> on(NP, 0); int counter = 0, nc = 0;
    std::function<void(int)> dfs = [&](int u)
    {
      idx[u] = low[u] = counter++; stk.push_back(u); on[u] = 1;
      for (int w = 0; w < NP; ++w) if (A[u * NP + w] && w != u) { if (idx[w] < 0) { dfs(w); low[u] = std::min(low[u], low[w]); } else if (on[w]) low[u] = std::min(low[u], idx[w]); }
      if (low[u] == idx[u]) { for (;;) { const int w = stk.back(); stk.pop_back(); on[w] = 0; comp[w] = nc; if (w == u) break; } ++nc; }
    };
    for (int u = 0; u < NP; ++u) if (idx[u] < 0) dfs(u);
    nBlk = nc; int pos = 0;   // component numbers are in dependency order (a component depends only on smaller numbers)
    for (int b = 0; b < nc; ++b) { bStart[b] = pos; for (int u = 0; u < NP; ++u) if (comp[u] == b) bOrd[pos++] = u; }
    bStart[nc] = pos;
    for (int b = 0; b < nc; ++b) for (int r = bStart[b]; r < bStart[b + 1]; ++r) { const int i = bOrd[r]; bDepN[r] = 0; for (int q = 0; q < bStart[b]; ++q) if (A[i * NP + bOrd[q]]) bDep[r][bDepN[r]++] = bOrd[q]; }
    int off = 0; for (int b = 0; b < nc; ++b) { bOff[b] = off; const int n = bStart[b + 1] - bStart[b]; off += n * n; } bOff[nc] = off;
    blkT = st.T; blkReady = true; for (auto& c : jc) c.valid = false;
  }
  bool BlockFactor(const double (&J)[NP][NP])
  {
    for (int b = 0; b < nBlk; ++b)
    {
      const int s = bStart[b], n = bStart[b + 1] - s; double* A = &blkLU[bOff[b]];
      for (int r = 0; r < n; ++r) for (int c = 0; c < n; ++c) A[r * n + c] = J[bOrd[s + r]][bOrd[s + c]];
      for (int c = 0; c < n; ++c)
      {
        int pr = c; double mx = std::fabs(A[c * n + c]);
        for (int i = c + 1; i < n; ++i) { const double a = std::fabs(A[i * n + c]); if (a > mx) { mx = a; pr = i; } }
        if (mx < 1e-300) return false;
        blkPiv[s + c] = pr;
        if (pr != c) for (int j = 0; j < n; ++j) { const double t = A[c * n + j]; A[c * n + j] = A[pr * n + j]; A[pr * n + j] = t; }
        const double inv = 1.0 / A[c * n + c]; blkInv[s + c] = inv;
        for (int i = c + 1; i < n; ++i) { const double f = A[i * n + c] * inv; A[i * n + c] = f; if (f == 0.0) continue; for (int j = c + 1; j < n; ++j) A[i * n + j] -= f * A[c * n + j]; }
      }
    }
    return true;
  }
  void BlockSolve(const double (&J)[NP][NP], double* F) const
  {
    for (int b = 0; b < nBlk; ++b)
    {
      const int s = bStart[b], n = bStart[b + 1] - s; const double* A = &blkLU[bOff[b]]; double rhs[NP];
      for (int r = 0; r < n; ++r) { const int i = bOrd[s + r]; double a = F[i]; const int nd = bDepN[s + r]; for (int d = 0; d < nd; ++d) { const int c = bDep[s + r][d]; a -= J[i][c] * F[c]; } rhs[r] = a; }
      for (int c = 0; c < n; ++c) { const int pr = blkPiv[s + c]; if (pr != c) { const double t = rhs[c]; rhs[c] = rhs[pr]; rhs[pr] = t; } }
      for (int c = 0; c < n; ++c) for (int i = c + 1; i < n; ++i) rhs[i] -= A[i * n + c] * rhs[c];
      for (int i = n - 1; i >= 0; --i) { double sum = rhs[i]; for (int j = i + 1; j < n; ++j) sum -= A[i * n + j] * rhs[j]; rhs[i] = sum * blkInv[s + i]; }
      for (int r = 0; r < n; ++r) F[bOrd[s + r]] = rhs[r];
    }
  }
  // Couplings of the currents into the state equations (bw, R x NF) and the port equations (df, NP x NF) of the model, for the DC solve of the idle state; built once per model type.
  struct DcMatrices
  {
    double bw[R * NF], df[NP * NF];
    DcMatrices()
    {
      constexpr int NS = M::kNS, NG = M::kNg;
      std::vector<double> t(static_cast<size_t>(NS) * NF);   // node currents of the currents f: DgSW Tc
      for (int a = 0; a < NS; ++a) for (int c = 0; c < NF; ++c) { double s = 0.0; for (int k = 0; k < NS; ++k) s += M::kDgSW[a * NS + k] * M::kTc[k * NF + c]; t[a * NF + c] = s; }
      for (int i = 0; i < NP; ++i) for (int c = 0; c < NF; ++c) { double s = 0.0; for (int a = 0; a < NS; ++a) s += M::kPpA[i * NS + a] * t[a * NF + c]; df[i * NF + c] = s; }
      for (int i = 0; i < R; ++i) for (int c = 0; c < NF; ++c) { double s = 0.0; for (int k = 0; k < NS; ++k) s += M::kBg[i * NG + NU + k] * M::kTc[k * NF + c]; bw[i * NF + c] = s; }
    }
    static const DcMatrices& Get() { static const DcMatrices inst; return inst; }
  };
  void PrepareSettle() { DcMatrices::Get(); }   // not on the audio thread (builds the matrices)
  PcmSteps<M> st;  // step tables of the PCM period of the hit (built from the nominal period unless SetPeriod() was called)
  // After the end of a hit the circuit is computed on with long step tables (PCM bits held, dense solve), so that a following hit starts from the state of the circuit at its instant. While tracking, st holds the
  // long tables and trk the tables of the hit.
  PcmSteps<M> trk; bool tracking = false;
  void PrepareTrack(double period) { if (!tracking && trk.T != period) trk.Build(period); }   // not on the audio thread
  void BeginTrack() { if (tracking) return; std::swap(st, trk); tracking = true; stepIdx = 0; RefreshAccent(); }
  void EndTrack() { if (!tracking) return; std::swap(st, trk); tracking = false; stepIdx = 0; RefreshAccent(); }
  const PcmSteps<M>& HitSteps() const { return tracking ? trk : st; }
  void SetPeriod(double period) { (tracking ? trk : st).Build(period); }
  double ClockPeriod() const { return HitSteps().T; }
  double tap[16]; int tapCount = 0;   // output taps of the last Step (uniform instants m*T/kTaps inside the step)
  bool lastConverged = true;   // the Newton iteration of the last Step reached its correction limit
  bool dcSolved = true; int dcIts = 0;   // result and iterations of the last DC solve of the idle state
  long long newtonIts = 0, newtonFallbacks = 0, stepsDone = 0; long long itsByStep[M::kSteps > 8 ? M::kSteps : 8] = {}, nByStep[M::kSteps > 8 ? M::kSteps : 8] = {}, itHist[12] = {0,0,0,0,0,0,0,0,0,0,0,0};   // 64 bit: a retrigger does not reset them

  void RefreshAccent()
  {
    if (st.T == 0.0) st.Build(M::kPeriod);
    for (int j = 0; j < M::kSteps; ++j) { const double* ca = &st.gsUc[(static_cast<size_t>(j) * NU + IACC) * R]; for (int i = 0; i < R; ++i) coneEff[j * R + i] = st.cone[static_cast<size_t>(j) * R + i] + acc * ca[i]; }
    for (int i = 0; i < NP; ++i) pd1Eff[i] = M::kPD1[i] + acc * M::kPDuc[IACC * NP + i];
    accOut = M::kDo1 + acc * M::kDou[IACC];
  }
  void LoadDc()   // state of the header: DC solution at the accent (nominal pot resistors), interpolated linearly in the accent grid; idle state of the models without pot resistors, start of the solve of the others
  {
    int g = 0; while (g < M::kAccGrid - 2 && acc >= M::kAccV[g + 1]) ++g;
    double t = (acc - M::kAccV[g]) / (M::kAccV[g + 1] - M::kAccV[g]); t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
    for (int i = 0; i < R; ++i) z[i] = (1.0 - t) * M::kDcZ[g * R + i] + t * M::kDcZ[(g + 1) * R + i];
    for (int i = 0; i < NP; ++i) p[i] = (1.0 - t) * M::kDcP[g * NP + i] + t * M::kDcP[(g + 1) * NP + i];
    for (int k = 0; k < NB; ++k) vbcFix[k] = (1.0 - t) * M::kDcVbc[g * NB + k] + t * M::kDcVbc[(g + 1) * NB + k];
  }
  void Reset()
  {
    EndTrack(); RefreshAccent(); LoadDc();
    for (int k = 0; k < NB; ++k)
    {
      const double* q = &M::kBjt[9 * k]; for (int i = 0; i < 9; ++i) bp[k][i] = q[i];
      bp[k][9] = 1.0 / (q[4] * kVtPcm); bp[k][10] = 1.0 / (q[7] * kVtPcm); bp[k][11] = 1.0 / q[5]; bp[k][12] = 1.0 / q[6]; bp[k][13] = 1.0 / q[2]; bp[k][14] = 1.0 / q[3]; bp[k][15] = kVtPcm * std::log(kVtPcm / (1.4142135623730951 * q[1]));   // [15]: critical junction voltage (limiting)
    }
    if constexpr (NR > 0) { if (!resInit) { for (int m = 0; m < NR; ++m) resG[m] = M::kResG0[m]; resInit = true; } }
    if constexpr (ND > 0)
      for (int m = 0; m < ND; ++m)
      {
        const double* q = &M::kDio[5 * m]; const double nvt = q[1] * kVtPcm;
        dp[m][0] = q[0]; dp[m][1] = nvt; dp[m][2] = q[2]; dp[m][3] = q[3] > 1e20 ? 1e30 : q[3] - nvt * std::log(std::fmax(q[4] / q[0], 1.0)); dp[m][4] = nvt * std::log(nvt / (1.4142135623730951 * q[0]));
        dioVj[m] = p[M::kPortD[m]]; dioG[m] = 0.0;
      }
    for (int i = 0; i < NU; ++i) u0[i] = M::kU0[i];
    bitsPrev = 0; for (int i = 0; i < NBITS; ++i) if (M::kU0[i] > 0.5 * M::kVhigh) bitsPrev |= (1u << i);
    if constexpr (NR > 0) { if (!LoadSettled()) { SettleResistors(); StoreSettled(); } }
    EvalF(p, f0, nullptr); lastOut = OutputNow(); period = 0; stepIdx = 0; layout = 0; newtonIts = 0; stepsDone = 0; outDc = OutputNow(); env = 0.0; peak = 1e-12; acLow = outDc; acEnv = 0.0; acPeak = 0.0; quietPeriods = 0; finished = false;
    for (int i = 0; i < 4; ++i) periodsByLayout[i] = 0;
    for (auto& c : jc) c.valid = false;   // a new hit starts with fresh Jacobians
    denseSteps = 0;
    if constexpr (ND > 0) for (int m = 0; m < ND; ++m) dioMemoOk[m] = false;
  }
  // The idle states of the last kSettled combinations of accent and pot resistors: a later hit from rest with the same combination takes its state from here instead of solving it again.
  static constexpr int kSettled = NR > 0 ? 16 : 1;   // one per accent level of the plugin and a second pot position
  struct Settled { double acc, g[NR > 0 ? NR : 1], z[R], p[NP], vj[ND > 0 ? ND : 1]; };
  Settled settled[kSettled]; int settledCount = 0, settledNext = 0;
  bool LoadSettled()
  {
    for (int k = 0; k < settledCount; ++k)
    {
      const Settled& s = settled[k]; if (s.acc != acc) continue;
      bool same = true; for (int m = 0; m < NR; ++m) if (s.g[m] != resG[m]) same = false;
      if (!same) continue;
      std::memcpy(z, s.z, sizeof z); std::memcpy(p, s.p, sizeof p);
      if constexpr (ND > 0) std::memcpy(dioVj, s.vj, sizeof dioVj);
      return true;
    }
    return false;
  }
  void StoreSettled()
  {
    Settled& s = settled[settledNext]; settledNext = (settledNext + 1) % kSettled; if (settledCount < kSettled) ++settledCount;
    s.acc = acc; for (int m = 0; m < NR; ++m) s.g[m] = resG[m];
    std::memcpy(s.z, z, sizeof z); std::memcpy(s.p, p, sizeof p);
    if constexpr (ND > 0) std::memcpy(s.vj, dioVj, sizeof dioVj);
  }
  // Idle state of a model with pot resistors: the DC point of its own equations at rest (bits at their initial values, no trigger) at the present resistors, 0 = A z + Bg (u, Tc f(p), 1) and
  // p = PXz z + PD1 + PDuc u + df f(p), solved by Newton for state z and port voltages p together, starting at the state of the header (nominal resistors; on the audio thread).
  // Nothing is eliminated: the slow states (isolated nodes of the circuit, balanced by gmin leakage only) are fixed by their own equations through the port voltages, which a division by their
  // eigenvalue cannot resolve. The rows differ by many orders of magnitude and are scaled to their largest entry. Without convergence the state of the header stays.
  void SettleResistors()
  {
    if constexpr (NR > 0)
    {
      constexpr int N = R + NP, NG = M::kNg;
      const DcMatrices& dc = DcMatrices::Get();
      const double memo = dioMemoThr; dioMemoThr = 0.0;
      double u[NU]; for (int i = 0; i < NU; ++i) u[i] = i == IACC ? acc : (((bitsPrev >> i) & 1u) ? M::kVhigh : 0.0);
      double x[N]; for (int i = 0; i < R; ++i) x[i] = z[i]; for (int i = 0; i < NP; ++i) x[R + i] = p[i];
      bool ok = false;
      for (int it = 0; it < kDcIts && !ok; ++it)
      {
        double f[NF], jf[NB][4]; EvalF(x + R, f, jf);
        double J[N][N] = {}, b[N];
        for (int i = 0; i < R; ++i)
        {
          double s = M::kBg[i * NG + NG - 1]; for (int k = 0; k < R; ++k) { s += M::kA[i * R + k] * x[k]; J[i][k] = M::kA[i * R + k]; }
          for (int k = 0; k < NU; ++k) s += M::kBg[i * NG + k] * u[k]; for (int c = 0; c < NF; ++c) s += dc.bw[i * NF + c] * f[c]; b[i] = -s;
        }
        for (int i = 0; i < NP; ++i)
        {
          double s = M::kPD1[i]; for (int k = 0; k < R; ++k) { s += M::kPXz[i * R + k] * x[k]; J[R + i][k] = -M::kPXz[i * R + k]; }
          for (int k = 0; k < NU; ++k) s += M::kPDuc[k * NP + i] * u[k]; for (int c = 0; c < NF; ++c) s += dc.df[i * NF + c] * f[c]; b[R + i] = s - x[R + i]; J[R + i][R + i] = 1.0;
        }
        auto slope = [&](int c, int q, double g) { for (int i = 0; i < R; ++i) J[i][R + q] += dc.bw[i * NF + c] * g; for (int i = 0; i < NP; ++i) J[R + i][R + q] -= dc.df[i * NF + c] * g; };   // current c depends on port q
        for (int k = 0; k < NB; ++k) for (int r = 0; r < 2; ++r) { slope(2 * k + r, M::kPortBe[k], jf[k][2 * r]); if (M::kPortBc[k] >= 0) slope(2 * k + r, M::kPortBc[k], jf[k][2 * r + 1]); }
        if constexpr (ND > 0) for (int m = 0; m < ND; ++m) slope(2 * NB + m, M::kPortD[m], dioG[m]);
        if constexpr (NT > 0) for (int m = 0; m < NT; ++m) slope(2 * NB + ND + m, M::kPortT[m], tanhG[m]);
        for (int m = 0; m < NR; ++m) slope(2 * NB + ND + NT + m, M::kPortR[m], resG[m]);
        if constexpr (NQ > 0) for (int m = 0; m < NQ; ++m) slope(Q0 + m, M::kQPort[m], qG[m]);
        for (int i = 0; i < N; ++i) { double s = 0.0; for (int k = 0; k < N; ++k) s = std::fmax(s, std::fabs(J[i][k])); if (s > 0.0) { const double inv = 1.0 / s; for (int k = 0; k < N; ++k) J[i][k] *= inv; b[i] *= inv; } }
        bool singular = false;
        for (int c = 0; c < N && !singular; ++c)   // elimination with partial pivoting
        {
          int pr = c; for (int r = c + 1; r < N; ++r) if (std::fabs(J[r][c]) > std::fabs(J[pr][c])) pr = r;
          if (pr != c) { for (int k = c; k < N; ++k) std::swap(J[c][k], J[pr][k]); std::swap(b[c], b[pr]); }
          if (!(std::fabs(J[c][c]) > 0.0)) { singular = true; break; }
          const double inv = 1.0 / J[c][c];
          for (int r = c + 1; r < N; ++r) { const double m = J[r][c] * inv; if (m == 0.0) continue; for (int k = c + 1; k < N; ++k) J[r][k] -= m * J[c][k]; b[r] -= m * b[c]; }
        }
        if (singular) break;
        for (int c = N - 1; c >= 0; --c) { double s = b[c]; for (int k = c + 1; k < N; ++k) s -= J[c][k] * b[k]; b[c] = s / J[c][c]; }
        double mx = 0.0, sum = 0.0; for (int i = 0; i < N; ++i) sum += std::fabs(b[i]); for (int i = 0; i < NP; ++i) mx = std::fmax(mx, std::fabs(b[R + i]));
        if (!(sum < 1e30)) break;
        const double sc = mx > kDcDamp ? kDcDamp / mx : 1.0; for (int i = 0; i < N; ++i) x[i] += sc * b[i];
        ok = mx < kDcTol; dcIts = it + 1;
      }
      dioMemoThr = memo;
      if (ok) { for (int i = 0; i < R; ++i) z[i] = x[i]; for (int i = 0; i < NP; ++i) p[i] = x[R + i]; }
      else LoadDc();
      dcSolved = ok;
    }
  }
  // restart of the PCM sequence while the analog state continues (the digital counters are reset, the capacitors keep their charge)
  void Retrigger()
  {
    period = 0; stepIdx = 0; layout = 0; finished = false; env = 0.0; peak = 1e-12; acLow = lastOut; acEnv = 0.0; acPeak = 0.0; quietPeriods = 0;
  }
  // currents f (and the block Jacobians when jf != nullptr) at the port voltages q
  void EvalF(const double* q, double* f, double (*jf)[4]) const
  {
#if defined(PCMFAST_DOT_AVX2) && !defined(PCMFAST_EXACT_BJT)
    constexpr int NX = (3 * NB + 3) / 4 * 4;   // the three exponentials of all devices at once (those a device does not need are computed and ignored)
    double xa[NX] = {}, xe[NX];
    for (int k = 0; k < NB; ++k)
    {
      const double vbe = q[M::kPortBe[k]], vbc = M::kPortBc[k] >= 0 ? q[M::kPortBc[k]] : vbcFix[k];
      xa[3 * k] = vbe * bp[k][9]; xa[3 * k + 1] = vbe * bp[k][10]; xa[3 * k + 2] = vbc * (1.0 / kVtPcm);
    }
    ExpBatch(xa, xe);
#endif
    for (int k = 0; k < NB; ++k)
    {
      const double vbe = q[M::kPortBe[k]], vbc = M::kPortBc[k] >= 0 ? q[M::kPortBc[k]] : vbcFix[k];
#if defined(PCMFAST_DOT_AVX2) && !defined(PCMFAST_EXACT_BJT)
      const BjtOut o = BjtFromExp(bp[k], vbe, vbc, xa[3 * k] < -36.0 ? 0.0 : xe[3 * k], xa[3 * k + 1] < -36.0 ? 0.0 : xe[3 * k + 1], xe[3 * k + 2]);   // deep cutoff as in BjtEvalFast
#elif defined(PCMFAST_EXACT_BJT)
      const BjtOut o = BjtEval(bp[k], vbe, vbc, kVtPcm);
#else
      const BjtOut o = BjtEvalFast(bp[k], vbe, vbc);
#endif
      if (M::kHasLinPart)   // semi-linear devices: the linear part about the DC point is in the linear model, only the remainder is nonlinear
      {
        const double* L = &M::kLinPart[8 * k]; const double dvbe = vbe - L[0], dvbc = vbc - L[1];
        f[2 * k] = o.ic - (L[2] + L[4] * dvbe + L[5] * dvbc); f[2 * k + 1] = o.ib - (L[3] + L[6] * dvbe + L[7] * dvbc);
        if (jf) { jf[k][0] = o.d[0][0] - L[4]; jf[k][1] = o.d[0][1] - L[5]; jf[k][2] = o.d[1][0] - L[6]; jf[k][3] = o.d[1][1] - L[7]; }
      }
      else
      {
        f[2 * k] = o.ic; f[2 * k + 1] = o.ib;
        if (jf) { jf[k][0] = o.d[0][0]; jf[k][1] = o.d[0][1]; jf[k][2] = o.d[1][0]; jf[k][3] = o.d[1][1]; }
      }
    }
    if constexpr (ND > 0)
      for (int m = 0; m < ND; ++m)
      {
        double i, g; const double v = q[M::kPortD[m]];
        if (dioMemoThr > 0.0 && dioMemoOk[m] && std::fabs(v - dioMemoV[m]) <= dioMemoThr) { i = dioMemoI[m]; g = dioMemoG[m]; }   // same terminal voltage as the last evaluation: same current and conductance
        else { DioEval(dp[m], v, dioVj[m], i, g); dioMemoV[m] = v; dioMemoI[m] = i; dioMemoG[m] = g; dioMemoOk[m] = true; }
        f[2 * NB + m] = i; if (jf) dioG[m] = g;
      }
    if constexpr (NT > 0)
      for (int t = 0; t < NT; ++t)
      {
        const double gm = M::kTanh[2 * t], im = M::kTanh[2 * t + 1], th = std::tanh(gm * q[M::kPortT[t]] / im);
        f[2 * NB + ND + t] = im * th - gm * q[M::kPortT[t]]; if (jf) tanhG[t] = -gm * th * th;
      }
    if constexpr (NR > 0)
      for (int m = 0; m < NR; ++m) f[2 * NB + ND + NT + m] = resG[m] * q[M::kPortR[m]];
    if constexpr (NQ > 0)
      for (int j = 0; j < NQ; ++j) { double dq, dc; if (qTable) QTable<M>::Get().Eval(j, q[M::kQPort[j]], dq, dc); else QCharge(&M::kQPar[11 * j], q[M::kQPort[j]], dq, dc); f[Q0 + j] = dq; if (jf) qG[j] = dc; }
  }
  double OutputNow() const   // output of the present state with the present bits (before the first step: the DC output of the hit)
  {
    double f1[NF]; EvalF(p, f1, nullptr); double out = M::kDo1 + acc * M::kDou[IACC];
    for (int k = 0; k < R; ++k) out += M::kXo[k] * z[k];
    for (uint32_t m = bitsPrev; m; m &= m - 1) out += M::kVhigh * M::kDou[Ctz(m)];
    for (int k = 0; k < NF; ++k) out += M::kDof[k] * f1[k];
    return out;
  }
  // advance one integration step; returns the output voltage at its end (volts, absolute node voltage)
  double Step()
  {
    PF_T0;
    if (stepIdx == 0)   // new PCM period: new bits at the end of the ramp; the layout of the period follows the signal level (the first period of a hit, with the gate edge, always runs the full layout)
    {
      bitsNow = tracking ? bitsPrev : ((period < M::kPeriods) ? M::kPcm[period] : M::kPcm[M::kPeriods - 1]);
      const bool coarse = M::kLayouts > 1 && period > 0 && !((bitsNow ^ bitsPrev) & GateMask<M>::value);   // layout 0: full; the last one: tail; a third one in between (below kMidRelCym of the loudest envelope)
      layout = coarse && env < tailRel * peak ? M::kLayouts - 1 : (M::kLayouts > 2 && coarse && env < kMidRelCym * peak ? 1 : 0); ++periodsByLayout[layout];
    }
    const int j = M::kLayoutFirst[layout] + stepIdx; lastH = st.h[j];
    const double* Phi = &st.phi[static_cast<size_t>(j) * R * R];
    const double* GaF = &st.gaF[static_cast<size_t>(j) * R * NF]; const double* GbF = &st.gbF[static_cast<size_t>(j) * R * NF]; const double* Cone = &coneEff[j * R]; const double* Zp = &st.zp[static_cast<size_t>(j) * NP * NF];
    double zk[R];
    AddProduct2<R, R, NF>(Cone, Phi, z, GaF, f0, zk);
    if (stepIdx == 0)   // ramp: old bits ramp to the new bits
    {
      for (uint32_t m = bitsPrev; m; m &= m - 1) { const int k = Ctz(m); const double* c = &st.gaUc[(static_cast<size_t>(j) * NU + k) * R]; for (int i = 0; i < R; ++i) zk[i] += M::kVhigh * c[i]; }
      for (uint32_t m = bitsNow; m; m &= m - 1) { const int k = Ctz(m); const double* c = &st.gbUc[(static_cast<size_t>(j) * NU + k) * R]; for (int i = 0; i < R; ++i) zk[i] += M::kVhigh * c[i]; }
    }
    else          // hold: constant bits
      for (uint32_t m = bitsNow; m; m &= m - 1) { const int k = Ctz(m); const double* c = &st.gsUc[(static_cast<size_t>(j) * NU + k) * R]; for (int i = 0; i < R; ++i) zk[i] += M::kVhigh * c[i]; }
    PF_LAP(0);
    double pk[NP];
    AddProduct<NP, R>(pd1Eff, M::kPXz, zk, pk);
    for (uint32_t m = bitsNow; m; m &= m - 1) { const int k = Ctz(m); const double* c = &M::kPDuc[k * NP]; for (int i = 0; i < NP; ++i) pk[i] += M::kVhigh * c[i]; }
    PF_LAP(1);
    double f1[NF]; const long long itsBefore = newtonIts; bool fCurrent = false, reuse = false; JCache& jcj = jc[j];
    if (!noSolve)
    {
    const bool blockMode = blockRel > 0.0 && !tracking;
    const double tol = env < quietLevel ? quietTol : newtonTol;   // correction limit of this step
    double J[NP][NP]; int piv[NP]; double jf[NB][4]; bool luOk = false; double lastMx = 0.0; bool denseLu = true; bool useBlk = blockMode && denseSteps == 0;
    if (denseSteps > 0) --denseSteps;
    double pStart[NP]; for (int i = 0; i < NP; ++i) pStart[i] = p[i];
    if (useBlk && jReuse > 0 && jcj.valid && jcj.age < jReuse)   // Jacobian and block factors of this step type from an earlier period: the first iteration is a chord iteration
    {
      std::memcpy(J, jcj.J, sizeof J); std::memcpy(blkLU, jcj.LU, sizeof blkLU); std::memcpy(blkPiv, jcj.pv, sizeof blkPiv); std::memcpy(blkInv, jcj.inv, sizeof blkInv); std::memcpy(jf, jcj.jf, sizeof jf);
      if constexpr (ND > 0) std::memcpy(dioG, jcj.dioG, sizeof dioG);
      if constexpr (NT > 0) std::memcpy(tanhG, jcj.tanhG, sizeof tanhG);
      if constexpr (NQ > 0) std::memcpy(qG, jcj.qG, sizeof qG);
      luOk = true; denseLu = false; reuse = true; ++jcj.age; ++jReuseUsed;
    }
    for (int attempt = 0; attempt < 2 && !fCurrent; ++attempt)   // second attempt after a failed Newton iteration: restart from the ports of the step start with damped corrections and more iterations
    {
    if (attempt == 1) { for (int i = 0; i < NP; ++i) p[i] = pStart[i]; luOk = false; lastMx = 0.0; ++newtonFallbacks; reuse = false; jcj.valid = false; }
    for (int it = 0; it < (attempt ? 120 : 40); ++it)
    {
      double F[NP];
      if (useBlk && stepsDone >= kBlkStartSteps && (it == kBlkSwitchIt || attempt == 1))   // switch to the dense Jacobian (see kBlkSwitchIt): this step starts again, the stored Jacobians are dropped
      {
        useBlk = false; reuse = false; luOk = false; lastMx = 0.0; denseSteps = kBlkDenseSteps; ++denseSwitches; for (auto& c : jc) c.valid = false;
        for (int i = 0; i < NP; ++i) p[i] = pStart[i]; it = 0;
      }
      if ((it == 0 && !reuse) || !luOk || lastMx > kChordMaxCorr)   // full Newton iteration (also while the correction is large: the Jacobian of the first iteration does not hold for a junction that moved far): currents, block Jacobians, factorization
      {
        EvalF(p, f1, jf); ++newtonIts;
        for (int attempt = 0; attempt < 2; ++attempt)   // 1st: diagonal pivots (J = I - Zp Jf is diagonally dominant in practice); 2nd: partial pivoting if a pivot was small
        {
          Residual<NP, NF>(p, pk, Zp, f1, F);
          for (int i = 0; i < NP; ++i)
          {
            const double* zr = Zp + i * NF;
            for (int c = 0; c < NP; ++c) J[i][c] = (i == c ? 1.0 : 0.0);
            for (int k = 0; k < NB; ++k)
            {
              J[i][M::kPortBe[k]] -= zr[2 * k] * jf[k][0] + zr[2 * k + 1] * jf[k][2];
              if (M::kPortBc[k] >= 0) J[i][M::kPortBc[k]] -= zr[2 * k] * jf[k][1] + zr[2 * k + 1] * jf[k][3];
            }
            if constexpr (ND > 0) for (int m = 0; m < ND; ++m) J[i][M::kPortD[m]] -= zr[2 * NB + m] * dioG[m];
            if constexpr (NT > 0) for (int t = 0; t < NT; ++t) J[i][M::kPortT[t]] -= zr[2 * NB + ND + t] * tanhG[t];
            if constexpr (NR > 0) for (int m = 0; m < NR; ++m) J[i][M::kPortR[m]] -= zr[2 * NB + ND + NT + m] * resG[m];
            if constexpr (NQ > 0) for (int j = 0; j < NQ; ++j) J[i][M::kQPort[j]] -= zr[Q0 + j] * qG[j];
          }
          if (attempt == 0)
          {
            if (useBlk)
            {
              if (!blkReady || blkT != st.T) BuildBlocks(); luOk = BlockFactor(J);
              if (luOk)
              {
                denseLu = false;
                if (jReuse > 0)   // keep the factors of this step type for the next periods
                {
                  std::memcpy(jcj.J, J, sizeof J); std::memcpy(jcj.LU, blkLU, sizeof blkLU); std::memcpy(jcj.pv, blkPiv, sizeof blkPiv); std::memcpy(jcj.inv, blkInv, sizeof blkInv); std::memcpy(jcj.jf, jf, sizeof jf);
                  if constexpr (ND > 0) std::memcpy(jcj.dioG, dioG, sizeof dioG);
                  if constexpr (NT > 0) std::memcpy(jcj.tanhG, tanhG, sizeof tanhG);
                  if constexpr (NQ > 0) std::memcpy(jcj.qG, qG, sizeof qG);
                  jcj.valid = true; jcj.age = 0;
                }
                break;
              }
            }
            denseLu = true; luOk = LuFactorNoPivot<NP>(J); if (luOk) { for (int i = 0; i < NP; ++i) piv[i] = i; break; }
          }
          else { denseLu = true; luOk = LuFactor<NP>(J, piv); }
        }
        if (!luOk) break;
      }
      else                    // chord iteration: Jacobian (and its factors) of the first iteration of the step; the correction is already small there
      {
        EvalF(p, f1, nullptr); ++newtonIts;
        Residual<NP, NF>(p, pk, Zp, f1, F);
      }
      if (denseLu) LuSolve<NP>(J, piv, F); else BlockSolve(J, F);
      double mx = 0.0; for (int i = 0; i < NP; ++i) mx = std::fmax(mx, std::fabs(F[i]));
      lastMx = mx;
      const double sc = (mx > 0.5 ? 0.5 / mx : 1.0) * (attempt ? 0.3 : 1.0);
      for (int i = 0; i < NP; ++i) F[i] *= sc;
      bool limited = false;   // junction voltage limiting (SPICE pnjlim): a forward step of a junction beyond its critical voltage grows only logarithmically, so a junction does not run onto the exponential branch
      for (int k = 0; k < NB; ++k)
        for (int pt = 0; pt < 2; ++pt)
        {
          const int idx = pt == 0 ? M::kPortBe[k] : M::kPortBc[k]; if (idx < 0) continue;
          const double vold = p[idx], vnew = vold + F[idx];
          if (vnew > bp[k][15] && std::fabs(vnew - vold) > 2.0 * kVtPcm)
          {
            double vl; if (vold > 0.0) { const double arg = 1.0 + (vnew - vold) / kVtPcm; vl = arg > 0.0 ? vold + kVtPcm * std::log(arg) : bp[k][15]; } else vl = kVtPcm * std::log(vnew / kVtPcm);
            F[idx] = vl - vold; limited = true;
          }
        }
      if constexpr (ND > 0)
        for (int m = 0; m < ND; ++m)
        {
          const int idx = M::kPortD[m]; const double vold = p[idx], vnew = vold + F[idx];
          if (vnew > dp[m][4] && std::fabs(vnew - vold) > 2.0 * dp[m][1])
          {
            double vl; if (vold > 0.0) { const double arg = 1.0 + (vnew - vold) / dp[m][1]; vl = arg > 0.0 ? vold + dp[m][1] * std::log(arg) : dp[m][4]; } else vl = dp[m][1] * std::log(vnew / dp[m][1]);
            F[idx] = vl - vold; limited = true;
          }
        }
      for (int i = 0; i < NP; ++i) p[i] += F[i];
      if (mx < (reuse || (blockMode && !useBlk) ? tol * jReuseTol : tol) && !limited)   // converged (dense steps of block mode: the limit of the reused Jacobian): carry the currents to the corrected ports with the Jacobian of this step (error of second order in the correction)
      {
        for (int k = 0; k < NB; ++k)
        {
          const double d0 = F[M::kPortBe[k]], d1 = M::kPortBc[k] >= 0 ? F[M::kPortBc[k]] : 0.0;
          f1[2 * k] += jf[k][0] * d0 + jf[k][1] * d1; f1[2 * k + 1] += jf[k][2] * d0 + jf[k][3] * d1;
        }
        if constexpr (ND > 0) for (int m = 0; m < ND; ++m) f1[2 * NB + m] += dioG[m] * F[M::kPortD[m]];
        if constexpr (NT > 0) for (int t = 0; t < NT; ++t) f1[2 * NB + ND + t] += tanhG[t] * F[M::kPortT[t]];
        if constexpr (NR > 0) for (int m = 0; m < NR; ++m) f1[2 * NB + ND + NT + m] += resG[m] * F[M::kPortR[m]];
        if constexpr (NQ > 0) for (int j = 0; j < NQ; ++j) f1[Q0 + j] += qG[j] * F[M::kQPort[j]];
        fCurrent = true; break;
      }
    }
    }
    }
    PF_LAP(2);
    lastConverged = fCurrent; if (!fCurrent) EvalF(p, f1, nullptr);
    { long long c = newtonIts - itsBefore; if (reuse && (c > 2 || !fCurrent)) jcj.valid = false; itsByStep[stepIdx] += c; ++nByStep[stepIdx]; ++itHist[c < 11 ? c : 11]; }
    tapCount = 0;
    if constexpr (M::kTapN > 0)   // uniformly spaced output taps inside this step (state at the step start, currents at both ends, no state update)
    {
      for (int t = M::kTapFirst[j]; t < M::kTapFirst[j + 1]; ++t)
      {
        double y = M::kTap_oc[t] + acc * M::kTap_oacc[t];
        const double* oz = &M::kTap_oz[t * R]; for (int k = 0; k < R; ++k) y += oz[k] * z[k];
        const double* o0 = &M::kTap_of0[t * NF]; const double* o1 = &M::kTap_of1[t * NF]; for (int k = 0; k < NF; ++k) y += o0[k] * f0[k] + o1[k] * f1[k];
        const double* ua = &M::kTap_oua[t * NU]; const double* ub = &M::kTap_oub[t * NU];
        for (uint32_t m = bitsPrev; m; m &= m - 1) y += M::kVhigh * ua[Ctz(m)];
        for (uint32_t m = bitsNow; m; m &= m - 1) y += M::kVhigh * ub[Ctz(m)];
        tap[tapCount++] = y;
      }
    }
    AddProduct<R, NF>(zk, GbF, f1, z);
    double out = accOut;
    for (int k = 0; k < R; ++k) out += M::kXo[k] * z[k];
    for (uint32_t m = bitsNow; m; m &= m - 1) out += M::kVhigh * M::kDou[Ctz(m)];
    for (int k = 0; k < NF; ++k) out += M::kDof[k] * f1[k];
    for (int k = 0; k < NF; ++k) f0[k] = f1[k];
    bitsPrev = bitsNow; lastOut = out;
    PF_LAP(3);
    ++stepsDone;
    if (++stepIdx == M::kLayoutN[layout])
    {
      stepIdx = 0; ++period;
      const double dev = std::fabs(out - outDc); env = dev > env * kEnvDecay ? dev : env * kEnvDecay; if (env > peak) peak = env;
      acLow += kAcAlpha * (out - acLow); const double ac = std::fabs(out - acLow); acEnv = ac > acEnv * kEnvDecay ? ac : acEnv * kEnvDecay; if (acEnv > acPeak) acPeak = acEnv;
      const bool quiet = acEndRel > 0.0 ? period >= M::kPeriods && acEnv < std::fmin(acEndLevel, acEndRel * acPeak) : acEnv < acEndLevel && period > M::kPeriods / 2;
      if (acPeak > 0.0 && quiet) { if (++quietPeriods >= kAcEndPeriods) finished = true; } else quietPeriods = 0;
    }
    return out;
  }
};

}}  // namespace dgg::pcmfast
