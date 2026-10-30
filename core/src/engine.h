#pragma once

#include <array>
#include <memory>
#include <vector>

#include "limiter.h"
#include "parametric_eq.h"
#include "params.h"
#include "spsc_queue.h"

namespace sorus {

enum class Block { Eq, Limiter };

// Chain: sanitize (non-finite -> 0) -> preamp -> EQ -> limiter. Two threads:
// control thread calls the setters and queries; they update control-side state and push commands.
// The audio thread drains the commands at the start of process_interleaved, so every call into the
// audio-side EQ and limiter happens on the audio thread. Preamp and bypass changes glide over 10 ms.
// Latency is always the limiter's: a bypassed limiter is replaced by a delay line of the same length.
class Engine {
 public:
  // Allocates. channels clamped to [1, kMaxChannels], max_frames to >= 1.
  Engine(double sample_rate, int max_frames, int channels);

  // Control thread. Never block: if the queue is full, the latest values stay pending and are
  // retried on the next setter call.
  void set_band(int index, const BandParams& p);  // out-of-range index ignored
  void set_preamp_db(double db);                  // clamped to +-24 dB; non-finite ignored
  void set_auto_preamp(bool on);                  // on by default
  void set_limiter_ceiling_db(double db);         // limiter clamps to [-24, 0] dBTP
  void set_bypass(Block b, bool bypassed);
  double eq_response_db(double freq) const;  // EQ curve as set (ignores bypass)
  int latency_frames() const { return latency_; }

  // Audio thread: no alloc, no locks. Interleaved `frames` x channels; frames <= 0 or null is a no-op.
  void process_interleaved(float* data, int frames);

 private:
  struct Command {
    enum class Kind { Band, Preamp, Ceiling, Bypass } kind;
    int index;  // band index, or Block for Bypass
    BandParams band;
    double value;  // dB for Preamp/Ceiling, 0/1 for Bypass
  };
  // Linear glide that lands exactly on its target.
  struct Ramp {
    double cur, target = cur, inc = 0;
    int left = 0;
    void set(double t, int len) { target = t; left = len; inc = (t - cur) / len; }
    double next() {
      if (left > 0) cur = --left ? cur + inc : target;
      return cur;
    }
  };

  void update_preamp();  // control: recompute the effective preamp, then flush
  void flush();          // control: push dirty state; whatever does not fit stays dirty
  void apply(const Command& c);
  void run(int frames);  // audio: chain on the planar scratch

  int max_frames_, channels_, latency_ = 0, ramp_len_ = 1;
  SpscQueue<Command, 256> queue_;

  // Control side.
  std::unique_ptr<ParametricEq> mirror_;  // same clamping as the audio EQ; feeds response/peak queries
  std::array<BandParams, kMaxBands> bands_{};
  std::array<bool, kMaxBands> band_dirty_{};
  double preamp_db_ = 0, effective_db_ = 0, ceiling_db_ = -1;
  bool auto_ = true;
  std::array<bool, 2> bypass_{};
  bool effective_dirty_ = false, ceiling_dirty_ = false;
  std::array<bool, 2> bypass_dirty_{};

  // Audio side.
  std::unique_ptr<ParametricEq> eq_;  // ~130 KB: heap
  TruePeakLimiter lim_;
  Ramp gain_{1}, eq_mix_{1}, lim_mix_{1};  // linear gain; 1 = block active, 0 = bypassed
  std::array<std::vector<float>, kMaxChannels> buf_, dry_, delay_;
  int delay_pos_ = 0;
};

}  // namespace sorus
