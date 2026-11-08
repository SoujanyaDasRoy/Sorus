// Spike W1 (throwaway): plays a 1 kHz tone (-12 dBFS peak) on a render endpoint while recording that endpoint
// with WASAPI loopback, and prints the recorded RMS level of channel 0 in dBFS.
// Usage: meter.exe [endpoint-id]   (default: the default console render device)
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <ks.h>
#include <ksmedia.h>
#include <cmath>
#include <cstdio>

#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { std::fprintf(stderr, "%s failed: 0x%08lx\n", #x, hr_); return 1; } } while (0)

int wmain(int argc, wchar_t** argv) {
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
  const bool is_float = wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
      (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
       reinterpret_cast<WAVEFORMATEXTENSIBLE*>(wf)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
  if (!is_float || wf->wBitsPerSample != 32) { std::fprintf(stderr, "mix format is not float32\n"); return 1; }
  const UINT32 ch = wf->nChannels, rate = wf->nSamplesPerSec;

  constexpr REFERENCE_TIME kBuf = 10'000'000;  // 1 s
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
      for (UINT32 c = 0; c < ch; ++c) f[i * ch + c] = static_cast<float>(0.25 * std::sin(phase));
    return rc->ReleaseBuffer(frames, 0);
  };
  CHECK(fill(ren_size));
  CHECK(cap->Start());
  CHECK(ren->Start());

  // Skip 0.5 s of settling, then average 2 s of channel 0.
  const UINT64 skip = rate / 2, measure = 2ull * rate;
  UINT64 seen = 0, used = 0;
  double sum = 0;
  while (used < measure) {
    Sleep(10);
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
      for (UINT32 i = 0; i < n && used < measure; ++i, ++seen) {
        if (seen < skip) continue;
        const double v = (flags & AUDCLNT_BUFFERFLAGS_SILENT) ? 0.0 : f[i * ch];
        sum += v * v;
        ++used;
      }
      CHECK(cc->ReleaseBuffer(n));
    }
    if (seen > 10ull * rate) { std::fprintf(stderr, "loopback delivered no usable audio\n"); return 1; }
  }
  ren->Stop();
  cap->Stop();
  const double rms = std::sqrt(sum / static_cast<double>(used));
  std::printf("%.2f\n", rms > 0 ? 20 * std::log10(rms) : -999.0);
  return 0;
}
