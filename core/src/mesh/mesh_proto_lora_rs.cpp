// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// LoRa APRS, MeshCom and LoRaWAN packet layers on top of the Rust crate rust/onair_mesh: the decoders of mesh_proto.h;
// mesh_proto_aprs.cpp and mesh_proto_lorawan.cpp keep the settings tables and packet builders. The APRS information field itself
// is parsed by aprs::parse here.
#include "mesh_proto_internal.h"
#include "mesh_rs_util.h"
#include <cstring>

namespace dect2 {

using namespace meshrs;

namespace {

std::string cleanText(const std::string& s) {
    return text<2048>([&](uint8_t* b, size_t n) { return onair_mesh_clean_text((const uint8_t*)s.data(), s.size(), b, n); });
}

std::string baseCall(const std::string& c) {
    const size_t d = c.find('-');
    return d == std::string::npos ? c : c.substr(0, d);
}

void cleanInfo(aprs::Info& a) {
    a.comment = cleanText(a.comment); a.text = cleanText(a.text); a.name = cleanText(a.name); a.summary = cleanText(a.summary);
}

} // namespace

std::string meshPrintable(const uint8_t* p, size_t n) {
    std::string o(n * 4 + 1, '\0');
    const size_t len = onair_mesh_printable(p, n, (uint8_t*)&o[0], o.size());
    o.resize(len == (size_t)-1 ? 0 : len);
    return o;
}

MeshDecodeResult meshDecodeLoraAprs(const uint8_t* p, size_t n) {
    MeshDecodeResult r;
    r.packet.protocol = MeshProtocol::LoraAprs;
    r.packet.size = n;
    auto a = std::make_unique<OnairMeshLoraAprs>();
    onair_mesh_lora_aprs(p, n, a.get());
    r.raw = str(a->raw);
    if (!a->ok) { r.packet.note = str(a->note); return r; }
    const std::string src = str(a->src), dest = str(a->dest), path = str(a->path);
    r.ok = true;
    r.hasAprs = true;
    r.aprsSource = src; r.aprsDest = dest; r.aprsPath = path;
    r.aprsInfo = str(a->info);
    r.packet.from = src; r.packet.to = dest; r.packet.path = path;
    r.packet.decrypted = true;
    if (aprs::parse(baseCall(dest), r.aprsInfo, r.aprs)) {
        cleanInfo(r.aprs);
        r.packet.type = r.aprs.type;
        r.packet.detail = r.aprs.summary;
        if (r.aprs.type == "Message" && !r.aprs.isAck && !r.aprs.isRej) {
            MeshTextMessage m;
            m.protocol = MeshProtocol::LoraAprs;
            m.channel = "APRS"; m.from = src; m.to = r.aprs.name; m.text = r.aprs.text;
            r.messages.push_back(m);
        }
    } else {
        r.aprs = aprs::Info();
        r.packet.type = "APRS?";
        r.packet.detail = cleanText(r.aprsInfo.substr(0, 80));
    }
    return r;
}

MeshDecodeResult meshDecodeMeshCom(const uint8_t* p, size_t n) {
    MeshDecodeResult r;
    r.packet.protocol = MeshProtocol::MeshCom;
    r.packet.size = n;
    auto m = std::make_unique<OnairMeshMeshCom>();
    onair_mesh_meshcom(p, n, m.get());
    r.raw = str(m->raw);
    if (n == 0) return r;
    r.packet.note = str(m->note);
    if (!m->ok) return r;
    r.ok = true;
    r.packet.type = str(m->type_name);
    r.packet.packetId = m->packet_id;
    r.packet.hopLimit = m->hop_limit;
    r.packet.decrypted = true;
    if (!m->has_aprs) { r.packet.detail = str(m->detail); return r; }          // ack
    const std::string src = str(m->src), destCall = str(m->dest_call);
    r.hasAprs = true;
    r.aprsSource = src;
    r.aprsPath = str(m->aprs_path);
    r.aprsDest = str(m->aprs_dest);
    r.aprsInfo = str(m->aprs_info);
    r.packet.from = src; r.packet.to = destCall; r.packet.path = r.aprsPath;
    if (m->kind == 1) {                                                        // text
        MeshTextMessage t;
        t.protocol = MeshProtocol::MeshCom;
        t.channel = str(m->channel); t.from = src; t.to = destCall; t.text = str(m->text); t.packetId = r.packet.packetId;
        r.messages.push_back(t);
        r.packet.detail = str(m->detail);
    } else if (m->kind == 2) {                                                 // position
        if (aprs::parse(baseCall(destCall), r.aprsInfo, r.aprs)) cleanInfo(r.aprs);
        else r.aprs = aprs::Info();
        r.batteryPct = m->battery_pct;
        r.packet.detail = (r.aprs.hasPos ? aprs::fmtLat(r.aprs.lat) + " " + aprs::fmtLon(r.aprs.lon) : std::string("position not readable")) +
                          (r.batteryPct >= 0 ? meshFmt(", battery %d %%", r.batteryPct) : std::string()) + " (" + str(m->tail) + ")";
    } else {
        r.packet.detail = str(m->detail);                                      // HEY
    }
    return r;
}

// ---- LoRaWAN ----
const char* lorawanMTypeName(int t) {
    static const char* const n[] = {"Join request", "Join accept", "Unconfirmed up", "Unconfirmed down", "Confirmed up", "Confirmed down", "Rejoin request", "Proprietary"};
    return t >= 0 && t < 8 ? n[t] : "?";
}

std::string lorawanMacName(uint8_t cid, bool uplink) { return text<64>([&](uint8_t* b, size_t n) { return onair_mesh_lorawan_mac_name(cid, uplink, b, n); }); }

std::string lorawanNetwork(uint32_t devAddr, int* typeOut, uint32_t* nwkOut, uint32_t* netOut) {
    int32_t kind = 0;
    const std::string s = text<64>([&](uint8_t* b, size_t n) { return onair_mesh_lorawan_network(devAddr, b, n, &kind, nwkOut, netOut); });
    if (typeOut) *typeOut = kind;
    return s;
}

const char* lorawanVendor(uint64_t eui) {
    static thread_local char buf[64];   // valid until the next call on this thread (the C++ returned string literals)
    const size_t n = onair_mesh_lorawan_vendor(eui, (uint8_t*)buf, sizeof buf - 1);
    buf[n == (size_t)-1 ? 0 : n] = 0;
    return buf;
}

std::string meshHexString(const uint8_t* p, size_t n) { return text<1024>([&](uint8_t* b, size_t cap) { return onair_mesh_hex_string(p, n, b, cap); }); }

std::string lorawanEui(uint64_t eui) { return text<32>([&](uint8_t* b, size_t n) { return onair_mesh_lorawan_eui(eui, b, n); }); }

MeshDecodeResult meshDecodeLorawan(const uint8_t* p, size_t n) {
    MeshDecodeResult r;
    r.packet.protocol = MeshProtocol::LoraWan;
    r.packet.size = n;
    auto l = std::make_unique<OnairMeshLorawan>();
    onair_mesh_lorawan_decode(p, n, l.get());
    MeshPacketInfo& pk = r.packet;
    MeshDecodeResult::Lorawan& w = r.lorawan;
    // everything the decoder had set when it stopped is copied, also for a payload it rejected (the reference does the same)
    w.mtype = l->mtype; w.major = l->major; w.uplink = l->uplink != 0; w.confirmed = l->confirmed != 0;
    pk.type = str(l->type_name); pk.note = str(l->note);
    pk.from = str(l->from); pk.to = str(l->to); pk.detail = str(l->detail); pk.packetId = l->packet_id;
    w.devAddr = l->dev_addr; w.netIdType = l->net_id_type; w.nwkId = l->nwk_id; w.netId = l->net_id; w.network = str(l->network);
    w.adr = l->adr != 0; w.adrAckReq = l->adr_ack_req != 0; w.ack = l->ack != 0; w.fpending = l->fpending != 0; w.classB = l->class_b != 0;
    w.foptsLen = l->fopts_len; w.fcnt = l->fcnt;
    for (uint32_t i = 0; i < l->mac_count && i < 16; i++) w.macCommands.push_back(str(l->mac_names[i]));
    w.macDetail = str(l->mac_detail);
    w.devStatusBattery = l->dev_status_battery; w.devStatusMargin = l->dev_status_margin;
    w.fport = l->fport; w.frmLen = l->frm_len; w.mic = l->mic;
    w.joinEui = l->join_eui; w.devEui = l->dev_eui; w.devNonce = l->dev_nonce; w.rejoinType = l->rejoin_type; w.rejoinNetId = l->rejoin_net_id;
    w.vendor = str(l->vendor);
    if (!l->ok) return r;
    r.ok = true;
    r.hasLorawan = true;
    pk.decrypted = false;
    return r;
}

} // namespace dect2
