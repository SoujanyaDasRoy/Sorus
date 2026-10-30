# Sorus Core Engine v1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the shared, platform-free C++ DSP engine (preamp → 32-band parametric EQ → true-peak limiter), its
preset format and C API, with tests and a performance gate. Both the Windows APO and the Android app link this later.

**Architecture:** Static library `sorus_core` in `core/`, C++20, no platform code. Control thread edits settings and
pushes commands through a lock-free SPSC queue; the audio thread only pops commands and processes interleaved float
audio in place. A plain C ABI (`sorus.h`) is the only thing platforms call.

**Tech Stack:** C++20, CMake ≥ 3.25, Ninja, MSVC 2022 or clang; doctest 2.4.11 (tests); nlohmann/json 3.11.3 (presets).
Both MIT, fetched with CMake `FetchContent`.

**Spec:** `docs/superpowers/specs/2026-10-30-sorus-design.md` (§7.1, §7.2, §8, §9). Roadmap: `docs/superpowers/plans/2026-10-30-sorus-roadmap.md` (this is M2).

## Global Constraints

- C++20. `core/` contains no OS, Android, or Windows headers.
- Audio thread (`process_interleaved` and everything it calls): no allocation, no locks, no syscalls, no logging.
- EQ: max **32 bands**; freq **10 Hz – 22 kHz** (and ≤ 0.45 × sample rate); gain **±24 dB**; Q **0.1 – 40**;
  HP/LP slope **6 – 96 dB/oct** in steps of 6.
- Band types: bell, low shelf, high shelf, low-pass, high-pass, notch, band-pass, tilt, all-pass.
  Channel modes: stereo, left, right, mid, side.
- Minimum-phase only in v1 (linear phase is M6). DSP latency **< 5 ms**.
- Full v1 chain **< 1% of one core** on a typical PC at 48 kHz stereo, 32 bands.
- Channels supported: **1 – 8**. Sample rates: **8 kHz – 384 kHz**.
- Dependencies: free, open-source, GPL-3.0-compatible only. Project licence **GPL-3.0**.
- Parameter changes must never click (smoothed on the audio thread).
- *Plan call:* v1 is scalar C++ relying on compiler auto-vectorization. Explicit SIMD (NEON, SSE4.1/AVX2 dispatch)
  lands in M6 with the heavier effects; Task 10's bench gate fails loudly if v1 misses budget without it.

## Review Focus

1. **Odd sample rates (44.1 k, 96 k, 192 k) with bands near Nyquist** — must stay stable, never blow up → Task 2 test `near-Nyquist band stays stable`.
2. **NaN / Inf in the input stream** (buggy host or app) — one bad sample must not poison filter state forever → Task 6 test `non-finite input is sanitized`.
3. **Long silence after loud audio** (denormals) — CPU must not spike → Task 6 test `denormals flushed during process` + Task 10 bench case `decay`.
4. **Host buffers larger than `max_frames`** — must process correctly, not overrun → Task 6 test `blocks larger than max_frames`.
5. **5.1 / 7.1 outputs (6–8 channels)** — stereo bands hit every channel, L/R/M/S bands only the front pair → Task 3 test `six channels routing`.

---

## File Structure

```
.gitignore  LICENSE  README.md
.github/workflows/core.yml          CI: build + test + bench (Windows, Linux)
core/
  CMakeLists.txt                    sorus_core lib, sorus_tests, sorus_no_alloc_test, sorus_bench
  include/sorus/sorus.h             public C API (only public header)
  src/params.h / params.cpp         BandParams, enums, clamp()
  src/band.h / band.cpp             one EQ band: SVF sections + smoothing, response_db()
  src/parametric_eq.h / .cpp        32 bands, channel routing (L/R/M/S), peak_boost_db()
  src/spsc_queue.h                  lock-free single-producer single-consumer queue
  src/limiter.h / limiter.cpp       true-peak lookahead limiter
  src/denormals.h                   ScopedFlushDenormals
  src/engine.h / engine.cpp         chain + commands + interleaved I/O
  src/preset.h / preset.cpp         Preset struct, JSON parse/serialize
  src/dp_fit.h / dp_fit.cpp         fit parametric curve to Android DynamicsProcessing bands
  src/sorus_c_api.cpp               C ABI wrapper
  tests/test_util.h                 sine, rms, measure_gain_db, true_peak_db
  tests/test_*.cpp                  one file per task
  tests/test_no_alloc.cpp           separate binary (replaces global operator new)
  bench/bench.cpp
```

Build & test commands used throughout (PowerShell, from repo root):

```bash
cmake -S core -B build/core -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/core
ctest --test-dir build/core --output-on-failure -R "<test name regex>"
```

---

### Task 0: Repo, toolchain, build skeleton

**Files:**
- Create: `.gitignore`, `LICENSE`, `README.md`, `core/CMakeLists.txt`, `core/tests/test_smoke.cpp`

**Interfaces:**
- Produces: CMake target `sorus_core` (STATIC), test executable `sorus_tests` (all `tests/test_*.cpp` except `test_no_alloc.cpp`, doctest main), registered with CTest via `doctest_discover_tests`.

- [ ] **Step 1: Verify toolchain**

Run: `git --version; cmake --version; ninja --version; cl` (inside "Developer PowerShell for VS 2022") or `clang++ --version`
Expected: git present, CMake ≥ 3.25, Ninja present, a C++20 compiler. If anything is missing, stop and ask the owner to
install (free): Visual Studio 2022 Build Tools with "Desktop development with C++", CMake, Ninja.

- [ ] **Step 2: `git init`; add `.gitignore` (`build/`, `.vs/`, `*.user`, `.idea/`, `local.properties`, `.gradle/`), `LICENSE` = GPL-3.0 text from gnu.org, `README.md` (one paragraph from spec §1).**

- [ ] **Step 3: Write `core/CMakeLists.txt`** — `sorus_core` from `src/*.cpp` (empty placeholder `src/params.cpp` for now),
include dirs `include/` (PUBLIC) and `src/` (PRIVATE, but tests get it too); `FetchContent` doctest `v2.4.11` and
nlohmann_json `v3.11.3`; warnings as errors (`/W4 /WX` or `-Wall -Wextra -Werror`); `enable_testing()`.

- [ ] **Step 4: Smoke test**

```cpp
// core/tests/test_smoke.cpp
#include <doctest/doctest.h>
TEST_CASE("smoke") { CHECK(1 + 1 == 2); }
```

- [ ] **Step 5: Build and run** — `ctest ... -R smoke` → `1 test passed`.

- [ ] **Step 6: Commit** — `git add -A; git commit -m "chore: repo skeleton, core build, test harness"`

---

### Task 1: Band parameters and clamping

**Files:** Create `core/src/params.h`, `core/src/params.cpp`, `core/tests/test_params.cpp`

**Interfaces:**
- Produces (namespace `sorus`):
  - `enum class FilterType { Bell, LowShelf, HighShelf, LowPass, HighPass, Notch, BandPass, Tilt, AllPass };`
  - `enum class ChannelMode { Stereo, Left, Right, Mid, Side };`
  - `struct BandParams { FilterType type = FilterType::Bell; double freq = 1000; double gain_db = 0; double q = 0.7071; int slope_db_oct = 12; ChannelMode mode = ChannelMode::Stereo; bool enabled = true; bool operator==(const BandParams&) const = default; };`
  - `BandParams clamp(BandParams p, double sample_rate);` — freq to [10, min(22000, 0.45·sr)], gain ±24, q [0.1, 40], slope rounded to nearest multiple of 6 within [6, 96]. Non-finite values → field default.
  - `constexpr int kMaxBands = 32; constexpr int kMaxChannels = 8;`

- [ ] **Step 1: Failing test**

```cpp
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
```

- [ ] **Step 2: Run** `-R clamp` → FAIL (compile error, `clamp` undefined).
- [ ] **Step 3: Implement** `params.h/.cpp` per Interfaces.
- [ ] **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `feat(core): band params and clamping`

---

### Task 2: EQ band (SVF filters + analytic response)

**Files:** Create `core/src/band.h`, `core/src/band.cpp`, `core/tests/test_util.h`, `core/tests/test_band.cpp`

**Interfaces:**
- Consumes: `BandParams`, `clamp`, `FilterType`.
- Produces:
  - `double response_db(const BandParams& p, double freq, double sample_rate);` — exact magnitude of what `Band` does (disabled band → 0).
  - `class Band { public: static constexpr int kMaxSlots = 8; void prepare(double sample_rate); void set(const BandParams& p); void reset(); void process(float* buf, int frames, int slot); };`
    `slot` = independent filter state per routed channel (0..7). `set` clamps internally.
  - `tests/test_util.h`: `std::vector<float> sine(double freq, double sr, int frames, float amp = 0.25f);`
    `double rms(const float* x, int n);`
    `double measure_gain_db(const std::function<void(float*, int)>& process, double sr, double freq);` (1 s sine, gain from RMS of last 0.5 s)
    `double true_peak_db(const std::vector<float>& x);` (16× windowed-sinc interpolation, max |x| in dBFS).

Approach: Cytomic trapezoidal SVF ("SvfLinearTrapOptimised2") for every 2nd-order type. HP/LP of order N = slope/6:
⌊N/2⌋ SVF sections with Butterworth Qs (`Q_k = 1 / (2·cos(π(2k+1)/(2N)))`, k = 0..⌊N/2⌋−1) plus one TPT one-pole
if N is odd. Tilt = low shelf (−gain/2) + high shelf (+gain/2) at `freq`. Per-sample one-pole smoothing (τ = 10 ms) of
the SVF coefficients toward their targets. `response_db` evaluates the bilinear-transformed (pre-warped) analog
prototypes — the same math, so measured and analytic agree.

- [ ] **Step 1: Failing tests**

```cpp
using namespace sorus;
static double measured(BandParams p, double sr, double f) {
  Band b; b.prepare(sr); b.set(p);
  return measure_gain_db([&](float* x, int n) { b.process(x, n, 0); }, sr, f);
}
TEST_CASE("bell hits its gain at centre") {
  BandParams p; p.freq = 1000; p.gain_db = 6; p.q = 1;
  CHECK(response_db(p, 1000, 48000) == doctest::Approx(6).epsilon(0.001));
  CHECK(std::abs(measured(p, 48000, 1000) - 6) < 0.05);
}
TEST_CASE("measured response matches response_db for every type") {
  for (auto t : {FilterType::Bell, FilterType::LowShelf, FilterType::HighShelf, FilterType::LowPass, FilterType::HighPass,
                 FilterType::Notch, FilterType::BandPass, FilterType::Tilt, FilterType::AllPass}) {
    BandParams p; p.type = t; p.freq = 1000; p.gain_db = 9; p.q = 2; p.slope_db_oct = 30;
    for (double f : {30.0, 100.0, 300.0, 1000.0, 3000.0, 10000.0, 18000.0}) {
      double a = response_db(p, f, 48000);
      if (a < -40) continue;  // deep stopband/notch: RMS measurement is noise-limited
      CAPTURE(static_cast<int>(t)); CAPTURE(f);
      CHECK(std::abs(measured(p, 48000, f) - a) < 0.1);
    }
  }
}
TEST_CASE("high-pass slope is 48 dB per octave") {
  BandParams p; p.type = FilterType::HighPass; p.freq = 1000; p.slope_db_oct = 48;
  CHECK(response_db(p, 250, 48000) - response_db(p, 125, 48000) == doctest::Approx(48).epsilon(0.02));
}
TEST_CASE("tilt spreads gain around its pivot") {
  BandParams p; p.type = FilterType::Tilt; p.freq = 1000; p.gain_db = 6;
  CHECK(std::abs(response_db(p, 20, 48000) + 3) < 0.5);
  CHECK(std::abs(response_db(p, 20000, 48000) - 3) < 0.5);
}
TEST_CASE("near-Nyquist band stays stable") {
  for (double sr : {44100.0, 96000.0, 192000.0}) {
    BandParams p; p.freq = 30000; p.gain_db = 24; p.q = 40;
    Band b; b.prepare(sr); b.set(p);
    std::vector<float> x(static_cast<int>(sr)); uint32_t s = 1;
    for (auto& v : x) { s = s * 1664525u + 1013904223u; v = (s >> 8) / 16777216.0f - 0.5f; }
    b.process(x.data(), (int)x.size(), 0);
    for (float v : x) REQUIRE((std::isfinite(v) && std::abs(v) < 100));
  }
}
```

- [ ] **Step 2: Run** `-R "bell|measured|slope|tilt|Nyquist"` → FAIL (undefined).
- [ ] **Step 3: Implement** `test_util.h`, then `Band` and `response_db`.
- [ ] **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `feat(core): SVF EQ band with analytic response`

---

### Task 3: Parametric EQ (32 bands, routing)

**Files:** Create `core/src/parametric_eq.h`, `core/src/parametric_eq.cpp`, `core/tests/test_parametric_eq.cpp`

**Interfaces:**
- Consumes: `Band`, `BandParams`, `response_db`, `kMaxBands`, `kMaxChannels`.
- Produces: `class ParametricEq { public: void prepare(double sample_rate, int max_frames, int channels); void set_band(int index, const BandParams& p); void process(float* const* ch, int channels, int frames); double response_db(double freq) const; double peak_boost_db() const; };`
  - Routing: `Stereo` bands → every channel. `Left`/`Right` → channel 0/1 only. `Mid`/`Side` → encode ch0/ch1 to M=(L+R)/2, S=(L−R)/2, filter, decode. With 1 channel, L/R/M/S bands act as `Stereo`.
  - `response_db(freq)` = sum of all enabled bands (display curve). `peak_boost_db()` = max(0, max of `response_db` over 512 log-spaced points 10 Hz–22 kHz). Control-thread use only.
  - `prepare` allocates; `set_band`/`process` never allocate.

- [ ] **Step 1: Failing tests**

```cpp
using namespace sorus;
// helper: run a sine on all channels through eq, return per-channel gain in dB
static std::vector<double> gains(ParametricEq& eq, int nch, double f);
TEST_CASE("left-only band leaves right untouched") {
  ParametricEq eq; eq.prepare(48000, 48000, 2);
  BandParams p; p.gain_db = 12; p.mode = ChannelMode::Left; eq.set_band(0, p);
  auto g = gains(eq, 2, 1000);
  CHECK(std::abs(g[0] - 12) < 0.1); CHECK(std::abs(g[1]) < 0.01);
}
TEST_CASE("side band does not change a mono signal") {
  ParametricEq eq; eq.prepare(48000, 48000, 2);
  BandParams p; p.gain_db = 12; p.mode = ChannelMode::Side; eq.set_band(0, p);
  auto g = gains(eq, 2, 1000);
  CHECK(std::abs(g[0]) < 0.01); CHECK(std::abs(g[1]) < 0.01);
}
TEST_CASE("six channels routing") {
  ParametricEq eq; eq.prepare(48000, 48000, 6);
  BandParams a; a.gain_db = 6; eq.set_band(0, a);
  BandParams b = a; b.mode = ChannelMode::Left; eq.set_band(1, b);
  auto g = gains(eq, 6, 1000);
  CHECK(std::abs(g[0] - 12) < 0.1);
  for (int c = 1; c < 6; ++c) CHECK(std::abs(g[c] - 6) < 0.1);
}
TEST_CASE("gain change mid-stream does not click") {
  ParametricEq eq; eq.prepare(48000, 48000, 1);
  auto x = sine(1000, 48000, 48000, 0.5f);
  float* ch[1] = {x.data()};
  eq.process(ch, 1, 24000);
  BandParams p; p.gain_db = 24; eq.set_band(0, p);
  float* ch2[1] = {x.data() + 24000};
  eq.process(ch2, 1, 24000);
  double steady = 2 * std::numbers::pi * 1000 / 48000 * 0.5 * std::pow(10, 24 / 20.0);  // max per-sample delta at +24 dB
  double worst = 0;
  for (int i = 1; i < 48000; ++i) worst = std::max(worst, (double)std::abs(x[i] - x[i - 1]));
  CHECK(worst <= 1.2 * steady);
}
TEST_CASE("peak boost of stacked bells") {
  ParametricEq eq; eq.prepare(48000, 512, 2);
  BandParams p; p.gain_db = 6; p.q = 1; eq.set_band(0, p); eq.set_band(1, p);
  CHECK(eq.peak_boost_db() == doctest::Approx(12).epsilon(0.01));
  p.gain_db = -6; eq.set_band(0, p); eq.set_band(1, p);
  CHECK(eq.peak_boost_db() == 0);
}
```

- [ ] **Step 2: Run** `-R "left-only|side band|six channels|mid-stream|peak boost"` → FAIL.
- [ ] **Step 3: Implement** `ParametricEq` (and the `gains` test helper).
- [ ] **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `feat(core): 32-band parametric EQ with L/R/M/S routing`

---

### Task 4: Lock-free SPSC queue

**Files:** Create `core/src/spsc_queue.h`, `core/tests/test_spsc_queue.cpp`

**Interfaces:**
- Produces: `template <class T, size_t N> class SpscQueue { public: bool push(const T& v); bool pop(T& out); };` — N a power of two, holds up to N items, `std::atomic<size_t>` head/tail with acquire/release, no allocation after construction. `T` must be trivially copyable (`static_assert`).

- [ ] **Step 1: Failing tests**

```cpp
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
```

- [ ] **Step 2: Run** `-R spsc` → FAIL. **Step 3: Implement.** **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `feat(core): lock-free SPSC queue`

---

### Task 5: True-peak limiter

**Files:** Create `core/src/limiter.h`, `core/src/limiter.cpp`, `core/tests/test_limiter.cpp`

**Interfaces:**
- Produces: `class TruePeakLimiter { public: void prepare(double sample_rate, int max_frames, int channels); void set_ceiling_db(double db); int latency_frames() const; void process(float* const* ch, int channels, int frames); };`
  - Defaults: ceiling −1.0 dBTP, lookahead 1.5 ms, release 50 ms. Linked gain across channels.
  - Detection: 4× oversampled peak (polyphase FIR) per ITU-R BS.1770 true-peak practice.
  - `latency_frames()` = lookahead in frames + detector group delay, constant after `prepare`.

- [ ] **Step 1: Failing tests**

```cpp
using namespace sorus;
TEST_CASE("limiter keeps true peak under ceiling") {
  TruePeakLimiter l; l.prepare(48000, 48000, 1);
  std::vector<float> x(48000);
  for (int i = 0; i < 48000; ++i) x[i] = 2.0f * std::sin(std::numbers::pi / 2 * i + std::numbers::pi / 4);  // fs/4, inter-sample peaks, +6 dBFS
  float* ch[1] = {x.data()}; l.process(ch, 1, 48000);
  std::vector<float> tail(x.begin() + 4800, x.end());
  CHECK(true_peak_db(tail) <= -1.0 + 0.2);
}
TEST_CASE("limiter is transparent below ceiling") {
  TruePeakLimiter l; l.prepare(48000, 48000, 1);
  auto x = sine(1000, 48000, 48000, 0.25f); auto in = x;
  float* ch[1] = {x.data()}; l.process(ch, 1, 48000);
  int d = l.latency_frames();
  for (int i = d; i < 48000; ++i) REQUIRE(std::abs(x[i] - in[i - d]) < 1e-4);
}
TEST_CASE("limiter latency under 5 ms") {
  for (double sr : {44100.0, 48000.0, 96000.0, 192000.0}) {
    TruePeakLimiter l; l.prepare(sr, 512, 2);
    CHECK(l.latency_frames() < 0.005 * sr);
  }
}
```

- [ ] **Step 2: Run** `-R limiter` → FAIL. **Step 3: Implement.** **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `feat(core): true-peak lookahead limiter`

---

### Task 6: Engine (chain, commands, interleaved I/O)

**Files:** Create `core/src/denormals.h`, `core/src/engine.h`, `core/src/engine.cpp`, `core/tests/test_engine.cpp`, `core/tests/test_no_alloc.cpp`; Modify `core/CMakeLists.txt` (add `sorus_no_alloc_test` executable, own doctest main, registered with CTest).

**Interfaces:**
- Consumes: `ParametricEq`, `TruePeakLimiter`, `SpscQueue`, `BandParams`, `kMaxChannels`.
- Produces:
  - `class ScopedFlushDenormals` (RAII; sets FTZ+DAZ in MXCSR on x86/x64, FZ in FPCR on arm64; restores on exit).
  - `enum class Block { Eq, Limiter };`
  - `class Engine` —
    control thread: `Engine(double sample_rate, int max_frames, int channels); void set_band(int index, const BandParams& p); void set_preamp_db(double db); void set_auto_preamp(bool on); void set_limiter_ceiling_db(double db); void set_bypass(Block b, bool bypassed); double eq_response_db(double freq) const; int latency_frames() const; void load(const Preset& p);` (`load` added in Task 7)
    audio thread: `void process_interleaved(float* data, int frames);`
  - Chain: sanitize (non-finite → 0) → preamp → EQ → limiter. Effective preamp = manual preamp − (auto ? `peak_boost_db()` of a control-side mirror EQ : 0), computed on the control thread and sent as one command.
  - Latency is constant = limiter latency; when the limiter is bypassed, a delay line of the same length keeps alignment.
  - Bypassing EQ also drops the auto-preamp offset. Preamp and bypass changes are smoothed (10 ms).
  - `frames > max_frames` → processed internally in chunks of `max_frames`.
  - Command queue: `SpscQueue<Command, 256>`; if full, control-side setters keep the latest value and retry on the next call (never block).

- [ ] **Step 1: Failing tests** (`test_engine.cpp`)

```cpp
using namespace sorus;
// helper: interleaved stereo sine through engine, returns gain dB of channel `c` (accounts for latency)
static double engine_gain_db(Engine& e, int c, double f);
TEST_CASE("auto preamp cancels the EQ peak") {
  Engine e(48000, 512, 2);
  BandParams p; p.gain_db = 6; p.q = 1; e.set_band(0, p); e.set_auto_preamp(true);
  CHECK(std::abs(engine_gain_db(e, 0, 1000)) < 0.1);
}
TEST_CASE("manual preamp applies when auto is off") {
  Engine e(48000, 512, 2); e.set_auto_preamp(false); e.set_preamp_db(-3);
  CHECK(std::abs(engine_gain_db(e, 1, 10000) + 3) < 0.1);
}
TEST_CASE("bypassed EQ is transparent and latency stays constant") {
  Engine e(48000, 512, 2);
  int lat = e.latency_frames();
  BandParams p; p.gain_db = 12; e.set_band(0, p); e.set_bypass(Block::Eq, true);
  CHECK(std::abs(engine_gain_db(e, 0, 1000)) < 0.05);
  e.set_bypass(Block::Limiter, true);
  CHECK(e.latency_frames() == lat);
}
TEST_CASE("non-finite input is sanitized") {
  Engine e(48000, 512, 2);
  std::vector<float> buf(1024, 0.1f); buf[10] = NAN; buf[11] = INFINITY;
  e.process_interleaved(buf.data(), 512);
  for (int i = 0; i < 20; ++i) { e.process_interleaved(buf.data(), 512); std::fill(buf.begin(), buf.end(), 0.1f); }
  for (float v : buf) REQUIRE(std::isfinite(v));
  CHECK(std::abs(engine_gain_db(e, 0, 1000)) < 0.1);
}
TEST_CASE("blocks larger than max_frames") {
  Engine big(48000, 4096, 2), small(48000, 256, 2);
  auto mono = sine(440, 48000, 4096);
  std::vector<float> a(8192), b(8192);
  for (int i = 0; i < 4096; ++i) a[2 * i] = a[2 * i + 1] = b[2 * i] = b[2 * i + 1] = mono[i];
  big.process_interleaved(a.data(), 4096); small.process_interleaved(b.data(), 4096);
  for (int i = 0; i < 8192; ++i) REQUIRE(a[i] == doctest::Approx(b[i]).epsilon(1e-6));
}
TEST_CASE("denormals flushed during process") {
  ScopedFlushDenormals g;
  volatile float tiny = 1e-40f;
  CHECK(tiny * 1.0f == 0.0f);
}
```

`test_no_alloc.cpp`: replace global `operator new`/`delete` with counting versions.

```cpp
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
```

- [ ] **Step 2: Run** `ctest ... -R "auto preamp|manual preamp|bypassed|non-finite|larger than|denormals|never allocates"` → FAIL.
- [ ] **Step 3: Implement** `denormals.h`, `Engine`.
- [ ] **Step 4: Run** → PASS (both binaries).
- [ ] **Step 5: Commit** `feat(core): engine chain with lock-free commands`

---

### Task 7: Presets (JSON)

**Files:** Create `core/src/preset.h`, `core/src/preset.cpp`, `core/tests/test_preset.cpp`; Modify `core/src/engine.h/.cpp` (add `Engine::load`).

**Interfaces:**
- Consumes: `BandParams`, `clamp`, `kMaxBands`, `Engine`.
- Produces:
  - `struct Preset { std::string name; double preamp_db = 0; bool auto_preamp = true; bool limiter_enabled = true; double limiter_ceiling_db = -1.0; std::vector<BandParams> bands; bool operator==(const Preset&) const = default; };`
  - `std::optional<Preset> parse_preset(std::string_view json, std::string* error);`
  - `std::string to_json(const Preset& p);`
  - `void Engine::load(const Preset& p);` — sets every field; bands beyond `p.bands.size()` are disabled.
- Format v1 (file extension `.sorus`). Type strings: `bell low_shelf high_shelf low_pass high_pass notch band_pass tilt all_pass`; mode strings: `stereo left right mid side`. Omitted band fields take `BandParams` defaults. Unknown keys ignored. Band values clamped to the spec ranges with freq in [10, 22000]; the engine re-clamps against its own sample rate.

```json
{ "sorus_preset": 1, "name": "Warm", "preamp_db": 0, "auto_preamp": true,
  "limiter": { "enabled": true, "ceiling_db": -1.0 },
  "eq": [ { "type": "high_pass", "freq": 20, "slope": 24 },
          { "type": "bell", "freq": 120, "gain_db": 3, "q": 0.7, "mode": "stereo", "enabled": true } ] }
```

- [ ] **Step 1: Failing tests**

```cpp
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
```

- [ ] **Step 2: Run** `-R preset` → FAIL. **Step 3: Implement** (nlohmann/json; catch its exceptions at the boundary → `"invalid json: <what>"`). **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `feat(core): versioned JSON presets`

---

### Task 8: C API

**Files:** Create `core/include/sorus/sorus.h`, `core/src/sorus_c_api.cpp`, `core/tests/test_c_api.cpp`

**Interfaces:**
- Consumes: `Engine`, `parse_preset`, `fit_to_dp_bands` (Task 9 adds that entry point).
- Produces (exact header; C linkage, `SORUS_API` export macro empty for static builds):

```c
typedef struct SorusEngine SorusEngine;
/* NULL if sample_rate outside [8000, 384000], max_frames < 1, or channels outside [1, 8]. */
SorusEngine* sorus_create(double sample_rate, int max_frames, int channels);
void sorus_destroy(SorusEngine* e);                                   /* NULL-safe */
/* audio thread */
void sorus_process(SorusEngine* e, float* interleaved, int frames);   /* NULL-safe, frames <= 0 no-op */
/* control thread; return 0 on success, -1 on bad args/parse error */
int sorus_load_preset(SorusEngine* e, const char* json, size_t len);
int sorus_last_error(const SorusEngine* e, char* buf, size_t len);    /* copies last parse error */
int sorus_latency_frames(const SorusEngine* e);
int sorus_eq_response_db(const SorusEngine* e, const double* freqs, double* out_db, int n);
```

- [ ] **Step 1: Failing tests**

```cpp
TEST_CASE("c api rejects bad create args") {
  CHECK(sorus_create(1000, 512, 2) == nullptr);
  CHECK(sorus_create(48000, 0, 2) == nullptr);
  CHECK(sorus_create(48000, 512, 9) == nullptr);
}
TEST_CASE("c api null safety") {
  sorus_destroy(nullptr); sorus_process(nullptr, nullptr, 10);
  CHECK(sorus_load_preset(nullptr, "{}", 2) == -1);
  CHECK(sorus_latency_frames(nullptr) == -1);
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
  sorus_destroy(e);
}
```

- [ ] **Step 2: Run** `-R "c api"` → FAIL. **Step 3: Implement** (no C++ exception may cross the C boundary — catch all, return −1). **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `feat(core): C API`

---

### Task 9: Android DynamicsProcessing fitter

**Files:** Create `core/src/dp_fit.h`, `core/src/dp_fit.cpp`, `core/tests/test_dp_fit.cpp`; Modify `sorus.h`, `sorus_c_api.cpp`.

**Interfaces:**
- Consumes: `BandParams`, `response_db`.
- Produces:
  - `void fit_to_dp_bands(const std::vector<BandParams>& bands, double sample_rate, const float* cutoff_hz, int n, float* gain_db_out);`
    DynamicsProcessing bands are defined by their **upper** cutoff. Band i spans [cutoff[i−1], cutoff[i]] (band 0 starts at 20 Hz). Output gain = summed parametric response at the band's geometric centre, clamped to ±24 dB. All bands of every channel mode are summed (DP is stereo-linked here).
  - C: `int sorus_fit_dp_bands(const char* preset_json, size_t len, double sample_rate, const float* cutoff_hz, int n, float* gain_db_out);` (0 ok, −1 on bad args/parse error).

- [ ] **Step 1: Failing tests**

```cpp
using namespace sorus;
TEST_CASE("flat curve fits to zero gains") {
  float cut[4] = {100, 1000, 5000, 20000}, g[4];
  fit_to_dp_bands({}, 48000, cut, 4, g);
  for (float v : g) CHECK(v == 0);
}
TEST_CASE("bell lands in the band containing its centre") {
  float cut[3] = {500, 2000, 20000}, g[3];   // band 1 spans 500..2000, centre = 1000
  BandParams p; p.freq = 1000; p.gain_db = 6; p.q = 1;
  fit_to_dp_bands({p}, 48000, cut, 3, g);
  CHECK(g[1] == doctest::Approx(6).epsilon(0.02));
  CHECK(g[0] < g[1]); CHECK(g[2] < g[1]);
}
```

- [ ] **Step 2: Run** `-R "fits|lands"` → FAIL. **Step 3: Implement.** **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `feat(core): DynamicsProcessing band fitter`

---

### Task 10: Benchmark gate + CI

**Files:** Create `core/bench/bench.cpp`, `.github/workflows/core.yml`; Modify `core/CMakeLists.txt` (add `sorus_bench`).

**Interfaces:**
- Consumes: `Engine`, `BandParams`.
- Produces: `sorus_bench` executable. Prints one line per case: `<case> realtime_fraction=<x>`; exit code 1 if a gate fails.
  - Case `full`: 48 kHz stereo, 32 enabled bells (spread 30 Hz–16 kHz, ±6 dB, Q 1–4), limiter on, 60 s of noise in 512-frame blocks. **Gate: realtime_fraction < 0.01.**
  - Case `decay`: same chain, 1 s loud noise then 59 s digital silence. **Gate: decay time ≤ 1.5 × full time** (catches denormals).

- [ ] **Step 1: Write `bench.cpp`** per Interfaces (time with `std::chrono::steady_clock`, best of 3 runs).
- [ ] **Step 2: Run** `build/core/sorus_bench` (Release) → both cases print, exit code 0. If `full` fails the gate, stop and report numbers to the owner — do not loosen the gate.
- [ ] **Step 3: Write `core.yml`**: on push/PR; matrix `windows-latest` (MSVC) + `ubuntu-latest` (clang); configure, build Release, `ctest --output-on-failure`, run `sorus_bench` (gate enforced on Windows only; Linux prints numbers — shared CI runners are noisy).
- [ ] **Step 4: Run all tests locally** — `ctest --test-dir build/core --output-on-failure` → all pass.
- [ ] **Step 5: Commit** `ci(core): bench gate and CI workflow`

---

## After this plan

- Ask the owner before creating the public GitHub repo and pushing (outward-facing).
- Then run spikes **A3** (uses Task 9), **W2**, **A2** per the roadmap, and write the M3 (Windows v0) and M4 (Android v0) plans.
