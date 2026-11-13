// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Several single-programme transport streams into one multi-programme multiplex (issue #492): the transmitter's Multiplex takes the
// streams of Digital video generators as its services.
//
// Each input keeps a fixed share of the output (its own mux rate over the channel's), so its packets keep their spacing and its PCRs
// stay right without restamping. Its PIDs are moved to a block of its own (PMT 0x1000 + k, streams 0x100 * (k + 1) + j), its PMT is
// rewritten with the new programme number and PIDs (descriptors kept), and the remux sends its own PAT and SDT. The inputs' PAT, SDT,
// other SI and null packets leave free slots: the new PAT / SDT go there, else a null packet.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace dect2 {

struct TsRemuxInput {
    std::function<bool(uint8_t*)> pop;    // the next queued 188 byte packet; false when none is waiting
    std::function<double()> rate;         // its transport-stream bit rate in bit/s (null packets included); 0: not running
    std::function<size_t()> queued;       // packets waiting (optional): a growing queue gets a slightly larger share
    std::function<void()> flush;          // drop what is queued (optional): when the input starts, so it does not start seconds late
    std::string name;                     // service name in the SDT; empty: the input's own
    std::string provider;                 // provider name; empty: the input's own
};

struct TsRemuxConfig {
    double outBitrate = 0;    // the channel's transport-stream rate, bit/s
    int firstServiceId = 1;   // input k is programme firstServiceId + k
    int tsid = 1;
    int onid = 0xFF01;
};

class TsRemux {
public:
    TsRemux(std::vector<TsRemuxInput> in, const TsRemuxConfig& c);
    void next(uint8_t* pk);   // one output packet

    static int pmtPid(int k) { return 0x1000 + k; }
    static int esPidBase(int k) { return 0x100 * (k + 1); }

private:
    struct In {
        TsRemuxInput src;
        bool active = false;          // running (rate > 0)
        int pmtIn = -1;               // the input's PMT PID, from its PAT
        bool pmtSeen = false;
        std::vector<std::pair<int, int>> map;   // input PID -> output PID
        std::vector<uint8_t> lastPmt;           // the last rewritten PMT section, version and CRC zeroed: a change bumps the version
        int pmtVersion = 0;
        std::string sdtName, sdtProvider;
        int serviceType = 1;
        double credit = 0;
    };
    bool fromInput(In& in, int k, uint8_t* pk);   // false: the slot is free
    bool rewritePmt(In& in, int k, const uint8_t* sec, int len, uint8_t* pk, int cc);   // false: not usable, the slot is free
    void readSdt(In& in, const uint8_t* sec, int len);
    void queueTables();
    void queueSection(int pid, const std::vector<uint8_t>& sec, int& cc);

    std::vector<In> in_;
    TsRemuxConfig cfg_;
    std::deque<std::array<uint8_t, 188>> psi_;   // PAT / SDT packets waiting for a free slot
    uint64_t count_ = 0;
    uint64_t patEvery_ = 1, sdtEvery_ = 1, nextPat_ = 0, nextSdt_ = 0;
    int patCc_ = 0, sdtCc_ = 0, patVersion_ = 0, sdtVersion_ = 0;
    std::vector<uint8_t> lastPat_, lastSdt_;
};

} // namespace dect2
