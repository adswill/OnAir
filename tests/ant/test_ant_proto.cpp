// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ANT+ frames: the CRC against frames captured off the air (rtl_433 issue 1990, reverse-engineered with reveng), the field layout, the
// heart rate and other profile pages.
#include "dect2/ant/ant_proto.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static std::vector<uint8_t> fromHex(const char* h) {
    std::vector<uint8_t> v;
    for (size_t i = 0; h[i] && h[i + 1]; i += 2) { unsigned b; sscanf(h + i, "%2x", &b); v.push_back((uint8_t)b); }
    return v;
}

int main() {
    // frames sent by an nRF52 with the ANT+ key (marker A6C5) and with a private key (marker 5B25): the last two bytes are the CRC on the air
    const char* caps[] = {"A6C5310078010AB9A521FBBD72C3459471", "5B25310078010AFDFEFF00010203044D10", "5B25310078010A010203040506070864F9", "5B25310078010A05060708090A0B0C1862"};
    for (const char* c : caps) {
        const std::vector<uint8_t> b = fromHex(c);
        CHECK(b.size() == 17, "capture length %zu", b.size());
        ant::Frame f;
        CHECK(ant::parseFrame(b.data(), f), "length byte");
        CHECK(f.crcOk, "CRC of %s: computed %04X, sent %04X", c, ant::crc16(b.data(), 15), f.crc);
        CHECK(f.deviceNumber == 0x0031 && f.type() == 0x78 && f.transType == 1, "fields of %s: dev %u type %d trans %d", c, f.deviceNumber, f.type(), f.transType);
        uint8_t out[17];
        ant::buildFrame(f, out);
        CHECK(memcmp(out, b.data(), 17) == 0, "rebuild of %s differs", c);
    }
    {   // the first capture carries the ANT+ network key itself as payload; device type 0x78 is a heart rate monitor
        ant::Frame f;
        ant::parseFrame(fromHex(caps[0]).data(), f);
        CHECK(f.antPlus(), "marker");
        CHECK(std::string(ant::deviceTypeName(f.type())) == "Heart rate monitor", "type name %s", ant::deviceTypeName(f.type()));
    }
    {   // a damaged frame
        std::vector<uint8_t> b = fromHex(caps[0]);
        b[9] ^= 0x10;
        ant::Frame f;
        ant::parseFrame(b.data(), f);
        CHECK(!f.crcOk, "damaged frame passed");
    }
    {   // air bits: preamble alternates into the first marker bit
        ant::Frame f;
        f.marker = ant::kAntPlusMarker; f.deviceNumber = 0x6941; f.deviceType = 120; f.transType = 1;
        std::vector<uint8_t> bits;
        ant::frameBits(f, bits);
        CHECK(bits.size() == 8 + 17 * 8, "bits %zu", bits.size());
        uint8_t pre = 0;
        for (int k = 0; k < 8; k++) pre = (uint8_t)(pre << 1 | bits[(size_t)k]);
        CHECK(pre == 0xAA, "preamble %02X before a marker starting with 1", pre);
        f.marker = 0x5B25;   // starts with 0: the preamble is 0x55
        ant::frameBits(f, bits);
        pre = 0;
        for (int k = 0; k < 8; k++) pre = (uint8_t)(pre << 1 | bits[(size_t)k]);
        CHECK(pre == 0x55, "preamble %02X before a marker starting with 0", pre);
    }
    {   // heart rate pages
        uint8_t p[8] = {0x80, 0xFF, 0xFF, 0xFF, 0x34, 0x12, 0x8F, 72};   // page 0, toggle set, beat time 0x1234, beat 143, 72 bpm
        ant::Reading r = ant::decodePayload(ant::HeartRate, p);
        CHECK(r.page == 0 && r.toggle && r.heartRate == 72 && r.beatCount == 143, "page 0: page %d hr %d beats %d", r.page, r.heartRate, r.beatCount);
        CHECK(std::fabs(r.beatTimeSec - 0x1234 / 1024.0) < 1e-9, "beat time %f", r.beatTimeSec);
        const uint8_t p2[8] = {0x02, 1, 0x39, 0x30, 0, 0, 0, 65};
        r = ant::decodePayload(ant::HeartRate, p2);
        CHECK(r.page == 2 && r.manufacturer == 1 && r.serial == 0x3039 && r.text.find("Garmin") != std::string::npos, "page 2: %s", r.text.c_str());
        const uint8_t p4[8] = {0x04, 0xFF, 0x00, 0x10, 0x00, 0x12, 10, 60};   // previous beat at 0x1000, this at 0x1200: 0x200 / 1024 = 0.5 s
        r = ant::decodePayload(ant::HeartRate, p4);
        CHECK(std::fabs(r.rrIntervalSec - 0.5) < 1e-6, "R-R %f", r.rrIntervalSec);
        const uint8_t p7[8] = {0x07, 85, 0x80, (uint8_t)(2 << 4 | 2), 0, 0, 1, 70};
        r = ant::decodePayload(ant::HeartRate, p7);
        CHECK(r.batteryPct == 85 && r.batteryStatus == 2 && std::fabs(r.batteryV - 2.5) < 1e-6, "page 7: %d%% %s %.2f V", r.batteryPct, ant::batteryStatusName(r.batteryStatus), r.batteryV);
        const uint8_t p0z[8] = {0x00, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0};
        r = ant::decodePayload(ant::HeartRate, p0z);
        CHECK(r.heartRate == 0 && r.text.find("no heart rate") != std::string::npos, "invalid hr: %s", r.text.c_str());
    }
    {   // bike power standard page
        const uint8_t p[8] = {0x10, 7, 0x80 | 52, 85, 0x10, 0x27, 0xB4, 0x00};
        ant::Reading r = ant::decodePayload(ant::BikePower, p);
        CHECK(r.instPowerW == 180 && r.instCadenceRpm == 85 && r.accumPowerW == 10000 && r.pedalPowerPct == 52, "power: %d W %d rpm acc %d pedal %d", r.instPowerW, r.instCadenceRpm, r.accumPowerW, r.pedalPowerPct);
        const uint8_t m[8] = {80, 0xFF, 0xFF, 1, 32, 0, 0x0B, 0};
        r = ant::decodePayload(ant::BikePower, m);
        CHECK(r.manufacturer == 32 && r.model == 11 && r.text.find("Wahoo") != std::string::npos, "common page 80: %s", r.text.c_str());
    }
    {   // speed and cadence counters, footpod, environment
        const uint8_t sc[8] = {0x00, 0x04, 0x10, 0x00, 0x00, 0x08, 0x20, 0x00};
        ant::Reading r = ant::decodePayload(ant::BikeSpeedCadence, sc);
        CHECK(r.cadenceEventTime == 0x0400 && r.cadenceRevs == 16 && r.speedEventTime == 0x0800 && r.speedRevs == 32, "s&c counters");
        const uint8_t sdm[8] = {0x01, 0x00, 0x00, 25, (uint8_t)(0x8 << 4 | 3), 0x80, 40, 0};
        r = ant::decodePayload(ant::StrideSdm, sdm);
        CHECK(std::fabs(r.sdmSpeedMs - 3.5) < 1e-6 && std::fabs(r.sdmDistanceM - 25.5) < 1e-6 && r.strideCount == 40, "footpod: %.2f m/s %.2f m %d", r.sdmSpeedMs, r.sdmDistanceM, r.strideCount);
        const uint8_t env[8] = {0x01, 0x00, 0x00, 0xFF, 0x7F, 0xF8, 0x4E, 0x08};   // 2126 / 100 = 21.26 C
        r = ant::decodePayload(ant::Environment, env);
        CHECK(std::fabs(r.temperatureC - 21.26) < 1e-6, "temperature %.2f", r.temperatureC);
    }
    printf(fails ? "ant proto: %d FAILED\n" : "ant proto: all passed\n", fails);
    return fails ? 1 : 0;
}
