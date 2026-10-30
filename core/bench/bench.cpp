// Performance gate for the v1 chain. Prints "<case> realtime_fraction=<x>" per case; exit 1 if a gate fails.
//   full:  48 kHz stereo, 32 bells, limiter on, 60 s of noise.        Gate: realtime_fraction < 0.01.
//   decay: same chain, 1 s of loud noise then 59 s of silence.       Gate: decay time <= 1.5 x full time.
//   loud:  same chain, the full-case noise x4 (true-peak detector busy). Gate: realtime_fraction < 0.01.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "engine.h"

namespace {

constexpr double kRate = 48000;
constexpr int kBlock = 512, kChannels = 2, kSeconds = 60;
constexpr int kFrames = static_cast<int>(kRate) * kSeconds;

void configure(sorus::Engine& e) {
  for (int i = 0; i < sorus::kMaxBands; ++i) {
    sorus::BandParams b;
    b.freq = 30 * std::pow(16000.0 / 30, i / 31.0);
    b.gain_db = (i % 2) ? -6 : 6;
    b.q = 1 + 3 * (i % 4) / 3.0;
    e.set_band(i, b);
  }
}

std::vector<float> noise(int loud_frames, float scale = 1) {
  std::vector<float> x(static_cast<size_t>(kFrames) * kChannels, 0.0f);
  uint32_t s = 1;
  for (size_t i = 0; i < static_cast<size_t>(loud_frames) * kChannels; ++i) {
    s = s * 1664525u + 1013904223u;
    x[i] = scale * static_cast<float>((s >> 8) / 16777216.0 - 0.5);
  }
  return x;
}

// Best of 3 runs, each on a fresh copy of `input`; returns seconds.
double time_chain(const std::vector<float>& input) {
  double best = 1e9;
  for (int run = 0; run < 3; ++run) {
    sorus::Engine e(kRate, kBlock, kChannels);
    configure(e);
    std::vector<float> buf = input;
    const auto t0 = std::chrono::steady_clock::now();
    for (int f = 0; f < kFrames; f += kBlock)
      e.process_interleaved(buf.data() + static_cast<size_t>(f) * kChannels, std::min(kBlock, kFrames - f));
    best = std::min(best, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
  }
  return best;
}

}  // namespace

int main() {
  const double full = time_chain(noise(kFrames));
  const double decay = time_chain(noise(static_cast<int>(kRate)));
  const double loud = time_chain(noise(kFrames, 4));
  std::printf("full realtime_fraction=%.5f\n", full / kSeconds);
  std::printf("decay realtime_fraction=%.5f\n", decay / kSeconds);
  std::printf("loud realtime_fraction=%.5f\n", loud / kSeconds);
  bool ok = true;
  if (full / kSeconds >= 0.01) { std::printf("FAIL full: >= 0.01 of realtime\n"); ok = false; }
  if (decay > 1.5 * full) { std::printf("FAIL decay: %.2fx full time (denormals?)\n", decay / full); ok = false; }
  if (loud / kSeconds >= 0.01) { std::printf("FAIL loud: >= 0.01 of realtime\n"); ok = false; }
  return ok ? 0 : 1;
}
