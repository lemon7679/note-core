#include <memory>

#include "ink/editor.h"
#include "ink/rasterizer.h"
#include "ink/serialization.h"
#include "ink/svg_export.h"
#include "../tools/synth_input.h"
#include "test.h"

using namespace ink;

namespace {

Stroke straight(Vec2 a, Vec2 b, const Brush& brush = presets::ballpoint(), int n = 40) {
  Stroke s;
  s.style = brush.style;
  for (int i = 0; i <= n; ++i) {
    StrokePoint p;
    p.pos = lerp(a, b, static_cast<float>(i) / n);
    p.width = brush.dynamics.width;
    p.time = i / 240.0f;
    s.points.push_back(p);
  }
  s.updateBounds();
  return s;
}

Stroke drawn(const Brush& brush, const std::vector<Vec2>& path, uint32_t seed) {
  synth::Options o;
  o.seed = seed;
  auto in = synth::synthesize(path, o);
  StrokeBuilder b(brush);
  b.begin(in.front());
  for (size_t i = 1; i < in.size(); ++i) b.addPoint(in[i]);
  b.end();
  return b.buildStroke();
}

}  // namespace

TEST(page_layers_and_ids) {
  Page page;
  REQUIRE(page.layers().size() == 1);
  Layer& l2 = page.addLayer("ink");
  CHECK(l2.id() != page.layers()[0].id());
  StrokeId a = page.allocateStrokeId(), b = page.allocateStrokeId();
  CHECK(b == a + 1);
  CHECK(page.layer(l2.id()) != nullptr);
  CHECK(page.removeLayer(l2.id()));
  CHECK(page.layer(9999) == nullptr);
}

TEST(change_set_apply_revert_roundtrip) {
  PageEditor ed;
  for (int i = 0; i < 5; ++i) ed.commitStroke(straight({0, 10.0f * i}, {100, 10.0f * i}));
  const Layer& layer = ed.page().layers()[0];
  std::vector<StrokePtr> before = layer.strokes();
  ChangeSet cs;
  LayerChange lc;
  lc.layer = layer.id();
  lc.removed = {{1, before[1]}, {3, before[3]}};
  auto x = std::make_shared<Stroke>(straight({0, 0}, {1, 1}));
  auto y = std::make_shared<Stroke>(straight({0, 0}, {2, 2}));
  x->id = 100;
  y->id = 101;
  lc.added = {{0, x}, {4, y}};
  cs.layers.push_back(lc);
  REQUIRE(applyChange(ed.page(), cs));
  CHECK(layer.size() == 5);
  CHECK(layer.at(0)->id == 100);
  CHECK(layer.at(4)->id == 101);
  REQUIRE(revertChange(ed.page(), cs));
  REQUIRE(layer.size() == before.size());
  for (size_t i = 0; i < before.size(); ++i) CHECK(layer.at(i) == before[i]);
  // Mismatched change is rejected without corrupting the layer.
  CHECK(!revertChange(ed.page(), cs));
  CHECK(layer.size() == before.size());
}

TEST(history_undo_redo) {
  PageEditor ed;
  ed.commitStroke(straight({0, 0}, {100, 0}));
  ed.commitStroke(straight({0, 20}, {100, 20}));
  CHECK(ed.page().strokeCount() == 2);
  CHECK(ed.undo());
  CHECK(ed.page().strokeCount() == 1);
  CHECK(ed.redo());
  CHECK(ed.page().strokeCount() == 2);
  ed.undo();
  ed.undo();
  CHECK(ed.page().strokeCount() == 0);
  CHECK(!ed.undo());
  ed.redo();
  ed.commitStroke(straight({0, 40}, {100, 40}));  // new action clears redo
  CHECK(!ed.canRedo());
  Rect dirty = ed.takeDirty();
  CHECK(!dirty.isEmpty());
  CHECK(ed.takeDirty().isEmpty());
}

TEST(stroke_eraser_removes_only_touched) {
  PageEditor ed;
  ed.commitStroke(straight({0, 0}, {100, 0}));
  ed.commitStroke(straight({0, 30}, {100, 30}));
  EraserOptions o;
  o.radius = 3.0f;
  ed.beginErase(o, {50, -10});
  ed.eraseTo({50, 10});
  ed.endErase();
  CHECK(ed.page().strokeCount() == 1);
  CHECK(ed.page().layers()[0].at(0)->points.front().pos.y == 30.0f);
  CHECK(ed.undo());
  CHECK(ed.page().strokeCount() == 2);
}

TEST(precise_eraser_splits_and_undoes) {
  PageEditor ed;
  ed.commitStroke(straight({0, 0}, {100, 0}));
  ed.commitStroke(straight({0, 50}, {100, 50}));
  StrokePtr original = ed.page().layers()[0].at(0);
  EraserOptions o;
  o.mode = EraserMode::Precise;
  o.radius = 5.0f;
  o.widthInfluence = 0.0f;
  const size_t undoBefore = ed.history().undoCount();
  ed.beginErase(o, {50, -20});
  for (float y = -20; y <= 20; y += 2) ed.eraseTo({50, y});
  ed.endErase();
  const Layer& layer = ed.page().layers()[0];
  REQUIRE(layer.size() == 3);
  const Stroke& left = *layer.at(0);
  const Stroke& right = *layer.at(1);
  CHECK_NEAR(left.points.front().pos.x, 0.0, 1e-4);
  // Pieces end just inside the erased band (the hole hides the rest of their cap).
  CHECK(left.points.back().pos.x >= 45.0f && left.points.back().pos.x <= 47.0f);
  CHECK(right.points.front().pos.x >= 53.0f && right.points.front().pos.x <= 55.0f);
  REQUIRE(left.holes.size() == 1);  // the straight drag is one hole
  CHECK(right.holes.size() == 1);
  {
    Bitmap bmp(400, 40, colors::kWhite);
    Rasterizer::renderPage(ed.page(), bmp, Affine::translate(0, 20) * Affine::scale(4.0f));
    auto inkAt = [&](float x) { return bmp.colorAt(static_cast<int>(x * 4.0f), 20).r < 0.5f; };
    CHECK(inkAt(10) && inkAt(44.5f) && inkAt(55.5f) && inkAt(90));
    CHECK(!inkAt(45.5f) && !inkAt(50) && !inkAt(54.5f));
  }
  CHECK_NEAR(right.points.back().pos.x, 100.0, 1e-4);
  CHECK(layer.at(2)->points.front().pos.y == 50.0f);  // z-order kept
  CHECK(left.style.kind == original->style.kind);
  CHECK(ed.history().undoCount() == undoBefore + 1);  // whole gesture = one undo step
  ed.undo();
  REQUIRE(layer.size() == 2);
  CHECK(layer.at(0) == original);
  ed.redo();
  CHECK(layer.size() == 3);
}

TEST(precise_eraser_removes_crumbs_and_dots) {
  PageEditor ed;
  ed.commitStroke(straight({0, 0}, {4, 0}));
  Stroke dot;
  dot.style = presets::ballpoint().style;
  StrokePoint p;
  p.pos = {50, 0};
  p.width = 1.5f;
  dot.points.push_back(p);
  ed.commitStroke(dot);
  EraserOptions o;
  o.mode = EraserMode::Precise;
  o.radius = 3.0f;
  ed.beginErase(o, {2, -5});
  ed.eraseTo({2, 5});
  ed.endErase();
  CHECK(ed.page().strokeCount() == 1);  // short stroke cut into crumbs -> gone; the dot stays
  ed.beginErase(o, {50, 0});
  ed.endErase();
  CHECK(ed.page().strokeCount() == 0);
}

namespace {

/// Page rendered at 4 px per unit on white.
Bitmap render4(const Page& page) {
  Bitmap bmp(static_cast<int>(page.width * 4), static_cast<int>(page.height * 4), colors::kWhite);
  Rasterizer::renderPage(page, bmp, Affine::scale(4.0f));
  return bmp;
}
/// True if the page has visible ink at (x, y) (page units).
bool inked(const Bitmap& b, float x, float y) {
  Color c = b.colorAt(static_cast<int>(x * 4.0f), static_cast<int>(y * 4.0f));
  return c.r + c.g + c.b < 2.97f;
}

}  // namespace

TEST(precise_eraser_on_highlighter_edge_removes_only_what_it_touches) {
  // A 5 mm highlighter (band y = 93..107). Brushing along its top edge with a
  // small eraser must leave a notch, not take out the whole height.
  PageEditor ed(300, 200);
  Brush hl = presets::highlighter();
  const float half = 0.5f * hl.dynamics.width;
  ed.commitStroke(straight({20, 100}, {280, 100}, hl, 60));
  const StrokePtr original = ed.page().layers()[0].at(0);
  EraserOptions o;
  o.mode = EraserMode::Precise;
  o.radius = 3.0f;
  const float y = 100.0f - half;  // eraser centre on the top edge
  ed.beginErase(o, {100, y});
  for (float x = 100; x <= 200; x += 1.5f) ed.eraseTo({x, y});
  ed.endErase();

  const Layer& layer = ed.page().layers()[0];
  REQUIRE(layer.size() == 1);  // nothing cut: the band below the eraser still shows
  const Stroke& s = *layer.at(0);
  CHECK(s.id == original->id);
  CHECK(s.points.size() == original->points.size());
  CHECK(s.holes.size() == 1);  // one straight drag = one hole
  Bitmap b = render4(ed.page());
  CHECK(!inked(b, 150, y + 1.0f));          // under the eraser: gone
  CHECK(!inked(b, 150, y + o.radius - 0.6f));
  CHECK(inked(b, 150, y + o.radius + 0.6f));  // just below the eraser: still there
  CHECK(inked(b, 150, 100));
  CHECK(inked(b, 150, 100 + half - 1.0f));
  CHECK(inked(b, 60, y + 1.0f));   // beside the stroke of the eraser: untouched
  CHECK(inked(b, 240, y + 1.0f));
  // Erasing the same place again changes nothing (no hole pile-up).
  ed.beginErase(o, {150, y});
  CHECK(!ed.eraseTo({151, y}));
  ed.endErase();
  CHECK(layer.at(0)->holes.size() == 1);
}

TEST(precise_eraser_across_highlighter_cuts_where_fully_erased) {
  PageEditor ed(300, 200);
  Brush hl = presets::highlighter();
  const float half = 0.5f * hl.dynamics.width;
  ed.commitStroke(straight({20, 100}, {280, 100}, hl, 60));
  EraserOptions o;
  o.mode = EraserMode::Precise;
  o.radius = 4.0f;
  // A small eraser scrubbing up and down across the band erases a gap.
  ed.beginErase(o, {140, 85});
  for (int i = 0; i <= 12; ++i) ed.eraseTo({140.0f + i * 1.5f, i % 2 ? 85.0f : 115.0f});
  ed.endErase();
  const Layer& layer = ed.page().layers()[0];
  REQUIRE(layer.size() == 2);  // fully hidden middle part is cut away
  CHECK(layer.at(0)->bounds.maxX < 160.0f);
  CHECK(layer.at(1)->bounds.minX > 140.0f);
  Bitmap b = render4(ed.page());
  for (float yy = 100 - half + 0.5f; yy < 100 + half; yy += 1.0f) {
    CHECK(!inked(b, 148, yy));
    CHECK(inked(b, 130, yy));
    CHECK(inked(b, 165, yy));
  }
  // Undo restores the original in one step.
  ed.undo();
  REQUIRE(layer.size() == 1);
  CHECK(layer.at(0)->holes.empty());
}

TEST(precise_eraser_render_matches_area_of_eraser) {
  // Thick marker: the ink that disappears is exactly the eraser's swept area.
  PageEditor ed(200, 100);
  Brush mk = presets::marker(colors::kBlack, 12.0f);
  ed.commitStroke(straight({20, 50}, {180, 50}, mk, 40));
  EraserOptions o;
  o.mode = EraserMode::Precise;
  o.radius = 4.0f;
  ed.beginErase(o, {100, 50});  // a tap in the middle of the stroke
  ed.endErase();
  REQUIRE(ed.page().strokeCount() == 1);
  Bitmap b = render4(ed.page());
  CHECK(!inked(b, 100, 50));
  CHECK(!inked(b, 103.4f, 50));
  CHECK(inked(b, 104.6f, 50));
  CHECK(inked(b, 100, 45.2f));  // above the 4 pt eraser disk, inside the 6 pt half-width
  CHECK(inked(b, 100, 54.8f));
}

TEST(stroke_eraser_ignores_erased_gaps) {
  PageEditor ed(300, 200);
  ed.commitStroke(straight({20, 100}, {280, 100}, presets::highlighter(), 60));
  EraserOptions precise;
  precise.mode = EraserMode::Precise;
  precise.radius = 4.0f;
  // Wipe the top half of the band over x = 100..200.
  ed.beginErase(precise, {100, 95});
  ed.eraseTo({200, 95});
  ed.endErase();
  REQUIRE(ed.page().strokeCount() == 1);
  EraserOptions whole;
  whole.radius = 1.0f;
  ed.beginErase(whole, {150, 94});  // inside the wiped area: nothing left to touch
  ed.endErase();
  CHECK(ed.page().strokeCount() == 1);
  ed.beginErase(whole, {150, 105});  // the remaining ink
  ed.endErase();
  CHECK(ed.page().strokeCount() == 0);
}

TEST(eraser_holes_follow_transform_and_survive_save) {
  PageEditor ed(300, 200);
  StrokeId id = ed.commitStroke(straight({20, 100}, {280, 100}, presets::highlighter(), 60));
  EraserOptions o;
  o.mode = EraserMode::Precise;
  o.radius = 3.0f;
  ed.beginErase(o, {100, 100});
  ed.endErase();
  REQUIRE(ed.page().layers()[0].at(0)->holes.size() == 1);
  ed.transform({id}, Affine::translate(10, 20));
  const Stroke& moved = *ed.page().layers()[0].at(0);
  REQUIRE(moved.holes.size() == 1);
  CHECK_NEAR(moved.holes[0].a.x, 110.0, 1e-4);
  CHECK_NEAR(moved.holes[0].a.y, 120.0, 1e-4);
  Bitmap b = render4(ed.page());
  CHECK(!inked(b, 110, 120));
  CHECK(inked(b, 120, 120));

  std::vector<uint8_t> data = ed.save();
  Page out;
  REQUIRE(deserializePage(data.data(), data.size(), out));
  const Stroke& loaded = *out.layers()[0].at(0);
  REQUIRE(loaded.holes.size() == 1);
  CHECK(distance(loaded.holes[0].a, moved.holes[0].a) < 0.01f);
  CHECK(distance(loaded.holes[0].b, moved.holes[0].b) < 0.01f);
  CHECK_NEAR(loaded.holes[0].radius, 3.0, 0.006);
  Stroke clip;
  std::vector<uint8_t> one = serializeStroke(moved);
  REQUIRE(deserializeStroke(one.data(), one.size(), clip));
  CHECK(clip.holes.size() == 1);
}

TEST(eraser_kind_mask) {
  PageEditor ed;
  ed.commitStroke(straight({0, 0}, {100, 0}, presets::ballpoint()));
  ed.commitStroke(straight({0, 0}, {100, 0}, presets::highlighter()));
  EraserOptions o;
  o.kindMask = kindBit(BrushKind::Highlighter);
  ed.beginErase(o, {50, -10});
  ed.eraseTo({50, 10});
  ed.endErase();
  REQUIRE(ed.page().strokeCount() == 1);
  CHECK(ed.page().layers()[0].at(0)->style.kind == BrushKind::Ballpoint);
}

TEST(lasso_and_transform) {
  PageEditor ed;
  StrokeId a = ed.commitStroke(straight({10, 10}, {40, 10}));
  ed.commitStroke(straight({100, 100}, {140, 100}));
  std::vector<Vec2> lasso = {{0, 0}, {60, 0}, {60, 30}, {0, 30}};
  std::vector<StrokeId> sel = ed.selectLasso(lasso);
  REQUIRE(sel.size() == 1);
  CHECK(sel[0] == a);
  CHECK(ed.transform(sel, Affine::translate(5, 7)));
  const Stroke& moved = *ed.page().layers()[0].at(0);
  CHECK(moved.id == a);
  CHECK_NEAR(moved.points.front().pos.x, 15.0, 1e-5);
  CHECK_NEAR(moved.bounds.minY, 17.0 - moved.points[0].width * 0.5, 1e-4);
  ed.transform(sel, Affine::scale(2));
  CHECK_NEAR(ed.page().layers()[0].at(0)->points[0].width, 2.0 * moved.points[0].width, 1e-5);
  ed.undo();
  ed.undo();
  CHECK_NEAR(ed.page().layers()[0].at(0)->points.front().pos.x, 10.0, 1e-5);
  CHECK(ed.recolor(sel, colors::kInkRed));
  CHECK(ed.page().layers()[0].at(0)->style.color == colors::kInkRed);
  auto dup = ed.duplicate(sel, {0, 50});
  CHECK(dup.size() == 1);
  CHECK(ed.page().strokeCount() == 3);
  CHECK(ed.remove(sel));
  CHECK(ed.page().strokeCount() == 2);
}

TEST(serialization_roundtrip) {
  PageEditor ed(600, 800);
  ed.commitStroke(drawn(presets::ballpoint(), synth::cursiveLoops({10, 50}, 3, 30), 1));
  ed.commitStroke(drawn(presets::pencil(), synth::wave({10, 150}, 200, 10, 60), 2));
  ed.commitStroke(drawn(presets::highlighter(), synth::polyline({{10, 200}, {300, 202}}), 3));
  ed.addLayer("second");
  ed.commitStroke(drawn(presets::calligraphy(), synth::spiral({300, 300}, 2, 3, 30), 4));
  ed.page().layers()[1].opacity = 0.5f;
  ed.page().layers()[1].locked = true;

  std::vector<uint8_t> data = ed.save();
  Page out;
  std::string err;
  REQUIRE(deserializePage(data.data(), data.size(), out, &err));
  const Page& in = ed.page();
  CHECK(out.width == in.width);
  CHECK(out.nextStrokeId() == in.nextStrokeId());
  REQUIRE(out.layers().size() == 2);
  CHECK_NEAR(out.layers()[1].opacity, 0.5, 1e-6);
  CHECK(out.layers()[1].locked);
  CHECK(out.layers()[1].name == "second");
  size_t points = 0;
  for (size_t l = 0; l < 2; ++l) {
    REQUIRE(out.layers()[l].size() == in.layers()[l].size());
    for (size_t s = 0; s < in.layers()[l].size(); ++s) {
      const Stroke& a = *in.layers()[l].at(s);
      const Stroke& b = *out.layers()[l].at(s);
      CHECK(a.id == b.id);
      CHECK(a.seed == b.seed);
      CHECK(a.style.kind == b.style.kind);
      CHECK(a.style.color == b.style.color);
      CHECK(a.style.nibAngle == b.style.nibAngle);
      REQUIRE(a.points.size() == b.points.size());
      for (size_t i = 0; i < a.points.size(); ++i) {
        CHECK(distance(a.points[i].pos, b.points[i].pos) < 0.01f);
        CHECK_NEAR(a.points[i].width, b.points[i].width, 0.006);
        CHECK_NEAR(a.points[i].alpha, b.points[i].alpha, 0.003);
        CHECK_NEAR(wrapAngle(a.points[i].azimuth - b.points[i].azimuth), 0.0, 0.03);
      }
      points += a.points.size();
    }
  }
  CHECK(data.size() < points * 14);  // compact: well under the 36 B/pt in memory
}

TEST(serialization_rejects_corrupt_input) {
  PageEditor ed;
  ed.commitStroke(drawn(presets::fountainPen(), synth::cursiveLoops({10, 50}, 2, 30), 5));
  std::vector<uint8_t> data = ed.save();
  Page out;
  // Every truncation fails cleanly.
  for (size_t n = 0; n < data.size(); n += 7) CHECK(!deserializePage(data.data(), n, out));
  // Random corruption never crashes (result may be either).
  for (uint32_t k = 0; k < 300; ++k) {
    std::vector<uint8_t> bad = data;
    for (int f = 0; f < 4; ++f) {
      size_t at = hash32(k, static_cast<uint32_t>(f)) % bad.size();
      bad[at] = static_cast<uint8_t>(hash32(at, k));
    }
    deserializePage(bad.data(), bad.size(), out);
  }
  CHECK(!deserializePage(nullptr, 0, out));
  const uint8_t junk[] = {'N', 'O', 'P', 'E', 1, 0, 0, 0, 0, 0};
  CHECK(!deserializePage(junk, sizeof(junk), out));
}

TEST(stroke_clipboard_roundtrip) {
  Stroke s = drawn(presets::brushPen(), synth::wave({0, 0}, 100, 10, 40), 9);
  s.id = 42;
  std::vector<uint8_t> d = serializeStroke(s);
  Stroke r;
  REQUIRE(deserializeStroke(d.data(), d.size(), r));
  CHECK(r.id == 42);
  CHECK(r.points.size() == s.points.size());
}

TEST(svg_export_outlines) {
  PageEditor ed;
  ed.commitStroke(straight({10, 10}, {100, 10}));
  ed.commitStroke(straight({10, 30}, {100, 30}, presets::highlighter()));
  StrokeOutline o = strokeOutline(*ed.page().layers()[0].at(0));
  REQUIRE(o.polygon.size() > 8);
  Rect r;
  for (Vec2 p : o.polygon) r.expand(p);
  float hw = presets::ballpoint().dynamics.width * 0.5f;
  CHECK_NEAR(r.minX, 10.0 - hw, 0.05);
  CHECK_NEAR(r.maxX, 100.0 + hw, 0.05);
  CHECK_NEAR(r.minY, 10.0 - hw, 0.05);
  std::string svg = exportSvg(ed.page());
  size_t paths = 0;
  for (size_t at = svg.find("<path"); at != std::string::npos; at = svg.find("<path", at + 1)) ++paths;
  CHECK(paths == 2);
  CHECK(svg.find("mix-blend-mode:multiply") != std::string::npos);
}

TEST(svg_export_masks_erased_ink) {
  PageEditor ed;
  ed.commitStroke(straight({10, 30}, {100, 30}, presets::highlighter()));
  EraserOptions o;
  o.mode = EraserMode::Precise;
  o.radius = 3.0f;
  ed.beginErase(o, {50, 30});
  ed.endErase();
  std::string svg = exportSvg(ed.page());
  size_t paths = 0;
  for (size_t at = svg.find("<path"); at != std::string::npos; at = svg.find("<path", at + 1)) ++paths;
  CHECK(paths == 1);  // the hole is a <polygon> in a <mask>, never an ink <path>
  CHECK(svg.find("<mask id=\"e") != std::string::npos);
  CHECK(svg.find("<polygon points=\"") != std::string::npos);
  CHECK(svg.find("mask=\"url(#e") != std::string::npos);
}

TEST(erase_gesture_history_is_the_net_change) {
  PageEditor ed;
  Brush hl = presets::highlighter();
  ed.commitStroke(drawn(hl, synth::polyline({{20, 100}, {560, 100}}), 3));
  ed.commitStroke(drawn(presets::ballpoint(), synth::polyline({{20, 130}, {560, 130}}), 4));
  StrokePtr hlOriginal = ed.page().layers()[0].at(0);
  EraserOptions o;
  o.mode = EraserMode::Precise;
  o.radius = 4.0f;
  // Scrub along the highlighter: it is cut again and again.
  ed.beginErase(o, {100, 90});
  for (int i = 0; i < 200; ++i) ed.eraseTo({100.0f + i * 2.0f, i % 2 ? 90.0f : 110.0f});
  ed.endErase();
  const size_t after = ed.page().layers()[0].size();
  REQUIRE(after > 2);
  // Undo restores exactly the original strokes; redo the final pieces.
  std::vector<StrokePtr> finalStrokes = ed.page().layers()[0].strokes();
  ed.undo();
  REQUIRE(ed.page().layers()[0].size() == 2);
  CHECK(ed.page().layers()[0].at(0) == hlOriginal);
  ed.redo();
  REQUIRE(ed.page().layers()[0].size() == after);
  for (size_t i = 0; i < after; ++i) CHECK(ed.page().layers()[0].at(i) == finalStrokes[i]);
}

TEST(precise_erase_damage_is_local_for_vector_ink) {
  PageEditor ed;
  ed.commitStroke(drawn(presets::highlighter(), synth::polyline({{20, 100}, {560, 100}}), 5));
  ed.commitStroke(drawn(presets::pencil(), synth::polyline({{20, 200}, {560, 200}}), 6));
  ed.takeDirty();
  EraserOptions o;
  o.mode = EraserMode::Precise;
  o.radius = 5.0f;
  ed.beginErase(o, {300, 80});
  ed.eraseTo({300, 120});
  Rect d = ed.takeDirty();
  REQUIRE(!d.isEmpty());
  CHECK(d.width() < 80.0f);  // the cut, not the 540 pt long highlighter
  ed.eraseTo({300, 220});    // pencil: stamp walk restarts in each piece -> whole stroke
  Rect d2 = ed.takeDirty();
  CHECK(d2.width() > 500.0f);
  ed.endErase();
}
