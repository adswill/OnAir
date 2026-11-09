// GOES-R series HRIT receiver (GOES-16 / 18 / 19 on 1694.1 MHz: BPSK, 927 ksym/s). Needs a dish (about 1 m or more, or a grid) with
// an LNA / filter at the feed; the carrier is far below the noise of a plain antenna.
//
// The input is mixed down (the coarse carrier offset from the spectrum of the squared signal, BPSK's doubled line), decimated to 2 - 4
// samples a symbol, filtered with the root raised cosine (roll-off 0.5), timed with a Gardner loop and a cubic interpolator, and the
// carrier is followed by a Costas loop. The frame start is found from the coded sync word (the 52 code symbols that do not depend on
// the encoder state) in the soft symbols, which also gives the pairing of the code symbols, the polarity and whether the second code
// output is inverted; then K = 7 Viterbi, sync word, randomiser, Reed-Solomon (meteor_proto.h), and the transport layer and the files
// (goes_proto.h).
//
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks),
// telemetry() and the setters from the interface thread.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct GoesTelemetry;   // goes_tel.h

class GoesReceiver {
public:
    GoesReceiver();
    ~GoesReceiver();

    void configure(double inputRateHz);
    void setSymbolRate(double hz);          // default 927 ksym/s (GOES-R HRIT); Elektro-L HRIT is 665.4 ksym/s
    double symbolRate() const;
    void setSignalOffset(double hz);
    void setOqpsk(bool on);                 // Elektro HRIT 1.15 Msym/s: OQPSK (QPSK with the Q rail half a symbol late)
    void setQpsk(bool on);                  // GK-2A HRIT: QPSK (G1 on I, G2 on Q) instead of BPSK
    // 1: the KMA (GK-2A) xRIT files; `tag` heads the log lines ("GK-2A HRIT")
    void setDialect(int dialect, const std::string& tag);
    void setKeys(const std::vector<std::pair<int, std::array<uint8_t, 8>>>& keys);   // DES keys for encrypted GK-2A files
    bool ready() const;
    void reset();                               // the demodulator and partial files; the products stay
    void clearProducts();
    void feed(const cf32* x, size_t n);
    bool telemetry(GoesTelemetry& out, uint64_t lastSeq);
    // The products (pictures, text, files) stay through reset(), configure() and retunes: only clearResults() empties them
    void clearResults();
    std::string saveResults() const;                     // for the results file (results_io.h)
    bool loadResults(const std::string& blob);           // false: not this build's format, nothing changes
    void setLogCallback(std::function<void(const std::string&)> cb);
    // every whole LRIT file (headers and data) as it is completed, from the analysis thread
    // every good frame as a 1024 byte CADU (sync word, then the 1020 bytes derandomised, as SatDump writes them), from the analysis thread
    void setCaduCallback(std::function<void(const uint8_t*)> cb);
    // decode the frames of a .cadu file (1024 byte CADUs, derandomised or as sent) through Reed-Solomon and the upper layers; done on the
    // analysis thread at its next feed(), so it needs the mode running
    void importCadu(std::vector<uint8_t> data);
    void setFileCallback(std::function<void(const std::vector<uint8_t>&, int vcid, int apid)> cb);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning goesTuning();

} // namespace dect2
