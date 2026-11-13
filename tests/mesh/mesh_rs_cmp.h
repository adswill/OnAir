// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Shared by tests/mesh/test_mesh_proto_rs.cpp and rs_part2_mesh.cpp: the mesh packet layer (dect2::, Rust-backed) and the digest every decode result goes into.
#pragma once
#include "dect2/mesh/mesh_proto.h"
#include "../common/golden.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace A = dect2;

extern golden::Dual dig;
extern std::mt19937 g;

inline uint32_t r32() { return g(); }
inline uint8_t rb() { return (uint8_t)g(); }
inline uint64_t r64() { const uint64_t hi = (uint64_t)g() << 32; return hi | g(); }   // (two draws in one expression would be evaluated in an unspecified order)
inline long iterations() { const char* e = getenv("ONAIR_DIFF_ITERS"); return e ? atol(e) : 100000; }

// Every field of a decode result goes into the digest (doubles rounded by the digest).
template <class X> void addNode(const X& a) {
    dig.add((int)a.protocol, a.nodeId, a.longName, a.shortName, a.hwModel, a.role, a.hasPosition, a.lat, a.lon, a.altM, a.batteryPct, a.voltage);
    dig.add(a.hasEnv, a.tempC, a.humidity, a.pressureHpa, a.publicKey, a.positionTime, a.channelUtilPct, a.airUtilTxPct, a.uptimeS, a.sats);
    dig.add(a.signatureChecked, a.signatureOk);
}
template <class X> void addMsg(const X& a) {
    dig.add((int)a.protocol, a.channel, a.from, a.to, a.text, a.hops, a.packetId, a.replyId, a.senderTime);
}
template <class X> void addRes(const X& a) {
    const auto& p = a.packet;
    dig.add(a.ok, (int)p.protocol, p.type, p.from, p.to, p.packetId, p.hopLimit, p.hopStart, p.channel, p.decrypted, p.note, p.size, p.detail, p.portnum);
    dig.add(p.channelHash, p.wantAck, p.viaMqtt, p.nextHop, p.relayNode, p.routeType, p.payloadType, p.payloadVersion, p.pathHashCount, p.pathHashSize, p.path);
    dig.add((uint64_t)a.nodes.size(), (uint64_t)a.messages.size());
    for (const auto& n : a.nodes) addNode(n);
    for (const auto& m : a.messages) addMsg(m);
    dig.add(a.hasAprs, a.aprsSource, a.aprsDest, a.aprsPath, a.aprsInfo, a.batteryPct, a.raw, a.aprs.type, a.aprs.summary, a.aprs.comment, a.aprs.text);
    dig.add(a.aprs.name, a.aprs.hasPos, a.aprs.lat, a.aprs.lon);
    const auto& x = a.lorawan;
    dig.add(a.hasLorawan, x.mtype, x.major, x.uplink, x.confirmed, x.devAddr, x.netIdType, x.nwkId, x.netId, x.network, x.adr, x.adrAckReq, x.ack);
    dig.add(x.fpending, x.classB, x.foptsLen, x.fcnt, x.macCommands, x.macDetail, x.devStatusBattery, x.devStatusMargin, x.fport, x.frmLen, x.mic);
    dig.add(x.joinEui, x.devEui, x.devNonce, x.rejoinType, x.rejoinNetId, x.vendor);
}

// Random protobuf: n fields with numbers from `nums` (or any), wire types 0 / 1 / 2 / 5 mostly, values sometimes extreme; `garbage` adds invalid bytes.
std::vector<uint8_t> randProto(int fields, const std::vector<int>& nums, int garbagePct);
std::vector<uint8_t> mutateBytes(std::vector<uint8_t> v);
int runMeshProto2();
