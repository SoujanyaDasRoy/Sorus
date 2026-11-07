#pragma once

#include <array>
#include <vector>

#include "band.h"
#include "params.h"

namespace sorus {

// 32 bands with per-band channel routing: Stereo -> every channel, Left/Right -> ch 0/1,
// Mid/Side -> M=(L+R)/2, S=(L-R)/2 of ch 0/1 (decoded back afterwards). With a single channel
// L/R/M/S bands act as Stereo. No internal threading: the engine keeps a separate control-side
// instance for response_db/peak_boost_db.
class ParametricEq {
 public:
  ParametricEq();  // all bands start disabled
  void prepare(double sample_rate, int max_frames, int channels);  // allocates; resets all state
  void set_band(int index, const BandParams& p);  // audio thread: no alloc; unchanged (after clamp): no-op
  void process(float* const* ch, int channels, int frames);        // audio thread: no alloc
  double response_db(double freq) const;  // sum of enabled bands, floored at kFloorDb; control thread only
  // max(0, max response over 10 Hz-min(22 kHz, 0.45 sr) and at each enabled band freq); control thread only
  double peak_boost_db() const;

 private:
  void run(float* const* ch, int channels, int frames);
  bool settled(int i, int channels) const;  // routed slots of band i are at pass-through
  void commit_mode(int i);                  // finish a pending mode switch from clean state

  double sr_ = 48000;
  int max_frames_ = 0;
  std::array<Band, kMaxBands> bands_;
  std::array<BandParams, kMaxBands> params_;  // requested (control view)
  std::array<BandParams, kMaxBands> given_;   // last params handed to the Band (audio view)
  std::array<ChannelMode, kMaxBands> mode_{};   // mode currently routed; lags params_ during a glide
  std::array<bool, kMaxBands> pending_{};       // mode switch waiting for the band to glide out
  int channels_ = 2;
  std::vector<float> mid_, side_;
};

}  // namespace sorus
