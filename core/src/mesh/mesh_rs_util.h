// Shared by the Rust-backed mesh files (mesh_*_rs.cpp): conversions from the flat records of rs/mesh.h and the start-up layout check.
#pragma once
#include "dect2/mesh/mesh_proto.h"
#include "dect2/rs/mesh.h"
#include <string>

namespace dect2 {
namespace meshrs {

// A record text (length + bytes) as std::string.
template <class S> inline std::string str(const S& s) { return std::string((const char*)s.b, s.len); }
// A byte buffer filled by a text writer of the C ABI (returns the text, empty when the buffer is too small).
template <size_t N, class F> inline std::string text(F f) {
    uint8_t b[N];
    const size_t n = f(b, N);
    return n == (size_t)-1 ? std::string() : std::string((const char*)b, n);
}
inline void node(const OnairMeshNode& c, MeshNodeUpdate& u) {
    u.longName = str(c.long_name); u.shortName = str(c.short_name); u.hwModel = str(c.hw_model); u.role = str(c.role); u.publicKey = str(c.public_key);
    u.hasPosition = c.has_position != 0; u.lat = c.lat; u.lon = c.lon; u.altM = c.alt_m;
    u.batteryPct = c.battery_pct; u.voltage = c.voltage; u.hasEnv = c.has_env != 0; u.tempC = c.temp_c; u.humidity = c.humidity; u.pressureHpa = c.pressure_hpa;
    u.positionTime = c.position_time; u.channelUtilPct = c.channel_util_pct; u.airUtilTxPct = c.air_util_tx_pct; u.uptimeS = c.uptime_s; u.sats = c.sats;
    u.signatureChecked = c.signature_checked != 0; u.signatureOk = c.signature_ok != 0;
}
inline void message(const OnairMeshMessage& c, MeshTextMessage& m) {
    m.from = str(c.from); m.to = str(c.to); m.text = str(c.text); m.hops = c.hops; m.packetId = c.packet_id; m.replyId = c.reply_id; m.senderTime = c.sender_time;
}

} // namespace meshrs
} // namespace dect2
