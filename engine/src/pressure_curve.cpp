#include "ink/pressure_curve.h"

#include <vector>

namespace ink {

namespace {
inline float bezier1(float c1, float c2, float t) {
  float u = 1.0f - t;
  return 3.0f * u * u * t * c1 + 3.0f * u * t * t * c2 + t * t * t;
}
}  // namespace

PressureCurve::PressureCurve() : PressureCurve(Vec2{1.0f / 3.0f, 1.0f / 3.0f}, Vec2{2.0f / 3.0f, 2.0f / 3.0f}) {}

PressureCurve::PressureCurve(Vec2 c1, Vec2 c2) : c1_(c1), c2_(c2) {
  c1_.x = clamp01(c1_.x); c1_.y = clamp01(c1_.y);
  c2_.x = clamp01(c2_.x); c2_.y = clamp01(c2_.y);
  build();
}

PressureCurve PressureCurve::soft() { return PressureCurve(Vec2{0.12f, 0.40f}, Vec2{0.50f, 0.95f}); }
PressureCurve PressureCurve::firm() { return PressureCurve(Vec2{0.45f, 0.05f}, Vec2{0.80f, 0.60f}); }

void PressureCurve::build() {
  // Sample the parametric curve densely, then resample onto a uniform x grid.
  constexpr int kSamples = 4096;
  std::vector<Vec2> pts(kSamples + 1);
  for (int i = 0; i <= kSamples; ++i) {
    float t = static_cast<float>(i) / kSamples;
    pts[i] = {bezier1(c1_.x, c2_.x, t), bezier1(c1_.y, c2_.y, t)};
  }
  // Enforce monotonic x so that the inversion is well defined.
  for (int i = 1; i <= kSamples; ++i) pts[i].x = std::max(pts[i].x, pts[i - 1].x);
  int j = 0;
  for (int i = 0; i <= 256; ++i) {
    float x = static_cast<float>(i) / 256.0f;
    while (j < kSamples && pts[j + 1].x < x) ++j;
    const Vec2& a = pts[j];
    const Vec2& b = pts[std::min(j + 1, kSamples)];
    float span = b.x - a.x;
    float t = span > 1e-9f ? clamp01((x - a.x) / span) : 0.0f;
    lut_[i] = clamp01(lerp(a.y, b.y, t));
  }
  lut_[0] = clamp01(pts[0].y);
  lut_[256] = 1.0f;
}

float PressureCurve::evaluate(float x) const {
  x = clamp01(x) * 256.0f;
  int i = static_cast<int>(x);
  if (i >= 256) return lut_[256];
  float f = x - static_cast<float>(i);
  return lerp(lut_[i], lut_[i + 1], f);
}

}  // namespace ink
