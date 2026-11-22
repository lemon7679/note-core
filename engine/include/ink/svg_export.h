#pragma once

#include <string>
#include <vector>

#include "ink/page.h"

namespace ink {

/// Vector outline of a stroke, for PDF / SVG export.
///  * Round tips: a closed polygon (left offset, end cap, right offset, start cap)
///    to be filled with the non-zero rule.
///  * Nibs: the swept centre ribbon, to be filled AND stroked with `strokeWidth`
///    (round joins) — that Minkowski sum reproduces the rounded flat nib.
struct StrokeOutline {
  std::vector<Vec2> polygon;
  float strokeWidth = 0.0f;
  float meanAlpha = 1.0f;
};

StrokeOutline strokeOutline(const Stroke& stroke, float tolerance = 0.25f);

struct SvgOptions {
  bool drawBackground = true;
  Color background = colors::kWhite;
  float tolerance = 0.25f;  ///< Page units.
};

/// Whole page as an SVG document (page units = pt). Pencil grain is not
/// representable in SVG; pencil strokes export as soft translucent outlines.
std::string exportSvg(const Page& page, const SvgOptions& opts = {});

}  // namespace ink
