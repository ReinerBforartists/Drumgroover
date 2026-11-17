// TR-909: the complete circuit as the engine of a hit.
//
// Net909 (909Net.h) solves the full Nava netlist of BD, SD, LT, MT, HT, HC, RM and the shared NOISE sheet (482 nodes), like the coupled NGSpice deck:
// the free-running oscillators of the toms, SD and BD, the shared noise line, the shared tom shaping filter and the slow RC states are parts of ONE circuit. The matrix is split along the
// structure of the circuit by 909NetPart.h (PartNet: blocks BD, SD, HC, RM and the tom sheets, coupled through the shared NOISE line by waveform relaxation, validated against the monolithic
// network and against NGSpice, plan tools/909_GESAMTSCHALTUNG_PLAN.md section S4). This runner renders a hit by
// continuing that circuit from an idle state that was run for one second (909_circuit_all7_state.h) with the trigger pulse of the voice, the accent voltage and the noise source of the
// hit, and returns the output of the voice minus its idle level at the sample rate of the request. It is a Tr909ModelRunner (the bake engine of BD, SD, LT, MT, HT, HC).
//
// Not a real-time engine (a hit takes minutes on one thread); the bake and the reference comparisons use it. Supported: voices BD, SD, LT, MT, HT, HC at any pot state of the voice (909PotParams.h; the pots are the circuit pots of the netlist), the 5 us reference noise path.
// Anything else returns false (no silent replacement by another engine).
#pragma once
#include "909BakeHost.h"
#include "909PotParams.h"
#include "909Net.h"
#include "909NetPart.h"
#include "909NetPhase.h"
#include "909HitNoise.h"
#include "909_circuit_all7_net.h"
#include "909_circuit_all7_state.h"
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
  // splitNoise: the network of this runner has the noise sheet as a block of its own (MT split off, PartNet::splitNoise): used for the tom voices LT / MT / HT (Tr909BakeRunner), see SplitNoiseVoice.
  explicit Tr909CircuitRunner(double dt = 1e-6, std::function<bool()> abort = nullptr, bool splitNoise = false) : dt_(dt), abort_(std::move(abort)), splitNoise_(splitNoise) {}
  // The split of the noise sheet is used for the toms only: it saves 10-31 % of their render time (E-03 phase 2); for BD / SD / HC it brings no gain (and moves HC beyond the 0.0254 V gate against the stand without the split).
  static bool SplitNoiseVoice(Tr909Voice v) { return v == Tr909Voice::Lt || v == Tr909Voice::Mt || v == Tr909Voice::Ht; }

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
  // Fine step after the trigger of an SD hit (audits/SD_COUPLED_NGSPICE_REFERENCE.md, addendum 6): the fixed step of 1 us (BDF2) falsifies the trigger edge event of the SD sheet (node sd_N_157: NGSpice 0.2098 V, 1 us 0.4599 V,
  // 0.1 us 0.2081 V, 20 us after the trigger); from the trigger time t0 for kFineWidth the net runs with kFineDt, before and after with the step of the runner. Validated for SD only (0.02 us / 20 us since addendum 8: spike of the SD output at 5 us 0.0278 V against NGSpice 0.0282 V; control: the split alone changes nothing).
  static constexpr double kFineDt = 2e-8, kFineWidth = 2e-5;
  // HC: base step 0.2 us (2026-11-14, report Nachtrag 19 continuation 6): against the NGSpice reference with the production noise start dt 1 us exceeds -30 dB in 0-1 ms at 4.064 V (seeds 2, 3: -23 dB), dt 0.2 us is at or below -37 dB in all six cases. About 3x the compute time; provisional accuracy step, not an optimisation.
  static constexpr double kHcDt = 2e-7;
  // Further ramps of the trigger source (addendum 11 / 12): the falling edge of pcmfast::HitTrigger (t0 + 2.01 ms ... 2.02 ms) is the same kind of event as the rising edge (the SD output jumps by 11 mV at 0.8 V / 126 mV at 4.064 V; error of the
  // 1 us step in 2.01-2.04 ms: 332 / 1082 uV rms, with the fine step 14 / 64 uV). The ramps (segments with a non-zero slope) are found by sampling HitTrigger (no fixed times); each gets the window [ramp start, ramp start + 2 x ramp length], relative to t0.
  static const std::vector<std::pair<double, double>>& TriggerRampWindows()
  {
    static const std::vector<std::pair<double, double>> w = [] {
      std::vector<std::pair<double, double>> r; const double h = 0.25e-6, tmax = 10e-3, slopeMin = 1e3; bool in = false; double a = 0.0;
      for (long i = 0; i * h < tmax; ++i) { const double t = -1e-6 + static_cast<double>(i) * h; const double s = (pcmfast::HitTrigger(t + h) - pcmfast::HitTrigger(t)) / h; const bool ramp = std::fabs(s) > slopeMin;
        if (ramp && !in) { in = true; a = std::max(t, 0.0); } else if (!ramp && in) { in = false; r.emplace_back(std::round(a * 1e9) * 1e-9, std::round((a + 2.0 * (t - a)) * 1e9) * 1e-9); } }
      return r; }();
    return w;
  }
  // Runs the hit to tEnd. The first window is the rising edge [t0, t0 + fineWidth] (validated, addendum 9); with extraRamps every further ramp of the trigger that starts after it gets a window of the same fine step (addendum 12).
  static bool RunHit(n909::PartNet& net, double t0, double tEnd, bool fineAfterTrigger, double fineDt = kFineDt, double fineWidth = kFineWidth, bool extraRamps = true)
  {
    if (!fineAfterTrigger || fineDt <= 0.0 || fineWidth <= 0.0) return net.run(tEnd);
    const double dt0 = net.fixedDt;
    auto fineWindow = [&](double a, double b) {   // windows end exactly at a and b (run(tstop) clips the last window to tstop)
      bool ok = true; if (a > net.T) { net.fixedDt = dt0; for (auto& bl : net.blocks) bl->hCur = dt0; ok = net.run(std::min(a, tEnd)); }
      if (ok && a < tEnd) { net.fixedDt = fineDt; for (auto& bl : net.blocks) bl->hCur = fineDt; ok = net.run(std::min(b, tEnd)); }
      net.fixedDt = dt0; for (auto& bl : net.blocks) bl->hCur = dt0; return ok; };
    bool ok = fineWindow(t0, t0 + fineWidth);
    if (extraRamps) for (const auto& w : TriggerRampWindows()) { if (!ok || t0 + w.first >= tEnd) break; if (w.first < fineWidth) continue; ok = fineWindow(t0 + w.first, t0 + w.second); }
    if (ok) ok = net.run(tEnd);
    return ok;
  }
  // ---- E-02: free-running oscillators as a hybrid automaton (Net909Phase.h). The phase of every oscillator of the sheet(s) of the voice is set directly from the stored circuit state: advance along the ramps
  // by a fraction of the period given by the noise seed (low-discrepancy sequence over the phases of the sheet: independent phases) or by a time (timeShift >= 0, validation), instead of idling for Preroll(seed).
  // The sheet's ramp parameters and the state map are measured per pot state from a 0.3 s idle run of the sheet's block (cached, SheetPhase::fit). BD: sheet 0 (1 oscillator); SD: 1 (2); LT / MT / HT: 2 / 3 / 4 (3 each);
  // HC: the BD and SD sheets (their noise stages stay in the relaxation of an HC hit).
  // Phases of the oscillators from the automaton (Net909Phase.h): standard since 2026-11-16 (DETAILS 10.28); DGG_PART_PHASE=0 / DGG_PART_PREROLL=1 / CT_PREROLL: idle run with Preroll(seed).
  bool UsePhase() const { if (const char* e = std::getenv("DGG_PART_PHASE")) return std::atoi(e) != 0; return std::getenv("DGG_PART_PREROLL") == nullptr && fixedPreroll < 0.0; }   // 2026-11-16: standard again (DETAILS 10.28); DGG_PART_PHASE=0 / DGG_PART_PREROLL=1 / fixedPreroll (CT_PREROLL) = the idle run with Preroll(seed)
  static const std::vector<n909::OscPair>& SheetPairs(int which)
  {
    static const std::vector<n909::OscPair> bd = {{"bd_N_60", "bd_N_59"}}, sd = {{"sd_N_115", "sd_R263"}, {"sd_N_106", "sd_N_101"}}, lt = {{"lt_N_177", "lt_N_181"}, {"lt_N_197", "lt_N_201"}, {"lt_N_187", "lt_N_191"}},
      mt = {{"mt_N_333", "mt_N_337"}, {"mt_N_313", "mt_N_317"}, {"mt_N_323", "mt_N_327"}}, ht = {{"ht_N_630", "ht_N_683"}, {"ht_N_689", "ht_N_693"}, {"ht_N_699", "ht_N_703"}};
    switch (which) { case 0: return bd; case 1: return sd; case 2: return lt; case 3: return mt; default: return ht; }
  }
  static int SheetOfVoice(Tr909Voice v) { switch (v) { case Tr909Voice::Bd: return 0; case Tr909Voice::Sd: return 1; case Tr909Voice::Lt: return 2; case Tr909Voice::Mt: return 3; case Tr909Voice::Ht: return 4; default: return -1; } }
  static Tr909Voice VoiceOfSheet(int which) { switch (which) { case 0: return Tr909Voice::Bd; case 1: return Tr909Voice::Sd; case 2: return Tr909Voice::Lt; case 3: return Tr909Voice::Mt; default: return Tr909Voice::Ht; } }
  bool CalibrateSheet(int which, uint64_t ownParams, n909::SheetPhase& sp)
  {
    n909::PartNet& net = *net_; const Tr909Voice sv = VoiceOfSheet(which); if (!SetPots(sv, ownParams)) return false;
    const std::vector<double>* table = nullptr; if (!net.restore(StateFor(sv, ownParams, table))) return false;
    if (!sp.setup(net, SheetPairs(which))) return false; sp.wantOrbit = (which == 1) && std::getenv("DGG_PART_NOORBIT") == nullptr;   // SD: the two oscillators are coupled and locked (22:15): orbit mode
    for (const char* t : {"BD", "SD", "LT", "MT", "HT", "HC", "RM"}) net.setSourceDc(std::string("VTRIG_") + t, 0.0);
    net.setSourceDc("Vnz", 7.5); for (const char* t : {"BD", "SD", "LT", "MT", "HT", "HC", "RM"}) net.setSourceDc(std::string("VACCENT_") + t, 2.4);
    for (size_t i = 0; i < net.blocks.size(); ++i) { net.blocks[i]->passive = static_cast<int>(i) != sp.block; net.blocks[i]->frozenIdle = net.blocks[i]->passive; for (bool* f : {&net.blocks[i]->fineEvents}) *f = false; }
    net.freezeIdle = true; net.fineEventDt = 0.0; net.fixedDt = dt_ * (std::getenv("DGG_PART_PHASEDT") ? std::atof(std::getenv("DGG_PART_PHASEDT")) : 0.1); net.eventHMax = 3e-6; net.statR = 1.0; net.qaRtol = 1e-3; net.qaAtol = 1e-4; net.qaHmax = 1e-4; net.maxIt = 2;   // DGG_PART_PHASEDT: factor on the base step of the calibration run (edges)
    net.quietUntil = 1e3; const double qh = net.quietH; net.quietH = kPhaseSample; net.clearProbes(); net.recordFrom = 1e9;
    sp.rt.clear(); sp.rx.clear(); const int blk = sp.block; const double tSkip = 0.01;
    net.afterWindow = [&](double t) { if (t < tSkip) return; sp.rt.push_back(t); const auto& xv = net.blocks[static_cast<size_t>(blk)]->net.x; sp.rx.insert(sp.rx.end(), xv.begin(), xv.end()); };
    const bool ok = net.run(kPhaseCalSeconds); net.afterWindow = nullptr; net.quietUntil = -1.0; net.quietH = qh; net.freezeIdle = false; net.fixedDt = dt_;
    if (std::getenv("DGG_PART_PHASELOG")) fprintf(stderr, "sheet %d calibration: run %s, %zu samples\n", which, ok ? "ok" : "FAILED", sp.rt.size());
    const bool fitted = ok && sp.fit();
    if (std::getenv("DGG_PART_PHASELOG") && fitted) if (sp.orbit) fprintf(stderr, "   orbit: period %.4f ms, %zu samples\n", 1e3 * sp.TL, sp.NL); for (const auto& o : sp.osc) fprintf(stderr, "   osc: xlo %.4f xhi %.4f su %.2f sd %.2f f %.4f Hz\n", o.xlo, o.xhi, o.su, o.sd, 1.0 / o.period());
    return fitted;
  }
  bool ApplyPhase(Tr909Voice v, uint64_t params, uint64_t seed, double timeShift = -1.0)
  {
    if (!Ready()) return false; n909::PartNet& net = *net_; std::vector<int> sheets;
    if (v == Tr909Voice::Hc) { sheets = {0, 1}; } else { const int w = SheetOfVoice(v); if (w < 0) return true; sheets = {w}; }
    std::vector<n909::SheetPhase*> sp; const auto cur = net.snapshot(); bool calibrated = false;
    for (int w : sheets) {
      const uint64_t own = (SheetOfVoice(v) == w) ? params : 0; const uint64_t key = ((own << 3) ^ static_cast<uint64_t>(w)) * 8u + static_cast<uint64_t>(w); auto it = sheets_.find(key);
      if (it == sheets_.end()) { n909::SheetPhase s; if (!CalibrateSheet(w, own, s)) { if (std::getenv("DGG_PART_PHASELOG")) fprintf(stderr, "sheet %d: %s\n", w, s.why.c_str()); SetPots(v, params); net.restore(cur); return false; } calibrated = true; if (sheets_.size() >= 16) sheets_.erase(sheets_.begin()); it = sheets_.emplace(key, std::move(s)).first; }
      sp.push_back(&it->second);
    }
    if (calibrated || true) { if (!SetPots(v, params)) return false; if (!net.restore(cur)) return false; }
    static const double alpha[3] = {0.8191725133961645, 0.6710436067037893, 0.5497004779019703};   // 1 / g^k, g = 1.2207440846057596 (generalised golden ratio, 3 dimensions)
    int kk = 0;
    for (n909::SheetPhase* p : sp) {
      std::vector<double> dts(p->osc.size());
      for (size_t i = 0; i < p->osc.size(); ++i, ++kk) { if (timeShift >= 0.0) dts[i] = timeShift; else { const double r = 0.5 + static_cast<double>(seed) * alpha[kk % 3]; dts[i] = (r - std::floor(r)) * p->osc[i].period(); } }
      std::vector<double> x0 = net.blocks[static_cast<size_t>(p->block)]->net.x, xn;
      if (const char* xsEnv = std::getenv("DGG_PART_PHASEXS")) {   // validation: triangle values and comparator signs "x1,s1,x2,s2,..." of the sheet (from an NGSpice trace)
        std::vector<double> xt, st; { std::string t = xsEnv; size_t a = 0; std::vector<double> v; while (a < t.size()) { size_t b = t.find(',', a); if (b == std::string::npos) b = t.size(); v.push_back(std::atof(t.substr(a, b - a).c_str())); a = b + 1; } for (size_t q = 0; q + 1 < v.size(); q += 2) { xt.push_back(v[q]); st.push_back(v[q + 1]); } }
        if (xt.size() != p->osc.size()) return false;
        if (p->orbit) p->orbitState(static_cast<double>(p->orbitNearest(xt, st)) * p->dtS, xn); else p->moveTo(x0, xt, st, xn);
      } else
      if (p->orbit) { if (timeShift >= 0.0) p->orbitState(p->orbitPosition(x0) + timeShift, xn); else { const double r = 0.5 + static_cast<double>(seed) * alpha[(kk - static_cast<int>(p->osc.size())) % 3]; p->orbitState((r - std::floor(r)) * p->TL, xn); } }   // seed: absolute position on the orbit
      else if (timeShift >= 0.0) p->moveState(x0, dts, xn);
      else p->moveAbsolute(x0, dts, xn);   // seed: absolute phases (time after the lower turning point), independent of the phase of the stored state
      if (!net.setBlockX(p->block, xn)) return false;
      if (const char* pd = std::getenv("DGG_PART_PHASEDUMP")) { FILE* df = fopen(pd, "a"); if (df) { const n909::Net& bn = net.blocks[static_cast<size_t>(p->block)]->net; for (int ni = 1; ni < bn.nNodes; ++ni) fprintf(df, "%s %.9g %.9g\n", bn.nodeName[static_cast<size_t>(ni)].c_str(), x0[static_cast<size_t>(ni - 1)], bn.x[static_cast<size_t>(ni - 1)]); fclose(df); } }   // test hook: node values of the block before / after the phase move
    }
    return true;
  }
  static constexpr double kPhaseSample = 20e-6, kPhaseCalSeconds = 0.3;
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
  std::map<uint64_t, n909::SheetPhase> sheets_;   // E-02: oscillator automata per sheet and pot state
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
    bool probesOk = !UsePhase(); if (probesOk) for (int k = 0; k < kOscillators; ++k) probesOk = net.addProbe(OscNodes()[k]) && probesOk;   // the Preroll table (oscillator frequencies of the pot state) is only needed without the automaton (E-02: its phases come from SheetPhase)
    const double settleSec = std::getenv("DGG_PART_SETTLE") ? std::atof(std::getenv("DGG_PART_SETTLE")) : (UsePhase() ? kSettlePhase : kSettle);   // test hook: length of the settle run (default kSettle; with the oscillator automaton kSettlePhase: the slow pot-network nodes are settled after 50 ms, hits equal to the 0.5 s run to -44...-78 dB, DETAILS 10.24)
    net.recordFrom = settleSec - 0.25;   // the last 0.25 s of the idle run give the oscillator frequencies of this pot state
    if (std::getenv("DGG_PART_SETTLEEV") != nullptr && std::atoi(std::getenv("DGG_PART_SETTLEEV")) != 0)   // test hook (E-02): the settle run with the event-controlled, error-controlled steps of the idle run before the trigger (all blocks, no resting blocks: the twelve oscillator frequencies are needed)
    { net.quietUntil = kSettle + 1.0; const int se = std::atoi(std::getenv("DGG_PART_SETTLEEV")); net.eventHMax = (se == 4) ? 1e-5 : 3e-6; net.statR = (se == 3) ? 0.0 : 1.0; net.qaRtol = (se == 2 || se == 4) ? 0.0 : 1e-3; net.qaAtol = 1e-4; net.qaHmax = 1e-4; net.freezeIdle = false; for (auto& bp : net.blocks) { bp->passive = true; bp->hLast = 0.0; } }
    FILE* slog = nullptr; if (const char* sl = std::getenv("DGG_PART_SETTLELOG")) { slog = fopen(sl, "w"); const int sb = net.blockOfNodeName(std::string(Tag(v) == std::string("BD") ? "bd" : Tag(v) == std::string("SD") ? "sd" : Tag(v) == std::string("LT") ? "lt" : Tag(v) == std::string("MT") ? "mt" : "ht") + "_" + Tag(v) + "_OUT");   // test hook: own block, all unknowns every 5 ms of the settle run
      if (slog && sb >= 0) { static double nextLog = 0.0; nextLog = 0.0; { const n909::Net& bn = net.blocks[static_cast<size_t>(sb)]->net; fprintf(slog, "# names"); for (int ni = 1; ni < bn.nNodes; ++ni) fprintf(slog, " %s", bn.nodeName[static_cast<size_t>(ni)].c_str()); fprintf(slog, "\n"); } net.afterWindow = [&net, sb, slog](double t) { if (t + 1e-12 < nextLog) return; nextLog += 5e-3; const auto& xv = net.blocks[static_cast<size_t>(sb)]->net.x; fprintf(slog, "%.6f", t); for (double q : xv) fprintf(slog, " %.7g", q); fprintf(slog, "\n"); }; } }
    const bool ok = net.run(settleSec); net.quietUntil = -1.0; net.fixedDt = dt_; if (slog) { fclose(slog); net.afterWindow = nullptr; }
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
    net_->splitNoise = std::getenv("DGG_PART_NOSPLITNS") ? false : (splitNoise_ || std::getenv("DGG_PART_SPLITNS") != nullptr);   // E-03 phase 2: the noise sheet is a block of its own, MT split off along TO_NOISE_MT (tom runner; DGG_PART_SPLITNS=1 forces it, DGG_PART_NOSPLITNS=1 switches it off)
    net_->modalOn = std::getenv("DGG_PART_MODAL") ? std::atoi(std::getenv("DGG_PART_MODAL")) != 0 : true;   // E-03: passive blocks that load the NOISE line as linear port models
    net_->passTol = std::getenv("DGG_PART_PASSTOL") ? std::atof(std::getenv("DGG_PART_PASSTOL")) : 1.0;
    net_->nzInside = std::getenv("DGG_PART_NZIN") ? std::atoi(std::getenv("DGG_PART_NZIN")) : 2;   // the NOISE line is solved inside the blocks of the hit (PartNet::nzInside, set before build, plan E-03): 2 = one pass per block set (two for several blocks); test hook DGG_PART_NZIN=0 relaxation as before, 1 = interfaces iterated
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
    const bool usePhase = UsePhase();   // E-02: phases from the oscillator automaton instead of the idle Preroll
    if (usePhase && !ApplyPhase(v, params, seed, std::getenv("DGG_PART_PHASETIME") ? std::atof(std::getenv("DGG_PART_PHASETIME")) : -1.0)) return false;
    const std::string tag = Tag(v), lower = tag == "BD" ? "bd" : tag == "SD" ? "sd" : tag == "LT" ? "lt" : tag == "MT" ? "mt" : tag == "HT" ? "ht" : "hc";
    net.clearProbes(); if (!net.addProbe(lower + "_" + tag + "_OUT")) return false;
    if (const char* r = std::getenv("DGG_PART_IFREL")) net.ifRel = std::atof(r);   // test hook
    const double t0 = kTriggerDelay + (usePhase ? 0.0 : (fixedPreroll >= 0.0 ? fixedPreroll : PrerollAt(table, seed)));   // trigger time of the hit in the run: the circuit idles on from the stored state before the trigger, so the free-running oscillators are at a different phase for every noise seed (variant)
    for (const char* t : {"BD", "SD", "LT", "MT", "HT", "HC", "RM"}) net.setSourceDc(std::string("VTRIG_") + t, 0.0);
    net.setSource("VTRIG_" + tag, [t0](double t) { return pcmfast::HitTrigger(t - t0); });
    net.setSourceDc("VACCENT_" + tag, Tr909AccentVolts(accentCode));
    pcmfast::HitNoise noise; noise.Start(seed, 5e-6, 0.0);   // the noise of the hit starts at knot 10000 of the sequence (50 ms) when the trigger arrives
    if (!silentNoise) net.setSource("Vnz", [&noise, t0](double t) { return t < t0 ? 7.5 : noise.At(kNoiseAtTrigger + (t - t0)); });
    else net.setSourceDc("Vnz", 7.5);
    const double ts = 1.0 / fs; const size_t nOut = static_cast<size_t>(seconds * fs + 0.5);
    net.fixedDt = (v == Tr909Voice::Hc && dt_ > kHcDt) ? kHcDt : dt_; if (v == Tr909Voice::Hc) if (const char* hd = std::getenv("DGG_PART_HCDT")) net.fixedDt = std::atof(hd);   // test hook: other HC base step (default kHcDt)
    net.recordFrom = t0 - 5e-5;
    net.bpOrigin = t0; net.bpStep = std::getenv("DGG_PART_NOBP") ? 0.0 : 5e-6; { const double kr = std::getenv("DGG_PART_KFREL") ? std::atof(std::getenv("DGG_PART_KFREL")) : 0.25; net.kfDt = kr * net.fixedDt; net.kfWidth = kr > 0.0 ? net.fixedDt * (std::getenv("DGG_PART_KFWREL") ? std::atof(std::getenv("DGG_PART_KFWREL")) : (v == Tr909Voice::Sd ? 3.0 : 1.0)) : 0.0; }   // finer step (a quarter of the base step) for the first base step after every knot; DGG_PART_KFREL=0 off   // E-03: steps end at the knots of the noise source (HitNoise step 5 us from the trigger); DGG_PART_NOBP=1: as before
    // idle run before the trigger (no noise on the NOISE line): event-controlled step (fixed step around comparator flips, up to 3 us between; plan section "Schritt 2"); from 0.2 ms before the trigger on the validated fixed step, with the noise of the hit
    net.quietUntil = std::getenv("DGG_PART_QUIET0") ? -1.0 : t0 - 2e-4; net.eventHMax = std::getenv("DGG_PART_EVHMAX") ? std::atof(std::getenv("DGG_PART_EVHMAX")) : 3e-6; if (const char* em = std::getenv("DGG_PART_EVMARGIN")) net.eventMargin = std::atof(em); if (const char* eh = std::getenv("DGG_PART_EVHOLD")) net.eventHold = std::atof(eh);   // test hooks: look-ahead margin and hold time of the comparator-flip prediction   // test hook DGG_PART_EVHMAX: longest step between comparator events in the idle run (default 3 us)
    if (const char* qh = std::getenv("DGG_PART_QUIETH")) net.quietH = std::atof(qh);   // test hook (E-02): window length of the idle run (default 60 us)
    const bool idleOld = std::getenv("DGG_PART_IDLEOLD") != nullptr;   // E-02: DGG_PART_IDLEOLD=1 restores the idle run of before (statR 0, steps between events 3 us, no resting blocks)
    net.statR = std::getenv("DGG_PART_STATR") ? std::atof(std::getenv("DGG_PART_STATR")) : (idleOld ? 0.0 : 1.0);   // idle run: a saturated op-amp counts as a flip candidate only above 1 V/s (the stationary accent stage forced 94 % of the 1 us steps)
    { net.qaRtol = idleOld ? 0.0 : 1e-3; net.qaAtol = 1e-4; net.qaHmax = 1e-4; if (const char* qa = std::getenv("DGG_PART_QADAPT")) { double a = 0, b = 1e-5, c = 1e-4; if (std::sscanf(qa, "%lf,%lf,%lf", &a, &b, &c) >= 1) { net.qaRtol = a; net.qaAtol = b; net.qaHmax = c; } } }   // test hook (E-02): DGG_PART_QADAPT=rtol,atol,hmax error-controlled steps between the comparator events of the idle run
    net.maxIt = (tag == "HC" || tag == "LT" || tag == "MT" || tag == "HT" || std::getenv("DGG_PART_MAXIT6")) ? 6 : 2;   // relaxation passes per window: BD / SD converge in the second pass to -48 dB against the converged run; HC (nonlinear noise path) and the toms (secondary interfaces) run converged
    // E-03: the blocks of the voices that are not played are passive: coarse event-controlled steps, they rest in the idle run. Those among them that only load the NOISE line (BD, SD, HC) are replaced in the noisy windows by
    // their linear port model (PartNet::modalOn, Net909Modal.h); the noise sheet block (nonlinear op-amp stage, owner of TOM_NOISE) solves the line with the coarse step of a passive block. The block of the hit voice
    // (toms: the toms block, and the noise sheet block) runs with the fixed step. Test hooks: DGG_PART_OTHERS0=1: no block passive; DGG_PART_MODAL=0: the passive blocks that load the line solve it themselves.
    { const int own = net.blockOfNodeName(lower + "_" + tag + "_OUT"), kb = net.blockOfNodeName("bd_BD_OUT"), ks = net.blockOfNodeName("sd_SD_OUT"), kn = net.blockOfNodeName("noise_N_712"); const bool tomHit = tag == "LT" || tag == "MT" || tag == "HT"; const bool off = std::getenv("DGG_PART_OTHERS0") != nullptr;
      for (size_t i = 0; i < net.blocks.size(); ++i) { const int bi = static_cast<int>(i); const bool keep = bi == own || (tomHit && bi == kn); net.blocks[i]->passive = !off && own >= 0 && !keep; net.blocks[i]->frozenIdle = net.blocks[i]->passive && !(tag == "HC" && (bi == kb || bi == ks)); }   // HC: the BD and SD blocks idle on before the trigger (their slow states settle: frozen at the stored state the first 0.5 ms of the hit are off by -13 dB), as ports of the line they are modelled in the noisy windows
      // BD (addendum 14): event-controlled fine step around comparator flips in the blocks that run full (PartNet::fineEventDt, Net909Part.h); validated against NGSpice for BD up to 60 ms after the trigger. All other voices: off (the net is shared between voices, so it is set on every hit).
      net.fineEventDt = (v == Tr909Voice::Bd || v == Tr909Voice::Sd || v == Tr909Voice::Lt || v == Tr909Voice::Mt || v == Tr909Voice::Ht) ? kFineDt : 0.0; if (const char* fe = std::getenv("DGG_PART_FINEEV")) net.fineEventDt = std::atof(fe); net.fineNodes.clear(); if (const char* fm = std::getenv("DGG_PART_FINEMARGIN")) net.fineMargin = std::atof(fm); if (const char* fh = std::getenv("DGG_PART_FINEHOLD")) net.fineHold = std::atof(fh); if (v != Tr909Voice::Bd && !std::getenv("DGG_PART_FINEALL")) for (int k = 0; k < kOscillators; ++k) net.fineNodes.insert(OscNodes()[k]); if (v != Tr909Voice::Bd && !std::getenv("DGG_PART_FINEALL")) for (const char* an : {"sd_N_154", "lt_N_229", "mt_N_364", "ht_N_765"}) net.fineNodes.insert(an);   // and the accent stages (their level change at the trigger shifts the oscillator phase)   // E-02 (DETAILS 10.26 / 10.27): the oscillator voices run the fine step around the comparator flips (oscillator comparators and accent stage, PartNet::fineNodes, statR rule): hit half periods against NGSpice -1e-4...-8e-4 -> <= 2e-4, phase-impulse outliers MT k = 4 -26 -> -53 dB; hit time +25...+43 %; DGG_PART_FINEEV=0 = old (only BD)
      for (auto& bp : net.blocks) bp->fineEvents = net.fineEventDt > 0.0 && !bp->passive; }
    net.freezeIdle = std::getenv("DGG_PART_FREEZE") ? std::atoi(std::getenv("DGG_PART_FREEZE")) != 0 : !idleOld;   // test hook (E-02): the blocks of the other voices rest during the idle run before the trigger
    const bool runOk = RunHit(net, t0, t0 + static_cast<double>(nOut) * ts + 2e-4, v == Tr909Voice::Sd || v == Tr909Voice::Bd); net.quietUntil = -1.0;
    if (!runOk) { out.clear(); return false; }
    if (std::getenv("DGG_PART_STATS")) { for (auto& bp : net.blocks) if (bp->qFixed + bp->qAd > 0) { fprintf(stderr, "quiet block %s: fixed steps %ld (%.1f ms), controlled steps %ld (%.1f ms)\n", bp->name.c_str(), bp->qFixed, 1e3 * bp->qFixedT, bp->qAd, 1e3 * bp->qAdT); fprintf(stderr, "   hold steps %ld, predicted-flip steps %ld; by op-amp (out node): ", bp->nHold, bp->nPred); for (size_t oi = 0; oi < bp->predBy.size(); ++oi) if (bp->predBy[oi] || bp->statBy[oi]) fprintf(stderr, "%s pred %ld stat %ld; ", bp->net.nodeName[static_cast<size_t>(bp->net.op[oi].out)].c_str(), bp->predBy[oi], bp->statBy[oi]); fprintf(stderr, "\n"); bp->nHold = bp->nPred = 0; bp->predBy.clear(); bp->statBy.clear(); bp->qFixed = bp->qAd = 0; bp->qFixedT = bp->qAdT = 0.0; } }
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
  static constexpr double kSettle = 0.5, kSettlePhase = 0.05;
  std::map<uint64_t, Settled> settled_;
  double dt_; std::function<bool()> abort_;
  std::unique_ptr<n909::PartNet> net_; n909::PartNet::Snapshot nominal_; bool ready_ = false; bool splitNoise_ = false;
};

}  // namespace dgg
