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
  }
  return "none";
}

namespace {

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

std::vector<Vec2> simplifyOpen(const std::vector<Vec2>& p, float eps) {
  std::vector<size_t> keep{0};
  rdp(p, 0, p.size() - 1, eps, keep);
  keep.push_back(p.size() - 1);
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
  std::vector<Vec2> out;
  for (size_t k : keep) out.push_back(p[k]);
  // Merge the closing vertex with the start if they coincide; also drop
  // vertices that are (almost) collinear with their neighbours.
  bool changed = true;
  while (changed && out.size() > 3) {
    changed = false;
    for (size_t i = 0; i < out.size(); ++i) {
      Vec2 prev = out[(i + out.size() - 1) % out.size()];
      Vec2 next = out[(i + 1) % out.size()];
      if (pointSegmentDistance(out[i], prev, next) < eps) {
        out.erase(out.begin() + static_cast<long>(i));
        changed = true;
        break;
      }
    }
  }
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

float interiorAngleDeg(Vec2 prev, Vec2 at, Vec2 next) {
  Vec2 a = normalize(prev - at), b = normalize(next - at);
  return std::acos(clamp(dot(a, b), -1.0f, 1.0f)) * 180.0f / kPi;
}

/// Least-squares rectangle through a near-rectangular quad.
std::vector<Vec2> regularizeRectangle(const std::vector<Vec2>& quad) {
  // Dominant orientation: average of edge angles modulo 90° (doubled-angle trick, x4).
  float sx = 0.0f, sy = 0.0f;
  for (size_t i = 0; i < 4; ++i) {
    Vec2 e = quad[(i + 1) % 4] - quad[i];
    float w = length(e);
    float a = std::atan2(e.y, e.x) * 4.0f;
    sx += std::cos(a) * w;
    sy += std::sin(a) * w;
  }
  float theta = std::atan2(sy, sx) / 4.0f;
  Vec2 u = fromAngle(theta), v = perp(u);
  // Robust extents: use the quad corners (already denoised by RDP).
  float minU = 1e30f, maxU = -1e30f, minV = 1e30f, maxV = -1e30f;
  for (Vec2 q : quad) {
    minU = std::min(minU, dot(q, u)); maxU = std::max(maxU, dot(q, u));
    minV = std::min(minV, dot(q, v)); maxV = std::max(maxV, dot(q, v));
  }
  std::vector<Vec2> r = {u * minU + v * minV, u * maxU + v * minV, u * maxU + v * maxV, u * minU + v * maxV};
  return r;
}

}  // namespace

Vec2 snapLineEnd(Vec2 start, Vec2 end, float snapDeg) {
  Vec2 d = end - start;
  float len = length(d);
  if (len <= 0.0f || snapDeg <= 0.0f) return end;
  float ang = std::atan2(d.y, d.x);
  float quarter = kPi * 0.5f;
  float snapped = std::round(ang / quarter) * quarter;
  if (std::fabs(wrapAngle(ang - snapped)) <= snapDeg * kPi / 180.0f) return start + fromAngle(snapped) * len;
  return end;
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

    // ---- Polygon (triangle / rectangle / quad) ----
    std::vector<Vec2> poly = simplifyClosed(loop, opts.cornerTolerance * diag);
    if (poly.size() == 3 || poly.size() == 4) {
      float err = polylineFitError(p, poly, true) / diag;
      bool sharp = true;
      for (size_t i = 0; i < poly.size(); ++i) {
        float ang = interiorAngleDeg(poly[(i + poly.size() - 1) % poly.size()], poly[i],
                                     poly[(i + 1) % poly.size()]);
        if (ang > 150.0f) sharp = false;
      }
      if (sharp && err <= opts.polygonFitError) {
        RecognizedShape s;
        s.error = err;
        if (poly.size() == 3) {
          s.type = ShapeType::Triangle;
          s.vertices = poly;
          return s;
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
          float ang = std::atan2(e.y, e.x);
          float snapped = std::round(ang / (kPi * 0.5f)) * (kPi * 0.5f);
          float delta = wrapAngle(snapped - ang);
          if (std::fabs(delta) <= opts.angleSnapDegrees * kPi / 180.0f * 2.0f) {
            Vec2 c = (s.vertices[0] + s.vertices[2]) * 0.5f;
            Affine rot = Affine::translate(c.x, c.y) * Affine::rotate(delta) * Affine::translate(-c.x, -c.y);
            for (Vec2& v : s.vertices) v = rot.apply(v);
          }
        } else {
          s.type = ShapeType::Quad;
          s.vertices = poly;
        }
        return s;
      }
    }

    // ---- Ellipse / circle ----
    Vec2 c;
    for (Vec2 q : loop) c += q;
    c = c / static_cast<float>(loop.size());
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
          s.rotation = 0.0f;
        } else {
          s.type = ShapeType::Ellipse;
          s.radii = {a, b};
          s.rotation = theta;
          // Snap the axis orientation when close to horizontal / vertical.
          float snapped = std::round(theta / (kPi * 0.5f)) * (kPi * 0.5f);
          if (std::fabs(wrapAngle(theta - snapped)) <= opts.angleSnapDegrees * 2.0f * kPi / 180.0f)
            s.rotation = snapped;
        }
        return s;
      }
    }
    return none;
  }

  // ---- Open polyline (2-3 segments) ----
  if (opts.allowPolyline) {
    std::vector<Vec2> poly = simplifyOpen(p, opts.cornerTolerance * diag);
    if (poly.size() == 3 || poly.size() == 4) {
      float err = polylineFitError(p, poly, false) / diag;
      bool sharp = true;
      for (size_t i = 1; i + 1 < poly.size(); ++i)
        if (interiorAngleDeg(poly[i - 1], poly[i], poly[i + 1]) > 150.0f) sharp = false;
      if (sharp && err <= opts.polygonFitError * 0.8f) {
        RecognizedShape s;
        s.type = ShapeType::Polyline;
        s.vertices = poly;
        s.error = err;
        return s;
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
  }
  return out;
}

}  // namespace ink
