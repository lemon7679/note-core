#pragma once

#include <cstdint>
#include <vector>

#include "ink/stroke.h"

namespace ink {

// Draw-and-hold shapes: the user draws roughly and keeps the pen still; the
// stroke is replaced by a clean shape. Values are part of the C ABI (InkShape).
enum class ShapeType : uint8_t {
  None = 0,
  Line,
  Polyline,   ///< Open, 2-3 straight segments (e.g. an angle bracket, a check mark).
  Triangle,
  Rectangle,  ///< Right angles (any rotation); `regular` = square.
  Quad,       ///< Closed 4-gon that is not a rectangle (diamond, trapezoid...).
  Ellipse,
  Circle,
  Arc,        ///< Open circular arc (e.g. a semicircle).
  Polygon,    ///< Closed 5- or 6-gon; `regular` = regular pentagon / hexagon.
};

const char* shapeTypeName(ShapeType type);

struct RecognizedShape {
  ShapeType type = ShapeType::None;
  std::vector<Vec2> vertices;  ///< Line / polyline / polygon corners (polygons are implicitly closed).
  Vec2 center;                 ///< Ellipse / circle / arc.
  Vec2 radii;                  ///< Ellipse semi-axes (x along `rotation`); arc: radius in x.
  float rotation = 0.0f;       ///< Ellipse rotation (radians).
  float startAngle = 0.0f;     ///< Arc start (radians).
  float sweep = 0.0f;          ///< Arc sweep (radians, signed).
  bool regular = false;        ///< Square, equilateral triangle, regular polygon, circle.
  float error = 0.0f;          ///< Normalized fit error (lower is better).

  bool closed() const {
    return type == ShapeType::Triangle || type == ShapeType::Rectangle || type == ShapeType::Quad ||
           type == ShapeType::Ellipse || type == ShapeType::Circle || type == ShapeType::Polygon;
  }
  /// Pivot used when the shape is resized / rotated after snapping.
  Vec2 anchor() const;
};

struct ShapeRecognizerOptions {
  float minSize = 6.0f;            ///< Page units; smaller drawings are never shapes.
  float lineTolerance = 0.045f;    ///< Max deviation / length for a straight line.
  float closeTolerance = 0.20f;    ///< End gap / path length to count as closed.
  float cornerTolerance = 0.06f;   ///< RDP epsilon as a fraction of the diagonal.
  float polygonFitError = 0.022f;  ///< Mean edge distance / diagonal.
  float edgeStraightness = 0.045f; ///< Mean deviation of each edge / its length (polygon vs curve).
  float ellipseFitError = 0.075f;  ///< Mean |r - 1| in normalized ellipse space.
  float arcFitError = 0.035f;      ///< Mean |d - r| / r for an arc.
  float minArcSweepDegrees = 25.0f;
  float circleRatio = 0.85f;       ///< Minor/major ratio above which an ellipse becomes a circle.
  float angleSnapDegrees = 3.0f;   ///< Lines / edges within this of horizontal/vertical are snapped.
  float rightAngleTolerance = 14.0f;  ///< Degrees; quads within it become rectangles.
  bool allowPolyline = true;
  bool allowArc = true;
  /// Automatically perfect near-regular shapes (square, equilateral and right
  /// triangles, regular polygons, semicircles).
  bool autoRegularize = true;
  float squareTolerance = 0.10f;      ///< Relative side difference to become a square.
  float equilateralTolerance = 0.12f; ///< Relative side difference to become equilateral.
  float regularPolygonTolerance = 0.16f;
};

/// Classifies a hand-drawn path. Returns ShapeType::None if nothing fits well —
/// handwriting must never be "corrected" by accident.
RecognizedShape recognizeShape(const std::vector<Vec2>& path, const ShapeRecognizerOptions& opts = {});

/// Perfects a shape. With `force` the result is always ideal (circle, square,
/// equilateral triangle, regular polygon, 15°-snapped line) — used while the
/// user holds a second finger down. Without it, only near-regular shapes change.
RecognizedShape regularizeShape(const RecognizedShape& shape, const ShapeRecognizerOptions& opts, bool force);

/// Applies a similarity transform (move / uniform scale / rotate) to a shape.
RecognizedShape transformShape(const RecognizedShape& shape, const Affine& t);

/// Similarity transform about `anchor` that maps `from` onto `to`
/// (used to resize / rotate a snapped shape by dragging the pen).
Affine dragTransform(Vec2 anchor, Vec2 from, Vec2 to, bool allowRotation, float rotationDeadZoneRadians);

/// Returns `end`, rotated around `start` onto the nearest multiple of
/// `stepDegrees` if it is within `snapDegrees` of it (step 90 = horizontal/vertical).
Vec2 snapLineEnd(Vec2 start, Vec2 end, float snapDegrees, float stepDegrees = 90.0f);

/// Converts a recognized shape to stroke control points with uniform attributes
/// taken from `templ`. Corners are flagged sharp so the spline keeps them crisp.
std::vector<StrokePoint> shapeToStrokePoints(const RecognizedShape& shape, const StrokePoint& templ);

}  // namespace ink
