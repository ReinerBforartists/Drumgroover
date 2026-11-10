// Tr909ModelRunner for the FullFast2 models: one HitRunner per (voice, accent anchor), created on first use.
//
// An object of this class is not thread safe and holds the propagators of every model it has run; use one object per worker thread.
#pragma once
#include "909BakeHost.h"
#include "tr909/HitRunner.h"
#include "tr909/HitLive.h"
#include "tr909/HitLanesApi.h"
#include "tr909/PcmHatAvx2.h"
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
#if __has_include("tr909/models_rom_test/crash_pcm.h")
// LOCAL TEST ONLY: hi-hat, crash and ride models with the original ROM data (folder is gitignored, never shipped; delete the folder to go back to the replacement samples)
#include "tr909/models_rom_test/crash_pcm.h"
#include "tr909/models_rom_test/ride_pcm.h"
#include "tr909/models_rom_test/hh_pcm.h"
#include "tr909/models_rom_test/hho_pcm.h"
#else
#include "tr909/models/crash_pcm.h"
#include "tr909/models/ride_pcm.h"
#include "tr909/models/hh_pcm.h"
#include "tr909/models/hho_pcm.h"
#endif
#include "tr909/PcmVoiceHost.h"
#include "909PcmTune.h"
#include "tr909/models/lt_a25_pot.h"
#include "tr909/models/lt_a50_pot.h"
#include "tr909/models/lt_a75_pot.h"
#include "tr909/models/lt_a100_pot.h"
#include "tr909/models/lt_a127_pot.h"
#include "tr909/models/mt_a25_pot.h"
#include "tr909/models/mt_a50_pot.h"
#include "tr909/models/mt_a75_pot.h"
#include "tr909/models/mt_a100_pot.h"
#include "tr909/models/mt_a127_pot.h"
#include "tr909/models/ht_a25_pot.h"
#include "tr909/models/ht_a50_pot.h"
#include "tr909/models/ht_a75_pot.h"
#include "tr909/models/ht_a100_pot.h"
#include "tr909/models/ht_a127_pot.h"
#include "tr909/models/bd_a25_pot.h"
#include "tr909/models/bd_a50_pot.h"
#include "tr909/models/bd_a75_pot.h"
#include "tr909/models/bd_a100_pot.h"
#include "tr909/models/bd_a127_pot.h"
#include "tr909/models/sd_a25_pot.h"
#include "tr909/models/sd_a50_pot.h"
#include "tr909/models/sd_a75_pot.h"
#include "tr909/models/sd_a100_pot.h"
#include "tr909/models/sd_a127_pot.h"
#include "tr909/models/hc_a25_pot.h"
#include "tr909/models/hc_a50_pot.h"
#include "tr909/models/hc_a75_pot.h"
#include "tr909/models/hc_a100_pot.h"
#include "tr909/models/hc_a127_pot.h"
#include "tr909/PotSet.h"
#include "909PotParams.h"
#include <atomic>
#include <memory>
#include <mutex>

namespace dgg {



class Tr909FullModelRunner : public Tr909ModelRunner, public Tr909LiveSource
{
public:
  bool Run(const Tr909RunRequest& req, std::vector<float>& out) override
  {
    int a = -1;
    for (int i = 0; i < kTr909AnchorCount; ++i) if (kTr909Anchors[i] == req.anchorCode) a = i;
    if (req.voice < Tr909Voice::kCount && Tr909IsPcmVoice(req.voice)) a = 0;   // the PCM voices have one model, the accent is its run-time input
    if (a < 0 || static_cast<int>(req.voice) < 0 || req.voice >= Tr909Voice::kCount) return false;
    std::unique_ptr<SlotBase>& slot = slots_[static_cast<int>(req.voice)][a];
    if (!slot) slot = Make(req.voice, a);
    if (!slot) return false;
    const bool fast20 = req.noise == Tr909NoisePath::Fast20us;
    return slot->Render(req.voice, req.params, fast20, req.noiseSeed, req.seconds, req.sampleRateHz, out, req.anchorCode);
  }
  // Lane engine for the cache bake (HitLanesApi.h): LT, MT, HT and BD so far (measured and gated per voice, tools handoff R392 / R393 / R394), 5 us reference noise only.
  int GroupSize(Tr909Voice v) const override { return (v == Tr909Voice::Lt || v == Tr909Voice::Mt || v == Tr909Voice::Ht || v == Tr909Voice::Bd) && Tr909LanesAvailable() ? 4 : 1; }
  bool RunGroup(const Tr909RunRequest& req, const uint64_t* seeds, int n, std::vector<float>* out) override
  {
    int a = -1;
    for (int i = 0; i < kTr909AnchorCount; ++i) if (kTr909Anchors[i] == req.anchorCode) a = i;
    if (a < 0 || (req.voice != Tr909Voice::Lt && req.voice != Tr909Voice::Mt && req.voice != Tr909Voice::Ht && req.voice != Tr909Voice::Bd) || req.noise != Tr909NoisePath::Reference5us || n < 1 || n > GroupSize(req.voice)) return false;
    SlotBase* slot = SlotFor(req.voice, req.anchorCode); if (!slot) return false;
    if (req.voice == Tr909Voice::Bd && pcmfast::HitTuningFor(Tr909Voice::Bd, false).linNoiseT >= 0.0) return slot->RenderGroup(req.voice, req.params, seeds, n, req.seconds, req.sampleRateHz, out);   // one noise-free run for the group
    Tr909LanePot lp; const Tr909LanePot* pot = slot->LanePotFor(req.voice, req.params, lp);   // the set stays in the slot of this thread until the next call with other pots
    const double fs = static_cast<double>(req.sampleRateHz);
    return req.voice == Tr909Voice::Lt ? Tr909LaneRenderLT(a, seeds, n, req.seconds, fs, out, &BakeAborted, pot)
                                       : req.voice == Tr909Voice::Mt ? Tr909LaneRenderMT(a, seeds, n, req.seconds, fs, out, &BakeAborted, pot)
                                                                     : req.voice == Tr909Voice::Ht ? Tr909LaneRenderHT(a, seeds, n, req.seconds, fs, out, &BakeAborted, pot)
                                                                                                                          : Tr909LaneRenderBD(a, seeds, n, req.seconds, fs, out, &BakeAborted, pot);
  }
  void Prebuild(int sampleRateHz) override   // the slots of the PCM voices (hi-hats, crash, ride) with their tables, so that the first live hit builds nothing on the audio thread; call from a non-audio thread
  {
    for (const Tr909Voice v : { Tr909Voice::Hh, Tr909Voice::Ho, Tr909Voice::Cr, Tr909Voice::Ri }) if (SlotBase* s = SlotFor(v, 0)) s->Prebuild(v, sampleRateHz);
    for (int i = 0; i < kTr909AnchorCount; ++i) if (SlotBase* s = SlotFor(Tr909Voice::Bd, kTr909Anchors[i])) s->Prebuild(Tr909Voice::Bd, sampleRateHz);   // BD: the sample rate of the prepared live hits (Warm() builds them)
  }
  bool Warm(Tr909Voice v, int accentCode, Tr909NoisePath np, uint64_t params = 0) override
  {
    Tr909AccentRun runs[2]; const int n = Tr909AccentRuns(v, accentCode, runs);
    for (int i = 0; i < n; ++i) { SlotBase* s = SlotFor(v, runs[i].anchorCode); if (!s) return false; s->Warm(v, params, np == Tr909NoisePath::Fast20us); }
    return n > 0;
  }
  bool Start(Tr909Voice v, int accentCode, Tr909NoisePath np, uint64_t noiseSeed, int sampleRateHz, double seconds, uint64_t params = 0) override
  {
    Tr909AccentRun runs[2]; const int n = Tr909AccentRuns(v, accentCode, runs);
    liveN_ = 0; if (n == 0) return false;
    for (int i = 0; i < n; ++i)
    {
      SlotBase* s = SlotFor(v, runs[i].anchorCode); if (!s) { liveN_ = 0; return false; }
      s->SetCoarse(n == 2 && (runs[i].weight < 0.5 || (runs[i].weight == 0.5 && i == 1)));
      if (!s->Begin(v, params, np == Tr909NoisePath::Fast20us, noiseSeed, seconds, sampleRateHz, runs[i].anchorCode)) { liveN_ = 0; return false; }
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
    virtual bool Render(Tr909Voice v, uint64_t params, bool fast20, uint64_t seed, double seconds, int fs, std::vector<float>& out, int accentCode) = 0;
    // Pot dependent model of the pot state `params` (909PotParams.h): nullptr for params == 0 (the arrays of the model header), else the set assembled by this slot, filled into lp.
    virtual const Tr909LanePot* LanePotFor(Tr909Voice v, uint64_t params, Tr909LanePot& lp) = 0;
    virtual void Warm(Tr909Voice v, uint64_t params, bool fast20) = 0;
    virtual bool Begin(Tr909Voice v, uint64_t params, bool fast20, uint64_t seed, double seconds, int fs, int accentCode) = 0;
    virtual int Pull(float* out, int n) = 0;
    virtual void SetCoarse(bool) {}   // the next Begin() may use the coarse live form (the run with the smaller weight of a blend; BD-D28)
    virtual bool RenderGroup(Tr909Voice, uint64_t, const uint64_t*, int, double, int, std::vector<float>*) { return false; }   // n noise sequences of one parameter set in one go (linear-response voices)
    virtual void Prebuild(Tr909Voice, int) {}   // builds what the first hit would build (tables of the model at the sample rate), without starting a hit
  };

  // One slot owns its PotSet (assembled on the thread of the runner, published to the engines as plain pointers; replaced only when the pot state changes, never modified while an engine renders).
  template <class Mdl, class Pot>
  struct Slot : SlotBase
  {
    pcmfast::HitRunner<Mdl> runner;
    pcmfast::PotSet<Mdl, Pot> potSet; uint64_t potParams = 0, potId = 0;   // potId: unique per assembly, never reused (0: no set)
    const Tr909LanePot* Select(Tr909Voice v, uint64_t params, Tr909LanePot& lp)
    {
      if (params == 0) return nullptr;
      if (potId == 0 || params != potParams)
      {
        double pots[8]; Tr909UnpackPots(v, params, pots); potSet.Assemble(pots); potParams = params;
        static std::atomic<uint64_t> counter{0}; potId = ++counter;
      }
      const auto pp = potSet.Ptrs(); lp = { potId, pp.A, pp.Bg, pp.XzS, pp.DgS, pp.Xo, pp.Dof, pp.Dou, pp.Do1 };
      return &lp;
    }
    const Tr909LanePot* LanePotFor(Tr909Voice v, uint64_t params, Tr909LanePot& lp) override { return Select(v, params, lp); }
    // Installs the pot state in the hit runner (its propagators are rebuilt only when the state changes).
    void ApplyPots(Tr909Voice v, uint64_t params)
    {
      Tr909LanePot lp; const Tr909LanePot* sel = Select(v, params, lp);
      if (sel == nullptr) { if (runnerId_ != 0) { runner.ClearPot(); runnerId_ = 0; } }
      else if (runnerId_ != sel->id) { runner.SetPot(potSet.Ptrs()); runnerId_ = sel->id; }
    }
    uint64_t runnerId_ = 0;
    // Live form in the linear-response mode (HitLive.h): prepared by Warm() on a non-audio thread for the sample rate set by Prebuild(); a hit uses it only when the parameter set (pots), sample rate and length match, else the
    // stepwise solver below runs the hit. The prepared object is replaced as a whole; the audio thread holds its own reference during a hit, retired objects are released on the preparing thread.
    using Live = pcmfast::LiveLin<Mdl, Pot>;
    std::shared_ptr<Live> live_, liveC_, hit_; std::vector<std::shared_ptr<Live>> retired_; int liveFs_ = 0; long hitWant_ = 0, hitProduced_ = 0; std::mutex liveMutex_; bool coarse_ = false;
    void Prebuild(Tr909Voice, int fs) override { liveFs_ = fs; }
    void SetCoarse(bool c) override { coarse_ = c; }
    // Two prepared forms per parameter set: the fine one (noise knots of the response every 10 us) and the coarse one (20 us, about half the cost per hit, response differs by -35 ... -57 dB re signal) for the run of a blend whose weight is below one half.
    void BuildLive(Tr909Voice v, uint64_t params)
    {
      const pcmfast::HitTuning tu = pcmfast::HitTuningFor(v, false); if (tu.linNoiseT < 0.0 || liveFs_ <= 0) return;
      std::lock_guard<std::mutex> lk(liveMutex_);   // one preparation at a time
      double pots[8]; if (params != 0) Tr909UnpackPots(v, params, pots);
      for (int form = 0; form < 2; ++form)
      {
        std::shared_ptr<Live>& slotPtr = form == 0 ? live_ : liveC_;
        std::shared_ptr<Live> cur = std::atomic_load(&slotPtr); if (cur && cur->Ready() && cur->Key() == params && cur->SampleRate() == liveFs_) continue;
        pcmfast::HitTuning tf = tu; if (form == 1) tf.replayKnot = 20;
        std::shared_ptr<Live> nl = std::make_shared<Live>(); if (!nl->Prepare(tf, params != 0 ? pots : nullptr, params, liveFs_, 0.35)) return;
        std::atomic_store(&slotPtr, nl); if (cur) retired_.push_back(cur);
      }
      for (size_t i = 0; i < retired_.size();) { if (retired_[i].use_count() == 1) retired_.erase(retired_.begin() + static_cast<long>(i)); else ++i; }   // not referenced by a running hit
    }
    bool Render(Tr909Voice v, uint64_t params, bool fast20, uint64_t seed, double seconds, int fs, std::vector<float>& out, int) override
    {
      ApplyPots(v, params);
      pcmfast::HitNoise noise; noise.Start(seed, fast20 ? 20 * 1e-6 : 5e-6, fast20 ? 0.13 : 0.0);   // 20 * 1e-6: the exact step the validation used
      return runner.Render(pcmfast::HitTuningFor(v, fast20), &noise, seconds, static_cast<double>(fs), out);
    }
    bool RenderGroup(Tr909Voice v, uint64_t params, const uint64_t* seeds, int n, double seconds, int fs, std::vector<float>* out) override
    {
      const pcmfast::HitTuning tu = pcmfast::HitTuningFor(v, false); if (tu.linNoiseT < 0.0 || n < 1 || n > 4) return false;
      ApplyPots(v, params); pcmfast::HitNoise nz[4]; for (int l = 0; l < n; ++l) nz[l].Start(seeds[l], 5e-6, 0.0);
      return runner.RenderLinGroup(tu, nz, n, seconds, static_cast<double>(fs), out);
    }
    void Warm(Tr909Voice v, uint64_t params, bool fast20) override { ApplyPots(v, params); runner.Warm(pcmfast::HitTuningFor(v, fast20)); if (!fast20) BuildLive(v, params); }
    bool Begin(Tr909Voice v, uint64_t params, bool fast20, uint64_t seed, double seconds, int fs, int) override
    {
      hit_.reset();
      if (!fast20 && fs == liveFs_) { std::shared_ptr<Live> lv = coarse_ ? std::atomic_load(&liveC_) : std::atomic_load(&live_); if (!lv || !lv->Ready()) lv = std::atomic_load(&live_); if (lv && lv->Ready() && lv->Key() == params && lv->SampleRate() == fs && seconds <= lv->Seconds() + 1e-9) { hit_ = lv; hit_->Begin(seed); hitWant_ = static_cast<long>(seconds * fs); hitProduced_ = 0; return true; } }
      ApplyPots(v, params);
      noise_.Start(seed, fast20 ? 20 * 1e-6 : 5e-6, fast20 ? 0.13 : 0.0);
      dec_.Init(static_cast<double>(fs)); produced_ = 0; want_ = static_cast<long>(seconds * fs);
      runner.Begin(pcmfast::HitTuningFor(v, fast20), &noise_, seconds); return true;
    }
    // Same steps, same decimator, same order as Render(): the samples are identical to the rendered hit.
    int Pull(float* out, int n) override
    {
      if (hit_) { const int want = static_cast<int>(std::min<long>(n, hitWant_ - hitProduced_)); const int got = want > 0 ? hit_->Pull(out, want) : 0; hitProduced_ += got; return got; }
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

  // A PCM voice (Crash, Ride): the reduced model with the ROM replacement inside, run through PcmVoiceHost (host rate decimator, Level = the output pot as an exact gain). The accent code is the accent input of the hit.
  template <class M>
  struct PcmSlot : SlotBase
  {
    pcmfast::PcmVoiceHost<M> host; int hostFs = 0; long want_ = 0; double curPeriod = 0.0;
    // Hats: the AVX2 unit (PcmHatAvx2.cpp) when the CPU supports it, else the baseline host above.
    Tr909HatAvx2* av = nullptr; bool avTried = false;
    ~PcmSlot() override { delete av; }
    bool UseAvx(Tr909Voice v)
    {
      if constexpr (pcmfast::ResCount<M>::value > 0)
      {
        if (!avTried) { avTried = true; if (Tr909LanesAvailable()) av = Tr909HatAvx2Create(v == Tr909Voice::Ho); }
        return av != nullptr;
      }
      else return false;
    }
    void Prepare(Tr909Voice v, uint64_t params, int fs, int accentCode)
    {
      double p[8]; Tr909UnpackPots(v, params, p);
      if (UseAvx(v)) { av->Prepare(fs, p[1], p[0], Tr909AccentVolts(accentCode)); return; }
      // Crash / Ride: p[1] is the Tune pot (the PCM clock of the voice). Hats: p[1] is the Decay pot, a circuit resistor of the model (VR21 for the Closed Hat, VR23 for the Open Hat), the PCM clock is fixed.
      const bool hat = pcmfast::ResCount<M>::value > 0;
      const double period = hat ? M::kPeriod : M::kPeriod / Tr909PcmClockRatio(p[1], v == Tr909Voice::Ri);
      if (hostFs != fs || period != curPeriod) { host.Init(static_cast<double>(fs)); hostFs = fs; host.SetPeriod(period); curPeriod = period; }
      if constexpr (pcmfast::ResCount<M>::value > 0)
      {
        const double x = std::max(p[1], 0.001), xn = 0.5;   // the other decay resistor stays at its nominal position
        const double g21 = v == Tr909Voice::Hh ? 1.0 / (100e3 * x) : 1.0 / (100e3 * xn), g23 = v == Tr909Voice::Ho ? 1.0 / (1e6 * std::pow(x, 2.74)) : 1.0 / (1e6 * std::pow(xn, 2.74));
        host.voice.SetResG(0, g21); host.voice.SetResG(1, g23);
      }
      host.SetHatMode(hat); host.SetAccent(Tr909AccentVolts(accentCode)); host.SetLevel(p[0]);
    }
    const Tr909LanePot* LanePotFor(Tr909Voice, uint64_t, Tr909LanePot&) override { return nullptr; }
    bool Render(Tr909Voice v, uint64_t params, bool, uint64_t, double seconds, int fs, std::vector<float>& out, int accentCode) override
    {
      Prepare(v, params, fs, accentCode); Trigger(); out.clear();
      const long maxN = static_cast<long>(seconds * fs); float buf[512];
      while (IsActive() && static_cast<long>(out.size()) < maxN)
      {
        const int n = static_cast<int>(std::min<long>(512, maxN - static_cast<long>(out.size())));
        RenderBlock(buf, n); out.insert(out.end(), buf, buf + n);
      }
      return !out.empty();
    }
    void Warm(Tr909Voice, uint64_t, bool) override {}
    void Prebuild(Tr909Voice v, int fs) override { Prepare(v, 0, fs, 127); }
    bool Begin(Tr909Voice v, uint64_t params, bool, uint64_t, double seconds, int fs, int accentCode) override { Prepare(v, params, fs, accentCode); Trigger(); want_ = static_cast<long>(seconds * fs); return true; }
    int Pull(float* out, int n) override
    {
      if (!IsActive() || want_ <= 0) return 0;
      const int w = static_cast<int>(std::min<long>(n, want_)); RenderBlock(out, w); want_ -= w; return w;
    }
    void Trigger() { if (av) av->Trigger(); else host.Trigger(); }
    bool IsActive() const { return av ? av->Active() : host.Active(); }
    void RenderBlock(float* out, int n) { if (av) av->Render(out, n); else host.Render(out, n); }
  };

  SlotBase* SlotFor(Tr909Voice v, int anchorCode)
  {
    int a = -1; for (int i = 0; i < kTr909AnchorCount; ++i) if (kTr909Anchors[i] == anchorCode) a = i;
    if (v < Tr909Voice::kCount && Tr909IsPcmVoice(v)) a = 0;
    if (a < 0 || static_cast<int>(v) < 0 || v >= Tr909Voice::kCount) return nullptr;
    std::unique_ptr<SlotBase>& slot = slots_[static_cast<int>(v)][a];
    if (!slot) slot = Make(v, a);
    return slot.get();
  }

  static std::unique_ptr<SlotBase> Make(Tr909Voice v, int a)
  {
#define DGG_909_SLOT(V, VOICE, CODE, IDX) if (v == Tr909Voice::VOICE && a == IDX) return std::unique_ptr<SlotBase>(new Slot<Tr909_##V##_A##CODE, Tr909_##V##_A##CODE##_Pot>());
#define DGG_909_VOICE(V, VOICE) DGG_909_SLOT(V, VOICE, 25, 0) DGG_909_SLOT(V, VOICE, 50, 1) DGG_909_SLOT(V, VOICE, 75, 2) DGG_909_SLOT(V, VOICE, 100, 3) DGG_909_SLOT(V, VOICE, 127, 4)
    DGG_909_VOICE(BD, Bd) DGG_909_VOICE(SD, Sd) DGG_909_VOICE(LT, Lt) DGG_909_VOICE(MT, Mt) DGG_909_VOICE(HT, Ht) DGG_909_VOICE(HC, Hc)
    if (v == Tr909Voice::Cr && a == 0) return std::unique_ptr<SlotBase>(new PcmSlot<CrashModelA0>());
    if (v == Tr909Voice::Ri && a == 0) return std::unique_ptr<SlotBase>(new PcmSlot<RideModelA0>());
    if (v == Tr909Voice::Hh && a == 0) return std::unique_ptr<SlotBase>(new PcmSlot<HhModelA0>());
    if (v == Tr909Voice::Ho && a == 0) return std::unique_ptr<SlotBase>(new PcmSlot<HhoModelA0>());
#undef DGG_909_VOICE
#undef DGG_909_SLOT
    return nullptr;
  }

  std::unique_ptr<SlotBase> slots_[static_cast<int>(Tr909Voice::kCount)][kTr909AnchorCount];
  SlotBase* live_[2] = {nullptr, nullptr}; double liveW_[2] = {0.0, 0.0}; int liveN_ = 0;
};

}  // namespace dgg
