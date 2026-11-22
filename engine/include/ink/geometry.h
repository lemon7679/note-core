#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "ink/brush.h"
#include "ink/stroke.h"

namespace ink {

// ---------------------------------------------------------------------------
// GPU-friendly render primitives.
//
// Strokes are rendered as the union of many small primitives whose coverage is
// evaluated analytically with signed distance functions (SDF) in the fragment
// shader (see shaders/ink_stroke.metal) and by the reference CPU rasterizer:
//
//   * CapsulePrim — a tapered capsule (two circles + tangent hull). Sweeping a
//     round tip along the path; joints are seamless because neighbouring
//     capsules share an end circle. Pixel-exact anti-aliasing at any zoom.
//   * NibPrim — a flat nib (segment with rounded ends) swept between two
//     positions: chisel highlighter and calligraphy.
//   * StampPrim — an oriented soft ellipse modulated by paper grain (pencil).
//
// Primitives of one stroke are max-accumulated into a coverage mask and the mask
// is then composited once with the stroke color / opacity / blend mode.
// All structs are tightly packed 32/48-byte PODs for direct upload as
// instance buffers.
// ---------------------------------------------------------------------------

struct CapsulePrim {
  Vec2 p0;
  float r0 = 0, a0 = 1;  ///< Radius and opacity at p0.
  Vec2 p1;
  float r1 = 0, a1 = 1;  ///< Radius and opacity at p1.
};
static_assert(sizeof(CapsulePrim) == 32, "CapsulePrim must be 32 bytes");

struct NibPrim {
  Vec2 p0;
  float h0 = 0, a0 = 1;  ///< Nib half-length (excluding rounding) and opacity at p0.
  Vec2 p1;
  float h1 = 0, a1 = 1;
  Vec2 dir;              ///< Unit direction of the nib's long axis.
  float round = 0;       ///< Corner rounding radius (= half thickness).
  float pad = 0;
};
static_assert(sizeof(NibPrim) == 48, "NibPrim must be 48 bytes");

struct StampPrim {
  Vec2 center;
  Vec2 axis;              ///< Unit direction of the major axis.
  float rMajor = 0, rMinor = 0;
  float alpha = 1;
  float depth = 0.5f;     ///< Grain penetration 0..1 (from pressure).
};
static_assert(sizeof(StampPrim) == 32, "StampPrim must be 32 bytes");

struct StrokeGeometry {
  std::vector<CapsulePrim> capsules;
  std::vector<NibPrim> nibs;
  std::vector<StampPrim> stamps;
  Rect bounds;

  void clear() {
    capsules.clear();
    nibs.clear();
    stamps.clear();
    bounds = Rect();
  }
  bool empty() const { return capsules.empty() && nibs.empty() && stamps.empty(); }
  size_t primitiveCount() const { return capsules.size() + nibs.size() + stamps.size(); }
  void append(const StrokeGeometry& o);
};

struct TessellationOptions {
  /// Device pixels per page unit at which the geometry will be displayed.
  /// Controls curve sampling density (level of detail).
  float pixelsPerUnit = 2.0f;
  float maxChordPixels = 4.0f;
  float maxAngleDegrees = 6.0f;
  float minStampSpacingPixels = 0.35f;
};

/// State carried between incremental tessellation calls (pencil stamp walk).
struct TessellationState {
  float carry = 0.0f;
  uint32_t stampIndex = 0;
  bool started = false;
};

class Tessellator {
 public:
  static void tessellate(const Stroke& stroke, const TessellationOptions& opts, StrokeGeometry& out);
  static void tessellate(const StrokeStyle& style, uint32_t seed, const StrokePoint* pts, size_t count,
                         const TessellationOptions& opts, StrokeGeometry& out);

  /// Tessellates curve segments [segBegin, segEnd) of the control polygon.
  /// Calling it on consecutive ranges with the same `state` yields exactly the
  /// same primitives as one call over the union — used for incremental live
  /// rendering where finalized segments are baked once.
  static void tessellateSegments(const StrokeStyle& style, uint32_t seed, const StrokePoint* pts,
                                 size_t count, size_t segBegin, size_t segEnd,
                                 const TessellationOptions& opts, TessellationState& state,
                                 StrokeGeometry& out);
};

/// Maps geometry through an affine transform (positions, axes; radii by the uniform scale).
void transformGeometry(StrokeGeometry& geom, const Affine& t);

}  // namespace ink
