// ISDB-S receiver (BS satellite television, L-band IF after the LNB), written to the engine's contract: configure() once the sample rate is
// known, feed() from the analysis thread, telemetry() and the setters from the interface thread.
// Input: complex baseband with the transponder (34.5 MHz wide, 28.86 Msymbol/s) near the centre, at least 39 Msample/s.
// Chain: matched filter and timing recovery (the DVB-S front end) -> coarse carrier from the 8th power -> frame sync (the coded W1 and
// W2 / W3 words of the BPSK header) -> carrier phase and frequency from W1 and the 192 bursts of each frame (squared: their content is not
// relied on) -> header Viterbi (TMCC bits, first frame of a super-frame) -> TMCC of the super-frame (RS(64,48)), applied two super-frames
// later -> Viterbi over the selected stream's slots (one code through the frame, TC8PSK's uncoded bit decided after) -> de-interleaving
// across the 8 frames, energy dispersal, Reed-Solomon -> the packets of one transport stream, handed to the packet callback.
// Receive only, and only what is sent in the clear: scrambled (conditional access) streams come out scrambled and are not touched.
// Not tried on a real BS transponder, see isdbs.h and docs/ISDBS.md.
#pragma once
#include "dect2/isdbs/isdbs.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/engine/mode_synth.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct IsdbsStreamRow {
    int rel = 0;                // relative transport stream number
    uint16_t tsId = 0;
    int slots = 0;
    int mod = 0;                // isdbs::Mod of its slots
    double bitrate = 0;         // transport stream bit/s it can carry
    bool mixed = false;         // its slots do not all use one modulation
};

struct IsdbsTelemetry {
    uint64_t seq = 0;
    int state = 0;              // 0 no input, 1 searching, 2 frame sync, 3 TMCC decoded, 4 transport stream
    double cfoHz = 0;
    float snrDb = 0;            // Es/N0 from the burst symbols
    bool dataValid = false;     // packets came out recently
    bool locked = false;        // frame sync
    uint64_t blocksOk = 0;      // Reed-Solomon packets that were good (clean or corrected)
    uint64_t blocksBad = 0;
    uint64_t frames = 0, superFrames = 0, tmccGood = 0, tmccBad = 0, syncLosses = 0;
    uint64_t packets = 0, rsClean = 0, rsCorrected = 0, rsFailed = 0;
    isdbs::Tmcc tmcc;           // the last one decoded (valid = false before the first)
    std::vector<IsdbsStreamRow> streams;
    int selected = -1;          // relative number being decoded, -1 none
    double tsBitrate = 0;       // measured, bit/s of the selected stream's packets
    double inputRate = 0;
    double centerHz = 0;
    std::vector<std::complex<float>> constellation;   // a few hundred recent data symbols after carrier recovery
};

class IsdbsReceiver {
public:
    IsdbsReceiver();
    ~IsdbsReceiver();
    void configure(double inputRateHz);
    bool ready() const;                                  // false when the input rate is too low (under 39 Msample/s)
    void reset();
    void feed(const std::complex<float>* x, size_t n);
    bool telemetry(IsdbsTelemetry& out, uint64_t lastSeq);
    uint64_t idleInputSamples() const;   // input samples let go by unprocessed while there was no signal (the no-signal pre-check; tests)
    void setLogCallback(std::function<void(const std::string&)> cb);
    // The decoded transport stream: whole 188 byte packets, `secs` of signal time since the previous call. Runs on the receiver thread.
    void setPacketCallback(std::function<void(const uint8_t* packets, size_t nPackets, double secs)> cb);
    void setSignalOffset(double hz);                     // where the transponder sits relative to the centre of the input
    void setCenterHz(double hz);                         // for the display only
    void selectStream(int rel);                          // relative transport stream number to decode; -1: the one with the most slots
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning isdbsTuning();
std::string isdbsSummary(const IsdbsTelemetry& t);
// The test signal of the mode (engine standard 156), the built-in source plays it. SynthConfig: snrDb is Es/N0, cfoHz the carrier offset, sroPpm the symbol clock
// offset; modeOpt[0] the layout: 0 (default) three streams in three modulations (TC8PSK 2/3 in 36 slots with the test programme, QPSK 3/4 in 8 slots and BPSK 1/2
// in 4, null packets in the last two), 1 TC8PSK 2/3 in all 48 slots, 2 QPSK 1/2 in all 48 slots. nullptr when the rate is under 39 Msample/s.
std::unique_ptr<ModeSynth> makeIsdbsSynth(const SynthConfig& cfg, double sampleRate);

}  // namespace dect2
