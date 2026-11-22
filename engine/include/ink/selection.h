#pragma once

#include <vector>

#include "ink/page.h"

namespace ink {

bool pointInPolygon(Vec2 p, const std::vector<Vec2>& polygon);

/// Strokes of `layer` enclosed by a lasso. A stroke is selected when at least
/// `minInsideFraction` of its length lies inside (so a lasso that clips the
/// tail of a word still selects it, but one grazing a neighbour does not).
std::vector<StrokeId> lassoSelect(const Layer& layer, const std::vector<Vec2>& lasso,
                                  float minInsideFraction = 0.6f);

/// Strokes intersecting a rectangle (any ink inside).
std::vector<StrokeId> rectSelect(const Layer& layer, const Rect& rect);

/// Bounds of the given strokes.
Rect selectionBounds(const Layer& layer, const std::vector<StrokeId>& ids);

/// Applies an affine transform (move / scale / rotate) to strokes in place.
/// Widths scale with the transform; nib angles and tilt azimuths rotate.
ChangeSet transformStrokes(Page& page, LayerId layer, const std::vector<StrokeId>& ids, const Affine& t);
ChangeSet recolorStrokes(Page& page, LayerId layer, const std::vector<StrokeId>& ids, Color color);
ChangeSet deleteStrokes(Page& page, LayerId layer, const std::vector<StrokeId>& ids);
/// Duplicates strokes (new ids) on top of the layer, offset by `offset`.
ChangeSet duplicateStrokes(Page& page, LayerId layer, const std::vector<StrokeId>& ids, Vec2 offset);

}  // namespace ink
