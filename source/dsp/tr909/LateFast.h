// Fast model of a TR-909 voice after its trigger phase (autonomous: all sources constant): reduced continuous model (late_core.py / gen_late_model.py) integrated with the
// exponential integrator at a fixed step h (the matrix exponential of the augmented system is computed at Init), BJT and diode currents interpolated linearly over the step,
// Newton on the port voltages. State and ports at the start (time t1 of the engine run) come from tables over the accent.
#pragma once
#include <cmath>
#include <cstdint>
#include <vector>
#include "PcmFast.h"

namespace dgg { namespace pcmfast {

// dense helpers (row-major, small)
inline void MatMulN(const double* a, const double* b, double* c, int n, int m, int k)   // c(n x k) = a(n x m) b(m x k)
{
  for (int i = 0; i < n; ++i) for (int j = 0; j < k; ++j) { double s = 0.0; for (int l = 0; l < m; ++l) s += a[i * m + l] * b[l * k + j]; c[i * k + j] = s; }
}
inline bool SolveN(std::vector<double> A, std::vector<double>& B, int n, int nrhs)   // A x = B, B overwritten with x (partial pivoting)
{
  for (int c = 0; c < n; ++c)
  {
    int pr = c; double mx = std::fabs(A[c * n + c]); for (int i = c + 1; i < n; ++i) if (std::fabs(A[i * n + c]) > mx) { mx = std::fabs(A[i * n + c]); pr = i; }
    if (mx < 1e-300) return false;
    if (pr != c) { for (int j = 0; j < n; ++j) std::swap(A[c * n + j], A[pr * n + j]); for (int j = 0; j < nrhs; ++j) std::swap(B[c * nrhs + j], B[pr * nrhs + j]); }
    for (int i = c + 1; i < n; ++i) { const double f = A[i * n + c] / A[c * n + c]; if (f == 0.0) continue; for (int j = c; j < n; ++j) A[i * n + j] -= f * A[c * n + j]; for (int j = 0; j < nrhs; ++j) B[i * nrhs + j] -= f * B[c * nrhs + j]; }
  }
  for (int i = n - 1; i >= 0; --i) for (int j = 0; j < nrhs; ++j) { double s = B[i * nrhs + j]; for (int l = i + 1; l < n; ++l) s -= A[i * n + l] * B[l * nrhs + j]; B[i * nrhs + j] = s / A[i * n + i]; }
  return true;
}
// matrix exponential, Pade [13/13] with scaling and squaring (Higham 2005)
inline std::vector<double> Expm(const std::vector<double>& M, int n)
{
  static const double b[14] = {64764752532480000.0, 32382376266240000.0, 7771770303897600.0, 1187353796428800.0, 129060195264000.0, 10559470521600.0, 670442572800.0, 33522128640.0,
                               1323241920.0, 40840800.0, 960960.0, 16380.0, 182.0, 1.0};
  double nrm = 0.0; for (int j = 0; j < n; ++j) { double s = 0.0; for (int i = 0; i < n; ++i) s += std::fabs(M[i * n + j]); nrm = std::fmax(nrm, s); }
  int sq = 0; if (nrm > 5.371920351148152) sq = (int)std::ceil(std::log2(nrm / 5.371920351148152));
  std::vector<double> A(M); const double sc = std::ldexp(1.0, -sq); for (double& x : A) x *= sc;
  const int nn = n * n; std::vector<double> A2(nn), A4(nn), A6(nn), U(nn), V(nn), T1(nn), T2(nn), I(nn, 0.0);
  for (int i = 0; i < n; ++i) I[i * n + i] = 1.0;
  MatMulN(A.data(), A.data(), A2.data(), n, n, n); MatMulN(A2.data(), A2.data(), A4.data(), n, n, n); MatMulN(A2.data(), A4.data(), A6.data(), n, n, n);
  // U = A (A6 (b13 A6 + b11 A4 + b9 A2) + b7 A6 + b5 A4 + b3 A2 + b1 I)
  for (int i = 0; i < nn; ++i) T1[i] = b[13] * A6[i] + b[11] * A4[i] + b[9] * A2[i];
  MatMulN(A6.data(), T1.data(), T2.data(), n, n, n);
  for (int i = 0; i < nn; ++i) T2[i] += b[7] * A6[i] + b[5] * A4[i] + b[3] * A2[i] + b[1] * I[i];
  MatMulN(A.data(), T2.data(), U.data(), n, n, n);
  for (int i = 0; i < nn; ++i) T1[i] = b[12] * A6[i] + b[10] * A4[i] + b[8] * A2[i];
  MatMulN(A6.data(), T1.data(), T2.data(), n, n, n);
  for (int i = 0; i < nn; ++i) V[i] = T2[i] + b[6] * A6[i] + b[4] * A4[i] + b[2] * A2[i] + b[0] * I[i];
  std::vector<double> P(nn), Q(nn); for (int i = 0; i < nn; ++i) { P[i] = V[i] + U[i]; Q[i] = V[i] - U[i]; }
  std::vector<double> R = P; SolveN(Q, R, n, n);
  for (int s = 0; s < sq; ++s) { MatMulN(R.data(), R.data(), T1.data(), n, n, n); R = T1; }
  return R;
}

#ifdef FULLFAST2_PROF
inline unsigned long long g_dioCalls = 0, g_dioRsCalls = 0, g_dioIters = 0, g_dioIterHist[12] = {0};
#endif
inline bool g_dioWarm = false;   // DIOWARM: the series-resistance loop starts from the junction voltage of the previous call of the same diode
inline void DiodeEval(const double* q, double v, double& i, double& g, double* vjHint = nullptr, const double* xbvPre = nullptr)   // Is, N, Rs, Bv, Ibv: the engine's dioRs/dioJunction
{
  const double Is = q[0], N = q[1], Rs = q[2], Bv = q[3], Ibv = q[4], nvt = N * kVt;
  auto junction = [&](double vd, double& ii, double& gg)
  {
    const double xbv = xbvPre ? *xbvPre : (Bv > 1e20 ? 1e30 : Bv - nvt * std::log(std::fmax(Ibv / Is, 1.0)));   // xbvPre: the same expression, evaluated once per diode
    if (vd > -xbv) { const double e = std::exp(std::fmin(vd / nvt, 80.0)); ii = Is * (e - 1.0) + kGmin * vd; gg = Is * e / nvt + kGmin; }
    else { const double e = std::exp(std::fmin(-(xbv + vd) / nvt, 80.0)); ii = -Is * e + kGmin * vd; gg = Is * e / nvt + kGmin; }
  };
#ifdef FULLFAST2_PROF
  ++g_dioCalls;
#endif
  if (Rs <= 0.0) { junction(v, i, g); return; }
#ifdef FULLFAST2_PROF
  ++g_dioRsCalls;
#endif
  double vj = (g_dioWarm && vjHint && std::fabs(*vjHint - v) < 1.0) ? *vjHint : v, ii, gg;
  for (int k = 0; k < 80; ++k)
  {
#ifdef FULLFAST2_PROF
    ++g_dioIters;
#endif
    junction(vj, ii, gg); double dv = (vj + ii * Rs - v) / (1.0 + gg * Rs); const double lim = 6.0 * nvt; dv = dv > lim ? lim : (dv < -lim ? -lim : dv); vj -= dv;
    if (std::fabs(dv) < 1e-13) break;
  }
  junction(vj, ii, gg); i = ii; g = gg / (1.0 + gg * Rs); if (vjHint) *vjHint = vj;
}

template <class M> struct LateFast
{
  static constexpr int R = M::kR, NG = M::kNg, NS = M::kNS, NF = M::kNF, NP = M::kNP, ND = M::kNumDev;
  double h = 0.0, acc = M::kAcc0;
  double Phi[R * R], GaF[R * NF], GbF[R * NF], ConeC[R], ConeU[R], Zp[NP * NF], PXz[NP * R], pConst[NP], coneEff[R];
  double z[R], f0[NF], p[NP], bp[M::kNumBjt > 0 ? M::kNumBjt : 1][16];
  double newtonTol = 1e-9; long steps = 0, its = 0; double lastOut = 0.0;

  void Init(double step)
  {
    h = step;
    for (int k = 0; k < M::kNumBjt; ++k)
    {
      const double* q = &M::kBjt[9 * k]; for (int i = 0; i < 9; ++i) bp[k][i] = q[i];
      bp[k][9] = 1.0 / (q[4] * kVt); bp[k][10] = 1.0 / (q[7] * kVt); bp[k][11] = 1.0 / q[5]; bp[k][12] = 1.0 / q[6]; bp[k][13] = 1.0 / q[2]; bp[k][14] = 1.0 / q[3]; bp[k][15] = 0.0;
    }
    const int n = R + 2 * NG; std::vector<double> N(n * n, 0.0);
    for (int i = 0; i < R; ++i) { for (int j = 0; j < R; ++j) N[i * n + j] = M::kA[i * R + j] * h; for (int j = 0; j < NG; ++j) N[i * n + R + j] = M::kBg[i * NG + j] * h; }
    for (int j = 0; j < NG; ++j) N[(R + j) * n + R + NG + j] = h;   // h * I in the (g, g') block: the augmented system of the first-order hold
    const std::vector<double> E = Expm(N, n);
    // E[:R,:R] = Phi ; E[:R, R:R+NG] = E12 ; E[:R, R+NG:] = E13 ; Ga = E12 - E13/h*h ... in the scaled form E13 already carries the factor h: Ga = E12 - E13 / h, Gb = E13 / h (as the generator)
    // here N carries h in the I block, so E13 = int_0^h int ... ; use the same definition as gen_crash_model.seg_mats with the unscaled identity: see comment in Init of the generator
    for (int i = 0; i < R; ++i) for (int j = 0; j < R; ++j) Phi[i * R + j] = E[i * n + j];
    std::vector<double> Ga(R * NG), Gb(R * NG);
    for (int i = 0; i < R; ++i) for (int j = 0; j < NG; ++j) { const double e12 = E[i * n + R + j], e13 = E[i * n + R + NG + j]; Ga[i * NG + j] = e12 - e13 / h; Gb[i * NG + j] = e13 / h; }
    // inputs: u = accent (column 0), W columns 1..NS, constant column NG-1
    for (int i = 0; i < R; ++i)
    {
      ConeC[i] = Ga[i * NG + NG - 1] + Gb[i * NG + NG - 1]; ConeU[i] = Ga[i * NG + 0] + Gb[i * NG + 0];
      for (int k = 0; k < NF; ++k) { double sa = 0.0, sb = 0.0; for (int s = 0; s < NS; ++s) { sa += Ga[i * NG + 1 + s] * M::kTc[s * NF + k]; sb += Gb[i * NG + 1 + s] * M::kTc[s * NF + k]; } GaF[i * NF + k] = sa; GbF[i * NF + k] = sb; }
    }
    // ports: p = Pp (XzS zk + DgS_u acc + DgS_c) + pc + Zp f1,  Zp = Pp (XzS GbF + DgS_W Tc)
    std::vector<double> XG(NS * NF, 0.0);
    for (int s = 0; s < NS; ++s) for (int k = 0; k < NF; ++k) { double v = 0.0; for (int i = 0; i < R; ++i) v += M::kXzS[s * R + i] * GbF[i * NF + k]; for (int q = 0; q < NS; ++q) v += M::kDgS[s * NG + 1 + q] * M::kTc[q * NF + k]; XG[s * NF + k] = v; }
    for (int a = 0; a < NP; ++a) { for (int k = 0; k < NF; ++k) { double v = 0.0; for (int s = 0; s < NS; ++s) v += M::kPp[a * NS + s] * XG[s * NF + k]; Zp[a * NF + k] = v; }
      for (int i = 0; i < R; ++i) { double v = 0.0; for (int s = 0; s < NS; ++s) v += M::kPp[a * NS + s] * M::kXzS[s * R + i]; PXz[a * R + i] = v; } }
  }
  void Start(double accent)
  {
    acc = accent; int g = 0; while (g < M::kAccGrid - 2 && accent >= M::kAccV[g + 1]) ++g;
    double t = (accent - M::kAccV[g]) / (M::kAccV[g + 1] - M::kAccV[g]); t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
    for (int i = 0; i < R; ++i) z[i] = (1.0 - t) * M::kZ0[g * R + i] + t * M::kZ0[(g + 1) * R + i];
    for (int i = 0; i < NP; ++i) p[i] = (1.0 - t) * M::kP0[g * NP + i] + t * M::kP0[(g + 1) * NP + i];
    for (int i = 0; i < R; ++i) coneEff[i] = ConeC[i] + acc * ConeU[i];
    for (int a = 0; a < NP; ++a) { double v = M::kPc[a]; for (int s = 0; s < NS; ++s) v += M::kPp[a * NS + s] * (M::kDgS[s * NG + 0] * acc + M::kDgS[s * NG + NG - 1]); pConst[a] = v; }
    EvalF(p, f0, nullptr); steps = 0; its = 0; lastOut = Output(f0);
  }
  // currents and Jacobian d f / d p (NF x NP)
  void EvalF(const double* q, double* f, double* jf) const
  {
    if (jf) for (int i = 0; i < NF * NP; ++i) jf[i] = 0.0;
    for (int d = 0; d < ND; ++d)
    {
      const int pp = M::kDevPort[d], cc = M::kDevCur[d], idx = M::kDevIdx[d];
      if (M::kDevType[d] == 0)
      {
        const BjtOut o = BjtEval(bp[idx], q[pp], q[pp + 1]); f[cc] = o.ic; f[cc + 1] = o.ib;
        if (jf) { jf[cc * NP + pp] = o.d[0][0]; jf[cc * NP + pp + 1] = o.d[0][1]; jf[(cc + 1) * NP + pp] = o.d[1][0]; jf[(cc + 1) * NP + pp + 1] = o.d[1][1]; }
      }
      else { double i, g; DiodeEval(&M::kDio[5 * idx], q[pp], i, g); f[cc] = i; if (jf) jf[cc * NP + pp] = g; }
    }
  }
  double Output(const double* f) const
  {
    double y = M::kDo1 + M::kDou[0] * acc; for (int i = 0; i < R; ++i) y += M::kXo[i] * z[i]; for (int k = 0; k < NF; ++k) y += M::kDof[k] * f[k]; return y;
  }
  double Step()
  {
    double zk[R], pk[NP];
    for (int i = 0; i < R; ++i) { double a = coneEff[i]; for (int k = 0; k < R; ++k) a += Phi[i * R + k] * z[k]; for (int k = 0; k < NF; ++k) a += GaF[i * NF + k] * f0[k]; zk[i] = a; }
    for (int a = 0; a < NP; ++a) { double v = pConst[a]; for (int i = 0; i < R; ++i) v += PXz[a * R + i] * zk[i]; pk[a] = v; }
    double f1[NF], jf[NF * NP];
    for (int it = 0; it < 50; ++it)
    {
      EvalF(p, f1, jf); ++its;
      double F[NP], J[NP * NP];   // F = -(p - pk - Zp f1), J = I - Zp Jf
      for (int a = 0; a < NP; ++a)
      {
        double zf = 0.0; for (int k = 0; k < NF; ++k) zf += Zp[a * NF + k] * f1[k]; F[a] = -(p[a] - pk[a] - zf);
        for (int b = 0; b < NP; ++b) { double v = (a == b) ? 1.0 : 0.0; for (int k = 0; k < NF; ++k) v -= Zp[a * NF + k] * jf[k * NP + b]; J[a * NP + b] = v; }
      }
      std::vector<double> Fv(F, F + NP), Jv(J, J + NP * NP); SolveN(Jv, Fv, NP, 1);
      double mx = 0.0; for (int a = 0; a < NP; ++a) mx = std::fmax(mx, std::fabs(Fv[a]));
      const double sc = mx > 0.5 ? 0.5 / mx : 1.0; for (int a = 0; a < NP; ++a) p[a] += sc * Fv[a];
      if (mx < newtonTol) break;
    }
    EvalF(p, f1, nullptr);
    for (int i = 0; i < R; ++i) { double a = zk[i]; for (int k = 0; k < NF; ++k) a += GbF[i * NF + k] * f1[k]; z[i] = a; }
    for (int k = 0; k < NF; ++k) f0[k] = f1[k];
    ++steps; lastOut = Output(f1); return lastOut;
  }
};

}}  // namespace dgg::pcmfast
