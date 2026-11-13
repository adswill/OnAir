// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Band hopping (round 4 #17): the coordinated 20-minute cycle, a band switched off (the next one in the table), bands outside the table,
// the plain rotation, hours with different bands and with none, slot edges and negative times, the settings text.
#include "dect2/hfdig/hfdig_hop.h"
#include "../common/test_util.h"
#include <string>
using namespace dect2;

int main() {
    const int64_t day = 1'799'971'200;   // 2027-01-15 00:00 UTC
    HopPlan all = HopPlan::allHours(0x3FF);   // 160 .. 10 m
    for (int m = 0; m < 40; m += 2) CHECK(hopBand(all, day + m * 60) == (m / 2) % 10, "coordinated minute %d -> %d", m, hopBand(all, day + m * 60));
    CHECK(hopBand(all, day + 119) == 0 && hopBand(all, day + 120) == 1 && hopNextChange(all, day + 119) == day + 120 && hopNextChange(all, day + 120) == day + 240, "slot edges");
    HopPlan no60 = HopPlan::allHours(0x3FF & ~(1 << 2));
    CHECK(hopBand(no60, day + 4 * 60) == 3, "60 m off: 40 m in its slot");
    HopPlan six = HopPlan::allHours(1 << 10);
    CHECK(hopBand(six, day) == 10 && hopBand(six, day + 600) == 10, "only 6 m (outside the table)");
    HopPlan rot = HopPlan::allHours((1 << 3) | (1 << 5) | (1 << 9));
    rot.coordinated = false;
    CHECK(hopBand(rot, day) == 3 && hopBand(rot, day + 120) == 5 && hopBand(rot, day + 240) == 9 && hopBand(rot, day + 360) == 3, "rotation 40 20 10 m");
    CHECK(hopBand(rot, day - 120) == 9, "before 1970 / negative slot: still in turn");
    HopPlan hrs;
    hrs.hours[0] = 1 << 1;   // 00 UTC: 80 m only
    hrs.hours[12] = (1 << 5) | (1 << 7);   // 12 UTC: 20 and 15 m
    CHECK(hopBand(hrs, day + 600) == 1 && hopBand(hrs, day + 3600) == -1 && hopBand(hrs, day + 12 * 3600 + 10 * 60) == 5 && hopBand(hrs, day + 12 * 3600 + 14 * 60) == 7 &&
          hopBand(hrs, day + 12 * 3600 + 16 * 60) == 5, "per hour, nothing on at 01 UTC");
    const HopPlan back = hopFromText(hopToText(hrs));
    CHECK(back.coordinated && back.hours[0] == 2 && back.hours[12] == 0xA0 && back.hours[5] == 0, "settings round trip: %s", hopToText(hrs).c_str());
    CHECK(!hopFromText("c=0").coordinated && hopFromText("junk").hours[3] == 0, "bad text");

    // ---- the user's own frequencies (issue #501) ----
    {
        HopPlan u = HopPlan::allHours(1 << 5);   // 20 m only
        u.coordinated = false;
        HopUserFreq dx; dx.hz = 14091000; dx.mode = 0; dx.label = "DX; pedition|1:2%";   // a label with every separator character
        CHECK(hopAddUser(u, dx).empty() && u.user.size() == 1 && u.hours[0] == ((1u << 5) | (1u << kHopUserBase)) && u.hours[23] == u.hours[0], "added, on in every hour");
        // hop order: 20 m then the user's frequency, in turn, repeating
        CHECK(hopBand(u, day) == 5 && hopBand(u, day + 120) == kHopUserBase && hopBand(u, day + 240) == 5 && hopBand(u, day + 360) == kHopUserBase, "rotation 20 m, user");
        CHECK(hopDialHz(u, kHopUserBase, 0) == 14091000 && hopDialHz(u, 5, 0) == 14074000 && hopDialHz(u, kHopUserBase + 3, 0) == 0, "dial frequencies");
        CHECK(hopBandName(u, 5) == "20 m" && hopBandName(u, kHopUserBase).find("14.0910 MHz") != std::string::npos, "names: %s", hopBandName(u, kHopUserBase).c_str());
        // a second one, another mode: FT8 plan only sees the FT8 entry, a JT65 plan only the JT65 one, in order of the list
        HopUserFreq jt; jt.hz = 14076000; jt.mode = 4; jt.label = "JT65 net";
        CHECK(hopAddUser(u, jt).empty() && u.user.size() == 2, "second added");
        for (int k = 0; k < 8; k++) { const int b = hopBand(u, day + k * 120, 0); CHECK(b == 5 || b == kHopUserBase, "FT8 plan hops only to 20 m and the FT8 entry (got %d)", b); }
        CHECK(hopBand(u, day, 4) == 5 && hopBand(u, day + 120, 4) == kHopUserBase + 1, "JT65 plan: the JT65 entry (and 20 m, which has no JT65 dial: the app notes it) in turn: %d %d", hopBand(u, day, 4), hopBand(u, day + 120, 4));
        // same frequency and mode replaces, nothing is added
        HopUserFreq dx2 = dx; dx2.label = "renamed"; dx2.enabled = false;
        CHECK(hopAddUser(u, dx2).empty() && u.user.size() == 2 && u.user[0].label == "renamed" && !u.user[0].enabled, "replace");
        CHECK(hopBand(u, day, 0) == 5 && hopBand(u, day + 120, 0) == 5, "a disabled entry is never hopped to");
        u.user[0].enabled = true; u.user[0].label = dx.label;
        // settings round trip: frequencies, modes, enable, hostile labels, the plan bits
        const HopPlan r = hopFromText(hopToText(u));
        CHECK(r.user.size() == 2 && r.user[0].hz == 14091000 && r.user[0].mode == 0 && r.user[0].enabled && r.user[0].label == dx.label && r.user[1].mode == 4 && r.user[1].label == "JT65 net" &&
              r.hours[7] == u.hours[7] && !r.coordinated, "user round trip: %s", hopToText(u).c_str());
        for (int k = 0; k < 8; k++) CHECK(hopBand(r, day + k * 120, 0) == hopBand(u, day + k * 120, 0), "same hop order after saving, slot %d", k);
        CHECK(hopFromText("c=1;h=3FF;u=garbage|14091000:0:1:ok").user.size() == 1 && hopFromText("u=").user.empty(), "bad user text does not crash");
        // coordinated: the table is unchanged with a user frequency on; the user frequency is used only when no table band is on
        HopPlan c = HopPlan::allHours(0x3FF); hopAddUser(c, dx);
        for (int m = 0; m < 40; m += 2) CHECK(hopBand(c, day + m * 60) == (m / 2) % 10, "coordinated table untouched by a user entry, minute %d", m);
        HopPlan only = HopPlan::allHours(0); hopAddUser(only, dx);
        CHECK(hopBand(only, day) == kHopUserBase && hopBand(only, day + 3600 * 5) == kHopUserBase, "only a user entry on: coordinated uses it");
        only.hours[1] = 0;
        CHECK(hopBand(only, day + 3600) == -1, "hour switched off");
        // delete: later entries move down and the plan keeps them on where they were
        HopPlan d = HopPlan::allHours(0); d.coordinated = false;
        for (int i = 0; i < 3; i++) { HopUserFreq f; f.hz = 14091000 + i * 1000; f.mode = 0; hopAddUser(d, f); }
        d.hours[2] = 1u << (kHopUserBase + 2);   // hour 2: only the third entry
        d.hours[3] = 1u << (kHopUserBase + 0);   // hour 3: only the first
        hopRemoveUser(d, 0);
        CHECK(d.user.size() == 2 && d.user[0].hz == 14092000 && d.user[1].hz == 14093000, "removed the first");
        CHECK(hopBand(d, day + 2 * 3600) == kHopUserBase + 1 && hopDialHz(d, kHopUserBase + 1, 0) == 14093000, "hour 2: the third entry is now number 1");
        CHECK(hopBand(d, day + 3 * 3600) == -1, "hour 3: the removed entry is gone from the plan");
        hopRemoveUser(d, 5); hopRemoveUser(d, -1);
        CHECK(d.user.size() == 2, "bad index ignored");
        // limits and validation
        HopPlan full; std::string e;
        for (int i = 0; i < kHopUserMax; i++) { HopUserFreq f; f.hz = 7000000 + i * 1000; CHECK(hopAddUser(full, f).empty(), "fill %d", i); }
        HopUserFreq more; more.hz = 7100000;
        e = hopAddUser(full, more);
        CHECK(!e.empty() && (int)full.user.size() == kHopUserMax && e.find("863109") != std::string::npos, "list full: %s", e.c_str());
        HopUserFreq bad; bad.hz = 50;
        CHECK(!hopAddUser(full, bad).empty() && !hopAddUser(u, HopUserFreq{14091000, 99, "", true}).empty() && u.user.size() == 2, "bad frequency / mode refused");
        // every parameter combination: all 20 entries on in a 24-hour plan, both modes of rotation, a week of slots, never a band that is off or a wrong index
        for (int co = 0; co < 2; co++) {
            full.coordinated = co; full.hours.fill(0xFFFFFFFFu & ~((1u << kHopUserBase) - 1u));   // all user entries on
            for (int k = 0; k < 7 * 720; k++) {
                const int64_t t = day + (int64_t)k * 120 + 17;
                const int b = hopBand(full, t, 0);
                CHECK(b >= kHopUserBase && b < kHopUserBase + kHopUserMax, "week slot %d band %d", k, b);
                if (b < kHopUserBase || b >= kHopUserBase + kHopUserMax) break;
            }
        }
        // negative times and day wrap: the rotation is the same each day at the same slot of the hour
        HopPlan w = HopPlan::allHours(0); w.coordinated = false; HopUserFreq a; a.hz = 14091000; hopAddUser(w, a); HopUserFreq b2; b2.hz = 14092000; hopAddUser(w, b2);
        CHECK(hopBand(w, day - 120) >= kHopUserBase && hopBand(w, day - 1) >= kHopUserBase && hopBand(w, day - 120) != hopBand(w, day - 240), "negative time: still in turn");
        // warnings: only for what is outside the plans; the frequency is still added
        CHECK(hopUserWarning(14091000, 0).find("away from the usual FT8") != std::string::npos, "14.091 FT8: off the usual dial: %s", hopUserWarning(14091000, 0).c_str());
        CHECK(hopUserWarning(14074000, 0).empty() && hopUserWarning(14076000, 4).empty(), "usual dial: no warning");
        CHECK(hopUserWarning(13000000, 0).find("not inside an amateur band") != std::string::npos && hopUserWarning(144174000, 4).empty(), "outside a band warned; 2 m inside");
    }
    return testSummary("hfdig_hop");
}
