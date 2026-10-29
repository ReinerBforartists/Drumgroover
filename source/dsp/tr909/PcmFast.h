// Reduced fast model of a TR-909 PCM voice analog path (CRASH / RIDE): linear dynamics exact (exponential integrator, piecewise-linear PCM inputs),
// BJT static currents interpolated linearly over the step, Newton on the BJT port voltages (vbe, vbc per device). Constants from gen_crash_model.py.
#pragma once
#include <cmath>
#include <cstdint>
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

constexpr double kVt = 0.025852, kGmin = 1e-12, kNewtonTol = 1e-3, kTailRel = 0.0, kEnvDecay = 0.975, kAcAlpha = 0.0031, kAcEndLevel = 2e-4; constexpr int kAcEndPeriods = 400;   // volts on the port voltages; the correction is applied before the test, so the error left is of the order of the square

// Gummel-Poon static currents of one BJT in node orientation: ic = s*cc (leaving the collector node into the device), ib = s*cb, and the 2x2 Jacobian wrt (vbe, vbc)
struct BjtOut { double ic, ib, d[2][2]; };
// Approximate evaluation for devices that stay below high injection (cbe/IKF < ~1e-3, checked by the generator) and have either vbc <= 0.1 V (the
// collector junction carries a negligible current: IS*exp(vbc/Vt) < 5e-12 A) or are evaluated with the exact junction term: no division, no sqrt.
inline BjtOut BjtEvalFast(const double* P, double vbe, double vbc)
{
  const double s = P[0], IS = P[1], ISE = P[8], invNFVt = P[9], invNEVt = P[10], invVAF = P[11], invIKF = P[12], invBF = P[13], invBR = P[14];
  const double eE = std::exp(std::fmin(vbe * invNFVt, 80.0));
  const double cbe = IS * (eE - 1.0) + kGmin * vbe, gbe = IS * eE * invNFVt + kGmin;
  double cbc = kGmin * vbc, gbc = kGmin;
  if (vbc > 0.1) { const double eC = std::exp(std::fmin(vbc * (1.0 / kVt), 80.0)); cbc = IS * (eC - 1.0) + kGmin * vbc; gbc = IS * eC * (1.0 / kVt) + kGmin; }
  double cben = 0.0, gben = 0.0;
  if (ISE != 0.0) { const double eL = std::exp(std::fmin(vbe * invNEVt, 80.0)); cben = ISE * (eL - 1.0); gben = ISE * eL * invNEVt; }
  const double iq1 = 1.0 - vbc * invVAF;                         // 1/q1 (Early effect)
  const double q2 = cbe * invIKF, dq2 = gbe * invIKF;
  const double g = 1.0 - q2 + 2.0 * q2 * q2, dg = (-1.0 + 4.0 * q2) * dq2;    // 1/(1 + q2 - q2^2), series in the small q2
  const double iq = iq1 * g, x = cbe - cbc;
  const double cc = x * iq - cbc * invBR, cb = cbe * invBF + cben + cbc * invBR;
  const double dccbe = gbe * iq + x * iq1 * dg, dccbc = -gbc * iq - x * (-invVAF) * 0.0 + x * (-invVAF) * g - gbc * invBR;
  const double dcbbe = gbe * invBF + gben, dcbbc = gbc * invBR;
  BjtOut o; o.ic = s * cc; o.ib = s * cb; o.d[0][0] = s * dccbe; o.d[0][1] = s * dccbc; o.d[1][0] = s * dcbbe; o.d[1][1] = s * dcbbc;
  return o;
}

inline BjtOut BjtEval(const double* P, double vbe, double vbc)
{
  const double s = P[0], IS = P[1], BR = P[3], ISE = P[8], invNFVt = P[9], invNEVt = P[10], invVAF = P[11], invIKF = P[12], invBF = P[13], invBR = P[14];
  const double eE = std::exp(std::fmin(vbe * invNFVt, 80.0)), eC = std::exp(std::fmin(vbc * (1.0 / kVt), 80.0));
  const double cbe = IS * (eE - 1.0) + kGmin * vbe, gbe = IS * eE * invNFVt + kGmin;
  const double cbc = IS * (eC - 1.0) + kGmin * vbc, gbc = IS * eC * (1.0 / kVt) + kGmin;
  double cben = 0.0, gben = 0.0;
  if (ISE != 0.0) { const double eL = std::exp(std::fmin(vbe * invNEVt, 80.0)); cben = ISE * (eL - 1.0); gben = ISE * eL * invNEVt; }
  const double q1 = 1.0 / (1.0 - vbc * invVAF), dq1 = q1 * q1 * invVAF;
  const double q2 = cbe * invIKF, arg = std::fmax(0.0, 1.0 + 4.0 * q2), sq = std::sqrt(arg), qb = q1 * (1.0 + sq) * 0.5;
  const double dsqbe = arg > 0.0 ? (2.0 * gbe * invIKF) / std::fmax(sq, 1e-12) : 0.0;
  const double dqbe = q1 * 0.5 * dsqbe, dqbc = dq1 * (1.0 + sq) * 0.5;
  const double iq = 1.0 / qb, x = cbe - cbc;
  const double cc = x * iq - cbc * invBR, cb = cbe * invBF + cben + cbc * invBR;
  const double dccbe = gbe * iq - x * iq * iq * dqbe, dccbc = -gbc * iq - x * iq * iq * dqbc - gbc * invBR;
  const double dcbbe = gbe * invBF + gben, dcbbc = gbc * invBR;
  BjtOut o; o.ic = s * cc; o.ib = s * cb; o.d[0][0] = s * dccbe; o.d[0][1] = s * dccbc; o.d[1][0] = s * dcbbe; o.d[1][1] = s * dcbbc;
  (void)BR;
  return o;
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

#ifdef PCMFAST_EXACT_BJT
#define BjtEvalSel BjtEval
#else
#define BjtEvalSel BjtEvalFast
#endif

template <class M> struct Voice
{
  static constexpr int R = M::kR, NU = M::kU, NP = M::kPorts, NF = M::kCur, NB = M::kNumBjt, NBITS = M::kBits, IACC = M::kAccentIdx;
  double coneEff[M::kSteps * R], pd1Eff[NP], accOut = 0.0;   // constant forcing of every step type including the accent input (RefreshAccent)
  double z[R], f0[NF], p[NP], u0[NU], vbcFix[NB], acc = M::kAccNom;   // vbcFix: vbc of the devices whose vbc is not a port (DC value at the accent)
    // acc: accent voltage of the hit (run-time input)
  double bp[NB][16];   // BJT parameters with precomputed reciprocals (BjtEval)
  int period = 0, stepIdx = 0, layout = 0; uint32_t bitsNow = 0, bitsPrev = 0;   // stepIdx counts within the layout of the current period
  double acLow = 0.0, acEnv = 0.0, acPeak = 0.0; long quietPeriods = 0; bool finished = false;   // audio end: 20 Hz high-passed output envelope below acEndLevel for acEndPeriods periods after its peak
  double lastOut = 0.0, lastH = 0.0, outDc = 0.0, env = 0.0, peak = 1e-12;   // last step length, DC output, level envelope and its maximum (layout choice)
  long periodsByLayout[4] = {0, 0, 0, 0}; double tailRel = kTailRel, newtonTol = kNewtonTol; int noSolve = 0;   // noSolve: timing probe only (no nonlinear solve); tailRel: switch to the cheaper layout below this fraction of the loudest envelope
  double prof[6] = {0, 0, 0, 0, 0, 0};
  uint32_t flipMask = 0;   // diagnosis only: address bits inverted while held
  double tap[16]; int tapCount = 0;   // output taps of the last Step (uniform instants m*T/kTaps inside the step)
  int freezeAH = -1;   // period whose address bits are held (default: the last one)
  int freezeA = -1;   // diagnosis only: from this period on the address bits (6..11) are at their final value
  int freezeP = -1;   // diagnosis only: from the period after freezeP on, the data bits (0..5) stay at their value in period freezeP
  long newtonIts = 0, stepsDone = 0; long itsByStep[8] = {0,0,0,0,0,0,0,0}, nByStep[8] = {0,0,0,0,0,0,0,0}, itHist[12] = {0,0,0,0,0,0,0,0,0,0,0,0};

  void RefreshAccent()
  {
    for (int j = 0; j < M::kSteps; ++j) { const double* ca = &M::kGsUc[(j * NU + IACC) * R]; for (int i = 0; i < R; ++i) coneEff[j * R + i] = M::kCone[j * R + i] + acc * ca[i]; }
    for (int i = 0; i < NP; ++i) pd1Eff[i] = M::kPD1[i] + acc * M::kPDuc[IACC * NP + i];
    accOut = M::kDo1 + acc * M::kDou[IACC];
  }
  void Reset()
  {
    RefreshAccent();
    {   // initial state of the hit: DC solution at the accent, interpolated linearly in the accent grid
      int g = 0; while (g < M::kAccGrid - 2 && acc >= M::kAccV[g + 1]) ++g;
      double t = (acc - M::kAccV[g]) / (M::kAccV[g + 1] - M::kAccV[g]); t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
      for (int i = 0; i < R; ++i) z[i] = (1.0 - t) * M::kDcZ[g * R + i] + t * M::kDcZ[(g + 1) * R + i];
      for (int i = 0; i < NP; ++i) p[i] = (1.0 - t) * M::kDcP[g * NP + i] + t * M::kDcP[(g + 1) * NP + i];
      for (int k = 0; k < NB; ++k) vbcFix[k] = (1.0 - t) * M::kDcVbc[g * NB + k] + t * M::kDcVbc[(g + 1) * NB + k];
    }
    for (int k = 0; k < NB; ++k)
    {
      const double* q = &M::kBjt[9 * k]; for (int i = 0; i < 9; ++i) bp[k][i] = q[i];
      bp[k][9] = 1.0 / (q[4] * kVt); bp[k][10] = 1.0 / (q[7] * kVt); bp[k][11] = 1.0 / q[5]; bp[k][12] = 1.0 / q[6]; bp[k][13] = 1.0 / q[2]; bp[k][14] = 1.0 / q[3]; bp[k][15] = 0.0;
    }
    for (int i = 0; i < NU; ++i) u0[i] = M::kU0[i];
    bitsPrev = 0; for (int i = 0; i < NBITS; ++i) if (M::kU0[i] > 0.5 * M::kVhigh) bitsPrev |= (1u << i);
    EvalF(p, f0, nullptr); lastOut = OutputNow(); period = 0; stepIdx = 0; layout = 0; newtonIts = 0; stepsDone = 0; outDc = OutputNow(); env = 0.0; peak = 1e-12; acLow = outDc; acEnv = 0.0; acPeak = 0.0; quietPeriods = 0; finished = false;
    for (int i = 0; i < 4; ++i) periodsByLayout[i] = 0;
  }
  // restart of the PCM sequence while the analog state continues (the digital counters are reset, the capacitors keep their charge)
  void Retrigger()
  {
    period = 0; stepIdx = 0; layout = 0; finished = false; env = 0.0; peak = 1e-12; acLow = lastOut; acEnv = 0.0; acPeak = 0.0; quietPeriods = 0;
  }
  // currents f (and the block Jacobians when jf != nullptr) at the port voltages q
  void EvalF(const double* q, double* f, double (*jf)[4]) const
  {
    for (int k = 0; k < NB; ++k)
    {
      const double vbe = q[M::kPortBe[k]], vbc = M::kPortBc[k] >= 0 ? q[M::kPortBc[k]] : vbcFix[k];
      const BjtOut o = BjtEvalSel(bp[k], vbe, vbc);
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
    if (stepIdx == 0)   // new PCM period: new bits at the end of the ramp; the layout of the period follows the signal level
    {
      bitsNow = (period < M::kPeriods) ? M::kPcm[period] : M::kPcm[M::kPeriods - 1];
      if (freezeA >= 0 && period >= freezeA) bitsNow = (bitsNow & 0x3Fu) | (M::kPcm[freezeAH >= 0 ? freezeAH : M::kPeriods - 1] & ~0x3Fu), bitsNow ^= flipMask;
      if (freezeP >= 0 && period > freezeP) bitsNow = (bitsNow & ~0x3Fu) | (M::kPcm[freezeP] & 0x3Fu);
      layout = (M::kLayouts > 1 && env < tailRel * peak) ? 1 : 0; ++periodsByLayout[layout];
    }
    const int j = M::kLayoutFirst[layout] + stepIdx; lastH = M::kStepLen[j];
    const double* Phi = &M::kPhi[j * R * R];
    const double* GaF = &M::kGaF[j * R * NF]; const double* GbF = &M::kGbF[j * R * NF]; const double* Cone = &coneEff[j * R]; const double* Zp = &M::kZp[j * NP * NF];
    double zk[R];
    for (int i = 0; i < R; ++i)
    {
      double a = Cone[i];
      const double* row = Phi + i * R; for (int k = 0; k < R; ++k) a += row[k] * z[k];
      const double* rf = GaF + i * NF; for (int k = 0; k < NF; ++k) a += rf[k] * f0[k];
      zk[i] = a;
    }
    if (stepIdx == 0)   // ramp: old bits ramp to the new bits
    {
      for (uint32_t m = bitsPrev; m; m &= m - 1) { const int k = Ctz(m); const double* c = &M::kGaUc[(j * NU + k) * R]; for (int i = 0; i < R; ++i) zk[i] += M::kVhigh * c[i]; }
      for (uint32_t m = bitsNow; m; m &= m - 1) { const int k = Ctz(m); const double* c = &M::kGbUc[(j * NU + k) * R]; for (int i = 0; i < R; ++i) zk[i] += M::kVhigh * c[i]; }
    }
    else          // hold: constant bits
      for (uint32_t m = bitsNow; m; m &= m - 1) { const int k = Ctz(m); const double* c = &M::kGsUc[(j * NU + k) * R]; for (int i = 0; i < R; ++i) zk[i] += M::kVhigh * c[i]; }
    PF_LAP(0);
    double pk[NP];
    for (int i = 0; i < NP; ++i)
    {
      double a = pd1Eff[i];
      const double* rx = &M::kPXz[i * R]; for (int k = 0; k < R; ++k) a += rx[k] * zk[k];
      pk[i] = a;
    }
    for (uint32_t m = bitsNow; m; m &= m - 1) { const int k = Ctz(m); const double* c = &M::kPDuc[k * NP]; for (int i = 0; i < NP; ++i) pk[i] += M::kVhigh * c[i]; }
    PF_LAP(1);
    double f1[NF]; const long itsBefore = newtonIts; bool fCurrent = false;
    if (!noSolve)
    {
    double J[NP][NP]; int piv[NP]; double jf[NB][4]; bool luOk = false;
    for (int it = 0; it < 40; ++it)
    {
      double F[NP];
      if (it == 0 || !luOk)   // full Newton iteration: currents, block Jacobians, factorization
      {
        EvalF(p, f1, jf); ++newtonIts;
        for (int attempt = 0; attempt < 2; ++attempt)   // 1st: diagonal pivots (J = I - Zp Jf is diagonally dominant in practice); 2nd: partial pivoting if a pivot was small
        {
          for (int i = 0; i < NP; ++i)
          {
            double zf = 0.0; const double* zr = Zp + i * NF; for (int k = 0; k < NF; ++k) zf += zr[k] * f1[k];
            F[i] = -(p[i] - pk[i] - zf);
            for (int c = 0; c < NP; ++c) J[i][c] = (i == c ? 1.0 : 0.0);
            for (int k = 0; k < NB; ++k)
            {
              J[i][M::kPortBe[k]] -= zr[2 * k] * jf[k][0] + zr[2 * k + 1] * jf[k][2];
              if (M::kPortBc[k] >= 0) J[i][M::kPortBc[k]] -= zr[2 * k] * jf[k][1] + zr[2 * k + 1] * jf[k][3];
            }
          }
          if (attempt == 0) { luOk = LuFactorNoPivot<NP>(J); if (luOk) { for (int i = 0; i < NP; ++i) piv[i] = i; break; } }
          else luOk = LuFactor<NP>(J, piv);
        }
        if (!luOk) break;
      }
      else                    // chord iteration: Jacobian (and its factors) of the first iteration of the step; the correction is already small there
      {
        EvalF(p, f1, nullptr); ++newtonIts;
        for (int i = 0; i < NP; ++i) { double zf = 0.0; const double* zr = Zp + i * NF; for (int k = 0; k < NF; ++k) zf += zr[k] * f1[k]; F[i] = -(p[i] - pk[i] - zf); }
      }
      LuSolve<NP>(J, piv, F);
      double mx = 0.0; for (int i = 0; i < NP; ++i) mx = std::fmax(mx, std::fabs(F[i]));
      const double sc = mx > 0.5 ? 0.5 / mx : 1.0;
      for (int i = 0; i < NP; ++i) p[i] += sc * F[i];
      if (mx < newtonTol)   // converged: carry the currents to the corrected ports with the Jacobian of this step (error of second order in the correction)
      {
        for (int k = 0; k < NB; ++k)
        {
          const double d0 = sc * F[M::kPortBe[k]], d1 = M::kPortBc[k] >= 0 ? sc * F[M::kPortBc[k]] : 0.0;
          f1[2 * k] += jf[k][0] * d0 + jf[k][1] * d1; f1[2 * k + 1] += jf[k][2] * d0 + jf[k][3] * d1;
        }
        fCurrent = true; break;
      }
    }
    }
    PF_LAP(2);
    if (!fCurrent) EvalF(p, f1, nullptr);
    { long c = newtonIts - itsBefore; itsByStep[stepIdx] += c; ++nByStep[stepIdx]; ++itHist[c < 11 ? c : 11]; }
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
    for (int i = 0; i < R; ++i) { double a = zk[i]; const double* rb = GbF + i * NF; for (int k = 0; k < NF; ++k) a += rb[k] * f1[k]; z[i] = a; }
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
      if (acPeak > 0.0 && acEnv < kAcEndLevel && period > M::kPeriods / 2) { if (++quietPeriods >= kAcEndPeriods) finished = true; } else quietPeriods = 0;
    }
    return out;
  }
};

}}  // namespace dgg::pcmfast
