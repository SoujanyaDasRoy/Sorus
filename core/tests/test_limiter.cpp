#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

#include "limiter.h"
#include "test_util.h"

using namespace sorus;
TEST_CASE("limiter keeps true peak under ceiling") {
  TruePeakLimiter l; l.prepare(48000, 48000, 1);
  std::vector<float> x(48000);
  for (int i = 0; i < 48000; ++i) x[i] = static_cast<float>(2.0f * std::sin(std::numbers::pi / 2 * i + std::numbers::pi / 4));  // fs/4, inter-sample peaks, +6 dBFS
  float* ch[1] = {x.data()}; l.process(ch, 1, 48000);
  std::vector<float> tail(x.begin() + 4800, x.end());
  CHECK(true_peak_db(tail) <= -1.0 + 0.2);
}
TEST_CASE("limiter is transparent below ceiling") {
  TruePeakLimiter l; l.prepare(48000, 48000, 1);
  auto x = sine(1000, 48000, 48000, 0.25f); auto in = x;
  float* ch[1] = {x.data()}; l.process(ch, 1, 48000);
  int d = l.latency_frames();
  for (int i = d; i < 48000; ++i) REQUIRE(std::abs(x[i] - in[i - d]) < 1e-4);
}
TEST_CASE("limiter latency under 5 ms") {
  for (double sr : {44100.0, 48000.0, 96000.0, 192000.0}) {
    TruePeakLimiter l; l.prepare(sr, 512, 2);
    CHECK(l.latency_frames() < 0.005 * sr);
  }
}

// Not in the brief: ceiling change (after a glide) + linked gain across channels, in small blocks.
TEST_CASE("limiter applies a lowered ceiling with one linked gain") {
  TruePeakLimiter l; l.prepare(48000, 256, 2);
  auto quiet = sine(440, 48000, 48000, 0.25f), loud = sine(1000, 48000, 48000, 1.5f);
  const auto quiet_in = quiet, loud_in = loud;
  l.set_ceiling_db(-6);
  for (int pos = 0; pos < 48000; pos += 256) {
    float* ch[2] = {quiet.data() + pos, loud.data() + pos};
    l.process(ch, 2, std::min(256, 48000 - pos));
  }
  const int d = l.latency_frames();
  // Cut at output zero crossings (input index a multiple of 24 = half a 1 kHz period): a cut mid-cycle
  // makes true_peak_db ring at the edges.
  const int a = 9600 + d % 24, b = a + 24 * 1500 + 1;
  CHECK(true_peak_db({loud.begin() + a, loud.begin() + b}) <= -6.0 + 0.2);
  // Same gain on both channels.
  for (int i = 24000; i < 48000; i += 101)
    CHECK(quiet[i] * loud_in[i - d] == doctest::Approx(loud[i] * quiet_in[i - d]).epsilon(1e-3));
}

TEST_CASE("limiter holds ceiling on broadband noise") {
  const int n = 96000, skip = 4800;  // 2 s at 48 kHz, skip 0.1 s
  TruePeakLimiter l; l.prepare(48000, n, 2);
  std::vector<float> y[2] = {std::vector<float>(n), std::vector<float>(n)};
  uint32_t s = 1;
  auto uni = [&] { s = s * 1664525u + 1013904223u; return ((s >> 8) + 0.5) / 16777216.0; };  // (0, 1)
  for (auto& c : y) {  // Gaussian (Box-Muller), scaled to a +12 dBFS sample peak
    float m = 0;
    for (auto& v : c) { v = static_cast<float>(std::sqrt(-2 * std::log(uni())) * std::cos(2 * std::numbers::pi * uni())); m = std::max(m, std::abs(v)); }
    for (auto& v : c) v *= 4 / m;
  }
  float* ch[2] = {y[0].data(), y[1].data()};
  l.process(ch, 2, n);
  for (const auto& c : y) CHECK(true_peak_db({c.begin() + skip, c.end()}) <= -1.0 + 0.2);
}
