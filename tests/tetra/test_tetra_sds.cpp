// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// TETRA SDS interpreter: D-SDS-DATA, SDS-TL protocol identifier, LIP short and long location reports (every shape, velocity type, time type, type 5 elements) and simple text, by round trip with the generator, over the
// whole globe, every code of position error / speed / heading, and truncated or random bits (never a crash, never a false position).
#include "dect2/tetra/tetra_sds.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>
using namespace dect2::tetra;
#include "../common/test_util.h"

int main() {
    std::mt19937 g(9);
    // positions: error under the quantisation step (1.1e-5 degrees)
    double worst = 0; int n = 0;
    for (int i = 0; i < 2000; i++) {
        const double lat = -89.9 + 179.8 * (g() % 100000) / 100000.0, lon = -179.9 + 359.8 * (g() % 100000) / 100000.0;
        const int err = (int)(g() % 8), spd = (int)(g() % 128), dir = (int)(g() % 16), tel = (int)(g() % 4), reason = (int)(g() % 256);
        const Bits pdu = buildSdsData(2000000 + (int)(g() % 1000), buildLipShort(lat, lon, err, spd, dir, tel, reason), 0x0A);
        SdsMsg m;
        // buildSdsData prefixes the PID: the user data is the LIP PDU
        CHECK(parseSdsData(pdu, m) && m.valid && m.kind == "location" && m.hasPos, "parsed %d", i);
        worst = std::max(worst, std::max(std::fabs(m.lat - lat), std::fabs(m.lon - lon)));
        CHECK(m.timeElapsed == tel && m.reason == reason, "fields");
        CHECK(m.headingDeg == (dir == 15 ? -1 : (int)std::lround(dir * 22.5)), "heading");
        CHECK(spd == 127 ? m.speedKmh < 0 : m.speedKmh >= 0, "speed");
        n++;
    }
    printf("LIP round trip over %d random positions: worst error %.2e degrees (step 1.1e-5)\n", n, worst);
    CHECK(worst < 1.2e-5, "quantisation");
    // text
    {
        SdsMsg m;
        CHECK(parseSdsData(buildSdsText(2001234, "Unit 7 on scene, send EMS"), m) && m.kind == "text" && m.text == "Unit 7 on scene, send EMS" && m.callingIssi == 2001234, "text");
    }
    // not an SDS, cut short, random bits: no crash, no false position
    {
        SdsMsg m;
        Bits other(100, 0);
        CHECK(!parseSdsData(other, m), "type 0 is not D-SDS-DATA");
        const Bits ok = buildSdsData(5, buildLipShort(10, 20, 1, 2, 3, 0, 0), 0x0A);
        for (size_t cut = 0; cut < ok.size(); cut++) { Bits c(ok.begin(), ok.begin() + (long)cut); SdsMsg x; if (parseSdsData(c, x)) CHECK(!x.hasPos || cut >= ok.size() - 0, "cut %zu", cut); }
        int pos = 0;
        for (int i = 0; i < 20000; i++) { Bits r(120); for (auto& b : r) b = g() & 1; r[0] = 0; r[1] = 1; r[2] = 1; r[3] = 1; r[4] = 1; SdsMsg x; if (parseSdsData(r, x) && x.hasPos) pos++; }
        printf("random D-SDS-DATA bits with PID 0x0A-like content giving a position: %d of 20000\n", pos);
        CHECK(pos < 20, "random bits rarely look like a position (%d)", pos);
    }

    // ---- LIP long location report (TS 100 392-18-1 clause 6.2.2), hand-assembled from the tables, not from the builder
    {
        auto bin = [](Bits& b, long long v, int n) { for (int i = n - 1; i >= 0; i--) b.push_back((uint8_t)(((unsigned long long)v >> i) & 1)); };
        // PDU 01, extension 0011, time elapsed (time type 01, value 00), shape 0001 (point) at lon 11.25 / lat 45.0, velocity type 1, no ack, reason 3 (push-to-talk)
        Bits h;
        bin(h, 1, 2); bin(h, 3, 4); bin(h, 1, 2); bin(h, 0, 2); bin(h, 1, 4);
        bin(h, 1 << 20, 25); bin(h, 1 << 22, 24);       // lon = 2^20 * 360 / 2^25 = 11.25, lat = 2^22 * 180 / 2^24 = 45
        bin(h, 1, 3); bin(h, 20, 7);                     // velocity type 1: horizontal velocity only, code 20 = 20 km/h
        bin(h, 0, 1); bin(h, 0, 1); bin(h, 3, 8);
        SdsMsg m;
        CHECK(parseLipLong(h, 0, h.size(), m) && m.hasPos && m.lipPdu == 1 && m.shape == 1, "hand-made long report parsed");
        CHECK(std::fabs(m.lon - 11.25) < 1e-9 && std::fabs(m.lat - 45.0) < 1e-9, "lon %.9f lat %.9f", m.lon, m.lat);
        CHECK(m.speedKmh == 20.0 && m.headingDeg == -1 && m.reason == 3 && m.timeElapsed == 0 && !m.hasAlt, "fields");
        CHECK(m.source.find("push-to-talk") != std::string::npos && m.source.find("point") != std::string::npos, "source '%s'", m.source.c_str());
        // west and south: two's complement, -90 / -45 exactly; user data instead of a reason
        Bits w;
        bin(w, 1, 2); bin(w, 3, 4); bin(w, 0, 2); bin(w, 1, 4); bin(w, -(1 << 23), 25); bin(w, -(1 << 22), 24);
        bin(w, 0, 3); bin(w, 0, 1); bin(w, 1, 1); bin(w, 77, 8);
        SdsMsg n;
        CHECK(parseLipLong(w, 0, w.size(), n) && n.hasPos && std::fabs(n.lon + 90.0) < 1e-9 && std::fabs(n.lat + 45.0) < 1e-9 && n.reason == -1, "west / south");
        const Bits sh = buildLipShort(45.0, 11.25, 2, 20, 4, 0, 3);
        SdsMsg k;
        CHECK(parseLip(sh, 0, sh.size(), k) && k.lipPdu == 0 && k.reason == 3 && k.horizUncM == 200, "parseLip short");
    }
    // tables: altitude (6.62), horizontal position uncertainty (6.49), reasons (6.94)
    {
        CHECK(lipAltitudeM(0) < -1e8 && lipAltitudeM(1) == -200 && lipAltitudeM(2) == -199 && lipAltitudeM(1201) == 1000 && lipAltitudeM(1202) == 1002, "alt low");
        CHECK(lipAltitudeM(1926) == 2450 && lipAltitudeM(1927) == 2525 && lipAltitudeM(2045) == 11375 && lipAltitudeM(2046) == 11450 && lipAltitudeM(2047) == 11525, "alt high");
        CHECK(std::fabs(lipHorizUncM(0) - 1) < 1e-9 && std::fabs(lipHorizUncM(1) - 2) < 1e-9 && std::fabs(lipHorizUncM(2) - 3.2) < 1e-9, "unc small");
        CHECK(std::fabs(lipHorizUncM(10) - 27) < 0.1 && std::fabs(lipHorizUncM(20) - 187) < 1 && std::fabs(lipHorizUncM(30) - 1180) < 5 && lipHorizUncM(63) < 0, "unc large");
        CHECK(std::string(lipReasonText(2)) == "emergency" && std::string(lipReasonText(32)).find("immediate") != std::string::npos && lipReasonText(200)[0] == 0, "reasons");
    }
    // every shape x velocity type x time type over random positions, through D-SDS-DATA; every cut of the fixed part is refused
    {
        int n = 0;
        double worst = 0;
        for (int shape = 0; shape <= 10; shape++)
            for (int vt = 0; vt <= 7; vt++)
                for (int tt = 0; tt <= 2; tt++)
                    for (int rep = 0; rep < 6; rep++) {
                        LipLongSpec c;
                        c.lat = -89.9 + 179.8 * (g() % 100000) / 100000.0; c.lon = -179.9 + 359.8 * (g() % 100000) / 100000.0;
                        c.timeType = tt; c.timeElapsed = (int)(g() % 4); c.day = 1 + (int)(g() % 31); c.hour = (int)(g() % 24); c.minute = (int)(g() % 60); c.second = (int)(g() % 60);
                        c.shape = shape; c.hpu = (int)(g() % 63); c.minorAxis = (int)(g() % 64); c.angle = (int)(g() % 256); c.confidence = (int)(g() % 8);
                        c.altRef = (int)(g() % 2); c.altValue = 1 + (int)(g() % 2047); c.altUnc = (int)(g() % 8); c.posErr = (int)(g() % 8);
                        c.inner = (int)(g() % 65536); c.outer = (int)(g() % 65536); c.startAngle = (int)(g() % 256); c.stopAngle = (int)(g() % 256);
                        c.velType = vt; c.hv = (int)(g() % 127); c.hvUnc = (int)(g() % 8); c.vvSign = (int)(g() % 2); c.vv = (int)(g() % 127); c.vvUnc = (int)(g() % 8);
                        c.dirExt = (int)(g() % 256); c.dirUnc = (int)(g() % 8);
                        c.ack = (int)(g() % 2); c.addType = (int)(g() % 2); c.addData = (int)(g() % 256);
                        const Bits fixed = buildLipLong(c);
                        const int issi = 2000000 + (int)(g() % 100000);
                        const Bits pdu = buildSdsData(issi, fixed, 0x0A);
                        SdsMsg m;
                        CHECK(parseSdsData(pdu, m) && m.valid, "SDS shape %d vt %d tt %d", shape, vt, tt);
                        CHECK(m.lipPdu == 1 && m.shape == shape && m.callingIssi == issi, "pdu %d shape %d", m.lipPdu, m.shape);
                        CHECK(m.hasPos == (shape != 0), "hasPos shape %d", shape);
                        if (shape != 0) {
                            worst = std::max(worst, std::max(std::fabs(m.lat - c.lat), std::fabs(m.lon - c.lon)));
                            CHECK(m.kind == "location", "kind");
                            const bool hasAlt = shape >= 4 && shape <= 8;
                            CHECK(m.hasAlt == hasAlt, "alt flag shape %d", shape);
                            if (hasAlt) CHECK(m.altM == lipAltitudeM(c.altValue) && m.altRef == c.altRef, "alt value");
                        }
                        CHECK(m.ackRequested == c.ack, "ack");
                        if (c.addType == 0) CHECK(m.reason == c.addData, "reason"); else CHECK(m.reason == -1, "no reason when user data");
                        if (tt == 1) CHECK(m.timeElapsed == c.timeElapsed && !m.hasTimePos, "elapsed");
                        if (tt == 2) CHECK(m.hasTimePos && m.tpDay == c.day && m.tpHour == c.hour && m.tpMin == c.minute && m.tpSec == c.second, "time of position");
                        if (vt >= 1) CHECK(std::fabs(m.speedKmh - (c.hv <= 28 ? c.hv : 16.0 * std::pow(1.038, c.hv - 13))) < 1e-9, "speed vt %d", vt);
                        if (vt >= 5) CHECK(m.headingDeg == (int)std::lround(c.dirExt * 360.0 / 256.0) % 360, "heading ext");
                        if (vt == 3 || vt == 4 || vt == 7) CHECK(m.hasVert, "vertical"); else CHECK(!m.hasVert, "no vertical");
                        for (size_t cut = 0; cut < fixed.size(); cut++) { SdsMsg x; CHECK(!parseLipLong(fixed, 0, cut, x), "cut %zu of %zu shape %d vt %d", cut, fixed.size(), shape, vt); }
                        n++;
                    }
        printf("LIP long report: %d reports over every shape, velocity type and time type, worst position error %.2e degrees\n", n, worst);
        CHECK(worst < 1.2e-5, "long report quantisation");
    }
    // type 5 elements after the fixed part
    {
        auto put = [](Bits& b, long long v, int n) { for (int i = n - 1; i >= 0; i--) b.push_back((uint8_t)(((unsigned long long)v >> i) & 1)); };
        LipLongSpec c;
        c.lat = 25.2; c.lon = 55.3; c.shape = 2; c.hpu = 12; c.velType = 5; c.hv = 33; c.dirExt = 64; c.addType = 0; c.addData = 32;
        Bits ssi; put(ssi, 1, 4); put(ssi, 2123456, 24);   // terminal identification: address type SSI
        Bits ref; put(ref, 99, 8);
        Bits res; put(res, 12, 8);
        Bits st; put(st, 0x8001, 16);
        Bits big; for (int i = 0; i < 80; i++) big.push_back((uint8_t)(i & 1));   // 80 bits: element length 0, extension 3 (ten octets)
        c.type5 = {{17, ssi}, {6, ref}, {12, res}, {15, st}, {1, big}};
        const Bits fixed = buildLipLong(c);
        SdsMsg m;
        CHECK(parseLipLong(fixed, 0, fixed.size(), m) && m.hasPos, "type 5 parsed");
        CHECK(m.terminalIssi == 2123456 && m.msgRef == 99 && m.resultCode == 12 && m.statusValue == 0x8001, "type 5 values %d %d %d %d", m.terminalIssi, m.msgRef, m.resultCode, m.statusValue);
        CHECK(std::fabs(m.horizUncM - lipHorizUncM(12)) < 1e-9 && m.reason == 32 && m.headingDeg == 90, "circle, heading");
        Bits cut = fixed; cut.resize(cut.size() - 40);   // an element claiming more bits than are left ends the list; the fix stays
        SdsMsg m2;
        CHECK(parseLipLong(cut, 0, cut.size(), m2) && m2.hasPos && std::fabs(m2.lat - 25.2) < 1e-4, "truncated tail keeps the fix");
        LipLongSpec z; z.shape = 0; z.velType = 0; z.addType = 0; z.addData = 4; z.type5 = {{15, st}};   // no location: a status, not a map point
        const Bits zb = buildSdsData(77, buildLipLong(z), 0x0A);
        SdsMsg zm;
        CHECK(parseSdsData(zb, zm) && !zm.hasPos && zm.statusValue == 0x8001 && zm.kind == "status / data", "status without location");
    }
    // reserved values and other PDUs; random bits behind a long header never give an out-of-range position
    {
        for (int shape = 11; shape <= 15; shape++) {
            LipLongSpec c; c.shape = shape; const Bits b = buildLipLong(c); SdsMsg m;
            CHECK(!parseLipLong(b, 0, b.size(), m), "reserved shape %d", shape);
        }
        LipLongSpec c; c.timeType = 3; const Bits b = buildLipLong(c); SdsMsg m;
        CHECK(!parseLipLong(b, 0, b.size(), m), "reserved time type");
        Bits other(100, 0); other[1] = 1;   // PDU type 1 but extension 0
        CHECK(!parseLipLong(other, 0, other.size(), m) && !parseLip(Bits(100, 1), 0, 100, m), "other PDU types");
        int pos = 0;
        for (int i = 0; i < 20000; i++) {
            Bits r(40 + g() % 120); for (auto& x : r) x = g() & 1;
            r[0] = 0; r[1] = 1; r[2] = 0; r[3] = 0; r[4] = 1; r[5] = 1;
            SdsMsg x;
            if (parseLipLong(r, 0, r.size(), x) && x.hasPos) { pos++; CHECK(std::fabs(x.lat) <= 90 && std::fabs(x.lon) <= 180, "range"); }
        }
        printf("random long-report bits giving a position: %d of 20000 (all in range)\n", pos);
    }
    return testSummary("tetra sds");
}
