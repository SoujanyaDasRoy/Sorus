#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstddef>
#include <cstdlib>
#include <new>
#include <vector>

#include "engine.h"

// Counting replacements for the global allocator (own binary: the counter would be meaningless
// alongside the rest of the suite).
static size_t g_allocs = 0;
void* operator new(std::size_t n) {
  ++g_allocs;
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

TEST_CASE("process never allocates") {
  sorus::Engine e(48000, 512, 2);
  for (int i = 0; i < 32; ++i) { sorus::BandParams p; p.freq = 30 + i * 600; p.gain_db = 3; e.set_band(i, p); }
  std::vector<float> buf(1024, 0.1f);
  e.process_interleaved(buf.data(), 512);   // warm-up drains the queued commands
  for (int i = 0; i < 32; ++i) { sorus::BandParams p; p.gain_db = -3; e.set_band(i, p); }
  size_t before = g_allocs;
  for (int i = 0; i < 1000; ++i) e.process_interleaved(buf.data(), 512);
  CHECK(g_allocs == before);
}
