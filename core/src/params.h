#pragma once

namespace sorus {

constexpr int kMaxBands = 32;
constexpr int kMaxChannels = 8;

enum class FilterType { Bell, LowShelf, HighShelf, LowPass, HighPass, Notch, BandPass, Tilt, AllPass };
enum class ChannelMode { Stereo, Left, Right, Mid, Side };

struct BandParams {
  FilterType type = FilterType::Bell;
  double freq = 1000;
  double gain_db = 0;
  double q = 0.7071;
  int slope_db_oct = 12;  // LowPass/HighPass only
  ChannelMode mode = ChannelMode::Stereo;
  bool enabled = true;
  bool operator==(const BandParams&) const = default;
};

// Clamps to spec ranges: freq [10, min(22000, 0.45*sr)], gain +-24 dB, q [0.1, 40],
// slope to the nearest multiple of 6 in [6, 96]. Non-finite values fall back to defaults.
BandParams clamp(BandParams p, double sample_rate);

}  // namespace sorus
