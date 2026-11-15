#pragma once
// The channel box the spectrum draws for DRM, from the Channel filter choice (0 off, 1 10 kHz, 2 20 kHz) and the occupancy the receiver found.
#include <algorithm>

namespace drmfilter {

constexpr double kDefaultMhz = 0.020;   // the nominal width of the DRM tuning entry, shown while nothing else is known

// 10 kHz / 20 kHz: the chosen width. Off: the occupancy found in the signal (detectedKhz > 0), else the default.
inline double boxWidthMhz(int choice, double detectedKhz) {
    if (choice == 1) return 0.010;
    if (choice == 2) return 0.020;
    return detectedKhz > 0 ? detectedKhz / 1000.0 : kDefaultMhz;
}

// The filter's one-sided pass band handed to the receiver (Hz); 0 = filter off.
inline int passHz(int choice) { return choice == 1 ? 7000 : choice == 2 ? 12000 : 0; }

}  // namespace drmfilter
