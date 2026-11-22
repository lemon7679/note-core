// InkEngine — reference Metal shaders for rendering stroke primitives on iPad.
//
// STATUS: reference implementation. It mirrors include/ink/sdf.h and
// src/rasterizer.cpp line for line, but it is not compiled by the CMake build
// (no Metal toolchain on Linux CI). Validate it in Xcode against the CPU
// rasterizer output (ink_editor_render_rgba) when integrating.
//
// Two passes per stroke (or per live stroke each frame):
//
//  1. COVERAGE: draw every primitive of the stroke as an instanced quad into a
//     single-channel coverage texture (R16Float or R8Unorm, cleared to 0) with
//       blending: rgbBlendOperation = .max, source/destination factors = .one
//     so overlapping primitives of the same stroke never accumulate.
//       capsules -> ink_capsule_vertex / ink_coverage_capsule
//       nibs     -> ink_nib_vertex     / ink_coverage_nib
//       stamps   -> ink_stamp_vertex   / ink_coverage_stamp (needs the grain texture)
//
//  2. COMPOSITE: draw the stroke's bounds (InkGeometryView.bounds) with
//     ink_composite_vertex / ink_composite_fragment onto the page texture
//     (premultiplied alpha):
//       Normal:   source = .one,               destination = .oneMinusSourceAlpha
//       Multiply: source = .destinationColor,  destination = .oneMinusSourceAlpha
//
// Opaque solid strokes (Stroke::isOpaqueSolid) may skip pass 1 and draw the
// coverage straight into the page with normal blending.
//
// Grain: upload ink_grain_texture() as an R8Unorm 256x256 texture, sampled with
// address mode .repeat and linear filtering (no mipmaps, to match the CPU).

#include <metal_stdlib>
using namespace metal;

// ---- Buffers: byte-identical to ink::CapsulePrim / NibPrim / StampPrim ----

struct Capsule {
  float2 p0; float r0; float a0;
  float2 p1; float r1; float a1;
};

struct Nib {
  float2 p0; float h0; float a0;
  float2 p1; float h1; float a1;
  float2 dir; float round; float pad;
};

struct Stamp {
  float2 center; float2 axis;
  float rMajor; float rMinor; float alpha; float depth;
};

struct ViewUniforms {
  // Page units -> pixels: x' = a*x + c*y + tx, y' = b*x + d*y + ty.
  float a, b, c, d, tx, ty;
  float2 viewportSize;  // pixels
  // Pixels -> page units (inverse of the above), for grain sampling.
  float ia, ib, ic, id, itx, ity;
  float scale;          // sqrt(|det|): page-unit lengths -> pixels
  float pad;
};

struct StampUniforms {
  float hardness;
  float grainStrength;
  float grainScale;  // page units per texel
  float pad;
};

struct CompositeUniforms {
  float4 color;     // straight sRGB color
  float opacity;
  float3 pad;
};

constant float kMinRadiusPx = 0.5;

static inline float2 toPixel(constant ViewUniforms& u, float2 p) {
  return float2(u.a * p.x + u.c * p.y + u.tx, u.b * p.x + u.d * p.y + u.ty);
}
static inline float2 toPageFromPixel(constant ViewUniforms& u, float2 p) {
  return float2(u.ia * p.x + u.ic * p.y + u.itx, u.ib * p.x + u.id * p.y + u.ity);
}
static inline float2 rotateVec(constant ViewUniforms& u, float2 v) {
  return normalize(float2(u.a * v.x + u.c * v.y, u.b * v.x + u.d * v.y));
}
static inline float4 toClip(constant ViewUniforms& u, float2 px) {
  float2 ndc = px / u.viewportSize * 2.0 - 1.0;
  return float4(ndc.x, -ndc.y, 0.0, 1.0);
}
static inline float cross2(float2 a, float2 b) { return a.x * b.y - a.y * b.x; }
static inline float segmentParam(float2 p, float2 a, float2 b) {
  float2 ab = b - a;
  float l2 = dot(ab, ab);
  return l2 <= 1e-20 ? 0.0 : saturate(dot(p - a, ab) / l2);
}

// ---- SDFs (see include/ink/sdf.h) ----

static float sdUnevenCapsule(float2 p, float2 pa, float2 pb, float ra, float rb) {
  float2 ba = pb - pa;
  float h = dot(ba, ba);
  float rd = ra - rb;
  if (h <= rd * rd + 1e-12) return min(length(p - pa) - ra, length(p - pb) - rb);
  float2 q0 = p - pa;
  float2 q = float2(dot(q0, float2(ba.y, -ba.x)), dot(q0, ba)) / h;
  q.x = abs(q.x);
  float2 c = float2(sqrt(h - rd * rd), rd);
  float k = cross2(c, q);
  float m = dot(c, q);
  float n = dot(q, q);
  if (k < 0.0) return sqrt(h * n) - ra;
  if (k > c.x) return sqrt(h * (n + 1.0 - 2.0 * q.y)) - rb;
  return m - ra;
}

static float sdQuad(float2 p, float2 v0, float2 v1, float2 v2, float2 v3) {
  float2 v[4] = {v0, v1, v2, v3};
  float d = dot(p - v[0], p - v[0]);
  float s = 1.0;
  for (int i = 0, j = 3; i < 4; j = i, ++i) {
    float2 e = v[j] - v[i];
    float2 w = p - v[i];
    float ee = dot(e, e);
    float t = ee > 1e-20 ? saturate(dot(w, e) / ee) : 0.0;
    float2 b = w - e * t;
    d = min(d, dot(b, b));
    bool c1 = p.y >= v[i].y, c2 = p.y < v[j].y, c3 = e.x * w.y > e.y * w.x;
    if ((c1 && c2 && c3) || (!c1 && !c2 && !c3)) s = -s;
  }
  return s * sqrt(d);
}

static inline float coverageFromDistance(float d) { return saturate(0.5 - d); }

static float grainDeposit(float grain, float depth, float strength) {
  depth = saturate(depth);
  float threshold = mix(0.80, 0.05, depth);
  float peaks = smoothstep(threshold - 0.30, threshold + 0.30, grain);
  float floorValue = 0.18 + 0.30 * depth;
  return mix(1.0, mix(floorValue, 1.0, peaks), saturate(strength));
}

// Quad corners for vertex ids 0..3 (triangle strip).
static inline float2 corner(uint vid) { return float2((vid & 1) ? 1.0 : -1.0, (vid & 2) ? 1.0 : -1.0); }

// ===========================================================================
// Capsules
// ===========================================================================

struct CapsuleVOut {
  float4 position [[position]];
  float2 p0 [[flat]];
  float2 p1 [[flat]];
  float4 radiiAlpha [[flat]];  // r0, r1, a0, a1 (pixels / alpha incl. thin-line scale)
};

vertex CapsuleVOut ink_capsule_vertex(uint vid [[vertex_id]], uint iid [[instance_id]],
                                      const device Capsule* caps [[buffer(0)]],
                                      constant ViewUniforms& u [[buffer(1)]]) {
  Capsule c = caps[iid];
  float2 p0 = toPixel(u, c.p0), p1 = toPixel(u, c.p1);
  float r0 = c.r0 * u.scale, r1 = c.r1 * u.scale;
  float k0 = 1.0, k1 = 1.0;
  if (r0 < kMinRadiusPx) { k0 = max(r0, 0.0) / kMinRadiusPx; r0 = kMinRadiusPx; }
  if (r1 < kMinRadiusPx) { k1 = max(r1, 0.0) / kMinRadiusPx; r1 = kMinRadiusPx; }
  // Oriented bounding box around the capsule (+1 px for anti-aliasing).
  float2 axis = p1 - p0;
  float len = length(axis);
  float2 dirU = len > 1e-4 ? axis / len : float2(1.0, 0.0);
  float2 dirV = float2(-dirU.y, dirU.x);
  float rMax = max(r0, r1) + 1.0;
  float2 cr = corner(vid);
  float along = cr.x < 0.0 ? -(r0 + 1.0) : len + r1 + 1.0;
  float2 px = p0 + dirU * along + dirV * (cr.y * rMax);
  CapsuleVOut o;
  o.position = toClip(u, px);
  o.p0 = p0;
  o.p1 = p1;
  o.radiiAlpha = float4(r0, r1, c.a0 * k0, c.a1 * k1);
  return o;
}

fragment float4 ink_coverage_capsule(CapsuleVOut in [[stage_in]]) {
  float2 p = in.position.xy;
  float d = sdUnevenCapsule(p, in.p0, in.p1, in.radiiAlpha.x, in.radiiAlpha.y);
  float a = mix(in.radiiAlpha.z, in.radiiAlpha.w, segmentParam(p, in.p0, in.p1));
  float v = coverageFromDistance(d) * a;
  return float4(v, v, v, v);
}

// ===========================================================================
// Nibs
// ===========================================================================

struct NibVOut {
  float4 position [[position]];
  float2 p0 [[flat]];
  float2 p1 [[flat]];
  float2 dir [[flat]];
  float4 params [[flat]];  // h0, h1, round, alphaScale
  float2 alpha [[flat]];
};

vertex NibVOut ink_nib_vertex(uint vid [[vertex_id]], uint iid [[instance_id]],
                              const device Nib* nibs [[buffer(0)]],
                              constant ViewUniforms& u [[buffer(1)]]) {
  Nib n = nibs[iid];
  float2 p0 = toPixel(u, n.p0), p1 = toPixel(u, n.p1);
  float h0 = n.h0 * u.scale, h1 = n.h1 * u.scale, r = n.round * u.scale, k = 1.0;
  if (r < kMinRadiusPx) { k = max(r, 0.0) / kMinRadiusPx; r = kMinRadiusPx; }
  float reach = max(h0, h1) + r + 1.0;
  float2 lo = min(p0, p1) - reach, hi = max(p0, p1) + reach;
  float2 cr = corner(vid) * 0.5 + 0.5;
  NibVOut o;
  o.position = toClip(u, mix(lo, hi, cr));
  o.p0 = p0;
  o.p1 = p1;
  o.dir = rotateVec(u, n.dir);
  o.params = float4(h0, h1, r, k);
  o.alpha = float2(n.a0, n.a1);
  return o;
}

fragment float4 ink_coverage_nib(NibVOut in [[stage_in]]) {
  float2 p = in.position.xy;
  float2 dir = in.dir;
  float d = sdQuad(p, in.p0 - dir * in.params.x, in.p1 - dir * in.params.y, in.p1 + dir * in.params.y,
                   in.p0 + dir * in.params.x) - in.params.z;
  float a = mix(in.alpha.x, in.alpha.y, segmentParam(p, in.p0, in.p1)) * in.params.w;
  float v = coverageFromDistance(d) * a;
  return float4(v, v, v, v);
}

// ===========================================================================
// Stamps (pencil)
// ===========================================================================

struct StampVOut {
  float4 position [[position]];
  float2 center [[flat]];
  float2 axis [[flat]];
  float4 params [[flat]];  // rMajor, rMinor, alpha (incl. small-dot scale), depth
};

vertex StampVOut ink_stamp_vertex(uint vid [[vertex_id]], uint iid [[instance_id]],
                                  const device Stamp* stamps [[buffer(0)]],
                                  constant ViewUniforms& u [[buffer(1)]]) {
  Stamp s = stamps[iid];
  float ra = s.rMajor * u.scale, rb = s.rMinor * u.scale, k = 1.0;
  if (ra < kMinRadiusPx) {
    k = (ra * ra) / (kMinRadiusPx * kMinRadiusPx);
    ra = rb = kMinRadiusPx;
  } else if (rb < kMinRadiusPx) {
    k = rb / kMinRadiusPx;
    rb = kMinRadiusPx;
  }
  float2 c = toPixel(u, s.center);
  float2 axis = rotateVec(u, s.axis);
  float2 v = float2(-axis.y, axis.x);
  float2 cr = corner(vid);
  StampVOut o;
  o.position = toClip(u, c + axis * (cr.x * (ra + 1.0)) + v * (cr.y * (rb + 1.0)));
  o.center = c;
  o.axis = axis;
  o.params = float4(ra, rb, s.alpha * k, s.depth);
  return o;
}

fragment float4 ink_coverage_stamp(StampVOut in [[stage_in]],
                                   constant ViewUniforms& u [[buffer(1)]],
                                   constant StampUniforms& su [[buffer(2)]],
                                   texture2d<float> grain [[texture(0)]],
                                   sampler grainSampler [[sampler(0)]]) {
  float2 p = in.position.xy;
  float2 d = p - in.center;
  float2 v = float2(-in.axis.y, in.axis.x);
  float qx = dot(d, in.axis) / in.params.x;
  float qy = dot(d, v) / in.params.y;
  float q2 = qx * qx + qy * qy;
  if (q2 >= 1.0) return float4(0.0);
  float edge = max(1.0 - saturate(su.hardness), 1.0 / min(in.params.x, in.params.y));
  float f = smoothstep(0.0, 1.0, (1.0 - sqrt(q2)) / edge);
  float val = f * in.params.z;
  if (su.grainStrength > 0.0) {
    float2 page = toPageFromPixel(u, p);
    float g = grain.sample(grainSampler, page / max(su.grainScale, 1e-3) / 256.0).r;
    val *= grainDeposit(g, in.params.w, su.grainStrength);
  }
  return float4(val, val, val, val);
}

// ===========================================================================
// Composite
// ===========================================================================

struct CompositeVOut {
  float4 position [[position]];
};

// Draws the stroke's pixel-space bounds rectangle (4 vertices, triangle strip).
vertex CompositeVOut ink_composite_vertex(uint vid [[vertex_id]],
                                          constant float4& boundsPx [[buffer(0)]],  // minX, minY, maxX, maxY
                                          constant ViewUniforms& u [[buffer(1)]]) {
  float2 cr = corner(vid) * 0.5 + 0.5;
  CompositeVOut o;
  o.position = toClip(u, mix(boundsPx.xy, boundsPx.zw, cr));
  return o;
}

fragment float4 ink_composite_fragment(CompositeVOut in [[stage_in]],
                                       constant CompositeUniforms& cu [[buffer(0)]],
                                       texture2d<float> coverage [[texture(0)]]) {
  float m = coverage.read(uint2(in.position.xy)).r;
  float a = saturate(m) * saturate(cu.opacity) * saturate(cu.color.a);
  return float4(cu.color.rgb * a, a);  // premultiplied
}
