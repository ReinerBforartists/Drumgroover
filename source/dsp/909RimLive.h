// TR-909 Rim Shot: live voice (no cache).
//
// The state-carrying full model of the RM circuit (tr909/rm/, 22 level models, one host with a model clock) runs while the voice plays: a hit is computed block by block,
// about 100 times faster than real time. Nothing is baked; the circuit state carries on between hits like the circuit's (a retrigger continues the running voice).
//
// The host is built on a worker thread (the propagators of the 22 models take about 1.4 s and 14 MB) by Prepare(), never on the audio thread. A hit before the host is ready
// is silent and counted. The host renders on every block while the voice is busy (a hit sounds or the finished voice relaxes): its clock is the output clock.
#pragma once
#include "909Playback.h"
#include "DrumVoice.h"
#include "tr909/rm/models/rm_full_levels.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <thread>

namespace dgg {

class Tr909RimLiveVoice : public DrumVoice
{
public:
  using Host = rmfast::FullLevelHost<RmFullLevels>;
  // Accent from the velocity alone (the MIDI path of the Nava firmware: velocity = DAC code), spread over the codes 25 (0.8 V) .. 127 (4.064 V), the range the model is validated for.
  static constexpr int kMinAccentCode = 25;
  static int AccentCodeForVelocity(double velocity01) { return std::clamp(static_cast<int>(std::lround(kMinAccentCode + std::clamp(velocity01, 0.0, 1.0) * (kTr909MaxAccentCode - kMinAccentCode))), kMinAccentCode, kTr909MaxAccentCode); }

  Tr909RimLiveVoice() = default;
  Tr909RimLiveVoice(const Tr909RimLiveVoice&) = delete;
  Tr909RimLiveVoice& operator=(const Tr909RimLiveVoice&) = delete;
  ~Tr909RimLiveVoice() override { if (mThread.joinable()) mThread.join(); delete mCur.load(); }

  // Builds the host for the current sample rate on a worker thread when there is none for it yet; returns at once. Call again after a change of the sample rate.
  void Prepare()
  {
    mWantRate.store(static_cast<int>(std::lround(mSampleRate)), std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(mMutex);
    if (mRunning) return;   // the running build re-checks the wanted rate before it ends
    if (mCur.load() && mCurRate.load() == mWantRate.load()) return;
    if (mThread.joinable()) mThread.join();
    mRunning = true;
    mThread = std::thread([this] { BuildLoop(); });
  }
  bool Ready() const { return HostIfReady() != nullptr; }

  void Trigger(double velocity01) override
  {
    Host* h = HostIfReady();
    if (!h) { ++mMisses; return; }
    h->SetAccent(Tr909AccentVolts(AccentCodeForVelocity(velocity01)));
    ApplyLevel(h);
    h->Trigger();
  }
  void Process(double* out, int nFrames) override
  {
    Host* h = HostIfReady();
    if (!h) return;
    ApplyLevel(h);
    const bool transposed = std::fabs(pitchRatio - 1.0) >= 1e-12;
    if (!transposed)
    {
      mWindowValid = false;
      float buf[kChunk];
      for (int pos = 0; pos < nFrames; pos += kChunk)
      {
        const int n = std::min(kChunk, nFrames - pos);
        h->Render(buf, n);
        for (int i = 0; i < n; ++i) out[pos + i] += kTr909VoltToSample * outputGain * static_cast<double>(buf[i]);
      }
      return;
    }
    // transposition: the model stream is read at pitchRatio samples per output sample (Catmull-Rom between the neighbours); the stream runs on in real time
    const double rate = std::clamp(pitchRatio, 0.25, 4.0);
    auto next = [&] { float s; h->Render(&s, 1); return static_cast<double>(s); };
    if (!mWindowValid) { mW[0] = 0.0; mW[1] = next(); mW[2] = next(); mW[3] = next(); mFrac = 0.0; mWindowValid = true; }
    for (int i = 0; i < nFrames; ++i)
    {
      const double t = mFrac, y0 = mW[0], y1 = mW[1], y2 = mW[2], y3 = mW[3];
      out[i] += kTr909VoltToSample * outputGain * (y1 + 0.5 * t * (y2 - y0 + t * (2.0 * y0 - 5.0 * y1 + 4.0 * y2 - y3 + t * (3.0 * (y1 - y2) + y3 - y0))));
      mFrac += rate;
      while (mFrac >= 1.0) { mFrac -= 1.0; mW[0] = mW[1]; mW[1] = mW[2]; mW[2] = mW[3]; mW[3] = next(); }
    }
  }
  bool IsActive() const override { const Host* h = HostIfReady(); return h && h->Busy(); }
  unsigned misses() const { return mMisses; }

  double pitchRatio = 1.0;   // digital transpose of the stream (not a circuit knob)
  double levelKnob = 0.7;    // the output pot of the circuit (R 17-18 = level x 100 kOhm), 0..1
  // Output trim after the model, not a circuit knob: the raw model output (volts, working scale reference dBFS = deck dBV) is 24.3 dB above the 808 rim shot of the plugin at the same
  // level knob (0.7) and velocity 1.0 (peaks, tools/_sweep/tr909_fast/rim_live/cmp808.cpp: 808 -13.4 dBFS, model +10.9 dBFS), so the 909 rim shot is matched to it. 10^(-24.3 / 20).
  double outputGain = 0.0610;

private:
  static constexpr int kChunk = 256;

  Host* HostIfReady() const
  {
    Host* h = mCur.load(std::memory_order_acquire);
    return (h && mCurRate.load(std::memory_order_acquire) == static_cast<int>(std::lround(mSampleRate))) ? h : nullptr;
  }
  // audio thread: a new host (generation) starts at its default level and without a resampling window
  void Sync() { const unsigned g = mGen.load(std::memory_order_acquire); if (g != mSeenGen) { mSeenGen = g; mAppliedLevel = -1.0; mWindowValid = false; } }
  void ApplyLevel(Host* h) { Sync(); if (levelKnob != mAppliedLevel) { mAppliedLevel = levelKnob; h->SetLevel(std::clamp(levelKnob, 0.0, 1.0)); } }
  void BuildLoop()
  {
    for (;;)
    {
      const int rate = mWantRate.load();
      { std::lock_guard<std::mutex> lk(mMutex); if (mCur.load() && mCurRate.load() == rate) { mRunning = false; return; } }
      std::unique_ptr<Host> h(new Host);
      h->Init(static_cast<double>(rate));
      Host* old = mCur.exchange(h.release(), std::memory_order_acq_rel);
      mGen.fetch_add(1, std::memory_order_release);   // before the rate: the audio thread accepts the host only with the rate, and then sees the new generation
      mCurRate.store(rate, std::memory_order_release);
      mRetired[1].reset(mRetired[0].release()); mRetired[0].reset(old);   // the audio thread may still hold the old host for the rest of its block: freed two rebuilds later
    }
  }

  std::atomic<Host*> mCur{nullptr};
  std::atomic<int> mCurRate{0}, mWantRate{0}; std::atomic<unsigned> mGen{0}; unsigned mSeenGen = 0;
  std::mutex mMutex; std::thread mThread; bool mRunning = false;
  std::unique_ptr<Host> mRetired[2];
  double mAppliedLevel = -1.0;
  bool mWindowValid = false; double mW[4] = {0, 0, 0, 0}, mFrac = 0.0;
  unsigned mMisses = 0;
};

}  // namespace dgg
