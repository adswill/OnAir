// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Golden-vector test of the AIS frame layer (dect2::ais, the Rust crate onair_proto): the known gpsd / AIS-catcher sentences, frames built here, and ONAIR_DIFF_ITERS
// (default 200000) seeded random and mutated frames, bit streams with values other than 0 and 1, and damaged sentences. Every result goes into a digest
// (tests/common/golden.h; doubles rounded to 1e-6) compared with the one recorded from the C++ AIS frame layer the Rust code replaced.
#include "dect2/ais/ais_proto.h"
#include "../common/golden.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
namespace A = dect2::ais;

static golden::Digest dig;
static long abiFails = 0;
#define CHECK(c, ...) do { if (!(c)) { abiFails++; printf("FAILED: " __VA_ARGS__); printf("\n"); } } while (0)

static void addMsg(const A::AisMsg& a) {
    dig.add(a.valid, a.type, a.repeat, a.mmsi, a.cls, a.hasPos, a.lat, a.lon, a.sog, a.cog, a.heading, a.hasRot, a.rotDegMin, a.navStatus, a.second, a.name, a.callsign);
    dig.add(a.destination, a.imo, a.shipType, a.dimA, a.dimB, a.dimC, a.dimD, a.etaMonth, a.etaDay, a.etaHour, a.etaMin, a.draughtM, a.epfd, a.partNo, a.vendor, a.aidType);
    dig.add(a.offPosition, a.virtualAid, a.altitudeM, a.year, a.month, a.day, a.hour, a.minute, a.sec, a.text, a.dac, a.fi, a.destMmsi, a.bits);
}

static void compareBits(const A::Bits& d, std::mt19937& g) {
    dig.add(A::fcsRegister(d.data(), d.size()));
    dig.add(A::fcsOk(d));
    dig.add(A::stuff(d));
    { A::Bits o1; const bool r1 = A::destuff(d, o1); dig.add(r1); if (r1) dig.add(o1); }
    const int lv = (int)(g() % 4) - 1;
    dig.add(A::nrziEncode(d, lv));
    dig.add(A::nrziDecode(d, lv));
    { A::Bits x = d; A::reverseOctets(x); dig.add(x); }
    { const A::HdlcResult h1 = A::hdlcFrames(d); dig.add((uint64_t)h1.good.size()); for (const auto& f : h1.good) dig.add(f); dig.add(h1.bad); }
    dig.add(A::burstLineBits(d));
    const size_t pos = g() % (d.size() + 20), len = g() % 70;
    dig.add(A::getU(d, pos, len));
    dig.add(A::getS(d, pos, len));
    dig.add(A::getText(d, pos, len));
}

static void compareMessage(const A::Bits& p) {
    A::AisMsg a;
    const bool ra = A::decodeMessage(p, a);
    dig.add(ra); addMsg(a);
    for (int dec = 0; dec < 2; dec++) dig.add(A::acceptableAis(a, dec != 0));
    int fa = 0;
    const std::string sa = A::armour(p, fa);
    dig.add(sa, fa);
    for (int fill = -2; fill < 9; fill++) dig.add(A::unarmour(sa, fill));
    dig.add(A::toNmea(p, 'B', 3), A::toNmea(p, 'A', -13));
}

static void compareSentence(const std::string& s) {
    std::string c1 = "x"; int f1 = 77, n1 = 77, k1 = 77; char h1 = 'q';
    const bool r1 = A::parseNmea(s, c1, f1, n1, k1, &h1);
    dig.add(r1, c1, f1, n1, k1, h1);
    std::string c3 = "y"; int a1 = 5, a2 = 5, a3 = 5;
    const bool r3 = A::parseNmea(s, c3, a1, a2, a3);
    dig.add(r3, c3, a1, a2, a3);
    dig.add(A::nmeaChecksum(s));
}

static const char* kSentences[] = {
    "!AIVDM,1,1,,B,177KQJ5000G?tO`K>RA1wUbN0TKH,0*5C",
    "!AIVDM,1,1,,A,15RTgt0PAso;90TKcjM8h6g208CQ,0*4A", "!AIVDM,1,1,,A,16SteH0P00Jt63hHaa6SagvJ087r,0*42",
    "!AIVDM,1,1,,B,25Cjtd0Oj;Jp7ilG7=UkKBoB0<06,0*60", "!AIVDM,1,1,,A,38Id705000rRVJhE7cl9n;160000,0*40",
    "!AIVDM,1,1,,A,403OviQuMGCqWrRO9>E6fE700@GO,0*4D", "!AIVDM,1,1,,A,91b77=h3h00nHt0Q3r@@07000<0b,0*69",
    "!AIVDM,1,1,,A,B52K>;h00Fc>jpUlNV@ikwpUoP06,0*4C", "!AIVDM,1,1,,B,C5N3SRgPEnJGEBT>NhWAwwo862PaLELTBJ:V00000000S0D:R220,0*0B",
    "!AIVDM,1,1,,A,H42O55i18tMET00000000000000,2*6D", "!AIVDM,1,1,,A,H42O55lti4hhhilD3nink000?050,0*40",
    "!AIVDM,1,1,,A,>5?Per18=HB1U:1@E=B0m<L,2*51", "!AIVDM,1,1,,A,>3R1p10E3;;R0USCR0HO>0@gN10kGJp,2*7F", "!AIVDM,1,1,,A,>4aDT81@E=@,2*2E",
    "!AIVDM,1,1,,B,6B?n;be:cbapalgc;i6?Ow4,2*4A", "!AIVDM,1,1,,A,85Mwp`1Kf3aCnsNvBWLi=wQuNhA5t43N`5nCuI=p<IBfVqnMgPGs,0*47",
    "!AIVDM,2,1,1,A,55?MbV02;H;s<HtKR20EHE:0@T4@Dn2222222216L961O5Gf0NSQEp6ClRp8,0*1C", "!AIVDM,2,2,1,A,88888888880,2*25",
    "!AIVDM,2,1,2,A,542M92h00001@<7;?G0PD4i@R0<tqA8tj37>220o0h:2240Ht500000000000000,0*3C", "!AIVDM,2,2,2,A,0000002,2*24",
    "!AIVDM,1,1,,A,15RTgt0PAso;90TKcjM8h6g208CQ,0*4B", "$AIVDM,1,1,,A,,0*26", "!AIVDM,1,1,,,x,0*00", "!AIVDM,0,1,,A,xx,0*00", "!AIVDM,1,1,,A,xx,9*00",
    "!AIVDM,1,1,,A,15RTgt0PAso;90TKcjM8h6g208CQ,0*4a", "!AIVDM,1,1,,A,15RTgt0PAso;90TKcjM8h6g208CQ,0*4", "!AIVDM,1,1,,A,15RTgt0PAso;90TKcjM8h6g208CQ,0*"};

static std::mt19937 gen(20261016);

static A::Bits randomMessage(int type) {
    A::Bits b;
    A::putU(b, 0, 6, (uint32_t)type);
    A::putU(b, 6, 2, gen() & 3);
    A::putU(b, 8, 30, (gen() & 1) ? 200000000u + gen() % 600000000u : gen() % 1000000000u);
    static const size_t lens[] = {168, 168, 168, 168, 424, 420, 72, 88, 56, 40, 168, 312, 272, 360, 160, 168, 96};
    size_t n = lens[gen() % (sizeof lens / sizeof *lens)];
    if (gen() % 5 == 0) n = 30 + gen() % 400;
    for (size_t i = 38; i < n; i++) b.push_back(0), A::putU(b, i, 1, gen() & 1);
    if (b.size() < n) b.resize(n, 0);
    // sometimes the "not available" sentinels
    if ((gen() & 7) == 0 && n >= 120) { A::putS(b, 61, 28, 0x6791AC0); A::putS(b, 89, 27, 0x3412140); }
    return b;
}

static void mutate(A::Bits& v) {
    const int k = 1 + (int)(gen() % 5);
    for (int j = 0; j < k; j++) {
        switch (gen() % 7) {
        case 0: if (!v.empty()) v[gen() % v.size()] ^= 1; break;
        case 1: if (!v.empty()) v[gen() % v.size()] = (uint8_t)gen(); break;           // values other than 0 and 1
        case 2: if (!v.empty()) v.erase(v.begin() + (long)(gen() % v.size())); break;
        case 3: { const size_t at = gen() % (v.size() + 1); const uint8_t bit = (uint8_t)(gen() & 1); v.insert(v.begin() + (long)at, bit); } break;   // draws in a fixed order: call arguments are evaluated in an unspecified one
        case 4: if (v.size() > 2) v.resize(gen() % v.size()); break;
        case 5: { const size_t a = gen() % (v.size() + 1); for (int q = 0; q < 6; q++) v.insert(v.begin() + (long)a, 1); break; }   // a run of ones: flags, aborts
        default: if (!v.empty()) { const size_t a = gen() % v.size(); const size_t l = gen() % 20; for (size_t q = a; q < v.size() && q < a + l; q++) v[q] = gen() & 1; } break;
        }
    }
}

static void mutateText(std::string& s) {
    const int k = 1 + (int)(gen() % 3);
    for (int j = 0; j < k && !s.empty(); j++) {
        switch (gen() % 5) {
        case 0: s[gen() % s.size()] = (char)gen(); break;
        case 1: s.erase(gen() % s.size(), 1); break;
        case 2: { const size_t at = gen() % (s.size() + 1); const char ch = ",*!$ 0123456789ABCDEFabcdef-+\t\n"[gen() % 31]; s.insert(at, 1, ch); } break;
        case 3: s.resize(gen() % s.size()); break;
        default: s[gen() % s.size()] = ",*"[gen() & 1]; break;
        }
    }
}

int main() {
    const char* e = getenv("ONAIR_DIFF_ITERS");
    const long iters = e ? atol(e) : 200000;

    // known sentences: decode, re-frame, and the damaged versions
    for (const char* s : kSentences) {
        compareSentence(s);
        std::string chars; int fill = 0, cnt = 0, no = 0;
        if (!A::parseNmea(s, chars, fill, cnt, no)) continue;
        const A::Bits p = A::unarmour(chars, fill);
        compareMessage(p);
        compareBits(p, gen);
        A::Bits line = A::burstLineBits(p);
        compareBits(line, gen);
        compareBits(A::nrziDecode(line), gen);
    }
    // frames built here from scratch (bit by bit, not with the library): flags, stuffing, X.25 FCS, NRZI, with the octet order of the air
    for (int t : {1, 2, 3, 4, 5, 9, 18, 19, 21, 24, 27}) for (int rep = 0; rep < 20; rep++) {
        A::Bits p = randomMessage(t), fr = p;
        for (size_t i = 0; i < fr.size(); i += 8) { size_t e8 = std::min(i + 8, fr.size()); std::reverse(fr.begin() + (long)i, fr.begin() + (long)e8); }
        uint16_t r = 0xFFFF;
        for (uint8_t b : fr) { const bool fb = ((b ^ r) & 1) != 0; r >>= 1; if (fb) r ^= 0x8408; }
        r = (uint16_t)~r;
        for (int i = 0; i < 16; i++) fr.push_back((r >> i) & 1);
        A::Bits st, d = {0, 1, 1, 1, 1, 1, 1, 0};
        for (int ones = 0; uint8_t b : fr) { st.push_back(b); ones = b ? ones + 1 : 0; if (ones == 5) { st.push_back(0); ones = 0; } }
        d.insert(d.end(), st.begin(), st.end());
        for (uint8_t b : {0, 1, 1, 1, 1, 1, 1, 0}) d.push_back(b);
        A::Bits line;
        int lv = 1;
        for (uint8_t b : d) { if (!b) lv ^= 1; line.push_back((uint8_t)lv); }
        const A::Bits dec = A::nrziDecode(line);
        if (p.size() >= 48 && p.size() <= 1000) CHECK(A::hdlcFrames(dec).good.size() == 1 && A::hdlcFrames(dec).good[0] == p, "built frame type %d decodes to the message (%zu bits)", t, p.size());
        compareBits(dec, gen);
        compareMessage(p);
    }

    for (long it = 0; it < iters; it++) {
        const int kind = (int)(gen() % 6);
        if (kind == 0) {            // a message of a random type and length
            compareMessage(randomMessage((int)(gen() % 64)));
        } else if (kind == 1) {     // a valid burst, damaged
            A::Bits p = randomMessage((int)(gen() % 28));
            A::Bits line = A::burstLineBits(p);
            if (gen() & 1) line = A::nrziDecode(line);
            mutate(line);
            compareBits(line, gen);
        } else if (kind == 2) {     // two bursts in one stream, one damaged
            A::Bits l1 = A::nrziDecode(A::burstLineBits(randomMessage(1 + (int)(gen() % 27)))), l2 = A::nrziDecode(A::burstLineBits(randomMessage(18)));
            if (gen() & 1) mutate(l2);
            l1.insert(l1.end(), l2.begin(), l2.end());
            compareBits(l1, gen);
            compareMessage(A::hdlcFrames(l1).good.empty() ? l1 : A::hdlcFrames(l1).good[0]);
        } else if (kind == 3) {     // pure noise, sometimes with values above 1
            A::Bits v(gen() % 1300);
            const bool wide = gen() & 1;
            for (auto& b : v) b = wide ? (uint8_t)gen() : (uint8_t)(gen() & 1);
            compareBits(v, gen);
        } else if (kind == 4) {     // sentences
            std::string s = kSentences[gen() % (sizeof kSentences / sizeof *kSentences)];
            mutateText(s);
            compareSentence(s);
        } else {                    // payload mutations, then message level
            A::Bits p = randomMessage(1 + (int)(gen() % 27));
            mutate(p);
            compareMessage(p);
        }
    }
    if (abiFails) return 1;
    return golden::check("ais_proto_rs", dig, 0x83ee07b4ec42ad84ull, !e);
}
