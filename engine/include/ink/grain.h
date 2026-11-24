#pragma once

#include <cstdint>
#include <vector>

#include "ink/math.h"

namespace ink {

/// Tileable paper-grain height map used by the pencil.
///
/// Grain is sampled in *page* space (not stroke space), like real paper: two
/// strokes crossing the same spot reveal the same paper tooth, and the texture
/// does not "swim" while drawing. Values are histogram-equalized to a uniform
/// distribution so a deposit threshold t covers exactly (1 - t) of the paper.
///
/// The GPU renderer should upload `data()` as an R8 texture with repeat
/// wrapping and bilinear filtering, so it matches the CPU reference exactly.
class GrainTexture {
 public:
  static constexpr int kSize = 256;

  /// Shared, lazily generated instance (deterministic across platforms).
  static const GrainTexture& shared();

  explicit GrainTexture(uint32_t seed);

  int size() const { return kSize; }
  const uint8_t* data() const { return texels_.data(); }

  /// Bilinear, wrapped. (u, v) in texel units.
  float sample(float u, float v) const;

 private:
  std::vector<uint8_t> texels_;
};

/// Fraction of graphite deposited on a texel of height `grain` (0..1) by a lead
/// pressed to `depth` (0..1). Light pressure mostly marks the peaks of the paper;
/// heavy pressure fills the valleys too. The transition is wide and valleys
/// keep a generous film (`floor`), so the texture reads as soft, velvety
/// mottling rather than hard speckles; repeated passes fill it in further
/// because the rasterizer accumulates graphite density.
inline float grainDeposit(float grain, float depth, float strength) {
  depth = clamp01(depth);
  float threshold = lerp(0.72f, 0.10f, depth);
  float peaks = smoothstep(threshold - 0.50f, threshold + 0.50f, grain);
  float floor = 0.15f + 0.65f * depth;
  return lerp(1.0f, lerp(floor, 1.0f, peaks), clamp01(strength));
}

}  // namespace ink
