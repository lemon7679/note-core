#include "ink/eraser.h"

#include <algorithm>
#include <memory>

#include "ink/rasterizer.h"
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

/// Union of eraser holes rasterized over a page-space area: coverage 1 means
/// "certainly erased" (the pixel centre is at least half a pixel inside a hole).
struct HoleGrid {
  Rect area;
  float ppu = 1.0f;
  int w = 0, h = 0;
  std::vector<float> cov;

  void init(const Rect& a, float pixelsPerUnit) {
    area = a;
    ppu = pixelsPerUnit;
    w = std::max(1, static_cast<int>(std::ceil(a.width() * ppu)));
    h = std::max(1, static_cast<int>(std::ceil(a.height() * ppu)));
    cov.assign(static_cast<size_t>(w) * h, 0.0f);
  }
  Vec2 toPixel(Vec2 p) const { return Vec2{(p.x - area.minX) * ppu, (p.y - area.minY) * ppu}; }

  /// Adds (MAX) the holes that touch the area.
  void add(const std::vector<EraseHole>& holes) {
    StrokeGeometry g;
    for (const EraseHole& hole : holes) {
      if (!hole.bounds().intersects(area)) continue;
      CapsulePrim c;
      c.p0 = toPixel(hole.a);
      c.p1 = toPixel(hole.b);
      c.r0 = c.r1 = hole.radius * ppu;
      c.a0 = c.a1 = 1.0f;
      g.capsules.push_back(c);
    }
    if (g.capsules.empty()) return;
    std::vector<float> m;
    Rasterizer::rasterizeCoverage(g, StrokeStyle{}, Affine::identity(), GrainTexture::shared(), 0, 0, w, h, m);
    for (size_t i = 0; i < cov.size(); ++i) cov[i] = std::max(cov[i], m[i]);
  }

  static constexpr float kFull = 0.999f;

  /// True if the disk (page units) lies inside the holes. Disks reaching
  /// outside the grid count as not erased.
  bool diskErased(Vec2 c, float radius) const {
    const Vec2 pc = toPixel(c);
    const float rr = radius * ppu + 0.5f;
    const int x0 = static_cast<int>(std::floor(pc.x - rr)), x1 = static_cast<int>(std::ceil(pc.x + rr));
    const int y0 = static_cast<int>(std::floor(pc.y - rr)), y1 = static_cast<int>(std::ceil(pc.y + rr));
    if (x0 < 0 || y0 < 0 || x1 > w || y1 > h) return false;
    const float rr2 = rr * rr;
    for (int y = y0; y < y1; ++y) {
      const float dy = static_cast<float>(y) + 0.5f - pc.y;
      for (int x = x0; x < x1; ++x) {
        const float dx = static_cast<float>(x) + 0.5f - pc.x;
        if (dx * dx + dy * dy > rr2) continue;
        if (cov[static_cast<size_t>(y) * w + x] < kFull) return false;
      }
    }
    return true;
  }

  /// True if some point of the disk (page units) within `reach` of segment ab
  /// is not erased yet.
  bool diskHasInkUnder(Vec2 c, float radius, Vec2 a, Vec2 b, float reach) const {
    const Vec2 pc = toPixel(c);
    const float rr = radius * ppu + 0.5f;
    const float slack = 0.5f / ppu;
    const int x0 = std::max(0, static_cast<int>(std::floor(pc.x - rr)));
    const int x1 = std::min(w, static_cast<int>(std::ceil(pc.x + rr)));
    const int y0 = std::max(0, static_cast<int>(std::floor(pc.y - rr)));
    const int y1 = std::min(h, static_cast<int>(std::ceil(pc.y + rr)));
    const float rr2 = rr * rr;
    for (int y = y0; y < y1; ++y) {
      const float dy = static_cast<float>(y) + 0.5f - pc.y;
      for (int x = x0; x < x1; ++x) {
        const float dx = static_cast<float>(x) + 0.5f - pc.x;
        if (dx * dx + dy * dy > rr2) continue;
        if (cov[static_cast<size_t>(y) * w + x] >= kFull) continue;
        const Vec2 page{area.minX + (static_cast<float>(x) + 0.5f) / ppu, area.minY + (static_cast<float>(y) + 0.5f) / ppu};
        if (pointSegmentDistance(page, a, b) <= reach + slack) return true;
      }
    }
    return false;
  }
};

/// Hole grid resolution: ~32 pixels across the eraser plus two ink radii.
/// Coarser grids only make the "fully hidden" test more conservative.
float gridPixelsPerUnit(float radius, float maxInk) {
  return std::min(4.0f, 32.0f / std::max(radius + 2.0f * maxInk, 1e-3f));
}

/// One point of the (partly densified) stroke and whether its ink footprint
/// lies entirely under the holes.
struct Item {
  StrokePoint p;
  bool erased = false;
  size_t seg = 0;      ///< Control segment it came from (for the damage area).
  bool dense = false;  ///< Resampled (not an original control point).
};

/// Precise eraser on one stroke: adds the eraser capsule as a hole (so exactly
/// the touched ink disappears, however wide the stroke) and drops the parts of
/// the stroke that are now completely hidden. Returns false if the stroke shows
/// no ink under the eraser (nothing to do).
///
/// `pieces` receives the resulting strokes' points (one entry = only the holes
/// changed), `holesOut` the stroke's new hole list. `changed` is the area whose
/// pixels can differ from before (the new hole, plus re-sampled spline near cuts).
bool eraseStroke(const Stroke& stroke, Vec2 from, Vec2 to, float radius, bool extend,
                 std::vector<std::vector<StrokePoint>>& pieces, std::vector<EraseHole>& holesOut, Rect& changed,
                 bool& holeOnly) {
  holeOnly = false;
  const std::vector<StrokePoint>& pts = stroke.points;
  const size_t n = pts.size();
  pieces.clear();
  if (n == 0) return false;
  for (const EraseHole& hole : stroke.holes)
    if (hole.covers(from, to, radius)) return false;  // already erased here

  float maxInk = 0.0f;
  for (const StrokePoint& p : pts) maxInk = std::max(maxInk, Stroke::inkRadius(stroke.style, p.width));
  auto inkRadius = [&](const StrokePoint& p) { return Stroke::inkRadius(stroke.style, p.width); };

  Rect eraserBox;
  eraserBox.expand(from, radius);
  eraserBox.expand(to, radius);
  if (!stroke.bounds.intersects(eraserBox)) return false;

  // Points whose ink can touch the eraser lie within radius + maxInk of it, so
  // their whole footprint lies within this area.
  const Rect area = eraserBox.inflated(2.0f * maxInk + 0.5f);
  // Grid resolution: see gridPixelsPerUnit.
  const float ppu = gridPixelsPerUnit(radius, maxInk);
  HoleGrid grid;
  grid.init(area, ppu);
  grid.add(stroke.holes);

  // Dense samples where the ink can be near the area; original control points elsewhere.
  CurveSamplingOptions sampling;
  sampling.maxChord = clamp(0.3f * std::min(radius, std::max(maxInk, 0.05f)), 0.05f, 1.5f);
  sampling.maxAngle = 8.0f * kPi / 180.0f;
  sampling.maxSubdivisions = 256;
  std::vector<Item> items;
  std::vector<CurveSample> samples;
  if (n == 1) {
    items.push_back(Item{pts[0], false, 0, false});
  } else {
    items.push_back(Item{pts[0], false, 0, false});
    for (size_t seg = 0; seg + 1 < n; ++seg) {
      const StrokePoint& p0 = pts[seg];
      const StrokePoint& p1 = pts[seg + 1];
      Rect hull;
      hull.expand(p0.pos);
      hull.expand(p1.pos);
      if (seg > 0) hull.expand(pts[seg - 1].pos);
      if (seg + 2 < n) hull.expand(pts[seg + 2].pos);
      if (!hull.inflated(maxInk).intersects(area)) {
        items.push_back(Item{p1, false, seg, false});
        continue;
      }
      samples.clear();
      CatmullRom::sampleSegment(pts.data(), n, seg, sampling, false, samples);
      for (size_t i = 0; i < samples.size(); ++i) {
        const float t = static_cast<float>(i + 1) / static_cast<float>(samples.size());
        Item it{toStrokePoint(samples[i], lerp(p0.time, p1.time, t)), false, seg, true};
        if (i + 1 == samples.size()) {
          it.p.flags = p1.flags;
          it.dense = false;  // the segment's end point (exact control point)
        }
        items.push_back(it);
      }
    }
  }

  // Does the eraser reach ink that still shows?
  bool touches = false;
  for (const Item& it : items) {
    const float ir = inkRadius(it.p);
    if (pointSegmentDistance(it.p.pos, from, to) > radius + ir) continue;
    if (grid.diskHasInkUnder(it.p.pos, ir, from, to, radius)) {
      touches = true;
      break;
    }
  }
  if (!touches) return false;

  // The new hole: stretch the previous one along a straight eraser run.
  holesOut = stroke.holes;
  if (extend && !holesOut.empty() && holesOut.back().b.x == from.x && holesOut.back().b.y == from.y &&
      holesOut.back().radius == radius) {
    holesOut.back().b = to;
  } else {
    holesOut.push_back(EraseHole{from, to, radius});
  }
  const EraseHole& added = holesOut.back();
  grid.add({added});
  changed.expand(added.bounds().inflated(0.5f));

  // Only ink under the new hole can have become hidden.
  for (Item& it : items) {
    const float ir = inkRadius(it.p);
    if (pointSegmentDistance(it.p.pos, added.a, added.b) > added.radius + ir) continue;
    it.erased = grid.diskErased(it.p.pos, ir);
  }
  // Keep erased points next to shown ones (the ink between them is partly
  // visible; the hole masks the rest), drop the others.
  const size_t m = items.size();
  auto keep = [&](size_t i) {
    if (!items[i].erased) return true;
    return (i > 0 && !items[i - 1].erased) || (i + 1 < m && !items[i + 1].erased);
  };
  bool dropped = false;
  for (size_t i = 0; i < m && !dropped; ++i) dropped = !keep(i);
  if (!dropped) {
    pieces.push_back(pts);  // same ink, only a new hole
    holeOnly = true;
    return true;
  }

  std::vector<StrokePoint> cur;
  for (size_t i = 0; i < m; ++i) {
    if (!keep(i)) {
      if (!cur.empty()) pieces.push_back(std::move(cur));
      cur.clear();
      continue;
    }
    cur.push_back(items[i].p);
    // Pieces are re-sampled where the stroke was densified: its spline changes
    // there (Catmull–Rom reaches two control points to each side).
    if (items[i].dense) {
      const size_t seg = items[i].seg;
      const size_t lo = seg >= 2 ? seg - 2 : 0, hi = std::min(n - 1, seg + 3);
      Rect local;
      float width = 0.0f;
      for (size_t k = lo; k <= hi; ++k) {
        local.expand(pts[k].pos);
        width = std::max(width, pts[k].width);
      }
      changed.expand(local.inflated(width + 0.5f));
    }
  }
  if (!cur.empty()) pieces.push_back(std::move(cur));
  return true;
}

/// Stroke eraser on a stroke with holes: true if ink that still shows lies
/// within `radius` of segment ab (touching an erased gap does not count).
bool showsInkNear(const Stroke& stroke, Vec2 a, Vec2 b, float radius) {
  if (stroke.holes.empty()) return true;
  float maxInk = 0.0f;
  for (const StrokePoint& p : stroke.points) maxInk = std::max(maxInk, Stroke::inkRadius(stroke.style, p.width));
  Rect eraserBox;
  eraserBox.expand(a, radius);
  eraserBox.expand(b, radius);
  HoleGrid grid;
  grid.init(eraserBox.inflated(2.0f * maxInk + 0.5f), gridPixelsPerUnit(radius, maxInk));
  grid.add(stroke.holes);
  CurveSamplingOptions sampling = hitSampling(std::min(radius, std::max(maxInk, 0.05f)));
  std::vector<CurveSample> samples;
  CatmullRom::sampleAll(stroke.points.data(), stroke.points.size(), sampling, samples);
  for (const CurveSample& c : samples) {
    const float ir = Stroke::inkRadius(stroke.style, c.width);
    if (pointSegmentDistance(c.pos, a, b) > radius + ir) continue;
    if (grid.diskHasInkUnder(c.pos, ir, a, b, radius)) return true;
  }
  return false;
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

ChangeSet eraseAlong(Page& page, LayerId layerId, Vec2 from, Vec2 to, const EraserOptions& opts, bool extendHoles) {
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
  std::vector<std::vector<StrokePoint>> pieces;
  std::vector<EraseHole> holes;
  Rect damage;

  for (size_t idx : candidates) {
    const StrokePtr& sp = layer->at(idx);
    if ((opts.kindMask & kindBit(sp->style.kind)) == 0) continue;

    if (opts.mode == EraserMode::Stroke) {
      if (!strokeHitsSegment(*sp, from, to, opts.radius, 1.0f) || !showsInkNear(*sp, from, to, opts.radius)) continue;
      lc.removed.emplace_back(idx, sp);
      damage.expand(sp->bounds);
      offset -= 1;
      continue;
    }

    Rect changed;
    bool holeOnly = false;
    if (!eraseStroke(*sp, from, to, opts.radius, extendHoles, pieces, holes, changed, holeOnly)) continue;
    lc.removed.emplace_back(idx, sp);
    size_t base = static_cast<size_t>(static_cast<long>(idx) + offset);
    for (size_t k = 0; k < pieces.size(); ++k) {
      auto piece = std::make_shared<Stroke>();
      piece->id = holeOnly ? sp->id : page.allocateStrokeId();
      piece->style = sp->style;
      piece->seed = holeOnly ? sp->seed : hash32(sp->seed, static_cast<uint32_t>(k + 1));
      piece->points = std::move(pieces[k]);
      piece->holes = holes;
      piece->updateBounds();
      piece->pruneHoles();
      lc.added.emplace_back(base + k, std::move(piece));
    }
    // Pieces look exactly like the original away from `changed`, except pencil
    // stamps (their walk restarts in each piece) and pieces that overlap each
    // other (one stroke's coverage was MAX-combined; separate pieces stack).
    bool local = (holeOnly || sp->style.method == RenderMethod::Vector) && !changed.isEmpty();
    const size_t first = lc.added.size() - pieces.size();
    for (size_t a = first; local && a < lc.added.size(); ++a)
      for (size_t b = a + 1; local && b < lc.added.size(); ++b)
        if (lc.added[a].second->bounds.intersects(lc.added[b].second->bounds)) local = false;
    damage.expand(local ? changed.intersection(sp->bounds.inflated(1.0f)) : sp->bounds);
    offset += static_cast<long>(pieces.size()) - 1;
  }

  if (lc.removed.empty()) return cs;
  cs.damage = damage;
  cs.layers.push_back(std::move(lc));
  applyChange(page, cs);
  return cs;
}

}  // namespace ink
