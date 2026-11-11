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
#include "FullFast2.h"

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
  size_t EpochBytes() const { size_t b = 0; for (const Epoch& e : eps_) b += sizeof(Epoch) + (e.LU.capacity() + e.JFA.capacity() + e.capG.capacity()) * 4 + e.ent.capacity() * sizeof(EntF); return b; }
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
  }
  // State of one hit (the tables of the epochs and the model are shared and not modified by a hit): several hits can run at the same time on one primed object, each with its own cursor.
  struct Cursor
  {
    HitNoise* nz = nullptr; double tOff = 0.0, nPrev = 0.0; long N = 0, k = 0; size_t ep = 0; const void* cur = nullptr; long refreshes = 0, knots = 0;
    double capGp[NF] = {0}; bool capGpOk = false; double dz[R] = {0}, dp[NP] = {0}, dfPrev[NF] = {0}; float dzF[R] = {0}, dpF[NP] = {0}, dfPrevF[NF] = {0};
  };
  void Start(HitNoise& nz, double tOff, double tEnd) { Start(cur0_, nz, tOff, tEnd); }
  bool Next(double& tOut, double& dyOut) { return Next(cur0_, tOut, dyOut); }
  long RefreshCount() const { return cur0_.refreshes; } long KnotCount() const { return cur0_.knots; }
  void Start(Cursor& C, HitNoise& nz, double tOff, double tEnd)
  {
    C.nz = &nz; C.tOff = tOff; C.N = std::min(static_cast<long>(tEnd / P_->h), epochKnots_); C.k = 0; C.ep = 0; C.cur = nullptr;
    for (int i = 0; i < R; ++i) C.dz[i] = 0.0; for (int a = 0; a < NP; ++a) C.dp[a] = 0.0; for (int j = 0; j < NF; ++j) C.dfPrev[j] = 0.0;
    C.nPrev = (coarse > 1 ? nz.AtCoarse(tOff, coarse, eq) : nz.At(tOff)) - 7.5; for (int i = 0; i < R; ++i) C.dzF[i] = 0.0f; for (int a = 0; a < NP; ++a) C.dpF[a] = 0.0f; for (int j = 0; j < NF; ++j) C.dfPrevF[j] = 0.0f;
  }
  bool Next(Cursor& C, double& tOut, double& dyOut)
  {
    if (C.k >= C.N) return false;
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
  struct Epoch { long start = 0; int j = 5; std::vector<float> capGs; int nA = 0, nF = 0; unsigned char act[NP], frz[NP], piv[NP]; std::vector<float> LU, JFA, capG; std::vector<double> LUd, JFAd; std::vector<EntF> ent; };   // LUd / JFAd: double precision inverse of the active block and frozen coupling (dbl)
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
