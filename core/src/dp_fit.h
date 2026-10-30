#pragma once

#include <vector>

#include "params.h"

namespace sorus {

// Fits a parametric curve onto Android DynamicsProcessing EQ bands (session mode). DP bands are defined by
// their upper cutoff: band i spans [cutoff_hz[i-1], cutoff_hz[i]], band 0 starts at 20 Hz. Each output gain
// is the summed response of every enabled band (all channel modes) at the band's geometric centre,
// clamped to +-24 dB.
// ponytail: point-sampled at the centre; a band-averaged fit if spike A3 shows > 1 dB error at 64 bands.
void fit_to_dp_bands(const std::vector<BandParams>& bands, double sample_rate, const float* cutoff_hz, int n,
                     float* gain_db_out);

}  // namespace sorus
