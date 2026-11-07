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
  void Init(double fsHost) { fs = fsHost; dec.Init(fsHost); double c = 0; for (int i = 0; i < M::kLayoutN[0]; ++i) { c += M::kStepLen[M::kLayoutFirst[0] + i]; cum[i + 1] = c; } cum[0] = 0; state = Idle; nOut = 0; }
  void SetAccent(double a) { accent = a; }                     // volts at the accent input, applied at the next Trigger
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
  enum State { Idle, Playing, Fading };
  void Advance()
  {
    if (state == Playing)
    {
      const int p0 = voice.period, s0 = voice.stepIdx; const double y = voice.Step(); tLast = tBase + p0 * M::kPeriod + cum[s0 + 1]; dec.Push(tLast, y);
      if (voice.finished) { state = Fading; tFade = 0.0; flushNodes = 0; yEnd = y; }
    }
    else   // after the hit: the remaining output offset relaxes with the slow time constant of the circuit (0.22 s, measured for CRASH and RIDE); then idle nodes until the filter has flushed
    {
      tLast += kFadeStep; tFade += kFadeStep; const double off = (yEnd - yRef) * std::exp(-tFade / kRelaxTau);
      dec.Push(tLast, yRef + off);
      if (std::fabs(off) < kOffsetFloor && ++flushNodes > 200) state = Idle;
    }
  }
  static constexpr double kFadeStep = 5e-4, kRelaxTau = 0.22, kOffsetFloor = 2e-6;
  HostDecimator dec; double fs = 48000.0, cum[8] = {0}, accent = 4.064, level = 0.5, yRef = 0.0, yEnd = 0.0, tBase = 0.0, tLast = 0.0; long nOut = 0; int flushNodes = 0; double tFade = 0.0; State state = Idle;
};

}}  // namespace dgg::pcmfast
