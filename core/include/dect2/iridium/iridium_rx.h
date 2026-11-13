// Iridium receiver: bursts over the whole capture (FFT detector), each burst demodulated on a worker thread (DQPSK, unique word),
// its bits handed to the frame layer (iridium_frame.h), the results kept as telemetry.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "iridium_frame.h"
#include "iridium_tel.h"
#include "iridium_voice.h"
#include "dect2/common/ring.h"
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

class IridiumReceiver {
public:
    IridiumReceiver();
    ~IridiumReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);                     // where the wanted channel sits relative to 0 Hz in the input
    bool ready() const;                                  // false when the input rate is too low for this mode
    void reset();                                        // after a retune: forget the signal (the decoded tables stay: clearResults)
    void feed(const cf32* x, size_t n);
    bool telemetry(IridiumTelemetry& out, uint64_t lastSeq);
    // The satellites, ring alerts, time broadcasts, pager messages, positions and ACARS stay through reset(), configure() and retunes: only clearResults() empties them
    void clearResults();
    std::string saveResults() const;                     // for the results file (results_io.h)
    bool loadResults(const std::string& blob);           // false: not this build's format, nothing changes
    void setLogCallback(std::function<void(const std::string&)> cb);   // events for the log pane; may be called from the receiver thread

    void setCenterMhz(double mhz);       // the tuned centre (default 1622): absolute burst frequencies, simplex channels
    void setThresholdDb(double db);      // burst detection threshold over the noise floor per FFT bin (default 13)
    // Files and tests: feed() waits for the worker instead of dropping bursts when it falls behind (never in the app).
    void setOffline(bool on);
    // 4.1: bit reliabilities go to the frame decoder, which repairs frames with one failing BCH block (default on; off: hard decisions as in 4.0)
    void setSoftDecisions(bool on);
    void flush();                        // waits until every burst found so far is demodulated and decoded
    // Every burst with a unique word, with its frame (tool output, tests). Called on the worker thread.
    // iridium-toolkit: writes every burst with a unique word (downlink, uplink, simplex) as a gr-iridium iridium-extractor "RAW:" line
    // (iridium_bits.h). Timestamps: the wall clock when writing started plus the receiver's sample clock since then.
    bool startBitsFile(const std::string& path, std::string* err = nullptr);
    void stopBitsFile();
    bool bitsFileOpen() const;
    uint64_t bitsLines() const;
    std::string bitsPath() const;
    // Same RAW lines live to host:port (UDP datagram per line, or TCP to a listener); with or without the file.
    bool startBitsStream(bool tcp, const std::string& host, int port, std::string* err = nullptr);
    void stopBitsStream();
    bool bitsStreamOpen() const;
    uint64_t bitsStreamLines() const;
    // Voice calls (iridium_voice.h): listen selector, volume, mute, recordings. Safe from the interface thread.
    IridiumVoice& voice();
    void setBurstCallback(std::function<void(const IridiumBurstBits&, const IridiumFrame&)> cb);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning iridiumTuning();

} // namespace dect2
