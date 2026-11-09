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
    noise_.Start(0, 5e-6, 0.0); noise_.Reserve(static_cast<size_t>((tu_.linNoiseT + 0.02) / 5e-6) + 64);
    dec_.Init(fsHost); ready_ = true; return true;
  }
  bool Ready() const { return ready_; }
  size_t Epochs() const { return runner_.replay_.Epochs(); }
  uint64_t Key() const { return key_; }
  double SampleRate() const { return fs_; }
  double Seconds() const { return seconds_; }
  size_t Bytes() const { return base_.capacity() * 4 + (runner_.replay_.tE.capacity() + runner_.replay_.pE.capacity()) * 8 + runner_.replay_.EpochBytes() + sizeof(*this); }

  // Audio thread: starts a hit with the noise sequence `seed`.
  void Begin(uint64_t seed)
  {
    noise_.Start(seed, 5e-6, 0.0); runner_.replay_.Start(noise_, Runner::StartTime(), tu_.linNoiseT + 0.01); dec_.Restart(0.0); pos_ = 0; replayOn_ = true;
  }
  // Audio thread: up to n samples of the hit at the host rate; fewer at the end of the hit.
  int Pull(float* out, int n)
  {
    int w = 0; double s;
    while (w < n && pos_ < base_.size())
    {
      float r = 0.0f;
      if (replayOn_)
      {
        while (!dec_.Pop(s)) { double t, dy; if (!runner_.replay_.Next(t, dy)) { replayOn_ = false; break; } dec_.Push(t, dy); s = 0.0; }
        if (replayOn_) r = static_cast<float>(s);
      }
      out[w++] = base_[pos_++] + r;
    }
    return w;
  }

private:
  Runner runner_; PotSet<Mdl, Pot> potSet_; HitTuning tu_; uint64_t key_ = 0; double fs_ = 0.0, seconds_ = 0.0; std::vector<float> base_; HitNoise noise_; HostDecimator dec_; size_t pos_ = 0; bool replayOn_ = false, ready_ = false;
};

}}  // namespace dgg::pcmfast
