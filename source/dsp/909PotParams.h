// TR-909 fast voices: the circuit pot positions of a set (Tr909SetKey::params).
//
// params packs one byte per circuit pot of the voice, in the generator's order (LT / MT / HT: Tune, Decay, Level; BD: Tune, Attack, Level, Decay; SD: Tune, Tone, Level, Snappy; HC: Level).
// A pot position is q / kTr909PotSteps with q = 0..kTr909PotSteps (finer than the 0.01 step of the UI knobs); the byte is q XOR kTr909PotNominalQ, so params == 0 is every pot at 0.5, the nominal position of the model headers.
// Level pots here are circuit pots (VR12, HC VR20), not a gain after the voice.
#pragma once
#include "909AccentPlan.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace dgg {

constexpr int kTr909PotSteps = 254;
constexpr int kTr909PotNominalQ = kTr909PotSteps / 2;

inline int Tr909PotCount(Tr909Voice v)
{
  switch (v) { case Tr909Voice::Bd: case Tr909Voice::Sd: return 4; case Tr909Voice::Lt: case Tr909Voice::Mt: case Tr909Voice::Ht: return 3; case Tr909Voice::Hc: return 1; default: return 0; }
}
inline int Tr909PotQuantise(double x) { return std::clamp(static_cast<int>(std::lround(std::clamp(x, 0.0, 1.0) * kTr909PotSteps)), 0, kTr909PotSteps); }
inline uint64_t Tr909PackPots(Tr909Voice v, const double* pots)
{
  uint64_t p = 0; const int n = Tr909PotCount(v);
  for (int i = 0; i < n; ++i) p |= static_cast<uint64_t>(Tr909PotQuantise(pots[i]) ^ kTr909PotNominalQ) << (8 * i);
  return p;
}
// Positions 0..1 of the pots of the voice; pots must hold Tr909PotCount(v) values.
inline void Tr909UnpackPots(Tr909Voice v, uint64_t params, double* pots)
{
  const int n = Tr909PotCount(v);
  for (int i = 0; i < n; ++i) pots[i] = static_cast<double>(static_cast<int>((params >> (8 * i)) & 0xFF) ^ kTr909PotNominalQ) / kTr909PotSteps;
}

}  // namespace dgg
