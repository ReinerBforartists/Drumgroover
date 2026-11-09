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
  void Clear(const double* ports0) { tE.clear(); pE.clear(); for (int a = 0; a < NP; ++a) p0[a] = ports0[a]; }
  void Record(double t, const double* p) { tE.push_back(t); pE.insert(pE.end(), p, p + NP); }
  long refreshes = 0, knots = 0; double thr = 0.3, laterMul = 12.0; bool refine = false;   // one step of iterative refinement with the full G (no effect on the result at the tested settings)   // G is re-evaluated when a port of the noise-free trajectory moved by thr x its thermal voltage (thr x 0.17 V without one)

  // Runs the knots 1 .. tEnd / h of the noise source `nz` (value at time tOff + t, idle level 7.5 V subtracted); emit(t, dy) is called once per knot with the output deviation.
  template <class Emit>
  void Run(Model& m, int pi, HitNoise& nz, double tOff, double tEnd, Emit&& emit)
  {
    const typename Model::Prop& P = m.props[pi]; const double h = P.h; const long N = static_cast<long>(tEnd / h);
    m.qlOn = false; m.qlcOn = false; m.capThr = 0.0; m.evCache = false; m.skipDev = nullptr;
    Setup(m, P);
    double dz[R] = {0}, dzk[R], df0c[NF] = {0}, dp[NP] = {0}, pS[NP], pEn[NP], pRef[NP], dfk[NF] = {0}, dfkc[NF], rhs[NP], dpk[NP], res[NP], dfG[NF], df1[NF], vin[R], vpd[NP], zt[NP];
    size_t seg = 0; bool have = false;
    Interp(0.0, seg, pS);
    double nPrev = nz.At(tOff) - 7.5; const double* Phi = P.Phi.data();
    for (long k = 0; k < N; ++k)
    {
      const double d0 = nPrev, d1 = nz.At(tOff + (k + 1) * h) - 7.5, dd = (d1 - d0) / h; nPrev = d1;
      Interp((k + 1) * h, seg, pEn);
      bool refresh = !have;
      if (!have) for (int a = 0; a < NP; ++a) pRef[a] = pEn[a];
      else for (int a = 0; a < NP; ++a) if (std::fabs(pEn[a] - pRef[a]) * invThr_[a] > (k * h < 1e-3 ? 1.0 : laterMul)) { refresh = true; break; }
      if (refresh) { Refresh(m, P, pEn, h); for (int a = 0; a < NP; ++a) pRef[a] = pEn[a]; have = true; ++refreshes; }
      for (int i = 0; i < R; ++i) vin[i] = P.Ga[i * NU + 1] * d0 + P.Gb[i * NU + 1] * d1 + (P.Ga[i * NU + 2] + P.Gb[i * NU + 2]) * dd;
      for (int a = 0; a < NP; ++a) vpd[a] = m.PD[a * NU + 1] * d1 + m.PD[a * NU + 2] * dd;
      for (int j = 0; j < nGc_; ++j) df0c[j] = dfPrev_[gcCols_[j]];
      Model::MatVec(Phi, R, R, dz, R, dzk, vin);
      Model::MatVec(gaF_.data(), nGc_, R, df0c, nGc_, dzk, dzk);
      Model::MatVec(P.PXz.data(), R, NP, dzk, R, dpk, vpd);
      // start-of-step part of the charge-device currents
      for (int j = 0; j < nCap_; ++j) dfkc[j] = -capG_[j] * dp[capPort_[j]];
      Model::MatVec(wCap_.data(), nCap_, NP, dfkc, nCap_, rhs, dpk);
      Solve(m, rhs, dp);
      if (refine)
      {
        ApplyG(dp, dfG);   // full G incl. the frozen ports
        Model::MatVec(P.Zp.data(), NF, NP, dfG, NF, res, rhs);
        for (int a = 0; a < NP; ++a) res[a] -= dp[a];
        Solve(m, res, zt); for (int a = 0; a < NP; ++a) dp[a] += zt[a];
      }
      ApplyG(dp, df1); for (int c = 0; c < nCap_; ++c) df1[capCur_[c]] += dfkc[c];
      Model::MatVec(P.GbF.data(), NF, R, df1, NF, dz, dzk);
      double dy = m.pm.Dou[1] * d1 + m.pm.Dou[2] * dd + Model::DotF(m.pm.Xo, dz, R, 0.0); dy = Model::DotF(m.pm.Dof, df1, NF, dy);
      for (int j = 0; j < NF; ++j) dfPrev_[j] = df1[j];
      for (int a = 0; a < NP; ++a) pS[a] = pEn[a];
      emit((k + 1) * h, dy); ++knots;
    }
  }

private:
  struct Ent { int cur, port; double g; };
  std::vector<Ent> ent_; std::vector<double> gaF_, wCap_, capG_; std::vector<int> gcCols_, capPort_, capCur_, capDev_; int nGc_ = 0, nCap_ = 0; double invThr_[NP]; double dfPrev_[NF] = {0};

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
    for (int j = 0; j < NF; ++j) dfPrev_[j] = 0.0;
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
  // solves (I - Zp G) x = b with the active-set factorisation (frozen ports explicitly)
  void Solve(Model& m, const double* b, double* x) const
  {
    double Fa[NP], Ff[NP]; const int nA = m.nAct, nF = m.nFrz;
    for (int i = 0; i < nA; ++i) Fa[i] = b[m.actL[i]]; for (int f = 0; f < nF; ++f) Ff[f] = b[m.frzL[f]];
    for (int c = 0; c < nA; ++c) if (m.piv[c] != c) std::swap(Fa[c], Fa[m.piv[c]]);
    for (int i = 1; i < nA; ++i) Fa[i] -= Model::DotF(m.LU[i], Fa, i, 0.0);
    for (int i = nA - 1; i >= 0; --i) Fa[i] = (Fa[i] - Model::DotF(&m.LU[i][i + 1], &Fa[i + 1], nA - i - 1, 0.0)) / m.LU[i][i];
    double tf[NP]; Model::MatVec(&m.JFA[0][0], NP, nF, Fa, nA, tf); for (int f = 0; f < nF; ++f) Ff[f] -= tf[f];
    for (int i = 0; i < nA; ++i) x[m.actL[i]] = Fa[i]; for (int f = 0; f < nF; ++f) x[m.frzL[f]] = Ff[f];
  }
  void ApplyG(const double* dp, double* df) const { for (int j = 0; j < NF; ++j) df[j] = 0.0; for (const Ent& e : ent_) df[e.cur] += e.g * dp[e.port]; }
};

}}  // namespace dgg::pcmfast
