// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Several transport streams into one multi-programme multiplex: the Digital video generator's Muxing tab puts its sources together
// with it (#458), and OnAir's transmitter puts several generators together (#492).
//
// Each input keeps a fixed share of the output (its own mux rate over the channel's), so its packets keep their spacing and its PCRs
// stay right without restamping. The remux sends its own PAT and SDT; the inputs' PAT, SDT, other SI and null packets leave free
// slots: the new PAT / SDT go there, else a null packet.
//
// Numbering, per input:
//  - automatic (the #494 default): the input's (first) programme becomes programme firstServiceId + k, PMT PID 0x1000 + k, its
//    streams 0x100 * (k + 1) + j (the PCR, when it has a PID of its own, 0x100 * (k + 1) + 0xFF);
//  - set (serviceId / pmtPid / esPids, the generator's Advanced Muxing tab): those values, the rest automatic;
//  - kept (keepIds, a generator's whole multiplex in the transmitter): every programme of the input with its own programme number,
//    PIDs and SDT names. A PID or programme number an earlier input already uses is moved (to an automatic block / the next free number).
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace dect2 {

struct TsRemuxInput {
    std::function<bool(uint8_t*)> pop;    // the next queued 188 byte packet; false when none is waiting
    std::function<double()> rate;         // its transport-stream bit rate in bit/s (null packets included); 0: not running
    std::function<size_t()> queued;       // packets waiting (optional): a growing queue gets a slightly larger share
    std::function<void()> flush;          // drop what is queued (optional): when the input starts, so it does not start seconds late
    std::string name;                     // service name in the SDT; empty: the input's own (not for keepIds)
    std::string provider;                 // provider name; empty: the input's own
    int serviceId = 0;                    // programme number / service ID; 0: firstServiceId + k
    int pmtPid = 0;                       // 0: 0x1000 + k
    std::vector<int> esPids;              // the streams in the order of the input's PMT; missing or 0: automatic
    bool keepIds = false;                 // carry every programme of the input as it is (see above)
};

struct TsRemuxConfig {
    double outBitrate = 0;    // the channel's transport-stream rate, bit/s
    int firstServiceId = 1;   // input k is programme firstServiceId + k (automatic numbering)
    int tsid = 1;
    int onid = 0xFF01;
};

// A programme in the output (for the window and the tests)
struct TsRemuxProgramme {
    int input = 0, serviceId = 0, pmtPid = 0;
    std::vector<int> esPids;
    std::string name;
};

class TsRemux {
public:
    TsRemux(std::vector<TsRemuxInput> in, const TsRemuxConfig& c);
    void next(uint8_t* pk);   // one output packet
    std::vector<TsRemuxProgramme> programmes() const;   // what the PAT carries now
    uint64_t packetsOut(size_t input) const { return input < in_.size() ? in_[input].out : 0; }

    static int pmtPid(int k) { return 0x1000 + k; }
    static int esPidBase(int k) { return 0x100 * (k + 1); }

private:
    struct Prog {
        int progIn = 0;                         // its programme number in the input
        int pmtIn = -1;                         // its PMT PID in the input
        bool pmtSeen = false;
        int sid = 0, pmtOut = 0;                // in the output
        std::vector<std::pair<int, int>> map;   // input PID -> output PID
        std::vector<uint8_t> lastPmt;           // the last rewritten PMT section, version and CRC zeroed: a change bumps the version
        int pmtVersion = 0;
        int autoIdx = -1;                       // keepIds: moved to the automatic block of this index (-1: its own PIDs)
    };
    struct SdtName { std::string name, provider; int type = 1; };
    struct In {
        TsRemuxInput src;
        bool active = false;                    // running (rate > 0)
        std::vector<Prog> progs;
        std::map<int, SdtName> sdt;             // the input's SDT, by its programme number
        double credit = 0;
        uint64_t out = 0;
    };
    bool fromInput(In& in, int k, uint8_t* pk);   // false: the slot is free
    void readPat(In& in, const uint8_t* sec, int len);
    bool rewritePmt(In& in, int k, Prog& pg, const uint8_t* sec, int len, uint8_t* pk, int cc);   // false: not usable, the slot is free
    void readSdt(In& in, const uint8_t* sec, int len);
    // used by another programme: one in the output, or one an earlier kept input announced in its PAT (it has priority)
    bool pidTaken(int pid, const Prog* self, int selfInput) const;
    bool sidTaken(int sid, const Prog* self, int selfInput) const;
    void queueTables();
    void queueSection(int pid, const std::vector<uint8_t>& sec, int& cc);

    std::vector<In> in_;
    TsRemuxConfig cfg_;
    std::deque<std::array<uint8_t, 188>> psi_;   // PAT / SDT packets waiting for a free slot
    uint64_t count_ = 0;
    uint64_t patEvery_ = 1, sdtEvery_ = 1, nextPat_ = 0, nextSdt_ = 0;
    int patCc_ = 0, sdtCc_ = 0, patVersion_ = 0, sdtVersion_ = 0;
    int moved_ = 0;                              // programmes of kept inputs moved to an automatic block
    std::vector<uint8_t> lastPat_, lastSdt_;
};

} // namespace dect2
