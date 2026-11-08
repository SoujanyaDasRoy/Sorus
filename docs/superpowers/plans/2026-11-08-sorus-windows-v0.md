# Sorus Windows v0 (M3) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** An installable Windows build where the shared core engine processes all audio on the chosen output devices,
controlled by a minimal app (enable/disable, 10-band graphic EQ, load `.sorus` preset, per-device status).

**Architecture:**
- **APO:** an unsigned, aggregatable Audio Processing Object (C++ COM DLL linking `sorus_core`) appended to each endpoint's composite mode-effect list (W1).
- **Settings channel:** a `Global\SorusSettings` seqlock block created by the APO, with a named event (W2), plus a persisted `%ProgramData%\Sorus\active.sorus` that the APO loads on its own at creation (W2 finding 2).
- **Apps:** a C# class library holds all logic (presets, settings client, endpoint setup), shared by a WinUI 3 app and a console tool `sorusctl` that the MSI installer and the end-to-end test drive.

**Tech Stack:** C++20 / MSVC / CMake ≥ 3.25 / doctest 2.4.11; .NET 9 (C#), Windows App SDK (WinUI 3, unpackaged,
self-contained) with Native AOT; xUnit; WiX Toolset v5 (`wix` .NET tool, MS-RL licence, free); GitHub Actions.

**Spec:** `docs/superpowers/specs/2026-10-30-sorus-design.md` (§6.2, §7.6, §8, §9, §10, §11). Spike results it builds on:
`docs/spikes/W1.md`, `docs/spikes/W2.md`.

## Global Constraints

- Windows 10 22H2 and Windows 11, x64 only.
- Everything free: no paid tools, certificates or services. The MSI is unsigned in v0 (SignPath comes in M9).
- APO real-time path: no allocation, no locks, no syscalls, no logging (spec §8). Control work happens on a worker thread.
- Install/uninstall needs admin once, and **restores each endpoint's original effect registration** (spec §6.2).
- "APO not loaded" must be detected and shown to the user (spec §5, anti-goal 1; §6.2).
- WASAPI exclusive-mode streams bypass APOs. The status view says so in one line (spec §6.2).
- Per-app profiles on Windows are out of v1 (spec §6.2).
- Licence GPL-3.0. Build output outside OneDrive (`%USERPROFILE%\build\sorus\...`), as for the core.
- Preset format: `.sorus` JSON v1, as parsed by `core/src/preset.cpp`. No new fields in v0.

## Review Focus

1. **Endpoint without composite FX lists** (older or legacy drivers). Install must skip it with status "unsupported driver (no composite effects)" and never touch its legacy single-CLSID properties. Test in Task 4.
2. **Install run twice, or uninstall without install.** Install must be idempotent, never double-appending the CLSID or overwriting the first backup. Uninstall with no backup is a no-op that exits 0. Test in Task 4.
3. **The UI writes while the APO worker reads.** A torn preset must never be applied: the seqlock reader retries. Test in Task 2.
4. **Audio service restarts while the app is open.** The new audiodg must apply the persisted preset before the app reconnects, and the app must reconnect without user action. Test in Task 3 (persisted load) and Task 7 (e2e restart step).
5. **Malformed or oversize preset** (bad JSON, more than 64 KiB, 33 bands). The app shows the core's error text, and nothing is written to the block or the file. Test in Task 5.

---

## File Structure

```
core/include/sorus/sorus.h           + sorus_set_enabled()
core/src/sorus_c_api.cpp             + sorus_set_enabled
core/CMakeLists.txt                  + sorus_shared (sorus.dll) target
core/tests/test_c_api.cpp            + enabled test
windows/
  CMakeLists.txt                     builds apo + its tests; add_subdirectory(../core)
  common/sorus_ipc.h                 settings block layout (C++), names, SDDL - mirrored in C# Ipc.cs
  apo/settings_block.{h,cpp}         create/map block + event, seqlock read/write, loaded-endpoint table
  apo/engine_set.{h,cpp}             process-wide registry of live engines + worker thread applying settings
  apo/sorus_apo.cpp                  COM APO (aggregation, IAudioProcessingObject*, IAudioSystemEffects)
  apo/sorus_apo.def                  DllGetClassObject, DllCanUnloadNow
  apo/tests/test_*.cpp               doctest: block, engine set, COM object with fake IAudioMediaType
  app/Sorus.Windows.sln
  app/Sorus.Core/                    C# lib: Ipc.cs, SettingsClient.cs, Graphic.cs, PresetValidator.cs (P/Invoke),
                                     Endpoints.cs, EndpointSetup.cs, Paths.cs
  app/Sorus.Core.Tests/              xUnit
  app/SorusCtl/                      console: setup install|uninstall, apply <file>, enable|disable, status
  app/Sorus.App/                     WinUI 3: Simple tab
  installer/Sorus.wxs                WiX v5 MSI
  tools/loopback-meter.cpp           from spikes/w1/meter.cpp (endpoint id arg, prints RMS dBFS)
  tests/e2e.ps1                      admin end-to-end acceptance script
scripts/windows.cmd                  build helper (like scripts/core.cmd)
.github/workflows/windows.yml        build + unit tests + MSI artifact
```

---

### Task 1: Core: enable switch in the C API, and `sorus.dll`

**Files:**
- Modify: `core/include/sorus/sorus.h`, `core/src/sorus_c_api.cpp`, `core/CMakeLists.txt`, `core/tests/test_c_api.cpp`

**Interfaces:**
- Produces:
  - `SORUS_API int sorus_set_enabled(SorusEngine* e, int enabled);` returns 0, or −1 on NULL. Control thread. Glides like `Engine::set_bypass` (10 ms), bypassing both `Block::Eq` and `Block::Limiter`. Latency is unchanged (spec: bypassed blocks cost nothing).
  - CMake target `sorus_shared` → `sorus.dll` with `SORUS_API=__declspec(dllexport)` (consumers define `__declspec(dllimport)`), linking `sorus_core`.

- [ ] **Step 1:** Write test `TEST_CASE("c api enable switch")`:
  - create a 48 kHz stereo engine and load `{"sorus_preset":1,"preamp_db":-6,"auto_preamp":false,"limiter":{"enabled":false}}`;
  - process 4800 frames of 0.5 constant: output ≈ 0.5 × 10^(−6/20) ± 1e-3;
  - `sorus_set_enabled(e, 0)`, then process 4800 frames: the last frame ≈ 0.5 ± 1e-4;
  - `sorus_set_enabled(nullptr, 1) == -1`.
- [ ] **Step 2:** Run `cmd /c "scripts\core.cmd -R \"enable switch\""`. Expected: compile error, `sorus_set_enabled` undeclared.
- [ ] **Step 3:** Implement it in `sorus_c_api.cpp` (two `set_bypass` calls). Document it in `sorus.h` next to `sorus_flush`. Add the `sorus_shared` target.
- [ ] **Step 4:** Run `cmd /c "scripts\core.cmd"`. Expected: all tests pass (67), and `sorus.dll` exists in the build directory.
- [ ] **Step 5:** Commit `feat(core): sorus_set_enabled and sorus.dll`.

### Task 2: Settings block (shared memory + event + seqlock)

**Files:**
- Create: `windows/common/sorus_ipc.h`, `windows/apo/settings_block.{h,cpp}`, `windows/apo/tests/test_settings_block.cpp`, `windows/CMakeLists.txt`, `scripts/windows.cmd`

**Interfaces:**
- Produces (`sorus_ipc.h`, the layout mirrored byte-for-byte in C# Task 5):
  ```cpp
  constexpr uint32_t kSorusIpcMagic = 0x31535253;      // "SRS1"
  constexpr wchar_t kSorusIpcBlock[] = L"Global\\SorusSettings";
  constexpr wchar_t kSorusIpcEvent[] = L"Global\\SorusSettingsChanged";
  constexpr wchar_t kSorusIpcSddl[]  = L"D:(A;;GA;;;SY)(A;;GA;;;LS)(A;;GA;;;BA)(A;;GRGW;;;IU)";  // W2
  constexpr int kSorusIpcJsonMax = 65536, kSorusIpcEndpoints = 16;
  struct SorusIpcEndpoint { wchar_t id[64]; int64_t last_qpc; };   // APO-written: endpoint seen processing
  struct SorusIpcBlock {
    uint32_t magic, version;            // version = 1
    volatile int32_t generation;        // set by the creating APO process (GetTickCount64 low 31 bits, never 0)
    volatile int32_t seq;               // seqlock: odd while the writer is mid-write
    int32_t enabled;                    // 0/1
    int32_t json_len;
    char json[kSorusIpcJsonMax];
    volatile int32_t applied_seq;       // APO worker: last seq applied
    char last_error[256];               // APO worker: sorus_last_error of the last apply ("" on success)
    SorusIpcEndpoint endpoints[kSorusIpcEndpoints];
  };
  ```
- `class SettingsBlock` (namespace `sorus::win`):
  - `static std::unique_ptr<SettingsBlock> create(const wchar_t* block_name, const wchar_t* event_name);` creates or opens with the SDDL; null on failure.
  - `bool created() const;` true if this call created it.
  - `SorusIpcBlock* get();`
  - `HANDLE event() const;`
  - `struct Snapshot { int32_t seq; bool enabled; std::string json; };`
  - `bool read(Snapshot* out) const;` seqlock read: retries while seq is odd or changes; gives up after 1000 tries.
  - `void write(bool enabled, std::string_view json);` test/tool writer: seq++ → write → seq++ → SetEvent.
  - `void mark_endpoint(const wchar_t* id, int64_t qpc);` updates the matching slot or takes a free one; ignores when full.
- Tests use `Local\SorusTest_<pid>` names, since standard users can't create `Global\` objects (W2).

- [ ] **Step 1:** Write the tests:
  - `create` twice → `created()` true, then false;
  - round-trip write/read;
  - **torn-write test:** a writer thread alternates two 40 KiB payloads ("A…A" / "B…B") 10 000 times while the reader checks that every snapshot is uniform;
  - `mark_endpoint` reuses its slot and ignores the 17th id;
  - JSON longer than `kSorusIpcJsonMax` is rejected by `write` (returns without bumping seq).
- [ ] **Step 2:** Run `cmd /c "scripts\windows.cmd -R settings"`. Expected: FAIL (undefined `SettingsBlock`).
- [ ] **Step 3:** Implement `settings_block.cpp`. Use `ConvertStringSecurityDescriptorToSecurityDescriptorW` for both the mapping and the event (auto-reset). `scripts/windows.cmd` mirrors `scripts/core.cmd` and builds into `%USERPROFILE%\build\sorus\windows`.
- [ ] **Step 4:** Run `cmd /c "scripts\windows.cmd"`. Expected: PASS.
- [ ] **Step 5:** Commit `feat(windows): settings block with seqlock and event`.

### Task 3: Engine set and worker (APO control side)

**Files:**
- Create: `windows/apo/engine_set.{h,cpp}`, `windows/apo/tests/test_engine_set.cpp`

**Interfaces:**
- Consumes: `SettingsBlock` (Task 2); `sorus_*` C API incl. `sorus_set_enabled` (Task 1).
- Produces `class EngineSet` (namespace `sorus::win`), one per process via `EngineSet& EngineSet::instance()`:
  - `void attach(SorusEngine* e);` applies the current snapshot to `e` under the set's mutex, then registers it.
  - `void detach(SorusEngine* e);`
  - `void start(const wchar_t* block_name, const wchar_t* event_name, std::wstring persisted_path);` idempotent. Creates or opens the block. If this process **created** it, loads `persisted_path` (when present and ≤ 64 KiB) into the block as `enabled=1, json=<file>`. Starts the worker: wait on the event (1 s timeout) → `read` → if `seq` changed: `sorus_load_preset` + `sorus_set_enabled` + `sorus_flush` for every attached engine. Then it writes `applied_seq` and `last_error`.
  - `void stop_for_tests();` joins the worker.
- Persisted path: `%ProgramData%\Sorus\active.sorus` (`Paths.Active` in C#).

- [ ] **Step 1:** Write the tests:
  - **persisted load:** write a file with `preamp_db −6`, then `start` with fresh `Local\` names → `applied_seq` becomes non-zero within 2 s and `block.json` equals the file;
  - **live apply:** an attached 48 kHz engine; `write(true, -6 dB preset)` → within 100 ms `applied_seq == seq`, and processing 0.5 gives about 0.501 × 0.5;
  - **bad JSON:** `last_error` starts with "invalid json", and the engine keeps its previous preset;
  - **disabled:** `write(false, …)` → output equals input after the 10 ms glide.
- [ ] **Step 2:** Run `cmd /c "scripts\windows.cmd -R engine_set"`. Expected: FAIL.
- [ ] **Step 3:** Implement. The worker is the only caller of control functions on attached engines; `attach`, `detach` and the worker take the same `std::mutex`.
- [ ] **Step 4:** Run `cmd /c "scripts\windows.cmd"`. Expected: PASS.
- [ ] **Step 5:** Commit `feat(windows): APO engine set and settings worker`.

### Task 4: COM APO

**Files:**
- Create: `windows/apo/sorus_apo.cpp`, `windows/apo/sorus_apo.def`, `windows/apo/tests/test_apo.cpp`
- Reference: `spikes/w2/apo.cpp` (aggregation pattern, format negotiation), `spikes/w2/selftest.cpp`

**Interfaces:**
- Consumes: `EngineSet` (Task 3).
- Produces:
  - `sorus_apo.dll` with CLSID `{469205DD-A208-4C1A-A434-D3514B371236}` (reuse W2's), friendly name "Sorus", `APO_FLAG_DEFAULT`, `APOInterface0 = IID_IAudioProcessingObject`, float32 only.
  - Exports `DllGetClassObject` and `DllCanUnloadNow`.

- [ ] **Step 1:** Write the tests, in a doctest that loads the DLL with `LoadLibraryW`, using a test-only fake `IAudioMediaType` returning float32 / 48 kHz / 2 ch:
  - an aggregated create with `IID_IUnknown` succeeds, and a non-`IUnknown` IID gives `CLASS_E_NOAGGREGATION`;
  - QI through the delegating `IUnknown` returns the outer;
  - `LockForProcess` with 480-frame descriptors, then `APOProcess` of 0.5 with the Task 3 test block set to −6 dB → 0.2506 ± 1e-3;
  - `BUFFER_SILENT` input → silent output flag, with the valid frame count copied;
  - `UnlockForProcess`, then a final `Release` → `DllCanUnloadNow() == S_OK`.

  The test DLL build uses `SORUS_APO_TEST_NAMES` so the APO uses `Local\` names.
- [ ] **Step 2:** Run `cmd /c "scripts\windows.cmd -R apo"`. Expected: FAIL.
- [ ] **Step 3:** Implement:
  - `Initialize`: parse `APOInitSystemEffects2/3` when `cbDataSize` matches, to get the endpoint id via `pDeviceCollection` → `IMMDevice::GetId`. Store it; empty if unknown.
  - `LockForProcess`: `sorus_create(rate, u32MaxFrameCount, channels)` → `EngineSet::instance().start(...)` → `attach`.
  - `APOProcess`: copy input→output, then `sorus_process` in place on the output. Call `mark_endpoint(id, QPC)` at most once per second (a plain store plus `QueryPerformanceCounter`; no locks).
  - `UnlockForProcess`: `detach` + `sorus_destroy`.
- [ ] **Step 4:** Run `cmd /c "scripts\windows.cmd"`. Expected: PASS.
- [ ] **Step 5:** Commit `feat(windows): Sorus APO`.

### Task 5: C# core library: IPC client, graphic EQ, validation, paths

**Files:**
- Create:
  - `windows/app/Sorus.Windows.sln`
  - `windows/app/Sorus.Core/{Sorus.Core.csproj, Ipc.cs, SettingsClient.cs, Graphic.cs, PresetValidator.cs, Paths.cs}`
  - `windows/app/Sorus.Core.Tests/{Sorus.Core.Tests.csproj, *Tests.cs}`

**Interfaces:**
- Consumes: `sorus.dll` (Task 1); the block layout (Task 2).
- Produces (namespace `Sorus.Core`; `net9.0-windows10.0.19041.0`, `IsAotCompatible=true`, `AllowUnsafeBlocks=true`):
  - `static class Paths { string Active; string ProgramDataDir; }`: `%ProgramData%\Sorus\active.sorus`.
  - `static class Graphic { static readonly double[] Freqs = {31.25,62.5,125,250,500,1000,2000,4000,8000,16000}; static string ToPresetJson(double[] gainsDb, string name); }`:
    - bell bands at `Freqs`, Q 1.41, gains clamped to ±12;
    - `auto_preamp` true, limiter on at −1 dBTP;
    - exactly the key names `core/src/preset.cpp` parses.
  - `static class PresetValidator { static string? Validate(string json); }`: null if valid, else the core's error text. It P/Invokes `sorus_create(48000,512,2)` + `sorus_load_preset` + `sorus_last_error` + `sorus_destroy`. Use `LibraryImport`, and reject over 64 KiB before calling.
  - `sealed class SettingsClient : IDisposable`:
    - `static SettingsClient? TryOpen(string blockName = @"Global\SorusSettings", string eventName = @"Global\SorusSettingsChanged")`;
    - `void Write(bool enabled, string json)`: seqlock write mirroring Task 2, then signal the event;
    - `(int seq, int appliedSeq, string lastError, int generation) Status()`;
    - `IReadOnlyList<(string EndpointId, DateTime LastSeenUtc)> Endpoints()`.
  - `static class Settings { static string? Apply(bool enabled, string json); }`:
    - validates, then writes `Paths.Active`, then writes the block if it's open;
    - returns the error text or null;
    - writes nothing when validation fails.

- [ ] **Step 1:** Write the xUnit tests:
  - `Graphic.ToPresetJson` (all-zero gains) is accepted by `PresetValidator`, and +13 clamps to 12;
  - `PresetValidator` returns a string starting `"invalid json"` for `"{nope"`, `"too many bands (33 > 32)"` for 33 bands, and non-null for 70 KiB of JSON;
  - `SettingsClient` round-trip against a block created by a **test helper that P/Invokes `CreateFileMappingW` with a `Local\` name** and the struct size from `Marshal.SizeOf`;
  - the size equals the C++ `sizeof(SorusIpcBlock)`. Hard-code the value printed by a Task 2 doctest `CHECK(sizeof(SorusIpcBlock) == N)`; both tests pin it;
  - `Settings.Apply` with bad JSON leaves `Paths.Active` untouched (override the path through an internal setter for tests).
- [ ] **Step 2:** Run `dotnet test windows/app/Sorus.Core.Tests`. Expected: FAIL (types missing).
- [ ] **Step 3:** Implement. Copy `sorus.dll` next to the test assembly via the csproj (`None Include=…sorus.dll CopyToOutputDirectory`).
- [ ] **Step 4:** Run `dotnet test windows/app/Sorus.Core.Tests`. Expected: PASS.
- [ ] **Step 5:** Commit `feat(windows): C# settings client, graphic EQ and validator`.

### Task 6: Endpoint setup and `sorusctl`

**Files:**
- Create: `windows/app/Sorus.Core/{Endpoints.cs, EndpointSetup.cs}`, tests in `Sorus.Core.Tests/EndpointSetupTests.cs`, `windows/app/SorusCtl/{SorusCtl.csproj, Program.cs}`

**Interfaces:**
- Consumes: Task 5.
- Produces:
  - `record Endpoint(string Id, string Name, bool Active, bool HasComposite)`;
  - `static class Endpoints { static IReadOnlyList<Endpoint> Render(RegistryKey hklm); }`: reads `MMDevices\Audio\Render\*`. The name comes from `{a45c254e-…},2` / `{b3f8fa53-…},6`, active means `DeviceState == 1`, and `HasComposite` means the mode-effect list `{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},14` exists.
  - `static class EndpointSetup`:
    - `InstallResult Install(RegistryKey hklm, string apoDllPath, IEnumerable<string>? endpointIds = null)`. The default is every active endpoint with composite lists. It writes:
      - the COM + `AudioEngine\AudioProcessingObjects` keys (values as in `spikes/w2/install.ps1`);
      - for each endpoint, a backup under `SOFTWARE\Sorus\Backup\<id>` (`Original` multi-string, or `Absent=1`), only if no backup exists;
      - the CLSID appended to list 14 only if missing. `FxProperties` is opened with **exactly** `SetValue|QueryValues` (W1).
    - `void Uninstall(RegistryKey hklm)`: restores each backup, deletes the keys, and is a no-op without backups.
    - `record InstallResult(IReadOnlyList<string> Installed, IReadOnlyList<(string Id, string Reason)> Skipped)`;
    - the audio service restart (`System.ServiceProcess.ServiceController`, `audiosrv`) is a separate `RestartAudio()`, so tests never call it.
  - `sorusctl` commands:
    - `setup install [--dll <path>]` / `setup uninstall`: admin, restarts audio;
    - `apply <file.sorus>` and `enable` / `disable`: via `Settings.Apply`, re-using the current JSON;
    - `status`: per endpoint, prints name, registered, and "loaded" when `Endpoints()` saw it within the last 5 s; plus generation, applied_seq and last_error.
    - Exit codes: 0 ok, 1 usage, 2 validation error, 3 needs admin.

- [ ] **Step 1:** Write the xUnit tests against a scratch root `HKCU\Software\SorusTest\<guid>`, seeded with two fake endpoints (one with list 14 `["{V1}"]`, one without):
  - `Install` → the first list is `["{V1}","{469205DD-…}"]`, and the second is skipped with reason `"unsupported driver (no composite effects)"`, its keys untouched;
  - **install twice** → no duplicate CLSID, and the backup still `["{V1}"]`;
  - `Uninstall` → the list is back to `["{V1}"]` and the Sorus keys are gone;
  - **uninstall again** → no throw.
- [ ] **Step 2:** Run `dotnet test windows/app/Sorus.Core.Tests`. Expected: FAIL.
- [ ] **Step 3:** Implement. Publish `SorusCtl` with `PublishAot=true` and confirm there are no AOT warnings.
- [ ] **Step 4:** Run `dotnet test …` (PASS), then `dotnet publish windows/app/SorusCtl -c Release -r win-x64`. Expected: `sorusctl.exe`, and `sorusctl status` runs as a normal user.
- [ ] **Step 5:** Commit `feat(windows): endpoint setup and sorusctl`.

### Task 7: Installer and end-to-end acceptance

**Files:**
- Create: `windows/installer/Sorus.wxs`, `windows/tools/loopback-meter.cpp` (add it to `windows/CMakeLists.txt`), `windows/tests/e2e.ps1`

**Interfaces:**
- Consumes: `sorus_apo.dll`, `sorus.dll`, `sorusctl.exe`, `Sorus.App.exe` (Task 8 can land after; the MSI includes it once it exists).
- Produces `Sorus-<version>-x64.msi`:
  - per-machine, into `ProgramFiles64Folder\Sorus`;
  - creates `CommonAppDataFolder\Sorus` with Users = Modify;
  - deferred, non-impersonated custom actions run `sorusctl setup install` after `InstallFiles` and `sorusctl setup uninstall` before `RemoveFiles`.
  - Install the tool with `dotnet tool install --global wix --version 5.*`.

- [ ] **Step 1:** Write `e2e.ps1` (`#Requires -RunAsAdministrator`, with the endpoint id as a parameter). It asserts, printing PASS/FAIL per check:
  1. MSI install exits 0, and `sorusctl status` shows the endpoint registered.
  2. `apply` a `preamp_db −6, auto_preamp false, limiter off` preset → the loopback level changes by −6.0 ± 0.1 dB against the reading taken just before (back-to-back, W1 lesson), and the endpoint shows "loaded".
  3. `disable` → back to the reference ± 0.1 dB.
  4. `Restart-Service audiosrv`, then play the meter tone **before** any `sorusctl` call → the level is still −6 (persisted load, W2 finding 2).
  5. MSI uninstall → list 14 equals the value recorded before install, `Program Files\Sorus` is gone, and the level is back to the reference.
- [ ] **Step 2:** Run it before the MSI exists. Expected: FAIL at check 1.
- [ ] **Step 3:** Write `Sorus.wxs` and the meter. Build with `wix build windows/installer/Sorus.wxs -arch x64 -o %USERPROFILE%\build\sorus\windows\Sorus.msi`.
- [ ] **Step 4:** The owner runs `e2e.ps1` in an admin PowerShell. Expected: all five PASS. Record the output in the M3 ledger.
- [ ] **Step 5:** Commit `feat(windows): MSI installer and e2e acceptance script`.

### Task 8: WinUI 3 app: Simple tab

**Files:**
- Create: `windows/app/Sorus.App/` (WinUI 3 unpackaged: `WindowsPackageType=None`, `WindowsAppSDKSelfContained=true`, `PublishAot=true`), with `MainWindow.xaml(.cs)`, `SimpleViewModel.cs`, and tests for the view-model in `Sorus.Core.Tests/SimpleViewModelTests.cs`.

**Interfaces:**
- Consumes: Tasks 5–6.
- Produces `SimpleViewModel`, a plain C# class with no WinUI types so it is testable:
  - `bool Enabled`;
  - `double[] Gains` (10);
  - `string? Error`;
  - `IReadOnlyList<EndpointRow> Devices`;
  - `void SetGain(int band, double db)`, debounced 50 ms before `Settings.Apply`;
  - `string? LoadPreset(string path)`: validates, then applies; on success sets `Gains` to zero and `PresetName`; on failure sets `Error` and leaves everything else as is;
  - `void Refresh()`: endpoints + status, polled every 1 s.
  - `record EndpointRow(string Name, string State)`, where `State` is one of "Processing", "Installed — no audio yet", "APO not loaded", "Not installed", "Unsupported driver".

  The view shows one line under the list: "Apps using exclusive mode bypass Sorus."

- [ ] **Step 1:** Write the view-model tests:
  - `SetGain(0, 6)` twice within 50 ms → one `Apply` (inject a fake `Func<bool,string,string?>`);
  - `LoadPreset` of a bad file → `Error` set and `Gains` unchanged;
  - an endpoint registered but not seen for 5 s while the block's generation exists → "APO not loaded";
  - the same with no block at all → "Installed — no audio yet".
- [ ] **Step 2:** Run `dotnet test`. Expected: FAIL.
- [ ] **Step 3:** Implement the view-model, then the XAML: an Enable toggle, 10 vertical sliders (−12…+12, labelled with `Graphic.Freqs`), a "Load preset…" button (`FileOpenPicker`, `.sorus`), an error InfoBar, and the device list. Single instance via a named mutex. Every apply goes through `Settings.Apply`; no file or registry writes elsewhere.
- [ ] **Step 4:** Run `dotnet test` (PASS) and `dotnet publish windows/app/Sorus.App -c Release -r win-x64`. Then a manual check: launch, move a slider, and `sorusctl status` shows `applied_seq` advancing.
- [ ] **Step 5:** Commit `feat(windows): WinUI Simple tab`.

### Task 9: CI

**Files:**
- Create: `.github/workflows/windows.yml`

- [ ] **Step 1:** The job on `windows-latest`:
  1. configure and build `windows/` with CMake;
  2. run `ctest` for the apo tests (they use `Local\` names, so no admin is needed);
  3. `dotnet test windows/app/Sorus.Core.Tests`;
  4. `dotnet publish` both apps (AOT);
  5. `dotnet tool install --global wix --version 5.*`;
  6. `wix build`;
  7. upload `Sorus.msi` as an artifact.

  `e2e.ps1` is not run in CI (it needs a real audio device and admin).
- [ ] **Step 2:** Push the branch. Expected: green; the MSI artifact is downloadable.
- [ ] **Step 3:** Commit `ci(windows): build, test, MSI artifact`.
