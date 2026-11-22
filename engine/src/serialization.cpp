#include "ink/serialization.h"

#include <cstring>
#include <memory>

namespace ink {

namespace {

constexpr uint8_t kMagic[4] = {'I', 'N', 'K', 'P'};
constexpr float kPosQuant = 100.0f;  // 0.01 page unit
constexpr float kTimeQuant = 1000.0f;  // ms

class Writer {
 public:
  std::vector<uint8_t> buf;

  void u8(uint8_t v) { buf.push_back(v); }
  void u32(uint32_t v) {
    for (int i = 0; i < 4; ++i) buf.push_back(static_cast<uint8_t>(v >> (8 * i)));
  }
  void f32(float v) {
    uint32_t u;
    std::memcpy(&u, &v, 4);
    u32(u);
  }
  void varint(uint64_t v) {
    while (v >= 0x80) {
      buf.push_back(static_cast<uint8_t>(v | 0x80));
      v >>= 7;
    }
    buf.push_back(static_cast<uint8_t>(v));
  }
  void svarint(int64_t v) { varint((static_cast<uint64_t>(v) << 1) ^ static_cast<uint64_t>(v >> 63)); }
  void str(const std::string& s) {
    varint(s.size());
    buf.insert(buf.end(), s.begin(), s.end());
  }
  void bytes(const std::vector<uint8_t>& b) {
    varint(b.size());
    buf.insert(buf.end(), b.begin(), b.end());
  }
};

class Reader {
 public:
  Reader(const uint8_t* d, size_t n) : data_(d), size_(n) {}
  bool ok() const { return ok_; }
  size_t remaining() const { return ok_ ? size_ - pos_ : 0; }

  uint8_t u8() {
    if (!need(1)) return 0;
    return data_[pos_++];
  }
  uint32_t u32() {
    if (!need(4)) return 0;
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(data_[pos_++]) << (8 * i);
    return v;
  }
  float f32() {
    uint32_t u = u32();
    float v;
    std::memcpy(&v, &u, 4);
    return v;
  }
  uint64_t varint() {
    uint64_t v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
      if (!need(1)) return 0;
      uint8_t b = data_[pos_++];
      v |= static_cast<uint64_t>(b & 0x7F) << shift;
      if (!(b & 0x80)) return v;
    }
    ok_ = false;
    return 0;
  }
  int64_t svarint() {
    uint64_t u = varint();
    return static_cast<int64_t>(u >> 1) ^ -static_cast<int64_t>(u & 1);
  }
  std::string str() {
    uint64_t n = varint();
    if (!need(n)) return {};
    std::string s(reinterpret_cast<const char*>(data_ + pos_), static_cast<size_t>(n));
    pos_ += static_cast<size_t>(n);
    return s;
  }
  /// Sub-reader over a length-prefixed block; the parent skips the whole block.
  Reader block() {
    uint64_t n = varint();
    if (!need(n)) return Reader(nullptr, 0);
    Reader r(data_ + pos_, static_cast<size_t>(n));
    pos_ += static_cast<size_t>(n);
    return r;
  }
  void fail() { ok_ = false; }

 private:
  bool need(uint64_t n) {
    if (!ok_ || n > size_ - pos_) {
      ok_ = false;
      return false;
    }
    return true;
  }
  const uint8_t* data_;
  size_t size_;
  size_t pos_ = 0;
  bool ok_ = true;
};

inline uint8_t unitToByte(float v) { return static_cast<uint8_t>(std::lround(clamp01(v) * 255.0f)); }
inline float byteToUnit(uint8_t b) { return static_cast<float>(b) / 255.0f; }
inline uint8_t angleToByte(float a) {
  float t = (wrapAngle(a) + kPi) / kTwoPi;  // [0, 1)
  return static_cast<uint8_t>(static_cast<int>(std::lround(t * 256.0f)) & 0xFF);
}
inline float byteToAngle(uint8_t b) { return wrapAngle(static_cast<float>(b) / 256.0f * kTwoPi - kPi); }
inline int64_t quant(float v, float q) { return static_cast<int64_t>(std::llround(static_cast<double>(v) * q)); }

void writeStyle(Writer& w, const StrokeStyle& s) {
  Writer b;
  b.u8(static_cast<uint8_t>(s.kind));
  b.u8(static_cast<uint8_t>(s.method));
  b.u8(static_cast<uint8_t>(s.tip));
  b.u8(static_cast<uint8_t>(s.blend));
  b.f32(s.color.r); b.f32(s.color.g); b.f32(s.color.b); b.f32(s.color.a);
  b.f32(s.opacity);
  b.f32(s.nibAngle); b.f32(s.nibThickness);
  b.f32(s.hardness); b.f32(s.spacing); b.f32(s.grainStrength); b.f32(s.grainScale);
  b.f32(s.tiltAspect); b.f32(s.positionJitter); b.f32(s.opacityJitter);
  b.u8(s.drawBehind ? 1 : 0);
  w.bytes(b.buf);
}

bool readStyle(Reader& r, StrokeStyle& s) {
  Reader b = r.block();
  if (!r.ok()) return false;
  uint8_t kind = b.u8(), method = b.u8(), tip = b.u8(), blend = b.u8();
  if (kind >= static_cast<uint8_t>(BrushKind::Count)) kind = 0;
  s.kind = static_cast<BrushKind>(kind);
  s.method = method == 1 ? RenderMethod::Stamp : RenderMethod::Vector;
  s.tip = tip == 1 ? TipShape::Nib : TipShape::Round;
  s.blend = blend == 1 ? BlendMode::Multiply : BlendMode::Normal;
  s.color.r = b.f32(); s.color.g = b.f32(); s.color.b = b.f32(); s.color.a = b.f32();
  s.opacity = b.f32();
  s.nibAngle = b.f32(); s.nibThickness = b.f32();
  s.hardness = b.f32(); s.spacing = b.f32(); s.grainStrength = b.f32(); s.grainScale = b.f32();
  s.tiltAspect = b.f32(); s.positionJitter = b.f32(); s.opacityJitter = b.f32();
  s.drawBehind = b.u8() != 0;
  // Any trailing bytes belong to newer versions and are ignored.
  return b.ok();
}

void writeStroke(Writer& w, const Stroke& s) {
  w.varint(s.id);
  writeStyle(w, s.style);
  w.u32(s.seed);
  w.varint(s.points.size());
  int64_t px = 0, py = 0, pw = 0, pt = 0;
  for (const StrokePoint& p : s.points) {
    int64_t x = quant(p.pos.x, kPosQuant), y = quant(p.pos.y, kPosQuant);
    int64_t wd = quant(p.width, kPosQuant), t = quant(p.time, kTimeQuant);
    w.svarint(x - px); w.svarint(y - py); w.svarint(wd - pw); w.svarint(t - pt);
    px = x; py = y; pw = wd; pt = t;
    w.u8(unitToByte(p.alpha));
    w.u8(unitToByte(p.pressure));
    w.u8(unitToByte(p.tilt));
    w.u8(angleToByte(p.azimuth));
    w.varint(p.flags);
  }
}

bool readStroke(Reader& r, Stroke& s) {
  s.id = r.varint();
  if (!readStyle(r, s.style)) return false;
  s.seed = r.u32();
  uint64_t n = r.varint();
  // Each point needs at least 9 bytes: reject impossible counts before allocating.
  if (!r.ok() || n > r.remaining() / 9 + 1) return false;
  s.points.resize(static_cast<size_t>(n));
  int64_t px = 0, py = 0, pw = 0, pt = 0;
  for (StrokePoint& p : s.points) {
    px += r.svarint(); py += r.svarint(); pw += r.svarint(); pt += r.svarint();
    p.pos = {static_cast<float>(px) / kPosQuant, static_cast<float>(py) / kPosQuant};
    p.width = static_cast<float>(pw) / kPosQuant;
    p.time = static_cast<float>(pt) / kTimeQuant;
    p.alpha = byteToUnit(r.u8());
    p.pressure = byteToUnit(r.u8());
    p.tilt = byteToUnit(r.u8());
    p.azimuth = byteToAngle(r.u8());
    p.flags = static_cast<uint32_t>(r.varint());
    if (!r.ok()) return false;
  }
  s.updateBounds();
  return r.ok();
}

}  // namespace

std::vector<uint8_t> serializePage(const Page& page) {
  Writer w;
  for (uint8_t m : kMagic) w.u8(m);
  w.u32(kPageFormatVersion);
  w.f32(page.width);
  w.f32(page.height);
  w.varint(page.nextStrokeId());
  w.varint(page.layers().size());
  for (const Layer& layer : page.layers()) {
    w.varint(layer.id());
    w.str(layer.name);
    w.u8(static_cast<uint8_t>((layer.visible ? 1 : 0) | (layer.locked ? 2 : 0)));
    w.f32(layer.opacity);
    w.varint(layer.size());
    for (const StrokePtr& s : layer.strokes()) writeStroke(w, *s);
  }
  return std::move(w.buf);
}

bool deserializePage(const uint8_t* data, size_t size, Page& out, std::string* error) {
  auto fail = [&](const char* msg) {
    if (error) *error = msg;
    return false;
  };
  if (!data || size < 8) return fail("truncated header");
  Reader r(data, size);
  for (uint8_t m : kMagic)
    if (r.u8() != m) return fail("not an InkEngine page");
  uint32_t version = r.u32();
  if (version == 0 || version > kPageFormatVersion) return fail("unsupported version");

  const float width = r.f32();  // separate statements: argument evaluation order is unspecified
  const float height = r.f32();
  Page page(width, height);
  page.layers().clear();
  page.setNextStrokeId(r.varint());
  uint64_t layerCount = r.varint();
  if (!r.ok() || layerCount > 4096) return fail("bad layer count");
  StrokeId maxId = 0;
  for (uint64_t li = 0; li < layerCount; ++li) {
    LayerId id = static_cast<LayerId>(r.varint());
    std::string name = r.str();
    uint8_t flags = r.u8();
    float opacity = r.f32();
    uint64_t count = r.varint();
    if (!r.ok() || count > r.remaining()) return fail("bad layer header");
    Layer& layer = page.addLayerWithId(id, name);
    layer.visible = (flags & 1) != 0;
    layer.locked = (flags & 2) != 0;
    layer.opacity = clamp01(opacity);
    for (uint64_t si = 0; si < count; ++si) {
      auto s = std::make_shared<Stroke>();
      if (!readStroke(r, *s)) return fail("corrupted stroke");
      maxId = std::max(maxId, s->id);
      layer.append(std::move(s));
    }
  }
  if (!r.ok()) return fail("truncated data");
  if (page.nextStrokeId() <= maxId) page.setNextStrokeId(maxId + 1);
  if (page.layers().empty()) page.addLayer("Layer 1");
  out = std::move(page);
  if (error) error->clear();
  return true;
}

std::vector<uint8_t> serializeStroke(const Stroke& stroke) {
  Writer w;
  w.u32(kPageFormatVersion);
  writeStroke(w, stroke);
  return std::move(w.buf);
}

bool deserializeStroke(const uint8_t* data, size_t size, Stroke& out) {
  if (!data) return false;
  Reader r(data, size);
  uint32_t version = r.u32();
  if (!r.ok() || version == 0 || version > kPageFormatVersion) return false;
  Stroke s;
  if (!readStroke(r, s)) return false;
  out = std::move(s);
  return true;
}

}  // namespace ink
