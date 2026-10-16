// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Reiner Prokein
#pragma once

// PreviewScheduler — internal playback for the plugin's own Play button.
// Stateful counterpart to Scheduler: it owns and advances its own beat
// position, independent of host transport. BPM is taken from the host
// on every Advance() call to stay in sync with live tempo changes.

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include "Scheduler.h"  // reuses DueEvent
#include "Types.h"

namespace dgg {

class PreviewScheduler {
public:
  // Replaces the pattern to preview‑play.
  void SetPattern(FullPattern pattern);

  // Starts playback at startBeat with the given loop range.
  // If loopEndBeat <= loopStartBeat, the whole pattern loops (0..totalBeats).
  void Start(double startBeat, double loopStartBeat, double loopEndBeat);

  // Stops playback; CurrentBeat() freezes at the stop position.
  void Stop();

  bool   IsPlaying() const { return mPlaying.load(std::memory_order_relaxed); }
  double CurrentBeat() const { return mCurrentBeat.load(std::memory_order_relaxed); }

  // Called once per ProcessBlock. Returns events due in this block.
  // bpm: current host tempo, passed fresh so tempo changes take effect immediately.
  // Returns empty if not playing.
  std::vector<DueEvent> Advance(double bpm, int nFrames, double sampleRate);

private:
  std::shared_ptr<const FullPattern> SnapshotPattern() const;

  std::shared_ptr<const FullPattern> mPattern;
  mutable std::mutex                 mPatternMutex;

  std::atomic<bool>   mPlaying{false};
  std::atomic<double> mCurrentBeat{0.0};
  std::atomic<double> mLoopStartBeat{0.0};
  std::atomic<double> mLoopEndBeat{0.0};
};

} // namespace dgg