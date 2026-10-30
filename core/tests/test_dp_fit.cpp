#include <doctest/doctest.h>

#include <cstring>

#include <sorus/sorus.h>

#include "dp_fit.h"

using namespace sorus;

TEST_CASE("flat curve fits to zero gains") {
  float cut[4] = {100, 1000, 5000, 20000}, g[4];
  fit_to_dp_bands({}, 48000, cut, 4, g);
  for (float v : g) CHECK(v == 0);
}

TEST_CASE("bell lands in the band containing its centre") {
  float cut[3] = {500, 2000, 20000}, g[3];  // band 1 spans 500..2000, centre = 1000
  BandParams p; p.freq = 1000; p.gain_db = 6; p.q = 1;
  fit_to_dp_bands({p}, 48000, cut, 3, g);
  CHECK(g[1] == doctest::Approx(6).epsilon(0.02));
  CHECK(g[0] < g[1]); CHECK(g[2] < g[1]);
}

TEST_CASE("fitted gains are clamped to +-24 dB") {
  float cut[2] = {500, 2000}, g[2];  // band 1 centre = 1000, where two +24 dB bells sum to +48
  BandParams p; p.freq = 1000; p.gain_db = 24; p.q = 0.5;
  fit_to_dp_bands({p, p}, 48000, cut, 2, g);
  CHECK(g[1] == 24);
}

TEST_CASE("c api fits a preset to dp bands") {
  const char* json = R"({"sorus_preset":1,"eq":[{"type":"bell","freq":1000,"gain_db":6,"q":1}]})";
  float cut[3] = {500, 2000, 20000}, g[3];
  CHECK(sorus_fit_dp_bands(json, strlen(json), 48000, cut, 3, g) == 0);
  CHECK(g[1] == doctest::Approx(6).epsilon(0.02));
  CHECK(sorus_fit_dp_bands("{nope", 5, 48000, cut, 3, g) == -1);
  CHECK(sorus_fit_dp_bands(json, strlen(json), 48000, nullptr, 3, g) == -1);
}
