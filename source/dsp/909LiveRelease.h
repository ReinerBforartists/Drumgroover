// Release of the linear-response live form of the toms (and the BD) per voice, accent anchor and pot state. Light header (no models): used by the playback voice, the live host and the model runner.
#pragma once
#include "909AccentPlan.h"
#include "909PotParams.h"
#include <cmath>
#include <cstdint>

namespace dgg {

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Release of the linear-response live form (HitLive.h) per voice, accent anchor and pot state: only what the R115 gate has measured (tools/909_TOMS_BETRIEBSBEREICHE.md, state 2026-11-12). Everything else plays the
// existing path (cache if baked, else the stepwise live run): same sound, only more expensive. BD: its own validation, not restricted here. Voices without a noise response mode: never released.
//   Anchors: LT 25-127, MT 25-127, HT 25-127 (MT 100 / 127 and HT 127 with the documented exception: one window 1-5 ms of seed 5, -28.7 ... -29.9 dB).
//   Pots (Tune / Decay / Level): nominal at every anchor; P1 0.8 / 0.2 / 0.5, P2 0.2 / 0.9 / 0.5, P4 1 / 1 / 0.5, P5 0.5 / 0.5 / 0.1 at the anchors 25, 75, 100, 127 (all three toms); P3 0 / 0 / 0.5 for LT at 25, 75, 100, 127
//   and MT at 25, 75 only, not for HT. A pot position matches a measured state within kTr909ReleasePotTol (the UI step 0.01 is the finest position of the knobs). Level >= kTr909ReleaseMaxLevel (state P6, 0.9: the replay diverges)
//   is never released. A blend of two anchors needs both anchors released (the shared source drops a hit whose run is not ready). The prepared object also refuses to be ready when its self test fails (HitLive.h).
constexpr double kTr909ReleasePotTol = 0.005, kTr909ReleaseMaxLevel = 0.85;
// Switch of the integration: the toms (LT / MT / HT) play live from prepared sets (shared noise stream, like the BD) when their set is released and prepared, else the cache path plays; the release check below is independent of it.
#ifndef DGG_TR909_LIVE_TOMS
#define DGG_TR909_LIVE_TOMS 0   // build switch (harness: /DDGG_TR909_LIVE_TOMS=1)
#endif
constexpr bool kTr909LiveTomsEnabled = DGG_TR909_LIVE_TOMS != 0;
inline bool Tr909LiveTomPots(Tr909Voice v, int anchorCode, uint64_t params)
{
  struct State { double t, d, l; int mask; };   // mask: bit per anchor index of kTr909Anchors (25, 50, 75, 100, 127)
  constexpr int kAll = 0x1F, kMeasured = 0x1D;   // kMeasured: 25, 75, 100, 127 (no 50)
  const bool lt = v == Tr909Voice::Lt, mt = v == Tr909Voice::Mt, ht = v == Tr909Voice::Ht; if (!lt && !mt && !ht) return false;
  const State states[] = {{0.5, 0.5, 0.5, kAll}, {0.8, 0.2, 0.5, kMeasured}, {0.2, 0.9, 0.5, kMeasured}, {0.0, 0.0, 0.5, ht ? 0 : (mt ? 0x05 : kMeasured)}, {1.0, 1.0, 0.5, kMeasured}, {0.5, 0.5, 0.1, kMeasured}};
  int ai = -1; for (int i = 0; i < kTr909AnchorCount; ++i) if (kTr909Anchors[i] == anchorCode) ai = i;
  if (ai < 0) return false;
  double pots[8]; Tr909UnpackPots(v, params, pots); if (pots[2] >= kTr909ReleaseMaxLevel) return false;
  for (const State& st : states)
    if (std::fabs(pots[0] - st.t) <= kTr909ReleasePotTol && std::fabs(pots[1] - st.d) <= kTr909ReleasePotTol && std::fabs(pots[2] - st.l) <= kTr909ReleasePotTol) return ((st.mask >> ai) & 1) != 0;
  return false;
}
inline bool Tr909LiveReleased(Tr909Voice v, int anchorCode, uint64_t params)
{
  if (v == Tr909Voice::Bd) return true;
  return Tr909LiveTomPots(v, anchorCode, params);
}
inline bool Tr909LiveIsTom(Tr909Voice v) { return v == Tr909Voice::Lt || v == Tr909Voice::Mt || v == Tr909Voice::Ht; }

// An accent code is released when every anchor of its run (one anchor, or two for a blend) is released for the pots.
inline bool Tr909LiveAccentReleased(Tr909Voice v, int accentCode, uint64_t params)
{
  Tr909AccentRun runs[2]; const int n = Tr909AccentRuns(v, accentCode, runs); if (n == 0) return false;
  for (int i = 0; i < n; ++i) if (!Tr909LiveReleased(v, runs[i].anchorCode, params)) return false;
  return true;
}

}  // namespace dgg
