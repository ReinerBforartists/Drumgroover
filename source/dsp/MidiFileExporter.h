// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Reiner Prokein
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "ArticulationAndCCMapper.h"
#include "Types.h"

namespace dgg {

// Builds Standard MIDI File (Type 1) bytes for a pattern (song or block).
class MidiFileExporter {
public:
  MidiFileExporter();

  // Builds a full multi‑track MIDI file with one track per drum voice plus
  // articulation CC automation tracks.
  std::vector<uint8_t> buildMidiBytes(const FullPattern& fullPattern, const ExportParams& params,
                                       const Grid& grid, const std::string& sectionMarkerLabel,
                                       const std::vector<Track>& tracks);

  // Builds a single‑track MIDI file with all notes merged; no CC tracks.
  // Used for drag‑and‑drop into FL Studio's Channel Rack.
  std::vector<uint8_t> buildMidiBytesSingleTrack(const FullPattern& fullPattern, const ExportParams& params,
                                                  const Grid& grid, const std::string& sectionMarkerLabel,
                                                  const std::vector<Track>& tracks);

  // Writes bytes to a file. Returns false on error.
  static bool writeToFile(const std::vector<uint8_t>& bytes, const std::string& path);

  static std::string slugify(const std::string& str);

private:
  int                     tpq_ = 480;
  ArticulationAndCCMapper mapper_;

  static std::vector<uint8_t> vlq(int n);
  static std::vector<uint8_t> writeMetaEvent(uint8_t type, const std::vector<uint8_t>& data);
  static std::vector<uint8_t> writeMetaEvent(uint8_t type, uint8_t data);

  std::vector<uint8_t>   writeTempoEvent(double bpm) const;
  std::vector<uint8_t>   writeTimeSigEvent(int numerator, int denominator, int clocksPerClick = 24, int notation = 4) const;
  std::vector<MidiEvent> writeKitMeta(double tick, const std::string& kitName, const std::string& genre) const;
  static std::vector<uint8_t> writeSectionMarker(const std::string& sectionName);

  static std::vector<uint8_t> writeTrackChunk(std::vector<MidiEvent> events);

  static const std::unordered_map<std::string, std::string>& trackNames();
};

} // namespace dgg