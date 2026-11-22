#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "ink/page.h"

namespace ink {

/// Undo / redo stack of ChangeSets. A gesture that produces many changes (an
/// eraser drag) is grouped into one entry with beginGroup()/endGroup().
class History {
 public:
  explicit History(size_t limit = 300) : limit_(limit) {}

  /// Records an already-applied change.
  void push(ChangeSet change, const std::string& label = {});
  void beginGroup(const std::string& label);
  void endGroup();
  bool grouping() const { return grouping_; }

  bool canUndo() const { return !undo_.empty(); }
  bool canRedo() const { return !redo_.empty(); }
  size_t undoCount() const { return undo_.size(); }
  size_t redoCount() const { return redo_.size(); }
  const std::string& undoLabel() const;
  const std::string& redoLabel() const;

  /// `dirty` receives the page area to redraw.
  bool undo(Page& page, Rect* dirty = nullptr);
  bool redo(Page& page, Rect* dirty = nullptr);
  void clear();

 private:
  struct Entry {
    std::vector<ChangeSet> changes;
    std::string label;
  };
  void commit(Entry entry);

  size_t limit_;
  std::vector<Entry> undo_;
  std::vector<Entry> redo_;
  bool grouping_ = false;
  Entry group_;
};

}  // namespace ink
