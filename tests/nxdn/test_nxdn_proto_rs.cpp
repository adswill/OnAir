// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Golden-vector test of the Rust NXDN frame layer (rust/onair_nxdn, dect2::nxdn): scrambler, LICH (all 256 bytes), CRCs, the three coded channels (clean,
// with bit errors and random soft values), layer 3 build / parse and the names, over ONAIR_DIFF_ITERS (default 200000) seeded inputs per family. Every result
// goes into a digest (tests/common/golden.h) compared with the one recorded from the C++ layer the Rust code replaced.
#include "dect2/nxdn/nxdn_proto.h"
#include "../common/golden.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>
namespace A = dect2::nxdn;

static golden::Digest dig;

static std::mt19937 g(4242);
static float rndSoft() {
    switch (g() % 12) {
    case 0: return 0.f;
    case 1: return (float)(g() % 7) - 3.f;
    case 2: return ((float)g() / 4294967296.f - 0.5f) * 1e9f;
    case 3: return g() % 50 == 0 ? std::nanf("") : 1.f;
    case 4: return g() % 50 == 0 ? INFINITY : -1.f;
    default: return ((float)g() / 4294967296.f - 0.5f) * 8.f;
    }
}
static void addMsg(const A::Msg& a) {
    dig.add(a.type, a.callType, a.option, a.src, a.dst, a.cipher, a.keyId);
    dig.add(a.status, a.block, a.blocks, a.text, a.location, a.service, a.channel);
}

int main() {
    const char* it = getenv("ONAIR_DIFF_ITERS");
    const long iters = it ? atol(it) : 200000;

    // the table and the 8-bit LICH space
    for (size_t k = 0; k < sizeof A::kFsw; k++) dig.add(((const uint8_t*)A::kFsw)[k]);
    for (int v = 0; v < 256; v++) {
        A::Lich a;
        const bool ra = A::lichDecode((uint8_t)v, a);
        dig.add(ra); if (ra) dig.add(a.rfct, a.fct, a.opt, a.dir);
    }
    for (long i = 0; i < iters; i++) {
        const int f[4] = {(int)g() - (1 << 30), (int)g() - (1 << 30), (int)g() - (1 << 30), (int)g() - (1 << 30)};
        A::Lich a{f[0], f[1], f[2], f[3]};
        dig.add(A::lichEncode(a));
    }
    for (int t = -5; t < 300; t++) {
        dig.add(A::msgName(t), A::callTypeName(t));
    }

    // scrambler and CRCs
    for (long i = 0; i < iters; i++) {
        const int n = (int)(g() % 420);
        std::vector<uint8_t> a((size_t)n + 1, 0xAA);
        A::pn9(a.data(), n);
        dig.add(a);
        std::vector<uint8_t> bits((size_t)n + 1);
        for (auto& x : bits) x = (uint8_t)(g() % 5 == 0 ? g() : (g() & 1));
        dig.add(A::crc6(bits.data(), n), A::crc12(bits.data(), n), A::crc16(bits.data(), n));
    }
    dig.add(A::crc6(nullptr, -3), A::crc16(nullptr, 0));

    // the channels
    const A::Ch ach[3] = {A::Ch::Sacch, A::Ch::Facch1, A::Ch::Cac};
    for (int ci = 0; ci < 3; ci++) {
        dig.add(A::infoBits(ach[ci]), A::airBits(ach[ci]));
        const int ni = A::infoBits(ach[ci]), na = A::airBits(ach[ci]);
        for (long i = 0; i < iters; i++) {
            std::vector<uint8_t> info((size_t)ni), ai((size_t)na, 7);
            for (auto& x : info) x = (uint8_t)(g() % 9 == 0 ? g() : (g() & 1));
            A::encode(ach[ci], info.data(), ai.data());
            dig.add(ai);
            // soft values: the clean codeword with 0..14 errors, scaled and noisy, or pure random
            std::vector<float> soft((size_t)na);
            const int mode = (int)(g() % 4);
            if (mode == 3) { for (auto& s : soft) s = rndSoft(); }
            else {
                for (int k = 0; k < na; k++) soft[(size_t)k] = (ai[(size_t)k] ? 1.f : -1.f) * (0.2f + (float)(g() % 100) / 50.f);
                const int errs = mode == 0 ? 0 : (int)(g() % 15);
                for (int e = 0; e < errs; e++) { float& s = soft[g() % (unsigned)na]; s = -s; }
            }
            std::vector<uint8_t> oa((size_t)ni, 3);
            int ea = -1;
            const bool da = A::decode(ach[ci], soft.data(), oa.data(), &ea);
            dig.add(da, oa); if (da) dig.add(ea);
            if (i % 8 == 0) dig.add(A::decode(ach[ci], soft.data(), oa.data()), oa);
        }
    }

    // layer 3: built from random fields, parsed from random bits (known types over-represented), every length
    static const int types[] = {A::kVcall, A::kTxRel, A::kIdle, A::kSiteInfo, A::kSrvInfo, A::kStatReq, A::kSdHeader, A::kSdData};
    for (long i = 0; i < iters; i++) {
        A::Msg a;
        a.type = g() % 4 ? types[g() % 8] : (int)(g() % 80) - 5;
        a.callType = (int)(g() % 9) - 1; a.option = (int)g(); a.src = (int)g(); a.dst = (int)g();
        a.cipher = (int)g(); a.keyId = (int)g(); a.status = (int)g(); a.block = (int)g();
        a.blocks = (int)g(); a.location = g(); a.service = (int)g(); a.channel = (int)g();
        std::string t; for (size_t k = g() % 12; k > 0; k--) t += (char)(g() % 4 ? 32 + g() % 95 : g() & 0xFF);
        a.text = t;
        const int maxBits = (int)(g() % 130) - 3;
        std::vector<uint8_t> ba((size_t)(maxBits > 0 ? maxBits : 0) + 1, 0x55);
        const int na = A::buildMsg(a, ba.data(), maxBits);
        dig.add(na, ba);
        // parse the result and random bits
        std::vector<uint8_t> bits(g() % 200);
        for (auto& x : bits) x = (uint8_t)(g() % 7 == 0 ? g() : (g() & 1));
        if (!bits.empty() && g() % 4) {   // a known type in the first byte
            const int ty = types[g() % 8];
            for (int k = 0; k < 8 && k < (int)bits.size(); k++) bits[(size_t)k] = (uint8_t)((ty >> (7 - k)) & 1);
        }
        const int nb2 = (int)bits.size() + (int)(g() % 5) - 2;
        A::Msg pa;
        pa.src = 77;   // must be reset
        const bool qa = A::parseMsg(bits.data(), nb2 > (int)bits.size() ? (int)bits.size() : nb2, pa);
        dig.add(qa); addMsg(pa);
        const int nn = na < maxBits ? na : maxBits;
        if (nn > 0) {
            A::Msg ra;
            const bool xa = A::parseMsg(ba.data(), nn, ra);
            dig.add(xa); addMsg(ra);
        }
    }
    for (long i = 0; i < iters / 4; i++) {
        uint8_t bits[72]; for (auto& x : bits) x = (uint8_t)(g() & 0xFF);
        char fa[4][24];
        A::ambeFrame(bits, fa);
        for (auto& row : fa) for (char c : row) dig.add(c);
    }
    return golden::check("nxdn_proto_rs", dig, 0x461c5b4739445ed7ull, !it);
}
