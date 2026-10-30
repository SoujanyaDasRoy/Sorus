#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "params.h"

namespace sorus {

// Sample-rate independent settings, stored as versioned JSON (.sorus).
struct Preset {
  std::string name;
  double preamp_db = 0;
  bool auto_preamp = true;
  bool limiter_enabled = true;
  double limiter_ceiling_db = -1.0;
  std::vector<BandParams> bands;  // at most kMaxBands
  bool operator==(const Preset&) const = default;
};

// Format v1. Unknown keys are ignored; omitted band fields take BandParams defaults; values are clamped
// to spec ranges (freq to [10, 22000]). On failure returns nullopt and sets *error (if non-null).
std::optional<Preset> parse_preset(std::string_view json, std::string* error);
std::string to_json(const Preset& p);

}  // namespace sorus
