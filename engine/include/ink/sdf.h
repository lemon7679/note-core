#pragma once

// Signed-distance functions shared by the CPU rasterizer. The Metal shader
// (shaders/ink_stroke.metal) implements exactly the same math so GPU and CPU
// output match pixel for pixel (within float precision).

#include "ink/math.h"

namespace ink {
namespace sdf {

/// Exact distance to a tapered capsule: circle (pa, ra) swept to circle (pb, rb)
/// (Inigo Quilez, "uneven capsule"). Negative inside.
inline float unevenCapsule(Vec2 p, Vec2 pa, Vec2 pb, float ra, float rb) {
  Vec2 ba = pb - pa;
  float h = dot(ba, ba);
  float rd = ra - rb;
  if (h <= rd * rd + 1e-12f) {
    // One end circle contains the other (or both coincide).
    return std::min(length(p - pa) - ra, length(p - pb) - rb);
  }
  Vec2 q0 = p - pa;
  Vec2 q{dot(q0, Vec2{ba.y, -ba.x}) / h, dot(q0, ba) / h};
  q.x = std::fabs(q.x);
  Vec2 c{std::sqrt(h - rd * rd), rd};
  float k = cross(c, q);
  float m = dot(c, q);
  float n = dot(q, q);
  if (k < 0.0f) return std::sqrt(h * n) - ra;
  if (k > c.x) return std::sqrt(h * (n + 1.0f - 2.0f * q.y)) - rb;
  return m - ra;
}

/// Exact signed distance to a quadrilateral (any simple / degenerate quad).
inline float quad(Vec2 p, const Vec2 v[4]) {
  float d = dot(p - v[0], p - v[0]);
  float s = 1.0f;
  for (int i = 0, j = 3; i < 4; j = i, ++i) {
    Vec2 e = v[j] - v[i];
    Vec2 w = p - v[i];
    float ee = dot(e, e);
    float t = ee > 1e-20f ? clamp01(dot(w, e) / ee) : 0.0f;
    Vec2 b = w - e * t;
    d = std::min(d, dot(b, b));
    bool c1 = p.y >= v[i].y, c2 = p.y < v[j].y, c3 = e.x * w.y > e.y * w.x;
    if ((c1 && c2 && c3) || (!c1 && !c2 && !c3)) s = -s;
  }
  return s * std::sqrt(d);
}

/// Flat nib of half-length h (plus rounding r) along `dir`, swept p0 -> p1.
inline float nibSweep(Vec2 p, Vec2 p0, float h0, Vec2 p1, float h1, Vec2 dir, float r) {
  Vec2 v[4] = {p0 - dir * h0, p1 - dir * h1, p1 + dir * h1, p0 + dir * h0};
  return quad(p, v) - r;
}

/// Box-filter coverage of a pixel by a shape with signed distance d (pixels).
inline float coverage(float d) { return clamp01(0.5f - d); }

}  // namespace sdf
}  // namespace ink
