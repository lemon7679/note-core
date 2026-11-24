#pragma once

#include <cstdint>
#include <string>

#include "ink/color.h"
#include "ink/filters.h"
#include "ink/math.h"
#include "ink/pressure_curve.h"

namespace ink {

enum class BrushKind : uint8_t {
  Ballpoint = 0,    ///< 볼펜 — crisp, near-constant width, subtle pressure response.
  Pencil = 1,       ///< 연필 — graphite stamps with paper grain, tilt shading.
  Highlighter = 2,  ///< 형광펜 — chisel tip, translucent, multiply, no self-overlap darkening.
  FountainPen = 3,  ///< 만년필 — pressure + speed sensitive width, ink tapers.
  BrushPen = 4,     ///< 붓펜 — strong pressure response, long tapers.
  Calligraphy = 5,  ///< 캘리그라피 — fixed-angle flat nib.
  Marker = 6,       ///< 마커 — opaque, round felt tip.
  Count
};

const char* brushKindName(BrushKind kind);

/// How a stroke's ink is produced.
enum class RenderMethod : uint8_t {
  Vector = 0,  ///< Swept SDF primitives (capsules or nib quads): resolution independent.
  Stamp = 1,   ///< Textured stamps (pencil): grain, tilt, soft edges.
};

/// Footprint of a vector tip.
enum class TipShape : uint8_t {
  Round = 0,  ///< Circle — swept into variable-width capsules.
  Nib = 1,    ///< Flat rounded rectangle at a fixed page angle (chisel / calligraphy).
};

/// How the finished stroke is composited onto what is below it.
enum class BlendMode : uint8_t {
  Normal = 0,
  Multiply = 1,  ///< Highlighter: text underneath stays fully readable.
};

/// Everything needed to *render* a stroke. Stored with each stroke so the
/// document renders identically even if brush presets change later.
/// Dynamics (pressure, speed, taper) are already baked into the stroke points.
struct StrokeStyle {
  BrushKind kind = BrushKind::Ballpoint;
  RenderMethod method = RenderMethod::Vector;
  TipShape tip = TipShape::Round;
  BlendMode blend = BlendMode::Normal;
  Color color = colors::kBlack;
  /// Stroke-level opacity. Strokes are composited as a unit (coverage is
  /// max-accumulated first), so overlapping parts of one stroke never darken.
  float opacity = 1.0f;

  // --- Nib (TipShape::Nib) ---
  float nibAngle = kPi * 0.5f;  ///< Direction of the nib's long axis, page space (radians).
  float nibThickness = 0.35f;   ///< Short axis as a fraction of the width.

  // --- Stamps (RenderMethod::Stamp) ---
  float hardness = 0.55f;       ///< 0 = airbrush-soft, 1 = hard edge.
  float spacing = 0.08f;        ///< Stamp spacing as a fraction of the stamp diameter.
  float grainStrength = 0.0f;   ///< 0..1, how much paper grain modulates deposit.
  float grainScale = 0.5f;      ///< Page units per grain texel.
  float tiltAspect = 2.5f;      ///< Max major/minor ratio of the stamp at full tilt.
  float positionJitter = 0.0f;  ///< Fraction of the radius.
  float opacityJitter = 0.0f;   ///< 0..1.

  /// Render below non-"behind" strokes of the same layer (highlighter under ink).
  bool drawBehind = false;
};

struct TaperParams {
  float startLength = 0.0f;  ///< In multiples of the nominal width.
  float endLength = 0.0f;    ///< In multiples of the nominal width.
  float minScale = 0.15f;    ///< Width scale at the very tip.
  /// Scale the end taper with the lift-off speed (a quick flick leaves a long tail).
  bool velocityScaledEnd = true;
};

struct TiltParams {
  bool enabled = false;
  float startAltitude = 0.70f;  ///< Radians (~40°): shading begins below this.
  float fullAltitude = 0.30f;   ///< Radians (~17°): full shading.
  float maxWidthScale = 5.0f;   ///< Width multiplier at full tilt.
  float minOpacityScale = 0.4f; ///< Opacity multiplier at full tilt.
};

struct SmoothingParams {
  OneEuroFilter2D::Params pencil{6.0f, 0.06f, 1.0f};  ///< Stylus: precise, low lag.
  OneEuroFilter2D::Params touch{1.5f, 0.02f, 1.0f};   ///< Finger/mouse: more smoothing.
  StabilizerParams stabilizer;                         ///< Optional extra stabilization.
  float pressureTau = 0.010f;  ///< s, pressure EMA time constant.
  float speedTau = 0.035f;     ///< s, speed EMA time constant.
};

/// 글씨 보정 (handwriting correction) settings; see ink/correction.h.
struct CorrectionParams {
  /// 0 = off, 1 = strong. Scales every threshold below.
  float strength = 0.0f;
  /// Smooth while writing: the ink behind the pen settles into the corrected
  /// curve as the pen moves on (no lag; prediction stays on). See LiveSmoother.
  bool liveStabilization = true;
  bool smoothCurves = true;       ///< Remove wobble from curved parts at pen-up.
  bool straightenLines = true;    ///< Near-straight segments become straight.
  bool snapToAxes = true;         ///< Near-horizontal / vertical segments become exact.

  bool enabled() const { return strength > 0.0f; }
};

/// Everything needed to turn raw input into stroke points.
struct BrushDynamics {
  float width = mmToPt(0.5f);  ///< Nominal diameter (page units) at mid pressure.
  PressureCurve pressureCurve = PressureCurve::soft();
  float pressureWidthMin = 1.0f, pressureWidthMax = 1.0f;      ///< Width multiplier at p=0 / p=1.
  float pressureOpacityMin = 1.0f, pressureOpacityMax = 1.0f;  ///< Opacity at p=0 / p=1.
  /// Pressure used when the device has none (finger/mouse). Speed modulates it
  /// by `simulatedPressureSpeedEffect` so finger strokes still look alive.
  float simulatedPressure = 0.55f;
  float simulatedPressureSpeedEffect = 0.35f;

  float velocityThinning = 0.0f;     ///< 0..1 width removed at/after reference speed.
  float velocityReference = 900.0f;  ///< View points per second.

  TaperParams taper;
  TiltParams tilt;
  SmoothingParams smoothing;

  float minPointDistance = 0.35f;  ///< View points; closer samples are merged.
  float hookLength = 1.6f;         ///< View points; tiny pen-up/down hooks are trimmed.
  float minWidth = 0.05f;          ///< Absolute floor (page units).

  // Draw-and-hold shape snapping (line / ellipse / triangle / rectangle).
  bool snapOnHold = true;
  float holdDuration = 0.55f;  ///< Seconds the pen must rest.
  float holdTolerance = 2.5f;  ///< View points of allowed wobble while resting.

  // 글씨 보정: applied when the pen lifts (off by default).
  CorrectionParams correction;

  // Built-in motion prediction (use the platform's predicted touches when available).
  bool builtInPrediction = false;
  float predictionHorizon = 0.016f;  ///< Seconds.
};

struct Brush {
  std::string name;
  StrokeStyle style;
  BrushDynamics dynamics;

  /// Sets the nominal width and keeps everything proportional.
  Brush& withWidth(float w) { dynamics.width = w; return *this; }
  Brush& withColor(Color c) { style.color = c; return *this; }
};

namespace presets {
/// 볼펜. Width defaults to 0.5 mm.
Brush ballpoint(Color color = colors::kBlack, float width = mmToPt(0.5f));
/// 연필. Width defaults to 0.7 mm lead.
Brush pencil(Color color = colors::kGraphite, float width = mmToPt(0.7f));
/// 형광펜. Width defaults to 5 mm chisel.
Brush highlighter(Color color = colors::kHighlightYellow, float width = mmToPt(5.0f));
/// 만년필. Width defaults to 0.6 mm (F/M nib).
Brush fountainPen(Color color = colors::kInkBlue, float width = mmToPt(0.6f));
/// 붓펜.
Brush brushPen(Color color = colors::kBlack, float width = mmToPt(1.2f));
/// 캘리그라피 (45° flat nib).
Brush calligraphy(Color color = colors::kBlack, float width = mmToPt(2.0f));
/// 마커.
Brush marker(Color color = colors::kInkRed, float width = mmToPt(1.5f));

Brush forKind(BrushKind kind);
}  // namespace presets

}  // namespace ink
