// TR-909 fast-voice accent handling: which model anchors serve which accent code, and how a baked set is named in the cache.
//
// A fast model is valid at the accent it was generated for (a different accent needs its own model: the reduction basis depends on the
// operating point). Between two anchors a hit is the linear blend of two full model runs with the same noise seed; this was validated
// per voice against the engine (tools handoff R170) only from code 50 (1.6 V) upwards, for LT and HC also at the midpoint 1.2 V.
// Below that range only an exact anchor may be baked.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace dgg {

enum class Tr909Voice : int { Bd = 0, Sd, Lt, Mt, Ht, Hc, kCount };

inline const char* Tr909VoiceName(Tr909Voice v)
{
  static const char* const n[] = {"bd", "sd", "lt", "mt", "ht", "hc"};
  return n[static_cast<int>(v)];
}
inline bool Tr909VoiceFromName(const char* s, Tr909Voice& v)
{
  for (int i = 0; i < static_cast<int>(Tr909Voice::kCount); ++i)
    if (std::strcmp(s, Tr909VoiceName(static_cast<Tr909Voice>(i))) == 0) { v = static_cast<Tr909Voice>(i); return true; }
  return false;
}

// DAC code of the accent bus (Nava firmware: code x 32 mV, 0..127 = 0..4.064 V).
constexpr int kTr909MaxAccentCode = 127;
inline double Tr909AccentVolts(int code) { return 0.032 * std::clamp(code, 0, kTr909MaxAccentCode); }
inline int Tr909AccentCode(double volts) { return std::clamp(static_cast<int>(std::lround(volts / 0.032)), 0, kTr909MaxAccentCode); }

// Anchors whose models exist (accent 0.8, 1.6, 2.4, 3.2, 4.064 V as codes 25, 50, 75, 100, 127).
constexpr int kTr909Anchors[] = {25, 50, 75, 100, 127};
constexpr int kTr909AnchorCount = 5;

// Lowest code from which the blend of the two neighbouring anchors is validated (-30 dB rule against the engine, 5 us noise source).
inline int Tr909BlendMinCode(Tr909Voice v)
{
  switch (v)
  {
    case Tr909Voice::Lt:
    case Tr909Voice::Hc: return 25;   // checked at the midpoint 1.2 V between the anchors 0.8 and 1.6 V
    default: return 50;               // BD, SD, MT, HT failed at 1.2 V (SD also at 1.0 / 1.4 V with anchors 0.2 V apart)
  }
}

enum class Tr909AccentKind { Exact, Blend, Unvalidated };
struct Tr909AccentPlan
{
  Tr909AccentKind kind = Tr909AccentKind::Unvalidated;
  int loAnchor = 0, hiAnchor = 0;   // anchor codes; equal for Exact
  double weightHi = 0.0;            // Blend: output = (1 - w) y(lo) + w y(hi), w linear in the accent voltage
};

inline Tr909AccentPlan Tr909PlanAccent(Tr909Voice v, int code)
{
  Tr909AccentPlan p;
  code = std::clamp(code, 0, kTr909MaxAccentCode);
  for (int i = 0; i < kTr909AnchorCount; ++i)
    if (kTr909Anchors[i] == code) { p.kind = Tr909AccentKind::Exact; p.loAnchor = p.hiAnchor = code; return p; }
  if (code < kTr909Anchors[0] || code < Tr909BlendMinCode(v)) return p;   // below the lowest anchor or outside the validated range
  int i = 0;
  while (i + 1 < kTr909AnchorCount - 1 && code > kTr909Anchors[i + 1]) ++i;
  p.kind = Tr909AccentKind::Blend; p.loAnchor = kTr909Anchors[i]; p.hiAnchor = kTr909Anchors[i + 1];
  p.weightHi = static_cast<double>(code - p.loAnchor) / static_cast<double>(p.hiAnchor - p.loAnchor);
  return p;
}

// The model runs a bake has to perform for an accent code: 1 (exact anchor), 2 (blend of the neighbour anchors with weights summing to 1) or 0
// when the accent is not validated. A bake must take its runs from here; there is no other way to obtain a blend weight.
struct Tr909AccentRun { int anchorCode = 0; double weight = 0.0; };
inline int Tr909AccentRuns(Tr909Voice v, int code, Tr909AccentRun out[2])
{
  const Tr909AccentPlan p = Tr909PlanAccent(v, code);
  if (p.kind == Tr909AccentKind::Exact) { out[0] = { p.loAnchor, 1.0 }; return 1; }
  if (p.kind == Tr909AccentKind::Blend) { out[0] = { p.loAnchor, 1.0 - p.weightHi }; out[1] = { p.hiAnchor, p.weightHi }; return 2; }
  return 0;
}

// Cache identity of a baked 909 set: voice, quantised knob state (opaque 64 bit), accent code and noise path. The variant files of a set are
// "<BaseName>_v<k>.f32" like every other set; k selects the noise seed of the variant (seed = k), so the seed is part of the file name.
enum class Tr909NoisePath : int { Reference5us = 0, Fast20us = 1 };

// Version of the 909 fast models, the noise sources and the blend rule. It is part of every set name (like the "_m<N>" of the other cache sets):
// changing a model invalidates the files baked with the old one. Increase it with every change of a header, tolerance or noise source.
constexpr int kTr909ModelVersion = 2;

struct Tr909SetKey
{
  Tr909Voice voice = Tr909Voice::Bd;
  uint64_t params = 0;   // quantised knob state of the voice
  int accentCode = 0;
  Tr909NoisePath noise = Tr909NoisePath::Reference5us;
  int sampleRateHz = 48000;                 // rate of the baked samples (the cache is rate specific)
  int modelVersion = kTr909ModelVersion;    // model the file was baked with
  bool IsCurrent() const { return modelVersion == kTr909ModelVersion; }
  bool operator==(const Tr909SetKey& o) const { return voice == o.voice && params == o.params && accentCode == o.accentCode && noise == o.noise && sampleRateHz == o.sampleRateHz && modelVersion == o.modelVersion; }
};

inline std::string Tr909SetBaseName(const Tr909SetKey& k)
{
  char b[128];
  std::snprintf(b, sizeof b, "909_%s_p%016llx_c%03d_n%d_sr%d_m%d", Tr909VoiceName(k.voice), static_cast<unsigned long long>(k.params), std::clamp(k.accentCode, 0, kTr909MaxAccentCode), static_cast<int>(k.noise), k.sampleRateHz, k.modelVersion);
  return b;
}

inline bool Tr909DecodeSetBaseName(const std::string& name, Tr909SetKey& k)
{
  char vb[8] = {0}; unsigned long long p = 0; int c = 0, n = 0, sr = 0, mv = 0;
  if (std::sscanf(name.c_str(), "909_%2[a-z]_p%16llx_c%3d_n%d_sr%d_m%d", vb, &p, &c, &n, &sr, &mv) != 6) return false;
  Tr909Voice v;
  if (!Tr909VoiceFromName(vb, v) || c < 0 || c > kTr909MaxAccentCode || (n != 0 && n != 1)) return false;
  Tr909SetKey t; t.voice = v; t.params = p; t.accentCode = c; t.noise = static_cast<Tr909NoisePath>(n); t.sampleRateHz = sr; t.modelVersion = mv;
  if (Tr909SetBaseName(t) != name) return false;   // the name must be exactly what the key spells
  k = t; return true;
}

// Noise seed of variant k (1-based file index _v<k>): the engine source has index 2 and the seed offset is the variant number.
inline uint64_t Tr909NoiseSeed(int variantIndex) { return static_cast<uint64_t>(std::max(variantIndex, 1)); }

}  // namespace dgg
