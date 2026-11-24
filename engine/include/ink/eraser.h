#pragma once

#include <cstdint>

#include "ink/page.h"

namespace ink {

enum class EraserMode : uint8_t {
  Stroke = 0,   ///< Touching any part of a stroke removes the whole stroke.
  /// Removes exactly the ink under the eraser: the swept area becomes a hole of
  /// the stroke (Stroke::holes), and the parts of the stroke that end up fully
  /// hidden are cut away (pieces keep their look).
  Precise = 1,
};

inline uint32_t kindBit(BrushKind k) { return 1u << static_cast<uint32_t>(k); }
constexpr uint32_t kAllKinds = 0xFFFFFFFFu;

struct EraserOptions {
  EraserMode mode = EraserMode::Stroke;
  float radius = 6.0f;  ///< Page units.
  /// Unused since precise erasing is area-exact (kept for source compatibility).
  float widthInfluence = 0.5f;
  /// Only strokes whose kind bit is set are erased (e.g. "erase highlighter only").
  uint32_t kindMask = kAllKinds;
};

/// Erases along the eraser path segment `from` -> `to` in one layer, applies the
/// change to the page and returns it (empty if nothing was hit). Call this for
/// every eraser move and group the results in History for a single undo step.
///
/// `extendHoles`: the eraser path from the previous call's start through `to`
/// is straight (PageEditor tracks this), so a stroke's last hole ending at
/// `from` is stretched to `to` instead of adding another one.
ChangeSet eraseAlong(Page& page, LayerId layer, Vec2 from, Vec2 to, const EraserOptions& opts,
                     bool extendHoles = false);

/// True if any ink of `stroke` lies within `radius` of segment ab.
bool strokeHitsSegment(const Stroke& stroke, Vec2 a, Vec2 b, float radius, float widthInfluence = 1.0f);

}  // namespace ink
