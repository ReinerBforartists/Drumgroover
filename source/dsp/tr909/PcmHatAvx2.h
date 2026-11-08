// Entry of the hi-hat voices (Closed / Open) computed by the AVX2 unit PcmHatAvx2.cpp: the same model and solver as pcmfast::PcmVoiceHost, with the AVX2/FMA dot products. The unit is compiled with /arch:AVX2 and
// lives in its own namespace (256-bit code must not be mixed with legacy-SSE code, see HitLanesApi.h); it is used only after the run-time check Tr909LanesAvailable(). Without MSVC x64 (or with DGG_NO_LANES)
// Create gives nullptr and the voices use the baseline implementation.
#pragma once

namespace dgg {

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
