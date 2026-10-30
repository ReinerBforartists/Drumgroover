// Tr909ModelRunner for the FullFast2 models: one HitRunner per (voice, accent anchor), created on first use.
//
// An object of this class is not thread safe and holds the propagators of every model it has run; use one object per worker thread.
#pragma once
#include "909BakeHost.h"
#include "tr909/HitRunner.h"
#include "tr909/HcHybrid.h"
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
#include <functional>
#include <memory>

namespace dgg {

class Tr909FullModelRunner : public Tr909ModelRunner, public Tr909LiveSource
{
public:
  // Table packages of the closed hi-hat hybrid model (one per accent anchor, tr909/HcHybrid.h). With a provider that returns a package for the anchor, the HC runs on the
  // hybrid model at the 5 us noise path; without one (or on the 20 us path, or when the package cannot be read) the HC runs on the full model as before.
  // Set before the first run; the packages must outlive the runner.
  using HcPackageProvider = std::function<const hch::HcPackage*(int anchorCode)>;
  void SetHcPackages(HcPackageProvider p) { hcPkgs_ = std::move(p); }
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
      if (!s->Begin(v, np == Tr909NoisePath::Fast20us, noiseSeed, seconds, sampleRateHz)) { liveN_ = 0; return false; }
      live_[i] = s; liveW_[i] = runs[i].weight;
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
    virtual bool Begin(Tr909Voice v, bool fast20, uint64_t seed, double seconds, int fs) = 0;
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
    bool Begin(Tr909Voice v, bool fast20, uint64_t seed, double seconds, int fs) override
    {
      noise_.Start(seed, fast20 ? 20 * 1e-6 : 5e-6, fast20 ? 0.13 : 0.0);
      dec_.Init(static_cast<double>(fs)); produced_ = 0; want_ = static_cast<long>(seconds * fs);
      runner.Begin(pcmfast::HitTuningFor(v, fast20), &noise_, seconds); return true;
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

  // HC on the hybrid model: tables from the package, state at 40 ms cached per noise seed. Anything the hybrid does not cover goes to the full model of the same anchor.
  template <class Mdl>
  struct HcSlot : SlotBase
  {
    HcSlot(HcPackageProvider prov, int anchorCode) : prov_(std::move(prov)), code_(anchorCode) {}
    bool Ensure(bool fast20) { if (fast20) return false; if (!pkg_) pkg_ = prov_(code_); if (!pkg_) return false; if (!tried_) { tried_ = true; ok_ = hs_.Load(*pkg_); } return ok_; }
    bool Render(Tr909Voice v, bool fast20, uint64_t seed, double seconds, int fs, std::vector<float>& out) override
    {
      if (!Ensure(fast20)) { if (fast20) return full_.Render(v, fast20, seed, seconds, fs, out); out.clear(); return false; }   // the 5 us path exists only as the hybrid model (model version 3)
      // like HitRunner::Render: the samples the decimator has produced up to the end of the hit, at most seconds x fs
      const long want = static_cast<long>(seconds * fs); out.clear(); out.resize(static_cast<size_t>(std::max(want, 0L)));
      if (!hs_.Begin(seed, static_cast<double>(fs), seconds)) { out.clear(); return false; }
      long got = 0; while (got < want) { const int w = hs_.Pull(out.data() + got, static_cast<int>(std::min<long>(want - got, 4096))); if (w <= 0) break; got += w; }
      out.resize(static_cast<size_t>(got)); return got > 0;
    }
    void Warm(Tr909Voice v, bool fast20) override { if (!Ensure(fast20) && fast20) full_.Warm(v, fast20); }
    bool Begin(Tr909Voice v, bool fast20, uint64_t seed, double seconds, int fs) override
    {
      useHybrid_ = Ensure(fast20); if (!useHybrid_) { if (fast20) { useFull_ = true; return full_.Begin(v, fast20, seed, seconds, fs); } useFull_ = false; return false; }
      useFull_ = false;
      return hs_.Begin(seed, static_cast<double>(fs), seconds);
    }
    int Pull(float* out, int n) override { return useHybrid_ ? hs_.Pull(out, n) : (useFull_ ? full_.Pull(out, n) : 0); }
    // builds the cached state of a noise seed (about 0.2 s); call on a worker thread before the first Begin of that seed
    bool Prepare(uint64_t seed, bool fast20) { return Ensure(fast20) && hs_.Prepare(seed); }
    HcPackageProvider prov_; int code_; const hch::HcPackage* pkg_ = nullptr; hch::HitStream<Mdl> hs_; Slot<Mdl> full_; bool tried_ = false, ok_ = false, useHybrid_ = false, useFull_ = false;
  };

  SlotBase* SlotFor(Tr909Voice v, int anchorCode)
  {
    int a = -1; for (int i = 0; i < kTr909AnchorCount; ++i) if (kTr909Anchors[i] == anchorCode) a = i;
    if (a < 0 || static_cast<int>(v) < 0 || v >= Tr909Voice::kCount) return nullptr;
    std::unique_ptr<SlotBase>& slot = slots_[static_cast<int>(v)][a];
    if (!slot) slot = Make(v, a);
    return slot.get();
  }

  std::unique_ptr<SlotBase> Make(Tr909Voice v, int a)
  {
    if (v == Tr909Voice::Hc && hcPkgs_)   // the package is looked up when the slot is used (Ensure), so a directory that is set later is found
    {
      const int code = kTr909Anchors[a];
      if (a == 0) return std::unique_ptr<SlotBase>(new HcSlot<Tr909_HC_A25>(hcPkgs_, code));
      if (a == 1) return std::unique_ptr<SlotBase>(new HcSlot<Tr909_HC_A50>(hcPkgs_, code));
      if (a == 2) return std::unique_ptr<SlotBase>(new HcSlot<Tr909_HC_A75>(hcPkgs_, code));
      if (a == 3) return std::unique_ptr<SlotBase>(new HcSlot<Tr909_HC_A100>(hcPkgs_, code));
      if (a == 4) return std::unique_ptr<SlotBase>(new HcSlot<Tr909_HC_A127>(hcPkgs_, code));
    }
#define DGG_909_SLOT(V, VOICE, CODE, IDX) if (v == Tr909Voice::VOICE && a == IDX) return std::unique_ptr<SlotBase>(new Slot<Tr909_##V##_A##CODE>());
#define DGG_909_VOICE(V, VOICE) DGG_909_SLOT(V, VOICE, 25, 0) DGG_909_SLOT(V, VOICE, 50, 1) DGG_909_SLOT(V, VOICE, 75, 2) DGG_909_SLOT(V, VOICE, 100, 3) DGG_909_SLOT(V, VOICE, 127, 4)
    DGG_909_VOICE(BD, Bd) DGG_909_VOICE(SD, Sd) DGG_909_VOICE(LT, Lt) DGG_909_VOICE(MT, Mt) DGG_909_VOICE(HT, Ht) DGG_909_VOICE(HC, Hc)
#undef DGG_909_VOICE
#undef DGG_909_SLOT
    return nullptr;
  }

  std::unique_ptr<SlotBase> slots_[static_cast<int>(Tr909Voice::kCount)][kTr909AnchorCount];
  HcPackageProvider hcPkgs_;
  SlotBase* live_[2] = {nullptr, nullptr}; double liveW_[2] = {0.0, 0.0}; int liveN_ = 0;
};

}  // namespace dgg
