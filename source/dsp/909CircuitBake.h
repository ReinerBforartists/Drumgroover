// TR-909 bake engine: the complete circuit first, the per-voice models where the circuit engine does not (yet) cover the request.
//
// Tr909CircuitRunner (tr909/CircuitRunner.h) renders a hit with the whole Nava circuit in one network. It covers BD, SD, LT, MT, HT, HC at every pot state with the 5 us noise path. A request outside of that
// (20 us noise path, PCM voices) is rendered by the per-voice model runner as before; the number of such requests is counted and logged so that the replacement can be completed voice by voice.
// DGG_909_PER_VOICE=1 (environment) selects the per-voice models for everything (diagnosis only).
#pragma once
#include "909ModelRunner.h"
#include "tr909/CircuitRunner.h"
#include "BakeAbort.h"
#include <cstdlib>
#include <functional>

namespace dgg {

class Tr909BakeRunner : public Tr909ModelRunner
{
public:
  // log: receives one line when a request is rendered by the per-voice models although the circuit engine exists for the voice (noise path).
  explicit Tr909BakeRunner(std::function<void(const std::string&)> log = nullptr) : circuit_(1e-6, []() { return BakeAborted(); }), log_(std::move(log)), perVoiceOnly_(std::getenv("DGG_909_PER_VOICE") != nullptr) {}

  bool Run(const Tr909RunRequest& req, std::vector<float>& out) override
  {
    if (!perVoiceOnly_ && Tr909CircuitRunner::Supports(req.voice))
    {
      if (req.noise == Tr909NoisePath::Reference5us) return circuit_.Run(req, out);
      if (log_ && !logged_) { logged_ = true; log_("909 bake: voice " + std::to_string(static_cast<int>(req.voice)) + " with a noise path outside the circuit engine: per-voice model"); }
    }
    return perVoice_.Run(req, out);
  }
  int GroupSize(Tr909Voice v) const override { return (!perVoiceOnly_ && Tr909CircuitRunner::Supports(v)) ? 1 : perVoice_.GroupSize(v); }
  bool RunGroup(const Tr909RunRequest& req, const uint64_t* seeds, int n, std::vector<float>* out) override
  {
    if (!perVoiceOnly_ && Tr909CircuitRunner::Supports(req.voice) && req.noise == Tr909NoisePath::Reference5us) return false;   // the circuit renders single hits
    return perVoice_.RunGroup(req, seeds, n, out);
  }

private:
  Tr909CircuitRunner circuit_; Tr909FullModelRunner perVoice_; std::function<void(const std::string&)> log_; bool perVoiceOnly_; bool logged_ = false;
};

}  // namespace dgg
