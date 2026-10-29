// Generalised fast model of a TR-909 voice over its whole hit (gen_full_model2.py / full_core2.py): reduced continuous model, any number NT of time-dependent sources (value linear
// over a step, derivative constant over the step) and the accent as constant input, integrated with the exponential integrator (first-order hold on the device currents), Newton
// on the port voltages. Devices: 0 BJT, 1 diode, 2 op-amp input stage (tanh deviation), 3 CD4069 inverter, 4 AN6912 comparator, 5 BA6110 OTA.
// Newton: factorisation kept between steps (chord), junction limiting (pnjlim), retry of a step that does not converge as two half steps (SetHalf).
#pragma once
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <limits>
#include <vector>
#include "X86Compat.h"
#include <algorithm>
#include <cstdio>
#ifdef FULLFAST2_PROF
#include <intrin.h>
#define PROF_T(v) (v) = __rdtsc()
#define PROF_ADD(acc, v) (acc) += __rdtsc() - (v)
#else
#define PROF_T(v)
#define PROF_ADD(acc, v)
#endif
#include "LateFast.h"
// AVX (no FMA) paths of the dense kernels: every 256-bit lane performs exactly the operation the 128-bit path performs on that element in the same order, so the results are bitwise equal; chosen at run time (cpuid + OS support), no compiler flag
inline bool g_ff2Avx = FF2DetectAvx();
inline int g_ff2DotMin = 32;   // shorter dot products stay on the 128-bit path (the horizontal reduction costs more than it saves)
#include "FullFast.h"   // DiodeEvalG, BjtEvalG

namespace dgg { namespace pcmfast {

struct InvPar { double an, ap, pp, vtn, vtp, lam, cs; };
inline double InvPosf(double x) { return 0.5 * (x + std::sqrt(x * x + 0.01)); }
inline double InvCur(const double* f, double vgs, double vds, bool n)
{
  const double ov = InvPosf(vgs - (n ? f[3] : f[4])), vs = std::fmax(f[6] * ov, 0.05);
  return (n ? f[0] : f[1]) * std::pow(ov, f[2]) * std::tanh(vds / vs) * (1.0 + f[5] * std::fabs(vds));
}
inline void InvDer(const double* f, double vgs, double vds, bool n, double& dgs, double& dds)   // Net909::invDer
{
  const double x = vgs - (n ? f[3] : f[4]), r = std::sqrt(x * x + 0.01), ov = 0.5 * (x + r), dov = 0.5 * (1.0 + x / r);
  const bool fl = f[6] * ov <= 0.05; const double vs = fl ? 0.05 : f[6] * ov, dvs = fl ? 0.0 : f[6] * dov;
  const double u = vds / vs, T = std::tanh(u), sech2 = 1.0 - T * T, L = 1.0 + f[5] * std::fabs(vds), A = n ? f[0] : f[1];
  const double Pw = std::pow(ov, f[2]), dP = ov > 0 ? f[2] * Pw / ov * dov : 0.0;
  dgs = A * (dP * T * L + Pw * sech2 * (-u / vs) * dvs * L);
  dds = A * Pw * (sech2 / vs * L + T * f[5] * (vds > 0 ? 1.0 : (vds < 0 ? -1.0 : 0.0)));
}

template <class M> struct FullFast2
{
  static constexpr int R = M::kR, NG = M::kNg, NS = M::kNS, NF = M::kNF, NP = M::kNP, ND = M::kNumDev, NU = M::kU, NT = M::kNT;
  static constexpr double kCmpK = 3e-3 / 0.2;
  struct Prop { double h = 0.0; std::vector<double> GqF, GcF, Phi, GaF, GbF, Ga, Gb, ConeC, Zp, ZpT, PXz, Zmax, wOut, XG; };
  std::vector<Prop> props; std::vector<int> halfIdx;
  double z[R], f0[NF], p[NP], bp[M::kNumBjt > 0 ? M::kNumBjt : 1][16], PD[NP * NU], PC[NP], portVt[NP], portVcrit[NP], portCap[NP], portScale[NP];
  double acc = M::kAcc, accLast = M::kAcc, yIdle = 0.0, newtonTol = 3e-5; int maxDepth = 6; double capH = 1.0, ps0[NP]; long sumAct = 0, nLU = 0; unsigned long long prof[16] = {0}; long why[6] = {0, 0, 0, 0, 0, 0}, asFallbacks = 0; long steps = 0, its = 0, facs = 0, fails = 0, retries = 0, softAccepts = 0;
  struct EvCache { double in[3] = {0, 0, 0}, f[2] = {0, 0}, jd[6] = {0, 0, 0, 0, 0, 0}; unsigned gen = 0; bool valid = false, hasJd = false; }; mutable EvCache evc[ND > 0 ? ND : 1]; bool evCache = false; bool dampOn = false; int abortIt = 0; bool noFallback = false;
  // quasi-linear devices (QL): a diode or tanh stage whose slope stayed within qlTol of its slope at the last linearisation for qlHold steps is evaluated as the linear model i_lin + g_lin (v - v_lin) in the Newton iteration; at the end of the step
  // the exact current replaces the linear one in the state, the device is relinearised, and it is dropped back to exact evaluation when its slope has moved by more than qlTol; a step in which a linear model was off by more than qlErr (in port volts) is repeated
  bool finCache = false; long finHits = 0, finCalls = 0, capHits = 0, capCalls = 0; std::vector<double> e1v, e1i, e1g; std::vector<unsigned char> e1ok;   // FINCACHE: the exact evaluation at the end of the step is skipped when the port voltage is bit-identical to the last evaluation
  bool qlOn = false; double qlTol = 0.05, qlErr = 1e-4; int qlHold = 30; std::vector<int> qlDev; std::vector<unsigned char> ql; std::vector<double> qlV, qlI, qlG; std::vector<int> qlStab, qlAge, qlHoldD, qlRejDev; bool qlBackoff = true; int qlSkip = 1; std::vector<double> qlRejMax; long qlSteps = 0, qlDrops = 0, qlReject = 0, qlDevSteps = 0, qlActSteps = 0;
  void QlEnable(bool on) { qlOn = on; qlDev.clear(); for (int d = 0; d < ND; ++d) if (M::kDevType[d] == 1 || M::kDevType[d] == 2) qlDev.push_back(d); e1v.assign(ND, 0.0); e1i.assign(ND, 0.0); e1g.assign(ND, 0.0); e1ok.assign(ND, 0); ql.assign(ND, 0); qlAge.assign(ND, 0); qlHoldD.assign(ND, qlHold); elim.assign(ND, 0); qlRejDev.assign(ND, 0); qlRejMax.assign(ND, 0.0); qlV.assign(ND, 0.0); qlI.assign(ND, 0.0); qlG.assign(ND, 0.0); qlStab.assign(ND, 0); }
  void Eval1(int d, double v, double& i, double& g) const
  {
    const int idx = M::kDevIdx[d];
    if (M::kDevType[d] == 1) { DiodeEvalG(&M::kDio[5 * idx], v, i, g, &dioVj[idx], dioPre ? &dioXbv[idx] : nullptr); return; }
    const double gm = M::kTanh[3 * idx], im = M::kTanh[3 * idx + 1], lg = M::kTanh[3 * idx + 2], x = gm * v / im;
    if (lg == gm && std::fabs(x) < 0.05) { const double x2 = x * x; i = im * x * x2 * (-1.0 / 3.0 + x2 * (2.0 / 15.0)); g = -gm * x2 * (1.0 - x2 * (2.0 / 3.0)); }
    else { const double th = std::tanh(x); i = im * th - lg * v; g = gm * (1.0 - th * th) - lg; }
  }
  bool QlFinish(double* f1, const Prop& P)
  {
    bool good = true; ++qlSteps;
    for (int d : qlDev)
    {
      if (qlSkip > 1 && ql[d] && qlAge[d] >= qleHold && (qlSteps % qlSkip) != 0) { ++qlAge[d]; continue; }   // long-stable linear device: the exact evaluation only every qlSkip-th step
      const int pp = M::kDevPort[d], cc = M::kDevCur[d]; double i, g; if (finCache && e1ok[d] && e1v[d] == p[pp]) { i = e1i[d]; g = e1g[d]; ++finHits; } else { Eval1(d, p[pp], i, g); e1v[d] = p[pp]; e1i[d] = i; e1g[d] = g; e1ok[d] = 1; } ++finCalls; const double w = P.Zmax[cc], gfl = 1e-3 * asTol / std::fmax(w, 1e-30);
      if (ql[d])
      {
        ++qlDevSteps; ++qlAge[d]; if (w * std::fabs(qlG[d]) > asTol) ++qlActSteps; const double lin = qlI[d] + qlG[d] * (p[pp] - qlV[d]);
        const bool bad = w * std::fabs(i - lin) > qlErr; if (bad) { if (qlBackoff) qlHoldD[d] = std::min(qlHoldD[d] * 2, 100000); good = false; ++qlReject; ++qlRejDev[d]; qlRejMax[d] = std::fmax(qlRejMax[d], w * std::fabs(i - lin)); }
        f1[cc] = i; qlV[d] = p[pp]; qlI[d] = i;   // the offset of the model follows the exact current at the end of every step
        if (bad || std::fabs(g - qlG[d]) > qlTol * std::fmax(std::fabs(qlG[d]), gfl)) { ql[d] = 0; qlStab[d] = 0; qlAge[d] = 0; qlV[d] = p[pp]; qlG[d] = g; ++qlDrops; }
      }
      else
      {
        if (std::fabs(g - qlG[d]) > qlTol * std::fmax(std::fabs(qlG[d]), gfl)) { qlG[d] = g; qlStab[d] = 0; }
        else if (++qlStab[d] >= qlHoldD[d]) { ql[d] = 1; qlV[d] = p[pp]; qlI[d] = i; qlG[d] = g; }
      }
      if (qleOn) { const double wg = w * std::fabs(qlG[d]); const bool want = ql[d] && (elim[d] || (qlAge[d] >= qleHold && wg > qleHi * asTol)); if (want != (elim[d] != 0)) { elim[d] = want ? 1 : 0; ++qVer; ++qleToggle[d]; } }
    }
    return good;
  }
  // quasi-linear capacitances (QLC=1): the charge of a junction capacitance is linearised at the step start (q0, C0 of the exact evaluation, cached from the end of the previous step), f = r1 C0 (v - v0)/h in the Newton iteration;
  // at the end of the step the exact charge replaces it in the state; a step in which the linear model was off by more than qlcErr (weighted, volts), or a capacitance that changed by more than qlcTol in the step, falls back to exact evaluation for a while
  bool qlcOn = false; double qlcTol = 0.05, qlcErr = 1e-4; int qlcWait = 20; std::vector<int> capDev; std::vector<unsigned char> qlcAct, qcOk; std::vector<int> qlcOff; std::vector<double> qcV, qcQ, qcC; long qlcSteps = 0, qlcReject = 0, qlcActSteps = 0, qlcDrops = 0;
  void QlcEnable(bool on) { qlcOn = on; capDev.clear(); for (int d = 0; d < ND; ++d) if (M::kDevType[d] == 6) capDev.push_back(d); qlcAct.assign(ND, 0); qcOk.assign(ND, 0); qlcOff.assign(ND, 0); qcV.assign(ND, 0.0); qcQ.assign(ND, 0.0); qcC.assign(ND, 0.0); }
  bool QlcFinish(double* f1, const Prop& P)
  {
    bool good = true; ++qlcSteps;
    for (int d : capDev)
    {
      const int pp = M::kDevPort[d], cc = M::kDevCur[d], idx = M::kDevIdx[d]; const double* r = &M::kCap[10 * idx]; double q1, c1; ++capCalls; if (finCache && qcOk[d] && qcV[d] == p[pp]) { q1 = qcQ[d]; c1 = qcC[d]; ++capHits; } else CapQ(r, p[pp], q1, c1, true);
      qcV[d] = p[pp]; qcQ[d] = q1; qcC[d] = c1; qcOk[d] = 1; const double fex = r[1] * (q1 - q0v[d]) / capH, c0 = c0v[d];
      if (qlcAct[d])
      {
        ++qlcActSteps; if (P.Zmax[cc] * std::fabs(fex - f1[cc]) > qlcErr) { good = false; ++qlcReject; qlcAct[d] = 0; qlcOff[d] = qlcWait * 4; }
        f1[cc] = fex;
      }
      const bool still = std::fabs(c1 - c0) <= qlcTol * std::fabs(c0 + r[2]);   // relative to the total capacitance (the charge function has the linear part r[2] v removed)
      if (!still) { if (qlcAct[d]) ++qlcDrops; qlcAct[d] = 0; qlcOff[d] = std::max(qlcOff[d], qlcWait); }
      else if (!qlcAct[d] && --qlcOff[d] <= 0) qlcAct[d] = 1;
    }
    return good;
  }
  // QL elimination (QLE=1): active quasi-linear ports (current c_i + g_i p_i) are removed from the Newton unknowns. Their ports follow from p_Q = t + Tm f_N (t from the step offset), the remaining ports see the effective propagator
  // Ze[a][k] = Zp[a][k] + sum_i Zp[a][qCur_i] g_i Tm[i][k] and the offset sum_i Zp[a][qCur_i] e_i; the matrix of the Newton iteration is I - Ze g_N and does not contain the eliminated ports
  struct QE { int pi = -1; unsigned ver = 0; bool bad = false; std::vector<double> Ze, ZeT, Minv, Tm, Zqq, ZpQ; };
  bool qleOn = false; std::vector<unsigned char> elim; std::vector<QE> qeC; unsigned qVer = 1, luQVer = 0, qSetVer = 0; bool isQ[NP]; int nQ = 0, qPort[NP], qCur[NP], qDev[NP]; const unsigned char* skipDev = nullptr; double qleHi = 2.0, qleLo = 0.5; int qleHold = 300; long qleToggle[ND > 0 ? ND : 1] = {0}; long qleBuilds = 0, qleSteps = 0, qleBad = 0, qleNsum = 0;
  void QleSetup()
  {
    nQ = 0; for (int a = 0; a < NP; ++a) isQ[a] = false;
    for (int d : qlDev) if (elim[d]) { qPort[nQ] = M::kDevPort[d]; qCur[nQ] = M::kDevCur[d]; qDev[nQ] = d; isQ[qPort[nQ]] = true; ++nQ; }
    qSetVer = qVer;
  }
  bool QleBuild(const Prop& P, QE& e, int pi)
  {
    ++qleBuilds; e.pi = pi; e.ver = qVer; e.bad = false; const int n = nQ;
    e.Zqq.assign(n * n, 0.0); std::vector<double> J(n * n), I(n * n, 0.0); e.Minv.assign(n * n, 0.0);
    for (int i = 0; i < n; ++i) for (int j = 0; j < n; ++j) { e.Zqq[i * n + j] = P.Zp[qPort[i] * NF + qCur[j]]; J[i * n + j] = (i == j ? 1.0 : 0.0) - e.Zqq[i * n + j] * qlG[qDev[j]]; }
    for (int i = 0; i < n; ++i) I[i * n + i] = 1.0;
    for (int c = 0; c < n; ++c)   // Gauss-Jordan with partial pivoting
    {
      int pr = c; for (int i = c + 1; i < n; ++i) if (std::fabs(J[i * n + c]) > std::fabs(J[pr * n + c])) pr = i;
      if (std::fabs(J[pr * n + c]) < 1e-9) { e.bad = true; ++qleBad; return false; }
      if (pr != c) for (int j = 0; j < n; ++j) { std::swap(J[c * n + j], J[pr * n + j]); std::swap(I[c * n + j], I[pr * n + j]); }
      const double inv = 1.0 / J[c * n + c]; for (int j = 0; j < n; ++j) { J[c * n + j] *= inv; I[c * n + j] *= inv; }
      for (int i = 0; i < n; ++i) if (i != c) { const double f = J[i * n + c]; if (f == 0.0) continue; for (int j = 0; j < n; ++j) { J[i * n + j] -= f * J[c * n + j]; I[i * n + j] -= f * I[c * n + j]; } }
    }
    e.Minv = I; bool qc[NF]; for (int k = 0; k < NF; ++k) qc[k] = false; for (int i = 0; i < n; ++i) qc[qCur[i]] = true;
    e.Tm.assign(n * NF, 0.0); for (int i = 0; i < n; ++i) for (int j = 0; j < n; ++j) { const double m = e.Minv[i * n + j]; if (m == 0.0) continue; const double* zr = &P.Zp[qPort[j] * NF]; for (int k = 0; k < NF; ++k) e.Tm[i * NF + k] += m * zr[k]; }
    for (int i = 0; i < n; ++i) for (int k = 0; k < NF; ++k) if (qc[k]) e.Tm[i * NF + k] = 0.0;
    e.ZpQ.assign(NP * n, 0.0); for (int a = 0; a < NP; ++a) for (int i = 0; i < n; ++i) e.ZpQ[a * n + i] = P.Zp[a * NF + qCur[i]];
    e.Ze.assign(NP * NF, 0.0); e.ZeT.assign(NF * NP, 0.0);
    for (int a = 0; a < NP; ++a)
    {
      if (isQ[a]) continue; double* ze = &e.Ze[a * NF]; for (int k = 0; k < NF; ++k) ze[k] = qc[k] ? 0.0 : P.Zp[a * NF + k];
      for (int i = 0; i < n; ++i) { const double w = e.ZpQ[a * n + i] * qlG[qDev[i]]; if (w == 0.0) continue; const double* tm = &e.Tm[i * NF]; for (int k = 0; k < NF; ++k) ze[k] += w * tm[k]; }
      for (int k = 0; k < NF; ++k) e.ZeT[k * NP + a] = ze[k];
    }
    return true;
  }
  // measurement of the change of the differential conductances (GSTAT): a device is relinearised when its slope has moved by more than tolRel x the slope at the last linearisation (slopes whose weighted size Zmax*g stays below 1e-3 asTol never count)
  bool gStat = false; int lastPi = 0; bool gRefOk = false; double gRefT[4][ND > 0 ? ND : 1][6]; long gEv[8][8][4] = {{{0}}}, gEntSteps[8][8] = {{0}}; long gDevEv[8][4][ND > 0 ? ND : 1] = {{{0}}}; double gwMax[8][ND > 0 ? ND : 1] = {{0}}; long gSteps[8] = {0};
  // BJT linear-model check (BJTSTAT): a BJT linearised at v0 (currents i0, slopes J) keeps its model until a slope has moved by more than tol x its size; counts steps in which the weighted current error Zmax |i - i_lin| exceeds err
  double bjV0[ND > 0 ? ND : 1][2], bjI0[ND > 0 ? ND : 1][2], bjJ[ND > 0 ? ND : 1][4]; bool bjInit[ND > 0 ? ND : 1] = {false}; long bjSteps[3] = {0, 0, 0}, bjViol[3][4] = {{0}}, bjReLin[3] = {0, 0, 0}; double bjErr[4] = {1e-4, 1e-3, 1e-2, 1e-1};
  void BjtStat(const double* f, double (*jd)[6], const Prop& P)
  {
    static const double tolB[3] = {0.2, 0.5, 1.0};
    for (int d = 0; d < ND; ++d) { if (M::kDevType[d] != 0) continue; const int pp = M::kDevPort[d], cc = M::kDevCur[d]; double wgt = 0; for (int k = 0; k < 4; ++k) wgt = std::max(wgt, std::fabs(jd[d][k]) * P.Zmax[cc + (k >> 1)]); if (wgt <= asTol) continue;
      for (int t = 0; t < 3; ++t) { if (!bjInit[d]) continue; double e = 0; for (int c = 0; c < 2; ++c) { double lin = bjI0[d][c]; for (int b = 0; b < 2; ++b) lin += bjJ[d][c * 2 + b] * (p[pp + b] - bjV0[d][b]); e = std::max(e, P.Zmax[cc + c] * std::fabs(f[cc + c] - lin)); } ++bjSteps[t]; for (int k = 0; k < 4; ++k) if (e > bjErr[k]) ++bjViol[t][k]; bool mv = false; for (int k = 0; k < 4; ++k) if (std::fabs(jd[d][k] - bjJ[d][k]) > tolB[t] * std::fabs(bjJ[d][k])) mv = true; if (mv) ++bjReLin[t]; }
      // one reference per device (tolerance 0.5): refresh when a slope moved by more than 50 %
      bool mv = !bjInit[d]; if (bjInit[d]) for (int k = 0; k < 4; ++k) if (std::fabs(jd[d][k] - bjJ[d][k]) > 0.5 * std::fabs(bjJ[d][k])) mv = true; if (mv) { bjInit[d] = true; for (int k = 0; k < 4; ++k) bjJ[d][k] = jd[d][k]; } for (int b = 0; b < 2; ++b) { bjV0[d][b] = p[pp + b]; bjI0[d][b] = f[cc + b]; } }
  }
  void GRecord(int win)
  {
    const Prop& P = props[lastPi]; double f[NF], jd[ND > 0 ? ND : 1][6]; for (int d = 0; d < ND; ++d) for (int e = 0; e < 6; ++e) jd[d][e] = 0.0; EvalF(p, f, jd);
    static const double tolRel[4] = {0.01, 0.05, 0.2, 1.0}; ++gSteps[win];
    for (int d = 0; d < ND; ++d)
    {
      const int ty = M::kDevType[d], npt = nPortsOf(ty), ncu = nCursOf(ty), cc = M::kDevCur[d];
      for (int e = 0; e < npt * ncu; ++e)
      {
        const double g = jd[d][e], w = P.Zmax[cc + e / npt]; gwMax[win][d] = std::fmax(gwMax[win][d], w * std::fabs(g)); ++gEntSteps[win][ty];
        if (!gRefOk) { for (int t = 0; t < 4; ++t) gRefT[t][d][e] = g; continue; }
        for (int t = 0; t < 4; ++t)
        {
          const double dg = std::fabs(g - gRefT[t][d][e]); if (w * dg < 1e-3 * asTol) continue;
          if (dg > tolRel[t] * std::fmax(std::fabs(gRefT[t][d][e]), 1e-3 * asTol / std::fmax(w, 1e-30))) { ++gEv[win][ty][t]; ++gDevEv[win][t][d]; gRefT[t][d][e] = g; }
        }
      }
    }
    BjtStat(f, jd, P); gRefOk = true;
  } double quadW = 0.0, quadCW = 0.0, fStartPrev[NF] = {0}; bool fPrevOk = false; double dampRatio = 0.9, dampMin = 1e-3; bool actStat = false; int lastAmx = 0; long wouldFreeze[8][8] = {{0}}; int actWin = 0; long actByType[8][8] = {{0}}, actN[8] = {0}; mutable unsigned long long evalProf[8] = {0,0,0,0,0,0,0,0}, evalCnt[8] = {0,0,0,0,0,0,0,0}; mutable unsigned q0Gen = 1, q0G[ND > 0 ? ND : 1] = {}; mutable double q0v[ND > 0 ? ND : 1] = {}, c0v[ND > 0 ? ND : 1] = {}; int frzMode = 0; double frzMul = 1.0, frzThr = 3.0, jacTol = 0.5; double jdFac[ND > 0 ? ND : 1][6]; double chordFirst = 2e-3; double predW = 0.0, pStartPrev[NP], hPrevStep = 0.0; bool chordAct = false;   /* predW > 0: initial port guess extrapolated linearly from the previous step (weight predW) */   /* chordAct: the first-correction test for a refactorisation only looks at the active ports */ static constexpr double kCap = 5.0, kSoftTol = 1e-3, kSoftY = 1e-6, kSoftZ = 1e-4, kSoftRes = 1e-4;
  double LU[NP][NP], Jc[NP][NP], JFA[NP][NP], pRef[NP]; bool portPure[NP]; int piv[NP], actL[NP], frzL[NP], nAct = NP, nFrz = 0; bool activeSet = true, keepAcrossH = false; double asTol = 1e-2; bool luOk = false; int luProp = -1; int dbgId = -1; double lastMx = 0; bool traceOn = false, noChord = false, zoh = false; double zohW = 0.1, zohHMin = 0.0; int maxIt = 50;
  static constexpr int nPortsOf(int ty) { return ty == 0 ? 2 : (ty == 3 ? 3 : (ty == 4 ? 2 : (ty == 5 ? 3 : 1))); }
  static constexpr int nCursOf(int ty) { return ty == 0 ? 2 : (ty == 3 ? 2 : 1); }

  void Init()
  {
    for (int k = 0; k < M::kNumDio; ++k) { const double* q = &M::kDio[5 * k]; const double nvt = q[1] * kVt; dioXbv[k] = q[3] > 1e20 ? 1e30 : q[3] - nvt * std::log(std::fmax(q[4] / q[0], 1.0)); }
    for (int k = 0; k < M::kNumBjt; ++k)
    {
      const double* q = &M::kBjt[9 * k]; for (int i = 0; i < 9; ++i) bp[k][i] = q[i];
      bp[k][9] = 1.0 / (q[4] * kVt); bp[k][10] = 1.0 / (q[7] * kVt); bp[k][11] = 1.0 / q[5]; bp[k][12] = 1.0 / q[6]; bp[k][13] = 1.0 / q[2]; bp[k][14] = 1.0 / q[3]; bp[k][15] = 0.0;
    }
    for (int a = 0; a < NP; ++a) { portVt[a] = 0.0; portVcrit[a] = 0.0; portCap[a] = 1e30; portScale[a] = 1.0; }
    for (int d = 0; d < ND; ++d) { if (M::kDevType[d] == 4) portScale[M::kDevPort[d] + 1] = 37.5e-6; else if (M::kDevType[d] == 5) portScale[M::kDevPort[d]] = 52e-3; }   // volts per port unit of the comparator / OTA input ports (the 5 V step cap is a cap in volts)
    { const double ic = 1.0; for (int d = 0; d < ND; ++d) if (M::kDevType[d] == 3) { portCap[M::kDevPort[d]] = ic; portCap[M::kDevPort[d] + 1] = ic; } }   // CD4069 gate: input and output port move by at most INVCAP per iteration (the channel currents saturate; an unlimited step ends far outside the rails)
    for (int d = 0; d < ND; ++d)
    {
      const int pp = M::kDevPort[d], idx = M::kDevIdx[d];
      if (M::kDevType[d] == 0) { const double IS = M::kBjt[9 * idx + 1], vtE = M::kBjt[9 * idx + 4] * kVt; portVt[pp] = vtE; portVcrit[pp] = vtE * std::log(vtE / (1.41421356 * IS)); portVt[pp + 1] = kVt; portVcrit[pp + 1] = kVt * std::log(kVt / (1.41421356 * IS)); }
      else if (M::kDevType[d] == 6) { const double* r = &M::kCap[10 * idx]; const double vt = (int)r[0] == 1 ? kVt : 1.0 / r[8]; portVt[pp] = vt; portVcrit[pp] = vt * std::log(vt / (1.41421356 * r[7])); }   // a charge device sees the same junction voltage as its BJT / diode: same limiting
      else if (M::kDevType[d] == 1) { const double Is = M::kDio[5 * idx], nvt = M::kDio[5 * idx + 1] * kVt; portVt[pp] = nvt; portVcrit[pp] = nvt * std::log(nvt / (1.41421356 * Is)); }
    }
    for (int a = 0; a < NP; ++a) { bool pure = true; for (int s_ = 0; s_ < NS; ++s_) if (M::kPp[a * NS + s_] != 0.0) { pure = false; break; } portPure[a] = pure; }
    props.clear(); steps = its = facs = fails = retries = softAccepts = 0; Reset(acc);
  }
  void ResetJob() { steps = its = facs = fails = retries = softAccepts = 0; Reset(acc); }
  void Reset(double accent)
  {
    hPrevStep = 0.0; fPrevOk = false; acc = accent; accLast = accent; int g = 0; while (g < M::kAccGrid - 2 && accent >= M::kAccV[g + 1]) ++g;
    double t = M::kAccGrid > 1 ? (accent - M::kAccV[g]) / (M::kAccV[g + 1] - M::kAccV[g]) : 0.0; t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
    const int g1 = M::kAccGrid > 1 ? g + 1 : g;
    for (int i = 0; i < R; ++i) z[i] = (1.0 - t) * M::kZ0[g * R + i] + t * M::kZ0[g1 * R + i];
    for (int i = 0; i < NP; ++i) p[i] = (1.0 - t) * M::kP0[g * NP + i] + t * M::kP0[g1 * NP + i];
    yIdle = (1.0 - t) * M::kY0[g] + t * M::kY0[g1];
    for (int i = 0; i < NP; ++i)
    {
      for (int u = 0; u < NU; ++u) { double v = 0.0; for (int s = 0; s < NS; ++s) v += M::kPp[i * NS + s] * M::kDgS[s * NG + u]; PD[i * NU + u] = v + M::kPuP[i * NU + u]; }
      double v = M::kPc[i]; for (int s = 0; s < NS; ++s) v += M::kPp[i * NS + s] * M::kDgS[s * NG + NG - 1]; PC[i] = v;
    }
    for (int i = 0; i < NP; ++i) ps0[i] = p[i]; capH = 1.0; ++q0Gen; EvalF(p, f0, nullptr); luOk = false;
  }
  void UIdle(double* u) const { for (int j = 0; j < NU; ++j) u[j] = M::kU0[j]; }   // inputs at the idle point (nominal accent entry)
  void SetHalf(int pi, int ph) { if ((int)halfIdx.size() <= pi) halfIdx.resize(pi + 1, -1); halfIdx[pi] = ph; }
  int Prepare(double h)
  {
    for (size_t i = 0; i < props.size(); ++i) if (std::fabs(props[i].h - h) < 1e-15) return (int)i;
    Prop P; P.h = h;
    const bool qd = quadW != 0.0, qc = quadCW != 0.0; const int n = R + (qd ? 3 : 2) * NG; std::vector<double> N(n * n, 0.0);
    for (int i = 0; i < R; ++i) { for (int j = 0; j < R; ++j) N[i * n + j] = M::kA[i * R + j] * h; for (int j = 0; j < NG; ++j) N[i * n + R + j] = M::kBg[i * NG + j] * h; }
    for (int j = 0; j < NG; ++j) N[(R + j) * n + R + NG + j] = h;
    if (qd) for (int j = 0; j < NG; ++j) N[(R + NG + j) * n + R + 2 * NG + j] = h;
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
      if (qd) { if (P.GqF.empty()) P.GqF.assign(R * NF, 0.0); for (int k = 0; k < NF; ++k) { double sq = 0.0; for (int s = 0; s < NS; ++s) sq += (2.0 * E[i * n + R + 2 * NG + NU + s] - h * E[i * n + R + NG + NU + s]) * M::kTc[s * NF + k]; P.GqF[i * NF + k] = sq; } }
    }
    if (zoh || zohW > 0.0) { const double w = zoh ? 1.0 : (P.h >= zohHMin ? zohW : 0.0); for (size_t i = 0; i < P.GbF.size(); ++i) { P.GbF[i] += w * P.GaF[i]; P.GaF[i] *= 1.0 - w; } }   // zohW: share of the start-value weight moved to the end value (0 = first-order hold, 1 = zero-order hold)   // zero-order hold: the device currents are taken constant (= end value) over the step (backward-Euler-like treatment of the nonlinear part)
    if (qc) { P.GcF.assign(R * NF, 0.0); for (int d = 0; d < ND; ++d) if (M::kDevType[d] == 6) { const int col = M::kDevCur[d]; for (int i = 0; i < R; ++i) { double sq = 0.0; for (int s = 0; s < NS; ++s) sq += (E[i * n + R + NG + NU + s] - 0.5 * h * E[i * n + R + NU + s]) * M::kTc[s * NF + col]; P.GcF[i * NF + col] = sq; } } }
    for (int d = 0; d < ND; ++d) if (M::kDevType[d] == 6) { const int col = M::kDevCur[d]; for (int i = 0; i < R; ++i) { P.GbF[i * NF + col] += P.GaF[i * NF + col]; P.GaF[i * NF + col] = 0.0; if (qd) P.GqF[i * NF + col] = 0.0; } }   // charge currents are mean currents over the step (constant): the charge that flows is exactly Qn(p1) - Qn(p0)
    std::vector<double> XG(NS * NF, 0.0);
    for (int s = 0; s < NS; ++s) for (int k = 0; k < NF; ++k)
    {
      double v = 0.0; for (int i = 0; i < R; ++i) v += M::kXzS[s * R + i] * P.GbF[i * NF + k];
      for (int q = 0; q < NS; ++q) v += M::kDgS[s * NG + NU + q] * M::kTc[q * NF + k];
      XG[s * NF + k] = v;
    }
    P.XG = XG;
    for (int a = 0; a < NP; ++a)
    {
      for (int k = 0; k < NF; ++k) { double v = 0.0; for (int s = 0; s < NS; ++s) v += M::kPp[a * NS + s] * XG[s * NF + k]; P.Zp[a * NF + k] = v; }
      for (int i = 0; i < R; ++i) { double v = 0.0; for (int s = 0; s < NS; ++s) v += M::kPp[a * NS + s] * M::kXzS[s * R + i]; P.PXz[a * R + i] = v; }
    }
    P.ZpT.assign(NF * NP, 0.0); for (int a = 0; a < NP; ++a) for (int k = 0; k < NF; ++k) P.ZpT[k * NP + a] = P.Zp[a * NF + k];
    P.wOut.assign(NF, 0.0); for (int k = 0; k < NF; ++k) { double v = M::kDof[k]; for (int i = 0; i < R; ++i) v += M::kXo[i] * P.GbF[i * NF + k]; P.wOut[k] = v; }   // output sensitivity to the end-of-step device currents
    P.Zmax.assign(NF, 0.0); for (int k = 0; k < NF; ++k) for (int a = 0; a < NP; ++a) P.Zmax[k] = std::fmax(P.Zmax[k], std::fabs(P.Zp[a * NF + k]));
    props.push_back(std::move(P)); return (int)props.size() - 1;
  }
  // junction charge of a charge device (row of kCap) minus its constant part C0 v: charge q and capacitance c as functions of the junction voltage v
  static bool& OnePow() { static bool b = false; return b; }   // ONEPOW: charge and capacitance of the depletion region share one pow (different rounding)
  static void CapQ(const double* r, double v, double& q, double& c, bool wantC = true)
  {
    const int kind = (int)r[0]; const double C0 = r[2], cj0 = r[3], vj = r[4], m = r[5], tt = r[6];
    double qj = 0.0, cj = 0.0;
    if (cj0 > 0.0)
    {
      const double fc = 0.5;
      if (v < fc * vj) { const double t = 1.0 - v / vj; if (OnePow()) { const double pw = std::pow(t, -m); qj = vj * cj0 / (1.0 - m) * (1.0 - t * pw); if (wantC) cj = cj0 * pw; } else { qj = vj * cj0 / (1.0 - m) * (1.0 - std::pow(t, 1.0 - m)); if (wantC) cj = cj0 / std::pow(t, m); } }
      else { const double f1 = vj * (1.0 - std::pow(1.0 - fc, 1.0 - m)) / (1.0 - m), f2 = std::pow(1.0 - fc, 1.0 + m), f3 = 1.0 - fc * (1.0 + m); qj = cj0 * (f1 + (1.0 / f2) * (f3 * (v - fc * vj) + m / (2.0 * vj) * (v * v - fc * fc * vj * vj))); cj = cj0 / f2 * (f3 + m * v / vj); }
    }
    if (kind == 0) { const double a = v * r[8], e = a < -40.0 ? 0.0 : std::exp(std::fmin(a, 80.0)); const double cbe = r[7] * (e - 1.0) + kGmin * v, gbe = r[7] * e * r[8] + kGmin; q = qj + tt * cbe; c = cj + tt * gbe; }
    else if (kind == 2 && tt > 0.0) { double i, g; DiodeEvalG(&M::kDio[5 * (int)r[9]], v, i, g); q = qj + tt * i; c = cj + tt * g; }
    else { q = qj; c = cj; }
    q -= C0 * v; c -= C0;
  }
  // device currents f and the dense blocks d f / d p of every device (row-major [current][port], at most 2 x 3)
  void EvalF(const double* q, double* f, double (*jd)[6]) const
  {
    for (int d = 0; d < ND; ++d)
    {
      const int pp = M::kDevPort[d], cc = M::kDevCur[d], idx = M::kDevIdx[d], ty = M::kDevType[d];
      if (skipDev && skipDev[d]) continue;
      if (evCache)
      {   // the device is a pure function of its ports (a charge device also of the step start, q0Gen): a call with bit-identical ports repeats the previous result
        EvCache& e = evc[d]; const int npt_ = nPortsOf(ty), ncu_ = nCursOf(ty); bool hit = e.valid && (jd == nullptr || e.hasJd) && (ty != 6 || e.gen == q0Gen);
        for (int b_ = 0; hit && b_ < npt_; ++b_) if (q[pp + b_] != e.in[b_]) hit = false;
        if (hit) { for (int c_ = 0; c_ < ncu_; ++c_) f[cc + c_] = e.f[c_]; if (jd) for (int k_ = 0; k_ < 6; ++k_) jd[d][k_] = e.jd[k_]; continue; }
      }
#ifdef FULLFAST2_PROF
      const unsigned long long tdev = __rdtsc();
#endif
      if (qlOn && ql[d]) { f[cc] = qlI[d] + qlG[d] * (q[pp] - qlV[d]); if (jd) jd[d][0] = qlG[d]; continue; }
      switch (ty)
      {
      case 0: { const BjtOut o = BjtEvalG(bp[idx], q[pp], q[pp + 1]); f[cc] = o.ic; f[cc + 1] = o.ib; if (jd) { jd[d][0] = o.d[0][0]; jd[d][1] = o.d[0][1]; jd[d][2] = o.d[1][0]; jd[d][3] = o.d[1][1]; } break; }
      case 1: { double i, g; DiodeEvalG(&M::kDio[5 * idx], q[pp], i, g, &dioVj[idx], dioPre ? &dioXbv[idx] : nullptr); f[cc] = i; if (jd) jd[d][0] = g; break; }
      case 6: { const double* r = &M::kCap[10 * idx]; if (qlcOn) { if (q0G[d] != q0Gen) { if (qcOk[d] && qcV[d] == ps0[pp]) { q0v[d] = qcQ[d]; c0v[d] = qcC[d]; } else CapQ(r, ps0[pp], q0v[d], c0v[d], true); q0G[d] = q0Gen; } if (qlcAct[d]) { f[cc] = r[1] * c0v[d] * (q[pp] - ps0[pp]) / capH; if (jd) jd[d][0] = r[1] * c0v[d] / capH; break; } } double q1, c1; CapQ(r, q[pp], q1, c1, jd != nullptr); if (q0G[d] != q0Gen) { double c0; CapQ(r, ps0[pp], q0v[d], c0, false); q0G[d] = q0Gen; } f[cc] = r[1] * (q1 - q0v[d]) / capH; if (jd) jd[d][0] = r[1] * c1 / capH; break; }   // the charge at the step start does not change during the step: computed once per step   // backward-Euler current of the charge that the constant capacitances do not carry
      case 2:
      {
        const double gm = M::kTanh[3 * idx], im = M::kTanh[3 * idx + 1], lg = M::kTanh[3 * idx + 2], x = gm * q[pp] / im;
        if (lg == gm && std::fabs(x) < 0.05) { const double x2 = x * x; f[cc] = im * x * x2 * (-1.0 / 3.0 + x2 * (2.0 / 15.0)); if (jd) jd[d][0] = -gm * x2 * (1.0 - x2 * (2.0 / 3.0)); }
        else { const double th = std::tanh(x); f[cc] = im * th - lg * q[pp]; if (jd) jd[d][0] = gm * (1.0 - th * th) - lg; }
        break;
      }
      case 3:
      {
        const double pa = q[pp], pb = q[pp + 1], pc = q[pp + 2];
        f[cc] = InvCur(M::kInvPar, pa, pb, true); f[cc + 1] = InvCur(M::kInvPar, pc - pa, pc - pb, false);
        if (jd)
        {
          double ngs, nds, pgs, pds; InvDer(M::kInvPar, pa, pb, true, ngs, nds); InvDer(M::kInvPar, pc - pa, pc - pb, false, pgs, pds);
          jd[d][0] = ngs; jd[d][1] = nds; jd[d][2] = 0.0; jd[d][3] = -pgs; jd[d][4] = -pds; jd[d][5] = pgs + pds;
        }
        break;
      }
      case 4:
      {
        const double th = std::tanh(q[pp + 1]), vo = q[pp]; f[cc] = kCmpK * vo * 0.5 * (1.0 + th);
        if (jd) { jd[d][0] = kCmpK * 0.5 * (1.0 + th); jd[d][1] = kCmpK * vo * 0.5 * (1.0 - th * th); }
        break;
      }
      default:
      {
        const double gb = M::kOtaG[2 * idx], gc = M::kOtaG[2 * idx + 1], th = std::tanh(q[pp]), ictl = gb * q[pp + 1] + gc * q[pp + 2]; f[cc] = ictl * th;
        if (jd) { jd[d][0] = ictl * (1.0 - th * th); jd[d][1] = gb * th; jd[d][2] = gc * th; }
        break;
      }
      }
#ifdef FULLFAST2_PROF
      evalProf[ty] += __rdtsc() - tdev; ++evalCnt[ty];
#endif
      if (evCache)
      {
        EvCache& e = evc[d]; const int npt_ = nPortsOf(ty), ncu_ = nCursOf(ty);
        for (int b_ = 0; b_ < npt_; ++b_) e.in[b_] = q[pp + b_];
        for (int c_ = 0; c_ < ncu_; ++c_) e.f[c_] = f[cc + c_];
        e.hasJd = jd != nullptr; if (jd) for (int k_ = 0; k_ < 6; ++k_) e.jd[k_] = jd[d][k_];
        e.gen = q0Gen; e.valid = true;
      }
    }
  }
  // a[j] -= f b[j] (j < n); same operation order per element as the scalar loop
  // CUT: weak-coupling cut (file written by CUTOUT): the columns of the devices of group cutFrom are zeroed in the Newton-matrix rows of the ports of group cutTo (chord error ~ the cross loop gain), the active ports of group cutFrom are ordered first so that the LU skips the zero block
  long refGrp[6][3] = {{0}}; bool cutOn = false; int cutFrom = 0, cutTo = 1; signed char portGrp[NP > 0 ? NP : 1];
  signed char curGrp[NF > 0 ? NF : 1];
  double NodeErrGrp(int pi, const double* df, int grp) const { const Prop& P = props[pi]; double m = 0.0; for (int s = 0; s < NS; ++s) { double v = 0.0; const double* g = &P.XG[s * NF]; for (int k = 0; k < NF; ++k) if (curGrp[k] == grp) v += g[k] * df[k]; m = std::fmax(m, std::fabs(v)); } return m; }
  void PortGrpInit() { for (int k = 0; k < NF; ++k) curGrp[k] = -1; for (int a = 0; a < NP; ++a) portGrp[a] = -1; }
  void CutLoad(const char* fn) { PortGrpInit();  FILE* f = std::fopen(fn, "r"); if (!f) return; char line[256]; while (std::fgets(line, sizeof line, f)) { int a, b; if (std::sscanf(line, "drop %d %d", &a, &b) == 2) { cutFrom = a; cutTo = b; } else if (std::sscanf(line, "dev %d %d", &a, &b) == 2) { const int pp = M::kDevPort[a], np = nPortsOf(M::kDevType[a]); for (int k = 0; k < np; ++k) portGrp[pp + k] = (signed char)b; const int cc = M::kDevCur[a], nc = nCursOf(M::kDevType[a]); for (int k = 0; k < nc; ++k) curGrp[cc + k] = (signed char)b; } } std::fclose(f); cutOn = true; }
  bool fastDot = false;   // FASTDOT: dot products with four independent partial sums (different rounding, no serial dependency chain)
  inline double Dot(const double* __restrict a, const double* __restrict b, int n, double init = 0.0) const
  {
    if (!fastDot) { double s = init; for (int k = 0; k < n; ++k) s += a[k] * b[k]; return s; }
    return DotF(a, b, n, init);
  }
  mutable double dioVj[M::kNumDio > 0 ? M::kNumDio : 1] = {0}; double dioXbv[M::kNumDio > 0 ? M::kNumDio : 1] = {0}; bool dioPre = true;   // DIOPRE: breakdown offset of every diode evaluated once (bitwise equal)
  bool matVec = true;   // MATVEC: four-row interleaved matrix-vector products (bitwise equal to the row-wise DotF)
  bool fastSolve = false;   // FASTSOLVE: substitution of the Newton solve with row-wise dot products
  static inline double DotF(const double* __restrict a, const double* __restrict b, int n, double init)
  {
    if (g_ff2Avx && n >= g_ff2DotMin)
    {
      __m256d v = _mm256_setzero_pd(); int k = 0;
      for (; k + 4 <= n; k += 4) v = _mm256_add_pd(v, _mm256_mul_pd(_mm256_loadu_pd(a + k), _mm256_loadu_pd(b + k)));
      __m128d s0 = _mm_add_pd(_mm256_castpd256_pd128(v), _mm256_extractf128_pd(v, 1)); _mm256_zeroupper();
      double t[2]; _mm_storeu_pd(t, s0); double s = init + (t[0] + t[1]); for (; k < n; ++k) s += a[k] * b[k]; return s;
    }
    __m128d s0 = _mm_setzero_pd(), s1 = _mm_setzero_pd(); int k = 0;
    for (; k + 4 <= n; k += 4) { s0 = _mm_add_pd(s0, _mm_mul_pd(_mm_loadu_pd(a + k), _mm_loadu_pd(b + k))); s1 = _mm_add_pd(s1, _mm_mul_pd(_mm_loadu_pd(a + k + 2), _mm_loadu_pd(b + k + 2))); }
    s0 = _mm_add_pd(s0, s1); double t[2]; _mm_storeu_pd(t, s0); double s = init + (t[0] + t[1]); for (; k < n; ++k) s += a[k] * b[k]; return s;
  }
  // y[i] = DotF(M row i, x, n, 0) for all rows, four rows interleaved: every row keeps the two-accumulator summation order of DotF (bitwise the same result) while eight independent chains hide the addition latency
  static inline void MatVec(const double* __restrict Mat, int ld, int rows, const double* __restrict x, int n, double* y, const double* init = nullptr)
  {
    int i = 0;
    if (g_ff2Avx)
    {
      for (; i + 4 <= rows; i += 4)
      {
        const double* m0 = Mat + (size_t)i * ld; const double* m1 = m0 + ld; const double* m2 = m1 + ld; const double* m3 = m2 + ld;
        __m256d a = _mm256_setzero_pd(), b = a, c = a, d = a; int k = 0;
        for (; k + 4 <= n; k += 4)
        {
          const __m256d xv = _mm256_loadu_pd(x + k);
          a = _mm256_add_pd(a, _mm256_mul_pd(_mm256_loadu_pd(m0 + k), xv)); b = _mm256_add_pd(b, _mm256_mul_pd(_mm256_loadu_pd(m1 + k), xv));
          c = _mm256_add_pd(c, _mm256_mul_pd(_mm256_loadu_pd(m2 + k), xv)); d = _mm256_add_pd(d, _mm256_mul_pd(_mm256_loadu_pd(m3 + k), xv));
        }
        __m128d ra = _mm_add_pd(_mm256_castpd256_pd128(a), _mm256_extractf128_pd(a, 1)), rb = _mm_add_pd(_mm256_castpd256_pd128(b), _mm256_extractf128_pd(b, 1));
        __m128d rc = _mm_add_pd(_mm256_castpd256_pd128(c), _mm256_extractf128_pd(c, 1)), rd = _mm_add_pd(_mm256_castpd256_pd128(d), _mm256_extractf128_pd(d, 1));
        double t[8]; _mm_storeu_pd(t, ra); _mm_storeu_pd(t + 2, rb); _mm_storeu_pd(t + 4, rc); _mm_storeu_pd(t + 6, rd); _mm256_zeroupper();
        double s0 = (init ? init[i] : 0.0) + (t[0] + t[1]), s1 = (init ? init[i + 1] : 0.0) + (t[2] + t[3]), s2 = (init ? init[i + 2] : 0.0) + (t[4] + t[5]), s3 = (init ? init[i + 3] : 0.0) + (t[6] + t[7]);
        for (int kk = k; kk < n; ++kk) { const double xv = x[kk]; s0 += m0[kk] * xv; s1 += m1[kk] * xv; s2 += m2[kk] * xv; s3 += m3[kk] * xv; }
        y[i] = s0; y[i + 1] = s1; y[i + 2] = s2; y[i + 3] = s3;
      }
    }
    for (; i + 4 <= rows; i += 4)
    {
      const double* m0 = Mat + (size_t)i * ld; const double* m1 = m0 + ld; const double* m2 = m1 + ld; const double* m3 = m2 + ld;
      __m128d a0 = _mm_setzero_pd(), a1 = a0, b0 = a0, b1 = a0, c0 = a0, c1 = a0, d0 = a0, d1 = a0; int k = 0;
      for (; k + 4 <= n; k += 4)
      {
        const __m128d xa = _mm_loadu_pd(x + k), xb = _mm_loadu_pd(x + k + 2);
        a0 = _mm_add_pd(a0, _mm_mul_pd(_mm_loadu_pd(m0 + k), xa)); a1 = _mm_add_pd(a1, _mm_mul_pd(_mm_loadu_pd(m0 + k + 2), xb));
        b0 = _mm_add_pd(b0, _mm_mul_pd(_mm_loadu_pd(m1 + k), xa)); b1 = _mm_add_pd(b1, _mm_mul_pd(_mm_loadu_pd(m1 + k + 2), xb));
        c0 = _mm_add_pd(c0, _mm_mul_pd(_mm_loadu_pd(m2 + k), xa)); c1 = _mm_add_pd(c1, _mm_mul_pd(_mm_loadu_pd(m2 + k + 2), xb));
        d0 = _mm_add_pd(d0, _mm_mul_pd(_mm_loadu_pd(m3 + k), xa)); d1 = _mm_add_pd(d1, _mm_mul_pd(_mm_loadu_pd(m3 + k + 2), xb));
      }
      a0 = _mm_add_pd(a0, a1); b0 = _mm_add_pd(b0, b1); c0 = _mm_add_pd(c0, c1); d0 = _mm_add_pd(d0, d1); double t[8]; _mm_storeu_pd(t, a0); _mm_storeu_pd(t + 2, b0); _mm_storeu_pd(t + 4, c0); _mm_storeu_pd(t + 6, d0);
      double s0 = (init ? init[i] : 0.0) + (t[0] + t[1]), s1 = (init ? init[i + 1] : 0.0) + (t[2] + t[3]), s2 = (init ? init[i + 2] : 0.0) + (t[4] + t[5]), s3 = (init ? init[i + 3] : 0.0) + (t[6] + t[7]);
      for (int kk = k; kk < n; ++kk) { const double xv = x[kk]; s0 += m0[kk] * xv; s1 += m1[kk] * xv; s2 += m2[kk] * xv; s3 += m3[kk] * xv; }
      y[i] = s0; y[i + 1] = s1; y[i + 2] = s2; y[i + 3] = s3;
    }
    for (; i < rows; ++i) y[i] = DotF(Mat + (size_t)i * ld, x, n, init ? init[i] : 0.0);
  }
  static inline void Axpy(double* __restrict a, const double* __restrict b, double f, int n)
  {
    const __m128d vf = _mm_set1_pd(f); int j = 0;
    if (g_ff2Avx) { const __m256d wf = _mm256_set1_pd(f); for (; j + 4 <= n; j += 4) _mm256_storeu_pd(a + j, _mm256_sub_pd(_mm256_loadu_pd(a + j), _mm256_mul_pd(wf, _mm256_loadu_pd(b + j)))); _mm256_zeroupper(); }
    for (; j + 2 <= n; j += 2) _mm_storeu_pd(a + j, _mm_sub_pd(_mm_loadu_pd(a + j), _mm_mul_pd(vf, _mm_loadu_pd(b + j))));
    if (j < n) a[j] -= f * b[j];
  }
  // a[j] = (a[j] - f0 b0[j]) - f1 b1[j]
  static inline void Axpy2(double* __restrict a, const double* __restrict b0, const double* __restrict b1, double f0, double f1, int n)
  {
    const __m128d v0 = _mm_set1_pd(f0), v1 = _mm_set1_pd(f1); int j = 0;
    if (g_ff2Avx) { const __m256d w0 = _mm256_set1_pd(f0), w1 = _mm256_set1_pd(f1); for (; j + 4 <= n; j += 4) _mm256_storeu_pd(a + j, _mm256_sub_pd(_mm256_sub_pd(_mm256_loadu_pd(a + j), _mm256_mul_pd(w0, _mm256_loadu_pd(b0 + j))), _mm256_mul_pd(w1, _mm256_loadu_pd(b1 + j)))); _mm256_zeroupper(); }
    for (; j + 2 <= n; j += 2) _mm_storeu_pd(a + j, _mm_sub_pd(_mm_sub_pd(_mm_loadu_pd(a + j), _mm_mul_pd(v0, _mm_loadu_pd(b0 + j))), _mm_mul_pd(v1, _mm_loadu_pd(b1 + j))));
    if (j < n) a[j] = (a[j] - f0 * b0[j]) - f1 * b1[j];
  }
  // in-place LU of the active block with row pivots (piv), two pivot columns per sweep; every element sees the same subtractions in the same order as the plain right-looking loop
  void FactorActive()
  {
    const int n = nAct; int c = 0;
    for (; c + 1 < n; c += 2)
    {
      int pr = c; double mx = std::fabs(LU[c][c]); for (int i = c + 1; i < n; ++i) if (std::fabs(LU[i][c]) > mx) { mx = std::fabs(LU[i][c]); pr = i; }
      piv[c] = pr; if (pr != c) for (int j = 0; j < n; ++j) std::swap(LU[c][j], LU[pr][j]);
      const double inv0 = 1.0 / LU[c][c], u01 = LU[c][c + 1];
      for (int i = c + 1; i < n; ++i) { const double f = LU[i][c] * inv0; LU[i][c] = f; if (f != 0.0) LU[i][c + 1] -= f * u01; }
      pr = c + 1; mx = std::fabs(LU[c + 1][c + 1]); for (int i = c + 2; i < n; ++i) if (std::fabs(LU[i][c + 1]) > mx) { mx = std::fabs(LU[i][c + 1]); pr = i; }
      piv[c + 1] = pr; if (pr != c + 1) for (int j = 0; j < n; ++j) std::swap(LU[c + 1][j], LU[pr][j]);
      const double inv1 = 1.0 / LU[c + 1][c + 1];
      { const double f = LU[c + 1][c]; if (f != 0.0) Axpy(&LU[c + 1][c + 2], &LU[c][c + 2], f, n - c - 2); }
      const double* b0 = &LU[c][c + 2]; const double* b1 = &LU[c + 1][c + 2];
      for (int i = c + 2; i < n; ++i)
      {
        const double f1 = LU[i][c + 1] * inv1; LU[i][c + 1] = f1; const double f0 = LU[i][c];
        if (f0 == 0.0 && f1 == 0.0) continue;
        Axpy2(&LU[i][c + 2], b0, b1, f0, f1, n - c - 2);
      }
    }
    if (c < n)
    {
      int pr = c; double mx = std::fabs(LU[c][c]); for (int i = c + 1; i < n; ++i) if (std::fabs(LU[i][c]) > mx) { mx = std::fabs(LU[i][c]); pr = i; }
      piv[c] = pr; if (pr != c) for (int j = 0; j < n; ++j) std::swap(LU[c][j], LU[pr][j]);
    }
  }
  double JacobiDrift(double (*jd)[6]) const
  {
    double m = 0.0; const Prop& P = props[luProp];
    for (int d = 0; d < ND; ++d)
    {
      const int cc = M::kDevCur[d], ty = M::kDevType[d], npt = nPortsOf(ty), ncu = nCursOf(ty);
      for (int c2 = 0; c2 < ncu; ++c2) for (int b = 0; b < npt; ++b)
      {
        const double z = P.Zmax[cc + c2], g0 = jd[d][c2 * npt + b], g1 = jdFac[d][c2 * npt + b]; if (z == 0.0) continue;
        const double e = z * std::fabs(g0 - g1) / (1.0 + z * std::fabs(g1)); if (e > m) m = e;
      }
    }
    return m;
  }
  // frozen-port test: true when the slope of the device at port a, weighted like the active-set test, now exceeds frzMul x asTol (the neglected Jacobian entry is no longer negligible)
  bool FrozenSlopeExceeds(int a, const double* q, const Prop& P) const
  {
    int d = -1, b = 0; for (int e = 0; e < ND; ++e) { const int pp = M::kDevPort[e]; if (a >= pp && a < pp + nPortsOf(M::kDevType[e])) { d = e; b = a - pp; break; } }
    if (d < 0) return true;
    const int pp = M::kDevPort[d], cc = M::kDevCur[d], idx = M::kDevIdx[d], ty = M::kDevType[d], npt = nPortsOf(ty), ncu = nCursOf(ty); double jd[6] = {0, 0, 0, 0, 0, 0};
    switch (ty)
    {
    case 0: { const BjtOut o = BjtEvalG(bp[idx], q[pp], q[pp + 1]); jd[0] = o.d[0][0]; jd[1] = o.d[0][1]; jd[2] = o.d[1][0]; jd[3] = o.d[1][1]; break; }
    case 1: { double i, g; DiodeEvalG(&M::kDio[5 * idx], q[pp], i, g); jd[0] = g; break; }
    case 6: { const double* r = &M::kCap[10 * idx]; double q1, c1; CapQ(r, q[pp], q1, c1); jd[0] = r[1] * c1 / capH; break; }
    default: return true;
    }
    for (int c2 = 0; c2 < ncu; ++c2) if (P.Zmax[cc + c2] * std::fabs(jd[c2 * npt + b]) > frzMul * asTol) return true;
    return false;
  }
  // self-test of the Jacobian blocks against central differences at the port values q (returns the largest relative error per device type in err[6])
  void JacCheck(const double* q, double* err) const
  {
    for (int t = 0; t < 6; ++t) err[t] = 0.0;
    double f[NF], jd[ND][6], fp[NF], fm[NF], qq[NP]; EvalF(q, f, jd);
    for (int d = 0; d < ND; ++d)
    {
      if (M::kDevType[d] > 5) continue;
      const int pp = M::kDevPort[d], cc = M::kDevCur[d], ty = M::kDevType[d], npt = nPortsOf(ty), ncu = nCursOf(ty);
      for (int b = 0; b < npt; ++b)
      {
        for (int i = 0; i < NP; ++i) qq[i] = q[i];
        const double dq = 1e-6 * (1.0 + std::fabs(q[pp + b])); qq[pp + b] = q[pp + b] + dq; EvalF(qq, fp, nullptr); qq[pp + b] = q[pp + b] - dq; EvalF(qq, fm, nullptr);
        for (int c2 = 0; c2 < ncu; ++c2)
        {
          const double fd = (fp[cc + c2] - fm[cc + c2]) / (2.0 * dq), an = jd[d][c2 * npt + b]; const double e = std::fabs(fd - an) / (std::fabs(fd) + std::fabs(an) + 1e-9); if (e > err[ty]) err[ty] = e;
        }
      }
    }
  }
  // estimate of the output error of a step with propagator pi caused by the curvature of the device currents: df = f(end) - linear extrapolation from the two previous steps
  double OutErr(int pi, const double* df) const { const Prop& P = props[pi]; double v = 0.0; for (int k = 0; k < NF; ++k) v += P.wOut[k] * df[k]; return v; }
  // largest change of a node voltage (S node) that the current deviation df of the step with propagator pi causes
  double NodeErrArg(int pi, const double* df, int& arg) const { const Prop& P = props[pi]; double m = 0.0; arg = 0; for (int s = 0; s < NS; ++s) { double v = 0.0; const double* g = &P.XG[s * NF]; for (int k = 0; k < NF; ++k) v += g[k] * df[k]; if (std::fabs(v) > m) { m = std::fabs(v); arg = s; } } return m; }
  double NodeErr(int pi, const double* df) const { const Prop& P = props[pi]; double m = 0.0; for (int s = 0; s < NS; ++s) { double v = 0.0; const double* g = &P.XG[s * NF]; for (int k = 0; k < NF; ++k) v += g[k] * df[k]; m = std::fmax(m, std::fabs(v)); } return m; }
  double Output(const double* f, const double* x1, const double* dx) const
  {
    double y = M::kDo1 + M::kDou[0] * acc; for (int t = 0; t < NT; ++t) y += M::kDou[1 + 2 * t] * x1[t] + M::kDou[2 + 2 * t] * dx[t];
    for (int i = 0; i < R; ++i) y += M::kXo[i] * z[i];
    for (int k = 0; k < NF; ++k) y += M::kDof[k] * f[k];
    return y;
  }
  // one step of propagator pi; x0 / x1: values of the NT time-dependent sources at the start / end of the step
  double Step(int pi, const double* x0, const double* x1, int depth = 0)
  {
    const double aL = accLast, aN = acc; double zs[R], fs[NF], ps[NP]; for (int i = 0; i < R; ++i) zs[i] = z[i]; for (int i = 0; i < NF; ++i) fs[i] = f0[i]; for (int i = 0; i < NP; ++i) ps[i] = p[i];
    bool ok = true; double y = StepOnce(pi, x0, x1, ok);
    if (ok) return y;
    if (activeSet && !noFallback)   // the reduced Newton matrix may have been the reason: the same step with the full matrix first
    {
      for (int i = 0; i < R; ++i) z[i] = zs[i]; for (int i = 0; i < NF; ++i) f0[i] = fs[i]; for (int i = 0; i < NP; ++i) p[i] = ps[i]; luOk = false; --steps; accLast = aL; ++asFallbacks;
      activeSet = false; ok = true; y = StepOnce(pi, x0, x1, ok); activeSet = true; luOk = false;
      if (ok) return y;
    }
    int ph = pi < (int)halfIdx.size() ? halfIdx[pi] : -1;
    if (ph < 0 && depth < maxDepth) { ph = Prepare(0.5 * props[pi].h); SetHalf(pi, ph); }   // a half-step propagator that was not set up is made on demand
    if (depth >= maxDepth || ph < 0) { ++fails; return y; }
    for (int i = 0; i < R; ++i) z[i] = zs[i]; for (int i = 0; i < NF; ++i) f0[i] = fs[i]; for (int i = 0; i < NP; ++i) p[i] = ps[i]; luOk = false; --steps; ++retries; accLast = aL;
    double xm[NT > 0 ? NT : 1]; for (int t = 0; t < NT; ++t) xm[t] = 0.5 * (x0[t] + x1[t]);
    acc = 0.5 * (aL + aN); Step(ph, x0, xm, depth + 1); acc = aN; return Step(ph, xm, x1, depth + 1);
  }
  double StepOnce(int pi, const double* x0, const double* x1, bool& ok)
  {
    const Prop& P = props[pi]; const double h = P.h; double zk[R], pk[NP], dx[NT > 0 ? NT : 1]; lastPi = pi;
    capH = h; for (int i = 0; i < NP; ++i) ps0[i] = p[i]; ++q0Gen;
    if (predW > 0.0 && hPrevStep > 0.0) { const double r = std::fmin(h / hPrevStep, 2.0) * predW; for (int i = 0; i < NP; ++i) p[i] += r * (ps0[i] - pStartPrev[i]); }
    unsigned long long tp0 = 0, tp1 = 0; PROF_T(tp0);
    for (int t = 0; t < NT; ++t) dx[t] = (x1[t] - x0[t]) / h;
    for (int i = 0; i < R; ++i)
    {
      double a = P.ConeC[i] + P.Ga[i * NU] * accLast + P.Gb[i * NU] * acc;
      for (int t = 0; t < NT; ++t) a += P.Ga[i * NU + 1 + 2 * t] * x0[t] + P.Gb[i * NU + 1 + 2 * t] * x1[t] + (P.Ga[i * NU + 2 + 2 * t] + P.Gb[i * NU + 2 + 2 * t]) * dx[t];
      if (fastDot && matVec) { zk[i] = a; continue; }
      const double* ph = &P.Phi[i * R]; a = Dot(ph, z, R, a);
      const double* gf = &P.GaF[i * NF]; a = Dot(gf, f0, NF, a);
      zk[i] = a;
    }
    if (fastDot && matVec) { MatVec(P.Phi.data(), R, R, z, R, zk, zk); MatVec(P.GaF.data(), NF, R, f0, NF, zk, zk); }
    for (int a = 0; a < NP; ++a)
    {
      double v = PC[a] + PD[a * NU] * acc; for (int t = 0; t < NT; ++t) v += PD[a * NU + 1 + 2 * t] * x1[t] + PD[a * NU + 2 + 2 * t] * dx[t];
      if (fastDot && matVec) { pk[a] = v; continue; }
      const double* px = &P.PXz[a * R]; v = Dot(px, zk, R, v);
      pk[a] = v;
    }
    if (fastDot && matVec) MatVec(P.PXz.data(), R, NP, zk, R, pk, pk);
    double f1[NF] = {0}, jd[ND][6] = {{0}}, F[NP] = {0}; bool fresh = false;
    unsigned long long tq = 0; PROF_ADD(prof[5], tp0); PROF_T(tq);
    bool elimActive = false; const QE* qe = nullptr; double qc_[NP], qt_[NP], qe_[NP]; const double* zbase = P.Zp.data(); const double* ztb = P.ZpT.data();
    if (qleOn && qlOn && activeSet)
    {
      if (qSetVer != qVer) QleSetup(); if (qeC.size() < props.size()) qeC.resize(props.size());
      if (nQ > 0) { QE& e = qeC[pi]; if (e.pi != pi || e.ver != qVer) QleBuild(P, e, pi); if (!e.bad) { elimActive = true; qe = &e; } }
    }
    if (elimActive)
    {
      ++qleSteps; qleNsum += nQ; const int n = nQ; double s_[NP];
      for (int i = 0; i < n; ++i) { const int d = qDev[i]; qc_[i] = qlI[d] - qlG[d] * qlV[d]; }
      for (int i = 0; i < n; ++i) { double v = pk[qPort[i]]; for (int j = 0; j < n; ++j) v += qe->Zqq[i * n + j] * qc_[j]; s_[i] = v; }
      for (int i = 0; i < n; ++i) { double v = 0.0; for (int j = 0; j < n; ++j) v += qe->Minv[i * n + j] * s_[j]; qt_[i] = v; qe_[i] = qc_[i] + qlG[qDev[i]] * v; }
      for (int a = 0; a < NP; ++a) { if (isQ[a]) continue; double v = 0.0; const double* zq = &qe->ZpQ[a * n]; for (int i = 0; i < n; ++i) v += zq[i] * qe_[i]; pk[a] += v; }
      for (int k = 0; k < NF; ++k) f1[k] = 0.0;
      zbase = qe->Ze.data(); ztb = qe->ZeT.data(); skipDev = elim.data();
    }
    PROF_ADD(prof[6], tq);
    bool refac = !luOk || (luProp != pi && !keepAcrossH) || noChord || luQVer != (elimActive ? qVer : 0u); const bool fromPrev = !refac; if (refac) { if (!luOk) ++why[0]; else if (luProp != pi && !keepAcrossH) ++why[1]; } double prevMx = 1e30, mx2 = 1e30, damp = 1.0; int itDone = 0;
    for (int it = 0; it < maxIt; ++it)
    {
      if (it == 0) PROF_ADD(prof[0], tp0);
      PROF_T(tp1); itDone = it + 1; EvalF(p, f1, (refac || frzMode == 2) ? jd : nullptr); ++its; PROF_ADD(prof[1], tp1);
      if (frzMode == 2 && !refac && luOk && JacobiDrift(jd) > jacTol) { refac = true; ++why[4]; }   // Jacobian error criterion: the relative change of the device slopes since the factorisation (chord contraction of the weakest device)
      PROF_T(tp1);
      if (fastDot && matVec) { double zfv[NP]; MatVec(zbase, NF, NP, f1, NF, zfv); for (int a = 0; a < NP; ++a) { if (elimActive && isQ[a]) { F[a] = 0.0; continue; } F[a] = pk[a] + zfv[a] - p[a]; } }
      else
      for (int a = 0; a < NP; ++a) { if (elimActive && isQ[a]) { F[a] = 0.0; continue; } const double* zp = &zbase[a * NF]; const double zf = Dot(zp, f1, NF); F[a] = pk[a] + zf - p[a]; }
      PROF_ADD(prof[13], tp1); PROF_T(tp1);
      if (refac)
      {
        {   // active set: ports whose devices change the Newton matrix noticeably (|Zp jd| > asTol somewhere); the others (reverse-biased junctions, switched-off transistors, ports that depend on the inputs only) are updated explicitly: dp_F = F_F - J_FA dp_A
          bool act[NP]; for (int a = 0; a < NP; ++a) act[a] = !activeSet;
          if (activeSet) for (int d = 0; d < ND; ++d)
          {
            if (elimActive && elim[d]) continue;
            const int pp = M::kDevPort[d], cc = M::kDevCur[d], ty = M::kDevType[d], npt = nPortsOf(ty), ncu = nCursOf(ty);
            for (int b = 0; b < npt; ++b)
            {
              if (portPure[pp + b]) continue;
              if (ty >= 2 && ty != 6) { act[pp + b] = true; if (actStat) { bool big = false; for (int c2 = 0; c2 < ncu; ++c2) if (P.Zmax[cc + c2] * std::fabs(jd[d][c2 * npt + b]) > asTol) big = true; if (!big) ++wouldFreeze[actWin][ty]; } continue; }
              for (int c2 = 0; c2 < ncu; ++c2) if (P.Zmax[cc + c2] * std::fabs(jd[d][c2 * npt + b]) > asTol) act[pp + b] = true;
            }
          }
          PROF_ADD(prof[10], tp1); PROF_T(tp1);
          nAct = 0; nFrz = 0; for (int a = 0; a < NP; ++a) { if (act[a]) actL[nAct++] = a; else frzL[nFrz++] = a; pRef[a] = p[a]; }
          if (cutOn) std::stable_partition(actL, actL + nAct, [&](int a) { return portGrp[a] == cutFrom; });
          sumAct += nAct; ++nLU; if (actStat) { for (int j_ = 0; j_ < nAct; ++j_) { int d_ = -1; for (int e_ = 0; e_ < ND; ++e_) { const int pp_ = M::kDevPort[e_]; if (actL[j_] >= pp_ && actL[j_] < pp_ + nPortsOf(M::kDevType[e_])) { d_ = e_; break; } } ++actByType[actWin][d_ < 0 ? 7 : M::kDevType[d_]]; } ++actN[actWin]; }
          for (int j = 0; j < nAct; ++j) { double* col = Jc[actL[j]]; for (int a = 0; a < NP; ++a) col[a] = 0.0; col[actL[j]] = 1.0; }
          PROF_ADD(prof[11], tp1); PROF_T(tp1);
          for (int d = 0; d < ND; ++d)
          {
            const int pp = M::kDevPort[d], cc = M::kDevCur[d], ty = M::kDevType[d], npt = nPortsOf(ty), ncu = nCursOf(ty);
            for (int b = 0; b < npt; ++b)
            {
              if (!act[pp + b]) continue; double* col = Jc[pp + b];
              for (int c2 = 0; c2 < ncu; ++c2) { const double g = jd[d][c2 * npt + b]; if (g == 0.0) continue; const double* zt = &ztb[(cc + c2) * NP]; Axpy(col, zt, g, NP); }
              if (cutOn && portGrp[pp + b] == cutFrom) for (int a = 0; a < NP; ++a) if (portGrp[a] == cutTo) col[a] = 0.0;
            }
          }
          PROF_ADD(prof[12], tp1); PROF_T(tp1);
          for (int j = 0; j < nAct; ++j) { const double* col = Jc[actL[j]]; for (int i = 0; i < nAct; ++i) LU[i][j] = col[actL[i]]; for (int f = 0; f < nFrz; ++f) JFA[f][j] = col[frzL[f]]; }
        }
        PROF_ADD(prof[2], tp1); PROF_T(tp1);
#ifdef FULLFAST2_OLDLU
        for (int c = 0; c < nAct; ++c)
        {
          int pr = c; double mx = std::fabs(LU[c][c]); for (int i = c + 1; i < nAct; ++i) if (std::fabs(LU[i][c]) > mx) { mx = std::fabs(LU[i][c]); pr = i; }
          piv[c] = pr; if (pr != c) for (int j = 0; j < nAct; ++j) std::swap(LU[c][j], LU[pr][j]);
          const double inv = 1.0 / LU[c][c];
          for (int i = c + 1; i < nAct; ++i) { const double fct = LU[i][c] * inv; LU[i][c] = fct; if (fct == 0.0) continue; for (int j = c + 1; j < nAct; ++j) LU[i][j] -= fct * LU[c][j]; }
        }
#else
        FactorActive();
#endif
        PROF_ADD(prof[3], tp1);
        luOk = true; luProp = pi; luQVer = elimActive ? qVer : 0u; refac = false; fresh = true; ++facs; if (frzMode == 2) std::memcpy(jdFac, jd, sizeof(jdFac));
      }
      else fresh = false;
      PROF_T(tp1);
      double Fpre[NP]; for (int a = 0; a < NP; ++a) Fpre[a] = F[a];
      {
        double Fa[NP], Ff[NP]; for (int i = 0; i < nAct; ++i) Fa[i] = F[actL[i]]; for (int f = 0; f < nFrz; ++f) Ff[f] = F[frzL[f]];
        for (int c = 0; c < nAct; ++c) if (piv[c] != c) std::swap(Fa[c], Fa[piv[c]]);
        if (fastSolve)
        {
          for (int i = 1; i < nAct; ++i) Fa[i] -= DotF(LU[i], Fa, i, 0.0);
          for (int i = nAct - 1; i >= 0; --i) Fa[i] = (Fa[i] - DotF(&LU[i][i + 1], &Fa[i + 1], nAct - i - 1, 0.0)) / LU[i][i];
          if (matVec) { double tf[NP]; MatVec(&JFA[0][0], NP, nFrz, Fa, nAct, tf); for (int f = 0; f < nFrz; ++f) Ff[f] -= tf[f]; }
          else for (int f = 0; f < nFrz; ++f) Ff[f] -= DotF(JFA[f], Fa, nAct, 0.0);
        }
        else
        {
        for (int c = 0; c < nAct; ++c) for (int i = c + 1; i < nAct; ++i) Fa[i] -= LU[i][c] * Fa[c];
        for (int i = nAct - 1; i >= 0; --i) { double sum = Fa[i]; for (int l = i + 1; l < nAct; ++l) sum -= LU[i][l] * Fa[l]; Fa[i] = sum / LU[i][i]; }
        for (int f = 0; f < nFrz; ++f) { double sum = Ff[f]; const double* jr = JFA[f]; for (int i = 0; i < nAct; ++i) sum -= jr[i] * Fa[i]; Ff[f] = sum; }
        }
        for (int i = 0; i < nAct; ++i) F[actL[i]] = Fa[i]; for (int f = 0; f < nFrz; ++f) F[frzL[f]] = Ff[f];
      }
      PROF_ADD(prof[4], tp1);
      double mx = 0.0; int amx = 0; for (int a = 0; a < NP; ++a) if (std::fabs(F[a]) > mx) { mx = std::fabs(F[a]); amx = a; } lastAmx = amx;
#ifdef FULLFAST2_ITERLOG
      if (traceOn) std::printf("  it %d mx %.3e port %d (p %.4g) fresh %d h %.2e\n", it, mx, amx, p[amx], (int)fresh, h);
#endif
      if (abortIt > 0 && it >= abortIt && mx >= newtonTol) { itDone = maxIt; break; }   // no convergence after abortIt iterations: treated as a Newton failure (retry with the half step)
      double mxA = mx; if (chordAct && nFrz > 0) { mxA = 0.0; for (int i = 0; i < nAct; ++i) mxA = std::fmax(mxA, std::fabs(F[actL[i]])); }
      if (!fresh && mx >= newtonTol && fromPrev && it == 0 && mxA > chordFirst) { refac = true; ++why[2]; ++refGrp[2][portGrp[amx] + 1]; continue; }
      if (!fresh && mx >= newtonTol && it >= 1 && mx > 0.5 * prevMx) { refac = true; ++why[3]; ++refGrp[3][portGrp[amx] + 1]; continue; }
      if (mx < newtonTol) break;
      double mxv = 0.0; for (int a = 0; a < NP; ++a) mxv = std::fmax(mxv, std::fabs(F[a]) * portScale[a]);
      double sc = mxv > kCap ? kCap / mxv : 1.0;
      if (dampOn)
      {   // no progress over two iterations (a two-cycle of the saturating gates): halve the step; relax again when the correction shrinks
        if (it >= 3 && std::fabs(mx - mx2) < dampRatio * mx && mx > dampMin) damp = std::fmax(damp * 0.5, 1.0 / 64.0); else if (it >= 2 && mx < 0.5 * prevMx && prevMx < 0.5 * mx2) damp = std::fmin(damp * 2.0, 1.0);
        mx2 = prevMx;
      }
      for (int a = 0; a < NP; ++a)
      {
        double vnew = p[a] + sc * F[a]; if (std::fabs(vnew - p[a]) > portCap[a]) vnew = p[a] + (vnew > p[a] ? portCap[a] : -portCap[a]);
        if (dampOn && damp < 1.0) vnew = p[a] + damp * (vnew - p[a]);
        if (portVt[a] > 0.0 && vnew > portVcrit[a] && std::fabs(vnew - p[a]) > 2.0 * portVt[a])
        {
          if (p[a] > 0.0) { const double arg = 1.0 + (vnew - p[a]) / portVt[a]; vnew = arg > 0.0 ? p[a] + portVt[a] * std::log(arg) : portVcrit[a]; }
          else vnew = portVt[a] * std::log(vnew / portVt[a]);
        }
        p[a] = vnew;
      }
      if (noChord) refac = true;
      if (nFrz > 0 && !refac && frzMode != 2) for (int f = 0; f < nFrz; ++f)
      {
        const int a = frzL[f]; if (elimActive && isQ[a]) continue;
        if (std::fabs(p[a] - pRef[a]) > (portVt[a] > 0.0 ? frzThr * portVt[a] : 1.0))
        {
          if (frzMode == 0 || FrozenSlopeExceeds(a, p, props[pi])) { refac = true; ++why[4]; ++refGrp[4][portGrp[a] + 1]; break; }
          pRef[a] = p[a];
        }
      }   // a frozen port left the region in which its device had no noticeable slope
      prevMx = mx;
    }
    if (itDone >= maxIt)
    {
      double mm = 0; for (int a = 0; a < NP; ++a) mm = std::fmax(mm, std::fabs(F[a])); lastMx = mm; ok = false;
#ifdef FULLFAST2_FAILLOG
      if (fails < 40) { int am = 0; for (int a = 1; a < NP; ++a) if (std::fabs(F[a]) > std::fabs(F[am])) am = a; int dty = -1, dix = -1; for (int d = 0; d < ND; ++d) { const int pp = M::kDevPort[d]; if (am >= pp && am < pp + nPortsOf(M::kDevType[d])) { dty = M::kDevType[d]; dix = d; } }
        std::printf("Newton failure (step %ld): step h %.3e, last correction %.3e at port %d (device %d type %d, p %.5g)\n", steps, P.h, mm, am, dix, dty, p[am]); }
#endif
      if (mm < kSoftTol)
      {
        double p2[NP], f2[NF], df[NF]; for (int a = 0; a < NP; ++a) p2[a] = p[a] + F[a]; for (int k = 0; k < NF; ++k) f2[k] = 0.0; EvalF(p2, f2, nullptr); for (int k = 0; k < NF; ++k) df[k] = f2[k] - f1[k];
        double zerr = 0.0, yerr = 0.0; for (int i = 0; i < R; ++i) { double v = 0.0; for (int k = 0; k < NF; ++k) v += P.GbF[i * NF + k] * df[k]; zerr = std::fmax(zerr, std::fabs(v)); yerr += M::kXo[i] * v; }
        for (int k = 0; k < NF; ++k) yerr += M::kDof[k] * df[k];
        double rres = 0.0; for (int a = 0; a < NP; ++a) { if (elimActive && isQ[a]) continue; const double* zp = &zbase[a * NF]; double zf = 0.0; for (int k = 0; k < NF; ++k) zf += zp[k] * f2[k]; rres = std::fmax(rres, std::fabs(pk[a] + zf - p2[a])); }
        if (std::fabs(yerr) < kSoftY && zerr < kSoftZ && rres < kSoftRes) { for (int a = 0; a < NP; ++a) p[a] = p2[a]; for (int k = 0; k < NF; ++k) f1[k] = f2[k]; ok = true; ++softAccepts; }
      }
    }
    PROF_T(tq);
    if (elimActive)
    {
      const int n = nQ; skipDev = nullptr;
      for (int i = 0; i < n; ++i) { double v = qt_[i]; const double* tm = &qe->Tm[i * NF]; for (int k = 0; k < NF; ++k) v += tm[k] * f1[k]; p[qPort[i]] = v; f1[qCur[i]] = qc_[i] + qlG[qDev[i]] * v; }
    }
    PROF_ADD(prof[7], tq); PROF_T(tq);
    if (qlcOn && ok && !QlcFinish(f1, P)) ok = false;
    PROF_ADD(prof[8], tq); PROF_T(tq);
    if (qlOn && ok && !QlFinish(f1, P)) ok = false;
    PROF_ADD(prof[9], tq);
    if (fastDot && matVec) MatVec(P.GbF.data(), NF, R, f1, NF, z, zk);
    else for (int i = 0; i < R; ++i) { double a = zk[i]; const double* gb = &P.GbF[i * NF]; a = Dot(gb, f1, NF, a); z[i] = a; }
    if (quadW != 0.0 && hPrevStep > 0.0 && fPrevOk) { double kap[NF]; const double i1 = 1.0 / (h + hPrevStep), ih = 1.0 / h, ihp = 1.0 / hPrevStep; for (int k = 0; k < NF; ++k) kap[k] = quadW * (((f1[k] - f0[k]) * ih - (f0[k] - fStartPrev[k]) * ihp) * i1); for (int i = 0; i < R; ++i) { const double* gq = &P.GqF[i * NF]; double a = 0.0; for (int k = 0; k < NF; ++k) a += gq[k] * kap[k]; z[i] += a; } }
    if (quadCW != 0.0 && hPrevStep > 0.0 && fPrevOk) { const double isl = quadCW / (0.5 * (h + hPrevStep)); for (int d = 0; d < ND; ++d) if (M::kDevType[d] == 6) { const int col = M::kDevCur[d]; const double sl = (f1[col] - f0[col]) * isl; for (int i = 0; i < R; ++i) z[i] += P.GcF[i * NF + col] * sl; } }
    for (int k = 0; k < NF; ++k) { fStartPrev[k] = f0[k]; f0[k] = f1[k]; } fPrevOk = true;
    accLast = acc; ++steps; for (int i = 0; i < NP; ++i) pStartPrev[i] = ps0[i]; hPrevStep = h; const double yo = Output(f1, x1, dx); if (!(std::fabs(yo) < 1e6)) ok = false; return yo;
  }
};

}}  // namespace dgg::pcmfast
