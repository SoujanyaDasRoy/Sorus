#include "params.h"

#include <algorithm>
#include <cmath>

namespace sorus {

namespace {
double finite_or(double v, double fallback) { return std::isfinite(v) ? v : fallback; }
}  // namespace

BandParams clamp(BandParams p, double sample_rate) {
  const BandParams d;
  p.freq = std::clamp(finite_or(p.freq, d.freq), 10.0, std::min(22000.0, 0.45 * sample_rate));
  p.gain_db = std::clamp(finite_or(p.gain_db, d.gain_db), -24.0, 24.0);
  p.q = std::clamp(finite_or(p.q, d.q), 0.1, 40.0);
  p.slope_db_oct = std::clamp(static_cast<int>(std::lround(p.slope_db_oct / 6.0)) * 6, 6, 96);
  return p;
}

}  // namespace sorus
