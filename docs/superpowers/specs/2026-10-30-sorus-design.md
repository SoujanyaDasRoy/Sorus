# Sorus — Design

Date: 2026-10-30
Status: approved 2026-10-30

## 1. Intent

Sorus is a free, open-source app that takes over the sound of a whole device — every song, video and movie —
and runs it through a professional-grade audio chain: preamp, a very advanced EQ, dynamics, bass, stereo/spatial
effects and a safety limiter. It ships for **Android** and **Windows**, built in parallel, sharing one DSP engine.

Why: phone and PC audio is tuned by the device maker for nobody in particular. Existing tools are either locked
behind root/paywalls, platform-specific, or amateur-grade. Sorus gives regular users pro-level control with nothing locked.

## 2. Decisions made by the owner

- **Audience:** regular users. On Android: **no root**, and no PC needed for any setup.
- **Nothing locked:** no paid tiers, no hidden "advanced" switch, no feature reserved for root.
  Give non-root users as much as is technically possible.
- **Everything free:** free for users (no ads, no subscriptions) and nothing in the build or distribution that costs money.
- **Open source.**
- **Both platforms at the same time**, Android and Windows.
- **Native performance** on both platforms, UI included. Must be very efficient and fast.
- **EQ must be really advanced and professional.**
- Songs and movies are both first-class use cases.
- **Builders:** the owner working with Claude (no wider team). "Parallel" means both platforms advance in the same
  milestones on the shared core, not separate teams.

## 3. Goals

1. System-wide processing on Android (non-root) and Windows, covering as many apps as the OS allows.
2. A professional parametric EQ (spec in §6) usable by beginners through a simple view.
3. Movie features (dialogue, night mode, virtual surround) and music features (correction, convolution, saturation).
4. Same engine, same presets on both platforms. A preset made on the phone loads on the PC unchanged.
5. Tight performance budget (§8), enforced by benchmarks on every change.

## 4. Non-goals

- Root / Magisk module (may come later; not in this design).
- Custom Windows virtual audio driver (needs a paid signing certificate — ruled out by "everything free").
- iOS, macOS, Linux.
- Accounts, cloud sync, ads, analytics, telemetry. *(My call — consistent with free + open source.)*
- Medical claims for the hearing profile.

## 5. Anti-goals (failure even if it "works")

- A feature silently not applying to an app, with the user not told.
- Any clipping or distortion the user did not ask for.
- Audio dropouts, clicks when moving EQ bands, or the UI ever stalling audio.
- Noticeable battery drain or heat.
- Anything locked, paywalled, or nagging.

## 6. How audio gets in

### 6.1 Android — the honest constraint

**Correction to what was said in chat:** non-root apps cannot inject their *own* DSP code into another app's audio
session. In session mode, Sorus can only drive the effects Android itself provides. So the two Android modes differ in
*features*, not just app coverage. This is Android's limit, not a lock — Sorus explains it in the UI.

| | **Session mode** (no setup) | **Full mode** (one-time Shizuku setup on-phone) |
|---|---|---|
| Mechanism | Listen for `ACTION_OPEN_AUDIO_EFFECT_CONTROL_SESSION` broadcasts; attach platform `DynamicsProcessing`, `Virtualizer`, `BassBoost`, `LoudnessEnhancer` to that session | `AudioPlaybackCapture` (Android 10+) → Sorus C++ engine → output via Oboe/AAudio; originals muted |
| Apps covered | Apps that announce their session (most music players, Spotify, etc.) | Every app that allows capture. DRM video apps (Netflix, Prime…) usually block it |
| Features | Approximation: Sorus's parametric curve is **fitted** onto DynamicsProcessing's band array (pre-EQ, multiband compressor, post-EQ, limiter, input gain) + platform virtualizer/bass. No dynamic EQ, linear phase, convolution, HRTF | **Everything** in §7 |
| Added latency | ~0 | Capture + processing + output buffers. Target < 40 ms total; DSP itself < 5 ms |

Extras with Shizuku (all free): grant `PROJECT_MEDIA` so the capture consent dialog doesn't reappear; grant `DUMP` so
Sorus can find sessions of apps that don't broadcast and attach session-mode effects to them too.

Both modes can be on at once; Sorus picks the best route per app and shows a per-app coverage list
(processed / not processed and why).

**Open risk (spike A1):** how to mute the original app audio in full mode without root. Shizuku runs with shell-level
privileges; candidate approaches need to be proven on real devices. **Owner decision:** if A1 fails, Android ships with
session mode only and full mode keeps being researched; Windows is unaffected. The feature gap between the two modes is accepted.

### 6.2 Windows — APO

- Sorus registers an **Audio Processing Object** (C++ COM DLL wrapping the shared engine) on the chosen output endpoints,
  as a mix-level effect. Runs inside Windows' audio engine (`audiodg.exe`): no added latency, lowest CPU.
- Install/uninstall needs admin once. Installer backs up and restores the endpoint's original effect registration.
- Streams in WASAPI exclusive mode bypass APOs — documented, not fixable.
- Some vendor drivers block or conflict with third-party APOs. Sorus detects "APO not loaded" and tells the user.
- **Open risk (spike W1):** confirm an unsigned APO loads on current Windows 10 22H2 and Windows 11 with default security settings.
- Per-app profiles on Windows are **out of v1** (an endpoint-level APO doesn't see which app is playing). *(My call.)*

## 7. Features

### 7.1 Signal chain (fixed order, each block bypassable at zero cost)

Input gain/preamp → **Pro EQ** → dynamics (compressor, multiband compressor) → bass engine (psychoacoustic bass, sub
synth) → saturation/exciter (tube, tape) → stereo (width, M/S, crossfeed Bauer/Meier) → spatial (HRTF virtual surround,
convolution) → loudness normalization (LUFS) → true-peak limiter → output.

### 7.2 Pro EQ

- Parametric, up to **32 bands**. Per band: type (bell, low/high shelf, HP/LP 6–96 dB/oct, notch, band-pass, tilt,
  all-pass), freq 10 Hz–22 kHz, gain ±24 dB, Q 0.1–40, channel mode (stereo / L / R / Mid / Side), enable.
- Topology: state-variable / SVF filters so parameter moves never click; double-precision state for low bands at high rates.
- **Dynamic EQ** per band: threshold, ratio, attack, release, optional sidechain band.
- **Phase modes:** minimum phase (default, zero latency); linear phase (FIR via partitioned convolution, latency shown in UI).
- **Auto preamp:** computes the curve's peak boost and sets matching negative preamp. Plus true-peak limiter at the end.
- **Gain-matched A/B bypass.**
- **Graphic view:** 10 / 15 / 31-band ISO, written into the same parametric band model (never a separate engine).
- **Analyzer:** pre/post FFT, peak hold, 1/3 or 1/6-octave smoothing; runs only while on screen.
- **Headphone correction:** public measurement database (AutoEq-style) + target curves (Harman OE/IE, diffuse field, custom).
- **Loudness compensation** (ISO 226), tied to system volume.
- **Hearing profile:** in-app tone test → compensation curve. No medical claims.
- Snapshots, undo/redo, import/export (EqualizerAPO, REW, GraphicEQ text).

### 7.3 Movie mode
Dialogue boost (center extraction + voice-band lift), night mode (DRC), virtual surround (stereo → virtual 5.1/7.1 →
binaural via HRTF; custom SOFA files), phone-speaker protection (HPF at speaker resonance + psychoacoustic bass + limiter).

### 7.4 Music mode
Convolution with user impulse responses, saturation/exciter, crossfeed, cross-app loudness normalization.
Android 14+: bit-perfect USB DAC output at native rate where the device supports it.

### 7.5 Everyday
Profiles that auto-switch by output device (each Bluetooth device, wired, speaker, USB DAC) and, on Android, by app.
Quick Settings tile, widget, notification controls (Android); tray icon + hotkeys (Windows). Rules by time/app/device.
Meters: LUFS, peak, correlation, CPU. Listening-exposure tracking.

### 7.6 UI
Two always-visible tabs: **Simple** (one-tap Music/Movie/Night/Dialogue, headphone picker, graphic EQ) and **Pro**
(everything). Both edit the same settings.

## 8. Performance budget

- Audio thread: no allocation, no locks, no syscalls, no logging. Parameter changes via lock-free queues, smoothed per sample.
- SIMD: NEON (Android arm64), SSE4.1/AVX2 with runtime dispatch (Windows x64).
- Full chain (excluding linear-phase EQ and convolution): < 5% of one core on a mid-range phone, < 1% on a typical PC.
- DSP latency < 5 ms in every mode except linear phase / convolution, whose latency is shown next to their switch.
- Bypassed blocks cost nothing; processing idles during silence; analyzer only runs while visible.
- UI: 60/120 fps; never blocks or waits on the audio thread.
- Benchmarks and null/response tests run on desktop for every change (free CI for public repos).

## 9. Architecture

```
sorus/
  core/       C++20 DSP engine + C API (sorus.h). No platform code. CMake.
  android/    Kotlin + Jetpack Compose app; JNI → core; Oboe/AAudio; Shizuku integration
  windows/    apo/ (C++ COM APO DLL linking core)  app/ (C# WinUI 3, Native AOT; P/Invoke → core)
  presets/    versioned JSON preset schema + built-in presets
  tools/      bench, response plots, preset converters
```

- **One engine, C ABI.** Android talks to it over JNI, Windows UI over P/Invoke, APO links it directly.
- **Preset format:** versioned JSON (`.sorus`), identical on both platforms. Session mode on Android reads the same
  preset and fits it onto platform effects.
- **Windows UI ↔ APO:** APO runs in `audiodg.exe`, a separate restricted process. Settings flow through a shared-memory
  block with a suitable ACL (lock-free), with a file fallback on load. *(My call; spike W2 proves it.)*
- Android: foreground service owns the engine and session listeners; UI process only edits state.

## 10. Free-only stack and distribution

- Android: Kotlin, Compose, NDK, Oboe, Shizuku — all free. Distribute via **GitHub Releases** and **F-Droid**.
  Play Store skipped ($25 fee).
- Windows: Windows App SDK / WinUI 3, .NET — free. Code signing via **SignPath Foundation** (free for open source);
  Microsoft Store individual account if confirmed free at the time.
- Candidate libraries (licences to verify before use): PFFFT (FFT), libmysofa (SOFA/HRTF), AutoEq measurement data.
- CI: GitHub Actions (free for public repos).
- Licence: **GPL-3.0** *(my call — keeps forks open, allows reuse of GPL audio code; owner may change)*.

## 11. Minimum platform versions *(my call)*

Android 10 (API 29) — needed for playback capture. Windows 10 22H2 and Windows 11, x64 (arm64 later).

## 12. Spikes before committing to the plan

| ID | Question | Kills/changes |
|---|---|---|
| A1 | Can Sorus mute original app audio in full mode via Shizuku, no root? | Full mode viability |
| A2 | Session-mode coverage across top 20 music/video apps on 3+ phone brands | Expectations, UI copy |
| A3 | How accurately can a 32-band parametric curve be fitted onto DynamicsProcessing bands? | Session-mode EQ quality |
| A4 | Real full-mode round-trip latency on mid-range phones | Movie lip sync |
| W1 | Unsigned APO loads on Win10/11 default settings; behaviour across Realtek/Intel/USB drivers | Windows viability |
| W2 | UI ↔ APO shared-memory settings path works under `audiodg.exe` restrictions | Windows architecture |

## 13. Left to the builder

Internal class design, exact filter math per band type, SIMD library choice, UI layout details, test framework choice,
preset schema field names, build tooling beyond CMake/Gradle/MSBuild.
