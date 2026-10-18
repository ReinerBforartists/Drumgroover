// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Reiner Prokein
#pragma once

// PreviewScheduler — internal playback for the plugin's own Play button.
// Stateful counterpart to Scheduler: it owns and advances its own beat
// position, independent of host transport. BPM is taken from the host
// on every Advance() call to stay in sync with live tempo changes.

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "Scheduler.h"  // reuses DueEvent
#include "Types.h"

namespace dgg {

// A velocity changed while the preview keeps running (a velocity gesture in the Note Editor).
struct VelocityEdit {
  std::string trackId;
  double      beatAbs;
  int         vel;
};

class PreviewScheduler {
public:
  // Replaces the pattern to preview‑play.
  void SetPattern(FullPattern pattern);

  // Starts playback at startBeat with the given loop range.
  // If loopEndBeat <= loopStartBeat, the whole pattern loops (0..totalBeats).
  void Start(double startBeat, double loopStartBeat, double loopEndBeat);

  // Stops playback; CurrentBeat() freezes at the stop position.
  void Stop();

  // Move the running playback to another position / loop range; the pattern is untouched.
  void Seek(double beat);
  void SetLoop(double loopStartBeat, double loopEndBeat);

  // Changes the velocity of single notes of the running pattern without replacing it or restarting playback. The changes lie in a
  // small overlay that Advance() consults; the next SetPattern() drops it (the new pattern carries the final velocities).
  void SetVelocities(const std::vector<VelocityEdit>& edits);

  bool   IsPlaying() const { return mPlaying.load(std::memory_order_relaxed); }
  double CurrentBeat() const { return mCurrentBeat.load(std::memory_order_relaxed); }

  // Called once per ProcessBlock. Returns events due in this block.
  // bpm: current host tempo, passed fresh so tempo changes take effect immediately.
  // Returns empty if not playing.
  std::vector<DueEvent> Advance(double bpm, int nFrames, double sampleRate);

private:
  std::shared_ptr<const FullPattern> SnapshotPattern() const;
  static uint64_t VelocityKey(const std::string& trackId, double beatAbs);

  std::shared_ptr<const FullPattern> mPattern;
  mutable std::mutex                 mPatternMutex;

  // Velocity overlay: written by the UI thread, read by the audio thread, which only try-locks and otherwise plays the pattern's own velocity.
  mutable std::mutex                 mVelMutex;
  std::unordered_map<uint64_t, int>  mVelOverride;
  std::atomic<bool>                  mHasVelOverride{false};

  std::atomic<bool>   mPlaying{false};
  std::atomic<double> mCurrentBeat{0.0};
  std::atomic<double> mLoopStartBeat{0.0};
  std::atomic<double> mLoopEndBeat{0.0};
};

} // namespace dgg