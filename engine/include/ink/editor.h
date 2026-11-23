#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ink/correction.h"
#include "ink/eraser.h"
#include "ink/history.h"
#include "ink/page.h"
#include "ink/selection.h"
#include "ink/stroke_builder.h"

namespace ink {

/// High-level editing façade for one page: owns the page and its undo history,
/// and accumulates the page-space region that needs redrawing.
///
/// Typical frame loop on the app side:
///   pen down/move/up      -> StrokeBuilder (live) -> commitStroke() on pen up
///   eraser down/move/up   -> beginErase() / eraseTo() / endErase()
///   every frame           -> takeDirty() -> re-render that region
class PageEditor {
 public:
  explicit PageEditor(float width = 595.0f, float height = 842.0f);
  explicit PageEditor(Page page);

  Page& page() { return page_; }
  const Page& page() const { return page_; }
  History& history() { return history_; }

  LayerId activeLayer() const { return activeLayer_; }
  bool setActiveLayer(LayerId id);
  LayerId addLayer(const std::string& name);

  /// Adds a finished stroke on top of the active layer (assigns its id).
  StrokeId commitStroke(Stroke stroke);
  StrokeId commitStroke(const StrokeBuilder& builder) { return commitStroke(builder.buildStroke()); }

  // --- Eraser gesture (one undo step per gesture) ---
  void beginErase(const EraserOptions& opts, Vec2 p);
  /// Returns true if anything was erased by this move.
  bool eraseTo(Vec2 p);
  void endErase();
  bool erasing() const { return erasing_; }

  // --- Selection ---
  std::vector<StrokeId> selectLasso(const std::vector<Vec2>& lasso, float minInsideFraction = 0.6f) const;
  bool transform(const std::vector<StrokeId>& ids, const Affine& t);
  bool recolor(const std::vector<StrokeId>& ids, Color color);
  bool remove(const std::vector<StrokeId>& ids);
  std::vector<StrokeId> duplicate(const std::vector<StrokeId>& ids, Vec2 offset);
  void clearLayer();

  // --- 글씨 보정 on existing strokes (one undo step each) ---
  /// Tidies the strokes (smoothing, straight segments, exact ㅡ/ㅣ).
  bool correct(const std::vector<StrokeId>& ids, const CorrectionParams& params, float unitsPerViewPoint = 1.0f);
  /// Straightens written lines: removes slant, levels drifting words.
  bool straighten(const std::vector<StrokeId>& ids, const StraightenParams& params = {});

  bool canUndo() const { return history_.canUndo(); }
  bool canRedo() const { return history_.canRedo(); }
  bool undo();
  bool redo();

  /// Region (page units) changed since the last call; empty if nothing changed.
  Rect takeDirty();
  void markDirty(const Rect& r) { dirty_.expand(r); }

  std::vector<uint8_t> save() const;
  bool load(const uint8_t* data, size_t size, std::string* error = nullptr);

 private:
  bool record(ChangeSet cs, const std::string& label);

  Page page_;
  History history_;
  LayerId activeLayer_ = 0;
  Rect dirty_;
  bool erasing_ = false;
  EraserOptions eraserOpts_;
  Vec2 eraserPos_;
};

}  // namespace ink
