// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Reiner Prokein
#pragma once
// TR-808 / Yocto toms and congas. The sound comes from the fast model in 808TomFast.h (derived from the LT/MT/HT sheets of the common NGSpice circuit). The voice synthesises
// every hit directly on the audio thread.
//
// One instance is one physical voice: a sheet (LT/MT/HT) with its Tom/Conga switch position. Rack toms are the sheets in the Tom position, the floor toms are two of the
// sheets in the Conga position. Nothing here is a synthesizer parameter: tuning and level are the two pots of the sheet (VR11/13/15 and VR10/12/14) and the velocity is the
// accent voltage the sequencer's DAC puts on ACCENT_* (MIDI velocity 0..127 -> 0..4.09 V); below the first accent node of the model (1.5 V, velocity 47) the circuit is silent.
//
// The circuit has no rim articulations. Every articulation (Center, Rimshot, Rim Only) of every position is its own instance with its own tuning, level and pitch knobs;
// RimShot and RimOnly play the model output under a very short exponential decay (kRimShotTauSec / kRimOnlyTauSec). The circuit has no decay control, the rim sound is shaped
// with tuning and level.
//
// The model runs at 48 kHz (the rate of the model data); at another host rate, or with pitchRatio != 1 ("pitch" is a digital transpose), the output is resampled (cubic).

#include "DrumVoice.h"
#include "808TomFast.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include "808MixBus.h"

namespace dgg {

class TR808TomVoice final : public DrumVoice
{
public:
  enum class Articulation { Center, RimShot, RimOnly };

  // Which circuit this instance is (set once per pool): sheet 0 = LT, 1 = MT, 2 = HT; conga = switch in the Conga position.
  int sheet = 0;
  bool conga = false;
  Articulation articulation = Articulation::Center;
  // Knob positions 0..1 as the UI shows them.
  double tuningKnob = 0.5;
  double levelKnob = 0.5;
  // Digital transpose, not a schematic knob (same convention as the other voices).
  double pitchRatio = 1.0;

  // Jack voltage -> sample: the common jack factor, weighted per sheet by its mixing resistors (LT 15k / 8.2k, MT 12k / 12k, HT 8.2k / 15k).
  static constexpr double kVoltToSample = dgg::kJackVoltToSample;
  static double SheetMix(int s) { return s == 1 ? 1.0 : dgg::kMixWeightWide; }
  static constexpr double kAccentFullScaleVolt = 4.09; // Dio.ino SetDacA: map(velocity, 0, 127, 0, 4090) mV
  // A hit ends when the 10 ms RMS of the output has stayed below kSilenceVolt for kSilenceHoldSec (not before kMinSeconds, not after kMaxSeconds); model time.
  static constexpr double kMaxSeconds = 8.0, kSilenceVolt = 1e-6, kSilenceHoldSec = 0.25, kMinSeconds = 0.3, kRmsWindowSec = 0.010;
  static constexpr double kRimShotTauSec = 0.040;  // placeholder decay of the rim articulations
  static constexpr double kRimOnlyTauSec = 0.015;

  static int Velocity127(double velocity01) { return std::clamp((int)std::llround(velocity01 * 127.0), 0, 127); }
  static double AccentVolt(int vel127) { return (double)vel127 / 127.0 * kAccentFullScaleVolt; }

  void SetSampleRate(double sr) override
  {
    DrumVoice::SetSampleRate(sr);
    mHostRatio = tomfast::TR808TomFast::kFs / sr;
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
    if (mSampleRate <= 0.0) return;
    if (!mModel.Setup(std::clamp(sheet, 0, 2) * 2 + (conga ? 1 : 0), std::clamp(tuningKnob, 0.0, 1.0), std::clamp(levelKnob, 0.0, 1.0))) return;
    if (!mModel.StartAccent(mAccent)) return;
    mRate = std::clamp(pitchRatio, 0.25, 4.0) * mHostRatio;
    mDirect = std::fabs(mRate - 1.0) < 1e-12;
    mFrac = 0.0; mSamples = 0; mQuiet = 0; mInWin = 0; mWinSq = 0.0; mEnv = 1.0;
    mEnvStep = articulation == Articulation::Center ? 1.0 : std::exp(-1.0 / ((articulation == Articulation::RimShot ? kRimShotTauSec : kRimOnlyTauSec) * mSampleRate));
    constexpr double kFs = tomfast::TR808TomFast::kFs;
    mWinSamples = std::max(1L, (long)(kRmsWindowSec * kFs)); mHoldSamples = (long)(kSilenceHoldSec * kFs);
    mMinSamples = (long)(kMinSeconds * kFs); mMaxSamples = (long)(kMaxSeconds * kFs);
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
      out[i] += v * kVoltToSample * SheetMix(sheet) * mEnv * mVelGain;   // no clamp, no limiter: the path is floating point
      mEnv *= mEnvStep;
      if (mEnv < 1e-4) { mActive = false; return; }
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
      mQuiet = (std::sqrt(mWinSq / (double)mWinSamples) < kSilenceVolt) ? mQuiet + mWinSamples : 0;
      mWinSq = 0.0; mInWin = 0;
    }
    if (++mSamples >= mMaxSamples || (mSamples >= mMinSamples && mQuiet >= mHoldSamples)) mActive = false;
    return v;
  }

  double mVelGain = 1.0, mAccent = 0.0;
  tomfast::TR808TomFast mModel;
  bool mActive = false, mDirect = true;
  double mHostRatio = 1.0, mRate = 1.0, mFrac = 0.0, mWinSq = 0.0, mY[4] = {};
  double mEnv = 1.0, mEnvStep = 1.0;   // rim articulations: exponential decay per output sample
  long mSamples = 0, mQuiet = 0, mInWin = 0, mWinSamples = 480, mHoldSamples = 12000, mMinSamples = 14400, mMaxSamples = 384000;
};

}  // namespace dgg
