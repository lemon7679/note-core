#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "ink/ink_c.h"
#include "test.h"

namespace {
InkInputPoint pt(float x, float y, double t, uint64_t id) {
  InkInputPoint p;
  std::memset(&p, 0, sizeof(p));
  p.x = x;
  p.y = y;
  p.pressure = 0.4f;
  p.altitude = 1.2f;
  p.azimuth = 0.0f;
  p.time = t;
  p.update_id = id;
  p.source = INK_SOURCE_PENCIL;
  return p;
}
}  // namespace

TEST(c_api_full_flow) {
  InkEditor* e = ink_editor_create(200, 100);
  InkBuilder* b = ink_builder_create(INK_BRUSH_FOUNTAIN_PEN);
  REQUIRE(e && b);
  ink_builder_set_color(b, InkColor{0.1f, 0.2f, 0.6f, 1.0f});

  InkInputPoint first = pt(10, 50, 0.0, 1);
  ink_builder_begin(b, &first);
  std::vector<InkInputPoint> pts;
  for (int i = 1; i <= 100; ++i) pts.push_back(pt(10.0f + i * 1.5f, 50.0f + 10.0f * std::sin(i * 0.1f), i / 240.0, i + 1));
  ink_builder_add_points(b, pts.data(), pts.size());
  InkInputPoint pred = pt(165, 50, 101 / 240.0, 0);
  ink_builder_set_predicted(b, &pred, 1);
  CHECK(ink_builder_is_active(b));

  InkLiveGeometry live;
  REQUIRE(ink_builder_update_geometry(b, 2.0f, &live));
  CHECK(live.stable.capsule_count + live.tail.capsule_count > 10);
  CHECK(live.stable_reset);
  CHECK(live.dirty.width > 0.0f);
  InkStyleInfo style;
  ink_builder_get_style(b, &style);
  CHECK(style.kind == INK_BRUSH_FOUNTAIN_PEN);
  CHECK_NEAR(style.color.b, 0.6, 1e-6);

  ink_builder_end(b);
  InkStrokeId id = ink_editor_commit(e, b);
  CHECK(id != 0);
  CHECK(ink_editor_stroke_count(e) == 1);
  InkRect dirty = ink_editor_take_dirty(e);
  CHECK(dirty.width > 100.0f);

  // GPU path: query + cached geometry.
  InkStrokeId ids[4];
  size_t n = ink_editor_query(e, InkRect{0, 0, 200, 100}, ids, 4);
  REQUIRE(n == 1);
  CHECK(ids[0] == id);
  InkGeometryView g1, g2;
  REQUIRE(ink_editor_stroke_geometry(e, id, 2.0f, &g1, &style));
  REQUIRE(ink_editor_stroke_geometry(e, id, 1.9f, &g2, nullptr));
  CHECK(g1.capsule_count > 10);
  CHECK(g1.capsules == g2.capsules);  // same zoom bucket -> cached
  CHECK(g1.capsules[0].r0 > 0.0f);

  // CPU path.
  std::vector<uint8_t> px(200 * 100 * 4);
  ink_editor_render_rgba(e, px.data(), 200, 100, 200 * 4, 1.0f, 0, 0, InkColor{1, 1, 1, 1}, nullptr);
  size_t inked = 0;
  for (size_t i = 0; i < px.size(); i += 4)
    if (px[i + 2] > px[i] + 40) ++inked;
  CHECK(inked > 150);

  // Persistence.
  size_t size = 0;
  uint8_t* data = ink_editor_save(e, &size);
  REQUIRE(data && size > 0);
  InkEditor* e2 = ink_editor_create(10, 10);
  CHECK(ink_editor_load(e2, data, size));
  CHECK(ink_editor_stroke_count(e2) == 1);
  ink_free(data);
  char* svg = ink_editor_export_svg(e2);
  REQUIRE(svg);
  CHECK(std::strstr(svg, "<path") != nullptr);
  ink_free(svg);
  ink_editor_destroy(e2);

  // Erase + undo/redo.
  ink_editor_erase_begin(e, INK_ERASER_PRECISE, 4.0f, 0, 80, 20);
  ink_editor_erase_move(e, 80, 80);
  ink_editor_erase_end(e);
  CHECK(ink_editor_stroke_count(e) == 2);
  CHECK(ink_editor_undo(e));
  CHECK(ink_editor_stroke_count(e) == 1);
  CHECK(ink_editor_redo(e));
  CHECK(ink_editor_stroke_count(e) == 2);
  // Geometry cache notices the replaced strokes.
  CHECK(!ink_editor_stroke_geometry(e, id, 2.0f, &g1, nullptr));

  // Lasso + transform.
  float lasso[] = {0, 0, 200, 0, 200, 100, 0, 100};
  InkStrokeId sel[8];
  size_t ns = ink_editor_select_lasso(e, lasso, 4, sel, 8);
  CHECK(ns == 2);
  CHECK(ink_editor_transform(e, sel, ns, 1, 0, 0, 1, 10, 0));
  CHECK(ink_editor_delete(e, sel, ns));
  CHECK(ink_editor_stroke_count(e) == 0);

  int gs = 0;
  CHECK(ink_grain_texture(&gs) != nullptr);
  CHECK(gs == 256);
  CHECK(std::strlen(ink_version()) > 0);

  ink_builder_destroy(b);
  ink_editor_destroy(e);
}

TEST(c_api_null_safety) {
  ink_builder_begin(nullptr, nullptr);
  ink_builder_end(nullptr);
  CHECK(!ink_builder_is_active(nullptr));
  CHECK(ink_editor_commit(nullptr, nullptr) == 0);
  CHECK(ink_editor_stroke_count(nullptr) == 0);
  CHECK(!ink_editor_undo(nullptr));
  ink_editor_destroy(nullptr);
  ink_builder_destroy(nullptr);
  ink_free(nullptr);
}

TEST(c_api_correction_and_shapes) {
  InkEditor* e = ink_editor_create(400, 200);
  InkBuilder* b = ink_builder_create(INK_BRUSH_BALLPOINT);
  REQUIRE(e && b);
  // A tilted bar with correction on: becomes exactly horizontal.
  ink_builder_set_correction(b, 0.6f);
  ink_builder_set_correction_options(b, true, true, true, true);
  InkInputPoint first = pt(10, 50, 0.0, 1);
  ink_builder_begin(b, &first);
  std::vector<InkInputPoint> pts;
  for (int i = 1; i <= 60; ++i) pts.push_back(pt(10.0f + i, 50.0f + i * 0.05f, i / 240.0, i + 1));
  ink_builder_add_points(b, pts.data(), pts.size());
  ink_builder_end(b);
  REQUIRE(ink_editor_commit(e, b) != 0);
  InkStrokeId id;
  REQUIRE(ink_editor_query(e, InkRect{0, 0, 400, 200}, &id, 1) == 1);
  InkGeometryView g;
  REQUIRE(ink_editor_stroke_geometry(e, id, 2.0f, &g, nullptr));
  REQUIRE(g.capsule_count > 2);
  for (size_t i = 0; i < g.capsule_count; ++i) CHECK_NEAR(g.capsules[i].y0, g.capsules[0].y0, 1e-3);

  // Draw-and-hold an ellipse with the constraint on -> perfect circle.
  ink_builder_set_correction(b, 0.0f);
  ink_builder_set_snap_on_hold(b, true);
  InkInputPoint s0 = pt(300, 60, 1.0, 0);
  ink_builder_begin(b, &s0);
  double t = 1.0;
  for (int i = 1; i <= 200; ++i) {
    float a = -1.5708f + 6.4f * i / 200.0f;
    t += 1.0 / 240.0;
    InkInputPoint q = pt(300.0f + 70.0f * std::cos(a), 100.0f + 40.0f * std::sin(a), t, 0);
    ink_builder_add_points(b, &q, 1);
  }
  bool snapped = false;
  InkInputPoint rest = pts.back();
  rest.x = 300.0f + 70.0f * std::cos(-1.5708f + 6.4f);
  rest.y = 100.0f + 40.0f * std::sin(-1.5708f + 6.4f);
  for (int k = 0; k < 200 && !snapped; ++k) {
    t += 1.0 / 240.0;
    rest.time = t;
    ink_builder_add_points(b, &rest, 1);
    snapped = ink_builder_tick(b, t) || ink_builder_snapped_shape(b) != INK_SHAPE_NONE;
  }
  CHECK(ink_builder_snapped_shape(b) == INK_SHAPE_ELLIPSE);
  ink_builder_set_shape_constraint(b, true);
  CHECK(ink_builder_shape_is_regular(b));
  ink_builder_end(b);
  ink_builder_set_shape_constraint(b, false);
  CHECK(ink_editor_commit(e, b) != 0);

  // Editor-side correction and straightening are undoable no-ops on clean input.
  InkStrokeId ids[4];
  size_t n = ink_editor_query(e, InkRect{0, 0, 400, 200}, ids, 4);
  CHECK(n == 2);
  ink_editor_correct(e, ids, n, 0.5f);
  ink_editor_straighten(e, ids, n, 1.0f);
  CHECK(ink_editor_stroke_count(e) == 2);
  ink_builder_destroy(b);
  ink_editor_destroy(e);
}

namespace {
InkStrokeId drawLine(InkEditor* e, InkBuilder* b, float y0, float x0 = 20.0f) {
  InkInputPoint first = pt(x0, y0, 0.0, 1);
  ink_builder_begin(b, &first);
  std::vector<InkInputPoint> pts;
  for (int i = 1; i <= 60; ++i) pts.push_back(pt(x0 + i * 2.0f, y0 + 4.0f * std::sin(i * 0.2f), i / 240.0, i + 1));
  ink_builder_add_points(b, pts.data(), pts.size());
  ink_builder_end(b);
  return ink_editor_commit(e, b);
}
}  // namespace

TEST(c_api_render_over_matches_render_rgba) {
  InkEditor* e = ink_editor_create(200, 100);
  InkBuilder* b = ink_builder_create(INK_BRUSH_FOUNTAIN_PEN);
  REQUIRE(e && b);
  drawLine(e, b, 40);
  ink_builder_set_brush(b, INK_BRUSH_HIGHLIGHTER, InkColor{1.0f, 0.85f, 0.2f, 1.0f}, 10.0f);
  drawLine(e, b, 45);
  REQUIRE(ink_editor_stroke_count(e) == 2);

  const int w = 200, h = 100;
  std::vector<uint8_t> ref(w * h * 4), over(w * h * 4, 255);  // opaque white background
  ink_editor_render_rgba(e, ref.data(), w, h, w * 4, 1.0f, 0, 0, InkColor{1, 1, 1, 1}, nullptr);
  ink_editor_render_over_rgba(e, over.data(), w, h, w * 4, 1.0f, 0, 0);
  int maxDiff = 0;
  size_t inked = 0;
  for (size_t i = 0; i < ref.size(); ++i) {
    int d = std::abs(static_cast<int>(ref[i]) - static_cast<int>(over[i]));
    if (d > maxDiff) maxDiff = d;
    if ((i & 3) == 0 && over[i] < 200) ++inked;
  }
  // Opaque background: straight == premultiplied; only the cached (denser) tessellation may differ.
  CHECK(maxDiff <= 8);
  CHECK(inked > 100);

  // Tiles: rendering two halves with offsets reproduces the whole page.
  std::vector<uint8_t> left(100 * h * 4, 255), right(100 * h * 4, 255);
  ink_editor_render_over_rgba(e, left.data(), 100, h, 100 * 4, 1.0f, 0, 0);
  ink_editor_render_over_rgba(e, right.data(), 100, h, 100 * 4, 1.0f, -100, 0);
  int tileDiff = 0;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      for (int c = 0; c < 4; ++c) {
        uint8_t v = x < 100 ? left[(y * 100 + x) * 4 + c] : right[(y * 100 + x - 100) * 4 + c];
        tileDiff = std::max(tileDiff, std::abs(static_cast<int>(v) - static_cast<int>(over[(y * w + x) * 4 + c])));
      }
  CHECK(tileDiff <= 1);
  ink_builder_destroy(b);
  ink_editor_destroy(e);
}

TEST(c_api_builder_overlay_is_premultiplied) {
  InkBuilder* b = ink_builder_create(INK_BRUSH_BALLPOINT);
  REQUIRE(b);
  ink_builder_set_color(b, InkColor{0.0f, 0.0f, 1.0f, 1.0f});
  const int w = 120, h = 60;
  std::vector<uint8_t> px(w * h * 4, 0);
  ink_builder_render_over_rgba(b, px.data(), w, h, w * 4, 1.0f, 0, 0);  // no geometry yet: no-op
  for (uint8_t v : px) CHECK(v == 0);

  InkInputPoint first = pt(10, 30, 0.0, 1);
  ink_builder_begin(b, &first);
  std::vector<InkInputPoint> pts;
  for (int i = 1; i <= 40; ++i) pts.push_back(pt(10.0f + i * 2.5f, 30.0f, i / 240.0, i + 1));
  ink_builder_add_points(b, pts.data(), pts.size());
  InkLiveGeometry live;
  REQUIRE(ink_builder_update_geometry(b, 1.0f, &live));
  ink_builder_render_over_rgba(b, px.data(), w, h, w * 4, 1.0f, 0, 0);
  size_t covered = 0;
  bool premultiplied = true;
  for (size_t i = 0; i < px.size(); i += 4) {
    if (px[i + 3] > 100) ++covered;
    if (px[i + 2] > px[i + 3] || px[i] > px[i + 3]) premultiplied = false;
  }
  CHECK(covered > 40);
  CHECK(premultiplied);
  // Drawing the overlay does not advance the incremental geometry.
  InkLiveGeometry again;
  REQUIRE(ink_builder_update_geometry(b, 1.0f, &again));
  CHECK(!again.stable_reset);
  ink_builder_destroy(b);
}

TEST(c_api_clipboard_duplicate_bounds) {
  InkEditor* a = ink_editor_create(300, 200);
  InkEditor* c = ink_editor_create(300, 200);
  InkBuilder* b = ink_builder_create(INK_BRUSH_BALLPOINT);
  REQUIRE(a && b && c);
  InkStrokeId ids[2] = {drawLine(a, b, 40), drawLine(a, b, 90)};
  REQUIRE(ids[0] && ids[1]);
  InkRect r = ink_editor_bounds_of(a, ids, 2);
  CHECK(r.width > 100.0f);
  CHECK(r.y < 40.0f && r.y + r.height > 90.0f);

  InkStrokeId dup[4];
  CHECK(ink_editor_duplicate(a, ids, 2, 5, 5, dup, 4) == 2);
  CHECK(ink_editor_stroke_count(a) == 4);
  CHECK(ink_editor_undo(a));
  CHECK(ink_editor_stroke_count(a) == 2);

  size_t size = 0;
  uint8_t* clip = ink_editor_copy_strokes(a, ids, 2, &size);
  REQUIRE(clip && size > 0);
  InkStrokeId pasted[4];
  CHECK(ink_editor_paste_strokes(c, clip, size, 0, 50, pasted, 4) == 2);
  ink_free(clip);
  CHECK(ink_editor_stroke_count(c) == 2);
  InkRect pr = ink_editor_bounds_of(c, pasted, 2);
  CHECK_NEAR(pr.y, r.y + 50.0f, 0.01);
  CHECK(ink_editor_undo(c));  // one undo step removes the whole paste
  CHECK(ink_editor_stroke_count(c) == 0);
  CHECK(ink_editor_paste_strokes(c, reinterpret_cast<const uint8_t*>("junk"), 4, 0, 0, nullptr, 0) == 0);
  CHECK_NEAR(ink_editor_page_width(c), 300.0, 1e-6);
  CHECK_NEAR(ink_editor_page_height(c), 200.0, 1e-6);
  ink_builder_destroy(b);
  ink_editor_destroy(a);
  ink_editor_destroy(c);
}

TEST(c_api_hidden_and_selected_rendering) {
  InkEditor* e = ink_editor_create(200, 100);
  InkBuilder* b = ink_builder_create(INK_BRUSH_MARKER);
  REQUIRE(e && b);
  InkStrokeId top = drawLine(e, b, 25);
  InkStrokeId bottom = drawLine(e, b, 75);
  REQUIRE(top && bottom);
  const int w = 200, h = 100;
  auto inkedRows = [&](const std::vector<uint8_t>& px, int y0, int y1) {
    size_t n = 0;
    for (int y = y0; y < y1; ++y)
      for (int x = 0; x < w; ++x)
        if (px[(y * w + x) * 4 + 3] > 128) ++n;
    return n;
  };
  std::vector<uint8_t> px(w * h * 4, 0);
  ink_editor_set_hidden(e, &top, 1);
  ink_editor_render_over_rgba(e, px.data(), w, h, w * 4, 1.0f, 0, 0);
  CHECK(inkedRows(px, 0, 50) == 0);
  CHECK(inkedRows(px, 50, 100) > 50);

  std::fill(px.begin(), px.end(), 0);
  ink_editor_render_strokes_over_rgba(e, &top, 1, px.data(), w, h, w * 4, 1.0f, 0, 0);
  CHECK(inkedRows(px, 0, 50) > 50);
  CHECK(inkedRows(px, 50, 100) == 0);

  ink_editor_set_hidden(e, nullptr, 0);
  std::fill(px.begin(), px.end(), 0);
  ink_editor_render_over_rgba(e, px.data(), w, h, w * 4, 1.0f, 0, 0);
  CHECK(inkedRows(px, 0, 50) > 50);
  ink_builder_destroy(b);
  ink_editor_destroy(e);
}
