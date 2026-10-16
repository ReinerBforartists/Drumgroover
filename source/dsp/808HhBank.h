// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Reiner Prokein
#pragma once
// HiHat oscillator bank + BP2 band pass (U3-B MFB) as one exact solution: the six CD4584 Schmitt oscillators are an analytic edge schedule (ideal gates, 1 us edges), the sum of their outputs S drives the
// linear BP2, whose DAE is solved in closed form per segment (modes from 808HhModel.h, bp2). Output = bp2out at any time, no time step.
#include <cmath>
#include <algorithm>
#include "808HhModel.h"
#include "808CyModel.h"

namespace dgg { namespace hhbank {

// the modes of the HiHat BP2 as the traits of the generic bank (one output, one real mode, one complex pair)
struct Bp2Modes
{
  static constexpr int kNumOut = 1, kNumR = 1, kNumC = 1;
  static constexpr double kY0[1] = {hhmodel::bp2::kY0}, kBeta[1] = {hhmodel::bp2::kBeta};
  static constexpr double kLamR[1] = {hhmodel::bp2::kLamR}, kCR[1] = {hhmodel::bp2::kCR}, kVR[1] = {hhmodel::bp2::kVR};
  static constexpr double kLamCRe[1] = {hhmodel::bp2::kLamCRe}, kLamCIm[1] = {hhmodel::bp2::kLamCIm}, kCCRe[1] = {hhmodel::bp2::kCCRe}, kCCIm[1] = {hhmodel::bp2::kCCIm};
  static constexpr double kVCRe[1] = {hhmodel::bp2::kVCRe}, kVCIm[1] = {hhmodel::bp2::kVCIm};
};

// M: modes of the linear band-pass network (Bp2Modes, or cymodel::bp of the cymbal: bp1out and bp2out from the same oscillator sum)
template <class M>
class BankT
{
 public:
  static constexpr double kVtp = 0.54, kVtm = 0.42, kVdd = 4.78756557, kEdge = 1e-6;

  // pitchRatio divides all RC products; offset = bank time at the trigger (s); preroll = how long before the trigger the BP2 is run from rest (s)
  void Reset(double pitchRatio, double offset, double preroll)
  {
    static const double R[6] = {680e3, 560e3, 560e3, 220e3, 100e3 + 220e3 * 0.4809, 150e3 + 220e3 * 0.4132};
    static const double C[6] = {10e-9, 10e-9, 18e-9, 18e-9, 18e-9, 10e-9};
    for (int k = 0; k < 6; ++k)
    {
      const double rc = R[k] * C[k] / pitchRatio; mT1[k] = -rc * std::log(1.0 - kVtp); mLow[k] = rc * std::log(kVtp / kVtm); mPer[k] = mLow[k] + rc * std::log((1.0 - kVtm) / (1.0 - kVtp));
    }
    mOffset = offset; mX = std::max(0.0, offset - preroll); mSp = 0.0; mLastDt = -1.0;
    for (int i = 0; i < M::kNumR; ++i) mZr[i] = 0.0;
    for (int j = 0; j < M::kNumC; ++j) mZcRe[j] = mZcIm[j] = 0.0;
    for (int k = 0; k < 6; ++k) Locate(k, mX);
  }

  // advances to t seconds after the trigger (t must not decrease between calls); Out(k) is output k at that time
  void Advance(double t) { AdvanceTo(mOffset + t); }
  // bp2out at t seconds after the trigger (single-output banks)
  double Eval(double t)
  {
    AdvanceTo(mOffset + t);
    return Out(0);
  }

 private:
  double mT1[6], mLow[6], mPer[6];
  double mBase[6], mEnd[6], mSlope[6]; int mSeg[6];
  double mOffset = 0.0, mX = 0.0, mSp = 0.0, mLastDt = -1.0;
  double mZr[M::kNumR], mZcRe[M::kNumC], mZcIm[M::kNumC], mEr[M::kNumR], mEcCos[M::kNumC], mEcSin[M::kNumC];

  static constexpr double kSlope = kVdd / kEdge;

  // the mode amplitudes are kept at the current time mX
 public:
  double Out(int k = 0) const
  {
    double y = M::kY0[k] - M::kBeta[k] * mSp;
    for (int i = 0; i < M::kNumR; ++i) y += M::kVR[k * M::kNumR + i] * mZr[i];
    for (int j = 0; j < M::kNumC; ++j) y += 2.0 * (M::kVCRe[k * M::kNumC + j] * mZcRe[j] - M::kVCIm[k * M::kNumC + j] * mZcIm[j]);
    return y;
  }

 private:
  // decay and rotation of the modes by dt; the factors of the last step length are cached (the host steps are equal up to rounding of the absolute time)
  void Rotate(double dt)
  {
    if (std::fabs(dt - mLastDt) > 1e-13)
    {
      mLastDt = dt;
      for (int i = 0; i < M::kNumR; ++i) mEr[i] = std::exp(M::kLamR[i] * dt);
      for (int j = 0; j < M::kNumC; ++j) { const double ec = std::exp(M::kLamCRe[j] * dt), ang = M::kLamCIm[j] * dt; mEcCos[j] = ec * std::cos(ang); mEcSin[j] = ec * std::sin(ang); }
    }
    for (int i = 0; i < M::kNumR; ++i) mZr[i] *= mEr[i];
    for (int j = 0; j < M::kNumC; ++j) { const double a = mZcRe[j], b = mZcIm[j]; mZcRe[j] = a * mEcCos[j] - b * mEcSin[j]; mZcIm[j] = a * mEcSin[j] + b * mEcCos[j]; }
  }

  // segment of oscillator k at bank time x: 0 pre (high), 1 falling edge, 2 low, 3 rising edge, 4 high
  void Locate(int k, double x)
  {
    if (x < mT1[k]) { mSeg[k] = 0; mBase[k] = 0.0; mEnd[k] = mT1[k]; mSlope[k] = 0.0; mSp += 0.0; return; }
    const double q = std::fmod(x - mT1[k], mPer[k]); mBase[k] = x - q;
    if (q < kEdge) mSeg[k] = 1; else if (q < mLow[k]) mSeg[k] = 2; else if (q < mLow[k] + kEdge) mSeg[k] = 3; else mSeg[k] = 4;
    SetSegment(k); mSp += mSlope[k];
  }
  void SetSegment(int k)
  {
    switch (mSeg[k])
    {
      case 1: mEnd[k] = mBase[k] + kEdge; mSlope[k] = -kSlope; break;
      case 2: mEnd[k] = mBase[k] + mLow[k]; mSlope[k] = 0.0; break;
      case 3: mEnd[k] = mBase[k] + mLow[k] + kEdge; mSlope[k] = kSlope; break;
      default: mEnd[k] = mBase[k] + mPer[k]; mSlope[k] = 0.0; break;
    }
  }

  void AdvanceTo(double x)
  {
    for (;;)
    {
      int kb = 0; for (int k = 1; k < 6; ++k) if (mEnd[k] < mEnd[kb]) kb = k;
      const double xe = mEnd[kb]; if (xe > x) break;
      // decay all modes to the corner, then the jump of zeta from the slope change
      const double dt = xe - mX; if (dt > 0.0) Rotate(dt);
      mX = xe;
      const double oldSlope = mSlope[kb];
      if (mSeg[kb] == 0) { mSeg[kb] = 1; mBase[kb] = mT1[kb]; }
      else if (mSeg[kb] == 4) { mSeg[kb] = 1; mBase[kb] += mPer[kb]; }
      else ++mSeg[kb];
      SetSegment(kb);
      const double dS = mSlope[kb] - oldSlope; mSp += dS;
      for (int i = 0; i < M::kNumR; ++i) mZr[i] += M::kCR[i] * dS;
      for (int j = 0; j < M::kNumC; ++j) { mZcRe[j] += M::kCCRe[j] * dS; mZcIm[j] += M::kCCIm[j] * dS; }
    }
    if (x > mX) { Rotate(x - mX); mX = x; }
  }
};

// The hi-hat bank: bp2out of the cymbal's bank. BP1 hangs on the same summing node (C10 / R52 / U3-A) and loads it, which a bank with BP2 alone (Bp2Modes) leaves out; NGSpice with the whole deck: the
// waveform error of bp2out against the full deck falls from -30 dB to -36 dB (-41 dB with the cymbal VCA inputs C42 / C44 taken off bp2out). Same oscillator schedule, Eval returns bp2out.
class Bank : public BankT<cymodel::bp>
{
 public:
  double Eval(double t) { Advance(t); return Out(1); }
};

} }  // namespace dgg::hhbank
