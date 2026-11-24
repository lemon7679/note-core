#include "ink/rasterizer.h"

#include <algorithm>
#include <cmath>
#include <limits>

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
//
// Every primitive is convex, so on each pixel row it covers one interval. The
// rasterizer walks rows and, per row, computes two intervals analytically:
//
//   outer  — pixels that can have any coverage (shape inflated by 0.5 px)
//   inner  — pixels certainly fully covered (shape eroded by 0.5 px)
//
// Inner pixels are filled directly; only the thin anti-aliased band between the
// two evaluates the exact SDF. Work is proportional to the covered area plus
// the perimeter, instead of (bounding box x primitives): wide strokes such as a
// highlighter at high zoom used to evaluate the quad SDF for hundreds of
// overlapping nib sweeps per pixel.
// ---------------------------------------------------------------------------

namespace {

constexpr float kMinRadiusPx = 0.5f;  // thinner ink is drawn at 0.5 px with reduced alpha
constexpr float kInf = std::numeric_limits<float>::infinity();

struct Box {
  int x0, y0, x1, y1;
  bool empty() const { return x0 >= x1 || y0 >= y1; }
  int width() const { return x1 - x0; }
};

/// Thin-line handling: keeps the perceived weight of hairlines instead of
/// letting them alias into broken dots.
inline void thin(float& r, float& alphaScale) {
  if (r < kMinRadiusPx) {
    alphaScale *= std::max(r, 0.0f) / kMinRadiusPx;
    r = kMinRadiusPx;
  }
}

/// Convex polygon with up to four vertices (a point, a segment or a trapezoid),
/// with per-edge data precomputed for fast horizontal slicing.
struct ConvexPoly {
  Vec2 v[4];
  int n = 0;
  float minY = 0, maxY = 0;
  struct Edge {
    float y0, y1;   // y0 <= y1
    float x0, x1;   // x at y0 / y1
    float dxdy;     // 0 for horizontal edges
    bool flat;
  };
  Edge e[4];
  int ne = 0;

  void finish() {
    minY = maxY = v[0].y;
    for (int i = 1; i < n; ++i) {
      minY = std::min(minY, v[i].y);
      maxY = std::max(maxY, v[i].y);
    }
    ne = 0;
    const int edges = n == 2 ? 1 : n;  // a segment has one edge
    for (int i = 0; i < edges; ++i) {
      Vec2 a = v[i], b = v[(i + 1) % n];
      if (a.y > b.y) std::swap(a, b);
      Edge& ed = e[ne++];
      ed.y0 = a.y;
      ed.y1 = b.y;
      ed.x0 = a.x;
      ed.x1 = b.x;
      ed.flat = (b.y - a.y) < 1e-12f;
      ed.dxdy = ed.flat ? 0.0f : (b.x - a.x) / (b.y - a.y);
    }
    if (n == 1) {
      e[0] = Edge{v[0].y, v[0].y, v[0].x, v[0].x, 0.0f, true};
      ne = 1;
    }
  }

  /// x-extent on the line y (exact for convex polygons).
  bool slice(float y, float& L, float& R) const {
    if (y < minY || y > maxY) return false;
    L = kInf;
    R = -kInf;
    for (int i = 0; i < ne; ++i) {
      const Edge& ed = e[i];
      if (y < ed.y0 || y > ed.y1) continue;
      if (ed.flat) {
        L = std::min(L, std::min(ed.x0, ed.x1));
        R = std::max(R, std::max(ed.x0, ed.x1));
      } else {
        const float x = ed.x0 + (y - ed.y0) * ed.dxdy;
        L = std::min(L, x);
        R = std::max(R, x);
      }
    }
    return L <= R;
  }

  /// x-extent of the part of the polygon inside the band [y0, y1].
  bool band(float y0, float y1, float& L, float& R) const {
    if (y1 < minY || y0 > maxY) return false;
    L = kInf;
    R = -kInf;
    for (int i = 0; i < n; ++i)
      if (v[i].y >= y0 && v[i].y <= y1) {
        L = std::min(L, v[i].x);
        R = std::max(R, v[i].x);
      }
    float l, r;
    if (slice(y0, l, r)) { L = std::min(L, l); R = std::max(R, r); }
    if (slice(y1, l, r)) { L = std::min(L, l); R = std::max(R, r); }
    return L <= R;
  }
};

/// First / last pixel index whose centre lies in [lo, hi], clamped to [a, b].
inline void pixelRange(float lo, float hi, int a, int b, int& first, int& last) {
  if (!(lo <= hi)) {
    first = 1;
    last = 0;
    return;
  }
  first = static_cast<int>(std::ceil(std::max(lo - 0.5f, static_cast<float>(a) - 1.0f)));
  last = static_cast<int>(std::floor(std::min(hi - 0.5f, static_cast<float>(b) + 1.0f)));
  first = std::max(first, a);
  last = std::min(last, b);
}

/// Rasterizes a convex shape with MAX accumulation. The shape must contain
/// P ⊕ disk(rIn) and be contained in P ⊕ disk(rOut); `distance` is its exact signed
/// distance and `alphaAt` the opacity at a pixel centre (never above amax).
template <typename Sdf, typename AlphaAt>
void rasterConvex(const ConvexPoly& P, float rOut, float rIn, float amax, const Box& b, float* mask, Sdf distance,
                  AlphaAt alphaAt) {
  const float rOuter = rOut + 0.5f;  // coverage > 0 only within this distance of P
  const float rho = rIn - 0.5f;      // full coverage within this distance of P
  const int ya = std::max(b.y0, static_cast<int>(std::floor(P.minY - rOuter)));
  const int yb = std::min(b.y1, static_cast<int>(std::ceil(P.maxY + rOuter)) + 1);
  const int W = b.width();
  // Inner interval: hull of horizontal chords of the disk at a few heights
  // (each chord swept along P lies inside P ⊕ disk(rho), so does their hull).
  static constexpr int kChords = 5;
  static constexpr float kDy[kChords] = {0.0f, 0.6f, -0.6f, 0.92f, -0.92f};
  static constexpr float kDx[kChords] = {1.0f, 0.8f, 0.8f, 0.392f, 0.392f};
  for (int y = ya; y < yb; ++y) {
    const float yc = static_cast<float>(y) + 0.5f;
    float oL, oR;
    if (!P.band(yc - rOuter, yc + rOuter, oL, oR)) continue;
    int xa, xb;
    pixelRange(oL - rOuter, oR + rOuter, b.x0, b.x1 - 1, xa, xb);
    if (xa > xb) continue;
    float iL = kInf, iR = -kInf;
    if (rho >= 0.0f) {
      for (int k = 0; k < kChords; ++k) {
        float l, r;
        if (P.slice(yc + kDy[k] * rho, l, r)) {
          iL = std::min(iL, l - kDx[k] * rho);
          iR = std::max(iR, r + kDx[k] * rho);
        }
      }
    } else {
      // Thin shape: pixels whose whole ±m square lies inside P.
      const float m = -rho;
      float l0, r0, l1, r1;
      if (P.slice(yc - m, l0, r0) && P.slice(yc + m, l1, r1)) {
        iL = std::max(l0, l1) + m;
        iR = std::min(r0, r1) - m;
      }
    }
    int ia, ib;
    pixelRange(iL, iR, xa, xb, ia, ib);
    float* row = mask + static_cast<size_t>(y - b.y0) * W - b.x0;
    for (int x = xa; x <= xb; ++x) {
      float& m = row[x];
      if (m >= amax) continue;  // MAX: nothing to add
      const Vec2 p{static_cast<float>(x) + 0.5f, yc};
      float v;
      if (x >= ia && x <= ib) {
        v = alphaAt(p);
      } else {
        const float d = distance(p);
        if (d >= 0.5f) continue;
        v = sdf::coverage(d) * alphaAt(p);
      }
      if (v > m) m = v;
    }
  }
}

void rasterCapsule(CapsulePrim c, const Box& b, float* mask) {
  float k0 = 1.0f, k1 = 1.0f;
  thin(c.r0, k0);
  thin(c.r1, k1);
  const float a0 = c.a0 * k0, a1 = c.a1 * k1;
  const float amax = std::max(a0, a1);
  if (amax <= 0.0f) return;
  // The tapered capsule (hull of the two end circles) lies between the
  // stadiums of radius min(r0, r1) and max(r0, r1) around its axis.
  ConvexPoly P;
  P.v[0] = c.p0;
  P.v[1] = c.p1;
  P.n = 2;
  P.finish();
  auto sdfFn = [&](Vec2 p) { return sdf::unevenCapsule(p, c.p0, c.p1, c.r0, c.r1); };
  const float rOut = std::max(c.r0, c.r1), rIn = std::min(c.r0, c.r1);
  if (a0 == a1) {
    rasterConvex(P, rOut, rIn, amax, b, mask, sdfFn, [a0](Vec2) { return a0; });
  } else {
    rasterConvex(P, rOut, rIn, amax, b, mask, sdfFn,
                 [&](Vec2 p) { return lerp(a0, a1, segmentParam(p, c.p0, c.p1)); });
  }
}

void rasterNib(NibPrim n, const Box& b, float* mask) {
  float r = n.round, k = 1.0f;
  thin(r, k);
  const float a0 = n.a0 * k, a1 = n.a1 * k;
  const float amax = std::max(a0, a1);
  if (amax <= 0.0f) return;
  ConvexPoly P;
  P.v[0] = n.p0 - n.dir * n.h0;
  P.v[1] = n.p1 - n.dir * n.h1;
  P.v[2] = n.p1 + n.dir * n.h1;
  P.v[3] = n.p0 + n.dir * n.h0;
  P.n = 4;
  P.finish();
  auto sdfFn = [&](Vec2 p) { return sdf::nibSweep(p, n.p0, n.h0, n.p1, n.h1, n.dir, r); };
  if (a0 == a1) {
    rasterConvex(P, r, r, amax, b, mask, sdfFn, [a0](Vec2) { return a0; });
  } else {
    rasterConvex(P, r, r, amax, b, mask, sdfFn, [&](Vec2 p) { return lerp(a0, a1, segmentParam(p, n.p0, n.p1)); });
  }
}

/// Mean of the stamp falloff w(q) = smoothstep(0, 1, (1 - q) / edge) over q in [0, 1].
inline float meanFalloff(float edge) {
  if (edge <= 1.0f) return 1.0f - 0.5f * edge;
  const float x = 1.0f / edge;
  return edge * (x * x * x - 0.5f * x * x * x * x);
}

/// Pencil: graphite accumulates. Each stamp adds optical density λ·w·deposit;
/// the stroke's coverage is 1 - exp(-density) (Beer–Lambert). Along the path
/// the stamps overlap ~n times, and λ is chosen so the centre of a straight
/// line reaches exactly the stamp's alpha. Unlike a MAX of hard stamps this
/// gives soft, rounded edges and a velvety tone that builds up where the lead
/// passes again (loops, scribbles), like real graphite.
void accumulateStamp(StampPrim s, const StrokeStyle& style, const Box& b, float* density, const float* grainBuf) {
  float ra = s.rMajor, rb = s.rMinor, k = 1.0f;
  const float spacing = std::max(2.0f * s.rMajor * std::max(style.spacing, 0.01f), 1e-3f);
  if (ra < kMinRadiusPx) {
    k *= (ra * ra) / (kMinRadiusPx * kMinRadiusPx);  // area-preserving for tiny dots
    ra = rb = kMinRadiusPx;
  } else if (rb < kMinRadiusPx) {
    k *= rb / kMinRadiusPx;
    rb = kMinRadiusPx;
  }
  const float a = clamp(s.alpha * k, 0.0f, 0.985f);
  if (a <= 0.0f) return;
  const float hardness = clamp01(style.hardness);
  // Soft edge in normalized radius units, never narrower than ~1 px.
  const float edge = std::max(1.0f - hardness, 1.0f / std::min(ra, rb));
  const float overlaps = std::max(1.0f, 2.0f * std::sqrt(ra * rb) / spacing * meanFalloff(edge));
  const float lambda = -std::log1p(-a) / overlaps;

  const Vec2 u = s.axis, v = perp(s.axis);
  // Implicit ellipse q(d) = A dx² + 2B dx dy + C dy².
  const float ia2 = 1.0f / (ra * ra), ib2 = 1.0f / (rb * rb);
  const float A = u.x * u.x * ia2 + v.x * v.x * ib2;
  const float B = u.x * u.y * ia2 + v.x * v.y * ib2;
  const float C = u.y * u.y * ia2 + v.y * v.y * ib2;
  const float halfH = std::sqrt(ra * ra * u.y * u.y + rb * rb * v.y * v.y);
  const int ya = std::max(b.y0, static_cast<int>(std::floor(s.center.y - halfH)));
  const int yb = std::min(b.y1, static_cast<int>(std::ceil(s.center.y + halfH)) + 1);
  const int W = b.width();
  const bool useGrain = grainBuf != nullptr;
  const float invEdge = 1.0f / edge;
  for (int y = ya; y < yb; ++y) {
    const float dy = static_cast<float>(y) + 0.5f - s.center.y;
    // A dx² + 2B dy dx + (C dy² - 1) < 0
    const float disc = B * B * dy * dy - A * (C * dy * dy - 1.0f);
    if (disc <= 0.0f) continue;
    const float sq = std::sqrt(disc);
    const float dx0 = (-B * dy - sq) / A, dx1 = (-B * dy + sq) / A;
    int xa, xb;
    pixelRange(s.center.x + dx0, s.center.x + dx1, b.x0, b.x1 - 1, xa, xb);
    const size_t rowOff = static_cast<size_t>(y - b.y0) * W;
    for (int x = xa; x <= xb; ++x) {
      const float dx = static_cast<float>(x) + 0.5f - s.center.x;
      const float q2 = A * dx * dx + 2.0f * B * dx * dy + C * dy * dy;
      if (q2 >= 1.0f) continue;
      float w = smoothstep(0.0f, 1.0f, (1.0f - std::sqrt(q2)) * invEdge);
      const size_t i = rowOff + static_cast<size_t>(x - b.x0);
      if (useGrain) w *= grainDeposit(grainBuf[i], s.depth, style.grainStrength);
      density[i] += lambda * w;
    }
  }
}

/// Page-space bounds of a primitive (cheap cull before transforming it).
inline Rect primBounds(const CapsulePrim& c) {
  Rect r;
  r.expand(c.p0, c.r0);
  r.expand(c.p1, c.r1);
  return r;
}
inline Rect primBounds(const NibPrim& n) {
  Rect r;
  const float reach0 = n.h0 + n.round, reach1 = n.h1 + n.round;
  r.expand(n.p0, reach0);
  r.expand(n.p1, reach1);
  return r;
}
inline Rect primBounds(const StampPrim& s) {
  Rect r;
  r.expand(s.center, s.rMajor);
  return r;
}

inline CapsulePrim mapPrim(CapsulePrim c, const Affine& t, float s) {
  c.p0 = t.apply(c.p0);
  c.p1 = t.apply(c.p1);
  c.r0 *= s;
  c.r1 *= s;
  return c;
}
inline NibPrim mapPrim(NibPrim n, const Affine& t, float s) {
  n.p0 = t.apply(n.p0);
  n.p1 = t.apply(n.p1);
  n.h0 *= s;
  n.h1 *= s;
  n.round *= s;
  n.dir = normalize(t.applyVector(n.dir));
  return n;
}
inline StampPrim mapPrim(StampPrim p, const Affine& t, float s) {
  p.center = t.apply(p.center);
  p.axis = normalize(t.applyVector(p.axis));
  p.rMajor *= s;
  p.rMinor *= s;
  return p;
}

/// Precise-eraser holes: buf *= (1 - hole coverage), hole by hole (the GPU
/// renderer blends the same product). Pixels fully inside a hole become 0.
void applyHoles(const std::vector<CapsulePrim>& holes, const Affine& toPixel, bool identity, float s, const Rect& cull,
                const Box& b, float* buf) {
  if (holes.empty()) return;
  const int W = b.width();
  thread_local std::vector<float> hm;
  for (const CapsulePrim& h : holes) {
    if (!primBounds(h).intersects(cull)) continue;
    CapsulePrim c = identity ? h : mapPrim(h, toPixel, s);
    float r = c.r0, k = 1.0f;
    thin(r, k);
    // Hole coverage only inside the capsule's pixel box.
    const float reach = r + 1.0f;
    Box hb;
    hb.x0 = std::max(b.x0, static_cast<int>(std::floor(std::min(c.p0.x, c.p1.x) - reach)));
    hb.y0 = std::max(b.y0, static_cast<int>(std::floor(std::min(c.p0.y, c.p1.y) - reach)));
    hb.x1 = std::min(b.x1, static_cast<int>(std::ceil(std::max(c.p0.x, c.p1.x) + reach)));
    hb.y1 = std::min(b.y1, static_cast<int>(std::ceil(std::max(c.p0.y, c.p1.y) + reach)));
    if (hb.empty()) continue;
    const int HW = hb.width();
    hm.assign(static_cast<size_t>(HW) * (hb.y1 - hb.y0), 0.0f);
    rasterCapsule(c, hb, hm.data());
    for (int y = hb.y0; y < hb.y1; ++y) {
      const float* hrow = hm.data() + static_cast<size_t>(y - hb.y0) * HW;
      float* row = buf + static_cast<size_t>(y - b.y0) * W + (hb.x0 - b.x0);
      for (int x = 0; x < HW; ++x)
        if (hrow[x] > 0.0f) row[x] *= 1.0f - std::min(hrow[x], 1.0f);
    }
  }
}

/// Coverage of `geom` (in geometry space; `toPixel` maps it to pixels) over the
/// pixel box b. `pixelToGrain` maps pixel centres to page space for the grain.
void coverage(const StrokeGeometry& geom, const StrokeStyle& style, const Affine& toPixel,
              const Affine& pixelToGrain, const GrainTexture& grain, const Box& b, std::vector<float>& mask) {
  const int W = b.width(), H = b.y1 - b.y0;
  mask.assign(static_cast<size_t>(std::max(W, 0)) * std::max(H, 0), 0.0f);
  if (W <= 0 || H <= 0) return;
  const bool identity = toPixel.isIdentity();
  const float s = toPixel.uniformScale();
  // Geometry-space cull rect: the pixel box mapped back, with room for the AA fringe and thin-line growth.
  const Rect cull = identity ? Rect(b.x0 - 1.0f, b.y0 - 1.0f, b.x1 + 1.0f, b.y1 + 1.0f)
                             : toPixel.inverse()
                                   .apply(Rect(static_cast<float>(b.x0), static_cast<float>(b.y0),
                                               static_cast<float>(b.x1), static_cast<float>(b.y1)))
                                   .inflated((kMinRadiusPx + 1.0f) / std::max(s, 1e-6f));
  float* m = mask.data();
  for (const CapsulePrim& c : geom.capsules) {
    if (!primBounds(c).intersects(cull)) continue;
    rasterCapsule(identity ? c : mapPrim(c, toPixel, s), b, m);
  }
  for (const NibPrim& n : geom.nibs) {
    if (!primBounds(n).intersects(cull)) continue;
    rasterNib(identity ? n : mapPrim(n, toPixel, s), b, m);
  }
  applyHoles(geom.holes, toPixel, identity, s, cull, b, m);
  if (geom.stamps.empty()) return;

  thread_local std::vector<float> density, grainBuf;
  density.assign(mask.size(), 0.0f);
  const bool useGrain = style.grainStrength > 0.0f;
  if (useGrain) {
    // Paper grain is a function of the pixel only: evaluate it once per pixel.
    grainBuf.resize(mask.size());
    const float inv = 1.0f / std::max(style.grainScale, 1e-3f);
    for (int y = b.y0; y < b.y1; ++y)
      for (int x = b.x0; x < b.x1; ++x) {
        Vec2 pp = pixelToGrain.apply(Vec2{x + 0.5f, y + 0.5f});
        grainBuf[static_cast<size_t>(y - b.y0) * W + (x - b.x0)] = grain.sample(pp.x * inv, pp.y * inv);
      }
  }
  bool any = false;
  for (const StampPrim& p : geom.stamps) {
    if (!primBounds(p).intersects(cull)) continue;
    accumulateStamp(identity ? p : mapPrim(p, toPixel, s), style, b, density.data(),
                    useGrain ? grainBuf.data() : nullptr);
    any = true;
  }
  if (!any) return;
  applyHoles(geom.holes, toPixel, identity, s, cull, b, density.data());
  for (size_t i = 0; i < mask.size(); ++i) {
    const float d = density[i];
    if (d <= 0.0f) continue;
    const float v = -std::expm1(-d);
    if (v > mask[i]) mask[i] = v;
  }
}

}  // namespace

void Rasterizer::rasterizeCoverage(const StrokeGeometry& g, const StrokeStyle& style, const Affine& pixelToPage,
                                   const GrainTexture& grain, int x0, int y0, int x1, int y1,
                                   std::vector<float>& mask) {
  coverage(g, style, Affine::identity(), pixelToPage, grain, Box{x0, y0, x1, y1}, mask);
}

// ---------------------------------------------------------------------------
// Compositing
// ---------------------------------------------------------------------------

namespace {

void compositeMask(Bitmap& target, const std::vector<float>& mask, const Box& b, const StrokeStyle& style) {
  const int W = b.width();
  const Color c = style.color;
  const float k = clamp01(style.opacity) * clamp01(c.a);
  for (int y = b.y0; y < b.y1; ++y) {
    const float* mrow = mask.data() + static_cast<size_t>(y - b.y0) * W - b.x0;
    for (int x = b.x0; x < b.x1; ++x) {
      float m = mrow[x];
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

inline uint8_t toByte(float v, float hi) { return static_cast<uint8_t>(clamp(v, 0.0f, hi) * 255.0f + 0.5f); }

/// Same math as compositeMask, straight into premultiplied RGBA8 (only the touched pixels).
void compositeMask8(PixelBufferRGBA8& target, const std::vector<float>& mask, const Box& b, const StrokeStyle& style) {
  const int W = b.width();
  const Color c = style.color;
  const float k = clamp01(style.opacity) * clamp01(c.a);
  constexpr float kInv = 1.0f / 255.0f;
  const bool multiply = style.blend == BlendMode::Multiply;
  for (int y = b.y0; y < b.y1; ++y) {
    const float* mrow = mask.data() + static_cast<size_t>(y - b.y0) * W - b.x0;
    uint8_t* row = target.data + static_cast<size_t>(y) * target.stride;
    for (int x = b.x0; x < b.x1; ++x) {
      const float m = mrow[x];
      if (m <= 0.0f) continue;
      const float a = std::min(m, 1.0f) * k;
      if (a < 0.5f * kInv) continue;  // below one 8-bit step
      uint8_t* p = row + static_cast<size_t>(x) * 4;
      const float d0 = p[0] * kInv, d1 = p[1] * kInv, d2 = p[2] * kInv, da = p[3] * kInv;
      float o0, o1, o2;
      if (multiply) {
        const float sr = c.r * a, sg = c.g * a, sb = c.b * a;
        o0 = sr * (1.0f - da) + d0 * (1.0f - a) + sr * d0;
        o1 = sg * (1.0f - da) + d1 * (1.0f - a) + sg * d1;
        o2 = sb * (1.0f - da) + d2 * (1.0f - a) + sb * d2;
      } else {
        o0 = c.r * a + d0 * (1.0f - a);
        o1 = c.g * a + d1 * (1.0f - a);
        o2 = c.b * a + d2 * (1.0f - a);
      }
      const float oa = clamp01(a + da * (1.0f - a));
      p[0] = toByte(o0, oa);
      p[1] = toByte(o1, oa);
      p[2] = toByte(o2, oa);
      p[3] = toByte(oa, 1.0f);
    }
  }
}

Rect targetRect(int width, int height, const RenderOptions& opts) {
  Rect full(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
  return opts.clip.isEmpty() ? full : opts.clip.intersection(full);
}

/// Pixel box covered by page-space geometry bounds under `pageToPixel`, clipped.
Box geometryBox(const StrokeGeometry& g, const Affine& pageToPixel, int width, int height, const RenderOptions& opts) {
  Rect area = pageToPixel.apply(g.bounds).inflated(2.0f).intersection(targetRect(width, height, opts));
  if (area.isEmpty()) return Box{0, 0, 0, 0};
  Box b;
  b.x0 = std::max(0, static_cast<int>(std::floor(area.minX)));
  b.y0 = std::max(0, static_cast<int>(std::floor(area.minY)));
  b.x1 = std::min(width, static_cast<int>(std::ceil(area.maxX)));
  b.y1 = std::min(height, static_cast<int>(std::ceil(area.maxY)));
  return b;
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
  Box b = geometryBox(pageGeometry, pageToPixel, target.width(), target.height(), opts);
  if (b.empty()) return;
  thread_local std::vector<float> mask;
  const GrainTexture& grain = opts.grain ? *opts.grain : GrainTexture::shared();
  coverage(pageGeometry, style, pageToPixel, pageToPixel.inverse(), grain, b, mask);
  compositeMask(target, mask, b, style);
}

void Rasterizer::renderGeometryOver(const StrokeGeometry& pageGeometry, const StrokeStyle& style,
                                    PixelBufferRGBA8& target, const Affine& pageToPixel, const RenderOptions& opts) {
  if (pageGeometry.empty() || !target.data) return;
  Box b = geometryBox(pageGeometry, pageToPixel, target.width, target.height, opts);
  if (b.empty()) return;
  thread_local std::vector<float> mask;
  const GrainTexture& grain = opts.grain ? *opts.grain : GrainTexture::shared();
  coverage(pageGeometry, style, pageToPixel, pageToPixel.inverse(), grain, b, mask);
  compositeMask8(target, mask, b, style);
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
  const Rect clipPx = targetRect(target.width(), target.height(), opts);
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
