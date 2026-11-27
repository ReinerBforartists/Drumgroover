// Entry of the PCM voices (Closed Hat, Open Hat, Crash) computed by the AVX2 unit PcmHatAvx2.cpp: the same model and solver as pcmfast::PcmVoiceHost, with AVX2/FMA matrix and dot products (intrinsics only, the unit itself
// is compiled for the baseline instruction set). It lives in its own namespace and is used only after the run-time check LanesAvailable909(). Without MSVC x64 (or with DGG_NO_LANES)
// Create gives nullptr and the voices use the baseline implementation. (clang-cl would need the target attribute on the intrinsic functions: baseline there.)
#pragma once
#if defined(_MSC_VER) && !defined(__clang__) && defined(_M_X64) && !defined(DGG_NO_LANES)
#include <intrin.h>
#endif

namespace dgg {

#if defined(_MSC_VER) && !defined(__clang__) && defined(_M_X64) && !defined(DGG_NO_LANES)
// True when the CPU and the operating system support AVX2 and FMA (checked without executing any AVX instruction).
inline bool LanesAvailable909()
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
#else
inline bool LanesAvailable909() { return false; }
#endif

class PcmAvx2909
{
public:
  virtual ~PcmAvx2909() = default;
  // As PcmSlot::Prepare: pot = Decay pot (0..1) of a hat or Tune pot of the Crash, level = Level pot, accentVolts = accent input; builds the tables when the sample rate or the PCM clock changes (not on the audio
  // thread of a hit that is already playing).
  virtual void Prepare(int sampleRateHz, double pot, double level, double accentVolts) = 0;
  virtual void Trigger() = 0;
  virtual void Render(float* out, int n) = 0;
  virtual bool Active() const = 0;
};

#if defined(_MSC_VER) && !defined(__clang__) && defined(_M_X64) && !defined(DGG_NO_LANES)
PcmAvx2909* HatAvx2Create909(bool open);   // open: Open Hat, else Closed Hat; the caller deletes it
PcmAvx2909* CrashAvx2Create909();          // the caller deletes it
#else
inline PcmAvx2909* HatAvx2Create909(bool) { return nullptr; }
inline PcmAvx2909* CrashAvx2Create909() { return nullptr; }
#endif

}  // namespace dgg
