#include <memory>

#include "ink/editor.h"
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
  CHECK_NEAR(left.points.back().pos.x, 45.0, 0.05);   // cut exactly at the eraser edge
  CHECK_NEAR(right.points.front().pos.x, 55.0, 0.05);
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
