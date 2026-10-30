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
  // Phase kOver-k is phase k mirrored (tap j <-> kTaps-1-j), so phases k and kOver-k come from one
  // folded pair of 16-tap filters (see h_).
  constexpr int half = kTaps / 2;
  double bound = 1;  // phase 0 reads the sample itself
  for (int k = 1; k <= kOver / 2; ++k) {
    double h[kTaps], sum = 0, abs_sum = 0;
    for (int j = 0; j < kTaps; ++j) {
      const double d = static_cast<double>(k) / kOver - (j - half + 1);
      sum += h[j] = std::sin(pi * d) / (pi * d) * 0.5 * (1 + std::cos(pi * d / half));
    }
    for (int j = 0; j < kTaps; ++j) abs_sum += std::abs(h[j] /= sum);
    bound = std::max(bound, abs_sum);
    for (int j = 0; j < half; ++j) {
      h_[j][k - 1] = static_cast<float>((h[j] + h[kTaps - 1 - j]) / 2);
      if (k < kOver / 2) h_[j][kOver / 2 + k - 1] = static_cast<float>((h[j] - h[kTaps - 1 - j]) / 2);
    }
  }
  // Float rounding puts each computed phase within ~1e-5 (relative to bound * max|x|) of its exact value.
  quiet_ = bound * (1 + 1e-4);

  const int lookahead = std::max(kGuard, static_cast<int>(std::lround(0.0015 * sample_rate)));
  box_ = lookahead + 1 - kGuard;
  hold_ = box_ + 2 * kGuard;
  // Peak detected at step n0 has the gain at or below its requirement on steps
  // [n0 + box_ - 1, n0 + box_ - 1 + 2*kGuard]; it is output in the middle of that range.
  latency_ = kDetectorDelay + box_ - 1 + kGuard;  // == kDetectorDelay + lookahead

  int size = 1;
  while (size <= std::max(latency_, kTaps)) size *= 2;
  mask_ = size - 1;
  for (auto& r : ring_) r.assign(2 * static_cast<size_t>(size), 0.0f);  // every channel: process may get up to 8
  pos_ = 0;
  n_ = 0;
  loud_ = -kTaps;  // silent history
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
  loud_ = n_;  // quiet_ tests so far assumed the old ceiling: detect until they leave the window
}

void TruePeakLimiter::process(float* const* ch, int channels, int frames) {
  channels = std::clamp(channels, 0, kMaxChannels);
  if (channels == 0 || box_v_.empty()) return;
  const int size = mask_ + 1;
  for (int i = 0; i < frames; ++i) {
    // ceiling_ glides monotonically to target_, so `lowest` bounds it for every step this sample is in
    // the detector window (a new target resets loud_).
    const double lowest = std::min(ceiling_, target_);
    for (int c = 0; c < channels; ++c) {
      float* r = ring_[static_cast<size_t>(c)].data();
      r[pos_] = r[pos_ + size] = ch[c][i];
      if (std::abs(ch[c][i]) * quiet_ >= lowest) loud_ = n_;
    }
    // True peak of the newest detectable point (input sample n - kDetectorDelay and the seven
    // positions after it), max over channels. Skipped while the whole window is quiet: it would be
    // below the ceiling anyway.
    // ponytail: lanes are 4-wide loops; clang/gcc vectorize them, MSVC (x64) keeps them scalar, so loud
    // input (detector on every sample) costs ~0.003 of a core more on Windows. Next step if that matters:
    // detect a chunk of samples per pass (sample-inner loops vectorize everywhere) or SSE2/NEON lanes.
    double peak = 0;
    if (n_ - loud_ < kTaps) {
      for (int c = 0; c < channels; ++c) {
        const float* x = ring_[static_cast<size_t>(c)].data() + ((pos_ - kTaps + 1) & mask_);
        constexpr int kHalf = kOver / 2;
        float y[kOver] = {};  // [k - 1]: (phase k + phase kOver-k) / 2; [kHalf + k - 1]: their half-difference
        for (int j = 0; j < kTaps / 2; ++j) {
          const float sum = x[j] + x[kTaps - 1 - j], diff = x[j] - x[kTaps - 1 - j], *h = h_[j];
          for (int k = 0; k < kHalf; ++k) y[k] += h[k] * sum;
          for (int k = kHalf; k < kOver; ++k) y[k] += h[k] * diff;
        }
        peak = std::max(peak, static_cast<double>(std::abs(x[kTaps - 1 - kDetectorDelay])));
        // max(|a + b|, |a - b|) == |a| + |b|: the larger of each mirrored pair of phases.
        for (int k = 0; k < kHalf; ++k)
          peak = std::max(peak, static_cast<double>(std::abs(y[k])) + std::abs(y[kHalf + k]));
      }
    }
    ceiling_ += ceiling_alpha_ * (target_ - ceiling_);
    const double required = peak > ceiling_ ? ceiling_ / peak : 1.0;

    // Sliding min over the last hold_ requirements (monotonic deque, amortised O(1)).
    if (dq_size_ > 0 && dq_t_[static_cast<size_t>(dq_head_)] <= n_ - hold_) {
      if (++dq_head_ == hold_) dq_head_ = 0;
      --dq_size_;
    }
    auto at = [&](int k) { k += dq_head_; return static_cast<size_t>(k < hold_ ? k : k - hold_); };
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
