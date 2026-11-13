// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Meteosat HRDCP: the format (the TD 16 CRC check value, header fields, the coding round trip with byte errors) and the receiver on generated signals:
// short and multi-frame messages, frequency errors to the 125 Hz limit, 1 Hz/s drift, clock errors, phase noise, Eb/N0, two platforms on neighbouring
// channels, a standard-rate platform beside them, noise only. Simulated signals only: no HRDCP recording was found.
#include "dect2/goesdcs/hrdcp_gen.h"
#include "dect2/goesdcs/hrdcp_live.h"
#include "dect2/goesdcs/hrdcp_proto.h"
#include "dect2/goesdcs/goesdcs_gen.h"
#include "dect2/goesdcs/goesdcs_proto.h"
#include "dect2/goesdcs/goesdcs_tel.h"
#include "dect2/meteor/meteor_proto.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
using namespace dect2;
#include "../common/test_util.h"

static std::vector<uint8_t> bytes(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }

static DcsTelemetry run(const HrdcpGenConfig& c, double seconds, std::vector<std::string>* logs = nullptr, const GoesDcsGenConfig* srdcp = nullptr) {
    HrdcpReceiver r;
    r.configure(c.rate);
    r.setCenterHz(c.centreHz);
    r.setStartUtc(1790000000);
    if (logs) r.setLogCallback([logs](const std::string& s) { logs->push_back(s); });
    HrdcpGenerator g(c);
    std::unique_ptr<GoesDcsGenerator> sr;
    if (srdcp) sr = std::make_unique<GoesDcsGenerator>(*srdcp);
    std::vector<cf32> x(32768), y(32768);
    for (int64_t n = 0; n < (int64_t)(seconds * c.rate); n += (int64_t)x.size()) {
        g.generate(x.data(), x.size());
        if (sr) { sr->generate(y.data(), y.size()); for (size_t i = 0; i < x.size(); i++) x[i] += y[i]; }
        r.feed(x.data(), x.size());
    }
    DcsTelemetry t;
    r.telemetry(t, 0);
    if (logs) for (const auto& l : *logs) printf("    log: %s\n", l.c_str());
    return t;
}

static const DcsMessage* find(const DcsTelemetry& t, uint32_t addr21, int seq = -1) {
    for (const auto& m : t.messages) if (m.address == dcs::addressWord(addr21) && (seq < 0 || m.sequence == seq)) return &m;
    return nullptr;
}

static HrdcpGenTx tx(double hz, uint32_t addr, const std::vector<uint8_t>& data, double start = 0.5) {
    HrdcpGenTx t;
    t.freqHz = hz; t.address21 = addr; t.data = data; t.startSec = start;
    return t;
}

int main() {
    const bool longRun = std::getenv("ONAIR_TEST_LONG") != nullptr;
    // ---- format
    {
        const char* cm = "CatMouse987654321";
        CHECK(hrdcp::crc32(reinterpret_cast<const uint8_t*>(cm), strlen(cm)) == 0x1FC0DFECu, "TD 16 CRC check value: %08X", hrdcp::crc32(reinterpret_cast<const uint8_t*>(cm), strlen(cm)));
        hrdcp::Header h;
        h.address = dcs::addressWord(0x2C412); h.length = 1234; h.sequence = 65535; h.type = 1; h.compression = 1; h.health = 0x2A5; h.spare = 0;
        uint8_t b[12];
        hrdcp::putHeader(h, b);
        CHECK(b[0] == 0x16 && b[1] == 0x20 && b[2] == 0x96 && b[3] == 0xC5, "address bytes %02X %02X %02X %02X (162096C4 with the reserved bit)", b[0], b[1], b[2], b[3]);
        CHECK(b[8] >> 5 == 1 && ((b[8] >> 4) & 1) == 1 && ((b[8] >> 2) & 3) == 1, "engineering bits %02X", b[8]);
        const hrdcp::Header g = hrdcp::getHeader(b);
        CHECK(g.address == h.address && g.reserved && g.length == 1234 && g.sequence == 65535 && g.type == 1 && g.compression == 1 && g.health == 0x2A5 && g.version == 1, "header round trip");
        CHECK(hrdcp::framesFor(12 + 653 + 4) == 1 && hrdcp::framesFor(12 + 654 + 4) == 2 && hrdcp::framesFor(12 + 1322 + 4) == 2 && hrdcp::framesFor(12 + 7343 + 4) == 11,
              "frames per message follow table 4 (653 bytes in a 10 s slot, 1322 in 15 s, 7343 in 60 s)");
        // coding round trip through hard-decision Viterbi, with Reed-Solomon errors
        std::vector<uint8_t> data;
        for (int i = 0; i < 1000; i++) data.push_back((uint8_t)(i * 7 + 3));
        hrdcp::Header hh; hh.address = dcs::addressWord(0x2C412); hh.sequence = 7;
        const auto msg = hrdcp::buildMessage(hh, data);
        auto coded = hrdcp::codedBytes(msg, 2);
        CHECK(coded.size() == 2 * 765 + 2, "coded %zu", coded.size());
        std::vector<uint8_t> ib, qb;
        hrdcp::convEncode(coded, ib, qb);
        meteor::Viterbi27 v;
        std::mt19937 rng(3);
        int flips = 0;
        for (size_t i = 0; i < ib.size(); i++) {
            int a = ib[i] ? 60 : -60, q = qb[i] ? -60 : 60;   // the receiver's soft values: G1 as sent, G2 = not Q
            if (rng() % 50 == 0) { a = -a; flips++; }
            v.push(a, q);
        }
        v.flush();
        std::vector<uint8_t> info(coded.size());
        for (size_t i = 0; i < info.size(); i++) { uint8_t x = 0; for (int k = 0; k < 8; k++) x = (uint8_t)(x << 1 | v.out()[i * 8 + (size_t)k]); info[i] = x; }
        for (int k = 0; k < 20; k++) info[(size_t)(k * 61 + 5)] ^= 0x5A;   // byte errors the Reed-Solomon code must fix
        hrdcp::Decoded d;
        CHECK(hrdcp::decodeMessage(info, d) == 0 && d.headerOk && d.complete && d.crcOk && d.data == data && d.hdr.sequence == 7, "coding round trip (%d code bits flipped)", flips);
        CHECK(d.rsCorrected >= 20, "RS corrected %d", d.rsCorrected);
        std::vector<uint8_t> shortInfo(info.begin(), info.begin() + 765);
        hrdcp::Decoded d2;
        CHECK(hrdcp::decodeMessage(shortInfo, d2) == 1 && d2.headerOk && !d2.complete, "one frame of two");
        info[100] ^= 0xFF; info[1500] ^= 0x01;
        for (int k = 0; k < 40; k++) info[(size_t)(765 + k * 3)] ^= 0x33;   // too many for the second frame
        hrdcp::Decoded d3;
        hrdcp::decodeMessage(info, d3);
        CHECK(d3.complete && !d3.crcOk && d3.framesBad == 1, "a broken frame fails the CRC");
    }
    const std::vector<uint8_t> synop = bytes("SYNOP 06590 32970 10125 20087 39982 40213 58004=\r\n");
    std::vector<uint8_t> big;
    for (int i = 0; i < 1000; i++) big.push_back((uint8_t)((i * 131 + 7) & 0xFF));
    // ---- a short report and a 2-frame binary one, at once on neighbouring 3 kHz assignments, at 250 ksps
    {
        HrdcpGenConfig c;
        c.rate = 250000; c.centreHz = 402.3e6; c.ebn0Db = 12;
        c.tx = {tx(402.2950e6, 0x2C412, synop, 0.5), tx(402.2980e6, 0x0A1B2, big, 1.2)};
        c.tx[1].sequence = 300; c.tx[1].type = 1; c.tx[1].health = 0x0F3;
        std::vector<std::string> logs;
        const DcsTelemetry t = run(c, 16.0, &logs);
        printf("  two platforms: %llu ok, %llu bad, %llu carriers\n", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, (unsigned long long)t.carriersSeen);
        const DcsMessage* a = find(t, 0x2C412);
        const DcsMessage* b = find(t, 0x0A1B2);
        CHECK(a != nullptr, "short report");
        if (a) {
            CHECK(a->crcOk && a->sequence == 1 && a->hrType == 0 && a->health == 0x155 && a->raw == synop && a->format == 0, "short report fields");
            CHECK(a->text.find("SYNOP 06590") != std::string::npos && a->channel == 174 && a->plan == 1, "text, channel %d", a->channel);
            CHECK(dcs::addressHex(a->address) == "162096C4", "address %s", dcs::addressHex(a->address).c_str());
            CHECK(std::fabs(a->freqHz - 402.2950e6) < 5, "frequency %.1f", a->freqHz - 402.2950e6);
        }
        CHECK(b != nullptr, "2-frame report");
        if (b) CHECK(b->crcOk && b->raw == big && b->frames == 2 && b->sequence == 300 && b->hrType == 1 && b->health == 0x0F3 && b->format == 2, "2-frame fields: frames %d", b->frames);
        CHECK(t.blocksBad == 0, "bad %llu", (unsigned long long)t.blocksBad);
    }
    // ---- impairments: frequency error to the 125 Hz limit, drift 1 Hz/s, clock errors 50 ppm, 2 degrees phase noise, Eb/N0 7 dB
    {
        struct Imp { double hz, drift, ppm, pn, eb; const char* name; };
        const Imp im[] = {{125, 1.0, 50, 2, 8, "+125 Hz, +1 Hz/s, +50 ppm, 2 deg"},
                          {-125, -1.0, -50, 2, 8, "-125 Hz, -1 Hz/s, -50 ppm, 2 deg"},
                          {40, 0.3, 0, 0, 6.5, "Eb/N0 6.5 dB"}};
        for (const auto& m : im) {
            HrdcpGenConfig c;
            c.rate = 96000; c.centreHz = 402.3e6; c.ebn0Db = m.eb; c.seed = 11;
            auto a = tx(402.3155e6, 0x2C412, big, 0.4);
            a.extraHz = m.hz; a.driftHzPerSec = m.drift; a.ppm = m.ppm; a.phaseNoiseDeg = m.pn;
            c.tx = {a};
            std::vector<std::string> logs;
            const DcsTelemetry t = run(c, 13.5, &logs);
            const DcsMessage* r = find(t, 0x2C412);
            printf("  %s: %s (rs corrected %d)\n", m.name, r && r->crcOk ? "decoded" : "LOST", r ? r->rsCorrected : -1);
            CHECK(r && r->crcOk && r->raw == big, "%s", m.name);
        }
    }
    // ---- a repeat: the sequence counter counts up; a standard-rate platform 6 kHz away does not disturb it and is not taken for HRDCP
    {
        HrdcpGenConfig c;
        c.rate = 250000; c.centreHz = 402.3e6; c.ebn0Db = 11;
        auto a = tx(402.2725e6, 0x2C412, synop, 0.5);
        a.repeatSec = 8.0; a.sequence = 65534;
        c.tx = {a};
        GoesDcsGenConfig s;
        s.rate = 250000; s.centreHz = 402.3e6; s.snrDb = 36; s.seed = 4;
        GoesDcsGenTx st;
        st.freqHz = 402.2785e6; st.bps = 100; st.ext = false; st.carrierSec = 5; st.startSec = 0.2; st.address21 = 0x03FFF;
        const std::string sd = "WL 1.23 TW 4.5 BV 12.6 RAIN 0.0\r\n";
        st.data.assign(sd.begin(), sd.end());
        s.tx = {st};
        const DcsTelemetry t = run(c, 16.5, nullptr, &s);
        CHECK(find(t, 0x2C412, 65534) && find(t, 0x2C412, 65535), "both repeats (%llu ok)", (unsigned long long)t.blocksOk);
        CHECK(t.blocksOk == 2 && t.blocksBad == 0, "only the HRDCP reports: %llu ok %llu bad", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    }
    // ---- noise only: nothing
    {
        HrdcpGenConfig c;
        c.rate = 250000; c.ebn0Db = 10;
        c.tx = {tx(402.3e6, 0x2C412, synop, 1000)};
        const DcsTelemetry t = run(c, 8.0);
        CHECK(t.blocksOk == 0 && t.blocksBad == 0 && t.messages.empty(), "noise only: %llu ok %llu bad", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    }
    // ---- Eb/N0 sweep (ONAIR_TEST_LONG=1)
    if (longRun) {
        for (double eb : {9.0, 7.0, 6.0, 5.0, 4.5, 4.0, 3.5}) {
            int ok = 0;
            for (uint32_t seed = 1; seed <= 5; seed++) {
                HrdcpGenConfig c;
                c.rate = 96000; c.centreHz = 402.3e6; c.ebn0Db = eb; c.seed = seed;
                c.tx = {tx(402.3155e6, 0x2C412, big, 0.4)};
                const DcsTelemetry t = run(c, 13.5);
                const DcsMessage* r = find(t, 0x2C412);
                ok += r && r->crcOk;
            }
            printf("  Eb/N0 %.1f dB: %d of 5 messages (1000 bytes)\n", eb, ok);
        }
    }
    return testSummary("hrdcp");
}
