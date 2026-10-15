#pragma once
#include "808VariantPoolCache.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

// The per-instrument singleton instantiations of
// TR808VariantPoolCache (see that file for the design rationale). Kept
// separate from 808VariantPoolCache.h so that header stays a generic,
// instrument-agnostic building block.
namespace dgg
{

// Generic 128-bit key for variant pools whose parameters do not fit in a
// uint64_t (Ride/HiHat need pitch in the key). Every variant is baked at its own
// pitch; the pool never blends between keys, it plays back one exact, whole recording.
struct TR808VariantKey128
{
  uint64_t lo = 0, hi = 0;
  bool operator==(const TR808VariantKey128& o) const { return lo == o.lo && hi == o.hi; }
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

struct TR808VariantKey128Hash
{
  size_t operator()(const TR808VariantKey128& k) const
  {
    uint64_t h = k.lo * 0x9E3779B97F4A7C15ULL + k.hi;
    h ^= h >> 33;
    return (size_t)h;
  }
};

class TR808RideVariantPool : public TR808VariantPoolCache<TR808VariantKey128, TR808VariantKey128Hash>
{
public:
  static TR808RideVariantPool& Instance() { static TR808RideVariantPool inst; return inst; }
};

class TR808HiHatVariantPool : public TR808VariantPoolCache<TR808VariantKey128, TR808VariantKey128Hash>
{
public:
  static TR808HiHatVariantPool& Instance() { static TR808HiHatVariantPool inst; return inst; }
};

}  // namespace dgg
