// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Contact graph: the message parser (FT8 / FT4 / JS8 / WSPR formats, hashed and compound calls), graph building, completed contacts,
// expiry, station positions (locator, prefix estimate) and the great-circle sampling
#include "dect2/map/contact_graph.h"
#include "dect2/geo/maidenhead.h"
#include <cmath>
#include <cstdio>
#include <string>

using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main() {
    // ---- parser
    ContactMsg m = parseContactMessage("CQ K1ABC FN42");
    CHECK(m.valid && m.cq && m.from == "K1ABC" && m.grid == "FN42" && m.cqTag.empty(), "CQ");
    m = parseContactMessage("CQ DX K1ABC FN42");
    CHECK(m.valid && m.cq && m.cqTag == "DX" && m.from == "K1ABC" && m.grid == "FN42", "CQ DX");
    m = parseContactMessage("CQ POTA W9XYZ EN37");
    CHECK(m.valid && m.cq && m.cqTag == "POTA" && m.from == "W9XYZ", "CQ POTA");
    m = parseContactMessage("CQ 123 LY2ABC KO24");
    CHECK(m.valid && m.cqTag == "123" && m.from == "LY2ABC", "CQ number");
    m = parseContactMessage("CQ K1ABC/P FN42");
    CHECK(m.valid && m.from == "K1ABC/P" && m.grid == "FN42", "CQ /P");
    m = parseContactMessage("K1ABC W9XYZ EN37");
    CHECK(m.valid && !m.cq && m.to == "K1ABC" && m.from == "W9XYZ" && m.grid == "EN37", "grid reply: to %s from %s", m.to.c_str(), m.from.c_str());
    m = parseContactMessage("W9XYZ K1ABC -12");
    CHECK(m.valid && m.to == "W9XYZ" && m.from == "K1ABC" && m.report == "-12" && !m.complete, "report");
    m = parseContactMessage("K1ABC W9XYZ R-05");
    CHECK(m.valid && m.report == "R-05" && !m.complete, "R report");
    m = parseContactMessage("W9XYZ K1ABC RRR");
    CHECK(m.valid && m.complete && !m.final73, "RRR");
    m = parseContactMessage("K1ABC W9XYZ RR73");
    CHECK(m.valid && m.complete && m.final73 && m.grid.empty(), "RR73 is not a locator");
    m = parseContactMessage("W9XYZ K1ABC 73");
    CHECK(m.valid && m.complete && m.final73 && m.from == "K1ABC", "73");
    m = parseContactMessage("<PJ4/K1ABC> W9XYZ +03");
    CHECK(m.valid && m.to == "PJ4/K1ABC" && m.from == "W9XYZ", "bracketed compound: %s", m.to.c_str());
    m = parseContactMessage("<...> W9XYZ RR73");
    CHECK(m.valid && m.to.empty() && m.from == "W9XYZ" && m.complete, "hashed unknown");
    m = parseContactMessage("<...> <...> RR73");
    CHECK(!m.valid, "two hashed calls name nobody");
    m = parseContactMessage("K1ABC/P W9XYZ/R -07");
    CHECK(m.valid && m.to == "K1ABC/P" && m.from == "W9XYZ/R", "/P /R");
    m = parseContactMessage("PJ4/K1ABC W9XYZ KO24");
    CHECK(m.valid && m.to == "PJ4/K1ABC", "prefix compound");
    m = parseContactMessage("cq dx k1abc fn42");
    CHECK(m.valid && m.from == "K1ABC", "lower case");
    m = parseContactMessage("TNX FOR QSO 73");
    CHECK(!m.valid, "free text");
    m = parseContactMessage("HELLO");
    CHECK(!m.valid, "one word");
    m = parseContactMessage("");
    CHECK(!m.valid, "empty");
    m = parseContactMessage("LY2ABC KO24 37");   // WSPR
    CHECK(m.valid && m.from == "LY2ABC" && m.grid == "KO24" && m.cq && m.to.empty(), "WSPR");
    m = parseContactMessage("LY2ABC: OK1XX SNR -12");   // JS8Call
    CHECK(m.valid && m.from == "LY2ABC" && m.to == "OK1XX" && !m.cq, "JS8 directed");
    m = parseContactMessage("LY2ABC: @HB HEARTBEAT KO24");
    CHECK(m.valid && m.cq && m.grid == "KO24", "JS8 heartbeat");
    m = parseContactMessage("LY2ABC: @ALLCALL HELLO");
    CHECK(!m.valid || m.to.empty(), "JS8 group");
    m = parseContactMessage("QRZ K1ABC");
    CHECK(m.valid && m.cq && m.from == "K1ABC", "QRZ");
    CHECK(baseCallsign("PJ4/K1ABC/P") == "K1ABC" && baseCallsign("K1ABC/P") == "K1ABC" && baseCallsign("w9xyz") == "W9XYZ", "base call");

    // ---- prefix positions
    double la = 0, lo = 0;
    CHECK(callsignPrefixPosition("LY2ABC", la, lo) && std::fabs(la - 55.2) < 1 && std::fabs(lo - 23.9) < 1, "LY");
    CHECK(callsignPrefixPosition("W9XYZ", la, lo) && lo < -50 && la > 25, "W is the US");
    CHECK(callsignPrefixPosition("DL1ABC", la, lo) && std::fabs(la - 51.2) < 1, "DL");
    CHECK(callsignPrefixPosition("DK1ABC", la, lo) && std::fabs(la - 51.2) < 1, "DK is Germany");
    CHECK(callsignPrefixPosition("KH6XX", la, lo) && la < 25 && lo < -150, "Hawaii beats the US");
    CHECK(callsignPrefixPosition("M0ABC", la, lo) && std::fabs(lo + 1.5) < 2, "M is England");
    CHECK(!callsignPrefixPosition("QQ9ZZ", la, lo), "unknown prefix");

    // ---- graph
    ContactGraph g(600);
    CHECK(g.add(1000, "CQ K1ABC FN42"), "cq added");
    CHECK(g.edgeCount() == 0, "a CQ draws no line");
    ContactStation s;
    CHECK(g.station("K1ABC", s) && s.known && !s.estimated && s.grid == "FN42", "CQ gives the position");
    double ela, elo;
    fromMaidenhead("FN42", ela, elo);
    CHECK(std::fabs(s.lat - ela) < 1e-6 && std::fabs(s.lon - elo) < 1e-6, "position is the square middle");
    g.add(1015, "K1ABC LY2ABC KO24", 0, 14074000);
    CHECK(g.edgeCount() == 1, "reply makes a line");
    CHECK(g.station("LY2ABC", s) && s.known && !s.estimated && s.grid == "KO24", "reply locator");
    g.add(1030, "LY2ABC K1ABC -10");
    g.add(1045, "K1ABC LY2ABC R-12");
    CHECK(g.edgeCount() == 1, "both directions are one line");
    auto es = g.edges();
    CHECK(es.size() == 1 && es[0].count == 3 && !es[0].complete && es[0].from == "LY2ABC" && es[0].to == "K1ABC" && es[0].first == 1015 && es[0].last == 1045, "edge fields");
    g.add(1060, "LY2ABC K1ABC RR73");
    es = g.edges();
    CHECK(es[0].complete && es[0].count == 4, "completed");
    g.add(1075, "K1ABC LY2ABC 73");
    CHECK(g.edges()[0].complete, "stays completed");
    // a station without a locator is placed by its prefix
    g.add(1100, "W9XYZ OK1XX -03");
    CHECK(g.station("OK1XX", s) && s.known && s.estimated && s.grid.empty(), "estimated by prefix");
    g.add(1110, "W9XYZ OK1XX JO70");
    CHECK(g.station("OK1XX", s) && !s.estimated && s.grid == "JO70", "locator replaces the estimate");
    // a hashed call makes no line but the sender is a station
    size_t n = g.edgeCount();
    g.add(1120, "<...> DL1ABC RR73");
    CHECK(g.edgeCount() == n && g.station("DL1ABC", s), "hashed: station only");
    // compound calls join their base call
    g.add(1130, "K1ABC/P LY2ABC/P -01");
    CHECK(g.edgeCount() == n, "/P is the same station: %zu", g.edgeCount());
    // the decoder's own locator for the sender
    g.add(1140, "OK1XX SP5ABC +05", 1, 0, "KO02");
    CHECK(g.station("SP5ABC", s) && s.grid == "KO02", "sender grid from the decoder");
    // a new QSO after a finished one starts again
    g.add(1300, "LY2ABC K1ABC KO24");
    es = g.edges();
    bool found = false;
    for (const auto& e : es) if ((e.from == "K1ABC" || e.to == "K1ABC") && (e.from == "LY2ABC" || e.to == "LY2ABC")) { found = true; CHECK(!e.complete && e.count == 1, "contact restarted"); }
    CHECK(found, "pair kept");
    CHECK(!g.add(1310, "NOTHING HERE"), "free text ignored");

    // ---- expiry
    ContactGraph x(300);
    x.add(1000, "K1ABC LY2ABC KO24");
    x.add(1200, "W9XYZ OK1XX JO70");
    x.expire(1250);
    CHECK(x.edgeCount() == 2, "nothing old yet");
    x.expire(1400);
    CHECK(x.edgeCount() == 1 && x.edges()[0].from == "OK1XX", "the older line dropped: %zu", x.edgeCount());
    ContactStation t;
    CHECK(!x.station("K1ABC", t) && x.station("W9XYZ", t), "stations of the dropped line gone, the others stay");
    x.expire(2000);
    CHECK(x.edgeCount() == 0 && x.stations().empty(), "all gone");

    // ---- great circle
    auto arc = greatCircleArc(54.7, 25.3, 40.7, -74.0, 5);
    CHECK(arc.size() >= 4 && std::fabs(arc.front().first - 54.7) < 1e-9 && std::fabs(arc.front().second - 25.3) < 1e-9, "arc start");
    CHECK(std::fabs(arc.back().first - 40.7) < 1e-9 && std::fabs(std::remainder(arc.back().second + 74.0, 360)) < 1e-9, "arc end");
    double km, brg;
    greatCircle(54.7, 25.3, 40.7, -74.0, km, brg);
    double sum = 0;
    for (size_t i = 1; i < arc.size(); i++) { double k, b; greatCircle(arc[i - 1].first, arc[i - 1].second, arc[i].first, arc[i].second, k, b); sum += k; }
    CHECK(std::fabs(sum - km) < 5, "arc length %.1f against %.1f km", sum, km);
    double maxLat = -90;
    for (auto& p : arc) maxLat = std::max(maxLat, p.first);
    CHECK(maxLat > 54.7, "the arc bulges to the north: %.1f", maxLat);
    for (size_t i = 1; i < arc.size(); i++) CHECK(std::fabs(arc[i].second - arc[i - 1].second) < 90, "longitude continuous at %zu", i);
    auto across = greatCircleArc(35, 170, 35, -170, 3);   // over the date line
    bool jump = false;
    for (size_t i = 1; i < across.size(); i++) jump |= std::fabs(across[i].second - across[i - 1].second) > 90;
    CHECK(!jump && across.back().second > 180, "date line: no jump, ends at %.1f", across.back().second);
    auto same = greatCircleArc(10, 20, 10, 20);
    CHECK(same.size() == 2, "same point: 2 points");
    auto longest = greatCircleArc(0, 0, 0, 179.9, 0.01);
    CHECK(longest.size() <= 400, "capped");

    if (fails) printf("%d failure(s)\n", fails);
    else printf("contact_graph: all passed\n");
    return fails ? 1 : 0;
}
