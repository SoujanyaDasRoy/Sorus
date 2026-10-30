#include <doctest/doctest.h>

#include <string>

#include "engine.h"
#include "preset.h"

using namespace sorus;

TEST_CASE("preset round-trips") {
  Preset p; p.name = "Warm"; p.preamp_db = -2; p.auto_preamp = false;
  BandParams b; b.type = FilterType::HighPass; b.freq = 20; b.slope_db_oct = 24; p.bands.push_back(b);
  b = {}; b.freq = 120; b.gain_db = 3; b.q = 0.7; b.mode = ChannelMode::Side; p.bands.push_back(b);
  std::string err; auto q = parse_preset(to_json(p), &err);
  REQUIRE(q); CHECK(*q == p);
}

TEST_CASE("preset errors") {
  std::string err;
  CHECK_FALSE(parse_preset("{}", &err)); CHECK(err == "not a sorus preset");
  CHECK_FALSE(parse_preset(R"({"sorus_preset":2})", &err)); CHECK(err == "unsupported preset version 2");
  CHECK_FALSE(parse_preset(R"({"sorus_preset":1,"eq":[{"type":"wobble"}]})", &err)); CHECK(err == "unknown band type 'wobble'");
  CHECK_FALSE(parse_preset("{not json", &err)); CHECK(err.rfind("invalid json", 0) == 0);
  std::string many = R"({"sorus_preset":1,"eq":[)";
  for (int i = 0; i < 33; ++i) many += std::string(i ? "," : "") + R"({"type":"bell"})";
  CHECK_FALSE(parse_preset(many + "]}", &err)); CHECK(err == "too many bands (33 > 32)");
}

TEST_CASE("preset values are clamped and unknown keys ignored") {
  std::string err;
  auto p = parse_preset(R"({"sorus_preset":1,"future":true,"eq":[{"type":"bell","gain_db":99,"freq":1}]})", &err);
  REQUIRE(p); CHECK(p->bands[0].gain_db == 24); CHECK(p->bands[0].freq == 10);
}

TEST_CASE("engine load replaces every band") {
  Engine e(48000, 512, 2);
  BandParams a; a.freq = 100; a.gain_db = 6; e.set_band(0, a);
  BandParams b; b.freq = 5000; b.gain_db = 6; e.set_band(5, b);
  Preset p; BandParams c; c.freq = 1000; c.gain_db = -6; c.q = 1; p.bands.push_back(c);
  e.load(p);
  CHECK(e.eq_response_db(1000) == doctest::Approx(-6).epsilon(0.001));
  CHECK(std::abs(e.eq_response_db(5000)) < 0.5);  // band 5 disabled; only band 0's skirt remains
  CHECK(std::abs(e.eq_response_db(100)) < 0.5);
}
