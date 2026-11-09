// Inmarsat BGAN forward bearers (ETSI TS 102 744 "Family SL"), signalling and structure only: which carriers are BGAN, what bearer type they are, satellite,
// spot beam, network, load and the system information the network broadcasts. Never user traffic and no subscriber identity.
// Not validated on a real BGAN signal: synthetic signals only (docs/BGAN-3.7.md). Engine standard 247, active standard 246, family 245.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/bgan/bgan_tel.h"
#include <complex>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

class BganReceiver {
public:
    BganReceiver();
    ~BganReceiver();
    void configure(double inputRateHz);
    void setSignalOffset(double hz);              // not used: the carriers are found anywhere in the capture
    void setCenterHz(double hz);
    bool ready() const;
    void reset();                                 // a new tuning (the tables stay)
    void clearResults();
    void feed(const std::complex<float>* x, size_t n);
    bool telemetry(BganTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);
    void setScanStatus(bool scanning, int step, int steps);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning bganTuning();
std::string bganSummary(const BganTelemetry& t);

} // namespace dect2
