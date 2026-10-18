// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Reiner Prokein
#include "DrumGroover.h"
#include "IPlug_include_in_plug_src.h"

#include "dsp/RhythmEngine.h"
#include "dsp/808VariantPoolBaker.h"
#include "dsp/808FactoryDefaults.h"

#include <fstream>
#include <filesystem>
#include <chrono>
#include <ctime>
#include <algorithm>
#include <iterator>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <atomic>
#include <random>
#include <thread>
#include <set>
#include <unordered_set>
#include <unordered_map>

#ifdef OS_WIN
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include "../iPlug2/IGraphics/Platforms/IGraphicsWin_dnd.h"

// Web UI compiled into the DLL (generated bundle, see cmake/DggEmbedWeb.cmake) and the WebView hook that serves it.
bool DggEmbeddedWebResource(const char* path, const unsigned char** data, size_t* size, const char** mime);
extern bool (*gIPlugEmbeddedWebResource)(const char* path, const unsigned char** data, size_t* size, const char** mime);
// Turns DevTools and the right-click menu of a running WebView on or off (source/platform/DggWebView_win.cpp).
void DggSetWebViewDevTools(iplug::IWebView* owner, bool enable);
#endif

#ifdef OS_MAC
#include "PlatformFileDialog.h"
#endif

namespace {
// Debug log to <temp>/DrumGroover_debug.log, compiled in only with -DDGG_DEBUG_LOG=1 (off in release builds, where DGG_Log() and its
// arguments vanish). Only messages starting with one of the prefixes below are written.
#if defined(DGG_DEBUG_LOG) && DGG_DEBUG_LOG
void DggLogWrite(const std::string& msg)
{
  if (msg.rfind("BakeTiming", 0) != 0 && msg.rfind("EvictCacheIfOverBudget", 0) != 0
    && msg.rfind("HandleClearCache", 0) != 0 && msg.rfind("RebakeRequiredVariantPools", 0) != 0
    && msg.rfind("ApplyFactoryDefaults", 0) != 0 && msg.rfind("EnqueueVariantBake", 0) != 0
    && msg.rfind("OfflineRender", 0) != 0 && msg.rfind("SnareBake", 0) != 0
    && msg.rfind("SnareMiss", 0) != 0 && msg.rfind("SnarePool", 0) != 0 && msg.rfind("SnareRef", 0) != 0 && msg.rfind("TomRef", 0) != 0
    && msg.rfind("CacheMiss", 0) != 0 && msg.rfind("Pc2Preview", 0) != 0 && msg.rfind("CommitPc2", 0) != 0)
    return;
  try
  {
    std::filesystem::path logPath = std::filesystem::temp_directory_path() / "DrumGroover_debug.log";
    std::ofstream f(logPath, std::ios::app);
    if (!f) return;

    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm tmBuf;
    localtime_s(&tmBuf, &t);
    char timeBuf[16];
    sprintf_s(timeBuf, "%02d:%02d:%02d.%03lld", tmBuf.tm_hour, tmBuf.tm_min, tmBuf.tm_sec, (long long)ms);

    f << "[" << timeBuf << "] " << msg << "\n";
  }
  catch (...) { /* logging must never crash the plugin */ }
}
#define DGG_Log(msg) DggLogWrite(msg)
#else
#define DGG_Log(msg) ((void)0)
#endif
}

namespace {
// Timestamp string for exported filenames.
std::string TimestampSlug()
{
  auto now = std::chrono::system_clock::now();
  std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tmBuf;
  localtime_s(&tmBuf, &t);
  char buf[32];
  sprintf_s(buf, "%04d-%02d-%02d_%02d%02d%02d",
    tmBuf.tm_year + 1900, tmBuf.tm_mon + 1, tmBuf.tm_mday,
    tmBuf.tm_hour, tmBuf.tm_min, tmBuf.tm_sec);
  return buf;
}

// Slugify a string for safe filenames.
std::string Slugify(const std::string& raw)
{
  std::string in = raw.empty() ? "block" : raw;
  std::string out;
  bool lastWasDash = false;
  for (char c : in)
  {
    char lc = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if ((lc >= 'a' && lc <= 'z') || (lc >= '0' && lc <= '9'))
    {
      out += lc;
      lastWasDash = false;
    }
    else if (!lastWasDash && !out.empty())
    {
      out += '-';
      lastWasDash = true;
    }
  }
  while (!out.empty() && out.back() == '-') out.pop_back();
  return out.empty() ? "block" : out;
}
}

namespace {
// One flat cache folder, %APPDATA%\DrumGroover\Cache\, holds every baked variant set as "<baseName>_v<k>.f32"
// (k = 1..N). The base name spells out the instrument and every quantized setting including the sample rate
// (VariantPoolBaseName() of the voices), so it is the key: one set per distinct setting.
// "default.index" in the same folder lists the factory-default sets; those are never evicted and never removed by
// "Clear Cache". Everything else is capped at the user's cache limit (Cache panel, stored machine-wide in
// %APPDATA%\DrumGroover\settings.json). The cache is machine-local and independent of the project.
constexpr int kDefaultCacheLimitGiB = 5;
constexpr int kMinCacheLimitGiB = 1;
constexpr int kMaxCacheLimitGiB = 1000;

// Variant k of a set renders after k-1 steps of this much free-running
// metal-bank warmup, so the variants of a set start from different
// oscillator phases. Every bake job adds one random extra warmup up to
// kBakeSaltMaxSec, so no two bakes of the same setting are identical and any
// single missing variant can be baked on its own.
constexpr double kBakeWarmupStepSec = 0.2173;
constexpr double kBakeSaltMaxSec = 5.0;

// GetCacheDir() is reachable from the audio thread during offline rendering
// (ProcessBlock -> WaitForCacheBeforeOfflineRender -> Enqueue*VariantBake ->
// ... -> VariantFilePath() -> GetCacheDir()), which has no try/catch anywhere
// in its call chain (unlike OnMessage()). std::filesystem::temp_directory_path()
// without an error_code argument throws on failure (e.g. TMP/TEMP pointing at
// a missing directory), so the non-throwing overload is used with a
// relative-path fallback: no exception can propagate out of ProcessBlock.
std::filesystem::path GetCacheDir()
{
  std::filesystem::path base;
  const char* appData = std::getenv("APPDATA");
  if (appData && *appData) base = std::filesystem::path(appData);
  else
  {
    std::error_code ec;
    base = std::filesystem::temp_directory_path(ec);
    if (ec) base = std::filesystem::path(".");
  }
  std::filesystem::path dir = base / "DrumGroover" / "Cache";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  return dir;
}

std::filesystem::path GetSettingsPath()
{
  std::filesystem::path base;
  const char* appData = std::getenv("APPDATA");
  if (appData && *appData) base = std::filesystem::path(appData);
  else
  {
    std::error_code ec;
    base = std::filesystem::temp_directory_path(ec);
    if (ec) base = std::filesystem::path(".");
  }
  std::filesystem::path dir = base / "DrumGroover";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  return dir / "settings.json";
}

// Mixer presets: one JSON file per preset in %APPDATA%\DrumGroover\MixerPresets\<engine>\ ({"name": ..., "mixer": <mixer state>}).
std::string MixerPresetEngineName(int engineMode) { return engineMode == static_cast<int>(EngineMode::LoFi) ? "lofi" : "808"; }

std::filesystem::path GetMixerPresetDir(const std::string& engine)
{
  const std::filesystem::path dir = GetSettingsPath().parent_path() / "MixerPresets" / engine;
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  return dir;
}

// A preset name as typed by the user -> the name that is stored (letters, digits, space and - _ ( ) . only, trimmed, at most 60 characters).
std::string SanitizeMixerPresetName(const std::string& in)
{
  std::string out;
  for (unsigned char c : in)
    if (std::isalnum(c) || c == ' ' || c == '-' || c == '_' || c == '(' || c == ')' || c == '.') out += static_cast<char>(c);
  while (!out.empty() && out.front() == ' ') out.erase(out.begin());
  while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
  if (out.size() > 60) out.resize(60);
  return out;
}

std::filesystem::path MixerPresetFile(const std::string& engine, const std::string& name)
{
  return GetMixerPresetDir(engine) / (name + ".json");
}

int ReadCacheLimitGiB()
{
  std::ifstream f(GetSettingsPath());
  if (!f) return kDefaultCacheLimitGiB;
  const nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
  if (!j.is_object() || !j.contains("cacheLimitGiB") || !j["cacheLimitGiB"].is_number_integer())
    return kDefaultCacheLimitGiB;
  return std::clamp(j["cacheLimitGiB"].get<int>(), kMinCacheLimitGiB, kMaxCacheLimitGiB);
}

void WriteSettingsField(const char* name, nlohmann::json value)
{
  nlohmann::json j = nlohmann::json::object();
  {
    std::ifstream f(GetSettingsPath());
    if (f)
    {
      nlohmann::json existing = nlohmann::json::parse(f, nullptr, false);
      if (existing.is_object()) j = std::move(existing);
    }
  }
  j[name] = std::move(value);
  std::ofstream out(GetSettingsPath(), std::ios::trunc);
  out << j.dump(2);
}

void WriteCacheLimitGiB(int gib)
{
  WriteSettingsField("cacheLimitGiB", std::clamp(gib, kMinCacheLimitGiB, kMaxCacheLimitGiB));
}

// When set, releasing a Ride/HiHat/Cymbal knob bakes a completely new set for
// the resulting setting even if one already exists (default: off).
bool ReadRebakeOnChange()
{
  std::ifstream f(GetSettingsPath());
  if (!f) return false;
  const nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
  return j.is_object() && j.contains("rebakeOnValueChange") && j["rebakeOnValueChange"].is_boolean()
    && j["rebakeOnValueChange"].get<bool>();
}

void WriteRebakeOnChange(bool on) { WriteSettingsField("rebakeOnValueChange", on); }

// Per-instrument cache switches (Cache panel): an instrument with its cache off neither plays nor bakes cached sounds, every hit is synthesised live.
// Ride and Cymbal share one engine and one pool, so they share one switch.
std::atomic<bool> gCacheOffHiHat{false}, gCacheOffRide{false};
bool ReadBoolSetting(const char* name)
{
  std::ifstream f(GetSettingsPath());
  if (!f) return false;
  const nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
  return j.is_object() && j.contains(name) && j[name].is_boolean() && j[name].get<bool>();
}

uintmax_t CacheMaxBytes() { return (uintmax_t)ReadCacheLimitGiB() * 1024ULL * 1024ULL * 1024ULL; }

std::filesystem::path VariantFilePath(const std::string& baseName, int k)
{
  return GetCacheDir() / (baseName + "_v" + std::to_string(k) + ".f32");
}

// default.index: one base name per line -- the factory-default sets.
std::unordered_set<std::string> ReadDefaultIndex()
{
  std::unordered_set<std::string> names;
  std::ifstream f(GetCacheDir() / "default.index");
  std::string line;
  while (std::getline(f, line))
  {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (!line.empty()) names.insert(line);
  }
  return names;
}

// Rewrites default.index only when its content actually changes.
void WriteDefaultIndex(const std::set<std::string>& names)
{
  const auto existing = ReadDefaultIndex();
  if (existing.size() == names.size() && std::all_of(names.begin(), names.end(), [&](const std::string& n) { return existing.count(n) > 0; }))
    return;
  const auto path = GetCacheDir() / "default.index";
  const auto tmp = path.string() + ".tmp";
  {
    std::ofstream f(tmp, std::ios::trunc);
    for (const auto& n : names) f << n << "\n";
    if (!f) return;
  }
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
  if (ec) std::filesystem::remove(tmp, ec);
}

// Variant file format: raw float32 samples, no header (sample count = file size / 4).
// Written to a ".tmp" sibling first and renamed onto `path` only after a successful write, so the final name
// never holds a truncated file; the existence of the final file is the validity proof.
bool SaveVariantToDisk(const std::filesystem::path& path, const std::vector<float>& buf)
{
  const auto tmp = path.string() + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(buf.data()), (std::streamsize)(buf.size() * sizeof(float)));
    if (!f) { std::error_code ec; std::filesystem::remove(tmp, ec); return false; }
  }  // f closed/flushed here, before the rename.
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
  if (ec) { std::filesystem::remove(tmp, ec); return false; }
  return true;
}

bool LoadVariantFromDisk(const std::filesystem::path& path, std::vector<float>& outBuf)
{
  std::error_code ec;
  const auto sizeBytes = std::filesystem::file_size(path, ec);
  if (ec || sizeBytes == 0 || (sizeBytes % sizeof(float)) != 0) return false;
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  outBuf.resize(sizeBytes / sizeof(float));
  f.read(reinterpret_cast<char*>(outBuf.data()), (std::streamsize)sizeBytes);
  return (bool)f;
}
}

namespace {
// Complete track roster for the drum kit: every kit instance of the instrument table (custom instruments get their tracks from the UI).
std::vector<dgg::Track> DemoTracks() {
  std::vector<dgg::Track> tracks;
  for (const dgg::InstrumentDef& d : dgg::kInstruments)
    if (d.gmNote >= 0) tracks.push_back({d.id, d.id, d.gmNote, d.trackName});
  return tracks;
}

// JSON conversion helpers.
dgg::Grid GridFromJson(const nlohmann::json& j)
{
  dgg::Grid g;
  g.numerator   = j.value("numerator", 4);
  g.denominator = j.value("denominator", 4);
  g.snap        = j.value("snap", 0.25);
  return g;
}

dgg::ExportParams ExportParamsFromJson(const nlohmann::json& j)
{
  dgg::ExportParams p;
  p.bpm     = j.value("bpm", 120.0);
  p.kitName = j.value("kitName", "DGG-Generated");
  p.genre   = j.value("genre", "Custom");
  return p;
}

dgg::Feel FeelFromJsonString(const std::string& s)
{
  if (s == "swing")    return dgg::Feel::Swing;
  if (s == "halftime") return dgg::Feel::Halftime;
  return dgg::Feel::Straight;
}

std::string FeelToJsonString(dgg::Feel feel)
{
  switch (feel)
  {
    case dgg::Feel::Swing:    return "swing";
    case dgg::Feel::Halftime: return "halftime";
    default:                  return "straight";
  }
}

dgg::Block BlockFromJson(const nlohmann::json& j)
{
  dgg::Block b;
  b.id        = j.value("id", "");
  b.name      = j.value("name", "");
  b.length    = j.value("length", 1.0);
  b.numerator = j.value("numerator", 4);
  b.feel      = FeelFromJsonString(j.value("feel", "straight"));

  if (j.contains("data") && j["data"].is_object())
  {
    for (auto& [trackId, notes] : j["data"].items())
    {
      std::vector<dgg::Note> trackNotes;
      for (auto& n : notes)
        trackNotes.push_back({ n.value("beat", 0.0), n.value("vel", 100), n.value("turnaround", false) });
      b.data[trackId] = std::move(trackNotes);
    }
  }

  if (j.contains("seeds") && j["seeds"].is_object())
  {
    for (auto& [groupId, val] : j["seeds"].items())
      b.seeds[groupId] = val.get<uint32_t>();
  }

  if (j.contains("enabledArticulations") && j["enabledArticulations"].is_object())
  {
    for (auto& [subGroup, ids] : j["enabledArticulations"].items())
    {
      std::vector<std::string> idVec;
      for (auto& id : ids) idVec.push_back(id.get<std::string>());
      b.enabledArticulations[subGroup] = std::move(idVec);
    }
  }

  if (j.contains("tripletLayers") && j["tripletLayers"].is_object())
  {
    for (auto& [groupId, ranges] : j["tripletLayers"].items())
    {
      std::vector<dgg::BeatRange> rangeVec;
      for (auto& r : ranges)
        rangeVec.push_back({ r.value("from", 0.0), r.value("to", 0.0) });
      b.tripletLayers[groupId] = std::move(rangeVec);
    }
  }

  if (j.contains("phraseLength") && j["phraseLength"].is_object())
  {
    for (auto& [groupId, val] : j["phraseLength"].items())
      b.phraseLength[groupId] = val.get<int>();
  }

  b.turnaroundInterval = j.value("turnaroundInterval", b.turnaroundInterval);
  b.velocityBuild      = j.value("velocityBuild", b.velocityBuild);

  if (j.contains("turnaroundInstruments") && j["turnaroundInstruments"].is_array())
  {
    std::vector<std::string> instruments;
    for (auto& v : j["turnaroundInstruments"])
      instruments.push_back(v.get<std::string>());
    b.turnaroundInstruments = std::move(instruments);
  }

  b.turnaroundDensity           = j.value("turnaroundDensity", b.turnaroundDensity);
  b.turnaroundSubdivision       = j.value("turnaroundSubdivision", b.turnaroundSubdivision);
  b.turnaroundLengthBeats       = j.value("turnaroundLengthBeats", b.turnaroundLengthBeats);
  b.turnaroundVaryPerOccurrence = j.value("turnaroundVaryPerOccurrence", b.turnaroundVaryPerOccurrence);
  b.turnaroundFillVel           = j.value("turnaroundFillVel", b.turnaroundFillVel);
  b.turnaroundCrash             = j.value("turnaroundCrash", true);
  b.turnaroundHiHat             = j.value("turnaroundHiHat", true);

  b.turnaroundKickDensity     = j.value("turnaroundKickDensity", b.turnaroundKickDensity);
  b.turnaroundKickSubdivision = j.value("turnaroundKickSubdivision", b.turnaroundKickSubdivision);
  b.turnaroundKickSeed        = j.value("turnaroundKickSeed", b.turnaroundKickSeed);

  if (j.contains("turnaroundSeed") && !j["turnaroundSeed"].is_null())
  {
    b.turnaroundSeedSet = true;
    b.turnaroundSeed    = j["turnaroundSeed"].get<uint32_t>();
  }

  b.breakParentId = j.value("breakParentId", b.breakParentId);
  b.breakBeats    = j.value("breakBeats", b.breakBeats);

  if (j.contains("breakInstruments") && j["breakInstruments"].is_array())
  {
    std::vector<std::string> instruments;
    for (auto& v : j["breakInstruments"])
      instruments.push_back(v.get<std::string>());
    b.breakInstruments = std::move(instruments);
  }

  b.breakDensity          = j.value("breakDensity", b.breakDensity);
  b.breakFillVel          = j.value("breakFillVel", b.breakFillVel);
  b.breakSubdivision      = j.value("breakSubdivision", b.breakSubdivision);
  b.breakKickDensity      = j.value("breakKickDensity", b.breakKickDensity);
  b.breakKickSubdivision  = j.value("breakKickSubdivision", b.breakKickSubdivision);
  b.breakKickSeed         = j.value("breakKickSeed", b.breakKickSeed);

  if (j.contains("breakSeed") && !j["breakSeed"].is_null())
  {
    b.breakSeedSet = true;
    b.breakSeed    = j["breakSeed"].get<uint32_t>();
  }

  b.isFillBlock = j.value("isFillBlock", b.isFillBlock);
  b.fillBeats   = j.value("fillBeats", b.fillBeats);
  b.fillAnchor  = j.value("fillAnchor", b.fillAnchor);

  if (j.contains("fillInstruments") && j["fillInstruments"].is_array())
  {
    std::vector<std::string> instruments;
    for (auto& v : j["fillInstruments"])
      instruments.push_back(v.get<std::string>());
    b.fillInstruments = std::move(instruments);
  }

  b.fillDensity         = j.value("fillDensity", b.fillDensity);
  b.fillVel             = j.value("fillVel", b.fillVel);
  b.fillSubdivision     = j.value("fillSubdivision", b.fillSubdivision);
  b.fillKickDensity     = j.value("fillKickDensity", b.fillKickDensity);
  b.fillKickSubdivision = j.value("fillKickSubdivision", b.fillKickSubdivision);
  b.fillKickSeed        = j.value("fillKickSeed", b.fillKickSeed);

  if (j.contains("fillSeed") && !j["fillSeed"].is_null())
  {
    b.fillSeedSet = true;
    b.fillSeed    = j["fillSeed"].get<uint32_t>();
  }

  return b;
}

dgg::FullPattern FullPatternFromJson(const nlohmann::json& j)
{
  dgg::FullPattern fp;
  fp.totalBeats = j.value("totalBeats", 0.0);
  if (j.contains("events"))
  {
    for (auto& e : j["events"])
      fp.events.push_back({ e.value("trackId", ""), e.value("beatAbs", 0.0), e.value("vel", 100) });
  }
  return fp;
}

nlohmann::json NoteVectorToJson(const std::vector<dgg::Note>& notes)
{
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& n : notes)
    arr.push_back({ {"beat", n.beat}, {"vel", n.vel}, {"turnaround", n.turnaround} });
  return arr;
}

nlohmann::json TracksToJson(const dgg::Block& block, const std::vector<std::string>& trackIds)
{
  nlohmann::json out = nlohmann::json::object();
  for (const auto& id : trackIds)
  {
    auto it = block.data.find(id);
    out[id] = it != block.data.end() ? NoteVectorToJson(it->second) : nlohmann::json::array();
  }
  return out;
}

nlohmann::json BlockDataToJson(const dgg::Block& block)
{
  nlohmann::json out = nlohmann::json::object();
  for (const auto& [trackId, notes] : block.data)
    out[trackId] = NoteVectorToJson(notes);
  return out;
}

// Family index for the 6 sound engine groups.
int FocusFamilyIndex(const std::string& family)
{
  if (family == "kick")   return 0;
  if (family == "snare")  return 1;
  if (family == "hihat")  return 2;
  if (family == "tom")    return 3;
  if (family == "cymbal") return 4;
  if (family == "ride")   return 5;
  return -1;
}

// GM note to family index for Lo‑Fi routing.
int NoteToFamilyIndex(int note)
{
  if (const dgg::InstrumentDef* d = dgg::FindInstrumentByNote(note)) return FocusFamilyIndex(d->family);
  return -1;
}

// GM note to Lo‑Fi role.
dgg::LoFiRole NoteToLoFiRole(int note)
{
  switch (note)
  {
    case 36:  return dgg::LoFiRole::Kick;
    case 34:  return dgg::LoFiRole::KickOpen;
    case 38:  return dgg::LoFiRole::Snare;
    case 37:  return dgg::LoFiRole::Rim;
    case 22:  return dgg::LoFiRole::HatClosed;
    case 64:  return dgg::LoFiRole::HatOpen;
    case 41:  return dgg::LoFiRole::TomLow;       // Floor Tom 2 Center
    case 43:  return dgg::LoFiRole::TomLowMid;    // Floor Tom 1 Center (new)
    case 45:  return dgg::LoFiRole::TomMid;       // Rack Tom 3 Center
    case 47:  return dgg::LoFiRole::TomHighMid;   // Rack Tom 2 Center (new)
    case 48:  return dgg::LoFiRole::TomHi;        // Rack Tom 1 Center

    // Rimshots (click + body ring) and rim-only (dry click), one per tom position.
    case 82:  return dgg::LoFiRole::TomRimshotHi;        // Rack1 Rimshot
    case 81:  return dgg::LoFiRole::TomRimOnlyHi;        // Rack1 Rim Only
    case 80:  return dgg::LoFiRole::TomRimshotHighMid;   // Rack2 Rimshot
    case 79:  return dgg::LoFiRole::TomRimOnlyHighMid;   // Rack2 Rim Only
    case 78:  return dgg::LoFiRole::TomRimshotMid;       // Rack3 Rimshot
    case 77:  return dgg::LoFiRole::TomRimOnlyMid;       // Rack3 Rim Only
    case 75:  return dgg::LoFiRole::TomRimshotLowMid;    // Floor1 Rimshot
    case 74:  return dgg::LoFiRole::TomRimOnlyLowMid;    // Floor1 Rim Only
    case 73:  return dgg::LoFiRole::TomRimshotLow;       // Floor2 Rimshot
    case 72:  return dgg::LoFiRole::TomRimOnlyLow;       // Floor2 Rim Only

    case 49:  return dgg::LoFiRole::Crash1;
    case 57:  return dgg::LoFiRole::Crash2;
    case 51:  return dgg::LoFiRole::Ride;
    case 53:  return dgg::LoFiRole::RideBell;
    default:  return dgg::LoFiRole::Generic;
  }
}

// GM note -> mixer channel index (dgg::MixerChannel). Mirrors the voice pool
// routing in TriggerGmNote() one-to-one; single source of truth for the
// channel layout. Unmapped notes (custom tracks) land on the Custom channel.
int NoteToChannel(int note)
{
  if (const dgg::InstrumentDef* d = dgg::FindInstrumentByNote(note)) return static_cast<int>(d->channel);
  return static_cast<int>(dgg::MixerChannel::Custom);
}

// True from the moment the factory-default sets are (re)created (no default.index yet) until all baking is done.
std::atomic<bool> gFirstRunBake{false};

// Voice parameter setters.
void SetKickVoiceParam(dgg::TR808KickVoice& v, const std::string& p, double val)
{
  if (p == "decayKnob01")      v.decayKnob01 = val;
  else if (p == "toneKnob")    v.toneKnob = val;
  else if (p == "levelKnob")   v.levelKnob = val;
  else if (p == "pitchRatio")  v.pitchRatio = val;  // digital BD transpose, shared convention with Ride/HiHat/Cymbal
}

nlohmann::json KickVoiceParamsToJson(const dgg::TR808KickVoice& v)
{
  return { {"decayKnob01", v.decayKnob01}, {"toneKnob", v.toneKnob}, {"levelKnob", v.levelKnob}, {"pitchRatio", v.pitchRatio} };
}

void SetSnareVoiceParam(dgg::TR808SnareVoice& v, const std::string& p, double val)
{
  if (p == "toneKnob")         v.toneKnob = val;
  else if (p == "snappyKnob")  v.snappyKnob = val;
  else if (p == "levelKnob")   v.levelKnob = val;
  else if (p == "pitchRatio")  v.pitchRatio = val;  // digital SD transpose, shared convention with Kick/Ride/HiHat/Cymbal
}

nlohmann::json SnareVoiceParamsToJson(const dgg::TR808SnareVoice& v)
{
  return { {"toneKnob", v.toneKnob}, {"snappyKnob", v.snappyKnob}, {"levelKnob", v.levelKnob}, {"pitchRatio", v.pitchRatio} };
}

// HiHat parameters: the circuit model determines the tone itself (the real hardware has no tone controls for
// OH/CH), so only the front panel's three pots (VR7/VR8 LEVEL, VR3 OH DECAY) plus the digital Pitch that Ride and
// Cymbal also have are exposed below.
void SetHiHatVoiceParam(dgg::HiHat808Voice& v, const std::string& p, double val)
{
  if (p == "level")               v.levelKnob01 = val;   // VR7/VR8 LEVEL
  else if (p == "decayKnob01")    v.decayKnob01 = val;    // VR3 OH DECAY, inert for Closed (real hardware has none there either)
  else if (p == "pitchRatio")     v.pitchRatio = val;     // digital metal-bank transpose, shared convention with Ride/Cymbal
  else if (p == "pedalFreqHz")    v.pedalFreqHz = val;
  else if (p == "pedalQ")         v.pedalQ = val;
  else if (p == "pedalDecaySec")  v.pedalDecaySec = val;
  else if (p == "pedalLevel")     v.pedalLevel = val;
}

nlohmann::json HiHatVoiceParamsToJson(const dgg::HiHat808Voice& v)
{
  return {
    {"level", v.levelKnob01}, {"decayKnob01", v.decayKnob01}, {"pitchRatio", v.pitchRatio},
    {"pedalFreqHz", v.pedalFreqHz}, {"pedalQ", v.pedalQ},
    {"pedalDecaySec", v.pedalDecaySec}, {"pedalLevel", v.pedalLevel},
  };
}

void SetTomVoiceParam(dgg::TR808TomVoice& v, const std::string& p, double val)
{
  if (p == "tuningKnob")          v.tuningKnob = val;
  else if (p == "levelKnob")      v.levelKnob = val;
  else if (p == "pitchRatio")     v.pitchRatio = val;
}

nlohmann::json RmVoiceParamsToJson(const dgg::TR808RmVoice& v)
{
  return { {"levelKnob", v.levelKnob}, {"pitchRatio", v.pitchRatio} };
}
void SetRmVoiceParam(dgg::TR808RmVoice& v, const std::string& p, double val)
{
  if (p == "levelKnob")           v.levelKnob = val;
  else if (p == "pitchRatio")     v.pitchRatio = val;
}
nlohmann::json HcVoiceParamsToJson(const dgg::TR808HcVoice& v)
{
  return { {"levelKnob", v.levelKnob}, {"pitchRatio", v.pitchRatio} };
}
void SetHcVoiceParam(dgg::TR808HcVoice& v, const std::string& p, double val)
{
  if (p == "levelKnob")           v.levelKnob = val;
  else if (p == "pitchRatio")     v.pitchRatio = val;
}
bool IsHcKind(dgg::EngineKind k) { return k == dgg::EngineKind::HcClap || k == dgg::EngineKind::HcMaracas; }
nlohmann::json CbVoiceParamsToJson(const dgg::TR808CbVoice& v)
{
  return { {"levelKnob", v.levelKnob}, {"pitchRatio", v.pitchRatio} };
}
void SetCbVoiceParam(dgg::TR808CbVoice& v, const std::string& p, double val)
{
  if (p == "levelKnob")           v.levelKnob = val;
  else if (p == "pitchRatio")     v.pitchRatio = val;
}
bool IsCbKind(dgg::EngineKind k) { return k == dgg::EngineKind::Cowbell; }
nlohmann::json Pc2VoiceParamsToJson(const dgg::TR808Pc2Voice& v)
{
  return { {"levelKnob", v.levelKnob}, {"lfoRateKnob", v.lfoRateKnob}, {"lfoDepthKnob", v.lfoDepthKnob}, {"pitchKnob", v.pitchKnob},
           {"decayKnob", v.decayKnob}, {"sweepKnob", v.sweepKnob}, {"lfoWaveKnob", v.lfoWaveKnob}, {"vcoWaveKnob", v.vcoWaveKnob} };
}
void SetPc2VoiceParam(dgg::TR808Pc2Voice& v, const std::string& p, double val)
{
  if (p == "levelKnob")           v.levelKnob = val;
  else if (p == "lfoRateKnob")    v.lfoRateKnob = val;
  else if (p == "lfoDepthKnob")   v.lfoDepthKnob = val;
  else if (p == "pitchKnob")      v.pitchKnob = val;
  else if (p == "decayKnob")      v.decayKnob = val;
  else if (p == "sweepKnob")      v.sweepKnob = val;
  else if (p == "lfoWaveKnob")    v.lfoWaveKnob = val;
  else if (p == "vcoWaveKnob")    v.vcoWaveKnob = val;
}
bool IsPc2Kind(dgg::EngineKind k) { return k == dgg::EngineKind::Pc2; }
nlohmann::json TomVoiceParamsToJson(const dgg::TR808TomVoice& v)
{
  return { {"tuningKnob", v.tuningKnob}, {"levelKnob", v.levelKnob}, {"pitchRatio", v.pitchRatio} };
}

// The same operations by voice type, for code that walks the instrument table over pools of different voice kinds.
void SetParamOn(dgg::TR808SnareVoice& v, const std::string& p, double val) { SetSnareVoiceParam(v, p, val); }
void SetParamOn(dgg::TR808RmVoice& v, const std::string& p, double val) { SetRmVoiceParam(v, p, val); }
void SetParamOn(dgg::TR808HcVoice& v, const std::string& p, double val) { SetHcVoiceParam(v, p, val); }
void SetParamOn(dgg::TR808CbVoice& v, const std::string& p, double val) { SetCbVoiceParam(v, p, val); }
void SetParamOn(dgg::TR808Pc2Voice& v, const std::string& p, double val) { SetPc2VoiceParam(v, p, val); }
nlohmann::json ParamsJsonOf(const dgg::TR808SnareVoice& v) { return SnareVoiceParamsToJson(v); }
nlohmann::json ParamsJsonOf(const dgg::TR808RmVoice& v) { return RmVoiceParamsToJson(v); }
// The four knobs of the factory-default set of a Ride/HiHat/Cymbal row (kind 0 = ride, 1 = hihat, 2 = cymbal, as dgg::FactoryDefaultEntry).
dgg::FactoryDefaultEntry FactoryEntryOf(const dgg::InstrumentDef& d)
{
  const int kind = d.kind == dgg::EngineKind::HiHat ? 1 : d.kind == dgg::EngineKind::Cymbal ? 2 : 0;
  return { kind, d.paramIndex, dgg::DefaultOf(d, "decayKnob01"), dgg::DefaultOf(d, "toneKnob"),
           dgg::DefaultOf(d, kind == 1 ? "level" : "levelKnob"), dgg::DefaultOf(d, "pitchRatio") };
}
void SetParamOn(dgg::HiHat808Voice& v, const std::string& p, double val) { SetHiHatVoiceParam(v, p, val); }
nlohmann::json ParamsJsonOf(const dgg::HiHat808Voice& v) { return HiHatVoiceParamsToJson(v); }
void SetParamOn(dgg::TR808KickVoice& v, const std::string& p, double val) { SetKickVoiceParam(v, p, val); }
nlohmann::json ParamsJsonOf(const dgg::TR808KickVoice& v) { return KickVoiceParamsToJson(v); }
void SetParamOn(dgg::TR808TomVoice& v, const std::string& p, double val) { SetTomVoiceParam(v, p, val); }
nlohmann::json ParamsJsonOf(const dgg::TR808TomVoice& v) { return TomVoiceParamsToJson(v); }
dgg::TR808TomVoice::Articulation TomArticulationOf(const char* name)
{
  if (dgg::StrEq(name, "RimShot")) return dgg::TR808TomVoice::Articulation::RimShot;
  if (dgg::StrEq(name, "RimOnly")) return dgg::TR808TomVoice::Articulation::RimOnly;
  return dgg::TR808TomVoice::Articulation::Center;
}

// Cymbal plays through TR808RideVoice (see dsp/808RideVoice.h
// Articulation::Crash) -- callers use SetRideVoiceParam/RideVoiceParamsToJson.

void SetRideVoiceParam(dgg::TR808RideVoice& v, const std::string& p, double val)
{
  if (p == "toneKnob")            v.toneKnob = val;
  else if (p == "levelKnob")      v.levelKnob = val;
  else if (p == "decayKnob01")    v.decayKnob01 = val;
  else if (p == "pitchRatio")     v.pitchRatio = val;
  else if (p == "vca1BiasOhm")    v.vca1BiasOhm = val;
  else if (p == "vca2BiasOhm")    v.vca2BiasOhm = val;
  else if (p == "vca3BiasOhm")    v.vca3BiasOhm = val;
  else if (p == "level")          v.level = val;
}

nlohmann::json RideVoiceParamsToJson(const dgg::TR808RideVoice& v)
{
  return {
    {"toneKnob", v.toneKnob}, {"levelKnob", v.levelKnob}, {"decayKnob01", v.decayKnob01},
    {"pitchRatio", v.pitchRatio},
    {"vca1BiasOhm", v.vca1BiasOhm}, {"vca2BiasOhm", v.vca2BiasOhm}, {"vca3BiasOhm", v.vca3BiasOhm},
    {"level", v.level},
  };
}
}

#ifdef OS_WIN
namespace {
// Win32 file dialog helpers.
std::wstring Utf8ToWide(const std::string& s)
{
  if (s.empty()) return std::wstring();
  int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
  std::wstring result(len, 0);
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &result[0], len);
  return result;
}

std::string WideToUtf8(const std::wstring& s)
{
  if (s.empty()) return std::string();
  int len = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
  std::string result(len, 0);
  WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &result[0], len, nullptr, nullptr);
  return result;
}

std::string Win32SaveFileDialog(HWND owner, const wchar_t* filter, const wchar_t* defExt, const std::string& suggestedFilename, const wchar_t* title = nullptr)
{
  wchar_t fileBuf[MAX_PATH] = {};
  std::wstring suggestedWide = Utf8ToWide(suggestedFilename);
  wcsncpy_s(fileBuf, suggestedWide.c_str(), _TRUNCATE);

  OPENFILENAMEW ofn = { sizeof(OPENFILENAMEW) };
  ofn.hwndOwner = owner;
  ofn.lpstrFilter = filter;
  ofn.lpstrFile = fileBuf;
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrDefExt = defExt;
  ofn.lpstrTitle = title;
  ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

  if (GetSaveFileNameW(&ofn))
    return WideToUtf8(fileBuf);
  return "";
}

std::string Win32OpenFileDialog(HWND owner, const wchar_t* filter, const wchar_t* defExt)
{
  wchar_t fileBuf[MAX_PATH] = {};

  OPENFILENAMEW ofn = { sizeof(OPENFILENAMEW) };
  ofn.hwndOwner = owner;
  ofn.lpstrFilter = filter;
  ofn.lpstrFile = fileBuf;
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrDefExt = defExt;
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

  if (GetOpenFileNameW(&ofn))
    return WideToUtf8(fileBuf);
  return "";
}
}
#endif

#if IPLUG_DSP
template <typename F>
void DrumGroover::ForEachSnarePool(F&& f)
{
  for (const dgg::InstrumentDef& d : dgg::kInstruments)
  {
    if (!dgg::StrEq(d.family, "snare")) continue;
    if (d.kind == dgg::EngineKind::SnareSd) f(d, mSnareSdPools[d.poolSlot]);
    else f(d, mSnareRmPools[d.poolSlot]);
  }
}

template <typename F>
void DrumGroover::ForEachSnarePool(F&& f) const
{
  for (const dgg::InstrumentDef& d : dgg::kInstruments)
  {
    if (!dgg::StrEq(d.family, "snare")) continue;
    if (d.kind == dgg::EngineKind::SnareSd) f(d, mSnareSdPools[d.poolSlot]);
    else f(d, mSnareRmPools[d.poolSlot]);
  }
}
template <typename F>
void DrumGroover::ForEachRidePool(F&& f)
{
  for (const dgg::InstrumentDef& d : dgg::kInstruments)
    if (d.kind == dgg::EngineKind::Ride) f(d, mRidePools[d.poolSlot]);
}

template <typename F>
void DrumGroover::ForEachRidePool(F&& f) const
{
  for (const dgg::InstrumentDef& d : dgg::kInstruments)
    if (d.kind == dgg::EngineKind::Ride) f(d, mRidePools[d.poolSlot]);
}

template <typename F>
void DrumGroover::ForEachCymbalPool(F&& f)
{
  for (const dgg::InstrumentDef& d : dgg::kInstruments)
    if (d.kind == dgg::EngineKind::Cymbal) f(d, mCymbalPools[d.poolSlot]);
}

template <typename F>
void DrumGroover::ForEachCymbalPool(F&& f) const
{
  for (const dgg::InstrumentDef& d : dgg::kInstruments)
    if (d.kind == dgg::EngineKind::Cymbal) f(d, mCymbalPools[d.poolSlot]);
}

template <typename F>
void DrumGroover::ForEachHiHatPool(F&& f)
{
  for (const dgg::InstrumentDef& d : dgg::kInstruments)
    if (d.kind == dgg::EngineKind::HiHat) f(d, mHiHatPools[d.poolSlot]);
}

template <typename F>
void DrumGroover::ForEachHiHatPool(F&& f) const
{
  for (const dgg::InstrumentDef& d : dgg::kInstruments)
    if (d.kind == dgg::EngineKind::HiHat) f(d, mHiHatPools[d.poolSlot]);
}

template <typename F>
void DrumGroover::ForEachKickPool(F&& f)
{
  for (const dgg::InstrumentDef& d : dgg::kInstruments)
    if (d.kind == dgg::EngineKind::Kick) f(d, mKickPools[d.poolSlot]);
}

template <typename F>
void DrumGroover::ForEachKickPool(F&& f) const
{
  for (const dgg::InstrumentDef& d : dgg::kInstruments)
    if (d.kind == dgg::EngineKind::Kick) f(d, mKickPools[d.poolSlot]);
}

template <typename F>
void DrumGroover::ForEachTomPool(F&& f)
{
  for (const dgg::InstrumentDef& d : dgg::kInstruments)
    if (d.kind == dgg::EngineKind::Tom && dgg::StrEq(d.family, "tom")) f(d, mTomPools[d.poolSlot]);
}

template <typename F>
void DrumGroover::ForEachTomPool(F&& f) const
{
  for (const dgg::InstrumentDef& d : dgg::kInstruments)
    if (d.kind == dgg::EngineKind::Tom && dgg::StrEq(d.family, "tom")) f(d, mTomPools[d.poolSlot]);
}
#endif

namespace {
// Number of live plugin instances in this process; the baker is shared by all of them.
std::atomic<int> gLiveInstances{0};
}

DrumGroover::~DrumGroover()
{
  if (gLiveInstances.fetch_sub(1) == 1)
    dgg::TR808VariantPoolBaker::Instance().Shutdown();
}

DrumGroover::DrumGroover(const InstanceInfo& info)
: Plugin(info, MakeConfig(kNumParams, 1))
, mTracks(DemoTracks())
, mMidiExport(mTracks)
{
  gLiveInstances.fetch_add(1);
  DGG_Log("Constructor start");

  // Generate a stable per-instance ID.
  {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dis;
    uint64_t v1 = dis(gen);
    uint64_t v2 = dis(gen);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llx%016llx", static_cast<unsigned long long>(v1), static_cast<unsigned long long>(v2));
    mInstanceId = buf;
  }

  // Fresh boot: instrument channels start with EMPTY FX slots (the user builds
  // their own chain); only the master gets a sensible default (EQ -> Limiter).
  // A project load later overrides this via HandleMixerState().
  ApplyDefaultMixerForEngine(static_cast<int>(EngineMode::Internal));   // the 808 default mixer (kit gain +4 dB)

  GetParam(kKickDensity)->InitPercentage("Kick Density", 50.);
  GetParam(kSnareDensity)->InitPercentage("Snare Density", 50.);
  GetParam(kSnareGhostDensity)->InitPercentage("Snare Ghost", 30.);

#if IPLUG_DSP
  // Set sample rate for all voice pools.
  for (auto& p : mKickPools) p.SetSampleRate(GetSampleRate());
  for (auto& p : mSnareSdPools) p.SetSampleRate(GetSampleRate());
  for (auto& p : mSnareRmPools) p.SetSampleRate(GetSampleRate());
  for (auto& p : mHiHatPools) p.SetSampleRate(GetSampleRate());
  for (auto& p : mCymbalPools)      p.SetSampleRate(GetSampleRate());
  // Start values of the ride voices: the factory-default set of the instrument table.
  ForEachRidePool([](const dgg::InstrumentDef& d, auto& pool) {
    const dgg::FactoryDefaultEntry e = FactoryEntryOf(d);
    for (auto& v : pool.Voices()) dgg::ApplyFactoryDefaultToVoice(v, e);
  });
  // Start values of the cymbal voices: the factory-default set of the instrument table.
  ForEachCymbalPool([](const dgg::InstrumentDef& d, auto& pool) {
    const dgg::FactoryDefaultEntry e = FactoryEntryOf(d);
    for (auto& v : pool.Voices()) dgg::ApplyFactoryDefaultToVoice(v, e);
  });
  for (auto& p : mRidePools)        p.SetSampleRate(GetSampleRate());
  for (auto& pool : mTomPools)  pool.SetSampleRate(GetSampleRate());
  for (auto& pool : mCustomPools) pool.SetSampleRate(GetSampleRate());
  for (auto& pool : mCustomRmPools) pool.SetSampleRate(GetSampleRate());
  for (auto& pool : mCustomHcPools) pool.SetSampleRate(GetSampleRate());
  for (auto& pool : mCustomCbPools) pool.SetSampleRate(GetSampleRate());
  for (auto& pool : mCustomPc2Pools) pool.SetSampleRate(GetSampleRate());

  // Start values of the kick voices: the defaults of the instrument table.
  ForEachKickPool([](const dgg::InstrumentDef& d, auto& pool) {
    for (auto& v : pool.Voices())
      for (int i = 0; i < d.numDefaults; ++i) SetParamOn(v, d.defaults[i].key, d.defaults[i].value);
  });

  // Start values of the snare voices: the defaults of the instrument table.
  ForEachSnarePool([](const dgg::InstrumentDef& d, auto& pool) {
    for (auto& v : pool.Voices())
      for (int i = 0; i < d.numDefaults; ++i) SetParamOn(v, d.defaults[i].key, d.defaults[i].value);
  });

  // Which circuit each tom instance plays (sheet, Tom/Conga switch, articulation) and its start values: the instrument table.
  ForEachTomPool([](const dgg::InstrumentDef& d, auto& pool) {
    for (auto& v : pool.Voices())
    {
      v.sheet = d.sheet; v.conga = d.conga; v.articulation = TomArticulationOf(d.articulation);
      for (int i = 0; i < d.numDefaults; ++i) SetParamOn(v, d.defaults[i].key, d.defaults[i].value);
    }
  });
  {
    for (int k = 0; k < dgg::kNumCustomInstruments; k++)
    {
      mCustomNote[k].store(-1, std::memory_order_relaxed);
      mCustomPreviewGain[k].store(dgg::DefaultOf(dgg::CustomInstrument(k), "volume", 1.0), std::memory_order_relaxed);   // the Volume default of the instrument
      const dgg::InstrumentDef& row = dgg::CustomInstrument(k);
      for (auto& v : mCustomPools[k].Voices()) { v.sheet = row.sheet; v.conga = row.conga; v.articulation = TomArticulationOf(row.articulation ? row.articulation : "Center"); }
      for (auto& v : mCustomRmPools[k].Voices()) v.claves = row.kind == dgg::EngineKind::RmClaves;
      for (auto& v : mCustomHcPools[k].Voices()) v.clap = row.kind == dgg::EngineKind::HcClap;
      // the circuit's start values: the row's defaults, on the pool of its kind
      for (int i = 0; i < row.numDefaults; ++i)
      {
        if (row.kind == dgg::EngineKind::Tom) { for (auto& v : mCustomPools[k].Voices()) SetParamOn(v, row.defaults[i].key, row.defaults[i].value); }
        else if (IsHcKind(row.kind)) { for (auto& v : mCustomHcPools[k].Voices()) SetParamOn(v, row.defaults[i].key, row.defaults[i].value); }
        else if (IsCbKind(row.kind)) { for (auto& v : mCustomCbPools[k].Voices()) SetParamOn(v, row.defaults[i].key, row.defaults[i].value); }
        else if (IsPc2Kind(row.kind)) { for (auto& v : mCustomPc2Pools[k].Voices()) SetParamOn(v, row.defaults[i].key, row.defaults[i].value); }
        else { for (auto& v : mCustomRmPools[k].Voices()) SetParamOn(v, row.defaults[i].key, row.defaults[i].value); }
      }
    }
  }

  // Circuit family, variantPoolArticulationId and start values of the hi‑hat pools: the instrument table. The articulation id
  // is distinct per pool (not just per circuit family) so every pool has its own cache sets.
  ForEachHiHatPool([](const dgg::InstrumentDef& d, auto& pool) {
    for (auto& v : pool.Voices())
    {
      v.circuitFamily = dgg::StrEq(d.circuitFamily, "Open") ? dgg::HiHat808Voice::CircuitFamily::Open : dgg::HiHat808Voice::CircuitFamily::Closed;
      v.variantPoolArticulationId = d.poolArtId;
      for (int i = 0; i < d.numDefaults; ++i) SetParamOn(v, d.defaults[i].key, d.defaults[i].value);
    }
  });

  // Set default polyphony.
  ForEachKickPool([](const dgg::InstrumentDef&, auto& pool) { pool.SetPolyphony(2); });
  ForEachSnarePool([](const dgg::InstrumentDef&, auto& pool) { pool.SetPolyphony(4); });
  ForEachHiHatPool([](const dgg::InstrumentDef&, auto& pool) { pool.SetPolyphony(4); });
  for (auto& pool : mTomPools) pool.SetPolyphony(3);
  for (auto& pool : mCustomPools) pool.SetPolyphony(3);
  for (auto& pool : mCustomRmPools) pool.SetPolyphony(3);
  for (auto& pool : mCustomHcPools) pool.SetPolyphony(3);
  for (auto& pool : mCustomCbPools) pool.SetPolyphony(3);
  for (auto& pool : mCustomPc2Pools) pool.SetPolyphony(3);
  for (auto& p : mRidePools) p.SetPolyphony(3);
  for (auto& p : mCymbalPools)      p.SetPolyphony(2);

  // Ride, HiHat and Cymbal play from the variant pool; a hit without a baked variant is reported (DrainCacheMisses() bakes it)
  // and synthesised live with the fast model, so the models are allocated here and not on the audio thread.
  // With the cache of an instrument switched off (Cache panel) it synthesises every hit live (ApplyCacheMode()).
  ForEachHiHatPool([](const dgg::InstrumentDef&, auto& pool) { for (auto& v : pool.Voices()) v.PrepareLive(); });
  for (auto& p : mRidePools) for (auto& v : p.Voices()) v.PrepareLive();

  // All five cymbal positions share one engine and default tuning (one real cymbal circuit); a per-position pitch
  // difference is the pitchRatio knob. Cymbal shares Ride's variant pool; the articulation (Crash/Mute) is part of
  // the key. Background baking runs through EnqueueCrashVariantBake() (pattern sync or knob release).
  for (auto& p : mCymbalPools) for (auto& v : p.Voices()) v.PrepareLive();
  ApplyCacheMode(ReadBoolSetting("cacheOffHiHat"), ReadBoolSetting("cacheOffRide"));

  // Variant-pool target size (HandleSetCacheVariantCount): the persisted project value or the default.
  {
    const int n = mCacheVariantCount.load(std::memory_order_relaxed);
    dgg::TR808RideVariantPool::Instance().SetTargetVariantCount(n);
    dgg::TR808HiHatVariantPool::Instance().SetTargetVariantCount(n);
  }

  // Initialize articulation gains to unity.
  for (auto& g : mArticulationGain) g.store(1.0, std::memory_order_relaxed);
  for (auto& g : mArticulationGainLoFi) g.store(1.0, std::memory_order_relaxed);
#endif

#if IPLUG_EDITOR
  // Set initial JS message buffer size; will grow as needed.
  SetMaxJSStringLength(mMaxJSBufferSize);

  // DevTools (right-click > Inspect) start on while debugging; the Options menu switches them off (HandleSetDevTools).
  // Set this back to false for a release build.
  SetEnableDevTools(true);

#ifdef OS_WIN
  // The web UI lives inside the DLL. Setting DGG_WEB_ROOT to a folder with index.html loads it from there
  // instead (live editing of the web files without rebuilding).
  char devWebRoot[MAX_PATH] = {};
  const bool useDevWebRoot = GetEnvironmentVariableA("DGG_WEB_ROOT", devWebRoot, MAX_PATH) > 0;
  if (!useDevWebRoot) gIPlugEmbeddedWebResource = &DggEmbeddedWebResource;
#endif

  mEditorInitFunc = [this]() {
    DGG_Log("mEditorInitFunc / OnWebViewReady fired");

#ifdef OS_WIN
    char root[MAX_PATH] = {};
    if (GetEnvironmentVariableA("DGG_WEB_ROOT", root, MAX_PATH) > 0)
    {
      const std::string indexPath = (std::filesystem::path(root) / "index.html").string();
      DGG_Log("Loading web UI from DGG_WEB_ROOT: " + indexPath);
      LoadFile(indexPath.c_str());
    }
    else
    {
      DGG_Log("Loading embedded web UI");
      LoadURL("https://iplug.example/index.html");
    }
#else
    LoadIndexHtml(PROJECT_ROOT_PATH, BUNDLE_ID);
#endif
    UpdateWebViewBoundsForHostWindow();
  };
#endif

  DGG_Log("Constructor end");
}

#if IPLUG_EDITOR
#ifdef OS_WIN
namespace {
  const UINT kDragResizeMsg = WM_APP + 1;

  LRESULT CALLBACK DGG_ResizeTimerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
  {
    if (msg == kDragResizeMsg)
    {
      DrumGroover* self = reinterpret_cast<DrumGroover*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
      if (self) self->OnDragResizeTimerFired();
      return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
  }
}

void DrumGroover::EnsureResizeTimerWindow()
{
  if (mResizeTimerWnd) return;
  HWND hwnd = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
  if (!hwnd)
  {
    DGG_Log("EnsureResizeTimerWindow: CreateWindowExW failed, GetLastError=" + std::to_string(GetLastError()));
    return;
  }
  SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(DGG_ResizeTimerWndProc));
  mResizeTimerWnd = hwnd;
}

void DrumGroover::OnDragResizeTimerFired()
{
  mDragResizePosted = false;
  DGG_Log("OnDragResizeTimerFired: requesting real resize to " + std::to_string(mPendingDragW) + "x" + std::to_string(mPendingDragH));
  mAwaitingSelfInitiatedResize = true;
  Resize(mPendingDragW, mPendingDragH);
}
#endif // OS_WIN

void DrumGroover::CloseWindow()
{
#ifdef OS_WIN
  if (mResizeTimerWnd)
  {
    DestroyWindow(static_cast<HWND>(mResizeTimerWnd));
    mResizeTimerWnd = nullptr;
  }
#endif
  WebViewEditorDelegate::CloseWindow();
}

void DrumGroover::UpdateWebViewBoundsForHostWindow()
{
#ifdef OS_WIN
  HWND hwnd = static_cast<HWND>(WebViewEditorDelegate::mView);
  RECT clientRect{};
  GetClientRect(hwnd, &clientRect);
  UINT dpi = GetDpiForWindow(hwnd);
  float hostScale = dpi / 96.0f;
  float logicalW = (clientRect.right - clientRect.left) / hostScale;
  float logicalH = (clientRect.bottom - clientRect.top) / hostScale;
  DGG_Log("Host window: client=" + std::to_string(clientRect.right - clientRect.left) + "x" + std::to_string(clientRect.bottom - clientRect.top)
    + " dpi=" + std::to_string(dpi) + " -> passing logicalW/H=" + std::to_string(logicalW) + "x" + std::to_string(logicalH));
  SetWebViewBounds(0.f, 0.f, logicalW, logicalH, 1.0f);
#else
  SetWebViewBounds(0.f, 0.f, static_cast<float>(PLUG_WIDTH), static_cast<float>(PLUG_HEIGHT), 1.0f);
#endif
}

bool DrumGroover::OnKeyDown(const IKeyPress& key)
{
#ifdef OS_WIN
  if (key.VK == kVK_SPACE)
  {
    if (!mInWebViewMessage)
      EvaluateJavaScript("(function(){var t=document.activeElement||document;t.dispatchEvent(new KeyboardEvent('keydown',{code:'Space',key:' ',keyCode:32,bubbles:true,cancelable:true}));})();");
    return true;
  }
#endif
  return WebViewEditorDelegate::OnKeyDown(key);
}

void DrumGroover::OnMessageFromWebView(const char* jsonStr)
{
  mInWebViewMessage = true;
  WebViewEditorDelegate::OnMessageFromWebView(jsonStr);
  mInWebViewMessage = false;
}

void DrumGroover::OnParentWindowResize(int width, int height)
{
  DGG_Log("OnParentWindowResize(" + std::to_string(width) + ", " + std::to_string(height) + ") — selfInitiated=" + std::string(mAwaitingSelfInitiatedResize ? "true" : "false"));

  if (mAwaitingSelfInitiatedResize)
  {
    mAwaitingSelfInitiatedResize = false;
    UpdateWebViewBoundsForHostWindow();
    EditorResizeFromUI(width, height, false);
    return;
  }

  // Host‑initiated resize: apply bounds and schedule a plugin‑initiated resize for proper frame update.
  float hostScale = 1.0f;
#ifdef OS_WIN
  HWND hwnd = static_cast<HWND>(WebViewEditorDelegate::mView);
  UINT dpi = GetDpiForWindow(hwnd);
  hostScale = dpi / 96.0f;
#endif
  const float logicalW = width / hostScale;
  const float logicalH = height / hostScale;

  SetWebViewBounds(0.f, 0.f, logicalW, logicalH, 1.0f);
  EditorResizeFromUI(width, height, false);

#ifdef OS_WIN
  mPendingDragW = width;
  mPendingDragH = height;
  EnsureResizeTimerWindow();
  if (mResizeTimerWnd && !mDragResizePosted)
  {
    mDragResizePosted = true;
    PostMessage(static_cast<HWND>(mResizeTimerWnd), kDragResizeMsg, 0, 0);
  }
#endif
}

bool DrumGroover::ConstrainEditorResize(int& w, int& h) const
{
  const int origW = w, origH = h;

  const double wRatio = static_cast<double>(w) / PLUG_WIDTH;
  const double hRatio = static_cast<double>(h) / PLUG_HEIGHT;
  const double scale = (wRatio + hRatio) / 2.0;

  w = static_cast<int>(std::lround(PLUG_WIDTH * scale));
  h = static_cast<int>(std::lround(PLUG_HEIGHT * scale));

  w = Clip(w, GetMinWidth(), GetMaxWidth());
  h = Clip(h, GetMinHeight(), GetMaxHeight());

  if (w != static_cast<int>(std::lround(PLUG_WIDTH * scale)))
    h = Clip(static_cast<int>(std::lround(PLUG_HEIGHT * (static_cast<double>(w) / PLUG_WIDTH))), GetMinHeight(), GetMaxHeight());

  return (w == origW && h == origH);
}

// --- Mixer state (see dsp/Mixer.h). ----------------------------------------
// Channel id strings must stay in sync with FIXED_CHANNELS in
// resources/web/mixer-tab.js (same order, same ids).
static const char* kMixerChannelNames[dgg::kMixerNumChannels] = {
  "kick", "snare", "hihat",
  "rackTom1", "rackTom2", "rackTom3", "floorTom1", "floorTom2",
  "cymbal1", "cymbal2", "cymbal3", "cymbal4", "cymbal5",
  "ride", "custom"
};

void DrumGroover::OnIdle()
{
  // Deferred refresh after UnserializeState.
  if (mPendingStateRefresh.exchange(false, std::memory_order_relaxed))
  {
    DGG_Log("OnIdle: servicing deferred state refresh from UnserializeState");
    PushEngineAndProjectStateToUI();
  }

  // Send level meter data.
  static const char* kFamilyNames[7] = { "kick", "snare", "hihat", "tom", "cymbal", "ride", "custom" };
  nlohmann::json levels = nlohmann::json::object();
  for (int f = 0; f < 7; f++)
  {
    const double peak = mFocusPeak[f].exchange(0.0, std::memory_order_relaxed);
    const double db = peak > 0.0 ? 20.0 * std::log10(peak) : -100.0;
    levels[kFamilyNames[f]] = db;
  }
  SendBridgeMessage("levelMeter", { {"levels", levels} });

  // Variant-pool bake progress (see 808VariantPoolBaker.h) -- polled here
  // like the meters above rather than pushed from the worker thread, so
  // the UI thread only ever reads SendBridgeMessage from OnIdle.
  {
    const auto snap = dgg::TR808VariantPoolBaker::Instance().GetSnapshot();
    PollApplyJob();
    DrainCacheMisses();
    // The first-run flag is latched in OnWebContentLoaded and must stay until the bake has actually finished; clearing it on a
    // merely idle baker would let the dialog show "Updating" instead of "Preparing" for the bakes running while the UI loads.
    if (gFirstRunBake.load() && snap.pendingJobs == 0
      && std::filesystem::exists(GetCacheDir() / "default.index"))
      gFirstRunBake.store(false);
    // Only send when the progress changed (plus a slow refresh): a message on every idle tick queues up in the WebView
    // while the bake keeps the CPU busy and arrives as a burst.
    const nlohmann::json progress = {
      {"active", snap.active}, {"done", snap.done}, {"total", snap.total}, {"label", snap.label},
      {"pendingJobs", snap.pendingJobs}, {"unitsDone", snap.unitsDone}, {"unitsTotal", snap.unitsPlanned}, {"labels", snap.labels},
      {"firstRun", gFirstRunBake.load()}, {"preview", false}
    };
    static nlohmann::json sLastProgress;
    static auto sLastProgressSend = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (progress != sLastProgress || now - sLastProgressSend > std::chrono::seconds(1))
    {
      sLastProgress = progress;
      sLastProgressSend = now;
      SendBridgeMessage("cacheBakeProgress", progress);
    }
  }

  // Mixer strip meters: L/R peak dB per channel + master + buses.
  {
    auto db = [](std::atomic<double>& a) {
      const double peak = a.exchange(0.0, std::memory_order_relaxed);
      return peak > 0.0 ? 20.0 * std::log10(peak) : -100.0;
    };
    nlohmann::json mixLevels = nlohmann::json::object();
    for (int i = 0; i < dgg::kMixerNumChannels; ++i)
      mixLevels[kMixerChannelNames[i]] = { db(mMixerPeakL[i]), db(mMixerPeakR[i]) };
    mixLevels["master"]    = { db(mMixerMasterPeakL), db(mMixerMasterPeakR) };
    mixLevels["reverbBus"] = { db(mMixerRevPeakL),    db(mMixerRevPeakR) };
    mixLevels["delayBus"]  = { db(mMixerDelPeakL),     db(mMixerDelPeakR) };
    mixLevels["kit"]       = { db(mMixerKitInL),       db(mMixerKitInR) };

    // Pre-limiter input peaks (dB), so the UI can show how far above its ceiling
    // a limiter's input ran — i.e. whether/where it engaged. Same [L, R] shape as
    // `levels`; only channels + master carry limiters in practice.
    nlohmann::json mixLimIn = nlohmann::json::object();
    for (int i = 0; i < dgg::kMixerNumChannels; ++i)
      mixLimIn[kMixerChannelNames[i]] = { db(mMixerLimInL[i]), db(mMixerLimInR[i]) };
    mixLimIn["master"] = { db(mMixerMasterLimInL), db(mMixerMasterLimInR) };

    nlohmann::json mixLimOut = nlohmann::json::object();
    for (int i = 0; i < dgg::kMixerNumChannels; ++i)
      mixLimOut[kMixerChannelNames[i]] = { db(mMixerLimOutL[i]), db(mMixerLimOutR[i]) };
    mixLimOut["master"] = { db(mMixerMasterLimOutL), db(mMixerMasterLimOutR) };

    // Limiter gain reduction in dB (>= 0), measured on the limiter's own gain so it lines up with its output.
    auto grDb = [](std::atomic<double>& a) {
      const double r = a.exchange(0.0, std::memory_order_relaxed);
      return r > 0.0 ? -20.0 * std::log10(std::max(1.0 - r, 1e-5)) : 0.0;
    };
    nlohmann::json mixLimGr = nlohmann::json::object();
    for (int i = 0; i < dgg::kMixerNumChannels; ++i)
      mixLimGr[kMixerChannelNames[i]] = { grDb(mMixerLimGrL[i]), grDb(mMixerLimGrR[i]) };
    mixLimGr["master"] = { grDb(mMixerMasterLimGrL), grDb(mMixerMasterLimGrR) };

    SendBridgeMessage("mixerLevels", { {"levels", mixLevels}, {"limIn", mixLimIn}, {"limOut", mixLimOut}, {"limGr", mixLimGr} });
  }

  // Preview position.
  SendBridgeMessage("previewPosition", {
    {"playing", mPreviewScheduler.IsPlaying()},
    {"beat", mPreviewScheduler.CurrentBeat()}
  });

  // Song position (host‑driven).
  SendBridgeMessage("songPosition", {
    {"playing", GetTransportIsRunning()},
    {"beat", mScheduler.CurrentPosition(GetPPQPos(),
      mLoopStartBeat.load(std::memory_order_relaxed),
      mLoopEndBeat.load(std::memory_order_relaxed))}
  });
}
#endif

#if IPLUG_EDITOR
void DrumGroover::OnWebContentLoaded()
{
  DGG_Log("OnWebContentLoaded fired — page + scripts finished loading");
  WebViewEditorDelegate::OnWebContentLoaded();

  // First-start latch: no default.index on disk means the factory-default sets are not baked yet. Set it here, before any bake
  // path runs (the pattern sync during UI load can already enqueue bakes), so the dialog shows "Preparing the sound cache"
  // from its first frame. Cleared in OnIdle once that bake has finished and the index exists.
  if (!std::filesystem::exists(GetCacheDir() / "default.index")) gFirstRunBake.store(true);

  // Before the instance id: the UI's dggOnReady callbacks run on the id and may already need the table.
  SendBridgeMessage("instrumentTable", dgg::InstrumentTableToJson());
  SendBridgeMessage("instanceId", { {"id", mInstanceId} });

  PushEngineAndProjectStateToUI();
}

void DrumGroover::PushEngineAndProjectStateToUI()
{
  std::string cachedProjectJson;
  std::map<std::string, PadPos> padPositionsCopy;
  {
    std::lock_guard<std::mutex> lock(mStateMutex);
    cachedProjectJson = mCachedProjectJson;
    padPositionsCopy = mPadPositions;
  }

  if (!cachedProjectJson.empty())
  {
    DGG_Log("PushEngineAndProjectStateToUI: restoring cached state (" + std::to_string(cachedProjectJson.size()) + " bytes)");
    SendBridgeMessage("restoreState", { {"text", cachedProjectJson} });
  }

  SendBridgeMessage("polyphonyState", {
    {"kick",  mKickPools[0].GetPolyphony()},
    {"snare", mSnareSdPools[0].GetPolyphony()},
    {"tom",   mTomPools[0].GetPolyphony()},
    {"ride",  mRidePools[0].GetPolyphony()},
  });

  SendBridgeMessage("cacheVariantCount", { {"value", mCacheVariantCount.load(std::memory_order_relaxed)} });

  SendBridgeMessage("engineModeState", {
    {"engine", mEngineMode.load(std::memory_order_relaxed) == static_cast<int>(EngineMode::LoFi) ? "lofi" : "internal"},
  });

  SendBridgeMessage("mutedState", {
    {"muted", mUserMuted.load(std::memory_order_relaxed)},
  });

  {
    const int engineMode = mEngineMode.load(std::memory_order_relaxed);
    const bool isLoFi = engineMode == static_cast<int>(EngineMode::LoFi);
    nlohmann::json articulationGains = BuildArticulationGainsJson(isLoFi);
    SendBridgeMessage("articulationLevelState", articulationGains);
  }

  {
    nlohmann::json voiceParams = BuildVoiceParamsJson();
    voiceParams["__configured"] = mVoiceParamsConfigured.load(std::memory_order_relaxed);
    SendBridgeMessage("voiceParamState", voiceParams);
  }

  {
    nlohmann::json padPositions = nlohmann::json::object();
    for (const auto& kv : padPositionsCopy) {
      padPositions[kv.first] = { {"x", kv.second.x}, {"y", kv.second.y}, {"synced", kv.second.synced} };
    }
    SendBridgeMessage("padPositionsState", padPositions);
  }

  // Mixer state — the UI applies it to its strips on load (see mixer-tab.js).
  SendBridgeMessage("mixerState", BuildMixerStateJson());
}

nlohmann::json DrumGroover::BuildVoiceParamsJson() const
{
  nlohmann::json voiceParams = nlohmann::json::object();
  ForEachKickPool([&](const dgg::InstrumentDef& d, const auto& pool) {
    for (auto& v : pool.Voices()) { voiceParams["kick:" + std::to_string(d.paramIndex)] = ParamsJsonOf(v); break; }
  });
  ForEachSnarePool([&](const dgg::InstrumentDef& d, const auto& pool) {
    for (auto& v : pool.Voices()) { voiceParams["snare:" + std::to_string(d.paramIndex)] = ParamsJsonOf(v); break; }
  });
  ForEachHiHatPool([&](const dgg::InstrumentDef& d, const auto& pool) {
    for (auto& v : pool.Voices()) { voiceParams["hihat:" + std::to_string(d.paramIndex)] = ParamsJsonOf(v); break; }
  });
  ForEachTomPool([&](const dgg::InstrumentDef& d, const auto& pool) {
    for (auto& v : pool.Voices()) { voiceParams["tom:" + std::to_string(d.paramIndex)] = ParamsJsonOf(v); break; }
  });
  for (int k = 0; k < dgg::kNumCustomInstruments; k++)
  {
    if (dgg::CustomInstrument(k).kind == dgg::EngineKind::Tom)
      for (auto& v : mCustomPools[k].Voices()) { voiceParams["custom:" + std::to_string(k)] = TomVoiceParamsToJson(v); break; }
    else if (IsHcKind(dgg::CustomInstrument(k).kind))
      for (auto& v : mCustomHcPools[k].Voices()) { voiceParams["custom:" + std::to_string(k)] = HcVoiceParamsToJson(v); break; }
    else if (IsCbKind(dgg::CustomInstrument(k).kind))
      for (auto& v : mCustomCbPools[k].Voices()) { voiceParams["custom:" + std::to_string(k)] = CbVoiceParamsToJson(v); break; }
    else if (IsPc2Kind(dgg::CustomInstrument(k).kind))
      for (auto& v : mCustomPc2Pools[k].Voices()) { voiceParams["custom:" + std::to_string(k)] = Pc2VoiceParamsToJson(v); break; }
    else
      for (auto& v : mCustomRmPools[k].Voices()) { voiceParams["custom:" + std::to_string(k)] = RmVoiceParamsToJson(v); break; }
  }
  ForEachCymbalPool([&](const dgg::InstrumentDef& d, const auto& pool) {
    for (auto& v : pool.Voices()) { voiceParams["cymbal:" + std::to_string(d.paramIndex)] = RideVoiceParamsToJson(v); break; }
  });
  ForEachRidePool([&](const dgg::InstrumentDef& d, const auto& pool) {
    for (auto& v : pool.Voices()) { voiceParams["ride:" + std::to_string(d.paramIndex)] = RideVoiceParamsToJson(v); break; }
  });
  return voiceParams;
}

nlohmann::json DrumGroover::BuildArticulationGainsJson(bool loFi) const
{
  nlohmann::json gains = nlohmann::json::object();
  for (int note = 0; note < 128; ++note)
  {
    const double gain = loFi
        ? mArticulationGainLoFi[note].load(std::memory_order_relaxed)
        : mArticulationGain[note].load(std::memory_order_relaxed);
    gains[std::to_string(note)] = gain;
  }
  return gains;
}

void DrumGroover::ApplyVoiceParamsJson(const nlohmann::json& voiceParams)
{
  if (!voiceParams.is_object()) return;
  for (auto& [key, params] : voiceParams.items())
  {
    if (!params.is_object()) continue;
    const size_t colonPos = key.find(':');
    if (colonPos == std::string::npos) continue;
    const std::string family = key.substr(0, colonPos);
    int index = -1;
    try { index = std::stoi(key.substr(colonPos + 1)); } catch (...) { continue; }

    for (auto& [param, value] : params.items())
    {
      if (!value.is_number()) continue;
      HandleSetVoiceParam({ {"family", family}, {"param", param}, {"value", value.get<double>()}, {"index", index} });
    }
  }
}

void DrumGroover::ApplyArticulationGainsJson(const nlohmann::json& gains, bool loFi)
{
  if (!gains.is_object()) return;
  for (auto& [noteStr, value] : gains.items())
  {
    if (!value.is_number()) continue;
    int note = -1;
    try { note = std::stoi(noteStr); } catch (...) { continue; }
    if (note < 0 || note > 127) continue;
    if (loFi) mArticulationGainLoFi[note].store(value.get<double>(), std::memory_order_relaxed);
    else      mArticulationGain[note].store(value.get<double>(), std::memory_order_relaxed);
  }
}

void DrumGroover::SendBridgeMessage(const char* type, const nlohmann::json& payload)
{
  nlohmann::json msg;
  msg["type"] = type;
  msg["payload"] = payload;

  const size_t rawLen = msg.dump().size();
  const size_t base64Len = ((rawLen + 2) / 3) * 4;
  const size_t needed = base64Len + 64;

  if (needed > mMaxJSBufferSize)
  {
    constexpr size_t kMB = 1024 * 1024;
    size_t newLimit = ((needed / kMB) + 1) * kMB;
    constexpr size_t kAbsoluteCeiling = 256 * kMB;
    if (newLimit > kAbsoluteCeiling)
    {
      DGG_Log("SendBridgeMessage('" + std::string(type) + "'): needed " + std::to_string(needed)
        + " bytes, exceeds absolute ceiling " + std::to_string(kAbsoluteCeiling) + " — NOT growing, message will be truncated");
    }
    else
    {
      DGG_Log("SendBridgeMessage('" + std::string(type) + "'): growing JS buffer from " + std::to_string(mMaxJSBufferSize)
        + " to " + std::to_string(newLimit) + " bytes (needed " + std::to_string(needed) + ")");
      SetMaxJSStringLength(newLimit);
      mMaxJSBufferSize = newLimit;
    }
  }

  SendJSONFromDelegate(msg);
}

bool DrumGroover::OnMessage(int msgTag, int ctrlTag, int dataSize, const void* pData)
{
  if (msgTag != -1) return false;

  std::string jsonStr(static_cast<const char*>(pData), static_cast<size_t>(dataSize));
  auto json = nlohmann::json::parse(jsonStr, nullptr, false);
  if (json.is_discarded())
  {
    DGG_Log("OnMessage: JSON parse failed, raw jsonStr=" + jsonStr);
    return false;
  }

  const std::string type = json.value("type", "");
  const nlohmann::json payload = json.contains("payload") ? json["payload"] : nlohmann::json::object();

  try
  {
    if (type == "exportSong")            HandleExportSong(payload);
    else if (type == "exportBlock")      HandleExportBlock(payload);
    else if (type == "exportBlocks")     HandleExportBlocks(payload);
    else if (type == "startNativeDragExportBlock") HandleStartNativeDragExportBlock(payload);
    else if (type == "startNativeDragExportSelection") HandleStartNativeDragExportSelection(payload);
    else if (type == "syncPlaybackPattern") HandleSyncPlaybackPattern(payload);
    else if (type == "previewPlay")       HandlePreviewPlay(payload);
    else if (type == "previewStop")       HandlePreviewStop(payload);
    else if (type == "previewSeek")       HandlePreviewSeek(payload);
    else if (type == "regenerateKickGroup") HandleRegenerateKickGroup(payload);
    else if (type == "regenerateSnareGroup") HandleRegenerateSnareGroup(payload);
    else if (type == "regenerateSnareGhostLayer") HandleRegenerateSnareGhostLayer(payload);
    else if (type == "regenerateHihatGroup") HandleRegenerateHihatGroup(payload);
    else if (type == "regenerateRideGroup") HandleRegenerateRideGroup(payload);
    else if (type == "regenerateTomsGroup") HandleRegenerateTomsGroup(payload);
    else if (type == "regenerateCymbalsGroup") HandleRegenerateCymbalsGroup(payload);
    else if (type == "regenerateCustomGroup") HandleRegenerateCustomGroup(payload);
    else if (type == "regenerateBreakBlock") HandleRegenerateBreakBlock(payload);
    else if (type == "regenerateBreakMain") HandleRegenerateBreakMain(payload);
    else if (type == "regenerateBreakKick") HandleRegenerateBreakKick(payload);
    else if (type == "regenerateFillBlock") HandleRegenerateFillBlock(payload);
    else if (type == "saveProject")      HandleSaveProject(payload);
    else if (type == "exportHowTo")      HandleExportHowTo(payload);
    else if (type == "requestLoadProject") HandleRequestLoadProject();
    else if (type == "projectIoGo")      HandleProjectIoGo();
    else if (type == "setUIScale")       HandleSetUIScale(payload);
    else if (type == "cacheState")       HandleCacheState(payload);
    else if (type == "midiNoteOn")       HandleMidiNoteOn(payload);
    else if (type == "midiNoteOff")      HandleMidiNoteOff(payload);
    else if (type == "setPolyphony")     HandleSetPolyphony(payload);
      else if (type == "commitVoiceParam") HandleCommitVoiceParam(payload);
    else if (type == "setCacheVariantCount") HandleSetCacheVariantCount(payload);
    else if (type == "queryCacheBakeEstimate") HandleQueryCacheBakeEstimate(payload);
    else if (type == "applyCacheVariantCount") HandleApplyCacheVariantCount(payload);
    else if (type == "cancelCacheBake") HandleCancelCacheBake(payload);
    else if (type == "queryCacheInfo") SendCacheInfo();
    else if (type == "setCacheLimit") HandleSetCacheLimit(payload);
    else if (type == "setRebakeOnChange") HandleSetRebakeOnChange(payload);
    else if (type == "setCacheDisabled") HandleSetCacheDisabled(payload);
    else if (type == "clearCache") HandleClearCache(payload);
    else if (type == "setFactoryDefaults") HandleSetFactoryDefaults(payload);
    else if (type == "setCustomInstruments") HandleSetCustomInstruments(payload);
    else if (type == "customPreview") HandleCustomPreview(payload);
    else if (type == "setCustomPreviewGain") HandleSetCustomPreviewGain(payload);
    else if (type == "setEngineMode")    HandleSetEngineMode(payload);
    else if (type == "mixerPresetList")    HandleMixerPresetList(payload);
    else if (type == "mixerPresetSave")    HandleMixerPresetSave(payload);
    else if (type == "mixerPresetLoad")    HandleMixerPresetLoad(payload);
    else if (type == "mixerPresetDelete")  HandleMixerPresetDelete(payload);
    else if (type == "mixerPresetRename")  HandleMixerPresetRename(payload);
    else if (type == "mixerPresetDefault") HandleMixerPresetDefault(payload);
    else if (type == "setMuted")         HandleSetMuted(payload);
    else if (type == "setDevTools")      HandleSetDevTools(payload);
    else if (type == "mixerState")       HandleMixerState(payload);
    else if (type == "setVoiceParam")    HandleSetVoiceParam(payload);
    else if (type == "setPadPos")         HandleSetPadPos(payload);
    else if (type == "setArticulationLevel") HandleSetArticulationLevel(payload);
    else
    {
      DGG_Log("OnMessage: unrecognized type='" + type + "'");
      return false;
    }
  }
  catch (const std::exception& ex)
  {
    DGG_Log("OnMessage: handler for type='" + type + "' threw: " + std::string(ex.what()));
    return false;
  }
  catch (...)
  {
    DGG_Log("OnMessage: handler for type='" + type + "' threw a non-std\u002Dexception value");
    return false;
  }

  return true;
}

void DrumGroover::HandleExportSong(const nlohmann::json& payload)
{
  dgg::FullPattern fullPattern = FullPatternFromJson(payload.at("fullPattern"));
  dgg::ExportParams params     = ExportParamsFromJson(payload.at("params"));
  dgg::Grid grid                = GridFromJson(payload.at("grid"));

#ifdef OS_WIN
  const std::string suggested = "dgg-song-" + TimestampSlug() + ".mid";
  const std::string path = Win32SaveFileDialog(static_cast<HWND>(WebViewEditorDelegate::mView),
    L"MIDI File (*.mid)\0*.mid\0\0", L"mid", suggested);
#elif defined(OS_MAC)
  const std::string suggested = "dgg-song-" + TimestampSlug() + ".mid";
  const std::string path = MacSaveFileDialog(WebViewEditorDelegate::mView, "mid", "MIDI File", suggested);
#else
  const std::string path;
#endif

  if (path.empty()) return;

  bool ok = mMidiExport.exportSong(fullPattern, params, grid, path);
  if (ok) SendBridgeMessage("exportComplete", { {"filename", path} });
  else    SendBridgeMessage("exportFailed", { {"error", "MIDI export failed"} });
}

void DrumGroover::HandleExportBlock(const nlohmann::json& payload)
{
  dgg::Block block          = BlockFromJson(payload.at("block"));
  dgg::ExportParams params  = ExportParamsFromJson(payload.at("params"));
  dgg::Grid grid            = GridFromJson(payload.at("grid"));

#ifdef OS_WIN
  const std::string stamp = payload.value("stamp", "");
  const std::string suggested = "dgg-block-" + Slugify(block.name) + "-" + (stamp.empty() ? TimestampSlug() : stamp) + ".mid";
  const std::string path = Win32SaveFileDialog(static_cast<HWND>(WebViewEditorDelegate::mView),
    L"MIDI File (*.mid)\0*.mid\0\0", L"mid", suggested);
#elif defined(OS_MAC)
  const std::string stamp = payload.value("stamp", "");
  const std::string suggested = "dgg-block-" + Slugify(block.name) + "-" + (stamp.empty() ? TimestampSlug() : stamp) + ".mid";
  const std::string path = MacSaveFileDialog(WebViewEditorDelegate::mView, "mid", "MIDI File", suggested);
#else
  const std::string path;
#endif

  if (path.empty()) return;

  bool ok = mMidiExport.exportBlock(block, params, grid, path);
  if (ok) SendBridgeMessage("exportComplete", { {"filename", path} });
  else    SendBridgeMessage("exportFailed", { {"error", "MIDI export failed"} });
}

// One save dialog for the whole batch: the chosen folder and file name stem apply to
// every block, written as <stem>-<block name>.mid.
void DrumGroover::HandleExportBlocks(const nlohmann::json& payload)
{
  dgg::ExportParams params = ExportParamsFromJson(payload.at("params"));
  dgg::Grid grid           = GridFromJson(payload.at("grid"));
  const auto& blocksJson   = payload.at("blocks");
  if (!blocksJson.is_array() || blocksJson.empty()) return;

  const std::string suggested = "dgg-blocks-" + TimestampSlug() + ".mid";
#ifdef OS_WIN
  const std::string chosen = Win32SaveFileDialog(static_cast<HWND>(WebViewEditorDelegate::mView),
    L"MIDI File (*.mid)\0*.mid\0\0", L"mid", suggested, L"Export ALL blocks - one .mid file per block (the name is only the prefix)");
#elif defined(OS_MAC)
  const std::string chosen = MacSaveFileDialog(WebViewEditorDelegate::mView, "mid", "MIDI File", suggested);
#else
  const std::string chosen;
#endif
  if (chosen.empty()) return;

  // Split the UTF-8 path as text (dir incl. trailing separator, name stem without ".mid").
  const size_t sep = chosen.find_last_of("\\/");
  const std::string dir = (sep == std::string::npos) ? std::string() : chosen.substr(0, sep + 1);
  std::string stem = (sep == std::string::npos) ? chosen : chosen.substr(sep + 1);
  if (stem.size() > 4)
  {
    std::string ext = stem.substr(stem.size() - 4);
    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
    if (ext == ".mid") stem.resize(stem.size() - 4);
  }

  std::set<std::string> used;
  int failed = 0;
  for (const auto& bj : blocksJson)
  {
    dgg::Block block = BlockFromJson(bj);
    std::string name = Slugify(block.name);
    std::string unique = name;
    for (int n = 2; !used.insert(unique).second; ++n) unique = name + "-" + std::to_string(n);
    const std::string path = dir + stem + "-" + unique + ".mid";

    if (!mMidiExport.exportBlock(block, params, grid, path)) ++failed;
  }

  if (failed == 0) SendBridgeMessage("exportComplete", { {"filename", std::to_string(blocksJson.size()) + " block files in " + dir} });
  else             SendBridgeMessage("exportFailed", { {"error", "MIDI export failed for " + std::to_string(failed) + " block(s)"} });
}

#ifdef OS_WIN
bool DrumGroover::RunNativeFileExportDrag(const std::string& path, const char* logPrefix)
{
  using namespace DragAndDropHelpers;
  OleInitialize(nullptr);

  FORMATETC format = { CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
  DataObject* dataObj = new DataObject(&format, path.c_str());
  DropSource* dropSource = new DropSource();

  DGG_Log(std::string(logPrefix) + ": calling DoDragDrop...");
  DWORD dropEffect = 0;
  HRESULT hr = DoDragDrop(dataObj, dropSource, DROPEFFECT_COPY, &dropEffect);
  DGG_Log(std::string(logPrefix) + ": DoDragDrop returned hr=0x" + [&]{ char b[16]; sprintf_s(b, "%08lX", (unsigned long)hr); return std::string(b); }()
    + " (" + (hr == DRAGDROP_S_DROP ? "DRAGDROP_S_DROP" : hr == DRAGDROP_S_CANCEL ? "DRAGDROP_S_CANCEL" : "other") + ") dropEffect=" + std::to_string(dropEffect));

  dataObj->Release();
  dropSource->Release();
  OleUninitialize();
  ReleaseCapture();

  return hr == DRAGDROP_S_DROP;
}
#endif

void DrumGroover::HandleStartNativeDragExportBlock(const nlohmann::json& payload)
{
  dgg::Block block          = BlockFromJson(payload.at("block"));
  dgg::ExportParams params  = ExportParamsFromJson(payload.at("params"));
  dgg::Grid grid            = GridFromJson(payload.at("grid"));
  DGG_Log("HandleStartNativeDragExportBlock: block.id='" + block.id + "'");

#ifdef OS_WIN
  std::error_code ec;
  std::filesystem::path exportsDir = std::filesystem::temp_directory_path(ec) / "DrumGrooverExports";
  if (!ec) std::filesystem::create_directories(exportsDir, ec);
  const std::string path = (exportsDir / ("dgg-drag-" + block.id + ".mid")).string();

  bool ok = mMidiExport.exportBlockSingleTrack(block, params, grid, path);
  DGG_Log("HandleStartNativeDragExportBlock: exportBlockSingleTrack('" + path + "') -> " + (ok ? "OK" : "FAILED"));
  if (!ok)
  {
    SendBridgeMessage("dragExportFailed", { {"blockId", block.id}, {"error", "MIDI export failed"} });
    return;
  }

  if (RunNativeFileExportDrag(path, "HandleStartNativeDragExportBlock"))
  {
    SendBridgeMessage("dragExportReady", { {"blockId", block.id}, {"filename", "dgg-drag-" + block.id + ".mid"} });
  }
  else
  {
    std::error_code ec2;
    std::filesystem::remove(path, ec2);
    DGG_Log("HandleStartNativeDragExportBlock: drop not accepted, removed '" + path + "' -> " + (ec2 ? ("ERROR: " + ec2.message()) : "OK"));
    SendBridgeMessage("dragExportFailed", { {"blockId", block.id}, {"error", "drag cancelled or rejected"} });
  }
#else
  DGG_Log("HandleStartNativeDragExportBlock: not OS_WIN, unsupported");
#endif
}

void DrumGroover::HandleStartNativeDragExportSelection(const nlohmann::json& payload)
{
  dgg::FullPattern fullPattern = FullPatternFromJson(payload.at("fullPattern"));
  dgg::ExportParams params     = ExportParamsFromJson(payload.at("params"));
  dgg::Grid grid                = GridFromJson(payload.at("grid"));
  DGG_Log("HandleStartNativeDragExportSelection: events=" + std::to_string(fullPattern.events.size()) + " totalBeats=" + std::to_string(fullPattern.totalBeats));

#ifdef OS_WIN
  std::error_code ec;
  std::filesystem::path exportsDir = std::filesystem::temp_directory_path(ec) / "DrumGrooverExports";
  if (!ec) std::filesystem::create_directories(exportsDir, ec);
  const std::string path = (exportsDir / "dgg-drag-selection.mid").string();

  bool ok = mMidiExport.exportFullPatternSingleTrack(fullPattern, params, grid, "Selection", path);
  DGG_Log("HandleStartNativeDragExportSelection: exportFullPatternSingleTrack('" + path + "') -> " + (ok ? "OK" : "FAILED"));
  if (!ok)
  {
    SendBridgeMessage("dragExportFailed", { {"error", "MIDI export failed"} });
    return;
  }

  if (RunNativeFileExportDrag(path, "HandleStartNativeDragExportSelection"))
  {
    SendBridgeMessage("dragExportReady", { {"filename", "dgg-drag-selection.mid"} });
  }
  else
  {
    std::error_code ec2;
    std::filesystem::remove(path, ec2);
    DGG_Log("HandleStartNativeDragExportSelection: drop not accepted, removed '" + path + "' -> " + (ec2 ? ("ERROR: " + ec2.message()) : "OK"));
    SendBridgeMessage("dragExportFailed", { {"error", "drag cancelled or rejected"} });
  }
#else
  DGG_Log("HandleStartNativeDragExportSelection: not OS_WIN, unsupported");
#endif
}

void DrumGroover::HandleSaveProject(const nlohmann::json& payload)
{
  std::string json = payload.value("json", "");
  std::string suggestedFilename = payload.value("suggestedFilename", "project.json");

#ifdef OS_WIN
  const std::string path = Win32SaveFileDialog(static_cast<HWND>(WebViewEditorDelegate::mView),
    L"DGG Project (*.json)\0*.json\0\0", L"json", suggestedFilename);
#elif defined(OS_MAC)
  const std::string path = MacSaveFileDialog(WebViewEditorDelegate::mView, "json", "DGG Project", suggestedFilename);
#else
  const std::string path;
#endif

  if (path.empty())
  {
    SendBridgeMessage("projectIoDone", {});
    return;
  }

  mPendingIoPath = path;
  mPendingIoJson = std::move(json);
  mPendingIoIsSave = true;
  SendBridgeMessage("projectIoBusy", { {"text", "Saving project..."} });
}

void DrumGroover::HandleExportHowTo(const nlohmann::json& payload)
{
  const std::string html = payload.value("html", "");
  const std::string suggestedFilename = payload.value("suggestedFilename", "DrumGroover-HowTo.html");
  if (html.empty()) return;

#ifdef OS_WIN
  const std::string path = Win32SaveFileDialog(static_cast<HWND>(WebViewEditorDelegate::mView),
    L"HTML Document (*.html)\0*.html\0\0", L"html", suggestedFilename);
#elif defined(OS_MAC)
  const std::string path = MacSaveFileDialog(WebViewEditorDelegate::mView, "html", "HTML Document", suggestedFilename);
#else
  const std::string path;
#endif

  if (path.empty()) return;

  std::ofstream out(path, std::ios::binary);
  if (!out) return;
  out << html;
}

void DrumGroover::HandleRequestLoadProject()
{
#ifdef OS_WIN
  const std::string path = Win32OpenFileDialog(static_cast<HWND>(WebViewEditorDelegate::mView),
    L"DGG Project (*.json)\0*.json\0\0", L"json");
#elif defined(OS_MAC)
  const std::string path = MacOpenFileDialog(WebViewEditorDelegate::mView, "json", "DGG Project");
#else
  const std::string path;
#endif

  if (path.empty())
  {
    DGG_Log("HandleRequestLoadProject: dialog cancelled or failed");
    SendBridgeMessage("projectIoDone", {});
    return;
  }

  mPendingIoPath = path;
  mPendingIoJson.clear();
  mPendingIoIsSave = false;
  SendBridgeMessage("projectIoBusy", { {"text", "Loading project..."} });
}

// Second half of project save/load: the page has drawn its busy note and asks for the actual file work.
void DrumGroover::HandleProjectIoGo()
{
  const std::string path = std::move(mPendingIoPath);
  mPendingIoPath.clear();
  if (path.empty()) return;

  if (mPendingIoIsSave)
  {
    std::string json = std::move(mPendingIoJson);
    mPendingIoJson.clear();
    std::ofstream out(path, std::ios::binary);
    if (!out)
    {
      SendBridgeMessage("projectIoDone", {});
      return;
    }
    out << json;
    out.close();
    SendBridgeMessage("projectSaved", {});
    return;
  }

  std::ifstream in(path, std::ios::binary);
  if (!in)
  {
    DGG_Log("HandleRequestLoadProject: could not open file: " + path);
    SendBridgeMessage("projectIoDone", {});
    return;
  }

  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  DGG_Log("HandleRequestLoadProject: read " + std::to_string(text.size()) + " bytes from " + path);

  SendBridgeMessage("projectLoaded", { {"text", text} });
  DGG_Log("HandleRequestLoadProject: SendBridgeMessage('projectLoaded') call returned");
}

void DrumGroover::HandleCacheState(const nlohmann::json& payload)
{
  std::string json = payload.value("json", "");
  const size_t size = json.size();
  {
    std::lock_guard<std::mutex> lock(mStateMutex);
    mCachedProjectJson = std::move(json);
  }
  DGG_Log("HandleCacheState: cached " + std::to_string(size) + " bytes");
}

namespace {
// The variant pool (instrument, pool index) a GM note plays through; the pool index is the paramIndex of the note's row in the
// instrument table (dsp/808InstrumentTable.h), which is RideArticulationForPool()'s / HiHatPoolFamily()'s / the cymbal pool order.
struct VariantPoolTarget { std::string instrument; int poolIndex; };

std::vector<VariantPoolTarget> NoteToVariantPoolTargets(int note)
{
  if (const dgg::InstrumentDef* d = dgg::FindInstrumentByNote(note))
    if (dgg::StrEq(d->family, "hihat") || dgg::StrEq(d->family, "cymbal") || dgg::StrEq(d->family, "ride")) return {{d->family, d->paramIndex}};
  return {};
}
}  // namespace

void DrumGroover::HandleSyncPlaybackPattern(const nlohmann::json& payload)
{
  dgg::FullPattern fp = FullPatternFromJson(payload.at("fullPattern"));
  const size_t eventCount = fp.events.size();
  const double totalBeats = fp.totalBeats;
  RebakeRequiredVariantPools(fp);  // before the move below -- reads fp.events
  mScheduler.SetPattern(std::move(fp));

  const double loopStart = (payload.contains("loopStart") && !payload["loopStart"].is_null())
    ? payload["loopStart"].get<double>() : -1.0;
  const double loopEnd = (payload.contains("loopEnd") && !payload["loopEnd"].is_null())
    ? payload["loopEnd"].get<double>() : -1.0;
  mLoopStartBeat.store(loopStart, std::memory_order_relaxed);
  mLoopEndBeat.store(loopEnd, std::memory_order_relaxed);

  DGG_Log("HandleSyncPlaybackPattern: " + std::to_string(eventCount) + " events, totalBeats="
    + std::to_string(totalBeats) + " loopStart=" + std::to_string(loopStart)
    + " loopEnd=" + std::to_string(loopEnd));
}

void DrumGroover::RebakeRequiredVariantPools(const dgg::FullPattern& fp)
{
  std::unordered_set<std::string> seenTrackIds;
  std::set<std::pair<std::string, int>> targets;  // (instrument, poolIndex), deduped

  for (const auto& ev : fp.events)
  {
    if (!seenTrackIds.insert(ev.trackId).second) continue;  // already resolved this trackId
    auto trackIt = std::find_if(mTracks.begin(), mTracks.end(),
      [&](const dgg::Track& t) { return t.id == ev.trackId; });
    if (trackIt == mTracks.end()) continue;
    for (const auto& target : NoteToVariantPoolTargets(trackIt->gm))
      targets.insert({target.instrument, target.poolIndex});
  }

  std::string targetsStr;
  for (const auto& [instrument, poolIndex] : targets)
    targetsStr += instrument + ":" + std::to_string(poolIndex) + " ";
  DGG_Log("RebakeRequiredVariantPools: " + std::to_string(fp.events.size()) + " events, "
    + std::to_string(seenTrackIds.size()) + " distinct tracks, " + std::to_string(targets.size())
    + " variant-pool targets [" + targetsStr + "]");

  for (const auto& [instrument, poolIndex] : targets)
  {
    if (instrument == "ride")   EnqueueRideVariantBake(poolIndex);
    if (instrument == "hihat")  EnqueueHiHatVariantBake(poolIndex);
    if (instrument == "cymbal") EnqueueCrashVariantBake(poolIndex);
  }
}

void DrumGroover::HandlePreviewPlay(const nlohmann::json& payload)
{
  dgg::FullPattern fp = FullPatternFromJson(payload.at("fullPattern"));
  const size_t eventCount = fp.events.size();
  const double totalBeats = fp.totalBeats;
  RebakeRequiredVariantPools(fp);  // before the move below -- reads fp.events
  mPreviewScheduler.SetPattern(std::move(fp));

  const double loopStart = (payload.contains("loopStart") && !payload["loopStart"].is_null())
    ? payload["loopStart"].get<double>() : 0.0;
  const double loopEnd = (payload.contains("loopEnd") && !payload["loopEnd"].is_null())
    ? payload["loopEnd"].get<double>() : totalBeats;
  const double startBeat = payload.value("startBeat", loopStart);

  mPreviewScheduler.Start(startBeat, loopStart, loopEnd);

  DGG_Log("HandlePreviewPlay: " + std::to_string(eventCount) + " events, totalBeats="
    + std::to_string(totalBeats) + " startBeat=" + std::to_string(startBeat)
    + " loopStart=" + std::to_string(loopStart) + " loopEnd=" + std::to_string(loopEnd));
}

void DrumGroover::HandlePreviewStop(const nlohmann::json& /*payload*/)
{
  mPreviewScheduler.Stop();
  DGG_Log("HandlePreviewStop");
}

// Moves the running preview to another position and/or loop range without sending the pattern again.
void DrumGroover::HandlePreviewSeek(const nlohmann::json& payload)
{
  if (payload.contains("loopStart") && payload.contains("loopEnd") && !payload["loopStart"].is_null() && !payload["loopEnd"].is_null())
    mPreviewScheduler.SetLoop(payload["loopStart"].get<double>(), payload["loopEnd"].get<double>());
  if (payload.contains("startBeat") && !payload["startBeat"].is_null())
    mPreviewScheduler.Seek(payload["startBeat"].get<double>());
}

void DrumGroover::HandleRegenerateKickGroup(const nlohmann::json& payload)
{
  dgg::Block block = BlockFromJson(payload.at("block"));
  dgg::Grid grid    = GridFromJson(payload.at("grid"));

  const double densityPct       = payload.value("densityPct", 50.0);
  const double variationPct     = payload.value("variationPct", 0.0);
  const int    subdivisionIndex = payload.value("subdivisionIndex", 1);
  const int    phrase           = std::max(1, payload.value("phrase", 1));

  std::vector<double> anchorBeats;
  if (payload.contains("anchorBeats") && payload.at("anchorBeats").is_array()) {
    anchorBeats = payload.at("anchorBeats").get<std::vector<double>>();
  }

  dgg::TurnaroundFillCache cache;
  dgg::applyKickGroupToBlock(block, grid, densityPct, variationPct, subdivisionIndex, block.feel, phrase, cache,
                              anchorBeats);

  SendBridgeMessage("kickGroupRegenerated", {
    {"blockId",  block.id},
    {"kick",     NoteVectorToJson(block.data["kick"])},
    {"kickOpen", NoteVectorToJson(block.data["kickOpen"])},
  });

  DGG_Log("HandleRegenerateKickGroup: blockId='" + block.id + "' density=" + std::to_string(densityPct)
    + " variation=" + std::to_string(variationPct) + " subdivisionIndex=" + std::to_string(subdivisionIndex)
    + " phrase=" + std::to_string(phrase));
}

void DrumGroover::HandleRegenerateSnareGroup(const nlohmann::json& payload)
{
  dgg::Block block = BlockFromJson(payload.at("block"));
  dgg::Grid grid    = GridFromJson(payload.at("grid"));

  const double densityPct       = payload.value("densityPct", 50.0);
  const double ghostDensityPct  = payload.value("ghostDensityPct", 0.0);
  const double variationPct     = payload.value("variationPct", 0.0);
  const int    subdivisionIndex = payload.value("subdivisionIndex", 1);
  const int    phrase           = std::max(1, payload.value("phrase", 1));

  std::vector<double> anchorBeats;
  if (payload.contains("anchorBeats") && payload.at("anchorBeats").is_array()) {
    anchorBeats = payload.at("anchorBeats").get<std::vector<double>>();
  }

  dgg::TurnaroundFillCache cache;
  dgg::applySnareGroupToBlock(block, grid, densityPct, ghostDensityPct, variationPct, subdivisionIndex,
                               block.feel, phrase, cache, anchorBeats);

  SendBridgeMessage("snareGroupRegenerated", {
    {"blockId", block.id},
    {"tracks",  TracksToJson(block, {"snare", "snareOffCenter", "snareEdge", "snareRimShot", "rim", "snareRimOnly"})},
  });

  DGG_Log("HandleRegenerateSnareGroup: blockId='" + block.id + "' density=" + std::to_string(densityPct)
    + " ghostDensity=" + std::to_string(ghostDensityPct) + " variation=" + std::to_string(variationPct)
    + " subdivisionIndex=" + std::to_string(subdivisionIndex) + " phrase=" + std::to_string(phrase));
}

void DrumGroover::HandleRegenerateSnareGhostLayer(const nlohmann::json& payload)
{
  dgg::Block block = BlockFromJson(payload.at("block"));
  dgg::Grid grid    = GridFromJson(payload.at("grid"));

  const double ghostDensityPct  = payload.value("ghostDensityPct", 0.0);
  const int    subdivisionIndex = payload.value("subdivisionIndex", 1);
  const int    phrase           = std::max(1, payload.value("phrase", 1));

  dgg::TurnaroundFillCache cache;
  dgg::applySnareGhostLayer(block, grid, ghostDensityPct, subdivisionIndex, block.feel, phrase, cache);

  SendBridgeMessage("snareGhostLayerRegenerated", {
    {"blockId", block.id},
    {"snare",   NoteVectorToJson(block.data["snare"])},
  });

  DGG_Log("HandleRegenerateSnareGhostLayer: blockId='" + block.id + "' ghostDensity="
    + std::to_string(ghostDensityPct) + " subdivisionIndex=" + std::to_string(subdivisionIndex)
    + " phrase=" + std::to_string(phrase));
}

void DrumGroover::HandlePulseGroupRegen(const nlohmann::json& payload, const char* responseType,
                                         const std::vector<std::string>& trackIds,
                                         void (*applyFn)(dgg::Block&, const dgg::Grid&, double, double, int,
                                                          dgg::Feel, int, dgg::TurnaroundFillCache&))
{
  dgg::Block block = BlockFromJson(payload.at("block"));
  dgg::Grid grid    = GridFromJson(payload.at("grid"));

  const double densityPct       = payload.value("densityPct", 50.0);
  const double variationPct     = payload.value("variationPct", 0.0);
  const int    subdivisionIndex = payload.value("subdivisionIndex", 1);
  const int    phrase           = std::max(1, payload.value("phrase", 1));

  dgg::TurnaroundFillCache cache;
  applyFn(block, grid, densityPct, variationPct, subdivisionIndex, block.feel, phrase, cache);

  SendBridgeMessage(responseType, {
    {"blockId", block.id},
    {"tracks",  TracksToJson(block, trackIds)},
  });

  DGG_Log(std::string("HandlePulseGroupRegen[") + responseType + "]: blockId='" + block.id + "' density="
    + std::to_string(densityPct) + " variation=" + std::to_string(variationPct) + " subdivisionIndex="
    + std::to_string(subdivisionIndex) + " phrase=" + std::to_string(phrase));
}

void DrumGroover::HandleRegenerateHihatGroup(const nlohmann::json& payload)
{
  HandlePulseGroupRegen(payload, "hihatGroupRegenerated",
    { "chh", "hihatTightEdge", "hihatClosedTip", "hihatTightTip", "ohh", "hihatOpenEdge4" },
    &dgg::applyHihatGroupToBlock);
}

void DrumGroover::HandleRegenerateRideGroup(const nlohmann::json& payload)
{
  HandlePulseGroupRegen(payload, "rideGroupRegenerated",
    { "ride", "rideBowShank", "rideEdge", "rideBellTip", "rideBell", "rideMuteHit" },
    &dgg::applyRideGroupToBlock);
}

void DrumGroover::HandleRegenerateTomsGroup(const nlohmann::json& payload)
{
  HandlePulseGroupRegen(payload, "tomsGroupRegenerated",
    { "rackTom2Center", "tomHi", "floorTom1Center", "tomMid", "tomLow" },
    &dgg::applyTomsGroupToBlock);
}

void DrumGroover::HandleRegenerateCymbalsGroup(const nlohmann::json& payload)
{
  HandlePulseGroupRegen(payload, "cymbalsGroupRegenerated",
    { "crash1", "cymbal1Crash", "cymbal3Crash", "crash2", "cymbal5Crash" },
    &dgg::applyCymbalsGroupToBlock);
}

void DrumGroover::HandleRegenerateCustomGroup(const nlohmann::json& payload)
{
  dgg::Block block = BlockFromJson(payload.at("block"));
  dgg::Grid grid    = GridFromJson(payload.at("grid"));

  const std::string trackId        = payload.value("trackId", "");
  const double densityPct          = payload.value("densityPct", 50.0);
  const int    subdivisionIndex    = payload.value("subdivisionIndex", 1);
  const int    phrase              = std::max(1, payload.value("phrase", 1));

  if (trackId.empty())
  {
    SendBridgeMessage("customGroupRegenerateFailed", { {"blockId", block.id} });
    DGG_Log("HandleRegenerateCustomGroup: FAILED — missing trackId");
    return;
  }

  dgg::TurnaroundFillCache cache;
  dgg::applyCustomGroupToBlock(block, grid, trackId, densityPct, subdivisionIndex, block.feel, phrase, cache);

  SendBridgeMessage("customGroupRegenerated", {
    {"blockId", block.id},
    {"trackId", trackId},
    {"notes",   NoteVectorToJson(block.data[trackId])},
  });

  DGG_Log("HandleRegenerateCustomGroup: blockId='" + block.id + "' trackId='" + trackId + "' density="
    + std::to_string(densityPct) + " subdivisionIndex=" + std::to_string(subdivisionIndex)
    + " phrase=" + std::to_string(phrase));
}

void DrumGroover::HandleRegenerateBreakBlock(const nlohmann::json& payload)
{
  dgg::Block block  = BlockFromJson(payload.at("block"));
  dgg::Block parent = BlockFromJson(payload.at("parent"));
  dgg::Grid grid     = GridFromJson(payload.at("grid"));

  const bool mainLocked = payload.value("mainLocked", false);
  const bool kickLocked = payload.value("kickLocked", false);

  const bool ok = dgg::regenerateBreakBlockInPlace(block, parent, grid, mainLocked, kickLocked);
  if (!ok)
  {
    SendBridgeMessage("breakBlockRegenerateFailed", { {"blockId", block.id} });
    DGG_Log("HandleRegenerateBreakBlock: blockId='" + block.id + "' FAILED (not a Break block — empty breakParentId)");
    return;
  }

  SendBridgeMessage("breakBlockRegenerated", {
    {"blockId",    block.id},
    {"data",       BlockDataToJson(block)},
    {"length",     block.length},
    {"breakBeats", block.breakBeats},
  });

  DGG_Log("HandleRegenerateBreakBlock: blockId='" + block.id + "' parentId='" + parent.id + "' mainLocked="
    + std::to_string(mainLocked) + " kickLocked=" + std::to_string(kickLocked));
}

void DrumGroover::HandleRegenerateBreakMain(const nlohmann::json& payload)
{
  dgg::Block block  = BlockFromJson(payload.at("block"));
  dgg::Block parent = BlockFromJson(payload.at("parent"));
  dgg::Grid grid     = GridFromJson(payload.at("grid"));

  const bool ok = dgg::regenerateBreakMainInPlace(block, parent, grid);
  if (!ok)
  {
    SendBridgeMessage("breakMainRegenerateFailed", { {"blockId", block.id} });
    DGG_Log("HandleRegenerateBreakMain: blockId='" + block.id + "' FAILED (not a Break block)");
    return;
  }

  const auto targetTracksSet = dgg::breakFillTargetTracks();
  const std::vector<std::string> targetTracks(targetTracksSet.begin(), targetTracksSet.end());

  SendBridgeMessage("breakMainRegenerated", {
    {"blockId", block.id},
    {"tracks",  TracksToJson(block, targetTracks)},
  });

  DGG_Log("HandleRegenerateBreakMain: blockId='" + block.id + "' parentId='" + parent.id + "'");
}

void DrumGroover::HandleRegenerateBreakKick(const nlohmann::json& payload)
{
  dgg::Block block  = BlockFromJson(payload.at("block"));
  dgg::Block parent = BlockFromJson(payload.at("parent"));
  dgg::Grid grid     = GridFromJson(payload.at("grid"));

  const bool ok = dgg::regenerateBreakKickInPlace(block, parent, grid);
  if (!ok)
  {
    SendBridgeMessage("breakKickRegenerateFailed", { {"blockId", block.id} });
    DGG_Log("HandleRegenerateBreakKick: blockId='" + block.id + "' FAILED (not a Break block)");
    return;
  }

  SendBridgeMessage("breakKickRegenerated", {
    {"blockId", block.id},
    {"kick",    NoteVectorToJson(block.data["kick"])},
  });

  DGG_Log("HandleRegenerateBreakKick: blockId='" + block.id + "' parentId='" + parent.id + "'");
}

void DrumGroover::HandleRegenerateFillBlock(const nlohmann::json& payload)
{
  dgg::Block block = BlockFromJson(payload.at("block"));
  dgg::Grid grid    = GridFromJson(payload.at("grid"));

  const bool ok = dgg::regenerateFillBlockInPlace(block, grid);
  if (!ok)
  {
    SendBridgeMessage("fillBlockRegenerateFailed", { {"blockId", block.id} });
    DGG_Log("HandleRegenerateFillBlock: blockId='" + block.id + "' FAILED");
    return;
  }

  SendBridgeMessage("fillBlockRegenerated", {
    {"blockId",   block.id},
    {"data",      BlockDataToJson(block)},
    {"length",    block.length},
    {"fillBeats", block.fillBeats},
    {"feel",      FeelToJsonString(block.feel)},
  });

  DGG_Log("HandleRegenerateFillBlock: blockId='" + block.id + "'");
}

void DrumGroover::HandleMidiNoteOn(const nlohmann::json& payload)
{
  const int note = payload.value("note", -1);
  if (note < 0 || note > 127) return;

  int velocity = payload.value("velocity", 127);
  velocity = std::max(1, std::min(127, velocity));

  IMidiMsg msg;
  msg.MakeNoteOnMsg(note, velocity, 0);
  SendMidiMsgFromUI(msg);
  DGG_Log("HandleMidiNoteOn: note=" + std::to_string(note) + " velocity=" + std::to_string(velocity));
}

void DrumGroover::HandleMidiNoteOff(const nlohmann::json& payload)
{
  const int note = payload.value("note", -1);
  if (note < 0 || note > 127) return;

  IMidiMsg msg;
  msg.MakeNoteOffMsg(note, 0);
  SendMidiMsgFromUI(msg);
  DGG_Log("HandleMidiNoteOff: note=" + std::to_string(note));
}

void DrumGroover::HandleSetUIScale(const nlohmann::json& payload)
{
  float scale = payload.value("scale", 1.0);
  scale = std::max(0.5f, std::min(3.0f, scale));

  DGG_Log("HandleSetUIScale: requesting scale=" + std::to_string(scale));

  const int w = static_cast<int>(PLUG_WIDTH * scale);
  const int h = static_cast<int>(PLUG_HEIGHT * scale);

  // Only request the window resize; do not call Resize(), which would also set the WebView bounds immediately. If the
  // host does not change the window (same size), no OnParentWindowResize() follows and the WebView would keep the
  // requested size while the real client area is smaller. The WebView bounds are instead synced from the actual
  // client rect in OnParentWindowResize() (UpdateWebViewBoundsForHostWindow()). The flag is set only when a resize is
  // really requested, so it cannot leak into the next host-initiated resize.
  const bool requestingNewSize = (w != GetEditorWidth()) || (h != GetEditorHeight());
  if (requestingNewSize)
    mAwaitingSelfInitiatedResize = true;

  EditorResizeFromUI(w, h, true);
}

void DrumGroover::HandleSetPolyphony(const nlohmann::json& payload)
{
  const std::string family = payload.value("family", "");
  const int value = payload.value("value", 1);

  if (family == "kick")       { ForEachKickPool([value](const dgg::InstrumentDef&, auto& pool) { pool.SetPolyphony(value); }); }
  else if (family == "snare")
  {
    ForEachSnarePool([value](const dgg::InstrumentDef&, auto& pool) { pool.SetPolyphony(value); });
  }
  else if (family == "tom")   { for (auto& pool : mTomPools) pool.SetPolyphony(value); }
  else if (family == "ride")  { for (auto& p : mRidePools) p.SetPolyphony(value); }
  else DGG_Log("HandleSetPolyphony: unknown family='" + family + "'");

  DGG_Log("HandleSetPolyphony: family='" + family + "' value=" + std::to_string(value));
}

void DrumGroover::HandleSetEngineMode(const nlohmann::json& payload)
{
  const std::string engine = payload.value("engine", "internal");
  const int mode = (engine == "lofi") ? static_cast<int>(EngineMode::LoFi) : static_cast<int>(EngineMode::Internal);
  mEngineMode.store(mode, std::memory_order_relaxed);

  DGG_Log("HandleSetEngineMode: engine='" + engine + "' (global, whole kit)");

  {
    const bool isLoFi = mode == static_cast<int>(EngineMode::LoFi);
    nlohmann::json articulationGains = nlohmann::json::object();
    for (int note = 0; note < 128; ++note)
    {
      const double gain = isLoFi
          ? mArticulationGainLoFi[note].load(std::memory_order_relaxed)
          : mArticulationGain[note].load(std::memory_order_relaxed);
      articulationGains[std::to_string(note)] = gain;
    }
    SendBridgeMessage("articulationLevelState", articulationGains);
  }

  // The new engine starts with its own default mixer; unsaved changes of the old one are gone (the UI asks before it switches).
  ApplyDefaultMixerForEngine(mode);
  SendBridgeMessage("mixerState", BuildMixerStateJson());
  SendMixerPresetList();
  SendBridgeMessage("mixerPresetLoaded", { {"name", ""} });
}

void DrumGroover::HandleSetMuted(const nlohmann::json& payload)
{
  const bool muted = payload.value("muted", false);
  mUserMuted.store(muted, std::memory_order_relaxed);

  DGG_Log("HandleSetMuted: muted=" + std::string(muted ? "true" : "false"));
}

void DrumGroover::HandleSetDevTools(const nlohmann::json& payload)
{
  const bool enabled = payload.value("enabled", false);
#ifdef OS_WIN
  DggSetWebViewDevTools(this, enabled);
#else
  SetEnableDevTools(enabled);
#endif
}


static int MixerChannelIndexFromId(const std::string& id)
{
  for (int i = 0; i < dgg::kMixerNumChannels; ++i)
    if (id == kMixerChannelNames[i]) return i;
  return -1;
}

static int FxTypeFromName(const std::string& name)
{
  if (name == "eq")     return static_cast<int>(dgg::FxType::Eq);
  if (name == "comp")   return static_cast<int>(dgg::FxType::Comp);
  if (name == "sat")    return static_cast<int>(dgg::FxType::Sat);
  if (name == "ts")     return static_cast<int>(dgg::FxType::Ts);
  if (name == "lim")    return static_cast<int>(dgg::FxType::Lim);
  if (name == "reverb") return static_cast<int>(dgg::FxType::Reverb);
  if (name == "delay")  return static_cast<int>(dgg::FxType::Delay);
  return -1;
}

static const char* FxTypeName(int type)
{
  switch (type)
  {
    case 0: return "eq";     case 1: return "comp";   case 2: return "sat";
    case 3: return "ts";     case 4: return "lim";    case 5: return "reverb";
    case 6: return "delay";  default: return "";
  }
}

static void ParseMixerSlot(const nlohmann::json& j, dgg::FxSlotState& out)
{
  if (!j.is_object()) return;
  out.type = FxTypeFromName(j.value("type", ""));
  out.on = j.value("on", true);
  if (j.contains("params") && j["params"].is_array())
    for (size_t i = 0; i < j["params"].size() && i < dgg::kFxMaxParams; ++i)
      if (j["params"][i].is_number()) out.p[i] = j["params"][i].get<double>();
}

static void ParseMixerChannel(const nlohmann::json& j, dgg::ChannelState& c)
{
  if (!j.is_object()) return;
  c.gainDb = std::max(-60.0, std::min(18.0, j.value("gainDb", c.gainDb)));
  c.pan = std::max(-1.0, std::min(1.0, j.value("pan", c.pan)));
  c.mute = j.value("mute", false);
  c.solo = j.value("solo", false);
  c.sendReverb = std::max(0.0, std::min(1.0, j.value("sendReverb", c.sendReverb)));
  c.sendDelay = std::max(0.0, std::min(1.0, j.value("sendDelay", c.sendDelay)));
  c.autoTargetDb = std::max(-60.0, std::min(18.0, j.value("autoTargetDb", c.autoTargetDb)));
  if (j.contains("fx") && j["fx"].is_array())
    for (size_t i = 0; i < j["fx"].size() && i < 4; ++i) ParseMixerSlot(j["fx"][i], c.fx[i]);
}

static void ParseMixerBus(const nlohmann::json& j, dgg::BusState& b)
{
  if (!j.is_object()) return;
  b.gainDb = std::max(-60.0, std::min(18.0, j.value("gainDb", b.gainDb)));
  b.pan = std::max(-1.0, std::min(1.0, j.value("pan", b.pan)));
  b.mute = j.value("mute", false);
  if (j.contains("fx") && j["fx"].is_array() && j["fx"].size() > 0) ParseMixerSlot(j["fx"][0], b.fx[0]);
}

void DrumGroover::HandleMixerState(const nlohmann::json& payload)
{
  // Defaults first, then overlay whatever the payload provides — partial
  // payloads keep the missing parts at sane values instead of zeroing them.
  dgg::MixerState st = dgg::MixerState{};
  st.kitGainDb = std::max(-24.0, std::min(24.0, payload.value("kitGainDb", 0.0)));
  st.kitAutoTargetDb = std::max(-60.0, std::min(18.0, payload.value("kitAutoTargetDb", 0.0)));

  if (payload.contains("channels") && payload["channels"].is_object())
    for (auto& [id, j] : payload["channels"].items())
    {
      const int idx = MixerChannelIndexFromId(id);
      if (idx >= 0) ParseMixerChannel(j, st.channels[idx]);
    }
  if (payload.contains("master") && payload["master"].is_object())
    ParseMixerChannel(payload["master"], st.master);
  if (payload.contains("reverbBus") && payload["reverbBus"].is_object())
    ParseMixerBus(payload["reverbBus"], st.reverbBus);
  if (payload.contains("delayBus") && payload["delayBus"].is_object())
    ParseMixerBus(payload["delayBus"], st.delayBus);

  {
    std::lock_guard<std::mutex> lk(mMixerMutex);
    mMixerState = st;
  }
  DGG_Log("HandleMixerState: applied (" + std::to_string(payload.dump().size()) + " bytes)");
}

nlohmann::json DrumGroover::BuildMixerStateJson() const
{
  std::lock_guard<std::mutex> lk(mMixerMutex);

  auto slotToJson = [](const dgg::FxSlotState& s) -> nlohmann::json {
    if (s.type < 0) return nullptr; // empty slot
    nlohmann::json j;
    j["type"] = FxTypeName(s.type);
    j["on"] = s.on;
    j["params"] = std::vector<double>(s.p, s.p + dgg::kFxMaxParams);
    return j;
  };

  auto chanToJson = [&](const dgg::ChannelState& c) {
    nlohmann::json j;
    j["gainDb"] = c.gainDb;
    j["pan"] = c.pan;
    j["mute"] = c.mute;
    j["solo"] = c.solo;
    j["sendReverb"] = c.sendReverb;
    j["sendDelay"] = c.sendDelay;
    j["autoTargetDb"] = c.autoTargetDb;
    nlohmann::json fx = nlohmann::json::array();
    for (int i = 0; i < 4; ++i) fx.push_back(slotToJson(c.fx[i]));
    j["fx"] = fx;
    return j;
  };

  auto busToJson = [&](const dgg::BusState& b) {
    nlohmann::json j;
    j["gainDb"] = b.gainDb;
    j["pan"] = b.pan;
    j["mute"] = b.mute;
    nlohmann::json fx = nlohmann::json::array();
    fx.push_back(slotToJson(b.fx[0]));
    j["fx"] = fx;
    return j;
  };

  nlohmann::json out;
  out["kitGainDb"] = mMixerState.kitGainDb;
  out["kitAutoTargetDb"] = mMixerState.kitAutoTargetDb;
  nlohmann::json chans = nlohmann::json::object();
  for (int i = 0; i < dgg::kMixerNumChannels; ++i)
    chans[kMixerChannelNames[i]] = chanToJson(mMixerState.channels[i]);
  out["channels"] = chans;
  out["master"] = chanToJson(mMixerState.master);
  out["reverbBus"] = busToJson(mMixerState.reverbBus);
  out["delayBus"] = busToJson(mMixerState.delayBus);
  return out;
}

void DrumGroover::ApplyDefaultMixerForEngine(int engineMode)
{
  dgg::MixerState st;
  st.ApplyDefaults();   // per-engine defaults go below
  if (engineMode == static_cast<int>(EngineMode::Internal))   // 808: kit gain and the channel faders (dB)
  {
    st.kitGainDb = 0.0;
    auto fader = [&st](dgg::MixerChannel c, double db) { st.channels[static_cast<int>(c)].gainDb = db; };
    fader(dgg::MixerChannel::Kick, 8.0);
    fader(dgg::MixerChannel::Snare, 9.0);
    fader(dgg::MixerChannel::HiHat, -2.0);
    fader(dgg::MixerChannel::RackTom1, -2.0);
    fader(dgg::MixerChannel::RackTom2, 2.0);
    fader(dgg::MixerChannel::RackTom3, 1.0);
    fader(dgg::MixerChannel::FloorTom1, 3.0);
    fader(dgg::MixerChannel::FloorTom2, -3.0);
    fader(dgg::MixerChannel::Cymbal1, 9.0);
    fader(dgg::MixerChannel::Cymbal2, 13.0);
    fader(dgg::MixerChannel::Cymbal3, 12.0);
    fader(dgg::MixerChannel::Cymbal4, 11.0);
    fader(dgg::MixerChannel::Cymbal5, 10.0);
    fader(dgg::MixerChannel::Ride, 15.0);
    fader(dgg::MixerChannel::Custom, -7.6);
    // Auto gain target peaks (fader values relative to the kick, source: Valgaray): Kick 0, Snare -2.5, all others -10.
    // Hand Clap shares the Custom channel with Cowbell, Claves, Rim Shot, Maracas and PC-2, so Custom gets -10.
    for (int i = 0; i < static_cast<int>(dgg::MixerChannel::Count); ++i) st.channels[i].autoTargetDb = -10.0;
    st.channels[static_cast<int>(dgg::MixerChannel::Kick)].autoTargetDb = 0.0;
    st.channels[static_cast<int>(dgg::MixerChannel::Snare)].autoTargetDb = -2.5;
    // Stereo placement from the drummer's seat (right-handed kit), at most 50 % to either side (-1 = left, +1 = right).
    auto pan = [&st](dgg::MixerChannel c, double p) { st.channels[static_cast<int>(c)].pan = p; };
    pan(dgg::MixerChannel::HiHat, -0.35);
    pan(dgg::MixerChannel::RackTom1, -0.30);
    pan(dgg::MixerChannel::RackTom2, -0.10);
    pan(dgg::MixerChannel::RackTom3, 0.10);
    pan(dgg::MixerChannel::FloorTom1, 0.30);
    pan(dgg::MixerChannel::FloorTom2, 0.50);
    pan(dgg::MixerChannel::Cymbal1, -0.45);
    pan(dgg::MixerChannel::Cymbal2, -0.225);
    pan(dgg::MixerChannel::Cymbal3, 0.0);
    pan(dgg::MixerChannel::Cymbal4, 0.225);
    pan(dgg::MixerChannel::Cymbal5, 0.45);
    pan(dgg::MixerChannel::Ride, 0.35);
  }
  std::lock_guard<std::mutex> lk(mMixerMutex);
  mMixerState = st;
}

void DrumGroover::SendMixerPresetList()
{
  const std::string engine = MixerPresetEngineName(mEngineMode.load(std::memory_order_relaxed));
  std::vector<std::string> names;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(GetMixerPresetDir(engine), ec))
  {
    if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
    names.push_back(entry.path().stem().string());
  }
  std::sort(names.begin(), names.end());
  SendBridgeMessage("mixerPresetList", { {"engine", engine}, {"presets", names} });
}

void DrumGroover::HandleMixerPresetList(const nlohmann::json&) { SendMixerPresetList(); }

void DrumGroover::HandleMixerPresetSave(const nlohmann::json& payload)
{
  const std::string name = SanitizeMixerPresetName(payload.value("name", ""));
  if (name.empty()) return;
  const std::string engine = MixerPresetEngineName(mEngineMode.load(std::memory_order_relaxed));
  nlohmann::json file;
  file["name"] = name;
  file["mixer"] = BuildMixerStateJson();
  {
    std::ofstream out(MixerPresetFile(engine, name), std::ios::trunc);
    if (!out) { DGG_Log("HandleMixerPresetSave: cannot write '" + name + "'"); return; }
    out << file.dump(1);
  }
  SendMixerPresetList();
  SendBridgeMessage("mixerPresetLoaded", { {"name", name} });
}

void DrumGroover::HandleMixerPresetLoad(const nlohmann::json& payload)
{
  const std::string name = SanitizeMixerPresetName(payload.value("name", ""));
  if (name.empty()) return;
  const std::string engine = MixerPresetEngineName(mEngineMode.load(std::memory_order_relaxed));
  std::ifstream in(MixerPresetFile(engine, name));
  if (!in) { DGG_Log("HandleMixerPresetLoad: no preset '" + name + "'"); SendMixerPresetList(); return; }
  const nlohmann::json file = nlohmann::json::parse(in, nullptr, false);
  if (file.is_discarded() || !file.contains("mixer") || !file["mixer"].is_object()) { DGG_Log("HandleMixerPresetLoad: unreadable preset '" + name + "'"); return; }
  HandleMixerState(file["mixer"]);
  SendBridgeMessage("mixerState", BuildMixerStateJson());
  SendBridgeMessage("mixerPresetLoaded", { {"name", name} });
}

void DrumGroover::HandleMixerPresetDelete(const nlohmann::json& payload)
{
  const std::string name = SanitizeMixerPresetName(payload.value("name", ""));
  if (name.empty()) return;
  const std::string engine = MixerPresetEngineName(mEngineMode.load(std::memory_order_relaxed));
  std::error_code ec;
  std::filesystem::remove(MixerPresetFile(engine, name), ec);
  SendMixerPresetList();
}

void DrumGroover::HandleMixerPresetRename(const nlohmann::json& payload)
{
  const std::string name = SanitizeMixerPresetName(payload.value("name", ""));
  const std::string newName = SanitizeMixerPresetName(payload.value("newName", ""));
  if (name.empty() || newName.empty() || name == newName) return;
  const std::string engine = MixerPresetEngineName(mEngineMode.load(std::memory_order_relaxed));
  const auto from = MixerPresetFile(engine, name), to = MixerPresetFile(engine, newName);
  std::error_code ec;
  if (!std::filesystem::exists(from, ec) || std::filesystem::exists(to, ec)) { SendMixerPresetList(); return; }
  nlohmann::json file;
  { std::ifstream in(from); file = nlohmann::json::parse(in, nullptr, false); }
  if (file.is_discarded() || !file.is_object()) return;
  file["name"] = newName;
  { std::ofstream out(to, std::ios::trunc); if (!out) return; out << file.dump(1); }
  std::filesystem::remove(from, ec);
  SendMixerPresetList();
  SendBridgeMessage("mixerPresetRenamed", { {"name", name}, {"newName", newName} });
}

void DrumGroover::HandleMixerPresetDefault(const nlohmann::json&)
{
  ApplyDefaultMixerForEngine(mEngineMode.load(std::memory_order_relaxed));
  SendBridgeMessage("mixerState", BuildMixerStateJson());
  SendBridgeMessage("mixerPresetLoaded", { {"name", ""} });
}

void DrumGroover::HandleSetVoiceParam(const nlohmann::json& payload)
{
  const std::string family = payload.value("family", "");
  const std::string param  = payload.value("param", "");
  const double value       = payload.value("value", 0.0);
  const int index          = payload.value("index", -1);

  DGG_Log("HandleSetVoiceParam: family='" + family + "' param='" + param + "' index=" + std::to_string(index) + " value=" + std::to_string(value));

  mVoiceParamsConfigured.store(true, std::memory_order_relaxed);

  if (family == "kick")
  {
    const int target = dgg::FindInstrument("kick", index) ? index : 0;   // an index outside the instances is the first one
    ForEachKickPool([&](const dgg::InstrumentDef& d, auto& pool) {
      if (d.paramIndex != target) return;
      for (auto& v : pool.Voices()) SetParamOn(v, param, value);
    });
  }
  else if (family == "snare")
  {
    const int target = dgg::FindInstrument("snare", index) ? index : 0;   // an index outside the instances (-1: family-wide send) is the first one
    ForEachSnarePool([&](const dgg::InstrumentDef& d, auto& pool) {
      if (d.paramIndex != target) return;
      for (auto& v : pool.Voices()) SetParamOn(v, param, value);
    });
  }
  else if (family == "hihat")
  {
    const int target = dgg::FindInstrument("hihat", index) ? index : 0;   // an index outside the instances is the first one
    ForEachHiHatPool([&](const dgg::InstrumentDef& d, auto& pool) {
      if (d.paramIndex != target) return;
      for (auto& v : pool.Voices()) SetParamOn(v, param, value);
    });
  }
  else if (family == "tom")
  {
    ForEachTomPool([&](const dgg::InstrumentDef& d, auto& pool) {
      if (index >= 0 && index != d.paramIndex) return;
      for (auto& v : pool.Voices()) SetParamOn(v, param, value);
    });
  }
  else if (family == "custom")
  {
    for (int k = 0; k < dgg::kNumCustomInstruments; k++)
    {
      if (index >= 0 && index != k) continue;
      if (dgg::CustomInstrument(k).kind == dgg::EngineKind::Tom) { for (auto& v : mCustomPools[k].Voices()) SetTomVoiceParam(v, param, value); }
      else if (IsHcKind(dgg::CustomInstrument(k).kind)) { for (auto& v : mCustomHcPools[k].Voices()) SetHcVoiceParam(v, param, value); }
      else if (IsCbKind(dgg::CustomInstrument(k).kind)) { for (auto& v : mCustomCbPools[k].Voices()) SetCbVoiceParam(v, param, value); }
      else if (IsPc2Kind(dgg::CustomInstrument(k).kind)) { for (auto& v : mCustomPc2Pools[k].Voices()) SetPc2VoiceParam(v, param, value); }
      else { for (auto& v : mCustomRmPools[k].Voices()) SetRmVoiceParam(v, param, value); }
    }
  }
  else if (family == "cymbal")
  {
    const int idx = (index >= 0 && index < (int)mCymbalPools.size()) ? index : 0;
    for (auto& v : mCymbalPools[idx].Voices()) SetRideVoiceParam(v, param, value);
  }
  else if (family == "ride")
  {
    const int idx = (index >= 0 && index < (int)mRidePools.size()) ? index : 0;
    for (auto& v : mRidePools[idx].Voices()) SetRideVoiceParam(v, param, value);
  }
  else
  {
    DGG_Log("HandleSetVoiceParam: unknown family='" + family + "'");
  }
}

// ---------------------------------------------------------------------
// Render-cache variant pool: lazy background baking (dsp/808VariantPoolCache.h, dsp/808VariantPoolBaker.h).
// A bake job renders variants of one (instrument, parameter-key) combination on a throwaway voice that is never
// reset between triggers, so the free-running oscillator bank lands at a different phase for each variant.
// ---------------------------------------------------------------------

void DrumGroover::HandleCommitVoiceParam(const nlohmann::json& payload)
{
  const std::string family = payload.value("family", "");
  const int index = payload.value("index", -1);
  const bool rebake = ReadRebakeOnChange();

  if (family == "ride")
  {
    const int idx = (index >= 0 && index < dgg::CountInstruments("ride")) ? index : 0;
    EnqueueRideVariantBake(idx, rebake);
  }
  else if (family == "hihat")
  {
    const int idx = (index >= 0 && index < dgg::CountInstruments("hihat")) ? index : 0;
    EnqueueHiHatVariantBake(idx, rebake);
  }
  else if (family == "cymbal")
  {
    const int idx = (index >= 0 && index < dgg::CountInstruments("cymbal")) ? index : 0;
    EnqueueCrashVariantBake(idx, rebake);
  }
}

void DrumGroover::HandleSetCacheVariantCount(const nlohmann::json& payload)
{
  const int value = std::clamp(payload.value("value", 7), 1, 20);
  mCacheVariantCount.store(value, std::memory_order_relaxed);
  dgg::TR808RideVariantPool::Instance().SetTargetVariantCount(value);
  dgg::TR808HiHatVariantPool::Instance().SetTargetVariantCount(value);
  SendBridgeMessage("cacheVariantCount", { {"value", value} });
}

// "Clear Cache" (Cache panel) has two separately-confirmed actions, chosen
// by payload.tier:
//   "cache"   -- deletes every baked set that is not a factory-default set
//                (per default.index), drops the in-memory pools and
//                re-enqueues every pool at its current knob values.
//   "default" -- deletes the factory-default sets and default.index, then
//                bakes them again from the JS default tables.
void DrumGroover::HandleClearCache(const nlohmann::json& payload)
{
  const std::string tier = payload.value("tier", "cache");
  const auto defaultNames = ReadDefaultIndex();
  const auto dir = GetCacheDir();

  // Deletes every set whose "is a default set" status equals wantDefault;
  // returns the bytes freed.
  auto deleteSets = [&](bool wantDefault) -> uintmax_t {
    std::vector<std::filesystem::path> victims;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
    {
      if (ec || !entry.is_regular_file()) continue;
      const auto& path = entry.path();
      if (path.extension() != ".f32") continue;
      const std::string stem = path.stem().string();
      const size_t vPos = stem.rfind("_v");
      if (vPos == std::string::npos) continue;
      if ((defaultNames.count(stem.substr(0, vPos)) > 0) == wantDefault) victims.push_back(path);
    }
    uintmax_t freed = 0;
    for (const auto& p : victims)
    {
      std::error_code sizeEc, removeEc;
      const auto size = std::filesystem::file_size(p, sizeEc);
      std::filesystem::remove(p, removeEc);
      if (!sizeEc && !removeEc) freed += size;
    }
    return freed;
  };

  if (tier == "cache")
  {
    // The in-memory pools don't record which resident variant belongs to a
    // default set, so they are dropped as a whole and refilled by the
    // re-enqueue below (default sets come straight back from disk).
    dgg::TR808RideVariantPool::Instance().ClearForTest();
    dgg::TR808HiHatVariantPool::Instance().ClearForTest();
    const uintmax_t freed = deleteSets(false);

    for (int i = 0; i < dgg::CountInstruments("ride"); i++)  EnqueueRideVariantBake(i);
    for (int i = 0; i < dgg::CountInstruments("hihat"); i++)  EnqueueHiHatVariantBake(i);
    for (int i = 0; i < dgg::CountInstruments("cymbal"); i++) EnqueueCrashVariantBake(i);

    DGG_Log("HandleClearCache[cache]: freed " + std::to_string(freed) + " bytes");
    SendBridgeMessage("cacheCleared", { {"tier", "cache"}, {"bytesFreed", (double)freed} });
  }
  else if (tier == "default")
  {
    // Resident variants stay: they remain valid audio, and dropping them
    // would also throw away non-default sets that are still on disk.
    const uintmax_t freed = deleteSets(true);
    std::error_code ec;
    std::filesystem::remove(dir / "default.index", ec);
    ApplyFactoryDefaults();

    DGG_Log("HandleClearCache[default]: freed " + std::to_string(freed) + " bytes, rebaking defaults");
    SendBridgeMessage("cacheCleared", { {"tier", "default"}, {"bytesFreed", (double)freed} });
  }
}

namespace {
// A variant file counts as present only if LoadVariantFromDisk() would
// accept its size (files are written atomically, see SaveVariantToDisk()).
bool VariantFileValid(const std::filesystem::path& path)
{
  std::error_code ec;
  const auto sizeBytes = std::filesystem::file_size(path, ec);
  return !ec && sizeBytes > 0 && (sizeBytes % sizeof(float)) == 0;
}

// Variant slots k in 1..target of set `baseName` that have no valid file yet.
std::vector<int> MissingVariantSlots(const std::string& baseName, int target)
{
  std::vector<int> missing;
  for (int k = 1; k <= target; k++)
    if (!VariantFileValid(VariantFilePath(baseName, k))) missing.push_back(k);
  return missing;
}

// Loads the set's existing files (v1..vTarget) into `pool`, but only for a
// key with nothing resident yet: whatever is already in memory came from
// an earlier load or from this process's own bakes, and loading again
// would duplicate it.
std::mutex gPoolLoadMutex;

template <typename Pool, typename Key>
void LoadExistingVariantsIntoPool(Pool& pool, const Key& key, const std::string& baseName)
{
  std::lock_guard<std::mutex> lock(gPoolLoadMutex);
  if (pool.GetStatus(key).ready > 0) return;
  const int target = pool.TargetVariantCount();
  for (int k = 1; k <= target; k++)
  {
    const auto path = VariantFilePath(baseName, k);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) continue;
    std::vector<float> buf;
    if (LoadVariantFromDisk(path, buf)) pool.AddVariant(key, std::move(buf));
  }
}

// The size cap itself is dgg::EvictCacheIfOverBudget() in 808VariantPoolCache.h; this wrapper supplies the cache
// folder and the protected default sets.
std::mutex gCacheEvictionMutex;

void EvictCacheIfOverBudget()
{
  std::lock_guard<std::mutex> lock(gCacheEvictionMutex);
  const auto result = dgg::EvictCacheIfOverBudget({ GetCacheDir() }, CacheMaxBytes(),ReadDefaultIndex());
  if (result.groupsEvicted > 0)
    DGG_Log("EvictCacheIfOverBudget: evicted " + std::to_string(result.groupsEvicted) + " set(s), "
      + std::to_string(result.bytesEvicted) + " bytes, " + std::to_string(result.bytesRemaining) + " bytes remaining");
}

struct CacheUsage { uintmax_t bytes = 0; int files = 0; };

CacheUsage MeasureCache()
{
  CacheUsage u;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(GetCacheDir(), ec))
  {
    if (ec || !entry.is_regular_file() || entry.path().extension() != ".f32") continue;
    std::error_code sizeEc;
    const auto size = entry.file_size(sizeEc);
    if (sizeEc) continue;
    u.bytes += size;
    u.files++;
  }
  return u;
}

// Guards against two callers baking the same variant slot concurrently.
// Keyed by "<set base name>#<slot>"; the value says whether the slot's job
// has started rendering (queued slots can be released by a cancel, running
// ones finish). Other slots of the same set are not affected.
std::mutex gInFlightBakeMutex;
std::unordered_map<std::string, bool> gInFlightSlots;

std::string SlotId(const std::string& baseName, int k) { return baseName + "#" + std::to_string(k); }

bool TryMarkSlotInFlight(const std::string& baseName, int k)
{
  std::lock_guard<std::mutex> lock(gInFlightBakeMutex);
  return gInFlightSlots.emplace(SlotId(baseName, k), false).second;
}

void MarkSlotStarted(const std::string& baseName, int k)
{
  std::lock_guard<std::mutex> lock(gInFlightBakeMutex);
  auto it = gInFlightSlots.find(SlotId(baseName, k));
  if (it != gInFlightSlots.end()) it->second = true;
}

void UnmarkSlotInFlight(const std::string& baseName, int k)
{
  std::lock_guard<std::mutex> lock(gInFlightBakeMutex);
  gInFlightSlots.erase(SlotId(baseName, k));
}

void ReleaseQueuedSlots()
{
  std::lock_guard<std::mutex> lock(gInFlightBakeMutex);
  for (auto it = gInFlightSlots.begin(); it != gInFlightSlots.end();)
    it = it->second ? std::next(it) : gInFlightSlots.erase(it);
}

// Bakes whichever variants (1..target) of set `baseName` have no file yet. Each missing variant is its own parallel
// job that renders on a fresh throwaway voice via render(warmupSec), with warmup (k-1)*kBakeWarmupStepSec. Phased:
// only the first immediateVariants (default 2, at least as many as idle baker threads) are queued at once so the set
// becomes playable quickly; the rest chain on when those have finished.
// loadIfComplete: also load the set's files into the pool when nothing is missing (a set that is only on disk
// would miss and stay silent). rebakeAll: bake all variants again with a random extra warmup and replace the set.
template <typename Pool>
void EnqueueMissingVariants(const char* label, Pool& pool, const dgg::TR808VariantKey128& key, const std::string& baseName,
                            int target, bool loadIfComplete, std::function<std::vector<float>(double)> render,
                            bool rebakeAll = false, int immediateVariants = 2)
{
  pool.SetTargetVariantCount(target);
  std::vector<int> missing;
  std::random_device rd;
  const double saltSec = std::uniform_real_distribution<double>(0.0, kBakeSaltMaxSec)(rd);
  if (rebakeAll)
  {
    for (int k = 1; k <= target; k++) missing.push_back(k);
    pool.ClearKey(key);
  }
  else
  {
    missing = MissingVariantSlots(baseName, target);
    if (missing.empty() && !loadIfComplete) return;
    LoadExistingVariantsIntoPool(pool, key, baseName);
  }
  // Slots another job is already baking are left to that job.
  missing.erase(std::remove_if(missing.begin(), missing.end(),
    [&](int k) { return !TryMarkSlotInFlight(baseName, k); }), missing.end());
  if (missing.empty()) return;

  DGG_Log("EnqueueVariantBake: " + baseName + " missing=" + std::to_string(missing.size()) + "/" + std::to_string(target));


  const int total = (int)missing.size();
  dgg::TR808VariantPoolBaker::Instance().PlanUnits(total);
  const int immediate = std::max(1, std::min(total, std::max(immediateVariants, dgg::TR808VariantPoolBaker::Instance().FreeThreads())));
  auto remainingImmediate = std::make_shared<std::atomic<int>>(immediate);
  Pool* poolPtr = &pool;
  const std::string labelStr = label;
  const uint64_t cancelGen = dgg::TR808VariantPoolBaker::Instance().CancelGeneration();

  auto bakeSlot = [=](int slot) {
    return [=](const dgg::TR808VariantPoolBaker::ProgressFn& report) {
      const int k = missing[slot];
      MarkSlotStarted(baseName, k);
      const double warmupSec = (k - 1) * kBakeWarmupStepSec + saltSec;
      const auto t0 = std::chrono::steady_clock::now();
      std::vector<float> tap = render(warmupSec);
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      dgg::TR808VariantPoolBaker::Instance().RecordBake(ms);
      const auto path = VariantFilePath(baseName, k);
      if ((rebakeAll || !VariantFileValid(path)) && !tap.empty())
      {
        if (SaveVariantToDisk(path, tap)) poolPtr->AddVariant(key, std::vector<float>(tap));
        EvictCacheIfOverBudget();
      }
      DGG_Log("BakeTiming: " + baseName + " v" + std::to_string(k) + " took " + std::to_string(ms) + "ms ("
        + std::to_string(tap.size()) + " samples, warmup=" + std::to_string(warmupSec) + "s)");
      report(1, 1);
      dgg::TR808VariantPoolBaker::Instance().FinishUnits(1);
      UnmarkSlotInFlight(baseName, k);
    };
  };

  for (int s = 0; s < immediate; s++)
  {
    auto job = bakeSlot(s);
    dgg::TR808VariantPoolBaker::Instance().Enqueue(labelStr,
      [=](const dgg::TR808VariantPoolBaker::ProgressFn& report) {
        job(report);
        if (remainingImmediate->fetch_sub(1) == 1 && dgg::TR808VariantPoolBaker::Instance().CancelGeneration() == cancelGen)
          for (int j = immediate; j < total; j++)
            dgg::TR808VariantPoolBaker::Instance().Enqueue(labelStr, bakeSlot(j));
      });
  }
}

// Ride and Cymbal share TR808RideVoice (Crash/Mute are articulations of
// it) and therefore one key space and one pool.
void EnqueueRideSet(const char* label, const dgg::TR808VariantKey128& key, int target, bool loadIfComplete, bool rebakeAll = false)
{
  if (gCacheOffRide.load(std::memory_order_relaxed)) return;
  const std::string baseName = dgg::TR808RideVoice::VariantPoolBaseName(key);
  auto render = [key](double warmupSec) {
    const auto p = dgg::TR808RideVoice::DecodeVariantPoolKey(key);
    dgg::TR808RideVoice bakeVoice;
    bakeVoice.SetSampleRate(p.sampleRateHz);
    bakeVoice.WarmupFreeRunningOscillatorsForBake(warmupSec);
    bakeVoice.decayKnob01 = p.decay; bakeVoice.toneKnob = p.tone; bakeVoice.levelKnob = p.level;
    bakeVoice.pitchRatio = p.pitch;
    bakeVoice.diagUseVariantPool = false;
    std::vector<float> tap;
    std::vector<double> scratch(4096);
    bakeVoice.tapVariantBake = &tap;
    bakeVoice.Trigger(1.0, static_cast<dgg::TR808RideVoice::Articulation>(p.art));
    while (bakeVoice.IsActive())
    {
      std::fill(scratch.begin(), scratch.end(), 0.0);
      bakeVoice.Process(scratch.data(), (int)scratch.size());
    }
    bakeVoice.tapVariantBake = nullptr;
    return tap;
  };
  EnqueueMissingVariants(label, dgg::TR808RideVariantPool::Instance(), key, baseName, target, loadIfComplete, render, rebakeAll);
}

void EnqueueHiHatSet(const dgg::TR808VariantKey128& key, int target, bool loadIfComplete, bool rebakeAll = false)
{
  if (gCacheOffHiHat.load(std::memory_order_relaxed)) return;
  const std::string baseName = dgg::HiHat808Voice::VariantPoolBaseName(key);
  auto render = [key](double warmupSec) {
    const auto p = dgg::HiHat808Voice::DecodeVariantPoolKey(key);
    dgg::HiHat808Voice bakeVoice;
    bakeVoice.circuitFamily = dgg::HiHatPoolFamily(p.poolIndex);
    bakeVoice.SetSampleRate(p.sampleRateHz);
    bakeVoice.WarmupFreeRunningOscillatorsForBake(warmupSec);
    bakeVoice.decayKnob01 = p.decay; bakeVoice.levelKnob01 = p.level;
    bakeVoice.pitchRatio = p.pitch;
    bakeVoice.diagUseVariantPool = false;
    std::vector<float> tap;
    std::vector<double> scratch(4096);
    bakeVoice.tapVariantBake = &tap;
    bakeVoice.Trigger(1.0);
    while (bakeVoice.IsActive())
    {
      std::fill(scratch.begin(), scratch.end(), 0.0);
      bakeVoice.Process(scratch.data(), (int)scratch.size());
    }
    bakeVoice.tapVariantBake = nullptr;
    return tap;
  };
  EnqueueMissingVariants("hihat", dgg::TR808HiHatVariantPool::Instance(), key, baseName, target, loadIfComplete, render, rebakeAll);
}

// The factory-default entries of the instrument table's Ride, HiHat and Cymbal rows, in bake order (ride, hi-hat, cymbal).
std::vector<dgg::FactoryDefaultEntry> FactoryDefaultEntries()
{
  std::vector<dgg::FactoryDefaultEntry> out;
  for (const dgg::EngineKind kind : { dgg::EngineKind::Ride, dgg::EngineKind::HiHat, dgg::EngineKind::Cymbal })
    for (const dgg::InstrumentDef& d : dgg::kInstruments)
      if (d.kind == kind) out.push_back(FactoryEntryOf(d));
  return out;
}

void EnqueueFactoryDefaultSet(const dgg::FactoryDefaultEntry& e, double sr, int target)
{
  const dgg::TR808VariantKey128 key = dgg::FactoryDefaultKey(e, sr);
  // resident from the start: a first hit on a key that is only on disk would miss and stay silent
  if (e.kind == 1) EnqueueHiHatSet(key, target, /*loadIfComplete=*/true);
  else EnqueueRideSet(e.kind == 0 ? "ride" : "cymbal", key, target, /*loadIfComplete=*/true);
}
}  // namespace

// The instrument table's defaults are the initial state of the voices (constructor) and define the factory-default cache sets: a
// voice at its default plays exactly the set named in default.index. Once any voice parameter has been set (a loaded project, a
// knob turn) the voices are left alone and only the default sets are (re)baked.
void DrumGroover::HandleSetFactoryDefaults(const nlohmann::json& /*payload*/)
{
  ApplyFactoryDefaults();
}

// The factory-default sets come from the instrument table, not from whatever the live voices currently hold, so
// they stay correct when the table changes. default.index is rewritten
// to exactly this list; sets that dropped out of it become ordinary,
// evictable cache. Baking runs on a background thread (it also reads
// existing files); the index is written first so the sets are protected
// from the moment they exist.
void DrumGroover::ApplyFactoryDefaults()
{
  const std::vector<dgg::FactoryDefaultEntry> entries = FactoryDefaultEntries();
  const double sr = GetSampleRate();
  const int target = mCacheVariantCount.load(std::memory_order_relaxed);

  std::set<std::string> names;
  for (const auto& e : entries) names.insert(dgg::FactoryDefaultBaseName(e, sr));

  if (names.empty()) return;
  if (!std::filesystem::exists(GetCacheDir() / "default.index")) gFirstRunBake.store(true);
  WriteDefaultIndex(names);
  DGG_Log("ApplyFactoryDefaults: " + std::to_string(names.size()) + " default sets, sr=" + std::to_string(sr));

  std::thread([entries, sr, target]() {
    for (const auto& e : entries) EnqueueFactoryDefaultSet(e, sr, target);
  }).detach();
}

std::vector<dgg::VariantBakeTarget> DrumGroover::CurrentBakeTargets()
{
  const double sr = GetSampleRate();
  std::vector<dgg::VariantBakeTarget> targets;
  std::set<std::string> seen;
  auto add = [&](int kind, const dgg::TR808VariantKey128& key) {
    const std::string name = kind == 1 ? dgg::HiHat808Voice::VariantPoolBaseName(key) : dgg::TR808RideVoice::VariantPoolBaseName(key);
    if (seen.insert(name).second) targets.push_back({ kind, key, name });
  };

  for (const auto& e : FactoryDefaultEntries()) add(e.kind, dgg::FactoryDefaultKey(e, sr));

  for (int i = 0; i < (int)mRidePools.size(); i++)
  {
    auto& voices = mRidePools[i].Voices();
    if (!voices.empty()) add(0, dgg::RideVoiceKey(dgg::RideArticulationForPool(i), voices[0], sr));
  }
  for (int i = 0; i < (int)mHiHatPools.size(); i++)
  {
    auto& voices = mHiHatPools[i].Voices();
    if (!voices.empty()) add(1, dgg::HiHatVoiceKey(i, voices[0], sr));
  }
  for (int i = 0; i < (int)mCymbalPools.size(); i++)
  {
    auto& voices = mCymbalPools[i].Voices();
    if (!voices.empty()) add(2, dgg::RideVoiceKey(dgg::CymbalArticulationForPool(i), voices[0], sr));
  }

  return targets;
}

namespace {
// The bake order the user confirmed in the Cache panel: every set in
// `targets` must have variants 1..value on disk before it counts as done.
// Completion is judged by the files, not by the baker's queue.
struct ApplyJob
{
  bool active = false;
  int value = 0;
  int total = 0;
  int retriesWithoutProgress = 0;
  int remainingAtLastRetry = 0;
  std::vector<dgg::VariantBakeTarget> targets;
  std::chrono::steady_clock::time_point lastPoll{};
};
std::mutex gApplyJobMutex;
ApplyJob gApplyJob;

constexpr int kApplyMaxRetriesWithoutProgress = 3;

int CountMissingFiles(const std::vector<dgg::VariantBakeTarget>& targets, int value)
{
  int missing = 0;
  for (const auto& t : targets) missing += (int)MissingVariantSlots(t.baseName, value).size();
  return missing;
}

void EnqueueTargets(const std::vector<dgg::VariantBakeTarget>& targets, int value)
{
  for (const auto& t : targets)
  {
    switch (t.kind)
    {
      case 1: EnqueueHiHatSet(t.key, value, /*loadIfComplete=*/true); break;
      default: EnqueueRideSet(t.kind == 0 ? "ride" : "cymbal", t.key, value, /*loadIfComplete=*/true); break;
    }
  }
}
}  // namespace

void DrumGroover::HandleQueryCacheBakeEstimate(const nlohmann::json& payload)
{
  const int value = std::clamp(payload.value("value", 7), 1, 20);
  int missing = 0, missingRide = 0, missingHiHat = 0, missingOther = 0;
  for (const auto& t : CurrentBakeTargets())
  {
    const int n = (int)MissingVariantSlots(t.baseName, value).size();
    missing += n;
    (t.kind == 1 ? missingHiHat : t.kind >= 3 ? missingOther : missingRide) += n;
  }

  // Projected cache size after baking: current size plus the missing files. With files already in the cache their average size is the basis.
  // An empty cache has none: a recording is at most about 5 s long (the longest measured cymbal decay is 3.9 s), 4 bytes per sample.
  const CacheUsage usage = MeasureCache();
  const double bytesPerFile = usage.files > 0 ? (double)usage.bytes / usage.files : 5.0 * GetSampleRate() * sizeof(float);
  const double newBytes = missing * bytesPerFile;

  auto& baker = dgg::TR808VariantPoolBaker::Instance();
  SendBridgeMessage("cacheBakeEstimate", {
    {"value", value}, {"missing", missing}, {"avgBakeMs", baker.AverageBakeMs()},
    {"threads", baker.ThreadCount()},
    {"projectedBytes", (double)usage.bytes + newBytes},
    {"limitBytes", (double)CacheMaxBytes()} });
}

void DrumGroover::SendCacheInfo()
{
  const CacheUsage usage = MeasureCache();
  SendBridgeMessage("cacheInfo", {
    {"usedBytes", (double)usage.bytes}, {"files", usage.files}, {"limitGiB", ReadCacheLimitGiB()},
    {"rebakeOnChange", ReadRebakeOnChange()}, {"cacheOffHiHat", gCacheOffHiHat.load(std::memory_order_relaxed)}, {"cacheOffRide", gCacheOffRide.load(std::memory_order_relaxed)} });
}

void DrumGroover::HandleSetRebakeOnChange(const nlohmann::json& payload)
{
  WriteRebakeOnChange(payload.value("on", false));
  SendCacheInfo();
}

// Per instrument: cache on = play the variant pool, report a miss and synthesise it live; cache off = no pool lookup,
// no miss reports, no baking (the Enqueue*Set functions return), every hit is synthesised live.
void DrumGroover::ApplyCacheMode(bool hiHatOff, bool rideOff)
{
  gCacheOffHiHat.store(hiHatOff, std::memory_order_relaxed);
  gCacheOffRide.store(rideOff, std::memory_order_relaxed);
  ForEachHiHatPool([hiHatOff](const dgg::InstrumentDef&, auto& pool) {
    for (auto& v : pool.Voices()) { v.diagUseVariantPool = !hiHatOff; v.cacheOnly = !hiHatOff; }
  });
  for (auto& p : mRidePools)
    for (auto& v : p.Voices()) { v.diagUseVariantPool = !rideOff; v.cacheOnly = !rideOff; }
  for (auto& p : mCymbalPools)
    for (auto& v : p.Voices()) { v.diagUseVariantPool = !rideOff; v.cacheOnly = !rideOff; }
  DGG_Log(std::string("ApplyCacheMode: hihat ") + (hiHatOff ? "live" : "cache") + ", ride/cymbal " + (rideOff ? "live" : "cache"));
}

void DrumGroover::HandleSetCacheDisabled(const nlohmann::json& payload)
{
  const std::string instrument = payload.value("instrument", "");
  const bool disabled = payload.value("disabled", false);
  bool hiHatOff = gCacheOffHiHat.load(std::memory_order_relaxed), rideOff = gCacheOffRide.load(std::memory_order_relaxed);
  if (instrument == "hihat") { hiHatOff = disabled; WriteSettingsField("cacheOffHiHat", disabled); }
  else if (instrument == "ride") { rideOff = disabled; WriteSettingsField("cacheOffRide", disabled); }
  else return;
  ApplyCacheMode(hiHatOff, rideOff);
  if (!disabled)   // back on: load or bake whatever the current settings need
  {
    ApplyFactoryDefaults();
    if (instrument == "ride")
    {
      for (int i = 0; i < (int)mRidePools.size(); ++i) EnqueueRideVariantBake(i);
      for (int i = 0; i < (int)mCymbalPools.size(); ++i) EnqueueCrashVariantBake(i);
    }
    else for (int i = 0; i < (int)mHiHatPools.size(); ++i) EnqueueHiHatVariantBake(i);
  }
  SendCacheInfo();
}

// Stores the machine-wide cache size limit and applies it right away:
// lowering it below the current size evicts the oldest non-default sets.
void DrumGroover::HandleSetCacheLimit(const nlohmann::json& payload)
{
  WriteCacheLimitGiB(payload.value("gib", kDefaultCacheLimitGiB));
  EvictCacheIfOverBudget();
  SendCacheInfo();
}

// Stops the running bake: queued jobs are dropped, jobs already rendering
// finish their file. Files baked so far stay, so applying again resumes
// where this stopped. The variant count in effect was never changed by the
// order, only the pools' target is put back to it.
void DrumGroover::HandleCancelCacheBake(const nlohmann::json&)
{
  {
    std::lock_guard<std::mutex> lock(gApplyJobMutex);
    gApplyJob.active = false;
  }
  dgg::TR808VariantPoolBaker::Instance().CancelPending();
  ReleaseQueuedSlots();
  HandleSetCacheVariantCount({ {"value", mCacheVariantCount.load(std::memory_order_relaxed)} });
  SendBridgeMessage("cacheBakeCancelled", nlohmann::json::object());
}

// Starts baking every file needed for payload.value variants per set. The
// count in effect (mCacheVariantCount) is NOT changed here: PollApplyJob()
// adopts the new value once all its files are verified on disk.
void DrumGroover::HandleApplyCacheVariantCount(const nlohmann::json& payload)
{
  const int value = std::clamp(payload.value("value", 7), 1, 20);
  std::vector<dgg::VariantBakeTarget> targets = CurrentBakeTargets();
  {
    std::lock_guard<std::mutex> lock(gApplyJobMutex);
    gApplyJob = ApplyJob{};
    gApplyJob.active = true;
    gApplyJob.value = value;
    gApplyJob.total = CountMissingFiles(targets, value);
    gApplyJob.remainingAtLastRetry = gApplyJob.total;
    gApplyJob.targets = targets;
  }
  EnqueueTargets(targets, value);
}

// Called from OnIdle. While an applied variant count is being baked, checks
// the files on disk (throttled), reports progress and closes the job only
// once every file of every target set exists. If the baker runs dry with
// files still missing (a failed write, or the cache size cap evicting a set)
// the missing slots are queued again; after several rounds without progress
// the job is reported as failed instead of waiting forever.
void DrumGroover::PollApplyJob()
{
  std::lock_guard<std::mutex> lock(gApplyJobMutex);
  if (!gApplyJob.active) return;
  const auto now = std::chrono::steady_clock::now();
  if (now - gApplyJob.lastPoll < std::chrono::milliseconds(300)) return;
  gApplyJob.lastPoll = now;

  const int remaining = CountMissingFiles(gApplyJob.targets, gApplyJob.value);
  bool failed = false;
  if (remaining > 0 && dgg::TR808VariantPoolBaker::Instance().IsIdle())
  {
    if (remaining < gApplyJob.remainingAtLastRetry) gApplyJob.retriesWithoutProgress = 0;
    else gApplyJob.retriesWithoutProgress++;
    gApplyJob.remainingAtLastRetry = remaining;
    if (gApplyJob.retriesWithoutProgress >= kApplyMaxRetriesWithoutProgress) failed = true;
    else EnqueueTargets(gApplyJob.targets, gApplyJob.value);
  }

  const bool done = remaining == 0 || failed;
  if (done)
  {
    gApplyJob.active = false;
    // Adopt the new count once verified; on failure put the pools' target
    // back to the count still in effect.
    HandleSetCacheVariantCount({ {"value", remaining == 0 ? gApplyJob.value : mCacheVariantCount.load(std::memory_order_relaxed)} });
    DGG_Log("ApplyCacheVariantCount: " + std::string(failed ? "FAILED" : "verified") + ", value="
      + std::to_string(gApplyJob.value) + ", missing=" + std::to_string(remaining));
  }
  SendBridgeMessage("cacheApplyProgress", {
    {"active", !done}, {"verified", remaining == 0}, {"failed", failed},
    {"total", gApplyJob.total}, {"remaining", remaining} });
}

void DrumGroover::EnqueueRideVariantBake(int poolIndex, bool rebakeAll)
{
  if (poolIndex < 0 || poolIndex >= (int)mRidePools.size()) return;
  auto& voices = mRidePools[poolIndex].Voices();
  if (voices.empty()) return;
  EnqueueRideSet("ride", dgg::RideVoiceKey(dgg::RideArticulationForPool(poolIndex), voices[0], GetSampleRate()),
    mCacheVariantCount.load(std::memory_order_relaxed), /*loadIfComplete=*/true, rebakeAll);
}

void DrumGroover::EnqueueHiHatVariantBake(int poolIndex, bool rebakeAll)
{
  if (poolIndex < 0 || poolIndex >= (int)mHiHatPools.size()) return;
  auto& voices = mHiHatPools[poolIndex].Voices();
  if (voices.empty()) return;
  EnqueueHiHatSet(dgg::HiHatVoiceKey(poolIndex, voices[0], GetSampleRate()),
    mCacheVariantCount.load(std::memory_order_relaxed), /*loadIfComplete=*/true, rebakeAll);
}

// Cymbal plays through TR808RideVoice (Crash/Mute articulations, see
// 808RideVoice.h); poolIndex = cymbalPos * 2 + (isMute ? 1 : 0).
void DrumGroover::EnqueueCrashVariantBake(int poolIndex, bool rebakeAll)
{
  if (poolIndex < 0 || poolIndex >= (int)mCymbalPools.size()) return;
  auto& voices = mCymbalPools[poolIndex].Voices();
  if (voices.empty()) return;
  EnqueueRideSet("cymbal", dgg::RideVoiceKey(dgg::CymbalArticulationForPool(poolIndex), voices[0], GetSampleRate()),
    mCacheVariantCount.load(std::memory_order_relaxed), /*loadIfComplete=*/true, rebakeAll);
}

void DrumGroover::DrainCacheMisses()
{
  for (int w = 0; w < 2; ++w)
  {
    uint64_t bits = mMissNoteBits[w].exchange(0, std::memory_order_relaxed);
    while (bits)
    {
      int b = 0;
      while (!((bits >> b) & 1)) ++b;
      bits &= bits - 1;
      for (const auto& target : NoteToVariantPoolTargets(w * 64 + b))
      {
        DGG_Log("CacheMiss: note " + std::to_string(w * 64 + b) + " -> " + target.instrument + " pool " + std::to_string(target.poolIndex));
        if (target.instrument == "ride")   EnqueueRideVariantBake(target.poolIndex);
        if (target.instrument == "hihat")  EnqueueHiHatVariantBake(target.poolIndex);
        if (target.instrument == "cymbal") EnqueueCrashVariantBake(target.poolIndex);
      }
    }
  }

}


// The UI tells which track each custom instrument is assigned to (Add Track dialog): {"assignments":[{"instrument":id,"trackId":..,"gm":..}]}
// (an instrument without an entry is unassigned). A bake of the instrument follows, so its set is resident for the first hit.
namespace { constexpr int kCustomPreviewMidiChannel = 15; }   // MIDI channel 16: reserved for the UI preview of custom instruments

void DrumGroover::HandleSetCustomInstruments(const nlohmann::json& payload)
{
  std::string ids[dgg::kNumCustomInstruments];
  int notes[dgg::kNumCustomInstruments];
  for (int k = 0; k < dgg::kNumCustomInstruments; k++) notes[k] = -1;
  if (payload.contains("assignments") && payload["assignments"].is_array())
    for (const auto& a : payload["assignments"])
    {
      const int k = dgg::CustomInstrumentIndex(a.value("instrument", "").c_str());
      const std::string trackId = a.value("trackId", "");
      if (k < 0 || trackId.empty()) continue;
      ids[k] = trackId;
      notes[k] = std::clamp(a.value("gm", -1), -1, 127);
    }
  {
    std::lock_guard<std::mutex> lock(mCustomMutex);
    for (int k = 0; k < dgg::kNumCustomInstruments; k++) mCustomTrackId[k] = ids[k];
  }
  bool gainsChanged = false;
  for (int k = 0; k < dgg::kNumCustomInstruments; k++)
  {
    const int previous = mCustomNote[k].load(std::memory_order_relaxed);
    mCustomNote[k].store(notes[k], std::memory_order_relaxed);
    // A newly assigned note starts with the instrument's default Volume (the "volume" of its table row) unless its gain was already changed.
    if (notes[k] >= 0 && notes[k] != previous)
    {
      const double volume = dgg::DefaultOf(dgg::CustomInstrument(k), "volume", 1.0);
      if (volume != 1.0 && mArticulationGain[notes[k]].load(std::memory_order_relaxed) == 1.0)
      {
        mArticulationGain[notes[k]].store(volume, std::memory_order_relaxed);
        gainsChanged = true;
      }
    }
    DGG_Log(std::string("HandleSetCustomInstruments: ") + dgg::CustomInstrument(k).name + " track='" + ids[k] + "' note=" + std::to_string(notes[k]));
  }
  if (gainsChanged) SendBridgeMessage("articulationLevelState", BuildArticulationGainsJson(mEngineMode.load(std::memory_order_relaxed) == static_cast<int>(EngineMode::LoFi)));
}

// Preview of a custom instrument from the UI (knobs of its Drum Engine card, no track needed): the note-on travels the same MIDI path as
// every preview, on a channel of its own (kCustomPreviewMidiChannel) with the registry index as note number.
void DrumGroover::HandleCustomPreview(const nlohmann::json& payload)
{
  const int k = payload.value("index", -1);
  if (k < 0 || k >= dgg::kNumCustomInstruments) return;
  IMidiMsg msg;
  const int vel = std::clamp(payload.value("velocity", 127), 1, 127);   // the volume knob is applied to the output, not to the velocity
  msg.MakeNoteOnMsg(k, vel, 0, kCustomPreviewMidiChannel);
  SendMidiMsgFromUI(msg);
  DGG_Log("HandleCustomPreview: instrument=" + std::string(dgg::CustomInstrument(k).name));
}

// Volume knob of a custom instrument card that has no track (with a track the knob is the note gain, setArticulationLevel).
void DrumGroover::HandleSetCustomPreviewGain(const nlohmann::json& payload)
{
  const int k = payload.value("index", -1);
  if (k < 0 || k >= dgg::kNumCustomInstruments) return;
  mCustomPreviewGain[k].store(std::clamp(payload.value("value", 1.0), 0.0, 3.0), std::memory_order_relaxed);
}

// audio thread: a short try-lock, a contended lookup simply does not trigger
int DrumGroover::CustomInstrumentForTrack(const std::string& trackId)
{
  std::unique_lock<std::mutex> lock(mCustomMutex, std::try_to_lock);
  if (!lock.owns_lock() || trackId.empty()) return -1;
  for (int k = 0; k < dgg::kNumCustomInstruments; k++) if (mCustomTrackId[k] == trackId) return k;
  return -1;
}

void DrumGroover::TriggerCustom(int k, double velocity01, double gain)
{
  if (IsPc2Kind(dgg::CustomInstrument(k).kind))
  {
    mCustomPc2Pools[k].SetNextGain(gain);
    mCustomPc2Pools[k].Trigger(velocity01);
    return;
  }
  if (IsCbKind(dgg::CustomInstrument(k).kind))
  {
    mCustomCbPools[k].SetNextGain(gain);
    mCustomCbPools[k].Trigger(velocity01);
    return;
  }
  if (IsHcKind(dgg::CustomInstrument(k).kind))
  {
    mCustomHcPools[k].SetNextGain(gain);
    mCustomHcPools[k].Trigger(velocity01);
    return;
  }
  if (dgg::CustomInstrument(k).kind != dgg::EngineKind::Tom)
  {
    mCustomRmPools[k].SetNextGain(gain);
    mCustomRmPools[k].Trigger(velocity01);
    return;
  }
  mCustomPools[k].SetNextGain(gain);
  mCustomPools[k].Trigger(velocity01);
}

void DrumGroover::HandleSetPadPos(const nlohmann::json& payload)
{
  const std::string family = payload.value("family", "");
  const int index = payload.value("index", -1);
  if (family.empty() || index < 0) return;
  const std::string key = family + ":" + std::to_string(index);
  PadPos pos;
  pos.x = payload.value("x", 0.5);
  pos.y = payload.value("y", 0.5);
  pos.synced = payload.value("synced", true);
  std::lock_guard<std::mutex> lock(mStateMutex);
  mPadPositions[key] = pos;
}

void DrumGroover::HandleSetArticulationLevel(const nlohmann::json& payload)
{
  const int note      = payload.value("note", -1);
  const double value  = payload.value("value", 1.0);
  if (note < 0 || note > 127)
  {
    DGG_Log("HandleSetArticulationLevel: note out of range: " + std::to_string(note));
    return;
  }
  const int engineMode = mEngineMode.load(std::memory_order_relaxed);
  if (engineMode == static_cast<int>(EngineMode::LoFi))
  {
    mArticulationGainLoFi[note].store(value, std::memory_order_relaxed);
  }
  else
  {
    mArticulationGain[note].store(value, std::memory_order_relaxed);
  }
  DGG_Log("HandleSetArticulationLevel: note=" + std::to_string(note) + " value=" + std::to_string(value) + " engine=" + (engineMode == static_cast<int>(EngineMode::LoFi) ? "lofi" : "internal"));
}

#endif

#if IPLUG_DSP
// Triggers the pool of a table instance.
void DrumGroover::TriggerInstrument(const dgg::InstrumentDef& inst, double velocity01, double gain)
{
  switch (inst.kind)
  {
    case dgg::EngineKind::SnareSd: mSnareSdPools[inst.poolSlot].SetNextGain(gain); mSnareSdPools[inst.poolSlot].Trigger(velocity01); break;
    case dgg::EngineKind::RmRim: mSnareRmPools[inst.poolSlot].SetNextGain(gain); mSnareRmPools[inst.poolSlot].Trigger(velocity01); break;
    case dgg::EngineKind::Tom: mTomPools[inst.poolSlot].SetNextGain(gain); mTomPools[inst.poolSlot].Trigger(velocity01); break;
    case dgg::EngineKind::Ride:
    {
      // A new Ride hit (any articulation) chokes every other still-ringing Ride voice first, real cymbal or not -- a physical strike
      // dampens whatever was resonating, it doesn't let it ring on freely underneath the new hit. The Mute hit does it after its own
      // trigger, to every other ride pool.
      const bool isMute = dgg::StrEq(inst.articulation, "Mute");
      if (!isMute) for (auto& p : mRidePools) p.ChokeAll();
      mRidePools[inst.poolSlot].SetNextGain(gain);
      mRidePools[inst.poolSlot].Trigger(velocity01, dgg::RideArticulationForPool(inst.poolSlot));
      if (isMute)
      {
        ForEachRidePool([&inst](const dgg::InstrumentDef& d, auto& pool) { if (d.paramIndex != inst.paramIndex) pool.ChokeAll(); });
        break;
      }
      break;
    }
    case dgg::EngineKind::Cymbal:   // Crash and Mute play TR808RideVoice's articulations of the same names
      mCymbalPools[inst.poolSlot].SetNextGain(gain); mCymbalPools[inst.poolSlot].Trigger(velocity01, inst.isMute ? dgg::TR808RideVoice::Articulation::Mute : dgg::TR808RideVoice::Articulation::Crash);
      if (inst.isMute)   // a mute hit chokes the crash of its cymbal
        ForEachCymbalPool([&inst](const dgg::InstrumentDef& d, auto& pool) { if (d.cymbalPos == inst.cymbalPos && !d.isMute) pool.ChokeAll(); });
      break;
    case dgg::EngineKind::HiHat:
      mHiHatPools[inst.poolSlot].SetNextGain(gain); mHiHatPools[inst.poolSlot].Trigger(velocity01);
      if (dgg::StrEq(inst.circuitFamily, "Closed"))   // a closed hit chokes every open hi-hat
        ForEachHiHatPool([](const dgg::InstrumentDef& d, auto& pool) { if (dgg::StrEq(d.circuitFamily, "Open")) pool.ChokeAll(); });
      break;
    case dgg::EngineKind::Kick:
      mKickPools[inst.poolSlot].SetNextGain(gain); mKickPools[inst.poolSlot].Trigger(velocity01);
      break;
    default: break;
  }
}

void DrumGroover::TriggerGmNote(int note, double velocity01)
{
  const uint32_t missBefore = dgg::CacheMissSeq().load(std::memory_order_relaxed);
  TriggerGmNoteVoices(note, velocity01);
  if (note >= 0 && note < 128 && dgg::CacheMissSeq().load(std::memory_order_relaxed) != missBefore)
    mMissNoteBits[note >> 6].fetch_or(uint64_t(1) << (note & 63), std::memory_order_relaxed);
}

void DrumGroover::TriggerGmNoteVoices(int note, double velocity01)
{
  if (!mActive.load(std::memory_order_relaxed) || mUserMuted.load(std::memory_order_relaxed)) return;

  const int engineMode = mEngineMode.load(std::memory_order_relaxed);
  const double gain = (engineMode == static_cast<int>(EngineMode::LoFi))
      ? mArticulationGainLoFi[note].load(std::memory_order_relaxed)
      : mArticulationGain[note].load(std::memory_order_relaxed);
  const double g_vel = std::min(1.0, velocity01 * gain);   // LoFi voices only: their volume still scales the velocity

  const int familyIdx = NoteToFamilyIndex(note);
  if (familyIdx >= 0 &&
      engineMode == static_cast<int>(EngineMode::LoFi))
  {
    // Per-voice mixer routing: the LoFi pools are family-granular, so each
    // voice carries its own target channel (same NoteToChannel table as the
    // internal engine); ProcessBlock() routes it into that channel buffer.
    auto& lofiPool = mLoFiPools[familyIdx];
    const int voiceIdx = lofiPool.Trigger(g_vel, NoteToLoFiRole(note));
    lofiPool.Voices()[voiceIdx].SetTargetChannel(NoteToChannel(note));
    return;
  }

  int customK = -1;   // a note of a track with a custom instrument plays that instrument (checked before the kit notes)
  for (int k = 0; k < dgg::kNumCustomInstruments && customK < 0; k++)
    if (note >= 0 && note == mCustomNote[k].load(std::memory_order_relaxed)) customK = k;
  if (customK >= 0) TriggerCustom(customK, std::min(1.0, velocity01), gain);
  else if (const dgg::InstrumentDef* inst = dgg::FindInstrumentByNote(note)) TriggerInstrument(*inst, std::min(1.0, velocity01), gain);
}

void DrumGroover::ProcessMidiMsg(const IMidiMsg& msg)
{
  if (msg.StatusMsg() == IMidiMsg::kNoteOn && msg.Velocity() > 0)
  {
    if (msg.Channel() == kCustomPreviewMidiChannel)   // UI preview of a custom instrument, note = registry index
    {
      if (msg.NoteNumber() >= 0 && msg.NoteNumber() < dgg::kNumCustomInstruments && mActive.load(std::memory_order_relaxed) && !mUserMuted.load(std::memory_order_relaxed))
        TriggerCustom(msg.NoteNumber(), msg.Velocity() / 127.0, mCustomPreviewGain[msg.NoteNumber()].load(std::memory_order_relaxed));
      return;
    }
    TriggerGmNote(msg.NoteNumber(), msg.Velocity() / 127.0);
  }
}

// The host reports its sample rate after construction (and again when it changes). The voices must follow it: the cache keys of
// the baker (GetSampleRate()) and of the playing voices (their own rate) contain it and have to agree.
void DrumGroover::OnReset()
{
#if IPLUG_DSP
  const double sr = GetSampleRate();
  for (auto& p : mKickPools) p.SetSampleRate(sr);
  for (auto& p : mSnareSdPools) p.SetSampleRate(sr);
  for (auto& p : mSnareRmPools) p.SetSampleRate(sr);
  for (auto& p : mHiHatPools) p.SetSampleRate(sr);
  for (auto& p : mCymbalPools) p.SetSampleRate(sr);
  for (auto& p : mRidePools) p.SetSampleRate(sr);
  for (auto& pool : mTomPools) pool.SetSampleRate(sr);
  for (auto& pool : mCustomPools) pool.SetSampleRate(sr);
  for (auto& pool : mCustomRmPools) pool.SetSampleRate(sr);
  for (auto& pool : mCustomHcPools) pool.SetSampleRate(sr);
  for (auto& pool : mCustomCbPools) pool.SetSampleRate(sr);
  for (auto& pool : mCustomPc2Pools) pool.SetSampleRate(sr);
  for (auto& buf : mChanBuf) buf.assign(std::max(GetBlockSize(), 1), 0.0);   // sized for the host's block size ahead of the audio thread
#endif
}

void DrumGroover::OnActivate(bool active)
{
  mActive.store(active, std::memory_order_relaxed);
  DGG_Log("OnActivate: active=" + std::string(active ? "true" : "false"));
}

bool DrumGroover::SerializeState(IByteChunk& chunk) const
{
  const bool paramsOk = SerializeParams(chunk);

  nlohmann::json state;
  state["formatVersion"] = 1;
  nlohmann::json padPos = nlohmann::json::object();
  size_t projectJsonSize = 0;
  {
    std::lock_guard<std::mutex> lock(mStateMutex);
    state["projectJson"] = mCachedProjectJson;
    projectJsonSize = mCachedProjectJson.size();
    for (const auto& kv : mPadPositions) {
      padPos[kv.first] = { {"x", kv.second.x}, {"y", kv.second.y}, {"synced", kv.second.synced} };
    }
  }

  nlohmann::json engine;
  engine["engineMode"] = (mEngineMode.load(std::memory_order_relaxed) == static_cast<int>(EngineMode::LoFi)) ? "lofi" : "internal";
  engine["muted"] = mUserMuted.load(std::memory_order_relaxed);
  engine["polyphony"] = {
    {"kick",  mKickPools[0].GetPolyphony()},
    {"snare", mSnareSdPools[0].GetPolyphony()},
    {"tom",   mTomPools[0].GetPolyphony()},
    {"ride",  mRidePools[0].GetPolyphony()},
  };
  // Only the target size (1-20), NOT the baked audio itself -- that lives
  // in %APPDATA%\DrumGroover\Cache\..., see GetCacheDir().
  engine["cacheVariantCount"] = mCacheVariantCount.load(std::memory_order_relaxed);
  engine["voiceParams"] = BuildVoiceParamsJson();
  engine["articulationGain"] = BuildArticulationGainsJson(false);
  engine["articulationGainLoFi"] = BuildArticulationGainsJson(true);
  engine["padPositions"] = padPos;
  state["drumEngine"] = engine;

  // Mixer (channels/master/buses incl. FX) — survives host project save/recall.
  state["mixer"] = BuildMixerStateJson();

  const std::string dump = state.dump();
  DGG_Log("SerializeState: writing " + std::to_string(dump.size()) + " bytes (projectJson=" + std::to_string(projectJsonSize) + " bytes)");
  chunk.PutStr(dump.c_str());
  return paramsOk;
}

int DrumGroover::UnserializeState(const IByteChunk& chunk, int startPos)
{
  startPos = UnserializeParams(chunk, startPos);

  WDL_String str;
  const int nextPos = chunk.GetStr(str, startPos);
  if (nextPos < 0)
  {
    DGG_Log("UnserializeState: no state blob found past IParams (old save or empty chunk) — leaving defaults");
    return startPos;
  }
  startPos = nextPos;

  nlohmann::json state;
  try { state = nlohmann::json::parse(str.Get()); }
  catch (const std::exception& e)
  {
    DGG_Log(std::string("UnserializeState: JSON parse failed, leaving defaults: ") + e.what());
    return startPos;
  }
  if (!state.is_object())
  {
    DGG_Log("UnserializeState: state blob was not a JSON object, leaving defaults");
    return startPos;
  }

  DGG_Log("UnserializeState: restoring " + std::to_string(str.GetLength()) + " bytes");

  if (state.contains("projectJson") && state["projectJson"].is_string())
  {
    std::string projectJson = state["projectJson"].get<std::string>();
    std::lock_guard<std::mutex> lock(mStateMutex);
    mCachedProjectJson = std::move(projectJson);
  }

  if (state.contains("drumEngine") && state["drumEngine"].is_object())
  {
    const nlohmann::json& engine = state["drumEngine"];

    if (engine.contains("voiceParams")) ApplyVoiceParamsJson(engine["voiceParams"]);
    if (engine.contains("articulationGain")) ApplyArticulationGainsJson(engine["articulationGain"], false);
    if (engine.contains("articulationGainLoFi")) ApplyArticulationGainsJson(engine["articulationGainLoFi"], true);
    if (engine.contains("padPositions") && engine["padPositions"].is_object()) {
      std::map<std::string, PadPos> restored;
      for (auto& [key, val] : engine["padPositions"].items()) {
        if (!val.is_object()) continue;
        PadPos pos;
        pos.x = val.value("x", 0.5);
        pos.y = val.value("y", 0.5);
        pos.synced = val.value("synced", true);
        restored[key] = pos;
      }
      std::lock_guard<std::mutex> lock(mStateMutex);
      mPadPositions = std::move(restored);
    }

    if (engine.contains("engineMode"))
    {
      const std::string engineStr = engine.value("engineMode", "internal");
      const int mode = (engineStr == "lofi") ? static_cast<int>(EngineMode::LoFi) : static_cast<int>(EngineMode::Internal);
      mEngineMode.store(mode, std::memory_order_relaxed);
    }
    if (engine.contains("muted")) HandleSetMuted({ {"muted", engine.value("muted", false)} });

    if (engine.contains("polyphony") && engine["polyphony"].is_object())
    {
      const nlohmann::json& poly = engine["polyphony"];
      if (poly.contains("kick"))  HandleSetPolyphony({ {"family", "kick"},  {"value", poly.value("kick", mKickPools[0].GetPolyphony())} });
      if (poly.contains("snare")) HandleSetPolyphony({ {"family", "snare"}, {"value", poly.value("snare", mSnareSdPools[0].GetPolyphony())} });
      if (poly.contains("tom"))   HandleSetPolyphony({ {"family", "tom"},   {"value", poly.value("tom", mTomPools[0].GetPolyphony())} });
      if (poly.contains("ride"))  HandleSetPolyphony({ {"family", "ride"},  {"value", poly.value("ride", mRidePools[0].GetPolyphony())} });
    }
    if (engine.contains("cacheVariantCount"))
      HandleSetCacheVariantCount({ {"value", engine.value("cacheVariantCount", 7)} });
  }

  // Mixer state (newer saves carry it; old saves keep the defaults).
  if (state.contains("mixer") && state["mixer"].is_object())
    HandleMixerState(state["mixer"]);

  mPendingStateRefresh.store(true, std::memory_order_relaxed);

  return startPos;
}

void DrumGroover::WaitForCacheBeforeOfflineRender()
{
  if (!GetRenderingOffline())
  {
    mOfflineCachePrepared = false;
    return;
  }

#if IPLUG_EDITOR
  if (!mOfflineCachePrepared)
  {
    mOfflineCachePrepared = true;
    for (int i = 0; i < dgg::CountInstruments("ride"); i++)  EnqueueRideVariantBake(i);
    for (int i = 0; i < dgg::CountInstruments("hihat"); i++)  EnqueueHiHatVariantBake(i);
    for (int i = 0; i < dgg::CountInstruments("cymbal"); i++) EnqueueCrashVariantBake(i);
  }
#endif

  auto& baker = dgg::TR808VariantPoolBaker::Instance();
  if (baker.IsIdle()) return;

  // Upper bound so a stuck bake job can't hang the host forever.
  constexpr double kMaxWaitSec = 600.0;
  const auto t0 = std::chrono::steady_clock::now();
  double waited = 0.0;
  while (!baker.IsIdle() && waited < kMaxWaitSec)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    waited = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  }
  DGG_Log("OfflineRender: waited " + std::to_string(waited) + "s for the cache" + (baker.IsIdle() ? "" : " (gave up)"));
}

void DrumGroover::ProcessBlock(sample** inputs, sample** outputs, int nFrames)
{
  const int nChans = NOutChansConnected();
  for (int s = 0; s < nFrames; s++)
  {
    for (int c = 0; c < nChans; c++)
    {
      outputs[c][s] = 0.0;
    }
  }

  if (!mActive.load(std::memory_order_relaxed) || mUserMuted.load(std::memory_order_relaxed)) return;

  WaitForCacheBeforeOfflineRender();

  const std::vector<dgg::DueEvent> dueEvents = mScheduler.Advance(
    GetPPQPos(), GetTempo(), GetTransportIsRunning(),
    nFrames, GetSampleRate(),
    mLoopStartBeat.load(std::memory_order_relaxed),
    mLoopEndBeat.load(std::memory_order_relaxed));

  const std::vector<dgg::DueEvent> previewDueEvents = mPreviewScheduler.Advance(
    GetTempo(), nFrames, GetSampleRate());

  std::vector<dgg::DueEvent> mergedDueEvents;
  mergedDueEvents.reserve(dueEvents.size() + previewDueEvents.size());
  mergedDueEvents.insert(mergedDueEvents.end(), dueEvents.begin(), dueEvents.end());
  mergedDueEvents.insert(mergedDueEvents.end(), previewDueEvents.begin(), previewDueEvents.end());
  std::sort(mergedDueEvents.begin(), mergedDueEvents.end(),
    [](const dgg::DueEvent& a, const dgg::DueEvent& b) { return a.sampleOffset < b.sampleOffset; });

  // --- Mixer channel buffers: one mono buffer per instrument channel (see dsp/Mixer.h). The Drum Engine focus
  // meter reads its family peaks from these (family = union of channels, below).
  auto& chanBuf = mChanBuf;
  for (auto& buf : chanBuf)
  {
    if (static_cast<int>(buf.size()) < nFrames) buf.resize(nFrames);   // only when a larger block than ever before arrives
    std::fill(buf.begin(), buf.begin() + nFrames, 0.0);
  }
  const double* chanIn[dgg::kMixerNumChannels];
  for (int i = 0; i < dgg::kMixerNumChannels; ++i) chanIn[i] = chanBuf[i].data();

  // Mixer state snapshot for this block (UI thread may be updating it).
  dgg::MixerState mix;
  {
    std::lock_guard<std::mutex> lk(mMixerMutex);
    mix = mMixerState;
  }

  {
    int pos = 0;
    size_t evIdx = 0;
    while (pos < nFrames)
    {
      while (evIdx < mergedDueEvents.size() && mergedDueEvents[evIdx].sampleOffset == pos)
      {
        const dgg::DueEvent& ev = mergedDueEvents[evIdx];
        auto trackIt = std::find_if(mTracks.begin(), mTracks.end(),
          [&](const dgg::Track& t) { return t.id == ev.trackId; });
        if (trackIt != mTracks.end())
          TriggerGmNote(trackIt->gm, ev.vel / 127.0);
        else
        {
          const int ck = CustomInstrumentForTrack(ev.trackId);
          if (ck >= 0)
          {
            const int cn = mCustomNote[ck].load(std::memory_order_relaxed);
            const double g = (cn >= 0 && cn < 128) ? mArticulationGain[cn].load(std::memory_order_relaxed) : 1.0;
            if (mActive.load(std::memory_order_relaxed) && !mUserMuted.load(std::memory_order_relaxed)) TriggerCustom(ck, ev.vel / 127.0, g);
          }
        }
        evIdx++;
      }

      int nextOffset = nFrames;
      if (evIdx < mergedDueEvents.size()) nextOffset = std::min(nextOffset, mergedDueEvents[evIdx].sampleOffset);

      const int segLen = nextOffset - pos;
      if (segLen > 0)
      {
        // Internal engine: pools are per-instrument -> their mixer channel.
        ForEachKickPool([&](const dgg::InstrumentDef& d, auto& pool) {
          pool.Process(chanBuf[static_cast<int>(d.channel)].data() + pos, segLen);
        });

        ForEachSnarePool([&](const dgg::InstrumentDef& d, auto& pool) {
          pool.Process(chanBuf[static_cast<int>(d.channel)].data() + pos, segLen);
        });

        ForEachHiHatPool([&](const dgg::InstrumentDef& d, auto& pool) {
          pool.Process(chanBuf[static_cast<int>(d.channel)].data() + pos, segLen);
        });

        {
          ForEachTomPool([&](const dgg::InstrumentDef& d, auto& pool) {
            pool.Process(chanBuf[static_cast<int>(d.channel)].data() + pos, segLen);
          });
          for (auto& pool : mCustomPools) pool.Process(chanBuf[static_cast<int>(dgg::MixerChannel::Custom)].data() + pos, segLen);
          for (auto& pool : mCustomRmPools) pool.Process(chanBuf[static_cast<int>(dgg::MixerChannel::Custom)].data() + pos, segLen);
          for (auto& pool : mCustomHcPools) pool.Process(chanBuf[static_cast<int>(dgg::MixerChannel::Custom)].data() + pos, segLen);
          for (auto& pool : mCustomCbPools) pool.Process(chanBuf[static_cast<int>(dgg::MixerChannel::Custom)].data() + pos, segLen);
          for (auto& pool : mCustomPc2Pools) pool.Process(chanBuf[static_cast<int>(dgg::MixerChannel::Custom)].data() + pos, segLen);
        }

        ForEachCymbalPool([&](const dgg::InstrumentDef& d, auto& pool) {
          pool.Process(chanBuf[static_cast<int>(d.channel)].data() + pos, segLen);
        });

        for (auto& p : mRidePools) p.Process(chanBuf[static_cast<int>(dgg::MixerChannel::Ride)].data() + pos, segLen);

        // LoFi engine: pools are family-granular, so route per voice via the
        // target channel set at trigger time (see TriggerGmNote).
        for (int f = 0; f < 6; ++f)
        {
          auto& pool = mLoFiPools[f];
          const int poly = pool.GetPolyphony();
          for (int i = 0; i < poly; ++i)
          {
            auto& v = pool.Voices()[i];
            if (!v.IsActive()) continue;
            v.Process(chanBuf[v.TargetChannel()].data() + pos, segLen);
          }
        }
      }

      pos = nextOffset;
    }
  }

  // Focus meter (Drum Engine tab): family peaks from the channel buffers.
  // The 'custom' family is the Custom mixer channel (custom instruments).
  static const int kFocusChannels[7][8] = {
    { static_cast<int>(dgg::MixerChannel::Kick), -1, -1, -1, -1, -1, -1, -1 },
    { static_cast<int>(dgg::MixerChannel::Snare), -1, -1, -1, -1, -1, -1, -1 },
    { static_cast<int>(dgg::MixerChannel::HiHat), -1, -1, -1, -1, -1, -1, -1 },
    { static_cast<int>(dgg::MixerChannel::RackTom1), static_cast<int>(dgg::MixerChannel::RackTom2),
      static_cast<int>(dgg::MixerChannel::RackTom3), static_cast<int>(dgg::MixerChannel::FloorTom1),
      static_cast<int>(dgg::MixerChannel::FloorTom2), -1, -1, -1 },
    { static_cast<int>(dgg::MixerChannel::Cymbal1), static_cast<int>(dgg::MixerChannel::Cymbal2),
      static_cast<int>(dgg::MixerChannel::Cymbal3), static_cast<int>(dgg::MixerChannel::Cymbal4),
      static_cast<int>(dgg::MixerChannel::Cymbal5), -1, -1, -1 },
    { static_cast<int>(dgg::MixerChannel::Ride), -1, -1, -1, -1, -1, -1, -1 },
    { static_cast<int>(dgg::MixerChannel::Custom), -1, -1, -1, -1, -1, -1, -1 },
  };
  for (int f = 0; f < 7; ++f)
  {
    double peak = 0.0;
    for (int ci = 0; kFocusChannels[f][ci] >= 0; ++ci)
      for (int s = 0; s < nFrames; ++s)
        peak = std::max(peak, std::fabs(chanBuf[kFocusChannels[f][ci]][s]));

    double prev = mFocusPeak[f].load(std::memory_order_relaxed);
    while (peak > prev && !mFocusPeak[f].compare_exchange_weak(prev, peak, std::memory_order_relaxed)) {}
  }

  // --- Mixer: channel chains -> fader/pan/mute-solo -> sends -> buses ->
  // master chain/fader -> output. Stereo; mono hosts get an equal-power
  // downmix.
  dgg::MixerProcessor::Peaks mpk;
  if (nChans >= 2)
  {
    mMixerProc.Process(chanIn, outputs[0], outputs[1], nFrames, GetSampleRate(), mix, &mpk);
  }
  else
  {
    std::vector<double> tmpR(nFrames, 0.0);
    mMixerProc.Process(chanIn, outputs[0], tmpR.data(), nFrames, GetSampleRate(), mix, &mpk);
    for (int s = 0; s < nFrames; ++s)
      outputs[0][s] = (outputs[0][s] + tmpR[s]) * 0.7071067811865476;
  }
  for (int c = 2; c < nChans; ++c)
    for (int s = 0; s < nFrames; ++s) outputs[c][s] = outputs[0][s];

  // Mixer meter peaks -> atomics, read by OnIdle() in 'mixerLevels'.
  auto casPeak = [](std::atomic<double>& dst, double v)
  {
    double prev = dst.load(std::memory_order_relaxed);
    while (v > prev && !dst.compare_exchange_weak(prev, v, std::memory_order_relaxed)) {}
  };
  for (int i = 0; i < dgg::kMixerNumChannels; ++i)
  {
    casPeak(mMixerPeakL[i], mpk.ch[i][0]);
    casPeak(mMixerPeakR[i], mpk.ch[i][1]);
  }
  casPeak(mMixerMasterPeakL, mpk.master[0]);
  casPeak(mMixerMasterPeakR, mpk.master[1]);
  casPeak(mMixerRevPeakL, mpk.reverb[0]);
  casPeak(mMixerRevPeakR, mpk.reverb[1]);
  casPeak(mMixerDelPeakL, mpk.delay[0]);
  casPeak(mMixerDelPeakR, mpk.delay[1]);
  // Pre-limiter input peaks (per channel + master) for the limiter engage meter.
  for (int i = 0; i < dgg::kMixerNumChannels; ++i)
  {
    casPeak(mMixerLimInL[i], mpk.limIn[i][0]);
    casPeak(mMixerLimInR[i], mpk.limIn[i][1]);
  }
  casPeak(mMixerMasterLimInL, mpk.masterLimIn[0]);
  casPeak(mMixerMasterLimInR, mpk.masterLimIn[1]);
  for (int i = 0; i < dgg::kMixerNumChannels; ++i)
  {
    casPeak(mMixerLimOutL[i], mpk.limOut[i][0]);
    casPeak(mMixerLimOutR[i], mpk.limOut[i][1]);
  }
  casPeak(mMixerMasterLimOutL, mpk.masterLimOut[0]);
  casPeak(mMixerMasterLimOutR, mpk.masterLimOut[1]);
  for (int i = 0; i < dgg::kMixerNumChannels; ++i)
  {
    casPeak(mMixerLimGrL[i], mpk.limGr[i][0]);
    casPeak(mMixerLimGrR[i], mpk.limGr[i][1]);
  }
  casPeak(mMixerMasterLimGrL, mpk.masterLimGr[0]);
  casPeak(mMixerMasterLimGrR, mpk.masterLimGr[1]);
  casPeak(mMixerKitInL, mpk.kitIn[0]);
  casPeak(mMixerKitInR, mpk.kitIn[1]);
}
#endif