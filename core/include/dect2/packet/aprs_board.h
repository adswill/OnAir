// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// APRS weather stations and messages: what the packet receiver keeps of them for the Weather and Messages views (docs/APRS-BOARD-3.9.md).
// Weather: the values of APRS101 chapter 12 in metric units, with a 24 h history per station. Messages: chapter 14 (messages, acks and
// rejects, bulletins, announcements, group bulletins and the National Weather Service bulletins), grouped by the two stations.
#pragma once
#include "packet_aprs.h"
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {

// One weather report, metric. NaN: the station did not send that value.
struct AprsWx {
    double timeSec = 0;                                  // signal seconds, as PacketStation::lastHeardSec
    float tempC = NAN, windDirDeg = NAN, windKmh = NAN, gustKmh = NAN;
    float rain1hMm = NAN, rain24hMm = NAN, rainMidnightMm = NAN;
    float humidityPct = NAN, pressureHpa = NAN;
    bool any() const;
};

AprsWx aprsWxFromReport(const aprs::Weather& w, double timeSec);
// The newest report merged into the station's last values (a value missing from this report keeps the one before, for at most 2 h), and the
// history: the last 24 h, one point per 5 minutes at most (a newer report within 5 min replaces the point), oldest first.
void aprsWxAdd(AprsWx& last, std::vector<AprsWx>& hist, const AprsWx& report);
constexpr double kAprsWxHistorySec = 24 * 3600.0;
constexpr double kAprsWxStepSec = 300.0;
// "12.3 °C  NW 14 km/h": the short text for a map label (empty when nothing is known)
std::string aprsWxLabel(const AprsWx& w);
const char* aprsCompass(float deg);                      // "N", "NNE" ... (8 points), "" for NaN

// ---- messages
enum class AprsMsgKind : uint8_t { Message, Bulletin, Announcement, GroupBulletin, Nws };
const char* aprsMsgKindName(AprsMsgKind k);
// The kind of a message from its addressee: BLN0-BLN9 bulletins, BLNA-BLNZ announcements, BLN<n><group> group bulletins,
// NWS- / SKY / CWA / BOM the weather services' bulletins, anything else a message to a station
AprsMsgKind aprsAddresseeKind(const std::string& addressee);
// The addressee is the user's callsign. Without an SSID the user's call matches every SSID of it; with one only that SSID. Case does not matter.
bool aprsIsForCall(const std::string& addressee, const std::string& myCall);

struct AprsMessage {
    double timeSec = 0;                  // first heard
    double lastSec = 0;                  // the last copy (stations send a message again until it is acknowledged)
    std::string from, to, text, msgNo;
    AprsMsgKind kind = AprsMsgKind::Message;
    bool acked = false, rejected = false;
    bool toMe = false, fromMe = false;
    uint32_t copies = 1;
};

class AprsMessageLog {
public:
    void setMyCall(const std::string& call);             // also marks the listed messages again
    const std::string& myCall() const { return my_; }
    enum class Added { No, New, Again, Ack };
    // One APRS packet of type Message from `from`. New: a message or bulletin heard for the first time (index() is it);
    // Again: a retry of one already listed; Ack: an ack or reject that marked a listed message; No: not a message, or a telemetry definition
    Added add(double timeSec, const std::string& from, const aprs::Info& a);
    int lastIndex() const { return last_; }
    const std::vector<AprsMessage>& list() const { return list_; }   // oldest first
    void shiftTimes(double dt);
    void clear() { list_.clear(); last_ = -1; }
    void setMax(size_t n) { max_ = n < 20 ? 20 : n; }
private:
    std::vector<AprsMessage> list_;
    std::string my_;
    int last_ = -1;
    size_t max_ = 500;
};

// The conversation a message belongs to: the two callsigns in alphabetical order, "A <> B"; bulletins have their own key, "BLN1 from X"
std::string aprsThreadKey(const AprsMessage& m);

} // namespace dect2
