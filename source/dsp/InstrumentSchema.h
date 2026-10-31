// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Reiner Prokein
#pragma once
// The pieces that every instrument definition shares: the knob schema (ControlDef), the default values, and the helpers on them. The kit
// instrument table (808InstrumentTable.h) and the custom instruments (CustomInstruments.h) both build on this; neither depends on the other.
#include "Mixer.h"

namespace dgg {

// One knob of a card: group = the title of the knob group it sits in, in the order of the array.
struct ControlDef { const char* key; const char* label; const char* group; const char* tooltip; double min, max, step; };

struct DefaultValue { const char* key; double value; };

constexpr bool StrEq(const char* a, const char* b)
{
  while (*a && *a == *b) { ++a; ++b; }
  return *a == *b;
}

inline double DefaultOfValues(const DefaultValue* defaults, int numDefaults, const char* key, double fallback = 0.0)
{
  for (int i = 0; i < numDefaults; ++i) if (StrEq(defaults[i].key, key)) return defaults[i].value;
  return fallback;
}

inline const char* MixerChannelName(MixerChannel c)
{
  static const char* kNames[] = {"Kick", "Snare", "HiHat", "RackTom1", "RackTom2", "RackTom3", "FloorTom1", "FloorTom2",
                                 "Cymbal1", "Cymbal2", "Cymbal3", "Cymbal4", "Cymbal5", "Ride", "Custom"};
  return kNames[static_cast<int>(c)];
}

}  // namespace dgg
