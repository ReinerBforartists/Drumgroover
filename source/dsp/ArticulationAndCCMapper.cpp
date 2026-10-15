#include "ArticulationAndCCMapper.h"

#include <algorithm>
#include <cmath>

namespace dgg {

ArticulationAndCCMapper::ArticulationAndCCMapper() {
  // Velocity bands per role — ported verbatim from app.js `articulationProfiles`.
  articulationProfiles_ = {
    {"kick",     {{1,40},{41,80},{81,110},{111,127}}},
    {"snare",    {{1,45},{46,85},{86,115},{116,127}}},
    {"rim",      {{1,40},{41,75},{76,105},{106,127}}},
    {"chh",      {{1,50},{51,90},{91,115},{116,127}}},
    {"ohh",      {{1,50},{51,90},{91,115},{116,127}}},
    {"hhPedal",  {{1,45},{46,80},{81,105},{106,127}}},
    {"ride",     {{1,45},{46,85},{86,110},{111,127}}},
    {"rideBell", {{1,50},{51,90},{91,115},{116,127}}},
    {"tomLow",   {{1,45},{46,80},{81,105},{106,127}}},
    {"tomMid",   {{1,45},{46,80},{81,105},{106,127}}},
    {"tomHi",    {{1,45},{46,80},{81,105},{106,127}}},
    {"crash1",   {{1,50},{51,90},{91,115},{116,127}}},
    {"crash2",   {{1,50},{51,90},{91,115},{116,127}}},
    {"custom",   {{1,45},{46,80},{81,105},{106,127}}},
  };

  // CC defaults per role — ported verbatim from app.js `ccDefaults`.
  ccDefaults_ = {
    {"kick",     {80, 64, 100}},
    {"snare",    {80, 64, 95}},
    {"rim",      {75, 60, 90}},
    {"chh",      {85, 40, 100}},
    {"ohh",      {85, 80, 100}},
    {"hhPedal",  {70, 64, 85}},
    {"ride",     {80, 70, 95}},
    {"rideBell", {75, 65, 90}},
    {"tomLow",   {80, 30, 95}},
    {"tomMid",   {80, 50, 95}},
    {"tomHi",    {80, 70, 95}},
    {"crash1",   {85, 75, 100}},
    {"crash2",   {85, 80, 100}},
    {"custom",   {80, 64, 90}},
  };
}

const ArticulationProfile& ArticulationAndCCMapper::profileFor(const std::string& role) const {
  auto it = articulationProfiles_.find(role);
  return it != articulationProfiles_.end() ? it->second : articulationProfiles_.at("custom");
}

const CCDefaults& ArticulationAndCCMapper::ccFor(const std::string& role) const {
  auto it = ccDefaults_.find(role);
  return it != ccDefaults_.end() ? it->second : ccDefaults_.at("custom");
}

ArticulationResult ArticulationAndCCMapper::mapArticulation(const std::string& role, int velocity) const {
  const auto& profile = profileFor(role);
  // Only the upper bound of each band is checked, same as the JS original
  // (profile.soft[1] / .normal[1] / .hard[1]) — the lower bounds are
  // unused by this function, kept only for data fidelity.
  if (velocity <= profile.soft.max)   return {"soft", 0};
  if (velocity <= profile.normal.max) return {"normal", 1};
  if (velocity <= profile.hard.max)   return {"hard", 2};
  return {"accent", 3};
}

std::vector<MidiEvent> ArticulationAndCCMapper::generateCCAutomation(const std::string& role, double totalTicks, int tpq) const {
  const auto& cc = ccFor(role);
  std::vector<MidiEvent> events;
  double step = static_cast<double>(tpq);
  for (double tick = 0.0; tick <= totalTicks; tick += step) {
    events.push_back({tick, {0xb9, 1,  cc.cc1}});
    events.push_back({tick, {0xb9, 10, cc.cc10}});
    events.push_back({tick, {0xb9, 11, cc.cc11}});
  }
  return events;
}

EnrichedPayload ArticulationAndCCMapper::enrichExportPayload(
    const FullPattern& fullPattern, const Grid& grid, const std::vector<Track>& tracks) const {

  constexpr int tpq = kDefaultTpq;
  int denominator = grid.denominator != 0 ? grid.denominator : 4;
  double ticksPerPulse = tpq * (4.0 / denominator);
  double totalTicks = fullPattern.totalBeats * ticksPerPulse;

  EnrichedPayload enriched;
  for (const auto& t : tracks) {
    enriched.tracks[t.id] = {};
    enriched.cc[t.id] = {};
  }

  for (const auto& ev : fullPattern.events) {
    auto trackIt = std::find_if(tracks.begin(), tracks.end(),
                                 [&](const Track& t) { return t.id == ev.trackId; });
    if (trackIt == tracks.end()) continue;

    ArticulationResult articulation = mapArticulation(trackIt->role, ev.vel);
    double tick = std::max(0.0, std::round(ev.beatAbs * ticksPerPulse));
    uint8_t gm = static_cast<uint8_t>(trackIt->gm);
    uint8_t vel = static_cast<uint8_t>(ev.vel);

    enriched.tracks[trackIt->id].push_back(EnrichedNoteEvent{
      tick,
      {0x99, gm, vel},
      {0x89, gm, 0},
      articulation,
      ev.vel
    });
  }

  for (const auto& t : tracks) {
    enriched.cc[t.id] = generateCCAutomation(t.role, totalTicks, tpq);
  }

  return enriched;
}

} // namespace dgg
