#include "Song.h"

#include <algorithm>

namespace dgg {

namespace {
// Port of JS `findBlock(id)` — linear search, same as the JS original
// (blocksList.find(...)). Block counts are small (a handful of arranged
// blocks per song), so this is not worth indexing.
const Block* FindBlockById(const std::string& blockId, const std::vector<Block>& blocks) {
  auto it = std::find_if(blocks.begin(), blocks.end(),
                          [&](const Block& b) { return b.id == blockId; });
  return it != blocks.end() ? &(*it) : nullptr;
}

// Port of the JS `block ? (block.length || 1) : 1` idiom, used by both
// slotSpanBars and assembleFullPattern's maxEndBeat tracking.
double BlockLengthBarsOr1(const Block* block) {
  if (!block) return 1.0;
  return block->length > 0.0 ? block->length : 1.0;
}
}  // namespace

double SlotSpanBars(const SongSlot& slot, const std::vector<Block>& blocks) {
  const Block* block = FindBlockById(slot.blockId, blocks);
  int loopCount = slot.loopCount > 0 ? slot.loopCount : 1; // port of `slot.loopCount || 1`
  return BlockLengthBarsOr1(block) * loopCount;
}

double SlotEndBar(const SongSlot& slot, const std::vector<Block>& blocks) {
  return slot.startBar + SlotSpanBars(slot, blocks);
}

std::vector<ExpandedSlot> ExpandedSequence(const Song& song, const std::vector<Block>& blocks) {
  std::vector<ExpandedSlot> expanded;
  for (const auto& slot : song.songSequence) {
    const Block* block = FindBlockById(slot.blockId, blocks);
    double len = BlockLengthBarsOr1(block);
    int loops = slot.loopCount > 0 ? slot.loopCount : 1;
    for (int i = 0; i < loops; i++) {
      expanded.push_back({slot.blockId, slot.startBar + i * len});
    }
  }
  return expanded;
}

FullPattern BuildFullPatternFromSong(const Song& song, const std::vector<Block>& blocks,
                                      const std::vector<Track>& tracks, const Grid& grid) {
  FullPattern out;
  auto chain = ExpandedSequence(song, blocks);

  double maxEndBeat = 0.0;
  for (const auto& slot : chain) {
    const Block* block = FindBlockById(slot.blockId, blocks);
    if (!block) continue; // gaps are just unoccupied bars, matching JS "nothing to place"

    double startBeat = slot.startBar * grid.numerator;

    // See Song.h header comment: assumes buildFullPatternFromBlock returns
    // block-relative beat positions, unoffset — this is the one place that
    // adds the slot's own position in the song.
    FullPattern blockPattern = buildFullPatternFromBlock(*block, tracks);
    for (const auto& ev : blockPattern.events) {
      out.events.push_back({ev.trackId, startBeat + ev.beatAbs, ev.vel});
    }

    maxEndBeat = std::max(maxEndBeat, startBeat + BlockLengthBarsOr1(block) * grid.numerator);
  }

  std::sort(out.events.begin(), out.events.end(),
            [](const PatternEvent& a, const PatternEvent& b) { return a.beatAbs < b.beatAbs; });
  out.totalBeats = maxEndBeat;
  return out;
}

}  // namespace dgg
