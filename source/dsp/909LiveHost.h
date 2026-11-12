// TR-909 fast voices: the live first hit of a cache miss, computed on the audio thread like the live hits of the 808 voices.
//
// A hit whose set is not baked (and that has no preview sample) is computed live by the voice itself while it plays (Tr909LiveSource: the model of the voice at the pot state of the hit); however long the model takes,
// the hit plays what it computes. There is one live source (one hit is computed at a time): a new live hit takes it over, the hit that had it ends where it is.
#pragma once
#include "909BakeHost.h"
#include "909LiveRelease.h"
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

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
  // Builds the tables of the PCM voices of the live source (init thread, before hits play).
  void Prebuild(int sampleRateHz) { if (Tr909LiveSource* s = mSrc.load(std::memory_order_acquire)) s->Prebuild(sampleRateHz); }

  // Starts the live hit of `owner` (builds the model of the pot state if it is not built yet: this takes as long as it takes).
  bool Start(const void* owner, const Tr909SetKey& key, uint64_t seed, double seconds, uint64_t streamKnot = ~0ULL)   // streamKnot: position of the trigger in the shared noise stream (~0: own sequence by `seed`)
  {
    Tr909LiveSource* s = mSrc.load(std::memory_order_acquire); if (!s) return false;
    if (!s->Warm(key.voice, key.accentCode, key.noise, key.params)) return false;
    s->SetStreamKnot(streamKnot);
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

// A bank of pre-built live sources for the PCM voices (hi-hats, crash, ride): each hit takes a free source, computes its own samples while it plays and returns the source at its end, so these voices
// need no cache and several hits run at the same time. The sources are built on a worker thread (the first start of a source builds the slot of the model, about 100 ms); while the bank is not ready
// or all sources of the voice are busy, Acquire gives nullptr and the voice uses the cache path.
class Tr909LiveBank
{
public:
  static Tr909LiveBank& Instance() { static Tr909LiveBank* inst = new Tr909LiveBank(); return *inst; }   // never destroyed
  // Builds the sources once (idempotent); returns at once, the bank is ready when the thread has finished.
  // fShared (optional): the factory of the sources that run hits from prepared sets (BD in the shared noise stream, several at the same time); the thread keeps running as the preparation worker of these sets.
  void BuildAsync(Tr909LiveSynth::Factory f, int sampleRateHz, Tr909LiveSynth::Factory fShared = nullptr)
  {
    if ((f == nullptr && fShared == nullptr) || mStarted.exchange(true)) return;
    std::thread([this, f, sampleRateHz, fShared]
    {
      // the hi-hat circuit exists once: a new hit takes over the source of the running one (retrigger), as the PCM counter of the 909 restarts
      const int counts[] = { 1 /*Hh*/, 1 /*Ho*/, 4 /*Cr*/, 4 /*Ri*/ }; const Tr909Voice voices[] = { Tr909Voice::Hh, Tr909Voice::Ho, Tr909Voice::Cr, Tr909Voice::Ri };
      for (int i = 0; i < (f ? 4 : 0); ++i)
        for (int n = 0; n < counts[i]; ++n)
        {
          std::unique_ptr<Entry> e(new Entry()); e->src = f();
          e->src->Start(voices[i], 127, Tr909NoisePath::Reference5us, 1, sampleRateHz, 0.01, 0);   // builds the slot of the model (and the idle tables) off the audio thread
          mEntries[static_cast<int>(voices[i])].push_back(std::move(e));
        }
      if (fShared) { for (const Tr909Voice sv : { Tr909Voice::Bd, Tr909Voice::Lt, Tr909Voice::Mt, Tr909Voice::Ht }) { if (sv != Tr909Voice::Bd && !kTr909LiveTomsEnabled) continue; std::unique_ptr<Entry> e(new Entry()); e->src = fShared(); mEntries[static_cast<int>(sv)].push_back(std::move(e)); } }   // BD and the toms: one source each, monophonic (a new trigger takes it over)
      mReady.store(true, std::memory_order_release);
      if (!fShared) return;
      for (;;)   // the preparation worker: the newest request wins
      {
        Tr909SetKey k; { std::unique_lock<std::mutex> lk(mPrepMutex); mPrepCv.wait(lk, [this] { return mPrepPending; }); k = mPrepKey; mPrepPending = false; }
        auto& v = mEntries[static_cast<int>(k.voice)]; if (!v.empty()) v[0]->src->PrepareLive(k.voice, k.accentCode, k.params, k.sampleRateHz);
        std::lock_guard<std::mutex> lk(mPrepMutex); mPrepBusy = mPrepPending;
      }
    }).detach();
  }
  // Audio thread: are all prepared sets of this hit (anchors, both forms, hit objects) there for exactly these pots and this sample rate?
  bool SharedReady(Tr909Voice v, int accentCode, uint64_t params) const
  {
    const auto& e = mEntries[static_cast<int>(v)]; return Ready() && !e.empty() && e[0]->src->LiveReady(v, accentCode, params);
  }
  // Audio thread: asks the worker to prepare the sets of this hit (never blocks: a busy lock skips the request, the next hit asks again).
  void RequestPrepare(const Tr909SetKey& k)
  {
    std::unique_lock<std::mutex> lk(mPrepMutex, std::try_to_lock); if (!lk.owns_lock()) return;
    if (mPrepBusy && mPrepKey.voice == k.voice && mPrepKey.accentCode == k.accentCode && mPrepKey.params == k.params && mPrepKey.sampleRateHz == k.sampleRateHz) return;   // already being prepared
    mPrepKey = k; mPrepPending = true; mPrepBusy = true; mPrepCv.notify_one();
  }
  bool Ready() const { return mReady.load(std::memory_order_acquire); }
  // A source of the voice for `owner` (audio thread): a free one, else the one that has run longest is taken over (*prevOwner = its previous owner, whose hit ends); nullptr while the bank is not ready.
  Tr909LiveSource* Acquire(Tr909Voice v, const void* owner, const void** prevOwner)
  {
    *prevOwner = nullptr;
    if (!Ready()) return nullptr;
    Entry* oldest = nullptr;
    for (auto& e : mEntries[static_cast<int>(v)])
    {
      if (e->owner == nullptr) { e->owner = owner; e->seq = ++mSeq; return e->src.get(); }
      if (!oldest || e->seq < oldest->seq) oldest = e.get();
    }
    if (!oldest) return nullptr;
    *prevOwner = oldest->owner; oldest->owner = owner; oldest->seq = ++mSeq; return oldest->src.get();
  }
  bool Owns(Tr909Voice v, Tr909LiveSource* s, const void* owner) const
  {
    for (auto& e : mEntries[static_cast<int>(v)]) if (e->src.get() == s) return e->owner == owner;
    return false;
  }
  void Release(Tr909Voice v, Tr909LiveSource* s, const void* owner)
  {
    for (auto& e : mEntries[static_cast<int>(v)]) if (e->src.get() == s) { if (e->owner == owner) e->owner = nullptr; return; }
  }
private:
  struct Entry { std::unique_ptr<Tr909LiveSource> src; const void* owner = nullptr; unsigned long long seq = 0; };
  unsigned long long mSeq = 0;
  std::mutex mPrepMutex; std::condition_variable mPrepCv; Tr909SetKey mPrepKey; bool mPrepPending = false, mPrepBusy = false;
  std::vector<std::unique_ptr<Entry>> mEntries[static_cast<int>(Tr909Voice::kCount)]; std::atomic<bool> mStarted{false}, mReady{false};
};

}  // namespace dgg
