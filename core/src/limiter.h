#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "params.h"

namespace sorus {

// Lookahead brickwall limiter on the 8x-oversampled true peak (BS.1770 practice, finer grid), one gain
// linked across all channels. Gain computer, per input sample:
//   required r = min(1, ceiling / true peak)  ->  sliding min over B + 2*kGuard samples
//   ->  box-car mean over B samples  ->  instant attack / 50 ms release,
// with B = lookahead (1.5 ms) + 1 - kGuard. The box-car of a min-held signal sits at or below r
// for 2*kGuard + 1 samples; the audio delay centres the peak (and +-kGuard neighbours) on that
// plateau, so the gain is fully down before the peak leaves the delay line. With no true peak
// over the ceiling the gain is exactly 1 and the output is the input delayed by latency_frames().
class TruePeakLimiter {
 public:
  // ponytail: 8x grid: a peak can sit T/16 from the nearest phase, under-reading by up to
  // -20*log10(cos(pi*f/(8*fs))) = 0.17 dB at Nyquist (32-tap Hann phases also droop above 0.9 Nyquist).
  // Next step if that matters: 16x (0.04 dB) or a parabolic fit around the largest phase.
  static constexpr int kOver = 8;   // detector oversampling
  static constexpr int kTaps = 32;  // detector FIR taps per oversampled phase
  static constexpr int kDetectorDelay = kTaps / 2;
  static constexpr int kGuard = 8;  // neighbours on each side held at the peak's gain

  void prepare(double sample_rate, int max_frames, int channels);  // allocates; resets state
  // Clamped to [-24, 0] dBTP (non-finite -> -1). Before prepare: applied as-is; after: 20 ms glide.
  void set_ceiling_db(double db);  // audio thread safe
  int latency_frames() const { return latency_; }
  void process(float* const* ch, int channels, int frames);  // audio thread: no alloc

 private:
  float h_[kOver - 1][kTaps] = {};  // fractional phases 1/8 .. 7/8 (phase 0 is the sample itself)
  static constexpr double kDefaultCeiling = 0.89125093813374556;  // -1 dBTP
  double ceiling_ = kDefaultCeiling, target_ = kDefaultCeiling;   // linear
  double ceiling_alpha_ = 1, release_alpha_ = 1, gain_ = 1;
  int latency_ = 0, hold_ = 1, box_ = 1, mask_ = 0;
  int64_t n_ = 0;  // samples processed: sliding-min timestamps
  std::array<std::vector<float>, kMaxChannels> ring_;  // input history = detector taps + audio delay
  int pos_ = 0;
  // Sliding min (monotonic deque) over `hold_` required gains.
  std::vector<int64_t> dq_t_;
  std::vector<double> dq_v_;
  int dq_head_ = 0, dq_size_ = 0;
  // Box-car over `box_` held gains, kept as a sum of deficits (1 - g); exactly 0 when none are set.
  std::vector<double> box_v_;
  double box_sum_ = 0;
  int box_pos_ = 0, box_nonzero_ = 0;
};

}  // namespace sorus
