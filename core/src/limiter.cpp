#include "limiter.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace sorus {

namespace {

using std::numbers::pi;

double db_to_lin(double db) { return std::pow(10, db / 20); }

}  // namespace

void TruePeakLimiter::prepare(double sample_rate, int /*max_frames: block-size independent*/, int /*channels*/) {
  // Hann-windowed sinc, normalised to unity DC gain. Tap j reads input sample (n - kTaps + 1 + j);
  // phase k interpolates t = n - kDetectorDelay + k/kOver.
  constexpr int half = kTaps / 2;
  for (int k = 1; k < kOver; ++k) {
    double h[kTaps], sum = 0;
    for (int j = 0; j < kTaps; ++j) {
      const double d = static_cast<double>(k) / kOver - (j - half + 1);
      sum += h[j] = std::sin(pi * d) / (pi * d) * 0.5 * (1 + std::cos(pi * d / half));
    }
    for (int j = 0; j < kTaps; ++j) h_[k - 1][j] = static_cast<float>(h[j] / sum);
  }

  const int lookahead = std::max(kGuard, static_cast<int>(std::lround(0.0015 * sample_rate)));
  box_ = lookahead + 1 - kGuard;
  hold_ = box_ + 2 * kGuard;
  // Peak detected at step n0 has the gain at or below its requirement on steps
  // [n0 + box_ - 1, n0 + box_ - 1 + 2*kGuard]; it is output in the middle of that range.
  latency_ = kDetectorDelay + box_ - 1 + kGuard;  // == kDetectorDelay + lookahead

  int size = 1;
  while (size <= std::max(latency_, kTaps)) size *= 2;
  mask_ = size - 1;
  for (auto& r : ring_) r.assign(static_cast<size_t>(size), 0.0f);  // every channel: process may get up to 8
  pos_ = 0;
  n_ = 0;
  dq_t_.assign(static_cast<size_t>(hold_), 0);
  dq_v_.assign(static_cast<size_t>(hold_), 1.0);
  dq_head_ = dq_size_ = 0;
  box_v_.assign(static_cast<size_t>(box_), 0.0);
  box_sum_ = 0;
  box_pos_ = box_nonzero_ = 0;
  gain_ = 1;
  release_alpha_ = 1 - std::exp(-1 / (0.050 * sample_rate));
  ceiling_alpha_ = 1 - std::exp(-1 / (0.020 * sample_rate));
  ceiling_ = target_;
}

void TruePeakLimiter::set_ceiling_db(double db) {
  if (!std::isfinite(db)) db = -1;
  target_ = db_to_lin(std::clamp(db, -24.0, 0.0));
}

void TruePeakLimiter::process(float* const* ch, int channels, int frames) {
  channels = std::clamp(channels, 0, kMaxChannels);
  if (channels == 0 || box_v_.empty()) return;
  for (int i = 0; i < frames; ++i) {
    // True peak of the newest detectable point (input sample n - kDetectorDelay and the seven
    // positions after it), max over channels.
    double peak = 0;
    for (int c = 0; c < channels; ++c) {
      float* r = ring_[static_cast<size_t>(c)].data();
      r[pos_] = ch[c][i];
      float x[kTaps];
      for (int j = 0; j < kTaps; ++j) x[j] = r[(pos_ - kTaps + 1 + j) & mask_];
      peak = std::max(peak, static_cast<double>(std::abs(x[kTaps - 1 - kDetectorDelay])));
      for (const auto& h : h_) {
        float y = 0;
        for (int j = 0; j < kTaps; ++j) y += h[j] * x[j];
        peak = std::max(peak, static_cast<double>(std::abs(y)));
      }
    }
    ceiling_ += ceiling_alpha_ * (target_ - ceiling_);
    const double required = peak > ceiling_ ? ceiling_ / peak : 1.0;

    // Sliding min over the last hold_ requirements (monotonic deque, amortised O(1)).
    if (dq_size_ > 0 && dq_t_[static_cast<size_t>(dq_head_)] <= n_ - hold_) {
      if (++dq_head_ == hold_) dq_head_ = 0;
      --dq_size_;
    }
    auto at = [&](int k) { return static_cast<size_t>((dq_head_ + k) % hold_); };
    while (dq_size_ > 0 && dq_v_[at(dq_size_ - 1)] >= required) --dq_size_;
    dq_t_[at(dq_size_)] = n_;
    dq_v_[at(dq_size_)] = required;
    ++dq_size_;
    const double held = dq_v_[at(0)];

    // Box-car mean of the held gain: cannot rise above the min it was fed during attack.
    const double deficit = 1 - held;
    double& old = box_v_[static_cast<size_t>(box_pos_)];
    box_nonzero_ += (deficit != 0) - (old != 0);
    box_sum_ += deficit - old;
    old = deficit;
    if (++box_pos_ == box_) box_pos_ = 0;
    if (box_nonzero_ == 0) box_sum_ = 0;  // drop rounding residue: exactly unity when idle
    const double smooth = 1 - box_sum_ / box_;

    // Instant attack (already shaped by the box-car), 50 ms release; snap the last ulps so the
    // gain returns to exactly 1.
    if (smooth < gain_) gain_ = smooth;
    else if ((gain_ += release_alpha_ * (smooth - gain_)) > smooth - 1e-7) gain_ = smooth;

    const int out = (pos_ - latency_) & mask_;
    for (int c = 0; c < channels; ++c)
      ch[c][i] = static_cast<float>(ring_[static_cast<size_t>(c)][static_cast<size_t>(out)] * gain_);
    pos_ = (pos_ + 1) & mask_;
    ++n_;
  }
}

}  // namespace sorus
