// Output decimation of a fast PCM voice (non-uniform step ends, ~3 per PCM period) to the host sample rate.
// Local 4-point Lagrange interpolation on the non-uniform nodes to a uniform grid of OS x fsHost, linear-phase Kaiser FIR low-pass, decimation by OS.
// Usage: Init(fsHost); Push(tEnd, y) for every step end (strictly increasing times, seconds since the voice start); Pop(out) returns the next host sample if available.
#pragma once
#include <cmath>
namespace { constexpr double kPiDec = 3.14159265358979323846; }
#include <vector>

namespace dgg { namespace pcmfast {

class HostDecimator
{
public:
  void Init(double fsHost)
  {
    fsh = fsHost; os = fsHost > 60000.0 ? 2 : 4; ntaps = fsHost > 60000.0 ? 65 : (fsHost > 46000.0 ? 129 : 257); const double beta = fsHost > 60000.0 ? 8.0 : (fsHost > 46000.0 ? 9.0 : 10.0);
    const double fso = os * fsh, cut = 0.5 * fsh * (fsHost > 60000.0 ? 0.93 : (fsHost > 46000.0 ? 0.93 : 0.95)) / fso;   // cut-off, fraction of the oversampled rate
    h.assign(ntaps, 0.0); const int mid = (ntaps - 1) / 2; double sum = 0.0;
    for (int n = 0; n < ntaps; ++n)
    {
      const double x = n - mid, s = x == 0.0 ? 2.0 * cut : std::sin(2.0 * kPiDec * cut * x) / (kPiDec * x);
      const double r = 2.0 * n / (ntaps - 1) - 1.0, w = I0(beta * std::sqrt(std::fmax(0.0, 1.0 - r * r))) / I0(beta);
      h[n] = s * w; sum += h[n];
    }
    for (double& c : h) c /= sum;   // unity gain at DC
    Reset();
  }
  void Reset() { Restart(0.0); }
  // start a new stream at the voice time tStart (a multiple of 1/fsHost is rounded): the first output sample is the signal at that instant
  void Restart(double tStart)
  {
    cnt = 0; bi = 1; gridStart = (long)std::llround(tStart * fsh) * os; grid = gridStart; cachedBi = -1; line.assign(2 * ntaps, 0.0); linePos = 0; outQ.clear(); outHead = 0;
  }
  long GridStart() const { return gridStart; }
  // delay of the filter in seconds: output sample k is the low-passed signal at the voice time k / fsHost
  double GroupDelay() const { return ((ntaps - 1) / 2.0) / (os * fsh); }
  void Push(double t, double y)
  {
    nT[cnt & 7] = t; nY[cnt & 7] = y; ++cnt;
    for (;;)   // a grid instant is evaluated once the node after its bracketing interval exists (nodes bi-1 .. bi+2)
    {
      const double tg = grid / (os * fsh);
      while (bi + 1 < cnt && nT[(bi + 1) & 7] <= tg) ++bi;
      if (bi + 2 >= cnt) break;
      EmitGrid(Lagrange4(tg)); ++grid;
    }
  }
  bool Pop(double& out) { if (outHead >= (int)outQ.size()) return false; out = outQ[outHead++]; if (outHead > 4096) { outQ.erase(outQ.begin(), outQ.begin() + outHead); outHead = 0; } return true; }

private:
  static double I0(double x) { double s = 1.0, t = 1.0; for (int k = 1; k < 60; ++k) { t *= (x / (2.0 * k)) * (x / (2.0 * k)); s += t; if (t < 1e-18 * s) break; } return s; }
  double Lagrange4(double tq)
  {
    if (cachedBi != bi)   // per bracket: reciprocals of the Lagrange denominators (the bracket changes once per integration step, the grid is evaluated several times per step)
    {
      const double t0 = nT[(bi - 1) & 7], t1 = nT[bi & 7], t2 = nT[(bi + 1) & 7], t3 = nT[(bi + 2) & 7];
      tt[0] = t0; tt[1] = t1; tt[2] = t2; tt[3] = t3;
      dd[0] = 1.0 / ((t0 - t1) * (t0 - t2) * (t0 - t3)); dd[1] = 1.0 / ((t1 - t0) * (t1 - t2) * (t1 - t3));
      dd[2] = 1.0 / ((t2 - t0) * (t2 - t1) * (t2 - t3)); dd[3] = 1.0 / ((t3 - t0) * (t3 - t1) * (t3 - t2)); cachedBi = bi;
    }
    const double a0 = tq - tt[0], a1 = tq - tt[1], a2 = tq - tt[2], a3 = tq - tt[3];
    return nY[(bi - 1) & 7] * dd[0] * a1 * a2 * a3 + nY[bi & 7] * dd[1] * a0 * a2 * a3 + nY[(bi + 1) & 7] * dd[2] * a0 * a1 * a3 + nY[(bi + 2) & 7] * dd[3] * a0 * a1 * a2;
  }
  void EmitGrid(double v)
  {
    if (grid == gridStart) { for (int i = 0; i < 2 * ntaps; ++i) line[i] = v; }   // the signal before the voice start is constant
    line[linePos] = v; line[linePos + ntaps] = v; linePos = linePos + 1 == ntaps ? 0 : linePos + 1;
    const long mid = (ntaps - 1) / 2;
    if (grid - gridStart >= mid && (grid - gridStart - mid) % os == 0)
    {
      const double* x = &line[linePos];   // x[0] oldest ... x[ntaps - 1] newest; h is symmetric
      double acc = h[mid] * x[mid];
      for (int n = 0; n < mid; ++n) acc += h[n] * (x[n] + x[ntaps - 1 - n]);
      outQ.push_back(acc);
    }
  }
  double fsh = 48000.0; int os = 4, ntaps = 129; std::vector<double> h, line, outQ; double nT[8] = {0}, nY[8] = {0}; long cnt = 0, bi = 1, grid = 0, gridStart = 0, cachedBi = -1; double tt[4] = {0}, dd[4] = {0}; int linePos = 0, outHead = 0;
};

}}  // namespace dgg::pcmfast
