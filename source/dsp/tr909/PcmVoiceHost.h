// Host-rate PCM voice (CRASH / RIDE): reduced model + host decimator + parameter inputs.
// Trigger(): starts a hit at the present position of the output stream (the first rendered sample is the instant of the trigger; the filter look-ahead is paid by
// simulating the voice ahead of the output, no audible latency). Retrigger while a hit plays restarts the PCM sequence and keeps the analog state.
// Level (0..1, the output potentiometer in the feedback of the output stage, 0.5 = the reference position) is a gain on the AC-coupled output: out = 2 * level * (y - yIdle).
// The output is in volts relative to the idle output of the voice at the accent of the hit (the slow DC drift of the circuit is part of the signal); after the voice has ended the
// remaining offset relaxes exponentially (tau 0.22 s) to zero.
#pragma once
#include "PcmFast.h"
#include "HostDecimator.h"

namespace dgg { namespace pcmfast {

template <class M> class PcmVoiceHost
{
public:
  void Init(double fsHost) { fs = fsHost; dec.Init(fsHost); if (voice.st.T == 0.0) voice.SetPeriod(M::kPeriod); RecomputeCum(); state = Idle; nOut = 0; }
  // PCM clock period of the hits (seconds; the nominal M::kPeriod scaled by the Tune pot): not while a hit plays.
  void SetPeriod(double period) { voice.SetPeriod(period); RecomputeCum(); }
  void SetAccent(double a) { accent = a; }                     // volts at the accent input, applied at the next Trigger
  // Hi-hat voices: the circuit is simulated down to a much lower level before the voice ends, and no synthetic relaxation of the output offset follows (kRelaxTau was measured on Crash and Ride only).
  void SetHatMode(bool hat) { voice.acEndLevel = hat ? kAcEndLevelHat : kAcEndLevel; relaxTau = hat ? 0.0 : kRelaxTau; }
  void SetLevel(double lv) { level = lv < 0.001 ? 0.001 : (lv > 1.0 ? 1.0 : lv); }
  bool Active() const { return state != Idle; }
  void Trigger()
  {
    if (state == Playing || state == Fading)   // retrigger: digital restart, analog state continues
    {
      voice.Retrigger(); state = Playing; tBase = tLast; return;
    }
    voice.acc = accent; voice.Reset(); yRef = voice.OutputNow(); state = Playing; tBase = nOut / fs; tLast = tBase;
    dec.Restart(tBase); dec.Push(tBase - 5e-5, yRef); dec.Push(tBase - 2.5e-5, yRef); dec.Push(tBase, yRef); tLast = tBase;
  }
  // render n host samples (volts); returns false when the voice is idle for the whole block
  bool Render(float* out, int n)
  {
    bool any = state != Idle;
    for (int i = 0; i < n; ++i)
    {
      double o;
      for (;;)
      {
        if (dec.Pop(o)) { out[i] = (float)(2.0 * level * (o - yRef)); break; }
        if (state == Idle) { out[i] = 0.0f; break; }
        Advance();
      }
      ++nOut;
    }
    return any;
  }
  long Position() const { return nOut; }
  Voice<M> voice;

private:
  void RecomputeCum() { double c = 0; for (int i = 0; i < M::kLayoutN[0]; ++i) { c += voice.st.h[M::kLayoutFirst[0] + i]; cum[i + 1] = c; } cum[0] = 0; }
  enum State { Idle, Playing, Fading };
  void Advance()
  {
    if (state == Playing)
    {
      const int p0 = voice.period, s0 = voice.stepIdx; const double y = voice.Step(); tLast = tBase + p0 * voice.ClockPeriod() + cum[s0 + 1]; dec.Push(tLast, y);
      if (voice.finished) { state = Fading; tFade = 0.0; flushNodes = 0; yEnd = y; }
    }
    else   // after the hit: the remaining output offset relaxes with the slow time constant of the circuit (0.22 s, measured for CRASH and RIDE); then idle nodes until the filter has flushed
    {
      tLast += kFadeStep; tFade += kFadeStep; const double off = relaxTau > 0.0 ? (yEnd - yRef) * std::exp(-tFade / relaxTau) : 0.0;
      dec.Push(tLast, yRef + off);
      if (std::fabs(off) < kOffsetFloor && ++flushNodes > 200) state = Idle;
    }
  }
  static constexpr double kFadeStep = 5e-4, kRelaxTau = 0.22, kOffsetFloor = 2e-6;
  HostDecimator dec; double relaxTau = kRelaxTau, fs = 48000.0, cum[8] = {0}, accent = 4.064, level = 0.5, yRef = 0.0, yEnd = 0.0, tBase = 0.0, tLast = 0.0; long nOut = 0; int flushNodes = 0; double tFade = 0.0; State state = Idle;
};

}}  // namespace dgg::pcmfast
