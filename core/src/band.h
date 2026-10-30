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
  void set(const BandParams& p);  // clamps; first call after prepare/reset snaps, later calls smooth
  void reset();
  void process(float* buf, int frames, int slot);  // audio thread: no alloc, no locks

 private:
  struct Slot {
    SvfSection cur[kMaxSections];
    double ic1[kMaxSections] = {}, ic2[kMaxSections] = {};
  };
  double sr_ = 48000, alpha_ = 0;
  bool enabled_ = true, snap_ = true;
  int live_ = 0;  // sections processed: max needed since the last snap
  SvfSection target_[kMaxSections];
  Slot slots_[kMaxSlots];
};

}  // namespace sorus
