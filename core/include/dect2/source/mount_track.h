// Pass-driven mount plan: what to tell an antenna mount at a moment of a satellite pass.
// Before AOS the mount is pre-positioned at the rise azimuth (elevation 0); during the pass it follows the satellite,
// with the azimuth/elevation speeds for tracking mounts; after LOS it is parked.
#pragma once
#include "dect2/geo/passes.h"

namespace dect2 {

enum class MountPhase { Idle, Preposition, Track, Park };

struct MountPlan {
    MountPhase phase = MountPhase::Idle;
    double azDeg = 0, elDeg = 0;            // true (corrected) direction to command
    double azRateDegS = 0, elRateDegS = 0;  // satellite angular speed (deg/s), for mounts that take a speed
    double minutesToAos = 0;
};

// now = UNIX seconds. prepositionSec: how long before AOS to start moving. parkAfterSec: how long after LOS to give the order to park (0 = never).
// Elevation below minElDeg during the pass is not followed (the mount waits at the last good aim).
MountPlan planMountForPass(const Sgp4& sat, const Observer& obs, const Pass& pass, double now,
                           double prepositionSec = 90, double parkAfterSec = 5, double minElDeg = 0);

// Wrapped azimuth difference to - from, in (-180, 180]
double azDelta(double fromDeg, double toDeg);

} // namespace dect2
