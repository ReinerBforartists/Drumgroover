// Output sampling of a circuit run: the probe holds the step ends of the integration (non-uniform, 0.2 - 5 us), the host wants samples at its rate.
// Linear-phase Kaiser-windowed sinc low-pass evaluated directly on the non-uniform nodes (trapezoid weights), sampled at the host instants.
// Content above half the host rate is removed instead of folding into the band.
#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

namespace dgg {

class OutputFilter909
{
public:
  static constexpr int kHalfSamples = 64;   // half width of the kernel in host samples
  static constexpr double kCutFraction = 0.95, kBeta = 9.0;   // cut-off as a fraction of half the host rate

  explicit OutputFilter909(int fs) : fs_(fs), half_(kHalfSamples / static_cast<double>(fs))
  {
    const double fc = kCutFraction * 0.5 * fs;
    tab_.resize(kTab + 2);
    for (int i = 0; i < kTab + 2; ++i)
    {
      const double tau = -half_ + 2.0 * half_ * i / kTab, r = tau / half_, x = 2.0 * 3.14159265358979323846 * fc * tau;
      const double s = tau == 0.0 ? 2.0 * fc : std::sin(x) / (3.14159265358979323846 * tau), w = std::fabs(r) >= 1.0 ? 0.0 : I0(kBeta * std::sqrt(1.0 - r * r)) / I0(kBeta);
      tab_[static_cast<size_t>(i)] = s * w;
    }
  }
  // Time before / after the first / last output instant that the nodes must cover.
  double Margin() const { return half_; }
  // Output sample k is the low-passed signal at t0 + k / fs; the nodes must cover [t0 - Margin(), t0 + (n - 1) / fs + Margin()].
  void Resample(const std::vector<double>& t, const std::vector<double>& v, double t0, size_t n, std::vector<double>& out) const
  {
    out.assign(n, 0.0); const size_t m = t.size(); if (m < 2) return;
    std::vector<double> w(m);
    w[0] = 0.5 * (t[1] - t[0]); w[m - 1] = 0.5 * (t[m - 1] - t[m - 2]); for (size_t j = 1; j + 1 < m; ++j) w[j] = 0.5 * (t[j + 1] - t[j - 1]);
    size_t lo = 0;
    for (size_t k = 0; k < n; ++k)
    {
      const double tk = t0 + static_cast<double>(k) / fs_;
      while (lo < m && t[lo] < tk - half_) ++lo;
      double num = 0.0, den = 0.0;
      for (size_t j = lo; j < m && t[j] <= tk + half_; ++j) { const double h = Kernel(t[j] - tk) * w[j]; num += h * v[j]; den += h; }
      out[k] = den != 0.0 ? num / den : 0.0;
    }
  }

private:
  static constexpr int kTab = 1 << 15;
  double Kernel(double tau) const { const double x = (tau + half_) * (kTab / (2.0 * half_)); const int i = static_cast<int>(x); const double f = x - i; return tab_[static_cast<size_t>(i)] * (1.0 - f) + tab_[static_cast<size_t>(i) + 1] * f; }
  static double I0(double x) { double s = 1.0, tt = 1.0; for (int k = 1; k < 60; ++k) { tt *= (x / (2.0 * k)) * (x / (2.0 * k)); s += tt; if (tt < 1e-18 * s) break; } return s; }
  int fs_; double half_; std::vector<double> tab_;
};

}  // namespace dgg
