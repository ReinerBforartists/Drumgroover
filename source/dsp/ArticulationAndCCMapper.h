// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Reiner Prokein
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "Types.h"

namespace dgg {

// MIDI ticks per quarter note (shared with MidiFileExporter).
constexpr int kDefaultTpq = 480;

// Velocity band for articulation mapping. Only the upper bound is used
// by mapArticulation; the lower bound is kept for completeness.
struct VelocityBand { int min; int max; };
struct ArticulationProfile {
  VelocityBand soft, normal, hard, accent;
};

// Default CC values for each role (CC1, CC10, CC11).
struct CCDefaults { uint8_t cc1; uint8_t cc10; uint8_t cc11; };

// Result of mapArticulation: articulation type and its index within the role.
struct ArticulationResult { std::string type; int index; };

// MIDI event with absolute tick (double) and raw bytes.
struct MidiEvent { double tick; std::vector<uint8_t> data; };

// Enriched note event for export: includes articulation info and velocity.
struct EnrichedNoteEvent {
  double            tick;
  std::array<uint8_t, 3> noteOn;
  std::array<uint8_t, 3> noteOff;
  ArticulationResult articulation;
  int               velocity;
};

struct EnrichedPayload {
  std::unordered_map<std::string, std::vector<EnrichedNoteEvent>> tracks;
  std::unordered_map<std::string, std::vector<MidiEvent>>         cc;
};

// Maps note velocity to articulation band and generates CC automation
// streams for SD3/Battery 4-style sample instruments.
class ArticulationAndCCMapper {
public:
  ArticulationAndCCMapper();

  // Determines articulation type and index for a given role and velocity.
  // Unknown roles fall back to the "custom" profile.
  ArticulationResult mapArticulation(const std::string& role, int velocity) const;

  // Generates CC1/CC10/CC11 automation events for the given role over the
  // total duration (in ticks) at the specified TPQ.
  std::vector<MidiEvent> generateCCAutomation(const std::string& role, double totalTicks, int tpq) const;

  // Enriches the full pattern with articulation and CC data for the given tracks.
  EnrichedPayload enrichExportPayload(const FullPattern& fullPattern, const Grid& grid,
                                       const std::vector<Track>& tracks) const;

private:
  std::unordered_map<std::string, ArticulationProfile> articulationProfiles_;
  std::unordered_map<std::string, CCDefaults>           ccDefaults_;

  const ArticulationProfile& profileFor(const std::string& role) const;
  const CCDefaults&          ccFor(const std::string& role) const;
};

} // namespace dgg