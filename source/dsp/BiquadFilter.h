// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Reiner Prokein
#pragma once

#include <cmath>

namespace dgg {

// RBJ Audio EQ Cookbook Biquad filter. Implements the same formulas as
// Web Audio's BiquadFilterNode, for sound parity with the original JS
// audio engine. Uses Direct Form I (stable and sufficient for a few voices).
// Currently supports Bandpass, Highpass, and Lowpass; can be extended.

class BiquadFilter
{
public:
  // Bandpass with constant 0‑dB peak gain (RBJ formula; Web Audio bandpass).
  void SetBandpass(double freqHz, double q, double sampleRate)
  {
    const double w0 = 6.283185307179586 * freqHz / sampleRate;
    const double alpha = std::sin(w0) / (2.0 * q);
    const double cosw0 = std::cos(w0);

    const double b0 = alpha;
    const double b1 = 0.0;
    const double b2 = -alpha;
    const double a0 = 1.0 + alpha;
    const double a1 = -2.0 * cosw0;
    const double a2 = 1.0 - alpha;

    SetCoeffs(b0, b1, b2, a0, a1, a2);
  }

  // Highpass with Q=0.7071 (Butterworth); matches Web Audio's default Q.
  void SetHighpass(double freqHz, double sampleRate, double q = 0.70710678)
  {
    const double w0 = 6.283185307179586 * freqHz / sampleRate;
    const double alpha = std::sin(w0) / (2.0 * q);
    const double cosw0 = std::cos(w0);

    const double b0 = (1.0 + cosw0) / 2.0;
    const double b1 = -(1.0 + cosw0);
    const double b2 = (1.0 + cosw0) / 2.0;
    const double a0 = 1.0 + alpha;
    const double a1 = -2.0 * cosw0;
    const double a2 = 1.0 - alpha;

    SetCoeffs(b0, b1, b2, a0, a1, a2);
  }

  // Lowpass with Q=0.7071 (Butterworth) by default; used for tone control
  // (same RBJ formulas as Highpass, just different b coefficients).
  void SetLowpass(double freqHz, double sampleRate, double q = 0.70710678)
  {
    const double w0 = 6.283185307179586 * freqHz / sampleRate;
    const double alpha = std::sin(w0) / (2.0 * q);
    const double cosw0 = std::cos(w0);

    const double b0 = (1.0 - cosw0) / 2.0;
    const double b1 = 1.0 - cosw0;
    const double b2 = (1.0 - cosw0) / 2.0;
    const double a0 = 1.0 + alpha;
    const double a1 = -2.0 * cosw0;
    const double a2 = 1.0 - alpha;

    SetCoeffs(b0, b1, b2, a0, a1, a2);
  }

  // Clears the internal delay line. Should be called on each new voice trigger
  // to avoid bleeding from the previous hit.
  void Reset()
  {
    mX1 = mX2 = mY1 = mY2 = 0.0;
  }

  double Process(double x)
  {
    const double y = mB0 * x + mB1 * mX1 + mB2 * mX2 - mA1 * mY1 - mA2 * mY2;
    mX2 = mX1; mX1 = x;
    mY2 = mY1; mY1 = y;
    return y;
  }

private:
  void SetCoeffs(double b0, double b1, double b2, double a0, double a1, double a2)
  {
    mB0 = b0 / a0; mB1 = b1 / a0; mB2 = b2 / a0;
    mA1 = a1 / a0; mA2 = a2 / a0;
  }

  double mB0 = 1.0, mB1 = 0.0, mB2 = 0.0, mA1 = 0.0, mA2 = 0.0;
  double mX1 = 0.0, mX2 = 0.0, mY1 = 0.0, mY2 = 0.0;
};

} // namespace dgg