// Internal structures shared by the C API (c_api.cpp) and the optional GPU
// renderer (engine/gpu). Not part of the public interface.
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "ink/geometry.h"
#include "ink/page.h"

namespace ink {
namespace capi {

/// Tessellation zoom bucket: re-tessellate only when zoom crosses a power of
/// two; geometry tessellated for a finer level is reused (slightly denser than
/// needed, never coarser).
inline float ppuBucket(float ppu) {
  ppu = clamp(ppu, 0.125f, 128.0f);
  return std::exp2(std::ceil(std::log2(ppu)));
}

/// Tessellated geometry shared by an editor and its snapshots. Thread-safe:
/// tile renderers on worker threads look strokes up while the main thread edits.
/// Two zoom buckets per stroke (e.g. screen tiles and a small preview) so
/// rendering both does not keep re-tessellating.
struct GeometryCache {
  struct Entry {
    StrokePtr stroke;
    float bucket = 0.0f;
    std::shared_ptr<const StrokeGeometry> geom;
    uint64_t used = 0;
  };
  std::mutex mutex;
  std::unordered_map<StrokeId, std::array<Entry, 2>> entries;
  uint64_t clock = 0;

  std::shared_ptr<const StrokeGeometry> get(const StrokePtr& s, float bucket) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      auto it = entries.find(s->id);
      if (it != entries.end())
        for (Entry& e : it->second)
          if (e.stroke == s && e.bucket == bucket) {
            e.used = ++clock;
            return e.geom;
          }
    }
    // Tessellate outside the lock; strokes are immutable.
    auto g = std::make_shared<StrokeGeometry>();
    TessellationOptions opts;
    opts.pixelsPerUnit = bucket;
    Tessellator::tessellate(*s, opts, *g);
    std::lock_guard<std::mutex> lock(mutex);
    std::array<Entry, 2>& slots = entries[s->id];
    Entry* victim = nullptr;
    for (Entry& e : slots) {
      if (e.stroke == s && e.bucket == bucket) return e.geom;  // another thread was faster
      if (!victim && e.stroke != s) victim = &e;              // empty or an old version
    }
    if (!victim) victim = slots[0].used <= slots[1].used ? &slots[0] : &slots[1];
    victim->stroke = s;
    victim->bucket = bucket;
    victim->geom = g;
    victim->used = ++clock;
    return g;
  }

  /// Drops entries of strokes no longer in the page (or replaced by an edit).
  void prune(const std::unordered_map<StrokeId, StrokePtr>& live) {
    std::lock_guard<std::mutex> lock(mutex);
    for (auto it = entries.begin(); it != entries.end();) {
      auto f = live.find(it->first);
      bool keep = false;
      if (f != live.end())
        for (Entry& e : it->second) {
          if (e.stroke && e.stroke != f->second) e = Entry{};
          keep = keep || e.stroke != nullptr;
        }
      it = keep ? std::next(it) : entries.erase(it);
    }
  }

  void clear() {
    std::lock_guard<std::mutex> lock(mutex);
    entries.clear();
  }
};

/// Immutable state of a page for rendering / saving off the editor's thread.
struct SnapshotData {
  Page page;
  std::unordered_set<StrokeId> hidden;
  std::shared_ptr<GeometryCache> cache;
};


}  // namespace capi
}  // namespace ink

struct InkSnapshot {
  std::shared_ptr<const ink::capi::SnapshotData> data;
};

/// Copy of a builder's live stroke (stable + tail geometry), safe to hand to
/// another thread (e.g. a GPU render thread).
struct InkLiveFrame {
  ink::StrokeStyle style;
  ink::StrokeGeometry geometry;
  ink::Rect region;  ///< Page units; empty = whole stroke.
};
