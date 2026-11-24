/*
 * InkEngine C API — stable ABI for Swift (iOS/iPadOS/macOS), Kotlin/JNI,
 * C#, Dart FFI and WebAssembly.
 *
 * Swift: add `engine/include` to the header search paths; the bundled
 * module.modulemap exposes this header as `import InkEngine`.
 *
 * Coordinates are page units (pt). Times are seconds.
 * Pointers returned in views (geometry, ids) stay valid until the next call
 * on the same object. Memory returned as `uint8_t*` / `char*` must be released
 * with ink_free().
 */
#ifndef INK_ENGINE_C_H
#define INK_ENGINE_C_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct InkEditor InkEditor;     /* one page + undo history */
typedef struct InkSnapshot InkSnapshot; /* immutable copy of a page's ink, usable from any thread */
typedef struct InkBuilder InkBuilder; /* the stroke being drawn */
typedef uint64_t InkStrokeId;

typedef enum InkBrushKind {
  INK_BRUSH_BALLPOINT = 0,
  INK_BRUSH_PENCIL = 1,
  INK_BRUSH_HIGHLIGHTER = 2,
  INK_BRUSH_FOUNTAIN_PEN = 3,
  INK_BRUSH_BRUSH_PEN = 4,
  INK_BRUSH_CALLIGRAPHY = 5,
  INK_BRUSH_MARKER = 6
} InkBrushKind;

typedef enum InkInputSource {
  INK_SOURCE_PENCIL = 0,
  INK_SOURCE_TOUCH = 1,
  INK_SOURCE_MOUSE = 2
} InkInputSource;

typedef enum InkInputFlags {
  INK_INPUT_PRESSURE_EXPECTS_UPDATE = 1 << 0,
  INK_INPUT_TILT_EXPECTS_UPDATE = 1 << 1
} InkInputFlags;

typedef enum InkStabilizerMode {
  INK_STABILIZER_NONE = 0,
  INK_STABILIZER_PULLED_STRING = 1, /* amount = string length (view points) */
  INK_STABILIZER_MOVING_AVERAGE = 2 /* amount = window (milliseconds) */
} InkStabilizerMode;

typedef enum InkEraserMode {
  INK_ERASER_STROKE = 0,
  INK_ERASER_PRECISE = 1
} InkEraserMode;

typedef enum InkShape {
  INK_SHAPE_NONE = 0,
  INK_SHAPE_LINE,
  INK_SHAPE_POLYLINE,
  INK_SHAPE_TRIANGLE,
  INK_SHAPE_RECTANGLE,
  INK_SHAPE_QUAD,
  INK_SHAPE_ELLIPSE,
  INK_SHAPE_CIRCLE,
  INK_SHAPE_ARC,
  INK_SHAPE_POLYGON /* pentagon / hexagon */
} InkShape;

typedef struct InkColor {
  float r, g, b, a; /* sRGB, straight alpha, 0..1 */
} InkColor;

typedef struct InkRect {
  float x, y, width, height; /* width <= 0 means empty */
} InkRect;

typedef struct InkInputPoint {
  float x, y;
  float pressure; /* force / maximumPossibleForce, 0..1; < 0 if unavailable */
  float altitude; /* radians, pi/2 = upright; < 0 if unavailable */
  float azimuth;  /* radians, page space */
  uint32_t flags; /* InkInputFlags */
  double time;
  uint64_t update_id; /* estimationUpdateIndex, 0 if none */
  uint32_t source;    /* InkInputSource */
  uint32_t reserved;
} InkInputPoint;

/* GPU primitive layouts (match ink::CapsulePrim / NibPrim / StampPrim). */
typedef struct InkCapsule {
  float x0, y0, r0, a0;
  float x1, y1, r1, a1;
} InkCapsule;

typedef struct InkNib {
  float x0, y0, h0, a0;
  float x1, y1, h1, a1;
  float dir_x, dir_y, round, pad;
} InkNib;

typedef struct InkStamp {
  float cx, cy, axis_x, axis_y;
  float r_major, r_minor, alpha, depth;
} InkStamp;

typedef struct InkGeometryView {
  const InkCapsule* capsules;
  size_t capsule_count;
  const InkNib* nibs;
  size_t nib_count;
  const InkStamp* stamps;
  size_t stamp_count;
  InkRect bounds;
  /* Erased areas (precise eraser): after accumulating the ink, multiply its
   * coverage (pencil: density) by (1 - coverage) of each of these capsules. */
  const InkCapsule* holes;
  size_t hole_count;
} InkGeometryView;

typedef struct InkLiveGeometry {
  InkGeometryView stable; /* grows monotonically; bake new prims once */
  InkGeometryView tail;   /* rebuilt each call; draw every frame */
  size_t new_capsules, new_nibs, new_stamps; /* first new index in `stable` */
  bool stable_reset;      /* discard baked stable coverage */
  InkRect dirty;
} InkLiveGeometry;

typedef struct InkStyleInfo {
  uint32_t kind;   /* InkBrushKind */
  uint32_t method; /* 0 vector, 1 stamp */
  uint32_t tip;    /* 0 round, 1 nib */
  uint32_t blend;  /* 0 normal, 1 multiply */
  InkColor color;
  float opacity;
  float hardness;
  float grain_strength;
  float grain_scale;
  bool draw_behind;
  float stamp_spacing; /* stamp spacing / stamp width (GPU pencil density normalization) */
} InkStyleInfo;

/* ---------------------------------------------------------------- misc */
const char* ink_version(void);
void ink_free(void* p);
/* R8 tileable paper grain for the pencil shader; *size receives the edge length. */
const uint8_t* ink_grain_texture(int* size);

/* ------------------------------------------------------------- builder */
InkBuilder* ink_builder_create(InkBrushKind kind);
void ink_builder_destroy(InkBuilder* b);

void ink_builder_set_brush(InkBuilder* b, InkBrushKind kind, InkColor color, float width);
void ink_builder_set_color(InkBuilder* b, InkColor color);
void ink_builder_set_width(InkBuilder* b, float width);
void ink_builder_set_opacity(InkBuilder* b, float opacity);
void ink_builder_set_stabilizer(InkBuilder* b, InkStabilizerMode mode, float amount);
void ink_builder_set_pressure_curve(InkBuilder* b, float c1x, float c1y, float c2x, float c2y);
void ink_builder_set_snap_on_hold(InkBuilder* b, bool enabled);
void ink_builder_set_builtin_prediction(InkBuilder* b, bool enabled);
/* Zoom: view points per page unit. */
void ink_builder_set_view_scale(InkBuilder* b, float view_points_per_unit);

/* 글씨 보정 (handwriting correction). strength 0 = off .. 1 = strong.
 * Applied when the pen lifts; with live stabilization the stroke is also
 * smoothed while writing. */
void ink_builder_set_correction(InkBuilder* b, float strength);
void ink_builder_set_correction_options(InkBuilder* b, bool live_stabilization, bool smooth_curves,
                                        bool straighten_lines, bool snap_to_axes);

/* Draw-and-hold shapes. After a shape snaps, keep moving the pen (without
 * lifting) to resize / rotate it. While `constrained` is true (e.g. a second
 * finger is down) the shape is perfect: circle, square, equilateral triangle,
 * regular polygon, 15°-stepped line. */
void ink_builder_set_shape_constraint(InkBuilder* b, bool constrained);
bool ink_builder_shape_is_regular(const InkBuilder* b);

void ink_builder_begin(InkBuilder* b, const InkInputPoint* p);
void ink_builder_add_points(InkBuilder* b, const InkInputPoint* pts, size_t count);
void ink_builder_set_predicted(InkBuilder* b, const InkInputPoint* pts, size_t count);
bool ink_builder_update_estimated(InkBuilder* b, uint64_t update_id, float pressure, float altitude, float azimuth);
/* Call every display frame while drawing. Returns true when the stroke snapped to a shape. */
bool ink_builder_tick(InkBuilder* b, double now);
void ink_builder_end(InkBuilder* b);
void ink_builder_cancel(InkBuilder* b);
bool ink_builder_is_active(const InkBuilder* b);
InkShape ink_builder_snapped_shape(const InkBuilder* b);
size_t ink_builder_point_count(const InkBuilder* b);

void ink_builder_get_style(const InkBuilder* b, InkStyleInfo* out);
/* pixels_per_unit = zoom * screen scale. */
bool ink_builder_update_geometry(InkBuilder* b, float pixels_per_unit, InkLiveGeometry* out);
/* CPU live-stroke overlay: composites the geometry of the last
 * ink_builder_update_geometry call (stable + tail, including predicted points)
 * over premultiplied RGBA8 pixels (Core Graphics premultipliedLast, RGBA
 * order). Page point (x, y) maps to pixel (x * scale + offset_x,
 * y * scale + offset_y). Use a transparent buffer for an overlay tile, or the
 * tile's background for an exact preview. No-op before the first update. */
void ink_builder_render_over_rgba(const InkBuilder* b, uint8_t* pixels, int width, int height, size_t stride,
                                  float scale, float offset_x, float offset_y);

/* -------------------------------------------------------------- editor */
InkEditor* ink_editor_create(float page_width, float page_height);
void ink_editor_destroy(InkEditor* e);

/* Adds the builder's finished stroke (after ink_builder_end). Returns 0 on failure. */
InkStrokeId ink_editor_commit(InkEditor* e, const InkBuilder* b);
size_t ink_editor_stroke_count(const InkEditor* e);

bool ink_editor_undo(InkEditor* e);
bool ink_editor_redo(InkEditor* e);
bool ink_editor_can_undo(const InkEditor* e);
bool ink_editor_can_redo(const InkEditor* e);

void ink_editor_erase_begin(InkEditor* e, InkEraserMode mode, float radius, uint32_t kind_mask, float x, float y);
bool ink_editor_erase_move(InkEditor* e, float x, float y);
void ink_editor_erase_end(InkEditor* e);

uint32_t ink_editor_add_layer(InkEditor* e, const char* name);
bool ink_editor_set_active_layer(InkEditor* e, uint32_t layer_id);
uint32_t ink_editor_active_layer(const InkEditor* e);
bool ink_editor_set_layer_visible(InkEditor* e, uint32_t layer_id, bool visible);

/* Selection. `xy` holds point_count (x, y) pairs. Returns the number of ids
 * selected; at most `capacity` are written. */
size_t ink_editor_select_lasso(InkEditor* e, const float* xy, size_t point_count, InkStrokeId* out, size_t capacity);
bool ink_editor_transform(InkEditor* e, const InkStrokeId* ids, size_t count, float a, float b, float c, float d,
                          float tx, float ty);
bool ink_editor_recolor(InkEditor* e, const InkStrokeId* ids, size_t count, InkColor color);
bool ink_editor_delete(InkEditor* e, const InkStrokeId* ids, size_t count);
/* Copies of `ids` moved by (dx, dy), as one undo step. Returns the number of new
 * strokes; at most `capacity` ids are written to `out` (may be NULL). */
size_t ink_editor_duplicate(InkEditor* e, const InkStrokeId* ids, size_t count, float dx, float dy, InkStrokeId* out,
                            size_t capacity);
/* Union of the strokes' ink bounds (page units); width 0 when none exist. */
InkRect ink_editor_bounds_of(const InkEditor* e, const InkStrokeId* ids, size_t count);
/* Clipboard: serializes the strokes (free with ink_free) and pastes them into
 * any page, moved by (dx, dy), as one undo step. */
uint8_t* ink_editor_copy_strokes(const InkEditor* e, const InkStrokeId* ids, size_t count, size_t* out_size);
size_t ink_editor_paste_strokes(InkEditor* e, const uint8_t* data, size_t size, float dx, float dy, InkStrokeId* out,
                                size_t capacity);

/* 글씨 보정 on existing strokes (each call = one undo step). */
bool ink_editor_correct(InkEditor* e, const InkStrokeId* ids, size_t count, float strength);
/* Straighten written lines: removes slant and levels drifting words. strength 0..1. */
bool ink_editor_straighten(InkEditor* e, const InkStrokeId* ids, size_t count, float strength);

/* Area (page units) that changed since the last call. */
InkRect ink_editor_take_dirty(InkEditor* e);

/* GPU rendering: ids of strokes intersecting `region` in paint order
 * (all layers, "draw behind" strokes first per layer). Returns the total count. */
size_t ink_editor_query(InkEditor* e, InkRect region, InkStrokeId* out, size_t capacity);
/* Cached tessellation of one stroke at a zoom level. */
bool ink_editor_stroke_geometry(InkEditor* e, InkStrokeId id, float pixels_per_unit, InkGeometryView* out,
                                InkStyleInfo* style);

/* CPU rendering into straight-alpha RGBA8. Page point (x, y) maps to pixel
 * (x * scale + offset_x, y * scale + offset_y). `live` may be NULL. */
void ink_editor_render_rgba(InkEditor* e, uint8_t* pixels, int width, int height, size_t stride, float scale,
                            float offset_x, float offset_y, InkColor background, const InkBuilder* live);
/* Tile rendering: composites the page's strokes over premultiplied RGBA8
 * pixels that already hold the paper / PDF background, so highlighter
 * Multiply darkens the real background. Uses the cached tessellation of
 * ink_editor_stroke_geometry, so re-rendering tiles at one zoom is cheap, and
 * touches only the pixels under ink. */
void ink_editor_render_over_rgba(InkEditor* e, uint8_t* pixels, int width, int height, size_t stride, float scale,
                                 float offset_x, float offset_y);
/* Lasso drag preview: strokes in the hidden set are skipped by
 * ink_editor_render_over_rgba (count 0 clears the set), while
 * ink_editor_render_strokes_over_rgba draws only `ids` (e.g. into a floating
 * layer that follows the finger). Commit the move with ink_editor_transform. */
void ink_editor_set_hidden(InkEditor* e, const InkStrokeId* ids, size_t count);
void ink_editor_render_strokes_over_rgba(InkEditor* e, const InkStrokeId* ids, size_t count, uint8_t* pixels,
                                         int width, int height, size_t stride, float scale, float offset_x,
                                         float offset_y);

/* Persistence. */
uint8_t* ink_editor_save(const InkEditor* e, size_t* out_size);
bool ink_editor_load(InkEditor* e, const uint8_t* data, size_t size);
/* Page size in page units (pt). */
float ink_editor_page_width(const InkEditor* e);
float ink_editor_page_height(const InkEditor* e);
char* ink_editor_export_svg(const InkEditor* e);

/* Snapshots: render and save off the editing thread.
 *
 * Editor calls must stay on one thread, but a snapshot is an immutable view of
 * the page at the moment it was taken (strokes are shared, not copied: taking
 * one is O(strokes) pointer copies, and repeated calls without edits in between
 * share the same data). Snapshots may be rendered and saved from any number of
 * threads at once while the editor keeps changing; they share the editor's
 * thread-safe tessellation cache. Typical use: a tile renderer on worker
 * threads, autosave on an I/O thread. Release every snapshot (any thread). */
InkSnapshot* ink_editor_snapshot(InkEditor* e);
void ink_snapshot_release(InkSnapshot* s);
size_t ink_snapshot_stroke_count(const InkSnapshot* s);
/* Same output as ink_editor_render_over_rgba at the time of the snapshot
 * (hidden strokes included in the snapshot are skipped). */
void ink_snapshot_render_over_rgba(const InkSnapshot* s, uint8_t* pixels, int width, int height, size_t stride,
                                   float scale, float offset_x, float offset_y);
/* Same bytes as ink_editor_save at the time of the snapshot (free with ink_free). */
uint8_t* ink_snapshot_save(const InkSnapshot* s, size_t* out_size);

#ifdef __cplusplus
}
#endif

#endif /* INK_ENGINE_C_H */
