// Internals shared by the picture-repair implementations (conceal.cpp: motion search and the CPU synthesis; conceal_d3d11.cpp: the Windows
// GPU synthesis). Not part of the public interface.
#pragma once
#include "dect2/conceal.h"
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace dect2 {

// Motion between the two real pictures of a gap: one vector per 32x32 block (in units of 4 pixels, from A to B) and the few motions that
// occur most often in the picture (in pixels), which the synthesis tries on every cell.
struct GapMotion {
    int bw = 0, bh = 0;
    std::vector<int8_t> mvx, mvy;
    std::vector<std::pair<int, int>> motions;
};

#ifdef _WIN32
// Direct3D 11 synthesis of the `count` pictures between A and B from a motion field (same algorithm as the CPU synthesis).
bool d3d11ConcealAvailable();                 // a usable discrete or integrated adapter was found and its shader compiled
const char* d3d11ConcealDevice();
bool d3d11Synthesize(const VideoFrame& A, const VideoFrame& B, int count, const GapMotion& gm, std::vector<std::shared_ptr<VideoFrame>>& out);
#endif

} // namespace dect2
