// TR-909 fast voices: the live first hit of a cache miss, computed on the audio thread like the live hits of the 808 voices.
//
// A hit whose set is not baked (and that has no preview sample) is computed live by the voice itself while it plays (Tr909LiveSource: the model of the voice at the pot state of the hit); however long the model takes,
// the hit plays what it computes. There is one live source (one hit is computed at a time): a new live hit takes it over, the hit that had it ends where it is.
#pragma once
#include "909BakeHost.h"
#include <atomic>
#include <memory>

namespace dgg {

class Tr909LiveSynth
{
public:
  using Factory = std::unique_ptr<Tr909LiveSource> (*)();
  static Tr909LiveSynth& Instance() { static Tr909LiveSynth* inst = new Tr909LiveSynth(); return *inst; }   // never destroyed

  // Creates the live source (once, from the init thread).
  void Enable(Factory f)
  {
    if (mSrc.load(std::memory_order_acquire) != nullptr || f == nullptr) return;
    Tr909LiveSource* s = f().release(); Tr909LiveSource* none = nullptr;
    if (!mSrc.compare_exchange_strong(none, s)) delete s;
  }
  bool Enabled() const { return mSrc.load(std::memory_order_acquire) != nullptr; }

  // Starts the live hit of `owner` (builds the model of the pot state if it is not built yet: this takes as long as it takes).
  bool Start(const void* owner, const Tr909SetKey& key, uint64_t seed, double seconds)
  {
    Tr909LiveSource* s = mSrc.load(std::memory_order_acquire); if (!s) return false;
    if (!s->Warm(key.voice, key.accentCode, key.noise, key.params)) return false;
    if (!s->Start(key.voice, key.accentCode, key.noise, seed, key.sampleRateHz, seconds, key.params)) return false;
    mOwner = owner; return true;
  }
  // The next samples of the hit of `owner`; fewer than n at its end (or 0 when the live source was taken over by another hit).
  int Pull(const void* owner, float* out, int n)
  {
    Tr909LiveSource* s = mSrc.load(std::memory_order_acquire);
    if (!s || mOwner != owner) return 0;
    return s->Pull(out, n);
  }
  void Release(const void* owner) { if (mOwner == owner) mOwner = nullptr; }

private:
  std::atomic<Tr909LiveSource*> mSrc{nullptr}; const void* mOwner = nullptr;
};

}  // namespace dgg
