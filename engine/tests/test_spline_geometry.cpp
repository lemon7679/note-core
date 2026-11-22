#include "ink/geometry.h"
#include "ink/sdf.h"
#include "ink/spline.h"
#include "test.h"

using namespace ink;

namespace {
std::vector<StrokePoint> pts(std::initializer_list<Vec2> ps, float width = 2.0f) {
  std::vector<StrokePoint> out;
  for (Vec2 p : ps) {
    StrokePoint sp;
    sp.pos = p;
    sp.width = width;
    out.push_back(sp);
  }
  return out;
}
}  // namespace

TEST(catmull_rom_interpolates_control_points) {
  auto p = pts({{0, 0}, {10, 5}, {20, -3}, {30, 8}, {35, 20}});
  std::vector<CurveSample> s;
  CatmullRom::sampleAll(p.data(), p.size(), {}, s);
  size_t found = 0;
  for (const StrokePoint& cp : p)
    for (const CurveSample& c : s)
      if (distance(c.pos, cp.pos) < 1e-5f) {
        ++found;
        break;
      }
  CHECK(found == p.size());
  CHECK(distance(s.front().pos, p.front().pos) < 1e-6f);
  CHECK(distance(s.back().pos, p.back().pos) < 1e-6f);
}

TEST(catmull_rom_collinear_stays_straight) {
  auto p = pts({{0, 0}, {3, 0}, {4, 0}, {12, 0}, {30, 0}});
  std::vector<CurveSample> s;
  CatmullRom::sampleAll(p.data(), p.size(), {}, s);
  for (const CurveSample& c : s) CHECK_NEAR(c.pos.y, 0.0, 1e-5);
  for (size_t i = 1; i < s.size(); ++i) CHECK(s[i].pos.x >= s[i - 1].pos.x - 1e-5f);  // no overshoot
}

TEST(catmull_rom_sharp_corners_are_kept) {
  auto p = pts({{0, 0}, {20, 0}, {20, 20}});
  p[1].flags = kPointSharp;
  std::vector<CurveSample> s;
  CatmullRom::sampleAll(p.data(), p.size(), {}, s);
  for (const CurveSample& c : s) {
    bool onFirst = std::fabs(c.pos.y) < 1e-4f && c.pos.x <= 20.0001f;
    bool onSecond = std::fabs(c.pos.x - 20.0f) < 1e-4f;
    CHECK(onFirst || onSecond);
  }
}

TEST(catmull_rom_sampling_respects_chord) {
  auto p = pts({{0, 0}, {50, 40}, {100, -20}});
  CurveSamplingOptions o;
  o.maxChord = 1.0f;
  std::vector<CurveSample> s;
  CatmullRom::sampleAll(p.data(), p.size(), o, s);
  for (size_t i = 1; i < s.size(); ++i) CHECK(distance(s[i].pos, s[i - 1].pos) < 1.6f);
}

TEST(sdf_uneven_capsule) {
  CHECK_NEAR(sdf::unevenCapsule({0, 0}, {0, 0}, {10, 0}, 2, 4), -2.0, 1e-5);
  CHECK_NEAR(sdf::unevenCapsule({10, 0}, {0, 0}, {10, 0}, 2, 4), -4.0, 1e-5);
  CHECK_NEAR(sdf::unevenCapsule({-5, 0}, {0, 0}, {10, 0}, 2, 4), 3.0, 1e-5);
  CHECK_NEAR(sdf::unevenCapsule({16, 0}, {0, 0}, {10, 0}, 2, 4), 2.0, 1e-5);
  // Equal radii: plain capsule.
  CHECK_NEAR(sdf::unevenCapsule({5, 7}, {0, 0}, {10, 0}, 3, 3), 4.0, 1e-5);
  // One circle containing the other.
  CHECK_NEAR(sdf::unevenCapsule({0, 10}, {0, 0}, {1, 0}, 5, 1), 5.0, 1e-4);
  // Degenerate (point).
  CHECK_NEAR(sdf::unevenCapsule({3, 4}, {0, 0}, {0, 0}, 1, 1), 4.0, 1e-5);
}

TEST(sdf_quad_and_nib) {
  Vec2 sq[4] = {{0, 0}, {10, 0}, {10, 10}, {0, 10}};
  CHECK_NEAR(sdf::quad({5, 5}, sq), -5.0, 1e-5);
  CHECK_NEAR(sdf::quad({15, 5}, sq), 5.0, 1e-5);
  Vec2 rev[4] = {{0, 0}, {0, 10}, {10, 10}, {10, 0}};
  CHECK_NEAR(sdf::quad({5, 5}, rev), -5.0, 1e-5);
  // Vertical nib (half-length 4, round 1) swept horizontally.
  float d = sdf::nibSweep({5, 0}, {0, 0}, 4, {10, 0}, 4, {0, 1}, 1);
  CHECK_NEAR(d, -5.0, 1e-4);
  CHECK_NEAR(sdf::nibSweep({5, 6}, {0, 0}, 4, {10, 0}, 4, {0, 1}, 1), 1.0, 1e-4);
  // Swept along its own axis: degenerate quad -> thin line of radius `round`.
  CHECK_NEAR(sdf::nibSweep({2, 0}, {0, 0}, 4, {0, 10}, 4, {0, 1}, 1), 1.0, 1e-4);
}

namespace {
bool sameCaps(const CapsulePrim& a, const CapsulePrim& b) {
  return a.p0 == b.p0 && a.p1 == b.p1 && a.r0 == b.r0 && a.r1 == b.r1 && a.a0 == b.a0 && a.a1 == b.a1;
}
}  // namespace

TEST(tessellation_incremental_equals_whole) {
  std::vector<StrokePoint> p;
  for (int i = 0; i < 40; ++i) {
    StrokePoint sp;
    sp.pos = {i * 3.0f, std::sin(i * 0.4f) * 10.0f};
    sp.width = 1.0f + 0.5f * std::sin(i * 0.2f);
    sp.pressure = 0.5f;
    p.push_back(sp);
  }
  for (RenderMethod m : {RenderMethod::Vector, RenderMethod::Stamp}) {
    StrokeStyle style;
    style.method = m;
    style.positionJitter = 0.1f;
    TessellationOptions o;
    StrokeGeometry whole, parts;
    Tessellator::tessellate(style, 7, p.data(), p.size(), o, whole);
    TessellationState st;
    Tessellator::tessellateSegments(style, 7, p.data(), p.size(), 0, 10, o, st, parts);
    Tessellator::tessellateSegments(style, 7, p.data(), p.size(), 10, 25, o, st, parts);
    Tessellator::tessellateSegments(style, 7, p.data(), p.size(), 25, 39, o, st, parts);
    CHECK(whole.capsules.size() == parts.capsules.size());
    CHECK(whole.stamps.size() == parts.stamps.size());
    for (size_t i = 0; i < std::min(whole.capsules.size(), parts.capsules.size()); ++i)
      CHECK(sameCaps(whole.capsules[i], parts.capsules[i]));
    for (size_t i = 0; i < std::min(whole.stamps.size(), parts.stamps.size()); ++i)
      CHECK(whole.stamps[i].center == parts.stamps[i].center);
  }
}

TEST(tessellation_dot_and_nib) {
  StrokePoint one;
  one.pos = {5, 5};
  one.width = 3.0f;
  StrokeStyle style;
  StrokeGeometry g;
  Tessellator::tessellate(style, 1, &one, 1, {}, g);
  REQUIRE(g.capsules.size() == 1);
  CHECK_NEAR(g.capsules[0].r0, 1.5, 1e-6);
  style.tip = TipShape::Nib;
  style.nibThickness = 0.2f;
  g.clear();
  Tessellator::tessellate(style, 1, &one, 1, {}, g);
  REQUIRE(g.nibs.size() == 1);
  CHECK_NEAR(g.nibs[0].round, 0.3, 1e-6);
  CHECK_NEAR(g.nibs[0].h0 + g.nibs[0].round, 1.5, 1e-6);
}

TEST(transform_geometry_scales_radii) {
  StrokeGeometry g;
  CapsulePrim c;
  c.p0 = {1, 1};
  c.p1 = {3, 1};
  c.r0 = c.r1 = 1.0f;
  g.capsules.push_back(c);
  g.bounds = Rect(0, 0, 4, 2);
  transformGeometry(g, Affine::translate(10, 0) * Affine::scale(2));
  CHECK_NEAR(g.capsules[0].p0.x, 12.0, 1e-6);
  CHECK_NEAR(g.capsules[0].r0, 2.0, 1e-6);
  CHECK_NEAR(g.bounds.maxX, 18.0, 1e-5);
}
