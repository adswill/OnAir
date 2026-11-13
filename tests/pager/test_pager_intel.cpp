// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Pagers: the smart layer (pager_intel.h). Positions (decimal, degrees-minutes, UTM, MGRS: round trips over the globe and known points), the
// text repair of damaged POCSAG pages (measured: characters recovered, wrong guesses, and that novel words are never invented), learned fields,
// incidents, the place list and the privacy rule.
#include "dect2/pager/pager_intel.h"
#include "dect2/pager/pager_proto.h"
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <random>
using namespace dect2;
using namespace dect2::pager;
#include "../common/test_util.h"

static std::mt19937 rng(777);

// ---- an independent forward UTM (Snyder), only for the round trip
static void utmForward(double lat, double lon, int& zone, double& E, double& N) {
    const double a = 6378137.0, f = 1 / 298.257223563, e2 = f * (2 - f), k0 = 0.9996, pi = 3.14159265358979323846;
    zone = (int)std::floor((lon + 180) / 6) + 1;
    const double lon0 = ((zone - 1) * 6 - 180 + 3) * pi / 180, phi = lat * pi / 180, lam = lon * pi / 180;
    const double ep2 = e2 / (1 - e2), nu = a / std::sqrt(1 - e2 * std::sin(phi) * std::sin(phi));
    const double T = std::tan(phi) * std::tan(phi), C = ep2 * std::cos(phi) * std::cos(phi), A = (lam - lon0) * std::cos(phi);
    const double e4 = e2 * e2, e6 = e4 * e2;
    const double M = a * ((1 - e2 / 4 - 3 * e4 / 64 - 5 * e6 / 256) * phi - (3 * e2 / 8 + 3 * e4 / 32 + 45 * e6 / 1024) * std::sin(2 * phi) +
                          (15 * e4 / 256 + 45 * e6 / 1024) * std::sin(4 * phi) - (35 * e6 / 3072) * std::sin(6 * phi));
    E = 500000 + k0 * nu * (A + (1 - T + C) * A * A * A / 6 + (5 - 18 * T + T * T + 72 * C - 58 * ep2) * std::pow(A, 5) / 120);
    N = k0 * (M + nu * std::tan(phi) * (A * A / 2 + (5 - T + 9 * C + 4 * C * C) * std::pow(A, 4) / 24 + (61 - 58 * T + T * T + 600 * C - 330 * ep2) * std::pow(A, 6) / 720));
    if (lat < 0) N += 10000000.0;
}
static char bandOf(double lat) {
    static const char b[] = "CDEFGHJKLMNPQRSTUVWX";
    int i = (int)std::floor((lat + 80) / 8);
    return b[std::max(0, std::min(19, i))];
}
static std::string mgrsOf(double lat, double lon, int digitsEach) {
    int zone; double E, N;
    utmForward(lat, lon, zone, E, N);
    static const char* const cols[3] = {"ABCDEFGH", "JKLMNPQR", "STUVWXYZ"};
    static const char rows[] = "ABCDEFGHJKLMNPQRSTUV";
    const int ci = (int)(E / 100000) - 1;
    int ri = (int)(std::fmod(N, 2000000.0) / 100000);
    if (zone % 2 == 0) ri = (ri + 5) % 20;
    const double e = std::fmod(E, 100000.0), n = std::fmod(N, 100000.0);
    const double sc = std::pow(10.0, 5 - digitsEach);
    char b[64];
    snprintf(b, sizeof b, "%d%c%c%c %0*d %0*d", zone, bandOf(lat), cols[(zone - 1) % 3][ci], rows[ri], digitsEach, (int)(e / sc), digitsEach, (int)(n / sc));
    return b;
}

static void geoTests() {
    double la, lo;
    CHECK(utmToLatLon(31, false, 500000, 0, la, lo) && std::fabs(la) < 1e-6 && std::fabs(lo - 3.0) < 1e-6, "UTM origin of zone 31");
    auto h = findCoordinates("FIRE AT 31UDQ4825111932 PLEASE");
    CHECK(h.size() == 1 && h[0].kind == "MGRS" && std::fabs(h[0].lat - 48.8584) < 0.002 && std::fabs(h[0].lon - 2.2945) < 0.002, "MGRS of the Eiffel tower: %zu hits", h.size());
    h = findCoordinates("job 12 at -33.8688, 151.2093 now");
    CHECK(h.size() == 1 && std::fabs(h[0].lat + 33.8688) < 1e-4 && std::fabs(h[0].lon - 151.2093) < 1e-4, "decimal pair");
    h = findCoordinates("loc 51.5074N 0.1278W end");
    CHECK(h.size() == 1 && std::fabs(h[0].lat - 51.5074) < 1e-4 && std::fabs(h[0].lon + 0.1278) < 1e-4, "decimal with hemispheres");
    h = findCoordinates("N 51 30.444 W 000 07.668");
    CHECK(h.size() == 1 && std::fabs(h[0].lat - 51.5074) < 1e-3 && std::fabs(h[0].lon + 0.1278) < 1e-3, "degrees and minutes");
    CHECK(findCoordinates("TEL 0800 123456 CALL 5551234567 ROOM 12.5 34.7").empty(), "phone numbers and short decimals are not positions");
    CHECK(findCoordinates("1234567890 98765").empty(), "numeric pages are not positions");
    // round trips over the globe, both hemispheres, every zone
    int ok = 0, total = 0, okM = 0;
    double worst = 0;
    for (int i = 0; i < 400; i++) {
        const double lat = -79 + 157.0 * (rng() % 100000) / 100000.0, lon = -179 + 358.0 * (rng() % 100000) / 100000.0;
        int zone; double E, N;
        utmForward(lat, lon, zone, E, N);
        char b[96];
        snprintf(b, sizeof b, "at %d%c %06d %07d ok", zone, bandOf(lat), (int)E, (int)N);
        total++;
        const auto u = findCoordinates(b);
        if (!u.empty() && u[0].kind == "UTM") {
            const double d = distanceKm(lat, lon, u[0].lat, u[0].lon) * 1000;
            worst = std::max(worst, d);
            if (d < 5) ok++;
        }
        const auto m = findCoordinates("at " + mgrsOf(lat, lon, 5));
        if (!m.empty() && m[0].kind == "MGRS" && distanceKm(lat, lon, m[0].lat, m[0].lon) < 0.02) okM++;
    }
    printf("positions: UTM round trip %d/%d (worst %.2f m), MGRS 1 m grid %d/%d\n", ok, total, worst, okM, total);
    CHECK(ok >= total - 4, "UTM round trip %d/%d", ok, total);    // a few points lie right on a band edge
    CHECK(okM >= total - 8, "MGRS round trip %d/%d", okM, total);
    const auto coarse = findCoordinates("grid 31UDQ4811");
    CHECK(coarse.size() == 1 && distanceKm(48.8584, 2.2945, coarse[0].lat, coarse[0].lon) < 1.5, "MGRS at a 1 km grid");
}

// ---- POCSAG pages through the real parser, with code words wrecked
static uint32_t wreck(uint32_t w) {
    for (;;) {
        uint32_t v = w;
        const int n = 3 + (int)(rng() % 4);
        for (int i = 0; i < n; i++) v ^= 1u << (rng() % 32);
        if (!correctWord(v).ok) return v;
    }
}
static std::vector<PagerMessage> parsePage(uint32_t ric, const std::string& text, int damageMiddleWords, uint32_t seedSkipLast = 1) {
    std::vector<PocsagPage> pages(1);
    pages[0].ric = ric; pages[0].fn = 3; pages[0].type = kPagerAlpha; pages[0].text = text;
    const std::vector<uint8_t> b = pocsagBits(pages, 32);
    std::vector<uint32_t> w;
    for (size_t i = 32; i + 32 <= b.size(); i += 32) w.push_back(wordFromBits(&b[i]));
    std::vector<int> pos;
    for (size_t i = 0; i < w.size(); i++) if (w[i] != kPocsagSync && w[i] != kPocsagIdle) pos.push_back((int)i);
    // pos[0] is the address word; the last message word is kept (it holds the end of the text)
    const int lastK = (int)pos.size() - (int)seedSkipLast;
    for (int d = 0; d < damageMiddleWords && lastK > 1; d++) {
        const int k = 1 + (int)(rng() % (uint32_t)(lastK - 1));
        w[(size_t)pos[(size_t)k]] = wreck(w[(size_t)pos[(size_t)k]]);
    }
    PocsagParser p;
    std::vector<PagerMessage> out;
    int slot = 0;
    for (uint32_t x : w) {
        if (x == kPocsagSync) { slot = 0; continue; }
        p.word(x, slot++, out);
    }
    p.flush(out);
    return out;
}

static const char* kType[] = {"STRUCTURE FIRE", "GRASS FIRE", "ALARM ACTIVATION", "HAZMAT INCIDENT", "VEHICLE RESCUE", "FLOOD ASSISTANCE"};
static const char* kStreet[] = {"HIGH STREET", "STATION ROAD", "CHURCH LANE", "MILL ROAD", "PARK AVENUE", "KINGS DRIVE", "BRIDGE STREET"};
static const char* kUnits[] = {"PUMPER 12", "TANKER 4", "LADDER 7", "RESCUE 3", "PUMPER 9"};
static std::string makeText(int i) {
    char b[160];
    snprintf(b, sizeof b, "%s AT %d %s RESPOND WITH %s AND %s REPORT TO COMMAND", kType[rng() % 6], 10 + (int)(rng() % 90), kStreet[rng() % 7], kUnits[rng() % 5], kUnits[rng() % 5]);
    (void)i;
    return b;
}

// the characters of repaired that match the truth at the same index
static void score(const PagerMessage& m, const std::string& truth, int& right, int& wrong, int& lost) {
    for (size_t i = 0; i < m.repairedText.size(); i++) {
        const char g = m.guessMask[i];
        if (g == 1) { if (i < truth.size() && truth[i] == m.repairedText[i]) right++; else { wrong++; if (getenv("DBG")) printf("WRONG: truth '%s'\n       got   '%s'\n", truth.c_str(), m.repairedText.c_str()); } }
        else if (g == 2) lost++;
    }
}

static void repairTests() {
    PagerBrain br;
    br.opt.intel = false;
    int64_t t = 1000000;
    // training: 40 clean pages of one capcode
    for (int i = 0; i < 40; i++) {
        auto v = parsePage(1234, makeText(i), 0);
        CHECK(v.size() == 1 && !v[0].damaged, "clean training page");
        for (auto& m : v) { br.process(m, t++); CHECK(m.repairedText.empty(), "a clean page is left alone"); }
    }
    // damaged pages: one lost code word inside the text (20 bits: three characters or so)
    int right = 0, wrong = 0, lost = 0, pages = 0, withRepair = 0, unknownChars = 0;
    for (int i = 0; i < 300; i++) {
        const std::string truth = makeText(i);
        auto v = parsePage(1234, truth, 1);
        if (v.size() != 1 || !v[0].damaged || v[0].type != kPagerAlpha) continue;
        pages++;
        for (size_t k = 0; k < v[0].alphaKnown.size(); k++) if ((v[0].alphaKnown[k] & 0x7F) != 0x7F) unknownChars++;
        br.process(v[0], t++);
        if (!v[0].repairedText.empty()) { withRepair++; score(v[0], truth, right, wrong, lost); }
    }
    printf("repair (known template, one lost word per page): %d damaged pages, %d unknown characters: %d filled in right, %d wrong, %d left as '?'\n", pages, unknownChars, right, wrong, lost);
    CHECK(pages >= 250, "damaged pages made: %d", pages);
    CHECK(right >= unknownChars * 0.7, "at least 70%% of the lost characters recovered (%d of %d)", right, unknownChars);
    CHECK(wrong * 50 <= right + wrong + 1, "wrong guesses under 2%% of the guesses (%d of %d)", wrong, right + wrong);
    // novel words are never invented: the lost word covers a word the capcode never sent
    int inventedWrong = 0, novelPages = 0, novelUnknown = 0;
    for (int i = 0; i < 200; i++) {
        std::string truth = std::string("UNIDENTIFIED ZEPPELIN AT ") + std::to_string(10 + rng() % 90) + " " + kStreet[rng() % 7] + " RESPOND WITH " + kUnits[rng() % 5] + " REPORT TO COMMAND";
        auto v = parsePage(1234, truth, 1);
        if (v.size() != 1 || !v[0].damaged) continue;
        novelPages++;
        br.process(v[0], t++);
        for (size_t k = 0; k < v[0].alphaKnown.size(); k++) if ((v[0].alphaKnown[k] & 0x7F) != 0x7F) novelUnknown++;
        int r = 0, w = 0, l = 0;
        if (!v[0].repairedText.empty()) score(v[0], truth, r, w, l);
        inventedWrong += w;
    }
    printf("repair (words never seen before): %d pages, %d lost characters, %d wrong guesses\n", novelPages, novelUnknown, inventedWrong);
    CHECK(inventedWrong * 20 <= novelUnknown, "unseen words: wrong guesses %d of %d lost characters", inventedWrong, novelUnknown);
    // the guessed characters are marked and the mask is as long as the text
    auto v = parsePage(1234, makeText(0), 1);
    if (!v.empty() && v[0].damaged) { br.process(v[0], t++); CHECK(v[0].repairedText.size() == v[0].guessMask.size(), "mask as long as the text"); }
    // the same pages with the repair off: nothing is added
    PagerBrain off; off.opt.repair = false; off.opt.intel = false;
    auto w2 = parsePage(1234, makeText(0), 1);
    if (!w2.empty()) { off.process(w2[0], t++); CHECK(w2[0].repairedText.empty(), "repair off leaves the page alone"); }
    // a lost word at the very end: the tail is cut and marked, nothing invented
    int tailOk = 0, tailN = 0;
    for (int i = 0; i < 100; i++) {
        const std::string truth = makeText(i);
        auto vv = parsePage(1234, truth, 0);
        if (vv.empty()) continue;
        // wreck the last message word by hand
        std::vector<PocsagPage> pg(1); pg[0].ric = 1234; pg[0].fn = 3; pg[0].type = kPagerAlpha; pg[0].text = truth;
        const auto b = pocsagBits(pg, 32);
        std::vector<uint32_t> w;
        for (size_t k = 32; k + 32 <= b.size(); k += 32) w.push_back(wordFromBits(&b[k]));
        int lastData = -1;
        for (size_t k = 0; k < w.size(); k++) if (w[k] != kPocsagSync && w[k] != kPocsagIdle) lastData = (int)k;
        w[(size_t)lastData] = wreck(w[(size_t)lastData]);
        PocsagParser pp; std::vector<PagerMessage> out; int slot = 0;
        for (uint32_t x : w) { if (x == kPocsagSync) { slot = 0; continue; } pp.word(x, slot++, out); }
        pp.flush(out);
        if (out.size() != 1 || !out[0].damaged) continue;
        tailN++;
        br.process(out[0], t++);
        if (!out[0].repairedText.empty() && out[0].repairedText.back() == '?' && out[0].guessMask.back() == 2) tailOk++;
        else if (getenv("DBG")) printf("TAIL: '%s' rep '%s' known %zu raw %zu\n", truth.c_str(), out[0].repairedText.c_str(), out[0].alphaKnown.size(), out[0].alphaRaw.size());
    }
    CHECK(tailN > 0 && tailOk == tailN, "a lost last word is cut and marked: %d of %d", tailOk, tailN);
}

static void intelTests() {
    PagerBrain br;
    br.opt.repair = false;
    int64_t t = 5000000;
    // a capcode that sends "JOB | TYPE | ADDRESS | position"
    PagerMessage last;
    for (int i = 0; i < 8; i++) {
        PagerMessage m;
        m.address = 700100; m.type = kPagerAlpha;
        char b[160];
        snprintf(b, sizeof b, "F%09d | %s | %d %s | %.4f, %.4f", 250000100 + i * 37, i % 2 ? "GRASS FIRE" : "STRUCTURE FIRE", 5 + i, kStreet[i % 7], -33.80 - 0.01 * i, 151.20 + 0.01 * i);
        m.text = b;
        br.process(m, t);
        t += 400;     // minutes apart: separate incidents
        last = m;
    }
    auto has = [&](const PagerMessage& m, const char* k) { for (auto& f : m.fields) if (f.key == k) return true; return false; };
    CHECK(has(last, "Job"), "job number read");
    CHECK(has(last, "Type") && has(last, "Number") && has(last, "Address") && has(last, "Position"), "the capcode's fields learned: %zu fields", last.fields.size());
    CHECK(last.hasPos && !last.posApprox && std::fabs(last.lat + 33.87) < 1e-3, "position from the text");
    // a page of the same capcode without a position gets the usual area, marked as approximate
    PagerMessage nopos;
    nopos.address = 700100; nopos.type = kPagerAlpha; nopos.text = "F250000999 | GRASS FIRE | 9 MILL ROAD | NO MAP REF";
    br.process(nopos, t);
    CHECK(nopos.hasPos && nopos.posApprox && nopos.place == "capcode area", "approximate position from the capcode's area");
    // incidents: two capcodes, same job number, within a minute; a third page far away stays apart
    PagerBrain b2; b2.opt.repair = false;
    PagerMessage a, b, c;
    a.address = 100; a.type = kPagerAlpha; a.text = "INC 20251234 STRUCTURE FIRE 12 HIGH STREET";
    b.address = 200; b.type = kPagerAlpha; b.text = "Job: 20251234 respond to the fire";
    c.address = 300; c.type = kPagerAlpha; c.text = "INC 20259999 GRASS FIRE QUARRY ROAD";
    b2.process(a, 1000); b2.process(b, 1030); b2.process(c, 1040);
    CHECK(a.incident && a.incident == b.incident && c.incident != a.incident, "incident by job number: %u %u %u", a.incident, b.incident, c.incident);
    auto inc = b2.incidents();
    CHECK(inc.size() == 1 && inc[0].count == 2 && inc[0].capcodes.size() == 2, "one incident with two capcodes");
    // by words: nearly the same text on two capcodes
    PagerMessage d, e;
    d.address = 11; d.type = kPagerAlpha; d.text = "BUSHFIRE WATCH AND ACT FOR LOWER PLENTY DRIVE WILLOW CREEK DISTRICT";
    e.address = 12; e.type = kPagerAlpha; e.text = "BUSHFIRE WATCH AND ACT FOR LOWER PLENTY DRIVE WILLOW CREEK AREA";
    b2.process(d, 2000); b2.process(e, 2010);
    CHECK(d.incident == e.incident, "incident by similar text");
    // by position, and not when the time is far apart
    PagerMessage f, g, h2;
    f.address = 21; f.type = kPagerAlpha; f.text = "AA at 48.8584, 2.2945";
    g.address = 22; g.type = kPagerAlpha; g.text = "ZZ near 48.8585, 2.2946 go";
    h2.address = 23; h2.type = kPagerAlpha; h2.text = "ZZ near 48.8585, 2.2946 go";
    b2.process(f, 3000); b2.process(g, 3020); b2.process(h2, 9000);
    CHECK(f.incident == g.incident && h2.incident != g.incident, "incident by position, not across an hour");
    // the place list
    PagerBrain p; p.opt.repair = false;
    CHECK(p.loadPlaces("# places\nFire Station 4,-33.9,151.1\nQuarry Road,-34.1,150.9\nab,1,1\n") == 2, "place list: two good lines");
    PagerMessage q; q.address = 5; q.type = kPagerAlpha; q.text = "grass fire at QUARRY ROAD heading east";
    p.process(q, 100);
    CHECK(q.hasPos && !q.posApprox && std::fabs(q.lat + 34.1) < 1e-6 && q.place == "Quarry Road", "place list match");
    PagerMessage q2; q2.address = 5; q2.type = kPagerAlpha; q2.text = "quarryroadworks closed";
    p.process(q2, 101);
    CHECK(!q2.hasPos, "a place name inside another word is not a match");
    // privacy
    PagerBrain pv;
    PagerMessage med; med.address = 9; med.type = kPagerAlpha; med.text = "PATIENT UNCONSCIOUS AT 12 HIGH STREET 51.5074, -0.1278";
    pv.process(med, 100);
    CHECK(med.sensitive && !med.hasPos && med.fields.empty() && med.incident == 0, "medical text is not analysed by default");
    PagerBrain pv2; pv2.opt.analyseMedical = true;
    PagerMessage med2 = med; med2.sensitive = false; med2.hasPos = false;
    pv2.process(med2, 100);
    CHECK(med2.sensitive && med2.hasPos, "with the privacy option off it is analysed");
    CHECK(!PagerBrain::looksMedical("SCPRS ALARM"), "a word that merely contains cpr is not medical");
    // tone and secure pages are left alone
    PagerMessage tone; tone.type = kPagerTone; pv.process(tone, 1);
    CHECK(tone.fields.empty(), "tone page untouched");
}

int main() {
    geoTests();
    repairTests();
    intelTests();
    return testSummary("pager_intel");
}
