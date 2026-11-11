// Mesh coverage map from this receiver's own receptions (3.9, docs/SMART5-iot-3.9.md). Every packet heard directly (not relayed) that
// carries the sender's position (Meshtastic position or node info with hop_start == hop_limit, MeshCore adverts with no path, LoRa APRS
// without a digipeater) adds its SNR to a grid cell of about cellKm x cellKm: count, best and median SNR. Only cells are kept: no node
// IDs, no tracks, no times beyond the cell's last update. The map shows where a node can reach this station from, and the link margin
// there (SNR against the demodulator's floor for the spreading factor).
// Pure logic, no clock.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <vector>

namespace dect2 {

// The SNR (dB) a LoRa demodulator still decodes at, by spreading factor (Semtech SX127x data sheets: -7.5 dB at SF7 .. -20 dB at SF12)
inline double loraSnrFloorDb(int sf) { return sf <= 6 ? -5 : sf >= 12 ? -20 : -7.5 - 2.5 * (sf - 7); }

struct CoverageCell {
    int row = 0, col = 0;        // grid indices
    double lat = 0, lon = 0;     // centre
    int count = 0;
    double bestSnr = -99, medianSnr = -99;
    double marginDb = 0;         // median SNR above the floor of the spreading factor heard most in the cell
    double km = -1;              // from the receiver, -1 when its position is not set
    int64_t lastUtc = 0;
};

class MeshCoverage {
public:
    explicit MeshCoverage(double cellKm = 1.0) : cellKm_(cellKm) {}
    void setReceiver(double lat, double lon) { rxLat_ = lat; rxLon_ = lon; rxSet_ = true; }
    // a direct reception; returns false when it was not taken (relayed, no position, a position that cannot be right)
    bool add(double lat, double lon, double snrDb, int sf, bool direct, int64_t utc) {
        if (!direct || !std::isfinite(lat) || !std::isfinite(lon) || std::fabs(lat) > 89.5 || std::fabs(lon) > 180 || (lat == 0 && lon == 0)) return false;
        if (rxSet_ && km(lat, lon, rxLat_, rxLon_) > 400) return false;   // beyond any direct LoRa link on the ground: a wrong position
        const double dLat = cellKm_ / 111.32;
        const int row = (int)std::floor(lat / dLat);
        const double cLat = (row + 0.5) * dLat;
        const double dLon = cellKm_ / (111.32 * std::max(0.05, std::cos(cLat * M_PI / 180)));
        const int col = (int)std::floor(lon / dLon);
        C& c = cells_[{row, col}];
        c.lat = cLat; c.lon = (col + 0.5) * dLon; c.row = row; c.col = col;
        c.snr.push_back((float)snrDb); if (c.snr.size() > 64) c.snr.erase(c.snr.begin());
        c.count++; c.best = std::max(c.best, snrDb); c.lastUtc = utc;
        c.sf[std::max(5, std::min(12, sf)) - 5]++;
        return true;
    }
    std::vector<CoverageCell> cells() const {
        std::vector<CoverageCell> v;
        for (const auto& [k, c] : cells_) {
            CoverageCell o; o.row = c.row; o.col = c.col; o.lat = c.lat; o.lon = c.lon; o.count = c.count; o.bestSnr = c.best; o.lastUtc = c.lastUtc;
            std::vector<float> s = c.snr; std::nth_element(s.begin(), s.begin() + (long)s.size() / 2, s.end()); o.medianSnr = s[s.size() / 2];
            int sf = 5; for (int i = 0; i < 8; i++) if (c.sf[i] > c.sf[sf - 5]) sf = i + 5;
            o.marginDb = o.medianSnr - loraSnrFloorDb(sf);
            o.km = rxSet_ ? km(c.lat, c.lon, rxLat_, rxLon_) : -1;
            v.push_back(o);
        }
        return v;
    }
    // the farthest cell heard at least minCount times
    bool farthest(CoverageCell& out, int minCount = 1) const {
        bool f = false;
        for (const auto& c : cells()) if (c.count >= minCount && c.km >= 0 && (!f || c.km > out.km)) { out = c; f = true; }
        return f;
    }
    size_t size() const { return cells_.size(); }
    void clear() { cells_.clear(); }
    static double km(double la1, double lo1, double la2, double lo2) {
        const double r = M_PI / 180, a = std::sin((la2 - la1) * r / 2), b = std::sin((lo2 - lo1) * r / 2);
        return 2 * 6371.0 * std::asin(std::min(1.0, std::sqrt(a * a + std::cos(la1 * r) * std::cos(la2 * r) * b * b)));
    }

private:
    struct C { int row = 0, col = 0; double lat = 0, lon = 0; std::vector<float> snr; int count = 0; double best = -99; int64_t lastUtc = 0; int sf[8] = {}; };
    std::map<std::pair<int, int>, C> cells_;
    double cellKm_;
    double rxLat_ = 0, rxLon_ = 0; bool rxSet_ = false;
};

} // namespace dect2
