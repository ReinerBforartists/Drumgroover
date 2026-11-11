// Live form of a voice in the linear-response mode (HitTuning::linNoiseT >= 0): everything that does not depend on the noise sequence is prepared once per parameter set on a non-audio thread, a hit on the
// audio thread only adds the response to the noise (HitReplay.h, streamed knot by knot) to the prepared noise-free output.
//
// Prepared once (Prepare(), about 0.5 s, allocates): the pot dependent model, the noise-free run of the whole hit with its port voltages, the noise-free output at the host rate, the replay tables.
// Per hit (Begin() / Pull(), audio thread, no allocation): the noise of the hit, the response at the 5 us knots of the first linNoiseT + 10 ms, its decimation to the host rate, the sum with the prepared output.
// Lifetime and sharing: one LiveLin belongs to one parameter set (voice, anchor, pot state, sample rate, hit length, tuning) and is immutable except for the state of the hit in progress (one hit at a time: the
// BD is monophonic, a new hit restarts it). The owner replaces it as a whole (std::shared_ptr) when the parameter set changes; a hit in progress keeps the old object alive. It owns its pot arrays.
#pragma once
#include <cstdint>
#include <memory>
#include <vector>
#include "HitRunner.h"
#include "PotSet.h"

namespace dgg { namespace pcmfast {

template <class Mdl, class Pot>
class LiveLin
{
public:
  using Runner = HitRunner<Mdl>;
  // pots: the pot positions in the order of the generator, or nullptr for the nominal positions of the model header. key: opaque identifier of the parameter set chosen by the owner.
  bool Prepare(const HitTuning& tu, const double* pots, uint64_t key, double fsHost, double seconds)
  {
    if (tu.linNoiseT < 0.0 || Mdl::kNT != 2) return false;
    tu_ = tu; tu_.detReuse = false; key_ = key; fs_ = fsHost; seconds_ = seconds;
    if (pots) { potSet_.Assemble(pots); runner_.SetPot(potSet_.Ptrs()); } else runner_.ClearPot();
    runner_.Warm(tu_);
    std::vector<double> ts, ys; if (!runner_.RunLinDet(tu_, seconds, ts, ys)) return false;
    base_.clear(); HostDecimator dec; dec.Init(fsHost); const double idle = runner_.idle(); double s; const size_t want = static_cast<size_t>(seconds * fsHost);
    for (size_t i = 0; i < ts.size(); ++i) { dec.Push(ts[i], ys[i] - idle); while (dec.Pop(s)) base_.push_back(static_cast<float>(s)); }
    while (dec.Pop(s)) base_.push_back(static_cast<float>(s)); if (base_.size() > want) base_.resize(want); base_.resize(want, 0.0f);
    hit0_ = NewHit(); ready_ = true; return true;
  }
  bool Ready() const { return ready_; }
  size_t Epochs() const { return runner_.replay_.Epochs(); }
  uint64_t Key() const { return key_; }
  double SampleRate() const { return fs_; }
  double Seconds() const { return seconds_; }
  size_t Bytes() const { return base_.capacity() * 4 + (runner_.replay_.tE.capacity() + runner_.replay_.pE.capacity()) * 8 + runner_.replay_.EpochBytes() + sizeof(*this); }

  // State of one hit (the prepared tables of this object are not modified by a hit): several hits can run at the same time on one prepared object, each with its own HitState (created off the audio thread, NewHit()).
  struct HitState { HitNoise noise; HostDecimator dec; size_t pos = 0; bool replayOn = false; typename NoiseReplay<Mdl>::Cursor cur; };
  static HitState MakeHit(const HitTuning& tu, double fsHost) { HitState h; h.noise.Start(0, 5e-6, 0.0); h.noise.Reserve(static_cast<size_t>((tu.linNoiseT + 0.02) / 5e-6) + 64); h.dec.Init(fsHost); return h; }   // allocates: not on the audio thread
  HitState NewHit() const { return MakeHit(tu_, fs_); }
  // Audio thread: starts a hit with the noise sequence `seed`, or (streamKnot != HitNoise::kNoStream) at the position of the shared noise stream.
  void Begin(HitState& h, uint64_t seed, uint64_t streamKnot = HitNoise::kNoStream) const
  {
    // the shared noise stream at the trigger position, or the hit's own sequence
    if (streamKnot == HitNoise::kNoStream) h.noise.Start(seed, 5e-6, 0.0); else h.noise.StartAt(HitNoise::kStreamSeed, streamKnot, 5e-6, 0.0);
    if (tu_.linBaseNoisy) h.noise.SetBase(&runner_.baseNz_);
    runner_.replay_.Start(h.cur, h.noise, Runner::StartTime(), tu_.linNoiseT + 0.01); h.dec.Restart(0.0); h.pos = 0; h.replayOn = true;
  }
  // Audio thread: up to n samples of the hit at the host rate; fewer at the end of the hit.
  int Pull(HitState& h, float* out, int n) const
  {
    int w = 0; double s;
    while (w < n && h.pos < base_.size())
    {
      float r = 0.0f;
      if (h.replayOn)
      {
        while (!h.dec.Pop(s)) { double t, dy; if (!runner_.replay_.Next(h.cur, t, dy)) { h.replayOn = false; break; } h.dec.Push(t, dy); s = 0.0; }
        if (h.replayOn) r = static_cast<float>(s);
      }
      out[w++] = base_[h.pos++] + r;
    }
    return w;
  }
  // The single-hit interface (one hit at a time on this object)
  void Begin(uint64_t seed, uint64_t streamKnot = HitNoise::kNoStream) { Begin(hit0_, seed, streamKnot); }
  int Pull(float* out, int n) { return Pull(hit0_, out, n); }

private:
  mutable Runner runner_; PotSet<Mdl, Pot> potSet_; HitTuning tu_; uint64_t key_ = 0; double fs_ = 0.0, seconds_ = 0.0; std::vector<float> base_; HitState hit0_; bool ready_ = false;
};

}}  // namespace dgg::pcmfast
