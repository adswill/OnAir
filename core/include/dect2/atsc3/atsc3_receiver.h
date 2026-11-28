// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The ATSC 3.0 receiver chain after the physical layer: baseband packets -> ALP -> IP/UDP -> low level signaling (service list) and ROUTE or
// MMTP for the selected service -> MP4 fragments -> transport stream (readTs(), the input of the HLS pipeline).
#pragma once
#include "atsc3_alp.h"
#include "atsc3_frame.h"
#include "atsc3_ip.h"
#include "atsc3_mmtp.h"
#include "atsc3_remux.h"
#include "atsc3_route.h"
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace dect2 {
namespace atsc3 {

struct ReceiverStats {
    long frames = 0, framesFailed = 0;
    long bbPackets = 0, bbBad = 0;
    long alpPackets = 0, alpIp = 0, alpSignaling = 0, alpTs = 0, alpCompressedIp = 0, alpOther = 0;
    long udp = 0, llsTables = 0, routeObjects = 0, objectsDelivered = 0;
    long llsSigned = 0;                    // LLS datagrams that were a SignedMultiTable (A/331 6.7)
    long slsObjects = 0, slsTables = 0;    // objects completed on the SLS channel (TSI 0), and the S-TSID / MPD / USBD documents read from them
    // PLPs backed off because they decoded nothing for several frames (decodePlan): times one was backed off, PLP frames in which only its probe
    // blocks were tried and none decoded, and the FEC blocks not tried in those frames
    long plpBackoffs = 0, plpProbeFrames = 0, blocksSkipped = 0;
    long mpusDelivered = 0, mpusDropped = 0;   // MMTP services: MPUs passed on (whole, or repaired down to the samples that can be played), and MPUs given up
    long lmtTables = 0;                    // link mapping tables read (ALP signalling)
    long plpsLeftOut = 0;                  // PLPs left out of a frame because they carry nothing the receiver needs (one per PLP and frame)
    long plpSelections = 0, plpRelearns = 0;   // switches from every PLP to only the needed ones, and back (plpDecodeMask())
};

struct FrameInfo {
    bool valid = false;
    int fftSize = 0, guard = 0, spDx = 0, spDy = 0, symbols = 0, preambleSymbols = 0;
    int bootstrapMinor = 0, bandwidthMhz = 0, preambleStructure = 0;
    struct Plp { int id = 0, bitsPerCell = 0, rate15 = 0, nInner = 0, blocks = 0, blocksOk = 0; bool leftOut = false; };   // leftOut: not decoded (plpDecodeMask())
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
    // Samples were lost between the frame committed last and the next one: the time interleavers and the packet reassembly start again.
    void lostSamples();
    // The plan for decoding frame number `frameIndex` (frames are numbered from 0 in the order they are committed; pushFrame and commitFrame count
    // them, failed frames included). A PLP that has decoded none of its FEC blocks in kDeadFrames frames in a row (kDeadFramesProtected for one
    // that carried the low level signaling or the selected service) is backed off: only kProbeBlocks (kProbeBlocksProtected) of its blocks are
    // tried in each frame until one of them decodes, and the PLP is then decoded in full again. The plan comes from the frames committed up to
    // number frameIndex - kPlanLag, not from the latest, so that it does not depend on how far the decoding threads have got (Atsc3Sync keeps no
    // more than kPlanLag frames in flight); the decoded result is the same for any number of threads.
    DecodePlan decodePlan(long frameIndex) const;
    long framesCommitted() const { std::lock_guard<std::mutex> lk(mu_); return st_.frames; }
    static constexpr int kPlanLag = 8, kDeadFrames = 4, kDeadFramesProtected = 8, kProbeBlocks = 4, kProbeBlocksProtected = 8;

    // Entry below the physical layer: the baseband packets of a PLP (header included), in order. A gap resets the packet reassembly.
    void pushBbPackets(int plpId, const std::vector<std::vector<uint8_t>>& packets, bool gapBefore = false);
    // Entry below the link layer: an IP packet.
    void pushIp(const uint8_t* packet, size_t size);

    std::vector<AeatAlert> alerts() const { std::lock_guard<std::mutex> lk(mu_); return alerts_; }   // the advanced emergency alerts in force (the last AEAT)
    std::vector<SltService> services() const;                 // from the low level signaling received so far
    bool selectService(int serviceId);                        // false until the service is known; may be called before, it is remembered
    bool selectServiceIfNone(int serviceId);                  // as selectService, but nothing happens when one is already selected (checked under the lock)
    int selectedService() const { std::lock_guard<std::mutex> lk(mu_); return selected_; }
    // When no service has been chosen, take the first visible linear video service (ROUTE or MMTP) as soon as the service list arrives.
    void setAutoSelect(bool on) { autoSelect_ = on; }
    bool serviceReady() const;                                // the service's signaling (S-TSID, or for MMTP the MP table and a first MPU) has arrived
    std::string mpd() const;
    // The components of the selected service. For an MMTP service, one per asset of the MP table: `tsi` is the asset's packet id.
    std::vector<RouteComponent> components() const;

    // PLP selection. While a service plays and the receiver knows which PLPs carry it (from the link mapping table, from the S-TSID, or from the
    // PLPs its packets came on while every PLP was decoded), only those PLPs and the ones with the low level signalling are decoded: the FEC of
    // the others is not run. Every PLP is decoded while no service plays or its signalling is still being read, for a few seconds after the PLPs
    // in L1 or the service's signalling change, when the service's packets stop coming although its PLPs decode cleanly, when a PLP carries an
    // MPEG-2 TS in ALP, with setDecodeAllPlps(true), and with the environment variable DECT2_ATSC3_ALL_PLPS=1.
    uint64_t plpDecodeMask() const { return plpMask_.load(std::memory_order_relaxed); }   // bit i set: PLP i is decoded
    void setDecodeAllPlps(bool on);

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
    int tsSeen_ = 0;                                          // ALP transport stream packets in a row, no IP packet between (sawTs_ needs several)
    void onAlp(const AlpPacket& a, int plpId);
    void pushIpFrom(const uint8_t* packet, size_t size, int plpId);
    void onUdp(const UdpDatagram& d, int plpId);
    void startRoute();
    void onObject(uint64_t gen, const RouteComponent& c, const RouteObject& o);
    void deliver(const std::string& key, bool init, bool repeatedInit, const std::vector<uint8_t>& data);   // to the remuxer, init segments once
    int componentId(const std::string& key);       // the remuxer's id of a component, given on first sight

    mutable std::mutex mu_;
    std::map<int, AlpReassembler> reasm_;
    IpReceiver ip_;
    std::vector<AeatAlert> alerts_;
    std::map<int, LlsTable> lls_;                  // by LLS_group_id (SLT only)
    std::vector<SltService> services_;
    int selected_ = -1;
    bool autoSelect_ = false;
    bool routeStarted_ = false;
    uint64_t routeGen_ = 0;                        // counts the routes made: objects of a route that a service change replaced are dropped (#650)
    std::shared_ptr<RouteService> route_;   // shared: onUdp keeps its copy while a service change (another thread) replaces route_
    std::shared_ptr<MmtpService> mmtp_;            // shared for the same reason; an MMTP service counts in routeGen_ as a route does
    std::map<std::string, int> remuxIds_;          // component key -> remux component id
    std::map<int, std::vector<uint8_t>> lastInit_;
    struct CtiPlp;
    PlpResult runCti(const PlpResult& in, int probe);   // de-interleaves a CTI PLP's cells in frame order and decodes the FEC blocks that are complete
    std::map<int, std::unique_ptr<CtiPlp>> cti_;   // by PLP id
    struct Straddle { std::vector<cf32> tail; std::vector<float> tailW; };
    PlpResult joinStraddle(const PlpResult& in);    // the block made of the tail of the last frame and the head of this one goes in front of the whole blocks
    std::map<int, Straddle> straddle_;
    Atsc3Remux remux_;
    ReceiverStats st_;
    FrameInfo info_;
    // the PLP backoff (decodePlan)
    struct PlpHealth { int deadFrames = 0; bool backedOff = false; long since = 0; };
    std::map<int, PlpHealth> health_;              // by PLP id
    std::set<int> llsPlps_, servicePlps_;          // PLPs that carried the low level signaling, and datagrams of the selected service
    std::deque<std::pair<long, DecodePlan>> plans_;   // the plan after each of the last frames committed, by the number of frames committed
    void updateHealth(const std::vector<const PlpResult*>& plps);   // after each frame, under mu_

    // PLP selection (plpDecodeMask()); all under mu_ but the mask
    void updatePlpSelection(const FrameResult& fr, const Bootstrap& bs, bool wantedClean, bool sawTs);   // after each decoded frame; also writes the choice into the frame's plan
    void choosePlpsLocked(const FrameResult& fr, const Bootstrap& bs, bool wantedClean, bool sawTs);
    void relearnLocked(const char* why);
    std::set<uint64_t> serviceFlowsLocked() const;          // (destination address << 16 | port) of the selected service's flows
    bool serviceReadyLocked() const;
    std::map<uint64_t, uint64_t> flowPlps_;                 // flow -> the PLPs it was seen on since the selection was last reset
    std::map<uint64_t, uint64_t> lmtPlps_;                  // flow -> PLP, from the last link mapping table
    std::array<double, 64> llsLast_ = filledLls();          // each PLP: when it last carried an LLS table or a link mapping table that parsed (-1: never)
    static std::array<double, 64> filledLls() { std::array<double, 64> a; a.fill(-1.0); return a; }
    std::string plpLayout_;                                 // the PLPs of the last frame, as text: a change resets the selection
    std::set<uint64_t> learnedFlows_;
    struct FlowRx { double last = 0, maxGap = 0; int seen = 0; };
    std::map<uint64_t, FlowRx> flowRx_;                     // each of the service's flows: when it last came and its longest gap
    double sigTime_ = 0, learnFrom_ = 0, lastServiceRx_ = 0, cleanSince_ = 0;
    int fullFrames_ = 0;                                    // frames with every PLP decoded since the selection was reset
    std::atomic<uint64_t> plpMask_{~0ull};
    std::atomic<bool> allPlps_{false};
};

} // namespace atsc3
} // namespace dect2
