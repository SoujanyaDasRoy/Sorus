// Spike W2 (throwaway): run as a normal user. Opens the APO's settings block, plays a 1 kHz tone on the endpoint
// while recording it with WASAPI loopback, and steps the gain 0 -> -6 -> -12 -> 0 dB. For each step it prints the
// time from the write until the APO applied it (QPC stamps) and the loopback level change.
// PASS: every step applied within 10 ms, level change = gain change +- 0.1 dB. Exit 0 = pass.
// Usage: w2test.exe [endpoint-id]
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <ks.h>
#include <ksmedia.h>
#include <cmath>
#include <cstdio>

#include "control.h"

#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { std::fprintf(stderr, "%s failed: 0x%08lx\n", #x, hr_); return 2; } } while (0)

int wmain(int argc, wchar_t** argv) {
  HANDLE h = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, SORUS_W2_SECTION);
  if (!h) {
    std::printf("FAIL cannot open %ls (error %lu): APO not loaded yet, or no access\n", SORUS_W2_SECTION, GetLastError());
    return 1;
  }
  auto* c = static_cast<SorusW2Control*>(MapViewOfFile(h, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(SorusW2Control)));
  if (!c || c->magic != kSorusW2Magic) { std::printf("FAIL settings block not initialised\n"); return 1; }
  std::printf("settings block open as a normal user; APOs mapped so far: %ld\n", c->apo_instances);

  CHECK(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
  IMMDeviceEnumerator* en = nullptr;
  CHECK(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)));
  IMMDevice* dev = nullptr;
  if (argc > 1) CHECK(en->GetDevice(argv[1], &dev));
  else CHECK(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev));
  IAudioClient *cap = nullptr, *ren = nullptr;
  CHECK(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&cap)));
  CHECK(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&ren)));
  WAVEFORMATEX* wf = nullptr;
  CHECK(ren->GetMixFormat(&wf));
  if (wf->wBitsPerSample != 32) { std::fprintf(stderr, "mix format is not float32\n"); return 2; }
  const UINT32 ch = wf->nChannels, rate = wf->nSamplesPerSec;
  constexpr REFERENCE_TIME kBuf = 2'000'000;  // 200 ms
  CHECK(cap->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, kBuf, 0, wf, nullptr));
  CHECK(ren->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, kBuf, 0, wf, nullptr));
  IAudioCaptureClient* cc = nullptr;
  IAudioRenderClient* rc = nullptr;
  CHECK(cap->GetService(IID_PPV_ARGS(&cc)));
  CHECK(ren->GetService(IID_PPV_ARGS(&rc)));
  UINT32 ren_size = 0;
  CHECK(ren->GetBufferSize(&ren_size));

  const double step = 2 * 3.14159265358979 * 1000 / rate;
  double phase = 0;
  auto fill = [&](UINT32 frames) -> HRESULT {
    BYTE* p = nullptr;
    HRESULT hr = rc->GetBuffer(frames, &p);
    if (FAILED(hr)) return hr;
    auto* f = reinterpret_cast<float*>(p);
    for (UINT32 i = 0; i < frames; ++i, phase += step)
      for (UINT32 k = 0; k < ch; ++k) f[i * ch + k] = static_cast<float>(0.25 * std::sin(phase));
    return rc->ReleaseBuffer(frames, 0);
  };
  LARGE_INTEGER freq;
  QueryPerformanceFrequency(&freq);
  auto set_gain = [&](float db) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    c->gain_db = db;
    c->written_qpc = now.QuadPart;
    return InterlockedIncrement(&c->seq);
  };

  set_gain(0);
  CHECK(fill(ren_size));
  CHECK(cap->Start());
  CHECK(ren->Start());

  // Step 0 is a warm-up (stream start, vendor effects settling) and is not judged; step 1 is the 0 dB reference.
  const float gains[] = {0, 0, -6, -12, 0};
  constexpr int kSteps = 5;
  double level[kSteps] = {}, latency_ms[kSteps] = {};
  const UINT64 seg = 3ull * rate / 2;           // 1.5 s per step
  const UINT64 settle = rate / 2;                // measure the last 1 s of each step
  UINT64 seen = 0;
  double sum = 0;
  UINT64 used = 0;
  int cur = -1;
  LONG pending = 0;
  LONGLONG written = 0;
  bool ok = true;
  while (cur < kSteps) {
    const int want = static_cast<int>(seen / seg);
    if (want != cur) {
      if (cur >= 0) level[cur] = 20 * std::log10(std::sqrt(sum / static_cast<double>(used)));
      cur = want;
      if (cur >= kSteps) break;
      sum = 0;
      used = 0;
      pending = set_gain(gains[cur]);
      written = c->written_qpc;
    }
    if (pending && ReadAcquire(&c->applied_seq) == pending) {
      latency_ms[cur] = 1000.0 * static_cast<double>(c->applied_qpc - written) / static_cast<double>(freq.QuadPart);
      pending = 0;
    }
    Sleep(1);
    UINT32 pad = 0;
    CHECK(ren->GetCurrentPadding(&pad));
    if (ren_size > pad) CHECK(fill(ren_size - pad));
    for (;;) {
      UINT32 n = 0;
      CHECK(cc->GetNextPacketSize(&n));
      if (n == 0) break;
      BYTE* p = nullptr;
      DWORD flags = 0;
      CHECK(cc->GetBuffer(&p, &n, &flags, nullptr, nullptr));
      const auto* f = reinterpret_cast<const float*>(p);
      for (UINT32 i = 0; i < n; ++i, ++seen) {
        if (seen % seg < settle || seen / seg != static_cast<UINT64>(cur)) continue;
        const double v = (flags & AUDCLNT_BUFFERFLAGS_SILENT) ? 0.0 : f[i * ch];
        sum += v * v;
        ++used;
      }
      CHECK(cc->ReleaseBuffer(n));
    }
  }
  ren->Stop();
  cap->Stop();

  for (int i = 1; i < kSteps; ++i) {
    const double change = level[i] - level[1];
    const bool lat_ok = latency_ms[i] > 0 && latency_ms[i] <= 10;
    const bool lvl_ok = std::fabs(change - gains[i]) <= 0.1;
    ok = ok && lat_ok && lvl_ok;
    std::printf("gain %6.1f dB: applied after %6.2f ms %s | level %7.2f dBFS, change %6.2f dB %s\n", gains[i],
                latency_ms[i], lat_ok ? "ok" : "FAIL", level[i], change, lvl_ok ? "ok" : "FAIL");
  }
  std::printf(ok ? "PASS\n" : "FAIL\n");
  return ok ? 0 : 1;
}
