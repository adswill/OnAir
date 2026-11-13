// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Protocol lab tests: every stage alone, then the whole pipeline on generated signals of several known protocols.
#include "dect2/protolab/protolab.h"
#include "dect2/common/stable_random.h"
#include <chrono>
#include <cstring>
#include <cstdio>
#include <random>
#include <string>

using namespace dect2::protolab;

static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECKM(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d  %s  ", __FILE__, __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

static std::vector<uint8_t> S(const char* s) { return std::vector<uint8_t>(s, s + strlen(s)); }

static void testCrcCatalog() {
    struct K { const char* name; uint32_t check; };
    const K ks[] = {
        {"CRC-8", 0xF4}, {"CRC-8/CDMA2000", 0xDA}, {"CRC-8/DARC", 0x15}, {"CRC-8/DVB-S2", 0xBC}, {"CRC-8/EBU", 0x97}, {"CRC-8/I-CODE", 0x7E},
        {"CRC-8/ITU", 0xA1}, {"CRC-8/MAXIM (Dallas 1-Wire)", 0xA1}, {"CRC-8/ROHC", 0xD0}, {"CRC-8/WCDMA", 0x25}, {"CRC-8/SAE-J1850", 0x4B},
        {"CRC-8/AUTOSAR", 0xDF}, {"CRC-8/BLUETOOTH", 0x26}, {"CRC-16/ARC", 0xBB3D}, {"CRC-16/CCITT-FALSE", 0x29B1}, {"CRC-16/XMODEM", 0x31C3},
        {"CRC-16/KERMIT (CCITT-TRUE)", 0x2189}, {"CRC-16/X-25 (HDLC, PPP FCS)", 0x906E}, {"CRC-16/GENIBUS", 0xD64E}, {"CRC-16/AUG-CCITT", 0xE5CC},
        {"CRC-16/MODBUS", 0x4B37}, {"CRC-16/USB", 0xB4C8}, {"CRC-16/MAXIM", 0x44C2}, {"CRC-16/DNP", 0xEA82}, {"CRC-16/EN-13757 (wM-Bus)", 0xC2B7},
        {"CRC-16/T10-DIF", 0xD0DB}, {"CRC-16/TELEDISK", 0x0FB3}, {"CRC-16/CDMA2000", 0x4C06}, {"CRC-16/DECT-X", 0x007F}, {"CRC-16/DECT-R", 0x007E},
        {"CRC-16/MCRF4XX", 0x6F91}, {"CRC-16/RIELLO", 0x63D0}, {"CRC-16/TMS37157", 0x26B1}, {"CRC-16/ISO-IEC-14443-3-A", 0xBF05},
        {"CRC-16/DDS-110", 0x9ECF}, {"CRC-16/BUYPASS", 0xFEE8},
        {"CRC-32/ISO-HDLC (Ethernet, zip)", 0xCBF43926}, {"CRC-32/BZIP2", 0xFC891918}, {"CRC-32/MPEG-2", 0x0376E6E7}, {"CRC-32C (Castagnoli)", 0xE3069283},
        {"CRC-32/JAMCRC", 0x340BC6D9}, {"CRC-32/POSIX (cksum)", 0x765E7680}, {"CRC-32/XFER", 0xBD0BE338}, {"CRC-32D", 0x87315576}, {"CRC-32Q", 0x3010BF7F},
        {"CRC-8/NRSC-5 (poly 0x31, init 0xFF)", 0xF7},
    };
    const auto msg = S("123456789");
    for (const K& k : ks) {
        bool found = false;
        for (const CrcModel& m : crcCatalog()) {
            if (std::string(m.name) != k.name) continue;
            found = true;
            const uint32_t v = crcCompute(m, msg.data(), msg.size());
            CHECKM(v == k.check, "%s: got %X want %X", k.name, v, k.check);
        }
        CHECKM(found, "%s missing", k.name);
    }
}

static void testLineAndFraming() {
    auto w = whiteningCandidates()[0];
    std::vector<uint8_t> z(8, 0);
    whiten(z, w);
    CHECKM(z[0] == 0xFF && z[1] == 0xE1 && z[2] == 0x1D && z[3] == 0x9A, "PN9 %02X %02X %02X %02X", z[0], z[1], z[2], z[3]);
    whiten(z, w);
    CHECK(z == std::vector<uint8_t>(8, 0));

    const std::vector<uint8_t> bits = {1, 0, 1, 1, 0, 0, 1};
    // Manchester (IEEE: 1 -> 01)
    std::vector<uint8_t> chips;
    for (uint8_t b : bits) { chips.push_back(b ? 0 : 1); chips.push_back(b ? 1 : 0); }
    CHECK(decodeLine(chips, Coding::ManchesterIEEE, 0) == bits);
    std::vector<uint8_t> inv = bits;
    for (auto& b : inv) b ^= 1;
    CHECK(decodeLine(chips, Coding::ManchesterThomas, 0) == inv);
    CHECK(decodeLine({0, 0, 1, 1}, Coding::ManchesterIEEE, 0).empty());   // an invalid pair ends the packet
    // NRZI: 1 = no change
    CHECK((decodeLine({0, 0, 1, 1, 1, 0}, Coding::NRZI) == std::vector<uint8_t>{1, 0, 1, 1, 0}));
    // differential Manchester round trip
    {
        std::vector<uint8_t> c;
        uint8_t last = 0;
        for (uint8_t b : bits) { const uint8_t first = b ? last : (uint8_t)(last ^ 1); c.push_back(first); c.push_back(first ^ 1); last = first ^ 1; }
        auto dd = decodeLine(c, Coding::DiffManchester, 0);   // the first bit has no earlier level to compare with
        CHECK(dd.size() == bits.size() && std::equal(bits.begin() + 1, bits.end(), dd.begin() + 1));
    }
    // bytes
    CHECK((bitsToBytes({1, 0, 1, 0, 1, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 1}, 0, false) == std::vector<uint8_t>{0xAB, 0x01}));
    CHECK((bitsToBytes({1, 0, 1, 0, 1, 0, 1, 1}, 0, true) == std::vector<uint8_t>{0xD5}));
    CHECK(bytesToBits({0xA5}, false) == (std::vector<uint8_t>{1, 0, 1, 0, 0, 1, 0, 1}));
    CHECK(bitsToBytes(bytesToBits({1, 2, 3, 200}, true), 0, true) == (std::vector<uint8_t>{1, 2, 3, 200}));
    // HDLC: stuffing and flags
    {
        std::vector<uint8_t> frame = {0xFF, 0x7E, 0x1F, 0x80, 0x55, 0x01, 0x02, 0x03};
        std::vector<uint8_t> out, st;
        const uint8_t flag[8] = {0, 1, 1, 1, 1, 1, 1, 0};
        out.insert(out.end(), flag, flag + 8);
        int ones = 0;
        for (uint8_t v : frame) for (int k = 0; k < 8; k++) { const uint8_t b = (v >> k) & 1; out.push_back(b); ones = b ? ones + 1 : 0; if (ones == 5) { out.push_back(0); ones = 0; } }
        out.insert(out.end(), flag, flag + 8);
        auto fr = hdlcFrames(out);
        CHECK(fr.size() == 1);
        if (fr.size() == 1) CHECK(bitsToBytes(fr[0], 0, true) == frame);
    }
}

static std::vector<std::vector<uint8_t>> makePackets(int n, std::vector<uint8_t> pre, std::vector<uint8_t> sync, int bodyLen, uint32_t seed) {
    std::mt19937 rng(seed);
    std::vector<std::vector<uint8_t>> out;
    for (int i = 0; i < n; i++) {
        std::vector<uint8_t> bytes = pre;
        bytes.insert(bytes.end(), sync.begin(), sync.end());
        for (int k = 0; k < bodyLen; k++) bytes.push_back((uint8_t)rng());
        out.push_back(bytesToBits(bytes, false));
    }
    return out;
}

static void testSync() {
    auto pk = makePackets(8, {0xAA, 0xAA, 0xAA, 0xAA}, {0x2D, 0xD4}, 10, 3);
    // packets start with different amounts of preamble (the slicer joins late)
    pk[2].erase(pk[2].begin(), pk[2].begin() + 6);
    pk[5].erase(pk[5].begin(), pk[5].begin() + 2);
    SyncInfo si = findSync(pk);
    CHECK(si.found == 8 && si.total == 8);
    CHECK(si.preambleBits >= 20);
    CHECKM(si.sync.size() >= 8, "sync %zu bits", si.sync.size());
    // the sync starts where the repeating pattern ends; the 0x2D / 0xD4 bits follow it
    CHECK(si.start[0] >= 0 && si.start[0] <= 33);
    // no preamble: a 16-bit word shared by the packets
    auto pk2 = makePackets(8, {}, {0x93, 0x0B, 0x51}, 12, 5);
    for (size_t i = 0; i < pk2.size(); i++) pk2[i].insert(pk2[i].begin(), (size_t)(i % 5), 1);   // random lead-in
    SyncInfo s2 = findSync(pk2);
    CHECK(s2.found >= 7);
    CHECK(s2.sync.size() >= 16);
}

static CrcModel mk(int w, uint32_t poly, uint32_t init, bool ri, bool ro, uint32_t xo) { return CrcModel{"", w, poly, init, ri, ro, xo}; }

static std::vector<std::vector<uint8_t>> withCrc(int n, size_t lenBase, bool varLen, const CrcModel& m, bool be, size_t skipLead, uint32_t seed) {
    std::mt19937 rng(seed);
    std::vector<std::vector<uint8_t>> out;
    for (int i = 0; i < n; i++) {
        std::vector<uint8_t> p = {0x2D, 0xD4};
        const size_t len = lenBase + (varLen ? rng() % 5 : 0);
        for (size_t k = 0; k < len; k++) p.push_back((uint8_t)rng());
        const uint32_t c = crcCompute(m, p.data() + skipLead, p.size() - skipLead);
        const int nb = m.width / 8;
        for (int b = 0; b < nb; b++) p.push_back((uint8_t)(be ? c >> (8 * (nb - 1 - b)) : c >> (8 * b)));
        out.push_back(p);
    }
    return out;
}

static void testCrcSearch() {
    // catalog, equal and unequal lengths, both byte orders, CRC not covering the sync
    for (const CrcModel* base : {&crcCatalog()[19], &crcCatalog()[16], &crcCatalog()[7], &crcCatalog()[40], &crcCatalog()[22]}) {
        for (int var = 0; var < 2; var++) for (int be = 0; be < 2; be++) {
            auto pk = withCrc(8, 9, var == 1, *base, be == 1, 2, 11);
            CrcOptions o;
            o.generic = false;
            auto hits = searchCrc(pk, 2, o);
            bool ok = false;
            for (const CrcHit& h : hits) if (h.name == base->name && h.matches == 8 && h.dataStart == 2 && h.endSlack == 0 && (base->width == 8 || h.crcBigEndian == (be == 1))) ok = true;
            CHECKM(ok, "catalog %s var=%d be=%d (hits %zu)", base->name, var, be, hits.size());
        }
    }
    // a CRC with an unusual init and xorout: found from the packets, no catalog
    {
        auto m = mk(16, 0xB5C3, 0x1234, false, false, 0x00FF);
        auto pk = withCrc(6, 10, false, m, true, 2, 21);
        auto hits = searchCrc(pk, 2, {});
        bool ok = false;
        for (const CrcHit& h : hits) if (h.width == 16 && h.poly == 0xB5C3 && !h.refin && !h.refout && h.crcBigEndian && h.dataStart == 2 && h.matches == 6) ok = true;
        CHECKM(ok, "generic 16-bit (%zu hits)", hits.size());
    }
    {
        auto m = mk(16, 0x5E7D, 0x0000, true, true, 0x0000);
        auto pk = withCrc(5, 12, false, m, false, 2, 22);
        auto hits = searchCrc(pk, 2, {});
        bool ok = false;
        for (const CrcHit& h : hits) if (h.width == 16 && h.poly == 0x5E7D && h.refin && h.refout && !h.crcBigEndian && h.matches == 5) ok = true;
        CHECKM(ok, "generic 16-bit reflected (%zu hits)", hits.size());
    }
    {
        auto m = mk(8, 0x9D, 0x5A, false, false, 0x00);
        auto pk = withCrc(8, 8, false, m, true, 2, 23);
        auto hits = searchCrc(pk, 2, {});
        bool ok = false;
        for (const CrcHit& h : hits) if (h.width == 8 && h.poly == 0x9D && h.matches == 8) ok = true;
        CHECKM(ok, "generic 8-bit (%zu hits)", hits.size());
    }
    // packets of different lengths: every packet verifies (with one length only a part of them would, the init / xorout being fitted to it)
    {
        auto m = mk(16, 0xB5C3, 0x1234, false, false, 0x00FF);
        auto pk = withCrc(10, 9, true, m, true, 2, 24);
        auto hits = searchCrc(pk, 2, {});
        bool ok = false;
        for (const CrcHit& h : hits) if (h.poly == 0xB5C3 && h.matches == 10) ok = true;
        CHECKM(ok, "generic with lengths (%zu hits)", hits.size());
    }
    // random bytes: no confident CRC (false positives)
    {
        std::mt19937 rng(77);
        std::vector<std::vector<uint8_t>> pk;
        for (int i = 0; i < 8; i++) { std::vector<uint8_t> p; for (int k = 0; k < 14; k++) p.push_back((uint8_t)rng()); pk.push_back(p); }
        auto hits = searchCrc(pk, 2, {});
        int confident = 0;
        for (const CrcHit& h : hits) if (h.matches >= 5) confident++;
        CHECKM(confident == 0, "false CRC in random data: %d", confident);
    }
    // simple checksums
    {
        std::mt19937 rng(5);
        std::vector<std::vector<uint8_t>> pk;
        for (int i = 0; i < 6; i++) { std::vector<uint8_t> p = {0x55, 0x01}; for (int k = 0; k < 8; k++) p.push_back((uint8_t)rng()); uint8_t s = 0; for (size_t k = 1; k < p.size(); k++) s = (uint8_t)(s + p[k]); p.push_back(s); pk.push_back(p); }
        auto hits = searchCrc(pk, 1, {});
        bool ok = false;
        for (const CrcHit& h : hits) if (h.isChecksum && std::string(h.name).find("sum of bytes (8") == 0 && h.dataStart == 1) ok = true;
        CHECK(ok);
    }
}

static void testFields() {
    std::mt19937 rng(9);
    std::vector<std::vector<uint8_t>> pk;
    for (int i = 0; i < 12; i++) {
        const uint8_t t = (uint8_t)(100 + (rng() % 40));
        pk.push_back({0xA5, 0x3C, (uint8_t)(i * 3), t, (uint8_t)(t + 50), (uint8_t)(rng() % 3), (uint8_t)rng(), 0x11, 0x22});
    }
    FieldReport f = analyseFields(pk, 2);
    CHECKM(f.cols.size() == 9, "%zu cols", f.cols.size());
    if (f.cols.size() == 9) {
        CHECK(f.cols[0].kind == FieldKind::Constant && f.cols[1].kind == FieldKind::Constant);
        CHECK(f.cols[2].kind == FieldKind::Counter && f.cols[2].counterStep == 3);
        CHECK(f.cols[3].kind == FieldKind::Varying);
        CHECKM(f.cols[4].correlatedWith == 3 && f.cols[4].correlation > 0.99, "corr %d %f", f.cols[4].correlatedWith, f.cols[4].correlation);
        CHECK(f.cols[5].kind == FieldKind::Enum);
        CHECK(f.cols[6].entropy > 2.5);
        CHECK(f.cols[7].kind == FieldKind::Checksum && f.cols[8].kind == FieldKind::Checksum);
        CHECKM(f.layout == "C C N V V E V K K", "layout %s", f.layout.c_str());
    }
    auto in = interpret(S("xx\x01Hello 42 yz\x02\x12\x34\x56\x78"));
    bool ascii = false, bcd = false;
    for (auto& i : in) { if (i.kind == "ASCII" && i.text.find("Hello 42 yz") == 0) ascii = true; if (i.kind == "BCD" && i.text == "0212345678") bcd = true; }
    CHECK(ascii);
    CHECK(bcd);
    float fl = 21.5f;
    uint8_t fb[4];
    memcpy(fb, &fl, 4);
    std::vector<uint8_t> fv = {0x00, fb[0], fb[1], fb[2], fb[3]};
    bool flt = false;
    for (auto& i : interpret(fv)) if (i.kind == "float32 LE" && i.offset == 1 && i.text == "21.5") flt = true;
    CHECK(flt);
    const std::string d = bitDiff({0xF0, 0x0F}, {0xF1, 0x0F});
    CHECK(d.find("11110001") != std::string::npos);
    CHECK(d.find("       ^") != std::string::npos);
    CHECK(hexDump({0x01, 0xAB}) == "01 AB");
}

static std::vector<float> square(const std::vector<int>& bitsv, double spb, double jitter, std::mt19937& rng) {
    std::vector<float> s;
    dect2::uniform_real_distribution<double> u(-jitter, jitter);
    double acc = 0;
    for (int b : bitsv) { acc += spb + u(rng); while ((double)s.size() < acc) s.push_back(b ? 1.f : -1.f); }
    return s;
}

static void testBaud() {
    std::mt19937 rng(4);
    for (double spb : {8.0, 20.0, 52.083, 130.0}) {
        std::vector<int> b;
        for (int i = 0; i < 400; i++) b.push_back((int)(rng() & 1));
        Soft s;
        s.a = square(b, spb, 0.3, rng);
        BaudEstimate e = estimateBaud({&s}, 250000.0);
        const double want = 250000.0 / spb;
        CHECKM(std::abs(e.baud - want) < want * 0.03, "spb %.2f: baud %.1f want %.1f", spb, e.baud, want);
        CHECK(e.quality > 0.6);
        // slicing back
        auto bits = sliceBits(s, 250000.0, e.baud, 2, false);
        int err = 0;
        const size_t n = std::min(bits.size(), b.size());
        CHECKM(n >= 390, "bits %zu", bits.size());
        for (size_t i = 0; i < n; i++) err += bits[i] != b[i];
        CHECKM(err == 0, "slicer errors %d of %zu at spb %.1f", err, n, spb);
        auto ib = sliceBits(s, 250000.0, e.baud, 2, true);
        CHECK(!ib.empty() && ib[0] != bits[0]);
    }
    // only long runs of two lengths: Manchester-like, the unit is the short run
    std::vector<int> mb;
    for (int i = 0; i < 100; i++) { int b = (int)(rng() & 1); mb.push_back(b ? 0 : 1); mb.push_back(b ? 1 : 0); }
    Soft ms;
    ms.a = square(mb, 25.0, 0.2, rng);
    BaudEstimate me = estimateBaud({&ms}, 100000.0);
    CHECKM(std::abs(me.baud - 4000.0) < 120, "manchester chips: %.1f", me.baud);
    // too few edges: nothing claimed
    Soft flat;
    flat.a.assign(500, 1.f);
    CHECK(estimateBaud({&flat}, 1e5).baud == 0);
}

struct Expect { TestProto p; Demod d; Coding c; double baud; const char* crcName; bool hasWhitening; };

static bool gQuick = false;

static void testPipeline() {
    const Expect ex[] = {
        {TestProto::FskCrc16, Demod::FSK2, Coding::NRZ, 4800, "CRC-16/CCITT-FALSE", false},
        {TestProto::ManchesterOok, Demod::OOK, Coding::ManchesterIEEE, 2000, "CRC-8 (poly 0x31, init 0)", false},
        {TestProto::HdlcFsk, Demod::FSK2, Coding::Hdlc, 9600, "CRC-16/X-25", false},
        {TestProto::WhitenedFsk, Demod::FSK2, Coding::NRZ, 9600, "CRC-16", true},
        {TestProto::Fsk4Crc8, Demod::FSK4, Coding::NRZ, 4800, "CRC-8/NRSC-5", false},
    };
    for (const Expect& e : ex) {
        for (double cfo : {0.0, -9000.0}) {
            for (double snr : {25.0, 14.0}) {
                if (gQuick && (cfo != 0 || snr != 25.0)) continue;   // the quick run takes the clean case only
                GenOptions g;
                g.cfoHz = cfo;
                g.snrDb = snr;
                g.seed = 7 + (uint32_t)(cfo + 20000) % 13;
                std::vector<std::vector<uint8_t>> sent;
                auto iq = generateTestSignal(e.p, g, &sent);
                AnalyseOptions ao;
                auto t0 = std::chrono::steady_clock::now();
                Report r = analyse(iq, g.fs, ao);
                const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                const std::string tag = std::string(testProtoName(e.p)) + " cfo=" + std::to_string((int)cfo) + " snr=" + std::to_string((int)snr);
                CHECKM((int)r.bursts.size() == g.packets, "%s: %zu bursts", tag.c_str(), r.bursts.size());
                CHECKM(!r.candidates.empty(), "%s: no candidate (%s)", tag.c_str(), r.note.c_str());
                if (r.candidates.empty()) continue;
                const Candidate& c = r.candidates[0];
                CHECKM(c.demod == e.d, "%s: demod %s", tag.c_str(), demodName(c.demod));
                CHECKM(c.coding == e.c, "%s: coding %s", tag.c_str(), codingName(c.coding));
                CHECKM(std::abs(c.baud - e.baud) < e.baud * 0.03, "%s: baud %.1f", tag.c_str(), c.baud);
                CHECKM(!c.crc.empty() && c.crc[0].matches >= g.packets - 2 && c.crc[0].name.find(e.crcName) == 0, "%s: crc %s %d/%d", tag.c_str(), c.crc.empty() ? "none" : c.crc[0].name.c_str(), c.crc.empty() ? 0 : c.crc[0].matches, g.packets);
                CHECKM(c.whitening.empty() != e.hasWhitening, "%s: whitening '%s'", tag.c_str(), c.whitening.c_str());
                CHECKM(secs < 20.0, "%s: %.1f s", tag.c_str(), secs);
                if (snr > 20 && cfo == 0) {   // the clean case must recover the payload bytes
                    int exact = 0;
                    for (const auto& p : c.packets) for (const auto& s : sent) {
                        if (p.size() >= s.size() && std::equal(s.begin(), s.end(), p.end() - (long)s.size() - (c.crc.empty() ? 0 : 0))) { exact++; break; }
                    }
                    if (e.p != TestProto::ManchesterOok && e.p != TestProto::HdlcFsk) CHECKM(exact >= g.packets - 2, "%s: %d payloads recovered", tag.c_str(), exact);
                }
            }
        }
    }
}

// real-world impairments: transmitter clock error, other sample rates, an off-centre signal
static void testImpairments() {
    struct Case { TestProto p; double fs, scale, cfo, snr; };
    const Case cs[] = {
        {TestProto::FskCrc16, 250000, 1.03, 3000, 20}, {TestProto::FskCrc16, 250000, 0.97, -12000, 20}, {TestProto::FskCrc16, 96000, 1.0, 0, 25},
        {TestProto::FskCrc16, 1000000, 1.0, 40000, 20}, {TestProto::ManchesterOok, 250000, 1.04, 0, 20}, {TestProto::ManchesterOok, 48000, 1.0, 0, 30},
        {TestProto::HdlcFsk, 500000, 0.98, 20000, 20}, {TestProto::Fsk4Crc8, 250000, 1.02, 5000, 25}, {TestProto::WhitenedFsk, 250000, 0.98, -4000, 22},
    };
    for (const Case& c : cs) {
        GenOptions g;
        g.fs = c.fs; g.baudScale = c.scale; g.cfoHz = c.cfo; g.snrDb = c.snr; g.seed = 31;
        Report r = analyse(generateTestSignal(c.p, g), g.fs);
        const std::string tag = std::string(testProtoName(c.p)) + " fs=" + std::to_string((int)c.fs) + " scale=" + std::to_string(c.scale) + " cfo=" + std::to_string((int)c.cfo);
        bool ok = !r.candidates.empty() && !r.candidates[0].crc.empty() && r.candidates[0].crc[0].matches >= g.packets - 2;
        CHECKM(ok, "%s: %s", tag.c_str(), r.candidates.empty() ? r.note.c_str() : (r.candidates[0].crc.empty() ? "no crc" : r.candidates[0].crc[0].name.c_str()));
    }
}

// one signal inside a wide capture: 2 Msps with the FSK burst train 300 kHz off-centre, plus a strong carrier elsewhere
static void testChannelise() {
    GenOptions g;
    g.fs = 250000;
    auto narrow = generateTestSignal(TestProto::FskCrc16, g);
    std::vector<cf> wide(narrow.size() * 8);   // zero-order-hold upsampling by 8 is enough for a test: images sit far away
    const double fsW = 2000000, pi = 3.14159265358979;
    for (size_t i = 0; i < wide.size(); i++) {
        const double ph = 2 * pi * 300000.0 * (double)i / fsW, ph2 = 2 * pi * -650000.0 * (double)i / fsW;
        wide[i] = narrow[i / 8] * cf((float)std::cos(ph), (float)std::sin(ph)) + 3.f * cf((float)std::cos(ph2), (float)std::sin(ph2));
    }
    double rate = 0;
    auto sig = channelise(wide, fsW, 300000.0, 20000.0, rate);
    CHECKM(rate >= 40000 && rate <= 100000, "rate %.0f", rate);
    Report r = analyse(sig, rate);
    CHECK(!r.candidates.empty());
    if (!r.candidates.empty()) CHECKM(!r.candidates[0].crc.empty() && r.candidates[0].crc[0].name == "CRC-16/CCITT-FALSE", "wide capture: %s", r.candidates[0].crc.empty() ? "no crc" : r.candidates[0].crc[0].name.c_str());
    double r2 = 0;
    CHECK(channelise({}, 1e6, 0, 1e4, r2).empty());
}

static void testPipelineEdgeCases() {
    // noise only
    std::mt19937 rng(3);
    dect2::normal_distribution<float> g(0.f, 0.1f);
    std::vector<cf> n(300000);
    for (auto& v : n) v = cf{g(rng), g(rng)};
    Report r = analyse(n, 250000);
    CHECK(r.candidates.empty());
    // one burst only
    GenOptions go;
    go.packets = 1;
    Report r1 = analyse(generateTestSignal(TestProto::FskCrc16, go), go.fs);
    CHECK(r1.candidates.empty() && !r1.note.empty());
    // structured but meaningless packets (preamble, sync word, random bytes): no CRC may be claimed
    {
        std::mt19937 rr(99);
        std::vector<std::vector<uint8_t>> pk;
        for (int i = 0; i < 12; i++) {
            std::vector<uint8_t> b = {0xAA, 0xAA, 0xAA, 0x2D, 0xD4};
            for (int k = 0; k < 14; k++) b.push_back((uint8_t)rr());
            pk.push_back(bytesToBits(b, false));
        }
        Report rn = analysePackets(pk, 4800);
        bool claim = false;
        for (auto& c : rn.candidates) for (auto& h : c.crc) claim |= h.matches >= 3 && h.matches * 10 >= h.total * 6 && !h.isChecksum;
        CHECK(!claim);
    }
    // a demodulator chosen by hand
    GenOptions g2;
    auto iq = generateTestSignal(TestProto::FskCrc16, g2);
    AnalyseOptions ao;
    ao.demod = Demod::FSK2;
    ao.baud = 4800;
    Report r2 = analyse(iq, g2.fs, ao);
    CHECK(!r2.candidates.empty() && r2.candidates[0].demod == Demod::FSK2 && !r2.candidates[0].crc.empty());
    // the wrong demodulator does not find a CRC
    ao.demod = Demod::OOK;
    ao.baud = 0;
    Report r3 = analyse(iq, g2.fs, ao);
    bool crc = false;
    for (auto& c : r3.candidates) crc |= !c.crc.empty() && c.crc[0].matches >= 6;
    CHECK(!crc);
    // packets typed in as bits
    GenOptions g3;
    std::vector<std::vector<uint8_t>> sent;
    generateTestSignal(TestProto::FskCrc16, g3, &sent);
    std::vector<std::vector<uint8_t>> bits;
    for (auto& s : sent) { std::vector<uint8_t> all = {0xAA, 0xAA, 0xAA, 0x2D, 0xD4}; all.insert(all.end(), s.begin(), s.end()); bits.push_back(bytesToBits(all, false)); }
    Report r4 = analysePackets(bits, 4800);
    CHECK(!r4.candidates.empty() && !r4.candidates[0].crc.empty() && r4.candidates[0].crc[0].name == "CRC-16/CCITT-FALSE");
    // QPSK / BPSK burst baud check through the demodulator: a carrier-offset BPSK burst is demodulated to two levels
    {
        const double fs = 100000, baud = 5000, spb = fs / baud, pi = 3.14159265358979;
        std::mt19937 rr(1);
        std::vector<cf> x;
        std::vector<int> b;
        double ph = 0.7;
        for (int i = 0; i < 300; i++) b.push_back((int)(rr() & 1));
        for (int s : b) for (int k = 0; k < (int)spb; k++) { ph += 2 * pi * 700.0 / fs; x.emplace_back((s ? 1.f : -1.f) * (float)std::cos(ph), (s ? 1.f : -1.f) * (float)std::sin(ph)); }
        Soft so = demodulate(x.data(), x.size(), Demod::PSK2, fs);
        auto bb = sliceBits(so, fs, baud, 2, false);
        // BPSK has a 180 degree ambiguity: bits or their inverse
        int same = 0;
        const size_t nn = std::min(bb.size(), b.size());
        for (size_t i = 0; i < nn; i++) same += bb[i] == b[i];
        CHECKM(nn >= 290 && (same >= (int)nn - 3 || same <= 3), "bpsk: %d of %zu", same, nn);
    }
    {
        const double fs = 100000, baud = 5000, spb = fs / baud, pi = 3.14159265358979;
        std::mt19937 rr(2);
        std::vector<cf> x;
        std::vector<int> b;
        double ph = 0.3;
        for (int i = 0; i < 300; i++) b.push_back((int)(rr() & 3));
        for (int s : b) for (int k = 0; k < (int)spb; k++) {
            ph += 2 * pi * 400.0 / fs;
            const cf sym(((s >> 1) ? 1.f : -1.f), ((s & 1) ? 1.f : -1.f));
            x.push_back(sym * cf((float)std::cos(ph), (float)std::sin(ph)));
        }
        Soft so = demodulate(x.data(), x.size(), Demod::PSK4, fs);
        auto q = sliceBits(so, fs, baud, 4, false);
        CHECKM(q.size() >= 560, "qpsk bits %zu", q.size());
        // four-fold phase ambiguity: the bit pairs agree up to a fixed rotation of the constellation; check that they are consistent
        std::vector<int> map(4, -1);
        int bad = 0;
        for (size_t i = 0; i < b.size() && 2 * i + 1 < q.size(); i++) {
            const int got = q[2 * i] * 2 + q[2 * i + 1];
            if (map[(size_t)b[i]] < 0) map[(size_t)b[i]] = got;
            else if (map[(size_t)b[i]] != got) bad++;
        }
        CHECKM(bad <= 6, "qpsk inconsistent symbols %d", bad);
    }
}

static void testExport() {
    GenOptions go;
    Report r = analyse(generateTestSignal(TestProto::FskCrc16, go), go.fs);
    CHECK(!r.candidates.empty());
    if (r.candidates.empty()) return;
    const std::string j = protocolJson(r.candidates[0], r.fields, "test \"proto\"", go.fs);
    CHECK(j.find("\"symbol_rate_baud\": 4") != std::string::npos);
    CHECK(j.find("CRC-16/CCITT-FALSE") != std::string::npos);
    CHECK(j.find("test \\\"proto\\\"") != std::string::npos);
    CHECK(j.find("\"kind\": \"counter\"") != std::string::npos);
    CHECK(j.find("\"0x1021\"") != std::string::npos);
    const std::string t = packetText(r.candidates[0]);
    CHECK(!t.empty() && t.find('\n') != std::string::npos && t.find_first_not_of("01\n") == std::string::npos);
    const std::string x = rtl433Spec(r.candidates[0], "my sensor");
    CHECK(x.find("m=FSK_PCM") != std::string::npos && x.find("n=my_sensor") != std::string::npos && x.find("s=208") != std::string::npos);
}

int main(int argc, char** argv) {
    gQuick = argc >= 2 && std::string(argv[1]) == "quick";
    auto want = [&](const char* n) { return argc < 2 || std::string(argv[1]) == n || std::string(argv[1]) == "quick" || (std::string(argv[1]) == "sweep" && (std::string(n) == "pipeline" || std::string(n) == "impair")); };
    if (argc >= 2 && std::string(argv[1]) == "quick") { /* all stages, the clean pipeline cases, no impairment sweep */ }
    auto timed = [&](const char* n, void (*f)()) {
        if (!want(n)) return;
        auto t0 = std::chrono::steady_clock::now();
        f();
        if (argc >= 2) printf("%s: %.2f s\n", n, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    };
    timed("catalog", testCrcCatalog);
    timed("line", testLineAndFraming);
    timed("sync", testSync);
    timed("crc", testCrcSearch);
    timed("fields", testFields);
    timed("baud", testBaud);
    timed("edge", testPipelineEdgeCases);
    timed("export", testExport);
    timed("channel", testChannelise);
    timed("pipeline", testPipeline);
    if (!gQuick) timed("impair", testImpairments);
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
