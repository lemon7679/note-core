#pragma once

#include <vector>

#include "ink/brush.h"
#include "ink/page.h"

namespace ink {

// ---------------------------------------------------------------------------
// 글씨 보정 (handwriting correction)
//
// Two complementary tools:
//
//  1. Stroke correction — applied to each stroke when the pen lifts (or later
//     to selected strokes). Keeps the writer's own shapes, only tidies them:
//       * zero-lag, corner-preserving smoothing of wobble,
//       * near-straight segments become straight,
//       * near-horizontal / vertical segments become exact (ㅡ ㅣ ㄱ ㄴ ㅁ ㄹ),
//         with shared corners kept connected.
//     Gentle curves stay curves: only wobble the size of tremor is
//     straightened. While writing, LiveSmoother applies the same curve
//     smoothing to the ink behind the pen, so it visibly settles into the
//     corrected curve as the pen moves on (GoodNotes-style).
//
//  2. Line straightening — for a selection of written lines: removes the
//     slant of each line and levels words that drift up or down, without
//     touching the letters themselves.
// ---------------------------------------------------------------------------

/// Statistics about what correctStroke changed (for tests / UI feedback).
struct CorrectionReport {
  int corners = 0;
  int straightened = 0;
  int axisSnapped = 0;
  float maxDisplacement = 0.0f;
};

/// Corrects one stroke's control points in place (positions only; the number
/// of points and all other attributes are preserved). `unitsPerViewPoint`
/// (= 1 / zoom) makes thresholds relative to what the user sees.
/// Returns true if anything moved.
bool correctStroke(std::vector<StrokePoint>& points, const CorrectionParams& params,
                   float unitsPerViewPoint = 1.0f, CorrectionReport* report = nullptr);

/// The live part of 글씨 보정 (GoodNotes-style "settling" ink): while the pen
/// moves, the same zero-shrink, corner-preserving curve smoothing that
/// correctStroke applies at pen-up is applied to the stroke behind the pen.
///
/// The ink starts exactly under the pen (no lag, prediction stays on) and,
/// as the pen moves on, the trail behind it visibly settles into a smooth
/// curve that follows the pen. Unlike a causal moving average it neither
/// trails behind the pen nor cuts curves short, and what you see while
/// writing is what remains after pen-up (apart from straightened lines).
///
/// Points settle (stop changing) once the pen is `settleDistance()` past
/// them, so incremental rendering only redraws that short trailing window.
class LiveSmoother {
 public:
  /// Disabled unless correction is on and smoothCurves is set.
  /// `typicalWidth` is the brush's nominal width (page units).
  void reset(const CorrectionParams& params, float unitsPerViewPoint, float typicalWidth);
  void clear();
  bool active() const { return sigma_ > 0.0f; }

  /// Call after control points were appended. `raw` holds the unsmoothed
  /// positions of `pts` (same length); positions of `pts` from settled() on
  /// are rewritten, all other attributes are left alone.
  void update(const std::vector<Vec2>& raw, std::vector<StrokePoint>& pts);

  /// Leading points whose positions are final.
  size_t settled() const { return settled_; }
  float settleDistance() const { return settleDist_; }

 private:
  float sigma_ = 0.0f;
  float w_ = 0.0f;
  float settleDist_ = 0.0f;
  std::vector<float> d_;  ///< Arc length of the raw polyline.
  std::vector<size_t> corners_;
  std::vector<float> cornerTurns_;
  size_t checked_ = 0;  ///< Points judged for corners so far.
  size_t settled_ = 0;
  std::vector<Vec2> work_, g1_, g2_;
};

/// Applies correctStroke to existing strokes (undoable).
ChangeSet correctStrokes(Page& page, LayerId layer, const std::vector<StrokeId>& ids,
                         const CorrectionParams& params, float unitsPerViewPoint = 1.0f);

struct StraightenParams {
  float strength = 1.0f;           ///< 0..1 blend between original and fully straightened.
  float maxAngleDegrees = 30.0f;   ///< Larger slants are treated as intentional.
  bool levelWords = true;          ///< Also level words that drift off the line.
};

/// Straightens handwritten lines made of the given strokes (undoable):
/// deskews (projection-profile search), splits into text lines, removes each
/// line's residual slant and levels its words onto a common centre line.
ChangeSet straightenHandwriting(Page& page, LayerId layer, const std::vector<StrokeId>& ids,
                                const StraightenParams& params = {});

}  // namespace ink
