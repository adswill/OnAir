// PNG decoding without any library, for the map tiles on systems where OnAir has no picture decoder of the system to call (Windows, Linux).
#pragma once
#include <cstdint>
#include <vector>

namespace dect2 {

// Every PNG colour type and bit depth, plain or interlaced, with tRNS transparency. rgba gets w*h pixels as bytes R, G, B, A with the
// colour premultiplied by alpha (what the macOS decoder gives too). False on anything that is not a valid PNG.
bool decodePng(const uint8_t* data, size_t size, int& w, int& h, std::vector<uint8_t>& rgba);

} // namespace dect2
