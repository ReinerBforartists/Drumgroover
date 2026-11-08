// TR-909 PCM voices (Crash, Ride): the Tune pot (VR25 / VR27) sets the PCM clock of the voice (the oscillator IC59 / IC57, divided by 2 by IC60 / IC56).
// Ratio of the clock to the ROM data clock (the real 909 plays the ROM at the pot position 0.3: Crash 32352 Hz, Ride 32376 Hz; the replacement samples are stored at 48 kHz for that clock, kPeriod = 1/48000), MEASURED on the real 909 recordings (AudioRealism Crash909-tune000..100, Ride909-tune000..100): the ROM data cross-correlated with the recording,
// clock searched in 8 Hz steps (tools/_sweep/tr909_rom_ref/clock_fit.py), pot 0 .. 1 in steps of 0.05 (log interpolation between the 0.1 measurements).
// Measured clocks of the recordings (Hz), pot 0.0 .. 1.0 in steps of 0.1: Crash 28060 28352 30128 32352 34272 36630 38992 42048 46576 51856 53808, Ride 28024 28544 30056 32376 35000 36832 39816 43776 48120 54528 56040.
#pragma once
namespace dgg {

constexpr double kTr909CrashClockRatio[21] = {
  0.867335, 0.871836, 0.876360, 0.903392, 0.931256, 0.965016, 1.000000,
  1.029247, 1.059348, 1.095185, 1.132234, 1.168168, 1.205243, 1.251582,
  1.299704, 1.367896, 1.439664, 1.519077, 1.602869, 1.632759, 1.663206,
};
constexpr double kTr909RideClockRatio[21] = {
  0.865580, 0.873574, 0.881641, 0.904690, 0.928343, 0.963506, 1.000000,
  1.039735, 1.081048, 1.108980, 1.137633, 1.182819, 1.229801, 1.289507,
  1.352113, 1.417614, 1.486287, 1.582156, 1.684211, 1.707402, 1.730912,
};

// Clock ratio at the pot position x (0..1), linear between the table points; ride = true for the Ride, false for the Crash.
inline double Tr909PcmClockRatio(double x, bool ride)
{
  const double* t_ = ride ? kTr909RideClockRatio : kTr909CrashClockRatio;
  x = x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x); const double f = x * 20.0; int i = static_cast<int>(f); if (i > 19) i = 19;
  const double t = f - i; return t_[i] * (1.0 - t) + t_[i + 1] * t;
}

}  // namespace dgg
