// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Reiner Prokein
#include "GrooveGenerator.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <unordered_map>

namespace dgg {

double subdivisionFromKnobIndex(int index) {
  if (index < 0 || index > 3) return 4.0;
  return static_cast<double>(kSubdivisionKnobValues[index]);
}

uint32_t groupSeed(const Block& block, const std::string& groupId) {
  auto it = block.seeds.find(groupId);
  if (it != block.seeds.end()) return it->second;
  return seededHash(block.id + "|" + groupId + "|seed");
}

bool isArticulationEnabled(const Block& block, const std::string& trackId, const std::string& subGroup) {
  auto storedIt = block.enabledArticulations.find(subGroup);
  if (storedIt != block.enabledArticulations.end() && !storedIt->second.empty()) {
    const auto& stored = storedIt->second;
    return std::find(stored.begin(), stored.end(), trackId) != stored.end();
  }
  auto defIt = kDefaultEnabledArticulations.find(subGroup);
  if (defIt != kDefaultEnabledArticulations.end()) {
    const auto& def = defIt->second;
    return std::find(def.begin(), def.end(), trackId) != def.end();
  }
  return true; // no whitelist entry -> everything enabled
}

namespace {
// If phrase > 1, add barIdx*9973 to vary seed per bar.
uint32_t barSeedFor(uint32_t groupSeedVal, int phrase, int barIdx) {
  return phrase > 1 ? groupSeedVal + static_cast<uint32_t>(barIdx) * 9973u : groupSeedVal;
}
}  // namespace

double resolveTurnaroundLengthBeats(const Block& block, int numerator) {
  return std::max(0.25, std::min(static_cast<double>(numerator), block.turnaroundLengthBeats));
}

const std::unordered_map<std::string, std::vector<double>>& getTurnaroundBarDistribution(
    const Block& block, int barIdx, int numerator, uint32_t seed, TurnaroundFillCache& cache) {
  auto cacheIt = cache.find(barIdx);
  if (cacheIt != cache.end()) return cacheIt->second;

  std::unordered_map<std::string, std::vector<double>> dist;

  std::vector<std::string> voicePool;
  for (const auto& instKey : block.turnaroundInstruments) {
    auto it = kTurnaroundInstrumentTracks.find(instKey);
    if (it == kTurnaroundInstrumentTracks.end()) continue;
    for (const auto& t : it->second) voicePool.push_back(t);
  }

  if (!voicePool.empty()) {
    const Feel feel = block.feel;
    const double lengthBeats = resolveTurnaroundLengthBeats(block, numerator);
    const double densityPct = block.turnaroundDensity;
    // At least two steps must fit in the window: a shorter window gets a finer grid, so Fill Length is what it says.
    const double stepsPerBeat = std::max(static_cast<double>(block.turnaroundSubdivision), std::ceil(2.0 / lengthBeats - 1e-9));
    const int totalSteps = static_cast<int>(numerator * stepsPerBeat);
    const int zoneSteps = std::max(2, static_cast<int>(std::round(lengthBeats * stepsPerBeat)));
    const int rawHits = std::max(1, static_cast<int>(std::round((densityPct / 100.0) * zoneSteps)));
    const int hits = (feel == Feel::Halftime) ? std::max(1, static_cast<int>(std::round(rawHits / 2.0))) : rawHits;

    auto mask = buildEndAnchoredEuclidean(hits, zoneSteps, seed);

    std::vector<double> slotBeats;
    for (int i = 0; i < static_cast<int>(mask.size()); i++) {
      if (!mask[i]) continue;
      const int step = totalSteps - zoneSteps + i;
      const int beatIdx = static_cast<int>(std::floor(step / stepsPerBeat));
      const double fracWithinBeat = std::fmod(static_cast<double>(step), stepsPerBeat) / stepsPerBeat;
      slotBeats.push_back(beatIdx + applyFeelWarp(fracWithinBeat, feel));
    }

    RNG rng(seed + 1);
    // Deduplicate voices preserving first-occurrence order.
    std::vector<std::string> uniqueVoices;
    for (const auto& v : voicePool) {
      if (std::find(uniqueVoices.begin(), uniqueVoices.end(), v) == uniqueVoices.end()) uniqueVoices.push_back(v);
    }
    // Shuffle voices using rng.
    std::vector<std::pair<std::string, double>> keyed;
    keyed.reserve(uniqueVoices.size());
    for (const auto& v : uniqueVoices) keyed.push_back({v, rng.next()});
    std::sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
    std::vector<std::string> shuffledVoices;
    shuffledVoices.reserve(keyed.size());
    for (const auto& kv : keyed) shuffledVoices.push_back(kv.first);

    // Round-robin base coverage first, leftover assigned randomly.
    for (size_t i = 0; i < slotBeats.size(); i++) {
      const std::string& voice = (i < shuffledVoices.size())
        ? shuffledVoices[i]
        : voicePool[static_cast<size_t>(rng.intRange(0, static_cast<int>(voicePool.size()) - 1))];
      dist[voice].push_back(slotBeats[i]);
    }
  }

  auto emplaced = cache.emplace(barIdx, std::move(dist));
  return emplaced.first->second;
}

BeatDistribution generateFixedBreakHandPattern(double breakBeats, double stepsPerBeat,
                                                const std::vector<HandPatternSpec>& patternSpec, bool anchorStart,
                                                Feel feel, double beatOffset) {
  // For halftime, halve each count (minimum 1).
  std::vector<HandPatternSpec> effectiveSpec;
  if (feel == Feel::Halftime) {
    effectiveSpec.reserve(patternSpec.size());
    for (const auto& p : patternSpec) {
      effectiveSpec.push_back({p.id, std::max(1, static_cast<int>(std::round(p.count / 2.0)))});
    }
  } else {
    effectiveSpec = patternSpec;
  }

  const int totalSteps = std::max(1, static_cast<int>(std::round(breakBeats * stepsPerBeat)));
  int patternSteps = 0;
  for (const auto& p : effectiveSpec) patternSteps += p.count;

  const int startStep = anchorStart ? 0 : std::max(0, totalSteps - patternSteps);

  BeatDistribution dist;
  int step = startStep;
  for (const auto& p : effectiveSpec) {
    for (int i = 0; i < p.count && step < totalSteps; i++, step++) {
      double warpedAbs = warpAbsoluteBeat(beatOffset + step / stepsPerBeat, feel);
      dist[p.id].push_back(warpedAbs - beatOffset);
    }
  }
  return dist;
}

BeatDistribution generateBreakFill(double breakBeats, const std::vector<std::string>& instruments,
                                    double densityPct, uint32_t seed, double stepsPerBeat, Feel feel,
                                    double beatOffset) {
  const int totalSteps = std::max(2, static_cast<int>(std::round(breakBeats * stepsPerBeat)));

  std::vector<std::string> voicePool;
  for (const auto& instKey : instruments) {
    auto it = kTurnaroundInstrumentTracks.find(instKey);
    if (it == kTurnaroundInstrumentTracks.end()) continue;
    for (const auto& t : it->second) voicePool.push_back(t);
  }

  const int rawHits = std::max(1, static_cast<int>(std::round((densityPct / 100.0) * totalSteps)));
  const int hits = (feel == Feel::Halftime) ? std::max(1, static_cast<int>(std::round(rawHits / 2.0))) : rawHits;
  auto mask = euclideanRhythm(hits, totalSteps, seed);

  BeatDistribution dist;
  if (!voicePool.empty()) {
    RNG rng(seed + 1);
    // Deduplicate voices.
    std::vector<std::string> uniqueVoices;
    for (const auto& v : voicePool) {
      if (std::find(uniqueVoices.begin(), uniqueVoices.end(), v) == uniqueVoices.end()) uniqueVoices.push_back(v);
    }
    // Shuffle voices.
    std::vector<std::pair<std::string, double>> keyed;
    keyed.reserve(uniqueVoices.size());
    for (const auto& v : uniqueVoices) keyed.push_back({v, rng.next()});
    std::sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
    std::vector<std::string> shuffledVoices;
    shuffledVoices.reserve(keyed.size());
    for (const auto& kv : keyed) shuffledVoices.push_back(kv.first);

    int vi = 0;
    for (int i = 0; i < static_cast<int>(mask.size()); i++) {
      if (!mask[i]) continue;
      const std::string& voice = (vi < static_cast<int>(shuffledVoices.size()))
          ? shuffledVoices[vi]
          : voicePool[static_cast<size_t>(rng.intRange(0, static_cast<int>(voicePool.size()) - 1))];
      vi++;
      double warpedAbs = warpAbsoluteBeat(beatOffset + i / stepsPerBeat, feel);
      dist[voice].push_back(warpedAbs - beatOffset);
    }
  }
  return dist;
}

Block generateBreakBlock(const Block& parent, const Grid& grid, const std::string& id,
                          const std::string& parentId, double totalLength, double breakBeats,
                          const BreakBlockOptions& opts) {
  const int num = grid.numerator;
  const double parentLenRaw = parent.length != 0.0 ? parent.length : 1.0;
  const double parentLen    = std::max(1.0, parentLenRaw);
  const double totalBeats   = totalLength * num;
  const double grooveBeats  = std::max(0.0, totalBeats - breakBeats);

  // Ensure all parent tracks exist on result, even if empty.
  std::unordered_map<std::string, std::vector<Note>> data;
  for (const auto& kv : parent.data) data[kv.first] = {};

  // Tile parent's groove bar-by-bar up to grooveBeats.
  const int barsNeeded = static_cast<int>(std::ceil(totalBeats / num));
  for (int barIdx = 0; barIdx < barsNeeded; barIdx++) {
    const double srcBar = std::fmod(static_cast<double>(barIdx), parentLen);
    for (const auto& kv : parent.data) {
      const std::string& tid = kv.first;
      for (const auto& n : kv.second) {
        const double localBeat = n.beat - srcBar * num;
        if (localBeat < -0.0001 || localBeat >= num) continue;
        const double absBeat = localBeat + barIdx * num;
        if (absBeat < grooveBeats - 0.0001) data[tid].push_back({absBeat, n.vel, false});
      }
    }
  }

  // Feel is inherited from parent; Break has no separate feel control.
  const Feel feel = parent.feel;

  std::vector<std::string> breakInstruments;
  for (const auto& i : opts.breakInstruments) {
    if (i != "kick") breakInstruments.push_back(i);
  }
  const double breakDensity     = opts.breakDensity;
  const int    breakFillVel     = opts.breakFillVel;
  const double breakSubdivision = opts.breakSubdivision;

  const bool hasSnare = std::find(breakInstruments.begin(), breakInstruments.end(), "snare") !=
                         breakInstruments.end();
  BeatDistribution fillDist = !opts.breakSeedSet
      ? generateFixedBreakHandPattern(breakBeats, breakSubdivision,
                                       hasSnare ? kBreakHandPatternSnareToms : kBreakHandPatternToms,
                                       /*anchorStart=*/false, feel, grooveBeats)
      : generateBreakFill(breakBeats, breakInstruments, breakDensity, opts.breakSeed, breakSubdivision, feel,
                           grooveBeats);
  if (!opts.breakSeedSet) {
    const bool hasToms = std::find(breakInstruments.begin(), breakInstruments.end(), "toms") != breakInstruments.end();
    for (auto it = fillDist.begin(); it != fillDist.end();) {
      const bool keep = (it->first == "snare") ? hasSnare : hasToms;
      it = keep ? std::next(it) : fillDist.erase(it);
    }
  }
  for (const auto& kv : fillDist) {
    for (double beat : kv.second) data[kv.first].push_back({beat + grooveBeats, breakFillVel, false});
  }

  // Kick has its own independent layer, seed, and density.
  const double breakKickDensity     = opts.breakKickDensity;
  const double breakKickSubdivision = opts.breakKickSubdivision;
  const int kickSteps    = std::max(1, static_cast<int>(std::round(breakBeats * breakKickSubdivision)));
  const int rawKickHits  = std::max(1, static_cast<int>(std::round((breakKickDensity / 100.0) * kickSteps)));
  const int kickHits     = (feel == Feel::Halftime) ? std::max(1, static_cast<int>(std::round(rawKickHits / 2.0)))
                                                     : rawKickHits;
  auto kickMask = buildEndAnchoredEuclidean(kickHits, kickSteps, opts.breakKickSeed);
  for (int i = 0; i < static_cast<int>(kickMask.size()); i++) {
    if (!kickMask[i]) continue;
    double beat = warpAbsoluteBeat(grooveBeats + i / breakKickSubdivision, feel);
    data["kick"].push_back({beat, 115, false});
  }
  std::sort(data["kick"].begin(), data["kick"].end(), [](const Note& a, const Note& b) { return a.beat < b.beat; });

  // No closing crash here; resolution belongs to song assembly.
  Block result;
  result.id        = id;
  const std::string labelKey = parentId + "Break";
  auto labelIt = kSectionLabels.find(labelKey);
  result.name = (labelIt != kSectionLabels.end()) ? labelIt->second : labelKey;
  result.numerator = num;
  result.length     = totalLength;
  result.breakBeats = breakBeats;
  result.breakInstruments = breakInstruments;
  result.breakDensity     = breakDensity;
  result.breakFillVel     = breakFillVel;
  result.breakSubdivision = breakSubdivision;
  result.breakKickDensity     = breakKickDensity;
  result.breakKickSubdivision = breakKickSubdivision;
  result.breakKickSeed = opts.breakKickSeed;
  result.breakSeedSet  = opts.breakSeedSet;
  result.breakSeed     = opts.breakSeed;
  result.breakParentId = parentId;
  result.turnaroundInterval = 0;
  result.velocityBuild      = 0.0;
  result.data = std::move(data);
  return result;
}

Block generateFillBlock(const Grid& grid, const std::string& id, const std::string& name, double totalLength,
                         double fillBeatsIn, bool anchorStart, Feel feel, const FillBlockOptions& opts) {
  const int num = grid.numerator;
  const double totalBeats = totalLength * num;
  const double fillBeats = std::max(0.5, std::min(totalBeats, fillBeatsIn));
  const double fillOffset = anchorStart ? 0.0 : std::max(0.0, totalBeats - fillBeats);

  std::unordered_map<std::string, std::vector<Note>> data;

  std::vector<std::string> fillInstruments;
  for (const auto& i : opts.fillInstruments) {
    if (i != "kick") fillInstruments.push_back(i);
  }
  const double fillDensity     = opts.fillDensity;
  const int    fillVel         = opts.fillVel;
  const double fillSubdivision = opts.fillSubdivision;

  const bool hasSnare = std::find(fillInstruments.begin(), fillInstruments.end(), "snare") != fillInstruments.end();
  BeatDistribution fillDist = !opts.fillSeedSet
      ? generateFixedBreakHandPattern(fillBeats, fillSubdivision,
                                       hasSnare ? kBreakHandPatternSnareToms : kBreakHandPatternToms, anchorStart,
                                       feel, fillOffset)
      : generateBreakFill(fillBeats, fillInstruments, fillDensity, opts.fillSeed, fillSubdivision, feel,
                           fillOffset);
  if (!opts.fillSeedSet) {
    const bool hasToms = std::find(fillInstruments.begin(), fillInstruments.end(), "toms") != fillInstruments.end();
    for (auto it = fillDist.begin(); it != fillDist.end();) {
      const bool keep = (it->first == "snare") ? hasSnare : hasToms;
      it = keep ? std::next(it) : fillDist.erase(it);
    }
  }
  for (const auto& kv : fillDist) {
    for (double beat : kv.second) data[kv.first].push_back({beat + fillOffset, fillVel, false});
  }

  // Independent kick layer.
  const double fillKickDensity     = opts.fillKickDensity;
  const double fillKickSubdivision = opts.fillKickSubdivision;
  const int kickSteps        = std::max(1, static_cast<int>(std::round(fillBeats * fillKickSubdivision)));
  const int rawFillKickHits  = std::max(1, static_cast<int>(std::round((fillKickDensity / 100.0) * kickSteps)));
  const int fillKickHits     = (feel == Feel::Halftime)
                                    ? std::max(1, static_cast<int>(std::round(rawFillKickHits / 2.0)))
                                    : rawFillKickHits;
  auto kickMask = buildEndAnchoredEuclidean(fillKickHits, kickSteps, opts.fillKickSeed);
  for (int i = 0; i < static_cast<int>(kickMask.size()); i++) {
    if (!kickMask[i]) continue;
    double beat = warpAbsoluteBeat(fillOffset + i / fillKickSubdivision, feel);
    data["kick"].push_back({beat, 115, false});
  }
  std::sort(data["kick"].begin(), data["kick"].end(), [](const Note& a, const Note& b) { return a.beat < b.beat; });

  Block result;
  result.id   = id;
  result.name = name;
  result.numerator = num;
  result.length    = totalLength;
  result.fillBeats  = fillBeats;
  result.fillAnchor = anchorStart ? "start" : "end";
  result.fillInstruments = fillInstruments;
  result.fillDensity     = fillDensity;
  result.fillVel         = fillVel;
  result.fillSubdivision = fillSubdivision;
  result.fillKickDensity     = fillKickDensity;
  result.fillKickSubdivision = fillKickSubdivision;
  result.fillKickSeed = opts.fillKickSeed;
  result.fillSeedSet  = opts.fillSeedSet;
  result.fillSeed     = opts.fillSeed;
  result.feel = feel;
  result.isFillBlock = true;
  result.turnaroundInterval = 0;
  result.velocityBuild      = 0.0;
  result.data = std::move(data);
  return result;
}

void spliceBreakTail(Block& block, const Block& fresh, double grooveBeats, const std::vector<std::string>& tracks) {
  for (const auto& tid : tracks) {
    std::vector<Note> keep;
    auto blockIt = block.data.find(tid);
    if (blockIt != block.data.end()) {
      for (const auto& n : blockIt->second) {
        if (n.beat < grooveBeats - 0.0001) keep.push_back(n);
      }
    }

    std::vector<Note> freshTail;
    auto freshIt = fresh.data.find(tid);
    if (freshIt != fresh.data.end()) {
      for (const auto& n : freshIt->second) {
        if (n.beat >= grooveBeats - 0.0001) freshTail.push_back(n);
      }
    }

    keep.insert(keep.end(), freshTail.begin(), freshTail.end());
    std::sort(keep.begin(), keep.end(), [](const Note& a, const Note& b) { return a.beat < b.beat; });
    block.data[tid] = std::move(keep);
  }
}

std::unordered_set<std::string> breakFillTargetTracks() {
  std::unordered_set<std::string> tracks;
  for (const auto& kv : kTurnaroundInstrumentTracks) {
    for (const auto& t : kv.second) tracks.insert(t);
  }
  for (const auto& p : kBreakHandPatternToms) tracks.insert(p.id);
  for (const auto& p : kBreakHandPatternSnareToms) tracks.insert(p.id);
  return tracks;
}

std::optional<Block> computeFreshBreak(const Block& block, const Block& parent, const Grid& grid) {
  if (block.breakParentId.empty()) return std::nullopt;

  const double totalLength = std::max(1.0, block.length != 0.0 ? block.length : 1.0);
  const double totalBeats  = totalLength * grid.numerator;
  // Use default if breakBeats is 0.0 (unset); clamp to valid range.
  const double storedBreakBeats = block.breakBeats != 0.0 ? block.breakBeats : kBreakDefaultBeats;
  const double breakBeats = std::max(0.5, std::min(totalBeats - 0.5, storedBreakBeats));

  BreakBlockOptions opts;
  opts.breakInstruments      = block.breakInstruments;
  opts.breakDensity          = block.breakDensity;
  opts.breakFillVel          = block.breakFillVel;
  opts.breakSubdivision      = block.breakSubdivision;
  opts.breakKickDensity      = block.breakKickDensity;
  opts.breakKickSubdivision  = block.breakKickSubdivision;
  opts.breakKickSeed         = block.breakKickSeed;
  opts.breakSeedSet          = block.breakSeedSet;
  opts.breakSeed             = block.breakSeed;

  return generateBreakBlock(parent, grid, block.id, block.breakParentId, totalLength, breakBeats, opts);
}

bool regenerateBreakBlockInPlace(Block& block, const Block& parent, const Grid& grid, bool mainLocked,
                                  bool kickLocked) {
  auto freshOpt = computeFreshBreak(block, parent, grid);
  if (!freshOpt) return false;
  const Block& fresh = *freshOpt;
  const double grooveBeats = std::max(0.0, fresh.length * grid.numerator - fresh.breakBeats);

  const auto mainTracks = breakFillTargetTracks();

  std::unordered_set<std::string> allTracks;
  for (const auto& kv : block.data) allTracks.insert(kv.first);
  for (const auto& kv : fresh.data) allTracks.insert(kv.first);

  for (const auto& tid : allTracks) {
    const bool isKick = (tid == "kick");
    const bool isMain = !isKick && mainTracks.count(tid) > 0;
    const bool tailLocked = isKick ? kickLocked : (isMain ? mainLocked : false);

    std::vector<Note> freshBefore;
    auto freshIt = fresh.data.find(tid);
    if (freshIt != fresh.data.end()) {
      for (const auto& n : freshIt->second) {
        if (n.beat < grooveBeats - 0.0001) freshBefore.push_back(n);
      }
    }

    std::vector<Note> tailSource;
    if (tailLocked) {
      auto blockIt = block.data.find(tid);
      if (blockIt != block.data.end()) {
        for (const auto& n : blockIt->second) {
          if (n.beat >= grooveBeats - 0.0001) tailSource.push_back(n);
        }
      }
    } else if (freshIt != fresh.data.end()) {
      for (const auto& n : freshIt->second) {
        if (n.beat >= grooveBeats - 0.0001) tailSource.push_back(n);
      }
    }

    std::vector<Note> merged = std::move(freshBefore);
    merged.insert(merged.end(), tailSource.begin(), tailSource.end());
    std::sort(merged.begin(), merged.end(), [](const Note& a, const Note& b) { return a.beat < b.beat; });
    block.data[tid] = std::move(merged);
  }

  block.length     = fresh.length;
  block.breakBeats = fresh.breakBeats;
  return true;
}

bool regenerateBreakMainInPlace(Block& block, const Block& parent, const Grid& grid) {
  auto freshOpt = computeFreshBreak(block, parent, grid);
  if (!freshOpt) return false;
  const Block& fresh = *freshOpt;
  const double grooveBeats = std::max(0.0, fresh.length * grid.numerator - fresh.breakBeats);
  const auto tracksSet = breakFillTargetTracks();
  const std::vector<std::string> tracks(tracksSet.begin(), tracksSet.end());
  spliceBreakTail(block, fresh, grooveBeats, tracks);
  return true;
}

bool regenerateBreakKickInPlace(Block& block, const Block& parent, const Grid& grid) {
  auto freshOpt = computeFreshBreak(block, parent, grid);
  if (!freshOpt) return false;
  const Block& fresh = *freshOpt;
  const double grooveBeats = std::max(0.0, fresh.length * grid.numerator - fresh.breakBeats);
  spliceBreakTail(block, fresh, grooveBeats, {"kick"});
  return true;
}

bool regenerateFillBlockInPlace(Block& block, const Grid& grid) {
  const double totalLength = std::max(1.0, block.length != 0.0 ? block.length : 1.0);
  const double totalBeats  = totalLength * grid.numerator;
  // Use default if fillBeats is 0.0 (unset); clamp to valid range.
  const double storedFillBeats = block.fillBeats != 0.0 ? block.fillBeats : kBreakDefaultBeats;
  const double fillBeats = std::max(0.5, std::min(totalBeats, storedFillBeats));

  const bool anchorStart = (block.fillAnchor == "start");

  FillBlockOptions opts;
  opts.fillInstruments      = block.fillInstruments;
  opts.fillDensity          = block.fillDensity;
  opts.fillVel              = block.fillVel;
  opts.fillSubdivision      = block.fillSubdivision;
  opts.fillKickDensity      = block.fillKickDensity;
  opts.fillKickSubdivision  = block.fillKickSubdivision;
  opts.fillKickSeed         = block.fillKickSeed;
  opts.fillSeedSet          = block.fillSeedSet;
  opts.fillSeed             = block.fillSeed;

  Block fresh = generateFillBlock(grid, block.id, block.name, totalLength, fillBeats, anchorStart, block.feel, opts);

  block.data      = std::move(fresh.data);
  block.length    = fresh.length;
  block.fillBeats = fresh.fillBeats;
  block.feel      = fresh.feel;
  return true;
}

void replicateGroupAcrossLength(Block& block, const Grid& grid, const std::vector<std::string>& trackIds,
                                 int phraseLen, TurnaroundFillCache& cache) {
  const int len = std::max(1, static_cast<int>(std::round(block.length > 0.0 ? block.length : 1.0)));
  const int num = grid.numerator;
  const int phrase = std::max(1, std::min(len, phraseLen != 0 ? phraseLen : 1));

  for (const auto& tid : trackIds) {
    std::vector<Note> phraseSource;
    auto srcIt = block.data.find(tid);
    if (srcIt != block.data.end()) {
      for (const auto& n : srcIt->second) {
        if (n.beat < phrase * num && !n.turnaround) phraseSource.push_back(n);
      }
    }

    std::vector<Note> all;
    for (int barIdx = 0; barIdx < len; barIdx++) {
      const int sourceBarIdx = barIdx % phrase;
      const double srcLo = sourceBarIdx * num;

      std::vector<Note> sourceBar;
      for (const auto& n : phraseSource) {
        if (n.beat >= srcLo && n.beat < srcLo + num) sourceBar.push_back({n.beat - srcLo, n.vel});
      }
      // hasNoteNear expects a beat list, extract first.
      std::vector<double> sourceBarBeats;
      sourceBarBeats.reserve(sourceBar.size());
      for (const auto& n : sourceBar) sourceBarBeats.push_back(n.beat);

      const bool turnaround = block.turnaroundInterval > 0 && ((barIdx + 1) % block.turnaroundInterval == 0);
      const double buildFrac = len > 1 ? static_cast<double>(barIdx) / (len - 1) : 0.0;

      for (const auto& n : sourceBar) {
        int vel = n.vel;
        if (block.velocityBuild > 0.0) {
          vel = std::max(1, std::min(127, static_cast<int>(std::round(vel + buildFrac * 20.0 * block.velocityBuild))));
        }
        all.push_back({n.beat + barIdx * num, vel, false});
      }

      if (turnaround) {
        uint32_t distSeed = 0;
        if (block.turnaroundSeedSet) {
          const bool varyPerBar = block.turnaroundVaryPerOccurrence;
          const std::string key = block.id + "|" + std::to_string(varyPerBar ? barIdx : 0)
            + "|turnaroundfill|" + std::to_string(block.turnaroundSeed);
          distSeed = seededHash(key);
        }
        const auto& dist = getTurnaroundBarDistribution(block, barIdx, num, distSeed, cache);
        auto distIt = dist.find(tid);
        if (distIt != dist.end()) {
          for (double b : distIt->second) {
            if (!hasNoteNear(sourceBarBeats, b)) all.push_back({b + barIdx * num, block.turnaroundFillVel, true});
          }
        }
      }

      // Kick's own independent turnaround layer.
      if (turnaround && tid == "kick") {
        const double lengthBeats = resolveTurnaroundLengthBeats(block, num);
        const double kickSubdivision = std::max(static_cast<double>(block.turnaroundKickSubdivision), std::ceil(2.0 / lengthBeats - 1e-9));
        const int kickTotalSteps = static_cast<int>(num * kickSubdivision);
        const int kickZoneSteps = std::max(2, static_cast<int>(std::round(lengthBeats * kickSubdivision)));
        const int kickHits = std::max(1, static_cast<int>(std::round((block.turnaroundKickDensity / 100.0) * kickZoneSteps)));
        auto kickMask = buildEndAnchoredEuclidean(kickHits, kickZoneSteps, block.turnaroundKickSeed);
        for (int i = 0; i < static_cast<int>(kickMask.size()); i++) {
          if (!kickMask[i]) continue;
          const double beat = (kickTotalSteps - kickZoneSteps + i) / kickSubdivision;
          if (!hasNoteNear(sourceBarBeats, beat)) all.push_back({beat + barIdx * num, 115, true});
        }
      }

      // Resolution crash lands on downbeat of following bar (wraps if last bar).
      if (turnaround && block.turnaroundCrash && tid == "crash1" && !hasNoteNear(sourceBarBeats, 0.0)) {
        const int targetBarIdx = (barIdx + 1 < len) ? barIdx + 1 : 0;
        all.push_back({static_cast<double>(targetBarIdx * num), 115, true});
      }
    }

    std::sort(all.begin(), all.end(), [](const Note& a, const Note& b) { return a.beat < b.beat; });
    block.data[tid] = std::move(all);
  }
}

void applyArticulationVariation(Block& block, const Grid& grid, const std::string& defaultTrackId,
                                 const std::vector<ArticulationTier>& tiers, double variationPct,
                                 uint32_t artGroupSeed, int phrase,
                                 std::function<bool(const Note&)> noteFilter) {
  const double bias = variationPct / 100.0;

  auto srcIt = block.data.find(defaultTrackId);
  const std::vector<Note> allNotes = srcIt != block.data.end() ? srcIt->second : std::vector<Note>{};

  std::vector<Note> notes;
  std::vector<Note> untouched;
  if (noteFilter) {
    for (const auto& n : allNotes) (noteFilter(n) ? notes : untouched).push_back(n);
  } else {
    notes = allNotes;
  }

  std::unordered_map<std::string, std::vector<Note>> trackNotes;
  for (const auto& t : tiers) trackNotes[t.id];

  // Filter tiers by enabled articulations; if none, fall back to all tiers.
  std::vector<ArticulationTier> pickableTiers;
  for (const auto& t : tiers) {
    if (isArticulationEnabled(block, t.id, t.group)) pickableTiers.push_back(t);
  }
  const std::vector<ArticulationTier>& effectiveTiers = !pickableTiers.empty() ? pickableTiers : tiers;

  // Cache weighted pick per (turnaround occurrence, phrase bar, beat within bar) for consistency.
  const bool turnaroundVaryPerOccurrence = block.turnaroundVaryPerOccurrence;
  const double turnaroundPhraseBeats =
      static_cast<double>(block.turnaroundInterval > 0 ? block.turnaroundInterval : 1) * grid.numerator;
  const int phraseLen = phrase > 1 ? phrase : 1;

  std::unordered_map<std::string, std::string> choiceCache;
  for (const auto& n : notes) {
    const double numerator = static_cast<double>(grid.numerator);
    double beatWithinBar = std::fmod(n.beat, numerator);
    if (beatWithinBar < 0.0) beatWithinBar += numerator;

    const int turnaroundOccIdx = turnaroundVaryPerOccurrence
        ? static_cast<int>(std::floor(n.beat / turnaroundPhraseBeats)) : 0;
    const int phraseBarIdx = phraseLen > 1
        ? static_cast<int>(std::floor(n.beat / numerator)) % phraseLen : 0;

    char keyBuf[64];
    std::snprintf(keyBuf, sizeof(keyBuf), "%d|%d|%.4f", turnaroundOccIdx, phraseBarIdx, beatWithinBar);
    const std::string cacheKey(keyBuf);

    auto cacheIt = choiceCache.find(cacheKey);
    std::string chosen;
    if (cacheIt == choiceCache.end()) {
      RNG rng(artGroupSeed + seededHash(cacheKey));
      chosen = pickWeightedArticulation(rng, bias, effectiveTiers);
      choiceCache.emplace(cacheKey, chosen);
    } else {
      chosen = cacheIt->second;
    }
    trackNotes[chosen].push_back(n);
  }

  auto& defaultBucket = trackNotes[defaultTrackId];
  defaultBucket.insert(defaultBucket.end(), untouched.begin(), untouched.end());

  for (const auto& t : tiers) {
    auto& vec = trackNotes[t.id];
    std::sort(vec.begin(), vec.end(), [](const Note& a, const Note& b) { return a.beat < b.beat; });
    block.data[t.id] = std::move(vec);
  }
}

void applyKickGroupToBlock(Block& block, const Grid& grid, double densityPct, double variationPct,
                            int subdivisionIndex, Feel feel, int phrase, TurnaroundFillCache& cache,
                            const std::vector<double>& anchorBeats) {
  const double subdivision = subdivisionFromKnobIndex(subdivisionIndex);
  const uint32_t seed = groupSeed(block, "kick");
  const int hits = static_cast<int>(std::round((densityPct / 100.0) * grid.numerator * subdivision));

  // Add triplet overlay if enabled.
  std::vector<double> tripletBeats;
  auto tlIt = block.tripletLayers.find("kick");
  if (tlIt != block.tripletLayers.end() && !tlIt->second.empty()) {
    const int tripletSubdivision = static_cast<int>(std::round(subdivision * 1.5));
    tripletBeats = generateTripletLayerBeats(tlIt->second, densityPct, tripletSubdivision);
  }

  std::vector<Note> notes;
  for (int barIdx = 0; barIdx < phrase; barIdx++) {
    const uint32_t barSeed = barSeedFor(seed, phrase, barIdx);
    auto beats = generateAnchoredPattern(grid.numerator, subdivision, hits, feel, barSeed, anchorBeats);
    // Main beats first, then triplet beats.
    for (const auto& ab : beats) notes.push_back({ab.beat + barIdx * grid.numerator, 110});
    for (double b : tripletBeats) notes.push_back({b + barIdx * grid.numerator, 110});
  }
  block.data["kick"] = notes;
  replicateGroupAcrossLength(block, grid, {"kick"}, phrase, cache);

  applyArticulationVariation(block, grid, "kick", kKickArticulationTiers, variationPct, seed + 3, phrase);
}

void applyCustomGroupToBlock(Block& block, const Grid& grid, const std::string& trackId,
                              double densityPct, int subdivisionIndex, Feel feel, int phrase,
                              TurnaroundFillCache& cache) {
  const double subdivision = subdivisionFromKnobIndex(subdivisionIndex);
  const uint32_t seed = groupSeed(block, "custom:" + trackId);
  const int totalSteps = static_cast<int>(std::round(grid.numerator * subdivision));
  const int hits = static_cast<int>(std::round((densityPct / 100.0) * totalSteps));

  std::vector<Note> notes;
  for (int barIdx = 0; barIdx < phrase; barIdx++) {
    const uint32_t barSeed = barSeedFor(seed, phrase, barIdx);
    auto beats = generateAnchoredPattern(grid.numerator, subdivision, hits, feel, barSeed, {});
    for (const auto& ab : beats) notes.push_back({ab.beat + barIdx * grid.numerator, 110});
  }
  std::sort(notes.begin(), notes.end(), [](const Note& a, const Note& b) { return a.beat < b.beat; });
  block.data[trackId] = std::move(notes);
  replicateGroupAcrossLength(block, grid, { trackId }, phrase, cache);
}

void applySnareGroupToBlock(Block& block, const Grid& grid, double densityPct, double ghostDensityPct,
                             double variationPct, int subdivisionIndex, Feel feel, int phrase,
                             TurnaroundFillCache& cache, const std::vector<double>& anchorBeats) {
  const double subdivision = subdivisionFromKnobIndex(subdivisionIndex);
  const uint32_t seed = groupSeed(block, "snare");
  const int totalSteps = static_cast<int>(std::round(grid.numerator * subdivision));
  const int mainHits = static_cast<int>(std::round((densityPct / 100.0) * totalSteps));

  std::vector<Note> notes;
  for (int barIdx = 0; barIdx < phrase; barIdx++) {
    const uint32_t barSeed = barSeedFor(seed, phrase, barIdx);
    auto anchoredResult = generateAnchoredPattern(grid.numerator, subdivision, mainHits, feel, barSeed, anchorBeats);

    std::vector<double> ghostBeats;
    if (!anchorBeats.empty()) {
      // With anchors, compute occupancy from anchoredResult, then fill ghosts in empty steps.
      std::vector<bool> occupied(totalSteps, false);
      for (const auto& ab : anchoredResult) {
        int step = static_cast<int>(std::round(ab.beat * subdivision)) % totalSteps;
        if (step < 0) step += totalSteps;
        occupied[step] = true;
      }
      std::vector<int> emptySteps;
      for (int i = 0; i < totalSteps; i++) if (!occupied[i]) emptySteps.push_back(i);
      const int ghostHits = static_cast<int>(std::round((ghostDensityPct / 100.0) * emptySteps.size()));
      auto ghostMask = euclideanRhythm(ghostHits, static_cast<int>(emptySteps.size()), barSeed + 1);
      for (int i = 0; i < static_cast<int>(ghostMask.size()); i++) {
        if (!ghostMask[i]) continue;
        const int step = emptySteps[i];
        const int beatIdx = static_cast<int>(std::floor(step / subdivision));
        const double fracWithinBeat = std::fmod(step, subdivision) / subdivision;
        ghostBeats.push_back(beatIdx + applyFeelWarp(fracWithinBeat, feel));
      }
    } else {
      // No anchors: use generateSnareGhostBeats.
      ghostBeats = generateSnareGhostBeats(grid.numerator, subdivision, mainHits, ghostDensityPct, feel, barSeed);
    }

    for (const auto& ab : anchoredResult) notes.push_back({ab.beat + barIdx * grid.numerator, 110});
    for (double b : ghostBeats) notes.push_back({b + barIdx * grid.numerator, 45});
  }
  std::sort(notes.begin(), notes.end(), [](const Note& a, const Note& b) { return a.beat < b.beat; });
  block.data["snare"] = notes;
  replicateGroupAcrossLength(block, grid, {"snare"}, phrase, cache);

  // Variation applies only to main hits (vel > 60); ghosts stay plain snare.
  applyArticulationVariation(block, grid, "snare", kSnareArticulationTiers, variationPct, seed + 2, phrase,
                              [](const Note& n) { return n.vel > 60; });
}

void applySnareGhostLayer(Block& block, const Grid& grid, double ghostDensityPct, int subdivisionIndex, Feel feel,
                           int phrase, TurnaroundFillCache& cache) {
  const double subdivision = subdivisionFromKnobIndex(subdivisionIndex);
  const int totalSteps = static_cast<int>(std::round(grid.numerator * subdivision));
  const uint32_t seed = groupSeed(block, "snare");
  const int num = grid.numerator;

  std::vector<Note> notes;
  for (int barIdx = 0; barIdx < phrase; barIdx++) {
    const double lo = barIdx * num, hi = lo + num;
    const uint32_t barSeed = barSeedFor(seed, phrase, barIdx);

    // Gather main hits from all snare articulation tracks to determine occupied steps.
    std::vector<double> mainBeatsThisBar;
    for (const auto& tier : kSnareArticulationTiers) {
      auto it = block.data.find(tier.id);
      if (it == block.data.end()) continue;
      for (const auto& n : it->second) {
        if (n.turnaround || n.beat < lo || n.beat >= hi) continue;
        if (tier.id == "snare" && n.vel <= 60) continue;
        mainBeatsThisBar.push_back(n.beat - lo);
      }
    }

    std::vector<bool> occupied(totalSteps > 0 ? totalSteps : 0, false);
    for (double b : mainBeatsThisBar) {
      if (totalSteps <= 0) break;
      int step = static_cast<int>(std::round(b * subdivision)) % totalSteps;
      if (step < 0) step += totalSteps;
      occupied[step] = true;
    }
    std::vector<int> emptySteps;
    for (int i = 0; i < totalSteps; i++) if (!occupied[i]) emptySteps.push_back(i);

    const int ghostHits = static_cast<int>(std::round((ghostDensityPct / 100.0) * emptySteps.size()));
    auto ghostMask = euclideanRhythm(ghostHits, static_cast<int>(emptySteps.size()), barSeed + 1);

    for (size_t i = 0; i < ghostMask.size(); i++) {
      if (!ghostMask[i]) continue;
      const int step = emptySteps[i];
      const int beatIdx = static_cast<int>(std::floor(step / subdivision));
      const double fracWithinBeat = std::fmod(static_cast<double>(step), subdivision) / subdivision;
      const double beat = beatIdx + applyFeelWarp(fracWithinBeat, feel);
      notes.push_back({beat + lo, 45});
    }

    // Preserve existing main hits (vel>60, non-turnaround) on 'snare'.
    auto snareIt = block.data.find("snare");
    if (snareIt != block.data.end()) {
      for (const auto& n : snareIt->second) {
        if (n.vel > 60 && !n.turnaround && n.beat >= lo && n.beat < hi) notes.push_back(n);
      }
    }
  }

  std::sort(notes.begin(), notes.end(), [](const Note& a, const Note& b) { return a.beat < b.beat; });
  block.data["snare"] = std::move(notes);

  // Re-tile the phrase across block length.
  replicateGroupAcrossLength(block, grid, {"snare"}, phrase, cache);
}

void applyPulseGroupToBlock(Block& block, const Grid& grid, const std::string& groupId,
                             const std::string& primaryTrack, const std::vector<ArticulationTier>& tiers,
                             int mainVel, double subdivision, double densityPct, double variationPct,
                             uint32_t variationSeedOffset, Feel feel, int phrase, TurnaroundFillCache& cache) {
  const uint32_t seed = groupSeed(block, groupId);
  const int hits = static_cast<int>(std::round((densityPct / 100.0) * grid.numerator * subdivision));

  std::vector<Note> notes;
  for (int barIdx = 0; barIdx < phrase; barIdx++) {
    const uint32_t barSeed = barSeedFor(seed, phrase, barIdx);
    // Use plain Euclidean pattern (no anchors) for pulse groups.
    auto beats = generateGroupPattern(grid.numerator, subdivision, hits, feel, barSeed);
    for (double b : beats) notes.push_back({b + barIdx * grid.numerator, mainVel});
  }
  block.data[primaryTrack] = notes;
  replicateGroupAcrossLength(block, grid, {primaryTrack}, phrase, cache);

  applyArticulationVariation(block, grid, primaryTrack, tiers, variationPct, seed + variationSeedOffset, phrase);
}

void applyHihatGroupToBlock(Block& block, const Grid& grid, double densityPct, double variationPct,
                             int subdivisionIndex, Feel feel, int phrase, TurnaroundFillCache& cache) {
  const double subdivision = subdivisionFromKnobIndex(subdivisionIndex);
  // Variation seed offset +2 to differentiate.
  applyPulseGroupToBlock(block, grid, "hihat", "chh", kHihatArticulationTiers, 100, subdivision, densityPct,
                          variationPct, 2, feel, phrase, cache);
}

void applyRideGroupToBlock(Block& block, const Grid& grid, double densityPct, double variationPct,
                            int subdivisionIndex, Feel feel, int phrase, TurnaroundFillCache& cache) {
  const double subdivision = subdivisionFromKnobIndex(subdivisionIndex);
  applyPulseGroupToBlock(block, grid, "ride", "ride", kRideArticulationTiers, 100, subdivision, densityPct,
                          variationPct, 3, feel, phrase, cache);
}

void applyTomsGroupToBlock(Block& block, const Grid& grid, double densityPct, double variationPct,
                            int subdivisionIndex, Feel feel, int phrase, TurnaroundFillCache& cache) {
  const double subdivision = subdivisionFromKnobIndex(subdivisionIndex);
  applyPulseGroupToBlock(block, grid, "toms", "rackTom2Center", kTomsArticulationTiers, 100, subdivision,
                          densityPct, variationPct, 3, feel, phrase, cache);
}

void applyCymbalsGroupToBlock(Block& block, const Grid& grid, double densityPct, double variationPct,
                               int subdivisionIndex, Feel feel, int phrase, TurnaroundFillCache& cache) {
  const double subdivision = subdivisionFromKnobIndex(subdivisionIndex);
  // Main velocity 110 for cymbals (higher than other pulse groups).
  applyPulseGroupToBlock(block, grid, "cymbals", "crash1", kCymbalsArticulationTiers, 110, subdivision,
                          densityPct, variationPct, 3, feel, phrase, cache);
}

} // namespace dgg