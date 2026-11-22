#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "ink/stroke.h"

namespace ink {

using LayerId = uint32_t;

/// Ordered stroke list (z-order = index). Strokes are immutable and shared
/// (StrokePtr), so undo snapshots and render threads never deep-copy ink.
class Layer {
 public:
  explicit Layer(LayerId id = 0, std::string layerName = {}) : name(std::move(layerName)), id_(id) {}

  std::string name;
  bool visible = true;
  bool locked = false;
  float opacity = 1.0f;

  LayerId id() const { return id_; }
  size_t size() const { return strokes_.size(); }
  bool empty() const { return strokes_.empty(); }
  const StrokePtr& at(size_t i) const { return strokes_[i]; }
  const std::vector<StrokePtr>& strokes() const { return strokes_; }

  void insert(size_t index, StrokePtr stroke);
  void append(StrokePtr stroke) { insert(strokes_.size(), std::move(stroke)); }
  StrokePtr removeAt(size_t index);
  void clear() { strokes_.clear(); bounds_.clear(); }

  /// Index of a stroke id, or -1. Searches newest-first (recent strokes are hit most).
  long indexOf(StrokeId id) const;

  /// Indices (ascending z-order) of strokes whose bounds intersect `r`.
  /// A tight array of bounds is scanned linearly: ~10 µs for 10k strokes, which
  /// beats tree maintenance for page-sized documents.
  void query(const Rect& r, std::vector<size_t>& out) const;
  Rect contentBounds() const;

 private:
  LayerId id_;
  std::vector<StrokePtr> strokes_;
  std::vector<Rect> bounds_;  // parallel to strokes_, cache-friendly culling
};

class Page {
 public:
  /// Default: A4 portrait in points.
  explicit Page(float width = 595.0f, float height = 842.0f);

  float width;
  float height;

  std::vector<Layer>& layers() { return layers_; }
  const std::vector<Layer>& layers() const { return layers_; }
  Layer& addLayer(const std::string& name);
  /// Inserts a layer with a known id (deserialization).
  Layer& addLayerWithId(LayerId id, const std::string& name);
  bool removeLayer(LayerId id);
  Layer* layer(LayerId id);
  const Layer* layer(LayerId id) const;

  StrokeId allocateStrokeId() { return nextStrokeId_++; }
  StrokeId nextStrokeId() const { return nextStrokeId_; }
  void setNextStrokeId(StrokeId id) { nextStrokeId_ = id; }

  size_t strokeCount() const;
  Rect contentBounds() const;

 private:
  std::vector<Layer> layers_;
  StrokeId nextStrokeId_ = 1;
  LayerId nextLayerId_ = 1;
};

/// A reversible edit of one layer.
///   removed: strokes taken out, with their indices in the layer *before* the edit (ascending)
///   added:   strokes put in, with their indices in the layer *after* the edit (ascending)
/// Forward = remove `removed` (descending) then insert `added` (ascending).
/// Backward = the mirror image. This one shape covers add, delete, partial
/// erase (1 stroke -> N pieces in place), transform and recolor.
struct LayerChange {
  LayerId layer = 0;
  std::vector<std::pair<size_t, StrokePtr>> removed;
  std::vector<std::pair<size_t, StrokePtr>> added;
};

struct ChangeSet {
  std::vector<LayerChange> layers;

  bool empty() const;
  /// Page-space area touched by the change (for redraw).
  Rect bounds() const;
};

/// Applies / reverts a change. Returns false (and leaves the page untouched as
/// far as possible) if the page does not match the change's expectations.
bool applyChange(Page& page, const ChangeSet& change);
bool revertChange(Page& page, const ChangeSet& change);

}  // namespace ink
