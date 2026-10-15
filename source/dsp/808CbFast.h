#pragma once
// TR-808 / Yocto cowbell, fast model. Derived from the CB sheet of the common NGSpice circuit.
//
// The model computes the whole chain from the accent voltage and the phase of the two free-running oscillators (no tables, no renders):
//   trigger stage  Q5 / R122 / D2 charge the envelope capacitor C9 (2 ms trigger, Newton on the three junction voltages)
//   core           the gates E (TP7) and F (TP6) switch D3/Q15 and D4/Q14: while a gate is high its branch node is clamped (diode law), otherwise it is the divider of the
//                  envelope node; the branch currents drive the U2B T network (ideal op-amp) -> n456
//   level stage    C75 / R116 / VR5 / C76 / R117 / U2A / R163 / output load, linear (5 states), exact for an input that is linear within a sample.
// The reduced linear network (6 states) is discretised exactly: full 1/fs step with four switch modes (matrix exponentials at SetSampleRate), partial steps at the gate
// edges and at the end of the trigger from the eigen-decomposition of the modes (808CbModes.h, generated).
// The nonlinear inputs (two clamp voltages and the injection current) are constant within a step and solved by fixed-point passes.
//
// Parameters that are measured, not derived: V0 / R0 of the conducting branch (regression to the engine) and the switching delays (2 us on, 6 us off after the ideal gate edge).

#include "808CbBranch.h"
#include "808CbModes.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dgg {
namespace cbfast {

namespace detail {
constexpr double kVp = 0.54, kVm = 0.42, kVt = 0.0258649;           // CD4584 Schmitt thresholds (fractions of VDD4), kT/q at 27 C
constexpr double R24 = 2200, R25 = 470e3, R26 = 1e4, R27 = 1e4, R28 = 1e5, R29 = 1e5, R82 = 33e3, R9 = 22e3, R14 = 4.7e3, R122 = 1e3;
constexpr double C9 = 470e-9, C34 = 1e-6, C28 = 1e-8, C29 = 3.3e-9, C30 = 2.2e-9, C31 = 3.3e-9;
constexpr double kDelayOnMax = 9e-6, kDelayOnSpan = 7e-6, kDelayOnVolt = 4.0, kDelayOff = 8e-6;   // the branch transistor conducts kDelayOnMax - kDelayOnSpan * min(1, V444 / kDelayOnVolt) after an ideal rising gate edge and stops kDelayOff after a falling one (fitted to the engine)                    // the branch transistor conducts / stops conducting this long after the ideal gate edge
constexpr double kD2Is = 4.352e-9, kD2Nv = 1.906 * kVt, kV442 = 0.029; // D2; Q1 collector while the trigger is high
constexpr double kQ5Is = 55.9e-15, kQ5Nf = 1.01201, kQ5Nr = 1.012, kQ5Bf = 205.0, kQ5Br = 2.048, kQ5Vaf = 135.0;
constexpr double kTrigSec = 0.002;

struct Osc { double t1, low, per; };
inline Osc MakeOsc(double R, double C)
{
  const double rc = R * C; Osc o;
  o.t1 = -rc * std::log(1 - kVp); o.low = rc * std::log(kVp / kVm); o.per = o.low + rc * std::log((1 - kVm) / (1 - kVp));
  return o;
}
inline double Ex(double v) { return v < 40.0 ? std::exp(v) : 2.3538526683702e17 * (1.0 + (v - 40.0)); }   // linear continuation far from the solution keeps Newton alive

// matrix exponential of the 9x9 augmented matrix Z*h (scaling and squaring, Taylor); setup only
inline void Expm9(const double Z[9][9], double h, double E[9][9])
{
  double A[9][9], nrm = 0;
  for (int i = 0; i < 9; ++i) { double rs = 0; for (int j = 0; j < 9; ++j) { A[i][j] = Z[i][j] * h; rs += std::fabs(A[i][j]); } nrm = std::fmax(nrm, rs); }
  int sq = 0; while (nrm > 0.5) { nrm *= 0.5; ++sq; }
  const double sc = std::ldexp(1.0, -sq);
  for (int i = 0; i < 9; ++i) for (int j = 0; j < 9; ++j) A[i][j] *= sc;
  double T[9][9], R[9][9], N[9][9];
  for (int i = 0; i < 9; ++i) for (int j = 0; j < 9; ++j) { T[i][j] = i == j ? 1.0 : 0.0; R[i][j] = T[i][j]; }
  for (int k = 1; k <= 12; ++k)
  {
    for (int i = 0; i < 9; ++i) for (int j = 0; j < 9; ++j) { double s = 0; for (int m = 0; m < 9; ++m) s += T[i][m] * A[m][j]; N[i][j] = s / k; }
    for (int i = 0; i < 9; ++i) for (int j = 0; j < 9; ++j) { T[i][j] = N[i][j]; R[i][j] += N[i][j]; }
  }
  for (int s = 0; s < sq; ++s)
  {
    for (int i = 0; i < 9; ++i) for (int j = 0; j < 9; ++j) { double q = 0; for (int m = 0; m < 9; ++m) q += R[i][m] * R[m][j]; N[i][j] = q; }
    std::memcpy(R, N, sizeof R);
  }
  std::memcpy(E, R, sizeof R);
}

inline bool Solve3(double a[3][4], double d[3])
{
  for (int c = 0; c < 3; ++c)
  {
    int p = c; for (int r = c + 1; r < 3; ++r) if (std::fabs(a[r][c]) > std::fabs(a[p][c])) p = r;
    if (a[p][c] == 0.0) return false;
    for (int j = 0; j < 4; ++j) std::swap(a[c][j], a[p][j]);
    for (int r = c + 1; r < 3; ++r) { const double m = a[r][c] / a[c][c]; for (int j = c; j < 4; ++j) a[r][j] -= m * a[c][j]; }
  }
  for (int i = 2; i >= 0; --i) { double s = a[i][3]; for (int j = i + 1; j < 3; ++j) s -= a[i][j] * d[j]; d[i] = s / a[i][i]; }
  return true;
}

// Q5 (PNP) + R122 + D2: the current injected into the envelope capacitor C9 for its voltage v; Newton on (vb, vc, vd) with analytic Jacobian, warm started from the previous call
struct Injection
{
  double acc = 4.09, z[3] = {3.47, 0.7, 0.62};
  void Reset(double a) { acc = a; z[0] = a - 0.62; z[1] = 0.7; z[2] = 0.62; }
  void Eval(double v, double& I, double& dI)
  {
    for (int it = 0; it < 50; ++it)
    {
      double J[3][4], f[3], d[3];
      Jac(v, J, f);
      for (int i = 0; i < 3; ++i) J[i][3] = -f[i];
      if (!Solve3(J, d)) break;
      double mx = 0; for (int i = 0; i < 3; ++i) { const double st = std::fmax(-0.15, std::fmin(0.15, d[i])); z[i] += st; mx = std::fmax(mx, std::fabs(st)); }
      if (mx < 1e-11) break;
    }
    // I = D2 current; dI/dv = did (dvd/dv - 1) with J dz/dv = [0 0 did]
    const double eD = Ex((z[2] - v) / kD2Nv), did = kD2Is * eD / kD2Nv; I = kD2Is * (eD - 1);
    double J[3][4], f[3], d[3]; Jac(v, J, f); J[0][3] = 0; J[1][3] = 0; J[2][3] = did;
    if (Solve3(J, d)) dI = did * (d[2] - 1.0); else dI = -did;
  }

private:
  // Jacobian J (3x3 of a[3][4]) and residual f of the junction equations at z for the C9 voltage v
  void Jac(double v, double a[3][4], double f[3]) const
  {
    const double vb = z[0], vc = z[1], vd = z[2], veb = acc - vb, vcb = vc - vb;
    const double eF = Ex(veb / (kQ5Nf * kVt)), eR = Ex(vcb / (kQ5Nr * kVt)), IF = kQ5Is * (eF - 1), IR = kQ5Is * (eR - 1);
    const double dIFb = -kQ5Is * eF / (kQ5Nf * kVt), dIRb = -kQ5Is * eR / (kQ5Nr * kVt), dIRc = -dIRb;
    const bool act = acc > vc; const double E = 1 + (act ? (acc - vc) / kQ5Vaf : 0.0), dE = act ? -1.0 / kQ5Vaf : 0.0;
    const double icc = (IF - IR) * E, ic = icc - IR / kQ5Br, ib = IF / kQ5Bf + IR / kQ5Br;
    const double dic_b = (dIFb - dIRb) * E - dIRb / kQ5Br, dic_c = -dIRc * E + (IF - IR) * dE - dIRc / kQ5Br, dib_b = dIFb / kQ5Bf + dIRb / kQ5Br, dib_c = dIRc / kQ5Br;
    const double eD = Ex((vd - v) / kD2Nv), id = kD2Is * (eD - 1), did = kD2Is * eD / kD2Nv;
    f[0] = (vb - acc) / R14 + (vb - kV442) / R9 - ib; f[1] = (vc - vd) / R122 - ic; f[2] = (vd - vc) / R122 + id;
    a[0][0] = 1 / R14 + 1 / R9 - dib_b; a[0][1] = -dib_c; a[0][2] = 0;
    a[1][0] = -dic_b; a[1][1] = 1 / R122 - dic_c; a[1][2] = -1 / R122;
    a[2][0] = 0; a[2][1] = -1 / R122; a[2][2] = 1 / R122 + did;
  }
};

// Taper A of the level pot (rotation position -> fraction of the resistance)
inline double LevelFraction(double pos)
{
  static const double K[11][2] = {{0, 0}, {.1, .01}, {.2, .025}, {.3, .055}, {.4, .105}, {.5, .16}, {.6, .26}, {.7, .4}, {.8, .65}, {.9, .93}, {1, 1}};
  if (pos <= 0.0) return 0.0;
  for (int i = 1; i < 11; ++i) if (pos <= K[i][0]) return K[i - 1][1] + (pos - K[i - 1][0]) / (K[i][0] - K[i - 1][0]) * (K[i][1] - K[i - 1][1]);
  return 1.0;
}

// Level stage behind the U2B output (R116 + VR5 -> C76 / R117 -> unity buffer U2A -> R163 -> output node with C507 and the two bus loads): s = [C75, C76, C507 (output), bus L, bus R] driven by u = n456
struct LevelStage
{
  double Ad[5][5], B0[5], B1[5], s[5], uPrev = 0;
  void Setup(double level, double dt)
  {
    const double f = LevelFraction(level), Ras = std::fmax(5e3 * f, 1.0), Rs = 8.2e3 + std::fmax(5e3 * (1.0 - f), 1.0), R117 = 22e3, R163 = 1e3, Rb = 12e3, Rpl = 1059.4315245478035, Rpr = 1000.813669650122;
    const double C75 = 1e-6, C76 = 22e-9, C507 = 10e-9, Cb = 10e-6, G = 1 / Rs + 1 / Ras + 1 / R117;
    double VA[6] = {1, 0, 0, 0, 0, 1}, VB[6], VC[6];
    for (int j = 0; j < 6; ++j) { VB[j] = (VA[j] / Rs + (j == 1 ? 1.0 / R117 : 0.0)) / G; VC[j] = VB[j] - (j == 1 ? 1.0 : 0.0); }
    double D[5][6] = {};
    for (int j = 0; j < 6; ++j)
    {
      const double s3 = j == 2 ? 1.0 : 0.0, s4 = j == 3 ? 1.0 : 0.0, s5 = j == 4 ? 1.0 : 0.0;
      D[0][j] = -(VA[j] - VB[j]) / (Rs * C75);
      D[1][j] = VC[j] / (R117 * C76);
      D[2][j] = ((VC[j] - s3) / R163 - (s3 - s4) / Rb - (s3 - s5) / Rb) / C507;
      D[3][j] = (-s4 / Rpl - (s4 - s3) / Rb) / Cb;
      D[4][j] = (-s5 / Rpr - (s5 - s3) / Rb) / Cb;
    }
    double Z[9][9] = {};   // augmented [x, u, w]: x' = F x + g u, u' = w, w' = 0
    for (int i = 0; i < 5; ++i) { for (int j = 0; j < 5; ++j) Z[i][j] = D[i][j]; Z[i][5] = D[i][5]; }
    Z[5][6] = 1.0;
    double E[9][9]; Expm9(Z, dt, E);
    for (int i = 0; i < 5; ++i) { for (int j = 0; j < 5; ++j) Ad[i][j] = E[i][j]; B0[i] = E[i][5]; B1[i] = E[i][6] / dt; }
    Reset();
  }
  void Reset() { for (int i = 0; i < 5; ++i) s[i] = 0.0; uPrev = 0.0; }
  inline double Step(double u)
  {
    double n[5];
    for (int i = 0; i < 5; ++i) { double q = B0[i] * uPrev + B1[i] * (u - uPrev); for (int j = 0; j < 5; ++j) q += Ad[i][j] * s[j]; n[i] = q; }
    for (int i = 0; i < 5; ++i) s[i] = n[i];
    uPrev = u; return s[2];
  }
};
}  // namespace detail

// The cowbell model. One instance = one voice. Not thread safe; no allocation after construction.
class TR808CbFast
{
public:
  static constexpr double kAccentFullScaleVolt = 4.09;   // Dio.ino SetDacA: map(velocity, 0, 127, 0, 4090) mV; the voice always runs with the full accent

  TR808CbFast() { SetSampleRate(48000.0); SetLevel(0.75); }

  // Recomputes the step matrices (four matrix exponentials, ~0.1 ms); not for the audio thread's hot path, but cheap enough for a rate change.
  void SetSampleRate(double fs)
  {
    using namespace detail;
    if (fs == mFs && mInit) return;
    mFs = fs; mDt = 1.0 / fs; mInit = true;
    mOscE = MakeOsc(205798.0, 1.8e-8); mOscF = MakeOsc(240904.0, 1e-8);   // gates E (TP7) and F (TP6): R, C from the deck
    for (int m = 0; m < 4; ++m)
    {
      double F[6][6], Ge[6][3], e[9];
      for (int c = 0; c < 9; ++c)
      {
        for (int i = 0; i < 9; ++i) e[i] = i == c ? 1.0 : 0.0;
        double d[6]; Deriv((m & 1) != 0, (m & 2) != 0, e, e + 6, d);
        for (int i = 0; i < 6; ++i) { if (c < 6) F[i][c] = d[i]; else Ge[i][c - 6] = d[i]; }
      }
      double Z[9][9] = {};
      for (int i = 0; i < 6; ++i) { for (int j = 0; j < 6; ++j) Z[i][j] = F[i][j]; for (int j = 0; j < 3; ++j) Z[i][6 + j] = Ge[i][j]; }
      double E[9][9]; Expm9(Z, mDt, E);
      for (int i = 0; i < 6; ++i) { for (int j = 0; j < 6; ++j) mAd[m][i][j] = E[i][j]; for (int j = 0; j < 3; ++j) mBd[m][i][j] = E[i][6 + j]; }
    }
    mLevel.Setup(mLevelPos, mDt);
  }

  // Level pot VR5 (rotation position 0..1, taper A). Recomputes the level stage (one 9x9 matrix exponential).
  void SetLevel(double level01)
  {
    mLevelPos = std::clamp(level01, 0.0, 1.0);
    if (mInit) mLevel.Setup(mLevelPos, mDt);
  }

  // Starts a hit. oscOffsetSec = engine time of the free-running gates at the trigger (the phase of both oscillators), accentVolt = accent stage voltage.
  void Start(double accentVolt, double oscOffsetSec)
  {
    using namespace detail;
    const double z[6] = {0, 0.021, 0, 0, 0, 0}; std::memcpy(mS, z, sizeof mS);
    mU[0] = mU[1] = 0.45; mU[2] = 0.0; mTime = 0.0; mTrigOn = false;
    // the gates have been switching the network for a long time: run it through the pre-trigger history, so the T network and the coupling capacitors are in their periodic state
    const double pre = prerollSec;
    NextEdge(mOscE, Wrap(mOscE, oscOffsetSec - pre), mHiE, mNextE); NextEdge(mOscF, Wrap(mOscF, oscOffsetSec - pre), mHiF, mNextF);
    if (!mHiF) mU[0] = OffDrop(0.0, 1 / R28 + 1 / R26);   // a gate that is low: its branch carries the off-state current
    if (!mHiE) mU[1] = OffDrop(0.0, 1 / R29 + 1 / R27);
    mLevel.Reset();
    const long n = (long)std::floor(pre * mFs);
    for (long i = 0; i < n; ++i) { StepCore(); mLevel.Step(mS[5]); }
    mTime -= (double)n * mDt; mNextE -= (double)n * mDt; mNextF -= (double)n * mDt;
    mInj.Reset(accentVolt); mTrigOn = true;
  }

  double prerollSec = 0.04;   // length of the pre-trigger history (the slowest time constant of the network is C34 R82 = 33 ms)

  // Next output sample: the voltage at the output node of the sheet (volts, the level stage included).
  inline double Step()
  {
    StepCore();
    return mLevel.Step(mS[5]);
  }

private:
  using Osc = detail::Osc;
  // derivative of the reduced state s = [V444, V466, n453, n454, n455, n456]; linear in (s, u = [clamp A, clamp B, injection])
  static void Deriv(bool onA, bool onB, const double* s, const double* u, double* d)
  {
    using namespace detail;
    const double gA = 1 / R28 + 1 / R26, gB = 1 / R29 + 1 / R27;
    // on: the node is the clamp voltage u; off: the divider voltage minus the drop of the off-state current of the transistor (u = Ioff / g)
    const double v2 = onA ? u[0] : (s[0] / R28 + s[2] / R26) / gA - u[0], v3 = onB ? u[1] : (s[0] / R29 + s[3] / R27) / gB - u[1];
    const double x6d = -s[5] / (R25 * C28);
    d[0] = (u[2] - (s[0] - s[1]) / R82 - (s[0] - v2) / R28 - (s[0] - v3) / R29) / C9;
    d[1] = -(s[1] - s[0]) / (R82 * C34);
    d[2] = x6d - (s[2] - v2) / (R26 * C30);
    d[3] = x6d - (s[3] - v3) / (R27 * C31);
    d[4] = x6d;
    d[5] = (s[4] / R24 + (s[2] - v2) / R26 + (s[3] - v3) / R27 + (C29 + C28) * x6d) / C29;
  }
  static bool High(const Osc& o, double t) { if (t < o.t1) return true; return std::fmod(t - o.t1, o.per) >= o.low; }
  // next level change of a gate, in time from the trigger (engine time off is the gate time at the trigger)
  static void NextEdge(const Osc& o, double off, bool& hi, double& next)
  {
    hi = High(o, off);
    if (off < o.t1) { next = o.t1 - off; return; }
    const double p = std::fmod(off - o.t1, o.per), base = off - p;
    next = (hi ? base + o.per : base + o.low) - off;
  }
  static double Wrap(const Osc& o, double t) { return t >= 0.0 ? t : t + std::ceil(-t / o.per) * o.per; }
  static void Flip(const Osc& o, bool& hi, double& next) { if (hi) { hi = false; next += o.low; } else { hi = true; next += o.per - o.low; } }

  // voltage of the conducting branch (D3/D4 + saturated Q) at the current I, from the NGSpice operating points of the circuit's own devices (808CbBranch.h): Catmull-Rom in ln I.
  // w and dw/d(ln I) are returned
  static inline double BranchW(double x, double& dw)
  {
    const double u = (x / 2.302585092994046 - kBranchP0) / kBranchDP;
    int k = (int)std::floor(u); if (k < 1) k = 1; if (k > kBranchN - 3) k = kBranchN - 3;
    const double t = u - k, y0 = kBranchW[k - 1], y1 = kBranchW[k], y2 = kBranchW[k + 1], y3 = kBranchW[k + 2];
    const double c1 = 0.5 * (y2 - y0), c2 = y0 - 2.5 * y1 + 2.0 * y2 - 0.5 * y3, c3 = 0.5 * (y3 - y0) + 1.5 * (y1 - y2);
    dw = (c1 + t * (2.0 * c2 + 3.0 * c3 * t)) / (kBranchDP * 2.302585092994046);
    return y1 + t * (c1 + t * (c2 + t * c3));
  }
  // collector current of the transistor in the gate-off state at the node voltage w (808CbBranch.h: base at its off level, 0.41 V), linear continuation above the table
  static inline double OffCurrent(double w)
  {
    const double u = std::fmax(w, 0.0) / kOffDW; int k = (int)u; if (k > kOffN - 2) k = kOffN - 2;
    return kOffI[k] + (u - k) * (kOffI[k + 1] - kOffI[k]);
  }
  // off-state input: the voltage drop u = Ioff / g of the divider node (fixed point with w = divider - u)
  static inline double OffDrop(double divider, double g)
  {
    double u = OffCurrent(divider) / g; u = OffCurrent(divider - u) / g; return u;
  }

  // node balance g w(I) + I = J (the node feeds the branch through the two divider resistors: Norton current J, conductance g); Newton in x = ln I (w is concave in x, e^x convex: monotone)
  static inline double Clamp(double J, double g, double w0)
  {
    using namespace detail;
    constexpr double kXmin = kBranchP0 * 2.302585092994046 + 3.0 * kBranchDP * 2.302585092994046, kXmax = (kBranchP0 + (kBranchN - 4) * kBranchDP) * 2.302585092994046;
    double dw, wmin = BranchW(kXmin, dw);
    static const double kExpXmin = std::exp(kXmin);
    if (J <= g * wmin + kExpXmin) return J / g;
    double x = std::log(std::fmax(J - g * w0, 1e-11)); x = std::fmin(std::fmax(x, kXmin), kXmax);
    for (int it = 0; it < 30; ++it)
    {
      const double w = BranchW(x, dw), e = std::exp(x), f = g * w + e - J, df = g * dw + e;
      double xn = x - f / df; xn = std::fmin(std::fmax(xn, x - 3.0), kXmax);
      const bool done = std::fabs(xn - x) < 1e-4; x = xn; if (done) break;   // the update is quadratically small: the error of the accepted x is about 1e-8
    }
    return BranchW(x, dw);
  }

  // one step of constant switch state: p = free response, B = input response; the nonlinear inputs are held constant over the step and updated by fixed-point passes
  inline void Advance(const double p[6], const double B[6][3])
  {
    using namespace detail;
    const bool needA = mHiF, needB = mHiE;
    const double u0[3] = {mU[0], mU[1], mU[2]};        // inputs at the start of the step: the injection is applied as the mean of start and end value (second order)
    double ue[3] = {mU[0], mU[1], mU[2]}, un[3] = {mU[0], mU[1], mU[2]}, sn[6];
    for (int pass = 0; pass < (mTrigOn ? 2 : 1); ++pass)
    {
      for (int i = 0; i < 6; ++i) sn[i] = p[i] + B[i][0] * ue[0] + B[i][1] * ue[1] + B[i][2] * ue[2];
      un[0] = needA ? Clamp(sn[0] / R28 + sn[2] / R26, 1 / R28 + 1 / R26, un[0]) : OffDrop((sn[0] / R28 + sn[2] / R26) / (1 / R28 + 1 / R26), 1 / R28 + 1 / R26);
      un[1] = needB ? Clamp(sn[0] / R29 + sn[3] / R27, 1 / R29 + 1 / R27, un[1]) : OffDrop((sn[0] / R29 + sn[3] / R27) / (1 / R29 + 1 / R27), 1 / R29 + 1 / R27);
      if (mTrigOn) { double di; mInj.Eval(sn[0], un[2], di); }
      ue[0] = un[0]; ue[1] = un[1]; ue[2] = 0.5 * (u0[2] + un[2]);
    }
    for (int k = 0; k < 3; ++k) mU[k] = un[k];
    for (int i = 0; i < 6; ++i) mS[i] = p[i] + B[i][0] * ue[0] + B[i][1] * ue[1] + B[i][2] * ue[2];
  }
  inline void AdvanceFull(int m)
  {
    double p[6];
    for (int i = 0; i < 6; ++i) { double q = 0; for (int j = 0; j < 6; ++j) q += mAd[m][i][j] * mS[j]; p[i] = q; }
    Advance(p, mBd[m]);
  }
  // partial step of length h in mode m from the eigen-decomposition (no matrix exponential)
  void AdvancePartial(int m, double h)
  {
    const ModeEig& E = kModes[m]; double ex[6], ei[6], ph[6], pi_[6], yr[6], yi[6];
    for (int i = 0; i < 6; ++i)
    {
      if (E.li[i] == 0.0) { ex[i] = std::exp(E.lr[i] * h); ei[i] = 0.0; }
      else if (i > 0 && E.li[i] == -E.li[i - 1] && E.lr[i] == E.lr[i - 1]) { ex[i] = ex[i - 1]; ei[i] = -ei[i - 1]; }   // conjugate partner of the previous eigenvalue
      else { const double a = std::exp(E.lr[i] * h); ex[i] = a * std::cos(E.li[i] * h); ei[i] = a * std::sin(E.li[i] * h); }
      const double lr = E.lr[i], li = E.li[i], l2 = lr * lr + li * li;
      if (l2 * h * h < 1e-18) { ph[i] = h; pi_[i] = 0; }
      else { const double nr = ex[i] - 1.0, ni = ei[i]; ph[i] = (nr * lr + ni * li) / l2; pi_[i] = (ni * lr - nr * li) / l2; }
      double sr = 0, si = 0; for (int j = 0; j < 6; ++j) { sr += E.wr[i][j] * mS[j]; si += E.wi[i][j] * mS[j]; }
      yr[i] = sr; yi[i] = si;
    }
    double p[6], B[6][3];
    for (int k = 0; k < 6; ++k)
    {
      double q = 0; for (int i = 0; i < 6; ++i) q += E.vr[k][i] * (ex[i] * yr[i] - ei[i] * yi[i]) - E.vi[k][i] * (ex[i] * yi[i] + ei[i] * yr[i]);
      p[k] = q;
      for (int c = 0; c < 3; ++c)
      {
        double b = 0;
        for (int i = 0; i < 6; ++i) { const double gr = ph[i] * E.gr[i][c] - pi_[i] * E.gi[i][c], gi = ph[i] * E.gi[i][c] + pi_[i] * E.gr[i][c]; b += E.vr[k][i] * gr - E.vi[k][i] * gi; }
        B[k][c] = b;
      }
    }
    Advance(p, B);
  }
  // one sample, split at the gate edges (with the switching delays) and at the end of the trigger
  double DelayOn() const { return detail::kDelayOnMax - detail::kDelayOnSpan * std::fmin(1.0, std::fmax(0.0, mS[0] / detail::kDelayOnVolt)); }
  void StepCore()
  {
    using namespace detail;
    const double tEnd = mTime + mDt;
    for (;;)
    {
      double te = tEnd; int what = 0;
      const double dOn = DelayOn(), evE = mNextE + (mHiE ? kDelayOff : dOn), evF = mNextF + (mHiF ? kDelayOff : dOn);
      if (evE < te) { te = evE; what = 1; }
      if (evF < te) { te = evF; what = 2; }
      if (mTrigOn && kTrigSec < te) { te = kTrigSec; what = 3; }
      const int m = (mHiF ? 1 : 0) | (mHiE ? 2 : 0);
      if (what == 0)
      {
        const double hr = tEnd - mTime;
        if (hr > 1e-15) { if (hr > mDt * (1 - 1e-12)) AdvanceFull(m); else AdvancePartial(m, hr); }
        mTime = tEnd; return;
      }
      const double h = te - mTime;
      if (h > 1e-12) AdvancePartial(m, h);
      mTime = te;
      if (what == 1) { Flip(mOscE, mHiE, mNextE); mU[1] = mHiE ? Clamp(mS[0] / R29 + mS[3] / R27, 1 / R29 + 1 / R27, mU[1]) : OffDrop((mS[0] / R29 + mS[3] / R27) / (1 / R29 + 1 / R27), 1 / R29 + 1 / R27); }
      else if (what == 2) { Flip(mOscF, mHiF, mNextF); mU[0] = mHiF ? Clamp(mS[0] / R28 + mS[2] / R26, 1 / R28 + 1 / R26, mU[0]) : OffDrop((mS[0] / R28 + mS[2] / R26) / (1 / R28 + 1 / R26), 1 / R28 + 1 / R26); }
      else { mTrigOn = false; mU[2] = 0.0; }
    }
  }

  bool mInit = false; double mFs = 48000.0, mDt = 1.0 / 48000.0, mLevelPos = 0.75;
  Osc mOscE{}, mOscF{};
  double mAd[4][6][6] = {}, mBd[4][6][3] = {};        // full-step matrices per switch mode m = (gate F high) | 2 (gate E high)
  detail::Injection mInj; detail::LevelStage mLevel;
  double mS[6] = {}, mU[3] = {}, mTime = 0.0, mNextE = 0.0, mNextF = 0.0; bool mHiE = false, mHiF = false, mTrigOn = false;
};

}  // namespace cbfast
}  // namespace dgg
