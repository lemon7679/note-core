#include "ink/stroke.h"

#include <algorithm>

namespace ink {

float Stroke::inkRadius(const StrokeStyle& style, float width) {
  float r = width * 0.5f;
  if (style.method == RenderMethod::Stamp) r *= 1.0f + std::max(0.0f, style.positionJitter);
  return r;
}

void Stroke::updateBounds() {
  bounds = Rect();
  for (const StrokePoint& p : points) bounds.expand(p.pos, inkRadius(style, p.width));
}

float Stroke::length() const {
  float len = 0.0f;
  for (size_t i = 1; i < points.size(); ++i) len += distance(points[i - 1].pos, points[i].pos);
  return len;
}

void Stroke::transformHoles(const Affine& t) {
  const float s = t.uniformScale();
  for (EraseHole& h : holes) {
    h.a = t.apply(h.a);
    h.b = t.apply(h.b);
    h.radius *= s;
  }
}

void Stroke::translateHoles(Vec2 offset) {
  for (EraseHole& h : holes) {
    h.a += offset;
    h.b += offset;
  }
}

void Stroke::pruneHoles() {
  const Rect ink = bounds.inflated(1.0f);
  holes.erase(std::remove_if(holes.begin(), holes.end(), [&](const EraseHole& h) { return !h.bounds().intersects(ink); }),
              holes.end());
}

bool Stroke::isOpaqueSolid() const {
  if (style.method != RenderMethod::Vector || style.blend != BlendMode::Normal) return false;
  if (style.opacity < 1.0f || style.color.a < 1.0f) return false;
  for (const StrokePoint& p : points)
    if (p.alpha < 1.0f) return false;
  return true;
}

}  // namespace ink
