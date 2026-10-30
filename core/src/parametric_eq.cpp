#include "parametric_eq.h"

#include <algorithm>
#include <cmath>

namespace sorus {

ParametricEq::ParametricEq() {
  BandParams p;
  p.enabled = false;
  params_.fill(p);
}

void ParametricEq::prepare(double sample_rate, int max_frames, int /*channels*/) {
  sr_ = sample_rate;
  max_frames_ = std::max(max_frames, 0);
  mid_.assign(static_cast<size_t>(max_frames_), 0.f);
  side_.assign(static_cast<size_t>(max_frames_), 0.f);
  for (int i = 0; i < kMaxBands; ++i) {
    bands_[i].prepare(sr_);
    bands_[i].set(params_[i]);
  }
}

void ParametricEq::set_band(int index, const BandParams& p) {
  if (index < 0 || index >= kMaxBands) return;
  params_[index] = clamp(p, sr_);
  bands_[index].set(params_[index]);
}

void ParametricEq::process(float* const* ch, int channels, int frames) {
  channels = std::min(channels, kMaxChannels);
  if (channels <= 0 || max_frames_ <= 0) return;
  for (int pos = 0; pos < frames; pos += max_frames_) {
    float* c[kMaxChannels];
    for (int i = 0; i < channels; ++i) c[i] = ch[i] + pos;
    run(c, channels, std::min(max_frames_, frames - pos));
  }
}

void ParametricEq::run(float* const* ch, int channels, int n) {
  bool ms = false;
  for (int i = 0; i < kMaxBands; ++i) {
    ChannelMode m = params_[i].mode;
    if (channels < 2) m = ChannelMode::Stereo;
    switch (m) {
      case ChannelMode::Stereo:
        for (int c = 0; c < channels; ++c) bands_[i].process(ch[c], n, c);
        break;
      case ChannelMode::Left: bands_[i].process(ch[0], n, 0); break;
      case ChannelMode::Right: bands_[i].process(ch[1], n, 1); break;
      case ChannelMode::Mid:
      case ChannelMode::Side: ms = true; break;
    }
  }
  if (!ms) return;
  float *l = ch[0], *r = ch[1], *mid = mid_.data(), *side = side_.data();
  for (int k = 0; k < n; ++k) {
    mid[k] = 0.5f * (l[k] + r[k]);
    side[k] = 0.5f * (l[k] - r[k]);
  }
  for (int i = 0; i < kMaxBands; ++i) {
    if (params_[i].mode == ChannelMode::Mid) bands_[i].process(mid, n, 0);
    else if (params_[i].mode == ChannelMode::Side) bands_[i].process(side, n, 1);
  }
  for (int k = 0; k < n; ++k) {
    l[k] = mid[k] + side[k];
    r[k] = mid[k] - side[k];
  }
}

double ParametricEq::response_db(double freq) const {
  double sum = 0;
  for (const auto& p : params_)
    if (p.enabled) sum += sorus::response_db(p, freq, sr_);
  return sum;
}

double ParametricEq::peak_boost_db() const {
  constexpr int kPoints = 512;
  constexpr double kLo = 10, kHi = 22000;
  double peak = 0;
  for (int i = 0; i < kPoints; ++i)
    peak = std::max(peak, response_db(kLo * std::pow(kHi / kLo, static_cast<double>(i) / (kPoints - 1))));
  return peak;
}

}  // namespace sorus
