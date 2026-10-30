/* Sorus core engine — C API. The only interface platform layers (Android JNI, Windows APO/UI) call.
 * Threads: sorus_process runs on the audio thread; every other call on one control thread. */
#ifndef SORUS_H
#define SORUS_H

#include <stddef.h>

#ifndef SORUS_API
#define SORUS_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SorusEngine SorusEngine;

/* NULL if sample_rate outside [8000, 384000], max_frames < 1, or channels outside [1, 8]. */
SORUS_API SorusEngine* sorus_create(double sample_rate, int max_frames, int channels);
SORUS_API void sorus_destroy(SorusEngine* e); /* NULL-safe */

/* Audio thread. In-place, interleaved. NULL-safe; frames <= 0 is a no-op. */
SORUS_API void sorus_process(SorusEngine* e, float* interleaved, int frames);

/* Control thread. Return 0 on success, -1 on bad args or parse error. */
SORUS_API int sorus_load_preset(SorusEngine* e, const char* json, size_t len);
SORUS_API int sorus_last_error(const SorusEngine* e, char* buf, size_t len); /* copies last error, truncated */
SORUS_API int sorus_latency_frames(const SorusEngine* e);                     /* -1 if e is NULL */
SORUS_API int sorus_eq_response_db(const SorusEngine* e, const double* freqs, double* out_db, int n);
/* Pushes settings still waiting for room in the audio queue. 1 = nothing pending, 0 = still pending,
 * -1 = NULL. Call on stream start, and periodically from the UI while it returns 0. */
SORUS_API int sorus_flush(SorusEngine* e);

/* Android session mode: fits a preset's EQ onto DynamicsProcessing bands given by their upper cutoffs
 * (band 0 starts at 20 Hz). Writes n gains (dB, clamped to +-24). 0 on success, -1 on bad args/parse error. */
SORUS_API int sorus_fit_dp_bands(const char* preset_json, size_t len, double sample_rate, const float* cutoff_hz,
                                 int n, float* gain_db_out);

#ifdef __cplusplus
}
#endif

#endif /* SORUS_H */
