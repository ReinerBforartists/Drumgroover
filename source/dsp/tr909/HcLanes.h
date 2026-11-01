// TR-909 hand clap: the tail (after the handover at 40 ms) of NL entries of the same anchor in lock step, one lane per entry (lane batch, see LaneVec.h).
//
// After the handover the hybrid model runs on the mini set only (reduced model, 5 ports, 3 currents: one OTA and one transistor), on the 5 us grid, without sub-steps, without flips and without the
// rectified-noise correction (the plugin runs without DELTA / EMA): every lane takes the same step at the same time, only the noise differs. The step is the one of Solver::Step (HcHybrid.h):
//   zb = Ad z + G0s fS + (table terms) + noise and trigger terms; pb = PZs zb + ...; Newton on the 5 ports (residual, Jacobian I - PGs D, line search); z = zb + G1s F; output y.
// Newton runs per lane with masks (a lane that has converged keeps its iterate); the linear solves use the elimination without pivoting (J is close to the unit matrix; a lane with a small pivot or one
// that does not converge ends the batch: the caller then renders the entries with the scalar path). FMA is used, results are therefore statistically, not bit-wise, equal to the scalar tail.
// Ad (and the other propagator matrices) are used as sparse rows: entries below dropRel x the largest entry of the matrix are dropped (the error is below dropRel x the amplification of the slowest mode).
#pragma once
#include "HcHybrid.h"
#include "LaneVec.h"
#if DGG_LANES_AVAILABLE
#include <memory>
#include <vector>

namespace dgg { namespace hclanes {
using namespace dgg::lanes;

static HC_FI LV fmaS(LV a, double b, LV c) { LV r; const __m256d t = _mm256_set1_pd(b); LR r.r[k] = _mm256_fmadd_pd(a.r[k], t, c.r[k]); return r; }   // a * b + c
static HC_FI LV fmaV(LV a, LV b, LV c) { LV r; LR r.r[k] = _mm256_fmadd_pd(a.r[k], b.r[k], c.r[k]); return r; }
static HC_FI LM ltV(LV a, LV b) { LM r; LR r.r[k] = _mm256_cmp_pd(a.r[k], b.r[k], _CMP_LT_OQ); return r; }
static HC_FI LM lmAnd(LM a, LM b) { LM r; LR r.r[k] = _mm256_and_pd(a.r[k], b.r[k]); return r; }
static HC_FI LM lmOr(LM a, LM b) { LM r; LR r.r[k] = _mm256_or_pd(a.r[k], b.r[k]); return r; }
static HC_FI LM lmNot(LM a) { LM r; const __m256d t = _mm256_cmp_pd(_mm256_setzero_pd(), _mm256_setzero_pd(), _CMP_EQ_OQ); LR r.r[k] = _mm256_xor_pd(a.r[k], t); return r; }
static HC_FI LV zeroV() { return bc(0.0); }

class MiniTail
{
public:
  static constexpr int kMaxR = 64, kMaxNP = 8, kMaxNS = 8;
  MiniTail(const hch::Red& MF, const hch::Red& MQ, const std::vector<char>& quiet, double dropRel = 1e-12) : MF_(MF), M_(MQ), quiet_(quiet), dropRel_(dropRel) {}

  // Start the lanes from entries (handover states) of this anchor at their handover time; seeds: the noise sequence of the tail of every lane. false: not suited (the remainder is not mini-only, the
  // dimensions do not fit, an entry has no state); the caller renders with the scalar path then.
  bool Begin(int n, const hch::HcHandover* const* ho, const long long* tailSeed, double fsHost, double idle)
  {
    if (n < 1 || n > NL || M_.R > kMaxR || M_.NP > kMaxNP || M_.NS > kMaxNS || M_.lev.empty()) return false;
    for (const hch::Dev& v : M_.dev) if (v.kind > 1) return false;   // OTA and transistor only
    n_ = n; R_ = M_.R; NP_ = M_.NP; NS_ = M_.NS; dt_ = M_.dt; idle_ = idle;
    t0_ = std::round(ho[0]->time / dt_) * dt_; k0_ = static_cast<long>(std::llround(t0_ / dt_));
    for (int l = 1; l < n; ++l) if (std::fabs(ho[l]->time - ho[0]->time) > 1e-12) return false;
    for (long k = k0_; k <= M_.n; ++k) if (!quiet_[static_cast<size_t>(std::min<long>(k, MF_.n))]) return false;   // the tail must be mini-only
    for (int l = 0; l < n; ++l) if (ho[l]->z.size() != static_cast<size_t>(R_) || ho[l]->fS.size() < static_cast<size_t>(NS_) || ho[l]->pprev.size() < static_cast<size_t>(NP_)) return false;
    BuildSparse();
    for (int i = 0; i < R_; ++i) { double zl[NL], zero = 0; for (int l = 0; l < NL; ++l) zl[l] = ho[std::min(l, n - 1)]->z[i]; Z_[i] = Load(zl); (void)zero; }
    for (int j = 0; j < NS_; ++j) { double v[NL]; for (int l = 0; l < NL; ++l) v[l] = ho[std::min(l, n - 1)]->fS[j]; FS_[j] = Load(v); }
    for (int a = 0; a < NP_; ++a) { double v[NL]; for (int l = 0; l < NL; ++l) v[l] = ho[std::min(l, n - 1)]->pprev[a]; P_[a] = Load(v); }
    for (int l = 0; l < n; ++l)
    {
      nz_[l].Start(static_cast<uint64_t>(tailSeed[l]), 5e-6, 0.0); dec_[l].Init(fsHost); dec_[l].Restart(0.0); host_[l].clear();
      for (size_t e = 0; e < ho[l]->et.size(); ++e) { dec_[l].Push(ho[l]->et[e], ho[l]->ey[e] - idle); double v; while (dec_[l].Pop(v)) host_[l].push_back(static_cast<float>(v)); }   // the events of the head
    }
    { double na[NL]; for (int l = 0; l < NL; ++l) na[l] = nz_[std::min(l, n - 1)].At(0.05 + t0_); Na_ = Load(na); }
    k_ = k0_; failed_ = false; return true;
  }
  // Runs the grid steps up to the time tEnd (s after the trigger). false: a lane failed (the batch is unusable from here).
  bool Run(double tEnd)
  {
    while (!failed_ && static_cast<double>(k_) * dt_ < tEnd - 1e-12 && k_ < M_.n) Step();
    return !failed_;
  }
  const std::vector<float>& Host(int lane) const { return host_[lane]; }
  long Steps() const { return steps_; }
  long NewtonIterations() const { return its_; }

private:
  static HC_FI LV Load(const double* v) { LV r; LR r.r[k] = _mm256_loadu_pd(v + 4 * k); return r; }

  struct Row { std::vector<int> j; std::vector<double> a; };
  void BuildSparse()
  {
    const hch::Lev& L = M_.lev[0]; double mx = 0; for (double v : L.Ad) mx = std::max(mx, std::fabs(v));
    ad_.assign(static_cast<size_t>(R_), Row()); const double thr = dropRel_ * mx; nnz_ = 0;
    for (int i = 0; i < R_; ++i) for (int j = 0; j < R_; ++j) { const double v = L.Ad[static_cast<size_t>(i) * R_ + j]; if (std::fabs(v) > thr) { ad_[static_cast<size_t>(i)].j.push_back(j); ad_[static_cast<size_t>(i)].a.push_back(v); ++nnz_; } }
  }

  // currents F (NS) and slopes D (NS x NP) of the devices at the ports P: the OTA (p0, p1, p2) and the transistor (ports po, po + 1)
  void Fsel(const LV* P, LV* F, LV (*D)[kMaxNP]) const
  {
    for (int j = 0; j < NS_; ++j) { F[j] = zeroV(); for (int b = 0; b < NP_; ++b) D[j][b] = zeroV(); }
    for (const hch::Dev& v : M_.dev)
    {
      if (v.kind == 0)
      {
        const LV ictl = P[1] * M_.gb + P[2] * M_.gc, th = vtanh(P[0]); F[v.co] = ictl * th; D[0][0] = ictl * (1.0 - th * th); D[0][1] = th * M_.gb; D[0][2] = th * M_.gc;
      }
      else
      {
        const double* Pm = &M_.bp[16 * static_cast<size_t>(v.idx)]; const LV vbe = P[v.po], vbc = P[v.po + 1];
        const double s = Pm[0], IS = Pm[1], ISE = Pm[8], invNFVt = Pm[9], invNEVt = Pm[10], invVAF = Pm[11], invIKF = Pm[12], invBF = Pm[13], invBR = Pm[14];
        const LV aE = vbe * invNFVt, aC = vbc * (1.0 / pcmfast::kVt);
        const LV eE = sel(lt(aE, -40.0), zeroV(), vexp(vmin(aE, bc(80.0)))), eC = sel(lt(aC, -40.0), zeroV(), vexp(vmin(aC, bc(80.0))));
        const LV cbe = IS * (eE - 1.0) + pcmfast::kGmin * vbe, gbe = IS * eE * invNFVt + pcmfast::kGmin;
        const LV cbc = IS * (eC - 1.0) + pcmfast::kGmin * vbc, gbc = IS * eC * (1.0 / pcmfast::kVt) + pcmfast::kGmin;
        LV cben = zeroV(), gben = zeroV();
        if (ISE != 0.0) { const LV aL = vbe * invNEVt; const LV eL = sel(lt(aL, -40.0), zeroV(), vexp(vmin(aL, bc(80.0)))); cben = ISE * (eL - 1.0); gben = ISE * eL * invNEVt; }
        const LV q1 = 1.0 / (1.0 - vbc * invVAF), dq1 = q1 * q1 * invVAF;
        const LV q2 = cbe * invIKF, arg = vmax(zeroV(), 1.0 + 4.0 * q2), sq = vsqrt(arg), qb = q1 * (1.0 + sq) * 0.5;
        const LV dsqbe = sel(gt(arg, 0.0), (2.0 * gbe * invIKF) / vmax(sq, bc(1e-12)), zeroV());
        const LV dqbe = q1 * 0.5 * dsqbe, dqbc = dq1 * (1.0 + sq) * 0.5;
        const LV iq = 1.0 / qb, x = cbe - cbc;
        const LV cc = x * iq - cbc * invBR, cb = cbe * invBF + cben + cbc * invBR;
        const LV dccbe = gbe * iq - x * iq * iq * dqbe, dccbc = -gbc * iq - x * iq * iq * dqbc - gbc * invBR;
        const LV dcbbe = gbe * invBF + gben, dcbbc = gbc * invBR;
        F[v.co] = s * cc; F[v.co + 1] = s * cb; D[v.co][v.po] = s * dccbe; D[v.co][v.po + 1] = s * dccbc; D[v.co + 1][v.po] = s * dcbbe; D[v.co + 1][v.po + 1] = s * dcbbc;
      }
    }
  }
  // g = p - pb - PGs F, scaled residual norm per lane (largest over the ports)
  LV Resid(const LV* P, const LV* PB, const double* PGs, LV* F, LV (*D)[kMaxNP], LV* g) const
  {
    Fsel(P, F, D); LV r = zeroV();
    for (int a = 0; a < NP_; ++a)
    {
      LV s = P[a] - PB[a]; for (int j = 0; j < NS_; ++j) s = fmaS(F[j], -PGs[static_cast<size_t>(a) * NS_ + j], s);
      g[a] = s; r = vmax(r, vabs(s) / ((1e-6 + 1e-9 * vabs(P[a])) ));
    }
    return r;
  }

  void Step()
  {
    const hch::Lev& L = M_.lev[0]; const long ia = std::min<long>(k_, M_.n - 1); const double ta = static_cast<double>(k_) * dt_, tb = ta + dt_;
    const double ta_ = hch::TrigAt(ta), tb_ = hch::TrigAt(tb), dtra = (hch::TrigAt((ia + 1) * dt_) - hch::TrigAt(ia * dt_)) / dt_;
    const double* fg = M_.FMGU.at(static_cast<int>(ia)); const double* pbr = M_.PBU.at(static_cast<int>(ia)); const double yu = M_.YU.at(static_cast<int>(ia))[1];
    double nbv[NL]; for (int l = 0; l < NL; ++l) nbv[l] = nz_[std::min(l, n_ - 1)].At(0.05 + tb);
    const LV Nb = Load(nbv), Dna = (Nb - Na_) * (1.0 / dt_);
    // zb
    LV ZB[kMaxR];
    for (int i = 0; i < R_; ++i)
    {
      const double* pz = &L.Pz[static_cast<size_t>(i) * 4];
      const double sc = fg[i] + L.G0t[i] * ta_ + L.G1t[i] * tb_ + pz[2] * dtra + pz[0] * M_.ACC + pz[3];
      LV acc = bc(sc); acc = fmaS(Na_, L.G0n[i], acc); acc = fmaS(Nb, L.G1n[i], acc); acc = fmaS(Dna, pz[1], acc);
      const Row& r = ad_[static_cast<size_t>(i)]; const size_t nr = r.j.size(); const int* jj = r.j.data(); const double* aa = r.a.data();
      LV a2 = zeroV(); size_t q = 0; for (; q + 1 < nr; q += 2) { acc = fmaS(Z_[jj[q]], aa[q], acc); a2 = fmaS(Z_[jj[q + 1]], aa[q + 1], a2); } if (q < nr) acc = fmaS(Z_[jj[q]], aa[q], acc);
      acc = acc + a2;
      for (int j = 0; j < NS_; ++j) acc = fmaS(FS_[j], L.G0s[static_cast<size_t>(i) * NS_ + j], acc);
      ZB[i] = acc;
    }
    LV PB[kMaxNP];
    for (int a = 0; a < NP_; ++a)
    {
      const double* pu = &M_.PU[static_cast<size_t>(a) * 5]; const double* pz = &M_.PZs[static_cast<size_t>(a) * R_];
      LV s = bc(pu[0] * M_.ACC + pu[3] * tb_ + pu[4] * dtra + pbr[NP_ + a]); s = fmaS(Nb, pu[1], s); s = fmaS(Dna, pu[2], s);
      LV s2 = zeroV(); int j = 0; for (; j + 1 < R_; j += 2) { s = fmaS(ZB[j], pz[j], s); s2 = fmaS(ZB[j + 1], pz[j + 1], s2); } if (j < R_) s = fmaS(ZB[j], pz[j], s);
      PB[a] = s + s2;
    }
    // Newton on the ports, per lane with masks
    const double* PGs = L.PGs.data(); LV P[kMaxNP], F[kMaxNS], G[kMaxNP]; LV D[kMaxNS][kMaxNP];
    for (int a = 0; a < NP_; ++a) P[a] = P_[a];
    LV rn = Resid(P, PB, PGs, F, D, G); LM done = lt(rn, 1.0);
    int it = 0; const int maxIt = 60;
    for (; it < maxIt && !allLane(done); ++it)
    {
      // J = I - PGs D, solve J dp = G (elimination without pivoting)
      LV J[kMaxNP][kMaxNP], dp[kMaxNP];
      for (int a = 0; a < NP_; ++a) { for (int b = 0; b < NP_; ++b) { LV s = bc(a == b ? 1.0 : 0.0); for (int j = 0; j < NS_; ++j) s = fmaS(D[j][b], -PGs[static_cast<size_t>(a) * NS_ + j], s); J[a][b] = s; } dp[a] = G[a]; }
      bool singular = false;
      for (int c = 0; c < NP_ && !singular; ++c)
      {
        if (anyLane(lt(vabs(J[c][c]), 1e-9))) { singular = true; break; }
        const LV inv = 1.0 / J[c][c];
        for (int r = c + 1; r < NP_; ++r) { const LV f = J[r][c] * inv; for (int k = c; k < NP_; ++k) J[r][k] = fmaV(f, -J[c][k], J[r][k]) ; dp[r] = fmaV(f, -dp[c], dp[r]); }
      }
      if (singular) { failed_ = true; return; }
      for (int r = NP_ - 1; r >= 0; --r) { LV s = dp[r]; for (int k = r + 1; k < NP_; ++k) s = fmaV(J[r][k], -dp[k], s); dp[r] = s / J[r][r]; }
      // limit the largest change of the junction ports to 0.04 V
      LV dpm = zeroV(); for (int a = 0; a < NP_; ++a) if (M_.jp[static_cast<size_t>(a)]) dpm = vmax(dpm, vabs(dp[a]));
      const LM big = gt(dpm, 0.04); const LV sc = sel(big, 0.04 / vmax(dpm, bc(1e-300)), bc(1.0)); for (int a = 0; a < NP_; ++a) dp[a] = dp[a] * sc;
      // line search per lane: lam = 1, 1/2, ... until the residual falls
      LV lam = bc(1.0), rt, PT[kMaxNP], FT[kMaxNS], GT[kMaxNP], DT[kMaxNS][kMaxNP]; LM accepted = done;   // converged lanes take no part
      LV Psel[kMaxNP], Fsv[kMaxNS], Gsv[kMaxNP], Dsv[kMaxNS][kMaxNP], rtSel = rn;
      for (int a = 0; a < NP_; ++a) Psel[a] = P[a]; for (int j = 0; j < NS_; ++j) Fsv[j] = F[j]; for (int a = 0; a < NP_; ++a) Gsv[a] = G[a]; for (int j = 0; j < NS_; ++j) for (int b = 0; b < NP_; ++b) Dsv[j][b] = D[j][b];
      for (int ls = 0; ls < 12; ++ls)
      {
        for (int a = 0; a < NP_; ++a) PT[a] = fmaV(lam, -dp[a], P[a]);
        rt = Resid(PT, PB, PGs, FT, DT, GT);
        const LM okLam = lmOr(ltV(rt, rn * (bc(1.0) - lam * 1e-4)), lt(rt, 1.0));   // accepted at this lam: rt < rn (1 - 1e-4 lam) or rt < 1
        const LM take = lmAnd(okLam, lmNot(accepted));   // lanes that are accepted now and were not before
        for (int a = 0; a < NP_; ++a) { Psel[a] = sel(take, PT[a], Psel[a]); Gsv[a] = sel(take, GT[a], Gsv[a]); }
        for (int j = 0; j < NS_; ++j) { Fsv[j] = sel(take, FT[j], Fsv[j]); for (int b = 0; b < NP_; ++b) Dsv[j][b] = sel(take, DT[j][b], Dsv[j][b]); }
        rtSel = sel(take, rt, rtSel); accepted = lmOr(accepted, okLam);
        if (allLane(accepted)) break;
        lam = sel(accepted, lam, lam * 0.5);
      }
      // lanes that never found a descent step take the last trial (as the scalar code does)
      const LM left = lmNot(accepted);
      if (anyLane(left)) { for (int a = 0; a < NP_; ++a) { Psel[a] = sel(left, PT[a], Psel[a]); Gsv[a] = sel(left, GT[a], Gsv[a]); } for (int j = 0; j < NS_; ++j) { Fsv[j] = sel(left, FT[j], Fsv[j]); for (int b = 0; b < NP_; ++b) Dsv[j][b] = sel(left, DT[j][b], Dsv[j][b]); } rtSel = sel(left, rt, rtSel); }
      for (int a = 0; a < NP_; ++a) { P[a] = Psel[a]; G[a] = Gsv[a]; } for (int j = 0; j < NS_; ++j) { F[j] = Fsv[j]; for (int b = 0; b < NP_; ++b) D[j][b] = Dsv[j][b]; }
      rn = rtSel; done = lt(rn, 1.0);
    }
    its_ += it + 1; ++steps_;
    if (!allLane(done) || it >= maxIt) { failed_ = true; return; }
    // state update and output
    LV ZN[kMaxR];
    for (int i = 0; i < R_; ++i) { LV s = ZB[i]; for (int j = 0; j < NS_; ++j) s = fmaS(F[j], L.G1s[static_cast<size_t>(i) * NS_ + j], s); ZN[i] = s; }
    LV y = bc(M_.Do1[0] + M_.Dou[0] * M_.ACC + M_.Dou[3] * tb_ + M_.Dou[4] * dtra + yu); y = fmaS(Nb, M_.Dou[1], y); y = fmaS(Dna, M_.Dou[2], y);
    for (int i = 0; i < R_; ++i) y = fmaS(ZN[i], M_.Xo[static_cast<size_t>(i)], y);
    for (int j = 0; j < NS_; ++j) y = fmaS(F[j], M_.Dofsel[static_cast<size_t>(j)], y);
    for (int i = 0; i < R_; ++i) Z_[i] = ZN[i];
    for (int j = 0; j < NS_; ++j) FS_[j] = F[j];
    for (int a = 0; a < NP_; ++a) P_[a] = P[a];
    Na_ = Nb;
    double yl[NL]; for (int k = 0; k < KR; ++k) _mm256_storeu_pd(yl + 4 * k, y.r[k]);
    for (int l = 0; l < n_; ++l) { dec_[l].Push(tb, yl[l] - idle_); double v; while (dec_[l].Pop(v)) host_[l].push_back(static_cast<float>(v)); }
    ++k_;
  }

  const hch::Red& MF_; const hch::Red& M_; const std::vector<char>& quiet_; double dropRel_;
  int n_ = 0, R_ = 0, NP_ = 0, NS_ = 0; double dt_ = 5e-6, t0_ = 0.0, idle_ = 0.0; long k0_ = 0, k_ = 0, nnz_ = 0, steps_ = 0, its_ = 0; bool failed_ = false;
  std::vector<Row> ad_;
  LV Z_[kMaxR], FS_[kMaxNS], P_[kMaxNP], Na_;
  pcmfast::HitNoise nz_[NL]; pcmfast::HostDecimator dec_[NL]; std::vector<float> host_[NL];
};

}}  // namespace dgg::hclanes
#endif
