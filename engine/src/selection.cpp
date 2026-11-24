#include "ink/selection.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace ink {

bool pointInPolygon(Vec2 p, const std::vector<Vec2>& poly) {
  bool inside = false;
  for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
    const Vec2& a = poly[i];
    const Vec2& b = poly[j];
    if ((a.y > p.y) != (b.y > p.y)) {
      float x = (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x;
      if (p.x < x) inside = !inside;
    }
  }
  return inside;
}

std::vector<StrokeId> lassoSelect(const Layer& layer, const std::vector<Vec2>& lasso, float minInsideFraction) {
  std::vector<StrokeId> out;
  if (lasso.size() < 3) return out;
  Rect box;
  for (Vec2 p : lasso) box.expand(p);
  std::vector<size_t> candidates;
  layer.query(box, candidates);
  for (size_t idx : candidates) {
    const Stroke& s = *layer.at(idx);
    if (s.points.size() == 1) {
      if (pointInPolygon(s.points[0].pos, lasso)) out.push_back(s.id);
      continue;
    }
    // Length-weighted inside fraction (independent of sampling density).
    float inside = 0.0f, total = 0.0f;
    for (size_t i = 1; i < s.points.size(); ++i) {
      Vec2 a = s.points[i - 1].pos, b = s.points[i].pos;
      float len = distance(a, b);
      total += len;
      if (pointInPolygon((a + b) * 0.5f, lasso)) inside += len;
    }
    if (total <= 0.0f) {
      if (pointInPolygon(s.points[0].pos, lasso)) out.push_back(s.id);
    } else if (inside / total >= minInsideFraction) {
      out.push_back(s.id);
    }
  }
  return out;
}

std::vector<StrokeId> rectSelect(const Layer& layer, const Rect& rect) {
  std::vector<StrokeId> out;
  std::vector<size_t> candidates;
  layer.query(rect, candidates);
  for (size_t idx : candidates) {
    const Stroke& s = *layer.at(idx);
    for (const StrokePoint& p : s.points) {
      if (rect.inflated(p.width * 0.5f).contains(p.pos)) {
        out.push_back(s.id);
        break;
      }
    }
  }
  return out;
}

Rect selectionBounds(const Layer& layer, const std::vector<StrokeId>& ids) {
  Rect r;
  for (StrokeId id : ids) {
    long i = layer.indexOf(id);
    if (i >= 0) r.expand(layer.at(static_cast<size_t>(i))->bounds);
  }
  return r;
}

ChangeSet mapStrokes(Page& page, LayerId layerId, const std::vector<StrokeId>& ids,
                     const std::function<StrokePtr(const StrokePtr&)>& fn) {
  ChangeSet cs;
  Layer* layer = page.layer(layerId);
  if (!layer || layer->locked) return cs;
  std::unordered_set<StrokeId> wanted(ids.begin(), ids.end());
  LayerChange lc;
  lc.layer = layerId;
  for (size_t i = 0; i < layer->size(); ++i) {
    const StrokePtr& s = layer->at(i);
    if (!wanted.count(s->id)) continue;
    StrokePtr repl = fn(s);
    if (repl == s) continue;  // unchanged
    lc.removed.emplace_back(i, s);
    // Index in the post-edit layer: shifted by the net removals before it.
    if (repl) lc.added.emplace_back(i - (lc.removed.size() - lc.added.size() - 1), std::move(repl));
  }
  if (lc.removed.empty()) return cs;
  cs.layers.push_back(std::move(lc));
  applyChange(page, cs);
  return cs;
}

StrokePtr transformedStroke(const Stroke& s, const Affine& t) {
  const float scale = t.uniformScale();
  const float rotation = std::atan2(t.b, t.a);
  auto out = std::make_shared<Stroke>(s);
  for (StrokePoint& p : out->points) {
    p.pos = t.apply(p.pos);
    p.width *= scale;
    p.azimuth = wrapAngle(p.azimuth + rotation);
  }
  out->style.nibAngle = wrapAngle(out->style.nibAngle + rotation);
  out->transformHoles(t);
  out->updateBounds();
  return out;
}

ChangeSet transformStrokes(Page& page, LayerId layer, const std::vector<StrokeId>& ids, const Affine& t) {
  return mapStrokes(page, layer, ids, [&](const StrokePtr& s) { return transformedStroke(*s, t); });
}

ChangeSet transformEach(Page& page, LayerId layer, const std::vector<std::pair<StrokeId, Affine>>& transforms) {
  std::unordered_map<StrokeId, Affine> byId(transforms.begin(), transforms.end());
  std::vector<StrokeId> ids;
  ids.reserve(transforms.size());
  for (const auto& e : transforms) ids.push_back(e.first);
  return mapStrokes(page, layer, ids, [&](const StrokePtr& s) { return transformedStroke(*s, byId.at(s->id)); });
}

ChangeSet recolorStrokes(Page& page, LayerId layer, const std::vector<StrokeId>& ids, Color color) {
  return mapStrokes(page, layer, ids, [&](const StrokePtr& s) {
    auto out = std::make_shared<Stroke>(*s);
    out->style.color = color;
    return StrokePtr(out);
  });
}

ChangeSet deleteStrokes(Page& page, LayerId layer, const std::vector<StrokeId>& ids) {
  return mapStrokes(page, layer, ids, [](const StrokePtr&) { return StrokePtr(); });
}

ChangeSet duplicateStrokes(Page& page, LayerId layerId, const std::vector<StrokeId>& ids, Vec2 offset) {
  ChangeSet cs;
  Layer* layer = page.layer(layerId);
  if (!layer || layer->locked) return cs;
  std::unordered_set<StrokeId> wanted(ids.begin(), ids.end());
  LayerChange lc;
  lc.layer = layerId;
  size_t at = layer->size();
  for (size_t i = 0; i < layer->size(); ++i) {
    const Stroke& s = *layer->at(i);
    if (!wanted.count(s.id)) continue;
    auto copy = std::make_shared<Stroke>(s);
    copy->id = page.allocateStrokeId();
    for (StrokePoint& p : copy->points) p.pos += offset;
    copy->translateHoles(offset);
    copy->updateBounds();
    lc.added.emplace_back(at++, std::move(copy));
  }
  if (lc.added.empty()) return cs;
  cs.layers.push_back(std::move(lc));
  applyChange(page, cs);
  return cs;
}

}  // namespace ink
