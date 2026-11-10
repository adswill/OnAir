// Mesh (LoRa) receiver: Meshtastic, MeshCore, LoRa APRS and MeshCom at the same time. Each LoRa setting of the region that falls inside the captured band
// gets a channel filter and a demodulator (mesh_lora.h); good frames go to the packet layer (mesh_proto.h) and fill the tables of
// MeshTelemetry. Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never
// blocks), telemetry() and the setters from the interface thread.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "mesh_tel.h"
#include "mesh_proto.h"
#include "dect2/common/ring.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

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
    std::string saveResults() const;                     // for the results file (results_io.h)
    bool loadResults(const std::string& blob);           // false: not this build's format, nothing changes
    void setLogCallback(std::function<void(const std::string&)> cb);   // events for the log pane; may be called from the receiver thread
    // Every frame with a good CRC from a decoder that takes MeshCore (protocol: the packet layer that read it, 2 = MeshCore), as it came off
    // the air, for the MeshCore device (meshcore_node.h). Called from the receiver thread: the callback must not block.
    void setFrameCallback(std::function<void(int protocol, const std::vector<uint8_t>& payload, float snrDb, double freqHz)> cb);

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

} // namespace dect2
