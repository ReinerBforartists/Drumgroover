// TR-909 bake engine: the complete circuit for BD, SD, LT, MT, HT and HC, the PCM models for hi-hats, crash and ride.
//
// CircuitRunner909 (909CircuitRunner.h) renders a hit with the whole Nava circuit in one network at every pot state with the 5 us noise path; the PCM voices have no circuit and are rendered by PcmModelRunner909.
#pragma once
#include "909ModelRunner.h"
#include "909CircuitRunner.h"
#include "BakeAbort.h"
#include <functional>

namespace dgg {

class BakeRunner909 : public ModelRunner909
{
public:
  explicit BakeRunner909(std::function<void(const std::string&)> log = nullptr) : circuit_(1e-6, []() { return BakeAborted(); }), circuitTom_(1e-6, []() { return BakeAborted(); }, true), log_(std::move(log)) {}

  bool Run(const RunRequest909& req, std::vector<float>& out) override
  {
    if (CircuitRunner909::Supports(req.voice))
    {
      if (req.noise == NoisePath909::Reference5us) return (CircuitRunner909::SplitNoiseVoice(req.voice) ? circuitTom_ : circuit_).Run(req, out);   // toms: network with the noise sheet split off (E-03 phase 2); BD / SD / HC: the network of before
      if (log_ && !logged_) { logged_ = true; log_("909 bake: voice " + std::to_string(static_cast<int>(req.voice)) + " requested with a noise path the circuit does not render: not baked"); }
      out.clear(); return false;
    }
    return pcm_.Run(req, out);
  }

private:
  CircuitRunner909 circuit_, circuitTom_; PcmModelRunner909 pcm_; std::function<void(const std::string&)> log_; bool logged_ = false;
};

}  // namespace dgg
