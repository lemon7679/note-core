#include "ink/stroke_builder.h"
#include "../tools/synth_input.h"
#include "test.h"

using namespace ink;

namespace {

Stroke drawWith(StrokeBuilder& b, const std::vector<InputPoint>& in) {
  b.begin(in.front());
  for (size_t i = 1; i < in.size(); ++i) b.addPoint(in[i]);
  b.end();
  return b.buildStroke();
}

Stroke drawWith(const Brush& brush, const std::vector<InputPoint>& in) {
  StrokeBuilder b(brush);
  return drawWith(b, in);
}

float roughness(const std::vector<Vec2>& p) {
  float r = 0.0f;
  for (size_t i = 2; i < p.size(); ++i) r += lengthSq(p[i] - p[i - 1] * 2.0f + p[i - 2]);
  return r / static_cast<float>(p.size());
}

std::vector<InputPoint> line(Vec2 a, Vec2 b, float pressure = 0.35f, float speed = 300.0f) {
  std::vector<InputPoint> out;
  float len = distance(a, b);
  int n = std::max(2, static_cast<int>(len / speed * 240.0f));
  for (int i = 0; i <= n; ++i) {
    InputPoint p;
    p.pos = lerp(a, b, static_cast<float>(i) / n);
    p.pressure = pressure;
    p.altitude = 1.2f;
    p.time = i / 240.0;
    p.updateId = static_cast<uint64_t>(i + 1);
    out.push_back(p);
  }
  return out;
}

}  // namespace

TEST(builder_ballpoint_basic) {
  synth::Options o;
  auto in = synth::synthesize(synth::cursiveLoops({10, 50}, 3, 30), o);
  Brush brush = presets::ballpoint();
  Stroke s = drawWith(brush, in);
  REQUIRE(s.points.size() > 20);
  for (const StrokePoint& p : s.points) {
    CHECK(p.width >= brush.dynamics.width * 0.70f);
    CHECK(p.width <= brush.dynamics.width * 1.13f);
    CHECK_NEAR(p.alpha, 1.0, 1e-6);
  }
  CHECK(distance(s.points.front().pos, in.front().pos) < 1.0f);
  CHECK(distance(s.points.back().pos, in.back().pos) < 1.0f);
  CHECK(!s.bounds.isEmpty());
}

TEST(builder_smooths_sensor_jitter) {
  synth::Options noisy;
  noisy.jitter = 0.6f;
  noisy.tremor = 0.0f;
  auto in = synth::synthesize(synth::wave({0, 0}, 300, 20, 120), noisy);
  std::vector<Vec2> raw;
  for (const InputPoint& p : in) raw.push_back(p.pos);
  Stroke s = drawWith(presets::ballpoint(), in);
  std::vector<Vec2> out;
  for (const StrokePoint& p : s.points) out.push_back(p.pos);
  CHECK(roughness(out) < roughness(raw) * 0.5f);
}

TEST(builder_tap_keeps_peak_pressure) {
  std::vector<InputPoint> tap;
  for (int k = 0; k < 15; ++k) {
    InputPoint p;
    p.pos = {10.0f + 0.01f * k, 10.0f};
    p.pressure = 0.5f * std::sin(kPi * (k + 0.5f) / 15.0f);
    p.time = k / 240.0;
    tap.push_back(p);
  }
  Brush brush = presets::fountainPen();
  Stroke s = drawWith(brush, tap);
  REQUIRE(s.points.size() == 1);
  float expected = brush.dynamics.width *
                   lerp(brush.dynamics.pressureWidthMin, brush.dynamics.pressureWidthMax,
                        brush.dynamics.pressureCurve.evaluate(0.5f * std::sin(kPi * 7.5f / 15.0f)));
  CHECK_NEAR(s.points[0].width, expected, expected * 0.05);
}

TEST(builder_estimated_pressure_updates) {
  auto in = line({0, 0}, {50, 0});  // ~0.17 s: within the estimate timeout
  for (InputPoint& p : in) {
    p.flags = kInputPressureExpectsUpdate;
    p.pressure = 0.1f;
  }
  Brush brush = presets::fountainPen();
  StrokeBuilder b(brush);
  b.begin(in.front());
  for (size_t i = 1; i < in.size(); ++i) b.addPoint(in[i]);
  CHECK(b.stablePointCount() == 0);  // everything still awaits a refined force
  float before = b.points().back().width;
  for (const InputPoint& p : in) b.updateEstimatedProperties(p.updateId, 0.8f, -1.0f, 0.0f);
  CHECK(b.stablePointCount() == b.points().size() - 1);
  CHECK(b.points().back().width > before * 1.5f);
  CHECK(!b.updateEstimatedProperties(999999, 0.5f, -1.0f, 0.0f));
}

TEST(builder_live_stable_geometry_is_prefix_of_final) {
  synth::Options o;
  auto in = synth::synthesize(synth::cursiveLoops({10, 50}, 4, 30), o);
  for (BrushKind k : {BrushKind::Ballpoint, BrushKind::Pencil, BrushKind::Highlighter}) {
    Brush brush = presets::forKind(k);
    StrokeBuilder b(brush);
    b.begin(in.front());
    TessellationOptions t;
    for (size_t i = 1; i < in.size(); ++i) {
      b.addPoint(in[i]);
      if (i % 5) continue;
      const LiveGeometry& g = b.updateGeometry(t);
      // Fresh tessellation of the current control points must start with the stable prims.
      StrokeGeometry fresh;
      Tessellator::tessellate(brush.style, b.buildStroke().seed, b.points().data(), b.points().size(), t, fresh);
      REQUIRE(fresh.capsules.size() >= g.stable->capsules.size());
      REQUIRE(fresh.stamps.size() >= g.stable->stamps.size());
      REQUIRE(fresh.nibs.size() >= g.stable->nibs.size());
      for (size_t c = 0; c < g.stable->capsules.size(); ++c) CHECK(fresh.capsules[c].p1 == g.stable->capsules[c].p1);
      for (size_t c = 0; c < g.stable->stamps.size(); ++c) CHECK(fresh.stamps[c].center == g.stable->stamps[c].center);
      for (size_t c = 0; c < g.stable->nibs.size(); ++c) CHECK(fresh.nibs[c].p1 == g.stable->nibs[c].p1);
    }
    b.end();
  }
}

TEST(builder_removes_lift_off_hook) {
  auto in = line({0, 0}, {120, 0});
  // Tiny hook: the pen flicks back down-left over the last ~1 pt.
  double t = in.back().time;
  for (int k = 1; k <= 4; ++k) {
    InputPoint p = in.back();
    p.pos = {120.0f - 0.25f * k, 0.25f * k};
    p.time = t + k / 240.0;
    in.push_back(p);
  }
  Stroke s = drawWith(presets::ballpoint(), in);
  float maxX = 0.0f;
  for (const StrokePoint& p : s.points) maxX = std::max(maxX, p.pos.x);
  CHECK(s.points.back().pos.x > maxX - 0.3f);
  CHECK(std::fabs(s.points.back().pos.y) < 0.3f);
}

TEST(builder_removes_touch_down_skid) {
  std::vector<InputPoint> in;
  // Nib skids ~1 pt down-left on touch-down, then the real stroke goes right.
  for (int k = 0; k < 4; ++k) {
    InputPoint p;
    p.pos = {1.0f - 0.25f * k, -1.0f + 0.25f * k};
    p.pressure = 0.35f;
    p.time = k / 240.0;
    in.push_back(p);
  }
  auto rest = line({0, 0}, {120, 0});
  for (InputPoint& p : rest) {
    p.time += 4 / 240.0;
    in.push_back(p);
  }
  Stroke s = drawWith(presets::ballpoint(), in);
  REQUIRE(s.points.size() > 10);
  // The stroke no longer starts by travelling backwards / sideways.
  for (size_t i = 1; i < 8; ++i) {
    Vec2 d = normalize(s.points[i].pos - s.points[i - 1].pos);
    CHECK(d.x > 0.57f);
  }
}

TEST(builder_keeps_real_corners_near_ends) {
  // A "ㄱ": the final down-stroke is long, so its corner is not a hook.
  auto in = line({0, 0}, {40, 0});
  auto down = line({40, 0}, {40, 12});
  for (InputPoint& p : down) {
    p.time += in.back().time + 1 / 240.0;
    in.push_back(p);
  }
  Stroke s = drawWith(presets::ballpoint(), in);
  CHECK(s.points.back().pos.y > 11.0f);
}

TEST(builder_tapers) {
  Brush brush = presets::fountainPen();
  synth::Options o;
  o.pressureWave = 0.0f;
  Stroke s = drawWith(brush, synth::synthesize(synth::polyline({{0, 0}, {150, 0}}), o));
  REQUIRE(s.points.size() > 10);
  float mid = s.points[s.points.size() / 2].width;
  CHECK(s.points.back().width < mid * 0.6f);
  CHECK(s.points.front().width < mid * 0.6f);
  // Ballpoint: no tapers at all.
  Stroke b = drawWith(presets::ballpoint(), line({0, 0}, {150, 0}));
  CHECK_NEAR(b.points.back().width, b.points[b.points.size() / 2].width, 0.05);
}

TEST(builder_short_stroke_does_not_vanish) {
  Brush brush = presets::brushPen();  // long tapers
  Stroke s = drawWith(brush, line({0, 0}, {4, 0}, 0.4f));
  float maxW = 0.0f;
  for (const StrokePoint& p : s.points) maxW = std::max(maxW, p.width);
  CHECK(maxW > brush.dynamics.width * 0.3f);
}

TEST(builder_draw_and_hold_snaps_shapes) {
  auto hold = [](std::vector<InputPoint> in, const Brush& brush) {
    StrokeBuilder b(brush);
    b.begin(in.front());
    for (size_t i = 1; i < in.size(); ++i) b.addPoint(in[i]);
    InputPoint rest = in.back();
    for (int k = 0; k < 200; ++k) {
      rest.time += 1.0 / 240.0;
      b.addPoint(rest);
      b.tick(rest.time);
    }
    b.end();
    return b.snappedShape();
  };
  synth::Options o;
  o.jitter = 0.3f;
  o.tremor = 0.8f;
  Brush pen = presets::ballpoint();
  CHECK(hold(synth::synthesize(synth::polyline({{0, 0}, {200, 4}}), o), pen) == ShapeType::Line);
  CHECK(hold(synth::synthesize(synth::ellipse({100, 100}, 50, 50, 0.04f, 1), o), pen) == ShapeType::Circle);
  CHECK(hold(synth::synthesize(synth::polyline({{0, 0}, {120, 2}, {119, 80}, {1, 79}, {0, 1}}), o), pen) ==
        ShapeType::Rectangle);
  // Handwriting must never be "corrected".
  CHECK(hold(synth::synthesize(synth::cursiveLoops({0, 0}, 3, 30), o), pen) == ShapeType::None);
  CHECK(hold(synth::synthesize(synth::scribble({0, 0}, 120, 14, 3), o), pen) == ShapeType::None);
  // Without a hold nothing snaps.
  Stroke s = drawWith(pen, synth::synthesize(synth::polyline({{0, 0}, {200, 4}}), o));
  CHECK(s.points.size() > 2);
}

TEST(builder_snapped_line_follows_pen) {
  StrokeBuilder b(presets::highlighter());
  auto in = line({0, 0}, {200, 0});
  b.begin(in.front());
  for (size_t i = 1; i < in.size(); ++i) b.addPoint(in[i]);
  InputPoint rest = in.back();
  for (int k = 0; k < 150; ++k) {
    rest.time += 1.0 / 240.0;
    b.addPoint(rest);
    b.tick(rest.time);
  }
  REQUIRE(b.snappedShape() == ShapeType::Line);
  for (int k = 1; k <= 60; ++k) {
    rest.time += 1.0 / 240.0;
    rest.pos = {200.0f + k, 0.5f * k};
    b.addPoint(rest);
  }
  b.end();
  Stroke s = b.buildStroke();
  REQUIRE(s.points.size() == 2);
  CHECK(distance(s.points[1].pos, {260, 30}) < 3.0f);
}

TEST(builder_prediction_is_not_committed) {
  StrokeBuilder b(presets::ballpoint());
  auto in = line({0, 0}, {50, 0});
  b.begin(in.front());
  for (size_t i = 1; i < in.size(); ++i) b.addPoint(in[i]);
  std::vector<InputPoint> pred;
  for (int k = 1; k <= 3; ++k) {
    InputPoint p = in.back();
    p.pos.x += 3.0f * k;
    p.time += k / 240.0;
    pred.push_back(p);
  }
  b.setPredictedPoints(pred.data(), pred.size());
  CHECK(b.predicted().size() == 3);
  TessellationOptions t;
  const LiveGeometry& g = b.updateGeometry(t);
  CHECK(g.tail->bounds.maxX > 52.0f);
  b.end();
  Stroke s = b.buildStroke();
  for (const StrokePoint& p : s.points) CHECK(p.pos.x <= 50.5f);
}

TEST(builder_pulled_string_catches_up) {
  Brush brush = presets::ballpoint();
  brush.dynamics.smoothing.stabilizer.mode = StabilizerMode::PulledString;
  brush.dynamics.smoothing.stabilizer.amount = 15.0f;
  Stroke s = drawWith(brush, line({0, 0}, {100, 0}));
  CHECK(distance(s.points.back().pos, {100, 0}) < 0.5f);
}

TEST(builder_feel_is_zoom_invariant) {
  // The same physical hand motion at 4x zoom produces the same stroke, scaled.
  synth::Options o;
  auto in = synth::synthesize(synth::cursiveLoops({10, 50}, 2, 30), o);
  Stroke a = drawWith(presets::ballpoint(), in);
  for (InputPoint& p : in) p.pos = p.pos * 0.25f;
  StrokeBuilder b(presets::ballpoint());
  b.setViewScale(4.0f);
  Stroke s = drawWith(b, in);
  REQUIRE(a.points.size() == s.points.size());
  for (size_t i = 0; i < a.points.size(); ++i) CHECK(distance(a.points[i].pos, s.points[i].pos * 4.0f) < 1e-3f);
}

TEST(builder_touch_input_simulates_pressure) {
  auto in = line({0, 0}, {100, 0}, -1.0f);
  for (InputPoint& p : in) p.source = InputSource::Touch;
  Stroke s = drawWith(presets::fountainPen(), in);
  REQUIRE(!s.points.empty());
  float w = s.points[s.points.size() / 2].width;
  CHECK(w > 0.3f * presets::fountainPen().dynamics.width);
}

TEST(builder_pencil_tilt_widens) {
  Brush pencil = presets::pencil();
  auto upright = line({0, 0}, {100, 0});
  auto tilted = upright;
  for (InputPoint& p : tilted) p.altitude = 0.3f;
  Stroke a = drawWith(pencil, upright), b = drawWith(pencil, tilted);
  float wa = a.points[a.points.size() / 2].width, wb = b.points[b.points.size() / 2].width;
  CHECK(wb > wa * 3.0f);
  CHECK(b.points[b.points.size() / 2].tilt > 0.9f);
  CHECK(b.points[b.points.size() / 2].alpha < a.points[a.points.size() / 2].alpha);
}
