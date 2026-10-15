#pragma once
// TR-808 / Yocto snare drum, fast model. Derived from the NOISE and SD sheets of the common NGSpice circuit.
//
// tone path (deterministic): the two bridged-T resonators n31 / n33 are linear and independent of the three pots; the trigger stage gives them the plateau P of node n29
//   (a function of the accent only), n(t) = rest + a1(t) P + a2(t) P^2 + a3(t) P^3 with the tables of 808SnareTables.h (least squares over five accents).
// noise path (statistical): the noise component of n73 (the Q49 emitter) is a minimum-phase FIR of white noise times a measured rms envelope per VR9 (snappy) and accent and
//   2 ms bin (808SnareTables.h). Its spectrum does not depend on VR9 / time; VR8 does not touch it.
// noise path, deterministic part: the trigger couples into n73 (a bump of up to 30 mV in the first 0.3 ms, dying out within ~2 ms; depends on VR9 and the accent): table kEarlyN73 of 808SnareEarly.h
//   (full circuit with the noise off), bilinear in VR9 and accent, added to the noise component before the output stage.
// output stage: linear (C63, VR8 mix, C65 / R200, U13A with R210 / C70 and VR7 / C68, C69 / R205, R207, C502 and the bus load), six states, exact discretisation (augmented matrix
//   exponential, input linear within a sample); recomputed only when VR7 / VR8 change.
// 48 kHz (the rate of the validation); the voice resamples to the host rate.

#include "808SnareTables.h"
#include "808SnareTone.h"
#include "808SnareEarly.h"
#include "808SnareSub.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace dgg {
namespace snarefast {

namespace detail {
constexpr double kFs = 48000.0, kPi = 3.14159265358979323846;
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

// matrix exponential (scaling and squaring, Taylor) of the n x n matrix Z*h, row-major, n <= 12; setup only
inline void Expm(const double* Z, int n, double h, double* E)
{
  double A[144], T[144], R[144], N[144], nrm = 0;
  for (int i = 0; i < n; ++i) { double rs = 0; for (int j = 0; j < n; ++j) { A[i * n + j] = Z[i * n + j] * h; rs += std::fabs(A[i * n + j]); } nrm = std::fmax(nrm, rs); }
  int sq = 0; while (nrm > 0.5) { nrm *= 0.5; ++sq; }
  const double sc = std::ldexp(1.0, -sq);
  for (int i = 0; i < n * n; ++i) A[i] *= sc;
  for (int i = 0; i < n; ++i) for (int j = 0; j < n; ++j) { T[i * n + j] = i == j ? 1.0 : 0.0; R[i * n + j] = T[i * n + j]; }
  for (int k = 1; k <= 14; ++k)
  {
    for (int i = 0; i < n; ++i) for (int j = 0; j < n; ++j) { double s = 0; for (int m = 0; m < n; ++m) s += T[i * n + m] * A[m * n + j]; N[i * n + j] = s / k; }
    for (int i = 0; i < n * n; ++i) { T[i] = N[i]; R[i] += N[i]; }
  }
  for (int s = 0; s < sq; ++s)
  {
    for (int i = 0; i < n; ++i) for (int j = 0; j < n; ++j) { double q = 0; for (int m = 0; m < n; ++m) q += R[i * n + m] * R[m * n + j]; N[i * n + j] = q; }
    std::memcpy(R, N, sizeof(double) * n * n);
  }
  std::memcpy(E, R, sizeof(double) * n * n);
}

inline double Vr7Fraction(double p)   // taper A of VR7
{
  static const double K[11][2] = {{0, 0}, {.1, .01}, {.2, .025}, {.3, .055}, {.4, .105}, {.5, .16}, {.6, .26}, {.7, .4}, {.8, .65}, {.9, .93}, {1, 1}};
  if (p <= 0.0) return 0.0;
  for (int i = 1; i < 11; ++i) if (p <= K[i][0]) return K[i - 1][1] + (p - K[i - 1][0]) / (K[i][0] - K[i - 1][0]) * (K[i][1] - K[i - 1][1]);
  return 1.0;
}

constexpr double C63 = 47e-9, C65 = 47e-9, C68 = 33e-6, C70 = 220e-12, C502 = 10e-9, C69 = 10e-9, R200 = 47e3, R204 = 100e3, R205 = 27e3, R207 = 1e3, R210 = 470e3, RL = 6e3;

// the output stage: x = [C63 voltage (n36 - n31), C65 voltage (n37 - n81), V129, C68 voltage, Vout, C69 voltage (n79 - n73)], u = [n31, n33, n73]
struct Stage
{
  double R8a = 5e4, R8e = 5e4, R7 = 1.6e4, Ad[6][6] = {}, B0[6][3] = {}, B1[6][3] = {}, x0[6] = {}, x[6] = {}, up[3] = {}, AdS[6][6] = {}, B0S[6][3] = {};   // AdS / B0S: one of kSubSteps sub-intervals of a sample
  void Ode(const double* s, const double* u, double* d) const
  {
    const double a = s[0] + u[0], g = 1 / R8a + 1 / R8e + 1 / R204, k = R200 / R8e;
    const double c = (u[1] - s[1] + k * a / (R8a * g)) / (1 + k - k / (R8e * g)), V77 = (a / R8a + c / R8e) / g, n79 = s[5] + u[2];
    d[0] = -(a - V77) / (R8a * C63); d[1] = (c - V77) / (R8e * C65);
    d[3] = (s[2] - s[3]) / (R7 * C68);
    d[2] = (-V77 / R204 - n79 / R205 - s[2] / R210 - (s[2] - s[3]) / R7) / C70;
    d[4] = ((s[2] - s[4]) / R207 - s[4] / RL) / C502;
    d[5] = -n79 / (R205 * C69);
  }
  // pots as rotation positions 0..1; rest = n31 / n33 before the trigger; the state settles to the DC rest of the inputs
  void Setup(double tone01, double level01, const double* rest)
  {
    R8a = std::fmax(1e5 * tone01, 1.0); R8e = std::fmax(1e5 * (1 - tone01), 1.0); R7 = std::fmax(1e5 * Vr7Fraction(level01), 1.0);
    double Z[144] = {}, A[6][6], B[6][3], z6[6] = {}, z3[3] = {}, d0[6], d[6]; Ode(z6, z3, d0);
    for (int j = 0; j < 6; ++j) { double e[6] = {}; e[j] = 1; Ode(e, z3, d); for (int i = 0; i < 6; ++i) A[i][j] = d[i] - d0[i]; }
    for (int j = 0; j < 3; ++j) { double e[3] = {}; e[j] = 1; Ode(z6, e, d); for (int i = 0; i < 6; ++i) B[i][j] = d[i] - d0[i]; }
    const int m = 12;
    for (int i = 0; i < 6; ++i) { for (int j = 0; j < 6; ++j) Z[i * m + j] = A[i][j]; for (int j = 0; j < 3; ++j) Z[i * m + 6 + j] = B[i][j]; }
    for (int j = 0; j < 3; ++j) Z[(6 + j) * m + 9 + j] = 1.0;
    double E[144]; Expm(Z, m, 1.0 / kFs, E);
    for (int i = 0; i < 6; ++i) for (int j = 0; j < 6; ++j) Ad[i][j] = E[i * m + j];
    for (int i = 0; i < 6; ++i) for (int j = 0; j < 3; ++j) { B0[i][j] = E[i * m + 6 + j]; B1[i][j] = E[i * m + 9 + j] * kFs; }
    Expm(Z, m, 1.0 / (kFs * kSubSteps), E);
    for (int i = 0; i < 6; ++i) { for (int j = 0; j < 6; ++j) AdS[i][j] = E[i * m + j]; for (int j = 0; j < 3; ++j) B0S[i][j] = E[i * m + 6 + j]; }
    // settle: doubling of the constant-input map x -> Ad x + B0 u
    double M[6][6], c[6], u0[3] = {rest[0], rest[1], 0.0};
    std::memcpy(M, Ad, sizeof M);
    for (int i = 0; i < 6; ++i) { c[i] = 0; for (int j = 0; j < 3; ++j) c[i] += B0[i][j] * u0[j]; }
    for (int it = 0; it < 26; ++it)
    {
      double c2[6], M2[6][6];
      for (int i = 0; i < 6; ++i)
      {
        c2[i] = c[i]; for (int j = 0; j < 6; ++j) c2[i] += M[i][j] * c[j];
        for (int j = 0; j < 6; ++j) { double q = 0; for (int k = 0; k < 6; ++k) q += M[i][k] * M[k][j]; M2[i][j] = q; }
      }
      std::memcpy(M, M2, sizeof M); std::memcpy(c, c2, sizeof c);
    }
    for (int i = 0; i < 6; ++i) x0[i] = c[i];
    Reset(rest);
  }
  void Reset(const double* rest) { for (int i = 0; i < 6; ++i) x[i] = x0[i]; up[0] = rest[0]; up[1] = rest[1]; up[2] = 0.0; }
  // state change at the end of a sample from an input deviation dev[m] (mean per sub-interval, m = 0..kSubSteps-1) from the straight line the step assumes
  void SubSampleState(const double (*dev)[3], double* d) const
  {
    for (int i = 0; i < 6; ++i) d[i] = 0.0;
    for (int m = 0; m < kSubSteps; ++m)
    {
      double n[6]; for (int i = 0; i < 6; ++i) { double q = 0; for (int j = 0; j < 6; ++j) q += AdS[i][j] * d[j]; for (int j = 0; j < 3; ++j) q += B0S[i][j] * dev[m][j]; n[i] = q; }
      for (int i = 0; i < 6; ++i) d[i] = n[i];
    }
  }
  inline double Step(double u1, double u2, double u3)
  {
    const double u[3] = {u1, u2, u3}; double n[6];
    for (int i = 0; i < 6; ++i)
    {
      double q = 0; for (int j = 0; j < 6; ++j) q += Ad[i][j] * x[j];
      for (int j = 0; j < 3; ++j) q += B0[i][j] * up[j] + B1[i][j] * (u[j] - up[j]);
      n[i] = q;
    }
    for (int i = 0; i < 6; ++i) x[i] = n[i];
    for (int j = 0; j < 3; ++j) up[j] = u[j];
    return x[4];
  }
};
}  // namespace detail

class TR808SnareFast
{
public:
  static constexpr double kFs = detail::kFs;                 // internal rate of the model
  static constexpr double kAccentFullScaleVolt = 4.09;      // Dio.ino SetDacA: map(velocity, 0, 127, 0, 4090) mV

  TR808SnareFast() { mRng.Seed(1); }
  void SetSeed(uint64_t seed) { mRng.Seed(seed); }
  double noiseScale = 1.0, subScale = 1.0;   // test hooks: noiseScale scales the statistical noise component only (0 = the deterministic part alone); subScale scales the first-sample correction

  // Starts a hit: MIDI velocity (the accent voltage is vel / 127 * 4.09 V), the three pots as rotation positions 0..1. False (silent) for velocity 0.
  bool Start(int vel127, double tone01, double snappy01, double level01)
  {
    using namespace detail;
    mActive = false; if (vel127 <= 0) return false;
    tone01 = std::clamp(tone01, 0.0, 1.0); snappy01 = std::clamp(snappy01, 0.0, 1.0); level01 = std::clamp(level01, 0.0, 1.0);
    if (!mStageValid || tone01 != mTone || level01 != mLevel) { mStage.Setup(tone01, level01, kToneRest); mTone = tone01; mLevel = level01; mStageValid = true; }
    else mStage.Reset(kToneRest);
    const int v = std::min(vel127, 127); const double P = kPlateau[v]; mK = 0;
    {   // coefficients of the tone modes at the plateau P: cubic Hermite between the nodes, proportional to P below the first
      if (P <= kTonePlateau[0]) { const double f = std::fmax(P, 0.0) / kTonePlateau[0]; for (int m = 0; m < kToneModes; ++m) mCoef[m] = f * kToneCoef[0][m]; }
      else
      {
        int i = 0; while (i < kToneNodes - 2 && P > kTonePlateau[i + 1]) ++i;
        const double h = kTonePlateau[i + 1] - kTonePlateau[i], x = std::fmin((P - kTonePlateau[i]) / h, 1.0), x2 = x * x, x3 = x2 * x;
        const double h00 = 2 * x3 - 3 * x2 + 1, h10 = x3 - 2 * x2 + x, h01 = -2 * x3 + 3 * x2, h11 = x3 - x2;
        for (int m = 0; m < kToneModes; ++m) mCoef[m] = h00 * kToneCoef[i][m] + h10 * h * kToneDCoef[i][m] + h01 * kToneCoef[i + 1][m] + h11 * h * kToneDCoef[i + 1][m];
      }
    }
    // noise envelope on the bin grid for this snappy position and accent: ln-interpolation in both
    const double acc = (double)v / 127.0 * kAccentFullScaleVolt;
    int iv, ia; double wv, wa; Locate(kNoiseVr9, kNoiseNumVr9, snappy01, iv, wv); Locate(kNoiseAccents, kNoiseNumAccents, acc, ia, wa);
    if (acc < kNoiseAccents[0]) { wa = 0; ia = 0; for (int b = 0; b < kNoiseNumBins; ++b) mEnv[b] = 0.0; }   // below the first measured accent the noise gate is shut
    else
      for (int b = 0; b < kNoiseNumBins; ++b)
      {
        const double e00 = std::log(std::fmax((double)kNoiseEnv[iv][ia][b], 1e-9)), e10 = std::log(std::fmax((double)kNoiseEnv[iv + 1][ia][b], 1e-9)),
                     e01 = std::log(std::fmax((double)kNoiseEnv[iv][ia + 1][b], 1e-9)), e11 = std::log(std::fmax((double)kNoiseEnv[iv + 1][ia + 1][b], 1e-9));
        mEnv[b] = std::exp((1 - wv) * ((1 - wa) * e00 + wa * e01) + wv * ((1 - wa) * e10 + wa * e11));
      }
    {   // deterministic coupling of the trigger into n73: bilinear in VR9 and accent (below the first accent proportional to the accent)
      int ev, ea; double wv2, wa2; Locate(kEarlyVr9, kEarlyNumVr9, snappy01, ev, wv2); Locate(kEarlyAccents, kEarlyNumAccents, acc, ea, wa2);
      const double fade = acc < kEarlyAccents[0] ? acc / kEarlyAccents[0] : 1.0; if (acc < kEarlyAccents[0]) wa2 = 0.0;
      for (int k = 0; k < kEarlySamples; ++k)
        mEarly[k] = fade * ((1 - wv2) * ((1 - wa2) * kEarlyN73[ev][ea][k] + wa2 * kEarlyN73[ev][ea + 1][k]) + wv2 * ((1 - wa2) * kEarlyN73[ev + 1][ea][k] + wa2 * kEarlyN73[ev + 1][ea + 1][k]));
    }
    {   // what the first sample's trigger spike does inside the sample, which the straight line of the step does not see (n31 / n33; n73 stays within 1 mV there)
      int sa; double sw; Locate(kSubAccents, kSubNumAccents, acc, sa, sw);
      const double fade = acc < kSubAccents[0] ? acc / kSubAccents[0] : 1.0; if (acc < kSubAccents[0]) sw = 0.0;
      double dev[kSubSteps][3], devEnd[kSubSteps][3];
      for (int m = 0; m < kSubSteps; ++m)
        for (int c = 0; c < 2; ++c)
        {
          dev[m][c] = fade * ((1 - sw) * kSubTone[c][sa][m] + sw * kSubTone[c][sa + 1][m]); devEnd[m][c] = fade * ((1 - sw) * kSubEnd[c][sa][m] + sw * kSubEnd[c][sa + 1][m]);
          dev[m][2] = devEnd[m][2] = 0.0;
        }
      mStage.SubSampleState(dev, mSubState); mStage.SubSampleState(devEnd, mSubEndState);
    }
    for (double& r : mRing) r = 0.0; mPos = 0;
    mActive = true; return true;
  }

  bool Active() const { return mActive; }

  // Next sample of the output node sd_out (volts, DC removed), at 48 kHz.
  inline double Step()
  {
    using namespace detail;
    if (!mActive) return 0.0;
    const int ki = mK < kToneSamples ? mK : kToneSamples - 1;
    double u1 = kToneRest[0], u2 = kToneRest[1];
    for (int m = 0; m < kToneModes; ++m) { u1 += mCoef[m] * kToneModeN31[m][ki]; u2 += mCoef[m] * kToneModeN33[m][ki]; }
    constexpr int nt = kNoiseFirTaps;
    mPos = mPos + 1 == nt ? 0 : mPos + 1; const double g = mRng.N(); mRing[mPos] = g; mRing[mPos + nt] = g;
    // four independent partial sums: the dependent chain of 64 additions would otherwise set the speed
    const double* r = mRing + mPos + nt; double s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    for (int j = 0; j < nt; j += 4) { s0 += kNoiseFir[j] * r[-j]; s1 += kNoiseFir[j + 1] * r[-j - 1]; s2 += kNoiseFir[j + 2] * r[-j - 2]; s3 += kNoiseFir[j + 3] * r[-j - 3]; }
    const double s = (s0 + s1) + (s2 + s3);
    double env = 0.0;
    if (mK / kNoiseBinSamples < kNoiseNumBins)
    {
      const double tb = ((double)mK + 0.5) / kNoiseBinSamples - 0.5; int b0 = (int)std::floor(tb); double w = tb - b0; if (tb < 0) { b0 = 0; w = 0; }
      const int b1 = std::min(kNoiseNumBins - 1, b0 + 1); b0 = std::min(kNoiseNumBins - 1, b0); env = (1 - w) * mEnv[b0] + w * mEnv[b1];
    }
    ++mK;
    if (mK == 1 || mK == kSubEndSample + 1)
    {
      mStage.Step(u1, u2, noiseScale * env * s + (mK - 1 < kEarlySamples ? mEarly[mK - 1] : 0.0));
      const double* d = mK == 1 ? mSubState : mSubEndState; for (int i = 0; i < 6; ++i) mStage.x[i] += subScale * d[i];
      return mStage.x[4];
    }
    return mStage.Step(u1, u2, noiseScale * env * s + (mK - 1 < kEarlySamples ? mEarly[mK - 1] : 0.0));
  }

private:
  static void Locate(const double* grid, int n, double x, int& i, double& w)
  {
    if (x <= grid[0]) { i = 0; w = 0; return; }
    if (x >= grid[n - 1]) { i = n - 2; w = 1; return; }
    i = 1; while (x > grid[i]) ++i; --i; w = (x - grid[i]) / (grid[i + 1] - grid[i]);
  }

  detail::Rng mRng; detail::Stage mStage; bool mStageValid = false, mActive = false; double mTone = -1, mLevel = -1;
  double mCoef[kToneModes] = {}, mEarly[kEarlySamples] = {}, mSubState[6] = {}, mSubEndState[6] = {}, mEnv[kNoiseNumBins] = {}, mRing[2 * kNoiseFirTaps] = {}; int mPos = 0, mK = 0;
};

}  // namespace snarefast
}  // namespace dgg
