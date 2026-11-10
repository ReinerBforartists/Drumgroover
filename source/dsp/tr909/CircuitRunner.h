// TR-909: the complete circuit as the engine of a hit.
//
// Net909 (tr909/Net909.h) solves the full Nava netlist of BD, SD, LT, MT, HT, HC, RM and the shared NOISE sheet in one network (482 nodes), like the coupled NGSpice deck:
// the free-running oscillators of the toms, SD and BD, the shared noise line, the shared tom shaping filter and the slow RC states are parts of ONE circuit. This runner renders a hit by
// continuing that circuit from an idle state that was run for one second (tr909/models/circuit_all7_state.h) with the trigger pulse of the voice, the accent voltage and the noise source of the
// hit, and returns the output of the voice minus its idle level at the sample rate of the request. It is a Tr909ModelRunner, so it can replace the per-voice models in the bake.
//
// Not a real-time engine (a hit takes minutes on one thread); the bake and the reference comparisons use it. Supported: voices BD, SD, LT, MT, HT, HC at any pot state of the voice (909PotParams.h; the pots are the circuit pots of the netlist), the 5 us reference noise path.
// Anything else returns false (no silent replacement by another engine).
#pragma once
#include "../909BakeHost.h"
#include "../909PotParams.h"
#include "Net909.h"
#include "HitRunner.h"
#include "models/circuit_all7_net.h"
#include "models/circuit_all7_state.h"
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace dgg {

class Tr909CircuitRunner : public Tr909ModelRunner
{
public:
  // dt: integration step of the circuit (1 us: the step at which the oscillator frequencies of the circuit were validated against NGSpice). abort: polled between steps, true stops the hit (the run then fails).
  explicit Tr909CircuitRunner(double dt = 1e-6, std::function<bool()> abort = nullptr) : dt_(dt), abort_(std::move(abort)) {}

  static constexpr double kMaxPreroll = 0.2;   // seconds of idle before the trigger, up to this
  static double Preroll(uint64_t seed) { const double g = static_cast<double>(seed) * 0.6180339887498949; return (g - std::floor(g)) * kMaxPreroll; }   // golden-ratio sequence: consecutive variants spread over the span
  double fixedPreroll = -1.0;   // test hook: fixed idle time before the trigger (>= 0), otherwise Preroll(seed)
  bool silentNoise = false;   // test hook: the noise source stays at its idle level (compare with net909 runs that have no noise)
  static bool Supports(Tr909Voice v) { return v == Tr909Voice::Bd || v == Tr909Voice::Sd || v == Tr909Voice::Lt || v == Tr909Voice::Mt || v == Tr909Voice::Ht || v == Tr909Voice::Hc; }

  bool Run(const Tr909RunRequest& req, std::vector<float>& out) override
  {
    out.clear();
    if (!Supports(req.voice) || req.noise != Tr909NoisePath::Reference5us) return false;
    if (!Ready()) return false;
    return Render(req.voice, req.params, req.anchorCode, req.noiseSeed, req.seconds, req.sampleRateHz, out);
  }

private:
  // Circuit pots of a voice in the order of 909PotParams.h (names of the deck parameters without vr_).
  static const char* const* PotNames(Tr909Voice v, int& n)
  {
    static const char* const bd[] = {"VR2_bd", "VR3_bd", "VR4_bd", "VR5_bd"}, *const sd[] = {"VR6_sd", "VR7_sd", "VR8_sd", "VR9_sd"}, *const lt[] = {"VR10_lt", "VR11_lt", "VR12_lt"}, *const mt[] = {"VR13_mt", "VR14_mt", "VR15_mt"},
                             *const ht[] = {"VR16_ht", "VR17_ht", "VR18_ht"}, *const hc[] = {"VR20_hc"};
    switch (v) { case Tr909Voice::Bd: n = 4; return bd; case Tr909Voice::Sd: n = 4; return sd; case Tr909Voice::Lt: n = 3; return lt; case Tr909Voice::Mt: n = 3; return mt; case Tr909Voice::Ht: n = 3; return ht; default: n = 1; return hc; }
  }
  bool SetPots(Tr909Voice v, uint64_t params)
  {
    int n = 0; const char* const* names = PotNames(v, n); double pos[8]; Tr909UnpackPots(v, params, pos);
    net_->resetPots();   // nominal pots are the conductances of the deck itself (bit-exact); only pots away from 0.5 are recomputed
    for (int i = 0; i < n; ++i) if (std::fabs(pos[i] - 0.5) > 1e-12 && !net_->setPot(names[i], pos[i])) return false;
    return true;
  }
  // Idle state for the pot state: the stored state belongs to nominal pots; for another pot state the circuit idles on for kSettle seconds with the pots set (the slow RC states of the pot networks follow;
  // 2 us steps, only the state is wanted), kept per pot state of the runner.
  const n909::Net::State& StateFor(Tr909Voice v, uint64_t params)
  {
    if (params == 0) return st_;
    const uint64_t key = (params << 3) ^ static_cast<uint64_t>(v);
    auto it = settled_.find(key); if (it != settled_.end()) return it->second;
    n909::Net& net = *net_; net.restoreState(st_);
    for (const char* t : {"BD", "SD", "LT", "MT", "HT", "HC", "RM"}) net.setSourceDc(std::string("VTRIG_") + t, 0.0);
    net.setSourceDc("Vnz", 7.5);
    for (const char* t : {"BD", "SD", "LT", "MT", "HT", "HC", "RM"}) net.setSourceDc(std::string("VACCENT_") + t, 2.4);   // the accent of the stored idle state
    double t = 0.0; const double h = 2e-6; net.dt = h;
    for (long i = 0; i < static_cast<long>(kSettle / h); ++i) { if (abort_ && (i & 255) == 0 && abort_()) break; if (!net.advance(t, h)) break; t += h; }
    if (settled_.size() >= 8) settled_.erase(settled_.begin());
    return settled_[key] = net.saveState();
  }
  static const char* Tag(Tr909Voice v)   // sheet tag of the netlist (node and source names)
  {
    switch (v) { case Tr909Voice::Bd: return "BD"; case Tr909Voice::Sd: return "SD"; case Tr909Voice::Lt: return "LT"; case Tr909Voice::Mt: return "MT"; case Tr909Voice::Ht: return "HT"; default: return "HC"; }
  }
  bool Ready()
  {
    if (ready_) return true;
    std::string text; for (int i = 0; i < kTr909CircuitNetLineCount; ++i) { text += kTr909CircuitNetLines[i]; text += '\n'; }
    net_ = std::make_unique<n909::Net>();
    if (!net_->loadText(text)) { net_.reset(); return false; }
    st_.x.assign(kTr909CircuitIdleState, kTr909CircuitIdleState + kTr909CircuitStateX);
    st_.q1.assign(kTr909CircuitIdleState + kTr909CircuitStateX, kTr909CircuitIdleState + kTr909CircuitStateX + kTr909CircuitStateQ);
    st_.q2.assign(kTr909CircuitIdleState + kTr909CircuitStateX + kTr909CircuitStateQ, kTr909CircuitIdleState + kTr909CircuitStateX + 2 * kTr909CircuitStateQ);
    st_.stepIndex = kTr909CircuitStateStepIndex; st_.hPrev = kTr909CircuitStateHPrev;
    if (!net_->restoreState(st_)) { net_.reset(); return false; }
    ready_ = true; return true;
  }
  bool Render(Tr909Voice v, uint64_t params, int accentCode, uint64_t seed, double seconds, int fs, std::vector<float>& out)
  {
    n909::Net& net = *net_;
    if (!SetPots(v, params)) return false;
    if (!net.restoreState(StateFor(v, params))) return false;
    const std::string tag = Tag(v), lower = tag == "BD" ? "bd" : tag == "SD" ? "sd" : tag == "LT" ? "lt" : tag == "MT" ? "mt" : tag == "HT" ? "ht" : "hc";
    const int outNode = net.nodeIndex(lower + "_" + tag + "_OUT"); if (outNode < 0) return false;
    const double t0 = kTriggerDelay + (fixedPreroll >= 0.0 ? fixedPreroll : Preroll(seed));   // trigger time of the hit in the run: the circuit idles on from the stored state before the trigger, so the free-running oscillators are at a different phase for every noise seed (variant)
    for (const char* t : {"BD", "SD", "LT", "MT", "HT", "HC", "RM"}) net.setSourceDc(std::string("VTRIG_") + t, 0.0);
    net.setSource("VTRIG_" + tag, [t0](double t) { return pcmfast::HitTrigger(t - t0); });
    net.setSourceDc("VACCENT_" + tag, Tr909AccentVolts(accentCode));
    pcmfast::HitNoise noise; noise.Start(seed, 5e-6, 0.0);   // the same source as the per-voice models: the noise of the hit starts at knot 10000 of the sequence (kStartTime) when the trigger arrives
    if (!silentNoise) net.setSource("Vnz", [&noise, t0](double t) { return t < t0 ? 7.5 : noise.At(kNoiseAtTrigger + (t - t0)); });
    else net.setSourceDc("Vnz", 7.5);
    const double ts = 1.0 / fs; const size_t nOut = static_cast<size_t>(seconds * fs + 0.5);
    out.assign(nOut, 0.0f);
    double t = 0.0, prevV = net.V(outNode), idle = prevV, nextOut = t0;   // output samples from the trigger on; idle level = output just before the trigger
    size_t k = 0; bool idleSet = false; unsigned steps = 0;
    net.dt = dt_;
    while (k < nOut)
    {
      if (abort_ && ((++steps & 255u) == 0) && abort_()) { out.clear(); return false; }
      if (!net.advance(t, dt_)) { out.clear(); return false; }
      const double tn = t + dt_, cur = net.V(outNode);
      if (!idleSet && tn >= t0 - 1e-12) { idle = cur; idleSet = true; }
      while (k < nOut && nextOut <= tn + 1e-15)
      {
        const double a = std::min(std::max((nextOut - t) / dt_, 0.0), 1.0);
        out[k++] = static_cast<float>(prevV + (cur - prevV) * a - idle); nextOut += ts;
      }
      prevV = cur; t = tn;
    }
    return true;
  }
  static constexpr double kTriggerDelay = 0.05;   // minimum idle before the trigger: the accent (a held level in the sequencer) and the pot states settle as in the NGSpice decks (50 ms)
  static constexpr double kNoiseAtTrigger = 0.05;   // HitRunner::kStartTime: the noise sequence of a hit is read from 50 ms on when the trigger arrives
  static constexpr double kSettle = 0.5;
  std::map<uint64_t, n909::Net::State> settled_;
  double dt_; std::function<bool()> abort_;
  std::unique_ptr<n909::Net> net_; n909::Net::State st_; bool ready_ = false;
};

}  // namespace dgg
