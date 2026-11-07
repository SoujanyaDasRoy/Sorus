#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

#include "parametric_eq.h"
#include "test_util.h"

using namespace sorus;

// Runs a 1 s sine on every channel through eq; returns per-channel RMS gain (dB) over the last 0.5 s.
static std::vector<double> gains(ParametricEq& eq, int nch, double f) {
  const int n = 48000, half = n / 2;
  const auto x = sine(f, 48000, n);
  std::vector<std::vector<float>> y(static_cast<size_t>(nch), x);
  float* ch[kMaxChannels];
  for (int c = 0; c < nch; ++c) ch[c] = y[static_cast<size_t>(c)].data();
  eq.process(ch, nch, n);
  std::vector<double> g;
  for (int c = 0; c < nch; ++c)
    g.push_back(20 * std::log10(rms(y[static_cast<size_t>(c)].data() + half, n - half) / rms(x.data() + half, n - half)));
  return g;
}

TEST_CASE("left-only band leaves right untouched") {
  ParametricEq eq; eq.prepare(48000, 48000, 2);
  BandParams p; p.gain_db = 12; p.mode = ChannelMode::Left; eq.set_band(0, p);
  auto g = gains(eq, 2, 1000);
  CHECK(std::abs(g[0] - 12) < 0.1); CHECK(std::abs(g[1]) < 0.01);
}
TEST_CASE("side band does not change a mono signal") {
  ParametricEq eq; eq.prepare(48000, 48000, 2);
  BandParams p; p.gain_db = 12; p.mode = ChannelMode::Side; eq.set_band(0, p);
  auto g = gains(eq, 2, 1000);
  CHECK(std::abs(g[0]) < 0.01); CHECK(std::abs(g[1]) < 0.01);
}
TEST_CASE("six channels routing") {
  ParametricEq eq; eq.prepare(48000, 48000, 6);
  BandParams a; a.gain_db = 6; eq.set_band(0, a);
  BandParams b = a; b.mode = ChannelMode::Left; eq.set_band(1, b);
  auto g = gains(eq, 6, 1000);
  CHECK(std::abs(g[0] - 12) < 0.1);
  for (int c = 1; c < 6; ++c) CHECK(std::abs(g[c] - 6) < 0.1);
}
TEST_CASE("gain change mid-stream does not click") {
  ParametricEq eq; eq.prepare(48000, 48000, 1);
  auto x = sine(1000, 48000, 48000, 0.5f);
  float* ch[1] = {x.data()};
  eq.process(ch, 1, 24012);  // 24012 = sine peak, where a click would be largest
  BandParams p; p.gain_db = 24; eq.set_band(0, p);
  float* ch2[1] = {x.data() + 24012};
  eq.process(ch2, 1, 23988);
  double steady = 2 * std::numbers::pi * 1000 / 48000 * 0.5 * std::pow(10, 24 / 20.0);  // max per-sample delta at +24 dB
  double worst = 0;
  for (int i = 1; i < 48000; ++i) worst = std::max(worst, (double)std::abs(x[i] - x[i - 1]));
  CHECK(worst <= 1.2 * steady);
}
TEST_CASE("peak boost of stacked bells") {
  ParametricEq eq; eq.prepare(48000, 512, 2);
  BandParams p; p.gain_db = 6; p.q = 1; eq.set_band(0, p); eq.set_band(1, p);
  CHECK(eq.peak_boost_db() == doctest::Approx(12).epsilon(0.01));
  p.gain_db = -6; eq.set_band(0, p); eq.set_band(1, p);
  CHECK(eq.peak_boost_db() == 0);
}
TEST_CASE("peak boost finds a narrow bell between grid points") {
  ParametricEq eq; eq.prepare(48000, 512, 2);
  BandParams p; p.gain_db = 12; p.q = 40; p.freq = 1000; eq.set_band(0, p);
  CHECK(eq.peak_boost_db() == doctest::Approx(12).epsilon(0.001));
}
TEST_CASE("mode change does not click") {
  ParametricEq eq; eq.prepare(48000, 48000, 2);
  BandParams p; p.gain_db = 12; p.q = 1; p.mode = ChannelMode::Left; eq.set_band(0, p);
  const int n1 = 24012, n2 = 48000;
  auto l = sine(1000, 48000, n1 + n2, 0.5f);
  auto r = l;
  float* ch[2] = {l.data(), r.data()};
  eq.process(ch, 2, n1);
  p.mode = ChannelMode::Right; eq.set_band(0, p);
  float* ch2[2] = {l.data() + n1, r.data() + n1};
  eq.process(ch2, 2, n2);
  const double steady = 2 * std::numbers::pi * 1000 / 48000 * 0.5 * std::pow(10, 12 / 20.0);
  double worst_l = 0, worst_r = 0;
  for (int i = 1; i < n1 + n2; ++i) {
    worst_l = std::max(worst_l, (double)std::abs(l[i] - l[i - 1]));
    worst_r = std::max(worst_r, (double)std::abs(r[i] - r[i - 1]));
  }
  CHECK(worst_l <= 1.2 * steady);
  CHECK(worst_r <= 1.2 * steady);
  const auto g = gains(eq, 2, 1000);  // band has long since glided in on the right
  CHECK(std::abs(g[0]) < 0.1); CHECK(std::abs(g[1] - 12) < 0.1);
}
