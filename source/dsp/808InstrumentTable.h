#pragma once
// The instrument table: one row per instrument instance (every articulation is its own instance) with the metadata the host and
// the UI share -- identity, GM note, engine kind and arguments, mixer channel, knob schema and default values. The host reads its
// pools, triggers, bakes and default cache sets from it; the UI receives it once as the 'instrumentTable' bridge message and builds
// its cards, knobs and defaults from it. Engine implementation (keys, pools, bake, choke) is not part of the table.
//
// The JSON of InstrumentTableToJson() is the exact shape the UI consumes.
#include "Mixer.h"
#include <array>
#if __has_include(<nlohmann/json.hpp>)
#include <nlohmann/json.hpp>
#else
#include "json.hpp"   // the plugin build has iPlug2's Extras/nlohmann on its include path
#endif

namespace dgg {

enum class EngineKind { Kick, SnareSd, RmRim, RmClaves, HcClap, HcMaracas, Tom, HiHat, Ride, Cymbal, Cowbell, Pc2 };

// One knob of a card: group = the title of the knob group it sits in, in the order of the array.
struct ControlDef { const char* key; const char* label; const char* group; const char* tooltip; double min, max, step; };

struct DefaultValue { const char* key; double value; };

struct InstrumentDef
{
  const char* id;          // track id (custom instrument: its registry id)
  const char* name;        // Drum Engine card / keyboard label
  const char* trackName;   // track roster / MIDI export name (custom: nullptr)
  const char* family;      // voiceParams family, together with paramIndex the 'family:index' key of saved projects
  int paramIndex;
  const char* uiFamily;    // colour / legend family of the keyboard
  int gmNote;              // -1: assigned per track (custom instruments)
  EngineKind kind;
  // engine arguments; only those of `kind` are used
  int sheet;               // Tom: 0 LT, 1 MT, 2 HT
  bool conga;              // Tom: Tom/Conga switch in the Conga position
  const char* articulation;// Tom: Center/RimShot/RimOnly; Ride: BowTip..Mute; Cymbal: Crash/Mute
  const char* circuitFamily;// HiHat: Open/Closed
  int poolArtId;           // HiHat: variantPoolArticulationId
  int cymbalPos;           // Cymbal: 0..4
  bool isMute;             // Cymbal
  MixerChannel channel;
  int poolSlot;            // index into the pool array of the engine kind
  const ControlDef* controls; int numControls;
  const DefaultValue* defaults; int numDefaults;
};

constexpr ControlDef kSnareSdControls[] = {
  {"toneKnob", "Tone", "Circuit", "Tone color.", 0.0, 1.0, 0.01},
  {"snappyKnob", "Snappy", "Circuit", "Amount of the snare rattle.", 0.0, 1.0, 0.01},
  {"levelKnob", "Level", "Output", "Output level.", 0.0, 1.0, 0.01},
  {"pitchRatio", "Pitch", "Pitch", "Transposes the snare, digitally (not a circuit knob).", 0.67, 1.5, 0.01},
};
constexpr ControlDef kSnareRmControls[] = {
  {"levelKnob", "Level", "Output", "Output level.", 0.0, 1.0, 0.01},
  {"pitchRatio", "Pitch", "Pitch", "Transposes the sound, digitally (not a circuit knob).", 0.67, 1.5, 0.01},
};
constexpr DefaultValue kSnareDefaults0[] = { {"toneKnob", 0.5}, {"snappyKnob", 0.5}, {"levelKnob", 0.7}, {"pitchRatio", 1.0} };
constexpr DefaultValue kSnareDefaults1[] = { {"toneKnob", 0.5}, {"snappyKnob", 0.3}, {"levelKnob", 0.7}, {"pitchRatio", 1.0} };
constexpr DefaultValue kSnareDefaults2[] = { {"toneKnob", 1}, {"snappyKnob", 0.4}, {"levelKnob", 0.55}, {"pitchRatio", 1.0} };
constexpr DefaultValue kSnareDefaults3[] = { {"levelKnob", 0.7}, {"pitchRatio", 1.0} };
constexpr DefaultValue kSnareDefaults4[] = { {"toneKnob", 1}, {"snappyKnob", 0.7}, {"levelKnob", 0.55}, {"pitchRatio", 1.0} };
constexpr DefaultValue kSnareDefaults5[] = { {"levelKnob", 0.4}, {"pitchRatio", 1.1} };

constexpr ControlDef kTomControls[] = {
  {"tuningKnob", "Tuning", "Circuit", "Pitch of the drum.", 0.0, 1.0, 0.01},
  {"levelKnob", "Level", "Output", "Output level.", 0.0, 1.0, 0.01},
  {"pitchRatio", "Pitch", "Pitch", "Transposes the drum, digitally (not a circuit knob).", 0.67, 1.5, 0.01},
};
constexpr DefaultValue kTomDefaults0[] = { {"tuningKnob", 0.5}, {"levelKnob", 0.7}, {"pitchRatio", 1.0} };
constexpr DefaultValue kTomDefaults1[] = { {"tuningKnob", 0.5}, {"levelKnob", 0.55}, {"pitchRatio", 1.0} };
constexpr DefaultValue kTomDefaults2[] = { {"tuningKnob", 0.5}, {"levelKnob", 0.4}, {"pitchRatio", 1.0} };
constexpr DefaultValue kTomDefaults3[] = { {"tuningKnob", 0.5}, {"levelKnob", 0.7}, {"pitchRatio", 1.0} };
constexpr DefaultValue kTomDefaults4[] = { {"tuningKnob", 0.5}, {"levelKnob", 0.55}, {"pitchRatio", 1.0} };
constexpr DefaultValue kTomDefaults5[] = { {"tuningKnob", 0.5}, {"levelKnob", 0.4}, {"pitchRatio", 1.0} };
constexpr DefaultValue kTomDefaults6[] = { {"tuningKnob", 0.5}, {"levelKnob", 0.7}, {"pitchRatio", 1.0} };
constexpr DefaultValue kTomDefaults7[] = { {"tuningKnob", 0.5}, {"levelKnob", 0.55}, {"pitchRatio", 1.0} };
constexpr DefaultValue kTomDefaults8[] = { {"tuningKnob", 0.5}, {"levelKnob", 0.4}, {"pitchRatio", 1.0} };
constexpr DefaultValue kTomDefaults9[] = { {"tuningKnob", 0.5}, {"levelKnob", 0.7}, {"pitchRatio", 1.0} };
constexpr DefaultValue kTomDefaults10[] = { {"tuningKnob", 0.5}, {"levelKnob", 0.55}, {"pitchRatio", 1.0} };
constexpr DefaultValue kTomDefaults11[] = { {"tuningKnob", 0.5}, {"levelKnob", 0.4}, {"pitchRatio", 1.0} };
constexpr DefaultValue kTomDefaults12[] = { {"tuningKnob", 0.5}, {"levelKnob", 0.7}, {"pitchRatio", 1.0} };
constexpr DefaultValue kTomDefaults13[] = { {"tuningKnob", 0.5}, {"levelKnob", 0.55}, {"pitchRatio", 1.0} };
constexpr DefaultValue kTomDefaults14[] = { {"tuningKnob", 0.5}, {"levelKnob", 0.4}, {"pitchRatio", 1.0} };

constexpr ControlDef kKickControls[] = {
  {"toneKnob", "Tone", "Circuit", "Tone color.", 0.0, 1.0, 0.01},
  {"decayKnob01", "Decay", "Circuit", "Ring-out length.", 0.0, 1.0, 0.01},
  {"levelKnob", "Level", "Output", "Output level.", 0.0, 1.0, 0.01},
  {"pitchRatio", "Pitch", "Pitch", "Transposes the kick, digitally (not a circuit knob).", 0.67, 1.5, 0.01},
};
constexpr DefaultValue kKickDefaults0[] = { {"decayKnob01", 0.5}, {"toneKnob", 0.5}, {"levelKnob", 0.7}, {"pitchRatio", 1.0} };
constexpr DefaultValue kKickDefaults1[] = { {"decayKnob01", 0.2}, {"toneKnob", 0.5}, {"levelKnob", 0.5}, {"pitchRatio", 1.0} };

constexpr ControlDef kHiHatOpenControls[] = {
  {"level", "Level", "Circuit", "OH/CH output level (VR7/VR8).", 0.0, 1.0, 0.01},
  {"decayKnob01", "Decay", "Circuit", "Open-hat decay length (VR3). No effect on Closed, as on the real front panel.", 0.0, 1.0, 0.01},
  {"pitchRatio", "Pitch", "Metal Bank", "Transposes the whole metal bank.", 0.67, 1.5, 0.01},
};
constexpr ControlDef kHiHatClosedControls[] = {
  {"level", "Level", "Circuit", "CH output level (VR8).", 0.0, 1.0, 0.01},
  {"pitchRatio", "Pitch", "Metal Bank", "Transposes the whole metal bank.", 0.67, 1.5, 0.01},
};
constexpr DefaultValue kHiHatDefaults0[] = { {"level", 0.7}, {"decayKnob01", 0.5}, {"pitchRatio", 1.0} };
constexpr DefaultValue kHiHatDefaults1[] = { {"level", 0.55}, {"decayKnob01", 0.5}, {"pitchRatio", 1.1} };
constexpr DefaultValue kHiHatDefaults2[] = { {"level", 0.7}, {"decayKnob01", 0}, {"pitchRatio", 1.0} };
constexpr DefaultValue kHiHatDefaults3[] = { {"level", 0.55}, {"decayKnob01", 1.0}, {"pitchRatio", 1.0} };
constexpr DefaultValue kHiHatDefaults4[] = { {"level", 0.55}, {"decayKnob01", 0.5}, {"pitchRatio", 1.0} };
constexpr DefaultValue kHiHatDefaults5[] = { {"level", 0.4}, {"decayKnob01", 0.5}, {"pitchRatio", 1.0} };

constexpr ControlDef kCymbalControls[] = {
  {"toneKnob", "Tone", "Circuit", "Tone color.", 0.0, 1.0, 0.01},
  {"decayKnob01", "Decay", "Circuit", "Ring-out length.", 0.0, 1.0, 0.01},
  {"pitchRatio", "Pitch", "Metal Bank", "Transposes the whole metal bank.", 0.67, 1.5, 0.01},
  {"levelKnob", "Level", "Output", "Overall output gain.", 0.0, 1.0, 0.01},
};
constexpr DefaultValue kCymbalDefaults0[] = {
  {"toneKnob", 0.2}, {"levelKnob", 0.7}, {"spread", 0.0}, {"ht3Scale", 1.0}, {"diagVca1OutScale", 1.0}, {"diagVca2OutScale", 1.0}, {"diagVca3OutScale", 1.0}, {"diagHp2R114Scale", 1.0}, {"level", 0.53}, {"pitchRatio", 0.94}, {"decayKnob01", 1.0}, {"partialFreqsHz0", 205.3}, {"partialFreqsHz1", 369.6}, {"partialFreqsHz2", 304.4}, {"partialFreqsHz3", 522.7}, {"partialFreqsHz4", 757.0}, {"partialFreqsHz5", 550.0}, {"partialLevel0", 0.6}, {"partialLevel1", 0.6}, {"partialLevel2", 0.6}, {"partialLevel3", 0.6}, {"partialLevel4", 1.4}, {"partialLevel5", 1.4}
};
constexpr DefaultValue kCymbalDefaults1[] = {
  {"toneKnob", 0.2}, {"levelKnob", 0.55}, {"spread", 0.0}, {"ht3Scale", 1.0}, {"diagVca1OutScale", 1.0}, {"diagVca2OutScale", 1.0}, {"diagVca3OutScale", 1.0}, {"diagHp2R114Scale", 1.0}, {"level", 0.53}, {"pitchRatio", 0.94}, {"decayKnob01", 0.0}, {"partialFreqsHz0", 205.3}, {"partialFreqsHz1", 369.6}, {"partialFreqsHz2", 304.4}, {"partialFreqsHz3", 522.7}, {"partialFreqsHz4", 757.0}, {"partialFreqsHz5", 550.0}, {"partialLevel0", 0.6}, {"partialLevel1", 0.6}, {"partialLevel2", 0.6}, {"partialLevel3", 0.6}, {"partialLevel4", 1.4}, {"partialLevel5", 1.4}
};
constexpr DefaultValue kCymbalDefaults2[] = {
  {"toneKnob", 0.2}, {"levelKnob", 0.7}, {"spread", 0.0}, {"ht3Scale", 1.0}, {"diagVca1OutScale", 1.0}, {"diagVca2OutScale", 1.0}, {"diagVca3OutScale", 1.0}, {"diagHp2R114Scale", 1.0}, {"level", 0.53}, {"pitchRatio", 0.97}, {"decayKnob01", 1.0}, {"partialFreqsHz0", 205.3}, {"partialFreqsHz1", 369.6}, {"partialFreqsHz2", 304.4}, {"partialFreqsHz3", 522.7}, {"partialFreqsHz4", 757.0}, {"partialFreqsHz5", 550.0}, {"partialLevel0", 0.6}, {"partialLevel1", 0.6}, {"partialLevel2", 0.6}, {"partialLevel3", 0.6}, {"partialLevel4", 1.4}, {"partialLevel5", 1.4}
};
constexpr DefaultValue kCymbalDefaults3[] = {
  {"toneKnob", 0.2}, {"levelKnob", 0.55}, {"spread", 0.0}, {"ht3Scale", 1.0}, {"diagVca1OutScale", 1.0}, {"diagVca2OutScale", 1.0}, {"diagVca3OutScale", 1.0}, {"diagHp2R114Scale", 1.0}, {"level", 0.53}, {"pitchRatio", 0.97}, {"decayKnob01", 0.0}, {"partialFreqsHz0", 205.3}, {"partialFreqsHz1", 369.6}, {"partialFreqsHz2", 304.4}, {"partialFreqsHz3", 522.7}, {"partialFreqsHz4", 757.0}, {"partialFreqsHz5", 550.0}, {"partialLevel0", 0.6}, {"partialLevel1", 0.6}, {"partialLevel2", 0.6}, {"partialLevel3", 0.6}, {"partialLevel4", 1.4}, {"partialLevel5", 1.4}
};
constexpr DefaultValue kCymbalDefaults4[] = {
  {"toneKnob", 0.2}, {"levelKnob", 0.7}, {"spread", 0.0}, {"ht3Scale", 1.0}, {"diagVca1OutScale", 1.0}, {"diagVca2OutScale", 1.0}, {"diagVca3OutScale", 1.0}, {"diagHp2R114Scale", 1.0}, {"level", 0.53}, {"pitchRatio", 1.0}, {"decayKnob01", 1.0}, {"partialFreqsHz0", 205.3}, {"partialFreqsHz1", 369.6}, {"partialFreqsHz2", 304.4}, {"partialFreqsHz3", 522.7}, {"partialFreqsHz4", 757.0}, {"partialFreqsHz5", 550.0}, {"partialLevel0", 0.6}, {"partialLevel1", 0.6}, {"partialLevel2", 0.6}, {"partialLevel3", 0.6}, {"partialLevel4", 1.4}, {"partialLevel5", 1.4}
};
constexpr DefaultValue kCymbalDefaults5[] = {
  {"toneKnob", 0.2}, {"levelKnob", 0.55}, {"spread", 0.0}, {"ht3Scale", 1.0}, {"diagVca1OutScale", 1.0}, {"diagVca2OutScale", 1.0}, {"diagVca3OutScale", 1.0}, {"diagHp2R114Scale", 1.0}, {"level", 0.53}, {"pitchRatio", 1.0}, {"decayKnob01", 0.0}, {"partialFreqsHz0", 205.3}, {"partialFreqsHz1", 369.6}, {"partialFreqsHz2", 304.4}, {"partialFreqsHz3", 522.7}, {"partialFreqsHz4", 757.0}, {"partialFreqsHz5", 550.0}, {"partialLevel0", 0.6}, {"partialLevel1", 0.6}, {"partialLevel2", 0.6}, {"partialLevel3", 0.6}, {"partialLevel4", 1.4}, {"partialLevel5", 1.4}
};
constexpr DefaultValue kCymbalDefaults6[] = {
  {"toneKnob", 0.2}, {"levelKnob", 0.7}, {"spread", 0.0}, {"ht3Scale", 1.0}, {"diagVca1OutScale", 1.0}, {"diagVca2OutScale", 1.0}, {"diagVca3OutScale", 1.0}, {"diagHp2R114Scale", 1.0}, {"level", 0.53}, {"pitchRatio", 1.03}, {"decayKnob01", 1.0}, {"partialFreqsHz0", 205.3}, {"partialFreqsHz1", 369.6}, {"partialFreqsHz2", 304.4}, {"partialFreqsHz3", 522.7}, {"partialFreqsHz4", 757.0}, {"partialFreqsHz5", 550.0}, {"partialLevel0", 0.6}, {"partialLevel1", 0.6}, {"partialLevel2", 0.6}, {"partialLevel3", 0.6}, {"partialLevel4", 1.4}, {"partialLevel5", 1.4}
};
constexpr DefaultValue kCymbalDefaults7[] = {
  {"toneKnob", 0.2}, {"levelKnob", 0.55}, {"spread", 0.0}, {"ht3Scale", 1.0}, {"diagVca1OutScale", 1.0}, {"diagVca2OutScale", 1.0}, {"diagVca3OutScale", 1.0}, {"diagHp2R114Scale", 1.0}, {"level", 0.53}, {"pitchRatio", 1.03}, {"decayKnob01", 0.0}, {"partialFreqsHz0", 205.3}, {"partialFreqsHz1", 369.6}, {"partialFreqsHz2", 304.4}, {"partialFreqsHz3", 522.7}, {"partialFreqsHz4", 757.0}, {"partialFreqsHz5", 550.0}, {"partialLevel0", 0.6}, {"partialLevel1", 0.6}, {"partialLevel2", 0.6}, {"partialLevel3", 0.6}, {"partialLevel4", 1.4}, {"partialLevel5", 1.4}
};
constexpr DefaultValue kCymbalDefaults8[] = {
  {"toneKnob", 0.2}, {"levelKnob", 0.7}, {"spread", 0.0}, {"ht3Scale", 1.0}, {"diagVca1OutScale", 1.0}, {"diagVca2OutScale", 1.0}, {"diagVca3OutScale", 1.0}, {"diagHp2R114Scale", 1.0}, {"level", 0.53}, {"pitchRatio", 1.06}, {"decayKnob01", 1.0}, {"partialFreqsHz0", 520.0}, {"partialFreqsHz1", 660.0}, {"partialFreqsHz2", 780.0}, {"partialFreqsHz3", 940.0}, {"partialFreqsHz4", 1180.0}, {"partialFreqsHz5", 1420.0}, {"partialLevel0", 0.6}, {"partialLevel1", 0.6}, {"partialLevel2", 0.6}, {"partialLevel3", 0.6}, {"partialLevel4", 1.4}, {"partialLevel5", 1.4}
};
constexpr DefaultValue kCymbalDefaults9[] = {
  {"toneKnob", 0.2}, {"levelKnob", 0.55}, {"spread", 0.0}, {"ht3Scale", 1.0}, {"diagVca1OutScale", 1.0}, {"diagVca2OutScale", 1.0}, {"diagVca3OutScale", 1.0}, {"diagHp2R114Scale", 1.0}, {"level", 0.53}, {"pitchRatio", 1.06}, {"decayKnob01", 0.0}, {"partialFreqsHz0", 520.0}, {"partialFreqsHz1", 660.0}, {"partialFreqsHz2", 780.0}, {"partialFreqsHz3", 940.0}, {"partialFreqsHz4", 1180.0}, {"partialFreqsHz5", 1420.0}, {"partialLevel0", 0.6}, {"partialLevel1", 0.6}, {"partialLevel2", 0.6}, {"partialLevel3", 0.6}, {"partialLevel4", 1.4}, {"partialLevel5", 1.4}
};

constexpr ControlDef kRideControls[] = {
  {"toneKnob", "Tone", "Circuit", "Tone color.", 0.0, 1.0, 0.01},
  {"decayKnob01", "Decay", "Circuit", "Ring-out length.", 0.0, 1.0, 0.01},
  {"pitchRatio", "Pitch", "Metal Bank", "Transposes the whole metal bank.", 0.67, 1.5, 0.01},
  {"levelKnob", "Level", "Output", "Overall output gain.", 0.0, 1.0, 0.01},
};
constexpr DefaultValue kRideDefaults0[] = {
  {"toneKnob", 0.15}, {"levelKnob", 0.7}, {"pitchRatio", 1.0}, {"vca1BiasOhm", 2000000.0}, {"vca2BiasOhm", 2000000.0}, {"vca3BiasOhm", 2000000.0}, {"level", 0.53}, {"decayKnob01", 0.65}
};
constexpr DefaultValue kRideDefaults1[] = {
  {"toneKnob", 0.7}, {"levelKnob", 0.7}, {"pitchRatio", 1.0}, {"vca1BiasOhm", 2000000.0}, {"vca2BiasOhm", 2000000.0}, {"vca3BiasOhm", 2000000.0}, {"level", 0.53}, {"decayKnob01", 1.0}
};
constexpr DefaultValue kRideDefaults2[] = {
  {"toneKnob", 0.85}, {"levelKnob", 0.55}, {"pitchRatio", 1.0}, {"vca1BiasOhm", 2000000.0}, {"vca2BiasOhm", 2000000.0}, {"vca3BiasOhm", 2000000.0}, {"level", 0.53}, {"decayKnob01", 0.25}
};
constexpr DefaultValue kRideDefaults3[] = {
  {"toneKnob", 1.0}, {"levelKnob", 0.55}, {"pitchRatio", 1.0}, {"vca1BiasOhm", 2000000.0}, {"vca2BiasOhm", 2000000.0}, {"vca3BiasOhm", 2000000.0}, {"level", 0.53}, {"decayKnob01", 1.0}
};
constexpr DefaultValue kRideDefaults4[] = {
  {"toneKnob", 0.6}, {"levelKnob", 0.55}, {"pitchRatio", 1.0}, {"vca1BiasOhm", 2000000.0}, {"vca2BiasOhm", 2000000.0}, {"vca3BiasOhm", 2000000.0}, {"level", 0.53}, {"decayKnob01", 0.7}
};
constexpr DefaultValue kRideDefaults5[] = {
  {"toneKnob", 0.8}, {"levelKnob", 0.4}, {"pitchRatio", 1.0}, {"vca1BiasOhm", 2000000.0}, {"vca2BiasOhm", 2000000.0}, {"vca3BiasOhm", 2000000.0}, {"level", 0.53}, {"decayKnob01", 0.1}
};

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

inline constexpr InstrumentDef kInstruments[] = {
  {"kick", "Kick High Stack", "Kick", "kick", 0, "kick", 36, EngineKind::Kick, 0, false, nullptr, nullptr, 0, 0, false, MixerChannel::Kick, 0,
   kKickControls, (int)(sizeof(kKickControls) / sizeof(ControlDef)), kKickDefaults0, (int)(sizeof(kKickDefaults0) / sizeof(DefaultValue))},
  {"kickOpen", "Kick Open", "Open", "kick", 1, "kick", 34, EngineKind::Kick, 0, false, nullptr, nullptr, 0, 0, false, MixerChannel::Kick, 1,
   kKickControls, (int)(sizeof(kKickControls) / sizeof(ControlDef)), kKickDefaults1, (int)(sizeof(kKickDefaults1) / sizeof(DefaultValue))},
  {"snare", "Snare Center", "Snare", "snare", 0, "snare", 38, EngineKind::SnareSd, 0, false, nullptr, nullptr, 0, 0, false, MixerChannel::Snare, 0,
   kSnareSdControls, (int)(sizeof(kSnareSdControls) / sizeof(ControlDef)), kSnareDefaults0, (int)(sizeof(kSnareDefaults0) / sizeof(DefaultValue))},
  {"snareOffCenter", "Snare Off Center", "Off Center", "snare", 1, "snare", 125, EngineKind::SnareSd, 0, false, nullptr, nullptr, 0, 0, false, MixerChannel::Snare, 1,
   kSnareSdControls, (int)(sizeof(kSnareSdControls) / sizeof(ControlDef)), kSnareDefaults1, (int)(sizeof(kSnareDefaults1) / sizeof(DefaultValue))},
  {"snareEdge", "Snare Edge", "Edge", "snare", 2, "snare", 33, EngineKind::SnareSd, 0, false, nullptr, nullptr, 0, 0, false, MixerChannel::Snare, 2,
   kSnareSdControls, (int)(sizeof(kSnareSdControls) / sizeof(ControlDef)), kSnareDefaults2, (int)(sizeof(kSnareDefaults2) / sizeof(DefaultValue))},
  {"snareRimShot", "Snare Rimshot", "Rim Shot", "snare", 3, "snare", 40, EngineKind::RmRim, 0, false, nullptr, nullptr, 0, 0, false, MixerChannel::Snare, 0,
   kSnareRmControls, (int)(sizeof(kSnareRmControls) / sizeof(ControlDef)), kSnareDefaults3, (int)(sizeof(kSnareDefaults3) / sizeof(DefaultValue))},
  {"rim", "Snare Sidestick", "Rim", "snare", 4, "snare", 37, EngineKind::SnareSd, 0, false, nullptr, nullptr, 0, 0, false, MixerChannel::Snare, 3,
   kSnareSdControls, (int)(sizeof(kSnareSdControls) / sizeof(ControlDef)), kSnareDefaults4, (int)(sizeof(kSnareDefaults4) / sizeof(DefaultValue))},
  {"snareRimOnly", "Snare Rim Only", "Rim Only", "snare", 5, "snare", 71, EngineKind::RmRim, 0, false, nullptr, nullptr, 0, 0, false, MixerChannel::Snare, 1,
   kSnareRmControls, (int)(sizeof(kSnareRmControls) / sizeof(ControlDef)), kSnareDefaults5, (int)(sizeof(kSnareDefaults5) / sizeof(DefaultValue))},
  {"chh", "Hi-Hat Closed Edge", "Closed Edge", "hihat", 0, "hihat-closed", 22, EngineKind::HiHat, 0, false, nullptr, "Closed", 0, 0, false, MixerChannel::HiHat, 0,
   kHiHatClosedControls, (int)(sizeof(kHiHatClosedControls) / sizeof(ControlDef)), kHiHatDefaults0, (int)(sizeof(kHiHatDefaults0) / sizeof(DefaultValue))},
  {"hihatTightEdge", "Hi-Hat Tight Edge", "Tight Edge", "hihat", 1, "hihat-closed", 62, EngineKind::HiHat, 0, false, nullptr, "Closed", 1, 0, false, MixerChannel::HiHat, 1,
   kHiHatClosedControls, (int)(sizeof(kHiHatClosedControls) / sizeof(ControlDef)), kHiHatDefaults1, (int)(sizeof(kHiHatDefaults1) / sizeof(DefaultValue))},
  {"ohh", "Hi-Hat Open Edge 0", "Open Edge 0", "hihat", 2, "hihat-open", 64, EngineKind::HiHat, 0, false, nullptr, "Open", 2, 0, false, MixerChannel::HiHat, 2,
   kHiHatOpenControls, (int)(sizeof(kHiHatOpenControls) / sizeof(ControlDef)), kHiHatDefaults2, (int)(sizeof(kHiHatDefaults2) / sizeof(DefaultValue))},
  {"hihatOpenEdge4", "Hi-Hat Open Edge 4", "Open Edge 4", "hihat", 3, "hihat-open", 60, EngineKind::HiHat, 0, false, nullptr, "Open", 3, 0, false, MixerChannel::HiHat, 3,
   kHiHatOpenControls, (int)(sizeof(kHiHatOpenControls) / sizeof(ControlDef)), kHiHatDefaults3, (int)(sizeof(kHiHatDefaults3) / sizeof(DefaultValue))},
  {"hihatClosedTip", "Hi-Hat Closed Tip", "Closed Tip", "hihat", 4, "hihat-closed", 61, EngineKind::HiHat, 0, false, nullptr, "Closed", 4, 0, false, MixerChannel::HiHat, 4,
   kHiHatClosedControls, (int)(sizeof(kHiHatClosedControls) / sizeof(ControlDef)), kHiHatDefaults4, (int)(sizeof(kHiHatDefaults4) / sizeof(DefaultValue))},
  {"hihatTightTip", "Hi-Hat Tight Tip", "Tight Tip", "hihat", 5, "hihat-closed", 63, EngineKind::HiHat, 0, false, nullptr, "Closed", 5, 0, false, MixerChannel::HiHat, 5,
   kHiHatClosedControls, (int)(sizeof(kHiHatClosedControls) / sizeof(ControlDef)), kHiHatDefaults5, (int)(sizeof(kHiHatDefaults5) / sizeof(DefaultValue))},
  {"tomHi", "Rack Tom 1 Center", "Rack Tom 1 Center", "tom", 0, "tom", 48, EngineKind::Tom, 2, false, "Center", nullptr, 0, 0, false, MixerChannel::RackTom1, 0,
   kTomControls, (int)(sizeof(kTomControls) / sizeof(ControlDef)), kTomDefaults0, (int)(sizeof(kTomDefaults0) / sizeof(DefaultValue))},
  {"rackTom1RimShot", "Rack Tom 1 Rimshot", "Rack Tom 1 Rimshot", "tom", 1, "tom", 82, EngineKind::Tom, 2, false, "RimShot", nullptr, 0, 0, false, MixerChannel::RackTom1, 1,
   kTomControls, (int)(sizeof(kTomControls) / sizeof(ControlDef)), kTomDefaults1, (int)(sizeof(kTomDefaults1) / sizeof(DefaultValue))},
  {"rackTom1RimOnly", "Rack Tom 1 Rim Only", "Rack Tom 1 Rim Only", "tom", 2, "tom", 81, EngineKind::Tom, 2, false, "RimOnly", nullptr, 0, 0, false, MixerChannel::RackTom1, 2,
   kTomControls, (int)(sizeof(kTomControls) / sizeof(ControlDef)), kTomDefaults2, (int)(sizeof(kTomDefaults2) / sizeof(DefaultValue))},
  {"rackTom2Center", "Rack Tom 2 Center", "Rack Tom 2 Center", "tom", 3, "tom", 47, EngineKind::Tom, 1, false, "Center", nullptr, 0, 0, false, MixerChannel::RackTom2, 3,
   kTomControls, (int)(sizeof(kTomControls) / sizeof(ControlDef)), kTomDefaults3, (int)(sizeof(kTomDefaults3) / sizeof(DefaultValue))},
  {"rackTom2RimShot", "Rack Tom 2 Rimshot", "Rack Tom 2 Rimshot", "tom", 4, "tom", 80, EngineKind::Tom, 1, false, "RimShot", nullptr, 0, 0, false, MixerChannel::RackTom2, 4,
   kTomControls, (int)(sizeof(kTomControls) / sizeof(ControlDef)), kTomDefaults4, (int)(sizeof(kTomDefaults4) / sizeof(DefaultValue))},
  {"rackTom2RimOnly", "Rack Tom 2 Rim Only", "Rack Tom 2 Rim Only", "tom", 5, "tom", 79, EngineKind::Tom, 1, false, "RimOnly", nullptr, 0, 0, false, MixerChannel::RackTom2, 5,
   kTomControls, (int)(sizeof(kTomControls) / sizeof(ControlDef)), kTomDefaults5, (int)(sizeof(kTomDefaults5) / sizeof(DefaultValue))},
  {"tomMid", "Rack Tom 3 Center", "Rack Tom 3 Center", "tom", 6, "tom", 45, EngineKind::Tom, 0, false, "Center", nullptr, 0, 0, false, MixerChannel::RackTom3, 6,
   kTomControls, (int)(sizeof(kTomControls) / sizeof(ControlDef)), kTomDefaults6, (int)(sizeof(kTomDefaults6) / sizeof(DefaultValue))},
  {"rackTom3RimShot", "Rack Tom 3 Rimshot", "Rack Tom 3 Rimshot", "tom", 7, "tom", 78, EngineKind::Tom, 0, false, "RimShot", nullptr, 0, 0, false, MixerChannel::RackTom3, 7,
   kTomControls, (int)(sizeof(kTomControls) / sizeof(ControlDef)), kTomDefaults7, (int)(sizeof(kTomDefaults7) / sizeof(DefaultValue))},
  {"rackTom3RimOnly", "Rack Tom 3 Rim Only", "Rack Tom 3 Rim Only", "tom", 8, "tom", 77, EngineKind::Tom, 0, false, "RimOnly", nullptr, 0, 0, false, MixerChannel::RackTom3, 8,
   kTomControls, (int)(sizeof(kTomControls) / sizeof(ControlDef)), kTomDefaults8, (int)(sizeof(kTomDefaults8) / sizeof(DefaultValue))},
  {"floorTom1Center", "Floor Tom 1 Center", "Floor Tom 1 Center", "tom", 9, "tom", 43, EngineKind::Tom, 1, true, "Center", nullptr, 0, 0, false, MixerChannel::FloorTom1, 9,
   kTomControls, (int)(sizeof(kTomControls) / sizeof(ControlDef)), kTomDefaults9, (int)(sizeof(kTomDefaults9) / sizeof(DefaultValue))},
  {"floorTom1RimShot", "Floor Tom 1 Rimshot", "Floor Tom 1 Rimshot", "tom", 10, "tom", 75, EngineKind::Tom, 1, true, "RimShot", nullptr, 0, 0, false, MixerChannel::FloorTom1, 10,
   kTomControls, (int)(sizeof(kTomControls) / sizeof(ControlDef)), kTomDefaults10, (int)(sizeof(kTomDefaults10) / sizeof(DefaultValue))},
  {"floorTom1RimOnly", "Floor Tom 1 Rim Only", "Floor Tom 1 Rim Only", "tom", 11, "tom", 74, EngineKind::Tom, 1, true, "RimOnly", nullptr, 0, 0, false, MixerChannel::FloorTom1, 11,
   kTomControls, (int)(sizeof(kTomControls) / sizeof(ControlDef)), kTomDefaults11, (int)(sizeof(kTomDefaults11) / sizeof(DefaultValue))},
  {"tomLow", "Floor Tom 2 Center", "Floor Tom 2 Center", "tom", 12, "tom", 41, EngineKind::Tom, 0, true, "Center", nullptr, 0, 0, false, MixerChannel::FloorTom2, 12,
   kTomControls, (int)(sizeof(kTomControls) / sizeof(ControlDef)), kTomDefaults12, (int)(sizeof(kTomDefaults12) / sizeof(DefaultValue))},
  {"floorTom2RimShot", "Floor Tom 2 Rimshot", "Floor Tom 2 Rimshot", "tom", 13, "tom", 73, EngineKind::Tom, 0, true, "RimShot", nullptr, 0, 0, false, MixerChannel::FloorTom2, 13,
   kTomControls, (int)(sizeof(kTomControls) / sizeof(ControlDef)), kTomDefaults13, (int)(sizeof(kTomDefaults13) / sizeof(DefaultValue))},
  {"floorTom2RimOnly", "Floor Tom 2 Rim Only", "Floor Tom 2 Rim Only", "tom", 14, "tom", 72, EngineKind::Tom, 0, true, "RimOnly", nullptr, 0, 0, false, MixerChannel::FloorTom2, 14,
   kTomControls, (int)(sizeof(kTomControls) / sizeof(ControlDef)), kTomDefaults14, (int)(sizeof(kTomDefaults14) / sizeof(DefaultValue))},
  {"cymbal1Crash", "Cymbal 1 Crash", "Cymbal 1 Crash", "cymbal", 0, "cymbal", 86, EngineKind::Cymbal, 0, false, "Crash", nullptr, 0, 0, false, MixerChannel::Cymbal1, 0,
   kCymbalControls, (int)(sizeof(kCymbalControls) / sizeof(ControlDef)), kCymbalDefaults0, (int)(sizeof(kCymbalDefaults0) / sizeof(DefaultValue))},
  {"cymbal1MuteHit", "Cymbal 1 Mute Hit", "Cymbal 1 Mute Hit", "cymbal", 1, "cymbal", 83, EngineKind::Cymbal, 0, false, "Mute", nullptr, 0, 0, true, MixerChannel::Cymbal1, 1,
   kCymbalControls, (int)(sizeof(kCymbalControls) / sizeof(ControlDef)), kCymbalDefaults1, (int)(sizeof(kCymbalDefaults1) / sizeof(DefaultValue))},
  {"crash1", "Cymbal 2 Crash", "Cymbal 2 Crash", "cymbal", 2, "cymbal", 49, EngineKind::Cymbal, 0, false, "Crash", nullptr, 0, 1, false, MixerChannel::Cymbal2, 2,
   kCymbalControls, (int)(sizeof(kCymbalControls) / sizeof(ControlDef)), kCymbalDefaults2, (int)(sizeof(kCymbalDefaults2) / sizeof(DefaultValue))},
  {"cymbal2MuteHit", "Cymbal 2 Mute Hit", "Cymbal 2 Mute Hit", "cymbal", 3, "cymbal", 50, EngineKind::Cymbal, 0, false, "Mute", nullptr, 0, 1, true, MixerChannel::Cymbal2, 3,
   kCymbalControls, (int)(sizeof(kCymbalControls) / sizeof(ControlDef)), kCymbalDefaults3, (int)(sizeof(kCymbalDefaults3) / sizeof(DefaultValue))},
  {"cymbal3Crash", "Cymbal 3 Crash", "Cymbal 3 Crash", "cymbal", 4, "cymbal", 55, EngineKind::Cymbal, 0, false, "Crash", nullptr, 0, 2, false, MixerChannel::Cymbal3, 4,
   kCymbalControls, (int)(sizeof(kCymbalControls) / sizeof(ControlDef)), kCymbalDefaults4, (int)(sizeof(kCymbalDefaults4) / sizeof(DefaultValue))},
  {"cymbal3MuteHit", "Cymbal 3 Mute Hit", "Cymbal 3 Mute Hit", "cymbal", 5, "cymbal", 56, EngineKind::Cymbal, 0, false, "Mute", nullptr, 0, 2, true, MixerChannel::Cymbal3, 5,
   kCymbalControls, (int)(sizeof(kCymbalControls) / sizeof(ControlDef)), kCymbalDefaults5, (int)(sizeof(kCymbalDefaults5) / sizeof(DefaultValue))},
  {"ride", "Ride Bow Tip", "Ride Bow Tip", "ride", 0, "ride", 51, EngineKind::Ride, 0, false, "BowTip", nullptr, 0, 0, false, MixerChannel::Ride, 0,
   kRideControls, (int)(sizeof(kRideControls) / sizeof(ControlDef)), kRideDefaults0, (int)(sizeof(kRideDefaults0) / sizeof(DefaultValue))},
  {"rideBowShank", "Ride Bow Shank", "Ride Bow Shank", "ride", 1, "ride", 29, EngineKind::Ride, 0, false, "BowShank", nullptr, 0, 0, false, MixerChannel::Ride, 1,
   kRideControls, (int)(sizeof(kRideControls) / sizeof(ControlDef)), kRideDefaults1, (int)(sizeof(kRideDefaults1) / sizeof(DefaultValue))},
  {"rideBellTip", "Ride Bell Tip", "Ride Bell Tip", "ride", 2, "ride", 30, EngineKind::Ride, 0, false, "BellTip", nullptr, 0, 0, false, MixerChannel::Ride, 2,
   kRideControls, (int)(sizeof(kRideControls) / sizeof(ControlDef)), kRideDefaults2, (int)(sizeof(kRideDefaults2) / sizeof(DefaultValue))},
  {"rideBell", "Ride Bell Shank", "Ride Bell Shank", "ride", 3, "ride", 53, EngineKind::Ride, 0, false, "BellShank", nullptr, 0, 0, false, MixerChannel::Ride, 3,
   kRideControls, (int)(sizeof(kRideControls) / sizeof(ControlDef)), kRideDefaults3, (int)(sizeof(kRideDefaults3) / sizeof(DefaultValue))},
  {"rideEdge", "Ride Edge", "Ride Edge", "ride", 4, "ride", 59, EngineKind::Ride, 0, false, "Edge", nullptr, 0, 0, false, MixerChannel::Ride, 4,
   kRideControls, (int)(sizeof(kRideControls) / sizeof(ControlDef)), kRideDefaults4, (int)(sizeof(kRideDefaults4) / sizeof(DefaultValue))},
  {"rideMuteHit", "Ride Mute Hit", "Ride Mute Hit", "ride", 5, "ride", 118, EngineKind::Ride, 0, false, "Mute", nullptr, 0, 0, false, MixerChannel::Ride, 5,
   kRideControls, (int)(sizeof(kRideControls) / sizeof(ControlDef)), kRideDefaults5, (int)(sizeof(kRideDefaults5) / sizeof(DefaultValue))},
  {"crash2", "Cymbal 4 Crash", "Cymbal 4 Crash", "cymbal", 6, "cymbal", 57, EngineKind::Cymbal, 0, false, "Crash", nullptr, 0, 3, false, MixerChannel::Cymbal4, 6,
   kCymbalControls, (int)(sizeof(kCymbalControls) / sizeof(ControlDef)), kCymbalDefaults6, (int)(sizeof(kCymbalDefaults6) / sizeof(DefaultValue))},
  {"cymbal4MuteHit", "Cymbal 4 Mute Hit", "Cymbal 4 Mute Hit", "cymbal", 7, "cymbal", 58, EngineKind::Cymbal, 0, false, "Mute", nullptr, 0, 3, true, MixerChannel::Cymbal4, 7,
   kCymbalControls, (int)(sizeof(kCymbalControls) / sizeof(ControlDef)), kCymbalDefaults7, (int)(sizeof(kCymbalDefaults7) / sizeof(DefaultValue))},
  {"cymbal5Crash", "Cymbal 5 Crash", "Cymbal 5 Crash", "cymbal", 8, "cymbal", 52, EngineKind::Cymbal, 0, false, "Crash", nullptr, 0, 4, false, MixerChannel::Cymbal5, 8,
   kCymbalControls, (int)(sizeof(kCymbalControls) / sizeof(ControlDef)), kCymbalDefaults8, (int)(sizeof(kCymbalDefaults8) / sizeof(DefaultValue))},
  {"cymbal5MuteHit", "Cymbal 5 Mute Hit", "Cymbal 5 Mute Hit", "cymbal", 9, "cymbal", 54, EngineKind::Cymbal, 0, false, "Mute", nullptr, 0, 4, true, MixerChannel::Cymbal5, 9,
   kCymbalControls, (int)(sizeof(kCymbalControls) / sizeof(ControlDef)), kCymbalDefaults9, (int)(sizeof(kCymbalDefaults9) / sizeof(DefaultValue))},
  {"highConga808", "High Conga 808", nullptr, "custom", 0, "custom", -1, EngineKind::Tom, 2, true, "Center", nullptr, 0, 0, false, MixerChannel::Custom, 0,
   kTomControls, (int)(sizeof(kTomControls) / sizeof(ControlDef)), kCongaDefaults, (int)(sizeof(kCongaDefaults) / sizeof(DefaultValue))},
  {"claves808", "Claves 808", nullptr, "custom", 1, "custom", -1, EngineKind::RmClaves, 0, false, nullptr, nullptr, 0, 0, false, MixerChannel::Custom, 1,
   kSnareRmControls, (int)(sizeof(kSnareRmControls) / sizeof(ControlDef)), kClavesDefaults, (int)(sizeof(kClavesDefaults) / sizeof(DefaultValue))},
  {"handClap808", "Hand Clap 808", nullptr, "custom", 2, "custom", -1, EngineKind::HcClap, 0, false, nullptr, nullptr, 0, 0, false, MixerChannel::Custom, 2,
   kSnareRmControls, (int)(sizeof(kSnareRmControls) / sizeof(ControlDef)), kHcDefaults, (int)(sizeof(kHcDefaults) / sizeof(DefaultValue))},
  {"maracas808", "Maracas 808", nullptr, "custom", 3, "custom", -1, EngineKind::HcMaracas, 0, false, nullptr, nullptr, 0, 0, false, MixerChannel::Custom, 3,
   kSnareRmControls, (int)(sizeof(kSnareRmControls) / sizeof(ControlDef)), kMaracasDefaults, (int)(sizeof(kMaracasDefaults) / sizeof(DefaultValue))},
  {"cowbell808", "Cowbell 808", nullptr, "custom", 4, "custom", -1, EngineKind::Cowbell, 0, false, nullptr, nullptr, 0, 0, false, MixerChannel::Custom, 4,
   kSnareRmControls, (int)(sizeof(kSnareRmControls) / sizeof(ControlDef)), kCbDefaults, (int)(sizeof(kCbDefaults) / sizeof(DefaultValue))},
  {"pc2808", "PC-2 808", nullptr, "custom", 5, "custom", -1, EngineKind::Pc2, 0, false, nullptr, nullptr, 0, 0, false, MixerChannel::Custom, 5,
   kPc2Controls, (int)(sizeof(kPc2Controls) / sizeof(ControlDef)), kPc2Defaults, (int)(sizeof(kPc2Defaults) / sizeof(DefaultValue))},
};

inline constexpr int kNumInstruments = (int)(sizeof(kInstruments) / sizeof(kInstruments[0]));

constexpr bool StrEq(const char* a, const char* b)
{
  while (*a && *a == *b) { ++a; ++b; }
  return *a == *b;
}

constexpr int CountInstruments(const char* family)
{
  int n = 0;
  for (const InstrumentDef& d : kInstruments) if (StrEq(d.family, family)) ++n;
  return n;
}

constexpr int CountInstruments(const char* family, EngineKind kind)
{
  int n = 0;
  for (const InstrumentDef& d : kInstruments) if (StrEq(d.family, family) && d.kind == kind) ++n;
  return n;
}

// The instance a GM note plays, nullptr for every other note (custom instruments have no fixed note).
inline const InstrumentDef* FindInstrumentByNote(int note)
{
  if (note < 0) return nullptr;
  for (const InstrumentDef& d : kInstruments) if (d.gmNote == note) return &d;
  return nullptr;
}

// The default value of a control of a row (fallback when the row has none for that key).
inline double DefaultOf(const InstrumentDef& d, const char* key, double fallback = 0.0)
{
  for (int i = 0; i < d.numDefaults; ++i) if (StrEq(d.defaults[i].key, key)) return d.defaults[i].value;
  return fallback;
}

// Custom instruments (Drum Engine > Custom tab): the rows of family "custom", indexed by their paramIndex. A custom instrument is a circuit
// of the common 808 engine played on a track the user assigns it to (Add Track dialog); it has no fixed GM note. Each has its own voice
// pool (mCustomPools for kind Tom, mCustomRmPools for kind RmClaves, mCustomHcPools for kind HcClap / HcMaracas, mCustomCbPools for kind Cowbell, mCustomPc2Pools for kind Pc2; the pools of the other kinds stay idle), default cache set and card.
inline constexpr int kNumCustomInstruments = CountInstruments("custom");

inline const InstrumentDef* FindInstrument(const char* family, int paramIndex)
{
  for (const InstrumentDef& d : kInstruments) if (d.paramIndex == paramIndex && StrEq(d.family, family)) return &d;
  return nullptr;
}


inline const char* EngineKindName(EngineKind k)
{
  switch (k)
  {
    case EngineKind::Kick: return "Kick";
    case EngineKind::SnareSd: return "SnareSd";
    case EngineKind::RmRim: return "RmRim";
    case EngineKind::RmClaves: return "RmClaves";
    case EngineKind::HcClap: return "HcClap";
    case EngineKind::HcMaracas: return "HcMaracas";
    case EngineKind::Tom: return "Tom";
    case EngineKind::HiHat: return "HiHat";
    case EngineKind::Ride: return "Ride";
    case EngineKind::Cymbal: return "Cymbal";
    case EngineKind::Cowbell: return "Cowbell";
    case EngineKind::Pc2: return "Pc2";
  }
  return "";
}

inline const char* MixerChannelName(MixerChannel c)
{
  static const char* kNames[] = {"Kick", "Snare", "HiHat", "RackTom1", "RackTom2", "RackTom3", "FloorTom1", "FloorTom2",
                                 "Cymbal1", "Cymbal2", "Cymbal3", "Cymbal4", "Cymbal5", "Ride", "Custom"};
  return kNames[static_cast<int>(c)];
}

inline nlohmann::json InstrumentToJson(const InstrumentDef& d)
{
  nlohmann::json j;
  j["id"] = d.id;
  j["name"] = d.name;
  j["trackName"] = d.trackName ? nlohmann::json(d.trackName) : nlohmann::json(nullptr);
  j["family"] = d.family;
  j["paramIndex"] = d.paramIndex;
  j["uiFamily"] = d.uiFamily;
  j["gm"] = d.gmNote >= 0 ? nlohmann::json(d.gmNote) : nlohmann::json(nullptr);
  j["mixerChannel"] = MixerChannelName(d.channel);
  j["poolSlot"] = d.poolSlot;

  nlohmann::json e;
  e["kind"] = EngineKindName(d.kind);
  switch (d.kind)
  {
    case EngineKind::Tom: e["sheet"] = d.sheet; e["conga"] = d.conga; e["articulation"] = d.articulation; break;
    case EngineKind::HiHat: e["circuitFamily"] = d.circuitFamily; e["variantPoolArticulationId"] = d.poolArtId; break;
    case EngineKind::Ride: e["articulation"] = d.articulation; break;
    case EngineKind::Cymbal: e["cymbalPos"] = d.cymbalPos; e["isMute"] = d.isMute; e["articulation"] = d.articulation; break;
    case EngineKind::RmRim: e["claves"] = false; break;
    case EngineKind::RmClaves: e["claves"] = true; break;
    case EngineKind::HcClap: e["clap"] = true; break;
    case EngineKind::HcMaracas: e["clap"] = false; break;
    default: break;
  }
  j["engine"] = e;

  nlohmann::json defaults = nlohmann::json::object();
  for (int i = 0; i < d.numDefaults; ++i) defaults[d.defaults[i].key] = d.defaults[i].value;
  j["defaults"] = defaults;

  nlohmann::json controls = nlohmann::json::array();
  for (int i = 0; i < d.numControls; ++i)
  {
    const ControlDef& c = d.controls[i];
    controls.push_back({ {"group", c.group}, {"key", c.key}, {"label", c.label}, {"tooltip", c.tooltip},
                         {"min", c.min}, {"max", c.max}, {"step", c.step},
                         {"default", defaults.contains(c.key) ? defaults[c.key] : nlohmann::json(nullptr)} });
  }
  j["controls"] = controls;
  return j;
}

// Payload of the 'instrumentTable' bridge message.
inline nlohmann::json InstrumentTableToJson()
{
  nlohmann::json instruments = nlohmann::json::array();
  for (const InstrumentDef& d : kInstruments) instruments.push_back(InstrumentToJson(d));
  return { {"instruments", instruments} };
}

inline const InstrumentDef& CustomInstrument(int k) { return *FindInstrument("custom", k); }

inline int CustomInstrumentIndex(const char* id)
{
  for (const InstrumentDef& d : kInstruments) if (StrEq(d.family, "custom") && StrEq(d.id, id)) return d.paramIndex;
  return -1;
}

}  // namespace dgg
