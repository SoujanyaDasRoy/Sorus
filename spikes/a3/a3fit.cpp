// Spike A3 (throwaway), desktop half: how well do DynamicsProcessing's flat bands approximate the core's parametric
// curve? For 20 reference presets and 32/64/128 log-spaced DP bands (20 Hz-20 kHz), compares the target response
// with three fits, each treated as piecewise-flat bands, at 1/48-octave points:
//   centre  - the core's fit_to_dp_bands (target sampled at the band's geometric centre)
//   average - mean of the target (in dB) across the band
//   minimax - midpoint of the target's min and max across the band (minimises the worst error)
// Prints max/RMS error per preset and a summary. With --json <file> <bands>, also writes the 64-band configs and the
// target at 1/3-octave test frequencies for the on-device half (A3 app mode).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "band.h"
#include "dp_fit.h"

using sorus::BandParams;
using sorus::FilterType;

namespace {
constexpr double kRate = 48000;

BandParams b(FilterType t, double f, double g, double q = 0.7071, int slope = 12) {
  BandParams p;
  p.type = t;
  p.freq = f;
  p.gain_db = g;
  p.q = q;
  p.slope_db_oct = slope;
  return p;
}

struct Ref {
  const char* name;
  std::vector<BandParams> bands;
};

std::vector<Ref> presets() {
  using F = FilterType;
  std::vector<Ref> r = {
      {"small bell 1k +3", {b(F::Bell, 1000, 3, 1)}},
      {"bass shelf 100 +6", {b(F::LowShelf, 100, 6)}},
      {"treble shelf 8k +6", {b(F::HighShelf, 8000, 6)}},
      {"V-shape", {b(F::LowShelf, 100, 6), b(F::HighShelf, 8000, 5), b(F::Bell, 1000, -3, 0.7)}},
      {"Harman-like", {b(F::LowShelf, 105, 5), b(F::Bell, 3000, 2, 1), b(F::HighShelf, 10000, -2)}},
      {"loudness", {b(F::LowShelf, 60, 8), b(F::HighShelf, 12000, 4)}},
      {"vocal + HP80", {b(F::Bell, 2500, 4, 1.4), b(F::HighPass, 80, 0, 0.7071, 12)}},
      {"narrow cut 3k Q8", {b(F::Bell, 3000, -10, 8)}},
      {"narrow boost 60 Q4", {b(F::Bell, 60, 9, 4)}},
      {"HP 100 24dB", {b(F::HighPass, 100, 0, 0.7071, 24)}},
      {"LP 5k 12dB", {b(F::LowPass, 5000, 0, 0.7071, 12)}},
      {"headphone correction", {b(F::Bell, 40, 3, 1), b(F::Bell, 150, -2, 1.5), b(F::Bell, 900, 1.5, 2),
                                b(F::Bell, 2200, -3, 3), b(F::Bell, 3500, 2.5, 4), b(F::Bell, 6000, -4, 2.5),
                                b(F::Bell, 9000, 3, 2), b(F::HighShelf, 12000, -2)}},
      {"sub bass 40 +10", {b(F::Bell, 40, 10, 1.5)}},
      {"de-ess 7k -6 Q3", {b(F::Bell, 7000, -6, 3)}},
      {"tilt 1k +3", {b(F::Tilt, 1000, 3)}},
      {"mid scoop 500 -6", {b(F::Bell, 500, -6, 0.8)}},
      {"telephone", {b(F::HighPass, 300, 0, 0.7071, 24), b(F::LowPass, 3000, 0, 0.7071, 24)}},
      {"max shelves +12", {b(F::LowShelf, 80, 12), b(F::HighShelf, 10000, 12)}},
      {"hum notch 50 Q10", {b(F::Notch, 50, 0, 10)}},
  };
  Ref graphic{"31-band graphic +-3", {}};
  for (int i = 0; i < 31; ++i)  // ISO 1/3-octave centres 20 Hz..20 kHz; 31 > 32-band limit is fine (31 <= 32)
    graphic.bands.push_back(b(F::Bell, 20 * std::pow(2.0, i / 3.0), (i % 2) ? -3 : 3, 4.32));
  r.push_back(graphic);
  return r;
}

double target(const std::vector<BandParams>& bands, double f) {
  double db = 0;
  for (const auto& p : bands) db += sorus::response_db(p, f, kRate);
  return db;
}

std::vector<float> cutoffs(int n) {
  std::vector<float> c(n);
  for (int i = 0; i < n; ++i) c[i] = static_cast<float>(20 * std::pow(1000.0, (i + 1.0) / n));
  c[n - 1] = static_cast<float>(kRate / 2);  // DP passes everything above the last cutoff untouched: end at Nyquist
  return c;
}

// Fits by the given method; for average/minimax the target is sampled at 16 points per band.
std::vector<float> fit(const std::vector<BandParams>& bands, const std::vector<float>& cut, int method) {
  const int n = static_cast<int>(cut.size());
  std::vector<float> g(n);
  if (method == 0) {
    sorus::fit_to_dp_bands(bands, kRate, cut.data(), n, g.data());
    return g;
  }
  double lo = 20;
  for (int i = 0; i < n; ++i) {
    const double hi = cut[i];
    double sum = 0, mn = 1e9, mx = -1e9;
    for (int k = 0; k < 16; ++k) {
      const double t = target(bands, lo * std::pow(hi / lo, (k + 0.5) / 16));
      sum += t;
      mn = std::min(mn, t);
      mx = std::max(mx, t);
    }
    g[i] = static_cast<float>(std::clamp(method == 1 ? sum / 16 : (mn + mx) / 2, -24.0, 24.0));
    lo = hi;
  }
  return g;
}

struct Err {
  double max = 0, rms = 0;
};

// Target vs piecewise-flat fit at 1/48-octave points. Targets below -24 dB (beyond DP's range) are compared
// against -24 dB, so steep filters' stop bands count only for what DP can express.
Err error(const std::vector<BandParams>& bands, const std::vector<float>& cut, const std::vector<float>& g) {
  Err e;
  int n = 0;
  for (double f = 20; f <= 20000; f *= std::pow(2.0, 1.0 / 48), ++n) {
    int i = 0;
    while (i + 1 < static_cast<int>(cut.size()) && f > cut[i]) ++i;
    const double d = std::max(target(bands, f), -24.0) - g[i];
    e.max = std::max(e.max, std::fabs(d));
    e.rms += d * d;
  }
  e.rms = std::sqrt(e.rms / n);
  return e;
}
}  // namespace

int main(int argc, char** argv) {
  const auto refs = presets();
  const char* methods[] = {"centre", "average", "minimax"};
  const int counts[] = {32, 64, 128};
  std::printf("%-22s", "preset");
  for (int n : counts) std::printf(" | %3d: centre  avg  minmax", n);
  std::printf("   (max |error| dB)\n");
  double worst[3][3] = {}, worst_rms[3][3] = {};
  int over1[3][3] = {};
  for (const auto& r : refs) {
    std::printf("%-22s", r.name);
    for (int c = 0; c < 3; ++c) {
      const auto cut = cutoffs(counts[c]);
      std::printf(" |     ");
      for (int m = 0; m < 3; ++m) {
        const Err e = error(r.bands, cut, fit(r.bands, cut, m));
        worst[c][m] = std::max(worst[c][m], e.max);
        worst_rms[c][m] = std::max(worst_rms[c][m], e.rms);
        over1[c][m] += e.max > 1.0;
        std::printf(" %5.2f", e.max);
      }
    }
    std::printf("\n");
  }
  std::printf("\nsummary over %zu presets (worst max error / worst RMS error / presets over 1 dB):\n", refs.size());
  for (int c = 0; c < 3; ++c)
    for (int m = 0; m < 3; ++m)
      std::printf("  %3d bands, %-7s: max %5.2f dB, rms %4.2f dB, %2d over 1 dB\n", counts[c], methods[m],
                  worst[c][m], worst_rms[c][m], over1[c][m]);

  // On-device half: per preset, the DP cutoffs/gains and the target at the test frequencies.
  if (argc >= 4 && std::strcmp(argv[1], "--json") == 0) {
    const int n = std::atoi(argv[3]);
    const int method = argc >= 5 ? std::atoi(argv[4]) : 0;
    const auto cut = cutoffs(n);
    FILE* f = std::fopen(argv[2], "w");
    if (!f) return 1;
    std::fprintf(f, "{\"bands\":%d,\"method\":\"%s\",\"cutoffs\":[", n, methods[method]);
    for (int i = 0; i < n; ++i) std::fprintf(f, "%s%.2f", i ? "," : "", cut[i]);
    std::fprintf(f, "],\"freqs\":[");
    std::vector<double> freqs;
    for (int i = 0; i < 31; ++i) freqs.push_back(20 * std::pow(2.0, i / 3.0));
    for (size_t i = 0; i < freqs.size(); ++i) std::fprintf(f, "%s%.2f", i ? "," : "", freqs[i]);
    std::fprintf(f, "],\"presets\":[");
    for (size_t p = 0; p < refs.size(); ++p) {
      const auto g = fit(refs[p].bands, cut, method);
      std::fprintf(f, "%s{\"name\":\"%s\",\"gains\":[", p ? "," : "", refs[p].name);
      for (int i = 0; i < n; ++i) std::fprintf(f, "%s%.3f", i ? "," : "", g[i]);
      std::fprintf(f, "],\"target\":[");
      for (size_t i = 0; i < freqs.size(); ++i)
        std::fprintf(f, "%s%.3f", i ? "," : "", std::max(target(refs[p].bands, freqs[i]), -24.0));
      std::fprintf(f, "]}");
    }
    std::fprintf(f, "]}\n");
    std::fclose(f);
    std::printf("\nwrote %s (%d bands, %s fit)\n", argv[2], n, methods[method]);
  }
  return 0;
}
