// TR-909 fast voices: the sets that are baked at the start and protected from eviction.
//
// Per voice the accent codes of all velocity levels (Tr909CodeForVelocity), so that every hit finds its set once the start bake is done.
// Knob state and noise path are those of the models (fixed knob positions, 5 us reference source).
#pragma once
#include "909Playback.h"
#include <set>
#include <vector>

namespace dgg {

inline std::vector<Tr909SetKey> Tr909FactoryDefaultKeys(int sampleRateHz)
{
  std::vector<Tr909SetKey> out;
  for (int v = 0; v < static_cast<int>(Tr909Voice::kCount); ++v)
  {
    std::set<int> codes;
    for (int level = 0; level < kTr909VelocityLevels; ++level) codes.insert(Tr909CodeForVelocity(static_cast<Tr909Voice>(v), static_cast<double>(level) / (kTr909VelocityLevels - 1)));
    for (int code : codes)
    {
      Tr909SetKey k; k.voice = static_cast<Tr909Voice>(v); k.params = 0; k.accentCode = code;
      k.noise = Tr909NoisePath::Reference5us; k.sampleRateHz = sampleRateHz;
      out.push_back(k);
    }
  }
  return out;
}

}  // namespace dgg
