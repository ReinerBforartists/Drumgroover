// Partitioned run of a TR-909 circuit netlist (Net909.h): the complete circuit is the same one, but the matrix is split along its
// physical structure. After removing the ideal supply rails (p15, m15, ground) and the NOISE line, the netlist falls apart into
// independent blocks (BD, SD, HC, RM, the tom sheets with TOM_NOISE). Every block is one Net with its own adaptive step size, so a
// comparator edge of one oscillator only forces small steps in its own block. The blocks stay coupled through the one node they
// share, the NOISE line: its voltage is the Thevenin source (noise generator behind Rno) minus Rno times the sum of the currents
// the blocks draw from it. The blocks see that voltage as a source; waveform relaxation over a macro window H repeats the blocks
// with the updated NOISE waveform until it no longer changes (tolerance ifTol): the loop gain is Rno x the load conductances, so
// the iteration converges in one or two passes. The coupling is therefore kept, not dropped.
#pragma once
#include "909Net.h"
#include "909NetModal.h"
#include <algorithm>
#include <chrono>
#include <set>
#include <unordered_map>

namespace n909 {

class PartNet {
 public:
  struct Wave {   // piecewise-linear current waveform of one block (current drawn from the NOISE line)
    std::vector<double> t, v; double hold = 0.0;
    double at(double tt) const {
      if (t.empty()) return hold;
      if (tt <= t.front()) return v.front(); if (tt >= t.back()) return v.back();
      const size_t i = (size_t)(std::upper_bound(t.begin(), t.end(), tt) - t.begin()); const double t0 = t[i - 1], t1 = t[i];
      return t1 > t0 ? v[i - 1] + (v[i] - v[i - 1]) * (tt - t0) / (t1 - t0) : v[i];
    }
  };
  struct Block {
    Net net; std::string name, text; int nodes = 0;
    int ifSrc = -1;   // index of the NOISE interface source in net.vs (-1: block does not touch the NOISE line)
    std::vector<int> capIdx, dioIdx, bjtIdx, opIdx, invIdx;   // index of the element in the full netlist (for the state transfer)
    double hCur = 1e-6;
    Net::State st; std::vector<double> xA, xB; double hAB = 0.0; double hCurSave = 1e-6;   // window start (for the relaxation passes)
    long steps = 0, rejected = 0;
    Wave wf, wfPrev, vl;   // vl: NOISE voltage at the steps of the last run (E-03: the line voltage the block solved)
    std::vector<double> ts; std::vector<std::vector<double>> pv;   // steps of the present pass: time, probe values
    std::vector<int> ifNode, ifIdx; std::vector<Wave> ifW;   // per secondary interface: node in this net (-1: not used), index of the VIF source (non-owner) / IFI current source (owner), waveform (owner: node voltage, other: current drawn)
    std::vector<double> vdPrev, vdPrev2; bool passive = false, frozenIdle = false, liveHit = false, fineEvents = false, modal = false; PortModel pm; PortModel::State pmWin, pmEnd; std::vector<char> fineMask; double hAd = 0.0; bool lastAd = false; long qFixed = 0, qAd = 0; long nHold = 0, nPred = 0, nStat = 0; std::vector<long> predBy, statBy; double qFixedT = 0.0, qAdT = 0.0;   // quiet-run step statistics   // passive: the voice of the block is not played: coarse event-controlled step, coarse step. frozenIdle: not stepped in the idle run before the trigger (stays at the stored state). modal (E-03): in the noisy windows the block is not stepped, its load on the NOISE line is its linear port model pm (Net909Modal.h)
     std::vector<signed char> sat; double holdUntil = -1.0, hLast = 0.0; std::vector<double> vdPrevS, vdPrev2S; std::vector<signed char> satS; double holdUntilS = -1.0, hLastS = 0.0;   // event-controlled stepping (quiet windows): comparator input differences of the last two steps, saturation, fine-step hold
  };
  // Secondary interface nodes (splitToms): nodes shared by blocks that are not the NOISE line. The owner block keeps the node and receives the loads of the other blocks as a current source; every other block
  // sees the node as a voltage source following the owner's voltage. Both waveforms are relaxed together with the NOISE line (coupling kept in both directions).
  struct Iface { std::string name, anchor; int owner = -1; Wave V, I; double vHold = 0.0, iHold = 0.0; };
  bool splitNoise = false;   // with splitToms: the noise sheet is a block of its own (MT is split off along TO_NOISE_MT; the supply nodes pVMT / mVMT are shared rails: their resistors / capacitors are copied into both blocks)
  bool splitToms = false;   // split the toms block (LT, MT + TOM_NOISE sheet, HT) along TOM_NOISE / noise_N_727; the trigger and accent sources of the toms are duplicated in the blocks that use them
  std::vector<Iface> ifaces; std::set<std::string> extraRails;
  std::vector<std::unique_ptr<Block>> blocks;
  Net master; int noiseNode = -1; std::string noiseName = "NOISE";
  std::vector<std::pair<int, double>> thev; double gLoadFix = 0.0, gTot = 0.0;   // master: (source index, conductance) behind the NOISE line; fixed leak
  std::string fullText;
  // run parameters
  double H = 20e-6, rtol = 1e-3, atol = 1e-6, hMin = 1e-9, hMax = 1e-5, ifTol = 1e-7, ifRel = 1e-6, fixedDt = 0.0; int maxIt = 6;
  // Event-controlled stepping in quiet windows (no noise on the NOISE line: the idle run before a hit): the block steps with the fixed step around comparator flips (nearest predicted crossing of a saturated op-amp
  // input difference closer than eventMargin, or eventHold after a flip) and up to eventHMax between them; the full circuit is solved, only the step size follows the events. quietUntil < 0: off (noisy runs stay fixed).
  double qaRtol = 0.0, qaAtol = 1e-5, qaHmax = 1e-4;   // E-02: > 0: in the quiet run the steps between the comparator events are error-controlled (Net::advanceAdaptive) instead of eventHMax
  double statR = 0.0;   // E-02: quiet run: a saturated op-amp with |input difference| < 50 mV counts as 'flip imminent' only if it moves faster than this (V/s); 0 = always (old rule)
  bool freezeHit = false;   /* Z-09: passive non-modal blocks of the other voices are not stepped during the hit either (load current held), see CircuitRunner909::HitFreezesOthers */
  bool freezeIdle = false;   // E-02: passive blocks are not stepped in the quiet (idle) run, see runBlock
  std::set<std::string> fineNodes;   // E-02: non-empty: only the op-amps whose output node is listed count as comparator flips of the fine-event step (the oscillator comparators; the saturating op-amps of the noise stages would otherwise keep it on)
  double fineMargin = -1.0, fineHold = -1.0;   // E-02: look-ahead margin and hold time of the fine-event step (< 0: eventMargin / eventHold)
  double fineLateFrom = 1e30, fineLateDt = 0.0;   // from this time on the fine-event step is fineLateDt (0: off)
  double fineEventDt = 0.0;   /* > 0: blocks with fineEvents run their fixed-step windows with this step around predicted / just happened comparator flips (op-amp outputs in saturation: input difference extrapolated to its zero crossing within eventMargin, or eventHold after a flip) and with fixedDt otherwise; default off */ double quietUntil = -1.0, eventHMax = 3e-6, eventMargin = 20e-6, eventHold = 12e-6, quietH = 60e-6;
  double nwAtolV = 1e-6, nwAtolI = 1e-8; bool predictor = false, reassemble = false; double bypassTol = 0.0; int jacReuse = 0; bool prestamp = false, compactRec = false, partialLU = false;   // Newton tolerances and predictor start of the blocks (Net defaults: 1e-6 / 1e-8, off)
  // statistics
  long windows = 0, passes = 0, unconverged = 0; long itHist[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  double ifMaxMismatch = 0.0, ifTolI = 1e-8, ifRelI = 1e-3, ifTolV = 5e-3, ifRelV = 3e-3;   // secondary interfaces: absolute / relative tolerance of the summed load current between passes
  // probes
  struct Probe { std::string name; int block = -1, node = 0; std::vector<double> t, v; };
  std::vector<Probe> probes;
  double T = 0.0;   // time reached

  // ---------------------------------------------------------------- split
  struct Line { std::string kind; std::vector<std::string> tok; std::vector<int> pins; };   // pins: token indices holding node numbers
  static std::vector<int> pinTokens(const std::string& k) {
    if (k == "R" || k == "C" || k == "RW" || k == "RT") return {1, 2};
    if (k == "RP") return {3, 4};
    if (k == "D") return {1, 2};
    if (k == "Q") return {1, 2, 3};
    if (k == "V" || k == "VD" || k == "VP" || k == "VS" || k == "VW" || k == "VN") return {2, 3};
    if (k == "X5218" || k == "XAN6912") return {1, 2, 3, 4, 5};
    if (k == "X4069") return {1, 2, 3, 4};
    if (k == "XBA6110") return {1, 2, 3, 4, 5, 6};
    return {};
  }
  // text: full netlist; globals: node names that are shared (not part of any block); the NOISE Thevenin source nodes are global too.
  bool build(const std::string& text, std::string* err = nullptr) {
    fullText = text; blocks.clear();
    std::istringstream is(text); std::string ln; std::vector<std::string> nodeNames; std::vector<Line> lines; std::string par4069;
    while (std::getline(is, ln)) {
      if (ln.empty()) continue; std::istringstream ls(ln); Line L; std::string t; ls >> L.kind; while (ls >> t) L.tok.push_back(t);
      L.tok.insert(L.tok.begin(), L.kind);   // tok[0] = kind
      if (L.kind == "NODE") { const int i = std::atoi(L.tok[1].c_str()); if (i >= (int)nodeNames.size()) nodeNames.resize(i + 1); nodeNames[i] = L.tok[2]; continue; }
      if (L.kind == "PAR4069") { par4069 = ln; continue; }
      L.pins = pinTokens(L.kind); lines.push_back(L);
    }
    const int N = (int)nodeNames.size();
    std::set<std::string> glob = {"0", "p15", "m15", noiseName, "nz_int"};
    ifaces.clear(); extraRails.clear();
    if (splitToms) {
      // TOM_NOISE (shaped noise, owner: the MT + noise sheet block) feeds the toms; TO_NOISE_LT / HT (tom output into the diode OR of the noise stage, owner: the tom's block) feed the noise sheet. MT's TO_NOISE_MT is inside its block.
      std::vector<std::pair<const char*, const char*>> ifn = {{"TOM_NOISE", "noise_N_712"}, {"TO_NOISE_LT", "lt_LT_OUT"}, {"TO_NOISE_HT", "ht_HT_OUT"}};
      if (splitNoise) ifn.push_back({"TO_NOISE_MT", "mt_MT_OUT"});
      for (const auto& e : ifn) { Iface f; f.name = e.first; f.anchor = e.second; ifaces.push_back(f); glob.insert(e.first); }
      for (const char* n : {"TRIG_LT", "TRIG_MT", "TRIG_HT", "ACCENT_LT", "ACCENT_MT", "ACCENT_HT"}) { extraRails.insert(n); glob.insert(n); }
      if (splitNoise) for (const char* n : {"pVMT", "mVMT"}) { extraRails.insert(n); glob.insert(n); }
    }
    auto isGlob = [&](int n) { return n == 0 || glob.count(nodeNames[n]) > 0; };
    std::vector<int> par(N); for (int i = 0; i < N; ++i) par[i] = i;
    std::function<int(int)> find = [&](int a) { while (par[a] != a) { par[a] = par[par[a]]; a = par[a]; } return a; };
    for (const Line& L : lines) { int first = -1; for (int p : L.pins) { const int n = std::atoi(L.tok[p].c_str()); if (isGlob(n)) continue; if (first < 0) first = n; else par[find(n)] = find(first); } }
    // components, sizes
    std::unordered_map<int, int> csize; for (int i = 0; i < N; ++i) if (!isGlob(i)) ++csize[find(i)];
    // tiny components (single nodes hanging on the rails): merged into the largest component of the same sheet
    auto sheetOf = [&](int n) { const std::string& s = nodeNames[n]; const size_t u = s.find('_'); return u == std::string::npos ? s : s.substr(0, u); };
    std::vector<std::pair<int, int>> order; for (auto& kv : csize) order.push_back({kv.second, kv.first}); std::sort(order.rbegin(), order.rend());
    std::vector<int> big; for (auto& o : order) if (o.first >= 8) big.push_back(o.second);
    if (big.empty()) { if (err) *err = "no block"; return false; }
    std::unordered_map<int, int> blockOfRoot; for (size_t b = 0; b < big.size(); ++b) blockOfRoot[big[b]] = (int)b;
    std::vector<std::vector<std::string>> bigSheets(big.size());
    for (int i = 0; i < N; ++i) if (!isGlob(i)) { auto it = blockOfRoot.find(find(i)); if (it != blockOfRoot.end()) bigSheets[it->second].push_back(sheetOf(i)); }
    for (auto& o : order) if (o.first < 8) {   // sheet of the small component -> the big block with the most nodes of that sheet
      std::string sh; for (int i = 0; i < N; ++i) if (!isGlob(i) && find(i) == o.second) { sh = sheetOf(i); break; }
      int best = 0, bc = -1; for (size_t b = 0; b < big.size(); ++b) { const int c = (int)std::count(bigSheets[b].begin(), bigSheets[b].end(), sh); if (c > bc) { bc = c; best = (int)b; } }
      blockOfRoot[o.second] = best; }
    auto blockOfNode = [&](int n) { return blockOfRoot[find(n)]; };
    // block texts
    const int nb = (int)big.size(); std::vector<std::vector<int>> bnodes(nb); std::vector<std::vector<const Line*>> blines(nb); std::vector<const Line*> mlines;
    std::vector<std::set<int>> bset(nb);
    std::vector<const Line*> extraLines;
    int cap = 0, dio = 0, bjt = 0, opn = 0, inv = 0;   // element counters of the FULL netlist (same order as Net::loadStream)
    std::vector<std::vector<int>> capI(nb), dioI(nb), bjtI(nb), opI(nb), invI(nb);
    for (const Line& L : lines) {
      int b = -1; for (int p : L.pins) { const int n = std::atoi(L.tok[p].c_str()); if (!isGlob(n)) { b = blockOfNode(n); break; } }
      const std::string& k = L.kind;
      if (b < 0) {   // all pins global: the rail sources go to every block that uses a rail; the rest is the NOISE master
        bool rail = (k == "V" || k == "VD") && glob.count(L.tok[1]) == 0 && (nodeNames[std::atoi(L.tok[2].c_str())] == "p15" || nodeNames[std::atoi(L.tok[2].c_str())] == "m15");
        bool xrail = (k == "V" || k == "VD") && extraRails.count(nodeNames[std::atoi(L.tok[2].c_str())]) > 0;
        if (!xrail && splitNoise && (k == "R" || k == "C")) for (int p : L.pins) if (extraRails.count(nodeNames[std::atoi(L.tok[p].c_str())]) > 0) xrail = true;   // supply resistor / capacitors of the shared rails pVMT / mVMT
        if (xrail) extraLines.push_back(&L);
        else if (rail) for (int q = 0; q < nb; ++q) blines[q].push_back(&L); else mlines.push_back(&L);
        continue;
      }
      blines[b].push_back(&L);
      if (k == "C") { capI[b].push_back(cap); ++cap; } else if (k == "D") { dioI[b].push_back(dio); ++dio; }
      else if (k == "Q") { bjtI[b].push_back(bjt); ++bjt; } else if (k == "XBA6110") { bjtI[b].push_back(bjt); ++bjt; }
      else if (k == "X5218") { opI[b].push_back(opn); ++opn; } else if (k == "X4069") { invI[b].push_back(inv); ++inv; }
      continue;
    }
    for (const Line* XL : extraLines) for (int q = 0; q < nb; ++q) { std::vector<int> xns; for (int pn : XL->pins) { const int n0 = std::atoi(XL->tok[pn].c_str()); if (extraRails.count(nodeNames[n0]) > 0) xns.push_back(n0); } bool use = false;   // toms trigger / accent sources, shared supply elements: a copy in every block whose elements touch the node
      for (const Line* L : blines[q]) { if (L == XL) continue; for (int pn : L->pins) for (int xn : xns) if (std::atoi(L->tok[pn].c_str()) == xn) use = true; } if (use) blines[q].push_back(XL); }
    // counters for elements of other blocks (must also advance): recount in a second pass in line order
    std::unordered_map<const Line*, int> capOf; cap = dio = bjt = opn = inv = 0; for (auto& v : capI) v.clear(); for (auto& v : dioI) v.clear(); for (auto& v : bjtI) v.clear(); for (auto& v : opI) v.clear(); for (auto& v : invI) v.clear();
    for (const Line& L : lines) {
      int b = -1; for (int p : L.pins) { const int n = std::atoi(L.tok[p].c_str()); if (!isGlob(n)) { b = blockOfNode(n); break; } }
      const std::string& k = L.kind;
      auto add = [&](std::vector<std::vector<int>>& I, int& c) { if (b >= 0) I[b].push_back(c); ++c; };
      if (k == "C") { capOf[&L] = cap; add(capI, cap); } else if (k == "D") add(dioI, dio); else if (k == "Q" || k == "XBA6110") add(bjtI, bjt); else if (k == "X5218") add(opI, opn); else if (k == "X4069") add(invI, inv);
    }
    for (const Line* XL : extraLines) if (XL->kind == "C") for (int q = 0; q < nb; ++q) if (std::find(blines[q].begin(), blines[q].end(), XL) != blines[q].end()) capI[q].push_back(capOf[XL]);   // copies of shared supply capacitors: same global capacitor number in every block that holds a copy
    // emit and load every block
    std::string masterText;
    {   // master: nodes 0 .. N-1 kept (names), only its lines
      std::ostringstream os; for (int i = 0; i < N; ++i) os << "NODE " << i << " " << nodeNames[i] << "\n"; for (const Line* L : mlines) { for (size_t i = 0; i < L->tok.size(); ++i) os << (i ? " " : "") << L->tok[i]; os << "\n"; } masterText = os.str(); }
    std::vector<int> ifId(ifaces.size(), -1);
    for (size_t g = 0; g < ifaces.size(); ++g) { for (int i = 0; i < N; ++i) { if (nodeNames[i] == ifaces[g].name) ifId[g] = i; if (nodeNames[i] == ifaces[g].anchor) ifaces[g].owner = blockOfNode(i); } if (ifId[g] < 0 || ifaces[g].owner < 0) { if (err) *err = "interface node missing"; return false; } }
    for (int b = 0; b < nb; ++b) {
      std::set<int> used; used.insert(0); for (const Line* L : blines[b]) for (int p : L->pins) used.insert(std::atoi(L->tok[p].c_str()));
      std::vector<int> nodes(used.begin(), used.end()); std::unordered_map<int, int> nid; for (size_t i = 0; i < nodes.size(); ++i) nid[nodes[i]] = (int)i;
      std::ostringstream os; if (!par4069.empty()) os << par4069 << "\n"; for (size_t i = 0; i < nodes.size(); ++i) os << "NODE " << i << " " << nodeNames[nodes[i]] << "\n";
      for (const Line* L : blines[b]) { for (size_t i = 0; i < L->tok.size(); ++i) { std::string s = L->tok[i]; if (std::find(L->pins.begin(), L->pins.end(), (int)i) != L->pins.end()) s = std::to_string(nid[std::atoi(s.c_str())]); os << (i ? " " : "") << s; } os << "\n"; }
      int nz = -1; for (size_t i = 0; i < nodes.size(); ++i) if (nodeNames[nodes[i]] == noiseName) nz = (int)i;
      if (nz > 0) os << "VD VNIF " << nz << " 0 7.5\n";
      std::vector<int> ifLocal(ifaces.size(), -1); for (size_t g = 0; g < ifaces.size(); ++g) if (used.count(ifId[g])) { ifLocal[g] = nid[ifId[g]]; if (ifaces[g].owner == b) os << "I IFI" << g << " " << ifLocal[g] << " 0 0\n"; else os << "VD VIF" << g << " " << ifLocal[g] << " 0 7.5\n"; }
      auto blk = std::make_unique<Block>(); blk->text = os.str(); if (!blk->net.loadText(os.str())) { if (err) *err = "block load failed"; return false; }
      blk->nodes = (int)nodes.size(); blk->capIdx = capI[b]; blk->dioIdx = dioI[b]; blk->bjtIdx = bjtI[b]; blk->opIdx = opI[b]; blk->invIdx = invI[b];
      for (size_t i = 0; i < blk->net.vs.size(); ++i) if (blk->net.vs[i].name == "VNIF") { blk->ifSrc = (int)i; blk->net.vs[i].thv = nzInside > 0; }
      blk->ifNode = ifLocal; blk->ifIdx.assign(ifaces.size(), -1); blk->ifW.assign(ifaces.size(), Wave());
      for (size_t g = 0; g < ifaces.size(); ++g) if (ifLocal[g] >= 0) { if (ifaces[g].owner == b) { for (size_t i = 0; i < blk->net.isrc.size(); ++i) if (blk->net.isrc[i].name == "IFI" + std::to_string(g)) blk->ifIdx[g] = (int)i; } else for (size_t i = 0; i < blk->net.vs.size(); ++i) if (blk->net.vs[i].name == "VIF" + std::to_string(g)) blk->ifIdx[g] = (int)i; }
      blk->name = sheetOf(nodes.size() > 1 ? nodes[1] : 0) + "+" + std::to_string(blk->nodes);
      blocks.push_back(std::move(blk));
    }
    if (!master.loadText(masterText)) { if (err) *err = "master load failed"; return false; }
    noiseNode = master.nodeIndex(noiseName);
    if (noiseNode < 0) { if (err) *err = "no NOISE node"; return false; }
    // Thevenin / Norton model of the master around the NOISE node: resistors to source-driven nodes and to ground
    thev.clear(); gLoadFix = 0.0;
    for (const Res& r : master.res) {
      int o = -1; if (r.a == noiseNode) o = r.b; else if (r.b == noiseNode) o = r.a; else continue;
      if (o == 0) { gLoadFix += r.g; continue; }
      int si = -1; for (size_t i = 0; i < master.vs.size(); ++i) if (master.vs[i].p == o && master.vs[i].n == 0) si = (int)i;
      if (si < 0) { if (err) *err = "unsupported master topology around NOISE"; return false; }
      thev.push_back({si, r.g});
    }
    gTot = gLoadFix + kGmin; for (auto& p : thev) gTot += p.second;
    return true;
  }
  // Thevenin voltage of the master (sources behind resistors) at time t: V_N = (sum g_i Vs_i(t) - I_loads) / gTot
  double theveninNum(double t) const {
    double s = 0.0; for (const auto& p : thev) { const Vsrc& v = master.vs[p.first]; s += p.second * (v.f ? v.f(t) : v.dc); } return s;
  }
  bool setSource(const std::string& nm, std::function<double(double)> fn) {
    for (auto& v : master.vs) if (v.name == nm) { v.f = std::move(fn); return true; }
    bool any = false; for (auto& b : blocks) any = b->net.setSource(nm, fn) || any;   // a source can be duplicated in several blocks (toms trigger / accent)
    return any;
  }
  bool setPot(const std::string& nm, double pos) { bool f = false; for (auto& b : blocks) f = b->net.setPot(nm, pos) || f; return f; }
  void resetPots() { for (auto& b : blocks) b->net.resetPots(); }
  bool setSourceDc(const std::string& nm, double volts) {
    for (auto& v : master.vs) if (v.name == nm) { v.f = nullptr; v.dc = volts; return true; }
    bool any = false; for (auto& b : blocks) any = b->net.setSourceDc(nm, volts) || any;
    return any;
  }
  int blockOfNodeName(const std::string& nm, int* node = nullptr) { for (size_t i = 0; i < blocks.size(); ++i) { const int k = blocks[i]->net.nodeIndex(nm); if (k >= 0) { if (node) *node = k; return (int)i; } } return -1; }
  bool addProbeIn(const std::string& nm, const std::string& anchor) { Probe p; p.name = nm; p.block = blockOfNodeName(anchor); if (p.block < 0) return false; p.node = blocks[static_cast<size_t>(p.block)]->net.nodeIndex(nm); if (p.node < 0) return false; probes.push_back(p); return true; }   // node of the block that contains `anchor` (a node name shared by blocks, e.g. NOISE, as the block of the played voice sees it)
  bool addProbe(const std::string& nm) { Probe p; p.name = nm; p.block = blockOfNodeName(nm, &p.node); if (p.block < 0) return false; probes.push_back(p); return true; }

  // ---------------------------------------------------------------- state transfer (full net state -> blocks)
  bool loadFullState(const Net::State& fs) {
    Net full; if (!full.loadText(fullText)) return false; if (!full.restoreState(fs)) return false;
    // names that are unique in the full net (internal nodes of the elements are mapped through the elements)
    std::set<int> internal; for (const Bjt& q : full.bjt) { if (q.bi != q.b) internal.insert(q.bi); if (q.ci != q.c) internal.insert(q.ci); } for (const Op5218& m : full.op) internal.insert(m.n1);
    std::unordered_map<std::string, int> byName; for (int i = 1; i < full.nNodes; ++i) if (!internal.count(i)) byName[full.nodeName[i]] = i;
    const int nnF = full.nNodes - 1;
    for (auto& bp : blocks) {
      Block& b = *bp; Net& s = b.net; const int nnS = s.nNodes - 1; Net::State st; st.x.assign(s.nUnk, 0.0); st.q1.assign(s.nq, 0.0); st.q2.assign(s.nq, 0.0); st.stepIndex = fs.stepIndex; st.hPrev = fs.hPrev;
      std::set<int> internalS; for (const Bjt& q : s.bjt) { if (q.bi != q.b) internalS.insert(q.bi); if (q.ci != q.c) internalS.insert(q.ci); } for (const Op5218& m : s.op) internalS.insert(m.n1);
      for (int i = 1; i < s.nNodes; ++i) if (!internalS.count(i)) { auto it = byName.find(s.nodeName[i]); if (it != byName.end()) st.x[i - 1] = full.x[it->second - 1]; }
      for (size_t j = 0; j < s.bjt.size() && j < b.bjtIdx.size(); ++j) { const Bjt& a = s.bjt[j]; const Bjt& f = full.bjt[b.bjtIdx[j]];
        if (a.bi != a.b && a.bi > 0 && f.bi > 0) st.x[a.bi - 1] = full.x[f.bi - 1]; if (a.ci != a.c && a.ci > 0 && f.ci > 0) st.x[a.ci - 1] = full.x[f.ci - 1];
        st.q1[a.qbe] = full.q1[f.qbe]; st.q2[a.qbe] = full.q2[f.qbe]; st.q1[a.qbc] = full.q1[f.qbc]; st.q2[a.qbc] = full.q2[f.qbc]; }
      for (size_t j = 0; j < s.op.size(); ++j) { const Op5218& a = s.op[j]; const Op5218& f = full.op[b.opIdx[j]]; st.x[a.n1 - 1] = full.x[f.n1 - 1]; st.q1[a.qc] = full.q1[f.qc]; st.q2[a.qc] = full.q2[f.qc]; }
      for (size_t j = 0; j < s.inv.size(); ++j) { const Inv4069& a = s.inv[j]; const Inv4069& f = full.inv[b.invIdx[j]]; st.q1[a.qin] = full.q1[f.qin]; st.q2[a.qin] = full.q2[f.qin]; st.q1[a.qout] = full.q1[f.qout]; st.q2[a.qout] = full.q2[f.qout]; }
      for (size_t j = 0; j < s.cap.size(); ++j) { const Cap& a = s.cap[j]; const Cap& f = full.cap[b.capIdx[j]]; st.q1[a.q] = full.q1[f.q]; st.q2[a.q] = full.q2[f.q]; }
      for (size_t j = 0; j < s.dio.size(); ++j) { const Diode& a = s.dio[j]; const Diode& f = full.dio[b.dioIdx[j]]; st.q1[a.q] = full.q1[f.q]; st.q2[a.q] = full.q2[f.q]; }
      for (const Vsrc& v : s.vs) { if (v.name == "VNIF") continue; for (const Vsrc& f : full.vs) if (f.name == v.name) st.x[nnS + v.br] = full.x[nnF + f.br]; }
      if (!s.restoreState(st)) return false;
      b.xA.clear(); b.xB.clear(); b.hAB = 0.0; b.hCur = std::min(fixedDt > 0 ? fixedDt : 1e-6, hMax);
      s.nwAtolV = nwAtolV; s.nwAtolI = nwAtolI; s.predictorStart = predictor; s.reassembleCharges = reassemble; s.bypassTol = bypassTol; s.jacReuseMax = jacReuse; s.prestamp = prestamp; s.compactRec = compactRec; s.partialLU = partialLU;
    }
    // starting currents of the blocks on the NOISE line
    for (auto& bp : blocks) { bp->wf.t.clear(); bp->wf.v.clear(); bp->wf.hold = 0.0; }
    T = 0.0; return true;
  }
  // Circuit state of all blocks (continuing a run: free-running oscillators and slow RC states belong to the circuit); time restarts at 0.
  struct Snapshot { std::vector<Net::State> st; std::vector<double> hold; };
  Snapshot snapshot() const { Snapshot sn; for (auto& b : blocks) { sn.st.push_back(b->net.saveState()); sn.hold.push_back(b->wf.hold); } return sn; }
  bool restore(const Snapshot& sn) {
    if (sn.st.size() != blocks.size()) return false;
    for (size_t i = 0; i < blocks.size(); ++i) {
      Block& b = *blocks[i]; if (!b.net.restoreState(sn.st[i])) return false;
      b.xA.clear(); b.xB.clear(); b.net.xA.clear(); b.net.xB.clear(); b.net.hAB = 0.0; b.hAB = 0.0; b.hCur = std::min(fixedDt > 0 ? fixedDt : 1e-6, hMax);
      b.net.nwAtolV = nwAtolV; b.net.nwAtolI = nwAtolI; b.net.predictorStart = predictor; b.net.reassembleCharges = reassemble; b.net.bypassTol = bypassTol; b.net.jacReuseMax = jacReuse; b.net.prestamp = prestamp; b.net.compactRec = compactRec; b.net.partialLU = partialLU;
      b.wf.t.clear(); b.wf.v.clear(); b.wf.hold = sn.hold[i]; b.hLast = 0.0;
    }
    T = 0.0; haveVEnd = false; gCarry = 0.0; modalBuilt = false; modalRun = false; for (auto& b : blocks) b->modal = false; return true;
  }
  void clearProbes() { probes.clear(); }
  std::function<void(double)> afterWindow;   // E-02: called with the time reached after every window of run() (recording of the idle trajectory)
  // E-02: replace all unknowns of one block (the oscillator phase moved); the charges follow from the new solution (as after a DC solve: first step is backward Euler from this state)
  bool setBlockX(int bi, const std::vector<double>& x) {
    if (bi < 0 || bi >= static_cast<int>(blocks.size())) return false; Net& s = blocks[static_cast<size_t>(bi)]->net; if (static_cast<int>(x.size()) != s.nUnk) return false;
    Net::State st = s.saveState(); st.x = x; if (!s.restoreState(st)) return false;
    s.dcMode = false; s.stepIndex = 0; s.assemble(0.0); s.q1 = s.qNow; s.q2 = s.qNow; s.hPrev = 0.0; s.xA.clear(); s.xB.clear(); s.hAB = 0.0;
    Block& b = *blocks[static_cast<size_t>(bi)]; b.xA.clear(); b.xB.clear(); b.hAB = 0.0; b.hLast = 0.0; return true;
  }
  std::function<bool()> abort;   // polled between windows; true stops the run (run() returns false)
  double recordFrom = -1.0;      // probe samples are kept from this time on (the idle phase before a hit is not recorded)
  // current the block draws from the NOISE line at its present state (the branch current of the interface source is the current into the source node: minus the load)
  static double loadCurrent(const Block& b) { if (b.ifSrc < 0) return 0.0; return -b.net.x[b.net.nNodes - 1 + b.net.vs[b.ifSrc].br]; }

  // ---------------------------------------------------------------- run
  // The Thevenin source (the noise of a hit) is evaluated at every Newton iteration of every block and pass: per window its values are tabulated on the step grid (fixed step) and read back by index;
  // other times (halved steps, adaptive runs) are evaluated directly with a one-value cache.
  mutable std::vector<double> thGrid; mutable double thT0 = 0.0, thDt = 0.0, thLastT = -1e300, thLastV = 0.0;
  void prepareThevenin(double t0, double t1) {
    thGrid.clear(); thDt = 0.0; if (fixedDt <= 0.0) return;
    const long n = static_cast<long>(std::ceil((t1 - t0) / fixedDt - 1e-9)) + 1; thT0 = t0; thDt = fixedDt; thGrid.resize(static_cast<size_t>(n));
    for (long i = 0; i < n; ++i) thGrid[static_cast<size_t>(i)] = theveninNum(t0 + static_cast<double>(i) * fixedDt);
  }
  double theveninAt(double t) const {
    if (thDt > 0.0) { const double u = (t - thT0) / thDt; const long i = std::lround(u); if (std::fabs(u - static_cast<double>(i)) < 1e-6 && i >= 0 && i < static_cast<long>(thGrid.size())) return thGrid[static_cast<size_t>(i)]; }
    if (t == thLastT) return thLastV; thLastT = t; thLastV = theveninNum(t); return thLastV;
  }
  double noiseVoltage(double t, double iSum) const { return (theveninAt(t) - iSum) / gTot; }
  // NOISE voltage waveform of the present pass: analytic (Thevenin source minus the loads held at their value at the window start) in the first pass, then the table of the previous pass's update.
  // NOISE line inside the blocks of the hit (nzInside: 0 off, 1 interfaces iterated, 2 one pass per block set, two for several blocks; set before build): the blocks that touch the NOISE line and are not passive (the played voice) get their interface source as a Thevenin source with the series resistance 1 / gTot of the master and the
  // loads of the other blocks (waveforms of their runs, Norton equivalents of the port models) as a known current: the line voltage then follows from the block's own Newton solve, no relaxation pass over the line is needed. All other windows run as before.
  int nzInside = 0; Block* inB = nullptr; Wave iOth; long insideWindows = 0;
  // E-03: the passive blocks that only load the NOISE line (BD, SD, HC while another voice is played) are not stepped in the noisy windows: their load is the linear port model of the block at its stored state
  // (Net909Modal.h), exact in the discretisation of the circuit, put into the Thevenin source of every block that solves the line as a Norton equivalent per step. The block that owns a secondary interface (the noise
  // sheet with the tom shaping filter, nonlinear op-amp stage) is not modelled: it runs as a block that solves the line, with the coarse event-controlled step of a passive block, in a second pass.
  // E-03: breakpoints of the noise source. It is piecewise linear with a knot every bpStep from bpOrigin (the trigger time) on; a step that spans a knot integrates the kink of the source with the error of the step
  // (the steps of a block drift off the lattice of the trigger after every fine event step, 1 us steps then straddle the knots), so every step ends at the next knot (bpStep <= 0: off).
  double bpOrigin = 0.0, bpStep = 0.0, kfDt = 0.0, kfWidth = 0.0, kfTailFrom = 1e30, kfTailDt = 0.0, kfTailWidth = -1.0;   // from kfTailFrom on the step after a knot is kfTailDt (> 0) for kfTailWidth (>= 0) instead of kfDt for kfWidth (SD tail)   // kfDt / kfWidth: finer step after every knot (0: off)
  double tailEvHMax = 0.0;   /* Z-06: > 0: after tailFrom the played blocks run event-controlled (fixedDt = tailDt near comparator flips) with this longest step between them */
  double bpEnd = 1e30, tailFrom = 1e30, tailDt = 0.0, tailKnotsFrom = 1e30;   /* Z-02: the steps no longer end at the noise knots from bpEnd on, base step tailDt from tailFrom, the steps end at the knots again from tailKnotsFrom on (BD tail plan, CircuitRunner909::BdTailPlan) */
  double qkDt = 0.0, qkWidth = 0.0, quietEvDt = 0.0;   // quietEvDt > 0: from tailFrom on, the event-controlled branch caps the step near comparator flips (predicted crossing within eventMargin, eventHold after a flip); the step grows by at most 2x per step; Z-02: kfDt / kfWidth of the event-controlled (quiet) branch (0 = those of knotStepCap)
  double quietCap(double t) const { if (qkDt <= 0.0 || qkWidth <= 0.0) return knotStepCap(t); if (t >= bpEnd - 1e-12 || bpStep <= 0.0 || t < bpOrigin - 1e-12) return 1e30; const double since = t - (bpOrigin + std::floor((t - bpOrigin) / bpStep + 1e-6) * bpStep); return since < qkWidth - 1e-12 ? qkDt : 1e30; }
  bool quietKnotCap = false; double quietKnotCapFrom = 0.0;   // Z-02: step cap after a knot (quietCap) also in the event-controlled (quiet) branch, from this absolute time on
  double knotStepCap(double t) const { if (t >= bpEnd - 1e-12 || kfDt <= 0.0 || bpStep <= 0.0 || t < bpOrigin - 1e-12) return 1e30; const double since = t - (bpOrigin + std::floor((t - bpOrigin) / bpStep + 1e-6) * bpStep); const bool tl = t >= kfTailFrom; const double w = tl && kfTailWidth >= 0.0 ? kfTailWidth : kfWidth, d = tl && kfTailDt > 0.0 ? kfTailDt : kfDt; return since < w - 1e-12 ? d : 1e30; }   // step after a knot of the source: kfDt for kfWidth (the kink of the source excites the fast transients of the linear stages: a quarter of the base step for the first base step)
  double nextBreak(double t) const { if (bpStep <= 0.0 || t >= bpEnd - 1e-12) return 1e30; if (t < bpOrigin - 1e-12) return bpOrigin; return bpOrigin + (std::floor((t - bpOrigin) / bpStep + 1e-6) + 1.0) * bpStep; }
  bool modalOn = true;   // false: the passive blocks that load the line run as blocks that solve the line (reference of the A/B comparison)
  bool modalBuilt = false, modalRun = false; double gM = 0.0, ihM = 0.0; long modelBuilds = 0; double passTol = 1.0;   // passTol: factor on the tolerance (ifTolV + ifRelV x swing) of the line voltage that a second pass may still change (0: always two passes)
  bool ownsIface(const Block* b) const { for (const Iface& f : ifaces) if (f.owner >= 0 && blocks[static_cast<size_t>(f.owner)].get() == b) return true; return false; }
  bool modalCandidate(const Block* b, bool quietW) const { return modalOn && nzInside == 2 && !quietW && b->passive && b->ifSrc >= 0 && !ownsIface(b); }
  bool quietWindow(double t1) const { return quietUntil > 0.0 && t1 <= quietUntil + 1e-12; }
  std::vector<Block*> insideBlocks(bool quietW) { std::vector<Block*> r; for (auto& b : blocks) if (b->ifSrc >= 0 && (!quietW || !b->passive) && !modalCandidate(b.get(), quietW)) r.push_back(b.get()); if (r.size() > 1 && nzInside != 2) r.clear(); return r; }   // blocks that solve the line: the played one(s); in the noisy windows also the passive block that cannot be modelled
  void buildModels(double tNow) {   // linearise the modal candidates at their present (stored) state; a block whose model cannot be built stays a block that solves the line
    modalBuilt = true; int bidx = -1;
    for (auto& bp : blocks) { Block& b = *bp; ++bidx; b.modal = false; if (!modalCandidate(&b, false)) continue;
      if (const char* sel = std::getenv("DGG_PART_MODALIDX")) { bool ok = false; for (const char* c = sel; *c; ++c) if (*c - '0' == bidx) ok = true; if (!ok) continue; }   // test hook: only the blocks with these indices are modelled
      b.net.setSrcRs(static_cast<size_t>(b.ifSrc), 0.0); if (!b.pm.build(b.net, b.ifSrc, tNow)) continue; b.modal = true; ++modelBuilds; b.wf.hold = 0.0; b.wf.t.clear(); b.wf.v.clear();
      if (std::getenv("DGG_PART_MDBG")) { fprintf(stderr, "E-03 model of block %s: n=%d, dynamic nodes %d, stable modes %d, kept %zu, d=%.4g, V0=%.4f, I0=%.4g\n", b.name.c_str(), b.pm.n, b.pm.nDyn, b.pm.nStable, b.pm.mu.size(), b.pm.d, b.pm.V0, b.pm.I0);
        for (size_t q = 0; q < b.pm.mu.size(); ++q) fprintf(stderr, "     mode %zu: mu=%.4e%+.4ej  s=%.5g%+.5gj 1/s  rho=%.4e%+.4ej S\n", q, b.pm.mu[q].real(), b.pm.mu[q].imag(), -(1.0 / b.pm.mu[q]).real(), -(1.0 / b.pm.mu[q]).imag(), b.pm.rho[q].real(), b.pm.rho[q].imag()); } }
  }
  Wave vUse; bool vAnalytic = true; double iHoldSum = 0.0;
  double gCarry = 0.0, vEnd = 0.0; bool haveVEnd = false;   // load conductance seen at the NOISE line and the NOISE voltage at the end of the previous window: they start the first pass of the next window (the loads follow the noise)
  double vAt(double t) const { return vAnalytic ? (haveVEnd ? (theveninAt(t) - iHoldSum + gCarry * vEnd) / (gTot + gCarry) : (theveninAt(t) - iHoldSum) / gTot) : vUse.at(t); }
  // advance every block over [t0, t1] with the NOISE voltage of the present pass
  void runBlock(Block& b, double t0, double t1) {
    Net& s = b.net;
    s.stepPre = nullptr; s.stepPost = nullptr;
    if (b.ifSrc >= 0) {
      if (inB == &b && modalRun) {   // the line is solved inside this block; the port models of the passive blocks are Norton equivalents per step: V = (Th - loads of the other blocks - Ih) / (gTot + g), series resistance 1 / (gTot + g)
        s.vs[b.ifSrc].f = [this](double t) { return (theveninAt(t) - iOth.at(t) - ihM) / (gTot + gM); };
        s.stepPre = [this, &b, &s](double, double h) { double g = 0.0, ih = 0.0; for (auto& q : blocks) if (q->modal) { double gj, ihj; q->pm.norton(h, gj, ihj); g += gj; ih += ihj; } gM = g; ihM = ih; s.setSrcRs(static_cast<size_t>(b.ifSrc), 1.0 / (gTot + g)); };
        s.stepPost = [this, &b, &s](double, double h) { const double v = s.V(s.vs[static_cast<size_t>(b.ifSrc)].p); for (auto& q : blocks) if (q->modal) q->pm.advance(h, v); };
      }
      else if (inB == &b) s.vs[b.ifSrc].f = [this](double t) { return (theveninAt(t) - iOth.at(t)) / gTot; };   // inB: the NOISE line is solved inside this block (Thevenin source with series resistance 1 / gTot, the loads of the other blocks as a known current)
      else s.vs[b.ifSrc].f = [this](double t) { return vAt(t); };
    }
    b.ts.clear(); b.wf.t.clear(); b.wf.v.clear(); b.vl.t.clear(); b.vl.v.clear(); b.pv.assign(probes.size(), {});
    b.wf.t.push_back(t0); b.wf.v.push_back(b.wf.hold);   // I at the window start (the state's)
    // secondary interfaces: the owner draws the summed load current of the other blocks (source IFI) and records its node voltage; the others follow the owner's voltage (source VIF) and record the current they draw
    auto recIf = [&](double tt) { for (size_t g = 0; g < ifaces.size(); ++g) if (b.ifNode[g] >= 0) { Wave& w = b.ifW[g]; w.t.push_back(tt); w.v.push_back(ifaces[g].owner >= 0 && blocks[static_cast<size_t>(ifaces[g].owner)].get() == &b ? s.V(b.ifNode[g]) : -s.x[static_cast<size_t>(s.nNodes - 1 + s.vs[static_cast<size_t>(b.ifIdx[g])].br)]); } };
    for (size_t g = 0; g < ifaces.size(); ++g) if (b.ifNode[g] >= 0) { b.ifW[g].t.clear(); b.ifW[g].v.clear();
      if (blocks[static_cast<size_t>(ifaces[g].owner)].get() == &b) s.isrc[static_cast<size_t>(b.ifIdx[g])].f = [this, g](double t) { return ifaces[g].I.at(t); };
      else s.vs[static_cast<size_t>(b.ifIdx[g])].f = [this, g](double t) { return ifaces[g].V.at(t); }; }
    recIf(t0);
    double t = t0; double h = b.hCur;
    if (freezeIdle && b.frozenIdle && ((quietUntil > 0.0 && t1 <= quietUntil + 1e-12) || (freezeHit && !b.modal && !b.liveHit)))   // E-02: the blocks of the voices that are not played are not stepped in the idle run before the trigger (their state stays at the stored state, load current held); their phases do not matter for the hit
    { b.wf.t.push_back(t1); b.wf.v.push_back(b.wf.hold); recIf(t1); b.ts.push_back(t1); for (size_t k = 0; k < probes.size(); ++k) b.pv[k].push_back(0.0); b.hLast = 0.0; b.hCur = h; return; }
    const bool quiet = fixedDt > 0 && eventHMax > fixedDt && (b.passive || (quietUntil > 0.0 && t1 <= quietUntil + 1e-12) || (tailEvHMax > 0.0 && tailDt > 0.0 && t0 >= tailFrom - 1e-12));
    if (quiet) {
      const size_t NO = s.op.size(); auto vd = [&](size_t i) { return s.V(s.op[i].inp) - s.V(s.op[i].inn); };
      if (b.vdPrev.size() != NO || b.hLast <= 0.0) { b.vdPrev.resize(NO); b.vdPrev2.resize(NO); b.sat.resize(NO); for (size_t i = 0; i < NO; ++i) { b.vdPrev[i] = b.vdPrev2[i] = vd(i); b.sat[i] = sgn3(s.V(s.op[i].out)); } b.hLast = fixedDt; b.holdUntil = t0 + eventHold; }
      while (t < t1 - 1e-13) {
        double tev = 1e9, tevP = 1e9; for (size_t i = 0; i < NO; ++i) if (b.sat[i] != 0) { const double d = b.vdPrev[i], r = (b.vdPrev[i] - b.vdPrev2[i]) / std::max(b.hLast, 1e-12); if (r * d < 0.0) { tev = std::min(tev, -d / r); tevP = std::min(tevP, -d / r); } else if (std::fabs(d) < 0.05 && std::fabs(r) >= statR) tev = 0.0; }
        const bool nearEv = t < b.holdUntil || tev <= eventMargin;
        if (std::getenv("DGG_PART_STATS")) { if (b.predBy.size() != NO) { b.predBy.assign(NO, 0); b.statBy.assign(NO, 0); }
          if (t < b.holdUntil) ++b.nHold; else if (tev <= eventMargin) { ++b.nPred; for (size_t i = 0; i < NO; ++i) if (b.sat[i] != 0) { const double d = b.vdPrev[i], r = (b.vdPrev[i] - b.vdPrev2[i]) / std::max(b.hLast, 1e-12); if (r * d < 0.0 && -d / r <= eventMargin) ++b.predBy[i]; else if (!(r * d < 0.0) && std::fabs(d) < 0.05) ++b.statBy[i]; } } }
        const bool idleQa = qaRtol > 0.0 && quietUntil > 0.0 && t1 <= quietUntil + 1e-12;   // error-controlled steps only in the idle run, not in the event-controlled blocks during the hit
        double hh = nearEv ? fixedDt : std::min(idleQa ? qaHmax : eventHMax, std::max(fixedDt, 0.5 * (tev - eventMargin))); if (quietKnotCap && t >= quietKnotCapFrom - 1e-13) hh = std::min(hh, quietCap(t)); if (quietEvDt > 0.0 && t >= tailFrom - 1e-13) { if (t < b.holdUntil || tevP <= eventMargin) hh = std::min(hh, quietEvDt); if (b.hLast > 0.0) hh = std::min(hh, 2.0 * b.hLast); }
        hh = std::min(hh, std::min(t1, nextBreak(t)) - t);
        if (idleQa && !nearEv && hh > 2.0 * fixedDt)   // error-controlled step between the events (start after a fixed-step phase: new history, first step is not checked)
        {
          if (!b.lastAd) { s.xA = s.x; s.xB = s.x; s.hAB = 0.0; b.hAd = 2.0 * fixedDt; }
          double hAcc = std::min(std::max(b.hAd, fixedDt), hh), hNext = hAcc;
          if (!s.advanceAdaptive(t, hAcc, hNext, qaRtol, qaAtol, fixedDt, hh)) { b.net.x = b.st.x; break; }
          hh = hAcc; b.hAd = hNext; b.lastAd = true; ++b.qAd; b.qAdT += hh;
        }
        else { b.lastAd = false; if (!s.advance(t, hh)) { b.net.x = b.st.x; break; } ++b.qFixed; b.qFixedT += hh; }
        t += hh; b.hLast = hh; b.wf.t.push_back(t); b.wf.v.push_back(loadCurrent(b)); if (b.ifSrc >= 0) { b.vl.t.push_back(t); b.vl.v.push_back(s.V(s.vs[static_cast<size_t>(b.ifSrc)].p)); } recIf(t);
        for (size_t i = 0; i < NO; ++i) { b.vdPrev2[i] = b.vdPrev[i]; b.vdPrev[i] = vd(i); const signed char st = sgn3(s.V(s.op[i].out)); if (st != b.sat[i]) b.holdUntil = t + eventHold; b.sat[i] = st; }
        b.ts.push_back(t); for (size_t k = 0; k < probes.size(); ++k) b.pv[k].push_back(probes[k].block >= 0 && blocks[probes[k].block].get() == &b ? s.V(probes[k].node) : 0.0);
      }
      b.hCur = fixedDt; return;
    }
    if (fixedDt > 0 && fineEventDt > 0.0 && b.fineEvents)   // fixed-step window with a fine step around comparator flips (the event state is kept across windows like in the quiet branch)
    {
      const size_t NO = s.op.size(); auto vd = [&](size_t i) { return s.V(s.op[i].inp) - s.V(s.op[i].inn); };
      if (b.vdPrev.size() != NO || b.hLast <= 0.0) { b.vdPrev.resize(NO); b.vdPrev2.resize(NO); b.sat.resize(NO); for (size_t i = 0; i < NO; ++i) { b.vdPrev[i] = b.vdPrev2[i] = vd(i); b.sat[i] = sgn3(s.V(s.op[i].out)); } b.hLast = fixedDt; b.holdUntil = t0; }
      if (fineNodes.empty()) b.fineMask.clear(); else if (b.fineMask.size() != NO) { b.fineMask.assign(NO, 0); for (size_t i = 0; i < NO; ++i) b.fineMask[i] = fineNodes.count(s.nodeName[static_cast<size_t>(s.op[i].out)]) ? 1 : 0; }
      const bool masked = !b.fineMask.empty();
      while (t < t1 - 1e-13) {
        double tev = 1e9; for (size_t i = 0; i < NO; ++i) if (b.sat[i] != 0 && (!masked || b.fineMask[i])) { const double d = b.vdPrev[i], r = (b.vdPrev[i] - b.vdPrev2[i]) / std::max(b.hLast, 1e-12); if (r * d < 0.0) tev = std::min(tev, -d / r); else if (std::fabs(d) < 0.05 && std::fabs(r) >= statR) tev = 0.0; }   // masked: a stationary saturated op-amp (accent stage) is no flip candidate (statR as in the idle run)
        double hh = (t < b.holdUntil || tev <= (fineMargin >= 0.0 ? fineMargin : eventMargin)) ? (t >= fineLateFrom && fineLateDt > 0.0 ? fineLateDt : fineEventDt) : fixedDt;   // from fineLateFrom on the event step is fineLateDt
        hh = std::min(hh, std::min(t1, nextBreak(t)) - t); hh = std::min(hh, knotStepCap(t));
        if (!s.advance(t, hh)) { b.net.x = b.st.x; break; }
        t += hh; b.hLast = hh; b.wf.t.push_back(t); b.wf.v.push_back(loadCurrent(b)); if (b.ifSrc >= 0) { b.vl.t.push_back(t); b.vl.v.push_back(s.V(s.vs[static_cast<size_t>(b.ifSrc)].p)); } recIf(t);
        for (size_t i = 0; i < NO; ++i) { b.vdPrev2[i] = b.vdPrev[i]; b.vdPrev[i] = vd(i); const signed char st = sgn3(s.V(s.op[i].out)); if (st != b.sat[i] && (!masked || b.fineMask[i])) b.holdUntil = t + (fineHold >= 0.0 ? fineHold : eventHold); b.sat[i] = st; }
        b.ts.push_back(t); for (size_t k = 0; k < probes.size(); ++k) b.pv[k].push_back(probes[k].block >= 0 && blocks[probes[k].block].get() == &b ? s.V(probes[k].node) : 0.0);
      }
      b.hCur = fixedDt; return;
    }
    b.hLast = 0.0;   // the fixed-step phase invalidates the event state (rebuilt when a quiet window follows)
    while (t < t1 - 1e-13) {
      double hh, hn;
      if (fixedDt > 0) { hh = std::min(fixedDt, std::min(t1, nextBreak(t)) - t); hh = std::min(hh, knotStepCap(t)); if (!s.advance(t, hh)) { b.net.x = b.st.x; hh = 0; break; } hn = fixedDt; }
      else {
        hh = std::min(h, t1 - t); const bool clipped = hh < h - 1e-15; hn = hh;
        if (!s.advanceAdaptive(t, hh, hn, rtol, atol, hMin, hMax)) { fprintf(stderr, "block %s: step failed at t=%g\n", b.name.c_str(), t); hh = 0; break; }
        h = (clipped && hn >= hh) ? std::max(hn, h) : hn; if (h > hMax) h = hMax;
        b.steps = s.nAccepted; b.rejected = s.nRejected;
      }
      t += hh; b.wf.t.push_back(t); b.wf.v.push_back(loadCurrent(b)); if (b.ifSrc >= 0) { b.vl.t.push_back(t); b.vl.v.push_back(s.V(s.vs[static_cast<size_t>(b.ifSrc)].p)); } recIf(t);
      b.ts.push_back(t); for (size_t k = 0; k < probes.size(); ++k) b.pv[k].push_back(probes[k].block >= 0 && blocks[probes[k].block].get() == &b ? s.V(probes[k].node) : 0.0);
    }
    b.hCur = h;
  }
  static signed char sgn3(double v) { return v > 10.0 ? 1 : (v < -10.0 ? -1 : 0); }
  void saveWindowStart(Block& b) { b.vdPrevS = b.vdPrev; b.vdPrev2S = b.vdPrev2; b.satS = b.sat; b.holdUntilS = b.holdUntil; b.hLastS = b.hLast; b.net.saveStateInto(b.st); b.xA = b.net.xA; b.xB = b.net.xB; b.hAB = b.net.hAB; b.hCurSave = b.hCur; }
  void restoreWindowStart(Block& b) { b.vdPrev = b.vdPrevS; b.vdPrev2 = b.vdPrev2S; b.sat = b.satS; b.holdUntil = b.holdUntilS; b.hLast = b.hLastS; b.net.restoreState(b.st); b.net.xA = b.xA; b.net.xB = b.xB; b.net.hAB = b.hAB; b.hCur = b.hCurSave; }
  // one macro window; returns false when a block fails. Waveform relaxation on the NOISE voltage V(t): the blocks draw the currents I(V); V = (Th - I) / gTot. Plain iteration (first pass) has the loop gain
  // Rno x (load conductance), which is no longer small while a hit drives the noise into the transistors of the voices; from the second pass on a secant estimate G(t) = dI / dV of the loads (from the two last
  // passes, clamped to passive values) is put into the update: V_new (gTot + G) = Th - I + G V_used (Newton on the interface waveform).
  // Window with the NOISE line inside the blocks that touch it and are not passive, and in the noisy windows the passive block that cannot be modelled (nzInside > 0; mode 1: iterate the interfaces, only one such block; mode 2: one pass for one such block, two passes for several). Modal blocks are not stepped (port model).
  // each inside block then solves the line itself: its interface source is a Thevenin source with the series resistance 1 / gTot of the master and the load currents of all other blocks (latest waveforms; the blocks that have not run yet in this window at their held value) as a known current.
  // Mode 1 needs a further pass only for an interface that a block that is run again takes part in (h owns TOM_NOISE: the toms read its voltage, their load current goes back into h); mode 2 runs every block once (the interface loads are held at the value of the window start), with several blocks that solve the line twice.
  bool windowInside(double t0, double t1, const std::vector<Block*>& ins, bool quietW) {
    const int nb = (int)blocks.size(); Block& h = *ins[0]; int hi = -1; for (int i = 0; i < nb; ++i) if (blocks[static_cast<size_t>(i)].get() == &h) hi = i;
    auto isIns = [&](const Block* b) { return std::find(ins.begin(), ins.end(), b) != ins.end(); };
    if (!quietW && modalOn && !modalBuilt) buildModels(t0);
    auto isModal = [&](const Block* b) { return b->modal && !quietW; };
    bool anyModal = false; for (auto& b : blocks) if (isModal(b.get())) anyModal = true;
    modalRun = anyModal;
    auto iterated = [&](const Block* b) { return isIns(b) || !b->passive; };   // blocks that run again in the later passes: the ones that solve the line and the played one
    for (auto& b : blocks) { if (isModal(b.get())) { b->pmWin = b->pm.st; continue; } saveWindowStart(*b); if (b->ifSrc >= 0) b->net.setSrcRs(static_cast<size_t>(b->ifSrc), isIns(b.get()) ? 1.0 / gTot : 0.0); }
    prepareThevenin(t0, t1);
    iHoldSum = 0.0; for (auto& b : blocks) if (!isModal(b.get())) iHoldSum += b->wf.hold; vAnalytic = true; vUse = Wave();
    for (Iface& f : ifaces) { f.V = Wave(); f.V.hold = f.vHold; f.I = Wave(); f.I.hold = f.iHold; }
    std::vector<char> rel(ifaces.size(), 0); bool anyRel = false, hOwns = false;
    for (size_t g = 0; g < ifaces.size(); ++g) { for (int i = 0; i < nb; ++i) if (iterated(blocks[static_cast<size_t>(i)].get()) && blocks[static_cast<size_t>(i)]->ifNode[g] >= 0) rel[g] = 1; if (rel[g]) anyRel = true; if (ifaces[g].owner == hi) hOwns = true; }
    auto onIf = [&](int i) { for (size_t g = 0; g < ifaces.size(); ++g) if (blocks[static_cast<size_t>(i)]->ifNode[g] >= 0) return true; return false; };
    std::vector<int> order; const int firstB = ifaces.empty() ? -1 : ifaces[0].owner;
    for (int i = 0; i < nb; ++i) if (blocks[static_cast<size_t>(i)].get() != &h && !iterated(blocks[static_cast<size_t>(i)].get()) && !isModal(blocks[static_cast<size_t>(i)].get()) && !onIf(i)) order.push_back(i);   // passive blocks without interface that do not solve the line: their currents are known before the blocks that solve it run
    if (hOwns) order.push_back(hi);
    if (firstB >= 0 && firstB != hi && !isModal(blocks[static_cast<size_t>(firstB)].get())) order.push_back(firstB);
    for (int i = 0; i < nb; ++i) if (std::find(order.begin(), order.end(), i) == order.end() && i != hi && !isModal(blocks[static_cast<size_t>(i)].get())) order.push_back(i);
    if (!hOwns) order.push_back(hi);
    Block* prim = &h; for (Block* q : ins) if (!q->passive) prim = q;   // the model states follow the line voltage of this block (the played one: its steps resolve the noise and the trigger events)
    int it = 0; bool conv = false; Wave iOthUsed;
    auto buildOth = [&](const Block& self) { Wave w; double hold = 0.0; std::vector<double> tg;   // sum of the load currents of the other blocks: waveforms of this window where they ran, else their value at the window start (modal blocks: in the Norton equivalent of the step)
      for (auto& b : blocks) if (b.get() != &self && !isModal(b.get())) { hold += b->wf.hold; for (double t : b->wf.t) if (t >= t0 - 1e-15) tg.push_back(t); }
      std::sort(tg.begin(), tg.end()); tg.erase(std::unique(tg.begin(), tg.end(), [](double a, double b2) { return b2 - a < 1e-15; }), tg.end());
      w.hold = hold; if (!tg.empty()) { w.t = tg; w.v.assign(tg.size(), 0.0); for (size_t k = 0; k < tg.size(); ++k) { double s = 0.0; for (auto& b : blocks) if (b.get() != &self && !isModal(b.get())) s += b->wf.at(tg[k]); w.v[k] = s; } } return w; };
    for (; it < maxIt; ++it) {
      double vMis = 0.0;
      for (int i : order) { Block& b = *blocks[static_cast<size_t>(i)]; if (it > 0 && !iterated(&b)) continue; if (it > 0) restoreWindowStart(b); const bool in = isIns(&b); if (in) { iOth = buildOth(b); if (&b == &h) iOthUsed = iOth; inB = &b; for (auto& q : blocks) if (isModal(q.get())) q->pm.st = q->pmWin; } else inB = nullptr; runBlock(b, t0, t1);
        if (in && &b == prim) for (auto& q : blocks) if (isModal(q.get())) q->pmEnd = q->pm.st;
        for (size_t g = 0; g < ifaces.size(); ++g) if (ifaces[g].owner == i && b.ifNode[g] >= 0) { const Wave& nw = b.ifW[g]; if (iterated(&b)) { double lo = 1e30, hi2 = -1e30, mv = 0.0; for (size_t k = 0; k < nw.t.size(); ++k) { lo = std::min(lo, nw.v[k]); hi2 = std::max(hi2, nw.v[k]); mv = std::max(mv, std::fabs(nw.v[k] - ifaces[g].V.at(nw.t[k]))); } vMis = std::max(vMis, mv - ifTolV - ifRelV * (hi2 - lo)); } ifaces[g].V = nw; } }
      ++passes;
      std::vector<double> tg; for (auto& b : blocks) if (!isModal(b.get())) tg.insert(tg.end(), b->wf.t.begin(), b->wf.t.end());
      std::sort(tg.begin(), tg.end()); tg.erase(std::unique(tg.begin(), tg.end(), [](double a, double b) { return b - a < 1e-15; }), tg.end());
      bool ifOk = vMis <= 0.0;
      for (size_t g = 0; g < ifaces.size(); ++g) { Iface& f = ifaces[g]; Wave In; In.t = tg; In.v.assign(tg.size(), 0.0); double mI = 0.0, sI = 0.0;
        for (size_t k = 0; k < tg.size(); ++k) { double sum = 0.0; for (int i = 0; i < nb; ++i) if (i != f.owner && blocks[static_cast<size_t>(i)]->ifNode[g] >= 0) sum += blocks[static_cast<size_t>(i)]->ifW[g].at(tg[k]); In.v[k] = sum; mI = std::max(mI, std::fabs(sum - f.I.at(tg[k]))); sI = std::max(sI, std::fabs(sum)); }
        if (rel[g] && mI > ifTolI + ifRelI * sI) ifOk = false; f.I = In; }
      bool lineOk = false;   // E-03: a second pass would change the line voltage by less than the tolerance. The blocks that solve the line saw each other's load current of the first pass at slightly different line voltages; the current error is the secant
      // admittance of the block times that voltage difference, and the line voltage follows it through the source resistance 1 / gTot.
      if (nzInside == 2 && it == 0 && ins.size() > 1 && passTol > 0.0) { lineOk = true; const Wave& vp = prim->vl; double lo = 1e30, hi3 = -1e30; for (double v : vp.v) { lo = std::min(lo, v); hi3 = std::max(hi3, v); }
        const double tol = passTol * (ifTolV + ifRelV * (hi3 - lo));
        for (Block* q : ins) if (q != prim) { double dvMax = 0.0, ilo = 1e30, ihi = -1e30, vlo = 1e30, vhi = -1e30; for (size_t k = 0; k < q->vl.t.size(); ++k) { dvMax = std::max(dvMax, std::fabs(q->vl.v[k] - vp.at(q->vl.t[k]))); ilo = std::min(ilo, q->wf.at(q->vl.t[k])); ihi = std::max(ihi, q->wf.at(q->vl.t[k])); vlo = std::min(vlo, q->vl.v[k]); vhi = std::max(vhi, q->vl.v[k]); }
          const double gSec = (vhi - vlo) > 1e-9 ? (ihi - ilo) / (vhi - vlo) : 0.0; if (gSec * dvMax / gTot > tol) lineOk = false; } }
      if (nzInside == 2 ? (ins.size() == 1 || it >= 1 || lineOk) : (!anyRel || ifOk)) { conv = true; ++it; break; }   // mode 2: one pass for a single block that solves the line, two passes for several (they see each other's current of the first pass)
    }
    inB = nullptr; if (anyModal) for (auto& q : blocks) if (isModal(q.get())) q->pm.st = q->pmEnd; modalRun = false;
    if (!conv) ++unconverged;
    ++windows; ++insideWindows; ++itHist[std::min(it, 7)];
    // line voltage at the steps of h and the load conductance seen at the line (secant over the window): they start the prediction of the blocks that do not solve the line in the next window
    { const Wave& w = h.wf; double sdi = 0.0, sdv = 0.0, vPrev = 0.0, iPrev = 0.0; for (size_t k = 0; k < w.t.size(); ++k) { const double itot = iOthUsed.at(w.t[k]) + w.v[k]; const double v = (theveninAt(w.t[k]) - itot) / gTot; if (k > 0) { sdi += (itot - iPrev) * (v - vPrev); sdv += (v - vPrev) * (v - vPrev); } vPrev = v; iPrev = itot; if (k + 1 == w.t.size()) { vEnd = v; haveVEnd = true; } }
      if (sdv > 1e-18) gCarry = std::min(std::max(sdi / sdv, 0.0), 1.0); }
    for (size_t k = 0; k < probes.size(); ++k) { Probe& p = probes[k]; Block& b = *blocks[static_cast<size_t>(p.block)]; if (p.t.empty() && t0 >= recordFrom) { p.t.push_back(t0); p.v.push_back(b.st.x.empty() ? 0.0 : (p.node > 0 ? b.st.x[static_cast<size_t>(p.node - 1)] : 0.0)); } for (size_t j = 0; j < b.ts.size(); ++j) { if (b.ts[j] < recordFrom) continue; p.t.push_back(b.ts[j]); p.v.push_back(b.pv[k][j]); } }
    for (auto& b : blocks) if (!isModal(b.get())) b->wf.hold = b->wf.v.back();
    for (size_t g = 0; g < ifaces.size(); ++g) { Iface& f = ifaces[g]; if (!f.V.v.empty()) f.vHold = f.V.v.back(); if (!f.I.v.empty()) f.iHold = f.I.v.back(); }
    T = t1; return true;
  }
  bool window(double t0, double t1) {
    const int nb = (int)blocks.size();
    if (nzInside) { const bool quietW = quietWindow(t1); const std::vector<Block*> ins = insideBlocks(quietW); if (!ins.empty()) return windowInside(t0, t1, ins, quietW); inB = nullptr; for (auto& b : blocks) if (b->ifSrc >= 0) b->net.setSrcRs(static_cast<size_t>(b->ifSrc), 0.0); }
    for (auto& b : blocks) saveWindowStart(*b);
    prepareThevenin(t0, t1);
    iHoldSum = 0.0; for (auto& b : blocks) iHoldSum += b->wf.hold; vAnalytic = true; vUse = Wave();
    Wave vPrevUsed, iPrevSum; bool havePrev = false;
    for (Iface& f : ifaces) { f.V = Wave(); f.V.hold = f.vHold; f.I = Wave(); f.I.hold = f.iHold; }
    std::vector<int> order; const int firstB = ifaces.empty() ? -1 : ifaces[0].owner; if (firstB >= 0) order.push_back(firstB); for (int i = 0; i < nb; ++i) if (i != firstB) order.push_back(i);   // the owner of the first secondary interface (TOM_NOISE) runs first, the toms read its voltage in the same pass
    double ifVMis = 0.0;
    int it = 0; bool conv = false;
    for (; it < maxIt; ++it) {
      for (int i : order) { if (it > 0 && blocks[i]->passive) continue; if (it > 0) restoreWindowStart(*blocks[i]); runBlock(*blocks[i], t0, t1);
        for (size_t g = 0; g < ifaces.size(); ++g) if (ifaces[g].owner == i && blocks[i]->ifNode[g] >= 0) { const Wave& nw = blocks[i]->ifW[g]; double lo = 1e30, hi = -1e30, mv = 0.0; for (size_t k = 0; k < nw.t.size(); ++k) { lo = std::min(lo, nw.v[k]); hi = std::max(hi, nw.v[k]); mv = std::max(mv, std::fabs(nw.v[k] - ifaces[g].V.at(nw.t[k]))); }
          ifVMis = std::max(ifVMis, mv - ifTolV - ifRelV * (hi - lo)); if (std::getenv("PART_DBG") && windows % 300 == 5) fprintf(stderr, "  owner of iface %zu: dV %.3g swing %.3g\n", g, mv, hi - lo); ifaces[g].V = nw; } }
      ++passes;
      std::vector<double> tg; for (auto& b : blocks) tg.insert(tg.end(), b->wf.t.begin(), b->wf.t.end());
      std::sort(tg.begin(), tg.end()); tg.erase(std::unique(tg.begin(), tg.end(), [](double a, double b) { return b - a < 1e-15; }), tg.end());
      Wave vNew, iSum, vUsed; vNew.t = iSum.t = vUsed.t = tg; vNew.v.resize(tg.size()); iSum.v.resize(tg.size()); vUsed.v.resize(tg.size());
      double mism = 0.0, gSum = 0.0, vLo = 1e30, vHi = -1e30; long gN = 0;
      for (size_t k = 0; k < tg.size(); ++k) {
        const double t = tg[k]; double I = 0.0; for (auto& b : blocks) I += b->wf.at(t);
        const double vu = vAt(t); double G = 0.0;
        if (havePrev) { const double dv = vu - vPrevUsed.at(t); if (std::fabs(dv) > 1e-12) G = std::min(std::max((I - iPrevSum.at(t)) / dv, 0.0), 1.0); gSum += G; ++gN; }
        const double vn = (theveninAt(t) - I + G * vu) / (gTot + G);
        vUsed.v[k] = vu; iSum.v[k] = I; vNew.v[k] = vn; mism = std::max(mism, std::fabs(vn - vu)); vLo = std::min(vLo, vn); vHi = std::max(vHi, vn);
      }
      ifMaxMismatch = std::max(ifMaxMismatch, mism);
      if (gN > 0) gCarry = gSum / static_cast<double>(gN);
      vEnd = vUsed.v.back(); haveVEnd = true;
      bool ifOk = ifVMis <= 0.0; const double dbgV = ifVMis; ifVMis = 0.0;   // secondary interfaces: summed load current of the non-owner blocks (this pass) against the one the owner used
      for (size_t g = 0; g < ifaces.size(); ++g) { Iface& f = ifaces[g]; Wave In; In.t = tg; In.v.assign(tg.size(), 0.0); double mI = 0.0, sI = 0.0;
        for (size_t k = 0; k < tg.size(); ++k) { double sum = 0.0; for (int i = 0; i < nb; ++i) if (i != f.owner && blocks[i]->ifNode[g] >= 0) sum += blocks[i]->ifW[g].at(tg[k]); In.v[k] = sum; mI = std::max(mI, std::fabs(sum - f.I.at(tg[k]))); sI = std::max(sI, std::fabs(sum)); }
        if (mI > ifTolI + ifRelI * sI) ifOk = false; if (std::getenv("PART_DBG") && windows % 300 == 5) fprintf(stderr, "win %ld pass %d iface %zu: Vexcess %.3g  dI %.3g (|I| %.3g)  noise mism %.3g\n", windows, it, g, dbgV, mI, sI, mism); f.I = In; }
      if (mism <= ifTol + ifRel * (vHi - vLo) && ifOk) { conv = true; ++it; break; }   // absolute tolerance while the line is quiet (idle), relative to the swing of the noise while a hit drives it
      vPrevUsed = vUsed; iPrevSum = iSum; havePrev = true; vUse = vNew; vAnalytic = false;
    }
    if (!conv) ++unconverged;
    ++windows; ++itHist[std::min(it, 7)];
    // commit: probes, current at the end of the window as the next starting current
    for (size_t k = 0; k < probes.size(); ++k) { Probe& p = probes[k]; Block& b = *blocks[p.block]; if (p.t.empty() && t0 >= recordFrom) { p.t.push_back(t0); p.v.push_back(b.st.x.empty() ? 0.0 : (p.node > 0 ? b.st.x[p.node - 1] : 0.0)); } for (size_t j = 0; j < b.ts.size(); ++j) { if (b.ts[j] < recordFrom) continue; p.t.push_back(b.ts[j]); p.v.push_back(b.pv[k][j]); } }
    for (auto& b : blocks) b->wf.hold = b->wf.v.back();
    for (size_t g = 0; g < ifaces.size(); ++g) { Iface& f = ifaces[g]; if (!f.V.v.empty()) f.vHold = f.V.v.back(); if (!f.I.v.empty()) f.iHold = f.I.v.back(); }
    T = t1; return true;
  }
  bool run(double tstop) {
    // probes need their starting value (the state at T)
    for (Probe& p : probes) if (p.t.empty() && T >= recordFrom) { p.t.push_back(T); p.v.push_back(p.node > 0 ? blocks[p.block]->net.x[p.node - 1] : 0.0); }
    for (auto& b : blocks) b->wf.hold = b->modal ? 0.0 : loadCurrent(*b);
    for (size_t g = 0; g < ifaces.size(); ++g) { Iface& f = ifaces[g]; f.iHold = 0.0;   // interface state at T: owner voltage, summed load current of the others
      for (size_t i = 0; i < blocks.size(); ++i) { Block& b = *blocks[i]; if (b.ifNode[g] < 0) continue; Net& n = b.net; if (static_cast<int>(i) == f.owner) f.vHold = n.V(b.ifNode[g]); else f.iHold += -n.x[static_cast<size_t>(n.nNodes - 1 + n.vs[static_cast<size_t>(b.ifIdx[g])].br)]; } }
    unsigned nw = 0;
    while (T < tstop - 1e-13) { if (tailDt > 0.0 && T >= tailFrom - 1e-13) { fixedDt = tailDt; if (tailEvHMax > 0.0) eventHMax = tailEvHMax; } if (T >= tailKnotsFrom - 1e-13) bpEnd = 1e30;
      if (abort && ((++nw & 63u) == 0) && abort()) return false; const bool q = quietUntil > 0.0 && fixedDt > 0.0 && eventHMax > fixedDt && T + 2.0 * H <= quietUntil; const double t1 = std::min(T + (q ? quietH : H), q ? std::min(tstop, quietUntil) : tstop); if (!window(T, t1)) return false; if (afterWindow) afterWindow(T); }
    return true;
  }
};

}  // namespace n909
