#include <doctest/doctest.h>

#include <thread>

#include "spsc_queue.h"

TEST_CASE("spsc fifo and capacity") {
  sorus::SpscQueue<int, 4> q;
  for (int i = 0; i < 4; ++i) CHECK(q.push(i));
  CHECK_FALSE(q.push(99));
  int v;
  for (int i = 0; i < 4; ++i) { CHECK(q.pop(v)); CHECK(v == i); }
  CHECK_FALSE(q.pop(v));
}

TEST_CASE("spsc across threads keeps order") {
  sorus::SpscQueue<int, 1024> q;
  constexpr int n = 1'000'000;
  std::thread prod([&] { for (int i = 0; i < n;) if (q.push(i)) ++i; });
  int expect = 0, v;
  while (expect < n) if (q.pop(v)) { REQUIRE(v == expect); ++expect; }
  prod.join();
}
