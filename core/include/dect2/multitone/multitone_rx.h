// Multitone paging (Multitone Electronics, UK; Mk IV - VII pagers, hospitals and industrial sites) detector: receive only.
//
// What is public (sigidwiki.com/wiki/Multitone_Paging, see docs/multitone-research.md): narrowband FM with FFSK, 12.5 / 25 kHz, "coding similar
// to POCSAG 1200" but with its own headers, readable only by Multitone's own pagers, plus some analogue tone use and occasional speech. No
// specification, tone table or sample of the digital format is public. So this is not a decoder: it is a detector. For every transmission
// (carrier up to carrier gone) it reports the carrier, the FM deviation, the spectral lines and steady tones of the audio and, when the audio is
// two-level FSK or audio FFSK (tones f and 2f, found on the sigidwiki recordings), the measured baud rate, the bit stream in hex and how many 32-bit words pass the POCSAG BCH(31,21) + parity check at the best
// alignment, and whether the POCSAG sync word is in it. Everything is labelled unverified; no message text is produced. Pager messages are
// private: nothing is decrypted and nothing is transmitted.
//
// Chain: mix out the signal offset, decimate to 48 kHz (the pager front end), FM discriminator. While the carrier is up the discriminator is
// kept (at most 6 s per piece) and its 8 kHz audio feeds an FFT for spectral lines and steady tones; when the carrier goes the piece is analysed
// for FSK (edge coherence against a bit clock, then slicing and the BCH search).
//
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks),
// telemetry() and the setters from the interface thread.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct MultitoneTelemetry;   // multitone_tel.h
struct MtBurst;

class MultitoneReceiver {
public:
    MultitoneReceiver();
    ~MultitoneReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    bool ready() const;
    void reset();                         // the detector only: the lists stay
    void clearResults();
    void feed(const cf32* x, size_t n);
    bool telemetry(MultitoneTelemetry& out, uint64_t lastSeq);
    std::string saveResults() const;                     // for the results file (results_io.h)
    bool loadResults(const std::string& blob);           // false: not this build's format, nothing changes
    void setLogCallback(std::function<void(const std::string&)> cb);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning multitoneTuning();

namespace multitone {
// The analysis of a discriminator recording (rad/sample at 48 kHz, one transmission), exposed for the tests and the tools
struct FskResult {
    bool found = false;
    double baud = 0, coherence = 0;
    int nBits = 0;
    std::vector<uint8_t> bits;            // as sliced, polarity +1 (high frequency = 1)
    bool ffsk = false;                    // audio-frequency FFSK (tones f and 2f, one 2f cycle or one f half cycle per symbol), found from the zero crossings
    double loHz = 0, hiHz = 0;            // its two audio tones
};
FskResult sliceFfsk(const std::vector<float>& disc, double rate);   // sliceFsk tries this first
FskResult sliceFsk(const std::vector<float>& disc, double rate);
// Best BCH(31,21)+parity alignment of a bit stream, both polarities: fills the burst's pol, align, words, counts, sync, hex and verdict
void analyseBits(const std::vector<uint8_t>& bits, MtBurst& b);
// Repetition and marker structure of a bit stream (fills the framing members of the burst); analyseBits calls it
void analyseFraming(const std::vector<uint8_t>& bits, MtBurst& b);
const char* nearestStandardBaud(double baud);   // "1200" within 1.5% of 512 / 1200 / 2400 / 600 / 300 / 4800 / 9600, else ""
} // namespace multitone

} // namespace dect2
