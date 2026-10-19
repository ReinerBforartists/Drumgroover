// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Reiner Prokein
#pragma once
// HiHat envelope generator (trigger / accent stage, decay storage, choke logic) as a fast model: the trigger rise phase (0 .. kT0, where the circuit is a stiff edge-driven transient) is a table per
// accent voltage (rows kAcc: the velocity steps up to 4.09 V, then the accent range up to 14 V, linear in between), from kT0 on the 24-node network is integrated with the dense trapezoid solver and a step
// that grows with the time since the trigger (the decay is smooth). The state at kT0 is tabulated per accent voltage and VR3 (OH). A closed-hat trigger chokes a sounding open hat by driving the CH section of the OH network to the state of a CH hit.
#include <algorithm>
#include <cmath>
#include "808HhNet.h"
#include "808HhModel.h"

namespace dgg { namespace hhenv {

class Env
{
 public:
  enum Side { Open = 0, Closed = 1 };
  using Tab = hhmodel::envtab;
  static constexpr int kN = hhmodel::env::kN, kNodeOh = 18, kNodeCh = 22;   // unknowns n334 (OH envelope) and n350 (CH envelope)
  static constexpr double kAccentFullScaleVolt = 4.09, kAccentMaxVolt = 14.0;

  // trigger amplitude of the accent stage: 0 = the normal note (4.09 V), 1 = the full accent of the 808 (14 V)
  static double AccentVolt(double accent01) { return kAccentFullScaleVolt + std::min(1.0, std::max(0.0, accent01)) * (kAccentMaxVolt - kAccentFullScaleVolt); }
  // hits below the lowest tabulated voltage do not move the envelope (the accent stays under the threshold of the trigger stage)
  static bool Audible(double accentVolt) { return accentVolt >= Tab::kAcc[0] - 1e-9; }

  // vr3: the decay pot of the open hat (0 .. 1, the rheostat of Rvr3 over 2 MOhm)
  void Trigger(Side side, double accentVolt, double vr3)
  {
    mSide = side; mActive = Audible(accentVolt); mNode = side == Open ? kNodeOh : kNodeCh; mStarted = false; mPendingChoke = -1; mChokeAcc = 0.0;
    mRest = hhmodel::env::kRest[mNode]; mT = 0.0;
    if (!mActive) return;
    mAccent = std::min(accentVolt, kAccentMaxVolt);
    int vi; double wa; AccRow(mAccent, vi, wa);
    const size_t ro = (size_t)vi * Tab::kNumT; mEarly = (side == Open ? Tab::kEarlyOh : Tab::kEarlyCh) + ro; mDrive = Tab::kDriveCh + ro; mEarlyIdx = mDriveIdx = 0;
    mEarly2 = wa > 0.0 ? mEarly + Tab::kNumT : nullptr; mDrive2 = wa > 0.0 ? mDrive + Tab::kNumT : nullptr; mEarly2Idx = mDrive2Idx = 0; mAccW = wa;
    // seed at kT0
    if (side == Open)
    {
      vr3 = std::min(1.0, std::max(0.0, vr3)); const double u = std::sqrt(vr3); double us[Tab::kNumVr3]; for (int j = 0; j < Tab::kNumVr3; ++j) us[j] = std::sqrt(Tab::kVr3[j]);
      int j = 0; while (j + 2 < Tab::kNumVr3 && u > us[j + 1]) ++j; const double w = (u - us[j]) / (us[j + 1] - us[j]);
      const double* a = Tab::kSeedOh + ((size_t)vi * Tab::kNumVr3 + j) * kN; const double* b = a + kN;
      for (int k = 0; k < kN; ++k) mSeed[k] = a[k] + w * (b[k] - a[k]);
      if (wa > 0.0)
      {
        const double* a2 = a + (size_t)Tab::kNumVr3 * kN; const double* b2 = a2 + kN;
        for (int k = 0; k < kN; ++k) mSeed[k] += wa * ((a2[k] + w * (b2[k] - a2[k])) - (a[k] + w * (b[k] - a[k])));
      }
      mNet.SetFixConductance(kVr3Fix, 1.0 / std::max(2e6 * vr3, 1.0));
    }
    else
    {
      const double* a = Tab::kSeedCh + (size_t)vi * kN; for (int k = 0; k < kN; ++k) mSeed[k] = a[k] + (wa > 0.0 ? wa * (a[kN + k] - a[k]) : 0.0);
      mNet.SetFixConductance(kVr3Fix, 1.0 / (2e6 * 0.5));
    }
  }

  // a closed hat is hit (velocity chVel127) while this open-hat envelope sounds: from now (the time of the next Value() call) the CH section of the network is that of a CH hit
  void Choke(double chAccentVolt)
  {
    if (mSide != Open || !mActive || !Audible(chAccentVolt)) return;
    mChokeAcc = std::min(chAccentVolt, kAccentMaxVolt); mPendingChoke = mT;   // taken over at the next network step boundary (at most one step, 1 ms, late)
  }

  // closed hat: the base n578 of the envelope follower QH28 (fed to the signal core, which contains the follower); t must not decrease between calls
  double Drive(double t)
  {
    if (!mActive || t >= Tab::kT0) return 0.0;
    double d = Interp(mDrive, mDriveIdx, t, 0.0);
    if (mDrive2) d += mAccW * (Interp(mDrive2, mDrive2Idx, t, 0.0) - d);
    return std::max(0.0, d);   // the ringing of the trigger edge below zero keeps the follower off anyway
  }

  bool Active() const { return mActive; }

  // envelope node voltage t seconds after the trigger; t must not decrease between calls
  double Value(double t)
  {
    if (!mActive) return mRest;
    mT = t;
    if (t < Tab::kT0) return Early(t);
    if (!mStarted) Start();
    while (mTb < t) StepNet();
    if (mPendingChoke >= 0.0) { ApplyChoke(); StepNet(); }
    if (mTb <= mTa) return mVb;
    const double w = (t - mTa) / (mTb - mTa); return mVa + w * (mVb - mVa);
  }

 private:
  static constexpr int kVr3Fix = 4;   // index of the Rvr3 conductance in the fixed-node list of hhmodel::env
  hhfast::DenseNet<hhmodel::env> mNet;
  Side mSide = Open; bool mActive = false, mStarted = false; int mNode = kNodeOh; double mAccent = 0.0, mRest = 0.0, mT = 0.0, mPendingChoke = -1.0, mChokeAcc = 0.0;
  const double* mEarly = nullptr; const double* mDrive = nullptr; const double* mEarly2 = nullptr; const double* mDrive2 = nullptr; int mEarlyIdx = 0, mDriveIdx = 0, mEarly2Idx = 0, mDrive2Idx = 0; double mAccW = 0.0; double mSeed[kN];

  // row of the accent table at or below the voltage and the weight of the next row (0 at a tabulated voltage, so the tabulated rows are used as they are)
  static void AccRow(double accentVolt, int& row, double& w)
  {
    int i = 0; while (i + 2 < Tab::kNumAcc && accentVolt >= Tab::kAcc[i + 1]) ++i;
    row = i; w = accentVolt <= Tab::kAcc[i] ? 0.0 : std::min(1.0, (accentVolt - Tab::kAcc[i]) / (Tab::kAcc[i + 1] - Tab::kAcc[i]));
  }
  double mTa = 0.0, mTb = 0.0, mVa = 0.0, mVb = 0.0;

  // rise phase: cubic Hermite through the table (starting at rest)
  double Early(double t)
  {
    const double e = Interp(mEarly, mEarlyIdx, t, mRest);
    return mEarly2 ? e + mAccW * (Interp(mEarly2, mEarly2Idx, t, mRest) - e) : e;
  }
  static double Interp(const double* tab, int& idx, double t, double rest)
  {
    const double* tg = Tab::kTime; const int n = Tab::kNumT;
    if (t <= tg[0]) { const double w = t / tg[0]; return rest + w * (tab[0] - rest); }
    while (idx + 1 < n - 1 && tg[idx + 1] < t) ++idx;
    const int i = idx; const double t0 = tg[i], t1 = tg[i + 1], h = t1 - t0, s = (t - t0) / h;
    auto slope = [&](int k) { if (k <= 0) return (tab[1] - tab[0]) / (tg[1] - tg[0]); if (k >= n - 1) return (tab[n - 1] - tab[n - 2]) / (tg[n - 1] - tg[n - 2]); return (tab[k + 1] - tab[k - 1]) / (tg[k + 1] - tg[k - 1]); };
    const double m0 = slope(i) * h, m1 = slope(i + 1) * h, p0 = tab[i], p1 = tab[i + 1];
    const double s2 = s * s, s3 = s2 * s;
    return (2 * s3 - 3 * s2 + 1) * p0 + (s3 - 2 * s2 + s) * m0 + (-2 * s3 + 3 * s2) * p1 + (s3 - s2) * m1;
  }

  void FixedValues(double* vf) const
  {
    for (int i = 0; i < 16; ++i) vf[i] = 0.0;
    vf[2] = 15.0; vf[3] = -15.0; vf[5] = vf[7] = mAccent; vf[8] = 0.5;
  }
  void Start()
  {
    mStarted = true; mNet.SetState(mSeed); mTa = mTb = Tab::kT0; mVa = mVb = mNet.V(mNode);
    if (mPendingChoke >= 0.0) ApplyChoke();
  }
  void ApplyChoke()
  {
    int vi; double wa; AccRow(mChokeAcc, vi, wa); const double* c = Tab::kSeedCh + (size_t)vi * kN;
    for (int k : {12, 13, 14, 22}) mNet.SetV(k, c[k] + (wa > 0.0 ? wa * (c[kN + k] - c[k]) : 0.0));
    mPendingChoke = -1.0;
  }
  void StepNet()
  {
    double vf[16]; FixedValues(vf);
    const double h = std::min(std::max(0.25 * (mTb - Tab::kT0), 1e-6), 2e-3);
    mTa = mTb; mVa = mNet.V(mNode); mNet.Step(h, vf, vf); mTb += h; mVb = mNet.V(mNode);
  }
};

} }  // namespace dgg::hhenv
