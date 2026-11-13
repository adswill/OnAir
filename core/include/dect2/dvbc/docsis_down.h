// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DOCSIS downstream structure from a J.83 annex B transport stream (PID 0x1FFE): the MAC frames and the clear management messages (SYNC, UCD, MAP, MDD). This shows how
// the cable plant is built. Only headers and these broadcast management messages are read; subscriber data frames are counted, never kept.
// Message numbers and TLV identifiers per CM-SP-MULPIv3.1, table 26.
#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dect2 {

struct DocsisUpstream {
    int channelId = -1, changeCount = 0, miniSlotTicks = 0, dsChannelId = -1, ucdType = 2;
    double symbolRateMsym = 0;   // UCD TLV 1, multiples of 0.16 Msym/s
    double frequencyMhz = 0;     // UCD TLV 2
    int bursts = 0;              // burst descriptors in the message
    uint64_t maps = 0;           // MAP messages for this channel
    uint64_t seen = 0;
};
struct DocsisDownstreamChannel {
    int channelId = -1;
    double frequencyMhz = 0;
    int modulation = -1, annex = -1;     // MDD TLV 1.3: low nibble QAM (0 64-QAM, 1 256-QAM), high nibble annex (0 B, 1 A)
    bool primaryCapable = false;
};
struct DocsisStatus {
    bool present = false;
    uint64_t framesTotal = 0, framesData = 0, framesMgmt = 0, timingSync = 0, hcsBad = 0, lost = 0;
    std::map<int, uint64_t> mgmtByType;
    std::map<int, DocsisUpstream> upstream;              // by channel ID
    std::vector<DocsisDownstreamChannel> downstream;     // from MDD
    uint32_t lastTimestamp = 0;                          // SYNC, 10.24 MHz counter
    int macVersionMax = 0;
    std::string lastMac;                                 // CMTS source MAC as text, from the last management message
};

class DocsisDownstream {
public:
    void feed(const uint8_t* packets, size_t count);     // 188-byte TS packets; only PID 0x1FFE is looked at
    const DocsisStatus& status() const { return st_; }
    void reset() { *this = DocsisDownstream(); }

private:
    void parseFrames();
    void management(const uint8_t* m, size_t n);
    DocsisStatus st_;
    std::vector<uint8_t> buf_;
    int cc_ = -1;
    bool synced_ = false;
};

// Message names, for the table on screen ("SYNC", "UCD", "MAP", "RNG-RSP", ...; numbers not known: nullptr)
const char* docsisMessageName(int type);

} // namespace dect2
