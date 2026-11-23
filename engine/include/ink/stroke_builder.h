#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "ink/brush.h"
#include "ink/correction.h"
#include "ink/filters.h"
#include "ink/geometry.h"
#include "ink/input.h"
#include "ink/shape_recognizer.h"
#include "ink/stroke.h"

namespace ink {

/// Geometry of the stroke being drawn, split for incremental rendering:
///
///  * `stable` grows monotonically: those primitives will never change until
///    pen-up, so a renderer can max-blend them into the live stroke's coverage
///    texture once (only the range starting at newCapsules/newNibs/newStamps
///    is new since the previous call).
///  * `tail` is rebuilt on every call: the last few volatile segments plus
///    predicted points. Draw it on top of the stable coverage each frame.
///
/// When `stableReset` is set the stable set was rebuilt (shape snap, zoom
/// change): discard anything baked and redraw all of `stable`.
struct LiveGeometry {
  const StrokeGeometry* stable = nullptr;
  const StrokeGeometry* tail = nullptr;
  size_t newCapsules = 0, newNibs = 0, newStamps = 0;
  bool stableReset = false;
  /// Page-space area that changed since the previous call (old tail + new
  /// tail + newly stable primitives). Use it as the redraw region.
  Rect dirty;
};

/// Converts a live stream of stylus samples into a stroke.
///
/// Pipeline per sample:
///   raw input ─► 1€ filter ─► optional stabilizer ─► de-duplication
///   ─► speed estimate ─► pressure (EMA + curve, or simulated) ─► tilt
///   ─► width / opacity (+ start taper) ─► control point
/// At pen-up: stabilizer catch-up, hook removal, end taper (+ short-stroke
/// protection) and, if the pen rested, shape snapping.
///
/// Not thread-safe; drive it from the input thread and hand the stroke to the
/// page when done.
class StrokeBuilder {
 public:
  explicit StrokeBuilder(const Brush& brush = presets::ballpoint());

  /// Changing the brush mid-stroke is ignored until the next begin().
  void setBrush(const Brush& brush);
  const Brush& brush() const { return brush_; }

  /// View points per page unit (the zoom factor). Filtering, speed and
  /// distance thresholds are evaluated in view space, so the pen feels the
  /// same at every zoom level while the ink stays in page units.
  void setViewScale(float viewPointsPerUnit);
  float viewScale() const { return viewScale_; }

  void begin(const InputPoint& p, uint32_t seed = 0);
  void addPoint(const InputPoint& p);
  void addPoints(const InputPoint* pts, size_t count);

  /// Platform-predicted samples (iOS `predictedTouches`). Replaces the previous
  /// prediction. They only affect rendering, never the final stroke.
  void setPredictedPoints(const InputPoint* pts, size_t count);
  void clearPredicted();

  /// Refined pressure / tilt for a sample sent earlier with an *ExpectsUpdate
  /// flag (iOS `touchesEstimatedPropertiesUpdated`). Negative values = unchanged.
  /// Returns false if no live point matches `updateId`.
  bool updateEstimatedProperties(uint64_t updateId, float pressure, float altitude, float azimuth);

  /// Advances time-based logic (draw-and-hold shape snapping). Call once per
  /// display frame while the pen is down. Returns true when the stroke snapped
  /// (a good moment for a light haptic tap).
  bool tick(double now);

  /// Constraint for a snapped shape (e.g. while a second finger touches the
  /// screen): ellipse -> circle, rectangle -> square, triangle -> equilateral,
  /// polygon -> regular, line -> 15° steps. Can be toggled at any time.
  void setShapeConstraint(bool constrained);
  bool shapeConstraint() const { return shapeConstraint_; }

  void end();
  void end(const InputPoint& last);
  void cancel();

  bool active() const { return active_; }
  bool finished() const { return finished_; }
  bool snapped() const { return snappedShape_ != ShapeType::None; }
  ShapeType snappedShape() const { return snappedShape_; }
  /// The snapped shape as currently shown (after drag-resize / constraint).
  const RecognizedShape& shape() const { return currentShape_; }
  /// What 글씨 보정 changed when the pen lifted (empty if it was off).
  const CorrectionReport& correctionReport() const { return correctionReport_; }

  const std::vector<StrokePoint>& points() const { return points_; }
  const std::vector<StrokePoint>& predicted() const { return predicted_; }
  /// Leading control points that can no longer change before pen-up.
  size_t stablePointCount() const;

  /// Incremental live geometry (see LiveGeometry). Pointers stay valid until
  /// the next mutating call.
  const LiveGeometry& updateGeometry(const TessellationOptions& opts);

  /// The finished stroke (call after end()). Id is assigned by the page.
  Stroke buildStroke() const;

 private:
  struct RawSample {
    InputPoint input;
    bool pressureAwaiting = false;
  };
  struct Aux {
    float rawPressure = 0.5f;
    float filteredPressure = 0.5f;
    float dt = 0.0f;
    float speed = 0.0f;  ///< Smoothed, view points / s.
    float velocityFactor = 1.0f;
    float tiltAmount = 0.0f;
    float tiltWidth = 1.0f;
    float tiltOpacity = 1.0f;
    float dist = 0.0f;  ///< Arc length from the start (page units).
    double time = 0.0;
    uint32_t input = 0;  ///< Index into raw_.
    bool awaiting = false;
  };

  void resetState();
  float unitsPerViewPoint() const { return 1.0f / viewScale_; }
  void processFiltered(Vec2 pos, double time, uint32_t inputIndex);
  void appendControl(Vec2 pos, double time, uint32_t inputIndex);
  float pressureFor(const InputPoint& in, float speed) const;
  void applyTilt(Aux& a, const InputPoint& in) const;
  void computeAttributes(size_t i);
  void recomputeFrom(size_t i);
  float startTaperLength() const;
  float taperFactor(size_t i) const;
  void removeHooks();
  void finalizeTapers();
  bool trySnap();
  void updateSnappedShape(Vec2 pen);
  void applyCorrection();
  void buildPrediction();
  void invalidateGeometry() { geomValid_ = false; }

  Brush brush_;
  Brush pendingBrush_;
  bool hasPendingBrush_ = false;
  float viewScale_ = 1.0f;

  bool active_ = false;
  bool finished_ = false;
  uint32_t seed_ = 0;
  double startTime_ = 0.0;
  double lastInputTime_ = 0.0;
  double lastFilterTime_ = 0.0;

  OneEuroFilter2D filter_;
  Stabilizer stabilizer_;
  std::vector<StabilizedSample> scratchStab_;
  std::vector<RawSample> raw_;
  std::vector<StrokePoint> points_;
  std::vector<Aux> aux_;
  std::vector<StrokePoint> predicted_;
  Vec2 velocity_;  ///< Smoothed page-space velocity (prediction).

  // Tapers (finalized at pen-up).
  float startTaperScale_ = 1.0f;
  float endTaperLength_ = 0.0f;

  // Draw-and-hold.
  Vec2 holdAnchor_;
  double holdAnchorTime_ = 0.0;
  ShapeType snappedShape_ = ShapeType::None;
  RecognizedShape baseShape_;     ///< As recognized (auto-regularized).
  RecognizedShape currentShape_;  ///< After drag / constraint.
  Vec2 snapPen_;                  ///< Pen position when the shape snapped.
  Vec2 lastPen_;                  ///< Latest filtered pen position.
  StrokePoint snapTemplate_;
  bool shapeConstraint_ = false;

  CorrectionReport correctionReport_;

  // Incremental geometry.
  bool geomValid_ = false;
  TessellationOptions geomOpts_;
  StrokeGeometry stableGeom_;
  StrokeGeometry tailGeom_;
  StrokeGeometry scratchGeom_;
  TessellationState stableState_;
  size_t stableSegments_ = 0;
  Rect prevTailBounds_;
  std::vector<StrokePoint> combined_;
  LiveGeometry live_;
};

}  // namespace ink
