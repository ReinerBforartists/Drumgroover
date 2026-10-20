// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Reiner Prokein
#pragma once

// RhythmEngine — C++ port of rhythm-logic.js.
// Pure algorithm layer: no DOM, no iPlug2, no allocation-per-audio-block assumptions.
// Mirrors the JS function‑for‑function for diffability.

#include <cstdint>
#include <string>
#include <vector>

namespace dgg {

// ---- RNG (port of JS `class RNG`) ----
// Same LCG as the JS version; must stay bit‑identical for reproducible seeds.
class RNG {
public:
  explicit RNG(uint32_t seed);
  double next();               // [0, 1)
  int    intRange(int a, int b); // inclusive both ends
  bool   chance(double p);

private:
  uint32_t s;
};

// Port of `hasNoteNear(arr, beat, eps)`.
bool hasNoteNear(const std::vector<double>& beats, double beat, double eps = 0.06);

// Port of `backbeatPositions(numerator)`.
std::vector<int> backbeatPositions(int numerator);

// ---- Euclidean distribution ----
// seed == 0 means "no seed given" → skip shuffle.
std::vector<bool> euclideanRhythm(int hits, int steps, uint32_t seed = 0);
std::vector<bool> seededShuffleMask(std::vector<bool> mask, uint32_t seed);

// ---- Pattern modes ----
// A mode scores every step; the `hits` best-scoring steps get a hit. Mirrors patternMask in rhythm-logic.js
// (integer or quantized scores, equal scores ordered by a seed hash).
enum class PatternMode { Classic, Metric, Sine, Bit, Logistic };

PatternMode patternModeFromString(const std::string& s); // unknown -> Classic
const char* patternModeToString(PatternMode mode);

// positions: absolute step indices within the bar grid; total: steps per bar grid.
// anchorEnd forces the last step to be a hit (Break/Fill zones end on a hit).
std::vector<bool> patternMask(PatternMode mode, int hits, const std::vector<int>& positions,
                              uint32_t seed, double stepsPerBeat, int total, bool anchorEnd = false);
std::vector<int> stepRange(int n);

// ---- Feel warp ----
// Only "straight" and "swing" exist in the JS map; "halftime" is handled separately.
enum class Feel { Straight, Swing, Halftime };

double applyFeelWarp(double frac, Feel feel);
double warpAbsoluteBeat(double absBeat, Feel feel);

// Returns absolute beat positions (0‑based, within one bar/window).
std::vector<double> generateGroupPattern(int numerator, double stepsPerBeat, int hits, Feel feel, uint32_t seed = 0,
                                         PatternMode mode = PatternMode::Classic);

// ---- Anchored pattern ----
struct AnchoredBeat {
  double beat;
  bool   isAnchor;
};

// anchorBeats: optional; empty vector falls back to generateGroupPattern with isAnchor=false.
std::vector<AnchoredBeat> generateAnchoredPattern(
    int numerator, double stepsPerBeat, int hits, Feel feel, uint32_t seed,
    const std::vector<double>& anchorBeats, PatternMode mode = PatternMode::Classic);

// ---- Triplet / independent‑layer beats ----
struct BeatRange { double from; double to; };

// tripletSubdivision <= 0 falls back to 3.
std::vector<double> generateTripletLayerBeats(
    const std::vector<BeatRange>& ranges, double densityPct, int tripletSubdivision = 3);

// ---- Snare ghost notes ----
std::vector<double> generateSnareGhostBeats(
    int numerator, double stepsPerBeat, int mainHits, double ghostDensityPct,
    Feel feel, uint32_t seed, PatternMode mode = PatternMode::Classic);

// ---- Misc ----
// FNV‑1a, matches the JS bit‑for‑bit.
uint32_t seededHash(const std::string& str);

// ---- Weighted articulation tiers ----
// tier (0‑3) indexes articulationTierWeight arrays.
double articulationTierWeight(int tier, double bias);

struct ArticulationTier {
  std::string id;
  int         tier; // 0‑3
  std::string group; // per‑instrument whitelist scope
};

// Returns the chosen tier's id; matches the JS weighted‑random‑pick loop.
std::string pickWeightedArticulation(RNG& rng, double bias, const std::vector<ArticulationTier>& tiers);

// ---- End‑anchored Euclidean distribution (Break/Turnaround fill zones) ----
// Bucket‑fill rotated so the LAST hit lands on the LAST step.
// seed == 0 skips shuffle.
std::vector<bool> buildEndAnchoredEuclidean(int hits, int steps, uint32_t seed = 0);

// End-anchored mask over steps 0..steps-1; Classic is buildEndAnchoredEuclidean.
std::vector<bool> endAnchoredMask(PatternMode mode, int hits, int steps, uint32_t seed, double stepsPerBeat);

} // namespace dgg