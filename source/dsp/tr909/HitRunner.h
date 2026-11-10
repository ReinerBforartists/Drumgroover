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
  double ytail3T = 1e9, ytail3Mul = 1.0;   // further factor on the tolerance from ytail3T seconds after the trigger
  double ytail2T = 0.0, ytail2Mul = 1.0;   // milder stage before ytailT
  double dpTol = 0.3;        // limit of the port voltage change per step
  double zohW = 0.2;         // share of zero-order hold in the current interpolation
  bool noTanhEvents = false; // true: the op-amp input stage event prediction does not limit the step size
  int jGrow = 1;             // the step size may grow by this many powers of two per step
  bool quasiLinearCaps = false;
  int fixedJ = -1;           // >= 0: fixed step 2^fixedJ x 0.156 us (only shortened to meet a pulse corner or noise knot); -1: adaptive
  int knotSpan = 1;          // a step may span this many noise knots (1: every knot ends a step)
  double capThr = 0.0;       // bypass of the nonlinear capacitance charges within this port voltage change (V) of the last full evaluation; 0: off (FullFast2::CapQm)
  double chordFirst = 5.0;   // a chord step whose first Newton correction (V) exceeds this refactors the Jacobian; 1e9: never (HC: 1.00 -> 0.36 factorisations per step in the engine phase, output changes by < -100 dB, tools handoff R304)
  bool cmpEvents = false;    // comparator input ports get the same event prediction as the op-amp input stages: a step never runs through the switching region unseen (HC: the flank times of the claps)
  double knotPairT = -1.0; int knotMaxJ = 6;   // knotPairT >= 0: from this time (s after the trigger) a step may span 2^(j-5) noise knots, j <= knotMaxJ, the noise handled as linear segments between the knots
  bool fastLinAlg = false;      // node error of the step control with the blocked AVX product (other summation order)
  int errMode = 0;           // error estimate of the step size control: 0 largest node voltage error, 1 output voltage error, 2 the larger of the output error and errNodeW x the node error
  double errNodeW = 0.1;
  double errT = 0.0;         // errMode 1 / 2 apply from this time (s after the trigger); before it the node error is used
  double noiseOffT = -1.0;   // >= 0: from this time (s after the trigger, rounded down to a noise knot) the noise source keeps its value at that knot; its response at the output has died out, the steps are no longer tied to the noise knots
  double linNoiseT = -1.0;   // >= 0: the response to the noise is computed as the linear response around the noise-free trajectory (HitReplay.h) for the first linNoiseT seconds, the hit itself runs without noise
  double linYtol = 1e-3, linTailMul = 2.0, replayThr = 0.3, replayLater = 12.0; bool replayRefine = false, replayFloat = false;
  double linT2T = 0.0, linT2Mul = 1.0;   // milder tolerance stage of the noise-free run between linT2T and linNoiseT
  int jMax = 10;             // largest step 2^jMax x 0.156 us
  bool squareProps = false;   // model build: propagators of doubled steps by squaring (FullFast2::squareProps)
  int replayKnot = 5;        // spacing of the noise knots of the linear response in us (5: the noise source itself; 10 / 20: the coarse source of HitNoise::AtCoarse)
  double replayEq = 0.0;     // equaliser of the coarse source
  bool detReuse = false;     // linear-response mode: the noise-free run of the last hit is reused when model, pots, tuning and length are the same (the run does not depend on the noise)   // tolerance of the noise-free run before linNoiseT, factor on it after linNoiseT
  double preRoll = 0.0;      // seconds of noise before the trigger (a multiple of the noise knot spacing): the noise-driven states then belong to the noise sequence of the hit
};

// Settings per voice. `fast20` selects the tolerances validated for the 20 us noise source (tools handoff R206); otherwise the settings of the
// 5 us reference source (tools `acc_sweep/cfg.sh`).
inline HitTuning HitTuningFor(Tr909Voice v, bool fast20)
{
  HitTuning t;
  switch (v)
  {
    case Tr909Voice::Lt: t.ytol = fast20 ? 1e-2 : 3e-3; t.yfloor = 1.0; t.ytailT = 0.1; t.ytailMul = fast20 ? 4.0 : 8.0; t.dpTol = 1.0; t.quasiLinearCaps = true; t.noTanhEvents = !fast20; t.jGrow = fast20 ? 1 : 3; break;
    case Tr909Voice::Mt: t.ytol = 3e-3; t.yfloor = 0.7; t.ytailT = 0.1; t.ytailMul = 1.6; t.dpTol = 1.0; t.quasiLinearCaps = true; break;
    case Tr909Voice::Ht: t.ytol = fast20 ? 3e-3 : 1.5e-3; t.yfloor = 0.4; t.ytailT = 0.1; t.ytailMul = 4.0; t.ytail2T = 0.02; t.ytail2Mul = 2.0; t.dpTol = 1.0; t.quasiLinearCaps = true; break;
    case Tr909Voice::Sd: t.ytol = fast20 ? 3e-2 : 9e-4; t.yfloor = 0.7; t.ytailT = fast20 ? 0.2 : 0.3; t.ytailMul = fast20 ? 6.0 : 2.0; t.dpTol = 1.0; t.quasiLinearCaps = true; break;
    case Tr909Voice::Bd: t.ytol = fast20 ? 1e-2 : 8e-3; t.yfloor = fast20 ? 0.7 : 1.0; if (!fast20) { t.noiseOffT = 0.03; t.linNoiseT = 0.03; t.errMode = 2; t.errNodeW = 0.3; t.errT = 0.05; t.ytailT = 0.03; t.ytailMul = 0.25; t.ytail2T = 0.005; t.ytail2Mul = 2.0; t.capThr = 1e-4; t.jGrow = 3; t.fastLinAlg = true; t.squareProps = true; t.replayKnot = 10; t.replayEq = 0.2; } break;   // BD-K03/K02/K17/K16 (tools 909_BD_OPTIMIERUNGEN.md): 10.6 -> 1.99 M steps in 18 cases, R115 0 violations
    case Tr909Voice::Hc: t.ytol = fast20 ? 3e-2 : 2e-3; t.yfloor = 0.7; t.zohW = 0.1; t.preRoll = 0.005; t.capThr = 1e-300; t.cmpEvents = true; t.knotPairT = 0.05; t.knotMaxJ = 7; break;   // capThr: a charge device evaluated at a bit-identical port voltage repeats its last result (output bitwise equal, HC 15 % less time)
    default: break;
  }
#ifdef DGG_HC_ENV_KNOBS   // test harness only
  if (const char* e = std::getenv("LINNOISET")) t.linNoiseT = std::atof(e);
  if (const char* e = std::getenv("NOISEOFFT")) t.noiseOffT = std::atof(e);
#endif
  return t;
}

// Noise source of the circuit: white samples every `step` seconds (sum of twelve uniform draws), linear in between. step = 5 us is the
// reference source of the engine. A larger step (20 us) keeps the power density and filters the interpolation lowpass (eq).
class HitNoise
{
public:
  static constexpr uint64_t kStreamSeed = 0x0909A0015EEDULL;   // seed of the shared noise stream of the circuit (one source for all voices)
  static constexpr uint64_t kNoStream = ~0ULL;                  // 'no stream position': the hit uses its own sequence (Start(seed))
  void Start(uint64_t seed, double stepSeconds = 5e-6, double eq = 0.0)
  {
    smp_.clear(); step_ = stepSeconds; amp_ = 5.22 * std::sqrt(5e-6 / stepSeconds); eq_ = eq;
    rng_ = 0x9E3779B97F4A7C15ULL ^ 2ULL ^ (seed * 0xD1B54A32D192ED03ULL);
  }
  // Shared noise stream (the circuit has one noise source for all voices): the same sequence as Start(seed), but the hit begins at knot `startKnot` of the stream (absolute time / step of the trigger). The generator is a 64-bit LCG with 12 draws per knot:
  // the state at knot n is the state at the start advanced by 12 n steps (jump by repeated squaring of the affine map), so every voice that reads the stream at the same absolute time gets the same values.
  void StartAt(uint64_t seed, uint64_t startKnot, double stepSeconds = 5e-6, double eq = 0.0)
  {
    Start(seed, stepSeconds, eq);
    uint64_t n = startKnot * 12ULL, A = 1ULL, C = 0ULL, a = 6364136223846793005ULL, c = 1442695040888963407ULL;   // (A, C): the map x -> A x + C advanced by n steps
    while (n) { if (n & 1ULL) { A = A * a; C = C * a + c; } c = c * (a + 1ULL); a = a * a; n >>= 1; }   // (a, c) squared: x -> a (a x + c) + c
    rng_ = A * rng_ + C;
  }
  // Constant source at the idle level (no noise) with knots every `stepSeconds`: separates the cost of the knots from the cost of the noise.
  void StartSilent(double stepSeconds) { Start(0, stepSeconds, 0.0); amp_ = 0.0; }
  double step() const { return step_; }
  void Reserve(size_t n) { smp_.reserve(n); }
  // Coarse value at a multiple of f knots (t = i step, i a multiple of f): the source averaged over f knots (trapezoid of the linear interpolation) and compensated by the equaliser (1 + 2 eq, -eq, -eq) over the neighbouring coarse values;
  // the response of a linear circuit to the coarse source at the spacing f step approximates its response to the source itself in the band below 0.5 / (f step)
  double AtCoarse(double t, int f, double eq)
  {
    const long i = std::lround(t / step_); const int hf = f / 2; Fill(i + f + hf + 2);
    auto M = [&](long c) { double s = 0.5 * (smp_[c < hf ? 0 : c - hf] + smp_[c + hf]); for (int m = -hf + 1; m <= hf - 1; ++m) s += smp_[c + m < 0 ? 0 : c + m]; return s / f; };
    const double m0 = M(i), mm = i - f >= 0 ? M(i - f) : m0, mp = M(i + f);
    return 7.5 + (1.0 + 2.0 * eq) * m0 - eq * (mm + mp);
  }   // capacity for the knots of a hit: At() then allocates nothing
  void Scale(double s) { amp_ *= s; }   // test harness: scaled noise amplitude (linearity of the response to the noise)
  double At(double t)
  {
    if (t < 0.0) t = 0.0;   // an idle run before the trigger that is longer than kStartTime (test harness, silent noise): the source stays at its start value
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

}}  // namespace dgg::pcmfast
#include "HitReplay.h"
namespace dgg { namespace pcmfast {

// Trigger of the voice: 5 V pulse, 10 us edges, 2 ms long, starting at t = 0.
inline double HitTrigger(double t)
{
  if (t < 0) return 0.0; if (t < 1e-5) return 5.0 * t / 1e-5; if (t < 1e-5 + 2e-3) return 5.0;
  if (t < 2e-5 + 2e-3) return 5.0 * (1.0 - (t - 1e-5 - 2e-3) / 1e-5); return 0.0;
}

template <class Mdl, class Pot> class LiveLin;

template <class Mdl>
class HitRunner
{
public:
  template <class M2, class P2> friend class LiveLin;
  static constexpr double StartTime() { return 0.05; }
  using Model = FullFast2<Mdl>;
  static_assert(Mdl::kNT == 1 || Mdl::kNT == 2, "inputs: trigger, optionally the noise source first");

  HitRunner() : m_(new Model()) {}

  // Starts one hit: a new model state, the step control at its initial values. `noise` must stay valid until the hit has ended.
  void Begin(const HitTuning& tu, HitNoise* noise, double seconds)
  {
    Prepare(tu);
    tu_ = tu; noise_ = noise;
#ifdef DGG_HC_ENV_KNOBS
    if (const char* e = std::getenv("PREROLL")) tu_.preRoll = std::atof(e) * 1e-3;   // test harness only (ms)
#endif
    knotN_ = (Mdl::kNT == 2 && noise) ? std::max(1LL, std::llround(noise->step() / kH0)) * std::max(1, tu.knotSpan) : 1LL;
    noiseOffN_ = (Mdl::kNT == 2 && tu.noiseOffT >= 0.0) ? ((std::llround(tu.noiseOffT / kH0) / knotN_) * knotN_) : (1LL << 40);
    preN_ = Mdl::kNT == 2 ? static_cast<long long>(std::llround(tu_.preRoll / kH0)) : 0LL;
    nEnd_ = static_cast<long long>(seconds / kH0) + preN_;
    for (int k = 0; k < Mdl::kNF; ++k) gFprev_[k] = 0.0;
    gHprev_ = 0.0; gEnv_ = 0.0; n_ = 0; j_ = 0; t_ = 0.0; y_ = 0.0; for (long& c : limCount) c = 0;
    while (n_ < preN_) Advance();   // the noise runs before the trigger; t is counted from the trigger
    for (int k = 0; k < Mdl::kNF; ++k) gFprev_[k] = 0.0;
    gHprev_ = 0.0; gEnv_ = 0.0; j_ = 0; t_ = 0.0; y_ = 0.0;   // the step control restarts at the trigger
  }
  bool Finished() const { return n_ >= nEnd_; }
  long errArgHist[Mdl::kNS] = {0};   // diagnostic (FULLFAST2_PROF): node that limits the step in the noise phase
  long limCount[4] = {0, 0, 0, 0};   // what set the next step size: none (maximum) / tanh stage event / error estimate / port voltage change
  // Integration time reached so far (seconds since the trigger) and the output voltage there.
  double Time() const { return t_; }
  double Output() const { return y_; }

  // One integration step with its step size control. Call only while !Finished().
  void Advance()
  {
    Model& m = *m_;
    const int jmax = tu_.jMax, jup = 5, jdown = 12; (void)jup;
    const long long bp[4] = {preN_ + 64, preN_ + 12864, preN_ + 12928, 1LL << 40};   // pulse corners (10 us, 2.01 ms, 2.02 ms) in units of h0
    static constexpr double kYenvRef = 0.1, kYenvTau = 2e-3, kXs = 4.0;
    const double h0 = kH0; double gF[Mdl::kNF];
    long long nextBp = 1LL << 40; for (long long b : bp) if (b > n_) { nextBp = b; break; }
    if (n_ < preN_) nextBp = std::min(nextBp, preN_);   // the trigger starts here
    if (Mdl::kNT == 2 && n_ - preN_ < noiseOffN_) nextBp = std::min(nextBp, (n_ / knotN_ + 1) * knotN_);
    long long nextCorner = 1LL << 40; for (long long b : bp) if (b > n_) { nextCorner = b; break; }
    int jj = tu_.fixedJ >= 0 ? tu_.fixedJ : j_; while (jj > 0 && n_ + (1LL << jj) > nextBp) --jj;
    int nseg = 1;
    if (Mdl::kNT == 2 && tu_.knotPairT >= 0.0 && knotN_ == 32 && n_ >= preN_ && (n_ - preN_) * kH0 >= tu_.knotPairT && ((n_ - preN_) % knotN_) == 0 && tu_.fixedJ < 0 && j_ >= 6)
    {
      int jp = std::min(j_, tu_.knotMaxJ); while (jp >= 6 && n_ + (1LL << jp) > nextCorner) --jp;
      if (jp >= 6) { jj = jp; nseg = 1 << (jp - 5); }
    }
    const double h = h0 * static_cast<double>(1LL << jj);
    const double xa[2] = {Mdl::kNT == 2 ? noise_->At(kStartTime + std::min(n_ - preN_, noiseOffN_) * h0) : 0.0, HitTrigger((n_ - preN_) * h0)};
    const double xb[2] = {Mdl::kNT == 2 ? noise_->At(kStartTime + std::min(n_ + (1LL << jj) - preN_, noiseOffN_) * h0) : 0.0, HitTrigger((n_ + (1LL << jj) - preN_) * h0)};
    const double* const x0 = Mdl::kNT == 2 ? xa : &xa[1]; const double* const x1 = Mdl::kNT == 2 ? xb : &xb[1];
    double xseg[9 * 2]; const double* x0s = x0; const double* x1s = x1;
    if (nseg > 1) { for (int m = 0; m <= nseg; ++m) { const long long nn = n_ + static_cast<long long>(m) * knotN_ - preN_; xseg[2 * m] = noise_->At(kStartTime + nn * h0); xseg[2 * m + 1] = HitTrigger(nn * h0); } x0s = xseg; x1s = xseg + 2 * nseg; }
    const long it0 = m.its, ret0 = m.retries, fail0 = m.fails;
    double pOld[Mdl::kNP]; for (int a = 0; a < Mdl::kNP; ++a) pOld[a] = m.p[a];
    for (int k = 0; k < Mdl::kNF; ++k) gF[k] = m.f0[k];
    const double y = m.Step(pj_[jj], x0s, x1s, 0, nseg); n_ += 1LL << jj;
    t_ = (n_ - preN_) * h0; y_ = y;
    gEnv_ = FMaxF(std::fabs(y), gEnv_ * std::exp(-h / kYenvTau));
    const long used = m.its - it0; const bool hard = (m.retries != ret0) || (m.fails != fail0) || used > jdown;
    // step control: (1) event prediction for the op-amp input stages, (2) curvature of the device currents, (3) change of the port voltages
    double hMax = h0 * static_cast<double>(1LL << jmax), dpMax = 0.0; int lim = 0;
    for (int a = 0; a < Mdl::kNP; ++a) dpMax = FMaxF(dpMax, std::fabs(m.p[a] - pOld[a]) * m.portScale[a]);
    for (int d = 0; d < Mdl::kNumDev; ++d) if (Mdl::kDevType[d] == 2)
    {
      const int pp = Mdl::kDevPort[d], idx = Mdl::kDevIdx[d]; const double k = Mdl::kTanh[3 * idx] / Mdl::kTanh[3 * idx + 1];
      const double xn = k * m.p[pp], xo = k * pOld[pp], rate = (xn - xo) / h, ax = std::fabs(xn), toward = (xn > 0 ? -rate : rate);
      double c = 1e300; if (ax < kXs) c = kXs / FMaxF(std::fabs(rate), 1e-30); else if (toward > 0) c = 0.5 * (ax - kXs) / toward;
      if (c < hMax && !tu_.noTanhEvents) { hMax = c; lim = 1; }
    }
    if (tu_.cmpEvents) for (int d = 0; d < Mdl::kNumDev; ++d) if (Mdl::kDevType[d] == 4)
    {   // comparator: the tanh argument is the second port voltage itself
      const int pp = Mdl::kDevPort[d] + 1; const double xn = m.p[pp], xo = pOld[pp], rate = (xn - xo) / h, ax = std::fabs(xn), toward = (xn > 0 ? -rate : rate);
      double c = 1e300; if (ax < kXs) c = kXs / FMaxF(std::fabs(rate), 1e-30); else if (toward > 0) c = 0.5 * (ax - kXs) / toward;
      if (c < hMax) { hMax = c; lim = 1; }
    }
    if (tu_.ytol > 0.0 && gHprev_ > 0.0)
    {
      double df[Mdl::kNF];
      for (int k = 0; k < Mdl::kNF; ++k) df[k] = m.f0[k] - (gF[k] + (gF[k] - gFprev_[k]) * (h / gHprev_));
      const double e = (tu_.errMode == 0 || (n_ - preN_) * h0 < tu_.errT) ? m.NodeErr(pj_[jj], df) : (tu_.errMode == 1 ? std::fabs(m.OutErr(pj_[jj], df)) : FMaxF(std::fabs(m.OutErr(pj_[jj], df)), tu_.errNodeW * m.NodeErr(pj_[jj], df)));
      if (e > 0.0)
      {
        double tol = tu_.ytol; const double tt = (n_ - preN_) * h0;
        if (tu_.ytailMul != 1.0 && tt >= tu_.ytailT) tol *= tu_.ytailMul; else if (tu_.ytail2Mul != 1.0 && tt >= tu_.ytail2T) tol *= tu_.ytail2Mul;
        if (tt >= tu_.ytail3T) tol *= tu_.ytail3Mul;
        tol *= FMinF(1.0, FMaxF(tu_.yfloor, gEnv_ / kYenvRef));
#ifdef FULLFAST2_PROF
        { int arg = 0; m.NodeErrArg(pj_[jj], df, arg); if (std::sqrt(0.8 * tol / e) < 1.0 && (n_ - preN_) * h0 < 0.03) ++errArgHist[arg]; }
#endif
        const double c = h * std::sqrt(0.8 * tol / e); if (c < hMax) { hMax = c; lim = 2; }
      }
    }
    for (int k = 0; k < Mdl::kNF; ++k) gFprev_[k] = gF[k];
    gHprev_ = h;
    if (dpMax > tu_.dpTol) { const double c = h * tu_.dpTol / dpMax; if (c < hMax) { hMax = c; lim = 3; } }
    ++limCount[lim];
    int jn = 0; while (jn < jmax && h0 * static_cast<double>(1LL << (jn + 1)) <= hMax) ++jn;
    j_ = hard ? std::max(0, std::min(jn, jj - 1)) : std::min(jn, jj + tu_.jGrow);
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
  // Pre-roll of the free-running oscillators (BD-D32): the model with idle inputs (the noise source at its idle level 7.5 V, trigger 0) for `seconds` with the fixed step 2^j x 0.156 us, from the state the model has now
  // (after Prepare / Begin: the rest state kZ0, or the state of a hit that has ended). cb(t, z, p, f0) after every step: t seconds since the start of this call, the capacitor states, the port voltages and the device currents.
  template <class Cb> void IdleRun(double seconds, int j, Cb&& cb)
  {
    Model& m = *m_; const double x[2] = {7.5, 0.0}; const double h = kH0 * static_cast<double>(1LL << j); const long n = static_cast<long>(seconds / h);
    for (long k = 0; k < n; ++k) { m.Step(pj_[j], x, x, 0, 1); cb(static_cast<double>(k + 1) * h, m.z, m.p, m.f0); }
  }
  // The next Begin() starts from this state (capacitor states z[kR], port voltages p[kNP]) instead of the rest state kZ0; ClearStartState() goes back.
  void SetStartState(const double* z, const double* p) { startZ_.assign(z, z + Mdl::kR); startP_.assign(p, p + Mdl::kNP); }
  void ClearStartState() { startZ_.clear(); startP_.clear(); }
  const double* State() const { return m_->z; }
  const double* Ports() const { return m_->p; }
  // Pot dependent model (PotSet.h): replaces A, Bg, XzS, DgS and the output row for all following hits; the arrays must outlive the runner / the next SetPot(). Not while a hit is running.
  void SetPot(const typename Model::PotPtrs& p) { pot_ = p; usePot_ = true; potStale_ = true; ++potVer_; }
  void ClearPot() { usePot_ = false; potStale_ = true; ++potVer_; }

  // Host-rate rendering: output voltage minus idle level at fsHost, `seconds` long.
  // Noise phase as linear response (tu.linNoiseT >= 0): the hit runs without noise (tolerances linYtol / linTailMul), its port voltages are recorded for the first linNoiseT + 10 ms, and the response of the circuit to the noise
  // of `noise` over that time (HitReplay.h) is added at the host rate.
  // Raw series of the linear-response mode: ts / ys = step ends and output voltage of the noise-free run (whole hit), tk / dyk = the noise response at the 5 us knots of the first linNoiseT + 10 ms.
  bool RunLin(const HitTuning& tu, HitNoise* noise, double seconds, std::vector<double>& ts, std::vector<double>& ys, std::vector<double>& tk, std::vector<double>& dyk)
  {
    if (!RunLinDet(tu, seconds, ts, ys)) return false;
    return RunLinResponse(tu, noise, tk, dyk);
  }
  // The noise-free run (shared by all noise sequences of a group of hits).
  bool RunLinDet(const HitTuning& tu, double seconds, std::vector<double>& ts, std::vector<double>& ys)
  {
    if (tu.detReuse && det_.valid && det_.potVer == potVer_ && det_.seconds == seconds && det_.sig == TuningSig(tu)) { ts = det_.ts; ys = det_.ys; return true; }   // replay_ still holds the trajectory and the epochs of this run (only RunLinDet changes them)
    det_.valid = false;
    HitNoise silent; silent.StartSilent(0.05);
    HitTuning td = tu; td.noiseOffT = -1.0; td.ytol = tu.linYtol; td.ytail2T = tu.linT2T; td.ytail2Mul = tu.linT2Mul; td.ytailT = tu.linNoiseT; td.ytailMul = tu.linTailMul;
    const double tRep = tu.linNoiseT + 0.01; ts.clear(); ys.clear();
    Begin(td, &silent, seconds); Model& m = *m_; replay_.Clear(m.p);
    unsigned cnt = 0;
    while (!Finished())
    {
      if ((++cnt & 127u) == 0 && ::dgg::BakeAborted()) return false;
      Advance(); if (t_ <= tRep + 0.001) replay_.Record(t_, m.p);
      ts.push_back(t_); ys.push_back(y_);
    }
    replay_.thr = tu.replayThr; replay_.laterMul = tu.replayLater; replay_.refine = tu.replayRefine; replay_.single = tu.replayFloat; replay_.coarse = tu.replayKnot >= 10 ? tu.replayKnot / 5 : 1; replay_.eq = tu.replayEq; replay_.Prime(m, pj_[tu.replayKnot >= 20 ? 7 : (tu.replayKnot >= 10 ? 6 : 5)], tRep);   // Jacobi epochs of the response for this trajectory
    if (tu.detReuse) { det_.valid = true; det_.potVer = potVer_; det_.seconds = seconds; det_.sig = TuningSig(tu); det_.ts = ts; det_.ys = ys; }
    return true;
  }
  static double TuningSig(const HitTuning& t) { return t.ytol * 1.0 + t.yfloor * 3.0 + t.ytailT * 5.0 + t.ytailMul * 7.0 + t.ytail2T * 11.0 + t.ytail2Mul * 13.0 + t.dpTol * 17.0 + t.zohW * 19.0 + t.jGrow * 23.0 + t.capThr * 29.0 + t.chordFirst * 31.0 + t.linNoiseT * 37.0 + t.linYtol * 41.0 + t.linTailMul * 43.0 + t.linT2T * 53.0 + t.linT2Mul * 59.0 + t.replayKnot * 67.0 + t.replayEq * 71.0 + t.jMax * 61.0 + t.fastLinAlg * 47.0; }
  struct DetCache { bool valid = false; unsigned potVer = 0; double seconds = 0.0, sig = 0.0; std::vector<double> ts, ys; } det_;
  // The response to the noise of `noise` around the noise-free trajectory of the last RunLinDet().
  bool RunLinResponse(const HitTuning& tu, HitNoise* noise, std::vector<double>& tk, std::vector<double>& dyk)
  {
    tk.clear(); dyk.clear(); replay_.Start(*noise, kStartTime, tu.linNoiseT + 0.01); double t, dy; while (replay_.Next(t, dy)) { tk.push_back(t); dyk.push_back(dy); }
    return true;
  }
  // n hits (noise sequences) of one parameter set: one noise-free run, one linear response per noise; out[l] at the host rate.
  bool RenderLinGroup(const HitTuning& tu, HitNoise* noises, int n, double seconds, double fsHost, std::vector<float>* out)
  {
    std::vector<double> ts, ys, tk, dyk; if (!RunLinDet(tu, seconds, ts, ys)) return false;
    const size_t want = static_cast<size_t>(seconds * fsHost); const double idleV = idle(); std::vector<float> base; double s;
    { HostDecimator dec; dec.Init(fsHost); for (size_t i = 0; i < ts.size(); ++i) { dec.Push(ts[i], ys[i] - idleV); while (dec.Pop(s)) base.push_back(static_cast<float>(s)); } while (dec.Pop(s)) base.push_back(static_cast<float>(s)); if (base.size() > want) base.resize(want); }
    for (int l = 0; l < n; ++l)
    {
      if (::dgg::BakeAborted()) return false;
      RunLinResponse(tu, &noises[l], tk, dyk); out[l] = base;
      HostDecimator decR; decR.Init(fsHost); size_t i = 0; for (size_t k = 0; k < tk.size(); ++k) { decR.Push(tk[k], dyk[k]); while (decR.Pop(s)) { if (i < out[l].size()) out[l][i] += static_cast<float>(s); ++i; } }
      while (decR.Pop(s)) { if (i < out[l].size()) out[l][i] += static_cast<float>(s); ++i; }
    }
    return true;
  }
  // Host-rate output of the linear-response mode (noise-free run plus the response, both decimated).
  bool RenderLin(const HitTuning& tu, HitNoise* noise, double seconds, double fsHost, std::vector<float>& out)
  {
    std::vector<double> ts, ys, tk, dyk; if (!RunLin(tu, noise, seconds, ts, ys, tk, dyk)) return false;
    const size_t want = static_cast<size_t>(seconds * fsHost); const double idleV = idle(); out.clear();
    HostDecimator dec, decR; dec.Init(fsHost); decR.Init(fsHost); std::vector<float> rsp; double s;
    for (size_t i = 0; i < ts.size(); ++i) { dec.Push(ts[i], ys[i] - idleV); while (dec.Pop(s)) out.push_back(static_cast<float>(s)); }
    while (dec.Pop(s)) out.push_back(static_cast<float>(s)); if (out.size() > want) out.resize(want);
    for (size_t i = 0; i < tk.size(); ++i) { decR.Push(tk[i], dyk[i]); while (decR.Pop(s)) rsp.push_back(static_cast<float>(s)); }
    while (decR.Pop(s)) rsp.push_back(static_cast<float>(s));
    for (size_t i = 0; i < rsp.size() && i < out.size(); ++i) out[i] += rsp[i];
    return true;
  }
  NoiseReplay<Mdl> replay_;
  bool Render(const HitTuning& tu, HitNoise* noise, double seconds, double fsHost, std::vector<float>& out)
  {
    if (tu.linNoiseT >= 0.0 && Mdl::kNT == 2 && noise) return RenderLin(tu, noise, seconds, fsHost, out);
    HostDecimator dec; dec.Init(fsHost); out.clear(); const size_t want = static_cast<size_t>(seconds * fsHost);
    auto drain = [&] { double s; while (dec.Pop(s)) out.push_back(static_cast<float>(s)); };
    const bool ok = RunSteps(tu, noise, seconds, [&](double t, double y) { dec.Push(t, y - idle()); drain(); });
    drain(); if (out.size() > want) out.resize(want);
    return ok;
  }

  double idle() const { return m_->yIdle; }
  long steps() const { return m_->steps; }
  long unrecovered() const { return m_->fails; }

private:
  static constexpr double kH0 = 1e-5 / 64.0;
  static constexpr double kStartTime = 0.05;   // the noise source runs before the trigger: the model starts from the state at the trigger time

  void Prepare(const HitTuning& tu)
  {
    // a new model state for every hit, the propagators of the previous hit are kept
    if (potStale_) { if (built_) { m_.reset(new Model()); } pj_.clear(); built_ = false; potStale_ = false; }   // the pot set changed: the propagators of the earlier hits belong to other matrices
    if (usePot_) m_->pm = pot_;   // before Init(): Init() / Reset() derive PD / PC from DgS
    if (built_) { auto props = std::move(m_->props); auto half = std::move(m_->halfIdx); m_.reset(new Model()); if (usePot_) m_->pm = pot_; m_->Init(); m_->props = std::move(props); m_->halfIdx = std::move(half); }
    else { m_->Init(); built_ = true; }
    Model& m = *m_;
    m.squareProps = tu.squareProps; m.dampOn = true; m.dampRatio = 0.01; m.noFallback = true; m.zohW = tu.zohW; m.chordFirst = tu.chordFirst; m.capThr = tu.capThr; m.fastLinAlg = tu.fastLinAlg; m.fastDot = true; m.fastSolve = true;
#ifdef DGG_HC_ENV_KNOBS   // test harness only: solver settings of the engine phase from the environment
    if (const char* e = std::getenv("PREDW")) m.predW = std::atof(e);
    if (const char* e = std::getenv("CHORDFIRST")) m.chordFirst = std::atof(e);
    if (const char* e = std::getenv("ASTOL")) m.asTol = std::atof(e);
    if (std::getenv("EVCACHE")) m.evCache = true;
    if (const char* e = std::getenv("ERRWNODE")) { int nd = 0; double w = 1.0; if (std::sscanf(e, "%d:%lf", &nd, &w) == 2 && nd >= 0 && nd < Mdl::kNS) m.nodeW[nd] = w; }
    if (const char* e = std::getenv("QLSKIP")) m.qlSkip = std::atoi(e);
    if (const char* e = std::getenv("QLEHOLD")) m.qleHold = std::atoi(e);
    if (const char* e = std::getenv("QLHOLD")) { m.qlHold = std::atoi(e); }
    if (const char* e = std::getenv("QLTOL")) m.qlTol = std::atof(e);
    if (const char* e = std::getenv("FRZCHK")) m.frzMode = std::atoi(e);
    if (const char* e = std::getenv("FRZMUL")) m.frzMul = std::atof(e);
    if (const char* e = std::getenv("FRZTHR")) m.frzThr = std::atof(e);
    if (const char* e = std::getenv("JACTOL")) m.jacTol = std::atof(e);
    if (std::getenv("KEEPH")) m.keepAcrossH = true;   // BD-K25: the factorisation is kept across step size changes
    if (const char* e = std::getenv("CAPTHR")) m.capThr = std::atof(e);
    if (const char* e = std::getenv("QLC")) { m.QlcEnable(std::atoi(e) != 0); if (const char* g = std::getenv("QLCERR")) m.qlcErr = std::atof(g); }
#endif
    m.Reset(Mdl::kAcc); m.PortGrpInit();
    if (!startZ_.empty()) m.SetState(startZ_.data(), startP_.data());
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
    { const double h0 = 1e-5 / 64.0; while ((int)pj_.size() <= tu.jMax) { const int j = (int)pj_.size(); pj_.push_back(m.Prepare(h0 * static_cast<double>(1 << j))); m.SetHalf(pj_[j], pj_[j - 1]); } }
  }

  std::vector<double> startZ_, startP_;   // start state of the next hit (SetStartState)
  std::unique_ptr<Model> m_; std::vector<int> pj_; bool built_ = false; typename Model::PotPtrs pot_{}; bool usePot_ = false, potStale_ = false;
  unsigned potVer_ = 0; HitTuning tu_; HitNoise* noise_ = nullptr; long long knotN_ = 1, noiseOffN_ = 1LL << 40, nEnd_ = 0, n_ = 0, preN_ = 0; int j_ = 0;
  double gFprev_[Mdl::kNF] = {0}, gHprev_ = 0.0, gEnv_ = 0.0, t_ = 0.0, y_ = 0.0;
};

}}  // namespace dgg::pcmfast
