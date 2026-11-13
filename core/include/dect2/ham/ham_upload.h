// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Opt-in logbook uploads for the Ham log: QRZ.com Logbook, Club Log (realtime) and eQSL. Off by default; nothing is sent unless a service is
// switched on AND its credentials are filled in. Only the ADIF record of the QSO is sent (call, grid, mode, band, frequency, reports, UTC time,
// your callsign and grid). HTTPS only with certificate verification; credentials travel in POST bodies, never in a URL, and are never logged.
// LoTW is not done: it needs a TQSL certificate and signing (tqsl), so it stays a manual ADIF export.
#pragma once
#include "dect2/ham/ham.h"
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dect2::ham {

enum class UpService { Qrz = 0, ClubLog = 1, Eqsl = 2 };
constexpr int kUpServices = 3;
const char* upServiceName(UpService s);

struct UpConfig {
    bool autoUpload = false;                       // upload each QSO as soon as it is logged (needs the service on too)
    bool on[kUpServices] = {false, false, false};
    std::string qrzKey;                            // QRZ Logbook API key (per logbook)
    std::string clubEmail, clubPassword, clubApiKey;   // Club Log: email, application password, application API key
    std::string eqslUser, eqslPassword;
    bool ready(UpService s) const;                 // on and the credentials present
};

struct UpResponse { int status = 0; std::string body, err; };
// POST application/x-www-form-urlencoded; the production one is curl with the secrets on stdin (never in argv), https only.
using UpTransport = std::function<UpResponse(const std::string& url, const std::string& body)>;
UpTransport curlTransport();

struct UpRequest { std::string url, body; };
UpRequest buildUpRequest(UpService s, const UpConfig& c, const QsoRecord& q);
// ok; permanent: a retry cannot help (bad credentials, rejected record); reason for the user.
struct UpOutcome { bool ok = false, permanent = false; std::string reason; };
UpOutcome parseUpResponse(UpService s, const UpResponse& r);

// st: p pending, u uploaded, f failed (reason says why), n skipped (the service was off when the QSO was logged)
struct UpState { char st = 'p'; int attempts = 0; long long nextTry = 0; bool force = false; std::string reason; };
struct UpItem { QsoRecord q; UpState s[kUpServices]; };

class HamUploader {
public:
    explicit HamUploader(UpTransport t = curlTransport()) : tr_(std::move(t)) {}
    ~HamUploader() { stopWorker(); }
    void setConfig(const UpConfig& c);
    UpConfig config() const;
    void setTestAllowPlainLoopback(bool b) { loopback_ = b; }   // tests only: http://127.0.0.1 accepted
    bool load(const std::string& path);           // the persisted queue; later saves go to the same file
    void enqueue(const QsoRecord& q, long long now);   // queues it; sends nothing by itself
    // Sends what is due, at most one request per service per kMinGapSec. Returns requests made. Never touches a service that is off.
    int poll(long long now);
    void uploadNow(long long now);                // the "upload now" button: pending, failed and skipped items for the ready services, even without auto
    std::vector<UpItem> items() const;
    size_t pendingCount() const;
    void startWorker();
    void stopWorker();
    static constexpr int kMinGapSec = 3, kMaxAttempts = 6;
private:
    bool save();
    mutable std::mutex mu_;
    UpConfig cfg_;
    UpTransport tr_;
    std::vector<UpItem> q_;
    std::string path_;
    long long lastReq_[kUpServices] = {0, 0, 0};
    bool loopback_ = false;
    std::thread th_;
    std::atomic<bool> quit_{false};
};

}  // namespace dect2::ham
