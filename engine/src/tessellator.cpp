#include "ink/geometry.h"
#include "ink/spline.h"

namespace ink {

void StrokeGeometry::append(const StrokeGeometry& o) {
  capsules.insert(capsules.end(), o.capsules.begin(), o.capsules.end());
  nibs.insert(nibs.end(), o.nibs.begin(), o.nibs.end());
  stamps.insert(stamps.end(), o.stamps.begin(), o.stamps.end());
  holes.insert(holes.end(), o.holes.begin(), o.holes.end());
  bounds.expand(o.bounds);
}

namespace {

CurveSamplingOptions samplingFor(const StrokeStyle& style, const TessellationOptions& opts) {
  CurveSamplingOptions c;
  // Vector tips: straight runs of wide ink need few primitives (each one is a
  // capsule / nib sweep that is exact for linear motion). Stamps walk by
  // spacing instead and keep the fine sampling.
  if (style.method == RenderMethod::Vector) c.widthChordFactor = opts.widthChordFactor;
  float ppu = std::max(opts.pixelsPerUnit, 1e-3f);
  c.maxChord = std::max(opts.maxChordPixels, 0.25f) / ppu;
  c.maxAngle = opts.maxAngleDegrees * kPi / 180.0f;
  c.maxSubdivisions = 64;
  return c;
}

struct Emitter {
  const StrokeStyle& style;
  uint32_t seed;
  const TessellationOptions& opts;
  TessellationState& state;
  StrokeGeometry& out;
  float minRadius;

  // ---- Round tip -----------------------------------------------------------
  void capsule(const CurveSample& a, const CurveSample& b) {
    CapsulePrim c;
    c.p0 = a.pos;
    c.r0 = std::max(a.width * 0.5f, minRadius);
    c.a0 = a.alpha;
    c.p1 = b.pos;
    c.r1 = std::max(b.width * 0.5f, minRadius);
    c.a1 = b.alpha;
    out.capsules.push_back(c);
    out.bounds.expand(c.p0, c.r0);
    out.bounds.expand(c.p1, c.r1);
  }

  // ---- Flat nib ------------------------------------------------------------
  void nib(const CurveSample& a, const CurveSample& b) {
    NibPrim n;
    float ra = std::max(a.width * style.nibThickness * 0.5f, minRadius);
    float rb = std::max(b.width * style.nibThickness * 0.5f, minRadius);
    n.round = 0.5f * (ra + rb);
    n.p0 = a.pos;
    n.h0 = std::max(a.width * 0.5f - n.round, 0.0f);
    n.a0 = a.alpha;
    n.p1 = b.pos;
    n.h1 = std::max(b.width * 0.5f - n.round, 0.0f);
    n.a1 = b.alpha;
    n.dir = fromAngle(style.nibAngle);
    out.nibs.push_back(n);
    out.bounds.expand(n.p0, n.h0 + n.round);
    out.bounds.expand(n.p1, n.h1 + n.round);
  }

  // ---- Stamps --------------------------------------------------------------
  float spacingAt(const CurveSample& s) const {
    float minSpacing = opts.minStampSpacingPixels / std::max(opts.pixelsPerUnit, 1e-3f);
    return std::max(std::max(s.width * style.spacing, minSpacing), 1e-3f);
  }

  void stamp(const CurveSample& s) {
    uint32_t idx = state.stampIndex++;
    StampPrim p;
    float rMajor = std::max(s.width * 0.5f, minRadius);
    float aspect = lerp(1.0f, std::max(style.tiltAspect, 1.0f), clamp01(s.tilt));
    p.rMajor = rMajor;
    p.rMinor = std::max(rMajor / aspect, minRadius);
    p.axis = s.tilt > 1e-3f ? fromAngle(s.azimuth) : Vec2{1.0f, 0.0f};
    Vec2 c = s.pos;
    if (style.positionJitter > 0.0f) {
      uint32_t h = hash32(seed, idx * 3u + 1u);
      float jx = hashToUnit(h) * 2.0f - 1.0f;
      float jy = hashToUnit(hash32(h)) * 2.0f - 1.0f;
      c += Vec2{jx, jy} * (style.positionJitter * rMajor);
    }
    p.center = c;
    float alpha = s.alpha;
    if (style.opacityJitter > 0.0f) alpha *= 1.0f - style.opacityJitter * hashToUnit(hash32(seed, idx * 3u + 2u));
    p.alpha = alpha;
    // Tilted (side-of-lead) shading skims the grain peaks: shallower deposit.
    p.depth = clamp01(s.pressure * (1.0f - 0.30f * clamp01(s.tilt)));
    out.stamps.push_back(p);
    out.bounds.expand(c, rMajor);
  }

  static CurveSample mix(const CurveSample& a, const CurveSample& b, float t) {
    CurveSample s;
    s.pos = lerp(a.pos, b.pos, t);
    s.width = lerp(a.width, b.width, t);
    s.alpha = lerp(a.alpha, b.alpha, t);
    s.pressure = lerp(a.pressure, b.pressure, t);
    s.tilt = lerp(a.tilt, b.tilt, t);
    s.azimuth = lerpAngle(a.azimuth, b.azimuth, t);
    return s;
  }

  void walkStamps(const std::vector<CurveSample>& samples) {
    if (samples.empty()) return;
    if (!state.started) {
      stamp(samples[0]);
      state.started = true;
      state.carry = spacingAt(samples[0]);
    }
    for (size_t i = 1; i < samples.size(); ++i) {
      const CurveSample& a = samples[i - 1];
      const CurveSample& b = samples[i];
      float segLen = distance(a.pos, b.pos);
      float traveled = 0.0f;
      while (state.carry <= segLen - traveled) {
        traveled += state.carry;
        CurveSample s = mix(a, b, segLen > 0.0f ? traveled / segLen : 1.0f);
        stamp(s);
        state.carry = spacingAt(s);
      }
      state.carry -= segLen - traveled;
    }
  }

  void dot(const StrokePoint& p) {
    CurveSample s = toCurveSample(p);
    if (style.method == RenderMethod::Stamp) {
      std::vector<CurveSample> one{s};
      walkStamps(one);
    } else if (style.tip == TipShape::Nib) {
      nib(s, s);
    } else {
      capsule(s, s);
    }
    state.started = true;
  }

  void emitSamples(const std::vector<CurveSample>& samples) {
    if (style.method == RenderMethod::Stamp) {
      walkStamps(samples);
      return;
    }
    if (samples.size() == 1) {
      if (style.tip == TipShape::Nib) nib(samples[0], samples[0]);
      else capsule(samples[0], samples[0]);
      return;
    }
    for (size_t i = 1; i < samples.size(); ++i) {
      if (style.tip == TipShape::Nib) nib(samples[i - 1], samples[i]);
      else capsule(samples[i - 1], samples[i]);
    }
    state.started = true;
  }
};

}  // namespace

void Tessellator::tessellate(const Stroke& stroke, const TessellationOptions& opts, StrokeGeometry& out) {
  tessellate(stroke.style, stroke.seed, stroke.points.data(), stroke.points.size(), opts, out);
  if (out.empty()) return;
  for (const EraseHole& h : stroke.holes) {
    if (!h.bounds().intersects(out.bounds)) continue;
    CapsulePrim c;
    c.p0 = h.a;
    c.p1 = h.b;
    c.r0 = c.r1 = h.radius;
    c.a0 = c.a1 = 1.0f;
    out.holes.push_back(c);
  }
}

void Tessellator::tessellate(const StrokeStyle& style, uint32_t seed, const StrokePoint* pts, size_t count,
                             const TessellationOptions& opts, StrokeGeometry& out) {
  TessellationState state;
  tessellateSegments(style, seed, pts, count, 0, CatmullRom::segmentCount(count), opts, state, out);
}

void Tessellator::tessellateSegments(const StrokeStyle& style, uint32_t seed, const StrokePoint* pts,
                                     size_t count, size_t segBegin, size_t segEnd,
                                     const TessellationOptions& opts, TessellationState& state,
                                     StrokeGeometry& out) {
  if (count == 0) return;
  // Sub-pixel radii are kept at a small floor; the rasterizer converts the
  // remaining thinness into coverage so hairlines fade instead of breaking up.
  Emitter em{style, seed, opts, state, out, 1e-3f};
  if (count == 1) {
    if (!state.started) em.dot(pts[0]);
    return;
  }
  CurveSamplingOptions sampling = samplingFor(style, opts);
  segEnd = std::min(segEnd, CatmullRom::segmentCount(count));
  std::vector<CurveSample> samples;
  samples.reserve(32);
  for (size_t seg = segBegin; seg < segEnd; ++seg) {
    samples.clear();
    CatmullRom::sampleSegment(pts, count, seg, sampling, /*includeStart=*/true, samples);
    em.emitSamples(samples);
  }
}

void transformGeometry(StrokeGeometry& geom, const Affine& t) {
  if (t.isIdentity()) return;
  float s = t.uniformScale();
  for (CapsulePrim& c : geom.capsules) {
    c.p0 = t.apply(c.p0);
    c.p1 = t.apply(c.p1);
    c.r0 *= s;
    c.r1 *= s;
  }
  for (NibPrim& n : geom.nibs) {
    n.p0 = t.apply(n.p0);
    n.p1 = t.apply(n.p1);
    n.h0 *= s;
    n.h1 *= s;
    n.round *= s;
    n.dir = normalize(t.applyVector(n.dir));
  }
  for (CapsulePrim& c : geom.holes) {
    c.p0 = t.apply(c.p0);
    c.p1 = t.apply(c.p1);
    c.r0 *= s;
    c.r1 *= s;
  }
  for (StampPrim& p : geom.stamps) {
    p.center = t.apply(p.center);
    p.axis = normalize(t.applyVector(p.axis));
    p.rMajor *= s;
    p.rMinor *= s;
  }
  geom.bounds = t.apply(geom.bounds);
}

}  // namespace ink
