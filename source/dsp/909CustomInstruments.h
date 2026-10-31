// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Reiner Prokein
#pragma once
// The TR-909 custom instruments: Hand Clap 909. It plays the 909 voice from the baked sets (909Playback.h) in every engine mode. The 909
// circuits run at their fixed knob positions; the card has the Output Level (the pot VR20, a gain on the baked set), the digital Pitch (the baked
// set is read at another rate) and the Volume (a gain after the voice, shared by all custom instruments).
#include "CustomInstruments.h"
#include "909Playback.h"

namespace dgg {

constexpr ControlDef kHandClap909Controls[] = {
  {"levelKnob", "Level", "Output", "Output level (VR20): the gain of the output stage.", 0.0, 1.0, 0.01},
  {"pitchRatio", "Pitch", "Pitch", "Transposes the sound, digitally (not a circuit knob).", 0.67, 1.5, 0.01},
};
constexpr DefaultValue kHandClap909Defaults[] = { {"levelKnob", 0.5}, {"pitchRatio", 1.0}, {"volume", 1.0} };

inline constexpr CustomInstrumentDef kCustomHandClap909 = {"handClap909", "Hand Clap 909", "TR-909", CustomKind::Tr909, (int)Tr909Voice::Hc, MixerChannel::Custom,
  kHandClap909Controls, (int)(sizeof(kHandClap909Controls) / sizeof(ControlDef)), kHandClap909Defaults, (int)(sizeof(kHandClap909Defaults) / sizeof(DefaultValue)),
  [] { return nlohmann::json{ {"kind", "Tr909"}, {"voice", Tr909VoiceName(Tr909Voice::Hc)} }; }};

}  // namespace dgg
