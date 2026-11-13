// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Timeshift: a rolling transport-stream buffer on disk, with a PCR based time map so playback can pause, jump back and forward and return to live.
// The packets are written to segment files (ts_<n>.seg in the timeshift folder); when the buffer holds more than the configured time (default 30
// minutes) or bytes, the oldest segment is deleted. Packets are numbered from 0 for ever, like TsRing, so a position stays valid while it is in the
// buffer. Also PacketSource, the interface the playback feeder reads (a timeshift buffer or a recorded file).
#pragma once
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace dect2 {

// Maps packet numbers to stream time from the PCRs (unwrapped over the 33 bit rollover every 26.5 hours; a jump in the clock continues the timeline).
class PcrTimeline {
public:
    void add(uint64_t packetIdx, double pcrSeconds);       // called for every PCR packet in order; marks are thinned to one per 0.1 s of stream
    void trimBefore(uint64_t packetIdx);                   // forget marks before this packet (keeps the last one before it)
    bool timeAt(uint64_t packetIdx, double& t) const;      // interpolated; false before the first / with no marks
    bool indexAt(double t, uint64_t& packetIdx) const;     // the packet at that stream time (clamped to the first / last mark)
    double firstTime() const { return marks_.empty() ? 0 : marks_.front().t; }
    double lastTime() const { return marks_.empty() ? 0 : marks_.back().t; }
    uint64_t firstIndex() const { return marks_.empty() ? 0 : marks_.front().idx; }
    uint64_t lastIndex() const { return marks_.empty() ? 0 : marks_.back().idx; }
    bool empty() const { return marks_.empty(); }
    void clear() { marks_.clear(); have_ = false; offset_ = 0; }
private:
    struct Mark { uint64_t idx; double t; };
    std::deque<Mark> marks_;
    bool have_ = false;
    double offset_ = 0, lastT_ = 0;
};

class PacketSource {
public:
    virtual ~PacketSource() = default;
    virtual uint64_t first() const = 0;                    // oldest packet still available
    virtual uint64_t end() const = 0;                      // one past the newest
    virtual size_t read(uint64_t idx, uint8_t* out, size_t n) const = 0;   // up to n packets, stops at end() / first()
    virtual bool timeAt(uint64_t idx, double& t) const = 0;
    virtual bool indexAt(double t, uint64_t& idx) const = 0;
    virtual bool growing() const = 0;                      // true for a live buffer (more may arrive)
};

struct TimeshiftConfig {
    std::string dir;
    double maxSeconds = 1800;          // 30 minutes
    uint64_t maxBytes = 0;             // 0 = only the time limit
    size_t segmentPackets = 16384;     // about 3 MB per file
};

class DiskTimeshift : public PacketSource {
public:
    ~DiskTimeshift() override { close(); }
    bool open(const TimeshiftConfig& c);           // creates the folder, removes left-over segments of an earlier run
    void close();                                  // deletes the segments
    bool isOpen() const;
    void clear();                                  // empties the buffer (a new service was selected); numbering continues
    void push(const uint8_t* pkts, size_t n);      // whole 188 byte packets of one service; the PCR is taken from the first PID that carries one
    bool failed() const;
    uint64_t bytes() const;                        // on disk
    double bufferedSeconds() const;
    size_t segmentCount() const;
    TimeshiftConfig config() const;
    void setLimits(double maxSeconds, uint64_t maxBytes);

    uint64_t first() const override;
    uint64_t end() const override;
    size_t read(uint64_t idx, uint8_t* out, size_t n) const override;
    bool timeAt(uint64_t idx, double& t) const override;
    bool indexAt(double t, uint64_t& idx) const override;
    bool growing() const override { return true; }

private:
    struct Seg { uint64_t first = 0, count = 0; std::string path; };
    void rollLocked();
    void trimLocked();
    void removeAllLocked();
    mutable std::mutex mu_;
    TimeshiftConfig cfg_;
    bool open_ = false, failed_ = false;
    std::deque<Seg> segs_;
    FILE* w_ = nullptr;
    mutable FILE* r_ = nullptr;
    mutable size_t rSeg_ = (size_t)-1;
    mutable uint64_t rFirst_ = ~0ull;
    uint64_t head_ = 0, serial_ = 0;
    int pcrPid_ = -1;
    PcrTimeline tl_;
};

} // namespace dect2
