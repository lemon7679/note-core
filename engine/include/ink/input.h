#pragma once

#include <cstdint>

#include "ink/math.h"

namespace ink {

enum class InputSource : uint8_t {
  Pencil = 0,  ///< Stylus with pressure (Apple Pencil, S-Pen, Wacom...).
  Touch = 1,   ///< Finger. No pressure: pressure is simulated from speed.
  Mouse = 2,   ///< Mouse / trackpad. No pressure.
};

enum InputFlags : uint32_t {
  /// The pressure value is an estimate; a refined value will arrive later through
  /// StrokeBuilder::updateEstimatedProperties (iOS `estimatedPropertiesExpectingUpdates`).
  kInputPressureExpectsUpdate = 1u << 0,
  /// Same, for altitude / azimuth.
  kInputTiltExpectsUpdate = 1u << 1,
};

/// One raw input sample as delivered by the platform. On iOS feed every coalesced
/// touch (`coalescedTouches(for:)`) — Apple Pencil reports at 240 Hz.
struct InputPoint {
  Vec2 pos;                ///< Page coordinates (page units, pt).
  float pressure = -1.0f;  ///< Normalized 0..1 (force / maximumPossibleForce). < 0 = unavailable.
  float altitude = -1.0f;  ///< Radians, pi/2 = perpendicular to screen. < 0 = unavailable.
  float azimuth = 0.0f;    ///< Radians in page space (direction the stylus points to).
  double time = 0.0;       ///< Seconds (monotonic clock).
  uint64_t updateId = 0;   ///< Platform id used to match later estimated-property updates.
  uint32_t flags = 0;      ///< InputFlags.
  InputSource source = InputSource::Pencil;

  bool hasPressure() const { return pressure >= 0.0f; }
  bool hasTilt() const { return altitude >= 0.0f; }
};

}  // namespace ink
