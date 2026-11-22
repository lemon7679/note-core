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

typedef struct InkEditor InkEditor;   /* one page + undo history */
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
  INK_SHAPE_CIRCLE
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

/* Persistence. */
uint8_t* ink_editor_save(const InkEditor* e, size_t* out_size);
bool ink_editor_load(InkEditor* e, const uint8_t* data, size_t size);
char* ink_editor_export_svg(const InkEditor* e);

#ifdef __cplusplus
}
#endif

#endif /* INK_ENGINE_C_H */
