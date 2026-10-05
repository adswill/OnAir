// ROUTE (A/331 Annex A): LCT packets over UDP carry delivery objects (files); this reads the LCT header, reassembles objects, reads the service
// layer signaling that arrives as a MIME package (USBD, S-TSID, MPD) on TSI 0, and routes the objects of each component of a service.
#pragma once
#include "atsc3_ip.h"
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

namespace dect2 {
namespace atsc3 {

struct LctPacket {
    int version = 1, psi = 2;
    uint32_t tsi = 0, toi = 0;
    int codePoint = 0;
    bool closeSession = false, closeObject = false;   // the A and B flags
    uint32_t startOffset = 0;                         // FEC Payload ID of source packets
    int64_t transferLength = -1;                      // EXT_TOL, -1 if absent
    bool hasSct = false;
    uint64_t sct = 0;                                 // 64-bit NTP time from EXT_TIME, if present
    std::vector<uint8_t> payload;
};

// Parses the UDP payload of a ROUTE packet; false when it is not a valid source packet.
bool parseRoutePacket(const uint8_t* data, size_t size, LctPacket& p);
// Builds a ROUTE packet (for tests): header with TSI, TOI, optional EXT_TOL (24 or 48 bit) and EXT_TIME.
std::vector<uint8_t> makeRoutePacket(const LctPacket& p, bool withToL = false);

struct RouteObject {
    uint32_t tsi = 0, toi = 0;
    int codePoint = 0;
    std::vector<uint8_t> data;
    bool hasSct = false;
    uint64_t sct = 0;
};

class RouteReceiver {
public:
    // Adds a packet; completed objects are appended to `done`. An object is complete when all bytes up to its length (EXT_TOL, or the packet
    // with the close object flag) have arrived.
    void push(const LctPacket& p, std::vector<RouteObject>& done);
    void clear() { open_.clear(); }
    size_t openObjects() const { return open_.size(); }
private:
    struct Open {
        std::vector<uint8_t> data;
        std::vector<std::pair<uint32_t, uint32_t>> ranges;   // received byte ranges, merged
        int64_t length = -1;
        int codePoint = 0;
        bool hasSct = false;
        uint64_t sct = 0;
        long age = 0;
    };
    std::map<std::pair<uint32_t, uint32_t>, Open> open_;
    long counter_ = 0;
};

// ---- MIME packages (unsigned package mode: multipart/related)
struct MimePart {
    std::map<std::string, std::string> headers;   // lower case names
    std::vector<uint8_t> body;
};
// Splits a multipart document; the first header block carries the boundary. Returns an empty list when it is not multipart.
std::vector<MimePart> parseMultipart(const std::vector<uint8_t>& doc);
std::vector<uint8_t> makeMultipart(const std::vector<MimePart>& parts, const std::string& boundary);

// ---- S-TSID
struct StsidPayload { int codePoint = 0, formatId = 0, frag = 0; bool order = false; };
struct StsidChannel {
    uint32_t tsi = 0;
    bool realTime = false;
    std::string repId, contentType, lang;
    bool startup = false;
    std::vector<StsidPayload> payloads;
};
struct StsidSession {
    uint32_t srcIp = 0, dstIp = 0;     // 0: the session that carries the SLS
    int dstPort = 0;
    std::vector<StsidChannel> channels;
};
struct Stsid { std::vector<StsidSession> sessions; };
bool parseStsid(const std::string& xml, Stsid& out);

// ---- one service: finds its signaling, learns the components and delivers their objects
struct RouteComponent {
    uint32_t dstIp = 0; int dstPort = 0; uint32_t tsi = 0;
    std::string repId, contentType, lang;
    bool realTime = false;
};

class RouteService {
public:
    // `slsIp`, `slsPort`: the ROUTE session announced by the SLT for this service
    RouteService(uint32_t slsSrcIp, uint32_t slsDstIp, int slsPort);
    // Feeds a UDP datagram of any session; returns true if it belonged to this service.
    bool push(const UdpDatagram& d);

    bool ready() const { return haveStsid_; }
    const std::vector<RouteComponent>& components() const { return components_; }
    const std::string& mpd() const { return mpd_; }
    const std::string& usbd() const { return usbd_; }

    // Called for every completed object of a component (init segments have codepoints 5 to 7, media segments 8 and 9).
    std::function<void(const RouteComponent&, const RouteObject&)> onObject;

private:
    void handleSls(const RouteObject& o);
    uint32_t slsSrc_, slsDst_;
    int slsPort_;
    bool haveStsid_ = false;
    Stsid stsid_;
    std::vector<RouteComponent> components_;
    std::string mpd_, usbd_;
    std::map<std::tuple<uint32_t, int, uint32_t>, std::unique_ptr<RouteReceiver>> rx_;   // per (ip, port, tsi)
    int stsidVersion_ = -1;
};

} // namespace atsc3
} // namespace dect2
