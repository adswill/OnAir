// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Golden-vector test of the mesh packet layer, part 2: MeshCore packets (adverts with valid / forged signatures, group text on the Public channel and a user channel,
// end-to-end headers, ACK, TRACE, CONTROL, random headers and bodies), LoRaWAN (every MType, FOpts MAC commands, join / rejoin requests, random and mutated
// frames, the name helpers), LoRa APRS and MeshCom frames. Same method as test_mesh_proto_rs.cpp: every result goes into its digest.
#include "mesh_rs_cmp.h"
#include "../common/ordered.h"
#include "dect2/mesh/mesh_crypto.h"

static std::string rndText(size_t n, const char* alphabet, size_t alen) { std::string s; for (size_t i = 0; i < n; i++) s += alphabet[g() % alen]; return s; }
#define RT(n, lit) rndText((n), lit, sizeof(lit) - 1)

// ---- MeshCore ----
static A::MeshCoreNode mcNode() {
    A::MeshCoreNode n;
    n.seed = (uint8_t)(1 + g() % 6);
    n.name = g() % 8 == 0 ? std::string() : RT(g() % 36, "abcdefghijklmnopqrstuvwxyz ABC-0123\t\xC3\xA4");
    n.lat = (double)((int)(g() % 180000) - 90000) / 1000.0;
    n.lon = g() % 6 == 0 ? 0.0 : (double)((int)(g() % 360000) - 180000) / 1000.0;
    if (g() % 9 == 0) n.lat = n.lon = 0;
    n.repeater = g() % 2;
    return n;
}

static void testMeshCore(long iters) {
    A::MeshProto pa;
    const char* sec = "00112233445566778899aabbccddeeff";
    pa.addMeshCoreChannel("#friends", "");
    pa.addMeshCoreChannel("Club", sec);
    uint8_t clubSecret[16], pubSecret[16];
    for (int i = 0; i < 16; i++) clubSecret[i] = (uint8_t)(i * 0x11);
    memcpy(pubSecret, A::meshcorePublicSecret(), 16);
    uint8_t hashSecret[32];
    dect2::meshcrypto::sha256((const uint8_t*)"#friends", 8, hashSecret);
    const A::MeshRadioInfo radio{};
    long adverts = 0, groups = 0, others = 0;
    for (long i = 0; i < iters; i++) {
        std::vector<uint8_t> pkt;
        const int mode = (int)(g() % 12);
        std::vector<uint8_t> path(g() % 4);
        for (auto& b : path) b = rb();
        if (mode <= 2) {
            pkt = ORD_CALL(A::meshcoreAdvert, mcNode(), r32());
            if (mode == 1) pkt = mutateBytes(pkt);
            if (mode == 2 && pkt.size() > 110) pkt.resize(102 + g() % (pkt.size() - 101));   // cut inside the app data
            adverts++;
        } else if (mode <= 5) {
            const uint8_t* s = g() % 3 == 0 ? pubSecret : g() % 2 ? clubSecret : hashSecret;
            pkt = ORD_CALL(A::meshcoreGroupText, s, mcNode(), r32(), RT(g() % 90, "hello world: ABC xyz\x01"), path);   // ORD_*: draws in a fixed order (arguments and operands are not evaluated in one)
            if (mode == 4 && !pkt.empty()) pkt = mutateBytes(pkt);
            groups++;
        } else if (mode == 6) {
            pkt = A::meshcoreAck(r32(), path);
            if (g() % 3 == 0) pkt = mutateBytes(pkt);
        } else {
            // a header with random fields and a random body (also the layouts of ADVERT / TRACE / CONTROL / ANON_REQ)
            const int route = (int)(g() % 4), type = (int)(g() % 16), ver = (int)(g() % 4);
            pkt.push_back((uint8_t)(route | type << 2 | ver << 6));
            if (route == 0 || route == 3) for (int k = 0; k < 4; k++) pkt.push_back(rb());
            const int hsz = (int)(g() % 12 == 0 ? 3 : g() % 3), hcnt = (int)(g() % 12);
            pkt.push_back((uint8_t)(hsz << 6 | hcnt));
            for (int k = 0; k < (hsz + 1) * hcnt; k++) pkt.push_back(rb());
            const size_t body = g() % 160;
            for (size_t k = 0; k < body; k++) pkt.push_back(rb());
            if (type == 4 && pkt.size() > 110) { pkt[pkt.size() - (pkt.size() - 100) + 0] = (uint8_t)(g() % 256); }
            if (g() % 10 == 0) pkt.resize(g() % (pkt.size() + 1));
            others++;
        }
        if (pkt.size() > 255) pkt.resize(255);
        const auto ra = pa.decodeMeshCore(pkt.data(), pkt.size(), radio);
        addRes(ra);
    }
    printf("  meshcore: %ld packets (%ld adverts, %ld group, %ld other)\n", iters, adverts, groups, others);
}

// ---- LoRaWAN ----
static std::vector<uint8_t> fopts() {
    static const uint8_t cids[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x20, 0x21, 0x7F, 0x80, 0xFF};
    std::vector<uint8_t> v;
    const size_t n = g() % 16;
    while (v.size() < n) { v.push_back(g() % 4 ? cids[g() % sizeof cids] : rb()); for (int k = (int)(g() % 6); k > 0 && v.size() < n; k--) v.push_back(rb()); }
    return v;
}

static void testLorawan(long iters) {
    long parsed = 0;
    for (long i = 0; i < iters; i++) {
        std::vector<uint8_t> f;
        const int mode = (int)(g() % 10);
        if (mode < 4) {
            A::LorawanTx t;
            t.mtype = (int)(g() % 8);
            t.devAddr = g() % 3 ? r32() : (0xFC014C00u | (g() & 0x3FF));
            if (g() % 4 == 0) { const uint32_t net = g() % 3 ? 0x26000000u : 0xE0000000u; t.devAddr = net | (g() & 0xFFFFFF); }
            t.adr = g() % 2; t.adrAckReq = g() % 2; t.ack = g() % 2; t.fpending = g() % 2; t.fcnt = (uint16_t)g();
            t.fopts = fopts();
            t.fport = g() % 4 == 0 ? -1 : (int)(g() % 256);
            t.frm.resize(g() % 40);
            for (auto& b : t.frm) b = rb();
            t.mic = r32();
            f = A::lorawanBuild(t);
        } else if (mode == 4) {
            {
                const uint64_t joinEui = r64();
                const uint64_t devEui = g() % 3 ? (0x70B3D57EDull << 28 | (r32() & 0xFFFFFFF)) : r64();
                const uint16_t devNonce = (uint16_t)g();
                f = A::lorawanJoinRequest(joinEui, devEui, devNonce, r32());
            }
        } else if (mode == 5) {
            f.push_back(0x60);                                           // rejoin request
            f.push_back((uint8_t)(g() % 4));
            const size_t n = g() % 4 == 0 ? g() % 30 : (f[1] == 1 ? 22 : 17);
            for (size_t k = 0; k < n; k++) f.push_back(rb());
        } else if (mode == 6) {
            f.push_back(0x20);                                           // join accept
            const size_t n = g() % 3 ? (g() % 2 ? 16 : 32) : g() % 40;
            for (size_t k = 0; k < n; k++) f.push_back(rb());
        } else {
            f.resize(g() % 40);
            for (auto& b : f) b = rb();
            if (!f.empty() && g() % 2) { const uint32_t hi3 = (g() % 8) << 5; f[0] = (uint8_t)(hi3 | (g() % 8 == 0 ? rb() & 0x1F : 0)); }
        }
        if (g() % 8 == 0) f = mutateBytes(f);
        const auto ra = A::meshDecodeLorawan(f.empty() ? nullptr : f.data(), f.size());
        addRes(ra);
        parsed += ra.ok;
        // the helpers
        const uint32_t da = r32();
        int ta = -2; uint32_t na = 11, ea = 22;
        dig.add(A::lorawanNetwork(da, &ta, &na, &ea), ta, na, ea);
        dig.add(A::lorawanMacName((uint8_t)i, i & 0x100));
        const uint64_t eui = i % 3 ? r64() : (0x70B3D57EDull << 28 | (r32() & 0xFFFFFFF));
        dig.add(std::string(A::lorawanVendor(eui)), A::lorawanEui(eui));
        dig.add(std::string(A::lorawanMTypeName((int)(i % 12) - 2)));
        dig.add(A::meshHexString(f.data(), f.size()));
    }
    for (uint32_t k = 0; k < 4096; k++) {          // every DevAddr prefix type and NwkID shape
        const uint32_t da = (k << 20) | (r32() & 0xFFFFF);
        int ta = -2; uint32_t na = 0, ea = 0;
        dig.add(A::lorawanNetwork(da, &ta, &na, &ea), ta, na, ea);
    }
    printf("  lorawan: %ld frames, %ld parsed\n", iters, parsed);
}

// ---- LoRa APRS and MeshCom ----
static std::string call() { return ORD_CAT(RT(1 + g() % 6, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"), (g() % 2 ? "-" + std::to_string(g() % 16) : std::string())); }
static std::string tnc2() {
    std::string s = ORD_CAT(call(), ">", (g() % 4 ? "APLRT1" : call()));
    for (int k = (int)(g() % 4); k > 0; k--) s += ORD_CAT(",", call(), (g() % 3 == 0 ? "*" : ""));
    s += ":";
    switch (g() % 5) {
    case 0: s += ORD_CAT("!", std::to_string(10 + g() % 79), "27.31N/0", std::to_string(10 + g() % 79), "15.50E>", RT(g() % 20, "test /A=000100")); break;
    case 1: s += ORD_CAT(":", (call() + "         ").substr(0, 9), ":", RT(g() % 30, "hello world "), "{", std::to_string(g() % 999)); break;
    case 2: s += ">" + RT(g() % 30, "status text "); break;
    case 3: s += ORD_CAT("=", RT(8, "0123456789"), "N/", RT(9, "0123456789"), "E-", RT(g() % 15, "caf\xC3\xA9 \xFF\x01")); break;
    default: s += RT(g() % 40, "abc XYZ!:>,*\r\n\xC3\xB6\x80"); break;
    }
    return s;
}

static void testLoraAprs(long iters) {
    long parsed = 0;
    for (long i = 0; i < iters; i++) {
        std::vector<uint8_t> f;
        const int mode = (int)(g() % 8);
        if (mode < 5) {
            const std::string s = tnc2();
            f = A::loraAprsFrame(s);
            if (g() % 4 == 0) f.push_back(g() % 3 == 0 ? 0 : g() % 2 ? '\r' : '\n');
            if (g() % 6 == 0) f = mutateBytes(f);
        } else if (mode == 5) {
            f = {0x3C, 0xFF, 0x01};
            const size_t n = g() % 50;
            for (size_t k = 0; k < n; k++) f.push_back((uint8_t)(g() % 3 ? "A>,:*-0 "[g() % 8] : rb()));
        } else {
            f.resize(g() % 30);
            for (auto& b : f) b = rb();
        }
        if (f.size() > 255) f.resize(255);
        const auto ra = A::meshDecodeLoraAprs(f.empty() ? nullptr : f.data(), f.size());
        addRes(ra);
        dig.add(A::meshPrintable(f.data(), f.size()));
        parsed += ra.ok;
    }
    printf("  lora aprs: %ld frames, %ld parsed\n", iters, parsed);
}

static void testMeshCom(long iters) {
    long parsed = 0, acks = 0;
    for (long i = 0; i < iters; i++) {
        std::vector<uint8_t> f;
        const int mode = (int)(g() % 10);
        if (mode < 6) {
            A::MeshComFrame m;
            const char types[] = {':', '!', '@'};
            m.type = types[g() % 3];
            m.msgId = r32(); m.maxHop = (int)(g() % 16); m.server = g() % 2; m.track = g() % 2; m.appOffline = g() % 2; m.mesh = g() % 2;
            m.sourcePath = call();
            for (int k = (int)(g() % 3); k > 0; k--) m.sourcePath += "," + call();
            m.dest = g() % 2 ? "*" : (g() % 2 ? call() : ORD_CAT(std::to_string(g() % 99999), ",", call()));
            if (m.type == '!') m.payload = ORD_CAT(std::to_string(10 + g() % 79), "27.31N/0", std::to_string(10 + g() % 79), "15.50E>", (g() % 2 ? "/B=" + std::to_string(g() % 150) : std::string()), " /A=000100");
            else m.payload = RT(g() % 90, "Hello World 123 \xC3\xA4\xC3\xB6 \xC3 ,*:!");
            m.hw = rb(); m.mod = rb(); m.fw = rb(); m.lastHw = rb(); m.subVersion = g() % 3 ? 'a' : (char)rb();
            f = A::meshcomBuild(m);
            if (g() % 5 == 0) f = mutateBytes(f);
        } else if (mode == 6) {
            f.assign(12 + g() % 4, 0);
            for (auto& b : f) b = rb();
            f[0] = 0x41; f[5] = (uint8_t)(0x80 | g() % 9); if (g() % 4) { f[10] = 1; f[11] = 0; }
            acks++;
        } else {
            f.resize(g() % 30);
            for (auto& b : f) b = rb();
            if (!f.empty() && g() % 2) f[0] = (uint8_t)(g() % 2 ? ':' : g() % 2 ? '!' : '@');
        }
        if (f.size() > 255) f.resize(255);
        const auto ra = A::meshDecodeMeshCom(f.empty() ? nullptr : f.data(), f.size());
        addRes(ra);
        parsed += ra.ok;
    }
    printf("  meshcom: %ld frames, %ld parsed (%ld ack candidates)\n", iters, parsed, acks);
}

int runMeshProto2() {
    const long iters = iterations();
    testMeshCore(iters);
    testLorawan(iters);
    testLoraAprs(iters);
    testMeshCom(iters);
    return 0;
}
