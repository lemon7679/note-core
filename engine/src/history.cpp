#include "ink/history.h"

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

void History::beginGroup(const std::string& label) {
  if (grouping_) endGroup();
  grouping_ = true;
  group_ = Entry{};
  group_.label = label;
}

void History::endGroup() {
  if (!grouping_) return;
  grouping_ = false;
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
