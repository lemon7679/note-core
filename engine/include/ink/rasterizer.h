#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "ink/color.h"
#include "ink/geometry.h"
#include "ink/grain.h"
#include "ink/page.h"

namespace ink {

/// Premultiplied-alpha float RGBA image (sRGB-encoded values, the same space
/// UIKit / Core Graphics composite in by default).
class Bitmap {
 public:
  Bitmap() = default;
  Bitmap(int width, int height, Color fill = Color(0, 0, 0, 0));

  int width() const { return width_; }
  int height() const { return height_; }
  float* pixel(int x, int y) { return &px_[(static_cast<size_t>(y) * width_ + x) * 4]; }
  const float* pixel(int x, int y) const { return &px_[(static_cast<size_t>(y) * width_ + x) * 4]; }
  void clear(Color c);

  /// Straight-alpha 8-bit RGBA, rows of `stride` bytes.
  void toRGBA8(uint8_t* out, size_t stride) const;
  std::vector<uint8_t> toRGBA8() const;
  /// Un-premultiplied color of a pixel.
  Color colorAt(int x, int y) const;

 private:
  int width_ = 0, height_ = 0;
  std::vector<float> px_;
};

struct RenderOptions {
  Color background = colors::kWhite;
  bool drawBackground = true;
  /// Pixel-space clip; empty = whole bitmap.
  Rect clip;
  TessellationOptions tessellation;  ///< pixelsPerUnit is derived from the transform.
  const GrainTexture* grain = nullptr;  ///< nullptr = GrainTexture::shared().
};

/// Reference software renderer. Used for export, thumbnails, tests and as the
/// specification the GPU renderer is validated against.
///
/// Per stroke: primitives are rasterized into a coverage mask with MAX
/// blending (so the stroke's own overlaps never double up), then the mask is
/// composited once with the stroke's color, opacity and blend mode.
class Rasterizer {
 public:
  /// Renders all visible layers of the page. `pageToPixel` maps page units to bitmap pixels.
  static void renderPage(const Page& page, Bitmap& target, const Affine& pageToPixel,
                         const RenderOptions& opts = {});
  static void renderStroke(const Stroke& stroke, Bitmap& target, const Affine& pageToPixel,
                           const RenderOptions& opts = {});
  /// Composites already-tessellated page-space geometry (e.g. a live stroke).
  static void renderGeometry(const StrokeGeometry& pageGeometry, const StrokeStyle& style, Bitmap& target,
                             const Affine& pageToPixel, const RenderOptions& opts = {});
  /// Coverage mask only (no compositing), pixel-space geometry. `mask` is
  /// box.width*box.height floats for the integer pixel box [x0,x1)x[y0,y1).
  static void rasterizeCoverage(const StrokeGeometry& pixelGeometry, const StrokeStyle& style,
                                const Affine& pixelToPage, const GrainTexture& grain, int x0, int y0,
                                int x1, int y1, std::vector<float>& mask);
};

}  // namespace ink
