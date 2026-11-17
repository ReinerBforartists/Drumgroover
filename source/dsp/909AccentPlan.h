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

enum class Voice909 : int { Bd = 0, Sd, Lt, Mt, Ht, Hc, Cr, Ri, Hh, Ho, kCount };   // Cr: Crash, Ri: Ride, Hh: Closed Hat, Ho: Open Hat (PCM voices)

// The PCM voices (Crash, Ride) play a ROM replacement through an analog path: the accent is a run-time input of the model, there is no noise source (every variant of a set is the same hit).
inline bool IsPcmVoice909(Voice909 v) { return v == Voice909::Cr || v == Voice909::Ri || v == Voice909::Hh || v == Voice909::Ho; }
// Length of a rendered hit in seconds (the PCM voices ring until the circuit has settled: this is the cap).
// Number of variants of a set: the PCM voices have no noise, every variant would be the same hit, so a set has one.
inline int VariantTarget909(Voice909 v, int target) { return IsPcmVoice909(v) ? 1 : target; }
inline double HitSeconds909(Voice909 v) { return IsPcmVoice909(v) ? 6.0 : 0.35; }

inline const char* VoiceName909(Voice909 v)
{
  static const char* const n[] = {"bd", "sd", "lt", "mt", "ht", "hc", "cr", "ri", "hh", "ho"};
  return n[static_cast<int>(v)];
}
inline bool VoiceFromName909(const char* s, Voice909& v)
{
  for (int i = 0; i < static_cast<int>(Voice909::kCount); ++i)
    if (std::strcmp(s, VoiceName909(static_cast<Voice909>(i))) == 0) { v = static_cast<Voice909>(i); return true; }
  return false;
}

// DAC code of the accent bus (Nava firmware: code x 32 mV, 0..127 = 0..4.064 V).
constexpr int kMaxAccentCode909 = 127;
inline double AccentVolts909(int code) { return 0.032 * std::clamp(code, 0, kMaxAccentCode909); }
inline int AccentCode909(double volts) { return std::clamp(static_cast<int>(std::lround(volts / 0.032)), 0, kMaxAccentCode909); }

// Anchors whose models exist (accent 0.8, 1.6, 2.4, 3.2, 4.064 V as codes 25, 50, 75, 100, 127).
constexpr int kAnchors909[] = {25, 50, 75, 100, 127};
constexpr int kAnchorCount909 = 5;

// Lowest code from which the blend of the two neighbouring anchors is validated (-30 dB rule against the engine, 5 us noise source).
inline int BlendMinCode909(Voice909 v)
{
  switch (v)
  {
    case Voice909::Lt:
    case Voice909::Hc: return 25;   // checked at the midpoint 1.2 V between the anchors 0.8 and 1.6 V
    default: return 50;               // BD, SD, MT, HT failed at 1.2 V (SD also at 1.0 / 1.4 V with anchors 0.2 V apart)
  }
}

enum class AccentKind909 { Exact, Blend, Unvalidated };
struct AccentPlan909
{
  AccentKind909 kind = AccentKind909::Unvalidated;
  int loAnchor = 0, hiAnchor = 0;   // anchor codes; equal for Exact
  double weightHi = 0.0;            // Blend: output = (1 - w) y(lo) + w y(hi), w linear in the accent voltage
};

inline AccentPlan909 PlanAccent909(Voice909 v, int code)
{
  AccentPlan909 p;
  code = std::clamp(code, 0, kMaxAccentCode909);
  if (IsPcmVoice909(v))   // the accent is a run-time input of the model: any code from the lowest anchor up is an exact run
  {
    if (code >= kAnchors909[0]) { p.kind = AccentKind909::Exact; p.loAnchor = p.hiAnchor = code; }
    return p;
  }
  for (int i = 0; i < kAnchorCount909; ++i)
    if (kAnchors909[i] == code) { p.kind = AccentKind909::Exact; p.loAnchor = p.hiAnchor = code; return p; }
  if (code < kAnchors909[0] || code < BlendMinCode909(v)) return p;   // below the lowest anchor or outside the validated range
  int i = 0;
  while (i + 1 < kAnchorCount909 - 1 && code > kAnchors909[i + 1]) ++i;
  p.kind = AccentKind909::Blend; p.loAnchor = kAnchors909[i]; p.hiAnchor = kAnchors909[i + 1];
  p.weightHi = static_cast<double>(code - p.loAnchor) / static_cast<double>(p.hiAnchor - p.loAnchor);
  return p;
}

// The model runs a bake has to perform for an accent code: 1 (exact anchor), 2 (blend of the neighbour anchors with weights summing to 1) or 0
// when the accent is not validated. A bake must take its runs from here; there is no other way to obtain a blend weight.
struct AccentRun909 { int anchorCode = 0; double weight = 0.0; };
inline int AccentRuns909(Voice909 v, int code, AccentRun909 out[2])
{
  const AccentPlan909 p = PlanAccent909(v, code);
  if (p.kind == AccentKind909::Exact) { out[0] = { p.loAnchor, 1.0 }; return 1; }
  if (p.kind == AccentKind909::Blend) { out[0] = { p.loAnchor, 1.0 - p.weightHi }; out[1] = { p.hiAnchor, p.weightHi }; return 2; }
  return 0;
}

// Cache identity of a baked 909 set: voice, quantised knob state (opaque 64 bit), accent code and noise path. The variant files of a set are
// "<BaseName>_v<k>.f32" like every other set; k selects the noise seed of the variant (seed = k), so the seed is part of the file name.
enum class NoisePath909 : int { Reference5us = 0, Fast20us = 1 };

// The cache of these voices is baked by the complete circuit (909CircuitRunner.h, 909CircuitBake.h): BD, SD, LT, MT, HT, HC with the 5 us noise path.
inline bool CircuitCovers909(Voice909 v, NoisePath909 n)
{
  return n == NoisePath909::Reference5us && (v == Voice909::Bd || v == Voice909::Sd || v == Voice909::Lt || v == Voice909::Mt || v == Voice909::Ht || v == Voice909::Hc);
}


struct SetKey909
{
  Voice909 voice = Voice909::Bd;
  uint64_t params = 0;   // quantised knob state of the voice
  int accentCode = 0;
  NoisePath909 noise = NoisePath909::Reference5us;
  int sampleRateHz = 48000;                 // rate of the baked samples (the cache is rate specific)
  bool operator==(const SetKey909& o) const { return voice == o.voice && params == o.params && accentCode == o.accentCode && noise == o.noise && sampleRateHz == o.sampleRateHz; }
};

inline std::string SetBaseName909(const SetKey909& k)
{
  char b[128];
  std::snprintf(b, sizeof b, "909_%s_p%016llx_c%03d_n%d_sr%d", VoiceName909(k.voice), static_cast<unsigned long long>(k.params), std::clamp(k.accentCode, 0, kMaxAccentCode909), static_cast<int>(k.noise), k.sampleRateHz);
  return b;
}

inline bool DecodeSetBaseName909(const std::string& name, SetKey909& k)
{
  char vb[8] = {0}; unsigned long long p = 0; int c = 0, n = 0, sr = 0;
  if (std::sscanf(name.c_str(), "909_%2[a-z]_p%16llx_c%3d_n%d_sr%d", vb, &p, &c, &n, &sr) != 5) return false;
  Voice909 v;
  if (!VoiceFromName909(vb, v) || c < 0 || c > kMaxAccentCode909 || (n != 0 && n != 1)) return false;
  SetKey909 t; t.voice = v; t.params = p; t.accentCode = c; t.noise = static_cast<NoisePath909>(n); t.sampleRateHz = sr;
  if (SetBaseName909(t) != name) return false;   // the name must be exactly what the key spells
  k = t; return true;
}

// Noise seed of variant k (1-based file index _v<k>): the engine source has index 2 and the seed offset is the variant number.
inline uint64_t NoiseSeed909(int variantIndex) { return static_cast<uint64_t>(std::max(variantIndex, 1)); }

}  // namespace dgg
