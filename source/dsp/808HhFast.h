// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Reiner Prokein
#pragma once
// HiHat fast model: oscillator bank + BP2 (exact), envelope generator (table + solver), signal core (QH27 / DH13 chain, reduced Newton on the nonlinear nodes) and the linear output stage with the level pot.
// One instance is one chain (open or closed); the open hat can be choked by a closed-hat hit. kSub core steps per host sample.
#include <algorithm>
#include <cmath>
#include <type_traits>
#include "808HhBank.h"
#include "808HhEnv.h"
#include "808HhNet.h"
#include "808HhModel.h"

namespace dgg { namespace hhfast {

template <bool kOpen> class HhChain
{
 public:
  using Core = ReducedNet<typename std::conditional<kOpen, hhmodel::zoh, hhmodel::zch>::type>;
  using Out = LinBlock<typename std::conditional<kOpen, hhmodel::outoh, hhmodel::outch>::type>;
#ifndef HH_KSUB
#define HH_KSUB 6
#endif
  static constexpr int kSub = HH_KSUB;    // core steps per host sample (3.5 us at 48 kHz); 3 steps cost 4-6 dB against NGSpice (the stiff nodes, 1.5 nF at 0.4-33 kOhm)
  static constexpr int kSubT = (kSub % 2 == 0) ? kSub / 2 : 0;   // core steps per host sample in the quiet tail (0: no coarse set)
  static constexpr int kLevels = 4;       // step halvings of a failed core step
  static constexpr int kEnvNode = kOpen ? 0 : 9;   // CH: n350 is part of the core
  static constexpr int kCoreOut = kOpen ? 7 : 7;   // n321 / n320, behind the coupling capacitor CH80 / CH82

  // vr3 (open only): decay pot 0 .. 1; level: output pot 0 .. 1 (the rheostat of Rvr7 / Rvr8 over 100 kOhm)
  void Configure(double sampleRate)
  {
    mSr = sampleRate; mH = 1.0 / (sampleRate * kSub); for (int l = 0; l < kLevels; ++l) mCoreL[l].Build(mH / (double)(1 << l)); mOut.Build(mH, mLevel);
    if (kSubT > 0) { mHT = 1.0 / (sampleRate * kSubT); for (int l = 0; l < kLevels; ++l) mCoreT[l].Build(mHT / (double)(1 << l)); mOutT.Build(mHT, mLevel); }
    ComputeCoreDc();
  }
  void SetLevel(double level) { mLevel = level; mOut.Build(mH, level); if (kSubT > 0) mOutT.Build(mHT, level); }

  // accentVolt: trigger amplitude of the accent stage (4.09 V = the normal note, up to 14 V; hhenv::Env::AccentVolt(accent01)); below the threshold of the stage the hit gives no sound; bankOffset / preroll: see Bank::Reset
  void Trigger(double accentVolt, double vr3, double pitchRatio, double bankOffset, double preroll)
  {
    mEnv.Trigger(kOpen ? hhenv::Env::Open : hhenv::Env::Closed, accentVolt, vr3);
    mActive = mEnv.Active(); mHost = 0; mQuiet = 0; mTail = false; mPow = 0.0; mPowPeak = 0.0;
    if (!mActive) return;
    mBank.Reset(pitchRatio, bankOffset, preroll);
    mCoreL[0].SetState(mCoreDc);
    mOut.Reset(mCoreL[0].V(kCoreOut)); mOutRest = mOut.Out();
    mBp0 = mBank.Eval(0.0); mEnv0 = kOpen ? mEnv.Value(0.0) : mCoreDc[kEnvNode]; mDrv0 = 0.0;
    if (kOpen) { mFolX = mFolDcX; mV13 = Follower(mEnv0, mCoreDc[0]); mV349prev = mCoreDc[0]; }
  }
  void Choke(double chAccentVolt) { if (kOpen) mEnv.Choke(chAccentVolt); }
  bool Active() const { return mActive; }

  // one host sample
  double Process()
  {
    if (!mActive) return 0.0;
    const bool tail = mTail; const int sub = tail ? kSubT : kSub; const double hh = tail ? mHT : mH;
    Core* const L = tail ? mCoreT : mCoreL; Out& out = tail ? mOutT : mOut;
    double vf0[16] = {0}, vf1[16] = {0};
    vf0[2] = vf1[2] = 15.0; vf0[3] = vf1[3] = -15.0; vf0[8] = mBp0; vf0[14] = mEnv0; vf0[15] = mDrv0; vf0[13] = mV13;
    double bp[kSub], en[kSub], dr[kSub];
    for (int k = 0; k < sub; ++k)
    {
      const double t = (double)(mHost * sub + k + 1) * hh; bp[k] = mBank.Eval(t); en[k] = kOpen ? mEnv.Value(t) : 0.0; dr[k] = kOpen ? 0.0 : mEnv.Drive(t);
#ifdef HH_EXT_SIGNALS   // test build: bank output and envelope node from arrays (the NGSpice traces), linear between the host samples
      if (extBp) bp[k] = ExtAt(extBp, extBp0, t); if (extEnv && kOpen) en[k] = ExtAt(extEnv, extEnv0, t);
#endif
    }
    // the envelope follower is slow: solved once per host sample at its end, linear in between
    double v13End = 0.0;
    if (kOpen) { const double v349 = L[0].V(0); v13End = Follower(en[sub - 1], v349 + (v349 - mV349prev)); mV349prev = v349; }
    for (int k = 0; k < sub; ++k)
    {
      vf1[8] = bp[k]; vf1[14] = en[k]; vf1[15] = dr[k]; vf1[13] = mV13 + (v13End - mV13) * (double)(k + 1) / (double)sub;
      double vin0 = L[0].V(kCoreOut); StepCore(L, vf0, vf1, 0); double vin1 = L[0].V(kCoreOut);
#ifdef HH_EXT_SIGNALS
      if (extOut) { vin0 = ExtAt(extOut, extOut0, (double)(mHost * sub + k) * hh); vin1 = ExtAt(extOut, extOut0, (double)(mHost * sub + k + 1) * hh); }
#endif
      out.Step(vin0, vin1);
      vf0[8] = vf1[8]; vf0[14] = vf1[14]; vf0[15] = vf1[15]; vf0[13] = vf1[13];
    }
    ++mHost;
    mBp0 = vf0[8]; mEnv0 = kOpen ? vf0[14] : L[0].V(kEnvNode); mDrv0 = vf0[15]; mV13 = vf0[13];
    if (mEnv0 < mEnvRest + 0.005 && (double)mHost / mSr > hhmodel::envtab::kT0) { if (++mQuiet > mQuietLimit) mActive = false; } else mQuiet = 0;
    const double y = out.Out() - mOutRest;
    if (adaptiveTail && kSubT > 0) UpdateTail(y);
    return y;
  }

  // Quiet tail: the error of a core step is a fraction of the signal (OH -38 dB at 6 steps per host sample, -25 dB at 3), so with the signal 15 dB below its peak the coarse set (half the
  // steps) stays below the absolute error of the normal set at the peak. The level is a 10 ms mean of the output power; leaving the coarse set (a rising level) has a hysteresis.
  bool adaptiveTail = true;
  bool InTail() const { return mTail; }

#ifdef HH_EXT_SIGNALS
 public:
  const double* extBp = nullptr; const double* extEnv = nullptr; const double* extOut = nullptr; double extBp0 = 0.0, extEnv0 = 0.0, extOut0 = 0.0;   // value at the trigger and the values at the end of host sample i = 0, 1, ...
  double extDt = 0.0;   // spacing of the arrays (0: the host sample)
  double ExtAt(const double* a, double v0, double t) const { const double pos = extDt > 0.0 ? t / extDt : t * mSr; const long i = (long)std::floor(pos); const double f = pos - (double)i; const double lo = i == 0 ? v0 : a[i - 1], hi = a[i]; return lo + f * (hi - lo); }
#endif
 private:
  // A step the Newton iteration does not master (a follower base that jumps by 0.8 V within a few microseconds; the failed step leaves the state untouched) is repeated as two half steps with the inputs
  // interpolated, down to 1/8 of the step. The state lives in mCoreL[0]; the finer levels only carry the matrices for their step size.
  void StepCore(Core* L, const double* vf0, const double* vf1, int level)
  {
    Core& c = L[level]; if (level > 0) c.CopyStateFrom(L[0]);
    if (c.Step(vf0, vf1) || level + 1 >= kLevels)
    {
      if (level > 0) L[0].CopyStateFrom(c);
      if (!L[0].Finite()) L[0].SetState(mCoreDc);
      return;
    }
    double vfm[16]; for (int i = 0; i < 16; ++i) vfm[i] = 0.5 * (vf0[i] + vf1[i]);
    StepCore(L, vf0, vfm, level + 1); StepCore(L, vfm, vf1, level + 1);
  }

  void UpdateTail(double y)
  {
    constexpr double kEnterRel = 3.16e-3, kLeaveRel = 1.0e-2;   // power relative to the peak power: -25 dB (-12.5 dB in amplitude) in, -20 dB out
    constexpr double kEarliest = 0.05;                            // s; the peak is reached within the first 20 ms
    mPow += (y * y - mPow) * (1.0 / (0.01 * mSr)); if (mPow > mPowPeak) mPowPeak = mPow;
    if (!mTail)
    {
      if ((double)mHost / mSr > kEarliest && mPow < mPowPeak * kEnterRel) { SwitchTo(true); }
    }
    else if (mPow > mPowPeak * kLeaveRel) SwitchTo(false);
  }
  // state transfer between the two step-size sets (no extrapolation history)
  void SwitchTo(bool tail)
  {
    Core* const from = tail ? mCoreL : mCoreT; Core* const to = tail ? mCoreT : mCoreL;
    double v[Core::N]; for (int i = 0; i < Core::N; ++i) v[i] = from[0].V(i);
    to[0].SetState(v); (tail ? mOutT : mOut).CopyStateFrom(tail ? mOut : mOutT); mTail = tail;
  }

  // Emitter n582 of the envelope follower QH29 (collector +15 V, base = envelope vb): algebraic node with RH143 33 k to ground and RH144 33 k to n349. With x = vbe the node equation
  // Ie(x) = (vb - x) (G1 + G2) - v349 G2 is a scalar equation (warm start from the last solution, one or two Newton steps).
  double Follower(double vb, double v349)
  {
    constexpr double G1 = 1.0 / 33000.0, G2 = 1.0 / 33000.0, G = G1 + G2;
    double x = mFolX;
    for (int it = 0; it < 8; ++it)
    {
      const auto c = detail::Bjt(detail::Npn(), x, vb - 15.0);
      const double F = (c.ic + c.ib) - (vb - x) * G + v349 * G2, dF = c.icx + c.ibx + G;
      double dx = -F / dF; dx = std::max(-0.1, std::min(0.1, dx)); x += dx;
      if (std::fabs(dx) < 1e-6) break;
    }
    mFolX = x; return vb - x;
  }

  void ComputeCoreDc()
  {
    using M = typename std::conditional<kOpen, hhmodel::zoh, hhmodel::zch>::type;
    DenseNet<M> net; double vf[16] = {0}; vf[2] = 15.0; vf[3] = -15.0;
    hhbank::Bank b; b.Reset(1.0, 0.0, 0.0); vf[8] = b.Eval(0.0); mEnvRest = hhmodel::env::kRest[kOpen ? hhenv::Env::kNodeOh : hhenv::Env::kNodeCh]; vf[14] = mEnvRest;
    if (kOpen) { mFolX = 0.0; for (int it = 0; it < 6; ++it) { vf[13] = Follower(mEnvRest, net.V(0)); net.Dc(vf); } mFolDcX = mFolX; }
    else net.Dc(vf);
    for (int i = 0; i < Core::N; ++i) mCoreDc[i] = net.V(i);
    mQuietLimit = (long)(0.05 * mSr);
  }

  double mSr = 48000.0, mH = 1.0 / 192000.0, mLevel = 1.0, mBp0 = 0.0, mEnv0 = 0.0, mDrv0 = 0.0, mOutRest = 0.0, mEnvRest = 0.0, mCoreDc[Core::N], mFolX = 0.0, mFolDcX = 0.0, mV13 = 0.0, mV349prev = 0.0;
  long mHost = 0, mQuiet = 0, mQuietLimit = 2400; bool mActive = false, mTail = false; double mHT = 0.0, mPow = 0.0, mPowPeak = 0.0;
  hhbank::Bank mBank; hhenv::Env mEnv; Core mCoreL[kLevels]; Out mOut;
  Core mCoreT[kLevels]; Out mOutT;   // the coarse set of the quiet tail
};

} }  // namespace dgg::hhfast
