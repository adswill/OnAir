// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Ham APRS and AX.25 packet: send and receive helpers for the Ham view. Frames are built here (UI frames with a digipeater path, APRS
// positions, status, messages with numbers and acknowledgements, bulletins, beacons) and turned into audio for the rig:
//   1200 baud  Bell 202 AFSK, 1200 Hz mark / 2200 Hz space, NRZI, for an FM rig's microphone or data input (VHF / UHF APRS, packet)
//   300 baud   HF packet AFSK, 200 Hz shift (1600 / 1800 Hz audio on USB), the HF APRS convention (10.1476 MHz)
//   9600 baud  G3RUH: NRZI, scrambled with x^17 + x^12 + 1, Gaussian filtered baseband for the rig's 9600 data port (flat FM modulator)
// Formats follow the APRS 1.0.1 specification (APRS101.pdf: uncompressed position, status, message, ack / rej, bulletin) and the AX.25 2.2
// link layer (packet_ax25.h). The receive side uses the repo's packet receiver and APRS parser (packet_rx.h, packet_aprs.h); this file adds the
// message session: retries with growing gaps, acknowledgement matching, duplicate suppression and automatic acks of messages to us.
// Nothing here transmits: aprsSend() goes through HamSlotTx::sendAudio (licence, Arm, amateur band) plus the duty-cycle limit.
#pragma once
#include "dect2/ham/ham.h"
#include "dect2/ham/ham_kbd.h"
#include "dect2/packet/packet_ax25.h"
#include <complex>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2::ham {

// ---- addresses and paths ----
// "LY1ABC-9" -> call + SSID; false when it is not 1..6 letters / digits with an SSID 0..15
bool ax25ParseAddress(const std::string& s, ax25::Address& a);
// "WIDE1-1,WIDE2-1" (spaces allowed, empty = direct): at most 8 digipeaters, each a valid address; err says what is wrong
bool ax25ParsePath(const std::string& s, std::vector<ax25::Address>& path, std::string& err);
// A UI frame FROM>TO,PATH:info. err when an address or the path is bad, or the info is longer than 256 bytes.
bool ax25UiFrame(const std::string& from, const std::string& to, const std::string& path, const std::string& info, ax25::Frame& f, std::string& err);

// ---- APRS information fields (APRS101) ----
constexpr const char* kAprsToCall = "APOAIR";   // the destination of frames OnAir makes (APRS "tocall": software id, AP + 4)
struct AprsPos {
    double lat = 0, lon = 0;         // degrees, north / east positive
    char symTable = '/', symCode = '-';   // '/-' house, '/>' car, '/#' digipeater ...
    std::string comment;             // at most 43 characters
    bool messaging = true;           // '=' (station takes messages) instead of '!'
    int ambiguity = 0;               // 0..4 digits of the position blanked (privacy)
    double altM = -1e9;              // >= -1e8: "/A=" altitude in feet in the comment
};
std::string aprsPosition(const AprsPos& p);                          // "=5441.00N/02517.00E-comment"
std::string aprsStatus(const std::string& text);                     // ">text" (62 characters at most)
// ":ADDRESSEE:text{NNN" ; msgNo "" for a message that wants no ack. text: 67 characters at most, without | ~ {
std::string aprsMessage(const std::string& to, const std::string& text, const std::string& msgNo);
std::string aprsAck(const std::string& to, const std::string& msgNo);   // ":ADDRESSEE:ackNNN"
std::string aprsRej(const std::string& to, const std::string& msgNo);
std::string aprsBulletin(char id, const std::string& text);         // ":BLN1     :text" (id '0'..'9' or 'A'..'Z')
// Maidenhead locator -> the centre of its square (for a position beacon from the station grid when no GPS is used)
bool gridCentre(const std::string& grid, double& lat, double& lon);

// ---- audio ----
enum class PacketBaud { B300 = 300, B1200 = 1200, B9600 = 9600 };
struct PacketAudioCfg {
    PacketBaud baud = PacketBaud::B1200;
    double txDelayMs = 300;          // flags before the first frame (the rig's PTT and squelch need time)
    double txTailMs = 30;            // flags after the last
    double rate = 48000;
    double centreHz = 1700;          // 300 baud: audio centre of the two tones (1600 / 1800 Hz)
    double emphasisDb = 0;           // 1200 baud: the space tone this much louder (some rigs' pre-emphasis wants -6, a flat input 0)
};
// The frames back to back (one PTT): audio at cfg.rate, peak 1. 9600: baseband NRZ, Gaussian BT 0.5.
std::vector<float> packetAudio(const std::vector<ax25::Frame>& frames, const PacketAudioCfg& cfg);
double packetBwHz(PacketBaud b);     // occupied RF bandwidth for the TX gate: 16 kHz FM (1200), 20 kHz (9600), 500 Hz SSB (300)
// The gate for a frame: VHF / UHF FM channels centred on the dial, HF 300 baud on USB at centreHz. The duty cycle limit applies as for the keyboard modes.
bool aprsSend(HamSlotTx& tx, TxDuty& duty, const HamTxCheck& gate, const std::vector<ax25::Frame>& frames, const PacketAudioCfg& cfg,
              double level, double nowSec, std::string& err);

// ---- receive: AFSK at any speed and shift (HF 300 baud packet on USB audio; the VHF 1200 baud receiver is pktdemod::Afsk1200) ----
// Mark and space energies over one bit (sliding), the stronger is the level, a clock loop on the level changes, HDLC (NRZI) after it.
// The tones may sit off their nominal audio frequency by up to about a quarter of the shift (SSB tuning error): a slow loop centres them.
class AfskRx {
public:
    AfskRx(double rate, double markHz, double spaceHz, double baud, ax25::HdlcRx::Callback cb);
    void feed(const float* x, size_t n);
    void reset();
    double tuneHz() const { return tune_; }   // the tuning error found
private:
    void design();
    double rate_, mark_, space_, baud_, tune_ = 0;
    int L_ = 0, steady_ = 0;   // samples since the level last changed
    std::vector<float> hist_;
    size_t hpos_ = 0;
    double phM_ = 0, phS_ = 0;
    std::vector<std::complex<float>> zm_, zs_;   // the mixer outputs over the last bit (ring)
    std::complex<double> accM_, accS_;
    std::complex<float> lastM_, lastS_;
    double eAvgM_ = 0, eAvgS_ = 0;
    float ph_ = -0.5f;
    bool lev_ = false, prev_ = false;
    ax25::HdlcRx hdlc_;
};

// ---- the message session ----
// Outgoing messages are numbered (1..99999, as text) and sent again until acknowledged: after 30 s, then each gap doubled, at most
// maxTries times (APRS101 chapter 14 leaves the timing to the station; this is the usual decaying schedule). Incoming messages to us are
// acknowledged once per copy (the sender may not have heard our ack), shown once (duplicates by sender + number are dropped).
struct AprsMsgOut { std::string to, text, no; int tries = 0; double nextSec = 0; bool acked = false, rejected = false, gaveUp = false; };
struct AprsMsgIn  { std::string from, text, no; double atSec = 0; };
class AprsMessenger {
public:
    std::string myCall;              // with SSID, "LY1ABC-7"
    int maxTries = 5;
    double firstGapSec = 30;
    // Queue a message; returns its number ("" when the text or call is not valid; err says why).
    std::string send(const std::string& to, const std::string& text, double nowSec, std::string& err);
    // A received frame's information field from `from` (with SSID). Returns the info fields to transmit now (acks), possibly none.
    std::vector<std::string> onReceive(const std::string& from, const std::string& info, double nowSec);
    // The info fields due now (first sends and retries); marks them sent.
    std::vector<std::string> due(double nowSec);
    const std::vector<AprsMsgOut>& outbox() const { return out_; }
    const std::vector<AprsMsgIn>& inbox() const { return in_; }
    void cancel(const std::string& no);
private:
    std::vector<AprsMsgOut> out_;
    std::vector<AprsMsgIn> in_;
    std::vector<std::string> seen_;  // FROM|NO of messages shown
    unsigned next_ = 1;
};

// ---- the beacon ----
// A position (and every Nth time a status) at a fixed interval: at least 60 s apart (10 min is usual for a fixed station, the default),
// stops after maxTx beacons without operator action, like the WSPR beacon.
struct AprsBeaconCfg {
    AprsPos pos;
    std::string status;              // sent every statusEvery-th beacon ("" never)
    int statusEvery = 6;
    double intervalSec = 600;
    int maxTx = 36;
};
class AprsBeacon {
public:
    AprsBeaconCfg cfg;
    void start(double nowSec) { on_ = true; sent_ = 0; next_ = nowSec; stopped_.clear(); }
    void stop() { on_ = false; }
    bool active() const { return on_; }
    void userAction() { sent_ = 0; }
    const std::string& stopReason() const { return stopped_; }
    // The info field to send now ("" nothing due); counts it as sent.
    std::string due(double nowSec);
private:
    bool on_ = false;
    int sent_ = 0, n_ = 0;
    double next_ = 0;
    std::string stopped_;
};

}  // namespace dect2::ham
