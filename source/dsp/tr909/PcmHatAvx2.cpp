// Hi-hat voices of the 909 (Closed / Open) with AVX2/FMA dot products (see PcmHatAvx2.h). This unit is compiled with /arch:AVX2 and everything it includes lives in the namespace dgg_hat (the macro below), so the
// AVX2 code of its copy of PcmFast / PcmVoiceHost is never shared with the rest of the plugin.
#include "PcmHatAvx2.h"
#if defined(_MSC_VER) && defined(_M_X64) && !defined(DGG_NO_LANES)
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <type_traits>
#include <vector>
#include <immintrin.h>
#define PCMFAST_DOT_AVX2
#define dgg dgg_hat
#include "PcmVoiceHost.h"
#include "models/hh_pcm.h"
#include "models/hho_pcm.h"
#undef dgg

namespace dgg {
namespace {

template <class M, bool Open>
class HatImpl final : public Tr909HatAvx2
{
public:
  void Prepare(int fs, double decay, double level, double accentVolts) override
  {
    if (hostFs != fs) { host.Init(static_cast<double>(fs)); hostFs = fs; host.SetPeriod(M::kPeriod); }
    const double x = std::max(decay, 0.001), xn = 0.5;   // the other decay resistor stays at its nominal position
    const double g21 = Open ? 1.0 / (100e3 * xn) : 1.0 / (100e3 * x), g23 = Open ? 1.0 / (1e6 * std::pow(x, 2.74)) : 1.0 / (1e6 * std::pow(xn, 2.74));
    host.voice.SetResG(0, g21); host.voice.SetResG(1, g23);
    host.SetHatMode(true); host.SetAccent(accentVolts); host.SetLevel(level);
  }
  void Trigger() override { host.Trigger(); }
  void Render(float* out, int n) override { host.Render(out, n); }
  bool Active() const override { return host.Active(); }
private:
  dgg_hat::pcmfast::PcmVoiceHost<M> host; int hostFs = 0;
};

}  // namespace

Tr909HatAvx2* Tr909HatAvx2Create(bool open)
{
  if (open) return new HatImpl<HhoModelA0, true>();
  return new HatImpl<HhModelA0, false>();
}

}  // namespace dgg
#endif
