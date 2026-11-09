// FullFast2 in AVX2 lane form for the cache bake: four hits (noise seeds) of one voice and anchor run in lockstep (tools handoff R392, LT-K01).
//
// One model and one step size per lockstep step (the smallest any lane asks for); per lane: model state, noise input, device evaluation, Newton matrix and its LU (row pivots per lane).
// The devices are evaluated exactly (no quasi-linear devices), the propagators and constants come from FullFast2 (same model, same tuning). The step control is HitRunner::Advance.
// Results agree with the scalar engine within the accuracy gate of the voice (R115), not bit for bit (FMA, lockstep step sizes). Needs an x64 MSVC build and a CPU with AVX2 and FMA:
// call it only after the run-time check of the machine (HitLanesApi.h). Not thread safe: one object per worker thread.
#pragma once
#if defined(_MSC_VER) && defined(_M_X64) && !defined(DGG_NO_LANES)
#define DGG_LANES_BUILD 1
#include "HitRunner.h"
#include <immintrin.h>
#include <intrin.h>
#include <algorithm>
#include <cstdio>
#include <memory>
#include <vector>
namespace dgg { namespace pcmfast { namespace lanes {

struct V
{
  __m256d v;
  V() {}
  V(__m256d x) : v(x) {}
  V(double s) : v(_mm256_set1_pd(s)) {}
};
static inline V operator+(V a, V b) { return _mm256_add_pd(a.v, b.v); }
static inline V operator-(V a, V b) { return _mm256_sub_pd(a.v, b.v); }
static inline V operator*(V a, V b) { return _mm256_mul_pd(a.v, b.v); }
static inline V operator/(V a, V b) { return _mm256_div_pd(a.v, b.v); }
static inline V operator-(V a) { return _mm256_xor_pd(a.v, _mm256_set1_pd(-0.0)); }
static inline V vabs(V a) { return _mm256_andnot_pd(_mm256_set1_pd(-0.0), a.v); }
static inline V vmin(V a, V b) { return _mm256_min_pd(a.v, b.v); }
static inline V vmax(V a, V b) { return _mm256_max_pd(a.v, b.v); }
static inline V vsqrt(V a) { return _mm256_sqrt_pd(a.v); }
static inline V vexp(V a) { return _mm256_exp_pd(a.v); }
static inline V vlog(V a) { return _mm256_log_pd(a.v); }
static inline V vtanh(V a) { return _mm256_tanh_pd(a.v); }
static inline V vpow(V a, V b) { return _mm256_pow_pd(a.v, b.v); }
static inline V lt(V a, V b) { return _mm256_cmp_pd(a.v, b.v, _CMP_LT_OQ); }
static inline V gt(V a, V b) { return _mm256_cmp_pd(a.v, b.v, _CMP_GT_OQ); }
static inline V le(V a, V b) { return _mm256_cmp_pd(a.v, b.v, _CMP_LE_OQ); }
static inline V ge(V a, V b) { return _mm256_cmp_pd(a.v, b.v, _CMP_GE_OQ); }
static inline V vand(V a, V b) { return _mm256_and_pd(a.v, b.v); }
static inline V sel(V m, V a, V b) { return _mm256_blendv_pd(b.v, a.v, m.v); }   // lane of m set: a, else b
static inline int mask(V m) { return _mm256_movemask_pd(m.v); }
static inline double get(V a, int l) { alignas(32) double t[4]; _mm256_store_pd(t, a.v); return t[l]; }
static inline V make(const double* d4) { return _mm256_loadu_pd(d4); }
static inline V fma(V a, V b, V c) { return _mm256_fmadd_pd(a.v, b.v, c.v); }      // a*b + c
static inline V fnma(V a, V b, V c) { return _mm256_fnmadd_pd(a.v, b.v, c.v); }    // c - a*b
static inline V bc(const double* p) { return _mm256_broadcast_sd(p); }
// y[i] = init[i] + sum_j Mat[i*ld+j] x[j], i < rows (four rows interleaved, y may alias init)
static inline void MV(const double* Mat, int ld, int rows, const V* x, int n, V* y, const V* init)
{
  int i = 0;
  for (; i + 4 <= rows; i += 4)
  {
    V a0 = init ? init[i] : V(0.0), a1 = init ? init[i + 1] : V(0.0), a2 = init ? init[i + 2] : V(0.0), a3 = init ? init[i + 3] : V(0.0);
    const double* m0 = Mat + (size_t)i * ld; const double* m1 = m0 + ld; const double* m2 = m1 + ld; const double* m3 = m2 + ld;
    for (int j = 0; j < n; ++j) { const V xj = x[j]; a0 = fma(bc(m0 + j), xj, a0); a1 = fma(bc(m1 + j), xj, a1); a2 = fma(bc(m2 + j), xj, a2); a3 = fma(bc(m3 + j), xj, a3); }
    y[i] = a0; y[i + 1] = a1; y[i + 2] = a2; y[i + 3] = a3;
  }
  for (; i < rows; ++i) { V a = init ? init[i] : V(0.0); const double* m0 = Mat + (size_t)i * ld; for (int j = 0; j < n; ++j) a = fma(bc(m0 + j), x[j], a); y[i] = a; }
}
// dot product of two lane vectors with four accumulators
static inline V DotV(const V* a, const V* b, int n)
{
  V s0(0.0), s1(0.0), s2(0.0), s3(0.0); int k = 0;
  for (; k + 4 <= n; k += 4) { s0 = fma(a[k], b[k], s0); s1 = fma(a[k + 1], b[k + 1], s1); s2 = fma(a[k + 2], b[k + 2], s2); s3 = fma(a[k + 3], b[k + 3], s3); }
  for (; k < n; ++k) s0 = fma(a[k], b[k], s0);
  return (s0 + s1) + (s2 + s3);
}

template <class M> struct Eng
{
  using S = FullFast2<M>;
  static constexpr int R = S::R, NF = S::NF, NP = S::NP, NS = S::NS, ND = S::ND, NU = S::NU, NT = S::NT;
  static constexpr int NL = 4;
  S sc;
  V z[R], f0[NF], p[NP], pk[NP], Fv[NP], f1[NF], zk[R], ps0[NP], q0v[ND > 0 ? ND : 1], jd[ND > 0 ? ND : 1][6];
  std::vector<V> LU, JFA; int piv_[1]; int actL[NP], frzL[NP], nAct = 0, nFrz = 0; V pRef[NP];
  double acc = M::kAcc, capH = 1.0, asTol = 1e-2, newtonTol = 3e-5, dampRatio = 0.01, dampMin = 1e-3; int maxIt = 50, maxDepth = 6;
  long steps = 0, its = 0, facs = 0, fails = 0, retries = 0, pivotFail = 0;
  std::vector<std::pair<int, int>> portDevs[NP]; bool luOk = false; int luProp = -1; double chordFirst = 5.0; int refacMin = 1, frzMin = 3; bool badPivot = false; int dbgLeft = 0; int pivL[NP][4]; long swaps = 0;

  void Init()
  {
    for (int d = 0; d < ND; ++d) { const int pp = M::kDevPort[d], npt = nPorts(M::kDevType[d]); for (int b = 0; b < npt; ++b) portDevs[pp + b].push_back({d, b}); }
    sc.Init(); sc.Reset(M::kAcc); acc = M::kAcc; LU.assign(NP * NP, V(0.0)); JFA.assign(NP * NP, V(0.0));
    for (int d = 0; d < ND; ++d) for (int k = 0; k < 6; ++k) jd[d][k] = V(0.0); for (int a = 0; a < NP; ++a) pRef[a] = V(0.0); for (int c = 0; c < NP; ++c) for (int l = 0; l < 4; ++l) pivL[c][l] = c;
    for (int i = 0; i < R; ++i) z[i] = V(sc.z[i]);
    for (int i = 0; i < NF; ++i) f0[i] = V(sc.f0[i]);
    for (int i = 0; i < NP; ++i) p[i] = V(sc.p[i]);
  }
  double yIdle() const { return sc.yIdle; }

  // ---- lane device evaluation -------------------------------------------------------------------------------------------------------------------------------------------------------
  static inline void Junction(const double* q, double nvt, double xbv, V vd, V& ii, V& gg)
  {
    const double Is = q[0];
    const double inv = 1.0 / nvt; const V e1 = vexp(vmin(vd * V(inv), V(80.0)));
    ii = V(Is) * (e1 - V(1.0)) + V(kGmin) * vd; gg = V(Is * inv) * e1 + V(kGmin);
    if (xbv < 1e29)
    {
      const V rev = le(vd, V(-xbv));   // vd <= -xbv: breakdown branch
      if (mask(rev))
      {
        const V e2 = vexp(vmin(-(V(xbv) + vd) / V(nvt), V(80.0)));
        const V i2 = -V(Is) * e2 + V(kGmin) * vd, g2 = V(Is) * e2 / V(nvt) + V(kGmin);
        ii = sel(rev, i2, ii); gg = sel(rev, g2, gg);
      }
    }
  }
  inline void CapQ(const double* r, V v, V& q, V& c, bool wantC)
  {
    const int kind = (int)r[0]; const double C0 = r[2], cj0 = r[3], vj = r[4], m = r[5], tt = r[6];
    V qj(0.0), cj(0.0);
    if (cj0 > 0.0)
    {
      const double fc = 0.5; const V low = lt(v, V(fc * vj));
      V ql(0.0), cl(0.0), qh(0.0), ch(0.0);
      if (mask(low))
      {
        const V t = V(1.0) - v / V(vj);
        const V pm = m == 0.5 ? vsqrt(t) : vpow(t, V(m));   // t^m once; t^(1-m) = t / t^m
        ql = V(vj * cj0 / (1.0 - m)) * (V(1.0) - t / pm); if (wantC) cl = V(cj0) / pm;
      }
      if (mask(low) != 15)
      {
        const double f1 = vj * (1.0 - std::pow(1.0 - fc, 1.0 - m)) / (1.0 - m), f2 = std::pow(1.0 - fc, 1.0 + m), f3 = 1.0 - fc * (1.0 + m);
        qh = V(cj0) * (V(f1) + V(1.0 / f2) * (V(f3) * (v - V(fc * vj)) + V(m / (2.0 * vj)) * (v * v - V(fc * fc * vj * vj)))); ch = V(cj0 / f2) * (V(f3) + V(m) * v / V(vj));
      }
      qj = sel(low, ql, qh); cj = sel(low, cl, ch);
    }
    if (kind == 0)
    {
      const V a = v * V(r[8]); const V e = sel(lt(a, V(-40.0)), V(0.0), vexp(vmin(a, V(80.0))));
      const V cbe = V(r[7]) * (e - V(1.0)) + V(kGmin) * v, gbe = V(r[7]) * e * V(r[8]) + V(kGmin); q = qj + V(tt) * cbe; c = cj + V(tt) * gbe;
    }
    else if (kind == 2 && tt > 0.0) { V i, g; DiodeStatic((int)r[9], v, i, g); q = qj + V(tt) * i; c = cj + V(tt) * g; }
    else { q = qj; c = cj; }
    q = q - V(C0) * v; c = c - V(C0);
  }
  inline void DiodeStatic(int idx, V v, V& i, V& g)
  {
    const double* q = &M::kDio[5 * idx]; const double Is = q[0], N = q[1], Rs = q[2], Bv = q[3], Ibv = q[4], nvt = N * kVt;
    const double xbv = sc.dioXbv[idx];
    const V deep = vand(lt(v, V(-40.0 * nvt)), gt(v, V(-xbv))); const int md = mask(deep);
    if (md == 15) { i = -V(Is) + V(kGmin) * v; g = V(kGmin); return; }
    V ii, gg;
    if (Rs <= 0.0) Junction(q, nvt, xbv, v, ii, gg);
    else
    {
      V vj = v; const V lim = V(6.0 * nvt);
      bool conv = false;
      for (int k = 0; k < 80; ++k)
      {
        Junction(q, nvt, xbv, vj, ii, gg); V dv = (vj + ii * V(Rs) - v) / (V(1.0) + gg * V(Rs)); dv = vmin(vmax(dv, -lim), lim); vj = vj - dv;
        if (mask(lt(vabs(dv), V(1e-9))) == 15) { ii = ii - gg * dv; conv = true; break; }   // Newton step below 1e-9 V: the current is moved to the new point along its slope
      }
      if (!conv) Junction(q, nvt, xbv, vj, ii, gg);
      gg = gg / (V(1.0) + gg * V(Rs));
    }
    if (md) { ii = sel(deep, -V(Is) + V(kGmin) * v, ii); gg = sel(deep, V(kGmin), gg); }
    i = ii; g = gg;
  }
  static inline V InvPosV(V x) { return V(0.5) * (x + vsqrt(x * x + V(0.01))); }
  // current and derivatives of one channel of a CD4069 gate (InvCur / InvDer of the scalar engine)
  static inline void Chan(const double* f, V vgs, V vds, bool n, V& cur, V& dgs, V& dds, bool wantD)
  {
    const V x = vgs - V(n ? f[3] : f[4]), r = vsqrt(x * x + V(0.01)), ov = V(0.5) * (x + r), dov = V(0.5) * (V(1.0) + x / r);
    const V fl = le(V(f[6]) * ov, V(0.05)); const V vs = sel(fl, V(0.05), V(f[6]) * ov), dvs = sel(fl, V(0.0), V(f[6]) * dov);
    const V u = vds / vs, T = vtanh(u), L = V(1.0) + V(f[5]) * vabs(vds); const double A = n ? f[0] : f[1];
    const V Pw = vpow(ov, V(f[2]));
    cur = V(A) * Pw * T * L;
    if (wantD)
    {
      const V sech2 = V(1.0) - T * T; const V dP = sel(gt(ov, V(0.0)), V(f[2]) * Pw / ov * dov, V(0.0));
      dgs = V(A) * (dP * T * L + Pw * sech2 * (-u / vs) * dvs * L);
      const V sg = sel(gt(vds, V(0.0)), V(1.0), sel(lt(vds, V(0.0)), V(-1.0), V(0.0)));
      dds = V(A) * Pw * (sech2 / vs * L + T * V(f[5]) * sg);
    }
  }
  // device currents f and the dense blocks d f / d p
  void EvalF(const V* q, V* f, bool wantJd)
  {
    for (int d = 0; d < ND; ++d)
    {
      const int pp = M::kDevPort[d], cc = M::kDevCur[d], idx = M::kDevIdx[d], ty = M::kDevType[d];
      switch (ty)
      {
      case 0:
      {
        const double* P = sc.bp[idx]; const double s = P[0], IS = P[1], ISE = P[8], invNFVt = P[9], invNEVt = P[10], invVAF = P[11], invIKF = P[12], invBF = P[13], invBR = P[14];
        const V vbe = q[pp], vbc = q[pp + 1]; const V aE = vbe * V(invNFVt), aC = vbc * V(1.0 / kVt);
        const V eE = sel(lt(aE, V(-40.0)), V(0.0), vexp(vmin(aE, V(80.0)))), eC = sel(lt(aC, V(-40.0)), V(0.0), vexp(vmin(aC, V(80.0))));
        const V cbe = V(IS) * (eE - V(1.0)) + V(kGmin) * vbe, gbe = V(IS) * eE * V(invNFVt) + V(kGmin);
        const V cbc = V(IS) * (eC - V(1.0)) + V(kGmin) * vbc, gbc = V(IS) * eC * V(1.0 / kVt) + V(kGmin);
        V cben(0.0), gben(0.0);
        if (ISE != 0.0) { const V aL = vbe * V(invNEVt); const V eL = sel(lt(aL, V(-40.0)), V(0.0), vexp(vmin(aL, V(80.0)))); cben = V(ISE) * (eL - V(1.0)); gben = V(ISE) * eL * V(invNEVt); }
        const V q1 = V(1.0) / (V(1.0) - vbc * V(invVAF)), dq1 = q1 * q1 * V(invVAF);
        const V q2 = cbe * V(invIKF), arg = vmax(V(0.0), V(1.0) + V(4.0) * q2), sq = vsqrt(arg), qb = q1 * (V(1.0) + sq) * V(0.5);
        const V dsqbe = sel(gt(arg, V(0.0)), (V(2.0) * gbe * V(invIKF)) / vmax(sq, V(1e-12)), V(0.0));
        const V dqbe = q1 * V(0.5) * dsqbe, dqbc = dq1 * (V(1.0) + sq) * V(0.5);
        const V iq = V(1.0) / qb, x = cbe - cbc;
        const V cc_ = x * iq - cbc * V(invBR), cb = cbe * V(invBF) + cben + cbc * V(invBR);
        f[cc] = V(s) * cc_; f[cc + 1] = V(s) * cb;
        if (wantJd)
        {
          const V dccbe = gbe * iq - x * iq * iq * dqbe, dccbc = -gbc * iq - x * iq * iq * dqbc - gbc * V(invBR);
          const V dcbbe = gbe * V(invBF) + gben, dcbbc = gbc * V(invBR);
          jd[d][0] = V(s) * dccbe; jd[d][1] = V(s) * dccbc; jd[d][2] = V(s) * dcbbe; jd[d][3] = V(s) * dcbbc;
        }
        break;
      }
      case 1: { V i, g; DiodeStatic(idx, q[pp], i, g); f[cc] = i; if (wantJd) jd[d][0] = g; break; }
      case 2:
      {
        const double gm = M::kTanh[3 * idx], im = M::kTanh[3 * idx + 1], lg = M::kTanh[3 * idx + 2]; const V x = V(gm) * q[pp] / V(im);
        const V th = vtanh(x); V fi = V(im) * th - V(lg) * q[pp], gi = V(gm) * (V(1.0) - th * th) - V(lg);
        if (lg == gm)
        {
          const V small = lt(vabs(x), V(0.05)); const V x2 = x * x;
          fi = sel(small, V(im) * x * x2 * (V(-1.0 / 3.0) + x2 * V(2.0 / 15.0)), fi); gi = sel(small, -V(gm) * x2 * (V(1.0) - x2 * V(2.0 / 3.0)), gi);
        }
        f[cc] = fi; if (wantJd) jd[d][0] = gi; break;
      }
      case 3:
      {
        const V pa = q[pp], pb = q[pp + 1], pc = q[pp + 2]; V i0, ngs, nds, i1, pgs, pds;
        Chan(M::kInvPar, pa, pb, true, i0, ngs, nds, wantJd); Chan(M::kInvPar, pc - pa, pc - pb, false, i1, pgs, pds, wantJd);
        f[cc] = i0; f[cc + 1] = i1;
        if (wantJd) { jd[d][0] = ngs; jd[d][1] = nds; jd[d][2] = V(0.0); jd[d][3] = -pgs; jd[d][4] = -pds; jd[d][5] = pgs + pds; }
        break;
      }
      case 6:
      {
        const double* r = &M::kCap[10 * idx]; V q1, c1; CapQ(r, q[pp], q1, c1, wantJd);
        f[cc] = V(r[1]) * (q1 - q0v[d]) / V(capH); if (wantJd) jd[d][0] = V(r[1]) * c1 / V(capH);
        break;
      }
      default: break;   // comparators / OTA: not in the toms
      }
    }
  }

  // ---- one step ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
  static constexpr int nPorts(int ty) { return S::nPortsOf(ty); }
  static constexpr int nCurs(int ty) { return S::nCursOf(ty); }
  V OutputV(const V* x1, const V* dx) const
  {
    V yv = V(sc.pm.Do1 + sc.pm.Dou[0] * acc);
    for (int t = 0; t < NT; ++t) yv = yv + V(sc.pm.Dou[1 + 2 * t]) * x1[t] + V(sc.pm.Dou[2 + 2 * t]) * dx[t];
    for (int i = 0; i < R; ++i) yv = yv + V(sc.pm.Xo[i]) * z[i];
    for (int k = 0; k < NF; ++k) yv = yv + V(sc.pm.Dof[k]) * f0[k];
    return yv;
  }
  // returns true when every lane converged; z, f0, p are updated (on failure they are undefined, the caller restores)
  bool StepOnce(int pi, const V* x0, const V* x1, V* dxOut, int* itsOut)
  {
    const typename S::Prop& P = sc.props[pi]; const double h = P.h; capH = h; V dx[NT];
    for (int a = 0; a < NP; ++a) ps0[a] = p[a];
    for (int d = 0; d < ND; ++d) if (M::kDevType[d] == 6) { V q, c; CapQ(&M::kCap[10 * M::kDevIdx[d]], ps0[M::kDevPort[d]], q, c, false); q0v[d] = q; }
    for (int t = 0; t < NT; ++t) dx[t] = (x1[t] - x0[t]) / V(h);
    for (int i = 0; i < R; ++i)
    {
      V a = V(P.ConeC[i] + P.Ga[i * NU] * acc + P.Gb[i * NU] * acc);
      for (int t = 0; t < NT; ++t) a = a + V(P.Ga[i * NU + 1 + 2 * t]) * x0[t] + V(P.Gb[i * NU + 1 + 2 * t]) * x1[t] + V(P.Ga[i * NU + 2 + 2 * t] + P.Gb[i * NU + 2 + 2 * t]) * dx[t];
      zk[i] = a;
    }
    MV(P.Phi.data(), R, R, z, R, zk, zk); MV(P.GaF.data(), NF, R, f0, NF, zk, zk);
    for (int a = 0; a < NP; ++a)
    {
      V v = V(sc.PC[a] + sc.PD[a * NU] * acc); for (int t = 0; t < NT; ++t) v = v + V(sc.PD[a * NU + 1 + 2 * t]) * x1[t] + V(sc.PD[a * NU + 2 + 2 * t]) * dx[t];
      pk[a] = v;
    }
    MV(P.PXz.data(), R, NP, zk, R, pk, pk);
    bool refac = !luOk || luProp != pi; const bool fromPrev = !refac; V prevMx(1e30), mx2(1e30), damp(1.0); int itDone = 0; bool conv = false;
    for (int it = 0; it < maxIt; ++it)
    {
      itDone = it + 1; EvalF(p, f1, refac); ++its;       MV(P.Zp.data(), NF, NP, f1, NF, Fv, pk); for (int a = 0; a < NP; ++a) Fv[a] = Fv[a] - p[a];       bool fresh = false;
      if (refac)
      {
        bool act[NP]; for (int a = 0; a < NP; ++a) act[a] = false;
        for (int d = 0; d < ND; ++d)
        {
          const int pp = M::kDevPort[d], cc = M::kDevCur[d], ty = M::kDevType[d], npt = nPorts(ty), ncu = nCurs(ty);
          for (int b = 0; b < npt; ++b)
          {
            if (sc.portPure[pp + b]) continue;
            if (ty >= 2 && ty != 6) { act[pp + b] = true; continue; }
            for (int c2 = 0; c2 < ncu; ++c2) if (mask(gt(vabs(jd[d][c2 * npt + b]) * V(P.Zmax[cc + c2]), V(asTol)))) act[pp + b] = true;
          }
        }
        nAct = 0; nFrz = 0; for (int a = 0; a < NP; ++a) { if (act[a]) actL[nAct++] = a; else frzL[nFrz++] = a; pRef[a] = p[a]; }
        V col[NP];
        for (int j = 0; j < nAct; ++j)
        {
          const int pj = actL[j]; for (int a = 0; a < NP; ++a) col[a] = V(0.0); col[pj] = V(1.0);
          for (const auto& db : portDevs[pj])
          {
            const int d = db.first, b = db.second; const int ty = M::kDevType[d], npt = nPorts(ty), ncu = nCurs(ty), cc = M::kDevCur[d];
            for (int c2 = 0; c2 < ncu; ++c2)
            {
              const V g = jd[d][c2 * npt + b]; if (mask(_mm256_cmp_pd(g.v, _mm256_setzero_pd(), _CMP_NEQ_OQ)) == 0) continue;
              const double* zt = &P.ZpT[(cc + c2) * NP]; for (int a = 0; a < NP; ++a) col[a] = fnma(g, bc(zt + a), col[a]);
            }
          }
          for (int i = 0; i < nAct; ++i) LU[i * NP + j] = col[actL[i]];
          for (int f = 0; f < nFrz; ++f) JFA[f * NP + j] = col[frzL[f]];
        }
                {   // LU of the active block, two pivot columns per sweep, row pivots per lane
          const int n = nAct;
          auto pivot = [&](int c)
          {
            for (int l = 0; l < 4; ++l)
            {
              int pr = c; double mxp = std::fabs(reinterpret_cast<const double*>(&LU[c * NP + c])[l]);
              for (int i = c + 1; i < n; ++i) { const double v = std::fabs(reinterpret_cast<const double*>(&LU[i * NP + c])[l]); if (v > mxp) { mxp = v; pr = i; } }
              pivL[c][l] = pr;
              if (pr != c) { ++swaps; for (int j = 0; j < n; ++j) std::swap(reinterpret_cast<double*>(&LU[c * NP + j])[l], reinterpret_cast<double*>(&LU[pr * NP + j])[l]); }
            }
            if (mask(lt(vabs(LU[c * NP + c]), V(1e-12)))) badPivot = true;
          };
          int c = 0;
          for (; c + 3 < n; c += 4)   // four pivot columns per sweep over the trailing block
          {
            V* R0 = &LU[c * NP]; V* R1 = &LU[(c + 1) * NP]; V* R2 = &LU[(c + 2) * NP]; V* R3 = &LU[(c + 3) * NP];
            pivot(c);
            { const V inv = V(1.0) / R0[c]; const V u1 = R0[c + 1], u2 = R0[c + 2], u3 = R0[c + 3];
              for (int i = c + 1; i < n; ++i) { V* ri = &LU[i * NP]; const V f = ri[c] * inv; ri[c] = f; ri[c + 1] = fnma(f, u1, ri[c + 1]); ri[c + 2] = fnma(f, u2, ri[c + 2]); ri[c + 3] = fnma(f, u3, ri[c + 3]); } }
            pivot(c + 1);
            { const V inv = V(1.0) / R1[c + 1]; const V u2 = R1[c + 2], u3 = R1[c + 3];
              for (int i = c + 2; i < n; ++i) { V* ri = &LU[i * NP]; const V f = ri[c + 1] * inv; ri[c + 1] = f; ri[c + 2] = fnma(f, u2, ri[c + 2]); ri[c + 3] = fnma(f, u3, ri[c + 3]); } }
            pivot(c + 2);
            { const V inv = V(1.0) / R2[c + 2]; const V u3 = R2[c + 3];
              for (int i = c + 3; i < n; ++i) { V* ri = &LU[i * NP]; const V f = ri[c + 2] * inv; ri[c + 2] = f; ri[c + 3] = fnma(f, u3, ri[c + 3]); } }
            pivot(c + 3);
            { const V inv = V(1.0) / R3[c + 3]; for (int i = c + 4; i < n; ++i) LU[i * NP + c + 3] = LU[i * NP + c + 3] * inv; }
            { const V l10 = R1[c], l20 = R2[c], l21 = R2[c + 1], l30 = R3[c], l31 = R3[c + 1], l32 = R3[c + 2];
              for (int j = c + 4; j < n; ++j)
              {
                const V a0 = R0[j]; const V a1 = fnma(l10, a0, R1[j]); R1[j] = a1;
                const V a2 = fnma(l21, a1, fnma(l20, a0, R2[j])); R2[j] = a2;
                R3[j] = fnma(l32, a2, fnma(l31, a1, fnma(l30, a0, R3[j])));
              } }
            for (int i = c + 4; i < n; ++i)
            {
              V* ri = &LU[i * NP]; const V f0 = ri[c], f1 = ri[c + 1], f2 = ri[c + 2], f3 = ri[c + 3];
              for (int j = c + 4; j < n; ++j) ri[j] = fnma(f3, R3[j], fnma(f2, R2[j], fnma(f1, R1[j], fnma(f0, R0[j], ri[j]))));
            }
          }
          for (; c + 1 < n; c += 2)
          {
            pivot(c);
            const V inv0 = V(1.0) / LU[c * NP + c], u01 = LU[c * NP + c + 1];
            for (int i = c + 1; i < n; ++i) { const V f = LU[i * NP + c] * inv0; LU[i * NP + c] = f; LU[i * NP + c + 1] = fnma(f, u01, LU[i * NP + c + 1]); }
            pivot(c + 1);
            const V inv1 = V(1.0) / LU[(c + 1) * NP + c + 1];
            { const V f = LU[(c + 1) * NP + c]; V* r1 = &LU[(c + 1) * NP]; const V* r0 = &LU[c * NP]; for (int j = c + 2; j < n; ++j) r1[j] = fnma(f, r0[j], r1[j]); }
            const V* b0 = &LU[c * NP]; const V* b1 = &LU[(c + 1) * NP];
            for (int i = c + 2; i < n; ++i)
            {
              V* ri = &LU[i * NP]; const V f1 = ri[c + 1] * inv1; ri[c + 1] = f1; const V f0 = ri[c];
              for (int j = c + 2; j < n; ++j) ri[j] = fnma(f1, b1[j], fnma(f0, b0[j], ri[j]));
            }
          }
          if (c < n) pivot(c);
        }        refac = false; fresh = true; ++facs; luOk = true; luProp = pi; if (badPivot) { ++pivotFail; return false; }
      }
      V Fa[NP], Ff[NP];
      for (int i = 0; i < nAct; ++i) Fa[i] = Fv[actL[i]];
      for (int f = 0; f < nFrz; ++f) Ff[f] = Fv[frzL[f]];
      for (int c = 0; c < nAct; ++c) for (int l = 0; l < 4; ++l) if (pivL[c][l] != c) std::swap(reinterpret_cast<double*>(&Fa[c])[l], reinterpret_cast<double*>(&Fa[pivL[c][l]])[l]);
      for (int i = 1; i < nAct; ++i) Fa[i] = Fa[i] - DotV(&LU[i * NP], Fa, i);
      for (int i = nAct - 1; i >= 0; --i) { const V* ri = &LU[i * NP]; Fa[i] = (Fa[i] - DotV(ri + i + 1, Fa + i + 1, nAct - i - 1)) / ri[i]; }
      for (int f = 0; f < nFrz; ++f) Ff[f] = Ff[f] - DotV(&JFA[f * NP], Fa, nAct);
      for (int i = 0; i < nAct; ++i) Fv[actL[i]] = Fa[i]; for (int f = 0; f < nFrz; ++f) Fv[frzL[f]] = Ff[f];
            V mx(0.0); for (int a = 0; a < NP; ++a) mx = vmax(mx, vabs(Fv[a]));
      if (mask(_mm256_cmp_pd(mx.v, mx.v, _CMP_UNORD_Q))) return false;
      if (!fresh && it == 0 && fromPrev && __popcnt(mask(vand(ge(mx, V(newtonTol)), gt(mx, V(chordFirst))))) >= (unsigned)refacMin) { refac = true; continue; }
      if (!fresh && it >= 1 && __popcnt(mask(vand(ge(mx, V(newtonTol)), gt(mx, V(0.5) * prevMx)))) >= (unsigned)refacMin) { refac = true; continue; }
      if (mask(ge(mx, V(newtonTol))) == 0) { conv = true; break; }
      V mxv(0.0); for (int a = 0; a < NP; ++a) mxv = vmax(mxv, vabs(Fv[a]) * V(sc.portScale[a]));
      const V sc_ = sel(gt(mxv, V(5.0)), V(5.0) / mxv, V(1.0));
      {   // damping against two-cycles of the saturating gates (per lane)
        const V c1 = it >= 3 ? vand(lt(vabs(mx - mx2), V(dampRatio) * mx), gt(mx, V(dampMin))) : V(0.0);
        const V c2 = it >= 2 ? vand(lt(mx, V(0.5) * prevMx), lt(prevMx, V(0.5) * mx2)) : V(0.0);
        damp = sel(c1, vmax(damp * V(0.5), V(1.0 / 64.0)), sel(c2, vmin(damp * V(2.0), V(1.0)), damp));
        mx2 = prevMx;
      }
      for (int a = 0; a < NP; ++a)
      {
        V vnew = p[a] + sc_ * Fv[a]; const double cap = sc.portCap[a];
        if (cap < 1e29) { const V dlt = vnew - p[a]; vnew = sel(gt(vabs(dlt), V(cap)), p[a] + sel(gt(vnew, p[a]), V(cap), V(-cap)), vnew); }
        vnew = p[a] + damp * (vnew - p[a]);
        const double vt = sc.portVt[a];
        if (vt > 0.0)
        {
          const V big = vand(gt(vnew, V(sc.portVcrit[a])), gt(vabs(vnew - p[a]), V(2.0 * vt)));
          if (mask(big))
          {
            const V arg = V(1.0) + (vnew - p[a]) / V(vt); const V pos = gt(p[a], V(0.0));
            const V v1 = sel(gt(arg, V(0.0)), p[a] + V(vt) * vlog(vmax(arg, V(1e-300))), V(sc.portVcrit[a]));
            const V v2 = V(vt) * vlog(vmax(vnew / V(vt), V(1e-300)));
            vnew = sel(big, sel(pos, v1, v2), vnew);
          }
        }
        p[a] = vnew;
      }
      if (nFrz > 0 && !refac)
        for (int f = 0; f < nFrz; ++f)
        {
          const int a = frzL[f]; const double vt = sc.portVt[a];
          if (__popcnt(mask(gt(vabs(p[a] - pRef[a]), V(vt > 0.0 ? 3.0 * vt : 1.0)))) >= (unsigned)frzMin) { refac = true; break; }
        }
      prevMx = mx;
    }
    *itsOut = itDone;
    if (!conv) return false;
    // z = zk + GbF f1, f0 = f1
    MV(P.GbF.data(), NF, R, f1, NF, z, zk);
    for (int k = 0; k < NF; ++k) f0[k] = f1[k];
    for (int t = 0; t < NT; ++t) dxOut[t] = dx[t];
    ++steps; return true;
  }
  // step with retry as two half steps (the scalar engine without the active-set fallback)
  bool Step(int pi, const V* x0, const V* x1, V* dxOut, int depth = 0)
  {
    V zs[R], fs[NF], ps[NP]; for (int i = 0; i < R; ++i) zs[i] = z[i]; for (int i = 0; i < NF; ++i) fs[i] = f0[i]; for (int i = 0; i < NP; ++i) ps[i] = p[i];
    int itd = 0; badPivot = false;
    if (StepOnce(pi, x0, x1, dxOut, &itd)) { lastIts = itd; return true; }
    luOk = false;
    int ph = pi < (int)sc.halfIdx.size() ? sc.halfIdx[pi] : -1;
    if (ph < 0 && depth < maxDepth) { ph = sc.Prepare(0.5 * sc.props[pi].h); sc.SetHalf(pi, ph); }
    luOk = false; if (depth >= maxDepth || ph < 0) { ++fails; return false; }
    for (int i = 0; i < R; ++i) z[i] = zs[i]; for (int i = 0; i < NF; ++i) f0[i] = fs[i]; for (int i = 0; i < NP; ++i) p[i] = ps[i]; ++retries; badPivot = false;
    V xm[NT]; for (int t = 0; t < NT; ++t) xm[t] = V(0.5) * (x0[t] + x1[t]);
    if (!Step(ph, x0, xm, dxOut, depth + 1)) return false;
    return Step(ph, xm, x1, dxOut, depth + 1);
  }
  int lastIts = 0;
};


} // namespace lanes

#ifdef DGG_LANE_REC   // test harness only: records (t, y of the four lanes) of every lockstep step
struct LaneRec { std::vector<double> t, y[4]; };
inline LaneRec* g_laneRec = nullptr;
#endif

// Renders up to four hits of one model at once; the output of lane l is the host-rate output of HitRunner::Render for the seed l (statistically, see above).
template <class Mdl>
class HitLaneRunner
{
public:
  using E = lanes::Eng<Mdl>; using V = lanes::V;
  static bool Supports(const HitTuning& tu)
  {
    return Mdl::kNT == 2 && Mdl::kNumCmp == 0 && Mdl::kNumOta == 0 && tu.preRoll == 0.0 && tu.knotPairT < 0.0 && !tu.cmpEvents && tu.fixedJ < 0 && tu.knotSpan == 1 && tu.ytol > 0.0;
  }
  // seeds[0..n-1], n in 1..4: out[l] receives the samples of hit l at fsHost. False: not supported, aborted, or a solver failure (the caller renders the hits one by one).
  bool RenderGroup(const HitTuning& tu, const uint64_t* seeds, int n, double seconds, double fsHost, std::vector<float>* out, bool (*aborted)() = nullptr)
  {
    if (n < 1 || n > 4 || !Supports(tu)) return false;
    Setup(tu); E& m = *e_; ResetState();
    HitNoise nz[4]; HostDecimator dec[4]; for (int l = 0; l < 4; ++l) { nz[l].Start(seeds[std::min(l, n - 1)], 5e-6, 0.0); dec[l].Init(fsHost); }
    for (int l = 0; l < n; ++l) out[l].clear();
    const size_t want = static_cast<size_t>(seconds * fsHost);
    long long k = 0; int j = 0; double gHprev = 0.0; V gFprev[E::NF]; for (int q = 0; q < E::NF; ++q) gFprev[q] = V(0.0); double gEnv[4] = {0, 0, 0, 0};
    const long long nEnd = static_cast<long long>(seconds / kH0), knotN = std::max(1LL, std::llround(nz[0].step() / kH0)); const long long bp[4] = {64, 12864, 12928, 1LL << 40};
    static constexpr double kYenvRef = 0.1, kYenvTau = 2e-3, kXs = 4.0; const int jmax = 10, jdown = 12; unsigned cnt = 0;
    const double idle = m.sc.yIdle;
    while (k < nEnd)
    {
      if ((++cnt & 127u) == 0 && aborted && aborted()) return false;
      long long nextBp = 1LL << 40; for (long long b : bp) if (b > k) { nextBp = b; break; } nextBp = std::min(nextBp, (k / knotN + 1) * knotN);
      int jj = j; while (jj > 0 && k + (1LL << jj) > nextBp) --jj; const double h = kH0 * double(1LL << jj);
      V x0[2], x1[2], dx[2]; { double a[4], b[4]; for (int l = 0; l < 4; ++l) { a[l] = nz[l].At(kStartTime + k * kH0); b[l] = nz[l].At(kStartTime + (k + (1LL << jj)) * kH0); } x0[0] = lanes::make(a); x1[0] = lanes::make(b); }
      x0[1] = V(HitTrigger(k * kH0)); x1[1] = V(HitTrigger((k + (1LL << jj)) * kH0));
      V gF[E::NF]; for (int q = 0; q < E::NF; ++q) gF[q] = m.f0[q]; V pOld[E::NP]; for (int a = 0; a < E::NP; ++a) pOld[a] = m.p[a];
      const long retries0 = m.retries, fails0 = m.fails;
      if (!m.Step(pj_[jj], x0, x1, dx)) return false;
      k += 1LL << jj; const double t = k * kH0; const V yv = m.OutputV(x1, dx);
      double yl[4]; for (int l = 0; l < 4; ++l) { yl[l] = lanes::get(yv, l); if (!(std::fabs(yl[l]) < 1e6)) return false; gEnv[l] = std::fmax(std::fabs(yl[l]), gEnv[l] * std::exp(-h / kYenvTau)); }
#ifdef DGG_LANE_REC
      if (g_laneRec) { g_laneRec->t.push_back(t); for (int l = 0; l < 4; ++l) g_laneRec->y[l].push_back(yl[l]); }
#endif
      for (int l = 0; l < n; ++l) { dec[l].Push(t, yl[l] - idle); double smp; while (dec[l].Pop(smp)) out[l].push_back(static_cast<float>(smp)); }
      const bool hard = (m.retries != retries0) || (m.fails != fails0) || m.lastIts > jdown;
      double hMax[4]; for (int l = 0; l < 4; ++l) hMax[l] = kH0 * double(1 << jmax);
      V dpv(0.0); for (int a = 0; a < E::NP; ++a) dpv = lanes::vmax(dpv, lanes::vabs(m.p[a] - pOld[a]) * V(m.sc.portScale[a]));
      for (int d = 0; d < E::ND; ++d) if (Mdl::kDevType[d] == 2)
      {
        const int pp = Mdl::kDevPort[d], idx = Mdl::kDevIdx[d]; const double kk = Mdl::kTanh[3 * idx] / Mdl::kTanh[3 * idx + 1];
        for (int l = 0; l < 4; ++l)
        {
          const double xn = kk * lanes::get(m.p[pp], l), xo = kk * lanes::get(pOld[pp], l), rate = (xn - xo) / h, ax = std::fabs(xn), toward = (xn > 0 ? -rate : rate);
          double c = 1e300; if (ax < kXs) c = kXs / std::fmax(std::fabs(rate), 1e-30); else if (toward > 0) c = 0.5 * (ax - kXs) / toward;
          if (c < hMax[l] && !tu.noTanhEvents) hMax[l] = c;
        }
      }
      if (gHprev > 0.0)
      {
        V df[E::NF]; for (int q = 0; q < E::NF; ++q) df[q] = m.f0[q] - (gF[q] + (gF[q] - gFprev[q]) * V(h / gHprev));
        const auto& P = m.sc.props[pj_[jj]]; V ev(0.0);
        for (int s = 0; s < E::NS; ++s) { V v(0.0); const double* g = &P.XG[s * E::NF]; for (int q = 0; q < E::NF; ++q) v = lanes::fma(V(g[q]), df[q], v); ev = lanes::vmax(ev, lanes::vabs(v)); }
        for (int l = 0; l < 4; ++l)
        {
          const double e_ = lanes::get(ev, l);
          if (e_ > 0.0)
          {
            double tol = tu.ytol; const double tt = k * kH0;
            if (tu.ytailMul != 1.0 && tt >= tu.ytailT) tol *= tu.ytailMul; else if (tu.ytail2Mul != 1.0 && tt >= tu.ytail2T) tol *= tu.ytail2Mul;
            tol *= std::fmin(1.0, std::fmax(tu.yfloor, gEnv[l] / kYenvRef));
            const double c = h * std::sqrt(0.8 * tol / e_); if (c < hMax[l]) hMax[l] = c;
          }
        }
      }
      for (int q = 0; q < E::NF; ++q) gFprev[q] = gF[q]; gHprev = h;
      for (int l = 0; l < 4; ++l) { const double dp = lanes::get(dpv, l); if (dp > tu.dpTol) { const double c = h * tu.dpTol / dp; if (c < hMax[l]) hMax[l] = c; } }
      int jnmin = jmax; for (int l = 0; l < 4; ++l) { int jn = 0; while (jn < jmax && kH0 * double(1LL << (jn + 1)) <= hMax[l]) ++jn; jnmin = std::min(jnmin, jn); }
      j = hard ? std::max(0, std::min(jnmin, jj - 1)) : std::min(jnmin, jj + tu.jGrow);
    }
    for (int l = 0; l < n; ++l) { double smp; while (dec[l].Pop(smp)) out[l].push_back(static_cast<float>(smp)); if (out[l].size() > want) out[l].resize(want); }
    return true;
  }
  // Builds the model and its propagators (about a second).
  void Warm(const HitTuning& tu) { Setup(tu); }
  // Pot dependent model (PotSet.h), as HitRunner::SetPot: the model and its propagators are rebuilt at the next Warm / RenderGroup; the arrays must outlive the runner / the next SetPot(). Not while a group is rendering.
  void SetPot(const typename FullFast2<Mdl>::PotPtrs& p) { pot_ = p; usePot_ = true; e_.reset(); }
  void ClearPot() { usePot_ = false; e_.reset(); }

private:
  static constexpr double kH0 = 1e-5 / 64.0, kStartTime = 0.05;
  void Setup(const HitTuning& tu)
  {
    if (e_) return;
    e_.reset(new E()); if (usePot_) e_->sc.pm = pot_; e_->Init(); e_->sc.zohW = tu.zohW; e_->sc.chordFirst = tu.chordFirst; e_->chordFirst = tu.chordFirst;
    pj_.resize(11); for (int j = 0; j <= 10; ++j) pj_[j] = e_->sc.Prepare(kH0 * double(1 << j));
    for (int j = 1; j <= 10; ++j) e_->sc.SetHalf(pj_[j], pj_[j - 1]);
    e_->sc.SetHalf(pj_[0], e_->sc.Prepare(kH0 * 0.5));
  }
  void ResetState()
  {
    E& m = *e_; m.sc.Reset(Mdl::kAcc); m.acc = Mdl::kAcc;
    for (int i = 0; i < E::R; ++i) m.z[i] = V(m.sc.z[i]); for (int i = 0; i < E::NF; ++i) m.f0[i] = V(m.sc.f0[i]); for (int i = 0; i < E::NP; ++i) m.p[i] = V(m.sc.p[i]);
    m.steps = m.its = m.facs = m.fails = m.retries = m.pivotFail = 0; m.luOk = false;
  }
  std::unique_ptr<E> e_; std::vector<int> pj_; typename FullFast2<Mdl>::PotPtrs pot_{}; bool usePot_ = false;
};

}}   // namespace dgg::pcmfast
#endif
