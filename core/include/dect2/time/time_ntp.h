// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The computer clock against a time source (GPS or a time station), the one-click clock sync, and the optional hobby NTP server.
//
// ClockDiscipline holds the newest measurement of each source: the Time signals and GNSS screens publish into it, the sync button and the NTP server
// read it. Nothing here changes the clock by itself: clockSyncRun() is only ever called by a click on "Sync now", and the NTP server only answers
// with the corrected time, it never sets the computer's clock.
//
// NTP: RFC 5905 server mode only (a reply to a client request, version 1 - 4), no symmetric modes, no control or private packets. Stratum 1 with
// reference id "GPS" when the source is GPS, stratum 2 when it is a time station; an unsynchronised answer (leap indicator 3, stratum 16) when
// there is no fresh measurement. Requests are rate limited per client and in all; only LAN sources (private, link-local, loopback) are answered.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace dect2 {

enum ClockSource { ClockGps = 0, ClockStation = 1, kClockSources };

struct ClockMeasurement {
    bool valid = false;           // locked and the offset is measured
    double offsetMs = 0;          // computer clock minus the source's UTC: positive, the computer is ahead
    double accuracyMs = 0;        // what the measurement is good to (USB / SDR latency, the path of a time station)
    double atUnix = 0;            // the computer's own clock when it was published, UTC epoch seconds
    std::string label;            // "GPS", "DCF77", "BPM" ...: the reference id of a stratum 2 answer is its first four characters
};

class ClockDiscipline {
public:
    static ClockDiscipline& get();
    void publish(int source, const ClockMeasurement& m);     // atUnix is set here
    bool latest(int source, ClockMeasurement& m, double maxAgeS) const;   // false when none, not valid or older than maxAgeS
    // The best fresh source for the NTP server: GPS first, else the time station
    bool best(ClockMeasurement& m, int& source, double maxAgeS) const;
    // The computer clock was stepped (sync or the user): the measurements no longer describe it
    void clearAll();
private:
    mutable std::mutex mu_;
    ClockMeasurement m_[kClockSources];
};

double systemUnixNow();    // the computer clock, UTC epoch seconds

// ---------------------------------------------------------------- the one-click sync
constexpr double kClockSyncMaxOffsetMs = 600000;   // more than ten minutes away: the measurement, not the clock, is wrong
// "" when a sync may be offered for this measurement, else the reason it is refused. nowUnix: the computer clock now.
std::string clockSyncRefusal(const ClockMeasurement& m, double nowUnix);
enum ClockOs { OsMac = 0, OsWindows, OsLinux };
ClockOs clockThisOs();
// The command a person can run as an administrator to move the clock back by offsetMs (a clock ahead is set back). Numbers only: no text from outside.
std::string clockSetCommand(ClockOs os, double offsetMs);
// The command as run through the operating system's own prompt (macOS: osascript administrator privileges, Windows: UAC, Linux: pkexec)
std::string clockElevatedCommand(ClockOs os, double offsetMs);
// "" if this offset can be applied on that system, else why not (macOS date sets whole seconds)
std::string clockSetLimit(ClockOs os, double offsetMs);

// Runs the elevated command and waits for it (a prompt may stay open: call from a thread). Returns true when the system said it was done.
bool clockSyncRun(double offsetMs, std::string& message);

// ---------------------------------------------------------------- NTP
namespace ntp {
constexpr size_t kPacketLen = 48;
constexpr uint64_t kUnixToNtpSeconds = 2208988800ULL;   // 1900-01-01 to 1970-01-01

struct Packet {
    int li = 0, vn = 4, mode = 3, stratum = 0, poll = 0, precision = 0;
    uint32_t rootDelay = 0, rootDispersion = 0;   // NTP short format: 16.16 seconds
    uint8_t refId[4] = {0, 0, 0, 0};
    uint64_t reference = 0, origin = 0, receive = 0, transmit = 0;   // NTP timestamps: 32.32 seconds since 1900
};

bool parse(const uint8_t* b, size_t n, Packet& p);            // false: shorter than 48 bytes
void serialize(const Packet& p, uint8_t* b48);
uint64_t fromUnix(double unixSec);
double toUnix(uint64_t ts);
uint32_t toShort(double seconds);
double fromShort(uint32_t v);

struct ServerState {
    bool synced = false;
    int stratum = 2;
    uint8_t refId[4] = {0, 0, 0, 0};
    double rootDelayS = 0, rootDispersionS = 0;
    double referenceUnix = 0;                  // when the clock was last measured against the source
};
// The state to advertise for a measurement (nullptr or invalid: unsynchronised). maxAgeS stretches the dispersion with the age of the measurement.
ServerState stateFor(const ClockMeasurement* m, int source, double nowUnix);
// The reply to a client request. False: not something this server answers (not mode 3, a version outside 1 - 4, too short).
bool makeReply(const Packet& req, const ServerState& st, double rxUnix, double txUnix, Packet& out);

bool isLanAddress(uint32_t hostOrderIpv4);     // 10/8, 172.16/12, 192.168/16, 169.254/16, 127/8
std::string ipText(uint32_t hostOrderIpv4);

// per client and in all: kPerClient requests in kWindowS seconds, kTotal in all in one second
class RateLimiter {
public:
    static constexpr int kPerClient = 10;
    static constexpr double kWindowS = 10;
    static constexpr int kTotalPerS = 300;
    bool allow(uint32_t client, double nowS);
private:
    struct Entry { uint32_t ip = 0; double t[kPerClient] = {}; int n = 0; };
    static constexpr int kSlots = 256;
    Entry e_[kSlots];
    double secStart_ = -1; int secCount_ = 0;
};

struct ServerStats {
    uint64_t requests = 0;     // answered (including unsynchronised answers)
    uint64_t unsynced = 0;     // answered with "not synchronised"
    uint64_t limited = 0;      // dropped by the rate limit
    uint64_t refused = 0;      // dropped: not a LAN address or not a client request
    uint32_t lastClient = 0;
    double lastAtUnix = 0;
};

class Server {
public:
    Server();
    ~Server();
    // Starts on UDP 123, else (no rights) on fallbackPort. Returns false with the reason in message. loopbackOnly (tests): bind 127.0.0.1.
    bool start(int fallbackPort, std::string& message, bool loopbackOnly = false, int firstPort = 123);
    void stop();
    bool running() const;
    int port() const;
    ServerStats stats() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace ntp
} // namespace dect2
