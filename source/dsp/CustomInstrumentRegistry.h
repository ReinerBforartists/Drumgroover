// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Reiner Prokein
#pragma once
// The custom instruments in the order of their registry index (the index the UI and the host address them by). The definitions are in the
// files of their implementations; this list only names them.
#include "808CustomInstruments.h"
#include "909CustomInstruments.h"
#include "808InstrumentTable.h"

namespace dgg {

inline constexpr CustomInstrumentDef kCustomInstruments[] = {
  kCustomHighConga808, kCustomClaves808, kCustomHandClap808, kCustomMaracas808, kCustomCowbell808, kCustomPc2808,
  kCustomHandClap909,
};

inline constexpr int kNumCustomInstruments = (int)(sizeof(kCustomInstruments) / sizeof(kCustomInstruments[0]));

inline const CustomInstrumentDef& CustomInstrument(int k) { return kCustomInstruments[k]; }

inline int CustomInstrumentIndex(const char* id)
{
  for (int k = 0; k < kNumCustomInstruments; ++k) if (StrEq(kCustomInstruments[k].id, id)) return k;
  return -1;
}

// Payload of the 'instrumentTable' bridge message: the kit table, then the custom instruments.
inline nlohmann::json InstrumentTableWithCustomsToJson()
{
  nlohmann::json t = InstrumentTableToJson();
  for (int k = 0; k < kNumCustomInstruments; ++k) t["instruments"].push_back(CustomInstrumentToJson(kCustomInstruments[k], k));
  return t;
}

}  // namespace dgg
