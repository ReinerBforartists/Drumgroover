// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Reiner Prokein
#pragma once
// The AD net of the cymbal / ride model (cell 1 Q18 / D12, Band1 buffer Q25, mesh, tone, U6-B, level, output load) split in two parts:
//   core   cymodel::ad1, rows 0..7 of the AD net: the nonlinear part (reduced Newton like the other sub-networks);
//   tail   rows 8..25: linear (resistors, capacitors, the two op-amps as linear macro models): one precomputed matrix product per step, no Newton iteration.
// The only coupling between the two is the capacitor C58 (q25emit - r123node, 10 nF): the core sees r123node as a fixed node (the value of the tail, extrapolated linearly to the end of the step),
// the tail gets the voltage of q25emit of the step it just finished. The Band1 buffer is an emitter follower (output impedance about 25 Ohm against the 100 kOhm of the branch behind C58).
// The tail uses the same time stepping rule as the networks (theta rule, C / h + theta G), so the linear part is the same as before.
#include <cmath>
#include <algorithm>
#include "808CyModel.h"

namespace dgg { namespace cyfast {

class LinTail
{
 public:
  static constexpr int NT = 16, R0 = 8, NA = 24;   // rows R0 .. R0 + NT - 1 of the 24 unknowns of the AD net (cymodel::ad, pruned)
  double x[NT];
  // pots: tone (vr4e, meshx2) and level (u6bout - u6bm) as in the other networks; h: step size
  void Build(double h, double theta, double dgT1, double dgT2, double dgL)
  {
    using cymodel::ad;
    double G[NT][NT], C[NT][NT]; for (int i = 0; i < NT; ++i) for (int j = 0; j < NT; ++j) { G[i][j] = ad::kG[(R0 + i) * NA + R0 + j]; C[i][j] = ad::kC[(R0 + i) * NA + R0 + j]; }
    auto stamp = [&](int a, int b, double dg) { a -= R0; G[a][a] += dg; if (b >= 0) { b -= R0; G[b][b] += dg; G[a][b] -= dg; G[b][a] -= dg; } };
    stamp(ad::kRow_vr4e, -1, dgT1); stamp(ad::kRow_meshx2, -1, dgT2); stamp(ad::kRow_u6bout, ad::kRow_u6bm, dgL);
    double A[NT][NT], B[NT][NT];
    for (int i = 0; i < NT; ++i) for (int j = 0; j < NT; ++j) { A[i][j] = C[i][j] / h + theta * G[i][j]; B[i][j] = C[i][j] / h - (1.0 - theta) * G[i][j]; }
    double Ai[NT][NT]; Invert(A, Ai);
    for (int i = 0; i < NT; ++i) for (int j = 0; j < NT; ++j) { double s = 0.0; for (int k = 0; k < NT; ++k) s += Ai[i][k] * B[k][j]; mPhi[i][j] = s; }
    double b0[NT]; for (int i = 0; i < NT; ++i) b0[i] = ad::kB0[R0 + i];
    for (int i = 0; i < NT; ++i) { double s = 0.0; for (int k = 0; k < NT; ++k) s += Ai[i][k] * b0[k]; mC0[i] = s; }
    // inputs: 0 = q25emit through C58 (row r123node), 1 = u5bout (kind 12), 2 = u5aout (kind 11), through the capacitors of the CAPFIX entries
    for (int k = 0; k < 3; ++k) for (int i = 0; i < NT; ++i) mGam[k][i] = 0.0;
    const double c58 = -ad::kC[7 * NA + R0];   // coupling capacitor q25emit - r123node (row 7 / row 8 of the AD net)
    for (int i = 0; i < NT; ++i) mGam[0][i] = Ai[i][0] * c58 / h;
    for (int k = 0; k < ad::kNumCapFix; ++k)
    {
      const int row = ad::kCapFix[k][0] - R0, kind = ad::kCapFix[k][1]; if (row < 0) continue;
      const int in = kind == 12 ? 1 : (kind == 11 ? 2 : -1); if (in < 0) continue;
      for (int i = 0; i < NT; ++i) mGam[in][i] += Ai[i][row] * ad::kCapFixC[k] / h;
    }
    // DC state (capacitors open): G x = b0
    double Gi[NT][NT]; Invert(G, Gi); for (int i = 0; i < NT; ++i) { double s = 0.0; for (int k = 0; k < NT; ++k) s += Gi[i][k] * b0[k]; mDc[i] = s; }
  }
  void SetState(const double* v) { for (int i = 0; i < NT; ++i) x[i] = v[i]; }
  void DcState() { for (int i = 0; i < NT; ++i) x[i] = mDc[i]; }
  // one step: values of q25emit, u5aout, u5bout at the start (0) and the end (1) of the step
  void Step(double q0, double q1, double ua0, double ua1, double ub0, double ub1)
  {
    const double dq = q1 - q0, da = ua1 - ua0, db = ub1 - ub0; double xn[NT];
    for (int i = 0; i < NT; ++i)
    {
      double s = mC0[i] + mGam[0][i] * dq + mGam[1][i] * db + mGam[2][i] * da;
      for (int j = 0; j < NT; ++j) s += mPhi[i][j] * x[j];
      xn[i] = s;
    }
    for (int i = 0; i < NT; ++i) x[i] = xn[i];
  }

 private:
  static void Invert(const double (*A)[NT], double (*Ai)[NT])
  {
    double W[NT][2 * NT]; for (int i = 0; i < NT; ++i) for (int j = 0; j < NT; ++j) { W[i][j] = A[i][j]; W[i][NT + j] = i == j ? 1.0 : 0.0; }
    for (int c = 0; c < NT; ++c)
    {
      int p = c; for (int r = c + 1; r < NT; ++r) if (std::fabs(W[r][c]) > std::fabs(W[p][c])) p = r;
      if (p != c) for (int j = 0; j < 2 * NT; ++j) std::swap(W[c][j], W[p][j]);
      const double d = 1.0 / W[c][c]; for (int j = 0; j < 2 * NT; ++j) W[c][j] *= d;
      for (int r = 0; r < NT; ++r) if (r != c) { const double f = W[r][c]; if (f != 0.0) for (int j = 0; j < 2 * NT; ++j) W[r][j] -= f * W[c][j]; }
    }
    for (int i = 0; i < NT; ++i) for (int j = 0; j < NT; ++j) Ai[i][j] = W[i][NT + j];
  }
  double mPhi[NT][NT], mGam[3][NT], mC0[NT], mDc[NT];
};

// Drop-in for SubNet<cymodel::ad>: the same members and rows (V(row) for the rows of cymodel::ad), core + tail inside.
template <class Core>
class AdBlock
{
 public:
  Core core; LinTail tail;
  double vf0[16] = {0}, vf1[16] = {0};
  void Setup(double h, double theta, double dgT1, double dgT2, double dgL) { core.Setup(h, theta, [](auto&) {}); tail.Build(h, theta, dgT1, dgT2, dgL); }
  double V(int row) const { return row < 8 ? core.V(row) : tail.x[row - LinTail::R0]; }
  void SetState(const double* v) { core.SetState(v); tail.SetState(v + LinTail::R0); mR[0] = mR[1] = tail.x[0]; }
  void CopyStateFrom(const AdBlock& o)
  {
    core.CopyStateFrom(o.core); for (int i = 0; i < LinTail::NT; ++i) tail.x[i] = o.tail.x[i]; mR[0] = o.mR[0]; mR[1] = o.mR[1];
    for (int i = 0; i < 16; ++i) { vf0[i] = o.vf0[i]; vf1[i] = o.vf1[i]; }
  }
  void Save() { core.Save(); for (int i = 0; i < LinTail::NT; ++i) mSx[i] = tail.x[i]; mSr[0] = mR[0]; mSr[1] = mR[1]; }
  void Restore() { core.Restore(); for (int i = 0; i < LinTail::NT; ++i) tail.x[i] = mSx[i]; mR[0] = mSr[0]; mR[1] = mSr[1]; }
  bool Dc(const double* vf)
  {
    tail.DcState(); mR[0] = mR[1] = tail.x[0]; double t[16]; for (int i = 0; i < 16; ++i) t[i] = vf[i]; t[14] = tail.x[0];
    return core.Dc(t);
  }
  void Step()
  {
    // fixed nodes of the core: bp1out (8), c45node (10), r123node of the tail (14): its value at the start and the linear extrapolation to the end of the step
    for (int i = 0; i < 16; ++i) { core.vf0[i] = vf0[i]; core.vf1[i] = vf1[i]; }
    core.vf0[14] = mR[1]; core.vf1[14] = 2.0 * mR[1] - mR[0];
    const double q0 = core.V(7); core.Step(); const double q1 = core.V(7);
    tail.Step(q0, q1, vf0[11], vf1[11], vf0[12], vf1[12]);
    mR[0] = mR[1]; mR[1] = tail.x[0];
  }
  long StatFail() const { return core.statFail; }
  long StatDense() const { return core.statDense; }
 private:
  double mR[2] = {0, 0}, mSx[LinTail::NT] = {0}, mSr[2] = {0, 0};
};

} }  // namespace dgg::cyfast
