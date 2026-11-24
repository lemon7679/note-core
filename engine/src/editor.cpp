#include "ink/editor.h"

#include <memory>

#include "ink/serialization.h"

namespace ink {

PageEditor::PageEditor(float width, float height) : page_(width, height) {
  activeLayer_ = page_.layers().front().id();
}

PageEditor::PageEditor(Page page) : page_(std::move(page)) {
  if (page_.layers().empty()) page_.addLayer("Layer 1");
  activeLayer_ = page_.layers().front().id();
}

bool PageEditor::setActiveLayer(LayerId id) {
  if (!page_.layer(id)) return false;
  activeLayer_ = id;
  return true;
}

LayerId PageEditor::addLayer(const std::string& name) {
  LayerId id = page_.addLayer(name).id();
  activeLayer_ = id;
  return id;
}

bool PageEditor::record(ChangeSet cs, const std::string& label) {
  if (cs.empty()) return false;
  dirty_.expand(cs.bounds());
  history_.push(std::move(cs), label);
  return true;
}

StrokeId PageEditor::commitStroke(Stroke stroke) {
  Layer* layer = page_.layer(activeLayer_);
  if (!layer || layer->locked || stroke.points.empty()) return 0;
  auto s = std::make_shared<Stroke>(std::move(stroke));
  s->id = page_.allocateStrokeId();
  s->updateBounds();
  StrokeId id = s->id;
  ChangeSet cs;
  LayerChange lc;
  lc.layer = activeLayer_;
  lc.added.emplace_back(layer->size(), std::move(s));
  cs.layers.push_back(std::move(lc));
  applyChange(page_, cs);
  record(std::move(cs), "Draw");
  return id;
}

void PageEditor::beginErase(const EraserOptions& opts, Vec2 p) {
  if (erasing_) endErase();
  erasing_ = true;
  eraserOpts_ = opts;
  eraserPos_ = p;
  history_.beginGroup("Erase", &page_);
  eraseTo(p);  // a tap erases too
}

bool PageEditor::eraseTo(Vec2 p) {
  if (!erasing_) return false;
  ChangeSet cs = eraseAlong(page_, activeLayer_, eraserPos_, p, eraserOpts_);
  eraserPos_ = p;
  if (cs.empty()) return false;
  dirty_.expand(cs.redrawBounds());
  history_.push(std::move(cs), "Erase");
  return true;
}

void PageEditor::endErase() {
  if (!erasing_) return;
  erasing_ = false;
  history_.endGroup(&page_);
}

std::vector<StrokeId> PageEditor::selectLasso(const std::vector<Vec2>& lasso, float minInsideFraction) const {
  const Layer* layer = page_.layer(activeLayer_);
  return layer ? lassoSelect(*layer, lasso, minInsideFraction) : std::vector<StrokeId>{};
}

bool PageEditor::transform(const std::vector<StrokeId>& ids, const Affine& t) {
  return record(transformStrokes(page_, activeLayer_, ids, t), "Transform");
}

bool PageEditor::recolor(const std::vector<StrokeId>& ids, Color color) {
  return record(recolorStrokes(page_, activeLayer_, ids, color), "Recolor");
}

bool PageEditor::remove(const std::vector<StrokeId>& ids) {
  return record(deleteStrokes(page_, activeLayer_, ids), "Delete");
}

std::vector<StrokeId> PageEditor::duplicate(const std::vector<StrokeId>& ids, Vec2 offset) {
  ChangeSet cs = duplicateStrokes(page_, activeLayer_, ids, offset);
  std::vector<StrokeId> out;
  for (const LayerChange& lc : cs.layers)
    for (const auto& e : lc.added) out.push_back(e.second->id);
  record(std::move(cs), "Duplicate");
  return out;
}

std::vector<StrokeId> PageEditor::addStrokes(const std::vector<Stroke>& strokes, Vec2 offset) {
  std::vector<StrokeId> out;
  Layer* layer = page_.layer(activeLayer_);
  if (!layer || layer->locked) return out;
  ChangeSet cs;
  LayerChange lc;
  lc.layer = activeLayer_;
  size_t at = layer->size();
  for (const Stroke& src : strokes) {
    if (src.points.empty()) continue;
    auto copy = std::make_shared<Stroke>(src);
    copy->id = page_.allocateStrokeId();
    for (StrokePoint& p : copy->points) p.pos += offset;
    copy->updateBounds();
    out.push_back(copy->id);
    lc.added.emplace_back(at++, std::move(copy));
  }
  if (lc.added.empty()) return out;
  cs.layers.push_back(std::move(lc));
  applyChange(page_, cs);
  record(std::move(cs), "Paste");
  return out;
}

Rect PageEditor::boundsOf(const std::vector<StrokeId>& ids) const {
  Rect r;
  for (const Layer& layer : page_.layers())
    for (StrokeId id : ids) {
      long i = layer.indexOf(id);
      if (i >= 0) r.expand(layer.at(static_cast<size_t>(i))->bounds);
    }
  return r;
}

void PageEditor::clearLayer() {
  Layer* layer = page_.layer(activeLayer_);
  if (!layer) return;
  std::vector<StrokeId> ids;
  for (const StrokePtr& s : layer->strokes()) ids.push_back(s->id);
  record(deleteStrokes(page_, activeLayer_, ids), "Clear");
}

bool PageEditor::correct(const std::vector<StrokeId>& ids, const CorrectionParams& params, float unitsPerViewPoint) {
  return record(correctStrokes(page_, activeLayer_, ids, params, unitsPerViewPoint), "Correct Handwriting");
}

bool PageEditor::straighten(const std::vector<StrokeId>& ids, const StraightenParams& params) {
  return record(straightenHandwriting(page_, activeLayer_, ids, params), "Straighten Lines");
}

bool PageEditor::undo() {
  if (erasing_) endErase();
  Rect r;
  bool ok = history_.undo(page_, &r);
  dirty_.expand(r);
  return ok;
}

bool PageEditor::redo() {
  if (erasing_) endErase();
  Rect r;
  bool ok = history_.redo(page_, &r);
  dirty_.expand(r);
  return ok;
}

Rect PageEditor::takeDirty() {
  Rect r = dirty_;
  dirty_ = Rect();
  return r;
}

std::vector<uint8_t> PageEditor::save() const { return serializePage(page_); }

bool PageEditor::load(const uint8_t* data, size_t size, std::string* error) {
  Page p;
  if (!deserializePage(data, size, p, error)) return false;
  page_ = std::move(p);
  history_.clear();
  activeLayer_ = page_.layers().front().id();
  erasing_ = false;
  dirty_ = Rect(0.0f, 0.0f, page_.width, page_.height);
  dirty_.expand(page_.contentBounds());
  return true;
}

}  // namespace ink
