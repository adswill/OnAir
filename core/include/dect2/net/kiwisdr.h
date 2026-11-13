// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KiwiSDR (a web-based HF receiver, 0 to 30 MHz, found worldwide): what the app asks of the source beyond the IqSource interface.
// The source itself is core/src/source/source_kiwisdr.cpp; docs/kiwisdr.md says what works and the etiquette on public receivers.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {

// What a connected KiwiSDR last reported (thread-safe snapshot). connected: an SND stream is running.
struct KiwiStatus {
    bool connected = false;
    double rssiDbm = -127;        // the S-meter of the frame last received
    bool adcOverload = false;     // the Kiwi's ADC clipped in the last frame
    bool gpsValid = false;        // the frames carry a GPS solution (gpsStatus not 254/255)
    uint32_t gpsSec = 0, gpsNsec = 0;   // GPS time of the frame last received: seconds of the GPS week and nanoseconds
    uint64_t frames = 0, gapFrames = 0, badFrames = 0;
    std::string message;          // the reason it is not streaming (busy, bad password, time limit ...) or a note, in plain words; "" when all is well
    std::string version;          // "1.8xx" when the server said it
};
// The status of the receiver at host:port (the nativeArgs of its device entry); a default KiwiStatus when unknown.
KiwiStatus kiwiStatus(const std::string& hostPort);

// GPS time of a Kiwi frame (seconds of the GPS week) to a Unix time: the week is taken from `nowUnix` (the computer's clock, only used to pick the
// week), leap seconds since 1980 are applied (18 s since 2017). Returns -1 for a status of no solution.
double kiwiGpsToUnix(uint32_t gpsSec, uint32_t gpsNsec, double nowUnix, int leapSeconds = 18);

// ---- the password -------------------------------------------------------------------------------------------------------------------------
// A receiver's password is kept in the network radio's list entry (the settings), never in a device name, a log line or a command line.
// netRadioDevices() hands it to the source here, keyed by host:port.
void kiwiSetPassword(const std::string& hostPort, const std::string& password);

// ---- the public list ----------------------------------------------------------------------------------------------------------------------
// One receiver of the public list (kiwisdr.com's list of Kiwis whose owners made them public)
struct KiwiServer {
    std::string name, host, location, antenna, hardware, id;
    int port = 8073;
    int users = 0, usersMax = 0;
    bool hasPosition = false;
    double lat = 0, lon = 0;
    double snr = 0;                 // dB, the HF (0-30 MHz) figure the owner's server measured; 0 = unknown
    double loHz = 0, hiHz = 30e6;   // the band it covers
    bool offline = false;
    double distanceKm = -1;         // from the user's station (kiwiSortByDistance)
    bool full() const { return usersMax > 0 && users >= usersMax; }
};
// Parses the list as the public listing gives it: a JSON array of objects, or the same array inside a JavaScript file ("var x = [ ... ];"). Fields read: name,
// url (http://host:port), loc, antenna, sdr_hw, gps ("(lat, lon)" or "lat,lon"), users, users_max, snr ("30,25" or 30), bands ("0-30000000"), offline, id.
// Entries without an address are skipped. Never throws; garbage gives an empty list.
std::vector<KiwiServer> parseKiwiList(const std::string& text);
double greatCircleKm(double lat1, double lon1, double lat2, double lon2);
void kiwiSortByDistance(std::vector<KiwiServer>& v, double lat, double lon);   // sets distanceKm; receivers without a position go last

} // namespace dect2
