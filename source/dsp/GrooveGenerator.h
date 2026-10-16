// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Reiner Prokein
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "RhythmEngine.h"
#include "Types.h"

namespace dgg {

// SUBDIVISION_KNOB_VALUES — index (0-3) as exposed by a UI knob,
// mapped to steps-per-beat (8th/16th/32nd/64th).
constexpr int kSubdivisionKnobValues[4] = {2, 4, 8, 16};

// subdivisionFromKnobIndex: index-to-value mapping, with fallback to 4
// for out-of-range indices.
double subdivisionFromKnobIndex(int index);

// Gets group seed: prefers block.seeds[groupId] if set, otherwise
// falls back to deterministic hash.
uint32_t groupSeed(const Block& block, const std::string& groupId);

// ---- Articulation tier tables ----
inline const std::vector<ArticulationTier> kKickArticulationTiers = {
  {"kick", 0, "kick"}, {"kickOpen", 1, "kick"},
};
inline const std::vector<ArticulationTier> kSnareArticulationTiers = {
  {"snare", 0, "snare"}, {"snareOffCenter", 1, "snare"}, {"snareEdge", 1, "snare"},
  {"snareRimShot", 2, "snare"}, {"rim", 2, "snare"}, {"snareRimOnly", 3, "snare"},
};
inline const std::vector<ArticulationTier> kHihatArticulationTiers = {
  {"chh", 0, "hihat"}, {"hihatTightEdge", 1, "hihat"}, {"hihatClosedTip", 1, "hihat"},
  {"hihatTightTip", 1, "hihat"}, {"ohh", 2, "hihat"}, {"hihatOpenEdge4", 3, "hihat"},
};
inline const std::vector<ArticulationTier> kRideArticulationTiers = {
  {"ride", 0, "ride"}, {"rideBowShank", 1, "ride"}, {"rideEdge", 1, "ride"},
  {"rideBellTip", 2, "ride"}, {"rideBell", 2, "ride"}, {"rideMuteHit", 3, "ride"},
};
inline const std::vector<ArticulationTier> kTomsArticulationTiers = {
  {"rackTom2Center", 0, "rackTom2"},
  {"tomHi", 1, "rackTom1"}, {"floorTom1Center", 1, "floorTom1"},
  {"tomMid", 2, "rackTom3"},
  {"tomLow", 3, "floorTom2"},
};
inline const std::vector<ArticulationTier> kCymbalsArticulationTiers = {
  {"crash1", 0, "cymbal2"},
  {"cymbal1Crash", 1, "cymbal1"}, {"cymbal3Crash", 1, "cymbal3"},
  {"crash2", 2, "cymbal4"},
  {"cymbal5Crash", 3, "cymbal5"},
};

// ---- Enabled-articulations whitelist ----
// Default enabled articulations for sub‑groups when
// block.enabledArticulations has no entry. Kick, cymbals and toms
// without explicit entry default to all enabled.
inline const std::unordered_map<std::string, std::vector<std::string>> kDefaultEnabledArticulations = {
  {"snare", {"snare", "snareOffCenter"}},
  {"hihat", {"chh", "ohh"}},
  {"ride", {"ride", "rideBowShank"}},
  {"rackTom1", {"tomHi", "rackTom1RimShot"}},
  {"rackTom2", {"rackTom2Center", "rackTom2RimShot"}},
  {"rackTom3", {"tomMid", "rackTom3RimShot"}},
  {"floorTom1", {"floorTom1Center", "floorTom1RimShot"}},
  {"floorTom2", {"tomLow", "floorTom2RimShot"}},
};

// Checks if articulation is enabled for the given track and sub‑group.
// subGroup is the ArticulationTier's own `group` field, not the six‑engine‑group id.
bool isArticulationEnabled(const Block& block, const std::string& trackId, const std::string& subGroup);

// =========================================
// Turnaround fill support (used by replicateGroupAcrossLength)
// =========================================

// Which real tracks each turnaround instrument checkbox contributes to
// the shared fill voice pool.
inline const std::unordered_map<std::string, std::vector<std::string>> kTurnaroundInstrumentTracks = {
  {"toms", {"tomHi", "rackTom2Center", "tomMid"}},
  {"snare", {"snare"}},
  {"hihat", {"chh"}},
  {"floorTom", {"tomLow"}},
  {"crash", {"crash1"}},
};

// Turnaround Length as an absolute beat count, capped at one full bar.
double resolveTurnaroundLengthBeats(const Block& block, int numerator);

// Per‑regeneration‑pass cache for turnaround fill distributions.
// One instance should be created at the start of each pass and passed
// to all apply*GroupToBlock calls to ensure consistent fill across groups.
using TurnaroundFillCache = std::unordered_map<int, std::unordered_map<std::string, std::vector<double>>>;

// Returns a reference into `cache` (populated on first call for barIdx,
// reused on subsequent calls) – trackId -> list of beat positions
// (bar‑relative) assigned to that track for this turnaround bar.
// Empty map if block.turnaroundInstruments resolves to no tracks.
const std::unordered_map<std::string, std::vector<double>>& getTurnaroundBarDistribution(
    const Block& block, int barIdx, int numerator, uint32_t seed, TurnaroundFillCache& cache);

// =========================================
// Break / Fill hand patterns (fixed reference shape, pre‑Reroll)
// =========================================

// HandPatternSpec: track id plus how many consecutive hits that track
// gets in the fixed run.
struct HandPatternSpec {
  std::string id;
  int         count;
};

// Fixed hand patterns used before user rerolls: descending tom run or
// snare‑led run.
inline const std::vector<HandPatternSpec> kBreakHandPatternToms = {
  {"tomHi", 4}, {"rackTom2Center", 2}, {"tomMid", 2},
};
inline const std::vector<HandPatternSpec> kBreakHandPatternSnareToms = {
  {"snare", 4}, {"tomHi", 2}, {"rackTom2Center", 2},
};

// BeatDistribution: trackId -> list of beat positions, window‑relative
// (offset already applied; caller re‑adds beatOffset when splicing).
using BeatDistribution = std::unordered_map<std::string, std::vector<double>>;

// Generates a fixed hand pattern. Right‑anchored by default: hits occupy
// the last N steps of the window (N = total hit count), landing exactly
// on the window's final step. anchorStart=true left‑anchors instead,
// starting at step 0. Extra steps remain silent. beatOffset is the
// window's absolute start (used for warping). Half‑Time halves each
// instrument's hit count rather than stretching the run.
BeatDistribution generateFixedBreakHandPattern(double breakBeats, double stepsPerBeat,
                                                const std::vector<HandPatternSpec>& patternSpec, bool anchorStart,
                                                Feel feel, double beatOffset);

// Generates a seeded Euclidean fill (used after Reroll). instruments
// is resolved via kTurnaroundInstrumentTracks; empty/absent falls back
// to {"toms"}. seed==0 disables the Euclidean shuffle but not the
// voice‑order shuffle – voice RNG is seeded off `seed+1`. Returns beats
// window‑relative.
BeatDistribution generateBreakFill(double breakBeats, const std::vector<std::string>& instruments,
                                    double densityPct, uint32_t seed, double stepsPerBeat, Feel feel,
                                    double beatOffset);

// =========================================
// Break block generation
// =========================================

// UI display‑name table for section labels; used to derive Break block names.
inline const std::unordered_map<std::string, std::string> kSectionLabels = {
  {"intro", "Intro"},     {"verse", "Verse"},   {"chorus", "Chorus"}, {"bridge", "Bridge"},
  {"outro", "Outro"},     {"fill", "Fill"},     {"verseBreak", "Verse Break"},
  {"chorusBreak", "Chorus Break"}, {"bridgeBreak", "Bridge Break"},
};

// Options for generateBreakBlock. breakSeedSet: false means use fixed
// pattern (no reroll); true means use seeded fill.
struct BreakBlockOptions {
  std::vector<std::string> breakInstruments = {"toms"};
  double   breakDensity     = 100.0;
  int      breakFillVel     = 112;
  double   breakSubdivision = 4.0;
  bool     breakSeedSet     = false;
  uint32_t breakSeed        = 0;
  double   breakKickDensity     = 100.0;
  double   breakKickSubdivision = 1.0;
  uint32_t breakKickSeed        = 0;
};

// Generates a Break block: full parent section with trailing `breakBeats`
// replaced by a fill (fixed hand pattern or seeded Euclidean), plus an
// independent Kick layer. Parent is already resolved (caller responsibility).
// Feel is left default – Break inherits feel from parent.
Block generateBreakBlock(const Block& parent, const Grid& grid, const std::string& id,
                          const std::string& parentId, double totalLength, double breakBeats,
                          const BreakBlockOptions& opts);

// =========================================
// Fill block generation (Intro/Outro – standalone)
// =========================================

// Options for generateFillBlock. fillSeedSet: false means fixed pattern,
// true means seeded fill.
struct FillBlockOptions {
  std::vector<std::string> fillInstruments = {"toms"};
  double   fillDensity     = 100.0;
  int      fillVel         = 112;
  double   fillSubdivision = 4.0;
  bool     fillSeedSet     = false;
  uint32_t fillSeed        = 0;
  double   fillKickDensity     = 100.0;
  double   fillKickSubdivision = 1.0;
  uint32_t fillKickSeed        = 0;
};

// Generates a standalone Fill block (Intro/Outro). anchorStart=false
// (Intro) resolves into what follows; anchorStart=true (Outro) places
// the fill at the block's start. Feel is stored on the block.
Block generateFillBlock(const Grid& grid, const std::string& id, const std::string& name, double totalLength,
                         double fillBeats, bool anchorStart, Feel feel, const FillBlockOptions& opts);

// Splices `fresh`'s break/fill tail (beat >= grooveBeats) into `block.data`
// for the given tracks; everything before grooveBeats stays unchanged.
void spliceBreakTail(Block& block, const Block& fresh, double grooveBeats, const std::vector<std::string>& tracks);

// Default number of trailing beats that form the actual break when not
// explicitly set.
constexpr double kBreakDefaultBeats = 2.0;

// Returns the set of tracks that the Break/Fill pool may write to.
std::unordered_set<std::string> breakFillTargetTracks();

// Computes a fresh Break block from the current settings of `block`.
// Returns nullopt if block is not a Break block.
std::optional<Block> computeFreshBreak(const Block& block, const Block& parent, const Grid& grid);

// Regenerates a Break block in place from its parent. mainLocked and
// kickLocked indicate whether the respective tails are locked (skip refresh).
bool regenerateBreakBlockInPlace(Block& block, const Block& parent, const Grid& grid, bool mainLocked,
                                  bool kickLocked);

// Regenerates only the Fill‑instrument (Main) tail of a Break block.
bool regenerateBreakMainInPlace(Block& block, const Block& parent, const Grid& grid);

// Regenerates only the Kick tail of a Break block.
bool regenerateBreakKickInPlace(Block& block, const Block& parent, const Grid& grid);

// Regenerates a standalone Fill block in place from its current settings.
// No lock parameters – standalone fills have no per‑group locks.
bool regenerateFillBlockInPlace(Block& block, const Grid& grid);

// Replicates a phrase‑bar pattern across the full block length:
// tiles the phrase, applies Velocity Build, and layers in turnaround fills
// every turnaroundInterval bars. Overwrites block.data for all trackIds.
void replicateGroupAcrossLength(Block& block, const Grid& grid, const std::vector<std::string>& trackIds,
                                 int phraseLen, TurnaroundFillCache& cache);

// =========================================
// Articulation variation
// =========================================

// Redistributes notes from defaultTrackId across the given articulation
// tiers via weighted random pick, deterministic per block+group.
// noteFilter, if provided, selects which notes are eligible for redistribution.
void applyArticulationVariation(Block& block, const Grid& grid, const std::string& defaultTrackId,
                                 const std::vector<ArticulationTier>& tiers, double variationPct,
                                 uint32_t artGroupSeed, int phrase,
                                 std::function<bool(const Note&)> noteFilter = nullptr);

// =========================================
// The six engine groups
// =========================================

// Kick group: main hits on "kick", variation redistributes to "kickOpen".
// densityPct 0‑100, subdivisionIndex 0‑3. anchorBeats: optional preset anchors.
void applyKickGroupToBlock(Block& block, const Grid& grid, double densityPct, double variationPct,
                            int subdivisionIndex, Feel feel, int phrase, TurnaroundFillCache& cache,
                            const std::vector<double>& anchorBeats = {});

// Custom instrument group: one track per custom instrument, single articulation —
// no variation tiers, no ghost layer. Group id is "custom:<trackId>" (seeds/phrase).
void applyCustomGroupToBlock(Block& block, const Grid& grid, const std::string& trackId,
                              double densityPct, int subdivisionIndex, Feel feel, int phrase,
                              TurnaroundFillCache& cache);

// Snare group: main hits plus ghost notes (ghostDensityPct), variation
// redistributes across snare articulations. anchorBeats switches ghost
// computation to anchored mode.
void applySnareGroupToBlock(Block& block, const Grid& grid, double densityPct, double ghostDensityPct,
                             double variationPct, int subdivisionIndex, Feel feel, int phrase,
                             TurnaroundFillCache& cache, const std::vector<double>& anchorBeats = {});

// Standalone ghost‑only regen. Does not touch main hits or articulation.
// Caller must pass the same subdivisionIndex and phrase that were used for
// the main pattern currently on the block.
void applySnareGhostLayer(Block& block, const Grid& grid, double ghostDensityPct, int subdivisionIndex, Feel feel,
                           int phrase, TurnaroundFillCache& cache);

// Shared engine for the four pulse groups (Hi‑Hat, Ride, Toms, Cymbals):
// generates a Euclidean pulse on primaryTrack, tiles via replicate,
// then applies articulation variation.
void applyPulseGroupToBlock(Block& block, const Grid& grid, const std::string& groupId,
                             const std::string& primaryTrack, const std::vector<ArticulationTier>& tiers,
                             int mainVel, double subdivision, double densityPct, double variationPct,
                             uint32_t variationSeedOffset, Feel feel, int phrase, TurnaroundFillCache& cache);

// Hi‑Hat group: writes 'chh', variation = open percentage.
void applyHihatGroupToBlock(Block& block, const Grid& grid, double densityPct, double variationPct,
                             int subdivisionIndex, Feel feel, int phrase, TurnaroundFillCache& cache);

// Ride group: writes 'ride'.
void applyRideGroupToBlock(Block& block, const Grid& grid, double densityPct, double variationPct,
                            int subdivisionIndex, Feel feel, int phrase, TurnaroundFillCache& cache);

// Toms group: writes 'rackTom2Center', variation redistributes across 5 tom voices.
void applyTomsGroupToBlock(Block& block, const Grid& grid, double densityPct, double variationPct,
                            int subdivisionIndex, Feel feel, int phrase, TurnaroundFillCache& cache);

// Cymbals group: writes 'crash1', variation redistributes across 5 cymbals.
// mainVel is 110 (higher than other pulse groups).
void applyCymbalsGroupToBlock(Block& block, const Grid& grid, double densityPct, double variationPct,
                               int subdivisionIndex, Feel feel, int phrase, TurnaroundFillCache& cache);

} // namespace dgg