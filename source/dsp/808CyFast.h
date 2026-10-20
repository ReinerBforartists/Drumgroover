// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Reiner Prokein
#pragma once
// Cymbal / ride fast model: oscillator bank + BP1 / BP2 as exact modes, and the rest of the circuit as four coupled sub-networks (808CyModel.h, generated):
//   E   trigger stage (Q2, Q6, Q19), EG#1..3, c45node / c40node / q20emit / q19emit        (cymodel::eg; cymodel::eg2 = the same without Q2 / Q6 / Q19 after the trigger pulse)
//   B   cell 2 (Q17, D11) with c39node / eg13node and the U5-B stage                          (cymodel::cellb)
//   C   cell 3 (Q16, D5, gate diode D6) with c38node / c37node and the U5-A stage             (cymodel::cellc)
//   AD  cell 1 (Q18, D12), Band1 buffer Q25, mesh, tone, U6-B, level and the output load     (cymodel::ad)
// Each sub-network is a reduced-Newton network (808HhNet.h) with step halving and a dense fallback; they exchange their interface nodes with one unit delay per Gauss-Seidel pass (3 passes during the first
// 3 ms, where the envelope rises fast, 1 pass afterwards). The trigger edges run in 1/64 steps with the dense solver. One instance is one voice (create it only for voices that synthesise).
#include <algorithm>
#include <cmath>
#include "808HhBank.h"
#include "808HhNet.h"
#include "808CyModel.h"
#include "808CyAdSplit.h"

namespace dgg { namespace cyfast {

using hhfast::DenseNet;
using hhfast::ReducedNet;

// One sub-network with its fixed-node vectors (values at the start / end of the step). Reduced Newton steps (state in level 0); a failed step is repeated as two half steps (kLev - 1 times), the last resort is
// the dense solver.
template <class M, int kLev = 3>
class SubNet
{
 public:
  static constexpr int N = M::kN;
  typename ReducedNet<M>::Snap snap;
  double vf0[16] = {0}, vf1[16] = {0};
  long statFail = 0, statDense = 0;
  template <class Stamp> void Setup(double h, double theta, Stamp stamp)
  {
    mH = h;
    for (int l = 0; l < kLev; ++l) { mLv[l] = ReducedNet<M>(); mLv[l].mTheta = theta; stamp(mLv[l]); mLv[l].Build(h / (double)(1 << l)); }
    mDense = DenseNet<M>(); stamp(mDense);
  }
  double V(int i) const { return mLv[0].V(i); }
  void SetState(const double* v) { mLv[0].SetState(v); }
  // the state and the fixed-node values of another set of the same network (other step size): no extrapolation history, the next step starts from the state
  void CopyStateFrom(const SubNet& o)
  {
    double v[N]; for (int i = 0; i < N; ++i) v[i] = o.mLv[0].V(i);
    mLv[0].SetState(v); for (int i = 0; i < 16; ++i) { vf0[i] = o.vf0[i]; vf1[i] = o.vf1[i]; }
  }
  void Save() { mLv[0].Save(snap); }
  void Restore() { mLv[0].Restore(snap); }
  bool Dc(const double* vf)
  {
    double v[N]; for (int i = 0; i < N; ++i) v[i] = mLv[0].V(i);
    mDense.SetState(v); const bool ok = mDense.Dc(vf);
    for (int i = 0; i < N; ++i) v[i] = mDense.V(i);
    mLv[0].SetState(v); return ok;
  }
  void Step() { StepL(vf0, vf1, 0); }
  // one dense step of length h (trigger edges)
  void DenseStep(const double* a, const double* b, double h)
  {
    ++statDense; double v[N]; for (int i = 0; i < N; ++i) v[i] = mLv[0].V(i);
    mDense.SetState(v); mDense.Step(h, a, b);
    bool fin = true; for (int i = 0; i < N; ++i) { v[i] = mDense.V(i); fin = fin && std::isfinite(v[i]); }
    if (fin) mLv[0].SetState(v);
  }

 private:
  void StepL(const double* a, const double* b, int l)
  {
    ReducedNet<M>& c = mLv[l]; if (l > 0) c.CopyStateFrom(mLv[0]);
    if (c.Step(a, b)) { if (l > 0) mLv[0].CopyStateFrom(c); return; }
    ++statFail;
    if (l + 1 >= kLev) { DenseStep(a, b, mH / (double)(1 << l)); return; }
    double m[16]; for (int i = 0; i < 16; ++i) m[i] = 0.5 * (a[i] + b[i]);
    StepL(a, m, l + 1); StepL(m, b, l + 1);
  }
  double mH = 0.0;
  ReducedNet<M> mLv[kLev]; DenseNet<M> mDense;
};

class CyChain
{
 public:
  using Bank = hhbank::BankT<cymodel::bp>;
  static constexpr double kTheta = 0.51;             // trapezoid rule with a trace of damping (at exactly 0.5 the residual of the algebraic nodes does not decay)
  double mFineUntil = 3e-3;                          // seconds after the trigger with mFinePasses Gauss-Seidel passes (study hooks: public members)
  int mFinePasses = 3, mPasses = 1;                  // passes during / after the fine phase
  static constexpr double kSwitchE = 6e-3;           // from here on the trigger devices Q2 / Q6 / Q19 are taken out (eg2)
  static constexpr double kECoarseSec = 10e-3;       // from here on the envelope generators (E net, time constants >= 10 us) are stepped once per host sample (the set of the quiet tail), the cells keep their core steps
  static constexpr int kEdgeParts = 64;              // dense steps per core step around the trigger edges
  static constexpr double kTrigWidth = 2e-3, kTrigEdge = 1e-6, kTrigAmp = 5.0;   // TRIG_CY pulse: 5 V, 1 us edges, 2 ms wide

  // Core steps per host sample for a sample rate: the core runs at about 192 kHz (step 5.2 .. 5.7 us). The coupling between the sub-networks is explicit (one unit delay per pass) and becomes unstable
  // below a step of about 2.8 us with one pass (Gauss-Seidel on interfaces coupled through capacitors), and the error against the full circuit falls with the square of the step: more steps than this buy nothing.
  static int CoreStepsFor(double sampleRate)
  {
    static const int k[6] = {1, 2, 3, 4, 6, 8}; const double want = 192000.0 / sampleRate; int best = 4;
    for (int i = 0; i < 6; ++i) if (std::fabs(k[i] - want) < std::fabs(best - want)) best = k[i];
    return best;
  }

  // Sample rate, core steps per host sample (1, 2, 3, 4, 6, 8; CoreStepsFor()) and the pots: decay = EG#1 pot Rvr2 (2 MOhm x decay, 0 .. 1),
  // tone = VR4 (0 .. 1), vr6frac = resistance fraction of the level rheostat Rvr6 (100 kOhm). The step matrices are rebuilt (some ms) only when one of them changed.
  void Prepare(double sampleRate, int coreStepsPerSample, double decay, double tone, double vr6frac)
  {
    if (mBuilt && sampleRate == mSr && coreStepsPerSample == mSub && decay == mDecay && tone == mTone && vr6frac == mVr6) return;
    mSr = sampleRate; mSub = coreStepsPerSample; mH = 1.0 / (sampleRate * mSub); mDecay = decay; mTone = tone; mVr6 = vr6frac;
    using P = cymodel::pots;
    const double dgDecay = Dg(P::kRvr2Max, decay, P::kRvr2Dump), dgT1 = Dg(P::kVr4Max, tone, P::kVr4Dump), dgT2 = Dg(P::kVr4Max, 1.0 - tone, P::kVr4Dump), dgL = Dg(P::kVr6Max, vr6frac, P::kVr6Dump);
    mE.Setup(mH, kTheta, [&](auto& n) { n.StampG(cymodel::eg::kRow_vr2wiper, -1, dgDecay); });
    mE2.Setup(mH, kTheta, [&](auto& n) { n.StampG(cymodel::eg2::kRow_vr2wiper, -1, dgDecay); });
    mB.Setup(mH, kTheta, [](auto&) {});
    mC.Setup(mH, kTheta, [](auto&) {});
    mAD.Setup(mH, kTheta, dgT1, dgT2, dgL);
    // the coarse set for the quiet tail: half the core steps per host sample (about 96 kHz), only with the normal 4 or more
    mSubT = mSub >= 4 ? mSub / 2 : 0; mHT = mSubT > 0 ? 1.0 / (sampleRate * mSubT) : 0.0;
    if (mSubT > 0)
    {
      mTE2.Setup(mHT * mSubT, kTheta, [&](auto& n) { n.StampG(cymodel::eg2::kRow_vr2wiper, -1, dgDecay); });
      mTB.Setup(mHT, kTheta, [](auto&) {});
      mTC.Setup(mHT, kTheta, [](auto&) {});
      mTAD.Setup(mHT, kTheta, dgT1, dgT2, dgL);
    }
    mBuilt = true;
  }

  // accentVolt: accent stage input (V, velocity / 127 * 4.09); bankOffset / preroll: see Bank::Reset
  void Trigger(double accentVolt, double pitchRatio, double bankOffset, double preroll)
  {
    mAccent = accentVolt; mHostN = 0; mActive = true; mUseE2 = false; mTail = false; mECoarse = false; mEnv = 0.0; mEnvPeak = 0.0;
    mBank.Reset(pitchRatio, bankOffset, preroll); mBank.Advance(0.0);
    double e[16] = {0}, a[16] = {0}, b[16] = {0}, c[16] = {0};
    e[2] = a[2] = b[2] = c[2] = 15.0; e[3] = a[3] = b[3] = c[3] = -15.0; e[5] = accentVolt; a[8] = mBank.Out(0); b[9] = c[9] = mBank.Out(1);
    mE.SetState(cymodel::eg::kRest); mB.SetState(cymodel::cellb::kRest); mC.SetState(cymodel::cellc::kRest); mAD.SetState(cymodel::ad::kRest);
    for (int it = 0; it < 4; ++it)   // DC state at rest: Gauss-Seidel sweeps over the interfaces
    {
      mE.Dc(e); a[10] = mE.V(cymodel::eg::kRow_c45node); b[10] = mE.V(cymodel::eg::kRow_c40node); b[11] = mE.V(cymodel::eg::kRow_q20emit); c[10] = mE.V(cymodel::eg::kRow_q19emit);
      mB.Dc(b); mC.Dc(c); a[11] = mC.V(cymodel::cellc::kRow_u5aout); a[12] = mB.V(cymodel::cellb::kRow_u5bout); mAD.Dc(a);
      b[12] = mAD.V(cymodel::ad::kRow_n120node); c[11] = mAD.V(cymodel::ad::kRow_n112node);
      e[10] = mAD.V(cymodel::ad::kRow_d12a); e[11] = mB.V(cymodel::cellb::kRow_c39node); e[12] = mB.V(cymodel::cellb::kRow_eg13node); e[13] = mC.V(cymodel::cellc::kRow_c38node);
    }
    for (int i = 0; i < 16; ++i) { mE.vf0[i] = e[i]; mAD.vf0[i] = a[i]; mB.vf0[i] = b[i]; mC.vf0[i] = c[i]; }
    mN120 = b[12]; mN112 = c[11]; mOutRest = mAD.V(cymodel::ad::kRow_u6bout); mCyRest = mAD.V(cymodel::ad::kRow_cy_out);
  }
  bool Active() const { return mActive; }
  // cy_out of the circuit (behind R166 and the bus load) relative to its rest value, valid after Process(); diagnostics (the voice uses the U6-B output u6bout)
  double OutCy() const { return (mTail ? mTAD : mAD).V(cymodel::ad::kRow_cy_out) - mCyRest; }
  long StatFail() const { return mE.statFail + mE2.statFail + mAD.StatFail() + mB.statFail + mC.statFail; }
  long StatDense() const { return mE.statDense + mE2.statDense + mAD.StatDense() + mB.statDense + mC.statDense; }

  // one host sample: the U6-B output u6bout relative to its rest value (V); cy_out behind R166 and the bus load is 2.5 dB lower
  double Process()
  {
    if (!mActive) return 0.0;
    if (!mECoarse && !mTail && mUseE2 && mSubT > 0 && (double)mHostN / mSr > kECoarseSec) { mTE2.CopyStateFrom(mE2); mECoarse = true; }
    const bool tail = mTail;
    const int sub = tail ? mSubT : mSub;
    for (int k = 0; k < sub; ++k) CoreStep(tail, mHostN, k, sub);
    ++mHostN;
    const double y = (tail ? mTAD : mAD).V(cymodel::ad::kRow_u6bout) - mOutRest;
    if (adaptiveTail && mSubT > 0) UpdateTail(y);
    return y;
  }

  // Quiet tail: the error of a core step is a fraction of the signal (about -27 dB at 4 steps per host sample, -17 dB at 2 steps), so with the signal 14 dB below its peak the
  // coarse set (half the steps) stays below the absolute error of the normal set at the peak. The envelope is a 10 ms mean of the output power; the switch back (a rising level)
  // has a hysteresis.
  bool adaptiveTail = true;
  bool InTail() const { return mTail; }

 private:
  void UpdateTail(double y)
  {
    constexpr double kEnterRel = 1.585e-3, kLeaveRel = 1.0e-2;   // power relative to the peak power: -28 dB (-14 dB in amplitude) in, -20 dB out
    constexpr double kEarliest = 0.03;                              // s; the peak is reached within the first 10 ms
    mEnv += (y * y - mEnv) * (1.0 / (0.01 * mSr)); if (mEnv > mEnvPeak) mEnvPeak = mEnv;
    if (!mTail)
    {
      if (mUseE2 && (double)mHostN / mSr > kEarliest && mEnv < mEnvPeak * kEnterRel)
      {
        if (!mECoarse) mTE2.CopyStateFrom(mE2);
        mTB.CopyStateFrom(mB); mTC.CopyStateFrom(mC); mTAD.CopyStateFrom(mAD); mTail = true;
      }
    }
    else if (mEnv > mEnvPeak * kLeaveRel)
    {
      mE2.CopyStateFrom(mTE2); mB.CopyStateFrom(mTB); mC.CopyStateFrom(mTC); mAD.CopyStateFrom(mTAD); mTail = false;
    }
  }

  static double Dg(double rmax, double frac, double dump) { return 1.0 / std::max(rmax * frac, 1.0) - 1.0 / std::max(rmax * dump, 1.0); }
  static double Trig(double t)
  {
    if (t <= 0.0) return 0.0;
    if (t < kTrigEdge) return kTrigAmp * t / kTrigEdge;
    if (t < kTrigWidth + kTrigEdge) return kTrigAmp;
    if (t < kTrigWidth + 2.0 * kTrigEdge) return kTrigAmp * (1.0 - (t - kTrigWidth - kTrigEdge) / kTrigEdge);
    return 0.0;
  }

  // Core step k of sub steps in host sample hostN, on the normal sub-networks or (tail) on the coarse set.
  void CoreStep(bool tail, long hostN, int k, int sub)
  {
    SubNet<cymodel::eg>& nE = mE;   // the trigger stage is only ever used in the normal set (the coarse set starts after kSwitchE)
    const bool eCoarse = tail || mECoarse;   // the E net runs on its host-rate set in the tail and, before it, from kECoarseSec on
    SubNet<cymodel::eg2>& nE2 = eCoarse ? mTE2 : mE2;
    SubNet<cymodel::cellb>& nB = tail ? mTB : mB;
    SubNet<cymodel::cellc>& nC = tail ? mTC : mC;
    AdBlock<SubNet<cymodel::ad1>>& nAD = tail ? mTAD : mAD;
    const double hh = tail ? mHT : mH;
    const double tStart = (double)(hostN * sub + k) * hh, tEnd = (double)(hostN * sub + k + 1) * hh;
    mBank.Advance(tEnd); const double bp1 = mBank.Out(0), bp2 = mBank.Out(1);
    const bool edge = (tEnd > 0.0 && tStart < 6e-6) || (tEnd > kTrigWidth + kTrigEdge && tStart < kTrigWidth + 2.0 * kTrigEdge + 5e-6);
    if (!mUseE2 && tEnd >= kSwitchE)   // same unknowns in eg and eg2
    {
      mUseE2 = true; double v[cymodel::eg::kN]; for (int i = 0; i < cymodel::eg::kN; ++i) v[i] = nE.V(i);
      nE2.SetState(v); for (int i = 0; i < 16; ++i) nE2.vf0[i] = nE.vf0[i];
    }
    const int passes = tEnd < mFineUntil ? mFinePasses : (hh < 3.5e-6 ? std::max(mPasses, 2) : mPasses);   // steps below 3.5 us need two passes to stay stable
    double e0[16];
    if (passes > 1)   // the states are restored only for the further passes
    {
      nE.Save(); nE2.Save(); nAD.Save(); nB.Save(); nC.Save();
      for (int i = 0; i < 16; ++i) e0[i] = mUseE2 ? nE2.vf0[i] : nE.vf0[i];
    }
    double n120 = mN120, n112 = mN112;
    for (int p = 0; p < passes; ++p)
    {
      // E sees the cell nodes of the previous step (pass 0) or of the previous pass
      const double in[4] = {nAD.V(cymodel::ad::kRow_d12a), nB.V(cymodel::cellb::kRow_c39node), nB.V(cymodel::cellb::kRow_eg13node), nC.V(cymodel::cellc::kRow_c38node)};
      if (p > 0)
      {
        if (mUseE2) { nE2.Restore(); for (int i = 0; i < 16; ++i) nE2.vf0[i] = e0[i]; } else { nE.Restore(); for (int i = 0; i < 16; ++i) nE.vf0[i] = e0[i]; }
        nB.Restore(); nC.Restore(); nAD.Restore();
      }
      double e1[16] = {0}; e1[2] = 15.0; e1[3] = -15.0; e1[4] = Trig(tEnd); e1[5] = mAccent; for (int i = 0; i < 4; ++i) e1[10 + i] = in[i];
      double c45, c40, q20, q19;
      if (mUseE2)
      {
        if (!eCoarse || k == 0)   // coarse set: the envelope generators are slow, one step per host sample (nE2 is built for that step), the values are held for the other core steps
        {
          for (int i = 0; i < 16; ++i) nE2.vf1[i] = e1[i];
          nE2.Step(); mEHold[0] = nE2.V(cymodel::eg2::kRow_c45node); mEHold[1] = nE2.V(cymodel::eg2::kRow_c40node); mEHold[2] = nE2.V(cymodel::eg2::kRow_q20emit); mEHold[3] = nE2.V(cymodel::eg2::kRow_q19emit);
        }
        c45 = mEHold[0]; c40 = mEHold[1]; q20 = mEHold[2]; q19 = mEHold[3];
      }
      else
      {
        for (int i = 0; i < 16; ++i) nE.vf1[i] = e1[i];
        if (!edge) nE.Step();
        else
          for (int q = 0; q < kEdgeParts; ++q)
          {
            double x0[16], x1[16]; const double f0 = (double)q / kEdgeParts, f1 = (double)(q + 1) / kEdgeParts;
            for (int i = 0; i < 16; ++i) { x0[i] = nE.vf0[i] + (nE.vf1[i] - nE.vf0[i]) * f0; x1[i] = nE.vf0[i] + (nE.vf1[i] - nE.vf0[i]) * f1; }
            x0[4] = Trig(tStart + (tEnd - tStart) * f0); x1[4] = Trig(tStart + (tEnd - tStart) * f1);
            nE.DenseStep(x0, x1, (tEnd - tStart) / kEdgeParts);
          }
        c45 = nE.V(cymodel::eg::kRow_c45node); c40 = nE.V(cymodel::eg::kRow_c40node); q20 = nE.V(cymodel::eg::kRow_q20emit); q19 = nE.V(cymodel::eg::kRow_q19emit);
      }
      nB.vf1[2] = 15.0; nB.vf1[3] = -15.0; nB.vf1[9] = bp2; nB.vf1[10] = c40; nB.vf1[11] = q20; nB.vf1[12] = n120; nB.Step();
      nC.vf1[2] = 15.0; nC.vf1[3] = -15.0; nC.vf1[9] = bp2; nC.vf1[10] = q19; nC.vf1[11] = n112; nC.Step();
      nAD.vf1[2] = 15.0; nAD.vf1[3] = -15.0; nAD.vf1[8] = bp1; nAD.vf1[10] = c45; nAD.vf1[11] = nC.V(cymodel::cellc::kRow_u5aout); nAD.vf1[12] = nB.V(cymodel::cellb::kRow_u5bout); nAD.Step();
      n120 = nAD.V(cymodel::ad::kRow_n120node); n112 = nAD.V(cymodel::ad::kRow_n112node);
    }
    mN120 = n120; mN112 = n112;
    for (int i = 0; i < 16; ++i) { if (mUseE2) nE2.vf0[i] = nE2.vf1[i]; else nE.vf0[i] = nE.vf1[i]; nAD.vf0[i] = nAD.vf1[i]; nB.vf0[i] = nB.vf1[i]; nC.vf0[i] = nC.vf1[i]; }
  }

  bool mECoarse = false;
  double mSr = 48000.0, mH = 1.0 / 192000.0, mDecay = 0.5, mTone = 0.5, mVr6 = 0.16, mAccent = 3.681, mOutRest = 0.0, mCyRest = 0.0, mN120 = 0.0, mN112 = 0.0;
  double mEHold[4] = {0.0, 0.0, 0.0, 0.0};   // c45 / c40 / q20 / q19 of the last E step
  long mHostN = 0; int mSub = 4, mSubT = 0; bool mActive = false, mBuilt = false, mUseE2 = false, mTail = false; double mHT = 0.0, mEnv = 0.0, mEnvPeak = 0.0;
  Bank mBank; SubNet<cymodel::eg> mE; SubNet<cymodel::eg2> mE2; SubNet<cymodel::cellb> mB; SubNet<cymodel::cellc> mC; AdBlock<SubNet<cymodel::ad1>> mAD;
  SubNet<cymodel::eg2> mTE2; SubNet<cymodel::cellb> mTB; SubNet<cymodel::cellc> mTC; AdBlock<SubNet<cymodel::ad1>> mTAD;   // the coarse set of the quiet tail
};

} }  // namespace dgg::cyfast
