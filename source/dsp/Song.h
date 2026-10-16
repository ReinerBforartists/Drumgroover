// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Reiner Prokein
#pragma once

// Song — C++ read‑side port of generator‑core.js's SongEngine.
// Provides the flat ordered slot list and assembleFullPattern.
// UI editing methods (addSlot, removeSlot, etc.) remain in JavaScript.

#include <string>
#include <vector>

#include "Types.h"

namespace dgg {

// One entry in the song sequence: a block placed at a given start bar,
// repeated loopCount times back‑to‑back. startBar is in bars (double,
// matching the JavaScript Number type).
struct SongSlot {
  std::string blockId;
  double      startBar  = 0.0;
  int         loopCount = 1;
};

// Read‑only state: the list of slots. Slots are assumed non‑overlapping;
// this invariant is enforced on the JavaScript side.
struct Song {
  std::vector<SongSlot> songSequence;
};

// Returns the span in bars of a slot: block.length * loopCount.
// If the block is missing, falls back to 1 bar.
double SlotSpanBars(const SongSlot& slot, const std::vector<Block>& blocks);

// Returns the exclusive end bar of a slot (startBar + SlotSpanBars).
double SlotEndBar(const SongSlot& slot, const std::vector<Block>& blocks);

// One expanded slot: a loop‑unrolled occurrence with its absolute start bar.
struct ExpandedSlot {
  std::string blockId;
  double      startBar;
};

// Expands all loops in the song into a flat list of consecutive occurrences.
std::vector<ExpandedSlot> ExpandedSequence(const Song& song, const std::vector<Block>& blocks);

// Builds a song‑wide FullPattern from the whole arrangement.
// Expands each slot via ExpandedSequence and uses buildFullPatternFromBlock
// per slot, adding the slot's startBar * grid.numerator offset to each note's beat.
FullPattern BuildFullPatternFromSong(const Song& song, const std::vector<Block>& blocks,
                                      const std::vector<Track>& tracks, const Grid& grid);

} // namespace dgg