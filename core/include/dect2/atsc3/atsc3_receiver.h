// The ATSC 3.0 receiver chain after the physical layer: baseband packets -> ALP -> IP/UDP -> low level signaling (service list) and ROUTE for
// the selected service -> MP4 fragments -> transport stream (readTs(), the input of the HLS pipeline).
#pragma once
#include "atsc3_alp.h"
#include "atsc3_frame.h"
#include "atsc3_ip.h"
#include "atsc3_remux.h"
#include "atsc3_route.h"
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace dect2 {
namespace atsc3 {

struct ReceiverStats {
    long frames = 0, framesFailed = 0;
    long bbPackets = 0, bbBad = 0;
    long alpPackets = 0, alpIp = 0, alpSignaling = 0, alpTs = 0, alpCompressedIp = 0, alpOther = 0;
    long udp = 0, llsTables = 0, routeObjects = 0, objectsDelivered = 0;
};

struct FrameInfo {
    bool valid = false;
    int fftSize = 0, guard = 0, spDx = 0, spDy = 0, symbols = 0, preambleSymbols = 0;
    int bootstrapMinor = 0, bandwidthMhz = 0, preambleStructure = 0;
    struct Plp { int id = 0, bitsPerCell = 0, rate15 = 0, nInner = 0, blocks = 0, blocksOk = 0; };
    std::vector<Plp> plps;
};

class Atsc3Receiver {
public:
    Atsc3Receiver();
    ~Atsc3Receiver();

    // A frame's samples (after the bootstrap, at the frame's sample rate); returns false if the frame could not be decoded.
    bool pushFrame(const cf32* x, size_t n, const Bootstrap& bs);
    // The second half of pushFrame: hands a decoded frame to the link layer. Frames must be committed in order, one at a time.
    bool commitFrame(const FrameResult& fr, const Bootstrap& bs);
    // Entry below the physical layer: the baseband packets of a PLP (header included), in order. A gap resets the packet reassembly.
    void pushBbPackets(int plpId, const std::vector<std::vector<uint8_t>>& packets, bool gapBefore = false);
    // Entry below the link layer: an IP packet.
    void pushIp(const uint8_t* packet, size_t size);

    std::vector<AeatAlert> alerts() const { std::lock_guard<std::mutex> lk(mu_); return alerts_; }   // the advanced emergency alerts in force (the last AEAT)
    std::vector<SltService> services() const;                 // from the low level signaling received so far
    bool selectService(int serviceId);                        // false until the service is known; may be called before, it is remembered
    bool selectServiceIfNone(int serviceId);                  // as selectService, but nothing happens when one is already selected (checked under the lock)
    int selectedService() const { return selected_; }
    // When no service has been chosen, take the first visible linear video service (ROUTE) as soon as the service list arrives.
    void setAutoSelect(bool on) { autoSelect_ = on; }
    bool serviceReady() const;                                // the service's signaling (S-TSID) has arrived
    std::string mpd() const;
    std::vector<RouteComponent> components() const;

    int readTs(uint8_t* buf, int size) { return remux_.read(buf, size); }   // blocks; the Reader of HlsPipeline
    int readTsTimed(uint8_t* buf, int size, int timeoutMs);   // a stream of MPEG-2 TS in ALP (A/330 5.4) comes out here directly; otherwise the remuxed services
    void stop() { { std::lock_guard<std::mutex> lk(tsMu_); stopped_ = true; } tsCv_.notify_all(); remux_.stop(); }
    Atsc3Remux& remux() { return remux_; }
    ReceiverStats stats() const;
    FrameInfo frameInfo() const;                              // the last frame that decoded

private:
    std::mutex tsMu_;
    std::condition_variable tsCv_;
    std::deque<uint8_t> tsDirect_;
    bool sawTs_ = false, stopped_ = false;
    void onAlp(const AlpPacket& a);
    void onUdp(const UdpDatagram& d);
    void startRoute();
    void onObject(const RouteComponent& c, const RouteObject& o);

    mutable std::mutex mu_;
    std::map<int, AlpReassembler> reasm_;
    IpReceiver ip_;
    std::vector<AeatAlert> alerts_;
    std::map<int, LlsTable> lls_;                  // by LLS_group_id (SLT only)
    std::vector<SltService> services_;
    int selected_ = -1;
    bool autoSelect_ = false;
    bool routeStarted_ = false;
    std::unique_ptr<RouteService> route_;
    std::map<std::string, int> remuxIds_;          // component key -> remux component id
    std::map<int, std::vector<uint8_t>> lastInit_;
    struct CtiPlp;
    PlpResult runCti(const PlpResult& in);          // de-interleaves a CTI PLP's cells in frame order and decodes the FEC blocks that are complete
    std::map<int, std::unique_ptr<CtiPlp>> cti_;   // by PLP id
    struct Straddle { std::vector<cf32> tail; };
    PlpResult joinStraddle(const PlpResult& in);    // the block made of the tail of the last frame and the head of this one goes in front of the whole blocks
    std::map<int, Straddle> straddle_;
    Atsc3Remux remux_;
    ReceiverStats st_;
    FrameInfo info_;
};

} // namespace atsc3
} // namespace dect2
