// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The Digital video generator program's engine (#458): several sources, each on its own thread, put together by a mux thread, with a
// preview thread for the window. The window thread only draws: it starts and stops things here and reads their status.
//
//   source k  (thread "vg-src-<k+1>", a VideoGenerator with Output::Queue): input, scaling, overlay and encoding into its own
//             single-programme transport stream (its own mux rate), pushed into its TsQueue
//   mux       (thread "vg-mux"): TsRemux takes the packets of every source's queue as programmes of one multiplex (the numbering of
//             the Muxing tab), pads it with null packets to the mux rate and sends it: to OnAir over the link (VgLinkClient), UDP, RTP
//             (paced in real time) or a .ts file (as fast as the sources encode)
//   preview   (thread "vg-preview"): about 12 times a second the newest picture of the chosen source, scaled to RGBA (latest wins)
//
// Queues: a live source's queue holds at most kSourceQueuePackets (the #494 limit); when the mux cannot keep up the oldest packets go
// and are counted. For a file the source waits for the mux instead (nothing is lost). A stalled source does not hold up the others:
// its slots in the multiplex are padded. Stop order: sources, then the mux, then the preview.
#pragma once
#include "ts_remux.h"
#include "videogen.h"
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace dect2::vgen {

class VgLinkClient;

constexpr int kMaxSources = 8;
constexpr size_t kSourceQueuePackets = 4000;   // about 6 MB, as the transmitter's queue of #494

// A bounded queue of 188 byte packets between a source thread and the mux thread
class TsQueue {
public:
    explicit TsQueue(size_t limit = kSourceQueuePackets) : limit_(limit) {}
    // n whole packets. blocking false: the oldest go when it is full (counted in dropped()); true: waits for room (a file) until close()
    void push(const uint8_t* packets, size_t n, bool blocking);
    bool pop(uint8_t* pk);
    size_t size() const;
    void clear();
    void close();                 // wakes a waiting push (Stop)
    void reopen();
    uint64_t dropped() const { return dropped_; }
    uint64_t pushed() const { return pushed_; }
    // the source's mux rate while it runs (bit/s); the mux thread treats 0 as "not in the multiplex"
    std::atomic<double> rate{0};
    std::atomic<bool> producing{false};   // the source thread runs (a file's last packets still count after it ended)

private:
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::array<uint8_t, 188>> q_;
    size_t limit_;
    bool closed_ = false;
    std::atomic<uint64_t> dropped_{0}, pushed_{0};
};

// How a source is numbered in the multiplex (the Muxing tab). 0 / empty: automatic, as #494 (programme firstServiceId + k, PMT
// 0x1000 + k, streams 0x100 * (k + 1) + j)
struct MuxNumbering {
    int serviceId = 0;
    int pmtPid = 0;
    std::vector<int> esPids;      // video, then the audio tracks
};

struct MuxConfig {
    Output output = Output::Udp;  // File, Udp, Rtp or Transmitter (over the link)
    std::string tsPath;           // File
    std::string host = "127.0.0.1";
    int port = 5000, ttl = 8;
    double channelBps = 0;        // the channel's capacity (from the transmitter, or typed in); 0: unknown
    bool padding = true;          // pad to the channel's capacity (when known); off: to the sources' sum plus a margin
    int firstServiceId = 1;
    int tsid = 1;
    std::string provider;         // the SDT provider for every programme (empty: each source's own)
    VgLinkClient* link = nullptr; // Transmitter: the link to OnAir (owned by the window)
};

// The mux rate of a multiplex: the channel's capacity with padding when it is known (over the link: the usable rate), else the
// sources' rates plus 2 % and room for the PAT / SDT
double muxRateFor(const MuxConfig& m, const std::vector<double>& sourceBps);

struct SourceStatus {
    bool running = false, previewOnly = false;
    GenStats gen;
    size_t queued = 0;
    uint64_t dropped = 0;         // packets dropped from its queue (the mux was behind)
    double cpuBusy = 0;           // its thread's CPU in percent of one core, about the last second
};

struct MuxStatus {
    bool running = false, finished = false;
    double rateBps = 0;           // the mux rate it paces at
    double sourcesBps = 0;        // what the running sources need
    uint64_t packets = 0, nulls = 0;
    double seconds = 0;
    double cpuBusy = 0;
    bool overload = false;        // the sources need more than the mux rate
    std::string error;
    std::vector<TsRemuxProgramme> programmes;
};

struct PreviewStatus {
    bool running = false;
    int source = 0;
    uint64_t frames = 0, dropped = 0;   // dropped: pictures made but replaced before the window took them
    double cpuBusy = 0;
};

class Session {
public:
    Session();
    ~Session();   // stops everything (sources, mux, preview), joins every thread
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // ---- the multiplex: sources plus the mux ----
    // Starts the mux thread with these sources (index = source number - 1; an entry with enabled false stays out).
    struct SourceSpec { bool enabled = true; GenConfig cfg; MuxNumbering num; };
    bool start(const std::vector<SourceSpec>& sources, const MuxConfig& mux, std::string& err);
    void stop();                                   // sources, then the mux; the preview keeps its source's pictures until stopPreview
    bool running() const;                          // the mux runs
    // one source while the mux runs (a new tab, Stop of one source): its slot is padded while it is stopped
    bool startSource(int k, const GenConfig& cfg, const MuxNumbering& num, std::string& err);
    void stopSource(int k);
    void setOverlay(int k, const Layout& l);       // takes effect on the source's next frame
    void setChannelBps(double bps);                // a new capacity from the transmitter (padding follows on the next start)

    // ---- a source alone, for the preview only (nothing encoded or sent) ----
    bool startPreviewOnly(int k, const GenConfig& cfg, std::string& err);
    bool sourceRunning(int k) const;
    bool sourcePreviewOnly(int k) const;
    void stopSourcePreview(int k);                 // stops a preview-only source (a streaming one is left alone)

    // ---- the preview thread ----
    void startPreview(int k);                      // shows source k (it must be running, streaming or preview-only); another k switches
    void stopPreview();
    bool previewRunning() const;
    // the newest picture, RGBA; false when none newer than `serial`
    bool previewFrame(std::vector<uint8_t>& rgba, int& w, int& h, uint64_t& serial);

    SourceStatus sourceStatus(int k) const;
    MuxStatus muxStatus() const;
    PreviewStatus previewStatus() const;
    TsQueue& queue(int k);                         // tests

    struct Impl;

private:
    std::unique_ptr<Impl> p_;
};

} // namespace dect2::vgen
