// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Radiosonde upload: decoded sondes (RS41, RS92, DFM, LMS6, MRZ, iMet-54, iMS-100) go to APRS-IS aggregators such as wettersonde.net and
// radiosondy.info, in the format of dxlAPRS / radiosonde_auto_rx (an APRS object per sonde) after a receiver station beacon.
// Every target is OFF until the user ticks it; nothing is connected or sent otherwise. Receive-only: the upload is the only direction used,
// nothing that a server sends is ever turned into radio traffic. The passcode is the user's own (settings only, not in logs or the status text).
// One decoder frame fans out to every enabled target. TCP connections reconnect with a growing wait. A sonde is sent at most once per
// objectGapS seconds (default 5) per serial, and only with a valid position; a field the sonde type does not report is left out.
#pragma once
#include "dect2/sonde/sonde_tel.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct SondeUploadTarget {
    std::string name;
    std::string host;
    int port = 14580;
    std::string call;           // callsign-SSID, for example DG9BFC-10
    std::string pass;           // APRS-IS passcode; empty = -1 (the server will not accept uploads from an unverified login)
    bool enabled = false;
    int beaconMin = 10;         // receiver station beacon: on connect and then every this many minutes (1 - 120)
};

struct SondeUploadStatus {
    bool connected = false;
    bool verified = false;      // the server answered "verified"
    uint64_t sent = 0;          // object lines handed to the server (the beacons are counted apart)
    uint64_t beacons = 0;
    uint64_t dropped = 0;       // lines thrown away: not connected, unverified login, queue full
    uint64_t reconnects = 0;
    std::string state, lastError, lastLine;   // lastLine: the last line sent, without the line end
};

struct SondeUploadPreset { const char* name; const char* host; int port; const char* note; };
const std::vector<SondeUploadPreset>& sondeUploadPresets();

struct SondeUploadTiming {
    double reconnectMinS = 2, reconnectMaxS = 120;
    double objectGapS = 5;            // per sonde serial
    double beaconEveryS = 0;          // > 0 overrides the targets' beaconMin (tests)
};

namespace sondeup {
// "user DG9BFC-10 pass 12345 vers OnAir 3.9\r\n"; an empty pass is sent as -1
std::string loginLine(const std::string& call, const std::string& pass);
// APRS position text: "5240.87N" / "00848.27E" (hundredths of minutes, carry handled, N / E for zero)
std::string latText(double lat);
std::string lonText(double lon);
// "DG9BFC-10>APRS,TCPIP*:!5240.87N/00848.27E&OnAir Radiosonde RX\r\n"; "" for a position out of range
std::string beaconLine(const std::string& call, double lat, double lon, const std::string& comment = "OnAir Radiosonde RX");
// The 9 character APRS object name of a sonde, after the agreement of the sonde decoders (RS41 / RS92 serial, D + DFM number, LMS6xxxxx,
// IMSxxxxxx, MRZxxxxxx, IMETxxxxx); "" when the type has none: M10 and M20 (the ID needs raw bytes OnAir does not keep) and iMet-1/4 (no serial).
std::string objectName(const SondeInfo& s);
// The text after "Type=": the subtype of RS41 / RS92 (RS41-SG), else the type; iMS-100 is IMS100
std::string typeText(const SondeInfo& s);
// "DG9BFC-10>APRS,TCPIP*:;R1234567 *123456h5240.87N/00848.27EO090/012/A=065617 Clb=5.1m/s t=-52.3C h=3.0% p=11.2hPa 403.500MHz Type=RS41-SG\r\n"
// wallUtc: Unix time used when the sonde sent none. "" when the sonde has no valid position or no object name (why: a short reason).
std::string objectLine(const std::string& call, const SondeInfo& s, double wallUtc, std::string* why = nullptr);
}

class SondeUpload {
public:
    static SondeUpload& get();
    void configure(const std::vector<SondeUploadTarget>& t);   // starts the thread on the first enabled target; disabled ones close
    std::vector<SondeUploadTarget> targets() const;
    SondeUploadStatus status(size_t i) const;
    void setStation(double lat, double lon, bool set);          // the receiver position for the beacon
    bool wanted() const { return want_.load(std::memory_order_relaxed); }   // cheap: any enabled target
    void frame(const SondeInfo& s, double wallUtc);             // one decoded frame (the receiver calls this; with a position only)
    void setTiming(const SondeUploadTiming& t);
    void stop();
    struct Impl;
private:
    SondeUpload();
    ~SondeUpload() = default;
    std::atomic<bool> want_{false};
    std::unique_ptr<Impl> impl_;
};

} // namespace dect2
