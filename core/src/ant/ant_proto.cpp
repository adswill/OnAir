// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ANT / ANT+ frames and device profiles (see ant_proto.h).
#include "dect2/ant/ant_proto.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include "dect2/common/crc.h"
#include "dect2/common/str.h"

namespace dect2 {
namespace ant {

uint16_t crc16(const uint8_t* p, size_t n) { return crc::crc16CcittFalse(p, n); }

bool parseFrame(const uint8_t* b, Frame& f) {
    f = Frame();
    f.marker = (uint16_t)(b[0] << 8 | b[1]);
    f.deviceNumber = (uint16_t)(b[2] | b[3] << 8);
    f.deviceType = b[4];
    f.transType = b[5];
    f.length = b[6];
    memcpy(f.payload, b + 7, kPayload);
    f.crc = (uint16_t)(b[15] << 8 | b[16]);
    f.crcOk = crc16(b, 15) == f.crc;
    return f.length == 0x0A;
}

void buildFrame(const Frame& f, uint8_t out[kFrameBytes]) {
    out[0] = (uint8_t)(f.marker >> 8); out[1] = (uint8_t)f.marker;
    out[2] = (uint8_t)f.deviceNumber; out[3] = (uint8_t)(f.deviceNumber >> 8);
    out[4] = f.deviceType; out[5] = f.transType; out[6] = 0x0A;
    memcpy(out + 7, f.payload, kPayload);
    const uint16_t c = crc16(out, 15);
    out[15] = (uint8_t)(c >> 8); out[16] = (uint8_t)c;
}

void frameBits(const Frame& f, std::vector<uint8_t>& bits) {
    uint8_t b[kFrameBytes];
    buildFrame(f, b);
    bits.clear();
    const uint8_t pre = (b[0] & 0x80) ? 0xAA : 0x55;   // alternates into the first bit of the marker
    for (int k = 7; k >= 0; k--) bits.push_back((uint8_t)(pre >> k & 1));
    for (int i = 0; i < kFrameBytes; i++) for (int k = 7; k >= 0; k--) bits.push_back((uint8_t)(b[i] >> k & 1));
}

const char* deviceTypeName(int t) {
    switch (t) {
    case HeartRate: return "Heart rate monitor";
    case BikeSpeedCadence: return "Bike speed and cadence";
    case BikeCadence: return "Bike cadence";
    case BikeSpeed: return "Bike speed";
    case StrideSdm: return "Footpod (stride)";
    case BikePower: return "Bike power meter";
    case FitnessEquipment: return "Fitness equipment";
    case MultiSportSdm: return "Multi-sport speed and distance";
    case WeightScale: return "Weight scale";
    case Controls: return "Remote control";
    case Environment: return "Environment sensor";
    case Lev: return "Light electric vehicle";
    case BloodPressure: return "Blood pressure monitor";
    case Geocache: return "Geocache";
    case MuscleOxygen: return "Muscle oxygen monitor";
    case BikeRadar: return "Bike radar";
    case BikeLights: return "Bike lights";
    case Shifting: return "Shifting";
    default: return "ANT device";
    }
}

const char* manufacturerName(int id) {
    switch (id) {
    case 1: return "Garmin";
    case 2: return "Garmin (FR405)";
    case 3: return "Zephyr";
    case 4: return "Dayton";
    case 5: return "IDT";
    case 6: return "SRM";
    case 7: return "Quarq";
    case 8: return "iBike";
    case 9: return "Saris";
    case 10: return "Spark HK";
    case 11: return "Tanita";
    case 12: return "Echowell";
    case 13: return "Dynastream OEM";
    case 14: return "Nautilus";
    case 15: return "Dynastream";
    case 16: return "Timex";
    case 17: return "Metrigear";
    case 18: return "Xelic";
    case 19: return "Beurer";
    case 20: return "Cardiosport";
    case 21: return "A&D";
    case 22: return "HMM";
    case 23: return "Suunto";
    case 24: return "Thita Elektronik";
    case 25: return "G.Pulse";
    case 26: return "Clean Mobile";
    case 27: return "Pedal Brain";
    case 28: return "Peaksware";
    case 29: return "Saxonar";
    case 30: return "Lemond Fitness";
    case 31: return "Dexcom";
    case 32: return "Wahoo Fitness";
    case 33: return "Octane Fitness";
    case 34: return "Archinoetics";
    case 35: return "The Hurt Box";
    case 36: return "Citizen Systems";
    case 37: return "Magellan";
    case 38: return "Osynce";
    case 39: return "Holux";
    case 40: return "Concept2";
    case 42: return "One Giant Leap";
    case 43: return "Ace Sensor";
    case 44: return "Brim Brothers";
    case 45: return "Xplova";
    case 46: return "Perception Digital";
    case 47: return "BF1 Systems";
    case 48: return "Pioneer";
    case 49: return "Spantec";
    case 50: return "Metalogics";
    case 51: return "4iiii";
    case 52: return "Seiko Epson";
    case 53: return "Seiko Epson OEM";
    case 54: return "IFOR Powell";
    case 55: return "Maxwell Guider";
    case 56: return "StarTrac";
    case 57: return "Breakaway";
    case 58: return "Alatech";
    case 59: return "MIO Technology";
    case 60: return "Rotor";
    case 61: return "Geonaute";
    case 62: return "ID Bike";
    case 63: return "Specialized";
    case 64: return "Wtek";
    case 65: return "Physical Enterprises";
    case 66: return "North Pole Engineering";
    case 67: return "BKool";
    case 68: return "Cateye";
    case 69: return "Stages Cycling";
    case 70: return "Sigma Sport";
    case 71: return "TomTom";
    case 72: return "Peripedal";
    case 73: return "Wattbike";
    case 76: return "Moxy";
    case 77: return "CicloSport";
    case 78: return "Powerbahn";
    case 79: return "Acorn Projects";
    case 80: return "Lifebeam";
    case 81: return "Bontrager";
    case 82: return "Wellgo";
    case 83: return "Scosche";
    case 84: return "Magura";
    case 85: return "Woodway";
    case 86: return "Elite";
    case 87: return "Nielsen-Kellerman";
    case 88: return "DK City";
    case 89: return "Tacx";
    case 90: return "Direction Technology";
    case 91: return "Magtonic";
    case 92: return "1partcarbon";
    case 93: return "Inside Ride";
    case 94: return "Sound of Motion";
    case 95: return "Stryd";
    case 96: return "ICG";
    case 97: return "DK City";
    case 98: return "Giant";
    case 99: return "Look";
    case 100: return "Polar";
    case 101: return "Ritchey";
    case 102: return "Schwinn";
    case 103: return "LiveRider";
    case 104: return "Grantham";
    case 105: return "Sigma Sport";
    case 106: return "Favero";
    case 107: return "Trainingpeaks";
    case 108: return "Bryton";
    case 109: return "SRAM";
    case 110: return "Navman";
    case 111: return "COBI";
    case 112: return "Spivi";
    case 113: return "Mio Magellan";
    case 114: return "Mio Magellan";
    case 115: return "Coros";
    case 116: return "Shimano";
    case 117: return "Mio Technology";
    case 118: return "Zwift";
    case 119: return "JetBlack";
    case 120: return "Lezyne";
    case 121: return "Saris";
    case 122: return "iFit";
    case 123: return "Johnson Health Tech";
    case 124: return "Keiser";
    case 125: return "Precor";
    case 126: return "Sole Fitness";
    case 127: return "Zwift";
    case 255: return "development";
    default: return "";
    }
}

const char* batteryStatusName(int s) {
    switch (s) { case 1: return "new"; case 2: return "good"; case 3: return "ok"; case 4: return "low"; case 5: return "critical"; default: return "unknown"; }
}

static int u16(const uint8_t* p) { return p[0] | p[1] << 8; }

static void commonPage(Reading& r, const uint8_t* p, char* b, size_t bn) {
    // the manufacturer and product pages every profile shares (80, 81, 82)
    if (r.page == 80) {
        r.pageName = "Manufacturer";
        r.hwRev = p[3]; r.manufacturer = u16(p + 4); r.model = u16(p + 6);
        const char* m = manufacturerName(r.manufacturer);
        snprintf(b, bn, "%s%s%s, model %d, hardware rev %d", m[0] ? m : "manufacturer ", m[0] ? "" : std::to_string(r.manufacturer).c_str(), "", r.model, r.hwRev);
        r.text = b;
    } else if (r.page == 81) {
        r.pageName = "Product";
        r.swRev = p[3] == 0xFF ? p[3] : p[3];
        const uint32_t ser = (uint32_t)p[4] | (uint32_t)p[5] << 8 | (uint32_t)p[6] << 16 | (uint32_t)p[7] << 24;
        snprintf(b, bn, "software rev %d, serial %u", p[3], ser);
        r.serial = (int)(ser & 0x7FFFFFFF);
        r.text = b;
    } else if (r.page == 82) {
        r.pageName = "Battery";
        r.batteryPct = p[1] == 0xFF ? -1 : p[1];
        const double v = (p[3] & 0x0F) + p[2] / 256.0;
        r.batteryV = (p[3] & 0x0F) == 0x0F ? -1 : v;
        r.batteryStatus = (p[3] >> 4) & 7;
        snprintf(b, bn, "battery %s%s%.2f V", batteryStatusName(r.batteryStatus), r.batteryV >= 0 ? ", " : "", r.batteryV >= 0 ? r.batteryV : 0.0);
        r.text = b;
        if (r.batteryV < 0) r.text = std::string("battery ") + batteryStatusName(r.batteryStatus);
    }
}

Reading decodePayload(int type, const uint8_t p[kPayload]) {
    Reading r;
    char b[200];
    r.page = p[0] & 0x7F;
    r.toggle = (p[0] & 0x80) != 0;
    switch (type) {
    case HeartRate: {
        // every page carries the heart rate fields in bytes 4 - 7
        r.beatTimeSec = u16(p + 4) / 1024.0;
        r.beatCount = p[6];
        r.heartRate = p[7];
        char extra[120] = "";
        switch (r.page) {
        case 0: r.pageName = "Heart rate"; break;
        case 1: r.pageName = "Operating time"; r.operatingTimeSec = (double)(p[1] | p[2] << 8 | p[3] << 16) * 2; snprintf(extra, sizeof extra, ", on for %.1f h", r.operatingTimeSec / 3600); break;
        case 2: { r.pageName = "Manufacturer"; r.manufacturer = p[1]; r.serial = u16(p + 2); const char* m = manufacturerName(r.manufacturer);
                  snprintf(extra, sizeof extra, ", %s%s serial %d", m[0] ? m : "manufacturer ", m[0] ? "," : std::to_string(r.manufacturer).c_str(), r.serial); break; }
        case 3: r.pageName = "Product"; r.hwRev = p[1]; r.swRev = p[2]; r.model = p[3]; snprintf(extra, sizeof extra, ", hw %d sw %d model %d", r.hwRev, r.swRev, r.model); break;
        case 4: { r.pageName = "Previous beat"; const double prev = u16(p + 2) / 1024.0; double rr = r.beatTimeSec - prev; if (rr < 0) rr += 64; r.rrIntervalSec = rr;
                  snprintf(extra, sizeof extra, ", R-R %.0f ms", rr * 1000); break; }
        case 5: r.pageName = "Swim interval"; snprintf(extra, sizeof extra, ", interval average %d bpm, max %d bpm", p[1], p[2]); break;
        case 6: r.pageName = "Capabilities"; snprintf(extra, sizeof extra, ", features 0x%02X enabled 0x%02X", p[2], p[3]); break;
        case 7: { r.pageName = "Battery"; r.batteryPct = p[1] == 0xFF ? -1 : p[1]; r.batteryV = (p[3] & 0x0F) == 0x0F ? -1 : (p[3] & 0x0F) + p[2] / 256.0; r.batteryStatus = (p[3] >> 4) & 7;
                  if (r.batteryPct >= 0) snprintf(extra, sizeof extra, ", battery %d%% (%s)", r.batteryPct, batteryStatusName(r.batteryStatus));
                  else snprintf(extra, sizeof extra, ", battery %s", batteryStatusName(r.batteryStatus)); break; }
        default: snprintf(r.pageName.empty() ? b : b, sizeof b, "Page %d", r.page); r.pageName = b; break;
        }
        if (r.heartRate > 0) snprintf(b, sizeof b, "%d bpm, beat %d at %.3f s%s", r.heartRate, r.beatCount, r.beatTimeSec, extra);
        else snprintf(b, sizeof b, "no heart rate (sensor not on the skin?), beat %d%s", r.beatCount, extra);
        r.text = b;
        break;
    }
    case BikeSpeedCadence:
        r.page = -1; r.toggle = false;   // this profile has no page byte: all 8 bytes are counters
        r.pageName = "Speed and cadence";
        r.cadenceEventTime = u16(p); r.cadenceRevs = u16(p + 2); r.speedEventTime = u16(p + 4); r.speedRevs = u16(p + 6);
        snprintf(b, sizeof b, "cadence %d revs at %.3f s, wheel %d revs at %.3f s", r.cadenceRevs, r.cadenceEventTime / 1024.0, r.speedRevs, r.speedEventTime / 1024.0);
        r.text = b;
        break;
    case BikeCadence: case BikeSpeed: {
        r.pageName = type == BikeCadence ? "Cadence" : "Speed";
        if (type == BikeCadence) { r.cadenceEventTime = u16(p + 4); r.cadenceRevs = u16(p + 6); }
        else { r.speedEventTime = u16(p + 4); r.speedRevs = u16(p + 6); }
        char extra[100] = "";
        if (r.page == 1) { r.operatingTimeSec = (double)(p[1] | p[2] << 8 | p[3] << 16) * 2; snprintf(extra, sizeof extra, ", on for %.1f h", r.operatingTimeSec / 3600); }
        else if (r.page == 2) { r.manufacturer = p[1]; r.serial = u16(p + 2); snprintf(extra, sizeof extra, ", %s serial %d", manufacturerName(r.manufacturer)[0] ? manufacturerName(r.manufacturer) : "manufacturer", r.serial); }
        else if (r.page == 3) { r.hwRev = p[1]; r.swRev = p[2]; r.model = p[3]; snprintf(extra, sizeof extra, ", hw %d sw %d model %d", r.hwRev, r.swRev, r.model); }
        snprintf(b, sizeof b, "%d revs at %.3f s%s", type == BikeCadence ? r.cadenceRevs : r.speedRevs, (type == BikeCadence ? r.cadenceEventTime : r.speedEventTime) / 1024.0, extra);
        r.text = b;
        break;
    }
    case BikePower:
        if (r.page == 0x10) {
            r.pageName = "Power";
            r.powerEvents = p[1]; r.pedalPowerPct = p[2] == 0xFF ? -1 : (p[2] & 0x7F); r.instCadenceRpm = p[3] == 0xFF ? -1 : p[3];
            r.accumPowerW = u16(p + 4); r.instPowerW = u16(p + 6);
            snprintf(b, sizeof b, "%d W%s%s", r.instPowerW, r.instCadenceRpm >= 0 ? (", " + std::to_string(r.instCadenceRpm) + " rpm").c_str() : "",
                     r.pedalPowerPct >= 0 ? (", " + std::to_string(r.pedalPowerPct) + "% " + ((p[2] & 0x80) ? "right" : "pedal")).c_str() : "");
            r.text = b;
        } else if (r.page == 0x12) { r.pageName = "Crank torque"; r.instCadenceRpm = p[3] == 0xFF ? -1 : p[3]; snprintf(b, sizeof b, "crank torque page, %d rpm", r.instCadenceRpm); r.text = b; }
        else if (r.page == 0x11) { r.pageName = "Wheel torque"; r.instCadenceRpm = p[3] == 0xFF ? -1 : p[3]; snprintf(b, sizeof b, "wheel torque page, %d rpm", r.instCadenceRpm); r.text = b; }
        else if (r.page == 0x01) { r.pageName = "Calibration"; snprintf(b, sizeof b, "calibration id 0x%02X", p[1]); r.text = b; }
        else if (r.page == 0x02) { r.pageName = "Get/set parameters"; r.text = "parameters page"; }
        else commonPage(r, p, b, sizeof b);
        break;
    case StrideSdm:
        if (r.page == 1) {
            r.pageName = "Speed and distance";
            r.sdmDistanceM = p[3] + (p[4] >> 4) / 16.0;
            r.sdmSpeedMs = (p[4] & 0x0F) + p[5] / 256.0;
            r.strideCount = p[6];
            snprintf(b, sizeof b, "%.2f m/s (%.1f km/h), distance %.2f m, strides %d", r.sdmSpeedMs, r.sdmSpeedMs * 3.6, r.sdmDistanceM, r.strideCount);
            r.text = b;
        } else if (r.page == 2) {
            r.pageName = "Cadence";
            r.sdmCadenceSpm = p[3] + (p[4] >> 4) / 16;
            r.sdmSpeedMs = (p[4] & 0x0F) + p[5] / 256.0;
            snprintf(b, sizeof b, "%d strides a minute, %.2f m/s", r.sdmCadenceSpm, r.sdmSpeedMs);
            r.text = b;
        } else commonPage(r, p, b, sizeof b);
        break;
    case Environment:
        if (r.page == 1) {
            r.pageName = "Temperature";
            const int16_t cur = (int16_t)u16(p + 6);
            r.temperatureC = cur / 100.0;
            const int lo = (p[3] | (p[4] & 0x0F) << 8), hi = ((p[4] >> 4) | p[5] << 4);
            const double lo24 = (lo & 0x800 ? lo - 0x1000 : lo) / 10.0, hi24 = (hi & 0x800 ? hi - 0x1000 : hi) / 10.0;
            snprintf(b, sizeof b, "%.2f C (24 h low %.1f, high %.1f)", r.temperatureC, lo24, hi24);
            r.text = b;
        } else commonPage(r, p, b, sizeof b);
        break;
    default:
        commonPage(r, p, b, sizeof b);
        if (r.text.empty()) { snprintf(b, sizeof b, "page %d: %s", r.page, hex(p + 1, 7).c_str()); r.pageName = "Page " + std::to_string(r.page); r.text = b; }
        break;
    }
    if (r.pageName.empty()) r.pageName = "Page " + std::to_string(r.page);
    return r;
}

std::string hex(const uint8_t* p, size_t n) { return dect2::str::toHex(p, n, true, ' '); }

} // namespace ant
} // namespace dect2
