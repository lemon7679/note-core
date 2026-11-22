// InkEngine — cross-platform handwriting engine.
// Basic 2D math used throughout the engine. Header-only, no dependencies.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace ink {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 2.0f * kPi;

/// Converts millimetres to page units (PostScript points, 1/72 inch).
constexpr float mmToPt(float mm) { return mm * (72.0f / 25.4f); }

struct Vec2 {
  float x = 0.0f;
  float y = 0.0f;

  constexpr Vec2() = default;
  constexpr Vec2(float x_, float y_) : x(x_), y(y_) {}

  constexpr Vec2 operator+(Vec2 o) const { return {x + o.x, y + o.y}; }
  constexpr Vec2 operator-(Vec2 o) const { return {x - o.x, y - o.y}; }
  constexpr Vec2 operator*(float s) const { return {x * s, y * s}; }
  constexpr Vec2 operator/(float s) const { return {x / s, y / s}; }
  constexpr Vec2 operator-() const { return {-x, -y}; }
  Vec2& operator+=(Vec2 o) { x += o.x; y += o.y; return *this; }
  Vec2& operator-=(Vec2 o) { x -= o.x; y -= o.y; return *this; }
  Vec2& operator*=(float s) { x *= s; y *= s; return *this; }
  constexpr bool operator==(Vec2 o) const { return x == o.x && y == o.y; }
  constexpr bool operator!=(Vec2 o) const { return !(*this == o); }
};

constexpr Vec2 operator*(float s, Vec2 v) { return {v.x * s, v.y * s}; }

inline float dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
inline float cross(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }
inline float lengthSq(Vec2 a) { return dot(a, a); }
inline float length(Vec2 a) { return std::sqrt(dot(a, a)); }
inline float distance(Vec2 a, Vec2 b) { return length(a - b); }
inline float distanceSq(Vec2 a, Vec2 b) { return lengthSq(a - b); }
/// Counter-clockwise perpendicular.
inline Vec2 perp(Vec2 a) { return {-a.y, a.x}; }
inline Vec2 normalize(Vec2 a, Vec2 fallback = {1.0f, 0.0f}) {
  float l = length(a);
  return l > 1e-12f ? a / l : fallback;
}
inline Vec2 fromAngle(float radians) { return {std::cos(radians), std::sin(radians)}; }

inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
inline Vec2 lerp(Vec2 a, Vec2 b, float t) { return a + (b - a) * t; }
inline float clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float clamp01(float v) { return clamp(v, 0.0f, 1.0f); }
inline float smoothstep(float e0, float e1, float x) {
  if (e0 == e1) return x < e0 ? 0.0f : 1.0f;
  float t = clamp01((x - e0) / (e1 - e0));
  return t * t * (3.0f - 2.0f * t);
}
/// Ease-out (quadratic): fast start, gentle finish. Used for tapers.
inline float easeOutQuad(float t) {
  t = clamp01(t);
  return 1.0f - (1.0f - t) * (1.0f - t);
}
inline float easeInOutSine(float t) { return 0.5f - 0.5f * std::cos(clamp01(t) * kPi); }

/// Wraps an angle to (-pi, pi].
inline float wrapAngle(float a) {
  a = std::fmod(a + kPi, kTwoPi);
  if (a < 0) a += kTwoPi;
  return a - kPi;
}
/// Interpolates angles along the shortest arc.
inline float lerpAngle(float a, float b, float t) { return a + wrapAngle(b - a) * t; }

/// Closest-point parameter of p on segment ab, in [0, 1].
inline float segmentParam(Vec2 p, Vec2 a, Vec2 b) {
  Vec2 ab = b - a;
  float l2 = lengthSq(ab);
  if (l2 <= 1e-20f) return 0.0f;
  return clamp01(dot(p - a, ab) / l2);
}

inline float pointSegmentDistance(Vec2 p, Vec2 a, Vec2 b) {
  return distance(p, lerp(a, b, segmentParam(p, a, b)));
}

/// Minimum distance between segments ab and cd.
inline float segmentSegmentDistance(Vec2 a, Vec2 b, Vec2 c, Vec2 d) {
  // Proper intersection -> 0.
  Vec2 r = b - a, s = d - c;
  float denom = cross(r, s);
  if (std::fabs(denom) > 1e-12f) {
    float t = cross(c - a, s) / denom;
    float u = cross(c - a, r) / denom;
    if (t >= 0.0f && t <= 1.0f && u >= 0.0f && u <= 1.0f) return 0.0f;
  }
  float d1 = pointSegmentDistance(a, c, d);
  float d2 = pointSegmentDistance(b, c, d);
  float d3 = pointSegmentDistance(c, a, b);
  float d4 = pointSegmentDistance(d, a, b);
  return std::min(std::min(d1, d2), std::min(d3, d4));
}

struct Rect {
  float minX = std::numeric_limits<float>::infinity();
  float minY = std::numeric_limits<float>::infinity();
  float maxX = -std::numeric_limits<float>::infinity();
  float maxY = -std::numeric_limits<float>::infinity();

  constexpr Rect() = default;
  constexpr Rect(float x0, float y0, float x1, float y1) : minX(x0), minY(y0), maxX(x1), maxY(y1) {}
  static Rect fromXYWH(float x, float y, float w, float h) { return {x, y, x + w, y + h}; }
  static Rect empty() { return Rect(); }

  bool isEmpty() const { return !(minX <= maxX && minY <= maxY); }
  float width() const { return isEmpty() ? 0.0f : maxX - minX; }
  float height() const { return isEmpty() ? 0.0f : maxY - minY; }
  Vec2 center() const { return {(minX + maxX) * 0.5f, (minY + maxY) * 0.5f}; }

  void expand(Vec2 p) {
    minX = std::min(minX, p.x); minY = std::min(minY, p.y);
    maxX = std::max(maxX, p.x); maxY = std::max(maxY, p.y);
  }
  void expand(Vec2 p, float radius) {
    minX = std::min(minX, p.x - radius); minY = std::min(minY, p.y - radius);
    maxX = std::max(maxX, p.x + radius); maxY = std::max(maxY, p.y + radius);
  }
  void expand(const Rect& r) {
    if (r.isEmpty()) return;
    minX = std::min(minX, r.minX); minY = std::min(minY, r.minY);
    maxX = std::max(maxX, r.maxX); maxY = std::max(maxY, r.maxY);
  }
  Rect inflated(float d) const {
    if (isEmpty()) return *this;
    return {minX - d, minY - d, maxX + d, maxY + d};
  }
  bool intersects(const Rect& o) const {
    return !isEmpty() && !o.isEmpty() && minX <= o.maxX && o.minX <= maxX && minY <= o.maxY &&
           o.minY <= maxY;
  }
  bool contains(Vec2 p) const { return p.x >= minX && p.x <= maxX && p.y >= minY && p.y <= maxY; }
  Rect intersection(const Rect& o) const {
    Rect r(std::max(minX, o.minX), std::max(minY, o.minY), std::min(maxX, o.maxX),
           std::min(maxY, o.maxY));
    return r.isEmpty() ? Rect() : r;
  }
};

/// 2D affine transform:  x' = a*x + c*y + tx,  y' = b*x + d*y + ty
/// (same convention as CGAffineTransform).
struct Affine {
  float a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;

  static Affine identity() { return {}; }
  static Affine translate(float x, float y) { Affine t; t.tx = x; t.ty = y; return t; }
  static Affine scale(float sx, float sy) { Affine t; t.a = sx; t.d = sy; return t; }
  static Affine scale(float s) { return scale(s, s); }
  static Affine rotate(float radians) {
    Affine t;
    float cs = std::cos(radians), sn = std::sin(radians);
    t.a = cs; t.b = sn; t.c = -sn; t.d = cs;
    return t;
  }

  Vec2 apply(Vec2 p) const { return {a * p.x + c * p.y + tx, b * p.x + d * p.y + ty}; }
  Vec2 applyVector(Vec2 v) const { return {a * v.x + c * v.y, b * v.x + d * v.y}; }
  float determinant() const { return a * d - b * c; }
  /// Geometric-mean scale factor; used to scale widths/radii.
  float uniformScale() const { return std::sqrt(std::fabs(determinant())); }
  bool isIdentity() const { return a == 1 && b == 0 && c == 0 && d == 1 && tx == 0 && ty == 0; }

  Affine inverse() const {
    float det = determinant();
    if (std::fabs(det) < 1e-20f) return identity();
    float id = 1.0f / det;
    Affine r;
    r.a = d * id; r.b = -b * id; r.c = -c * id; r.d = a * id;
    r.tx = -(r.a * tx + r.c * ty);
    r.ty = -(r.b * tx + r.d * ty);
    return r;
  }

  /// Returns the transform that applies `first`, then `*this`.
  Affine operator*(const Affine& first) const {
    Affine r;
    r.a = a * first.a + c * first.b;
    r.b = b * first.a + d * first.b;
    r.c = a * first.c + c * first.d;
    r.d = b * first.c + d * first.d;
    r.tx = a * first.tx + c * first.ty + tx;
    r.ty = b * first.tx + d * first.ty + ty;
    return r;
  }

  Rect apply(const Rect& r) const {
    if (r.isEmpty()) return r;
    Rect o;
    o.expand(apply(Vec2{r.minX, r.minY}));
    o.expand(apply(Vec2{r.maxX, r.minY}));
    o.expand(apply(Vec2{r.minX, r.maxY}));
    o.expand(apply(Vec2{r.maxX, r.maxY}));
    return o;
  }
};

/// Small, fast, deterministic integer hash (used for reproducible jitter).
inline uint32_t hash32(uint32_t x) {
  x ^= x >> 16; x *= 0x7feb352dU;
  x ^= x >> 15; x *= 0x846ca68bU;
  x ^= x >> 16;
  return x;
}
inline uint32_t hash32(uint32_t a, uint32_t b) { return hash32(a ^ (hash32(b) + 0x9e3779b9U + (a << 6) + (a >> 2))); }
/// Uniform float in [0, 1) derived from a hash.
inline float hashToUnit(uint32_t h) { return static_cast<float>(h >> 8) * (1.0f / 16777216.0f); }

}  // namespace ink
