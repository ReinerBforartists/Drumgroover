// TR-909: the complete circuit as the engine of a hit.
//
// Net909 (tr909/Net909.h) solves the full Nava netlist of BD, SD, LT, MT, HT, HC, RM and the shared NOISE sheet (482 nodes), like the coupled NGSpice deck:
// the free-running oscillators of the toms, SD and BD, the shared noise line, the shared tom shaping filter and the slow RC states are parts of ONE circuit. The matrix is split along the
// structure of the circuit by tr909/Net909Part.h (PartNet: blocks BD, SD, HC, RM and the tom sheets, coupled through the shared NOISE line by waveform relaxation, validated against the monolithic
// network and against NGSpice, plan tools/909_GESAMTSCHALTUNG_PLAN.md section S4). This runner renders a hit by
// continuing that circuit from an idle state that was run for one second (tr909/models/circuit_all7_state.h) with the trigger pulse of the voice, the accent voltage and the noise source of the
// hit, and returns the output of the voice minus its idle level at the sample rate of the request. It is a Tr909ModelRunner (the bake engine of BD, SD, LT, MT, HT, HC).
//
// Not a real-time engine (a hit takes minutes on one thread); the bake and the reference comparisons use it. Supported: voices BD, SD, LT, MT, HT, HC at any pot state of the voice (909PotParams.h; the pots are the circuit pots of the netlist), the 5 us reference noise path.
// Anything else returns false (no silent replacement by another engine).
#pragma once
#include "../909BakeHost.h"
#include "../909PotParams.h"
#include "Net909.h"
#include "Net909Part.h"
#include "HitNoise.h"
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
  // Idle delay before the trigger of variant `seed` (seed = variant index, 1, 2, ...): the free-running oscillators (twelve, 52 ... 310 Hz depending on the pots) are at a different phase for every delay. The table
  // is a farthest-point sequence over delays in [0, 0.2 s): each next delay maximises the smallest circular phase distance (over all twelve oscillators and three pot states: nominal and two shifted sets with the measured
  // frequencies) to the delays before it (tools/_sweep/tr909_cpp/preroll_table.py 0.2 64). Worst empty phase gap per oscillator, units of the ideal 1/N: N = 4 / 8 / 16 / 32 variants 2.3-2.8 / 4.4 / 3.6-5.5 / 4.5-4.8 (single golden-ratio delay
  // frac(seed x 0.618) x 0.2 s, used before: 3.7-3.8 / 6.8-6.9 / 10.8-12.0 / 10.5-17.0). Beyond 64 variants the table repeats with a small golden-ratio offset (up to 3 ms).
  static double Preroll(uint64_t seed)
  {
    static const double kTable[64] = {0.0000, 0.0028, 0.1471, 0.1495, 0.0021, 0.0006, 0.0986, 0.0937, 0.1059, 0.1183, 0.1953, 0.1474, 0.1988, 0.1484, 0.1792, 0.0989, 0.0436, 0.0326, 0.0507, 0.1121, 0.0025, 0.0486, 0.0540, 0.1537, 0.1794, 0.0919, 0.0985, 0.1472, 0.0319, 0.0988, 0.1245, 0.1423, 0.0439, 0.0181, 0.1577, 0.1171, 0.0388, 0.1785, 0.1053, 0.1391, 0.0801, 0.1436, 0.0518, 0.1925, 0.0457, 0.1792, 0.1423, 0.0103, 0.0320, 0.1903, 0.0115, 0.0507, 0.0547, 0.0021, 0.1835, 0.0713, 0.0944, 0.0547, 0.0006, 0.1725, 0.0067, 0.1018, 0.0468, 0.1698};
    const uint64_t i = seed > 0 ? seed - 1 : 0; const double g = static_cast<double>(i >> 6) * 0.6180339887498949;
    return kTable[i & 63u] + (g - std::floor(g)) * 0.003;
  }
  // Preroll sequence for a measured pot state: the same farthest-point rule over delays in [0, 0.2 s) in steps of 50 us, but with the twelve oscillator frequencies of THIS pot state (measured in its idle
  // run) as the only phase axes: worst empty gap in a test with 25 random pot states 3.0 / 3.8 / 4.0 (8 / 16 / 32 variants) against 4.4 / 5.3 / 6.8 of the fixed table.
  static constexpr int kOscillators = 12, kPrerollTable = 64;
  static void BuildPrerollTable(const double* f, double* out, int n)
  {
    const int nc = static_cast<int>(kMaxPreroll / 5e-5 + 0.5);
    std::vector<double> ph(static_cast<size_t>(nc) * kOscillators), mind(static_cast<size_t>(nc) * kOscillators, 1.0);
    for (int c = 0; c < nc; ++c) for (int j = 0; j < kOscillators; ++j) { const double x = c * 5e-5 * f[j]; ph[static_cast<size_t>(c) * kOscillators + j] = x - std::floor(x); }
    auto update = [&](int pick) { for (int c = 0; c < nc; ++c) for (int j = 0; j < kOscillators; ++j) { double d = std::fabs(ph[static_cast<size_t>(c) * kOscillators + j] - ph[static_cast<size_t>(pick) * kOscillators + j]); d = std::min(d, 1.0 - d); double& m = mind[static_cast<size_t>(c) * kOscillators + j]; m = std::min(m, d); } };
    int pick = 0; out[0] = 0.0; update(0);
    for (int k = 1; k < n; ++k)
    {
      double best = -1.0; int bi = 0;
      for (int c = 0; c < nc; ++c) { double sc = 1.0; for (int j = 0; j < kOscillators; ++j) sc = std::min(sc, mind[static_cast<size_t>(c) * kOscillators + j]); if (sc > best) { best = sc; bi = c; } }
      pick = bi; out[k] = pick * 5e-5; update(pick);
    }
  }
  // Delay of variant `seed`: from the table of the pot state (64 entries, same repetition rule as Preroll) or the fixed table.
  static double PrerollAt(const std::vector<double>* table, uint64_t seed)
  {
    if (!table || table->size() < static_cast<size_t>(kPrerollTable)) return Preroll(seed);
    const uint64_t i = seed > 0 ? seed - 1 : 0; const double g = static_cast<double>(i >> 6) * 0.6180339887498949;
    return (*table)[i & 63u] + (g - std::floor(g)) * 0.003;
  }
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
  struct Settled { n909::PartNet::Snapshot snap; std::vector<double> table; };   // idle state of a pot state and its Preroll table (empty: use the fixed table)
  static const char* const* OscNodes() { static const char* const n[kOscillators] = {"bd_N_60", "sd_N_115", "sd_N_106", "lt_N_197", "lt_N_177", "lt_N_187", "mt_N_333", "mt_N_313", "mt_N_323", "ht_N_630", "ht_N_689", "ht_N_699"}; return n; }
  // Frequency of an oscillator node from the upward crossings of its mid level (first to last crossing); false if it does not oscillate.
  static bool CrossingFrequency(const n909::PartNet::Probe& p, double& f)
  {
    if (p.t.size() < 100) return false;
    double lo = p.v[0], hi = p.v[0]; for (double y : p.v) { lo = std::min(lo, y); hi = std::max(hi, y); }
    if (hi - lo < 1.0) return false;
    const double mid = 0.5 * (lo + hi); int n = 0; double tFirst = 0, tLast = 0;
    for (size_t k = 1; k < p.t.size(); ++k) if (p.v[k - 1] < mid && p.v[k] >= mid) { const double tc = p.t[k - 1] + (mid - p.v[k - 1]) * (p.t[k] - p.t[k - 1]) / (p.v[k] - p.v[k - 1]); if (n++ == 0) tFirst = tc; tLast = tc; }
    if (n < 4) return false; f = (n - 1) / (tLast - tFirst); return true;
  }
  const n909::PartNet::Snapshot& StateFor(Tr909Voice v, uint64_t params, const std::vector<double>*& table)
  {
    table = nullptr;
    if (params == 0) return nominal_;
    const uint64_t key = (params << 3) ^ static_cast<uint64_t>(v);
    auto it = settled_.find(key); if (it != settled_.end()) { table = it->second.table.empty() ? nullptr : &it->second.table; return it->second.snap; }
    n909::PartNet& net = *net_; net.restore(nominal_);
    for (const char* t : {"BD", "SD", "LT", "MT", "HT", "HC", "RM"}) net.setSourceDc(std::string("VTRIG_") + t, 0.0);
    net.setSourceDc("Vnz", 7.5);
    for (const char* t : {"BD", "SD", "LT", "MT", "HT", "HC", "RM"}) net.setSourceDc(std::string("VACCENT_") + t, 2.4);   // the accent of the stored idle state
    net.clearProbes(); net.fixedDt = 2e-6;
    bool probesOk = true; for (int k = 0; k < kOscillators; ++k) probesOk = net.addProbe(OscNodes()[k]) && probesOk;
    net.recordFrom = kSettle - 0.25;   // the last 0.25 s of the idle run give the oscillator frequencies of this pot state
    const bool ok = net.run(kSettle); net.fixedDt = dt_;
    if (settled_.size() >= 8) settled_.erase(settled_.begin());
    if (!ok) { net.clearProbes(); return nominal_; }   // aborted: the caller sees the abort in the hit itself
    Settled st; st.snap = net.snapshot();
    if (probesOk && net.probes.size() == static_cast<size_t>(kOscillators))
    {
      double f[kOscillators]; bool all = true; for (int k = 0; k < kOscillators; ++k) all = CrossingFrequency(net.probes[static_cast<size_t>(k)], f[k]) && all;
      if (all) { st.table.assign(kPrerollTable, 0.0); BuildPrerollTable(f, st.table.data(), kPrerollTable);
        if (std::getenv("DGG_PART_STATS")) { fprintf(stderr, "pot state oscillators (Hz):"); for (int k = 0; k < kOscillators; ++k) fprintf(stderr, " %.2f", f[k]); fprintf(stderr, "\n"); } }
    }
    net.clearProbes();
    Settled& ref = settled_[key] = std::move(st); table = ref.table.empty() ? nullptr : &ref.table; return ref.snap;
  }
  static const char* Tag(Tr909Voice v)   // sheet tag of the netlist (node and source names)
  {
    switch (v) { case Tr909Voice::Bd: return "BD"; case Tr909Voice::Sd: return "SD"; case Tr909Voice::Lt: return "LT"; case Tr909Voice::Mt: return "MT"; case Tr909Voice::Ht: return "HT"; default: return "HC"; }
  }
  bool Ready()
  {
    if (ready_) return true;
    std::string text; for (int i = 0; i < kTr909CircuitNetLineCount; ++i) { text += kTr909CircuitNetLines[i]; text += '\n'; }
    net_ = std::make_unique<n909::PartNet>(); net_->fixedDt = dt_; net_->H = 20e-6;   // fixed step of the validated runs; relaxation window 20 us, NOISE tolerance 1e-7 V
    net_->splitToms = std::getenv("DGG_PART_NOSPLIT") == nullptr;   // toms block split into LT / HT / MT + noise sheet blocks (Net909Part.h, secondary interfaces TOM_NOISE, TO_NOISE_LT, TO_NOISE_HT); DGG_PART_NOSPLIT=1: one toms block
    if (!net_->build(text)) { net_.reset(); return false; }
    n909::Net::State st;
    st.x.assign(kTr909CircuitIdleState, kTr909CircuitIdleState + kTr909CircuitStateX);
    st.q1.assign(kTr909CircuitIdleState + kTr909CircuitStateX, kTr909CircuitIdleState + kTr909CircuitStateX + kTr909CircuitStateQ);
    st.q2.assign(kTr909CircuitIdleState + kTr909CircuitStateX + kTr909CircuitStateQ, kTr909CircuitIdleState + kTr909CircuitStateX + 2 * kTr909CircuitStateQ);
    st.stepIndex = kTr909CircuitStateStepIndex; st.hPrev = kTr909CircuitStateHPrev;
    if (!net_->loadFullState(st)) { net_.reset(); return false; }
    net_->bypassTol = 1e-4;   // device bypass (Net909.h): validated over full 350 ms hits against NGSpice and the run without bypass (plan, section S4 faster circuit)
    if (const char* bp = std::getenv("DGG_PART_BYPASS")) net_->bypassTol = std::atof(bp);   // test hook: other tolerance, 0 = off
    nominal_ = net_->snapshot(); net_->abort = abort_;
    ready_ = true; return true;
  }
  bool Render(Tr909Voice v, uint64_t params, int accentCode, uint64_t seed, double seconds, int fs, std::vector<float>& out)
  {
    n909::PartNet& net = *net_;
    if (!SetPots(v, params)) return false;
    const std::vector<double>* table = nullptr;
    if (!net.restore(StateFor(v, params, table))) return false;
    const std::string tag = Tag(v), lower = tag == "BD" ? "bd" : tag == "SD" ? "sd" : tag == "LT" ? "lt" : tag == "MT" ? "mt" : tag == "HT" ? "ht" : "hc";
    net.clearProbes(); if (!net.addProbe(lower + "_" + tag + "_OUT")) return false;
    if (const char* r = std::getenv("DGG_PART_IFREL")) net.ifRel = std::atof(r);   // test hook
    const double t0 = kTriggerDelay + (fixedPreroll >= 0.0 ? fixedPreroll : PrerollAt(table, seed));   // trigger time of the hit in the run: the circuit idles on from the stored state before the trigger, so the free-running oscillators are at a different phase for every noise seed (variant)
    for (const char* t : {"BD", "SD", "LT", "MT", "HT", "HC", "RM"}) net.setSourceDc(std::string("VTRIG_") + t, 0.0);
    net.setSource("VTRIG_" + tag, [t0](double t) { return pcmfast::HitTrigger(t - t0); });
    net.setSourceDc("VACCENT_" + tag, Tr909AccentVolts(accentCode));
    pcmfast::HitNoise noise; noise.Start(seed, 5e-6, 0.0);   // the noise of the hit starts at knot 10000 of the sequence (50 ms) when the trigger arrives
    if (!silentNoise) net.setSource("Vnz", [&noise, t0](double t) { return t < t0 ? 7.5 : noise.At(kNoiseAtTrigger + (t - t0)); });
    else net.setSourceDc("Vnz", 7.5);
    const double ts = 1.0 / fs; const size_t nOut = static_cast<size_t>(seconds * fs + 0.5);
    net.fixedDt = dt_; net.recordFrom = t0 - 5e-5;
    // idle run before the trigger (no noise on the NOISE line): event-controlled step (fixed step around comparator flips, up to 3 us between; plan section "Schritt 2"); from 0.2 ms before the trigger on the validated fixed step, with the noise of the hit
    net.quietUntil = std::getenv("DGG_PART_QUIET0") ? -1.0 : t0 - 2e-4; net.eventHMax = 3e-6;
    net.maxIt = (tag == "HC" || tag == "LT" || tag == "MT" || tag == "HT" || std::getenv("DGG_PART_MAXIT6")) ? 6 : 2;   // relaxation passes per window: BD / SD converge in the second pass to -48 dB against the converged run; HC (nonlinear noise path) and the toms (secondary interfaces) run converged
    // Blocks of the voices that are not triggered are only weakly coupled to the NOISE line (measured, plan section "Schritt 3"): they run once per window (no relaxation re-passes) with event-controlled steps;
    // the block of the hit voice (toms: the toms block) always runs with the fixed step and the full relaxation. HC keeps the BD and SD blocks in the relaxation (their noise stages load the line).
    // Test hook DGG_PART_OTHERS0=1: all blocks full (as before).
    { const int own = net.blockOfNodeName(lower + "_" + tag + "_OUT"), kb = net.blockOfNodeName("bd_BD_OUT"), ks = net.blockOfNodeName("sd_SD_OUT"), kn = net.blockOfNodeName("noise_N_712"); const bool tomHit = tag == "LT" || tag == "MT" || tag == "HT"; const bool off = std::getenv("DGG_PART_OTHERS0") != nullptr;
      for (size_t i = 0; i < net.blocks.size(); ++i) { const int bi = static_cast<int>(i); const bool keep = bi == own || (tag == "HC" && (bi == kb || bi == ks)) || (tomHit && bi == kn); net.blocks[i]->weak = !off && own >= 0 && !keep; net.blocks[i]->eventAlways = net.blocks[i]->weak; } }
    const bool runOk = net.run(t0 + static_cast<double>(nOut) * ts + 2e-4); net.quietUntil = -1.0;
    if (!runOk) { out.clear(); return false; }
    if (std::getenv("DGG_PART_STATS")) { fprintf(stderr, "PartNet: windows %ld passes %ld unconverged %ld hist", net.windows, net.passes, net.unconverged); for (int i = 1; i < 8; ++i) fprintf(stderr, " %d:%ld", i, net.itHist[i]); fprintf(stderr, " max mismatch %.3g\n", net.ifMaxMismatch); }
    const auto& pr = net.probes[0];
    if (pr.t.size() < 2) { out.clear(); return false; }
    size_t j = 0;
    auto at = [&](double tt) { while (j + 1 < pr.t.size() - 1 && pr.t[j + 1] < tt) ++j; const double a = std::min(std::max((tt - pr.t[j]) / std::max(pr.t[j + 1] - pr.t[j], 1e-30), 0.0), 1.0); return pr.v[j] + (pr.v[j + 1] - pr.v[j]) * a; };
    const double idle = at(t0);   // output just before the trigger
    out.assign(nOut, 0.0f);
    for (size_t k = 0; k < nOut; ++k) out[k] = static_cast<float>(at(t0 + static_cast<double>(k) * ts) - idle);
    return true;
  }
  static constexpr double kTriggerDelay = 0.05;   // minimum idle before the trigger: the accent (a held level in the sequencer) and the pot states settle as in the NGSpice decks (50 ms)
  static constexpr double kNoiseAtTrigger = 0.05;   // the noise sequence of a hit is read from 50 ms on when the trigger arrives
  static constexpr double kSettle = 0.5;
  std::map<uint64_t, Settled> settled_;
  double dt_; std::function<bool()> abort_;
  std::unique_ptr<n909::PartNet> net_; n909::PartNet::Snapshot nominal_; bool ready_ = false;
};

}  // namespace dgg
