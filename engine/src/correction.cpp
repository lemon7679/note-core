#include "ink/correction.h"

#include <algorithm>
#include <memory>
#include <numeric>
#include <unordered_set>

#include "ink/selection.h"

namespace ink {

namespace {

inline float rad(float degrees) { return degrees * kPi / 180.0f; }

/// Arc-length parameterized view of a stroke's control polygon.
struct ArcLength {
  const std::vector<Vec2>& p;
  std::vector<float> d;

  explicit ArcLength(const std::vector<Vec2>& pts) : p(pts), d(pts.size(), 0.0f) {
    for (size_t i = 1; i < p.size(); ++i) d[i] = d[i - 1] + distance(p[i - 1], p[i]);
  }
  float total() const { return d.empty() ? 0.0f : d.back(); }
  Vec2 at(float s) const {
    if (s <= 0.0f) return p.front();
    if (s >= total()) return p.back();
    size_t i = static_cast<size_t>(std::upper_bound(d.begin(), d.end(), s) - d.begin());
    float span = d[i] - d[i - 1];
    return lerp(p[i - 1], p[i], span > 0.0f ? (s - d[i - 1]) / span : 0.0f);
  }
  /// Direction change at point i measured over +-w of arc length.
  float turn(size_t i, float w) const {
    Vec2 a = at(d[i] - w), b = at(d[i] + w);
    Vec2 v1 = p[i] - a, v2 = b - p[i];
    float l1 = length(v1), l2 = length(v2);
    if (l1 < w * 0.3f || l2 < w * 0.3f) return 0.0f;
    return std::acos(clamp(dot(v1, v2) / (l1 * l2), -1.0f, 1.0f));
  }
};

/// Disjoint-set union over segment boundary nodes.
struct UnionFind {
  std::vector<size_t> parent;
  explicit UnionFind(size_t n) : parent(n) { std::iota(parent.begin(), parent.end(), size_t{0}); }
  size_t find(size_t x) {
    while (parent[x] != x) x = parent[x] = parent[parent[x]];
    return x;
  }
  void unite(size_t a, size_t b) { parent[find(a)] = find(b); }
};

/// Gaussian smoothing along arc length, restricted to points [a, b].
/// Each neighbour is weighted by the arc length it represents, so uneven
/// sample spacing (slow corners, fast strokes) does not bias the result.
void gaussianAlongArc(const std::vector<Vec2>& in, const std::vector<float>& d, size_t a, size_t b, float sigma,
                      std::vector<Vec2>& out) {
  out = in;
  const float reach = 3.0f * sigma;
  const float inv2s2 = 1.0f / (2.0f * sigma * sigma);
  size_t lo = a;
  for (size_t j = a; j <= b; ++j) {
    while (lo < j && d[j] - d[lo] > reach) ++lo;
    Vec2 sum;
    float wsum = 0.0f;
    for (size_t k = lo; k <= b && d[k] - d[j] <= reach; ++k) {
      float share = 0.5f * (d[std::min(k + 1, b)] - d[k > a ? k - 1 : a]);
      float dd = d[k] - d[j];
      float w = std::exp(-dd * dd * inv2s2) * std::max(share, 1e-4f);
      sum += in[k] * w;
      wsum += w;
    }
    if (wsum > 0.0f) out[j] = sum / wsum;
  }
}

float median(std::vector<float> v) {
  if (v.empty()) return 0.0f;
  size_t m = v.size() / 2;
  std::nth_element(v.begin(), v.begin() + static_cast<long>(m), v.end());
  return v[m];
}

}  // namespace

bool correctStroke(std::vector<StrokePoint>& points, const CorrectionParams& params, float u,
                   CorrectionReport* report) {
  CorrectionReport rep;
  const float s = clamp01(params.strength);
  const size_t n = points.size();
  if (report) *report = rep;
  if (s <= 0.0f || n < 3) return false;

  std::vector<Vec2> pos(n);
  std::vector<float> widths(n);
  for (size_t i = 0; i < n; ++i) {
    pos[i] = points[i].pos;
    widths[i] = points[i].width;
  }
  const ArcLength arc(pos);
  const float total = arc.total();
  if (total < 3.0f * u) return false;  // dots and specks stay as they are
  const float medW = median(widths);

  // ---- 1. Corners: sharp, localized direction changes. ----------------------
  // A corner turns as much over half the window as over the whole window; a
  // smooth curve turns only half as much. This keeps loops and bowls (ㅇ, ㅎ,
  // 'e') from being chopped into straight pieces.
  const float w = std::max(2.5f * u, 1.2f * medW);
  const float cornerAngle = rad(45.0f);
  struct Cand { size_t i; float turn; };
  std::vector<Cand> cands;
  for (size_t i = 1; i + 1 < n; ++i) {
    if ((points[i].flags & kPointSharp) != 0) {
      cands.push_back({i, kPi});
      continue;
    }
    if (arc.d[i] < 0.5f * w || total - arc.d[i] < 0.5f * w) continue;
    float t1 = arc.turn(i, w);
    if (t1 < cornerAngle) continue;
    if (arc.turn(i, 0.5f * w) < 0.65f * t1) continue;
    cands.push_back({i, t1});
  }
  std::sort(cands.begin(), cands.end(), [](const Cand& x, const Cand& y) { return x.turn > y.turn; });
  std::vector<size_t> nodes{0, n - 1};
  for (const Cand& c : cands) {
    bool clear = true;
    for (size_t k : nodes)
      if (std::fabs(arc.d[k] - arc.d[c.i]) < w && k != 0 && k != n - 1) clear = false;
    if (clear) nodes.push_back(c.i);
  }
  std::sort(nodes.begin(), nodes.end());
  nodes.erase(std::unique(nodes.begin(), nodes.end()), nodes.end());
  rep.corners = static_cast<int>(nodes.size()) - 2;

  // ---- 2. Classify the segments between corners. ----------------------------
  const size_t segs = nodes.size() - 1;
  std::vector<bool> straight(segs, false);
  std::vector<int> axis(segs, -1);  // 0 = horizontal, 1 = vertical
  const float devTol = lerp(0.025f, 0.07f, s);
  const float arcTol = lerp(1.02f, 1.07f, s);
  const float axisTol = rad(lerp(4.0f, 12.0f, s));
  for (size_t k = 0; k < segs; ++k) {
    size_t a = nodes[k], b = nodes[k + 1];
    Vec2 A = pos[a], B = pos[b];
    float L = distance(A, B);
    if (!params.straightenLines || L < 5.0f * u || arc.d[b] - arc.d[a] > L * arcTol) continue;
    float dev = 0.0f;
    for (size_t j = a + 1; j < b; ++j) dev = std::max(dev, pointSegmentDistance(pos[j], A, B));
    if (dev > L * devTol + 0.15f * u) continue;
    straight[k] = true;
    ++rep.straightened;
    if (!params.snapToAxes) continue;
    float ang = std::atan2(B.y - A.y, B.x - A.x);
    float dH = std::fabs(wrapAngle(ang - std::round(ang / kPi) * kPi));
    float dV = std::fabs(wrapAngle(ang - kPi * 0.5f - std::round((ang - kPi * 0.5f) / kPi) * kPi));
    if (dH <= axisTol && dH <= dV) axis[k] = 0;
    else if (dV <= axisTol) axis[k] = 1;
    if (axis[k] >= 0) ++rep.axisSnapped;
  }

  // ---- 3. Move corners so axis-aligned segments become exact. ---------------
  // Nodes joined by horizontal segments share one y, by vertical ones one x,
  // so connected strokes (ㄱ ㄴ ㄹ ㅁ) stay connected at their corners.
  const size_t m = nodes.size();
  std::vector<Vec2> nodePos(m);
  for (size_t k = 0; k < m; ++k) nodePos[k] = pos[nodes[k]];
  UnionFind ux(m), uy(m);
  for (size_t k = 0; k < segs; ++k) {
    if (axis[k] == 0) uy.unite(k, k + 1);
    if (axis[k] == 1) ux.unite(k, k + 1);
  }
  std::vector<float> sumX(m, 0.0f), cntX(m, 0.0f), sumY(m, 0.0f), cntY(m, 0.0f);
  for (size_t k = 0; k < m; ++k) {
    sumX[ux.find(k)] += nodePos[k].x;
    cntX[ux.find(k)] += 1.0f;
    sumY[uy.find(k)] += nodePos[k].y;
    cntY[uy.find(k)] += 1.0f;
  }
  std::vector<Vec2> newNode(nodePos);
  for (size_t k = 0; k < m; ++k) {
    newNode[k].x = sumX[ux.find(k)] / cntX[ux.find(k)];
    newNode[k].y = sumY[uy.find(k)] / cntY[uy.find(k)];
  }

  // ---- 4. Rebuild each segment. ---------------------------------------------
  std::vector<Vec2> out(pos);
  const float sigma = lerp(0.4f, 2.0f, s) * u;
  std::vector<Vec2> g1, g2;
  for (size_t k = 0; k < segs; ++k) {
    const size_t a = nodes[k], b = nodes[k + 1];
    const float span = arc.d[b] - arc.d[a];
    const Vec2 A = newNode[k], B = newNode[k + 1];
    const Vec2 dA = A - pos[a], dB = B - pos[b];
    for (size_t j = a; j <= b; ++j) {
      float t = span > 0.0f ? (arc.d[j] - arc.d[a]) / span : 0.0f;
      // Straight: points slide onto the line, keeping their spacing (speed).
      out[j] = straight[k] ? lerp(A, B, t) : pos[j] + lerp(dA, dB, t);
    }
    if (straight[k] || !params.smoothCurves || b - a < 3 || span < 2.0f * sigma) continue;
    // Zero-shrink smoothing ("twicing"): 2·G(x) − G(G(x)). Plain Gaussian
    // smoothing pulls curves inward; twicing cancels that to second order
    // while still removing tremor. Ends are pinned to the corners.
    gaussianAlongArc(out, arc.d, a, b, sigma, g1);
    gaussianAlongArc(g1, arc.d, a, b, sigma, g2);
    for (size_t j = a + 1; j < b; ++j) {
      float edge = std::min(arc.d[j] - arc.d[a], arc.d[b] - arc.d[j]);
      float alpha = smoothstep(0.0f, 2.0f * sigma, edge);
      out[j] = lerp(out[j], g1[j] * 2.0f - g2[j], alpha);
    }
  }

  bool changed = false;
  for (size_t i = 0; i < n; ++i) {
    float dmove = distance(out[i], points[i].pos);
    rep.maxDisplacement = std::max(rep.maxDisplacement, dmove);
    if (dmove > 1e-5f) changed = true;
    points[i].pos = out[i];
  }
  if (report) *report = rep;
  return changed;
}

ChangeSet correctStrokes(Page& page, LayerId layer, const std::vector<StrokeId>& ids, const CorrectionParams& params,
                         float unitsPerViewPoint) {
  return mapStrokes(page, layer, ids, [&](const StrokePtr& s) -> StrokePtr {
    auto copy = std::make_shared<Stroke>(*s);
    if (!correctStroke(copy->points, params, unitsPerViewPoint)) return s;
    copy->updateBounds();
    return copy;
  });
}

// ---------------------------------------------------------------------------
// Line straightening
// ---------------------------------------------------------------------------

namespace {

struct InkSample {
  Vec2 p;
  float w;
  size_t stroke;
};

struct StrokeInfo {
  const Stroke* stroke = nullptr;
  float ink = 0.0f;  // total weight
  Rect box;          // in deskewed space
  float midY = 0.0f; // weighted median y in deskewed space
  size_t line = 0;
};

float weightedMedian(std::vector<std::pair<float, float>> vw) {
  if (vw.empty()) return 0.0f;
  std::sort(vw.begin(), vw.end());
  float total = 0.0f;
  for (const auto& e : vw) total += e.second;
  float acc = 0.0f;
  for (const auto& e : vw) {
    acc += e.second;
    if (acc >= 0.5f * total) return e.first;
  }
  return vw.back().first;
}

Vec2 rotateAround(Vec2 p, Vec2 c, float cs, float sn) {
  Vec2 d = p - c;
  return c + Vec2{d.x * cs - d.y * sn, d.x * sn + d.y * cs};
}

/// Peakiness of the horizontal projection profile after rotating by -theta.
float profileScore(const std::vector<InkSample>& samples, Vec2 c, float theta, float bin) {
  const float cs = std::cos(-theta), sn = std::sin(-theta);
  float lo = 1e30f, hi = -1e30f;
  std::vector<float> ys(samples.size());
  for (size_t i = 0; i < samples.size(); ++i) {
    ys[i] = rotateAround(samples[i].p, c, cs, sn).y;
    lo = std::min(lo, ys[i]);
    hi = std::max(hi, ys[i]);
  }
  size_t nb = static_cast<size_t>((hi - lo) / bin) + 2;
  if (nb > 16384) return 0.0f;
  std::vector<float> h(nb, 0.0f);
  for (size_t i = 0; i < samples.size(); ++i) {
    float f = (ys[i] - lo) / bin;
    size_t k = static_cast<size_t>(f);
    float t = f - static_cast<float>(k);
    h[k] += samples[i].w * (1.0f - t);
    h[k + 1] += samples[i].w * t;
  }
  float score = 0.0f;
  for (float v : h) score += v * v;
  return score;
}

}  // namespace

ChangeSet straightenHandwriting(Page& page, LayerId layerId, const std::vector<StrokeId>& ids,
                                const StraightenParams& params) {
  ChangeSet none;
  const Layer* layer = page.layer(layerId);
  const float k = clamp01(params.strength);
  if (!layer || layer->locked || k <= 0.0f) return none;

  std::unordered_set<StrokeId> wanted(ids.begin(), ids.end());
  std::vector<StrokeInfo> strokes;
  for (const StrokePtr& sp : layer->strokes())
    if (wanted.count(sp->id) && !sp->points.empty()) strokes.push_back(StrokeInfo{sp.get(), 0.0f, Rect(), 0.0f, 0});
  if (strokes.empty()) return none;

  // ---- Ink samples weighted by the length they represent. ----
  std::vector<InkSample> samples;
  std::vector<float> sizes;
  for (size_t si = 0; si < strokes.size(); ++si) {
    const std::vector<StrokePoint>& pts = strokes[si].stroke->points;
    Rect b;
    for (size_t i = 0; i < pts.size(); ++i) {
      float share = 0.5f * ((i + 1 < pts.size() ? distance(pts[i].pos, pts[i + 1].pos) : 0.0f) +
                            (i > 0 ? distance(pts[i - 1].pos, pts[i].pos) : 0.0f));
      float w = std::max(share, 0.05f);
      samples.push_back({pts[i].pos, w, si});
      strokes[si].ink += w;
      b.expand(pts[i].pos);
    }
    sizes.push_back(std::max(b.width(), b.height()));
  }
  const float textSize = std::max(median(sizes), 1e-2f);
  Vec2 c;
  float wsum = 0.0f;
  for (const InkSample& sm : samples) {
    c += sm.p * sm.w;
    wsum += sm.w;
  }
  c = c / wsum;

  // ---- 1. Global deskew: angle with the sharpest projection profile. ----
  const float bin = std::max(textSize * 0.15f, 0.2f);
  const float maxA = rad(params.maxAngleDegrees);
  float best = 0.0f, bestScore = profileScore(samples, c, 0.0f, bin);
  const float baseScore = bestScore;
  for (float a = -maxA; a <= maxA + 1e-6f; a += rad(1.0f)) {
    float sc = profileScore(samples, c, a, bin);
    if (sc > bestScore) { bestScore = sc; best = a; }
  }
  const float coarse = best;
  for (float a = coarse - rad(1.0f); a <= coarse + rad(1.0f) + 1e-6f; a += rad(0.1f)) {
    float sc = profileScore(samples, c, a, bin);
    if (sc > bestScore) { bestScore = sc; best = a; }
  }
  if (bestScore < baseScore * 1.02f) best = 0.0f;  // no clear writing direction
  const float alpha = best;

  // ---- 2. Strokes in deskewed space. ----
  const float cs = std::cos(-alpha), sn = std::sin(-alpha);
  std::vector<std::vector<std::pair<float, float>>> ysPerStroke(strokes.size());
  for (const InkSample& sm : samples) {
    Vec2 q = rotateAround(sm.p, c, cs, sn);
    strokes[sm.stroke].box.expand(q);
    ysPerStroke[sm.stroke].push_back({q.y, sm.w});
  }
  for (size_t si = 0; si < strokes.size(); ++si) strokes[si].midY = weightedMedian(ysPerStroke[si]);

  // ---- 3. Text lines by local chaining. ----
  // Two strokes belong to the same line when they are horizontally near and
  // vertically close; chaining these local links follows lines that are still
  // slanted or wavy after the global deskew, while separate lines (one line
  // height or more apart) never link.
  struct Line {
    std::vector<size_t> members;
  };
  auto isMajor = [&](const StrokeInfo& st) {
    return std::max(st.box.width(), st.box.height()) >= 0.5f * textSize;
  };
  std::vector<float> majorHeights;
  for (const StrokeInfo& st : strokes)
    if (isMajor(st)) majorHeights.push_back(st.box.height());
  const float T = std::max(median(majorHeights), 0.5f * textSize);  // typical stroke height
  const float maxDrift = std::tan(rad(15.0f));
  std::vector<size_t> byX(strokes.size());
  std::iota(byX.begin(), byX.end(), size_t{0});
  std::sort(byX.begin(), byX.end(), [&](size_t a, size_t b) { return strokes[a].box.minX < strokes[b].box.minX; });
  UnionFind uf(strokes.size());
  for (size_t ii = 0; ii < byX.size(); ++ii) {
    const StrokeInfo& a = strokes[byX[ii]];
    if (!isMajor(a)) continue;
    for (size_t jj = ii + 1; jj < byX.size(); ++jj) {
      const StrokeInfo& b = strokes[byX[jj]];
      float gap = b.box.minX - a.box.maxX;
      if (gap > 2.5f * T) break;  // sorted by minX: nothing further can be near
      if (!isMajor(b)) continue;
      if (std::fabs(a.midY - b.midY) <= 0.75f * T + std::max(gap, 0.0f) * maxDrift) uf.unite(byX[ii], byX[jj]);
    }
  }
  // Minor strokes (dots, short dashes) follow the nearest major stroke.
  bool anyMajor = false;
  for (size_t si = 0; si < strokes.size(); ++si) anyMajor = anyMajor || isMajor(strokes[si]);
  for (size_t si = 0; si < strokes.size() && anyMajor; ++si) {
    if (isMajor(strokes[si])) continue;
    size_t bestJ = si;
    float bestD = 1e30f;
    for (size_t sj = 0; sj < strokes.size(); ++sj) {
      if (!isMajor(strokes[sj])) continue;
      const Rect& r = strokes[sj].box;
      Vec2 mc = strokes[si].box.center();
      float dx = std::max({r.minX - mc.x, 0.0f, mc.x - r.maxX});
      float dy = std::max({r.minY - mc.y, 0.0f, mc.y - r.maxY});
      float d = dx * dx + dy * dy;
      if (d < bestD) { bestD = d; bestJ = sj; }
    }
    uf.unite(si, bestJ);
  }
  std::vector<Line> lines;
  {
    std::vector<long> lineOf(strokes.size(), -1);
    for (size_t si : byX) {
      size_t root = uf.find(si);
      if (lineOf[root] < 0) {
        lineOf[root] = static_cast<long>(lines.size());
        lines.push_back({});
      }
      lines[static_cast<size_t>(lineOf[root])].members.push_back(si);
    }
  }

  // ---- 4. Per line: residual slant + word leveling. ----
  std::vector<std::pair<StrokeId, Affine>> transforms;
  for (Line& line : lines) {
    if (line.members.empty()) continue;
    std::vector<float> heights;
    Rect lineBox;
    for (size_t si : line.members) {
      if (isMajor(strokes[si])) heights.push_back(strokes[si].box.height());
      lineBox.expand(strokes[si].box);
    }
    const float lineH = std::max(median(heights), 0.5f * textSize);

    // Words: strokes that overlap or nearly touch horizontally.
    std::vector<size_t> lineByX = line.members;  // already sorted by minX
    std::vector<std::vector<size_t>> words;
    float curMax = -1e30f;
    for (size_t si : lineByX) {
      if (words.empty() || strokes[si].box.minX > curMax + 0.8f * lineH) {
        words.push_back({si});
        curMax = strokes[si].box.maxX;
      } else {
        words.back().push_back(si);
        curMax = std::max(curMax, strokes[si].box.maxX);
      }
    }
    struct Word {
      Vec2 center;
      float ink;
    };
    std::vector<Word> wc;
    for (const auto& word : words) {
      std::vector<std::pair<float, float>> ys;
      float xs = 0.0f, ink = 0.0f;
      for (size_t si : word) {
        ys.insert(ys.end(), ysPerStroke[si].begin(), ysPerStroke[si].end());
        xs += strokes[si].box.center().x * strokes[si].ink;
        ink += strokes[si].ink;
      }
      wc.push_back({{xs / std::max(ink, 1e-6f), weightedMedian(ys)}, ink});
    }

    // Residual slant of this line (weighted least squares through word centres).
    float theta = 0.0f;
    Vec2 pivot = lineBox.center();
    if (wc.size() >= 2 && lineBox.width() >= 2.5f * lineH) {
      float sw = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
      for (const Word& wd : wc) {
        sw += wd.ink; sx += wd.ink * wd.center.x; sy += wd.ink * wd.center.y;
        sxx += wd.ink * wd.center.x * wd.center.x; sxy += wd.ink * wd.center.x * wd.center.y;
      }
      float den = sw * sxx - sx * sx;
      if (std::fabs(den) > 1e-9f) {
        float slope = (sw * sxy - sx * sy) / den;
        theta = clamp(std::atan(slope), -maxA, maxA);
        pivot = {sx / sw, sy / sw};
      }
    }
    const float lc = std::cos(-theta), ls = std::sin(-theta);
    std::vector<std::pair<float, float>> centres;
    for (Word& wd : wc) {
      wd.center = rotateAround(wd.center, pivot, lc, ls);
      centres.push_back({wd.center.y, wd.ink});
    }
    const float target = weightedMedian(centres);

    for (size_t wi = 0; wi < words.size(); ++wi) {
      float dy = params.levelWords ? clamp(target - wc[wi].center.y, -lineH, lineH) : 0.0f;
      // Full transform in page space: deskew about c, line slant about pivot
      // (expressed back in page space), then level along the line normal.
      Affine toLine = Affine::translate(c.x, c.y) * Affine::rotate(-alpha) * Affine::translate(-c.x, -c.y);
      Affine slant = Affine::translate(pivot.x, pivot.y) * Affine::rotate(-theta) * Affine::translate(-pivot.x, -pivot.y);
      Affine full = Affine::translate(0.0f, dy) * slant * toLine;
      const float phi = -(alpha + theta);
      for (size_t si : words[wi]) {
        const Stroke& st = *strokes[si].stroke;
        Vec2 cs0 = st.bounds.center();
        Vec2 moved = full.apply(cs0);
        // Partial strength: interpolate the rigid motion (angle + translation).
        Vec2 target0 = lerp(cs0, moved, k);
        Affine t = Affine::translate(target0.x, target0.y) * Affine::rotate(phi * k) * Affine::translate(-cs0.x, -cs0.y);
        if (std::fabs(phi * k) < rad(0.01f) && distance(target0, cs0) < 1e-3f) continue;
        transforms.push_back({st.id, t});
      }
    }
  }
  if (transforms.empty()) return none;
  return transformEach(page, layerId, transforms);
}

}  // namespace ink
