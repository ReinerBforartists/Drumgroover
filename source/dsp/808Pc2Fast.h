#pragma once
// Fast model of the TR-808 / Yocto PC-2 (Boss PC-2 equivalent, sheet PC-2): one hit for any knob state, synthesised sample by sample at any sample rate.
//
// A functional-block model derived from the PC-2 sheet of the common NGSpice circuit. Every knob acts on exactly one block:
//   envelope E        (trigger charges C7 in 2 ms to a target that depends on the accent (table), then exponential decay with the VR19 time constant)
//   control network   (U25B, linear: V394 = c0 + cE E + cL Vlfo, solved once per knob state from VR18 pitch, VR3 LFO depth, VR20 sweep and the trimmer TM5)
//   LFO (VR2 rate, SW2 wave), VCO (integrator with the comparator thresholds, SW3 wave), OTA (BA6110: tanh stage, current from Q23/R212), the output stage with the JFET Q6 (gate = -E, level VR1)
//   and the Q22 transient block (C23 from E injects a decaying pulse into the summing node at every trigger).
// The VCO overshoot, the LFO amplitude and dwell time and the accent -> envelope target are tabulated from the engine; everything else follows from the circuit values.
// The oscillators run freely in the circuit, so a hit starts at a random phase (Start() draws it); the slow states (coupling capacitors, output-stage cap) start at their steady state.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace dgg {
namespace pc2fast {

namespace detail {
constexpr double kRailPc2 = 14.32;   // VCO integrator rail
constexpr double kIdleE = 0.15, kIdleAfter = 0.3;   // tail skip: E below 0.15 V (output < -80 dB) and 0.3 s after the last trigger
constexpr double kQ22Fine = 2e-6, kQ22SlowAfter = 0.03; constexpr long kQ22Decim = 8, kQ22Decim2 = 64;   // Q22 transient block: decimated updates 30 ms after a trigger
struct Pt { double x, y; };
inline double interpLog(const Pt* t, int n, double s)
{
  const double ls = std::log(std::max(s, 1.0));
  if (ls <= std::log(t[0].x)) return t[0].y;
  for (int i = 1; i < n; ++i)
    if (ls <= std::log(t[i].x))
    {
      const double a = std::log(t[i - 1].x), b = std::log(t[i].x);
      return t[i - 1].y + (ls - a) / (b - a) * (t[i].y - t[i - 1].y);
    }
  return t[n - 1].y;
}
inline const Pt kVcoUp[] = {{349, 7.117}, {2248, 7.155}, {2694, 7.163}, {4428, 7.186}, {7453, 7.233}, {10928, 7.285}, {16166, 7.369}, {22631, 7.458}, {30839, 7.572}, {47687, 7.831}, {76550, 8.268}, {120000, 9.2}};
inline const Pt kVcoDn[] = {{679, 7.166}, {1055, 7.173}, {2346, 7.189}, {2734, 7.197}, {4485, 7.221}, {7058, 7.267}, {10286, 7.320}, {15353, 7.409}, {21571, 7.496}, {29714, 7.634}, {42818, 7.853}, {68478, 8.266}, {120000, 9.2}};
inline const Pt kLfoAmp[] = {{42, 6.678}, {109, 6.726}, {175, 6.763}, {271, 6.811}, {364, 6.853}, {714, 6.985}, {1735, 7.270}, {2557, 7.448}, {3658, 7.649}, {6035, 7.999}, {11222, 8.557}, {13720, 8.771}};
inline const Pt kLfoDwell[] = {{175, 206e-6}, {271, 204e-6}, {364, 168e-6}, {714, 194e-6}, {1735, 180e-6}, {2557, 172e-6}, {3658, 165e-6}, {6035, 153e-6}, {11222, 138e-6}, {13720, 133e-6}};
// uniform-in-slope lookup (no log() per sample): value(s) for s in [0, kTabMax], step kTabStep, built once from the log-interpolated tables
struct SlopeTab
{
  static constexpr int N = 1201; static constexpr double step = 100.0; double v[N + 1];
  void build(const Pt* t, int n) { for (int i = 0; i <= N; ++i) v[i] = interpLog(t, n, std::max(i * step, 1.0)); }
  double operator()(double s) const { const double u = std::min(std::max(s, 0.0) / step, (double)N - 1e-9); const int k = (int)u; const double f = u - k; return v[k] + f * (v[k + 1] - v[k]); }
};
inline const Pt kAccent[] = {{1.00, 0.0047}, {1.30, 0.0065}, {1.40, 0.0400}, {1.45, 0.0798}, {1.50, 0.1527}, {1.55, 0.2852}, {1.60, 0.5245}, {1.65, 0.7559}, {1.70, 1.1466}, {1.75, 1.8389}, {1.80, 3.0339}, {1.85, 3.6972}, {1.90, 3.8362}, {1.95, 3.9540}, {2.00, 4.0645}, {2.10, 4.2749}, {2.20, 4.4802}, {2.30, 4.6817}, {2.40, 4.8824}, {2.50, 5.0812}, {3.00, 6.0710}, {4.00, 8.0393}, {4.50, 9.0235}, {5.00, 10.0079}, {6.00, 11.9774}};
constexpr int kAccentN = 25;

inline double taperA(double p)
{
  static const double k[11][2] = {{0, 0}, {.1, .01}, {.2, .025}, {.3, .055}, {.4, .105}, {.5, .16}, {.6, .26}, {.7, .40}, {.8, .65}, {.9, .93}, {1, 1}};
  for (int i = 0; i < 10; ++i) if (p <= k[i + 1][0]) return k[i][1] + (p - k[i][0]) / (k[i + 1][0] - k[i][0]) * (k[i + 1][1] - k[i][1]);
  return 1.0;
}

// ---- control network U25B: V394 = c0 + cE*E + cL*Vlfo (ideal op-amp, 11 nodes + op-amp current) ----
inline void solveControl(double vr18, double vr3, double vr20, double tm5, double& c0, double& cE, double& cL)
{
  const double f18 = taperA(vr18), f3 = taperA(vr3);
  enum { n396, n395, n394, n407, n406, n405, n400, n403, n383, n382, n387, N0 };
  const int N = N0 + 1;
  auto build = [&](double E, double Vl) {
    std::vector<double> G(N * N, 0.0), b(N, 0.0);
    auto node = [&](int a, int c, double r, int fixedKind, double fixedV) {   // resistor between node a and (node c | fixed voltage)
      const double g = 1.0 / std::max(r, 1e-3);
      G[a * N + a] += g;
      if (c >= 0) { G[a * N + c] -= g; G[c * N + c] += g; G[c * N + a] -= g; } else b[a] += g * fixedV;
      (void)fixedKind;
    };
    const int GND = -1;
    node(n395, n383, 47e3, 0, 0); node(n394, n395, 100e3, 0, 0); node(n395, GND, 100e3, 0, 0);
    node(n396, n387, 47e3, 0, 0); node(n383, GND, 5.6e3, 0, 0); node(n382, GND, 4.7e3, 0, E);
    node(n383, n382, std::max(10e3 * vr20, 1e-3), 0, 0); node(n382, n387, std::max(10e3 * (1 - vr20), 1e-3), 0, 0); node(n387, GND, std::max(10e3 * tm5, 1e-3), 0, 0);
    node(n406, n396, std::max(100e3 * f18, 1.0), 0, 0); node(n396, n407, std::max(100e3 * (1 - f18), 1.0), 0, 0); node(n407, GND, 56e3, 0, 15.0);
    node(n406, n405, 10e3, 0, 0); node(n405, GND, 150e3, 0, -15.0);
    node(n400, GND, std::max(100e3 * f3, 1.0), 0, 0); node(n400, n396, std::max(100e3 * (1 - f3), 1.0), 0, 0); node(n400, n403, 47e3, 0, 0); node(n403, GND, 47e3, 0, Vl);
    G[n394 * N + (N - 1)] += -1.0; G[(N - 1) * N + n395] = 1.0; G[(N - 1) * N + n396] = -1.0;
    // op-amp ideal: V395 = V396
    return std::make_pair(G, b);
  };
  auto solve = [&](double E, double Vl) {
    auto gb = build(E, Vl); auto& G = gb.first; auto& b = gb.second;
    for (int i = 0; i < N; ++i)
    {
      int p = i; for (int r = i + 1; r < N; ++r) if (std::fabs(G[r * N + i]) > std::fabs(G[p * N + i])) p = r;
      if (p != i) { for (int c = 0; c < N; ++c) std::swap(G[i * N + c], G[p * N + c]); std::swap(b[i], b[p]); }
      for (int r = i + 1; r < N; ++r) { const double f = G[r * N + i] / G[i * N + i]; for (int c = i; c < N; ++c) G[r * N + c] -= f * G[i * N + c]; b[r] -= f * b[i]; }
    }
    std::vector<double> x(N);
    for (int i = N - 1; i >= 0; --i) { double s = b[i]; for (int c = i + 1; c < N; ++c) s -= G[i * N + c] * x[c]; x[i] = s / G[i * N + i]; }
    return x[n394];
  };
  const double a = solve(0, 0), bE = solve(1, 0), cc = solve(0, 1);
  c0 = a; cE = bE - a; cL = cc - a;
}

// ---- Q23 + R212 -> Ic(E) table ----
struct IcTable
{
  static constexpr int N = 4096; double emax = 12.0, v[N + 1];
  IcTable()
  {
    const double Vt = 0.025852, IS = 55.9e-15, NF = 1.01201, BF = 205.0;
    for (int k = 0; k <= N; ++k)
    {
      const double e = emax * k / N; double ie = std::max((e - 0.55) / 33e3, 1e-9);
      if (e <= 0) { v[k] = 0; continue; }
      for (int it = 0; it < 60; ++it)
      {
        const double veb = NF * Vt * std::log(ie * BF / (BF + 1) / IS + 1), f = ie * 33e3 + veb - e, df = 33e3 + NF * Vt / ie;
        ie = std::max(ie - f / df, 1e-12); if (std::fabs(f) < 1e-9) break;
      }
      v[k] = ie * BF / (BF + 1);
    }
  }
  double operator()(double e) const { if (e <= 0) return 0; const double u = std::min(e, emax - 1e-9) / emax * N; const int k = (int)u; const double f = u - k; return v[k] + f * (v[k + 1] - v[k]); }
};

// JFET Q6 (J2SK30, RD = RS = 30 Ohm): current from drain terminal to source terminal, symmetric; id and d id / d Vds
constexpr double gVto = -0.54;   // Q6 (J2SK30PC2 of the deck): Vto calibrated to the Yocto2 recordings, 2.2 uF -> 3.32 uF effective decay capacitor (CPC27 of the deck)
inline void jfetCore(double vgs, double vds, double& id, double& gm, double& gds)
{
  const double vto = gVto, beta = 1.1e-3, lam = 1e-3, ov = vgs - vto;
  if (ov <= 0) { id = gm = gds = 0; return; }
  if (vds >= ov) { id = beta * ov * ov * (1 + lam * vds); gm = 2 * beta * ov * (1 + lam * vds); gds = beta * ov * ov * lam; }
  else { id = beta * vds * (2 * ov - vds) * (1 + lam * vds); gm = 2 * beta * vds * (1 + lam * vds); gds = beta * ((2 * ov - 2 * vds) * (1 + lam * vds) + vds * (2 * ov - vds) * lam); }
}
inline void jfet(double vg, double vs, double vd, double& id, double& gd)
{
  double sgn = 1.0; if (vd < vs) { std::swap(vs, vd); sgn = -1.0; }
  const double rs = 30.0, rd = 30.0, vgs = vg - vs, vds = vd - vs;
  double i = 0, gm, gds, J;
  jfetCore(vgs, vds, J, gm, gds);
  i = J;
  for (int it = 0; it < 8; ++it)
  {
    jfetCore(vgs - i * rs, vds - i * (rs + rd), J, gm, gds);
    const double f = i - J, df = 1 + gm * rs + gds * (rs + rd);
    const double step = f / df; i -= step; if (std::fabs(step) < 1e-12) break;
  }
  jfetCore(vgs - i * rs, vds - i * (rs + rd), J, gm, gds);
  id = sgn * i;
  // d id / d vd: forward (drain above source) only the channel conductance; reversed (the drain terminal is the lower one) the gate-to-lower-terminal voltage moves with vd as well
  const double den = 1 + gm * rs + gds * (rs + rd);
  gd = sgn > 0 ? gds / den : (gm + gds) / den;
}
// ---- Q22 transient block: E step -> C23 -> Q22 -> CR ladder -> R25 220k into the U11A summing node (backward Euler + damped Newton, 5 unknowns) ----
struct Q22Transient
{
  double u[5] = {0.0, 15.0, 0.0008, 0.0008, 15.0}, eOld = 0.034, n368 = 0.00244;
  static double sexp(double x, double& d) { const double lim = 34.0; if (x < lim) { const double e = std::exp(x); d = e; return e; } const double e = std::exp(lim); d = e; return e * (1 + x - lim); }
  void step(double E, double dt)
  {
    const double VT = 0.025852, IS = 35.1e-15, BF = 222.31, BR = 1.0, ISD = 4.352e-9, ND = 1.906, C = 47e-9, R21 = 1e6, R22 = 100e3, R23 = 100e3, R24 = 1e3, R25 = 220e3;
    const double o[5] = {u[0], u[1], u[2], u[3], u[4]}; double x[5] = {u[0], u[1], u[2], u[3], u[4]};
    for (int it = 0; it < 200; ++it)
    {
      double def, der = 0.0, dd; const double vbc = x[0] - x[1], ef = sexp(x[0] / VT, def), ed = sexp((x[4] - x[1]) / (ND * VT), dd);
      const double er = vbc > -0.5 ? sexp(vbc / VT, der) : 0.0;   // collector junction reverse biased by more than 0.5 V: its current is < 1e-17 A
      const double ic = IS * (ef - er) - IS / BR * (er - 1), ib = IS / BF * (ef - 1) + IS / BR * (er - 1), idd = ISD * (ed - 1);
      const double gbe = def / VT, gbc = der / VT, gd = ISD * dd / (ND * VT);
      const double dicbe = IS * gbe, dicbc = -IS * gbc - IS / BR * gbc, dibbe = IS / BF * gbe, dibbc = IS / BR * gbc;
      const double a = C / dt;
      double f[5], J[5][6] = {};
      f[0] = a * ((x[0] - E) - (o[0] - eOld)) + x[0] / R22 + ib;
      f[1] = a * (x[1] - o[1]) + a * ((x[1] - x[2]) - (o[1] - o[2])) + ic - idd;
      f[2] = a * ((x[2] - x[1]) - (o[2] - o[1])) + x[2] / R23 + (x[2] - x[3]) / R24;
      f[3] = a * (x[3] - o[3]) + (x[3] - x[2]) / R24 + (x[3] - n368) / R25;
      f[4] = (15.0 - x[4]) / R21 - idd;
      J[0][0] = a + 1 / R22 + dibbe + dibbc; J[0][1] = -dibbc;
      J[1][0] = dicbe + dicbc; J[1][1] = 2 * a - dicbc + gd; J[1][2] = -a; J[1][4] = -gd;
      J[2][1] = -a; J[2][2] = a + 1 / R23 + 1 / R24; J[2][3] = -1 / R24;
      J[3][2] = -1 / R24; J[3][3] = a + 1 / R24 + 1 / R25;
      J[4][1] = gd; J[4][4] = -1 / R21 - gd;
      for (int r = 0; r < 5; ++r) J[r][5] = -f[r];
      for (int i = 0; i < 5; ++i)
      {
        int p = i; for (int r = i + 1; r < 5; ++r) if (std::fabs(J[r][i]) > std::fabs(J[p][i])) p = r;
        if (p != i) for (int c = 0; c < 6; ++c) std::swap(J[i][c], J[p][c]);
        for (int r = i + 1; r < 5; ++r) { const double m = J[r][i] / J[i][i]; for (int c = i; c < 6; ++c) J[r][c] -= m * J[i][c]; }
      }
      double dx[5];
      for (int i = 4; i >= 0; --i) { double sum = J[i][5]; for (int c = i + 1; c < 5; ++c) sum -= J[i][c] * dx[c]; dx[i] = sum / J[i][i]; }
      static const double lim[5] = {0.08, 1.0, 1.0, 1.0, 0.15}; double mx = 0;
      for (int i = 0; i < 5; ++i) { dx[i] = std::max(-lim[i], std::min(lim[i], dx[i])); x[i] += dx[i]; mx = std::max(mx, std::fabs(dx[i])); }
      if (mx < 1e-7) break;
    }
    for (int i = 0; i < 5; ++i) u[i] = x[i];
    eOld = E;
  }
};
}  // namespace detail

class TR808Pc2Fast
{
public:
  struct Knobs { double vr1 = 0.5, vr2 = 0.5, vr3 = 0.5, vr18 = 0.5, vr19 = 0.5, vr20 = 0.5, tm5 = 0.5; int sw2 = 1, sw3 = 1; };
  // optional start state (engine comparisons): E, VCO x / direction, LFO l / direction, coupling capacitor
  struct StartState { double E, x, l, vcap; int vdir, ldir; };

  TR808Pc2Fast() { Tables(); SetSampleRate(48000.0); }

  void SetSampleRate(double sr) { if (sr == mSr) return; mSr = sr; mKnobsValid = false; }
  // Knob state (computes the blocks that depend on it when it changed)
  void SetKnobs(const Knobs& k)
  {
    if (mKnobsValid && std::memcmp(&k, &mK, sizeof(Knobs)) == 0) return;
    using namespace detail;
    mK = k; mKnobsValid = true; mSnapValid = false; mDt = 1.0 / mSr; const double dt = mDt;
    solveControl(k.vr18, k.vr3, k.vr20, k.tm5, mC0, mCE, mCL); mC0 -= 0.0025;
    const double rpDecay = 1.0 / (1.0 / 94e3 + 1.0 / (1e3 + 1e6 * taperA(k.vr19)));
    mTau = 3.32e-6 * rpDecay * 1.0013; mDecay = std::exp(-dt / mTau);
    mERest = 8.6886e-8 * rpDecay;   // the envelope rests where the offset current of U8B (86.9 nA, the same for every knob state in the engine) drops over R10 + R9 || R11 + VR19
    const double fL = taperA(k.vr2), rse = 100e3 * (1 - fL), rp = 1.0 / (1.0 / (100e3 * fL + 330) + 1.0 / 47e3);
    mSLfo = 14.276 * rp / (rse + rp) / 47e3 / 22e-9; mLfoA = interpLog(kLfoAmp, 12, mSLfo); mLfoDwell = interpLog(kLfoDwell, 10, mSLfo);
    mR1 = std::max(100e3 * taperA(k.vr1), 1.0);
    mAOta = 1 - std::exp(-dt / (33e3 * 1e-9)); mAOut = 1 - std::exp(-dt / (857.0 * 10e-9));
    mACap3 = 1 - std::exp(-dt / (471e3 * 1e-6)); mACap0 = 1 - std::exp(-dt / (1001e3 * 1e-6));
    mAlphaHp = kCouplingRC / (kCouplingRC + dt);
  }

  // Starts a hit from the rest state with a random oscillator phase (seed) or the given state. False: not ready.
  bool Start(double accentVolt, uint64_t seed, const StartState* st = nullptr)
  {
    using namespace detail;
    if (!mKnobsValid) return false;
    if (!mSnapValid) BuildSnapshot();
    mRng = seed * 0x9E3779B97F4A7C15ull + 0x1234567ull;
    mE = mERest; mHold = 0.0; mWasIdle = false; mIdleRunning = true; mAttacking = true; mI = 0;
    mX = (Rnd() * 2 - 1) * 7.1; mVdir = Rnd() < 0.5 ? 1 : -1; mL = (Rnd() * 2 - 1) * 6.6; mLdir = Rnd() < 0.5 ? 1 : -1;
    mVcap = mSnapVcap; mVc = mSnapVc; mY = mYPrev = mSnapY; mYo = mSnapY * 0.857; mN385 = 0.0; mQ22 = Q22Transient(); mQ22.eOld = mERest;
    if (st) { mE = st->E; mX = st->x; mVdir = st->vdir; mL = st->l; mLdir = st->ldir; if (st->vcap == st->vcap) mVcap = st->vcap; }   // vcap NaN: the steady state
    mEt = AccentTarget(accentVolt);
    mTTrig = 0.0; mHpFirst = true; mHpX1 = mHpY1 = 0.0; mActive = true; return true;
  }

  // next output sample (volts at the jack after the 10.6 Hz coupling of Block 11)
  double Step();

  bool Active() const { return mActive; }
  static constexpr double kCouplingRC = 15e-6 * 1e3;   // Block 11: 15 uF, 1 kOhm

private:
  using IcTable = detail::IcTable; using SlopeTab = detail::SlopeTab; using Q22Transient = detail::Q22Transient;
  struct Tabs { IcTable ic; SlopeTab up, dn; Tabs() { up.build(detail::kVcoUp, 12); dn.build(detail::kVcoDn, 13); } };
  static const Tabs& Tables() { static const Tabs t; return t; }

  double Rnd() { mRng += 0x9E3779B97F4A7C15ull; unsigned long long r = mRng; r = (r ^ (r >> 30)) * 0xBF58476D1CE4E5B9ull; r = (r ^ (r >> 27)) * 0x94D049BB133111EBull; r ^= r >> 31; return (double)(r >> 11) / 9007199254740992.0; }
  static double AccentTarget(double accent)
  {
    using namespace detail; const int n = kAccentN; if (accent <= kAccent[0].x) return kAccent[0].y;
    for (int i = 1; i < n; ++i) if (accent <= kAccent[i].x) return kAccent[i - 1].y + (accent - kAccent[i - 1].x) / (kAccent[i].x - kAccent[i - 1].x) * (kAccent[i].y - kAccent[i - 1].y);
    return kAccent[n - 1].y;
  }
  void BuildSnapshot();   // steady state of the slow states at rest for the current knobs

  Knobs mK; bool mKnobsValid = false, mSnapValid = false, mActive = false; double mSr = 0.0, mDt = 0.0;
  double mC0 = 0, mCE = 0, mCL = 0, mTau = 0, mDecay = 0, mSLfo = 0, mLfoA = 0, mLfoDwell = 0, mR1 = 0, mAOta = 0, mAOut = 0, mACap3 = 0, mACap0 = 0, mAlphaHp = 0;
  double mSnapVcap = 0, mSnapVc = 0, mSnapY = 0;
  // running state
  unsigned long long mRng = 0; long mI = 0; bool mWasIdle = false, mIdleRunning = true, mAttacking = true, mHpFirst = true;
  double mE = 0.0, mERest = 0.0, mEt = 0, mTTrig = 0, mX = 0, mL = 0, mHold = 0, mVcap = 0, mVc = 0, mY = 0, mYPrev = 0, mYo = 0, mN385 = 0, mHpX1 = 0, mHpY1 = 0; int mVdir = 1, mLdir = 1;
  detail::Q22Transient mQ22;
};

inline void TR808Pc2Fast::BuildSnapshot()
{
  using namespace detail;
  const Tabs& T = Tables(); const double E0 = mERest, vRest = mC0 + mCE * E0; const bool running = vRest > 0.0755;
  // coupling capacitor C2157: it settles at the mean of the VCO output (tau 0.47 s / 1.0 s)
  if (running)
  {
    const double rUp = ((vRest * 0.5 - 0.02) / 47e3 - vRest / 200e3) * 1e9, rDn = vRest / 200e3 * 1e9;
    const double pUp = T.up(std::max(rUp, 1.0)), pDn = T.dn(std::max(rDn, 1.0)), tUp = (pUp + pDn) / std::max(rUp, 1.0), tDn = (pUp + pDn) / std::max(rDn, 1.0);
    mSnapVcap = mK.sw3 ? 0.5 * (pUp - pDn) : (14.25 * tUp - 14.31 * tDn) / (tUp + tDn);
  }
  else mSnapVcap = mK.sw3 ? -kRailPc2 : -14.31;
  // output stage at rest (E at its rest value, OTA output 0): steady state of the summing node z and the cap voltage vc (R260 / C293, tau up to 15 s): the DC solution
  const double n368 = 0.00244, iin = (0.0 - n368) / 100e3 + (0.0008 - n368) / 220e3, vg = -E0, R1 = mR1;
  double z = 0.0, vc = 0.0;
  auto F = [&](double zz, double vcc, double& f1, double& f2) {
    const double vds = zz - vcc - n368; double id, gd; jfet(vg, n368, vds + n368, id, gd);
    f1 = iin + (zz - n368) / 470e3 + vds / R1 + id; f2 = vds / R1 + id - (vg - (zz - vcc)) / 470e3;
  };
  for (int it = 0; it < 60; ++it)
  {
    double f1, f2, a1, a2, b1, b2; F(z, vc, f1, f2); const double h = 1e-6; F(z + h, vc, a1, a2); F(z, vc + h, b1, b2);
    const double j11 = (a1 - f1) / h, j12 = (b1 - f1) / h, j21 = (a2 - f2) / h, j22 = (b2 - f2) / h, det = j11 * j22 - j12 * j21;
    if (std::fabs(det) < 1e-30) break;
    const double dz = (f1 * j22 - f2 * j12) / det, dv = (j11 * f2 - j21 * f1) / det;
    z -= std::max(-0.5, std::min(0.5, dz)); vc -= std::max(-0.5, std::min(0.5, dv));
    if (std::fabs(dz) < 1e-12 && std::fabs(dv) < 1e-12) break;
  }
  mSnapY = z; mSnapVc = vc; mSnapValid = true;
}

inline double TR808Pc2Fast::Step()
{
  using namespace detail;
  if (!mActive) return 0.0;
  const Tabs& T = Tables(); const double dt = mDt, t = mI * dt;
  const bool sw2 = mK.sw2 != 0, sw3 = mK.sw3 != 0;
  if (mAttacking && t - mTTrig < 2e-3 && mE < mEt)
  {   // attack: the engine's rise of E after the trigger (concave, the same for every accent) up to the accent target; engine samples at 48 kHz, linear in time between them and beyond the last one
    static constexpr double kRise[30] = {0.0194, 0.3883, 0.8000, 1.1989, 1.5852, 1.9594, 2.3219, 2.6730, 3.0131, 3.3426, 3.6617, 3.9708, 4.2702, 4.5603, 4.8412, 5.1134, 5.3770, 5.6324, 5.8798, 6.1194, 6.3516, 6.5765, 6.7943, 7.0053, 7.2098, 7.4078, 7.5997, 7.7855, 7.9656, 8.1391};
    constexpr double kRiseDt = 1.0 / 48000.0; const double pos = (t + dt - mTTrig) / kRiseDt;   // the first sample is at one sample period after the trigger
    const int i0 = std::min((int)pos, 28); const double rise = pos < 1.0 ? kRise[0] * pos : kRise[i0 - 1] + (kRise[i0] - kRise[i0 - 1]) * (pos - i0);
    mE = std::min(mERest + rise, mEt); mAttacking = mE < mEt;
  }
  else { mAttacking = false; mE = mERest + (mE - mERest) * mDecay; }
  const double sinceTrig = t - mTTrig;
  const bool idle = !mAttacking && mE < kIdleE && sinceTrig > kIdleAfter;   // inaudible tail: oscillators and OTA are not run, the stage is updated at 1/64 rate
  double v394 = 0;
  if (idle)
  {
    if (!mWasIdle)
    {   // entering the tail: the VCO either keeps running (arbitrary phase) or has stalled at its rail
      const double vRest = mC0 + mCE * mE; mIdleRunning = vRest > 0.0755;
      if (!mIdleRunning) { if (vRest >= 0) { mVdir = 1; mX = -kRailPc2; } else mX = mVdir > 0 ? -kRailPc2 : kRailPc2; }
      mWasIdle = true;
    }
    const double xt = mIdleRunning ? 0.0 : mX;
    mVcap += (sw3 ? mACap3 : mACap0) * ((sw3 ? xt : (xt > 0 ? 14.25 : -14.31)) - mVcap);
  }
  else
  {
    if (mWasIdle)
    {   // leaving the tail: free-running oscillators have an arbitrary phase
      mWasIdle = false;
      if (mIdleRunning) { mX = (Rnd() * 2 - 1) * 7.1; mVdir = Rnd() < 0.5 ? 1 : -1; mL = (Rnd() * 2 - 1) * 6.6; mLdir = Rnd() < 0.5 ? 1 : -1; mHold = 0; }
    }
    // LFO
    double rem = dt;
    while (rem > 0)
    {
      if (mHold > 0) { const double h = std::min(mHold, rem); mHold -= h; rem -= h; if (mHold <= 0) mLdir = -mLdir; continue; }
      const double sl = mLdir > 0 ? mSLfo - 3.0 : mSLfo + 3.0, tgt = mLdir > 0 ? mLfoA : -mLfoA, tt = (tgt - mL) / (sl * mLdir);
      if (tt <= rem) { mL = tgt; rem -= tt; mHold = mLfoDwell; } else { mL += mLdir * sl * rem; rem = 0; }
    }
    const double vl = sw2 ? -14.277 * mLdir : mL;
    v394 = std::min(std::max(mC0 + mCE * mE + mCL * vl, -13.70), 13.70);   // U25B output limit
    // VCO: signed integrator rate; V394 <= ~0.08 V (or < 0) lets the integrator run into its rail and the oscillator stalls
    rem = dt;
    while (rem > 0)
    {
      const double r = mVdir > 0 ? ((v394 * 0.5 - 0.02) / 47e3 - v394 / 200e3) * 1e9 : -v394 / 200e3 * 1e9;
      if (std::fabs(r) < 1e-3) break;
      double tgt; bool flip;
      if (mVdir > 0) { if (r > 0) { tgt = T.up(r); flip = true; } else { tgt = -kRailPc2; flip = false; } }
      else { if (r < 0) { tgt = -T.dn(-r); flip = true; } else { tgt = kRailPc2; flip = false; } }
      const double tt = std::max((tgt - mX) / r, 0.0);
      if (tt <= rem) { mX = tgt; rem -= tt; if (flip) mVdir = -mVdir; else rem = 0; } else { mX += r * rem; rem = 0; }
    }
  }
  {   // Q22 transient block: fine grid right after a trigger; later the ladder is slow: one backward-Euler step per 8 samples after 30 ms, per 64 after 0.3 s
    const double since = t - mTTrig; const long dec = since < 0.005 ? 1 : (since < kQ22SlowAfter ? 2 : (since < 0.3 ? kQ22Decim : kQ22Decim2));
    if (dec == 1 || (mI & (dec - 1)) == 0)
    {
      const double span = dt * dec, e0 = mQ22.eOld; double tk = 0;
      while (tk < span - 1e-12) { const double h = std::min(since < 1.5e-3 ? kQ22Fine : span, span - tk); tk += h; mQ22.step(e0 + (mE - e0) * (tk / span), h); }   // E moves linearly across the interval
    }
  }
  // OTA
  if (!idle)
  {   // C2157 1u coupling (high pass, tau 0.47 s via 470k, 1.0 s via the 1M square path) into R218 1k
    const double vin = sw3 ? (mX - mVcap) * 1e3 / 471e3 : ((mVdir > 0 ? 14.25 : -14.31) - mVcap) * 1e3 / 1001e3;
    mVcap += (sw3 ? mACap3 : mACap0) * ((sw3 ? mX : (mVdir > 0 ? 14.25 : -14.31)) - mVcap);
    mN385 += mAOta * (33e3 * T.ic(mE) * std::tanh(vin / 52e-3) - mN385);
  }
  else mN385 = 0.0;
  if (!idle || (mI & 63) == 0)
  {
    const double n368 = 0.00244, C236 = 220e-12, C293 = 33e-6;
    const double dtS = idle ? dt * 64 : dt, gC = C236 / dtS;
    // output stage: iin + (z-n368)/470k + gC*(z-y) + (z-vc-n368)/R1 + Id(Vds) = 0
    const double iin = (mN385 - n368) / 100e3 + (mQ22.u[3] - n368) / 220e3, vg = -mE; double z = idle ? mY : 2.0 * mY - mYPrev; const double yOld = mY;
    // JFET pinched off (gate below Vto at both channel ends): the stage is linear, one direct solve
    const double zLin = (-iin + n368 / 470e3 + gC * mY + (mVc + n368) / mR1) / (1.0 / 470e3 + gC + 1.0 / mR1);
    const bool pinched = vg - std::min(n368, zLin - mVc) - gVto <= 0.0;
    if (pinched) z = zLin;
    else for (int it = 0; it < 20; ++it)
    {
      const double vds = z - mVc - n368; double id, gd; jfet(vg, n368, vds + n368, id, gd);
      const double f = iin + (z - n368) / 470e3 + gC * (z - mY) + vds / mR1 + id, d = 1.0 / 470e3 + gC + 1.0 / mR1 + gd;
      const double step = f / d; z -= step; if (std::fabs(step) < 1e-7) break;
    }
    mYPrev = yOld; mY = z;
    { const double vds = mY - mVc - n368; double id = 0, gd = 0; if (!pinched) jfet(vg, n368, vds + n368, id, gd); const double fo = vds / mR1 + id - (vg - (mY - mVc)) / 470e3, kC = dtS / C293;
      // forward gate-drain junction (IS 10 fA) is stiff: implicit scalar Newton for the new cap voltage
      double vn = mVc + kC * fo;   // gate junction reverse or barely forward (< 0.2 V): its current is below 1e-12 A, explicit step
      if (vg - (mY - mVc) > 0.2) for (int it = 0; it < 40; ++it)
      {
        double dd; const double ex = Q22Transient::sexp((vg - (mY - vn)) / 0.025852, dd), igd = 10e-15 * (ex - 1.0), gg = 10e-15 * dd / 0.025852;
        const double g = vn - mVc - kC * (fo - igd), dg = 1.0 + kC * gg, st = std::max(-0.2, std::min(0.2, g / dg));
        vn -= st; if (std::fabs(st) < 1e-9) break;
      }
      mVc = vn; /* R260 470k from the gate (-E) and the forward gate-drain junction (IS 10 fA) feed the drain node and charge C293 */ }
  }
  mYo += mAOut * (mY * 0.857 - mYo);
  ++mI;
  // the 10.6 Hz coupling of Block 11 (15 uF, 1 kOhm) removes the DC offset of the circuit
  if (mHpFirst) { mHpX1 = mYo; mHpFirst = false; }
  mHpY1 = mAlphaHp * (mHpY1 + mYo - mHpX1); mHpX1 = mYo;
  return mHpY1;
}

}  // namespace pc2fast
}  // namespace dgg
