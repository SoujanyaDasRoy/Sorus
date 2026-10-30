#include <sorus/sorus.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <string_view>

#include "dp_fit.h"
#include "engine.h"
#include "preset.h"

// No C++ exception may cross the C boundary: every entry point that can throw catches everything.
struct SorusEngine {
  sorus::Engine engine;
  std::string last_error;
};

extern "C" {

SorusEngine* sorus_create(double sample_rate, int max_frames, int channels) {
  if (!(sample_rate >= 8000 && sample_rate <= 384000) || max_frames < 1 || channels < 1 ||
      channels > sorus::kMaxChannels)
    return nullptr;
  try {
    return new SorusEngine{sorus::Engine(sample_rate, max_frames, channels), {}};
  } catch (...) {
    return nullptr;
  }
}

void sorus_destroy(SorusEngine* e) { delete e; }

void sorus_process(SorusEngine* e, float* interleaved, int frames) {
  if (e) e->engine.process_interleaved(interleaved, frames);
}

int sorus_load_preset(SorusEngine* e, const char* json, size_t len) {
  if (!e || !json) return -1;
  try {
    auto p = sorus::parse_preset(std::string_view(json, len), &e->last_error);
    if (!p) return -1;
    e->engine.load(*p);
    e->last_error.clear();
    return 0;
  } catch (const std::exception& ex) {
    e->last_error = ex.what();
    return -1;
  } catch (...) {
    e->last_error = "unknown error";
    return -1;
  }
}

int sorus_last_error(const SorusEngine* e, char* buf, size_t len) {
  if (!e || !buf || len == 0) return -1;
  const size_t n = std::min(len - 1, e->last_error.size());
  std::memcpy(buf, e->last_error.data(), n);
  buf[n] = '\0';
  return 0;
}

int sorus_latency_frames(const SorusEngine* e) { return e ? e->engine.latency_frames() : -1; }

int sorus_eq_response_db(const SorusEngine* e, const double* freqs, double* out_db, int n) {
  if (!e || !freqs || !out_db || n < 0) return -1;
  for (int i = 0; i < n; ++i) out_db[i] = e->engine.eq_response_db(freqs[i]);
  return 0;
}

int sorus_flush(SorusEngine* e) { return e ? (e->engine.flush() ? 1 : 0) : -1; }

int sorus_fit_dp_bands(const char* preset_json, size_t len, double sample_rate, const float* cutoff_hz, int n,
                       float* gain_db_out) {
  if (!preset_json || !cutoff_hz || !gain_db_out || n < 0 || !(sample_rate >= 8000 && sample_rate <= 384000))
    return -1;
  try {
    auto p = sorus::parse_preset(std::string_view(preset_json, len), nullptr);
    if (!p) return -1;
    sorus::fit_to_dp_bands(p->bands, sample_rate, cutoff_hz, n, gain_db_out);
    return 0;
  } catch (...) {
    return -1;
  }
}

}  // extern "C"
