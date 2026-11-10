// LoRa / Meshtastic network census (3.9, docs/SMART5-field-3.9.md item 6): from the frames this receiver decoded, the nodes of the mesh
// (first and last heard, role, the fewest hops any copy of their packets had used, packets, airtime), and the network as a whole: active
// nodes, roles, the hop histogram, airtime per hour as a share of the channel (channel utilisation as Meshtastic measures it) and the
// busiest hours of the day. Every copy of a packet costs airtime; a packet is counted once (by sender and packet id).
// Airtime: Semtech AN1200.13 (SX1272/76/77/78 datasheet section 4.1.1.7): Tsym = 2^SF / BW, preamble (n + 4.25) Tsym, payload
// 8 + max(ceil((8 PL - 4 SF + 28 + 16 CRC - 20 IH) / (4 (SF - 2 DE))) (CR + 4), 0) symbols.
// Insights: "mesh_busy" when the last hour used more than busyPct of the channel (again only after it fell below 80 % of that); "mesh_router" when a node with a router role first
// appears. Node ids and roles are what the nodes broadcast to everyone; no position is kept here. Pure logic, no clock.
#pragma once
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace dect2 {

// LoRa time on air, seconds
inline double loraAirtime(int sf, double bwHz, int cr /*5..8 for 4/5..4/8*/, int payloadBytes, int preamble = 16, bool crc = true, bool implicitHeader = false, int ldro = -1) {
    if (sf < 5 || sf > 12 || bwHz <= 0) return 0;
    const double tsym = std::pow(2.0, sf) / bwHz;
    const int de = ldro >= 0 ? ldro : (tsym > 0.016 ? 1 : 0);   // low data rate optimisation: on when a symbol is longer than 16 ms
    const int c = std::clamp(cr, 5, 8) - 4;
    const double num = 8.0 * payloadBytes - 4.0 * sf + 28 + 16 * (crc ? 1 : 0) - 20 * (implicitHeader ? 1 : 0);
    const double nPay = 8 + std::max(std::ceil(num / (4.0 * (sf - 2 * de))) * (c + 4), 0.0);
    return (preamble + 4.25) * tsym + nPay * tsym;
}

struct MeshCensusFrame {
    double t = 0;                        // Unix seconds
    std::string from, type, role;        // role: when this frame carries the node's NodeInfo ("ROUTER", "CLIENT" ...), else ""
    uint32_t packetId = 0;
    int hopStart = -1, hopLimit = -1;
    int sf = 11; double bwHz = 250000; int cr = 5; int payloadBytes = 0; int preamble = 16;
    bool crcOk = true;
};

struct MeshCensusNode {
    std::string id, role;
    double first = 0, last = 0;
    uint32_t packets = 0, copies = 0;
    int minHops = -1;
    double airtimeSec = 0;
};

class MeshCensus {
public:
    double busyPct = 25;                 // Meshtastic slows its own traffic from about 25 % channel use

    // one received copy; insights ("mesh_busy", "mesh_router") as kind/text pairs in `out`
    void add(const MeshCensusFrame& f, std::vector<std::pair<std::string, std::string>>* out = nullptr) {
        const double air = loraAirtime(f.sf, f.bwHz, f.cr, f.payloadBytes, f.preamble);
        const int64_t hourSlot = (int64_t)std::floor(f.t / 3600);
        air_[hourSlot] += air;
        while (!air_.empty() && air_.begin()->first < hourSlot - 48) air_.erase(air_.begin());
        totalAir_ += air;
        if (!f.crcOk || f.from.empty()) return;
        MeshCensusNode& n = nodes_[f.from];
        if (n.id.empty()) { n.id = f.from; n.first = f.t; }
        n.last = std::max(n.last, f.t);
        n.copies++;
        n.airtimeSec += air;
        if (!f.role.empty() && n.role != f.role) {
            n.role = f.role;
            if (out && isRouter(f.role) && routersSeen_.insert(f.from).second) out->push_back({"mesh_router", "New mesh " + lower(f.role) + " heard: " + f.from});
        }
        const int hops = f.hopStart >= 0 && f.hopLimit >= 0 && f.hopStart >= f.hopLimit ? f.hopStart - f.hopLimit : -1;
        if (hops >= 0 && (n.minHops < 0 || hops < n.minHops)) n.minHops = hops;
        const bool fresh = !f.packetId || seen_.insert({f.from, f.packetId}).second;
        if (seenOrder_.size() > 20000) { seen_.clear(); seenOrder_.clear(); }
        if (f.packetId) seenOrder_.push_back(f.packetId);
        if (!fresh) return;
        n.packets++;
        packets_++;
        hourOfDay_[(int)(((hourSlot % 24) + 24) % 24)]++;
        if (hops >= 0 && hops < 8) hopHist_[hops]++;
        if (!f.type.empty()) types_[f.type]++;
        if (out && f.t - lastBusyCheck_ >= 300) {
            lastBusyCheck_ = f.t;
            const double u = utilisation(f.t);
            if (u < busyPct * 0.8) busy_ = false;   // quiet again (a little below the line, so a channel at the line is not reported again and again)
            if (u >= busyPct && !busy_) {
                busy_ = true;
                char b[160]; snprintf(b, sizeof b, "Mesh channel busy: %.0f %% airtime in the last hour (%zu nodes)", u, activeNodes(f.t, 3600));
                out->push_back({"mesh_busy", b});
            }
        }
    }

    // channel use over the last hour before `now`, percent (from the airtime of every copy heard)
    double utilisation(double now) const {
        double s = 0;
        const int64_t cur = (int64_t)std::floor(now / 3600);
        const double frac = (now - cur * 3600.0) / 3600.0;   // the current hour's part, plus the previous hour weighted by what is left
        auto a = air_.find(cur); if (a != air_.end()) s += a->second;
        auto b = air_.find(cur - 1); if (b != air_.end()) s += b->second * (1 - frac);
        return s / 3600.0 * 100.0;
    }
    size_t activeNodes(double now, double withinSec) const { size_t n = 0; for (const auto& [k, v] : nodes_) if (now - v.last <= withinSec) n++; return n; }
    std::map<std::string, int> roles() const { std::map<std::string, int> m; for (const auto& [k, v] : nodes_) m[v.role.empty() ? "unknown" : v.role]++; return m; }
    const std::array<uint64_t, 8>& hops() const { return hopHist_; }
    // the busiest hours of the day (UTC), most packets first
    std::vector<std::pair<int, uint64_t>> busiestHours(int n = 3) const {
        std::vector<std::pair<int, uint64_t>> v;
        for (int h = 0; h < 24; h++) if (hourOfDay_[h]) v.push_back({h, hourOfDay_[h]});
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        if ((int)v.size() > n) v.resize(n);
        return v;
    }
    const std::array<uint64_t, 24>& hourOfDay() const { return hourOfDay_; }
    std::vector<MeshCensusNode> nodes() const {
        std::vector<MeshCensusNode> v; for (const auto& [k, n] : nodes_) v.push_back(n);
        std::sort(v.begin(), v.end(), [](const MeshCensusNode& a, const MeshCensusNode& b) { return a.airtimeSec > b.airtimeSec; });
        return v;
    }
    const std::map<std::string, uint64_t>& types() const { return types_; }
    uint64_t packets() const { return packets_; }
    double totalAirtime() const { return totalAir_; }

    static bool isRouter(const std::string& r) { const std::string u = upperS(r); return u.find("ROUTER") != std::string::npos || u.find("REPEATER") != std::string::npos; }

private:
    std::map<std::string, MeshCensusNode> nodes_;
    std::set<std::pair<std::string, uint32_t>> seen_;
    std::deque<uint32_t> seenOrder_;
    std::set<std::string> routersSeen_;
    std::map<int64_t, double> air_;
    std::array<uint64_t, 24> hourOfDay_{};
    std::array<uint64_t, 8> hopHist_{};
    std::map<std::string, uint64_t> types_;
    uint64_t packets_ = 0; double totalAir_ = 0, lastBusyCheck_ = 0; bool busy_ = false;
    static std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); for (auto& c : s) if (c == '_') c = ' '; return s; }
    static std::string upperS(std::string s) { for (auto& c : s) c = (char)std::toupper((unsigned char)c); return s; }
};

} // namespace dect2
