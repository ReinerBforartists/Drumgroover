// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Reiner Prokein
#pragma once
// Audio-thread counter of hits that found no baked variant while a voice was in cache-only mode (silent instead of
// falling back to a reference recording or the live simulation). DrumGroover::TriggerGmNote() compares it before and
// after a trigger to learn that the note missed; the cache then loads or bakes that key from OnIdle().
#include <atomic>
#include <cstdint>

namespace dgg {

inline std::atomic<uint32_t>& CacheMissSeq()
{
  static std::atomic<uint32_t> seq{0};
  return seq;
}

}  // namespace dgg
