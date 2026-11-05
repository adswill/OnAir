// Sub-GHz sensors (433.92 MHz first, also 315, 868 and 915 MHz): pulse detector, slicers and device decoders of rtl_433 (third_party/rtl_433, GPL-2.0-or-later)
// behind the engine's receiver contract, a device table with history, an unknown signal analyser and flex-style custom decoders.
//
// Chain: the IQ stream (any rate; used as it is up to 1.1 Msps, faster inputs averaged down towards 1 Msps) -> 8-bit IQ as rtl_433 reads it -> AM envelope + FM discriminator -> rtl_433's pulse detector
// (OOK and FSK packages) -> every enabled decoder through its slicer (PWM, PPM, Manchester, PCM, DMC, ...) -> decoded messages. A package that no
// decoder takes goes to the unknown signal analyser (pulse and gap widths, modulation guess, bits, a flex decoder definition).
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
#include <vector>

namespace dect2 {

struct SensorsTelemetry;   // sensors_tel.h

struct SensorProtocolInfo { unsigned num; std::string name; bool defaultEnabled; unsigned modulation; };

class SensorsReceiver {
public:
    SensorsReceiver();
    ~SensorsReceiver();
    void configure(double inputRateHz);
    void setSignalOffset(double hz);     // where the signal sits in the samples
    void setCenterHz(double hz);         // the radio's frequency (reported with unknown signals)
    bool ready() const;
    void reset();
    void clearResults();
    void feed(const cf32* x, size_t n);
    bool telemetry(SensorsTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);
    void flush();

    // "Remove after": sensors not heard for this long leave the list (0 = never)
    void setRemoveAfterSec(int s);
    // flex-style custom decoders, rtl_433 flex syntax ("n=name,m=OOK_PWM,s=300,l=600,r=2000,bits>=20"); returns how many were accepted
    int setCustomDecoders(const std::vector<std::string>& specs);
    // "" when the spec is valid, else why not
    static std::string checkFlexSpec(const std::string& spec);

    // the decoders: protocol numbers are rtl_433's; the default set is the ones rtl_433 runs by default
    static std::vector<SensorProtocolInfo> protocols();
    void enableAll(bool includeDefaultOff);
    void setProtocolEnabled(unsigned num, bool on);

    // Offline: 8-bit IQ (interleaved I, Q as rtl_433 reads .cu8 files) at the given rate, nothing resampled.
    // json (optional) receives each decoded message as an rtl_433 JSON line. fskMode: 0 classic, 1 minmax, 2 auto (rtl_433's -Y).
    void feedCu8(const uint8_t* iq, size_t nSamples, uint32_t rate);
    void setJsonCallback(std::function<void(const std::string&)> cb);
    void setFskMode(int m);
    void setFixedLevelDb(float db);      // rtl_433 -Y level=
    void setMinLevelDb(float db);        // rtl_433 -Y minlevel=
    void setAutoLevel(bool on);
    void setClock(std::function<int64_t()> clk);   // tests: a clock for the message times
    // Offline: a bit buffer in rtl_433's "{N}hex" code syntax (one row per "{N}hex"; rows joined with a space or nothing) through every enabled decoder
    // (what rtl_433 -y does). Returns how many messages were decoded; json gets one rtl_433 JSON line per message.
    int decodeCode(const std::string& code, std::vector<std::string>* json = nullptr);
    void setProtocolsOnly(const std::vector<unsigned>& nums);   // exactly these decoders (protocol numbers), no others
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// CSV, JSON and MQTT-style lines of the device table
std::string sensorsCsv(const SensorsTelemetry& t);
std::string sensorsJson(const SensorsTelemetry& t);
// one "topic payload" line per value, topics as rtl_433's MQTT output: <prefix>/devices/<model>/<channel>/<id>/<key>
std::string sensorsMqtt(const SensorsTelemetry& t, const std::string& prefix = "rtl_433");

ModeTuning sensorsTuning();

} // namespace dect2
