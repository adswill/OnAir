// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Drone Remote ID messages (rid_proto.h): messages written byte by byte from the ASTM F3411-22a field tables, decoded; encode / decode
// round trips of every type; a message pack; the BLE service data and the Wi-Fi vendor element; unknown values and malformed input.
#include "dect2/rid/rid_proto.h"
#include "dect2/wifi/wifi_proto.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
using namespace dect2;
using namespace dect2::rid;
#include "../common/test_util.h"
static bool nearBy(double a, double b, double e = 1e-6) { return std::fabs(a - b) <= e; }

int main() {
    {   // Basic ID: protocol version 2, serial number, multirotor
        uint8_t m[25] = {0x02, 0x12};
        memcpy(m + 2, "1581F5FKD229400B", 16);
        Message o;
        CHECK(decodeMessage(m, o) && o.type == BasicId && o.version == 2 && o.idType == 1 && o.uaType == 2 && o.id == "1581F5FKD229400B", "basic id %s", o.id.c_str());
        CHECK(std::string(uaTypeName(o.uaType)) == "multirotor / helicopter" && std::string(idTypeName(1)) == "serial number", "names");
    }
    {   // Location: airborne, track 90, 8.5 m/s, up 1.5 m/s, 54.6872 N 25.2797 E, baro 230.5, geo 245, height 120 above take-off
        const uint8_t m[25] = {0x12, 0x20, 0x5A, 0x22, 0x03, 0xC0, 0x9A, 0x98, 0x20, 0x48, 0x60, 0x11, 0x0F, 0x9D, 0x09, 0xBA, 0x09, 0xC0, 0x08, 0x4A, 0x33, 0x39, 0x30, 0x02, 0x00};
        Message o;
        CHECK(decodeMessage(m, o) && o.type == Location, "location type");
        CHECK(o.status == 2 && o.heightType == 0 && nearBy(o.direction, 90) && nearBy(o.speed, 8.5) && o.vspeedValid && nearBy(o.vspeed, 1.5), "vector %.1f %.2f %.1f", o.direction, o.speed, o.vspeed);
        CHECK(o.posValid && nearBy(o.lat, 54.6872) && nearBy(o.lon, 25.2797), "position %.7f %.7f", o.lat, o.lon);
        CHECK(nearBy(o.altBaro, 230.5) && nearBy(o.altGeo, 245) && nearBy(o.height, 120), "altitudes %.1f %.1f %.1f", o.altBaro, o.altGeo, o.height);
        CHECK(o.horizAcc == 10 && o.vertAcc == 4 && o.baroAcc == 3 && o.speedAcc == 3 && o.timestamp == 12345 && o.tsAcc == 2, "accuracies");
        CHECK(horizAccText(10) == "< 10 m" && vertAccText(4) == "< 10 m" && speedAccText(3) == "< 1 m/s", "accuracy names");
    }
    {   // Location: track 275 (east-west bit), 70 m/s (speed multiplier), vertical speed unknown, height above ground
        const uint8_t m[25] = {0x12, 0x27, 0x5F, 0x08, 0x7E, 0xA8, 0x92, 0x9A, 0x20, 0x10, 0x81, 0x14, 0x0F, 0x04, 0x0B, 0x18, 0x0B, 0x28, 0x0A, 0x5B, 0x04, 0x58, 0x02, 0x00, 0x00};
        Message o;
        CHECK(decodeMessage(m, o) && nearBy(o.direction, 275) && nearBy(o.speed, 69.75) && !o.vspeedValid && o.heightType == 1, "dir %.0f speed %.2f", o.direction, o.speed);
        CHECK(nearBy(o.lat, 54.7001) && nearBy(o.lon, 25.3002) && nearBy(o.height, 300) && o.timestamp == 600, "position 2");
    }
    {   // Location with every value unknown
        uint8_t m[25] = {0x12, 0x03, 181, 255, 126};
        m[21] = 0xFF; m[22] = 0xFF;
        Message o;
        CHECK(decodeMessage(m, o) && !o.posValid && o.direction < 0 && o.speed < 0 && !o.vspeedValid && o.altGeo <= -1000 && o.timestamp < 0, "unknowns");
    }
    {   // System: live GNSS operator at 54.6860 N 25.2770 E, EU, open category class C1, operator altitude 126 m
        const uint8_t m[25] = {0x42, 0x05, 0xE0, 0x6B, 0x98, 0x20, 0xD0, 0xF6, 0x10, 0x0F, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x12, 0xCC, 0x08, 0x40, 0x67, 0x9A, 0x0E, 0x00};
        Message o;
        CHECK(decodeMessage(m, o) && o.type == System && o.opLocType == 1 && o.classType == 1 && o.opValid, "system");
        CHECK(nearBy(o.opLat, 54.6860) && nearBy(o.opLon, 25.2770) && nearBy(o.opAlt, 126) && o.category == 1 && o.uaClass == 2 && o.sysTime == 245000000 && o.areaCount == 1,
              "system fields %.6f %.6f %.1f", o.opLat, o.opLon, o.opAlt);
        CHECK(className(o.uaClass) == "C1" && std::string(categoryName(1)) == "open" && std::string(opLocTypeName(1)) == "live GNSS", "system names");
    }
    {   // Self-ID, Operator ID, authentication page 0
        uint8_t s[25] = {0x32, 0x00}; memcpy(s + 2, "Survey flight", 13);
        uint8_t op[25] = {0x52, 0x00}; memcpy(op + 2, "LTUabcd1234efgh", 15);
        uint8_t au[25] = {0x22, 0x10, 0x02, 0x33, 0x40, 0x67, 0x9A, 0x0E};
        for (int i = 8; i < 25; i++) au[i] = (uint8_t)i;
        Message a, b, c;
        CHECK(decodeMessage(s, a) && a.type == SelfId && a.text == "Survey flight", "self id '%s'", a.text.c_str());
        CHECK(decodeMessage(op, b) && b.type == OperatorId && b.text == "LTUabcd1234efgh", "operator id");
        CHECK(decodeMessage(au, c) && c.type == Auth && c.authType == 1 && c.page == 0 && c.lastPage == 2 && c.authLength == 0x33 && c.authTime == 245000000 &&
              c.authData.size() == 17 && c.authData[0] == 8, "auth");
    }
    {   // a reserved message type and a short block
        uint8_t m[25] = {0x72};
        Message o;
        std::vector<Message> v;
        CHECK(!decodeMessage(m, o) && decodeBlock(m, 25, v) == -1 && decodeBlock(m, 10, v) == -1 && v.empty(), "malformed");
    }
    {   // round trips of every type, and a pack of them
        std::vector<Message> in;
        Message b; b.type = BasicId; b.version = 2; b.idType = 4; b.uaType = 6; b.id = "abc-123"; in.push_back(b);
        Message l; l.type = Location; l.version = 2; l.status = 3; l.posValid = true; l.lat = -33.8688; l.lon = 151.2093; l.altGeo = 50; l.height = 12.5; l.speed = 200;
        l.direction = 359; l.vspeedValid = true; l.vspeed = -4; l.timestamp = 35999; in.push_back(l);
        Message y; y.type = System; y.version = 2; y.opValid = true; y.opLat = -33.87; y.opLon = 151.2; y.opLocType = 2; y.areaCount = 3; y.areaRadius = 250;
        y.areaCeiling = 120; y.areaFloor = 0; in.push_back(y);
        Message o; o.type = OperatorId; o.version = 2; o.text = "OP-1"; in.push_back(o);
        const std::vector<uint8_t> pk = encodePack(in);
        CHECK(pk.size() == 3 + 4 * 25 && pk[0] == 0xF2 && pk[1] == 25 && pk[2] == 4, "pack header");
        std::vector<Message> out;
        CHECK(decodeBlock(pk.data(), pk.size(), out) == 4, "pack decode %zu", out.size());
        if (out.size() == 4) {
            CHECK(out[0].idType == 4 && out[0].uaType == 6 && out[0].id == "abc-123", "rt basic");
            CHECK(out[1].status == 3 && nearBy(out[1].lat, -33.8688) && nearBy(out[1].lon, 151.2093) && nearBy(out[1].altGeo, 50) && nearBy(out[1].height, 12.5) &&
                  std::fabs(out[1].speed - 200) < 0.4 && nearBy(out[1].direction, 359) && nearBy(out[1].vspeed, -4) && out[1].timestamp == 35999, "rt location %.2f", out[1].speed);
            CHECK(out[2].opLocType == 2 && out[2].areaCount == 3 && out[2].areaRadius == 250 && nearBy(out[2].areaCeiling, 120) && nearBy(out[2].areaFloor, 0) && nearBy(out[2].opLat, -33.87), "rt system");
            CHECK(out[3].text == "OP-1", "rt operator");
        }
        // the Wi-Fi vendor element inside a beacon
        wifi::ApSpec s; s.ssid = "drone"; s.security = wifi::Open;
        std::vector<uint8_t> f = wifi::makeBeacon(s);
        f.resize(f.size() - 4);
        const std::vector<uint8_t> ie = beaconElement(7, in);
        CHECK(ie[0] == 221 && ie[2] == 0xFA && ie[3] == 0x0B && ie[4] == 0xBC && ie[5] == 0x0D && ie[6] == 7, "element header");
        f.insert(f.end(), ie.begin(), ie.end());
        int counter = -1; out.clear();
        CHECK(fromBeacon(f.data(), f.size(), counter, out) && counter == 7 && out.size() == 4, "beacon: %zu messages", out.size());
        f.resize(f.size() - ie.size());
        out.clear();
        CHECK(!fromBeacon(f.data(), f.size(), counter, out) && out.empty(), "beacon without Remote ID");
        // BLE service data
        const std::vector<uint8_t> sd = bleServiceData(42, l);
        CHECK(sd.size() == 31 && sd[0] == 30 && sd[1] == 0x16 && sd[2] == 0xFA && sd[3] == 0xFF && sd[4] == 0x0D && sd[5] == 42, "service data header");
        std::vector<uint8_t> ad = {0x02, 0x01, 0x06};
        ad.insert(ad.end(), sd.begin(), sd.end());
        out.clear();
        CHECK(fromBleAdvData(ad.data(), ad.size(), counter, out) && counter == 42 && out.size() == 1 && out[0].type == Location, "ble service data");
        std::vector<uint8_t> other = {0x05, 0x16, 0x0F, 0x18, 0x0D, 0x01};   // battery service data
        out.clear();
        CHECK(!fromBleAdvData(other.data(), other.size(), counter, out), "other service data");
    }
    printf(fails ? "rid proto: %d FAILED\n" : "rid proto: all passed\n", fails);
    return fails ? 1 : 0;
}
