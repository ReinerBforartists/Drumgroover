// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Reiner Prokein
#pragma once
// TR-808 / Yocto rim shot and claves (RM sheet, "RS/CL"). The sound comes from the fast model in 808RmFast.h (derived from the RM sheet of the common NGSpice circuit). The voice synthesises
// every hit directly on the audio thread.
//
// One instance is one position of the RS/CL switch SW11: Rim Shot (throw O) or Claves (throw S). Nothing here is a synthesizer parameter: the level is the pot VR16 and the velocity is the
// accent voltage the sequencer's DAC puts on ACCENT_RM (MIDI velocity 0..127 -> 0..4.09 V); below the first accent node of the model (rim shot 1.7 V, claves 1.7 V; the circuit is silent
// below about 1.9 V) nothing sounds. The circuit has no tuning or decay control; "pitch" is a digital transpose (the model output is read at pitchRatio samples per output sample).
//
// The model runs at 48 kHz (the rate of the model data); at another host rate, or with pitchRatio != 1, the output is resampled (cubic interpolation).

#include "DrumVoice.h"
#include "808RmFast.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "808MixBus.h"

namespace dgg {

class TR808RmVoice final : public DrumVoice
{
public:
  // Which position of SW11 this instance is (set once per pool): false = Rim Shot (throw O), true = Claves (throw S).
  bool claves = false;
  // Knob position 0..1 as the UI shows it (VR16, taper A).
  double levelKnob = 0.5;
  // Digital transpose, not a schematic knob.
  double pitchRatio = 1.0;

  // Jack voltage -> sample: the common jack factor (rim shot and claves both sit on a 12k / 12k channel).
  static constexpr double kVoltToSample = dgg::kJackVoltToSample;
  static constexpr double kAccentFullScaleVolt = 4.09; // Dio.ino SetDacA: map(velocity, 0, 127, 0, 4090) mV
  // A hit ends when the 2 ms RMS of the output has stayed below kHitSilenceVolt (-100 dBFS; the model leaves a few uV of DC from the slow C126 / VR16 relaxation, the engine does not) for kHitSilenceHoldSec (not before kHitMinSeconds, not after kHitMaxSeconds); model time.
  static constexpr double kHitSilenceVolt = 1e-5, kHitSilenceHoldSec = 0.010, kHitMinSeconds = 0.03, kHitMaxSeconds = 0.5, kRmsWindowSec = 0.002;

  static int Velocity127(double velocity01) { return std::clamp((int)std::llround(velocity01 * 127.0), 0, 127); }
  static double AccentVolt(int vel127) { return (double)vel127 / 127.0 * kAccentFullScaleVolt; }

  void SetSampleRate(double sr) override
  {
    DrumVoice::SetSampleRate(sr);
    mHostRatio = rmfast::TR808RmFast::kFs / sr;
  }

  void SetAccent(double accent01) override { mAccent = std::clamp(accent01, 0.0, 1.0); }
  // The velocity is the loudness of this note (it scales the output linearly, as in the Lo-Fi engine). The circuit runs at the trigger amplitude of the accent set by
  // SetAccent (the Accent lane and the Accent amount): 0 = the normal note, 4.09 V, the reference point every level and the Yocto2 comparison are calibrated at;
  // 1 = 14 V, the top of the 808's accent range (4.09 V + accent * 9.91 V). Level and tone change with the accent as in the circuit.
  void Trigger(double velocity01) override
  {
    mActive = false;
    const int vel = Velocity127(velocity01);
    if (vel <= 0 || mSampleRate <= 0.0) return;
    mVelGain = (double)vel / 127.0;
    mModel.SetLevel(std::clamp(levelKnob, 0.0, 1.0));
    if (!mModel.StartAccent(claves, mAccent)) return;
    mRate = std::clamp(pitchRatio, 0.25, 4.0) * mHostRatio;
    mDirect = std::fabs(mRate - 1.0) < 1e-12;
    mFrac = 0.0; mSamples = 0; mQuiet = 0; mInWin = 0; mWinSq = 0.0;
    constexpr double kFs = rmfast::TR808RmFast::kFs;
    mWinSamples = std::max(1L, (long)(kRmsWindowSec * kFs)); mHoldSamples = (long)(kHitSilenceHoldSec * kFs);
    mMinSamples = (long)(kHitMinSeconds * kFs); mMaxSamples = (long)(kHitMaxSeconds * kFs);
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
    const double v = mModel.Step();
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

  double mVelGain = 1.0, mAccent = 0.0;
  rmfast::TR808RmFast mModel;
  bool mActive = false, mDirect = true;
  double mHostRatio = 1.0, mRate = 1.0, mFrac = 0.0, mWinSq = 0.0, mY[4] = {};
  long mSamples = 0, mQuiet = 0, mInWin = 0, mWinSamples = 96, mHoldSamples = 480, mMinSamples = 1440, mMaxSamples = 24000;
};

}  // namespace dgg
