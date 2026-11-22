#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "ink/brush.h"
#include "ink/math.h"

namespace ink {

using StrokeId = uint64_t;

enum StrokePointFlags : uint32_t {
  /// Corner: the spline does not round through this point (used by snapped shapes).
  kPointSharp = 1u << 0,
};

/// A processed control point. All brush dynamics (pressure curve, speed,
/// taper, tilt) are already baked in, so rendering is a pure function of the
/// stroke and stays stable when strokes are split, moved or re-loaded.
struct StrokePoint {
  Vec2 pos;
  float width = 1.0f;     ///< Final diameter (page units).
  float alpha = 1.0f;     ///< Final per-point opacity, 0..1.
  float pressure = 0.5f;  ///< Curved pressure 0..1 (drives pencil grain depth).
  float tilt = 0.0f;      ///< Tilt shading amount 0..1 (0 = upright).
  float azimuth = 0.0f;   ///< Radians, page space.
  float time = 0.0f;      ///< Seconds since stroke start.
  uint32_t flags = 0;
};

struct Stroke {
  StrokeId id = 0;
  StrokeStyle style;
  uint32_t seed = 0;  ///< Deterministic randomness (pencil jitter).
  std::vector<StrokePoint> points;
  Rect bounds;  ///< Conservative ink bounds (page units). Call updateBounds() after edits.

  void updateBounds();
  /// Half-extent of the ink footprint around a point of the given width.
  static float inkRadius(const StrokeStyle& style, float width);
  float length() const;
  /// True when the stroke can be drawn straight onto the target without an
  /// isolated coverage pass (opaque, solid, normal blend, vector).
  bool isOpaqueSolid() const;
};

using StrokePtr = std::shared_ptr<const Stroke>;

}  // namespace ink
