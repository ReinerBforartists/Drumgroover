// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Reiner Prokein
#pragma once
// =========================================
// DrumVoice — Base class for the internal sound engine
// =========================================
// A "Voice" corresponds to an instrument family (Kick, Snare, Hi-Hat, ...),
// not to a single articulation from DemoTracks() in DrumGroover.cpp: those
// track roles are used for MIDI export / GM mapping, the sound engine groups
// them into a few voice families (Kick, Snare, Hi-Hat, Tom, Cymbal, Ride).
//
// Pure synthesis (oscillators + noise + filters + envelopes), no sample
// playback.
//
// A voice instance is reusable (Trigger() resets state, no new/delete
// in the audio thread). Overlapping hits are handled by VoicePool, which
// holds several instances of a voice.

#include <cmath>
#include <cstdint>

namespace dgg {

// Exponential ramp — exactly mimics the Web Audio behavior of
// AudioParam.exponentialRampToValueAtTime:
//   value(t) = v0 * (v1/v0)^(t/duration), t clamped to [0, duration],
//   after which the value holds at v1.
// v0 and v1 must both be > 0 (Web Audio throws an error for 0/negative
// target values for the same reason — intentionally not safeguarded here,
// see caller defaults which are all > 0).
inline double ExpRamp(double v0, double v1, double t, double duration)
{
  if (duration <= 0.0) return v1;
  double tt = t < 0.0 ? 0.0 : (t > duration ? duration : t);
  return v0 * std::pow(v1 / v0, tt / duration);
}

// Simple, fast white noise generator (xorshift32) — intentionally not an
// exact bit-for-bit equivalent to JS' Math.random() (wouldn't be audibly
// distinguishable for noise anyway), just fast and allocation-free for
// the audio thread. Shared here because
// HiHat808Voice needs the same generator — both headers already include
// DrumVoice.h anyway.
class WhiteNoise
{
public:
  double Next()
  {
    mState ^= mState << 13;
    mState ^= mState >> 17;
    mState ^= mState << 5;
    // uint32 -> [-1, 1)
    return (static_cast<double>(mState) / 2147483648.0) - 1.0;
  }

private:
  uint32_t mState = 0x9e3779b9u; // fix: must not be 0 (xorshift freezes on 0)
};

class DrumVoice
{
public:
  virtual ~DrumVoice() = default;

  virtual void SetSampleRate(double sr) { mSampleRate = sr; }

  // velocity01: 0..1, normalized — matches the (vel/127) convention
  // used everywhere in JS (see audio-engine.js).
  virtual void Trigger(double velocity01) = 0;

  // Renders nFrames mono samples and ADDS them to out (does not overwrite)
  // — caller mixes multiple voices into the same buffer.
  // Must be safely callable every block even when inactive
  // (then becomes a no-op).
  virtual void Process(double* out, int nFrames) = 0;

  virtual bool IsActive() const = 0;

protected:
  double mSampleRate = 44100.0;
};

} // namespace dgg
