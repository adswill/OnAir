// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/common/diag.h"
#include "dect2/common/stage_prof.h"
#include "dect2/common/thread_name.h"
#include <chrono>
#include "dect2/atsc3/atsc3_receiver.h"
#include "dect2/atsc3/atsc3_bb.h"
#include "atsc3_ldpc.h"
#include "dect2/atsc3/atsc3_ti.h"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace dect2 {
namespace atsc3 {

Atsc3Receiver::Atsc3Receiver() {
    const char* all = getenv("DECT2_ATSC3_ALL_PLPS");
    allPlps_ = all && *all && *all != '0';
}

Atsc3Receiver::~Atsc3Receiver() { remux_.stop(); }


// The state of a PLP in convolutional time interleaving mode, which lasts from frame to frame.
struct Atsc3Receiver::CtiPlp {
    int rows = 0, fec = -1, mod = -1, cod = -1;
    std::unique_ptr<CtiDeinterleaver> deint;
    std::unique_ptr<CtiDeinterleaver> deintW;   // the same de-interleaver for the cells' channel state weights (as real parts); null: none
    std::unique_ptr<Bicm> bicm;
    long pushed = 0;           // cells fed to the de-interleaver since it started; the transmit index of the next frame's first cell, counted from there
    long outPos = 0;           // position in the de-interleaved stream (one cell out per cell in) of buf[0]
    long nextBlock = -1;       // position in that stream where the next FEC block starts; -1 until known
    std::vector<cf32> buf;     // de-interleaved cells from outPos on, not yet cut into blocks
    std::vector<float> bufW;   // their weights (with deintW)
};

PlpResult Atsc3Receiver::runCti(const PlpResult& in, int probe) {
    DECT2_STAGE("a3.ti");
    PlpResult out;
    out.id = in.id;
    std::unique_ptr<CtiPlp>& up = cti_[in.id];
    if (!up) up.reset(new CtiPlp());
    CtiPlp* c = up.get();
    // the commutator carries on from the previous frame: another position means frames were missed
    // Channel state (4.1): each cell's weight goes through a second de-interleaver of the same shape, so that a cell from a deep fade enters the
    // LDPC decoder with the little confidence it deserves (the other time-interleaving modes had this in 4.0, docs/SENSITIVITY-4.0-X3.md). The
    // cells of the start-up (the de-interleaver's initial zeros) get weight 0. Without weights (DECT2_ATSC3_NOCSI) one noise figure per block.
    const bool haveW = in.cellW.size() == in.cells.size() && !in.cells.empty();
    if (!c->deint || c->rows != in.ctiRows || c->fec != in.cfg.fecType || c->mod != in.cfg.mod || c->cod != in.cfg.cod || c->deint->row() != in.ctiStartRow % c->rows ||
        (c->deintW != nullptr) != haveW) {
        up.reset(new CtiPlp());
        c = up.get();
        c->rows = in.ctiRows; c->fec = in.cfg.fecType; c->mod = in.cfg.mod; c->cod = in.cfg.cod;
        if (c->rows <= 1) {
            DIAG_DEBUG_EVERY(266505, 10.0, "ATSC 3.0 PLP %d announces a convolutional time interleaver with %d rows (at least 2 needed), so its data is skipped.", in.id, c->rows);
            return out;
        }
        c->bicm.reset(new Bicm(plpBicm(in.cfg)));
        if (!c->bicm->ok()) {
            DIAG_DEBUG_EVERY(759567, 10.0, "ATSC 3.0 PLP %d announces a modulation and code rate the receiver cannot set up, so its data is skipped.", in.id);
            c->bicm.reset();
            return out;
        }
        c->deint.reset(new CtiDeinterleaver(c->rows, in.ctiStartRow));
        if (haveW) c->deintW.reset(new CtiDeinterleaver(c->rows, in.ctiStartRow));
    }
    const int cb = c->bicm->cells();
    const long lat = c->deint->latency();
    // L1D_plp_CTI_fec_block_start = C + Nrows * ((start_row + C) mod Nrows), C the cells of the FEC block before the first complete one
    const long fbs = in.ctiFecBlockStart, row0 = in.ctiStartRow % c->rows;
    const long cStart = fbs - (long)c->rows * ((row0 + fbs % c->rows) % c->rows);
    if (cStart >= 0) {
        const long pos = c->pushed + cStart + lat;   // where the first complete FEC block of this frame is in the de-interleaved stream
        if (c->nextBlock < 0 || (pos - c->nextBlock) % cb != 0) c->nextBlock = pos;
    }
    // cells the equaliser blew up (a deep fade at a carrier makes the channel estimate near zero) are far outside the constellation: they are
    // treated as erased, which the LDPC decoder copes with; kept, they would be confident wrong bits. Far outside is beyond what the outermost
    // point plus 3 standard deviations of the PLP's noise reaches (and at least twice its power): at low SNR, or with the Enhanced layer of LDM in
    // the noise, a fixed limit of twice its power erased up to a quarter of the good cells, the most confident ones first.
    std::vector<cf32> cellsIn(in.cells);
    {
        float pmax = 0;
        for (const cf32& p : c->bicm->constellation()) pmax = std::max(pmax, std::norm(p));
        const float reach = std::sqrt(pmax) + 3.f * std::sqrt(std::max(in.noise, 0.f));
        const float lim = std::max(2.0f * pmax, reach * reach);
        for (size_t i = 0; i < cellsIn.size(); i++) if (!(std::norm(cellsIn[i]) <= lim)) cellsIn[i] = cf32(0, 0);
    }
    std::vector<cf32> d = c->deint->process(cellsIn);
    if (c->deintW) {
        std::vector<cf32> wIn(cellsIn.size());
        for (size_t i = 0; i < wIn.size(); i++) wIn[i] = cf32(cellsIn[i] == cf32(0, 0) ? 0.f : in.cellW[i], 0);
        const std::vector<cf32> dw = c->deintW->process(wIn);
        for (const cf32& v : dw) c->bufW.push_back(v.real());
    }
    c->pushed += (long)in.cells.size();
    c->buf.insert(c->buf.end(), d.begin(), d.end());
    std::vector<std::vector<cf32>> blocks;
    std::vector<std::vector<float>> wblocks;
    while (c->nextBlock >= 0 && c->nextBlock + cb <= c->outPos + (long)c->buf.size()) {
        if (c->nextBlock >= c->outPos) {
            const size_t o = (size_t)(c->nextBlock - c->outPos);
            blocks.emplace_back(c->buf.begin() + o, c->buf.begin() + o + cb);
            if (c->deintW) wblocks.emplace_back(c->bufW.begin() + o, c->bufW.begin() + o + cb);
        }
        c->nextBlock += cb;
    }
    if (c->nextBlock > c->outPos) {
        const size_t drop = std::min<size_t>((size_t)(c->nextBlock - c->outPos), c->buf.size());
        c->buf.erase(c->buf.begin(), c->buf.begin() + drop);
        if (c->deintW) c->bufW.erase(c->bufW.begin(), c->bufW.begin() + (long)std::min(drop, c->bufW.size()));
        c->outPos += (long)drop;
    }
    // the blocks are independent: they go to the decoder in batches, the batches on a few threads
    decodeFecBlocks(*c->bicm, blocks, in.noise, wblocks.empty() ? nullptr : &wblocks, probe, std::max(1, std::min(8, (int)std::thread::hardware_concurrency())), out);
    return out;
}


PlpResult Atsc3Receiver::joinStraddle(const PlpResult& in) {
    Straddle& st = straddle_[in.id];
    PlpResult out = in;
    out.head.clear(); out.tail.clear(); out.straddle = false;
    Bicm bicm(plpBicm(in.cfg));
    const int cb = bicm.ok() ? bicm.cells() : 0;
    if (cb > 0 && !in.head.empty()) {
        // the first block of the frame: the end of the one the frame before left open
        PlpResult first;
        if (in.skipped > 0) {   // the PLP is backed off and none of its probe blocks in this frame decoded: this one is not tried either
            first.blocks = 1;
            first.ok.push_back(false);
            out.skipped++;
        } else if (!st.tail.empty() && st.tail.size() + in.head.size() == (size_t)cb) {
            std::vector<cf32> cells(st.tail);
            cells.insert(cells.end(), in.head.begin(), in.head.end());
            std::vector<const cf32*> ptr{cells.data()};
            std::vector<std::vector<uint8_t>> pl;
            std::vector<char> good;
            // the channel state of both parts when both have it (docs/SENSITIVITY-4.0-X3.md)
            std::vector<float> w;
            if (st.tailW.size() == st.tail.size() && in.headW.size() == in.head.size() && !in.headW.empty()) { w = st.tailW; w.insert(w.end(), in.headW.begin(), in.headW.end()); }
            std::vector<const float*> wp{w.data()};
            bicm.decodeBatch(ptr, in.noise, pl, good, w.empty() ? nullptr : &wp);
            first.blocks = 1;
            first.ok.push_back(good[0] != 0);
            if (good[0]) { bbScramble(pl[0]); first.packets.push_back(bitsToBytes(pl[0])); first.blocksOk = 1; }
        } else {   // the frame before was lost or is not known yet: the block cannot be put together
            first.blocks = 1;
            first.ok.push_back(false);
        }
        out.ok.insert(out.ok.begin(), first.ok.begin(), first.ok.end());
        out.packets.insert(out.packets.begin(), first.packets.begin(), first.packets.end());
        out.blocks += first.blocks; out.blocksOk += first.blocksOk;
    }
    st.tail = in.tail;
    st.tailW = in.tailW;
    return out;
}

bool Atsc3Receiver::pushFrame(const cf32* x, size_t n, const Bootstrap& bs) {
    const DecodePlan plan = decodePlan(framesCommitted());
    return commitFrame(decodeFrame(x, n, bs, &plan), bs);
}

DecodePlan Atsc3Receiver::decodePlan(long frameIndex) const {
    std::lock_guard<std::mutex> lk(mu_);
    const long c = frameIndex - kPlanLag;   // the plan after this many frames were committed
    if (c <= 0 || plans_.empty()) return DecodePlan();
    for (auto it = plans_.rbegin(); it != plans_.rend(); ++it)
        if (it->first == c) return it->second;
    // not kept (frames committed out of turn, or more than kPlanLag frames in flight): the nearest one
    DIAG_DEBUG_EVERY(248686, 10.0, "The ATSC 3.0 decode plan for frame %ld is not known (plans kept for frames %ld to %ld), so the nearest one is used.", frameIndex, plans_.front().first + kPlanLag, plans_.back().first + kPlanLag);
    return c < plans_.front().first ? plans_.front().second : plans_.back().second;
}

void Atsc3Receiver::updateHealth(const std::vector<const PlpResult*>& plps) {
    for (const PlpResult* p : plps) {
        if (p->blocks <= 0) continue;   // no complete block in this frame (a time interleaver that is still filling, or a PLP not decoded): no news
        PlpHealth& h = health_[p->id];
        if (p->skipped > 0) { st_.plpProbeFrames++; st_.blocksSkipped += p->skipped; }
        if (p->blocksOk > 0) {
            if (h.backedOff)
                DIAG_DEBUG_EVERY(693135, 10.0, "ATSC 3.0 PLP %d decodes again after %ld frames in which only a few of its FEC blocks were tried, so it is decoded in full.", p->id, st_.frames - h.since);
            h = PlpHealth();
            continue;
        }
        h.deadFrames++;
        const bool prot = llsPlps_.count(p->id) || servicePlps_.count(p->id);
        if (!h.backedOff && h.deadFrames >= (prot ? kDeadFramesProtected : kDeadFrames)) {
            h.backedOff = true;
            h.since = st_.frames;
            st_.plpBackoffs++;
            DIAG_EVERY(406502, 30.0, "ATSC 3.0 PLP %d decoded none of its FEC blocks in %d frames in a row, so only %d of its blocks per frame are tried until one of them decodes.", p->id, h.deadFrames,
                       prot ? kProbeBlocksProtected : kProbeBlocks);
        }
    }
    DecodePlan plan;
    for (auto& [id, h] : health_)
        if (h.backedOff) plan.probe.push_back({id, llsPlps_.count(id) || servicePlps_.count(id) ? kProbeBlocksProtected : kProbeBlocks});
    plan.leftOut = ~plpMask_.load(std::memory_order_relaxed);   // the PLP selection as it stands; updatePlpSelection() writes this frame's choice
    plans_.emplace_back(st_.frames, plan);
    while (plans_.size() > 4 * (size_t)kPlanLag) plans_.pop_front();
}

void Atsc3Receiver::lostSamples() {
    std::lock_guard<std::mutex> lk(mu_);
    cti_.clear();
    straddle_.clear();
    for (auto& r : reasm_) r.second.reset();
}

bool Atsc3Receiver::commitFrame(const FrameResult& fr, const Bootstrap& bs) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        st_.frames++;
        if (!fr.ok) {
            st_.framesFailed++;
            cti_.clear();
            straddle_.clear();
            updateHealth({});
            cleanSince_ = sigTime_;
            DIAG_DEBUG_EVERY(209186, 5.0, "An ATSC 3.0 frame could not be decoded (%llu failed of %llu so far), so its data is dropped and the time interleavers restart.", (unsigned long long)st_.framesFailed, (unsigned long long)st_.frames);
            return false;
        }
        FrameInfo fi;
        fi.valid = true;
        fi.fftSize = fr.subframe.fftSize; fi.guard = fr.subframe.guard;
        fi.spDx = spDx(fr.subframe.spPattern); fi.spDy = spDy(fr.subframe.spPattern);
        fi.symbols = fr.subframe.numSymbols; fi.preambleSymbols = fr.preamble.numSymbols;
        fi.bootstrapMinor = bs.minorVersion; fi.bandwidthMhz = bs.systemBandwidth == 0 ? 6 : bs.systemBandwidth == 1 ? 7 : 8; fi.preambleStructure = bs.preambleStructure;
        for (size_t i = 0; i < fr.plps.size(); i++) {
            const PlpResult& pr = fr.plps[i];
            BicmConfig bc = plpBicm(pr.cfg);
            FrameInfo::Plp pi;
            pi.id = pr.id; pi.bitsPerCell = bc.bitsPerCell; pi.rate15 = bc.rate15; pi.nInner = bc.nInner;
            pi.blocks = pr.blocks; pi.blocksOk = pr.blocksOk; pi.leftOut = pr.leftOut;
            fi.plps.push_back(pi);
        }
        info_ = fi;
    }
    std::vector<PlpResult> ctiOut;
    for (auto& p : fr.plps) {
        if (p.leftOut) {   // left out on purpose: its interleaver and packet reassembly start afresh when it is decoded again
            std::lock_guard<std::mutex> lk(mu_);
            st_.plpsLeftOut++;
            cti_.erase(p.id);
            straddle_.erase(p.id);
            reasm_[p.id].reset();
            continue;
        }
        if (p.cti) ctiOut.push_back(runCti(p, fr.plan.probeBlocks(p.id))); else if (p.straddle) ctiOut.push_back(joinStraddle(p));
    }
    size_t ci = 0;
    std::vector<const PlpResult*> todo;
    for (auto& p : fr.plps) if (!p.leftOut) todo.push_back(p.cti || p.straddle ? &ctiOut[ci++] : &p);
    {
        std::lock_guard<std::mutex> lk(mu_);
        updateHealth(todo);
    }
    bool wantedClean = true;   // every PLP that was decoded (those the selection keeps) came without a failed FEC block in this frame
    for (const PlpResult* pp : todo) {
        const PlpResult& p = *pp;
        if (p.blocksOk < p.blocks) wantedClean = false;
        // blocks that failed leave a gap: the packet reassembly restarts after it
        std::vector<std::vector<uint8_t>> run;
        size_t j = 0;
        bool gap = false;
        for (size_t i = 0; i < p.ok.size(); i++) {
            if (p.ok[i]) { run.push_back(p.packets[j++]); continue; }
            if (!run.empty() || gap) { pushBbPackets(p.id, run, gap); run.clear(); }
            gap = true;
            std::lock_guard<std::mutex> lk(mu_);
            st_.bbBad++;
        }
        if (!run.empty()) pushBbPackets(p.id, run, gap);
    }
    bool sawTs;
    { std::lock_guard<std::mutex> lk(tsMu_); sawTs = sawTs_; }
    updatePlpSelection(fr, bs, wantedClean, sawTs);
    return true;
}

void Atsc3Receiver::pushBbPackets(int plpId, const std::vector<std::vector<uint8_t>>& packets, bool gap) {
    std::vector<AlpPacket> out;
    {
        std::lock_guard<std::mutex> lk(mu_);
        AlpReassembler& r = reasm_[plpId];
        if (gap) r.reset();
        for (auto& pk : packets) {
            st_.bbPackets++;
            BbHeader h;
            if (!parseBbHeader(pk.data(), (int)pk.size(), h)) {
                DIAG_DEBUG_EVERY(442689, 5.0, "An ATSC 3.0 baseband packet of %zu bytes on PLP %d has an invalid header, so it is dropped and ALP reassembly restarts.", pk.size(), plpId);
                st_.bbBad++;
                r.reset();
                continue;
            }
            r.push(pk.data() + h.headerBytes, (int)pk.size() - h.headerBytes, h.pointer, out);
        }
    }
    for (auto& a : out) onAlp(a, plpId);
}

namespace {
constexpr int kTsPacketsToSwitch = 16;   // ALP packets of transport stream in a row (up to 16 TS packets each) before the reader takes that stream
}

void Atsc3Receiver::onAlp(const AlpPacket& a, int plpId) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        st_.alpPackets++;
        if (a.type == AlpIpv4) st_.alpIp++;
        else if (a.type == AlpSignaling) st_.alpSignaling++;
        else if (a.type == AlpTs) st_.alpTs++;
        else if (a.type == AlpCompressedIp) st_.alpCompressedIp++;
        else st_.alpOther++;
        if (a.type == AlpSignaling && a.signalingType == 1 && a.signalingFormat == 0 && a.signalingEncoding == 0) {   // link mapping table
            std::vector<LmtFlow> flows;
            if (parseLmt(a.data.data(), a.data.size(), flows)) {
                st_.lmtTables++;
                // only a table that parses marks its PLP as a signalling PLP: a damaged packet that looks like signalling must not keep a PLP
                if (plpId >= 0 && plpId < 64) llsLast_[plpId] = sigTime_;
                lmtPlps_.clear();
                for (const LmtFlow& f : flows) lmtPlps_[((uint64_t)f.dstIp << 16) | (uint64_t)f.dstPort] |= 1ull << f.plp;
            } else DIAG_DEBUG_EVERY(735959, 10.0, "An ATSC 3.0 link mapping table of %zu bytes is cut short, so the PLPs of the IP flows are learnt from the packets instead.", a.data.size());
        }
    }
    if (a.type == AlpIpv4) {
        {   // transport stream packets between IP packets are damaged baseband packets, not a stream: they are forgotten, bytes and count
            std::lock_guard<std::mutex> lk(tsMu_);
            if (!sawTs_ && tsSeen_ > 0) { tsSeen_ = 0; tsDirect_.clear(); }
        }
        pushIpFrom(a.data.data(), a.data.size(), plpId);
    } else if (a.type == AlpTs) {
        std::lock_guard<std::mutex> lk(tsMu_);
        // The reader switches to this stream for good, so one packet is not enough: a damaged baseband packet can look like one. It takes
        // kTsPacketsToSwitch of them with no IP packet between. A service whose ROUTE media already play is not switched at all; reading only
        // this stream would leave the remuxed one unread and stopped.
        if (!sawTs_ && ++tsSeen_ >= kTsPacketsToSwitch && !remux_.started()) sawTs_ = true;
        if (!sawTs_ && remux_.started()) return;   // not this service's stream
        if (tsDirect_.size() < (size_t)64 << 20) tsDirect_.insert(tsDirect_.end(), a.data.begin(), a.data.end());
        else DIAG_DEBUG_EVERY(546455, 10.0, "The ATSC 3.0 transport stream buffer holds 64 MB that nobody has read, so incoming transport stream data is dropped.");
        tsCv_.notify_all();
    }
}

int Atsc3Receiver::readTsTimed(uint8_t* buf, int size, int timeoutMs) {
    {
        std::unique_lock<std::mutex> lk(tsMu_);
        if (sawTs_) {
            tsCv_.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] { return !tsDirect_.empty() || stopped_; });
            if (stopped_ && tsDirect_.empty()) return -1;
            const int n = (int)std::min<size_t>(tsDirect_.size() / 188 * 188, (size_t)size / 188 * 188);
            std::copy(tsDirect_.begin(), tsDirect_.begin() + n, buf);
            tsDirect_.erase(tsDirect_.begin(), tsDirect_.begin() + n);
            return n;
        }
    }
    return remux_.readTimed(buf, size, timeoutMs);
}

void Atsc3Receiver::pushIp(const uint8_t* p, size_t n) { pushIpFrom(p, n, -1); }

void Atsc3Receiver::pushIpFrom(const uint8_t* p, size_t n, int plpId) {
    UdpDatagram d;
    bool got;
    {
        std::lock_guard<std::mutex> lk(mu_);
        got = ip_.push(p, n, d);
        if (got) st_.udp++;
    }
    if (got) onUdp(d, plpId);
}

void Atsc3Receiver::onUdp(const UdpDatagram& d, int plpId) {
    const uint64_t flow = ((uint64_t)d.dstIp << 16) | (uint64_t)d.dstPort;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (plpId >= 0 && plpId < 64) flowPlps_[flow] |= 1ull << plpId;
        if (learnedFlows_.count(flow)) {
            lastServiceRx_ = sigTime_;
            FlowRx& r = flowRx_[flow];
            if (r.seen++) r.maxGap = std::max(r.maxGap, sigTime_ - r.last);
            r.last = sigTime_;
        }
    }
    if (d.dstIp == kLlsAddress && d.dstPort == kLlsPort) {
        // a station that signs its signalling (A/360) sends its tables inside a SignedMultiTable: every table in it counts
        const auto tables = parseLlsAll(d.payload.data(), d.payload.size());
        bool start = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (plpId >= 0) llsPlps_.insert(plpId);   // never backed off while it decodes at all
            if (!tables.empty() && plpId >= 0 && plpId < 64) llsLast_[plpId] = sigTime_;   // only LLS that parses marks a signalling PLP
            if (d.payload.size() >= 4 && d.payload[0] == 0xFE) st_.llsSigned++;
            for (const auto& t : tables) {
                st_.llsTables++;
                if (t.tableId == 4) { alerts_ = parseAeat(t.xml); continue; }
                if (t.tableId != 1) continue;
                lls_[t.groupId] = t;
                services_.clear();
                for (auto& g : lls_) services_.insert(services_.end(), g.second.slt.services.begin(), g.second.slt.services.end());
                start = start || (selected_ >= 0 && !routeStarted_);
                if (selected_ < 0 && autoSelect_) {
                    for (auto& sv : services_)
                        if (sv.category == 1 && !sv.hidden && (sv.slsProtocol == 1 || sv.slsProtocol == 2)) { selected_ = sv.serviceId; start = true; break; }
                }
            }
        }
        if (start) startRoute();
        return;
    }
    std::shared_ptr<RouteService> r;
    std::shared_ptr<MmtpService> m;
    {
        std::lock_guard<std::mutex> lk(mu_);
        r = route_;
        m = mmtp_;
    }
    bool mine = r && r->push(d);
    if (m && m->push(d)) mine = true;
    if (mine && plpId >= 0) {   // the selected service's signaling or media: never backed off while it decodes at all
        std::lock_guard<std::mutex> lk(mu_);
        servicePlps_.insert(plpId);
    }
}

bool Atsc3Receiver::selectService(int id) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        selected_ = id;
        relearnLocked("another service was chosen");
        routeStarted_ = false;
        route_.reset();
        servicePlps_.clear();
        mmtp_.reset();
        remuxIds_.clear();
        lastInit_.clear();
    }
    remux_.newService();   // the component ids start again: the old service's components must not read the new one's segments
    startRoute();
    std::lock_guard<std::mutex> lk(mu_);
    return routeStarted_;
}

bool Atsc3Receiver::selectServiceIfNone(int id) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (selected_ >= 0) return routeStarted_;   // the receiver chose one itself from the signaling while the caller looked: do not restart its route
        selected_ = id;
        relearnLocked("another service was chosen");
        routeStarted_ = false;
        route_.reset();
        servicePlps_.clear();
        mmtp_.reset();
        remuxIds_.clear();
        lastInit_.clear();
    }
    remux_.newService();   // the component ids start again: the old service's components must not read the new one's segments
    startRoute();
    std::lock_guard<std::mutex> lk(mu_);
    return routeStarted_;
}

void Atsc3Receiver::startRoute() {
    std::lock_guard<std::mutex> lk(mu_);
    if (routeStarted_) return;
    for (auto& s : services_) {
        if (s.serviceId != selected_) continue;
        if (s.slsProtocol == 2) {   // MMTP: the video and audio assets; captions and the other signalling are not read yet
            auto m = std::make_shared<MmtpService>(s.slsSrcIp, s.slsDstIp, s.slsDstPort);
            const uint32_t ip = s.slsDstIp;
            const int port = s.slsDstPort;
            auto key = [ip, port](uint16_t pid) { char k[64]; snprintf(k, sizeof k, "%08x:%d:p%u", ip, port, (unsigned)pid); return std::string(k); };
            const uint64_t gen = ++routeGen_;
            // as for a route: what the service that was just left still delivers is dropped
            auto current = [this, gen]() { std::lock_guard<std::mutex> lk(mu_); return gen == routeGen_ && mmtp_; };
            m->onObject = [this, key, current](uint16_t pid, const std::string&, bool init, const std::vector<uint8_t>& data) { if (current()) deliver(key(pid), init, false, data); };
            m->onAsset = [this, key, current](uint16_t pid, const MmtpAsset&) { if (current()) remux_.addComponent(componentId(key(pid))); };
            mmtp_ = m;
            routeStarted_ = true;
            return;
        }
        if (s.slsProtocol != 1) {
            DIAG_EVERY(923174, 30.0, "ATSC 3.0 service %d uses a signalling protocol (%d) that is neither ROUTE nor MMTP, so it cannot be played.", s.serviceId, (int)s.slsProtocol);
            return;
        }
        route_ = std::make_shared<RouteService>(s.slsSrcIp, s.slsDstIp, s.slsDstPort);
        const uint64_t gen = ++routeGen_;
        route_->onObject = [this, gen](const RouteComponent& c, const RouteObject& o) { onObject(gen, c, o); };
        routeStarted_ = true;
        return;
    }
}

void Atsc3Receiver::onObject(uint64_t gen, const RouteComponent& c, const RouteObject& o) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (gen != routeGen_ || !route_) return;   // an object of the service that was just left: its component numbers would mix into the new service
        st_.routeObjects++;
    }
    char key[64];
    snprintf(key, sizeof key, "%08x:%d:%u", c.dstIp, c.dstPort, c.tsi);
    // an initialization segment: by its code point, or (not every sender uses the code points of the S-TSID) by its first box
    const bool boxInit = o.data.size() >= 8 && (memcmp(o.data.data() + 4, "ftyp", 4) == 0 || memcmp(o.data.data() + 4, "moov", 4) == 0);
    const bool init = (o.codePoint >= 5 && o.codePoint <= 7) || boxInit;
    deliver(key, init, o.codePoint == 7, o.data);
}

int Atsc3Receiver::componentId(const std::string& key) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = remuxIds_.find(key);
    if (it != remuxIds_.end()) return it->second;
    const int id = (int)remuxIds_.size() + 1;
    remuxIds_[key] = id;
    return id;
}

void Atsc3Receiver::deliver(const std::string& key, bool init, bool repeatedInit, const std::vector<uint8_t>& data) {
    const int id = componentId(key);
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (init) {
            if (repeatedInit && lastInit_.count(id)) return;   // a repeat of the init segment
            if (lastInit_.count(id) && lastInit_[id] == data) return;
            lastInit_[id] = data;
        } else if (!lastInit_.count(id)) {
            DIAG_DEBUG_EVERY(772844, 10.0, "An ATSC 3.0 media segment arrived on component %d before its initialization segment, so it is dropped until the initialization segment is received.", id);
            return;   // media before the init segment cannot be played
        }
        st_.objectsDelivered++;
    }
    remux_.addComponent(id);
    remux_.push(id, data);
}

// ---- PLP selection

namespace {
uint64_t flowKey(uint32_t ip, int port) { return ((uint64_t)ip << 16) | (uint64_t)(port & 0xFFFF); }
std::string plpList(uint64_t m) {
    std::string out;
    for (int i = 0; i < 64; i++) if ((m >> i) & 1) out += (out.empty() ? "" : ", ") + std::to_string(i);
    return out.empty() ? "none" : out;
}
} // namespace

void Atsc3Receiver::setDecodeAllPlps(bool on) {
    allPlps_ = on;
    std::lock_guard<std::mutex> lk(mu_);
    if (on) relearnLocked("every PLP was asked for");
}

void Atsc3Receiver::relearnLocked(const char* why) {
    flowPlps_.clear();
    fullFrames_ = 0;
    learnFrom_ = sigTime_;
    flowRx_.clear();   // gaps are measured anew: a flow that has ended for good is not waited for
    if (plpMask_.load(std::memory_order_relaxed) != ~0ull) {
        st_.plpRelearns++;
        DIAG_DEBUG_EVERY(544842, 10.0, "The ATSC 3.0 receiver decodes every PLP again (%s) and learns anew which ones the service needs.", why);
    }
    plpMask_ = ~0ull;
}

std::set<uint64_t> Atsc3Receiver::serviceFlowsLocked() const {
    std::set<uint64_t> f;
    if (selected_ < 0 || !routeStarted_) return f;
    for (const SltService& s : services_) if (s.serviceId == selected_) f.insert(flowKey(s.slsDstIp, s.slsDstPort));
    if (route_) for (const RouteComponent& c : route_->components()) f.insert(flowKey(c.dstIp, c.dstPort));
    return f;
}

// The choice goes into the plan of the frame just committed (decodePlan), so it takes effect kPlanLag frames later, at the same frame for any
// number of decoding threads.
void Atsc3Receiver::updatePlpSelection(const FrameResult& fr, const Bootstrap& bs, bool wantedClean, bool sawTs) {
    std::lock_guard<std::mutex> lk(mu_);
    choosePlpsLocked(fr, bs, wantedClean, sawTs);
    if (!plans_.empty() && plans_.back().first == st_.frames) plans_.back().second.leftOut = ~plpMask_.load(std::memory_order_relaxed);
}

void Atsc3Receiver::choosePlpsLocked(const FrameResult& fr, const Bootstrap& bs, bool wantedClean, bool sawTs) {
    const PreambleResult& pre = fr.preamble;
    const size_t len = frameLengthSamples(bs, pre.basic, pre.detail);
    sigTime_ += (double)bs.numSymbols * 3072.0 / 6.144e6 + (len ? (double)len / postBootstrapRate(bs) : 0.0);
    if (!wantedClean) cleanSince_ = sigTime_;
    // the PLPs of the frame, and those L1 marks as carrying the low level signalling (L1D_plp_lls_flag)
    uint64_t all = 0, lls = 0;
    std::string layout;
    for (const L1DetailSubframe& sf : pre.detail.subframes) {
        for (const L1DetailPlp& p : sf.plps) {
            if (p.id < 0 || p.id > 63) continue;
            all |= 1ull << p.id;
            if (p.llsFlag) lls |= 1ull << p.id;
            char b[80];
            snprintf(b, sizeof b, "%d:%d:%d:%d:%d:%d:%d;", p.id, p.layer, p.fecType, p.mod, p.cod, p.tiMode, p.llsFlag);
            layout += b;
        }
    }
    if (layout != plpLayout_) {
        if (!plpLayout_.empty()) {
            relearnLocked("the PLPs in L1 changed");
            llsLast_.fill(-1.0);
        }
        plpLayout_ = layout;
    }
    const std::set<uint64_t> flows = serviceFlowsLocked();
    if (flows != learnedFlows_) {
        relearnLocked("the selected service or its signalling changed");
        learnedFlows_ = flows;
        lastServiceRx_ = sigTime_;
        flowRx_.clear();
    }
    bool full = true;
    for (const PlpResult& p : fr.plps) full = full && !p.leftOut;
    if (full) fullFrames_++;
    const uint64_t cur = plpMask_.load(std::memory_order_relaxed);
    // a service whose packets stop coming while its PLPs decode cleanly may have moved to a PLP that is left out
    if (cur != ~0ull && sigTime_ - std::max(lastServiceRx_, cleanSince_) > 2.0) {
        relearnLocked("no packet of the selected service came for 2 s although its PLPs decode cleanly");
        return;
    }
    // ... and so may one of its flows alone (audio, or another ROUTE session) while the others stay: a flow that came at least twice and
    // then stays away for more than 2 s and 2.5 times its longest gap so far
    if (cur != ~0ull) {
        for (const auto& [f, r] : flowRx_) {
            const double quiet = sigTime_ - std::max(r.last, cleanSince_);
            if (r.seen >= 2 && quiet > std::max(2.0, 2.5 * r.maxGap)) {
                relearnLocked("a flow of the selected service stopped although its PLPs decode cleanly");
                return;
            }
        }
    }
    const char* why = nullptr;
    uint64_t want = 0;
    if (allPlps_) why = "every PLP was asked for";
    else if (sawTs) why = "a PLP carries an MPEG-2 transport stream";
    else if (flows.empty() || !serviceReadyLocked()) why = "no service is playing";
    else if (fullFrames_ < 4 || sigTime_ - learnFrom_ < 2.0) why = "the service's PLPs are still being learnt";
    else {
        // the PLPs that carried signalling in the last 30 s of signal: a signalling PLP is kept, so it keeps coming; a PLP taken for one by
        // mistake (a damaged packet that parsed) is left out and ages out of the set
        uint64_t llsSeen = 0;
        for (int i = 0; i < 64; i++) if (llsLast_[i] >= 0 && sigTime_ - llsLast_[i] < 30.0) llsSeen |= 1ull << i;
        want = (lls | llsSeen) & all;
        if (!want) why = "no PLP is known to carry the low level signalling";
        for (uint64_t f : flows) {
            if (why) break;
            uint64_t m = 0;
            auto a = flowPlps_.find(f);
            if (a != flowPlps_.end()) m |= a->second;
            auto b = lmtPlps_.find(f);
            if (b != lmtPlps_.end()) m |= b->second;
            if (route_)
                for (const RouteComponent& c : route_->components())
                        if (flowKey(c.dstIp, c.dstPort) == f && c.plpId >= 0 && c.plpId < 64) m |= 1ull << c.plpId;
            m &= all;
            if (!m) why = "a flow of the service has not been seen on any PLP yet";
            want |= m;
        }
    }
    if (why) {
        if (cur != ~0ull) relearnLocked(why);
        return;
    }
    const uint64_t next = (want & all) == all ? ~0ull : want;
    if (next == cur) return;
    if (next != ~0ull) {
        st_.plpSelections++;
        DIAG_DEBUG_EVERY(878483, 10.0, "ATSC 3.0 service %d: only PLP %s of the PLPs %s are decoded (the service and the low level signalling); the others are left out.", selected_, plpList(next).c_str(), plpList(all).c_str());
    }
    plpMask_ = next;
}

std::vector<SltService> Atsc3Receiver::services() const { std::lock_guard<std::mutex> lk(mu_); return services_; }
bool Atsc3Receiver::serviceReady() const { std::lock_guard<std::mutex> lk(mu_); return serviceReadyLocked(); }
bool Atsc3Receiver::serviceReadyLocked() const { return (route_ && route_->ready()) || (mmtp_ && mmtp_->ready()); }
std::string Atsc3Receiver::mpd() const { std::lock_guard<std::mutex> lk(mu_); return route_ ? route_->mpd() : std::string(); }
std::vector<RouteComponent> Atsc3Receiver::components() const {
    std::lock_guard<std::mutex> lk(mu_);
    if (route_) return route_->components();
    std::vector<RouteComponent> out;
    if (!mmtp_) return out;
    for (const MmtpAsset& a : mmtp_->assets()) {
        for (uint16_t pid : a.packetIds) {
            RouteComponent c;
            for (auto& s : services_) if (s.serviceId == selected_) { c.dstIp = s.slsDstIp; c.dstPort = s.slsDstPort; }
            c.tsi = pid;
            c.repId = a.id;
            c.contentType = assetKind(a.type);
            c.realTime = true;
            out.push_back(c);
        }
    }
    return out;
}
FrameInfo Atsc3Receiver::frameInfo() const { std::lock_guard<std::mutex> lk(mu_); return info_; }
ReceiverStats Atsc3Receiver::stats() const {
    std::lock_guard<std::mutex> lk(mu_);
    ReceiverStats s = st_;
    if (route_) { s.slsObjects = route_->slsObjects(); s.slsTables = route_->slsTables(); }
    if (mmtp_) { const MmtpStats ms = mmtp_->stats(); s.mpusDelivered = ms.mpusDelivered; s.mpusDropped = ms.mpusDropped; }
    return s;
}

} // namespace atsc3
} // namespace dect2
