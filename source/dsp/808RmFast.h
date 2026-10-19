// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Reiner Prokein
#pragma once
// Fast model of the TR-808 / Yocto rim shot and claves (RM sheet, switch SW11): one hit for any level and accent, synthesised sample by sample.
//
// The model is derived from the RM sheet of the NGSpice circuit:
//  - the circuit upstream of the output stage (trigger network, the four silicon diodes, the ring) does not depend on the level pot: its two signals that drive the output stage, the ring
//    source e (the open-circuit voltage of the ring node n185 behind its 220 kohm source resistance R319 / R324) and the JFET gate (envelope) n175, are functions of the accent alone. They are tables per accent (808RmTables.h, generated:
//    piecewise-linear knots, the first 20.8 us at 3.84 MHz and up to 12 ms at 384 kHz because the edges and the ring burst are under-sampled at 48 kHz, then 48 kHz up to 0.25 s); the accent is blended linearly
//    between the table nodes;
//  - the output stage is solved live: e --220k--> C125 --R326--> summing node of U19A (R330 || C153 feedback; second feedback branch: C126 in series with VR16 (level) in parallel with the JFET
//    Q74 (J2SK30, RD = RS = 30 ohm, gate = n175; Newton per step)) -> n179 -> the linear output network (R295, C8, two 12k loads on the bus capacitors), discretised exactly.
// While the op-amp is slew limited during the gate edge the summing node moves by hundreds of mV (more with a high level: the trigger click is the JFET gate charge through Cgs into the
// feedback branch); that deviation is carried through the ring branch (series 220k + R326 + C125), C153 and C126, which is what makes the click right at every level.
// The first output sample runs at 3.84 MHz with 4 sub-steps per table interval (80 intervals: the gate edge of the trigger lasts 1.3 us), up to 12 ms at 8 steps per output sample, the rest at the output rate; Step() returns the output
// at 48 kHz, rest level removed.

#include "808RmTables.h"
#include "808RmAccentTables.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace dgg {
namespace rmfast {

namespace detail {
constexpr double kCgs = 5.6e-12, kCgd = 8.62e-12, kPb = 1.0, kRout = 196.08;
// uPC4558 (U19A) as the engine models it: transconductance stage gm = 2 pi 3 MHz C1 with the current limit Imax = 1 MHz C1 into C1 = 53 pF || R1 = 1e5 / gm, output resistance kRout
constexpr double kOaC1 = 53e-12, kOaGm = 2.0 * 3.14159265358979 * 3e6 * kOaC1, kOaImax = 1e6 * kOaC1, kOaR1 = 1e5 / kOaGm;   // gate-source junction of the JFET (depletion capacitance, grading 0.5)
constexpr double kGateRest = -0.0167259783;   // n175 at rest (the envelope gate: the JFET conducts); the trigger edge takes it to -15 V within the first 2.6 us
constexpr double kRth = 220e3;   // source resistance of the ring node n185 (R319 / R324 path); the table signal is the open-circuit voltage behind it
constexpr double kR326 = 10e3, kC125 = 10e-9, kR330 = 470e3, kC153 = 22e-12, kC126 = 33e-6, kVto = -1.4;

// charge on the gate plate of the gate-source depletion capacitance at the junction voltage v (reverse biased: v < 0): Q(v) = -2 C0 Pb sqrt(1 - v/Pb), dQ/dv = C0 / sqrt(1 - v/Pb)
inline double GateCharge(double v, double c0 = kCgs) { return -2.0 * c0 * kPb * std::sqrt(1.0 - std::fmin(v, 0.5 * kPb) / kPb); }

// Taper A of the level pot VR16 (rotation position -> fraction of 100k)
inline double Taper(double p)
{
  static const double K[11][2] = {{0, 0}, {.1, .01}, {.2, .025}, {.3, .055}, {.4, .105}, {.5, .16}, {.6, .26}, {.7, .4}, {.8, .65}, {.9, .93}, {1, 1}};
  if (p <= 0.0) return 0.0;
  for (int i = 1; i < 11; ++i) if (p <= K[i][0]) return K[i - 1][1] + (p - K[i - 1][0]) / (K[i][0] - K[i - 1][0]) * (K[i][1] - K[i - 1][1]);
  return 1.0;
}

inline void JfetCore(double vgs, double vds, double& id, double& gm, double& gds)
{
  const double beta = 1.1e-3, lam = 1e-3, ov = vgs - kVto;
  if (ov <= 0) { id = gm = gds = 0; return; }
  if (vds >= ov) { id = beta * ov * ov * (1 + lam * vds); gm = 2 * beta * ov * (1 + lam * vds); gds = beta * ov * ov * lam; }
  else { id = beta * vds * (2 * ov - vds) * (1 + lam * vds); gm = 2 * beta * vds * (1 + lam * vds); gds = beta * ((2 * ov - 2 * vds) * (1 + lam * vds) + vds * (2 * ov - vds) * lam); }
}
// current from the drain terminal to the source terminal (symmetric) with the 30 ohm series resistances, and d id / d vd
inline void Jfet(double vg, double vs, double vd, double& id, double& gd)
{
  double sgn = 1.0; if (vd < vs) { std::swap(vs, vd); sgn = -1.0; }
  const double rs = 30.0, rd = 30.0, vgs = vg - vs, vds = vd - vs; double i, gm, gds, J;
  JfetCore(vgs, vds, J, gm, gds); i = J;
  for (int it = 0; it < 8; ++it) { JfetCore(vgs - i * rs, vds - i * (rs + rd), J, gm, gds); const double f = i - J, df = 1 + gm * rs + gds * (rs + rd), st = f / df; i -= st; if (std::fabs(st) < 1e-10) break; }   // tolerance in amperes: the currents are milliamperes and the correction of the last step is quadratically small
  JfetCore(vgs - i * rs, vds - i * (rs + rd), J, gm, gds);
  id = sgn * i; const double den = 1 + gm * rs + gds * (rs + rd); gd = sgn > 0 ? gds / den : (gm + gds) / den;
}

// matrix exponential of an n x n matrix times h (scaling and squaring, Taylor); setup only
template <int N> void Expm(const double (&Z)[N][N], double h, double (&E)[N][N])
{
  double A[N][N], nrm = 0;
  for (int i = 0; i < N; ++i) { double rs = 0; for (int j = 0; j < N; ++j) { A[i][j] = Z[i][j] * h; rs += std::fabs(A[i][j]); } nrm = std::fmax(nrm, rs); }
  int sq = 0; while (nrm > 0.5) { nrm *= 0.5; ++sq; }
  const double sc = std::ldexp(1.0, -sq);
  for (int i = 0; i < N; ++i) for (int j = 0; j < N; ++j) A[i][j] *= sc;
  double T[N][N], R[N][N], M[N][N];
  for (int i = 0; i < N; ++i) for (int j = 0; j < N; ++j) { T[i][j] = i == j ? 1.0 : 0.0; R[i][j] = T[i][j]; }
  for (int k = 1; k <= 14; ++k)
  {
    for (int i = 0; i < N; ++i) for (int j = 0; j < N; ++j) { double s = 0; for (int m = 0; m < N; ++m) s += T[i][m] * A[m][j]; M[i][j] = s / k; }
    for (int i = 0; i < N; ++i) for (int j = 0; j < N; ++j) { T[i][j] = M[i][j]; R[i][j] += M[i][j]; }
  }
  for (int s = 0; s < sq; ++s)
  {
    for (int i = 0; i < N; ++i) for (int j = 0; j < N; ++j) { double q = 0; for (int m = 0; m < N; ++m) q += R[i][m] * R[m][j]; M[i][j] = q; }
    for (int i = 0; i < N; ++i) for (int j = 0; j < N; ++j) R[i][j] = M[i][j];
  }
  for (int i = 0; i < N; ++i) for (int j = 0; j < N; ++j) E[i][j] = R[i][j];
}

// the linear output network behind U19A: x = [C8 voltage (output node), bus capacitor L, bus capacitor R], input z = n179; x(n+1) = Ad x + G0 z(n) + G1 d1 + G2 d2 for the input z(t) = z(n) + d1 s + d2 s^2 over one step (s = 0..1; linear: d1 = z(n+1) - z(n), d2 = 0; quadratic through z(n-1), z(n), z(n+1): d1 = (z(n+1) - z(n-1)) / 2, d2 = (z(n+1) - 2 z(n) + z(n-1)) / 2)
struct OutNet
{
  double Ad[3][3], G0[3], G1[3], G2[3];
  void Setup(double dt)
  {
    constexpr double C8 = 10e-9, Cb = 10e-6, R1 = 1e3, R12 = 12e3, Rl = 1059.4315245478035, Rr = 1000.813669650122;
    double M[6][6] = {}, E[6][6];
    M[0][0] = (-1 / R1 - 2 / R12) / C8 * dt; M[0][1] = (1 / R12) / C8 * dt; M[0][2] = (1 / R12) / C8 * dt; M[0][3] = 1 / R1 / C8 * dt;
    M[1][0] = (1 / R12) / Cb * dt; M[1][1] = (-1 / R12 - 1 / Rl) / Cb * dt; M[2][0] = (1 / R12) / Cb * dt; M[2][2] = (-1 / R12 - 1 / Rr) / Cb * dt; M[3][4] = 1.0; M[4][5] = 1.0;   // input states: p, p', p'' (per step)
    Expm<6>(M, 1.0, E);
    for (int i = 0; i < 3; ++i) { for (int j = 0; j < 3; ++j) Ad[i][j] = E[i][j]; G0[i] = E[i][3]; G1[i] = E[i][4]; G2[i] = 2.0 * E[i][5]; }
  }
};

// walks the knots of one table signal (index space of the segment) with an index that increases by one
struct Cursor
{
  const uint16_t* row = nullptr; const float* val = nullptr; uint32_t k = 0, end = 0;
  void Init(const uint16_t* r, const float* v, uint32_t k0, uint32_t k1) { row = r; val = v; k = k0; end = k1; }
  inline double At(double i)
  {
    while (k + 2 < end && i > row[k + 1]) ++k;
    const double r0 = row[k], r1 = row[k + 1]; if (i >= r1) return val[k + 1]; if (i <= r0) return val[k];
    return val[k] + (val[k + 1] - val[k]) * (i - r0) / (r1 - r0);
  }
};
}  // namespace detail

class TR808RmFast
{
public:
  static constexpr double kFs = 48000.0;
  static constexpr int kRatio = tab::kHiRatio;
  static constexpr int kFineSub = 4;   // sub-steps per 3.84 MHz table interval in the first output sample (backward Euler on the op-amp pole: 4 keeps the error below 2 mV at every level)

  TR808RmFast() { mOutLo.Setup(1.0 / kFs); mOutHi.Setup(1.0 / (kFs * kRatio)); mOutFine.Setup(1.0 / (kFs * kRatio * tab::kFineRatio)); mOutFineS.Setup(1.0 / (kFs * kRatio * tab::kFineRatio * kFineSub)); SetLevel(0.5); }

  // Level pot VR16 (rotation position 0..1, taper A)
  void SetLevel(double level01) { mR16 = std::fmax(100e3 * detail::Taper(std::clamp(level01, 0.0, 1.0)), 1.0); }

  // Starts a hit from the rest state. claves: SW11 position. False (silent): accent below the first table node.
  bool Start(bool claves, double accentVolt) { return StartModel(tab::kModels[claves ? 1 : 0], accentVolt); }

  // Starts a hit at an accent 0..1 of the 808 trigger range: 0 = the full MIDI-velocity accent (4.09 V, exactly Start(claves, 4.09)), 1 = 14 V (accent voltage 4.09 + accent * 9.91 V). Above 4.09 V the
  // upstream signals come from the nodes of 808RmAccentTables.h (node 0 = the last node of 808RmTables.h), blended linearly in the accent voltage like the velocity nodes.
  bool StartAccent(bool claves, double accent01)
  {
    const tab::ModelData& M0 = tab::kModels[claves ? 1 : 0];
    if (!(accent01 > 0.0)) return StartModel(M0, M0.acc[M0.nAcc - 1]);
    return StartModel(tab::kModelsX[claves ? 1 : 0], 4.09 + std::fmin(accent01, 1.0) * (14.0 - 4.09));
  }

private:
  bool StartModel(const tab::ModelData& M, double accentVolt)
  {
    using namespace detail;
    mActive = false; mM = &M;
    mIBias = (M.z0 - M.n348) / kR330;   // the circuit supplies this current at the summing node at rest (its output sits at z0, not at the virtual ground): the rest state is an equilibrium
    if (!(accentVolt >= M.acc[0])) return false;
    int ia = 0; double wa = 0.0;
    if (accentVolt >= M.acc[M.nAcc - 1]) ia = M.nAcc - 1;
    else { int j = 1; while (accentVolt > M.acc[j]) ++j; ia = j - 1; wa = (accentVolt - M.acc[j - 1]) / (M.acc[j] - M.acc[j - 1]); }
    mWa = wa; mNodes = wa > 0.0 ? 2 : 1;
    for (int n = 0; n < mNodes; ++n)
      for (int s = 0; s < 6; ++s) { const uint32_t o = (uint32_t)((ia + n) * 6 + s); mCur[n][s].Init(M.row, M.val, M.off[o], M.off[o + 1]); }
    mV348 = M.n348; mXc = M.urest - M.n348; mIc = 0.0; mDeltaPrev = 0.0; mI153 = 0.0; mZ = M.z0; mV351 = M.z0;   // at rest no current flows through C126: its node sits at the output of the summing amplifier
    // the output network starts at its DC solution for the rest value of n179: x = -A^-1 B z0 (the network is a divider chain; solved here from the exact step map: x = (I - Ad)^-1 (G0 z0) with the unit step)
    mX0[0] = mX0[1] = mX0[2] = 0.0;
    { double I3[3][3], b[3]; for (int i = 0; i < 3; ++i) { for (int j = 0; j < 3; ++j) I3[i][j] = (i == j ? 1.0 : 0.0) - mOutLo.Ad[i][j]; b[i] = mOutLo.G0[i] * M.z0; } Solve3(I3, b, mX0); }
    for (int i = 0; i < 3; ++i) mX[i] = mX0[i];
    mGPrev = kGateRest; mGPrevD = kGateRest; mQs = GateCharge(kGateRest - M.z0); mZeffPrev = M.z0; mN1 = M.z0 + kRout * (M.z0 - M.n348) / kR330; mSeg = -1; mIdx = 0; mHist = 0; mActive = true; return true;
  }

public:
  // next output sample (volts, rest level removed), 48 kHz
  double Step()
  {
    if (!mActive) return 0.0;
    if (mSeg < 0)
    {
      // the first output sample: 80 steps at 3.84 MHz (the 1.3 us gate edge of the trigger is resolved), the hit continues at 384 kHz from step 8
      for (int r = 0; r < tab::kFineSamples; ++r) { for (int q = 1; q <= kFineSub; ++q) AdvanceDyn((double)r - 1.0 + (double)q / kFineSub, 4, 1.0 / (kFs * kRatio * tab::kFineRatio * kFineSub), mOutFineS); }
      mSeg = 0; mHist = 0; mIdx = tab::kFineSamples / tab::kFineRatio;
    }
    else if (mSeg == 0)
    {
      for (int r = 0; r < kRatio; ++r) { Advance(mIdx++, 0, 1.0 / (kFs * kRatio), mOutHi); }
      if (mIdx >= tab::kHiSamples) { mHist = 0; mSeg = 1; mIdx = tab::kHiSamples / kRatio; }
    }
    else { Advance(mIdx < tab::kLoSamples ? mIdx : tab::kLoSamples - 1, 2, 1.0 / kFs, mOutLo); ++mIdx; }
    return mX[0] - mX0[0];
  }

private:
  static void Solve3(double A[3][3], double b[3], double x[3])
  {
    double M[3][4]; for (int i = 0; i < 3; ++i) { for (int j = 0; j < 3; ++j) M[i][j] = A[i][j]; M[i][3] = b[i]; }
    for (int c = 0; c < 3; ++c)
    {
      int p = c; for (int r = c + 1; r < 3; ++r) if (std::fabs(M[r][c]) > std::fabs(M[p][c])) p = r;
      for (int j = 0; j < 4; ++j) std::swap(M[c][j], M[p][j]);
      for (int r = 0; r < 3; ++r) if (r != c) { const double f = M[r][c] / M[c][c]; for (int j = c; j < 4; ++j) M[r][j] -= f * M[c][j]; }
    }
    for (int i = 0; i < 3; ++i) x[i] = M[i][3] / M[i][i];
  }

  // one step: ring signal u and gate g from the tables (signal indices s0 / s0 + 1 of the accent nodes), input stage, JFET branch (Newton), output network
  inline void Advance(int i, int s0, double dt, const detail::OutNet& net)
  {
    using namespace detail;
    double u = mCur[0][s0].At(i), gate = mCur[0][s0 + 1].At(i);
    if (mNodes == 2) { u = (1.0 - mWa) * u + mWa * mCur[1][s0].At(i); gate = (1.0 - mWa) * gate + mWa * mCur[1][s0 + 1].At(i); }
    const double gC = 2.0 * kC153 / dt;   // C153 (feedback capacitor, pole 15 kHz at R330): trapezoid; backward Euler took 1.5 % of the tone amplitude at 48 kHz
    // the gate edge pushes charge through the gate-source capacitance into the source node (C126, the summing point): the click of the trigger edge
    const double iCs = (GateCharge(gate - mV351) - mQs) / dt; mGPrev = gate;
    // ring branch: source e (u) behind R319 + R326 charging C125 (voltage mXc), trapezoid: i = (e - mXc - dt iOld / 2C - n348) / (Rt + dt / 2C), n348 = virtual ground here
    const double Reff = kRth + kR326 + dt / (2.0 * kC125), iin = (u - mXc - dt * mIc / (2.0 * kC125) - mV348) / Reff; double zz = mZ;
    mXc += dt / (2.0 * kC125) * (mIc + iin); mIc = iin;
    double idL = 0.0, gdL = 0.0, zL = zz;   // the last evaluation of the JFET branch: its current at the converged node follows from the linearisation
    for (int it = 0; it < 30; ++it)
    {
      double id, gd; Jfet(gate, mV351, zz, id, gd); idL = id; gdL = gd; zL = zz;
      const double f = iin + (zz - mV348) / kR330 - mIBias + gC * (zz - mZ) - mI153 + (zz - mV351) / mR16 + id + iCs, d = 1.0 / kR330 + gC + 1.0 / mR16 + gd, st = f / d; zz -= st; if (std::fabs(st) < 1e-7) break;
    }
    const double zp = mZ; mZ = zz; mI153 = gC * (zz - zp) - mI153;
    // the gate-drain capacitance pushes its charge into the output node of the op-amp, which answers through its output resistance
    const double iCd = (GateCharge(gate - mZ, kCgd) - GateCharge(mGPrevD - zp, kCgd)) / dt; mGPrevD = gate;
    const double zeff = mZ + kRout * iCd, zeffp = mZeffPrev; mZeffPrev = zeff;
    { const double id = idL + gdL * (mZ - zL); const double vOld = mV351; mV351 += dt / kC126 * ((mZ - mV351) / mR16 + id + iCs); mV351 += (GateCharge(gate - mV351) - GateCharge(gate - vOld)) / kC126; mQs = GateCharge(gate - mV351); }
    // the 48 kHz and 384 kHz segments hold the input to the output network quadratically (linear holding costs (w h)^2 / 12 of a 2.5 kHz tone, 0.7 % at 48 kHz); after a change of the step the first step is linear
    const double d1 = mHist > 0 ? 0.5 * (zeff - mZeffPrev2) : zeff - zeffp, d2 = mHist > 0 ? 0.5 * (zeff - 2.0 * zeffp + mZeffPrev2) : 0.0;
    double xn[3]; for (int a = 0; a < 3; ++a) { xn[a] = net.G0[a] * zeffp + net.G1[a] * d1 + net.G2[a] * d2; for (int b = 0; b < 3; ++b) xn[a] += net.Ad[a][b] * mX[b]; } for (int a = 0; a < 3; ++a) mX[a] = xn[a];
    mZeffPrev2 = zeffp; if (mHist < 1) ++mHist;
  }

  // one step of the first output sample (3.84 MHz) with the op-amp dynamics: the closed loop (the summing node n348 moves by delta = net current / total conductance, the transconductance
  // stage charges C1 with Imax tanh(gm (-delta) / Imax), the output node is n1 - Rout i_out) is solved for the output node z by the secant method
  inline void AdvanceDyn(double i, int s0, double dt, const detail::OutNet& net)
  {
    using namespace detail;
    double u = mCur[0][s0].At(i), gate = mCur[0][s0 + 1].At(i);
    if (mNodes == 2) { u = (1.0 - mWa) * u + mWa * mCur[1][s0].At(i); gate = (1.0 - mWa) * gate + mWa * mCur[1][s0 + 1].At(i); }
    const double gC = kC153 / dt;
    const double Reff = kRth + kR326 + dt / (2.0 * kC125), iin = (u - mXc - dt * mIc / (2.0 * kC125) - mV348) / Reff;   // Norton current of the ring branch at the nominal summing node
    const double iCs = (GateCharge(gate - mV351) - mQs) / dt; mGPrev = gate;
    const double zp = mZ, qdPrev = GateCharge(mGPrevD - zp, kCgd), ax = net.Ad[0][0] * mX[0] + net.Ad[0][1] * mX[1] + net.Ad[0][2] * mX[2] + net.G0[0] * zp - net.G1[0] * zp;
    double deltaLast = 0.0;
    auto resid = [&](double z) {
      double id, gd; Jfet(gate, mV351, z, id, gd);
      const double ib = (z - mV351) / mR16 + id;
      const double r = iin + (z - mV348) / kR330 - mIBias + gC * (z - zp) + gC * mDeltaPrev + ib + iCs;   // C153 carries the summing node's own previous deviation
      deltaLast = r / (1.0 / Reff + 1.0 / kR330 + gC + 1.0 / mR16 + gd); const double delta = deltaLast;
      const double iCd = (GateCharge(gate - z, kCgd) - qdPrev) / dt;
      const double x0n = ax + net.G1[0] * z;
      const double iout = (z - mV348 - delta) / kR330 + gC * ((z - zp) - (delta - mDeltaPrev)) + ib - delta / mR16 + (z - x0n) / 1e3 - iCd;
      const double n1 = z + kRout * iout;
      return kOaC1 * (n1 - mN1) / dt + n1 / kOaR1 - kOaImax * std::tanh(kOaGm * (-delta) / kOaImax);
    };
    double za = zp, fa = resid(za), zb = zp + 1e-4, fb = resid(zb);
    for (int it = 0; it < 40 && std::fabs(zb - za) > 1e-10; ++it)
    {
      const double den = fb - fa; if (den == 0.0) break;
      const double zn = zb - fb * (zb - za) / den; za = zb; fa = fb; zb = zn; fb = resid(zb);
      if (std::fabs(fb) < 1e-15) break;
    }
    const double z = zb; mZ = z;
    { const double iN = iin - deltaLast / Reff; mXc += dt / (2.0 * kC125) * (mIc + iN); mIc = iN; }   // the summing node moved by delta: the ring branch current follows
    { double id, gd; Jfet(gate, mV351, z, id, gd); const double vOld = mV351; mV351 += dt / kC126 * ((z - mV351) / mR16 + id + iCs); mV351 += (GateCharge(gate - mV351) - GateCharge(gate - vOld)) / kC126; mQs = GateCharge(gate - mV351);
      const double ib = (z - mV351) / mR16 + id; const double x0n = ax + net.G1[0] * z; const double iCd = (GateCharge(gate - z, kCgd) - qdPrev) / dt; mGPrevD = gate;
      mN1 = z + kRout * ((z - mV348 - deltaLast) / kR330 + gC * ((z - zp) - (deltaLast - mDeltaPrev)) + ib - deltaLast / mR16 + (z - x0n) / 1e3 - iCd); mI153 = gC * ((z - zp) - (deltaLast - mDeltaPrev)); mDeltaPrev = deltaLast; }
    double xn[3]; for (int a = 0; a < 3; ++a) { xn[a] = net.G0[a] * zp + net.G1[a] * (z - zp); for (int b = 0; b < 3; ++b) xn[a] += net.Ad[a][b] * mX[b]; } for (int a = 0; a < 3; ++a) mX[a] = xn[a];
    mZeffPrev = z;
  }

  const tab::ModelData* mM = nullptr;
  detail::OutNet mOutLo, mOutHi, mOutFine, mOutFineS; detail::Cursor mCur[2][6];
  double mR16 = 16e3, mIBias = 0.0, mN1 = 0.0, mQs = 0.0, mGPrev = 0.0, mGPrevD = 0.0, mZeffPrev = 0.0, mWa = 0.0, mV348 = 0, mXc = 0, mIc = 0, mZ = 0, mV351 = 0, mX[3] = {}, mX0[3] = {};
  double mDeltaPrev = 0, mI153 = 0, mZeffPrev2 = 0; int mHist = 0; int mNodes = 1, mSeg = 0, mIdx = 0; bool mActive = false;
};

}  // namespace rmfast
}  // namespace dgg
