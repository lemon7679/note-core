#include "ink/rasterizer.h"

#include <algorithm>

#include "ink/sdf.h"

namespace ink {

// ---------------------------------------------------------------------------
// Bitmap
// ---------------------------------------------------------------------------

Bitmap::Bitmap(int width, int height, Color fill) : width_(std::max(width, 0)), height_(std::max(height, 0)) {
  px_.resize(static_cast<size_t>(width_) * height_ * 4);
  clear(fill);
}

void Bitmap::clear(Color c) {
  for (size_t i = 0; i < px_.size(); i += 4) {
    px_[i + 0] = c.r * c.a;
    px_[i + 1] = c.g * c.a;
    px_[i + 2] = c.b * c.a;
    px_[i + 3] = c.a;
  }
}

Color Bitmap::colorAt(int x, int y) const {
  const float* p = pixel(x, y);
  if (p[3] <= 0.0f) return Color(0, 0, 0, 0);
  return Color(p[0] / p[3], p[1] / p[3], p[2] / p[3], p[3]);
}

void Bitmap::toRGBA8(uint8_t* out, size_t stride) const {
  for (int y = 0; y < height_; ++y) {
    uint8_t* row = out + static_cast<size_t>(y) * stride;
    for (int x = 0; x < width_; ++x) {
      Color c = colorAt(x, y);
      row[x * 4 + 0] = static_cast<uint8_t>(std::lround(clamp01(c.r) * 255.0f));
      row[x * 4 + 1] = static_cast<uint8_t>(std::lround(clamp01(c.g) * 255.0f));
      row[x * 4 + 2] = static_cast<uint8_t>(std::lround(clamp01(c.b) * 255.0f));
      row[x * 4 + 3] = static_cast<uint8_t>(std::lround(clamp01(c.a) * 255.0f));
    }
  }
}

void Bitmap::toPremultipliedRGBA8(uint8_t* out, size_t stride) const {
  for (int y = 0; y < height_; ++y) {
    uint8_t* row = out + static_cast<size_t>(y) * stride;
    const float* p = pixel(0, y);
    for (int x = 0; x < width_ * 4; x += 4) {
      const float a = clamp01(p[x + 3]);
      // Premultiplied channels can never exceed alpha.
      for (int i = 0; i < 3; ++i)
        row[x + i] = static_cast<uint8_t>(std::lround(clamp(p[x + i], 0.0f, a) * 255.0f));
      row[x + 3] = static_cast<uint8_t>(std::lround(a * 255.0f));
    }
  }
}

Bitmap Bitmap::fromPremultipliedRGBA8(const uint8_t* in, int width, int height, size_t stride) {
  Bitmap b(width, height);
  constexpr float k = 1.0f / 255.0f;
  for (int y = 0; y < b.height_; ++y) {
    const uint8_t* row = in + static_cast<size_t>(y) * stride;
    float* p = b.pixel(0, y);
    for (int x = 0; x < b.width_ * 4; ++x) p[x] = static_cast<float>(row[x]) * k;
  }
  return b;
}

std::vector<uint8_t> Bitmap::toRGBA8() const {
  std::vector<uint8_t> out(static_cast<size_t>(width_) * height_ * 4);
  toRGBA8(out.data(), static_cast<size_t>(width_) * 4);
  return out;
}

// ---------------------------------------------------------------------------
// Coverage
// ---------------------------------------------------------------------------

namespace {

constexpr float kMinRadiusPx = 0.5f;  // thinner ink is drawn at 0.5 px with reduced alpha

struct Box {
  int x0, y0, x1, y1;
  bool empty() const { return x0 >= x1 || y0 >= y1; }
};

Box clipBox(const Rect& r, int x0, int y0, int x1, int y1) {
  Box b;
  b.x0 = std::max(x0, static_cast<int>(std::floor(r.minX)));
  b.y0 = std::max(y0, static_cast<int>(std::floor(r.minY)));
  b.x1 = std::min(x1, static_cast<int>(std::ceil(r.maxX)) + 1);
  b.y1 = std::min(y1, static_cast<int>(std::ceil(r.maxY)) + 1);
  return b;
}

/// Thin-line handling: keeps the perceived weight of hairlines instead of
/// letting them alias into broken dots.
inline void thin(float& r, float& alphaScale) {
  if (r < kMinRadiusPx) {
    alphaScale *= std::max(r, 0.0f) / kMinRadiusPx;
    r = kMinRadiusPx;
  }
}

}  // namespace

void Rasterizer::rasterizeCoverage(const StrokeGeometry& g, const StrokeStyle& style, const Affine& pixelToPage,
                                   const GrainTexture& grain, int x0, int y0, int x1, int y1,
                                   std::vector<float>& mask) {
  const int W = x1 - x0, H = y1 - y0;
  mask.assign(static_cast<size_t>(std::max(W, 0)) * std::max(H, 0), 0.0f);
  if (W <= 0 || H <= 0) return;
  auto at = [&](int x, int y) -> float& { return mask[static_cast<size_t>(y - y0) * W + (x - x0)]; };

  for (const CapsulePrim& c : g.capsules) {
    float r0 = c.r0, r1 = c.r1, k0 = 1.0f, k1 = 1.0f;
    thin(r0, k0);
    thin(r1, k1);
    Rect bb;
    bb.expand(c.p0, r0 + 1.0f);
    bb.expand(c.p1, r1 + 1.0f);
    Box b = clipBox(bb, x0, y0, x1, y1);
    if (b.empty()) continue;
    const float a0 = c.a0 * k0, a1 = c.a1 * k1;
    for (int y = b.y0; y < b.y1; ++y) {
      for (int x = b.x0; x < b.x1; ++x) {
        Vec2 p{x + 0.5f, y + 0.5f};
        float d = sdf::unevenCapsule(p, c.p0, c.p1, r0, r1);
        if (d >= 0.5f) continue;
        float v = sdf::coverage(d) * lerp(a0, a1, segmentParam(p, c.p0, c.p1));
        float& m = at(x, y);
        if (v > m) m = v;
      }
    }
  }

  for (const NibPrim& n : g.nibs) {
    float r = n.round, k = 1.0f;
    thin(r, k);
    float reach = std::max(n.h0, n.h1) + r + 1.0f;
    Rect bb;
    bb.expand(n.p0, reach);
    bb.expand(n.p1, reach);
    Box b = clipBox(bb, x0, y0, x1, y1);
    if (b.empty()) continue;
    for (int y = b.y0; y < b.y1; ++y) {
      for (int x = b.x0; x < b.x1; ++x) {
        Vec2 p{x + 0.5f, y + 0.5f};
        float d = sdf::nibSweep(p, n.p0, n.h0, n.p1, n.h1, n.dir, r);
        if (d >= 0.5f) continue;
        float v = sdf::coverage(d) * lerp(n.a0, n.a1, segmentParam(p, n.p0, n.p1)) * k;
        float& m = at(x, y);
        if (v > m) m = v;
      }
    }
  }

  if (!g.stamps.empty()) {
    // Paper grain is a function of the pixel only: evaluate it once per pixel.
    std::vector<float> grainBuf;
    const bool useGrain = style.grainStrength > 0.0f;
    if (useGrain) {
      grainBuf.resize(mask.size());
      const float inv = 1.0f / std::max(style.grainScale, 1e-3f);
      for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
          Vec2 pp = pixelToPage.apply(Vec2{x + 0.5f, y + 0.5f});
          grainBuf[static_cast<size_t>(y - y0) * W + (x - x0)] = grain.sample(pp.x * inv, pp.y * inv);
        }
    }
    const float hardness = clamp01(style.hardness);
    for (const StampPrim& s : g.stamps) {
      float ra = s.rMajor, rb = s.rMinor, k = 1.0f;
      if (ra < kMinRadiusPx) {
        k *= (ra * ra) / (kMinRadiusPx * kMinRadiusPx);  // area-preserving for tiny dots
        ra = rb = kMinRadiusPx;
      } else if (rb < kMinRadiusPx) {
        k *= rb / kMinRadiusPx;
        rb = kMinRadiusPx;
      }
      Rect bb;
      bb.expand(s.center, ra + 1.0f);
      Box b = clipBox(bb, x0, y0, x1, y1);
      if (b.empty()) continue;
      const Vec2 u = s.axis, v = perp(s.axis);
      // Soft edge in normalized radius units, never narrower than ~1 px.
      const float edge = std::max(1.0f - hardness, 1.0f / std::min(ra, rb));
      const float alpha = s.alpha * k;
      for (int y = b.y0; y < b.y1; ++y) {
        for (int x = b.x0; x < b.x1; ++x) {
          Vec2 d = Vec2{x + 0.5f, y + 0.5f} - s.center;
          float qx = dot(d, u) / ra, qy = dot(d, v) / rb;
          float q2 = qx * qx + qy * qy;
          if (q2 >= 1.0f) continue;
          float f = smoothstep(0.0f, 1.0f, (1.0f - std::sqrt(q2)) / edge);
          float val = f * alpha;
          if (useGrain)
            val *= grainDeposit(grainBuf[static_cast<size_t>(y - y0) * W + (x - x0)], s.depth, style.grainStrength);
          float& m = at(x, y);
          if (val > m) m = val;
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Compositing
// ---------------------------------------------------------------------------

namespace {

void compositeMask(Bitmap& target, const std::vector<float>& mask, int x0, int y0, int x1, int y1,
                   const StrokeStyle& style) {
  const int W = x1 - x0;
  const Color c = style.color;
  const float k = clamp01(style.opacity) * clamp01(c.a);
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      float m = mask[static_cast<size_t>(y - y0) * W + (x - x0)];
      if (m <= 0.0f) continue;
      float a = clamp01(m) * k;
      float* d = target.pixel(x, y);
      if (style.blend == BlendMode::Multiply) {
        // W3C multiply with premultiplied colors.
        float sr = c.r * a, sg = c.g * a, sb = c.b * a;
        float da = d[3];
        d[0] = sr * (1.0f - da) + d[0] * (1.0f - a) + sr * d[0];
        d[1] = sg * (1.0f - da) + d[1] * (1.0f - a) + sg * d[1];
        d[2] = sb * (1.0f - da) + d[2] * (1.0f - a) + sb * d[2];
      } else {
        d[0] = c.r * a + d[0] * (1.0f - a);
        d[1] = c.g * a + d[1] * (1.0f - a);
        d[2] = c.b * a + d[2] * (1.0f - a);
      }
      d[3] = a + d[3] * (1.0f - a);
    }
  }
}

Rect targetRect(const Bitmap& t, const RenderOptions& opts) {
  Rect full(0.0f, 0.0f, static_cast<float>(t.width()), static_cast<float>(t.height()));
  return opts.clip.isEmpty() ? full : opts.clip.intersection(full);
}

void renderLayerStrokes(const Layer& layer, Bitmap& target, const Affine& pageToPixel, const RenderOptions& opts,
                        const Rect& pageClip) {
  std::vector<size_t> visible;
  layer.query(pageClip, visible);
  // "Draw behind" strokes (e.g. highlighter under ink) go first.
  for (int pass = 0; pass < 2; ++pass) {
    for (size_t idx : visible) {
      const Stroke& s = *layer.at(idx);
      if (s.style.drawBehind != (pass == 0)) continue;
      Rasterizer::renderStroke(s, target, pageToPixel, opts);
    }
  }
}

}  // namespace

void Rasterizer::renderGeometry(const StrokeGeometry& pageGeometry, const StrokeStyle& style, Bitmap& target,
                                const Affine& pageToPixel, const RenderOptions& opts) {
  if (pageGeometry.empty()) return;
  StrokeGeometry g = pageGeometry;
  transformGeometry(g, pageToPixel);
  Rect area = g.bounds.inflated(2.0f).intersection(targetRect(target, opts));
  if (area.isEmpty()) return;
  int x0 = std::max(0, static_cast<int>(std::floor(area.minX)));
  int y0 = std::max(0, static_cast<int>(std::floor(area.minY)));
  int x1 = std::min(target.width(), static_cast<int>(std::ceil(area.maxX)));
  int y1 = std::min(target.height(), static_cast<int>(std::ceil(area.maxY)));
  if (x0 >= x1 || y0 >= y1) return;
  thread_local std::vector<float> mask;
  const GrainTexture& grain = opts.grain ? *opts.grain : GrainTexture::shared();
  rasterizeCoverage(g, style, pageToPixel.inverse(), grain, x0, y0, x1, y1, mask);
  compositeMask(target, mask, x0, y0, x1, y1, style);
}

void Rasterizer::renderStroke(const Stroke& stroke, Bitmap& target, const Affine& pageToPixel,
                              const RenderOptions& opts) {
  TessellationOptions t = opts.tessellation;
  t.pixelsPerUnit = pageToPixel.uniformScale();
  StrokeGeometry g;
  Tessellator::tessellate(stroke, t, g);
  renderGeometry(g, stroke.style, target, pageToPixel, opts);
}

void Rasterizer::renderPage(const Page& page, Bitmap& target, const Affine& pageToPixel, const RenderOptions& opts) {
  const Rect clipPx = targetRect(target, opts);
  if (clipPx.isEmpty()) return;
  const int cx0 = static_cast<int>(std::floor(clipPx.minX)), cy0 = static_cast<int>(std::floor(clipPx.minY));
  const int cx1 = static_cast<int>(std::ceil(clipPx.maxX)), cy1 = static_cast<int>(std::ceil(clipPx.maxY));
  if (opts.drawBackground) {
    const Color bg = opts.background;
    for (int y = cy0; y < cy1; ++y)
      for (int x = cx0; x < cx1; ++x) {
        float* d = target.pixel(x, y);
        d[0] = bg.r * bg.a; d[1] = bg.g * bg.a; d[2] = bg.b * bg.a; d[3] = bg.a;
      }
  }
  const Rect pageClip = pageToPixel.inverse().apply(clipPx).inflated(1.0f);

  for (const Layer& layer : page.layers()) {
    if (!layer.visible || layer.opacity <= 0.0f) continue;
    if (layer.opacity >= 0.999f) {
      renderLayerStrokes(layer, target, pageToPixel, opts, pageClip);
      continue;
    }
    // Translucent layer: render in isolation, then fade the whole layer.
    Bitmap tmp(target.width(), target.height());
    renderLayerStrokes(layer, tmp, pageToPixel, opts, pageClip);
    const float o = layer.opacity;
    for (int y = cy0; y < cy1; ++y)
      for (int x = cx0; x < cx1; ++x) {
        const float* s = tmp.pixel(x, y);
        if (s[3] <= 0.0f) continue;
        float* d = target.pixel(x, y);
        float sa = s[3] * o;
        for (int i = 0; i < 3; ++i) d[i] = s[i] * o + d[i] * (1.0f - sa);
        d[3] = sa + d[3] * (1.0f - sa);
      }
  }
}

}  // namespace ink
