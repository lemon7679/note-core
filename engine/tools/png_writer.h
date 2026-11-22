// Minimal dependency-free PNG writer (stored deflate blocks) for the demo tool.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

bool writePng(const std::string& path, const uint8_t* rgba, int width, int height);
