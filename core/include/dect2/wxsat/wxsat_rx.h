// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Receiver of the wxsat links (wxsat_tel.h): one class, one profile per mode-list entry. PM with a residual carrier (the Manchester links, NOAA HRPT, DSB,
// Meteor HRPT), BPSK (NOAA GAC, RDAS), QPSK with the CCSDS convolutional code (AWS), or the FM + 2400 Hz AM of APT. Arktika GGAK runs the Elektro GGAK receiver at
// its own frequency. Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread, telemetry() and the setters from the
// interface thread.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include "dect2/wxsat/wxsat_tel.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {
namespace wxsat { class FilecastLayer; }

class WxsatReceiver {
public:
    WxsatReceiver(int profile);
    ~WxsatReceiver();
    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    bool ready() const;
    void reset();
    void clearImage();
    void feed(const cf32* x, size_t n);
    bool telemetry(WxsatTelemetry& out, uint64_t lastSeq);
    uint64_t idleInputSamples() const;   // input samples let go by unprocessed while there was no signal (the no-signal pre-check; tests)
    void clearResults();
    std::string saveResults() const;
    bool loadResults(const std::string& blob);
    void setLogCallback(std::function<void(const std::string&)> cb);
    int profile() const;
    // ---- file broadcasts (HimawariCast, GeoNetCast)
    void setSaveDir(const std::string& dir);          // where files and pictures go ("" nowhere)
    void setFilecastSymbolRate(double hz);            // Advanced: 0 is the link's own rate
    void setFilecastPid(int pid);                     // Advanced: 0 is the link's own PID
    void setAptNoiseFilter(bool on);                  // APT: the noise-adaptive line filter (on by default; a clean line passes unchanged)
    wxsat::FilecastLayer* fileLayer();                // the file layer behind the radio, null for the other profiles
    void setBlocking(bool blocking);                  // file sources: before the first feed; wait for the decoder instead of losing frames
    void flush();                                     // tests and recordings: everything fed so far is decoded and reported

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning wxsatTuning(int profile);

namespace wxsat {
// APT below the FM discriminator, on its own so recordings of the audio can be tested: real audio at audioRate in, 2080-sample lines out.
class AptDecoder {
public:
    AptDecoder();
    ~AptDecoder();
    void configure(double audioRate);
    void reset();
    void pushAudio(const float* x, size_t n);
    std::function<void(int idx, const uint8_t* px)> onLine;       // px: 2080 samples, 8 bit
    bool locked() const;
    uint64_t linesOut() const;
    float carrierLevel() const;
    void setNoiseFilter(bool on);   // on by default: see wxsat_apt.cpp
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};
}

} // namespace dect2
