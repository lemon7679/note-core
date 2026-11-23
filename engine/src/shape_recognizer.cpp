#include "ink/shape_recognizer.h"

#include <numeric>

namespace ink {

const char* shapeTypeName(ShapeType type) {
  switch (type) {
    case ShapeType::None: return "none";
    case ShapeType::Line: return "line";
    case ShapeType::Polyline: return "polyline";
    case ShapeType::Triangle: return "triangle";
    case ShapeType::Rectangle: return "rectangle";
    case ShapeType::Quad: return "quad";
    case ShapeType::Ellipse: return "ellipse";
    case ShapeType::Circle: return "circle";
    case ShapeType::Arc: return "arc";
    case ShapeType::Polygon: return "polygon";
  }
  return "none";
}

namespace {

inline float rad(float degrees) { return degrees * kPi / 180.0f; }
inline float deg(float radians) { return radians * 180.0f / kPi; }
inline float angleOf(Vec2 v) { return std::atan2(v.y, v.x); }

/// Snaps `a` to the nearest multiple of `step` (plus `offset`) when within `tol`.
float snapAngle(float a, float step, float tol, float offset = 0.0f) {
  float s = std::round((a - offset) / step) * step + offset;
  return std::fabs(wrapAngle(a - s)) <= tol ? s : a;
}

Vec2 centroid(const std::vector<Vec2>& v) {
  Vec2 c;
  for (Vec2 q : v) c += q;
  return v.empty() ? c : c / static_cast<float>(v.size());
}

/// Twice the signed area (shoelace); the sign is the winding direction.
float signedArea2(const std::vector<Vec2>& v) {
  float a = 0.0f;
  for (size_t i = 0; i < v.size(); ++i) a += cross(v[i], v[(i + 1) % v.size()]);
  return a;
}

float pathLength(const std::vector<Vec2>& p) {
  float len = 0.0f;
  for (size_t i = 1; i < p.size(); ++i) len += distance(p[i - 1], p[i]);
  return len;
}

/// Uniform arc-length resampling (makes every metric independent of input rate).
std::vector<Vec2> resample(const std::vector<Vec2>& p, size_t n) {
  std::vector<Vec2> out;
  float total = pathLength(p);
  if (p.empty()) return out;
  if (total <= 0.0f || p.size() < 2) return std::vector<Vec2>(n, p[0]);
  float step = total / static_cast<float>(n - 1);
  out.push_back(p[0]);
  float acc = 0.0f;
  size_t i = 1;
  Vec2 prev = p[0];
  while (out.size() < n - 1 && i < p.size()) {
    float d = distance(prev, p[i]);
    if (acc + d >= step && d > 0.0f) {
      float t = (step - acc) / d;
      Vec2 q = lerp(prev, p[i], t);
      out.push_back(q);
      prev = q;
      acc = 0.0f;
    } else {
      acc += d;
      prev = p[i];
      ++i;
    }
  }
  while (out.size() < n) out.push_back(p.back());
  return out;
}

void rdp(const std::vector<Vec2>& p, size_t a, size_t b, float eps, std::vector<size_t>& keep) {
  if (b <= a + 1) return;
  float best = -1.0f;
  size_t idx = a;
  for (size_t i = a + 1; i < b; ++i) {
    float d = pointSegmentDistance(p[i], p[a], p[b]);
    if (d > best) { best = d; idx = i; }
  }
  if (best > eps) {
    rdp(p, a, idx, eps, keep);
    keep.push_back(idx);
    rdp(p, idx, b, eps, keep);
  }
}

/// Moves each vertex to the true corner: the path point farthest from the
/// segment joining its two neighbours. RDP alone may keep a point beside a
/// rounded corner, which makes the adjacent edges look curved.
void refineCorners(const std::vector<Vec2>& p, std::vector<size_t>& idx, bool closed) {
  const size_t n = p.size(), m = idx.size();
  if (m < 3) return;
  for (int pass = 0; pass < 2; ++pass) {
    for (size_t k = closed ? 0 : 1; k < (closed ? m : m - 1); ++k) {
      size_t a = idx[(k + m - 1) % m], b = idx[(k + 1) % m];
      size_t best = idx[k];
      float bestD = -1.0f;
      for (size_t j = (a + 1) % n; j != b; j = (j + 1) % n) {
        float d = pointSegmentDistance(p[j], p[a], p[b]);
        if (d > bestD) { bestD = d; best = j; }
      }
      idx[k] = best;
    }
  }
}

std::vector<Vec2> simplifyOpen(const std::vector<Vec2>& p, float eps) {
  std::vector<size_t> keep{0};
  rdp(p, 0, p.size() - 1, eps, keep);
  keep.push_back(p.size() - 1);
  refineCorners(p, keep, false);
  std::vector<Vec2> out;
  for (size_t k : keep) out.push_back(p[k]);
  return out;
}

/// Closed simplification: split at the point farthest from the start.
std::vector<Vec2> simplifyClosed(const std::vector<Vec2>& p, float eps) {
  size_t far = 0;
  float best = -1.0f;
  for (size_t i = 0; i < p.size(); ++i) {
    float d = distance(p[0], p[i]);
    if (d > best) { best = d; far = i; }
  }
  std::vector<size_t> keep{0};
  rdp(p, 0, far, eps, keep);
  keep.push_back(far);
  rdp(p, far, p.size() - 1, eps, keep);
  // Drop vertices that are (almost) collinear with their neighbours (this also
  // merges the start point into an edge when the stroke started mid-edge).
  bool changed = true;
  while (changed && keep.size() > 3) {
    changed = false;
    for (size_t i = 0; i < keep.size(); ++i) {
      Vec2 prev = p[keep[(i + keep.size() - 1) % keep.size()]];
      Vec2 next = p[keep[(i + 1) % keep.size()]];
      if (pointSegmentDistance(p[keep[i]], prev, next) < eps) {
        keep.erase(keep.begin() + static_cast<long>(i));
        changed = true;
        break;
      }
    }
  }
  refineCorners(p, keep, true);
  std::vector<Vec2> out;
  for (size_t k : keep) out.push_back(p[k]);
  return out;
}

float polylineFitError(const std::vector<Vec2>& pts, const std::vector<Vec2>& poly, bool closed) {
  if (poly.size() < 2) return 1e9f;
  float sum = 0.0f;
  size_t edges = closed ? poly.size() : poly.size() - 1;
  for (Vec2 q : pts) {
    float best = 1e30f;
    for (size_t e = 0; e < edges; ++e)
      best = std::min(best, pointSegmentDistance(q, poly[e], poly[(e + 1) % poly.size()]));
    sum += best;
  }
  return sum / static_cast<float>(pts.size());
}

/// Worst per-edge mean deviation relative to the edge length. A real polygon
/// has straight edges (small value); a circle simplified to 6 vertices does not.
float edgeCurviness(const std::vector<Vec2>& pts, const std::vector<Vec2>& poly, bool closed) {
  size_t edges = closed ? poly.size() : poly.size() - 1;
  std::vector<float> sum(edges, 0.0f), cnt(edges, 0.0f);
  for (Vec2 q : pts) {
    float best = 1e30f;
    size_t be = 0;
    for (size_t e = 0; e < edges; ++e) {
      float d = pointSegmentDistance(q, poly[e], poly[(e + 1) % poly.size()]);
      if (d < best) { best = d; be = e; }
    }
    sum[be] += best;
    cnt[be] += 1.0f;
  }
  float worst = 0.0f;
  for (size_t e = 0; e < edges; ++e) {
    if (cnt[e] <= 0.0f) continue;
    float len = std::max(distance(poly[e], poly[(e + 1) % poly.size()]), 1e-3f);
    worst = std::max(worst, sum[e] / cnt[e] / len);
  }
  return worst;
}

float interiorAngleDeg(Vec2 prev, Vec2 at, Vec2 next) {
  Vec2 a = normalize(prev - at), b = normalize(next - at);
  return deg(std::acos(clamp(dot(a, b), -1.0f, 1.0f)));
}

/// Least-squares rectangle through a near-rectangular quad.
std::vector<Vec2> regularizeRectangle(const std::vector<Vec2>& quad) {
  // Dominant orientation: average of edge angles modulo 90° (angle x4 trick).
  float sx = 0.0f, sy = 0.0f;
  for (size_t i = 0; i < 4; ++i) {
    Vec2 e = quad[(i + 1) % 4] - quad[i];
    float w = length(e);
    float a = angleOf(e) * 4.0f;
    sx += std::cos(a) * w;
    sy += std::sin(a) * w;
  }
  float theta = std::atan2(sy, sx) / 4.0f;
  Vec2 u = fromAngle(theta), v = perp(u);
  float minU = 1e30f, maxU = -1e30f, minV = 1e30f, maxV = -1e30f;
  for (Vec2 q : quad) {
    minU = std::min(minU, dot(q, u)); maxU = std::max(maxU, dot(q, u));
    minV = std::min(minV, dot(q, v)); maxV = std::max(maxV, dot(q, v));
  }
  return {u * minU + v * minV, u * maxU + v * minV, u * maxU + v * maxV, u * minU + v * maxV};
}

/// Least-squares circle (Bullock / Kåsa) through points.
bool fitCircle(const std::vector<Vec2>& p, Vec2& center, float& radius) {
  if (p.size() < 3) return false;
  Vec2 m = centroid(p);
  double suu = 0, suv = 0, svv = 0, suuu = 0, svvv = 0, suvv = 0, svuu = 0;
  for (Vec2 q : p) {
    double u = q.x - m.x, v = q.y - m.y;
    suu += u * u; suv += u * v; svv += v * v;
    suuu += u * u * u; svvv += v * v * v; suvv += u * v * v; svuu += v * u * u;
  }
  double det = suu * svv - suv * suv;
  if (std::fabs(det) < 1e-12) return false;
  double bu = 0.5 * (suuu + suvv), bv = 0.5 * (svvv + svuu);
  double uc = (bu * svv - suv * bv) / det;
  double vc = (suu * bv - suv * bu) / det;
  double r2 = uc * uc + vc * vc + (suu + svv) / static_cast<double>(p.size());
  if (r2 <= 0.0) return false;
  center = m + Vec2{static_cast<float>(uc), static_cast<float>(vc)};
  radius = static_cast<float>(std::sqrt(r2));
  return true;
}

void rotateAbout(std::vector<Vec2>& v, Vec2 c, float a) {
  Affine t = Affine::translate(c.x, c.y) * Affine::rotate(a) * Affine::translate(-c.x, -c.y);
  for (Vec2& q : v) q = t.apply(q);
}

std::vector<Vec2> regularPolygon(Vec2 c, float r, float phase, int n, float winding) {
  std::vector<Vec2> out;
  float step = kTwoPi / static_cast<float>(n) * (winding < 0.0f ? -1.0f : 1.0f);
  for (int k = 0; k < n; ++k) out.push_back(c + fromAngle(phase + step * static_cast<float>(k)) * r);
  return out;
}

/// Phase that puts a flat side at the bottom (page y points down) when within `tol`.
float flatBottomPhase(float phase, int n, float winding, float tol) {
  float step = kTwoPi / static_cast<float>(n) * (winding < 0.0f ? -1.0f : 1.0f);
  // Side midpoints sit at phase + step * (k + 0.5); we want one at +90°.
  float mid = phase + step * 0.5f;
  float sector = kTwoPi / static_cast<float>(n);
  float d = std::fmod(wrapAngle(kPi * 0.5f - mid), sector);
  if (d > sector * 0.5f) d -= sector;
  if (d < -sector * 0.5f) d += sector;
  return std::fabs(d) <= tol ? phase + d : phase;
}

bool polygonIsNearRegular(const std::vector<Vec2>& v, float sideTol, float angleTolDeg) {
  const size_t n = v.size();
  float mean = 0.0f;
  for (size_t i = 0; i < n; ++i) mean += distance(v[i], v[(i + 1) % n]);
  mean /= static_cast<float>(n);
  float ideal = 180.0f * static_cast<float>(n - 2) / static_cast<float>(n);
  for (size_t i = 0; i < n; ++i) {
    if (std::fabs(distance(v[i], v[(i + 1) % n]) - mean) > sideTol * mean) return false;
    if (std::fabs(interiorAngleDeg(v[(i + n - 1) % n], v[i], v[(i + 1) % n]) - ideal) > angleTolDeg) return false;
  }
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------

Vec2 RecognizedShape::anchor() const {
  switch (type) {
    case ShapeType::Line:
    case ShapeType::Polyline:
      return vertices.empty() ? center : vertices.front();
    case ShapeType::Arc:
      return center + fromAngle(startAngle) * radii.x;
    case ShapeType::Ellipse:
    case ShapeType::Circle:
      return center;
    case ShapeType::Triangle:
    case ShapeType::Rectangle:
    case ShapeType::Quad:
    case ShapeType::Polygon:
      return centroid(vertices);
    case ShapeType::None:
      break;
  }
  return center;
}

Vec2 snapLineEnd(Vec2 start, Vec2 end, float snapDeg, float stepDeg) {
  Vec2 d = end - start;
  float len = length(d);
  if (len <= 0.0f || snapDeg <= 0.0f || stepDeg <= 0.0f) return end;
  float a = angleOf(d);
  float s = snapAngle(a, rad(stepDeg), rad(snapDeg));
  return s == a ? end : start + fromAngle(s) * len;
}

Affine dragTransform(Vec2 anchor, Vec2 from, Vec2 to, bool allowRotation, float deadZone) {
  Vec2 a = from - anchor, b = to - anchor;
  float la = length(a), lb = length(b);
  if (la < 1e-4f) return Affine::identity();
  float s = std::max(lb / la, 0.05f);
  float rot = 0.0f;
  if (allowRotation && lb > 1e-4f) {
    rot = std::atan2(cross(a, b), dot(a, b));
    // Soft dead zone: resizing alone never tilts the shape by accident.
    float m = std::max(0.0f, std::fabs(rot) - deadZone);
    rot = rot < 0.0f ? -m : m;
  }
  return Affine::translate(anchor.x, anchor.y) * Affine::rotate(rot) * Affine::scale(s) *
         Affine::translate(-anchor.x, -anchor.y);
}

RecognizedShape transformShape(const RecognizedShape& shape, const Affine& t) {
  RecognizedShape r = shape;
  const float s = t.uniformScale();
  const float rot = std::atan2(t.b, t.a);
  for (Vec2& v : r.vertices) v = t.apply(v);
  r.center = t.apply(shape.center);
  r.radii = shape.radii * s;
  r.rotation = wrapAngle(shape.rotation + rot);
  r.startAngle = wrapAngle(shape.startAngle + rot);
  return r;
}

RecognizedShape regularizeShape(const RecognizedShape& shape, const ShapeRecognizerOptions& opts, bool force) {
  RecognizedShape r = shape;
  const float axisTol = rad(opts.angleSnapDegrees * 2.0f);
  switch (shape.type) {
    case ShapeType::None:
      break;

    case ShapeType::Line:
      if (r.vertices.size() == 2)
        r.vertices[1] = force ? snapLineEnd(r.vertices[0], r.vertices[1], 7.5f, 15.0f)
                              : snapLineEnd(r.vertices[0], r.vertices[1], opts.angleSnapDegrees);
      break;

    case ShapeType::Polyline:
      // Snap each segment near an axis (or to 15° steps when forced), carrying
      // the rest of the polyline along so lengths and later angles are kept.
      for (size_t i = 1; i < r.vertices.size(); ++i) {
        Vec2 snapped = force ? snapLineEnd(r.vertices[i - 1], r.vertices[i], 7.5f, 15.0f)
                             : snapLineEnd(r.vertices[i - 1], r.vertices[i], opts.angleSnapDegrees);
        Vec2 delta = snapped - r.vertices[i];
        for (size_t j = i; j < r.vertices.size(); ++j) r.vertices[j] += delta;
      }
      break;

    case ShapeType::Rectangle: {
      if (r.vertices.size() != 4) break;
      Vec2 e0 = r.vertices[1] - r.vertices[0], e1 = r.vertices[2] - r.vertices[1];
      float w = length(e0), h = length(e1);
      if (force || std::fabs(w - h) <= opts.squareTolerance * std::max(w, h)) {
        float side = 0.5f * (w + h);
        Vec2 c = (r.vertices[0] + r.vertices[2]) * 0.5f;
        Vec2 u = normalize(e0) * (side * 0.5f), v = normalize(e1) * (side * 0.5f);
        r.vertices = {c - u - v, c + u - v, c + u + v, c - u + v};
        r.regular = true;
      }
      break;
    }

    case ShapeType::Triangle: {
      if (r.vertices.size() != 3) break;
      const std::vector<Vec2>& v = r.vertices;
      const float winding = signedArea2(v);
      const Vec2 g = centroid(v);
      if (force || polygonIsNearRegular(v, opts.equilateralTolerance, 12.0f)) {
        float radius = 0.0f;
        for (Vec2 q : v) radius += distance(q, g);
        radius /= 3.0f;
        // Flat side when close (always when forced): vertex angles at 30° + k·60°.
        float phase = snapAngle(angleOf(v[0] - g), rad(60.0f), force ? rad(30.0f) : rad(10.0f), rad(30.0f));
        r.vertices = regularPolygon(g, radius, phase, 3, winding);
        r.regular = true;
        break;
      }
      // Right triangle: make the near-right angle exact.
      size_t ri = 3;
      float bestDiff = 1e9f;
      for (size_t i = 0; i < 3; ++i) {
        float a = interiorAngleDeg(v[(i + 2) % 3], v[i], v[(i + 1) % 3]);
        if (std::fabs(a - 90.0f) < bestDiff) { bestDiff = std::fabs(a - 90.0f); ri = i; }
      }
      if (bestDiff <= 10.0f) {
        Vec2 o = v[ri], pa = v[(ri + 1) % 3], pb = v[(ri + 2) % 3];
        float la = distance(o, pa), lb = distance(o, pb);
        float alpha = angleOf(pa - o), beta = angleOf(pb - o);
        float side = cross(pa - o, pb - o) < 0.0f ? -1.0f : 1.0f;
        // Circular mean of the two leg directions expressed in the leg-a frame.
        float phi = alpha + 0.5f * wrapAngle(beta - side * kPi * 0.5f - alpha);
        phi = snapAngle(phi, kPi * 0.5f, axisTol);
        std::vector<Vec2> out(3);
        out[ri] = o;
        out[(ri + 1) % 3] = o + fromAngle(phi) * la;
        out[(ri + 2) % 3] = o + fromAngle(phi + side * kPi * 0.5f) * lb;
        r.vertices = out;
        break;
      }
      // Otherwise level a near-horizontal side about the centroid.
      for (size_t i = 0; i < 3; ++i) {
        float a = angleOf(v[(i + 1) % 3] - v[i]);
        float s = snapAngle(a, kPi, axisTol);
        if (s != a) {
          rotateAbout(r.vertices, g, wrapAngle(s - a));
          break;
        }
      }
      break;
    }

    case ShapeType::Polygon: {
      const int n = static_cast<int>(r.vertices.size());
      if (n < 5) break;
      if (force || polygonIsNearRegular(r.vertices, opts.regularPolygonTolerance, 14.0f)) {
        const float winding = signedArea2(r.vertices);
        const Vec2 g = centroid(r.vertices);
        float radius = 0.0f;
        for (Vec2 q : r.vertices) radius += distance(q, g);
        radius /= static_cast<float>(n);
        float phase = flatBottomPhase(angleOf(r.vertices[0] - g), n, winding, force ? kPi : rad(10.0f));
        r.vertices = regularPolygon(g, radius, phase, n, winding);
        r.regular = true;
      }
      break;
    }

    case ShapeType::Ellipse:
      if (force) {
        r.type = ShapeType::Circle;
        float rr = 0.5f * (r.radii.x + r.radii.y);
        r.radii = {rr, rr};
        r.rotation = 0.0f;
        r.regular = true;
      }
      break;

    case ShapeType::Circle:
      r.regular = true;
      break;

    case ShapeType::Arc: {
      // Semicircles / quarter circles; level the chord when close.
      float step = force ? kPi * 0.25f : kPi * 0.5f;
      float s = snapAngle(r.sweep, step, force ? step * 0.5f : rad(10.0f));
      if (std::fabs(s) > 1e-3f) r.sweep = s;
      Vec2 a = fromAngle(r.startAngle), b = fromAngle(r.startAngle + r.sweep);
      if (std::fabs(std::fabs(r.sweep) - kTwoPi) > 1e-3f) {
        float chord = angleOf(b - a);
        float sc = snapAngle(chord, kPi * 0.5f, force ? rad(15.0f) : axisTol);
        r.startAngle = wrapAngle(r.startAngle + wrapAngle(sc - chord));
      }
      break;
    }

    case ShapeType::Quad:
      break;
  }
  return r;
}

RecognizedShape recognizeShape(const std::vector<Vec2>& rawPath, const ShapeRecognizerOptions& opts) {
  RecognizedShape none;
  if (rawPath.size() < 2) return none;
  const std::vector<Vec2> p = resample(rawPath, 96);
  Rect box;
  for (Vec2 q : p) box.expand(q);
  float diag = std::sqrt(box.width() * box.width() + box.height() * box.height());
  if (diag < opts.minSize) return none;
  float total = pathLength(p);
  Vec2 start = p.front(), end = p.back();
  float chord = distance(start, end);
  auto finish = [&](RecognizedShape s) { return opts.autoRegularize ? regularizeShape(s, opts, false) : s; };

  // ---- Line ----
  if (chord > 0.0f && chord / total > 0.92f) {
    float maxDev = 0.0f;
    for (Vec2 q : p) maxDev = std::max(maxDev, pointSegmentDistance(q, start, end));
    if (maxDev <= opts.lineTolerance * chord) {
      RecognizedShape s;
      s.type = ShapeType::Line;
      s.vertices = {start, snapLineEnd(start, end, opts.angleSnapDegrees)};
      s.error = maxDev / chord;
      return s;
    }
  }

  const bool closed = chord <= opts.closeTolerance * total && total > 2.0f * diag * 0.9f;
  if (closed) {
    // Drop the closing overlap so the polygon fit sees one lap.
    std::vector<Vec2> loop(p.begin(), p.end() - 1);

    // ---- Polygons: triangle / rectangle / quad / pentagon / hexagon ----
    std::vector<Vec2> poly = simplifyClosed(loop, opts.cornerTolerance * diag);
    if (poly.size() >= 3 && poly.size() <= 6) {
      float err = polylineFitError(p, poly, true) / diag;
      float maxAngle = poly.size() <= 4 ? 150.0f : 165.0f;
      bool sharp = true;
      for (size_t i = 0; i < poly.size(); ++i) {
        float ang = interiorAngleDeg(poly[(i + poly.size() - 1) % poly.size()], poly[i], poly[(i + 1) % poly.size()]);
        if (ang > maxAngle) sharp = false;
      }
      if (sharp && err <= opts.polygonFitError && edgeCurviness(p, poly, true) <= opts.edgeStraightness) {
        RecognizedShape s;
        s.error = err;
        if (poly.size() == 3) {
          s.type = ShapeType::Triangle;
          s.vertices = poly;
          return finish(s);
        }
        if (poly.size() >= 5) {
          s.type = ShapeType::Polygon;
          s.vertices = poly;
          return finish(s);
        }
        bool right = true;
        for (size_t i = 0; i < 4; ++i) {
          float ang = interiorAngleDeg(poly[(i + 3) % 4], poly[i], poly[(i + 1) % 4]);
          if (std::fabs(ang - 90.0f) > opts.rightAngleTolerance) right = false;
        }
        if (right) {
          s.type = ShapeType::Rectangle;
          s.vertices = regularizeRectangle(poly);
          // Snap the orientation to the axes when close.
          Vec2 e = s.vertices[1] - s.vertices[0];
          float ang = angleOf(e);
          float snapped = snapAngle(ang, kPi * 0.5f, rad(opts.angleSnapDegrees * 2.0f));
          if (snapped != ang) rotateAbout(s.vertices, (s.vertices[0] + s.vertices[2]) * 0.5f, wrapAngle(snapped - ang));
        } else {
          s.type = ShapeType::Quad;
          s.vertices = poly;
        }
        return finish(s);
      }
    }

    // ---- Ellipse / circle ----
    Vec2 c = centroid(loop);
    float sxx = 0, sxy = 0, syy = 0;
    for (Vec2 q : loop) {
      Vec2 d = q - c;
      sxx += d.x * d.x; sxy += d.x * d.y; syy += d.y * d.y;
    }
    float theta = 0.5f * std::atan2(2.0f * sxy, sxx - syy);
    Vec2 u = fromAngle(theta), v = perp(u);
    float minU = 1e30f, maxU = -1e30f, minV = 1e30f, maxV = -1e30f;
    for (Vec2 q : loop) {
      Vec2 d = q - c;
      minU = std::min(minU, dot(d, u)); maxU = std::max(maxU, dot(d, u));
      minV = std::min(minV, dot(d, v)); maxV = std::max(maxV, dot(d, v));
    }
    Vec2 center = c + u * ((minU + maxU) * 0.5f) + v * ((minV + maxV) * 0.5f);
    float a = (maxU - minU) * 0.5f, b = (maxV - minV) * 0.5f;
    if (a > 1e-3f && b > 1e-3f) {
      float err = 0.0f;
      for (Vec2 q : loop) {
        Vec2 d = q - center;
        float x = dot(d, u) / a, y = dot(d, v) / b;
        err += std::fabs(std::sqrt(x * x + y * y) - 1.0f);
      }
      err /= static_cast<float>(loop.size());
      if (err <= opts.ellipseFitError) {
        RecognizedShape s;
        s.error = err;
        s.center = center;
        float ratio = std::min(a, b) / std::max(a, b);
        if (ratio >= opts.circleRatio) {
          s.type = ShapeType::Circle;
          float r = 0.5f * (a + b);
          s.radii = {r, r};
          s.regular = true;
        } else {
          s.type = ShapeType::Ellipse;
          s.radii = {a, b};
          s.rotation = snapAngle(theta, kPi * 0.5f, rad(opts.angleSnapDegrees * 2.0f));
        }
        return s;
      }
    }
    return none;
  }

  // ---- Arc ----
  if (opts.allowArc) {
    Vec2 c;
    float r = 0.0f;
    if (fitCircle(p, c, r) && r < diag * 4.0f) {
      float err = 0.0f;
      for (Vec2 q : p) err += std::fabs(distance(q, c) - r);
      err /= static_cast<float>(p.size()) * r;
      if (err <= opts.arcFitError) {
        // Unwrap the swept angle and make sure the pen went round consistently.
        float prev = angleOf(p[0] - c), sweep = 0.0f, forward = 0.0f, backward = 0.0f;
        for (size_t i = 1; i < p.size(); ++i) {
          float a = angleOf(p[i] - c);
          float d = wrapAngle(a - prev);
          sweep += d;
          (d > 0.0f ? forward : backward) += std::fabs(d);
          prev = a;
        }
        float minor = std::min(forward, backward);
        float mag = std::fabs(sweep);
        if (minor < 0.1f * mag && mag >= rad(opts.minArcSweepDegrees) && mag <= rad(330.0f)) {
          RecognizedShape s;
          s.type = ShapeType::Arc;
          s.center = c;
          s.radii = {r, r};
          s.startAngle = angleOf(p[0] - c);
          s.sweep = sweep;
          s.error = err;
          return finish(s);
        }
      }
    }
  }

  // ---- Open polyline (2-3 segments) ----
  if (opts.allowPolyline) {
    std::vector<Vec2> poly = simplifyOpen(p, opts.cornerTolerance * diag);
    if (poly.size() == 3 || poly.size() == 4) {
      float err = polylineFitError(p, poly, false) / diag;
      bool sharp = true;
      for (size_t i = 1; i + 1 < poly.size(); ++i)
        if (interiorAngleDeg(poly[i - 1], poly[i], poly[i + 1]) > 150.0f) sharp = false;
      if (sharp && err <= opts.polygonFitError * 0.8f && edgeCurviness(p, poly, false) <= opts.edgeStraightness) {
        RecognizedShape s;
        s.type = ShapeType::Polyline;
        s.vertices = poly;
        s.error = err;
        return finish(s);
      }
    }
  }
  return none;
}

std::vector<StrokePoint> shapeToStrokePoints(const RecognizedShape& shape, const StrokePoint& templ) {
  std::vector<StrokePoint> out;
  auto make = [&](Vec2 pos, bool sharp) {
    StrokePoint sp = templ;
    sp.pos = pos;
    sp.flags = sharp ? kPointSharp : 0u;
    sp.time = templ.time + 0.001f * static_cast<float>(out.size());
    out.push_back(sp);
  };
  switch (shape.type) {
    case ShapeType::None:
      break;
    case ShapeType::Line:
    case ShapeType::Polyline:
      for (Vec2 v : shape.vertices) make(v, true);
      break;
    case ShapeType::Triangle:
    case ShapeType::Rectangle:
    case ShapeType::Quad:
    case ShapeType::Polygon:
      for (Vec2 v : shape.vertices) make(v, true);
      if (!shape.vertices.empty()) make(shape.vertices.front(), true);
      break;
    case ShapeType::Ellipse:
    case ShapeType::Circle: {
      float perim = kTwoPi * std::sqrt(0.5f * (shape.radii.x * shape.radii.x + shape.radii.y * shape.radii.y));
      int n = static_cast<int>(clamp(perim / 3.0f, 48.0f, 360.0f));
      Vec2 u = fromAngle(shape.rotation), v = perp(u);
      // Start at the top so the (invisible) seam sits where a hand would start.
      float phase = -kPi * 0.5f;
      for (int i = 0; i <= n; ++i) {
        float t = phase + kTwoPi * static_cast<float>(i) / static_cast<float>(n);
        make(shape.center + u * (std::cos(t) * shape.radii.x) + v * (std::sin(t) * shape.radii.y), false);
      }
      break;
    }
    case ShapeType::Arc: {
      float len = std::fabs(shape.sweep) * shape.radii.x;
      int n = static_cast<int>(clamp(len / 2.5f, 8.0f, 360.0f));
      for (int i = 0; i <= n; ++i) {
        float t = shape.startAngle + shape.sweep * static_cast<float>(i) / static_cast<float>(n);
        make(shape.center + fromAngle(t) * shape.radii.x, false);
      }
      break;
    }
  }
  return out;
}

}  // namespace ink
