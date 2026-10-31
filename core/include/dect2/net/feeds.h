// Data feeds out to other programs: ADS-B (SBS-1 BaseStation and Beast TCP servers), AIS (NMEA over UDP), APRS (APRS-IS igate
// client and a KISS TCP server), ACARS and VDL2 (acarsdec / dumpvdl2 JSON over UDP). All off until the user turns one on.
// The decoders hand their results to Feeds::get(); the network work runs on the feeds' own thread. A decoder never waits:
// when the thread or a client is slow, data is dropped (and counted).
#pragma once
#include "dect2/acars/acars_tel.h"
#include "dect2/adsb/adsb_msg.h"
#include "dect2/gnss/gnss_tel.h"
#include "dect2/net/aprs_gate.h"
#include "dect2/net/feed_targets.h"
#include "dect2/vdl2/vdl2_tel.h"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dect2 {

enum FeedId { kFeedSbs, kFeedBeast, kFeedAis, kFeedAprsIs, kFeedKiss, kFeedAcars, kFeedVdl2, kFeedGnss, kFeedBle, kFeedLora, kNumFeeds };

struct FeedConfig {
    bool on[kNumFeeds] = {};
    int sbsPort = 30003, beastPort = 30005, kissPort = 8001;   // TCP servers
    bool lan = false;                                          // the TCP servers also accept other computers (else this one only)
    std::string aisHost = "127.0.0.1"; int aisPort = 10110;    // UDP
    std::string acarsHost = "127.0.0.1"; int acarsPort = 5550; // UDP, acarsdec JSON (acarshub's ACARS port)
    std::string vdl2Host = "127.0.0.1"; int vdl2Port = 5555;   // UDP, dumpvdl2 JSON (acarshub's VDLM2 port)
    std::string gnssHost = "127.0.0.1"; int gnssPort = 10111;  // UDP, NMEA RMC/GGA
    std::string bleHost = "127.0.0.1"; int blePort = 5620;    // UDP JSON lines: mac, name, rssi
    std::string loraHost = "127.0.0.1"; int loraPort = 5621;  // UDP JSON lines: LoRa / LoRaWAN / Meshtastic
    std::string aprsCall, aprsPass;                            // APRS-IS login; the feed stays idle without a callsign
    std::string aprsServer = "rotate.aprs2.net"; int aprsServerPort = 14580;
    AprsGateConfig aprsGate;                                   // rate limits and the duplicate window of the iGate (aprs_gate.h)
    std::vector<FeedTarget> targets;                           // outbound aggregator targets (feed_targets.h), each off until enabled
    std::string stationId = "OnAir";                           // station_id in the ACARS / VDL2 JSON
};

struct FeedStatus {
    bool active = false;         // listening / socket open / logged in
    int clients = 0;             // TCP servers: connected clients
    uint64_t sent = 0;           // messages sent (to at least one client)
    uint64_t dropped = 0;        // messages thrown away (queue full, a slow client)
    std::string state;           // a few words: "listening on 30003", "connected, verified", ...
    std::string lastError;
    std::string detail;          // APRS-IS: why packets were not gated ("gated 12, duplicates 3, ...")
};

// One ADS-B frame that passed the checks (adsb_track.cpp fills it)
struct FeedAdsbFrame {
    uint8_t bytes[14] = {};
    int bits = 112;
    double timeSec = 0;          // signal time: the Beast 12 MHz counter
    float levelDbfs = -120;      // mean pulse power
    uint32_t icao = 0;
    bool nonIcao = false;        // DF18 anonymous / non-ICAO address
    adsb::Msg msg;
    bool hasPos = false;         // this frame completed a position
    double lat = 0, lon = 0;
};

// The formats, without any network (feeds_fmt.cpp)
namespace feedfmt {
// SBS-1 BaseStation "MSG,..." line with CRLF; "" when the frame has no SBS transmission type. wallMs: Unix milliseconds (local time is written).
std::string sbsLine(const FeedAdsbFrame& f, int64_t wallMs);
// Beast binary: 0x1A, '2' or '3', 6 byte 12 MHz timestamp, signal byte, the frame; every 0x1A after the first doubled
std::string beastFrame(const uint8_t* bytes, int bits, uint64_t ts12, uint8_t signal);
uint8_t beastSignal(float levelDbfs);    // amplitude 0 - 255 from the pulse power in dBFS
// KISS data frame (port 0): FEND 0x00 data FEND, FEND / FESC escaped
std::string kissFrame(const uint8_t* d, size_t n);
// APRS-IS: "FROM>TO,PATH,qAR,MYCALL:info\r\n", or "" when the packet must not be gated (NOGATE / RFONLY / TCPIP / TCPXX in the path,
// third-party, query, not a UI frame with PID F0)
std::string aprsIsLine(const std::string& from, const std::string& to, const std::string& path, const std::string& info, const std::string& myCall, bool uiF0);
std::string aprsIsLogin(const std::string& call, const std::string& pass);
// acarsdec style JSON (one line, no newline at the end)
std::string acarsJson(const AcarsMessage& m, const std::string& station);
// dumpvdl2 style JSON
std::string vdl2Json(const Vdl2Message& m, const std::string& station);
std::string jsonEscape(const std::string& s);
std::string gnssRmc(const GnssFix& f);
std::string gnssGga(const GnssFix& f);
std::string bleJson(const std::string& mac, const std::string& name, float rssi);
// kind: "lora", "meshtastic", "meshcore", "lorawan", ... extra: more JSON object fields without braces (may be empty)
std::string loraJson(const char* kind, const std::string& hex, float snrDb, double freqHz, int sf, const std::string& extra);
} // namespace feedfmt

class Feeds {
public:
    static Feeds& get();
    ~Feeds();
    void configure(const FeedConfig& c);     // starts or stops the thread and the sockets as needed
    FeedConfig config() const;
    FeedStatus status(int id) const;
    void stop();                             // closes everything (configure() starts again)

    // what the decoders call; cheap when the feed is off
    bool wantAdsb() const { return FeedTargets::get().wantAdsb() || want_[kFeedSbs].load(std::memory_order_relaxed) || want_[kFeedBeast].load(std::memory_order_relaxed); }
    bool wantAis() const { return want_[kFeedAis].load(std::memory_order_relaxed) || FeedTargets::get().wantAis(); }
    bool wantAprs() const { return want_[kFeedAprsIs].load(std::memory_order_relaxed) || want_[kFeedKiss].load(std::memory_order_relaxed); }
    bool wantKiss() const { return want_[kFeedKiss].load(std::memory_order_relaxed); }
    bool wantAcars() const { return want_[kFeedAcars].load(std::memory_order_relaxed); }
    bool wantVdl2() const { return want_[kFeedVdl2].load(std::memory_order_relaxed); }
    bool wantGnss() const { return want_[kFeedGnss].load(std::memory_order_relaxed); }
    bool wantBle() const { return want_[kFeedBle].load(std::memory_order_relaxed); }
    bool wantLora() const { return want_[kFeedLora].load(std::memory_order_relaxed); }
    void adsb(const FeedAdsbFrame& f);
    void ais(const std::vector<std::string>& sentences);
    void aprs(const std::vector<uint8_t>& ax25NoFcs, const std::string& from, const std::string& to, const std::string& path, const std::string& info, bool uiF0);
    void kiss(const uint8_t* d, size_t n);   // a frame for the KISS server only (CubeSat telemetry: AX.25 without FCS, or a CSP packet)
    void acars(const AcarsMessage& m);
    void vdl2(const Vdl2Message& m);
    void gnss(const GnssFix& f);
    void ble(const std::string& mac, const std::string& name, float rssi);
    void lora(const char* kind, const std::string& hex, float snrDb, double freqHz, int sf, const std::string& extra = {});

    struct Impl;
private:
    Feeds();
    void push(int id, std::string&& payload);
    std::atomic<bool> want_[kNumFeeds];
    std::unique_ptr<Impl> impl_;
};

} // namespace dect2
