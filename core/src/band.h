#pragma once

#include "params.h"

namespace sorus {

// Exact magnitude (dB) of what Band does with `p` at `freq`; 0 for a disabled band.
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
  bool settled(int slot) const { return slots_[slot].bypassed; }  // disabled and glided to pass-through

 private:
  struct Slot {
    SvfSection cur[kMaxSections];
    double ic1[kMaxSections] = {}, ic2[kMaxSections] = {};
    int settle = 0;         // samples left before a disabled band counts as settled
    bool bypassed = false;  // settled at pass-through: process skips the slot
    void clear_state();
  };
  double sr_ = 48000, alpha_ = 0;
  int settle_ = 0;  // 20 tau: coefficients within e^-20 of pass-through
  bool enabled_ = true, snap_ = true;
  int live_ = 0;  // sections processed: max needed since the last snap
  SvfSection target_[kMaxSections];
  Slot slots_[kMaxSlots];
};

}  // namespace sorus
