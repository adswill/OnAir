// Where a click on the spectrum puts the dial. The spectrum axis is the radio's real RF (centre = dial + the mode's tune offset), so a
// signal at the clicked frequency is the dial itself: the radio then sits tuneOffsetHz above it (Engine::radioTune adds the offset).
// Subtracting the offset here put the dial 250 kHz (ISM) below the signal that was clicked (issue #219).
#pragma once
namespace dect2 {
inline double dialMhzFromClick(double clickedMhz, double /*tuneOffsetHz*/) { return clickedMhz; }
inline double radioMhzForDial(double dialMhz, double tuneOffsetHz) { return dialMhz + tuneOffsetHz / 1e6; }

// Sideband audio modes (the HF digital keyboard modes: Hellschreiber, Olivia, MFSK, ...), as fldigi: the signal sits at dial + audio (USB) or dial - audio
// (LSB). A click on a signal inside the audio passband keeps the dial and moves the decoder's audio frequency onto it; a click outside it moves the dial so
// that the clicked signal lands on the audio frequency the decoder uses now. Before this a click made the clicked signal the dial, so a Hell signal was
// received only by clicking 1500 Hz below it (issue #225).
struct SidebandClick { bool keepDial; double dialHz; double audioHz; };
inline SidebandClick sidebandClick(double clickHz, double dialHz, bool lsb, double audioHz, double loHz = 300, double hiHz = 3500) {
    const double a = lsb ? dialHz - clickHz : clickHz - dialHz;
    if (a >= loHz && a <= hiHz) return {true, dialHz, a};
    return {false, lsb ? clickHz + audioHz : clickHz - audioHz, audioHz};
}
}
