// ModelRunner909 of the PCM voices (hi-hats, crash, ride): the reduced model with the ROM replacement inside, one slot per voice, created on first use. The voices with a circuit (BD, SD, LT, MT, HT, HC) are rendered by
// the complete circuit (909CircuitRunner.h, 909CircuitBake.h).
//
// An object of this class is not thread safe; use one object per worker thread.
#pragma once
#include "909BakeHost.h"
#include "909HitNoise.h"
#include "909PcmHatAvx2.h"
#include "909_crash_pcm.h"
#include "909_ride_pcm.h"
#include "909_hh_pcm.h"
#include "909_hho_pcm.h"
#include "909PcmVoiceHost.h"
#include "909PcmTune.h"
#include "909PotParams.h"
#include <algorithm>
#include <memory>

namespace dgg {

class PcmModelRunner909 : public ModelRunner909, public LiveSource909
{
public:
  bool Run(const RunRequest909& req, std::vector<float>& out) override
  {
    if (req.voice >= Voice909::kCount || !IsPcmVoice909(req.voice)) return false;   // the PCM voices have one model, the accent is its run-time input
    SlotBase* slot = SlotFor(req.voice); if (!slot) return false;
    return slot->Render(req.voice, req.params, req.seconds, req.sampleRateHz, out, req.anchorCode);
  }
  void Prebuild(int sampleRateHz) override   // the slots of the PCM voices with their tables, so that the first live hit builds nothing on the audio thread; call from a non-audio thread
  {
    for (const Voice909 v : { Voice909::Hh, Voice909::Ho, Voice909::Cr, Voice909::Ri }) if (SlotBase* s = SlotFor(v)) s->Prebuild(v, sampleRateHz);
  }
  bool Warm(Voice909 v, int, NoisePath909, uint64_t = 0) override { return SlotFor(v) != nullptr; }
  bool Start(Voice909 v, int accentCode, NoisePath909, uint64_t, int sampleRateHz, double seconds, uint64_t params = 0) override
  {
    live_ = nullptr; SlotBase* s = SlotFor(v); if (!s) return false;
    if (!s->Begin(v, params, seconds, sampleRateHz, accentCode)) return false;
    live_ = s; return true;
  }
  int Pull(float* out, int n) override
  {
    if (!live_) return 0;
    const int w = live_->Pull(out, n); if (w < n) live_ = nullptr; return w;
  }
  bool Active() const override { return live_ != nullptr; }

private:
  struct SlotBase
  {
    virtual ~SlotBase() = default;
    virtual bool Render(Voice909 v, uint64_t params, double seconds, int fs, std::vector<float>& out, int accentCode) = 0;
    virtual bool Begin(Voice909 v, uint64_t params, double seconds, int fs, int accentCode) = 0;
    virtual int Pull(float* out, int n) = 0;
    virtual void Prebuild(Voice909, int) {}   // builds what the first hit would build (tables of the model at the sample rate), without starting a hit
  };

  // A PCM voice (Crash, Ride): the reduced model with the ROM replacement inside, run through PcmVoiceHost (host rate decimator, Level = the output pot as an exact gain). The accent code is the accent input of the hit.
  template <class M>
  struct PcmSlot : SlotBase
  {
    pcmfast::PcmVoiceHost<M> host; int hostFs = 0; long want_ = 0; double curPeriod = 0.0;
    // Hats: the AVX2 unit (909PcmHatAvx2.cpp) when the CPU supports it, else the baseline host above.
    HatAvx2909* av = nullptr; bool avTried = false;
    ~PcmSlot() override { delete av; }
    bool UseAvx(Voice909 v)
    {
      if constexpr (pcmfast::ResCount<M>::value > 0)
      {
        if (!avTried) { avTried = true; if (LanesAvailable909()) av = HatAvx2Create909(v == Voice909::Ho); }
        return av != nullptr;
      }
      else return false;
    }
    void Prepare(Voice909 v, uint64_t params, int fs, int accentCode)
    {
      double p[8]; UnpackPots909(v, params, p);
      if (UseAvx(v)) { av->Prepare(fs, p[1], p[0], AccentVolts909(accentCode)); return; }
      // Crash / Ride: p[1] is the Tune pot (the PCM clock of the voice). Hats: p[1] is the Decay pot, a circuit resistor of the model (VR21 for the Closed Hat, VR23 for the Open Hat), the PCM clock is fixed.
      const bool hat = pcmfast::ResCount<M>::value > 0;
      const double period = hat ? M::kPeriod : M::kPeriod / PcmClockRatio909(p[1], v == Voice909::Ri);
      if (hostFs != fs || period != curPeriod) { host.Init(static_cast<double>(fs)); hostFs = fs; host.SetPeriod(period); curPeriod = period; }
      if constexpr (pcmfast::ResCount<M>::value > 0)
      {
        const double x = std::max(p[1], 0.001), xn = 0.5;   // the other decay resistor stays at its nominal position
        const double g21 = v == Voice909::Hh ? 1.0 / (100e3 * x) : 1.0 / (100e3 * xn), g23 = v == Voice909::Ho ? 1.0 / (1e6 * std::pow(x, 2.74)) : 1.0 / (1e6 * std::pow(xn, 2.74));
        host.voice.SetResG(0, g21); host.voice.SetResG(1, g23);
      }
      host.SetHatMode(hat); host.SetAccent(AccentVolts909(accentCode)); host.SetLevel(p[0]);
    }
    bool Render(Voice909 v, uint64_t params, double seconds, int fs, std::vector<float>& out, int accentCode) override
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
    void Prebuild(Voice909 v, int fs) override { Prepare(v, 0, fs, 127); }
    bool Begin(Voice909 v, uint64_t params, double seconds, int fs, int accentCode) override { Prepare(v, params, fs, accentCode); Trigger(); want_ = static_cast<long>(seconds * fs); return true; }
    int Pull(float* out, int n) override
    {
      if (!IsActive() || want_ <= 0) return 0;
      const int w = static_cast<int>(std::min<long>(n, want_)); RenderBlock(out, w); want_ -= w; return w;
    }
    void Trigger() { if (av) av->Trigger(); else host.Trigger(); }
    bool IsActive() const { return av ? av->Active() : host.Active(); }
    void RenderBlock(float* out, int n) { if (av) av->Render(out, n); else host.Render(out, n); }
  };

  SlotBase* SlotFor(Voice909 v)
  {
    if (v >= Voice909::kCount || !IsPcmVoice909(v)) return nullptr;
    std::unique_ptr<SlotBase>& slot = slots_[static_cast<int>(v)];
    if (!slot) slot = Make(v);
    return slot.get();
  }
  static std::unique_ptr<SlotBase> Make(Voice909 v)
  {
    if (v == Voice909::Cr) return std::unique_ptr<SlotBase>(new PcmSlot<CrashModelA0>());
    if (v == Voice909::Ri) return std::unique_ptr<SlotBase>(new PcmSlot<RideModelA0>());
    if (v == Voice909::Hh) return std::unique_ptr<SlotBase>(new PcmSlot<HhModelA0>());
    if (v == Voice909::Ho) return std::unique_ptr<SlotBase>(new PcmSlot<HhoModelA0>());
    return nullptr;
  }

  std::unique_ptr<SlotBase> slots_[static_cast<int>(Voice909::kCount)];
  SlotBase* live_ = nullptr;
};

}  // namespace dgg
