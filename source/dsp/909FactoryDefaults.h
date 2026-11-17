// TR-909 fast voices: the sets that are baked at the start and protected from eviction.
//
// Per voice the accent codes of all velocity levels (CodeForVelocity909), so that every hit finds its set once the start bake is done.
// Knob state and noise path are those of the models (fixed knob positions, 5 us reference source).
#pragma once
#include "909Playback.h"
#include <set>
#include <vector>

namespace dgg {

// The sets of one voice at one pot state (909PotParams.h): the accent codes of all velocity levels.
inline std::vector<SetKey909> KeysForParams909(Voice909 voice, uint64_t params, int sampleRateHz)
{
  std::vector<SetKey909> out;
  std::set<int> codes;
  for (int level = 0; level < kVelocityLevels909; ++level) codes.insert(CodeForVelocity909(voice, static_cast<double>(level) / (kVelocityLevels909 - 1)));
  for (int code : codes)
  {
    SetKey909 k; k.voice = voice; k.params = params; k.accentCode = code;
    k.noise = NoisePath909::Reference5us; k.sampleRateHz = sampleRateHz;
    out.push_back(k);
  }
  return out;
}

inline std::vector<SetKey909> FactoryDefaultKeys909(int sampleRateHz)
{
  std::vector<SetKey909> out;
  for (int v = 0; v < static_cast<int>(Voice909::kCount); ++v)
  {
    const std::vector<SetKey909> keys = KeysForParams909(static_cast<Voice909>(v), 0, sampleRateHz);
    out.insert(out.end(), keys.begin(), keys.end());
  }
  return out;
}

}  // namespace dgg
