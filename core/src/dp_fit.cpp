#include "dp_fit.h"

#include <algorithm>
#include <cmath>

#include "band.h"

namespace sorus {

void fit_to_dp_bands(const std::vector<BandParams>& bands, double sample_rate, const float* cutoff_hz, int n,
                     float* gain_db_out) {
  double lo = 20;
  for (int i = 0; i < n; ++i) {
    const double hi = cutoff_hz[i];
    const double centre = std::sqrt(lo * hi);
    double db = 0;
    for (const BandParams& b : bands) db += response_db(b, centre, sample_rate);
    gain_db_out[i] = static_cast<float>(std::clamp(db, -24.0, 24.0));
    lo = hi;
  }
}

}  // namespace sorus
