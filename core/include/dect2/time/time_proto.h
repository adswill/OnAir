// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Time signal stations: the minute frames of the amplitude time codes, the stations' sites and the propagation delay.
//
// Each second of a minute is one symbol. LF stations key the carrier: DCF77 (77.5 kHz, Mainflingen) drops it to 15 % for 100 ms (0) or
// 200 ms (1), not at all in second 59; MSF (60 kHz, Anthorn) switches it off for 100 ms, then for bit A 100 - 200 ms and bit B 200 - 300 ms,
// 500 ms at the minute marker; WWVB (60 kHz, Fort Collins) lowers it 17 dB for 200 (0), 500 (1) or 800 ms (marker); JJY (40 kHz Ohtakadoya,
// 60 kHz Hagane) keeps it high for 800 (0), 500 (1) or 200 ms (marker), low the rest. WWV / WWVH (2.5 - 20 MHz) send the same BCD frame as a
// 100 Hz subcarrier (200 / 500 / 800 ms pulses, none in second 0) with 5 ms ticks at 1000 Hz (WWV) or 1200 Hz (WWVH). CHU (3.33 / 7.85 /
// 14.67 MHz) sends 300 bit/s Bell 103 FSK frames in seconds 31 - 39 (decoded here: the A frames with day of year and time).
//
// DCF77 and MSF announce the minute that begins at the next minute marker; WWVB, JJY and WWV the minute their frame starts. decodeMinute()
// returns the UTC start of the minute the symbols were received in either way.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace timesig {

enum Station { Dcf77 = 0, Msf, Wwvb, Jjy40, Jjy60, Wwv, Chu, Bpm, Bpc, Hla, BpmC, Rwm, kNumStations };
const char* stationName(int s);      // "DCF77", "MSF", "WWVB", "JJY 40 kHz", "JJY 60 kHz", "WWV / WWVH", "CHU", "BPM", "BPC", "HLA", "BPM coded", "RWM"
double stationKhz(int s);            // the usual frequency (WWV: 10 MHz, CHU: 7.85 MHz, BPM and BPM coded: 5 MHz, BPC 68.5 kHz, HLA 5 MHz)
bool stationIsHf(int s);             // WWV / WWVH, CHU, BPM, HLA, BPM coded and RWM: HF stations, sky wave (BPC is LF)
bool stationKeysCarrier(int s);      // the time code keys the carrier itself (the LF stations and RWM), not an audio tone on AM
int stationForKhz(double khz, int pref60);   // the station of a frequency (pref60: which of MSF / WWVB / JJY 60 at 60 kHz), -1 none
const char* stationTimeZone(int s);  // the time the frame carries: "CET / CEST", "UK", "UTC", "JST"
// BPM (Pucheng, China; 2.5 / 5 / 10 / 15 MHz, the same frequencies as WWV): 1 kHz ticks of 10 ms, a pulse-width coded second after each tick
// (100 ms 0, 200 ms 1, 300 ms marker; the long tone of the minute is the marker of second 0), the WWVB frame layout, UTC. In the minutes the
// station sends UT1 the ticks follow UT1 (Fields::ut1). Its frequencies are WWV's: pick the station by hand.

// BPC (Shangqiu, China, 68.5 kHz, UTC+8, needs a radio that tunes LF or an upconverter): the carrier drops for 0.1 - 0.4 s at each second, 2 bits
// (00, 01, 10, 11, MSB first); no drop in second 0 of a 20 s frame (P0), three frames a minute (decodeMinute reads one frame: 20 symbols of
// 0 - 3, SNone first; Fields::frameIndex and utcMinute = the frame's start). Hour is 12-hour like with AM / PM in P3, even parity over seconds
// 1 - 9 (P3) and 11 - 18 (P4), the year's high bit in P4.
// HLA (Korea, 5 MHz, UTC+9): 100 Hz subcarrier, 1 bit a second: pulses of 0.17 s (0), 0.47 s (1), 0.77 s (marker P1..P5 at seconds 9, 19, ..., 49
// and P0 at 59), second 0 empty with a 0.8 s 1800 Hz tone, 6 ms 1800 Hz ticks 32 ms before each second. BCD minute, hour, day of year, no year, no parity.
// BPM coded (BPMc, 2.5 / 5 MHz since 2019, in the pure carrier minutes): 10 ms code elements of 1 kHz, five cycles = 1. Each second S (1) + an empty
// element, then sec, min, hour, day, month, year (LSB first, an odd parity bit after each) and DUT1 in ms; second 0 sends M (300 ms) and the
// 0.1 ms DUT1 digit, the leap count and the leap second flag. Elements are plain 0 / 1 bytes below.

// RWM (Moscow, 4996 / 9996 / 14996 kHz, UTC): the carrier itself is keyed (A1N), no time of day is sent. In the half hour: minutes 0 - 7
// plain carrier, 8 off, 9 the call sign in Morse, 10 - 19 a 100 ms pulse at each second (500 ms at the minute), 20 - 29 10 pulses a second
// (20 ms, 40 ms on the second, 500 ms at the minute). DUT1 and dUT1 by doubled pulses (CCIR / ITU-R TF.460 code: a second pulse 200 - 300 ms
// after the second): DUT1 +n doubles seconds 1 .. n, -n seconds 9 .. 8 + n (0.1 s steps); dUT1 +n seconds 21 .. 20 + n, -n 31 .. 30 + n
// (0.02 s steps). Symbols: S0 a single pulse, S1 a doubled one, SB a second of the 10 pulse minutes, SMark the minute pulse. decodeMinute()
// gives only DUT1 / dUT1 (utcMinute 0): the minute itself comes from the computer clock.

// Symbols, one a second. bit 0 is the bit (MSF: bit A), bit 1 MSF bit B.
enum Sym : uint8_t { S0 = 0, S1 = 1, SB = 2, SAB = 3, SMark = 4, SNone = 5, SUnknown = 6 };

struct Fields {
    int year = 0, month = 0, day = 0, doy = 0, hour = 0, minute = 0;   // of the frame's own time (local time for DCF77, MSF, JJY)
    int weekday = -1;               // 1 Monday .. 7 Sunday, -1 not sent
    int utcOffsetMin = 0;           // the frame's time minus UTC
    int dst = -1;                   // summer time in effect: 1, 0, -1 not sent
    int dstCode = -1;               // WWVB / WWV: 0 standard time, 1 DST ends today, 2 begins today, 3 in effect
    bool dstAnnounce = false;       // DCF77 A1, MSF 53B: the change at the end of this hour
    bool leapAnnounce = false;      // a leap second at the end of this month (DCF77 A2) or day (WWVB, WWV, JJY)
    int leapSign = 0;               // JJY: +1 insert, -1 delete
    int dut1 = 99;                  // UT1 - UTC in 0.1 s (99 not sent)
    int dUt1 = 99;                  // RWM: the extra UT1 - UTC in 0.02 s (99 not sent)
    bool callBit = false;           // DCF77 R: the transmitter runs on its reserve antenna
    bool leapYear = false;          // WWVB
    bool ut1 = false;               // BPM: the ticks of this minute follow UT1, not UTC
    int64_t utcMinute = 0;          // UTC epoch second of the minute the symbols were received in (BPC: of the frame, a multiple of 20 s)
    int second = -1;                // BPM coded: the second of the minute this frame names; BPC: the frame's first second (0, 20, 40)
    int frameIndex = -1;            // BPC: 0 - 2
    bool dut1Neg = false;           // BPM coded: the sign bit says UT1 is behind UTC (also for 0 ms)
    bool hasDut1Ms = false;         // BPM coded: dut1Ms is valid
    double dut1Ms = 0;              // BPM coded: UT1 - UTC in milliseconds (the 0.1 ms digit once a minute frame was read)
    int dut1Tenth = -1;             // BPM coded: the 0.1 ms digit of the minute frame, -1 not read yet
    int leapCount = -1;             // BPM coded: TAI - UTC in seconds from the minute frame, -1 not read
    bool parityOk = true;           // BPC, BPM coded: every parity bit checked
};

// Encode the minute that starts at UTC epoch second utcMinute (a multiple of 60): the symbols of its seconds 0 - 59.
struct EncodeOpts { bool leapAnnounce = false; int leapSign = 1; int dut1 = 0; int dUt1 = 0; bool callBit = false; bool ut1 = false;
                    double dut1Ms = -35.7; int leapCount = 37; bool leapAtMonthEnd = false; };   // the last three: BPM coded
std::vector<uint8_t> encodeMinute(int station, int64_t utcMinute, const EncodeOpts& o = {});
// Decode the 60 symbols of a received minute (second 0 first). False when a fixed bit, a parity, a range or a marker is wrong.
// yearHint: the year two-digit years are placed near (0: the computer's), and JJY's at minutes 15 and 45 (no year sent)
bool decodeMinute(int station, const uint8_t* sym, Fields& f, int yearHint = 0);
// BPM coded: one second frame (52 elements), the minute frame at second 0 (46 elements, M excluded in the input: the 14 data bits are elements 32 - 45)
std::vector<uint8_t> encodeBpmcSecond(int64_t utcSec, double dut1Ms);
std::vector<uint8_t> encodeBpmcMinute(int dut1Tenth, int leapCount, bool leapAtMonthEnd);
bool decodeBpmcSecond(const uint8_t* e, Fields& f, int yearHint = 0);
bool decodeBpmcMinute(const uint8_t* e, int& dut1Tenth, int& leapCount, bool& leapAtMonthEnd);
// BPC: the 20 symbols of the frame that starts at UTC epoch second t (a multiple of 20)
std::vector<uint8_t> encodeBpcFrame(int64_t t);
std::string fieldsText(const Fields& f, int station);   // "2026-10-12 14:03 UTC (16:03 CEST)"

// CHU: the A frame of a second 32 - 39 (10 bytes: day of year, hour, minute, second as BCD digits, sent twice), and its parse
std::vector<uint8_t> chuFrameA(int doy, int hour, int minute, int second);
std::vector<uint8_t> chuFrameB(int year);   // second 31: data then its complement (only its shape is checked when decoding)
bool chuParseA(const uint8_t* b, int& doy, int& hour, int& minute, int& second);
bool chuIsB(const uint8_t* b);

// Calendar (proleptic Gregorian, UTC)
int64_t daysFromCivil(int y, int m, int d);
void civilFromDays(int64_t z, int& y, int& m, int& d);
int weekdayOf(int64_t days);         // 1 Monday .. 7 Sunday
bool euSummerTime(int64_t utc);      // the EU rule (last Sunday of March to last Sunday of October, 01:00 UTC)
bool usDstOnDate(int y, int m, int d);   // the US rule: the date is from the second Sunday of March to the day before the first Sunday of November

// The transmitter sites, the great-circle distance and the delay of the path
void stationSite(int s, double& lat, double& lon, bool hawaii = false);
double distanceKm(double lat1, double lon1, double lat2, double lon2);
// Ground wave for LF (distance / c); for HF a sky wave over hops of up to 4000 km off a 300 km layer. Milliseconds.
double propagationMs(int s, double km);

} // namespace timesig
} // namespace dect2
