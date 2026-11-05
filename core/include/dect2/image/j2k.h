// JPEG 2000 (ISO 15444-1) decoder for the pictures of the GK-2A xRIT files (lossless, reversible 5/3, one component). Written from the
// standard; no third-party code. Also handles the 9/7 irreversible transform, several tiles, layers and the five progression orders.
// Not handled: the selective arithmetic-coding bypass style, packed packet headers (PPM / PPT), region-of-interest shifts. The decoder
// says so in `error` instead of returning a wrong picture.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2::j2k {

struct Image {
    bool ok = false;
    std::string error;
    int width = 0, height = 0;        // of the picture (the reference grid minus the offset)
    int precision = 0;                // bits of component 0
    int components = 0;
    int levels = 0, layers = 0;       // decomposition levels and quality layers of the first tile
    bool reversible = false;
    std::vector<uint16_t> pix;        // component 0, width x height, unsigned, clipped to the precision
};

// Offset of the codestream (the SOC marker) inside raw bytes or a JP2 file; false when there is none
bool findCodestream(const uint8_t* d, size_t n, size_t& off);
// Decodes component 0. A damaged stream returns whatever was decodable (ok false, pix filled where possible when the headers were fine).
Image decode(const uint8_t* d, size_t n);

// A lossless codestream (one tile, one component, reversible 5/3 with `levels` decomposition levels, 64 x 64 code-blocks, one layer, LRCP):
// the test signals of the GK-2A mode send their pictures with it. Empty when the arguments are out of range.
std::vector<uint8_t> encodeLossless(const uint16_t* pix, int width, int height, int precision, int levels);

} // namespace dect2::j2k
