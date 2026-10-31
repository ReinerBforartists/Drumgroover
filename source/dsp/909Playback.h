// TR-909 fast voices: playback of baked sets.
//
// A hit looks up the baked variants of its set (voice, knob state, accent code, noise path, sample rate, model version) in the pool and plays
// one at random. There is no live synthesis for these voices: a hit without a baked set is silent, is counted, and its set is put in the miss
// queue, which the plugin's cache drain turns into a bake.
//
// Fixed knob state: the models are the Nava circuit at its fixed knob positions; the knob state of the key (`params`) is always 0.
#pragma once
#include "909AccentPlan.h"
#include "808VariantPoolCache.h"
#include "DrumVoice.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>

namespace dgg {

struct Tr909SetKeyHash
{
  size_t operator()(const Tr909SetKey& k) const
  {
    uint64_t h = k.params * 0x9E3779B97F4A7C15ULL;
    h ^= (static_cast<uint64_t>(k.voice) << 56) ^ (static_cast<uint64_t>(k.accentCode) << 40) ^ (static_cast<uint64_t>(k.noise) << 36) ^ (static_cast<uint64_t>(k.sampleRateHz) << 8) ^ static_cast<uint64_t>(k.modelVersion);
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

class Tr909CachedVoice : public DrumVoice
{
public:
  explicit Tr909CachedVoice(Tr909Voice v = Tr909Voice::Bd) : mVoice(v) {}
  Tr909Voice voice() const { return mVoice; }
  void SetVoice(Tr909Voice v) { mVoice = v; }
  void SetNoisePath(Tr909NoisePath p) { mNoise = p; }

  void Trigger(double velocity01) override
  {
    Tr909SetKey k; k.voice = mVoice; k.params = 0; k.noise = mNoise; k.sampleRateHz = static_cast<int>(std::lround(mSampleRate));
    k.accentCode = Tr909CodeForVelocity(mVoice, velocity01);
    mBuf = Tr909VariantPool::Instance().GetRandom(k); mPos = 0; mFrac = 0.0;
    mRate = std::clamp(pitchRatio, 0.25, 4.0);
    mLevelGain = std::clamp(levelKnob, 0.0, 1.0) / kLevelKnobBaked;
    if (!mBuf) { ++mMisses; Tr909MissQueue::Instance().Push(k); }
  }
  void Process(double* out, int nFrames) override
  {
    if (!mBuf) return;
    const std::vector<float>& b = *mBuf; const size_t n = b.size();
    if (std::fabs(mRate - 1.0) < 1e-12)
    {
      for (int i = 0; i < nFrames && mPos < n; ++i, ++mPos) out[i] += kTr909VoltToSample * mLevelGain * static_cast<double>(b[mPos]);
    }
    else
    {
      // transposition: the baked set is read at pitchRatio samples per output sample (Catmull-Rom between the neighbours)
      auto at = [&](long j) { return (j < 0 || j >= static_cast<long>(n)) ? 0.0 : static_cast<double>(b[static_cast<size_t>(j)]); };
      for (int i = 0; i < nFrames && mPos < n; ++i)
      {
        const long j = static_cast<long>(mPos); const double t = mFrac;
        const double y0 = at(j - 1), y1 = at(j), y2 = at(j + 1), y3 = at(j + 2);
        out[i] += kTr909VoltToSample * mLevelGain * (y1 + 0.5 * t * (y2 - y0 + t * (2.0 * y0 - 5.0 * y1 + 4.0 * y2 - y3 + t * (3.0 * (y1 - y2) + y3 - y0))));
        mFrac += mRate;
        const size_t adv = static_cast<size_t>(mFrac); mPos += adv; mFrac -= static_cast<double>(adv);
      }
    }
    if (mPos >= n) mBuf.reset();
  }
  bool IsActive() const override { return mBuf != nullptr; }
  void Choke() { mBuf.reset(); }
  unsigned misses() const { return mMisses; }

  double pitchRatio = 1.0;   // digital transpose of the baked set (not a circuit knob)
  // Output level pot of the voice (0..1). The sets are baked at kLevelKnobBaked; the pot is the feedback resistor of the output op-amp, which scales the output
  // exactly (Hand Clap: engine at 0.25 / 0.5 / 1.0 gives gains 0.500 / 1 / 2.001, no clipping), so the level is a gain on the baked set.
  double levelKnob = 0.5;
  static constexpr double kLevelKnobBaked = 0.5;

private:
  Tr909Voice mVoice; Tr909NoisePath mNoise = Tr909NoisePath::Reference5us;
  std::shared_ptr<const std::vector<float>> mBuf; size_t mPos = 0; unsigned mMisses = 0;
  double mFrac = 0.0, mRate = 1.0, mLevelGain = 1.0;
};

}  // namespace dgg
