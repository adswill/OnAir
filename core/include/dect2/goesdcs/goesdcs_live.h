// GOES / Meteosat DCS receiver for the engine: IQ in, platform reports out.
// A data collection platform sends a short report (a few seconds at 100 or 300 bps, a fraction of that at 1200 bps) on its own 1.5 kHz channel of the
// 401.7 - 402.1 MHz uplink band: unmodulated carrier, bit preamble, frame sync, a BCH-protected platform address and the sensor data in ASCII or
// pseudo-binary characters. The receiver watches the whole captured band for carrier lines, follows each with a phase locked loop, tries every bit rate
// and coding in parallel for the frame sync, decodes the report and keeps a table of the platforms heard. Every report can be appended to a CSV file.
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

namespace dect2 {

struct DcsTelemetry;   // goesdcs_tel.h

class GoesDcsReceiver {
public:
    GoesDcsReceiver();
    ~GoesDcsReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);            // not used: the whole captured band is searched
    void setCenterHz(double hz);                // the radio's centre (0 unknown: no channel numbers)
    void setPlan(int plan);                     // channel numbers: -1 Auto (from the centre), 0 GOES, 1 Meteosat, 2 Himawari (dcs::Plan)
    bool ready() const;
    void reset();                               // the filters and the carriers; the lists stay
    void clearResults();                        // empties the report list and the platform table
    void setLogFile(const std::string& path);   // append every report as a CSV line ("" off)
    void setStartUtc(int64_t utcSec);           // tests: the UTC second of the first sample (default: now)
    void setThresholdDb(double db);             // carrier line over the noise (default 11)
    void feed(const cf32* x, size_t n);
    bool telemetry(DcsTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning goesdcsTuning();

} // namespace dect2
