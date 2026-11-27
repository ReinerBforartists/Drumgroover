// TR-909 fast voices: the live first hit of a cache miss, computed on the audio thread like the live hits of the 808 voices.
//
// A hit whose set is not baked (and that has no preview sample) is computed live by the voice itself while it plays (LiveSource909: the model of the voice at the pot state of the hit); however long the model takes,
// the hit plays what it computes. As in the 909 every voice has its own circuit: one live source per voice. A new live hit of a voice takes its source over (retrigger: the analog state continues, the earlier hit ends),
// hits of different voices run side by side. A source whose hit nobody plays any more (choked, or the voice plays a baked sample now) keeps running in time (Advance, called after every block), as the circuit does:
// the next live hit of the voice finds the state of that moment.
#pragma once
#include "909BakeHost.h"
#include "909NoiseStream.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace dgg {

// Time of a live source on the sample clock of the host (the positions of NoiseStream909).
struct LiveClock909
{
  uint64_t at = 0;   // host sample up to which the source has run
  // Brings the source to the host sample `to`. played: its owner has pulled the samples up to there; else the samples are computed here and dropped.
  void RunTo(LiveSource909& s, bool played, uint64_t to)
  {
    if (to <= at) return;
    if (!played)
    {
      float drop[256];
      for (uint64_t left = to - at; left > 0 && s.Active();)
      {
        const int n = static_cast<int>(std::min<uint64_t>(left, 256));
        if (s.Pull(drop, n) < n) break;
        left -= static_cast<uint64_t>(n);
      }
    }
    at = to;
  }
};

class LiveSynth909
{
public:
  using Factory = std::unique_ptr<LiveSource909> (*)();
  static LiveSynth909& Instance() { static LiveSynth909* inst = new LiveSynth909(); return *inst; }   // never destroyed

  // Creates the live sources, one per voice (once, from the init thread).
  void Enable(Factory f)
  {
    if (f == nullptr) return;
    for (Slot& c : mSlot)
    {
      if (c.src.load(std::memory_order_acquire) != nullptr) continue;
      LiveSource909* s = f().release(); LiveSource909* none = nullptr;
      if (!c.src.compare_exchange_strong(none, s)) delete s;
    }
  }
  bool Enabled() const { return mSlot[0].src.load(std::memory_order_acquire) != nullptr; }
  // Builds the tables of the PCM voices of the live sources (init thread, before hits play).
  void Prebuild(int sampleRateHz) { for (int v = 0; v < kVoices; ++v) if (LiveSource909* s = mSlot[v].src.load(std::memory_order_acquire)) s->Prebuild(static_cast<Voice909>(v), sampleRateHz); }

  // Starts the live hit of `owner` (builds the model of the pot state if it is not built yet: this takes as long as it takes).
  bool Start(const void* owner, const SetKey909& key, uint64_t seed, double seconds, uint64_t streamKnot = ~0ULL)   // streamKnot: position of the trigger in the shared noise stream (~0: own sequence by `seed`)
  {
    Slot& c = mSlot[static_cast<int>(key.voice)];
    LiveSource909* s = c.src.load(std::memory_order_acquire); if (!s) return false;
    c.clock.RunTo(*s, c.owner != nullptr, NoiseStream909::Sample());   // a hit nobody plays has run on up to this trigger
    if (!s->Warm(key.voice, key.accentCode, key.noise, key.params)) return false;
    s->SetStreamKnot(streamKnot);
    if (!s->Start(key.voice, key.accentCode, key.noise, seed, key.sampleRateHz, seconds, key.params)) return false;
    c.owner = owner; return true;
  }
  // The next samples of the hit of `owner`; fewer than n at its end (or 0 when another hit of the voice has taken the source over).
  int Pull(const void* owner, float* out, int n)
  {
    for (Slot& c : mSlot) if (c.owner == owner) { LiveSource909* s = c.src.load(std::memory_order_acquire); return s ? s->Pull(out, n) : 0; }
    return 0;
  }
  // `owner` stops playing its hit (at the trigger or block position of NoiseStream909); the source runs on.
  void Release(const void* owner)
  {
    for (Slot& c : mSlot) if (c.owner == owner) { if (LiveSource909* s = c.src.load(std::memory_order_acquire)) c.clock.RunTo(*s, true, NoiseStream909::Sample()); c.owner = nullptr; }
  }
  // Audio thread, after the voices have rendered a block that ends at the host sample `to`: the hits nobody plays run on to there.
  void Advance(uint64_t to) { for (Slot& c : mSlot) if (LiveSource909* s = c.src.load(std::memory_order_acquire)) c.clock.RunTo(*s, c.owner != nullptr, to); }

private:
  static constexpr int kVoices = static_cast<int>(Voice909::kCount);
  struct Slot { std::atomic<LiveSource909*> src{nullptr}; const void* owner = nullptr; LiveClock909 clock; };
  Slot mSlot[kVoices];
};

// A bank of pre-built live sources for the PCM voices (hi-hats, crash, ride): each hit takes a free source, computes its own samples while it plays and returns the source at its end, so these voices
// need no cache and several hits run at the same time. The sources are built on a worker thread (building the slot of the model takes about 100 ms); while the bank is not ready
// or all sources of the voice are busy, Acquire gives nullptr and the voice uses the cache path.
class LiveBank909
{
public:
  static LiveBank909& Instance() { static LiveBank909* inst = new LiveBank909(); return *inst; }   // never destroyed
  // Builds the sources once (idempotent); returns at once, the bank is ready when the thread has finished.
  void BuildAsync(LiveSynth909::Factory f, int sampleRateHz)
  {
    if (f == nullptr || mStarted.exchange(true)) return;
    std::thread([this, f, sampleRateHz]
    {
      // the hi-hat circuit exists once: a new hit takes over the source of the running one (retrigger), as the PCM counter of the 909 restarts
      const int counts[] = { 1 /*Hh*/, 1 /*Ho*/, 4 /*Cr*/, 4 /*Ri*/ }; const Voice909 voices[] = { Voice909::Hh, Voice909::Ho, Voice909::Cr, Voice909::Ri };
      for (int i = 0; i < 4; ++i)
        for (int n = 0; n < counts[i]; ++n)
        {
          std::unique_ptr<Entry> e(new Entry()); e->src = f();
          e->src->Prebuild(voices[i], sampleRateHz);   // builds the slot of the model (and the idle tables) off the audio thread
          mEntries[static_cast<int>(voices[i])].push_back(std::move(e));
        }
      mReady.store(true, std::memory_order_release);
    }).detach();
  }
  bool Ready() const { return mReady.load(std::memory_order_acquire); }
  // A source of the voice for `owner` (audio thread): a free one, else the one that has run longest is taken over (*prevOwner = its previous owner, whose hit ends); nullptr while the bank is not ready.
  LiveSource909* Acquire(Voice909 v, const void* owner, const void** prevOwner)
  {
    *prevOwner = nullptr;
    if (!Ready()) return nullptr;
    Entry* oldest = nullptr;
    for (auto& e : mEntries[static_cast<int>(v)])
    {
      if (e->owner == nullptr) { e->clock.RunTo(*e->src, false, NoiseStream909::Sample()); e->owner = owner; e->seq = ++mSeq; return e->src.get(); }
      if (!oldest || e->seq < oldest->seq) oldest = e.get();
    }
    if (!oldest) return nullptr;
    oldest->clock.RunTo(*oldest->src, true, NoiseStream909::Sample());
    *prevOwner = oldest->owner; oldest->owner = owner; oldest->seq = ++mSeq; return oldest->src.get();
  }
  bool Owns(Voice909 v, LiveSource909* s, const void* owner) const
  {
    for (auto& e : mEntries[static_cast<int>(v)]) if (e->src.get() == s) return e->owner == owner;
    return false;
  }
  void Release(Voice909 v, LiveSource909* s, const void* owner)
  {
    for (auto& e : mEntries[static_cast<int>(v)]) if (e->src.get() == s) { if (e->owner == owner) { e->clock.RunTo(*s, true, NoiseStream909::Sample()); e->owner = nullptr; } return; }
  }
  // As LiveSynth909::Advance.
  void Advance(uint64_t to)
  {
    if (!Ready()) return;
    for (auto& voice : mEntries) for (auto& e : voice) e->clock.RunTo(*e->src, e->owner != nullptr, to);
  }
private:
  struct Entry { std::unique_ptr<LiveSource909> src; const void* owner = nullptr; unsigned long long seq = 0; LiveClock909 clock; };
  unsigned long long mSeq = 0;
  std::vector<std::unique_ptr<Entry>> mEntries[static_cast<int>(Voice909::kCount)]; std::atomic<bool> mStarted{false}, mReady{false};
};

}  // namespace dgg
