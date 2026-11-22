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
