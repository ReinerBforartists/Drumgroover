// Tr909ModelRunner for the FullFast2 models: one HitRunner per (voice, accent anchor), created on first use.
//
// An object of this class is not thread safe and holds the propagators of every model it has run; use one object per worker thread.
#pragma once
#include "909BakeHost.h"
#include "tr909/HitRunner.h"
#include "tr909/models/lt_a25.h"
#include "tr909/models/lt_a50.h"
#include "tr909/models/lt_a75.h"
#include "tr909/models/lt_a100.h"
#include "tr909/models/lt_a127.h"
#include "tr909/models/mt_a25.h"
#include "tr909/models/mt_a50.h"
#include "tr909/models/mt_a75.h"
#include "tr909/models/mt_a100.h"
#include "tr909/models/mt_a127.h"
#include "tr909/models/ht_a25.h"
#include "tr909/models/ht_a50.h"
#include "tr909/models/ht_a75.h"
#include "tr909/models/ht_a100.h"
#include "tr909/models/ht_a127.h"
#include "tr909/models/bd_a25.h"
#include "tr909/models/bd_a50.h"
#include "tr909/models/bd_a75.h"
#include "tr909/models/bd_a100.h"
#include "tr909/models/bd_a127.h"
#include "tr909/models/sd_a25.h"
#include "tr909/models/sd_a50.h"
#include "tr909/models/sd_a75.h"
#include "tr909/models/sd_a100.h"
#include "tr909/models/sd_a127.h"
#include "tr909/models/hc_a25.h"
#include "tr909/models/hc_a50.h"
#include "tr909/models/hc_a75.h"
#include "tr909/models/hc_a100.h"
#include "tr909/models/hc_a127.h"
#include <memory>

namespace dgg {

class Tr909FullModelRunner : public Tr909ModelRunner, public Tr909LiveSource
{
public:
  bool Run(const Tr909RunRequest& req, std::vector<float>& out) override
  {
    int a = -1;
    for (int i = 0; i < kTr909AnchorCount; ++i) if (kTr909Anchors[i] == req.anchorCode) a = i;
    if (a < 0 || static_cast<int>(req.voice) < 0 || req.voice >= Tr909Voice::kCount) return false;
    std::unique_ptr<SlotBase>& slot = slots_[static_cast<int>(req.voice)][a];
    if (!slot) slot = Make(req.voice, a);
    if (!slot) return false;
    const bool fast20 = req.noise == Tr909NoisePath::Fast20us;
    return slot->Render(req.voice, fast20, req.noiseSeed, req.seconds, req.sampleRateHz, out);
  }

  bool Warm(Tr909Voice v, int accentCode, Tr909NoisePath np) override
  {
    Tr909AccentRun runs[2]; const int n = Tr909AccentRuns(v, accentCode, runs);
    for (int i = 0; i < n; ++i) { SlotBase* s = SlotFor(v, runs[i].anchorCode); if (!s) return false; s->Warm(v, np == Tr909NoisePath::Fast20us); }
    return n > 0;
  }
  bool Start(Tr909Voice v, int accentCode, Tr909NoisePath np, uint64_t noiseSeed, int sampleRateHz, double seconds) override
  {
    Tr909AccentRun runs[2]; const int n = Tr909AccentRuns(v, accentCode, runs);
    liveN_ = 0; if (n == 0) return false;
    for (int i = 0; i < n; ++i)
    {
      SlotBase* s = SlotFor(v, runs[i].anchorCode); if (!s) { liveN_ = 0; return false; }
      s->Begin(v, np == Tr909NoisePath::Fast20us, noiseSeed, seconds, sampleRateHz); live_[i] = s; liveW_[i] = runs[i].weight;
    }
    liveN_ = n; return true;
  }
  int Pull(float* out, int n) override
  {
    if (liveN_ == 0) return 0;
    if (liveN_ == 1) { const int w = live_[0]->Pull(out, n); if (w < n) liveN_ = 0; return w; }
    static thread_local std::vector<float> tmp[2]; int got[2];
    for (int i = 0; i < 2; ++i) { tmp[i].assign(static_cast<size_t>(n), 0.0f); got[i] = live_[i]->Pull(tmp[i].data(), n); }
    const int w = std::max(got[0], got[1]);
    for (int k = 0; k < w; ++k) out[k] = static_cast<float>(liveW_[0] * tmp[0][k] + liveW_[1] * tmp[1][k]);
    if (w < n) liveN_ = 0;
    return w;
  }
  bool Active() const override { return liveN_ != 0; }

private:
  struct SlotBase
  {
    virtual ~SlotBase() = default;
    virtual bool Render(Tr909Voice v, bool fast20, uint64_t seed, double seconds, int fs, std::vector<float>& out) = 0;
    virtual void Warm(Tr909Voice v, bool fast20) = 0;
    virtual void Begin(Tr909Voice v, bool fast20, uint64_t seed, double seconds, int fs) = 0;
    virtual int Pull(float* out, int n) = 0;
  };

  template <class Mdl>
  struct Slot : SlotBase
  {
    pcmfast::HitRunner<Mdl> runner;
    bool Render(Tr909Voice v, bool fast20, uint64_t seed, double seconds, int fs, std::vector<float>& out) override
    {
      pcmfast::HitNoise noise; noise.Start(seed, fast20 ? 20 * 1e-6 : 5e-6, fast20 ? 0.13 : 0.0);   // 20 * 1e-6: the exact step the validation used
      return runner.Render(pcmfast::HitTuningFor(v, fast20), &noise, seconds, static_cast<double>(fs), out);
    }
    void Warm(Tr909Voice v, bool fast20) override { runner.Warm(pcmfast::HitTuningFor(v, fast20)); }
    void Begin(Tr909Voice v, bool fast20, uint64_t seed, double seconds, int fs) override
    {
      noise_.Start(seed, fast20 ? 20 * 1e-6 : 5e-6, fast20 ? 0.13 : 0.0);
      dec_.Init(static_cast<double>(fs)); produced_ = 0; want_ = static_cast<long>(seconds * fs);
      runner.Begin(pcmfast::HitTuningFor(v, fast20), &noise_, seconds);
    }
    // Same steps, same decimator, same order as Render(): the samples are identical to the rendered hit.
    int Pull(float* out, int n) override
    {
      int w = 0; double smp;
      while (w < n && produced_ < want_)
      {
        if (dec_.Pop(smp)) { out[w++] = static_cast<float>(smp); ++produced_; continue; }
        if (runner.Finished()) break;
        runner.Advance(); dec_.Push(runner.Time(), runner.Output() - runner.idle());
      }
      return w;
    }
    pcmfast::HitNoise noise_; pcmfast::HostDecimator dec_; long produced_ = 0, want_ = 0;
  };

  SlotBase* SlotFor(Tr909Voice v, int anchorCode)
  {
    int a = -1; for (int i = 0; i < kTr909AnchorCount; ++i) if (kTr909Anchors[i] == anchorCode) a = i;
    if (a < 0 || static_cast<int>(v) < 0 || v >= Tr909Voice::kCount) return nullptr;
    std::unique_ptr<SlotBase>& slot = slots_[static_cast<int>(v)][a];
    if (!slot) slot = Make(v, a);
    return slot.get();
  }

  static std::unique_ptr<SlotBase> Make(Tr909Voice v, int a)
  {
#define DGG_909_SLOT(V, VOICE, CODE, IDX) if (v == Tr909Voice::VOICE && a == IDX) return std::unique_ptr<SlotBase>(new Slot<Tr909_##V##_A##CODE>());
#define DGG_909_VOICE(V, VOICE) DGG_909_SLOT(V, VOICE, 25, 0) DGG_909_SLOT(V, VOICE, 50, 1) DGG_909_SLOT(V, VOICE, 75, 2) DGG_909_SLOT(V, VOICE, 100, 3) DGG_909_SLOT(V, VOICE, 127, 4)
    DGG_909_VOICE(BD, Bd) DGG_909_VOICE(SD, Sd) DGG_909_VOICE(LT, Lt) DGG_909_VOICE(MT, Mt) DGG_909_VOICE(HT, Ht) DGG_909_VOICE(HC, Hc)
#undef DGG_909_VOICE
#undef DGG_909_SLOT
    return nullptr;
  }

  std::unique_ptr<SlotBase> slots_[static_cast<int>(Tr909Voice::kCount)][kTr909AnchorCount];
  SlotBase* live_[2] = {nullptr, nullptr}; double liveW_[2] = {0.0, 0.0}; int liveN_ = 0;
};

}  // namespace dgg
