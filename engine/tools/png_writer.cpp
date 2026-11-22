#include "png_writer.h"

#include <cstdio>

namespace {

uint32_t crcTable[256];
bool crcInit = false;

uint32_t crc32(const uint8_t* data, size_t n, uint32_t crc = 0xFFFFFFFFu) {
  if (!crcInit) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      crcTable[i] = c;
    }
    crcInit = true;
  }
  for (size_t i = 0; i < n; ++i) crc = crcTable[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  return crc;
}

void be32(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back(static_cast<uint8_t>(x >> 24));
  v.push_back(static_cast<uint8_t>(x >> 16));
  v.push_back(static_cast<uint8_t>(x >> 8));
  v.push_back(static_cast<uint8_t>(x));
}

void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
  be32(out, static_cast<uint32_t>(data.size()));
  std::vector<uint8_t> td(type, type + 4);
  td.insert(td.end(), data.begin(), data.end());
  out.insert(out.end(), td.begin(), td.end());
  be32(out, crc32(td.data(), td.size()) ^ 0xFFFFFFFFu);
}

}  // namespace

bool writePng(const std::string& path, const uint8_t* rgba, int width, int height) {
  std::vector<uint8_t> raw;
  raw.reserve(static_cast<size_t>(height) * (width * 4 + 1));
  for (int y = 0; y < height; ++y) {
    raw.push_back(0);  // filter: none
    raw.insert(raw.end(), rgba + static_cast<size_t>(y) * width * 4, rgba + static_cast<size_t>(y + 1) * width * 4);
  }
  // zlib stream with stored (uncompressed) deflate blocks.
  std::vector<uint8_t> z{0x78, 0x01};
  size_t pos = 0;
  uint32_t a = 1, b = 0;
  for (uint8_t c : raw) {
    a = (a + c) % 65521u;
    b = (b + a) % 65521u;
  }
  while (pos < raw.size() || raw.empty()) {
    size_t n = std::min<size_t>(65535, raw.size() - pos);
    bool last = pos + n >= raw.size();
    z.push_back(last ? 1 : 0);
    z.push_back(static_cast<uint8_t>(n));
    z.push_back(static_cast<uint8_t>(n >> 8));
    z.push_back(static_cast<uint8_t>(~n));
    z.push_back(static_cast<uint8_t>(~n >> 8));
    z.insert(z.end(), raw.begin() + static_cast<long>(pos), raw.begin() + static_cast<long>(pos + n));
    pos += n;
    if (last) break;
  }
  be32(z, (b << 16) | a);

  std::vector<uint8_t> png{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  std::vector<uint8_t> ihdr;
  be32(ihdr, static_cast<uint32_t>(width));
  be32(ihdr, static_cast<uint32_t>(height));
  ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});  // 8-bit RGBA
  chunk(png, "IHDR", ihdr);
  chunk(png, "IDAT", z);
  chunk(png, "IEND", {});
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  bool ok = std::fwrite(png.data(), 1, png.size(), f) == png.size();
  std::fclose(f);
  return ok;
}
