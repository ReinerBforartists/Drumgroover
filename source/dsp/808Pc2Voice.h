#pragma once
// TR-808 / Yocto PC-2 (Boss PC-2 equivalent, sheet PC-2). The sound comes from the fast model in 808Pc2Fast.h (a functional-block model derived from the PC-2 sheet of the common NGSpice
// circuit). The voice synthesises every hit directly on the audio thread, at the host
// sample rate.
//
// The knobs are the pots of the sheet: VR1 level, VR2 LFO rate, VR3 LFO depth, VR18 pitch, VR19 decay, VR20 sweep (bipolar, centre 0.5 with the trimmer TM5 at its factory position), and
// the switches SW2 (LFO waveform) and SW3 (VCO waveform). The velocity is the accent voltage the sequencer's DAC puts on ACCENT_PC2 (MIDI velocity 0..127 -> 0..4.09 V).
//
// The VCO and the LFO of the circuit run freely, so the sound depends on their phases at the trigger: every hit draws random phases. The output passes the one-pole 10.6 Hz coupling
// of Block 11 of the deck (15 uF, 1 kOhm), which removes the DC offset of the circuit (inside the model).

#include "DrumVoice.h"
#include "808Pc2Fast.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include "808MixBus.h"

namespace dgg {

class TR808Pc2Voice final : public DrumVoice
{
public:
  // Knob positions 0..1 as the UI shows them (provisional working defaults, see 808InstrumentTable.h).
  double levelKnob = 0.5, lfoRateKnob = 0.2, lfoDepthKnob = 0.16, pitchKnob = 0.49, decayKnob = 0.97, sweepKnob = 0.5;
  // Switches as 0/1 (1 = throw O): lfoWave 1 = square (U10B), 0 = triangle (U10A); vcoWave 1 = triangle (U25A), 0 = square (U11B).
  double lfoWaveKnob = 1.0, vcoWaveKnob = 0.0;

  // Jack voltage -> sample: the common jack factor. The largest peak of a hit over the knob corners is 4.9 V (level 1, velocity 127).
  static constexpr double kPeakVoltWorstCase = 5.0;
  static constexpr double kVoltToSample = dgg::kJackVoltToSample;
  static constexpr double kAccentFullScaleVolt = 4.09;   // Dio.ino SetDacA: map(velocity, 0, 127, 0, 4090) mV
  static constexpr double kTrimmerTm5 = 0.5;             // factory position of TM5, not a knob
  // A hit ends when the 10 ms RMS of the output has stayed below kHitSilenceVolt for kHitSilenceHoldSec (not before kHitMinSeconds, not after kHitMaxSeconds).
  static constexpr double kHitSilenceVolt = 2e-5, kHitSilenceHoldSec = 0.030, kHitMinSeconds = 0.06, kHitMaxSeconds = 3.0, kRmsWindowSec = 0.010;

  static int Velocity127(double velocity01) { return std::clamp((int)std::llround(velocity01 * 127.0), 0, 127); }
  static double AccentVolt(int vel127) { return (double)vel127 / 127.0 * kAccentFullScaleVolt; }

  void SetSampleRate(double sr) override
  {
    DrumVoice::SetSampleRate(sr);
    mModel.SetSampleRate(sr);
    mWinSamples = std::max(1L, (long)(kRmsWindowSec * sr)); mHoldSamples = (long)(kHitSilenceHoldSec * sr);
    mMinSamples = (long)(kHitMinSeconds * sr); mMaxSamples = (long)(kHitMaxSeconds * sr);
  }

  void Trigger(double velocity01) override
  {
    mActive = false;
    const int vel = Velocity127(velocity01);
    if (vel <= 0 || mSampleRate <= 0.0) return;
    pc2fast::TR808Pc2Fast::Knobs k;
    k.vr1 = std::clamp(levelKnob, 0.0, 1.0); k.vr2 = std::clamp(lfoRateKnob, 0.0, 1.0); k.vr3 = std::clamp(lfoDepthKnob, 0.0, 1.0); k.vr18 = std::clamp(pitchKnob, 0.0, 1.0);
    k.vr19 = std::clamp(decayKnob, 0.0, 1.0); k.vr20 = std::clamp(sweepKnob, 0.0, 1.0); k.tm5 = kTrimmerTm5; k.sw2 = lfoWaveKnob > 0.5 ? 1 : 0; k.sw3 = vcoWaveKnob > 0.5 ? 1 : 0;
    mModel.SetKnobs(k);
    if (mSeed == 0) mSeed = 0x9E3779B97F4A7C15ULL * (1u + gVoiceCounter.fetch_add(1, std::memory_order_relaxed));
    mSeed += 0x9E3779B97F4A7C15ULL;
    if (!mModel.Start(AccentVolt(vel), mSeed)) return;
    mSamples = 0; mQuiet = 0; mInWin = 0; mWinSq = 0.0; mActive = true;
  }

  void Process(double* out, int nFrames) override
  {
    if (!mActive) return;
    for (int i = 0; i < nFrames; ++i)
    {
      const double v = mModel.Step();
      if (!std::isfinite(v)) { mActive = false; return; }
      out[i] += v * kVoltToSample;   // no clamp, no limiter: the path is floating point, only the gain is set
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
  static inline std::atomic<uint32_t> gVoiceCounter{1};
  pc2fast::TR808Pc2Fast mModel;
  bool mActive = false;
  double mWinSq = 0.0; uint64_t mSeed = 0;
  long mSamples = 0, mQuiet = 0, mInWin = 0, mWinSamples = 480, mHoldSamples = 1440, mMinSamples = 2880, mMaxSamples = 144000;
};

}  // namespace dgg
