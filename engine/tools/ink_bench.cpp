// Micro-benchmarks for the hot paths (input processing, tessellation, erasing).
#include <chrono>
#include <cstdio>

#include "ink/ink.h"
#include "synth_input.h"

using namespace ink;
using Clock = std::chrono::steady_clock;

namespace {
double ms(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }
}  // namespace

int main() {
  synth::Options o;
  auto in = synth::synthesize(synth::cursiveLoops({10, 50}, 40, 30), o);
  std::printf("input: %zu samples (%.2f s of writing at 240 Hz)\n", in.size(), in.size() / 240.0);

  for (BrushKind k : {BrushKind::Ballpoint, BrushKind::Pencil, BrushKind::Highlighter, BrushKind::FountainPen}) {
    Brush brush = presets::forKind(k);
    // 1) Input pipeline only.
    auto t0 = Clock::now();
    const int reps = 20;
    Stroke s;
    for (int r = 0; r < reps; ++r) {
      StrokeBuilder b(brush);
      b.begin(in.front());
      for (size_t i = 1; i < in.size(); ++i) b.addPoint(in[i]);
      b.end();
      s = b.buildStroke();
    }
    auto t1 = Clock::now();
    // 2) Live loop: geometry every 4 samples (= 60 fps at 240 Hz input).
    StrokeBuilder live(brush);
    TessellationOptions topt;
    topt.pixelsPerUnit = 2.0f;
    live.begin(in.front());
    size_t frames = 0;
    double worst = 0.0;
    for (size_t i = 1; i < in.size(); ++i) {
      live.addPoint(in[i]);
      if (i % 4 == 0) {
        auto f0 = Clock::now();
        live.updateGeometry(topt);
        worst = std::max(worst, ms(f0, Clock::now()));
        ++frames;
      }
    }
    auto t2 = Clock::now();
    // 3) Full tessellation of the finished stroke.
    StrokeGeometry g;
    for (int r = 0; r < reps; ++r) {
      g.clear();
      Tessellator::tessellate(s, topt, g);
    }
    auto t3 = Clock::now();
    std::printf("%-12s input %.3f us/sample | live frame worst %.3f ms (%zu frames) | tessellate %.3f ms -> %zu prims\n",
                brushKindName(k), ms(t0, t1) * 1000.0 / (reps * in.size()), worst, frames, ms(t2, t3) / reps,
                g.primitiveCount());
  }

  // Eraser on a dense page.
  PageEditor ed;
  uint32_t seed = 1;
  for (int row = 0; row < 40; ++row)
    for (int col = 0; col < 12; ++col) {
      synth::Options so;
      so.seed = seed++;
      auto pts = synth::synthesize(synth::cursiveLoops({10.0f + col * 48.0f, 20.0f + row * 20.0f}, 2, 12), so);
      StrokeBuilder b(presets::ballpoint());
      b.begin(pts.front());
      for (size_t i = 1; i < pts.size(); ++i) b.addPoint(pts[i]);
      b.end();
      ed.commitStroke(b);
    }
  EraserOptions eo;
  eo.mode = EraserMode::Precise;
  eo.radius = 6.0f;
  auto e0 = Clock::now();
  ed.beginErase(eo, {0, 0});
  int moves = 0;
  for (float t = 0; t <= 1.0f; t += 0.002f, ++moves) ed.eraseTo({600.0f * t, 820.0f * t});
  ed.endErase();
  auto e1 = Clock::now();
  std::printf("precise eraser: %d moves over %zu strokes -> %.3f ms/move\n", moves, ed.page().strokeCount(),
              ms(e0, e1) / moves);

  Bitmap bmp(1190, 1684);
  auto r0 = Clock::now();
  Rasterizer::renderPage(ed.page(), bmp, Affine::scale(2.0f));
  auto r1 = Clock::now();
  std::printf("CPU render full A4 page @2x (%zu strokes): %.1f ms\n", ed.page().strokeCount(), ms(r0, r1));
  return 0;
}
