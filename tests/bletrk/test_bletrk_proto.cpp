// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// BLE tracker advertisements: the Find My network frame, Tile, Chipolo, Samsung SmartTag and Google Find My Device tags are recognised,
// ordinary advertisers (a phone's Nearby Info, a beacon, a short Apple frame) are not.
#include "dect2/ble/ble_proto.h"
#include "dect2/bletrk/bletrk_proto.h"
#include <cstdio>
#include <vector>
using namespace dect2;
using namespace dect2::bletrk;
using dect2::ble::AdBuilder;
#include "../common/test_util.h"

static Detection det(const std::vector<uint8_t>& ad, bool* ok = nullptr) {
    Detection d;
    const bool r = detect(ad.data(), ad.size(), d);
    if (ok) *ok = r;
    return d;
}

int main() {
    std::vector<uint8_t> fm = {0x12, 0x19, 0x24};
    for (int i = 0; i < 22; i++) fm.push_back((uint8_t)i);
    fm.push_back(0x02); fm.push_back(0x00);
    bool ok = false;
    Detection d = det(AdBuilder().mfr(0x004C, fm).b, &ok);
    CHECK(ok && d.kind == FindMy && d.separated && d.status == 0x24, "Find My frame: kind %d separated %d status %d", d.kind, d.separated, d.status);
    // flags before it, as some accessories send
    d = det(AdBuilder().flags(0x06).mfr(0x004C, fm).b, &ok);
    CHECK(ok && d.kind == FindMy, "Find My after flags");
    // a truncated frame and Apple's other types are not trackers
    std::vector<uint8_t> shortFm(fm.begin(), fm.begin() + 12);
    d = det(AdBuilder().mfr(0x004C, shortFm).b, &ok);
    CHECK(!ok && d.kind == None, "truncated Find My frame accepted");
    d = det(AdBuilder().mfr(0x004C, {0x10, 0x05, 0x01, 0x18, 0x44, 0x22, 0x11}).b, &ok);
    CHECK(!ok, "Nearby Info taken for a tracker");
    d = det(AdBuilder().mfr(0x0059, fm).b, &ok);
    CHECK(!ok, "another company's data taken for Find My");
    d = det(AdBuilder().serviceData16(0xFEED, {1, 2, 3}).b, &ok);
    CHECK(ok && d.kind == Tile && !d.separated, "Tile service data");
    d = det(AdBuilder().uuid16({0x180F, 0xFD84}).b, &ok);
    CHECK(ok && d.kind == Tile, "Tile in a UUID list");
    d = det(AdBuilder().uuid16({0xFE33}).b, &ok);
    CHECK(ok && d.kind == Chipolo, "Chipolo");
    d = det(AdBuilder().serviceData16(0xFD5A, {1, 2, 3, 4}).b, &ok);
    CHECK(ok && d.kind == SmartTag, "SmartTag");
    d = det(AdBuilder().serviceData16(0xFEAA, {0x41, 1, 2, 3}).b, &ok);
    CHECK(ok && d.kind == GoogleFmd && d.unwantedProtection, "Google tag in unwanted-tracking mode");
    d = det(AdBuilder().serviceData16(0xFEAA, {0x40, 1, 2, 3}).b, &ok);
    CHECK(ok && d.kind == GoogleFmd && !d.unwantedProtection, "Google tag");
    d = det(AdBuilder().serviceData16(0xFEAA, {0x00, 0x20, 0x00, 0x01}).b, &ok);
    CHECK(!ok, "an Eddystone UID frame taken for a tracker");
    // a malformed block does not run over
    const uint8_t bad[] = {0x09, 0xFF, 0x4C, 0x00, 0x12};
    Detection e;
    CHECK(!detect(bad, sizeof bad, e), "malformed length accepted");
    CHECK(!detect(nullptr, 0, e), "empty");
    CHECK(std::string(kindName(FindMy)).find("AirTag") != std::string::npos, "kind name");
    return testSummary("bletrk proto");
}
