#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ink/page.h"

namespace ink {

/// Compact, versioned binary page format ("INKP").
///
/// * Positions and widths are quantized to 1/100 page unit and delta-encoded as
///   zig-zag varints; alpha / pressure / tilt / azimuth use 8 bits. A typical
///   stroke point costs ~8-11 bytes instead of 36.
/// * Every style block is length-prefixed, so newer writers can append fields
///   and older readers skip them (forward compatible).
/// * Reading is bounds-checked: corrupted input yields an error, never a crash.
std::vector<uint8_t> serializePage(const Page& page);
bool deserializePage(const uint8_t* data, size_t size, Page& out, std::string* error = nullptr);

/// Single stroke (clipboard / drag-and-drop between pages).
std::vector<uint8_t> serializeStroke(const Stroke& stroke);
bool deserializeStroke(const uint8_t* data, size_t size, Stroke& out);

constexpr uint32_t kPageFormatVersion = 1;

}  // namespace ink
