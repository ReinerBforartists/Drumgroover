#pragma once

#include "DrumVoice.h"
#include "BiquadFilter.h"
#include "Mixer.h"
#include <cmath>
#include <cstdint>
#include <algorithm>

namespace dgg {

// Port of audio-engine.js WebAudio synthesis (oscillator + noise, no samples).
// Used for the "Lo-Fi" engine mode. Each play*() function is reimplemented
// with per‑sample ExpRamp() matching Web Audio's exponential ramp.
// One simplification: decay floor is peak * 0.001 instead of absolute 0.001
// to avoid the floor being louder than a very quiet hit.
enum class LoFiRole {
  Kick, KickOpen, Snare, Rim, HatClosed, HatOpen,
  TomLow, TomMid, TomHi,
  TomLowMid, TomHighMid,   // Rack2 / Floor1 centers -> distinct pitches so all five toms differ
  TomRimshotLow, TomRimshotLowMid, TomRimshotMid, TomRimshotHighMid, TomRimshotHi,
  TomRimOnlyLow, TomRimOnlyLowMid, TomRimOnlyMid, TomRimOnlyHighMid, TomRimOnlyHi,
  Crash1, Crash2, Ride, RideBell,
  Generic   // fallback for unmapped GM notes
};

class LoFiVoice final : public DrumVoice
{
public:
  void Trigger(double velocity01) override { Trigger(velocity01, LoFiRole::Generic); }

  // Real entry point – selects the synthesis layer based on role.
  void Trigger(double velocity01, LoFiRole role)
  {
    mElapsed = 0.0;
    mActive = true;
    mSeed = (mSeed * 1664525u + 1013904223u) | 1u;

    mLayerA = Layer{};
    mLayerB = Layer{};
    BuildLayers(role, velocity01, mLayerA, mLayerB);

    mPhaseA = mPhaseB = 0.0;
    mFilterA.Reset();
    mFilterB.Reset();
    if (mLayerA.filterType == kFilterBandpass) mFilterA.SetBandpass(mLayerA.filterFreq, mLayerA.filterQ, mSampleRate);
    else if (mLayerA.filterType == kFilterHighpass) mFilterA.SetHighpass(mLayerA.filterFreq, mSampleRate, mLayerA.filterQ);
    if (mLayerB.filterType == kFilterBandpass) mFilterB.SetBandpass(mLayerB.filterFreq, mLayerB.filterQ, mSampleRate);
    else if (mLayerB.filterType == kFilterHighpass) mFilterB.SetHighpass(mLayerB.filterFreq, mSampleRate, mLayerB.filterQ);

    mStopTime = std::max(mLayerA.active ? mLayerA.stopTime : 0.0,
                          mLayerB.active ? mLayerB.stopTime : 0.0);
  }

  void Process(double* out, int nFrames) override
  {
    if (!mActive) return;
    const double dt = 1.0 / mSampleRate;

    for (int i = 0; i < nFrames; i++)
    {
      if (mElapsed >= mStopTime)
      {
        mActive = false;
        break;
      }

      double sample = 0.0;
      if (mLayerA.active) sample += RenderLayer(mLayerA, mFilterA, mPhaseA);
      if (mLayerB.active) sample += RenderLayer(mLayerB, mFilterB, mPhaseB);

      out[i] += sample;
      mElapsed += dt;
    }
  }

  bool IsActive() const override { return mActive; }

  // Mixer routing: which mixer channel this voice's output is summed into
  // (dgg::MixerChannel). Set by the engine right after Trigger(); defaults
  // to the Custom catch-all channel.
  void SetTargetChannel(int ch) { mTargetChannel = ch; }
  int TargetChannel() const { return mTargetChannel; }

private:
  static constexpr int kFilterNone     = 0;
  static constexpr int kFilterBandpass = 1;
  static constexpr int kFilterHighpass = 2;
  static constexpr int kOscSine        = 0;
  static constexpr int kOscTriangle    = 1;

  struct Layer {
    bool   active         = false;
    bool   isNoise        = false;
    int    oscType         = kOscSine;
    double freqStart       = 0.0;
    double freqEnd         = 0.0;
    double freqRampDur     = 0.0;      // 0 → constant frequency
    int    filterType      = kFilterNone;
    double filterFreq      = 1000.0;
    double filterQ         = 0.7071;
    double gainAttackDur   = 0.0;      // 0 → jump to peak (setValueAtTime)
    double gainPeak        = 0.0;
    double gainDecayDur    = 0.05;
    double stopTime        = 0.0;      // layer (and voice) goes silent after this
  };

  // Exponential ramp: a*(b/a)^(t/d), clamped to [0,d]. Falls back to linear if a or b <= 0.
  static double ExpRamp(double a, double b, double t, double d)
  {
    if (d <= 0.0) return b;
    const double frac = std::clamp(t / d, 0.0, 1.0);
    if (a <= 0.0 || b <= 0.0) return a + (b - a) * frac;
    return a * std::pow(b / a, frac);
  }

  double LayerGain(const Layer& layer, double t) const
  {
    const double floor = layer.gainPeak * 0.001;
    if (layer.gainAttackDur > 0.0 && t < layer.gainAttackDur)
      return ExpRamp(0.001, layer.gainPeak, t, layer.gainAttackDur);

    const double tDecay = t - layer.gainAttackDur;
    if (tDecay < layer.gainDecayDur)
      return ExpRamp(layer.gainPeak, floor, tDecay, layer.gainDecayDur);

    return floor;
  }

  // Fast xorshift32 noise, per‑voice state to avoid shared randomness.
  double NextNoiseSample()
  {
    mSeed ^= mSeed << 13;
    mSeed ^= mSeed >> 17;
    mSeed ^= mSeed << 5;
    return (static_cast<double>(mSeed) / 4294967295.0) * 2.0 - 1.0;
  }

  double RenderLayer(const Layer& layer, BiquadFilter& filter, double& phase)
  {
    double raw;
    if (layer.isNoise)
    {
      raw = NextNoiseSample();
    }
    else
    {
      const double freq = layer.freqRampDur > 0.0
        ? ExpRamp(layer.freqStart, layer.freqEnd, mElapsed, layer.freqRampDur)
        : layer.freqStart;
      phase += 2.0 * 3.14159265358979 * freq / mSampleRate;
      if (phase > 2.0 * 3.14159265358979) phase -= 2.0 * 3.14159265358979;
      raw = (layer.oscType == kOscTriangle)
        ? (2.0 / 3.14159265358979) * std::asin(std::sin(phase))
        : std::sin(phase);
    }

    const double filtered = (layer.filterType == kFilterNone) ? raw : filter.Process(raw);
    return filtered * LayerGain(layer, mElapsed);
  }

  // 1:1 port of audio-engine.js play*() functions.
  static void BuildLayers(LoFiRole role, double velocity01, Layer& a, Layer& b)
  {
    switch (role)
    {
      case LoFiRole::Kick:     BuildKick(velocity01, false, a); break;
      case LoFiRole::KickOpen: BuildKick(velocity01, true,  a); break;

      case LoFiRole::Snare:
        a.active = true; a.isNoise = true;
        a.filterType = kFilterBandpass; a.filterFreq = 1800.0; a.filterQ = 0.8;
        a.gainPeak = 0.7 * velocity01; a.gainDecayDur = 0.18;
        a.stopTime = 0.2;
        b.active = true; b.isNoise = false; b.oscType = kOscTriangle;
        b.freqStart = 190.0; b.freqEnd = 190.0; b.freqRampDur = 0.0;
        b.gainPeak = 0.5 * velocity01; b.gainDecayDur = 0.1;
        b.stopTime = 0.1;
        break;

      case LoFiRole::Rim:
        a.active = true; a.isNoise = true;
        a.filterType = kFilterBandpass; a.filterFreq = 2600.0; a.filterQ = 3.0;
        a.gainPeak = 0.5 * velocity01; a.gainDecayDur = 0.04;
        a.stopTime = 0.05;
        break;

      case LoFiRole::HatClosed:
        a.active = true; a.isNoise = true;
        a.filterType = kFilterHighpass; a.filterFreq = 7000.0;
        a.gainPeak = 0.35 * velocity01; a.gainDecayDur = 0.06;
        a.stopTime = 0.08;
        break;

      case LoFiRole::HatOpen:
        a.active = true; a.isNoise = true;
        a.filterType = kFilterHighpass; a.filterFreq = 7000.0;
        a.gainPeak = 0.35 * velocity01; a.gainDecayDur = 0.35;
        a.stopTime = 0.37;
        break;

      case LoFiRole::Ride:
        a.active = true; a.isNoise = true;
        a.filterType = kFilterHighpass; a.filterFreq = 5000.0;
        a.gainPeak = 0.25 * velocity01; a.gainDecayDur = 0.55;
        a.stopTime = 0.6;
        b.active = true; b.isNoise = false; b.oscType = kOscSine;
        b.freqStart = 800.0; b.freqEnd = 800.0; b.freqRampDur = 0.0;
        b.gainPeak = 0.15 * velocity01; b.gainDecayDur = 0.4;
        b.stopTime = 0.4;
        break;

      case LoFiRole::RideBell:
        a.active = true; a.isNoise = false; a.oscType = kOscSine;
        a.freqStart = 1400.0; a.freqEnd = 1400.0; a.freqRampDur = 0.0;
        a.gainPeak = 0.3 * velocity01; a.gainDecayDur = 0.3;
        a.stopTime = 0.3;
        b.active = true; b.isNoise = true;
        b.filterType = kFilterHighpass; b.filterFreq = 6500.0;
        b.gainPeak = 0.2 * velocity01; b.gainDecayDur = 0.12;
        b.stopTime = 0.15;
        break;

      case LoFiRole::TomLow:     BuildTom(velocity01, 110.0, a); break; // Floor Tom 2 Center
      case LoFiRole::TomLowMid:  BuildTom(velocity01, 130.0, a); break; // Floor Tom 1 Center (new)
      case LoFiRole::TomMid:     BuildTom(velocity01, 150.0, a); break; // Rack Tom 3 Center
      case LoFiRole::TomHighMid: BuildTom(velocity01, 180.0, a); break; // Rack Tom 2 Center (new)
      case LoFiRole::TomHi:      BuildTom(velocity01, 210.0, a); break; // Rack Tom 1 Center

      case LoFiRole::TomRimshotLow:      BuildTomRimshot(velocity01, 110.0, a, b); break;
      case LoFiRole::TomRimshotLowMid:   BuildTomRimshot(velocity01, 130.0, a, b); break;
      case LoFiRole::TomRimshotMid:      BuildTomRimshot(velocity01, 150.0, a, b); break;
      case LoFiRole::TomRimshotHighMid:  BuildTomRimshot(velocity01, 180.0, a, b); break;
      case LoFiRole::TomRimshotHi:       BuildTomRimshot(velocity01, 210.0, a, b); break;

      case LoFiRole::TomRimOnlyLow:      BuildTomRimOnly(velocity01, 110.0, a); break;
      case LoFiRole::TomRimOnlyLowMid:   BuildTomRimOnly(velocity01, 130.0, a); break;
      case LoFiRole::TomRimOnlyMid:      BuildTomRimOnly(velocity01, 150.0, a); break;
      case LoFiRole::TomRimOnlyHighMid:  BuildTomRimOnly(velocity01, 180.0, a); break;
      case LoFiRole::TomRimOnlyHi:       BuildTomRimOnly(velocity01, 210.0, a); break;

      case LoFiRole::Crash1: BuildCrash(velocity01, false, a); break;
      case LoFiRole::Crash2: BuildCrash(velocity01, true,  a); break;

      case LoFiRole::Generic:
      default:
        a.active = true; a.isNoise = true;
        a.filterType = kFilterBandpass; a.filterFreq = 1500.0; a.filterQ = 2.0;
        a.gainPeak = 0.5 * velocity01; a.gainDecayDur = 0.08;
        a.stopTime = 0.09;
        break;
    }
  }

  // Kick: open variant is less punchy and shorter.
  static void BuildKick(double velocity01, bool open, Layer& a)
  {
    a.active = true; a.isNoise = false; a.oscType = kOscSine;
    a.freqStart = 150.0; a.freqEnd = 42.0; a.freqRampDur = 0.14;
    a.gainAttackDur = 0.005;
    a.gainPeak = (open ? 0.7 : 0.9) * velocity01;
    a.gainDecayDur = open ? 0.15 : 0.275;
    a.stopTime = open ? 0.17 : 0.3;
  }

  static void BuildTom(double velocity01, double baseFreq, Layer& a)
  {
    a.active = true; a.isNoise = false; a.oscType = kOscSine;
    a.freqStart = baseFreq; a.freqEnd = baseFreq * 0.6; a.freqRampDur = 0.18;
    a.gainPeak = 0.8 * velocity01; a.gainDecayDur = 0.22;
    a.stopTime = 0.24;
  }

  static void BuildCrash(double velocity01, bool bright, Layer& a)
  {
    a.active = true; a.isNoise = true;
    a.filterType = kFilterHighpass; a.filterFreq = bright ? 5000.0 : 3800.0;
    a.gainPeak = 0.35 * velocity01; a.gainDecayDur = 1.3;
    a.stopTime = 1.4;
  }

  // Rim-only: dry stick-on-rim click, no body. Click brightness tracks the tom's
  // pitch so each position differs (higher toms -> brighter click).
  static void BuildTomRimOnly(double velocity01, double baseFreq, Layer& a)
  {
    const double clickFreq = 2200.0 + (baseFreq - 110.0) * 6.0; // ~2200..2800 across the ladder
    a.active = true; a.isNoise = true;
    a.filterType = kFilterBandpass; a.filterFreq = clickFreq; a.filterQ = 3.5;
    a.gainPeak = 0.45 * velocity01; a.gainDecayDur = 0.04;
    a.stopTime = 0.06;
  }

  // Rimshot: rim click + an audible short body ring at this tom's own pitch, so it
  // clearly differs from Rim Only (no body) and from other toms (different pitch).
  static void BuildTomRimshot(double velocity01, double baseFreq, Layer& a, Layer& b)
  {
    const double clickFreq = 2200.0 + (baseFreq - 110.0) * 6.0;
    // Body ring (audible): the head cracks briefly at this tom's pitch.
    a.active = true; a.isNoise = false; a.oscType = kOscSine;
    a.freqStart = baseFreq; a.freqEnd = baseFreq * 0.75; a.freqRampDur = 0.10;
    a.gainPeak = 0.6 * velocity01; a.gainDecayDur = 0.14;
    a.stopTime = 0.16;
    // Rim click on top.
    b.active = true; b.isNoise = true;
    b.filterType = kFilterBandpass; b.filterFreq = clickFreq; b.filterQ = 3.5;
    b.gainPeak = 0.4 * velocity01; b.gainDecayDur = 0.03;
    b.stopTime = 0.05;
  }

  Layer mLayerA, mLayerB;
  BiquadFilter mFilterA, mFilterB;
  double mPhaseA = 0.0, mPhaseB = 0.0;
  double mElapsed = 0.0;
  double mStopTime = 0.0;
  bool   mActive = false;
  uint32_t mSeed = 0x9e3779b9u;
  int    mTargetChannel = static_cast<int>(MixerChannel::Custom); // mixer routing (see SetTargetChannel)
};

} // namespace dgg