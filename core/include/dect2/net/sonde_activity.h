// Radiosonde activity (#526, Discord DG9BFC): the balloon uploaders (the APRS-IS aggregators wettersonde.net / radiosondy.info and the SondeHub
// station record) must show the receiver as active only while a radiosonde mode decodes. The decoder notes every frame here; the uploaders
// connect and beacon only while a frame came in during the last kActiveWindowS seconds, and drop their connection when it stays quiet.
// Settings stay as the user made them: ticking a target still means "upload whenever a sonde is decoded".
#pragma once
#include "dect2/common/timecompat.h"
#include <atomic>

namespace dect2 {
namespace sondeactivity {

constexpr double kActiveWindowS = 120.0;

inline std::atomic<double>& lastFrameSteady() { static std::atomic<double> t{-1e18}; return t; }
inline void noteFrameAt(double steadyNow) { lastFrameSteady().store(steadyNow, std::memory_order_relaxed); }
inline void noteFrame() { noteFrameAt(steadyNowSec()); }
inline bool activeAt(double steadyNow, double windowS = kActiveWindowS) { return steadyNow - lastFrameSteady().load(std::memory_order_relaxed) <= windowS; }
inline bool active() { return activeAt(steadyNowSec()); }

}  // namespace sondeactivity
}  // namespace dect2
