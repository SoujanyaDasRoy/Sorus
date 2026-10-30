#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "denormals.h"
#include "engine.h"
#include "test_util.h"

using namespace sorus;

// helper: interleaved stereo sine through engine, returns gain dB of channel `c` (accounts for latency)
static double engine_gain_db(Engine& e, int c, double f) {
  constexpr int n = 48000, half = n / 2, block = 480;
  const int lat = e.latency_frames();
  const auto x = sine(f, 48000, n);
  std::vector<float> buf(2 * n);
  for (int i = 0; i < n; ++i) buf[2 * i] = buf[2 * i + 1] = x[i];
  for (int pos = 0; pos < n; pos += block) e.process_interleaved(buf.data() + 2 * pos, std::min(block, n - pos));
  double out = 0, in = 0;
  for (int i = half; i < n; ++i) {
    out += static_cast<double>(buf[2 * i + c]) * buf[2 * i + c];
    in += static_cast<double>(x[i - lat]) * x[i - lat];
  }
  return 10 * std::log10(out / in);
}

TEST_CASE("auto preamp cancels the EQ peak") {
  Engine e(48000, 512, 2);
  BandParams p; p.gain_db = 6; p.q = 1; e.set_band(0, p); e.set_auto_preamp(true);
  CHECK(std::abs(engine_gain_db(e, 0, 1000)) < 0.1);
}
TEST_CASE("manual preamp applies when auto is off") {
  Engine e(48000, 512, 2); e.set_auto_preamp(false); e.set_preamp_db(-3);
  CHECK(std::abs(engine_gain_db(e, 1, 10000) + 3) < 0.1);
}
TEST_CASE("bypassed EQ is transparent and latency stays constant") {
  Engine e(48000, 512, 2);
  int lat = e.latency_frames();
  BandParams p; p.gain_db = 12; e.set_band(0, p); e.set_bypass(Block::Eq, true);
  CHECK(std::abs(engine_gain_db(e, 0, 1000)) < 0.05);
  e.set_bypass(Block::Limiter, true);
  CHECK(e.latency_frames() == lat);
}
TEST_CASE("non-finite input is sanitized") {
  Engine e(48000, 512, 2);
  std::vector<float> buf(1024, 0.1f); buf[10] = NAN; buf[11] = INFINITY;
  e.process_interleaved(buf.data(), 512);
  for (float v : buf) REQUIRE(std::isfinite(v));  // not in the brief: the loop below re-checks a refilled buffer
  for (int i = 0; i < 20; ++i) { e.process_interleaved(buf.data(), 512); std::fill(buf.begin(), buf.end(), 0.1f); }
  for (float v : buf) REQUIRE(std::isfinite(v));
  CHECK(std::abs(engine_gain_db(e, 0, 1000)) < 0.1);
}
TEST_CASE("blocks larger than max_frames") {
  Engine big(48000, 4096, 2), small(48000, 256, 2);
  auto mono = sine(440, 48000, 4096);
  std::vector<float> a(8192), b(8192);
  for (int i = 0; i < 4096; ++i) a[2 * i] = a[2 * i + 1] = b[2 * i] = b[2 * i + 1] = mono[i];
  big.process_interleaved(a.data(), 4096); small.process_interleaved(b.data(), 4096);
  for (int i = 0; i < 8192; ++i) REQUIRE(a[i] == doctest::Approx(b[i]).epsilon(1e-6));
}
TEST_CASE("denormals flushed during process") {
  ScopedFlushDenormals g;
  volatile float tiny = 1e-40f;
  CHECK(tiny * 1.0f == 0.0f);
}

// Not in the brief: the brief's latency check passes without a delay line. Below the ceiling the limiter
// is exactly a delay, so every bypass crossfade must leave the output equal to the input delayed by latency.
TEST_CASE("bypass crossfades keep the signal aligned") {
  Engine e(48000, 512, 2);
  constexpr int n = 4 * 4800;
  const int lat = e.latency_frames();
  const auto x = sine(440, 48000, n);
  std::vector<float> buf(2 * n);
  for (int i = 0; i < n; ++i) buf[2 * i] = buf[2 * i + 1] = x[i];
  e.process_interleaved(buf.data(), 4800);
  e.set_bypass(Block::Limiter, true);
  e.process_interleaved(buf.data() + 2 * 4800, 4800);
  e.set_bypass(Block::Eq, true);
  e.process_interleaved(buf.data() + 4 * 4800, 4800);
  e.set_bypass(Block::Limiter, false);
  e.process_interleaved(buf.data() + 6 * 4800, 4800);
  for (int i = 0; i < n; ++i)
    for (int c = 0; c < 2; ++c) REQUIRE(buf[2 * i + c] == doctest::Approx(i < lat ? 0.f : x[i - lat]).epsilon(1e-5));
}

// Not in the brief: a full queue must not lose the latest value.
TEST_CASE("setters on a full queue keep the latest value") {
  Engine e(48000, 512, 2); e.set_auto_preamp(false);
  for (int i = 0; i < 1000; ++i) e.set_preamp_db(i == 999 ? -6 : 0);
  BandParams p;
  for (int i = 0; i < 1000; ++i) { p.gain_db = i == 999 ? 9 : 0; e.set_band(0, p); }
  std::vector<float> buf(1024);
  e.process_interleaved(buf.data(), 512);  // drains the stale commands
  e.set_limiter_ceiling_db(-1);            // any setter retries the pending latest values
  CHECK(std::abs(engine_gain_db(e, 0, 1000) - 3) < 0.1);
}

// Not in the brief: out-of-range arguments are clamped or ignored.
TEST_CASE("engine tolerates bad arguments") {
  Engine e(48000, 0, 99);
  BandParams p; p.gain_db = 12;
  e.set_band(-1, p); e.set_band(kMaxBands, p);
  e.process_interleaved(nullptr, 64);
  std::vector<float> buf(kMaxChannels * 100, 0.1f);
  e.process_interleaved(buf.data(), 0);
  e.process_interleaved(buf.data(), -5);
  CHECK(buf[0] == 0.1f);
  e.process_interleaved(buf.data(), 100);  // max_frames 0 -> 1: frame by frame
  for (float v : buf) REQUIRE(std::isfinite(v));
  CHECK(e.eq_response_db(1000) == 0);
}
