#include "ink/grain.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace ink {


const GrainTexture& GrainTexture::shared() {
  static const GrainTexture instance(0x5EED1234u);
  return instance;
}

GrainTexture::GrainTexture(uint32_t seed) {
  const int n = kSize;
  std::vector<float> h(static_cast<size_t>(n) * n, 0.0f);
  // Paper tooth as a field of soft, round bumps (sparse Gaussian convolution
  // noise): isotropic, no lattice squares, no per-texel white noise. Fine
  // bumps give the velvety grain, a few broad ones a gentle "sizing" mottle,
  // and faint elongated ones the fibres along x.
  struct Layer { int count; float sigmaMin, sigmaMax, aspect, weight; };
  const Layer layers[] = {
      {7000, 0.9f, 1.7f, 1.0f, 1.00f},   // tooth
      {700, 2.5f, 4.0f, 1.0f, 0.28f},    // clusters
      {60, 10.0f, 16.0f, 1.0f, 0.16f},   // sizing
      {300, 0.9f, 1.4f, 4.0f, 0.30f},    // fibres (stretched along x)
  };
  uint32_t k = 0;
  for (const Layer& L : layers) {
    for (int i = 0; i < L.count; ++i, ++k) {
      const uint32_t h0 = hash32(seed, k * 4u + 1u);
      const float cx = hashToUnit(h0) * n;
      const float cy = hashToUnit(hash32(h0, 2u)) * n;
      const float sigma = lerp(L.sigmaMin, L.sigmaMax, hashToUnit(hash32(h0, 3u)));
      const float amp = L.weight * (0.5f + hashToUnit(hash32(h0, 4u)));
      const float sx = sigma * L.aspect, sy = sigma;
      const float ix = 1.0f / (2.0f * sx * sx), iy = 1.0f / (2.0f * sy * sy);
      const int rx = static_cast<int>(std::ceil(3.0f * sx)), ry = static_cast<int>(std::ceil(3.0f * sy));
      const int x0 = static_cast<int>(std::floor(cx)), y0 = static_cast<int>(std::floor(cy));
      for (int dy = -ry; dy <= ry; ++dy) {
        const float fy = static_cast<float>(y0 + dy) + 0.5f - cy;
        const float gy = std::exp(-fy * fy * iy);
        const size_t row = static_cast<size_t>(((y0 + dy) % n + n) % n) * n;
        for (int dx = -rx; dx <= rx; ++dx) {
          const float fx = static_cast<float>(x0 + dx) + 0.5f - cx;
          h[row + static_cast<size_t>(((x0 + dx) % n + n) % n)] += amp * gy * std::exp(-fx * fx * ix);
        }
      }
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
