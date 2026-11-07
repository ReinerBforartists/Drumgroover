// TR-909 mode: which instrument-table rows play a 909 fast voice.
//
// In the 909 engine mode the kick, the snare (SD circuit), the rack and floor toms (not the congas), the cymbals (Crash) and the ride play the 909 voices of the same name;
// the hi-hat rows play the Closed Hat or Open Hat 909 voice; the mute hits keep the 808 voice. The rim shot rows play the live 909 Rim Shot (Tr909RimLiveForInstrument). The 909 voice goes to the mixer channel of the row.
// The Hand Clap 909 is not a table row of the 808 table: it is defined in 909CustomInstruments.h.
#pragma once
#include "808InstrumentTable.h"
#include "909Playback.h"
#include "909RimLive.h"

namespace dgg {

inline bool Tr909VoiceForInstrument(const InstrumentDef& d, Tr909Voice& v)
{
  switch (d.kind)
  {
    case EngineKind::Kick: v = Tr909Voice::Bd; return true;
    case EngineKind::SnareSd: v = Tr909Voice::Sd; return true;
    case EngineKind::Tom:
      // The 909 has three toms (LT, MT, HT), the kit six tom positions. The rack toms play the 909 tom of their sheet (Rack Tom 1 HT, 2 MT, 3 LT); the floor toms (the table's
      // "conga" flag marks the floor rows) are the low tom pitched down (Tr909BasePitchForInstrument). The sixth tom position (sheet 2, the position of the 808 High Conga) is the
      // custom instrument Floor Tom 3 909 (909CustomInstruments.h), as the High Conga is for the 808.
      if (d.conga) { v = Tr909Voice::Lt; return true; }
      v = d.sheet == 0 ? Tr909Voice::Lt : d.sheet == 1 ? Tr909Voice::Mt : Tr909Voice::Ht; return true;
    // The 909 has a Crash and a Ride cymbal (PCM voices): the Crash plays on the Cymbal rows, the Ride on the Ride rows; every articulation of a row plays the same 909 sound. The Mute rows keep the 808 voice (the 909 has no mute hit).
    case EngineKind::Cymbal: if (d.isMute) return false; v = Tr909Voice::Cr; return true;
    case EngineKind::Ride: if (StrEq(d.articulation, "Mute")) return false; v = Tr909Voice::Ri; return true;
    // The hi-hat rows play the Closed Hat (rows of the Closed family) or the Open Hat (Open family) PCM voice.
    case EngineKind::HiHat: v = StrEq(d.circuitFamily, "Open") ? Tr909Voice::Ho : Tr909Voice::Hh; return true;
    default: return false;
  }
}

// Fixed transposition of the 909 voice of a kit row: Floor Tom 1 (sheet 1) 2 semitones, Floor Tom 2 (sheet 0) 4 semitones below the low tom (Floor Tom 3: 6 semitones, 909CustomInstruments.h).
inline double Tr909BasePitchForInstrument(const InstrumentDef& d)
{
  if (d.kind != EngineKind::Tom || !d.conga) return 1.0;
  return d.sheet == 1 ? 0.8908987181403393 : 0.7937005259840998;
}

// Circuit pots of the 909 voice of a kit row, for the UI cards (the card keys are those of Tr909CachedVoice::SetPot): Kick Tune / Attack / Decay / Level, Snare Tune / Tone / Snappy / Level,
// Toms Tune / Decay / Level, Crash and Ride Tune / Level. The Pitch knob of the row (digital transpose) is shared with the 808 circuit and stays where it is.
inline nlohmann::json Tr909ControlsJson(const InstrumentDef& d)
{
  Tr909Voice v;
  if (!Tr909VoiceForInstrument(d, v)) return nullptr;
  auto knob = [](const char* key, const char* label, const char* group, const char* tip) { return nlohmann::json{ {"key", key}, {"label", label}, {"group", group}, {"tooltip", tip}, {"min", 0.0}, {"max", 1.0}, {"step", 0.01} }; };
  nlohmann::json c = nlohmann::json::array();
  if (v == Tr909Voice::Bd) { c.push_back(knob("tune909", "Tune", "Circuit", "Tuning of the drum.")); c.push_back(knob("attack909", "Attack", "Circuit", "Attack of the kick.")); c.push_back(knob("decay909", "Decay", "Circuit", "Decay of the kick.")); }
  else if (v == Tr909Voice::Sd) { c.push_back(knob("tune909", "Tune", "Circuit", "Tuning of the drum.")); c.push_back(knob("tone909", "Tone", "Circuit", "Tone of the snare.")); c.push_back(knob("snappy909", "Snappy", "Circuit", "Amount of the snare rattle.")); }
  else if (v == Tr909Voice::Hh || v == Tr909Voice::Ho) c.push_back(knob("decay909", "Decay", "Circuit", "Decay of the hi-hat (the circuit pot of the Closed / Open Hat)."));
  else if (Tr909IsPcmVoice(v)) c.push_back(knob("tune909", "Tune", "Circuit", "Playback speed of the cymbal sample (the PCM clock of the voice)."));
  else { c.push_back(knob("tune909", "Tune", "Circuit", "Tuning of the drum.")); c.push_back(knob("decay909", "Decay", "Circuit", "Decay of the drum.")); }
  c.push_back(knob("level909", "Level", "Output", "Output level (circuit pot of the voice)."));
  return c;
}

// Adds "controls909" (and the pots' start values to "defaults", all at the nominal position 0.5) to the rows of the table JSON that play a 909 voice. The rows are those of kInstruments, in order.
inline void AddTr909Controls(nlohmann::json& table)
{
  auto& rows = table["instruments"];
  for (size_t i = 0; i < rows.size() && i < static_cast<size_t>(kNumInstruments); ++i)
  {
    nlohmann::json c = Tr909ControlsJson(kInstruments[i]);
    if (c.is_null()) continue;
    for (const auto& k : c) rows[i]["defaults"][k["key"].get<std::string>()] = 0.5;
    rows[i]["controls909"] = std::move(c);
  }
}

// The two Rim Shot rows (Snare Rimshot, Snare Rim Only) play the one 909 Rim Shot circuit, computed live (909RimLive.h; the 909 has no second rim circuit).
inline bool Tr909RimLiveForInstrument(const InstrumentDef& d) { return d.kind == EngineKind::RmRim; }

}  // namespace dgg
