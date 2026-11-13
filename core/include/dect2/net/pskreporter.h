// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// PSK Reporter (pskreporter.info) reception reports: IPFIX over UDP to report.pskreporter.info:4739, https://pskreporter.info/pskdev.html.
// The encoder / decoder / batcher are pure (tested without a network); PskReporter is the sender thread. Off until the user opts in:
// spots are public, and need the user's own amateur callsign and locator.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {
namespace pskr {

constexpr size_t kMaxPacket = 1024;       // a datagram stays below this
constexpr int kSenderTemplateId = 0x9993, kReceiverTemplateId = 0x9992;

struct Receiver { std::string call, locator, software, antenna; };
struct Spot {
    std::string call;         // the transmitting station
    std::string locator;      // its locator when the message has one ("" otherwise)
    std::string mode;         // ADIF mode: FT8, FT4, FT2, WSPR, PSK31, JS8, CW, RTTY
    uint32_t freqHz = 0;      // RF frequency (dial + audio)
    int snr = 0;              // dB (-128 .. 127)
    int64_t time = 0;         // Unix seconds the callsign was recognised
};

// One datagram: header, the two templates (withTemplates), the receiver record, as many spots from `first` as fit in kMaxPacket.
// Returns how many spots went in (0 when there were none: a pure receiver update).
size_t encodePacket(std::vector<uint8_t>& out, const Receiver& rx, const std::vector<Spot>& spots, size_t first, int64_t now, uint32_t seq, uint32_t streamId, bool withTemplates);

struct Decoded { Receiver rx; std::vector<Spot> spots; uint32_t seq = 0, streamId = 0; int64_t time = 0; bool hasTemplates = false; };
bool decodePacket(const uint8_t* d, size_t n, Decoded& out);   // for the tests; reads what encodePacket writes

bool validCallsign(const std::string& c);   // 3 .. 12 chars of A-Z 0-9 /, at least one digit and one letter
bool validLocator(const std::string& l);    // 4, 6 or 8 characters of Maidenhead
bool sendsInText(const std::string& text, const std::string& call);   // CW / PSK text: the call follows "DE " (the sender, not a station merely mentioned)

// Collects spots: the same sender + mode + band inside dedupeSec is kept once; a batch is due when full or every intervalSec
class Batcher {
public:
    explicit Batcher(int intervalSec = 300, size_t maxSpots = 60, int dedupeSec = 600) : interval_(intervalSec), max_(maxSpots), dedupe_(dedupeSec) {}
    bool add(const Spot& s, int64_t now);     // false when it was a duplicate (or empty)
    bool due(int64_t now) const;
    std::vector<Spot> take(int64_t now);      // the pending spots; restarts the interval
    size_t pending() const { return v_.size(); }
private:
    struct Seen { std::string key; int64_t t; };
    int interval_; size_t max_; int dedupe_;
    std::vector<Spot> v_;
    std::vector<Seen> seen_;
    int64_t last_ = 0;
};
} // namespace pskr

struct PskReporterConfig {
    bool on = false;
    std::string call, locator, antenna;
    std::string host = "report.pskreporter.info"; int port = 4739;
};
struct PskReporterStatus {
    bool active = false;
    uint64_t spotsSent = 0, packets = 0, duplicates = 0;
    int64_t lastSend = 0;       // Unix seconds, 0 never
    std::string state, lastError;
};

class PskReporter {
public:
    static PskReporter& get();
    ~PskReporter();
    void configure(const PskReporterConfig& c);
    PskReporterConfig config() const;
    PskReporterStatus status() const;
    void stop();
    void setDialHz(double hz) { dial_.store(hz, std::memory_order_relaxed); }   // the radio's tuned frequency; the decoders give audio offsets
    bool want() const { return want_.load(std::memory_order_relaxed); }
    // what the decoders call (cheap when off). audioHz: offset from the dial; call: the sender's callsign (not the user's).
    // dialHz > 0: the dial frequency of the band the signal was heard on (multi-band / whole-span decoding: several bands at once);
    // 0: the radio's tuned frequency (setDialHz).
    void spot(const char* mode, double audioHz, float snrDb, const std::string& call, const std::string& grid = std::string(), double dialHz = 0);
    // Tests: replace the UDP socket by a function that gets each datagram, and send what is pending right now.
    void setSenderForTest(std::function<bool(const uint8_t*, size_t)> f);
    void flushNowForTest(int64_t now);
    struct Impl;
private:
    PskReporter();
    std::atomic<bool> want_{false};
    std::atomic<double> dial_{0};
    std::unique_ptr<Impl> impl_;
};

} // namespace dect2
