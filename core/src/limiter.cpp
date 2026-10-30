#include "limiter.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#define SORUS_LIMITER_SSE2 1
#elif defined(__ARM_NEON) || defined(_M_ARM64)
#include <arm_neon.h>
#define SORUS_LIMITER_NEON 1
#endif

namespace sorus {

namespace {

using std::numbers::pi;

double db_to_lin(double db) { return std::pow(10, db / 20); }

// Four floats: SSE2 / NEON, else a plain array (same arithmetic, same rounding: no fused multiply-add).
// max(c, keep) ignores a NaN `c` in every backend.
#if defined(SORUS_LIMITER_SSE2)
struct F4 {
  __m128 v;
};
F4 load(const float* p) { return {_mm_loadu_ps(p)}; }
void store(float* p, F4 a) { _mm_storeu_ps(p, a.v); }
F4 operator+(F4 a, F4 b) { return {_mm_add_ps(a.v, b.v)}; }
F4 operator-(F4 a, F4 b) { return {_mm_sub_ps(a.v, b.v)}; }
F4 operator*(F4 a, F4 b) { return {_mm_mul_ps(a.v, b.v)}; }
F4 abs(F4 a) { return {_mm_andnot_ps(_mm_set1_ps(-0.0f), a.v)}; }
F4 max(F4 c, F4 keep) { return {_mm_max_ps(c.v, keep.v)}; }  // c > keep ? c : keep
F4 zero() { return {_mm_setzero_ps()}; }
#elif defined(SORUS_LIMITER_NEON)
struct F4 {
  float32x4_t v;
};
F4 load(const float* p) { return {vld1q_f32(p)}; }
void store(float* p, F4 a) { vst1q_f32(p, a.v); }
F4 operator+(F4 a, F4 b) { return {vaddq_f32(a.v, b.v)}; }
F4 operator-(F4 a, F4 b) { return {vsubq_f32(a.v, b.v)}; }
F4 operator*(F4 a, F4 b) { return {vmulq_f32(a.v, b.v)}; }
F4 abs(F4 a) { return {vabsq_f32(a.v)}; }
F4 max(F4 c, F4 keep) { return {vmaxnmq_f32(c.v, keep.v)}; }
F4 zero() { return {vdupq_n_f32(0)}; }
#else
struct F4 {
  float v[4];
};
template <class Op>
F4 map(F4 a, F4 b, Op op) {
  for (int i = 0; i < 4; ++i) a.v[i] = op(a.v[i], b.v[i]);
  return a;
}
F4 load(const float* p) { return {{p[0], p[1], p[2], p[3]}}; }
void store(float* p, F4 a) { std::copy(a.v, a.v + 4, p); }
F4 operator+(F4 a, F4 b) { return map(a, b, [](float x, float y) { return x + y; }); }
F4 operator-(F4 a, F4 b) { return map(a, b, [](float x, float y) { return x - y; }); }
F4 operator*(F4 a, F4 b) { return map(a, b, [](float x, float y) { return x * y; }); }
F4 abs(F4 a) { return map(a, a, [](float x, float) { return std::abs(x); }); }
F4 max(F4 c, F4 keep) { return map(c, keep, [](float x, float y) { return x > y ? x : y; }); }
F4 zero() { return {}; }
#endif

}  // namespace

void TruePeakLimiter::prepare(double sample_rate, int /*max_frames: block-size independent*/, int /*channels*/) {
  // Hann-windowed sinc, normalised to unity DC gain. Tap j reads input sample (n - kTaps + 1 + j);
  // phase k interpolates t = n - kDetectorDelay + k/kOver.
  // Phase kOver-k is phase k mirrored (tap j <-> kTaps-1-j), so phases k and kOver-k come from one
  // folded pair of 16-tap filters (see h_).
  constexpr int half = kTaps / 2;
  auto set = [&](int j, int lane, double v) { std::fill_n(h_[j][lane], 4, static_cast<float>(v)); };
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
      set(j, k - 1, (h[j] + h[kTaps - 1 - j]) / 2);
      if (k < kOver / 2) set(j, kOver / 2 + k - 1, (h[j] - h[kTaps - 1 - j]) / 2);
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
  // Room for the output delay plus a chunk written ahead; the mirror then covers every chunk window.
  while (size < latency_ + kChunk + kTaps) size *= 2;
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

void TruePeakLimiter::detect(float* peak, int channels, int n) const {
  constexpr int kHalf = kOver / 2;
  for (int c = 0; c < channels; ++c) {
    // Step i's window is x[i .. i + kTaps - 1]: contiguous thanks to the mirrored ring. Four steps at a
    // time; steps past n (up to kChunk) read older ring samples and are discarded.
    const float* x = ring_[static_cast<size_t>(c)].data() + ((pos_ - kTaps + 1) & mask_);
    for (int i = 0; i < n; i += 4) {
      // Lane k - 1: (phase k + phase kOver-k) / 2; lane kHalf + k - 1: their half-difference (k < kHalf).
      F4 y[kOver - 1];
      for (auto& v : y) v = zero();
      for (int j = 0; j < kTaps / 2; ++j) {
        const F4 a = load(x + i + j), b = load(x + i + (kTaps - 1 - j)), sum = a + b, diff = a - b;
        for (int k = 0; k < kHalf; ++k) y[k] = y[k] + load(h_[j][k]) * sum;
        for (int k = kHalf; k < kOver - 1; ++k) y[k] = y[k] + load(h_[j][k]) * diff;
      }
      F4 p = abs(load(x + i + (kTaps - 1 - kDetectorDelay)));
      // max(|a + b|, |a - b|) == |a| + |b|: the larger of each mirrored pair of phases.
      for (int k = 0; k < kHalf - 1; ++k) p = max(abs(y[k]) + abs(y[kHalf + k]), p);
      p = max(abs(y[kHalf - 1]), p);
      store(peak + i, max(p, load(peak + i)));
    }
  }
}

void TruePeakLimiter::process(float* const* ch, int channels, int frames) {
  channels = std::clamp(channels, 0, kMaxChannels);
  if (channels == 0 || box_v_.empty()) return;
  const int size = mask_ + 1;
  for (int base = 0; base < frames; base += kChunk) {
    const int n = std::min(kChunk, frames - base);
    // ceiling_ glides monotonically to target_, so `lowest` bounds it for every step these samples are
    // in the detector window (a new target resets loud_).
    const double lowest = std::min(ceiling_, target_);
    for (int c = 0; c < channels; ++c) {
      float* r = ring_[static_cast<size_t>(c)].data();
      const float* in = ch[c] + base;
      for (int i = 0; i < n; ++i) {
        const int p = (pos_ + i) & mask_;
        r[p] = r[p + size] = in[i];
        if (std::abs(in[i]) * quiet_ >= lowest) loud_ = std::max(loud_, n_ + i);
      }
    }
    // True peak per step: the newest detectable point (input sample n - kDetectorDelay and the seven
    // positions after it), max over channels. Skipped while no window of the chunk holds a loud sample:
    // every peak would be under the ceiling, so the gain is the same.
    float peak[kChunk] = {};
    if (loud_ > n_ - kTaps) detect(peak, channels, n);

    for (int i = 0; i < n; ++i) {
      ceiling_ += ceiling_alpha_ * (target_ - ceiling_);
      const double required = peak[i] > ceiling_ ? ceiling_ / peak[i] : 1.0;

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

      // Written ahead by up to kChunk samples: the ring is sized so this one is still intact.
      const int out = (pos_ - latency_) & mask_;
      for (int c = 0; c < channels; ++c)
        ch[c][base + i] = static_cast<float>(ring_[static_cast<size_t>(c)][static_cast<size_t>(out)] * gain_);
      pos_ = (pos_ + 1) & mask_;
      ++n_;
    }
  }
}

}  // namespace sorus
