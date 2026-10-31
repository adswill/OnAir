#include "dect2/source/mount_track.h"
#include <cmath>

namespace dect2 {

double azDelta(double from, double to) {
    double d = std::fmod(to - from, 360.0);
    if (d > 180.0) d -= 360.0;
    if (d <= -180.0) d += 360.0;
    return d;
}

MountPlan planMountForPass(const Sgp4& sat, const Observer& obs, const Pass& pass, double now, double preSec, double parkAfterSec, double minEl) {
    MountPlan m;
    m.minutesToAos = (pass.aos - now) / 60.0;
    if (now < pass.aos - preSec) return m;
    if (now < pass.aos) {
        m.phase = MountPhase::Preposition;
        m.azDeg = pass.aosAzDeg;
        m.elDeg = 0;
        return m;
    }
    if (now <= pass.los) {
        Look a, b;
        if (!lookAt(sat, obs, now, a)) return m;
        m.phase = MountPhase::Track;
        m.azDeg = a.azDeg;
        m.elDeg = a.elDeg < minEl ? minEl : a.elDeg;
        if (lookAt(sat, obs, now + 1.0, b)) {
            m.azRateDegS = azDelta(a.azDeg, b.azDeg);
            m.elRateDegS = b.elDeg - a.elDeg;
        }
        return m;
    }
    if (parkAfterSec > 0 && now <= pass.los + parkAfterSec + 120) m.phase = MountPhase::Park;
    return m;
}

} // namespace dect2
