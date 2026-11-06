// TR-909 mode: which instrument-table rows play a 909 fast voice.
//
// In the 909 engine mode the kick, the snare (SD circuit) and the rack and floor toms (not the congas) play the 909 voices of the same name;
// every other kit row (hi-hats, cymbals) keeps playing the 808 voice. The rim shot rows play the live 909 Rim Shot (Tr909RimLiveForInstrument). The 909 voice goes to the mixer channel of the row.
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
    default: return false;
  }
}

// Fixed transposition of the 909 voice of a kit row: Floor Tom 1 (sheet 1) 2 semitones, Floor Tom 2 (sheet 0) 4 semitones below the low tom (Floor Tom 3: 6 semitones, 909CustomInstruments.h).
inline double Tr909BasePitchForInstrument(const InstrumentDef& d)
{
  if (d.kind != EngineKind::Tom || !d.conga) return 1.0;
  return d.sheet == 1 ? 0.8908987181403393 : 0.7937005259840998;
}

// The two Rim Shot rows (Snare Rimshot, Snare Rim Only) play the one 909 Rim Shot circuit, computed live (909RimLive.h; the 909 has no second rim circuit).
inline bool Tr909RimLiveForInstrument(const InstrumentDef& d) { return d.kind == EngineKind::RmRim; }

}  // namespace dgg
