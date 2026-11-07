// Lane engine of the HT toms (see HitLanesApi.h). This unit is compiled with /arch:AVX2 and everything it includes lives in the namespace dgg_lane (the macro below), so the AVX2 code
// of its copy of FullFast2 and the helpers is never shared with the rest of the plugin.
#include "HitLanesApi.h"
#if defined(_MSC_VER) && defined(_M_X64) && !defined(DGG_NO_LANES)
#include <atomic>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>
#include <immintrin.h>
#include <intrin.h>
#define dgg dgg_lane
#define FF2DetectAvx FF2DetectAvx_lane   // the only global (outside namespace dgg) inline functions / variables of the included headers
#define g_ff2Avx g_ff2Avx_lane
#define g_ff2DotMin g_ff2DotMin_lane
#include "HitLanes.h"
#include "models/ht_a25.h"
#include "models/ht_a50.h"
#include "models/ht_a75.h"
#include "models/ht_a100.h"
#include "models/ht_a127.h"
#undef dgg
#undef FF2DetectAvx
#undef g_ff2Avx
#undef g_ff2DotMin

namespace dgg {
namespace {
template <class Mdl>
bool RenderModel(const uint64_t* seeds, int n, double seconds, double fs, std::vector<float>* out, bool (*aborted)(), const Tr909LanePot* pot)
{
  thread_local dgg_lane::pcmfast::HitLaneRunner<Mdl> runner;   // one engine per worker thread and model: its propagators are built once
  thread_local uint64_t potId = 0;   // set currently installed in this thread's runner (0: header arrays)
  const uint64_t want = pot ? pot->id : 0;
  if (want != potId)
  {
    if (pot) runner.SetPot({ pot->A, pot->Bg, pot->XzS, pot->DgS, pot->Xo, pot->Dof, pot->Dou, pot->Do1 }); else runner.ClearPot();
    potId = want;
  }
  return runner.RenderGroup(dgg_lane::pcmfast::HitTuningFor(dgg_lane::Tr909Voice::Ht, false), seeds, n, seconds, fs, out, aborted);
}
}  // namespace

bool Tr909LaneRenderHT(int anchorIdx, const uint64_t* seeds, int n, double seconds, double fs, std::vector<float>* out, bool (*aborted)(), const Tr909LanePot* pot)
{
  switch (anchorIdx)
  {
    case 0: return RenderModel<Tr909_HT_A25>(seeds, n, seconds, fs, out, aborted, pot);
    case 1: return RenderModel<Tr909_HT_A50>(seeds, n, seconds, fs, out, aborted, pot);
    case 2: return RenderModel<Tr909_HT_A75>(seeds, n, seconds, fs, out, aborted, pot);
    case 3: return RenderModel<Tr909_HT_A100>(seeds, n, seconds, fs, out, aborted, pot);
    case 4: return RenderModel<Tr909_HT_A127>(seeds, n, seconds, fs, out, aborted, pot);
    default: return false;
  }
}
}  // namespace dgg
#endif