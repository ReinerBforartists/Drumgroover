// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Reiner Prokein
#include "MidiExportService.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace dgg {

std::string timestampSlug() {
  auto now = std::chrono::system_clock::now();
  std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tmBuf{};
#if defined(_WIN32)
  localtime_s(&tmBuf, &t);
#else
  localtime_r(&t, &tmBuf);
#endif
  std::ostringstream oss;
  oss << std::put_time(&tmBuf, "%Y%m%d-%H%M%S");
  return oss.str();
}

MidiExportService::MidiExportService(std::vector<Track> tracks) : tracks_(std::move(tracks)) {}

bool MidiExportService::exportSong(const FullPattern& fullPattern, const ExportParams& params,
                                    const Grid& grid, const std::string& path) {
  auto bytes = exporter_.buildMidiBytes(fullPattern, params, grid, "Full Pattern", tracks_);
  return MidiFileExporter::writeToFile(bytes, path);
}

bool MidiExportService::exportBlock(const Block& block, const ExportParams& params, const Grid& grid,
                                     const std::string& path) {
  FullPattern pattern = buildFullPatternFromBlock(block, tracks_);
  Grid blockGrid = grid;
  blockGrid.numerator = block.numerator; // blocks can differ from the global time signature — same as JS
  auto bytes = exporter_.buildMidiBytes(pattern, params, blockGrid,
                                         block.name.empty() ? "Block" : block.name, tracks_);
  return MidiFileExporter::writeToFile(bytes, path);
}

bool MidiExportService::exportBlockSingleTrack(const Block& block, const ExportParams& params, const Grid& grid,
                                                const std::string& path) {
  FullPattern pattern = buildFullPatternFromBlock(block, tracks_);
  Grid blockGrid = grid;
  blockGrid.numerator = block.numerator;
  auto bytes = exporter_.buildMidiBytesSingleTrack(pattern, params, blockGrid,
                                                    block.name.empty() ? "Block" : block.name, tracks_);
  return MidiFileExporter::writeToFile(bytes, path);
}

bool MidiExportService::exportFullPatternSingleTrack(const FullPattern& fullPattern, const ExportParams& params,
                                                       const Grid& grid, const std::string& sectionMarkerLabel,
                                                       const std::string& path) {
  auto bytes = exporter_.buildMidiBytesSingleTrack(fullPattern, params, grid, sectionMarkerLabel, tracks_);
  return MidiFileExporter::writeToFile(bytes, path);
}

std::string MidiExportService::defaultSongFilename() const {
  return "dgg-song-" + timestampSlug() + ".mid";
}

std::string MidiExportService::defaultBlockFilename(const Block& block, const std::string& stamp) const {
  return "dgg-block-" + MidiFileExporter::slugify(block.name) + "-" + stamp + ".mid";
}

} // namespace dgg
