// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Reiner Prokein
#pragma once
// TR-808 / Yocto bass drum. The sound comes from the fast model in 808KickFast.h (derived from the BD sheet of the common NGSpice circuit). The voice synthesises
// every hit directly on the audio thread, at the host sample rate.
//
// Nothing here is a synthesizer parameter: decay, tone and level are the three pots of the BD sheet (VR6, VR5, VR4) and the velocity is the accent voltage the sequencer's DAC puts on
// ACCENT_BD (MIDI velocity 0..127 -> 0..4.09 V). pitchRatio is a digital transpose (not a schematic knob): the model output is read at pitchRatio times the host rate, with linear
// interpolation, so decay stretches with the pitch (the same convention as the Ride/HiHat pitch ratio).

#include "DrumVoice.h"
#include "808KickFast.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "808MixBus.h"

namespace dgg {

class TR808KickVoice final : public DrumVoice
{
public:
  // Knob positions 0..1 as the UI shows them.
  double decayKnob01 = 0.5;
  double toneKnob = 0.5;
  double levelKnob = 0.5;
  double pitchRatio = 1.0;

  // Jack voltage -> sample. 1 V would clip the kick (peak up to 2.06 V at full accent and brightest tone); 0.45 puts 2.2 V at full scale. No clamp: the path is floating point, only the gain is set.
  static constexpr double kVoltToSample = dgg::kJackVoltToSample;
  static constexpr double kAccentFullScaleVolt = 4.09;   // Dio.ino SetDacA: map(velocity, 0, 127, 0, 4090) mV
  static constexpr double kMaxSeconds = 8.0;
  static constexpr double kSilenceVolt = 1e-6;
  static constexpr double kSilenceHoldSec = 0.25;
  static constexpr double kMinSeconds = 0.3;

  // ---- pot laws (from the pot labels of the schematic: VR6 500K(B) linear, VR5 10K(C) reverse-log, VR4 100KA audio taper; clockwise = longer decay / brighter tone / louder)
  static double AudioTaper(double p)
  {
    p = std::clamp(p, 0.0, 1.0);
    return p <= 0.5 ? 0.3 * p : 0.15 + 1.7 * (p - 0.5);
  }
  static double ReverseLogTaper(double p) { return 1.0 - AudioTaper(1.0 - std::clamp(p, 0.0, 1.0)); }
  static kickfast::TR808KickFast::Pots PotsFromKnobs(double decay01, double tone01, double level01)
  {
    kickfast::TR808KickFast::Pots p;
    p.vr6 = std::clamp(decay01, 0.0, 1.0);
    p.vr5 = 1.0 - ReverseLogTaper(tone01);
    p.vr4 = AudioTaper(level01);
    return p;
  }
  static int Velocity127(double velocity01) { return std::clamp((int)std::llround(velocity01 * 127.0), 0, 127); }
  static double AccentVolt(int vel127) { return (double)vel127 / 127.0 * kAccentFullScaleVolt; }

  void SetSampleRate(double sr) override { DrumVoice::SetSampleRate(sr); mModel.SetSampleRate(sr); mHoldSamples = (long)(kSilenceHoldSec * sr); mMinSamples = (long)(kMinSeconds * sr); mMaxSamples = (long)(kMaxSeconds * sr); }

  void Trigger(double velocity01) override
  {
    mActive = false;
    const int vel = Velocity127(velocity01);
    if (vel <= 0 || mSampleRate <= 0.0) return;
    mModel.SetPots(PotsFromKnobs(decayKnob01, toneKnob, levelKnob));
    if (!mModel.Start(vel)) return;
    mRest = mModel.RestOutput();
    mPitch = std::clamp(pitchRatio, 0.25, 4.0); mPos = 0.0; mHave = 0; mS0 = mS1 = 0.0; mSamples = 0; mQuiet = 0; mActive = true;
  }

  void Process(double* out, int nFrames) override
  {
    if (!mActive) return;
    for (int i = 0; i < nFrames; ++i)
    {
      // read head at pitchRatio model samples per host sample; model sample j is the output at t = (j + 1) / sr
      while (mHave < (long)mPos + 2) { mS0 = mS1; mS1 = mModel.Step() - mRest; ++mHave; }
      const double frac = mPos - std::floor(mPos), v = mS0 + frac * (mS1 - mS0);
      if (!std::isfinite(v)) { mActive = false; return; }
      out[i] += v * kVoltToSample;
      mPos += mPitch;
      mQuiet = (std::fabs(v) < kSilenceVolt) ? mQuiet + 1 : 0;
      if (++mSamples >= mMaxSamples || (mSamples >= mMinSamples && mQuiet >= mHoldSamples)) { mActive = false; return; }
    }
  }

  bool IsActive() const override { return mActive; }

private:
  kickfast::TR808KickFast mModel;
  bool mActive = false; double mRest = 0.0, mPitch = 1.0, mPos = 0.0, mS0 = 0.0, mS1 = 0.0; long mHave = 0, mSamples = 0, mQuiet = 0, mHoldSamples = 12000, mMinSamples = 14400, mMaxSamples = 384000;
};

}  // namespace dgg
