// Noise source and trigger pulse of the TR-909 circuit (shared by the circuit runner and the other 909 engines).
#pragma once
#include <atomic>
#include "../BakeAbort.h"
#include <cmath>
#include <cstdint>
#include <vector>

namespace dgg { namespace pcmfast {

// Raised by the host to stop the hits that are being rendered right now (Pause of the cache bake); same counter for every engine (BakeAbort.h).
inline std::atomic<unsigned>& BakeAbortGeneration() { return ::dgg::BakeAbortCounter(); }

// Noise source of the circuit: white samples every `step` seconds (sum of twelve uniform draws), linear in between. step = 5 us is the
// reference source of the engine. A larger step (20 us) keeps the power density and filters the interpolation lowpass (eq).
class HitNoise
{
public:
  static constexpr uint64_t kStreamSeed = 0x0909A0015EEDULL;   // seed of the shared noise stream of the circuit (one source for all voices)
  static constexpr uint64_t kNoStream = ~0ULL;                  // 'no stream position': the hit uses its own sequence (Start(seed))
  void Start(uint64_t seed, double stepSeconds = 5e-6, double eq = 0.0)
  {
    smp_.clear(); step_ = stepSeconds; amp_ = 5.22 * std::sqrt(5e-6 / stepSeconds); eq_ = eq;
    rng_ = 0x9E3779B97F4A7C15ULL ^ 2ULL ^ (seed * 0xD1B54A32D192ED03ULL);
  }
  // Shared noise stream (the circuit has one noise source for all voices): the same sequence as Start(seed), but the hit begins at knot `startKnot` of the stream (absolute time / step of the trigger). The generator is a 64-bit LCG with 12 draws per knot:
  // the state at knot n is the state at the start advanced by 12 n steps (jump by repeated squaring of the affine map), so every voice that reads the stream at the same absolute time gets the same values.
  void StartAt(uint64_t seed, uint64_t startKnot, double stepSeconds = 5e-6, double eq = 0.0)
  {
    Start(seed, stepSeconds, eq);
    uint64_t n = startKnot * 12ULL, A = 1ULL, C = 0ULL, a = 6364136223846793005ULL, c = 1442695040888963407ULL;   // (A, C): the map x -> A x + C advanced by n steps
    while (n) { if (n & 1ULL) { A = A * a; C = C * a + c; } c = c * (a + 1ULL); a = a * a; n >>= 1; }   // (a, c) squared: x -> a (a x + c) + c
    rng_ = A * rng_ + C;
  }
  double step() const { return step_; }
  void Reserve(size_t n) { smp_.reserve(n); }
  double At(double t)
  {
    if (t < 0.0) t = 0.0;   // an idle run before the trigger that is longer than the 50 ms lead-in: the source stays at its start value
    const long idx = static_cast<long>(std::floor(t / step_)); Fill(idx + 1);
    const double fr = t / step_ - idx;
    const double a = eq_ != 0.0 ? Eq(idx) : smp_[idx], b = eq_ != 0.0 ? Eq(idx + 1) : smp_[idx + 1];
    return 7.5 + (a * (1 - fr) + b * fr);
  }
private:
  void Fill(long idx)
  {
    while (static_cast<long>(smp_.size()) <= idx + 1)
    {
      double sm = 0; for (int q = 0; q < 12; ++q) { rng_ = rng_ * 6364136223846793005ULL + 1442695040888963407ULL; sm += (rng_ >> 11) * (1.0 / 9007199254740992.0); }
      double v = (sm - 6.0) * amp_; smp_.push_back(v);
    }
  }
  double Eq(long i) { const double rm = i > 0 ? smp_[i - 1] : smp_[i]; return (1.0 + 2.0 * eq_) * smp_[i] - eq_ * (rm + smp_[i + 1]); }
  std::vector<double> smp_; uint64_t rng_ = 0; double step_ = 5e-6, amp_ = 5.22, eq_ = 0.0;
};

// Trigger of the voice: 5 V pulse, 10 us edges, 2 ms long, starting at t = 0.
inline double HitTrigger(double t)
{
  if (t < 0) return 0.0; if (t < 1e-5) return 5.0 * t / 1e-5; if (t < 1e-5 + 2e-3) return 5.0;
  if (t < 2e-5 + 2e-3) return 5.0 * (1.0 - (t - 1e-5 - 2e-3) / 1e-5); return 0.0;
}

}}  // namespace dgg::pcmfast
