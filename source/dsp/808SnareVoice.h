#pragma once
// TR-808 / Yocto snare drum voice. The sound comes from the fast model in 808SnareFast.h (derived from the NOISE and SD sheets of the common NGSpice circuit). The voice
// synthesises every hit directly on the audio thread. Every hit draws its own noise seed, as the circuit's noise source never repeats.
//
// Nothing here is a synthesizer parameter: level, tone and snappy are the three pots of the SD sheet (VR7, VR8, VR9), as rotation positions 0..1, and the velocity is the accent
// voltage the sequencer's DAC puts on ACCENT_SD (MIDI velocity 0..127 -> 0..4.09 V); the trigger stage turns on between about 1.4 V and 2 V. "Pitch" is a digital transpose (the model
// output is read at pitchRatio samples per output sample).
//
// The model runs at 48 kHz (the rate of the model data); at another host rate the output is resampled (cubic interpolation).

#include "DrumVoice.h"
#include "808SnareFast.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include "808MixBus.h"

namespace dgg {

class TR808SnareVoice final : public DrumVoice
{
public:
  // Pot positions 0..1 as the UI shows them.
  double toneKnob = 0.5;     // VR8
  double snappyKnob = 0.5;   // VR9
  double levelKnob = 0.5;    // VR7
  // Digital transpose, not a schematic knob.
  double pitchRatio = 1.0;

  // Jack voltage -> sample: the common jack factor (the snare channel is 12k / 12k).
  static constexpr double kVoltToSample = dgg::kJackVoltToSample;
  static constexpr double kAccentFullScaleVolt = snarefast::TR808SnareFast::kAccentFullScaleVolt;
  // A hit ends when the 10 ms RMS of the output has stayed below kSilenceVolt for kSilenceHoldSec (not before kMinSeconds, not after kMaxSeconds); model time.
  static constexpr double kSilenceVolt = 1e-4, kSilenceHoldSec = 0.15, kMinSeconds = 0.3, kMaxSeconds = 3.0, kRmsWindowSec = 0.010;

  static int Velocity127(double velocity01) { return std::clamp((int)std::llround(velocity01 * 127.0), 0, 127); }
  static double AccentVolt(int vel127) { return (double)vel127 / 127.0 * kAccentFullScaleVolt; }

  void SetSampleRate(double sr) override
  {
    DrumVoice::SetSampleRate(sr);
    mHostRatio = snarefast::TR808SnareFast::kFs / sr;
  }

  void Trigger(double velocity01) override
  {
    mActive = false;
    const int vel = Velocity127(velocity01);
    if (vel <= 0) return;
    if (mSeedCounter == 0) mSeedCounter = 0x9E3779B97F4A7C15ULL * (1u + gVoiceCounter.fetch_add(1, std::memory_order_relaxed));
    mSeedCounter += 0x9E3779B97F4A7C15ULL; mModel.SetSeed(Mix(mSeedCounter));
    if (!mModel.Start(vel, toneKnob, snappyKnob, levelKnob)) return;
    mRate = std::clamp(pitchRatio, 0.25, 4.0) * mHostRatio;
    mDirect = std::fabs(mRate - 1.0) < 1e-12;
    mFrac = 0.0; mSamples = 0; mQuiet = 0; mInWin = 0; mWinSq = 0.0;
    mWinSamples = std::max(1L, (long)(kRmsWindowSec * snarefast::TR808SnareFast::kFs)); mHoldSamples = (long)(kSilenceHoldSec * snarefast::TR808SnareFast::kFs);
    mMinSamples = (long)(kMinSeconds * snarefast::TR808SnareFast::kFs); mMaxSamples = (long)(kMaxSeconds * snarefast::TR808SnareFast::kFs);
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
      out[i] += v * kVoltToSample;   // no clamp, no limiter: the path is floating point, only the gain is set
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
      mQuiet = (std::sqrt(mWinSq / (double)mWinSamples) < kSilenceVolt) ? mQuiet + mWinSamples : 0;
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

  snarefast::TR808SnareFast mModel;
  bool mActive = false, mDirect = true;
  double mHostRatio = 1.0, mRate = 1.0, mFrac = 0.0, mWinSq = 0.0, mLast = 0.0, mY[4] = {};
  long mSamples = 0, mQuiet = 0, mInWin = 0, mWinSamples = 480, mHoldSamples = 1440, mMinSamples = 2880, mMaxSamples = 144000;
  uint64_t mSeedCounter = 0;
};

}  // namespace dgg
