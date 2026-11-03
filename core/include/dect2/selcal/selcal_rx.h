// SELCAL receiver (ARINC 714): the two-pair tone call that alerts an aircraft's crew on an HF or VHF voice channel. It runs the SELCAL detector
// of the tone decoders (tones_rx.h: 16 tones, 100 ms Hann windows, two tones in balance, each pair at least 0.55 s, up to 0.8 s apart) on the
// radio's channel (USB on the aeronautical HF channels, FM or AM on VHF) or on a sound card, and keeps a call log with the time, the four tone
// frequencies and the dial frequency. SELCAL carries no position: a code names an airframe. Written to the engine's contract: configure() once
// the rate is known, feed() from the analysis thread, telemetry() and the setters from the interface thread.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

struct SelcalTelemetry;   // selcal_tel.h

class SelcalReceiver {
public:
    SelcalReceiver();
    ~SelcalReceiver();
    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    void setAudioInput(bool on);
    void setDemod(int d);                  // TonesDemod: 0 FM, 1 USB, 2 LSB, 3 AM
    void setDialHz(double hz);             // logged with every call; 0 = unknown
    bool ready() const;
    void reset();
    void clearResults();
    void feed(const cf32* x, size_t n);
    bool telemetry(SelcalTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);
    void setAircraftTable(const std::string& tsv);   // "CODE<TAB>registration" per line, # comments
    void setListen(bool on);
    void setVolume(float v);
    void setMuted(bool m);
    void setSilent(bool s);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// "AM-CP" -> the four tone frequencies; false for a code that is not two letter pairs of the plan
bool selcalCodeTones(const std::string& code, double hz[4]);
ModeTuning selcalTuning();

} // namespace dect2
