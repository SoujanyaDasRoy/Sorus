# Sorus Android v0 (M4) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** An installable Android app that applies the user's Sorus EQ system-wide in **session mode**: a fitted
`DynamicsProcessing` EQ on every audio session it can reach, with a Simple tab and an honest per-app coverage list.

**Architecture:**
- **Session discovery:** a foreground service discovers audio sessions two ways. Every app gets the broadcast path; with Shizuku, `dumpsys audio` is parsed as well (A2: without it, only Spotify of 5 apps was reached).
- **EQ:** the service fits the active `.sorus` preset onto 64 log-spaced DynamicsProcessing bands through the shared core over JNI (`sorus_fit_dp_bands`), and drives one `DynamicsProcessing` per session.
- **UI:** the Compose UI only edits the persisted preset and reads coverage from the service.
- Full mode (A1/A4) is M5 and out of scope here.

**Tech Stack:** Kotlin, Jetpack Compose (Material 3), AGP 9.0.1 (built-in Kotlin) + Gradle 9.1.0 wrapper, JDK from
Android Studio (25), NDK + CMake from the SDK (`sdkmanager`), shared C++20 core via JNI, Shizuku API 13.1.5, JUnit 4
(JVM unit tests), AndroidX Test (instrumented, on the Pixel 8 emulator and the owner's Galaxy A55).

**Spec:** `docs/superpowers/specs/2026-10-30-sorus-design.md` (§6.1, §7.6, §8, §9, §10, §11). Spike results it builds on:
`docs/spikes/A1.md`, `A2.md`, `A3.md`, `A4.md`.

## Global Constraints

- `minSdk 29` (Android 10), `targetSdk 36`, `compileSdk 36`. Application id `dev.sorus.android`.
- No root, and no PC for any setup step. Shizuku is optional and adds discovery only.
- Everything free. Distribution is GitHub Releases + F-Droid (no Play Store), so `QUERY_ALL_PACKAGES` is allowed. It's needed to name apps (A2 finding 3).
- Never silently skip an app (spec §5): every session the service sees gets a coverage row with a reason.
- Session-effect ownership: exactly one Sorus `DynamicsProcessing` per session, priority `Int.MAX_VALUE` (A2 finding 4). Losing control to another app is shown as "taken over by another effects app".
- UI draws edge-to-edge and handles system-bar insets (A1 finding 6).
- Preset format: `.sorus` JSON v1, same file as Windows. Defaults when keys are absent: `preamp_db 0`, `auto_preamp true`, `limiter.enabled true`, `limiter.ceiling_db −1` (`core/src/preset.h`).
- DynamicsProcessing layout (A3): **64 pre-EQ bands**, log-spaced, cutoff i = 20·1000^((i+1)/64), **last cutoff = sample rate / 2**; **band-average fit**; `VARIANT_FAVOR_FREQUENCY_RESOLUTION`; frame duration **platform default** (don't call `setPreferredFrameDuration`). A3: 40 ms frames cut bass-curve error from ~2.9 to ~1.4 dB, but likely add latency in the mode used for video apps. Revisit in M6 with a latency measurement.

## Review Focus

1. **A session with id 0 or −1 in dumpsys** (the output mix; Chrome's AAudio). Never attach to it, and show "can't be reached (no audio session)". Test in Task 4.
2. **dumpsys misses a live player for one poll.** The effect must not be released and re-created (that flickers, A1 finding 2): release only after 3 consecutive misses. Test in Task 4.
3. **Shizuku stops** (reboot, Wi-Fi change). Discovery falls back to broadcasts only, without crashing, and the UI shows "Shizuku not running: only apps that announce their audio are processed". Test in Task 4.
4. **Preset edited while 10+ sessions are live.** Every session gets the new gains within 200 ms, with no re-creation of effects (re-creating can lose control). Test in Task 5.
5. **Malformed or oversize preset** imported via the file picker: the core's error text is shown, and the active preset is unchanged. Test in Task 6.

---

## File Structure

```
core/CMakeLists.txt                       + option(SORUS_BUILD_TESTS ON); OFF skips tests/bench/doctest
core/src/dp_fit.{h,cpp}                   band-average fit (A3)
core/tests/test_dp_fit.cpp                + band-average expectations
android/
  settings.gradle.kts, build.gradle.kts, gradle.properties, gradle/wrapper/*, gradlew(.bat)
  native/                                 Android library: JNI over the core
    build.gradle.kts, src/main/cpp/{CMakeLists.txt, sorus_jni.cpp}
    src/main/kotlin/dev/sorus/natives/SorusNative.kt
    src/androidTest/.../SorusNativeTest.kt
  app/
    build.gradle.kts, src/main/AndroidManifest.xml
    src/main/kotlin/dev/sorus/android/
      preset/PresetStore.kt, preset/Graphic.kt, preset/PresetInfo.kt
      eq/DpLayout.kt, eq/SessionEq.kt
      session/Players.kt, session/SessionTracker.kt, session/Coverage.kt
      shizuku/ShizukuShell.kt
      service/SorusService.kt
      ui/MainActivity.kt, ui/SimpleScreen.kt, ui/SimpleViewModel.kt
    src/test/...                          JVM unit tests (+ fixtures/dumpsys_*.txt from the spikes)
    src/androidTest/...                   instrumented tests
.github/workflows/android.yml
```

---

### Task 1: Core changes for Android (fit + build option)

**Files:**
- Modify: `core/src/dp_fit.h`, `core/src/dp_fit.cpp`, `core/tests/test_dp_fit.cpp`, `core/CMakeLists.txt`

**Interfaces:**
- Produces:
  - `fit_to_dp_bands` (same signature) now returns the **mean of the summed response in dB over 16 log-spaced points per band**, clamped to ±24. A3 measured it as better than centre sampling (worst RMS 0.71 vs 2.17 dB at 64 bands).
  - CMake `option(SORUS_BUILD_TESTS "..." ON)`. When OFF, only `sorus_core` (and `sorus_shared`) are defined, and doctest isn't fetched.

- [ ] **Step 1:** Write `TEST_CASE("dp fit averages across the band")` with one band spanning [1000, 2000] Hz and a Bell 1414 Hz +6 dB Q 4:
  - the fitted gain equals the mean of `response_db` at 16 log points (compute it in the test), ± 1e-6;
  - it is lower than the response at the centre.

  Keep the existing dp_fit tests; adjust their expected values only where they assumed centre sampling.
- [ ] **Step 2:** Run `cmd /c "scripts\core.cmd -R dp"`. Expected: FAIL.
- [ ] **Step 3:** Implement. Update the `ponytail:` comment in `dp_fit.h` (resolved by A3). Add the option.
- [ ] **Step 4:** Run `cmd /c "scripts\core.cmd"`. Expected: all pass. Also configure with `-DSORUS_BUILD_TESTS=OFF` and confirm only the library builds.
- [ ] **Step 5:** Commit `feat(core): band-average DP fit; SORUS_BUILD_TESTS option`.

### Task 2: Android project skeleton + JNI module

**Files:**
- Create: everything under `android/` except `app/src/main/kotlin/**` contents beyond `MainActivity` stub; `android/native/**`

**Interfaces:**
- Consumes: `sorus_fit_dp_bands` from `core/include/sorus/sorus.h`.
- Produces `object SorusNative` (module `:native`, package `dev.sorus.natives`):
  - `fun fitDpBands(presetJson: String, sampleRate: Double, cutoffsHz: FloatArray): FloatArray?` (null on parse error);
  - `fun lastFitError(): String`: the parse error text of the last failed call, thread-local on the C++ side;
  - the library name is `sorus_jni`; ABIs are `arm64-v8a` and `x86_64`.

- [ ] **Step 1:** Install the toolchain (free):

  ```bash
  "%LOCALAPPDATA%\Android\Sdk\cmdline-tools\latest\bin\sdkmanager.bat" "ndk;28.2.13676358" "cmake;3.31.6"
  ```

  Use the newest stable versions `sdkmanager --list` shows if these exact ones aren't offered. Record the versions in `android/gradle.properties` comments. Generate the wrapper with the cached Gradle 9.1.0 (`gradle wrapper --gradle-version 9.1.0`, as `spikes/a1/build.cmd` locates it). Builds go to `%USERPROFILE%\build\sorus\android` (`layout.buildDirectory`, as in `spikes/a1`).
- [ ] **Step 2:** Write the instrumented test `SorusNativeTest.fitsBassShelf`:
  - the preset is `{"sorus_preset":1,"eq":[{"type":"low_shelf","freq":100,"gain_db":6}]}`, at 48 kHz with the 64-band layout from Global Constraints;
  - result size 64, band 0 ∈ [5.5, 6.0], band 63 ∈ [−0.1, 0.1];
  - `fitDpBands("{nope", …) == null`, and `lastFitError()` starts with `"invalid json"`.
- [ ] **Step 3:** Run `android\gradlew.bat -p android :native:connectedAndroidTest` with the emulator running. Expected: FAIL (no native method).
- [ ] **Step 4:** Implement:
  - `src/main/cpp/CMakeLists.txt` does `set(SORUS_BUILD_TESTS OFF)` + `add_subdirectory(../../../../core core)`;
  - `sorus_jni.cpp` builds `sorus_jni` linking `sorus_core`;
  - the JNI function copies the arrays and calls `sorus_fit_dp_bands`.
- [ ] **Step 5:** Run the same command. Expected: PASS on x86_64. This is the first time the core runs on an Android ABI (closes the deferred "NDK build" minor). Then build `:native:assembleRelease` to confirm arm64-v8a compiles, including the NEON limiter path (`__aarch64__`).
- [ ] **Step 6:** Commit `feat(android): project skeleton and JNI fit`.

### Task 3: Presets and DP layout (pure Kotlin)

**Files:**
- Create: `app/.../preset/{PresetStore.kt, Graphic.kt, PresetInfo.kt}`, `app/.../eq/DpLayout.kt`, JVM tests

**Interfaces:**
- Produces:
  - `object DpLayout { const val BANDS = 64; fun cutoffs(sampleRate: Int): FloatArray }`: the last cutoff is `sampleRate / 2f`. `const val FRAME_MS = 0f` (0 = platform default; see Global Constraints).
  - `object Graphic { val FREQS = doubleArrayOf(31.25,62.5,125.0,250.0,500.0,1000.0,2000.0,4000.0,8000.0,16000.0); fun toPresetJson(gainsDb: DoubleArray, name: String): String }`: the same output as the Windows `Graphic.ToPresetJson` (bells, Q 1.41, clamp ±12, `auto_preamp` true, limiter on at −1).
  - `data class PresetInfo(val preampDb: Double, val autoPreamp: Boolean, val limiterOn: Boolean, val ceilingDb: Double) { companion object { fun of(json: String): PresetInfo } }`: reads the top-level keys with `org.json`, using the defaults from Global Constraints.
  - `class PresetStore(dir: File)`:
    - `fun active(): String`: the flat default if missing;
    - `fun setActive(json: String): String?`: validates via `SorusNative.fitDpBands` (non-null) and the 64 KiB limit; writes atomically (temp file + rename); returns the error or null; writes nothing on error.
    - `val changes: StateFlow<String>`.
- [ ] **Step 1:** Write the JVM tests:
  - `cutoffs(48000)`: size 64, `[0]` ≈ 22.24 ± 0.01, `[62]` ≈ 20·1000^(63/64), `[63] == 24000f`;
  - `Graphic.toPresetJson` with `gains[0]=13` → `"gain_db":12`, and the JSON has 10 `eq` entries;
  - `PresetInfo.of("{\"sorus_preset\":1}") == PresetInfo(0.0, true, true, -1.0)`;
  - `PresetStore.setActive` with a fake validator (inject `(String) -> String?`) leaves the old file on error.
- [ ] **Step 2:** Run `android\gradlew.bat -p android :app:testDebugUnitTest`. Expected: FAIL.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run the same command. Expected: PASS.
- [ ] **Step 5:** Commit `feat(android): presets and DP layout`.

### Task 4: Session discovery (broadcasts + dumpsys) and coverage

**Files:**
- Create: `app/.../session/{Players.kt, SessionTracker.kt, Coverage.kt}`, `app/.../shizuku/ShizukuShell.kt`, JVM tests with fixtures `src/test/resources/dumpsys_a55.txt`. Take the fixture from a real `adb shell dumpsys audio` on the A55 with YouTube, Spotify and Chrome playing.

**Interfaces:**
- Produces:
  - `data class Player(val uid: Int, val session: Int, val usage: String, val flags: Int, val started: Boolean, val type: String)`;
  - `object Players { fun parse(dump: String): List<Player> }`: the regexes from `spikes/a1/.../Spike.kt` `Dumper`, which match the A55 format, plus `sessionId:-1`;
  - `enum class Reach { Processed, NoSession, ForbidsCapture, TakenOver, AttachFailed }`;
  - `data class AppCoverage(val pkg: String, val label: String, val reach: Reach, val via: Set<Source>)`;
  - `enum class Source { Broadcast, Dumpsys }`;
  - `class SessionTracker(val myUid: Int, val uidToPkg: (Int) -> String?)`:
    - `fun onBroadcastOpen(pkg: String, session: Int)` and `fun onBroadcastClose(session: Int)`;
    - `fun onDump(players: List<Player>?)`, where null means Shizuku is unavailable;
    - `fun sessionsToAttach(): Set<Int>` and `fun sessionsToRelease(): Set<Int>`, the latter after 3 consecutive dump misses, and only for dump-sourced sessions with no open broadcast;
    - `fun coverage(): List<AppCoverage>`;
    - `val shizukuAvailable: Boolean`.
  - `object ShizukuShell { fun available(): Boolean; fun run(cmd: String): String? }`: the reflection `newProcess` approach from `spikes/a1/.../Spike.kt`, returning null on any failure.
  - Eligible usages: `USAGE_MEDIA`, `USAGE_GAME`, `USAGE_UNKNOWN`. Sessions ≤ 0 are never attached; a ≤ 0 session of a started eligible player marks that app `NoSession`.

- [ ] **Step 1:** Write the JVM tests:
  - parsing the fixture: YouTube is an AudioTrack on a positive session; Chrome is AAudio with session −1 → `NoSession`; Spotify is AAudio;
  - a broadcast-only session is attached and released on CLOSE;
  - **one missed dump → not released; three → released**;
  - `onDump(null)` → `shizukuAvailable == false` and broadcast sessions are unaffected;
  - our own uid is ignored;
  - a player with flag `1 shl 10` is still attached (session mode doesn't need capture). The flag only matters to M5, so assert `reach == Processed`.
- [ ] **Step 2:** Run `:app:testDebugUnitTest`. Expected: FAIL.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run it. Expected: PASS.
- [ ] **Step 5:** Commit `feat(android): session discovery and coverage`.

### Task 5: Session EQ (DynamicsProcessing per session)

**Files:**
- Create: `app/.../eq/SessionEq.kt`, `src/androidTest/.../SessionEqTest.kt`

**Interfaces:**
- Consumes: `DpLayout`, `PresetInfo`, `SorusNative.fitDpBands`.
- Produces `class SessionEq(sampleRate: Int)`:
  - `fun attach(session: Int): Result<Unit>`: `DynamicsProcessing(Int.MAX_VALUE, session, config)` with `VARIANT_FAVOR_FREQUENCY_RESOLUTION`, 2 channels, pre-EQ `DpLayout.BANDS`, no MBC, no post-EQ, limiter on, `setPreferredFrameDuration(DpLayout.FRAME_MS)` when > 0;
  - `fun release(session: Int)`;
  - `fun apply(presetJson: String)`, which updates **in place** on every live effect:
    - every band's gain;
    - input gain: `autoPreamp` ? −max(0, max gain) : `preampDb`, clamped to ±24;
    - limiter enabled and threshold = `ceilingDb`;
  - `fun setEnabled(on: Boolean)`;
  - `fun lost(): Set<Int>`: sessions whose control-status listener reported loss;
  - `fun live(): Set<Int>`.

- [ ] **Step 1:** Write the instrumented tests, on the emulator, using the test's own `AudioTrack` session:
  - `attach` → `live()` contains it, and `getPreEqBandByChannelIndex(0, 0).gain` equals the fitted band-0 gain ± 0.01 after `apply(bass shelf +6)`;
  - **10 sessions** (10 AudioTracks): `apply` a new preset → every effect's band 0 is updated within 200 ms, and the effect objects are the same instances (no re-create);
  - `apply` with `auto_preamp` true and a +6 shelf → the input gain ≈ −6 ± 0.3;
  - `release` removes it.
- [ ] **Step 2:** Run `android\gradlew.bat -p android :app:connectedDebugAndroidTest`. Expected: FAIL.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run it. Expected: PASS.
- [ ] **Step 5:** Commit `feat(android): per-session DynamicsProcessing EQ`.

### Task 6: Foreground service + Simple tab

**Files:**
- Create: `app/.../service/SorusService.kt`, `app/.../ui/{MainActivity.kt, SimpleScreen.kt, SimpleViewModel.kt}`, `app/src/main/AndroidManifest.xml`, JVM test `SimpleViewModelTest.kt`

**Interfaces:**
- Consumes: Tasks 3–5.
- Produces:
  - `SorusService` (`foregroundServiceType="specialUse"`, with the subtype property as in `spikes/a1`):
    - owns `SessionTracker` + `SessionEq` + the `PresetStore` subscription;
    - registers the OPEN/CLOSE receiver (`RECEIVER_EXPORTED`);
    - polls `ShizukuShell.run("dumpsys audio")` every 1 s while Shizuku is available;
    - exposes `StateFlow<List<AppCoverage>>` and `StateFlow<Boolean> shizukuAvailable` through a bound `LocalBinder`.
  - The notification shows "Sorus: EQ on N apps".
  - The manifest adds:
    - `QUERY_ALL_PACKAGES`, `MODIFY_AUDIO_SETTINGS`, `FOREGROUND_SERVICE`, `FOREGROUND_SERVICE_SPECIAL_USE`, `POST_NOTIFICATIONS`;
    - the Shizuku provider (as in `spikes/a1`).
  - `SimpleViewModel(store, coverage, shizuku)`:
    - `enabled`;
    - `gains[10]`, debounced 50 ms → `store.setActive(Graphic.toPresetJson(...))`;
    - `importPreset(uri text)` returns an error string or null;
    - `rows: List<AppCoverage>`;
    - `shizukuBanner: String?`, which is "Shizuku not running: only apps that announce their audio are processed" when unavailable.
  - Reason copy:
    - `Processed` → "EQ on";
    - `NoSession` → "Can't be reached (no audio session)";
    - `TakenOver` → "Taken over by another effects app";
    - `AttachFailed` → "Couldn't attach";
    - `ForbidsCapture` is unused in M4.

- [ ] **Step 1:** Write the JVM tests:
  - two slider moves within 50 ms → one `setActive`;
  - import of `"{nope"` → error `"invalid json…"` and the active preset unchanged;
  - `shizuku=false` → the banner text exact;
  - a row for Chrome with `NoSession` maps to the exact copy above.
- [ ] **Step 2:** Run `:app:testDebugUnitTest`. Expected: FAIL.
- [ ] **Step 3:** Implement:
  - the Compose Simple tab: an Enable switch, 10 vertical sliders (−12…+12 dB, labels from `Graphic.FREQS`), "Import preset" (`OpenDocument`, `*/*`, validated), the coverage list, and the Shizuku card with a "How to set up" link to the Shizuku app;
  - edge-to-edge with `WindowInsets.safeDrawing` padding;
  - the service starts on app open and stays in the foreground while enabled.
- [ ] **Step 4:** Run `:app:testDebugUnitTest` (PASS). Then the acceptance run on the A55, driven over adb as in A2:
  1. install;
  2. enable;
  3. play YouTube, Spotify, com.mero and Instagram Reels;
  4. `dumpsys media.audio_flinger` shows one Dynamics Processing effect in each session's chain;
  5. the coverage list shows those four as "EQ on", and Chrome as "Can't be reached (no audio session)".

  Then stop Shizuku: the banner appears, and Spotify (broadcast) is still "EQ on". Record this in the M4 ledger.
- [ ] **Step 5:** Commit `feat(android): service and Simple tab`.

### Task 7: CI + release artifact

**Files:**
- Create: `.github/workflows/android.yml`

- [ ] **Step 1:** The job on `ubuntu-latest`:
  1. `actions/setup-java` (Temurin 21);
  2. the SDK via `android-actions/setup-android`, with `sdkmanager` installing the NDK/CMake versions from Task 2;
  3. `./gradlew -p android :app:testDebugUnitTest :app:assembleRelease`;
  4. upload the APK.

  For v0, release signing uses the debug key (GitHub Releases signing is M9). Instrumented tests stay local (an emulator in CI is slow and flaky).
- [ ] **Step 2:** Push the branch. Expected: green; the APK artifact is downloadable. The arm64 core build now runs in CI (closes the deferred "NDK/arm64 CI job" minor).
- [ ] **Step 3:** Commit `ci(android): unit tests and APK artifact`.
