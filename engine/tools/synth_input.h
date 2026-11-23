// Synthetic stylus input for demos, tests and benchmarks.
// Emulates an Apple Pencil: 240 Hz samples, speed that slows down in curves
// (2/3 power law), sensor jitter + hand tremor, pressure ramps on touch-down
// and lift-off, and optional tilt.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ink/input.h"

namespace synth {

struct Options {
  float speed = 260.0f;        ///< Mean writing speed, pt/s.
  float rate = 240.0f;         ///< Samples per second.
  float jitter = 0.12f;        ///< Sensor noise, pt (std-dev-ish).
  float tremor = 0.25f;        ///< Slow hand tremor amplitude, pt.
  float pressure = 0.32f;      ///< Base normalized force.
  float pressureWave = 0.10f;  ///< Pressure variation along the stroke.
  float pressureWavelength = 90.0f;  ///< pt.
  float altitude = 1.15f;      ///< Radians.
  float azimuth = 0.9f;        ///< Radians.
  bool hasPressure = true;
  ink::InputSource source = ink::InputSource::Pencil;
  uint32_t seed = 1;
  double startTime = 0.0;
};

std::vector<ink::InputPoint> synthesize(const std::vector<ink::Vec2>& path, const Options& opts);

// ---- Path generators (page units) ----
std::vector<ink::Vec2> polyline(const std::vector<ink::Vec2>& pts, float step = 0.5f);
std::vector<ink::Vec2> cursiveLoops(ink::Vec2 origin, int loops, float size);
std::vector<ink::Vec2> wave(ink::Vec2 origin, float length, float amplitude, float wavelength);
std::vector<ink::Vec2> spiral(ink::Vec2 center, float turns, float r0, float r1);
std::vector<ink::Vec2> ellipse(ink::Vec2 center, float rx, float ry, float wobble, uint32_t seed,
                               float startAngle = -1.6f, float sweep = 6.45f);
/// "Handwritten" signature-like scribble from a seed.
std::vector<ink::Vec2> scribble(ink::Vec2 origin, float width, float height, uint32_t seed);

/// Stroke paths of a Hangul syllable from a small built-in set
/// (가 나 다 라 마 바 사 아 한 글 보 정), written in a box of `size` at `origin`
/// (top-left). `messiness` (0..1) perturbs corners and stroke angles like a
/// hurried hand. Unknown characters yield no strokes.
std::vector<std::vector<ink::Vec2>> hangul(char32_t syllable, ink::Vec2 origin, float size, uint32_t seed,
                                           float messiness);

/// A line of text: every stroke of every syllable as synthesized input, with
/// realistic per-stroke timing. `baseline(i)` returns the vertical offset of
/// syllable i, `slantRadians` rotates the whole line about its start.
struct WrittenStroke {
  std::vector<ink::InputPoint> input;
  size_t syllable = 0;
};
std::vector<WrittenStroke> writeLine(const std::u32string& text, ink::Vec2 origin, float size, uint32_t seed,
                                     float messiness, float slantRadians = 0.0f,
                                     const std::vector<float>& baseline = {});

}  // namespace synth
