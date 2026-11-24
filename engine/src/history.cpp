#include "ink/history.h"

#include <unordered_set>

namespace ink {

namespace {
const std::string kEmpty;
}

void History::commit(Entry entry) {
  if (entry.changes.empty()) return;
  undo_.push_back(std::move(entry));
  if (undo_.size() > limit_) undo_.erase(undo_.begin());
  redo_.clear();
}

void History::push(ChangeSet change, const std::string& label) {
  if (change.empty()) return;
  if (grouping_) {
    group_.changes.push_back(std::move(change));
    return;
  }
  Entry e;
  e.changes.push_back(std::move(change));
  e.label = label;
  commit(std::move(e));
}

void History::beginGroup(const std::string& label, const Page* page) {
  if (grouping_) endGroup(page);
  grouping_ = true;
  group_ = Entry{};
  group_.label = label;
  groupStart_.clear();
  if (page)
    for (const Layer& l : page->layers()) groupStart_.emplace_back(l.id(), l.strokes());
}

namespace {

/// Net change from `before` to `after` (same layers, surviving strokes keep
/// their relative order — true for every edit ChangeSet can express).
bool netChange(const std::vector<std::pair<LayerId, std::vector<StrokePtr>>>& before, const Page& page,
               ChangeSet& out) {
  if (before.size() != page.layers().size()) return false;
  for (size_t li = 0; li < before.size(); ++li) {
    const Layer& layer = page.layers()[li];
    if (layer.id() != before[li].first) return false;
    const std::vector<StrokePtr>& a = before[li].second;
    const std::vector<StrokePtr>& b = layer.strokes();
    std::unordered_set<const Stroke*> inA, inB;
    inA.reserve(a.size());
    inB.reserve(b.size());
    for (const StrokePtr& s : a) inA.insert(s.get());
    for (const StrokePtr& s : b) inB.insert(s.get());
    LayerChange lc;
    lc.layer = layer.id();
    for (size_t i = 0; i < a.size(); ++i)
      if (!inB.count(a[i].get())) lc.removed.emplace_back(i, a[i]);
    for (size_t i = 0; i < b.size(); ++i)
      if (!inA.count(b[i].get())) lc.added.emplace_back(i, b[i]);
    if (!lc.removed.empty() || !lc.added.empty()) out.layers.push_back(std::move(lc));
  }
  return true;
}

}  // namespace

void History::endGroup(const Page* page) {
  if (!grouping_) return;
  grouping_ = false;
  if (page && !groupStart_.empty() && group_.changes.size() > 1) {
    ChangeSet net;
    if (netChange(groupStart_, *page, net)) {
      group_.changes.clear();
      if (!net.empty()) group_.changes.push_back(std::move(net));
    }
  }
  groupStart_.clear();
  commit(std::move(group_));
  group_ = Entry{};
}

const std::string& History::undoLabel() const { return undo_.empty() ? kEmpty : undo_.back().label; }
const std::string& History::redoLabel() const { return redo_.empty() ? kEmpty : redo_.back().label; }

bool History::undo(Page& page, Rect* dirty) {
  if (grouping_) endGroup();
  if (undo_.empty()) return false;
  Entry e = std::move(undo_.back());
  undo_.pop_back();
  bool ok = true;
  for (size_t i = e.changes.size(); i-- > 0;) {
    ok = revertChange(page, e.changes[i]) && ok;
    if (dirty) dirty->expand(e.changes[i].bounds());
  }
  redo_.push_back(std::move(e));
  return ok;
}

bool History::redo(Page& page, Rect* dirty) {
  if (grouping_) endGroup();
  if (redo_.empty()) return false;
  Entry e = std::move(redo_.back());
  redo_.pop_back();
  bool ok = true;
  for (const ChangeSet& c : e.changes) {
    ok = applyChange(page, c) && ok;
    if (dirty) dirty->expand(c.bounds());
  }
  undo_.push_back(std::move(e));
  return ok;
}

void History::clear() {
  undo_.clear();
  redo_.clear();
  grouping_ = false;
  group_ = Entry{};
}

}  // namespace ink
