#pragma once

// Scheduler — real-time, host-synced playback of a compiled FullPattern.
//
// Stateless: Advance() derives the current position from the host's PPQ
// position on every call. Host rewind / reposition / host-loop are handled
// implicitly — there is no internal cursor.
//
// Two loop concepts:
//   - Host transport loop: handled automatically by the stateless design.
//   - DrumGroover's own loop range (loopStartBeat/loopEndBeat): an internal
//     musical loop applied *on top* of the host position. Playback wraps
//     within this range once the host position enters it; positions before
//     loopStartBeat play normally without wrapping.
//
// Beats are in quarter‑note units (Grid.denominator==4), matching iPlug2's
// PPQ position.

#include <memory>
#include <mutex>
#include <vector>

#include "Types.h"

namespace dgg {

// One event due for triggering inside the current ProcessBlock call.
struct DueEvent {
  std::string trackId;
  int         vel;
  int         sampleOffset; // offset within this block (0..nFrames-1)
};

class Scheduler {
public:
  // Replaces the compiled pattern to play. Called from the UI thread when
  // the arrangement changes. Internally guarded by a mutex that only copies
  // a shared_ptr; not held across event search, so it does not block audio.
  void SetPattern(FullPattern pattern);

  // Called once per ProcessBlock, before any Trigger() calls.
  // Returns all events due at their sample offsets, sorted ascending.
  //
  // Parameters:
  //   hostPpqPos          – host PPQ position at the START of the block
  //   bpm                 – current host tempo
  //   transportIsPlaying  – if false, returns empty
  //   nFrames, sampleRate – block size and sample rate
  //   loopStartBeat       – own loop start (beats); < 0 means no loop
  //   loopEndBeat         – own loop end (beats); must be > loopStartBeat if used
  std::vector<DueEvent> Advance(double hostPpqPos, double bpm, bool transportIsPlaying,
                                 int nFrames, double sampleRate,
                                 double loopStartBeat, double loopEndBeat);

  // Returns the song‑space position (wrapped through own loop range) that
  // corresponds to the given host PPQ position. Used for UI cursor feedback.
  // Safe to call from the UI thread; cheap (no event search).
  double CurrentPosition(double hostPpqPos, double loopStartBeat, double loopEndBeat) const;

private:
  // Returns a snapshot of the current pattern under the mutex.
  std::shared_ptr<const FullPattern> SnapshotPattern() const;

  std::shared_ptr<const FullPattern> mPattern;
  mutable std::mutex                 mPatternMutex;
};

} // namespace dgg