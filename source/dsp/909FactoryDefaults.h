// TR-909 fast voices: the sets that are baked at the start and protected from eviction.
//
// Per voice the accent codes of all velocity levels (Tr909CodeForVelocity), so that every hit finds its set once the start bake is done.
// Knob state and noise path are those of the models (fixed knob positions, 5 us reference source).
#pragma once
#include "909Playback.h"
#include <set>
#include <vector>

namespace dgg {

// The sets of one voice at one pot state (909PotParams.h): the accent codes of all velocity levels.
inline std::vector<Tr909SetKey> Tr909KeysForParams(Tr909Voice voice, uint64_t params, int sampleRateHz)
{
  std::vector<Tr909SetKey> out;
  std::set<int> codes;
  for (int level = 0; level < kTr909VelocityLevels; ++level) codes.insert(Tr909CodeForVelocity(voice, static_cast<double>(level) / (kTr909VelocityLevels - 1)));
  for (int code : codes)
  {
    Tr909SetKey k; k.voice = voice; k.params = params; k.accentCode = code;
    k.noise = Tr909NoisePath::Reference5us; k.sampleRateHz = sampleRateHz;
    out.push_back(k);
  }
  return out;
}

inline std::vector<Tr909SetKey> Tr909FactoryDefaultKeys(int sampleRateHz)
{
  std::vector<Tr909SetKey> out;
  for (int v = 0; v < static_cast<int>(Tr909Voice::kCount); ++v)
  {
    const std::vector<Tr909SetKey> keys = Tr909KeysForParams(static_cast<Tr909Voice>(v), 0, sampleRateHz);
    out.insert(out.end(), keys.begin(), keys.end());
  }
  return out;
}

}  // namespace dgg
