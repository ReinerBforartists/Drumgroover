// TR-909 PCM voices (Crash, Ride): the Tune pot (VR25 / VR27) sets the PCM clock of the voice (the oscillator IC59 / IC57, two NAND gates with C131 / C130 470 pF, R442 / R440 10k, R443 / R441 6.8k and the 10k pot, divided by 2 by IC60 / IC56).
// Ratio of the clock to its value at the nominal wiper position 0.5, from NGSpice (tools/_sweep/tom_opt/ng_pcm_clock.py, CD4069UB inverter model of the project as the gate model), wiper 0 .. 1 in steps of 0.05.
#pragma once
namespace dgg {

constexpr double kTr909PcmClockRatio[21] = {
  0.726370, 0.746638, 0.768099, 0.790853, 0.814929, 0.840923, 0.868411,
  0.897957, 0.929431, 0.963363, 1.000000, 1.039673, 1.082596, 1.129080,
  1.180294, 1.236384, 1.297986, 1.365960, 1.441601, 1.526496, 1.622241,
};

// Clock ratio at the pot position x (0..1), linear between the table points.
inline double Tr909PcmClockRatio(double x)
{
  x = x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x); const double f = x * 20.0; int i = static_cast<int>(f); if (i > 19) i = 19;
  const double t = f - i; return kTr909PcmClockRatio[i] * (1.0 - t) + kTr909PcmClockRatio[i + 1] * t;
}

}  // namespace dgg
