// Golden-vector test of the mesh packet layer (rust/onair_mesh behind dect2::), part 1: the protobuf reader, the Meshtastic name / key helpers and the Meshtastic
// packet decoder on random and structured protobuf, every port payload (text, position, node info, telemetry, routing, traceroute, neighbours) encrypted with the
// default key and with a user key, mutated and truncated packets, headers of every length. ONAIR_DIFF_ITERS (default 100000, 200000 verified) per family. Part 2 is
// rs_part2_mesh.cpp. Every result goes into a digest (tests/common/golden.h; doubles rounded to 1e-6) compared with the one recorded from the C++ mesh layer the
// Rust code replaced.
#include "mesh_rs_cmp.h"
#include "../common/ordered.h"
#include "dect2/mesh/mesh_pb.h"
#include "dect2/mesh/mesh_crypto.h"

golden::Dual dig;
std::mt19937 g(31337);

std::vector<uint8_t> mutateBytes(std::vector<uint8_t> v) {
    for (int k = (int)(g() % 4); k > 0; k--) {
        if (v.empty()) { v.push_back(rb()); continue; }
        switch (g() % 6) {
        case 0: v[g() % v.size()] = rb(); break;
        case 1: v[g() % v.size()] ^= (uint8_t)(1u << (g() % 8)); break;
        case 2: v.erase(v.begin() + (long)(g() % v.size())); break;
        case 3: { const size_t at = g() % (v.size() + 1); v.insert(v.begin() + (long)at, rb()); break; }   // the position first, then the byte (two draws in one call would be unordered)
        case 4: v.resize(g() % (v.size() + 1)); break;
        default: v.push_back(rb()); break;
        }
    }
    return v;
}

static void putVar(std::vector<uint8_t>& v, uint64_t x) { while (x >= 0x80) { v.push_back((uint8_t)(x | 0x80)); x >>= 7; } v.push_back((uint8_t)x); }

std::vector<uint8_t> randProto(int fields, const std::vector<int>& nums, int garbagePct) {
    std::vector<uint8_t> v;
    for (int i = 0; i < fields; i++) {
        const uint32_t num = nums.empty() ? 1 + g() % 30 : (uint32_t)nums[g() % nums.size()];
        const int wire = g() % 8 < 3 ? 0 : g() % 4 == 0 ? 2 : g() % 3 == 0 ? 1 : g() % 2 ? 5 : 2;
        putVar(v, (uint64_t)num << 3 | (uint64_t)wire);
        const uint64_t big = (g() % 6 == 0) ? r64() : (g() % 3 == 0 ? g() : g() % 300);
        if (wire == 0) putVar(v, big);
        else if (wire == 1) for (int k = 0; k < 8; k++) v.push_back(rb());
        else if (wire == 5) { const uint32_t f = g() % 4 == 0 ? g() : (uint32_t)(g() % 2000000 - 1000000) * 17; for (int k = 0; k < 4; k++) v.push_back((uint8_t)(f >> (8 * k))); }
        else { const size_t n = g() % 4 == 0 ? g() % 40 : g() % 12; putVar(v, n); for (size_t k = 0; k < n; k++) v.push_back((uint8_t)(g() % 3 ? 32 + g() % 90 : g())); }
    }
    if ((int)(g() % 100) < garbagePct) v = mutateBytes(v);
    return v;
}

// ---- protobuf reader ----
static void testPb(long iters) {
    namespace PA = A::meshpb;
    for (long i = 0; i < iters; i++) {
        std::vector<uint8_t> d = (i % 3 == 0) ? std::vector<uint8_t>() : randProto(1 + (int)(g() % 8), {}, 40);
        if (i % 3 == 0) { const size_t n = g() % 40; for (size_t k = 0; k < n; k++) d.push_back(rb()); }
        PA::Reader a(d.empty() ? nullptr : d.data(), d.size());
        for (int step = 0; step < 40; step++) {
            PA::Field x;
            const bool ra = a.next(x);
            dig.add(ra, a.ok(), a.atEnd());
            if (!ra) break;
            dig.add(x.num, x.wire, x.value, x.len, x.data == nullptr, x.data ? (int64_t)(x.data - d.data()) : (int64_t)-1, x.asInt32(), x.asSint32(), x.asString());
            dig.add(x.asFloat() == x.asFloat() ? 0 : 1);
            if (x.asFloat() == x.asFloat()) dig.add(x.asFloat());
            std::vector<uint32_t> p32;
            std::vector<uint64_t> p64;
            PA::readRepeatedFixed32(x, p32);
            PA::readRepeatedVarint(x, p64);
            dig.add(p32);
            dig.add(p64);
        }
    }
}

// ---- Meshtastic helpers ----
static void testHelpers(long iters) {
    for (int hw = -5; hw < 400; hw++) dig.add(A::meshtasticHwModelName(hw));
    for (int hw = 0; hw < 400; hw++) {
        const std::string n = A::meshtasticHwModelName(hw);
        dig.add(A::meshtasticHwModelValue(n));
    }
    dig.add(A::meshtasticHwModelValue("NOPE"), A::meshtasticHwModelValue(""));
    for (long i = 0; i < iters; i++) {
        const uint32_t n = r32();
        dig.add(A::meshtasticNodeIdString(n));
        std::string name;
        for (size_t k = g() % 20; k > 0; k--) name += (char)rb();
        std::vector<uint8_t> key(g() % 40);
        for (auto& b : key) b = rb();
        dig.add(A::meshtasticChannelHash(name, key));
        std::vector<uint8_t> psk(g() % 36), ka;
        for (auto& b : psk) b = (g() % 4 == 0) ? (uint8_t)(g() % 4) : rb();
        const bool oa = A::meshtasticExpandPsk(psk, ka);
        dig.add(oa, ka);
    }
}

// ---- Meshtastic packets ----
static A::MeshtasticNode node(uint32_t num) {
    A::MeshtasticNode n;
    n.num = num;
    for (size_t k = g() % 30; k > 0; k--) n.longName += (char)(g() % 5 ? 32 + g() % 90 : g());
    for (size_t k = g() % 6; k > 0; k--) n.shortName += (char)(32 + g() % 90);
    n.hwModel = (int)(g() % 130);
    n.lat = (double)((int)(g() % 180000000) - 90000000) / 1e6;
    n.lon = (double)((int)(g() % 360000000) - 180000000) / 1e6;
    n.altM = (double)((int)(g() % 9000) - 100);
    n.role = (int)(g() % 14);
    return n;
}

static std::vector<uint8_t> portPayload(int port) {
    switch (port) {
    case 1: { std::vector<uint8_t> t(g() % 200); for (auto& b : t) b = (uint8_t)(g() % 5 ? 32 + g() % 90 : g()); return t; }
    case 3: return randProto(1 + (int)(g() % 9), {1, 2, 3, 4, 5, 7, 9, 19, 23, 30}, 25);
    case 4: return randProto(1 + (int)(g() % 8), {1, 2, 3, 4, 5, 6, 7, 8}, 25);
    case 67: return randProto(1 + (int)(g() % 4), {1, 2, 3, 6}, 25);
    case 5: return randProto(g() % 4, {1, 2, 3}, 25);
    case 70: return randProto(1 + (int)(g() % 5), {1, 2, 3, 4}, 25);
    case 71: return randProto(1 + (int)(g() % 5), {1, 2, 3, 4}, 25);
    default: { std::vector<uint8_t> t(g() % 60); for (auto& b : t) b = rb(); return t; }
    }
}

static void testMeshtastic(long iters) {
    A::MeshProto pa;
    // two user channels: the default key under another name, and a 32 byte key
    const char* kSecretB64 = "q83vASNFZ4mrze8BI0VniavN7wEjRWeJq83vASNFZ4k=";
    pa.addMeshtasticChannel("Friends", "1PG7OiApB1nwvP+rz05pAQ==");
    pa.addMeshtasticChannel("Secret", kSecretB64);
    std::vector<uint8_t> keyDefault, keySecret;
    A::meshtasticExpandPsk({1}, keyDefault);
    dect2::meshcrypto::base64Decode(kSecretB64, keySecret);
    const int ports[] = {1, 3, 4, 67, 5, 70, 71, 0, 8, 66, 72, 256};
    const A::MeshRadioInfo radio{};
    long decoded = 0, withNode = 0, withMsg = 0;
    for (long i = 0; i < iters; i++) {
        std::vector<uint8_t> pkt;
        const int mode = (int)(g() % 20);
        const uint32_t from = r32() | 1, to = g() % 3 ? 0xFFFFFFFFu : r32(), id = r32();
        const int hl = (int)(g() % 8), hs = (int)(g() % 8);
        if (mode < 13) {
            // a port payload with random content inside a Data protobuf, encrypted with the default or a user key
            int port = ports[g() % (sizeof ports / sizeof *ports)];
            if (g() % 8 == 0) port = (int)(1 + g() % 600);
            const std::vector<uint8_t> payload = portPayload(port);
            const bool wantResp = g() % 4 == 0;
            std::vector<uint8_t> data = ORD_CALL(A::meshtasticData, port, payload, wantResp, g() % 5 ? 0 : r32(), g() % 5 ? 0 : r32(), g() % 3 ? 0 : r32(), g() % 4 ? 0 : r32());   // ORD_*: draws in a fixed order (arguments and operands are not evaluated in one)
            if (g() % 12 == 0) data = mutateBytes(data);
            const int k = (int)(g() % 6);
            const std::string chName = k == 0 ? "Friends" : k == 1 ? "Secret" : "LongFast";
            pkt = A::meshtasticRaw(from, to, id, hl, hs, g() % 2, chName, k == 1 ? keySecret : keyDefault, data);
            if (!pkt.empty() && g() % 15 == 0) pkt = mutateBytes(pkt);
        } else if (mode == 13) {
            pkt = ORD_CALL(A::meshtasticText, node(from), to, id, hl, hs, std::string((size_t)(g() % 90), 'x'));
        } else if (mode == 14) {
            pkt = A::meshtasticNodeInfo(node(from), id, hl, hs);
        } else if (mode == 15) {
            pkt = ORD_CALL(A::meshtasticPositionEx, node(from), id, hl, hs, g() % 2 ? r32() : 0, (int)(g() % 36));
        } else if (mode == 16) {
            pkt = ORD_CALL(A::meshtasticTelemetry, node(from), id, hl, hs, (double)(g() % 105), 2.5 + (double)(g() % 200) / 100.0, (double)(g() % 1000) / 10.0, (double)(g() % 100) / 10.0, r32());
        } else if (mode == 17) {
            std::vector<uint32_t> route(g() % 5), back(g() % 4);
            std::vector<int> s1(g() % 5), s2(g() % 4);
            for (auto& x : route) x = r32();
            for (auto& x : back) x = r32();
            for (auto& x : s1) x = (int)(g() % 80) - 40;
            for (auto& x : s2) x = (int)(g() % 80) - 40;
            pkt = ORD_CALL(A::meshtasticTraceroute, node(from), to, id, hl, hs, g() % 2, g() % 2 ? 0 : r32(), route, s1, back, s2);
        } else if (mode == 18) {
            std::vector<A::MeshNeighbor> nb(g() % 7);
            for (auto& x : nb) { x.node = r32(); x.snr = (float)((int)(g() % 400) - 200) / 10.f; }
            pkt = g() % 2 ? ORD_CALL(A::meshtasticNeighborInfo, node(from), id, hl, hs, r32() % 3600, nb)
                          : ORD_CALL(A::meshtasticEnvTelemetry, node(from), id, hl, hs, (double)((int)(g() % 900) - 300) / 10.0, (double)(g() % 1000) / 10.0, 900.0 + (double)(g() % 1500) / 10.0);
        } else {
            pkt.resize(g() % 40);                                           // anything: short headers, random bodies
            for (auto& b : pkt) b = rb();
            if (pkt.size() >= 16 && g() % 2) pkt[13] = (uint8_t)(g() % 2 ? 0x08 : rb());
        }
        if (pkt.size() > 255) pkt.resize(255);
        const auto ra = pa.decodeMeshtastic(pkt.data(), pkt.size(), radio);
        addRes(ra);
        decoded += ra.packet.decrypted;
        withNode += !ra.nodes.empty();
        withMsg += !ra.messages.empty();
    }
    printf("  meshtastic: %ld packets, %ld decrypted, %ld with a node update, %ld with a text message\n", iters, decoded, withNode, withMsg);
}

int main() {
    const long iters = iterations();
    testPb(iters);
    testHelpers(iters);
    testMeshtastic(iters);
    const int rc2 = runMeshProto2();
    if (rc2) return 1;
    // recorded again after the decoder kept battery level 101 ("powered") and cut the text detail on a whole UTF-8 character

    // recorded again for 3.0: the APRS message number stops at a '}' (APRS 1.1 reply-ack, fix-packet-030, rust/onair_ax25/src/aprs.rs)
    // recorded again 2026-11-10: Meshtastic waypoints, air quality, power telemetry, alert, range test, paxcounter, store and forward and map report ports are now read (was 0x06855ab108a90207, 0xa47f503349624973)
    return golden::checkDual("mesh_proto_rs", dig, 0x62843d8cbcf9a988ull, 0xc098570f38c0b568ull, !getenv("ONAIR_DIFF_ITERS"));
}
