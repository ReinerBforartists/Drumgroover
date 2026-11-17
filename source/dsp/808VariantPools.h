// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Reiner Prokein
#pragma once
#include "808VariantPoolCache.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

// The per-instrument singleton instantiations of
// VariantPoolCache808 (see that file for the design rationale). Kept
// separate from 808VariantPoolCache.h so that header stays a generic,
// instrument-agnostic building block.
namespace dgg
{

// Generic 128-bit key for variant pools whose parameters do not fit in a
// uint64_t (Ride/HiHat need pitch in the key). Every variant is baked at its own
// pitch; the pool never blends between keys, it plays back one exact, whole recording.
struct VariantKey128808
{
  uint64_t lo = 0, hi = 0;
  bool operator==(const VariantKey128808& o) const { return lo == o.lo && hi == o.hi; }
};

// Every knob in a variant-pool key is quantized to the UI's own 0.01 grid
// (all Ride/HiHat/Cymbal knobs step by 0.01, Pitch 0.67..1.50), so a key --
// and the cache file name derived from it -- distinguishes exactly the
// settings a user can actually dial in. Both voices' MakeVariantPoolKey()
// use these, and their name/decode helpers invert them.
constexpr double kVariantPitchMin = 0.25, kVariantPitchMax = 4.0;
inline uint64_t QuantizeVariantKnob01(double x)
{
  return (uint64_t)std::llround(std::clamp(x, 0.0, 1.0) * 100.0);
}
inline uint64_t QuantizeVariantPitch(double x)
{
  return (uint64_t)std::llround(std::clamp(x, kVariantPitchMin, kVariantPitchMax) * 100.0);
}
inline double DequantizeVariant(uint64_t q) { return (double)q / 100.0; }

// Accent of a baked set: the Accent amount (0..1) in steps of 1 % (the steps of the Accent slider) (0 = a normal hit, the key and file name of a set without accent stay as they were). Accented sets are baked on demand
// (the first accented hit that finds no variant plays live and starts the bake), never ahead of time. The step lives in the upper 32 bits of VariantKey128808::hi, the sample rate in the lower.
constexpr int kAccentStepsPerUnit = 100;
inline int QuantizeAccentStep(double accent01) { return (int)std::llround(std::clamp(accent01, 0.0, 1.0) * kAccentStepsPerUnit); }
inline double DequantizeAccentStep(int step) { return std::clamp(step, 0, kAccentStepsPerUnit) / (double)kAccentStepsPerUnit; }
inline uint64_t PackKeyHi(double sampleRateHz, int accentStep) { return (uint64_t)std::llround(sampleRateHz) | ((uint64_t)std::clamp(accentStep, 0, kAccentStepsPerUnit) << 32); }
inline double KeyHiSampleRate(uint64_t hi) { return (double)(hi & 0xFFFFFFFFull); }
inline int KeyHiAccentStep(uint64_t hi) { return (int)(hi >> 32); }

struct VariantKey128Hash808
{
  size_t operator()(const VariantKey128808& k) const
  {
    uint64_t h = k.lo * 0x9E3779B97F4A7C15ULL + k.hi;
    h ^= h >> 33;
    return (size_t)h;
  }
};

class RideVariantPool808 : public VariantPoolCache808<VariantKey128808, VariantKey128Hash808>
{
public:
  static RideVariantPool808& Instance() { static RideVariantPool808 inst; return inst; }
};

class HiHatVariantPool808 : public VariantPoolCache808<VariantKey128808, VariantKey128Hash808>
{
public:
  static HiHatVariantPool808& Instance() { static HiHatVariantPool808 inst; return inst; }
};

}  // namespace dgg
