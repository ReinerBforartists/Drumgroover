#pragma once
// TR-808 / Yocto cowbell (CB sheet). The sound comes from the fast model in 808CbFast.h (derived from the CB sheet of the common NGSpice circuit). The voice synthesises
// every hit directly on the audio thread.
//
// Nothing here is a synthesizer parameter: the level is the pot VR5. The circuit has no tuning or decay control; "pitch" is a digital transpose (the model output is read at
// pitchRatio samples per output sample, linear interpolation). The velocity does not go to the accent stage: its transistor gate switches at about 3.3 V (MIDI velocity ~102),
// so below that the circuit is silent and above it the level saturates within 1.4 dB. Every hit therefore runs with the full accent voltage and the velocity scales the amplitude
// linearly (v/127).
//
// The cowbell gates the two free-running oscillators TP7 (gate E) and TP6 (gate F) of the CY oscillator bank, so the sound depends on the phase of both oscillators at the trigger:
// every hit draws a random phase (both periods are below 6 ms: all relative phases occur).

#include "DrumVoice.h"
#include "808CbFast.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include "808MixBus.h"

namespace dgg {

class TR808CbVoice final : public DrumVoice
{
public:
  // Knob position 0..1 as the UI shows it (VR5, taper A).
  double levelKnob = 0.5;
  // Digital transpose, not a schematic knob.
  double pitchRatio = 1.0;
  // Jack voltage -> sample: the common jack factor (peak up to kPeakVoltWorstCase at level 1).
  static constexpr double kPeakVoltWorstCase = 1.47;
  static constexpr double kVoltToSample = dgg::kJackVoltToSample;
  static constexpr double kAccentFullScaleVolt = cbfast::TR808CbFast::kAccentFullScaleVolt;
  // The gate phase at the trigger is drawn within kPhaseSpanSec.
  static constexpr double kPhaseSpanSec = 1.0;
  // A hit ends when the 10 ms RMS of the output has stayed below kHitSilenceVolt for kHitSilenceHoldSec (not before kHitMinSeconds, not after kHitMaxSeconds).
  static constexpr double kHitSilenceVolt = 2e-5, kHitSilenceHoldSec = 0.030, kHitMinSeconds = 0.06, kHitMaxSeconds = 1.5, kRmsWindowSec = 0.010;

  static int Velocity127(double velocity01) { return std::clamp((int)std::llround(velocity01 * 127.0), 0, 127); }

  void SetSampleRate(double sr) override
  {
    DrumVoice::SetSampleRate(sr);
    mModel.SetSampleRate(sr);
    mLevelApplied = -1.0;
    mWinSamples = std::max(1L, (long)(kRmsWindowSec * sr)); mHoldSamples = (long)(kHitSilenceHoldSec * sr);
    mMinSamples = (long)(kHitMinSeconds * sr); mMaxSamples = (long)(kHitMaxSeconds * sr);
  }

  void Trigger(double velocity01) override
  {
    mActive = false;
    const int vel = Velocity127(velocity01);
    if (vel <= 0) return;
    if (levelKnob != mLevelApplied) { mModel.SetLevel(levelKnob); mLevelApplied = levelKnob; }
    mModel.Start(kAccentFullScaleVolt, NextPhaseOffsetSec());
    mGain = kVoltToSample * (double)vel / 127.0;
    mRate = std::clamp(pitchRatio, 0.25, 4.0);
    mFrac = 0.0; mY0 = 0.0; mY1 = mModel.Step(); mSamples = 0; mQuiet = 0; mInWin = 0; mWinSq = 0.0;
    mActive = true;
  }

  void Process(double* out, int nFrames) override
  {
    if (!mActive) return;
    for (int i = 0; i < nFrames; ++i)
    {
      double v;
      if (mRate == 1.0) { v = mY1; mY1 = mModel.Step(); }
      else
      {
        v = mY0 + mFrac * (mY1 - mY0);
        mFrac += mRate;
        while (mFrac >= 1.0) { mFrac -= 1.0; mY0 = mY1; mY1 = mModel.Step(); }
      }
      if (!std::isfinite(v)) { mActive = false; return; }
      out[i] += v * mGain;   // no clamp, no limiter: the path is floating point
      mWinSq += v * v;
      if (++mInWin == mWinSamples)
      {
        mQuiet = (std::sqrt(mWinSq / (double)mWinSamples) < kHitSilenceVolt) ? mQuiet + mWinSamples : 0;
        mWinSq = 0.0; mInWin = 0;
      }
      if (++mSamples >= mMaxSamples || (mSamples >= mMinSamples && mQuiet >= mHoldSamples)) { mActive = false; return; }
    }
  }

  bool IsActive() const override { return mActive; }

private:
  // splitmix64 on a per-voice stream: uniform in [0, kPhaseSpanSec)
  double NextPhaseOffsetSec()
  {
    if (mRng == 0) mRng = 0x9E3779B97F4A7C15ULL * (1u + gSeedCounter.fetch_add(1, std::memory_order_relaxed));
    mRng += 0x9E3779B97F4A7C15ULL;
    uint64_t z = mRng;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z ^= z >> 31;
    return (double)(z >> 11) * (1.0 / 9007199254740992.0) * kPhaseSpanSec;
  }
  static inline std::atomic<uint32_t> gSeedCounter{1};

  cbfast::TR808CbFast mModel;
  bool mActive = false;
  double mGain = 0.0, mRate = 1.0, mFrac = 0.0, mY0 = 0.0, mY1 = 0.0, mLevelApplied = -1.0, mWinSq = 0.0;
  long mSamples = 0, mQuiet = 0, mInWin = 0;
  long mWinSamples = 480, mHoldSamples = 1440, mMinSamples = 2880, mMaxSamples = 72000;
  uint64_t mRng = 0;
};

}  // namespace dgg
