#include "ink/ink_c.h"

#include <cstdlib>
#include <cstring>
#include <cstddef>
#include <new>
#include <unordered_map>

#include "ink/editor.h"
#include "ink/rasterizer.h"
#include "ink/serialization.h"
#include "ink/stroke_builder.h"
#include "ink/svg_export.h"

using namespace ink;

// The C structs are views onto the engine's primitive arrays: layouts must match.
static_assert(sizeof(InkCapsule) == sizeof(CapsulePrim), "InkCapsule layout");
static_assert(sizeof(InkNib) == sizeof(NibPrim), "InkNib layout");
static_assert(sizeof(InkStamp) == sizeof(StampPrim), "InkStamp layout");
static_assert(offsetof(InkCapsule, x1) == offsetof(CapsulePrim, p1), "InkCapsule layout");
static_assert(offsetof(InkNib, dir_x) == offsetof(NibPrim, dir), "InkNib layout");
static_assert(offsetof(InkStamp, r_major) == offsetof(StampPrim, rMajor), "InkStamp layout");

struct InkBuilder {
  Brush brush;
  StrokeBuilder builder;
};

struct InkEditor {
  PageEditor editor;
  struct Cached {
    StrokePtr stroke;
    float ppu = 0.0f;
    StrokeGeometry geom;
  };
  std::unordered_map<StrokeId, Cached> cache;
  std::unordered_map<StrokeId, StrokePtr> index;
  bool indexDirty = true;

  InkEditor(float w, float h) : editor(w, h) {}
  void touched() { indexDirty = true; }
  void rebuildIndex() {
    if (!indexDirty) return;
    index.clear();
    for (const Layer& l : editor.page().layers())
      for (const StrokePtr& s : l.strokes()) index[s->id] = s;
    for (auto it = cache.begin(); it != cache.end();) {
      auto f = index.find(it->first);
      if (f == index.end() || f->second != it->second.stroke) it = cache.erase(it);
      else ++it;
    }
    indexDirty = false;
  }
};

namespace {

InputPoint toInput(const InkInputPoint& p) {
  InputPoint in;
  in.pos = {p.x, p.y};
  in.pressure = p.pressure;
  in.altitude = p.altitude;
  in.azimuth = p.azimuth;
  in.time = p.time;
  in.updateId = p.update_id;
  in.flags = p.flags;
  in.source = p.source == INK_SOURCE_TOUCH ? InputSource::Touch
              : p.source == INK_SOURCE_MOUSE ? InputSource::Mouse
                                             : InputSource::Pencil;
  return in;
}

InkRect toC(const Rect& r) {
  if (r.isEmpty()) return InkRect{0, 0, 0, 0};
  return InkRect{r.minX, r.minY, r.width(), r.height()};
}

Rect fromC(const InkRect& r) {
  if (r.width <= 0.0f || r.height <= 0.0f) return Rect();
  return Rect::fromXYWH(r.x, r.y, r.width, r.height);
}

Color fromC(const InkColor& c) { return Color(c.r, c.g, c.b, c.a); }

InkGeometryView view(const StrokeGeometry& g) {
  InkGeometryView v;
  v.capsules = reinterpret_cast<const InkCapsule*>(g.capsules.data());
  v.capsule_count = g.capsules.size();
  v.nibs = reinterpret_cast<const InkNib*>(g.nibs.data());
  v.nib_count = g.nibs.size();
  v.stamps = reinterpret_cast<const InkStamp*>(g.stamps.data());
  v.stamp_count = g.stamps.size();
  v.bounds = toC(g.bounds);
  return v;
}

void fillStyle(const StrokeStyle& s, InkStyleInfo* out) {
  out->kind = static_cast<uint32_t>(s.kind);
  out->method = static_cast<uint32_t>(s.method);
  out->tip = static_cast<uint32_t>(s.tip);
  out->blend = static_cast<uint32_t>(s.blend);
  out->color = InkColor{s.color.r, s.color.g, s.color.b, s.color.a};
  out->opacity = s.opacity;
  out->hardness = s.hardness;
  out->grain_strength = s.grainStrength;
  out->grain_scale = s.grainScale;
  out->draw_behind = s.drawBehind;
}

BrushKind kindFromC(InkBrushKind k) {
  int v = static_cast<int>(k);
  if (v < 0 || v >= static_cast<int>(BrushKind::Count)) return BrushKind::Ballpoint;
  return static_cast<BrushKind>(v);
}

float ppuBucket(float ppu) {
  // Re-tessellate only when zoom crosses a power of two: geometry tessellated
  // for a finer level is reused (slightly denser than needed, never coarser).
  ppu = clamp(ppu, 0.125f, 128.0f);
  return std::exp2(std::ceil(std::log2(ppu)));
}

void applyBrush(InkBuilder* b) { b->builder.setBrush(b->brush); }

}  // namespace

extern "C" {

const char* ink_version(void) { return "0.1.0"; }

void ink_free(void* p) { std::free(p); }

const uint8_t* ink_grain_texture(int* size) {
  const GrainTexture& g = GrainTexture::shared();
  if (size) *size = g.size();
  return g.data();
}

// --------------------------------------------------------------- builder

InkBuilder* ink_builder_create(InkBrushKind kind) {
  InkBuilder* b = new (std::nothrow) InkBuilder();
  if (!b) return nullptr;
  b->brush = presets::forKind(kindFromC(kind));
  applyBrush(b);
  return b;
}

void ink_builder_destroy(InkBuilder* b) { delete b; }

void ink_builder_set_brush(InkBuilder* b, InkBrushKind kind, InkColor color, float width) {
  if (!b) return;
  b->brush = presets::forKind(kindFromC(kind));
  b->brush.style.color = fromC(color);
  if (width > 0.0f) b->brush.dynamics.width = width;
  applyBrush(b);
}

void ink_builder_set_color(InkBuilder* b, InkColor color) {
  if (!b) return;
  b->brush.style.color = fromC(color);
  applyBrush(b);
}

void ink_builder_set_width(InkBuilder* b, float width) {
  if (!b || width <= 0.0f) return;
  b->brush.dynamics.width = width;
  applyBrush(b);
}

void ink_builder_set_opacity(InkBuilder* b, float opacity) {
  if (!b) return;
  b->brush.style.opacity = clamp01(opacity);
  applyBrush(b);
}

void ink_builder_set_stabilizer(InkBuilder* b, InkStabilizerMode mode, float amount) {
  if (!b) return;
  StabilizerParams& sp = b->brush.dynamics.smoothing.stabilizer;
  sp.mode = mode == INK_STABILIZER_PULLED_STRING    ? StabilizerMode::PulledString
            : mode == INK_STABILIZER_MOVING_AVERAGE ? StabilizerMode::MovingAverage
                                                    : StabilizerMode::None;
  sp.amount = std::max(amount, 0.0f);
  applyBrush(b);
}

void ink_builder_set_pressure_curve(InkBuilder* b, float c1x, float c1y, float c2x, float c2y) {
  if (!b) return;
  b->brush.dynamics.pressureCurve = PressureCurve(Vec2{c1x, c1y}, Vec2{c2x, c2y});
  applyBrush(b);
}

void ink_builder_set_snap_on_hold(InkBuilder* b, bool enabled) {
  if (!b) return;
  b->brush.dynamics.snapOnHold = enabled;
  applyBrush(b);
}

void ink_builder_set_builtin_prediction(InkBuilder* b, bool enabled) {
  if (!b) return;
  b->brush.dynamics.builtInPrediction = enabled;
  applyBrush(b);
}

void ink_builder_set_view_scale(InkBuilder* b, float s) {
  if (b) b->builder.setViewScale(s);
}

void ink_builder_begin(InkBuilder* b, const InkInputPoint* p) {
  if (b && p) b->builder.begin(toInput(*p));
}

void ink_builder_add_points(InkBuilder* b, const InkInputPoint* pts, size_t count) {
  if (!b || !pts) return;
  for (size_t i = 0; i < count; ++i) b->builder.addPoint(toInput(pts[i]));
}

void ink_builder_set_predicted(InkBuilder* b, const InkInputPoint* pts, size_t count) {
  if (!b) return;
  std::vector<InputPoint> in;
  in.reserve(count);
  for (size_t i = 0; pts && i < count; ++i) in.push_back(toInput(pts[i]));
  b->builder.setPredictedPoints(in.data(), in.size());
}

bool ink_builder_update_estimated(InkBuilder* b, uint64_t id, float pressure, float altitude, float azimuth) {
  return b && b->builder.updateEstimatedProperties(id, pressure, altitude, azimuth);
}

bool ink_builder_tick(InkBuilder* b, double now) { return b && b->builder.tick(now); }
void ink_builder_end(InkBuilder* b) {
  if (b) b->builder.end();
}
void ink_builder_cancel(InkBuilder* b) {
  if (b) b->builder.cancel();
}
bool ink_builder_is_active(const InkBuilder* b) { return b && b->builder.active(); }
InkShape ink_builder_snapped_shape(const InkBuilder* b) {
  return b ? static_cast<InkShape>(b->builder.snappedShape()) : INK_SHAPE_NONE;
}
size_t ink_builder_point_count(const InkBuilder* b) { return b ? b->builder.points().size() : 0; }

void ink_builder_get_style(const InkBuilder* b, InkStyleInfo* out) {
  if (b && out) fillStyle(b->builder.brush().style, out);
}

bool ink_builder_update_geometry(InkBuilder* b, float pixels_per_unit, InkLiveGeometry* out) {
  if (!b || !out) return false;
  TessellationOptions opts;
  opts.pixelsPerUnit = std::max(pixels_per_unit, 1e-3f);
  const LiveGeometry& g = b->builder.updateGeometry(opts);
  out->stable = view(*g.stable);
  out->tail = view(*g.tail);
  out->new_capsules = g.newCapsules;
  out->new_nibs = g.newNibs;
  out->new_stamps = g.newStamps;
  out->stable_reset = g.stableReset;
  out->dirty = toC(g.dirty);
  return true;
}

// ---------------------------------------------------------------- editor

InkEditor* ink_editor_create(float w, float h) { return new (std::nothrow) InkEditor(w, h); }
void ink_editor_destroy(InkEditor* e) { delete e; }

InkStrokeId ink_editor_commit(InkEditor* e, const InkBuilder* b) {
  if (!e || !b || b->builder.points().empty()) return 0;
  e->touched();
  return e->editor.commitStroke(b->builder);
}

size_t ink_editor_stroke_count(const InkEditor* e) { return e ? e->editor.page().strokeCount() : 0; }

bool ink_editor_undo(InkEditor* e) {
  if (!e) return false;
  e->touched();
  return e->editor.undo();
}
bool ink_editor_redo(InkEditor* e) {
  if (!e) return false;
  e->touched();
  return e->editor.redo();
}
bool ink_editor_can_undo(const InkEditor* e) { return e && e->editor.canUndo(); }
bool ink_editor_can_redo(const InkEditor* e) { return e && e->editor.canRedo(); }

void ink_editor_erase_begin(InkEditor* e, InkEraserMode mode, float radius, uint32_t kind_mask, float x, float y) {
  if (!e) return;
  EraserOptions o;
  o.mode = mode == INK_ERASER_PRECISE ? EraserMode::Precise : EraserMode::Stroke;
  o.radius = std::max(radius, 0.1f);
  o.kindMask = kind_mask == 0 ? kAllKinds : kind_mask;
  e->touched();
  e->editor.beginErase(o, Vec2{x, y});
}

bool ink_editor_erase_move(InkEditor* e, float x, float y) {
  if (!e) return false;
  e->touched();
  return e->editor.eraseTo(Vec2{x, y});
}

void ink_editor_erase_end(InkEditor* e) {
  if (e) e->editor.endErase();
}

uint32_t ink_editor_add_layer(InkEditor* e, const char* name) {
  return e ? e->editor.addLayer(name ? name : "Layer") : 0;
}
bool ink_editor_set_active_layer(InkEditor* e, uint32_t id) { return e && e->editor.setActiveLayer(id); }
uint32_t ink_editor_active_layer(const InkEditor* e) { return e ? e->editor.activeLayer() : 0; }

bool ink_editor_set_layer_visible(InkEditor* e, uint32_t id, bool visible) {
  if (!e) return false;
  Layer* l = e->editor.page().layer(id);
  if (!l) return false;
  if (l->visible != visible) {
    l->visible = visible;
    e->editor.markDirty(l->contentBounds());
  }
  return true;
}

size_t ink_editor_select_lasso(InkEditor* e, const float* xy, size_t n, InkStrokeId* out, size_t capacity) {
  if (!e || !xy || n < 3) return 0;
  std::vector<Vec2> lasso(n);
  for (size_t i = 0; i < n; ++i) lasso[i] = {xy[2 * i], xy[2 * i + 1]};
  std::vector<StrokeId> ids = e->editor.selectLasso(lasso);
  for (size_t i = 0; out && i < ids.size() && i < capacity; ++i) out[i] = ids[i];
  return ids.size();
}

bool ink_editor_transform(InkEditor* e, const InkStrokeId* ids, size_t count, float a, float b, float c, float d,
                          float tx, float ty) {
  if (!e || !ids) return false;
  Affine t;
  t.a = a; t.b = b; t.c = c; t.d = d; t.tx = tx; t.ty = ty;
  e->touched();
  return e->editor.transform(std::vector<StrokeId>(ids, ids + count), t);
}

bool ink_editor_recolor(InkEditor* e, const InkStrokeId* ids, size_t count, InkColor color) {
  if (!e || !ids) return false;
  e->touched();
  return e->editor.recolor(std::vector<StrokeId>(ids, ids + count), fromC(color));
}

bool ink_editor_delete(InkEditor* e, const InkStrokeId* ids, size_t count) {
  if (!e || !ids) return false;
  e->touched();
  return e->editor.remove(std::vector<StrokeId>(ids, ids + count));
}

InkRect ink_editor_take_dirty(InkEditor* e) { return e ? toC(e->editor.takeDirty()) : InkRect{0, 0, 0, 0}; }

size_t ink_editor_query(InkEditor* e, InkRect region, InkStrokeId* out, size_t capacity) {
  if (!e) return 0;
  Rect r = fromC(region);
  size_t total = 0;
  std::vector<size_t> idx;
  for (const Layer& layer : e->editor.page().layers()) {
    if (!layer.visible) continue;
    layer.query(r, idx);
    for (int pass = 0; pass < 2; ++pass)
      for (size_t i : idx) {
        const Stroke& s = *layer.at(i);
        if (s.style.drawBehind != (pass == 0)) continue;
        if (out && total < capacity) out[total] = s.id;
        ++total;
      }
  }
  return total;
}

bool ink_editor_stroke_geometry(InkEditor* e, InkStrokeId id, float ppu, InkGeometryView* out, InkStyleInfo* style) {
  if (!e || !out) return false;
  e->rebuildIndex();
  auto it = e->index.find(id);
  if (it == e->index.end()) return false;
  const float bucket = ppuBucket(ppu);
  InkEditor::Cached& c = e->cache[id];
  if (c.stroke != it->second || c.ppu != bucket) {
    c.stroke = it->second;
    c.ppu = bucket;
    c.geom.clear();
    TessellationOptions opts;
    opts.pixelsPerUnit = bucket;
    Tessellator::tessellate(*c.stroke, opts, c.geom);
  }
  *out = view(c.geom);
  if (style) fillStyle(c.stroke->style, style);
  return true;
}

void ink_editor_render_rgba(InkEditor* e, uint8_t* pixels, int width, int height, size_t stride, float scale,
                            float offset_x, float offset_y, InkColor background, const InkBuilder* live) {
  if (!e || !pixels || width <= 0 || height <= 0) return;
  Bitmap bmp(width, height);
  Affine t = Affine::translate(offset_x, offset_y) * Affine::scale(scale);
  RenderOptions opts;
  opts.background = fromC(background);
  Rasterizer::renderPage(e->editor.page(), bmp, t, opts);
  if (live && live->builder.active()) {
    Stroke s = live->builder.buildStroke();
    s.points.insert(s.points.end(), live->builder.predicted().begin(), live->builder.predicted().end());
    Rasterizer::renderStroke(s, bmp, t, opts);
  }
  bmp.toRGBA8(pixels, stride);
}

uint8_t* ink_editor_save(const InkEditor* e, size_t* out_size) {
  if (!e) return nullptr;
  std::vector<uint8_t> data = e->editor.save();
  uint8_t* p = static_cast<uint8_t*>(std::malloc(data.size()));
  if (!p) return nullptr;
  std::memcpy(p, data.data(), data.size());
  if (out_size) *out_size = data.size();
  return p;
}

bool ink_editor_load(InkEditor* e, const uint8_t* data, size_t size) {
  if (!e) return false;
  e->touched();
  e->cache.clear();
  return e->editor.load(data, size);
}

char* ink_editor_export_svg(const InkEditor* e) {
  if (!e) return nullptr;
  std::string svg = exportSvg(e->editor.page());
  char* p = static_cast<char*>(std::malloc(svg.size() + 1));
  if (!p) return nullptr;
  std::memcpy(p, svg.c_str(), svg.size() + 1);
  return p;
}

}  // extern "C"
