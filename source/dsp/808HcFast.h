#pragma once
// TR-808 / Yocto hand clap and maracas, fast model. Both positions of SW12 are driven by the noise source of the NOISE sheet, so the model is statistical:
// a hit is a function of (accent, level, noise seed) and a different noise realisation gives a different hit of the same statistics, as in the circuit.
// Derived from the NOISE and HC sheets of the common NGSpice circuit.
//
// Hand Clap (cp): white noise -> wn spectrum (AR(1) + floor fitted to the engine) -> pre filter (U21B band-pass, exact rational transfer function, bilinear) -> OTA
//   n207 = -K Ic(t) n204 with K = R383/(2 Vt) -> post filter (U19B stage with VR17, output load). The OTA control current Ic(t) comes from the control path (no tables): C144
//   envelope (D71/R365 charge, R368 path, discharge through R367 in four fixed windows) -> differential pair QHC71/72 with the TM3 tap and the accent capacitor (static solve).
// Maracas (ma): only the accent matters: the output noise is a minimum-phase FIR (spectrum from the engine) times a measured rms envelope per accent and 2 ms bin
//   (808HcMaTables.h) and the analytic level correction of the post stage.
// Both run at 48 kHz (the rate of the validation); the voice resamples to the host rate.
//
// Below the accent threshold of the sheet the circuit is silent (hand clap 1.85 V, maracas 1.72 V; MIDI velocity 58 / 54).

#include "808HcMaTables.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>

namespace dgg {
namespace hcfast {

namespace detail {
constexpr double kFs = 48000.0, kVt = 0.0258649, kPi = 3.14159265358979323846;

// Normal distribution by the ziggurat method of Marsaglia and Tsang (J. Stat. Softw. 5 (2000), 128 layers): one table lookup and one multiplication for 98.8 % of the samples.
struct ZigTables
{
  uint32_t kn[128]; double wn[128], fn[128];
  ZigTables()
  {
    const double m1 = 2147483648.0, vn = 9.91256303526217e-3; double dn = 3.442619855899, tn = dn; const double q = vn / std::exp(-0.5 * dn * dn);
    kn[0] = (uint32_t)((dn / q) * m1); kn[1] = 0; wn[0] = q / m1; wn[127] = dn / m1; fn[0] = 1.0; fn[127] = std::exp(-0.5 * dn * dn);
    for (int i = 126; i >= 1; --i) { dn = std::sqrt(-2.0 * std::log(vn / dn + std::exp(-0.5 * dn * dn))); kn[i + 1] = (uint32_t)((dn / tn) * m1); tn = dn; fn[i] = std::exp(-0.5 * dn * dn); wn[i] = dn / m1; }
  }
};
inline const ZigTables kZig;

struct Rng
{
  uint64_t s0 = 1, s1 = 2;
  void Seed(uint64_t seed) { s0 = seed * 0x9E3779B97F4A7C15ull + 1; s1 = (seed + 12345) * 0xBF58476D1CE4E5B9ull + 7; for (int i = 0; i < 8; ++i) Next(); }
  inline uint64_t Next() { uint64_t x = s0, y = s1; s0 = y; x ^= x << 23; s1 = x ^ y ^ (x >> 17) ^ (y >> 26); return s1 + y; }
  inline double U() { return ((Next() >> 11) + 0.5) * (1.0 / 9007199254740992.0); }
  inline double N()
  {
    const int32_t hz = (int32_t)(Next() >> 32); const int iz = hz & 127; const uint32_t a = hz < 0 ? 0u - (uint32_t)hz : (uint32_t)hz;
    if (a < kZig.kn[iz]) return hz * kZig.wn[iz];
    return NSlow(hz, iz);
  }
  double NSlow(int32_t hz, int iz)
  {
    const double r = 3.442619855899;
    for (;;)
    {
      double x = hz * kZig.wn[iz];
      if (iz == 0) { double y; do { x = -std::log(U()) * 0.2904764; y = -std::log(U()); } while (y + y < x * x); return hz > 0 ? r + x : -r - x; }   // the tail beyond r
      if (kZig.fn[iz] + U() * (kZig.fn[iz - 1] - kZig.fn[iz]) < std::exp(-0.5 * x * x)) return x;
      hz = (int32_t)(Next() >> 32); iz = hz & 127; const uint32_t a = hz < 0 ? 0u - (uint32_t)hz : (uint32_t)hz;
      if (a < kZig.kn[iz]) return hz * kZig.wn[iz];
    }
  }
};

// rational filter H(s) = num(s)/den(s) (descending powers, order n <= 4) -> bilinear (s = c (1 - z^-1)/(1 + z^-1)) -> transposed direct form II
struct Iir
{
  int n = 0; double b[5] = {}, a[5] = {}, z[5] = {};
  void Reset() { for (double& v : z) v = 0.0; }
  void FromAnalog(const double* num, int numLen, const double* den, int denLen, double c)
  {
    n = denLen - 1; double nn[5] = {}, dd[5] = {};
    Convert(num, numLen, c, nn); Convert(den, denLen, c, dd);
    const double a0 = dd[0];
    for (int i = 0; i <= n; ++i) { b[i] = nn[i] / a0; a[i] = dd[i] / a0; }
    for (int i = n + 1; i < 5; ++i) { b[i] = 0; a[i] = 0; }
    Reset();
  }
  inline double Step(double x)
  {
    const double y = b[0] * x + z[0];
    for (int i = 0; i < n - 1; ++i) z[i] = z[i + 1] + b[i + 1] * x - a[i + 1] * y;
    z[n - 1] = b[n] * x - a[n] * y;
    return y;
  }

private:
  void Convert(const double* p, int len, double c, double* out) const
  {
    const int deg = len - 1;
    for (int k = 0; k <= deg; ++k)
    {
      // coefficient of s^(deg-k) -> c^(deg-k) (1 - z^-1)^(deg-k) (1 + z^-1)^(n - (deg-k))
      const int pw = deg - k; double t[6] = {p[k] * std::pow(c, pw)}; int tl = 1;
      for (int i = 0; i < n; ++i)
      {
        const double sgn = i < pw ? -1.0 : 1.0; double u[6] = {};
        for (int j = 0; j < tl; ++j) { u[j] += t[j]; u[j + 1] += sgn * t[j]; }
        ++tl; for (int j = 0; j < tl; ++j) t[j] = u[j];
      }
      for (int i = 0; i <= n; ++i) out[i] += t[i];
    }
  }
};

// ---- control path of the hand clap (see hc_ctrl_study.py)
constexpr double C144 = 470e-9, R365 = 82e3, R367 = 330.0, R368 = 470e3, R371 = 15e3, kVlow = -14.9, kD71End = 31.667e-3;
constexpr double kWin[4][2] = {{0.0, 0.5286e-3}, {12.888e-3, 13.234e-3}, {25.690e-3, 26.039e-3}, {31.667e-3, 31.8385e-3}};   // Q73 on: fixed hardware times (engine at 384 kHz)
constexpr double kItail = 985e-6, kNvt = 0.0265, kDv0 = -0.0008, kBeta = 205.0;
struct Control
{
  double v = -0.51, vd = 0.52, tt = 0, tm3 = 0.6, acc = 4.09, xw = 0.0; unsigned nvd = 0;
  void Start(double tm, double ac) { xw = 0; nvd = 0; v = -0.51; vd = 0.52; tt = 0; tm3 = tm; acc = ac; }
  inline bool InWin(double t) const { for (int i = 0; i < 4; ++i) if (t > kWin[i][0] && t <= kWin[i][1]) return true; return false; }
  // advance the envelope by h (constant window / D71 state over h), exact for the linear ODE with the diode drop frozen at the start of the step
  inline void Adv(double h, bool win, bool d71)
  {
    double g = 0.9690 / R368, c0 = 0.0;
    if (d71) { if ((++nvd & 7) == 1) { const double I = std::fmax((-v - vd) / R365, 0.0); vd = 0.0493 * std::log1p(I / 4.352e-9) + 0.6458 * I; } g += 1.0 / R365; c0 += -vd / R365; }
    if (win) { g += 1.0 / R367; c0 += kVlow / R367; }
    const double vinf = c0 / g, e = std::exp(-h * g / C144); v = vinf + (v - vinf) * e;
  }
  inline void Step(double dt)
  {
    double t = tt; const double tEnd = tt + dt;
    for (;;)
    {
      double te = tEnd;
      for (int i = 0; i < 4; ++i) { if (kWin[i][0] > t + 1e-15 && kWin[i][0] < te) te = kWin[i][0]; if (kWin[i][1] > t + 1e-15 && kWin[i][1] < te) te = kWin[i][1]; }
      if (kD71End > t + 1e-15 && kD71End < te) te = kD71End;
      const double mid = 0.5 * (t + te); Adv(te - t, InWin(mid), mid < kD71End); t = te; if (te >= tEnd - 1e-15) break;
    }
    tt = tEnd;
  }
  // static solve of the differential pair: x = (V212 - V197 - DV0)/NVT; V212 and V197 are linear in Ic72 (base currents), Ic72 = Itail / (1 + e^x): monotone scalar Newton, warm started
  inline double Ic()
  {
    const double v215 = InWin(tt) ? kVlow : v - 0.0094, vp = acc - 1.02, v196 = tt < 2e-3 ? vp * (0.9 + 0.1 * tt / 2e-3) : vp * std::exp(-(tt - 2e-3) / 0.2745);
    const double g12 = 1 / R368 + 1 / R371, ga = 1.0 / (1e4 * (1 - tm3 + 1e-9)), gb = 1.0 / (1e4 * tm3 + 1e-9), gp = ga + gb, gq = 1 / 47e3, hq = gq / (gp + gq);
    const double g197 = gq + 1 / 330e3 + 1 / 2.2e3 - gq * hq;
    const double a1 = v215 / (R368 * g12), a2 = 1.0 / (kBeta * g12), b1 = (-15.0 * ga * hq + v196 / 330e3 + kItail / kBeta) / g197, b2 = 1.0 / (kBeta * g197);
    const double c0 = (a1 - b1 - kDv0) / kNvt, c1 = (a2 + b2) * kItail / kNvt;
    double x = xw;
    for (int it = 0; it < 6; ++it)
    {
      const double e = std::exp(x > 60 ? 60 : (x < -60 ? -60 : x)), sg = 1.0 / (1.0 + e), f = x - c0 - c1 * sg, df = 1.0 + c1 * e * sg * sg, st = f / df;
      x -= st; if (std::fabs(st) < 1e-6) break;
    }
    xw = x; return kItail / (1.0 + std::exp(x > 60 ? 60 : (x < -60 ? -60 : x)));
  }
};


// ---- second noise path of the hand clap: Q70 (collector n195, R363 / C146 / C152 / R376 into the summing node). Its gain follows the DC bias of its base, set by the slow RC ladder
// C139 (n190, the accent cap) - R348 - C138 (n194) - R349 - C137 (n189 = base) and the base-emitter junction with the emitter network (R369 || R200p). Tail of the hit (> 150 ms at TM3 0.6).
struct Q70Bias
{
  static constexpr double R348 = 1e6, R349 = 1e6, C139 = 18e-9, C138 = 47e-9, C137 = 33e-9, kIs = 35.1e-15 * 0.83, kBf = 222.0, kRe = 319.0, kVe0 = -0.4792, kStep = 0.5e-3, kTrigSec = 0.002;
  double v190 = 0, v194 = 0, v189 = 0, ic = 0, vp = 0, tt = 0;
  // rest voltage of the base node (TM3 sets it, see the engine) and the voltage the accent cap reaches during the 2 ms trigger, both from the engine
  static double Rest(double tm3)
  {
    static const double T[8] = {0.1, 0.2, 0.3, 0.45, 0.6, 0.75, 0.9, 1.0}, V[8] = {-0.0881, -0.1283, -0.1846, -0.2777, -0.3739, -0.4747, -0.5828, -0.6606};
    if (tm3 <= T[0]) return V[0];
    for (int i = 1; i < 8; ++i) if (tm3 <= T[i]) return V[i - 1] + (tm3 - T[i - 1]) / (T[i] - T[i - 1]) * (V[i] - V[i - 1]);
    return V[7];
  }
  static double AccentCap(double acc)
  {
    static const double A[5] = {2.0, 2.5, 3.2, 3.681, 4.09}, V[5] = {1.0853, 1.6095, 2.3067, 2.7829, 3.1875};
    if (acc <= A[0]) return V[0] + (acc - A[0]) * 1.05;
    for (int i = 1; i < 5; ++i) if (acc <= A[i]) return V[i - 1] + (acc - A[i - 1]) / (A[i] - A[i - 1]) * (V[i] - V[i - 1]);
    return V[4] + (acc - A[4]) * 1.0;
  }
  void Start(double acc, double tm3) { vp = AccentCap(acc); v190 = vp; v194 = v189 = Rest(tm3); tt = 0; Solve(); }
  // collector current of Q70 for the base voltage vb with the emitter degeneration (Newton on ic)
  double Collector(double vb) const
  {
    double i = 0.0; for (int it = 0; it < 30; ++it) { const double e = std::exp(std::fmin((vb - (kVe0 + kRe * i)) / kVt, 60.0)), f = i - kIs * e, df = 1.0 + kIs * e * kRe / kVt, st = f / df; i -= st; if (i < 0) i = 0; if (std::fabs(st) < 1e-15 + 1e-9 * i) break; }
    return i;
  }
  void Solve() { ic = Collector(v189); }
  // one backward-Euler step of length h; the accent cap is held at its trigger value for the first 2 ms
  void Advance(double h)
  {
    tt += h; const bool trig = tt <= kTrigSec;
    const double i0 = Collector(v189), g = i0 / (kBf * (kVt + kRe * i0)), ib0 = i0 / kBf;   // base current and its slope
    const double a = h / (R348 * C139), b = h / (R348 * C138), c = h / (R349 * C138), d = h / (R349 * C137), e = h * g / C137, f = h * (ib0 - g * v189) / C137;
    // unknowns (x190, x194, x189):  x190 - x190_old = -a (x190 - x194);  x194 - old = b (x190 - x194) - c (x194 - x189);  x189 - old = d (x194 - x189) - e x189 - f
    double A[3][4] = {{1 + a, -a, 0, v190}, {-b, 1 + b + c, -c, v194}, {0, -d, 1 + d + e, v189 - f}};
    if (trig) { A[0][0] = 1; A[0][1] = 0; A[0][3] = vp; }
    for (int cI = 0; cI < 3; ++cI)
    {
      int p = cI; for (int r = cI + 1; r < 3; ++r) if (std::fabs(A[r][cI]) > std::fabs(A[p][cI])) p = r;
      for (int j = 0; j < 4; ++j) std::swap(A[cI][j], A[p][j]);
      for (int r = cI + 1; r < 3; ++r) { const double m = A[r][cI] / A[cI][cI]; for (int j = cI; j < 4; ++j) A[r][j] -= m * A[cI][j]; }
    }
    double x[3]; for (int i = 2; i >= 0; --i) { double sum = A[i][3]; for (int j = i + 1; j < 3; ++j) sum -= A[i][j] * x[j]; x[i] = sum / A[i][i]; }
    v190 = x[0]; v194 = x[1]; v189 = x[2]; Solve();
  }
};

inline double LevelFraction(double pos)   // taper A of VR17
{
  static const double K[11][2] = {{0, 0}, {.1, .01}, {.2, .025}, {.3, .055}, {.4, .105}, {.5, .16}, {.6, .26}, {.7, .4}, {.8, .65}, {.9, .93}, {1, 1}};
  if (pos <= 0.0) return 0.0;
  for (int i = 1; i < 11; ++i) if (pos <= K[i][0]) return K[i - 1][1] + (pos - K[i - 1][0]) / (K[i][0] - K[i - 1][0]) * (K[i][1] - K[i - 1][1]);
  return 1.0;
}
}  // namespace detail

class TR808HcFast
{
public:
  static constexpr double kFs = detail::kFs;                                   // internal rate of the model
  static constexpr double kClapThresholdVolt = 1.85, kMaracasThresholdVolt = 1.72;   // accent threshold of the sheet (engine: silent below)
  static constexpr double kTm3 = 0.60;                                         // trimmer TM3 (C.P. OFFSET), the position of the deck

  TR808HcFast()
  {
    using namespace detail;
    // Pre: n204/wn = (-1.2563e-5 s^3 - 0.2585 s^2) / (1.312146e-3 s^3 + 8.143875 s^2 + 88960 s + 9.25e7), from the netlist (U21B network, C136 / R372 / R382)
    // = H199(s) (n199, the U21B output: second order) x the high-pass C136 / (R372 + R382) (tau 0.88 ms) / 40
    const double n199[3] = {-22842.0, -470000000.0, 0.0}, d199[3] = {59643.0, 302400000.0, 3700000000000.0}, hp[2] = {0.88e-3, 0.0}, hpd[2] = {0.88e-3, 1.0};
    mPre.FromAnalog(n199, 3, d199, 3, 2 * kFs); mHp.FromAnalog(hp, 2, hpd, 2, 2 * kFs);
    // wn: AR(1) + white floor, fitted to the engine's wn spectrum (A 3.693e-6 V^2/Hz, corner 5487 Hz, floor 2.456e-7)
    mAr = std::exp(-2 * kPi * 5487.0 / kFs); mSAr = std::sqrt(3.693e-6 * kFs * (1 - mAr) * (1 - mAr) / 2); mSW = std::sqrt(2.456e-7 * kFs / 2);
    mRng.Seed(1);
  }

  void SetSeed(uint64_t seed) { mRng.Seed(seed); }

  // Starts a hand clap; false (and silent) below the accent threshold.
  bool StartClap(double accentVolt, double level01)
  {
    using namespace detail;
    mActive = false; if (accentVolt < kClapThresholdVolt) return false;
    const double R17 = std::fmax(1e5 * LevelFraction(level01), 1.0), G = 1 / 470e3 + 1 / R17, C149 = 220e-12, C150 = 1e-9, tau1 = 100e3 * 1e-9, Rl = 6.0e3, ko = Rl / (1e3 + Rl), tauo = 1e3 * Rl / (1e3 + Rl) * 1e-8;
    // Post: -(1/(G + s C149)) * s C150 / (1 + s R378 C150) * Rl/(R296 + Rl) / (1 + s tau_o), G = 1/R375 + 1/R17 (C145 is a short above a few Hz)
    const double d1[2] = {C149, G}, d2[2] = {tau1, 1.0}, d3[2] = {tauo, 1.0}; double d12[3], d[4];
    Mul(d1, 2, d2, 2, d12); Mul(d12, 3, d3, 2, d);
    const double num[2] = {-C150 * ko, 0.0}; mPost.FromAnalog(num, 2, d, 4, 2 * kFs);
    // path of Q70 into the summing node: ko/(1 + s tau_o) * 1/(G + s C149) * s C152 / ((G363 + s C146)(1 + s R376 C152) + s C152)
    {
      const double C146 = 18e-9, C152 = 4.7e-9, tau152 = 47e3 * C152, G363 = 1e-4;
      const double b[3] = {C146 * tau152, C146 + G363 * tau152 + C152, G363}, a[2] = {C149, G}; double ab[4], dd[5]; Mul(a, 2, b, 3, ab); Mul(ab, 4, d3, 2, dd);
      const double n2[2] = {ko * C152, 0.0}; mPost2.FromAnalog(n2, 2, dd, 5, 2 * kFs);
    }
    mPre.Reset(); mHp.Reset(); mCtl.Start(kTm3, accentVolt); mQ70.Start(accentVolt, kTm3); mQ70N = 0; mGm = Gm(mQ70.ic); mWn = 0.0; mClap = true; mActive = true; return true;
  }

  // Starts maracas; false (and silent) below the accent threshold.
  bool StartMaracas(double accentVolt, double level01)
  {
    using namespace detail;
    mActive = false; if (accentVolt < kMaracasThresholdVolt) return false;
    // envelope of this accent on the bin grid: ln-interpolation between the measured accents
    int ia = 0; double wa = 0; const int na = kMaNumAccents;
    if (accentVolt <= kMaAccents[0]) { ia = 0; wa = 0; }
    else if (accentVolt >= kMaAccents[na - 1]) { ia = na - 2; wa = 1; }
    else { ia = 1; while (accentVolt > kMaAccents[ia]) ++ia; --ia; wa = (accentVolt - kMaAccents[ia]) / (kMaAccents[ia + 1] - kMaAccents[ia]); }
    for (int b = 0; b < kMaNumBins; ++b) mMaLn[b] = (1 - wa) * std::log(std::fmax((double)kMaEnv[ia][b], 1e-9)) + wa * std::log(std::fmax((double)kMaEnv[ia + 1][b], 1e-9));
    // the table holds the rms of a whole bin; for the geometric interpolation the value at the bin centre of an exponential slope s (nepers per bin) is rms / sqrt(sinh(s) / s)
    double ln0[kMaNumBins]; for (int b = 0; b < kMaNumBins; ++b) ln0[b] = mMaLn[b];
    for (int b = 0; b < kMaNumBins; ++b)
    {
      const double sl = (ln0[std::min(kMaNumBins - 1, b + 1)] - ln0[std::max(0, b - 1)]) / (double)(std::min(kMaNumBins - 1, b + 1) - std::max(0, b - 1));
      if (std::fabs(sl) > 1e-6) mMaLn[b] -= 0.5 * std::log(std::sinh(std::fabs(sl)) / std::fabs(sl));
    }
    // level correction relative to the reference level 0.75: C(s) = (Gref + s C149)/(G + s C149) as a one-zero FIR b0 + b1 z^-1 fitted over 1-24 kHz
    const double R17 = std::fmax(1e5 * LevelFraction(level01), 1.0), G = 1 / 470e3 + 1 / R17, Gref = 1 / 470e3 + 1 / (1e5 * LevelFraction(0.75)), C149 = 220e-12;
    double sn = 0, sc = 0, sr = 0, sq = 0; const int K = 96;
    for (int k = 0; k < K; ++k)
    {
      const double f = 1000.0 + (23000.0 * k) / (K - 1), w = 2 * kPi * f / kFs; const std::complex<double> sj(0, 2 * kPi * f), Cc = (Gref + sj * C149) / (G + sj * C149);
      sn += 1; sc += std::cos(w); sr += Cc.real(); sq += (Cc * std::complex<double>(std::cos(w), std::sin(w))).real();
    }
    const double det = sn * sn - sc * sc; mMaL0 = (sr * sn - sc * sq) / det; mMaL1 = (sn * sq - sc * sr) / det;
    for (double& r : mRing) r = 0.0; mPos = 0; mMaPrev = 0.0; mK = 0; mClap = false; mActive = true; return true;
  }

  bool Active() const { return mActive; }

  // Next sample of the output node (volts, DC removed), at 48 kHz.
  inline double Step()
  {
    using namespace detail;
    if (!mActive) return 0.0;
    if (mClap)
    {
      mWn = mAr * mWn + mRng.N(); const double wn = mSAr * mWn + mSW * mRng.N();
      const double n199 = mPre.Step(wn), n204 = mHp.Step(n199) * (1.0 / 40.0); mCtl.Step(1.0 / kFs); const double ic = mCtl.Ic();
      if (++mQ70N == kQ70Every) { mQ70N = 0; mQ70.Advance(kQ70Every / kFs); mGm = Gm(mQ70.ic); }
      return mPost.Step(-kK * ic * n204) + mPost2.Step(mGm * (0.04337 * n199));
    }
    constexpr int nt = kMaFirTaps;
    mPos = mPos + 1 == nt ? 0 : mPos + 1; const double g = mRng.N(); mRing[mPos] = g; mRing[mPos + nt] = g;
    // four independent partial sums: the dependent chain of 64 additions would otherwise set the speed
    const double* r = mRing + mPos + nt; double s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    for (int j = 0; j < nt; j += 4) { s0 += kMaFir[j] * r[-j]; s1 += kMaFir[j + 1] * r[-j - 1]; s2 += kMaFir[j + 2] * r[-j - 2]; s3 += kMaFir[j + 3] * r[-j - 3]; }
    const double s = (s0 + s1) + (s2 + s3);
    double env = 0.0;
    if (mK / kMaBinSamples < kMaNumBins)
    {
      const double tb = ((double)mK + 0.5) / kMaBinSamples - 0.5; int b0 = (int)std::floor(tb); double w = tb - b0; if (tb < 0) { b0 = 0; w = 0; }
      const int b1 = std::min(kMaNumBins - 1, b0 + 1); b0 = std::min(kMaNumBins - 1, b0); env = std::exp((1 - w) * mMaLn[b0] + w * mMaLn[b1]);   // geometric between the bin centres: the envelope falls by a factor 6 within 2 ms at the end of the burst
    }
    ++mK; const double xin = env * s, y = mMaL0 * xin + mMaL1 * mMaPrev; mMaPrev = xin; return y;
  }

private:
  static constexpr double kK = 82e3 / (2.0 * detail::kVt);   // OTA: gm = Ic/(2 Vt) into R383
  static constexpr int kQ70Every = 24;                         // the Q70 bias network is slow (ms): one step per 24 samples
  static double Gm(double ic) { return ic / (detail::kVt + detail::Q70Bias::kRe * ic); }   // Q70 transconductance with the emitter degeneration
  static void Mul(const double* a, int na, const double* b, int nb, double* r) { for (int i = 0; i < na + nb - 1; ++i) r[i] = 0; for (int i = 0; i < na; ++i) for (int j = 0; j < nb; ++j) r[i + j] += a[i] * b[j]; }

  detail::Rng mRng; detail::Iir mPre, mHp, mPost, mPost2; detail::Control mCtl; detail::Q70Bias mQ70; double mGm = 0.0; int mQ70N = 0;
  double mAr = 0, mSAr = 0, mSW = 0, mWn = 0;
  double mMaLn[kMaNumBins] = {}, mRing[2 * kMaFirTaps] = {}, mMaL0 = 1, mMaL1 = 0, mMaPrev = 0; int mPos = 0; long mK = 0;
  bool mClap = true, mActive = false;
};

}  // namespace hcfast
}  // namespace dgg
