#pragma once

#include <cstdint>
#include <vector>

#include "ink/stroke.h"

namespace ink {

enum class ShapeType : uint8_t {
  None = 0,
  Line,
  Polyline,   ///< Open, 2-3 straight segments (e.g. an angle bracket).
  Triangle,
  Rectangle,  ///< Regularized to right angles (any rotation).
  Quad,       ///< Closed 4-gon that is not a rectangle.
  Ellipse,
  Circle,
};

const char* shapeTypeName(ShapeType type);

struct RecognizedShape {
  ShapeType type = ShapeType::None;
  std::vector<Vec2> vertices;  ///< Line / polyline / polygon corners (polygons are implicit-closed).
  Vec2 center;                 ///< Ellipse / circle.
  Vec2 radii;                  ///< Ellipse semi-axes (x along `rotation`).
  float rotation = 0.0f;       ///< Ellipse rotation (radians).
  float error = 0.0f;          ///< Normalized fit error (lower is better).
  bool closed() const {
    return type == ShapeType::Triangle || type == ShapeType::Rectangle || type == ShapeType::Quad ||
           type == ShapeType::Ellipse || type == ShapeType::Circle;
  }
};

struct ShapeRecognizerOptions {
  float minSize = 6.0f;            ///< Page units; smaller drawings are never shapes.
  float lineTolerance = 0.045f;    ///< Max deviation / length for a straight line.
  float closeTolerance = 0.20f;    ///< End gap / path length to count as closed.
  float cornerTolerance = 0.06f;   ///< RDP epsilon as a fraction of the diagonal.
  float polygonFitError = 0.022f;  ///< Mean edge distance / diagonal.
  float ellipseFitError = 0.075f;  ///< Mean |r - 1| in normalized ellipse space.
  float circleRatio = 0.85f;       ///< Minor/major ratio above which an ellipse becomes a circle.
  float angleSnapDegrees = 3.0f;   ///< Lines within this of horizontal/vertical are snapped.
  float rightAngleTolerance = 14.0f;  ///< Degrees; quads within it become rectangles.
  bool allowPolyline = true;
};

/// Classifies a hand-drawn path. Returns ShapeType::None if nothing fits well —
/// handwriting must never be "corrected" by accident.
RecognizedShape recognizeShape(const std::vector<Vec2>& path, const ShapeRecognizerOptions& opts = {});

/// Returns `end`, rotated around `start` onto the nearest horizontal/vertical
/// direction if it is within `snapDegrees` of it.
Vec2 snapLineEnd(Vec2 start, Vec2 end, float snapDegrees);

/// Converts a recognized shape to stroke control points with uniform attributes
/// taken from `templ`. Corners are flagged sharp so the spline keeps them crisp.
std::vector<StrokePoint> shapeToStrokePoints(const RecognizedShape& shape, const StrokePoint& templ);

}  // namespace ink
