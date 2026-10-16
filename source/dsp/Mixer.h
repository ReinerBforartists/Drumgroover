// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Reiner Prokein
#pragma once
// ============================================================================
// MIXER — per-channel signal chain for the 15 instrument channels, master
// bus and reverb/delay buses.
//
// Header-only, dependency-free C++17, same convention as the other dsp/
// headers. Parameter state (MixerState) is a plain value: the owning plugin
// keeps one copy guarded by a mutex (UI thread writes), and snapshots it
// into MixerProcessor::Process() on the audio thread.
//
// Signal flow per instrument channel (mono voice sum in):
//   FX slots (UI order, any of EQ/Comp/Sat/Transient/Limiter)
//     -> Fader (gainDb) -> Equal-Power Pan -> Mute/Solo gate
//     -> Master sum  (+ post-fader Reverb/Delay sends)
//
// Buses:  send sum (mono) -> Reverb/Delay module -> bus fader/mute -> Master
// Master: L/R sum + returns -> FX slots (L and R through separate module
//         instances, same params) -> master fader -> output
// ============================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace dgg {

// ---------------------------------------------------------------------------
// Layout — must stay in sync with FIXED_CHANNELS / BUS_CHANNELS in
// resources/web/mixer-tab.js (same order, same ids).
// ---------------------------------------------------------------------------
enum class MixerChannel : int
{
  Kick = 0,
  Snare,
  HiHat,
  RackTom1,
  RackTom2,
  RackTom3,
  FloorTom1,
  FloorTom2,
  Cymbal1,
  Cymbal2,
  Cymbal3,
  Cymbal4,
  Cymbal5,
  Ride,
  Custom,
  Count   // == 15 instrument channels
};

constexpr int kMixerNumChannels = static_cast<int>(MixerChannel::Count);

// FX module types (what a slot contains). Off == empty slot.
enum class FxType : int
{
  Off    = -1,
  Eq     = 0,
  Comp   = 1,
  Sat    = 2,
  Ts     = 3,
  Lim    = 4,
  Reverb = 5,   // bus module (reverb bus only)
  Delay  = 6    // bus module (delay bus only)
};

// Max params per module (the EQ uses 9: Freq+Gain+Q per band).
constexpr int kFxMaxParams = 12;

struct FxSlotState
{
  int type = -1;   // FxType::Off
  bool on = true;  // bypass flag (UI "an/aus")
  double p[kFxMaxParams] = { 0.0 };
};

// Param indices per module type (into FxSlotState::p).
namespace FxParam
{
  // Eq — full 3-band: each band has Freq + Gain + Q (Q = peaking width on the mid
  // peak, shelf slope on low/high). Order MUST match JS FX_DEFS.eq (positional).
  enum {
    EqLowFreqHz = 0, EqLowGainDb, EqLowQ,
    EqMidFreqHz,     EqMidGainDb, EqMidQ,
    EqHighFreqHz,    EqHighGainDb, EqHighQ
  };
  // Comp
  enum { CompThresholdDb = 0, CompRatio, CompAttackMs, CompReleaseMs, CompMakeupDb };
  // Sat
  enum { SatDrivePct = 0 };
  // Ts — amounts in -100..+100 (UI scale), converted to -1..+1 internally.
  enum { TsAttackAmt = 0, TsSustainAmt };
  // Lim — order MUST match JS FX_DEFS.lim (positional): Ceiling, Attack, Release.
  enum { LimCeilingDb = 0, LimAttackMs, LimReleaseMs };
  // Reverb (bus module)
  enum { RevSizePct = 0, RevDecaySec, RevDampingPct, RevMixPct };
  // Delay (bus module)
  enum { DelTimeMs = 0, DelFeedbackPct, DelMixPct, DelMode };
}

struct ChannelState
{
  double gainDb = 0.0;      // UI fader default (unity — no built-in headroom cut)
  double pan = 0.0;         // -1..+1 (L..R), equal-power
  bool mute = false;
  bool solo = false;
  double sendReverb = 0.0;  // 0..1, post-fader
  double sendDelay = 0.0;   // 0..1, post-fader
  double autoTargetDb = 0.0; // UI only (Auto gain target peak); stored with the mixer state, not used by the DSP
  FxSlotState fx[4];        // chain order == array order (UI slot order)
};

struct BusState
{
  double gainDb = 0.0;      // unity — no built-in headroom cut
  bool mute = false;
  double pan = 0.0;         // -1..+1 equal-power pan of the bus return (L..R), center=0
  FxSlotState fx[1];        // [0] = the bus's own module (Reverb / Delay)
};

// Default FX chain + param values per slot. MUST stay in sync with the JS
// defaults (resources/web/mixer-tab.js, FX_DEFS) so the settings popover and
// the C++ DSP agree on a fresh boot.
inline void FxSlotDefaultEq(FxSlotState& s)   {
  s.type = static_cast<int>(FxType::Eq); s.on = true;
  // Low/High Q default to 3.0 (a clearly-defined shelf, on the sharper side of the range);
  // Mid Q stays at 1.0 (standard parametric peak). See SetLowShelf/SetHighShelf for the mapping.
  s.p[FxParam::EqLowFreqHz]  = 250.0;  s.p[FxParam::EqLowGainDb]  = 0.0; s.p[FxParam::EqLowQ]  = 3.0;
  s.p[FxParam::EqMidFreqHz]  = 1000.0; s.p[FxParam::EqMidGainDb]  = 0.0; s.p[FxParam::EqMidQ]  = 1.0;
  s.p[FxParam::EqHighFreqHz] = 4000.0; s.p[FxParam::EqHighGainDb] = 0.0; s.p[FxParam::EqHighQ] = 3.0;
}
inline void FxSlotDefaultLim(FxSlotState& s)  { s.type = static_cast<int>(FxType::Lim);  s.on = true; s.p[FxParam::LimCeilingDb] = -0.3; s.p[FxParam::LimAttackMs] = 0.5; s.p[FxParam::LimReleaseMs] = 50; }

struct MixerState
{
  double kitAutoTargetDb = 0.0; // UI only (Auto gain target peak of the Kit Gain strip); stored with the mixer state, not used by the DSP
  double kitGainDb = 0.0;   // gain on the input of every instrument channel (before EQ / FX / fader), -24..+24 dB
  ChannelState channels[kMixerNumChannels];
  ChannelState master;      // master strip: fader + Pan + FX chain (no sends)
  BusState reverbBus;
  BusState delayBus;

  bool AnySolo() const
  {
    for (int i = 0; i < kMixerNumChannels; ++i)
      if (channels[i].solo) return true;
    return false;
  }

  // Fresh-boot default: every instrument channel starts with EMPTY FX slots —
  // the user builds their own chain. Only the master strip gets a sensible
  // default (EQ -> Limiter); its remaining slots stay empty too. Called once
  // from the plugin constructor.
  void ApplyDefaults()
  {
    for (auto& c : channels)
      for (auto& fx : c.fx) fx = FxSlotState();   // all slots empty (type Off)

    FxSlotDefaultEq(master.fx[0]);   // EQ
    FxSlotDefaultLim(master.fx[1]);  // Limiter
    master.fx[2] = FxSlotState();    // empty
    master.fx[3] = FxSlotState();    // empty
  }
};

// ---------------------------------------------------------------------------
// Biquad — RBJ "Audio EQ Cookbook" filters, transposed direct form II.
// ---------------------------------------------------------------------------
struct Biquad
{
  double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
  double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;

  void Reset() { x1 = x2 = y1 = y2 = 0.0; }

  static double ClampFreq(double f, double sr) { return std::max(20.0, std::min(f, sr * 0.49)); }

  void SetPeak(double freqHz, double gainDb, double q, double sr)
  {
    const double A = std::pow(10.0, gainDb / 40.0);
    const double w0 = 2.0 * M_PI * ClampFreq(freqHz, sr) / sr;
    const double cw = std::cos(w0), sw = std::sin(w0);
    const double alpha = sw / (2.0 * std::max(0.01, q));
    const double a0 = 1.0 + alpha / A;
    b0 = (1.0 + alpha * A) / a0;
    b1 = (-2.0 * cw) / a0;
    b2 = (1.0 - alpha * A) / a0;
    a1 = (-2.0 * cw) / a0;
    a2 = (1.0 - alpha / A) / a0;
  }

  // Low shelf (RBJ). q = shelf slope. At gainDb==0 (A==1) the response is exactly
  // flat — identical structure to the JS eqShelf() curve, so the drawn curve always
  // matches the audio.
  void SetLowShelf(double freqHz, double gainDb, double q, double sr)
  {
    const double A = std::pow(10.0, gainDb / 40.0);
    const double w0 = 2.0 * M_PI * ClampFreq(freqHz, sr) / sr;
    const double cw = std::cos(w0), sw = std::sin(w0), sA = std::sqrt(A);
    // knob q [0.3,4] -> RBJ shelf-Q [0.3, 1/sqrt(2)]: higher knob = sharper transition.
    // Hard-capped at 1/sqrt(2) (~0.707): per the RBJ Audio EQ Cookbook that is the steepest
    // shelf that stays MONOTONIC — anything above it becomes a resonant peak + notch, which
    // is what made high settings sound "mushy" and pushed the response past its +/-gain limits.
    const double t = std::min(1.0, std::max(0.0, (q - 0.3) / 3.7));
    const double qs = 0.3 + (0.7071 - 0.3) * t;
    const double alpha = sw / (2.0 * qs);
    const double a0 = (A + 1.0) + (A - 1.0) * cw + 2.0 * sA * alpha;
    b0 = A * ((A + 1.0) - (A - 1.0) * cw + 2.0 * sA * alpha) / a0;
    b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cw) / a0;
    b2 = A * ((A + 1.0) - (A - 1.0) * cw - 2.0 * sA * alpha) / a0;
    a1 = (-2.0 * ((A - 1.0) + (A + 1.0) * cw)) / a0;
    a2 = ((A + 1.0) + (A - 1.0) * cw - 2.0 * sA * alpha) / a0;
  }

  // High shelf (RBJ). q = shelf slope. Flat at gainDb==0, matches JS eqShelf().
  void SetHighShelf(double freqHz, double gainDb, double q, double sr)
  {
    const double A = std::pow(10.0, gainDb / 40.0);
    const double w0 = 2.0 * M_PI * ClampFreq(freqHz, sr) / sr;
    const double cw = std::cos(w0), sw = std::sin(w0), sA = std::sqrt(A);
    // knob q [0.3,4] -> RBJ shelf-Q [0.3, 1/sqrt(2)]: higher knob = sharper transition.
    // Hard-capped at 1/sqrt(2) (~0.707): per the RBJ Audio EQ Cookbook that is the steepest
    // shelf that stays MONOTONIC — anything above it becomes a resonant peak + notch, which
    // is what made high settings sound "mushy" and pushed the response past its +/-gain limits.
    const double t = std::min(1.0, std::max(0.0, (q - 0.3) / 3.7));
    const double qs = 0.3 + (0.7071 - 0.3) * t;
    const double alpha = sw / (2.0 * qs);
    const double a0 = (A + 1.0) - (A - 1.0) * cw + 2.0 * sA * alpha;
    b0 = A * ((A + 1.0) + (A - 1.0) * cw + 2.0 * sA * alpha) / a0;
    b1 = (-2.0 * A * ((A - 1.0) + (A + 1.0) * cw)) / a0;
    b2 = A * ((A + 1.0) + (A - 1.0) * cw - 2.0 * sA * alpha) / a0;
    a1 = (2.0 * ((A - 1.0) - (A + 1.0) * cw)) / a0;
    a2 = ((A + 1.0) - (A - 1.0) * cw - 2.0 * sA * alpha) / a0;
  }

  inline double Process(double x)
  {
    const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
    x2 = x1; x1 = x;
    y2 = y1; y1 = y;
    return y;
  }
};

// 3-band EQ: low shelf (fixed freq) + parametric mid peak + high shelf (fixed freq).
struct Eq3Band
{
  Biquad low, mid, high;

  void SetParams(double lowFreq, double lowGainDb, double lowQ,
                 double midFreq, double midGainDb, double midQ,
                 double highFreq, double highGainDb, double highQ, double sr)
  {
    low.SetLowShelf(lowFreq, lowGainDb, lowQ, sr);
    mid.SetPeak(midFreq, midGainDb, midQ, sr);
    high.SetHighShelf(highFreq, highGainDb, highQ, sr);
  }

  inline double Process(double x) { return high.Process(mid.Process(low.Process(x))); }
  void Reset() { low.Reset(); mid.Reset(); high.Reset(); }
};

// ---------------------------------------------------------------------------
// Feedforward VCA compressor: envelope follower -> ratio gain -> smoothed
// gain reduction -> static makeup.
// ---------------------------------------------------------------------------
struct VcaCompressor
{
  double mEnv = 0.0;
  double mGainSm = 1.0;
  double mA = 1.0, mR = 1.0, mG = 1.0;   // smoothing coefficients (per sample)
  double mThr = 0.25, mRatio = 4.0, mMakeup = 1.0;

  void SetParams(double thrDb, double ratio, double atkMs, double relMs, double makeupDb, double sr)
  {
    const double dt = 1.0 / sr;
    mA = std::exp(-dt / (std::max(0.05, atkMs) * 1e-3));
    mR = std::exp(-dt / (std::max(1.0, relMs) * 1e-3));
    const double gCoef = std::exp(-dt / 0.002); // fixed ~2 ms zipper smoothing
    mG = gCoef;
    mThr = std::pow(10.0, thrDb / 20.0);
    mRatio = std::max(1.0, ratio);
    mMakeup = std::pow(10.0, makeupDb / 20.0);
  }

  void Reset() { mEnv = 0.0; mGainSm = 1.0; }

  inline double Process(double x)
  {
    const double abs = std::fabs(x);
    const double coef = (abs > mEnv) ? mA : mR;
    mEnv += coef * (abs - mEnv);

    double target = 1.0;
    if (mEnv > mThr)
      target = (mThr + (mEnv - mThr) / mRatio) / mEnv;

    mGainSm += mG * (target - mGainSm);
    return x * mGainSm * mMakeup;
  }
};

// ---------------------------------------------------------------------------
// Soft-clip saturation: tanh(drive*x) normalized to unity at full scale.
// ---------------------------------------------------------------------------
struct Saturation
{
  double mDrive = 0.0;
  double mNorm = 1.0;

  void SetParams(double drivePct)
  {
    const double d = std::max(0.0, std::min(100.0, drivePct)) / 100.0 * 5.0; // 0..5
    mDrive = d;
    mNorm = (d > 1e-3) ? 1.0 / std::tanh(d) : 1.0;
  }

  inline double Process(double x)
  {
    if (mDrive < 1e-3) return x;
    return std::tanh(mDrive * x) * mNorm;
  }
};

// ---------------------------------------------------------------------------
// Transient shaper: gain jumps toward (1+attack) on rising signal, relaxes
// toward (1+sustain) — amounts in -1..+1.
// ---------------------------------------------------------------------------
struct TransientShaper
{
  double mFast = 0.0, mPrevAbs = 0.0, mGain = 1.0;
  double mFastDecay = 1.0, mRelax = 1.0;
  double mAtkG = 1.0, mSusG = 1.0;

  void SetParams(double atkAmt, double susAmt, double sr)
  {
    const double dt = 1.0 / sr;
    mFastDecay = std::exp(-dt / 0.008);  // ~8 ms transient tail
    mRelax = std::exp(-dt / 0.004);      // gain relaxation toward sustain level
    mAtkG = 1.0 + std::max(-1.0, std::min(1.0, atkAmt));
    mSusG = 1.0 + std::max(-1.0, std::min(1.0, susAmt));
  }

  void Reset() { mFast = 0.0; mPrevAbs = 0.0; mGain = 1.0; }

  inline double Process(double x)
  {
    const double abs = std::fabs(x);
    const bool rising = (abs > mPrevAbs);
    mPrevAbs = abs;
    if (abs > mFast) mFast = abs; else mFast *= mFastDecay;
    const double target = rising ? mAtkG : mSusG;
    mGain += mRelax * (target - mGain);
    return x * mGain;
  }
};

// ---------------------------------------------------------------------------
// Limiter: look-ahead peak limiter. The audio is delayed by Attack (L-1 samples);
// the gain needed for each incoming sample is min-filtered over L samples and then
// averaged over L samples, so the gain is already down when the peak leaves the delay
// (no overshoot, no clipping). Release is a one-pole recovery that never exceeds the
// look-ahead gain. The final clamp is only a numeric safety net.
// ---------------------------------------------------------------------------
struct Limiter
{
  double mGain = 1.0;       // gain applied to the current output sample
  double mCeiling = 0.98;
  double mRelCoef = 1.0;
  int mL = 0;               // look-ahead length in samples (>= 1)
  long long mN = 0;         // running sample counter
  std::vector<double> mDelay;       // ring, mL samples
  std::vector<double> mDqVal;       // monotonic deque for the sliding minimum (ring, mL entries)
  std::vector<long long> mDqIdx;
  int mDqHead = 0, mDqCount = 0;
  std::vector<double> mAvg;         // ring of the last mL minima
  double mAvgSum = 0.0;

  void SetParams(double ceilingDb, double attackMs, double releaseMs, double sr)
  {
    mCeiling = std::pow(10.0, ceilingDb / 20.0);
    mRelCoef = std::exp(-1.0 / (std::max(1.0, releaseMs) * 1e-3 * sr));
    // Attack = look-ahead time. Floored at 0.05 ms so a UI value of 0 stays tiny but finite.
    const int L = std::max(1, std::min(8192, static_cast<int>(std::lround(std::max(0.05, attackMs) * 1e-3 * sr))));
    if (L != mL)
    {
      mL = L;
      mDelay.assign(L, 0.0);
      mDqVal.assign(L, 1.0);
      mDqIdx.assign(L, 0);
      mAvg.assign(L, 1.0);
      Reset();
    }
  }

  void Reset()
  {
    mGain = 1.0; mN = 0; mDqHead = 0; mDqCount = 0; mAvgSum = static_cast<double>(mL);
    std::fill(mDelay.begin(), mDelay.end(), 0.0);
    std::fill(mAvg.begin(), mAvg.end(), 1.0);
  }

  inline double Process(double x)
  {
    if (mL <= 0) return x;
    const double ax = std::fabs(x);
    const double need = ax > mCeiling ? mCeiling / ax : 1.0;

    // Sliding minimum of `need` over the last mL samples.
    while (mDqCount > 0 && mDqVal[(mDqHead + mDqCount - 1) % mL] >= need) --mDqCount;
    while (mDqCount > 0 && mDqIdx[mDqHead] <= mN - mL) { mDqHead = (mDqHead + 1) % mL; --mDqCount; }
    const int tail = (mDqHead + mDqCount) % mL;
    mDqVal[tail] = need; mDqIdx[tail] = mN; ++mDqCount;
    const double m = mDqVal[mDqHead];

    // Average of the minima over mL samples (exact re-sum once per ring turn against drift).
    const int w = static_cast<int>(mN % mL);
    mAvgSum += m - mAvg[w];
    mAvg[w] = m;
    if (w == 0) { double s = 0.0; for (int i = 0; i < mL; ++i) s += mAvg[i]; mAvgSum = s; }
    const double s = std::min(1.0, mAvgSum / mL);

    const double rec = 1.0 - (1.0 - mGain) * mRelCoef;
    mGain = std::min(s, rec);

    // Delay line: write the newest sample, read the one from L-1 samples ago.
    mDelay[w] = x;
    const double y = mDelay[(w + 1) % mL] * mGain;
    ++mN;
    return y > mCeiling ? mCeiling : (y < -mCeiling ? -mCeiling : y);
  }
};

// ---------------------------------------------------------------------------
// Reverb — Schroeder/Moore: parallel damped combs -> two allpass sections
// per output channel. Mono in, stereo out (bus return; wet only).
// ---------------------------------------------------------------------------
struct Comb
{
  std::vector<double> mBuf;
  size_t mIdx = 0;
  int mDelay = 1;
  double mFilt = 0.0;      // damped feedback state (one-pole LP of delayed out)
  double mDampCoef = 1.0;  // one-pole LP coefficient
  double mFb = 0.5;

  void SetDelay(int d)
  {
    mDelay = std::max(1, d);
    if (static_cast<int>(mBuf.size()) < mDelay + 2) mBuf.assign(mDelay + 2, 0.0);
  }

  inline double Process(double x)
  {
    const double delayed = mBuf[mIdx];
    mFilt += mDampCoef * (delayed - mFilt);
    const double y = x + mFb * mFilt;
    mBuf[mIdx] = y;
    ++mIdx;
    if (mIdx >= mBuf.size()) mIdx = 0;
    return y;
  }
};

struct Allpass
{
  double mY1 = 0.0;
  double mG = 0.5;

  inline double Process(double x)
  {
    const double w = x - mG * mY1;
    const double y = mG * x + w;
    mY1 = y;
    return y;
  }
};

struct ReverbUnit
{
  static constexpr int kNumCombs = 6;

  Comb mCombs[kNumCombs];
  Allpass mApL[2], mApR[2];
  double mMix = 1.0;

  void Reset()
  {
    for (auto& c : mCombs) { c.mFilt = 0.0; std::fill(c.mBuf.begin(), c.mBuf.end(), 0.0); }
    mApL[0].mY1 = mApL[1].mY1 = mApR[0].mY1 = mApR[1].mY1 = 0.0;
  }

  void SetParams(double sizePct, double decaySec, double dampingPct, double mixPct, double sr)
  {
    const double size = std::max(0.0, std::min(100.0, sizePct)) / 100.0;
    const double T60 = std::max(0.05, std::min(12.0, decaySec));
    const double damp = std::max(0.0, std::min(100.0, dampingPct)) / 100.0;
    mMix = std::max(0.0, std::min(100.0, mixPct)) / 100.0;

    // Base comb delays at 48 kHz, scaled to the current rate and size.
    static const int kBaseDelays[kNumCombs] = { 250, 311, 353, 397, 449, 503 };
    for (int i = 0; i < kNumCombs; ++i)
    {
      const int d = std::max(1, static_cast<int>(kBaseDelays[i] * (sr / 48000.0) * (0.5 + size)));
      // Per-comb loop gain for the target T60, corrected for N parallel
      // incoherent paths (power adds): g = 1000^(-D/(2*T60)).
      const double Dsec = d / sr;
      double g = std::pow(1000.0, -Dsec / (2.0 * T60));
      mCombs[i].SetDelay(d);
      mCombs[i].mFb = std::min(0.97, g);
    }

    // Damping: one-pole LP cutoff from ~8 kHz (bright) to ~1.5 kHz (dark).
    const double fc = 8000.0 * std::pow(1500.0 / 8000.0, damp);
    const double coef = 1.0 - std::exp(-2.0 * M_PI * fc / sr);
    for (auto& c : mCombs) c.mDampCoef = coef;

    // Allpass delays: small fixed offsets, L/R decorrelated.
    const auto apDelay = [&](int base) { return std::max(1, static_cast<int>(base * sr / 48000.0)); };
    (void)apDelay;
    mApL[0].mG = 0.51; mApL[1].mG = 0.53;
    mApR[0].mG = 0.52; mApR[1].mG = 0.54;
  }

  // Must be called every frame (even with in == 0) so tails keep decaying.
  inline void ProcessStereo(double in, double& outL, double& outR)
  {
    double sum = 0.0;
    for (auto& c : mCombs) sum += c.Process(in);
    sum *= 1.0 / kNumCombs; // level normalization (N parallel combs)

    const double l = mApL[1].Process(mApL[0].Process(sum));
    const double r = mApR[1].Process(mApR[0].Process(sum));
    outL += l * mMix;
    outR += r * mMix;
  }
};

// ---------------------------------------------------------------------------
// Feedback delay, mono in / stereo out (slight L/R offset for width).
// ---------------------------------------------------------------------------
struct DelayUnit
{
  // Mode (Delay-Settings "Mode"-Knob): 0=Mono, 1=Stereo, 2=Ping-Pong, 3=Off.
  enum { kMono = 0, kStereo = 1, kPingPong = 2, kOff = 3 };

  std::vector<double> mBufL, mBufR;
  size_t mIdxL = 0, mIdxR = 0;
  int mDelL = 1, mDelR = 2;
  double mFb = 0.3;
  double mMix = 0.25;
  int mMode = kStereo;

  void Reset()
  {
    std::fill(mBufL.begin(), mBufL.end(), 0.0);
    std::fill(mBufR.begin(), mBufR.end(), 0.0);
    mIdxL = mIdxR = 0;
  }

  void SetParams(double timeMs, double feedbackPct, double mixPct, double sr, int mode)
  {
    const int maxDelay = static_cast<int>(sr * 1.05); // UI max 1000 ms + margin
    if (static_cast<int>(mBufL.size()) < maxDelay)
    {
      mBufL.assign(maxDelay, 0.0);
      mBufR.assign(maxDelay, 0.0);
    }
    const double t = std::max(1.0, std::min(1000.0, timeMs));
    mDelL = std::max(1, static_cast<int>(t * 1e-3 * sr));
    // Only stereo widens R slightly (width); mono/ping-pong keep both taps equal.
    mDelR = (mode == kStereo) ? std::max(mDelL + 1, static_cast<int>((t + 8.0) * 1e-3 * sr)) : mDelL;
    if (mDelR >= maxDelay) mDelR = maxDelay - 1;
    mFb = std::max(0.0, std::min(95.0, feedbackPct)) / 100.0;
    mMix = std::max(0.0, std::min(100.0, mixPct)) / 100.0;
    mMode = mode;
  }

  // Must be called every frame (even when in == 0) so the feedback tail decays.
  inline void ProcessStereo(double in, double& outL, double& outR)
  {
    if (mMode == kOff) return;                 // bypassed: no delay output at all

    const double l = mBufL[mIdxL];
    const double r = mBufR[mIdxR];

    if (mMode == kPingPong)
    {
      // Input only on L; feedback cross-couples so that repeats bounce L<->R.
      mBufL[mIdxL] = in + mFb * r;
      mBufR[mIdxR] =       mFb * l;
    }
    else if (mMode == kMono)
    {
      // Ein geteilter Tap, identisch auf beiden Ausgaengen.
      const double v = in + mFb * l;
      mBufL[mIdxL] = v;
      mBufR[mIdxR] = v;
    }
    else // kStereo: wie bisher (leichter L/R-Offset fuer Breite)
    {
      mBufL[mIdxL] = in + mFb * l;
      mBufR[mIdxR] = in + mFb * r;
    }

    ++mIdxL; if (mIdxL >= mBufL.size()) mIdxL = 0;
    ++mIdxR; if (mIdxR >= mBufR.size()) mIdxR = 0;
    outL += l * mMix;
    outR += r * mMix;
  }
};

// ---------------------------------------------------------------------------
// Per-slot module instances. One instance set PER SLOT so duplicate modules
// in different slots (e.g. EQ in slot 1 and slot 3) keep independent state.
// ---------------------------------------------------------------------------
struct ChainModules
{
  Eq3Band eq[4];
  VcaCompressor comp[4];
  Saturation sat[4];
  TransientShaper ts[4];
  Limiter lim[4];

  void Reset()
  {
    for (int i = 0; i < 4; ++i)
    {
      eq[i].Reset(); comp[i].Reset(); sat[i].Process(0.0); ts[i].Reset(); lim[i].Reset();
    }
  }
};

// ============================================================================
// MixerProcessor — owns all module state (audio thread only).
// ============================================================================
class MixerProcessor
{
public:
  struct Peaks
  {
    double ch[kMixerNumChannels][2] = {{ 0.0, 0.0 }};
    double master[2] = { 0.0, 0.0 };
    double reverb[2] = { 0.0, 0.0 };
    double delay[2] = { 0.0, 0.0 };
    // Peak of the signal as it ENTERS a Lim slot (pre-limiting), per channel
    // and for master. Lets the UI show how far above its ceiling a limiter's
    // input ran — i.e. whether/where it engaged. Channels are mono, so both
    // [i][0] and [i][1] hold the same value.
    double limIn[kMixerNumChannels][2] = {{ 0.0, 0.0 }};
    double masterLimIn[2] = { 0.0, 0.0 };
    // Peak of the signal as it LEAVES a Lim slot (same layout as limIn).
    double limOut[kMixerNumChannels][2] = {{ 0.0, 0.0 }};
    double masterLimOut[2] = { 0.0, 0.0 };
    // Deepest gain reduction applied by a Lim slot, stored as 1 - gain (0 = none), aligned with the
    // limiter's output samples. Same layout as limIn.
    double limGr[kMixerNumChannels][2] = {{ 0.0, 0.0 }};
    double masterLimGr[2] = { 0.0, 0.0 };
    // Loudest audible instrument-channel input after Kit Gain (before EQ/FX/fader): the
    // level the Kit Gain strip meter shows. Mono, so [0] and [1] hold the same value.
    double kitIn[2] = { 0.0, 0.0 };

    void Reset() { *this = Peaks{}; }
  };

  ChainModules mChains[kMixerNumChannels];
  ChainModules mMasterL, mMasterR; // stereo pair: same params, independent state
  ReverbUnit mReverb;
  DelayUnit mDelay;

  void Reset()
  {
    for (auto& c : mChains) c.Reset();
    mMasterL.Reset();
    mMasterR.Reset();
    mReverb.Reset();
    mDelay.Reset();
  }

  // Refresh one chain's module parameters from a slot array (audio thread).
  void UpdateChain(ChainModules& c, const FxSlotState* slots, int nSlots, double sr)
  {
    for (int i = 0; i < nSlots && i < 4; ++i)
    {
      const FxSlotState& sl = slots[i];
      switch (static_cast<FxType>(sl.type))
      {
        case FxType::Eq:
          c.eq[i].SetParams(sl.p[FxParam::EqLowFreqHz], sl.p[FxParam::EqLowGainDb], sl.p[FxParam::EqLowQ],
                            sl.p[FxParam::EqMidFreqHz], sl.p[FxParam::EqMidGainDb], sl.p[FxParam::EqMidQ],
                            sl.p[FxParam::EqHighFreqHz], sl.p[FxParam::EqHighGainDb], sl.p[FxParam::EqHighQ], sr);
          break;
        case FxType::Comp:
          c.comp[i].SetParams(sl.p[FxParam::CompThresholdDb], sl.p[FxParam::CompRatio],
                              sl.p[FxParam::CompAttackMs], sl.p[FxParam::CompReleaseMs],
                              sl.p[FxParam::CompMakeupDb], sr);
          break;
        case FxType::Sat:
          c.sat[i].SetParams(sl.p[FxParam::SatDrivePct]);
          break;
        case FxType::Ts:
          c.ts[i].SetParams(sl.p[FxParam::TsAttackAmt] / 100.0, sl.p[FxParam::TsSustainAmt] / 100.0, sr);
          break;
        case FxType::Lim:
          c.lim[i].SetParams(sl.p[FxParam::LimCeilingDb], sl.p[FxParam::LimAttackMs], sl.p[FxParam::LimReleaseMs], sr);
          break;
        default: break; // Off / bus-only modules
      }
    }
  }

  // If limInPeak is non-null, the largest |x| seen at any Lim slot's INPUT in
  // this chain is written back (max over all Lim slots) — used to report how
  // far above a limiter's ceiling its input ran.
  // limOutPeak likewise receives the largest |x| leaving any Lim slot.
  // limGrPeak receives the largest 1 - gain applied by any Lim slot.
  static double ApplyChain(ChainModules& c, const FxSlotState* slots, int nSlots, double x, double* limInPeak = nullptr,
                           double* limOutPeak = nullptr, double* limGrPeak = nullptr)
  {
    for (int i = 0; i < nSlots && i < 4; ++i)
    {
      const FxSlotState& sl = slots[i];
      if (!sl.on) continue;
      switch (static_cast<FxType>(sl.type))
      {
        case FxType::Eq:   x = c.eq[i].Process(x); break;
        case FxType::Comp: x = c.comp[i].Process(x); break;
        case FxType::Sat:  x = c.sat[i].Process(x); break;
        case FxType::Ts:   x = c.ts[i].Process(x); break;
        case FxType::Lim:
          if (limInPeak) { const double a = std::fabs(x); if (a > *limInPeak) *limInPeak = a; }
          x = c.lim[i].Process(x);
          if (limOutPeak) { const double a = std::fabs(x); if (a > *limOutPeak) *limOutPeak = a; }
          if (limGrPeak) { const double g = 1.0 - c.lim[i].mGain; if (g > *limGrPeak) *limGrPeak = g; }
          break;
        default: break;
      }
    }
    return x;
  }

  // Process one block. chanIn[i] may be nullptr (treated as silence).
  void Process(const double* const* chanIn, double* outL, double* outR, int nFrames,
               double sr, const MixerState& st, Peaks* peaks)
  {
    // --- Parameter refresh (once per block). ---
    for (int i = 0; i < kMixerNumChannels; ++i)
      UpdateChain(mChains[i], st.channels[i].fx, 4, sr);
    UpdateChain(mMasterL, st.master.fx, 4, sr);
    UpdateChain(mMasterR, st.master.fx, 4, sr);

    const FxSlotState& revSlot = st.reverbBus.fx[0];
    mReverb.SetParams(revSlot.p[FxParam::RevSizePct], revSlot.p[FxParam::RevDecaySec],
                      revSlot.p[FxParam::RevDampingPct], revSlot.p[FxParam::RevMixPct], sr);
    const FxSlotState& delSlot = st.delayBus.fx[0];
    mDelay.SetParams(delSlot.p[FxParam::DelTimeMs], delSlot.p[FxParam::DelFeedbackPct],
                     delSlot.p[FxParam::DelMixPct], sr,
                     static_cast<int>(delSlot.p[FxParam::DelMode]));

    // --- Per-channel constants for this block. ---
    const bool anySolo = st.AnySolo();
    const double masterGainLin = std::pow(10.0, st.master.gainDb / 20.0);
    const double kitGainLin = std::pow(10.0, st.kitGainDb / 20.0);
    // Master pan: balance of the stereo image (L<->R). Center = identity,
    // full left/right fades the opposite side to zero. Linear law ->
    // no gain boost, so no extra headroom risk above the master fader.
    const double masterPan   = std::max(-1.0, std::min(1.0, st.master.pan));
    const double masterLgain = (masterPan > 0.0) ? (1.0 - masterPan) : 1.0;
    const double masterRgain = (masterPan < 0.0) ? (1.0 + masterPan) : 1.0;
    const double revBusGainLin = st.reverbBus.mute ? 0.0 : std::pow(10.0, st.reverbBus.gainDb / 20.0);
    const double delBusGainLin = st.delayBus.mute ? 0.0 : std::pow(10.0, st.delayBus.gainDb / 20.0);
    // Bus-Pan: equal-power pan of the Reverb/Delay return (same law as channel
    // pan). Center=identity; full L/R nulls the opposite side. Applied to the wet
    // return only — dry is the direct channel path, so this places the effect in
    // the stereo field rather than changing its level.
    const double revPan  = std::max(-1.0, std::min(1.0, st.reverbBus.pan));
    const double delPan  = std::max(-1.0, std::min(1.0, st.delayBus.pan));
    const double revPanL = std::cos((revPan + 1.0) * 0.25 * M_PI);
    const double revPanR = std::sin((revPan + 1.0) * 0.25 * M_PI);
    const double delPanL = std::cos((delPan + 1.0) * 0.25 * M_PI);
    const double delPanR = std::sin((delPan + 1.0) * 0.25 * M_PI);

    double chGain[kMixerNumChannels], panL[kMixerNumChannels], panR[kMixerNumChannels];
    for (int i = 0; i < kMixerNumChannels; ++i)
    {
      const ChannelState& c = st.channels[i];
      chGain[i] = std::pow(10.0, c.gainDb / 20.0);
      // Equal-power pan: pan -1 -> full L, +1 -> full R, 0 -> -3 dB per side.
      const double pan = std::max(-1.0, std::min(1.0, c.pan));
      const double theta = (pan + 1.0) * 0.25 * M_PI;
      panL[i] = std::cos(theta);
      panR[i] = std::sin(theta);
    }

    // --- Per-frame processing. ---
    for (int s = 0; s < nFrames; ++s)
    {
      double sumL = 0.0, sumR = 0.0, revIn = 0.0, delIn = 0.0;

      for (int i = 0; i < kMixerNumChannels; ++i)
      {
        const ChannelState& c = st.channels[i];
        double x = chanIn[i] ? chanIn[i][s] * kitGainLin : 0.0;
        if (c.mute || (anySolo && !c.solo))
        {
          x = 0.0;
        }
        else if (x != 0.0)
        {
          if (peaks)
          {
            const double ax = std::fabs(x);
            if (ax > peaks->kitIn[0]) { peaks->kitIn[0] = ax; peaks->kitIn[1] = ax; }
          }
          double chLimIn = 0.0, chLimOut = 0.0, chLimGr = 0.0;
          x = ApplyChain(mChains[i], c.fx, 4, x, &chLimIn, &chLimOut, &chLimGr);
          x *= chGain[i];
          if (peaks && chLimIn > 0.0)
          {
            if (chLimIn > peaks->limIn[i][0]) peaks->limIn[i][0] = chLimIn;
            if (chLimIn > peaks->limIn[i][1]) peaks->limIn[i][1] = chLimIn;
            if (chLimOut > peaks->limOut[i][0]) peaks->limOut[i][0] = chLimOut;
            if (chLimOut > peaks->limOut[i][1]) peaks->limOut[i][1] = chLimOut;
            if (chLimGr > peaks->limGr[i][0]) { peaks->limGr[i][0] = chLimGr; peaks->limGr[i][1] = chLimGr; }
          }
        }

        const double xl = x * panL[i];
        const double xr = x * panR[i];
        sumL += xl;
        sumR += xr;
        revIn += x * c.sendReverb;
        delIn += x * c.sendDelay;

        if (peaks)
        {
          const double al = std::fabs(xl), ar = std::fabs(xr);
          if (al > peaks->ch[i][0]) peaks->ch[i][0] = al;
          if (ar > peaks->ch[i][1]) peaks->ch[i][1] = ar;
        }
      }

      // Buses: always processed so tails decay even without new input.
      double revL = 0.0, revR = 0.0, delL = 0.0, delR = 0.0;
      mReverb.ProcessStereo(revIn, revL, revR);
      mDelay.ProcessStereo(delIn, delL, delR);
      revL *= revBusGainLin * revPanL; revR *= revBusGainLin * revPanR;
      delL *= delBusGainLin * delPanL; delR *= delBusGainLin * delPanR;

      double mL = sumL + revL + delL;
      double mR = sumR + revR + delR;

      if (!st.master.mute)
      {
        double limInL = 0.0, limInR = 0.0, limOutL = 0.0, limOutR = 0.0, limGrL = 0.0, limGrR = 0.0;
        // The master fader comes first and works as the input level of the chain, so a Limiter in the last slot
        // keeps its ceiling at any fader position. The slot order is the user's (effect chain), nothing is reordered.
        mL *= masterGainLin;
        mR *= masterGainLin;
        mL = ApplyChain(mMasterL, st.master.fx, 4, mL, &limInL, &limOutL, &limGrL);
        mR = ApplyChain(mMasterR, st.master.fx, 4, mR, &limInR, &limOutR, &limGrR);
        if (peaks)
        {
          if (limInL > peaks->masterLimIn[0]) peaks->masterLimIn[0] = limInL;
          if (limInR > peaks->masterLimIn[1]) peaks->masterLimIn[1] = limInR;
          if (limOutL > peaks->masterLimOut[0]) peaks->masterLimOut[0] = limOutL;
          if (limOutR > peaks->masterLimOut[1]) peaks->masterLimOut[1] = limOutR;
          if (limGrL > peaks->masterLimGr[0]) peaks->masterLimGr[0] = limGrL;
          if (limGrR > peaks->masterLimGr[1]) peaks->masterLimGr[1] = limGrR;
        }
        // Master-Pan ganz am Ende auf das fertige Stereo-Bild anwenden.
        mL *= masterLgain;
        mR *= masterRgain;
      }
      else
      {
        mL = 0.0; mR = 0.0;
      }

      outL[s] = mL;
      outR[s] = mR;

      if (peaks)
      {
        const double al = std::fabs(mL), ar = std::fabs(mR);
        if (al > peaks->master[0]) peaks->master[0] = al;
        if (ar > peaks->master[1]) peaks->master[1] = ar;
        const double rl = std::fabs(revL), rr = std::fabs(revR);
        if (rl > peaks->reverb[0]) peaks->reverb[0] = rl;
        if (rr > peaks->reverb[1]) peaks->reverb[1] = rr;
        const double dl = std::fabs(delL), dr = std::fabs(delR);
        if (dl > peaks->delay[0]) peaks->delay[0] = dl;
        if (dr > peaks->delay[1]) peaks->delay[1] = dr;
      }
    }
  }
};

} // namespace dgg
