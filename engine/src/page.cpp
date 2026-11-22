#include "ink/page.h"

#include <algorithm>

namespace ink {

// ---------------------------------------------------------------------------
// Layer
// ---------------------------------------------------------------------------

void Layer::insert(size_t index, StrokePtr stroke) {
  index = std::min(index, strokes_.size());
  Rect b = stroke ? stroke->bounds : Rect();
  strokes_.insert(strokes_.begin() + static_cast<long>(index), std::move(stroke));
  bounds_.insert(bounds_.begin() + static_cast<long>(index), b);
}

StrokePtr Layer::removeAt(size_t index) {
  if (index >= strokes_.size()) return nullptr;
  StrokePtr s = std::move(strokes_[index]);
  strokes_.erase(strokes_.begin() + static_cast<long>(index));
  bounds_.erase(bounds_.begin() + static_cast<long>(index));
  return s;
}

long Layer::indexOf(StrokeId id) const {
  for (size_t i = strokes_.size(); i-- > 0;)
    if (strokes_[i]->id == id) return static_cast<long>(i);
  return -1;
}

void Layer::query(const Rect& r, std::vector<size_t>& out) const {
  out.clear();
  if (r.isEmpty()) return;
  for (size_t i = 0; i < bounds_.size(); ++i) {
    const Rect& b = bounds_[i];
    if (b.minX <= r.maxX && r.minX <= b.maxX && b.minY <= r.maxY && r.minY <= b.maxY) out.push_back(i);
  }
}

Rect Layer::contentBounds() const {
  Rect r;
  for (const Rect& b : bounds_) r.expand(b);
  return r;
}

// ---------------------------------------------------------------------------
// Page
// ---------------------------------------------------------------------------

Page::Page(float w, float h) : width(w), height(h) { addLayer("Layer 1"); }

Layer& Page::addLayer(const std::string& name) { return addLayerWithId(nextLayerId_, name); }

Layer& Page::addLayerWithId(LayerId id, const std::string& name) {
  nextLayerId_ = std::max(nextLayerId_, id + 1);
  layers_.emplace_back(id, name);
  return layers_.back();
}

bool Page::removeLayer(LayerId id) {
  auto it = std::find_if(layers_.begin(), layers_.end(), [&](const Layer& l) { return l.id() == id; });
  if (it == layers_.end()) return false;
  layers_.erase(it);
  return true;
}

Layer* Page::layer(LayerId id) {
  for (Layer& l : layers_)
    if (l.id() == id) return &l;
  return nullptr;
}

const Layer* Page::layer(LayerId id) const {
  for (const Layer& l : layers_)
    if (l.id() == id) return &l;
  return nullptr;
}

size_t Page::strokeCount() const {
  size_t n = 0;
  for (const Layer& l : layers_) n += l.size();
  return n;
}

Rect Page::contentBounds() const {
  Rect r;
  for (const Layer& l : layers_) r.expand(l.contentBounds());
  return r;
}

// ---------------------------------------------------------------------------
// ChangeSet
// ---------------------------------------------------------------------------

bool ChangeSet::empty() const {
  for (const LayerChange& lc : layers)
    if (!lc.removed.empty() || !lc.added.empty()) return false;
  return true;
}

Rect ChangeSet::bounds() const {
  Rect r;
  for (const LayerChange& lc : layers) {
    for (const auto& e : lc.removed) r.expand(e.second->bounds);
    for (const auto& e : lc.added) r.expand(e.second->bounds);
  }
  return r;
}

namespace {

/// Removes `items` (ascending indices) and inserts `inserts` (ascending indices).
bool swapStrokes(Layer& layer, const std::vector<std::pair<size_t, StrokePtr>>& items,
                 const std::vector<std::pair<size_t, StrokePtr>>& inserts) {
  // Validate before mutating so a mismatched change cannot corrupt the layer.
  for (const auto& e : items)
    if (e.first >= layer.size() || layer.at(e.first)->id != e.second->id) return false;
  for (size_t i = items.size(); i-- > 0;) layer.removeAt(items[i].first);
  for (const auto& e : inserts) layer.insert(e.first, e.second);
  return true;
}

}  // namespace

bool applyChange(Page& page, const ChangeSet& change) {
  bool ok = true;
  for (const LayerChange& lc : change.layers) {
    Layer* layer = page.layer(lc.layer);
    if (!layer || !swapStrokes(*layer, lc.removed, lc.added)) ok = false;
  }
  return ok;
}

bool revertChange(Page& page, const ChangeSet& change) {
  bool ok = true;
  for (size_t i = change.layers.size(); i-- > 0;) {
    const LayerChange& lc = change.layers[i];
    Layer* layer = page.layer(lc.layer);
    if (!layer || !swapStrokes(*layer, lc.added, lc.removed)) ok = false;
  }
  return ok;
}

}  // namespace ink
