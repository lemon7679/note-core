// Renders sample sheets for every pen with the reference rasterizer.
//   ink_demo [output_dir]
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>

#include "ink/ink.h"
#include "png_writer.h"
#include "synth_input.h"

using namespace ink;

namespace {

std::string g_out = "ink_demo_out";

Stroke draw(const Brush& brush, const std::vector<InputPoint>& in, float holdSeconds = 0.0f) {
  StrokeBuilder b(brush);
  b.begin(in.front());
  for (size_t i = 1; i < in.size(); ++i) b.addPoint(in[i]);
  if (holdSeconds > 0.0f) {
    // Pen rests at the end: keep sending stationary samples, tick the clock.
    InputPoint p = in.back();
    for (float t = 0.0f; t < holdSeconds; t += 1.0f / 240.0f) {
      p.time += 1.0 / 240.0;
      b.addPoint(p);
      b.tick(p.time);
    }
  }
  b.end();
  return b.buildStroke();
}

void add(PageEditor& ed, Stroke s) { ed.commitStroke(std::move(s)); }

void save(const Page& page, float scale, const std::string& name, Color bg = colors::kWhite) {
  int w = static_cast<int>(page.width * scale), h = static_cast<int>(page.height * scale);
  Bitmap bmp(w, h);
  RenderOptions opts;
  opts.background = bg;
  auto t0 = std::chrono::steady_clock::now();
  Rasterizer::renderPage(page, bmp, Affine::scale(scale), opts);
  auto t1 = std::chrono::steady_clock::now();
  std::vector<uint8_t> px = bmp.toRGBA8();
  std::string path = g_out + "/" + name;
  writePng(path, px.data(), w, h);
  std::printf("  %-22s %4dx%-4d  %zu strokes  render %.1f ms\n", name.c_str(), w, h, page.strokeCount(),
              std::chrono::duration<double, std::milli>(t1 - t0).count());
}

synth::Options opt(uint32_t seed, float pressure = 0.32f) {
  synth::Options o;
  o.seed = seed;
  o.pressure = pressure;
  return o;
}

/// A row of test strokes for one pen: cursive loops, a pressure wave and
/// Korean jamo (sharp corners + a circle).
void penRow(PageEditor& ed, const Brush& brush, float y, uint32_t seed) {
  synth::Options o = opt(seed);
  add(ed, draw(brush, synth::synthesize(synth::cursiveLoops({20, y}, 6, 34), o)));
  synth::Options w = opt(seed + 1, 0.30f);
  w.pressureWave = 0.22f;
  w.pressureWavelength = 70.0f;
  add(ed, draw(brush, synth::synthesize(synth::wave({215, y - 8}, 190, 12, 95), w)));
  // ㄱ
  add(ed, draw(brush, synth::synthesize(synth::polyline({{430, y - 30}, {462, y - 30}, {460, y + 8}}), opt(seed + 2))));
  // ㄴ
  add(ed, draw(brush, synth::synthesize(synth::polyline({{480, y - 32}, {481, y + 6}, {516, y + 6}}), opt(seed + 3))));
  // ㅁ (two strokes like real handwriting)
  add(ed, draw(brush, synth::synthesize(synth::polyline({{534, y - 30}, {535, y + 7}}), opt(seed + 4))));
  add(ed, draw(brush, synth::synthesize(synth::polyline({{535, y - 31}, {568, y - 31}, {567, y + 6}, {536, y + 6}}),
                                         opt(seed + 5))));
  // ㅇ
  add(ed, draw(brush, synth::synthesize(synth::ellipse({602, y - 12}, 17, 18, 0.03f, seed), opt(seed + 6))));
  // Quick flick strokes (taper behaviour) and dots.
  synth::Options fast = opt(seed + 7);
  fast.speed = 700.0f;
  add(ed, draw(brush, synth::synthesize(synth::polyline({{640, y + 6}, {690, y - 30}}), fast)));
  add(ed, draw(brush, synth::synthesize(synth::polyline({{700, y - 30}, {735, y + 8}}), opt(seed + 8, 0.45f))));
  {
    // A tap: ~60 ms in place, force rising and falling (like a real dot on an "i").
    std::vector<InputPoint> tap;
    for (int k = 0; k < 15; ++k) {
      InputPoint p;
      p.pos = {752.0f + 0.03f * k, y - 2};
      p.pressure = 0.38f * std::sin(ink::kPi * (k + 0.5f) / 15.0f);
      p.altitude = 1.1f;
      p.time = k / 240.0;
      tap.push_back(p);
    }
    add(ed, draw(brush, tap));
  }
  add(ed, draw(brush, synth::synthesize(synth::spiral({790, y - 12}, 2.6f, 2, 18), opt(seed + 10))));
}

void sheetPens() {
  PageEditor ed(840, 720);
  const Brush brushes[] = {presets::ballpoint(), presets::fountainPen(), presets::pencil(), presets::brushPen(),
                           presets::calligraphy(), presets::marker()};
  float y = 60;
  uint32_t seed = 10;
  for (const Brush& b : brushes) {
    penRow(ed, b, y, seed);
    y += 95;
    seed += 20;
  }
  // Highlighter row: ballpoint "text" with highlights over it (one crosses itself).
  Brush pen = presets::ballpoint();
  for (int line = 0; line < 2; ++line) {
    float ly = y - 8 + line * 30;
    add(ed, draw(pen, synth::synthesize(synth::scribble({24, ly}, 360, 12, 77 + line), opt(500 + line))));
  }
  add(ed, draw(presets::highlighter(), synth::synthesize(synth::polyline({{18, y - 12}, {300, y - 13}}), opt(601))));
  add(ed, draw(presets::highlighter(colors::kHighlightGreen),
               synth::synthesize(synth::polyline({{40, y + 18}, {390, y + 17}, {380, y + 22}, {60, y + 25}}), opt(602))));
  add(ed, draw(presets::highlighter(colors::kHighlightPink),
               synth::synthesize(synth::ellipse({560, y + 4}, 120, 22, 0.02f, 3), opt(603))));
  add(ed, draw(presets::highlighter(), synth::synthesize(synth::wave({430, y + 4}, 260, 14, 120), opt(604))));
  save(ed.page(), 2.0f, "pens.png");
}

void sheetPencil() {
  PageEditor ed(420, 260);
  Brush pencil = presets::pencil();
  // Pressure ramp: light -> heavy.
  {
    synth::Options o = opt(1);
    o.pressureWave = 0.0f;
    auto in = synth::synthesize(synth::polyline({{20, 40}, {400, 40}}), o);
    for (size_t i = 0; i < in.size(); ++i) in[i].pressure = 0.04f + 0.8f * static_cast<float>(i) / in.size();
    add(ed, draw(pencil, in));
  }
  // Handwriting-ish loops at normal pressure.
  add(ed, draw(pencil, synth::synthesize(synth::cursiveLoops({20, 100}, 7, 40), opt(2, 0.30f))));
  // Tilted shading (side of the lead): zig-zag hatch with a low altitude.
  {
    std::vector<Vec2> zig;
    for (int k = 0; k < 14; ++k) {
      zig.push_back({250.0f + k * 10.0f, 135.0f});
      zig.push_back({245.0f + k * 10.0f, 215.0f});
    }
    synth::Options o = opt(3, 0.35f);
    o.altitude = 0.32f;
    o.azimuth = 0.3f;
    add(ed, draw(pencil, synth::synthesize(synth::polyline(zig), o)));
  }
  // Cross-hatching: separate strokes build up darkness; grain stays aligned.
  for (int k = 0; k < 9; ++k) {
    add(ed, draw(pencil, synth::synthesize(synth::polyline({{20.0f + k * 9, 150}, {70.0f + k * 9, 230}}), opt(10 + k, 0.28f))));
    add(ed, draw(pencil, synth::synthesize(synth::polyline({{150.0f - k * 9, 150}, {100.0f - k * 9, 230}}), opt(30 + k, 0.28f))));
  }
  save(ed.page(), 4.0f, "pencil.png");
}

void sheetEraser() {
  PageEditor ed(600, 200);
  auto fill = [&](float x0) {
    add(ed, draw(presets::ballpoint(), synth::synthesize(synth::cursiveLoops({x0, 60}, 4, 34), opt(1))));
    add(ed, draw(presets::fountainPen(), synth::synthesize(synth::wave({x0, 110}, 170, 10, 60), opt(2))));
    add(ed, draw(presets::highlighter(), synth::synthesize(synth::polyline({{x0, 150}, {x0 + 170, 150}}), opt(3))));
    add(ed, draw(presets::pencil(), synth::synthesize(synth::spiral({x0 + 85, 160}, 2.0f, 3, 22), opt(4))));
  };
  fill(10);
  fill(210);
  fill(410);
  // Precise eraser through the middle panel, stroke eraser through the right one.
  EraserOptions precise;
  precise.mode = EraserMode::Precise;
  precise.radius = 5.0f;
  ed.beginErase(precise, {290, 20});
  for (float y = 20; y <= 190; y += 3) ed.eraseTo({290.0f + 25.0f * std::sin(y * 0.05f), y});
  ed.endErase();
  EraserOptions whole;
  whole.radius = 4.0f;
  ed.beginErase(whole, {470, 40});
  ed.eraseTo({470, 70});
  ed.endErase();
  save(ed.page(), 2.0f, "eraser.png");
  std::printf("    eraser: %zu strokes after erase, undo -> ", ed.page().strokeCount());
  ed.undo();
  ed.undo();
  std::printf("%zu strokes\n", ed.page().strokeCount());
}

void sheetShapes() {
  PageEditor ed(640, 420);
  Brush pen = presets::ballpoint(colors::kInkBlue, mmToPt(0.6f));
  std::vector<std::vector<Vec2>> shapes = {
      synth::ellipse({70, 75}, 48, 46, 0.05f, 1),
      synth::ellipse({200, 75}, 70, 35, 0.04f, 2, -1.6f, 6.5f),
      synth::polyline({{300, 30}, {404, 34}, {402, 118}, {301, 115}, {301, 33}}),
      synth::polyline({{480, 120}, {530, 30}, {585, 118}, {478, 121}}),
      synth::polyline({{20, 160}, {330, 166}}),
      synth::polyline({{360, 150}, {450, 190}, {600, 150}}),
  };
  uint32_t seed = 40;
  for (const auto& p : shapes) {
    synth::Options o = opt(seed++);
    o.jitter = 0.35f;
    o.tremor = 1.2f;
    auto in = synth::synthesize(p, o);
    add(ed, draw(pen, in));  // as drawn
    for (InputPoint& q : in) q.pos.y += 200;
    StrokeBuilder b(pen);
    b.begin(in.front());
    for (size_t i = 1; i < in.size(); ++i) b.addPoint(in[i]);
    InputPoint rest = in.back();
    for (int k = 0; k < 160; ++k) {
      rest.time += 1.0 / 240.0;
      b.addPoint(rest);
      b.tick(rest.time);
    }
    b.end();
    std::printf("    shape -> %s\n", shapeTypeName(b.snappedShape()));
    add(ed, b.buildStroke());
  }
  save(ed.page(), 2.0f, "shapes.png");
}

void sheetZoom() {
  // Fountain pen + ballpoint rendered at 8x: smoothness and anti-aliasing.
  PageEditor ed(110, 60);
  add(ed, draw(presets::fountainPen(), synth::synthesize(synth::cursiveLoops({6, 30}, 3, 24), opt(5))));
  add(ed, draw(presets::ballpoint(colors::kInkRed), synth::synthesize(synth::spiral({85, 30}, 2.2f, 2, 18), opt(6))));
  save(ed.page(), 8.0f, "zoom.png");
}

void sheetLive() {
  // Incremental live geometry must match the final geometry of a stroke that
  // needed no end processing (ballpoint has no tapers).
  Brush b = presets::ballpoint();
  auto in = synth::synthesize(synth::cursiveLoops({10, 40}, 5, 30), opt(9));
  StrokeBuilder sb(b);
  sb.begin(in.front());
  TessellationOptions t;
  t.pixelsPerUnit = 2.0f;
  size_t frames = 0;
  for (size_t i = 1; i < in.size(); ++i) {
    sb.addPoint(in[i]);
    if (i % 4 == 0) {
      sb.updateGeometry(t);
      ++frames;
    }
  }
  const LiveGeometry& g = sb.updateGeometry(t);
  std::printf("    live: %zu frames, stable %zu prims, tail %zu prims\n", frames, g.stable->primitiveCount(),
              g.tail->primitiveCount());
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1) g_out = argv[1];
  std::error_code ec;
  std::filesystem::create_directories(g_out, ec);
  std::printf("InkEngine demo -> %s/\n", g_out.c_str());
  sheetPens();
  sheetPencil();
  sheetEraser();
  sheetShapes();
  sheetZoom();
  sheetLive();

  // Serialization + SVG of the pens sheet content.
  PageEditor ed(840, 140);
  penRow(ed, presets::fountainPen(), 60, 1);
  std::vector<uint8_t> bin = ed.save();
  size_t points = 0;
  for (const StrokePtr& s : ed.page().layers()[0].strokes()) points += s->points.size();
  std::printf("  serialized %zu strokes / %zu points -> %zu bytes (%.1f B/pt)\n", ed.page().strokeCount(), points,
              bin.size(), static_cast<double>(bin.size()) / static_cast<double>(points));
  std::string svg = exportSvg(ed.page());
  FILE* f = std::fopen((g_out + "/fountain.svg").c_str(), "wb");
  if (f) {
    std::fwrite(svg.data(), 1, svg.size(), f);
    std::fclose(f);
  }
  return 0;
}
