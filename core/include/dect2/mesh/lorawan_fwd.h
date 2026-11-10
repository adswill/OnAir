// LoRaWAN receive-only packet forwarder: uplinks the mesh mode heard, sent to the user's own network server gateway entry (ChirpStack, The Things
// Network) over the Semtech UDP protocol (packet_forwarder PROTOCOL.TXT, version 2): PUSH_DATA with an "rxpk" JSON array, PULL_DATA as the
// keep-alive. Downlinks (PULL_RESP) are never sent on the air: OnAir only listens. docs/LORAWAN-FWD-3.9.md.
#pragma once
#include "dect2/mesh/mesh_tel.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

// "SF7BW125", the coding rate "4/5"; the frequency in MHz with 6 decimals; data = the PHYPayload in base64 (from MeshPacket::raw, hex)
std::string semtechRxpk(const MeshPacket& p, int64_t unixMicros, uint32_t tmst, int chan = 0);
std::vector<uint8_t> semtechPushData(uint16_t token, const uint8_t eui[8], const std::string& json);   // {"rxpk":[...]} or {"stat":{...}}
std::vector<uint8_t> semtechPullData(uint16_t token, const uint8_t eui[8]);
// the gateway EUI as typed ("AA555A0000000101", with or without : or -); false when not 16 hex digits
bool semtechParseEui(const std::string& s, uint8_t eui[8]);
// an uplink worth forwarding: LoRaWAN (protocol 5), CRC good, join request or data up (MType 0, 2, 4), payload bytes present
bool semtechIsUplink(const MeshPacket& p);

struct SemtechStats { uint64_t pushed = 0, acked = 0, pulls = 0, pullAcks = 0; std::string error; };

class SemtechForwarder {
public:
    SemtechForwarder();
    ~SemtechForwarder();
    // host and port of the gateway bridge (ChirpStack: 1700; TTN: eu1.cloud.thethings.network 1700); an empty host stops it
    bool configure(const std::string& host, int port, const std::string& eui, std::string& err);
    void forward(const MeshPacket& p);          // sends at once when it is an uplink
    void poll(double nowSec);                   // keep-alive every 30 s, reads the acks
    SemtechStats stats() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace dect2
