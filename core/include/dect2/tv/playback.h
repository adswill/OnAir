// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Playing recorded or timeshifted transport streams through the live player: a file as a PacketSource (with a PCR time map for seeking), the pacer
// that releases packets at the speed of the stream's own clock, and the feeder thread that reads a PacketSource and hands the packets to a sink
// (the player's push()). Pause, jump back / forward and "go live" are positions in the source.
#pragma once
#include "dect2/tv/timeshift.h"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dect2 {

// A .ts file as a source. open() reads the whole file once to build the time map (about a second per GB); do it off the UI thread.
class TsFileSource : public PacketSource {
public:
    ~TsFileSource() override { close(); }
    bool open(const std::string& path);
    void close();
    double duration() const;                       // seconds, 0 when the file carries no PCR
    uint64_t first() const override { return 0; }
    uint64_t end() const override { return n_; }
    size_t read(uint64_t idx, uint8_t* out, size_t n) const override;
    bool timeAt(uint64_t idx, double& t) const override;
    bool indexAt(double t, uint64_t& idx) const override;
    bool growing() const override { return false; }
    // T2-MI (ETSI TS 102 773, issue 498): a modulator feed, an outer stream of T2-MI packets on one PID (often with no PCR). open() finds it in
    // the first packets and plays the transport stream of one PLP instead (the one with the most packets), rebuilt into a temporary file that
    // close() deletes. selectT2miPlp() rebuilds it for another PLP: stop the feeder first and start it again at 0 afterwards.
    bool t2mi() const { std::lock_guard<std::mutex> lk(mu_); return t2miPid_ >= 0; }
    int t2miPid() const { std::lock_guard<std::mutex> lk(mu_); return t2miPid_; }
    int t2miPlp() const { std::lock_guard<std::mutex> lk(mu_); return t2miPlp_; }
    std::vector<int> t2miPlps() const { std::lock_guard<std::mutex> lk(mu_); return plps_; }
    bool selectT2miPlp(int plp);
    void setScratchDir(const std::string& dir) { std::lock_guard<std::mutex> lk(mu_); scratch_ = dir; }   // where the rebuilt PLP stream goes (default: the system temp folder)
private:
    bool buildInner(int plp);
    mutable std::mutex mu_;
    FILE* f_ = nullptr;
    uint64_t n_ = 0;
    PcrTimeline tl_;
    std::string path_, inner_, scratch_;
    int t2miPid_ = -1, t2miPlp_ = -1;
    std::vector<int> plps_;
};

// Decides how long to wait before releasing a packet whose stream time is t: the stream runs at `speed` times the wall clock from the anchor.
class TsPacer {
public:
    void reset(double streamT, double wallNow) { s0_ = streamT; w0_ = wallNow; }
    // Seconds to wait (>= 0). A packet more than 2 s late re-anchors the pacer (after a stall, or a jump in the clock).
    double delay(double streamT, double wallNow, double speed = 1.0, double lead = 0.25) {
        const double due = w0_ + (streamT - s0_) / (speed > 0.05 ? speed : 1.0) - lead;
        const double d = due - wallNow;
        if (d < -2.0) { reset(streamT, wallNow); return 0; }
        return d > 0 ? d : 0;
    }
private:
    double s0_ = 0, w0_ = 0;
};

class PlaybackFeeder {
public:
    using Sink = std::function<void(const uint8_t* pkts, size_t n)>;
    ~PlaybackFeeder() { stop(); }
    void start(PacketSource* src, uint64_t startIdx, Sink sink);
    void stop();
    bool running() const { return run_; }
    void pause(bool on);
    bool paused() const { return paused_; }
    void seekIndex(uint64_t idx);
    void seekSeconds(double t);                    // stream time on the source's timeline
    void skip(double dt);                          // relative: negative = back
    void goLive();                                 // the end of a growing source
    void setSpeed(double s) { speed_ = s; }
    uint64_t position() const { return pos_; }
    double positionSeconds() const;
    double behindLiveSeconds() const;              // 0 on a file; on a live buffer how far playback is behind the newest packet
    bool atEnd() const { return atEnd_; }          // a file source reached its last packet
    uint64_t sent() const { return sent_; }
private:
    void loop();
    PacketSource* src_ = nullptr;
    Sink sink_;
    std::thread th_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::atomic<bool> run_{false}, paused_{false}, atEnd_{false};
    std::atomic<uint64_t> pos_{0}, sent_{0};
    std::atomic<double> speed_{1.0};
    bool seek_ = false;
    uint64_t seekTo_ = 0;
};

} // namespace dect2
