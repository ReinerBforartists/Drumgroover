// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Reiner Prokein
#pragma once
// =========================================
// HiHat808Voice -- cache playback; the fast model (808HhFast.h) is the baker.
//
// The open and the closed hat of the HH sheet are played from the variant pool (baked renders, one buffer per (pool, decay, level, pitch) key, several random bank phases per key).
// When no buffer exists (a knob was just moved), the voice synthesises the hit itself with the fast model (oscillator bank + BP2 exact, envelope generator, reduced Newton core, linear output stage);
// a plugin voice (cacheOnly) additionally reports the miss so the key gets baked. That is also the path the baker (DrumGroover.cpp::EnqueueHiHatSet) renders with, and what the diagnostic tools hear.
//
// Velocity is a linear gain on the (full-accent) render, as in the cache files.
// =========================================

#include "DrumVoice.h"
#include "808HhFast.h"
#include "808VariantPools.h"
#include "808CacheMiss.h"
#include "BiquadFilter.h"
#include <cmath>
#include <algorithm>
#include <vector>
#include <memory>
#include <cstdint>
#include <cstdio>
#include <string>
#include "808MixBus.h"

namespace dgg {

class HiHat808Voice final : public DrumVoice
{
public:
  // Model volt -> sample gain of the metal path: the common jack factor with the mixing weight of the OH / CH channel (15k / 8.2k).
  static constexpr double kVoltToSample = dgg::kJackVoltToSample * dgg::kMixWeightWide;
  enum class Articulation { Metal, Pedal };
  enum class CircuitFamily { Open, Closed };
  CircuitFamily circuitFamily = CircuitFamily::Open;

  // VR7/VR8 LEVEL knobs, 0..1 (real rheostat of the output stage over 100 kOhm). One knob per family (the OH LEVEL / CH LEVEL pots).
  double levelKnob01 = 0.5; // the pot at its reference position
  // VR3 -- real OH decay knob, 0..1. No effect on Closed (real hardware has no Closed decay knob either).
  double decayKnob01 = 0.5;
  // Purely digital transposition of the six bank oscillators, shared with Ride/Crash. 1.0 = unchanged.
  double pitchRatio = 1.0;

  // Pedal: a filtered-noise chick, not part of the HH sheet
  double pedalFreqHz = 900.0, pedalQ = 1.5, pedalDecaySec = 0.05, pedalLevel = 0.4;

  // Variant pool: highest priority in Trigger().
  bool diagUseVariantPool = false;
  // Plugin playback voices: a miss is counted (dgg::CacheMissSeq()) so the key gets baked; the hit itself is synthesised live.
  bool cacheOnly = false;
  // The render of the hit (unscaled, DC blocked) is appended here while the voice synthesises (the baker's tap).
  std::vector<float>* tapVariantBake = nullptr;

  // Which of the 6 HiHat pools (ClosedEdge/TightEdge/OpenEdge0/OpenEdge4/ClosedTip/TightTip) this voice belongs to -- part of the cache key.
  int variantPoolArticulationId = 0;

  HiHat808Voice()
  {
    static uint32_t sSeed = 0x2545F491u; sSeed = sSeed * 1664525u + 1013904223u; mRng = sSeed | 1u;   // every voice has its own bank phases
  }

  // The bank of a baked render has been running this long before the hit: every variant of a key gets its own bank phase (the baker passes a different value per variant).
  void WarmupFreeRunningOscillatorsForBake(double seconds) { mWarmupSec = seconds; }

  // pitchRatio is part of THIS key: the variant pool never blends between keys. articulationId: the pool index (0-5), so pools of the same family and equal knobs still bake independently.
  static dgg::TR808VariantKey128 MakeVariantPoolKey(int familyKey, int articulationId, double decay01, double levelKnob01, double pitchRatio, double sampleRateHz)
  {
    dgg::TR808VariantKey128 key;
    uint64_t lo = (uint64_t)(familyKey & 0xF);
    lo = (lo << 4) | (uint64_t)(articulationId & 0xF);
    lo = (lo << 12) | QuantizeVariantKnob01(decay01);
    lo = (lo << 12) | QuantizeVariantKnob01(levelKnob01);
    lo = (lo << 12) | QuantizeVariantPitch(pitchRatio);
    key.lo = lo;
    key.hi = (uint64_t)std::llround(sampleRateHz);
    return key;
  }

  // The quantized settings a key stands for -- the exact inverse of MakeVariantPoolKey().
  struct VariantPoolParams { int familyKey, poolIndex; double decay, level, pitch, sampleRateHz; };
  static VariantPoolParams DecodeVariantPoolKey(const dgg::TR808VariantKey128& key)
  {
    VariantPoolParams p;
    p.pitch = dgg::DequantizeVariant(key.lo & 0xFFF);
    p.level = dgg::DequantizeVariant((key.lo >> 12) & 0xFFF);
    p.decay = dgg::DequantizeVariant((key.lo >> 24) & 0xFFF);
    p.poolIndex = (int)((key.lo >> 36) & 0xF);
    p.familyKey = (int)((key.lo >> 40) & 0xF);
    p.sampleRateHz = (double)key.hi;
    return p;
  }

  // Cache file base name for a key, e.g. "HiHat_OpenEdge0_decay85_level100_pitch100_sr44100_m5" (knobs in hundredths).
  // kModelVersion is part of the file name: changing the model invalidates existing cache files.
  static constexpr int kModelVersion = 5;
  static std::string VariantPoolBaseName(const dgg::TR808VariantKey128& key)
  {
    static const char* kPoolNames[6] = { "ClosedEdge", "TightEdge", "OpenEdge0", "OpenEdge4", "ClosedTip", "TightTip" };
    const VariantPoolParams p = DecodeVariantPoolKey(key);
    char buf[128];
    std::snprintf(buf, sizeof(buf), "HiHat_%s_decay%d_level%d_pitch%d_sr%d_m%d",
      kPoolNames[std::clamp(p.poolIndex, 0, 5)], (int)std::llround(p.decay * 100.0),
      (int)std::llround(p.level * 100.0), (int)std::llround(p.pitch * 100.0), (int)std::llround(p.sampleRateHz), kModelVersion);
    return buf;
  }

  // family as int for the cache / library keys (0=Open, 1=Closed)
  static int FamilyKey(CircuitFamily f) { return f == CircuitFamily::Open ? 0 : 1; }

  // Allocates the fast-model chains (about 100 kB) so the first live hit does no allocation on the audio thread; call from the init thread after SetSampleRate().
  void PrepareLive() { EnsureChains(); }

  void SetSampleRate(double sr) override
  {
    const bool hadChains = (bool)mOpen;
    DrumVoice::SetSampleRate(sr);
    mOpen.reset(); mClosed.reset();   // rebuilt for the new rate
    if (hadChains) EnsureChains();
  }

  void Trigger(double velocity01) override { Trigger(velocity01, Articulation::Metal); }

  void Trigger(double velocity01, Articulation art)
  {
    mVelocity = velocity01;
    mActive = true;
    mArt = art;

    if (art == Articulation::Pedal)
    {
      mPedalFilter.SetBandpass(pedalFreqHz, pedalQ, mSampleRate);
      mPedalFilter.Reset();
      mElapsed = 0.0;
      return;
    }

    mQuietStreakSamples = 0; mPeakSinceTrigger = 0.0;
    mCachePlayback.reset();
    mCachePlaybackPos = 0.0;
    mCachePlaybackIsPrePitched = false;
    mSynthesising = false; mChoking = false; mChokeGain = 1.0;
    // Variant pool first: each variant is baked at its own real pitch, so no resampling at playback.
    if (diagUseVariantPool)
    {
      const dgg::TR808VariantKey128 variantKey = MakeVariantPoolKey(FamilyKey(circuitFamily), variantPoolArticulationId, decayKnob01, levelKnob01, pitchRatio, mSampleRate);
      mCachePlayback = dgg::TR808HiHatVariantPool::Instance().GetRandom(variantKey);
      if (mCachePlayback) mCachePlaybackIsPrePitched = true;
    }
    // A miss is reported (the plugin bakes the key); this hit is synthesised live with the fast model.
    if (cacheOnly && !mCachePlayback) dgg::CacheMissSeq().fetch_add(1, std::memory_order_relaxed);
    if (!mCachePlayback) StartSynthesis();
  }

  void Process(double* out, int nFrames) override
  {
    if (!mActive) return;
    const double dt = 1.0 / mSampleRate;

    if (mArt == Articulation::Pedal)
    {
      for (int i = 0; i < nFrames; i++)
      {
        if (mElapsed > pedalDecaySec) { mActive = false; break; }
        const double amp = ExpRamp(pedalLevel, 0.001, mElapsed, pedalDecaySec);
        out[i] += mPedalFilter.Process(mNoise.Next()) * amp * mVelocity;
        mElapsed += dt;
      }
      return;
    }

    for (int i = 0; i < nFrames; i++)
    {
      double acOut;
      if (mCachePlayback)
      {
        // Pitch resampling: reference-library buffers are baked at pitch 1.0, the read head runs with pitchRatio; variant buffers are already at the real pitch (1:1).
        const size_t i0 = (size_t)mCachePlaybackPos;
        if (i0 >= mCachePlayback->size()) { mActive = false; break; }
        const double frac = mCachePlaybackPos - (double)i0;
        const double s0 = (double)(*mCachePlayback)[i0];
        const size_t i1 = i0 + 1;
        const double s1 = (i1 < mCachePlayback->size()) ? (double)(*mCachePlayback)[i1] : 0.0;
        acOut = s0 + frac * (s1 - s0);
        if (mChoking)
        {
          if (mChokeHoldSamples > 0) --mChokeHoldSamples; else { mChokeGain *= mChokeStep; if (mChokeGain < kChokeFloor) { mActive = false; break; } }
          acOut *= mChokeGain;
        }
        mCachePlaybackPos += mCachePlaybackIsPrePitched ? 1.0 : pitchRatio;
      }
      else
      {
        acOut = SynthStep();
        // UNSCALED (before the velocity gain) so the render serves every velocity
        if (tapVariantBake) tapVariantBake->push_back((float)acOut);
      }

      out[i] += acOut * kVoltToSample * mVelocity;
      mElapsed += dt;

      if (!SilenceEarlyExitCheck(acOut)) continue;
      break;
    }
  }

  bool IsActive() const override { return mActive; }

  // Cross-voice choke (VoicePool::ChokeAll(), a closed-hat note triggers). A hit that is synthesised right now takes it into the envelope network. A ringing cache buffer is faded out: the fast model
  // (OH hit, CH hit on top) gives a level that falls like exp(-t / 115 ms) after a delay of about 3 ms (gain 0.89 at 15 ms, 0.65 at 50 ms, 0.46 at 90 ms, 0.21 at 175 ms), independent of the decay pot.
  void Choke()
  {
    if (circuitFamily != CircuitFamily::Open || mArt != Articulation::Metal || !mActive) return;
    if (mSynthesising) { if (mOpen) mOpen->Choke(kFullAccentVel); return; }
    if (mChoking) return;
    mChoking = true; mChokeHoldSamples = (long)std::llround(kChokeDelaySec * mSampleRate); mChokeGain = 1.0; mChokeStep = std::exp(-1.0 / (kChokeTauSec * mSampleRate));
  }

private:
  static constexpr int kFullAccentVel = 127;
  static constexpr double kChokeTauSec = 0.115, kChokeDelaySec = 0.003, kChokeFloor = 0.003;   // fade of a choked cache buffer; below kChokeFloor (-50 dB) the voice ends
  static constexpr double kPrerollSec = 0.4;                       // the BP2 runs this long from rest before the hit
  static constexpr double kDcBlockR = 0.9995;                       // DC blocker (the rectified signal shifts the coupling capacitors), ~4 Hz corner
  static constexpr double kSilenceFloorRel = 1.0e-4;
  static constexpr double kSilenceFloorAbs = 1.0e-5;
  static constexpr double kSilenceHoldSec = 0.15;

  // The chains exist only for a voice that synthesises (bake voices, live hits on a cache miss).
  void EnsureChains()
  {
    if (mOpen) return;
    mOpen = std::make_unique<hhfast::HhChain<true>>(); mOpen->Configure(mSampleRate);
    mClosed = std::make_unique<hhfast::HhChain<false>>(); mClosed->Configure(mSampleRate);
  }

  // Bank offset of a synthesised hit: the baker's warm-up value, or a pseudo-random phase for a hit synthesised outside the baker.
  void StartSynthesis()
  {
    mSynthesising = true;
    EnsureChains();
    double extra = mWarmupSec;
    if (extra <= 0.0) { mRng ^= mRng << 13; mRng ^= mRng >> 17; mRng ^= mRng << 5; extra = 2.0 * (double)mRng / 4294967296.0; }
    const double level = std::min(1.0, std::max(0.0, levelKnob01)), decay = std::min(1.0, std::max(0.0, decayKnob01));
    const double offset = kPrerollSec + extra;
    if (circuitFamily == CircuitFamily::Open) { mOpen->SetLevel(level); mOpen->Trigger(kFullAccentVel, decay, pitchRatio, offset, kPrerollSec); }
    else { mClosed->SetLevel(level); mClosed->Trigger(kFullAccentVel, 0.5, pitchRatio, offset, kPrerollSec); }
    mOutX1 = mOutY1 = 0.0;
  }

  double SynthStep()
  {
    const double raw = circuitFamily == CircuitFamily::Open ? mOpen->Process() : mClosed->Process();
    const double ac = raw - mOutX1 + kDcBlockR * mOutY1; mOutX1 = raw; mOutY1 = ac;
    if (!(circuitFamily == CircuitFamily::Open ? mOpen->Active() : mClosed->Active())) mActive = false;
    return ac;
  }

  // Silence cutoff: a purely relative floor degenerates once the peak itself is at the numerical noise floor, so the larger of a relative and an absolute floor counts.
  bool SilenceEarlyExitCheck(double outSample)
  {
    mPeakSinceTrigger = std::max(mPeakSinceTrigger, std::abs(outSample));
    const double floor = std::max(mPeakSinceTrigger * kSilenceFloorRel, kSilenceFloorAbs);
    if (std::abs(outSample) >= floor) { mQuietStreakSamples = 0; return false; }
    if (++mQuietStreakSamples < (int)(kSilenceHoldSec * mSampleRate)) return false;
    mActive = false;
    return true;
  }

  std::shared_ptr<const std::vector<float>> mCachePlayback;
  double mCachePlaybackPos = 0.0;
  bool mCachePlaybackIsPrePitched = false;
  bool mSynthesising = false;
  bool mChoking = false; long mChokeHoldSamples = 0; double mChokeGain = 1.0, mChokeStep = 1.0;

  std::unique_ptr<hhfast::HhChain<true>> mOpen;
  std::unique_ptr<hhfast::HhChain<false>> mClosed;
  double mOutX1 = 0.0, mOutY1 = 0.0, mWarmupSec = 0.0;
  uint32_t mRng = 0x2545F491u;
  int mQuietStreakSamples = 0;
  double mPeakSinceTrigger = 0.0;

  BiquadFilter mPedalFilter;
  WhiteNoise mNoise;
  Articulation mArt = Articulation::Metal;
  double mElapsed = 0.0;
  double mVelocity = 0.0;
  bool mActive = false;
};

}  // namespace dgg
