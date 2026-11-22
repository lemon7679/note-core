#pragma once

#include <cstddef>
#include <vector>

#include "ink/stroke.h"

namespace ink {

/// A point on the smooth curve through the control points.
struct CurveSample {
  Vec2 pos;
  float width = 1.0f;
  float alpha = 1.0f;
  float pressure = 0.5f;
  float tilt = 0.0f;
  float azimuth = 0.0f;
};

struct CurveSamplingOptions {
  float maxChord = 2.0f;                  ///< Max distance between samples (page units).
  float maxAngle = 6.0f * kPi / 180.0f;   ///< Max tangent turn per sample (radians).
  int maxSubdivisions = 64;               ///< Per control segment.
};

/// Centripetal Catmull–Rom (alpha = 0.5) through stroke control points.
///
/// Centripetal parameterization never forms cusps or self-loops inside a
/// segment, even with unevenly spaced samples — the property that keeps fast
/// handwriting from developing little knots. Interior points are interpolated
/// exactly, so the ink passes through every (filtered) pen position.
class CatmullRom {
 public:
  /// Number of curve segments for `count` control points (count - 1, min 0).
  static size_t segmentCount(size_t count) { return count > 1 ? count - 1 : 0; }

  /// Appends samples for segment `seg` (from point seg to seg+1).
  /// The start sample (t = 0) is emitted only if `includeStart` is set, so
  /// consecutive segments can be concatenated without duplicates.
  static void sampleSegment(const StrokePoint* pts, size_t count, size_t seg,
                            const CurveSamplingOptions& opts, bool includeStart,
                            std::vector<CurveSample>& out);

  /// Samples the whole curve. A single point yields a single sample.
  static void sampleAll(const StrokePoint* pts, size_t count, const CurveSamplingOptions& opts,
                        std::vector<CurveSample>& out);

  /// Position on segment `seg` at local parameter t in [0, 1].
  static Vec2 evaluate(const StrokePoint* pts, size_t count, size_t seg, float t);
};

CurveSample toCurveSample(const StrokePoint& p);

}  // namespace ink
