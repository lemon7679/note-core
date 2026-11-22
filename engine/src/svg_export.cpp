#include "ink/svg_export.h"

#include <cstdio>

#include "ink/spline.h"

namespace ink {

namespace {

void arc(std::vector<Vec2>& out, Vec2 c, float r, float start, float sweep, float tolerance) {
  // Segment count from the sagitta error r(1 - cos(step/2)) <= tolerance.
  float maxStep = r > tolerance ? 2.0f * std::acos(clamp(1.0f - tolerance / r, -1.0f, 1.0f)) : kPi / 2.0f;
  int n = std::max(2, static_cast<int>(std::ceil(std::fabs(sweep) / std::max(maxStep, 0.05f))));
  for (int i = 0; i <= n; ++i) {
    float a = start + sweep * static_cast<float>(i) / static_cast<float>(n);
    out.push_back(c + fromAngle(a) * r);
  }
}

inline float angleOf(Vec2 v) { return std::atan2(v.y, v.x); }

std::vector<CurveSample> denseSamples(const Stroke& s, float tolerance) {
  CurveSamplingOptions o;
  o.maxChord = std::max(tolerance * 4.0f, 0.25f);
  o.maxAngle = 8.0f * kPi / 180.0f;
  std::vector<CurveSample> raw, out;
  CatmullRom::sampleAll(s.points.data(), s.points.size(), o, raw);
  for (const CurveSample& c : raw)
    if (out.empty() || distanceSq(out.back().pos, c.pos) > 1e-8f) out.push_back(c);
  return out;
}

StrokeOutline roundOutline(const std::vector<CurveSample>& s, float tolerance) {
  StrokeOutline o;
  const size_t n = s.size();
  if (n == 1) {
    arc(o.polygon, s[0].pos, s[0].width * 0.5f, 0.0f, kTwoPi, tolerance);
    return o;
  }
  std::vector<Vec2> left, right;
  left.reserve(n);
  right.reserve(n);
  const float kSharp = 25.0f * kPi / 180.0f;
  for (size_t i = 0; i < n; ++i) {
    float r = s[i].width * 0.5f;
    Vec2 dIn = i > 0 ? normalize(s[i].pos - s[i - 1].pos) : normalize(s[1].pos - s[0].pos);
    Vec2 dOut = i + 1 < n ? normalize(s[i + 1].pos - s[i].pos) : dIn;
    float sweep = std::atan2(cross(dIn, dOut), dot(dIn, dOut));
    Vec2 avgN = perp(normalize(dIn + dOut, perp(dIn)));
    if (std::fabs(sweep) > kSharp && i > 0 && i + 1 < n) {
      // Round join on the outer side; the inner side folds inside the ink.
      if (sweep > 0.0f) {
        left.push_back(s[i].pos + avgN * r);
        arc(right, s[i].pos, r, angleOf(-perp(dIn)), sweep, tolerance);
      } else {
        arc(left, s[i].pos, r, angleOf(perp(dIn)), sweep, tolerance);
        right.push_back(s[i].pos - avgN * r);
      }
    } else {
      left.push_back(s[i].pos + avgN * r);
      right.push_back(s[i].pos - avgN * r);
    }
  }
  Vec2 tEnd = normalize(s[n - 1].pos - s[n - 2].pos);
  Vec2 tStart = normalize(s[1].pos - s[0].pos);
  o.polygon = left;
  arc(o.polygon, s[n - 1].pos, s[n - 1].width * 0.5f, angleOf(perp(tEnd)), -kPi, tolerance);
  o.polygon.insert(o.polygon.end(), right.rbegin(), right.rend());
  arc(o.polygon, s[0].pos, s[0].width * 0.5f, angleOf(-perp(tStart)), -kPi, tolerance);
  return o;
}

StrokeOutline nibOutline(const std::vector<CurveSample>& s, const StrokeStyle& style) {
  StrokeOutline o;
  Vec2 dir = fromAngle(style.nibAngle);
  float roundSum = 0.0f;
  std::vector<Vec2> back;
  for (const CurveSample& c : s) {
    float round = c.width * style.nibThickness * 0.5f;
    float h = std::max(c.width * 0.5f - round, 0.0f);
    roundSum += round;
    o.polygon.push_back(c.pos + dir * h);
    back.push_back(c.pos - dir * h);
  }
  o.polygon.insert(o.polygon.end(), back.rbegin(), back.rend());
  o.strokeWidth = 2.0f * roundSum / static_cast<float>(s.size());
  return o;
}

void appendHex(std::string& out, Color c) {
  char buf[8];
  std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", static_cast<int>(std::lround(clamp01(c.r) * 255.0f)),
                static_cast<int>(std::lround(clamp01(c.g) * 255.0f)),
                static_cast<int>(std::lround(clamp01(c.b) * 255.0f)));
  out += buf;
}

void appendNum(std::string& out, float v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(v));
  // Trim trailing zeros to keep files small.
  std::string s(buf);
  while (!s.empty() && s.back() == '0') s.pop_back();
  if (!s.empty() && s.back() == '.') s.pop_back();
  if (s == "-0") s = "0";
  out += s;
}

}  // namespace

StrokeOutline strokeOutline(const Stroke& stroke, float tolerance) {
  StrokeOutline o;
  if (stroke.points.empty()) return o;
  std::vector<CurveSample> s = denseSamples(stroke, tolerance);
  float alphaSum = 0.0f;
  for (const CurveSample& c : s) alphaSum += c.alpha;
  if (stroke.style.method == RenderMethod::Vector && stroke.style.tip == TipShape::Nib)
    o = nibOutline(s, stroke.style);
  else
    o = roundOutline(s, tolerance);
  o.meanAlpha = alphaSum / static_cast<float>(s.size());
  return o;
}

std::string exportSvg(const Page& page, const SvgOptions& opts) {
  std::string out;
  out.reserve(1 << 16);
  out += "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"";
  appendNum(out, page.width);
  out += "pt\" height=\"";
  appendNum(out, page.height);
  out += "pt\" viewBox=\"0 0 ";
  appendNum(out, page.width);
  out += " ";
  appendNum(out, page.height);
  out += "\">\n";
  if (opts.drawBackground) {
    out += "<rect width=\"100%\" height=\"100%\" fill=\"";
    appendHex(out, opts.background);
    out += "\"/>\n";
  }
  for (const Layer& layer : page.layers()) {
    if (!layer.visible) continue;
    out += "<g";
    if (layer.opacity < 1.0f) {
      out += " opacity=\"";
      appendNum(out, layer.opacity);
      out += "\"";
    }
    out += ">\n";
    for (int pass = 0; pass < 2; ++pass) {
      for (const StrokePtr& sp : layer.strokes()) {
        const Stroke& s = *sp;
        if (s.style.drawBehind != (pass == 0)) continue;
        StrokeOutline o = strokeOutline(s, opts.tolerance);
        if (o.polygon.size() < 2) continue;
        float alpha = clamp01(s.style.opacity * s.style.color.a * o.meanAlpha);
        if (s.style.method == RenderMethod::Stamp) alpha *= 0.85f;  // graphite is never fully opaque
        out += "<path d=\"M";
        for (size_t i = 0; i < o.polygon.size(); ++i) {
          if (i) out += i == 1 ? " L" : " ";
          appendNum(out, o.polygon[i].x);
          out += ",";
          appendNum(out, o.polygon[i].y);
        }
        out += "Z\" fill=\"";
        appendHex(out, s.style.color);
        out += "\"";
        if (o.strokeWidth > 0.0f) {
          out += " stroke=\"";
          appendHex(out, s.style.color);
          out += "\" stroke-width=\"";
          appendNum(out, o.strokeWidth);
          out += "\" stroke-linejoin=\"round\"";
        }
        if (alpha < 1.0f) {
          out += " opacity=\"";
          appendNum(out, alpha);
          out += "\"";
        }
        if (s.style.blend == BlendMode::Multiply) out += " style=\"mix-blend-mode:multiply\"";
        out += "/>\n";
      }
    }
    out += "</g>\n";
  }
  out += "</svg>\n";
  return out;
}

}  // namespace ink
