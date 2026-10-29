// TR-909 fast voices: what a bake of one cache set does, independent of the model that renders a single hit.
//
// The model side (FullFast2 voices, one header per voice and accent anchor) is a Tr909ModelRunner: it renders ONE hit at an anchor accent with a
// given noise seed and noise path and returns the samples at the requested rate. This file decides which runs a set needs (909AccentPlan.h),
// calls the runner with the same seed for every run, and blends the results. A set whose accent is not validated is refused before any
// model run is made.
#pragma once
#include "909AccentPlan.h"
#include <vector>

namespace dgg {

struct Tr909RunRequest
{
  Tr909Voice voice = Tr909Voice::Bd;
  uint64_t params = 0;          // quantised knob state of the voice (same value as in the set key)
  int anchorCode = 0;           // accent anchor the model is generated for
  Tr909NoisePath noise = Tr909NoisePath::Reference5us;
  uint64_t noiseSeed = 1;
  int sampleRateHz = 48000;
  double seconds = 0.35;        // length of the rendered hit
};

class Tr909ModelRunner
{
public:
  virtual ~Tr909ModelRunner() = default;
  // Renders one hit; false when the model cannot be run (no model for this voice/anchor, solver failure).
  virtual bool Run(const Tr909RunRequest& req, std::vector<float>& out) = 0;
};

// Streaming side of a model runner: one hit at a time, produced block by block while it plays.
class Tr909LiveSource
{
public:
  virtual ~Tr909LiveSource() = default;
  // Builds the models of the hit (propagators, about a second per model) without running it; never on the audio thread.
  virtual bool Warm(Tr909Voice voice, int accentCode, Tr909NoisePath noise) = 0;
  // Starts a hit; false when the accent has no validated model.
  virtual bool Start(Tr909Voice voice, int accentCode, Tr909NoisePath noise, uint64_t noiseSeed, int sampleRateHz, double seconds) = 0;
  // Writes the next samples of the running hit; fewer than n only at its end.
  virtual int Pull(float* out, int n) = 0;
  virtual bool Active() const = 0;
};

enum class Tr909BakeResult { Ok, AccentNotValidated, RunFailed };

// Bakes variant k (noise seed = k) of the set `key`. `out` is empty unless the result is Ok.
inline Tr909BakeResult Tr909BakeVariant(const Tr909SetKey& key, int variantIndex, Tr909ModelRunner& runner, std::vector<float>& out)
{
  out.clear();
  Tr909AccentRun runs[2];
  const int n = Tr909AccentRuns(key.voice, key.accentCode, runs);
  if (n == 0) return Tr909BakeResult::AccentNotValidated;
  std::vector<float> y[2];
  for (int i = 0; i < n; ++i)
  {
    Tr909RunRequest r; r.voice = key.voice; r.params = key.params; r.anchorCode = runs[i].anchorCode; r.noise = key.noise;
    r.noiseSeed = Tr909NoiseSeed(variantIndex); r.sampleRateHz = key.sampleRateHz;
    if (!runner.Run(r, y[i]) || y[i].empty()) return Tr909BakeResult::RunFailed;
  }
  if (n == 1) { out = std::move(y[0]); return Tr909BakeResult::Ok; }
  // the shorter run is continued with silence (a hit that has already decayed)
  const size_t len = std::max(y[0].size(), y[1].size());
  out.assign(len, 0.0f);
  for (size_t i = 0; i < len; ++i)
  {
    const double a = i < y[0].size() ? y[0][i] : 0.0, b = i < y[1].size() ? y[1][i] : 0.0;
    out[i] = static_cast<float>(runs[0].weight * a + runs[1].weight * b);
  }
  return Tr909BakeResult::Ok;
}

}  // namespace dgg
