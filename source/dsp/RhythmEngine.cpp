#include "RhythmEngine.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace dgg {

// ---- RNG ----
// JS: this.s = seed >>> 0 || (Date.now() & 0xffffffff)
// uint32_t arithmetic wraps the same way `>>> 0` masks in JS, so the LCG
// step below is bit-identical to `(this.s * 1664525 + 1013904223) >>> 0`.
RNG::RNG(uint32_t seed) {
  s = seed != 0 ? seed
                : static_cast<uint32_t>(
                      std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count() &
                      0xffffffffu);
}

double RNG::next() {
  s = static_cast<uint32_t>(s * 1664525u + 1013904223u);
  return static_cast<double>(s) / 4294967295.0; // 0xffffffff
}

int RNG::intRange(int a, int b) {
  return a + static_cast<int>(std::floor(next() * (b - a + 1)));
}

bool RNG::chance(double p) { return next() < p; }

bool hasNoteNear(const std::vector<double>& beats, double beat, double eps) {
  return std::any_of(beats.begin(), beats.end(),
                      [&](double n) { return std::fabs(n - beat) < eps; });
}

std::vector<int> backbeatPositions(int numerator) {
  std::vector<int> out;
  for (int b = 1; b < numerator; b += 2) out.push_back(b);
  return out;
}

std::vector<bool> euclideanRhythm(int hits, int steps, uint32_t seed) {
  if (steps <= 0) return {};
  if (hits <= 0) return std::vector<bool>(steps, false);
  if (hits >= steps) return std::vector<bool>(steps, true);

  std::vector<bool> raw;
  raw.reserve(steps);
  int bucket = 0;
  for (int i = 0; i < steps; i++) {
    bucket += hits;
    if (bucket >= steps) { bucket -= steps; raw.push_back(true); }
    else raw.push_back(false);
  }

  int firstHit = 0;
  while (firstHit < steps && !raw[firstHit]) firstHit++;
  // JS `raw.indexOf(true)` returns -1 if none found, then `.slice(-1)` /
  // `.slice(0,-1)` behave oddly — but that branch is unreachable here since
  // hits>0 guarantees at least one true, so firstHit < steps always holds.

  std::vector<bool> mask;
  mask.reserve(steps);
  for (int i = firstHit; i < steps; i++) mask.push_back(raw[i]);
  for (int i = 0; i < firstHit; i++) mask.push_back(raw[i]);

  return seed ? seededShuffleMask(std::move(mask), seed) : mask;
}

std::vector<bool> seededShuffleMask(std::vector<bool> mask, uint32_t seed) {
  RNG rng(seed);
  std::vector<bool> out = mask;
  int hitCount = static_cast<int>(std::count(out.begin(), out.end(), true));
  int shuffles = std::max(1, static_cast<int>(std::round(hitCount * 0.3)));
  const int n = static_cast<int>(out.size());

  for (int sIdx = 0; sIdx < shuffles; sIdx++) {
    std::vector<int> hitIdxs;
    for (int i = 0; i < n; i++) if (out[i]) hitIdxs.push_back(i);
    if (hitIdxs.empty()) break;

    int pick = hitIdxs[rng.intRange(0, static_cast<int>(hitIdxs.size()) - 1)];
    int dir = rng.chance(0.5) ? 1 : -1;
    int target = ((pick + dir) % n + n) % n;
    if (!out[target]) { out[pick] = false; out[target] = true; }
  }
  return out;
}

double applyFeelWarp(double frac, Feel feel) {
  double f = std::fmod(std::fmod(frac, 1.0) + 1.0, 1.0);
  if (feel == Feel::Swing) {
    double stepFrac = f * 2.0;
    double pairIdx = std::floor(stepFrac);
    double within = stepFrac - pairIdx;
    double warpedWithin = within < 0.5 ? within * (4.0 / 3.0)
                                        : (2.0 / 3.0) + (within - 0.5) * (2.0 / 3.0);
    return (pairIdx + warpedWithin) / 2.0;
  }
  // Straight (and Halftime, which never reaches here — see generateGroupPattern)
  return f;
}

double warpAbsoluteBeat(double absBeat, Feel feel) {
  double beatIdx = std::floor(absBeat);
  return beatIdx + applyFeelWarp(absBeat - beatIdx, feel);
}

std::vector<double> generateGroupPattern(int numerator, double stepsPerBeat, int hits, Feel feel, uint32_t seed) {
  if (feel == Feel::Halftime) {
    int halfSteps = static_cast<int>(std::round((numerator / 2.0) * stepsPerBeat));
    int halfHits = std::max(1, static_cast<int>(std::round(hits / 2.0)));
    auto mask = euclideanRhythm(halfHits, halfSteps, seed);
    std::vector<double> beats;
    for (int i = 0; i < static_cast<int>(mask.size()); i++) {
      if (!mask[i]) continue;
      int beatIdx = static_cast<int>(std::floor(i / stepsPerBeat));
      double fracWithinBeat = std::fmod(i, stepsPerBeat) / stepsPerBeat;
      beats.push_back((beatIdx + fracWithinBeat) * 2.0);
    }
    return beats;
  }

  int totalSteps = static_cast<int>(std::round(numerator * stepsPerBeat));
  auto mask = euclideanRhythm(hits, totalSteps, seed);
  std::vector<double> beats;
  for (int i = 0; i < static_cast<int>(mask.size()); i++) {
    if (!mask[i]) continue;
    int beatIdx = static_cast<int>(std::floor(i / stepsPerBeat));
    double fracWithinBeat = std::fmod(i, stepsPerBeat) / stepsPerBeat;
    beats.push_back(beatIdx + applyFeelWarp(fracWithinBeat, feel));
  }
  return beats;
}

std::vector<AnchoredBeat> generateAnchoredPattern(
    int numerator, double stepsPerBeat, int hits, Feel feel, uint32_t seed,
    const std::vector<double>& anchorBeats) {

  int totalSteps = static_cast<int>(std::round(numerator * stepsPerBeat));

  if (anchorBeats.empty() || hits <= 0) {
    auto beats = generateGroupPattern(numerator, stepsPerBeat, hits, feel, seed);
    std::vector<AnchoredBeat> out;
    out.reserve(beats.size());
    for (double b : beats) out.push_back({b, false});
    return out;
  }

  // anchorSteps: dedupe + range-clip, port of the JS `new Set(...)` build.
  std::vector<int> anchorStepsVec;
  for (double b : anchorBeats) {
    int st = static_cast<int>(std::round(b * stepsPerBeat));
    if (st >= 0 && st < totalSteps) anchorStepsVec.push_back(st);
  }
  std::sort(anchorStepsVec.begin(), anchorStepsVec.end());
  anchorStepsVec.erase(std::unique(anchorStepsVec.begin(), anchorStepsVec.end()), anchorStepsVec.end());

  std::vector<bool> isAnchorStep(totalSteps, false);
  for (int st : anchorStepsVec) isAnchorStep[st] = true;

  int embellishHits = std::max(0, hits - static_cast<int>(anchorStepsVec.size()));

  std::vector<int> freeSteps;
  for (int i = 0; i < totalSteps; i++) if (!isAnchorStep[i]) freeSteps.push_back(i);

  auto embellishMask = euclideanRhythm(embellishHits, static_cast<int>(freeSteps.size()), seed);

  // chosen: anchors first, then embellish hits — freeSteps/anchorSteps are
  // disjoint by construction so no overwrite ordering to worry about
  // (unlike the JS Map, which relies on that same disjointness).
  std::vector<AnchoredBeat> beats;
  beats.reserve(anchorStepsVec.size() + embellishMask.size());

  for (int st : anchorStepsVec) {
    int beatIdx = static_cast<int>(std::floor(st / stepsPerBeat));
    double fracWithinBeat = std::fmod(st, stepsPerBeat) / stepsPerBeat;
    beats.push_back({beatIdx + applyFeelWarp(fracWithinBeat, feel), true});
  }
  for (int i = 0; i < static_cast<int>(embellishMask.size()); i++) {
    if (!embellishMask[i]) continue;
    int st = freeSteps[i];
    int beatIdx = static_cast<int>(std::floor(st / stepsPerBeat));
    double fracWithinBeat = std::fmod(st, stepsPerBeat) / stepsPerBeat;
    beats.push_back({beatIdx + applyFeelWarp(fracWithinBeat, feel), false});
  }

  std::sort(beats.begin(), beats.end(), [](const AnchoredBeat& a, const AnchoredBeat& b) {
    return a.beat < b.beat;
  });
  return beats;
}

std::vector<double> generateTripletLayerBeats(
    const std::vector<BeatRange>& ranges, double densityPct, int tripletSubdivision) {
  if (ranges.empty()) return {};
  double stepSize = 1.0 / (tripletSubdivision > 0 ? tripletSubdivision : 3);

  std::vector<double> out;
  for (const auto& range : ranges) {
    int startStep = static_cast<int>(std::round(range.from / stepSize));
    int endStep = static_cast<int>(std::round(range.to / stepSize));
    int localSteps = endStep - startStep;
    if (localSteps <= 0) continue;

    int hits = static_cast<int>(std::round((densityPct / 100.0) * localSteps));
    auto mask = euclideanRhythm(hits, localSteps);
    for (int i = 0; i < static_cast<int>(mask.size()); i++) {
      if (mask[i]) out.push_back((startStep + i) * stepSize);
    }
  }
  return out;
}

std::vector<double> generateSnareGhostBeats(
    int numerator, double stepsPerBeat, int mainHits, double ghostDensityPct,
    Feel feel, uint32_t seed) {
  int totalSteps = static_cast<int>(std::round(numerator * stepsPerBeat));
  auto mainMask = euclideanRhythm(mainHits, totalSteps, seed);

  std::vector<int> emptyIdx;
  for (int i = 0; i < static_cast<int>(mainMask.size()); i++)
    if (!mainMask[i]) emptyIdx.push_back(i);
  if (emptyIdx.empty()) return {};

  int ghostHits = static_cast<int>(std::round((ghostDensityPct / 100.0) * emptyIdx.size()));
  auto ghostMask = euclideanRhythm(ghostHits, static_cast<int>(emptyIdx.size()), seed ? seed + 1 : 0);

  std::vector<double> beats;
  for (int i = 0; i < static_cast<int>(ghostMask.size()); i++) {
    if (!ghostMask[i]) continue;
    int step = emptyIdx[i];
    int beatIdx = static_cast<int>(std::floor(step / stepsPerBeat));
    double fracWithinBeat = std::fmod(step, stepsPerBeat) / stepsPerBeat;
    beats.push_back(beatIdx + applyFeelWarp(fracWithinBeat, feel));
  }
  return beats;
}

uint32_t seededHash(const std::string& str) {
  uint32_t h = 2166136261u;
  for (char c : str) {
    h ^= static_cast<uint8_t>(c);
    h = h * 16777619u; // unsigned overflow wraps like Math.imul + >>> 0
  }
  return h;
}

double articulationTierWeight(int tier, double bias) {
  switch (tier) {
    case 0: return std::pow(1.0 - bias, 1.3) * 10.0;
    case 1: return bias * (1.3 - bias * 0.3) * 4.0;
    case 2: return std::pow(bias, 2.0) * 2.5;
    case 3: return std::pow(bias, 3.5) * 3.0;
    default: return 0.0;
  }
}

std::string pickWeightedArticulation(RNG& rng, double bias, const std::vector<ArticulationTier>& tiers) {
  if (tiers.empty()) return {};

  std::vector<double> weights;
  weights.reserve(tiers.size());
  double total = 0.0;
  for (const auto& t : tiers) {
    double w = std::max(0.0, articulationTierWeight(t.tier, bias));
    weights.push_back(w);
    total += w;
  }

  if (total <= 0.0) return tiers[0].id;

  double r = rng.next() * total;
  for (size_t i = 0; i < tiers.size(); i++) {
    r -= weights[i];
    if (r <= 0.0) return tiers[i].id;
  }
  return tiers.back().id;
}

std::vector<bool> buildEndAnchoredEuclidean(int hits, int steps, uint32_t seed) {
  if (steps <= 0) return {};
  if (hits <= 0) return std::vector<bool>(steps, false);
  if (hits >= steps) return std::vector<bool>(steps, true);

  std::vector<bool> raw;
  raw.reserve(steps);
  int bucket = 0;
  for (int i = 0; i < steps; i++) {
    bucket += hits;
    if (bucket >= steps) { bucket -= steps; raw.push_back(true); }
    else raw.push_back(false);
  }

  // Port of `raw.lastIndexOf(true)` — hits>0 and hits<steps (both handled
  // above) guarantee at least one true, so lastHit >= 0 always holds, same
  // reasoning as euclideanRhythm's firstHit comment in this file.
  int lastHit = -1;
  for (int i = steps - 1; i >= 0; i--) { if (raw[i]) { lastHit = i; break; } }
  const int shift = steps - 1 - lastHit;

  std::vector<bool> mask(steps);
  for (int i = 0; i < steps; i++) mask[((i + shift) % steps + steps) % steps] = raw[i];

  return seed ? seededShuffleMask(mask, seed) : mask;
}

} // namespace dgg
