#include "ink/correction.h"
#include "ink/editor.h"
#include "ink/shape_recognizer.h"
#include "ink/stroke_builder.h"
#include "../tools/synth_input.h"
#include "test.h"

using namespace ink;

namespace {

synth::Options wobbly(uint32_t seed) {
  synth::Options o;
  o.seed = seed;
  o.jitter = 0.35f;
  o.tremor = 1.1f;
  return o;
}

/// Draws a path, rests the pen until it snaps, optionally drags, then lifts.
StrokeBuilder holdShape(const std::vector<Vec2>& path, uint32_t seed, Vec2 drag = {}, bool constrained = false) {
  auto in = synth::synthesize(path, wobbly(seed));
  StrokeBuilder b(presets::ballpoint());
  b.begin(in.front());
  for (size_t i = 1; i < in.size(); ++i) b.addPoint(in[i]);
  InputPoint rest = in.back();
  for (int k = 0; k < 170; ++k) {
    rest.time += 1.0 / 240.0;
    b.addPoint(rest);
    b.tick(rest.time);
  }
  b.setShapeConstraint(constrained);
  Vec2 from = rest.pos;
  for (int k = 1; k <= 40 && (drag.x != 0.0f || drag.y != 0.0f); ++k) {
    rest.time += 1.0 / 240.0;
    rest.pos = from + drag * (k / 40.0f);
    b.addPoint(rest);
  }
  b.end();
  return b;
}

float sideSpread(const std::vector<Vec2>& v) {
  float lo = 1e30f, hi = 0.0f;
  for (size_t i = 0; i < v.size(); ++i) {
    float l = distance(v[i], v[(i + 1) % v.size()]);
    lo = std::min(lo, l);
    hi = std::max(hi, l);
  }
  return (hi - lo) / hi;
}

float angleDeg(Vec2 prev, Vec2 at, Vec2 next) {
  Vec2 a = normalize(prev - at), b = normalize(next - at);
  return std::acos(clamp(dot(a, b), -1.0f, 1.0f)) * 180.0f / kPi;
}

std::vector<Vec2> regularNgon(Vec2 c, float r, int n, float phase) {
  std::vector<Vec2> pts;
  for (int k = 0; k <= n; ++k) pts.push_back(c + fromAngle(phase + kTwoPi * k / n) * r);
  return synth::polyline(pts, 0.5f);
}

std::vector<StrokePoint> toPoints(const std::vector<Vec2>& path, float width = 1.4f) {
  std::vector<StrokePoint> out;
  for (Vec2 p : path) {
    StrokePoint sp;
    sp.pos = p;
    sp.width = width;
    out.push_back(sp);
  }
  return out;
}

float roughness(const std::vector<StrokePoint>& p) {
  float r = 0.0f;
  for (size_t i = 2; i < p.size(); ++i) r += lengthSq(p[i].pos - p[i - 1].pos * 2.0f + p[i - 2].pos);
  return r / static_cast<float>(p.size());
}

}  // namespace

// ---------------------------------------------------------------------------
// Draw-and-hold shapes
// ---------------------------------------------------------------------------

TEST(shapes_circles_are_never_polygons) {
  for (uint32_t seed = 1; seed <= 10; ++seed) {
    StrokeBuilder b = holdShape(synth::ellipse({100, 100}, 50, 48, 0.04f, seed), seed);
    CHECK(b.snappedShape() == ShapeType::Circle);
  }
}

TEST(shapes_regular_polygons) {
  for (uint32_t seed = 1; seed <= 6; ++seed) {
    StrokeBuilder hex = holdShape(regularNgon({100, 100}, 50, 6, 0.2f * seed), seed);
    REQUIRE(hex.snappedShape() == ShapeType::Polygon);
    CHECK(hex.shape().vertices.size() == 6);
    CHECK(hex.shape().regular);
    CHECK(sideSpread(hex.shape().vertices) < 0.01f);
    StrokeBuilder pent = holdShape(regularNgon({100, 100}, 50, 5, -kPi / 2), seed + 20);
    REQUIRE(pent.snappedShape() == ShapeType::Polygon);
    CHECK(pent.shape().vertices.size() == 5);
    CHECK(pent.shape().regular);
  }
}

TEST(shapes_square_and_triangles_are_perfected) {
  StrokeBuilder sq = holdShape(synth::polyline({{0, 0}, {100, 3}, {98, 95}, {1, 93}, {0, 1}}), 3);
  REQUIRE(sq.snappedShape() == ShapeType::Rectangle);
  CHECK(sq.shape().regular);
  CHECK(sideSpread(sq.shape().vertices) < 0.01f);
  // A clear rectangle stays a rectangle.
  StrokeBuilder rect = holdShape(synth::polyline({{0, 0}, {140, 2}, {139, 80}, {1, 79}, {0, 1}}), 4);
  REQUIRE(rect.snappedShape() == ShapeType::Rectangle);
  CHECK(!rect.shape().regular);

  StrokeBuilder eq = holdShape(synth::polyline({{50, 0}, {101, 88}, {-2, 86}, {49, 2}}), 5);
  REQUIRE(eq.snappedShape() == ShapeType::Triangle);
  CHECK(eq.shape().regular);
  CHECK(sideSpread(eq.shape().vertices) < 0.01f);
  // Flat base: one side exactly horizontal.
  bool flat = false;
  for (size_t i = 0; i < 3; ++i)
    flat = flat || std::fabs(eq.shape().vertices[i].y - eq.shape().vertices[(i + 1) % 3].y) < 1e-3f;
  CHECK(flat);

  StrokeBuilder right = holdShape(synth::polyline({{0, 0}, {2, 90}, {150, 91}, {1, 2}}), 6);
  REQUIRE(right.snappedShape() == ShapeType::Triangle);
  const auto& v = right.shape().vertices;
  float best = 180.0f;
  for (size_t i = 0; i < 3; ++i) best = std::min(best, std::fabs(angleDeg(v[(i + 2) % 3], v[i], v[(i + 1) % 3]) - 90.0f));
  CHECK(best < 0.05f);
}

TEST(shapes_arcs) {
  // Semicircle (drawn over the top) snaps to exactly 180° with a level chord.
  StrokeBuilder semi = holdShape(synth::ellipse({100, 100}, 50, 49, 0.02f, 3, kPi * 1.02f, kPi * 0.97f), 7);
  REQUIRE(semi.snappedShape() == ShapeType::Arc);
  CHECK_NEAR(std::fabs(semi.shape().sweep), kPi, 1e-4);
  Vec2 a = semi.shape().center + fromAngle(semi.shape().startAngle) * semi.shape().radii.x;
  Vec2 b = semi.shape().center + fromAngle(semi.shape().startAngle + semi.shape().sweep) * semi.shape().radii.x;
  CHECK_NEAR(a.y, b.y, 1e-3);
  // A 120° arc keeps its sweep.
  StrokeBuilder arc = holdShape(synth::ellipse({100, 100}, 60, 60, 0.01f, 4, 0.3f, 2.1f), 8);
  REQUIRE(arc.snappedShape() == ShapeType::Arc);
  CHECK_NEAR(std::fabs(arc.shape().sweep), 2.1, 0.12);
}

TEST(shapes_irregular_quad_and_handwriting) {
  StrokeBuilder trap = holdShape(synth::polyline({{20, 0}, {100, 0}, {130, 70}, {0, 70}, {20, 0}}), 9);
  CHECK(trap.snappedShape() == ShapeType::Quad);
  // Korean handwriting held at the end must stay handwriting.
  auto glyphs = synth::hangul(U'라', {0, 0}, 40, 3, 0.5f);
  StrokeBuilder ra = holdShape(glyphs[0], 10);
  CHECK(ra.snappedShape() == ShapeType::None);
  StrokeBuilder loops = holdShape(synth::cursiveLoops({0, 0}, 3, 30), 11);
  CHECK(loops.snappedShape() == ShapeType::None);
}

TEST(shapes_drag_after_snap_resizes) {
  // Circle drawn from the top; dragging the pen 30 up grows the radius by ~30.
  StrokeBuilder grow = holdShape(synth::ellipse({100, 100}, 40, 40, 0.03f, 2), 12, {0, -30});
  REQUIRE(grow.snappedShape() == ShapeType::Circle);
  CHECK_NEAR(grow.shape().radii.x, 70.0, 3.0);
  CHECK(distance(grow.shape().center, {100, 100}) < 2.0f);
  // Rectangle: pulling the corner straight outwards scales without rotating.
  auto rectPath = synth::polyline({{0, 0}, {120, 1}, {119, 60}, {1, 59}, {0, 1}});
  StrokeBuilder scaled = holdShape(rectPath, 13, Vec2{-60, -30} * 0.5f);
  REQUIRE(scaled.snappedShape() == ShapeType::Rectangle);
  const auto& v = scaled.shape().vertices;
  Vec2 e = v[1] - v[0];
  CHECK(std::fabs(e.y) < 0.02f * length(e) + 1e-3f);  // still axis aligned
  CHECK(length(e) > 150.0f);
}

TEST(shapes_constraint_mode) {
  StrokeBuilder ell = holdShape(synth::ellipse({100, 100}, 80, 40, 0.03f, 5), 14, {}, true);
  REQUIRE(ell.snappedShape() == ShapeType::Ellipse);
  CHECK(ell.shape().type == ShapeType::Circle);
  CHECK(ell.shape().regular);
  StrokeBuilder rect = holdShape(synth::polyline({{0, 0}, {140, 2}, {139, 80}, {1, 79}, {0, 1}}), 15, {}, true);
  CHECK(rect.shape().regular);
  StrokeBuilder line = holdShape(synth::polyline({{0, 0}, {100, 33}}), 16, {}, true);
  REQUIRE(line.snappedShape() == ShapeType::Line);
  Vec2 d = line.shape().vertices[1] - line.shape().vertices[0];
  float deg = std::atan2(d.y, d.x) * 180.0f / kPi;
  CHECK_NEAR(deg, std::round(deg / 15.0f) * 15.0f, 1e-3);
}

TEST(shapes_transform_and_regularize_helpers) {
  RecognizedShape s;
  s.type = ShapeType::Polygon;
  for (int k = 0; k < 5; ++k) s.vertices.push_back(Vec2{50, 50} + fromAngle(0.1f + kTwoPi * k / 5) * (30.0f + k));
  RecognizedShape r = regularizeShape(s, {}, true);
  CHECK(r.regular);
  // Flat bottom: the lowest two vertices share y.
  std::vector<float> ys;
  for (Vec2 v : r.vertices) ys.push_back(v.y);
  std::sort(ys.begin(), ys.end());
  CHECK_NEAR(ys[3], ys[4], 1e-3);
  RecognizedShape t = transformShape(r, Affine::translate(10, 0) * Affine::scale(2));
  CHECK_NEAR(distance(t.vertices[0], t.vertices[1]), 2.0 * distance(r.vertices[0], r.vertices[1]), 1e-3);
  Affine d = dragTransform({0, 0}, {10, 0}, {20, 0}, true, 0.1f);
  CHECK(distance(d.apply({10, 0}), {20, 0}) < 1e-5f);
}

// ---------------------------------------------------------------------------
// 글씨 보정: stroke correction
// ---------------------------------------------------------------------------

TEST(correction_off_is_identity) {
  auto pts = toPoints(synth::polyline({{0, 0}, {50, 3}, {60, 40}}));
  auto copy = pts;
  CorrectionParams p;
  CHECK(!correctStroke(pts, p));
  for (size_t i = 0; i < pts.size(); ++i) CHECK(pts[i].pos == copy[i].pos);
}

TEST(correction_makes_near_horizontal_exact) {
  // ㅡ written with a 4° tilt and some tremor.
  std::vector<Vec2> path;
  for (int i = 0; i <= 80; ++i) {
    float x = i * 0.75f;
    path.push_back({x, x * std::tan(4.0f * kPi / 180.0f) + 0.15f * std::sin(i * 0.9f)});
  }
  auto pts = toPoints(path);
  CorrectionParams p;
  p.strength = 0.5f;
  CorrectionReport rep;
  REQUIRE(correctStroke(pts, p, 1.0f, &rep));
  CHECK(rep.straightened == 1);
  CHECK(rep.axisSnapped == 1);
  for (const StrokePoint& q : pts) CHECK_NEAR(q.pos.y, pts.front().pos.y, 1e-4);
  CHECK(pts.size() == path.size());
  CHECK_NEAR(pts.front().pos.x, 0.0, 1e-4);
  CHECK_NEAR(pts.back().pos.x, 60.0, 1e-4);
}

TEST(correction_keeps_corners_connected) {
  // ㄱ: tilted top bar, then a tilted downward leg — one stroke.
  auto path = synth::polyline({{0, 0}, {30, 1.6f}, {28.5f, 32}}, 0.5f);
  auto pts = toPoints(path);
  CorrectionParams p;
  p.strength = 0.6f;
  CorrectionReport rep;
  REQUIRE(correctStroke(pts, p, 1.0f, &rep));
  CHECK(rep.corners == 1);
  CHECK(rep.axisSnapped == 2);
  // Find the corner: the point with the largest x.
  size_t c = 0;
  for (size_t i = 0; i < pts.size(); ++i)
    if (pts[i].pos.x > pts[c].pos.x) c = i;
  CHECK_NEAR(pts.front().pos.y, pts[c].pos.y, 1e-3);   // top bar horizontal
  CHECK_NEAR(pts.back().pos.x, pts[c].pos.x, 1e-3);    // leg vertical
  CHECK_NEAR(angleDeg(pts.front().pos, pts[c].pos, pts.back().pos), 90.0, 0.05);
}

TEST(correction_preserves_curves_and_diagonals) {
  // ㅇ with tremor: smoother, same size, nothing straightened.
  auto circle = synth::synthesize(synth::ellipse({50, 50}, 12, 12, 0.0f, 1), wobbly(3));
  std::vector<Vec2> path;
  for (const InputPoint& q : circle) path.push_back(q.pos);
  auto pts = toPoints(path);
  float before = roughness(pts);
  CorrectionParams p;
  p.strength = 1.0f;
  CorrectionReport rep;
  correctStroke(pts, p, 1.0f, &rep);
  CHECK(rep.straightened == 0);
  CHECK(roughness(pts) < before * 0.5f);
  float meanR = 0.0f;
  for (const StrokePoint& q : pts) meanR += distance(q.pos, {50, 50});
  meanR /= static_cast<float>(pts.size());
  CHECK_NEAR(meanR, 12.0, 0.4);  // no shrinking
  // A 30° diagonal (ㅅ) is straightened but not snapped to an axis.
  auto diag = toPoints(synth::polyline({{0, 0}, {26, 15.2f}, {52, 30}}, 0.5f));
  CorrectionReport r2;
  correctStroke(diag, p, 1.0f, &r2);
  CHECK(r2.axisSnapped == 0);
  Vec2 d = diag.back().pos - diag.front().pos;
  CHECK_NEAR(std::atan2(d.y, d.x) * 180.0 / kPi, 30.0, 0.5);
}

TEST(correction_in_builder_and_on_existing_strokes) {
  auto strokes = synth::writeLine(U"나다", {10, 10}, 30, 4, 1.0f);
  Brush pen = presets::ballpoint();
  pen.dynamics.correction.strength = 0.7f;
  int snapped = 0;
  PageEditor ed;
  std::vector<StrokeId> raw;
  for (const auto& ws : strokes) {
    StrokeBuilder b(pen);
    b.begin(ws.input.front());
    for (size_t i = 1; i < ws.input.size(); ++i) b.addPoint(ws.input[i]);
    b.end();
    snapped += b.correctionReport().axisSnapped;
    // Same input without correction goes onto the page for the editor test.
    StrokeBuilder plain(presets::ballpoint());
    plain.begin(ws.input.front());
    for (size_t i = 1; i < ws.input.size(); ++i) plain.addPoint(ws.input[i]);
    plain.end();
    raw.push_back(ed.commitStroke(plain));
  }
  CHECK(snapped >= 6);  // ㄴ ㄷ ㅏ ...: most segments are axis aligned
  std::vector<StrokePtr> before = ed.page().layers()[0].strokes();
  CorrectionParams p;
  p.strength = 0.7f;
  REQUIRE(ed.correct(raw, p));
  CHECK(ed.page().layers()[0].at(0) != before[0]);
  CHECK(ed.page().layers()[0].at(0)->id == before[0]->id);
  ed.undo();
  for (size_t i = 0; i < before.size(); ++i) CHECK(ed.page().layers()[0].at(i) == before[i]);
}

TEST(correction_keeps_gentle_curves_curved) {
  // A curve drawn downwards — the side of ( or ), a descender, a long sweep —
  // used to be flattened into a (vertical) straight line at pen-up when it
  // bulged less than a few percent of its length.
  auto bulge = [](const std::vector<StrokePoint>& pts) {
    Vec2 A = pts.front().pos, B = pts.back().pos;
    float m = 0.0f;
    for (const StrokePoint& q : pts) m = std::max(m, pointSegmentDistance(q.pos, A, B));
    return m;
  };
  for (float strength : {0.3f, 0.6f, 1.0f}) {
    for (float L : {40.0f, 80.0f, 150.0f}) {
      for (float rel : {0.03f, 0.06f}) {
        const float h = L * rel;
        std::vector<Vec2> path;
        for (int i = 0; i <= 200; ++i) {
          float t = i / 200.0f;
          path.push_back({50.0f + h * 4.0f * t * (1.0f - t), 10.0f + L * t});
        }
        Brush pen = presets::ballpoint();
        pen.dynamics.correction.strength = strength;
        pen.dynamics.snapOnHold = false;
        auto in = synth::synthesize(path, synth::Options{});
        StrokeBuilder b(pen);
        b.begin(in.front());
        for (size_t i = 1; i < in.size(); ++i) b.addPoint(in[i]);
        b.end();
        CHECK(b.correctionReport().straightened == 0);
        CHECK(b.correctionReport().axisSnapped == 0);
        CHECK_NEAR(bulge(b.points()), h, 0.12f * h + 0.1f);
      }
    }
  }
  // A wobbly but intended straight line is still straightened.
  std::vector<Vec2> line;
  for (int i = 0; i <= 160; ++i) line.push_back({20.0f + 0.4f * std::sin(i * 0.31f), 10.0f + i * 0.5f});
  auto pts = toPoints(line);
  CorrectionParams p;
  p.strength = 0.3f;
  CorrectionReport rep;
  correctStroke(pts, p, 1.0f, &rep);
  CHECK(rep.straightened == 1);
  CHECK(rep.axisSnapped == 1);
}

TEST(correction_live_smoothing_follows_the_pen) {
  // GoodNotes-style live correction: the ink at the pen is under the pen, the
  // trail behind it settles into the smoothed curve while writing, settled
  // points never move again, and pen-up does not make the curve jump.
  auto in = synth::synthesize(synth::cursiveLoops({10, 50}, 4, 30), wobbly(7));
  for (float strength : {0.3f, 1.0f}) {
    Brush pen = presets::ballpoint();
    pen.dynamics.correction.strength = strength;
    pen.dynamics.correction.straightenLines = false;  // compare curves only
    pen.dynamics.snapOnHold = false;
    Brush plainPen = pen;
    plainPen.dynamics.correction.strength = 0.0f;
    StrokeBuilder b(pen), plain(plainPen);
    b.begin(in.front());
    plain.begin(in.front());
    std::vector<Vec2> frozen;
    for (size_t i = 1; i < in.size(); ++i) {
      b.addPoint(in[i]);
      plain.addPoint(in[i]);
      // Same control points, same pen position: no lag at the pen.
      REQUIRE(b.points().size() == plain.points().size());
      CHECK(b.points().back().pos == plain.points().back().pos);
      // Points that were stable stay exactly where they were.
      for (size_t k = 0; k < frozen.size(); ++k) CHECK(b.points()[k].pos == frozen[k]);
      for (size_t k = frozen.size(); k < b.stablePointCount(); ++k) frozen.push_back(b.points()[k].pos);
    }
    CHECK(frozen.size() > b.points().size() / 2);
    // The trail is visibly smoothed while the pen is still down.
    CHECK(roughness(b.points()) < roughness(plain.points()) * 0.85f);
    std::vector<StrokePoint> live = b.points();
    b.end();
    // Pen-up: the corrected stroke is what was shown (corners may tighten a little).
    std::vector<float> moved;
    for (size_t k = 0; k < std::min(live.size(), b.points().size()); ++k)
      moved.push_back(distance(live[k].pos, b.points()[k].pos));
    std::sort(moved.begin(), moved.end());
    CHECK(moved[moved.size() * 9 / 10] < 0.05f);
  }
}

// ---------------------------------------------------------------------------
// 글씨 보정: line straightening
// ---------------------------------------------------------------------------

namespace {
struct LineStats {
  float slopeDeg;
  float spread;  // max - min of word centre y
  float meanY;
};

/// Word centres (ink-weighted mean y) of a written line, given its stroke ids.
LineStats measure(const Layer& layer, const std::vector<StrokeId>& ids, const std::vector<size_t>& wordOf) {
  std::vector<Vec2> sum(8);
  std::vector<float> cnt(8, 0.0f);
  for (size_t k = 0; k < ids.size(); ++k) {
    const Stroke& s = *layer.at(static_cast<size_t>(layer.indexOf(ids[k])));
    for (const StrokePoint& p : s.points) {
      sum[wordOf[k]] += p.pos;
      cnt[wordOf[k]] += 1.0f;
    }
  }
  std::vector<Vec2> c;
  for (size_t w = 0; w < 8; ++w)
    if (cnt[w] > 0) c.push_back(sum[w] / cnt[w]);
  float lo = 1e30f, hi = -1e30f, my = 0.0f;
  for (Vec2 q : c) {
    lo = std::min(lo, q.y);
    hi = std::max(hi, q.y);
    my += q.y;
  }
  float slope = std::atan2(c.back().y - c.front().y, c.back().x - c.front().x) * 180.0f / kPi;
  return {slope, hi - lo, my / static_cast<float>(c.size())};
}
}  // namespace

TEST(straighten_lines_removes_slant_and_drift) {
  PageEditor ed(600, 400);
  Brush pen = presets::ballpoint();
  // Two lines with opposite slants; words written at drifting heights.
  std::vector<float> drift = {0, 0, 0, 6, 6, 6, 6, 0, -5, -5, -5};
  std::vector<StrokeId> ids1, ids2;
  std::vector<size_t> w1, w2;
  auto write = [&](const std::u32string& text, Vec2 at, float slant, std::vector<StrokeId>& ids, std::vector<size_t>& words) {
    for (const auto& ws : synth::writeLine(text, at, 26, 3, 0.4f, slant, drift)) {
      StrokeBuilder b(pen);
      b.begin(ws.input.front());
      for (size_t i = 1; i < ws.input.size(); ++i) b.addPoint(ws.input[i]);
      b.end();
      ids.push_back(ed.commitStroke(b));
      size_t word = 0;
      for (size_t i = 0; i < ws.syllable; ++i)
        if (text[i] == U' ') ++word;
      words.push_back(word);
    }
  };
  write(U"가나다 라마바사 아한글", {20, 40}, 0.09f, ids1, w1);
  write(U"보정 가나다라 마바", {20, 150}, -0.07f, ids2, w2);
  const Layer& layer = ed.page().layers()[0];
  LineStats a0 = measure(layer, ids1, w1), b0 = measure(layer, ids2, w2);
  CHECK(std::fabs(a0.slopeDeg) > 3.0f);
  CHECK(std::fabs(b0.slopeDeg) > 3.0f);

  std::vector<StrokeId> all = ids1;
  all.insert(all.end(), ids2.begin(), ids2.end());
  REQUIRE(ed.straighten(all));
  LineStats a1 = measure(layer, ids1, w1), b1 = measure(layer, ids2, w2);
  CHECK(std::fabs(a1.slopeDeg) < 0.8f);
  CHECK(std::fabs(b1.slopeDeg) < 0.8f);
  CHECK(a1.spread < 1.5f);
  CHECK(b1.spread < 1.5f);
  CHECK(b1.meanY - a1.meanY > 60.0f);  // lines stay separate
  CHECK(layer.size() == all.size());
  ed.undo();
  LineStats a2 = measure(layer, ids1, w1);
  CHECK_NEAR(a2.slopeDeg, a0.slopeDeg, 1e-3);
}

TEST(straighten_leaves_level_text_alone) {
  PageEditor ed;
  std::vector<StrokeId> ids;
  for (const auto& ws : synth::writeLine(U"가나다라 마바", {20, 40}, 26, 3, 0.0f)) {
    StrokeBuilder b(presets::ballpoint());
    b.begin(ws.input.front());
    for (size_t i = 1; i < ws.input.size(); ++i) b.addPoint(ws.input[i]);
    b.end();
    ids.push_back(ed.commitStroke(b));
  }
  std::vector<StrokePtr> before = ed.page().layers()[0].strokes();
  ed.straighten(ids);
  float maxMove = 0.0f;
  for (size_t i = 0; i < before.size(); ++i) {
    const Stroke& a = *before[i];
    const Stroke& b = *ed.page().layers()[0].at(i);
    for (size_t k = 0; k < a.points.size(); ++k) maxMove = std::max(maxMove, distance(a.points[k].pos, b.points[k].pos));
  }
  CHECK(maxMove < 1.0f);
}
