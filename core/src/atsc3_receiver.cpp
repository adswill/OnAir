#include "dect2/atsc3_receiver.h"
#include "dect2/atsc3_bb.h"
#include <algorithm>

namespace dect2 {
namespace atsc3 {

Atsc3Receiver::Atsc3Receiver() {}

Atsc3Receiver::~Atsc3Receiver() { remux_.stop(); }

bool Atsc3Receiver::pushFrame(const cf32* x, size_t n, const Bootstrap& bs) {
    return commitFrame(decodeFrame(x, n, bs), bs);
}

bool Atsc3Receiver::commitFrame(const FrameResult& fr, const Bootstrap& bs) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        st_.frames++;
        if (!fr.ok) { st_.framesFailed++; return false; }
        FrameInfo fi;
        fi.valid = true;
        fi.fftSize = fr.subframe.fftSize; fi.guard = fr.subframe.guard;
        fi.spDx = spDx(fr.subframe.spPattern); fi.spDy = spDy(fr.subframe.spPattern);
        fi.symbols = fr.subframe.numSymbols; fi.preambleSymbols = fr.preamble.numSymbols;
        fi.bootstrapMinor = bs.minorVersion; fi.bandwidthMhz = bs.systemBandwidth == 0 ? 6 : bs.systemBandwidth == 1 ? 7 : 8; fi.preambleStructure = bs.preambleStructure;
        const auto& dp = fr.preamble.detail.subframes.empty() ? std::vector<L1DetailPlp>() : fr.preamble.detail.subframes[0].plps;
        for (size_t i = 0; i < fr.plps.size() && i < dp.size(); i++) {
            FramePlp q; q.fecType = dp[i].fecType; q.mod = dp[i].mod; q.cod = dp[i].cod;
            BicmConfig bc = plpBicm(q);
            FrameInfo::Plp pi;
            pi.id = fr.plps[i].id; pi.bitsPerCell = bc.bitsPerCell; pi.rate15 = bc.rate15; pi.nInner = bc.nInner;
            pi.blocks = fr.plps[i].blocks; pi.blocksOk = fr.plps[i].blocksOk;
            fi.plps.push_back(pi);
        }
        info_ = fi;
    }
    for (auto& p : fr.plps) {
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
        const bool init = o.codePoint >= 5 && o.codePoint <= 7;
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
