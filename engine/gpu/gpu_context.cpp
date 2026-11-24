// Headless OpenGL ES 3 context for offscreen tile rendering (see ink_gpu.h).
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <new>

#include "ink/ink_gpu.h"

struct InkGpuContext {
  EGLDisplay display = EGL_NO_DISPLAY;
  EGLContext context = EGL_NO_CONTEXT;
  EGLSurface surface = EGL_NO_SURFACE;
};

namespace {

EGLDisplay openDisplay() {
#if defined(__linux__) && !defined(__ANDROID__) && defined(EGL_PLATFORM_SURFACELESS_MESA)
  // Desktop Linux (CI, tests): no window system needed.
  auto getPlatformDisplay =
      reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
  if (getPlatformDisplay) {
    EGLDisplay d = getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    if (d != EGL_NO_DISPLAY) return d;
  }
#endif
  return eglGetDisplay(EGL_DEFAULT_DISPLAY);
}

}  // namespace

extern "C" {

InkGpuContext* ink_gpu_context_create(void) {
  EGLDisplay d = openDisplay();
  if (d == EGL_NO_DISPLAY || !eglInitialize(d, nullptr, nullptr)) return nullptr;
  eglBindAPI(EGL_OPENGL_ES_API);
#ifndef EGL_OPENGL_ES3_BIT_KHR
#define EGL_OPENGL_ES3_BIT_KHR 0x0040
#endif
  const EGLint configAttribs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                                  EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
  EGLConfig config = nullptr;
  EGLint count = 0;
  if (!eglChooseConfig(d, configAttribs, &config, 1, &count) || count < 1) return nullptr;
  const EGLint contextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
  EGLContext ctx = eglCreateContext(d, config, EGL_NO_CONTEXT, contextAttribs);
  if (ctx == EGL_NO_CONTEXT) return nullptr;
  // Tiles render into our own framebuffer objects; a 1x1 pbuffer keeps drivers
  // without EGL_KHR_surfaceless_context happy.
  const EGLint pbufferAttribs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
  EGLSurface surface = eglCreatePbufferSurface(d, config, pbufferAttribs);
  if (!eglMakeCurrent(d, surface, surface, ctx)) {
    if (surface != EGL_NO_SURFACE) eglDestroySurface(d, surface);
    eglDestroyContext(d, ctx);
    return nullptr;
  }
  InkGpuContext* c = new (std::nothrow) InkGpuContext();
  if (!c) {
    eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (surface != EGL_NO_SURFACE) eglDestroySurface(d, surface);
    eglDestroyContext(d, ctx);
    return nullptr;
  }
  c->display = d;
  c->context = ctx;
  c->surface = surface;
  return c;
}

void ink_gpu_context_destroy(InkGpuContext* c) {
  if (!c) return;
  eglMakeCurrent(c->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  if (c->surface != EGL_NO_SURFACE) eglDestroySurface(c->display, c->surface);
  eglDestroyContext(c->display, c->context);
  eglReleaseThread();
  delete c;
}

}  // extern "C"
