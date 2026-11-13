// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Iridium receiver telemetry, shared by the receiver and the UI. Every vector is capped; a report stays well below 100 kB.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace dect2 {

// Frame type counters: the index is (int)IridiumType of iridium_frame.h (0 = not recognised).
constexpr int kIridiumTypeSlots = 13;
inline const char* iridiumTypeLabel(int i) {
    static const char* n[kIridiumTypeSlots] = {"other", "IRA", "IBC", "ISY", "ITL", "MSG", "IDA", "IIP", "IIQ", "IIR", "IIU", "IMS", "voice"};
    return i >= 0 && i < kIridiumTypeSlots ? n[i] : "?";
}

struct IridiumSatInfo {
    int id = -1;                     // satellite number as the frames carry it (Iridium's internal number, 0..127)
    uint64_t frames = 0;             // frames heard from it
    double lastHeard = 0;            // receiver time, seconds
    double freqOffsetHz = 0;         // offset of its last burst from the channel grid (Doppler plus the radio's error)
    bool hasPos = false;             // sub-point from a ring alert with a satellite position
    double lat = 0, lon = 0, altKm = 0;
    std::vector<int> beams;          // beam ids heard, ascending (at most 48)
};

struct IridiumRingAlert {            // IRA
    double time = 0;                 // receiver time, seconds
    int sat = -1, beam = -1;
    bool hasPos = false;
    double lat = 0, lon = 0, altKm = 0;   // as the frame carries it: the satellite (hundreds of km) or the beam centre (near 0)
    int paged = 0;                   // TMSIs paged
    double freqHz = 0;
    std::vector<uint32_t> tmsis;     // the paged temporary identities (at most 12 per alert)
};

struct IridiumIbcInfo {              // IBC
    double time = 0;
    int sat = -1, beam = -1;
    bool hasTime = false;
    double unixTime = 0;             // Iridium time converted to UTC seconds since 1970
};

struct IridiumPagerMsg {             // MSG, assembled over its parts
    double time = 0;
    int ric = -1, seq = -1;
    std::string text;
    bool complete = false;
    int fmt = -1;                    // 5 alphanumeric, 3 numeric, -1 unknown
};

struct IridiumAcarsMsg {             // ACARS carried in short burst data (IDA frames joined), Phase B + extra
    double time = 0;
    bool downlink = true;            // Iridium direction: true = from the satellite (to the aircraft)
    char mode = 0, ack = 0, blockId = 0;
    std::string reg, label, seq, flight, text;
    bool more = false;               // another block follows (ETB) and has not arrived
    // added by the ACARS layer (iridium_acars.h)
    uint64_t serial = 0;             // grows by one per listed message
    int64_t wall = 0;                // wall clock when decoded
    double freqHz = 0;
    int regKind = 0;                 // 0 none, 1 tail number, 2 24-bit ICAO address in hex, 3 other
    uint32_t icao = 0;               // when regKind is 2, or an ADS-C airframe group gave it
    int crc = 0;                     // block check: 0 not present, 1 matches, 2 does not match
    int parity = 0;                  // 0 no parity bits used, 1 odd parity all right, 2 parity errors
    int blocks = 1;                  // blocks joined into this message
    int repeats = 0;                 // retransmissions of the same block that were dropped
    std::string labelText;           // what the label means, "" when unknown
    std::string decoded;             // OOOI times and airports, ADS-C groups in words
    bool hasPos = false;             // a position report from the aircraft (aero_pos.h)
    double lat = 0, lon = 0;
    bool hasAlt = false;
    int altFt = 0;
    std::string posKind;
    double updated = 0;              // receiver time of the last block joined into it
    std::vector<std::string> blockKeys;   // the blocks seen (retransmission check), at most 8
};

struct IridiumAircraft {             // an aircraft heard on Iridium ACARS
    std::string reg;                 // as the messages give it (dots removed)
    std::string flight;
    uint32_t icao = 0;
    std::string lastLabel;
    uint32_t messages = 0;
    double freqHz = 0;
    double lastHeard = 0;            // receiver time, seconds
    bool hasPos = false;
    double lat = 0, lon = 0;
    bool hasAlt = false;
    int altFt = 0;
    bool hasTrack = false;
    double trackDeg = 0;
    bool hasSpeed = false;
    double speedKt = 0;
    std::string posKind;
    double posTime = 0;              // receiver time
    int64_t posWall = 0;
    uint32_t positions = 0;
    std::vector<std::pair<double, double>> track;   // oldest first, at most 24
};

struct IridiumPacketRec {            // a joined IDA packet (short burst data), for the packet dump
    uint64_t serial = 0;
    double time = 0, freqHz = 0;
    int64_t wall = 0;
    bool downlink = true;
    int fragments = 0;
    bool complete = true;
    int sbdType = -1;                // 0x0600, 0x76xx, -1 not short burst data
    uint32_t length = 0;             // bytes of the whole packet
    std::string hex;                 // first 256 bytes
    std::string status;              // "ACARS new", "ACARS repeat", "ACARS joined", "SBD", "not SBD", "incomplete"
    uint64_t acarsSerial = 0;        // the listed ACARS message, 0 when none
};

struct IridiumFrameRec {             // one demodulated data frame (IDA, IIP, IIQ, IIR, IIU, IMS, ITL, MSG), for the packet dump
    uint64_t serial = 0;
    double time = 0, freqHz = 0;
    int64_t wall = 0;
    bool downlink = true;
    uint8_t type = 0;                // IridiumType
    bool ok = false;                 // codes checked out
    bool crcOk = false;
    int corrected = 0;
    int ctr = -1;                    // IDA counter
    bool cont = false;               // IDA: another part follows
    uint32_t length = 0;             // payload bytes
    std::string hex;                 // first 64 bytes
    uint64_t packet = 0;             // IridiumPacketRec::serial it was joined into, 0 = not (yet) assembled
    std::string status;              // "ok", "code failed", "crc failed", "ok, no start"
};

struct IridiumMapPoint {             // positions for the map, from ring alerts
    double time = 0;
    int sat = -1, beam = -1;
    float lat = 0, lon = 0, altKm = 0;
    bool satellite = false;          // true: the satellite's position; false: a beam centre on the ground
};

struct IridiumBurstDot {             // burst scatter: the last 2 s
    float age = 0;                   // seconds before the report (0 .. 2)
    float freqKHz = 0;               // relative to the tuned centre
    uint8_t kind = 0;                // 0 detected, 1 unique word found, 2 frame decoded
};

struct IridiumCall {                 // a voice call: the traffic frames on one channel, one direction
    int id = 0;                      // 1, 2, ... by start
    double start = 0, last = 0;      // receiver time, seconds
    double freqHz = 0;               // last burst frequency
    bool downlink = true;
    bool active = false;             // frames within the last second
    int sat = -1, beam = -1;         // from a frame heard on this frequency just before; -1 unknown
    uint32_t frames = 0;             // voice frames
    uint32_t readable = 0;           // frames whose link control word checked out
    uint32_t decoded = 0;            // frames the codec plug-in turned into sound
    double audioSec = 0;             // length of the sound
    std::string status;              // "decoded", "codec needed", "not readable"
    std::string file;                // the recording, when saved (never a silent one)
    std::string warning;             // plain-words reason when there is no sound or no file (codec missing, plug-in returns silence, write failed)
    int64_t wallEnd = 0;             // Unix time the recording was saved (0 = not saved)
    uint32_t silent = 0;             // frames the network marks as silence (link control word 'silent'): never sent to the codec
    uint32_t dataSlots = 0;          // voice slots carrying data (Reed-Solomon / CRC passed, the toolkit's VO6 VOD VOZ): not speech, not sent to the codec
    uint32_t emptyFrames = 0;        // frames the codec plug-in refused (returned no sound)
    uint32_t gapFrames = 0;          // 90 ms frames of silence inserted for lost bursts
    float level = 0;                 // peak of the last decoded frame, 0..1
    bool mute = false, solo = false; // mixer: a soloed call is heard alone; a muted call is recorded but not played
    bool audible = false;            // played now (listen choice, mute and solo applied)
};

struct IridiumTelemetry {
    uint64_t seq = 0;            // grows with every report
    int state = 0;               // 0 searching, 1 signal (bursts with a unique word), 2 decoding (frames with good codes)
    double cfoHz = 0;            // offset of the last good burst from the channel grid: satellite Doppler plus the radio's error
    float snrDb = 0;             // mean Es/N0 of the bursts with a unique word over the last report
    bool dataValid = false;      // a frame has been decoded since the start or the last retune
    uint64_t blocksOk = 0;       // frames whose codes checked out
    uint64_t blocksBad = 0;      // bursts with a unique word whose frame failed its checks (voice not counted)

    double timeSec = 0;          // receiver time of this report (seconds of signal since the start or the last retune)
    double inputRate = 0;        // sample rate
    double centerMhz = 0;        // tuned centre the receiver assumes (setCenterMhz)
    // bursts: totals and per second over the last report
    uint64_t bursts = 0, demodulated = 0, uwOk = 0, dropped = 0, downlink = 0, uplink = 0;
    uint64_t duplicates = 0;     // a strong burst found twice (or its spectral skirt): counted once
    float burstsPerSec = 0, demodPerSec = 0, uwPerSec = 0, framesPerSec = 0;
    float confidence = 0;        // mean demodulator confidence (percent of symbols within 22 degrees) over the last report
    uint64_t typeCount[kIridiumTypeSlots] = {};
    uint64_t voiceFrames = 0;    // voice slots heard (iridium_voice.h: grouped into calls; decoded only through the user's plug-in)
    std::vector<IridiumCall> calls;            // newest last, at most 100
    uint64_t pagedTotal = 0;     // TMSIs paged in all ring alerts heard
    // time from the broadcast channel
    bool hasTime = false;
    double iridiumUtc = 0;       // UTC seconds since 1970 at timeSec, from the last IBC time
    std::vector<IridiumSatInfo> sats;          // at most 128, by id
    std::vector<IridiumRingAlert> ringAlerts;  // newest last, at most 100
    std::vector<IridiumIbcInfo> ibc;           // newest last, at most 32
    std::vector<IridiumPagerMsg> messages;     // newest last, at most 100 (complete, and incomplete ones that timed out)
    std::vector<IridiumMapPoint> positions;    // newest last, at most 500
    std::vector<IridiumBurstDot> scatter;      // at most 2000
    uint64_t sbdPackets = 0;                   // IDA packets joined (short burst data)
    std::vector<IridiumAcarsMsg> acars;        // newest last, at most 200
    std::vector<IridiumAircraft> aircraft;     // most recently heard last, at most 100
    std::vector<IridiumPacketRec> packets;     // newest last, at most 300
    std::vector<IridiumFrameRec> frameDump;    // newest last, at most 600
    uint64_t acarsSerial = 0;                  // serial of the newest listed ACARS message
    uint64_t acarsDuplicates = 0;              // retransmitted blocks dropped
    uint64_t acarsCrcBad = 0, acarsParityBad = 0;
};

inline std::string iridiumSummary(const IridiumTelemetry& t) {
    if (t.state == 0) return "Iridium: searching";
    char b[200];
    snprintf(b, sizeof b, "Iridium: %s, %.0f bursts/s, %.0f frames/s, %zu satellites, %llu ring alerts, %zu messages", t.state == 1 ? "signal" : "decoding",
             t.burstsPerSec, t.framesPerSec, t.sats.size(), (unsigned long long)t.typeCount[1], t.messages.size());
    return b;
}

} // namespace dect2
