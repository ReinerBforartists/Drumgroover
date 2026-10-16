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
inline TR808RideVoice::Articulation RideArticulationForPool(int poolIndex)
{
  static const TR808RideVoice::Articulation kArts[6] = {
    TR808RideVoice::Articulation::BowTip, TR808RideVoice::Articulation::BowShank,
    TR808RideVoice::Articulation::BellTip, TR808RideVoice::Articulation::BellShank,
    TR808RideVoice::Articulation::Edge, TR808RideVoice::Articulation::Mute
  };
  return kArts[std::clamp(poolIndex, 0, 5)];
}

inline TR808RideVoice::Articulation CymbalArticulationForPool(int poolIndex)
{
  return (poolIndex % 2) != 0 ? TR808RideVoice::Articulation::Mute : TR808RideVoice::Articulation::Crash;
}

// Pool order: ClosedEdge, TightEdge, OpenEdge0, OpenEdge4, ClosedTip, TightTip.
inline HiHat808Voice::CircuitFamily HiHatPoolFamily(int poolIndex)
{
  return (poolIndex == 2 || poolIndex == 3) ? HiHat808Voice::CircuitFamily::Open : HiHat808Voice::CircuitFamily::Closed;
}

// The key of the set a voice currently plays -- the one place every cache
// request (pattern sync, knob release, clear-cache reload) builds its key
// from a live voice.
inline TR808VariantKey128 RideVoiceKey(TR808RideVoice::Articulation art, const TR808RideVoice& v, double sampleRateHz)
{
  return TR808RideVoice::MakeVariantPoolKey(static_cast<int>(art), v.decayKnob01, v.toneKnob, v.levelKnob, v.pitchRatio, sampleRateHz);
}

inline TR808VariantKey128 HiHatVoiceKey(int poolIndex, const HiHat808Voice& v, double sampleRateHz)
{
  return HiHat808Voice::MakeVariantPoolKey(HiHat808Voice::FamilyKey(HiHatPoolFamily(poolIndex)), poolIndex,
    v.decayKnob01, v.levelKnob01, v.pitchRatio, sampleRateHz);
}

// The key of a factory-default set.
inline TR808VariantKey128 FactoryDefaultKey(const FactoryDefaultEntry& e, double sampleRateHz)
{
  if (e.kind == 1)
    return HiHat808Voice::MakeVariantPoolKey(HiHat808Voice::FamilyKey(HiHatPoolFamily(e.index)), e.index, e.decay, e.level, e.pitch, sampleRateHz);
  const auto art = (e.kind == 0) ? RideArticulationForPool(e.index) : CymbalArticulationForPool(e.index);
  return TR808RideVoice::MakeVariantPoolKey(static_cast<int>(art), e.decay, e.tone, e.level, e.pitch, sampleRateHz);
}

inline std::string FactoryDefaultBaseName(const FactoryDefaultEntry& e, double sampleRateHz)
{
  const TR808VariantKey128 key = FactoryDefaultKey(e, sampleRateHz);
  return e.kind == 1 ? HiHat808Voice::VariantPoolBaseName(key) : TR808RideVoice::VariantPoolBaseName(key);
}

// Makes a voice's knobs exactly the factory default (the UI's default
// tables are the single source of these numbers).
inline void ApplyFactoryDefaultToVoice(TR808RideVoice& v, const FactoryDefaultEntry& e)
{
  v.decayKnob01 = e.decay; v.toneKnob = e.tone; v.levelKnob = e.level; v.pitchRatio = e.pitch;
}

inline void ApplyFactoryDefaultToVoice(HiHat808Voice& v, const FactoryDefaultEntry& e)
{
  v.decayKnob01 = e.decay; v.levelKnob01 = e.level; v.pitchRatio = e.pitch;
}

}  // namespace dgg
