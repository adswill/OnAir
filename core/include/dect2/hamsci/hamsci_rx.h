// Ham science receivers (QRSS, DFCW, FSK441, ISCAT, PI4, Throb, SuperSID, riometer, pulsar): one class, constructed with the kind. The digital modes take the
// upper sideband audio of the channel (dial at 0 Hz, 200 .. 3800 Hz above it) from the usual front end and hand 12 kHz audio to the decoders of hamsci_dsp.h;
// SuperSID takes a VLF span, the riometer the whole capture as a noise power, the pulsar folder the whole capture as a filter bank.
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread, telemetry() and the setters from the UI thread.
// Automatic parameters are on by default (Throb variant, ISCAT variant, QRSS centre and element length, pulsar search ranges); every manual setter turns its own Auto off.
#pragma once
#include "dect2/common/ring.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/hamsci/hamsci_tel.h"
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

class HamsciReceiver {
public:
    HamsciReceiver(int kind = 0);   // not explicit: the engine holds an array {0}, {1}, ...
    ~HamsciReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    void setCenterHz(double hz);          // the radio centre (SuperSID: dial = centre + offset)
    bool ready() const;
    void reset();
    void feed(const cf32* x, size_t n);
    bool telemetry(HamsciTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);
    int kind() const { return kind_; }

    void clearResults();                  // text, lists, counters (not the quiet-day curve)
    // automatic parameters
    void setAuto(bool on);
    bool autoParams() const;
    // QRSS / DFCW (manual: turns Auto off)
    void setQrssCentreHz(double audioHz);
    void setQrssDotSec(double s);
    void setDfcwReverse(bool on);
    // ISCAT / Throb: -1 / 2 = automatic
    void setVariant(int v);
    // SuperSID
    void setSidRiseDb(double db);
    // riometer
    void setRioLongitude(double lonDeg);
    void setRioThresholdDb(double db);
    std::string rioExportQdc() const;
    bool rioImportQdc(const std::string& s);
    // pulsar: the target's nominal period (s), dispersion measure, the observing frequency (MHz) and bandwidth set by the radio; search ranges stay automatic
    void setPulsarTarget(double periodS, double dm, double fluxJy);
    void pulsarSearchNow();
    // event monitors (kinds 10 .. 15, hamsci_evt.h): detection threshold (the kind's default when never set), CSV of the events and of the trend
    void setEvtThresholdDb(double db);
    std::string evtExportCsv() const;
    std::string evtExportTrendCsv() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
    int kind_;
};

ModeTuning hamsciTuning(int kind);
std::string hamsciSummary(const HamsciTelemetry& t);
const char* hamsciName(int kind);
const char* hamsciId(int kind);

}  // namespace dect2
