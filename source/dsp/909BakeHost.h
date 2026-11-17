// TR-909 fast voices: what a bake of one cache set does, independent of the model that renders a single hit.
//
// The model side (circuit runner, PCM voices) is a ModelRunner909: it renders ONE hit at an anchor accent with a
// given noise seed and noise path and returns the samples at the requested rate. This file decides which runs a set needs (909AccentPlan.h),
// calls the runner with the same seed for every run, and blends the results. A set whose accent is not validated is refused before any
// model run is made.
#pragma once
#include "909AccentPlan.h"
#include <vector>

namespace dgg {

struct RunRequest909
{
  Voice909 voice = Voice909::Bd;
  uint64_t params = 0;          // quantised knob state of the voice (same value as in the set key)
  int anchorCode = 0;           // accent anchor the model is generated for
  NoisePath909 noise = NoisePath909::Reference5us;
  uint64_t noiseSeed = 1;
  int sampleRateHz = 48000;
  double seconds = 0.35;        // length of the rendered hit
};

class ModelRunner909
{
public:
  virtual ~ModelRunner909() = default;
  // Renders one hit; false when the model cannot be run (no model for this voice/anchor, solver failure).
  virtual bool Run(const RunRequest909& req, std::vector<float>& out) = 0;
  // Number of hits of one model that RunGroup renders at once for this voice on this machine (1: no group engine).
  virtual int GroupSize(Voice909 voice) const { (void)voice; return 1; }
  // Renders the hits with the noise seeds seeds[0..n-1] (n <= GroupSize) at once, out[i] for seed i; false when the group engine is not available or failed (the caller renders the hits one by one).
  virtual bool RunGroup(const RunRequest909& req, const uint64_t* seeds, int n, std::vector<float>* out) { (void)req; (void)seeds; (void)n; (void)out; return false; }
};

// Streaming side of a model runner: one hit at a time, produced block by block while it plays.
class LiveSource909
{
public:
  virtual ~LiveSource909() = default;
  // params: pot state of the hit (909PotParams.h, 0 = nominal). Builds the models of the hit (propagators, about a second per model) without running it; never on the audio thread.
  virtual bool Warm(Voice909 voice, int accentCode, NoisePath909 noise, uint64_t params = 0) = 0;
  // Starts a hit; false when the accent has no validated model.
  virtual bool Start(Voice909 voice, int accentCode, NoisePath909 noise, uint64_t noiseSeed, int sampleRateHz, double seconds, uint64_t params = 0) = 0;
  // Writes the next samples of the running hit; fewer than n only at its end.
  virtual int Pull(float* out, int n) = 0;
  virtual bool Active() const = 0;
  // Position of the trigger in the shared noise stream (knot, HitNoise::kNoStream: none) for the next Start(); default: ignored.
  virtual void SetStreamKnot(uint64_t knot) { (void)knot; }
  // Builds the tables a first hit of the PCM voices would build (default: nothing); never on the audio thread.
  virtual void Prebuild(int sampleRateHz) { (void)sampleRateHz; }
};

enum class BakeResult909 { Ok, AccentNotValidated, RunFailed };

// Bakes variant k (noise seed = k) of the set `key`. `out` is empty unless the result is Ok.
inline BakeResult909 BakeVariant909(const SetKey909& key, int variantIndex, ModelRunner909& runner, std::vector<float>& out)
{
  out.clear();
  AccentRun909 runs[2];
  const int n = AccentRuns909(key.voice, key.accentCode, runs);
  if (n == 0) return BakeResult909::AccentNotValidated;
  std::vector<float> y[2];
  for (int i = 0; i < n; ++i)
  {
    RunRequest909 r; r.voice = key.voice; r.params = key.params; r.anchorCode = runs[i].anchorCode; r.noise = key.noise;
    r.noiseSeed = NoiseSeed909(variantIndex); r.sampleRateHz = key.sampleRateHz; r.seconds = HitSeconds909(key.voice);
    if (!runner.Run(r, y[i]) || y[i].empty()) return BakeResult909::RunFailed;
  }
  if (n == 1) { out = std::move(y[0]); return BakeResult909::Ok; }
  // the shorter run is continued with silence (a hit that has already decayed)
  const size_t len = std::max(y[0].size(), y[1].size());
  out.assign(len, 0.0f);
  for (size_t i = 0; i < len; ++i)
  {
    const double a = i < y[0].size() ? y[0][i] : 0.0, b = i < y[1].size() ? y[1][i] : 0.0;
    out[i] = static_cast<float>(runs[0].weight * a + runs[1].weight * b);
  }
  return BakeResult909::Ok;
}

// Bakes the variants variantIndex[0..n-1] of the set `key` (n <= 4); outs[i] / results[i] as for BakeVariant909. Runs the group engine of the runner when it has one for the voice
// and falls back to single hits otherwise or when the group engine refuses (failure, stop request), so a group never delivers less than its single hits would.
inline void BakeVariantGroup909(const SetKey909& key, const int* variantIndex, int n, ModelRunner909& runner, std::vector<float>* outs, BakeResult909* results)
{
  for (int i = 0; i < n; ++i) { outs[i].clear(); results[i] = BakeResult909::RunFailed; }
  AccentRun909 runs[2];
  const int nr = AccentRuns909(key.voice, key.accentCode, runs);
  if (nr == 0) { for (int i = 0; i < n; ++i) results[i] = BakeResult909::AccentNotValidated; return; }
  bool grouped = n > 1 && n <= runner.GroupSize(key.voice);
  std::vector<float> y[2][4];
  for (int r = 0; grouped && r < nr; ++r)
  {
    RunRequest909 rq; rq.voice = key.voice; rq.params = key.params; rq.anchorCode = runs[r].anchorCode; rq.noise = key.noise; rq.sampleRateHz = key.sampleRateHz; rq.seconds = HitSeconds909(key.voice);
    uint64_t seeds[4]; for (int i = 0; i < n; ++i) seeds[i] = NoiseSeed909(variantIndex[i]);
    if (!runner.RunGroup(rq, seeds, n, y[r])) grouped = false;
    for (int i = 0; grouped && i < n; ++i) if (y[r][i].empty()) grouped = false;
  }
  if (!grouped) { for (int i = 0; i < n; ++i) results[i] = BakeVariant909(key, variantIndex[i], runner, outs[i]); return; }
  for (int i = 0; i < n; ++i)
  {
    if (nr == 1) { outs[i] = std::move(y[0][i]); results[i] = BakeResult909::Ok; continue; }
    const size_t len = std::max(y[0][i].size(), y[1][i].size());
    outs[i].assign(len, 0.0f);
    for (size_t s = 0; s < len; ++s)
    {
      const double a = s < y[0][i].size() ? y[0][i][s] : 0.0, b = s < y[1][i].size() ? y[1][i][s] : 0.0;
      outs[i][s] = static_cast<float>(runs[0].weight * a + runs[1].weight * b);
    }
    results[i] = BakeResult909::Ok;
  }
}
}  // namespace dgg
