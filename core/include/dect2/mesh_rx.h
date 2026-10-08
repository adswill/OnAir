// Mesh (LoRa) receiver: Meshtastic and MeshCore at the same time. Each LoRa setting of the region that falls inside the captured band
// gets a channel filter and a demodulator (mesh_lora.h); good frames go to the packet layer (mesh_proto.h) and fill the tables of
// MeshTelemetry. Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never
// blocks), telemetry() and the setters from the interface thread.
#pragma once
#include "mode_tuning.h"
#include "mesh_tel.h"
#include "ring.h"
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
    void reset();                                        // after a retune: forget the signal and the decoded state
    void feed(const cf32* x, size_t n);
    bool telemetry(MeshTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);   // events for the log pane; may be called from the receiver thread

    // settings (any thread; they take effect at the next feed())
    void setTunedHz(double hz);                          // the user's frequency (default: meshTuning().defMhz); places the channels
    void setRegion(int region);                          // 0 EU (default), 1 US
    void setProtocols(int mask);                         // 1 Meshtastic, 2 MeshCore, 3 both (default)
    void setPresetSearch(bool all);                      // also the other Meshtastic presets of the region (more CPU)
    bool addMeshtasticChannel(const std::string& name, const std::string& base64Psk);   // false: bad key
    bool addMeshCoreChannel(const std::string& name, const std::string& secret);        // hex or base64; false: bad key
    void clearUserChannels();

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning meshTuning();

} // namespace dect2
