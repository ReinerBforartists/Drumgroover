// Generic transient circuit engine for the TR-909 (Nava) voice netlists exported by export_net.py.
// MNA, Newton iteration, BDF2 (first step backward Euler). Primitives: R, C, diode (series R, junction charge), Gummel-Poon BJT
// (RB, RC, depletion and diffusion charge), ideal voltage sources. Macro models mirror the NGSpice subcircuits of nava_models.inc:
// M5218_V1, CD4069UB_INV, AN6912_CMP, BA6110_OTA_N.
#pragma once
#include <cstring>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace n909 {

constexpr double kVt = 0.025852;
constexpr double kGmin = 1e-12;

struct Diode { int a = 0, c = 0; double Is = 1e-14, N = 1, Rs = 0, Cjo = 0, Vj = 1, M = 0.5, Tt = 0, Bv = 1e30, Ibv = 1e-3; int q = -1; double vdLast = 0; };
struct Bjt {
  int c = 0, b = 0, e = 0, bi = 0, ci = 0; double s = 1;
  double IS = 1e-16, BF = 100, BR = 1, NF = 1, VAF = 1e30, RB = 0, RC = 0, CJC = 0, VJC = .75, MJC = .33, CJE = 0, VJE = .75, MJE = .33, NE = 1.5, ISE = 0, IKF = 1e30, TF = 0;
  int qbe = -1, qbc = -1; double vbeLast = 0, vbcLast = 0;
  bool capSet = false; double cbe0 = 0, cbc0 = 0;   // junction capacitances at the first transient evaluation (N909_PRUNE=lincap)
};
struct Vsrc { std::string name; int p = 0, n = 0; double dc = 0; int br = 0; std::function<double(double)> f; };
struct Res { int a, b; double g; bool dyn = false; double r0 = 0, r1 = 0, t0 = 0, tr = 0; std::vector<double> pw; };   // pw: piecewise-linear resistance over time, pairs (t, R) (net element RW)   // dyn: resistance r0 before t0, ramping linearly to r1 over tr (net element RT)
struct Cap { int a, b; double c; int q; };
struct Op5218 { int inp, inn, out, vcc, vee, n1; int qc; };
struct Inv4069 { int in, out, vdd, vss; int qin, qout; };
struct Cmp6912 { int inp, inn, out, vcc, vee; };
struct Ota6110 { int inp, inn, ctrl, out, vcc, vee; int qc; };

class Net {
 public:
  struct InvFn { double an, ap, pp, vtn, vtp, lam, cs; };
  InvFn invFit = {0, 0, 0, 0, 0, 0, 0};
  std::vector<std::string> nodeName;
  std::vector<Res> res; std::vector<Cap> cap; std::vector<Diode> dio; std::vector<Bjt> bjt; std::vector<Vsrc> vs;
  std::vector<Op5218> op; std::vector<Inv4069> inv; std::vector<Cmp6912> cmp; std::vector<Ota6110> ota;
  int nNodes = 0;   // including ground (index 0) and internal nodes
  int nUnk = 0;     // nodes-1 + branches
  int nq = 0;       // charge states
  std::vector<double> x, xprev1;   // solution, previous
  std::vector<double> q1, q2;      // charge history
  double hPrev = 0;   // spacing between the stored charges q2 and q1 (the step size of the last accepted step)
  bool varStepBdf2 = std::getenv("N909_FIXEDBDF2") == nullptr;   // default: variable-step coefficients; N909_FIXEDBDF2=1 restores the fixed 1.5 / -2 / 0.5
  double dt = 2.5e-6; int stepIndex = 0; bool dcMode = true; double srcScale = 1.0;
  double gminExtra = 0, opGain = 1.0;   // opGain: DC homotopy on the op-amp transconductance (1 = the model)

  int newNode(const std::string& nm) { nodeName.push_back(nm); return nNodes++; }
  int newQ() { return nq++; }

  // ---- loading ----
  bool load(const std::string& path) { std::ifstream f(path); if (!f) return false; return loadStream(f); }
  bool loadText(const std::string& text) { std::istringstream f(text); return loadStream(f); }   // netlist text (embedded netlists of the plugin)
  bool loadStream(std::istream& f) {
    nodeName.clear(); nNodes = 0;
    std::vector<std::string> lines; std::string ln; int maxNode = 0;
    while (std::getline(f, ln)) { if (!ln.empty()) lines.push_back(ln); }
    for (auto& l : lines) { std::istringstream is(l); std::string k; is >> k; if (k == "NODE") { int i; std::string nm; is >> i >> nm; if (i >= (int)nodeName.size()) nodeName.resize(i + 1); nodeName[i] = nm; maxNode = std::max(maxNode, i); } }
    nNodes = (int)nodeName.size();
    for (auto& l : lines) {
      std::istringstream is(l); std::string k; is >> k;
      if (k == "RW") { int a, b, cnt; is >> a >> b >> cnt; Res q; q.a = a; q.b = b; q.pw.resize(2 * cnt); for (int i = 0; i < 2 * cnt; ++i) is >> q.pw[i]; q.g = 1.0 / q.pw[1]; q.dyn = true; if (a != b) res.push_back(q); }
      else if (k == "RT") { int a, b; double r0, r1, t0, tr; is >> a >> b >> r0 >> r1 >> t0 >> tr; Res q; q.a = a; q.b = b; q.g = 1.0 / r0; q.dyn = true; q.r0 = r0; q.r1 = r1; q.t0 = t0; q.tr = tr; if (a != b) res.push_back(q); }
      else if (k == "R") { int a, b; double v; is >> a >> b >> v; if (a != b) res.push_back({a, b, 1.0 / v}); }
      else if (k == "RP") {   // pot leg: RP <pot> <a|b> n1 n2 <R total> <taper exponent> <value of the deck>; leg a = Rt p^e, leg b = Rt (1 - p^e) + 1 mOhm with p = max(position, 0.001)
        std::string pot, leg; int a, b; double rt, ex, v; is >> pot >> leg >> a >> b >> rt >> ex >> v;
        if (a != b) { res.push_back({a, b, 1.0 / v}); PotLeg pl; pl.res = (int)res.size() - 1; pl.pot = pot; pl.second = leg == "b"; pl.rt = rt; pl.ex = ex; pl.g0 = 1.0 / v; potLegs.push_back(pl); } }
      else if (k == "C") { int a, b; double v; is >> a >> b >> v; cap.push_back({a, b, v, newQ()}); }
      else if (k == "D") { Diode d; is >> d.a >> d.c >> d.Is >> d.N >> d.Rs >> d.Cjo >> d.Vj >> d.M >> d.Tt >> d.Bv >> d.Ibv; d.q = newQ(); dio.push_back(d); }
      else if (k == "Q") { Bjt q; std::string typ; is >> q.c >> q.b >> q.e >> typ; q.s = typ == "pnp" ? -1 : 1;
        is >> q.IS >> q.BF >> q.NF >> q.VAF >> q.RB >> q.RC >> q.CJC >> q.VJC >> q.MJC >> q.CJE >> q.VJE >> q.MJE >> q.NE >> q.ISE >> q.IKF >> q.TF; if (!(is >> q.BR)) q.BR = 1; addBjt(q); }
      else if (k == "VD" || k == "V") { Vsrc v; is >> v.name >> v.p >> v.n >> v.dc; vs.push_back(v); }
      else if (k == "VP") { Vsrc v; double v0, v1, d, r, fl, w, per; is >> v.name >> v.p >> v.n >> v0 >> v1 >> d >> r >> fl >> w >> per;
        v.f = [=](double t) { if (per > 0 && per < 1e8 && t > d) t = d + std::fmod(t - d, per);
          if (t < d) return v0; if (t < d + r) return v0 + (v1 - v0) * (t - d) / r; if (t < d + r + w) return v1; if (t < d + r + w + fl) return v1 + (v0 - v1) * (t - d - r - w) / fl; return v0; }; vs.push_back(v); }
      else if (k == "VS") { Vsrc v; double off, amp, fr; is >> v.name >> v.p >> v.n >> off >> amp >> fr; v.f = [=](double t) { return off + amp * std::sin(2 * 3.14159265358979 * fr * t); }; vs.push_back(v); }
      else if (k == "VW") { Vsrc v; int cnt; is >> v.name >> v.p >> v.n >> cnt; auto tv = std::make_shared<std::vector<double>>(2 * cnt); for (int i = 0; i < 2 * cnt; ++i) is >> (*tv)[i];
        v.f = [tv, cnt](double t) { const auto& a = *tv; if (t <= a[0]) return a[1]; if (t >= a[2 * (cnt - 1)]) return a[2 * (cnt - 1) + 1];
          int lo = 0, hi = cnt - 1; while (hi - lo > 1) { int mid = (lo + hi) / 2; if (a[2 * mid] <= t) lo = mid; else hi = mid; }
          double t0 = a[2 * lo], t1 = a[2 * hi]; return t1 > t0 ? a[2 * lo + 1] + (a[2 * hi + 1] - a[2 * lo + 1]) * (t - t0) / (t1 - t0) : a[2 * hi + 1]; }; vs.push_back(v); }
      else if (k == "VN") { Vsrc v; double mean, amp, step; is >> v.name >> v.p >> v.n >> mean >> amp >> step;
        auto smp = std::make_shared<std::vector<double>>(); auto rng = std::make_shared<unsigned long long>(0x9E3779B97F4A7C15ull ^ (unsigned long long)vs.size() ^ (std::getenv("N909_NSEED") ? std::strtoull(std::getenv("N909_NSEED"), nullptr, 10) * 0xD1B54A32D192ED03ull : 0ull));
        v.f = [=](double t) { long idx = (long)std::floor(t / step); while ((long)smp->size() <= idx + 1) { double s = 0; for (int q = 0; q < 12; ++q) { *rng = *rng * 6364136223846793005ull + 1442695040888963407ull; s += ((*rng >> 11) * (1.0 / 9007199254740992.0)); } smp->push_back((s - 6.0) * amp); }
          double fr = t / step - idx; return mean + (*smp)[idx] * (1 - fr) + (*smp)[idx + 1] * fr; }; vs.push_back(v); }
      else if (k == "X5218") { Op5218 m; is >> m.inp >> m.inn >> m.out >> m.vcc >> m.vee; m.n1 = newNode("x5218_n1"); m.qc = newQ(); op.push_back(m); }
      else if (k == "X4069") { Inv4069 m; is >> m.in >> m.out >> m.vdd >> m.vss; m.qin = newQ(); m.qout = newQ(); inv.push_back(m); }
      else if (k == "PAR4069") { is >> invFit.an >> invFit.ap >> invFit.pp >> invFit.vtn >> invFit.vtp >> invFit.lam >> invFit.cs; }
      else if (k == "XAN6912") { Cmp6912 m; is >> m.inp >> m.inn >> m.out >> m.vcc >> m.vee; cmp.push_back(m); }
      else if (k == "XBA6110") { Ota6110 m; is >> m.inp >> m.inn >> m.ctrl >> m.out >> m.vcc >> m.vee;
        Bjt q; q.c = q.b = m.ctrl; q.e = m.vee; q.IS = 35.1e-15; q.BF = 222.31; q.NF = 1; q.VAF = 315; q.RB = 9.86; q.RC = 0.68; q.CJC = 6.4e-12; q.VJC = .75; q.MJC = .33;
        q.CJE = 20e-12; q.VJE = .75; q.MJE = .33; q.NE = 1.319; q.ISE = 6.63e-15; q.IKF = 0.130; q.TF = 0.5564e-9; m.qc = (int)bjt.size(); addBjt(q); ota.push_back(m); }
    }
    int branch = 0; for (auto& v : vs) v.br = branch++;
    nUnk = nNodes - 1 + branch;
    x.assign(nUnk, 0.0); xprev1 = x; q1.assign(nq, 0.0); q2.assign(nq, 0.0);
    return true;
  }
  void addBjt(Bjt q) { q.bi = q.RB > 0 ? newNode("bi") : q.b; q.ci = q.RC > 0 ? newNode("ci") : q.c; q.qbe = newQ(); q.qbc = newQ(); bjt.push_back(q); }
  int nodeIndex(const std::string& nm) const { for (size_t i = 0; i < nodeName.size(); ++i) if (nodeName[i] == nm) return (int)i; return -1; }
  Vsrc* source(const std::string& nm) { for (auto& v : vs) if (v.name == nm) return &v; return nullptr; }
  // Circuit state for continuing a run (free-running oscillators and slow RC states are part of the circuit): unknowns, charge history of the integrator, step history.
  struct State { std::vector<double> x, q1, q2; int stepIndex = 0; double hPrev = 0; };
  State saveState() const { State st; st.x = x; st.q1 = q1; st.q2 = q2; st.stepIndex = stepIndex; st.hPrev = hPrev; return st; }
  bool restoreState(const State& st) { if (st.x.size() != x.size() || st.q1.size() != q1.size() || st.q2.size() != q2.size()) return false; x = st.x; xprev1 = x; q1 = st.q1; q2 = st.q2; stepIndex = st.stepIndex; hPrev = st.hPrev; dcMode = false; srcScale = 1.0; rampT = 0.0; gminExtra = 0.0; opGain = 1.0; vdState.clear(); return true; }
  struct PotLeg { int res = 0; std::string pot; bool second = false; double rt = 0, ex = 1, g0 = 0; };   // g0: conductance of the deck (nominal position)
  std::vector<PotLeg> potLegs;
  // Position (0..1) of a circuit pot named like the deck parameter without 'vr_' (VR2_bd, VR10_lt ...): sets the resistance of its legs (the netlist keeps the structure; only the conductances change). False: no such pot.
  void resetPots() { for (const PotLeg& pl : potLegs) res[pl.res].g = pl.g0; }   // all pots at the nominal position of the deck, bit-exact
  bool setPot(const std::string& name, double pos)
  {
    bool found = false; const double p = std::max(pos, 0.001);
    for (const PotLeg& pl : potLegs) if (pl.pot == name) { const double pe = std::pow(p, pl.ex); res[pl.res].g = 1.0 / (pl.second ? pl.rt * (1.0 - pe) + 1e-3 : pl.rt * pe); found = true; }
    return found;
  }
  bool setSource(const std::string& nm, std::function<double(double)> fn) { Vsrc* v = source(nm); if (!v) return false; v->f = std::move(fn); return true; }   // replaces the time function of a source of the netlist (trigger, accent, noise)
  bool setSourceDc(const std::string& nm, double volts) { Vsrc* v = source(nm); if (!v) return false; v->f = nullptr; v->dc = volts; return true; }

  // ---- helpers ----
  double V(int n) const { return n == 0 ? 0.0 : x[n - 1]; }
  struct Stamp { std::vector<double>* A; std::vector<double>* b; int n; };
  std::vector<double> A, B;
  bool bjtLinearOnly = false;
  std::vector<char> touched; bool patDirty = true;   // structural pattern of A (all positions ever stamped), see SparseLU below
  std::vector<int> touchedList;   // indices of touched entries: only these need zeroing before the next assembly
  void touch(size_t idx) { if (touched.size() != A.size()) { touched.assign(A.size(), 0); touchedList.clear(); patDirty = true; } if (!touched[idx]) { touched[idx] = 1; touchedList.push_back((int)idx); patDirty = true; } }
  void ad(int i, int j, double v) { if (i > 0 && j > 0) { size_t idx = (size_t)(i - 1) * nUnk + (j - 1); touch(idx); A[idx] += v; } }
  void rhs(int i, double v) { if (i > 0) B[i - 1] += v; }
  // current i leaving node a, entering node b, linearised in (nodes k[], derivatives g[])
  void nlCurrent(int a, int b, double i0, int nk, const int* k, const double* g) {
    double ieq = i0; for (int m = 0; m < nk; ++m) ieq -= g[m] * V(k[m]);
    for (int m = 0; m < nk; ++m) { ad(a, k[m], g[m]); ad(b, k[m], -g[m]); }
    rhs(a, -ieq); rhs(b, ieq);
  }
  void conduct(int a, int b, double g) { ad(a, a, g); ad(b, b, g); ad(a, b, -g); ad(b, a, -g); }
  // charge element between nodes a,b: q(v) with c = dq/dv at the present iterate
  void chargeStamp(int a, int b, int slot, double q0, double c0) {
    if (dcMode) return;
    double a0, a1, a2; if (stepIndex == 0) { a0 = 1; a1 = -1; a2 = 0; } else if (varStepBdf2 && hPrev > 0) { const double r = dt / hPrev; a0 = (1 + 2 * r) / (1 + r); a1 = -(1 + r); a2 = r * r / (1 + r); } else { a0 = 1.5; a1 = -2; a2 = 0.5; }   // variable-step BDF2 (r = h / h_prev; r = 1 gives 1.5, -2, 0.5): after a halved step the history is not equally spaced
    double v0 = V(a) - V(b); double g = a0 * c0 / dt; double ieq = (a0 * (q0 - c0 * v0) + a1 * q1[slot] + a2 * q2[slot]) / dt;
    conduct(a, b, g); rhs(a, -ieq); rhs(b, ieq);
    qNow[slot] = q0;
  }
  std::vector<double> qNow;

  // diode with series resistance: I(V) at terminal voltage V (anode-cathode), returns current and conductance; vd = internal junction voltage
  static void dioJunction(double vd, double Is, double N, double Bv, double Ibv, double& i, double& g) {
    double nvt = N * kVt;
    const double xbv = Bv > 1e20 ? 1e30 : Bv - nvt * std::log(std::max(Ibv / Is, 1.0));      // knee so that I(-Bv) = -Ibv
    if (vd > -xbv) {   // junction exponential down to the breakdown knee (a cut at -3 nvt made the current jump there and Newton cycled)
      double e = std::exp(std::min(vd / nvt, 80.0)); i = Is * (e - 1.0) + kGmin * vd; g = Is * e / nvt + kGmin;
    } else { double e = std::exp(std::min(-(xbv + vd) / nvt, 80.0)); i = -Is * e + kGmin * vd; g = Is * e / nvt + kGmin; }
  }
  static void dioRs(double V0, double Is, double N, double Rs, double Bv, double Ibv, double& vd, double& i, double& g) {
    if (Rs <= 0) { vd = V0; dioJunction(V0, Is, N, Bv, Ibv, i, g); return; }
    double v = vd; double ii, gg;
    for (int k = 0; k < 60; ++k) {
      dioJunction(v, Is, N, Bv, Ibv, ii, gg);
      double f = v + ii * Rs - V0, df = 1 + gg * Rs; double dv = f / df;
      // limit step in the exponential region
      double lim = 2 * N * kVt * 3; if (dv > lim) dv = lim; if (dv < -lim) dv = -lim;
      v -= dv; if (std::fabs(dv) < 1e-12) break;
    }
    dioJunction(v, Is, N, Bv, Ibv, ii, gg); vd = v; i = ii; g = gg / (1 + gg * Rs);
  }
  static void depl(double v, double cj0, double vj, double m, double& q, double& c) {
    const double fc = 0.5;
    if (cj0 <= 0) { q = 0; c = 0; return; }
    if (v < fc * vj) { double t = 1 - v / vj; q = vj * cj0 / (1 - m) * (1 - std::pow(t, 1 - m)); c = cj0 / std::pow(t, m); }
    else { double f1 = vj * (1 - std::pow(1 - fc, 1 - m)) / (1 - m), f2 = std::pow(1 - fc, 1 + m), f3 = 1 - fc * (1 + m);
      q = cj0 * (f1 + (1 / f2) * (f3 * (v - fc * vj) + m / (2 * vj) * (v * v - fc * fc * vj * vj))); c = cj0 / f2 * (f3 + m * v / vj); }
  }
  static double pnjlim(double vnew, double vold, double vt, double vcrit) {
    if (vnew > vcrit && std::fabs(vnew - vold) > vt + vt) {
      if (vold > 0) { double arg = (vnew - vold) / vt; vnew = arg > 0 ? vold + vt * (2 + std::log(arg - 2)) : vold - vt * (2 + std::log(2 - arg)); }
      else vnew = vt * std::log(vnew / vt);
    }
    return vnew;
  }
  static void dioLim(double V0, double Is, double N, double Rs, double Bv, double Ibv, double& state, double& iLin, double& g) {
    double vd = state, i, gg; dioRs(V0, Is, N, Rs, Bv, Ibv, vd, i, gg);
    double nvt = N * kVt, vcrit = nvt * std::log(nvt / (1.41421356 * Is)), vl = vd;
    if (Bv > 1e20 || vd > -3 * nvt) vl = pnjlim(vd, state, nvt, vcrit);
    double il, gl; dioJunction(vl, Is, N, Bv, Ibv, il, gl); double gt = gl / (1 + gl * Rs), Vl = vl + il * Rs;
    iLin = il + gt * (V0 - Vl); g = gt; state = vl;
  }
  void diodeStamp(int a, int c, double Is, double N, double Rs, double Cjo, double Vj, double M, double Tt, double Bv, double Ibv, int q, double* vdLast) {
    if (bjtLinearOnly) return;   // dump mode: diodes are listed separately as nonlinear devices
    double V0 = V(a) - V(c), i, g; dioLim(V0, Is, N, Rs, Bv, Ibv, *vdLast, i, g); double vd = *vdLast;
    int k[2] = {a, c}; double gg[2] = {g, -g}; nlCurrent(a, c, i, 2, k, gg);
    if (!dcMode && q >= 0) {
      double qj, cj; depl(vd, Cjo, Vj, M, qj, cj); double id_, gd_; dioJunction(vd, Is, N, Bv, Ibv, id_, gd_);
      double qq = qj + Tt * id_, cc = (cj + Tt * gd_) / (1 + (gd_)*Rs);   // charge on the junction, seen through Rs (approx.)
      chargeStamp(a, c, q, qq, cc);
    }
  }
  void bjtStamp(Bjt& t) {
    double s = t.s;
    if (t.bi != t.b) conduct(t.b, t.bi, 1.0 / t.RB);
    if (t.ci != t.c) conduct(t.c, t.ci, 1.0 / t.RC);
    if (bjtLinearOnly) return;   // dump mode: only the linear series resistors of the BJT
    int bi = t.bi, ci = t.ci, e = t.e;
    double vbe = s * (V(bi) - V(e)), vbc = s * (V(bi) - V(ci)); const double vbeAct = vbe, vbcAct = vbc;
    double vcritE = t.NF * kVt * std::log(t.NF * kVt / (1.41421356 * t.IS)); vbe = pnjlim(vbe, t.vbeLast, t.NF * kVt, vcritE);
    double vcritC = kVt * std::log(kVt / (1.41421356 * t.IS)); vbc = pnjlim(vbc, t.vbcLast, kVt, vcritC);
    t.vbeLast = vbe; t.vbcLast = vbc;
    double eE = std::exp(std::min(vbe / (t.NF * kVt), 80.0)), eC = std::exp(std::min(vbc / kVt, 80.0)), eL = std::exp(std::min(vbe / (t.NE * kVt), 80.0));
    double cbe = t.IS * (eE - 1) + kGmin * vbe, gbe = t.IS * eE / (t.NF * kVt) + kGmin;
    double cbc = t.IS * (eC - 1) + kGmin * vbc, gbc = t.IS * eC / kVt + kGmin;
    double cben = t.ISE * (eL - 1), gben = t.ISE * eL / (t.NE * kVt);
    double q1_ = 1.0 / (1.0 - vbc / t.VAF), dq1 = q1_ * q1_ / t.VAF;
    double q2_ = cbe / t.IKF, arg = std::max(0.0, 1 + 4 * q2_), sq = std::sqrt(arg);
    double qb = q1_ * (1 + sq) / 2; double dsqbe = arg > 0 ? (2 * gbe / t.IKF) / std::max(sq, 1e-12) : 0;
    double dqbe = q1_ * 0.5 * dsqbe, dqbc = dq1 * (1 + sq) / 2;
    double cc = (cbe - cbc) / qb - cbc / t.BR, cb = cbe / t.BF + cben + cbc / t.BR;
    if (dbgBjt && (std::fabs(cc) > 1.0 || std::fabs(cb) > 1.0)) fprintf(stderr, "BJT big current: c=%s b=%s e=%s s=%g vbe=%.3f vbc=%.3f cc=%.3g cb=%.3g\n", nodeName[t.c].c_str(), nodeName[t.b].c_str(), nodeName[e].c_str(), s, vbe, vbc, cc, cb);
    double dccbe = gbe / qb - (cbe - cbc) / (qb * qb) * dqbe;
    double dccbc = -gbc / qb - (cbe - cbc) / (qb * qb) * dqbc - gbc / t.BR;
    double dcbbe = gbe / t.BF + gben, dcbbc = gbc / t.BR;
    // terminal currents (NPN convention): into collector cc, into base cb, out of emitter. PNP: sign s.
    // vbe = s (Vbi - Ve), vbc = s (Vbi - Vci): d vbe/dVbi = s, dVe = -s ; d vbc/dVbi = s, dVci = -s
    int kk[3] = {bi, e, ci};
    { // current leaving ci into the device: s*cc
      double g[3] = {s * dccbe * s + s * dccbc * s, -s * dccbe * s, -s * dccbc * s};
      nlCurrent(ci, 0, s * (cc + dccbe * (vbeAct - vbe) + dccbc * (vbcAct - vbc)), 3, kk, g); // second node 0: ground reference, leaving ci only
    }
    { // current leaving bi into the device: s*cb
      double g[3] = {s * dcbbe * s + s * dcbbc * s, -s * dcbbe * s, -s * dcbbc * s};
      nlCurrent(bi, 0, s * (cb + dcbbe * (vbeAct - vbe) + dcbbc * (vbcAct - vbc)), 3, kk, g);
    }
    { // current entering e from the device: -(cc+cb)*s  => leaving e into device = -(s*(cc+cb))
      double dbe = dccbe + dcbbe, dbc = dccbc + dcbbc;
      double g[3] = {-(s * dbe * s + s * dbc * s), (s * dbe * s), (s * dbc * s)};
      nlCurrent(e, 0, -s * ((cc + cb) + dbe * (vbeAct - vbe) + dbc * (vbcAct - vbc)), 3, kk, g);
    }
    if (!dcMode) {
      double qje, cje, qjc, cjc; depl(vbe, t.CJE, t.VJE, t.MJE, qje, cje); depl(vbc, t.CJC, t.VJC, t.MJC, qjc, cjc);
      double qbe_ = qje + t.TF * cbe, cbe_ = cje + t.TF * gbe, qbc_ = qjc, cbc_ = cjc;
      if (pruneLinCap) {   // diagnosis: charges linear in the junction voltages with the capacitances of the DC operating point
        if (!t.capSet) { t.cbe0 = cbe_; t.cbc0 = cbc_; t.capSet = true; }
        qbe_ = t.cbe0 * vbe; qbc_ = t.cbc0 * vbc; cbe_ = t.cbe0; cbc_ = t.cbc0;
      }
      chargeStamp(bi, e, t.qbe, s * qbe_, cbe_); chargeStamp(bi, ci, t.qbc, s * qbc_, cbc_);
    }
  }

  // ---- full assembly ----
  void assemble(double tNow) {
    if (A.size() != (size_t)nUnk * nUnk || touched.size() != A.size()) A.assign((size_t)nUnk * nUnk, 0.0); else for (int idx : touchedList) A[idx] = 0.0;
    B.assign(nUnk, 0.0); qNow.assign(nq, 0.0);
    int nn = nNodes - 1;
    for (int i = 1; i < nNodes; ++i) ad(i, i, kGmin + gminExtra);
    for (auto& r : res)
    {
      if (!r.pw.empty())
      {   // piecewise-linear resistance
        const size_t n = r.pw.size() / 2; double R = r.pw[2 * n - 1];
        if (tNow <= r.pw[0]) R = r.pw[1]; else for (size_t i = 1; i < n; ++i) if (tNow <= r.pw[2 * i]) { const double t0 = r.pw[2 * i - 2], t1 = r.pw[2 * i]; R = r.pw[2 * i - 1] + (r.pw[2 * i + 1] - r.pw[2 * i - 1]) * (t1 > t0 ? (tNow - t0) / (t1 - t0) : 1.0); break; }
        conduct(r.a, r.b, 1.0 / R);
      }
      else conduct(r.a, r.b, r.dyn ? 1.0 / (tNow <= r.t0 ? r.r0 : (tNow >= r.t0 + r.tr || r.tr <= 0 ? r.r1 : r.r0 + (r.r1 - r.r0) * (tNow - r.t0) / r.tr)) : r.g);
    }
    for (auto& c : cap) if (!dcMode) chargeStamp(c.a, c.b, c.q, c.c * (V(c.a) - V(c.b)), c.c);
    for (auto& d : dio) diodeStamp(d.a, d.c, d.Is, d.N, d.Rs, d.Cjo, d.Vj, d.M, d.Tt, d.Bv, d.Ibv, d.q, &d.vdLast);
    for (auto& t : bjt) bjtStamp(t);
    for (auto& v : vs) {
      int br = nn + v.br; double val = v.f ? v.f(tNow) : v.dc * (rampT > 0 ? std::min(1.0, tNow / rampT) : srcScale);
      if (v.p > 0) { touch((size_t)(v.p - 1) * nUnk + br); touch((size_t)br * nUnk + (v.p - 1)); A[(size_t)(v.p - 1) * nUnk + br] += 1; A[(size_t)br * nUnk + (v.p - 1)] += 1; }
      if (v.n > 0) { touch((size_t)(v.n - 1) * nUnk + br); touch((size_t)br * nUnk + (v.n - 1)); A[(size_t)(v.n - 1) * nUnk + br] -= 1; A[(size_t)br * nUnk + (v.n - 1)] -= 1; }
      B[br] += val;
    }
    for (auto& m : op) stamp5218(m);
    for (auto& m : inv) stamp4069(m);
    for (auto& m : cmp) stamp6912(m);
    for (auto& m : ota) stamp6110(m);
  }

  // ---- macro models ----
  Diode clampD(double Is, double Rs) { Diode d; d.Is = Is; d.N = 1; d.Rs = Rs; return d; }
  std::map<const void*, double> vdState;
  void stamp5218(Op5218& m) {
    const double C1 = 53e-12, LOADK = 2000.0 / (2000.0 + 196.08), GM = 2 * 3.14159265 * 7e6 * C1 / LOADK, R1 = 316228.0 / LOADK / GM, IMAX = 2.2e6 * C1 / LOADK;
    const double CMRR = 31622.8, HEAD = 15.0 - (14.275 - 0.885), ROUT = 196.08;
    if (pruneDummy && m.inp == 0 && m.inn == 0 && m.out == 0) { ad(m.n1, m.n1, 1.0); return; }   // unused op-amp half (all pins grounded): only keeps its internal node regular
    conduct(m.inp, m.inn, 1.0 / 5e6); ad(m.inp, m.inp, 1e-12); ad(m.inn, m.inn, 1e-12);
    rhs(m.inp, 62.5e-9); rhs(m.inn, 57.5e-9);
    { int k[2] = {m.vcc, m.vee}; double g[2] = {0, 0}; nlCurrent(m.vcc, m.vee, 1.5e-3, 2, k, g); }   // Iq
    double vid = V(m.inp) - V(m.inn), vcm = 0.5 * (V(m.inp) + V(m.inn)); double xx = vid - 0.5e-3 + vcm / CMRR;
    const double gmEff = GM * opGain; double th = std::tanh(gmEff / IMAX * xx); double ig = IMAX * th, dig = gmEff * (1 - th * th);
    if (pruneLin) { ig = gmEff * xx; dig = gmEff; }   // linear input stage (small differential input, proven by the stress run); clamp diodes dropped below
    // current injected into n1: ig  => current leaving n1 = -ig
    if (!pruneNl) { int k[2] = {m.inp, m.inn}; double g[2] = {-dig * (1 + 0.5 / CMRR), -dig * (-1 + 0.5 / CMRR)}; nlCurrent(m.n1, 0, -ig, 2, k, g); }
    ad(m.n1, m.n1, 1.0 / R1);
    chargeStamp(m.n1, 0, m.qc, C1 * V(m.n1), C1);
    // clamp diodes: n1 -> (VCC - HEAD), (VEE + HEAD) -> n1
    for (int hi = 0; hi < (pruneLin || pruneNl ? 0 : 2); ++hi) {
      double Vt_ = hi ? V(m.n1) - V(m.vcc) + HEAD : V(m.vee) + HEAD - V(m.n1); double i, g; dioLim(Vt_, 1e-20, 1, 1.0, 1e30, 1e-3, vdState[(const void*)((char*)&m + hi)], i, g);
      if (hi) { int k[2] = {m.n1, m.vcc}; double gg[2] = {g, -g}; nlCurrent(m.n1, m.vcc, i, 2, k, gg); }
      else { int k[2] = {m.vee, m.n1}; double gg[2] = {g, -g}; nlCurrent(m.vee, m.n1, i, 2, k, gg); }
    }
    // Eout (unity VCVS from n1) with ROUT into OUT: current into OUT = (V(n1)-V(OUT))/ROUT
    double go = 1.0 / ROUT; ad(m.out, m.out, go); ad(m.out, m.n1, -go);
  }
  static double posf(double x) { return 0.5 * (x + std::sqrt(x * x + 0.01)); }
  static double invCur(const InvFn& f, double vgs, double vds, bool n) {
    double ov = posf(vgs - (n ? f.vtn : f.vtp)); double vs_ = std::max(f.cs * ov, 0.05);
    return (n ? f.an : f.ap) * std::pow(ov, f.pp) * std::tanh(vds / vs_) * (1 + f.lam * std::fabs(vds));
  }
  static void invDer(const InvFn& f, double vgs, double vds, bool n, double& dgs, double& dds) {   // d invCur / d vgs, d vds
    double x = vgs - (n ? f.vtn : f.vtp), r = std::sqrt(x * x + 0.01), ov = 0.5 * (x + r), dov = 0.5 * (1 + x / r);
    bool floor_ = f.cs * ov <= 0.05; double vs_ = floor_ ? 0.05 : f.cs * ov, dvs = floor_ ? 0.0 : f.cs * dov;
    double u = vds / vs_, T = std::tanh(u), sech2 = 1 - T * T, L = 1 + f.lam * std::fabs(vds), A = n ? f.an : f.ap;
    double P = std::pow(ov, f.pp), dP = ov > 0 ? f.pp * P / ov * dov : 0.0;
    dgs = A * (dP * T * L + P * sech2 * (-u / vs_) * dvs * L);
    dds = A * P * (sech2 / vs_ * L + T * f.lam * (vds > 0 ? 1.0 : (vds < 0 ? -1.0 : 0.0)));
  }
  static const InvFn& invDefault() { static const InvFn F = {0.001465, 0.001191, 1.214, 2.5, 1.476, 0.0, 0.555}; return F; }
  void stamp4069(Inv4069& m) {
    static const InvFn F = {0.001465, 0.001191, 1.214, 2.5, 1.476, 0.0, 0.555};   // set from cd4069ub_fit.json by loadInvFit()
    const InvFn& f = invFit.an > 0 ? invFit : F;
    double vin = V(m.in), vout = V(m.out), vdd = V(m.vdd), vss = V(m.vss);
    auto In = [&](double a, double b, double c, double d) { return invCur(f, a - d, b - d, true); };           // in,out,vdd,vss -> n current out->vss
    auto Ip = [&](double a, double b, double c, double d) { return invCur(f, c - a, c - b, false); };          // current vdd->out
    double in0 = In(vin, vout, vdd, vss), ip0 = Ip(vin, vout, vdd, vss);
    int nodes[4] = {m.in, m.out, m.vdd, m.vss};
    double gn[4], gp[4];
    {   // analytic derivatives (a finite-difference Jacobian made Newton cycle where the gate sits in its high-gain region)
      double ngs, nds, pgs, pds; invDer(f, vin - vss, vout - vss, true, ngs, nds); invDer(f, vdd - vin, vdd - vout, false, pgs, pds);
      gn[0] = ngs; gn[1] = nds; gn[2] = 0; gn[3] = -(ngs + nds);          // In(in,out,vdd,vss): vgs = in-vss, vds = out-vss
      gp[0] = -pgs; gp[1] = -pds; gp[2] = pgs + pds; gp[3] = 0;           // Ip: vgs = vdd-in, vds = vdd-out
    }
    if (!bjtLinearOnly) {   // dump mode: the channel currents and the clamp diodes are nonlinear devices listed separately (inv lines); only the constant capacitances stay in C
    nlCurrent(m.out, m.vss, in0, 4, nodes, gn);          // out -> vss
    nlCurrent(m.vdd, m.out, ip0, 4, nodes, gp);          // vdd -> out
    }
    chargeStamp(m.in, m.vss, m.qin, 10e-12 * (V(m.in) - V(m.vss)), 10e-12);
    chargeStamp(m.out, m.vss, m.qout, 5e-12 * (V(m.out) - V(m.vss)), 5e-12);
    if (bjtLinearOnly) return;
    for (int hi = 0; hi < 2; ++hi) {        // clamp diodes in->vdd, vss->in (Is 1e-14, Rs 10)
      double Vt_ = hi ? V(m.in) - V(m.vdd) : V(m.vss) - V(m.in); auto key = (const void*)((char*)&m + hi); double i, g; dioLim(Vt_, 1e-14, 1, 10.0, 1e30, 1e-3, vdState[key], i, g);
      if (hi) { int k[2] = {m.in, m.vdd}; double gg[2] = {g, -g}; nlCurrent(m.in, m.vdd, i, 2, k, gg); }
      else { int k[2] = {m.vss, m.in}; double gg[2] = {g, -g}; nlCurrent(m.vss, m.in, i, 2, k, gg); }
    }
  }
  void stamp6912(Cmp6912& m) {
    if (bjtLinearOnly) return;   // dump mode: nonlinear device, listed separately (cmp line)
    const double K = 3e-3 / 0.2, Vw = 15.0 / 2 / 200000.0;
    double vo = V(m.out) - V(m.vee), d = (V(m.inn) - V(m.inp)) / Vw; double th = std::tanh(d);
    double i = K * vo * 0.5 * (1 + th); double dvo = K * 0.5 * (1 + th), dd = K * vo * 0.5 * (1 - th * th) / Vw;
    int k[4] = {m.out, m.vee, m.inn, m.inp}; double g[4] = {dvo, -dvo, dd, -dd};
    nlCurrent(m.out, m.vee, i, 4, k, g);
  }
  void stamp6110(Ota6110& m) {
    if (bjtLinearOnly) return;   // dump mode: nonlinear device, listed separately (ota line)
    Bjt& qc = bjt[m.qc]; double GB = 1.0 / qc.RB, GC = 1.0 / qc.RC;
    double ictl = GB * (V(m.ctrl) - V(qc.bi)) + GC * (V(m.ctrl) - V(qc.ci));
    double vd = V(m.inp) - V(m.inn), th = std::tanh(vd / 52e-3);
    double iout = ictl * th;     // injected into OUT from VEE
    int k[4] = {m.ctrl, qc.bi, qc.ci, 0}; (void)k;
    int kk[5] = {m.ctrl, qc.bi, qc.ci, m.inp, m.inn}; double dth = (1 - th * th) / 52e-3;
    double g[5] = {(GB + GC) * th, -GB * th, -GC * th, ictl * dth, -ictl * dth};
    nlCurrent(m.vee, m.out, iout, 5, kk, g);
    for (int hi = 0; hi < 2; ++hi) {        // output compliance clamps (Is 1e-14, Rs 5), offset 1.7 V
      double Vt_ = hi ? V(m.out) - V(m.vcc) + 1.7 : V(m.vee) + 1.7 - V(m.out); auto key = (const void*)((char*)&m + hi); double i, gq; dioLim(Vt_, 1e-14, 1, 5.0, 1e30, 1e-3, vdState[key], i, gq);
      if (hi) { int kq[2] = {m.out, m.vcc}; double gg[2] = {gq, -gq}; nlCurrent(m.out, m.vcc, i, 2, kq, gg); }
      else { int kq[2] = {m.vee, m.out}; double gg[2] = {gq, -gq}; nlCurrent(m.vee, m.out, i, 2, kq, gg); }
    }
  }

  // ---- solving ----
  bool luSolve(std::vector<double>& M, std::vector<double>& rhsv, std::vector<double>& sol) {
    int n = nUnk; sol = rhsv; std::vector<int> piv(n);
    for (int k = 0; k < n; ++k) {
      int p = k; double mx = std::fabs(M[(size_t)k * n + k]);
      for (int i = k + 1; i < n; ++i) { double v = std::fabs(M[(size_t)i * n + k]); if (v > mx) { mx = v; p = i; } }
      if (mx < 1e-30) return false;
      if (p != k) { for (int j = 0; j < n; ++j) std::swap(M[(size_t)k * n + j], M[(size_t)p * n + j]); std::swap(sol[k], sol[p]); }
      double d = M[(size_t)k * n + k];
      for (int i = k + 1; i < n; ++i) { double f = M[(size_t)i * n + k] / d; if (f == 0) continue; M[(size_t)i * n + k] = 0; for (int j = k + 1; j < n; ++j) M[(size_t)i * n + j] -= f * M[(size_t)k * n + j]; sol[i] -= f * sol[k]; }
    }
    for (int i = n - 1; i >= 0; --i) { double s = sol[i]; for (int j = i + 1; j < n; ++j) s -= M[(size_t)i * n + j] * sol[j]; sol[i] = s / M[(size_t)i * n + i]; }
    return true;
  }
  // ---- sparse LU: fixed structure from the stamped pattern, minimum-degree column order, pivot rows chosen once by dense
  // partial pivoting (diagonal preferred) and kept; every Newton iteration only refactors numerically in that structure.
  // A pivot that becomes small relative to its column triggers a new pivot search; if that fails the dense solver is used. ----
  bool useSparse = !std::getenv("N909_DENSE");
  const char* envPrune = std::getenv("N909_PRUNE");   // comma list: dummyop (drop grounded op-amp halves), oplin (linear op-amp input stage, no clamps)
  bool pruneDummy = envPrune && std::strstr(envPrune, "dummyop"), pruneLin = envPrune && std::strstr(envPrune, "oplin"), pruneNl = envPrune && std::strstr(envPrune, "opnl");   // opnl: the whole input stage current (no linear gm either) is left to the generator as a nonlinear device
  bool pruneLinCap = envPrune && std::strstr(envPrune, "lincap");
  const char* envJF = std::getenv("N909_JF"); const char* envDebug = std::getenv("N909_DEBUG");   // read once, not per Newton iteration
  struct SparseLU {
    int n = 0; bool ok = false; std::vector<int> q, pinv, acp, ari, lp, li, up, ui; std::vector<double> lx, ux, d, w, y;
  } sp;
  long spRebuilds = 0, spRepivots = 0;
  void spAnalyze() {   // pattern -> column order, column pointers (CSC of the pattern)
    int n = nUnk; sp.n = n; sp.ok = false; ++spRebuilds;
    std::vector<std::vector<char>> adj(n, std::vector<char>(n, 0));
    sp.acp.assign(n + 1, 0); sp.ari.clear();
    for (int c = 0; c < n; ++c) { for (int r = 0; r < n; ++r) if (touched[(size_t)r * n + c]) { sp.ari.push_back(r); if (r != c) adj[r][c] = adj[c][r] = 1; } sp.acp[c + 1] = (int)sp.ari.size(); }
    std::vector<char> gone(n, 0); sp.q.assign(n, 0);
    for (int k = 0; k < n; ++k) {   // minimum degree on the symmetrised pattern
      int best = -1, bd = 1 << 30;
      for (int i = 0; i < n; ++i) if (!gone[i]) { int dg = 0; for (int j = 0; j < n; ++j) dg += adj[i][j]; if (dg < bd) { bd = dg; best = i; } }
      sp.q[k] = best; gone[best] = 1; std::vector<int> nb; for (int j = 0; j < n; ++j) if (adj[best][j] && !gone[j]) nb.push_back(j);
      for (int a : nb) for (int b : nb) if (a != b) adj[a][b] = 1;
      for (int j = 0; j < n; ++j) adj[best][j] = adj[j][best] = 0;
    }
  }
  bool spPivot() {   // row pivots by dense partial pivoting on the current numbers (columns in order q), then the L/U structure
    int n = sp.n; ++spRepivots; sp.ok = false;
    std::vector<double> M((size_t)n * n); std::vector<int> rp(n); for (int i = 0; i < n; ++i) rp[i] = i;
    for (int k = 0; k < n; ++k) for (int i = 0; i < n; ++i) M[(size_t)i * n + k] = A[(size_t)i * n + sp.q[k]];
    for (int k = 0; k < n; ++k) {
      int p = k; double mx = 0; for (int i = k; i < n; ++i) mx = std::max(mx, std::fabs(M[(size_t)i * n + k]));
      if (mx < 1e-30) return false;
      double dg = -1; int dr = -1; for (int i = k; i < n; ++i) if (rp[i] == sp.q[k]) { dg = std::fabs(M[(size_t)i * n + k]); dr = i; }
      if (dr >= 0 && dg >= 0.1 * mx) p = dr; else { double b = 0; for (int i = k; i < n; ++i) { double v = std::fabs(M[(size_t)i * n + k]); if (v > b) { b = v; p = i; } } }
      if (p != k) { for (int j = 0; j < n; ++j) std::swap(M[(size_t)k * n + j], M[(size_t)p * n + j]); std::swap(rp[k], rp[p]); }
      for (int i = k + 1; i < n; ++i) { double f = M[(size_t)i * n + k] / M[(size_t)k * n + k]; if (f == 0) continue; for (int j = k + 1; j < n; ++j) M[(size_t)i * n + j] -= f * M[(size_t)k * n + j]; }
    }
    sp.pinv.assign(n, 0); for (int k = 0; k < n; ++k) sp.pinv[rp[k]] = k;
    std::vector<std::vector<char>> S(n, std::vector<char>(n, 0));
    for (int c = 0; c < n; ++c) for (int r = 0; r < n; ++r) if (touched[(size_t)r * n + sp.q[c]]) S[sp.pinv[r]][c] = 1;
    for (int k = 0; k < n; ++k) { S[k][k] = 1; for (int i = k + 1; i < n; ++i) if (S[i][k]) for (int j = k + 1; j < n; ++j) if (S[k][j]) S[i][j] = 1; }
    sp.lp.assign(n + 1, 0); sp.up.assign(n + 1, 0); sp.li.clear(); sp.ui.clear();
    for (int k = 0; k < n; ++k) {
      for (int j = 0; j < k; ++j) if (S[j][k]) sp.ui.push_back(j);
      for (int i = k + 1; i < n; ++i) if (S[i][k]) sp.li.push_back(i);
      sp.up[k + 1] = (int)sp.ui.size(); sp.lp[k + 1] = (int)sp.li.size();
    }
    sp.lx.assign(sp.li.size(), 0.0); sp.ux.assign(sp.ui.size(), 0.0); sp.d.assign(n, 0.0); sp.w.assign(n, 0.0); sp.y.assign(n, 0.0); sp.ok = true; return true;
  }
  bool spRefactor() {   // false: a pivot became small (or zero) in the fixed structure
    int n = sp.n; double* w = sp.w.data();
    for (int k = 0; k < n; ++k) {
      int c = sp.q[k];
      for (int e = sp.acp[c]; e < sp.acp[c + 1]; ++e) { int r = sp.ari[e]; w[sp.pinv[r]] = A[(size_t)r * n + c]; }
      for (int e = sp.up[k]; e < sp.up[k + 1]; ++e) {
        int j = sp.ui[e]; double xj = w[j]; sp.ux[e] = xj;
        for (int f = sp.lp[j]; f < sp.lp[j + 1]; ++f) w[sp.li[f]] -= sp.lx[f] * xj;
      }
      double piv = w[k], mx = std::fabs(piv);
      for (int f = sp.lp[k]; f < sp.lp[k + 1]; ++f) mx = std::max(mx, std::fabs(w[sp.li[f]]));
      bool bad = !(std::fabs(piv) >= 1e-3 * mx) || std::fabs(piv) < 1e-30 || !std::isfinite(piv);
      if (!bad) { sp.d[k] = piv; for (int f = sp.lp[k]; f < sp.lp[k + 1]; ++f) sp.lx[f] = w[sp.li[f]] / piv; }
      for (int e = sp.up[k]; e < sp.up[k + 1]; ++e) w[sp.ui[e]] = 0.0;
      w[k] = 0.0; for (int f = sp.lp[k]; f < sp.lp[k + 1]; ++f) w[sp.li[f]] = 0.0;
      if (bad) { std::fill(sp.w.begin(), sp.w.end(), 0.0); return false; }
    }
    return true;
  }
  void spSolve(const std::vector<double>& b, std::vector<double>& sol) {
    int n = sp.n; double* y = sp.y.data(); sol.assign(n, 0.0);
    for (int i = 0; i < n; ++i) y[sp.pinv[i]] = b[i];
    for (int k = 0; k < n; ++k) { double yk = y[k]; if (yk != 0) for (int f = sp.lp[k]; f < sp.lp[k + 1]; ++f) y[sp.li[f]] -= sp.lx[f] * yk; }
    for (int k = n - 1; k >= 0; --k) { y[k] /= sp.d[k]; double yk = y[k]; for (int e = sp.up[k]; e < sp.up[k + 1]; ++e) y[sp.ui[e]] -= sp.ux[e] * yk; }
    for (int k = 0; k < n; ++k) sol[sp.q[k]] = y[k];
  }
  bool solveSystem(std::vector<double>& M, const std::vector<double>& Rhs, std::vector<double>& sol) {
    if (useSparse) {
      if (patDirty || !sp.ok || sp.n != nUnk) { spAnalyze(); patDirty = false; if (!spPivot()) goto dense; }
      if (!spRefactor()) { if (!spPivot() || !spRefactor()) goto dense; }
      spSolve(Rhs, sol); return true;
    }
  dense:
    M = A; std::vector<double> R2 = Rhs; return luSolve(M, R2, sol);
  }
  long itTotal = 0, itHist[8] = {0,0,0,0,0,0,0,0};
  bool newton(double tNow, int maxIt = 160) {
    std::vector<double> sol, M, R;
    for (int it = 0; it < maxIt; ++it) {
      if (it == 20 && envJF && std::fabs(tNow - std::atof(envJF)) < 1e-9) { checkJacobian(tNow, 30); std::exit(0); }
      ++itTotal; assemble(tNow);
      if (!solveSystem(M, B, sol)) return false;
      double maxd = 0; bool conv = true;
      for (int i = 0; i < nUnk; ++i) { double d = std::fabs(sol[i] - x[i]); double tol = (i < nNodes - 1 ? 1e-6 : 1e-8) + convRel * std::fabs(sol[i]); if (d > tol) conv = false; if (d > maxd) maxd = d; }
      // damp very large steps
      if (envDebug && std::fabs(tNow - std::atof(envDebug)) < 1e-9) { int im = 0; for (int i = 0; i < nUnk; ++i) if (std::fabs(sol[i] - x[i]) >= std::fabs(sol[im] - x[im])) im = i; fprintf(stderr, "it %d maxd %.3g at unk %d (%s) sol %.4g\n", it, maxd, im, im < nNodes - 1 ? nodeName[im + 1].c_str() : "branch", sol[im]); }
      double damp = 1.0; double vlim = (it < 3) ? 5.0 : 1.0; if (maxd > vlim) damp = vlim / maxd;
      if (it >= 40) damp *= (it < 80 ? 0.5 : 0.25);      // damp limit cycles
      for (int i = 0; i < nUnk; ++i) x[i] += damp * (sol[i] - x[i]);
      if (conv && it > 0) return true;
    }
    return false;
  }
  double convRel = std::getenv("N909_RELTOL") ? std::atof(std::getenv("N909_RELTOL")) : 1e-4;
  double rampT = 0;     // supply ramp time of the start-up fallback (0 = off)
  void startupFromZero(double ramp) {   // used when no DC operating point is found: ramp the DC sources from 0
    rampT = ramp; dcMode = false; stepIndex = 0; x.assign(nUnk, 0.0); q1.assign(nq, 0.0); q2.assign(nq, 0.0); srcScale = 1.0; gminExtra = 0;
  }
  // DC point by pseudo-transient continuation: supplies ramped up over 5 ms, circuit integrated until it has settled (default 0.5 s, step 20 us), then a Newton DC solve from
  // the settled state with no extra conductance. Returns true when that solve converges (the DC point of the circuit itself); otherwise the settled transient state is
  // kept as the starting state and false is returned.
  bool settleToDc(double tSettle) {
    startupFromZero(5e-3); double t = 0.0, h = 20e-6;
    while (t < tSettle - 1e-12) { if (!advance(t, h)) { fprintf(stderr, "dc: settling transient failed at t=%g\n", t); break; } t += h; }
    std::vector<double> xs = x; rampT = 0.0; srcScale = 1.0; gminExtra = 0.0; opGain = 1.0; dcMode = true;
    bool ok = newton(0.0, 200);
    if (!ok) { x = xs; dcMode = false; stepIndex = 0; assemble(0.0); q1 = qNow; q2 = qNow; return false; }
    dcMode = false; stepIndex = 0; assemble(0.0); q1 = qNow; q2 = qNow; return true;
  }
  double opGainStart = std::getenv("N909_OPGAIN") ? std::atof(std::getenv("N909_OPGAIN")) : 1.0;   // start value of the op-amp gain continuation in the DC solve (1 = off)
  bool dcOperatingPoint() {
    dcMode = true; x.assign(nUnk, 0.0); gminExtra = 1e-6; opGain = opGainStart;
    // adaptive source stepping: lambda 0 -> 1, halving the increment on failure
    double lam = 0.0, inc = 0.05; int guard = 0; std::vector<double> good = x;
    while (lam < 1.0 - 1e-12 && guard++ < 4000) {
      double tryLam = std::min(1.0, lam + inc); srcScale = tryLam; std::vector<double> xs = x;
      if (newton(0.0, 60)) { lam = tryLam; good = x; inc = std::min(inc * 1.5, 0.2); }
      else { x = xs; inc *= 0.5; if (inc < 1e-5) { fprintf(stderr, "dc: source stepping stalled at lambda=%g\n", lam); return settleToDc(std::getenv("N909_SETTLE") ? std::atof(std::getenv("N909_SETTLE")) : 0.5); } }
    }
    srcScale = 1.0;
    if (opGainStart < 1.0) {   // gain continuation: the op-amp loop gain from a small value to the model value at full supplies
      for (double g = opGainStart; ; g = std::min(1.0, g * 4.0)) { opGain = g; if (!newton(0.0, 100)) { fprintf(stderr, "dc: op-amp gain stepping failed at gain scale %g\n", g); opGain = 1.0; return false; } if (g >= 1.0) break; }
    }
    // gmin stepping down to the model floor
    std::vector<double> okx = x; double okg = 1e-6;
    for (double gm : {1e-7, 1e-8, 1e-9, 1e-10, 1e-11, 0.0}) { gminExtra = gm; if (newton(0.0, 100)) { okx = x; okg = gm; } else { x = okx; gminExtra = okg; fprintf(stderr, "dc: stopped at extra gmin %g (no convergence below)\n", okg); if (okg > 1e-9) return false; break; } }
    if (okg > 0) {   // no convergence without the extra conductance: try the pseudo-transient path for the circuit's own DC point
      std::vector<double> keepX = x; const double keepG = okg;
      if (settleToDc(std::getenv("N909_SETTLE") ? std::atof(std::getenv("N909_SETTLE")) : 0.5)) { fprintf(stderr, "dc: operating point by pseudo-transient settling (the extra gmin %g is not needed)\n", keepG); return true; }
      fprintf(stderr, "dc: pseudo-transient settling did not give a DC point; keeping the solution with extra gmin %g\n", keepG);
      x = keepX; gminExtra = keepG; dcMode = true; opGain = opGainStart; srcScale = 1.0; rampT = 0.0; newton(0.0, 100);
    }
    dcMode = false; stepIndex = 0; assemble(0.0);   // evaluates the charges at the operating point
    q1 = qNow; q2 = qNow; return true;
  }
  // consistency check of the stamped Jacobian against finite differences of the residual r(x) = A(x) x - b(x)
  void checkJacobian(double tNow, int maxPrint = 12) {
    auto resid = [&](const std::vector<double>& xx) { std::vector<double> save = x; x = xx; assemble(tNow); std::vector<double> r(nUnk, 0.0);
      for (int i = 0; i < nUnk; ++i) { double s = -B[i]; for (int j = 0; j < nUnk; ++j) s += A[(size_t)i * nUnk + j] * x[j]; r[i] = s; } x = save; return r; };
    assemble(tNow); std::vector<double> Jm = A; std::vector<double> r0 = resid(x); int bad = 0;
    for (int k = 0; k < nUnk; ++k) {
      std::vector<double> xx = x; double h = 1e-6 * std::max(1.0, std::fabs(x[k])); xx[k] += h; std::vector<double> r1 = resid(xx);
      for (int i = 0; i < nUnk; ++i) { double fd = (r1[i] - r0[i]) / h, an = Jm[(size_t)i * nUnk + k]; double tol = 1e-3 * (std::fabs(fd) + std::fabs(an)) + 1e-9;
        if (std::fabs(fd - an) > tol && bad < maxPrint) { ++bad; fprintf(stderr, "J mismatch row %d (%s) col %d (%s): analytic %.6g fd %.6g\n", i, i < nNodes - 1 ? nodeName[i + 1].c_str() : "br", k, k < nNodes - 1 ? nodeName[k + 1].c_str() : "br", an, fd); } }
    }
  }
  bool dbgBjt = std::getenv("N909_BJT") != nullptr;
  long subdivided = 0;
  // ---- adaptive step size ----
  // One accepted step from t with a step size chosen by the local error: the predictor is the linear extrapolation of the last two accepted solutions (xA at t, xB at t - hAB); the difference of the corrector to it, per node
  // voltage relative to atol + rtol |v|, estimates the local error of BDF2 (constant 1/3). A rejected step is repeated with a smaller h. On return h is the size of the step taken and hNext the size proposed for the next one.
  std::vector<double> xA, xB; double hAB = 0, errMax = 0; long nRejected = 0, nAccepted = 0;
  void rollback(const State& st) { x = st.x; q1 = st.q1; q2 = st.q2; stepIndex = st.stepIndex; hPrev = st.hPrev; }   // back to the state before a step (the solver mode, e.g. the supply ramp of the start-up, stays)
  bool advanceAdaptive(double t, double& h, double& hNext, double rtol, double atol, double hMin, double hMax)
  {
    if (xA.size() != x.size()) { xA = x; xB = x; hAB = 0; }
    h = std::min(std::max(h, hMin), hMax);
    for (int tries = 0; tries < 40; ++tries)
    {
      const State keep = saveState(); const std::vector<double> xaKeep = xA, xbKeep = xB; const double habKeep = hAB;
      if (!advance(t, h)) { rollback(keep); xA = xaKeep; xB = xbKeep; hAB = habKeep; if (h <= hMin * 1.0000001) return false; h = std::max(h * 0.5, hMin); continue; }
      double err = 0.0;
      if (hAB > 0.0) { const double r = h / hAB; for (int i = 0; i < nNodes - 1; ++i) { const double xp = xA[i] + r * (xA[i] - xB[i]); err = std::max(err, std::fabs(x[i] - xp) / (atol + rtol * std::fabs(x[i]))); } }
      const double e = err / 3.0; errMax = e;
      if (e > 1.0 && h > hMin * 1.0000001) { rollback(keep); xA = xaKeep; xB = xbKeep; hAB = habKeep; ++nRejected; h = std::max(h * std::max(0.9 * std::pow(1.0 / e, 1.0 / 3.0), 0.25), hMin); continue; }
      xB = xA; xA = x; hAB = h; ++nAccepted;   // accepted
      hNext = std::min(h * std::min(e > 1e-12 ? 0.9 * std::pow(1.0 / e, 1.0 / 3.0) : 2.0, 2.0), hMax); hNext = std::max(hNext, hMin);
      return true;
    }
    return false;
  }
  bool advance(double t, double h, int depth = 0) {   // step from t to t+h; on Newton failure retry with halves (backward Euler)
    double saveDt = dt, saveHp = hPrev; std::vector<double> xs = x, q1s = q1, q2s = q2; int si = stepIndex;
    dt = h; bool ok = step(t + h); dt = saveDt;
    if (ok) return true;
    x = xs; q1 = q1s; q2 = q2s; stepIndex = si; hPrev = saveHp;
    if (depth >= 8) return false;
    ++subdivided; stepIndex = 0; if (std::getenv("N909_FAILS") && subdivided < 100000) fprintf(stderr, "subdivide at t=%.9g h=%.3g depth=%d\n", t, h, depth);
    bool a = advance(t, h / 2, depth + 1); if (!a) return false;
    stepIndex = 0; return advance(t + h / 2, h / 2, depth + 1);
  }
  bool step(double tNow) {   // advance to time tNow (uses dt)
    std::vector<double> xs = x;
    if (!newton(tNow)) { x = xs; return false; }
    q2 = q1; assemble(tNow); q1 = qNow; ++stepIndex; hPrev = dt;  // qNow of the converged solution (assemble evaluated charges at x)
    return true;
  }
};

}  // namespace n909
