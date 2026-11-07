#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <tuple>
#include <vector>

#include "parametric_eq.h"
#include "test_util.h"

using namespace sorus;

// Runs a 1 s sine on every channel through eq (channel 1 inverted if `anti`: L = -R); returns per-channel RMS
// gain (dB) over the last 0.5 s.
static std::vector<double> gains(ParametricEq& eq, int nch, double f, bool anti = false) {
  const int n = 48000, half = n / 2;
  const auto x = sine(f, 48000, n);
  std::vector<std::vector<float>> y(static_cast<size_t>(nch), x);
  if (anti && nch > 1) for (float& v : y[1]) v = -v;
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

// M/S routing, positively: each mode touches exactly what it should.
TEST_CASE("side band boosts anti-phase, mid band boosts mono") {
  BandParams p; p.gain_db = 12; p.q = 1;
  for (auto [mode, mono_db, anti_db] : {std::tuple{ChannelMode::Side, 0.0, 12.0}, std::tuple{ChannelMode::Mid, 12.0, 0.0}}) {
    CAPTURE(static_cast<int>(mode));
    ParametricEq eq; eq.prepare(48000, 48000, 2);
    p.mode = mode; eq.set_band(0, p);
    for (double g : gains(eq, 2, 1000)) CHECK(std::abs(g - mono_db) < 0.1);
    for (double g : gains(eq, 2, 1000, true)) CHECK(std::abs(g - anti_db) < 0.1);
  }
}
TEST_CASE("right band touches only channel 1") {
  ParametricEq eq; eq.prepare(48000, 48000, 2);
  BandParams p; p.gain_db = 12; p.mode = ChannelMode::Right; eq.set_band(0, p);
  auto g = gains(eq, 2, 1000);
  CHECK(std::abs(g[0]) < 0.01); CHECK(std::abs(g[1] - 12) < 0.1);
}
TEST_CASE("M/S bands leave channels 2+ untouched") {
  ParametricEq eq; eq.prepare(48000, 48000, 4);
  BandParams p; p.gain_db = 6; p.mode = ChannelMode::Mid; eq.set_band(0, p);
  p.gain_db = 12; p.mode = ChannelMode::Side; eq.set_band(1, p);
  auto g = gains(eq, 4, 1000, true);  // ch 0/1 anti-phase: pure Side
  CHECK(std::abs(g[0] - 12) < 0.1); CHECK(std::abs(g[1] - 12) < 0.1);
  CHECK(std::abs(g[2]) < 0.01); CHECK(std::abs(g[3]) < 0.01);
}
TEST_CASE("M/S processing is the same in chunks as in one block") {
  ParametricEq big, small;
  big.prepare(48000, 48000, 2); small.prepare(48000, 256, 2);
  BandParams p; p.gain_db = 9; p.q = 2; p.mode = ChannelMode::Side; big.set_band(0, p); small.set_band(0, p);
  p.freq = 300; p.mode = ChannelMode::Mid; big.set_band(1, p); small.set_band(1, p);
  auto l = sine(440, 48000, 4096), r = sine(1000, 48000, 4096);
  auto l2 = l, r2 = r;
  float* a[2] = {l.data(), r.data()};
  float* b[2] = {l2.data(), r2.data()};
  big.process(a, 2, 4096); small.process(b, 2, 4096);
  for (int i = 0; i < 4096; ++i) { REQUIRE(l[i] == l2[i]); REQUIRE(r[i] == r2[i]); }
}
// Mono/anti-phase inputs cannot see an L/R swap in the decode; a left-only input can:
// L = M + G*S = (1 + G)/2 * x, R = M - G*S = (1 - G)/2 * x for a Side band of gain G at its centre.
TEST_CASE("M/S decode keeps left on the left") {
  ParametricEq eq; eq.prepare(48000, 48000, 2);
  BandParams p; p.gain_db = 12; p.q = 1; p.mode = ChannelMode::Side; eq.set_band(0, p);
  auto l = sine(1000, 48000, 48000);
  std::vector<float> r(48000, 0.f);
  const auto x = l;
  float* ch[2] = {l.data(), r.data()};
  eq.process(ch, 2, 48000);
  const double g = std::pow(10, 12 / 20.0), in = rms(x.data() + 24000, 24000);
  CHECK(rms(l.data() + 24000, 24000) / in == doctest::Approx((1 + g) / 2).epsilon(0.01));
  CHECK(rms(r.data() + 24000, 24000) / in == doctest::Approx((g - 1) / 2).epsilon(0.01));
}
