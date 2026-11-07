// Performance gate for the v1 chain. Prints "<case> realtime_fraction=<x>" per case; exit 1 if a gate fails.
//   full:   48 kHz stereo, 32 bells, limiter on, 60 s of noise.        Gate: realtime_fraction < 0.01.
//   decay:  same chain, 1 s of loud noise then 59 s of silence.       Gate: decay time <= 1.5 x full time.
//   loud:   same chain, the full-case noise x4 (true-peak detector busy). Gate: realtime_fraction < 0.01.
//   reload: `full` plus Engine::load(identical preset) every 2048 frames; only the audio calls are timed.
//           Gate: realtime_fraction < 0.01. Also prints the mean control-side load time.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "engine.h"

namespace {

using Clock = std::chrono::steady_clock;
constexpr double kRate = 48000;
constexpr int kBlock = 512, kChannels = 2, kSeconds = 60, kReloadEvery = 2048;
constexpr int kFrames = static_cast<int>(kRate) * kSeconds;

sorus::BandParams band(int i) {
  sorus::BandParams b;
  b.freq = 30 * std::pow(16000.0 / 30, i / 31.0);
  b.gain_db = (i % 2) ? -6 : 6;
  b.q = 1 + 3 * (i % 4) / 3.0;
  return b;
}

void configure(sorus::Engine& e) {
  for (int i = 0; i < sorus::kMaxBands; ++i) e.set_band(i, band(i));
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

double seconds(Clock::time_point t0) { return std::chrono::duration<double>(Clock::now() - t0).count(); }

// Best of 3 runs, each on a fresh copy of `input`; returns seconds of audio work. With `reload`, the
// identical preset is loaded every kReloadEvery frames (untimed; mean load time in *load_s).
double time_chain(const std::vector<float>& input, bool reload = false, double* load_s = nullptr) {
  sorus::Preset preset;
  for (int i = 0; i < sorus::kMaxBands; ++i) preset.bands.push_back(band(i));
  double best = 1e9;
  for (int run = 0; run < 3; ++run) {
    sorus::Engine e(kRate, kBlock, kChannels);
    configure(e);
    std::vector<float> buf = input;
    double audio = 0, loads = 0;
    int n_loads = 0;
    const int every = reload ? kReloadEvery : kFrames;  // without reload: one timed span, as before
    for (int chunk = 0; chunk < kFrames; chunk += every) {
      const auto t0 = Clock::now();
      for (int f = chunk; f < std::min(kFrames, chunk + every); f += kBlock)
        e.process_interleaved(buf.data() + static_cast<size_t>(f) * kChannels, std::min(kBlock, kFrames - f));
      audio += seconds(t0);
      if (!reload) continue;
      const auto t1 = Clock::now();
      e.load(preset);
      loads += seconds(t1);
      ++n_loads;
    }
    if (audio < best) {
      best = audio;
      if (load_s) *load_s = loads / std::max(n_loads, 1);
    }
  }
  return best;
}

}  // namespace

int main() {
  const auto input = noise(kFrames);
  const double full = time_chain(input);
  const double decay = time_chain(noise(static_cast<int>(kRate)));
  const double loud = time_chain(noise(kFrames, 4));
  double load_s = 0;
  const double reload = time_chain(input, true, &load_s);
  std::printf("full realtime_fraction=%.5f\n", full / kSeconds);
  std::printf("decay realtime_fraction=%.5f\n", decay / kSeconds);
  std::printf("loud realtime_fraction=%.5f\n", loud / kSeconds);
  std::printf("reload realtime_fraction=%.5f (load %.3f ms)\n", reload / kSeconds, load_s * 1e3);
  bool ok = true;
  if (full / kSeconds >= 0.01) { std::printf("FAIL full: >= 0.01 of realtime\n"); ok = false; }
  if (decay > 1.5 * full) { std::printf("FAIL decay: %.2fx full time (denormals?)\n", decay / full); ok = false; }
  if (loud / kSeconds >= 0.01) { std::printf("FAIL loud: >= 0.01 of realtime\n"); ok = false; }
  if (reload / kSeconds >= 0.01) { std::printf("FAIL reload: >= 0.01 of realtime\n"); ok = false; }
  return ok ? 0 : 1;
}
