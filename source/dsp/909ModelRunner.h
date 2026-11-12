// Tr909ModelRunner of the PCM voices (hi-hats, crash, ride): the reduced model with the ROM replacement inside, one slot per voice, created on first use. The voices with a circuit (BD, SD, LT, MT, HT, HC) are rendered by
// the complete circuit (tr909/CircuitRunner.h, 909CircuitBake.h).
//
// An object of this class is not thread safe; use one object per worker thread.
#pragma once
#include "909BakeHost.h"
#include "tr909/HitNoise.h"
#include "tr909/PcmHatAvx2.h"
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
#include "909PotParams.h"
#include <algorithm>
#include <memory>

namespace dgg {

class Tr909PcmModelRunner : public Tr909ModelRunner, public Tr909LiveSource
{
public:
  bool Run(const Tr909RunRequest& req, std::vector<float>& out) override
  {
    if (req.voice >= Tr909Voice::kCount || !Tr909IsPcmVoice(req.voice)) return false;   // the PCM voices have one model, the accent is its run-time input
    SlotBase* slot = SlotFor(req.voice); if (!slot) return false;
    return slot->Render(req.voice, req.params, req.seconds, req.sampleRateHz, out, req.anchorCode);
  }
  void Prebuild(int sampleRateHz) override   // the slots of the PCM voices with their tables, so that the first live hit builds nothing on the audio thread; call from a non-audio thread
  {
    for (const Tr909Voice v : { Tr909Voice::Hh, Tr909Voice::Ho, Tr909Voice::Cr, Tr909Voice::Ri }) if (SlotBase* s = SlotFor(v)) s->Prebuild(v, sampleRateHz);
  }
  bool Warm(Tr909Voice v, int, Tr909NoisePath, uint64_t = 0) override { return SlotFor(v) != nullptr; }
  bool Start(Tr909Voice v, int accentCode, Tr909NoisePath, uint64_t, int sampleRateHz, double seconds, uint64_t params = 0) override
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
    virtual bool Render(Tr909Voice v, uint64_t params, double seconds, int fs, std::vector<float>& out, int accentCode) = 0;
    virtual bool Begin(Tr909Voice v, uint64_t params, double seconds, int fs, int accentCode) = 0;
    virtual int Pull(float* out, int n) = 0;
    virtual void Prebuild(Tr909Voice, int) {}   // builds what the first hit would build (tables of the model at the sample rate), without starting a hit
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
    bool Render(Tr909Voice v, uint64_t params, double seconds, int fs, std::vector<float>& out, int accentCode) override
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
    void Prebuild(Tr909Voice v, int fs) override { Prepare(v, 0, fs, 127); }
    bool Begin(Tr909Voice v, uint64_t params, double seconds, int fs, int accentCode) override { Prepare(v, params, fs, accentCode); Trigger(); want_ = static_cast<long>(seconds * fs); return true; }
    int Pull(float* out, int n) override
    {
      if (!IsActive() || want_ <= 0) return 0;
      const int w = static_cast<int>(std::min<long>(n, want_)); RenderBlock(out, w); want_ -= w; return w;
    }
    void Trigger() { if (av) av->Trigger(); else host.Trigger(); }
    bool IsActive() const { return av ? av->Active() : host.Active(); }
    void RenderBlock(float* out, int n) { if (av) av->Render(out, n); else host.Render(out, n); }
  };

  SlotBase* SlotFor(Tr909Voice v)
  {
    if (v >= Tr909Voice::kCount || !Tr909IsPcmVoice(v)) return nullptr;
    std::unique_ptr<SlotBase>& slot = slots_[static_cast<int>(v)];
    if (!slot) slot = Make(v);
    return slot.get();
  }
  static std::unique_ptr<SlotBase> Make(Tr909Voice v)
  {
    if (v == Tr909Voice::Cr) return std::unique_ptr<SlotBase>(new PcmSlot<CrashModelA0>());
    if (v == Tr909Voice::Ri) return std::unique_ptr<SlotBase>(new PcmSlot<RideModelA0>());
    if (v == Tr909Voice::Hh) return std::unique_ptr<SlotBase>(new PcmSlot<HhModelA0>());
    if (v == Tr909Voice::Ho) return std::unique_ptr<SlotBase>(new PcmSlot<HhoModelA0>());
    return nullptr;
  }

  std::unique_ptr<SlotBase> slots_[static_cast<int>(Tr909Voice::kCount)];
  SlotBase* live_ = nullptr;
};

}  // namespace dgg
