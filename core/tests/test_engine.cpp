#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
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
// A huge finite sample times a positive preamp overflows float; the EQ state must not stay poisoned.
TEST_CASE("huge finite input does not poison the EQ") {
  Engine e(48000, 512, 2); e.set_auto_preamp(false); e.set_preamp_db(6);
  BandParams p; p.gain_db = 3; p.q = 1; e.set_band(0, p);
  std::vector<float> buf(1024, 0.1f);
  e.process_interleaved(buf.data(), 512);  // preamp ramp lands on +6 dB
  std::fill(buf.begin(), buf.end(), 0.1f); buf[10] = 3e38f; buf[11] = -3e38f;
  e.process_interleaved(buf.data(), 512);
  for (int i = 0; i < 100; ++i) {
    for (float v : buf) REQUIRE(std::isfinite(v));
    std::fill(buf.begin(), buf.end(), 0.1f);
    e.process_interleaved(buf.data(), 512);
  }
  CHECK(std::abs(engine_gain_db(e, 0, 1000) - 9) < 0.1);
}
// Stacked +24 dB bells with +24 dB preamp overflow float inside the EQ: output must still be finite.
TEST_CASE("engine output is always finite") {
  Engine e(48000, 512, 2); e.set_auto_preamp(false); e.set_preamp_db(24);
  BandParams p; p.gain_db = 24; p.q = 1;
  for (int i = 0; i < kMaxBands; ++i) e.set_band(i, p);
  const auto x = sine(1000, 48000, 4800);
  std::vector<float> buf(2 * 4800);
  for (int i = 0; i < 4800; ++i) buf[2 * i] = buf[2 * i + 1] = x[i];
  buf[100] = 3e38f;
  e.process_interleaved(buf.data(), 4800);
  for (float v : buf) REQUIRE(std::isfinite(v));
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

TEST_CASE("edits made while the queue is full reach audio after flush") {
  Engine e(48000, 512, 2); e.set_auto_preamp(true);
  BandParams p; p.q = 1;
  for (int i = 0; i < 300; ++i) { p.gain_db = i == 299 ? 6 : -6 + i % 12; e.set_band(0, p); }
  std::vector<float> buf(1024);
  e.process_interleaved(buf.data(), 512);  // drains what fit
  bool done = false;
  for (int i = 0; i < 10 && !done; ++i) { done = e.flush(); e.process_interleaved(buf.data(), 512); }
  REQUIRE(done);
  CHECK(std::abs(engine_gain_db(e, 0, 1000)) < 0.1);
  // Auto preamp cancels any boosted band at its centre, so a stale band would pass the check above too;
  // off-centre, only the final +6 dB band with its compensation gives this value.
  CHECK(std::abs(engine_gain_db(e, 0, 100) - (e.eq_response_db(100) - e.eq_response_db(1000))) < 0.1);
}

// A band change and its auto-preamp compensation must reach the audio thread together. The split is made
// deterministic by filling the queue so exactly one more command fits: a separate Preamp command after the
// Band command would be left behind and the block would play the boost uncompensated.
TEST_CASE("band and compensation land in the same block") {
  Engine e(48000, 512, 2);  // auto preamp on by default
  constexpr int block = 512;
  const auto x = sine(1000, 48000, 48000);  // amplitude 0.25
  std::vector<float> buf(2 * block);
  int pos = 0;
  auto run_block = [&] {
    for (int i = 0; i < block; ++i) buf[2 * i] = buf[2 * i + 1] = x[pos + i];
    pos += block;
    e.process_interleaved(buf.data(), block);
    float peak = 0;
    for (float v : buf) peak = std::max(peak, std::abs(v));
    return peak;
  };
  for (int i = 0; i < 20; ++i) run_block();
  for (int i = 0; i < 255; ++i) e.set_preamp_db(0);
  BandParams p; p.gain_db = 12; p.q = 1; e.set_band(0, p);
  CHECK(20 * std::log10(run_block() / 0.25) <= 1.0);
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

// Reloading the preset that is already active must not touch the audio side: same output, bit for bit.
TEST_CASE("reloading an identical preset leaves the audio untouched") {
  Preset pr; pr.auto_preamp = true;
  for (int i = 0; i < kMaxBands; ++i) {
    BandParams b; b.freq = 30 * std::pow(16000.0 / 30, i / 31.0); b.gain_db = (i % 2) ? -6 : 6; b.q = 1 + i % 4;
    pr.bands.push_back(b);
  }
  Engine a(48000, 512, 2), b(48000, 512, 2);
  a.load(pr); b.load(pr);
  constexpr int n = 48000;
  std::vector<float> x(2 * n);
  uint32_t s = 1;
  for (auto& v : x) { s = s * 1664525u + 1013904223u; v = 0.25f * ((s >> 8) / 16777216.0f - 0.5f); }
  auto ya = x, yb = x;
  for (int pos = 0; pos < n; pos += 512) {
    if (pos == 24064) a.load(pr);  // bands settled long ago
    a.process_interleaved(ya.data() + 2 * pos, std::min(512, n - pos));
    b.process_interleaved(yb.data() + 2 * pos, std::min(512, n - pos));
  }
  int diff = 0;
  for (int i = 0; i < 2 * n; ++i) diff += ya[i] != yb[i];
  CHECK(diff == 0);
}

// Settled-bypassed blocks do no work: even an EQ whose output would overflow float leaves the signal alone,
// and the output is the input delayed by latency_frames(), bit for bit.
TEST_CASE("bypassed blocks are skipped and the output is the delayed input") {
  Engine e(48000, 512, 2); e.set_auto_preamp(false);
  e.set_bypass(Block::Eq, true); e.set_bypass(Block::Limiter, true);
  constexpr int n = 9600;
  const int lat = e.latency_frames();
  const auto x = sine(1000, 48000, n, 1.5f);  // over the ceiling too
  std::vector<float> warm(2 * 4800, 0.1f);
  e.process_interleaved(warm.data(), 4800);  // bypass glides settle
  BandParams p; p.gain_db = 24; p.q = 1;
  for (int i = 0; i < kMaxBands; ++i) e.set_band(i, p);
  std::vector<float> buf(2 * n);
  for (int i = 0; i < n; ++i) buf[2 * i] = buf[2 * i + 1] = x[i];
  e.process_interleaved(buf.data(), n);
  for (int i = lat; i < n; ++i)
    for (int c = 0; c < 2; ++c) REQUIRE(buf[2 * i + c] == x[i - lat]);
}

// Un-bypassing an EQ that sat idle restarts it from clean state and glides the bands in: no click, even when
// the state it froze with (loud) no longer matches the signal (quiet).
TEST_CASE("un-bypassing the EQ does not click") {
  Engine e(48000, 512, 2); e.set_auto_preamp(false);
  BandParams p; p.gain_db = 12; p.q = 4; e.set_band(0, p);
  constexpr int quiet_from = 24000, unbypass = 36012, n = 48000;  // un-bypass on a sine peak
  constexpr float loud = 0.2f, quiet = 0.002f;                    // +12 dB of loud: under the ceiling
  const auto x = sine(1000, 48000, n);
  std::vector<float> buf(2 * n);
  for (int i = 0; i < n; ++i) buf[2 * i] = buf[2 * i + 1] = x[i] / 0.25f * (i < quiet_from ? loud : quiet);
  e.process_interleaved(buf.data(), 12000);
  e.set_bypass(Block::Eq, true);
  e.process_interleaved(buf.data() + 2 * 12000, unbypass - 12000);
  e.set_bypass(Block::Eq, false);
  e.process_interleaved(buf.data() + 2 * unbypass, n - unbypass);
  const double steady = 2 * std::numbers::pi * 1000 / 48000 * quiet * std::pow(10, 12 / 20.0);
  double worst = 0;  // from well after the level drop (plus latency)
  for (int i = quiet_from + 1000; i < n; ++i) worst = std::max(worst, static_cast<double>(std::abs(buf[2 * i] - buf[2 * i - 2])));
  CHECK(worst <= 1.2 * steady);
}

// Un-bypassing the limiter after it sat idle: once its release has settled it matches a limiter that never stopped.
TEST_CASE("un-bypassed limiter limits again") {
  Engine e(48000, 512, 2), ref(48000, 512, 2);
  e.set_bypass(Block::Limiter, true);
  std::vector<float> a(2 * 4800, 0.1f), b = a;
  e.process_interleaved(a.data(), 4800);
  ref.process_interleaved(b.data(), 4800);
  e.set_bypass(Block::Limiter, false);
  constexpr int n = 48000;
  const auto x = sine(997, 48000, n, 2.0f);
  std::vector<float> y(2 * n);
  for (int i = 0; i < n; ++i) y[2 * i] = y[2 * i + 1] = x[i];
  auto z = y;
  e.process_interleaved(y.data(), n);
  ref.process_interleaved(z.data(), n);
  for (int i = n; i < 2 * n; ++i) REQUIRE(std::abs(y[i] - z[i]) < 1e-4);
}
