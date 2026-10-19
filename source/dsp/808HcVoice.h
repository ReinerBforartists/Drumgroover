// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Reiner Prokein
#pragma once
// TR-808 / Yocto hand clap and maracas (HC sheet, "CP/MA"). The sound comes from the fast model in 808HcFast.h (derived from the NOISE and HC sheets of the common NGSpice
// circuit). The voice synthesises every hit directly on the audio thread. Every hit draws its own noise seed, as the circuit's noise source never repeats.
//
// One instance is one position of the CP/MA switch SW12: Hand Clap (throw O) or Maracas (throw S). Nothing here is a synthesizer parameter: the level is the pot VR17 and
// the velocity is the accent voltage the sequencer's DAC puts on ACCENT_HC (MIDI velocity 0..127 -> 0..4.09 V); below the accent threshold of the sheet (hand clap velocity 58,
// maracas velocity 54) the circuit is silent. The circuit has no tuning or decay control; "pitch" is a digital transpose (the model output is read at pitchRatio samples per
// output sample). The trimmer TM3 (C.P. OFFSET) is a factory setting, not a knob.
//
// The model runs at 48 kHz (the rate of the model data); at another host rate the output is resampled (cubic interpolation).

#include "DrumVoice.h"
#include "808HcFast.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include "808MixBus.h"

namespace dgg {

class TR808HcVoice final : public DrumVoice
{
public:
  // Which position of SW12 this instance is (set once per pool): true = Hand Clap (throw O), false = Maracas (throw S).
  bool clap = true;
  // Knob position 0..1 as the UI shows it (VR17, taper A).
  double levelKnob = 0.5;
  // Digital transpose, not a schematic knob.
  double pitchRatio = 1.0;

  // Jack voltage -> sample: the common jack factor (clap and maracas share the CP_MA_OUT channel, 12k / 12k).
  static constexpr double kVoltToSample = dgg::kJackVoltToSample;
  static constexpr double kAccentFullScaleVolt = 4.09; // Dio.ino SetDacA: map(velocity, 0, 127, 0, 4090) mV
  // A hit ends when the 10 ms RMS of the output has stayed below kHitSilenceVolt for kHitSilenceHoldSec (not before kHitMinSeconds, not after kHitMaxSeconds); model time.
  static constexpr double kHitSilenceVolt = 1e-3, kHitSilenceHoldSec = 0.030, kHitMinSeconds = 0.06, kHitMaxSeconds = 3.0, kRmsWindowSec = 0.010;

  static int Velocity127(double velocity01) { return std::clamp((int)std::llround(velocity01 * 127.0), 0, 127); }
  static double AccentVolt(int vel127) { return (double)vel127 / 127.0 * kAccentFullScaleVolt; }

  void SetSampleRate(double sr) override
  {
    DrumVoice::SetSampleRate(sr);
    mHostRatio = hcfast::TR808HcFast::kFs / sr;
  }

  void SetAccent(double accent01) override { mAccent = std::clamp(accent01, 0.0, 1.0); }
  // The velocity is the loudness of this note (it scales the output linearly, as in the Lo-Fi engine). The circuit runs at the trigger amplitude of the accent set by
  // SetAccent (the Accent lane and the Accent amount): 0 = the normal note, 4.09 V, the reference point every level and the Yocto2 comparison are calibrated at;
  // 1 = 14 V, the top of the 808's accent range (4.09 V + accent * 9.91 V). Level and tone change with the accent as in the circuit.
  void Trigger(double velocity01) override
  {
    mActive = false;
    const int vel = Velocity127(velocity01);
    if (vel <= 0) return;
    mVelGain = (double)vel / 127.0;
    if (mSeedCounter == 0) mSeedCounter = 0x9E3779B97F4A7C15ULL * (1u + gVoiceCounter.fetch_add(1, std::memory_order_relaxed));
    mSeedCounter += 0x9E3779B97F4A7C15ULL; mModel.SetSeed(Mix(mSeedCounter));
    const double lvl = std::clamp(levelKnob, 0.0, 1.0);
    if (!(clap ? mModel.StartClapAccent(mAccent, lvl) : mModel.StartMaracasAccent(mAccent, lvl))) return;
    mRate = std::clamp(pitchRatio, 0.25, 4.0) * mHostRatio;
    mDirect = std::fabs(mRate - 1.0) < 1e-12;
    mFrac = 0.0; mSamples = 0; mQuiet = 0; mInWin = 0; mWinSq = 0.0;
    mWinSamples = std::max(1L, (long)(kRmsWindowSec * hcfast::TR808HcFast::kFs)); mHoldSamples = (long)(kHitSilenceHoldSec * hcfast::TR808HcFast::kFs);
    mMinSamples = (long)(kHitMinSeconds * hcfast::TR808HcFast::kFs); mMaxSamples = (long)(kHitMaxSeconds * hcfast::TR808HcFast::kFs);
    mActive = true;
    if (!mDirect) { mY[0] = 0.0; mY[1] = NextModel(); mY[2] = NextModel(); mY[3] = NextModel(); }   // history of the cubic interpolation: the first output sample is the first model sample
  }

  void Process(double* out, int nFrames) override
  {
    if (!mActive) return;
    for (int i = 0; i < nFrames; ++i)
    {
      double v;
      if (mDirect) { v = NextModel(); }
      else
      {
        // Catmull-Rom between mY[1] and mY[2] (mY[0], mY[3] neighbours); the model is run ahead as the read position advances
        while (mFrac >= 1.0) { mFrac -= 1.0; mY[0] = mY[1]; mY[1] = mY[2]; mY[2] = mY[3]; mY[3] = NextModel(); }
        const double t = mFrac, y0 = mY[0], y1 = mY[1], y2 = mY[2], y3 = mY[3];
        v = y1 + 0.5 * t * (y2 - y0 + t * (2.0 * y0 - 5.0 * y1 + 4.0 * y2 - y3 + t * (3.0 * (y1 - y2) + y3 - y0)));
        mFrac += mRate;
      }
      if (!mActive) return;
      out[i] += v * kVoltToSample * mVelGain;   // no clamp, no limiter: the path is floating point, only the gain is set
    }
  }

  bool IsActive() const override { return mActive; }

private:
  // next model sample (volts); runs the hit-end detection in model time
  double NextModel()
  {
    const double v = mModel.Step(); mLast = v;
    if (!std::isfinite(v)) { mActive = false; return 0.0; }
    mWinSq += v * v;
    if (++mInWin == mWinSamples)
    {
      mQuiet = (std::sqrt(mWinSq / (double)mWinSamples) < kHitSilenceVolt) ? mQuiet + mWinSamples : 0;
      mWinSq = 0.0; mInWin = 0;
    }
    if (++mSamples >= mMaxSamples || (mSamples >= mMinSamples && mQuiet >= mHoldSamples)) mActive = false;
    return v;
  }
  static uint64_t Mix(uint64_t z)
  {
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
  }
  static inline std::atomic<uint32_t> gVoiceCounter{1};

  double mVelGain = 1.0, mAccent = 0.0;
  hcfast::TR808HcFast mModel;
  bool mActive = false, mDirect = true;
  double mHostRatio = 1.0, mRate = 1.0, mFrac = 0.0, mWinSq = 0.0, mLast = 0.0, mY[4] = {};
  long mSamples = 0, mQuiet = 0, mInWin = 0, mWinSamples = 480, mHoldSamples = 1440, mMinSamples = 2880, mMaxSamples = 144000;
  uint64_t mSeedCounter = 0;
};

}  // namespace dgg
