# Sorus Roadmap

**Spec:** `docs/superpowers/specs/2026-10-30-sorus-design.md`

The spec covers several independent subsystems (shared core, Windows APO + app, Android session mode, Android full mode,
effects, distribution). Each gets **its own detailed implementation plan**, written when its milestone starts — later
milestones depend on spike results, so planning them in detail now would be guessing.

Detailed plan available now: **M2 Core engine v1** → `docs/superpowers/plans/2026-10-30-sorus-core-engine.md`.

Builders: the owner + Claude. Platforms advance milestone by milestone on the shared core.

---

## Order

| # | Milestone | Depends on | Output | Plan |
|---|---|---|---|---|
| M0 | Setup | — | Git repo, toolchain verified, public GitHub repo | in core plan, Task 0 |
| M1a | Go/no-go spikes **W1**, **A1** | M0 | `docs/spikes/W1.md`, `docs/spikes/A1.md` with verdict | this file, below |
| M2 | Core engine v1 (EQ + preamp + limiter + presets + C API + bench) | M0 | `core/` static lib, tests green, bench under budget | **written** |
| M1b | Spikes **W2**, **A2**, **A3**, **A4** | M2 (A3 needs the fitter) | `docs/spikes/*.md` | this file, below |
| M3 | Windows v0: APO host + installer + minimal WinUI 3 app (load preset, Simple tab graphic EQ, enable/disable) | M2, W1, W2 | Installable `.msi`/`.exe`, EQ works system-wide | write at start |
| M4 | Android v0: session mode (foreground service, session broadcasts, DynamicsProcessing fitted EQ, Simple tab) | M2, A2, A3 | Installable APK, EQ applies to announcing apps | write at start |
| M5 | Android full mode (Shizuku setup wizard, playback capture → core → Oboe, per-app coverage list) | M4, A1 pass, A4 | Full engine on Android | write at start |
| M6 | Pro EQ v2: dynamic EQ, linear phase, analyzer feed, M/S UI, snapshots/undo, A/B gain-matched bypass, SIMD (NEON / SSE4.1+AVX2 dispatch) | M2 | Core + Pro tab on both platforms | write at start |
| M7 | Effects: compressor/multiband, bass engine, saturation, stereo/crossfeed, HRTF (libmysofa), convolution, LUFS normalization, dialogue, night mode, speaker protection | M6 | Movie + Music modes | write at start |
| M8 | Smart features: headphone correction DB + targets, ISO 226 loudness comp, hearing profile, device/app profiles + rules, import/export (EqualizerAPO, REW, GraphicEQ), tray/QS tile/widget, meters, exposure tracking | M3, M4 | Feature-complete v1 | write at start |
| M9 | Distribution: GitHub Releases CI, F-Droid metadata, SignPath signing, Microsoft Store (if free) | M3, M4 | Public v1 release | write at start |

If **A1 fails**: skip M5, ship Android on session mode (owner decision), keep researching full mode.
If **W1 fails**: stop and revisit the Windows design with the owner before M3 (only free options are acceptable).

---

## Spike protocols

Spikes are throwaway code in `spikes/<id>/` (never imported by product code). No TDD. Each ends with a written
result in `docs/spikes/<id>.md`: what was tried, devices/OS versions, measurements, **PASS/FAIL**, and what the product
plan should change.

### W1 — Unsigned APO loads system-wide (go/no-go)
1. Read Equalizer APO's source (GPL-2.0-or-later, licence-compatible) for how it registers its APO on an endpoint
   (`HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\{id}\FxProperties`) and restarts `audiosrv`.
2. Build a minimal C++ COM APO that applies a fixed −6 dB gain. Register it as the mix/endpoint effect on the default render device.
3. Verify with a WASAPI loopback recording of a 1 kHz tone: level drops by 6.0 ± 0.1 dB with the APO on, back with it off.
4. Repeat on: Windows 10 22H2 and Windows 11 (latest), default security settings; Realtek onboard, a USB DAC, Bluetooth headphones.
5. **PASS:** works unsigned on Win10 + Win11 with at least onboard + USB. Record every device where it did not load and why.

### A1 — Full mode without root (go/no-go)
1. Read RootlessJamesDSP's source (GPL-3.0, licence-compatible) to learn how it captures and how it silences the original audio.
2. Throwaway app: Shizuku API → grant `PROJECT_MEDIA` + `DUMP`; `AudioPlaybackCapture` of `USAGE_MEDIA`/`USAGE_GAME`/`USAGE_UNKNOWN`;
   play the captured audio back through Oboe with a fixed +6 dB bass shelf so processing is audible.
3. Prove the original stream is silenced while the processed one plays.
4. Shizuku must be started on-phone via wireless debugging (Android 11+) — no PC.
5. Test Spotify, YouTube, YouTube Music, a local player, one game; on the owner's phone + at least one other Android 13+ device or emulator.
6. **PASS:** processed-only audio for all capturable test apps, no root, no PC, survives screen-off for 10 minutes.

### W2 — UI ↔ APO settings path
1. In the W1 APO, open a named shared-memory section created by a normal user-mode app, with an SDDL that grants
   read access to the account `audiodg.exe` runs as.
2. The app writes a gain value; the APO applies it.
3. **PASS:** change audible/measurable within 10 ms; works after audio service restart and after user logoff/logon.

### A2 — Session-mode coverage
1. Throwaway app with a dynamically registered receiver for `ACTION_OPEN_AUDIO_EFFECT_CONTROL_SESSION` /
   `ACTION_CLOSE_AUDIO_EFFECT_CONTROL_SESSION`, running in a foreground service. Log package + session id.
2. With Shizuku `DUMP`, also list active sessions from `dumpsys media.audio_flinger` and attach `DynamicsProcessing` to them.
3. Test the top 20 music/video apps available to the owner.
4. Output: table app × (broadcasts? / attach via dumpsys works? / effect audible?). No pass/fail — sets expectations and UI copy.

### A3 — DynamicsProcessing fit accuracy (after core Task 9)
1. Desktop: for 20 reference presets, compare the core's parametric response vs `fit_to_dp_bands` output treated as
   piecewise-flat bands, at 32 / 64 / 128 DP bands, 20 Hz–20 kHz.
2. On device: play a log sweep through a session with the fitted DP config, capture, compare to target.
3. **PASS:** max error ≤ 1 dB at 64 bands on device. Record the band count to ship.

### A4 — Full-mode latency (after A1 pass)
1. Play an A/V sync test video in YouTube through full mode; film the screen + listen with a second device; measure offset.
2. Also measure via Oboe timestamps in the spike app (capture timestamp → output presentation timestamp).
3. **PASS:** added latency ≤ 40 ms on a mid-range phone. If not, record the lowest buffer settings that stay dropout-free.
