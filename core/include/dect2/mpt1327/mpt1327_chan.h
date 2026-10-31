// MPT 1327 channel number -> frequency. The control channel names traffic channels by number only; the frequency follows from the control channel's own
// frequency and number and the channel spacing when the plan is contiguous (a duplex gap or a split plan needs the right control channel number).
// Header only, receive-side helper for the channel table (docs/TRUNK-TRACKING.md).
#pragma once
#include <cmath>

namespace dect2 {
namespace mpt1327 {

// Frequency of `channel` given the control channel's frequency and channel number. Returns 0 when the inputs cannot give one (negative numbers,
// spacing <= 0, a result at or below 0 Hz).
inline double channelFreqHz(int channel, int controlChannel, double controlHz, double spacingHz) {
    if (channel < 0 || controlChannel < 0 || !(spacingHz > 0) || !(controlHz > 0)) return 0;
    const double f = controlHz + (double)(channel - controlChannel) * spacingHz;
    return f > 0 ? f : 0;
}

} // namespace mpt1327
} // namespace dect2
