// DVB-C2 receiver (EN 302 769), written to the engine's contract: configure() once the sample rate is known, feed() from the analysis
// thread (the work runs on the receiver's own thread), telemetry() and the setters from the interface thread.
// Input: complex baseband of a 7.61 MHz tuning window of the C2 system, at least 8 Msample/s (resampled to the 64/7 Msample/s of the
// 8 MHz raster). Chain: guard interval correlation (symbol timing, the guard interval, the fractional carrier offset) -> preamble symbol
// found from its pilot structure, the absolute carrier index of the window from the differential pilot reference sequence (around the
// tuned frequency, or over the whole sequence when the frequency is not known) -> preamble channel estimate (pilots every 6 carriers),
// preamble header (Reed-Muller 32,16), L1 part 2 (16K LDPC, BCH, CRC-32) -> the data symbols: scattered, continual and edge pilots for the
// channel, common phase and clock tracking -> the chosen data slice: frequency and time de-interleaving, Data Slice Packets (the PLP_START
// pointer of Type 1, the FECFrame headers of Type 2) -> LDPC, BCH, BB frames -> transport stream packets to the packet callback.
// Receive only, and only what is sent in the clear. 16-QAM to 256-QAM; 1024 / 4096-QAM, GSE and reserved tones are not decoded.
// Nothing here has met a real cable network: checked against the generator only (docs/DVBC2-3.9.md).
#pragma once
#include "dvbc2.h"
#include "dect2/engine/mode_tuning.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct Dvbc2PlpRow {
    int sliceId = 0, plpId = 0;
    int fecType = 1, mod = 0, cod = 0;    // Type 1 from the L1, Type 2 from the last FECFrame header seen
    int type = 2, payloadType = 3;
    double bitrate = 0;                   // measured bit/s of its transport stream (the decoded PLP only)
};

struct Dvbc2Telemetry {
    // ---- the part every mode has
    uint64_t seq = 0;
    int state = 0;               // 0 searching, 1 symbol timing, 2 preamble found, 3 L1 decoded, 4 decoding data
    double cfoHz = 0;
    float snrDb = 0;             // Es/N0 from the pilots of the data symbols
    bool dataValid = false;      // packets came out recently
    uint64_t blocksOk = 0, blocksBad = 0;   // FEC blocks

    // ---- DVB-C2
    int guardInterval = -1;      // 0 = 1/128, 1 = 1/64, -1 unknown
    double clockPpm = 0;         // sample clock error being corrected
    int64_t windowCarrier = 0;   // absolute carrier index of the centre of the input (0 before the preamble is found)
    bool l1Ok = false;
    uint64_t frames = 0, l1Good = 0, l1Bad = 0, syncLosses = 0;
    dvbc2::L1Part2 l1;           // the last one decoded
    std::vector<Dvbc2PlpRow> plps;
    int selectedSlice = -1, selectedPlp = -1;   // ids being decoded
    double merDb = 0;            // of the decoded data cells
    double preBer = 0;
    uint64_t packets = 0;
    double netMbps = 0;
    int headerMetric = 0;        // Type 2: the last FECFrame header's agreement (of 32)
    std::string stage;
    std::vector<cf32> cells;     // recent equalised data cells, at most 2048
    std::vector<float> psdDb;    // spectrum of the input, fft shifted over the input rate
    double psdSpanHz = 0;
    uint64_t dropped = 0;        // input samples lost because the decoder fell behind
};

std::string dvbc2Summary(const Dvbc2Telemetry& t);

class Dvbc2Receiver {
public:
    Dvbc2Receiver();
    ~Dvbc2Receiver();
    void configure(double inputRateHz);
    bool ready() const;                                  // false when the input rate is too low
    void reset();
    void feed(const cf32* x, size_t n);
    bool telemetry(Dvbc2Telemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);
    void setPacketCallback(std::function<void(const uint8_t* packets, size_t nPackets, double secs)> cb);
    void setSignalOffset(double hz);                     // where the window sits relative to the centre of the input
    void setCenterHz(double hz);                         // the tuned frequency (0 = unknown: the absolute carrier index is searched)
    void selectPlp(int sliceId, int plpId);              // -1, -1: the first data slice and its first PLP
    void setBlocking(bool b);                            // recordings and tests: wait for the decoder instead of dropping input
    void flush();                                        // waits until everything fed so far is decoded

    struct Impl;
private:
    std::unique_ptr<Impl> p_;
};

} // namespace dect2
