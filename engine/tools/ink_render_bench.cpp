// Tile rendering benchmark for the app's hot paths (zoom, scroll, erase) on a
// dense "study notes" page: handwriting, many overlapping highlighter strokes
// and some pencil. Renders 512-px tiles through the C API exactly like the
// Android / iPad canvases do.
//
//   ink_render_bench            all scenarios
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "ink/ink.h"
#include "ink/ink_c.h"
#include "synth_input.h"

using namespace ink;
using Clock = std::chrono::steady_clock;

namespace {

double ms(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }

void add(PageEditor& ed, const Brush& brush, const std::vector<InputPoint>& in) {
  if (in.size() < 2) return;
  StrokeBuilder b(brush);
  b.begin(in.front());
  for (size_t i = 1; i < in.size(); ++i) b.addPoint(in[i]);
  b.end();
  ed.commitStroke(b);
}

/// A4 page of notes: 30 lines of cursive, ~90 highlighter strokes (stacked 1-3 deep), pencil sketches.
std::vector<uint8_t> notesPage() {
  PageEditor ed;
  uint32_t seed = 1;
  for (int row = 0; row < 30; ++row)
    for (int col = 0; col < 6; ++col) {
      synth::Options so;
      so.seed = seed++;
      add(ed, presets::ballpoint(), synth::synthesize(synth::cursiveLoops({40.0f + col * 90.0f, 60.0f + row * 25.0f}, 4, 14), so));
    }
  const Color hl[] = {colors::kHighlightYellow, colors::kHighlightGreen, colors::kHighlightPink};
  for (int row = 0; row < 30; ++row) {
    int layers = 1 + row % 3;
    for (int k = 0; k < layers; ++k) {
      synth::Options so;
      so.seed = seed++;
      so.speed = 420.0f;
      float y = 58.0f + row * 25.0f + k * 3.0f;
      add(ed, presets::highlighter(hl[(row + k) % 3]),
          synth::synthesize(synth::polyline({{30.0f + k * 20.0f, y}, {560.0f - k * 30.0f, y + 1.5f}}), so));
    }
  }
  // A few big diagonal / looping highlights on top.
  for (int k = 0; k < 6; ++k) {
    synth::Options so;
    so.seed = seed++;
    add(ed, presets::highlighter(hl[k % 3]), synth::synthesize(synth::wave({40.0f, 120.0f + k * 110.0f}, 500.0f, 40.0f, 160.0f), so));
  }
  for (int k = 0; k < 8; ++k) {
    synth::Options so;
    so.seed = seed++;
    so.pressure = 0.45f;
    add(ed, presets::pencil(), synth::synthesize(synth::scribble({60.0f + (k % 4) * 120.0f, 700.0f + (k / 4) * 60.0f}, 100.0f, 45.0f, seed), so));
  }
  return ed.save();
}

struct TileStats {
  double total = 0, worst = 0;
  int tiles = 0;
};

/// Renders the 512-px tiles covering a viewport (pixels) of the page at `scale` px/pt.
TileStats renderViewport(InkEditor* e, float scale, float vx, float vy, int vw, int vh) {
  const int T = 512;
  std::vector<uint8_t> px(static_cast<size_t>(T) * T * 4);
  TileStats st;
  for (int ty = static_cast<int>(vy) / T * T; ty < vy + vh; ty += T)
    for (int tx = static_cast<int>(vx) / T * T; tx < vx + vw; tx += T) {
      std::memset(px.data(), 255, px.size());
      auto t0 = Clock::now();
      ink_editor_render_over_rgba(e, px.data(), T, T, T * 4, scale, -static_cast<float>(tx), -static_cast<float>(ty));
      double d = ms(t0, Clock::now());
      st.total += d;
      st.worst = std::max(st.worst, d);
      ++st.tiles;
    }
  return st;
}

}  // namespace

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  std::vector<uint8_t> data = notesPage();
  InkEditor* e = ink_editor_create(595, 842);
  ink_editor_load(e, data.data(), data.size());
  std::printf("page: %zu strokes, %zu bytes\n", ink_editor_stroke_count(e), data.size());

  // Tablet in landscape (2560x1600): the page fills the width at zoom 1 (~4.2 px/pt).
  const int vw = 2560, vh = 1600;
  for (float zoom : {1.0f, 2.0f, 5.0f}) {
    float scale = 4.2f * zoom;
    // Viewport over the dense upper-middle part of the page.
    float vx = (595.0f * scale - vw) * 0.5f, vy = 150.0f * scale;
    if (vx < 0) vx = 0;
    // First render at this zoom (tessellation cache cold) and a second pass (warm).
    TileStats cold = renderViewport(e, scale, vx, vy, vw, vh);
    TileStats warm = renderViewport(e, scale, vx, vy, vw, vh);
    std::printf("zoom %.0fx (%.1f px/pt): %2d tiles  cold %7.1f ms (worst tile %6.1f)  warm %7.1f ms (worst %6.1f)\n",
                zoom, scale, warm.tiles, cold.total, cold.worst, warm.total, warm.worst);
  }

  // Precise eraser dragged across stacked highlighters at zoom 1; damage is
  // re-rendered once per frame (4 moves per frame at 240 Hz).
  {
    const float scale = 4.2f;
    ink_editor_erase_begin(e, INK_ERASER_PRECISE, 6.0f, 0, 300.0f, 100.0f);
    ink_editor_take_dirty(e);
    double total = 0, worst = 0, eraseMs = 0;
    int frames = 0;
    for (int i = 1; i <= 240; ++i) {
      auto m0 = Clock::now();
      ink_editor_erase_move(e, 300.0f + 0.3f * i, 100.0f + 1.6f * i);
      eraseMs += ms(m0, Clock::now());
      if (i % 4) continue;
      InkRect d = ink_editor_take_dirty(e);
      if (d.width <= 0) continue;
      auto f0 = Clock::now();
      // Repaint only the damaged pixels (background + ink), as the canvases do.
      int x0 = static_cast<int>(std::floor(d.x * scale)) - 1, y0 = static_cast<int>(std::floor(d.y * scale)) - 1;
      int pw = static_cast<int>(std::ceil(d.width * scale)) + 3, ph = static_cast<int>(std::ceil(d.height * scale)) + 3;
      std::vector<uint8_t> part(static_cast<size_t>(pw) * ph * 4, 255);
      ink_editor_render_over_rgba(e, part.data(), pw, ph, static_cast<size_t>(pw) * 4, scale, -static_cast<float>(x0),
                                  -static_cast<float>(y0));
      double f = ms(f0, Clock::now());
      total += f;
      worst = std::max(worst, f);
      ++frames;
    }
    ink_editor_erase_end(e);
    std::printf("precise erase across highlights: erase %.3f ms/move | redraw per frame avg %.1f ms, worst %.1f ms (%d frames)\n",
                eraseMs / 240.0, total / std::max(frames, 1), worst, frames);
  }
  ink_editor_destroy(e);
  return 0;
}
