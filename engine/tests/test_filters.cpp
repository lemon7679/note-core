#include "ink/filters.h"
#include "ink/pressure_curve.h"
#include "test.h"

using namespace ink;

TEST(one_euro_constant_input_is_fixed_point) {
  OneEuroFilter2D f;
  Vec2 out;
  for (int i = 0; i < 100; ++i) out = f.filter({10.0f, -3.0f}, 1.0f / 240.0f);
  CHECK_NEAR(out.x, 10.0, 1e-5);
  CHECK_NEAR(out.y, -3.0, 1e-5);
}

TEST(one_euro_reduces_jitter_at_rest) {
  OneEuroFilter2D f;
  float rawVar = 0.0f, outVar = 0.0f;
  for (int i = 0; i < 2000; ++i) {
    float n = hashToUnit(hash32(static_cast<uint32_t>(i))) - 0.5f;
    Vec2 out = f.filter({n, 0.0f}, 1.0f / 240.0f);
    if (i > 50) {
      rawVar += n * n;
      outVar += out.x * out.x;
    }
  }
  CHECK(outVar < rawVar * 0.25f);
}

TEST(one_euro_tracks_fast_motion_with_small_lag) {
  OneEuroFilter2D f;
  Vec2 out;
  const float speed = 600.0f;  // pt/s
  for (int i = 0; i < 240; ++i) out = f.filter({speed * i / 240.0f, 0.0f}, 1.0f / 240.0f);
  float lag = speed * 239 / 240.0f - out.x;
  CHECK(lag > 0.0f);
  CHECK(lag < 8.0f);  // < ~13 ms behind at 600 pt/s
}

TEST(pulled_string_trails_and_catches_up) {
  StabilizerParams p;
  p.mode = StabilizerMode::PulledString;
  p.amount = 10.0f;
  Stabilizer s(p);
  s.reset({0, 0}, 0.0, 0);
  std::vector<StabilizedSample> out;
  for (int i = 1; i <= 100; ++i) s.push({static_cast<float>(i), 0.0f}, i * 0.01, static_cast<uint32_t>(i), out);
  REQUIRE(!out.empty());
  CHECK_NEAR(out.back().pos.x, 90.0, 1e-3);  // trails by the string length
  out.clear();
  s.flush(out);
  REQUIRE(!out.empty());
  CHECK_NEAR(out.back().pos.x, 100.0, 1e-3);
}

TEST(moving_average_flush_converges) {
  StabilizerParams p;
  p.mode = StabilizerMode::MovingAverage;
  p.amount = 80.0f;
  Stabilizer s(p);
  s.reset({0, 0}, 0.0, 0);
  std::vector<StabilizedSample> out;
  for (int i = 1; i <= 60; ++i) s.push({static_cast<float>(i), 0.0f}, i / 240.0, static_cast<uint32_t>(i), out);
  CHECK(out.back().pos.x < 60.0f);
  out.clear();
  s.flush(out);
  REQUIRE(!out.empty());
  CHECK_NEAR(out.back().pos.x, 60.0, 1e-3);
}

TEST(pressure_curve_properties) {
  PressureCurve lin = PressureCurve::linear();
  for (float x = 0.0f; x <= 1.0f; x += 0.05f) CHECK_NEAR(lin.evaluate(x), x, 0.01);
  PressureCurve soft = PressureCurve::soft();
  CHECK_NEAR(soft.evaluate(0.0f), 0.0, 1e-4);
  CHECK_NEAR(soft.evaluate(1.0f), 1.0, 1e-4);
  CHECK(soft.evaluate(0.3f) > 0.45f);  // light writing pressure is boosted
  float prev = -1.0f;
  for (float x = 0.0f; x <= 1.0f; x += 0.01f) {
    float y = soft.evaluate(x);
    CHECK(y >= prev - 1e-6f);  // monotonic
    prev = y;
  }
  CHECK_NEAR(soft.evaluate(-1.0f), 0.0, 1e-4);
  CHECK_NEAR(soft.evaluate(2.0f), 1.0, 1e-4);
}
