#pragma once

#include "params.h"

namespace sorus {

// Exact magnitude (dB) of what Band does with `p` at `freq`; 0 for a disabled band. freq is clamped to
// [0, sample_rate / 2] (NaN reads as 0); the result is finite and floored at kFloorDb.
constexpr double kFloorDb = -200;
double response_db(const BandParams& p, double freq, double sample_rate);

// One Cytomic trapezoidal SVF section: y = m0*v0 + m1*bandpass + m2*lowpass.
struct SvfSection {
  double g = 0, k = 1, m0 = 1, m1 = 0, m2 = 0;  // defaults = pass-through
};

class Band {
 public:
  static constexpr int kMaxSlots = 8;
  static constexpr int kMaxSections = 8;  // 96 dB/oct = order 16 = 8 sections

  void prepare(double sample_rate);
  // Clamps. First call after prepare/reset snaps; later calls smooth, including enable/disable
  // (a disabled band glides its mix to pass-through, then stops processing).
  void set(const BandParams& p);
  void reset();
  void process(float* buf, int frames, int slot);  // audio thread: no alloc, no locks
  // Slot s on bufs[s] for s < count (every slot of a Stereo band): same result, runs slot pairs together.
  void process(float* const* bufs, int count, int frames);
  bool settled(int slot) const { return slots_[slot].bypassed; }  // disabled and glided to pass-through

 private:
  // One section as a 2x2 state-space update on the same state (ic1, ic2): the Cytomic maths
  // reassociated for a shorter dependency chain. Only used once the glide has settled.
  struct Steady {
    double c11 = 0, c12 = 0, c21 = 0, c22 = 0, b1 = 0, b2 = 0, d0 = 1, d1 = 0, d2 = 0;
  };
  struct Slot {
    SvfSection cur[kMaxSections];
    double ic1[kMaxSections] = {}, ic2[kMaxSections] = {};
    int settle = 0;         // samples left in the glide; then cur snaps to target
    bool bypassed = false;  // settled at pass-through: process skips the slot
    bool steady = false;    // settled at the target: runs steady_, no smoothing (implies !bypassed)
    void clear_state();
    void heal(int live);  // clears the state if an overflowing input made it non-finite
  };
  void glide(Slot& st, float* buf, int frames);
  template <int N>  // N slots in lockstep: N independent recurrences overlap in the pipeline
  void run_steady(Slot* const* st, float* const* buf, int frames) const;

  double sr_ = 48000, alpha_ = 0;
  int settle_ = 0;  // 20 tau: coefficients within e^-20 of their target
  bool enabled_ = true, snap_ = true;
  int live_ = 0;  // sections processed: max needed since the last snap
  SvfSection target_[kMaxSections];
  Steady steady_[kMaxSections];  // target_ in state-space form
  Slot slots_[kMaxSlots];
};

}  // namespace sorus
