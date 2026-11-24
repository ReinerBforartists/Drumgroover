// TR-909: the complete circuit as the engine of a hit.
//
// Net909 (909Net.h) solves the full Nava netlist of BD, SD, LT, MT, HT, HC, RM and the shared NOISE sheet (482 nodes), like the coupled NGSpice deck:
// the free-running oscillators of the toms, SD and BD, the shared noise line, the shared tom shaping filter and the slow RC states are parts of ONE circuit. The matrix is split along the
// structure of the circuit by 909NetPart.h (PartNet: blocks BD, SD, HC, RM and the tom sheets, coupled through the shared NOISE line by waveform relaxation, validated against the monolithic
// network and against NGSpice, plan tools/909_GESAMTSCHALTUNG_PLAN.md section S4). This runner renders a hit by
// continuing that circuit from an idle state that was run for one second (909_circuit_all7_state.h) with the trigger pulse of the voice, the accent voltage and the noise source of the
// hit, and returns the output of the voice minus its idle level at the sample rate of the request. It is a ModelRunner909 (the bake engine of BD, SD, LT, MT, HT, HC).
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
#include "909OutputFilter.h"
#include "909_circuit_all7_net.h"
#include "909_circuit_all7_state.h"
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace dgg {

class CircuitRunner909 : public ModelRunner909
{
public:
  // dt: integration step of the circuit (1 us: the step at which the oscillator frequencies of the circuit were validated against NGSpice). abort: polled between steps, true stops the hit (the run then fails).
  // splitNoise: the network of this runner has the noise sheet as a block of its own (MT split off, PartNet::splitNoise): used for the tom voices LT / MT / HT (BakeRunner909), see SplitNoiseVoice.
  explicit CircuitRunner909(double dt = 1e-6, std::function<bool()> abort = nullptr, bool splitNoise = false) : dt_(dt), abort_(std::move(abort)), splitNoise_(splitNoise) {}
  // The split of the noise sheet is used for the toms only: it saves 10-31 % of their render time (E-03 phase 2); for BD / SD / HC it brings no gain (and moves HC beyond the 0.0254 V gate against the stand without the split).
  static bool SplitNoiseVoice(Voice909 v) { return v == Voice909::Lt || v == Voice909::Mt || v == Voice909::Ht; }

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
  // SD-04: from 1 ms after the trigger the fine event step around comparator flips is coarser for SD / LT / MT / HT (the 20 ns steps are 42 % of the steps of the SD hit): window errors against NGSpice (accents 4.064 / 0.8 V) equal to or within 0.4 dB of 20 ns, oscillator frequency error against NGSpice 2.5-6.9e-5 instead of 5.9-12.9e-5 (SD); hit time SD -40 %, LT -27 %, MT -27 %, HT -34 %; BD: no effect on time, unchanged
  static constexpr double kFineLateFromT = 1e-3;
  static double FineLateDt(Voice909 v) { switch (v) { case Voice909::Sd: return 1e-7; case Voice909::Lt: return 7e-8; case Voice909::Mt: return 1e-7; case Voice909::Ht: return 7e-8; default: return 0.0; } }
  // HC: base step 0.2 us (2026-11-14, report Nachtrag 19 continuation 6): against the NGSpice reference with the production noise start dt 1 us exceeds -30 dB in 0-1 ms at 4.064 V (seeds 2, 3: -23 dB), dt 0.2 us is at or below -37 dB in all six cases. About 3x the compute time; provisional accuracy step, not an optimisation.
  static constexpr double kHcDt = 2e-7;
  // HC-01: staircase of the base step after the trigger (the first stage is kHcDt): 0.2 us to 20 ms, 0.5 us to 60 ms, 1 us to 100 ms, 2.5 us after. Against the NGSpice window errors of the constant 0.2 us the cells to 60 ms change by at most 0.3 dB (accents 4.064 / 0.8 V, seeds 1-3); the windows from 60 ms (no NGSpice reference) are at or below -33 dB against the 0.2 us run (that run is within -54 dB of 0.1 us there), 0.8 V ... 4.064 V, seeds, Level pot 0.1 / 0.9. 5 us from 100 ms: -24 dB.
  static constexpr int kHcStages = 3; static constexpr double kHcStageT[kHcStages] = {0.02, 0.06, 0.1}, kHcStageDt[kHcStages] = {5e-7, 1e-6, 2.5e-6};
  static bool RunHcStaged(n909::PartNet& net, double t0, double tEnd)
  {
    bool ok = net.run(std::min(t0 + kHcStageT[0], tEnd));
    for (int i = 0; ok && i < kHcStages && t0 + kHcStageT[i] < tEnd; ++i) { net.fixedDt = kHcStageDt[i]; for (auto& bl : net.blocks) bl->hCur = kHcStageDt[i]; ok = net.run(i + 1 < kHcStages ? std::min(t0 + kHcStageT[i + 1], tEnd) : tEnd); }
    return ok;
  }
  // SD-03: from kSdTailFromT after the trigger the step after a noise knot is kSdTailKnotDt (uniform over the knot interval) instead of 0.25 x the base step (0.625 us): windows 100-350 ms against NGSpice within 0.3 dB of before (accents 4.064 / 0.8 V, seeds 1 / 2), -16 % hit time; 1.667 us (-22 %) and 2.5 us worsen the windows 100-350 ms by 1.7-5 dB
  static constexpr double kSdTailFromT = 0.1, kSdTailKnotDt = 1.25e-6;
  static constexpr double kFineHitDt = 1e-6;   // LT / MT / HT: base step of the hit; the oscillator periods and the output windows against NGSpice need 1 us (2.5 us: period errors HT 2.2e-4, LT 1.9e-4, MT 1.05e-4)
  static constexpr double HitDt(Voice909 v) { return (v == Voice909::Ht || v == Voice909::Mt || v == Voice909::Lt) ? kFineHitDt : kHitDt; }
  static constexpr double kHitDt = 2.5e-6;   // Z-01: base step of the hit for BD / SD (a divisor of the 5 us noise knots; 12-25 % faster than 1 us, window errors against NGSpice within -4...+2 dB of 1 us); idle run, calibration and settle keep dt_
  // Z-02: BD tail plan (accent code >= kBdTailMinCode). The noise source keeps running (the earlier freeze of the source from 60 ms on was 30-39 dB off against NGSpice at decay pot >= 0.5). Until kBdTailT after the trigger the regime of the hit continues (steps end at the 5 us knots, fine steps after every knot: mean step 0.83 us); from kBdTailT the base step is kBdTailDt with event-controlled steps up to kBdTailEvHMax between comparator flips and the steps end at the knots, at most kBdTailKnotDt for kBdTailKnotWidth after each knot (kBdTailKnotWidth = the knot interval: uniform 1.25 us steps; a 1.25 + 1.25 + 2.5 us pattern made the op-amp outputs ring in saturation, DETAILS Z-02 addendum 23). BD-03: the 100-400 Hz error of the output in 200-350 ms came from the BDF2 step error of the passive noise network (N_76 ... N_82, CJC of QQ6 into the base N_77); it falls with the square of the step (2.5 -> 1.25 us: 4.6x at N_77, N_82 and the output), uniform 0.625 us steps cost +51 % Newton iterations, 1.25 us +15 %. kBdTailKnotsT = kBdTailT: the knots never stop ending the steps (switching to 2.5 us steps at 60 ms, or 20 us steps between 60 and 100 ms, gave a seed difference at the output 24-35 dB above NGSpice, DETAILS BD-03).
  static constexpr double kBdTailT = 0.15, kBdTailFromT = 0.2, kBdTailDt = 5e-6, kBdTailEvHMax = 20e-6, kBdTailKnotsT = 0.2, kBdTailKnotDt = 1.25e-6, kBdTailKnotWidth = 5e-6, kBdTailEvDt = 2.5e-7; static constexpr int kBdTailMinCode = 63;   // kBdTailEvDt: step cap around the comparator flips of the tail (the 3 us edge of IC12B drives the response of N_26 / N_33 / N_39: with 1.25 us steps 21 % too large, with 0.25 us -0.7 %; +6 % steps)
  // Z-07: finer step after every noise knot (kfDt = rel * base step for the first base step): SD and BD need it (SD without: -28.5 / -32.1 dB against NGSpice, BD 0.8 V window 200-350 ms -30.0 dB); LT / MT / HT / HC do not (same window errors, 3-12 % faster)
  static double KnotFineRel(Voice909 v) { return (v == Voice909::Lt || v == Voice909::Ht || v == Voice909::Hc) ? 0.0 : 0.25; }   // MT keeps it: without it the window 100-200 ms at 4.064 V goes -31.9 -> -29.9 dB against NGSpice (0.35 s hit)
  // Z-05: clamp diode threshold of the op-amp stages (Net::clampSkipV); MT evaluates them always: with the skip its windows 1-20 ms at 0.8 V go from -30.6...-32.7 dB to -28.3...-29.9 dB against NGSpice
  static double ClampSkipV(Voice909 v) { return v == Voice909::Mt ? 0.0 : -1.0; }
  // Z-09: the passive non-modal blocks of the voices that are not played are not stepped during the hit (load current held): SD, LT, MT, HT always; BD always, with the noise sheet block kept live in the hit where the noise is not frozen (BD-02: with it frozen too the window 200-350 ms at accent 0.8 V reaches -29.9 dB, with it live -32.5 dB); not HC (4.064 V window error -24 -> -17 dB)
  static bool HitFreezesOthers(Voice909 v, int accentCode, uint64_t params) { return v == Voice909::Sd || v == Voice909::Lt || v == Voice909::Mt || v == Voice909::Ht || v == Voice909::Bd; }   // BD: always; the noise sheet block (owner of the NOISE line) stays live in the hit where the noise is not frozen (liveHit, Render)
  static bool BdTailPlan(Voice909 v, int accentCode, uint64_t params) { (void)params; return v == Voice909::Bd && accentCode >= kBdTailMinCode; }
  // Z-02 / BD-02: the noise sheet block is live in the BD hit (full stepping) only below decay pot kBdNoiseLiveMaxDecay or accent code below kBdTailMinCode; above it rests like the other blocks of the voices that are not played: the same window errors against NGSpice (band 0.05-0.29 dB at decay pot 0.5 and 0.9 over the pot matrix, both ways), 2.5x less compute time (2.2 s against 5.5 s per 0.35 s hit).
  static constexpr double kBdNoiseLiveMaxDecay = 0.5;
  static bool BdNoiseSheetLive(Voice909 v, int accentCode, uint64_t params) { if (v != Voice909::Bd) return false; if (accentCode < kBdTailMinCode) return true; double pp[8] = {0}; UnpackPots909(v, params, pp); return pp[3] < kBdNoiseLiveMaxDecay - 1e-9; }
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
  static int SheetOfVoice(Voice909 v) { switch (v) { case Voice909::Bd: return 0; case Voice909::Sd: return 1; case Voice909::Lt: return 2; case Voice909::Mt: return 3; case Voice909::Ht: return 4; default: return -1; } }
  static Voice909 VoiceOfSheet(int which) { switch (which) { case 0: return Voice909::Bd; case 1: return Voice909::Sd; case 2: return Voice909::Lt; case 3: return Voice909::Mt; default: return Voice909::Ht; } }
  bool CalibrateSheet(int which, uint64_t ownParams, n909::SheetPhase& sp)
  {
    n909::PartNet& net = *net_; const Voice909 sv = VoiceOfSheet(which); if (!SetPots(sv, ownParams)) return false;
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
  bool ApplyPhase(Voice909 v, uint64_t params, uint64_t seed, double timeShift = -1.0)
  {
    if (!Ready()) return false; n909::PartNet& net = *net_; std::vector<int> sheets;
    if (v == Voice909::Hc) { sheets = {0, 1}; } else { const int w = SheetOfVoice(v); if (w < 0) return true; sheets = {w}; }
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
  static bool Supports(Voice909 v) { return v == Voice909::Bd || v == Voice909::Sd || v == Voice909::Lt || v == Voice909::Mt || v == Voice909::Ht || v == Voice909::Hc; }

  bool Run(const RunRequest909& req, std::vector<float>& out) override
  {
    out.clear();
    if (!Supports(req.voice) || req.noise != NoisePath909::Reference5us) return false;
    if (!Ready()) return false;
    return Render(req.voice, req.params, req.anchorCode, req.noiseSeed, req.seconds, req.sampleRateHz, out, req.streamKnot);
  }

private:
  // Circuit pots of a voice in the order of 909PotParams.h (names of the deck parameters without vr_).
  static const char* const* PotNames(Voice909 v, int& n)
  {
    static const char* const bd[] = {"VR2_bd", "VR3_bd", "VR4_bd", "VR5_bd"}, *const sd[] = {"VR6_sd", "VR7_sd", "VR8_sd", "VR9_sd"}, *const lt[] = {"VR10_lt", "VR11_lt", "VR12_lt"}, *const mt[] = {"VR13_mt", "VR14_mt", "VR15_mt"},
                             *const ht[] = {"VR16_ht", "VR17_ht", "VR18_ht"}, *const hc[] = {"VR20_hc"};
    switch (v) { case Voice909::Bd: n = 4; return bd; case Voice909::Sd: n = 4; return sd; case Voice909::Lt: n = 3; return lt; case Voice909::Mt: n = 3; return mt; case Voice909::Ht: n = 3; return ht; default: n = 1; return hc; }
  }
  bool SetPots(Voice909 v, uint64_t params)
  {
    int n = 0; const char* const* names = PotNames(v, n); double pos[8]; UnpackPots909(v, params, pos);
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
  const n909::PartNet::Snapshot& StateFor(Voice909 v, uint64_t params, const std::vector<double>*& table)
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
  static const char* Tag(Voice909 v)   // sheet tag of the netlist (node and source names)
  {
    switch (v) { case Voice909::Bd: return "BD"; case Voice909::Sd: return "SD"; case Voice909::Lt: return "LT"; case Voice909::Mt: return "MT"; case Voice909::Ht: return "HT"; default: return "HC"; }
  }
  bool Ready()
  {
    if (ready_) return true;
    std::string text; for (int i = 0; i < kCircuitNetLineCount909; ++i) { text += kCircuitNetLines909[i]; text += '\n'; }
    net_ = std::make_unique<n909::PartNet>(); net_->fixedDt = dt_; net_->H = 20e-6;   // fixed step of the validated runs; relaxation window 20 us, NOISE tolerance 1e-7 V
    net_->splitToms = std::getenv("DGG_PART_NOSPLIT") == nullptr;   // toms block split into LT / HT / MT + noise sheet blocks (Net909Part.h, secondary interfaces TOM_NOISE, TO_NOISE_LT, TO_NOISE_HT); DGG_PART_NOSPLIT=1: one toms block
    net_->splitNoise = std::getenv("DGG_PART_NOSPLITNS") ? false : (splitNoise_ || std::getenv("DGG_PART_SPLITNS") != nullptr);   // E-03 phase 2: the noise sheet is a block of its own, MT split off along TO_NOISE_MT (tom runner; DGG_PART_SPLITNS=1 forces it, DGG_PART_NOSPLITNS=1 switches it off)
    net_->modalOn = std::getenv("DGG_PART_MODAL") ? std::atoi(std::getenv("DGG_PART_MODAL")) != 0 : true;   // E-03: passive blocks that load the NOISE line as linear port models
    net_->passTol = std::getenv("DGG_PART_PASSTOL") ? std::atof(std::getenv("DGG_PART_PASSTOL")) : 1.0;
    net_->nzInside = std::getenv("DGG_PART_NZIN") ? std::atoi(std::getenv("DGG_PART_NZIN")) : 2;   // the NOISE line is solved inside the blocks of the hit (PartNet::nzInside, set before build, plan E-03): 2 = one pass per block set (two for several blocks); test hook DGG_PART_NZIN=0 relaxation as before, 1 = interfaces iterated
    if (!net_->build(text)) { net_.reset(); return false; }
    n909::Net::State st;
    st.x.assign(kCircuitIdleState909, kCircuitIdleState909 + kCircuitStateX909);
    st.q1.assign(kCircuitIdleState909 + kCircuitStateX909, kCircuitIdleState909 + kCircuitStateX909 + kCircuitStateQ909);
    st.q2.assign(kCircuitIdleState909 + kCircuitStateX909 + kCircuitStateQ909, kCircuitIdleState909 + kCircuitStateX909 + 2 * kCircuitStateQ909);
    st.stepIndex = kCircuitStateStepIndex909; st.hPrev = kCircuitStateHPrev909;
    if (!net_->loadFullState(st)) { net_.reset(); return false; }
    net_->bypassTol = 1e-4;   // device bypass (Net909.h): validated over full 350 ms hits against NGSpice and the run without bypass (plan, section S4 faster circuit)
    if (const char* bp = std::getenv("DGG_PART_BYPASS")) net_->bypassTol = std::atof(bp);   // test hook: other tolerance, 0 = off
    nominal_ = net_->snapshot(); net_->abort = abort_;
    ready_ = true; return true;
  }
  // Run settings of the net (step plan, relaxation, idle-run and fine-step controls, block roles) back to those of a runner that has not rendered a hit: the settle and calibration runs of a hit then do not depend on the hit
  // rendered before on the same runner (the result of a hit is the same alone, after other hits and in any thread).
  void ResetRunSettings(n909::PartNet& net) const
  {
    net.bypassTol = kBypassTolBase; net.fixedDt = dt_; net.maxIt = 6; net.ifRel = 1e-6; net.bpOrigin = 0.0; net.bpStep = 0.0; net.kfDt = 0.0; net.kfWidth = 0.0;
    net.quietUntil = -1.0; net.eventHMax = 3e-6; net.eventMargin = 20e-6; net.eventHold = 12e-6; net.quietH = 60e-6; net.qaRtol = 0.0; net.qaAtol = 1e-5; net.qaHmax = 1e-4; net.statR = 0.0; net.freezeIdle = false;
    net.fineEventDt = 0.0; net.fineMargin = -1.0; net.fineHold = -1.0; net.fineNodes.clear(); net.afterWindow = nullptr; net.recordFrom = -1.0;
    for (auto& bp : net.blocks) { bp->passive = false; bp->frozenIdle = false; bp->fineEvents = false; }
  }
  bool Render(Voice909 v, uint64_t params, int accentCode, uint64_t seed, double seconds, int fs, std::vector<float>& out, uint64_t streamKnot = pcmfast::HitNoise::kNoStream)
  {
    n909::PartNet& net = *net_;
    ResetRunSettings(net);
    if (!SetPots(v, params)) return false;
    const std::vector<double>* table = nullptr;
    if (!net.restore(StateFor(v, params, table))) return false;
    const bool usePhase = UsePhase();   // E-02: phases from the oscillator automaton instead of the idle Preroll
    if (usePhase && !ApplyPhase(v, params, seed, std::getenv("DGG_PART_PHASETIME") ? std::atof(std::getenv("DGG_PART_PHASETIME")) : -1.0)) return false;
    { double bt = (v == Voice909::Hc || v == Voice909::Bd) ? kBypassTolBase : kBypassTolHit; if (const char* bp = std::getenv("DGG_PART_BYPASS")) bt = std::atof(bp);   // device bypass of the hit run per voice (E-12); the settle and calibration runs above use the base tolerance (shared by all voices)
      net.bypassTol = bt; for (auto& bp : net.blocks) bp->net.bypassTol = bt; }
    const std::string tag = Tag(v), lower = tag == "BD" ? "bd" : tag == "SD" ? "sd" : tag == "LT" ? "lt" : tag == "MT" ? "mt" : tag == "HT" ? "ht" : "hc";
    net.clearProbes(); if (!net.addProbe(lower + "_" + tag + "_OUT")) return false;
    if (const char* r = std::getenv("DGG_PART_IFREL")) net.ifRel = std::atof(r);   // test hook
    const double t0 = kTriggerDelay + (usePhase ? 0.0 : (fixedPreroll >= 0.0 ? fixedPreroll : PrerollAt(table, seed)));   // trigger time of the hit in the run: the circuit idles on from the stored state before the trigger, so the free-running oscillators are at a different phase for every noise seed (variant)
    for (const char* t : {"BD", "SD", "LT", "MT", "HT", "HC", "RM"}) net.setSourceDc(std::string("VTRIG_") + t, 0.0);
    net.setSource("VTRIG_" + tag, [t0](double t) { return pcmfast::HitTrigger(t - t0); });
    for (const char* t : {"BD", "SD", "LT", "MT", "HT", "HC", "RM"}) net.setSourceDc(std::string("VACCENT_") + t, 2.4);   // the accent of the stored idle state on the sheets of the other voices (the previous hit of the runner left its own)
    net.setSourceDc("VACCENT_" + tag, AccentVolts909(accentCode));
    pcmfast::HitNoise noise; if (streamKnot == pcmfast::HitNoise::kNoStream) noise.Start(seed, 5e-6, 0.0); else noise.StartAt(pcmfast::HitNoise::kStreamSeed, streamKnot, 5e-6, 0.0);   // shared noise stream at the trigger position of the hit (E-18), else the sequence of the seed; the noise of the hit starts at knot 10000 of the sequence (50 ms) when the trigger arrives
    if (!silentNoise) net.setSource("Vnz", [&noise, t0](double t) { return t < t0 ? 7.5 : noise.At(kNoiseAtTrigger + (t - t0)); });
    else net.setSourceDc("Vnz", 7.5);
    const double ts = 1.0 / fs; const size_t nOut = static_cast<size_t>(seconds * fs + 0.5);
    net.fixedDt = (v == Voice909::Hc && dt_ > kHcDt) ? kHcDt : (v != Voice909::Hc && dt_ < HitDt(v) ? HitDt(v) : dt_); if (v == Voice909::Hc) if (const char* hd = std::getenv("DGG_PART_HCDT")) net.fixedDt = std::atof(hd);   // test hook: other HC base step (default kHcDt)
    const OutputFilter909 ofilt(fs); const double fMargin = ofilt.Margin() + 1e-5;   // the run covers the kernel half width before the first and after the last output instant
    net.recordFrom = t0 - 5e-5 - fMargin;
    if (BdTailPlan(v, accentCode, params)) { net.bpEnd = t0 + kBdTailT; net.tailFrom = t0 + kBdTailFromT; net.tailDt = kBdTailDt; net.tailEvHMax = kBdTailEvHMax; net.tailKnotsFrom = t0 + kBdTailKnotsT; net.quietKnotCap = true; net.quietKnotCapFrom = 0.0; net.qkDt = kBdTailKnotDt; net.qkWidth = kBdTailKnotWidth; net.quietEvDt = kBdTailEvDt; }
    else { net.quietEvDt = 0.0; net.tailEvHMax = 0.0; net.bpEnd = 1e30; net.tailFrom = 1e30; net.tailDt = 0.0; net.tailKnotsFrom = 1e30; net.quietKnotCap = false; net.quietKnotCapFrom = 0.0; net.qkDt = 0.0; net.qkWidth = 0.0; }
    net.freezeHit = HitFreezesOthers(v, accentCode, params); { const double cs = ClampSkipV(v); for (auto& blk : net.blocks) blk->net.clampSkipV = cs; }
    net.bpOrigin = t0; net.bpStep = std::getenv("DGG_PART_NOBP") ? 0.0 : 5e-6; { const double kr = std::getenv("DGG_PART_KFREL") ? std::atof(std::getenv("DGG_PART_KFREL")) : KnotFineRel(v); net.kfDt = kr * net.fixedDt; net.kfWidth = kr > 0.0 ? net.fixedDt * (std::getenv("DGG_PART_KFWREL") ? std::atof(std::getenv("DGG_PART_KFWREL")) : (v == Voice909::Sd ? 3.0 : (v == Voice909::Bd ? 2.0 : 1.0))) : 0.0; }   // finer step (a quarter of the base step) for the first base step after every knot; DGG_PART_KFREL=0 off   // E-03: steps end at the knots of the noise source (HitNoise step 5 us from the trigger); DGG_PART_NOBP=1: as before
    net.kfTailFrom = v == Voice909::Sd ? t0 + kSdTailFromT : 1e30; net.kfTailDt = kSdTailKnotDt; net.kfTailWidth = net.bpStep;   // SD tail: uniform kSdTailKnotDt steps over the whole knot interval from kSdTailFromT after the trigger
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
      for (size_t i = 0; i < net.blocks.size(); ++i) { const int bi = static_cast<int>(i); const bool keep = bi == own || (tomHit && bi == kn); net.blocks[i]->passive = !off && own >= 0 && !keep; net.blocks[i]->frozenIdle = net.blocks[i]->passive && !(tag == "HC" && (bi == kb || bi == ks)); net.blocks[i]->liveHit = BdNoiseSheetLive(v, accentCode, params) && bi == kn; }   // HC: the BD and SD blocks idle on before the trigger (their slow states settle: frozen at the stored state the first 0.5 ms of the hit are off by -13 dB), as ports of the line they are modelled in the noisy windows
      // BD (addendum 14): event-controlled fine step around comparator flips in the blocks that run full (PartNet::fineEventDt, Net909Part.h); validated against NGSpice for BD up to 60 ms after the trigger. All other voices: off (the net is shared between voices, so it is set on every hit).
      net.fineEventDt = (v == Voice909::Bd || v == Voice909::Sd || v == Voice909::Lt || v == Voice909::Mt || v == Voice909::Ht) ? kFineDt : 0.0; if (const char* fe = std::getenv("DGG_PART_FINEEV")) net.fineEventDt = std::atof(fe); net.fineNodes.clear(); if (const char* fm = std::getenv("DGG_PART_FINEMARGIN")) net.fineMargin = std::atof(fm); if (const char* fh = std::getenv("DGG_PART_FINEHOLD")) net.fineHold = std::atof(fh); if (v != Voice909::Bd && !std::getenv("DGG_PART_FINEALL")) for (int k = 0; k < kOscillators; ++k) net.fineNodes.insert(OscNodes()[k]); if (v != Voice909::Bd && !std::getenv("DGG_PART_FINEALL")) for (const char* an : {"sd_N_154", "lt_N_229", "mt_N_364", "ht_N_765"}) net.fineNodes.insert(an);   // and the accent stages (their level change at the trigger shifts the oscillator phase)   // E-02 (DETAILS 10.26 / 10.27): the oscillator voices run the fine step around the comparator flips (oscillator comparators and accent stage, PartNet::fineNodes, statR rule): hit half periods against NGSpice -1e-4...-8e-4 -> <= 2e-4, phase-impulse outliers MT k = 4 -26 -> -53 dB; hit time +25...+43 %; DGG_PART_FINEEV=0 = old (only BD)
      for (auto& bp : net.blocks) bp->fineEvents = net.fineEventDt > 0.0 && !bp->passive; }
    net.fineLateFrom = t0 + kFineLateFromT; net.fineLateDt = FineLateDt(v);   // from kFineLateFromT after the trigger the fine event step is FineLateDt(v) instead of kFineDt (0: unchanged)
    net.freezeIdle = std::getenv("DGG_PART_FREEZE") ? std::atoi(std::getenv("DGG_PART_FREEZE")) != 0 : !idleOld;   // test hook (E-02): the blocks of the other voices rest during the idle run before the trigger
    const double tEndHit = t0 + static_cast<double>(nOut) * ts + 2e-4 + fMargin; const bool runOk = v == Voice909::Hc ? RunHcStaged(net, t0, tEndHit) : RunHit(net, t0, tEndHit, v == Voice909::Sd || v == Voice909::Bd); net.quietUntil = -1.0;
    if (!runOk) { out.clear(); return false; }
    if (std::getenv("DGG_PART_STATS")) { for (auto& bp : net.blocks) if (bp->qFixed + bp->qAd > 0) { fprintf(stderr, "quiet block %s: fixed steps %ld (%.1f ms), controlled steps %ld (%.1f ms)\n", bp->name.c_str(), bp->qFixed, 1e3 * bp->qFixedT, bp->qAd, 1e3 * bp->qAdT); fprintf(stderr, "   hold steps %ld, predicted-flip steps %ld; by op-amp (out node): ", bp->nHold, bp->nPred); for (size_t oi = 0; oi < bp->predBy.size(); ++oi) if (bp->predBy[oi] || bp->statBy[oi]) fprintf(stderr, "%s pred %ld stat %ld; ", bp->net.nodeName[static_cast<size_t>(bp->net.op[oi].out)].c_str(), bp->predBy[oi], bp->statBy[oi]); fprintf(stderr, "\n"); bp->nHold = bp->nPred = 0; bp->predBy.clear(); bp->statBy.clear(); bp->qFixed = bp->qAd = 0; bp->qFixedT = bp->qAdT = 0.0; } }
    if (std::getenv("DGG_PART_STATS")) { fprintf(stderr, "PartNet: windows %ld passes %ld unconverged %ld hist", net.windows, net.passes, net.unconverged); for (int i = 1; i < 8; ++i) fprintf(stderr, " %d:%ld", i, net.itHist[i]); fprintf(stderr, " max mismatch %.3g\n", net.ifMaxMismatch); }
    const auto& pr = net.probes[0];
    if (pr.t.size() < 2) { out.clear(); return false; }
    std::vector<double> yk; ofilt.Resample(pr.t, pr.v, t0, nOut, yk);   // low-pass resampling of the step ends to the host instants (OutputFilter909)
    const size_t i1 = std::min<size_t>(static_cast<size_t>(std::lower_bound(pr.t.begin(), pr.t.end(), t0) - pr.t.begin()), pr.t.size() - 1), i0 = i1 > 0 ? i1 - 1 : 0;   // idle level: the raw output just before the trigger
    const double idle = i1 == i0 ? pr.v[i0] : pr.v[i0] + (pr.v[i1] - pr.v[i0]) * std::min(std::max((t0 - pr.t[i0]) / std::max(pr.t[i1] - pr.t[i0], 1e-30), 0.0), 1.0);
    out.assign(nOut, 0.0f); for (size_t k = 0; k < nOut; ++k) out[k] = static_cast<float>(yk[k] - idle);
    return true;
  }
  static constexpr double kBypassTolBase = 1e-4, kBypassTolHit = 1e-3;   // E-12: SD / toms 1e-3 in the hit run (window errors against 1e-4 <= -42 dB, 1.2-3.3 mV; against NGSpice within 2.2 dB of 1e-4), BD and HC 1e-4 (BD 0.8 V at 1e-3: 200-350 ms window -29.5 dB against NGSpice, 1e-4 -39 dB; HC 1e-3 departs from 1e-4 by -50 dB)
  static constexpr double kTriggerDelay = 0.05;   // minimum idle before the trigger: the accent (a held level in the sequencer) and the pot states settle as in the NGSpice decks (50 ms)
  static constexpr double kNoiseAtTrigger = 0.05;   // the noise sequence of a hit is read from 50 ms on when the trigger arrives
  static constexpr double kSettle = 0.5, kSettlePhase = 0.05;
  std::map<uint64_t, Settled> settled_;
  double dt_; std::function<bool()> abort_;
  std::unique_ptr<n909::PartNet> net_; n909::PartNet::Snapshot nominal_; bool ready_ = false; bool splitNoise_ = false;
};

}  // namespace dgg
