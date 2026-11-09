// BGAN mode: finds forward bearers in a wide capture, runs a demodulator per carrier through one channelizer, merges what they decode.
// Carriers are classified by width, the demodulator confirms by frame sync (pilots, unique word) and CRC. A carrier 170 - 225 kHz wide that does not
// lock and whose squared spectrum has the pair of lines of filtered OQPSK at 135.4 ksym/s is listed as a GMR-2 class forward carrier (the IsatPhone family).
#pragma once
#include "dect2/bgan/bgan_tel.h"
#include "dect2/common/ring.h"
#include <cstddef>
#include <functional>
#include <memory>
#include <string>

namespace dect2::bgan {

struct ManagerConfig {
    double rate = 0;              // capture rate
    double centerHz = 0;          // absolute centre of the capture
    int maxCarriers = 8;
};

class Manager {
public:
    Manager();
    ~Manager();
    void configure(const ManagerConfig& c);
    bool ready() const;
    void reset();                                 // new tuning: demodulators stop, the merged tables stay
    void clearResults();
    void feed(const cf32* x, size_t n);
    bool snapshot(BganTelemetry& out, uint64_t lastSeq);
    void setCenter(double hz);
    void setScanStatus(bool scanning, int step, int steps);
    void setLogCallback(std::function<void(const std::string&)> cb);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2::bgan
