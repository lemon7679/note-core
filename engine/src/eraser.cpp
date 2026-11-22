#include "ink/eraser.h"

#include <memory>

#include "ink/spline.h"

namespace ink {

namespace {

CurveSamplingOptions hitSampling(float radius) {
  CurveSamplingOptions o;
  // Samples closer than the eraser radius guarantee the eraser path cannot
  // slip between two samples without touching one of them.
  o.maxChord = clamp(radius * 0.35f, 0.05f, 1.5f);
  o.maxAngle = 8.0f * kPi / 180.0f;
  o.maxSubdivisions = 256;
  return o;
}

StrokePoint toStrokePoint(const CurveSample& s, float time) {
  StrokePoint p;
  p.pos = s.pos;
  p.width = s.width;
  p.alpha = s.alpha;
  p.pressure = s.pressure;
  p.tilt = s.tilt;
  p.azimuth = s.azimuth;
  p.time = time;
  return p;
}

StrokePoint mixPoints(const StrokePoint& a, const StrokePoint& b, float t) {
  StrokePoint p;
  p.pos = lerp(a.pos, b.pos, t);
  p.width = lerp(a.width, b.width, t);
  p.alpha = lerp(a.alpha, b.alpha, t);
  p.pressure = lerp(a.pressure, b.pressure, t);
  p.tilt = lerp(a.tilt, b.tilt, t);
  p.azimuth = lerpAngle(a.azimuth, b.azimuth, t);
  p.time = lerp(a.time, b.time, t);
  return p;
}

struct Cutter {
  Vec2 a, b;
  float radius;
  float influence;

  float margin(const StrokePoint& p) const {
    return pointSegmentDistance(p.pos, a, b) - (radius + 0.5f * p.width * influence);
  }
  bool inside(const StrokePoint& p) const { return margin(p) <= 0.0f; }

  /// Point on [out, in] where the eraser boundary is crossed (bisection).
  StrokePoint boundary(const StrokePoint& out, const StrokePoint& in) const {
    float lo = 0.0f, hi = 1.0f;  // lo: outside, hi: inside
    for (int i = 0; i < 18; ++i) {
      float mid = 0.5f * (lo + hi);
      if (inside(mixPoints(out, in, mid))) hi = mid;
      else lo = mid;
    }
    return mixPoints(out, in, lo);
  }
};

/// Splits `stroke` by the eraser. Returns false if the stroke is untouched.
bool cutStroke(const Stroke& stroke, const Cutter& cut, std::vector<std::vector<StrokePoint>>& pieces) {
  const std::vector<StrokePoint>& pts = stroke.points;
  const size_t n = pts.size();
  pieces.clear();
  if (n == 0) return false;
  if (n == 1) {
    if (!cut.inside(pts[0])) return false;
    return true;  // a dot under the eraser disappears entirely
  }

  const CurveSamplingOptions sampling = hitSampling(cut.radius);
  const Rect eraserBox = [&] {
    Rect r;
    r.expand(cut.a);
    r.expand(cut.b);
    return r;
  }();

  bool touched = false;
  std::vector<StrokePoint> cur;
  std::vector<CurveSample> samples;
  std::vector<StrokePoint> dense;
  auto flush = [&] {
    if (cur.size() >= 2) {
      float len = 0.0f, wsum = 0.0f;
      for (size_t i = 1; i < cur.size(); ++i) len += distance(cur[i - 1].pos, cur[i].pos);
      for (const StrokePoint& p : cur) wsum += p.width;
      // Drop crumbs: a fragment shorter than half its width reads as dirt.
      if (len >= std::max(0.5f * wsum / static_cast<float>(cur.size()), 0.25f)) pieces.push_back(cur);
    }
    cur.clear();
  };

  for (size_t seg = 0; seg + 1 < n; ++seg) {
    const StrokePoint& p0 = pts[seg];
    const StrokePoint& p1 = pts[seg + 1];
    // Cheap reject: segment hull far from the eraser keeps the original control points.
    Rect hull;
    float reach = cut.radius + 0.5f * std::max(p0.width, p1.width) * cut.influence;
    hull.expand(p0.pos);
    hull.expand(p1.pos);
    if (seg > 0) hull.expand(pts[seg - 1].pos);
    if (seg + 2 < n) hull.expand(pts[seg + 2].pos);
    bool maybe = hull.inflated(reach).intersects(eraserBox);

    if (!maybe) {
      if (cur.empty()) cur.push_back(p0);
      cur.push_back(p1);
      continue;
    }

    samples.clear();
    CatmullRom::sampleSegment(pts.data(), n, seg, sampling, true, samples);
    dense.clear();
    for (size_t i = 0; i < samples.size(); ++i) {
      float t = samples.size() > 1 ? static_cast<float>(i) / static_cast<float>(samples.size() - 1) : 0.0f;
      dense.push_back(toStrokePoint(samples[i], lerp(p0.time, p1.time, t)));
    }
    dense.front().flags = p0.flags;
    dense.back().flags = p1.flags;

    bool anyInside = false;
    for (const StrokePoint& d : dense)
      if (cut.inside(d)) { anyInside = true; break; }
    if (!anyInside) {
      if (cur.empty()) cur.push_back(p0);
      cur.push_back(p1);
      continue;
    }
    touched = true;

    bool prevIn = cut.inside(dense[0]);
    if (!prevIn && cur.empty()) cur.push_back(dense[0]);
    for (size_t j = 1; j < dense.size(); ++j) {
      bool in = cut.inside(dense[j]);
      if (!prevIn && in) {
        cur.push_back(cut.boundary(dense[j - 1], dense[j]));
        flush();
      } else if (prevIn && !in) {
        cur.clear();
        cur.push_back(cut.boundary(dense[j], dense[j - 1]));
        cur.push_back(dense[j]);
      } else if (!in) {
        cur.push_back(dense[j]);
      }
      prevIn = in;
    }
  }
  flush();
  return touched;
}

}  // namespace

bool strokeHitsSegment(const Stroke& stroke, Vec2 a, Vec2 b, float radius, float widthInfluence) {
  const std::vector<StrokePoint>& pts = stroke.points;
  if (pts.empty()) return false;
  Rect eraser;
  eraser.expand(a);
  eraser.expand(b);
  if (!stroke.bounds.inflated(radius).intersects(eraser)) return false;
  if (pts.size() == 1)
    return pointSegmentDistance(pts[0].pos, a, b) <= radius + 0.5f * pts[0].width * widthInfluence;

  std::vector<CurveSample> samples;
  CatmullRom::sampleAll(pts.data(), pts.size(), hitSampling(radius), samples);
  for (size_t i = 1; i < samples.size(); ++i) {
    const CurveSample& s0 = samples[i - 1];
    const CurveSample& s1 = samples[i];
    float reach = radius + 0.5f * std::max(s0.width, s1.width) * widthInfluence;
    if (segmentSegmentDistance(s0.pos, s1.pos, a, b) <= reach) return true;
  }
  return false;
}

ChangeSet eraseAlong(Page& page, LayerId layerId, Vec2 from, Vec2 to, const EraserOptions& opts) {
  ChangeSet cs;
  Layer* layer = page.layer(layerId);
  if (!layer || layer->locked || !layer->visible) return cs;

  Rect query;
  query.expand(from);
  query.expand(to);
  query = query.inflated(opts.radius);
  std::vector<size_t> candidates;
  layer->query(query, candidates);
  if (candidates.empty()) return cs;

  LayerChange lc;
  lc.layer = layerId;
  long offset = 0;  // index shift in the post-edit layer caused by earlier replacements
  Cutter cutter{from, to, opts.radius, clamp01(opts.widthInfluence)};
  std::vector<std::vector<StrokePoint>> pieces;

  for (size_t idx : candidates) {
    const StrokePtr& sp = layer->at(idx);
    if ((opts.kindMask & kindBit(sp->style.kind)) == 0) continue;

    if (opts.mode == EraserMode::Stroke) {
      if (!strokeHitsSegment(*sp, from, to, opts.radius, 1.0f)) continue;
      lc.removed.emplace_back(idx, sp);
      offset -= 1;
      continue;
    }

    if (!cutStroke(*sp, cutter, pieces)) continue;
    lc.removed.emplace_back(idx, sp);
    size_t base = static_cast<size_t>(static_cast<long>(idx) + offset);
    for (size_t k = 0; k < pieces.size(); ++k) {
      auto piece = std::make_shared<Stroke>();
      piece->id = page.allocateStrokeId();
      piece->style = sp->style;
      piece->seed = hash32(sp->seed, static_cast<uint32_t>(k + 1));
      piece->points = std::move(pieces[k]);
      piece->updateBounds();
      lc.added.emplace_back(base + k, std::move(piece));
    }
    offset += static_cast<long>(pieces.size()) - 1;
  }

  if (lc.removed.empty()) return cs;
  cs.layers.push_back(std::move(lc));
  applyChange(page, cs);
  return cs;
}

}  // namespace ink
