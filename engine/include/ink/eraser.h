#pragma once

#include <cstdint>

#include "ink/page.h"

namespace ink {

enum class EraserMode : uint8_t {
  Stroke = 0,   ///< Touching any part of a stroke removes the whole stroke.
  Precise = 1,  ///< Cuts strokes where the eraser passes (vector-exact, pieces keep their look).
};

inline uint32_t kindBit(BrushKind k) { return 1u << static_cast<uint32_t>(k); }
constexpr uint32_t kAllKinds = 0xFFFFFFFFu;

struct EraserOptions {
  EraserMode mode = EraserMode::Stroke;
  float radius = 6.0f;  ///< Page units.
  /// Precise mode: how much of the stroke's own half-width counts as "touched".
  /// 0 = only the centreline, 1 = any ink under the eraser.
  float widthInfluence = 0.5f;
  /// Only strokes whose kind bit is set are erased (e.g. "erase highlighter only").
  uint32_t kindMask = kAllKinds;
};

/// Erases along the eraser path segment `from` -> `to` in one layer, applies the
/// change to the page and returns it (empty if nothing was hit). Call this for
/// every eraser move and group the results in History for a single undo step.
ChangeSet eraseAlong(Page& page, LayerId layer, Vec2 from, Vec2 to, const EraserOptions& opts);

/// True if any ink of `stroke` lies within `radius` of segment ab.
bool strokeHitsSegment(const Stroke& stroke, Vec2 a, Vec2 b, float radius, float widthInfluence = 1.0f);

}  // namespace ink
