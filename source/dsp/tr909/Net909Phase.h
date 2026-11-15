// E-02: free-running oscillators of a voice sheet as a hybrid automaton (replaces the idle run `Preroll` and the phase table).
// Every oscillator of the TR-909 voices is a triangle (integrator, node `tri`) driven by a comparator (node `comp`): between the flips the triangle is a ramp with constant slope, the comparator state s = +-1 gives its direction.
// Per oscillator four numbers describe the cycle: the turning values xlo / xhi (intersection of the ramp before and after a comparator flip) and the slopes su / sd (rising / falling); they are measured from the
// circuit's own idle trajectory (here: a recorded idle run of the block, the same numbers come out of NGSpice traces, DETAILS 10.15 / 10.23). Away from the flips (|t - flip| > 150 us) all other unknowns of the block are
// linear functions of (x_i, s_i, x_i s_i) (measured: 135 of 149 nodes of the LT sheet within 1 mV, the rest is the soft-min of the clamp node lt_N_206 and is solved by the first Newton step); the map G is fitted by least squares on
// the same recorded run. A new phase = advance (x_i, s_i) along the ramps by a time, then move the block state by G^T (feat_new - feat_old): the slow states of the stored circuit state are kept, only the oscillator
// dependent part follows the phase.
#pragma once
#include "Net909Part.h"
#include <cmath>
#include <vector>
#include <string>

namespace n909 {

struct OscPair { const char* comp; const char* tri; };

class SheetPhase {
 public:
  struct Osc { int comp = -1, tri = -1; double xlo = 0, xhi = 0, su = 1, sd = 1; bool ok = false; double period() const { return (xhi - xlo) * (1.0 / su + 1.0 / sd); } };
  int block = -1, nU = 0, nf = 0; std::vector<Osc> osc; std::vector<double> G;   // G: nf x nU, row-major
  bool ok = false; std::string why;
  // recorded samples of the calibration run (block unknowns every window end)
  std::vector<double> rt; std::vector<double> rx;

  static constexpr double kExcl = 150e-6;   // distance from a comparator flip within which the samples do not enter the fits

  // block / node lookup; false if a node is missing
  bool setup(PartNet& net, const std::vector<OscPair>& pairs) {
    osc.clear(); block = -1; ok = false;
    for (const OscPair& p : pairs) {
      int nc = 0, nt = 0; const int bc = net.blockOfNodeName(p.comp, &nc), bt = net.blockOfNodeName(p.tri, &nt);
      if (bc < 0 || bt != bc || (block >= 0 && bc != block)) { why = std::string("node not found / different blocks: ") + p.comp + " " + p.tri; return false; }
      block = bc; Osc o; o.comp = nc; o.tri = nt; osc.push_back(o);
    }
    nU = net.blocks[static_cast<size_t>(block)]->net.nUnk; nf = 1 + 3 * static_cast<int>(osc.size()); return !osc.empty();
  }
  void feat(const double* xx, const double* ss, double* f) const {
    const size_t k = osc.size(); f[0] = 1.0; for (size_t i = 0; i < k; ++i) { f[1 + i] = xx[i]; f[1 + k + i] = ss[i]; f[1 + 2 * k + i] = xx[i] * ss[i]; }
  }
  // fit from the recorded samples (rt, rx: one row of nU unknowns per sample)
  bool fit() {
    ok = false; const size_t k = osc.size(), ns = rt.size(); if (ns < 200) { why = "too few samples"; return false; }
    std::vector<std::vector<double>> tri(k, std::vector<double>(ns)), sg(k, std::vector<double>(ns));
    for (size_t i = 0; i < k; ++i) for (size_t j = 0; j < ns; ++j) { tri[i][j] = rx[j * static_cast<size_t>(nU) + static_cast<size_t>(osc[i].tri - 1)]; sg[i][j] = rx[j * static_cast<size_t>(nU) + static_cast<size_t>(osc[i].comp - 1)] > 0.0 ? 1.0 : -1.0; }
    std::vector<char> far(ns, 1);   // away from the flips of every oscillator of the sheet
    for (size_t i = 0; i < k; ++i) {
      std::vector<size_t> fl; for (size_t j = 1; j < ns; ++j) if (sg[i][j] != sg[i][j - 1]) fl.push_back(j);
      for (size_t j : fl) { const double tf = rt[j]; for (size_t m = j; m > 0 && rt[m] > tf - kExcl - 40e-6; --m) far[m] = 0; for (size_t m = j; m < ns && rt[m] < tf + kExcl; ++m) far[m] = 0; }
      // ramps between consecutive flips
      struct Seg { double dir, a, b; }; std::vector<Seg> segs;
      for (size_t q = 0; q + 1 < fl.size(); ++q) {
        const double t0 = rt[fl[q]] + kExcl, t1 = rt[fl[q + 1]] - kExcl - 40e-6; double n = 0, st = 0, sx = 0, stt = 0, stx = 0;
        for (size_t j = fl[q]; j < fl[q + 1]; ++j) if (rt[j] > t0 && rt[j] < t1) { const double tt = rt[j] - t0; ++n; st += tt; sx += tri[i][j]; stt += tt * tt; stx += tt * tri[i][j]; }
        if (n < 8) continue; const double den = n * stt - st * st; if (std::fabs(den) < 1e-30) continue;
        const double sl = (n * stx - st * sx) / den, ic = (sx - sl * st) / n;   // x = ic + sl (t - t0)
        segs.push_back({sg[i][fl[q]], sl, ic - sl * t0});   // x = b + a t
      }
      double sumLo = 0, sumHi = 0, sumU = 0, sumD = 0; int nLo = 0, nHi = 0, nU2 = 0, nD = 0;
      for (size_t q = 0; q < segs.size(); ++q) { if (segs[q].dir > 0) { sumU += segs[q].a; ++nU2; } else { sumD += -segs[q].a; ++nD; } }
      for (size_t q = 0; q + 1 < segs.size(); ++q) {
        const double da = segs[q].a - segs[q + 1].a; if (std::fabs(da) < 1e-9) continue; const double ts = (segs[q + 1].b - segs[q].b) / da, xs = segs[q].b + segs[q].a * ts;
        if (segs[q].dir > 0) { sumHi += xs; ++nHi; } else { sumLo += xs; ++nLo; }
      }
      if (nLo < 2 || nHi < 2 || nU2 < 2 || nD < 2) { why = "too few ramps for oscillator " + std::to_string(i); return false; }
      osc[i].xlo = sumLo / nLo; osc[i].xhi = sumHi / nHi; osc[i].su = sumU / nU2; osc[i].sd = sumD / nD; osc[i].ok = true;
    }
    // least squares map unknowns <- features (normal equations, tiny ridge)
    const int F = nf; std::vector<double> AtA(static_cast<size_t>(F * F), 0.0), AtX(static_cast<size_t>(F) * static_cast<size_t>(nU), 0.0); std::vector<double> f(static_cast<size_t>(F)), xx(k), ss(k); size_t used = 0;
    for (size_t j = 0; j < ns; ++j) { if (!far[j]) continue; for (size_t i = 0; i < k; ++i) { xx[i] = tri[i][j]; ss[i] = sg[i][j]; } feat(xx.data(), ss.data(), f.data()); ++used;
      for (int a = 0; a < F; ++a) { for (int b = 0; b < F; ++b) AtA[static_cast<size_t>(a * F + b)] += f[static_cast<size_t>(a)] * f[static_cast<size_t>(b)]; const double fa = f[static_cast<size_t>(a)]; const double* row = &rx[j * static_cast<size_t>(nU)]; double* dst = &AtX[static_cast<size_t>(a) * static_cast<size_t>(nU)]; for (int u = 0; u < nU; ++u) dst[u] += fa * row[u]; } }
    if (used < static_cast<size_t>(4 * F)) { why = "too few samples away from the flips"; return false; }
    for (int a = 0; a < F; ++a) AtA[static_cast<size_t>(a * F + a)] *= (1.0 + 1e-12);
    // Gauss-Jordan on [AtA | AtX]
    std::vector<double> M(AtA); std::vector<double> R(AtX);
    for (int c = 0; c < F; ++c) {
      int piv = c; for (int r = c + 1; r < F; ++r) if (std::fabs(M[static_cast<size_t>(r * F + c)]) > std::fabs(M[static_cast<size_t>(piv * F + c)])) piv = r;
      if (std::fabs(M[static_cast<size_t>(piv * F + c)]) < 1e-18) { why = "singular normal equations"; return false; }
      if (piv != c) { for (int b = 0; b < F; ++b) std::swap(M[static_cast<size_t>(c * F + b)], M[static_cast<size_t>(piv * F + b)]); for (int u = 0; u < nU; ++u) std::swap(R[static_cast<size_t>(c) * static_cast<size_t>(nU) + static_cast<size_t>(u)], R[static_cast<size_t>(piv) * static_cast<size_t>(nU) + static_cast<size_t>(u)]); }
      const double d = M[static_cast<size_t>(c * F + c)];
      for (int b = 0; b < F; ++b) M[static_cast<size_t>(c * F + b)] /= d; for (int u = 0; u < nU; ++u) R[static_cast<size_t>(c) * static_cast<size_t>(nU) + static_cast<size_t>(u)] /= d;
      for (int r = 0; r < F; ++r) if (r != c) { const double m = M[static_cast<size_t>(r * F + c)]; if (m == 0.0) continue; for (int b = 0; b < F; ++b) M[static_cast<size_t>(r * F + b)] -= m * M[static_cast<size_t>(c * F + b)]; for (int u = 0; u < nU; ++u) R[static_cast<size_t>(r) * static_cast<size_t>(nU) + static_cast<size_t>(u)] -= m * R[static_cast<size_t>(c) * static_cast<size_t>(nU) + static_cast<size_t>(u)]; }
    }
    G = std::move(R); ok = true; rt.clear(); rt.shrink_to_fit(); rx.clear(); rx.shrink_to_fit(); return true;
  }
  // phase advance of one oscillator along its ramps: (x, s) after dt seconds
  static void advance(const Osc& o, double& x, double& s, double dt) {
    const double span = o.xhi - o.xlo, Tu = span / o.su, Td = span / o.sd; const double T = Tu + Td; if (dt <= 0.0) return;
    double tau = s > 0 ? (o.xhi - x) / o.su : (x - o.xlo) / o.sd; if (tau < 0.0) tau = 0.0;
    if (dt < tau) { x += (s > 0 ? o.su : -o.sd) * dt; return; }
    const double r = std::fmod(dt - tau, T);
    if (s > 0) { if (r < Td) { x = o.xhi - o.sd * r; s = -1.0; } else { x = o.xlo + o.su * (r - Td); s = 1.0; } }
    else { if (r < Tu) { x = o.xlo + o.su * r; s = 1.0; } else { x = o.xhi - o.sd * (r - Tu); s = -1.0; } }
  }
  // new block state (all unknowns) after advancing oscillator i by dts[i] seconds from the present state x0
  void moveState(const std::vector<double>& x0, const std::vector<double>& dts, std::vector<double>& out) const {
    const size_t k = osc.size(); std::vector<double> xo(k), so(k), xn(k), sn(k), fo(static_cast<size_t>(nf)), fn(static_cast<size_t>(nf));
    for (size_t i = 0; i < k; ++i) { xo[i] = x0[static_cast<size_t>(osc[i].tri - 1)]; so[i] = x0[static_cast<size_t>(osc[i].comp - 1)] > 0.0 ? 1.0 : -1.0; xn[i] = xo[i]; sn[i] = so[i]; advance(osc[i], xn[i], sn[i], dts[i]); }
    feat(xo.data(), so.data(), fo.data()); feat(xn.data(), sn.data(), fn.data()); out = x0;
    for (int a = 0; a < nf; ++a) { const double d = fn[static_cast<size_t>(a)] - fo[static_cast<size_t>(a)]; if (d == 0.0) continue; const double* g = &G[static_cast<size_t>(a) * static_cast<size_t>(nU)]; for (int u = 0; u < nU; ++u) out[static_cast<size_t>(u)] += d * g[u]; }
    for (size_t i = 0; i < k; ++i) { out[static_cast<size_t>(osc[i].tri - 1)] = xn[i]; }   // the triangle node itself exactly on its ramp
  }
};

}  // namespace n909
