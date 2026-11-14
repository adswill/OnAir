// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// SondeHub radiosonde upload (4.0): decoded sondes go to the SondeHub v2 database (tracker.sondehub.org) the way the reference uploader
// radiosonde_auto_rx does it, after the SondeHub wiki (github.com/projecthorus/sondehub-infra/wiki: "SondeHub Telemetry Format" and
// "API (Beta)"):
//  - HTTPS PUT of a gzip-compressed JSON array of telemetry objects to <server>/sondes/telemetry, with the Date header (RFC 7231, used by
//    SondeHub to correct a wrong receiver clock), User-Agent OnAir-<version>, Content-Encoding gzip, Content-Type application/json.
//  - The frames are collected and sent every 15 s (the auto_rx default; 30 s at most, 1.5 times the interval, while a DFM or Meisei sonde is
//    in the batch so that SondeHub's z-check gets enough points). 200 is success; 201 / 202 accepted with notes (shown); 5xx and no answer
//    are retried, up to 5 tries; any other answer is not retried and its text is shown.
//  - The station (listener) position goes to <server>/listeners as plain JSON: at the start and every 6 hours (auto_rx), or every 30 s with
//    "mobile": true for a chase car with a live GPS position (ChaseMapper's default). Without "Send my position" the position is sent as null.
//  - Only frames whose check passed reach the uploader; a frame is only sent with a GPS position (and at least 4 satellites where the sonde
//    reports them), the sonde's own date and time (never the computer clock: DO NOT USE LOCAL SYSTEM TIME, says the format page) within one
//    hour of now (no replays of old recordings), and a serial number. iMet-1/4 are not sent: auto_rx makes up their serial from the power-on
//    time and an iMet-1 / iMet-4 distinction OnAir does not have, so the same sonde would show up twice.
// Off until the user ticks it; nothing is connected or sent otherwise. software_name is "OnAir", software_version the OnAir version.
#pragma once
#include "dect2/net/sonde_upload.h"
#include "dect2/sonde/sonde_tel.h"
#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct SondeHubConfig {
    bool enabled = false;
    std::string call;                 // uploader_callsign (any text; SondeHub asks for something unique, a callsign is best)
    std::string antenna;              // uploader_antenna, free text (optional)
    std::string email;                // uploader_contact_email: seen only by the SondeHub admins (optional)
    bool sharePos = true;             // send the station position (uploader_position); off: null, the station is not drawn on the tracker
    bool mobile = false;              // chase car: the live GPS position every 30 s as "mobile": true
    int uploadS = 15;                 // batch interval, 10 - 60 s
    std::string server = "https://api.v2.sondehub.org";   // Advanced (tests point it at a mock server on localhost)
};

struct SondeHubStation { bool set = false; double lat = 0, lon = 0, altM = 0; bool live = false; };   // live: from a GPS right now

namespace sondehub {
constexpr const char* kSoftwareName = "OnAir";
const char* softwareVersion();        // ONAIR_VERSION
std::string userAgent();              // "OnAir-<version>"

struct Ident {
    std::string manufacturer, type, subtype, serial;
    long long frame = -1;
    bool slow = false;                // DFM or Meisei: the batch waits longer (z-check)
};
// The SondeHub names of a sonde: manufacturer, type, subtype and serial as the format page lists them. false (and why) for a type SondeHub
// cannot take from OnAir or a serial that is not known yet.
bool identify(const SondeInfo& s, Ident& id, std::string* why = nullptr);
std::string isoTime(double unixT);    // "2026-11-12T10:20:30.500000Z"
std::string httpDate(double unixT);   // "Thu, 12 Nov 2026 10:20:30 GMT"
std::string jsonEscape(const std::string& s);
bool validUploader(const std::string& call);   // 3 - 64 printable characters
// One telemetry object (no spaces), "" when the frame must not be sent (why: the reason). wallUtc: the computer's UTC now (time_received).
std::string telemetryJson(const SondeHubConfig& c, const SondeHubStation& st, const SondeInfo& s, double wallUtc, std::string* why = nullptr, Ident* id = nullptr);
std::string listenerJson(const SondeHubConfig& c, const SondeHubStation& st);
std::string jsonArray(const std::vector<std::string>& objects);
uint32_t crc32(const void* data, size_t n);
std::string gzip(const std::string& in);      // RFC 1952, one member, deflate from stb_image_write's zlib compressor
// The text of a SondeHub answer for the user: its message, errors and warnings (raw text, shortened, when it is not that JSON)
std::string answerText(const std::string& body);
// What a 2xx answer says about the frames: SondeHub answers 200 / 201 / 202 with {"message", "errors":[{"error_message", "payload":{...}}]} when
// payloads were refused ("some or all payloads could not be processed"), so the status code alone does not say they were stored.
struct Reply {
    int rejected = 0;                 // errors that mean a frame was refused (anything but a z-check)
    int zcheck = 0;                   // z-check notes (frames flagged as GPS outliers: normal for DFM and RS92, see the wiki)
    std::vector<std::string> serials; // serial numbers named in the errors' payloads
    std::string text;                 // answerText of the body
};
Reply analyseReply(const std::string& body);

// The batch: frames wait here until the interval is up. A frame of a sonde already queued with the same frame number is a duplicate.
class Batch {
public:
    bool add(const std::string& serial, long long frame, const std::string& json, bool slow, double t);   // false: duplicate or full
    double intervalS(int uploadS) const;   // the interval now (1.5 times, at most 30 s, while a DFM or Meisei sonde is queued)
    bool due(double t, int uploadS) const;
    std::vector<std::string> take(double t);
    size_t size() const { return q_.size(); }
    uint64_t duplicates = 0, overflow = 0;
    static constexpr size_t kMax = 3000;
private:
    std::vector<std::string> q_;
    std::map<std::string, long long> last_;
    bool slow_ = false;
    double since_ = -1;
};
}  // namespace sondehub

struct SondeHubStatus : SondeUploadStatus {
    uint64_t skipped = 0;             // frames the rules kept back (no fix, no time, unknown serial, ...)
    std::string skipWhy;              // the last such reason
    uint64_t uploads = 0;             // PUT requests that SondeHub accepted
    uint64_t rejected = 0;            // frames SondeHub answered with an error for (bad serial, ...), not counted as sent
    std::string lastReply;            // the raw body of SondeHub's last answer (shortened), shown under "What is sent"
};

struct SondeHubTiming {            // tests shorten these
    double uploadS = 0;            // > 0 overrides the configured interval
    double retryWaitS = 2;         // between tries of one upload
    double listenerEveryS = 6 * 3600, chaseEveryS = 30;
};

class SondeHub {
public:
    static SondeHub& get();
    void configure(const SondeHubConfig& c);   // starts the thread when enabled
    SondeHubConfig config() const;
    // sent: telemetry frames SondeHub took; beacons: station uploads; dropped: frames not sent (refused, given up, queue full);
    // reconnects: retries. lastLine: what the last upload was (frames, sondes, bytes before and after gzip).
    SondeHubStatus status() const;
    std::string lastJson() const;              // the JSON of the last telemetry upload before compression: "show exactly what is sent"
    std::string lastListenerJson() const;
    void setStation(const SondeHubStation& st);
    bool wanted() const { return want_.load(std::memory_order_relaxed); }
    void frame(const SondeInfo& s, double wallUtc);   // one good frame from the receiver
    void setTiming(const SondeHubTiming& t);
    void stop();
    struct Impl;
private:
    SondeHub();
    ~SondeHub() = default;
    std::atomic<bool> want_{false};
    std::unique_ptr<Impl> impl_;
};

}  // namespace dect2
