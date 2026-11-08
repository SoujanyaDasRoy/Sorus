// Spike W2 (throwaway): the W1 APO, but its gain comes from a shared-memory block (control.h) that a normal
// user-mode app writes. Registered by install.ps1 into an endpoint's composite MFX list; audiodg.exe loads it.
#include <windows.h>
#include <audioenginebaseapo.h>
#include <audiomediatype.h>
#include <ks.h>
#include <ksmedia.h>
#include <sddl.h>
#include <cmath>
#include <mutex>
#include <new>

#include "control.h"

namespace {

// {469205DD-A208-4C1A-A434-D3514B371236}
constexpr CLSID kClsid = {0x469205DD, 0xA208, 0x4C1A, {0xA4, 0x34, 0xD3, 0x51, 0x4B, 0x37, 0x12, 0x36}};
LONG g_objects = 0, g_locks = 0;

// Created once per audiodg process and kept for its lifetime (spike: the handle is never closed).
SorusW2Control* Control() {
  static SorusW2Control* block = nullptr;
  static std::once_flag once;
  std::call_once(once, [] {
    // SYSTEM, LocalService (audiodg) and Administrators: full; interactive users: read + write.
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:(A;;GA;;;SY)(A;;GA;;;LS)(A;;GA;;;BA)(A;;GRGW;;;IU)", SDDL_REVISION_1, &sd, nullptr))
      return;
    SECURITY_ATTRIBUTES sa{sizeof sa, sd, FALSE};
    HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(SorusW2Control),
                                  SORUS_W2_SECTION);
    LocalFree(sd);
    if (!h) return;
    auto* p = static_cast<SorusW2Control*>(MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SorusW2Control)));
    if (!p) return;
    InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&p->magic), kSorusW2Magic, 0);
    block = p;
  });
  return block;
}

class GainApo final : public IAudioProcessingObject,
                      public IAudioProcessingObjectRT,
                      public IAudioProcessingObjectConfiguration,
                      public IAudioSystemEffects {
 public:
  // audiodg creates APOs aggregated (outer != null): the public IUnknown methods delegate to the outer
  // object, and inner_ is the non-delegating IUnknown that owns the lifetime.
  explicit GainApo(IUnknown* outer) : outer_(outer ? outer : &inner_) {
    inner_.self = this;
    InterlockedIncrement(&g_objects);
  }
  ~GainApo() { InterlockedDecrement(&g_objects); }

  struct Inner final : IUnknown {
    GainApo* self = nullptr;
    STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
      if (!out) return E_POINTER;
      if (iid == __uuidof(IUnknown)) {
        *out = this;
        AddRef();
        return S_OK;
      }
      if (iid == __uuidof(IAudioProcessingObject))
        *out = static_cast<IAudioProcessingObject*>(self);
      else if (iid == __uuidof(IAudioProcessingObjectRT))
        *out = static_cast<IAudioProcessingObjectRT*>(self);
      else if (iid == __uuidof(IAudioProcessingObjectConfiguration))
        *out = static_cast<IAudioProcessingObjectConfiguration*>(self);
      else if (iid == __uuidof(IAudioSystemEffects))
        *out = static_cast<IAudioSystemEffects*>(self);
      else {
        *out = nullptr;
        return E_NOINTERFACE;
      }
      self->outer_->AddRef();
      return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&self->refs_); }
    STDMETHODIMP_(ULONG) Release() override {
      const ULONG r = InterlockedDecrement(&self->refs_);
      if (r == 0) delete self;
      return r;
    }
  } inner_;

  // Delegating IUnknown (shared by every interface below).
  STDMETHODIMP QueryInterface(REFIID iid, void** out) override { return outer_->QueryInterface(iid, out); }
  STDMETHODIMP_(ULONG) AddRef() override { return outer_->AddRef(); }
  STDMETHODIMP_(ULONG) Release() override { return outer_->Release(); }

  // IAudioProcessingObject
  STDMETHODIMP Reset() override { return S_OK; }
  STDMETHODIMP GetLatency(HNSTIME* t) override {
    if (!t) return E_POINTER;
    *t = 0;
    return S_OK;
  }
  STDMETHODIMP GetRegistrationProperties(APO_REG_PROPERTIES** props) override {
    if (!props) return E_POINTER;
    auto* p = static_cast<APO_REG_PROPERTIES*>(CoTaskMemAlloc(sizeof(APO_REG_PROPERTIES)));
    if (!p) return E_OUTOFMEMORY;
    ZeroMemory(p, sizeof *p);
    p->clsid = kClsid;
    p->Flags = APO_FLAG_DEFAULT;
    wcscpy_s(p->szFriendlyName, L"Sorus W2 spike APO");
    wcscpy_s(p->szCopyrightInfo, L"Sorus contributors, GPL-3.0");
    p->u32MajorVersion = 1;
    p->u32MinInputConnections = p->u32MaxInputConnections = 1;
    p->u32MinOutputConnections = p->u32MaxOutputConnections = 1;
    p->u32MaxInstances = 0xffffffff;
    p->u32NumAPOInterfaces = 1;
    p->iidAPOInterfaceList[0] = __uuidof(IAudioProcessingObject);
    *props = p;
    return S_OK;
  }
  STDMETHODIMP Initialize(UINT32, BYTE*) override { return S_OK; }
  STDMETHODIMP IsInputFormatSupported(IAudioMediaType*, IAudioMediaType* req, IAudioMediaType** out) override {
    return Supported(req, out);
  }
  STDMETHODIMP IsOutputFormatSupported(IAudioMediaType*, IAudioMediaType* req, IAudioMediaType** out) override {
    return Supported(req, out);
  }
  STDMETHODIMP GetInputChannelCount(UINT32* n) override {
    if (!n) return E_POINTER;
    *n = channels_;
    return S_OK;
  }

  // IAudioProcessingObjectConfiguration
  STDMETHODIMP LockForProcess(UINT32 nin, APO_CONNECTION_DESCRIPTOR** in, UINT32 nout,
                              APO_CONNECTION_DESCRIPTOR**) override {
    if (nin != 1 || nout != 1 || !in || !in[0] || !in[0]->pFormat) return E_INVALIDARG;
    UNCOMPRESSEDAUDIOFORMAT f{};
    const HRESULT hr = in[0]->pFormat->GetUncompressedAudioFormat(&f);
    if (FAILED(hr)) return hr;
    channels_ = f.dwSamplesPerFrame;
    if (!control_ && (control_ = Control()) != nullptr) InterlockedIncrement(&control_->apo_instances);
    return S_OK;
  }
  STDMETHODIMP UnlockForProcess() override { return S_OK; }

  // IAudioProcessingObjectRT: real-time thread, no allocation or locks.
  STDMETHODIMP_(void) APOProcess(UINT32, APO_CONNECTION_PROPERTY** in, UINT32,
                                 APO_CONNECTION_PROPERTY** out) override {
    const auto* src = reinterpret_cast<const float*>(in[0]->pBuffer);
    auto* dst = reinterpret_cast<float*>(out[0]->pBuffer);
    const UINT32 frames = in[0]->u32ValidFrameCount;
    if (control_) {
      const LONG seq = ReadAcquire(&control_->seq);
      if (seq != seen_) {
        seen_ = seq;
        gain_ = std::pow(10.0f, control_->gain_db / 20.0f);
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        WriteRelease64(&control_->applied_qpc, now.QuadPart);
        InterlockedExchange(&control_->applied_seq, seq);
      }
    }
    if (in[0]->u32BufferFlags != BUFFER_SILENT) {
      for (UINT32 i = 0, n = frames * channels_; i < n; ++i) dst[i] = src[i] * gain_;
    }
    out[0]->u32ValidFrameCount = frames;
    out[0]->u32BufferFlags = in[0]->u32BufferFlags;
  }
  STDMETHODIMP_(UINT32) CalcInputFrames(UINT32 n) override { return n; }
  STDMETHODIMP_(UINT32) CalcOutputFrames(UINT32 n) override { return n; }

 private:
  // Float32 only (the shared-mode engine's native format); output format must equal input.
  static HRESULT Supported(IAudioMediaType* req, IAudioMediaType** out) {
    if (!req || !out) return E_POINTER;
    *out = nullptr;
    UNCOMPRESSEDAUDIOFORMAT f{};
    if (FAILED(req->GetUncompressedAudioFormat(&f))) return APOERR_FORMAT_NOT_SUPPORTED;
    if (f.guidFormatType != KSDATAFORMAT_SUBTYPE_IEEE_FLOAT || f.dwBytesPerSampleContainer != 4)
      return APOERR_FORMAT_NOT_SUPPORTED;
    req->AddRef();
    *out = req;
    return S_OK;
  }

  IUnknown* outer_;
  LONG refs_ = 1;
  UINT32 channels_ = 2;
  SorusW2Control* control_ = nullptr;
  LONG seen_ = 0;
  float gain_ = 1;  // unity until the app writes a value
};

class Factory final : public IClassFactory {
 public:
  STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IClassFactory)) {
      *out = this;
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return 2; }  // static instance
  STDMETHODIMP_(ULONG) Release() override { return 1; }
  STDMETHODIMP CreateInstance(IUnknown* outer, REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (outer && iid != __uuidof(IUnknown)) return CLASS_E_NOAGGREGATION;  // COM rule for aggregation
    auto* apo = new (std::nothrow) GainApo(outer);
    if (!apo) return E_OUTOFMEMORY;
    const HRESULT hr = apo->inner_.QueryInterface(iid, out);
    apo->inner_.Release();
    return hr;
  }
  STDMETHODIMP LockServer(BOOL lock) override {
    lock ? InterlockedIncrement(&g_locks) : InterlockedDecrement(&g_locks);
    return S_OK;
  }
};

Factory g_factory;

}  // namespace

STDAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void** out) {
  if (clsid != kClsid) return CLASS_E_CLASSNOTAVAILABLE;
  return g_factory.QueryInterface(iid, out);
}

STDAPI DllCanUnloadNow() { return (g_objects == 0 && g_locks == 0) ? S_OK : S_FALSE; }
