#include "engine.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "denormals.h"

namespace sorus {

namespace {
constexpr float kMaxIn = 1000;  // +60 dBFS input clamp: preamp + EQ stay inside float range for sane settings
double preamp_or(double db, double keep) { return std::isfinite(db) ? std::clamp(db, -24.0, 24.0) : keep; }
}  // namespace

Engine::Engine(double sample_rate, int max_frames, int channels)
    : max_frames_(std::max(max_frames, 1)),
      channels_(std::clamp(channels, 1, kMaxChannels)),
      mirror_(std::make_unique<ParametricEq>()),
      eq_(std::make_unique<ParametricEq>()) {
  mirror_->prepare(sample_rate, 1, channels_);
  eq_->prepare(sample_rate, max_frames_, channels_);
  lim_.prepare(sample_rate, max_frames_, channels_);
  BandParams off;
  off.enabled = false;  // matches the EQs, which start with every band disabled
  bands_.fill(off);
  latency_ = lim_.latency_frames();  // > 0: detector delay + lookahead
  ramp_len_ = std::max(1, static_cast<int>(std::lround(0.01 * sample_rate)));
  for (int c = 0; c < channels_; ++c) {
    buf_[c].assign(static_cast<size_t>(max_frames_), 0.f);
    dry_[c].assign(static_cast<size_t>(max_frames_), 0.f);
    delay_[c].assign(static_cast<size_t>(latency_), 0.f);
  }
}

bool Engine::stage_band(int index, const BandParams& p) {
  if (index < 0 || index >= kMaxBands || p == bands_[index]) return false;
  bands_[index] = p;
  band_dirty_[index] = true;
  mirror_->set_band(index, p);
  return true;
}

void Engine::set_band(int index, const BandParams& p) {
  if (stage_band(index, p)) update_preamp();
  else flush();  // unchanged: still retries anything pending
}

void Engine::set_preamp_db(double db) {
  preamp_db_ = preamp_or(db, preamp_db_);
  update_preamp();
}

void Engine::set_auto_preamp(bool on) {
  auto_ = on;
  update_preamp();
}

void Engine::set_limiter_ceiling_db(double db) {
  ceiling_db_ = db;
  ceiling_dirty_ = true;
  flush();
}

void Engine::set_bypass(Block b, bool bypassed) {
  const int i = static_cast<int>(b);
  bypass_[i] = bypassed;
  bypass_dirty_[i] = true;
  update_preamp();  // EQ bypass drops the auto offset
}

void Engine::load(const Preset& p) {
  // Stage everything, then one peak-boost evaluation and one flush. Unchanged values send nothing.
  preamp_db_ = preamp_or(p.preamp_db, preamp_db_);
  auto_ = p.auto_preamp;
  if (p.limiter_ceiling_db != ceiling_db_) {
    ceiling_db_ = p.limiter_ceiling_db;
    ceiling_dirty_ = true;
  }
  const int lim = static_cast<int>(Block::Limiter);
  if (bypass_[lim] == p.limiter_enabled) {
    bypass_[lim] = !p.limiter_enabled;
    bypass_dirty_[lim] = true;
  }
  BandParams off;
  off.enabled = false;
  for (int i = 0; i < kMaxBands; ++i) stage_band(i, i < static_cast<int>(p.bands.size()) ? p.bands[i] : off);
  update_preamp();
}

double Engine::eq_response_db(double freq) const { return mirror_->response_db(freq); }

void Engine::update_preamp() {
  const bool offset = auto_ && !bypass_[static_cast<int>(Block::Eq)];
  effective_db_ = preamp_db_ - (offset ? mirror_->peak_boost_db() : 0);
  effective_dirty_ = true;
  flush();
}

bool Engine::flush() {
  using K = Command::Kind;
  for (int i = 0; i < kMaxBands; ++i) {
    if (!band_dirty_[i]) continue;
    // Carries the latest effective preamp (it already counts any band still pending; the last band to
    // land brings the audio side to the matching pair).
    if (!queue_.push({K::Band, i, bands_[i], effective_db_})) return false;
    band_dirty_[i] = false;
    effective_dirty_ = false;
  }
  if (effective_dirty_) {
    if (!queue_.push({K::Preamp, 0, {}, effective_db_})) return false;
    effective_dirty_ = false;
  }
  if (ceiling_dirty_) {
    if (!queue_.push({K::Ceiling, 0, {}, ceiling_db_})) return false;
    ceiling_dirty_ = false;
  }
  for (int i = 0; i < 2; ++i) {
    if (!bypass_dirty_[i]) continue;
    if (!queue_.push({K::Bypass, i, {}, bypass_[i] ? 1.0 : 0.0})) return false;
    bypass_dirty_[i] = false;
  }
  return true;
}

void Engine::apply(const Command& c) {
  switch (c.kind) {
    case Command::Kind::Band:
      eq_->set_band(c.index, c.band);
      gain_.set(std::pow(10.0, c.value / 20), ramp_len_);
      break;
    case Command::Kind::Preamp: gain_.set(std::pow(10.0, c.value / 20), ramp_len_); break;
    case Command::Kind::Ceiling: lim_.set_ceiling_db(c.value); break;
    case Command::Kind::Bypass:
      (c.index == static_cast<int>(Block::Eq) ? eq_mix_ : lim_mix_).set(c.value != 0 ? 0 : 1, ramp_len_);
      break;
  }
}

void Engine::process_interleaved(float* data, int frames) {
  if (!data || frames <= 0) return;
  ScopedFlushDenormals ftz;
  Command c{};
  while (queue_.pop(c)) apply(c);
  const int nc = channels_;
  for (int done = 0; done < frames;) {
    const int n = std::min(max_frames_, frames - done);
    float* io = data + static_cast<std::ptrdiff_t>(done) * nc;
    for (int i = 0; i < n; ++i)
      for (int ch = 0; ch < nc; ++ch) {
        const float v = io[i * nc + ch];
        buf_[ch][i] = std::isfinite(v) ? std::clamp(v, -kMaxIn, kMaxIn) : 0.f;
      }
    run(n);
    for (int i = 0; i < n; ++i)
      for (int ch = 0; ch < nc; ++ch) {
        const float v = buf_[ch][i];  // extreme EQ settings can overflow float: never emit inf/NaN
        io[i * nc + ch] = std::isfinite(v) ? v : 0.f;
      }
    done += n;
  }
}

void Engine::run(int n) {
  const int nc = channels_;
  float* ch[kMaxChannels];
  for (int c = 0; c < nc; ++c) ch[c] = buf_[c].data();

  // Preamp; keep the pre-EQ signal as the EQ's dry path.
  for (int i = 0; i < n; ++i) {
    const float g = static_cast<float>(gain_.next());
    for (int c = 0; c < nc; ++c) dry_[c][i] = ch[c][i] *= g;
  }
  // A settled bypass skips the EQ (ch already is the dry signal); it comes back from clean state.
  if (eq_mix_.off()) {
    eq_idle_ = true;
  } else {
    if (eq_idle_) eq_->restart();
    eq_idle_ = false;
    eq_->process(ch, nc, n);
    for (int i = 0; i < n; ++i) {
      const float m = static_cast<float>(eq_mix_.next());
      for (int c = 0; c < nc; ++c) ch[c][i] = ch[c][i] * m + dry_[c][i] * (1 - m);
    }
  }

  // Limiter dry path: the input delayed by exactly the limiter latency.
  for (int i = 0; i < n; ++i) {
    for (int c = 0; c < nc; ++c) {
      float& slot = delay_[c][delay_pos_];
      dry_[c][i] = slot;
      slot = ch[c][i];
    }
    if (++delay_pos_ == latency_) delay_pos_ = 0;
  }
  if (lim_mix_.off()) {  // settled bypass: the limiter only keeps its input history current
    lim_.skip(ch, nc, n);
    for (int c = 0; c < nc; ++c) std::copy(dry_[c].data(), dry_[c].data() + n, ch[c]);
    return;
  }
  lim_.process(ch, nc, n);
  for (int i = 0; i < n; ++i) {
    const float m = static_cast<float>(lim_mix_.next());
    for (int c = 0; c < nc; ++c) ch[c][i] = ch[c][i] * m + dry_[c][i] * (1 - m);
  }
}

}  // namespace sorus
