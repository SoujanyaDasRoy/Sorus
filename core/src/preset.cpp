#include "preset.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

#include <nlohmann/json.hpp>

namespace sorus {

namespace {

using json = nlohmann::json;

constexpr std::array<std::pair<FilterType, const char*>, 9> kTypes{{
    {FilterType::Bell, "bell"},         {FilterType::LowShelf, "low_shelf"}, {FilterType::HighShelf, "high_shelf"},
    {FilterType::LowPass, "low_pass"},  {FilterType::HighPass, "high_pass"}, {FilterType::Notch, "notch"},
    {FilterType::BandPass, "band_pass"}, {FilterType::Tilt, "tilt"},         {FilterType::AllPass, "all_pass"},
}};
constexpr std::array<std::pair<ChannelMode, const char*>, 5> kModes{{
    {ChannelMode::Stereo, "stereo"}, {ChannelMode::Left, "left"}, {ChannelMode::Right, "right"},
    {ChannelMode::Mid, "mid"},       {ChannelMode::Side, "side"},
}};

template <class E, size_t N>
const char* name_of(const std::array<std::pair<E, const char*>, N>& table, E v) {
  for (auto& [e, s] : table) if (e == v) return s;
  return table[0].second;
}

template <class E, size_t N>
std::optional<E> parse_name(const std::array<std::pair<E, const char*>, N>& table, const std::string& s) {
  for (auto& [e, n] : table) if (s == n) return e;
  return std::nullopt;
}

double finite_clamp(double v, double lo, double hi, double fallback) {
  return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
}

std::optional<Preset> fail(std::string* error, std::string msg) {
  if (error) *error = std::move(msg);
  return std::nullopt;
}

}  // namespace

std::optional<Preset> parse_preset(std::string_view text, std::string* error) {
  try {
    const json j = json::parse(text);
    if (!j.is_object() || !j.contains("sorus_preset")) return fail(error, "not a sorus preset");
    const int version = j.at("sorus_preset").get<int>();
    if (version != 1) return fail(error, "unsupported preset version " + std::to_string(version));

    const Preset d;
    Preset p;
    p.name = j.value("name", d.name);
    p.preamp_db = finite_clamp(j.value("preamp_db", d.preamp_db), -24, 24, d.preamp_db);
    p.auto_preamp = j.value("auto_preamp", d.auto_preamp);
    if (j.contains("limiter")) {
      const json& l = j.at("limiter");
      p.limiter_enabled = l.value("enabled", d.limiter_enabled);
      p.limiter_ceiling_db = finite_clamp(l.value("ceiling_db", d.limiter_ceiling_db), -24, 0, d.limiter_ceiling_db);
    }

    const json eq = j.value("eq", json::array());
    if (eq.size() > kMaxBands)
      return fail(error, "too many bands (" + std::to_string(eq.size()) + " > " + std::to_string(kMaxBands) + ")");
    for (const json& jb : eq) {
      const BandParams bd;
      BandParams b;
      const std::string type = jb.value("type", std::string(name_of(kTypes, bd.type)));
      const auto t = parse_name(kTypes, type);
      if (!t) return fail(error, "unknown band type '" + type + "'");
      const std::string mode = jb.value("mode", std::string(name_of(kModes, bd.mode)));
      const auto m = parse_name(kModes, mode);
      if (!m) return fail(error, "unknown band mode '" + mode + "'");
      b.type = *t;
      b.mode = *m;
      b.freq = jb.value("freq", bd.freq);
      b.gain_db = jb.value("gain_db", bd.gain_db);
      b.q = jb.value("q", bd.q);
      b.slope_db_oct = jb.value("slope", bd.slope_db_oct);
      b.enabled = jb.value("enabled", bd.enabled);
      p.bands.push_back(clamp(b, 22000 / 0.45));  // freq ceiling 22 kHz; the engine re-clamps per sample rate
    }
    return p;
  } catch (const json::exception& e) {
    return fail(error, std::string("invalid json: ") + e.what());
  }
}

std::string to_json(const Preset& p) {
  json eq = json::array();
  for (const BandParams& b : p.bands) {
    eq.push_back({{"type", name_of(kTypes, b.type)}, {"freq", b.freq}, {"gain_db", b.gain_db}, {"q", b.q},
                  {"slope", b.slope_db_oct}, {"mode", name_of(kModes, b.mode)}, {"enabled", b.enabled}});
  }
  const json j = {{"sorus_preset", 1},
                  {"name", p.name},
                  {"preamp_db", p.preamp_db},
                  {"auto_preamp", p.auto_preamp},
                  {"limiter", {{"enabled", p.limiter_enabled}, {"ceiling_db", p.limiter_ceiling_db}}},
                  {"eq", eq}};
  return j.dump(2);
}

}  // namespace sorus
