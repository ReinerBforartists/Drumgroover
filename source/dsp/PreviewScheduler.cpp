// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Reiner Prokein
#include "PreviewScheduler.h"

#include <algorithm>
#include <cmath>
#include <functional>

namespace dgg {

namespace {
// Always-non-negative modulo (same as in Scheduler.cpp).
double PositiveMod(double a, double n) {
  double r = std::fmod(a, n);
  return r < 0.0 ? r + n : r;
}
}  // namespace

void PreviewScheduler::SetPattern(FullPattern pattern) {
  auto shared = std::make_shared<const FullPattern>(std::move(pattern));
  {
    std::lock_guard<std::mutex> lock(mPatternMutex);
    mPattern.swap(shared);
  }
  {
    std::lock_guard<std::mutex> lock(mVelMutex);
    mVelOverride.clear();
    mHasVelOverride.store(false, std::memory_order_relaxed);
  }
  // The previous pattern is released here, outside the lock, so the audio thread never waits for its deallocation.
}

uint64_t PreviewScheduler::VelocityKey(const std::string& trackId, double beatAbs) {
  const uint64_t beatKey = static_cast<uint64_t>(std::llround(beatAbs * 1.0e6));
  return std::hash<std::string>{}(trackId) ^ (beatKey * 0x9E3779B97F4A7C15ULL);
}

void PreviewScheduler::SetVelocities(const std::vector<VelocityEdit>& edits) {
  if (edits.empty()) return;
  std::lock_guard<std::mutex> lock(mVelMutex);
  for (const auto& e : edits) mVelOverride[VelocityKey(e.trackId, e.beatAbs)] = e.vel;
  mHasVelOverride.store(true, std::memory_order_relaxed);
}

std::shared_ptr<const FullPattern> PreviewScheduler::SnapshotPattern() const {
  std::lock_guard<std::mutex> lock(mPatternMutex);
  return mPattern;
}

void PreviewScheduler::Start(double startBeat, double loopStartBeat, double loopEndBeat) {
  mLoopStartBeat.store(loopStartBeat, std::memory_order_relaxed);
  mLoopEndBeat.store(loopEndBeat, std::memory_order_relaxed);
  mCurrentBeat.store(std::max(0.0, startBeat), std::memory_order_relaxed);
  // Release: Advance() acquires mPlaying, so once it sees true it also sees the start position and loop bounds stored above.
  mPlaying.store(true, std::memory_order_release);
}

void PreviewScheduler::Stop() {
  mPlaying.store(false, std::memory_order_relaxed);
}

void PreviewScheduler::Seek(double beat) {
  mCurrentBeat.store(std::max(0.0, beat), std::memory_order_relaxed);   // Advance() only persists its position if this value is unchanged
}

void PreviewScheduler::SetLoop(double loopStartBeat, double loopEndBeat) {
  mLoopStartBeat.store(loopStartBeat, std::memory_order_relaxed);
  mLoopEndBeat.store(loopEndBeat, std::memory_order_relaxed);
}

std::vector<DueEvent> PreviewScheduler::Advance(double bpm, int nFrames, double sampleRate) {
  std::vector<DueEvent> due;

  if (!mPlaying.load(std::memory_order_acquire)) return due;
  if (bpm <= 0.0 || nFrames <= 0 || sampleRate <= 0.0) return due; // guards against a div-by-zero below

  std::shared_ptr<const FullPattern> pattern = SnapshotPattern();
  if (!pattern || pattern->events.empty() || pattern->totalBeats <= 0.0) return due;

  // Never wait for the UI thread here: without the lock the pattern's own velocities play for this block.
  std::unique_lock<std::mutex> velLock(mVelMutex, std::try_to_lock);
  const bool useVelOverride = velLock.owns_lock() && mHasVelOverride.load(std::memory_order_relaxed);

  double loopStart = mLoopStartBeat.load(std::memory_order_relaxed);
  double loopEnd    = mLoopEndBeat.load(std::memory_order_relaxed);
  if (loopEnd <= loopStart) { loopStart = 0.0; loopEnd = pattern->totalBeats; } // "no range defined" -> whole pattern, see Start()'s header comment
  const double loopLen = loopEnd - loopStart;

  const double samplesPerBeat = sampleRate * 60.0 / bpm;
  const double blockBeats     = nFrames / samplesPerBeat;

  const double beatAtBlockStart = mCurrentBeat.load(std::memory_order_relaxed);
  double beatPos = beatAtBlockStart;
  const double patternEnd = pattern->totalBeats;
  // A position before loopStart plays through linearly until it reaches loopStart; only then does looping start
  // (same convention as Scheduler::Advance). A position behind the loop's end (Start() puts it there to leave the loop)
  // plays on to the end of the pattern, then the loop takes over again.
  const bool behindLoop = beatPos >= loopEnd && loopEnd < patternEnd;
  if (!behindLoop && beatPos >= loopStart) {
    beatPos = loopStart + PositiveMod(beatPos - loopStart, loopLen);
  }

  const auto& events = pattern->events; // sorted ascending by beatAbs (Song.h/Types.cpp's contract)
  auto beatLess = [](const PatternEvent& ev, double beat) { return ev.beatAbs < beat; };

  double remainingBeats = blockBeats;
  double consumedBeats  = 0.0; // how far into THIS block (in beats) we've already accounted for

  // Splits the block at loop boundaries, as in Scheduler::Advance; loopLen is always valid here.
  while (remainingBeats > 1e-9) {
    double segStart = beatPos;
    double segEnd;
    bool   segWraps;

    if (segStart >= loopEnd && loopEnd < patternEnd) {
      segEnd   = patternEnd;   // behind the loop: on to the end of the pattern, then back to loopStart
      segWraps = true;
    } else if (segStart >= loopStart) {
      segEnd   = loopEnd;
      segWraps = true;
    } else {
      segEnd   = loopStart;
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
      int vel = it->vel;
      if (useVelOverride) {
        const auto found = mVelOverride.find(VelocityKey(it->trackId, it->beatAbs));
        if (found != mVelOverride.end()) vel = found->second;
      }
      due.push_back({it->trackId, vel, sampleOffset});
    }

    consumedBeats  += segLen;
    remainingBeats -= segLen;

    if (available <= 1e-9) break; // degenerate segment (zero-length loop edge case) — bail, not stuck
    beatPos = (segLen >= available - 1e-9 && segWraps) ? loopStart : segStart + segLen;
  }

  // Persist the position for the next block, but only if Start() has not set a new one in the meantime.
  double expected = beatAtBlockStart;
  mCurrentBeat.compare_exchange_strong(expected, beatPos, std::memory_order_relaxed);

  std::sort(due.begin(), due.end(),
            [](const DueEvent& a, const DueEvent& b) { return a.sampleOffset < b.sampleOffset; });
  return due;
}

}  // namespace dgg
