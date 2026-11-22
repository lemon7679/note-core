#include "ink/brush.h"

namespace ink {

const char* brushKindName(BrushKind kind) {
  switch (kind) {
    case BrushKind::Ballpoint: return "ballpoint";
    case BrushKind::Pencil: return "pencil";
    case BrushKind::Highlighter: return "highlighter";
    case BrushKind::FountainPen: return "fountain-pen";
    case BrushKind::BrushPen: return "brush-pen";
    case BrushKind::Calligraphy: return "calligraphy";
    case BrushKind::Marker: return "marker";
    case BrushKind::Count: break;
  }
  return "unknown";
}

namespace presets {

Brush ballpoint(Color color, float width) {
  // A ballpoint deposits a near-constant line: the ball diameter fixes the width,
  // pressure only flattens the ink trail slightly. Crisp, solid, no tapers —
  // what makes GoodNotes' ballpoint feel "precise".
  Brush b;
  b.name = "Ballpoint";
  b.style.kind = BrushKind::Ballpoint;
  b.style.method = RenderMethod::Vector;
  b.style.tip = TipShape::Round;
  b.style.color = color;
  b.dynamics.width = width;
  b.dynamics.pressureCurve = PressureCurve::soft();
  b.dynamics.pressureWidthMin = 0.80f;
  b.dynamics.pressureWidthMax = 1.12f;
  b.dynamics.velocityThinning = 0.06f;
  b.dynamics.smoothing.pencil = {6.0f, 0.06f, 1.0f};
  return b;
}

Brush pencil(Color color, float width) {
  // Graphite: pressure drives both darkness and how deep into the paper grain the
  // lead reaches. Tilting the Pencil switches to broad, light shading.
  Brush b;
  b.name = "Pencil";
  b.style.kind = BrushKind::Pencil;
  b.style.method = RenderMethod::Stamp;
  b.style.tip = TipShape::Round;
  b.style.color = color;
  b.style.hardness = 0.30f;
  b.style.spacing = 0.07f;
  b.style.grainStrength = 0.85f;
  b.style.grainScale = 0.35f;
  b.style.tiltAspect = 2.2f;
  b.style.positionJitter = 0.05f;
  b.style.opacityJitter = 0.10f;
  b.dynamics.width = width;
  b.dynamics.pressureCurve = PressureCurve::soft();
  b.dynamics.pressureWidthMin = 0.70f;
  b.dynamics.pressureWidthMax = 1.25f;
  b.dynamics.pressureOpacityMin = 0.25f;
  b.dynamics.pressureOpacityMax = 0.95f;
  b.dynamics.taper = {1.5f, 1.5f, 0.55f, false};
  b.dynamics.tilt.enabled = true;
  b.dynamics.tilt.maxWidthScale = 6.0f;
  b.dynamics.tilt.minOpacityScale = 0.45f;
  b.dynamics.snapOnHold = false;
  return b;
}

Brush highlighter(Color color, float width) {
  // Chisel-tip highlighter: constant width, translucent, multiplied so the text
  // underneath stays crisp. The whole stroke is one coverage mask, so a stroke
  // that crosses itself does not get darker — only separate strokes stack.
  Brush b;
  b.name = "Highlighter";
  b.style.kind = BrushKind::Highlighter;
  b.style.method = RenderMethod::Vector;
  b.style.tip = TipShape::Nib;
  b.style.blend = BlendMode::Multiply;
  b.style.color = color;
  b.style.opacity = 0.60f;
  b.style.nibAngle = kPi * 0.5f;  // vertical chisel: full height on horizontal strokes
  b.style.nibThickness = 0.35f;
  b.dynamics.width = width;
  b.dynamics.pressureCurve = PressureCurve::linear();
  b.dynamics.pressureWidthMin = b.dynamics.pressureWidthMax = 1.0f;
  b.dynamics.smoothing.pencil = {3.0f, 0.03f, 1.0f};  // steadier: highlights are long, calm strokes
  b.dynamics.hookLength = 3.0f;
  b.dynamics.snapOnHold = true;
  return b;
}

Brush fountainPen(Color color, float width) {
  // Ink flows more with pressure and less with speed; a fast lift leaves a tail.
  Brush b;
  b.name = "Fountain Pen";
  b.style.kind = BrushKind::FountainPen;
  b.style.method = RenderMethod::Vector;
  b.style.tip = TipShape::Round;
  b.style.color = color;
  b.dynamics.width = width;
  b.dynamics.pressureCurve = PressureCurve::soft();
  b.dynamics.pressureWidthMin = 0.35f;
  b.dynamics.pressureWidthMax = 1.60f;
  b.dynamics.velocityThinning = 0.25f;
  b.dynamics.taper = {1.0f, 2.5f, 0.25f, true};
  return b;
}

Brush brushPen(Color color, float width) {
  Brush b;
  b.name = "Brush Pen";
  b.style.kind = BrushKind::BrushPen;
  b.style.method = RenderMethod::Vector;
  b.style.tip = TipShape::Round;
  b.style.color = color;
  b.dynamics.width = width;
  b.dynamics.pressureCurve = PressureCurve(Vec2{0.30f, 0.10f}, Vec2{0.55f, 0.95f});
  b.dynamics.pressureWidthMin = 0.10f;
  b.dynamics.pressureWidthMax = 1.90f;
  b.dynamics.velocityThinning = 0.35f;
  b.dynamics.taper = {2.5f, 4.0f, 0.06f, true};
  b.dynamics.snapOnHold = false;
  return b;
}

Brush calligraphy(Color color, float width) {
  Brush b;
  b.name = "Calligraphy";
  b.style.kind = BrushKind::Calligraphy;
  b.style.method = RenderMethod::Vector;
  b.style.tip = TipShape::Nib;
  b.style.color = color;
  b.style.nibAngle = -kPi * 0.25f;  // 45° italic nib (page y points down)
  b.style.nibThickness = 0.12f;
  b.dynamics.width = width;
  b.dynamics.pressureCurve = PressureCurve::soft();
  b.dynamics.pressureWidthMin = 0.70f;
  b.dynamics.pressureWidthMax = 1.15f;
  b.dynamics.snapOnHold = false;
  return b;
}

Brush marker(Color color, float width) {
  Brush b;
  b.name = "Marker";
  b.style.kind = BrushKind::Marker;
  b.style.method = RenderMethod::Vector;
  b.style.tip = TipShape::Round;
  b.style.color = color;
  b.dynamics.width = width;
  b.dynamics.pressureCurve = PressureCurve::linear();
  b.dynamics.pressureWidthMin = 0.95f;
  b.dynamics.pressureWidthMax = 1.05f;
  return b;
}

Brush forKind(BrushKind kind) {
  switch (kind) {
    case BrushKind::Ballpoint: return ballpoint();
    case BrushKind::Pencil: return pencil();
    case BrushKind::Highlighter: return highlighter();
    case BrushKind::FountainPen: return fountainPen();
    case BrushKind::BrushPen: return brushPen();
    case BrushKind::Calligraphy: return calligraphy();
    case BrushKind::Marker: return marker();
    case BrushKind::Count: break;
  }
  return ballpoint();
}

}  // namespace presets
}  // namespace ink
