// TR-909 mode: which instrument-table rows play a 909 fast voice.
//
// In the 909 engine mode the kick, the snare (SD circuit), the rack and floor toms (not the congas) and the hand clap play the 909 voices of the
// same name; every other row (hi-hats, cymbals, rim shot, claves, maracas, cowbell, PC-2, congas) keeps playing the 808 voice. The 909 voice
// goes to the mixer channel of the row.
#pragma once
#include "808InstrumentTable.h"
#include "909Playback.h"

namespace dgg {

inline bool Tr909VoiceForInstrument(const InstrumentDef& d, Tr909Voice& v)
{
  switch (d.kind)
  {
    case EngineKind::Kick: v = Tr909Voice::Bd; return true;
    case EngineKind::SnareSd: v = Tr909Voice::Sd; return true;
    case EngineKind::HcClap: v = Tr909Voice::Hc; return true;
    case EngineKind::Tom:
      if (d.conga) return false;
      v = d.sheet == 0 ? Tr909Voice::Lt : d.sheet == 1 ? Tr909Voice::Mt : Tr909Voice::Ht; return true;
    default: return false;
  }
}

}  // namespace dgg
