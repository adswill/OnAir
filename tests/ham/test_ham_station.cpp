// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The Ham station helpers (4.0): the mode catalog (every keyboard entry is a real preset, every slot entry a mode the engine runs), usual dial
// frequencies, the frequency memory and its text, the band plan segments and their warnings, the S-meter, the region of a country, the setup
// hints, CW speed matching, the band-hop schedule, the fake-it split.
#include "dect2/ham/ham.h"
#include "dect2/ham/ham_kbd.h"
#include "dect2/ham/ham_station.h"
#include <cmath>
#include <cstdio>
#include <string>
using namespace dect2::ham;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main() {
    // ---- the catalog ----
    CHECK(hamModeInfo("FT8") && hamModeInfo("ft8")->slotMode == 0);
    CHECK(hamModeBySlot(8) && std::string(hamModeBySlot(8)->id) == "msk144");
    CHECK(hamModeBySlot(6) && std::string(hamModeBySlot(6)->adifSub) == "Q65");
    CHECK(hamModeByKbd("olivia-16-500") && std::string(hamModeByKbd("olivia-16-500")->id) == "olivia");
    CHECK(hamModeByKbd("psk250") && std::string(hamModeByKbd("psk250")->id) == "psk31");
    CHECK(hamModeByKbd("qpsk31") && std::string(hamModeByKbd("qpsk31")->id) == "psk31");
    CHECK(hamModeByKbd("slowhell") && std::string(hamModeByKbd("slowhell")->id) == "hell");
    CHECK(hamModeByKbd("thor22") && std::string(hamModeByKbd("thor22")->id) == "thor");
    CHECK(hamModeByKbd("mt63-500s") && std::string(hamModeByKbd("mt63-500s")->id) == "mt63");
    CHECK(hamModeByKbd("cw") && std::string(hamModeByKbd("cw")->id) == "cw");
    CHECK(!hamModeInfo("nosuchmode"));
    int slot = 0, kbd = 0, screen = 0;
    for (const auto& m : hamModeCatalog()) {
        if (m.run == HamRun::Keyboard) { kbd++; CHECK(m.kbdId && kbdPresetById(m.kbdId) != nullptr); if (m.kbdId && !kbdPresetById(m.kbdId)) printf("  missing preset %s\n", m.kbdId); }
        if (m.run == HamRun::Slot) { slot++; CHECK(m.slotMode == 3 || hamModeHasQso(m.slotMode)); CHECK(std::string(hamModeName(m.slotMode)) == (m.slotMode == 3 ? "WSPR" : m.name)); }
        if (m.run == HamRun::Screen) { screen++; CHECK(m.screen && *m.screen); }
        CHECK(hamModeInfo(m.id) == &m);   // ids are unique
    }
    CHECK(slot == 10 && kbd >= 14 && screen >= 12);
    CHECK(hamModeGroups().size() == 5);   // weak signal, keyboard, image, voice, packet
    CHECK(defaultRst(*hamModeInfo("cw")) == "599" && defaultRst(*hamModeInfo("ssb")) == "59" && defaultRst(*hamModeInfo("ft8")) == "-10");

    // ---- usual dials ----
    CHECK(std::fabs(usualDialMhz("20m", "ft8", 1) - 14.074) < 1e-9);
    CHECK(std::fabs(usualDialMhz("40m", "psk31", 1) - 7.040) < 1e-9 && std::fabs(usualDialMhz("40m", "psk31", 2) - 7.070) < 1e-9);
    CHECK(std::fabs(usualDialMhz("6m", "msk144", 1) - 50.260) < 1e-9 && std::fabs(usualDialMhz("2m", "msk144", 2) - 144.150) < 1e-9);
    CHECK(std::fabs(usualDialMhz("20m", "q65", 1) - 14.074) < 1e-9);   // no own table: the band's FT8 dial (dialFor)
    CHECK(usualDialMhz("20m", "olivia", 1) == 0 && usualDialMhz("20m", "nosuch", 1) == 0);

    // ---- the frequency memory ----
    {
        FreqMemory m;
        CHECK(m.recall("20m", "ft8", 14074000) == 14074000);
        m.remember("20m", "FT8", 14075500);
        m.remember("40m", "cw", 7023500);
        m.remember("", "cw", 1);           // ignored
        m.remember("20m", "cw", -5);       // ignored
        CHECK(m.size() == 2 && m.has("20M", "ft8") && m.recall("20m", "ft8", 0) == 14075500);
        FreqMemory n;
        n.fromText(m.toText());
        CHECK(n.size() == 2 && n.recall("40m", "CW", 0) == 7023500);
        n.fromText("x=;20m|cw=abc;40m|cw=7023000;|=5;garbage");
        CHECK(n.size() == 1 && n.recall("40m", "cw", 0) == 7023000);
        n.forget("40m", "cw");
        CHECK(n.size() == 0);
    }

    // ---- band plan segments ----
    for (int r = 1; r <= 3; r++)
        for (const auto& s : bandSegments(r)) {
            CHECK(s.loHz < s.hiHz);
            CHECK(hamBandAt(0.5 * (s.loHz + s.hiHz)) != nullptr);   // every segment lies in an amateur band
        }
    CHECK(segmentAt(1, 14.030e6) && segmentAt(1, 14.030e6)->use == SegUse::Cw);
    CHECK(!segmentWarning(1, 14.030e6, "DIGI").empty() && segmentWarning(1, 14.030e6, "CW").empty());
    CHECK(segmentWarning(1, 14.0755e6, "DIGI").empty());               // FT8 on 20 m
    CHECK(!segmentWarning(1, 14.0755e6, "PHONE").empty());             // phone in the narrow-band part
    CHECK(!segmentWarning(1, 14.100e6, "DIGI").empty());               // the IBP beacons
    CHECK(segmentWarning(1, 14.200e6, "PHONE").empty() && segmentWarning(1, 7.0755e6, "DIGI").empty());
    CHECK(segmentWarning(2, 7.150e6, "PHONE").empty() && !segmentWarning(2, 7.020e6, "PHONE").empty());
    CHECK(segmentWarning(1, 144.800e6, "DIGI").empty() && segmentWarning(1, 145.500e6, "PHONE").empty());
    CHECK(!segmentWarning(1, 145.900e6, "PHONE").empty());             // satellite
    CHECK(segmentWarning(1, 27.0e6, "PHONE").empty() && !segmentAt(1, 27.0e6));   // not a band in the table: no segment warning (the band gate warns)
    CHECK(segmentWarning(1, 14.030e6, "DIGI").find("Region 1") != std::string::npos);

    // ---- the S-meter ----
    CHECK(sMeterText(-73, 14e6) == "S9" && sMeterText(-93, 145e6) == "S9");
    CHECK(sMeterText(-93, 14e6) == "S6");
    CHECK(sMeterText(-53, 14e6) == "S9+20" && sMeterText(-33, 14e6) == "S9+40");
    CHECK(sMeterText(-70, 14e6) == "S9" && sMeterText(-140, 14e6) == "S0");
    CHECK(std::fabs(sUnits(-79, 14e6) - 8.0) < 1e-9);

    // ---- region ----
    CHECK(regionForCountry("LT") == 1 && regionForCountry("lt") == 1 && regionForCountry("US") == 2);
    CHECK(regionForCountry("") == 0 && regionForCountry("ZZ") == 0);

    // ---- setup hints ----
    {
        StationSetup s;
        CHECK(setupHints(s).size() == 9);
        s.call = "LY1ABC"; s.grid = "KO24"; s.licClass = "A"; s.country = "LT"; s.power = "100"; s.antennas = {"dipole"}; s.region = 1;
        s.licence = true; s.rig = true; s.audioOut = true;
        CHECK(setupHints(s).empty());
        s.grid = "XX";
        CHECK(setupHints(s).size() == 1);
    }

    // ---- CW speed matching ----
    {
        CwSpeedMatch m;
        CHECK(!m.ready() && m.wpm(20) == 20);
        m.heard(22); m.heard(23); m.heard(22);
        CHECK(m.ready() && m.wpm(20) == 22);
        m.heard(80);                                     // a misread: ignored
        CHECK(m.wpm(20) == 22);
        m.heard(0); m.heard(-3);                         // not readings
        for (int i = 0; i < 12; i++) m.heard(30);
        CHECK(m.wpm(20) == 30);
        m.maxWpm = 25;
        CHECK(m.wpm(20) == 25);
        m.reset();
        CHECK(!m.ready());
    }

    // ---- the band-hop schedule ----
    {
        HopSchedule h;
        std::string err;
        CHECK(HopSchedule::parse("0000-0600 40m ft8\n0600-1800 20m\n1800-2400 auto # the propagation hopper\n\n", h, err) && h.entries.size() == 3);
        const double day = 1794441600.0;   // 2026-11-12 00:00 UTC
        CHECK(h.at(day + 3 * 3600) && h.at(day + 3 * 3600)->band == "40m" && h.at(day + 3 * 3600)->modeId == "ft8");
        CHECK(h.at(day + 12 * 3600) && h.at(day + 12 * 3600)->band == "20m");
        CHECK(h.at(day + 20 * 3600) && h.at(day + 20 * 3600)->band == "auto");
        HopSchedule w;
        CHECK(HopSchedule::parse("2200-0200 80m", w, err));
        CHECK(w.at(day + 23 * 3600) && w.at(day + 3600) && !w.at(day + 12 * 3600));
        HopSchedule r;
        CHECK(HopSchedule::parse(h.toText(), r, err) && r.entries.size() == 3 && r.entries[2].startMin == 1080 && r.entries[2].endMin == 0 && r.entries[0].modeId == "ft8");
        CHECK(!HopSchedule::parse("2500-0100 20m", r, err) && err.find("line 1") != std::string::npos);
        CHECK(!HopSchedule::parse("0000-0100 20m\n0100-0200 banana", r, err) && err.find("line 2") != std::string::npos);
    }

    // ---- fake-it split ----
    {
        SplitTx s = fakeItSplit(1700);
        CHECK(s.dialShiftHz == 0 && s.audioHz == 1700);
        s = fakeItSplit(2600);
        CHECK(s.dialShiftHz == 1000 && s.audioHz == 1600);
        s = fakeItSplit(300);
        CHECK(s.dialShiftHz == -1500 && s.audioHz == 1800);
        s = fakeItSplit(1500);
        CHECK(s.dialShiftHz == 0 && s.audioHz == 1500);
        s = fakeItSplit(2000);
        CHECK(s.dialShiftHz == 500 && s.audioHz == 1500);
    }

    printf(fails ? "ham_station: %d FAILED\n" : "ham_station: all passed\n", fails);
    return fails ? 1 : 0;
}
