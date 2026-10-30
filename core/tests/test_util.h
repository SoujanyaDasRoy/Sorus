#pragma once

#include <algorithm>
#include <cmath>
#include <functional>
#include <numbers>
#include <vector>

inline std::vector<float> sine(double freq, double sr, int frames, float amp = 0.25f) {
  std::vector<float> x(static_cast<size_t>(frames));
  for (int i = 0; i < frames; ++i)
    x[i] = amp * static_cast<float>(std::sin(2 * std::numbers::pi * freq * i / sr));
  return x;
}

inline double rms(const float* x, int n) {
  double s = 0;
  for (int i = 0; i < n; ++i) s += static_cast<double>(x[i]) * x[i];
  return std::sqrt(s / n);
}

// Runs a 1 s sine through `process`; gain is the RMS ratio over the last 0.5 s.
inline double measure_gain_db(const std::function<void(float*, int)>& process, double sr, double freq) {
  const int n = static_cast<int>(sr), half = n / 2;
  const auto x = sine(freq, sr, n);
  auto y = x;
  process(y.data(), n);
  return 20 * std::log10(rms(y.data() + half, n - half) / rms(x.data() + half, n - half));
}

// Max |x| in dBFS after 16x windowed-sinc (Hann, 16 taps each side) interpolation.
inline double true_peak_db(const std::vector<float>& x) {
  constexpr int kOver = 16, kHalf = 16;
  double h[kOver][2 * kHalf];
  for (int k = 0; k < kOver; ++k)
    for (int m = -kHalf + 1; m <= kHalf; ++m) {
      const double d = static_cast<double>(k) / kOver - m;  // from tap i+m to t = i + k/16
      const double sinc = d == 0 ? 1 : std::sin(std::numbers::pi * d) / (std::numbers::pi * d);
      h[k][m + kHalf - 1] = sinc * 0.5 * (1 + std::cos(std::numbers::pi * d / kHalf));
    }
  const int n = static_cast<int>(x.size());
  double peak = 0;
  for (int i = 0; i < n; ++i)
    for (int k = 0; k < kOver; ++k) {
      double y = 0;
      for (int m = std::max(-kHalf + 1, -i); m <= std::min(kHalf, n - 1 - i); ++m) y += x[i + m] * h[k][m + kHalf - 1];
      peak = std::max(peak, std::abs(y));
    }
  return 20 * std::log10(peak);
}
