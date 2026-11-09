// Fast model of a TR-909 voice over its WHOLE hit (gen_full_model.py / full_core.py): reduced continuous model with the trigger as an input (value, linear over a step, and its
// derivative for the capacitive coupling), integrated with the exponential integrator; BJT, diode (incl. op-amp clamp) and op-amp input-stage (tanh minus linear gm) currents
// interpolated linearly over the step, Newton on the port voltages. The state is carried through retriggers (the trigger is an input event, not a restart).
// Several step sizes: Prepare(h) computes the propagators of one step size (matrix exponential of the augmented system) and returns its index.
// Initial (idle) state: tables over the accent (Reset(accent)).
#pragma once
#ifndef RMF_FULLFAST_DIOWARM
#define RMF_FULLFAST_DIOWARM 0
#endif
#ifndef RMF_ASET
#define RMF_ASET 1   // RM-K08/K12: active-set Newton (ports whose column of the matrix moves by < ASTOL stay unit columns), 2.6 -> 2.1 ms per hit
#endif
#ifndef RMF_ASTOL
#define RMF_ASTOL 1e-2
#endif
#ifndef RMF_KPRED
#define RMF_KPRED 1.0
#endif
#ifndef RMF_KRES_DIV
#define RMF_KRES_DIV 3.0   // see the residual test in StepOnce
#endif
#ifndef RMF_KACCEL
#define RMF_KACCEL 0           // extend the chord updates by the geometric-series factor (see StepOnce)
#endif
#ifndef RMF_KACCEL_RMIN
#define RMF_KACCEL_RMIN 0.05
#endif
#ifndef RMF_KACCEL_RMAX
#define RMF_KACCEL_RMAX 0.5
#endif
#ifndef RMF_KACCEL_W
#define RMF_KACCEL_W 2.0
#endif
#ifndef RMF_KREUSE
#define RMF_KREUSE 1           // a rejected chord update refactors at the same point without evaluating the devices again
#endif
#ifndef RMF_KMAXIT
#define RMF_KMAXIT 50      // Newton iterations of one step before it is retried with half the step size
#endif
#ifndef RMF_KEVAL_ON
#define RMF_KEVAL_ON 1   // device currents within RMF_KEVAL_THR (V) of the last full evaluation follow from its value and slopes
#endif
#ifndef RMF_KEVAL_THR
#define RMF_KEVAL_THR 3e-4
#endif
#ifndef RMF_KCHORD_RATIO
#define RMF_KCHORD_RATIO 0.5
#endif
#ifndef RMF_KPRED_ON
#define RMF_KPRED_ON 1
#endif
#ifndef RMF_KPRED2
#define RMF_KPRED2 1           // quadratic extrapolation of the port voltages through the last three steps as Newton start
#endif
#ifndef RMF_KPRED2_HMIN
#define RMF_KPRED2_HMIN 1e-3    // the quadratic start predictor is used for steps of at least this size (s)
#endif
#ifndef RMF_KPRED_LIM_NJ
#define RMF_KPRED_LIM_NJ RMF_KPRED_LIM   // limit of the start predictor for the ports that are not junctions
#endif
#ifndef RMF_KPRED_FREE_V
#define RMF_KPRED_FREE_V 0.3       // a port that stays below this voltage (reverse biased junction, no current) is predicted without the limit
#endif
#ifndef RMF_KPRED_LINV
#define RMF_KPRED_LINV -1e30         // ports below this voltage start from the linear solution with the device currents of the previous step
#endif
#ifndef RMF_KPRED2_REVV
#define RMF_KPRED2_REVV -1e30        // ports below this voltage (reverse biased diodes) use the quadratic start predictor for every step size
#endif
#ifndef RMF_KPRED_SKIP
#define RMF_KPRED_SKIP 0       // bit mask of ports left out of the Newton start predictor
#endif
#ifndef RMF_KLIM_SKIP
#define RMF_KLIM_SKIP 0        // bit mask of ports left out of the junction voltage limiting
#endif
#ifndef RMF_KPRED_LIM
#define RMF_KPRED_LIM 0.05
#endif
#ifndef RMF_KPRED_MAXRATIO
#define RMF_KPRED_MAXRATIO 4.0
#endif
#ifndef RMF_KCHORD_FIRST
#define RMF_KCHORD_FIRST 1e-2
#endif
#ifndef RMF_FULLFAST_PREDICT
#define RMF_FULLFAST_PREDICT 0
#endif
#ifndef RMF_FULLFAST_LIMIT
#define RMF_FULLFAST_LIMIT 1
#endif
#ifndef RMF_FULLFAST_LIMV
#define RMF_FULLFAST_LIMV 0.15
#endif
#ifndef RMF_FULLFAST_TRACE
#define RMF_FULLFAST_TRACE 0
#endif
#ifndef RMF_FULLFAST_CAP
#define RMF_FULLFAST_CAP 5.0
#endif
#ifndef RMF_FULLFAST_NOCHORD
#define RMF_FULLFAST_NOCHORD 0
#endif
#define RMF_FULLFAST_NOCHORD0 0
#ifndef RMF_NEWTON_TOL_DEFAULT
#define RMF_NEWTON_TOL_DEFAULT 1e-4
#endif
#include <cmath>
#include <cstdio>
#include <vector>
#ifdef RMF_FULLFAST_PROFILE
#include <intrin.h>
#define RMF_FP_T(n) { const unsigned long long t_ = __rdtsc(); prof[n] += t_ - tlast_; tlast_ = t_; }
#else
#define RMF_FP_T(n)
#endif
#include "../LateFast.h"

namespace dgg { namespace rmfast {
using namespace ::dgg::pcmfast;

// Evaluations with the exponentials of deeply reverse-biased junctions skipped (their value is below the gmin term by many orders of magnitude)
inline void DiodeEvalG(const double* q, double v, double& i, double& g, double* vjHint = nullptr, const double* xbvPre = nullptr)
{
  const double Is = q[0], N = q[1], Bv = q[3], nvt = N * kVt;
  if (Bv > 1e20) { if (v < -40.0 * nvt) { i = -Is + kGmin * v; g = kGmin; return; } }
  else if (v < -40.0 * nvt) { const double xbv = xbvPre ? *xbvPre : Bv - nvt * std::log(std::fmax(q[4] / Is, 1.0)); if (v > -xbv) { i = -Is + kGmin * v; g = kGmin; return; } }   // exp(-40) of Is is below double resolution of Is
  DiodeEval(q, v, i, g, vjHint, xbvPre);
}
#ifndef RMF_FULLFAST_DIOTOL
#define RMF_FULLFAST_DIOTOL 1e-13
#endif
// Diode with series resistance for the RM model: same early returns as DiodeEvalG; the inner iteration ends when the junction voltage moves by less than RMF_FULLFAST_DIOTOL and
// the current at the final junction voltage is taken from the last evaluation corrected by its slope (no extra exponential); xbv = breakdown offset of the diode (evaluated once)
inline void DiodeEvalRs(const double* q, double v, double& i, double& g, double xbv)
{
  const double Is = q[0], N = q[1], Rs = q[2], Bv = q[3], nvt = N * kVt;
  if (Bv > 1e20) { if (v < -40.0 * nvt) { i = -Is + kGmin * v; g = kGmin; return; } }
  else if (v < -40.0 * nvt && v > -xbv) { i = -Is + kGmin * v; g = kGmin; return; }
  double vj = v, ii = 0.0, gg = 0.0, dv = 0.0;
  for (int k = 0; k < 80; ++k)
  {
    if (vj > -xbv) { const double e = std::exp(std::fmin(vj / nvt, 80.0)); ii = Is * (e - 1.0) + kGmin * vj; gg = Is * e / nvt + kGmin; }
    else { const double e = std::exp(std::fmin(-(xbv + vj) / nvt, 80.0)); ii = -Is * e + kGmin * vj; gg = Is * e / nvt + kGmin; }
    dv = (vj + ii * Rs - v) / (1.0 + gg * Rs); const double lim = 6.0 * nvt; dv = dv > lim ? lim : (dv < -lim ? -lim : dv); vj -= dv;
    if (std::fabs(dv) < RMF_FULLFAST_DIOTOL) break;
  }
  i = ii - gg * dv; g = gg / (1.0 + gg * Rs);
}
inline BjtOut BjtEvalG(const double* P, double vbe, double vbc)
{
  const double s = P[0], IS = P[1], ISE = P[8], invNFVt = P[9], invNEVt = P[10], invVAF = P[11], invIKF = P[12], invBF = P[13], invBR = P[14];
  const double aE = vbe * invNFVt, aC = vbc * (1.0 / kVt);
  const double eE = aE < -40.0 ? 0.0 : std::exp(std::fmin(aE, 80.0)), eC = aC < -40.0 ? 0.0 : std::exp(std::fmin(aC, 80.0));
  const double cbe = IS * (eE - 1.0) + kGmin * vbe, gbe = IS * eE * invNFVt + kGmin;
  const double cbc = IS * (eC - 1.0) + kGmin * vbc, gbc = IS * eC * (1.0 / kVt) + kGmin;
  double cben = 0.0, gben = 0.0;
  if (ISE != 0.0) { const double aL = vbe * invNEVt; const double eL = aL < -40.0 ? 0.0 : std::exp(std::fmin(aL, 80.0)); cben = ISE * (eL - 1.0); gben = ISE * eL * invNEVt; }
  const double q1 = 1.0 / (1.0 - vbc * invVAF), dq1 = q1 * q1 * invVAF;
  const double q2 = cbe * invIKF, arg = std::fmax(0.0, 1.0 + 4.0 * q2), sq = std::sqrt(arg), qb = q1 * (1.0 + sq) * 0.5;
  const double dsqbe = arg > 0.0 ? (2.0 * gbe * invIKF) / std::fmax(sq, 1e-12) : 0.0;
  const double dqbe = q1 * 0.5 * dsqbe, dqbc = dq1 * (1.0 + sq) * 0.5;
  const double iq = 1.0 / qb, x = cbe - cbc;
  const double cc = x * iq - cbc * invBR, cb = cbe * invBF + cben + cbc * invBR;
  const double dccbe = gbe * iq - x * iq * iq * dqbe, dccbc = -gbc * iq - x * iq * iq * dqbc - gbc * invBR;
  const double dcbbe = gbe * invBF + gben, dcbbc = gbc * invBR;
  BjtOut o; o.ic = s * cc; o.ib = s * cb; o.d[0][0] = s * dccbe; o.d[0][1] = s * dccbc; o.d[1][0] = s * dcbbe; o.d[1][1] = s * dcbbc;
  return o;
}

template <class M> struct FullFast
{
  static constexpr int R = M::kR, NG = M::kNg, NS = M::kNS, NF = M::kNF, NP = M::kNP, ND = M::kNumDev, NU = 3;
  struct Prop { double h = 0.0; std::vector<double> Phi, GaF, GbF, Ga, Gb, ConeC, Zp, PXz; std::vector<int> pxStart, pxCol; std::vector<double> pxVal; };   // px*: PXz as sparse rows (entries above 1e-12 of the largest)
  std::vector<Prop> props;
  double z[R], f0[NF], p[NP], bp[M::kNumBjt > 0 ? M::kNumBjt : 1][16], PD[NP * NU], PC[NP];
  double acc = M::kAcc, accLast = M::kAcc, yIdle = 0.0, newtonTol = RMF_NEWTON_TOL_DEFAULT; long steps = 0, its = 0;
  static constexpr double kSoftTol = 1e-3, kSoftY = 1e-6, kSoftZ = 1e-4, kSoftRes = 1e-4; long softAccepts = 0; double softMaxY = 0, softMaxZ = 0, softMaxRes = 0;   // after 50 iterations a last correction below this is accepted
  double kChordFirst = RMF_KCHORD_FIRST;   // largest first correction (V) with which the kept factorisation is still trusted
  double lastY = 0, lastZ = 0, lastR = 0; int dbgId = -1, traceCount = 0, lastPort = 0; double lastMx = 0, lastP = 0; long facs = 0, fails = 0, retries = 0; std::vector<int> halfIdx; double portVt[NP > 0 ? NP : 1], portVcrit[NP > 0 ? NP : 1];   // junction ports: thermal voltage and critical voltage for the limiting (0 for the other ports)
#if RMF_ASET
  double LUA[NP][NP], JIA[NP][NP], invDA[NP]; int pivA[NP], actL[NP], inL[NP], nA = NP, nI = 0; long asSumA = 0, asN = 0;   // active-set Newton: ports whose device slopes move the matrix column by < RMF_ASTOL are treated as unit columns
#endif
  double LU[NP][NP], invD[NP]; int piv[NP]; bool luOk = false; int luProp = -1;   // factorisation of the Newton matrix kept between steps (chord iteration)
  mutable double dioVj[M::kNumDio > 0 ? M::kNumDio : 1] = {0}; double dioXbv[M::kNumDio > 0 ? M::kNumDio : 1] = {0};   // series-resistance diodes: breakdown offset evaluated once, junction voltage of the previous call as start of the inner iteration
  unsigned long long prof[8] = {0}, tlast_ = 0;
#ifdef RMF_FULLFAST_PROFILE
  long nRatio = 0, nRatioF = 0; double ratioMax = 0, ratioMaxF = 0, ratioSum = 0;   // |J^-1 F|inf / |F|inf of the Newton updates
  std::vector<long> stSteps, stIts, stFacs;   // per propagator: steps, Newton iterations, factorisations
#endif
  mutable unsigned long long evalProf[3] = {0, 0, 0};
#ifdef RMF_FULLFAST_PROFILE
  ~FullFast() { if (steps > 0) { double t = 0; for (int i = 0; i < 8; ++i) t += (double)prof[i]; std::printf("  [FullFast profile, chan %d] cycles/step: state update %.0f, ports %.0f, EvalF %.0f, matrix build %.0f, LU %.0f, solve %.0f, rest %.0f (total %.0f), steps %ld\n", dbgId, prof[0] / (double)steps, prof[1] / (double)steps, prof[2] / (double)steps, prof[3] / (double)steps, prof[4] / (double)steps, prof[5] / (double)steps, (prof[6] + prof[7]) / (double)steps, t / steps, steps); std::printf("  [EvalF by type, cycles/step] BJT %.0f, diode %.0f, tanh %.0f\n", evalProf[0] / (double)steps, evalProf[1] / (double)steps, evalProf[2] / (double)steps); std::printf("  [|update|/|residual|] updates %ld, mean %.3f, max %.3f; updates below 3 tol: %ld, max %.3f\n", nRatio, ratioSum / (nRatio ? nRatio : 1), ratioMax, nRatioF, ratioMaxF); for (size_t q = 0; q < stSteps.size(); ++q) if (stSteps[q] > 0) std::printf("  [step size %8.2f us] steps %6ld, iterations/step %.2f, factorisations/step %.2f\n", props[q].h * 1e6, stSteps[q], (double)stIts[q] / stSteps[q], (double)stFacs[q] / stSteps[q]); } }
#endif
  double fmax[NF > 0 ? NF : 1] = {0}, pmax[NP > 0 ? NP : 1] = {0}, pmin[NP > 0 ? NP : 1] = {0}; bool stats = false;   // diagnosis: extremes of the device currents and port voltages

  void Init()
  {
    for (int k = 0; k < M::kNumBjt; ++k)
    {
      const double* q = &M::kBjt[9 * k]; for (int i = 0; i < 9; ++i) bp[k][i] = q[i];
      bp[k][9] = 1.0 / (q[4] * kVt); bp[k][10] = 1.0 / (q[7] * kVt); bp[k][11] = 1.0 / q[5]; bp[k][12] = 1.0 / q[6]; bp[k][13] = 1.0 / q[2]; bp[k][14] = 1.0 / q[3]; bp[k][15] = 0.0;
    }
    for (int k = 0; k < M::kNumDio; ++k) { const double* q = &M::kDio[5 * k]; const double nvt = q[1] * kVt; dioXbv[k] = q[3] > 1e20 ? 1e30 : q[3] - nvt * std::log(std::fmax(q[4] / q[0], 1.0)); dioVj[k] = 0.0; }
#if RMF_FULLFAST_DIOWARM
    g_dioWarm = true;
#endif
    for (int a = 0; a < NP; ++a) { portVt[a] = 0.0; portVcrit[a] = 0.0; }
    for (int d = 0; d < ND; ++d)
    {
      const int pp = M::kDevPort[d], idx = M::kDevIdx[d];
      if (M::kDevType[d] == 0) { const double IS = M::kBjt[9 * idx + 1], vtE = M::kBjt[9 * idx + 4] * kVt; portVt[pp] = vtE; portVcrit[pp] = vtE * std::log(vtE / (1.41421356 * IS)); portVt[pp + 1] = kVt; portVcrit[pp + 1] = kVt * std::log(kVt / (1.41421356 * IS)); }
      else if (M::kDevType[d] == 1) { const double Is = M::kDio[5 * idx], nvt = M::kDio[5 * idx + 1] * kVt; portVt[pp] = nvt; portVcrit[pp] = nvt * std::log(nvt / (1.41421356 * Is)); }
    }
    props.clear(); steps = 0; its = 0; facs = 0; fails = 0; retries = 0; Reset(acc);
  }
  // idle state of the circuit at the accent voltage (linear in the tables of the engine's DC solutions)
  void Reset(double accent)
  {
#if RMF_KPRED_ON
    predOk = false; predOk2 = false;
#endif
    acc = accent; accLast = accent; int g = 0; while (g < M::kAccGrid - 2 && accent >= M::kAccV[g + 1]) ++g;
    double t = M::kAccGrid > 1 ? (accent - M::kAccV[g]) / (M::kAccV[g + 1] - M::kAccV[g]) : 0.0; t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
    const int g1 = M::kAccGrid > 1 ? g + 1 : g;
    for (int i = 0; i < R; ++i) z[i] = (1.0 - t) * M::kZ0[g * R + i] + t * M::kZ0[g1 * R + i];
    for (int i = 0; i < NP; ++i) p[i] = (1.0 - t) * M::kP0[g * NP + i] + t * M::kP0[g1 * NP + i];
    yIdle = (1.0 - t) * M::kY0[g] + t * M::kY0[g1];
    for (int i = 0; i < NP; ++i)
    {
      for (int u = 0; u < NU; ++u) { double v = 0.0; for (int s = 0; s < NS; ++s) v += M::kPp[i * NS + s] * M::kDgS[s * NG + u]; PD[i * NU + u] = v + M::kPuP[i * NU + u]; }   // + the direct dependence of a port on a source node (e.g. the accent input of an op-amp)
      double v = M::kPc[i]; for (int s = 0; s < NS; ++s) v += M::kPp[i * NS + s] * M::kDgS[s * NG + NG - 1]; PC[i] = v;
    }
    EvalF(p, f0, nullptr); luOk = false;
  }
  // idle state tables at an accent voltage (linear interpolation)
  void IdleAt(double accent, double* zo, double* po, double& yo) const
  {
    int g = 0; while (g < M::kAccGrid - 2 && accent >= M::kAccV[g + 1]) ++g;
    double t = M::kAccGrid > 1 ? (accent - M::kAccV[g]) / (M::kAccV[g + 1] - M::kAccV[g]) : 0.0; t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
    const int g1 = M::kAccGrid > 1 ? g + 1 : g;
    for (int i = 0; i < R; ++i) zo[i] = (1.0 - t) * M::kZ0[g * R + i] + t * M::kZ0[g1 * R + i];
    for (int i = 0; i < NP; ++i) po[i] = (1.0 - t) * M::kP0[g * NP + i] + t * M::kP0[g1 * NP + i];
    yo = (1.0 - t) * M::kY0[g] + t * M::kY0[g1];
  }
  // a new accent voltage while the circuit is not at rest: the accent circuitry is taken to have settled (its idle state moves to that of the new accent), what the hit
  // left behind (the deviation from the idle state) is kept. An abrupt step of the accent input in the running model would need the accent stage to slew through the clamps.
  void ShiftAccent(double a)
  {
    if (a == acc) return;
    double zo[R], po[NP], zn[R], pn[NP], yo, yn; IdleAt(acc, zo, po, yo); IdleAt(a, zn, pn, yn);
    for (int i = 0; i < R; ++i) z[i] += zn[i] - zo[i];
    for (int i = 0; i < NP; ++i) p[i] += pn[i] - po[i];
    acc = a; accLast = a; yIdle = yn; EvalF(p, f0, nullptr); luOk = false;
  }
  int Prepare(double h)
  {
    for (size_t i = 0; i < props.size(); ++i) if (std::fabs(props[i].h - h) < 1e-15) return (int)i;
    Prop P; P.h = h;
    const int n = R + 2 * NG; std::vector<double> N(n * n, 0.0);
    for (int i = 0; i < R; ++i) { for (int j = 0; j < R; ++j) N[i * n + j] = M::kA[i * R + j] * h; for (int j = 0; j < NG; ++j) N[i * n + R + j] = M::kBg[i * NG + j] * h; }
    for (int j = 0; j < NG; ++j) N[(R + j) * n + R + NG + j] = h;
    const std::vector<double> E = Expm(N, n);
    P.Phi.assign(R * R, 0.0); P.Ga.assign(R * NU, 0.0); P.Gb.assign(R * NU, 0.0); P.ConeC.assign(R, 0.0); P.GaF.assign(R * NF, 0.0); P.GbF.assign(R * NF, 0.0); P.Zp.assign(NP * NF, 0.0); P.PXz.assign(NP * R, 0.0);
    for (int i = 0; i < R; ++i) for (int j = 0; j < R; ++j) P.Phi[i * R + j] = E[i * n + j];
    std::vector<double> GaA(R * NG), GbA(R * NG);
    for (int i = 0; i < R; ++i) for (int j = 0; j < NG; ++j) { const double e12 = E[i * n + R + j], e13 = E[i * n + R + NG + j]; GaA[i * NG + j] = e12 - e13 / h; GbA[i * NG + j] = e13 / h; }
    for (int i = 0; i < R; ++i)
    {
      for (int u = 0; u < NU; ++u) { P.Ga[i * NU + u] = GaA[i * NG + u]; P.Gb[i * NU + u] = GbA[i * NG + u]; }
      P.ConeC[i] = GaA[i * NG + NG - 1] + GbA[i * NG + NG - 1];
      for (int k = 0; k < NF; ++k)
      {
        double sa = 0.0, sb = 0.0;
        for (int s = 0; s < NS; ++s) { sa += GaA[i * NG + NU + s] * M::kTc[s * NF + k]; sb += GbA[i * NG + NU + s] * M::kTc[s * NF + k]; }
        P.GaF[i * NF + k] = sa; P.GbF[i * NF + k] = sb;
      }
    }
    std::vector<double> XG(NS * NF, 0.0);
    for (int s = 0; s < NS; ++s) for (int k = 0; k < NF; ++k)
    {
      double v = 0.0; for (int i = 0; i < R; ++i) v += M::kXzS[s * R + i] * P.GbF[i * NF + k];
      for (int q = 0; q < NS; ++q) v += M::kDgS[s * NG + NU + q] * M::kTc[q * NF + k];
      XG[s * NF + k] = v;
    }
    for (int a = 0; a < NP; ++a)
    {
      for (int k = 0; k < NF; ++k) { double v = 0.0; for (int s = 0; s < NS; ++s) v += M::kPp[a * NS + s] * XG[s * NF + k]; P.Zp[a * NF + k] = v; }
      for (int i = 0; i < R; ++i) { double v = 0.0; for (int s = 0; s < NS; ++s) v += M::kPp[a * NS + s] * M::kXzS[s * R + i]; P.PXz[a * R + i] = v; }
    }
    {   // sparse rows of PXz: the entries below 1e-12 of the largest one are rounding noise of the propagator computation
      double mx = 0.0; for (double x : P.PXz) mx = std::fmax(mx, std::fabs(x));
      P.pxStart.assign(NP + 1, 0); P.pxCol.clear(); P.pxVal.clear();
      for (int a = 0; a < NP; ++a) { for (int i = 0; i < R; ++i) { const double x = P.PXz[a * R + i]; if (std::fabs(x) > 1e-12 * mx) { P.pxCol.push_back(i); P.pxVal.push_back(x); } } P.pxStart[a + 1] = (int)P.pxCol.size(); }
    }
    props.push_back(std::move(P)); return (int)props.size() - 1;
  }
  // device currents f (and, with jd, the entries of d f / d p per device: BJT 2x2 block (d[0][0], d[0][1], d[1][0], d[1][1]), diode / tanh the slope in d[0][0])
  struct EvalRef { double q[2] = {0, 0}, f[2] = {0, 0}, j[4] = {0, 0, 0, 0}; bool valid = false; };
  mutable EvalRef eR[M::kNumDev > 0 ? M::kNumDev : 1];   // last full evaluation of every device (RMF_KEVAL_THR)
  void EvalF(const double* q, double* f, double (*jd)[4]) const
  {
    for (int d = 0; d < ND; ++d)
    {
#ifdef RMF_FULLFAST_PROFILE
      const unsigned long long te_ = __rdtsc();
#endif
      const int pp = M::kDevPort[d], cc = M::kDevCur[d], idx = M::kDevIdx[d], ty = M::kDevType[d];
      if (ty == 0)
      {
#if RMF_KEVAL_ON
        {   // within RMF_KEVAL_THR of the last full evaluation the currents follow from its value and slopes (the error is of second order in the distance)
          const double d0 = q[pp] - eR[d].q[0], d1 = q[pp + 1] - eR[d].q[1];
          if (eR[d].valid && std::fabs(d0) <= RMF_KEVAL_THR && std::fabs(d1) <= RMF_KEVAL_THR)
          {
            f[cc] = eR[d].f[0] + eR[d].j[0] * d0 + eR[d].j[1] * d1; f[cc + 1] = eR[d].f[1] + eR[d].j[2] * d0 + eR[d].j[3] * d1;
            if (jd) { jd[d][0] = eR[d].j[0]; jd[d][1] = eR[d].j[1]; jd[d][2] = eR[d].j[2]; jd[d][3] = eR[d].j[3]; }
                      }
          else
          {
            const BjtOut o = BjtEvalG(bp[idx], q[pp], q[pp + 1]); f[cc] = o.ic; f[cc + 1] = o.ib;
            eR[d].q[0] = q[pp]; eR[d].q[1] = q[pp + 1]; eR[d].f[0] = o.ic; eR[d].f[1] = o.ib; eR[d].j[0] = o.d[0][0]; eR[d].j[1] = o.d[0][1]; eR[d].j[2] = o.d[1][0]; eR[d].j[3] = o.d[1][1]; eR[d].valid = true;
            if (jd) { jd[d][0] = o.d[0][0]; jd[d][1] = o.d[0][1]; jd[d][2] = o.d[1][0]; jd[d][3] = o.d[1][1]; }
          }
        }
#else
        const BjtOut o = BjtEvalG(bp[idx], q[pp], q[pp + 1]); f[cc] = o.ic; f[cc + 1] = o.ib;
        if (jd) { jd[d][0] = o.d[0][0]; jd[d][1] = o.d[0][1]; jd[d][2] = o.d[1][0]; jd[d][3] = o.d[1][1]; }
#endif
      }
      else if (ty == 1)
      {
        double i, g;
#if RMF_KEVAL_ON
        const double d0 = q[pp] - eR[d].q[0];
        if (eR[d].valid && std::fabs(d0) <= RMF_KEVAL_THR) { i = eR[d].f[0] + eR[d].j[0] * d0; g = eR[d].j[0]; }
        else
        {
          if (M::kDio[5 * idx + 2] > 0.0) DiodeEvalRs(&M::kDio[5 * idx], q[pp], i, g, dioXbv[idx]); else DiodeEvalG(&M::kDio[5 * idx], q[pp], i, g, &dioVj[idx], &dioXbv[idx]);
          eR[d].q[0] = q[pp]; eR[d].f[0] = i; eR[d].j[0] = g; eR[d].valid = true;
        }
#else
        if (M::kDio[5 * idx + 2] > 0.0) DiodeEvalRs(&M::kDio[5 * idx], q[pp], i, g, dioXbv[idx]); else DiodeEvalG(&M::kDio[5 * idx], q[pp], i, g, &dioVj[idx], &dioXbv[idx]);
#endif
        f[cc] = i; if (jd) jd[d][0] = g;
      }
      else
      {
        const double gm = M::kTanh[2 * idx], im = M::kTanh[2 * idx + 1], x = gm * q[pp] / im;
        if (std::fabs(x) < 0.05) { const double x2 = x * x; f[cc] = im * x * x2 * (-1.0 / 3.0 + x2 * (2.0 / 15.0)); if (jd) jd[d][0] = -gm * x2 * (1.0 - x2 * (2.0 / 3.0)); }   // tanh(x) - x and 1 - tanh^2 - 1 by series
        else { const double th = std::tanh(x); f[cc] = im * th - gm * q[pp]; if (jd) jd[d][0] = gm * (1.0 - th * th) - gm; }
      }
#ifdef RMF_FULLFAST_PROFILE
      evalProf[ty] += __rdtsc() - te_;
#endif
    }
  }
  bool noChord = false;   // refactor at the first iteration of every step (the trigger phase: the Jacobian changes from step to step)
  double lastTr1 = 0.0, lastDtr = 0.0;
#if RMF_KPRED_ON
  double pPrev2[NP] = {0}, hPrev2 = 0.0; bool predOk2 = false;   // the step before the previous one (quadratic start predictor)
  double pPrev[NP] = {0}, hPrev = 0.0; bool predOk = false, predEnable = true;   // ports at the start of the previous step and its size (start predictor of the Newton iteration)
#endif
  double OutputNow() const { return Output(f0, lastTr1, lastDtr); }   // output of the present state (after a change of the state between two steps)
  double Output(const double* f, double tr1, double dtr) const
  {
    double y = M::kDo1 + M::kDou[0] * acc + M::kDou[1] * tr1 + M::kDou[2] * dtr;
    for (int i = 0; i < R; ++i) y += M::kXo[i] * z[i];
    for (int k = 0; k < NF; ++k) y += M::kDof[k] * f[k];
    return y;
  }
  // one step with propagator index pi: trigger value tr0 -> tr1 over the step, derivative dtr (constant over the step)
  // the step is retried as two half steps (smaller propagator, registered with SetHalf) when the Newton iteration does not converge; the state is restored first
  void SetHalf(int pi, int ph) { if ((int)halfIdx.size() <= pi) halfIdx.resize(pi + 1, -1); halfIdx[pi] = ph; }
  // state before the last step of the caller (top level), for RedoLast
  struct Snap { double z[R], f0[NF], p[NP], acc, accLast; int pi; double tr0, tr1, dtr; bool valid = false; } snap;
  // The last step is computed again from the state before it with a tight Newton tolerance and a tight chord criterion: the currents at its end carry the error of the loose tolerance, which a following step of
  // much smaller size would turn into a transient of the output (0.5 mV after a step from 167 us to 2.6 us)
  void RedoLast(double tol, double chordFirst)
  {
    if (!snap.valid) return;
    for (int i = 0; i < R; ++i) z[i] = snap.z[i]; for (int i = 0; i < NF; ++i) f0[i] = snap.f0[i]; for (int i = 0; i < NP; ++i) p[i] = snap.p[i]; acc = snap.acc; accLast = snap.accLast;
    const double t0 = newtonTol, c0 = kChordFirst; newtonTol = tol; kChordFirst = chordFirst; luOk = false; --steps;
    const Snap s = snap; Step(s.pi, s.tr0, s.tr1, s.dtr); newtonTol = t0; kChordFirst = c0;
  }
  double Step(int pi, double tr0, double tr1, double dtr, int depth = 0)
  {
    if (depth == 0) { for (int i = 0; i < R; ++i) snap.z[i] = z[i]; for (int i = 0; i < NF; ++i) snap.f0[i] = f0[i]; for (int i = 0; i < NP; ++i) snap.p[i] = p[i]; snap.acc = acc; snap.accLast = accLast; snap.pi = pi; snap.tr0 = tr0; snap.tr1 = tr1; snap.dtr = dtr; snap.valid = true; }
    const double aL = accLast, aN = acc; double zs[R], fs[NF], ps[NP]; for (int i = 0; i < R; ++i) zs[i] = z[i]; for (int i = 0; i < NF; ++i) fs[i] = f0[i]; for (int i = 0; i < NP; ++i) ps[i] = p[i];
    bool ok = true; const double y = StepOnce(pi, tr0, tr1, dtr, ok);
    if (ok) return y;
    const int ph = pi < (int)halfIdx.size() ? halfIdx[pi] : -1;
    if (depth >= 3 || ph < 0)
    {
      ++fails;
#ifdef FULLFAST_FAILLOG2
      { double zm = 0, pm = 0; for (int i = 0; i < R; ++i) zm = std::fmax(zm, std::fabs(zs[i])); for (int i = 0; i < NP; ++i) pm = std::fmax(pm, std::fabs(ps[i])); std::printf("  state before the step: max|z| %.3e max|p| %.3e\n", zm, pm); }
      std::printf("FINAL failure: chan %d acc %.3f depth %d prop h %.3e tr %.3f->%.3f, last correction %.3e at port %d (p %.5f), y %.4g, step %ld, check: output effect %.2e, state effect %.2e, residual %.2e\n", dbgId, acc, depth, props[pi].h, tr0, tr1, lastMx, lastPort, lastP, y, steps, lastY, lastZ, lastR);
#endif
      return y;
    }
    for (int i = 0; i < R; ++i) z[i] = zs[i]; for (int i = 0; i < NF; ++i) f0[i] = fs[i]; for (int i = 0; i < NP; ++i) p[i] = ps[i]; luOk = false; --steps; ++retries; accLast = aL;
    const double trm = 0.5 * (tr0 + tr1); acc = 0.5 * (aL + aN); Step(ph, tr0, trm, dtr, depth + 1);
    acc = aN; return Step(ph, trm, tr1, dtr, depth + 1);
  }
  double StepOnce(int pi, double tr0, double tr1, double dtr, bool& ok)
  {
#ifdef RMF_FULLFAST_PROFILE
    tlast_ = __rdtsc();
#endif
#ifdef RMF_FULLFAST_PROFILE
    const long its0 = its, facs0 = facs;
#endif
#ifdef FULLFAST_STEPLOG
    const long itsLog0 = its, facsLog0 = facs;
#endif
    const Prop& P = props[pi]; double zk[R], pk[NP];
    const double cu = (P.Ga[0] + P.Gb[0]);
    for (int i = 0; i < R; ++i)
    {
      double a = P.ConeC[i] + P.Ga[i * NU] * accLast + P.Gb[i * NU] * acc + P.Ga[i * NU + 1] * tr0 + P.Gb[i * NU + 1] * tr1 + (P.Ga[i * NU + 2] + P.Gb[i * NU + 2]) * dtr;
      const double* ph = &P.Phi[i * R]; for (int k = 0; k < R; ++k) a += ph[k] * z[k];
      const double* gf = &P.GaF[i * NF]; for (int k = 0; k < NF; ++k) a += gf[k] * f0[k];
      zk[i] = a;
    }
    (void)cu; RMF_FP_T(0)
    for (int a = 0; a < NP; ++a)
    {
      double v = PC[a] + PD[a * NU] * acc + PD[a * NU + 1] * tr1 + PD[a * NU + 2] * dtr;
      for (int q2 = P.pxStart[a]; q2 < P.pxStart[a + 1]; ++q2) v += P.pxVal[q2] * zk[P.pxCol[q2]];
      pk[a] = v;
    }
    RMF_FP_T(1)
    double f1[NF], jd[ND][4], F[NP]; bool fresh = false;
#ifdef FULLFAST_DEBUG
    double Jc[NP][NP], Fc[NP];
#endif
#if RMF_FULLFAST_PREDICT
    for (int a2 = 0; a2 < NP; ++a2) { const double* zp = &P.Zp[a2 * NF]; double zf = 0.0; for (int k = 0; k < NF; ++k) zf += zp[k] * f0[k]; p[a2] = pk[a2] + zf; }   // start from the linear prediction with the currents of the previous step
#endif
#if RMF_KPRED_ON
    double pStart[NP]; for (int a2 = 0; a2 < NP; ++a2) pStart[a2] = p[a2];
    if (predOk && predEnable && P.h > 0.0 && hPrev > 0.0 && P.h <= RMF_KPRED_MAXRATIO * hPrev && P.h >= hPrev / RMF_KPRED_MAXRATIO)
    {   // Newton start: the change of the port voltages of the previous step, scaled to this step, limited so that a junction never leaves its region by more than a few thermal voltages
      const double g = RMF_KPRED * P.h / hPrev;
#if RMF_KPRED2
      const bool quad = predOk2 && hPrev2 > 0.0 && hPrev2 <= RMF_KPRED_MAXRATIO * hPrev && hPrev2 >= hPrev / RMF_KPRED_MAXRATIO;
      const double qa = hPrev, qb = hPrev2, qh = P.h;
      const double l0 = (qh + qa) * (qh + qa + qb) / (qa * (qa + qb)), l1 = -qh * (qh + qa + qb) / (qa * qb), l2 = qh * (qh + qa) / (qb * (qa + qb));
#endif
      for (int a2 = 0; a2 < NP; ++a2) { if ((RMF_KPRED_SKIP >> a2) & 1) continue; double d2 = g * (p[a2] - pPrev[a2]);
#if RMF_KPRED2
        if (quad && (P.h >= RMF_KPRED2_HMIN || p[a2] < RMF_KPRED2_REVV)) d2 = (l0 - 1.0) * p[a2] + l1 * pPrev[a2] + l2 * pPrev2[a2];
#endif
 const double lim = portVt[a2] > 0.0 ? RMF_KPRED_LIM : RMF_KPRED_LIM_NJ; if (!(p[a2] + d2 < RMF_KPRED_FREE_V)) d2 = d2 > lim ? lim : (d2 < -lim ? -lim : d2); p[a2] += d2; }
      for (int a2 = 0; a2 < NP; ++a2)
        if (p[a2] < RMF_KPRED_LINV) { const double* zp = &P.Zp[a2 * NF]; double zf = 0.0; for (int k = 0; k < NF; ++k) zf += zp[k] * f0[k]; p[a2] = pk[a2] + zf; }   // a port below this voltage carries no junction current: the linear network with the currents of the previous step gives its voltage
    }
#endif
    bool refac = !luOk || luProp != pi || RMF_FULLFAST_NOCHORD || noChord; const bool fromPrev = !refac; double prevMx = 1e30; bool firstChord = true;
    int itDone = 0;
#if RMF_KREUSE
    bool reuse = false; double Fres[NP];
#endif
    for (int it = 0; it < RMF_KMAXIT; ++it)
    {
      itDone = it + 1;
#if RMF_KREUSE
      if (reuse) { reuse = false; for (int a = 0; a < NP; ++a) F[a] = Fres[a]; }   // the kept factorisation was rejected at this p: device values, slopes and residual are those of the rejected iteration
      else
#endif
      {
      EvalF(p, f1, RMF_KREUSE ? jd : (refac ? jd : nullptr)); ++its; RMF_FP_T(2)
      {   // residual of the port equations at p; if it is small enough that the update would be below the tolerance (|update| <= 1.6 |residual| measured), p and f1 are accepted without the solve
        double fm = 0.0;
        for (int a = 0; a < NP; ++a) { const double* zp = &P.Zp[a * NF]; double zf = 0.0; for (int k = 0; k < NF; ++k) zf += zp[k] * f1[k]; F[a] = pk[a] + zf - p[a]; fm = std::fmax(fm, std::fabs(F[a])); }
        if (fm * RMF_KRES_DIV < newtonTol) { fresh = false; break; }
      }
      }
#if RMF_KREUSE
      if (!refac) for (int a = 0; a < NP; ++a) Fres[a] = F[a];
#endif
      if (refac)
      {
        for (int a = 0; a < NP; ++a)
        {
          const double* zp = &P.Zp[a * NF];
          for (int b = 0; b < NP; ++b) LU[a][b] = (a == b) ? 1.0 : 0.0;
          for (int d = 0; d < ND; ++d)
          {
            const int pp = M::kDevPort[d], cc = M::kDevCur[d];
            if (M::kDevType[d] == 0) { LU[a][pp] -= zp[cc] * jd[d][0] + zp[cc + 1] * jd[d][2]; LU[a][pp + 1] -= zp[cc] * jd[d][1] + zp[cc + 1] * jd[d][3]; }
            else LU[a][pp] -= zp[cc] * jd[d][0];
          }
        }
#ifdef FULLFAST_DEBUG
        for (int a2 = 0; a2 < NP; ++a2) { Fc[a2] = F[a2]; for (int b2 = 0; b2 < NP; ++b2) Jc[a2][b2] = LU[a2][b2]; }
#endif
        RMF_FP_T(3)
#ifdef RMF_LUSTAT   // test harness only: occupancy of the Newton matrix before the LU, and the columns whose deviation from the unit matrix is small (quasi linear ports)
        { static long nf = 0, nz1 = 0, nz9 = 0, ql2 = 0, ql3 = 0, ql4 = 0; ++nf; for (int i = 0; i < NP; ++i) { double mx = 0; for (int j = 0; j < NP; ++j) mx = std::fmax(mx, std::fabs(LU[i][j])); for (int j = 0; j < NP; ++j) { if (LU[i][j] != 0.0) ++nz1; if (std::fabs(LU[i][j]) > 1e-9 * mx) ++nz9; } }
          for (int b = 0; b < NP; ++b) { double dv = 0; for (int a = 0; a < NP; ++a) dv = std::fmax(dv, std::fabs(LU[a][b] - (a == b ? 1.0 : 0.0))); if (dv < 1e-2) ++ql2; if (dv < 1e-3) ++ql3; if (dv < 1e-4) ++ql4; }
          if ((nf & 1023) == 0) std::printf("LUSTAT factorisations %ld: nonzero %.1f %%, above 1e-9 %.1f %%; ports of %d with column deviation < 1e-2: %.1f, < 1e-3: %.1f, < 1e-4: %.1f\n", nf, 100.0 * nz1 / (double(nf) * NP * NP), 100.0 * nz9 / (double(nf) * NP * NP), NP, double(ql2) / nf, double(ql3) / nf, double(ql4) / nf); }
#endif
#if RMF_ASET
        {   // active set: the columns of the ports whose devices barely move the matrix (|J - 1| < RMF_ASTOL in every row) are unit columns; only the active block is factorised
          nA = 0; nI = 0;
          { double dv[NP]; for (int b = 0; b < NP; ++b) dv[b] = 0.0; for (int a = 0; a < NP; ++a) { LU[a][a] -= 1.0; for (int b = 0; b < NP; ++b) { const double v = LU[a][b] < 0.0 ? -LU[a][b] : LU[a][b]; dv[b] = dv[b] > v ? dv[b] : v; } LU[a][a] += 1.0; } for (int b = 0; b < NP; ++b) { if (dv[b] >= RMF_ASTOL) actL[nA++] = b; else inL[nI++] = b; } }
          asSumA += nA; ++asN;
          for (int ia = 0; ia < nA; ++ia) for (int ja = 0; ja < nA; ++ja) LUA[ia][ja] = LU[actL[ia]][actL[ja]];
          for (int ii = 0; ii < nI; ++ii) for (int ja = 0; ja < nA; ++ja) JIA[ii][ja] = LU[inL[ii]][actL[ja]];
          for (int c = 0; c < nA; ++c)
          {
            int pr = c; double mx = std::fabs(LUA[c][c]); for (int i2 = c + 1; i2 < nA; ++i2) if (std::fabs(LUA[i2][c]) > mx) { mx = std::fabs(LUA[i2][c]); pr = i2; }
            pivA[c] = pr;
            if (pr != c) for (int j2 = 0; j2 < nA; ++j2) std::swap(LUA[c][j2], LUA[pr][j2]);
            const double inv = 1.0 / LUA[c][c]; invDA[c] = inv;
            for (int i2 = c + 1; i2 < nA; ++i2) { const double fct = LUA[i2][c] * inv; LUA[i2][c] = fct; if (fct == 0.0) continue; for (int j2 = c + 1; j2 < nA; ++j2) LUA[i2][j2] -= fct * LUA[c][j2]; }
          }
        }
#else
        // LU with partial pivoting, in place (multipliers kept below the diagonal)
        for (int c = 0; c < NP; ++c)
        {
          int pr = c; double mx = std::fabs(LU[c][c]); for (int i = c + 1; i < NP; ++i) if (std::fabs(LU[i][c]) > mx) { mx = std::fabs(LU[i][c]); pr = i; }
          piv[c] = pr;
          if (pr != c) for (int j = 0; j < NP; ++j) std::swap(LU[c][j], LU[pr][j]);
          const double inv = 1.0 / LU[c][c]; invD[c] = inv;
          for (int i = c + 1; i < NP; ++i) { const double fct = LU[i][c] * inv; LU[i][c] = fct; if (fct == 0.0) continue; for (int j = c + 1; j < NP; ++j) LU[i][j] -= fct * LU[c][j]; }
        }
#endif
        luOk = true; luProp = pi; refac = false; fresh = true; firstChord = true; ++facs; RMF_FP_T(4)
      }
      else
      {
        fresh = false; RMF_FP_T(3)
      }
#ifdef RMF_FULLFAST_PROFILE
      double fmax0 = 0.0; for (int a = 0; a < NP; ++a) fmax0 = std::fmax(fmax0, std::fabs(F[a]));
#endif
#if RMF_ASET
      {
        double Fa[NP]; for (int ia = 0; ia < nA; ++ia) Fa[ia] = F[actL[ia]];
        for (int c = 0; c < nA; ++c) if (pivA[c] != c) std::swap(Fa[c], Fa[pivA[c]]);
        for (int c = 0; c < nA; ++c) for (int i2 = c + 1; i2 < nA; ++i2) Fa[i2] -= LUA[i2][c] * Fa[c];
        for (int c = nA - 1; c >= 0; --c) { const double xc = Fa[c] * invDA[c]; Fa[c] = xc; for (int i2 = 0; i2 < c; ++i2) Fa[i2] -= LUA[i2][c] * xc; }
        for (int ii = 0; ii < nI; ++ii) { double v = F[inL[ii]]; for (int ja = 0; ja < nA; ++ja) v -= JIA[ii][ja] * Fa[ja]; F[inL[ii]] = v; }
        for (int ia = 0; ia < nA; ++ia) F[actL[ia]] = Fa[ia];
      }
#else
      for (int c = 0; c < NP; ++c) if (piv[c] != c) std::swap(F[c], F[piv[c]]);   // all interchanges first (the multipliers are stored with their rows permuted), then L, then U
      for (int c = 0; c < NP; ++c) for (int i = c + 1; i < NP; ++i) F[i] -= LU[i][c] * F[c];
      for (int c = NP - 1; c >= 0; --c) { const double xc = F[c] * invD[c]; F[c] = xc; for (int i = 0; i < c; ++i) F[i] -= LU[i][c] * xc; }   // column-oriented back substitution: the updates of one column are independent of each other
#endif
#ifdef FULLFAST_DEBUG
      if (refac == false && fresh && steps < 2) { double r = 0; for (int a2 = 0; a2 < NP; ++a2) { double v = -Fc[a2]; for (int b2 = 0; b2 < NP; ++b2) v += Jc[a2][b2] * F[b2]; r = std::fmax(r, std::fabs(v)); } std::printf("  linear solve residual %.3e (max |F| %.3e)\n", r,[&]{ double m2 = 0; for (int a2 = 0; a2 < NP; ++a2) m2 = std::fmax(m2, std::fabs(Fc[a2])); return m2; }()); }
#endif
      RMF_FP_T(5)
      double mx = 0.0; for (int a = 0; a < NP; ++a) mx = std::fmax(mx, std::fabs(F[a]));
#ifdef FULLFAST_ITLOG
      if (steps >= FULLFAST_ITLOG && steps < FULLFAST_ITLOG + 12) { int am = 0; for (int a = 1; a < NP; ++a) if (std::fabs(F[a]) > std::fabs(F[am])) am = a; std::fprintf(stderr, "IT %ld %d mx %.3e port %d p %.4f fresh %d\n", steps, it, mx, am, p[am], (int)fresh); }
#endif
#ifdef RMF_FULLFAST_PROFILE
      if (fmax0 > 0.0) { const double r = mx / fmax0; ++nRatio; if (r > ratioMax) ratioMax = r; ratioSum += r; if (mx < 3.0 * newtonTol) { ++nRatioF; if (r > ratioMaxF) ratioMaxF = r; } }
#endif
#ifdef FULLFAST_DEBUG
      if (steps < 3 || (RMF_FULLFAST_TRACE && tr0 > 1.87 && tr0 < 1.88 && P.h < 1.3e-6 && traceCount < 60 && ++traceCount)) { double sp = 0; for (int a3 = 0; a3 < NP; ++a3) sp += std::fabs(p[a3]); int am = 0; for (int a3 = 1; a3 < NP; ++a3) if (std::fabs(F[a3]) > std::fabs(F[am])) am = a3; std::printf("step %ld it %d mx %.3e fresh %d sum|p| %.6f argmax port %d (p %.5f dp %.5f)\n", steps, it, mx, (int)fresh, sp, am, p[am], F[am]); }
#endif
      // a factorisation kept from an earlier step is trusted for the first correction only when that is small; a factorisation made in this step is reused while it converges
      if (!fresh && mx >= newtonTol && ((fromPrev && it == 0 && mx > kChordFirst) || (it >= 1 && mx > RMF_KCHORD_RATIO * prevMx))) { refac = true;
#ifdef FULLFAST_RFLOG
        if (steps < FULLFAST_RFLOG) std::fprintf(stderr, "RF %ld %.2e it %d %s mx %.2e prev %.2e\n", steps, P.h, it, (it == 0 ? "first" : "ratio"), mx, prevMx);
#endif
#if RMF_KREUSE
        reuse = true;
#endif
        continue; }   // the kept factorisation does not serve here: refactor at the same p, this update is dropped
      if (!fresh) firstChord = false;
      if (mx < newtonTol) break;   // p and f1 are a consistent pair; the remaining update is below the tolerance
#if RMF_KACCEL
      if (it >= 1 && !fresh && prevMx < 1e29)
      {   // chord iterations contract geometrically with the ratio r = mx / prevMx; the update is extended by 1 / (1 - r), the sum of the remaining geometric series
        const double r = mx / prevMx;
        if (r > RMF_KACCEL_RMIN && r < RMF_KACCEL_RMAX) { double w = 1.0 / (1.0 - r); if (w > RMF_KACCEL_W) w = RMF_KACCEL_W; for (int a = 0; a < NP; ++a) F[a] *= w; }
      }
#endif
#if RMF_FULLFAST_LIMIT > 0
      {   // junction voltage limiting as SPICE pnjlim: a junction port (BJT vbe / vbc, diode) never jumps far up the exponential in one iteration
        const double sc = mx > RMF_FULLFAST_CAP ? RMF_FULLFAST_CAP / mx : 1.0;
        for (int a = 0; a < NP; ++a)
        {
          double vnew = p[a] + sc * F[a];
          if (portVt[a] > 0.0 && !((RMF_KLIM_SKIP >> a) & 1) && vnew > portVcrit[a] && std::fabs(vnew - p[a]) > 2.0 * portVt[a])
          {
            if (p[a] > 0.0) { const double arg = 1.0 + (vnew - p[a]) / portVt[a]; vnew = arg > 0.0 ? p[a] + portVt[a] * std::log(arg) : portVcrit[a]; }
            else vnew = portVt[a] * std::log(vnew / portVt[a]);
          }
          p[a] = vnew;
        }
      }
#else
      const double sc = mx > 0.5 ? 0.5 / mx : 1.0; for (int a = 0; a < NP; ++a) p[a] += sc * F[a];
#endif
      prevMx = mx;
    }
    if (itDone >= RMF_KMAXIT)
    {
      ok = false; { double mm = 0; for (int a3 = 0; a3 < NP; ++a3) if (std::fabs(F[a3]) > mm) { mm = std::fabs(F[a3]); lastPort = a3; lastP = p[a3]; } lastMx = mm; if (mm < kSoftTol)
      {   // verified acceptance: apply the last correction and check what it still changes: the device currents, hence the state update and the output, and the residual of the port equations at the new point
        double p2[NP], f2[NF], dz[R]; for (int a4 = 0; a4 < NP; ++a4) p2[a4] = p[a4] + F[a4]; EvalF(p2, f2, nullptr);
        double df[NF]; for (int k = 0; k < NF; ++k) df[k] = f2[k] - f1[k];
        double zerr = 0.0, yerr = 0.0; for (int i = 0; i < R; ++i) { double v = 0.0; for (int k = 0; k < NF; ++k) v += P.GbF[i * NF + k] * df[k]; dz[i] = v; zerr = std::fmax(zerr, std::fabs(v)); yerr += M::kXo[i] * v; }
        for (int k = 0; k < NF; ++k) yerr += M::kDof[k] * df[k];
        double rres = 0.0; for (int a4 = 0; a4 < NP; ++a4) { const double* zp = &P.Zp[a4 * NF]; double zf = 0.0; for (int k = 0; k < NF; ++k) zf += zp[k] * f2[k]; rres = std::fmax(rres, std::fabs(pk[a4] + zf - p2[a4])); }
        lastY = yerr; lastZ = zerr; lastR = rres;
        if (std::fabs(yerr) < kSoftY && zerr < kSoftZ && rres < kSoftRes)
        {
          for (int a4 = 0; a4 < NP; ++a4) p[a4] = p2[a4]; for (int k = 0; k < NF; ++k) f1[k] = f2[k];
          ok = true; ++softAccepts; softMaxY = std::fmax(softMaxY, std::fabs(yerr)); softMaxZ = std::fmax(softMaxZ, zerr); softMaxRes = std::fmax(softMaxRes, rres);
        }
      }
    }   // a correction that stays below kSoftTol (V) is as good as converged: the noise floor of the residual, not a divergence
#ifdef FULLFAST_FAILLOG
      double mm = 0; int am = 0; for (int a3 = 0; a3 < NP; ++a3) if (std::fabs(F[a3]) > mm) { mm = std::fabs(F[a3]); am = a3; }
      std::printf("Newton failure: prop h %.3e, tr %.3f->%.3f, last correction %.3e at port %d (p %.5f), its so far %ld\n", P.h, tr0, tr1, mm, am, p[am], its);
#endif
    }
    RMF_FP_T(5)
    for (int i = 0; i < R; ++i) { double a = zk[i]; const double* gb = &P.GbF[i * NF]; for (int k = 0; k < NF; ++k) a += gb[k] * f1[k]; z[i] = a; }
    for (int k = 0; k < NF; ++k) f0[k] = f1[k];
    RMF_FP_T(6)
    if (stats)
    {
      for (int k = 0; k < NF; ++k) if (std::fabs(f1[k]) > fmax[k]) fmax[k] = std::fabs(f1[k]);
      for (int a = 0; a < NP; ++a) { if (p[a] > pmax[a]) pmax[a] = p[a]; if (p[a] < pmin[a]) pmin[a] = p[a]; }
    }
#if RMF_KPRED_ON
#if RMF_KPRED2
    for (int a2 = 0; a2 < NP; ++a2) pPrev2[a2] = pPrev[a2]; hPrev2 = hPrev; predOk2 = predOk;
#endif
    for (int a2 = 0; a2 < NP; ++a2) pPrev[a2] = pStart[a2]; hPrev = P.h; predOk = true;
#endif
#ifdef RMF_FULLFAST_PROFILE
    if ((int)stSteps.size() <= pi) { stSteps.resize(pi + 1, 0); stIts.resize(pi + 1, 0); stFacs.resize(pi + 1, 0); }
    ++stSteps[pi]; stIts[pi] += its - its0; stFacs[pi] += facs - facs0;
#endif
#ifdef FULLFAST_STEPLOG
    if (steps < FULLFAST_STEPLOG) std::fprintf(stderr, "SL %ld %.3e %ld %ld\n", steps, P.h, its - itsLog0, facs - facsLog0);
#endif
    accLast = acc; ++steps; lastTr1 = tr1; lastDtr = dtr; const double yo = Output(f1, tr1, dtr); if (!(std::fabs(yo) < 1e6)) ok = false; return yo;
  }
};

}}  // namespace dgg::rmfast
