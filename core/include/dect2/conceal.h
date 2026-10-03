// Picture concealment for live playback: when a deep fade swallows a few hundred milliseconds of the stream (or damages the pictures
// after it), the missing pictures are synthesised from the pictures on both sides (Apple ML interpolation, or a motion search).
// Nothing here recovers the lost data: it makes the loss less visible.
#pragma once
#include "player.h"
#include <cstdint>
#include <memory>
#include <random>
#include <vector>

namespace dect2 {

// Interpolates `count` pictures between `a` and `b` (NV12, same size). t of picture k is (k+1)/(count+1). Returns false when the
// pictures cannot be used (different size, RGBA overlay, empty planes).
bool interpolateGap(const VideoFrame& a, const VideoFrame& b, int count, std::vector<std::shared_ptr<VideoFrame>>& out);

// macOS: Apple's machine-learning frame interpolation (VideoToolbox, macOS 15.4+). Elsewhere these are stubs that return false.
bool appleInterpolationAvailable();
bool interpolateGapApple(const VideoFrame& a, const VideoFrame& b, int count, std::vector<std::shared_ptr<VideoFrame>>& out);
// Name of the interpolation that interpolateGap() uses on this machine ("Apple ML" or "motion search").
const char* interpolationBackend();
// Loads the machine-learning model for pictures of this size in the background, so that the first real gap is not delayed.
void prepareInterpolation(int w, int h);

} // namespace dect2
