// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Meshtastic and MeshCore packet layers and the Meshtastic name / key helpers on top of the Rust crate rust/onair_mesh: the functions of
// mesh_proto.h; mesh_proto.cpp, mesh_proto_meshtastic.cpp and mesh_proto_meshcore.cpp keep the key-dependent parts and the packet builders. Rust parses and formats; the
// channel tables, AES-CTR, AES-ECB / HMAC and Ed25519 stay here because they need keys.
#include "mesh_proto_internal.h"
#include "mesh_rs_util.h"
#include "dect2/common/rs_abi.h"
#include "dect2/mesh/mesh_crypto.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dect2 {

using namespace meshcrypto;
using namespace meshrs;

namespace {
struct AbiCheck {
    AbiCheck() {
        size_t s[10];
        onair_mesh_struct_sizes(s);
        dect2::rsAbiCheck("onair_mesh", s, {sizeof(OnairMeshNode), sizeof(OnairMeshMessage), sizeof(OnairMeshMtDecoded), sizeof(OnairMeshMcParsed), sizeof(OnairMeshMcOut), sizeof(OnairMeshMcGroup), sizeof(OnairMeshLorawan), sizeof(OnairMeshLoraAprs), sizeof(OnairMeshMeshCom), sizeof(OnairMeshPbField)});
    }
} abiCheck;
} // namespace

// ---- names and keys ----
std::string meshtasticNodeIdString(uint32_t num) { return text<16>([&](uint8_t* b, size_t n) { return onair_mesh_node_id(num, b, n); }); }

std::string meshtasticHwModelName(int hw) { return text<64>([&](uint8_t* b, size_t n) { return onair_mesh_hw_model_name(hw, b, n); }); }

int meshtasticHwModelValue(const std::string& name) { return onair_mesh_hw_model_value((const uint8_t*)name.data(), name.size()); }

uint8_t meshtasticChannelHash(const std::string& name, const std::vector<uint8_t>& key) {
    return onair_mesh_channel_hash((const uint8_t*)name.data(), name.size(), key.data(), key.size());
}

bool meshtasticExpandPsk(const std::vector<uint8_t>& psk, std::vector<uint8_t>& key) {
    uint8_t k[32];
    const int n = onair_mesh_expand_psk(psk.data(), psk.size(), k);
    key.clear();
    if (n < 0) return false;
    key.assign(k, k + n);
    return true;
}

// ---- Meshtastic ----
MeshDecodeResult MeshProto::decodeMeshtastic(const uint8_t* p, size_t n, const MeshRadioInfo&) const {
    MeshDecodeResult r;
    MeshPacketInfo& pi = r.packet;
    pi.protocol = MeshProtocol::Meshtastic;
    pi.size = n;
    OnairMeshMtHeader h;
    if (!onair_mesh_mt_header(p, n, &h)) { pi.note = "too short for a Meshtastic header"; return r; }
    auto nodeStr = [](uint32_t v) { return v == 0xFFFFFFFFu ? std::string("^all") : meshtasticNodeIdString(v); };
    const uint32_t to = h.to, from = h.from, id = h.id;
    const uint8_t chan = h.channel;
    pi.from = nodeStr(from);
    pi.to = nodeStr(to);
    pi.packetId = id;
    pi.hopLimit = h.flags & 7;
    pi.hopStart = h.flags >> 5;
    pi.wantAck = (h.flags & 8) != 0;
    pi.viaMqtt = (h.flags & 0x10) != 0;
    pi.channelHash = chan;
    pi.nextHop = h.next_hop;
    pi.relayNode = h.relay;
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
            auto d = std::make_unique<OnairMeshMtDecoded>();
            if (!onair_mesh_mt_decode(plain.data(), m, from, to, h.flags >> 5, h.flags & 7, id, d.get())) continue;
            pi.decrypted = true;
            pi.portnum = d->portnum;
            pi.type = str(d->type_name);
            if (match) pi.channel = c.name;
            else pi.note = "decrypted with the key of channel " + c.name + " (header hash differs)";
            pi.detail = str(d->detail);
            if (d->has_message) {
                MeshTextMessage t;
                t.protocol = MeshProtocol::Meshtastic;
                t.channel = pi.channel;
                message(d->message, t);
                r.messages.push_back(t);
            }
            if (d->has_node) {
                MeshNodeUpdate u;
                u.protocol = MeshProtocol::Meshtastic;
                u.nodeId = meshtasticNodeIdString(from);
                node(d->node, u);
                r.nodes.push_back(u);
            }
            return r;
        }
    }
    pi.type = "ENCRYPTED";
    if (chan == 0 && to != 0xFFFFFFFFu && !hashMatched) pi.note = "encrypted (direct)";
    else if (hashMatched) pi.note = "wrong key or damaged payload";
    else pi.note = meshFmt("unknown channel 0x%02x", chan);
    return r;
}

// ---- MeshCore ----
namespace {

// GRP_TXT / GRP_DATA (and the end-to-end types, tried as group text): the channel whose hash matches the first byte and whose MAC fits decrypts it
bool decodeGroupPayload(int type, int hcnt, const std::string& via, const uint8_t* pay, size_t m, const std::vector<MeshCoreChannel>& chans, MeshPacketInfo& pi,
                        MeshDecodeResult& r) {
    if (m < 4) return false;
    const uint8_t h = pay[0];
    pi.channelHash = h;
    pi.channel = meshFmt("0x%02x", h);
    bool any = false;
    for (const MeshCoreChannel& c : chans) {
        if (c.hash != h) continue;
        any = true;
        std::vector<uint8_t> d;
        if (!meshcoreMacThenDecrypt(c, pay + 1, m - 1, d)) continue;
        auto g = std::make_unique<OnairMeshMcGroup>();
        onair_mesh_mc_group_plain((uint8_t)type, (uint8_t)hcnt, (const uint8_t*)via.data(), via.size(), d.data(), d.size(), g.get());
        pi.decrypted = true;
        pi.channel = c.name;
        pi.type = str(g->type_name);
        pi.note = str(g->note);
        if (g->has_message) {
            MeshTextMessage t;
            t.protocol = MeshProtocol::MeshCore;
            t.channel = c.name;
            message(g->message, t);
            pi.from = t.from;
            pi.to = c.name;
            r.messages.push_back(t);
        }
        pi.detail = str(g->detail);
        return true;
    }
    if (!pi.decrypted) {
        OnairMeshS128 note;
        onair_mesh_mc_group_fail(h, any, &note);
        pi.note = str(note);
    }
    return false;
}

} // namespace

MeshDecodeResult MeshProto::decodeMeshCore(const uint8_t* p, size_t n, const MeshRadioInfo&) const {
    MeshDecodeResult r;
    MeshPacketInfo& pi = r.packet;
    pi.protocol = MeshProtocol::MeshCore;
    pi.size = n;
    OnairMeshMcParsed h;
    OnairMeshS128 note;
    if (!onair_mesh_mc_header(p, n, &h, &note)) { pi.note = str(note); return r; }
    pi.routeType = h.route;
    pi.payloadType = h.kind;
    pi.payloadVersion = h.ver;
    pi.pathHashCount = h.hcnt;
    pi.pathHashSize = h.hsz;
    pi.path = str(h.path);
    pi.type = str(h.type_name);
    r.ok = true;
    const uint8_t* pay = p + h.pay_off;
    const size_t m = h.m;
    const std::string via = str(h.via);
    bool sigOk = false;
    if (h.kind == 4 && m >= 32 + 4 + 64 + 1) {                         // ADVERT: the signature covers public key, time and the app data (at most 32 bytes)
        size_t al = m - 100;
        if (al > 32) al = 32;                                          // Mesh.cpp clamps to MAX_ADVERT_DATA_SIZE
        std::vector<uint8_t> msg(pay, pay + 36);                       // public key and the time stamp (little endian), as sent
        msg.insert(msg.end(), pay + 100, pay + 100 + al);
        sigOk = ed25519Verify(pay + 36, msg.data(), msg.size(), pay);
    }
    auto o = std::make_unique<OnairMeshMcOut>();
    onair_mesh_mc_decode(p, n, &h, sigOk, o.get());
    if (o->from.len) pi.from = str(o->from);
    if (o->to.len) pi.to = str(o->to);
    pi.decrypted = o->decrypted != 0;
    if (o->note.len) pi.note = str(o->note);
    if (o->detail.len) pi.detail = str(o->detail);
    if (o->clear_path) pi.path.clear();
    if (o->has_node) {
        MeshNodeUpdate u;
        u.protocol = MeshProtocol::MeshCore;
        u.nodeId = pi.from;
        node(o->node, u);
        r.nodes.push_back(u);
    }
    if (o->need_crypto) {
        std::vector<MeshCoreChannel> chans;
        { std::lock_guard<std::mutex> g(s_->m); chans = s_->mc; }
        if (h.kind == 5 || h.kind == 6) {
            decodeGroupPayload(h.kind, h.hcnt, via, pay, m, chans, pi, r);
        } else if (!decodeGroupPayload(5, h.hcnt, via, pay, m, chans, pi, r)) {   // end-to-end encrypted: dest hash + src hash + MAC + data
            pi.note = "encrypted (direct)";
            pi.detail = "to " + pi.to + " from " + pi.from + via;
        }
    }
    return r;
}

} // namespace dect2
