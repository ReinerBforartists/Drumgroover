// The noise source of the 909 circuit exists once (IC33 -> NOISE net, one LFSR for all voices); here is its clock: the sample position of the trigger of the voice that is being started, counted by the host
// (ProcessBlock) from the start of playback. A hit reads the shared stream (pcmfast::HitNoise::StartAt) at the knot that belongs to that position, so voices triggered at the same time see the same noise and voices
// triggered at different times see different parts of the same stream. Audio thread only.
#pragma once
#include <cmath>
#include <cstdint>

namespace dgg {

struct Tr909NoiseStream
{
  static uint64_t& Sample() { static uint64_t s = 0; return s; }   // sample position of the trigger being handled
  static void SetTriggerSample(uint64_t s) { Sample() = s; }
  // Knot of the stream (5 us spacing) of the trigger. The hit's noise starts 50 ms before the trigger, the stream's time axis is shifted by the same 50 ms, so the knot is simply t / 5 us.
  static uint64_t TriggerKnot(double sampleRateHz) { return static_cast<uint64_t>(std::llround(static_cast<double>(Sample()) / sampleRateHz / 5e-6)); }
};

}  // namespace dgg
