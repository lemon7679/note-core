#include "ink/stroke_builder.h"

#include <algorithm>

#include "ink/spline.h"

namespace ink {

namespace {
constexpr size_t kAwaitWindow = 96;     // control points scanned for pending estimates
constexpr double kAwaitTimeout = 0.25;  // s; an estimate older than this is considered final
constexpr float kMinSnapSizeView = 12.0f;

float median(std::vector<float> v) {
  if (v.empty()) return 0.0f;
  size_t mid = v.size() / 2;
  std::nth_element(v.begin(), v.begin() + static_cast<long>(mid), v.end());
  return v[mid];
}
}  // namespace

StrokeBuilder::StrokeBuilder(const Brush& brush) : brush_(brush) {}

void StrokeBuilder::setBrush(const Brush& brush) {
  if (active_) {
    pendingBrush_ = brush;
    hasPendingBrush_ = true;
  } else {
    brush_ = brush;
  }
}

void StrokeBuilder::setViewScale(float viewPointsPerUnit) {
  if (viewPointsPerUnit > 1e-4f) viewScale_ = viewPointsPerUnit;
}

void StrokeBuilder::resetState() {
  raw_.clear();
  points_.clear();
  aux_.clear();
  predicted_.clear();
  velocity_ = {};
  startTaperScale_ = 1.0f;
  endTaperLength_ = 0.0f;
  snappedShape_ = ShapeType::None;
  filter_.reset();
  finished_ = false;
  geomValid_ = false;
  stableGeom_.clear();
  tailGeom_.clear();
  stableState_ = {};
  stableSegments_ = 0;
  prevTailBounds_ = Rect();
  live_ = {};
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

void StrokeBuilder::begin(const InputPoint& p, uint32_t seed) {
  if (hasPendingBrush_) {
    brush_ = pendingBrush_;
    hasPendingBrush_ = false;
  }
  resetState();
  active_ = true;
  const BrushDynamics& dyn = brush_.dynamics;
  filter_.setParams(p.source == InputSource::Pencil ? dyn.smoothing.pencil : dyn.smoothing.touch);
  stabilizer_.setParams(dyn.smoothing.stabilizer);
  startTime_ = lastInputTime_ = lastFilterTime_ = p.time;
  seed_ = seed != 0 ? seed
                    : hash32(static_cast<uint32_t>(p.time * 1000.0),
                             static_cast<uint32_t>(p.pos.x * 31.0f + p.pos.y * 17.0f));

  raw_.push_back({p, (p.flags & kInputPressureExpectsUpdate) != 0});
  Vec2 f = filter_.filter(p.pos, 0.0f, viewScale_);
  stabilizer_.reset(f, p.time, 0, unitsPerViewPoint());

  Aux a;
  a.time = p.time;
  a.input = 0;
  a.awaiting = raw_[0].pressureAwaiting;
  a.rawPressure = pressureFor(p, 0.0f);
  a.filteredPressure = a.rawPressure;
  applyTilt(a, p);
  StrokePoint sp;
  sp.pos = f;
  sp.azimuth = p.azimuth;
  sp.time = 0.0f;
  points_.push_back(sp);
  aux_.push_back(a);
  computeAttributes(0);

  holdAnchor_ = p.pos;
  holdAnchorTime_ = p.time;
}

void StrokeBuilder::addPoints(const InputPoint* pts, size_t count) {
  for (size_t i = 0; i < count; ++i) addPoint(pts[i]);
}

void StrokeBuilder::addPoint(const InputPoint& in) {
  if (!active_) return;
  InputPoint p = in;
  if (p.time <= lastInputTime_) p.time = lastInputTime_ + 1e-4;  // enforce monotonic time
  lastInputTime_ = p.time;
  const uint32_t idx = static_cast<uint32_t>(raw_.size());
  raw_.push_back({p, (p.flags & kInputPressureExpectsUpdate) != 0});

  if (distance(p.pos, holdAnchor_) * viewScale_ > brush_.dynamics.holdTolerance) {
    holdAnchor_ = p.pos;
    holdAnchorTime_ = p.time;
  }

  float dt = static_cast<float>(p.time - lastFilterTime_);
  lastFilterTime_ = p.time;
  Vec2 f = filter_.filter(p.pos, dt, viewScale_);

  if (snapped()) {
    updateSnappedLine(f);
    return;
  }
  processFiltered(f, p.time, idx);

  predicted_.clear();  // stale; the platform sends a fresh prediction per event
  if (brush_.dynamics.builtInPrediction) buildPrediction();
  tick(p.time);
}

void StrokeBuilder::processFiltered(Vec2 pos, double time, uint32_t inputIndex) {
  scratchStab_.clear();
  stabilizer_.push(pos, time, inputIndex, scratchStab_);
  for (const StabilizedSample& s : scratchStab_) appendControl(s.pos, s.time, s.source);
}

void StrokeBuilder::appendControl(Vec2 pos, double time, uint32_t inputIndex) {
  const BrushDynamics& dyn = brush_.dynamics;
  const InputPoint& in = raw_[inputIndex].input;
  const size_t lastIndex = points_.size() - 1;
  const float d = distance(pos, points_[lastIndex].pos);

  if (d * viewScale_ < dyn.minPointDistance) {
    // Pen (almost) at rest: don't add geometry, but follow pressure / tilt so
    // pressing harder in place still grows the dot. Ink already deposited does
    // not shrink when the pen eases off (lift-off), hence the max.
    Aux& last = aux_[lastIndex];
    last.rawPressure = std::max(last.rawPressure, pressureFor(in, last.speed));
    last.input = inputIndex;
    last.awaiting = raw_[inputIndex].pressureAwaiting;
    applyTilt(last, in);
    points_[lastIndex].azimuth = in.azimuth;
    recomputeFrom(lastIndex);
    return;
  }

  const Aux& prev = aux_[lastIndex];
  const float dt = std::max(static_cast<float>(time - prev.time), 1e-4f);
  const float instSpeed = d * viewScale_ / dt;
  Aux a;
  a.dt = dt;
  a.time = time;
  a.input = inputIndex;
  a.awaiting = raw_[inputIndex].pressureAwaiting;
  a.dist = prev.dist + d;
  a.speed = lerp(prev.speed, instSpeed, emaAlpha(dyn.smoothing.speedTau, dt));
  float speedT = clamp01(a.speed / std::max(dyn.velocityReference, 1.0f));
  a.velocityFactor = 1.0f - dyn.velocityThinning * easeOutQuad(speedT);
  a.rawPressure = pressureFor(in, a.speed);
  applyTilt(a, in);
  velocity_ = lerp(velocity_, (pos - points_[lastIndex].pos) / dt, emaAlpha(0.03f, dt));

  StrokePoint sp;
  sp.pos = pos;
  sp.azimuth = in.azimuth;
  sp.time = static_cast<float>(time - startTime_);
  points_.push_back(sp);
  aux_.push_back(a);
  computeAttributes(points_.size() - 1);
}

float StrokeBuilder::pressureFor(const InputPoint& in, float speed) const {
  if (in.source == InputSource::Pencil && in.hasPressure()) return clamp01(in.pressure);
  // No pressure sensor: emulate a pen that is pressed a bit harder when moving
  // slowly — keeps finger / mouse strokes from looking mechanical.
  const BrushDynamics& dyn = brush_.dynamics;
  float t = smoothstep(0.0f, 1.0f, clamp01(speed / std::max(dyn.velocityReference, 1.0f)));
  float effect = dyn.simulatedPressureSpeedEffect;
  return clamp01(dyn.simulatedPressure * lerp(1.0f + 0.5f * effect, 1.0f - effect, t));
}

void StrokeBuilder::applyTilt(Aux& a, const InputPoint& in) const {
  const TiltParams& tp = brush_.dynamics.tilt;
  if (!tp.enabled || !in.hasTilt()) {
    a.tiltAmount = 0.0f;
    a.tiltWidth = 1.0f;
    a.tiltOpacity = 1.0f;
    return;
  }
  float amount = smoothstep(tp.startAltitude, tp.fullAltitude, in.altitude);
  a.tiltAmount = amount;
  a.tiltWidth = lerp(1.0f, tp.maxWidthScale, amount);
  a.tiltOpacity = lerp(1.0f, tp.minOpacityScale, amount);
}

void StrokeBuilder::computeAttributes(size_t i) {
  const BrushDynamics& dyn = brush_.dynamics;
  Aux& a = aux_[i];
  if (i == 0) {
    a.filteredPressure = a.rawPressure;
  } else {
    a.filteredPressure =
        lerp(aux_[i - 1].filteredPressure, a.rawPressure, emaAlpha(dyn.smoothing.pressureTau, a.dt));
  }
  float p = dyn.pressureCurve.evaluate(a.filteredPressure);
  float w = dyn.width * lerp(dyn.pressureWidthMin, dyn.pressureWidthMax, p) * a.velocityFactor *
            a.tiltWidth * taperFactor(i);
  StrokePoint& sp = points_[i];
  sp.width = std::max(w, dyn.minWidth);
  sp.alpha = clamp01(lerp(dyn.pressureOpacityMin, dyn.pressureOpacityMax, p) * a.tiltOpacity);
  sp.pressure = p;
  sp.tilt = a.tiltAmount;
}

void StrokeBuilder::recomputeFrom(size_t i) {
  for (size_t j = i; j < points_.size(); ++j) computeAttributes(j);
}

float StrokeBuilder::startTaperLength() const {
  return brush_.dynamics.taper.startLength * brush_.dynamics.width * startTaperScale_;
}

float StrokeBuilder::taperFactor(size_t i) const {
  const TaperParams& tp = brush_.dynamics.taper;
  float f = 1.0f;
  float ls = startTaperLength();
  if (ls > 0.0f) f *= lerp(tp.minScale, 1.0f, easeOutQuad(aux_[i].dist / ls));
  if (endTaperLength_ > 0.0f) {
    float fromEnd = aux_.back().dist - aux_[i].dist;
    f *= lerp(tp.minScale, 1.0f, easeOutQuad(fromEnd / endTaperLength_));
  }
  return f;
}

// ---------------------------------------------------------------------------
// Prediction & estimated properties
// ---------------------------------------------------------------------------

void StrokeBuilder::setPredictedPoints(const InputPoint* pts, size_t count) {
  predicted_.clear();
  if (!active_ || snapped() || points_.empty()) return;
  // A stabilized stroke intentionally trails the pen; predicting would defeat it.
  if (brush_.dynamics.smoothing.stabilizer.mode != StabilizerMode::None) return;
  OneEuroFilter2D f = filter_;  // copy: prediction must not disturb the real filter
  double lastT = lastFilterTime_;
  Vec2 lastPos = points_.back().pos;
  for (size_t i = 0; i < count; ++i) {
    const InputPoint& in = pts[i];
    float dt = std::max(static_cast<float>(in.time - lastT), 1e-4f);
    lastT = std::max(lastT, in.time);
    Vec2 q = f.filter(in.pos, dt, viewScale_);
    if (distance(q, lastPos) * viewScale_ < brush_.dynamics.minPointDistance) continue;
    StrokePoint sp = points_.back();
    sp.pos = q;
    sp.flags = 0;
    sp.time = static_cast<float>(in.time - startTime_);
    predicted_.push_back(sp);
    lastPos = q;
  }
}

void StrokeBuilder::clearPredicted() { predicted_.clear(); }

void StrokeBuilder::buildPrediction() {
  predicted_.clear();
  if (!active_ || snapped() || points_.empty()) return;
  if (brush_.dynamics.smoothing.stabilizer.mode != StabilizerMode::None) return;
  const BrushDynamics& dyn = brush_.dynamics;
  if (length(velocity_) * viewScale_ < 60.0f) return;  // nothing to hide when slow
  Vec2 disp = velocity_ * dyn.predictionHorizon;
  float maxLen = std::max(dyn.width * 4.0f, 12.0f * unitsPerViewPoint());
  if (length(disp) > maxLen) disp = normalize(disp) * maxLen;
  const StrokePoint base = points_.back();
  for (float k : {0.5f, 1.0f}) {
    StrokePoint sp = base;
    sp.pos = base.pos + disp * k;
    sp.time = base.time + dyn.predictionHorizon * k;
    sp.flags = 0;
    predicted_.push_back(sp);
  }
}

bool StrokeBuilder::updateEstimatedProperties(uint64_t updateId, float pressure, float altitude,
                                              float azimuth) {
  if ((!active_ && !finished_) || updateId == 0) return false;
  size_t r = raw_.size();
  bool found = false;
  while (r-- > 0) {
    if (raw_[r].input.updateId == updateId) {
      found = true;
      break;
    }
    if (raw_.size() - r > 1024) break;
  }
  if (!found) return false;
  InputPoint& in = raw_[r].input;
  if (pressure >= 0.0f) in.pressure = pressure;
  if (altitude >= 0.0f) {
    in.altitude = altitude;
    in.azimuth = azimuth;
  }
  raw_[r].pressureAwaiting = false;
  if (snapped()) return true;  // snapped shapes have uniform attributes

  size_t first = static_cast<size_t>(-1);
  for (size_t i = aux_.size(); i-- > 0;) {
    if (aux_[i].input < r) break;  // inputs are monotonic along the stroke
    if (aux_[i].input == r) {
      aux_[i].awaiting = false;
      aux_[i].rawPressure = pressureFor(in, aux_[i].speed);
      applyTilt(aux_[i], in);
      points_[i].azimuth = in.azimuth;
      first = i;
    }
  }
  if (first != static_cast<size_t>(-1)) recomputeFrom(first);
  return true;
}

// ---------------------------------------------------------------------------
// Draw-and-hold shape snapping
// ---------------------------------------------------------------------------

bool StrokeBuilder::tick(double now) {
  const BrushDynamics& dyn = brush_.dynamics;
  if (!active_ || snapped() || !dyn.snapOnHold) return false;
  if (now - holdAnchorTime_ < dyn.holdDuration) return false;
  if (aux_.back().dist * viewScale_ < kMinSnapSizeView) return false;
  if (trySnap()) return true;
  holdAnchorTime_ = 1e300;  // don't retry until the pen moves again
  return false;
}

bool StrokeBuilder::trySnap() {
  if (points_.size() < 3) return false;
  std::vector<Vec2> path;
  path.reserve(points_.size());
  for (const StrokePoint& p : points_) path.push_back(p.pos);
  ShapeRecognizerOptions opts;
  opts.minSize = kMinSnapSizeView * unitsPerViewPoint();
  RecognizedShape shape = recognizeShape(path, opts);
  if (shape.type == ShapeType::None) return false;

  // Uniform attributes from the body of the stroke (tapered ends excluded).
  std::vector<float> widths, alphas, pressures;
  float total = aux_.back().dist;
  for (size_t i = 0; i < points_.size(); ++i) {
    if (aux_[i].dist < total * 0.1f || aux_[i].dist > total * 0.9f) continue;
    widths.push_back(points_[i].width);
    alphas.push_back(points_[i].alpha);
    pressures.push_back(points_[i].pressure);
  }
  if (widths.empty()) {
    for (const StrokePoint& p : points_) {
      widths.push_back(p.width);
      alphas.push_back(p.alpha);
      pressures.push_back(p.pressure);
    }
  }
  StrokePoint templ = points_[points_.size() / 2];
  templ.width = median(widths);
  templ.alpha = median(alphas);
  templ.pressure = median(pressures);
  templ.time = 0.0f;
  templ.flags = 0;

  points_ = shapeToStrokePoints(shape, templ);
  aux_.assign(points_.size(), Aux{});
  for (size_t i = 1; i < points_.size(); ++i)
    aux_[i].dist = aux_[i - 1].dist + distance(points_[i - 1].pos, points_[i].pos);
  snappedShape_ = shape.type;
  predicted_.clear();
  invalidateGeometry();
  return true;
}

void StrokeBuilder::updateSnappedLine(Vec2 pos) {
  // After a line snaps, keep following the pen so the user can adjust the end
  // point before lifting (GoodNotes-style).
  if (snappedShape_ != ShapeType::Line || points_.size() != 2) return;
  points_[1].pos = snapLineEnd(points_[0].pos, pos, ShapeRecognizerOptions{}.angleSnapDegrees);
  aux_[1].dist = distance(points_[0].pos, pos);
  invalidateGeometry();
}

// ---------------------------------------------------------------------------
// Pen-up
// ---------------------------------------------------------------------------

void StrokeBuilder::end(const InputPoint& last) {
  addPoint(last);
  end();
}

void StrokeBuilder::end() {
  if (!active_) return;
  predicted_.clear();
  if (!snapped()) {
    scratchStab_.clear();
    stabilizer_.flush(scratchStab_);
    for (const StabilizedSample& s : scratchStab_) appendControl(s.pos, s.time, s.source);
    // Finish exactly under the pen (removes the 1€ filter's residual lag).
    const StabilizerParams& sp = brush_.dynamics.smoothing.stabilizer;
    if (sp.mode == StabilizerMode::None || sp.catchUp) {
      const uint32_t lastIdx = static_cast<uint32_t>(raw_.size() - 1);
      appendControl(raw_[lastIdx].input.pos, raw_[lastIdx].input.time, lastIdx);
    }
    removeHooks();
    finalizeTapers();
  }
  active_ = false;
  finished_ = true;
  invalidateGeometry();
}

void StrokeBuilder::cancel() {
  resetState();
  active_ = false;
  finished_ = false;
}

void StrokeBuilder::removeHooks() {
  // A hook is a sharp direction change confined to the last (or first) ~0.6 mm
  // of the stroke: the nib flicking as the pen lifts, or skidding on touch-down.
  // Because the 1€ filter rounds a raw reversal into a bend, we look for the
  // first segment inside that tiny window that deviates strongly from the
  // stroke's heading, rather than for an exact back-track. Real letter features
  // are far larger than the window, so handwriting is never altered.
  const size_t n = points_.size();
  if (n < 5) return;
  const float H = brush_.dynamics.hookLength * unitsPerViewPoint();
  if (H <= 0.0f) return;
  const float total = aux_.back().dist;
  if (total < 6.0f * H) return;
  const float kCos = 0.57f;  // > ~55° away from the heading

  // --- End hook ---
  {
    size_t k = n - 1;  // first point inside the tail window
    while (k > 0 && total - aux_[k - 1].dist <= H) --k;
    const size_t base = k > 0 ? k - 1 : 0;
    size_t m = base;
    while (m > 0 && aux_[base].dist - aux_[m].dist < 2.0f * H) --m;
    const Vec2 heading = normalize(points_[base].pos - points_[m].pos, {0.0f, 0.0f});
    if (lengthSq(heading) > 0.0f) {
      for (size_t j = base; j + 1 < n; ++j) {
        Vec2 d = normalize(points_[j + 1].pos - points_[j].pos, heading);
        if (dot(d, heading) < kCos) {
          points_.resize(j + 1);
          aux_.resize(j + 1);
          break;
        }
      }
    }
  }

  // --- Start hook ---
  if (points_.size() >= 5) {
    const size_t cnt = points_.size();
    size_t k = 0;  // last point inside the head window
    while (k + 1 < cnt && aux_[k + 1].dist <= H) ++k;
    const size_t base = std::min(k + 1, cnt - 1);
    size_t m = base;
    while (m + 1 < cnt && aux_[m].dist - aux_[base].dist < 2.0f * H) ++m;
    const Vec2 heading = normalize(points_[m].pos - points_[base].pos, {0.0f, 0.0f});
    if (lengthSq(heading) > 0.0f) {
      for (size_t j = base; j > 0; --j) {
        Vec2 d = normalize(points_[j].pos - points_[j - 1].pos, heading);
        if (dot(d, heading) < kCos) {
          points_.erase(points_.begin(), points_.begin() + static_cast<long>(j));
          aux_.erase(aux_.begin(), aux_.begin() + static_cast<long>(j));
          const float d0 = aux_[0].dist;
          for (Aux& a : aux_) a.dist -= d0;
          aux_[0].dt = 0.0f;
          break;
        }
      }
    }
  }
}

void StrokeBuilder::finalizeTapers() {
  const BrushDynamics& dyn = brush_.dynamics;
  const TaperParams& tp = dyn.taper;
  const float total = aux_.back().dist;
  float ls = tp.startLength * dyn.width;
  float le = tp.endLength * dyn.width;
  if (le > 0.0f && tp.velocityScaledEnd) {
    // Lift-off speed: average over the last few samples (a flick is fast).
    float v = 0.0f;
    size_t cnt = 0;
    for (size_t i = aux_.size(); i-- > 0 && cnt < 4; ++cnt) v += aux_[i].speed;
    v /= static_cast<float>(std::max<size_t>(cnt, 1));
    le *= clamp(v / (0.5f * dyn.velocityReference), 0.35f, 1.6f);
  }
  if (total < 1.5f * dyn.width) {
    // A dot: no tapers, it must stay a round dot.
    startTaperScale_ = 0.0f;
    le = 0.0f;
  } else if (ls + le > 0.85f * total) {
    // Short stroke: shrink both tapers so it never vanishes.
    float k = 0.85f * total / (ls + le);
    startTaperScale_ = k;
    le *= k;
  }
  endTaperLength_ = le;
  recomputeFrom(0);
}

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------

size_t StrokeBuilder::stablePointCount() const {
  const size_t n = points_.size();
  if (!active_) return n;
  if (snapped() || n == 0) return 0;
  size_t s = n - 1;  // the last point can still merge new pressure samples
  size_t from = n > kAwaitWindow ? n - kAwaitWindow : 0;
  for (size_t i = from; i < n; ++i) {
    if (aux_[i].awaiting && lastInputTime_ - aux_[i].time < kAwaitTimeout) {
      s = std::min(s, i);
      break;
    }
  }
  return s;
}

const LiveGeometry& StrokeBuilder::updateGeometry(const TessellationOptions& opts) {
  Rect dirty;
  live_.stableReset = false;
  const bool optsChanged = opts.pixelsPerUnit != geomOpts_.pixelsPerUnit ||
                           opts.maxChordPixels != geomOpts_.maxChordPixels ||
                           opts.maxAngleDegrees != geomOpts_.maxAngleDegrees ||
                           opts.minStampSpacingPixels != geomOpts_.minStampSpacingPixels;
  if (!geomValid_ || optsChanged) {
    dirty.expand(stableGeom_.bounds);
    stableGeom_.clear();
    stableState_ = {};
    stableSegments_ = 0;
    geomOpts_ = opts;
    geomValid_ = true;
    live_.stableReset = true;
  }
  live_.newCapsules = stableGeom_.capsules.size();
  live_.newNibs = stableGeom_.nibs.size();
  live_.newStamps = stableGeom_.stamps.size();

  const size_t n = points_.size();
  const size_t stablePts = stablePointCount();
  const size_t target = finished_ ? CatmullRom::segmentCount(n) : (stablePts >= 2 ? stablePts - 2 : 0);
  if (target > stableSegments_) {
    scratchGeom_.clear();
    Tessellator::tessellateSegments(brush_.style, seed_, points_.data(), n, stableSegments_, target,
                                    geomOpts_, stableState_, scratchGeom_);
    stableGeom_.append(scratchGeom_);
    dirty.expand(scratchGeom_.bounds);
    stableSegments_ = target;
  }

  tailGeom_.clear();
  combined_.assign(points_.begin(), points_.end());
  combined_.insert(combined_.end(), predicted_.begin(), predicted_.end());
  TessellationState st = stableState_;
  Tessellator::tessellateSegments(brush_.style, seed_, combined_.data(), combined_.size(), stableSegments_,
                                  CatmullRom::segmentCount(combined_.size()), geomOpts_, st, tailGeom_);
  dirty.expand(prevTailBounds_);
  dirty.expand(tailGeom_.bounds);
  prevTailBounds_ = tailGeom_.bounds;

  live_.stable = &stableGeom_;
  live_.tail = &tailGeom_;
  live_.dirty = dirty;
  return live_;
}

Stroke StrokeBuilder::buildStroke() const {
  Stroke s;
  s.style = brush_.style;
  s.seed = seed_;
  s.points = points_;
  s.updateBounds();
  return s;
}

}  // namespace ink
