// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Reiner Prokein
#include "Scheduler.h"

#include <algorithm>
#include <cmath>

namespace dgg {

namespace {
// Always-non-negative modulo (std::fmod can return a negative result for
// a negative dividend, which C++'s % / fmod do NOT clean up for you the
// way JS's `((a % n) + n) % n` idiom does — this matches that JS idiom).
double PositiveMod(double a, double n) {
  double r = std::fmod(a, n);
  return r < 0.0 ? r + n : r;
}
}  // namespace

void Scheduler::SetPattern(FullPattern pattern) {
  auto shared = std::make_shared<const FullPattern>(std::move(pattern));
  std::lock_guard<std::mutex> lock(mPatternMutex);
  mPattern = std::move(shared);
}

std::shared_ptr<const FullPattern> Scheduler::SnapshotPattern() const {
  std::lock_guard<std::mutex> lock(mPatternMutex);
  return mPattern;
}

std::vector<DueEvent> Scheduler::Advance(double hostPpqPos, double bpm, bool transportIsPlaying,
                                          int nFrames, double sampleRate,
                                          double loopStartBeat, double loopEndBeat) {
  std::vector<DueEvent> due;

  // Stopped transport: nothing due, nothing to remember for later — see
  // the stateless design note in Scheduler.h.
  if (!transportIsPlaying) return due;
  if (bpm <= 0.0 || nFrames <= 0 || sampleRate <= 0.0) return due; // guards against a div-by-zero below

  std::shared_ptr<const FullPattern> pattern = SnapshotPattern();
  if (!pattern || pattern->events.empty() || pattern->totalBeats <= 0.0) return due;

  // "No own loop range defined" -> NO looping, play once and stay silent.
  // Host-synced playback should follow the host position without wrapping.
  bool hasLoop = loopStartBeat >= 0.0;
  double activeLoopStart = 0.0;
  double activeLoopEnd   = 0.0;
  double loopLen = 0.0;
  if (hasLoop) {
    activeLoopStart = loopStartBeat;
    activeLoopEnd   = loopEndBeat;
    loopLen = activeLoopEnd - activeLoopStart;
  }
  bool hasValidLoop = hasLoop && loopLen > 1e-9; // degenerate/zero-length range disables wrapping

  const double samplesPerBeat = sampleRate * 60.0 / bpm;
  const double blockBeats     = nFrames / samplesPerBeat;

  // Initial song-space position for this block. A host position before the loop range's start is never wrapped into
  // the range: it plays through linearly until it reaches activeLoopStart, and only then does looping start. So a host
  // reset to 0 starts at song bar 0 instead of jumping into an active loop range.
  double songPos = hostPpqPos;
  if (hasValidLoop && hostPpqPos >= activeLoopStart) {
    songPos = activeLoopStart + PositiveMod(hostPpqPos - activeLoopStart, loopLen);
  }

  const auto& events = pattern->events; // sorted ascending by beatAbs (Song.h/Types.cpp's contract)
  auto beatLess = [](const PatternEvent& ev, double beat) { return ev.beatAbs < beat; };

  double remainingBeats  = blockBeats;
  double consumedBeats   = 0.0; // how far into THIS block (in host beats) we've already accounted for

  // Splits the block into however many loop-boundary-aligned segments it
  // takes to cover blockBeats — normally just one (or two, if the block
  // happens to straddle a wrap point), a small fixed number of iterations
  // even in pathological cases since remainingBeats strictly shrinks by
  // each segment's length every pass.
  while (remainingBeats > 1e-9) {
    double segStart = songPos;
    double segEnd;      // song-space end of this segment
    bool   segWraps;    // does reaching segEnd mean "wrap back to activeLoopStart"?

    if (hasValidLoop && segStart >= activeLoopStart) {
      segEnd   = activeLoopEnd;
      segWraps = true;
    } else if (hasValidLoop) {
      // Pre-loop-range playthrough (segStart < activeLoopStart): runs
      // until it reaches the range, no wrap yet.
      segEnd   = activeLoopStart;
      segWraps = false;
    } else {
      // No valid loop at all: plays once, then silence forever (still
      // fully safe — events simply stop existing past totalBeats, and
      // this segment never "ends" within any finite block).
      segEnd   = segStart + remainingBeats;
      segWraps = false;
    }

    double available = std::max(0.0, segEnd - segStart);
    double segLen     = std::min(available, remainingBeats);

    double windowStart = segStart;
    double windowEnd   = segStart + segLen;

    auto lo = std::lower_bound(events.begin(), events.end(), windowStart, beatLess);
    auto hi = std::lower_bound(events.begin(), events.end(), windowEnd, beatLess);
    for (auto it = lo; it != hi; ++it) {
      double beatIntoBlock = consumedBeats + (it->beatAbs - windowStart);
      int sampleOffset = static_cast<int>(std::lround(beatIntoBlock * samplesPerBeat));
      sampleOffset = std::clamp(sampleOffset, 0, nFrames - 1);
      due.push_back({it->trackId, it->vel, sampleOffset});
    }

    consumedBeats  += segLen;
    remainingBeats -= segLen;

    if (available <= 1e-9) break; // degenerate segment (zero-length loop edge case) — bail, not stuck
    songPos = (segLen >= available - 1e-9 && segWraps) ? activeLoopStart : segStart + segLen;
  }

  std::sort(due.begin(), due.end(),
            [](const DueEvent& a, const DueEvent& b) { return a.sampleOffset < b.sampleOffset; });
  return due;
}


double Scheduler::CurrentPosition(double hostPpqPos, double loopStartBeat, double loopEndBeat) const {
  std::shared_ptr<const FullPattern> pattern = SnapshotPattern();
  if (!pattern || pattern->totalBeats <= 0.0) return hostPpqPos; // nothing to wrap against yet

  // Same derivation as songPos in Advance() (a position before the loop start is never wrapped).
  bool hasLoop = loopStartBeat >= 0.0;
  double activeLoopStart = 0.0;
  double activeLoopEnd   = 0.0;
  double loopLen = 0.0;
  if (hasLoop) {
    activeLoopStart = loopStartBeat;
    activeLoopEnd   = loopEndBeat;
    loopLen = activeLoopEnd - activeLoopStart;
  }
  bool hasValidLoop = hasLoop && loopLen > 1e-9;

  double songPos = hostPpqPos;
  if (hasValidLoop && hostPpqPos >= activeLoopStart) {
    songPos = activeLoopStart + PositiveMod(hostPpqPos - activeLoopStart, loopLen);
  }
  return songPos;
}

}  // namespace dgg
