// Meshtastic packets: the 16 byte header, AES-CTR with the channel key, the Data protobuf and the port payloads.
// Sources: meshtastic/firmware src/mesh/RadioInterface.h (PacketHeader, flag masks), src/mesh/CryptoEngine.cpp (initNonce,
// encryptAESCtr: nonce = packet id as 64 bit little endian, from node, 4 byte counter), src/mesh/Channels.cpp (hash),
// src/modules/{NodeInfoModule,PositionModule,Telemetry/DeviceTelemetry}.cpp (what the firmware sends); meshtastic/protobufs
// mesh.proto (Data, Position, User, Routing, RouteDiscovery, NeighborInfo), portnums.proto, telemetry.proto, config.proto.
#include "mesh_proto_internal.h"
#include "dect2/mesh_crypto.h"
#include "dect2/mesh_pb.h"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace dect2 {

using namespace meshcrypto;
using namespace meshpb;

namespace {

uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
void put32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; i++) v.push_back((uint8_t)(x >> (8 * i))); }

struct DataMsg {
    int portnum = 0;
    const uint8_t* payload = nullptr;
    size_t payloadLen = 0;
    bool wantResponse = false, hasBitfield = false;
    uint32_t dest = 0, source = 0, requestId = 0, replyId = 0, emoji = 0, bitfield = 0;
};

// The firmware's decoder accepts any Data that parses and has a portnum; to avoid taking random bytes from a wrong key for a
// message we also require the portnum to come first (nanopb writes fields in order, portnum is never zero for real traffic).
bool parseData(const uint8_t* p, size_t n, DataMsg& d) {
    Reader r(p, n);
    Field f;
    bool first = true;
    while (r.next(f)) {
        if (f.num > 10) return false;                   // Data has fields 1 to 10 only (mesh.proto)
        if (first) {
            if (f.num != 1 || f.wire != 0 || f.value == 0 || f.value > 511) return false;
            first = false;
        }
        switch (f.num) {
        case 1: if (f.wire == 0) d.portnum = (int)f.value; break;
        case 2: if (f.wire == 2) { d.payload = f.data; d.payloadLen = f.len; } break;
        case 3: d.wantResponse = f.value != 0; break;
        case 4: d.dest = (uint32_t)f.value; break;
        case 5: d.source = (uint32_t)f.value; break;
        case 6: d.requestId = (uint32_t)f.value; break;
        case 7: d.replyId = (uint32_t)f.value; break;
        case 8: d.emoji = (uint32_t)f.value; break;
        case 9: d.hasBitfield = true; d.bitfield = (uint32_t)f.value; break;
        default: break;
        }
    }
    return r.ok() && !first;
}

std::string portName(int port) {
    const char* n = meshEnumName(kMeshPortNums, port);
    return n ? n : meshFmt("PORT_%d", port);
}

std::string nodeStr(uint32_t n) { return n == 0xFFFFFFFFu ? "^all" : meshtasticNodeIdString(n); }

std::string clip(const std::string& s, size_t n) { return s.size() <= n ? s : s.substr(0, n) + "..."; }

// printable text only: replaces control characters, keeps UTF-8 bytes
std::string cleanText(const uint8_t* p, size_t n) {
    std::string s;
    for (size_t i = 0; i < n; i++) s += (p[i] < 0x20 && p[i] != '\n') ? ' ' : (char)p[i];
    return s;
}

void decodePort(const DataMsg& d, uint32_t from, uint32_t to, const MeshPacketInfo& pi, MeshDecodeResult& r) {
    const std::string fromId = meshtasticNodeIdString(from);
    MeshPacketInfo& out = r.packet;
    switch (d.portnum) {
    case 1: {                                                           // TEXT_MESSAGE_APP: UTF-8 text
        MeshTextMessage m;
        m.protocol = MeshProtocol::Meshtastic;
        m.channel = pi.channel;
        m.from = fromId;
        m.to = nodeStr(to);
        m.text = cleanText(d.payload, d.payloadLen);
        m.hops = pi.hopStart > 0 ? pi.hopStart - pi.hopLimit : -1;
        m.packetId = pi.packetId;
        m.replyId = d.replyId;
        out.detail = clip(m.text, 80);
        if (d.replyId) out.detail += " (reply)";
        r.messages.push_back(m);
        break;
    }
    case 3: {                                                           // POSITION_APP
        Reader rd(d.payload, d.payloadLen);
        Field f;
        bool hasLat = false, hasLon = false;
        int32_t lat = 0, lon = 0, alt = 0, altHae = 0;
        bool hasAlt = false, hasHae = false;
        uint32_t t = 0, ts = 0, prec = 0;
        int sats = -1;
        while (rd.next(f)) {
            if (f.num == 1 && f.wire == 5) { lat = f.asInt32(); hasLat = true; }
            else if (f.num == 2 && f.wire == 5) { lon = f.asInt32(); hasLon = true; }
            else if (f.num == 3 && f.wire == 0) { alt = f.asInt32(); hasAlt = true; }
            else if (f.num == 4 && f.wire == 5) t = (uint32_t)f.value;
            else if (f.num == 7 && f.wire == 5) ts = (uint32_t)f.value;
            else if (f.num == 9 && f.wire == 0) { altHae = f.asSint32(); hasHae = true; }
            else if (f.num == 19 && f.wire == 0) sats = (int)f.value;
            else if (f.num == 23 && f.wire == 0) prec = (uint32_t)f.value;
        }
        MeshNodeUpdate u;
        u.protocol = MeshProtocol::Meshtastic;
        u.nodeId = fromId;
        if (hasLat && hasLon && (lat != 0 || lon != 0)) {
            u.hasPosition = true;
            u.lat = lat * 1e-7;
            u.lon = lon * 1e-7;
            u.altM = hasAlt ? alt : (hasHae ? altHae : 0);
            u.positionTime = t ? t : ts;
            u.sats = sats;
            out.detail = meshFmt("%.5f, %.5f, %d m", u.lat, u.lon, (int)u.altM);
            if (prec && prec < 32) out.detail += meshFmt(", %u bit precision", prec);
            r.nodes.push_back(u);
        } else {
            out.detail = "no position fix";
        }
        break;
    }
    case 4: {                                                           // NODEINFO_APP: User
        Reader rd(d.payload, d.payloadLen);
        Field f;
        MeshNodeUpdate u;
        u.protocol = MeshProtocol::Meshtastic;
        u.nodeId = fromId;
        int hw = 0, role = 0;
        bool hasKey = false;
        while (rd.next(f)) {
            if (f.num == 2 && f.wire == 2) u.longName = f.asString();
            else if (f.num == 3 && f.wire == 2) u.shortName = f.asString();
            else if (f.num == 5 && f.wire == 0) hw = (int)f.value;
            else if (f.num == 7 && f.wire == 0) role = (int)f.value;
            else if (f.num == 8 && f.wire == 2) hasKey = f.len == 32;
        }
        u.hwModel = meshtasticHwModelName(hw);
        const char* rn = meshEnumName(kMeshRoles, role);
        u.role = rn ? rn : meshFmt("ROLE_%d", role);
        out.detail = u.longName + " (" + u.shortName + "), " + u.hwModel + ", " + u.role + (hasKey ? ", has public key" : "");
        r.nodes.push_back(u);
        break;
    }
    case 67: {                                                          // TELEMETRY_APP
        Reader rd(d.payload, d.payloadLen);
        Field f;
        MeshNodeUpdate u;
        u.protocol = MeshProtocol::Meshtastic;
        u.nodeId = fromId;
        std::string det;
        auto metrics = [&](const Field& m, bool local) {
            Reader mr(m.data, m.len);
            Field g;
            while (mr.next(g)) {
                if (local) {
                    if (g.num == 1 && g.wire == 0) u.uptimeS = (long)g.value;
                    else if (g.num == 2 && g.wire == 5) u.channelUtilPct = g.asFloat();
                    else if (g.num == 3 && g.wire == 5) u.airUtilTxPct = g.asFloat();
                } else {
                    if (g.num == 1 && g.wire == 0) u.batteryPct = g.value > 100 ? 100 : (double)g.value;
                    else if (g.num == 2 && g.wire == 5) u.voltage = g.asFloat();
                    else if (g.num == 3 && g.wire == 5) u.channelUtilPct = g.asFloat();
                    else if (g.num == 4 && g.wire == 5) u.airUtilTxPct = g.asFloat();
                    else if (g.num == 5 && g.wire == 0) u.uptimeS = (long)g.value;
                }
            }
        };
        while (rd.next(f)) {
            if (f.num == 2 && f.wire == 2) {
                metrics(f, false);
                det = u.batteryPct >= 0 ? meshFmt("battery %d %%, %.2f V", (int)u.batteryPct, u.voltage) : "device metrics";
                // the firmware reports 101 when external power is used
                Reader mr(f.data, f.len); Field g;
                while (mr.next(g)) if (g.num == 1 && g.wire == 0 && g.value > 100) det = meshFmt("powered, %.2f V", u.voltage);
                det += meshFmt(", ch util %.1f %%, air tx %.1f %%", u.channelUtilPct, u.airUtilTxPct);
            } else if (f.num == 3 && f.wire == 2) {
                Reader mr(f.data, f.len);
                Field g;
                while (mr.next(g)) {
                    if (g.num == 1 && g.wire == 5) { u.tempC = g.asFloat(); u.hasEnv = true; }
                    else if (g.num == 2 && g.wire == 5) { u.humidity = g.asFloat(); u.hasEnv = true; }
                    else if (g.num == 3 && g.wire == 5) { u.pressureHpa = g.asFloat(); u.hasEnv = true; }
                }
                det = meshFmt("environment %.1f C, %.0f %%, %.0f hPa", u.tempC, u.humidity, u.pressureHpa);
            } else if (f.num == 6 && f.wire == 2) {
                metrics(f, true);
                det = meshFmt("local stats, uptime %ld s", u.uptimeS);
            }
        }
        out.detail = det.empty() ? "telemetry" : det;
        r.nodes.push_back(u);
        break;
    }
    case 5: {                                                           // ROUTING_APP: ack or error
        Reader rd(d.payload, d.payloadLen);
        Field f;
        int err = 0;
        bool hasRoute = false;
        while (rd.next(f)) {
            if (f.num == 3 && f.wire == 0) err = (int)f.value;
            else if ((f.num == 1 || f.num == 2) && f.wire == 2) hasRoute = true;
        }
        if (hasRoute) out.detail = "route discovery";
        else if (err == 0) out.detail = meshFmt("ack of %08x", d.requestId);
        else { const char* en = meshEnumName(kMeshRoutingErrors, err); out.detail = meshFmt("error %s for %08x", en ? en : "?", d.requestId); }
        break;
    }
    case 70: {                                                          // TRACEROUTE_APP: RouteDiscovery
        Reader rd(d.payload, d.payloadLen);
        Field f;
        std::vector<uint32_t> route, back;
        std::vector<uint64_t> snr, snrBack;
        while (rd.next(f)) {
            if (f.num == 1) readRepeatedFixed32(f, route);
            else if (f.num == 2) readRepeatedVarint(f, snr);
            else if (f.num == 3) readRepeatedFixed32(f, back);
            else if (f.num == 4) readRepeatedVarint(f, snrBack);
        }
        std::string s = d.wantResponse || d.requestId == 0 ? "traceroute request: " : "traceroute reply: ";
        s += d.wantResponse || d.requestId == 0 ? fromId : nodeStr(to);
        for (uint32_t n : route) s += " > " + meshtasticNodeIdString(n);
        s += " > " + (d.wantResponse || d.requestId == 0 ? nodeStr(to) : fromId);
        if (!snr.empty()) {
            s += " (SNR";
            for (uint64_t v : snr) s += meshFmt(" %.1f", (int32_t)(uint32_t)v / 4.0);
            s += " dB)";
        }
        if (!back.empty()) s += meshFmt(", back %zu hops", back.size());
        out.detail = s;
        break;
    }
    case 71: {                                                          // NEIGHBORINFO_APP
        Reader rd(d.payload, d.payloadLen);
        Field f;
        std::string s;
        int count = 0;
        while (rd.next(f)) {
            if (f.num == 4 && f.wire == 2) {
                Reader nr(f.data, f.len);
                Field g;
                uint32_t id = 0;
                float snr = 0;
                while (nr.next(g)) { if (g.num == 1) id = (uint32_t)g.value; else if (g.num == 2 && g.wire == 5) snr = g.asFloat(); }
                if (count < 4) s += (s.empty() ? "" : ", ") + meshFmt("%s %.1f dB", meshtasticNodeIdString(id).c_str(), snr);
                count++;
            }
        }
        out.detail = meshFmt("%d neighbors", count) + (s.empty() ? "" : ": " + s);
        break;
    }
    default:
        out.detail = meshFmt("%zu bytes", d.payloadLen);
        break;
    }
}

} // namespace

MeshDecodeResult MeshProto::decodeMeshtastic(const uint8_t* p, size_t n, const MeshRadioInfo&) const {
    MeshDecodeResult r;
    MeshPacketInfo& pi = r.packet;
    pi.protocol = MeshProtocol::Meshtastic;
    pi.size = n;
    if (n < 16) { pi.note = "too short for a Meshtastic header"; return r; }
    const uint32_t to = le32(p), from = le32(p + 4), id = le32(p + 8);
    const uint8_t flags = p[12], chan = p[13];
    pi.from = nodeStr(from);
    pi.to = nodeStr(to);
    pi.packetId = id;
    pi.hopLimit = flags & 7;
    pi.hopStart = flags >> 5;
    pi.wantAck = (flags & 8) != 0;
    pi.viaMqtt = (flags & 0x10) != 0;
    pi.channelHash = chan;
    pi.nextHop = p[14];
    pi.relayNode = p[15];
    pi.channel = meshFmt("0x%02x", chan);
    r.ok = true;
    const uint8_t* body = p + 16;
    const size_t m = n - 16;
    if (m == 0) { pi.type = "EMPTY"; pi.note = "no payload"; return r; }

    std::vector<MeshtasticChannel> chans;
    {
        std::lock_guard<std::mutex> g(s_->m);
        chans = s_->mt;
    }
    // channels whose hash matches first, then every other known key (the hash is only a hint; a channel with a custom name and
    // the default key is common). Plausibility is checked on the decrypted Data either way.
    bool hashMatched = false;
    std::vector<uint8_t> plain(m);
    for (int pass = 0; pass < 2; pass++) {
        for (const MeshtasticChannel& c : chans) {
            const bool match = c.hash == chan;
            if ((pass == 0) != match) continue;
            if (match) hashMatched = true;
            std::memcpy(plain.data(), body, m);
            if (!c.key.empty()) {
                uint8_t nonce[16] = {};
                for (int i = 0; i < 4; i++) { nonce[i] = (uint8_t)(id >> (8 * i)); nonce[8 + i] = (uint8_t)(from >> (8 * i)); }
                if (!aesCtr(c.key.data(), c.key.size(), nonce, plain.data(), m)) continue;
            }
            DataMsg d;
            if (!parseData(plain.data(), m, d)) continue;
            pi.decrypted = true;
            pi.portnum = d.portnum;
            pi.type = portName(d.portnum);
            if (match) pi.channel = c.name;
            else pi.note = "decrypted with the key of channel " + c.name + " (header hash differs)";
            decodePort(d, from, to, pi, r);
            return r;
        }
    }
    pi.type = "ENCRYPTED";
    if (chan == 0 && to != 0xFFFFFFFFu && !hashMatched) pi.note = "encrypted (direct)";
    else if (hashMatched) pi.note = "wrong key or damaged payload";
    else pi.note = meshFmt("unknown channel 0x%02x", chan);
    return r;
}

// ---- builders ----
std::vector<uint8_t> meshtasticData(int portnum, const std::vector<uint8_t>& payload, bool wantResponse, uint32_t dest, uint32_t source,
                                    uint32_t requestId, uint32_t replyId, uint32_t emoji) {
    Writer w;
    w.varint(1, (uint64_t)portnum);
    if (!payload.empty()) w.bytes(2, payload);
    if (wantResponse) w.boolean(3, true);
    if (dest) w.fixed32(4, dest);
    if (source) w.fixed32(5, source);
    if (requestId) w.fixed32(6, requestId);
    if (replyId) w.fixed32(7, replyId);
    if (emoji) w.fixed32(8, emoji);
    w.varint(9, 0);                                     // bitfield (the firmware sends it; 0 = not OK to MQTT), see firmware test_crypto
    return w.data();
}

std::vector<uint8_t> meshtasticRaw(uint32_t from, uint32_t to, uint32_t packetId, int hopLimit, int hopStart, bool wantAck,
                                   const std::string& channelName, const std::vector<uint8_t>& key, const std::vector<uint8_t>& dataProto) {
    std::vector<uint8_t> out;
    if (dataProto.size() > 255 - 16) return out;
    put32(out, to);
    put32(out, from);
    put32(out, packetId);
    out.push_back((uint8_t)((hopLimit & 7) | (wantAck ? 8 : 0) | ((hopStart & 7) << 5)));
    out.push_back(meshtasticChannelHash(channelName, key));
    out.push_back(0);                                   // next_hop: no preference
    out.push_back((uint8_t)(from & 0xff));              // relay_node: the originator's last byte
    std::vector<uint8_t> body = dataProto;
    if (!key.empty()) {
        uint8_t nonce[16] = {};
        for (int i = 0; i < 4; i++) { nonce[i] = (uint8_t)(packetId >> (8 * i)); nonce[8 + i] = (uint8_t)(from >> (8 * i)); }
        aesCtr(key.data(), key.size(), nonce, body.data(), body.size());
    }
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

namespace {
std::vector<uint8_t> onDefault(const MeshtasticNode& from, uint32_t to, uint32_t id, int hl, int hs, bool wantAck, const std::vector<uint8_t>& data) {
    const std::vector<uint8_t> key(meshtasticDefaultKey(), meshtasticDefaultKey() + 16);
    return meshtasticRaw(from.num, to, id, hl, hs, wantAck, "LongFast", key, data);
}
}

std::vector<uint8_t> meshtasticTextEx(const MeshtasticNode& from, uint32_t to, uint32_t id, int hl, int hs, const std::string& text,
                                      uint32_t replyId, bool wantAck) {
    const std::vector<uint8_t> t(text.begin(), text.end());
    return onDefault(from, to, id, hl, hs, wantAck, meshtasticData(1, t, false, 0, 0, 0, replyId));
}

std::vector<uint8_t> meshtasticText(const MeshtasticNode& from, uint32_t to, uint32_t id, int hl, int hs, const std::string& text) {
    return meshtasticTextEx(from, to, id, hl, hs, text, 0, false);
}

std::vector<uint8_t> meshtasticNodeInfo(const MeshtasticNode& from, uint32_t id, int hl, int hs) {
    Writer u;
    u.string(1, meshtasticNodeIdString(from.num));
    u.string(2, from.longName);
    u.string(3, from.shortName);
    u.varint(5, (uint64_t)from.hwModel);
    if (from.role) u.varint(7, (uint64_t)from.role);
    return onDefault(from, 0xFFFFFFFFu, id, hl, hs, false, meshtasticData(4, u.data()));
}

std::vector<uint8_t> meshtasticPositionEx(const MeshtasticNode& from, uint32_t id, int hl, int hs, uint32_t unixTime, int precisionBits) {
    int32_t lat = (int32_t)std::lround(from.lat * 1e7), lon = (int32_t)std::lround(from.lon * 1e7);
    if (precisionBits > 0 && precisionBits < 32) {      // PositionModule.cpp computeImpreciseLatLon: truncate, then centre the cell
        const uint32_t mask = 0xFFFFFFFFu << (32 - precisionBits);
        const uint32_t half = 1u << (31 - precisionBits);
        lat = (int32_t)(((uint32_t)lat & mask) + half);
        lon = (int32_t)(((uint32_t)lon & mask) + half);
    }
    Writer w;
    w.sfixed32(1, lat);
    w.sfixed32(2, lon);
    w.int32(3, (int32_t)std::lround(from.altM));
    if (unixTime) w.fixed32(4, unixTime);
    w.varint(5, 2);                                     // LOC_INTERNAL
    w.varint(19, 8);                                    // sats_in_view
    w.varint(23, (uint64_t)precisionBits);
    return onDefault(from, 0xFFFFFFFFu, id, hl, hs, false, meshtasticData(3, w.data()));
}

std::vector<uint8_t> meshtasticPosition(const MeshtasticNode& from, uint32_t id, int hl, int hs, uint32_t unixTime) {
    return meshtasticPositionEx(from, id, hl, hs, unixTime, 32);
}

std::vector<uint8_t> meshtasticTelemetry(const MeshtasticNode& from, uint32_t id, int hl, int hs, double battery, double voltage,
                                         double chUtil, double airTx, uint32_t uptime) {
    Writer dm;                                          // DeviceTelemetry.cpp: all five fields are sent
    dm.varint(1, (uint64_t)std::lround(battery));
    dm.floating(2, (float)voltage);
    dm.floating(3, (float)chUtil);
    dm.floating(4, (float)airTx);
    dm.varint(5, uptime);
    Writer t;
    t.bytes(2, dm.data());                              // Telemetry.time (field 1) is left out: the builder has no clock
    return onDefault(from, 0xFFFFFFFFu, id, hl, hs, false, meshtasticData(67, t.data()));
}

std::vector<uint8_t> meshtasticEnvTelemetry(const MeshtasticNode& from, uint32_t id, int hl, int hs, double tempC, double hum, double hpa) {
    Writer em;
    em.floating(1, (float)tempC);
    em.floating(2, (float)hum);
    em.floating(3, (float)hpa);
    Writer t;
    t.bytes(3, em.data());
    return onDefault(from, 0xFFFFFFFFu, id, hl, hs, false, meshtasticData(67, t.data()));
}

std::vector<uint8_t> meshtasticRoutingAck(const MeshtasticNode& from, uint32_t to, uint32_t id, int hl, int hs, uint32_t requestId, int err) {
    Writer w;
    if (err) w.varint(3, (uint64_t)err);                // error NONE (0) is the proto3 default and is not sent
    return onDefault(from, to, id, hl, hs, false, meshtasticData(5, w.data(), false, 0, 0, requestId));
}

std::vector<uint8_t> meshtasticTraceroute(const MeshtasticNode& from, uint32_t to, uint32_t id, int hl, int hs, bool reply, uint32_t requestId,
                                          const std::vector<uint32_t>& route, const std::vector<int>& snrTowards,
                                          const std::vector<uint32_t>& routeBack, const std::vector<int>& snrBack) {
    Writer w;
    w.packedFixed32(1, route);
    std::vector<uint64_t> a, b;
    for (int v : snrTowards) a.push_back((uint64_t)(int64_t)v);
    for (int v : snrBack) b.push_back((uint64_t)(int64_t)v);
    w.packedVarint(2, a);
    w.packedFixed32(3, routeBack);
    w.packedVarint(4, b);
    return onDefault(from, to, id, hl, hs, false, meshtasticData(70, w.data(), !reply, 0, 0, reply ? requestId : 0));
}

std::vector<uint8_t> meshtasticNeighborInfo(const MeshtasticNode& from, uint32_t id, int hl, int hs, uint32_t interval, const std::vector<MeshNeighbor>& nb) {
    Writer w;
    w.varint(1, from.num);
    w.varint(2, from.num);
    w.varint(3, interval);
    for (const MeshNeighbor& x : nb) {
        Writer m;
        m.varint(1, x.node);
        m.floating(2, x.snr);
        m.varint(4, interval);
        w.bytes(4, m.data());
    }
    return onDefault(from, 0xFFFFFFFFu, id, hl, hs, false, meshtasticData(71, w.data()));
}

} // namespace dect2
