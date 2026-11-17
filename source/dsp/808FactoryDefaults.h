// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Reiner Prokein
#pragma once
#include "808RideVoice.h"
#include "808HiHatVoice.h"
#include <algorithm>
#include <string>

// Factory-default settings for the Ride/HiHat/Cymbal pools and the cache keys derived from them. The values come
// from the instrument table (808InstrumentTable.h, the only place defaults are stored); this header holds pool
// ordering, key construction and applying a default to a voice.
namespace dgg
{

// kind: 0 = ride (index 0..5), 1 = hihat (index 0..5),
//       2 = cymbal (index 0..9, even = Crash, odd = Mute).
// `tone` is unused for hihat.
struct FactoryDefaultEntry { int kind; int index; double decay, tone, level, pitch; };

// Pool order: BowTip, BowShank, BellTip, BellShank, Edge, Mute.
inline RideVoice808::Articulation RideArticulationForPool(int poolIndex)
{
  static const RideVoice808::Articulation kArts[6] = {
    RideVoice808::Articulation::BowTip, RideVoice808::Articulation::BowShank,
    RideVoice808::Articulation::BellTip, RideVoice808::Articulation::BellShank,
    RideVoice808::Articulation::Edge, RideVoice808::Articulation::Mute
  };
  return kArts[std::clamp(poolIndex, 0, 5)];
}

inline RideVoice808::Articulation CymbalArticulationForPool(int poolIndex)
{
  return (poolIndex % 2) != 0 ? RideVoice808::Articulation::Mute : RideVoice808::Articulation::Crash;
}

// Pool order: ClosedEdge, TightEdge, OpenEdge0, OpenEdge4, ClosedTip, TightTip.
inline HiHat808Voice::CircuitFamily HiHatPoolFamily(int poolIndex)
{
  return (poolIndex == 2 || poolIndex == 3) ? HiHat808Voice::CircuitFamily::Open : HiHat808Voice::CircuitFamily::Closed;
}

// The key of the set a voice currently plays -- the one place every cache
// request (pattern sync, knob release, clear-cache reload) builds its key
// from a live voice.
inline VariantKey128808 RideVoiceKey(RideVoice808::Articulation art, const RideVoice808& v, double sampleRateHz, int accentStep = 0)
{
  return RideVoice808::MakeVariantPoolKey(static_cast<int>(art), v.decayKnob01, v.toneKnob, v.levelKnob, v.pitchRatio, sampleRateHz, accentStep);
}

inline VariantKey128808 HiHatVoiceKey(int poolIndex, const HiHat808Voice& v, double sampleRateHz, int accentStep = 0)
{
  return HiHat808Voice::MakeVariantPoolKey(HiHat808Voice::FamilyKey(HiHatPoolFamily(poolIndex)), poolIndex,
    v.decayKnob01, v.levelKnob01, v.pitchRatio, sampleRateHz, accentStep);
}

// The key of a factory-default set.
inline VariantKey128808 FactoryDefaultKey(const FactoryDefaultEntry& e, double sampleRateHz)
{
  if (e.kind == 1)
    return HiHat808Voice::MakeVariantPoolKey(HiHat808Voice::FamilyKey(HiHatPoolFamily(e.index)), e.index, e.decay, e.level, e.pitch, sampleRateHz);
  const auto art = (e.kind == 0) ? RideArticulationForPool(e.index) : CymbalArticulationForPool(e.index);
  return RideVoice808::MakeVariantPoolKey(static_cast<int>(art), e.decay, e.tone, e.level, e.pitch, sampleRateHz);
}

inline std::string FactoryDefaultBaseName(const FactoryDefaultEntry& e, double sampleRateHz)
{
  const VariantKey128808 key = FactoryDefaultKey(e, sampleRateHz);
  return e.kind == 1 ? HiHat808Voice::VariantPoolBaseName(key) : RideVoice808::VariantPoolBaseName(key);
}

// Makes a voice's knobs exactly the factory default (the UI's default
// tables are the single source of these numbers).
inline void ApplyFactoryDefaultToVoice(RideVoice808& v, const FactoryDefaultEntry& e)
{
  v.decayKnob01 = e.decay; v.toneKnob = e.tone; v.levelKnob = e.level; v.pitchRatio = e.pitch;
}

inline void ApplyFactoryDefaultToVoice(HiHat808Voice& v, const FactoryDefaultEntry& e)
{
  v.decayKnob01 = e.decay; v.levelKnob01 = e.level; v.pitchRatio = e.pitch;
}

}  // namespace dgg
