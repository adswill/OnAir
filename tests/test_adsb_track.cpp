// ADS-B frame checks and the aircraft table: published messages go in as sliced frames, the table must come out as the documents say.
// Messages and positions: "The 1090 Megahertz Riddle" (see test_adsb_msg.cpp). Everything else is built here with the encoders and checked against what was put in.
#include "dect2/adsb_encode.h"
#include "dect2/adsb_track.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static AdsbRaw raw(const char* hex, double t, float level = -30.f) {
    AdsbRaw r;
    const int n = (int)strlen(hex) / 2;
    for (int i = 0; i < n; i++) { unsigned v; sscanf(hex + 2 * i, "%2x", &v); r.bytes[i] = (uint8_t)v; }
    r.bits = n * 8;
    r.levelDbfs = level; r.snrDb = 20; r.timeSec = t;
    for (float& c : r.conf) c = 10.f;
    return r;
}
static AdsbRaw raw(const adsb::Frame& f, double t, float level = -30.f) {
    AdsbRaw r;
    memcpy(r.bytes, f.b, 14);
    r.bits = f.bits; r.levelDbfs = level; r.snrDb = 20; r.timeSec = t;
    for (float& c : r.conf) c = 10.f;
    return r;
}
static bool feed(AdsbTracker& tr, AdsbRaw r) { AdsbFrame f; return tr.accept(r, f); }
static const AdsbAircraft* find(const AdsbTelemetry& t, uint32_t icao) { for (auto& a : t.aircraft) if (a.icao == icao) return &a; return nullptr; }
static AdsbTelemetry snap(const AdsbTracker& tr, double now) { AdsbTelemetry t; tr.snapshot(t, now); return t; }
// the aircraft by value: a pointer into a temporary report would dangle
static AdsbAircraft get(const AdsbTracker& tr, uint32_t icao, double now) {
    AdsbTelemetry t = snap(tr, now);
    for (auto& a : t.aircraft) if (a.icao == icao) return a;
    AdsbAircraft none;
    none.icao = 0xFFFFFFFF;
    return none;
}

static void testPublishedMessages() {
    AdsbTracker tr;
    // identification and the position pair of the Riddle
    CHECK(feed(tr, raw("8D4840D6202CC371C32CE0576098", 1.0)), "identification rejected");
    CHECK(feed(tr, raw("8D40621D58C382D690C8AC2863A7", 2.0)), "even position rejected");
    CHECK(feed(tr, raw("8D40621D58C386435CC412692AD6", 3.0)), "odd position rejected");
    CHECK(feed(tr, raw("8D485020994409940838175B284F", 4.0)), "velocity rejected");
    AdsbTelemetry t = snap(tr, 4.0);
    CHECK(t.aircraftCount == 3, "aircraft %u", t.aircraftCount);
    const AdsbAircraft* a = find(t, 0x4840D6);
    CHECK(a && a->callsign == "KLM1023" && a->category.empty(), "KLM1023: %s", a ? a->callsign.c_str() : "missing");
    a = find(t, 0x40621D);
    CHECK(a && a->hasAlt && a->altFt == 38000 && a->hasPos && a->posKind == 2, "position aircraft");
    if (a) CHECK(std::fabs(a->lat - 52.26578017412606) < 1e-9 && std::fabs(a->lon - 3.93891) < 1e-4, "global position %.8f %.8f", a->lat, a->lon);   // odd frame latest
    a = find(t, 0x485020);
    CHECK(a && a->hasSpeed && std::fabs(a->speedKt - 159) < 0.5 && a->hasHeading && std::fabs(a->headingDeg - 182.88) < 0.01 && a->hasVrate && a->vrateFpm == -832, "velocity aircraft");
    CHECK(t.dfCount[17] == 4 && t.blocksOk == 0 /* set by the receiver */ && tr.good() == 4, "counts DF17 %llu good %llu", (unsigned long long)t.dfCount[17], (unsigned long long)tr.good());
    CHECK(t.frames.size() == 4 && t.frames[0].hex == "8D4840D6202CC371C32CE0576098" && t.frames[0].what == "Identification KLM1023", "frame list: %s", t.frames.empty() ? "" : t.frames[0].what.c_str());
    // Mode S replies of known aircraft: the address is in the parity, so they only count after the aircraft has been heard cleanly
    AdsbTracker tr2;
    CHECK(!feed(tr2, raw("2000171806A983", 1.0)), "an altitude reply from an unknown address was accepted");
    CHECK(tr2.bad() == 1, "bad count %llu", (unsigned long long)tr2.bad());
    // the address of that reply: the remainder of the whole message
    uint8_t m[7] = {0x20, 0x00, 0x17, 0x18, 0x06, 0xA9, 0x83};
    const uint32_t addr = adsb::crc24(m, 56);
    char hex[32];
    adsb::Frame sq = adsb::encodeAllCall(addr, 5, 0);
    CHECK(feed(tr2, raw(sq, 0.5)), "all-call of %06X rejected", addr);
    snprintf(hex, sizeof hex, "%s", "2000171806A983");
    CHECK(feed(tr2, raw(hex, 1.0)), "the altitude reply is rejected although %06X is known", addr);
    AdsbTelemetry t2 = snap(tr2, 1.0);
    const AdsbAircraft* b = find(t2, addr);
    CHECK(b && b->hasAlt && b->altFt == 36000 && !b->ground, "altitude of the replying aircraft");
}

static void testChecks() {
    AdsbTracker tr;
    const uint32_t icao = 0xABCDEF;
    adsb::Frame id = adsb::encodeIdentification(icao, 5, 4, 3, "TEST123");
    CHECK(feed(tr, raw(id, 1.0)), "clean identification");
    // a bad CRC is rejected and counted
    AdsbRaw bad = raw(adsb::encodeAirbornePosition(icao, 5, 11, 30000, false, false, 25.0, 55.0), 2.0);
    bad.bytes[5] ^= 0x10; bad.bytes[9] ^= 0x01;      // two flipped bits
    tr.setOptions({1, false, 60});
    CHECK(!feed(tr, bad), "a frame with two errors was accepted with 1-bit correction");
    CHECK(tr.bad() == 1, "bad %llu", (unsigned long long)tr.bad());
    // one flipped bit in a frame of a known aircraft is repaired
    AdsbRaw one = raw(adsb::encodeAirbornePosition(icao, 5, 11, 30000, false, true, 25.0, 55.0), 3.0);
    for (int p = 0; p < 112; p++) {
        AdsbRaw r = one;
        r.bytes[p >> 3] ^= (uint8_t)(0x80 >> (p & 7));
        AdsbFrame f;
        const bool ok = tr.accept(r, f);
        CHECK(ok && f.corrected == 1 && memcmp(f.bytes, one.bytes, 14) == 0, "bit %d: ok %d corrected %d", p, ok, f.corrected);
    }
    CHECK(tr.corrected() == 112, "corrected %llu", (unsigned long long)tr.corrected());
    // the same from an address that has not been heard: refused, unless allowed
    AdsbTracker cold;
    AdsbRaw r = one;
    r.bytes[6] ^= 0x04;
    CHECK(!feed(cold, r), "a repaired frame of an unknown aircraft was accepted");
    cold.setOptions({1, true, 60});
    CHECK(feed(cold, r) && cold.corrected() == 1, "fixUnknown did not repair it");
    // correction off
    AdsbTracker off;
    off.setOptions({0, false, 60});
    feed(off, raw(id, 1.0));
    r = one; r.bytes[6] ^= 0x04;
    CHECK(!feed(off, r), "correction is off but the frame was repaired");
    // two flipped bits with the two least certain bits at those places
    AdsbTracker two;
    two.setOptions({2, false, 60});
    feed(two, raw(id, 1.0));
    r = one; r.bytes[3] ^= 0x20; r.bytes[8] ^= 0x02;
    for (float& c : r.conf) c = 10.f;
    r.conf[3 * 8 + 2] = 0.5f; r.conf[8 * 8 + 6] = 0.7f;
    AdsbFrame f;
    CHECK(two.accept(r, f) && f.corrected == 2 && memcmp(f.bytes, one.bytes, 14) == 0, "two bit correction");
    // ... but not when the bits that are wrong are not among the least certain
    r = one; r.bytes[3] ^= 0x20; r.bytes[8] ^= 0x02;
    for (float& c : r.conf) c = 10.f;
    CHECK(!two.accept(r, f), "two errors that the soft values do not point at were repaired");
    // an error in the DF field itself (DF17 -> DF19, a one-bit change) is repaired too
    r = one; r.bytes[0] ^= 0x02;
    CHECK(tr.accept(r, f) && f.df == 17, "DF field error: df %d", f.df);
    // DF11 with an interrogator code: accepted only for a known address
    AdsbTracker ii;
    adsb::Frame ac = adsb::encodeAllCall(0x123456, 5, 9);
    CHECK(!feed(ii, raw(ac, 1.0)), "all-call with interrogator code 9 accepted for an unknown address");
    feed(ii, raw(adsb::encodeAllCall(0x123456, 5, 0), 1.0));
    CHECK(feed(ii, raw(ac, 2.0)), "all-call with interrogator code 9 refused for a known address");
    // an unknown DF
    CHECK(!feed(ii, raw("0000000000000000", 3.0)), "empty frame");
}

static void testPlausibility() {
    // replies of the address / parity kind from a known aircraft are refused when they do not fit what the aircraft is known to do
    AdsbTracker tr;
    const uint32_t icao = 0x4B7777;
    feed(tr, raw(adsb::encodeAirbornePosition(icao, 5, 11, 30000, false, false, 47.0, 8.5), 1.0, -30.f));
    CHECK(feed(tr, raw(adsb::encodeAltitudeReply(icao, 0, 30200, false), 2.0, -30.f)), "an altitude reply that fits was refused");
    CHECK(!feed(tr, raw(adsb::encodeAltitudeReply(icao, 0, 38000, false), 3.0, -30.f)), "an altitude reply 8000 ft off the ADS-B altitude was accepted");
    CHECK(!feed(tr, raw(adsb::encodeIdentityReply(icao, 0, 1200), 4.0, -70.f)), "a reply 40 dB weaker than the aircraft's squitters was accepted");
    CHECK(feed(tr, raw(adsb::encodeIdentityReply(icao, 0, 1200), 4.5, -45.f)), "a reply 15 dB weaker was refused");
    // flight status 6 and 7 are not assigned
    adsb::Frame f6 = adsb::encodeIdentityReply(icao, 6, 1200);
    CHECK(!feed(tr, raw(f6, 5.0, -30.f)), "flight status 6 accepted");
    // more than 30 s after the last ADS-B altitude there is no yardstick for an altitude any more (the address is still known: DF11 at 35 s)
    feed(tr, raw(adsb::encodeAllCall(icao, 5, 0), 35.0, -30.f));
    CHECK(feed(tr, raw(adsb::encodeAltitudeReply(icao, 0, 38000, false), 36.0, -30.f)), "an altitude reply was refused 35 s after the last ADS-B altitude");
    // the extended length formats are DF 24 - 27 (bit 3 is a spare bit); 28 - 31 are not looked for
    CHECK(adsb::dfLength(24) == 112 && adsb::dfLength(27) == 112 && adsb::dfLength(28) == 0 && adsb::dfLength(31) == 0 && adsb::dfLength(22) == 0 && adsb::dfLength(1) == 0, "dfLength");
}

static void testPositions() {
    AdsbAircraft ac;
    const AdsbAircraft* a = nullptr;
    AdsbTracker tr;
    const uint32_t icao = 0x4B1234;
    const double lat = 47.0, lon = 8.5;
    // without a reference one frame gives nothing; the pair gives a global fix
    CHECK(feed(tr, raw(adsb::encodeAirbornePosition(icao, 5, 11, 20000, false, false, lat, lon), 1.0)), "first");
    AdsbTelemetry t = snap(tr, 1.0);
    CHECK(!find(t, icao)->hasPos, "a single frame gave a position without a reference");
    CHECK(feed(tr, raw(adsb::encodeAirbornePosition(icao, 5, 11, 20000, false, true, lat, lon), 1.5)), "second");
    t = snap(tr, 1.5);
    a = find(t, icao);
    CHECK(a->hasPos && a->posKind == 2 && std::fabs(a->lat - lat) < 1e-4 && std::fabs(a->lon - lon) < 2e-4, "global %.5f %.5f kind %d", a->lat, a->lon, a->posKind);
    // then single frames follow it by local decoding
    CHECK(feed(tr, raw(adsb::encodeAirbornePosition(icao, 5, 11, 20000, false, false, lat + 0.01, lon + 0.01), 15.0)), "third");   // the odd frame is too old for a pair
    ac = get(tr, icao, 15.0); a = &ac;
    CHECK(a->posKind == 3 && std::fabs(a->lat - (lat + 0.01)) < 1e-4, "local %.5f kind %d", a->lat, a->posKind);
    CHECK(a->track.size() >= 1, "track history");
    // frames more than 10 s apart do not make a global fix
    AdsbTracker slow;
    feed(slow, raw(adsb::encodeAirbornePosition(icao, 5, 11, 20000, false, false, lat, lon), 1.0));
    feed(slow, raw(adsb::encodeAirbornePosition(icao, 5, 11, 20000, false, true, lat, lon), 12.5));
    ac = get(slow, icao, 12.5);
    CHECK(!ac.hasPos, "a pair 11.5 s apart gave a position");
    // a reference position gives a local decode of the first frame, marked as not confirmed; the pair then confirms it
    AdsbTracker ref;
    ref.setReference(47.5, 9.0);
    feed(ref, raw(adsb::encodeAirbornePosition(icao, 5, 11, 20000, false, false, lat, lon), 1.0));
    ac = get(ref, icao, 1.0); a = &ac;
    CHECK(a->hasPos && a->posKind == 1 && std::fabs(a->lat - lat) < 1e-4, "local from reference: kind %d lat %.5f", a->posKind, a->lat);
    CHECK(a->hasRange && std::fabs(a->distNm - adsb::distanceNm(47.5, 9.0, lat, lon)) < 0.01 && std::fabs(a->bearingDeg - adsb::bearingDeg(47.5, 9.0, lat, lon)) < 0.01, "range and bearing %.2f NM %.1f", a->distNm, a->bearingDeg);
    feed(ref, raw(adsb::encodeAirbornePosition(icao, 5, 11, 20000, false, true, lat, lon), 1.5));
    ac = get(ref, icao, 1.5); a = &ac;
    CHECK(a->posKind == 2, "the pair did not confirm the position (kind %d)", a->posKind);
    CHECK(std::fabs(snap(ref, 1.5).maxRangeNm - adsb::distanceNm(47.5, 9.0, lat, lon)) < 0.01, "maximum range %.1f", snap(ref, 1.5).maxRangeNm);
    // an impossible jump is dropped; three in a row are believed
    AdsbTracker jump;
    feed(jump, raw(adsb::encodeAirbornePosition(icao, 5, 11, 20000, false, false, lat, lon), 1.0));
    feed(jump, raw(adsb::encodeAirbornePosition(icao, 5, 11, 20000, false, true, lat, lon), 1.5));
    const double lat2 = lat + 3.0;   // 180 NM in 1 s
    feed(jump, raw(adsb::encodeAirbornePosition(icao, 5, 11, 20000, false, false, lat2, lon), 2.5));
    feed(jump, raw(adsb::encodeAirbornePosition(icao, 5, 11, 20000, false, true, lat2, lon), 3.0));   // global fix of the pair: far from the last one
    ac = get(jump, icao, 3.0); a = &ac;
    CHECK(std::fabs(a->lat - lat) < 0.01, "the jump was believed at once (lat %.4f)", a->lat);
    feed(jump, raw(adsb::encodeAirbornePosition(icao, 5, 11, 20000, false, false, lat2, lon), 3.5));
    feed(jump, raw(adsb::encodeAirbornePosition(icao, 5, 11, 20000, false, true, lat2, lon), 4.0));
    ac = get(jump, icao, 4.0); a = &ac;
    CHECK(std::fabs(a->lat - lat2) < 0.01, "a position that keeps coming was never accepted (lat %.4f)", a->lat);
    // the southern hemisphere and the date line
    AdsbTracker sh;
    const double slat = -33.9, slon = 179.9;
    feed(sh, raw(adsb::encodeAirbornePosition(icao, 5, 11, 20000, false, false, slat, slon), 1.0));
    feed(sh, raw(adsb::encodeAirbornePosition(icao, 5, 11, 20000, false, true, slat, slon), 1.5));
    ac = get(sh, icao, 1.5); a = &ac;
    CHECK(a->hasPos && std::fabs(a->lat - slat) < 1e-3 && std::fabs(std::remainder(a->lon - slon, 360.0)) < 2e-3, "south / date line: %.4f %.4f", a->lat, a->lon);
}

static void testSurface() {
    AdsbAircraft ac;
    const AdsbAircraft* a = nullptr;
    AdsbTracker tr;
    const uint32_t icao = 0x4B1234;
    tr.setReference(25.25, 55.36);
    const double lat = 25.2532, lon = 55.3657;
    feed(tr, raw(adsb::encodeAllCall(icao, 4, 0), 0.5));
    feed(tr, raw(adsb::encodeSurfacePosition(icao, 4, 7, 17.0, 92.8, false, lat, lon), 1.0));
    feed(tr, raw(adsb::encodeSurfacePosition(icao, 4, 7, 17.0, 92.8, true, lat, lon), 2.0));
    ac = get(tr, icao, 2.0); a = &ac;
    CHECK(a && a->ground && a->hasPos && std::fabs(a->lat - lat) < 1e-4 && std::fabs(a->lon - lon) < 1e-4, "surface position %.5f %.5f", a ? a->lat : 0, a ? a->lon : 0);
    CHECK(a && a->hasSpeed && std::fabs(a->speedKt - 17.0) < 1e-9 && a->hasHeading && std::fabs(a->headingDeg - 92.8125) < 1e-3, "surface speed and track");
    // without a reference a surface aircraft has no position (the zones repeat every 90 degrees)
    AdsbTracker none;
    feed(none, raw(adsb::encodeAllCall(icao, 4, 0), 0.5));
    feed(none, raw(adsb::encodeSurfacePosition(icao, 4, 7, 17.0, 92.8, false, lat, lon), 1.0));
    feed(none, raw(adsb::encodeSurfacePosition(icao, 4, 7, 17.0, 92.8, true, lat, lon), 2.0));
    ac = get(none, icao, 2.0); a = &ac;
    CHECK(a && a->ground && !a->hasPos, "a surface position without any reference");
    // the aircraft's own last fix serves as the reference once it has taken off and landed
    AdsbTracker own;
    feed(own, raw(adsb::encodeAirbornePosition(icao, 5, 11, 1000, false, false, 25.30, 55.40), 1.0));
    feed(own, raw(adsb::encodeAirbornePosition(icao, 5, 11, 1000, false, true, 25.30, 55.40), 1.5));
    feed(own, raw(adsb::encodeSurfacePosition(icao, 4, 7, 20.0, 10.0, false, 25.3002, 55.4003), 2.0));
    ac = get(own, icao, 2.0); a = &ac;
    CHECK(a->ground && std::fabs(a->lat - 25.3002) < 1e-3 && std::fabs(a->lon - 55.4003) < 1e-3, "surface position near the last fix: %.5f %.5f", a->lat, a->lon);
}

static void testStatus() {
    AdsbAircraft ac;
    const AdsbAircraft* a = nullptr;
    AdsbTracker tr;
    const uint32_t icao = 0x111111;
    feed(tr, raw(adsb::encodeAllCall(icao, 5, 0), 1.0));
    feed(tr, raw(adsb::encodeIdentityReply(icao, 0, 7700), 1.1));
    AdsbTelemetry t = snap(tr, 1.1);
    a = find(t, icao);
    CHECK(a->hasSquawk && a->squawk == 7700 && a->emergency == 1, "squawk 7700 gives emergency %d", a->emergency);
    // a reply that changes the squawk is believed when it comes twice (its address is only in the parity)
    feed(tr, raw(adsb::encodeIdentityReply(icao, 0, 1200), 2.0));
    ac = get(tr, icao, 2.0); a = &ac;
    CHECK(a->squawk == 7700 && a->emergency == 1, "a squawk change from one reply was taken at once (%d)", a->squawk);
    feed(tr, raw(adsb::encodeIdentityReply(icao, 0, 1200), 2.2));
    ac = get(tr, icao, 2.2); a = &ac;
    CHECK(a->squawk == 1200 && a->emergency == 0, "back to normal");
    feed(tr, raw(adsb::encodeIdentityReply(icao, 0, 2200), 2.4));
    feed(tr, raw(adsb::encodeIdentityReply(icao, 0, 3300), 2.6));    // a different one: the first is forgotten
    feed(tr, raw(adsb::encodeIdentityReply(icao, 0, 2200), 2.8));
    ac = get(tr, icao, 2.8); a = &ac;
    CHECK(a->squawk == 1200, "squawk %d after changes that were not confirmed", a->squawk);
    feed(tr, raw(adsb::encodeIdentityReply(icao, 0, 2200), 2.9));
    ac = get(tr, icao, 2.9); a = &ac;
    CHECK(a->squawk == 2200, "squawk %d after two equal replies", a->squawk);
    feed(tr, raw(adsb::encodeAircraftStatus(icao, 5, 5, 7500), 3.0));   // DF17 TC 28: unlawful interference
    ac = get(tr, icao, 3.0); a = &ac;
    CHECK(a->emergency == 5 && a->squawk == 7500, "TC 28 emergency %d squawk %d", a->emergency, a->squawk);
    feed(tr, raw(adsb::encodeAircraftStatus(icao, 5, 0, 1200), 4.0));
    ac = get(tr, icao, 4.0); a = &ac;
    CHECK(a->emergency == 0, "emergency cleared by TC 28 state 0 (%d)", a->emergency);
    // flight status: alert and ground
    feed(tr, raw(adsb::encodeAltitudeReply(icao, 3, 0, false), 5.0));
    ac = get(tr, icao, 5.0); a = &ac;
    CHECK(a->ground && a->alert, "flight status 3: ground %d alert %d", a->ground, a->alert);
    // Comm-B: callsign, selected altitude, track and turn, heading and speed
    uint8_t mb[7];
    adsb::mbCallsign(mb, "UAE412");
    feed(tr, raw(adsb::encodeCommB(icao, false, 0, 30000, mb), 6.0));
    ac = get(tr, icao, 6.0); a = &ac;
    CHECK(a->callsign == "UAE412" && a->hasAlt && a->altFt == 30000 && !a->ground, "BDS 2,0 callsign '%s'", a->callsign.c_str());
    adsb::mbSelectedVertical(mb, 35008, 35008, 1013.2);
    feed(tr, raw(adsb::encodeCommB(icao, false, 0, 30000, mb), 7.0));
    ac = get(tr, icao, 7.0); a = &ac;
    CHECK(a->hasSelAlt && a->selAltFt == 35008 && a->hasBaroSet && std::fabs(a->baroSetMb - 1013.2) < 0.06, "BDS 4,0 selected altitude %d", a->selAltFt);
    adsb::mbHeadingAndSpeed(mb, 123.0, 280, 0.78, -640, -640);
    feed(tr, raw(adsb::encodeCommB(icao, true, 0, 1200, mb), 8.0));
    ac = get(tr, icao, 8.0); a = &ac;
    CHECK(a->hasIas && a->iasKt == 280 && a->hasMach && std::fabs(a->mach - 0.78) < 0.005 && a->hasVrate && a->vrateFpm == -640, "BDS 6,0 ias %d mach %.3f", a->iasKt, a->mach);
    // target state and operational status
    feed(tr, raw(adsb::encodeTargetState(icao, 5, 36000, 1013.6, 90.0, 9), 9.0));
    feed(tr, raw(adsb::encodeOperationalStatus(icao, 5, 2, 10, 3), 9.5));
    ac = get(tr, icao, 9.5); a = &ac;
    CHECK(a->hasSelAlt && a->selAltFt == 36000 && a->hasSelHdg && std::fabs(a->selHdgDeg - 90.0) < 0.5 && a->adsbVersion == 2 && a->nacp == 10, "TC 29 / 31: sel alt %d hdg %.1f version %d nacp %d", a->selAltFt, a->selHdgDeg, a->adsbVersion, a->nacp);
    // GNSS height
    feed(tr, raw(adsb::encodeGnssPosition(icao, 5, 20, 35025, false, 25.0, 55.0), 10.0));
    ac = get(tr, icao, 10.0); a = &ac;
    CHECK(a->hasGeoAlt && a->geoAltFt == 35025 && a->altFt == 30000, "GNSS height %d ft, barometric %d", a->geoAltFt, a->altFt);
    // TIS-B with a non-ICAO address is kept apart from an ICAO address of the same value
    AdsbTracker tb;
    adsb::Frame es = adsb::encodeIdentification(0x222222, 5, 4, 3, "TISB1");
    feed(tb, raw(adsb::encodeTisb(es, 1), 1.0));
    feed(tb, raw(es, 1.5));
    AdsbTelemetry tt = snap(tb, 2.0);
    CHECK(tt.aircraftCount == 2, "TIS-B and ICAO aircraft with the same number: %u rows", tt.aircraftCount);
    // Gillham altitude (Q = 0) and all categories of the message list
    AdsbTracker g;
    feed(g, raw(adsb::encodeAirbornePosition(0x333333, 5, 11, 35000, true, false, 40.0, 10.0), 1.0));
    ac = get(g, 0x333333, 1.0); a = &ac;
    CHECK(a->hasAlt && a->altFt == 35000, "Gillham altitude %d", a->altFt);
}

static void testExpiryAndLimits() {
    AdsbTracker tr;
    feed(tr, raw(adsb::encodeAllCall(0x400001, 5, 0), 1.0));
    feed(tr, raw(adsb::encodeAllCall(0x400002, 5, 0), 30.0));
    tr.tick(50.0);
    CHECK(tr.aircraftCount() == 2, "expired early");
    tr.tick(61.5);
    CHECK(tr.aircraftCount() == 1, "the aircraft silent for 60 s is still in the table (%zu)", tr.aircraftCount());
    tr.tick(91.0);
    CHECK(tr.aircraftCount() == 0, "table not empty");
    // a flood of addresses: the table and the report stay bounded
    AdsbTracker many;
    for (uint32_t i = 0; i < 3000; i++) feed(many, raw(adsb::encodeAllCall(0x500000 + i, 5, 0), 1.0 + i * 0.001));
    CHECK(many.aircraftCount() <= 2000, "table grew to %zu", many.aircraftCount());
    AdsbTelemetry t = snap(many, 5.0);
    CHECK(t.aircraft.size() <= 128 && t.frames.size() <= 64, "report: %zu aircraft, %zu frames", t.aircraft.size(), t.frames.size());
    CHECK(t.aircraft.size() == 128 && t.aircraft[0].ageSec <= t.aircraft[1].ageSec + 1e-6f, "the report lists the most recent first");
    // size of the full report: 128 aircraft with the longest callsign and a full track each, and the frame list (about 100 kB is the limit)
    AdsbTracker full;
    for (uint32_t i = 0; i < 200; i++) {
        const uint32_t a = 0x500000 + i;
        feed(full, raw(adsb::encodeIdentification(a, 5, 4, 3, "ABCD1234"), 1.0));
        for (int k = 0; k < 40; k++) {
            feed(full, raw(adsb::encodeAirbornePosition(a, 5, 11, 30000, false, false, 25.0 + 0.001 * k, 55.0), 2.0 + 6.0 * k));
            feed(full, raw(adsb::encodeAirbornePosition(a, 5, 11, 30000, false, true, 25.0 + 0.001 * k, 55.0), 2.1 + 6.0 * k));
        }
    }
    AdsbTelemetry big = snap(full, 250.0);
    size_t bytes = sizeof(AdsbTelemetry) + big.frames.size() * sizeof(AdsbFrameInfo);
    for (auto& f : big.frames) bytes += f.hex.capacity() + f.what.capacity();
    for (auto& a : big.aircraft) bytes += sizeof(AdsbAircraft) + a.callsign.capacity() + a.category.capacity() + a.track.capacity() * sizeof(AdsbTrackPoint);
    printf("  a full report is about %zu bytes (%zu aircraft, longest track %zu points)\n", bytes, big.aircraft.size(), big.aircraft.empty() ? 0 : big.aircraft[0].track.size());
    CHECK(bytes < 100000 && big.aircraft.size() == 128 && big.aircraft[0].track.size() == 30, "report size %zu", bytes);
    // messages per second over the last second
    AdsbTracker rate;
    for (int w = 0; w < 8; w++) {
        for (int k = 0; k < 10; k++) feed(rate, raw(adsb::encodeAllCall(0x400003, 5, 0), w * 0.25 + k * 0.01));
        rate.closeWindow();
    }
    CHECK(std::fabs(snap(rate, 2.0).msgsPerSec - 40.0) < 0.5, "messages per second %.1f (expected 40)", snap(rate, 2.0).msgsPerSec);
    // reset empties it all but keeps the reference
    rate.setReference(1, 2);
    rate.reset();
    CHECK(rate.aircraftCount() == 0 && rate.good() == 0 && rate.refValid(), "reset");
}

int main() {
    testPublishedMessages();
    testChecks();
    testPlausibility();
    testPositions();
    testSurface();
    testStatus();
    testExpiryAndLimits();
    printf(fails ? "adsb_track: %d FAILED\n" : "adsb_track: all passed\n", fails);
    return fails ? 1 : 0;
}
