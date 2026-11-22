#include "ink/spline.h"

namespace ink {

namespace {

struct HermiteSegment {
  Vec2 p1, p2;  // endpoints
  Vec2 m1, m2;  // tangents (already scaled to the [0,1] parameter range)

  Vec2 eval(float t) const {
    float t2 = t * t, t3 = t2 * t;
    float h00 = 2 * t3 - 3 * t2 + 1;
    float h10 = t3 - 2 * t2 + t;
    float h01 = -2 * t3 + 3 * t2;
    float h11 = t3 - t2;
    return p1 * h00 + m1 * h10 + p2 * h01 + m2 * h11;
  }
};

inline float knot(Vec2 a, Vec2 b) {
  // Centripetal: |Δ|^0.5. Clamp to avoid division by zero for coincident points.
  return std::max(std::sqrt(distance(a, b)), 1e-4f);
}

/// Builds the Hermite form of the centripetal Catmull-Rom segment between
/// pts[seg] and pts[seg + 1], synthesizing reflected phantom neighbours at the
/// stroke ends and at sharp corners.
HermiteSegment buildSegment(const StrokePoint* pts, size_t count, size_t seg) {
  const Vec2 p1 = pts[seg].pos;
  const Vec2 p2 = pts[seg + 1].pos;
  const bool sharp1 = (pts[seg].flags & kPointSharp) != 0;
  const bool sharp2 = (pts[seg + 1].flags & kPointSharp) != 0;
  Vec2 p0 = (seg == 0 || sharp1) ? p1 * 2.0f - p2 : pts[seg - 1].pos;
  Vec2 p3 = (seg + 2 >= count || sharp2) ? p2 * 2.0f - p1 : pts[seg + 2].pos;

  HermiteSegment h;
  h.p1 = p1;
  h.p2 = p2;
  if (distanceSq(p1, p2) < 1e-14f) {
    h.m1 = h.m2 = {0.0f, 0.0f};
    return h;
  }
  // Degenerate neighbours (duplicates) fall back to the reflected phantom.
  if (distanceSq(p0, p1) < 1e-14f) p0 = p1 * 2.0f - p2;
  if (distanceSq(p3, p2) < 1e-14f) p3 = p2 * 2.0f - p1;

  float d01 = knot(p0, p1), d12 = knot(p1, p2), d23 = knot(p2, p3);
  // Tangents of the non-uniform Catmull–Rom spline (Barry–Goldman), rescaled to [0,1].
  Vec2 m1 = (p1 - p0) / d01 - (p2 - p0) / (d01 + d12) + (p2 - p1) / d12;
  Vec2 m2 = (p2 - p1) / d12 - (p3 - p1) / (d12 + d23) + (p3 - p2) / d23;
  h.m1 = m1 * d12;
  h.m2 = m2 * d12;
  return h;
}

inline float angleBetween(Vec2 a, Vec2 b) {
  float la = length(a), lb = length(b);
  if (la < 1e-9f || lb < 1e-9f) return 0.0f;
  return std::acos(clamp(dot(a, b) / (la * lb), -1.0f, 1.0f));
}

inline CurveSample interpolate(const StrokePoint& a, const StrokePoint& b, Vec2 pos, float t) {
  CurveSample s;
  s.pos = pos;
  s.width = lerp(a.width, b.width, t);
  s.alpha = lerp(a.alpha, b.alpha, t);
  s.pressure = lerp(a.pressure, b.pressure, t);
  s.tilt = lerp(a.tilt, b.tilt, t);
  s.azimuth = lerpAngle(a.azimuth, b.azimuth, t);
  return s;
}

}  // namespace

CurveSample toCurveSample(const StrokePoint& p) {
  CurveSample s;
  s.pos = p.pos;
  s.width = p.width;
  s.alpha = p.alpha;
  s.pressure = p.pressure;
  s.tilt = p.tilt;
  s.azimuth = p.azimuth;
  return s;
}

Vec2 CatmullRom::evaluate(const StrokePoint* pts, size_t count, size_t seg, float t) {
  if (count == 0) return {};
  if (count == 1 || seg + 1 >= count) return pts[std::min(seg, count - 1)].pos;
  return buildSegment(pts, count, seg).eval(clamp01(t));
}

void CatmullRom::sampleSegment(const StrokePoint* pts, size_t count, size_t seg,
                               const CurveSamplingOptions& opts, bool includeStart,
                               std::vector<CurveSample>& out) {
  if (seg + 1 >= count) return;
  const StrokePoint& a = pts[seg];
  const StrokePoint& b = pts[seg + 1];
  HermiteSegment h = buildSegment(pts, count, seg);

  float chord = distance(a.pos, b.pos);
  float turn = angleBetween(h.m1, h.m2);
  // Bulge of the curve vs. its chord also needs samples (tangent length matters).
  float tangentLen = std::max(length(h.m1), length(h.m2));
  float approxLen = std::max(chord, tangentLen * 0.75f);
  int n = 1;
  if (opts.maxChord > 0.0f) n = std::max(n, static_cast<int>(std::ceil(approxLen / opts.maxChord)));
  if (opts.maxAngle > 0.0f) n = std::max(n, static_cast<int>(std::ceil(turn / opts.maxAngle)));
  n = std::min(std::max(n, 1), std::max(opts.maxSubdivisions, 1));

  if (includeStart) out.push_back(toCurveSample(a));
  for (int i = 1; i < n; ++i) {
    float t = static_cast<float>(i) / static_cast<float>(n);
    out.push_back(interpolate(a, b, h.eval(t), t));
  }
  out.push_back(toCurveSample(b));
}

void CatmullRom::sampleAll(const StrokePoint* pts, size_t count, const CurveSamplingOptions& opts,
                           std::vector<CurveSample>& out) {
  if (count == 0) return;
  if (count == 1) {
    out.push_back(toCurveSample(pts[0]));
    return;
  }
  for (size_t s = 0; s + 1 < count; ++s) sampleSegment(pts, count, s, opts, s == 0, out);
}

}  // namespace ink
