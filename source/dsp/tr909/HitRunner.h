// Renders one hit of a TR-909 fast voice (FullFast2 model of one voice at one accent anchor).
//
// The model state is a reduced circuit (see FullFast2.h). The runner drives it with the trigger pulse and the noise source of the circuit,
// chooses the step size from the curvature of the device currents, and returns the output voltage at the host rate.
// One runner object serves many hits: the propagators are built once, every hit starts from a new model state.
#pragma once
#include <atomic>
#include "FullFast2.h"
#include "HostDecimator.h"
#include "../909AccentPlan.h"
#include "../BakeAbort.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

namespace dgg { namespace pcmfast {

// Raised by the host to stop the hits that are being rendered right now (Pause of the cache bake): RunSteps() returns false within a few hundred steps,
// the hit is incomplete and must not be stored. A hit that is started afterwards is not affected (it compares with the value it saw at its start).
inline std::atomic<unsigned>& BakeAbortGeneration() { return ::dgg::BakeAbortCounter(); }   // same counter for every engine (BakeAbort.h)

// Numeric settings of one voice. Validated per voice against the engine (tools handoff R162-R165).
struct HitTuning
{
  double ytol = 2e-3;        // tolerance of the error estimate per step (curvature of the device currents)
  double yfloor = 0.4;       // lower clamp of the envelope dependent tolerance factor
  double ytailT = 0.0, ytailMul = 1.0;     // looser tolerance from ytailT seconds after the trigger
  double ytail2T = 0.0, ytail2Mul = 1.0;   // milder stage before ytailT
  double dpTol = 0.3;        // limit of the port voltage change per step
  double zohW = 0.2;         // share of zero-order hold in the current interpolation
  bool quasiLinearCaps = false;
  int fixedJ = -1;           // >= 0: fixed step 2^fixedJ x 0.156 us (only shortened to meet a pulse corner or noise knot); -1: adaptive
  int knotSpan = 1;          // a step may span this many noise knots (1: every knot ends a step)
  double preRoll = 0.0;      // seconds of noise before the trigger (a multiple of the noise knot spacing): the noise-driven states then belong to the noise sequence of the hit
};

// Settings per voice. `fast20` selects the tolerances validated for the 20 us noise source (tools handoff R206); otherwise the settings of the
// 5 us reference source (tools `acc_sweep/cfg.sh`).
inline HitTuning HitTuningFor(Tr909Voice v, bool fast20)
{
  HitTuning t;
  switch (v)
  {
    case Tr909Voice::Lt: t.ytol = fast20 ? 1e-2 : 2e-3; t.yfloor = 1.0; t.ytailT = 0.1; t.ytailMul = 4.0; t.dpTol = 1.0; t.quasiLinearCaps = true; break;
    case Tr909Voice::Mt: t.ytol = fast20 ? 3e-3 : 1.5e-3; t.yfloor = 0.7; t.ytailT = 0.1; t.ytailMul = 1.6; t.dpTol = 1.0; t.quasiLinearCaps = true; break;
    case Tr909Voice::Ht: t.ytol = fast20 ? 3e-3 : 7e-4; t.yfloor = 0.4; t.ytailT = 0.1; t.ytailMul = 4.0; t.ytail2T = 0.02; t.ytail2Mul = 2.0; t.dpTol = 1.0; t.quasiLinearCaps = true; break;
    case Tr909Voice::Sd: t.ytol = fast20 ? 3e-2 : 2e-3; t.yfloor = 0.7; t.ytailT = 0.2; t.ytailMul = 6.0; t.dpTol = 1.0; t.quasiLinearCaps = true; break;
    case Tr909Voice::Bd: t.ytol = fast20 ? 1e-2 : 2e-3; t.yfloor = 0.7; break;
    case Tr909Voice::Hc: t.ytol = fast20 ? 3e-2 : 1e-3; t.yfloor = 0.4; t.zohW = 0.1; t.preRoll = 0.005; break;
    default: break;
  }
  return t;
}

// Noise source of the circuit: white samples every `step` seconds (sum of twelve uniform draws), linear in between. step = 5 us is the
// reference source of the engine. A larger step (20 us) keeps the power density and filters the interpolation lowpass (eq).
class HitNoise
{
public:
  void Start(uint64_t seed, double stepSeconds = 5e-6, double eq = 0.0)
  {
    smp_.clear(); step_ = stepSeconds; amp_ = 5.22 * std::sqrt(5e-6 / stepSeconds); eq_ = eq;
    rng_ = 0x9E3779B97F4A7C15ULL ^ 2ULL ^ (seed * 0xD1B54A32D192ED03ULL);
  }
  // Constant source at the idle level (no noise) with knots every `stepSeconds`: separates the cost of the knots from the cost of the noise.
  void StartSilent(double stepSeconds) { Start(0, stepSeconds, 0.0); amp_ = 0.0; }
  double step() const { return step_; }
  double At(double t)
  {
    const long idx = static_cast<long>(std::floor(t / step_)); Fill(idx + 1);
    const double fr = t / step_ - idx;
    const double a = eq_ != 0.0 ? Eq(idx) : smp_[idx], b = eq_ != 0.0 ? Eq(idx + 1) : smp_[idx + 1];
    return 7.5 + (a * (1 - fr) + b * fr);
  }
private:
  void Fill(long idx)
  {
    while (static_cast<long>(smp_.size()) <= idx + 1)
    {
      double sm = 0; for (int q = 0; q < 12; ++q) { rng_ = rng_ * 6364136223846793005ULL + 1442695040888963407ULL; sm += (rng_ >> 11) * (1.0 / 9007199254740992.0); }
      smp_.push_back((sm - 6.0) * amp_);
    }
  }
  double Eq(long i) { const double rm = i > 0 ? smp_[i - 1] : smp_[i]; return (1.0 + 2.0 * eq_) * smp_[i] - eq_ * (rm + smp_[i + 1]); }
  std::vector<double> smp_; uint64_t rng_ = 0; double step_ = 5e-6, amp_ = 5.22, eq_ = 0.0;
};

// Trigger of the voice: 5 V pulse, 10 us edges, 2 ms long, starting at t = 0.
inline double HitTrigger(double t)
{
  if (t < 0) return 0.0; if (t < 1e-5) return 5.0 * t / 1e-5; if (t < 1e-5 + 2e-3) return 5.0;
  if (t < 2e-5 + 2e-3) return 5.0 * (1.0 - (t - 1e-5 - 2e-3) / 1e-5); return 0.0;
}

template <class Mdl>
class HitRunner
{
public:
  using Model = FullFast2<Mdl>;
  static_assert(Mdl::kNT == 1 || Mdl::kNT == 2, "inputs: trigger, optionally the noise source first");

  HitRunner() : m_(new Model()) {}

  // Starts one hit: a new model state, the step control at its initial values. `noise` must stay valid until the hit has ended.
  void Begin(const HitTuning& tu, HitNoise* noise, double seconds)
  {
    Prepare(tu);
    tu_ = tu; noise_ = noise;
    knotN_ = (Mdl::kNT == 2 && noise) ? std::max(1LL, std::llround(noise->step() / kH0)) * std::max(1, tu.knotSpan) : 1LL;
    preN_ = Mdl::kNT == 2 ? static_cast<long long>(std::llround(tu.preRoll / kH0)) : 0LL;
    nEnd_ = static_cast<long long>(seconds / kH0) + preN_;
    for (int k = 0; k < Mdl::kNF; ++k) gFprev_[k] = 0.0;
    gHprev_ = 0.0; gEnv_ = 0.0; n_ = 0; j_ = 0; t_ = 0.0; y_ = 0.0; for (long& c : limCount) c = 0;
    while (n_ < preN_) Advance();   // the noise runs before the trigger; t is counted from the trigger
    for (int k = 0; k < Mdl::kNF; ++k) gFprev_[k] = 0.0;
    gHprev_ = 0.0; gEnv_ = 0.0; j_ = 0; t_ = 0.0; y_ = 0.0;   // the step control restarts at the trigger
  }
  bool Finished() const { return n_ >= nEnd_; }
  long limCount[4] = {0, 0, 0, 0};   // what set the next step size: none (maximum) / tanh stage event / error estimate / port voltage change
  // Integration time reached so far (seconds since the trigger) and the output voltage there.
  double Time() const { return t_; }
  double Output() const { return y_; }

  // One integration step with its step size control. Call only while !Finished().
  void Advance()
  {
    Model& m = *m_;
    const int jmax = 10, jup = 5, jdown = 12; (void)jup;
    const long long bp[4] = {preN_ + 64, preN_ + 12864, preN_ + 12928, 1LL << 40};   // pulse corners (10 us, 2.01 ms, 2.02 ms) in units of h0
    static constexpr double kYenvRef = 0.1, kYenvTau = 2e-3, kXs = 4.0;
    const double h0 = kH0; double gF[Mdl::kNF];
    long long nextBp = 1LL << 40; for (long long b : bp) if (b > n_) { nextBp = b; break; }
    if (n_ < preN_) nextBp = std::min(nextBp, preN_);   // the trigger starts here
    if (Mdl::kNT == 2) nextBp = std::min(nextBp, (n_ / knotN_ + 1) * knotN_);
    int jj = tu_.fixedJ >= 0 ? tu_.fixedJ : j_; while (jj > 0 && n_ + (1LL << jj) > nextBp) --jj;
    const double h = h0 * static_cast<double>(1LL << jj);
    const double xa[2] = {Mdl::kNT == 2 ? noise_->At(kStartTime + (n_ - preN_) * h0) : 0.0, HitTrigger((n_ - preN_) * h0)};
    const double xb[2] = {Mdl::kNT == 2 ? noise_->At(kStartTime + (n_ + (1LL << jj) - preN_) * h0) : 0.0, HitTrigger((n_ + (1LL << jj) - preN_) * h0)};
    const double* const x0 = Mdl::kNT == 2 ? xa : &xa[1]; const double* const x1 = Mdl::kNT == 2 ? xb : &xb[1];
    const long it0 = m.its, ret0 = m.retries, fail0 = m.fails;
    double pOld[Mdl::kNP]; for (int a = 0; a < Mdl::kNP; ++a) pOld[a] = m.p[a];
    for (int k = 0; k < Mdl::kNF; ++k) gF[k] = m.f0[k];
    const double y = m.Step(pj_[jj], x0, x1); n_ += 1LL << jj;
    t_ = (n_ - preN_) * h0; y_ = y;
    gEnv_ = std::fmax(std::fabs(y), gEnv_ * std::exp(-h / kYenvTau));
    const long used = m.its - it0; const bool hard = (m.retries != ret0) || (m.fails != fail0) || used > jdown;
    // step control: (1) event prediction for the op-amp input stages, (2) curvature of the device currents, (3) change of the port voltages
    double hMax = h0 * static_cast<double>(1LL << jmax), dpMax = 0.0; int lim = 0;
    for (int a = 0; a < Mdl::kNP; ++a) dpMax = std::fmax(dpMax, std::fabs(m.p[a] - pOld[a]) * m.portScale[a]);
    for (int d = 0; d < Mdl::kNumDev; ++d) if (Mdl::kDevType[d] == 2)
    {
      const int pp = Mdl::kDevPort[d], idx = Mdl::kDevIdx[d]; const double k = Mdl::kTanh[3 * idx] / Mdl::kTanh[3 * idx + 1];
      const double xn = k * m.p[pp], xo = k * pOld[pp], rate = (xn - xo) / h, ax = std::fabs(xn), toward = (xn > 0 ? -rate : rate);
      double c = 1e300; if (ax < kXs) c = kXs / std::fmax(std::fabs(rate), 1e-30); else if (toward > 0) c = 0.5 * (ax - kXs) / toward;
      if (c < hMax) { hMax = c; lim = 1; }
    }
    if (tu_.ytol > 0.0 && gHprev_ > 0.0)
    {
      double df[Mdl::kNF];
      for (int k = 0; k < Mdl::kNF; ++k) df[k] = m.f0[k] - (gF[k] + (gF[k] - gFprev_[k]) * (h / gHprev_));
      const double e = m.NodeErr(pj_[jj], df);
      if (e > 0.0)
      {
        double tol = tu_.ytol; const double tt = (n_ - preN_) * h0;
        if (tu_.ytailMul != 1.0 && tt >= tu_.ytailT) tol *= tu_.ytailMul; else if (tu_.ytail2Mul != 1.0 && tt >= tu_.ytail2T) tol *= tu_.ytail2Mul;
        tol *= std::fmin(1.0, std::fmax(tu_.yfloor, gEnv_ / kYenvRef));
        const double c = h * std::sqrt(0.8 * tol / e); if (c < hMax) { hMax = c; lim = 2; }
      }
    }
    for (int k = 0; k < Mdl::kNF; ++k) gFprev_[k] = gF[k];
    gHprev_ = h;
    if (dpMax > tu_.dpTol) { const double c = h * tu_.dpTol / dpMax; if (c < hMax) { hMax = c; lim = 3; } }
    ++limCount[lim];
    int jn = 0; while (jn < jmax && h0 * static_cast<double>(1LL << (jn + 1)) <= hMax) ++jn;
    j_ = hard ? std::max(0, std::min(jn, jj - 1)) : std::min(jn, jj + 1);
    if (tu_.fixedJ >= 0) j_ = tu_.fixedJ;
  }

  // Renders `seconds` of one hit and delivers (t, y) for every integration step: t in seconds since the trigger, y the output voltage.
  template <class StepFn>
  bool RunSteps(const HitTuning& tu, HitNoise* noise, double seconds, StepFn&& onStep)
  {
    Begin(tu, noise, seconds);
    unsigned cnt = 0;
    while (!Finished())
    {
      if ((++cnt & 127u) == 0 && ::dgg::BakeAborted()) return false;   // a bake job enqueued before the last Pause / Close stops here (never true outside bake jobs)
      Advance(); onStep(t_, y_);
    }
    return true;
  }

  // Builds the model and its propagators without running a hit (the first Begin() does this otherwise; takes about a second per model).
  void Warm(const HitTuning& tu) { Prepare(tu); }

  // Host-rate rendering: output voltage minus idle level at fsHost, `seconds` long.
  bool Render(const HitTuning& tu, HitNoise* noise, double seconds, double fsHost, std::vector<float>& out)
  {
    HostDecimator dec; dec.Init(fsHost); out.clear(); const size_t want = static_cast<size_t>(seconds * fsHost);
    auto drain = [&] { double s; while (dec.Pop(s)) out.push_back(static_cast<float>(s)); };
    const bool ok = RunSteps(tu, noise, seconds, [&](double t, double y) { dec.Push(t, y - idle()); drain(); });
    drain(); if (out.size() > want) out.resize(want);
    return ok;
  }

  double idle() const { return m_->yIdle; }
  long steps() const { return m_->steps; }
  long unrecovered() const { return m_->fails; }
  auto& ModelState() { return *m_; }   // read access to the integrator state for the hybrid reduced model (HcHybrid.h) at its hand-over

private:
  static constexpr double kH0 = 1e-5 / 64.0;
  static constexpr double kStartTime = 0.05;   // the noise source runs before the trigger: the model starts from the state at the trigger time

  void Prepare(const HitTuning& tu)
  {
    // a new model state for every hit, the propagators of the previous hit are kept
    if (built_) { auto props = std::move(m_->props); auto half = std::move(m_->halfIdx); m_.reset(new Model()); m_->Init(); m_->props = std::move(props); m_->halfIdx = std::move(half); }
    else { m_->Init(); built_ = true; }
    Model& m = *m_;
    m.dampOn = true; m.dampRatio = 0.01; m.noFallback = true; m.zohW = tu.zohW; m.chordFirst = 5; m.fastDot = true; m.fastSolve = true;
    m.Reset(Mdl::kAcc); m.PortGrpInit();
    if (tu.quasiLinearCaps) { m.QlcEnable(true); m.qlcErr = 1e-3; }
    m.QlEnable(true); m.qlTol = 0.1; m.qlErr = 1e-3; m.qleOn = true; m.qleHold = 500;
    m.qlHoldD.assign(Mdl::kNumDev, m.qlHold);
    if (pj_.empty())
    {
      const double h0 = 1e-5 / 64.0; pj_.resize(11);
      for (int j = 0; j <= 10; ++j) pj_[j] = m.Prepare(h0 * static_cast<double>(1 << j));
      for (int j = 1; j <= 10; ++j) m.SetHalf(pj_[j], pj_[j - 1]);
      m.SetHalf(pj_[0], m.Prepare(h0 * 0.5));
    }
  }

  std::unique_ptr<Model> m_; std::vector<int> pj_; bool built_ = false;
  HitTuning tu_; HitNoise* noise_ = nullptr; long long knotN_ = 1, nEnd_ = 0, n_ = 0, preN_ = 0; int j_ = 0;
  double gFprev_[Mdl::kNF] = {0}, gHprev_ = 0.0, gEnv_ = 0.0, t_ = 0.0, y_ = 0.0;
};

}}  // namespace dgg::pcmfast
