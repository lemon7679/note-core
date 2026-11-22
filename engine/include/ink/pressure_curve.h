#pragma once

#include <array>

#include "ink/math.h"

namespace ink {

/// Pressure response curve: a cubic Bézier from (0,0) to (1,1) with two control
/// points — the same model Procreate / ibisPaint expose as "pressure curve".
/// Evaluated through a 256-segment lookup table, so it is cheap per sample.
class PressureCurve {
 public:
  PressureCurve();  ///< Linear.
  PressureCurve(Vec2 c1, Vec2 c2);

  static PressureCurve linear() { return PressureCurve(); }
  /// Default curve tuned for writing: light touches still produce ink,
  /// typical writing pressure (~0.25-0.4 normalized force) lands mid-range.
  static PressureCurve soft();
  static PressureCurve firm();

  float evaluate(float x) const;
  Vec2 control1() const { return c1_; }
  Vec2 control2() const { return c2_; }

 private:
  void build();
  Vec2 c1_, c2_;
  std::array<float, 257> lut_{};
};

}  // namespace ink
