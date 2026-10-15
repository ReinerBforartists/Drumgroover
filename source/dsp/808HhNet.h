#pragma once
// Dense trapezoid / Newton solver for the small nonlinear networks of the HiHat fast model (generated data in 808HhModel.h): the signal cores of the open and closed chain and the envelope generators.
// Modified nodal analysis with Gummel-Poon transistors (Q2SC945 / Q2SA733) and 1N4148 diodes, without the junction charges of the devices (the substrate capacitors
// stay in C): C dv/dt + G v + f(v) = b(t), b(t) from the fixed nodes (+15 V, -15 V, trigger, accent, bp2out, envelope input), capacitors to fixed nodes inject C dVf/dt.

#include <algorithm>
#include <cmath>
#include <cstring>
#ifdef HH_PROFILE
#include <x86intrin.h>
#endif

namespace dgg { namespace hhfast {

namespace prof {
#ifdef HH_PROFILE
inline unsigned long long t[16] = {}, last = 0;   // TSC cycles per section (study builds with -DHH_PROFILE only)
inline void Mark(int i) { const unsigned long long n = __rdtsc(); t[i] += n - last; last = n; }
#define HHP(i) ::dgg::hhfast::prof::Mark(i)
#define HHP_START() (::dgg::hhfast::prof::last = __rdtsc())
#else
#define HHP(i) ((void)0)
#define HHP_START() ((void)0)
#endif
}

namespace detail {
constexpr double kVt = 0.0258649, kDiodeIs = 4.352e-9, kDiodeN = 1.906;
struct BjtModel { double is, bf, nf, ise, ne, ikf, vaf, br, nr; double inf, ine, inr, ibf, ibr, iikf, ivaf; };
inline BjtModel MakeModel(double is, double bf, double nf, double ise, double ne, double ikf, double vaf, double br, double nr) { return {is, bf, nf, ise, ne, ikf, vaf, br, nr, 1.0 / (nf * 0.0258649), 1.0 / (ne * 0.0258649), 1.0 / (nr * 0.0258649), 1.0 / bf, 1.0 / br, 1.0 / ikf, 1.0 / vaf}; }
inline const BjtModel& Npn() { static const BjtModel m = MakeModel(35.1e-15, 222.31, 1.0, 6.63e-15, 1.319, 0.130, 315.0, 1.0, 1.0); return m; }
inline const BjtModel& Pnp() { static const BjtModel m = MakeModel(55.9e-15, 205.0, 1.01201, 180e-15, 1.65, 0.4626, 135.0, 2.048, 1.012); return m; }
inline double LimExp(double x, double& d) { if (x > 40.0) { const double e = std::exp(40.0); d = e; return e * (1.0 + (x - 40.0)); } const double e = std::exp(x); d = e; return e; }
// Gummel-Poon collector / base currents (NPN convention, x = vbe, y = vbc) and their partial derivatives
struct BjtI { double ic, ib, icx, icy, ibx, iby; };
inline BjtI Bjt(const BjtModel& m, double vbe, double vbc)
{
  double de1, de2 = 0.0, de3;
  const double xc = vbc * m.inr;
  const double e1 = LimExp(vbe * m.inf, de1), e3 = LimExp(vbe * m.ine, de3); double e2 = 0.0; if (xc > -20.0) e2 = LimExp(xc, de2);   // reverse biased collector junction (exp < 2e-9): -Is
  const double cbe = m.is * (e1 - 1.0), cbc = m.is * (e2 - 1.0), ile = m.ise * (e3 - 1.0);
  const double dcbe = m.is * de1 * m.inf, dcbc = m.is * de2 * m.inr, dile = m.ise * de3 * m.ine;
  const double den = 1.0 - vbc * m.ivaf; const bool clampQ1 = den < 0.05; const double q1 = 1.0 / (clampQ1 ? 0.05 : den), dq1 = clampQ1 ? 0.0 : q1 * q1 * m.ivaf;
  const bool pos = cbe > 0.0; const double q2 = (pos ? cbe : 0.0) * m.iikf, dq2 = pos ? dcbe * m.iikf : 0.0;
  const double sq = std::sqrt(1.0 + 4.0 * q2), isq = 1.0 / sq, qb = 0.5 * q1 * (1.0 + sq), dqbx = q1 * dq2 * isq, dqby = 0.5 * dq1 * (1.0 + sq);
  BjtI r; const double iq = 1.0 / qb, x = cbe - cbc, iq2 = iq * iq;
  r.ic = x * iq - cbc * m.ibr; r.ib = cbe * m.ibf + ile + cbc * m.ibr;
  r.icx = dcbe * iq - x * dqbx * iq2; r.icy = -dcbc * iq - x * dqby * iq2 - dcbc * m.ibr;
  r.ibx = dcbe * m.ibf + dile; r.iby = dcbc * m.ibr;
  return r;
}
// the device kernels the generated straight-line device code of 808HhModel.h (Devices<D, WithJ>) calls
struct DevOps
{
  static inline BjtI Bjt(const BjtModel& m, double vbe, double vbc) { return detail::Bjt(m, vbe, vbc); }
  static inline const BjtModel& Npn() { return detail::Npn(); }
  static inline const BjtModel& Pnp() { return detail::Pnp(); }
  static inline void Diode(double vd, double& id, double& gd) { double de; const double e = LimExp(vd / (kDiodeN * kVt), de); id = kDiodeIs * (e - 1.0); gd = kDiodeIs * de / (kDiodeN * kVt); }
};
// dense Gaussian elimination with partial pivoting, solves A x = b in place (A destroyed)
template <int N> inline bool Solve(double (*A)[N], double* b)
{
  for (int c = 0; c < N; ++c)
  {
    int p = c; for (int r = c + 1; r < N; ++r) if (std::fabs(A[r][c]) > std::fabs(A[p][c])) p = r;
    if (std::fabs(A[p][c]) < 1e-300) return false;
    if (p != c) { for (int j = c; j < N; ++j) std::swap(A[c][j], A[p][j]); std::swap(b[c], b[p]); }
    const double inv = 1.0 / A[c][c];
    for (int r = c + 1; r < N; ++r) { const double m = A[r][c] * inv; if (m == 0.0) continue; for (int j = c + 1; j < N; ++j) A[r][j] -= m * A[c][j]; b[r] -= m * b[c]; }
  }
  for (int i = N - 1; i >= 0; --i) { double s = b[i]; for (int j = i + 1; j < N; ++j) s -= A[i][j] * b[j]; b[i] = s / A[i][i]; }
  return true;
}
}  // namespace detail

// M: one of the namespaces of 808HhModel.h (kN, kG, kC, kFix, kFixG, kCapFix, kCapFixC, kDevType, kDev, kRest)
template <class M> class DenseNet
{
public:
  static constexpr int N = M::kN;
  static constexpr int kKinds = 16;   // values of the fixed nodes by kind: 2 = +15 V, 3 = -15 V, 4 / 6 trigger, 5 / 7 accent, 8 / 14 inputs

  DenseNet() { for (int i = 0; i < N * N; ++i) mG[i] = M::kG[i]; for (int k = 0; k < M::kNumFix; ++k) mFixG[k] = M::kFixG[k]; Reset(); }
  void Reset() { for (int i = 0; i < N; ++i) mV[i] = M::kRest[i]; }
  // a resistor to a fixed node (pot) changes its conductance: the diagonal term and the forcing coupling
  void SetFixConductance(int k, double g) { mG[M::kFix[k][0] * N + M::kFix[k][0]] += g - mFixG[k]; mFixG[k] = g; }
  // a resistor between two unknowns (b = -1: ground) changes its conductance by dg (pots inside the network)
  void StampG(int a, int b, double dg) { mG[a * N + a] += dg; if (b >= 0) { mG[b * N + b] += dg; mG[a * N + b] -= dg; mG[b * N + a] -= dg; } }
  double V(int i) const { return mV[i]; }
  void SetV(int i, double v) { mV[i] = v; }
  void SetState(const double* v) { for (int i = 0; i < N; ++i) mV[i] = v[i]; }

  // DC state for the fixed-node values vf (Newton on G v + f(v) = b), starting from the current state; returns false when it did not converge
  bool Dc(const double* vf)
  {
    using namespace detail;
    double b[N], f[N], r[N], A[N][N], J[N][N]; for (int i = 0; i < N; ++i) b[i] = M::kB0[i];
    for (int k = 0; k < M::kNumFix; ++k) b[M::kFix[k][0]] += mFixG[k] * Kind(vf, M::kFix[k][1]);
    for (int it = 0; it < 100; ++it)
    {
      Devices(mV, vf, f, J);
      for (int i = 0; i < N; ++i)
      {
        double gv = 0.0; for (int j = 0; j < N; ++j) { gv += mG[i * N + j] * mV[j]; A[i][j] = mG[i * N + j] + J[i][j]; }
        r[i] = -(gv + f[i] - b[i]);
      }
      if (!Solve<N>(A, r)) return false;
      double mx = 0.0, scale = 1.0; for (int i = 0; i < N; ++i) { mx = std::max(mx, std::fabs(r[i])); if (std::fabs(r[i]) > 0.5) scale = std::min(scale, 0.5 / std::fabs(r[i])); }
      for (int i = 0; i < N; ++i) mV[i] += scale * r[i];
      if (mx * scale < 1e-12 && scale == 1.0) return true;
    }
    return false;
  }

  // One step h from (fixed values vf0 at the start, vf1 at the end of the step); trapezoid. Returns false when Newton did not converge.
  bool Step(double h, const double* vf0, const double* vf1)
  {
    using namespace detail;
    double b0[N], b1[N], f0[N], f1[N], v1[N], r[N], A[N][N], base[N], Jd[N][N];
    for (int i = 0; i < N; ++i) { b0[i] = b1[i] = M::kB0[i]; }
    for (int k = 0; k < M::kNumFix; ++k) { b0[M::kFix[k][0]] += mFixG[k] * Kind(vf0, M::kFix[k][1]); b1[M::kFix[k][0]] += mFixG[k] * Kind(vf1, M::kFix[k][1]); }
    double dq[N]; for (int i = 0; i < N; ++i) dq[i] = 0.0;   // C dVf over the step of the capacitors to fixed nodes
    for (int k = 0; k < M::kNumCapFix; ++k) dq[M::kCapFix[k][0]] += M::kCapFixC[k] * (Kind(vf1, M::kCapFix[k][1]) - Kind(vf0, M::kCapFix[k][1]));
    Devices(mV, vf0, f0, nullptr);
    double gv0[N]; for (int i = 0; i < N; ++i) { double s = 0.0; for (int j = 0; j < N; ++j) s += mG[i * N + j] * mV[j]; gv0[i] = s; }
    for (int i = 0; i < N; ++i) { base[i] = 0.5 * (gv0[i] + f0[i] - b0[i]) - 0.5 * b1[i] - dq[i] / h; v1[i] = mV[i]; }
    bool conv = false;
    auto residual = [&](const double* v, double* out) {   // unsolved residual (the sign of r above)
      double fv[N]; Devices(v, vf1, fv, nullptr); double mxr = 0.0;
      for (int i = 0; i < N; ++i)
      {
        double cv = 0.0, gv = 0.0; for (int j = 0; j < N; ++j) { cv += M::kC[i * N + j] * (v[j] - mV[j]); gv += mG[i * N + j] * v[j]; }
        out[i] = -(cv / h + 0.5 * (gv + fv[i]) + base[i]); mxr = std::max(mxr, std::fabs(out[i]));
      }
      return mxr;
    };
    for (int it = 0; it < 40; ++it)
    {
      Devices(v1, vf1, f1, Jd);
      double mx = 0.0;
      for (int i = 0; i < N; ++i)
      {
        double cv = 0.0, gv = 0.0; for (int j = 0; j < N; ++j) { cv += M::kC[i * N + j] * (v1[j] - mV[j]); gv += mG[i * N + j] * v1[j]; }
        r[i] = -(cv / h + 0.5 * (gv + f1[i]) + base[i]); mx = std::max(mx, std::fabs(r[i]));
        for (int j = 0; j < N; ++j) A[i][j] = M::kC[i * N + j] / h + 0.5 * (mG[i * N + j] + Jd[i][j]);
      }
      if (mx < 1e-12) { conv = true; break; }
      if (!Solve<N>(A, r)) break;
      double scale = 1.0, dmax = 0.0; for (int i = 0; i < N; ++i) { dmax = std::max(dmax, std::fabs(r[i])); if (std::fabs(r[i]) > 0.5) scale = std::min(scale, 0.5 / std::fabs(r[i])); }
      // backtracking: halve the step while the residual does not shrink
      double vt[N], rt[N];
      for (int bt = 0; bt < 10; ++bt)
      {
        for (int i = 0; i < N; ++i) vt[i] = v1[i] + scale * r[i];
        if (residual(vt, rt) < mx || bt == 9) break;
        scale *= 0.5;
      }
      for (int i = 0; i < N; ++i) v1[i] = vt[i];
      if (dmax * scale < 1e-9 && scale == 1.0) { conv = true; break; }
    }
    for (int i = 0; i < N; ++i) mV[i] = v1[i];
    return conv;
  }

private:
  static double Kind(const double* vf, int kind) { return kind == 1 ? 0.0 : vf[kind]; }
  static double Term(const double* v, const double* vf, int t) { return t >= 0 ? v[t] : (t == -1 ? 0.0 : vf[-t]); }

  // device currents leaving the nodes and (when J != nullptr) their Jacobian
  static void Devices(const double* v, const double* vf, double* f, double (*J)[N])
  {
    using namespace detail;
    for (int i = 0; i < N; ++i) f[i] = 0.0;
    if (J) for (int i = 0; i < N; ++i) for (int j = 0; j < N; ++j) J[i][j] = 0.0;
    for (int k = 0; k < M::kNumDev; ++k)
    {
      const int ta = M::kDev[k][0], tb = M::kDev[k][1], tc = M::kDev[k][2];
      const double va = Term(v, vf, ta), vb = Term(v, vf, tb);
      if (M::kDevType[k] == 'D')
      {
        const double vd = va - vb; double de; const double e = LimExp(vd / (kDiodeN * kVt), de); const double id = kDiodeIs * (e - 1.0), gd = kDiodeIs * de / (kDiodeN * kVt);
        if (ta >= 0) { f[ta] += id; if (J) { J[ta][ta] += gd; if (tb >= 0) J[ta][tb] -= gd; } }
        if (tb >= 0) { f[tb] -= id; if (J) { J[tb][tb] += gd; if (ta >= 0) J[tb][ta] -= gd; } }
        continue;
      }
      const double s = (M::kDevType[k] == 'N') ? 1.0 : -1.0; const BjtModel& m = (M::kDevType[k] == 'N') ? Npn() : Pnp();
      const double vc = va, ve = Term(v, vf, tc);
      const BjtI c = Bjt(m, s * (vb - ve), s * (vb - vc));
      const double Ic = s * c.ic, Ib = s * c.ib, Ie = -(Ic + Ib);
      const int ix[3] = {ta, tb, tc}; const double cur[3] = {Ic, Ib, Ie};
      for (int q = 0; q < 3; ++q) if (ix[q] >= 0) f[ix[q]] += cur[q];
      if (J)
      {
        // d/d(vc, vb, ve) of Ic, Ib (the signs of s cancel): (-xy, xx + xy, -xx)
        const double dIc[3] = {-c.icy, c.icx + c.icy, -c.icx}, dIb[3] = {-c.iby, c.ibx + c.iby, -c.ibx};
        for (int p = 0; p < 3; ++p)
        {
          if (ix[p] < 0) continue;
          const double dcur[3] = {dIc[p], dIb[p], -(dIc[p] + dIb[p])};
          for (int q = 0; q < 3; ++q) if (ix[q] >= 0) J[ix[q]][ix[p]] += dcur[q];
        }
      }
    }
  }

  double mV[N];
  double mG[N * N], mFixG[M::kNumFix > 0 ? M::kNumFix : 1];
};

// Same network with the Newton iteration restricted to the unknowns that carry a nonlinear device terminal (kS): the linear part A = C/h + G/2 (with the linearised devices in G and their constant current
// in kB0) is inverted once per step size, a step costs two small matrix-vector products and the Newton iteration on the S nodes.
template <class M> class ReducedNet
{
public:
  static constexpr int N = M::kN, K = M::kNumS;
  ReducedNet() { for (int i = 0; i < N * N; ++i) mGv[i] = M::kG[i]; Reset(); }
  // a resistor between two unknowns (b = -1: ground) changes its conductance by dg (pots inside the network); takes effect with the next Build
  void StampG(int a, int b, double dg) { mGv[a * N + a] += dg; if (b >= 0) { mGv[b * N + b] += dg; mGv[a * N + b] -= dg; mGv[b * N + a] -= dg; } }
  void Reset() { for (int i = 0; i < N; ++i) mV[i] = M::kRest[i]; SyncPrev(); }
  double V(int i) const { return mV[i]; }
  void SetV(int i, double v) { mV[i] = v; mHaveF0 = false; }
  // whole state; the extrapolation of the next step starts from it (no history)
  void SetState(const double* v) { for (int i = 0; i < N; ++i) mV[i] = v[i]; SyncPrev(); }
  long statSteps = 0, statIters = 0, statEvals = 0;

  // precomputes the matrices for the step size h
  void Build(double h)
  {
    mH = h; double A[N][N], B[N][N];
    for (int i = 0; i < N; ++i) for (int j = 0; j < N; ++j) { A[i][j] = M::kC[i * N + j] / h + mTheta * mGv[i * N + j]; B[i][j] = M::kC[i * N + j] / h - (1.0 - mTheta) * mGv[i * N + j]; }
    double W[N][2 * N];
    for (int i = 0; i < N; ++i) for (int j = 0; j < N; ++j) { W[i][j] = A[i][j]; W[i][N + j] = i == j ? 1.0 : 0.0; }
    for (int c = 0; c < N; ++c)
    {
      int p = c; for (int r = c + 1; r < N; ++r) if (std::fabs(W[r][c]) > std::fabs(W[p][c])) p = r;
      if (p != c) for (int j = 0; j < 2 * N; ++j) std::swap(W[c][j], W[p][j]);
      const double d = 1.0 / W[c][c]; for (int j = 0; j < 2 * N; ++j) W[c][j] *= d;
      for (int r = 0; r < N; ++r) if (r != c) { const double f = W[r][c]; if (f != 0.0) for (int j = 0; j < 2 * N; ++j) W[r][j] -= f * W[c][j]; }
    }
    double Ainv[N][N], Phi[N][N];
    for (int i = 0; i < N; ++i) for (int j = 0; j < N; ++j) Ainv[i][j] = W[i][N + j];
    for (int i = 0; i < N; ++i) for (int j = 0; j < N; ++j) { double s = 0.0; for (int k = 0; k < N; ++k) s += Ainv[i][k] * B[k][j]; Phi[i][j] = s; }
    {
      double Wk[K][2 * K];   // Minv: inverse of the S block of the step matrix inverse
      for (int i = 0; i < K; ++i) for (int j = 0; j < K; ++j) { Wk[i][j] = Ainv[M::kS[i]][M::kS[j]]; Wk[i][K + j] = i == j ? 1.0 : 0.0; }
      for (int c = 0; c < K; ++c)
      {
        int p = c; for (int r = c + 1; r < K; ++r) if (std::fabs(Wk[r][c]) > std::fabs(Wk[p][c])) p = r;
        if (p != c) for (int j = 0; j < 2 * K; ++j) std::swap(Wk[c][j], Wk[p][j]);
        const double d = 1.0 / Wk[c][c]; for (int j = 0; j < 2 * K; ++j) Wk[c][j] *= d;
        for (int r = 0; r < K; ++r) if (r != c) { const double f = Wk[r][c]; if (f != 0.0) for (int j = 0; j < 2 * K; ++j) Wk[r][j] -= f * Wk[c][j]; }
      }
      for (int i = 0; i < K; ++i) for (int j = 0; j < K; ++j) mMinv[i][j] = Wk[i][K + j];
    }
    // dense copies for the generated products (M::Pre / M::Recon read only their structural nonzeros)
    for (int i = 0; i < N; ++i) for (int j = 0; j < N; ++j) { mPhi[i * N + j] = Phi[i][j]; mAi[i * N + j] = Ainv[i][j]; }
    for (int i = 0; i < N; ++i) for (int k = 0; k < K; ++k) { mS0[i * K + k] = (1.0 - mTheta) * Ainv[i][M::kS[k]]; mS1[i * K + k] = mTheta * Ainv[i][M::kS[k]]; }
    mHaveF0 = false;
  }

  // one step; vf0 / vf1 = fixed-node values at the start / end of the step
  bool Step(const double* vf0, const double* vf1)
  {
    HHP_START();
    const double h = mH;
    double force[N];
    for (int i = 0; i < N; ++i) force[i] = M::kB0[i];
    for (int k = 0; k < M::kNumFix; ++k) force[M::kFix[k][0]] += M::kFixG[k] * ((1.0 - mTheta) * Kind(vf0, M::kFix[k][1]) + mTheta * Kind(vf1, M::kFix[k][1]));
    // the constant forcing counts fully (it is the same at both ends of the step); the fixed-node couplings enter as the mean of both ends: b0 / 2 + b1 / 2
    for (int k = 0; k < M::kNumCapFix; ++k) force[M::kCapFix[k][0]] += M::kCapFixC[k] * (Kind(vf1, M::kCapFix[k][1]) - Kind(vf0, M::kCapFix[k][1])) / h;
    // device currents at the start of the step: the last evaluation of the previous step belongs to this state and to these fixed-node values
    if (!mHaveF0) { double y0[K]; for (int i = 0; i < K; ++i) y0[i] = mV[M::kS[i]]; M::template Devices<detail::DevOps, false>(y0, vf0, mF0, nullptr); }
    HHP(0);   // forcing and device currents at the start
    double base[N];
    M::Pre(mPhi, mAi, mS0, mV, force, mF0, base);
    HHP(1);   // Pre
    double cS[K];
    for (int i = 0; i < K; ++i) { double s = 0.0; for (int j = 0; j < K; ++j) s += mMinv[i][j] * base[M::kS[j]]; cS[i] = s; }
    HHP(2);   // S-node right-hand side
    // two sets (current point / trial point) that swap roles on acceptance
    double Y[2][K], F[2][K], JF[2][K][K], R[2][K]; int c = 0;
    for (int i = 0; i < K; ++i) Y[0][i] = mV[M::kS[i]] + mExtrap * (mV[M::kS[i]] - mVprev[i]);   // start: linear extrapolation of the S nodes
    // the equations of the S nodes in the form  Minv y + theta f(y) = Minv baseS  (Minv = inverse of the S block of the step matrix): no matrix product with the device Jacobian is needed
    auto evaluate = [&](int k) {
      HHP(7); M::template Devices<detail::DevOps, true>(Y[k], vf1, F[k], JF[k]); HHP(3); double nrm = 0.0;
      for (int i = 0; i < K; ++i) { double s = 0.0; for (int j = 0; j < K; ++j) s += mMinv[i][j] * Y[k][j]; R[k][i] = s + mTheta * F[k][i] - cS[i]; nrm = std::max(nrm, std::fabs(R[k][i])); }
      return nrm;
    };
    double rn = evaluate(0); bool conv = false; double lastDy = 0.0; ++statSteps; ++statEvals;
    for (int it = 0; it < 150; ++it)
    {
      ++statIters;
      double A[K][K], dy[K];
      for (int i = 0; i < K; ++i) { for (int j = 0; j < K; ++j) A[i][j] = mMinv[i][j] + mTheta * JF[c][i][j]; dy[i] = -R[c][i]; }
      HHP(7); const bool solved = M::NewtonSolve(A, dy);
      HHP(4);
      if (!solved) break;
      double mxd = 0.0; for (int i = 0; i < K; ++i) mxd = std::max(mxd, std::fabs(dy[i]));
      lastDy = mxd;
      if (mxd < mTol)   // converged: the last update is taken with the currents of the linearisation (no further evaluation)
      {
        for (int i = 0; i < K; ++i) { Y[c][i] += dy[i]; double s = 0.0; for (int j = 0; j < K; ++j) s += JF[c][i][j] * dy[j]; F[c][i] += s; }
        conv = true; break;
      }
      double alpha = mxd > 0.5 ? 0.5 / mxd : 1.0, rnNew = rn; const int t = c ^ 1;
      for (int tr = 0; tr < 12; ++tr)
      {
        for (int i = 0; i < K; ++i) Y[t][i] = Y[c][i] + alpha * dy[i];
        rnNew = evaluate(t); ++statEvals;
        if (rnNew < rn * (1.0 - 1e-4 * alpha) || rnNew < 1e-11) break;
        alpha *= 0.5;
      }
      c = t; rn = rnNew;
    }
    HHP(5);   // Newton bookkeeping
    const double* f = F[c];
    mLastRn = lastDy;
    double vn[N]; M::Recon(mS1, base, f, vn); bool finite = true; for (int i = 0; i < N; ++i) finite = finite && std::isfinite(vn[i]);
    if (!conv && (!finite || !(lastDy < 1e-2))) return false;   // a failed step does not touch the state: the caller repeats it with shorter steps
    HHP(6);   // Recon
    for (int i = 0; i < K; ++i) { mVprev[i] = mV[M::kS[i]]; mF0[i] = f[i]; }
    for (int i = 0; i < N; ++i) mV[i] = vn[i];
    mHaveF0 = conv;
    return true;
  }

  // state snapshot (a failed step is repeated with shorter steps)
  struct Snap { double v[N]; double p[K]; };
  void Save(Snap& sn) const { for (int i = 0; i < N; ++i) sn.v[i] = mV[i]; for (int i = 0; i < K; ++i) sn.p[i] = mVprev[i]; }
  void Restore(const Snap& sn) { for (int i = 0; i < N; ++i) mV[i] = sn.v[i]; for (int i = 0; i < K; ++i) mVprev[i] = sn.p[i]; mHaveF0 = false; }
  void CopyStateFrom(const ReducedNet& o) { for (int i = 0; i < N; ++i) mV[i] = o.mV[i]; for (int i = 0; i < K; ++i) mVprev[i] = o.mVprev[i]; mHaveF0 = false; }
  bool Finite() const { for (int i = 0; i < N; ++i) if (!std::isfinite(mV[i])) return false; return true; }
  double mExtrap = 1.0, mTol = 6e-3, mLastRn = 0.0, mTheta = 0.52;   // theta of the trapezoid rule: 0.52 takes a little of the ringing of the stiff nodes out; with 6 core steps per host sample this is the best of 0.50 .. 0.60 against NGSpice (OH -32.3, CH -31 dB), 0.50 gives -31 / -29.5

private:
  void SyncPrev() { for (int i = 0; i < K; ++i) mVprev[i] = mV[M::kS[i]]; mHaveF0 = false; }
  static double Kind(const double* vf, int kind) { return kind == 1 ? 0.0 : vf[kind]; }

  double mV[N], mH = 0.0, mMinv[K][K], mF0[K] = {}; bool mHaveF0 = false; double mVprev[K] = {};
  double mPhi[N * N], mAi[N * N], mS0[N * K], mS1[N * K], mGv[N * N];
};

// Linear block C x' + G x = kInG * vin * e_kInRow + B0 (the output stages, op-amp linearised) with the level pot between kPotA and kPotB: trapezoid with a fixed step, the inverse is rebuilt when the step or the pot changes.
template <class M> class LinBlock
{
public:
  static constexpr int N = M::kN;
  LinBlock() { Build(1.0 / 48000.0, 1.0); Reset(0.0); }
  double V(int i) const { return mX[i]; }
  // vr: pot position 0 .. 1 (Rmax * vr, at least 1 Ohm)
  void Build(double h, double vr)
  {
    double G[N][N], A[N][N]; const double g0 = 1.0 / std::max(M::kRmax * M::kVrDump, 1.0), g1 = 1.0 / std::max(M::kRmax * vr, 1.0), dg = g1 - g0;
    for (int i = 0; i < N; ++i) for (int j = 0; j < N; ++j) G[i][j] = M::kG[i * N + j];
    G[M::kPotA][M::kPotA] += dg; G[M::kPotB][M::kPotB] += dg; G[M::kPotA][M::kPotB] -= dg; G[M::kPotB][M::kPotA] -= dg;
    for (int i = 0; i < N; ++i) for (int j = 0; j < N; ++j) { A[i][j] = M::kC[i * N + j] / h + 0.5 * G[i][j]; mB[i][j] = M::kC[i * N + j] / h - 0.5 * G[i][j]; mG[i][j] = G[i][j]; }
    Invert(A, mAinv);
    // sparse copies for the step (the matrices of these small networks are about 40 % full)
    double mx = 0.0; for (int i = 0; i < N; ++i) for (int j = 0; j < N; ++j) mx = std::max(mx, std::max(std::fabs(mAinv[i][j]), std::fabs(mB[i][j])));
    const double thr = 1e-13 * mx; int na = 0, nb = 0;
    for (int i = 0; i < N; ++i)
    {
      mARow[i] = na; mBRow[i] = nb;
      for (int j = 0; j < N; ++j) { if (std::fabs(mAinv[i][j]) > thr) { mACol[na] = j; mAVal[na++] = mAinv[i][j]; } if (std::fabs(mB[i][j]) > thr) { mBCol[nb] = j; mBVal[nb++] = mB[i][j]; } }
    }
    mARow[N] = na; mBRow[N] = nb;
  }
  // DC state for the input vin
  void Reset(double vin)
  {
    double A[N][N], b[N]; for (int i = 0; i < N; ++i) { b[i] = M::kB0[i]; for (int j = 0; j < N; ++j) A[i][j] = mG[i][j]; }
    b[M::kInRow] += M::kInG * vin; detail::Solve<N>(A, b); for (int i = 0; i < N; ++i) mX[i] = b[i];
  }
  void Step(double vin0, double vin1)
  {
    double rhs[N]; for (int i = 0; i < N; ++i) { double s = M::kB0[i]; for (int p = mBRow[i]; p < mBRow[i + 1]; ++p) s += mBVal[p] * mX[mBCol[p]]; rhs[i] = s; }
    rhs[M::kInRow] += 0.5 * M::kInG * (vin0 + vin1);
    for (int i = 0; i < N; ++i) { double s = 0.0; for (int p = mARow[i]; p < mARow[i + 1]; ++p) s += mAVal[p] * rhs[mACol[p]]; mX[i] = s; }
  }
  double Out() const { return mX[M::kOut]; }
  void CopyStateFrom(const LinBlock& o) { for (int i = 0; i < N; ++i) mX[i] = o.mX[i]; }

private:
  static void Invert(double (*A)[N], double (*Ai)[N])
  {
    double W[N][2 * N];
    for (int i = 0; i < N; ++i) for (int j = 0; j < N; ++j) { W[i][j] = A[i][j]; W[i][N + j] = i == j ? 1.0 : 0.0; }
    for (int c = 0; c < N; ++c)
    {
      int p = c; for (int r = c + 1; r < N; ++r) if (std::fabs(W[r][c]) > std::fabs(W[p][c])) p = r;
      if (p != c) for (int j = 0; j < 2 * N; ++j) std::swap(W[c][j], W[p][j]);
      const double d = 1.0 / W[c][c]; for (int j = 0; j < 2 * N; ++j) W[c][j] *= d;
      for (int r = 0; r < N; ++r) if (r != c) { const double f = W[r][c]; if (f != 0.0) for (int j = 0; j < 2 * N; ++j) W[r][j] -= f * W[c][j]; }
    }
    for (int i = 0; i < N; ++i) for (int j = 0; j < N; ++j) Ai[i][j] = W[i][N + j];
  }
  double mAinv[N][N], mB[N][N], mG[N][N], mX[N];
  int mARow[N + 1], mACol[N * N], mBRow[N + 1], mBCol[N * N]; double mAVal[N * N], mBVal[N * N];
};

}  // namespace hhfast
}  // namespace dgg
