/* InkEngine GPU renderer (OpenGL ES 3.0) — optional module, built with
 * INK_BUILD_GPU (engine/gpu). Same pixels as the CPU rasterizer within a few
 * 8-bit levels: the shaders evaluate the same signed-distance functions at
 * pixel centres, accumulate a stroke's coverage with MAX (vector ink) or its
 * graphite density by addition (pencil) in an R16F texture, and composite the
 * stroke once with its color, opacity and blend mode.
 *
 * Threading: every call needs a current OpenGL ES 3.0 context and must stay on
 * that context's thread. An InkGpu belongs to the context it was created on.
 * Snapshots (ink_editor_snapshot) and live frames are immutable, so any number
 * of threads, each with its own context and InkGpu, may render at once. */
#ifndef INK_ENGINE_GPU_H
#define INK_ENGINE_GPU_H

#include "ink/ink_c.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct InkGpu InkGpu;
typedef struct InkGpuContext InkGpuContext;
typedef struct InkLiveFrame InkLiveFrame;

/* Headless EGL context (OpenGL ES 3) made current on the calling thread, for
 * offscreen tile rendering on worker threads. NULL if unavailable. Destroy it
 * on the same thread. */
InkGpuContext* ink_gpu_context_create(void);
void ink_gpu_context_destroy(InkGpuContext* c);

/* NULL when the current context cannot run the renderer (no ES 3.0, or no
 * renderable / blendable half-float color buffers for pencil density). */
InkGpu* ink_gpu_create(void);
void ink_gpu_destroy(InkGpu* g);
/* "vendor / renderer / version" of the context, for logs. */
const char* ink_gpu_description(const InkGpu* g);

/* GPU version of ink_snapshot_render_over_rgba: composites the snapshot's ink
 * over premultiplied RGBA8 pixels that hold an opaque background (paper / PDF),
 * then writes them back. Returns false (pixels untouched) when the snapshot
 * needs a feature the GPU path does not cover (translucent layers): fall back
 * to the CPU renderer. */
bool ink_gpu_render_snapshot_over_rgba(InkGpu* g, const InkSnapshot* s, uint8_t* pixels, int width, int height,
                                       size_t stride, float scale, float offset_x, float offset_y);

/* Live stroke. Capture on the builder's thread right after
 * ink_builder_update_geometry (NULL when nothing is drawn), draw it on the GPU
 * thread, release it on any thread.
 *
 * `region` (page units, may be NULL): keep only the primitives touching it and
 * limit drawing to it — pass InkLiveGeometry.dirty so a front buffer only
 * repaints what changed (no full clear while the display scans it out).
 * NULL = the whole stroke. */
InkLiveFrame* ink_builder_capture_live(const InkBuilder* b, const InkRect* region);
void ink_live_frame_release(InkLiveFrame* f);
/* True when the stroke composites with Multiply (needs the real background:
 * draw it into the canvas, not on a transparent overlay). */
bool ink_live_frame_multiply(const InkLiveFrame* f);

/* Draws the live stroke into the currently bound framebuffer (premultiplied,
 * normal blending), over what is there. Page point p maps to pixel
 * p * scale + offset in a pixel_width x pixel_height space (y down);
 * `clip_from_pixel` (column-major 4x4) maps those pixels to clip space and may
 * include a front buffer's pre-rotation; NULL = the plain mapping onto a
 * framebuffer of the same size. When the frame has a region, that area is
 * cleared to transparent first and nothing outside it is touched. Restores
 * the framebuffer binding and viewport. */
bool ink_gpu_draw_live(InkGpu* g, const InkLiveFrame* f, int pixel_width, int pixel_height, int framebuffer_width,
                       int framebuffer_height, const float* clip_from_pixel, float scale, float offset_x,
                       float offset_y);

#ifdef __cplusplus
}
#endif

#endif /* INK_ENGINE_GPU_H */
