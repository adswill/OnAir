// Wireless M-Bus (EN 13757-4) receiver for the engine: IQ in, meter frames out.
// 868 MHz plan: S1 on 868.3 MHz, T1 and C1 on 868.95 MHz, R2 on 868.03 / 868.09 / 868.15 MHz; 169 MHz plan: N1a..f from 169.40625 MHz in 12.5 kHz steps.
// Every channel that lies inside the capture is mixed to baseband, filtered, FM-demodulated and run through a chip clock recovery and sync search per mode
// (Manchester for S and R2, 3-out-of-6 for T, NRZ for C and N); the bytes are checked with the block CRCs and parsed (wmbus_proto.h).
// Encrypted payloads are listed as encrypted and never decoded.
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

struct WmbusTelemetry;   // wmbus_tel.h

class WmbusLiveReceiver {
public:
    WmbusLiveReceiver();
    ~WmbusLiveReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);            // not used: the channels are placed from the centre frequency
    void setCenterHz(double hz);                // the radio's centre (0: a file: the 868 MHz plan centred on 868.625 MHz); a centre below 300 MHz selects the 169 MHz plan
    bool ready() const;
    void reset();                               // the demodulators; the device table stays
    void clearResults();                        // empties the device table and the frame log
    void feed(const cf32* x, size_t n);
    bool telemetry(WmbusTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);
    // 4.0: a view onto some wireless M-Bus modes: bit k of the mask = wmbus::Phy k; only the meters and frames of those modes are reported (telemetry()), 0 = every mode.
    void setPhyMask(uint32_t mask);
    void setMfrFilter(const std::string& code);   // 4.0: only the meters (and frames) of this manufacturer code ("KAM"); "" every manufacturer. Combined with the mode mask.

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning wmbusTuning();

// 4.0: the wireless M-Bus modes that OnAir lists as modes of their own (docs/VIEWS-4.0.md): views onto this receiver (all channels still searched, the frames of one mode listed).
// family N, active standard N + 1, engine standard N + 2, poll slot N.
struct WmbusView {
    int family;
    const char* id;          // dect2cli --standard
    const char* name;        // the mode-list name
    uint32_t phyMask;        // bits of wmbus::Phy
    double defMhz;
    bool plan169;            // the 169 MHz plan (mode N)
    const char* mfr = "";    // the manufacturer code of the meters shown ("KAM"), empty: every manufacturer
};
const WmbusView* wmbusViews(int& count);
const WmbusView* wmbusViewForActive(int activeStd);
const WmbusView* wmbusViewForStd(int stdMode);
ModeTuning wmbusViewTuning(int i);
#define DECT2_WMBUSVIEW_ACTIVE_CASES case 721: case 724: case 727: case 733: case 736: case 744: case 798: case 468: case 471:

} // namespace dect2
