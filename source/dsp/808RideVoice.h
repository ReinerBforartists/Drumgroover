#pragma once
// TR808RideVoice -- the engine of the Ride and Cymbal/Crash instruments. A hit is played from the variant pool (808VariantPoolCache.h) when a baked
// set exists; otherwise (and always while baking) it is synthesised live with the fast model of 808CyFast.h: metal bank and
// BP1/BP2 as exact modes, the rest of the circuit as one reduced-Newton network.
//
// Articulation::Crash is the Cymbal hit; Articulation::Mute is shared with Ride's own mute pad (the same "choke whatever is ringing" behaviour).
// DrumGroover.h/.cpp use this one class for both instruments' voice pools.

#include "DrumVoice.h"
#include "808VariantPools.h"
#include "808CacheMiss.h"
#include "808CyFast.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace dgg {

class TR808RideVoice final : public DrumVoice
{
public:
  enum class Articulation { BowTip, BowShank, BellTip, BellShank, Edge, Mute, Crash };

  // Tone stage: toneKnob = VR4 position, 0..1 (1 = brighter). levelKnob = VR6 output-level pot wiper, 0..1.
  double toneKnob = 0.5;
  double levelKnob = 0.5;

  // Decay knob, 0..1 (VR2). Ride's class default is 0.85 (longer decay), Cymbal's 0.5.
  double decayKnob01 = 0.85;

  // Transposes all six metal-bank oscillators together. A variant-pool hit is baked at its own pitch and played 1:1.
  double pitchRatio = 1.0;

  // VCA bias resistors of the circuit (R102+R103, R99+R100, R96+R97); stored and reported as voice parameters.
  double vca1BiasOhm = 2.0e6;
  double vca2BiasOhm = 2.0e6;
  double vca3BiasOhm = 2.0e6;

  double level = 0.53;

  // Variant pool (808VariantPoolCache.h): final playback, highest priority in Trigger(). Switched on by DrumGroover.cpp for the real voice pools.
  bool diagUseVariantPool = false;
  // Plugin playback voices: a hit without a baked variant is reported via dgg::CacheMissSeq() (the plugin bakes the key) and synthesised live with the
  // fast model. Bake voices and diagnostic tools leave this false.
  bool cacheOnly = false;
  // Core steps per host sample of the fast model (1, 2, 3, 4, 6, 8; 0 = automatic: the core runs at about 192 kHz, see cyfast::CyChain::CoreStepsFor()).
  int fastCoreSteps = 0;
  // The quiet tail of a hit runs with half the core steps (808CyFast.h, CyChain::adaptiveTail). false: the normal steps throughout.
  bool fastAdaptiveTail = true;
  // Set by the baker: collects the raw model output of ONE Trigger() run (before DC-blocker, choke, plateau fade, level and velocity).
  std::vector<float>* tapVariantBake = nullptr;

  // Bake-only: advances the free-running metal bank by `seconds` before the next Trigger(), so that several variants baked in parallel on separate
  // voice instances do not start at the same oscillator phase. Call after SetSampleRate(), before Trigger().
  void WarmupFreeRunningOscillatorsForBake(double seconds) { mFastBankWarmupSec += seconds; }

  // Variant pool key. pitchRatio is part of it (see 808VariantPools.h::TR808VariantKey128), and so is the articulation: every articulation gets its
  // own baked set.
  static dgg::TR808VariantKey128 MakeVariantPoolKey(int art, double decay01, double tone01, double levelKnob01, double pitchRatio, double sampleRateHz)
  {
    dgg::TR808VariantKey128 key;
    uint64_t lo = (uint64_t)(art & 0xF);
    lo = (lo << 12) | QuantizeVariantKnob01(decay01);
    lo = (lo << 12) | QuantizeVariantKnob01(tone01);
    lo = (lo << 12) | QuantizeVariantKnob01(levelKnob01);
    lo = (lo << 12) | QuantizeVariantPitch(pitchRatio);
    key.lo = lo;
    key.hi = (uint64_t)std::llround(sampleRateHz);
    return key;
  }

  // The quantized settings a key stands for -- the exact inverse of MakeVariantPoolKey(). A bake renders these (not the raw knob values),
  // so a cached set is a pure function of its key.
  struct VariantPoolParams { int art; double decay, tone, level, pitch, sampleRateHz; };
  static VariantPoolParams DecodeVariantPoolKey(const dgg::TR808VariantKey128& key)
  {
    VariantPoolParams p;
    p.pitch = dgg::DequantizeVariant(key.lo & 0xFFF);
    p.level = dgg::DequantizeVariant((key.lo >> 12) & 0xFFF);
    p.tone  = dgg::DequantizeVariant((key.lo >> 24) & 0xFFF);
    p.decay = dgg::DequantizeVariant((key.lo >> 36) & 0xFFF);
    p.art   = (int)((key.lo >> 48) & 0xF);
    p.sampleRateHz = (double)key.hi;
    return p;
  }

  // Cache file base name for a key, e.g. "Ride_BowTip_decay65_tone15_level60_pitch100_sr44100_m9" (knobs in hundredths).
  // Crash is the cymbal; every other articulation is Ride.
  static constexpr int kModelVersion = 9;   // 9: the live synthesis is the fast model (808CyFast.h)
  static std::string VariantPoolBaseName(const dgg::TR808VariantKey128& key)
  {
    static const char* kArtNames[7] = { "Ride_BowTip", "Ride_BowShank", "Ride_BellTip", "Ride_BellShank", "Ride_Edge", "Ride_Mute", "Cymbal_Crash" };
    const VariantPoolParams p = DecodeVariantPoolKey(key);
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s_decay%d_tone%d_level%d_pitch%d_sr%d_m%d",
      kArtNames[std::clamp(p.art, 0, 6)], (int)std::llround(p.decay * 100.0), (int)std::llround(p.tone * 100.0),
      (int)std::llround(p.level * 100.0), (int)std::llround(p.pitch * 100.0), (int)std::llround(p.sampleRateHz), kModelVersion);
    return buf;
  }

  void SetSampleRate(double sr) override
  {
    DrumVoice::SetSampleRate(sr);
    // Pole of the recording-chain DC-blocker: tau = 15 ms (C 15u, R 1k).
    static constexpr double kDcBlockTauSec = 15.0e-3;
    mDcBlockAlpha = std::exp(-(1.0 / mSampleRate) / kDcBlockTauSec);
  }

  void Trigger(double velocity01) override { Trigger(velocity01, Articulation::BowTip); }

  void Trigger(double velocity01, Articulation art)
  {
    mVelocity = velocity01;
    mElapsed = 0.0;
    mActive = true;
    mChokeStartSec = 0.0;
    mQuietStreakSamples = 0;
    mPeakSinceTrigger = 0.0;
    // No automatic fade: the note length comes from the circuit's own decay plus the relative silence cut-off. The choke fade is only used by Choke().
    mEnvelopeChokeSec = kNoAutoChokeSec;
    mChokeExpRate = 0.0;
    // The DC-blocker starts "discharged": the first sample is its reference point.
    mDcBlockPrimed = false;
    mDcBlockXPrev = 0.0;
    mDcBlockY = 0.0;

    mCachePlayback.reset();
    mCachePlaybackPos = 0;
    mDebugVariantPoolIndex = -1;
    mDebugVariantPoolSize = 0;
    if (diagUseVariantPool)
    {
      const dgg::TR808VariantKey128 variantKey = MakeVariantPoolKey(static_cast<int>(art), decayKnob01, toneKnob, levelKnob, pitchRatio, mSampleRate);
      mCachePlayback = dgg::TR808RideVariantPool::Instance().GetRandomIndexed(variantKey, mDebugVariantPoolIndex, &mDebugVariantPoolSize);
    }
    // A miss on a plugin voice is reported (the plugin bakes the key); this hit is synthesised live with the fast model below.
    if (cacheOnly && !mCachePlayback) dgg::CacheMissSeq().fetch_add(1, std::memory_order_relaxed);
    mFastActive = false;
    if (!mCachePlayback)
    {
      PrepareLive();
      mFast.p->adaptiveTail = fastAdaptiveTail;
      mFast.p->Prepare(mSampleRate, fastCoreSteps > 0 ? fastCoreSteps : dgg::cyfast::CyChain::CoreStepsFor(mSampleRate), decayKnob01, toneKnob, Vr6Fraction(levelKnob));
      mFast.p->Trigger(velocity01 * 4.09, pitchRatio, kFastBankOffsetSec + mFastBankWarmupSec, kFastPrerollSec);   // accent = velocity / 127 * 4.09 V
      mFastActive = true;
    }
  }

  // External physical-damping choke (e.g. a Mute hit choking a still-ringing articulation of the same pool): a fixed fast fade, independent of the
  // decay knob of the ring being choked.
  void Choke()
  {
    if (!mActive) return;
    mChokeStartSec = mElapsed;
    mEnvelopeChokeSec = kMuteChokeFadeSec;
    mChokeExpRate = (mEnvelopeChokeSec > 0.0) ? (std::log(0.001) / mEnvelopeChokeSec) : 0.0;
  }

  void Process(double* out, int nFrames) override
  {
    if (!mActive) return;
    const double dt = 1.0 / mSampleRate;
    const double totalDecay = mChokeStartSec + mEnvelopeChokeSec;
    const double tau1_1 = (kR93Ohm + kVr2MaxOhm * decayKnob01) * kC41Farad;

    for (int i = 0; i < nFrames; i++)
    {
      if (mElapsed > totalDecay) { mActive = false; break; }

      double mixedNew;
      if (mCachePlayback)
      {
        // Variant-pool buffers are baked at the real pitch: read 1:1.
        if (mCachePlaybackPos >= mCachePlayback->size()) { mActive = false; break; }
        mixedNew = (double)(*mCachePlayback)[mCachePlaybackPos++];
      }
      else
      {
        mixedNew = mFast.p->Process() * kFastOutScale;
        if (tapVariantBake) tapVariantBake->push_back((float)mixedNew);
      }

      // Recording-chain DC-blocker (y[n] = x[n] - x[n-1] + alpha * y[n-1]); cached and live output are treated alike.
      if (!mDcBlockPrimed) { mDcBlockXPrev = mixedNew; mDcBlockY = 0.0; mDcBlockPrimed = true; }
      mDcBlockY = (mixedNew - mDcBlockXPrev) + mDcBlockAlpha * mDcBlockY;
      mDcBlockXPrev = mixedNew;
      mixedNew = mDcBlockY;

      if (mEnvelopeChokeSec <= 0.0) { mixedNew *= 0.001; }
      else
      {
        const double tt = std::clamp(mElapsed - mChokeStartSec, 0.0, mEnvelopeChokeSec);
        mixedNew *= std::exp(mChokeExpRate * tt);
      }

      const double fadeStart = kPlateauFadeStartTauMult * tau1_1;
      if (mElapsed >= fadeStart)
      {
        const double x = std::min(1.0, (mElapsed - fadeStart) / kPlateauFadeDurSec);
        mixedNew *= 0.5 * (1.0 + std::cos(3.141592653589793 * x));
      }

      const double outSampleNew = mixedNew * level * mVelocity;
      out[i] += std::max(-1.0, std::min(1.0, outSampleNew));
      mElapsed += dt;
      if (SilenceEarlyExitCheck(outSampleNew)) break;
    }
  }

  bool IsActive() const override { return mActive; }

  // Diagnostics: which variant-pool slot this Trigger() used (-1 = pool miss/not used) and how many variants the pool held.
  int DebugVariantPoolIndex() const { return mDebugVariantPoolIndex; }
  int DebugVariantPoolSize() const { return mDebugVariantPoolSize; }

  // Allocates the fast model so the first live hit (a cache miss) does no allocation on the audio thread; call from the init thread.
  void PrepareLive() { if (!mFast.p) mFast.p.reset(new dgg::cyfast::CyChain()); }

private:
  static constexpr double kMuteChokeFadeSec = 0.04;   // fast fade of an external choke
  static constexpr double kNoAutoChokeSec = 1.0e9;
  // Plateau fade: starts at kPlateauFadeStartTauMult * tau1_1 (EG#1.1 discharge path R93 + VR2(k), C41) and runs for kPlateauFadeDurSec.
  static constexpr double kPlateauFadeStartTauMult = 1.3;
  static constexpr double kPlateauFadeDurSec = 0.4;
  static constexpr double kR93Ohm = 470000.0, kVr2MaxOhm = 2000000.0;
  static constexpr double kC41Farad = 1.2e-6;   // calibrated to the Yocto2 recordings (schematic 1u)
  // Once the output has stayed 80 dB below this hit's own peak for kSilenceHoldSec, nothing audible can still happen.
  static constexpr double kSilenceFloorRel = 1.0e-4;
  static constexpr double kSilenceHoldSec = 0.15;
  static constexpr double kFastOutScale = 0.18;
  static constexpr double kFastBankOffsetSec = 0.4, kFastPrerollSec = 0.03;   // bank time at the trigger (+ the bake warm-up), BP1/BP2 run from rest this long before it

  // Resistance fraction of VR6 at knob position 0..1: JIS.A "15A" curve, 11 points (0/10/.../100 % rotation), piecewise linear.
  static double Vr6Fraction(double knob01)
  {
    static constexpr double kPts[11] = {0.0, 0.01, 0.025, 0.055, 0.105, 0.16, 0.26, 0.40, 0.65, 0.93, 1.0};
    const double x = std::min(1.0, std::max(0.0, knob01)) * 10.0;
    const int lo = std::min(9, (int)x);
    return kPts[lo] + (x - (double)lo) * (kPts[lo + 1] - kPts[lo]);
  }

  // Returns true once the caller should stop (mActive already cleared).
  bool SilenceEarlyExitCheck(double outSample)
  {
    mPeakSinceTrigger = std::max(mPeakSinceTrigger, std::abs(outSample));
    const double floor = mPeakSinceTrigger * kSilenceFloorRel;
    if (std::abs(outSample) >= floor) { mQuietStreakSamples = 0; return false; }
    if (++mQuietStreakSamples < (int)(kSilenceHoldSec * mSampleRate)) return false;
    mActive = false;
    return true;
  }

  // The fast model: created by PrepareLive() (plugin voices) or on the first hit that synthesises. A copy of the voice copies the model too.
  struct FastHolder
  {
    std::unique_ptr<dgg::cyfast::CyChain> p;
    FastHolder() = default;
    FastHolder(const FastHolder& o) : p(o.p ? new dgg::cyfast::CyChain(*o.p) : nullptr) {}
    FastHolder& operator=(const FastHolder& o) { p.reset(o.p ? new dgg::cyfast::CyChain(*o.p) : nullptr); return *this; }
  };
  FastHolder mFast;
  bool mFastActive = false;
  double mFastBankWarmupSec = 0.0;

  int mDebugVariantPoolIndex = -1;
  int mDebugVariantPoolSize = 0;
  double mElapsed = 0.0;
  double mVelocity = 0.0;
  bool mActive = false;
  double mChokeStartSec = 0.0;
  // Choke fade in effect for the current hit and its precomputed exponential rate (log(0.001) / duration).
  double mEnvelopeChokeSec = kNoAutoChokeSec;
  double mChokeExpRate = 0.0;
  int mQuietStreakSamples = 0;
  double mPeakSinceTrigger = 0.0;
  // DC-blocker state.
  double mDcBlockAlpha = 0.0;
  double mDcBlockXPrev = 0.0;
  double mDcBlockY = 0.0;
  bool mDcBlockPrimed = false;
  // The variant-pool recording being played, and the read position.
  std::shared_ptr<const std::vector<float>> mCachePlayback;
  size_t mCachePlaybackPos = 0;
};

} // namespace dgg
