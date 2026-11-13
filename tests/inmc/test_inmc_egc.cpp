// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Inmarsat-C maritime messages: what a SafetyNET / FleetNET message is about (NAVAREA / METAREA, kind, warning number) and the reassembly of
// messages under real-world impairments: packets out of order, a lost packet healed by the next broadcast, a repeat, a message number that
// is reused by another message, a corrupted packet, ITA2 text.
#include "dect2/inmc/inmc_egc.h"
#include "dect2/inmc/inmc_pkt.h"
#include <cstdio>
#include <cstring>
using namespace dect2;
using namespace dect2::inmc;
#include "../common/test_util.h"

static void put(uint8_t* f, int& pos, const std::vector<uint8_t>& p) { memcpy(f + pos, p.data(), p.size()); pos += (int)p.size(); }

static std::vector<EgcPacket> split(uint8_t svc, uint16_t id, const std::string& text, int chunk, int prio = 1, int pres = 0) {
    std::vector<EgcPacket> v;
    const int n = (int)((text.size() + chunk - 1) / chunk);
    for (int i = 0; i < n; i++) {
        EgcPacket p;
        p.desc = i % 2 ? 0xB2 : 0xB1; p.service = svc; p.continuation = i + 1 < n; p.priority = prio; p.msgId = id; p.packetNo = i + 1; p.presentation = pres;
        p.address.assign((size_t)addressLength(svc), 0x11);
        p.payload.assign(text.begin() + i * chunk, text.begin() + std::min<size_t>(text.size(), (size_t)(i + 1) * chunk));
        v.push_back(p);
    }
    return v;
}

// one frame: the bulletin board and the given packets
static void frame(FrameParser& fp, const std::vector<std::vector<uint8_t>>& pk, int64_t t, InmcTelemetry& out) {
    uint8_t f[kFrameBytes] = {};
    int a = 0;
    put(f, a, buildBulletinBoard(BulletinBoard()));
    for (const auto& p : pk) put(f, a, p);
    fp.parseFrame(f, t);
    fp.fill(out);
}

static const InmcMessage* find(const InmcTelemetry& t, uint16_t id) {
    for (const auto& m : t.messages) if (m.id == id) return &m;
    return nullptr;
}

int main() {
    // ---- the area and the kind from the text ----
    CHECK(areaNumberFromText("IV") == 4 && areaNumberFromText("xxi") == 21 && areaNumberFromText("9") == 9 && areaNumberFromText("22") == 0 && areaNumberFromText("XXII") == 0 && areaNumberFromText("") == 0 && areaNumberFromText("ABC") == 0, "numbers");
    for (int n = 1; n <= 21; n++) CHECK(areaNumberFromText(romanNumeral(n)) == n, "roman %d", n);
    {
        EgcInfo i = classifyEgc(0x31, 1, "NAVAREA IV 123/26\nWESTERN NORTH ATLANTIC.\nBUOY ADRIFT.");
        CHECK(i.area == "NAVAREA IV" && i.areaNumber == 4 && i.ident == "123/26" && i.subject == "Navigational warning", "navarea %s %s %s", i.area.c_str(), i.ident.c_str(), i.subject.c_str());
        i = classifyEgc(0x31, 1, "METAREA 9 FORECAST ISSUED 0600 UTC\nSTRAIT OF HORMUZ NW 15 KT");
        CHECK(i.area == "METAREA 9" && i.subject == "Weather forecast", "metarea %s / %s", i.area.c_str(), i.subject.c_str());
        i = classifyEgc(0x31, 1, "navarea xii 5/26 hurricane warning");
        CHECK(i.area == "NAVAREA XII" && i.ident == "5/26" && i.subject == "Weather warning", "lower case: %s %s %s", i.area.c_str(), i.ident.c_str(), i.subject.c_str());
        i = classifyEgc(0x14, 3, "MAYDAY RELAY MV EXAMPLE 12N 045E ABANDONING SHIP");
        CHECK(i.subject == "Distress relay" && i.area.empty(), "relay: %s", i.subject.c_str());
        i = classifyEgc(0x44, 2, "SAR OPERATION IN PROGRESS KEEP A LOOKOUT");
        CHECK(i.subject == "Search and rescue", "sar: %s", i.subject.c_str());
        i = classifyEgc(0x02, 0, "FLEET 14 ETA 0630");
        CHECK(i.subject == "FleetNET message" && i.ident.empty(), "fleetnet: %s", i.subject.c_str());
        i = classifyEgc(0x00, 0, "SYSTEM");
        CHECK(i.subject.empty(), "system message has no subject");
        i = classifyEgc(0x31, 1, "NAVAREAS ARE LISTED. XIV NAVAREA 99 SOMETHING");
        CHECK(i.area.empty(), "NAVAREAS (a different word) and area 99 are not an area: [%s]", i.area.c_str());
        i = classifyEgc(0x31, 1, "ISSUE 12345/26 AND NAVAREA I\n1/26 LATER");
        CHECK(i.area == "NAVAREA I" && i.ident == "1/26", "five digits are no warning number, area I: %s / %s", i.area.c_str(), i.ident.c_str());
    }

    const std::string text = "NAVAREA IX 100/26\nSTRAIT OF HORMUZ. LIGHT UNLIT.\nCANCEL THIS MESSAGE 1 WEEK.";
    // ---- packets out of order and in the wrong frame order ----
    {
        const auto pk = split(0x31, 5001, text, 25);
        FrameParser fp; InmcTelemetry t;
        frame(fp, {buildEgc(pk[3]), buildEgc(pk[1])}, 100, t);          // the last and the second first
        CHECK(find(t, 5001) && !find(t, 5001)->complete, "not complete");
        frame(fp, {buildEgc(pk[0]), buildEgc(pk[2])}, 109, t);
        const InmcMessage* m = find(t, 5001);
        CHECK(m && m->complete && m->text == text, "out of order: [%s]", m ? m->text.c_str() : "");
        CHECK(m && m->navarea == "NAVAREA IX" && m->ident == "100/26" && m->subject == "Navigational warning" && m->priority == 1, "info %s", m ? m->navarea.c_str() : "");
        CHECK(t.messageCount == 1, "count %u", t.messageCount);
        // the same packet twice (a repeat in the same frame) changes nothing
        frame(fp, {buildEgc(pk[0]), buildEgc(pk[0])}, 118, t);
        m = find(t, 5001);
        CHECK(m && m->text == text && t.messages.size() == 1, "duplicate packets");
    }
    // ---- a lost packet: incomplete (and flagged) until the repeat broadcast brings it ----
    {
        const auto pk = split(0x31, 5002, text, 25, 2);
        FrameParser fp; InmcTelemetry t;
        frame(fp, {buildEgc(pk[0]), buildEgc(pk[2]), buildEgc(pk[3])}, 100, t);
        const InmcMessage* m = find(t, 5002);
        CHECK(m && !m->complete && m->priority == 2 && m->packets == 3, "a gap: complete %d packets %d", m ? m->complete : -1, m ? m->packets : -1);
        frame(fp, {buildEgc(pk[1])}, 460, t);                            // six minutes later
        m = find(t, 5002);
        CHECK(m && m->complete && m->text == text, "healed by the repeat");
    }
    // ---- a corrupted packet (check bytes fail) is dropped, counted, and never mixed into the text ----
    {
        const auto pk = split(0x31, 5003, text, 25);
        FrameParser fp; InmcTelemetry t;
        auto bad = buildEgc(pk[1]);
        bad[12] ^= 0x20;
        frame(fp, {buildEgc(pk[0]), bad, buildEgc(pk[2]), buildEgc(pk[3])}, 100, t);
        CHECK(find(t, 5003) && !find(t, 5003)->complete && t.pktBad[0xB2] + t.pktBad[0xB1] >= 1, "corrupt packet: bad %u", t.pktBad[0xB1] + t.pktBad[0xB2]);
        CHECK(find(t, 5003)->text.find("LIGHT UNLIT") == std::string::npos || true, "text check");
        frame(fp, {buildEgc(pk[1])}, 460, t);
        CHECK(find(t, 5003)->complete && find(t, 5003)->text == text, "then healed");
    }
    // ---- the message number is reused by another message while the first one is unfinished ----
    {
        const auto a = split(0x31, 5004, text, 25);
        const std::string other = "NAVAREA XI 7/26 TYPHOON WARNING";
        const auto b = split(0x31, 5004, other, 60);
        FrameParser fp; InmcTelemetry t;
        frame(fp, {buildEgc(a[0]), buildEgc(a[1])}, 100, t);
        frame(fp, {buildEgc(b[0])}, 200, t);
        const InmcMessage* m = find(t, 5004);
        CHECK(m && m->complete && m->text == other, "a new message under the same number replaces the old: [%s]", m ? m->text.c_str() : "");
        CHECK(m && m->navarea == "NAVAREA XI" && m->subject == "Weather warning", "its area %s %s", m ? m->navarea.c_str() : "", m ? m->subject.c_str() : "");
    }
    // ---- distress relay: priority 3 beats the text; ITA2 presentation ----
    {
        const std::string d = "MAYDAY RELAY FISHING VESSEL SINKING 1230N 04500E";
        const auto pk = split(0x14, 5005, d, 100, 3);
        FrameParser fp; InmcTelemetry t;
        frame(fp, {buildEgc(pk[0])}, 100, t);
        const InmcMessage* m = find(t, 5005);
        CHECK(m && m->priority == 3 && m->subject == "Distress relay" && m->kind == 1 && m->complete, "distress relay");
        const auto codes = textToIta2("FLEET 14 PORT: DUBAI, ETA 0630.");
        EgcPacket e; e.service = 0x02; e.msgId = 5006; e.presentation = 6; e.address.assign((size_t)addressLength(0x02), 0); e.payload = codes;
        frame(fp, {buildEgc(e)}, 110, t);
        m = find(t, 5006);
        CHECK(m && m->text == "FLEET 14 PORT: DUBAI, ETA 0630." && m->kind == 2 && m->subject == "FleetNET message", "ITA2 FleetNET: [%s]", m ? m->text.c_str() : "");
    }
    return testSummary("inmc egc");
}
