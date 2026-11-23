#include "synth_input.h"

#include <cmath>

using ink::Vec2;

namespace synth {

namespace {

float noise(uint32_t seed, uint32_t i) {
  // Sum of 3 uniforms ~ bell-shaped in [-1.5, 1.5].
  float s = 0.0f;
  for (uint32_t k = 0; k < 3; ++k) s += ink::hashToUnit(ink::hash32(seed * 7u + k, i)) - 0.5f;
  return s;
}

float pathLength(const std::vector<Vec2>& p) {
  float l = 0.0f;
  for (size_t i = 1; i < p.size(); ++i) l += ink::distance(p[i - 1], p[i]);
  return l;
}

/// Unsigned curvature (1/pt) around index i, from the turning angle over a window.
float curvatureAt(const std::vector<Vec2>& p, size_t i) {
  size_t a = i >= 3 ? i - 3 : 0, b = std::min(i + 3, p.size() - 1);
  if (b <= a + 1) return 0.0f;
  Vec2 d0 = p[i] - p[a], d1 = p[b] - p[i];
  float l0 = ink::length(d0), l1 = ink::length(d1);
  if (l0 < 1e-4f || l1 < 1e-4f) return 0.0f;
  float ang = std::acos(ink::clamp(ink::dot(d0, d1) / (l0 * l1), -1.0f, 1.0f));
  return ang / (0.5f * (l0 + l1));
}

}  // namespace

std::vector<ink::InputPoint> synthesize(const std::vector<Vec2>& path, const Options& o) {
  std::vector<ink::InputPoint> out;
  if (path.empty()) return out;
  const float total = pathLength(path);
  const float dt = 1.0f / o.rate;
  float s = 0.0f;  // arc length travelled
  size_t seg = 0;
  float segStart = 0.0f;
  uint32_t i = 0;
  double t = o.startTime;
  const float tremorPhase = ink::hashToUnit(ink::hash32(o.seed, 99u)) * 6.28f;
  // Estimated duration for the pressure ramps.
  for (;;) {
    while (seg + 1 < path.size() && segStart + ink::distance(path[seg], path[seg + 1]) < s) {
      segStart += ink::distance(path[seg], path[seg + 1]);
      ++seg;
    }
    Vec2 pos;
    if (seg + 1 < path.size()) {
      float l = ink::distance(path[seg], path[seg + 1]);
      pos = ink::lerp(path[seg], path[seg + 1], l > 0 ? (s - segStart) / l : 0.0f);
    } else {
      pos = path.back();
    }
    float u = total > 0.0f ? ink::clamp01(s / total) : 1.0f;
    // Sensor jitter + slow tremor.
    Vec2 j{noise(o.seed, i * 2u) * o.jitter, noise(o.seed, i * 2u + 1u) * o.jitter};
    float tt = static_cast<float>(t - o.startTime);
    j += Vec2{std::sin(tt * 9.0f + tremorPhase), std::cos(tt * 7.0f + tremorPhase * 1.3f)} * o.tremor * 0.5f;

    ink::InputPoint p;
    p.pos = pos + j;
    p.time = t;
    p.source = o.source;
    if (o.hasPressure) {
      float wave = std::sin(s / o.pressureWavelength * 6.2832f + tremorPhase);
      float pr = o.pressure + o.pressureWave * wave;
      // Touch-down / lift-off ramps (first and last ~25 ms worth of path).
      float rampLen = std::max(o.speed * 0.025f, 1.0f);
      pr *= ink::smoothstep(0.0f, 1.0f, std::min(s / rampLen + 0.25f, 1.0f));
      pr *= ink::smoothstep(0.0f, 1.0f, std::min((total - s) / rampLen + 0.25f, 1.0f));
      p.pressure = ink::clamp01(pr);
      p.altitude = o.altitude;
      p.azimuth = o.azimuth;
    }
    p.updateId = static_cast<uint64_t>(o.seed) * 100000u + i + 1u;
    out.push_back(p);
    if (s >= total) break;
    // 2/3 power law: slow down in tight curves; ease in/out at the ends.
    float k = seg < path.size() ? curvatureAt(path, seg) : 0.0f;
    float curveFactor = std::pow(1.0f / (1.0f + k * 6.0f), 1.0f / 3.0f);
    float envelope = 0.35f + 0.65f * std::sqrt(std::sin(ink::kPi * ink::clamp(u, 0.02f, 0.98f)));
    s = std::min(total, s + o.speed * curveFactor * envelope * dt);
    t += dt;
    ++i;
  }
  return out;
}

std::vector<Vec2> polyline(const std::vector<Vec2>& pts, float step) {
  std::vector<Vec2> out;
  const Vec2* prev = nullptr;
  for (const Vec2& p : pts) {
    if (prev) {
      int n = std::max(1, static_cast<int>(std::ceil(ink::distance(*prev, p) / step)));
      for (int k = 1; k <= n; ++k) out.push_back(ink::lerp(*prev, p, static_cast<float>(k) / n));
    } else {
      out.push_back(p);
    }
    prev = &p;
  }
  return out;
}

std::vector<Vec2> cursiveLoops(Vec2 origin, int loops, float size) {
  // Prolate trochoid: the classic "llll" cursive exercise.
  std::vector<Vec2> out;
  const float a = size * 0.18f, b = size * 0.42f, c = size * 0.5f;
  const float tEnd = loops * ink::kTwoPi + ink::kPi * 0.5f;
  for (float t = -ink::kPi * 0.5f; t <= tEnd; t += 0.02f)
    out.push_back(origin + Vec2{a * t - b * std::sin(t) + b, -c * std::cos(t) - c * 0.0f});
  return out;
}

std::vector<Vec2> wave(Vec2 origin, float length, float amplitude, float wavelength) {
  std::vector<Vec2> out;
  for (float x = 0.0f; x <= length; x += 0.5f)
    out.push_back(origin + Vec2{x, amplitude * std::sin(x / wavelength * ink::kTwoPi)});
  return out;
}

std::vector<Vec2> spiral(Vec2 center, float turns, float r0, float r1) {
  std::vector<Vec2> out;
  const float end = turns * ink::kTwoPi;
  for (float t = 0.0f; t <= end; t += 0.01f) {
    float r = ink::lerp(r0, r1, t / end);
    out.push_back(center + Vec2{std::cos(t), std::sin(t)} * r);
  }
  return out;
}

std::vector<Vec2> ellipse(Vec2 center, float rx, float ry, float wobble, uint32_t seed, float startAngle,
                          float sweep) {
  std::vector<Vec2> out;
  float ph = ink::hashToUnit(ink::hash32(seed, 5u)) * 6.0f;
  for (float t = 0.0f; t <= 1.0f; t += 0.002f) {
    float a = startAngle + sweep * t;
    float w = 1.0f + wobble * std::sin(a * 3.0f + ph) + wobble * 0.5f * std::sin(a * 5.0f - ph);
    out.push_back(center + Vec2{std::cos(a) * rx * w, std::sin(a) * ry * w});
  }
  return out;
}

std::vector<Vec2> scribble(Vec2 origin, float width, float height, uint32_t seed) {
  std::vector<Vec2> out;
  float x = 0.0f;
  float phase = 0.0f;
  int i = 0;
  while (x < width) {
    float r = ink::hashToUnit(ink::hash32(seed, static_cast<uint32_t>(i++)));
    float amp = height * (0.35f + 0.65f * r);
    float speed = 0.10f + 0.10f * ink::hashToUnit(ink::hash32(seed + 3u, static_cast<uint32_t>(i)));
    for (int k = 0; k < 40; ++k) {
      phase += speed;
      x += 0.45f + 0.35f * std::sin(phase * 0.5f);
      out.push_back(origin + Vec2{x + std::sin(phase) * height * 0.22f, -amp * 0.5f * (1.0f + std::cos(phase))});
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// Hangul handwriting
// ---------------------------------------------------------------------------

namespace {

struct GlyphStroke {
  std::vector<Vec2> pts;  // polyline in a 20 x 22 box, or empty for an ellipse
  Vec2 c, r;              // ellipse
};

std::vector<GlyphStroke> glyph(char32_t ch) {
  using P = std::vector<Vec2>;
  auto L = [](P p) { return GlyphStroke{std::move(p), {}, {}}; };
  auto O = [](Vec2 c, Vec2 r) { return GlyphStroke{{}, c, r}; };
  const GlyphStroke aV = L({{14, 2}, {14, 20}}), aH = L({{14, 11}, {18, 11}});  // ㅏ
  switch (ch) {
    case U'가': return {L({{2, 5}, {10, 5}, {9.5f, 10}, {6, 17}}), aV, aH};
    case U'나': return {L({{3, 4}, {3, 16}, {11, 16}}), aV, aH};
    case U'다': return {L({{3, 5}, {10, 5}}), L({{3, 5}, {3, 16}, {11, 16}}), aV, aH};
    case U'라': return {L({{2, 4}, {10, 4}, {10, 10}, {2, 10}, {2, 16}, {11, 16}}), aV, aH};
    case U'마': return {L({{3, 4}, {3, 16}}), L({{3, 4}, {11, 4}, {11, 16}, {3, 16}}), aV, aH};
    case U'바': return {L({{3, 3}, {3, 16}}), L({{10, 3}, {10, 16}}), L({{3, 9}, {10, 9}}), L({{3, 16}, {10, 16}}), aV, aH};
    case U'사': return {L({{7, 4}, {6, 9}, {2, 16}}), L({{6.5f, 9}, {11, 16}}), aV, aH};
    case U'아': return {O({7, 10}, {4.5f, 5.5f}), aV, aH};
    case U'한':
      return {L({{5, 1.5f}, {8, 2.5f}}), L({{3, 4.5f}, {11, 4.5f}}), O({7, 8.5f}, {3, 2.6f}),
              L({{14.5f, 1}, {14.5f, 13}}), L({{14.5f, 6.5f}, {18, 6.5f}}), L({{5, 14}, {5, 20}, {16, 20}})};
    case U'글':
      return {L({{4, 2}, {15, 2}, {14.5f, 7}}), L({{2, 9.5f}, {18, 9.5f}}),
              L({{4, 12}, {15, 12}, {15, 15.5f}, {4, 15.5f}, {4, 19.5f}, {16, 19.5f}})};
    case U'보':
      return {L({{5, 2}, {5, 10}}), L({{13, 2}, {13, 10}}), L({{5, 6}, {13, 6}}), L({{5, 10}, {13, 10}}),
              L({{9, 11}, {9, 15}}), L({{2, 16}, {17, 16}})};
    case U'정':
      return {L({{2, 3}, {10, 3}, {4, 12}}), L({{6.5f, 7}, {11, 12}}), L({{16, 1}, {16, 14}}),
              L({{12.5f, 7}, {16, 7}}), O({9, 17.5f}, {3.5f, 3})};
    default: return {};
  }
}

}  // namespace

std::vector<std::vector<Vec2>> hangul(char32_t syllable, Vec2 origin, float size, uint32_t seed, float messiness) {
  std::vector<std::vector<Vec2>> out;
  const float k = size / 20.0f;
  uint32_t n = 0;
  auto rnd = [&]() { return ink::hashToUnit(ink::hash32(seed, n++)) * 2.0f - 1.0f; };
  for (const GlyphStroke& g : glyph(syllable)) {
    if (g.pts.empty()) {
      Vec2 c = origin + g.c * k + Vec2{rnd(), rnd()} * (messiness * 0.8f * k);
      out.push_back(ellipse(c, g.r.x * k * (1.0f + 0.1f * messiness * rnd()), g.r.y * k, 0.03f + 0.04f * messiness,
                            seed + n, -1.7f + 0.3f * rnd(), 6.45f));
      continue;
    }
    // A hurried hand: corners land off-target and strokes are slightly tilted.
    float tilt = rnd() * messiness * 0.09f;
    std::vector<Vec2> pts;
    Vec2 first = g.pts.front();
    for (Vec2 p : g.pts) {
      Vec2 d = p - first;
      Vec2 r{d.x * std::cos(tilt) - d.y * std::sin(tilt), d.x * std::sin(tilt) + d.y * std::cos(tilt)};
      pts.push_back(origin + (first + r) * k + Vec2{rnd(), rnd()} * (messiness * 0.6f * k));
    }
    out.push_back(polyline(pts, 0.4f));
  }
  return out;
}

std::vector<WrittenStroke> writeLine(const std::u32string& text, Vec2 origin, float size, uint32_t seed,
                                     float messiness, float slant, const std::vector<float>& baseline) {
  std::vector<WrittenStroke> out;
  double t = 0.0;
  const float advance = size * 1.12f;
  float x = 0.0f;
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == U' ') {
      x += size * 0.7f;
      continue;
    }
    float dy = i < baseline.size() ? baseline[i] : 0.0f;
    Vec2 local{x, dy};
    Vec2 rotated{local.x * std::cos(slant) - local.y * std::sin(slant), local.x * std::sin(slant) + local.y * std::cos(slant)};
    auto strokes = hangul(text[i], origin + rotated, size, seed + static_cast<uint32_t>(i) * 31u, messiness);
    // Rotate each glyph by the slant as well (the whole line is tilted).
    for (auto& path : strokes) {
      for (Vec2& p : path) {
        Vec2 d = p - (origin + rotated);
        p = origin + rotated + Vec2{d.x * std::cos(slant) - d.y * std::sin(slant), d.x * std::sin(slant) + d.y * std::cos(slant)};
      }
      Options o;
      o.seed = seed + static_cast<uint32_t>(out.size()) * 7u + 1u;
      o.speed = 140.0f;
      o.tremor = 0.25f + 0.6f * messiness;
      o.jitter = 0.12f + 0.2f * messiness;
      o.startTime = t;
      WrittenStroke ws;
      ws.input = synthesize(path, o);
      ws.syllable = i;
      t = ws.input.back().time + 0.12;
      out.push_back(std::move(ws));
    }
    x += advance;
  }
  return out;
}

}  // namespace synth
