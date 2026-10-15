#pragma once
// Level reference of the instrument voices: one jack-voltage -> sample factor for the whole kit, scaled by the mixing resistors of the IO sheet.
// Each instrument reaches the left and the right bus through its own resistor (12k, or 8.2k / 15k for LT, HT, CY, OH, CH). A pair is weighted
// by the power mean of 12k / R over both buses, so that a channel with the 12k / 12k pair has weight 1.
namespace dgg {

constexpr double kJackVoltToSample = 0.45;

// sqrt(((12 / 8.2)^2 + (12 / 15)^2) / 2)
constexpr double kMixWeightWide = 1.1793;

} // namespace dgg
