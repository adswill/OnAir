// Where a click on the spectrum puts the dial. The spectrum axis is the radio's real RF (centre = dial + the mode's tune offset), so a
// signal at the clicked frequency is the dial itself: the radio then sits tuneOffsetHz above it (Engine::radioTune adds the offset).
// Subtracting the offset here put the dial 250 kHz (ISM) below the signal that was clicked (issue #219).
#pragma once
namespace dect2 {
inline double dialMhzFromClick(double clickedMhz, double /*tuneOffsetHz*/) { return clickedMhz; }
inline double radioMhzForDial(double dialMhz, double tuneOffsetHz) { return dialMhz + tuneOffsetHz / 1e6; }
}
