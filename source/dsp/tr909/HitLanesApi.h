// Entry of the lane engine for the cache bake (tr909/HitLanes.h, tools handoff R392). The engine lives in its own translation unit (HitLanesLT.cpp), compiled with /arch:AVX2: the 256-bit code
// must not be mixed with legacy-SSE code (state transitions make it nine times slower), and that unit is in its own namespace so that none of its AVX2-compiled inline functions can replace the
// ones of the rest of the plugin. Without MSVC x64 (or with DGG_NO_LANES) the entries are stubs that refuse, and the bake renders hit by hit.
#pragma once
#include <cstdint>
#include <vector>
#if defined(_MSC_VER) && defined(_M_X64) && !defined(DGG_NO_LANES)
#include <intrin.h>
namespace dgg {

// True when the CPU and the operating system support AVX2 and FMA (checked without executing any AVX instruction).
inline bool Tr909LanesAvailable()
{
  static const bool ok = []
  {
    int r0[4]; __cpuid(r0, 0); if (r0[0] < 7) return false;
    int r1[4]; __cpuid(r1, 1);
    if ((r1[2] & (1 << 27)) == 0 || (r1[2] & (1 << 28)) == 0 || (r1[2] & (1 << 12)) == 0) return false;   // OSXSAVE, AVX, FMA3
    if ((_xgetbv(0) & 6) != 6) return false;
    int r7[4]; __cpuidex(r7, 7, 0); return (r7[1] & (1 << 5)) != 0;   // AVX2
  }();
  return ok;
}
// Renders n (1..4) hits of the LT model at anchor index anchorIdx (0..4 for the codes 25 / 50 / 75 / 100 / 127) with the 5 us reference noise at once; out[i] for seeds[i] at fs. aborted: the bake abort
// test of the caller (polled every few hundred steps). False: not rendered (solver failure, abort, unsupported) - the caller renders the hits one by one. Call only when Tr909LanesAvailable().
bool Tr909LaneRenderLT(int anchorIdx, const uint64_t* seeds, int n, double seconds, double fs, std::vector<float>* out, bool (*aborted)());
}  // namespace dgg
#else
namespace dgg {
inline bool Tr909LanesAvailable() { return false; }
inline bool Tr909LaneRenderLT(int, const uint64_t*, int, double, double, std::vector<float>*, bool (*)()) { return false; }
}  // namespace dgg
#endif