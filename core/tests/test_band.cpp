#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

#include "band.h"
#include "test_util.h"

using namespace sorus;
static double measured(BandParams p, double sr, double f) {
  Band b; b.prepare(sr); b.set(p);
  return measure_gain_db([&](float* x, int n) { b.process(x, n, 0); }, sr, f);
}
TEST_CASE("bell hits its gain at centre") {
  BandParams p; p.freq = 1000; p.gain_db = 6; p.q = 1;
  CHECK(response_db(p, 1000, 48000) == doctest::Approx(6).epsilon(0.001));
  CHECK(std::abs(measured(p, 48000, 1000) - 6) < 0.05);
}
TEST_CASE("measured response matches response_db for every type") {
  for (auto t : {FilterType::Bell, FilterType::LowShelf, FilterType::HighShelf, FilterType::LowPass, FilterType::HighPass,
                 FilterType::Notch, FilterType::BandPass, FilterType::Tilt, FilterType::AllPass}) {
    BandParams p; p.type = t; p.freq = 1000; p.gain_db = 9; p.q = 2; p.slope_db_oct = 30;
    for (double f : {30.0, 100.0, 300.0, 1000.0, 3000.0, 10000.0, 18000.0}) {
      double a = response_db(p, f, 48000);
      if (a < -40) continue;  // deep stopband/notch: RMS measurement is noise-limited
      CAPTURE(static_cast<int>(t)); CAPTURE(f);
      CHECK(std::abs(measured(p, 48000, f) - a) < 0.1);
    }
  }
}
TEST_CASE("high-pass slope is 48 dB per octave") {
  BandParams p; p.type = FilterType::HighPass; p.freq = 1000; p.slope_db_oct = 48;
  CHECK(response_db(p, 250, 48000) - response_db(p, 125, 48000) == doctest::Approx(48).epsilon(0.02));
}
TEST_CASE("tilt spreads gain around its pivot") {
  BandParams p; p.type = FilterType::Tilt; p.freq = 1000; p.gain_db = 6;
  CHECK(std::abs(response_db(p, 20, 48000) + 3) < 0.5);
  CHECK(std::abs(response_db(p, 20000, 48000) - 3) < 0.5);
}
TEST_CASE("near-Nyquist band stays stable") {
  for (double sr : {44100.0, 96000.0, 192000.0}) {
    BandParams p; p.freq = 30000; p.gain_db = 24; p.q = 40;
    Band b; b.prepare(sr); b.set(p);
    std::vector<float> x(static_cast<int>(sr)); uint32_t s = 1;
    for (auto& v : x) { s = s * 1664525u + 1013904223u; v = (s >> 8) / 16777216.0f - 0.5f; }
    b.process(x.data(), (int)x.size(), 0);
    for (float v : x) REQUIRE((std::isfinite(v) && std::abs(v) < 100));
  }
}

// Beyond the brief: odd-order Butterworth must still be -3 dB at freq; and the true-peak helper.
TEST_CASE("odd-order low-pass is -3 dB at its cutoff") {
  BandParams p; p.type = FilterType::LowPass; p.freq = 1000; p.slope_db_oct = 30;
  CHECK(response_db(p, 1000, 48000) == doctest::Approx(-3.0103).epsilon(0.001));
}
TEST_CASE("true_peak_db finds inter-sample peaks") {
  // fs/4 sine sampled at +-45 deg: samples at -3 dB, true peak 0 dB. Faded so edges don't ring.
  const int n = 4800, fade = 64;
  std::vector<float> x(n);
  for (int i = 0; i < n; ++i) {
    const double edge = std::min({1.0, (i + 0.5) / fade, (n - i - 0.5) / fade});
    x[i] = static_cast<float>(std::sin(std::numbers::pi * (i / 2.0 + 0.25)) * std::sin(std::numbers::pi / 2 * edge));
  }
  CHECK(std::abs(true_peak_db(x)) < 0.02);
}
TEST_CASE("first set() snaps, later set() glides") {
  auto gain_first_5ms = [](Band& b) {  // 1 kHz, 240 samples = 5 whole cycles
    auto x = sine(1000, 48000, 240), y = x;
    b.process(y.data(), 240, 0);
    return 20 * std::log10(rms(y.data(), 240) / rms(x.data(), 240));
  };
  BandParams p; p.gain_db = 12;
  Band snapped; snapped.prepare(48000); snapped.set(p);
  CHECK(gain_first_5ms(snapped) > 11);
  Band glide; glide.prepare(48000); p.gain_db = 0; glide.set(p); p.gain_db = 12; glide.set(p);
  const double g = gain_first_5ms(glide);
  CHECK(g > 1); CHECK(g < 6);
}

// Runs ~0.5 s of a 1 kHz sine at 0.5 through a +12 dB Q1 bell enabled as `before`, then the rest as `after`.
// The toggle lands on a sine peak (0.5 s + 90 deg): exactly 0.5 s is a zero crossing and would hide the click.
static std::vector<float> toggled(Band& b, bool before, bool after) {
  constexpr int kToggle = 24012;
  BandParams p; p.gain_db = 12; p.q = 1; p.enabled = before;
  b.prepare(48000); b.set(p);
  auto y = sine(1000, 48000, 48000, 0.5f);
  b.process(y.data(), kToggle, 0);
  p.enabled = after; b.set(p);
  b.process(y.data() + kToggle, 48000 - kToggle, 0);
  return y;
}
static double max_delta(const float* y, int from, int to) {
  double d = 0;
  for (int i = from + 1; i < to; ++i) d = std::max(d, std::abs(static_cast<double>(y[i]) - y[i - 1]));
  return d;
}
TEST_CASE("disabling a band does not click") {
  Band b;
  auto off = toggled(b, true, false);
  const double wet_off = max_delta(off.data(), 12000, 24000);  // steady state at +12 dB
  CHECK(max_delta(off.data(), 0, 48000) <= 1.2 * wet_off);
  auto on = toggled(b, false, true);
  const double wet_on = max_delta(on.data(), 36000, 48000);
  CHECK(max_delta(on.data(), 0, 48000) <= 1.2 * wet_on);
}
TEST_CASE("disabled band settles to unity") {
  Band b;
  toggled(b, true, false);
  CHECK(std::abs(measure_gain_db([&](float* x, int n) { b.process(x, n, 0); }, 48000, 1000)) < 0.01);
}
TEST_CASE("band state recovers from an overflowing input") {
  BandParams p; p.gain_db = 6; p.q = 1;
  Band b; b.prepare(48000); b.set(p);
  std::vector<float> hit(64, 0.f); hit[3] = INFINITY;
  b.process(hit.data(), 64, 0);
  CHECK(std::abs(measure_gain_db([&](float* x, int n) { b.process(x, n, 0); }, 48000, 1000) - 6) < 0.05);
}
// Edits to a disabled band that is still gliding out must not restart its 0.2 s glide.
TEST_CASE("editing a disabled gliding band does not extend its glide") {
  BandParams p; p.gain_db = 12;
  Band b; b.prepare(48000); b.set(p);
  std::vector<float> x(5000, 0.f);
  p.enabled = false; b.set(p);
  b.process(x.data(), 5000, 0);
  p.gain_db = 6; b.set(p);
  b.process(x.data(), 5000, 0);  // 10000 samples since disable > 0.2 s
  CHECK(b.settled(0));
}
TEST_CASE("response_db is finite and floored at -200 dB") {
  BandParams notch; notch.type = FilterType::Notch; notch.freq = 1000;
  BandParams hp; hp.type = FilterType::HighPass; hp.freq = 100;
  for (double f : {1000.0, 0.0, -5.0, double(NAN), double(INFINITY), 30000.0})
    for (const BandParams& p : {notch, hp}) {
      const double v = response_db(p, f, 48000);
      CAPTURE(f); CAPTURE(v);
      CHECK(std::isfinite(v)); CHECK(v >= -200);
    }
}
