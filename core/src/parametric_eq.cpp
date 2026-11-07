#include "parametric_eq.h"

#include <algorithm>
#include <cmath>

namespace sorus {

namespace {
// Slots a band touches: bit s = slot s. Mid uses slot 0, Side slot 1; one channel acts as Stereo.
unsigned slot_mask(ChannelMode m, int channels) {
  if (channels < 2) m = ChannelMode::Stereo;
  switch (m) {
    case ChannelMode::Stereo: return (1u << channels) - 1;
    case ChannelMode::Right:
    case ChannelMode::Side: return 2;
    default: return 1;
  }
}
}  // namespace

ParametricEq::ParametricEq() {
  BandParams p;
  p.enabled = false;
  params_.fill(p);
  given_.fill(p);
}

void ParametricEq::prepare(double sample_rate, int max_frames, int channels) {
  sr_ = sample_rate;
  max_frames_ = std::max(max_frames, 0);
  channels_ = std::clamp(channels, 1, kMaxChannels);
  mid_.assign(static_cast<size_t>(max_frames_), 0.f);
  side_.assign(static_cast<size_t>(max_frames_), 0.f);
  for (int i = 0; i < kMaxBands; ++i) {
    given_[i] = params_[i];
    mode_[i] = params_[i].mode;
    pending_[i] = false;
    bands_[i].prepare(sr_);
    bands_[i].set(given_[i]);
  }
}

bool ParametricEq::settled(int i, int channels) const {
  const unsigned mask = slot_mask(mode_[i], channels);
  for (int s = 0; s < kMaxChannels; ++s)
    if ((mask >> s & 1u) && !bands_[i].settled(s)) return false;
  return true;
}

void ParametricEq::commit_mode(int i) {
  // Everything is at pass-through: drop all slot state (the new slots may be stale), then glide in.
  BandParams out = params_[i];
  out.enabled = false;
  bands_[i].reset();
  bands_[i].set(out);  // snaps to bypassed
  bands_[i].set(params_[i]);
  given_[i] = params_[i];
  mode_[i] = params_[i].mode;
  pending_[i] = false;
}

void ParametricEq::set_band(int index, const BandParams& p) {
  if (index < 0 || index >= kMaxBands) return;
  const int i = index;
  const BandParams c = clamp(p, sr_);
  if (c == params_[i] && !pending_[i]) return;  // unchanged: no re-glide
  params_[i] = c;
  if (params_[i].mode == mode_[i]) {  // routing unchanged (also cancels a pending switch)
    pending_[i] = false;
    given_[i] = params_[i];
    bands_[i].set(given_[i]);
  } else if (!pending_[i]) {  // glide out on the old routing, switch once settled
    pending_[i] = true;
    given_[i].enabled = false;
    bands_[i].set(given_[i]);
    if (settled(i, channels_)) commit_mode(i);
  }  // else: switch already pending, params_ holds the latest request
}

void ParametricEq::restart() {
  for (int i = 0; i < kMaxBands; ++i) commit_mode(i);
}

void ParametricEq::process(float* const* ch, int channels, int frames) {
  channels = std::min(channels, kMaxChannels);
  if (channels <= 0 || max_frames_ <= 0) return;
  channels_ = channels;
  for (int pos = 0; pos < frames; pos += max_frames_) {
    float* c[kMaxChannels];
    for (int i = 0; i < channels; ++i) c[i] = ch[i] + pos;
    run(c, channels, std::min(max_frames_, frames - pos));
  }
}

void ParametricEq::run(float* const* ch, int channels, int n) {
  const bool two = channels >= 2;
  bool ms = false;
  for (int i = 0; i < kMaxBands; ++i) {
    const ChannelMode m = mode_[i];
    if (two && (m == ChannelMode::Mid || m == ChannelMode::Side)) {
      ms = true;
      continue;
    }
    const unsigned mask = slot_mask(m, channels);
    if (mask == (1u << channels) - 1) {
      // Every slot: lets the band run them in pairs. ponytail: L/R/M/S bands run one recurrence at a
      // time (~1.6x slower per channel); pair them across bands if many single-channel bands matter.
      bands_[i].process(ch, channels, n);
      continue;
    }
    for (int s = 0; s < channels; ++s)
      if (mask >> s & 1u) bands_[i].process(ch[s], n, s);
  }
  if (ms) {
    float *l = ch[0], *r = ch[1], *mid = mid_.data(), *side = side_.data();
    for (int k = 0; k < n; ++k) {
      mid[k] = 0.5f * (l[k] + r[k]);
      side[k] = 0.5f * (l[k] - r[k]);
    }
    for (int i = 0; i < kMaxBands; ++i) {
      if (mode_[i] == ChannelMode::Mid) bands_[i].process(mid, n, 0);
      else if (mode_[i] == ChannelMode::Side) bands_[i].process(side, n, 1);
    }
    for (int k = 0; k < n; ++k) {
      l[k] = mid[k] + side[k];
      r[k] = mid[k] - side[k];
    }
  }
  for (int i = 0; i < kMaxBands; ++i)
    if (pending_[i] && settled(i, channels)) commit_mode(i);
}

double ParametricEq::response_db(double freq) const {
  double sum = 0;
  for (const auto& p : params_)
    if (p.enabled) sum += sorus::response_db(p, freq, sr_);
  return std::max(kFloorDb, sum);
}

double ParametricEq::peak_boost_db() const {
  constexpr int kPoints = 512;
  constexpr double kLo = 10;
  const double hi = std::min(22000.0, 0.45 * sr_);  // the band frequency range
  double peak = 0;
  for (int i = 0; i < kPoints; ++i)
    peak = std::max(peak, response_db(kLo * std::pow(hi / kLo, static_cast<double>(i) / (kPoints - 1))));
  for (const auto& p : params_)  // narrow bells can fall between grid points
    if (p.enabled) peak = std::max(peak, response_db(p.freq));
  return peak;
}

}  // namespace sorus
