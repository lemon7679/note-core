// InkEngine GPU renderer (OpenGL ES 3.0). See include/ink/ink_gpu.h.
//
// Mirrors src/rasterizer.cpp: every primitive is an instanced quad whose
// fragment shader evaluates the same signed-distance function at the pixel
// centre. Per stroke (or per run of consecutive opaque strokes of one color,
// which composite identically) coverage is accumulated in an R16F texture —
// MAX for vector ink, addition of graphite density for pencil stamps — and
// then composited once with the stroke's color, opacity and blend mode.
//
// Pixel space is y-down with no flip anywhere: pixel row y is framebuffer /
// texture row y. Uploading the tile's background with glTexSubImage2D and
// reading it back with glReadPixels therefore keep memory row order, and
// interpolated pixel positions land exactly on pixel centres.
#include <GLES3/gl3.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "../src/c_api_internal.h"
#include "ink/grain.h"
#include "ink/ink_gpu.h"

#if defined(__ANDROID__)
#include <android/log.h>
#define INK_GPU_LOG(...) __android_log_print(ANDROID_LOG_WARN, "InkGpu", __VA_ARGS__)
#else
#define INK_GPU_LOG(...) std::fprintf(stderr, __VA_ARGS__)
#endif

using namespace ink;

namespace {

// ---------------------------------------------------------------------------
// Shaders
// ---------------------------------------------------------------------------

const char* kCommon = R"(#version 300 es
precision highp float;
precision highp int;
uniform mat4 u_clip;     // pixel (y down) -> clip
uniform float u_scale;   // page -> pixel
uniform vec2 u_offset;
vec2 corner(int id) { return vec2(float(id & 1), float((id >> 1) & 1)); }
)";

const char* kCapsuleVS = R"(
layout(location = 0) in vec4 i0;  // p0.xy, r0, a0
layout(location = 1) in vec4 i1;  // p1.xy, r1, a1
out vec2 v_pos;
flat out vec4 v_seg;
flat out vec4 v_ra;
void main() {
  vec2 p0 = i0.xy * u_scale + u_offset, p1 = i1.xy * u_scale + u_offset;
  float r0 = i0.z * u_scale, r1 = i1.z * u_scale, a0 = i0.w, a1 = i1.w;
  if (r0 < 0.5) { a0 *= max(r0, 0.0) / 0.5; r0 = 0.5; }
  if (r1 < 0.5) { a1 *= max(r1, 0.0) / 0.5; r1 = 0.5; }
  vec2 d = p1 - p0;
  float len = length(d);
  vec2 u = len > 1e-6 ? d / len : vec2(1.0, 0.0);
  vec2 n = vec2(-u.y, u.x);
  float rr = max(r0, r1) + 1.0;
  vec2 c = corner(gl_VertexID);
  vec2 pos = mix(p0 - u * rr, p1 + u * rr, c.x) + n * ((c.y * 2.0 - 1.0) * rr);
  v_pos = pos;
  v_seg = vec4(p0, p1);
  v_ra = vec4(r0, r1, a0, a1);
  gl_Position = u_clip * vec4(pos, 0.0, 1.0);
}
)";

const char* kCapsuleFS = R"(
in vec2 v_pos;
flat in vec4 v_seg;
flat in vec4 v_ra;
out vec4 o;
float segParam(vec2 p, vec2 a, vec2 b) {
  vec2 ab = b - a;
  float l2 = dot(ab, ab);
  return l2 <= 1e-20 ? 0.0 : clamp(dot(p - a, ab) / l2, 0.0, 1.0);
}
float unevenCapsule(vec2 p, vec2 pa, vec2 pb, float ra, float rb) {
  vec2 ba = pb - pa;
  float h = dot(ba, ba);
  float rd = ra - rb;
  if (h <= rd * rd + 1e-12) return min(length(p - pa) - ra, length(p - pb) - rb);
  vec2 q0 = p - pa;
  vec2 q = vec2(dot(q0, vec2(ba.y, -ba.x)) / h, dot(q0, ba) / h);
  q.x = abs(q.x);
  vec2 c = vec2(sqrt(h - rd * rd), rd);
  float k = c.x * q.y - c.y * q.x;
  float m = dot(c, q);
  float n = dot(q, q);
  if (k < 0.0) return sqrt(h * n) - ra;
  if (k > c.x) return sqrt(h * (n + 1.0 - 2.0 * q.y)) - rb;
  return m - ra;
}
void main() {
  float d = unevenCapsule(v_pos, v_seg.xy, v_seg.zw, v_ra.x, v_ra.y);
  if (d >= 0.5) discard;
  float v = clamp(0.5 - d, 0.0, 1.0) * mix(v_ra.z, v_ra.w, segParam(v_pos, v_seg.xy, v_seg.zw));
  o = vec4(v);
}
)";

const char* kNibVS = R"(
layout(location = 0) in vec4 i0;  // p0.xy, h0, a0
layout(location = 1) in vec4 i1;  // p1.xy, h1, a1
layout(location = 2) in vec4 i2;  // dir.xy, round, pad
out vec2 v_pos;
flat out vec4 v_seg;
flat out vec4 v_h;    // h0, h1, round
flat out vec4 v_dir;  // dir.xy, a0, a1
void main() {
  vec2 p0 = i0.xy * u_scale + u_offset, p1 = i1.xy * u_scale + u_offset;
  float h0 = i0.z * u_scale, h1 = i1.z * u_scale, r = i2.z * u_scale;
  float a0 = i0.w, a1 = i1.w;
  if (r < 0.5) { float k = max(r, 0.0) / 0.5; a0 *= k; a1 *= k; r = 0.5; }
  float dl = length(i2.xy);
  vec2 dir = dl > 1e-6 ? i2.xy / dl : vec2(1.0, 0.0);
  vec2 n = vec2(-dir.y, dir.x);
  float t0 = dot(p0, dir), t1 = dot(p1, dir);
  float s0 = dot(p0, n), s1 = dot(p1, n);
  float tmin = min(t0 - h0, t1 - h1) - r - 1.0, tmax = max(t0 + h0, t1 + h1) + r + 1.0;
  float smin = min(s0, s1) - r - 1.0, smax = max(s0, s1) + r + 1.0;
  vec2 c = corner(gl_VertexID);
  vec2 pos = dir * mix(tmin, tmax, c.x) + n * mix(smin, smax, c.y);
  v_pos = pos;
  v_seg = vec4(p0, p1);
  v_h = vec4(h0, h1, r, 0.0);
  v_dir = vec4(dir, a0, a1);
  gl_Position = u_clip * vec4(pos, 0.0, 1.0);
}
)";

const char* kNibFS = R"(
in vec2 v_pos;
flat in vec4 v_seg;
flat in vec4 v_h;
flat in vec4 v_dir;
out vec4 o;
float segParam(vec2 p, vec2 a, vec2 b) {
  vec2 ab = b - a;
  float l2 = dot(ab, ab);
  return l2 <= 1e-20 ? 0.0 : clamp(dot(p - a, ab) / l2, 0.0, 1.0);
}
float sdQuad(vec2 p, vec2 q0, vec2 q1, vec2 q2, vec2 q3) {
  vec2 v[4] = vec2[4](q0, q1, q2, q3);
  float d = dot(p - v[0], p - v[0]);
  float s = 1.0;
  int j = 3;
  for (int i = 0; i < 4; ++i) {
    vec2 e = v[j] - v[i];
    vec2 w = p - v[i];
    float ee = dot(e, e);
    float t = ee > 1e-20 ? clamp(dot(w, e) / ee, 0.0, 1.0) : 0.0;
    vec2 b = w - e * t;
    d = min(d, dot(b, b));
    bool c1 = p.y >= v[i].y, c2 = p.y < v[j].y, c3 = e.x * w.y > e.y * w.x;
    if ((c1 && c2 && c3) || (!c1 && !c2 && !c3)) s = -s;
    j = i;
  }
  return s * sqrt(d);
}
void main() {
  vec2 p0 = v_seg.xy, p1 = v_seg.zw, dir = v_dir.xy;
  float d = sdQuad(v_pos, p0 - dir * v_h.x, p1 - dir * v_h.y, p1 + dir * v_h.y, p0 + dir * v_h.x) - v_h.z;
  if (d >= 0.5) discard;
  float v = clamp(0.5 - d, 0.0, 1.0) * mix(v_dir.z, v_dir.w, segParam(v_pos, p0, p1));
  o = vec4(v);
}
)";

const char* kStampVS = R"(
layout(location = 0) in vec4 i0;  // center.xy, axis.xy
layout(location = 1) in vec4 i1;  // rMajor, rMinor, alpha, depth
uniform float u_spacing;          // StrokeStyle::spacing
uniform float u_hardness;
out vec2 v_pos;
flat out vec4 v_c;    // center, axis
flat out vec4 v_r;    // ra, rb, lambda, depth
flat out float v_edge;
float meanFalloff(float edge) {
  if (edge <= 1.0) return 1.0 - 0.5 * edge;
  float x = 1.0 / edge;
  return edge * (x * x * x - 0.5 * x * x * x * x);
}
void main() {
  vec2 c = i0.xy * u_scale + u_offset;
  float al = length(i0.zw);
  vec2 axis = al > 1e-6 ? i0.zw / al : vec2(1.0, 0.0);
  float ra = i1.x * u_scale, rb = i1.y * u_scale, k = 1.0;
  float spacing = max(2.0 * ra * max(u_spacing, 0.01), 1e-3);
  if (ra < 0.5) { k = (ra * ra) / 0.25; ra = 0.5; rb = 0.5; }
  else if (rb < 0.5) { k = rb / 0.5; rb = 0.5; }
  float a = clamp(i1.z * k, 0.0, 0.985);
  float edge = max(1.0 - clamp(u_hardness, 0.0, 1.0), 1.0 / min(ra, rb));
  float overlaps = max(1.0, 2.0 * sqrt(ra * rb) / spacing * meanFalloff(edge));
  float lambda = a > 0.0 ? -log(1.0 - a) / overlaps : 0.0;
  vec2 n = vec2(-axis.y, axis.x);
  vec2 cr = corner(gl_VertexID) * 2.0 - 1.0;
  vec2 pos = c + axis * (cr.x * (ra + 1.0)) + n * (cr.y * (rb + 1.0));
  v_pos = pos;
  v_c = vec4(c, axis);
  v_r = vec4(ra, rb, lambda, i1.w);
  v_edge = edge;
  gl_Position = u_clip * vec4(pos, 0.0, 1.0);
}
)";

const char* kStampFS = R"(
uniform sampler2D u_grain;
uniform float u_grainStrength;
uniform float u_grainScale;
in vec2 v_pos;
flat in vec4 v_c;
flat in vec4 v_r;
flat in float v_edge;
out vec4 o;
float grainDeposit(float grain, float depth, float strength) {
  depth = clamp(depth, 0.0, 1.0);
  float threshold = mix(0.72, 0.10, depth);
  float peaks = smoothstep(threshold - 0.50, threshold + 0.50, grain);
  float floorValue = 0.15 + 0.65 * depth;
  return mix(1.0, mix(floorValue, 1.0, peaks), clamp(strength, 0.0, 1.0));
}
void main() {
  vec2 d = v_pos - v_c.xy;
  vec2 n = vec2(-v_c.w, v_c.z);
  float qx = dot(d, v_c.zw) / v_r.x, qy = dot(d, n) / v_r.y;
  float q2 = qx * qx + qy * qy;
  if (q2 >= 1.0) discard;
  float w = smoothstep(0.0, 1.0, (1.0 - sqrt(q2)) / v_edge);
  if (u_grainStrength > 0.0) {
    vec2 page = (v_pos - u_offset) / u_scale;
    float g = texture(u_grain, page / max(u_grainScale, 1e-3) / 256.0).r;
    w *= grainDeposit(g, v_r.w, u_grainStrength);
  }
  o = vec4(v_r.z * w);
}
)";

const char* kCompositeVS = R"(
uniform vec4 u_rect;  // x0, y0, x1, y1 (pixels)
out vec2 v_pos;
void main() {
  vec2 pos = mix(u_rect.xy, u_rect.zw, corner(gl_VertexID));
  v_pos = pos;
  gl_Position = u_clip * vec4(pos, 0.0, 1.0);
}
)";

const char* kCompositeFS = R"(
uniform sampler2D u_cov;
uniform int u_density;  // 1: the texture holds graphite density
uniform vec4 u_color;   // straight rgb, a = opacity * color alpha
in vec2 v_pos;
out vec4 o;
void main() {
  float m = texelFetch(u_cov, ivec2(floor(v_pos)), 0).r;
  if (m <= 0.0) discard;
  if (u_density == 1) m = 1.0 - exp(-m);
  float a = min(m, 1.0) * u_color.a;
  if (a < 0.5 / 255.0) discard;
  o = vec4(u_color.rgb * a, a);
}
)";

GLuint compile(GLenum type, const char* body) {
  GLuint sh = glCreateShader(type);
  const char* src[2] = {kCommon, body};
  glShaderSource(sh, 2, src, nullptr);
  glCompileShader(sh);
  GLint ok = 0;
  glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[2048];
    glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
    INK_GPU_LOG("shader compile failed: %s\n", log);
    glDeleteShader(sh);
    return 0;
  }
  return sh;
}

GLuint link(const char* vs, const char* fs) {
  GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
  if (!v || !f) {
    if (v) glDeleteShader(v);
    if (f) glDeleteShader(f);
    return 0;
  }
  GLuint p = glCreateProgram();
  glAttachShader(p, v);
  glAttachShader(p, f);
  glLinkProgram(p);
  glDeleteShader(v);
  glDeleteShader(f);
  GLint ok = 0;
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[2048];
    glGetProgramInfoLog(p, sizeof(log), nullptr, log);
    INK_GPU_LOG("program link failed: %s\n", log);
    glDeleteProgram(p);
    return 0;
  }
  return p;
}

struct Program {
  GLuint id = 0;
  GLint clip = -1, scale = -1, offset = -1;
  void init(GLuint p) {
    id = p;
    clip = glGetUniformLocation(p, "u_clip");
    scale = glGetUniformLocation(p, "u_scale");
    offset = glGetUniformLocation(p, "u_offset");
  }
};

/// Column-major orthographic map of a w x h pixel space onto clip space, no flip
/// (pixel row y -> framebuffer row y).
void pixelOrtho(float w, float h, float m[16]) {
  std::memset(m, 0, 16 * sizeof(float));
  m[0] = 2.0f / w;
  m[5] = 2.0f / h;
  m[10] = 1.0f;
  m[12] = -1.0f;
  m[13] = -1.0f;
  m[15] = 1.0f;
}

struct PixelBox {
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  bool empty() const { return x0 >= x1 || y0 >= y1; }
  void expand(const PixelBox& o) {
    if (o.empty()) return;
    if (empty()) {
      *this = o;
      return;
    }
    x0 = std::min(x0, o.x0);
    y0 = std::min(y0, o.y0);
    x1 = std::max(x1, o.x1);
    y1 = std::max(y1, o.y1);
  }
};

PixelBox toBox(const Rect& px, int w, int h) {
  if (px.isEmpty()) return {};
  PixelBox b;
  b.x0 = std::max(0, static_cast<int>(std::floor(px.minX)));
  b.y0 = std::max(0, static_cast<int>(std::floor(px.minY)));
  b.x1 = std::min(w, static_cast<int>(std::ceil(px.maxX)));
  b.y1 = std::min(h, static_cast<int>(std::ceil(px.maxY)));
  return b;
}

/// Ink of one composite pass: one translucent stroke, or a run of opaque
/// strokes of the same color (their coverage MAXes to the same result).
struct Batch {
  StrokeStyle style;
  bool density = false;
  bool opaqueRun = false;
  StrokeGeometry geom;  // page space, culled
  PixelBox box;
  void clear() {
    geom.capsules.clear();
    geom.nibs.clear();
    geom.stamps.clear();
    box = PixelBox{};
    opaqueRun = false;
  }
  bool empty() const { return geom.empty() || box.empty(); }
};

}  // namespace

struct InkGpu {
  Program capsule, nib, stamp, composite;
  GLint stampSpacing = -1, stampHardness = -1, stampGrain = -1, stampGrainStrength = -1, stampGrainScale = -1;
  GLint compRect = -1, compCov = -1, compDensity = -1, compColor = -1;
  GLuint vaoCapsule = 0, vaoNib = 0, vaoStamp = 0, vaoEmpty = 0;
  GLuint vboCapsule = 0, vboNib = 0, vboStamp = 0;
  GLuint grainTex = 0;
  GLuint targetTex = 0, targetFbo = 0;
  int targetW = 0, targetH = 0;
  GLuint covTex = 0, covFbo = 0;
  int covW = 0, covH = 0;
  std::string description;

  bool init();
  void release();
  bool ensureTarget(int w, int h);
  bool ensureCoverage(int w, int h);
  void setView(const Program& p, const float clip[16], float scale, float ox, float oy) const;
  /// Coverage pass of `b` into covFbo, then composite into `dstFbo`.
  void drawBatch(const Batch& b, GLuint dstFbo, int dstW, int dstH, int pixW, int pixH, const float pixClip[16],
                 const float dstClip[16], float scale, float ox, float oy, const PixelBox* dstScissor);
};

namespace {

bool hasExtension(const char* name) {
  const char* ext = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
  if (!ext) return false;
  const size_t n = std::strlen(name);
  for (const char* p = ext; (p = std::strstr(p, name)) != nullptr; p += n)
    if ((p == ext || p[-1] == ' ') && (p[n] == ' ' || p[n] == '\0')) return true;
  return false;
}

void setupInstanceAttrib(GLuint index, GLsizei stride, size_t offset) {
  glEnableVertexAttribArray(index);
  glVertexAttribPointer(index, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(offset));
  glVertexAttribDivisor(index, 1);
}

}  // namespace

bool InkGpu::init() {
  const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
  if (!version) return false;
  int major = 0, minor = 0;
  glGetIntegerv(GL_MAJOR_VERSION, &major);
  glGetIntegerv(GL_MINOR_VERSION, &minor);
  if (major < 3) return false;
  const bool es32 = major > 3 || (major == 3 && minor >= 2);
  if (!es32 && !hasExtension("GL_EXT_color_buffer_half_float") && !hasExtension("GL_EXT_color_buffer_float")) {
    INK_GPU_LOG("no renderable half-float color buffers\n");
    return false;
  }
  description = std::string(reinterpret_cast<const char*>(glGetString(GL_VENDOR))) + " / " +
                reinterpret_cast<const char*>(glGetString(GL_RENDERER)) + " / " + version;

  GLuint p;
  if (!(p = link(kCapsuleVS, kCapsuleFS))) return false;
  capsule.init(p);
  if (!(p = link(kNibVS, kNibFS))) return false;
  nib.init(p);
  if (!(p = link(kStampVS, kStampFS))) return false;
  stamp.init(p);
  stampSpacing = glGetUniformLocation(p, "u_spacing");
  stampHardness = glGetUniformLocation(p, "u_hardness");
  stampGrain = glGetUniformLocation(p, "u_grain");
  stampGrainStrength = glGetUniformLocation(p, "u_grainStrength");
  stampGrainScale = glGetUniformLocation(p, "u_grainScale");
  if (!(p = link(kCompositeVS, kCompositeFS))) return false;
  composite.init(p);
  compRect = glGetUniformLocation(p, "u_rect");
  compCov = glGetUniformLocation(p, "u_cov");
  compDensity = glGetUniformLocation(p, "u_density");
  compColor = glGetUniformLocation(p, "u_color");

  GLuint vaos[4], vbos[3];
  glGenVertexArrays(4, vaos);
  glGenBuffers(3, vbos);
  vaoCapsule = vaos[0];
  vaoNib = vaos[1];
  vaoStamp = vaos[2];
  vaoEmpty = vaos[3];
  vboCapsule = vbos[0];
  vboNib = vbos[1];
  vboStamp = vbos[2];
  glBindVertexArray(vaoCapsule);
  glBindBuffer(GL_ARRAY_BUFFER, vboCapsule);
  setupInstanceAttrib(0, sizeof(CapsulePrim), 0);
  setupInstanceAttrib(1, sizeof(CapsulePrim), 16);
  glBindVertexArray(vaoNib);
  glBindBuffer(GL_ARRAY_BUFFER, vboNib);
  setupInstanceAttrib(0, sizeof(NibPrim), 0);
  setupInstanceAttrib(1, sizeof(NibPrim), 16);
  setupInstanceAttrib(2, sizeof(NibPrim), 32);
  glBindVertexArray(vaoStamp);
  glBindBuffer(GL_ARRAY_BUFFER, vboStamp);
  setupInstanceAttrib(0, sizeof(StampPrim), 0);
  setupInstanceAttrib(1, sizeof(StampPrim), 16);
  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);

  // Paper grain: R8, repeat, bilinear — the CPU samples texel centres the same way.
  const GrainTexture& grain = GrainTexture::shared();
  glGenTextures(1, &grainTex);
  glBindTexture(GL_TEXTURE_2D, grainTex);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, grain.size(), grain.size(), 0, GL_RED, GL_UNSIGNED_BYTE, grain.data());
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glBindTexture(GL_TEXTURE_2D, 0);

  // Prove the coverage format works here (some drivers list extensions they cannot blend).
  if (!ensureCoverage(64, 64)) return false;
  return glGetError() == GL_NO_ERROR;
}

void InkGpu::release() {
  GLuint progs[4] = {capsule.id, nib.id, stamp.id, composite.id};
  for (GLuint p : progs)
    if (p) glDeleteProgram(p);
  GLuint vaos[4] = {vaoCapsule, vaoNib, vaoStamp, vaoEmpty};
  glDeleteVertexArrays(4, vaos);
  GLuint vbos[3] = {vboCapsule, vboNib, vboStamp};
  glDeleteBuffers(3, vbos);
  GLuint texs[3] = {grainTex, targetTex, covTex};
  glDeleteTextures(3, texs);
  GLuint fbos[2] = {targetFbo, covFbo};
  glDeleteFramebuffers(2, fbos);
}

bool InkGpu::ensureTarget(int w, int h) {
  if (targetTex && w <= targetW && h <= targetH) return true;
  const int nw = std::max(w, targetW), nh = std::max(h, targetH);
  if (!targetTex) glGenTextures(1, &targetTex);
  glBindTexture(GL_TEXTURE_2D, targetTex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, nw, nh, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glBindTexture(GL_TEXTURE_2D, 0);
  if (!targetFbo) glGenFramebuffers(1, &targetFbo);
  glBindFramebuffer(GL_FRAMEBUFFER, targetFbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, targetTex, 0);
  const bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  targetW = nw;
  targetH = nh;
  return ok;
}

bool InkGpu::ensureCoverage(int w, int h) {
  if (covTex && w <= covW && h <= covH) return true;
  const int nw = std::max(w, covW), nh = std::max(h, covH);
  if (!covTex) glGenTextures(1, &covTex);
  glBindTexture(GL_TEXTURE_2D, covTex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_R16F, nw, nh, 0, GL_RED, GL_HALF_FLOAT, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glBindTexture(GL_TEXTURE_2D, 0);
  GLint prev = 0;
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev);
  if (!covFbo) glGenFramebuffers(1, &covFbo);
  glBindFramebuffer(GL_FRAMEBUFFER, covFbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, covTex, 0);
  const bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev));
  covW = nw;
  covH = nh;
  if (!ok) INK_GPU_LOG("R16F coverage framebuffer incomplete\n");
  return ok;
}

void InkGpu::setView(const Program& p, const float clip[16], float scale, float ox, float oy) const {
  glUseProgram(p.id);
  glUniformMatrix4fv(p.clip, 1, GL_FALSE, clip);
  if (p.scale >= 0) glUniform1f(p.scale, scale);
  if (p.offset >= 0) glUniform2f(p.offset, ox, oy);
}

void InkGpu::drawBatch(const Batch& b, GLuint dstFbo, int dstW, int dstH, int pixW, int pixH,
                       const float pixClip[16], const float dstClip[16], float scale, float ox, float oy,
                       const PixelBox* dstScissor) {
  // 1. Coverage (pixel space).
  glBindFramebuffer(GL_FRAMEBUFFER, covFbo);
  glViewport(0, 0, pixW, pixH);
  glEnable(GL_SCISSOR_TEST);
  glScissor(b.box.x0, b.box.y0, b.box.x1 - b.box.x0, b.box.y1 - b.box.y0);
  glClearColor(0, 0, 0, 0);
  glClear(GL_COLOR_BUFFER_BIT);
  glEnable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ONE);
  glBlendEquation(b.density ? GL_FUNC_ADD : GL_MAX);
  if (!b.geom.capsules.empty()) {
    setView(capsule, pixClip, scale, ox, oy);
    glBindVertexArray(vaoCapsule);
    glBindBuffer(GL_ARRAY_BUFFER, vboCapsule);
    glBufferData(GL_ARRAY_BUFFER, b.geom.capsules.size() * sizeof(CapsulePrim), b.geom.capsules.data(),
                 GL_STREAM_DRAW);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, static_cast<GLsizei>(b.geom.capsules.size()));
  }
  if (!b.geom.nibs.empty()) {
    setView(nib, pixClip, scale, ox, oy);
    glBindVertexArray(vaoNib);
    glBindBuffer(GL_ARRAY_BUFFER, vboNib);
    glBufferData(GL_ARRAY_BUFFER, b.geom.nibs.size() * sizeof(NibPrim), b.geom.nibs.data(), GL_STREAM_DRAW);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, static_cast<GLsizei>(b.geom.nibs.size()));
  }
  if (!b.geom.stamps.empty()) {
    setView(stamp, pixClip, scale, ox, oy);
    glUniform1f(stampSpacing, b.style.spacing);
    glUniform1f(stampHardness, b.style.hardness);
    glUniform1f(stampGrainStrength, b.style.grainStrength);
    glUniform1f(stampGrainScale, b.style.grainScale);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, grainTex);
    glUniform1i(stampGrain, 0);
    glBindVertexArray(vaoStamp);
    glBindBuffer(GL_ARRAY_BUFFER, vboStamp);
    glBufferData(GL_ARRAY_BUFFER, b.geom.stamps.size() * sizeof(StampPrim), b.geom.stamps.data(), GL_STREAM_DRAW);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, static_cast<GLsizei>(b.geom.stamps.size()));
  }

  // 2. Composite onto the destination.
  glBindFramebuffer(GL_FRAMEBUFFER, dstFbo);
  glViewport(0, 0, dstW, dstH);
  if (dstScissor) {
    glScissor(dstScissor->x0, dstScissor->y0, dstScissor->x1 - dstScissor->x0, dstScissor->y1 - dstScissor->y0);
  } else {
    glDisable(GL_SCISSOR_TEST);
  }
  glBlendEquation(GL_FUNC_ADD);
  if (b.style.blend == BlendMode::Multiply) {
    // W3C multiply over an opaque destination: Cs*Cd + Cd*(1 - as).
    glBlendFuncSeparate(GL_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  } else {
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  }
  setView(composite, dstClip, scale, ox, oy);
  glUniform4f(compRect, static_cast<float>(b.box.x0), static_cast<float>(b.box.y0), static_cast<float>(b.box.x1),
              static_cast<float>(b.box.y1));
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, covTex);
  glUniform1i(compCov, 0);
  glUniform1i(compDensity, b.density ? 1 : 0);
  const Color c = b.style.color;
  glUniform4f(compColor, c.r, c.g, c.b, clamp01(b.style.opacity) * clamp01(c.a));
  glBindVertexArray(vaoEmpty);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

namespace {

/// Appends the primitives of page-space `g` that touch `cull` (page units).
void appendCulled(const StrokeGeometry& g, const Rect& cull, StrokeGeometry& out) {
  for (const CapsulePrim& c : g.capsules) {
    Rect r;
    r.expand(c.p0, c.r0);
    r.expand(c.p1, c.r1);
    if (r.intersects(cull)) out.capsules.push_back(c);
  }
  for (const NibPrim& n : g.nibs) {
    Rect r;
    r.expand(n.p0, n.h0 + n.round);
    r.expand(n.p1, n.h1 + n.round);
    if (r.intersects(cull)) out.nibs.push_back(n);
  }
  for (const StampPrim& s : g.stamps) {
    Rect r;
    r.expand(s.center, s.rMajor);
    if (r.intersects(cull)) out.stamps.push_back(s);
  }
}

bool sameColor(const Color& a, const Color& b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; }

}  // namespace

extern "C" {

InkGpu* ink_gpu_create(void) {
  InkGpu* g = new (std::nothrow) InkGpu();
  if (!g) return nullptr;
  if (!g->init()) {
    g->release();
    delete g;
    return nullptr;
  }
  return g;
}

void ink_gpu_destroy(InkGpu* g) {
  if (!g) return;
  g->release();
  delete g;
}

const char* ink_gpu_description(const InkGpu* g) { return g ? g->description.c_str() : ""; }

bool ink_gpu_render_snapshot_over_rgba(InkGpu* g, const InkSnapshot* s, uint8_t* pixels, int width, int height,
                                       size_t stride, float scale, float offset_x, float offset_y) {
  if (!g || !s || !s->data || !pixels || width <= 0 || height <= 0 || scale <= 0.0f || stride % 4 != 0) return false;
  const capi::SnapshotData& snap = *s->data;
  for (const Layer& layer : snap.page.layers())
    if (layer.visible && layer.opacity > 0.0f && layer.opacity < 0.999f) return false;  // CPU handles these
  if (!g->ensureTarget(width, height) || !g->ensureCoverage(width, height)) return false;

  GLint prevFbo = 0;
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
  // Background in.
  glBindTexture(GL_TEXTURE_2D, g->targetTex);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(stride / 4));
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  glBindTexture(GL_TEXTURE_2D, 0);

  float clip[16];
  pixelOrtho(static_cast<float>(width), static_cast<float>(height), clip);
  const Affine t = Affine::translate(offset_x, offset_y) * Affine::scale(scale);
  const Rect clipPx(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
  const Rect pageClip = t.inverse().apply(clipPx).inflated(1.0f);
  // Room for the anti-aliased fringe and for hairlines grown to 0.5 px.
  const Rect cull = pageClip.inflated(2.0f / scale);
  const float bucket = capi::ppuBucket(scale);

  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  Batch batch;
  auto flush = [&] {
    if (!batch.empty())
      g->drawBatch(batch, g->targetFbo, width, height, width, height, clip, clip, scale, offset_x, offset_y, nullptr);
    batch.clear();
  };
  std::vector<size_t> visible;
  for (const Layer& layer : snap.page.layers()) {
    if (!layer.visible || layer.opacity <= 0.0f) continue;
    visible.clear();
    layer.query(pageClip, visible);
    for (int pass = 0; pass < 2; ++pass)
      for (size_t idx : visible) {
        const StrokePtr& sp = layer.at(idx);
        if (sp->style.drawBehind != (pass == 0) || snap.hidden.count(sp->id)) continue;
        std::shared_ptr<const StrokeGeometry> geom = snap.cache->get(sp, bucket);
        PixelBox box = toBox(t.apply(geom->bounds).inflated(2.0f), width, height);
        if (box.empty()) continue;
        const bool opaque = sp->isOpaqueSolid();
        const bool joins = opaque && batch.opaqueRun && sameColor(batch.style.color, sp->style.color);
        if (!joins) {
          flush();
          batch.style = sp->style;
          batch.density = sp->style.method == RenderMethod::Stamp;
          batch.opaqueRun = opaque;
        }
        appendCulled(*geom, cull, batch.geom);
        batch.box.expand(box);
      }
  }
  flush();

  // Pixels out (same row order).
  glBindFramebuffer(GL_FRAMEBUFFER, g->targetFbo);
  glPixelStorei(GL_PACK_ALIGNMENT, 4);
  glPixelStorei(GL_PACK_ROW_LENGTH, static_cast<GLint>(stride / 4));
  glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
  glPixelStorei(GL_PACK_ROW_LENGTH, 0);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_BLEND);
  glBindVertexArray(0);
  glUseProgram(0);
  glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
  return glGetError() == GL_NO_ERROR;
}

bool ink_gpu_draw_live(InkGpu* g, const InkLiveFrame* f, int pixel_width, int pixel_height, int framebuffer_width,
                       int framebuffer_height, const float* clip_from_pixel, float scale, float offset_x,
                       float offset_y) {
  if (!g || !f || pixel_width <= 0 || pixel_height <= 0 || framebuffer_width <= 0 || framebuffer_height <= 0 ||
      scale <= 0.0f)
    return false;
  if (!g->ensureCoverage(pixel_width, pixel_height)) return false;
  GLint prevFbo = 0, prevViewport[4] = {0, 0, 0, 0};
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
  glGetIntegerv(GL_VIEWPORT, prevViewport);

  float pixClip[16], dstClip[16];
  pixelOrtho(static_cast<float>(pixel_width), static_cast<float>(pixel_height), pixClip);
  if (clip_from_pixel) std::memcpy(dstClip, clip_from_pixel, sizeof(dstClip));
  else pixelOrtho(static_cast<float>(framebuffer_width), static_cast<float>(framebuffer_height), dstClip);
  const Affine t = Affine::translate(offset_x, offset_y) * Affine::scale(scale);

  // Repaint region in pixels, and in framebuffer coordinates (through the clip transform).
  PixelBox region{0, 0, pixel_width, pixel_height};
  PixelBox fbRegion{0, 0, framebuffer_width, framebuffer_height};
  const bool regional = !f->region.isEmpty();
  if (regional) {
    region = toBox(t.apply(f->region).inflated(1.0f), pixel_width, pixel_height);
    if (region.empty()) {
      glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
      return true;
    }
    Rect fb;
    const float xs[2] = {static_cast<float>(region.x0), static_cast<float>(region.x1)};
    const float ys[2] = {static_cast<float>(region.y0), static_cast<float>(region.y1)};
    for (float x : xs)
      for (float y : ys) {
        const float cx = dstClip[0] * x + dstClip[4] * y + dstClip[12];
        const float cy = dstClip[1] * x + dstClip[5] * y + dstClip[13];
        const float w = dstClip[3] * x + dstClip[7] * y + dstClip[15];
        fb.expand(Vec2{(cx / w * 0.5f + 0.5f) * framebuffer_width, (cy / w * 0.5f + 0.5f) * framebuffer_height});
      }
    // Pixel-aligned boundaries stay pixel-aligned under the (90-degree) pre-rotation:
    // round instead of growing, so exactly the cleared area is repainted.
    fbRegion.x0 = std::max(0, static_cast<int>(std::lround(fb.minX)));
    fbRegion.y0 = std::max(0, static_cast<int>(std::lround(fb.minY)));
    fbRegion.x1 = std::min(framebuffer_width, static_cast<int>(std::lround(fb.maxX)));
    fbRegion.y1 = std::min(framebuffer_height, static_cast<int>(std::lround(fb.maxY)));
    if (fbRegion.empty()) {
      glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
      return true;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
    glViewport(0, 0, framebuffer_width, framebuffer_height);
    glEnable(GL_SCISSOR_TEST);
    glScissor(fbRegion.x0, fbRegion.y0, fbRegion.x1 - fbRegion.x0, fbRegion.y1 - fbRegion.y0);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
  }

  Batch batch;
  batch.style = f->style;
  batch.density = f->style.method == RenderMethod::Stamp;
  batch.geom = f->geometry;
  PixelBox ink = toBox(t.apply(f->geometry.bounds).inflated(2.0f), pixel_width, pixel_height);
  if (regional) {
    ink.x0 = std::max(ink.x0, region.x0);
    ink.y0 = std::max(ink.y0, region.y0);
    ink.x1 = std::min(ink.x1, region.x1);
    ink.y1 = std::min(ink.y1, region.y1);
  }
  batch.box = ink;
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  if (!batch.empty())
    g->drawBatch(batch, static_cast<GLuint>(prevFbo), framebuffer_width, framebuffer_height, pixel_width,
                 pixel_height, pixClip, dstClip, scale, offset_x, offset_y, regional ? &fbRegion : nullptr);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_BLEND);
  glBindVertexArray(0);
  glUseProgram(0);
  glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
  glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
  return glGetError() == GL_NO_ERROR;
}

}  // extern "C"
