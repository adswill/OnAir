// Mesh (LoRa) receiver: Meshtastic, MeshCore, LoRa APRS and MeshCom at the same time. Each LoRa setting of the region that falls inside the captured band
// gets a channel filter and a demodulator (mesh_lora.h); good frames go to the packet layer (mesh_proto.h) and fill the tables of
// MeshTelemetry. Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never
// blocks), telemetry() and the setters from the interface thread.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "mesh_tel.h"
#include "mesh_proto.h"
#include "dect2/common/ring.h"
#include <cmath>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

class MeshReceiver {
public:
    MeshReceiver();
    ~MeshReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);                     // where the user's frequency sits relative to 0 Hz in the input
    bool ready() const;                                  // false when the input rate is too low for this mode
    void reset();                                        // after a retune: forget the signal (the decoded lists stay: clearResults)
    void feed(const cf32* x, size_t n);
    bool telemetry(MeshTelemetry& out, uint64_t lastSeq);
    // The packet list, nodes, messages, APRS stations and LoRaWAN devices stay through reset(), configure() and retunes: only clearResults() empties them
    void clearResults();
    uint64_t framesWithJumpsFixed() const;               // frames since the start of the program whose CRC came right only after undoing sample jumps (any thread)
    std::string saveResults() const;                     // for the results file (results_io.h)
    bool loadResults(const std::string& blob);           // false: not this build's format, nothing changes
    void setLogCallback(std::function<void(const std::string&)> cb);   // events for the log pane; may be called from the receiver thread

    // settings (any thread; they take effect at the next feed())
    void setTunedHz(double hz);                          // the user's frequency (default: meshTuning().defMhz); places the channels
    void setRegion(int region);                          // 0 EU (default), 1 US (also LoRaWAN EU868 / US915), 2 ANZ (no LoRaWAN), 3 ANZ with MeshCore Mid
    void setTableScale(int scale);                       // 1 (default: 100 packets, 150 nodes, 100 messages, 150 APRS stations, 200 LoRaWAN devices) .. 20 times that; the oldest still leave first
    void setProtocols(int mask);                         // 1 Meshtastic, 2 MeshCore, 4 LoRa APRS, 8 MeshCom, 16 LoRaWAN (default 15: all but LoRaWAN)
    void setPresetSearch(bool all);                      // also the other Meshtastic presets of the region (more CPU)
    // Meshtastic channel plan: a firmware region ("EU_433", "ANZ", "LORA_24", ...; "" = EU_868 / US from setRegion) and the primary
    // modem preset ("LongFast" default; the preset search adds the others of the region)
    void setMeshtasticPlan(const std::string& region, const std::string& preset);
    // Custom LoRa setting: only this one is searched while on (freqHz 0 = the tuned frequency; bwHz any, sf 5..12, cr 5..8). The frame
    // is offered to the protocols of the sync word: 0x2B Meshtastic / MeshCom, 0x12 MeshCore / LoRa APRS, 0x34 LoRaWAN, other: all.
    void setCustom(bool on, const MeshLoraSettings& s);
    bool addMeshtasticChannel(const std::string& name, const std::string& base64Psk);   // false: bad key
    bool addMeshCoreChannel(const std::string& name, const std::string& secret);        // hex or base64; false: bad key
    void clearUserChannels();

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning meshTuning();

// Does a channel of bwHz, inOffHz from 0 Hz in the input, get a demodulator at this input rate? (The receiver's own rule.)
inline bool meshChannelFits(double rateHz, double inOffHz, double bwHz) {
    return std::fabs(inOffHz) + 0.5 * bwHz + 25e3 < 0.45 * rateHz && rateHz >= 2.2 * bwHz;
}
// An RTL-SDR (USB or rtl_tcp) drops samples at 2.048 Msps, which breaks frames of a long LoRa symbol (issue #386); a channel that fits
// in 1.024 Msps loses none there. Returns that rate when the board is an RTL-SDR and the channel, placed where the mode's tune offset
// puts it, fits (250 kHz and narrower do, 500 kHz does not); 0 = the usual rate. Other radios keep the mode's 2 Msps.
inline double meshRtlRateHz(const std::string& board, double bwHz) {
    constexpr double kRate = 1.024e6;
    if (board != "rtlsdr" && board != "rtltcp") return 0;
    return bwHz > 0 && meshChannelFits(kRate, -meshTuning().tuneOffsetHz, bwHz) ? kRate : 0;
}

} // namespace dect2
