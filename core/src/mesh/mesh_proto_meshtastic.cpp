// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Meshtastic packets: the 16 byte header, AES-CTR with the channel key, the Data protobuf and the port payloads.
// Sources: meshtastic/firmware src/mesh/RadioInterface.h (PacketHeader, flag masks), src/mesh/CryptoEngine.cpp (initNonce,
// encryptAESCtr: nonce = packet id as 64 bit little endian, from node, 4 byte counter), src/mesh/Channels.cpp (hash),
// src/modules/{NodeInfoModule,PositionModule,Telemetry/DeviceTelemetry}.cpp (what the firmware sends); meshtastic/protobufs
// mesh.proto (Data, Position, User, Routing, RouteDiscovery, NeighborInfo), portnums.proto, telemetry.proto, config.proto.
#include "mesh_proto_internal.h"
#include "dect2/mesh/mesh_crypto.h"
#include "dect2/mesh/mesh_pb.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include "dect2/common/endian.h"

namespace dect2 {

using namespace meshcrypto;
using namespace meshpb;

namespace {

void put32(std::vector<uint8_t>& v, uint32_t x) { dect2::endian::putLe32(v, x); }

} // namespace


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
