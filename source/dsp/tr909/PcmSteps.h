// Step tables of a PCM voice model (Crash / Ride) for a given PCM clock period, computed at run time from the linear model of the header (kA, kBg, ...): the same composition the generator (gen_crash_model.py) does for the
// nominal period, with the matrix exponential of the first-order-hold Van Loan matrix of every segment. The clock of the voice is a pot (Crash VR25 / Ride VR27, the PCM clock oscillator), the analog chain stays the same:
// only the step lengths change. Building takes a few milliseconds (never on the audio thread of a hit that is already playing).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace dgg { namespace pcmfast {

// Dense row-major helpers (n up to about 60).
inline std::vector<double> MatMul(const std::vector<double>& a, int m, int k, const std::vector<double>& b, int n)
{
  std::vector<double> c(static_cast<size_t>(m) * n, 0.0);
  for (int i = 0; i < m; ++i) for (int l = 0; l < k; ++l) { const double v = a[static_cast<size_t>(i) * k + l]; if (v == 0.0) continue; for (int j = 0; j < n; ++j) c[static_cast<size_t>(i) * n + j] += v * b[static_cast<size_t>(l) * n + j]; }
  return c;
}

// Solves A X = B (n x n, n x m) by Gaussian elimination with partial pivoting.
inline std::vector<double> SolveDense(std::vector<double> a, std::vector<double> b, int n, int m)
{
  for (int c = 0; c < n; ++c)
  {
    int pv = c; for (int r = c + 1; r < n; ++r) if (std::fabs(a[static_cast<size_t>(r) * n + c]) > std::fabs(a[static_cast<size_t>(pv) * n + c])) pv = r;
    if (pv != c) { for (int j = 0; j < n; ++j) std::swap(a[static_cast<size_t>(c) * n + j], a[static_cast<size_t>(pv) * n + j]); for (int j = 0; j < m; ++j) std::swap(b[static_cast<size_t>(c) * m + j], b[static_cast<size_t>(pv) * m + j]); }
    const double d = 1.0 / a[static_cast<size_t>(c) * n + c];
    for (int r = c + 1; r < n; ++r)
    {
      const double f = a[static_cast<size_t>(r) * n + c] * d; if (f == 0.0) continue;
      for (int j = c; j < n; ++j) a[static_cast<size_t>(r) * n + j] -= f * a[static_cast<size_t>(c) * n + j];
      for (int j = 0; j < m; ++j) b[static_cast<size_t>(r) * m + j] -= f * b[static_cast<size_t>(c) * m + j];
    }
  }
  for (int c = n - 1; c >= 0; --c)
    for (int j = 0; j < m; ++j)
    {
      double v = b[static_cast<size_t>(c) * m + j]; for (int l = c + 1; l < n; ++l) v -= a[static_cast<size_t>(c) * n + l] * b[static_cast<size_t>(l) * m + j];
      b[static_cast<size_t>(c) * m + j] = v / a[static_cast<size_t>(c) * n + c];
    }
  return b;
}

// Matrix exponential (scaling and squaring with the degree 13 Pade approximant, Higham 2005).
inline std::vector<double> ExpmDense(std::vector<double> a, int n)
{
  static const double b[14] = {64764752532480000., 32382376266240000., 7771770303897600., 1187353796428800., 129060195264000., 10559470521600., 670442572800., 33522128640., 1323241920., 40840800., 960960., 16380., 182., 1.};
  double norm1 = 0.0; for (int j = 0; j < n; ++j) { double s = 0.0; for (int i = 0; i < n; ++i) s += std::fabs(a[static_cast<size_t>(i) * n + j]); norm1 = std::max(norm1, s); }
  int sq = 0; if (norm1 > 5.371920351148152) sq = std::max(0, static_cast<int>(std::ceil(std::log2(norm1 / 5.371920351148152))));
  const double sc = std::ldexp(1.0, -sq); for (double& v : a) v *= sc;
  const std::vector<double> a2 = MatMul(a, n, n, a, n), a4 = MatMul(a2, n, n, a2, n), a6 = MatMul(a4, n, n, a2, n);
  const size_t nn = static_cast<size_t>(n) * n; std::vector<double> t(nn), u, v(nn);
  for (size_t i = 0; i < nn; ++i) t[i] = b[13] * a6[i] + b[11] * a4[i] + b[9] * a2[i];
  t = MatMul(a6, n, n, t, n);
  for (size_t i = 0; i < nn; ++i) t[i] += b[7] * a6[i] + b[5] * a4[i] + b[3] * a2[i];
  for (int i = 0; i < n; ++i) t[static_cast<size_t>(i) * n + i] += b[1];
  u = MatMul(a, n, n, t, n);
  for (size_t i = 0; i < nn; ++i) t[i] = b[12] * a6[i] + b[10] * a4[i] + b[8] * a2[i];
  t = MatMul(a6, n, n, t, n);
  for (size_t i = 0; i < nn; ++i) v[i] = t[i] + b[6] * a6[i] + b[4] * a4[i] + b[2] * a2[i];
  for (int i = 0; i < n; ++i) v[static_cast<size_t>(i) * n + i] += b[0];
  std::vector<double> p(nn), q(nn); for (size_t i = 0; i < nn; ++i) { p[i] = v[i] + u[i]; q[i] = v[i] - u[i]; }
  std::vector<double> e = SolveDense(q, p, n, n);
  for (int i = 0; i < sq; ++i) e = MatMul(e, n, n, e, n);
  return e;
}

// The step tables of model M for the PCM period T (seconds): step lengths h, Phi, GaF, GbF, Cone, Zp, and the input columns GaUc / GbUc / GsUc (NU x R per step, column-major as the header).
template <class M> struct PcmSteps
{
  static constexpr int R = M::kR, NU = M::kU, NP = M::kPorts, NF = M::kCur, NSn = M::kNS, NG = M::kNg, NST = M::kSteps;
  double T = 0.0;
  std::vector<double> h, phi, gaF, gbF, cone, zp, gaUc, gbUc, gsUc;
  void Build(double period)
  {
    T = period; h.assign(NST, 0.0); phi.assign(static_cast<size_t>(NST) * R * R, 0.0); gaF.assign(static_cast<size_t>(NST) * R * NF, 0.0); gbF = gaF; cone.assign(static_cast<size_t>(NST) * R, 0.0);
    zp.assign(static_cast<size_t>(NST) * NP * NF, 0.0); gaUc.assign(static_cast<size_t>(NST) * NU * R, 0.0); gbUc = gaUc; gsUc = gaUc;
    const std::vector<double> A(M::kA, M::kA + R * R), Bg(M::kBg, M::kBg + R * NG), Tc(M::kTc, M::kTc + NSn * NF), Pp(M::kPpA, M::kPpA + NP * NSn), XzS(M::kXzSm, M::kXzSm + NSn * R), DgW(M::kDgSW, M::kDgSW + NSn * NSn);
    const int NN = R + 2 * NG;
    for (int j = 0; j < NST; ++j)
    {
      double H = 0.0; for (int s = M::kSegFirst[j]; s < M::kSegFirst[j + 1]; ++s) H += M::kSegFrac[s] * T;
      std::vector<double> P(static_cast<size_t>(R) * R, 0.0), Qo(static_cast<size_t>(R) * NU, 0.0), Qn = Qo, S0(static_cast<size_t>(R) * NSn, 0.0), S1 = S0, cc(R, 0.0);
      for (int i = 0; i < R; ++i) P[static_cast<size_t>(i) * R + i] = 1.0;
      double tau = 0.0;
      for (int s = M::kSegFirst[j]; s < M::kSegFirst[j + 1]; ++s)
      {
        const double hs = M::kSegFrac[s] * T; const bool ramp = M::kSegRamp[s] != 0;
        std::vector<double> N(static_cast<size_t>(NN) * NN, 0.0);
        for (int i = 0; i < R; ++i) { for (int k = 0; k < R; ++k) N[static_cast<size_t>(i) * NN + k] = A[static_cast<size_t>(i) * R + k] * hs; for (int k = 0; k < NG; ++k) N[static_cast<size_t>(i) * NN + R + k] = Bg[static_cast<size_t>(i) * NG + k] * hs; }
        for (int k = 0; k < NG; ++k) N[static_cast<size_t>(R + k) * NN + R + NG + k] = hs;
        const std::vector<double> E = ExpmDense(N, NN);
        auto at = [&](int i, int k) { return E[static_cast<size_t>(i) * NN + k]; };
        auto Ga = [&](int i, int k) { return at(i, R + k) - at(i, R + NG + k) / hs; };   // columns of the input vector g = (u, W, 1)
        auto Gb = [&](int i, int k) { return at(i, R + NG + k) / hs; };
        std::vector<double> Phi(static_cast<size_t>(R) * R); for (int i = 0; i < R; ++i) for (int k = 0; k < R; ++k) Phi[static_cast<size_t>(i) * R + k] = at(i, k);
        const double ws = tau / H, we = (tau + hs) / H;
        P = MatMul(Phi, R, R, P, R); Qo = MatMul(Phi, R, R, Qo, NU); Qn = MatMul(Phi, R, R, Qn, NU); S0 = MatMul(Phi, R, R, S0, NSn); S1 = MatMul(Phi, R, R, S1, NSn); cc = MatMul(Phi, R, R, cc, 1);
        for (int i = 0; i < R; ++i)
        {
          for (int k = 0; k < NU; ++k) { if (ramp) { Qo[static_cast<size_t>(i) * NU + k] += Ga(i, k); Qn[static_cast<size_t>(i) * NU + k] += Gb(i, k); } else Qn[static_cast<size_t>(i) * NU + k] += Ga(i, k) + Gb(i, k); }
          for (int k = 0; k < NSn; ++k) { S0[static_cast<size_t>(i) * NSn + k] += Ga(i, NU + k) * (1.0 - ws) + Gb(i, NU + k) * (1.0 - we); S1[static_cast<size_t>(i) * NSn + k] += Ga(i, NU + k) * ws + Gb(i, NU + k) * we; }
          cc[i] += Ga(i, NG - 1) + Gb(i, NG - 1);
        }
        tau += hs;
      }
      std::vector<double> Zw = MatMul(XzS, NSn, R, S1, NSn); for (size_t i = 0; i < Zw.size(); ++i) Zw[i] += DgW[i];
      const std::vector<double> gaFj = MatMul(S0, R, NSn, Tc, NF), gbFj = MatMul(S1, R, NSn, Tc, NF), zpj = MatMul(MatMul(Pp, NP, NSn, Zw, NSn), NP, NSn, Tc, NF);
      h[j] = H; std::copy(P.begin(), P.end(), phi.begin() + static_cast<size_t>(j) * R * R); std::copy(gaFj.begin(), gaFj.end(), gaF.begin() + static_cast<size_t>(j) * R * NF); std::copy(gbFj.begin(), gbFj.end(), gbF.begin() + static_cast<size_t>(j) * R * NF);
      std::copy(cc.begin(), cc.end(), cone.begin() + static_cast<size_t>(j) * R); std::copy(zpj.begin(), zpj.end(), zp.begin() + static_cast<size_t>(j) * NP * NF);
      for (int k = 0; k < NU; ++k) for (int i = 0; i < R; ++i)
      {
        const size_t o = (static_cast<size_t>(j) * NU + k) * R + i; gaUc[o] = Qo[static_cast<size_t>(i) * NU + k]; gbUc[o] = Qn[static_cast<size_t>(i) * NU + k]; gsUc[o] = gaUc[o] + gbUc[o];
      }
    }
  }
};

}}  // namespace dgg::pcmfast
