// TR-909 bake engine: the complete circuit for BD, SD, LT, MT, HT and HC, the PCM models for hi-hats, crash and ride.
//
// Tr909CircuitRunner (tr909/CircuitRunner.h) renders a hit with the whole Nava circuit in one network at every pot state with the 5 us noise path; the PCM voices have no circuit and are rendered by Tr909PcmModelRunner.
#pragma once
#include "909ModelRunner.h"
#include "tr909/CircuitRunner.h"
#include "BakeAbort.h"
#include <functional>

namespace dgg {

class Tr909BakeRunner : public Tr909ModelRunner
{
public:
  explicit Tr909BakeRunner(std::function<void(const std::string&)> log = nullptr) : circuit_(1e-6, []() { return BakeAborted(); }), log_(std::move(log)) {}

  bool Run(const Tr909RunRequest& req, std::vector<float>& out) override
  {
    if (Tr909CircuitRunner::Supports(req.voice))
    {
      if (req.noise == Tr909NoisePath::Reference5us) return circuit_.Run(req, out);
      if (log_ && !logged_) { logged_ = true; log_("909 bake: voice " + std::to_string(static_cast<int>(req.voice)) + " requested with a noise path the circuit does not render: not baked"); }
      out.clear(); return false;
    }
    return pcm_.Run(req, out);
  }

private:
  Tr909CircuitRunner circuit_; Tr909PcmModelRunner pcm_; std::function<void(const std::string&)> log_; bool logged_ = false;
};

}  // namespace dgg
