// Host-rate voice for the full-hit models (FullFast): one model per grid level of the output level, each a continuous process whose circuit state is carried through
// retriggers (the trigger is a time-dependent input of the model; nothing is restarted). A hit uses the two grid levels around the level at the trigger; their outputs,
// normalised to unit gain ((y - yIdle) / (2 level_k)) and multiplied by the current level (2 level) when they enter the decimator, are blended linearly in the level.
// Per model ("channel"): Dormant (circuit at its idle point, nothing running) -> Playing (trigger schedule, then free-running steps of growing size) -> Flushing -> Relaxing
// (coarse steps without output until the slow states have decayed) -> Dormant. A trigger on a Playing or Relaxing channel continues from its present circuit state.
// Time: the model clock is in host seconds; the trigger of a channel that is already ahead of the output (decimator look-ahead) takes effect at its present clock.
#pragma once
#ifndef RMF_KEND_LEVEL
#define RMF_KEND_LEVEL 1e-6
#endif
#ifndef RMF_KLEVEL_SLEW
#define RMF_KLEVEL_SLEW 0.0
#endif
#ifndef RMF_KLEVEL_EVERY
#define RMF_KLEVEL_EVERY 16
#endif
#ifndef RMF_KFINE_UNITS
#define RMF_KFINE_UNITS 32
#endif
#ifndef RMF_KTAPER_HOLD
#define RMF_KTAPER_HOLD 1
#endif
#ifndef RMF_KNOCHORD_SCHED
#define RMF_KNOCHORD_SCHED 1
#endif
#ifndef RMF_KLEVEL_REDO
#define RMF_KLEVEL_REDO 1
#endif
#ifndef RMF_KREDO_MINJ
#define RMF_KREDO_MINJ 0   // the last step is redone only if it was at least 2^RMF_KREDO_MINJ base steps
#endif
#ifndef RMF_KREDO_TOL
#define RMF_KREDO_TOL 1e-6
#endif
#ifndef RMF_KREDO_CHORD
#define RMF_KREDO_CHORD 1e-6
#endif
#ifndef RMF_KPRETAPER
#define RMF_KPRETAPER 0
#endif
#ifndef RMF_KFINE_CHORD
#define RMF_KFINE_CHORD 1e-3
#endif
#ifndef RMF_KFINE_TOL
#define RMF_KFINE_TOL 1e-4
#endif
#ifndef RMF_KLEVEL_EDGE
#define RMF_KLEVEL_EDGE 0
#endif
#ifndef RMF_KLEVEL_STATES
#define RMF_KLEVEL_STATES 1
#endif
#ifndef RMF_KSINGLE
#define RMF_KSINGLE 1
#endif
#ifndef RMF_KSINGLE_AT
#define RMF_KSINGLE_AT 0.5
#endif
#ifndef RMF_KRELAX_SEC
#define RMF_KRELAX_SEC 1.5   // seconds after the end of a voice until its circuit state is dropped (the next hit starts from the idle state)
#endif
#ifndef RMF_KRELAX_J
#define RMF_KRELAX_J 11   // step 2^RMF_KRELAX_J base steps while a finished voice relaxes
#endif
#ifndef RMF_KEDGE_STEPS
#define RMF_KEDGE_STEPS 8
#endif
#ifndef RMF_KHOLD_SPLIT
#define RMF_KHOLD_SPLIT 768
#endif
#ifndef RMF_KHOLD2
#define RMF_KHOLD2 4
#endif
#ifndef RMF_KHOLD_DEFAULT
#define RMF_KHOLD_DEFAULT 4
#endif
#include <memory>
#include <cstdio>
#ifdef RMF_HOST_PROFILE
#include <intrin.h>
#endif
#include <cstdint>
#include "RmFullFast.h"
#ifndef RMF_DEC_RATIO
#define RMF_DEC_RATIO 16   // decimator: linear interpolation where the node spacing changes by more than this factor (a step edge between sparse and dense nodes), see HostDecimator.h
#endif
#include "RmHostDecimator.h"

namespace dgg { namespace rmfast {
using namespace ::dgg::pcmfast;

struct FullModelBase
{
  virtual ~FullModelBase() {}
  virtual int Prepare(double h) = 0;
  virtual void Reset(double accent) = 0;
  virtual void SetAccent(double accent) = 0;
  virtual void GetState(double* z, double* p, double* f, double& accent) const = 0;
  virtual void SetState(const double* z, const double* p, const double* f, double accent) = 0;
  virtual double Step(int pi, double tr0, double tr1, double dtr) = 0;
  virtual void SetHalf(int pi, int ph) = 0;
  virtual long Retries() const = 0;
  virtual void SetDbgId(int id) = 0;
  virtual double IdleOut() const = 0;
  virtual long Its() const = 0;
  virtual long Facs() const = 0;
  virtual long Fails() const = 0;
  virtual long ModelSteps() const = 0;
  virtual double OutputNow() const = 0;
  virtual void SetNewtonTol(double t, double chordFirst) = 0;
  virtual void RedoLast(double t, double chordFirst) = 0;
  virtual void SetPredictor(bool on) = 0;
  virtual void SetNoChord(bool on) = 0;
  virtual void SoftStats(long& n, double& y, double& z, double& r) const = 0;
};
template <class M> struct FullModelImpl : FullModelBase
{
  FullModelImpl() { f.Init(); }
  int Prepare(double h) override { return f.Prepare(h); }
  void Reset(double accent) override { f.Reset(accent); }
  void SetAccent(double accent) override { f.acc = accent; }
  void GetState(double* z, double* p, double* fo, double& accent) const override { for (int i = 0; i < M::kR; ++i) { double v = 0.0; for (int j = 0; j < M::kR; ++j) v += M::kQ[i * M::kR + j] * f.z[j]; z[i] = v; }   /* canonical coordinates */ for (int i = 0; i < M::kNP; ++i) p[i] = f.p[i]; for (int i = 0; i < M::kNF; ++i) fo[i] = f.f0[i]; accent = f.acc; }
  void SetState(const double* z, const double* p, const double* fi, double accent) override { for (int j = 0; j < M::kR; ++j) { double v = 0.0; for (int i = 0; i < M::kR; ++i) v += M::kQ[i * M::kR + j] * z[i]; f.z[j] = v; } for (int i = 0; i < M::kNP; ++i) f.p[i] = p[i]; for (int i = 0; i < M::kNF; ++i) f.f0[i] = fi[i]; f.acc = accent; f.accLast = accent; f.luOk = false; }
  double Step(int pi, double tr0, double tr1, double dtr) override { return f.Step(pi, tr0, tr1, dtr); }
  void SetHalf(int pi, int ph) override { f.SetHalf(pi, ph); }
  long Retries() const override { return f.retries; }
  void SetDbgId(int id) override { f.dbgId = id; }
  double IdleOut() const override { return f.yIdle; }
  long Its() const override { return f.its; }
  long Facs() const override { return f.facs; }
  long Fails() const override { return f.fails; }
  long ModelSteps() const override { return f.steps; }
  double OutputNow() const override { return f.OutputNow(); }
  void SetNewtonTol(double t, double chordFirst) override { f.newtonTol = t; f.kChordFirst = chordFirst; }
  void RedoLast(double t, double chordFirst) override { f.RedoLast(t, chordFirst); }
  void SetNoChord(bool on) override { f.noChord = on; }
#if RMF_KPRED_ON
  void SetPredictor(bool on) override { f.predEnable = on; }
#else
  void SetPredictor(bool) override {}
#endif
  void SoftStats(long& n, double& y, double& z, double& r) const override { n = f.softAccepts; y = f.softMaxY; z = f.softMaxZ; r = f.softMaxRes; }
  FullFast<M> f;
};

template <class S> class FullLevelHost
{
public:
  static constexpr double kHc = 1.0 / 384000.0;      // base step; every step of the free-running model is kHc * 2^j
  static constexpr int kAccRampSteps = 40;           // accent change of a running channel: ramp over 40 base steps (104 us)
  static constexpr int kEdgeSteps = RMF_KEDGE_STEPS, kMaxJ = 8, kRelaxJ = RMF_KRELAX_J > 8 ? RMF_KRELAX_J : 8, kHold = RMF_KHOLD_DEFAULT;    // steps per 10 us trigger edge; largest free-running step 2^kMaxJ * kHc (also the relaxation step)
  void Init(double fsHost)
  {
    fs = fsHost; nOut = 0; pk[0] = pk[1] = -1;
    for (int k = 0; k < S::kN; ++k)
    {
      Chan& c = ch[k]; c.dec.Init(fsHost); c.m.reset(S::Make(k)); c.st = Dormant; c.w = 0.0; c.lv = S::kLv[k]; c.m->SetDbgId(k);
      const int pe = c.m->Prepare(1e-5 / kEdgeSteps);
      if (k == 0)
      {
        // trigger schedule: rise, hold (2 ms = 192 base steps), fall, pad step to the base grid
        sched.clear(); double t = 0.0; const double he = 1e-5 / kEdgeSteps;
        auto add = [&](double h, int pi) { Step s; s.h = h; s.pi = pi; s.tr0 = Trig(t); s.tr1 = Trig(t + h); s.dtr = (s.tr1 - s.tr0) / h; sched.push_back(s); t += h; };
        for (int i = 0; i < kEdgeSteps; ++i) add(he, pe);
        for (int i = 0; i < RMF_KHOLD_SPLIT / kHold; ++i) add(kHc * kHold, c.m->Prepare(kHc * kHold));
        for (int i = 0; i < (768 - RMF_KHOLD_SPLIT) / RMF_KHOLD2; ++i) add(kHc * RMF_KHOLD2, c.m->Prepare(kHc * RMF_KHOLD2));
        for (int i = 0; i < kEdgeSteps; ++i) add(he, pe);
        const double pad = std::ceil(t / kHc - 1e-9) * kHc - t; if (pad > 1e-12) add(pad, c.m->Prepare(pad));
        schedBase = (int)std::lround(t / kHc);
      }
      else { c.m->Prepare(1e-5 / kEdgeSteps); for (const Step& s : sched) c.m->Prepare(s.h); }
      for (int j = 0; j <= kRelaxJ; ++j) pj[j] = c.m->Prepare(kHc * (double)(1 << j));
      // retry chain of a step that does not converge: half and quarter step sizes
      { const double he = 1e-5 / kEdgeSteps; const int h1 = c.m->Prepare(he * 0.5), h2 = c.m->Prepare(he * 0.25); c.m->SetHalf(pe, h1); c.m->SetHalf(h1, h2); }
      { const int u1 = c.m->Prepare(kHc * 0.5), u2 = c.m->Prepare(kHc * 0.25); c.m->SetHalf(pj[0], u1); c.m->SetHalf(u1, u2); }
      for (int j = 1; j <= kRelaxJ; ++j) c.m->SetHalf(pj[j], pj[j - 1]);
      { const Step& last = sched.back(); if (std::fabs(last.h - 1e-5 / kEdgeSteps) > 1e-15) { const int hp = c.m->Prepare(last.h * 0.5); c.m->SetHalf(last.pi, hp); } }
      c.m->Reset(accent);
    }
  }
  void SetAccent(double a)
  {   // the accent voltage acts at once: a running channel moves to the new value in kAccRampSteps base steps from its model clock (the circuit sees a ramp, not a jump)
    accent = a; const double tNow = nOut / fs; double lag = 0.0;
    for (int k = 0; k < S::kN; ++k)
    {
      Chan& c = ch[k]; if ((c.st != Playing && c.st != Flushing) || c.accCur < 0.0) continue;
      lag = std::fmax(lag, c.tm - tNow);
      const double now = c.accRamp ? c.accFrom + (c.accTo - c.accFrom) * std::fmin(1.0, std::fmax(0.0, (c.tm - c.accT0) / (kAccRampSteps * kHc))) : c.accCur;
      if (std::fabs(a - now) <= 1e-9) continue;
      c.accFrom = now; c.accTo = a; c.accT0 = c.tm; c.accRamp = true;
    }
    if (lag > 0.0) dbgLag = lag;   // diagnosis: the accent acts on the stream from the model clock on
  }
  // A change of the level scales the output of the model by level / lv, but not the charges the output coupling capacitors carry: they are the physical state of the circuit and stay as they are.
  // The model of a level lives in coordinates in which these charges are 1 / scale of the physical ones (the output stage is linear in the level), so a change of the scale s_old -> s_new multiplies
  // the capacitor voltages in the model state by s_old / s_new (directions and functionals: level_states.py).
  void RescaleOutputStates()
  {
    for (int q = 0; q < 2; ++q)
    {
      if (pk[q] < 0) continue; Chan& c = ch[pk[q]]; const double sn = level / c.lv;
      if (c.st != Dormant && RMF_KLEVEL_STATES && std::fabs(c.sc / sn - 1.0) > 1e-12)
      {
        // physical voltage of a coupling capacitor = A + s * (model voltage - A) with A the level-independent part of its idle voltage (op-amp offset, not scaled by the level), s = level / lv;
        // the physical voltage does not jump when the level changes: the part above A is multiplied by s_old / s_new (A and the directions: level_states.py)
        double z[S::kR], p[S::kNP], f[S::kNF], a; c.m->GetState(z, p, f, a); const double ratio = c.sc / sn; const double* W = S::LevW(); const double* D = S::LevD(); const double* A = S::LevA();
        double dz[S::kR] = {0};
        for (int k = 0; k < S::kLevN; ++k) { double v = -A[k]; for (int i = 0; i < S::kR; ++i) v += W[k * S::kR + i] * z[i]; for (int i = 0; i < S::kR; ++i) dz[i] += (ratio - 1.0) * v * D[k * S::kR + i]; }
        for (int i = 0; i < S::kR; ++i) z[i] += dz[i];
        c.m->SetState(z, p, f, a);
      }
      c.sc = sn; if (RMF_KFINE_UNITS > 0 && (c.st == Playing || c.st == Flushing)) { c.sinceLevel = 0; c.m->SetNewtonTol(RMF_KFINE_TOL, RMF_KFINE_CHORD); c.m->SetPredictor(false); }
      if (RMF_KLEVEL_EDGE && (c.st == Playing || c.st == Flushing)) c.dec.Push(c.tm + 1e-9, (c.m->OutputNow() - c.yIdle) * level / c.lv);   // the step of the output at the change is a node of its own (otherwise it is spread over the next step)
    }
  }
  // The level knob of a running hit moves like a potentiometer, not like a switch: a change of more than kLevelSlew (natural logarithm per kLevelEvery output samples)
  // is slewed in steps of that size, so that the output stage sees small steps (a jump of the feedback resistor starts a transient of the output coupling that a model of the nearest grid level cannot follow).
  void SetLevel(double lv)
  {
    lv = lv < 0.001 ? 0.001 : (lv > 1.0 ? 1.0 : lv); levelTarget = lv; if (std::fabs(lv - level) < 1e-12) { slewing = false; return; }
    if (RMF_KLEVEL_SLEW > 0.0 && Active() && std::fabs(std::log(lv / level)) > RMF_KLEVEL_SLEW) { slewing = true; return; }
    slewing = false;
    if (RMF_KPRETAPER && Active()) { pend = true; pendLevel = lv; return; }   // the level takes effect when the step has shrunk to the base step (an abrupt reduction of the step by 64 x starts a transient of 0.5 mV in the output)
    ApplyLevel(lv);
  }
  void ApplyPending() { if (pend) { pend = false; pendReady = false; ApplyLevel(pendLevel); } }
  void ApplyLevel(double lv)
  {
    if (RMF_KLEVEL_REDO) for (int k = 0; k < S::kN; ++k) if (ch[k].st == Playing && ch[k].pos >= (int)sched.size() && ch[k].lastJ >= RMF_KREDO_MINJ) ch[k].m->RedoLast(RMF_KREDO_TOL, RMF_KREDO_CHORD);
    level = lv; UpdatePair(); RescaleOutputStates();
    if (kLogLevel) { double t = EventClock(); std::fprintf(stderr, "LVLOG %.9f %.9g\n",t, level); }
    const double tNow = nOut / fs; double lag = 0.0; for (int q = 0; q < 2; ++q) if (pk[q] >= 0 && ch[pk[q]].st != Dormant) lag = std::fmax(lag, ch[pk[q]].tm - tNow);
    dbgLag = lag;   // diagnosis: the level acts on the stream from the model clock on, i.e. this much later than the call
  }
  bool Active() const { for (int k = 0; k < S::kN; ++k) if (ch[k].st == Playing || ch[k].st == Flushing) return true; return false; }
  void Trigger()
  {
    ApplyPending(); UpdatePair(); const double tNow = nOut / fs;
    if (pk[0] >= 0) StartHit(ch[pk[0]], pw[0], tNow);
    if (pk[1] >= 0) StartHit(ch[pk[1]], pw[1], tNow);
  }
  bool Render(float* out, int n)
  {
    const bool any = Active();
    for (int i = 0; i < n; ++i)
    {
      if (pend && (pendReady || !Active())) ApplyPending();
      if (slewing && (nOut % RMF_KLEVEL_EVERY) == 0) SlewStep();
      double sum = 0.0; const double tOut = nOut / fs;
      for (int k = 0; k < S::kN; ++k)
      {
        Chan& c = ch[k]; if (c.st == Dormant && c.w == 0.0) continue;
        double o; if (Next(c, o)) sum += c.w * o;
        if ((i & 63) == 0 && c.st == Relaxing) Relax(c, tOut);
      }
      out[i] = (float)sum; ++nOut;
    }
    return any;
  }
  long long Position() const { return nOut; }
  bool Busy() const { for (int k = 0; k < S::kN; ++k) if (ch[k].st != Dormant) return true; return false; }   // a hit sounds or a finished voice still relaxes: the host must keep rendering (its clock is the output clock)
  long dbgSteps = 0; double dbgLag = 0.0;
  long DbgIts() const { long n = 0; for (int k = 0; k < S::kN; ++k) n += ch[k].m->Its(); return n; }
  long DbgRetries() const { long n = 0; for (int k = 0; k < S::kN; ++k) n += ch[k].m->Retries(); return n; }
  void DbgSoft(long& n, double& y, double& z, double& r) const { n = 0; y = z = r = 0; for (int k = 0; k < S::kN; ++k) { long a; double b, c, d; ch[k].m->SoftStats(a, b, c, d); n += a; y = y > b ? y : b; z = z > c ? z : c; r = r > d ? r : d; } }
  static inline bool kLogLevel = false; static inline double kPushT0 = 0.0, kPushT1 = 0.0;   // diagnosis: print the pushed samples of a time window
  double EventClock() const { double t = -1.0; for (int k = 0; k < S::kN; ++k) if (ch[k].st == Playing || ch[k].st == Flushing) t = t > ch[k].tm ? t : ch[k].tm; return t < 0.0 ? nOut / fs : t; }   // diagnosis: model clock at which a parameter change or trigger takes effect
  long DbgModelSteps() const { long n = 0; for (int k = 0; k < S::kN; ++k) n += ch[k].m->ModelSteps(); return n; }
  long DbgFails() const { long n = 0; for (int k = 0; k < S::kN; ++k) n += ch[k].m->Fails(); return n; }
  long DbgFacs() const { long n = 0; for (int k = 0; k < S::kN; ++k) n += ch[k].m->Facs(); return n; }   // diagnosis: how far the model clock of a running channel was ahead of the output at the last retrigger

private:
  void SlewStep()
  {
    const double r = std::log(levelTarget / level);
    if (!Active() || std::fabs(r) <= RMF_KLEVEL_SLEW) { slewing = false; ApplyLevel(levelTarget); }
    else ApplyLevel(level * std::exp(r > 0.0 ? RMF_KLEVEL_SLEW : -RMF_KLEVEL_SLEW));
  }
  double levelTarget = 0.5, pendLevel = 0.5; bool slewing = false, pend = false, pendReady = false;
  enum State { Dormant, Playing, Flushing, Relaxing };
  struct Step { double h, tr0, tr1, dtr; int pi; };
  struct Chan
  {
    std::unique_ptr<FullModelBase> m; HostDecimator dec; State st = Dormant; double w = 0.0, lv = 0.5, yIdle = 0.0, tm = 0.0, tEnd = 0.0; int pos = 0, nSince = 0, flush = 0, sinceLevel = -1, lastJ = 0, tapN = 0; double sc = 1.0; bool accRamp = false; double accFrom = 0.0, accTo = 0.0, accT0 = 0.0; double quietT = 0.0, accCur = -1.0;
  };
  static double Trig(double t)   // the engine's trigger source: 0 -> 5 V in 10 us, 2 ms high, 5 -> 0 V in 10 us
  {
    if (t < 0.0) return 0.0; if (t < 1e-5) return 5.0 * t / 1e-5; if (t < 1e-5 + 2e-3) return 5.0;
    if (t < 2e-5 + 2e-3) return 5.0 * (1.0 - (t - 1e-5 - 2e-3) / 1e-5); return 0.0;
  }
  void StartHit(Chan& c, double weight, double tNow)
  {
    c.w = weight; if (c.st == Playing || c.st == Flushing) dbgLag = c.tm - tNow; if (c.st == Dormant) c.sc = level / c.lv;
    if (c.st == Dormant)
    {
      c.m->Reset(accent); c.yIdle = c.m->IdleOut(); c.tm = tNow;
      c.dec.Restart(tNow); c.dec.Push(tNow - 5e-5, 0.0); c.dec.Push(tNow - 2.5e-5, 0.0); c.dec.Push(tNow, 0.0);
    }
    else if (c.st == Relaxing)
    {
      while (c.tm < tNow - 1e-12) { int j = kRelaxJ; while (j > 0 && c.tm + kHc * (double)(1 << j) > tNow + 1e-12) --j; c.m->Step(pj[j], 0.0, 0.0, 0.0); c.tm += kHc * (double)(1 << j); }
      c.dec.Restart(tNow); c.dec.Push(tNow - 5e-5, 0.0); c.dec.Push(tNow - 2.5e-5, 0.0); c.dec.Push(c.tm, 0.0);
    }
    if (c.st == Dormant || c.accCur < 0.0) c.m->SetAccent(accent);
    else if (std::fabs(accent - c.accCur) > 1e-9)
    {   // the accent voltage moves to its new value in kAccRampSteps base steps before the trigger edge (a jump of the input is more than the accent stage can follow in the model)
      const double a0 = c.accCur; for (int k = 1; k <= kAccRampSteps; ++k) { c.m->SetAccent(a0 + (accent - a0) * k / kAccRampSteps); const double y = c.m->Step(pj[0], 0.0, 0.0, 0.0); c.tm += kHc; c.dec.Push(c.tm, (y - c.yIdle) * level / c.lv); if (kPushT1 > 0.0 && c.tm >= kPushT0 && c.tm <= kPushT1) std::fprintf(stderr, "RAMP %.9f %.5f\n", c.tm, (y - c.yIdle) * level / c.lv); }
    }
    c.accCur = accent; c.accRamp = false;
    c.pos = 0; c.nSince = 0; c.quietT = 0.0; c.flush = 0; c.st = Playing;
  }
  // the two grid levels around the level, and the weight of the upper one
  void Bracket(double lv, int& ka, int& kb, double& w) const
  {
    const double lvT = lv < S::kLv[0] ? S::kLv[0] : lv;
    ka = 0; while (ka < S::kN - 2 && lvT >= S::kLv[ka + 1]) ++ka;
    w = S::kN > 1 ? (lvT - S::kLv[ka]) / (S::kLv[ka + 1] - S::kLv[ka]) : 0.0; w = w < 0.0 ? 0.0 : (w > 1.0 ? 1.0 : w);
#if RMF_KSINGLE
    w = w >= RMF_KSINGLE_AT ? 1.0 : 0.0;   // diagnosis: one model per hit, the nearest grid level; the output follows the level through the factor level / lv
#endif
    kb = S::kN > 1 ? ka + 1 : -1; if (w < 1e-9) kb = -1; else if (w > 1.0 - 1e-9) { ka = kb; kb = -1; w = 0.0; }
  }
  // The running hit lives in the channels of the current pair. When the level moves to another pair, the circuit state (capacitor charges, here also the output coupling) is handed on
  // to the new channels together with the output stream, and the old channels are retired: the level potentiometer changes the output stage, not the memory of the circuit.
  void UpdatePair()
  {
    int ka, kb; double w; Bracket(level, ka, kb, w);
    bool alive = false; for (int q = 0; q < 2; ++q) if (pk[q] >= 0 && ch[pk[q]].st != Dormant) alive = true;
    if (alive)
    {
      for (int q = 0; q < 2; ++q)
      {
        const int kn = q == 0 ? ka : kb; if (kn < 0 || (kn == pk[0] || kn == pk[1])) continue;
        int kd = -1; for (int o = 0; o < 2; ++o) if (pk[o] >= 0 && ch[pk[o]].st != Dormant && (kd < 0 || std::fabs(S::kLv[pk[o]] - S::kLv[kn]) < std::fabs(S::kLv[kd] - S::kLv[kn]))) kd = pk[o];
        if (kd >= 0) Transfer(ch[kd], ch[kn]);
      }
      for (int o = 0; o < 2; ++o) if (pk[o] >= 0 && pk[o] != ka && pk[o] != kb) { ch[pk[o]].st = Dormant; ch[pk[o]].w = 0.0; }
    }
    pk[0] = ka; pk[1] = kb; pw[0] = kb >= 0 ? 1.0 - w : 1.0; pw[1] = w;
    if (alive) { if (ka >= 0 && ch[ka].st != Dormant) ch[ka].w = pw[0]; if (kb >= 0 && ch[kb].st != Dormant) ch[kb].w = pw[1]; }
  }
  void Transfer(Chan& from, Chan& to)
  {
    double zt[S::kR], pt[S::kNP], ft[S::kNF], a; from.m->GetState(zt, pt, ft, a); to.m->SetState(zt, pt, ft, a);
    to.dec = from.dec; to.st = from.st; to.tm = from.tm; to.tEnd = from.tEnd; to.pos = from.pos; to.nSince = from.nSince; to.flush = from.flush; to.quietT = from.quietT; to.sc = from.sc; to.accCur = from.accCur; to.accRamp = from.accRamp; to.accFrom = from.accFrom; to.accTo = from.accTo; to.accT0 = from.accT0; to.yIdle = from.yIdle;
  }
  // coarse steps of a relaxing channel up to the output time; the slow states have decayed after kRelaxSeconds
  void Relax(Chan& c, double tOut)
  {
    const double hb = kHc * (double)(1 << kRelaxJ);
    while (c.tm + hb <= tOut) { c.m->Step(pj[kRelaxJ], 0.0, 0.0, 0.0); c.tm += hb; }
    if (tOut - c.tEnd > kRelaxSeconds) { c.st = Dormant; c.w = 0.0; }
  }
  bool Next(Chan& c, double& o)
  {
    for (;;)
    {
      if (c.dec.Pop(o)) return true;
      if (c.st != Playing && c.st != Flushing) return false;
      Advance(c);
    }
  }
#ifdef RMF_HOST_PROFILE
public:
  unsigned long long hp[4] = {0,0,0,0};
#define RMF_HP_T(n) { const unsigned long long t_ = __rdtsc(); hp[n] += t_ - hl_; hl_ = t_; }
  unsigned long long hl_ = 0;
private:
#else
#define RMF_HP_T(n)
#endif
  void Advance(Chan& c)
  {
#ifdef RMF_HOST_PROFILE
    hl_ = __rdtsc();
#endif
    if (c.st == Flushing) { c.tm += kHc; c.dec.Push(c.tm, 0.0); if (++c.flush > 400) { c.st = Relaxing; c.tEnd = c.tm; } return; }
    double h, y; int pi; ++dbgSteps;
    if (c.pos < (int)sched.size()) { const Step& s = sched[c.pos++]; h = s.h; AccStep(c, h); if (RMF_KNOCHORD_SCHED) c.m->SetNoChord(true); y = c.m->Step(s.pi, s.tr0, s.tr1, s.dtr); if (RMF_KNOCHORD_SCHED) c.m->SetNoChord(false); c.nSince = c.pos >= (int)sched.size() ? schedBase : 0; }
    else
    {
      int j = 0; while (j < kMaxJ && j < StepLimit(c.nSince) && (c.nSince & ((1 << (j + 1)) - 1)) == 0) ++j;
      if (pend)
      {   // shrink the step by one doubling per step before the pending change of the level
        const int cap = (c.lastJ > 0 && (++c.tapN % RMF_KTAPER_HOLD) == 0) ? c.lastJ - 1 : c.lastJ; if (j > cap) j = cap; if (j == 0) pendReady = true;
      }
      if (c.sinceLevel >= 0)
      {   // after a change of the level the circuit is resolved finely and the step grows back by one doubling per doubling of the time since the change (feedback resistor with its 220 pF, transient of the output stage)
        int cap = 0; for (int q = c.sinceLevel / RMF_KFINE_UNITS + 1; q > 1; q >>= 1) ++cap;
        if (cap >= kMaxJ) { c.sinceLevel = -1; c.m->SetNewtonTol(RMF_NEWTON_TOL_DEFAULT, RMF_KCHORD_FIRST); c.m->SetPredictor(true); } else { if (j > cap) j = cap; c.sinceLevel += 1 << j; }
      }
      c.lastJ = j; h = kHc * (double)(1 << j); pi = pj[j]; AccStep(c, h); y = c.m->Step(pi, 0.0, 0.0, 0.0); c.nSince += 1 << j;
    }
    RMF_HP_T(0)
    c.tm += h; const double dev = y - c.yIdle; c.dec.Push(c.tm, dev * level / c.lv); RMF_HP_T(1)
    if (kPushT1 > 0.0 && c.tm >= kPushT0 && c.tm <= kPushT1) std::fprintf(stderr, "PUSH %.9f %.5f (model %.5f scale %.4f h %.1f us)\n", c.tm, dev * level / c.lv, y, level / c.lv, h * 1e6);
    if (c.pos >= (int)sched.size() && std::fabs(dev) < kEndLevel) { c.quietT += h; if (c.quietT >= kEndSeconds) c.st = Flushing; } else c.quietT = 0.0;
  }
  // accent value at the end of the coming step of length h while a running channel ramps to a new accent
  void AccStep(Chan& c, double h)
  {
    if (!c.accRamp) return;
    const double u = (c.tm + h - c.accT0) / (kAccRampSteps * kHc);
    if (u >= 1.0) { c.m->SetAccent(c.accTo); c.accCur = c.accTo; c.accRamp = false; } else c.m->SetAccent(c.accFrom + (c.accTo - c.accFrom) * (u < 0.0 ? 0.0 : u));
  }
  // free-running step size 2^j * base as a function of the time since the trigger (units of the base step)
  static int StepLimit(int nSince)
  {
    int j = 0; for (int i = 0; i < 8; ++i) { if (nSince >= kLim[i]) j = i + 1; }
    return j;
  }
  static constexpr int kLimDefault[6] = {1920, 7680, 38400, 76800, 153600, 1 << 30};   // 5 / 20 / 100 / 200 / 400 ms: from there on the step is doubled (units of the base step)
public:
  static inline int kLim[8] = {144, 288, 576, 1152, 2304, 4800, 38400, 153600};   // tuning access; RM-K16: 2880 / 9600 / never / never before, 2.8 -> 2.6 ms per hit, dynamic regression seq_all 0 FAIL
private:
  static constexpr double kEndLevel = RMF_KEND_LEVEL, kEndSeconds = 0.005, kRelaxSeconds = RMF_KRELAX_SEC;
  Chan ch[S::kN]; std::vector<Step> sched; int schedBase = 0, pj[kRelaxJ + 1] = {0};
  int pk[2] = {-1, -1}; double pw[2] = {1.0, 0.0};   // current pair of level channels and their weights
  double fs = 48000.0, accent = 4.064, level = 0.5; long long nOut = 0;   // 64 bit: a 32 bit count overflows after 12 h at 48 kHz
};

}}  // namespace dgg::rmfast
