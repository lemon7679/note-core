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
