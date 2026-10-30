#include <doctest/doctest.h>

#include <cstring>
#include <string>
#include <vector>

#include <sorus/sorus.h>

TEST_CASE("c api rejects bad create args") {
  CHECK(sorus_create(1000, 512, 2) == nullptr);
  CHECK(sorus_create(48000, 0, 2) == nullptr);
  CHECK(sorus_create(48000, 512, 9) == nullptr);
}

TEST_CASE("c api null safety") {
  sorus_destroy(nullptr); sorus_process(nullptr, nullptr, 10);
  CHECK(sorus_load_preset(nullptr, "{}", 2) == -1);
  CHECK(sorus_latency_frames(nullptr) == -1);
  CHECK(sorus_flush(nullptr) == -1);
}

TEST_CASE("c api loads preset and reports response") {
  SorusEngine* e = sorus_create(48000, 512, 2);
  const char* bad = "{nope"; CHECK(sorus_load_preset(e, bad, strlen(bad)) == -1);
  char msg[128]; CHECK(sorus_last_error(e, msg, sizeof msg) == 0); CHECK(std::string(msg).rfind("invalid json", 0) == 0);
  const char* ok = R"({"sorus_preset":1,"eq":[{"type":"bell","freq":1000,"gain_db":6,"q":1}]})";
  CHECK(sorus_load_preset(e, ok, strlen(ok)) == 0);
  double f = 1000, db = 0; CHECK(sorus_eq_response_db(e, &f, &db, 1) == 0);
  CHECK(db == doctest::Approx(6).epsilon(0.001));
  std::vector<float> buf(1024, 0.1f); sorus_process(e, buf.data(), 512);
  CHECK(sorus_flush(e) == 1);  // nothing pending after a drain
  CHECK(sorus_latency_frames(e) > 0);
  sorus_destroy(e);
}
