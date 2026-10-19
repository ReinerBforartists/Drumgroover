// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Reiner Prokein
#pragma once
// Fast model of the TR-808 / Yocto toms and congas (LT/MT/HT sheet, Tom or Conga switch position): one hit for any tuning, level and accent, synthesised sample by sample.
//
// The model is derived from the LT/MT/HT sheet of the NGSpice circuit:
//  - the linearised network is reduced with a fixed Petrov-Galerkin basis (right and left eigenvectors at the corners of the tuning x level range); A(tuning) = A0 + A1/t + A2/(1-t)
//    is exact (the tuning pot is a two-resistor stamp), the level pot is one conductance stamp; per knob state the reduced matrix is decomposed (EigReal.h) into modes;
//  - the nonlinear parts are the two Ge diodes of the output stage (solved per sample with Newton) and the trigger stage: its injection (the residual of the nonlinear
//    devices in the first 2.5 ms, independent of tuning and level) is stored per accent at a 6.144 MHz non-uniform raster, the residual of the rest state (Q52 pulse etc.) at 6 kHz,
//    both as piecewise-linear knots (808TomTables.h, generated). The accent is interpolated linearly between the table nodes.
// The ring is stepped at 48 kHz (the rate the tables are made for); the voice resamples to the host rate.
//
// Setup(circuit, tuning, level) costs a few 100 us (once per knob change), Start(accent) about 0.4 ms (trigger stage), Step() one 48 kHz sample.

#include "808TomTables.h"
#include "808TomAccentTables.h"
#include "EigReal.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

namespace dgg {
namespace tomfast {


class TR808TomFast
{
public:
  static constexpr double kFs = 48000.0, kFineFs = 6144000.0;

  // circuit = sheet * 2 + (conga ? 1 : 0). Returns false if the model cannot be built.
  bool Setup(int circuit, double tuning, double level)
  {
    circuit = std::clamp(circuit, 0, 5); tuning = std::clamp(tuning, 0.002, 0.998);   // the engine floors the pot resistances (500 ohm * t) at 1 ohm level = std::clamp(level, 0.0, 1.0);
    if (mReady && circuit == mCircuit && tuning == mTuning && level == mLevel) return true;
    mReady = false; mCircuit = circuit; mTuning = tuning; mLevel = level; mM = &tab::kModels[circuit];
    if (mM->np != 2) return false;
    if (!BuildModes(tuning, level)) return false;
    mReady = true; return true;
  }

  // Starts a hit from the rest state. False: silent (accent below the first node).
  bool Start(double accentVolt)
  {
    mActive = false;
    if (!mReady || !(accentVolt >= tab::kAccent[0])) return false;
    const ModelData& M = *mM;
    int ia = 0; double wa = 0.0;
    if (accentVolt >= tab::kAccent[tab::kNumAccent - 1]) ia = tab::kNumAccent - 1;
    else { int j = 1; while (accentVolt > tab::kAccent[j]) ++j; ia = j - 1; wa = (accentVolt - tab::kAccent[j - 1]) / (tab::kAccent[j] - tab::kAccent[j - 1]); }
    ExpandTable(M.fineOff, M.fineRow, M.fineVal, M.nAct, M.nFine, mFineT.data(), ia, wa, mFineTab);
    ExpandTable(M.audOff, M.audRow, M.audVal, M.nActA, M.nAud, nullptr, ia, wa, mAudTab);
    if (tab::kModelsX[mCircuit].nActA != M.nActA)   // the rest-residual set has extra columns (only excited above 4.09 V): zero here
    {
      const int nx = tab::kModelsX[mCircuit].nActA; Vec wide((size_t)M.nAud * nx, 0.0);
      for (int r = 0; r < M.nAud; ++r) for (int c = 0; c < M.nActA; ++c) wide[(size_t)r * nx + c] = mAudTab[(size_t)r * M.nActA + c];
      mAudTab.swap(wide);
    }
    RunTriggerStage();
    // ring stage state
    mS = 0; mSA = 0; mSB = 0; FetchAudio(0, mDA); mDB = mDA; mUPrev = mDA.br; mActive = true;
    mPreIdx = 0;                                   // sample 0 is the rest level, samples 1..nPre the trigger stage, then the ring
    return true;
  }

  // Starts a hit at an accent 0..1 of the 808 trigger range: 0 = the full MIDI-velocity accent (4.09 V, exactly Start(4.09)), 1 = 14 V (accent voltage 4.09 + accent * 9.91 V). Above 4.09 V the
  // trigger-stage and rest residuals come from the nodes of 808TomAccentTables.h (node 0 = the last node of 808TomTables.h), blended linearly in the accent voltage like the velocity nodes.
  bool StartAccent(double accent01)
  {
    if (!(accent01 > 0.0)) return Start(tab::kAccent[tab::kNumAccent - 1]);
    mActive = false;
    if (!mReady) return false;
    accent01 = std::fmin(accent01, 1.0);
    const ModelData& M = *mM; const tab::ModelDataX& X = tab::kModelsX[mCircuit];
    const double u = accent01 * (tab::kNumAccentX - 1); int ia = (int)u; if (ia >= tab::kNumAccentX - 1) ia = tab::kNumAccentX - 2; const double wa = u - ia;
    ExpandTable(X.fineOff, X.fineRow, X.fineVal, M.nAct, M.nFine, mFineT.data(), ia, wa, mFineTab, tab::kNumAccentX);
    ExpandTable(X.audOff, X.audRow, X.audVal, X.nActA, M.nAud, nullptr, ia, wa, mAudTab, tab::kNumAccentX);
    RunTriggerStage();
    mS = 0; mSA = 0; mSB = 0; FetchAudio(0, mDA); mDB = mDA; mUPrev = mDA.br; mActive = true;
    mPreIdx = 0;
    return true;
  }

  // next output sample in volts relative to the rest level
  double Step()
  {
    if (!mActive) return 0.0;
    if (mPreIdx <= (int)mPreOut.size()) { const int j = mPreIdx++; return j == 0 ? 0.0 : mPreOut[j - 1]; }
    ++mS;
    if (mS > mSB) { mDA = mDB; mSA = mSB; mSB = mSA + tab::kDecA; FetchAudio(mSB, mDB); }
    const double fr = double(mS - mSA) / double(mSB - mSA);
    LerpDrv(mDA, mDB, fr, mDCur);
    CoreStep(mAud, 0, mDCur);
    return OutputOf(mAud, mDCur);
  }

  bool Active() const { return mActive; }

private:
  using ModelData = tab::ModelData;
  using cplx = std::complex<double>;
  using Vec = std::vector<double>;
  struct Cx { double re = 0.0, im = 0.0; };
  struct Drv { std::vector<Cx> br; double cr[2] = {0, 0}; double yr = 0.0; };
  struct Port { int a, b; double vb, IS, vte, bv; int hasbv; double v0, i0; };
  struct ColSet { int n = 0; std::vector<Cx> TK; Vec TC, TY; };   // table columns -> modal drive (K x n), static port voltage (2 x n), static output (n)

  static constexpr double kPi = 3.14159265358979323846, kE = 2.71828182845904523536;
  static constexpr int kMaxExp = tab::kMaxFineExp;
  // One modal decomposition of the reduced network: the trigger stage steps ALL stable modes (its first 100 us excite modes up to MHz, they shape the attack), the ring stage only
  // the modes below 3 kHz (the faster ones are static there and enter through the static correction).
  struct ModeSet
  {
    int K = 0; std::vector<int> keep;
    std::vector<Cx> Bm, PhiM, OvM, ED, PH;       // Bm: K x 2, PhiM: 2 x K; ED/PH: exact step factors (trigger stage: the 2^e fine steps, ring stage: the 48 kHz step)
    double Psi[4] = {}, Og[2] = {}, Jl[(kMaxExp + 1) * 4] = {};
    ColSet cs;
  };

  // ---------------- small dense linear algebra ----------------
  static bool InvertReal(int n, const Vec& A, Vec& Ainv)
  {
    Vec M(A); Ainv.assign((size_t)n * n, 0.0); for (int i = 0; i < n; ++i) Ainv[(size_t)i * n + i] = 1.0;
    for (int c = 0; c < n; ++c)
    {
      int p = c; for (int r = c + 1; r < n; ++r) if (std::fabs(M[(size_t)r * n + c]) > std::fabs(M[(size_t)p * n + c])) p = r;
      if (M[(size_t)p * n + c] == 0.0) return false;
      if (p != c) for (int j = 0; j < n; ++j) { std::swap(M[(size_t)c * n + j], M[(size_t)p * n + j]); std::swap(Ainv[(size_t)c * n + j], Ainv[(size_t)p * n + j]); }
      const double d = 1.0 / M[(size_t)c * n + c]; for (int j = 0; j < n; ++j) { M[(size_t)c * n + j] *= d; Ainv[(size_t)c * n + j] *= d; }
      for (int r = 0; r < n; ++r) if (r != c) { const double f = M[(size_t)r * n + c]; if (f != 0.0) for (int j = 0; j < n; ++j) { M[(size_t)r * n + j] -= f * M[(size_t)c * n + j]; Ainv[(size_t)r * n + j] -= f * Ainv[(size_t)c * n + j]; } }
    }
    return true;
  }
  static bool InvertCplx(int n, const std::vector<cplx>& A, std::vector<cplx>& Ainv)
  {
    std::vector<cplx> M(A); Ainv.assign((size_t)n * n, cplx(0, 0)); for (int i = 0; i < n; ++i) Ainv[(size_t)i * n + i] = 1.0;
    for (int c = 0; c < n; ++c)
    {
      int p = c; for (int r = c + 1; r < n; ++r) if (std::abs(M[(size_t)r * n + c]) > std::abs(M[(size_t)p * n + c])) p = r;
      if (std::abs(M[(size_t)p * n + c]) == 0.0) return false;
      if (p != c) for (int j = 0; j < n; ++j) { std::swap(M[(size_t)c * n + j], M[(size_t)p * n + j]); std::swap(Ainv[(size_t)c * n + j], Ainv[(size_t)p * n + j]); }
      const cplx d = 1.0 / M[(size_t)c * n + c]; for (int j = 0; j < n; ++j) { M[(size_t)c * n + j] *= d; Ainv[(size_t)c * n + j] *= d; }
      for (int r = 0; r < n; ++r) if (r != c) { const cplx f = M[(size_t)r * n + c]; if (f != cplx(0, 0)) for (int j = 0; j < n; ++j) { M[(size_t)r * n + j] -= f * M[(size_t)c * n + j]; Ainv[(size_t)r * n + j] -= f * Ainv[(size_t)c * n + j]; } }
    }
    return true;
  }
  static void MatMul(int a, int b, int c, const Vec& A, const Vec& B, Vec& C)   // C(a x c) = A(a x b) B(b x c)
  {
    C.assign((size_t)a * c, 0.0);
    for (int i = 0; i < a; ++i) for (int k = 0; k < b; ++k) { const double x = A[(size_t)i * b + k]; if (x == 0.0) continue; for (int j = 0; j < c; ++j) C[(size_t)i * c + j] += x * B[(size_t)k * c + j]; }
  }

  // Ge diode junction (the engine's DiodeEval characteristic)
  static void DiodeIG(const Port& p, double v, double& i, double& g)
  {
    if (v >= -3.0 * p.vte) { const double e = std::exp(std::min(v / p.vte, 700.0)); i = p.IS * (e - 1.0); g = p.IS * e / p.vte; }
    else if (!p.hasbv || v >= -p.bv) { const double arg = 3.0 * p.vte / (v * kE), a3 = arg * arg * arg; i = -p.IS * (1.0 + a3); g = p.IS * 3.0 * a3 / v; }
    else { const double e = std::exp(-(p.bv + v) / p.vte); i = -p.IS * e; g = p.IS * e / p.vte; }
  }

  // level knob -> conductance of the level pot stamp (piecewise-linear f(level), g = 1/(Rl f))
  double GLevel(double level) const
  {
    const int nk = tab::kNumLevelKnots; const double* k = tab::kLevelKnots; double f = 0.0;
    if (level <= k[0]) f = k[1]; else if (level >= k[(nk - 1) * 2]) f = k[(nk - 1) * 2 + 1];
    else for (int i = 1; i < nk; ++i) if (level <= k[i * 2]) { f = k[(i - 1) * 2 + 1] + (level - k[(i - 1) * 2]) / (k[i * 2] - k[(i - 1) * 2]) * (k[i * 2 + 1] - k[(i - 1) * 2 + 1]); break; }
    return 1.0 / (mM->Rl * std::max(f, 1e-9));
  }

  // ---------------- per knob state: modal form ----------------
  bool BuildModes(double tuning, double level)
  {
    const ModelData& M = *mM; const int N = M.N, m = M.m, np = 2; const size_t NN = (size_t)N * N;
    Vec A(NN, 0.0);
    for (int i = 0; i < M.nA; ++i) A[M.aIdx[i]] = M.a0[i] + M.a1[i] / tuning + M.a2[i] / (1.0 - tuning);
    const double dg = GLevel(level) - GLevel(0.5); const int i0 = M.iL0, i1 = M.iL1;
    A[(size_t)i0 * N + i0] += dg; A[(size_t)i1 * N + i1] += dg; A[(size_t)i0 * N + i1] -= dg; A[(size_t)i1 * N + i0] -= dg;
    mPorts.resize(np);
    for (int r = 0; r < np; ++r)
    {
      const double* p = &M.ports[(size_t)r * 7]; Port& q = mPorts[r]; q.a = (int)p[0]; q.b = (int)p[1]; q.vb = p[2]; q.IS = p[3]; q.vte = p[4]; q.bv = p[5]; q.hasbv = (int)p[6];
      q.v0 = M.rest[q.a] - (q.b >= 0 ? M.rest[q.b] : q.vb); double g; DiodeIG(q, q.v0, q.i0, g);
    }
    const Vec Vv(M.V, M.V + (size_t)N * m), Ww(M.W, M.W + (size_t)N * m);
    Vec Wt((size_t)m * N); for (int i = 0; i < N; ++i) for (int j = 0; j < m; ++j) Wt[(size_t)j * N + i] = Ww[(size_t)i * m + j];
    // Er = W^T E V, E sparse
    Vec EV((size_t)N * m, 0.0), Er, Eri;
    for (int e = 0; e < M.nE; ++e) { const int i = M.eIdx[e] / N, j = M.eIdx[e] % N; for (int c = 0; c < m; ++c) EV[(size_t)i * m + c] += M.eVal[e] * Vv[(size_t)j * m + c]; }
    MatMul(m, N, m, Wt, EV, Er); if (!InvertReal(m, Er, Eri)) return false;
    Vec AV, Ar, Aq; MatMul(N, N, m, A, Vv, AV); MatMul(m, N, m, Wt, AV, Ar); MatMul(m, m, m, Eri, Ar, Aq); for (auto& x : Aq) x = -x;
    Vec wr(m), wi(m), Vr((size_t)m * m);
    if (!eig::EigReal(m, Aq.data(), wr.data(), wi.data(), Vr.data())) return false;
    std::vector<cplx> VrC((size_t)m * m), WrC;
    for (int k = 0; k < m; ++k)
      for (int i = 0; i < m; ++i)
      {
        if (wi[k] == 0) VrC[(size_t)i * m + k] = Vr[(size_t)i * m + k];
        else if (wi[k] > 0) VrC[(size_t)i * m + k] = cplx(Vr[(size_t)i * m + k], Vr[(size_t)i * m + k + 1]);
        else VrC[(size_t)i * m + k] = cplx(Vr[(size_t)i * m + k - 1], -Vr[(size_t)i * m + k]);
      }
    if (!InvertCplx(m, VrC, WrC)) return false;
    Vec Ainv, Arinv, Rg, ArEr;
    if (!InvertReal(N, A, Ainv) || !InvertReal(m, Ar, Arinv)) return false;
    { Vec ArinvWt, tmp; MatMul(m, m, N, Arinv, Wt, ArinvWt); MatMul(N, m, N, Vv, ArinvWt, tmp); Rg.resize(NN); for (size_t i = 0; i < NN; ++i) Rg[i] = Ainv[i] - tmp[i]; }
    MatMul(m, m, m, Arinv, Er, ArEr);
    Vec WtP((size_t)m * np, 0.0), Bq;
    for (int r = 0; r < np; ++r) for (int j = 0; j < m; ++j) WtP[(size_t)j * np + r] = Wt[(size_t)j * N + mPorts[r].a] - (mPorts[r].b >= 0 ? Wt[(size_t)j * N + mPorts[r].b] : 0.0);
    MatMul(m, m, np, Eri, WtP, Bq); for (auto& x : Bq) x = -x;
    Vec PhiF((size_t)np * m); for (int r = 0; r < np; ++r) for (int j = 0; j < m; ++j) PhiF[(size_t)r * m + j] = Vv[(size_t)mPorts[r].a * m + j] - (mPorts[r].b >= 0 ? Vv[(size_t)mPorts[r].b * m + j] : 0.0);
    // wmax: modes with |lambda| below it are stepped, the others are static; cols: the residual-table columns this stage is driven by
    auto makeSet = [&](ModeSet& S, double wmax, const int* cols, int nc, bool fine) {
      Vec fac; S.keep.clear();
      for (int k = 0; k < m; ++k)
      {
        const double ab = std::hypot(wr[k], wi[k]);
        if (ab < wmax && wr[k] < 0 && wi[k] >= 0) { S.keep.push_back(k); fac.push_back(wi[k] == 0 ? 1.0 : 2.0); }
      }
      const int K = (int)S.keep.size(); S.K = K; const std::vector<int>& keep = S.keep;
      Vec Pi((size_t)m * m, 0.0);
      for (int q = 0; q < K; ++q) { const int k = keep[q]; for (int i = 0; i < m; ++i) for (int j = 0; j < m; ++j) Pi[(size_t)i * m + j] += fac[q] * (VrC[(size_t)i * m + k] * WrC[(size_t)k * m + j]).real(); }
      Vec IminusPi((size_t)m * m); for (int i = 0; i < m; ++i) for (int j = 0; j < m; ++j) IminusPi[(size_t)i * m + j] = (i == j ? 1.0 : 0.0) - Pi[(size_t)i * m + j];
      Vec Sq; MatMul(m, m, m, IminusPi, ArEr, Sq);
      S.Bm.assign((size_t)K * np, Cx{}); S.PhiM.assign((size_t)np * K, Cx{}); S.OvM.assign(K, Cx{});
      std::vector<cplx> lam(K);
      for (int q = 0; q < K; ++q)
      {
        const int k = keep[q]; lam[q] = cplx(wr[k], wi[k]);
        for (int r = 0; r < np; ++r) { cplx t = 0; for (int j = 0; j < m; ++j) t += WrC[(size_t)k * m + j] * Bq[(size_t)j * np + r]; S.Bm[(size_t)q * np + r] = {t.real(), t.imag()}; }
        for (int r = 0; r < np; ++r) { cplx t = 0; for (int j = 0; j < m; ++j) t += PhiF[(size_t)r * m + j] * VrC[(size_t)j * m + k]; t *= fac[q]; S.PhiM[(size_t)r * K + q] = {t.real(), t.imag()}; }
        { cplx t = 0; for (int j = 0; j < m; ++j) t += Vv[(size_t)M.out * m + j] * VrC[(size_t)j * m + k]; t *= fac[q]; S.OvM[q] = {t.real(), t.imag()}; }
      }
      Vec SqBq, XsA, XsU; MatMul(m, m, np, Sq, Bq, SqBq); MatMul(N, m, np, Vv, SqBq, XsA); MatMul(N, m, m, Vv, Sq, XsU);
      Vec Xs = XsA; for (int r = 0; r < np; ++r) for (int i = 0; i < N; ++i) Xs[(size_t)i * np + r] -= Rg[(size_t)i * N + mPorts[r].a] - (mPorts[r].b >= 0 ? Rg[(size_t)i * N + mPorts[r].b] : 0.0);
      for (int r = 0; r < np; ++r)
      {
        for (int c = 0; c < np; ++c) S.Psi[r * np + c] = Xs[(size_t)mPorts[r].a * np + c] - (mPorts[r].b >= 0 ? Xs[(size_t)mPorts[r].b * np + c] : 0.0);
        S.Og[r] = Xs[(size_t)M.out * np + r];
      }
      ColSet& cs = S.cs; cs.n = nc; cs.TK.assign((size_t)K * nc, Cx{}); cs.TC.assign((size_t)np * nc, 0.0); cs.TY.assign(nc, 0.0);
      Vec WtC((size_t)m * nc), UrMap; for (int c = 0; c < nc; ++c) for (int j = 0; j < m; ++j) WtC[(size_t)j * nc + c] = Wt[(size_t)j * N + cols[c]];
      MatMul(m, m, nc, Eri, WtC, UrMap); for (auto& x : UrMap) x = -x;
      for (int q = 0; q < K; ++q) for (int c = 0; c < nc; ++c) { cplx t = 0; for (int j = 0; j < m; ++j) t += WrC[(size_t)keep[q] * m + j] * UrMap[(size_t)j * nc + c]; cs.TK[(size_t)q * nc + c] = {t.real(), t.imag()}; }
      Vec Xr; MatMul(N, m, nc, XsU, UrMap, Xr); for (int c = 0; c < nc; ++c) for (int i = 0; i < N; ++i) Xr[(size_t)i * nc + c] -= Rg[(size_t)i * N + cols[c]];
      for (int r = 0; r < np; ++r) for (int c = 0; c < nc; ++c) cs.TC[(size_t)r * nc + c] = Xr[(size_t)mPorts[r].a * nc + c] - (mPorts[r].b >= 0 ? Xr[(size_t)mPorts[r].b * nc + c] : 0.0);
      for (int c = 0; c < nc; ++c) cs.TY[c] = Xr[(size_t)M.out * nc + c];
      // exact step factors and the linear port coupling of one step
      const int nE = fine ? kMaxExp + 1 : 1; S.ED.assign((size_t)nE * K, Cx{}); S.PH.assign((size_t)nE * K, Cx{});
      for (int e = 0; e < nE; ++e)
      {
        const double h = fine ? (double)(1 << e) / kFineFs : 1.0 / kFs; Cx* ed = &S.ED[(size_t)e * K]; Cx* ph = &S.PH[(size_t)e * K];
        for (int q = 0; q < K; ++q)
        {
          const cplx edq = std::exp(lam[q] * h), phq = 0.5 * ((std::abs(lam[q]) > 1e-12) ? (edq - 1.0) / lam[q] : cplx(h, 0));
          ed[q] = {edq.real(), edq.imag()}; ph[q] = {phq.real(), phq.imag()};
        }
        for (int p = 0; p < np; ++p) for (int c = 0; c < np; ++c)
        {
          double dv = S.Psi[p * np + c];
          for (int q = 0; q < K; ++q) dv += CxMulRe(S.PhiM[(size_t)p * K + q], ph[q], S.Bm[(size_t)q * np + c]);
          S.Jl[e * 4 + p * 2 + c] = dv;
        }
      }
    };
    makeSet(mFine, 1e30, M.act, M.nAct, true);
    makeSet(mAud, 2.0 * kPi * 3000.0, tab::kModelsX[mCircuit].actA, tab::kModelsX[mCircuit].nActA, false);
    // the slow modes are a subset of all modes: where the state of each goes over from the trigger stage
    mHand.resize(mAud.K); for (int q = 0; q < mAud.K; ++q) { mHand[q] = 0; for (int f = 0; f < mFine.K; ++f) if (mFine.keep[f] == mAud.keep[q]) mHand[q] = f; }
    // time axis of the trigger-stage rows (fine samples at the end of each step)
    mFineT.resize(M.nFine); { double t = 0; for (int s = 0; s < M.nFine; ++s) { t += (double)(1 << M.fExp[s]); mFineT[s] = t; } }
    return true;
  }

  static double CxMulRe(Cx a, Cx b, Cx c) { const double r = a.re * b.re - a.im * b.im, i = a.re * b.im + a.im * b.re; return r * c.re - i * c.im; }

  // knots -> blended full table (rows x ncols): (1-wa) * node ia + wa * node ia+1; time[] = time axis of the rows (nullptr: the row index)
  static void ExpandTable(const uint32_t* off, const uint16_t* krow, const float* kval, int ncols, int nrows, const double* time, int ia, double wa, Vec& tab, int numNodes = tab::kNumAccent)
  {
    tab.assign((size_t)nrows * ncols, 0.0);
    for (int pass = 0; pass < 2; ++pass)
    {
      const double w = pass == 0 ? 1.0 - wa : wa; if (w == 0.0) continue;
      const int node = ia + pass; if (node >= numNodes) continue;
      for (int c = 0; c < ncols; ++c)
      {
        const uint32_t k0 = off[(size_t)node * ncols + c], k1 = off[(size_t)node * ncols + c + 1];
        for (uint32_t k = k0; k + 1 < k1; ++k)
        {
          const int r0 = krow[k], r1 = krow[k + 1]; const double v0 = kval[k], v1 = kval[k + 1];
          const double t0 = time ? time[r0] : (double)r0, t1 = time ? time[r1] : (double)r1;
          for (int r = r0 + (k > k0 ? 1 : 0); r <= r1; ++r) { const double t = time ? time[r] : (double)r; tab[(size_t)r * ncols + c] += w * (v0 + (v1 - v0) * (t - t0) / (t1 - t0)); }
        }
      }
    }
  }

  // modal drive, static port voltage and static output of one table row
  static void Derive(const ModeSet& S, const double* rr, Drv& d)
  {
    const ColSet& cs = S.cs; const int nc = cs.n, K = S.K; d.br.resize(K);
    for (int q = 0; q < K; ++q) { double sr = 0, si = 0; const Cx* t = &cs.TK[(size_t)q * nc]; for (int c = 0; c < nc; ++c) { sr += t[c].re * rr[c]; si += t[c].im * rr[c]; } d.br[q] = {sr, si}; }
    for (int p = 0; p < 2; ++p) { double v = 0; for (int c = 0; c < nc; ++c) v += cs.TC[(size_t)p * nc + c] * rr[c]; d.cr[p] = v; }
    double v = 0; for (int c = 0; c < nc; ++c) v += cs.TY[c] * rr[c]; d.yr = v;
  }
  static void LerpDrv(const Drv& a, const Drv& b, double fr, Drv& out)
  {
    const int K = (int)a.br.size(); out.br.resize(K);
    for (int q = 0; q < K; ++q) out.br[q] = {a.br[q].re + (b.br[q].re - a.br[q].re) * fr, a.br[q].im + (b.br[q].im - a.br[q].im) * fr};
    for (int p = 0; p < 2; ++p) out.cr[p] = a.cr[p] + (b.cr[p] - a.cr[p]) * fr;
    out.yr = a.yr + (b.yr - a.yr) * fr;
  }
  void FetchAudio(int row, Drv& d) const
  {
    const int r = std::min(row / tab::kDecA, mM->nAud - 1);
    Derive(mAud, &mAudTab[(size_t)r * mAud.cs.n], d);
  }

  // output voltage (relative to the rest level) at the current state; d = static part of the table row
  double OutputOf(const ModeSet& S, const Drv& d) const
  {
    double yo = d.yr;
    for (int p = 0; p < 2; ++p) yo += S.Og[p] * mG[p];
    for (int q = 0; q < S.K; ++q) yo += S.OvM[q].re * mEta[q].re - S.OvM[q].im * mEta[q].im;
    return yo;
  }

  // One step: modal update with the Newton solution of the two diode currents (mG holds the currents of the previous step). dCur = drive of this step.
  void CoreStep(const ModeSet& S, int e, const Drv& dCur)
  {
    const int K = S.K; mEtaBase.resize(K); const Cx* ed = &S.ED[(size_t)e * K]; const Cx* ph = &S.PH[(size_t)e * K]; const double* Jl = &S.Jl[e * 4];
    for (int q = 0; q < K; ++q)
    {
      double br = dCur.br[q].re + mUPrev[q].re, bi = dCur.br[q].im + mUPrev[q].im;
      for (int p = 0; p < 2; ++p) { const Cx b = S.Bm[(size_t)q * 2 + p]; br += b.re * mG[p]; bi += b.im * mG[p]; }
      const Cx x = mEta[q], d = ed[q], f = ph[q];
      mEtaBase[q] = {d.re * x.re - d.im * x.im + f.re * br - f.im * bi, d.re * x.im + d.im * x.re + f.re * bi + f.im * br};
    }
    double vB[2];
    for (int p = 0; p < 2; ++p) { double vp = mPorts[p].v0 + dCur.cr[p]; for (int q = 0; q < K; ++q) vp += S.PhiM[(size_t)p * K + q].re * mEtaBase[q].re - S.PhiM[(size_t)p * K + q].im * mEtaBase[q].im; vB[p] = vp; }
    double gg[2] = {mG[0], mG[1]};
    for (int it = 0; it < 40; ++it)
    {
      double res[2], J[4], worst = 0, gmax = 0, v[2];
      for (int p = 0; p < 2; ++p) { v[p] = vB[p]; for (int c = 0; c < 2; ++c) v[p] += Jl[p * 2 + c] * gg[c]; }
      for (int p = 0; p < 2; ++p) { double i, gd; DiodeIG(mPorts[p], v[p], i, gd); res[p] = gg[p] - (i - mPorts[p].i0); worst = std::max(worst, std::fabs(res[p])); gmax = std::max(gmax, std::fabs(gg[p])); for (int c = 0; c < 2; ++c) J[p * 2 + c] = gd * Jl[p * 2 + c]; }
      if (worst < 1e-14 + 1e-10 * gmax) break;
      const double a = 1 - J[0], b = -J[1], c = -J[2], d = 1 - J[3], det = a * d - b * c;
      gg[0] += (-res[0] * d + res[1] * b) / det; gg[1] += (-a * res[1] + c * res[0]) / det;
    }
    for (int q = 0; q < K; ++q)
    {
      double br = 0, bi = 0; for (int p = 0; p < 2; ++p) { const Cx b = S.Bm[(size_t)q * 2 + p]; br += b.re * gg[p]; bi += b.im * gg[p]; }
      const Cx f = ph[q]; mEta[q] = {mEtaBase[q].re + f.re * br - f.im * bi, mEtaBase[q].im + f.re * bi + f.im * br};
    }
    mG[0] = gg[0]; mG[1] = gg[1]; mUPrev = dCur.br;
  }

  // the trigger stage (all modes): integrated from the rest state over the non-uniform raster, one table row per step; the output is read at the 48 kHz instants
  void RunTriggerStage()
  {
    const ModelData& M = *mM; Drv zero; zero.br.assign(mFine.K, Cx{}); mUPrev = zero.br; Drv d;
    mEta.assign(mFine.K, Cx{}); mG[0] = mG[1] = 0.0;
    const int nPre = (int)std::lround(M.tdump * kFs); const double ratio = kFineFs / kFs;   // fine samples per 48 kHz sample
    mPreOut.assign(nPre, 0.0); int jn = 1; double tPrev = 0.0, yPrev = 0.0;
    for (int s = 1; s <= M.nFine; ++s)
    {
      Derive(mFine, &mFineTab[(size_t)(s - 1) * mFine.cs.n], d);
      CoreStep(mFine, M.fExp[s - 1], d);
      const double t = mFineT[s - 1], y = OutputOf(mFine, d);
      for (; jn <= nPre && (double)jn * ratio <= t + 1e-9; ++jn) mPreOut[jn - 1] = yPrev + (y - yPrev) * ((double)jn * ratio - tPrev) / (t - tPrev);   // linear between the step ends
      tPrev = t; yPrev = y;
    }
    // hand the slow modes over to the ring stage
    std::vector<Cx> slow(mAud.K); for (int q = 0; q < mAud.K; ++q) slow[q] = mEta[mHand[q]]; mEta = slow;
  }

  const ModelData* mM = nullptr;
  bool mReady = false, mActive = false;
  int mCircuit = -1, mPreIdx = 0, mS = 0, mSA = 0, mSB = 0;
  double mTuning = -1.0, mLevel = -1.0;
  std::vector<Port> mPorts;
  std::vector<Cx> mEta, mEtaBase, mUPrev;
  double mG[2] = {};
  ModeSet mFine, mAud; std::vector<int> mHand;
  Vec mFineT, mFineTab, mAudTab, mPreOut;
  Drv mDA, mDB, mDCur;
};

}  // namespace tomfast
}  // namespace dgg
