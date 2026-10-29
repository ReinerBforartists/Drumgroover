// Fast model of a TR-909 voice over its WHOLE hit (gen_full_model.py / full_core.py): reduced continuous model with the trigger as an input (value, linear over a step, and its
// derivative for the capacitive coupling), integrated with the exponential integrator; BJT, diode (incl. op-amp clamp) and op-amp input-stage (tanh minus linear gm) currents
// interpolated linearly over the step, Newton on the port voltages. The state is carried through retriggers (the trigger is an input event, not a restart).
// Several step sizes: Prepare(h) computes the propagators of one step size (matrix exponential of the augmented system) and returns its index.
// Initial (idle) state: tables over the accent (Reset(accent)).
#pragma once
#ifndef FULLFAST_PREDICT
#define FULLFAST_PREDICT 0
#endif
#ifndef FULLFAST_LIMIT
#define FULLFAST_LIMIT 1
#endif
#ifndef FULLFAST_LIMV
#define FULLFAST_LIMV 0.15
#endif
#ifndef FULLFAST_TRACE
#define FULLFAST_TRACE 0
#endif
#ifndef FULLFAST_CAP
#define FULLFAST_CAP 5.0
#endif
#ifndef FULLFAST_NOCHORD
#define FULLFAST_NOCHORD 0
#endif
#define FULLFAST_NOCHORD0 0
#ifndef NEWTON_TOL_DEFAULT
#define NEWTON_TOL_DEFAULT 3e-5
#endif
#include <cmath>
#include <cstdio>
#include <vector>
#ifdef FULLFAST_PROFILE
#include <intrin.h>
#define FP_T(n) { const unsigned long long t_ = __rdtsc(); prof[n] += t_ - tlast_; tlast_ = t_; }
#else
#define FP_T(n)
#endif
#include "LateFast.h"

namespace dgg { namespace pcmfast {

// Evaluations with the exponentials of deeply reverse-biased junctions skipped (their value is below the gmin term by many orders of magnitude)
inline void DiodeEvalG(const double* q, double v, double& i, double& g, double* vjHint = nullptr, const double* xbvPre = nullptr)
{
  const double Is = q[0], N = q[1], Bv = q[3], nvt = N * kVt;
  if (Bv > 1e20) { if (v < -40.0 * nvt) { i = -Is + kGmin * v; g = kGmin; return; } }
  else if (v < -40.0 * nvt) { const double xbv = xbvPre ? *xbvPre : Bv - nvt * std::log(std::fmax(q[4] / Is, 1.0)); if (v > -xbv) { i = -Is + kGmin * v; g = kGmin; return; } }   // exp(-40) of Is is below double resolution of Is
  DiodeEval(q, v, i, g, vjHint, xbvPre);
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
  struct Prop { double h = 0.0; std::vector<double> Phi, GaF, GbF, Ga, Gb, ConeC, Zp, PXz; };
  std::vector<Prop> props;
  double z[R], f0[NF], p[NP], bp[M::kNumBjt > 0 ? M::kNumBjt : 1][16], PD[NP * NU], PC[NP];
  double acc = M::kAcc, accLast = M::kAcc, yIdle = 0.0, newtonTol = NEWTON_TOL_DEFAULT; long steps = 0, its = 0;
  static constexpr double kSoftTol = 1e-3, kSoftY = 1e-6, kSoftZ = 1e-4, kSoftRes = 1e-4; long softAccepts = 0; double softMaxY = 0, softMaxZ = 0, softMaxRes = 0;   // after 50 iterations a last correction below this is accepted
  static constexpr double kChordFirst = 2e-3;   // largest first correction (V) with which the kept factorisation is still trusted
  double lastY = 0, lastZ = 0, lastR = 0; int dbgId = -1, traceCount = 0, lastPort = 0; double lastMx = 0, lastP = 0; long facs = 0, fails = 0, retries = 0; std::vector<int> halfIdx; double portVt[NP > 0 ? NP : 1], portVcrit[NP > 0 ? NP : 1];   // junction ports: thermal voltage and critical voltage for the limiting (0 for the other ports)
  double LU[NP][NP]; int piv[NP]; bool luOk = false; int luProp = -1;   // factorisation of the Newton matrix kept between steps (chord iteration)
  unsigned long long prof[8] = {0}, tlast_ = 0;
  double fmax[NF > 0 ? NF : 1] = {0}, pmax[NP > 0 ? NP : 1] = {0}, pmin[NP > 0 ? NP : 1] = {0}; bool stats = false;   // diagnosis: extremes of the device currents and port voltages

  void Init()
  {
    for (int k = 0; k < M::kNumBjt; ++k)
    {
      const double* q = &M::kBjt[9 * k]; for (int i = 0; i < 9; ++i) bp[k][i] = q[i];
      bp[k][9] = 1.0 / (q[4] * kVt); bp[k][10] = 1.0 / (q[7] * kVt); bp[k][11] = 1.0 / q[5]; bp[k][12] = 1.0 / q[6]; bp[k][13] = 1.0 / q[2]; bp[k][14] = 1.0 / q[3]; bp[k][15] = 0.0;
    }
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
    props.push_back(std::move(P)); return (int)props.size() - 1;
  }
  // device currents f (and, with jd, the entries of d f / d p per device: BJT 2x2 block (d[0][0], d[0][1], d[1][0], d[1][1]), diode / tanh the slope in d[0][0])
  void EvalF(const double* q, double* f, double (*jd)[4]) const
  {
    for (int d = 0; d < ND; ++d)
    {
      const int pp = M::kDevPort[d], cc = M::kDevCur[d], idx = M::kDevIdx[d], ty = M::kDevType[d];
      if (ty == 0)
      {
        const BjtOut o = BjtEvalG(bp[idx], q[pp], q[pp + 1]); f[cc] = o.ic; f[cc + 1] = o.ib;
        if (jd) { jd[d][0] = o.d[0][0]; jd[d][1] = o.d[0][1]; jd[d][2] = o.d[1][0]; jd[d][3] = o.d[1][1]; }
      }
      else if (ty == 1) { double i, g; DiodeEvalG(&M::kDio[5 * idx], q[pp], i, g); f[cc] = i; if (jd) jd[d][0] = g; }
      else
      {
        const double gm = M::kTanh[2 * idx], im = M::kTanh[2 * idx + 1], x = gm * q[pp] / im;
        if (std::fabs(x) < 0.05) { const double x2 = x * x; f[cc] = im * x * x2 * (-1.0 / 3.0 + x2 * (2.0 / 15.0)); if (jd) jd[d][0] = -gm * x2 * (1.0 - x2 * (2.0 / 3.0)); }   // tanh(x) - x and 1 - tanh^2 - 1 by series
        else { const double th = std::tanh(x); f[cc] = im * th - gm * q[pp]; if (jd) jd[d][0] = gm * (1.0 - th * th) - gm; }
      }
    }
  }
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
  double Step(int pi, double tr0, double tr1, double dtr, int depth = 0)
  {
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
#ifdef FULLFAST_PROFILE
    tlast_ = __rdtsc();
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
    (void)cu; FP_T(0)
    for (int a = 0; a < NP; ++a)
    {
      double v = PC[a] + PD[a * NU] * acc + PD[a * NU + 1] * tr1 + PD[a * NU + 2] * dtr;
      const double* px = &P.PXz[a * R]; for (int i = 0; i < R; ++i) v += px[i] * zk[i];
      pk[a] = v;
    }
    FP_T(1)
    double f1[NF], jd[ND][4], F[NP]; bool fresh = false;
#ifdef FULLFAST_DEBUG
    double Jc[NP][NP], Fc[NP];
#endif
#if FULLFAST_PREDICT
    for (int a2 = 0; a2 < NP; ++a2) { const double* zp = &P.Zp[a2 * NF]; double zf = 0.0; for (int k = 0; k < NF; ++k) zf += zp[k] * f0[k]; p[a2] = pk[a2] + zf; }   // start from the linear prediction with the currents of the previous step
#endif
    bool refac = !luOk || luProp != pi || FULLFAST_NOCHORD; const bool fromPrev = !refac; double prevMx = 1e30; bool firstChord = true;
    int itDone = 0;
    for (int it = 0; it < 50; ++it)
    {
      itDone = it + 1;
      EvalF(p, f1, refac ? jd : nullptr); ++its; FP_T(2)
      if (refac)
      {
        for (int a = 0; a < NP; ++a)
        {
          const double* zp = &P.Zp[a * NF]; double zf = 0.0; for (int k = 0; k < NF; ++k) zf += zp[k] * f1[k];
          F[a] = pk[a] + zf - p[a];
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
        FP_T(3)
        // LU with partial pivoting, in place (multipliers kept below the diagonal)
        for (int c = 0; c < NP; ++c)
        {
          int pr = c; double mx = std::fabs(LU[c][c]); for (int i = c + 1; i < NP; ++i) if (std::fabs(LU[i][c]) > mx) { mx = std::fabs(LU[i][c]); pr = i; }
          piv[c] = pr;
          if (pr != c) for (int j = 0; j < NP; ++j) std::swap(LU[c][j], LU[pr][j]);
          const double inv = 1.0 / LU[c][c];
          for (int i = c + 1; i < NP; ++i) { const double fct = LU[i][c] * inv; LU[i][c] = fct; if (fct == 0.0) continue; for (int j = c + 1; j < NP; ++j) LU[i][j] -= fct * LU[c][j]; }
        }
        luOk = true; luProp = pi; refac = false; fresh = true; firstChord = true; ++facs; FP_T(4)
      }
      else
      {
        for (int a = 0; a < NP; ++a) { const double* zp = &P.Zp[a * NF]; double zf = 0.0; for (int k = 0; k < NF; ++k) zf += zp[k] * f1[k]; F[a] = pk[a] + zf - p[a]; }
        fresh = false; FP_T(3)
      }
      for (int c = 0; c < NP; ++c) if (piv[c] != c) std::swap(F[c], F[piv[c]]);   // all interchanges first (the multipliers are stored with their rows permuted), then L, then U
      for (int c = 0; c < NP; ++c) for (int i = c + 1; i < NP; ++i) F[i] -= LU[i][c] * F[c];
      for (int i = NP - 1; i >= 0; --i) { double sum = F[i]; for (int l = i + 1; l < NP; ++l) sum -= LU[i][l] * F[l]; F[i] = sum / LU[i][i]; }
#ifdef FULLFAST_DEBUG
      if (refac == false && fresh && steps < 2) { double r = 0; for (int a2 = 0; a2 < NP; ++a2) { double v = -Fc[a2]; for (int b2 = 0; b2 < NP; ++b2) v += Jc[a2][b2] * F[b2]; r = std::fmax(r, std::fabs(v)); } std::printf("  linear solve residual %.3e (max |F| %.3e)\n", r,[&]{ double m2 = 0; for (int a2 = 0; a2 < NP; ++a2) m2 = std::fmax(m2, std::fabs(Fc[a2])); return m2; }()); }
#endif
      FP_T(5)
      double mx = 0.0; for (int a = 0; a < NP; ++a) mx = std::fmax(mx, std::fabs(F[a]));
#ifdef FULLFAST_DEBUG
      if (steps < 3 || (FULLFAST_TRACE && tr0 > 1.87 && tr0 < 1.88 && P.h < 1.3e-6 && traceCount < 60 && ++traceCount)) { double sp = 0; for (int a3 = 0; a3 < NP; ++a3) sp += std::fabs(p[a3]); int am = 0; for (int a3 = 1; a3 < NP; ++a3) if (std::fabs(F[a3]) > std::fabs(F[am])) am = a3; std::printf("step %ld it %d mx %.3e fresh %d sum|p| %.6f argmax port %d (p %.5f dp %.5f)\n", steps, it, mx, (int)fresh, sp, am, p[am], F[am]); }
#endif
      // a factorisation kept from an earlier step is trusted for the first correction only when that is small; a factorisation made in this step is reused while it converges
      if (!fresh && mx >= newtonTol && ((fromPrev && it == 0 && mx > kChordFirst) || (it >= 1 && mx > 0.5 * prevMx))) { refac = true; continue; }   // the kept factorisation does not serve here: refactor at the same p, this update is dropped
      if (!fresh) firstChord = false;
      if (mx < newtonTol) break;   // p and f1 are a consistent pair; the remaining update is below the tolerance
#if FULLFAST_LIMIT > 0
      {   // junction voltage limiting as SPICE pnjlim: a junction port (BJT vbe / vbc, diode) never jumps far up the exponential in one iteration
        const double sc = mx > FULLFAST_CAP ? FULLFAST_CAP / mx : 1.0;
        for (int a = 0; a < NP; ++a)
        {
          double vnew = p[a] + sc * F[a];
          if (portVt[a] > 0.0 && vnew > portVcrit[a] && std::fabs(vnew - p[a]) > 2.0 * portVt[a])
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
    if (itDone >= 50)
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
    FP_T(5)
    for (int i = 0; i < R; ++i) { double a = zk[i]; const double* gb = &P.GbF[i * NF]; for (int k = 0; k < NF; ++k) a += gb[k] * f1[k]; z[i] = a; }
    for (int k = 0; k < NF; ++k) f0[k] = f1[k];
    FP_T(6)
    if (stats)
    {
      for (int k = 0; k < NF; ++k) if (std::fabs(f1[k]) > fmax[k]) fmax[k] = std::fabs(f1[k]);
      for (int a = 0; a < NP; ++a) { if (p[a] > pmax[a]) pmax[a] = p[a]; if (p[a] < pmin[a]) pmin[a] = p[a]; }
    }
    accLast = acc; ++steps; const double yo = Output(f1, tr1, dtr); if (!(std::fabs(yo) < 1e6)) ok = false; return yo;
  }
};

}}  // namespace dgg::pcmfast
