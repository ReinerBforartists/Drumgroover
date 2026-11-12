// Entry of the hi-hat voices (Closed / Open) computed by the AVX2 unit PcmHatAvx2.cpp: the same model and solver as pcmfast::PcmVoiceHost, with the AVX2/FMA dot products. The unit is compiled with /arch:AVX2 and
// lives in its own namespace (256-bit code must not be mixed with legacy-SSE code); it is used only after the run-time check Tr909LanesAvailable(). Without MSVC x64 (or with DGG_NO_LANES)
// Create gives nullptr and the voices use the baseline implementation.
#pragma once
#if defined(_MSC_VER) && defined(_M_X64) && !defined(DGG_NO_LANES)
#include <intrin.h>
#endif

namespace dgg {

#if defined(_MSC_VER) && defined(_M_X64) && !defined(DGG_NO_LANES)
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
#else
inline bool Tr909LanesAvailable() { return false; }
#endif

class Tr909HatAvx2
{
public:
  virtual ~Tr909HatAvx2() = default;
  // As PcmSlot::Prepare of a hat: decay = Decay pot (0..1), level = Level pot (VR22), accentVolts = accent input; builds the tables when the sample rate changes (not on the audio thread).
  virtual void Prepare(int sampleRateHz, double decay, double level, double accentVolts) = 0;
  virtual void Trigger() = 0;
  virtual void Render(float* out, int n) = 0;
  virtual bool Active() const = 0;
};

#if defined(_MSC_VER) && defined(_M_X64) && !defined(DGG_NO_LANES)
Tr909HatAvx2* Tr909HatAvx2Create(bool open);   // open: Open Hat, else Closed Hat; the caller deletes it
#else
inline Tr909HatAvx2* Tr909HatAvx2Create(bool) { return nullptr; }
#endif

}  // namespace dgg
