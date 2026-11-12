#include "dect2/common/stage_prof.h"
#include "dect2/common/thread_name.h"
#include <chrono>
#include "dect2/atsc3/atsc3_receiver.h"
#include "dect2/atsc3/atsc3_bb.h"
#include "atsc3_ldpc.h"
#include "dect2/atsc3/atsc3_ti.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <thread>

namespace dect2 {
namespace atsc3 {

Atsc3Receiver::Atsc3Receiver() {}

Atsc3Receiver::~Atsc3Receiver() { remux_.stop(); }


// The state of a PLP in convolutional time interleaving mode, which lasts from frame to frame.
struct Atsc3Receiver::CtiPlp {
    int rows = 0, fec = -1, mod = -1, cod = -1;
    std::unique_ptr<CtiDeinterleaver> deint;
    std::unique_ptr<Bicm> bicm;
    long pushed = 0;           // cells fed to the de-interleaver since it started; the transmit index of the next frame's first cell, counted from there
    long outPos = 0;           // position in the de-interleaved stream (one cell out per cell in) of buf[0]
    long nextBlock = -1;       // position in that stream where the next FEC block starts; -1 until known
    std::vector<cf32> buf;     // de-interleaved cells from outPos on, not yet cut into blocks
};

PlpResult Atsc3Receiver::runCti(const PlpResult& in) {
    DECT2_STAGE("a3.ti");
    PlpResult out;
    out.id = in.id;
    std::unique_ptr<CtiPlp>& up = cti_[in.id];
    if (!up) up.reset(new CtiPlp());
    CtiPlp* c = up.get();
    // the commutator carries on from the previous frame: another position means frames were missed
    if (!c->deint || c->rows != in.ctiRows || c->fec != in.cfg.fecType || c->mod != in.cfg.mod || c->cod != in.cfg.cod || c->deint->row() != in.ctiStartRow % c->rows) {
        up.reset(new CtiPlp());
        c = up.get();
        c->rows = in.ctiRows; c->fec = in.cfg.fecType; c->mod = in.cfg.mod; c->cod = in.cfg.cod;
        if (c->rows <= 1) return out;
        c->bicm.reset(new Bicm(plpBicm(in.cfg)));
        if (!c->bicm->ok()) { c->bicm.reset(); return out; }
        c->deint.reset(new CtiDeinterleaver(c->rows, in.ctiStartRow));
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
    // treated as erased, which the LDPC decoder copes with; kept, they would be confident wrong bits
    std::vector<cf32> cellsIn(in.cells);
    {
        float pmax = 0;
        for (const cf32& p : c->bicm->constellation()) pmax = std::max(pmax, std::norm(p));
        const float lim = 2.0f * pmax;
        for (cf32& v : cellsIn) if (!(std::norm(v) <= lim)) v = cf32(0, 0);
    }
    std::vector<cf32> d = c->deint->process(cellsIn);
    c->pushed += (long)in.cells.size();
    c->buf.insert(c->buf.end(), d.begin(), d.end());
    std::vector<std::vector<cf32>> blocks;
    while (c->nextBlock >= 0 && c->nextBlock + cb <= c->outPos + (long)c->buf.size()) {
        if (c->nextBlock >= c->outPos) {
            const size_t o = (size_t)(c->nextBlock - c->outPos);
            blocks.emplace_back(c->buf.begin() + o, c->buf.begin() + o + cb);
        }
        c->nextBlock += cb;
    }
    if (c->nextBlock > c->outPos) {
        const size_t drop = std::min<size_t>((size_t)(c->nextBlock - c->outPos), c->buf.size());
        c->buf.erase(c->buf.begin(), c->buf.begin() + drop);
        c->outPos += (long)drop;
    }
    // the blocks are independent: they go to the decoder in batches (one block per vector lane), the batches on a few threads
    const size_t n = blocks.size();
    std::vector<std::vector<uint8_t>> bits(n);
    std::vector<char> ok(n, 0);
    const Bicm& bicm = *c->bicm;
    const float in_noise = in.noise;
    const size_t bs = (size_t)Ldpc::kBatch, nBatches = (n + bs - 1) / bs;
    auto work = [&](std::atomic<size_t>& next) {
        for (size_t k; (k = next++) < nBatches;) {
            const size_t from = k * bs, to = std::min(n, from + bs);
            std::vector<const cf32*> in;
            for (size_t i = from; i < to; i++) in.push_back(blocks[i].data());
            std::vector<std::vector<uint8_t>> pl;
            std::vector<char> good;
            bicm.decodeBatch(in, in_noise, pl, good);
            for (size_t i = from; i < to; i++) { ok[i] = good[i - from]; bits[i].swap(pl[i - from]); }
        }
    };
    std::atomic<size_t> next{0};
    const size_t nt = std::min<size_t>(nBatches, (size_t)std::max(1, std::min(8, (int)std::thread::hardware_concurrency())));
    std::vector<std::thread> th;
    for (size_t t = 1; t < nt; t++) th.emplace_back(dect2::namedThread("onair-a3w", work, std::ref(next)));
    work(next);
    for (auto& t : th) t.join();
    for (size_t k = 0; k < n; k++) {
        out.ok.push_back(ok[k] != 0);
        out.blocks++;
        if (ok[k]) { bbScramble(bits[k]); out.packets.push_back(bitsToBytes(bits[k])); out.blocksOk++; }
    }
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
        if (!st.tail.empty() && st.tail.size() + in.head.size() == (size_t)cb) {
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
    return commitFrame(decodeFrame(x, n, bs), bs);
}

bool Atsc3Receiver::commitFrame(const FrameResult& fr, const Bootstrap& bs) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        st_.frames++;
        if (!fr.ok) { st_.framesFailed++; cti_.clear(); straddle_.clear(); return false; }
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
            pi.blocks = pr.blocks; pi.blocksOk = pr.blocksOk;
            fi.plps.push_back(pi);
        }
        info_ = fi;
    }
    std::vector<PlpResult> ctiOut;
    for (auto& p : fr.plps) if (p.cti) ctiOut.push_back(runCti(p)); else if (p.straddle) ctiOut.push_back(joinStraddle(p));
    size_t ci = 0;
    std::vector<const PlpResult*> todo;
    for (auto& p : fr.plps) todo.push_back(p.cti || p.straddle ? &ctiOut[ci++] : &p);
    for (const PlpResult* pp : todo) {
        const PlpResult& p = *pp;
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
            if (!parseBbHeader(pk.data(), (int)pk.size(), h)) { st_.bbBad++; r.reset(); continue; }
            r.push(pk.data() + h.headerBytes, (int)pk.size() - h.headerBytes, h.pointer, out);
        }
    }
    for (auto& a : out) onAlp(a);
}

void Atsc3Receiver::onAlp(const AlpPacket& a) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        st_.alpPackets++;
        if (a.type == AlpIpv4) st_.alpIp++;
        else if (a.type == AlpSignaling) st_.alpSignaling++;
        else if (a.type == AlpTs) st_.alpTs++;
        else if (a.type == AlpCompressedIp) st_.alpCompressedIp++;
        else st_.alpOther++;
    }
    if (a.type == AlpIpv4) pushIp(a.data.data(), a.data.size());
    else if (a.type == AlpTs) {
        std::lock_guard<std::mutex> lk(tsMu_);
        sawTs_ = true;
        if (tsDirect_.size() < (size_t)64 << 20) tsDirect_.insert(tsDirect_.end(), a.data.begin(), a.data.end());
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

void Atsc3Receiver::pushIp(const uint8_t* p, size_t n) {
    UdpDatagram d;
    bool got;
    {
        std::lock_guard<std::mutex> lk(mu_);
        got = ip_.push(p, n, d);
        if (got) st_.udp++;
    }
    if (got) onUdp(d);
}

void Atsc3Receiver::onUdp(const UdpDatagram& d) {
    if (d.dstIp == kLlsAddress && d.dstPort == kLlsPort) {
        auto t = parseLls(d.payload.data(), d.payload.size());
        if (!t.ok) return;
        bool start = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            st_.llsTables++;
            if (t.tableId == 4) { alerts_ = parseAeat(t.xml); return; }
            if (t.tableId != 1) return;
            lls_[t.groupId] = t;
            services_.clear();
            for (auto& g : lls_) services_.insert(services_.end(), g.second.slt.services.begin(), g.second.slt.services.end());
            start = selected_ >= 0 && !routeStarted_;
            if (selected_ < 0 && autoSelect_) {
                for (auto& sv : services_)
                    if (sv.category == 1 && !sv.hidden && sv.slsProtocol == 1) { selected_ = sv.serviceId; start = true; break; }
            }
        }
        if (start) startRoute();
        return;
    }
    RouteService* r = nullptr;
    {
        std::lock_guard<std::mutex> lk(mu_);
        r = route_.get();
    }
    if (r) r->push(d);
}

bool Atsc3Receiver::selectService(int id) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        selected_ = id;
        routeStarted_ = false;
        route_.reset();
        remuxIds_.clear();
        lastInit_.clear();
    }
    startRoute();
    std::lock_guard<std::mutex> lk(mu_);
    return routeStarted_;
}

bool Atsc3Receiver::selectServiceIfNone(int id) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (selected_ >= 0) return routeStarted_;   // the receiver chose one itself from the signaling while the caller looked: do not restart its route
        selected_ = id;
        routeStarted_ = false;
        route_.reset();
        remuxIds_.clear();
        lastInit_.clear();
    }
    startRoute();
    std::lock_guard<std::mutex> lk(mu_);
    return routeStarted_;
}

void Atsc3Receiver::startRoute() {
    std::lock_guard<std::mutex> lk(mu_);
    if (routeStarted_) return;
    for (auto& s : services_) {
        if (s.serviceId != selected_) continue;
        if (s.slsProtocol != 1) return;   // ROUTE only; MMTP services are not supported
        route_.reset(new RouteService(s.slsSrcIp, s.slsDstIp, s.slsDstPort));
        route_->onObject = [this](const RouteComponent& c, const RouteObject& o) { onObject(c, o); };
        routeStarted_ = true;
        return;
    }
}

void Atsc3Receiver::onObject(const RouteComponent& c, const RouteObject& o) {
    int id;
    {
        std::lock_guard<std::mutex> lk(mu_);
        st_.routeObjects++;
        char key[64];
        snprintf(key, sizeof key, "%08x:%d:%u", c.dstIp, c.dstPort, c.tsi);
        auto it = remuxIds_.find(key);
        if (it == remuxIds_.end()) {
            id = (int)remuxIds_.size() + 1;
            remuxIds_[key] = id;
        } else id = it->second;
        // an initialization segment: by its code point, or (not every sender uses the code points of the S-TSID) by its first box
        const bool boxInit = o.data.size() >= 8 && (memcmp(o.data.data() + 4, "ftyp", 4) == 0 || memcmp(o.data.data() + 4, "moov", 4) == 0);
        const bool init = (o.codePoint >= 5 && o.codePoint <= 7) || boxInit;
        if (init) {
            if (o.codePoint == 7 && lastInit_.count(id)) return;   // a repeat of the init segment
            if (lastInit_.count(id) && lastInit_[id] == o.data) return;
            lastInit_[id] = o.data;
        } else if (!lastInit_.count(id)) {
            return;   // media before the init segment cannot be played
        }
        st_.objectsDelivered++;
    }
    remux_.addComponent(id);
    remux_.push(id, o.data);
}

std::vector<SltService> Atsc3Receiver::services() const { std::lock_guard<std::mutex> lk(mu_); return services_; }
bool Atsc3Receiver::serviceReady() const { std::lock_guard<std::mutex> lk(mu_); return route_ && route_->ready(); }
std::string Atsc3Receiver::mpd() const { std::lock_guard<std::mutex> lk(mu_); return route_ ? route_->mpd() : std::string(); }
std::vector<RouteComponent> Atsc3Receiver::components() const { std::lock_guard<std::mutex> lk(mu_); return route_ ? route_->components() : std::vector<RouteComponent>(); }
FrameInfo Atsc3Receiver::frameInfo() const { std::lock_guard<std::mutex> lk(mu_); return info_; }
ReceiverStats Atsc3Receiver::stats() const { std::lock_guard<std::mutex> lk(mu_); return st_; }

} // namespace atsc3
} // namespace dect2
