#include "ink/editor.h"
#include "ink/rasterizer.h"
#include "test.h"

using namespace ink;

namespace {

Stroke polyStroke(std::initializer_list<Vec2> pts, const Brush& brush, float width, float pressure = 0.5f) {
  Stroke s;
  s.style = brush.style;
  for (Vec2 p : pts) {
    StrokePoint sp;
    sp.pos = p;
    sp.width = width;
    sp.pressure = pressure;
    sp.flags = kPointSharp;
    s.points.push_back(sp);
  }
  s.updateBounds();
  return s;
}

Bitmap render(const Page& page, int w, int h, float scale = 1.0f) {
  Bitmap bmp(w, h);
  Rasterizer::renderPage(page, bmp, Affine::scale(scale));
  return bmp;
}

float luminance(const Bitmap& b, int x, int y) {
  Color c = b.colorAt(x, y);
  return (c.r + c.g + c.b) / 3.0f;
}

}  // namespace

TEST(render_solid_line_coverage) {
  PageEditor ed(100, 40);
  ed.commitStroke(polyStroke({{10, 20}, {90, 20}}, presets::ballpoint(Color(0, 0, 0)), 4.0f));
  Bitmap b = render(ed.page(), 100, 40);
  CHECK(luminance(b, 50, 19) < 0.01f);   // inside
  CHECK(luminance(b, 50, 30) > 0.99f);   // outside
  CHECK(luminance(b, 50, 25) > 0.99f);
  // Edge pixel row 22 spans y in [22,23): the line edge is at y = 22 -> half covered at most.
  float edge = luminance(b, 50, 21);
  CHECK(edge < 0.05f);
  // Round caps extend past the end points.
  CHECK(luminance(b, 8, 20) < 0.5f);
}

TEST(render_highlighter_no_self_overlap_darkening) {
  PageEditor ed(200, 100);
  Brush hl = presets::highlighter();
  hl.style.tip = TipShape::Round;
  // A stroke that doubles back over itself.
  ed.commitStroke(polyStroke({{20, 50}, {180, 50}, {100, 50}}, hl, 20.0f));
  Bitmap b = render(ed.page(), 200, 100);
  Color once = b.colorAt(150, 50);    // covered once... and also by the return? (no: return ends at 100)
  Color twice = b.colorAt(120, 50);   // covered by both passes
  CHECK_NEAR(once.b, twice.b, 1e-4);
  CHECK(once.b < 0.9f);  // but it is visibly highlighted
  // A second, separate stroke does stack.
  ed.commitStroke(polyStroke({{110, 20}, {110, 80}}, hl, 20.0f));
  Bitmap b2 = render(ed.page(), 200, 100);
  CHECK(b2.colorAt(110, 50).b < twice.b - 0.05f);
}

TEST(render_multiply_keeps_text_black) {
  PageEditor ed(100, 40);
  ed.commitStroke(polyStroke({{10, 20}, {90, 20}}, presets::ballpoint(Color(0, 0, 0)), 4.0f));
  ed.commitStroke(polyStroke({{10, 20}, {90, 20}}, presets::highlighter(), 16.0f));
  Bitmap b = render(ed.page(), 100, 40);
  CHECK(luminance(b, 50, 20) < 0.01f);  // ink under the highlighter stays black
  Color hl = b.colorAt(50, 14);
  CHECK(hl.r > 0.95f && hl.b < 0.8f);  // yellow tint around it
}

TEST(render_hairline_fades_instead_of_breaking) {
  PageEditor ed(100, 20);
  ed.commitStroke(polyStroke({{5, 10.3f}, {95, 10.3f}}, presets::ballpoint(Color(0, 0, 0)), 0.3f));
  Bitmap b = render(ed.page(), 100, 20);
  // Every column along the line has some ink (no gaps), and it is lighter than a solid line.
  for (int x = 10; x < 90; x += 3) {
    float darkest = 1.0f;
    for (int y = 8; y < 13; ++y) darkest = std::min(darkest, luminance(b, x, y));
    CHECK(darkest < 0.85f);
    CHECK(darkest > 0.3f);
  }
}

TEST(render_scale_invariance) {
  PageEditor ed(50, 50);
  ed.commitStroke(polyStroke({{10, 10}, {40, 40}}, presets::ballpoint(Color(0, 0, 0)), 3.0f));
  Bitmap a = render(ed.page(), 50, 50, 1.0f);
  Bitmap b = render(ed.page(), 200, 200, 4.0f);
  // Average darkness of corresponding areas matches (box-downsampled).
  double sa = 0.0, sb = 0.0;
  for (int y = 0; y < 50; ++y)
    for (int x = 0; x < 50; ++x) sa += luminance(a, x, y);
  for (int y = 0; y < 200; ++y)
    for (int x = 0; x < 200; ++x) sb += luminance(b, x, y);
  CHECK_NEAR(sa / 2500.0, sb / 40000.0, 0.01);
}

TEST(grain_texture_is_uniform_and_deterministic) {
  GrainTexture a(1234), b(1234);
  double sum = 0.0;
  int bins[4] = {0, 0, 0, 0};
  for (int i = 0; i < a.size() * a.size(); ++i) {
    CHECK(a.data()[i] == b.data()[i]);
    sum += a.data()[i] / 255.0;
    ++bins[a.data()[i] / 64];
  }
  CHECK_NEAR(sum / (a.size() * a.size()), 0.5, 0.01);
  for (int k = 0; k < 4; ++k) CHECK_NEAR(bins[k] / static_cast<double>(a.size() * a.size()), 0.25, 0.01);
  // Wraps seamlessly.
  CHECK_NEAR(a.sample(0.5f, 0.5f), a.sample(256.5f, -255.5f), 1e-6);
}

TEST(render_pencil_pressure_darkens) {
  auto avgDarkness = [](float pressure, float alpha) {
    PageEditor ed(100, 30);
    Brush pencil = presets::pencil();
    Stroke s = polyStroke({{10, 15}, {90, 15}}, pencil, 3.0f, pressure);
    for (StrokePoint& p : s.points) p.alpha = alpha;
    ed.commitStroke(s);
    Bitmap b(100, 30);
    Rasterizer::renderPage(ed.page(), b, Affine::identity());
    double d = 0.0;
    for (int y = 10; y < 20; ++y)
      for (int x = 20; x < 80; ++x) d += 1.0 - luminance(b, x, y);
    return d;
  };
  // Deeper lead fills more of the paper's valleys (alpha fixed below saturation).
  CHECK(avgDarkness(0.9f, 0.6f) > avgDarkness(0.2f, 0.6f) * 1.3);
  CHECK(avgDarkness(0.5f, 0.9f) > avgDarkness(0.5f, 0.3f) * 2.0);
}

TEST(render_pencil_is_soft_and_builds_up) {
  Brush pencil = presets::pencil();
  auto render1 = [&](int passes) {
    PageEditor ed(100, 30);
    for (int k = 0; k < passes; ++k) {
      Stroke s = polyStroke({{10, 15.5f}, {90, 15.5f}}, pencil, 8.0f, 0.4f);
      for (StrokePoint& p : s.points) p.alpha = 0.5f;
      ed.commitStroke(s);
    }
    Bitmap b(100, 30);
    Rasterizer::renderPage(ed.page(), b, Affine::scale(1.0f));
    return b;
  };
  Bitmap once = render1(1), twice = render1(2);
  // Averaged over the grain, the centre reaches about the stroke's alpha...
  double centre = 0.0;
  for (int x = 20; x < 80; ++x) centre += 1.0 - luminance(once, x, 15);
  centre /= 60.0;
  CHECK(centre > 0.25 && centre < 0.55);
  // ...falls off smoothly towards the edge (no hard cut)...
  double mid = 0.0, rim = 0.0;
  for (int x = 20; x < 80; ++x) {
    mid += 1.0 - luminance(once, x, 17);  // 2 px from the centre line
    rim += 1.0 - luminance(once, x, 18);  // 3 px (radius 4)
  }
  CHECK(mid / 60.0 < centre && rim / 60.0 < mid / 60.0 && rim / 60.0 > 0.0);
  // ...and a second pass over the same place deposits more graphite.
  double again = 0.0;
  for (int x = 20; x < 80; ++x) again += 1.0 - luminance(twice, x, 15);
  CHECK(again / 60.0 > centre * 1.3);
}

TEST(render_layer_visibility_and_opacity) {
  PageEditor ed(40, 40);
  ed.commitStroke(polyStroke({{5, 20}, {35, 20}}, presets::ballpoint(Color(0, 0, 0)), 6.0f));
  Bitmap full = render(ed.page(), 40, 40);
  ed.page().layers()[0].opacity = 0.5f;
  Bitmap half = render(ed.page(), 40, 40);
  CHECK_NEAR(luminance(half, 20, 20), 0.5, 0.02);
  ed.page().layers()[0].visible = false;
  Bitmap none = render(ed.page(), 40, 40);
  CHECK(luminance(none, 20, 20) > 0.999f);
  CHECK(luminance(full, 20, 20) < 0.01f);
}
