// CP16 receiver: the picture of the 16 carriers and the characters read from it (cp16_proto.h has the mode).
//
// The channel (the dial frequency) is mixed to 0 Hz and brought to 8 kHz complex, so the audio of either sideband is there: upper sideband carriers at
// +f, lower sideband at -f. Every second the last 20 s are measured again:
//   carriers  a 4096-point spectrum (1.95 Hz bins) averaged over the window; the comb of 16 equally spaced lines (spacing 5 .. 30 Hz) that stands
//             out most against the gaps between its lines; a comb below 0 Hz is a lower-sideband signal (its columns run downwards)
//   rows      each carrier through a box filter one carrier spacing long (zeros on the neighbours), the edges of all 16 envelopes, and the period
//             (25 .. 500 ms) and phase in which the edges line up best; the longest period that lines up is taken (its halves line up too)
// Then each row is the mean of the box-filtered carriers over the row, scaled between the level of the blank dots and that of the inked ones:
// that is the picture (a person reads it, as on any waterfall). The characters are read from it as well: every 16 rows after the first inked
// row are compared with the font's 7445 GB2312 glyphs and the full-width ASCII (cp16_proto.h recognise), at the alignment that fits best. While the
// first characters come in, the receiver also tries the comb up to four carriers lower or higher (a glyph's outer columns are often blank) and
// keeps the arrangement whose characters fit best.
// Under Advanced the base frequency, spacing, character time and the sideband can be fixed instead.
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread, telemetry() and the setters from
// the interface thread.
#pragma once
#include "dect2/common/ring.h"
#include "dect2/engine/mode_tuning.h"
#include "cp16_tel.h"
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

class Cp16Receiver {
public:
    Cp16Receiver();
    ~Cp16Receiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);                     // where the dial frequency sits in the input, Hz from 0
    bool ready() const;
    void reset();                                        // after a retune (the text and picture stay: clearResults)
    void feed(const cf32* x, size_t n);
    bool telemetry(Cp16Telemetry& out, uint64_t lastSeq);
    void clearResults();
    std::string saveResults() const;
    bool loadResults(const std::string& blob);
    void setLogCallback(std::function<void(const std::string&)> cb);
    // Advanced: 0 = automatic. baseHz is the audio frequency of the lowest carrier (either sideband); sideband 0 automatic, 1 upper, 2 lower
    void setManual(double baseHz, double spacingHz, double charSec, int sideband);
    // Advanced: read characters (on by default); off shows only the picture
    void setRead(bool on);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning cp16Tuning();

} // namespace dect2
