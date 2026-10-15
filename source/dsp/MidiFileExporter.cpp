#include "MidiFileExporter.h"
#include "808InstrumentTable.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>

namespace dgg {

MidiFileExporter::MidiFileExporter() = default;

std::vector<uint8_t> MidiFileExporter::vlq(int nIn) {
  uint32_t n = static_cast<uint32_t>(std::max(0, nIn));
  std::vector<uint8_t> buf = {static_cast<uint8_t>(n & 0x7f)};
  n >>= 7;
  while (n > 0) {
    buf.insert(buf.begin(), static_cast<uint8_t>((n & 0x7f) | 0x80));
    n >>= 7;
  }
  return buf;
}

std::vector<uint8_t> MidiFileExporter::writeMetaEvent(uint8_t type, const std::vector<uint8_t>& data) {
  // Data length is written as a single byte; safe for all current callers (<128 bytes).
  std::vector<uint8_t> bytes = {0xff, type};
  bytes.push_back(static_cast<uint8_t>(data.size()));
  bytes.insert(bytes.end(), data.begin(), data.end());
  return bytes;
}

std::vector<uint8_t> MidiFileExporter::writeMetaEvent(uint8_t type, uint8_t data) {
  return {0xff, type, data};
}

std::vector<uint8_t> MidiFileExporter::writeTempoEvent(double bpm) const {
  int us = static_cast<int>(std::round(60000000.0 / bpm));
  return writeMetaEvent(0x51, {
    static_cast<uint8_t>((us >> 16) & 0xff),
    static_cast<uint8_t>((us >> 8) & 0xff),
    static_cast<uint8_t>(us & 0xff)
  });
}

std::vector<uint8_t> MidiFileExporter::writeTimeSigEvent(int numerator, int denominator, int clocksPerClick, int notation) const {
  auto denLog2 = static_cast<uint8_t>(std::log2(static_cast<double>(denominator)));
  return writeMetaEvent(0x58, {
    static_cast<uint8_t>(numerator),
    denLog2,
    static_cast<uint8_t>(clocksPerClick),
    static_cast<uint8_t>(notation)
  });
}

std::vector<MidiEvent> MidiFileExporter::writeKitMeta(double tick, const std::string& kitName, const std::string& genre) const {
  std::vector<uint8_t> nameBytes(kitName.begin(), kitName.end());
  std::vector<uint8_t> genreBytes(genre.begin(), genre.end());
  return {
    {tick, writeMetaEvent(0x03, nameBytes)},
    {tick, writeMetaEvent(0x01, genreBytes)}
  };
}

std::vector<uint8_t> MidiFileExporter::writeSectionMarker(const std::string& sectionName) {
  std::vector<uint8_t> bytes(sectionName.begin(), sectionName.end());
  return writeMetaEvent(0x05, bytes);
}

std::vector<uint8_t> MidiFileExporter::writeTrackChunk(std::vector<MidiEvent> events) {
  std::sort(events.begin(), events.end(),
            [](const MidiEvent& a, const MidiEvent& b) { return a.tick < b.tick; });

  std::vector<uint8_t> trackData;
  double lastTick = 0.0;
  for (const auto& ev : events) {
    double deltaD = ev.tick - lastTick;
    if (deltaD < 0) continue;
    auto delta = vlq(static_cast<int>(deltaD));
    trackData.insert(trackData.end(), delta.begin(), delta.end());
    trackData.insert(trackData.end(), ev.data.begin(), ev.data.end());
    lastTick = ev.tick;
  }
  auto endDelta = vlq(0);
  trackData.insert(trackData.end(), endDelta.begin(), endDelta.end());
  trackData.push_back(0xff);
  trackData.push_back(0x2f);
  trackData.push_back(0x00);

  auto len = static_cast<uint32_t>(trackData.size());
  std::vector<uint8_t> out = {
    0x4d, 0x54, 0x72, 0x6b,
    static_cast<uint8_t>((len >> 24) & 0xff), static_cast<uint8_t>((len >> 16) & 0xff),
    static_cast<uint8_t>((len >> 8) & 0xff), static_cast<uint8_t>(len & 0xff)
  };
  out.insert(out.end(), trackData.begin(), trackData.end());
  return out;
}

const std::unordered_map<std::string, std::string>& MidiFileExporter::trackNames() {
  // the name of every kit track (instrument table), and the generic name of a custom track
  static const std::unordered_map<std::string, std::string> names = [] {
    std::unordered_map<std::string, std::string> m;
    for (const InstrumentDef& d : kInstruments)
      if (d.trackName) m[d.id] = d.trackName;
    m["custom"] = "Custom";
    return m;
  }();
  return names;
}

std::vector<uint8_t> MidiFileExporter::buildMidiBytes(const FullPattern& fullPattern, const ExportParams& params,
                                                        const Grid& grid, const std::string& sectionMarkerLabel,
                                                        const std::vector<Track>& tracks) {
  EnrichedPayload enriched = mapper_.enrichExportPayload(fullPattern, grid, tracks);
  int denominator = grid.denominator != 0 ? grid.denominator : 4;
  double ticksPerPulse = tpq_ * (4.0 / denominator);

  std::vector<std::vector<uint8_t>> trackChunks;

  // Meta track
  std::vector<MidiEvent> metaEvents;
  metaEvents.push_back({0.0, writeTempoEvent(params.bpm)});
  metaEvents.push_back({0.0, writeTimeSigEvent(grid.numerator, denominator)});
  auto kitMeta = writeKitMeta(0.0, params.kitName.empty() ? "DGG-Generated" : params.kitName,
                               params.genre.empty() ? "Custom" : params.genre);
  metaEvents.insert(metaEvents.end(), kitMeta.begin(), kitMeta.end());
  metaEvents.push_back({0.0, writeSectionMarker(sectionMarkerLabel)});
  trackChunks.push_back(writeTrackChunk(metaEvents));

  // Instrument tracks (one per drum voice)
  for (const auto& track : tracks) {
    auto it = enriched.tracks.find(track.id);
    if (it == enriched.tracks.end() || it->second.empty()) continue;
    const auto& evts = it->second;

    std::string nameStr;
    auto nameIt = trackNames().find(track.role);
    if (nameIt != trackNames().end()) nameStr = nameIt->second;
    else if (!track.name.empty()) nameStr = track.name;
    else nameStr = "Custom";

    std::vector<uint8_t> nameBytes(nameStr.begin(), nameStr.end());
    std::vector<MidiEvent> trackData;
    trackData.push_back({0.0, writeMetaEvent(0x03, nameBytes)});

    double noteOffOffset = std::max(10.0, std::round(grid.snap * ticksPerPulse * 0.5));

    for (const auto& e : evts)
      trackData.push_back({e.tick, {e.noteOn[0], e.noteOn[1], e.noteOn[2]}});
    for (const auto& e : evts)
      trackData.push_back({e.tick + noteOffOffset, {e.noteOff[0], e.noteOff[1], e.noteOff[2]}});

    trackChunks.push_back(writeTrackChunk(trackData));
  }

  // CC automation tracks
  for (const auto& track : tracks) {
    auto it = enriched.cc.find(track.id);
    if (it == enriched.cc.end() || it->second.empty()) continue;
    trackChunks.push_back(writeTrackChunk(it->second));
  }

  // Header. Track count fits in one byte (assumes <256 tracks).
  std::vector<uint8_t> header = {
    0x4d, 0x54, 0x68, 0x64, 0, 0, 0, 6, 0, 1,
    0, static_cast<uint8_t>(trackChunks.size() & 0xff),
    static_cast<uint8_t>((tpq_ >> 8) & 0xff), static_cast<uint8_t>(tpq_ & 0xff)
  };

  std::vector<uint8_t> out;
  size_t total = header.size();
  for (const auto& c : trackChunks) total += c.size();
  out.reserve(total);
  out.insert(out.end(), header.begin(), header.end());
  for (const auto& c : trackChunks) out.insert(out.end(), c.begin(), c.end());
  return out;
}

std::vector<uint8_t> MidiFileExporter::buildMidiBytesSingleTrack(const FullPattern& fullPattern, const ExportParams& params,
                                                                    const Grid& grid, const std::string& sectionMarkerLabel,
                                                                    const std::vector<Track>& tracks) {
  EnrichedPayload enriched = mapper_.enrichExportPayload(fullPattern, grid, tracks);
  int denominator = grid.denominator != 0 ? grid.denominator : 4;
  double ticksPerPulse = tpq_ * (4.0 / denominator);
  double noteOffOffset = std::max(10.0, std::round(grid.snap * ticksPerPulse * 0.5));

  std::vector<std::vector<uint8_t>> trackChunks;

  // Meta track (same as multi‑track version)
  std::vector<MidiEvent> metaEvents;
  metaEvents.push_back({0.0, writeTempoEvent(params.bpm)});
  metaEvents.push_back({0.0, writeTimeSigEvent(grid.numerator, denominator)});
  auto kitMeta = writeKitMeta(0.0, params.kitName.empty() ? "DGG-Generated" : params.kitName,
                               params.genre.empty() ? "Custom" : params.genre);
  metaEvents.insert(metaEvents.end(), kitMeta.begin(), kitMeta.end());
  metaEvents.push_back({0.0, writeSectionMarker(sectionMarkerLabel)});
  trackChunks.push_back(writeTrackChunk(metaEvents));

  // Single merged note track (all voices combined). Used for drag‑and‑drop
  // into FL Studio's Channel Rack where multiple tracks would spawn many channels.
  std::vector<uint8_t> nameBytes(sectionMarkerLabel.begin(), sectionMarkerLabel.end());
  std::vector<MidiEvent> trackData;
  trackData.push_back({0.0, writeMetaEvent(0x03, nameBytes)});

  for (const auto& track : tracks) {
    auto it = enriched.tracks.find(track.id);
    if (it == enriched.tracks.end() || it->second.empty()) continue;
    for (const auto& e : it->second) {
      trackData.push_back({e.tick, {e.noteOn[0], e.noteOn[1], e.noteOn[2]}});
      trackData.push_back({e.tick + noteOffOffset, {e.noteOff[0], e.noteOff[1], e.noteOff[2]}});
    }
  }
  trackChunks.push_back(writeTrackChunk(trackData));

  // Header: always 2 tracks (meta + merged note track)
  std::vector<uint8_t> header = {
    0x4d, 0x54, 0x68, 0x64, 0, 0, 0, 6, 0, 1,
    0, static_cast<uint8_t>(trackChunks.size() & 0xff),
    static_cast<uint8_t>((tpq_ >> 8) & 0xff), static_cast<uint8_t>(tpq_ & 0xff)
  };

  std::vector<uint8_t> out;
  size_t total = header.size();
  for (const auto& c : trackChunks) total += c.size();
  out.reserve(total);
  out.insert(out.end(), header.begin(), header.end());
  for (const auto& c : trackChunks) out.insert(out.end(), c.begin(), c.end());
  return out;
}

bool MidiFileExporter::writeToFile(const std::vector<uint8_t>& bytes, const std::string& path) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file.is_open()) return false;
  file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  return file.good();
}

std::string MidiFileExporter::slugify(const std::string& str) {
  std::string src = str.empty() ? "block" : str;

  std::string lowered;
  lowered.reserve(src.size());
  for (char c : src) lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

  std::string out;
  out.reserve(lowered.size());
  bool lastWasDash = false;
  for (char c : lowered) {
    bool isAlnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    if (isAlnum) { out.push_back(c); lastWasDash = false; }
    else if (!lastWasDash) { out.push_back('-'); lastWasDash = true; }
  }
  // Trim leading/trailing dashes
  size_t begin = out.find_first_not_of('-');
  if (begin == std::string::npos) return "block";
  size_t end = out.find_last_not_of('-');
  out = out.substr(begin, end - begin + 1);

  return out.empty() ? "block" : out;
}

} // namespace dgg