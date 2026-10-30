#include <doctest/doctest.h>

#include <cmath>

#include "params.h"

TEST_CASE("clamp limits band params to spec ranges") {
  using namespace sorus;
  BandParams p; p.freq = 5; p.gain_db = 40; p.q = 0.01; p.slope_db_oct = 100;
  auto c = clamp(p, 48000);
  CHECK(c.freq == 10); CHECK(c.gain_db == 24); CHECK(c.q == doctest::Approx(0.1)); CHECK(c.slope_db_oct == 96);
  p.freq = 30000; p.gain_db = -40; p.q = 99; p.slope_db_oct = 14;
  c = clamp(p, 48000);
  CHECK(c.freq == doctest::Approx(21600)); CHECK(c.gain_db == -24); CHECK(c.q == 40); CHECK(c.slope_db_oct == 12);
  CHECK(clamp(p, 44100).freq == doctest::Approx(19845));
  p.gain_db = std::nan(""); CHECK(clamp(p, 48000).gain_db == 0);
}
