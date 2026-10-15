#pragma once

#include <string>
#include <vector>

#include "MidiFileExporter.h"
#include "Types.h"

namespace dgg {

// Returns a timestamp string in YYYYMMDD-HHMMSS format for filenames.
std::string timestampSlug();

// Handles exporting MIDI files for a full pattern (song) or a single block.
// Provides variants for single-track output (merged notes) used for drag‑and‑drop
// into FL Studio's Channel Rack.
class MidiExportService {
public:
  explicit MidiExportService(std::vector<Track> tracks);

  // Exports the given full pattern to a multi‑track MIDI file.
  bool exportSong(const FullPattern& fullPattern, const ExportParams& params,
                   const Grid& grid, const std::string& path);

  // Exports a single block as a multi‑track MIDI file.
  bool exportBlock(const Block& block, const ExportParams& params, const Grid& grid,
                    const std::string& path);

  // Exports a single block as a single‑track MIDI file (merged notes, no CC).
  // Used for drag‑out into FL Studio's Channel Rack.
  bool exportBlockSingleTrack(const Block& block, const ExportParams& params, const Grid& grid,
                               const std::string& path);

  // Exports an already‑assembled FullPattern as a single‑track MIDI file.
  // Used for multi‑clip Timeline selection drag‑out.
  bool exportFullPatternSingleTrack(const FullPattern& fullPattern, const ExportParams& params,
                                     const Grid& grid, const std::string& sectionMarkerLabel,
                                     const std::string& path);

  // Returns default filenames.
  std::string defaultSongFilename() const;
  std::string defaultBlockFilename(const Block& block, const std::string& stamp) const;

private:
  std::vector<Track> tracks_;
  MidiFileExporter   exporter_;
};

} // namespace dgg