// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Polish GUM e-Czas / PCSK225 time frame (225 kHz LW carrier, 50 bit/s PCSK).
// Spec: https://e-czas.gum.gov.pl/wp-content/uploads/2024/06/e-CzasPL-Opis-ramki-czasu-e-Czas-Radio.pdf
#pragma once
#include <cstdint>

namespace dect2::eczas {

constexpr int kFrameBytes = 12;          // 96 bits on air
constexpr int kSyncWord = 0x5555;        // 16-bit sync (MSB first in the frame)
constexpr uint8_t kTimeHeader = 0x60;    // time packet (slot 0 of the minute frame)
constexpr uint8_t kTimeMarker = 0x5;     // 3-bit field after the header (0b101)
constexpr int64_t kEpochUtc = 946684800; // 2000-01-01 00:00:00 UTC
constexpr double kBaud = 50.0;
constexpr double kSlotSec = 3.0;
constexpr int kSlotsPerMinute = 20;
constexpr double kCarrierKhz = 225.0;

enum class DecodeStatus { CrcOk, RsOk, BadCrc, BadRs, WrongType, TooShort };

struct TimePacket {
    int64_t utcSec = 0;       // official time, UTC (3 s x the 30-bit counter since 2000-01-01)
    int64_t localSec = 0;     // utcSec + tzHours (the local civil time, as UTC-looking seconds)
    // 0..3 hours added to UTC for local civil time. TZ0 is the LOW bit, as the table in the GUM PDF v1.0 has it (1 0 = +1 h, 0 1 = +2 h, 1 1 = +3 h):
    // checked on the recordings of 2024-08-07 and 2026-08-13 (summer time, they send 0 1 and the clock of the recording is UTC + 2 h)
    int tzHours = 0;
    uint32_t periods3s = 0;   // raw counter (seconds since epoch / 3)
    uint8_t timeMarker = 0;
    bool leapAnnounce = false;
    bool leapSubtract = false;
    bool dstChangeSoon = false;
    uint8_t txState = 0;      // SK0 is the low bit: 0 normal, 1 (SK0 SK1 = 1 0) planned off for a day, 2 (0 1) for a week, 3 (1 1) for longer
    DecodeStatus status = DecodeStatus::TooShort;
};

const char* txStateText(int s);                    // "normal", "off for a day" ...
// Seconds since the Unix epoch to calendar fields (proleptic Gregorian, no time zone)
void civilFromUnix(int64_t sec, int& y, int& mo, int& d, int& h, int& mi, int& s);

uint8_t crc8Poly07(const uint8_t* in, int len);   // CRC-8, poly 0x07, init 0 (on-air check)

// 12-byte on-air frame (sync..CRC). RS correction is not implemented yet: CRC must match.
bool decodeTimeFrame(const uint8_t frame[kFrameBytes], TimePacket& out);

// Build a valid on-air frame for tests (CRC ok; RS bytes zero).
void encodeTimeFrameForTest(uint32_t periods3s, int tzHours, uint8_t flags, uint8_t txState, uint8_t out[kFrameBytes]);

} // namespace dect2::eczas
