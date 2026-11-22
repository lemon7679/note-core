#include "ink/grain.h"

#include <algorithm>
#include <numeric>

namespace ink {

namespace {

inline float quintic(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }

/// Periodic value noise with (freqX, freqY) lattice cells across the tile.
float valueNoise(float x, float y, int freqX, int freqY, uint32_t seed) {
  float fx = x * freqX, fy = y * freqY;
  int ix = static_cast<int>(std::floor(fx)), iy = static_cast<int>(std::floor(fy));
  float tx = quintic(fx - ix), ty = quintic(fy - iy);
  auto lattice = [&](int i, int j) {
    i = ((i % freqX) + freqX) % freqX;
    j = ((j % freqY) + freqY) % freqY;
    return hashToUnit(hash32(seed, static_cast<uint32_t>(i * 7919 + j * 104729)));
  };
  float a = lattice(ix, iy), b = lattice(ix + 1, iy);
  float c = lattice(ix, iy + 1), d = lattice(ix + 1, iy + 1);
  return lerp(lerp(a, b, tx), lerp(c, d, tx), ty);
}

}  // namespace

const GrainTexture& GrainTexture::shared() {
  static const GrainTexture instance(0x5EED1234u);
  return instance;
}

GrainTexture::GrainTexture(uint32_t seed) {
  const int n = kSize;
  std::vector<float> h(static_cast<size_t>(n) * n);
  // Paper tooth is fine-grained: most energy at 1-4 texel features, a little
  // mid-frequency "sizing" variation, and short fibres elongated along x.
  // (Low frequencies must stay weak, or graphite looks blotchy/camouflaged.)
  struct Octave { int freqX, freqY; float weight; };
  const Octave octaves[] = {
      {16, 16, 0.05f}, {64, 64, 0.22f}, {128, 128, 0.30f}, {256, 256, 0.25f}, {48, 192, 0.10f},
  };
  for (int y = 0; y < n; ++y) {
    for (int x = 0; x < n; ++x) {
      float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(n);
      float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(n);
      float sum = 0.0f;
      uint32_t o = 0;
      for (const Octave& oc : octaves) {
        sum += valueNoise(u, v, oc.freqX, oc.freqY, seed + o) * oc.weight;
        ++o;
      }
      // Fine white tooth.
      sum += 0.08f * hashToUnit(hash32(seed ^ 0xA5A5A5u, static_cast<uint32_t>(y * n + x)));
      h[static_cast<size_t>(y) * n + x] = sum;
    }
  }
  // Histogram equalization -> uniform distribution in [0, 1].
  std::vector<uint32_t> order(h.size());
  std::iota(order.begin(), order.end(), 0u);
  std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return h[a] < h[b]; });
  texels_.resize(h.size());
  for (size_t rank = 0; rank < order.size(); ++rank)
    texels_[order[rank]] = static_cast<uint8_t>((rank * 256) / order.size());
}

float GrainTexture::sample(float u, float v) const {
  const int n = kSize;
  u -= 0.5f;
  v -= 0.5f;
  float fu = std::floor(u), fv = std::floor(v);
  int x0 = static_cast<int>(fu), y0 = static_cast<int>(fv);
  float tx = u - fu, ty = v - fv;
  auto at = [&](int x, int y) {
    x &= n - 1;
    y &= n - 1;
    return texels_[static_cast<size_t>(y) * n + x] * (1.0f / 255.0f);
  };
  float a = at(x0, y0), b = at(x0 + 1, y0), c = at(x0, y0 + 1), d = at(x0 + 1, y0 + 1);
  return lerp(lerp(a, b, tx), lerp(c, d, tx), ty);
}

}  // namespace ink
