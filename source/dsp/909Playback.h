// TR-909 fast voices: playback of baked sets.
//
// A hit looks up the baked variants of its set (voice, knob state, accent code, noise path, sample rate, model version) in the pool and plays
// one at random. There is no live synthesis for these voices: a hit without a baked set is silent, is counted, and its set is put in the miss
// queue, which the plugin's cache drain turns into a bake.
//
// Fixed knob state: the models are the Nava circuit at its fixed knob positions; the knob state of the key (`params`) is always 0.
#pragma once
#include "909AccentPlan.h"
#include "909PotParams.h"
#include "909LiveHost.h"
#include "808VariantPoolCache.h"
#include "DrumVoice.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <string>

namespace dgg {

struct Tr909SetKeyHash
{
  size_t operator()(const Tr909SetKey& k) const
  {
    uint64_t h = k.params * 0x9E3779B97F4A7C15ULL;
    h ^= (static_cast<uint64_t>(k.voice) << 56) ^ (static_cast<uint64_t>(k.accentCode) << 40) ^ (static_cast<uint64_t>(k.noise) << 36) ^ (static_cast<uint64_t>(k.sampleRateHz) << 8);
    h ^= h >> 33; h *= 0xFF51AFD7ED558CCDULL; h ^= h >> 33;
    return static_cast<size_t>(h);
  }
};

class Tr909VariantPool : public TR808VariantPoolCache<Tr909SetKey, Tr909SetKeyHash>
{
public:
  static Tr909VariantPool& Instance() { static Tr909VariantPool inst; return inst; }
};

// Voltage of the circuit output to sample value: the working scale of the project is reference dBFS = deck dBV (tools handoff R47).
constexpr double kTr909VoltToSample = 1.0;

// Accent codes of a voice whose accent is validated (Tr909AccentRuns gives runs), ascending; returns how many.
inline int Tr909ValidCodes(Tr909Voice v, int out[kTr909MaxAccentCode + 1])
{
  int n = 0; Tr909AccentRun r[2];
  for (int c = 0; c <= kTr909MaxAccentCode; ++c) if (Tr909AccentRuns(v, c, r) > 0) out[n++] = c;
  return n;
}

// The accent codes a voice plays are spread over its validated codes, not over 0..127: the velocity range 0..1 covers exactly the supported
// accent range (the quietest hit is the lowest validated code, the loudest code 127) and the level rises steadily with the velocity, without a
// jump into a range that has no model. The velocity is quantised to kTr909VelocityLevels levels, so a voice uses at most that many sets.
constexpr int kTr909VelocityLevels = 8;
inline int Tr909CodeForVelocity(Tr909Voice v, double velocity01)
{
  int codes[kTr909MaxAccentCode + 1]; const int n = Tr909ValidCodes(v, codes);
  const int level = static_cast<int>(std::lround(std::clamp(velocity01, 0.0, 1.0) * (kTr909VelocityLevels - 1)));
  return codes[static_cast<int>(std::lround(static_cast<double>(level) * (n - 1) / (kTr909VelocityLevels - 1)))];
}

// Sets that a hit found missing: written by the audio thread, read by the cache drain (one producer, one consumer, no locks).
class Tr909MissQueue
{
public:
  static Tr909MissQueue& Instance() { static Tr909MissQueue q; return q; }
  bool Push(const Tr909SetKey& k)
  {
    const unsigned t = mTail.load(std::memory_order_relaxed), n = (t + 1) % kSize;
    if (n == mHead.load(std::memory_order_acquire)) return false;   // full: the miss is dropped, the next hit of the set reports it again
    mItems[t] = k; mTail.store(n, std::memory_order_release); return true;
  }
  bool Pop(Tr909SetKey& k)
  {
    const unsigned h = mHead.load(std::memory_order_relaxed);
    if (h == mTail.load(std::memory_order_acquire)) return false;
    k = mItems[h]; mHead.store((h + 1) % kSize, std::memory_order_release); return true;
  }
private:
  static constexpr unsigned kSize = 64;
  Tr909SetKey mItems[kSize]; std::atomic<unsigned> mHead{0}, mTail{0};
};

// The preview sample of a knob drag (not part of the cache): one per voice, the sound of the pot state the knob is at, played by every hit of that pot state until another preview sample replaces it.
// It is baked in the background while the knob moves (one hit) and is independent of the cache sets, which are baked after the release.
class Tr909PreviewSample
{
public:
  static Tr909PreviewSample& Instance() { static Tr909PreviewSample inst; return inst; }
  void Set(Tr909Voice v, uint64_t params, std::vector<float> samples)
  {
    auto buf = std::make_shared<const std::vector<float>>(std::move(samples));
    std::lock_guard<std::mutex> lock(mMutex); mSlot[static_cast<int>(v)] = { params, std::move(buf) };
  }
  // The preview sample of the voice when it was made for this pot state, else nullptr.
  std::shared_ptr<const std::vector<float>> Get(Tr909Voice v, uint64_t params)
  {
    std::lock_guard<std::mutex> lock(mMutex); const Slot& s = mSlot[static_cast<int>(v)];
    return s.buf && s.params == params ? s.buf : nullptr;
  }
  void Clear() { std::lock_guard<std::mutex> lock(mMutex); for (Slot& s : mSlot) s = Slot(); }
private:
  struct Slot { uint64_t params = 0; std::shared_ptr<const std::vector<float>> buf; };
  std::mutex mMutex; Slot mSlot[static_cast<int>(Tr909Voice::kCount)];
};

// True while a circuit knob of a 909 voice is being moved (set by the plugin on every knob edit, cleared on the release): hits then play the temporary sample of the current pot state, never a cache set.
inline std::atomic<bool>& Tr909KnobEditing() { static std::atomic<bool> f{false}; return f; }

class Tr909CachedVoice : public DrumVoice
{
public:
  explicit Tr909CachedVoice(Tr909Voice v = Tr909Voice::Bd) : mVoice(v) {}
  Tr909Voice voice() const { return mVoice; }
  void SetVoice(Tr909Voice v) { mVoice = v; }
  void SetNoisePath(Tr909NoisePath p) { mNoise = p; }

  ~Tr909CachedVoice() override { EndHit(); }
  void Trigger(double velocity01) override
  {
    std::vector<float> carry; if (mBankSrc && mPos < mLiveBuf.size()) carry.assign(mLiveBuf.begin() + static_cast<std::ptrdiff_t>(mPos), mLiveBuf.end());   // samples this voice had computed but not played: a retrigger of the same voice keeps them
    EndHit();
    Tr909SetKey k; k.voice = mVoice; k.params = Params(); k.noise = mNoise; k.sampleRateHz = static_cast<int>(std::lround(mSampleRate));
    k.accentCode = Tr909CodeForVelocity(mVoice, velocity01);
    if (Tr909IsPcmVoice(mVoice))   // hi-hats, crash, ride: computed live by a source of the bank, no cache (falls back to the cache path while the bank is not ready or all its sources are busy)
    {
      mRate = std::clamp(pitchRatio * basePitch, 0.25, 4.0); mPos = 0; mFrac = 0.0;
      const void* prev = nullptr;
      if (Tr909LiveSource* s = Tr909LiveBank::Instance().Acquire(mVoice, this, &prev))
      {
        mLiveBuf.clear(); mLiveDone = false;
        if (prev) static_cast<Tr909CachedVoice*>(const_cast<void*>(prev))->HandOverTo(*this);   // the hit that ran on the source ends; what it had computed and not played continues here
        else mLiveBuf.assign(carry.begin(), carry.end());
        if (s->Start(k.voice, k.accentCode, k.noise, 1, k.sampleRateHz, Tr909HitSeconds(mVoice), k.params)) { mBankSrc = s; mLiveOn = true; mLiveBuf.reserve(static_cast<size_t>(mSampleRate * (Tr909HitSeconds(mVoice) + 0.05))); return; }
        Tr909LiveBank::Instance().Release(mVoice, s, this); mLiveBuf.clear();
      }
    }
    // Only the factory pot state (params 0: every circuit pot at its nominal position) lives in the cache. A pot state set with a knob never does: its first hit is computed live, the following hits play the
    // temporary sample of that pot state (in memory, one per voice) or are computed live again (another velocity than the temporary sample, or the sample is not ready yet).
    const bool cacheState = k.params == 0 && !Tr909KnobEditing().load(std::memory_order_relaxed);
    mBuf = cacheState ? Tr909VariantPool::Instance().GetRandom(k) : nullptr; mPos = 0; mFrac = 0.0;
    mRate = std::clamp(pitchRatio * basePitch, 0.25, 4.0);
    if (!mBuf)
    {
      // No baked set: cache state: the miss is reported so that the set gets baked; edited state: so that the temporary sample gets baked (once, when none of this pot state exists).
      mBuf = Tr909PreviewSample::Instance().Get(mVoice, k.params);   // (the PCM voices have no noise: a preview sample is the hit)
      if (cacheState || !mBuf) { ++mMisses; Tr909MissQueue::Instance().Push(k); }
    }
    if (!mBuf)
    {
      // The first hit is computed live, on the audio thread (909LiveHost.h), however long the model takes.
      static std::atomic<unsigned> liveCounter{0};
      mLiveBuf.clear(); mLiveDone = false;
      mLiveOn = Tr909LiveSynth::Instance().Start(this, k, Tr909NoiseSeed(1 + static_cast<int>(liveCounter.fetch_add(1, std::memory_order_relaxed) % 7)), Tr909HitSeconds(mVoice));
      if (mLiveOn) mLiveBuf.reserve(static_cast<size_t>(mSampleRate * (Tr909HitSeconds(mVoice) + 0.05)));
    }
  }
  void Process(double* out, int nFrames) override
  {
    const float* b; size_t n; bool complete;
    if (mBuf) { b = mBuf->data(); n = mBuf->size(); complete = true; }
    else if (mLiveOn)
    {
      // the live hit computes the samples this block needs
      const size_t need = mPos + static_cast<size_t>(nFrames * std::max(1.0, mRate)) + 8;
      while (!mLiveDone && mLiveBuf.size() < need)
      {
        const int chunk = mBankSrc ? 64 : 256;   // a bank source is pulled in small chunks: the first block of a hit computes no more than it needs
        float tmp[256]; const int g = mBankSrc ? mBankSrc->Pull(tmp, chunk) : Tr909LiveSynth::Instance().Pull(this, tmp, chunk);
        mLiveBuf.insert(mLiveBuf.end(), tmp, tmp + g);
        if (g < chunk) mLiveDone = true;
      }
      b = mLiveBuf.data(); n = mLiveBuf.size(); complete = mLiveDone;
    }
    else return;
    if (std::fabs(mRate - 1.0) < 1e-12)
    {
      for (int i = 0; i < nFrames && mPos < n; ++i, ++mPos) out[i] += kTr909VoltToSample * static_cast<double>(b[mPos]);
    }
    else
    {
      // transposition: the baked set is read at pitchRatio samples per output sample (Catmull-Rom between the neighbours)
      auto at = [&](long j) { return (j < 0 || j >= static_cast<long>(n)) ? 0.0 : static_cast<double>(b[static_cast<size_t>(j)]); };
      for (int i = 0; i < nFrames && mPos < n; ++i)
      {
        const long j = static_cast<long>(mPos); const double t = mFrac;
        if (!complete && static_cast<size_t>(j) + 2 >= n) break;
        const double y0 = at(j - 1), y1 = at(j), y2 = at(j + 1), y3 = at(j + 2);
        out[i] += kTr909VoltToSample * (y1 + 0.5 * t * (y2 - y0 + t * (2.0 * y0 - 5.0 * y1 + 4.0 * y2 - y3 + t * (3.0 * (y1 - y2) + y3 - y0))));
        mFrac += mRate;
        const size_t adv = static_cast<size_t>(mFrac); mPos += adv; mFrac -= static_cast<double>(adv);
      }
    }
    if (mPos >= n && complete) EndHit();
  }
  bool IsActive() const override { return mBuf != nullptr || mLiveOn; }
  void Choke() { EndHit(); }
  unsigned misses() const { return mMisses; }

  // Circuit pots of the voice (0..1, 909PotParams.h): a set is baked for one pot state, the state is part of the set key. Level is the output pot of the circuit (VR12 / HC VR20), not a gain.
  double tuneKnob = 0.5, decayKnob = 0.5, attackKnob = 0.5, toneKnob = 0.5, snappyKnob = 0.5, levelKnob = 0.5;
  // The pots in the order of the generator (909PotParams.h).
  void PotValues(double* p) const
  {
    switch (mVoice)
    {
      case Tr909Voice::Lt: case Tr909Voice::Mt: case Tr909Voice::Ht: p[0] = tuneKnob; p[1] = decayKnob; p[2] = levelKnob; break;
      case Tr909Voice::Bd: p[0] = tuneKnob; p[1] = attackKnob; p[2] = levelKnob; p[3] = decayKnob; break;
      case Tr909Voice::Sd: p[0] = tuneKnob; p[1] = toneKnob; p[2] = levelKnob; p[3] = snappyKnob; break;
      case Tr909Voice::Hc: p[0] = levelKnob; break;
      case Tr909Voice::Cr: case Tr909Voice::Ri: p[0] = levelKnob; p[1] = tuneKnob; break;
      case Tr909Voice::Hh: case Tr909Voice::Ho: p[0] = levelKnob; p[1] = decayKnob; break;   // Level VR22; Decay VR21 (Closed) / VR23 (Open)
      default: break;
    }
  }
  uint64_t Params() const { double p[8] = {0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5}; PotValues(p); return Tr909PackPots(mVoice, p); }
  // Card keys of the 909 circuit pots (909Kit.h Tr909ControlsJson): own keys, so that the knobs of the 808 circuit of the same row keep their values.
  static bool IsPotName(const std::string& n) { return n == "tune909" || n == "decay909" || n == "attack909" || n == "tone909" || n == "snappy909" || n == "level909"; }
  // Sets a circuit pot by its card key; false for a key that is not a pot (the caller handles Pitch).
  bool SetPot(const std::string& name, double v)
  {
    v = std::clamp(v, 0.0, 1.0);
    if (name == "tune909") tuneKnob = v;
    else if (name == "decay909") decayKnob = v;
    else if (name == "attack909") attackKnob = v;
    else if (name == "tone909") toneKnob = v;
    else if (name == "snappy909") snappyKnob = v;
    else if (name == "level909") levelKnob = v;
    else return false;
    return true;
  }
  double pitchRatio = 1.0;   // digital transpose of the baked set (not a circuit knob)
  double basePitch = 1.0;    // fixed transposition of the instrument (the floor toms are the low tom pitched down, 909Kit.h); the Pitch knob acts on top of it
private:
  Tr909Voice mVoice; Tr909NoisePath mNoise = Tr909NoisePath::Reference5us;
  void EndHit() { mBuf.reset(); if (mBankSrc) { Tr909LiveBank::Instance().Release(mVoice, mBankSrc, this); mBankSrc = nullptr; mLiveOn = false; } else if (mLiveOn) { Tr909LiveSynth::Instance().Release(this); mLiveOn = false; } }
  // Another voice takes over the bank source this hit runs on: it continues with the samples this hit had computed and not played yet; this hit ends.
  void HandOverTo(Tr909CachedVoice& next)
  {
    if (mPos < mLiveBuf.size()) next.mLiveBuf.assign(mLiveBuf.begin() + static_cast<std::ptrdiff_t>(mPos), mLiveBuf.end());
    mBankSrc = nullptr; mLiveOn = false; mLiveBuf.clear(); mPos = 0; mFrac = 0.0;
  }
  Tr909LiveSource* mBankSrc = nullptr;   // the source of the live bank this hit runs on (PCM voices)
  std::shared_ptr<const std::vector<float>> mBuf; std::vector<float> mLiveBuf; bool mLiveOn = false, mLiveDone = false; size_t mPos = 0; unsigned mMisses = 0;
  double mFrac = 0.0, mRate = 1.0;
};

}  // namespace dgg
