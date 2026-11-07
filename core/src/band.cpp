#include "band.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <complex>
#include <numbers>

namespace sorus {

namespace {

using std::numbers::pi;

// Section coefficients per Cytomic "SvfLinearTrapOptimised2". `w` = tan(pi*freq/sr), `k` = 1/Q.
SvfSection bell(double w, double q, double gain_db) {
  const double a = std::pow(10, gain_db / 40), k = 1 / (q * a);
  return {w, k, 1, k * (a * a - 1), 0};
}
SvfSection low_shelf(double w, double q, double gain_db) {
  const double a = std::pow(10, gain_db / 40), k = 1 / q;
  return {w / std::sqrt(a), k, 1, k * (a - 1), a * a - 1};
}
SvfSection high_shelf(double w, double q, double gain_db) {
  const double a = std::pow(10, gain_db / 40), k = 1 / q;
  return {w * std::sqrt(a), k, a * a, k * (1 - a) * a, 1 - a * a};
}

// Butterworth LP/HP of order N = slope/6 (q ignored). Pole pairs sit at angle
// pi*(2m + 1 + N%2)/(2N) from the negative real axis; odd N adds the real pole as a
// critically damped (k = 2) section with numerator (s + 1) -> exactly 1/(1 + s), i.e. a TPT one-pole.
int butterworth(double w, int order, bool high, SvfSection* out) {
  int n = 0;
  for (int m = 0; m < order / 2; ++m) {
    const double k = 2 * std::cos(pi * (2 * m + 1 + order % 2) / (2 * order));
    out[n++] = high ? SvfSection{w, k, 1, -k, -1} : SvfSection{w, k, 0, 0, 1};
  }
  if (order % 2) out[n++] = high ? SvfSection{w, 2, 1, -1, -1} : SvfSection{w, 2, 0, 1, 1};
  return n;
}

// `p` must already be clamped. Returns the number of sections written.
int design(const BandParams& p, double sr, SvfSection* out) {
  const double w = std::tan(pi * p.freq / sr), k = 1 / p.q;
  switch (p.type) {
    case FilterType::Bell: out[0] = bell(w, p.q, p.gain_db); return 1;
    case FilterType::LowShelf: out[0] = low_shelf(w, p.q, p.gain_db); return 1;
    case FilterType::HighShelf: out[0] = high_shelf(w, p.q, p.gain_db); return 1;
    case FilterType::LowPass: return butterworth(w, p.slope_db_oct / 6, false, out);
    case FilterType::HighPass: return butterworth(w, p.slope_db_oct / 6, true, out);
    case FilterType::Notch: out[0] = {w, k, 1, -k, 0}; return 1;
    case FilterType::BandPass: out[0] = {w, k, 0, k, 0}; return 1;  // 0 dB peak
    case FilterType::AllPass: out[0] = {w, k, 1, -2 * k, 0}; return 1;
    case FilterType::Tilt:
      out[0] = low_shelf(w, p.q, -p.gain_db / 2);
      out[1] = high_shelf(w, p.q, p.gain_db / 2);
      return 2;
  }
  return 0;
}

}  // namespace

double response_db(const BandParams& raw, double freq, double sample_rate) {
  const BandParams p = clamp(raw, sample_rate);
  if (!p.enabled) return 0;
  SvfSection sec[Band::kMaxSections];
  const int n = design(p, sample_rate, sec);
  // Trapezoidal SVF == bilinear transform: z = e^{jw} maps to analog s = j*tan(w/2)/g.
  const double t = std::tan(pi * freq / sample_rate);
  std::complex<double> h = 1;
  for (int i = 0; i < n; ++i) {
    const SvfSection& c = sec[i];
    const std::complex<double> s(0, t / c.g), d = s * s + c.k * s + 1.0;
    h *= c.m0 + (c.m1 * s + c.m2) / d;
  }
  return 20 * std::log10(std::abs(h));
}

void Band::prepare(double sample_rate) {
  sr_ = sample_rate;
  alpha_ = 1 - std::exp(-1 / (0.010 * sample_rate));  // tau = 10 ms
  settle_ = static_cast<int>(0.2 * sample_rate);
  live_ = 0;
  reset();
}

void Band::Slot::clear_state() {
  std::fill(std::begin(ic1), std::end(ic1), 0.0);
  std::fill(std::begin(ic2), std::end(ic2), 0.0);
}

void Band::reset() {
  for (auto& s : slots_) s.clear_state();
  snap_ = true;
}

void Band::set(const BandParams& raw) {
  const BandParams p = clamp(raw, sr_);
  const bool was_enabled = enabled_;
  enabled_ = p.enabled;
  SvfSection t[kMaxSections];
  const int n = design(p, sr_, t);
  for (int i = 0; i < kMaxSections; ++i) {
    if (i < n) target_[i] = t[i];
    if (i >= n || !enabled_) {  // unused or disabled: keep g/k, glide the mix to pass-through
      target_[i].m0 = 1;
      target_[i].m1 = target_[i].m2 = 0;
    }
  }
  for (int i = 0; i < kMaxSections; ++i) {
    const SvfSection& c = target_[i];
    const double a1 = 1 / (1 + c.g * (c.g + c.k)), a2 = c.g * a1, a3 = c.g * a2;
    steady_[i] = {2 * a1 - 1, -2 * a2, 2 * a2, 1 - 2 * a3, 2 * a2, 2 * a3,
                  c.m0 + c.m1 * a2 + c.m2 * a3, c.m1 * a1 + c.m2 * a2, c.m2 * (1 - a3) - c.m1 * a2};
  }
  if (snap_) {
    for (auto& s : slots_) {
      std::copy(std::begin(target_), std::end(target_), std::begin(s.cur));
      s.bypassed = !enabled_;
      s.steady = enabled_;
      s.settle = 0;
    }
    live_ = n;
    snap_ = false;
    return;
  }
  // ponytail: sections only retire on the next snap (reset/prepare); idle ones still cost a tick each.
  live_ = std::max(live_, n);
  for (auto& s : slots_) {
    if (enabled_ || was_enabled) s.settle = settle_;  // re-arm the glide; a disabled band keeps gliding out
    s.steady = false;
    if (!was_enabled && enabled_ && s.bypassed) {  // fully bypassed: start clean from pass-through
      s.clear_state();
      s.bypassed = false;
    }
  }
}

void Band::Slot::heal(int live) {
  for (int j = 0; j < live; ++j)
    if (!std::isfinite(ic1[j]) || !std::isfinite(ic2[j])) return clear_state();
}

void Band::process(float* buf, int frames, int slot) {
  assert(slot >= 0 && slot < kMaxSlots);
  Slot& st = slots_[slot];
  if (st.bypassed) return;
  if (st.steady) {
    Slot* s[1] = {&st};
    run_steady<1>(s, &buf, frames);
  } else {
    glide(st, buf, frames);
    if ((st.settle -= frames) <= 0) {  // within e^-20 of the target: snap and stop smoothing
      std::copy(std::begin(target_), std::end(target_), std::begin(st.cur));
      st.bypassed = !enabled_;  // mix has reached pass-through: stop ticking
      st.steady = enabled_;
    }
  }
  st.heal(live_);
}

void Band::process(float* const* bufs, int count, int frames) {
  assert(count >= 0 && count <= kMaxSlots);
  int s = 0;
  for (; s + 1 < count; s += 2) {
    if (slots_[s].steady && slots_[s + 1].steady) {
      Slot* st[2] = {&slots_[s], &slots_[s + 1]};
      run_steady<2>(st, bufs + s, frames);
      st[0]->heal(live_);
      st[1]->heal(live_);
    } else {
      process(bufs[s], frames, s);
      process(bufs[s + 1], frames, s + 1);
    }
  }
  if (s < count) process(bufs[s], frames, s);
}

void Band::glide(Slot& st, float* buf, int frames) {
  for (int i = 0; i < frames; ++i) {
    double v = buf[i];
    for (int j = 0; j < live_; ++j) {
      SvfSection& c = st.cur[j];
      const SvfSection& t = target_[j];
      c.g += alpha_ * (t.g - c.g);
      c.k += alpha_ * (t.k - c.k);
      c.m0 += alpha_ * (t.m0 - c.m0);
      c.m1 += alpha_ * (t.m1 - c.m1);
      c.m2 += alpha_ * (t.m2 - c.m2);
      const double a1 = 1 / (1 + c.g * (c.g + c.k)), a2 = c.g * a1, a3 = c.g * a2;
      double& ic1 = st.ic1[j];
      double& ic2 = st.ic2[j];
      const double v3 = v - ic2, v1 = a1 * ic1 + a2 * v3, v2 = ic2 + a2 * ic1 + a3 * v3;
      ic1 = 2 * v1 - ic1;
      ic2 = 2 * v2 - ic2;
      v = c.m0 * v + c.m1 * v1 + c.m2 * v2;
    }
    buf[i] = static_cast<float>(v);
  }
}

template <int N>
void Band::run_steady(Slot* const* st, float* const* buf, int frames) const {
  constexpr int kChunk = 64;  // sections chain in double, as in glide()
  double v[N][kChunk];
  for (int pos = 0; pos < frames; pos += kChunk) {
    const int n = std::min(kChunk, frames - pos);
    for (int c = 0; c < N; ++c)
      for (int i = 0; i < n; ++i) v[c][i] = buf[c][pos + i];
    for (int j = 0; j < live_; ++j) {
      const Steady k = steady_[j];
      double s1[N], s2[N];  // locals: no aliasing with v/buf, so the state stays in registers
      for (int c = 0; c < N; ++c) s1[c] = st[c]->ic1[j], s2[c] = st[c]->ic2[j];
      for (int i = 0; i < n; ++i) {
        for (int c = 0; c < N; ++c) {
          const double x = v[c][i];
          v[c][i] = k.d0 * x + k.d1 * s1[c] + k.d2 * s2[c];
          const double t = k.c11 * s1[c] + k.c12 * s2[c] + k.b1 * x;
          s2[c] = k.c21 * s1[c] + k.c22 * s2[c] + k.b2 * x;
          s1[c] = t;
        }
      }
      for (int c = 0; c < N; ++c) st[c]->ic1[j] = s1[c], st[c]->ic2[j] = s2[c];
    }
    for (int c = 0; c < N; ++c)
      for (int i = 0; i < n; ++i) buf[c][pos + i] = static_cast<float>(v[c][i]);
  }
}

}  // namespace sorus
