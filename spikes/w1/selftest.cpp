// Spike W1 (throwaway): loads sorus_w1_apo.dll directly and creates the APO aggregated, as audiodg does,
// then runs APOProcess on a stereo buffer and checks the -6 dB gain. Exit 0 = pass.
#include <windows.h>
#include <audioenginebaseapo.h>
#include <cmath>
#include <cstdio>

namespace {
constexpr CLSID kClsid = {0xB3433C7B, 0x8D79, 0x4545, {0x8F, 0xD7, 0xD6, 0x30, 0x9E, 0xD9, 0xA0, 0x37}};

// Minimal controlling outer object: forwards every other interface to the inner APO.
struct Outer final : IUnknown {
  IUnknown* inner = nullptr;
  LONG refs = 1;
  STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
    if (iid == __uuidof(IUnknown)) { *out = this; AddRef(); return S_OK; }
    return inner->QueryInterface(iid, out);
  }
  STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs); }
  STDMETHODIMP_(ULONG) Release() override { return InterlockedDecrement(&refs); }  // stack object
};

int fail(const char* what, HRESULT hr = S_OK) {
  std::printf("FAIL %s 0x%08lx\n", what, hr);
  return 1;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  HMODULE dll = LoadLibraryW(argc > 1 ? argv[1] : L"sorus_w1_apo.dll");
  if (!dll) return fail("LoadLibrary", HRESULT_FROM_WIN32(GetLastError()));
  auto get = reinterpret_cast<HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**)>(GetProcAddress(dll, "DllGetClassObject"));
  if (!get) return fail("DllGetClassObject export");
  IClassFactory* f = nullptr;
  HRESULT hr = get(kClsid, IID_PPV_ARGS(&f));
  if (FAILED(hr)) return fail("DllGetClassObject", hr);

  Outer outer;
  void* bad = nullptr;
  if (f->CreateInstance(&outer, __uuidof(IAudioProcessingObject), &bad) != CLASS_E_NOAGGREGATION)
    return fail("aggregated create must require IID_IUnknown");
  hr = f->CreateInstance(&outer, __uuidof(IUnknown), reinterpret_cast<void**>(&outer.inner));
  if (FAILED(hr)) return fail("CreateInstance (aggregated)", hr);

  IAudioProcessingObjectRT* rt = nullptr;
  hr = outer.QueryInterface(IID_PPV_ARGS(&rt));
  if (FAILED(hr)) return fail("QI IAudioProcessingObjectRT", hr);
  IAudioProcessingObject* apo = nullptr;
  if (FAILED(rt->QueryInterface(IID_PPV_ARGS(&apo)))) return fail("QI through delegating IUnknown");
  IUnknown* back = nullptr;
  if (FAILED(apo->QueryInterface(IID_PPV_ARGS(&back))) || back != &outer) return fail("IUnknown must be the outer");
  back->Release();

  float in[8] = {1, -1, 0.5f, -0.5f, 0.25f, 0, 1, 1}, out[8] = {};
  APO_CONNECTION_PROPERTY ci{reinterpret_cast<UINT_PTR>(in), 4, BUFFER_VALID, APO_CONNECTION_PROPERTY_SIGNATURE};
  APO_CONNECTION_PROPERTY co{reinterpret_cast<UINT_PTR>(out), 0, BUFFER_INVALID, APO_CONNECTION_PROPERTY_SIGNATURE};
  APO_CONNECTION_PROPERTY* pin = &ci;
  APO_CONNECTION_PROPERTY* pout = &co;
  rt->APOProcess(1, &pin, 1, &pout);
  if (co.u32ValidFrameCount != 4 || co.u32BufferFlags != BUFFER_VALID) return fail("output connection fields");
  for (int i = 0; i < 8; ++i)
    if (std::fabs(out[i] - in[i] * 0.501187234f) > 1e-6f) return fail("gain");

  apo->Release();
  rt->Release();
  outer.inner->Release();  // last inner ref: deletes the APO
  using CanUnload = HRESULT(STDAPICALLTYPE*)();
  if (reinterpret_cast<CanUnload>(GetProcAddress(dll, "DllCanUnloadNow"))() != S_OK) return fail("object leaked");
  std::printf("PASS\n");
  return 0;
}
