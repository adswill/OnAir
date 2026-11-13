// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ANT / ANT+ air frames and the public ANT+ device profiles (heart rate, bike speed and cadence, bike power, stride-based speed and
// distance, environment). Receive only: OnAir never transmits, pairs or acknowledges.
//
// The air format is Nordic ShockBurst, as sniffed with SDRs (the format is not published by Garmin; two independent reverse-engineering
// efforts agree): 1 Mbit/s GFSK, about 160 kHz deviation, bytes most significant bit first:
//   preamble 0xAA or 0x55 (whichever alternates with the first bit of the next byte) | network marker, 2 bytes (0xA6 0xC5 for the ANT+
//   network) | device number, low byte then high | device type | transmission type | length 0x0A (8 payload + 2 CRC) | 8 payload bytes |
//   CRC-16 (poly 0x1021, init 0xFFFF, no reflection) over the marker through the payload, high byte first.
// ANT+ devices sit on 2457 MHz; other ANT networks use any 1 MHz channel from 2400 to 2524 MHz with their own marker.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace ant {

constexpr int kFrameBytes = 17;          // marker (2) + channel id (4) + length (1) + payload (8) + CRC (2)
constexpr int kPayload = 8;
constexpr uint16_t kAntPlusMarker = 0xA6C5;
constexpr double kChannelMhz = 2457.0;   // the ANT+ channel (RF channel 57)
constexpr double kDevHz = 160e3;         // nominal GFSK deviation
constexpr double kHrmPeriodSec = 8070.0 / 32768;   // the heart rate profile's message period (about 4.06 a second)

uint16_t crc16(const uint8_t* p, size_t n);          // CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF

struct Frame {
    uint16_t marker = 0;                 // network marker (0xA6C5: ANT+)
    uint16_t deviceNumber = 0;
    uint8_t deviceType = 0;              // bit 7: the pairing request bit; the type is the low 7 bits
    uint8_t transType = 0;
    uint8_t length = 0;
    uint8_t payload[kPayload] = {};
    uint16_t crc = 0;
    bool crcOk = false;
    bool antPlus() const { return marker == kAntPlusMarker; }
    int type() const { return deviceType & 0x7F; }
    bool pairing() const { return (deviceType & 0x80) != 0; }
};

// Parse 17 bytes (after the preamble). Returns false when the length byte is not 0x0A; crcOk says whether the CRC matched.
bool parseFrame(const uint8_t* b, Frame& f);
// Build the 17 bytes of a frame (CRC computed), and the full air bits with the preamble (MSB first), for the generator and the tests
void buildFrame(const Frame& f, uint8_t out[kFrameBytes]);
void frameBits(const Frame& f, std::vector<uint8_t>& bits);

// Device types (the ANT+ device profiles)
enum DeviceType { HeartRate = 120, BikeSpeedCadence = 121, BikeCadence = 122, BikeSpeed = 123, StrideSdm = 124, BikePower = 11,
                  FitnessEquipment = 17, MultiSportSdm = 15, WeightScale = 119, Controls = 16, Environment = 25, Lev = 20,
                  BloodPressure = 18, Geocache = 19, MuscleOxygen = 31, BikeRadar = 40, BikeLights = 35, Shifting = 34 };
const char* deviceTypeName(int type);    // "Heart rate monitor", ... or "ANT device"
const char* manufacturerName(int id);    // the ANT+ manufacturer ids that matter in practice (Garmin, Wahoo, ...) or ""
const char* batteryStatusName(int s);

// What a payload says, for the profile of the device type. Fields not in this page stay unset (-1 / NaN).
struct Reading {
    int page = -1;                       // data page number (without the toggle bit)
    bool toggle = false;                 // the page toggle bit (heart rate: a legacy / modern sender tell)
    std::string pageName;                // "Heart rate", "Manufacturer", ... what the page carries
    std::string text;                    // the page's fields in words: "72 bpm, beat 143, 12.5 s", "Garmin, serial 4321"
    // heart rate
    int heartRate = -1;                  // bpm (0: invalid)
    int beatCount = -1;                  // the cumulative beat count byte
    double beatTimeSec = -1;             // the last beat's event time within the 64 s cycle
    double rrIntervalSec = -1;           // from page 4 (previous beat time), when sent
    // cycling
    int cadenceEventTime = -1, cadenceRevs = -1, speedEventTime = -1, speedRevs = -1;   // raw counters (1/1024 s, revolutions)
    int instPowerW = -1, instCadenceRpm = -1, accumPowerW = -1, powerEvents = -1, pedalPowerPct = -1;
    // stride-based (footpod)
    double sdmSpeedMs = -1, sdmDistanceM = -1;
    int strideCount = -1, sdmCadenceSpm = -1;
    // environment
    double temperatureC = -1000;
    // common pages
    int manufacturer = -1, serial = -1, hwRev = -1, swRev = -1, model = -1;
    double operatingTimeSec = -1;
    int batteryPct = -1, batteryStatus = -1;
    double batteryV = -1;
};
Reading decodePayload(int deviceType, const uint8_t payload[kPayload]);

std::string hex(const uint8_t* p, size_t n);

} // namespace ant
} // namespace dect2
