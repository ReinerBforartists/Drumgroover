// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Reiner Prokein
#pragma once
// The TR-909 custom instruments: Hand Clap 909. It plays the 909 voice from the baked sets (909Playback.h) in every engine mode. The 909
// circuits run at the pot positions of the card; the card has the Output Level (the pot VR20, a circuit pot), the digital Pitch (the baked
// set is read at another rate) and the Volume (a gain after the voice, shared by all custom instruments).
#include "CustomInstruments.h"
#include "909Playback.h"

namespace dgg {

constexpr ControlDef kHandClap909Controls[] = {
  {"level909", "Level", "Output", "Output level (VR20), a pot of the circuit.", 0.0, 1.0, 0.01},
  {"pitchRatio", "Pitch", "Pitch", "Transposes the sound, digitally (not a circuit knob).", 0.67, 1.5, 0.01},
};
constexpr DefaultValue kHandClap909Defaults[] = { {"level909", 0.5}, {"pitchRatio", 1.0}, {"volume", 1.0} };

inline constexpr CustomInstrumentDef kCustomHandClap909 = {"handClap909", "Hand Clap 909", "TR-909", CustomKind::Tr909, (int)Tr909Voice::Hc, MixerChannel::Custom,
  kHandClap909Controls, (int)(sizeof(kHandClap909Controls) / sizeof(ControlDef)), kHandClap909Defaults, (int)(sizeof(kHandClap909Defaults) / sizeof(DefaultValue)),
  [] { return nlohmann::json{ {"kind", "Tr909"}, {"voice", Tr909VoiceName(Tr909Voice::Hc)} }; }};

// Floor Tom 3 909: the sixth tom position. The kit table has five free tom slots (Rack Tom 1-3, Floor Tom 1-2), so the sixth one is a custom instrument, as the High Conga
// is for the 808 (sheet 2; the 909 has no conga, only the three toms LT, MT, HT). It plays the 909 low tom (LT) from the baked sets, pitched 6 semitones down (the floor toms are the low tom pitched down); same card controls as the Hand Clap 909.
constexpr ControlDef kFloorTom3_909Controls[] = {
  {"tune909", "Tune", "Circuit", "Tuning of the drum.", 0.0, 1.0, 0.01},
  {"decay909", "Decay", "Circuit", "Decay of the drum.", 0.0, 1.0, 0.01},
  {"level909", "Level", "Output", "Output level (VR12), a pot of the circuit.", 0.0, 1.0, 0.01},
  {"pitchRatio", "Pitch", "Pitch", "Transposes the drum, digitally (not a circuit knob).", 0.67, 1.5, 0.01},
};
constexpr DefaultValue kFloorTom3_909Defaults[] = { {"tune909", 0.5}, {"decay909", 0.5}, {"level909", 0.5}, {"pitchRatio", 1.0}, {"volume", 1.0} };

inline constexpr CustomInstrumentDef kCustomFloorTom3_909 = {"floorTom3_909", "Floor Tom 3 909", "TR-909", CustomKind::Tr909, (int)Tr909Voice::Lt, MixerChannel::Custom,
  kFloorTom3_909Controls, (int)(sizeof(kFloorTom3_909Controls) / sizeof(ControlDef)), kFloorTom3_909Defaults, (int)(sizeof(kFloorTom3_909Defaults) / sizeof(DefaultValue)),
  [] { return nlohmann::json{ {"kind", "Tr909"}, {"voice", Tr909VoiceName(Tr909Voice::Lt)} }; }};

// Fixed transposition of a 909 custom instrument (1.0 for all but the Floor Tom 3 909).
inline double Tr909CustomBasePitch(const CustomInstrumentDef& d) { return StrEq(d.id, "floorTom3_909") ? 0.7071067811865476 : 1.0; }

}  // namespace dgg
