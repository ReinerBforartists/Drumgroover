// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Reiner Prokein
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>

namespace dgg {

// VoicePool holds up to N simultaneously sounding instances of a DrumVoice
// subclass. It automatically selects a free voice on each Trigger(); if all
// are active, the oldest voice is stolen (standard polyphonic voice stealing).
// Polyphony can be changed at runtime via SetPolyphony() (1..N) without
// allocation in the audio thread; voices beyond the current polyphony limit
// are never triggered and not processed.
//
// Choke behaviour (e.g. a closed hi-hat cutting the open one, a cymbal mute cutting the crash) is done by the
// owner via ChokeAll() on the pool to be cut.
template <typename VoiceT, int N>
class VoicePool
{
public:
  VoicePool() { mGain.fill(1.0); }

  void SetSampleRate(double sr)
  {
    for (auto& v : mVoices) v.SetSampleRate(sr);
  }

  // Polyphony range: [1, N]. Atomic because SetPolyphony() is called from the
  // UI thread while Trigger()/Process() run in the audio thread.
  void SetPolyphony(int n)
  {
    mPolyphony.store(n < 1 ? 1 : (n > N ? N : n), std::memory_order_relaxed);
  }

  int GetPolyphony() const { return mPolyphony.load(std::memory_order_relaxed); }

  // Linear output gain of the NEXT Trigger() (the voice that call picks); applied to that voice's whole output, independent of the velocity.
  // Resets to 1.0 after the trigger.
  void SetNextGain(double g) { mNextGain = g; }

  // Triggers a voice with variadic arguments forwarded to VoiceT::Trigger().
  // Returns the index of the voice that was triggered (mVoices[idx]), so
  // callers can inspect that specific voice right after (e.g. debug/diag
  // accessors) without guessing which one the pool picked.
  template <typename... Args>
  int Trigger(Args&&... args)
  {
    const int poly = mPolyphony.load(std::memory_order_relaxed);

    int idx = -1;
    for (int i = 0; i < poly; i++)
    {
      if (!mVoices[i].IsActive()) { idx = i; break; }
    }

    if (idx < 0)
    {
      // All voices busy: reuse the oldest (smallest trigger order).
      idx = 0;
      for (int i = 1; i < poly; i++)
        if (mTriggerOrder[i] < mTriggerOrder[idx]) idx = i;
    }

    mVoices[idx].Trigger(std::forward<Args>(args)...);
    mGain[idx] = mNextGain;
    mNextGain = 1.0;
    mTriggerOrder[idx] = mNextOrder++;
    return idx;
  }

  // Sums the output of all active voices (up to mPolyphony) into the given buffer.
  void Process(double* out, int nFrames)
  {
    const int poly = mPolyphony.load(std::memory_order_relaxed);
    for (int i = 0; i < poly; i++)
    {
      const double g = mGain[i];
      if (g == 1.0) { mVoices[i].Process(out, nFrames); continue; }
      if (!mVoices[i].IsActive()) continue;
      for (int pos = 0; pos < nFrames; pos += kScratchFrames)   // the voice adds into a zeroed scratch, scaled by its gain into out
      {
        const int n = std::min(kScratchFrames, nFrames - pos);
        std::fill(mScratch.begin(), mScratch.begin() + n, 0.0);
        mVoices[i].Process(mScratch.data(), n);
        for (int j = 0; j < n; j++) out[pos + j] += g * mScratch[j];
      }
    }
  }

  // Access to all N voices, used for pre‑initialisation of parameters that must
  // apply to all possible voices before polyphony is increased.
  std::array<VoiceT, N>& Voices() { return mVoices; }
  const std::array<VoiceT, N>& Voices() const { return mVoices; }

  // Chokes every currently active voice in the pool. Only compiles when VoiceT
  // provides a Choke() method; for other voice types this function is never
  // instantiated.
  void ChokeAll()
  {
    for (auto& v : mVoices)
      if (v.IsActive()) v.Choke();
  }

private:
  static constexpr int kScratchFrames = 1024;
  std::array<VoiceT, N> mVoices;
  std::array<double, N> mGain;                // per-voice linear output gain (see SetNextGain)
  double mNextGain = 1.0;
  std::array<double, kScratchFrames> mScratch{};
  std::array<uint64_t, N> mTriggerOrder{};   // per‑voice trigger counter
  uint64_t mNextOrder = 0;
  std::atomic<int> mPolyphony{ N };
};

} // namespace dgg