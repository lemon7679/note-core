// GPU renderer (engine/gpu) against the CPU reference rasterizer, on a
// headless OpenGL ES 3 context (Mesa llvmpipe on CI / Linux, any EGL device).
//
//   INK_GPU_DUMP=dir ink_gpu_tests   also writes cpu / gpu / diff PNGs
#include <GLES3/gl3.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../tools/png_writer.h"
#include "../tools/synth_input.h"
#include "ink/ink.h"
#include "ink/ink_c.h"
#include "ink/ink_gpu.h"
#include "test.h"

using namespace ink;

namespace {

struct Gpu {
  InkGpuContext* ctx = nullptr;
  InkGpu* gpu = nullptr;
  Gpu() {
    ctx = ink_gpu_context_create();
    if (ctx) gpu = ink_gpu_create();
    if (gpu) std::printf("    GPU: %s\n", ink_gpu_description(gpu));
    else std::printf("    GPU: unavailable (context %s)\n", ctx ? "ok" : "failed");
  }
};

Gpu& gpu() {
  static Gpu g;
  return g;
}

void add(PageEditor& ed, const Brush& brush, const std::vector<InputPoint>& in) {
  if (in.size() < 2) return;
  StrokeBuilder b(brush);
  b.begin(in.front());
  for (size_t i = 1; i < in.size(); ++i) b.addPoint(in[i]);
  b.end();
  ed.commitStroke(b);
}

/// Handwriting, stacked highlighters (several colors, crossing), pencil
/// writing and shading, fountain / brush pens and calligraphy.
std::vector<uint8_t> mixedPage() {
  PageEditor ed(300, 300);
  uint32_t seed = 11;
  for (int row = 0; row < 6; ++row) {
    synth::Options o;
    o.seed = seed++;
    add(ed, presets::ballpoint(), synth::synthesize(synth::cursiveLoops({20.0f, 30.0f + row * 22.0f}, 6, 12), o));
  }
  const Color hl[] = {colors::kHighlightYellow, colors::kHighlightGreen, colors::kHighlightPink};
  for (int k = 0; k < 5; ++k) {
    synth::Options o;
    o.seed = seed++;
    add(ed, presets::highlighter(hl[k % 3]),
        synth::synthesize(synth::polyline({{15.0f, 28.0f + k * 22.0f}, {280.0f, 32.0f + k * 22.0f}}), o));
  }
  {
    synth::Options o;
    o.seed = seed++;
    add(ed, presets::highlighter(hl[1]), synth::synthesize(synth::wave({20, 60}, 250, 40, 120), o));
  }
  auto line = synth::writeLine(U"한글 보정 가나다", {16, 180}, 24, 7, 0.3f);
  for (auto& ws : line) add(ed, presets::pencil(), ws.input);
  {
    std::vector<Vec2> zig;
    for (int k = 0; k < 8; ++k) {
      zig.push_back({20.0f + k * 8.0f, 220.0f});
      zig.push_back({16.0f + k * 8.0f, 285.0f});
    }
    synth::Options o;
    o.seed = seed++;
    o.altitude = 0.32f;
    o.azimuth = 0.3f;
    add(ed, presets::pencil(), synth::synthesize(synth::polyline(zig), o));
  }
  synth::Options o;
  o.seed = seed++;
  add(ed, presets::fountainPen(), synth::synthesize(synth::spiral({180, 250}, 2.5f, 4, 35), o));
  add(ed, presets::brushPen(), synth::synthesize(synth::wave({120, 215}, 160, 10, 50), o));
  add(ed, presets::calligraphy(), synth::synthesize(synth::cursiveLoops({130, 270}, 3, 20), o));
  add(ed, presets::marker(colors::kInkRed), synth::synthesize(synth::polyline({{240, 200}, {290, 290}}), o));
  return ed.save();
}

struct Diff {
  int maxDiff = 0;
  double mean = 0.0;
  size_t over8 = 0, over32 = 0, pixels = 0;
};

Diff compare(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, int w, int h) {
  Diff d;
  double sum = 0.0;
  for (int i = 0; i < w * h; ++i) {
    int m = 0;
    for (int c = 0; c < 4; ++c) m = std::max(m, std::abs(static_cast<int>(a[i * 4 + c]) - b[i * 4 + c]));
    d.maxDiff = std::max(d.maxDiff, m);
    sum += m;
    if (m > 8) ++d.over8;
    if (m > 32) ++d.over32;
  }
  d.pixels = static_cast<size_t>(w) * h;
  d.mean = sum / static_cast<double>(d.pixels);
  return d;
}

void dump(const std::string& name, const std::vector<uint8_t>& cpu, const std::vector<uint8_t>& gpuPx, int w, int h) {
  const char* dir = std::getenv("INK_GPU_DUMP");
  if (!dir) return;
  std::vector<uint8_t> diff(cpu.size());
  for (int i = 0; i < w * h; ++i) {
    int m = 0;
    for (int c = 0; c < 3; ++c) m = std::max(m, std::abs(static_cast<int>(cpu[i * 4 + c]) - gpuPx[i * 4 + c]));
    uint8_t v = static_cast<uint8_t>(std::min(255, m * 8));
    diff[i * 4 + 0] = v;
    diff[i * 4 + 1] = v;
    diff[i * 4 + 2] = v;
    diff[i * 4 + 3] = 255;
  }
  writePng(std::string(dir) + "/" + name + "_cpu.png", cpu.data(), w, h);
  writePng(std::string(dir) + "/" + name + "_gpu.png", gpuPx.data(), w, h);
  writePng(std::string(dir) + "/" + name + "_diff.png", diff.data(), w, h);
}

}  // namespace

TEST(gpu_tiles_match_cpu_reference) {
  if (!gpu().gpu) {
    std::printf("    (skipped: no OpenGL ES 3 context)\n");
    return;
  }
  std::vector<uint8_t> data = mixedPage();
  InkEditor* e = ink_editor_create(300, 300);
  REQUIRE(ink_editor_load(e, data.data(), data.size()));
  InkSnapshot* snap = ink_editor_snapshot(e);
  struct Case { float scale, ox, oy; int w, h; };
  const Case cases[] = {
      {1.0f, 0, 0, 300, 300},        // zoomed out
      {2.5f, -40, -30, 512, 512},    // a tile at a typical zoom
      {4.2f, -500, -700, 512, 384},  // pencil + pens, odd tile size
      {9.0f, -300, -250, 256, 256},  // close up on highlighters
  };
  int k = 0;
  for (const Case& c : cases) {
    // Paper-colored opaque background, like the canvas tiles.
    std::vector<uint8_t> cpu(static_cast<size_t>(c.w) * c.h * 4), gpuPx;
    for (size_t i = 0; i < cpu.size(); i += 4) {
      cpu[i] = 252;
      cpu[i + 1] = 250;
      cpu[i + 2] = 245;
      cpu[i + 3] = 255;
    }
    gpuPx = cpu;
    ink_snapshot_render_over_rgba(snap, cpu.data(), c.w, c.h, c.w * 4, c.scale, c.ox, c.oy);
    REQUIRE(ink_gpu_render_snapshot_over_rgba(gpu().gpu, snap, gpuPx.data(), c.w, c.h, c.w * 4, c.scale, c.ox, c.oy));
    Diff d = compare(cpu, gpuPx, c.w, c.h);
    std::printf("    scale %.1f: max %d, mean %.4f, >8: %zu, >32: %zu of %zu px\n", c.scale, d.maxDiff, d.mean, d.over8,
                d.over32, d.pixels);
    dump("tile" + std::to_string(k++), cpu, gpuPx, c.w, c.h);
    // Same SDFs at the same pixel centres: only float rounding and the merged
    // coverage of crossing same-color opaque strokes may differ.
    CHECK(d.maxDiff <= 4);
    CHECK(d.mean < 1.0);
  }
  ink_snapshot_release(snap);
  ink_editor_destroy(e);
}

TEST(gpu_strided_partial_render) {
  if (!gpu().gpu) return;
  std::vector<uint8_t> data = mixedPage();
  InkEditor* e = ink_editor_create(300, 300);
  REQUIRE(ink_editor_load(e, data.data(), data.size()));
  InkSnapshot* snap = ink_editor_snapshot(e);
  // A 100x80 part of a bitmap whose rows are 160 px wide (Android scratch bitmaps).
  const int w = 100, h = 80, strideW = 160;
  std::vector<uint8_t> cpu(static_cast<size_t>(strideW) * h * 4, 255), gpuPx;
  gpuPx = cpu;
  ink_snapshot_render_over_rgba(snap, cpu.data(), w, h, strideW * 4, 3.0f, -60, -90);
  REQUIRE(ink_gpu_render_snapshot_over_rgba(gpu().gpu, snap, gpuPx.data(), w, h, strideW * 4, 3.0f, -60, -90));
  int maxOutside = 0;
  std::vector<uint8_t> a, b;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < strideW; ++x)
      for (int c = 0; c < 4; ++c) {
        uint8_t va = cpu[(y * strideW + x) * 4 + c], vb = gpuPx[(y * strideW + x) * 4 + c];
        if (x >= w) maxOutside = std::max(maxOutside, std::abs(255 - static_cast<int>(vb)));
        else {
          a.push_back(va);
          b.push_back(vb);
        }
      }
  CHECK(maxOutside == 0);  // nothing written past the row width
  Diff d = compare(a, b, w, h);
  CHECK(d.maxDiff <= 4);
  ink_snapshot_release(snap);
  ink_editor_destroy(e);
}

TEST(gpu_falls_back_for_translucent_layers) {
  if (!gpu().gpu) return;
  InkEditor* e = ink_editor_create(100, 100);
  // Layer opacity is not exposed through the C API: make a page with one through serialization.
  ink::PageEditor ed(100, 100);
  ed.page().layers()[0].opacity = 0.5f;
  synth::Options o;
  add(ed, presets::ballpoint(), synth::synthesize(synth::polyline({{10, 10}, {90, 90}}), o));
  std::vector<uint8_t> data = ed.save();
  REQUIRE(ink_editor_load(e, data.data(), data.size()));
  InkSnapshot* snap = ink_editor_snapshot(e);
  std::vector<uint8_t> px(100 * 100 * 4, 255);
  CHECK(!ink_gpu_render_snapshot_over_rgba(gpu().gpu, snap, px.data(), 100, 100, 400, 1.0f, 0, 0));
  ink_snapshot_release(snap);
  ink_editor_destroy(e);
}

TEST(gpu_live_stroke_matches_cpu_overlay) {
  if (!gpu().gpu) return;
  const int w = 400, h = 240;
  GLuint tex = 0, fbo = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
  REQUIRE(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

  for (InkBrushKind kind : {INK_BRUSH_PENCIL, INK_BRUSH_FOUNTAIN_PEN, INK_BRUSH_BALLPOINT}) {
    InkBuilder* b = ink_builder_create(kind);
    synth::Options o;
    o.seed = 5;
    auto in = synth::synthesize(synth::cursiveLoops({20, 60}, 5, 30), o);
    std::vector<InkInputPoint> pts;
    for (const InputPoint& p : in) {
      InkInputPoint q{};
      q.x = p.pos.x;
      q.y = p.pos.y;
      q.pressure = p.pressure;
      q.altitude = p.altitude;
      q.azimuth = p.azimuth;
      q.time = p.time;
      pts.push_back(q);
    }
    ink_builder_begin(b, &pts[0]);
    const float scale = 2.0f, ox = 10.0f, oy = -20.0f;
    // Draw incrementally, repainting only each frame's dirty region, like a front buffer.
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, w, h);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    InkLiveGeometry live{};
    for (size_t i = 1; i < pts.size(); i += 7) {
      size_t n = std::min<size_t>(7, pts.size() - i);
      ink_builder_add_points(b, &pts[i], n);
      ink_builder_update_geometry(b, scale, &live);
      if (live.dirty.width <= 0) continue;
      InkLiveFrame* f = ink_builder_capture_live(b, &live.dirty);
      if (f) CHECK(ink_gpu_draw_live(gpu().gpu, f, w, h, w, h, nullptr, scale, ox, oy));
      ink_live_frame_release(f);
    }
    std::vector<uint8_t> incremental(w * h * 4), whole(w * h * 4), cpu(w * h * 4, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, incremental.data());
    // Whole stroke at once.
    glClear(GL_COLOR_BUFFER_BIT);
    InkLiveFrame* all = ink_builder_capture_live(b, nullptr);
    REQUIRE(all != nullptr);
    CHECK(!ink_live_frame_multiply(all));
    CHECK(ink_gpu_draw_live(gpu().gpu, all, w, h, w, h, nullptr, scale, ox, oy));
    ink_live_frame_release(all);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, whole.data());
    ink_builder_render_over_rgba(b, cpu.data(), w, h, w * 4, scale, ox, oy);
    Diff dw = compare(cpu, whole, w, h), di = compare(whole, incremental, w, h);
    std::printf("    live %d: vs cpu max %d mean %.4f | incremental vs whole max %d mean %.4f\n", kind, dw.maxDiff,
                dw.mean, di.maxDiff, di.mean);
    dump("live" + std::to_string(kind), cpu, whole, w, h);
    dump("liveinc" + std::to_string(kind), whole, incremental, w, h);
    CHECK(dw.mean < 0.25);
    CHECK(dw.over8 < dw.pixels / 2000);
    CHECK(di.maxDiff <= 2);  // dirty-region repaint == full repaint
    ink_builder_destroy(b);
  }
  glDeleteFramebuffers(1, &fbo);
  glDeleteTextures(1, &tex);
}

TEST(gpu_live_stroke_with_prerotated_framebuffer) {
  // Front buffers are pre-rotated to the display's install orientation: the
  // pixel space (w x h, y down) lands rotated by 90 degrees in an h x w buffer.
  if (!gpu().gpu) return;
  const int w = 300, h = 200;
  auto makeTarget = [](int tw, int th, GLuint& tex, GLuint& fbo) {
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glViewport(0, 0, tw, th);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
  };
  GLuint texA, fboA, texB, fboB;
  makeTarget(w, h, texA, fboA);
  makeTarget(h, w, texB, fboB);
  // pixel (x, y) -> buffer (y, w - x): column-major.
  float rot[16] = {0};
  rot[4] = 2.0f / h;
  rot[12] = -1.0f;
  rot[1] = -2.0f / w;
  rot[13] = 1.0f;
  rot[10] = 1.0f;
  rot[15] = 1.0f;

  InkBuilder* b = ink_builder_create(INK_BRUSH_PENCIL);
  synth::Options o;
  auto in = synth::synthesize(synth::wave({10, 40}, 120, 15, 50), o);
  std::vector<InkInputPoint> pts;
  for (const InputPoint& p : in) {
    InkInputPoint q{};
    q.x = p.pos.x;
    q.y = p.pos.y;
    q.pressure = p.pressure;
    q.altitude = p.altitude;
    q.time = p.time;
    pts.push_back(q);
  }
  ink_builder_begin(b, &pts[0]);
  InkLiveGeometry live{};
  for (size_t i = 1; i < pts.size(); i += 5) {
    ink_builder_add_points(b, &pts[i], std::min<size_t>(5, pts.size() - i));
    ink_builder_update_geometry(b, 2.0f, &live);
    if (live.dirty.width <= 0) continue;
    InkLiveFrame* f = ink_builder_capture_live(b, &live.dirty);
    glBindFramebuffer(GL_FRAMEBUFFER, fboA);
    CHECK(ink_gpu_draw_live(gpu().gpu, f, w, h, w, h, nullptr, 2.0f, 0, 0));
    glBindFramebuffer(GL_FRAMEBUFFER, fboB);
    CHECK(ink_gpu_draw_live(gpu().gpu, f, w, h, h, w, rot, 2.0f, 0, 0));
    ink_live_frame_release(f);
  }
  std::vector<uint8_t> a(w * h * 4), r(w * h * 4);
  glBindFramebuffer(GL_FRAMEBUFFER, fboA);
  glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, a.data());
  glBindFramebuffer(GL_FRAMEBUFFER, fboB);
  glReadPixels(0, 0, h, w, GL_RGBA, GL_UNSIGNED_BYTE, r.data());
  int maxDiff = 0;
  size_t inked = 0;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const int fx = y, fy = w - 1 - x;
      for (int c = 0; c < 4; ++c)
        maxDiff = std::max(maxDiff, std::abs(static_cast<int>(a[(y * w + x) * 4 + c]) - r[(fy * h + fx) * 4 + c]));
      if (a[(y * w + x) * 4 + 3] > 40) ++inked;
    }
  CHECK(inked > 500);
  CHECK(maxDiff <= 1);
  ink_builder_destroy(b);
  glDeleteFramebuffers(1, &fboA);
  glDeleteFramebuffers(1, &fboB);
  glDeleteTextures(1, &texA);
  glDeleteTextures(1, &texB);
}
