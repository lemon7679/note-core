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
//     Optionally the live stroke is stabilized while writing (ibisPaint-style).
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
