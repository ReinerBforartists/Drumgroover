// PotSet.h - pot dependent part of a reduced 909 voice model, assembled from generated pot data (tools: gen_pot_header.py, handoff 2026-11-06).
//
// The reduction (full_core2 / gen_full_model2) is exact; the pots are resistors, G(x) = G0 + U diag(D(x)) U^T with D_k = 1/R_k(x) - 1/R_k(0.5) for the K stamped pot resistors.
// All big products are pre-computed by the generator (K x n factors); here only a K x K system and (n x K)(K x m) products are evaluated, no inversion of large matrices.
//   Minv = Dm (I + Y1 Dm)^-1,  DQr = D .* Qr_T,  E1 = Y1 DQr,  E2 = Minv F2G,  E3 = Minv E1,  G1 = Minv F2RPg
//   dA  = -(CPr D) Qr_T + CW (DQr - E2 - E3) + (CPr D)(F2G + E1 - Y1 E2 - Y1 E3)
//   dBg = CW G1 - (CPr D) F2RPg + (CPr D)(Y1 G1)
//   dXz = VnF1 (E2 - DQr + E3)   (rows S and the output row),  dDg = -VnF1 G1   (rows S and the output row)
// The base arrays (all pots at the nominal position of the Nava deck) are those of the model header M; the corrections are added to them. Capacitor states are not touched here.
// Thread use: Assemble() is a pure function of the pot positions (own storage per PotSet); the engine takes the pointers with FullFast2::SetPot().
#pragma once
#include <cmath>
#include <vector>
#include "FullFast2.h"

namespace dgg { namespace pcmfast {

template <class M, class P> struct PotSet
{
  static constexpr int R = M::kR, NG = M::kNg, NS = M::kNS, NF = M::kNF, NU = M::kU, K = P::kPotK, NPOT = P::kPotN;
  std::vector<double> A, Bg, XzS, DgS, Xo, Dof, Dou; double Do1 = 0.0;

  // pots[i], i < kPotN, positions 0..1 of the knobs in the order of the generator (LT / MT / HT: Tune, Decay, Level; BD: Tune, Attack, Level, Decay; SD: Tune, Tone, Level, Snappy)
  void Assemble(const double* pots)
  {
    double D[K]; for (int k = 0; k < K; ++k) { double x = pots[P::kPotIdx[k]]; x = x < 0.001 ? 0.001 : (x > 1.0 ? 1.0 : x); const double xe = std::pow(x, P::kPotE[k]); const double Rk = P::kPotKind[k] == 0 ? P::kPotR[k] * xe : P::kPotR[k] * (1.0 - xe) + 1e-3; D[k] = 1.0 / Rk - P::kPotG0[k]; }
    // Minv = Dm (I + Y1 Dm)^-1 (Gauss-Jordan with partial pivoting, K <= 5)
    double T[K][2 * K]; for (int i = 0; i < K; ++i) for (int j = 0; j < K; ++j) { T[i][j] = (i == j ? 1.0 : 0.0) + P::kPotY1[i * K + j] * D[j]; T[i][K + j] = i == j ? 1.0 : 0.0; }
    for (int c = 0; c < K; ++c)
    {
      int pv = c; for (int r = c + 1; r < K; ++r) if (std::fabs(T[r][c]) > std::fabs(T[pv][c])) pv = r;
      if (pv != c) for (int j = 0; j < 2 * K; ++j) std::swap(T[c][j], T[pv][j]);
      const double d = 1.0 / T[c][c]; for (int j = 0; j < 2 * K; ++j) T[c][j] *= d;
      for (int r = 0; r < K; ++r) if (r != c) { const double f = T[r][c]; if (f != 0.0) for (int j = 0; j < 2 * K; ++j) T[r][j] -= f * T[c][j]; }
    }
    double Minv[K][K], Y1[K][K]; for (int i = 0; i < K; ++i) for (int j = 0; j < K; ++j) { Minv[i][j] = D[i] * T[i][K + j]; Y1[i][j] = P::kPotY1[i * K + j]; }
    // K x R and K x NG blocks
    std::vector<double> DQr(K * R), E1(K * R), E2(K * R), E3(K * R), W1(K * R), W2(K * R), G1(K * NG), Q(K * R), Ytmp(K * NG);
    auto KxK_times = [&](const double (*L)[K], const double* B, int m, double* out) { for (int i = 0; i < K; ++i) for (int j = 0; j < m; ++j) { double s = 0.0; for (int l = 0; l < K; ++l) s += L[i][l] * B[l * m + j]; out[i * m + j] = s; } };
    for (int k = 0; k < K; ++k) for (int j = 0; j < R; ++j) DQr[k * R + j] = D[k] * P::kPotQr_T[k * R + j];
    KxK_times(Y1, DQr.data(), R, E1.data()); KxK_times(Minv, P::kPotF2G, R, E2.data()); KxK_times(Minv, E1.data(), R, E3.data()); KxK_times(Minv, P::kPotF2RPg, NG, G1.data());
    std::vector<double> Y1E2(K * R), Y1E3(K * R); KxK_times(Y1, E2.data(), R, Y1E2.data()); KxK_times(Y1, E3.data(), R, Y1E3.data());
    for (int i = 0; i < K * R; ++i) { W1[i] = DQr[i] - E2[i] - E3[i]; W2[i] = P::kPotF2G[i] + E1[i] - Y1E2[i] - Y1E3[i]; Q[i] = E2[i] - DQr[i] + E3[i]; }
    // dA (R x R)
    A.assign(R * R, 0.0);
    for (int i = 0; i < R; ++i)
    {
      double cpd[K]; for (int k = 0; k < K; ++k) cpd[k] = P::kPotCPr[i * K + k] * D[k];
      for (int j = 0; j < R; ++j)
      {
        double s = 0.0; for (int k = 0; k < K; ++k) s += -cpd[k] * P::kPotQr_T[k * R + j] + P::kPotCW[i * K + k] * W1[k * R + j] + cpd[k] * W2[k * R + j];
        A[i * R + j] = M::kA[i * R + j] + s;
      }
    }
    // dBg (R x NG): CW G1 - (CPr D) F2RPg + (CPr D)(Y1 G1)
    KxK_times(Y1, G1.data(), NG, Ytmp.data()); Bg.assign(R * NG, 0.0);
    for (int i = 0; i < R; ++i)
    {
      double cpd[K]; for (int k = 0; k < K; ++k) cpd[k] = P::kPotCPr[i * K + k] * D[k];
      for (int j = 0; j < NG; ++j)
      {
        double s = 0.0; for (int k = 0; k < K; ++k) s += P::kPotCW[i * K + k] * G1[k * NG + j] - cpd[k] * P::kPotF2RPg[k * NG + j] + cpd[k] * Ytmp[k * NG + j];
        Bg[i * NG + j] = M::kBg[i * NG + j] + s;
      }
    }
    // XzS, DgS (S rows) and the output row
    XzS.assign(NS * R, 0.0); DgS.assign(NS * NG, 0.0);
    for (int s = 0; s < NS; ++s)
    {
      for (int j = 0; j < R; ++j) { double v = 0.0; for (int k = 0; k < K; ++k) v += P::kPotVnF1_S[s * K + k] * Q[k * R + j]; XzS[s * R + j] = M::kXzS[s * R + j] + v; }
      for (int j = 0; j < NG; ++j) { double v = 0.0; for (int k = 0; k < K; ++k) v += P::kPotVnF1_S[s * K + k] * G1[k * NG + j]; DgS[s * NG + j] = M::kDgS[s * NG + j] - v; }
    }
    Xo.assign(R, 0.0); for (int j = 0; j < R; ++j) { double v = 0.0; for (int k = 0; k < K; ++k) v += P::kPotVnF1_o[k] * Q[k * R + j]; Xo[j] = M::kXo[j] + v; }
    double dDo[NG]; for (int j = 0; j < NG; ++j) { double v = 0.0; for (int k = 0; k < K; ++k) v += P::kPotVnF1_o[k] * G1[k * NG + j]; dDo[j] = -v; }
    Dou.assign(NU, 0.0); for (int u = 0; u < NU; ++u) Dou[u] = M::kDou[u] + dDo[u];
    Dof.assign(NF, 0.0); for (int k = 0; k < NF; ++k) { double v = M::kDof[k]; for (int s = 0; s < NS; ++s) v += dDo[NU + s] * M::kTc[s * NF + k]; Dof[k] = v; }
    Do1 = M::kDo1 + dDo[NG - 1];
  }
  typename FullFast2<M>::PotPtrs Ptrs() const { return { A.data(), Bg.data(), XzS.data(), DgS.data(), Xo.data(), Dof.data(), Dou.data(), Do1 }; }
};

} }   // namespace dgg::pcmfast
