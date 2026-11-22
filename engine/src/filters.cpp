#include "ink/filters.h"

namespace ink {

float lowPassAlpha(float cutoffHz, float dt) {
  if (dt <= 0.0f) return 1.0f;
  float tau = 1.0f / (kTwoPi * std::max(cutoffHz, 1e-3f));
  return 1.0f / (1.0f + tau / dt);
}

float emaAlpha(float tau, float dt) {
  if (tau <= 0.0f) return 1.0f;
  if (dt <= 0.0f) return 0.0f;
  return 1.0f - std::exp(-dt / tau);
}

Vec2 OneEuroFilter2D::filter(Vec2 x, float dt, float speedScale) {
  if (!initialized_) {
    initialized_ = true;
    x_ = x;
    dx_ = {0.0f, 0.0f};
    return x_;
  }
  dt = std::max(dt, 1e-4f);
  // Derivative estimate (low-passed with a fixed cutoff).
  Vec2 rawDx = (x - x_) / dt;
  float ad = lowPassAlpha(params_.dCutoff, dt);
  dx_ = lerp(dx_, rawDx, ad);
  // Adaptive cutoff from speed magnitude (2D: one cutoff for both axes keeps curves round).
  float speed = length(dx_) * speedScale;
  float cutoff = params_.minCutoff + params_.beta * speed;
  float a = lowPassAlpha(cutoff, dt);
  x_ = lerp(x_, x, a);
  return x_;
}

// ---------------------------------------------------------------------------

void Stabilizer::reset(Vec2 p, double t, uint32_t source, float unitsPerViewPoint) {
  unitsPerViewPoint_ = unitsPerViewPoint;
  cursor_ = p;
  last_ = {p, t, source};
  hasLast_ = true;
  window_.clear();
  window_.push_back(last_);
}

Vec2 Stabilizer::averaged() const {
  // Triangular weights: recent samples count more, so the cursor keeps up.
  Vec2 sum;
  float wsum = 0.0f;
  float i = 1.0f;
  for (const Entry& e : window_) {
    sum += e.pos * i;
    wsum += i;
    i += 1.0f;
  }
  return wsum > 0.0f ? sum / wsum : last_.pos;
}

void Stabilizer::push(Vec2 p, double t, uint32_t source, std::vector<StabilizedSample>& out) {
  last_ = {p, t, source};
  hasLast_ = true;
  switch (params_.mode) {
    case StabilizerMode::None:
      out.push_back({p, t, source});
      break;
    case StabilizerMode::PulledString: {
      float len = std::max(0.0f, params_.amount) * unitsPerViewPoint_;
      Vec2 d = p - cursor_;
      float dist = length(d);
      if (dist > len) {
        cursor_ = p - d * (len / dist);
        out.push_back({cursor_, t, source});
      }
      break;
    }
    case StabilizerMode::MovingAverage: {
      double window = std::max(0.0f, params_.amount) * 1e-3;
      window_.push_back({p, t, source});
      while (window_.size() > 1 && t - window_.front().time > window) window_.pop_front();
      cursor_ = averaged();
      out.push_back({cursor_, t, source});
      break;
    }
  }
}

void Stabilizer::flush(std::vector<StabilizedSample>& out) {
  if (!hasLast_) return;
  switch (params_.mode) {
    case StabilizerMode::None:
      break;
    case StabilizerMode::PulledString:
      if (params_.catchUp && distance(cursor_, last_.pos) > 1e-4f) {
        // Ease into the pen position in a few steps so the end is not a kink.
        Vec2 from = cursor_;
        const int steps = 4;
        for (int i = 1; i <= steps; ++i) {
          float t = easeOutQuad(static_cast<float>(i) / steps);
          out.push_back({lerp(from, last_.pos, t), last_.time, last_.source});
        }
        cursor_ = last_.pos;
      }
      break;
    case StabilizerMode::MovingAverage:
      if (params_.catchUp) {
        // Drop the oldest samples one by one: the average converges to the pen.
        while (window_.size() > 1) {
          window_.pop_front();
          cursor_ = averaged();
          out.push_back({cursor_, last_.time, last_.source});
        }
      }
      break;
  }
}

}  // namespace ink
