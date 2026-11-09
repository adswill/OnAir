// Receiver of the SatDump tier 3 links (sd3_tel.h): one class, one profile per mode-list entry. Written to the engine's contract: configure() once the sample rate is known,
// feed() from the analysis thread, telemetry() and the setters from the interface thread. Link parameters from SatDump (GPL-3.0, Copyright Aang23 and contributors).
#pragma once
#include "dect2/common/ring.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/sd3/sd3_tel.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

class Sd3Receiver {
public:
    Sd3Receiver(int profile);
    ~Sd3Receiver();
    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    bool ready() const;
    void reset();
    void clearImage();
    void feed(const cf32* x, size_t n);
    bool telemetry(Sd3Telemetry& out, uint64_t lastSeq);
    void clearResults();
    void setLogCallback(std::function<void(const std::string&)> cb);
    int profile() const;
    // the satellite / link within the profile (sd3LinkAt); takes effect at the next feed
    void setLink(int index);
    int link() const;
    void setSaveDir(const std::string& dir);          // where files and pictures go ("" nowhere)
    void setProduct(int key);                         // GRB: the picture shown, -1 follows the newest
    void setBlocking(bool blocking);                  // DVB-S2 links on recordings: wait for the decoder instead of dropping frames
    void flush();                                     // tests: everything fed so far is decoded and reported

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning sd3Tuning(int profile);
int sd3LinkForFrequency(int profile, double mhz);      // the link of the table closest to this frequency (within 1 MHz), or -1

} // namespace dect2
