// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Per-aircraft trust score and the ACAS conflict timeline. All signals here are simulated: a learned level curve from a synthetic fleet with
// random antenna gain per aircraft, then ghosts of each kind; DF16 and TC 28 advisories through the real tracker and parser.
#include "dect2/adsb/adsb_encode.h"
#include "dect2/adsb/adsb_msg.h"
#include "dect2/adsb/adsb_reg.h"
#include "dect2/adsb/adsb_track.h"
#include "dect2/adsb/adsb_trust.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
using namespace dect2;
#include "../common/test_util.h"

struct Sim { std::mt19937 rng{7}; double gain(double range) { std::normal_distribution<double> n(0, 3); return -8 - 20 * std::log10(range) + n(rng); } };

static AdsbTrustObs at(uint32_t id, double t, double rangeNm, double brg, double level, bool confirmed = true) {
    AdsbTrustObs o; o.icao = id; o.timeSec = t; o.hasRange = true; o.rangeNm = rangeNm; o.bearingDeg = brg; o.levelDbfs = level; o.confirmed = confirmed;
    const double r = brg * M_PI / 180; o.lat = 55 + rangeNm * std::cos(r) / 60; o.lon = 25 + rangeNm * std::sin(r) / (60 * std::cos(55 * M_PI / 180));
    o.hasAlt = true; o.altFt = 36000; return o;
}

static void learn(AdsbTrustModel& m, Sim& s, double& t) {
    for (int k = 0; k < 40; k++) {
        const uint32_t id = 0x400000 + k; const double r0 = 20 + k * 6, br = k * 9.0;
        for (int i = 0; i < 30; i++) { const double r = r0 + i * 0.1; m.observe(at(id, t + i * 11.0, r, br, s.gain(r))); }
    }
    t += 340;
}

static int finalScore(AdsbTrustModel& m, uint32_t id, double t) { return m.verdict(id, t).score; }

int main() {
    Sim s; double t = 0;
    {   // honest fleet, noise sweep: nothing is flagged after the curve is learned, also with 6 dB wider level scatter
        AdsbTrustModel m; learn(m, s, t);
        CHECK(m.calibrated(), "calibrated after 40 aircraft");
        int flagged = 0;
        for (int k = 40; k < 120; k++) for (int i = 0; i < 20; i++) {
            const double r = 15 + k * 2.0 + i * 0.12;
            std::normal_distribution<double> extra(0, k < 80 ? 0.0 : 3.0);
            m.observe(at(0x500000 + k, t + i * 3.0, r, k * 11.0, s.gain(r) + extra(s.rng)));
        }
        for (int k = 40; k < 120; k++) if (m.verdict(0x500000 + k, t + 60).doubtful()) flagged++;
        CHECK(flagged <= 1, "honest aircraft flagged: %d of 80", flagged);
        printf("honest flagged %d / 80\n", flagged);
    }
    {   // a ghost: claims 250 nm but arrives as loud as something 5 nm away
        AdsbTrustModel m; learn(m, s, t);
        for (int i = 0; i < 6; i++) m.observe(at(0x700000, t + i * 2.0, 250, 40, -14));
        const auto v = m.verdict(0x700000, t + 12);
        CHECK(v.flags & kTrustTooLoud, "loud ghost flag");
        CHECK(v.doubtful() && v.score <= 45, "loud ghost score %d", v.score);
        CHECK(!v.why.empty(), "a reason is given");
    }
    {   // the level check is off for relayed (TIS-B) tracks and for positions from another receiver
        AdsbTrustModel m; learn(m, s, t);
        for (int i = 0; i < 6; i++) { auto o = at(0x700001, t + i * 2.0, 250, 40, -14); o.relayed = true; m.observe(o); }
        for (int i = 0; i < 6; i++) { auto o = at(0x700002, t + i * 2.0, 250, 40, -14); o.hasLevel = false; m.observe(o); }
        CHECK(finalScore(m, 0x700001, t + 12) == 100, "relayed not judged by level");
        CHECK(finalScore(m, 0x700002, t + 12) == 100, "other receiver not judged by level");
    }
    {   // before the curve exists nothing can be said about level
        AdsbTrustModel m;
        for (int i = 0; i < 6; i++) m.observe(at(0x700003, i * 2.0, 250, 40, -14));
        CHECK(finalScore(m, 0x700003, 12) == 100, "no verdict from level before calibration");
    }
    {   // beyond the horizon: FL100 at 400 nm
        AdsbTrustModel m;
        for (int i = 0; i < 4; i++) { auto o = at(0x700010, i * 2.0, 400, 10, -40); o.altFt = 10000; m.observe(o); }
        CHECK(m.verdict(0x700010, 8).flags & kTrustHorizon, "horizon");
    }
    {   // position jumps (a CPR error once costs 40, it does not call the track a ghost alone) and speed against positions
        AdsbTrustModel m;
        m.observe(at(0x700020, 0, 50, 0, -40)); m.observe(at(0x700020, 2, 150, 0, -40));
        auto v = m.verdict(0x700020, 3);
        CHECK(v.flags & kTrustJump, "jump");
        CHECK(!v.ghost(), "one jump is no ghost: %d", v.score);
        AdsbTrustModel m2;
        for (int i = 0; i < 40; i++) { auto o = at(0x700021, i * 2.0, 100 + i * 2.0 * 450.0 / 3600, 10, -40); o.hasGroundSpeed = true; o.groundSpeedKt = i ? 90 : 90; m2.observe(o); }
        CHECK(m2.verdict(0x700021, 80).flags & kTrustSpeed, "claims 90 kt, moves 450");
        AdsbTrustModel m3;
        for (int i = 0; i < 40; i++) { auto o = at(0x700022, i * 2.0, 100 + i * 2.0 * 450.0 / 3600, 10, -40); o.hasGroundSpeed = true; o.groundSpeedKt = 440; m3.observe(o); }
        CHECK(finalScore(m3, 0x700022, 80) == 100, "speed agrees");
    }
    {   // identity: N-number that does not fit the address, the right one does; one callsign in two places
        uint32_t right = 0; CHECK(adsbIcaoFromNNumber("N12345", right), "n-number");
        AdsbTrustModel m;
        for (int i = 0; i < 3; i++) { auto o = at(right, i * 2.0, 40, 10, -40); o.callsign = "N12345"; m.observe(o); }
        for (int i = 0; i < 3; i++) { auto o = at(0x4CA123, i * 2.0, 40, 200, -40); o.callsign = "N12345 "; m.observe(o); }
        CHECK(!(m.verdict(right, 6).flags & kTrustRegistration), "right address");
        CHECK(m.verdict(0x4CA123, 6).flags & kTrustRegistration, "wrong address");
        AdsbTrustModel d;
        for (int i = 0; i < 4; i++) { auto o = at(0x400100, i * 2.0, 40, 10, -40); o.callsign = "RYR123"; d.observe(o); o = at(0x400200, i * 2.0 + 1, 150, 200, -40); o.callsign = "RYR123"; d.observe(o); }
        CHECK(d.verdict(0x400100, 9).flags & kTrustDuplicate, "duplicate callsign");
        AdsbTrustModel n;
        for (int i = 0; i < 4; i++) { auto o = at(0x400100, i * 2.0, 40, 10, -40); o.callsign = "RYR123"; n.observe(o); o = at(0x400200, i * 2.0 + 1, 41, 10, -40); o.callsign = "RYR123"; n.observe(o); }
        CHECK(!(n.verdict(0x400100, 9).flags & kTrustDuplicate), "two next to each other is not a clash");
    }
    {   // a finding fades
        AdsbTrustModel m;
        m.observe(at(0x700030, 0, 50, 0, -40)); m.observe(at(0x700030, 2, 150, 0, -40));
        CHECK(m.verdict(0x700030, 2 + kTrustHoldSec + 1).flags == 0, "faded");
    }
    {   // the time base moves with the session
        AdsbTrustModel m; m.observe(at(0x700031, 0, 50, 0, -40)); m.observe(at(0x700031, 2, 150, 0, -40)); m.shiftTime(1000);
        CHECK(m.verdict(0x700031, 1003).flags & kTrustJump, "shifted");
    }
    // ---- the advisories through the real parser and table
    using namespace dect2::adsb;
    auto raMv = [](uint8_t* mb, uint32_t threat, unsigned ara, bool term) {
        std::memset(mb, 0, 7); mb[0] = 0x30;
        auto set = [&](int from, int n, uint32_t v) { for (int i = 0; i < n; i++) { int bit = from - 1 + i; if ((v >> (n - 1 - i)) & 1) mb[bit / 8] |= 0x80 >> (bit % 8); } };
        set(9, 14, ara); set(27, 1, term); set(29, 2, 1); set(31, 24, threat);
    };
    const uint32_t A = 0x4B1234, B = 0x4B5678;
    {
        AdsbTracker tr; tr.setReference(55, 25);
        auto feed = [&](Frame f, double when) { AdsbRaw raw; std::memcpy(raw.bytes, f.b, 14); raw.bits = f.bits; for (auto& c : raw.conf) c = 10; raw.levelDbfs = -30; raw.timeSec = when; AdsbFrame out; return tr.accept(raw, out); };
        double T = 10;
        for (uint32_t id : {A, B}) {
            const double la = id == A ? 55.2 : 55.3;
            CHECK(feed(encodeAirbornePosition(id, 5, 11, 36000, false, false, la, 25.1), T), "even"); T += 0.5;
            CHECK(feed(encodeAirbornePosition(id, 5, 11, 36000, false, true, la, 25.1), T), "odd"); T += 0.5;
        }
        // DF16 with MV = BDS 3,0
        Frame df16 = encodeAirAir(A, false, 36000, true);
        raMv(df16.b + 4, B, 0x2000, false);
        { df16.b[11]=df16.b[12]=df16.b[13]=0; uint32_t p = crcParity(df16.b, 112) ^ A; df16.b[11] = p >> 16; df16.b[12] = p >> 8; df16.b[13] = p; }
        CHECK(feed(df16, T), "df16 accepted"); T += 1;
        AdsbTelemetry t; tr.snapshot(t, T);
        CHECK(t.raEvents.size() == 1, "one event, got %zu", t.raEvents.size());
        if (!t.raEvents.empty()) {
            const auto& e = t.raEvents[0];
            CHECK(e.icao == A && e.threat == B, "pair");
            CHECK(e.via == "air-air", "via %s", e.via.c_str());
            CHECK(e.hasSeparation && e.separationNm > 3 && e.separationNm < 9, "sep %.2f", e.separationNm);
            CHECK(e.hasThreatAlt && e.threatAltFt == 36000, "threat alt");
        }
        // TC 28 subtype 2: same advisory repeated merges; terminated flag is carried
        uint8_t mb[7]; raMv(mb, B, 0x2000, true); mb[0] = (28 << 3) | 2;
        Frame es = encodeAircraftStatus(A, 5, 0, 7000);
        std::memcpy(es.b + 4, mb, 7);
        { es.b[11]=es.b[12]=es.b[13]=0; uint32_t p = crcParity(es.b, 112); es.b[11] = p >> 16; es.b[12] = p >> 8; es.b[13] = p; }
        CHECK(feed(es, T), "tc28/2 accepted"); T += 1;
        tr.snapshot(t, T);
        CHECK(t.raEvents.size() == 1 && t.raEvents[0].updates == 2 && t.raEvents[0].terminated && t.raEvents[0].via == "squitter", "merged, terminated");
        bool hasRa = false; for (auto& a : t.aircraft) if (a.icao == A) hasRa = a.hasRa && a.raThreat == B;
        CHECK(hasRa, "aircraft shows the RA and threat");
        // VDS other than 0x30: not an advisory
        Frame other = encodeAirAir(B, false, 36000, true);
        { other.b[11]=other.b[12]=other.b[13]=0; uint32_t p = crcParity(other.b, 112) ^ B; other.b[11] = p >> 16; other.b[12] = p >> 8; other.b[13] = p; }
        feed(other, T);
        tr.snapshot(t, T);
        CHECK(t.raEvents.size() == 1, "no event from plain DF16");
    }
    return testSummary("adsb_trust");
}
