#pragma once

#include <cstdint>
#include <deque>
#include <vector>

#include "ink/math.h"

namespace ink {

/// Smoothing factor of a first-order low-pass with the given cutoff (Hz) for a step of dt seconds.
float lowPassAlpha(float cutoffHz, float dt);
/// Smoothing factor of an exponential moving average with time constant tau (s).
float emaAlpha(float tau, float dt);

/// 1€ filter (Casiez, Roussel, Vogel — CHI 2012) for 2D positions.
///
/// An adaptive low-pass: at low speed the cutoff is low (kills jitter where it is
/// visible), at high speed the cutoff rises (kills lag where it matters). This is
/// the standard choice for stylus input because it removes tremor without the
/// "rubber band" lag of a fixed moving average.
class OneEuroFilter2D {
 public:
  struct Params {
    float minCutoff = 4.0f;  ///< Hz. Lower = smoother at slow speed, more lag.
    float beta = 0.05f;      ///< Speed coefficient (per view-pt/s). Higher = less lag when fast.
    float dCutoff = 1.0f;    ///< Hz. Cutoff for the derivative estimate.
  };

  OneEuroFilter2D() = default;
  explicit OneEuroFilter2D(const Params& p) : params_(p) {}

  void setParams(const Params& p) { params_ = p; }
  const Params& params() const { return params_; }
  void reset() { initialized_ = false; }
  bool initialized() const { return initialized_; }

  /// `speedScale` converts the derivative into the units `beta` is tuned for
  /// (e.g. page units -> view points when zoomed).
  Vec2 filter(Vec2 x, float dt, float speedScale = 1.0f);
  Vec2 value() const { return x_; }

 private:
  Params params_;
  bool initialized_ = false;
  Vec2 x_;
  Vec2 dx_;
};

/// Which stabilizer runs after the 1€ filter (ibisPaint-style "stabilization").
enum class StabilizerMode : uint8_t {
  None = 0,
  /// Lazy-nose / pulled string: the ink trails the pen by a fixed string length.
  /// Produces very smooth, deliberate lines. `amount` = string length in view points.
  PulledString = 1,
  /// Time-window weighted moving average. `amount` = window length in milliseconds.
  MovingAverage = 2,
};

struct StabilizerParams {
  StabilizerMode mode = StabilizerMode::None;
  float amount = 0.0f;
  bool catchUp = true;  ///< On pen-up, finish the stroke exactly at the pen position.
};

/// Output sample of the stabilizer, tagged with the input it came from.
struct StabilizedSample {
  Vec2 pos;
  double time = 0.0;
  uint32_t source = 0;  ///< Index of the producing input (caller-defined).
};

class Stabilizer {
 public:
  Stabilizer() = default;
  explicit Stabilizer(const StabilizerParams& p) : params_(p) {}

  void setParams(const StabilizerParams& p) { params_ = p; }
  const StabilizerParams& params() const { return params_; }

  /// `scale` converts view points to page units (string length is defined on screen).
  void reset(Vec2 p, double t, uint32_t source, float unitsPerViewPoint = 1.0f);
  void push(Vec2 p, double t, uint32_t source, std::vector<StabilizedSample>& out);
  /// Drains the stabilizer at pen-up so the stroke reaches the pen.
  void flush(std::vector<StabilizedSample>& out);

 private:
  struct Entry {
    Vec2 pos;
    double time;
    uint32_t source;
  };
  Vec2 averaged() const;

  StabilizerParams params_;
  float unitsPerViewPoint_ = 1.0f;
  Vec2 cursor_;
  Entry last_{};
  bool hasLast_ = false;
  std::deque<Entry> window_;
};

}  // namespace ink
