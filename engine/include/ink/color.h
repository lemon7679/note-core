#pragma once

#include <cstdint>

namespace ink {

/// Straight (non-premultiplied) sRGB color, components in [0, 1].
struct Color {
  float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;

  constexpr Color() = default;
  constexpr Color(float r_, float g_, float b_, float a_ = 1.0f) : r(r_), g(g_), b(b_), a(a_) {}

  /// 0xRRGGBB (alpha = 1).
  static constexpr Color fromRGB(uint32_t rgb) {
    return Color(channel(rgb >> 16), channel(rgb >> 8), channel(rgb), 1.0f);
  }
  /// 0xRRGGBBAA.
  static constexpr Color fromRGBA(uint32_t rgba) {
    return Color(channel(rgba >> 24), channel(rgba >> 16), channel(rgba >> 8), channel(rgba));
  }
  static constexpr float channel(uint32_t v) { return static_cast<float>(v & 0xFFu) / 255.0f; }
  constexpr bool operator==(const Color& o) const { return r == o.r && g == o.g && b == o.b && a == o.a; }
  constexpr bool operator!=(const Color& o) const { return !(*this == o); }
};

namespace colors {
constexpr Color kBlack = Color::fromRGB(0x1A1A1A);
constexpr Color kInkBlue = Color::fromRGB(0x1F3F9A);
constexpr Color kInkRed = Color::fromRGB(0xC62828);
constexpr Color kGraphite = Color::fromRGB(0x2B2B30);
constexpr Color kHighlightYellow = Color::fromRGB(0xFFE14D);
constexpr Color kHighlightGreen = Color::fromRGB(0x9BEA74);
constexpr Color kHighlightPink = Color::fromRGB(0xFF9AC8);
constexpr Color kWhite = Color(1, 1, 1, 1);
}  // namespace colors

}  // namespace ink
