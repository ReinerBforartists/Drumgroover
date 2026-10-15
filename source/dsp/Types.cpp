#include "Types.h"

#include <algorithm>

namespace dgg {

FullPattern buildFullPatternFromBlock(const Block& block, const std::vector<Track>& tracks) {
  FullPattern pattern;

  for (const auto& t : tracks) {
    auto it = block.data.find(t.id);
    if (it == block.data.end()) continue;
    for (const auto& note : it->second) {
      pattern.events.push_back({t.id, note.beat, note.vel});
    }
  }

  std::sort(pattern.events.begin(), pattern.events.end(),
            [](const PatternEvent& a, const PatternEvent& b) { return a.beatAbs < b.beatAbs; });

  // Port of `(block.length || 1) * block.numerator` — JS `||` treats 0 as
  // falsy too, so a zero length also falls back to 1 bar, not "0 beats".
  double length = block.length != 0.0 ? block.length : 1.0;
  pattern.totalBeats = length * block.numerator;

  return pattern;
}

} // namespace dgg
