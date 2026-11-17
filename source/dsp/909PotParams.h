// TR-909 fast voices: the circuit pot positions of a set (SetKey909::params).
//
// params packs one byte per circuit pot of the voice, in the generator's order (LT / MT / HT: Tune, Decay, Level; BD: Tune, Attack, Level, Decay; SD: Tune, Tone, Level, Snappy; HC: Level).
// Crash and Ride: Level (VR24 / VR26) and Tune (VR25 / VR27, the PCM clock; 909PcmTune.h); Level keeps byte 0 of the key.
// A pot position is q / kPotSteps909 with q = 0..kPotSteps909 (finer than the 0.01 step of the UI knobs); the byte is q XOR kPotNominalQ909, so params == 0 is every pot at 0.5, the nominal position of the model headers.
// Level pots here are circuit pots (VR12, HC VR20), not a gain after the voice.
#pragma once
#include "909AccentPlan.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace dgg {

constexpr int kPotSteps909 = 254;
constexpr int kPotNominalQ909 = kPotSteps909 / 2;

inline int PotCount909(Voice909 v)
{
  switch (v) { case Voice909::Bd: case Voice909::Sd: return 4; case Voice909::Lt: case Voice909::Mt: case Voice909::Ht: return 3; case Voice909::Hc: return 1; case Voice909::Cr: case Voice909::Ri: case Voice909::Hh: case Voice909::Ho: return 2; default: return 0; }
}
inline int PotQuantise909(double x) { return std::clamp(static_cast<int>(std::lround(std::clamp(x, 0.0, 1.0) * kPotSteps909)), 0, kPotSteps909); }
inline uint64_t PackPots909(Voice909 v, const double* pots)
{
  uint64_t p = 0; const int n = PotCount909(v);
  for (int i = 0; i < n; ++i) p |= static_cast<uint64_t>(PotQuantise909(pots[i]) ^ kPotNominalQ909) << (8 * i);
  return p;
}
// Positions 0..1 of the pots of the voice; pots must hold PotCount909(v) values.
inline void UnpackPots909(Voice909 v, uint64_t params, double* pots)
{
  const int n = PotCount909(v);
  for (int i = 0; i < n; ++i) pots[i] = static_cast<double>(static_cast<int>((params >> (8 * i)) & 0xFF) ^ kPotNominalQ909) / kPotSteps909;
}

}  // namespace dgg
