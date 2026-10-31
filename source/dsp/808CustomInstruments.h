// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Reiner Prokein
#pragma once
// The TR-808 custom instruments: High Conga, Claves, Hand Clap, Maracas, Cowbell, PC-2. Each runs on its 808 circuit voice (the kind names the
// pool the host plays it on). Their knobs reuse the kit table's schemas where the circuit sheet is the same (808InstrumentTable.h).
#include "CustomInstruments.h"
#include "808InstrumentTable.h"

namespace dgg {

// Custom instruments carry a "volume": the default of their Volume knob in the Drum Engine tab (a linear gain after the voice, 1.0 = unity).
constexpr DefaultValue kClavesDefaults[] = { {"levelKnob", 0.7}, {"pitchRatio", 1.0}, {"volume", 1.0} };
// Hand Clap / Maracas (HC sheet): the level pot VR17 (deck default 0.75) and the digital transpose.
constexpr DefaultValue kHcDefaults[] = { {"levelKnob", 0.5}, {"pitchRatio", 1.0}, {"volume", 1.0} };
constexpr DefaultValue kMaracasDefaults[] = { {"levelKnob", 0.7}, {"pitchRatio", 1.0}, {"volume", 1.0} };
constexpr DefaultValue kCongaDefaults[] = { {"tuningKnob", 0.5}, {"levelKnob", 0.7}, {"pitchRatio", 1.0}, {"volume", 1.0} };
constexpr DefaultValue kCbDefaults[] = { {"levelKnob", 0.7}, {"pitchRatio", 1.0}, {"volume", 1.0} };
// PC-2 (sheet PC-2): the six pots and the two switches of the sheet. The defaults give about 330 Hz with a light square-wave LFO and a long decay. The pot centre (all pots at 0.5, both switches in throw O) stalls the oscillator in one LFO state (57 Hz).
constexpr ControlDef kPc2Controls[] = {
  {"pitchKnob", "Pitch", "Oscillator", "Pitch of the oscillator (VR18).", 0.0, 1.0, 0.01},
  {"vcoWaveKnob", "Wave", "Oscillator", "Oscillator waveform (SW3): 1 triangle, 0 square.", 0.0, 1.0, 1.0},
  {"sweepKnob", "Sweep", "Oscillator", "Pitch sweep after the trigger (VR20), centre = none.", 0.0, 1.0, 0.01},
  {"decayKnob", "Decay", "Envelope", "Decay of the sound (VR19).", 0.0, 1.0, 0.01},
  {"lfoRateKnob", "Rate", "LFO", "LFO rate (VR2).", 0.0, 1.0, 0.01},
  {"lfoDepthKnob", "Depth", "LFO", "LFO depth (VR3).", 0.0, 1.0, 0.01},
  {"lfoWaveKnob", "Wave", "LFO", "LFO waveform (SW2): 1 square, 0 triangle.", 0.0, 1.0, 1.0},
  {"levelKnob", "Level", "Output", "Output level (VR1).", 0.0, 1.0, 0.01},
};
constexpr DefaultValue kPc2Defaults[] = { {"levelKnob", 0.7}, {"lfoRateKnob", 0.2}, {"lfoDepthKnob", 0.16}, {"pitchKnob", 0.49}, {"decayKnob", 0.97}, {"sweepKnob", 0.5}, {"lfoWaveKnob", 1.0}, {"vcoWaveKnob", 0.0}, {"volume", 1.0} };

inline nlohmann::json Custom808EngineJson(const char* kind, nlohmann::json extra = nlohmann::json::object())
{
  extra["kind"] = kind;
  return extra;
}

inline constexpr CustomInstrumentDef kCustomHighConga808 = {"highConga808", "High Conga 808", "TR-808", CustomKind::Conga, 2, MixerChannel::Custom,
  kTomControls, (int)(sizeof(kTomControls) / sizeof(ControlDef)), kCongaDefaults, (int)(sizeof(kCongaDefaults) / sizeof(DefaultValue)),
  [] { return Custom808EngineJson("Tom", { {"sheet", 2}, {"conga", true}, {"articulation", "Center"} }); }};
inline constexpr CustomInstrumentDef kCustomClaves808 = {"claves808", "Claves 808", "TR-808", CustomKind::Claves, 0, MixerChannel::Custom,
  kSnareRmControls, (int)(sizeof(kSnareRmControls) / sizeof(ControlDef)), kClavesDefaults, (int)(sizeof(kClavesDefaults) / sizeof(DefaultValue)),
  [] { return Custom808EngineJson("RmClaves", { {"claves", true} }); }};
inline constexpr CustomInstrumentDef kCustomHandClap808 = {"handClap808", "Hand Clap 808", "TR-808", CustomKind::HandClap, 0, MixerChannel::Custom,
  kSnareRmControls, (int)(sizeof(kSnareRmControls) / sizeof(ControlDef)), kHcDefaults, (int)(sizeof(kHcDefaults) / sizeof(DefaultValue)),
  [] { return Custom808EngineJson("HcClap", { {"clap", true} }); }};
inline constexpr CustomInstrumentDef kCustomMaracas808 = {"maracas808", "Maracas 808", "TR-808", CustomKind::Maracas, 0, MixerChannel::Custom,
  kSnareRmControls, (int)(sizeof(kSnareRmControls) / sizeof(ControlDef)), kMaracasDefaults, (int)(sizeof(kMaracasDefaults) / sizeof(DefaultValue)),
  [] { return Custom808EngineJson("HcMaracas", { {"clap", false} }); }};
inline constexpr CustomInstrumentDef kCustomCowbell808 = {"cowbell808", "Cowbell 808", "TR-808", CustomKind::Cowbell, 0, MixerChannel::Custom,
  kSnareRmControls, (int)(sizeof(kSnareRmControls) / sizeof(ControlDef)), kCbDefaults, (int)(sizeof(kCbDefaults) / sizeof(DefaultValue)),
  [] { return Custom808EngineJson("Cowbell"); }};
inline constexpr CustomInstrumentDef kCustomPc2808 = {"pc2808", "PC-2 808", "TR-808", CustomKind::Pc2, 0, MixerChannel::Custom,
  kPc2Controls, (int)(sizeof(kPc2Controls) / sizeof(ControlDef)), kPc2Defaults, (int)(sizeof(kPc2Defaults) / sizeof(DefaultValue)),
  [] { return Custom808EngineJson("Pc2"); }};

}  // namespace dgg
