// TR-909 hand clap (HC), hybrid reduced model (state hc909-lev7-fine1-gap3): production engine for the first millisecond, then a reduced Newton model (22 ports / 16 currents
// around the comparator events, 5 ports / 3 currents elsewhere) on the 5 us noise grid, with precomputed tables of the unsolved devices.
// Carried over mechanically from tools/_sweep/wrenfold_hc/hc_hybrid9.cpp by gen_hchybrid.py; validated R246-R264 (see tools/DEVELOPMENT_HANDOFF.md).
// One Renderer (and the two Red table sets it uses) per hit stream and per thread; the tables are read-only after Red::Load and may be shared.
#pragma once
#include "HitRunner.h"
#include "HostDecimator.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace dgg { namespace hch {

#define PR(...)
// Model version of the stored tables and equations: raise whenever device values, table contents or the reduced-model equations change (it is part of the cache key).
inline const char* const kModelVer = "hc909-lev7-fine1-gap3";
// Environment knobs exist only in the test harness (DGG_HC_ENV_KNOBS); the plugin always runs with the defaults.
inline const char* HcEnv(const char* n)
{
#ifdef DGG_HC_ENV_KNOBS
  return std::getenv(n);
#else
  (void)n; return nullptr;
#endif
}
[[noreturn]] inline void HcFail(const std::string& what) { throw std::runtime_error("HC hybrid: " + what); }

// ---- table package: one file per accent anchor, entries "F/<name>" (full set, includes F/fine.bin), "Q/<name>" (mini set) and "meta" (key=value lines)
struct HcBlob { const uint8_t* p = nullptr; size_t n = 0; };
struct Rd
{
  const uint8_t* p; size_t n, pos = 0; explicit Rd(const HcBlob& b) : p(b.p), n(b.n) {}
  size_t read(void* dst, size_t sz, size_t cnt)
  {
    const size_t want = sz * cnt;
    if (pos + want > n) { const size_t have = (n - pos) / sz; std::memcpy(dst, p + pos, have * sz); pos += have * sz; return have; }
    std::memcpy(dst, p + pos, want); pos += want; return cnt;
  }
};
struct HcPackage
{
  std::vector<uint8_t> own; std::map<std::string, HcBlob> idx; std::string prefix; std::map<std::string, std::string> meta; unsigned long long hash = 0;
  // Layout: "HCPK", u32 version (1), u32 count, count x { char name[48]; u64 offset; u64 size }, data. The memory must outlive the package (or use Open(std::vector&&)).
  bool Open(const uint8_t* data, size_t n)
  {
    if (n < 12 || std::memcmp(data, "HCPK", 4) != 0) return false;
    uint32_t ver, cnt; std::memcpy(&ver, data + 4, 4); std::memcpy(&cnt, data + 8, 4); if (ver != 1) return false;
    size_t pos = 12; idx.clear(); meta.clear();
    for (uint32_t i = 0; i < cnt; ++i)
    {
      if (pos + 64 > n) return false; char nm[49]; std::memcpy(nm, data + pos, 48); nm[48] = 0; uint64_t off, sz; std::memcpy(&off, data + pos + 48, 8); std::memcpy(&sz, data + pos + 56, 8); pos += 64;
      if (off + sz > n) return false; idx[nm] = HcBlob{ data + off, (size_t)sz };
    }
    hash = 1469598103934665603ULL; for (size_t i = 0; i < n; ++i) { hash ^= data[i]; hash *= 1099511628211ULL; }
    const auto it = idx.find("meta"); if (it != idx.end())
    {
      const std::string t((const char*)it->second.p, it->second.n); size_t a = 0;
      while (a < t.size()) { size_t b = t.find('\n', a); if (b == std::string::npos) b = t.size(); const std::string line = t.substr(a, b - a); const size_t e = line.find('='); if (e != std::string::npos) meta[line.substr(0, e)] = line.substr(e + 1); a = b + 1; }
    }
    return true;
  }
  bool Open(std::vector<uint8_t>&& bytes) { own = std::move(bytes); return Open(own.data(), own.size()); }
  HcBlob Find(const std::string& name) const { auto it = idx.find(prefix + name); return it == idx.end() ? HcBlob{} : it->second; }
  // view of one table set ("F/" or "Q/") of the package; shares the memory of the parent
  HcPackage View(const char* pfx) const { HcPackage v; v.idx = idx; v.prefix = pfx; v.meta = meta; v.hash = hash; return v; }
};

struct Tab
{
  int rows = 0, cols = 0, nk = 0; std::vector<int> kr; std::vector<float> yf; std::vector<double> yd, ref; bool useF = false;
  std::vector<double> buf; int base = -1, top = -2; long fills = 0, filledRows = 0;
  bool Load(const HcBlob& blob)
  {
    if (!blob.p) return false; Rd rd(blob);
    int h[4]; if (rd.read(h, 4, 4) != 4) HcFail("table data"); rows = h[0]; cols = h[1]; nk = h[2]; const int isz = h[3];
    kr.resize(nk); if (rd.read(kr.data(), 4, nk) != (size_t)nk) HcFail("table data");
    const size_t cnt = (size_t)nk * cols;
    if (isz == 41) { ref.resize(cols); if (rd.read(ref.data(), 8, cols) != (size_t)cols) HcFail("table data"); useF = true; yf.resize(cnt); if (rd.read(yf.data(), 4, cnt) != cnt) HcFail("table data"); }
    else if (isz == 4) { useF = true; yf.resize(cnt); if (rd.read(yf.data(), 4, cnt) != cnt) HcFail("table data"); }
    else { yd.resize(cnt); if (rd.read(yd.data(), 8, cnt) != cnt) HcFail("table data"); }
     return true;
  }
  double Y(int i, int c) const { const size_t k = (size_t)i * cols + c; return useF ? (double)yf[k] + (ref.empty() ? 0.0 : ref[c]) : yd[k]; }
  void Fill(int r0, int r1)
  {
    int i = (int)(std::upper_bound(kr.begin(), kr.end(), r0) - kr.begin()) - 1; if (i < 0) i = 0;
    int j = i + 1; while (j < nk - 1 && (kr[j] < r1 || kr[j] - kr[i] < 256)) ++j; j = std::min(j, nk - 1);
    base = kr[i]; top = kr[j]; buf.resize((size_t)(top - base + 1) * cols); ++fills; filledRows += top - base + 1;
    for (int s = i; s < j; ++s)
    {
      const int a0 = std::max(s - 1, 0), b0 = std::min(s + 1, nk - 1), a1 = s, b1 = std::min(s + 2, nk - 1);
      const double H = (double)(kr[s + 1] - kr[s]), d0 = (double)(kr[b0] - kr[a0]), d1 = (double)(kr[b1] - kr[a1]);
      for (int c = 0; c < cols; ++c)
      {
        const double y0 = Y(s, c), y1 = Y(s + 1, c), m0 = d0 > 0 ? (Y(b0, c) - Y(a0, c)) / d0 : 0.0, m1 = d1 > 0 ? (Y(b1, c) - Y(a1, c)) / d1 : 0.0;
        for (int r = kr[s]; r <= kr[s + 1]; ++r)
        {
          const double u = (r - kr[s]) / H, u2 = u * u, u3 = u2 * u;
          buf[(size_t)(r - base) * cols + c] = (2 * u3 - 3 * u2 + 1) * y0 + (u3 - 2 * u2 + u) * H * m0 + (-2 * u3 + 3 * u2) * y1 + (u3 - u2) * H * m1;
        }
      }
    }
    if (j == i) for (int c = 0; c < cols; ++c) buf[c] = Y(i, c);
  }
  const double* at(int r) { const int r1 = std::min(r + 1, rows - 1); if (r < base || r1 > top) Fill(r, r1); return &buf[(size_t)(r - base) * cols]; }
  size_t Bytes() const { return yf.size() * 4 + yd.size() * 8 + buf.size() * 8; }
};
inline std::vector<double> LdPacked(const HcBlob& blob)
{
  if (!blob.p) return {}; Rd rd(blob);
  int h[4]; if (rd.read(h, 4, 4) != 4) HcFail("table data"); const int rows = h[0], cols = h[1], nk = h[2], isz = h[3];
  std::vector<int> kr(nk); if (rd.read(kr.data(), 4, nk) != (size_t)nk) HcFail("table data");
  std::vector<double> y((size_t)nk * cols);
  if (isz == 41)
  {
    std::vector<double> ref(cols); if (rd.read(ref.data(), 8, cols) != (size_t)cols) HcFail("table data");
    std::vector<float> t(y.size()); if (rd.read(t.data(), 4, t.size()) != t.size()) HcFail("table data");
    for (size_t i = 0; i < t.size(); ++i) y[i] = (double)t[i] + ref[i % cols];
  }
  else if (isz == 4) { std::vector<float> t(y.size()); if (rd.read(t.data(), 4, t.size()) != t.size()) HcFail("table data"); for (size_t i = 0; i < t.size(); ++i) y[i] = t[i]; }
  else if (rd.read(y.data(), 8, y.size()) != y.size()) HcFail("table data");
  
  // slopes: 3-point central on the nonuniform knots (one-sided at the ends), cubic Hermite between knots
  std::vector<double> m(y.size());
  for (int i = 0; i < nk; ++i)
  {
    const int a = std::max(i - 1, 0), b = std::min(i + 1, nk - 1); const double dtk = (double)(kr[b] - kr[a]);
    for (int c = 0; c < cols; ++c) m[(size_t)i * cols + c] = dtk > 0 ? (y[(size_t)b * cols + c] - y[(size_t)a * cols + c]) / dtk : 0.0;
  }
  std::vector<double> out((size_t)rows * cols);
  for (int i = 0; i + 1 < nk; ++i)
  {
    const int r0 = kr[i], r1 = kr[i + 1]; const double H = (double)(r1 - r0);
    for (int r = r0; r <= r1; ++r)
    {
      const double u = (r - r0) / H, u2 = u * u, u3 = u2 * u, h00 = 2 * u3 - 3 * u2 + 1, h10 = u3 - 2 * u2 + u, h01 = -2 * u3 + 3 * u2, h11 = u3 - u2;
      for (int c = 0; c < cols; ++c) out[(size_t)r * cols + c] = h00 * y[(size_t)i * cols + c] + h10 * H * m[(size_t)i * cols + c] + h01 * y[(size_t)(i + 1) * cols + c] + h11 * H * m[(size_t)(i + 1) * cols + c];
    }
  }
  return out;
}
inline std::vector<double> Ld(const HcPackage& P, const std::string& n)
{
  { const HcBlob b = P.Find(n + ".pk"); if (b.p) { std::vector<double> p = LdPacked(b); if (!p.empty()) return p; } }
  const HcBlob b = P.Find(n + ".bin"); if (!b.p) HcFail("missing table " + n);
  std::vector<double> v(b.n / 8); std::memcpy(v.data(), b.p, v.size() * 8); return v;
}
struct Lev { std::vector<double> Ad, G0n, G1n, G0t, G1t, Pz, G0s, G1s, PGs, GU; };
struct Dev { int kind, idx, po, co; };   // kind 0 ota, 1 bjt, 2 diode, 3 comparator
// ---- optional fine-resolution table terms in windows (diagnostic, env FINE=file: nwin,h,NU,NP, then per window t0,cnt, means cnt*NU, end values cnt*NP)
struct FineTab { bool on = false; double h = 0; int NU = 0, NP = 0; unsigned long long hash = 0; std::vector<double> t0, t1; std::vector<size_t> off; std::vector<int> cnt; std::vector<double> fm, pe;
  // fine.bin of the table package of one anchor (floats of 8 bytes: nwin, h, NU, NP, then per window t0, cnt, step means cnt*NU, end values cnt*NP); absent file = no fine windows
  void Load(const HcBlob& blob)
  {
    on = false; if (!blob.p || blob.n < 32) return;
    hash = 1469598103934665603ULL; for (size_t i = 0; i < blob.n; ++i) { hash ^= blob.p[i]; hash *= 1099511628211ULL; }
    std::vector<double> raw8(blob.n / 8); std::memcpy(raw8.data(), blob.p, raw8.size() * 8); const long sz = (long)(raw8.size() * 8);
    const double* d = raw8.data(); size_t pos = 0, nd = sz / 8; if (nd < 4) return;
    const int nw = (int)d[0]; h = d[1]; NU = (int)d[2]; NP = (int)d[3]; pos = 4; t0.clear(); t1.clear(); off.clear(); cnt.clear(); fm.clear(); pe.clear();
    for (int w = 0; w < nw; ++w)
    {
      if (pos + 2 > nd) return; const int c = (int)d[pos + 1]; t0.push_back(d[pos]); cnt.push_back(c); t1.push_back(d[pos] + c * h); off.push_back(pe.size() / NP); pos += 2;
      const size_t nm = (size_t)c * NU, np_ = (size_t)c * NP; if (pos + nm + np_ > nd) return; fm.insert(fm.end(), d + pos, d + pos + nm); pos += nm; pe.insert(pe.end(), d + pos, d + pos + np_); pos += np_;
    }
    on = true;
  }
  // window index containing [ta, tb], or -1
  int Find(double ta, double tb) const { if (!on) return -1; size_t k = std::upper_bound(t0.begin(), t0.end(), ta + 1e-12) - t0.begin(); if (k == 0) return -1; --k; return (ta >= t0[k] - 1e-12 && tb <= t1[k] + 1e-12) ? (int)k : -1; }
  bool In(double t) const { if (!on) return false; size_t k = std::upper_bound(t0.begin(), t0.end(), t + 1e-12) - t0.begin(); if (k == 0) return false; --k; return t < t1[k] - 1e-12; } };

struct Red
{
  int R, NF, NP, NS, NU, n, LEV; double dt, ACC;
  std::vector<int> prow, sell, un, cx;
  std::vector<Dev> dev; double gb, gc, kcmp;
  std::vector<Lev> lev; FineTab fine;
  std::vector<double> PZs, PFun, PU, PCc, Xo, Dou, Dofsel, Dofun, Do1;
  mutable Tab FMU, FMGU, PBU, FTU, YU, PSILp;
  std::vector<std::vector<char>> sgn;   // sign (>0) of the comparator input ports (cx) on the whole grid
  std::vector<double> evs;              // comparator event times of the noise-free table (s), > 0.6 ms
  std::vector<double> bp;   // preprocessed bjt params 16 per device
  std::vector<double> dio;
  std::vector<char> jp;     // junction port flags (step limiting)
  void Load(const HcPackage& P)
  {
    auto d = Ld(P, "dims"); R = (int)d[0]; NF = (int)d[1]; NP = (int)d[2]; NS = (int)d[3]; NU = (int)d[4]; n = (int)d[5]; LEV = (int)d[6]; dt = d[7]; ACC = d[8];
    auto iv = [](const std::vector<double>& v) { std::vector<int> o(v.size()); for (size_t i = 0; i < v.size(); ++i) o[i] = (int)v[i]; return o; };
    prow = iv(Ld(P, "prow")); sell = iv(Ld(P, "sell")); un = iv(Ld(P, "un")); cx = iv(Ld(P, "cx"));
    auto lay = Ld(P, "layout"); for (size_t i = 0; i + 3 < lay.size(); i += 4) dev.push_back({(int)lay[i], (int)lay[i + 1], (int)lay[i + 2], (int)lay[i + 3]});
    auto o = Ld(P, "ota"); gb = o[0]; gc = o[1]; kcmp = Ld(P, "kcmp")[0];
    auto bj = Ld(P, "bjt"); dio = Ld(P, "dio");
    bp.assign(16 * (bj.size() / 9), 0.0);
    for (size_t k = 0; k < bj.size() / 9; ++k)
    {
      const double* q = &bj[9 * k]; double* b = &bp[16 * k]; for (int i = 0; i < 9; ++i) b[i] = q[i];
      b[9] = 1.0 / (q[4] * pcmfast::kVt); b[10] = 1.0 / (q[7] * pcmfast::kVt); b[11] = 1.0 / q[5]; b[12] = 1.0 / q[6]; b[13] = 1.0 / q[2]; b[14] = 1.0 / q[3];
    }
    lev.resize(LEV);
    for (int m = 0; m < LEV; ++m)
    {
      Lev& L = lev[m]; std::string s = std::to_string(m);
      L.Ad = Ld(P, "Ad" + s); L.G0n = Ld(P, "G0n" + s); L.G1n = Ld(P, "G1n" + s); L.G0t = Ld(P, "G0t" + s); L.G1t = Ld(P, "G1t" + s); L.Pz = Ld(P, "Pz" + s);
      L.G0s = Ld(P, "G0s" + s); L.G1s = Ld(P, "G1s" + s); L.PGs = Ld(P, "PGs" + s); L.GU = Ld(P, "GU" + s);
    }
    PZs = Ld(P, "PZs"); PFun = Ld(P, "PFun"); PU = Ld(P, "PU"); PCc = Ld(P, "PCc"); Xo = Ld(P, "Xo"); Dou = Ld(P, "Dou"); Dofsel = Ld(P, "Dofsel"); Dofun = Ld(P, "Dofun"); Do1 = Ld(P, "Do1");
    const char* tn[6] = {"FMU", "FMGU", "PBU", "FTU", "YU", "PSILp"}; Tab* tp[6] = {&FMU, &FMGU, &PBU, &FTU, &YU, &PSILp};
    for (int i = 0; i < 6; ++i) if (!tp[i]->Load(P.Find(std::string(tn[i]) + ".pk"))) HcFail(std::string("missing table ") + tn[i]);
    fine.Load(P.Find("fine.bin"));
    sgn.assign(cx.size(), std::vector<char>(n + 1));
    for (int k = 0; k <= n; ++k) { const double* r = PSILp.at(k); for (size_t q = 0; q < cx.size(); ++q) sgn[q][k] = r[cx[q]] > 0; }
    for (size_t q = 0; q < cx.size(); ++q) for (int k = 1; k <= n; ++k) if (sgn[q][k] != sgn[q][k - 1]) evs.push_back(k * dt);
    jp.assign(NP, 0); for (const Dev& v : dev) { if (v.kind == 1) { jp[v.po] = jp[v.po + 1] = 1; } if (v.kind == 2) jp[v.po] = 1; }
  }
};
inline double TrigAt(double t) { return pcmfast::HitTrigger(t); }

struct Solver
{
  const Red& M; pcmfast::HitNoise& nz;
  // rectified-noise correction for the mini set: the currents that are solved in the full set but table-driven in the mini set are larger than the noise-free table by a slowly varying factor (1 + rho_j);
  // rho is estimated in the full phase (moving means) and applied to the table currents in the mini phase
  bool dlOn = false; std::vector<double> dlfm, fmb, fgb, rhoT; std::vector<int> dlj;
  void SetDelta(const std::vector<double>& rho)
  {
    const int R = M.R, NU = M.NU; dlOn = true; dlfm = rho; dlj.clear(); for (int j = 0; j < NU; ++j) if (rho[j] != 0.0) dlj.push_back(j); fmb.assign(NU, 0.0); fgb.assign(R, 0.0); rhoT.assign(M.NP, 0.0);
  }
  void ClearDelta() { dlOn = false; }

  std::vector<double> z, fS, pprev; long nsteps = 0, nsub = 0, nits = 0, nfail = 0, nfall = 0;
  std::vector<double> evt, evy;   // event list (t, y)
  std::vector<double>* evtp = nullptr; std::vector<double>* evyp = nullptr;
  double HMIN;
  // scratch
  std::vector<std::vector<double>> zL, xL, pL; std::vector<double> bP, bG, bPt, bGt, bDp, bBest, bDpm, bJm, bFv, bA, rF, fPa, fFv, fSt;
  std::vector<double> zb, pb, F, g, Dp, J, dp, pp, pt, gt, tmp;
  Solver(const Red& m, pcmfast::HitNoise& n_) : M(m), nz(n_)
  {
    HMIN = M.dt / (double)(1 << (M.LEV - 1)); zL.assign(8, std::vector<double>(M.R)); xL.assign(8, std::vector<double>(M.NS)); pL.assign(8, std::vector<double>(M.NP));
    bP.resize(M.NP); bG.resize(M.NP); bPt.resize(M.NP); bGt.resize(M.NP); bDp.resize(M.NP); bBest.resize(M.NP); bDpm.resize((size_t)M.NS * M.NP); bJm.resize((size_t)M.NP * M.NP); bFv.resize(M.NS); bA.resize((size_t)M.NP * M.NP); rF.resize(M.NS); fPa.resize(M.NP); fFv.resize(M.NS); fSt.resize(M.NP); zb.resize(M.R); pb.resize(M.NP); F.resize(M.NS); g.resize(M.NP); Dp.resize((size_t)M.NS * M.NP); J.resize((size_t)M.NP * M.NP); dp.resize(M.NP); pp.resize(M.NP); pt.resize(M.NP); gt.resize(M.NP); tmp.resize(M.NP);
  }
  // device currents F(p) and derivatives Dp (NS x NP, row = current, column = port) when wantD
  void Fsel(const double* p, double* Fo, double* D) const
  {
    if (D) std::fill(D, D + (size_t)M.NS * M.NP, 0.0);
    for (const Dev& v : M.dev)
    {
      const double* q = p + v.po; double* Fc = Fo + v.co;
      switch (v.kind)
      {
      case 0: { const double ictl = M.gb * p[1] + M.gc * p[2], th = std::tanh(p[0]); Fc[0] = ictl * th; if (D) { D[0 * M.NP + 0] = ictl * (1 - th * th); D[0 * M.NP + 1] = M.gb * th; D[0 * M.NP + 2] = M.gc * th; } break; }
      case 1: { const pcmfast::BjtOut o = pcmfast::BjtEvalG(&M.bp[16 * v.idx], q[0], q[1]); Fc[0] = o.ic; Fc[1] = o.ib;
                if (D) { D[(size_t)(v.co) * M.NP + v.po] = o.d[0][0]; D[(size_t)(v.co) * M.NP + v.po + 1] = o.d[0][1]; D[(size_t)(v.co + 1) * M.NP + v.po] = o.d[1][0]; D[(size_t)(v.co + 1) * M.NP + v.po + 1] = o.d[1][1]; } break; }
      case 2: { double i, gg; pcmfast::DiodeEvalG(&M.dio[5 * v.idx], q[0], i, gg); Fc[0] = i; if (D) D[(size_t)v.co * M.NP + v.po] = gg; break; }
      default: { const double th = std::tanh(q[1]), vo = q[0]; Fc[0] = M.kcmp * vo * 0.5 * (1.0 + th); if (D) { D[(size_t)v.co * M.NP + v.po] = M.kcmp * 0.5 * (1.0 + th); D[(size_t)v.co * M.NP + v.po + 1] = M.kcmp * vo * 0.5 * (1.0 - th * th); } break; }
      }
    }
  }
  // g(p) = p - pb - PGs F(p); returns scaled residual norm
  const double gRS = HcEnv("RS") ? std::atof(HcEnv("RS")) : 1.0;
  double Resid(const double* p, double* gout, const double* PGs) const
  {
    std::vector<double>& Fv = const_cast<std::vector<double>&>(rF); Fsel(p, Fv.data(), nullptr);
    double r = 0; for (int a = 0; a < M.NP; ++a)
    {
      double s = p[a] - pb[a]; for (int j = 0; j < M.NS; ++j) s -= PGs[(size_t)a * M.NS + j] * Fv[j];
      gout[a] = s; if (!std::isfinite(s)) return INFINITY; r = std::max(r, std::fabs(s) / (gRS * (1e-6 + 1e-9 * std::fabs(p[a]))));
    }
    return r;
  }
  bool Newton(const double* p0, const double* PGs, double* pout, int& itOut, int maxIt = 60)
  {
    double rhist[64]; std::vector<double>& p = bP; p.assign(p0, p0 + M.NP); std::vector<double>& gg = bG; std::vector<double>& pt_ = bPt; std::vector<double>& gt_ = bGt; std::vector<double>& dpv = bDp; std::vector<double>& best = bBest; best.assign(p0, p0 + M.NP); std::vector<double>& Dpm = bDpm; std::vector<double>& Jm = bJm; std::vector<double>& Fv = bFv; int nzj[512], nzb[512]; double nzv[512];
    double rn = Resid(p.data(), gg.data(), PGs), bestr = rn; bool ok = false; int it = 0;
    for (; it < maxIt; ++it)
    {
      if (rn < 1.0) { ok = true; break; }
      Fsel(p.data(), Fv.data(), Dpm.data());
      {
        int nzn = 0; for (int j = 0; j < M.NS; ++j) for (int b = 0; b < M.NP; ++b) { const double v = Dpm[(size_t)j * M.NP + b]; if (v != 0.0) { nzj[nzn] = j; nzb[nzn] = b; nzv[nzn] = v; ++nzn; } }
        for (int a = 0; a < M.NP; ++a) { double* Jr = &Jm[(size_t)a * M.NP]; for (int b = 0; b < M.NP; ++b) Jr[b] = (a == b) ? 1.0 : 0.0; const double* Pr = &PGs[(size_t)a * M.NS]; for (int k = 0; k < nzn; ++k) Jr[nzb[k]] -= Pr[nzj[k]] * nzv[k]; }
      }
      dpv = gg;   // solve Jm dp = gg (Gaussian elimination, partial pivoting)
      {
        std::vector<double>& A = bA; std::copy(Jm.begin(), Jm.end(), A.begin()); bool sing = false; const int N = M.NP;
        for (int c = 0; c < N; ++c)
        {
          int pr = c; double mx = std::fabs(A[(size_t)c * N + c]); for (int r = c + 1; r < N; ++r) if (std::fabs(A[(size_t)r * N + c]) > mx) { mx = std::fabs(A[(size_t)r * N + c]); pr = r; }
          if (mx < 1e-300) { sing = true; break; }
          if (pr != c) { for (int k = 0; k < N; ++k) std::swap(A[(size_t)c * N + k], A[(size_t)pr * N + k]); std::swap(dpv[c], dpv[pr]); }
          for (int r = c + 1; r < N; ++r) { const double f = A[(size_t)r * N + c] / A[(size_t)c * N + c]; if (f != 0.0) { for (int k = c; k < N; ++k) A[(size_t)r * N + k] -= f * A[(size_t)c * N + k]; dpv[r] -= f * dpv[c]; } }
        }
        if (sing) break;
        for (int r = N - 1; r >= 0; --r) { double s = dpv[r]; for (int k = r + 1; k < N; ++k) s -= A[(size_t)r * N + k] * dpv[k]; dpv[r] = s / A[(size_t)r * N + r]; }
      }
      bool fin = true; for (double v : dpv) if (!std::isfinite(v)) fin = false; if (!fin) break;
      double dpm = 0; for (int a = 0; a < M.NP; ++a) if (M.jp[a]) dpm = std::max(dpm, std::fabs(dpv[a]));
      // the limit applies to the port change implied by the current step: dp itself (ports are the unknowns)
      static const int limMode = HcEnv("LIM") ? std::atoi(HcEnv("LIM")) : 0; static const double limV = HcEnv("LIMV") ? std::atof(HcEnv("LIMV")) : 0.04;
      if (limMode == 0) { if (dpm > 0.04) for (double& v : dpv) v *= 0.04 / dpm; }
      else if (limMode == 1) { for (int a_ = 0; a_ < M.NP; ++a_) if (M.jp[a_] && std::fabs(dpv[a_]) > limV) dpv[a_] = dpv[a_] > 0 ? limV : -limV; }
      else if (limMode == 2) { /* no limit */ }
      double lam = 1.0, rt = INFINITY;
      for (int ls = 0; ls < 12; ++ls)
      {
        for (int a = 0; a < M.NP; ++a) pt_[a] = p[a] - lam * dpv[a];
        rt = Resid(pt_.data(), gt_.data(), PGs); if (rt < rn * (1 - 1e-4 * lam) || rt < 1.0) break; lam *= 0.5;
      }
      p = pt_; gg = gt_; rn = rt; if (rn < bestr) { bestr = rn; best = p; }
      PR(if (it < 40) rhist[it] = rn;)
    }
    PR(if (HcEnv("RSTAT")) { if (!ok) { if (gNF < 400 && gNF % 20 == 0) std::printf("FAIL its %d rn: r1 %.2e r3 %.2e r5 %.2e r8 %.2e r12 %.2e r20 %.2e r39 %.2e\n", it, rhist[0], rhist[2], rhist[4], rhist[7], rhist[11], rhist[19], rhist[39]); } else if (it >= 8) { std::printf("OK-slow its %d rn: r1 %.2e r3 %.2e r5 %.2e r8 %.2e\n", it, rhist[0], rhist[2], rhist[4], rhist[7]); } })
    PR(if (!ok) { ++gNF; gNFits += it; }) itOut = it; std::copy(ok ? p.begin() : best.begin(), ok ? p.end() : best.end(), pout); return ok;
  }
  struct Ex { double nb, dna, tb_, dtra, ycomp; };
  // one step of size h from ta (seconds after the trigger); returns false when not converged
  bool Step(double ta, double h, int m, std::vector<double>& znew, std::vector<double>& xnew, std::vector<double>& pnew, Ex& ex)
  {
    const Lev& L = M.lev[m]; const int R = M.R, NP = M.NP, NS = M.NS, NU = M.NU;
    const int ia = std::min((int)std::floor(ta / M.dt + 1e-6), M.n - 1); const double tb = ta + h; const bool ongrid = (m == 0);
    const double na = nz.At(0.05 + ta), nb = nz.At(0.05 + tb), dna = (nz.At(0.05 + (ia + 1) * M.dt) - nz.At(0.05 + ia * M.dt)) / M.dt;
    PR(const double q0 = Now();)
    const double ta_ = TrigAt(ta), tb_ = TrigAt(tb), dtra = (TrigAt((ia + 1) * M.dt) - TrigAt(ia * M.dt)) / M.dt;
    // table terms
    const int fw = (M.fine.on && m == M.LEV - 1) ? M.fine.Find(ta, tb) : -1; const bool fineStep = fw >= 0; const size_t fidx = fineStep ? M.fine.off[fw] + (size_t)std::llround((ta - M.fine.t0[fw]) / M.fine.h) : 0;
    const double* fm = ongrid ? nullptr : (fineStep ? &M.fine.fm[fidx * M.fine.NU] : M.FMU.at(ia)); const double* fg = ongrid ? M.FMGU.at(ia) : nullptr;
    const double* dtail = nullptr;   // table end values (rows ia, ia+1) for the port correction
    if (dlOn)
    {
      if (ongrid) { const double* fu = M.FMU.at(ia); const double* g0 = fg; for (int i = 0; i < R; ++i) { double t = g0[i]; const double* gu = &M.lev[0].GU[(size_t)i * NU]; for (int j : dlj) t += gu[j] * dlfm[j] * fu[j]; fgb[i] = t; } fg = fgb.data(); }
      else { const double* f0 = fm; for (int j = 0; j < NU; ++j) fmb[j] = f0[j]; for (int j : dlj) fmb[j] = f0[j] * (1.0 + dlfm[j]); fm = fmb.data(); }
      dtail = M.FTU.at(ia);
    } const double* pbr = M.PBU.at(ia); const double frac = (ta - ia * M.dt) / M.dt + h / M.dt;   // position of tb in the interval
    for (int i = 0; i < R; ++i)
    {
      double s = 0; const double* Ad = &L.Ad[(size_t)i * R]; for (int j = 0; j < R; ++j) s += Ad[j] * z[j];
      const double* g0 = &L.G0s[(size_t)i * NS]; for (int j = 0; j < NS; ++j) s += g0[j] * fS[j];
      if (ongrid) s += fg[i]; else { const double* gu = &L.GU[(size_t)i * NU]; for (int j = 0; j < NU; ++j) s += gu[j] * fm[j]; }
      s += L.G0n[i] * na + L.G1n[i] * nb + L.Pz[(size_t)i * 4 + 1] * dna + L.G0t[i] * ta_ + L.G1t[i] * tb_ + L.Pz[(size_t)i * 4 + 2] * dtra + L.Pz[(size_t)i * 4 + 0] * M.ACC + L.Pz[(size_t)i * 4 + 3];
      zb[i] = s;
    }
    for (int a = 0; a < NP; ++a)
    {
      double s = 0; const double* pz = &M.PZs[(size_t)a * R]; for (int j = 0; j < R; ++j) s += pz[j] * zb[j];
      const double* pu = &M.PU[(size_t)a * 5]; s += pu[0] * M.ACC + pu[1] * nb + pu[2] * dna + pu[3] * tb_ + pu[4] * dtra;
      const double pbu = ongrid ? pbr[NP + a] : (fineStep ? M.fine.pe[fidx * M.fine.NP + a] : (1 - frac) * pbr[a] + frac * pbr[NP + a]);
      double dpb = 0.0; if (dtail) { const double* pf = &M.PFun[(size_t)a * NU]; for (int j : dlj) dpb += pf[j] * dlfm[j] * (ongrid ? dtail[NU + j] : (1 - frac) * dtail[j] + frac * dtail[NU + j]); }
      pb[a] = s + pbu + dpb;
    }
    PR(const double q1 = Now(); gT[0] += q1 - q0;)
    const double* PGs = L.PGs.data();
    // pcmfast ports -> NewtonStart
    int it = 0; static const int nit2 = HcEnv("NIT2") ? std::atoi(HcEnv("NIT2")) : 60; static const int nit1 = HcEnv("NIT1") ? std::atoi(HcEnv("NIT1")) : 60; int predq = -1; static const bool pred = HcEnv("PRED") != nullptr;
    if (pred)
    {
      Fsel(pprev.data(), fFv.data(), nullptr);
      for (size_t qx = 0; qx < M.cx.size() && predq < 0; ++qx)
      {
        const int jx = M.cx[qx]; double sp = pb[jx]; for (int j = 0; j < NS; ++j) sp += PGs[(size_t)jx * NS + j] * fFv[j];
        if ((sp > 0) != (pprev[jx] > 0)) predq = (int)qx;
      }
    }
    bool ok = predq >= 0 ? false : Newton(pprev.data(), PGs, pp.data(), it, nit1);
    if (!ok)
    {
      ++nfall; PR(const double tf0 = Now();)
      const int kn = std::min(std::max((int)std::lround(tb / M.dt), 0), M.n);
      int cbest[8], cdist[8], cord[8]; const int ncx = (int)M.cx.size();
      for (int qx = 0; qx < ncx; ++qx)
      {
        const char cs = pprev[M.cx[qx]] > 0; int best = -1;
        for (int d = 0; d <= M.n && best < 0; ++d) { const int k1 = kn - d, k2 = kn + d; if (k1 >= 0 && k1 <= M.n && M.sgn[qx][k1] != cs) best = k1; else if (k2 <= M.n && k2 >= 0 && M.sgn[qx][k2] != cs) best = k2; }
        cbest[qx] = best; cdist[qx] = best < 0 ? (1 << 30) : std::abs(best - kn); cord[qx] = qx;
      }
      static const bool byDist = HcEnv("ORDER") != nullptr;
      if (byDist) for (int a = 1; a < ncx; ++a) { const int v = cord[a]; int b = a - 1; while (b >= 0 && cdist[cord[b]] > cdist[v]) { cord[b + 1] = cord[b]; --b; } cord[b + 1] = v; }
      if (predq >= 0) { int pos = 0; while (cord[pos] != predq) ++pos; for (; pos > 0; --pos) cord[pos] = cord[pos - 1]; cord[0] = predq; }
      for (int oi = 0; oi < ncx; ++oi)
      {
        const int qx = cord[oi]; const int best = cbest[qx];
        PR(gT[4] += Now() - tf0;) if (best < 0) continue;
        const double* par = M.PSILp.at(best); std::vector<double>& pa = fPa; pa.assign(par, par + NP); std::vector<double>& Fv = fFv; std::vector<double>& st = fSt;
        for (int pass = 0; pass < 2 && !ok; ++pass)
        {
          if (pass == 0) st = pa; else { Fsel(pa.data(), Fv.data(), nullptr); for (int a = 0; a < NP; ++a) { double s = pb[a]; for (int j = 0; j < NS; ++j) s += PGs[(size_t)a * NS + j] * Fv[j]; st[a] = s; } }
          ok = Newton(st.data(), PGs, pp.data(), it, nit2);
        }
        if (ok) break;
      }
      PR(gT[3] += Now() - tf0;)
    }
    PR(const double q2 = Now(); gT[1] += q2 - q1;)
    nits += it + 1;
    Fsel(pp.data(), F.data(), nullptr); xnew.assign(F.begin(), F.end()); pnew.assign(pp.begin(), pp.end());
    for (int i = 0; i < R; ++i) { double s = zb[i]; const double* g1 = &L.G1s[(size_t)i * NS]; for (int j = 0; j < NS; ++j) s += g1[j] * xnew[j]; znew[i] = s; }
    double yu; if (ongrid) yu = M.YU.at(ia)[1]; else { yu = 0; const double* f0 = M.FTU.at(ia); const double* f1 = f0 + NU; for (int j = 0; j < NU; ++j) yu += M.Dofun[j] * ((1 - frac) * f0[j] + frac * f1[j]); }
    double y = M.Do1[0] + M.Dou[0] * M.ACC + M.Dou[1] * nb + M.Dou[2] * dna + M.Dou[3] * tb_ + M.Dou[4] * dtra + yu;
    for (int i = 0; i < R; ++i) y += M.Xo[i] * znew[i];
    for (int j = 0; j < NS; ++j) y += M.Dofsel[j] * xnew[j];
    PR(gT[2] += Now() - q2;)
    ex = {nb, dna, tb_, dtra, y};
    return ok;
  }
  void Advance(double ta, double h, int m)
  {
    std::vector<double>& zn = zL[m]; std::vector<double>& xn = xL[m]; std::vector<double>& pn = pL[m]; Ex ex; const bool ok = Step(ta, h, m, zn, xn, pn, ex); ++nsteps;
    bool flip = false; for (int jx : M.cx) if ((pprev[jx] > 0) != (pn[jx] > 0)) flip = true;
    static const double thr = HcEnv("THR") ? std::atof(HcEnv("THR")) : 0.0; bool big = false; if (thr > 0) for (int jx : M.cx) if (std::fabs(pn[jx] - pprev[jx]) > thr) big = true;
    if ((flip || big || !ok) && h > HMIN * 1.001 && m + 1 < M.LEV) { ++nsub; Advance(ta, h / 2, m + 1); Advance(ta + h / 2, h / 2, m + 1); return; }
    if (!ok) ++nfail;
    z = zn; fS = xn; pprev = pn; lastFlip = flip || lastFlip; evtp->push_back(ta + h); evyp->push_back(ex.ycomp);
  }
  bool lastFlip = false;
};

template <class Mdl>
struct Renderer
{
  const Red &MF, &MQ; const std::vector<char>& quiet; pcmfast::HitRunner<Mdl> r; pcmfast::HitNoise nz; decltype(pcmfast::HitTuningFor(Tr909Voice::Hc, false)) tu;
  pcmfast::HostDecimator dec; double idle = 0; size_t pushed = 0; std::vector<float> host;   // host-rate output (voltage minus idle level), filled by Pump()
  unsigned long long pkgHash = 0; double ytfK = 0, dptK = 0; std::string KeyFor(int seed, double capTime) const   // identifies everything the stored state depends on; an entry with another key is not used
  {
    unsigned long long hq = 1469598103934665603ULL; for (char c : quiet) { hq ^= (unsigned char)c; hq *= 1099511628211ULL; }
    char b[384]; std::snprintf(b, sizeof b, "hc|mv=%s|acc=%.6g|dt=%.6g|n=%d|seed=%d|ytf=%.6g|dpt=%.6g|cap=%.6g|win=%llx|lev=%d|fine=%llx/%.6g|ema=%.6g|pk=%llx", kModelVer, MF.ACC, MF.dt, MF.n, seed, ytfK, dptK, capTime, hq, MF.LEV, MF.fine.hash, MF.fine.h, emaOn ? emaTau : 0.0, pkgHash); return b;
  }
  Solver SF, SQ; Solver* S; std::vector<double> evt, evy; bool engine = true, full = true; double t_ = 0, hcur = 0; int hold = 0; long swc = 0; const double handover = 1.0e-3; const int HOLD = 8;
  Renderer(const Red& f, const Red& q, const std::vector<char>& qt, double ytf, double dpt) : MF(f), MQ(q), quiet(qt), SF(f, nz), SQ(q, nz), S(&SF)
  {
    tu = pcmfast::HitTuningFor(Tr909Voice::Hc, false); tu.ytol *= ytf; tu.dpTol = dpt; ytfK = ytf; dptK = dpt; r.Warm(tu);
    host.reserve(24000); dec.Init(48000.0); evt.reserve(200000); evy.reserve(200000); SF.evtp = SQ.evtp = &evt; SF.evyp = SQ.evyp = &evy;
  }
  struct Handover { std::vector<double> z, fS, pprev, et, ey; double time = 0, idle = 0; bool full = true; std::string key; std::vector<double> avg, avgT, dl; };
  // moving mean (time constant emaTau) of the offset "solved current of the full set minus noise-free table" for the currents that the mini set takes from the table
  bool emaOn = HcEnv("EMA") != nullptr; double emaTau = HcEnv("TAU") ? std::atof(HcEnv("TAU")) * 1e-6 : 200e-6; std::vector<double> dlAvg, dlAvgT; std::vector<std::pair<int, int>> dlMap;
  void InitEma()
  {
    if (dlMap.empty()) for (int ju = 0; ju < MQ.NU; ++ju) for (int jf = 0; jf < MF.NS; ++jf) if (MF.sell[jf] == MQ.un[ju]) dlMap.push_back({ju, jf});
    dlAvg.assign(MQ.NU, 0.0); dlAvgT.assign(MQ.NU, 0.0); SQ.ClearDelta();
  }
  Handover* cap = nullptr; Handover* cap2 = nullptr; double capT = -1; int capSeed = 0; int curSeed = 0;   // cap2: state at the first grid time >= capT on the reduced model   // when set, the state at the end of the engine phase is stored there
  void BeginCached(int seed, const Handover& h)
  {
    nz.Start(seed, 5e-6, 0.0); engine = false; full = true; S = &SF; swc = 0; hold = 0; InitEma();
    evt.assign(h.et.begin(), h.et.end()); evy.assign(h.ey.begin(), h.ey.end());
    SF.z = h.z; SF.fS = h.fS; SF.pprev = h.pprev; SQ.z = SF.z; SQ.fS.assign(SF.fS.begin(), SF.fS.begin() + MQ.NS); SQ.pprev.assign(SF.pprev.begin(), SF.pprev.begin() + MQ.NP);
    t_ = std::round(h.time / MF.dt) * MF.dt; hcur = MF.dt;
  }
  bool BeginCachedAt(int seed, const Handover& h)   // restart from a state taken with cap2 (either solver may be active); false (nothing started) when the entry's key differs
  {
    if (h.key != KeyFor(seed, h.time)) return false;
    curSeed = seed;
    nz.Start(seed, 5e-6, 0.0); engine = false; swc = 0; hold = 0; dec.Restart(0.0); host.clear(); pushed = 0; idle = h.idle;
    evt.assign(h.et.begin(), h.et.end()); evy.assign(h.ey.begin(), h.ey.end());
    if (h.full) { SF.z = h.z; SF.fS = h.fS; SF.pprev = h.pprev; full = true; S = &SF; }
    else { SQ.z = h.z; SQ.fS = h.fS; SQ.pprev = h.pprev; full = false; S = &SQ; }
    InitEma(); if (!h.avg.empty()) dlAvg = h.avg; if (!h.avgT.empty()) dlAvgT = h.avgT; if (!h.full && !h.dl.empty()) SQ.SetDelta(h.dl);
    t_ = h.time; hcur = MF.dt; Pump(); return true;
  }
  void SetHostRate(double fs) { dec.Init(fs); }
  void Begin(int seed)
  {
    curSeed = seed; capSeed = seed; nz.Start(seed, 5e-6, 0.0); r.Begin(tu, &nz, 0.35); engine = true; full = true; S = &SF; evt.clear(); evy.clear(); swc = 0; hold = 0; dec.Restart(0.0); host.clear(); pushed = 0; idle = r.idle(); InitEma();
  }
  bool Done() const { return !engine && t_ >= 0.35 - 1e-12; }
  void Pump() { static const bool gridOnly = HcEnv("GRIDONLY") != nullptr; for (; pushed < evt.size(); ++pushed) { if (gridOnly) { const double q = evt[pushed] / 5e-6; if (std::fabs(q - std::llround(q)) > 1e-6) continue; } dec.Push(evt[pushed], evy[pushed] - idle); double v; while (dec.Pop(v)) host.push_back((float)v); } }
  void Render(double tEnd) { RenderCore(tEnd); Pump(); }
  void RenderCore(double tEnd)
  {
    if (engine)
    {
      while (!r.Finished() && r.Time() < handover - 1e-12 && r.Time() < tEnd) { r.Advance(); evt.push_back(r.Time()); evy.push_back(r.Output()); }
      if (!(r.Finished() || r.Time() >= handover - 1e-12)) return;
      auto& m = r.ModelState();
      SF.z.assign(m.z, m.z + MF.R); SF.fS.resize(MF.NS); for (int j = 0; j < MF.NS; ++j) SF.fS[j] = m.f0[MF.sell[j]]; SF.pprev.resize(MF.NP); for (int a = 0; a < MF.NP; ++a) SF.pprev[a] = m.p[MF.prow[a]];
      SQ.z = SF.z; SQ.fS.assign(SF.fS.begin(), SF.fS.begin() + MQ.NS); SQ.pprev.assign(SF.pprev.begin(), SF.pprev.begin() + MQ.NP);
      full = true; S = &SF; t_ = std::round(r.Time() / MF.dt) * MF.dt; hcur = MF.dt; engine = false;
      if (cap) { cap->z = SF.z; cap->fS = SF.fS; cap->pprev = SF.pprev; cap->et = evt; cap->ey = evy; cap->time = r.Time(); }
    }
    const double dt = MF.dt;
    while (t_ < 0.35 - 1e-12 && t_ < tEnd)
    {
      if (cap2 && t_ >= capT - 1e-12 && hcur >= dt * 0.999 && std::fabs(t_ / dt - std::llround(t_ / dt)) < 1e-6) { cap2->z = S->z; cap2->fS = S->fS; cap2->pprev = S->pprev; cap2->et = evt; cap2->ey = evy; cap2->time = t_; cap2->full = full; cap2->idle = idle; cap2->avg = dlAvg; cap2->avgT = dlAvgT; cap2->dl = SQ.dlOn ? SQ.dlfm : std::vector<double>(); cap2->key = KeyFor(capSeed, t_); cap2 = nullptr; }
      const long kk0 = (long)std::llround(t_ / dt);
      if (hcur >= dt * 0.999 && std::fabs(t_ / dt - kk0) < 1e-6)
      {
        const bool wantFull = !quiet[std::min<long>(kk0, MF.n)];
        if (wantFull != full)
        {
          ++swc;
          if (wantFull)
          {
            SF.z = SQ.z; { const double* pr = MF.PSILp.at((int)kk0); SF.pprev.assign(pr, pr + MF.NP); }
            SF.Fsel(SF.pprev.data(), SF.fFv.data(), nullptr); SF.fS = SF.fFv;
            for (int j = 0; j < MQ.NS; ++j) SF.fS[j] = SQ.fS[j];
            for (int a = 0; a < MQ.NP; ++a) SF.pprev[a] = SQ.pprev[a];
            S = &SF; SQ.ClearDelta();
          }
          else
          {
            SQ.z = SF.z; SQ.fS.assign(SF.fS.begin(), SF.fS.begin() + MQ.NS); SQ.pprev.assign(SF.pprev.begin(), SF.pprev.begin() + MQ.NP); S = &SQ;
            const bool dlEnabled = HcEnv("DELTA") != nullptr || emaOn;   // diagnostic only: the instantaneous offset is wrong in long quiet phases (R260)
            if (dlEnabled)
            {
              std::vector<double> d(MQ.NU, 0.0);
              if (emaOn) for (auto& pr : dlMap) d[pr.first] = std::fabs(dlAvgT[pr.first]) > 1e-13 ? dlAvg[pr.first] / dlAvgT[pr.first] : 0.0;
              SQ.SetDelta(d);
            }
          }
          full = wantFull;
        }
      }
      if (MF.fine.In(t_) && S == &SF) { hcur = MF.dt / (double)(1 << (MF.LEV - 1)); hold = 0; }
      const double gnext = (std::floor(t_ / dt + 1e-6) + 1) * dt; int mlev = 0;
      for (int ml = 0; ml < MF.LEV; ++ml) { const double hl = dt / (1 << ml); const double kk = t_ / hl; if (hl <= hcur * 1.0000001 && t_ + hl <= gnext + 1e-12 && std::fabs(kk - std::round(kk)) < 1e-6) { mlev = ml; break; } }
      const double h = dt / (1 << mlev); const long n0 = S->nsub;
      S->Advance(t_, h, mlev); t_ += h;
      if (emaOn && full && mlev == 0) { const double al = std::min(1.0, dt / emaTau); const double* tv = MQ.FTU.at((int)std::llround(t_ / dt)); for (auto& pr : dlMap) { const double x = SF.fS[pr.second] - tv[pr.first]; dlAvg[pr.first] += al * (x - dlAvg[pr.first]); dlAvgT[pr.first] += al * (tv[pr.first] - dlAvgT[pr.first]); } }
      if (S->nsub > n0) { hcur = MF.dt / (double)(1 << (MF.LEV - 1)); hold = 0; } else { ++hold; if (hold >= HOLD && h >= hcur * 0.999) { hcur = std::min(hcur * 2, dt); hold = 0; } }
    }
  }
};

// Event window rule of the prototype: samples of the 5 us grid with a comparator event of the noise-free table within [e - mbMs, e + maMs] run on the full set, all others on
// the mini set; quiet runs shorter than gapMs between two full-set runs stay on the full set (the switch to the mini set leaves a state error, R263).
inline std::vector<char> BuildQuiet(const Red& MF, double mbMs, double maMs, double gapMs)
{
  const double mb = mbMs * 1e-3, ma = maMs * 1e-3; std::vector<char> quiet(MF.n + 1, 1);
  for (double e : MF.evs) for (int k = 0; k <= MF.n; ++k) { const double t = k * MF.dt; if (t > e - mb && t < e + ma) quiet[k] = 0; }
  const int gapN = (int)std::llround(gapMs * 1e-3 / MF.dt);
  if (gapN > 0)
  {
    const int nq = (int)quiet.size(); int k = 0; bool seenFull = false;
    while (k < nq) { if (quiet[k]) { int k2 = k; while (k2 < nq && quiet[k2]) ++k2; if (seenFull && k2 < nq && k2 - k < gapN) for (int q = k; q < k2; ++q) quiet[q] = 0; k = k2; } else { seenFull = true; ++k; } }
  }
  return quiet;
}

// ---- one hit stream of an anchor: tables from the package, handover cache per noise seed (the state at 40 ms, ~0.2 MB), synchronous Pull.
// Never call Load / Prepare / the first Begin of a seed on the audio thread (about 0.2 s of compute each); Begin of a cached seed and Pull are cheap per block.
template <class Mdl>
class HitStream
{
public:
  using Rend = Renderer<Mdl>;
  static constexpr double kEngineYtf = 0.01, kEngineDpt = 0.1, kCapT = 40e-3;
  bool Load(const HcPackage& pkg)
  {
    try
    {
      const HcPackage full = pkg.View("F/"), mini = pkg.View("Q/");
      MF_ = std::make_unique<Red>(); MQ_ = std::make_unique<Red>(); MF_->Load(full); MQ_->Load(mini);
      auto num = [&](const char* k, double d) { auto it = pkg.meta.find(k); return it == pkg.meta.end() ? d : std::atof(it->second.c_str()); };
      quiet_ = BuildQuiet(*MF_, num("mb_ms", 3.0), num("ma_ms", 8.0), num("gapmin_ms", 3.0));
      R_ = std::make_unique<Rend>(*MF_, *MQ_, quiet_, kEngineYtf, kEngineDpt); R_->pkgHash = pkg.hash;
      cache_.clear(); loaded_ = true; return true;
    }
    catch (const std::exception&) { loaded_ = false; return false; }
  }
  bool Loaded() const { return loaded_; }
  // Builds the handover entry of a seed (engine phase 0-1 ms and the reduced model up to 40 ms); afterwards Begin(seed) starts from the entry.
  bool Prepare(uint64_t seed)
  {
    if (!loaded_) return false;
    typename Rend::Handover& h = cache_[(int)seed]; if (!h.key.empty()) return true;
    R_->SetHostRate(48000.0); R_->cap2 = &h; R_->capT = kCapT; R_->Begin((int)seed); R_->Render(kCapT + 0.002); R_->cap2 = nullptr; return !h.key.empty();
  }
  bool Begin(uint64_t seed, double fsHz, double seconds)
  {
    if (!loaded_) return false;
    const int sd = (int)seed; fs_ = fsHz; R_->SetHostRate(fsHz); rd_ = 0; produced_ = 0; want_ = (long)(seconds * fsHz); target_ = 0.0;
    auto it = cache_.find(sd);
    if (it != cache_.end() && !it->second.key.empty() && R_->BeginCachedAt(sd, it->second)) { target_ = kCapT; return true; }
    typename Rend::Handover& h = cache_[sd]; h = typename Rend::Handover(); R_->cap2 = &h; R_->capT = kCapT; R_->Begin(sd); return true;   // cap2 fills the entry when the stream passes 40 ms
  }
  int Pull(float* out, int n)
  {
    if (!loaded_ || n <= 0) return 0;
    while ((long)R_->host.size() - rd_ < n && !R_->Done()) { target_ += std::max(1.0e-3, (n - ((long)R_->host.size() - rd_)) / fs_); R_->Render(target_); }
    long avail = (long)R_->host.size() - rd_; if (produced_ + avail > want_) avail = std::max(0L, want_ - produced_);
    const int w = (int)std::min<long>(n, avail); if (w > 0) std::memcpy(out, R_->host.data() + rd_, (size_t)w * sizeof(float));
    rd_ += w; produced_ += w; return w;
  }
  bool Finished() const { return !loaded_ || produced_ >= want_ || (R_->Done() && rd_ >= (long)R_->host.size()); }

private:
  std::unique_ptr<Red> MF_, MQ_; std::vector<char> quiet_; std::unique_ptr<Rend> R_; std::map<int, typename Rend::Handover> cache_;
  bool loaded_ = false; long rd_ = 0, produced_ = 0, want_ = 0; double target_ = 0.0, fs_ = 48000.0;
};

#undef PR
}}  // namespace dgg::hch
