// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Reiner Prokein
#pragma once
// TR-808 / Yocto bass drum, fast model derived from the BD sheet of the common NGSpice circuit.
//
// The circuit splits into two parts that do not feed back into each other (VR4 has no influence on the first, VR5 0.002 %):
//   resonator       trigger network, gate transistors Q39-Q43, bridge-T, op-amps U12A/U12B and the decay pot VR6; output node n55. It depends on the accent voltage and VR6 only.
//   output network  R171 ... bus load (tone pot VR5, level pot VR4, follower Q44): linear (Q44 linearised around its rest state, exact to 0.006 %), driven by n55.
// Phase 1 (the first 8 ms after the trigger, all transistors switching): tables per MIDI velocity and VR6 anchor (cubic spline in VR6): the state of the resonator at t1 and its output n55 on a
// knot grid (384 kHz around the two trigger edges, 48 kHz elsewhere). The output network is integrated over the knots (trapezoid) and sampled at the host rate.
// From t1 on, the resonator is a linear system plus the three devices that keep switching with the swing (Q42, Q43, D52), solved per host sample like the engine does it (trapezoid, Newton on the
// nodes of the three devices), but with precomputed dense matrices; the output network follows with its own trapezoid step.

#include "808KickModel.h"
#include "808KickAccentTables.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace dgg { namespace kickfast {

namespace detail {
constexpr int N = kickmodel::kN, K = kickmodel::kNumS, NO = kickmodel::kO_N;
constexpr double kVt = 0.0258649, kDiodeIs = 4.352e-9, kDiodeN = 1.906;
constexpr double kT1 = 0.008, kTick = 1.0 / 384000.0;
constexpr int kTailDecN = 4; constexpr double kTailDec = kTailDecN, kTailEnterRel = 1e-3;   // tail step in host samples; power relative to the peak power (-30 dB) from which the tail steps are used
constexpr double kNewtonTol = 1e-8;   // volts: residual of the device nodes below which the step is accepted (the silence cutoff of the voice is 1e-6 V)
static_assert(kickmodel::kNumS == 5 && kickmodel::kS[0] == 6 && kickmodel::kS[1] == 7 && kickmodel::kS[2] == 8 && kickmodel::kS[3] == 9 && kickmodel::kS[4] == 10, "device node layout of Devices()");
static_assert(kickmodel::kQ42[0] == 7 && kickmodel::kQ42[1] == 6 && kickmodel::kQ42[2] == -1 && kickmodel::kQ43[0] == 9 && kickmodel::kQ43[1] == 8 && kickmodel::kQ43[2] == -1 && kickmodel::kD52[0] == 10 && kickmodel::kD52[1] == -1, "device nodes");

struct BjtModel { double is, bf, nf, ise, ne, ikf, vaf, br, nr, tf, tr, cje, vje, mje, cjc, vjc, mjc, fc, cjs; };
inline const BjtModel& Npn() { static const BjtModel m{35.1e-15, 222.31, 1.0, 6.63e-15, 1.319, 0.130, 315.0, 1.0, 1.0, 0.5564e-9, 10e-9, 20e-12, 0.75, 0.33, 6.4e-12, 0.75, 0.33, 0.5, 2e-12}; return m; }

// Gummel-Poon collector and base currents with analytic derivatives (NPN, emitter at ground): ic, ib and d/dvbe, d/dvbc of both
struct BjtI { double ic, ib, icbe, icbc, ibbe, ibbc; };
inline double LimExp(double x, double& d) { if (x > 40.0) { const double e = std::exp(40.0); d = e; return e * (1.0 + (x - 40.0)); } const double e = std::exp(x); d = e; return e; }
inline BjtI Bjt(const BjtModel& m, double vbe, double vbc)
{
  double de1, de2, de3;
  const double e1 = LimExp(vbe / (m.nf * kVt), de1), e2 = LimExp(vbc / (m.nr * kVt), de2), e3 = LimExp(vbe / (m.ne * kVt), de3);
  const double cbe = m.is * (e1 - 1.0), cbc = m.is * (e2 - 1.0), ile = m.ise * (e3 - 1.0);
  const double dcbe = m.is * de1 / (m.nf * kVt), dcbc = m.is * de2 / (m.nr * kVt), dile = m.ise * de3 / (m.ne * kVt);
  const double den = 1.0 - vbc / m.vaf; const bool clampQ1 = den < 0.05; const double q1 = 1.0 / (clampQ1 ? 0.05 : den), dq1 = clampQ1 ? 0.0 : q1 * q1 / m.vaf;
  const bool pos = cbe > 0.0; const double q2 = (pos ? cbe : 0.0) / m.ikf, dq2 = pos ? dcbe / m.ikf : 0.0;
  const double sq = std::sqrt(1.0 + 4.0 * q2), qb = 0.5 * q1 * (1.0 + sq), dqbe = 0.5 * q1 * (2.0 * dq2 / sq), dqbc = 0.5 * dq1 * (1.0 + sq);
  BjtI r; const double iq = 1.0 / qb, x = cbe - cbc;
  r.ic = x * iq - cbc / m.br; r.ib = cbe / m.bf + ile + cbc / m.br;
  r.icbe = dcbe * iq - x * dqbe * iq * iq; r.icbc = -dcbc * iq - x * dqbc * iq * iq - dcbc / m.br;
  r.ibbe = dcbe / m.bf + dile; r.ibbc = dcbc / m.br;
  return r;
}

// Gauss-Jordan inverse with partial pivoting (n <= 20)
template <int M> inline bool Invert(const double (*A)[M], double (*Inv)[M], int n)
{
  double W[M][2 * M];
  for (int i = 0; i < n; ++i) { for (int j = 0; j < n; ++j) { W[i][j] = A[i][j]; W[i][n + j] = (i == j) ? 1.0 : 0.0; } }
  for (int c = 0; c < n; ++c)
  {
    int p = c; for (int r = c + 1; r < n; ++r) if (std::fabs(W[r][c]) > std::fabs(W[p][c])) p = r;
    if (std::fabs(W[p][c]) < 1e-300) return false;
    if (p != c) for (int j = 0; j < 2 * n; ++j) std::swap(W[c][j], W[p][j]);
    const double d = 1.0 / W[c][c]; for (int j = 0; j < 2 * n; ++j) W[c][j] *= d;
    for (int r = 0; r < n; ++r) if (r != c) { const double f = W[r][c]; if (f != 0.0) for (int j = 0; j < 2 * n; ++j) W[r][j] -= f * W[c][j]; }
  }
  for (int i = 0; i < n; ++i) for (int j = 0; j < n; ++j) Inv[i][j] = W[i][n + j];
  return true;
}

// not-a-knot cubic spline through the 9 VR6 anchors: weights w[j] with value(x) = sum_j w[j] y[j]
struct SplineBasis
{
  static constexpr int NA = kickmodel::kNumAnchor;
  double S[NA][NA];   // second derivatives M = S y
  SplineBasis()
  {
    double A[NA][NA] = {}; const double* x = kickmodel::kAnchors; double h[NA - 1]; for (int i = 0; i < NA - 1; ++i) h[i] = x[i + 1] - x[i];
    // not-a-knot at both ends: third derivative continuous at x1 and x[NA-2]
    A[0][0] = -1.0 / h[0]; A[0][1] = 1.0 / h[0] + 1.0 / h[1]; A[0][2] = -1.0 / h[1];
    for (int i = 1; i < NA - 1; ++i) { A[i][i - 1] = h[i - 1] / 6.0; A[i][i] = (h[i - 1] + h[i]) / 3.0; A[i][i + 1] = h[i] / 6.0; }
    A[NA - 1][NA - 3] = -1.0 / h[NA - 3]; A[NA - 1][NA - 2] = 1.0 / h[NA - 3] + 1.0 / h[NA - 2]; A[NA - 1][NA - 1] = -1.0 / h[NA - 2];
    double Ainv[NA][NA]; Invert<NA>(A, Ainv, NA);
    // right-hand side: rows 1..NA-2: (y[i+1]-y[i])/h[i] - (y[i]-y[i-1])/h[i-1]; rows 0 and NA-1: 0
    for (int i = 0; i < NA; ++i) for (int j = 0; j < NA; ++j)
    {
      double s = 0.0;
      for (int r = 1; r < NA - 1; ++r)
      {
        double d = 0.0;   // d(rhs_r) / d y_j
        if (j == r + 1) d += 1.0 / h[r]; if (j == r) d += -1.0 / h[r] - 1.0 / h[r - 1]; if (j == r - 1) d += 1.0 / h[r - 1];
        s += Ainv[i][r] * d;
      }
      S[i][j] = s;
    }
  }
  void Weights(double xv, double* w) const
  {
    const double* x = kickmodel::kAnchors; xv = std::min(std::max(xv, x[0]), x[NA - 1]);
    int i = 0; while (i < NA - 2 && xv > x[i + 1]) ++i;
    const double h = x[i + 1] - x[i], a = x[i + 1] - xv, b = xv - x[i];
    for (int j = 0; j < NA; ++j)
    {
      const double Mi = S[i][j], Mj = S[i + 1][j], yi = (j == i) ? 1.0 : 0.0, yj = (j == i + 1) ? 1.0 : 0.0;
      w[j] = (Mi * a * a * a + Mj * b * b * b) / (6.0 * h) + (yi / h - Mi * h / 6.0) * a + (yj / h - Mj * h / 6.0) * b;
    }
  }
};
inline const SplineBasis& Spline() { static const SplineBasis s; return s; }
}  // namespace detail

class TR808KickFast
{
public:
  struct Pots { double vr4 = 1.0, vr5 = 0.5, vr6 = 0.5; };
  // false: every host sample is stepped (the plain render); true: the quiet tail is stepped coarsely, see TrackLevel()
  bool adaptiveTail = true;   // fractions of the pots as the engine takes them (level, tone, decay)

  TR808KickFast() { detail::Spline(); SetSampleRate(48000.0); }

  void SetSampleRate(double sr) { if (sr == mSr) return; mSr = sr; mValid = false; }
  void SetPots(const Pots& p) { if (mValid && p.vr4 == mP.vr4 && p.vr5 == mP.vr5 && p.vr6 == mP.vr6) return; mP = p; mValid = false; }

  // Starts a hit at the MIDI velocity 1..127 (accent voltage vel/127 * 4.09 V). False: no hit.
  bool Start(int vel127)
  {
    using namespace detail;
    if (vel127 < 1 || vel127 > kickmodel::kNumVel) { mActive = false; return false; }
    if (!mValid) Build();
    return StartTables(kickmodel::kState + (size_t)(vel127 - 1) * kickmodel::kNumAnchor * detail::N, kickmodel::kN55 + (size_t)(vel127 - 1) * kickmodel::kNumAnchor * kickmodel::kNumKnots, (double)vel127 / 127.0 * 4.09);
  }

  // Starts a hit at an accent 0..1 of the 808 trigger range: 0 = the full MIDI-velocity accent (4.09 V, exactly Start(127)), 1 = 14 V (accent voltage 4.09 + accent * 9.91 V). Between the
  // table nodes (kickmodel::kNumAccX of them above 4.09 V) the phase-1 tables are interpolated linearly in the accent voltage.
  bool StartAccent(double accent01)
  {
    using namespace detail;
    if (!(accent01 > 0.0)) return Start(kickmodel::kNumVel);
    if (accent01 > 1.0) accent01 = 1.0;
    if (!mValid) Build();
    constexpr size_t kSt = (size_t)kickmodel::kNumAnchor * N, kKn = (size_t)kickmodel::kNumAnchor * kickmodel::kNumKnots;
    const double u = accent01 * kickmodel::kNumAccX; int i0 = (int)u; if (i0 >= kickmodel::kNumAccX) i0 = kickmodel::kNumAccX - 1;
    const double f = u - i0;                                                  // node i0 (0 = the velocity-127 row) .. i0 + 1
    const double* s0 = i0 == 0 ? kickmodel::kState + (size_t)(kickmodel::kNumVel - 1) * kSt : kickmodel::kStateX + (size_t)(i0 - 1) * kSt; const double* s1 = kickmodel::kStateX + (size_t)i0 * kSt;
    const float* k0 = i0 == 0 ? kickmodel::kN55 + (size_t)(kickmodel::kNumVel - 1) * kKn : kickmodel::kN55X + (size_t)(i0 - 1) * kKn; const float* k1 = kickmodel::kN55X + (size_t)i0 * kKn;
    for (size_t i = 0; i < kSt; ++i) mAccSt[i] = s0[i] + f * (s1[i] - s0[i]);
    for (size_t i = 0; i < kKn; ++i) mAccKn[i] = (float)((double)k0[i] + f * ((double)k1[i] - (double)k0[i]));
    return StartTables(mAccSt, mAccKn, kickmodel::kAccentLoVolt + accent01 * (kickmodel::kAccentHiVolt - kickmodel::kAccentLoVolt));
  }

private:
  bool StartTables(const double* st, const float* kn, double accentVolt)
  {
    using namespace detail;
    mAccent = accentVolt;
    // tables -> state at t1 and n55 knots for this VR6
    double w[SplineBasis::NA]; Spline().Weights(mP.vr6, w);
    for (int i = 0; i < N; ++i) { double s = 0.0; for (int a = 0; a < kickmodel::kNumAnchor; ++a) s += w[a] * st[a * N + i]; mV[i] = s; }
    // output network over the knots (deviations from its rest state; input n55 piecewise linear between the knots)
    double x[NO]; for (int i = 0; i < NO; ++i) x[i] = 0.0;
    double u0 = kickmodel::kN55Rest - kickmodel::kN55Rest, tickPrev = 0.0;   // deviation of the input: zero at the start
    mKnotT[0] = 0.0; mKnotY[0] = kickmodel::kO_Rest[kickmodel::kO_Out];
    for (int k = 0; k < kickmodel::kNumKnots; ++k)
    {
      double n55 = 0.0; for (int a = 0; a < kickmodel::kNumAnchor; ++a) n55 += w[a] * (double)kn[a * kickmodel::kNumKnots + k];
      const double u1 = n55 - kickmodel::kN55Rest, tick = (double)kickmodel::kKnotTicks[k]; const OutSys& os = (tick - tickPrev < 1.5) ? mOut1 : mOut8;
      double xn[NO]; const double ub = 0.5 * (u0 + u1);
      for (int i = 0; i < NO; ++i) { double s = os.Psi[i] * ub; for (int j = 0; j < NO; ++j) s += os.Phi[i][j] * x[j]; xn[i] = s; }
      for (int i = 0; i < NO; ++i) x[i] = xn[i];
      mKnotT[k + 1] = tick * kTick; mKnotY[k + 1] = x[kickmodel::kO_Out] + kickmodel::kO_Rest[kickmodel::kO_Out];
      u0 = u1; tickPrev = tick; mU1 = n55;
    }
    for (int i = 0; i < NO; ++i) mXo[i] = x[i];
    mI = 0; mPhase = 0; mActive = true; mKnotPos = 0; mTail = false; mTailPh = 0; mPow = mPowPeak = 0.0;
    // forcing and constant vector of the resonator (accent enters linearly)
    for (int i = 0; i < N; ++i) mForce[i] = kickmodel::kF0[i] + mAccent * kickmodel::kFa[i] - kickmodel::kC01[i];
    return true;
  }

public:
  bool Active() const { return mActive; }
  static double RestOutput() { return kickmodel::kO_Rest[kickmodel::kO_Out]; }   // output bus voltage before the trigger (DC level of the circuit)

  // Next output sample (volts at the output bus node bd_out), at t = (i + 1) / sr after the trigger.
  double Step()
  {
    using namespace detail;
    if (!mActive) return kickmodel::kO_Rest[kickmodel::kO_Out];
    ++mI; const double t = (double)mI / mSr;
    if (mPhase == 0)
    {
      if (t <= kT1 * (1.0 + 1e-9)) return Interp(t);
      // first host sample after t1: gap step from t1 to t, then regular steps
      mPhase = 1; const double g = t - kT1;
      if (std::fabs(g - 1.0 / mSr) < 1e-12) { mResUse = &mRes; mOutUse = &mOutHost; std::memset(&mResGap, 0, sizeof(mResGap)); }
      else { BuildRes(mResGap, g); BuildOut(mOutGap, g); mResUse = &mResGap; mOutUse = &mOutGap; }
      InitDevices();
      return Advance();
    }
    if (mTail) return TailSample();
    mResUse = &mRes; mOutUse = &mOutHost;
    const double y = Advance();
    TrackLevel(y);
    return y;
  }

private:
  // Quiet tail: once the output power (10 ms mean) is 30 dB below its peak, the resonator and the output network are stepped at kTailDec host samples per step (the tail is a slowly decaying
  // low-frequency oscillation) and the host samples in between are cubic Lagrange interpolated. The trapezoid frequency error grows with the square of the step (6e-5 at 50 Hz and 4 samples:
  // an amplitude/phase drift of about 2 % after 1.5 s); the difference to the plain host-rate render is below -95 dB re peak.
  double AdvanceTail() { mResUse = &mResTail; mOutUse = &mOutTail; return Advance(); }
  void TrackLevel(double y)
  {
    mHist[mI & 7] = y;
    const double d = y - kickmodel::kO_Rest[kickmodel::kO_Out];
    mPow += (d * d - mPow) * (1.0 / (0.01 * mSr)); if (mPow > mPowPeak) mPowPeak = mPow;
    if (adaptiveTail && mI > (long)(0.02 * mSr) && mPow < mPowPeak * detail::kTailEnterRel)
    {
      for (int i = 0; i < detail::N; ++i) { double s = 0.0; for (int j = 0; j < detail::N; ++j) s += mResTail.Ainv[i][j] * mForce[j]; mWTail[i] = s; }
      mQ[0] = mHist[(mI - detail::kTailDecN) & 7]; mQ[1] = y; mQ[2] = AdvanceTail(); mQ[3] = AdvanceTail(); mTail = true; mTailPh = 0;
    }
  }
  double TailSample()
  {
    ++mTailPh; const double f = (double)mTailPh / detail::kTailDec;
    // cubic Lagrange through the points at -1, 0, 1, 2 (in units of the tail step)
    const double v = mQ[0] * (-f * (f - 1.0) * (f - 2.0) / 6.0) + mQ[1] * ((f + 1.0) * (f - 1.0) * (f - 2.0) / 2.0) + mQ[2] * (-(f + 1.0) * f * (f - 2.0) / 2.0) + mQ[3] * ((f + 1.0) * f * (f - 1.0) / 6.0);
    if (mTailPh >= detail::kTailDecN) { mQ[0] = mQ[1]; mQ[1] = mQ[2]; mQ[2] = mQ[3]; mQ[3] = AdvanceTail(); mTailPh = 0; }
    return v;
  }
  using ResSys = struct { double Phi[detail::N][detail::N]; double Ainv[detail::N][detail::N]; double AinvS[detail::N][detail::K]; double M[detail::K][detail::K]; double h; double G0[detail::N][detail::N]; };
  using OutSys = struct { double Phi[detail::NO][detail::NO]; double Psi[detail::NO]; };

  void BuildRes(ResSys& rs, double h) const
  {
    using namespace detail;
    const double R = std::max(kickmodel::kVr6FullOhm * mP.vr6, 1.0), g6 = 1.0 / R;
    double A[N][N], B[N][N]; rs.h = h;
    for (int i = 0; i < N; ++i) for (int j = 0; j < N; ++j)
    {
      double G = kickmodel::kG1[i * N + j];
      if ((i == kickmodel::kVr6a && j == kickmodel::kVr6a) || (i == kickmodel::kVr6b && j == kickmodel::kVr6b)) G += g6;
      if ((i == kickmodel::kVr6a && j == kickmodel::kVr6b) || (i == kickmodel::kVr6b && j == kickmodel::kVr6a)) G -= g6;
      const double C = kickmodel::kC1[i * N + j];
      A[i][j] = C / h + 0.5 * G; B[i][j] = C / h - 0.5 * G;
    }
    Invert<N>(A, rs.Ainv, N);
    for (int i = 0; i < N; ++i) for (int j = 0; j < N; ++j) { double s = 0.0; for (int k = 0; k < N; ++k) s += rs.Ainv[i][k] * B[k][j]; rs.Phi[i][j] = s; }
    for (int i = 0; i < N; ++i) for (int j = 0; j < K; ++j) rs.AinvS[i][j] = rs.Ainv[i][kickmodel::kS[j]];
    for (int i = 0; i < K; ++i) for (int j = 0; j < K; ++j) rs.M[i][j] = rs.AinvS[kickmodel::kS[i]][j];
  }

  void BuildOut(OutSys& os, double h) const
  {
    using namespace detail;
    double G[NO][NO], A[NO][NO], B[NO][NO], Ai[NO][NO];
    for (int i = 0; i < NO; ++i) for (int j = 0; j < NO; ++j) G[i][j] = kickmodel::kO_G[i * NO + j];
    for (int p = 0; p < 3; ++p)
    {
      const int pot = kickmodel::kO_Pot[p][0], ia = kickmodel::kO_Pot[p][1], ib = kickmodel::kO_Pot[p][2];
      const double r = std::max(pot == 1 ? kickmodel::kO_PotOhm[p] * (1.0 - mP.vr4) : pot == 2 ? kickmodel::kO_PotOhm[p] * mP.vr4 : kickmodel::kO_PotOhm[p] * mP.vr5, 1.0), g = 1.0 / r;
      if (ia >= 0) G[ia][ia] += g;
      if (ib >= 0) G[ib][ib] += g;
      if (ia >= 0 && ib >= 0) { G[ia][ib] -= g; G[ib][ia] -= g; }
    }
    for (int i = 0; i < NO; ++i) for (int j = 0; j < NO; ++j) { const double C = kickmodel::kO_C[i * NO + j]; A[i][j] = C / h + 0.5 * G[i][j]; B[i][j] = C / h - 0.5 * G[i][j]; }
    Invert<NO>(A, Ai, NO);
    for (int i = 0; i < NO; ++i)
    {
      for (int j = 0; j < NO; ++j) { double s = 0.0; for (int k = 0; k < NO; ++k) s += Ai[i][k] * B[k][j]; os.Phi[i][j] = s; }
      double s = 0.0; for (int k = 0; k < NO; ++k) s += Ai[i][k] * kickmodel::kO_Fa[k]; os.Psi[i] = s;
    }
  }

  void Build()
  {
    BuildRes(mRes, 1.0 / mSr); BuildOut(mOutHost, 1.0 / mSr); BuildOut(mOut1, detail::kTick); BuildOut(mOut8, 8.0 * detail::kTick);
    BuildRes(mResTail, detail::kTailDec / mSr); BuildOut(mOutTail, detail::kTailDec / mSr); mValid = true;
  }

  double Interp(double t) const   // 4-point Lagrange through the knot outputs of phase 1 (exact at the knots)
  {
    const int n = kickmodel::kNumKnots + 1;
    int k = mKnotPos; while (k < n - 2 && mKnotT[k + 1] < t - 1e-15) ++k; while (k > 0 && mKnotT[k] > t + 1e-15) --k;
    mKnotPos = k;
    if (std::fabs(t - mKnotT[k]) < 1e-12) return mKnotY[k];
    if (std::fabs(t - mKnotT[k + 1]) < 1e-12) return mKnotY[k + 1];
    int i0 = std::min(std::max(k - 1, 0), n - 4);
    double s = 0.0;
    for (int a = 0; a < 4; ++a)
    {
      double l = 1.0; for (int b = 0; b < 4; ++b) if (b != a) l *= (t - mKnotT[i0 + b]) / (mKnotT[i0 + a] - mKnotT[i0 + b]);
      s += l * mKnotY[i0 + a];
    }
    return s;
  }

  // device currents (leaving the node) at the S nodes of the resonator and their Jacobian; Q42, Q43 (emitter at ground) and D52. The junction charges of the three devices are not modelled (0.03 % of the swing).
  static void Devices(const double* y, double* f, double (*Jf)[detail::K])
  {
    using namespace detail;
    for (int i = 0; i < K; ++i) { f[i] = 0.0; for (int j = 0; j < K; ++j) Jf[i][j] = 0.0; }
    // local indices: kS = {6,7,8,9,10}: n46 (Q42 base), n49 (Q42 collector), n52 (Q43 base), n69 (Q43 collector), n65 (D52 anode)
    auto bjt = [&](int ic, int ib) {
      const double vb = y[ib], vc = y[ic]; const BjtI c = Bjt(Npn(), vb, vb - vc);
      f[ic] += c.ic; f[ib] += c.ib;
      Jf[ic][ic] += -c.icbc; Jf[ic][ib] += c.icbe + c.icbc; Jf[ib][ic] += -c.ibbc; Jf[ib][ib] += c.ibbe + c.ibbc;
    };
    bjt(1, 0); bjt(3, 2);
    double de; const double e = LimExp(y[4] / (kDiodeN * kVt), de);
    f[4] += kDiodeIs * (e - 1.0); Jf[4][4] += kDiodeIs * de / (kDiodeN * kVt);
  }

  void InitDevices()
  {
    using namespace detail;
    double y[K], Jf[K][K]; for (int i = 0; i < K; ++i) y[i] = mV[kickmodel::kS[i]];
    Devices(y, mFold, Jf);
    // constant part of the right-hand side of the active system: Ainv (F - c0)
    for (int i = 0; i < N; ++i) { double s = 0.0, s2 = 0.0; for (int j = 0; j < N; ++j) { s += mRes.Ainv[i][j] * mForce[j]; s2 += mResGap.Ainv[i][j] * mForce[j]; } mWReg[i] = s; mWGap[i] = s2; }
  }

  double Advance()
  {
    using namespace detail;
    const ResSys& rs = *mResUse; const double* w = (mResUse == &mRes) ? mWReg : (mResUse == &mResTail ? mWTail : mWGap);
    // base = Phi v + Ainv (F - c0) - AinvS fold/2   (trapezoid: v1 = base - AinvS g, g = f(y1)/2 + 0.5 f(y0) ... see the engine: the explicit part of the device currents moves to the right-hand side)
    double base[N], inj[K];
    for (int j = 0; j < K; ++j) inj[j] = -0.5 * mFold[j];
    for (int i = 0; i < N; ++i)
    {
      double s = w[i]; for (int j = 0; j < N; ++j) s += rs.Phi[i][j] * mV[j];
      for (int j = 0; j < K; ++j) s += rs.AinvS[i][j] * inj[j];
      base[i] = s;
    }
    double baseS[K]; for (int i = 0; i < K; ++i) baseS[i] = base[kickmodel::kS[i]];
    // start: the device currents of the previous step carried over, y = baseS - M g(prev)
    double y[K], g[K], f[K], Jf[K][K], r[K];
    for (int i = 0; i < K; ++i) { double s = baseS[i]; for (int j = 0; j < K; ++j) s -= rs.M[i][j] * 0.5 * mFold[j]; y[i] = s; }
    auto evaluate = [&](const double* yy, double* gg, double* rr, double* ff, double (*JF)[K]) {
      Devices(yy, ff, JF); double nrm = 0.0;
      for (int i = 0; i < K; ++i) gg[i] = 0.5 * ff[i];
      for (int i = 0; i < K; ++i) { double s = 0.0; for (int j = 0; j < K; ++j) s += rs.M[i][j] * gg[j]; rr[i] = yy[i] + s - baseS[i]; nrm = std::max(nrm, std::fabs(rr[i])); }
      return nrm;
    };
    double rn = evaluate(y, g, r, f, Jf);
    for (int it = 0; it < 60 && rn >= kNewtonTol; ++it)
    {
      double Jr[K][K], dy[K];
      for (int i = 0; i < K; ++i) for (int j = 0; j < K; ++j) { double s = 0.0; for (int l = 0; l < K; ++l) s += rs.M[i][l] * 0.5 * Jf[l][j]; Jr[i][j] = s + (i == j ? 1.0 : 0.0); }
      for (int i = 0; i < K; ++i) dy[i] = -r[i];
      if (!SolveSmall(Jr, dy)) break;
      double alpha = 1.0; for (int i = 0; i < K; ++i) if (std::fabs(dy[i]) > 0.5) alpha = std::min(alpha, 0.5 / std::fabs(dy[i]));
      double yt[K], gt[K], rt[K], ft[K], JFt[K][K], rnNew = rn;
      for (int tr = 0; tr < 14; ++tr)
      {
        for (int i = 0; i < K; ++i) yt[i] = y[i] + alpha * dy[i];
        rnNew = evaluate(yt, gt, rt, ft, JFt);
        if (rnNew < rn * (1.0 - 1e-4 * alpha) || rnNew < 1e-11) break;
        alpha *= 0.5;
      }
      for (int i = 0; i < K; ++i) { y[i] = yt[i]; g[i] = gt[i]; r[i] = rt[i]; f[i] = ft[i]; for (int j = 0; j < K; ++j) Jf[i][j] = JFt[i][j]; }
      rn = rnNew;
    }
    for (int i = 0; i < K; ++i) mFold[i] = f[i];
    for (int i = 0; i < N; ++i) { double s = base[i]; for (int j = 0; j < K; ++j) s -= rs.AinvS[i][j] * g[j]; mV[i] = s; }
    // output network: trapezoid with the n55 of this and the previous step
    const double n55 = mV[kickmodel::kOut], ub = 0.5 * ((mU1 - kickmodel::kN55Rest) + (n55 - kickmodel::kN55Rest)); mU1 = n55;
    const OutSys& os = *mOutUse; double xn[NO];
    for (int i = 0; i < NO; ++i) { double s = os.Psi[i] * ub; for (int j = 0; j < NO; ++j) s += os.Phi[i][j] * mXo[j]; xn[i] = s; }
    for (int i = 0; i < NO; ++i) mXo[i] = xn[i];
    return mXo[kickmodel::kO_Out] + kickmodel::kO_Rest[kickmodel::kO_Out];
  }

  static bool SolveSmall(double (*A)[detail::K], double* b)
  {
    constexpr int n = detail::K;
    for (int c = 0; c < n; ++c)
    {
      int p = c; for (int r = c + 1; r < n; ++r) if (std::fabs(A[r][c]) > std::fabs(A[p][c])) p = r;
      if (std::fabs(A[p][c]) < 1e-300) return false;
      if (p != c) { for (int j = 0; j < n; ++j) std::swap(A[c][j], A[p][j]); std::swap(b[c], b[p]); }
      for (int r = c + 1; r < n; ++r) { const double m = A[r][c] / A[c][c]; for (int j = c; j < n; ++j) A[r][j] -= m * A[c][j]; b[r] -= m * b[c]; }
    }
    for (int i = n - 1; i >= 0; --i) { double s = b[i]; for (int j = i + 1; j < n; ++j) s -= A[i][j] * b[j]; b[i] = s / A[i][i]; }
    return true;
  }

  double mSr = 48000.0; Pots mP; bool mValid = false, mActive = false; int mPhase = 0; long mI = 0;
  ResSys mResTail; OutSys mOutTail; double mWTail[detail::N];
  bool mTail = false; int mTailPh = 0; double mPow = 0.0, mPowPeak = 0.0, mHist[8] = {0}, mQ[4] = {0};
  ResSys mRes, mResGap; OutSys mOutHost, mOutGap, mOut1, mOut8; const ResSys* mResUse = nullptr; const OutSys* mOutUse = nullptr;
  double mAccSt[(size_t)kickmodel::kNumAnchor * detail::N]; float mAccKn[(size_t)kickmodel::kNumAnchor * kickmodel::kNumKnots];   // interpolated accent tables (StartAccent)
  double mAccent = 0.0, mV[detail::N], mForce[detail::N], mWReg[detail::N], mWGap[detail::N], mFold[detail::K], mXo[detail::NO], mU1 = 0.0;
  double mKnotT[kickmodel::kNumKnots + 1], mKnotY[kickmodel::kNumKnots + 1]; mutable int mKnotPos = 0;
};

}  // namespace kickfast
}  // namespace dgg
