// Linear response of a FullFast2 voice to the noise source around the noise-free trajectory (the noise phase of the hit).
//
// The noise-free trajectory (silent source) is integrated by the production solver and its port voltages are recorded at the step ends. The response of the circuit to the noise is the linearisation of the same
// exponential-integrator step around that trajectory, integrated at the noise knots (one step per knot, 5 us):
//   dzk = Phi dz + GaF df0 + noise input terms,   dpk = PXz dzk + noise input terms of the ports,
//   (I - Zp G) dp = dpk + Zp dfk,                 df1 = G dp + dfk,       dz = dzk + GbF df1,      dy = Xo dz + Dof df1 + noise input terms of the output,
// G = Jacobian of the device currents at the noise-free port voltages, dfk = start-of-step part of the charge-device currents (-G dp of the previous knot). G is re-evaluated when a port voltage of the noise-free trajectory
// has moved by a fraction of its thermal voltage since the last evaluation; the Newton-like solve uses the active ports only (frozen ports are updated explicitly) and one step of iterative refinement with the full G.
#pragma once
#include <cmath>
#include <vector>
#include <algorithm>
#include <thread>
#include <random>
#include <cstdio>
#include <cstdlib>
#include "FullFast2.h"

#ifdef DGG_LIVE_PROF
#include <intrin.h>
namespace dgg { namespace pcmfast { inline unsigned long long g_prof[8] = {0}; }}
#define LIVE_PROF_T(i, expr) do { const unsigned long long t0_ = __rdtsc(); expr; ::dgg::pcmfast::g_prof[i] += __rdtsc() - t0_; } while (0)
#else
#define LIVE_PROF_T(i, expr) expr
#endif
namespace dgg { namespace pcmfast {

template <class Mdl>
class NoiseReplay
{
public:
  using Model = FullFast2<Mdl>;
  static constexpr int R = Mdl::kR, NF = Mdl::kNF, NP = Mdl::kNP, NU = Mdl::kU, ND = Mdl::kNumDev;

  // noise-free trajectory: time and port voltages at the end of every step; p0 = ports at t = 0
  std::vector<double> tE, pE; double p0[NP];
  size_t Epochs() const { return eps_.size(); }
  size_t EpochBytes() const { size_t b = 0; for (const Epoch& e : eps_) b += sizeof(Epoch) + (e.rin.Mt.capacity() + e.rst.Mt.capacity()) * 4 + (e.LU.capacity() + e.JFA.capacity() + e.capG.capacity()) * 4 + e.ent.capacity() * sizeof(EntF); return b; }
  void Clear(const double* ports0) { tE.clear(); pE.clear(); for (int a = 0; a < NP; ++a) p0[a] = ports0[a]; }
  void Record(double t, const double* p) { tE.push_back(t); pE.insert(pE.end(), p, p + NP); }
  bool capPrev = false;   // diagnosis: charge-device lag term with the capacitance slope of the previous knot
  double capLag = 1.0;   // diagnosis: factor on the start-of-step part of the charge-device currents
  bool dbl = false;   // the active-block solve of the epochs in double precision (diagnosis of the float inverse)
  double thr = 0.3, laterMul = 12.0; int coarse = 1; double eq = 0.0; bool refine = false, single = false;   // single: the fixed matrices and the state in float (the response is a small deviation; float rounding is far below its accuracy)   // one step of iterative refinement with the full G (no effect on the result at the tested settings)   // G is re-evaluated when a port of the noise-free trajectory moved by thr x its thermal voltage (thr x 0.17 V without one)

  // Streaming form (no allocation after Prime): Prime() once per model, Start() per hit, Next() per knot; the knot k covers (k h, (k + 1) h]. Run() is the batch form.
  // The Jacobian epochs (refresh points, active sets, factorisations) depend on the noise-free trajectory only: they are built here once (a dry run over the knots up to tEnd) and shared by all hits.
  void Prime(Model& m, int pi, double tEnd)
  {
    m_ = &m; P_ = &m.props[pi]; m.qlOn = false; m.qlcOn = false; m.capThr = 0.0; m.evCache = false; m.skipDev = nullptr;
    Setup(m, *P_); ent_.reserve(ND * 6 + 8); BuildEpochs(m, *P_, tEnd); BuildFloat(m, *P_);
    redOn_ = condTol > 0.0 && !single && !refine && !dbl; condOn_ = !redOn_ && cond > 0 && !single && !refine && !dbl; condTolUsed = 0.0;
    if (redOn_)   // reduced form with a self test: tighter rank tolerance (x 1e-2, three times) when the difference to the exact maps is too large, the exact path when it never passes
    {
      double tol = condTol; bool ok = false;
      for (int attempt = 0; attempt < 6 && !ok; ++attempt) { BuildRed(m, *P_, tol); redErrDb = ValidateRed(tEnd); ok = redErrDb <= valDb; condTolUsed = tol; if (!ok) tol *= 1e-1; }
      redOn_ = ok; if (!ok) condTolUsed = 0.0; else FreeFull();
    }
    else if (condOn_) BuildCond(m, *P_);
  }
  double condTol = 0.0;   // > 0: reduced-order form (BuildRed), relative rank tolerance
  double startT = 0.05, valDb = -70.0, redErrDb = 0.0, condTolUsed = 0.0;   // valDb: the reduced form is accepted when its difference to the exact form is below this (dB re response energy); redErrDb, condTolUsed: result of the self test
  int cond = 0;   // > 0: condensed state-space form (see BuildCond), blocks of cond knots
  // State of one hit (the tables of the epochs and the model are shared and not modified by a hit): several hits can run at the same time on one primed object, each with its own cursor.
  struct Cursor
  {
    HitNoise* nz = nullptr; double tOff = 0.0, nPrev = 0.0; long N = 0, k = 0; size_t ep = 0; const void* cur = nullptr; long refreshes = 0, knots = 0;
    double capGp[NF] = {0}; bool capGpOk = false; double dz[R] = {0}, dp[NP] = {0}, dfPrev[NF] = {0}; float dzF[R] = {0}, dpF[NP] = {0}, dfPrevF[NF] = {0};
    float xr[3 + 200 + 16] = {0}; float sC[R + NF + NP + 8 + 24] = {0}; double yq[24] = {0}; int yqPos = 0, yqN = 0;   // condensed form: state, outputs of the current block
  };
  void Start(HitNoise& nz, double tOff, double tEnd) { Start(cur0_, nz, tOff, tEnd); }
  bool Next(double& tOut, double& dyOut) { return Next(cur0_, tOut, dyOut); }
  const double* DpNow() const { return cur0_.dp; }   // diagnosis: port response of the last knot (exact path)
  long RefreshCount() const { return cur0_.refreshes; } long KnotCount() const { return cur0_.knots; }
  void Start(Cursor& C, HitNoise& nz, double tOff, double tEnd)
  {
    C.nz = &nz; C.tOff = tOff; C.N = std::min(static_cast<long>(tEnd / P_->h), epochKnots_); C.k = 0; C.ep = 0; C.cur = nullptr;
    for (int i = 0; i < R; ++i) C.dz[i] = 0.0; for (int a = 0; a < NP; ++a) C.dp[a] = 0.0; for (int j = 0; j < NF; ++j) C.dfPrev[j] = 0.0;
    for (float& x : C.sC) x = 0.0f; for (float& x : C.xr) x = 0.0f; C.yqPos = C.yqN = 0; C.xr[1] = static_cast<float>((coarse > 1 ? nz.AtCoarse(tOff, coarse, eq) : nz.At(tOff)) - 7.5);
    C.nPrev = (coarse > 1 ? nz.AtCoarse(tOff, coarse, eq) : nz.At(tOff)) - 7.5; for (int i = 0; i < R; ++i) C.dzF[i] = 0.0f; for (int a = 0; a < NP; ++a) C.dpF[a] = 0.0f; for (int j = 0; j < NF; ++j) C.dfPrevF[j] = 0.0f;
  }
  bool Next(Cursor& C, double& tOut, double& dyOut)
  {
    if (C.k >= C.N) return false;
    if (redOn_) return NextR(C, tOut, dyOut);
    if (condOn_) return NextC(C, tOut, dyOut);
    if (single) return NextF(C, tOut, dyOut);
    Model& m = *m_; const typename Model::Prop& P = *P_; const double h = P.h; const long k = C.k;
    double dzk[R], df0c[NF], dfkc[NF], rhs[NP], dpk[NP], res[NP], dfG[NF], df1[NF], vin[R], vpd[NP], zt[NP];
    const double d0 = C.nPrev, d1 = (coarse > 1 ? C.nz->AtCoarse(C.tOff + (k + 1) * h, coarse, eq) : C.nz->At(C.tOff + (k + 1) * h)) - 7.5, dd = (d1 - d0) / h; C.nPrev = d1;
    if (C.ep < eps_.size() && eps_[C.ep].start == k) { C.cur = &eps_[C.ep]; ++C.ep; ++C.refreshes; }
    const Epoch& E = *static_cast<const Epoch*>(C.cur);
    for (int i = 0; i < R; ++i) vin[i] = P.Ga[i * NU + 1] * d0 + P.Gb[i * NU + 1] * d1 + (P.Ga[i * NU + 2] + P.Gb[i * NU + 2]) * dd;
    for (int a = 0; a < NP; ++a) vpd[a] = m.PD[a * NU + 1] * d1 + m.PD[a * NU + 2] * dd;
    for (int j = 0; j < nGc_; ++j) df0c[j] = C.dfPrev[gcCols_[j]];
    Model::MatVec(P.Phi.data(), R, R, C.dz, R, dzk, vin);
    Model::MatVec(gaF_.data(), nGc_, R, df0c, nGc_, dzk, dzk);
    Model::MatVec(P.PXz.data(), R, NP, dzk, R, dpk, vpd);
    for (int j = 0; j < nCap_; ++j) dfkc[j] = -capLag * ((capPrev && C.capGpOk) ? C.capGp[j] : E.capG[j]) * C.dp[capPort_[j]];   // start-of-step part of the charge-device currents (capPrev: with the slope of the previous knot, Q(p1) - Q(p0) linearised at both ends)
    for (int j = 0; j < nCap_; ++j) C.capGp[j] = E.capG[j]; C.capGpOk = true;
    Model::MatVec(wCap_.data(), nCap_, NP, dfkc, nCap_, rhs, dpk);
    Solve(E, rhs, C.dp);
    if (refine)
    {
      ApplyG(E, C.dp, dfG);   // full G incl. the frozen ports
      Model::MatVec(P.Zp.data(), NF, NP, dfG, NF, res, rhs);
      for (int a = 0; a < NP; ++a) res[a] -= C.dp[a];
      Solve(E, res, zt); for (int a = 0; a < NP; ++a) C.dp[a] += zt[a];
    }
    ApplyG(E, C.dp, df1); for (int c = 0; c < nCap_; ++c) df1[capCur_[c]] += dfkc[c];
    Model::MatVec(P.GbF.data(), NF, R, df1, NF, C.dz, dzk);
    double dy = m.pm.Dou[1] * d1 + m.pm.Dou[2] * dd + Model::DotF(m.pm.Xo, C.dz, R, 0.0); dy = Model::DotF(m.pm.Dof, df1, NF, dy);
    for (int j = 0; j < NF; ++j) C.dfPrev[j] = df1[j];
    tOut = (k + 1) * h; dyOut = dy; ++C.k; ++C.knots; return true;
  }
  template <class Emit>
  void Run(Model& m, int pi, HitNoise& nz, double tOff, double tEnd, Emit&& emit)
  {
    Prime(m, pi, tEnd); Start(nz, tOff, tEnd); double t, dy; while (Next(t, dy)) emit(t, dy);
  }

  // ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
  // Stepwise mode: the response follows the ACTUAL STEPS of the recorded base run (tE / pE: step ends, one record per step; the base run of the noisy expansion point has steps of 2^j x h0, never spanning a noise knot).
  // Per step s (h = tE[s] - tE[s-1], propagators props[pj[j]], ports p0 = pE[s-1] -> p1 = pE[s]):  dzk = Phi dz + GaF df0 + input,  dpk = PXz dzk + input,  dfk = -C(p0)/h dp_prev (charge devices),
  // (I - Zp G(p1)) dp = dpk + Zp dfk,  df1 = G dp + dfk,  dz1 = dzk + GbF df1.  This is the linearisation of the model's own step (the 5 us knot map of Next() is a different discretisation of the same system).
  // pj[0..5]: indices of the propagators of the steps 2^j x h0 (j = 0..5, h0 = 5 us / 32), as the runner's pj_.
  double stepThr = -1.0;   // >= 0: a new epoch only when a port moved by stepThr x its thermal voltage since the last epoch (same j); < 0: an epoch for every step (exact)
  long StepCount() const { return stepsN_; }
  void PrimeSteps(Model& m, const int* pj, double tEnd)
  {
    m_ = &m; P_ = &m.props[pj[5]]; m.qlOn = false; m.qlcOn = false; m.capThr = 0.0; m.evCache = false; m.skipDev = nullptr;
    for (int j = 0; j < 6; ++j) pjIdx_[j] = pj[j]; h0_ = P_->h / 32.0;
    Setup(m, *P_); ent_.reserve(ND * 6 + 8); BuildStepEpochs(m, tEnd);
  }
  void StartSteps(Cursor& C, HitNoise& nz, double tOff, double tEnd)
  {
    Start(C, nz, tOff, tEnd); long n = 0; while (n < static_cast<long>(tE.size()) && tE[static_cast<size_t>(n)] <= tEnd) ++n; C.N = std::min(n, stepsN_);
  }
  void StartSteps(HitNoise& nz, double tOff, double tEnd) { StartSteps(cur0_, nz, tOff, tEnd); }
  bool NextStep(double& tOut, double& dyOut) { return NextStep(cur0_, tOut, dyOut); }
  bool NextStep(Cursor& C, double& tOut, double& dyOut)
  {
    if (C.k >= C.N) return false;
    Model& m = *m_; const long s = C.k; const double tEnd = tE[static_cast<size_t>(s)], tBeg = s > 0 ? tE[static_cast<size_t>(s - 1)] : 0.0;
    if (C.ep < eps_.size() && eps_[C.ep].start == s) { C.cur = &eps_[C.ep]; ++C.ep; ++C.refreshes; }
    const Epoch& E = *static_cast<const Epoch*>(C.cur); const typename Model::Prop& P = m.props[pjIdx_[E.j]]; const double h = tEnd - tBeg;
    double dzk[R], dfkc[NF], rhs[NP], dpk[NP], df1[NF], vin[R], vpd[NP];
    const double d0 = C.nPrev, d1 = (coarse > 1 ? C.nz->AtCoarse(C.tOff + tEnd, coarse, eq) : C.nz->At(C.tOff + tEnd)) - 7.5, dd = (d1 - d0) / h; C.nPrev = d1;
    for (int i = 0; i < R; ++i) vin[i] = P.Ga[i * NU + 1] * d0 + P.Gb[i * NU + 1] * d1 + (P.Ga[i * NU + 2] + P.Gb[i * NU + 2]) * dd;
    for (int a = 0; a < NP; ++a) vpd[a] = m.PD[a * NU + 1] * d1 + m.PD[a * NU + 2] * dd;
    Model::MatVec(P.Phi.data(), R, R, C.dz, R, dzk, vin);
    { const int nc = static_cast<int>(P.gaCols.size()); double fc[NF]; for (int j = 0; j < nc; ++j) fc[j] = C.dfPrev[P.gaCols[static_cast<size_t>(j)]]; if (nc > 0) Model::MatVec(P.GaFc.data(), nc, R, fc, nc, dzk, dzk); }
    Model::MatVec(P.PXz.data(), R, NP, dzk, R, dpk, vpd);
    for (int j = 0; j < nCap_; ++j) dfkc[j] = -capLag * static_cast<double>(E.capGs[static_cast<size_t>(j)]) * C.dp[capPort_[static_cast<size_t>(j)]];
    for (int a = 0; a < NP; ++a) { double sacc = dpk[a]; for (int j = 0; j < nCap_; ++j) sacc += P.Zp[static_cast<size_t>(a) * NF + capCur_[static_cast<size_t>(j)]] * dfkc[j]; rhs[a] = sacc; }
    Solve(E, rhs, C.dp);
    ApplyG(E, C.dp, df1); for (int c = 0; c < nCap_; ++c) df1[capCur_[static_cast<size_t>(c)]] += dfkc[c];
    Model::MatVec(P.GbF.data(), NF, R, df1, NF, C.dz, dzk);
    double dy = m.pm.Dou[1] * d1 + m.pm.Dou[2] * dd + Model::DotF(m.pm.Xo, C.dz, R, 0.0); dy = Model::DotF(m.pm.Dof, df1, NF, dy);
    for (int j = 0; j < NF; ++j) C.dfPrev[j] = df1[j];
    tOut = tEnd; dyOut = dy; ++C.k; ++C.knots; return true;
  }

private:
  int pjIdx_[6] = {0, 0, 0, 0, 0, 0}; double h0_ = 0.0; long stepsN_ = 0;
  struct Ent { int cur, port; double g; };   // device Jacobian entry (current, port, slope)
  struct EntF { int cur, port; float g; };   // the same in the epoch tables (float: the response is a small deviation, the rounding of the tables is far below its accuracy)
  std::vector<Ent> ent_; std::vector<double> gaF_, wCap_, capG_; std::vector<int> gcCols_, capPort_, capCur_, capDev_; int nGc_ = 0, nCap_ = 0; double invThr_[NP];
  struct Blk { int m = 0, rowsPad = 0, cols = 0; std::vector<float> Mt; };   // condensed block of m knots: [s_m; y_1..y_m] = [M W] [s_0; n_0..n_m], stored column-major (cols x rowsPad), the noise values are the last m + 1 columns
  struct RedOp { int rn = 0, rp = 0, rowsPad = 0, cols = 0; std::vector<float> Mt; };   // reduced-order step operator: Mt is (3 + rp) columns x rowsPad: columns n_(k-1), n_k, n_(k+1), z_1..z_rp; rows z_new (rn), y (row rn)
  bool condOn_ = false; int nS_ = 0, nPad_ = 0; std::vector<int> capPU_;
  struct Epoch { Blk bm, b1; RedOp rin, rst; long start = 0; int j = 5; std::vector<float> capGs; int nA = 0, nF = 0; unsigned char act[NP], frz[NP], piv[NP]; std::vector<float> LU, JFA, capG; std::vector<double> LUd, JFAd; std::vector<EntF> ent; };   // LUd / JFAd: double precision inverse of the active block and frozen coupling (dbl)
  std::vector<Epoch> eps_; long epochKnots_ = 0; Cursor cur0_;   // cur0_: the cursor of the single-hit interface
  Model* m_ = nullptr; const typename Model::Prop* P_ = nullptr;
  std::vector<float> phiF_, gaFF_, pxzF_, wCapF_, gbFF_, xoF_, dofF_, ga1F_, gb1F_, ga2F_, gb2F_, pd1F_, pd2F_;
  static void ToF(const std::vector<double>& a, std::vector<float>& b) { b.resize(a.size()); for (size_t i = 0; i < a.size(); ++i) b[i] = static_cast<float>(a[i]); }
  void BuildFloat(Model& m, const typename Model::Prop& P)
  {
    ToF(P.Phi, phiF_); ToF(gaF_, gaFF_); ToF(P.PXz, pxzF_); ToF(wCap_, wCapF_); ToF(P.GbF, gbFF_);
    xoF_.resize(R); for (int i = 0; i < R; ++i) xoF_[i] = static_cast<float>(m.pm.Xo[i]); dofF_.resize(NF); for (int j = 0; j < NF; ++j) dofF_[j] = static_cast<float>(m.pm.Dof[j]);
    ga1F_.resize(R); gb1F_.resize(R); ga2F_.resize(R); gb2F_.resize(R); pd1F_.resize(NP); pd2F_.resize(NP);
    for (int i = 0; i < R; ++i) { ga1F_[i] = static_cast<float>(P.Ga[i * NU + 1]); gb1F_[i] = static_cast<float>(P.Gb[i * NU + 1]); ga2F_[i] = static_cast<float>(P.Ga[i * NU + 2] + P.Gb[i * NU + 2]); }
    for (int a = 0; a < NP; ++a) { pd1F_[a] = static_cast<float>(m.PD[a * NU + 1]); pd2F_[a] = static_cast<float>(m.PD[a * NU + 2]); }
  }
  // y = init + M x (rows x n, row stride ld), float; AVX with 4 rows per sweep, else SSE
  static void MatVecF(const float* Mat, int ld, int rows, const float* x, int n, float* y, const float* init)
  {
    int i = 0;
    if (g_ff2Avx)
    {
      for (; i + 4 <= rows; i += 4)
      {
        const float* m0 = Mat + (size_t)i * ld; const float* m1 = m0 + ld; const float* m2 = m1 + ld; const float* m3 = m2 + ld;
        __m256 a = _mm256_setzero_ps(), b = a, c = a, d = a; int k = 0;
        for (; k + 8 <= n; k += 8) { const __m256 xv = _mm256_loadu_ps(x + k); a = _mm256_add_ps(a, _mm256_mul_ps(_mm256_loadu_ps(m0 + k), xv)); b = _mm256_add_ps(b, _mm256_mul_ps(_mm256_loadu_ps(m1 + k), xv)); c = _mm256_add_ps(c, _mm256_mul_ps(_mm256_loadu_ps(m2 + k), xv)); d = _mm256_add_ps(d, _mm256_mul_ps(_mm256_loadu_ps(m3 + k), xv)); }
        const __m256 t0 = _mm256_hadd_ps(a, b), t1 = _mm256_hadd_ps(c, d), t2 = _mm256_hadd_ps(t0, t1);   // pairs: [a0 a1 b0 b1 | ...]
        const __m128 lo = _mm256_castps256_ps128(t2), hi = _mm256_extractf128_ps(t2, 1); const __m128 sm = _mm_add_ps(lo, hi); _mm256_zeroupper();
        float t[4]; _mm_storeu_ps(t, sm);   // order after hadd(hadd(a,b),hadd(c,d)) and lo + hi: a, b, c, d
        float s0 = t[0], s1 = t[1], s2 = t[2], s3 = t[3];
        for (; k < n; ++k) { const float xv = x[k]; s0 += m0[k] * xv; s1 += m1[k] * xv; s2 += m2[k] * xv; s3 += m3[k] * xv; }
        y[i] = (init ? init[i] : 0.0f) + s0; y[i + 1] = (init ? init[i + 1] : 0.0f) + s1; y[i + 2] = (init ? init[i + 2] : 0.0f) + s2; y[i + 3] = (init ? init[i + 3] : 0.0f) + s3;
      }
    }
    for (; i < rows; ++i) { const float* mr = Mat + (size_t)i * ld; float s = init ? init[i] : 0.0f; for (int k = 0; k < n; ++k) s += mr[k] * x[k]; y[i] = s; }
  }
  bool NextF(Cursor& C, double& tOut, double& dyOut)
  {
    Model& m = *m_; const typename Model::Prop& P = *P_; const double h = P.h; const long k = C.k;
    float dzk[R], df0c[NF], dfkc[NF], rhs[NP], dpk[NP], df1[NF], vin[R], vpd[NP];
    const double d0 = C.nPrev, d1 = (coarse > 1 ? C.nz->AtCoarse(C.tOff + (k + 1) * h, coarse, eq) : C.nz->At(C.tOff + (k + 1) * h)) - 7.5, dd = (d1 - d0) / h; C.nPrev = d1;
    if (C.ep < eps_.size() && eps_[C.ep].start == k) { C.cur = &eps_[C.ep]; ++C.ep; ++C.refreshes; }
    const Epoch& E = *static_cast<const Epoch*>(C.cur);
    const float f0 = static_cast<float>(d0), f1 = static_cast<float>(d1), fd = static_cast<float>(dd);
    for (int i = 0; i < R; ++i) vin[i] = ga1F_[i] * f0 + gb1F_[i] * f1 + ga2F_[i] * fd;
    for (int a = 0; a < NP; ++a) vpd[a] = pd1F_[a] * f1 + pd2F_[a] * fd;
    for (int j = 0; j < nGc_; ++j) df0c[j] = C.dfPrevF[gcCols_[j]];
    MatVecF(phiF_.data(), R, R, C.dzF, R, dzk, vin);
    MatVecF(gaFF_.data(), nGc_, R, df0c, nGc_, dzk, dzk);
    MatVecF(pxzF_.data(), R, NP, dzk, R, dpk, vpd);
    for (int j = 0; j < nCap_; ++j) dfkc[j] = -E.capG[j] * C.dpF[capPort_[j]];
    MatVecF(wCapF_.data(), nCap_, NP, dfkc, nCap_, rhs, dpk);
    double rd[NP], xd[NP]; for (int a = 0; a < NP; ++a) rd[a] = rhs[a]; Solve(E, rd, xd); for (int a = 0; a < NP; ++a) C.dpF[a] = static_cast<float>(xd[a]);
    for (int j = 0; j < NF; ++j) df1[j] = 0.0f; for (const EntF& n : E.ent) df1[n.cur] += n.g * C.dpF[n.port]; for (int c = 0; c < nCap_; ++c) df1[capCur_[c]] += dfkc[c];
    MatVecF(gbFF_.data(), NF, R, df1, NF, C.dzF, dzk);
    float dyf = 0.0f; for (int i = 0; i < R; ++i) dyf += xoF_[i] * C.dzF[i]; for (int j = 0; j < NF; ++j) dyf += dofF_[j] * df1[j];
    const double dy = m.pm.Dou[1] * d1 + m.pm.Dou[2] * dd + dyf;
    for (int j = 0; j < NF; ++j) C.dfPrevF[j] = df1[j];
    tOut = (k + 1) * h; dyOut = dy; ++C.k; ++C.knots; return true;
  }
  // ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
  // Condensed form. Within an epoch the knot map is linear time-invariant: s' = A s + B0 n_k + B1 n_(k+1), dy = cy.s + e0 n_k + e1 n_(k+1) with the state s = (dz, dfPrev at the GaF columns, dp at the charge-device ports) and the
  // noise values n_k at the knots. A, B, cy, e are obtained by running the existing one-knot map (StepState, the body of Next()) on unit vectors; a block of m knots is then M = [A^m; cy A^(i)], W = input responses. The result is
  // the same linear map as Next() (no approximation, only the summation order and the float tables differ).
  void StepState(const Epoch& E, const double* s, double d0, double d1, double* s2, double& dyOut) const
  {
    Model& m = *m_; const typename Model::Prop& P = *P_; const double h = P.h, dd = (d1 - d0) / h;
    double dz[R], dfPrev[NF], dp0[NP], dzk[R], df0c[NF], dfkc[NF], rhs[NP], dpk[NP], dpn[NP], df1[NF], vin[R], vpd[NP], dzn[R];
    for (int i = 0; i < R; ++i) dz[i] = s[i]; for (int j = 0; j < NF; ++j) dfPrev[j] = 0.0; for (int a = 0; a < NP; ++a) dp0[a] = 0.0;
    for (int j = 0; j < nGc_; ++j) dfPrev[gcCols_[j]] = s[R + j]; for (size_t j = 0; j < capPU_.size(); ++j) dp0[capPU_[j]] = s[R + nGc_ + j];
    for (int i = 0; i < R; ++i) vin[i] = P.Ga[i * NU + 1] * d0 + P.Gb[i * NU + 1] * d1 + (P.Ga[i * NU + 2] + P.Gb[i * NU + 2]) * dd;
    for (int a = 0; a < NP; ++a) vpd[a] = m.PD[a * NU + 1] * d1 + m.PD[a * NU + 2] * dd;
    for (int j = 0; j < nGc_; ++j) df0c[j] = dfPrev[gcCols_[j]];
    Model::MatVec(P.Phi.data(), R, R, dz, R, dzk, vin);
    Model::MatVec(gaF_.data(), nGc_, R, df0c, nGc_, dzk, dzk);
    Model::MatVec(P.PXz.data(), R, NP, dzk, R, dpk, vpd);
    for (int j = 0; j < nCap_; ++j) dfkc[j] = -capLag * E.capG[j] * dp0[capPort_[j]];
    Model::MatVec(wCap_.data(), nCap_, NP, dfkc, nCap_, rhs, dpk);
    Solve(E, rhs, dpn);
    ApplyG(E, dpn, df1); for (int c = 0; c < nCap_; ++c) df1[capCur_[c]] += dfkc[c];
    Model::MatVec(P.GbF.data(), NF, R, df1, NF, dzn, dzk);
    double dy = m.pm.Dou[1] * d1 + m.pm.Dou[2] * dd + Model::DotF(m.pm.Xo, dzn, R, 0.0); dy = Model::DotF(m.pm.Dof, df1, NF, dy);
    for (int i = 0; i < R; ++i) s2[i] = dzn[i]; for (int j = 0; j < nGc_; ++j) s2[R + j] = df1[gcCols_[j]]; for (size_t j = 0; j < capPU_.size(); ++j) s2[R + nGc_ + j] = dpn[capPU_[j]];
    dyOut = dy;
  }
  static void MulD(const std::vector<double>& a, int ra, int ca, const std::vector<double>& b, int cb, std::vector<double>& c)   // c (ra x cb) = a (ra x ca) b (ca x cb)
  {
    c.assign(static_cast<size_t>(ra) * cb, 0.0);
    for (int i = 0; i < ra; ++i) for (int k = 0; k < ca; ++k) { const double v = a[static_cast<size_t>(i) * ca + k]; if (v == 0.0) continue; const double* br = &b[static_cast<size_t>(k) * cb]; double* cr = &c[static_cast<size_t>(i) * cb]; for (int j = 0; j < cb; ++j) cr[j] += v * br[j]; }
  }
  void MakeBlk(const std::vector<double>& A, const std::vector<double>& B0, const std::vector<double>& B1, const std::vector<double>& cy, double e0, double e1, int mk, Blk& out) const
  {
    const int n = nS_, npad = nPad_; out.m = mk; const int rows = n + mk, nin = mk + 1;
    std::vector<double> Ap(static_cast<size_t>(n) * n, 0.0), V(static_cast<size_t>(n) * nin, 0.0), V2, Orow(static_cast<size_t>(mk) * n, 0.0), T(static_cast<size_t>(mk) * nin, 0.0);
    // powers of A: A^mk by binary exponentiation
    { std::vector<double> R1(static_cast<size_t>(n) * n, 0.0), Pw = A, tmp; for (int i = 0; i < n; ++i) R1[static_cast<size_t>(i) * n + i] = 1.0; int e = mk; while (e > 0) { if (e & 1) { MulD(R1, n, n, Pw, n, tmp); R1.swap(tmp); } e >>= 1; if (e) { MulD(Pw, n, n, Pw, n, tmp); Pw.swap(tmp); } } Ap = R1; }
    // output rows: cy A^i (i = 0 .. mk-1), and the response of the state to the noise values V^(i) (n x nin), outputs T
    std::vector<double> row = cy, rn(static_cast<size_t>(n));
    for (int i = 0; i < mk; ++i)
    {
      for (int j = 0; j < n; ++j) Orow[static_cast<size_t>(i) * n + j] = row[j];
      // T[i][j] = cy . V^(i)_j + [j == i] e0 + [j == i + 1] e1
      for (int j = 0; j < nin; ++j) { double sacc = 0.0; for (int q = 0; q < n; ++q) sacc += cy[q] * V[static_cast<size_t>(q) * nin + j]; if (j == i) sacc += e0; if (j == i + 1) sacc += e1; T[static_cast<size_t>(i) * nin + j] = sacc; }
      // V^(i+1) = A V^(i) + B0 at column i + B1 at column i + 1
      MulD(A, n, n, V, nin, V2); for (int q = 0; q < n; ++q) { V2[static_cast<size_t>(q) * nin + i] += B0[q]; V2[static_cast<size_t>(q) * nin + i + 1] += B1[q]; } V.swap(V2);
      for (int j = 0; j < n; ++j) { double sacc = 0.0; for (int q = 0; q < n; ++q) sacc += row[q] * A[static_cast<size_t>(q) * n + j]; rn[j] = sacc; } row = rn;
    }
    out.rowsPad = (rows + 7) & ~7; out.cols = npad + nin; out.Mt.assign(static_cast<size_t>(out.cols) * out.rowsPad, 0.0f);
    for (int i = 0; i < n; ++i) { for (int j = 0; j < n; ++j) out.Mt[static_cast<size_t>(j) * out.rowsPad + i] = static_cast<float>(Ap[static_cast<size_t>(i) * n + j]); for (int j = 0; j < nin; ++j) out.Mt[static_cast<size_t>(npad + j) * out.rowsPad + i] = static_cast<float>(V[static_cast<size_t>(i) * nin + j]); }
    for (int i = 0; i < mk; ++i) { for (int j = 0; j < n; ++j) out.Mt[static_cast<size_t>(j) * out.rowsPad + n + i] = static_cast<float>(Orow[static_cast<size_t>(i) * n + j]); for (int j = 0; j < nin; ++j) out.Mt[static_cast<size_t>(npad + j) * out.rowsPad + n + i] = static_cast<float>(T[static_cast<size_t>(i) * nin + j]); }
  }
  // diagnosis (env DGG_CONDDIAG): numerical rank (pivoted Gram-Schmidt, relative tolerance) of A and A^cond of some epochs
  static int RankPiv(std::vector<double> Mx, int n, double tol)
  {
    std::vector<double> nrm(n); int rank = 0; double first = 0.0;
    for (int step = 0; step < n; ++step)
    {
      int bj = -1; double bn = -1.0; for (int j = 0; j < n; ++j) { double s = 0.0; for (int i = 0; i < n; ++i) s += Mx[static_cast<size_t>(i) * n + j] * Mx[static_cast<size_t>(i) * n + j]; if (s > bn) { bn = s; bj = j; } }
      bn = std::sqrt(bn); if (step == 0) first = bn; if (bn <= tol * first) break; ++rank;
      std::vector<double> q(n); for (int i = 0; i < n; ++i) q[i] = Mx[static_cast<size_t>(i) * n + bj] / bn;
      for (int j = 0; j < n; ++j) { double d = 0.0; for (int i = 0; i < n; ++i) d += q[i] * Mx[static_cast<size_t>(i) * n + j]; for (int i = 0; i < n; ++i) Mx[static_cast<size_t>(i) * n + j] -= d * q[i]; }
    }
    return rank;
  }
  void BuildCond(Model& m, const typename Model::Prop& P)
  {
    (void)P; capPU_.clear(); for (int j = 0; j < nCap_; ++j) { bool have = false; for (int q : capPU_) if (q == capPort_[j]) have = true; if (!have) capPU_.push_back(capPort_[j]); }
    nS_ = R + nGc_ + static_cast<int>(capPU_.size()); nPad_ = (nS_ + 7) & ~7; const int n = nS_; (void)m;
    auto work = [&](size_t e0i, size_t e1i)
    {
      std::vector<double> A(static_cast<size_t>(n) * n), B0(n), B1(n), cy(n), s(n), s2(n);
      for (size_t ei = e0i; ei < e1i; ++ei)
      {
        Epoch& E = eps_[ei]; double dy, e0, e1;
        for (int c = 0; c < n; ++c) { std::fill(s.begin(), s.end(), 0.0); s[c] = 1.0; StepState(E, s.data(), 0.0, 0.0, s2.data(), dy); for (int i = 0; i < n; ++i) A[static_cast<size_t>(i) * n + c] = s2[i]; cy[c] = dy; }
        std::fill(s.begin(), s.end(), 0.0); StepState(E, s.data(), 1.0, 0.0, B0.data(), e0); StepState(E, s.data(), 0.0, 1.0, B1.data(), e1);
        MakeBlk(A, B0, B1, cy, e0, e1, cond, E.bm); MakeBlk(A, B0, B1, cy, e0, e1, 1, E.b1);
        if (std::getenv("DGG_CONDDIAG") && ei % 60 == 30) { std::vector<double> P = A, T2; for (int q = 1; q < cond; q *= 2) { MulD(P, n, n, P, n, T2); P.swap(T2); } const long len = (ei + 1 < eps_.size() ? eps_[ei + 1].start : epochKnots_) - E.start; std::fprintf(stderr, "epoch %zu start %ld len %ld n %d (R %d nGc %d nCap %zu) rank A: %d %d %d, rank A^%d: %d %d %d\n", ei, E.start, len, n, R, nGc_, capPU_.size(), RankPiv(A, n, 1e-3), RankPiv(A, n, 1e-5), RankPiv(A, n, 1e-7), cond, RankPiv(P, n, 1e-3), RankPiv(P, n, 1e-5), RankPiv(P, n, 1e-7)); }
      }
    };
    unsigned nt = std::max(1u, std::thread::hardware_concurrency()); if (condThreads > 0) nt = static_cast<unsigned>(condThreads); if (nt > eps_.size()) nt = static_cast<unsigned>(eps_.size() ? eps_.size() : 1);
    std::vector<std::thread> th; const size_t ne = eps_.size();
    for (unsigned t = 0; t < nt; ++t) th.emplace_back(work, ne * t / nt, ne * (t + 1) / nt);
    for (auto& t : th) t.join();
  }
  int condThreads = 0;   // 0: all hardware threads for the preparation
  // y[0 .. rowsPad) = sum over the columns c of x[c] * Mt[c * rowsPad ..] (column-major, rowsPad a multiple of 8): the accumulators of a row chunk stay in registers over all columns
  template <int NV> static inline void AxpyChunk(const float* Mt, int cols, int rowsPad, const float* x, float* y, int r0)
  {
    __m256 a[NV]; for (int v = 0; v < NV; ++v) a[v] = _mm256_setzero_ps();
    for (int c = 0; c < cols; ++c) { const __m256 xv = _mm256_broadcast_ss(x + c); const float* p = Mt + static_cast<size_t>(c) * rowsPad + r0; for (int v = 0; v < NV; ++v) a[v] = _mm256_add_ps(a[v], _mm256_mul_ps(_mm256_loadu_ps(p + 8 * v), xv)); }
    for (int v = 0; v < NV; ++v) _mm256_storeu_ps(y + r0 + 8 * v, a[v]);
  }
  static void AxpyCols(const float* Mt, int cols, int rowsPad, const float* x, float* y)
  {
    if (g_ff2Avx)
    {
      int r0 = 0; for (; r0 + 48 <= rowsPad; r0 += 48) AxpyChunk<6>(Mt, cols, rowsPad, x, y, r0);
      switch ((rowsPad - r0) / 8) { case 1: AxpyChunk<1>(Mt, cols, rowsPad, x, y, r0); break; case 2: AxpyChunk<2>(Mt, cols, rowsPad, x, y, r0); break; case 3: AxpyChunk<3>(Mt, cols, rowsPad, x, y, r0); break; case 4: AxpyChunk<4>(Mt, cols, rowsPad, x, y, r0); break; case 5: AxpyChunk<5>(Mt, cols, rowsPad, x, y, r0); break; default: break; }
      _mm256_zeroupper(); return;
    }
    for (int i = 0; i < rowsPad; ++i) y[i] = 0.0f;
    for (int c = 0; c < cols; ++c) { const float xv = x[c]; const float* p = Mt + static_cast<size_t>(c) * rowsPad; for (int i = 0; i < rowsPad; ++i) y[i] += p[i] * xv; }
  }
  // ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
  // Reduced-order form (condTol > 0). The one-knot matrix A of an epoch has a small numerical rank (MT: ~40 at 1e-5 of the largest column, state dimension 233): A = Q R (pivoted Gram-Schmidt, Q n x r orthonormal, R = Q^T A).
  // With z_k = R s_k the state is s_k = Q z_(k-1) + B0 n_(k-1) + B1 n_k, so per knot  z_k = F z_(k-1) + G0 n_(k-1) + G1 n_k  and  y_(k+1) = c z_(k-1) + w0 n_(k-1) + w1 n_k + e1 n_(k+1)  (F = R Q, c = cy Q, ...).
  // The first knot of an epoch maps the reduced state of the previous epoch (basis Q_p) into the basis of the new one (F = R_e Q_p, c = cy_e Q_p, G = R_e B_p, ...): the full state is Q_p z + B0_p n_(k-1) + B1_p n_k.
  bool redOn_ = false; static constexpr int kRMax = 200;
  struct EpFull { int r = 0; std::vector<double> Q, Rm, B0, B1, cy; double e0 = 0.0, e1 = 0.0; };   // Q: n x r (row-major), Rm: r x n
  static void FactorQR(const std::vector<double>& A, int n, double tol, int rmax, std::vector<double>& Q, std::vector<double>& Rm, int& r)
  {
    std::vector<double> Wt(static_cast<size_t>(n) * n); for (int i = 0; i < n; ++i) for (int j = 0; j < n; ++j) Wt[static_cast<size_t>(j) * n + i] = A[static_cast<size_t>(i) * n + j];   // columns as rows
    std::vector<std::vector<double>> q; double first = 0.0; std::vector<double> nr(n);
    for (int j = 0; j < n; ++j) { double s = 0.0; for (int i = 0; i < n; ++i) s += Wt[static_cast<size_t>(j) * n + i] * Wt[static_cast<size_t>(j) * n + i]; nr[j] = s; }
    while (static_cast<int>(q.size()) < rmax)
    {
      int bj = -1; double bn = -1.0; for (int j = 0; j < n; ++j) if (nr[j] > bn) { bn = nr[j]; bj = j; }
      bn = std::sqrt(std::max(bn, 0.0)); if (q.empty()) first = bn; if (bj < 0 || bn <= tol * first || bn <= 0.0) break;
      std::vector<double> v(Wt.begin() + static_cast<size_t>(bj) * n, Wt.begin() + static_cast<size_t>(bj + 1) * n);
      for (const auto& qi : q) { double d = 0.0; for (int i = 0; i < n; ++i) d += qi[i] * v[i]; for (int i = 0; i < n; ++i) v[i] -= d * qi[i]; }
      double nn = 0.0; for (int i = 0; i < n; ++i) nn += v[i] * v[i]; nn = std::sqrt(nn); if (nn <= 0.0) break; for (int i = 0; i < n; ++i) v[i] /= nn;
      for (int j = 0; j < n; ++j) { double* w = &Wt[static_cast<size_t>(j) * n]; double d = 0.0; for (int i = 0; i < n; ++i) d += v[i] * w[i]; double s = 0.0; for (int i = 0; i < n; ++i) { w[i] -= d * v[i]; s += w[i] * w[i]; } nr[j] = s; }
      q.push_back(std::move(v));
    }
    r = static_cast<int>(q.size()); Q.assign(static_cast<size_t>(n) * (r > 0 ? r : 1), 0.0); Rm.assign(static_cast<size_t>(r > 0 ? r : 1) * n, 0.0);
    for (int c = 0; c < r; ++c) for (int i = 0; i < n; ++i) Q[static_cast<size_t>(i) * r + c] = q[c][i];
    for (int c = 0; c < r; ++c) for (int j = 0; j < n; ++j) { double s = 0.0; for (int i = 0; i < n; ++i) s += q[c][i] * A[static_cast<size_t>(i) * n + j]; Rm[static_cast<size_t>(c) * n + j] = s; }
  }
  // op for a step that maps the reduced state of basis `prev` (Qp, rp; nullptr: none / zero state) into the basis of `cur`; the output of the step uses the cy / e of `cur`
  void MakeOp(const EpFull& cur, const EpFull* prev, RedOp& op) const
  {
    const int n = nS_, rn = cur.r, rp = prev ? prev->r : 0; op.rn = rn; op.rp = rp; op.cols = 3 + rp; op.rowsPad = ((rn + 1) + 7) & ~7; op.Mt.assign(static_cast<size_t>(op.cols) * op.rowsPad, 0.0f);
    auto at = [&](int c, int row) -> float& { return op.Mt[static_cast<size_t>(c) * op.rowsPad + row]; };
    if (!prev) { at(1, rn) = static_cast<float>(cur.e0); at(2, rn) = static_cast<float>(cur.e1); return; }   // first knot of the hit: s_0 = 0, y_1 = e0 n_0 + e1 n_1 (columns: n_(k-1) unused, n_k = n_0, n_(k+1) = n_1)
    for (int i = 0; i < rn; ++i)
    {
      double g0 = 0.0, g1 = 0.0; for (int q = 0; q < n; ++q) { const double rv = cur.Rm[static_cast<size_t>(i) * n + q]; g0 += rv * prev->B0[q]; g1 += rv * prev->B1[q]; }
      at(0, i) = static_cast<float>(g0); at(1, i) = static_cast<float>(g1);
      for (int j = 0; j < rp; ++j) { double s = 0.0; for (int q = 0; q < n; ++q) s += cur.Rm[static_cast<size_t>(i) * n + q] * prev->Q[static_cast<size_t>(q) * rp + j]; at(3 + j, i) = static_cast<float>(s); }
    }
    double w0 = 0.0, w1 = 0.0; for (int q = 0; q < n; ++q) { w0 += cur.cy[q] * prev->B0[q]; w1 += cur.cy[q] * prev->B1[q]; }
    at(0, rn) = static_cast<float>(w0); at(1, rn) = static_cast<float>(w1 + cur.e0); at(2, rn) = static_cast<float>(cur.e1);
    for (int j = 0; j < rp; ++j) { double s = 0.0; for (int q = 0; q < n; ++q) s += cur.cy[q] * prev->Q[static_cast<size_t>(q) * rp + j]; at(3 + j, rn) = static_cast<float>(s); }
  }
  void BuildRed(Model& m, const typename Model::Prop& P, double tolUse)
  {
    (void)P; (void)m; capPU_.clear(); for (int j = 0; j < nCap_; ++j) { bool have = false; for (int q : capPU_) if (q == capPort_[j]) have = true; if (!have) capPU_.push_back(capPort_[j]); }
    nS_ = R + nGc_ + static_cast<int>(capPU_.size()); const int n = nS_; const size_t ne = eps_.size(); const size_t CH = 48; std::vector<EpFull> F(CH + 1); size_t fb = 0;   // waves of CH epochs (a window of the full-dimension factors: bounded memory)
    unsigned nt = std::max(1u, std::thread::hardware_concurrency()); if (condThreads > 0) nt = static_cast<unsigned>(condThreads); if (nt > ne) nt = static_cast<unsigned>(ne ? ne : 1);
    auto stage1 = [&](size_t a, size_t b)
    {
      std::vector<double> A(static_cast<size_t>(n) * n), s(n), s2(n);
      for (size_t ei = a; ei < b; ++ei)
      {
        Epoch& E = eps_[ei]; EpFull& f = F[ei - fb]; double dy; f.B0.assign(n, 0.0); f.B1.assign(n, 0.0); f.cy.assign(n, 0.0);
        for (int c = 0; c < n; ++c) { std::fill(s.begin(), s.end(), 0.0); s[c] = 1.0; StepState(E, s.data(), 0.0, 0.0, s2.data(), dy); for (int i = 0; i < n; ++i) A[static_cast<size_t>(i) * n + c] = s2[i]; f.cy[c] = dy; }
        std::fill(s.begin(), s.end(), 0.0); StepState(E, s.data(), 1.0, 0.0, f.B0.data(), f.e0); StepState(E, s.data(), 0.0, 1.0, f.B1.data(), f.e1);
        // state scale: rms of every state component in a noise driven run of this epoch's map (the truncation must be relative to the size of the state, which differs by orders of magnitude between the components)
        std::vector<double> D(n, 0.0), sx(n, 0.0), sy(n); { std::mt19937 rg(static_cast<unsigned>(ei) * 7919u + 17u); std::normal_distribution<double> nd(0.0, 1.0); double un = nd(rg); const int steps = 80;
          for (int st = 0; st < steps; ++st) { const double un1 = nd(rg); for (int i = 0; i < n; ++i) { double a = f.B0[i] * un + f.B1[i] * un1; const double* ar = &A[static_cast<size_t>(i) * n]; for (int j = 0; j < n; ++j) a += ar[j] * sx[j]; sy[i] = a; } sx = sy; un = un1; if (st >= steps / 2) for (int i = 0; i < n; ++i) D[i] += sx[i] * sx[i]; } }
        double dmax = 0.0; for (int i = 0; i < n; ++i) { D[i] = std::sqrt(D[i] / (80 / 2)); dmax = std::max(dmax, D[i]); } for (int i = 0; i < n; ++i) D[i] = std::max(D[i], 1e-4 * dmax + 1e-300);
        { std::vector<double> As(static_cast<size_t>(n) * n); for (int i = 0; i < n; ++i) for (int j = 0; j < n; ++j) As[static_cast<size_t>(i) * n + j] = A[static_cast<size_t>(i) * n + j] * D[j] / D[i];
          FactorQR(As, n, tolUse, kRMax, f.Q, f.Rm, f.r); const int r = f.r; for (int i = 0; i < n; ++i) for (int c = 0; c < r; ++c) f.Q[static_cast<size_t>(i) * r + c] *= D[i]; for (int c = 0; c < r; ++c) for (int j = 0; j < n; ++j) f.Rm[static_cast<size_t>(c) * n + j] /= D[j]; }
        if (std::getenv("DGG_CONDDIAG") && ei % 60 == 30) std::fprintf(stderr, "epoch %zu rank %d (tol %g)\n", ei, f.r, tolUse);
      }
    };
    auto stage2 = [&](size_t a, size_t b) { for (size_t ei = a; ei < b; ++ei) { MakeOp(F[ei - fb], ei > 0 ? &F[ei - 1 - fb] : nullptr, eps_[ei].rin); MakeOp(F[ei - fb], &F[ei - fb], eps_[ei].rst); } };
    auto par = [&](auto&& fn, size_t a, size_t b) { unsigned n2 = static_cast<unsigned>(std::min<size_t>(nt, b - a)); std::vector<std::thread> th; for (unsigned t = 0; t < n2; ++t) th.emplace_back(fn, a + (b - a) * t / n2, a + (b - a) * (t + 1) / n2); for (auto& t : th) t.join(); };
    for (size_t wa = 0; wa < ne; wa += CH) { const size_t we = std::min(ne, wa + CH); fb = wa > 0 ? wa - 1 : 0; par(stage1, fb, we); par(stage2, wa, we); }
  }
  void FreeFull() { std::vector<double>().swap(tE); std::vector<double>().swap(pE); for (Epoch& E : eps_) { std::vector<float>().swap(E.LU); std::vector<float>().swap(E.JFA); std::vector<float>().swap(E.capG); std::vector<EntF>().swap(E.ent); std::vector<float>().swap(E.capGs); } }
  // Self test of the reduced form against the exact one-knot maps on two noise sequences (the full tables are still present): relative energy of the difference of the responses in dB.
  double ValidateRed(double tEnd)
  {
    double worst = -300.0;
    for (int sd = 0; sd < 2; ++sd)
    {
      HitNoise n1, n2; n1.Start(777 + sd, 5e-6, 0.0); n2.Start(777 + sd, 5e-6, 0.0); Cursor* Ce = new Cursor(); Cursor* Cr = new Cursor(); double e = 0.0, s = 0.0, t, dy, t2, dy2;
      redOn_ = false; Start(*Ce, n1, startT, tEnd); redOn_ = true; Start(*Cr, n2, startT, tEnd);
      for (;;) { redOn_ = false; const bool a = Next(*Ce, t, dy); redOn_ = true; const bool b = Next(*Cr, t2, dy2); if (!a || !b) break; e += (dy - dy2) * (dy - dy2); s += dy * dy; }
      delete Ce; delete Cr; worst = std::max(worst, 10.0 * std::log10(std::max(e, 1e-300) / std::max(s, 1e-300)));
    }
    return worst;
  }

  bool NextR(Cursor& C, double& tOut, double& dyOut)
  {
    const double h = P_->h; const long k = C.k; bool entry = false;
    if (C.ep < eps_.size() && eps_[C.ep].start == k) { C.cur = &eps_[C.ep]; ++C.ep; ++C.refreshes; entry = true; }
    const Epoch& E = *static_cast<const Epoch*>(C.cur); const RedOp& op = entry ? E.rin : E.rst;
    float* x = C.xr; x[2] = static_cast<float>((coarse > 1 ? C.nz->AtCoarse(C.tOff + (k + 1) * h, coarse, eq) : C.nz->At(C.tOff + (k + 1) * h)) - 7.5);
    float out[kRMax + 16]; LIVE_PROF_T(4, AxpyCols(op.Mt.data(), op.cols, op.rowsPad, x, out));
    for (int i = 0; i < op.rn; ++i) x[3 + i] = out[i];
    dyOut = out[op.rn]; x[0] = x[1]; x[1] = x[2]; tOut = (k + 1) * h; ++C.k; ++C.knots; return true;
  }
  bool NextC(Cursor& C, double& tOut, double& dyOut)
  {
    const double h = P_->h;
    if (C.yqPos >= C.yqN)
    {
      const long k = C.k;
      if (C.ep < eps_.size() && eps_[C.ep].start == k) { C.cur = &eps_[C.ep]; ++C.ep; ++C.refreshes; }
      const Epoch& E = *static_cast<const Epoch*>(C.cur); const long nextStart = C.ep < eps_.size() ? eps_[C.ep].start : epochKnots_; const long lim = std::min(nextStart, C.N);
      const Blk& B = (lim - k >= E.bm.m) ? E.bm : E.b1; const int mk = B.m; float* nb = C.sC + nPad_; float out[R + NF + NP + 64]; nb[0] = static_cast<float>(C.nPrev);
      LIVE_PROF_T(2, for (int j = 1; j <= mk; ++j) nb[j] = static_cast<float>((coarse > 1 ? C.nz->AtCoarse(C.tOff + (k + j) * h, coarse, eq) : C.nz->At(C.tOff + (k + j) * h)) - 7.5));
      C.nPrev = nb[mk];
      LIVE_PROF_T(4, AxpyCols(B.Mt.data(), B.cols, B.rowsPad, C.sC, out));
      for (int i = 0; i < nS_; ++i) C.sC[i] = out[i]; for (int i = 0; i < mk; ++i) C.yq[i] = out[nS_ + i]; C.yqPos = 0; C.yqN = mk;
      for (int i = 0; i < nS_; ++i) C.sC[i] = out[i]; for (int i = 0; i < mk; ++i) C.yq[i] = out[nS_ + i]; C.yqPos = 0; C.yqN = mk;
    }
    dyOut = C.yq[C.yqPos++]; tOut = (C.k + 1) * h; ++C.k; ++C.knots; return true;
  }

  void BuildEpochs(Model& m, const typename Model::Prop& P, double tEnd)
  {
    const double h = P.h; epochKnots_ = static_cast<long>(tEnd / h); eps_.clear(); size_t seg = 0; double pEn[NP], pRef[NP]; bool have = false;
    for (long k = 0; k < epochKnots_; ++k)
    {
      Interp((k + 1) * h, seg, pEn); bool refresh = !have;
      if (have) for (int a = 0; a < NP; ++a) if (std::fabs(pEn[a] - pRef[a]) * invThr_[a] > (k * h < 1e-3 ? 1.0 : laterMul)) { refresh = true; break; }
      if (!refresh) continue;
      Refresh(m, P, pEn, h); for (int a = 0; a < NP; ++a) pRef[a] = pEn[a]; have = true;
      eps_.emplace_back(); Epoch& e = eps_.back(); e.start = k; e.nA = m.nAct; e.nF = m.nFrz;
      for (int i = 0; i < NP; ++i) { e.act[i] = (unsigned char)m.actL[i]; e.frz[i] = (unsigned char)m.frzL[i]; e.piv[i] = (unsigned char)m.piv[i]; }
      if (dbl) e.LUd.assign((size_t)e.nA * e.nA, 0.0);
      e.LU.assign((size_t)e.nA * e.nA, 0.0f);   // the inverse of the active block (column c: solution for the unit vector c), computed once here from the factorisation
      for (int c = 0; c < e.nA; ++c)
      {
        double Fa[NP]; for (int i = 0; i < e.nA; ++i) Fa[i] = i == c ? 1.0 : 0.0;
        for (int q = 0; q < e.nA; ++q) if (m.piv[q] != q) std::swap(Fa[q], Fa[m.piv[q]]);
        for (int i = 1; i < e.nA; ++i) Fa[i] -= Model::DotF(m.LU[i], Fa, i, 0.0);
        for (int i = e.nA - 1; i >= 0; --i) Fa[i] = (Fa[i] - Model::DotF(&m.LU[i][i + 1], &Fa[i + 1], e.nA - i - 1, 0.0)) / m.LU[i][i];
        for (int i = 0; i < e.nA; ++i) { e.LU[(size_t)i * e.nA + c] = static_cast<float>(Fa[i]); if (dbl) e.LUd[(size_t)i * e.nA + c] = Fa[i]; }
      }
      if (dbl) { e.JFAd.assign((size_t)(e.nF > 0 ? e.nF : 1) * (e.nA > 0 ? e.nA : 1), 0.0); for (int f = 0; f < e.nF; ++f) for (int j = 0; j < e.nA; ++j) e.JFAd[(size_t)f * e.nA + j] = m.JFA[f][j]; }
      e.JFA.assign((size_t)(e.nF > 0 ? e.nF : 1) * (e.nA > 0 ? e.nA : 1), 0.0f); for (int f = 0; f < e.nF; ++f) for (int j = 0; j < e.nA; ++j) e.JFA[(size_t)f * e.nA + j] = static_cast<float>(m.JFA[f][j]);
      e.capG.assign(capG_.begin(), capG_.end()); e.ent.reserve(ent_.size()); for (const Ent& n : ent_) e.ent.push_back({n.cur, n.port, static_cast<float>(n.g)});
    }
  }

  void CapGAt(Model& m, const double* pSt, double h, std::vector<float>& out)
  {
    double jd[ND][6], f[NF]; for (int d = 0; d < ND; ++d) for (int e = 0; e < 6; ++e) jd[d][e] = 0.0;
    for (int i = 0; i < NP; ++i) m.ps0[i] = pSt[i]; ++m.q0Gen; m.capH = h; m.EvalF(pSt, f, jd);
    out.assign(static_cast<size_t>(nCap_ > 0 ? nCap_ : 1), 0.0f); for (int j = 0; j < nCap_; ++j) out[static_cast<size_t>(j)] = static_cast<float>(jd[capDev_[static_cast<size_t>(j)]][0]);
  }
  void BuildStepEpochs(Model& m, double tEnd)
  {
    eps_.clear(); stepsN_ = 0; double pRef[NP]; bool have = false; int jRef = -1; const long S = static_cast<long>(tE.size()); double pSt[NP];
    for (long s = 0; s < S && tE[static_cast<size_t>(s)] <= tEnd; ++s)
    {
      const double tb = s > 0 ? tE[static_cast<size_t>(s - 1)] : 0.0, h = tE[static_cast<size_t>(s)] - tb; int j = static_cast<int>(std::lround(std::log2(h / h0_))); if (j < 0) j = 0; if (j > 5) j = 5;
      const double* pEn = &pE[static_cast<size_t>(s) * NP]; if (s > 0) for (int a = 0; a < NP; ++a) pSt[a] = pE[static_cast<size_t>(s - 1) * NP + a]; else for (int a = 0; a < NP; ++a) pSt[a] = p0[a];
      bool refresh = !have || stepThr < 0.0 || j != jRef;
      if (!refresh) for (int a = 0; a < NP; ++a) if (std::fabs(pEn[a] - pRef[a]) * invThr_[a] > stepThr) { refresh = true; break; }
      ++stepsN_; if (!refresh) continue;
      const typename Model::Prop& P = m.props[pjIdx_[j]]; Refresh(m, P, pEn, h); for (int a = 0; a < NP; ++a) pRef[a] = pEn[a]; have = true; jRef = j;
      eps_.emplace_back(); Epoch& e = eps_.back(); e.start = s; e.j = j; e.nA = m.nAct; e.nF = m.nFrz;
      for (int i = 0; i < NP; ++i) { e.act[i] = (unsigned char)m.actL[i]; e.frz[i] = (unsigned char)m.frzL[i]; e.piv[i] = (unsigned char)m.piv[i]; }
      if (dbl) e.LUd.assign((size_t)e.nA * e.nA, 0.0);
      e.LU.assign((size_t)e.nA * e.nA, 0.0f);
      for (int c = 0; c < e.nA; ++c)
      {
        double Fa[NP]; for (int i = 0; i < e.nA; ++i) Fa[i] = i == c ? 1.0 : 0.0;
        for (int q = 0; q < e.nA; ++q) if (m.piv[q] != q) std::swap(Fa[q], Fa[m.piv[q]]);
        for (int i = 1; i < e.nA; ++i) Fa[i] -= Model::DotF(m.LU[i], Fa, i, 0.0);
        for (int i = e.nA - 1; i >= 0; --i) Fa[i] = (Fa[i] - Model::DotF(&m.LU[i][i + 1], &Fa[i + 1], e.nA - i - 1, 0.0)) / m.LU[i][i];
        for (int i = 0; i < e.nA; ++i) { e.LU[(size_t)i * e.nA + c] = static_cast<float>(Fa[i]); if (dbl) e.LUd[(size_t)i * e.nA + c] = Fa[i]; }
      }
      if (dbl) { e.JFAd.assign((size_t)(e.nF > 0 ? e.nF : 1) * (e.nA > 0 ? e.nA : 1), 0.0); for (int f = 0; f < e.nF; ++f) for (int jj = 0; jj < e.nA; ++jj) e.JFAd[(size_t)f * e.nA + jj] = m.JFA[f][jj]; }
      e.JFA.assign((size_t)(e.nF > 0 ? e.nF : 1) * (e.nA > 0 ? e.nA : 1), 0.0f); for (int f = 0; f < e.nF; ++f) for (int jj = 0; jj < e.nA; ++jj) e.JFA[(size_t)f * e.nA + jj] = static_cast<float>(m.JFA[f][jj]);
      e.capG.assign(capG_.begin(), capG_.end()); e.ent.reserve(ent_.size()); for (const Ent& n : ent_) e.ent.push_back({n.cur, n.port, static_cast<float>(n.g)});
      CapGAt(m, pSt, h, e.capGs);
    }
  }

  void Interp(double t, size_t& seg, double* out) const
  {
    const size_t n = tE.size(); if (seg > 2) seg -= 2; else seg = 0;
    while (seg + 1 < n && tE[seg] < t) ++seg;
    const double ta = seg == 0 ? 0.0 : tE[seg - 1], tb = tE[seg]; const double* pa = seg == 0 ? p0 : &pE[(seg - 1) * NP]; const double* pb = &pE[seg * NP];
    double w = tb > ta ? (t - ta) / (tb - ta) : 1.0; if (w < 0.0) w = 0.0; if (w > 1.0) w = 1.0;
    for (int i = 0; i < NP; ++i) out[i] = pa[i] + w * (pb[i] - pa[i]);
  }
  void Setup(Model& m, const typename Model::Prop& P)
  {
    gcCols_.clear(); for (int j = 0; j < NF; ++j) { bool nz = false; for (int i = 0; i < R && !nz; ++i) if (P.GaF[(size_t)i * NF + j] != 0.0) nz = true; if (nz) gcCols_.push_back(j); }
    nGc_ = (int)gcCols_.size(); gaF_.assign((size_t)R * nGc_, 0.0); for (int i = 0; i < R; ++i) for (int j = 0; j < nGc_; ++j) gaF_[(size_t)i * nGc_ + j] = P.GaF[(size_t)i * NF + gcCols_[j]];
    capPort_.clear(); capCur_.clear(); capDev_.clear(); for (int d = 0; d < ND; ++d) if (Mdl::kDevType[d] == 6) { capPort_.push_back(Mdl::kDevPort[d]); capCur_.push_back(Mdl::kDevCur[d]); capDev_.push_back(d); }
    nCap_ = (int)capPort_.size(); wCap_.assign((size_t)NP * (nCap_ > 0 ? nCap_ : 1), 0.0); for (int a = 0; a < NP; ++a) for (int j = 0; j < nCap_; ++j) wCap_[(size_t)a * nCap_ + j] = P.Zp[(size_t)a * NF + capCur_[j]];
    capG_.assign(nCap_ > 0 ? nCap_ : 1, 0.0);
    for (int a = 0; a < NP; ++a) invThr_[a] = m.portVt[a] > 0.0 ? 1.0 / (thr * m.portVt[a]) : 1.0 / (thr * 0.17);
  }
  void Refresh(Model& m, const typename Model::Prop& P, const double* pEn, double h)
  {
    double jd[ND][6], f[NF]; for (int d = 0; d < ND; ++d) for (int e = 0; e < 6; ++e) jd[d][e] = 0.0;
    for (int i = 0; i < NP; ++i) m.ps0[i] = pEn[i]; ++m.q0Gen; m.capH = h; m.EvalF(pEn, f, jd);
    ent_.clear(); for (int d = 0; d < ND; ++d) { const int pp = Mdl::kDevPort[d], cc = Mdl::kDevCur[d], ty = Mdl::kDevType[d], npt = Model::nPortsOf(ty), ncu = Model::nCursOf(ty); for (int c = 0; c < ncu; ++c) for (int b = 0; b < npt; ++b) { const double g = jd[d][c * npt + b]; if (g != 0.0) ent_.push_back({cc + c, pp + b, g}); } }
    for (int j = 0; j < nCap_; ++j) capG_[j] = jd[capDev_[j]][0];
    // active ports: same rule as the Newton iteration of the solver
    bool act[NP]; for (int a = 0; a < NP; ++a) act[a] = false;
    for (int d = 0; d < ND; ++d)
    {
      const int pp = Mdl::kDevPort[d], cc = Mdl::kDevCur[d], ty = Mdl::kDevType[d], npt = Model::nPortsOf(ty), ncu = Model::nCursOf(ty);
      for (int b = 0; b < npt; ++b) { if (m.portPure[pp + b]) continue; if (ty >= 2 && ty != 6) { act[pp + b] = true; continue; } for (int c2 = 0; c2 < ncu; ++c2) if (P.Zmax[cc + c2] * std::fabs(jd[d][c2 * npt + b]) > m.asTol) act[pp + b] = true; }
    }
    m.nAct = 0; m.nFrz = 0; for (int a = 0; a < NP; ++a) { if (act[a]) m.actL[m.nAct++] = a; else m.frzL[m.nFrz++] = a; }
    for (int j = 0; j < m.nAct; ++j) { double* col = m.Jc[m.actL[j]]; for (int a = 0; a < NP; ++a) col[a] = 0.0; col[m.actL[j]] = 1.0; }
    for (int d = 0; d < ND; ++d)
    {
      const int pp = Mdl::kDevPort[d], cc = Mdl::kDevCur[d], ty = Mdl::kDevType[d], npt = Model::nPortsOf(ty), ncu = Model::nCursOf(ty);
      for (int b = 0; b < npt; ++b) { if (!act[pp + b]) continue; double* col = m.Jc[pp + b]; for (int c2 = 0; c2 < ncu; ++c2) { const double g = jd[d][c2 * npt + b]; if (g == 0.0) continue; Model::Axpy(col, &P.ZpT[(size_t)(cc + c2) * NP], g, NP); } }
    }
    for (int j = 0; j < m.nAct; ++j) { const double* col = m.Jc[m.actL[j]]; for (int i = 0; i < m.nAct; ++i) m.LU[i][j] = col[m.actL[i]]; for (int f = 0; f < m.nFrz; ++f) m.JFA[f][j] = col[m.frzL[f]]; }
    m.FactorActive();
  }
  // solves (I - Zp G) x = b with the active-set factorisation of the epoch (frozen ports explicitly)
  static void Solve(const Epoch& e, const double* b, double* x)
  {
    if (!e.LUd.empty() || (e.nA == 0 && !e.JFAd.empty()))
    {
      double Fa[NP], Ff[NP], xa[NP]; const int nA = e.nA, nF = e.nF;
      for (int i = 0; i < nA; ++i) Fa[i] = b[e.act[i]]; for (int f = 0; f < nF; ++f) Ff[f] = b[e.frz[f]];
      for (int i = 0; i < nA; ++i) { double sacc = 0.0; for (int j = 0; j < nA; ++j) sacc += e.LUd[(size_t)i * nA + j] * Fa[j]; xa[i] = sacc; }
      for (int f = 0; f < nF; ++f) { double sacc = 0.0; for (int j = 0; j < nA; ++j) sacc += e.JFAd[(size_t)f * nA + j] * xa[j]; Ff[f] -= sacc; }
      for (int i = 0; i < nA; ++i) x[e.act[i]] = xa[i]; for (int f = 0; f < nF; ++f) x[e.frz[f]] = Ff[f];
      return;
    }
    float Fa[NP], Ff[NP], xa[NP]; const int nA = e.nA, nF = e.nF;
    for (int i = 0; i < nA; ++i) Fa[i] = static_cast<float>(b[e.act[i]]); for (int f = 0; f < nF; ++f) Ff[f] = static_cast<float>(b[e.frz[f]]);
    MatVecF(e.LU.data(), nA, nA, Fa, nA, xa, nullptr);   // LU holds the inverse of the active block
    float tf[NP]; if (nF > 0) MatVecF(e.JFA.data(), nA, nF, xa, nA, tf, nullptr); for (int f = 0; f < nF; ++f) Ff[f] -= tf[f];
    for (int i = 0; i < nA; ++i) x[e.act[i]] = xa[i]; for (int f = 0; f < nF; ++f) x[e.frz[f]] = Ff[f];
  }
  static void ApplyG(const Epoch& e, const double* dp, double* df) { for (int j = 0; j < NF; ++j) df[j] = 0.0; for (const EntF& n : e.ent) df[n.cur] += static_cast<double>(n.g) * dp[n.port]; }
};

}}  // namespace dgg::pcmfast
